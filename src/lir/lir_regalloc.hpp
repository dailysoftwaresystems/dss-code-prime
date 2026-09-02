#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

// Linear-scan register allocator over the `LirLiveness` substrate.
// Substrate-tier; produces a per-function assignment side-table
// mapping virtual registers to physical registers OR spill slots.
// A separate rewrite pass consumes the side-table to replace vregs
// with phys regs and insert spill loads/reloads — that pass also
// fires the `LirVerifier`'s "no virtual registers after regalloc"
// rule.
//
// **Algorithm**: classical linear-scan (Poletto-Sarkar 1999). When
// no register is free for the current range, pick the same-class
// active range with the latest `end`; if that end is later than the
// current range's end, evict it and reuse its register; otherwise
// spill the current range.
//
// **Call-aware constraint**: a range that survives past the late
// slot of a call instruction MUST NOT be assigned to a caller-saved
// physical register (per the active `TargetCallingConvention`), so a
// cross-call range takes a callee-saved register or spills. Call
// detection is target-agnostic via `TargetOpcodeInfo::isCall`.
//
// **Partition preference** (plan 22 OPT8): a range that does NOT cross a
// call is offered the CALLER-SAVED partition first and falls back to
// callee-saved. Both were always legal for such a range; the order is what
// changed, and it changed because a callee-saved register is not free —
// using one obliges the function to save and restore it (✔MEASURED 3352
// prologue saves, 5.1% of the emitted `examples/c/**` instruction stream,
// before the change) and spends a register that the cross-call ranges are
// the only legitimate consumers of. See `tryAllocate`'s docblock in the
// .cpp for the full argument and why the ABI envelope does not move.
//
// ── REGISTER COALESCING (plan 22 OPT8) ──────────────────────────────
//
// ★★★ THE ALLOCATOR ALLOCATES **CLASSES OF COPY-RELATED VREGS**, NOT
// INDIVIDUAL VREGS. Before allocation it partitions the function's
// virtual registers with a union-find whose edges are COPIES, merging
// two classes only when their live ranges do not interfere
// (`lirRangesInterfere` — the ONE predicate, see `lir_liveness.hpp`).
// Every member of a class receives the SAME physical register, and
// `rewriteWithAllocation` then does not emit the copy that related them
// at all. The copy is removed **at its source** — the allocator stops
// creating the difference — rather than pattern-matched afterwards.
// (`lir_peephole`'s rule R1 still deletes the identity copies the scan
// produced by COINCIDENCE, which carry no such proof; see
// `coalescedCopyInsts` for why the two are different questions.)
//
// **Two kinds of edge, both target VOCABULARY, neither a mnemonic:**
//
//   * an EXPLICIT copy — an instruction whose opcode IS the register
//     class's declared move (`regClassOpOpcode(cls, RegClassOp::Move)`)
//     with one virtual-register operand and a virtual-register result of
//     the same class, AT LEAST AS WIDE AS EVERY STATED ACCESS TO EITHER
//     END. The opcode-identity half is R1's, asked one tier earlier:
//     `trunc`/`zext` print as `mov` and are NOT copies.
//     ★★ THE WIDTH HALF IS **NOT** R1's AND USED TO BE
//     (D-LIR-COPY-COALESCING-ASKS-THE-REGISTERS-WIDTH-NOT-THE-VALUES).
//     R1 asks whether the copy writes the whole REGISTER, because
//     post-regalloc that is all it can ask. Here the ends are still
//     virtual, so the honest question is whether the copy writes the
//     whole VALUE — and asking R1's question one tier early vetoed the
//     entire floating-point file on both shipped targets, forever: a
//     class move is emitted with the width-default flags (64) and `xmm`
//     and `v` are 16 bytes, so the equality could never hold.
//   * an IMPLICIT copy — the (result, tied-operand) pair of an opcode
//     declaring `requires2Address`. `legalizeTwoAddress` materializes
//     `mov result, operands[tied]` iff the two differ AFTER allocation,
//     so making them agree means the copy is never minted at all.
//     ⓘ ✔MEASURED: nothing in the pipeline previously SOUGHT that
//     agreement — the allocator merely stopped short of forbidding it
//     (`tryAllocateExcluding` skips the tied index), so the tied
//     "coalesce" happened only when the free list's LIFO order
//     coincidentally produced it.
//
// **The three vetoes, and each closes a specific miscompile:**
//
//   1. INTERFERENCE — two ranges live at the same position may never
//      share a register. Tested on the CLASS HULL, which is what the
//      linear scan will actually hold, so the transform and the
//      allocator agree by construction rather than by argument.
//   2. ANTI-AFFINITY — the result of a `requires2Address` instruction
//      may never join the class of an UNTIED operand of that same
//      instruction, even when their ranges are disjoint. Legalize's
//      `mov result, operands[tied]` runs BEFORE the operation and would
//      destroy that operand's value first: `add r, [r, r]` instead of
//      `add r, [r, x]` (D-CSUBSET-BINOP-RIGHT-CLOBBER, the same hazard
//      `tryAllocateExcluding` guards for unmerged vregs — merging is a
//      route around that exclusion, so it needs its own veto).
//   3. CLASS — a merge never crosses `LirRegClass`.
//
// **Pressure is provably NOT increased.** A coalescable copy's source
// ends exactly where its destination begins (`s.end == d.start`: the
// copy is a USE of the source, so the source's range reaches the copy;
// if it reached PAST it the two would interfere and veto 1 fires), so a
// merged hull is the two ranges glued at a point with NO hole. A class
// therefore holds its register over exactly the union of the intervals
// that needed one. This is why coalescing here needs no spill-cost
// model to decide anything, and why OPT23 is not a prerequisite.
//
// **Spill-slot coalescing** rides the same predicate: a spilled class
// reuses a stack slot whose previous occupant's hull has ended, within
// the same register class. `numSpillSlots` is now the number of
// DISTINCT slots (the frame's spill area size), not the number of
// spilled values.
//
// **Reserved registers**: registers that appear in no calling-
// convention saved/arg/return set (e.g. `rsp`, `rflags`) are filtered
// out of the allocator's pool. Allocating `rsp` would clobber the
// stack pointer mid-function — fatal at runtime.
//
// **Diagnostics**: the allocator threads a `DiagnosticReporter&` and
// emits `R_*` codes for schema-config errors (no calling conventions
// declared, missing class on a vreg) plus one `R_SpillSummary` Info
// note per function with non-zero spills. Per-spill granularity is
// captured in `LirFuncAllocation::numSpillSlots` — the reporter is
// not used as a streaming spill log (the per-code cap = 50 in the
// reporter would silently drop notes past the 50th).
//
// **Target-blind**: depends only on `TargetSchema.registers()` +
// `callingConventions()` + per-opcode `isCall` flag. No mnemonic
// checks, no per-target if-arms.
//
// **Source-language-blind**: consumes only `Lir` + `LirLiveness`.

