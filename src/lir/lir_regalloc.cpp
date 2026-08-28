#include "lir/lir_regalloc.hpp"

#include "core/types/call_payload.hpp"
#include "core/types/parse_diagnostic.hpp"
// `ArgCursors` + `argPassingRegister` — the ONE owner of "which register does
// outgoing argument k of class C land in", reused by the outgoing-argument
// pre-coloring hint rather than re-derived. A .cpp-level dependency only:
// `lir_callconv.hpp` includes `lir_regalloc.hpp` (for `LirAllocation`), so the
// edge runs one way and no header cycle is created.
#include "lir/lir_callconv.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"
// `kByValueStackArgExhaust{Gpr,Fpr}` — the by-value stack-aggregate class-
// exhaust codes the outgoing-argument cursor walk must honour, read from the
// same declaration `lir_rewrite` and `lir_callconv` read them from.
#include "mir/mir_opcode.hpp"
#include "lir/lir_reg_constraints.hpp"

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
#include <tuple>
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

    // D-TARGET-ALLOCATABLE-POOL-LIST-SET-HAS-NO-OWNER: the six cc lists that
    // make a register allocatable used to be spelled out HERE and again in
    // `lir_rewrite::collectAllocatable`, two hand-kept copies of one set. They
    // now read the same published table, `kAllocatablePoolLists`
    // (target_schema.hpp), which is also what `TargetSchemaData::validate()`
    // judges — so the set the allocator absorbs and the set the loader
    // validates are the same object rather than two lists that agree today.
    // ⚠ `argVrs`/`returnVrs` are DELIBERATELY not in that table; adding them
    // is refused at LOAD on any target whose vector and float views alias
    // (D-TARGET-ALIASED-VIEWS-BOTH-ALLOCATABLE-DOUBLE-COUNT-ONE-FILE).
    std::unordered_set<std::string_view> allocatable;
    for (auto const list : kAllocatablePoolLists) {
        for (auto const& n : cc.*list) allocatable.insert(n);
    }

    std::unordered_set<std::string_view> callerSet;
    callerSet.reserve(cc.callerSaved.size());
    for (auto const& n : cc.callerSaved) callerSet.insert(n);

    auto const regs = schema.registers();
    for (std::uint16_t i = 0; i < regs.size(); ++i) {
        auto const& info = regs[i];
        if (info.regClass == TargetRegClass::None) continue;
        // D-TARGET-CC-NAMES-SUB-REGISTER: no `subOf` skip here. A
        // sub-register can only reach this pool by being named in one of
        // the cc lists `allocatable` was built from, and `TargetSchema::
        // validate()` now REJECTS such a config at load. The skip that
        // used to sit on this line was therefore unreachable — and being
        // unreachable, untestable, and so free to rot into a
        // false-comfort guard. The load-time rule is the single
        // enforcement point; this loop simply trusts it.
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
    // END and SKIPPING any arg/sret ordinal.
    //
    // ⓘ THE RESERVE IS UNAFFECTED BY THE OPT8 PARTITION PREFERENCE, and
    // the reason is the ORDER OF OPERATIONS rather than an argument about
    // pressure: these K registers are held back BEFORE the free lists are
    // built, so they are never in circulation for `tryAllocate` to prefer
    // in the first place. (The comment here used to describe caller-saved
    // as "the allocator's last-choice partition"; a non-call-crossing
    // range now prefers it — see `tryAllocate`'s docblock.) cc-config-driven
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
// ★★★ OUTPUTS ARE NOT FORBIDDEN, AND THE REASON IS DIFFERENT FOR THE TWO
// KINDS OF VALUE THIS EXCLUSION SET GOVERNS. THE OLD ONE-LINE ARGUMENT
// COVERED ONLY THE FIRST AND WAS SILENTLY CARRIED OVER TO THE SECOND, WHICH
// IS THE DEFECT
// D-LIR-PER-INSTRUCTION-OUTPUTS-NOT-ENFORCED-SUBSET-OF-CLOBBERED NAMES.
//   - An OPERAND OF THIS INSTRUCTION may share a register with an implicit
//     output, and that is safe ON ITS OWN TERMS: the op reads its operands
//     before it writes its outputs. This half needs no invariant, and it is
//     what the argument was originally about.
//   - A RANGE THAT MERELY CROSSES this position — the case this very function
//     collects — is NOT read by the instruction, so "reads before writes"
//     says nothing whatever about it. A register the instruction WRITES
//     destroys such a value exactly as a declared clobber would. What keeps
//     it safe is a SEPARATE guarantee this exclusion set does not restate:
//     `outputs ⊆ clobbered`, so the clobber half of the union already
//     contains every output.
// ⇒ the omission is not free-standing; it is a DEPENDENCY, and the guarantee
// is enforced at both of the type's producers rather than assumed: the
// `.target.json` loader validates it for the per-OPCODE carrier, and
// `LirBuilder::regConstraintPoolAdd` refuses a per-INSTRUCTION entry that
// violates it (`ImplicitRegisterConstraint::firstOutputNotClobbered` is the
// one implementation of the rule; the inline-asm lowering pre-validates with
// the same query, because a tier fed by USER text owes a diagnostic rather
// than a process kill). ⛔ DO NOT "simplify" this by teaching the exclusion
// set to forbid outputs instead: that diverges the per-instruction path from
// the per-opcode one and pessimises every compound op (`idiv`, shift-by-CL)
// whose output legitimately aliases an operand — the first bullet above.
// ⛔ And do not delete the dependency sentence as redundant: an argument that
// silently rests on a guarantee it does not name is what let the
// per-instruction carrier ship for a cycle with no producer-side check at all.
//
// ★ CHOKEPOINT (2026-08-15, D-LIR-PER-INST-REG-CONSTRAINTS): the union is no
// longer built here. It comes from `effectiveForbiddenOrdinals`, which reads
// BOTH carriers — the per-OPCODE `implicitRegisters` this comment describes
// AND the per-INSTRUCTION pool an inline-asm statement fills. Before that,
// this site (and the two others that built the same union by hand) read only
// the opcode carrier, so a faithfully-carried per-instruction clobber set was
// silently ignored by the only consumer that can act on it.
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
            std::vector<std::uint16_t> forbidden =
                effectiveForbiddenOrdinals(lir, schema, inst);
            if (!forbidden.empty()) {
                out.push_back({pos, std::move(forbidden)});
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
// Caught by `examples/c/division/` exiting with the OS's
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
    // OPT8: the COALESCED CLASS this entry stands for. `range` is that class's
    // hull, and eviction must spill every member — a spill written onto the
    // representative alone would leave the other members pointing at a register
    // the evicting range now owns, which is a silent miscompile rather than a
    // missed optimization. Zero for a caller that builds an entry outside the
    // class-driven scan (there is none today; the field is not optional).
    std::uint32_t classRoot = 0;
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

// ── THE PARTITION PREFERENCE (plan 22 OPT8) ─────────────────────────────
//
// ★★★ A RANGE THAT DOES NOT CROSS A CALL PREFERS A **CALLER-SAVED**
// REGISTER, AND THE ORDER USED TO BE THE OTHER WAY ROUND.
//
// The ABI envelope is unchanged and is the part that must not move: a range
// that DOES cross a call may only be assigned a callee-saved register,
// because the callee is free to destroy every caller-saved one. That clause
// is still the first thing this function checks.
//
// What changed is the preference for the ranges the envelope does NOT
// constrain. A callee-saved register is not free: the function that uses one
// must SAVE it in its prologue and RESTORE it in its epilogue, which is two
// memory instructions plus (for the first one in a frameless function) the
// `sub rsp` / `add rsp` pair that makes somewhere to put it. A caller-saved
// register costs nothing at all to a range that never crosses a call.
// Handing out the expensive partition first therefore bought a leaf function
// a prologue it did not need, AND consumed the very registers that the
// call-crossing ranges are the only legitimate consumers of — so it made
// spills more likely at the same time.
//
// ✔MEASURED on the emitted `examples/c/**` artifacts before this change
// (2026-08-25, 561 ELF64/x86_64 release artifacts): 3352 callee-saved
// prologue SAVES, each with its epilogue restore — 6704 instructions, 5.1%
// of the whole emitted stream — and 20.5% of all instructions touch the
// stack. The `int main(void){return 42;}` shape emitted SEVEN instructions
// (`sub rsp` / save r15 / `mov $42,r15` / `mov r15,rax` / restore r15 /
// `add rsp` / `ret`) where the references emit two.
//
// ⚠ THE ENVELOPE IS WHAT MAKES THIS SAFE, NOT THE MEASUREMENT. Nothing
// below widens what a cross-call range may be given; the caller-saved arm is
// reachable only when `crossesCall` is false, exactly as before. A
// non-cross-call range was ALREADY allowed both partitions — this only
// changes which it is offered first.
[[nodiscard]] std::optional<AllocPick>
tryAllocate(FreeListsByClass& free, LirRegClass cls, bool crossesCall) {
    auto& bucket = free[static_cast<std::size_t>(cls)];
    if (!crossesCall) {
        if (auto r = popReg(bucket.callerSaved); r.has_value()) {
            return AllocPick{*r, false};
        }
    }
    if (auto r = popReg(bucket.calleeSaved); r.has_value()) {
        return AllocPick{*r, true};
    }
    return std::nullopt;
}

// Pop a free register, SKIPPING any ordinal in `excluded`. Matches
// the `tryAllocate` policy exactly (a call-crossing range takes only
// callee-saved; any other range prefers caller-saved and falls back to
// callee-saved) but removes the picked entry only when it's
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
    // Same partition preference as `tryAllocate` — see its docblock. The two
    // must agree, because which one runs is decided by whether the range
    // happens to have an exclusion set, which is not a property a register
    // choice may depend on.
    if (!crossesCall) {
        if (auto r = popFiltered(bucket.callerSaved); r.has_value()) {
            return AllocPick{*r, false};
        }
    }
    if (auto r = popFiltered(bucket.calleeSaved); r.has_value()) {
        return AllocPick{*r, true};
    }
    return std::nullopt;
}

// ── PRE-COLORING: COALESCING A VREG AGAINST A **PHYSICAL** REGISTER ─────────
//
// ★★★ THE THIRD KIND OF COPY, AND ✔MEASURED 2026-08-26 IT IS THE BIG ONE. Over
// 500 `examples/c/**` compiles, counted through the compiler's own post-stage
// LIR dump: at POST-REWRITE — the tier the union-find coalescer acts on — only
// **1110** register-to-register copies survive. At POST-CALLCONV there are
// **14487**. `materializeCallingConvention` MINTS roughly thirteen thousand of
// them, and it runs AFTER `lir_peephole`, so R1 never sees one.
//
// They are ABI moves: `mov <param's home>, <incoming arg register>` per
// register-resident parameter, and the mirrored moves for outgoing arguments,
// call results and return values. Each is a copy whose one end is a FIXED
// PHYSICAL register, which no union-find over virtual registers can reach —
// there is no vreg on that side to merge with. The instrument that removes them
// is a PREFERENCE: allocate the vreg INTO the fixed register, and the copy
// stops being minted at all (`lir_callconv.cpp`'s `maybeMov` emits nothing when
// `dest.id == src.id`).
//
// This closes the long-standing `D-ML7-2.5` (plan 12) — "regalloc pre-coloring
// hint for `arg`/`call` arg-position vregs" — whose own trigger names this
// cycle: *"when the redundant-mov count becomes a measurable perf issue … or
// the codegen-quality/peephole arc (plan 22) opens"*. Plan 22 OPT8 is that arc.
//
// **A PREFERENCE, NEVER A PIN, AND THE DISTINCTION IS THE WHOLE SAFETY
// ARGUMENT.** This function can only reorder choices that were ALREADY legal:
// it draws from the same bucket `tryAllocate` would have drawn from, honours
// the same `excluded` set, and returns nullopt — falling back to the ordinary
// policy — whenever the preferred register is unavailable. It cannot widen what
// a range may be given, so every existing exclusion (cross-call envelope,
// implicit clobbers, arg-register occupancy, the two-address anti-clobber set)
// binds it unchanged.
//
// ⚠ WHICH SIDE IS HINTED, AND WHY ONLY THAT SIDE. The hint used here is the
// DEF-side one for an incoming parameter, and it comes from
// `collectArgRegisterOccupied` — i.e. from `lir_pass_util::incomingArgRegister`,
// the ONE formula the rewriter's spill-scratch forbid already shares. No second
// owner of "which register holds parameter k" is created by this.
[[nodiscard]] std::optional<AllocPick>
tryAllocatePreferred(FreeListsByClass& free, LirRegClass cls, bool crossesCall,
                     std::span<std::uint16_t const> excluded,
                     std::optional<std::uint16_t> preferred) {
    if (!preferred.has_value()) return std::nullopt;
    for (auto e : excluded) {
        if (e == *preferred) return std::nullopt;
    }
    auto takeFrom = [&](std::vector<std::uint16_t>& regs) -> bool {
        for (auto it = regs.begin(); it != regs.end(); ++it) {
            if (*it != *preferred) continue;
            regs.erase(it);
            return true;
        }
        return false;
    };
    auto& bucket = free[static_cast<std::size_t>(cls)];
    // Same partition order as `tryAllocate` — the preference reorders WITHIN
    // what the policy already allows and never across it. A cross-call range
    // still cannot be handed a caller-saved register, which is exactly what
    // makes an incoming-arg hint silently decline for a parameter that lives
    // across a call.
    if (!crossesCall && takeFrom(bucket.callerSaved)) {
        return AllocPick{*preferred, false};
    }
    if (takeFrom(bucket.calleeSaved)) return AllocPick{*preferred, true};
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

// ── the independent coalescing auditor (contract: `lir_regalloc.hpp`) ───────
//
// Deliberately NOT written in terms of the coalescer's data structures. Its
// only inputs are the ranges liveness produced and the assignments the
// allocator wrote — the two things a WRONG merge would have to corrupt
// together to escape. It is a pure function so a test can hand it a
// hand-built, deliberately-broken allocation and observe the refusal directly,
// rather than reading the arm and believing it.
std::optional<LirAllocationConflict>
findAllocationConflict(LirFuncLiveness const&   flow,
                       LirFuncAllocation const& alloc) noexcept {
    // Ranges carrying an assignment, paired with the resource they landed on.
    struct Placed {
        LirLiveRange const* range = nullptr;
        std::uint32_t       resource = 0;
        bool                isSpill  = false;
    };
    std::vector<Placed> placed;
    placed.reserve(flow.ranges.size());
    for (auto const& rng : flow.ranges) {
        if (rng.vreg.id == 0) continue;
        auto const* a = alloc.forVReg(rng.vreg.id);
        if (a == nullptr) continue;
        if (a->isSpilled()) {
            placed.push_back({&rng, a->spillSlot().v, true});
        } else {
            // A physical ordinal is only comparable WITHIN a register class:
            // GPR 0 and FPR 0 are different registers. Fold the class into the
            // key rather than comparing ordinals across classes, which would
            // manufacture conflicts that do not exist.
            placed.push_back({&rng,
                              (static_cast<std::uint32_t>(a->physReg().id) << 8)
                                  | static_cast<std::uint32_t>(
                                        a->vreg.regClass()),
                              false});
        }
    }
    // ── GROUP BY RESOURCE, THEN COMPARE ONLY ADJACENT RANGES.
    //
    // ⚠ THE ALL-PAIRS SCAN THIS REPLACED WAS A COMPILE-TIME HAZARD, NOT A
    // TIDINESS ONE. It ran on EVERY function of every compile, so its cost is
    // Σ n² over functions — a shape that is invisible on a corpus of small
    // examples and expensive on the one real-world function with twenty
    // thousand virtual registers. A wrong-answer guard has to be cheap enough
    // that nobody is ever tempted to make it conditional.
    //
    // ★ The reduction is exact, not a heuristic: within ONE resource, sort the
    // ranges by start; if any two of them overlap, then SOME ADJACENT PAIR
    // overlaps. (If `a` precedes `c` in start order and they overlap, then any
    // `b` between them has `a.start ≤ b.start ≤ c.start < a.end`, so `a` and
    // `b` overlap too.) One sort plus one linear pass per resource finds every
    // conflict the quadratic scan would have.
    std::sort(placed.begin(), placed.end(),
              [](Placed const& x, Placed const& y) {
                  return std::tie(x.isSpill, x.resource, x.range->start,
                                  x.range->vreg.id)
                       < std::tie(y.isSpill, y.resource, y.range->start,
                                  y.range->vreg.id);
              });
    for (std::size_t i = 1; i < placed.size(); ++i) {
        auto const& prev = placed[i - 1];
        auto const& cur  = placed[i];
        if (prev.isSpill != cur.isSpill) continue;
        if (prev.resource != cur.resource) continue;
        // Two vregs COALESCED into one class share a resource ON PURPOSE and
        // are not a conflict — but that is decided HERE by the ranges, never
        // by asking the coalescer whether it meant it.
        if (!lirRangesInterfere(*prev.range, *cur.range)) continue;
        LirAllocationConflict c;
        c.a = prev.range->vreg;
        c.b = cur.range->vreg;
        c.isSpillSlot = prev.isSpill;
        c.sharedResource = prev.isSpill ? prev.resource : (prev.resource >> 8);
        return c;
    }
    return std::nullopt;
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

// ── REGISTER COALESCING (plan 22 OPT8) ──────────────────────────────────────
//
// The contract, the two edge kinds and the three vetoes are written out in
// `lir_regalloc.hpp`'s "REGISTER COALESCING" section; what follows is the
// mechanism. Everything here reads DECLARED vocabulary — the class move
// resolved through `regClassOpOpcode`, the register's own `widthBytes`, the
// opcode's `requires2Address` index — and holds no mnemonic, no register name
// and no target identity.

// The full declared width, in bits, of register class `cls` on this target:
// the widest register the table declares for that class.
//
// ⚠ WHY THE **WIDEST** AND NOT THE FIRST. A target register table declares the
// narrow VIEWS alongside their parents (x86-64 `eax` is a 4-byte GPR row whose
// `subOf` is `rax`), so "the first GPR row" answers 4 bytes on a 64-bit machine.
// Taking the maximum answers the question this predicate actually asks — is
// this copy writing the WHOLE register — which is the same question
// `lir_peephole`'s R1 asks post-regalloc, where it can read the assigned
// register's own row directly. Pre-regalloc there is no assigned row yet, so
// the class's own maximum is the honest stand-in, and it is CONSERVATIVE in the
// direction that matters: a narrow copy can never equal it, so a partial-
// register write is never mistaken for a full one.
//
// Returns 0 when the class declares no register at all — a caller reads that as
// "cannot prove full width" and coalesces nothing.
[[nodiscard]] std::uint32_t
classFullWidthBits(TargetSchema const& schema, LirRegClass cls) noexcept {
    std::uint32_t widest = 0;
    for (auto const& info : schema.registers()) {
        if (static_cast<LirRegClass>(info.regClass) != cls) continue;
        auto const bits = static_cast<std::uint32_t>(info.widthBytes) * 8u;
        if (bits > widest) widest = bits;
    }
    return widest;
}

// One copy-affinity edge: `dst` and `src` are copy-related vreg ids and want
// the same physical register.
struct CopyEdge {
    std::uint32_t dst = 0;
    std::uint32_t src = 0;
};

// One anti-affinity pair: `a` and `b` are vreg ids that must NEVER share a
// physical register even when their live ranges are disjoint. Veto 2 in the
// header — a `requires2Address` result against each UNTIED register operand of
// its own defining instruction.
struct AntiAffinityPair {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

// A PHYSICAL-register affinity: vreg `vregId` is one end of a copy whose other
// end is the fixed physical ordinal `ordinal`. There is no vreg on that side to
// merge with, so the affinity is expressed as a PREFERENCE at allocation time
// (`tryAllocatePreferred`) rather than as a union-find edge — get the vreg into
// that register and the copy becomes a move onto itself, which R1 deletes.
//
// ⓘ This is the shape `D-ML7-2.5`'s SECOND consumer names: the div/mod family
// captures its implicit-output register with `result = mov <rax>`, so the
// capture disappears exactly when the result vreg is allocated there. Nothing
// about the rule is div-specific — it reads the operand's `isPhysical` bit, so
// any lowering that pins one end of a copy gets the same treatment.
struct PhysAffinity {
    std::uint32_t vregId  = 0;
    std::uint16_t ordinal = 0;
};

struct CoalesceInput {
    std::vector<CopyEdge>         edges;
    std::vector<AntiAffinityPair> forbidden;
    std::vector<PhysAffinity>     physHints;
};

// The register a value of class `cls` is RETURNED in under `cc`, or nullopt
// when this function cannot answer for that class.
//
// ★★ IT DECLINES RATHER THAN GUESSES, AND THAT IS WHAT KEEPS IT FROM BECOMING
// A SECOND OWNER OF THE RETURN-REGISTER FORMULA. `lir_callconv.cpp`'s
// `returnRegisterForClass` is the owner: it MUST answer for every class,
// including the VR case that `D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR`
// records going wrong when a two-way `(cls == FPR) ? fprs : gprs` was written
// out by hand. This one answers ONLY where the vocabulary is unambiguous and
// returns nullopt everywhere else, so it cannot reproduce that defect: the
// class it refuses is exactly the class the real formula has to think about.
//
// ⚠ AND THE CONSEQUENCE OF BEING WRONG IS BOUNDED BY CONSTRUCTION. The answer
// is used ONLY as a `tryAllocatePreferred` hint. A hint that names the wrong
// register produces a legal allocation in a different register, and
// `materializeCallingConvention` then emits the copy it would always have
// emitted — a missed optimization, never a wrong answer. Nothing downstream
// reads this to decide where the value IS.
[[nodiscard]] std::optional<std::uint16_t>
returnRegisterHint(TargetSchema const& schema,
                   TargetCallingConvention const& cc, LirRegClass cls) {
    std::vector<std::string> const* names = nullptr;
    if (cls == LirRegClass::GPR)      names = &cc.returnGprs;
    else if (cls == LirRegClass::FPR) names = &cc.returnFprs;
    else                              return std::nullopt;
    if (names->empty()) return std::nullopt;
    return schema.registerByName(names->front());
}

// Resolve the class-move opcode per register class, once per function.
// Deliberately the SAME resolution `lir_2addr_legalize` uses to SYNTHESIZE a
// copy and `lir_peephole` uses to RECOGNIZE one: three passes, one question,
// one owner (`TargetSchema::regClassOpOpcode`).
struct MoveOpcodeCache {
    TargetSchema const& schema;
    std::array<std::optional<std::optional<std::uint16_t>>,
               kLirRegClassCount> byClass{};
    std::array<std::optional<std::uint32_t>, kLirRegClassCount> widthByClass{};

    [[nodiscard]] std::optional<std::uint16_t> moveOpcode(LirRegClass cls) {
        auto const c = static_cast<std::size_t>(cls);
        if (c >= byClass.size()) return std::nullopt;
        if (!byClass[c].has_value()) {
            byClass[c] = schema.regClassOpOpcode(
                static_cast<TargetRegClass>(c), RegClassOp::Move);
        }
        return *byClass[c];
    }
    [[nodiscard]] std::uint32_t fullWidthBits(LirRegClass cls) {
        auto const c = static_cast<std::size_t>(cls);
        if (c >= widthByClass.size()) return 0;
        if (!widthByClass[c].has_value()) {
            widthByClass[c] = classFullWidthBits(schema, cls);
        }
        return *widthByClass[c];
    }
};

// ── THE OUTGOING-ARGUMENT HINT (D-ML7-2.5, the half that was withheld) ───────
//
// The DEF-side hints cover where a value is BORN: an incoming parameter
// arrives in its ABI register, a call result arrives in the return register.
// This one covers where a value must BE AT ITS LAST USE — the argument
// register the call reads it out of. Together they are the whole population of
// ABI copies `materializeCallingConvention` mints after the peephole has run.
//
// ★★★ WHY IT IS SAFE TO LAND, AND THE REASON IT WAS HELD BACK IS REFUTED BY
// EXECUTION RATHER THAN BY ARGUMENT. The stated hazard was that pre-coloring an
// outgoing argument makes some argument-move SOURCES be argument registers,
// producing the move-graph cycle `L_MoveCycleUnsupported` used to refuse. Two
// independent measurements say otherwise:
//
//   * THE REFUSAL IS GONE. `D-ML7-2.3`'s parallel-copy resolution shipped in
//     c76: `emitParallelRegMoves` emits the acyclic part in dependency order
//     and breaks each remaining cycle with a scratch drawn from
//     `cc.callerSaved`. The v1 O(N^2) detector it superseded was deleted.
//   * AND THE DEF-SIDE HINT ALREADY PRODUCES THAT EXACT SHAPE. Pinning an
//     incoming parameter to its home makes that home an argument register, so
//     `int f(int a,int b){ return g(b,a); }` is ALREADY a 2-cycle whose two
//     sources are both argument registers — before this function exists.
//     ✔MEASURED on that source at post-callconv: three moves through a
//     scratch, and the program exits 42.
//
// ⇒ this hint reaches a population no DEF-side hint can (a COMPUTED value
// passed as an argument has no ABI birthplace to be hinted from) while adding
// no move-graph shape the allocator was not already producing.
//
// ⚠ THE ARGUMENT POSITION IS NOT THE OPERAND INDEX, AND THAT IS WHY THIS
// FUNCTION IS SHORT. Which register argument k lands in is a cursor walk:
// an indirect-result operand shifts the base, a by-value stack aggregate
// consumes a position and can EXHAUST a whole class, two pools may share one
// cursor when they are two views of one physical register file, `slotAligned`
// collapses every class onto a single positional cursor, and a variadic call
// past its fixed count may be forced to the stack. That walk has exactly ONE
// owner — `ArgCursors` — because it previously existed in three hand-kept
// copies that disagreed
// (D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR). This
// reuses the owner and re-derives nothing; likewise the slot→register step is
// `argPassingRegister`, the published owner of that lookup.
//
// ⓘ WHY PASSING THE REAL REPORTER ADDS NO FAILURE SURFACE. `argPassingRegister`
// refuses loudly in three cases — the class owns no pool, the cc declares the
// pool empty, the pool is exhausted — and all three are excluded by the guards
// below (a class with no row yields no cursor slot; an empty or overrun pool
// fails `index < poolSize`). Were one to fire anyway it would mean `ArgCursors`
// and `argPassingRegister` disagree, and `materializeCallingConvention` calls
// the SAME function with the SAME index for the SAME operand a few passes
// later — so the refusal is one this compile was already going to make, raised
// one tier earlier. Nothing here can refuse a program the pipeline accepts.
void collectOutgoingArgHints(std::span<LirOperand const> ops,
                             std::uint32_t                 payload,
                             TargetSchema const&           schema,
                             TargetCallingConvention const& cc,
                             DiagnosticReporter&           reporter,
                             CoalesceInput&                out) {
    ArgCursors argCursors{schema, cc};

    bool const hasIrr = ::dss::call_payload::hasIndirectResult(payload);
    std::size_t const firstArgIdx = hasIrr ? 2u : 1u;
    bool const variadicForcesStack =
        cc.variadicArgsAlwaysStack && ::dss::call_payload::isVariadic(payload);
    std::uint32_t const fixedOps = ::dss::call_payload::fixedOperandCount(payload);

    std::uint32_t argRegionIdx = 0;
    for (std::size_t k = firstArgIdx; k < ops.size(); ++k) {
        LirOperand const& argOp = ops[k];
        // The marker and its MemOffset are consumed WITH the carrier, never
        // argument positions of their own — the same reading
        // `classifyCallRegArgs` and the callconv placement site perform.
        if (lirIsByValueStackAggDescriptor(ops, k)) continue;
        if (lirIsByValueStackAggCarrier(ops, k)) {
            std::uint8_t const ex = ops[k + 1].byValueAggExhaust;
            if (ex == kByValueStackArgExhaustGpr) {
                argCursors.exhaust(LirRegClass::GPR);
            } else if (ex == kByValueStackArgExhaustFpr) {
                argCursors.exhaust(LirRegClass::FPR);
            }
            ++argRegionIdx;
            continue;
        }
        if (argOp.kind != LirOperandKind::Reg) { ++argRegionIdx; continue; }
        LirRegClass const cls = argOp.reg.regClass();
        // ⚠ THE CURSOR ADVANCES FOR EVERY REGISTER OPERAND, HINTED OR NOT.
        // Skipping the advance for an operand this function declines to hint
        // (a physical register, say) would shift every LATER argument's
        // position and hint them into the wrong registers.
        auto const slot = argCursors.next(cls);
        bool const forceStack = variadicForcesStack && argRegionIdx >= fixedOps;
        ++argRegionIdx;
        if (!slot.has_value()) continue;              // class owns no pool
        if (slot->index >= slot->poolSize) continue;  // stack-passed
        if (forceStack) continue;
        // Only a VIRTUAL register can be hinted; a physical one already IS
        // somewhere and the allocator has nothing left to choose.
        if (!argOp.reg.valid() || argOp.reg.isPhysical != 0) continue;
        auto const reg = argPassingRegister(
            schema, cc, slot->index, cls,
            "regalloc: outgoing-argument pre-coloring hint", reporter);
        if (!reg.has_value()) continue;
        out.physHints.push_back(
            {argOp.reg.id, static_cast<std::uint16_t>(reg->id)});
    }
}

// Collect both edge kinds and the anti-affinity pairs in ONE walk of the
// function, in instruction order — which is what makes the merge sequence
// deterministic and therefore the allocation reproducible.
[[nodiscard]] CoalesceInput
collectCoalesceInput(Lir const& lir, TargetSchema const& schema,
                     TargetCallingConvention const& cc,
                     LirFuncLiveness const& flow, MoveOpcodeCache& moves,
                     DiagnosticReporter& reporter) {
    CoalesceInput out;
    for (auto const& blk : flow.blockOrder) {
        std::uint32_t const n = lir.blockInstCount(blk);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = lir.blockInstAt(blk, i);
            auto const  opcode = lir.instOpcode(inst);
            auto const* info   = schema.opcodeInfo(opcode);
            if (info == nullptr) continue;
            LirReg const result = lir.instResult(inst);
            auto const   ops    = lir.instOperands(inst);

            // ── the IMPLICIT copy: a `requires2Address` (result, tied) pair,
            // plus the anti-affinity against every UNTIED register operand.
            // Both come from the same declaration, so a target that ties its
            // result to operand 1 gets the affinity on 1 and the veto on 0
            // with no edit here.
            if (info->requires2Address.has_value()) {
                std::size_t const tied = *info->requires2Address;
                if (result.valid() && result.isPhysical == 0) {
                    // The tied operand's own vreg id, needed BEFORE the loop:
                    // an untied operand that IS the tied one carries no
                    // clobber hazard and must not veto the merge.
                    std::uint32_t tiedId = 0;
                    if (ops.size() > tied && ops[tied].kind == LirOperandKind::Reg
                        && ops[tied].reg.valid()
                        && ops[tied].reg.isPhysical == 0) {
                        tiedId = ops[tied].reg.id;
                    }
                    for (std::size_t k = 0; k < ops.size(); ++k) {
                        if (ops[k].kind != LirOperandKind::Reg) continue;
                        LirReg const r = ops[k].reg;
                        if (!r.valid() || r.isPhysical != 0) continue;
                        if (k == tied) {
                            if (r.regClass() == result.regClass()) {
                                out.edges.push_back({result.id, r.id});
                            }
                            continue;
                        }
                        // ⚠ `add r, [x, x]` — the SAME vreg in both operand
                        // slots — is the shape that made this veto too strong.
                        // The hazard the veto exists for is legalize's
                        // `mov result, ops[tied]` destroying a DIFFERENT
                        // value; when the untied operand IS the tied one the
                        // copy is `mov R, R`, nothing is destroyed, and
                        // `add R, [R, R]` computes exactly x+x.
                        // ✔MEASURED: without this clause `int f(int x)
                        // { return x + x; }` — the simplest two-address shape
                        // there is — coalesced NOTHING.
                        if (tiedId != 0 && r.id == tiedId) continue;
                        out.forbidden.push_back({result.id, r.id});
                    }
                }
                continue;  // a 2-addr op is never also a plain copy
            }

            // ── the ABI copies `materializeCallingConvention` will mint AFTER
            // the peephole has run, hinted here so it mints nothing instead.
            // ✔MEASURED 2026-08-26 through the compiler's own post-stage LIR
            // dump over 500 examples: 1110 copies exist at post-rewrite and
            // 14487 at post-callconv, so this population is roughly THIRTEEN
            // TIMES the one a vreg-to-vreg union-find can reach.
            //
            //   * a CALL's result arrives in the cc's return register;
            //   * a RETURN's operand must be in it.
            //
            // Both are read from DECLARED vocabulary — `isCall` and
            // `terminatorKind == Return`, never a mnemonic — and both are
            // PREFERENCES, so a class that already owes its register to
            // something stronger simply keeps it.
            if (info->isCall) {
                if (result.valid() && result.isPhysical == 0) {
                    if (auto const rr = returnRegisterHint(schema, cc,
                                                           result.regClass());
                        rr.has_value()) {
                        out.physHints.push_back({result.id, *rr});
                    }
                }
                // ── and the USE side: every register-passed outgoing argument
                // wants to already BE in the register the call will read it
                // from. This is the population `materializeCallingConvention`
                // mints after the peephole has run — see
                // `collectOutgoingArgHints` for why it is safe and why the
                // position comes from `ArgCursors` rather than from `k`.
                collectOutgoingArgHints(ops, lir.instPayload(inst), schema, cc,
                                        reporter, out);
                continue;
            }
            if (info->terminatorKind == TargetTerminatorKind::Return) {
                for (auto const& o : ops) {
                    if (o.kind != LirOperandKind::Reg) continue;
                    if (!o.reg.valid() || o.reg.isPhysical != 0) continue;
                    if (auto const rr = returnRegisterHint(schema, cc,
                                                           o.reg.regClass());
                        rr.has_value()) {
                        out.physHints.push_back({o.reg.id, *rr});
                    }
                }
                continue;
            }

            // ── the EXPLICIT copy. The admission test is R1's, asked one tier
            // earlier and against VIRTUAL registers.
            if (info->isTerminator()) continue;
            if (info->hasSideEffects) continue;
            if (info->implicitRegisters.has_value()) continue;
            // A per-INSTRUCTION constraint entry is referenced BY INDEX from
            // the instruction stream; R1 refuses to delete such an instruction
            // for that reason, so merging across it would buy nothing and
            // would put a pinned-register instruction inside a merged class
            // whose exclusion set is computed per member. Fail-safe: skip.
            // Ask via the raw HANDLE, not the resolved pointer: the resolver
            // aborts on a dangling index, and a fail-safe skip must not be the
            // thing that kills the process on malformed input.
            if (lir.instRegConstraintHandle(inst) != kLirNoRegConstraints) {
                continue;
            }
            if (!result.valid()) continue;
            if (ops.size() != 1) continue;
            if (ops[0].kind != LirOperandKind::Reg) continue;
            LirReg const src = ops[0].reg;
            if (!src.valid()) continue;
            if (src.regClass() != result.regClass()) continue;
            auto const mov = moves.moveOpcode(result.regClass());
            if (!mov.has_value() || opcode != *mov) continue;
            // ── ONE END PHYSICAL: a PREFERENCE, not a union-find edge. Emitted
            // before the width clause deliberately — a preference cannot change
            // what any instruction MEANS (it only picks which legal register a
            // vreg gets), so the partial-register hazard the width clause
            // guards does not apply to it. The worst case for a narrow copy is
            // that the copy survives, which is where it started.
            if (result.isPhysical != 0 && src.isPhysical == 0) {
                out.physHints.push_back(
                    {src.id, static_cast<std::uint16_t>(result.id)});
                continue;
            }
            if (result.isPhysical == 0 && src.isPhysical != 0) {
                out.physHints.push_back(
                    {result.id, static_cast<std::uint16_t>(src.id)});
                continue;
            }
            if (result.isPhysical != 0 || src.isPhysical != 0) continue;
            // THE WIDTH CLAUSE — R1's second, independent guard. A copy
            // narrower than its register writes bits it did not read, so
            // merging its two ends changes what the surviving instruction
            // means. Only a FULL-width class move is an edge.
            auto const full = moves.fullWidthBits(result.regClass());
            if (full == 0) continue;
            if (static_cast<std::uint32_t>(lirInstWidthBits(lir.instFlags(inst)))
                != full) {
                continue;
            }
            out.edges.push_back({result.id, src.id});
        }
    }
    return out;
}

// The coalesced partition. `parent` is a union-find over vreg ids; `hull` holds
// each ROOT's merged live range (the interval the linear scan will actually
// hold the register over) and `members` its vreg ids.
//
// ★ THE HULL IS THE SUBJECT OF THE INTERFERENCE TEST, NOT THE MEMBER LIST, AND
// THAT IS DELIBERATE. The allocator will hold one register over the hull, so
// asking about the hull is asking about what will actually happen. A hull can
// only be coarser than the union of its members, so the test can only REFUSE
// merges a member-wise test would have allowed — never admit one it would have
// refused.
struct CoalescePartition {
    std::vector<std::uint32_t>              parent;
    std::vector<LirLiveRange>               hull;
    std::vector<std::vector<std::uint32_t>> members;
    std::uint32_t                           unions = 0;

    [[nodiscard]] std::uint32_t find(std::uint32_t x) noexcept {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];  // path halving
            x = parent[x];
        }
        return x;
    }
};

// ── THE PRESSURE MAP — VETO 4, AND IT IS **MEASURED**, NOT REASONED ─────────
//
// ★★★ COALESCING INTO A REGION WHERE A SPILL IS ALREADY FORCED MAKES THE
// FUNCTION WORSE, AND THE FIRST BUILD OF THIS PASS PROVED IT.
//
// ✔MEASURED 2026-08-26 over 517 `examples/c/**` ELF64/x86_64 artifacts, with
// vetoes 1–3 only: 1069 register-to-register copies removed and 151 examples
// smaller — while SIXTEEN grew, one by 96 instructions with 218 extra memory
// operands, and EVERY ONE of the sixteen was a function that already spills.
// `examples/c/arg_reg_fp_pressure` is the clean specimen: a 16-term
// floating-point accumulator chain. Each `sum = sum + x` is a two-address op,
// so the tied edges merge the WHOLE CHAIN into one class — and the resulting
// class has, by construction, the LATEST end of anything active.
// `findSpillCandidate` spills the latest-ending active range, so the merged
// accumulator became the guaranteed victim and sixteen register adds turned
// into sixteen store/reload pairs.
//
// ★ AND THE MERGE BOUGHT NOTHING THERE: the copy count was UNCHANGED (the
// pre-OPT8 allocator already handed that chain one register, by the free
// list's LIFO order). Pure cost, zero benefit — which is what makes this a
// veto rather than a trade-off to price.
//
// ⚠ THE FIRST HYPOTHESIS WAS WRONG AND IS RECORDED BECAUSE IT WAS TESTED. It
// looked like POOL pressure — a merge re-homing a non-crossing range into the
// scarcer callee-saved pool — so a veto on exactly that was built and
// measured: it fixed ONE of the sixteen regressions and gave up THIRTY-SEVEN
// improvements. Reverted. The mechanism is the spill-victim heuristic, not the
// pool split.
//
// **THE RULE:** refuse a merge whose hull spans a position where at least K
// same-class values are simultaneously live, K being the number of registers
// the calling convention makes allocatable for that class. Inside such a
// region SOMETHING must go to memory; a merged class is what will, and it
// takes every member's uses with it.
//
// ⓘ WHY THIS IS NOT THE COST MODEL OPT23 OWES. It prices nothing. There is no
// spill weight, no block frequency, no latency and no machine model — it
// compares a COUNT of live ranges against a COUNT of declared registers, both
// of which this function already has in hand. What a cost model would add is
// the judgement to overrule it ("this merge is worth a spill anyway"), which
// is a refinement of a rule that already errs toward leaving the pre-OPT8
// allocation alone.
//
// ⓘ AND IT IS EXACTLY THE CLASSICAL "CONSERVATIVE COALESCING" INTUITION
// (Briggs / George) reduced to what this substrate can answer honestly: with
// no interference GRAPH there are no neighbour degrees to count, but the live
// count at a position is a sound stand-in for the one question that matters —
// is this region already over capacity.
struct PressureMap {
    // `nextHot[cls][p]` — the smallest position ≥ p at which class `cls` is at
    // or over capacity, or `kNoHot` when there is none at or after p. One
    // backward sweep to build, O(1) to query, and it never materialises the
    // per-position live counts a range-max structure would need.
    static constexpr std::uint32_t kNoHot = UINT32_MAX;
    std::array<std::vector<std::uint32_t>, kLirRegClassCount> nextHot{};

    [[nodiscard]] bool spansHotRegion(LirRegClass cls,
                                      LirLiveRange const& r) const noexcept {
        auto const& v = nextHot[static_cast<std::size_t>(cls)];
        if (v.empty()) return false;
        auto const lo = (r.start < v.size())
                            ? r.start
                            : static_cast<std::uint32_t>(v.size() - 1u);
        return v[lo] < r.end;
    }
};

[[nodiscard]] PressureMap
buildPressureMap(LirFuncLiveness const& flow,
                 std::array<std::uint32_t, kLirRegClassCount> const& capacity) {
    PressureMap out;
    std::uint32_t const positions = flow.totalPositions + 1u;
    for (std::size_t c = 0; c < kLirRegClassCount; ++c) {
        if (capacity[c] == 0) continue;   // no allocatable register: no map
        // Difference array → live count per position.
        std::vector<std::int32_t> delta(positions + 1u, 0);
        bool any = false;
        for (auto const& rng : flow.ranges) {
            if (rng.vreg.id == 0) continue;
            if (static_cast<std::size_t>(rng.vreg.regClass()) != c) continue;
            if (rng.start >= positions) continue;
            any = true;
            ++delta[rng.start];
            --delta[(rng.end < positions) ? rng.end : positions];
        }
        if (!any) continue;
        std::vector<std::uint32_t> next(positions, PressureMap::kNoHot);
        std::int32_t live = 0;
        std::vector<bool> hot(positions, false);
        for (std::uint32_t p = 0; p < positions; ++p) {
            live += delta[p];
            hot[p] = (live >= static_cast<std::int32_t>(capacity[c]));
        }
        std::uint32_t seen = PressureMap::kNoHot;
        for (std::uint32_t p = positions; p-- > 0;) {
            if (hot[p]) seen = p;
            next[p] = seen;
        }
        out.nextHot[c] = std::move(next);
    }
    return out;
}

// Build the partition. `rangeOf` is the per-vreg-id live range (`.vreg.id == 0`
// marks a vreg with no range — one that liveness never saw). `callPositions`
// `pressure` is the per-class capacity map VETO 4 consults.
[[nodiscard]] CoalescePartition
buildCoalescePartition(CoalesceInput const&             in,
                       std::vector<LirLiveRange> const&  rangeOf,
                       PressureMap const&                pressure) {
    CoalescePartition p;
    std::uint32_t const n = static_cast<std::uint32_t>(rangeOf.size());
    p.parent.resize(n);
    p.hull = rangeOf;
    p.members.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        p.parent[i] = i;
        if (rangeOf[i].vreg.id != 0) p.members[i].push_back(i);
    }
    // Anti-affinity is checked against the CLASSES being merged, so it must be
    // asked as "does any forbidden pair straddle these two roots" — a pair
    // whose two ends have already been pulled into one class by an unrelated
    // chain would otherwise slip through. Kept as a flat list and re-resolved
    // through `find` on every query: the lists are short (one entry per untied
    // register operand of a 2-address instruction) and re-resolution is what
    // keeps the answer true as the partition evolves.
    auto const straddles = [&](std::uint32_t ra, std::uint32_t rb) {
        for (auto const& f : in.forbidden) {
            if (f.a >= n || f.b >= n) continue;
            auto const fa = p.find(f.a);
            auto const fb = p.find(f.b);
            if ((fa == ra && fb == rb) || (fa == rb && fb == ra)) return true;
        }
        return false;
    };
    for (auto const& e : in.edges) {
        if (e.dst == 0 || e.src == 0) continue;
        if (e.dst >= n || e.src >= n) continue;
        if (rangeOf[e.dst].vreg.id == 0 || rangeOf[e.src].vreg.id == 0) continue;
        auto const ra = p.find(e.dst);
        auto const rb = p.find(e.src);
        if (ra == rb) continue;
        // VETO 3 — class.
        if (p.hull[ra].vreg.regClass() != p.hull[rb].vreg.regClass()) continue;
        // VETO 1 — interference, on the hulls, through the ONE predicate.
        if (lirRangesInterfere(p.hull[ra], p.hull[rb])) continue;
        // ── VETO 2 — anti-affinity.
        //
        // ⚠⚠ ✔MEASURED 2026-08-26 (cycle P40 red-on-disable arm M3): DISABLING
        // THIS VETO CHANGES NOTHING ON THE CORPUS — the mutant built, the
        // subject `.obj` and `libdsscp.dll` md5s both differed from clean, and
        // every scoped test stayed GREEN. It is recorded here rather than
        // quietly enjoyed, because a guard whose mutant reddens nothing is the
        // shape `D-LIR-RETURN-REG-REFUSAL-IS-UNREACHABLE-FROM-THE-TEST-TIER`
        // names, and the honest response is to say WHY, not to claim an arm.
        //
        // ★ WHY IT CANNOT FIRE, AND THE ARGUMENT IS STRUCTURAL RATHER THAN
        // INCIDENTAL. A forbidden pair is always (result, UNTIED operand) of
        // ONE instruction, and the tied operand is the only thing an edge ever
        // connects the result to. So for a forbidden pair to straddle two
        // classes about to merge, the untied operand `b` must have been pulled
        // into the same class as the tied operand `a` by some chain. But `a`
        // and `b` are BOTH READ AT THAT INSTRUCTION'S EARLY SLOT, so each of
        // their ranges contains that position, so they INTERFERE — and veto 1,
        // asked on the class HULLS (each of which covers its members), refuses
        // any merge that would put them together. Veto 1 therefore implies
        // veto 2 for any liveness analysis in which a use is live at the
        // position it is used, which is every correct one, split intervals
        // (D-ML6-1.1) included.
        //
        // ⇒ IT STAYS, DELIBERATELY, AS DEFENCE IN DEPTH WITH ITS PROOF
        // ATTACHED. What it guards — legalize's `mov result, operands[tied]`
        // destroying an untied operand before the operation reads it — is
        // D-CSUBSET-BINOP-RIGHT-CLOBBER, a SILENT miscompile that cost this
        // project a cycle to find once already. Deleting a correctness veto on
        // the strength of an argument, when the argument's premise lives in a
        // different file (`lir_liveness.cpp`'s use recording) and the failure
        // mode is silence, is the trade this project does not take. The
        // measurement above is what a reader needs to know it is dormant; the
        // proof is what they need to know it is dormant for a REASON.
        if (straddles(ra, rb)) continue;
        // VETO 4 — pressure. See `PressureMap`'s docblock for the measurement
        // that produced this and for the hypothesis it replaced.
        LirLiveRange const mergedProbe = LirLiveRange::make(
            p.hull[ra].vreg,
            (p.hull[ra].start < p.hull[rb].start) ? p.hull[ra].start
                                                  : p.hull[rb].start,
            (p.hull[ra].end > p.hull[rb].end) ? p.hull[ra].end
                                              : p.hull[rb].end);
        if (pressure.spansHotRegion(p.hull[ra].vreg.regClass(), mergedProbe)) {
            continue;
        }
        // Merge. The surviving root is the LOWER vreg id so the representative
        // is a deterministic function of the class's membership rather than of
        // the order the edges happened to arrive in.
        auto const keep = (ra < rb) ? ra : rb;
        auto const drop = (ra < rb) ? rb : ra;
        p.parent[drop] = keep;
        auto const lo = (p.hull[keep].start < p.hull[drop].start)
                            ? p.hull[keep].start : p.hull[drop].start;
        auto const hi = (p.hull[keep].end > p.hull[drop].end)
                            ? p.hull[keep].end : p.hull[drop].end;
        p.hull[keep] = LirLiveRange::make(p.hull[keep].vreg, lo, hi);
        p.members[keep].insert(p.members[keep].end(),
                               p.members[drop].begin(), p.members[drop].end());
        p.members[drop].clear();
        ++p.unions;
    }
    return p;
}

