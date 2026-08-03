#include "lir/lir_regalloc.hpp"

#include "core/types/call_payload.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss {

namespace {

// Producer-side invariants (factory misuse). Genuine programmer
// errors — substrate-tier consumers route data-driven failures
// through `DiagnosticReporter` instead.
[[noreturn]] void regallocFatal(char const* what) {
    std::fputs("dss::LirRegAlloc fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// `report()` shim hoisted to `core/types/diagnostic_reporter.hpp` as
// `dss::report` at LK10 cycle 3 post-fold #2 (D-LK10-8). Call sites
// below resolve to the canonical free function via ADL.

// Per-class register lists. The naming is conservative: `calleeSaved`
// here means "treated as call-safe for allocation" — populated with
// every register in the cc's `allocatable` set that is NOT in
// `cc.callerSaved`. A target that declares an arg-only or return-only
// register without also placing it in `callerSaved` will land that
// register in this bucket; downstream cross-call ranges will use it.
// The conservatism assumes any register a producer deliberately omits
// from `callerSaved` is safe to keep live across a call.
struct RegList {
    std::vector<std::uint16_t> calleeSaved;
    std::vector<std::uint16_t> callerSaved;
};

// kLirRegClassCount derives from `LirRegClass::Flags + 1` — extending
// the enum past `Flags` auto-widens the constant. The literal lock
// (`static_assert(kLirRegClassCount == 5u)`) pins the count so adding
// a tail entry (e.g. predicates for SVE) trips here and forces an
// audit of `buildFreeLists`, the bucket layout, and downstream
// consumers.
constexpr std::size_t kLirRegClassCount =
    static_cast<std::size_t>(LirRegClass::Flags) + 1u;
using FreeListsByClass = std::array<RegList, kLirRegClassCount>;
static_assert(kLirRegClassCount == 5u,
              "FreeListsByClass size out of sync with LirRegClass enum; "
              "audit buildFreeLists when adding a new class");

[[nodiscard]] std::optional<std::uint16_t>
popReg(std::vector<std::uint16_t>& regs) {
    if (regs.empty()) return std::nullopt;
    std::uint16_t const r = regs.back();
    regs.pop_back();
    return r;
}

[[nodiscard]] FreeListsByClass
buildFreeLists(TargetSchema const&            schema,
               TargetCallingConvention const& cc,
               std::array<std::uint16_t, kLirRegClassCount> const&
                   reloadReserve,
               // D-CSUBSET-VLA (C1b): the frame-pointer ordinal to RESERVE (hold out
               // of every allocatable pool) for a function that contains a VLA — it
               // becomes the fixed-frame base. std::nullopt for a non-VLA function
               // (rbp/x29 stays an ordinary allocatable callee-saved GPR → byte-
               // identical frames, the zero-blast-radius invariant).
               std::optional<std::uint16_t> reservedFramePointer = std::nullopt) {
    FreeListsByClass out{};

    std::unordered_set<std::string_view> allocatable;
    auto absorb = [&](std::vector<std::string> const& names) {
        for (auto const& n : names) allocatable.insert(n);
    };
    absorb(cc.callerSaved);
    absorb(cc.calleeSaved);
    absorb(cc.argGprs);
    absorb(cc.argFprs);
    absorb(cc.returnGprs);
    absorb(cc.returnFprs);

    std::unordered_set<std::string_view> callerSet;
    callerSet.reserve(cc.callerSaved.size());
    for (auto const& n : cc.callerSaved) callerSet.insert(n);

    auto const regs = schema.registers();
    for (std::uint16_t i = 0; i < regs.size(); ++i) {
        auto const& info = regs[i];
        if (info.regClass == TargetRegClass::None) continue;
        if (!info.subOf.empty()) continue;
        if (!allocatable.contains(info.name)) continue;  // reserved (rsp / rflags / …)
        // D-CSUBSET-VLA (C1b): in a VLA function the frame pointer is reserved as the
        // fixed-frame base — hold it out of the allocatable pool (mirrors the rsp/
        // rflags reservation above). No-op for a non-VLA function (nullopt).
        if (reservedFramePointer.has_value() && i == *reservedFramePointer) continue;
        std::size_t const classIdx = static_cast<std::size_t>(info.regClass);
        if (classIdx >= out.size()) {
            regallocFatal("buildFreeLists: TargetRegClass out of range — "
                          "audit kLirRegClassCount");
        }
        if (callerSet.contains(info.name)) {
            out[classIdx].callerSaved.push_back(i);
        } else {
            out[classIdx].calleeSaved.push_back(i);
        }
    }

    // c75 (D-AS-REGALLOC-SPILL-RELOAD-SCRATCH): reserve, per register
    // class, `reloadReserve[c]` CALLER-SAVED registers as guaranteed
    // spill-reload scratch. Held back from the free lists → never
    // assigned to a vreg → the rewriter's pickScratchRegs
    // (lir_rewrite.cpp) picks them up as scratch automatically
    // (unassigned + still allocatable). Caller-saved so a transient
    // reload needs no callee-save. K = reloadReserve[c] is DERIVED
    // per-function from the max single-instruction register-reload
    // demand (computeReloadReserve) — never a hardcoded count; each
    // target computes its own from its own opcode operand shapes.
    //
    // D-AS-REGALLOC-ARG-REGISTER-OCCUPIED (c75 correctness fix): the
    // reserved scratch MUST NOT be an incoming-argument register (nor
    // the indirect-result register). An arg register holds an INCOMING
    // PARAMETER at function entry until that param's `arg` op
    // materializes the value out of it; a reload staged through it at
    // entry (by the rewriter's pickScratchRegs, which harvests the
    // held-back registers) clobbers the incoming param before it is
    // read (SILENT miscompile — e.g. x86_64 SysV's last caller-saved
    // GPR is r9 = the 6th integer arg register). Reserve K caller-saved
    // NON-ARG (and non-sret) registers, scanning from the caller-saved
    // END (the allocator's last-choice partition — tryAllocate prefers
    // callee-saved) and SKIPPING any arg/sret ordinal. cc-config-driven
    // (argGprs/argFprs/indirectResultRegister); no register names, no
    // arch identity. x86_64 SysV non-arg caller-saved GPRs = {rax, r10,
    // r11} = 3 ≥ K (K ≤ the max non-call same-class virtual reg
    // operand+result count over the shipped opcodes). If a class has
    // fewer than K non-arg caller-saved registers (ms_x64 FPR has only
    // xmm4/xmm5 = 2), reserve what EXISTS — under-reserving only weakens
    // the scratch GUARANTEE (a too-tight function then fails LOUD at the
    // rewriter backstop, never silently), whereas reserving an arg
    // register would silently re-open the clobber (see the loop below).
    std::unordered_set<std::uint16_t> argOrdinals;
    auto absorbArgOrds = [&](std::vector<std::string> const& names) {
        for (auto const& n : names)
            if (auto ord = schema.registerByName(n); ord.has_value())
                argOrdinals.insert(*ord);
    };
    absorbArgOrds(cc.argGprs);
    absorbArgOrds(cc.argFprs);
    if (cc.indirectResultRegister.has_value())
        argOrdinals.insert(cc.indirectResultRegister->ordinal);

    for (std::size_t c = 0; c < out.size(); ++c) {
        std::size_t const k = static_cast<std::size_t>(reloadReserve[c]);
        if (k == 0) continue;
        auto& caller = out[c].callerSaved;
        // Walk from the END, moving up to K reserved NON-ARG registers
        // out of the free list. Arg/sret ordinals are left in place
        // (still allocatable) and skipped over — NEVER reserved (they
        // hold incoming params at entry; reserving one re-opens the
        // silent clobber). If a class has FEWER than K non-arg caller-
        // saved registers (e.g. ms_x64 FPR: xmm4/xmm5 are the only non-
        // arg caller-saved of xmm0..xmm5), reserve what EXISTS and stop.
        // Under-reserving is SAFE: the reservation only GUARANTEES scratch
        // availability; a function whose per-instruction reload demand
        // exceeds the reserved-plus-otherwise-free scratch still fails
        // LOUD at the rewriter's L_VirtualRegInPostRegalloc backstop
        // (never a silent miscompile). Callee-saved registers are NOT
        // drawn for the reservation — pickScratchRegs uses reserved regs
        // raw (no prologue/epilogue save), so a callee-saved scratch
        // would clobber the caller's value; caller-saved-only keeps the
        // transient-reload contract. (Widening the non-call reload
        // demand past the non-arg caller-saved supply is the deferred
        // wide-operand concern D-AS-REGALLOC-WIDE-CALL-OPERAND-COUNT's
        // sibling; not reachable on the shipped targets, where K ≤ 3 and
        // every class has ≥2 non-arg caller-saved registers.)
        std::size_t reserved = 0;
        std::size_t scan = caller.size();
        while (reserved < k && scan > 0) {
            --scan;
            if (argOrdinals.contains(caller[scan])) continue;  // never reserve an arg reg
            caller.erase(caller.begin() + static_cast<std::ptrdiff_t>(scan));
            ++reserved;
        }
    }

    return out;
}

// c75 (D-AS-REGALLOC-SPILL-RELOAD-SCRATCH): the max single-instruction
// register-reload demand of `fn`, per register class — the count of
// same-class VIRTUAL register operands (+ a virtual register result)
// the rewriter must simultaneously materialize for ONE instruction (its
// per-inst scratch-cursor peak, lir_rewrite.cpp resolveReg).
// buildFreeLists reserves this many caller-saved registers per class as
// guaranteed reload scratch. CALLS are excluded — a call's arg operands
// can exceed the register file (the deferred wide-call anchor
// D-AS-REGALLOC-WIDE-CALL-OPERAND-COUNT); reserving that many is neither
// possible nor the general-pressure fix this cycle targets. Terminators
// / `arg` / ordinary ops ARE counted (the rewriter reloads their
// spilled operands too). Physical operands are skipped (they never
// spill), as are immediate / block-ref operands (o.kind != Reg).
// Derived, per-target, per-function — never a hardcoded count.
[[nodiscard]] std::array<std::uint16_t, kLirRegClassCount>
computeReloadReserve(Lir const& lir, TargetSchema const& schema,
                     LirFuncLiveness const& flow) {
    std::array<std::uint16_t, kLirRegClassCount> reserve{};
    for (auto const& blk : flow.blockOrder) {
        std::uint32_t const n = lir.blockInstCount(blk);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = lir.blockInstAt(blk, i);
            auto const* info = schema.opcodeInfo(lir.instOpcode(inst));
            // c77 (D-AS-REGALLOC-DIRECT-ARG-RELOAD): CALLS are EXCLUDED again
            // (reverting c76's option-E removal). With direct-arg-reload, a
            // spilled register-passed call arg becomes a `SpillSlotRef` that the
            // rewriter does NOT scratch-reload — callconv loads it DIRECTLY into
            // its ABI arg register (demand == supply by construction). So a call's
            // register args need ZERO rewriter reload scratch, and counting them
            // here would only shrink the allocatable GPR pool for the rest of the
            // function with no correctness benefit. The wide-call blocker
            // (func-2088) is closed by the direct reload, not by reserving K. A
            // spilled INDIRECT-CALLEE (ops[0]) still reloads into a scratch, but
            // that is a SINGLE same-class operand (demand ≤ 1) — well within the
            // non-arg caller-saved supply, and general-body ops (counted below)
            // already dominate it. The `store_outgoing_arg` carriers the wide-call
            // pass emits are NON-call single-operand insts, still counted below.
            if (info != nullptr && info->isCall) continue;
            std::array<std::uint16_t, kLirRegClassCount> demand{};
            auto const bump = [&](LirReg r) {
                if (!r.valid() || r.isPhysical != 0) return;
                std::size_t const c = static_cast<std::size_t>(r.regClass());
                if (c < demand.size()) ++demand[c];
            };
            for (auto const& o : lir.instOperands(inst)) {
                if (o.kind == LirOperandKind::Reg) bump(o.reg);
            }
            bump(lir.instResult(inst));
            for (std::size_t c = 0; c < reserve.size(); ++c) {
                if (demand[c] > reserve[c]) reserve[c] = demand[c];
            }
        }
    }
    return reserve;
}

// Returns the EARLY slot (`pos`) of each call instruction, scaled to
// liveness's 2-slot-per-inst convention (see lir_liveness.cpp). The
// `pos += 2u` arithmetic is coupled to `rangeCrossesCall`'s `p + 1`
// (= late slot) test AND to liveness's slot scale — these three must
// move together.
[[nodiscard]] std::vector<std::uint32_t>
collectCallPositions(Lir const& lir, TargetSchema const& schema,
                     LirFuncLiveness const& flow) {
    std::vector<std::uint32_t> out;
    std::uint32_t pos = 0;
    for (auto const& b : flow.blockOrder) {
        std::uint32_t const n = lir.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = lir.blockInstAt(b, i);
            auto const* info = schema.opcodeInfo(lir.instOpcode(inst));
            if (info != nullptr && info->isCall) {
                out.push_back(pos);
            }
            pos += 2u;
        }
    }
    return out;
}

// FC4 c2 — the indirect-callee/arg-setup collision rule (R2). For
// every `isCall` instruction whose ops[0] is a VIRTUAL Reg (the
// indirect-call callee, post-isel pre-regalloc), record its position
// (EARLY slot, same 2-slot scale as collectCallPositions), the callee
// vreg id, and the call payload's variadic + indirect-result bits.
//
// WHY: the callconv materializer inserts the arg-passing moves (and
// the variadic count-reg set, and the FC7-C3 indirect-result `mov x8, R`
// reroute) POST-regalloc, BETWEEN the callee's definition and its use at
// the call. A callee consumed AT the call does not "cross" it
// (`rangeCrossesCall` requires `pos + 1 < r.end`), so every caller-saved
// register — INCLUDING all arg registers AND the cc's indirect-result
// register (x8 on AAPCS64, caller-saved, NOT an arg reg) — is otherwise
// eligible for the callee vreg; fixed-def interference from the
// not-yet-emitted moves is not modeled. A callee parked in such a
// register is then clobbered by its own call's arg/result setup → the
// call jumps THROUGH a setup value (D-FC4-C2 silent garbage), or trips
// the loud L_IndirectCalleeClobberedByArgSetup backstop. The consumer in
// allocateOneFunc excludes the cc's argGprs ∪ argFprs (+ the variadic
// vector-count register when the payload's variadic bit is set + the
// indirect-result register when the payload's indirect-result bit is set
// — D-FC7-INDIRECT-X8-SRET-CALLEE-EXCLUSION) from any range of the callee
// vreg covering the call. Entirely cc-config-driven — no register names,
// no arch identity.
struct IndirectCalleeAt {
    std::uint32_t position;      // call's EARLY slot
    std::uint32_t calleeVregId;
    bool          variadic;      // call payload's isVariadic bit
    bool          indirectResult; // call payload's hasIndirectResult bit (x8 sret)
};

[[nodiscard]] std::vector<IndirectCalleeAt>
collectIndirectCalleePositions(Lir const& lir, TargetSchema const& schema,
                               LirFuncLiveness const& flow) {
    std::vector<IndirectCalleeAt> out;
    std::uint32_t pos = 0;
    for (auto const& b : flow.blockOrder) {
        std::uint32_t const n = lir.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = lir.blockInstAt(b, i);
            auto const* info = schema.opcodeInfo(lir.instOpcode(inst));
            if (info != nullptr && info->isCall) {
                auto const ops = lir.instOperands(inst);
                if (!ops.empty()
                    && ops[0].kind == LirOperandKind::Reg
                    && ops[0].reg.valid()
                    && ops[0].reg.isPhysical == 0) {
                    out.push_back({pos,
                                   static_cast<std::uint32_t>(ops[0].reg.id),
                                   ::dss::call_payload::isVariadic(
                                       lir.instPayload(inst)),
                                   ::dss::call_payload::hasIndirectResult(
                                       lir.instPayload(inst))});
                }
            }
            pos += 2u;
        }
    }
    return out;
}

