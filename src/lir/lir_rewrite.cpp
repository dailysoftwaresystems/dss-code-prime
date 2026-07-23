#include "lir/lir_rewrite.hpp"

#include "core/types/call_payload.hpp"
#include "core/types/parse_diagnostic.hpp"
// c77 (D-AS-REGALLOC-DIRECT-ARG-RELOAD): the by-value-stack-arg exhaust-class
// constants — the rewriter's call-arg classifier advances the arg cursors past a
// by-value aggregate carrier EXACTLY as lir_callconv / lir_wide_call_args do, so a
// spilled scalar arg's register-vs-overflow decision matches theirs.
#include "mir/mir_opcode.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dss {

namespace {

using dss::report;

// Per-class scratch register pool. Each class holds the ordered list
// of physical registers that:
//   * appear in the active calling convention's allocatable pool
//     (saved/arg/return sets) — excludes reserved-role regs like
//     `rsp` whose absence-from-the-pool was the rsp-as-scratch
//     CRITICAL filter,
//   * are NOT assigned by the allocator to any vreg in this function.
//
// The list is target-blind-ordered (lowest-target-declared-ordinal
// first) and used as a per-instruction cursor: the per-inst loop
// hands out scratches in order, one per spilled operand. When two or
// more spilled operands of the same class appear in one inst, the
// loop pulls successive scratches from this list — closes the
// silent-miscompile path where a single shared scratch would lose
// the earlier loads' values to later loads' overwrites.
//
// An empty inner vector signals "no scratch available for this
// class" — the rewriter then emits L_VirtualRegInPostRegalloc and
// bails.
struct ScratchPerClass {
    std::array<std::vector<LirReg>, 5> pool{};
};

[[nodiscard]] bool
collectAllocatable(TargetSchema const& schema, std::uint16_t ccIndex,
                   DiagnosticReporter& reporter,
                   std::unordered_set<std::string_view>& outAllocatable,
                   std::unordered_set<std::string_view>& outCallerSet) {
    auto const* cc = schema.callingConvention(ccIndex);
    if (cc == nullptr) {
        // Without a calling convention we cannot tell allocatable regs
        // from reserved-role regs (rsp / rflags) — emit a loud error
        // instead of falling back to "every reg is eligible scratch".
        report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
               DiagnosticSeverity::Error,
               std::format("pickScratchRegs: calling-convention index {} "
                           "is missing — cannot determine allocatable "
                           "scratch set safely",
                           static_cast<unsigned>(ccIndex)));
        return false;
    }
    auto absorb = [&](std::vector<std::string> const& names) {
        for (auto const& n : names) outAllocatable.insert(n);
    };
    absorb(cc->callerSaved);
    absorb(cc->calleeSaved);
    absorb(cc->argGprs);
    absorb(cc->argFprs);
    absorb(cc->returnGprs);
    absorb(cc->returnFprs);
    outCallerSet.reserve(cc->callerSaved.size());
    for (auto const& n : cc->callerSaved) outCallerSet.insert(n);
    return true;
}