namespace dss {

// Per-vreg assignment. The `assignment` payload is a tagged union:
//   * `std::get<LirReg>(assignment)` — assigned a physical register
//   * `std::get<LirSpillSlot>(assignment)` — assigned a stack slot
// Use `isSpilled()` to discriminate, then `physReg()` or `spillSlot()`
// to read the active arm. Default-constructed slots (`vreg.id == 0`)
// are the unfilled sentinel — used to pre-size the per-vreg vector
// and recognized by `LirFuncAllocation::forVReg` via `vreg.id == 0`
// alone (the assignment payload is irrelevant for the sentinel check).
// The variant's default arm is `LirReg{}`; a downstream consumer must
// NOT use `isSpilled() == false` as a "has-real-assignment" probe.
//
// Preconditions (process aborts on violation via the factories):
//   makePhys:    vreg.isPhysical == 0; phys.isPhysical == 1;
//                vreg.regClass() == phys.regClass()
//   makeSpill:   vreg.isPhysical == 0; slot.valid() (i.e. slot.v != 0)
//
// Accessor preconditions (`noexcept` — bad-arm access aborts via
// std::bad_variant_access propagating through noexcept → terminate):
//   physReg():   !isSpilled()
//   spillSlot(): isSpilled()
struct DSS_EXPORT LirRegAssignment {
    LirReg                              vreg{};       // input virtual register
    std::variant<LirReg, LirSpillSlot>  assignment{}; // phys reg OR spill slot