// D-AS-REGALLOC-ARG-REGISTER-OCCUPIED (c75 correctness fix): for every
// incoming-parameter `arg` op, record the physical INCOMING arg register
// (cc.argGprs[payload] for a GPR-class result, cc.argFprs[payload] for
// FPR) and the RELEASE position = the arg op's LATE slot. The caller
// (post-isel) placed the k-th param in that register; the callconv pass
// materializes `mov <regalloc-home>, <argreg>` AT the arg op POST-
// regalloc, so the incoming register is LIVE over [entry=0, releasePos).
// The allocator + rewriter must not reuse it in that window (assigning
// it to another vreg, or staging a spill-reload through it, clobbers the
// incoming param before it is read — SILENT miscompile: x86_64 SysV's
// r9 is both the last caller-saved GPR and the 6th int arg register).
//
// Identified by the `arg` mnemonic — the SAME handle mir_to_lir emits
// (MnemonicSlot::Arg = "arg") and lir_callconv materializes (h.arg). A
// target without an `arg` op (no register-machine param passing) yields
// an empty list — zero new behavior. `payload` is the per-class arg
// index (D-ML7-2.10: HIR→MIR emits a monotonic per-class counter). The
// arg register NAME→ordinal resolves via the cc; a name that fails to
// resolve is left unrecorded (the callconv pass fails loud on it later —
// this collector never weakens allocation on a bad schema by inventing
// an ordinal). Entirely cc-config-driven — no register names, no arch.
struct ArgRegisterOccupiedAt {
    std::uint16_t ordinal;       // incoming physical arg-register ordinal
    LirRegClass   cls;           // its register class (GPR/FPR)
    std::uint32_t releasePos;    // arg op's LATE slot (register free at/after)
    std::uint32_t paramVregId;   // the arg op's result vreg (its own home — NOT excluded)
};