[[nodiscard]] ScratchPerClass
pickScratchRegs(TargetSchema const& schema, LirFuncAllocation const& alloc,
                DiagnosticReporter& reporter, bool& outOk) {
    ScratchPerClass out{};
    outOk = true;

    std::unordered_set<std::string_view> allocatable;
    std::unordered_set<std::string_view> callerSet;  // unused here but mirrors regalloc shape
    if (!collectAllocatable(schema, alloc.callingConventionIndex,
                            reporter, allocatable, callerSet)) {
        outOk = false;
        return out;
    }

    // `usedOrdinals[c]` holds the GLOBAL register-table ordinal of every
    // phys reg of class `c` assigned to a vreg in this function. Keyed by
    // the global ordinal in an unbounded set — NOT a fixed-width bitmask
    // indexed by the ordinal — because the global ordinal space exceeds 64
    // on a multi-class target (arm64: 33 GPR table slots push d31 to global
    // ordinal 64, past a 64-bit mask). This mirrors `buildFreeLists`
    // (lir_regalloc.cpp), which already represents the allocatable set as
    // per-class vectors of global ordinals for the same reason. The only
    // bound is the class index (kLirRegClassCount == 5), which is a true
    // substrate invariant, not a per-target register-count limit.
    std::array<std::unordered_set<std::uint16_t>, 5> usedOrdinals{};
    for (auto const& a : alloc.assignments) {
        if (a.vreg.id == 0 || a.isSpilled()) continue;
        LirReg const phys = a.physReg();
        if (!phys.valid()) continue;
        std::size_t const c = static_cast<std::size_t>(phys.regClass());
        if (c >= usedOrdinals.size()) {
            report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
                   DiagnosticSeverity::Error,
                   std::format("pickScratchRegs: phys reg class {} out of the "
                               "{}-class bound (internal invariant)",
                               static_cast<int>(phys.regClass()),
                               usedOrdinals.size()));
            outOk = false;
            return out;
        }
        usedOrdinals[c].insert(phys.id);
    }

    auto const regs = schema.registers();
    for (std::uint16_t i = 0; i < regs.size(); ++i) {
        auto const& info = regs[i];
        if (info.regClass == TargetRegClass::None) continue;
        if (!info.subOf.empty()) continue;
        if (!allocatable.contains(info.name)) continue;  // reserved-role filter
        // D-CSUBSET-VLA (C1b): in a VLA function the frame pointer is RESERVED as the
        // fixed-frame base (regalloc held it out of the allocatable pool). It is
        // therefore unassigned AND allocatable — exactly the shape this loop harvests
        // as scratch. Skip it, or the rewriter would stage a spill reload through the
        // frame pointer and clobber the frame base (a stack miscompile). No-op for a
        // non-VLA function (reservedFramePointer == nullopt).
        if (alloc.reservedFramePointer.has_value()
            && i == *alloc.reservedFramePointer) continue;
        std::size_t const c = static_cast<std::size_t>(info.regClass);
        if (c >= out.pool.size()) continue;
        if (usedOrdinals[c].contains(i)) continue;  // already assigned to a vreg
        out.pool[c].push_back(
            makePhysicalReg(i, static_cast<LirRegClass>(info.regClass)));
    }
    return out;
}

// Resolved vreg → phys-reg mapping. `phys.valid() == false` ONLY for
// scratch-exhaustion paths where the source vreg's class has no
// remaining pool entry; physical/invalid inputs pass through unchanged.
// `spillSlot` is set iff the source vreg was spilled, signalling the
// caller must emit a `frame_load` at the appropriate site.
struct ResolvedReg {
    LirReg                      phys;
    std::optional<LirSpillSlot> spillSlot;
};

// Resolve a vreg using the scratch pool's per-class cursor. For a
// spilled vreg, allocates the NEXT scratch reg of its class (advancing
// the cursor). For non-spilled, returns the assigned phys reg.
// Returns InvalidLirReg on exhaustion (cursor exhausted scratch list).
//
// FC4 c2 (B2): `forbiddenOrdinals` (empty for every operand except an
// `isCall` instruction's ops[0] — the indirect-call CALLEE) filters
// the scratch pick. A SPILLED callee reloads into a scratch register
// BETWEEN the callconv pass's not-yet-emitted arg-passing moves and
// the call — a scratch from the cc's argGprs/argFprs (or the variadic
// count register) would be overwritten by its own call's arg setup
// (silent garbage jump; same hazard the allocator-side exclusion
// closes for register-resident callees — the allocator CANNOT close
// this one, because the scratch pool is by definition the registers
// the allocator did NOT assign). The first admissible entry is
// ROTATED to the cursor position before the cursor advances, so the
// skipped (forbidden) entries stay available to this instruction's
// later spilled operands — exhaustion semantics stay exact and loud.
[[nodiscard]] ResolvedReg
resolveReg(LirReg r, LirFuncAllocation const& alloc,
           ScratchPerClass& scratch,
           std::array<std::size_t, 5>& cursor,
           std::span<std::uint16_t const> forbiddenOrdinals = {}) {
    if (!r.valid() || r.isPhysical != 0) return {r, std::nullopt};
    auto const* a = alloc.forVReg(r.id);
    if (a == nullptr) return {r, std::nullopt};
    if (!a->isSpilled()) return {a->physReg(), std::nullopt};
    std::size_t const c = static_cast<std::size_t>(r.regClass());
    if (c >= scratch.pool.size()) return {InvalidLirReg, a->spillSlot()};
    auto& pool = scratch.pool[c];
    auto const isForbidden = [&](LirReg s) {
        for (auto const ord : forbiddenOrdinals) {
            if (static_cast<std::uint32_t>(ord) == s.id) return true;
        }
        return false;
    };
    std::size_t j = cursor[c];
    while (j < pool.size() && isForbidden(pool[j])) ++j;
    if (j >= pool.size()) {
        return {InvalidLirReg, a->spillSlot()};  // exhaustion stays loud
    }
    if (j != cursor[c]) {
        // Move the admissible entry to the cursor slot; the skipped
        // forbidden entries shift up one (relative order preserved)
        // and remain available to later operands.
        std::rotate(pool.begin() + static_cast<std::ptrdiff_t>(cursor[c]),
                    pool.begin() + static_cast<std::ptrdiff_t>(j),
                    pool.begin() + static_cast<std::ptrdiff_t>(j) + 1);
    }
    LirReg const s = pool[cursor[c]++];
    return {s, a->spillSlot()};
}