    [[nodiscard]] bool isSpilled() const noexcept {
        return std::holds_alternative<LirSpillSlot>(assignment);
    }
    [[nodiscard]] LirReg physReg() const noexcept {
        return std::get<LirReg>(assignment);
    }
    [[nodiscard]] LirSpillSlot spillSlot() const noexcept {
        return std::get<LirSpillSlot>(assignment);
    }

    [[nodiscard]] static LirRegAssignment makePhys(LirReg vreg, LirReg phys);
    [[nodiscard]] static LirRegAssignment makeSpill(LirReg vreg, LirSpillSlot slot);
};

// Per-function allocation result. `assignments[i]` is the assignment
// for vreg id `i` (slot 0 is the default-constructed sentinel — every
// well-formed vreg has id ≥ 1; the substrate guarantees dense vreg
// ids from 1 so the vector indexed by id has no holes beyond the
// sentinel). `numSpillSlots` is the total stack slot count (one slot
// per spilled vreg — coalescing not yet implemented).
//
// `ok` is set by `allocateFuncRegisters` from the per-function delta
// in `reporter.errorCount()` (false iff this function emitted any
// error-severity diagnostic). Schema-config errors short-circuit
// allocation: the result carries empty `assignments` and `ok = false`.
//
// ⚠ THE "one slot per spilled vreg — coalescing not yet implemented"
// THAT USED TO STAND ON `numSpillSlots` IS GONE BECAUSE THE THING IT
// DESCRIBED IS BUILT (plan 22 OPT8). Both coalescers ship: copy-related
// vregs share a REGISTER, and non-interfering spilled classes share a
// SLOT. `numSpillSlots` counts DISTINCT slots.
struct DSS_EXPORT LirFuncAllocation {
    LirFuncId                     fn{};
    // Stamp of the source function's `symbol` field at allocation
    // time. Downstream passes (ML6 cycle 3b rewrite, ML7 callconv)
    // produce fresh `Lir` modules with new arena tags, so
    // `forFunc(LirFuncId)` lookups can't survive the pipeline.
    // Symbol-id is stable across passes (the rewrite preserves it),
    // so a per-function `originalSymbol` cross-check structurally
    // detects reorder/drop drift in the parallel-index contract.
    SymbolId                      originalSymbol{};
    std::vector<LirRegAssignment> assignments;
    std::uint32_t                 numSpillSlots = 0;
    bool                          ok            = true;
    // ── OPT8 coalescing counters. Read by the pass's differential tests
    // and by nothing else — they are OBSERVATIONS, never inputs to a
    // decision, so a counter that drifts cannot change what is emitted.
    // `coalescedCopies` counts UNION operations performed (each one is a
    // copy the pipeline will not have to emit or delete);
    // `coalescedSpillSlots` counts spilled classes that REUSED an
    // existing slot instead of minting one (each is `slotSize` bytes of
    // frame the function does not reserve).
    std::uint32_t                 coalescedCopies     = 0;
    std::uint32_t                 coalescedSpillSlots = 0;

