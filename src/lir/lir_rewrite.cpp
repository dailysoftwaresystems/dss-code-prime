#include "lir/lir_rewrite.hpp"

#include "core/types/call_payload.hpp"
#include "core/types/parse_diagnostic.hpp"
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

    // `usedMask[c]` bit `i` set iff phys ordinal `i` of class `c` is
    // assigned to a vreg in this function. 64 bits fits every shipped
    // per-class register file. A future target with > 64 registers in
    // a single class needs to widen this — the bounds-check below is
    // a LOUD guard: `regallocFatal` on ordinal >= 64 rather than
    // silently treating it as unused.
    std::array<std::uint64_t, 5> usedMask{};
    for (auto const& a : alloc.assignments) {
        if (a.vreg.id == 0 || a.isSpilled()) continue;
        LirReg const phys = a.physReg();
        if (!phys.valid()) continue;
        std::size_t const c = static_cast<std::size_t>(phys.regClass());
        if (c >= usedMask.size() || phys.id >= 64) {
            report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
                   DiagnosticSeverity::Error,
                   std::format("pickScratchRegs: phys ordinal {} or class {} "
                               "out of 64-bit/5-class bound — widen masks",
                               static_cast<std::uint32_t>(phys.id),
                               static_cast<int>(phys.regClass())));
            outOk = false;
            return out;
        }
        usedMask[c] |= std::uint64_t{1} << phys.id;
    }

    auto const regs = schema.registers();
    for (std::uint16_t i = 0; i < regs.size(); ++i) {
        auto const& info = regs[i];
        if (info.regClass == TargetRegClass::None) continue;
        if (!info.subOf.empty()) continue;
        if (!allocatable.contains(info.name)) continue;  // reserved-role filter
        std::size_t const c = static_cast<std::size_t>(info.regClass);
        if (c >= out.pool.size()) continue;
        if (i >= 64) continue;  // outside mask range — skip defensively
        if ((usedMask[c] >> i) & 1u) continue;
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
                    std::span<std::uint16_t const> forbidden{};
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
                auto const rr = resolveReg(srcResult, alloc, scratch, cursor);
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