[[nodiscard]] std::vector<ArgRegisterOccupiedAt>
collectArgRegisterOccupied(Lir const& lir, TargetSchema const& schema,
                           TargetCallingConvention const& cc,
                           LirFuncLiveness const& flow) {
    std::vector<ArgRegisterOccupiedAt> out;
    auto const argOp = schema.opcodeByMnemonic("arg");
    if (!argOp.has_value()) return out;  // no register-machine arg passing
    std::uint32_t pos = 0;
    for (auto const& b : flow.blockOrder) {
        std::uint32_t const n = lir.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = lir.blockInstAt(b, i);
            if (lir.instOpcode(inst) == *argOp) {
                LirReg const res = lir.instResult(inst);
                if (res.valid() && res.isPhysical == 0) {
                    LirRegClass const cls = res.regClass();
                    // Shared with the rewriter's spill-scratch forbid
                    // (D-AS-REWRITE-SPILL-SCRATCH-INCOMING-ARG-CLOBBER) so the
                    // "which incoming register holds this param" verdict cannot
                    // drift between the two consumers of the one formula.
                    auto const inc = lir_pass_util::incomingArgRegister(
                        schema, cc, cls, lir.instPayload(inst));
                    if (inc.kind
                        == lir_pass_util::IncomingArgRegKind::Register) {
                        out.push_back({inc.ordinal, cls, /*releasePos=*/pos + 1u,
                                       static_cast<std::uint32_t>(res.id)});
                    }
                    // StackPassed: no incoming register to protect (callconv
                    // reads the param from the caller's outgoing area).
                    // UnresolvableName: a schema-misconfigured cc register —
                    // left UNRECORDED here (callconv fails loud on it later),
                    // preserving this collector's pre-hoist behavior; the
                    // rewriter's consumer of the same helper fails loud
                    // directly, as its safety-exclusion role demands.
                }
            }
            pos += 2u;
        }
    }
    return out;
}

// True iff the range is live STRICTLY PAST the call's late slot —
// i.e. survives the call's caller-saved clobber. A vreg used only as
// a call argument has `range.end == call.early + 1 == call.late`
// (the use is at `call.early`, range end is `lastUse + 1`); that
// case is NOT a crossing because the value is consumed by the call
// itself.
[[nodiscard]] bool
rangeCrossesCall(LirLiveRange const& r,
                 std::vector<std::uint32_t> const& callPositions) {
    auto lo = std::lower_bound(callPositions.begin(), callPositions.end(),
                               r.start);
    if (lo == callPositions.end()) return false;
    return *lo + 1u < r.end;
}

// Per-opcode implicit-clobber consumer (cycle 10q closure of the
// 10p substrate). Some opcodes (x86 idiv/div, future x86 shift-by-CL,
// future mul-1-op-for-128-bit-result) destroy specific physical
// registers as part of their semantic contract — distinct from
// caller-saved (which is target-wide, applies to all calls) and
// distinct from requires2Address (which forces ops[0]==result).
// The 10p substrate declared the constraint per-opcode JSON-side;
// 10q wires the regalloc to read + respect it.
//
// Mechanism mirrors callPositions: scan the LIR once, collect a
// (position, clobbered-ordinals) entry per opcode-with-clobbers.
// Per-range allocation then checks crossings + adds the union of
// crossed clobbers to the exclusion set passed to tryAllocate
// Excluding. Universal across CPUs: the constraint is per-opcode-
// JSON-declared; no `if (opcode == idiv)` ever.
// `forbiddenOrdinals` = (implicitRegisters.inputs ∪
// implicitRegisters.clobbered) at this position. Cycle-10r fix:
// the operand vregs USED AT a compound op must avoid BOTH input
// AND clobbered regs:
//   - Operand allocated to an implicit-INPUT reg: the implicit-
//     input-pinning mov (e.g., `mov rax, dividend` in lowerDiv)
//     happens BEFORE the op reads the operand — overwriting the
//     operand's value with the implicit-input's value. Silent
//     miscompile (divisor reads as dividend; 100/100 = 1).
//   - Operand allocated to an implicit-CLOBBERED reg: the op's
//     pre-emit (CQO writes RDX before IDIV reads operand 0)
//     destroys the operand's value mid-op. Silent miscompile
//     (operand becomes sign-extension of dividend; 100/0 = trap).
// Outputs are not forbidden (the op reads the operand BEFORE
// writing outputs; same-reg overlap is fine).
struct ImplicitClobberAt {
    std::uint32_t              position;
    std::vector<std::uint16_t> forbiddenOrdinals;
};