    // ── THE WIDTH-SAFE COPIES, AND THIS ONE **IS** AN INPUT TO A DECISION ──
    //
    // ★★★ D-LIR-COPY-COALESCING-ASKS-THE-REGISTERS-WIDTH-NOT-THE-VALUES. Source
    // `LirInstId.v`s, ASCENDING, of every explicit class-move copy in this
    // function whose two ends are virtual registers of one class AND whose
    // width covers every stated access to either end. `rewriteWithAllocation`
    // consults it: a copy in this list whose two ends the allocation put in the
    // SAME physical register is not emitted at all.
    //
    // ⚠ IT IS THE **PROOF**, NOT THE OUTCOME, AND THE TWO ARE DELIBERATELY
    // SEPARATE. Membership says only "deleting this copy cannot be observed",
    // which is a property of the LIR; whether the two ends actually coincide is
    // a property of the ALLOCATION, re-checked at the rewrite on the mapped
    // registers. A copy the coalescer proved safe but whose union a veto
    // refused is still deletable if the scan happened to land both ends on one
    // register — and a copy that landed on one register by coincidence with NO
    // proof is NOT, because a narrower-than-the-datum copy that survives is
    // load-bearing (AArch64's `FMOV Dd,Dn` zeroes bits 127:64, so dropping it
    // where something reads 128 bits changes the value). Carrying the proof
    // rather than re-deriving it downstream is what keeps the rewrite from
    // owning a second copy of the width question.
    std::vector<std::uint32_t>    coalescedCopyInsts;
    // Cached index of the calling convention used to drive this
    // allocation. The downstream prologue/epilogue emitter reads this
    // so it doesn't re-derive the cc choice. Hazard: reordering
    // calling-convention entries in a target JSON file will change
    // every function's allocation here unless callers also re-pin by
    // name.
    std::uint16_t                 callingConventionIndex = 0;

    // D-CSUBSET-VLA (C1b): the register-table ordinal of the frame pointer this
    // function RESERVES as its fixed-frame base (set iff the function contains a VLA
    // `sub_sp_reg`). Held out of the allocatable pool (`buildFreeLists`) AND the
    // rewriter's spill-reload SCRATCH pool (`pickScratchRegs`) — both consult the
    // cc's allocatable set, which still lists rbp/x29 (they are ordinary callee-
    // saved GPRs for a NON-VLA function). Without excluding it from the scratch pool
    // too, the rewriter would harvest the reserved-but-unassigned frame pointer as a
    // spill scratch and clobber the frame base. std::nullopt for a non-VLA function.
    std::optional<std::uint16_t>  reservedFramePointer;

    // Find the assignment for the given vreg id, or nullptr if none.
    [[nodiscard]] LirRegAssignment const*
    forVReg(std::uint32_t vregId) const noexcept;
};

// ── THE INDEPENDENT COALESCING AUDITOR (plan 22 OPT8) ───────────────
//
// ★★★ A COALESCER THAT MERGES TWO INTERFERING LIVE RANGES PUTS TWO LIVE
// VALUES IN ONE REGISTER. Nothing downstream can notice: the LIR is
// well-formed, the verifier's post-regalloc rules pass (no virtual
// registers, valid spill slots), the assembler encodes clean bytes, and
// the program computes the wrong answer. It is the worst failure class
// this project recognizes, so the check for it must not be able to
// inherit the transform's own belief.
//
// `findAllocationConflict` re-derives the question from ONLY the
// liveness side-table and the finished assignment table. It never sees
// the union-find, the copy edges, or the anti-affinity list — it walks
// the per-vreg assignments, groups them by the physical ordinal (or the
// spill slot) they landed on, and asks `lirRangesInterfere` about every
// pair inside a group. Two vregs sharing a register with overlapping
// ranges is a conflict WHATEVER decided it, which is why this also
// guards the pre-OPT8 linear scan and the eviction path, not merely the
// merges.
//
// ⓘ It reports the FIRST conflict it finds and stops — the caller's
// response is to abort the compile, so an exhaustive list buys nothing
// and the scan is quadratic in the group size.
//
// Returns `std::nullopt` when the allocation is conflict-free.
struct DSS_EXPORT LirAllocationConflict {
    LirReg        a{};                 // the two virtual registers that
    LirReg        b{};                 // were handed the same resource
    std::uint32_t sharedResource = 0;  // phys ordinal, or spill-slot v
    bool          isSpillSlot    = false;
};

[[nodiscard]] DSS_EXPORT std::optional<LirAllocationConflict>
findAllocationConflict(LirFuncLiveness const&   flow,
                       LirFuncAllocation const& alloc) noexcept;

// Module-level wrapper. Per-function entries in the same order as
// `lir.funcAt(i)`. `ok()` is a derived property — true iff every
// per-function allocation succeeded. Computed on access rather than
// stored so the value cannot drift from the per-function results.
struct DSS_EXPORT LirAllocation {
    std::vector<LirFuncAllocation> perFunc;

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] LirFuncAllocation const* forFunc(LirFuncId fn) const noexcept;
};