// translateNonVregOperand + emitTerminator are now in lir_pass_util
// (D-ML7-1.1 fold — shared with lir_callconv).

// c77 (D-AS-REGALLOC-DIRECT-ARG-RELOAD): classify each operand of a CALL
// instruction as a REGISTER-PASSED scalar arg (→ its LirRegClass) or not
// (→ nullopt). Only a register-passed scalar arg is eligible to be DEFERRED to
// callconv as a `SpillSlotRef` when spilled — the callee (ops[0]), the sret
// pointer (ops[1] when hasIrr), a by-value-aggregate carrier (Reg + marker), the
// marker itself, and any overflow scalar arg (already removed pre-regalloc by
// lowerWideCallArgs) all keep the ordinary scratch-reload path.
//
// This MIRRORS the arg-placement cursor of lir_callconv / lir_wide_call_args
// EXACTLY (same monotonic NSAA cursor, same slot-aligned vs independent-counter
// pool logic, same ByValueStackAgg-carrier skip + class-exhaust clamp, same
// hasIndirectResult / firstArgIdx / variadicForcesStack handling) so the
// register-vs-overflow verdict here is byte-identical to the one callconv reaches
// — a spilled arg the rewriter defers is exactly one callconv will arg-move.
// Config-driven from the cc descriptor; no arch/format branch.
//
// `out[k]` is the classification of the CALL's operand index `k`. Returned by
// value (a call has few operands); indexed by the operand loop below.
[[nodiscard]] std::vector<std::optional<LirRegClass>>
classifyCallRegArgs(std::span<LirOperand const> ops, std::uint32_t payload,
                    TargetCallingConvention const& cc) {
    std::vector<std::optional<LirRegClass>> out(ops.size(), std::nullopt);
    std::uint32_t const gprPoolSize =
        static_cast<std::uint32_t>(cc.argGprs.size());
    std::uint32_t const fprPoolSize =
        static_cast<std::uint32_t>(cc.argFprs.size());
    std::uint32_t const slotAlignedPoolSize = std::max(gprPoolSize, fprPoolSize);

    bool const hasIrr = ::dss::call_payload::hasIndirectResult(payload);
    std::size_t const firstArgIdx = hasIrr ? 2u : 1u;
    bool const variadicForcesStack =
        cc.variadicArgsAlwaysStack && ::dss::call_payload::isVariadic(payload);
    std::uint32_t const fixedOps = ::dss::call_payload::fixedOperandCount(payload);

    std::uint32_t gprIdx = 0, fprIdx = 0, slotIdx = 0;
    std::uint32_t argRegionIdx = 0;
    for (std::size_t k = firstArgIdx; k < ops.size(); ++k) {
        LirOperand const& argOp = ops[k];
        if (argOp.kind == LirOperandKind::ByValueStackAgg) continue;  // marker
        bool const isByValCarrier =
            argOp.kind == LirOperandKind::Reg && (k + 1) < ops.size()
            && ops[k + 1].kind == LirOperandKind::ByValueStackAgg;
        if (isByValCarrier) {
            // Wholly-stacked aggregate — NOT a register arg (keep scratch path).
            // Advance the shared cursors + class-exhaust clamp as callconv does.
            std::uint8_t const ex = ops[k + 1].byValueAggExhaust;
            if (ex == kByValueStackArgExhaustGpr)
                gprIdx = std::max(gprIdx, gprPoolSize);
            else if (ex == kByValueStackArgExhaustFpr)
                fprIdx = std::max(fprIdx, fprPoolSize);
            ++argRegionIdx;
            continue;
        }
        if (argOp.kind != LirOperandKind::Reg) { ++argRegionIdx; continue; }
        LirRegClass const cls = argOp.reg.regClass();
        std::uint32_t argIndex, poolSize;
        if (cc.slotAligned) {
            argIndex = slotIdx++;
            poolSize = slotAlignedPoolSize;
        } else if (cls == LirRegClass::FPR) {
            argIndex = fprIdx++;
            poolSize = fprPoolSize;
        } else {
            argIndex = gprIdx++;
            poolSize = gprPoolSize;
        }
        bool const forceStack =
            variadicForcesStack && argRegionIdx >= fixedOps;
        if (argIndex < poolSize && !forceStack) {
            out[k] = cls;   // register-passed scalar arg — eligible to defer
        }
        // else: overflow scalar arg. lowerWideCallArgs already removed these
        // pre-regalloc, so this branch is not reached for a live scalar arg; if
        // a target ever leaves one, it keeps the scratch-reload path (safe).
        ++argRegionIdx;
    }
    return out;
}