// A spill slot in circulation, with the position past which it is reusable.
struct SlotInFlight {
    LirSpillSlot  slot{};
    LirRegClass   cls = LirRegClass::None;
    std::uint32_t end = 0;
};

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

    // ── WHEN THE FRAME POINTER LEAVES THE ALLOCATABLE POOL ──────────────────
    //
    // D-CSUBSET-VLA (C1b): a function that contains a `sub_sp_reg` op (a dynamic
    // VLA stack allocation) reserves the frame pointer as its fixed-frame base —
    // exclude it from the allocatable pool so it is never handed to a vreg. The
    // callconv pass force-saves it in the prologue + captures it as the base.
    //
    // D-CODEGEN-APPLE-ARM64-X29-USED-AS-GENERAL-SCRATCH-AGAINST-ITS-RESERVED-ROLE:
    // and SOME platform ABIs reserve the register UNCONDITIONALLY, whether or not
    // this function needs a frame base. That is a fact about the CONVENTION, so
    // it arrives as one — `cc->framePointerReservation`, declared in the
    // `.target.json` row — and the switch below is over that declared verb.
    // ⚠ THE ALTERNATIVE WOULD HAVE BEEN AN `if (format == MachO)` HERE, which is
    // the agnosticism break this tier forbids outright; the register-role fact
    // belongs beside the register, in the document that names it.
    // Absent/`dynamic-frame-only` ⇒ the VLA-only behavior above, unchanged, so
    // every convention that declares nothing keeps byte-identical frames.
    std::optional<std::uint16_t> reservedFramePointer;
    if (cc->framePointer.has_value()) {
        bool reserve = false;
        switch (cc->framePointerReservation) {
            case FramePointerReservation::Always:
                reserve = true;
                break;
            case FramePointerReservation::DynamicFrameOnly:
                if (auto const subSpReg = schema.opcodeByMnemonic("sub_sp_reg");
                    subSpReg.has_value()
                    && functionContainsOpcode(lir, flow.fn, *subSpReg)) {
                    reserve = true;
                }
                break;
        }
        if (reserve) reservedFramePointer = cc->framePointer->ordinal;
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

    // ── OPT8: partition the vregs into COPY-RELATED CLASSES before the scan.
    // `rangeOf[id]` is the per-id view of `flow.ranges` the partition needs;
    // a `vreg.id == 0` entry means liveness never saw that id.
    std::vector<LirLiveRange> rangeOf(maxVRegId + 1u, LirLiveRange{});
    for (auto const& r : flow.ranges) {
        if (r.vreg.id == 0 || r.vreg.id > maxVRegId) continue;
        rangeOf[r.vreg.id] = r;
    }
    MoveOpcodeCache moves{schema};
    // Capacity per class, taken BEFORE the scan consumes the free lists: the
    // number of registers this calling convention actually makes allocatable
    // for that class, which is the only honest K to compare a live count
    // against. Reserved registers and the VLA frame pointer are already out.
    std::array<std::uint32_t, kLirRegClassCount> classCapacity{};
    for (std::size_t c = 0; c < kLirRegClassCount; ++c) {
        classCapacity[c] = static_cast<std::uint32_t>(
            free[c].calleeSaved.size() + free[c].callerSaved.size());
    }
    CoalesceInput const coalesceIn =
        collectCoalesceInput(lir, schema, *cc, flow, moves, reporter);
    CoalescePartition part = buildCoalescePartition(
        coalesceIn, rangeOf, buildPressureMap(flow, classCapacity));
    out.coalescedCopies = part.unions;

    // ── PRE-COLORING HINTS, one per vreg. Two sources, and the DEF-SIDE ABI
    // hint wins: an incoming parameter's home is fixed by the calling
    // convention at function entry, while a physical-register copy inside the
    // body is a lowering's local pin. Both are PREFERENCES — see
    // `tryAllocatePreferred` for why neither can widen what a range may take.
    std::vector<std::optional<std::uint16_t>> hintOf(maxVRegId + 1u);
    for (auto const& h : coalesceIn.physHints) {
        if (h.vregId == 0 || h.vregId > maxVRegId) continue;
        if (!hintOf[h.vregId].has_value()) hintOf[h.vregId] = h.ordinal;
    }
    for (auto const& ao : argOccupied) {
        if (ao.paramVregId == 0 || ao.paramVregId > maxVRegId) continue;
        hintOf[ao.paramVregId] = ao.ordinal;
    }

    // The scan's subjects: one entry per CLASS, in hull-start order. A
    // singleton class is byte-identical to the range it wraps, so a function
    // with no coalescable copy walks exactly the sequence it walked before.
    //
    // ⚠ THE SORT KEY MUST STAY (hull.start, representative id) — the same key
    // `analyzeFuncLiveness` sorts `ranges` by. A class's hull start is the
    // MINIMUM of its members' starts, so this ordering still visits every
    // operand's class before the class of any instruction that reads it, which
    // is what keeps `tryAllocateExcluding`'s operand-ordinal exclusion able to
    // SEE the operand's assignment.
    std::vector<std::uint32_t> classRoots;
    classRoots.reserve(flow.ranges.size());
    for (std::uint32_t id = 1; id <= maxVRegId; ++id) {
        if (rangeOf[id].vreg.id == 0) continue;
        if (part.find(id) != id) continue;
        classRoots.push_back(id);
    }
    std::sort(classRoots.begin(), classRoots.end(),
              [&](std::uint32_t a, std::uint32_t b) {
                  return std::tie(part.hull[a].start, a)
                       < std::tie(part.hull[b].start, b);
              });

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

    // Slots in circulation, for spill-slot coalescing. Expired against the
    // same monotonic scan position the register free lists are.
    std::vector<SlotInFlight> slotsInFlight;

    for (std::uint32_t const classRoot : classRoots) {
        // THE SUBJECT OF THE SCAN IS THE CLASS HULL. For a singleton class it
        // IS the vreg's own range, byte-for-byte, which is why a function with
        // no coalescable copy allocates exactly as it did before OPT8.
        LirLiveRange const& r = part.hull[classRoot];
        std::vector<std::uint32_t> const& classMembers = part.members[classRoot];
        if (r.vreg.id == 0) continue;
        LirRegClass const cls = r.vreg.regClass();
        if (cls == LirRegClass::None) {
            for (std::uint32_t const mid : classMembers) {
                report(reporter, DiagnosticCode::R_VRegHasNoClass,
                       DiagnosticSeverity::Error,
                       std::format("func {} vreg id {} has LirRegClass::None — "
                                   "run LirVerifier before allocator",
                                   flow.fn.v, mid));
            }
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
        // ★★ OPT8: THE EXCLUSION SET OF A COALESCED CLASS IS THE **UNION** OVER
        // ITS MEMBERS, AND ANYTHING LESS IS A SILENT MISCOMPILE. Every rule
        // below is keyed on a range's own DEFINING INSTRUCTION or on its own
        // start position, so a class holding N vregs has N defining
        // instructions and N start positions to answer for. Computing the set
        // from the representative alone would honour one member's constraints
        // and quietly drop the other N-1 — and the dropped ones are exactly the
        // `requires2Address` operand exclusions whose whole job is to stop
        // `add r, [r, r]`. A singleton class walks this loop once, over its own
        // range, which is what it did before OPT8.
        for (std::uint32_t const memberId : classMembers) {
            LirLiveRange const& mr = rangeOf[memberId];
            if (mr.vreg.id == 0) continue;
        if (LirInstId const producingInst =
                (mr.start < flow.positionToInst.size())
                    ? flow.positionToInst[mr.start]
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
            if (info != nullptr && info->requires2Address.has_value()
                && lir.instResult(producingInst) == mr.vreg) {
                auto const ops = lir.instOperands(producingInst);
                // The COALESCE TARGET is the operand the schema ties
                // the result to, not a literal 0
                // (D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE).
                //
                // ⚠⚠ SKIPPING THE WRONG ONE IS A SILENT MISCOMPILE, NOT
                // A MISSED OPTIMIZATION, and the direction is worth
                // spelling out because it is not symmetric. Legalize
                // runs AFTER this pass and inserts `mov result,
                // operands[tied]` BEFORE the op — so `result` sharing a
                // register with any UNTIED operand means that operand's
                // value is destroyed by the copy before the op ever
                // reads it. Excluding the tied operand instead would
                // only cost a redundant move; failing to exclude an
                // untied one costs correctness. Hence: skip exactly the
                // index the schema names.
                std::size_t const tied = *info->requires2Address;
                // The 2026-06-02 HIGH-2 fixed-buffer overflow
                // pre-check (`ops.size() > excludedStorage.size()
                // + 1`) is gone with the growable scratch
                // (D-OPT-REGALLOC-EXCLUSION-BUFFER closure): an
                // N-ary requires2Address op of ANY arity now has
                // every operand[1..N] ordinal excluded — push_back
                // cannot truncate, so the fail-loud arm's job
                // (never silently drop an exclusion) is satisfied
                // by construction.
                // Skip the tied operand (legitimate coalesce target).
                for (std::size_t k = 0; k < ops.size(); ++k) {
                    if (k == tied) continue;
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
                //
                // ★ CHOKEPOINT (2026-08-15): reads BOTH constraint
                // carriers via `effectiveForbiddenOrdinals`. The
                // reasoning above is carrier-blind — the 2-addr
                // legalize's `mov result, ops[0]` runs before the op
                // reads its implicit registers whether the target
                // JSON declared them or an inline-asm statement did.
                // The dedup'd append remains total for any declared
                // union size (D-OPT-REGALLOC-EXCLUSION-BUFFER
                // closure): the scratch grows; nothing truncates.
                appendEffectiveForbiddenOrdinals(lir, schema,
                                                 producingInst,
                                                 excludedScratch);
            }
        }
        }  // end per-member exclusion walk
        // Augment exclusion with implicit-register clobbers from any
        // opcode this range crosses (cycle 10q substrate consumer).
        // Universal across CPUs — driven entirely by the per-opcode
        // schema declaration; no `if (opcode == idiv)` branch.
        //
        // ⓘ OPT8: asked over the CLASS HULL rather than per member. The hull is
        // the interval the register is actually held over, and it is a
        // SUPERSET of every member's, so this can only exclude MORE — the
        // conservative direction. It also needs no argument about whether a
        // merged hull can contain a gap: a clobber sitting in one would be
        // excluded anyway.
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
            auto const classHoldsVreg = [&](std::uint32_t id) {
                for (std::uint32_t const m : classMembers) {
                    if (m == id) return true;
                }
                return false;
            };
            for (auto const& ic : indirectCallees) {
                // OPT8: "is THIS range the callee vreg" becomes "does this
                // CLASS hold it" — the class is what receives the register, so
                // it is the class that must be kept off the arg set.
                if (!classHoldsVreg(ic.calleeVregId)) continue;
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
                // OPT8: per MEMBER, and the "its own home" exemption stays per
                // member too. A class that holds the param AND some other vreg
                // live before `releasePos` must still avoid the arg register:
                // the incoming value sits there from function entry, which is
                // BEFORE the param's own range even starts, so merging the
                // param into a class does not make that window safe.
                for (std::uint32_t const memberId : classMembers) {
                    if (memberId == ao.paramVregId) continue;  // its own home
                    if (rangeOf[memberId].start < ao.releasePos) {
                        addExcludedArg(ao.ordinal);
                    }
                }
            }
        }

        std::span<std::uint16_t const> const excluded{
            excludedScratch.data(), excludedScratch.size()};

        // The class's pre-coloring hint (see `tryAllocatePreferred`). Taken
        // from the LOWEST member id that carries one, so the choice is a
        // function of the class's MEMBERSHIP rather than of iteration order —
        // two members with different hints is a real shape (a parameter
        // copy-related to a physically-pinned temp) and the tie-break is what
        // keeps the allocation reproducible when it happens.
        std::optional<std::uint16_t> preferredOrdinal;
        {
            std::uint32_t bestMember = 0;
            for (std::uint32_t const memberId : classMembers) {
                if (memberId == 0 || memberId >= hintOf.size()) continue;
                if (!hintOf[memberId].has_value()) continue;
                if (bestMember == 0 || memberId < bestMember) {
                    bestMember = memberId;
                    preferredOrdinal = hintOf[memberId];
                }
            }
        }

        // Write one decision onto EVERY member of the class — that identity is
        // the whole mechanism: `rewriteWithAllocation` is a per-vreg lookup, so
        // members sharing an assignment turn the copy that related them into a
        // register-to-register move onto itself, which `lir_peephole` R1 then
        // deletes.
        auto const assignClassPhys = [&](std::uint16_t ordinal) {
            LirReg const phys = makePhysicalReg(ordinal, cls);
            for (std::uint32_t const memberId : classMembers) {
                LirLiveRange const& mr = rangeOf[memberId];
                if (mr.vreg.id == 0) continue;
                out.assignments[memberId] =
                    LirRegAssignment::makePhys(mr.vreg, phys);
            }
        };
        auto const assignClassSpill = [&](std::vector<std::uint32_t> const& mem,
                                          LirSpillSlot slot) {
            for (std::uint32_t const memberId : mem) {
                LirLiveRange const& mr = rangeOf[memberId];
                if (mr.vreg.id == 0) continue;
                out.assignments[memberId] =
                    LirRegAssignment::makeSpill(mr.vreg, slot);
            }
        };
        // ── SPILL-SLOT COALESCING. A slot whose occupant's hull has ENDED is
        // reusable, by the same `end <= start` test `expireActive` applies to
        // registers — i.e. by the same `!lirRangesInterfere` fact. Restricted
        // to one REGISTER CLASS: the frame's slot stride is a per-function
        // maximum over the classes present, so sharing across classes would be
        // sound only by an argument about that stride, and there is nothing to
        // buy by making it.
        auto const acquireSlot = [&](LirRegClass slotCls,
                                     LirLiveRange const& hull) -> LirSpillSlot {
            for (auto& s : slotsInFlight) {
                if (s.cls != slotCls) continue;
                if (s.end > hull.start) continue;   // still live — interferes
                s.end = hull.end;
                ++out.coalescedSpillSlots;
                return s.slot;
            }
            LirSpillSlot const fresh = mintSlot();
            slotsInFlight.push_back({fresh, slotCls, hull.end});
            return fresh;
        };

        auto pick = tryAllocatePreferred(free, cls, crossesCall, excluded,
                                         preferredOrdinal);
        if (!pick.has_value()) {
            pick = tryAllocateExcluding(free, cls, crossesCall, excluded);
        }
        if (pick.has_value()) {
            assignClassPhys(pick->ordinal);
            active.push_back({r, cls, pick->ordinal, pick->isCalleeSaved,
                              classRoot});
            continue;
        }

        // Invariant: every spill takes exactly one slot via `acquireSlot`
        // (fresh or reused) and contributes to one `SpillStats` counter.
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
            // Spill this class itself.
            assignClassSpill(classMembers, acquireSlot(cls, r));
            if (crossesCall) ++spills.crossCallExhaustion;
            else             ++spills.pressure;
            continue;
        }

        // Evict spillIt: its whole CLASS goes to a spill slot; this class gets
        // its physical register. The evicted range's spill cause is its
        // OWN crossesCall status, not this one's — they may differ.
        // ⚠ The evictee is still LIVE (that is why it was chosen: its end is
        // later), so `acquireSlot` records its hull end and the slot stays out
        // of circulation for exactly as long as the value does.
        assignClassSpill(part.members[spillIt->classRoot],
                         acquireSlot(spillIt->cls, spillIt->range));
        bool const evictedCrossesCall =
            rangeCrossesCall(spillIt->range, callPositions);
        if (evictedCrossesCall) ++spills.crossCallExhaustion;
        else                    ++spills.pressure;

        std::uint16_t const freedOrdinal = spillIt->physOrdinal;
        bool const freedIsCalleeSaved    = spillIt->isCalleeSaved;
        active.erase(spillIt);

        assignClassPhys(freedOrdinal);
        active.push_back({r, cls, freedOrdinal, freedIsCalleeSaved, classRoot});
    }

    emitSpillSummary(reporter, flow.fn, spills);

    // ── THE FAIL-LOUD SELF-CHECK, AND IT RUNS UNCONDITIONALLY.
    //
    // ★ It is `regallocFatal` rather than a diagnostic ON PURPOSE, and the
    // choice follows this file's own stated split: a `DiagnosticReporter` code
    // is for a DATA-DRIVEN failure the user can act on (a schema with no
    // calling convention, a vreg with no class); this is a PRODUCER-SIDE
    // INVARIANT — the allocator contradicting itself. There is no user input
    // that can reach it and no source edit that can avoid it, so a
    // recoverable, suppressible diagnostic would be the wrong shape: the only
    // correct response is to stop before a wrong artifact exists. It joins
    // `makePhys`'s class-mismatch abort, three lines up the same wall.
    //
    // ⚠ It is NOT a debug assertion. A miscompile that only fails to be caught
    // in release builds is a miscompile that ships.
    if (auto const conflict = findAllocationConflict(flow, out);
        conflict.has_value()) {
        std::fprintf(stderr,
                     "dss::LirRegAlloc: allocation conflict in func %u — "
                     "vreg %u and vreg %u have overlapping live ranges but "
                     "were both assigned %s %u\n",
                     flow.fn.v,
                     static_cast<unsigned>(conflict->a.id),
                     static_cast<unsigned>(conflict->b.id),
                     conflict->isSpillSlot ? "spill slot" : "physical register",
                     static_cast<unsigned>(conflict->sharedResource));
        regallocFatal("interfering live ranges share one resource — the "
                      "coalescer or the linear scan is unsound; refusing to "
                      "emit a silently wrong artifact");
    }

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