[[nodiscard]] std::vector<ImplicitClobberAt>
collectImplicitClobberPositions(Lir const& lir, TargetSchema const& schema,
                                LirFuncLiveness const& flow) {
    std::vector<ImplicitClobberAt> out;
    std::uint32_t pos = 0;
    for (auto const& b : flow.blockOrder) {
        std::uint32_t const n = lir.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = lir.blockInstAt(b, i);
            auto const* info = schema.opcodeInfo(lir.instOpcode(inst));
            if (info != nullptr
             && info->implicitRegisters.has_value()) {
                auto const& ir = *info->implicitRegisters;
                if (!ir.inputOrdinals.empty()
                 || !ir.clobberedOrdinals.empty()) {
                    std::vector<std::uint16_t> forbidden;
                    forbidden.reserve(ir.inputOrdinals.size()
                                      + ir.clobberedOrdinals.size());
                    for (auto const o : ir.inputOrdinals)     forbidden.push_back(o);
                    for (auto const o : ir.clobberedOrdinals) {
                        // dedup against inputs (idiv's RDX is both
                        // input + clobbered)
                        bool dup = false;
                        for (auto const e : forbidden) if (e == o) { dup = true; break; }
                        if (!dup) forbidden.push_back(o);
                    }
                    out.push_back({pos, std::move(forbidden)});
                }
            }
            pos += 2u;
        }
    }
    return out;
}

// Returns the union of clobbered-register ordinals across every
// implicit-clobber opcode whose position is COVERED by the range
// (range.start <= pos < range.end). This is DIFFERENT from
// rangeCrossesCall's "strictly past" semantics — discovered cycle
// 10r catastrophically:
//
// A call clobbers its caller-saved registers AFTER the call returns
// — args consumed at call.early are safe even if held in
// caller-saved. So `rangeCrossesCall` uses `pos + 1 < r.end`
// (range must extend PAST the call's late slot).
//
// A compound op (x86 sdiv_compound = CQO + IDIV; udiv_compound =
// XOR + DIV) clobbers MID-OP: CQO destroys RDX BEFORE IDIV reads
// its operand 0. A divisor vreg whose range ENDS at the compound
// op (range.end = pos + 1, the "consumed by op" case in
// `rangeCrossesCall`'s reasoning) is STILL READ AFTER THE
// CLOBBER — the call-style "safe because consumed at early slot"
// invariant DOES NOT hold for compound ops. Pre-10r-fix this
// shipped silent-miscompile: a divisor allocated to RDX would be
// destroyed by CQO before IDIV read it, producing IDIV by
// (sign-extension-of-RAX) which traps with STATUS_INTEGER_DIVIDE_
// BY_ZERO when the dividend happens to be a small positive value.
// Caught by `examples/c-subset/division/` exiting with the OS's
// trap signature instead of 47.
//
// The fix: use "covers position P" semantics — exclude clobbers
// from any range with `r.start <= pos < r.end`. Captures both
// (a) ranges crossing past, AND (b) ranges with use-at-pos.
//
// Appends into the caller's growable exclusion scratch
// (D-OPT-REGALLOC-EXCLUSION-BUFFER closure, 2026-06-11): the prior
// fixed `std::array<uint16_t, 8>` + `regallocFatal` overflow arm is
// gone — the schema loader places NO cap on `implicitRegisters`
// list sizes (bounded only by the target's register table, which is
// itself unbounded), so only a growable buffer makes the exclusion
// contract TOTAL over every loadable schema. Removing the fail-loud
// arm is sound precisely because the replacement cannot lose an
// ordinal: `push_back` grows; nothing truncates.
void
implicitClobbersCrossedBy(LirLiveRange const& r,
                          std::vector<ImplicitClobberAt> const& clobbers,
                          std::vector<std::uint16_t>& out) {
    for (auto const& c : clobbers) {
        if (c.position < r.start) continue;
        if (c.position >= r.end) continue;
        for (std::uint16_t const ord : c.forbiddenOrdinals) {
            // Dedup against what's already in `out` (the
            // requires2Address pass populated the leading slice;
            // multiple implicit-clobber positions may repeat the
            // same ordinal).
            bool already = false;
            for (std::uint16_t const e : out) {
                if (e == ord) { already = true; break; }
            }
            if (already) continue;
            out.push_back(ord);
        }
    }
}

struct ActiveEntry {
    LirLiveRange  range;
    LirRegClass   cls;
    std::uint16_t physOrdinal;
    bool          isCalleeSaved;
};

void expireActive(std::vector<ActiveEntry>& active,
                  FreeListsByClass&         free,
                  std::uint32_t             currentStart) {
    auto it = active.begin();
    while (it != active.end()) {
        if (it->range.end <= currentStart) {
            auto& bucket = free[static_cast<std::size_t>(it->cls)];
            auto& list   = it->isCalleeSaved ? bucket.calleeSaved
                                             : bucket.callerSaved;
            list.push_back(it->physOrdinal);
            it = active.erase(it);
        } else {
            ++it;
        }
    }
}

struct AllocPick {
    std::uint16_t ordinal;
    bool          isCalleeSaved;
};

[[nodiscard]] std::optional<AllocPick>
tryAllocate(FreeListsByClass& free, LirRegClass cls, bool crossesCall) {
    auto& bucket = free[static_cast<std::size_t>(cls)];
    if (auto r = popReg(bucket.calleeSaved); r.has_value()) {
        return AllocPick{*r, true};
    }
    if (crossesCall) return std::nullopt;
    if (auto r = popReg(bucket.callerSaved); r.has_value()) {
        return AllocPick{*r, false};
    }
    return std::nullopt;
}