[[nodiscard]] bool
rewriteOneFunc(Lir const&               src,
               LirFuncId                fn,
               TargetSchema const&      schema,
               LirFuncAllocation const& alloc,
               LirBuilder&              b,
               DiagnosticReporter&      reporter) {
    bool scratchOk = true;
    // Non-const: resolveReg's FC4 c2 forbidden-filter may rotate a
    // pool entry to the cursor position (see its docblock).
    ScratchPerClass scratch = pickScratchRegs(schema, alloc, reporter, scratchOk);
    if (!scratchOk) return false;

    // FC4 c2 (B2): the forbidden-scratch set for a SPILLED indirect-
    // call callee reload — the cc's argGprs ∪ argFprs (+ the variadic
    // vector-count register for variadic call sites). Resolved
    // LAZILY on the first spilled-callee occurrence so functions
    // without one gain zero new failure modes; a cc register name
    // that fails to resolve is a schema misconfiguration → fail LOUD
    // (a silently-weakened filter would re-open the callee-clobber
    // hazard). Entirely cc-config-driven.
    bool forbiddenBuilt = false;
    bool forbiddenBuildFailed = false;
    std::vector<std::uint16_t> forbiddenBase;
    std::vector<std::uint16_t> forbiddenWithCountReg;
    auto const buildForbidden = [&]() -> bool {
        if (forbiddenBuilt) return !forbiddenBuildFailed;
        forbiddenBuilt = true;
        auto const* cc =
            schema.callingConvention(alloc.callingConventionIndex);
        if (cc == nullptr) {
            // pickScratchRegs already failed loud for a missing cc;
            // defensive arm for a future caller bypass.
            report(reporter, DiagnosticCode::L_CcRegLookupFailed,
                   DiagnosticSeverity::Error,
                   std::format("rewriteOneFunc: calling-convention index "
                               "{} is missing — cannot build the spilled-"
                               "callee forbidden-scratch set",
                               static_cast<unsigned>(
                                   alloc.callingConventionIndex)));
            forbiddenBuildFailed = true;
            return false;
        }
        auto const resolveInto =
            [&](std::vector<std::string> const& names) -> bool {
            for (auto const& name : names) {
                auto const ord = schema.registerByName(name);
                if (!ord.has_value()) {
                    report(reporter, DiagnosticCode::L_CcRegLookupFailed,
                           DiagnosticSeverity::Error,
                           std::format("rewriteOneFunc: cc '{}' arg "
                                       "register '{}' does not resolve in "
                                       "the target register table — "
                                       "cannot build the spilled-callee "
                                       "forbidden-scratch set",
                                       cc->name, name));
                    return false;
                }
                forbiddenBase.push_back(*ord);
            }
            return true;
        };
        if (!resolveInto(cc->argGprs) || !resolveInto(cc->argFprs)) {
            forbiddenBuildFailed = true;
            return false;
        }
        forbiddenWithCountReg = forbiddenBase;
        if (cc->variadicVectorCountReg.has_value()) {
            forbiddenWithCountReg.push_back(
                cc->variadicVectorCountReg->ordinal);
        }
        return true;
    };
    auto const frameLoadOp  = schema.opcodeByMnemonic(schema.frameLoadMnemonic());
    auto const frameStoreOp = schema.opcodeByMnemonic(schema.frameStoreMnemonic());
    if (!frameLoadOp.has_value() || !frameStoreOp.has_value()) {
        report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
               DiagnosticSeverity::Error,
               "target schema missing frame_load / frame_store opcodes");
        return false;
    }
    // D-AS-REGALLOC-ARG-REGISTER-OCCUPIED (c75 correctness fix): the
    // `arg` opcode handle. When a SPILLED param's `arg` op stages its
    // incoming value into a scratch register for the frame-store, that
    // scratch must NOT be an incoming arg register still holding a live
    // param — the `arg` ops are emitted CONTIGUOUSLY at the entry-block
    // head (mir_to_lir), so at any arg op the later params' arg
    // registers are still live; staging through one clobbers that
    // incoming param before its own `arg` op materializes it (variant 1:
    // x86_64 SysV r9 = the 6th int arg register AND a caller-saved
    // scratch candidate). The forbidden set = argGprs ∪ argFprs (the
    // SAME `forbiddenBase` the spilled-indirect-callee filter uses; the
    // result's class pool only ever matches its own-class arg regs).
    // `arg` ops have no operands, so THIS handle covers only the arg-op's
    // OWN spilled RESULT store. ★ SCOPE-CORRECTED (TF-C55, D-AS-REWRITE-SPILL-
    // SCRATCH-INCOMING-ARG-CLOBBER): c75's premise that "every later
    // instruction runs after all args are materialized, so the arg-op result
    // store is the SOLE entry-region reload that can clobber a live incoming
    // arg reg" is FALSE under the release optimizer, which reorders a spilled
    // def AHEAD of the `arg` capture. The general case — ANY reordered
    // instruction's spill-reload scratch — is covered by the `pendingArgOrdinals`
    // forbid below (unioned into the default forbid at every resolveReg). This
    // arg-op-result forbid stays correct + load-bearing for its own (spilled
    // arg-op) case.
    auto const argOp = schema.opcodeByMnemonic("arg");

    // c77 (D-AS-REGALLOC-DIRECT-ARG-RELOAD): the active cc, cached for the
    // per-call arg-register classifier (classifyCallRegArgs) below. A missing cc
    // already fails loud in pickScratchRegs; nullptr here disables the direct-arg-
    // reload path (calls keep the scratch-reload path, still correct — just the
    // old exhaustion surface), so no new failure mode for a bypass caller.
    auto const* ccForArgs =
        schema.callingConvention(alloc.callingConventionIndex);

    auto const& funcInfo = src.funcArena().at(fn);
    b.addFunction(SymbolId{funcInfo.symbol});

    std::uint32_t const blockCount = src.funcBlockCount(fn);
    std::unordered_map<std::uint32_t, LirBlockId> srcToDst;
    srcToDst.reserve(blockCount);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const srcBlock = src.funcBlockAt(fn, bi);
        srcToDst[srcBlock.v] = b.createBlock();
    }

    bool classExhausted = false;

    // D-AS-REWRITE-SPILL-SCRATCH-INCOMING-ARG-CLOBBER: the incoming arg-register
    // ordinals that still hold a live parameter. A spill-reload SCRATCH must not
    // stage through one of these before its `arg` op materializes the param out
    // of it. The optimizer (release pipeline) can reorder a spilled def AHEAD of
    // the arg materialization, and the scratch pool is ordinal-ordered so pool[0]
    // is the first arg register (x0 on AAPCS64); a def staged through it clobbers
    // the incoming param (the release-only arm64 sqlite fault: arg0 read as 0 →
    // `ldur [0x10]` SEGV; argc read as 0 → shell sees no args). POSITION-AWARE:
    // each ordinal is retired the moment its `arg` op is WALKED below (== where
    // lir_callconv captures the param, freeing the register), so the forbid
    // covers only the entry window and never shrinks the pool function-wide
    // (which could turn a post-window-correct pick into a loud pool exhaustion).
    // OCCUPIED registers only — an arg register the function has no param for
    // never holds an incoming value, so it stays a free scratch. The allocator
    // side (D-AS-REGALLOC-ARG-REGISTER-OCCUPIED) closes the vreg-HOME variant of
    // this hazard; this closes the transient-SCRATCH variant through the same
    // shared `incomingArgRegister` formula.
    std::vector<std::uint16_t> pendingArgOrdinals;
    if (argOp.has_value() && ccForArgs != nullptr) {
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const blk = src.funcBlockAt(fn, bi);
            std::uint32_t const n = src.blockInstCount(blk);
            for (std::uint32_t i = 0; i < n; ++i) {
                LirInstId const inst = src.blockInstAt(blk, i);
                if (src.instOpcode(inst) != *argOp) continue;
                LirReg const res = src.instResult(inst);
                if (!res.valid() || res.isPhysical != 0) continue;
                auto const inc = lir_pass_util::incomingArgRegister(
                    schema, *ccForArgs, res.regClass(), src.instPayload(inst));
                if (inc.kind
                    == lir_pass_util::IncomingArgRegKind::UnresolvableName) {
                    // A cc arg register absent from the target table — the same
                    // schema misconfiguration `buildForbidden` fails loud on;
                    // never build a silently-weakened scratch exclusion.
                    report(reporter, DiagnosticCode::L_CcRegLookupFailed,
                           DiagnosticSeverity::Error,
                           std::format("rewriteOneFunc: cc '{}' arg register "
                                       "(payload {}) does not resolve in the "
                                       "target register table — cannot build "
                                       "the incoming-arg spill-scratch forbid",
                                       ccForArgs->name,
                                       src.instPayload(inst)));
                    return false;
                }
                if (inc.kind == lir_pass_util::IncomingArgRegKind::Register) {
                    bool dup = false;
                    for (auto const e : pendingArgOrdinals)
                        if (e == inc.ordinal) { dup = true; break; }
                    if (!dup) pendingArgOrdinals.push_back(inc.ordinal);
                }
            }
        }
    }

    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const srcBlock = src.funcBlockAt(fn, bi);
        LirBlockId const dstBlock = srcToDst.at(srcBlock.v);
        b.beginBlock(dstBlock);

        std::uint32_t const instN = src.blockInstCount(srcBlock);
        for (std::uint32_t i = 0; i < instN; ++i) {
            LirInstId const inst = src.blockInstAt(srcBlock, i);
            std::uint16_t const op = src.instOpcode(inst);
            auto const ops = src.instOperands(inst);
            LirReg const srcResult = src.instResult(inst);
            std::uint32_t const payload = src.instPayload(inst);
            std::uint8_t const flags    = src.instFlags(inst);

            // Per-inst scratch cursor. Each spilled operand / result
            // pulls the NEXT scratch reg from its class's pool; if
            // the pool runs out, we set `classExhausted = true` and
            // emit a single L_VirtualRegInPostRegalloc at end-of-func.
            // This closes the silent-miscompile path where multiple
            // spilled operands of the same class would otherwise share
            // ONE scratch — earlier loads' values would be lost to
            // later loads' overwrites.
            std::array<std::size_t, 5> cursor{};

            // Hoisted above the operand loop (FC4 c2): the loop needs
            // `info->isCall` to apply the spilled-callee forbidden-
            // scratch filter at operand index 0.
            auto const* info = schema.opcodeInfo(op);

            // c77 (D-AS-REGALLOC-DIRECT-ARG-RELOAD): for a CALL, classify which
            // operands are register-passed scalar args (→ a class). A spilled one
            // is DEFERRED to callconv as a `SpillSlotRef` rather than scratch-
            // reloaded here, so a wide call's register args demand ZERO rewriter
            // scratch (the func-2088 exhaustion). Empty for non-calls / no cc.
            std::vector<std::optional<LirRegClass>> callRegArgClass;
            if (info != nullptr && info->isCall && ccForArgs != nullptr) {
                callRegArgClass = classifyCallRegArgs(ops, payload, *ccForArgs);
            }

            // D-AS-REGALLOC-ARG-REGISTER-OCCUPIED (c75 correctness fix,
            // implicit-clobber sibling): a reload scratch for THIS
            // instruction must NOT be one of the instruction's OWN
            // implicit registers (inputs ∪ clobbered — e.g. x86 idiv's rax
            // dividend + rdx high-half). reserve-K puts non-arg caller-
            // saved registers (SysV rax) into the rewriter scratch pool;
            // without this filter a spilled idiv DIVISOR reloads into rax
            // and clobbers the dividend the idiv still needs — a SILENT
            // miscompile (121 not 160). The allocator's
            // implicitClobbersCrossedBy keeps VREG HOMES off these ordinals
            // across the op, but the rewriter's transient reload scratch is
            // a SEPARATE pool needing the same exclusion AT the op. Driven
            // by the per-opcode schema declaration (no `if (op == idiv)`) —
            // the SAME inputs∪clobbered union collectImplicitClobberPositions
            // builds. Calls + `arg` ops carry no implicitRegisters, so this
            // is DISJOINT from the isCall-op0 / spilled-arg-result filters.
            std::vector<std::uint16_t> implicitForbidden;
            if (info != nullptr && info->implicitRegisters.has_value()) {
                auto const& ir = *info->implicitRegisters;
                implicitForbidden.reserve(ir.inputOrdinals.size()
                                          + ir.clobberedOrdinals.size());
                for (auto const o : ir.inputOrdinals)
                    implicitForbidden.push_back(o);
                for (auto const o : ir.clobberedOrdinals) {
                    bool dup = false;
                    for (auto const e : implicitForbidden)
                        if (e == o) { dup = true; break; }
                    if (!dup) implicitForbidden.push_back(o);
                }
            }
            // D-AS-REWRITE-SPILL-SCRATCH-INCOMING-ARG-CLOBBER: union the still-
            // pending incoming-arg ordinals into the DEFAULT forbid so a spilled
            // operand/result reload for THIS instruction (possibly a def the
            // optimizer reordered ahead of the arg materializations) never
            // stages through a live incoming arg register. The isCall-op0 and
            // spilled-`arg`-result cases below instead use `forbiddenBase` (all
            // argGprs∪argFprs, a SUPERSET of the pending set) — already covered.
            for (auto const ord : pendingArgOrdinals) {
                bool dup = false;
                for (auto const e : implicitForbidden)
                    if (e == ord) { dup = true; break; }
                if (!dup) implicitForbidden.push_back(ord);
            }
            std::span<std::uint16_t const> const implicitForbiddenSpan{
                implicitForbidden.data(), implicitForbidden.size()};

            struct PendingLoad { LirReg scratch; LirSpillSlot slot; };
            std::vector<PendingLoad> loads;
            std::vector<LirOperand> newOps;
            newOps.reserve(ops.size());
            for (std::size_t opIdx = 0; opIdx < ops.size(); ++opIdx) {
                auto const& o = ops[opIdx];
                if (o.kind == LirOperandKind::Reg && o.reg.valid()
                    && o.reg.isPhysical == 0) {
                    // FC4 c2 (B2): an `isCall` instruction's ops[0] is
                    // the indirect-call CALLEE — when it was SPILLED,
                    // its reload scratch must avoid the cc's arg
                    // registers (+ the variadic count register), or
                    // the callconv pass's arg-setup moves would
                    // overwrite the reloaded callee before the call
                    // consumes it (see resolveReg's docblock). The
                    // L_IndirectCalleeClobberedByArgSetup backstop in
                    // lir_callconv catches any escape loudly.
                    // c77 (D-AS-REGALLOC-DIRECT-ARG-RELOAD): a SPILLED register-
                    // passed CALL ARG is DEFERRED to callconv — emit a
                    // `SpillSlotRef` (slot + class) instead of pulling a scratch +
                    // frame_load. callconv's arg-setup then loads it DIRECTLY into
                    // its ABI arg register, sequenced within the parallel-move
                    // machinery (a memory-source arg move). This is what makes a
                    // wide call's register-arg reload demand ZERO scratch (the
                    // callee ops[0] / sret ops[1] / by-value carriers below still
                    // scratch-reload — callRegArgClass[opIdx] is nullopt for them).
                    if (opIdx < callRegArgClass.size()
                        && callRegArgClass[opIdx].has_value()) {
                        auto const* a = alloc.forVReg(o.reg.id);
                        if (a != nullptr && a->isSpilled()) {
                            newOps.push_back(LirOperand::makeSpillSlotRef(
                                a->spillSlot().v,
                                static_cast<std::uint8_t>(
                                    *callRegArgClass[opIdx])));
                            continue;
                        }
                        // Not spilled: fall through to the normal path (the arg is
                        // register-resident; callconv reg-moves it as before).
                    }
                    std::span<std::uint16_t const> forbidden =
                        implicitForbiddenSpan;
                    if (opIdx == 0 && info != nullptr && info->isCall) {
                        auto const* a = alloc.forVReg(o.reg.id);
                        if (a != nullptr && a->isSpilled()) {
                            if (!buildForbidden()) return false;
                            forbidden = ::dss::call_payload::isVariadic(
                                            payload)
                                ? std::span<std::uint16_t const>{
                                      forbiddenWithCountReg}
                                : std::span<std::uint16_t const>{
                                      forbiddenBase};
                        }
                    }
                    auto const rr = resolveReg(o.reg, alloc, scratch,
                                               cursor, forbidden);
                    if (!rr.phys.valid()) {
                        classExhausted = true;
                        newOps.push_back(o);
                        continue;
                    }
                    if (rr.spillSlot) loads.push_back({rr.phys, *rr.spillSlot});
                    newOps.push_back(LirOperand::makeReg(rr.phys));
                } else {
                    newOps.push_back(lir_pass_util::remapBlockRef(o, srcToDst));
                }
            }

            for (auto const& pl : loads) {
                b.addInst(*frameLoadOp, pl.scratch,
                          std::span<LirOperand const>{}, pl.slot.v);
            }

            LirReg newResult = srcResult;
            std::optional<LirSpillSlot> pendingStore;
            if (srcResult.valid() && srcResult.isPhysical == 0) {
                // D-AS-REGALLOC-ARG-REGISTER-OCCUPIED (c75 correctness
                // fix): a SPILLED `arg` op's result stages the incoming
                // param through a scratch reg for the frame-store —
                // forbid the cc's arg registers as that scratch (a later
                // param's still-live incoming arg reg must not be
                // clobbered before its own `arg` op runs). Applies only
                // when this is an `arg` op AND its result is spilled;
                // mirrors the spilled-indirect-callee forbidden filter.
                std::span<std::uint16_t const> resultForbidden =
                    implicitForbiddenSpan;
                if (argOp.has_value() && op == *argOp) {
                    auto const* a = alloc.forVReg(srcResult.id);
                    if (a != nullptr && a->isSpilled()) {
                        if (!buildForbidden()) return false;
                        resultForbidden =
                            std::span<std::uint16_t const>{forbiddenBase};
                    }
                }
                auto const rr = resolveReg(srcResult, alloc, scratch, cursor,
                                           resultForbidden);
                if (!rr.phys.valid()) {
                    classExhausted = true;
                } else {
                    newResult = rr.phys;
                    pendingStore = rr.spillSlot;
                }
            }

            bool const isTerm = (info != nullptr && info->isTerminator());
            if (isTerm) {
                auto const succs = src.blockSuccessors(srcBlock);
                if (!lir_pass_util::emitTerminator(b, op, info, succs, newOps,
                                                   payload, flags, srcToDst,
                                                   "rewrite", reporter)) {
                    return false;
                }
            } else {
                b.addInst(op, newResult, newOps, payload, flags);
            }

            if (pendingStore.has_value()) {
                std::array<LirOperand, 1> storeOps{LirOperand::makeReg(newResult)};
                b.addInst(*frameStoreOp, InvalidLirReg, storeOps,
                          pendingStore->v);
            }

            // D-AS-REWRITE-SPILL-SCRATCH-INCOMING-ARG-CLOBBER: this `arg` op has
            // now been walked — lir_callconv captures the param out of its
            // incoming register at THIS position, so past here the register is a
            // free scratch. Retire its ordinal from the pending forbid (empties
            // the set past the entry window → no function-wide pool loss).
            if (argOp.has_value() && op == *argOp && ccForArgs != nullptr
                && srcResult.valid() && srcResult.isPhysical == 0) {
                auto const inc = lir_pass_util::incomingArgRegister(
                    schema, *ccForArgs, srcResult.regClass(), payload);
                if (inc.kind == lir_pass_util::IncomingArgRegKind::Register) {
                    for (auto it = pendingArgOrdinals.begin();
                         it != pendingArgOrdinals.end(); ++it) {
                        if (*it == inc.ordinal) {
                            pendingArgOrdinals.erase(it);
                            break;
                        }
                    }
                }
            }
        }
    }

    if (classExhausted) {
        report(reporter, DiagnosticCode::L_VirtualRegInPostRegalloc,
               DiagnosticSeverity::Error,
               std::format("rewriteOneFunc: function {} exhausted the "
                           "per-class scratch pool — register pressure "
                           "leaves no scratch register for a spilled vreg",
                           fn.v));
        return false;
    }
    return true;
}

} // namespace