// Allocate physical registers for every function in `lir`. Reads
// `schema.registers()` + `schema.callingConventions()` and the
// liveness side-tables. Schema-wide validity (≥1 calling convention
// declared) is checked ONCE at the top — failure short-circuits the
// per-function loop, and every `LirFuncAllocation` carries the
// schema-level error visible via its own `ok` flag.
//
// Producer-side errors (None-class vreg slipping past LirVerifier)
// are emitted via the reporter as `R_*` Error diagnostics; each
// affected function's `ok` flips to false. Spill decisions emit one
// `R_SpillSummary` Info note per function with non-zero spills —
// granular per-vreg spill data lives in `numSpillSlots` rather than
// in the reporter stream (which has a per-code cap).
//
// The LIR module is NOT mutated.
// `callingConventionIndex` is the ordinal into
// `schema.callingConventions()` that the per-target ABI resolver
// (FF3 `resolveAbi`) picked for the (target, format) pair driving
// this compile. The allocator records it on every produced
// `LirFuncAllocation`; `materializeCallingConvention` reads it back
// to look up the structured cc and emit the right prologue/epilogue.
// Pre-D-FF3-3 every function was hardcoded to index 0 — silent
// miscompile on non-default-cc targets (e.g. PE64 + x86_64 silently
// dispatched to sysv_amd64 instead of ms_x64).
[[nodiscard]] DSS_EXPORT LirAllocation
allocateRegisters(Lir const&          lir,
                  TargetSchema const& schema,
                  LirLiveness const&  liveness,
                  std::uint16_t       callingConventionIndex,
                  DiagnosticReporter& reporter);

// Allocate for a single function. The caller must supply the matching
// `LirFuncLiveness` produced over the same `lir` AND the per-(target,
// format) calling-convention ordinal it would normally receive from
// `dss::ffi::resolveAbi(...)`. The schema-wide validity check
// (≥1 calling convention) is repeated here for callers that bypass
// `allocateRegisters`.
//
// Post-fold-#5 code-reviewer-#82 fold: the parameter is REQUIRED (no
// default) so a future caller cannot accidentally inherit the
// pre-D-FF3-3 `0` hardcode silently. Test callers pass `0` explicitly
// when the test fixture's target ships a single cc (cc[0] is then
// the only valid choice).
[[nodiscard]] DSS_EXPORT LirFuncAllocation
allocateFuncRegisters(Lir const&             lir,
                      TargetSchema const&    schema,
                      LirFuncLiveness const& flow,
                      std::uint16_t          callingConventionIndex,
                      DiagnosticReporter&    reporter);

// ── THE VALUE-WIDTH BOUND THE COPY CLAUSE IS DECIDED AGAINST ────────────────
//
// D-LIR-COPY-COALESCING-ASKS-THE-REGISTERS-WIDTH-NOT-THE-VALUES. The widest
// operation width, in bits, that any instruction of `flow.fn` STATES while
// naming each virtual register — as its result or as a register operand.
// Indexed by vreg id; entry 0 is the sentinel and is always 0. A vreg liveness
// never saw is out of range, which a caller reads as "cannot bound it".
//
// ★ PUBLISHED SO A PIN CAN ASSERT THE BOUND DIRECTLY. The coalescer's decision
// is "the copy is at least this wide"; inferring that from an allocation
// outcome would make every pin depend on which register the linear scan
// happened to pick, and a pin that can pass because two vregs coincidentally
// missed each other is measuring the scan, not the rule.
//
// ⚠ ONE WIDTH PER INSTRUCTION, SO A MEMORY OP'S BASE REGISTER IS CREDITED WITH
// THE WIDTH OF THE DATA IT MOVES. That over-states the access to an address
// register, which REFUSES merges and never admits one — the safe direction, and
// the reason this is a bound rather than a measurement.
[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
lirMaxStatedAccessWidthBits(Lir const& lir, LirFuncLiveness const& flow);

} // namespace dss