// Pop a free register, SKIPPING any ordinal in `excluded`. Matches
// the `tryAllocate` policy (callee-saved first, then caller-saved
// unless crossesCall) but removes the picked entry only when it's
// admissible. Excluded entries stay in the bucket (will be returned
// to circulation when an unfettered call site asks for them).
//
// Closes D-CSUBSET-BINOP-RIGHT-CLOBBER (2026-06-02): when allocating
// the result of a `requires2Address` instruction, operand[1..N]'s
// physical registers must not be selected — the 2-addr legalize
// would otherwise emit `mov result, ops[0]` and CLOBBER ops[N]'s
// value before the binary op reads it. Universal across CPUs +
// commutativity (the alias is a regalloc-tier invariant, not a
// per-op special case).
[[nodiscard]] std::optional<AllocPick>
tryAllocateExcluding(FreeListsByClass& free,
                     LirRegClass cls,
                     bool crossesCall,
                     std::span<std::uint16_t const> excluded) {
    // Empty-excluded fast path falls back to the standard policy
    // (preserves existing allocation traces for tests).
    if (excluded.empty()) {
        return tryAllocate(free, cls, crossesCall);
    }
    auto isExcluded = [&](std::uint16_t ord) noexcept {
        for (auto e : excluded) if (e == ord) return true;
        return false;
    };
    auto popFiltered = [&](std::vector<std::uint16_t>& regs)
        -> std::optional<std::uint16_t> {
        // Scan back-to-front (LIFO, matching popReg's order). The
        // first non-excluded ordinal is returned and erased.
        for (auto it = regs.rbegin(); it != regs.rend(); ++it) {
            if (!isExcluded(*it)) {
                std::uint16_t const ord = *it;
                regs.erase(std::next(it).base());
                return ord;
            }
        }
        return std::nullopt;
    };
    auto& bucket = free[static_cast<std::size_t>(cls)];
    if (auto r = popFiltered(bucket.calleeSaved); r.has_value()) {
        return AllocPick{*r, true};
    }
    if (crossesCall) return std::nullopt;
    if (auto r = popFiltered(bucket.callerSaved); r.has_value()) {
        return AllocPick{*r, false};
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<ActiveEntry>::iterator
findSpillCandidate(std::vector<ActiveEntry>& active, LirRegClass cls,
                   bool requireCalleeSaved,
                   std::span<std::uint16_t const> excluded = {}) {
    // D-CSUBSET-BINOP-RIGHT-CLOBBER spill-aware closure (silent-
    // failure audit HIGH-1, 2026-06-02): when the caller is
    // resolving a `requires2Address` result whose `tryAllocate
    // Excluding` returned nullopt, the spill fallback MUST NOT
    // pick an evictee whose physical ordinal is in the excluded
    // set — otherwise the freed register lands on operand[k>=1]'s
    // ordinal and the clobber bug recurs under register pressure
    // (just-freed-reg → result-vreg → mov clobbers source). Pass
    // the same excluded span used for tryAllocateExcluding so the
    // exclusion contract holds end-to-end across the alloc + spill
    // arms.
    auto const isExcluded = [&](std::uint16_t ord) noexcept {
        for (auto e : excluded) if (e == ord) return true;
        return false;
    };
    auto best = active.end();
    std::uint32_t bestEnd = 0;
    for (auto it = active.begin(); it != active.end(); ++it) {
        if (it->cls != cls) continue;
        if (requireCalleeSaved && !it->isCalleeSaved) continue;
        if (isExcluded(it->physOrdinal)) continue;
        if (it->range.end > bestEnd) {
            bestEnd = it->range.end;
            best    = it;
        }
    }
    return best;
}

// Per-function spill bookkeeping. Aggregated and emitted as a single
// `R_SpillSummary` note at end-of-function so the reporter's per-code
// cap (50) cannot silently drop notes on highly-pressured functions
// (the per-vreg-note design would lose data past the 50th spill with
// no visible signal).
struct SpillStats {
    std::uint32_t pressure       = 0;
    std::uint32_t crossCallExhaustion = 0;
};

void emitSpillSummary(DiagnosticReporter& reporter, LirFuncId fn,
                      SpillStats const& s) {
    if (s.pressure == 0 && s.crossCallExhaustion == 0) return;
    DiagnosticCode const code =
        (s.crossCallExhaustion > 0)
            ? DiagnosticCode::R_SpilledDueToCrossCallExhaustion
            : DiagnosticCode::R_SpilledDueToPressure;
    report(reporter, code, DiagnosticSeverity::Info,
           std::format("func {} spilled {} vreg(s) ({} pressure, "
                       "{} cross-call exhaustion)",
                       fn.v,
                       s.pressure + s.crossCallExhaustion,
                       s.pressure, s.crossCallExhaustion));
}

} // namespace

// ── LirRegAssignment ────────────────────────────────────────────────

LirRegAssignment LirRegAssignment::makePhys(LirReg vreg, LirReg phys) {
    if (vreg.isPhysical != 0) {
        regallocFatal("makePhys: input vreg must be virtual");
    }
    if (phys.isPhysical != 1) {
        regallocFatal("makePhys: output must be a physical register");
    }
    if (vreg.regClass() != phys.regClass()) {
        regallocFatal("makePhys: class mismatch between vreg and physReg");
    }
    LirRegAssignment a{};
    a.vreg       = vreg;
    a.assignment = phys;
    return a;
}

LirRegAssignment LirRegAssignment::makeSpill(LirReg vreg, LirSpillSlot slot) {
    if (vreg.isPhysical != 0) {
        regallocFatal("makeSpill: input vreg must be virtual");
    }
    if (!slot.valid()) {
        regallocFatal("makeSpill: slot must be valid (v != 0)");
    }
    LirRegAssignment a{};
    a.vreg       = vreg;
    a.assignment = slot;
    return a;
}

// ── LirFuncAllocation / LirAllocation ──────────────────────────────

LirRegAssignment const*
LirFuncAllocation::forVReg(std::uint32_t vregId) const noexcept {
    // id 0 is the sentinel slot — never a valid lookup target.
    // Out-of-range ids return nullptr rather than UB.
    if (vregId == 0 || vregId >= assignments.size()) return nullptr;
    auto const& a = assignments[vregId];
    if (a.vreg.id == 0) return nullptr;  // unfilled slot
    return &a;
}

bool LirAllocation::ok() const noexcept {
    for (auto const& f : perFunc) {
        if (!f.ok) return false;
    }
    return true;
}

LirFuncAllocation const* LirAllocation::forFunc(LirFuncId fn) const noexcept {
    for (auto const& f : perFunc) {
        if (f.fn.v == fn.v && f.fn.arenaTag == fn.arenaTag) return &f;
    }
    return nullptr;
}

// ── allocate ───────────────────────────────────────────────────────

namespace {

// D-CSUBSET-VLA (C1b): does function `fn` contain any inst with opcode `op`?
// Used to detect the `sub_sp_reg` VLA marker so the frame pointer is reserved
// out of the allocatable pool. Source/target-agnostic — a plain opcode-match
// scan (the `functionHasCalls`/`functionUsesVaStart` predicate shape).
[[nodiscard]] bool
functionContainsOpcode(Lir const& lir, LirFuncId fn, std::uint16_t op) noexcept {
    std::uint32_t const blockCount = lir.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const blk = lir.funcBlockAt(fn, bi);
        std::uint32_t const n = lir.blockInstCount(blk);
        for (std::uint32_t i = 0; i < n; ++i) {
            if (lir.instOpcode(lir.blockInstAt(blk, i)) == op) return true;
        }
    }
    return false;
}

// Per-function core. Wraps the linear-scan loop with `ok` derivation
// via reporter delta + emits the per-function spill summary at the
// end. `schemaOk` is the pre-checked schema-wide validity (≥1 cc) —
// false short-circuits to an empty result with `ok = false`.
LirFuncAllocation allocateOneFunc(Lir const& lir,
                                  TargetSchema const& schema,
                                  LirFuncLiveness const& flow,
                                  std::uint16_t callingConventionIndex,
                                  DiagnosticReporter& reporter,
                                  bool schemaOk) {
    LirFuncAllocation out;
    out.fn = flow.fn;
    out.originalSymbol = SymbolId{lir.funcArena().at(flow.fn).symbol};
    auto const baseline = reporter.errorCount();
    if (!schemaOk) {
        // Schema-wide error already reported by the caller; mark this
        // func failed without re-emitting (avoids per-func duplication
        // that the reporter's dedup-window would silently swallow).
        out.ok = false;
        return out;
    }

    // D-FF3-3 post-fold #5: callingConventionIndex now comes from
    // `resolveAbi(target, format)` resolution at compileOneTarget,
    // threaded through compileSingleUnit. The previous hardcoded
    // `0` silently dispatched non-ELF targets (e.g. PE64+x86_64)
    // to the first cc (sysv_amd64) instead of the correct cc
    // (ms_x64) — a real miscompile surface, not a substrate
    // placeholder.
    out.callingConventionIndex = callingConventionIndex;
    auto const* cc = schema.callingConvention(callingConventionIndex);
    if (cc == nullptr) {
        report(reporter, DiagnosticCode::R_CallingConventionLookupFailed,
               DiagnosticSeverity::Error,
               std::format("calling convention index {} lookup returned "
                           "nullptr (target schema declares {} cc rows)",
                           static_cast<unsigned>(callingConventionIndex),
                           schema.callingConventionCount()));
        out.ok = false;
        return out;
    }

    // D-CSUBSET-VLA (C1b): a function that contains a `sub_sp_reg` op (a dynamic
    // VLA stack allocation) reserves the frame pointer as its fixed-frame base —
    // exclude it from the allocatable pool so it is never handed to a vreg. The
    // callconv pass force-saves it in the prologue + captures it as the base. A
    // target/module without VLA (no `sub_sp_reg` opcode, or none in this function)
    // takes the nullopt path — rbp/x29 stays allocatable (byte-identical frames).
    std::optional<std::uint16_t> reservedFramePointer;
    if (auto const subSpReg = schema.opcodeByMnemonic("sub_sp_reg");
        subSpReg.has_value() && cc->framePointer.has_value()
        && functionContainsOpcode(lir, flow.fn, *subSpReg)) {
        reservedFramePointer = cc->framePointer->ordinal;
    }
    // Publish the reservation so the rewriter's scratch-pool build
    // (pickScratchRegs) also holds the frame pointer out — otherwise it would
    // harvest the reserved-but-unassigned register as a spill scratch.
    out.reservedFramePointer = reservedFramePointer;
    FreeListsByClass free =
        buildFreeLists(schema, *cc, computeReloadReserve(lir, schema, flow),
                       reservedFramePointer);
    std::vector<std::uint32_t> const callPositions =
        collectCallPositions(lir, schema, flow);
    // Cycle 10q closure of 10p substrate: per-opcode implicit
    // clobbers (e.g., x86 idiv/div clobber RDX). One scan, consumed
    // by every range that crosses an implicit-clobber position.
    std::vector<ImplicitClobberAt> const implicitClobbers =
        collectImplicitClobberPositions(lir, schema, flow);
    // D-AS-REGALLOC-ARG-REGISTER-OCCUPIED (c75 correctness fix): the
    // incoming arg registers, live [entry, argOp.late). A vreg alive in
    // that window must not be assigned the arg register that still holds
    // its param (variant 2: the allocator assigning xmm7 — the 8th FP
    // arg reg — to a non-incoming vreg's home clobbers the incoming
    // param it aliases). Consumed by the covered-window exclusion below.
    std::vector<ArgRegisterOccupiedAt> const argOccupied =
        collectArgRegisterOccupied(lir, schema, *cc, flow);
    // FC4 c2 (R2): indirect-call callee vregs (see IndirectCalleeAt's
    // docblock for the silent-garbage-jump hazard this rule closes).
    // The cc's arg-register ordinal set is resolved ONLY when the
    // function actually contains an indirect callee — zero new
    // failure modes for every function without one. A cc register
    // name that fails to resolve is a schema misconfiguration: fail
    // LOUD here rather than allocate with a weakened exclusion (the
    // missing ordinal would re-open the clobber hazard silently).
    std::vector<IndirectCalleeAt> const indirectCallees =
        collectIndirectCalleePositions(lir, schema, flow);
    std::vector<std::uint16_t> ccArgRegOrdinals;
    std::optional<std::uint16_t> ccVariadicCountRegOrdinal;
    std::optional<std::uint16_t> ccIndirectResultRegOrdinal;
    if (!indirectCallees.empty()) {
        auto const resolveInto = [&](std::vector<std::string> const& names)
            -> bool {
            for (auto const& name : names) {
                auto const ord = schema.registerByName(name);
                if (!ord.has_value()) {
                    report(reporter,
                           DiagnosticCode::R_CallingConventionLookupFailed,
                           DiagnosticSeverity::Error,
                           std::format("regalloc: cc '{}' arg register "
                                       "'{}' does not resolve in the "
                                       "target register table — cannot "
                                       "build the indirect-callee "
                                       "exclusion set",
                                       cc->name, name));
                    return false;
                }
                ccArgRegOrdinals.push_back(*ord);
            }
            return true;
        };
        if (!resolveInto(cc->argGprs) || !resolveInto(cc->argFprs)) {
            out.ok = false;
            return out;
        }
        if (cc->variadicVectorCountReg.has_value()) {
            ccVariadicCountRegOrdinal = cc->variadicVectorCountReg->ordinal;
        }
        // D-FC7-INDIRECT-X8-SRET-CALLEE-EXCLUSION: an indirect call that
        // returns a by-value aggregate via the cc's indirect-result register
        // (x8 on AAPCS64) gets a POST-regalloc `mov x8, callee` reroute move;
        // keep the callee vreg off that register too (it is caller-saved and
        // NOT in argGprs, so the arg-reg exclusion above does not cover it).
        if (cc->indirectResultRegister.has_value()) {
            ccIndirectResultRegOrdinal = cc->indirectResultRegister->ordinal;
        }
    }

    std::uint32_t maxVRegId = 0;
    for (auto const& r : flow.ranges) {
        if (r.vreg.id > maxVRegId) maxVRegId = r.vreg.id;
    }
    out.assignments.assign(maxVRegId + 1u, LirRegAssignment{});

    std::vector<ActiveEntry> active;
    active.reserve(flow.ranges.size());

    // Exclusion scratch, hoisted out of the range loop
    // (D-OPT-REGALLOC-EXCLUSION-BUFFER closure, 2026-06-11). Holds
    // the per-range union of (a) requires2Address operand[1..N]
    // clobber-prevention + (b) implicit-register clobbers from
    // opcodes the range COVERS + (c) the result-def implicit
    // (inputs ∪ clobbered) set. `clear()` keeps capacity, so the
    // loop is allocation-free after the high-water mark — and the
    // buffer GROWS for any declared union size (the schema loader
    // places no cap on `implicitRegisters` lists, so the prior
    // fixed array<uint16_t, 8> + its two regallocFatal overflow
    // arms were not total; push_back can never truncate the
    // exclusion contract).
    std::vector<std::uint16_t> excludedScratch;

    SpillStats spills;
    // Slots start at 1; slot 0 is the LirSpillSlot invalid sentinel.
    std::uint32_t nextSlotV = 1;

    auto mintSlot = [&]() -> LirSpillSlot {
        LirSpillSlot const s{nextSlotV++};
        ++out.numSpillSlots;
        return s;
    };

    for (auto const& r : flow.ranges) {
        if (r.vreg.id == 0) continue;
        LirRegClass const cls = r.vreg.regClass();
        if (cls == LirRegClass::None) {
            report(reporter, DiagnosticCode::R_VRegHasNoClass,
                   DiagnosticSeverity::Error,
                   std::format("func {} vreg id {} has LirRegClass::None — "
                               "run LirVerifier before allocator",
                               flow.fn.v,
                               static_cast<std::uint32_t>(r.vreg.id)));
            continue;
        }

        expireActive(active, free, r.start);

        bool const crossesCall = rangeCrossesCall(r, callPositions);

        // D-CSUBSET-BINOP-RIGHT-CLOBBER closure (2026-06-02): when
        // this range is the result of a `requires2Address` opcode,
        // the legalize pass will emit `mov result, ops[0]` to
        // satisfy the 2-addr constraint when `result != ops[0]`.
        // That mov CLOBBERS the destination register's prior value
        // — if the allocator assigned `result` to a register that
        // also holds operand[k>=1]'s value, the second source is
        // destroyed before the binary op reads it (`add result,
        // [result, result]` instead of `add result, [result,
        // ops[k]]`). Prevent by EXCLUDING operand[1..N]'s physical
        // registers from this allocation. Operand[0] alias remains
        // permitted (and preferred — the coalesce case where
        // legalize emits no mov at all).
        //
        // Universal across CPUs: the schema's `requires2Address`
        // flag drives the exclusion; no `if (target == X)` branch.
        // Universal across commutativity: the bug fires for both
        // commutative and non-commutative 2-addr ops; both want
        // the same exclusion.
        // The exclusion scratch (hoisted above) holds the union of
        // (a) requires2Address operand[1..N] clobber-prevention +
        // (b) cycle-10q implicit-register clobbers from COVERED
        // opcodes (e.g., x86 idiv's RDX) + (c) the FC3.5 result-def
        // rule below: the DEFINING op's own implicit (inputs ∪
        // clobbered) set when it is requires2Address (x86
        // shift-by-CL's RCX). Today's worst case across the shipped
        // schemas: (a) ≤ 1 + (b) ≤ 3 distinct + (c) ⊆ {RCX},
        // dedup'd — union ≤ 4. The buffer is GROWABLE
        // (D-OPT-REGALLOC-EXCLUSION-BUFFER ✅ CLOSED 2026-06-11):
        // a schema declaring a union of ANY size allocates correctly
        // with every declared ordinal excluded — the prior fixed
        // array<uint16_t, 8> tripped regallocFatal past 8, which
        // was not total (the loader caps nothing).
        excludedScratch.clear();
        if (LirInstId const producingInst =
                (r.start < flow.positionToInst.size())
                    ? flow.positionToInst[r.start]
                    : LirInstId{};
            producingInst.valid()) {
            auto const opcode = lir.instOpcode(producingInst);
            auto const* info  = schema.opcodeInfo(opcode);
            // HIGH-3 silent-failure fold (2026-06-02): verify the
            // looked-up instruction actually DEFINES `r.vreg`. The
            // liveness builder produces `start = 0` for use-only
            // vregs (a verifier-rejected shape, but defense-in-
            // depth here): `positionToInst[0]` returns the first
            // inst in the function, which is unrelated to r.vreg.
            // Without this check, an unrelated 2-addr op's
            // operands would silently drive the exclusion set and
            // misallocate r.vreg. Skip the exclusion when the
            // looked-up inst isn't this range's definer.
            if (info != nullptr && info->requires2Address
                && lir.instResult(producingInst) == r.vreg) {
                auto const ops = lir.instOperands(producingInst);
                // The 2026-06-02 HIGH-2 fixed-buffer overflow
                // pre-check (`ops.size() > excludedStorage.size()
                // + 1`) is gone with the growable scratch
                // (D-OPT-REGALLOC-EXCLUSION-BUFFER closure): an
                // N-ary requires2Address op of ANY arity now has
                // every operand[1..N] ordinal excluded — push_back
                // cannot truncate, so the fail-loud arm's job
                // (never silently drop an exclusion) is satisfied
                // by construction.
                // Skip operand[0] (legitimate coalesce target).
                for (std::size_t k = 1; k < ops.size(); ++k) {
                    if (ops[k].kind != LirOperandKind::Reg) continue;
                    LirReg const opReg = ops[k].reg;
                    // Source reg may be already-physical (e.g. from
                    // an `arg` lowering pre-coalesced to a phys reg)
                    // or a vreg we've assigned earlier in this loop.
                    // LirReg's `id` field holds the ordinal in BOTH
                    // forms — for physical regs the id IS the
                    // physical ordinal; for vregs it's the vreg id
                    // and we route through the assignments table.
                    std::uint16_t ord = 0;
                    if (opReg.isPhysical) {
                        ord = static_cast<std::uint16_t>(opReg.id);
                    } else {
                        if (opReg.id == 0
                            || opReg.id >= out.assignments.size()) {
                            continue;
                        }
                        auto const& a = out.assignments[opReg.id];
                        if (a.isSpilled()) continue;
                        // Skip if the assignment was never set
                        // (default-constructed sentinel has zero
                        // classKind, hence `!valid()`).
                        if (!a.vreg.valid()) continue;
                        ord = static_cast<std::uint16_t>(
                            a.physReg().id);
                    }
                    excludedScratch.push_back(ord);
                }
                // FC3.5 sweep-c1 CRITICAL fix (2026-06-11): the
                // RESULT of a requires2Address op that ALSO declares
                // implicit input/clobbered registers must avoid those
                // registers. The 2-addr legalize materializes
                // `mov result, ops[0]` BEFORE the op, so the result's
                // physical register becomes a live conduit for ops[0]
                // ACROSS the op's implicit-register read — result ==
                // implicit-input means that mov destroys the pinned
                // value (x86 shift-by-CL: lowerShift emits `mov rcx,
                // count`, then the legalize's `mov result(=rcx),
                // value` overwrites the count → the shift computes
                // value << (value & 63) instead of value << count;
                // SILENT miscompile under register pressure). The
                // covered-position exclusion below CANNOT catch this:
                // the result's range STARTS at the op's LATE slot
                // (lir_liveness firstDef = latePos) while the
                // implicit-clobber entry sits at the EARLY slot, so
                // `c.position < r.start` skips it. Generic over the
                // DECLARED implicitRegisters (inputs ∪ clobbered —
                // the same union collectImplicitClobberPositions
                // forbids for covering operands); any future
                // requires2Address op with implicit registers is
                // covered by construction — no shift/RCX identity.
                // The div family never enters this arm (idiv_op/
                // div_op declare `result: none`; their SSA result is
                // captured by a separate post-op mov, so result ==
                // RAX is benign there).
                if (info->implicitRegisters.has_value()) {
                    auto const& ir = *info->implicitRegisters;
                    // Dedup'd append; the growable scratch makes
                    // this total for any declared union size — the
                    // prior fixed-buffer regallocFatal arm is gone
                    // (D-OPT-REGALLOC-EXCLUSION-BUFFER closure;
                    // removal is sound because push_back can never
                    // drop a declared ordinal).
                    auto addForbidden = [&](std::uint16_t ord) {
                        for (std::uint16_t const e : excludedScratch) {
                            if (e == ord) return;
                        }
                        excludedScratch.push_back(ord);
                    };
                    for (auto const o : ir.inputOrdinals) {
                        addForbidden(o);
                    }
                    for (auto const o : ir.clobberedOrdinals) {
                        addForbidden(o);
                    }
                }
            }
        }
        // Augment exclusion with implicit-register clobbers from any
        // opcode this range crosses (cycle 10q substrate consumer).
        // Universal across CPUs — driven entirely by the per-opcode
        // schema declaration; no `if (opcode == idiv)` branch.
        implicitClobbersCrossedBy(r, implicitClobbers, excludedScratch);
        // FC4 c2 (R2): when THIS range is the callee vreg of an
        // indirect call it covers, exclude the cc's argGprs ∪ argFprs
        // (+ the variadic vector-count register when that call's
        // payload is variadic). The callee is consumed AT the call
        // (`range.end == call.early + 1`), so covered-position
        // semantics — NOT rangeCrossesCall — are required, exactly
        // like the compound-op clobbers above: the arg-passing moves
        // the materializer emits post-regalloc land BETWEEN the
        // callee's def and the call. cc-config-driven only.
        if (!indirectCallees.empty()) {
            auto const addExcluded = [&](std::uint16_t ord) {
                for (std::uint16_t const e : excludedScratch) {
                    if (e == ord) return;
                }
                excludedScratch.push_back(ord);
            };
            for (auto const& ic : indirectCallees) {
                if (ic.calleeVregId != r.vreg.id) continue;
                if (ic.position < r.start || ic.position >= r.end) continue;
                for (std::uint16_t const ord : ccArgRegOrdinals) {
                    addExcluded(ord);
                }
                if (ic.variadic && ccVariadicCountRegOrdinal.has_value()) {
                    addExcluded(*ccVariadicCountRegOrdinal);
                }
                if (ic.indirectResult
                    && ccIndirectResultRegOrdinal.has_value()) {
                    addExcluded(*ccIndirectResultRegOrdinal);
                }
            }
        }
        // D-AS-REGALLOC-ARG-REGISTER-OCCUPIED (c75 correctness fix): an
        // incoming arg register is live over [entry=0, releasePos). THIS
        // range conflicts with it iff it starts before the register is
        // freed (`r.start < releasePos`) — contiguous ranges start at
        // their def, so `start < releasePos` ⇔ overlap with [0,
        // releasePos). Exclude the arg-register ordinal from this range's
        // allocation UNLESS this range IS that arg op's own result vreg
        // (its start == releasePos, so `start < releasePos` is already
        // false — the guard is belt-and-suspenders). Covers variant 2
        // (an earlier param's home vreg, or any temp, is kept off a
        // later param's still-live incoming register). Threaded through
        // tryAllocateExcluding AND findSpillCandidate below, so a spill-
        // evict never re-lands the freed register on an arg ordinal
        // either. cc-config-driven; no register names, no arch identity.
        if (!argOccupied.empty()) {
            auto const addExcludedArg = [&](std::uint16_t ord) {
                for (std::uint16_t const e : excludedScratch) {
                    if (e == ord) return;
                }
                excludedScratch.push_back(ord);
            };
            for (auto const& ao : argOccupied) {
                if (ao.cls != cls) continue;              // class-partitioned pools
                if (r.vreg.id == ao.paramVregId) continue; // its own home
                if (r.start < ao.releasePos) addExcludedArg(ao.ordinal);
            }
        }

        std::span<std::uint16_t const> const excluded{
            excludedScratch.data(), excludedScratch.size()};

        if (auto pick = tryAllocateExcluding(free, cls, crossesCall, excluded);
            pick.has_value()) {
            LirReg const phys = makePhysicalReg(pick->ordinal, cls);
            out.assignments[r.vreg.id] =
                LirRegAssignment::makePhys(r.vreg, phys);
            active.push_back({r, cls, pick->ordinal, pick->isCalleeSaved});
            continue;
        }

        // Invariant: every spill emits exactly one slot increment via
        // `mintSlot` and contributes to one `SpillStats` counter.
        // The `excluded` set is propagated so the evictee's physical
        // ordinal is never in operand[1..N]'s set — closes the
        // silent-failure HIGH-1 audit fold: without this, the spill
        // fallback could free a register the exclusion explicitly
        // forbids, recreating the clobber bug under register pressure.
        auto const spillIt = findSpillCandidate(active, cls, crossesCall,
                                                 excluded);
        bool const evictCandidate =
            spillIt != active.end() && spillIt->range.end > r.end;

        if (!evictCandidate) {
            // Spill r itself.
            LirSpillSlot const slot = mintSlot();
            out.assignments[r.vreg.id] =
                LirRegAssignment::makeSpill(r.vreg, slot);
            if (crossesCall) ++spills.crossCallExhaustion;
            else             ++spills.pressure;
            continue;
        }

        // Evict spillIt: its vreg goes to a new spill slot; r gets its
        // physical register. The evicted range's spill cause is its
        // OWN crossesCall status, not r's — they may differ.
        LirSpillSlot const slot = mintSlot();
        out.assignments[spillIt->range.vreg.id] =
            LirRegAssignment::makeSpill(spillIt->range.vreg, slot);
        bool const evictedCrossesCall =
            rangeCrossesCall(spillIt->range, callPositions);
        if (evictedCrossesCall) ++spills.crossCallExhaustion;
        else                    ++spills.pressure;

        std::uint16_t const freedOrdinal = spillIt->physOrdinal;
        bool const freedIsCalleeSaved    = spillIt->isCalleeSaved;
        active.erase(spillIt);

        LirReg const phys = makePhysicalReg(freedOrdinal, cls);
        out.assignments[r.vreg.id] =
            LirRegAssignment::makePhys(r.vreg, phys);
        active.push_back({r, cls, freedOrdinal, freedIsCalleeSaved});
    }

    emitSpillSummary(reporter, flow.fn, spills);
    out.ok = (reporter.errorCount() == baseline);
    return out;
}

} // namespace