LirRewriteResult
rewriteWithAllocation(Lir const&           src,
                      TargetSchema const&  schema,
                      LirAllocation const& alloc,
                      DiagnosticReporter&  reporter) {
    LirBuilder b{schema};
    // D-CSUBSET-BITFIELD-WIDE-UNIT: carry the wide-literal pool across
    // the rebuild (LiteralIndex operands reference it by index).
    lir_pass_util::copyLiteralPool(src, b);
    auto const baseline = reporter.errorCount();
    bool anyFunctionFailed = false;

    std::size_t const fnCount = src.moduleFuncCount();
    for (std::uint32_t i = 0; i < fnCount; ++i) {
        LirFuncId const fn = src.funcAt(i);
        auto const* funcAlloc = alloc.forFunc(fn);
        if (funcAlloc == nullptr || !funcAlloc->ok) {
            report(reporter, DiagnosticCode::L_VirtualRegInPostRegalloc,
                   DiagnosticSeverity::Error,
                   std::format("rewriteWithAllocation: function {} has no "
                               "valid allocation (skipped)", fn.v));
            anyFunctionFailed = true;
            continue;
        }
        if (!rewriteOneFunc(src, fn, schema, *funcAlloc, b, reporter)) {
            // Mid-failure: the builder may have a half-open function.
            // Bail without calling `finish()` (which would fatal on
            // the unterminated open block). The output module is
            // intentionally empty — callers MUST check `ok` to decide
            // whether to consume the result.
            return LirRewriteResult{Lir{}, false};
        }
    }

    LirRewriteResult out;
    out.lir = std::move(b).finish();
    out.ok  = !anyFunctionFailed && (reporter.errorCount() == baseline);
    return out;
}

} // namespace dss