LirFuncAllocation
allocateFuncRegisters(Lir const&             lir,
                      TargetSchema const&    schema,
                      LirFuncLiveness const& flow,
                      std::uint16_t          callingConventionIndex,
                      DiagnosticReporter&    reporter) {
    bool const schemaOk = (schema.callingConventionCount() > 0);
    if (!schemaOk) {
        report(reporter, DiagnosticCode::R_NoCallingConventions,
               DiagnosticSeverity::Error,
               "target schema declares no calling conventions");
    }
    return allocateOneFunc(lir, schema, flow,
                           callingConventionIndex,
                           reporter, schemaOk);
}

LirAllocation
allocateRegisters(Lir const&          lir,
                  TargetSchema const& schema,
                  LirLiveness const&  liveness,
                  std::uint16_t       callingConventionIndex,
                  DiagnosticReporter& reporter) {
    LirAllocation out;
    bool const schemaOk = (schema.callingConventionCount() > 0);
    if (!schemaOk) {
        // Emit ONCE at module level rather than re-emitting per-
        // function (which would hit the reporter's dedup window after
        // the 4th identical message).
        report(reporter, DiagnosticCode::R_NoCallingConventions,
               DiagnosticSeverity::Error,
               "target schema declares no calling conventions");
    }
    out.perFunc.reserve(liveness.perFunc.size());
    for (auto const& flow : liveness.perFunc) {
        out.perFunc.push_back(
            allocateOneFunc(lir, schema, flow,
                            callingConventionIndex, reporter, schemaOk));
    }
    return out;
}

} // namespace dss
