#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "lir/lir.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// `LirLiveness` (plan 12 §2.8) — per-function liveness analysis over the
// frozen LIR module. Substrate-tier; consumed by the linear-scan
// allocator (§2.8 cycle 2) and by post-regalloc spill scheduling.
//
// Target-blind: the analysis depends only on the LIR module's CFG +
// per-instruction operand shape (Reg-kind operand → use; result vreg
// → def — and on an inline-asm bundle, each operand's ROLE, read through
// `lirForEachInstUse` / `lirForEachInstDef`). It never inspects opcode
// semantics, mnemonics, or
// `TargetSchema`. Source-language-blind: input is LIR; the analysis
// never touches MIR/HIR types.
//
// Position numbering: blocks are visited in reverse post-order (RPO);
// each instruction gets TWO position slots (early/late half-steps) so
// uses (read at the early half) and defs (written at the late half)
// at the same instruction don't collide. Position `p` for the N-th
// instruction in RPO order: `early = 2*N`, `late = 2*N + 1`.
//
// ★ THE ONE EXCEPTION, and it is the whole mechanism behind inline asm's
// `"=&r"`: an instruction carrying `kLirInstFlagEarlyClobberResult` has its
// def recorded at the EARLY slot instead. Its result range then overlaps the
// slot at which the instruction's inputs are read, so no input expires under
// it and the allocator cannot reuse an input's register for the result. Every
// documented invariant below is unaffected — position numbering,
// `positionToInst` pairing and the sort are untouched; only which of an
// instruction's own two slots the def lands on changes. The
// use-before-def-within-an-instruction discipline of the block-level USE/DEF
// sets is likewise unchanged (uses are still recorded first, so an input is
// still upward-exposed).
//
// The
// substrate ships flat single-interval ranges per vreg today; the
// allocator co-designs split-aware sub-intervals with this substrate
// — see plan 12 §3.1's ML6 sub-interval deferral, which the registry now
// carries as D-PLAN12-SUB-INTERVAL-LIRLIVERANGE-LIST-DEFERRED-FROM-ML6-CYCLE
// (its pre-migration plan-step spelling was ML6-1.1).
//
// LIR has no Phi opcode: MIR Phis were resolved into parallel-copy
// `mov`s on predecessor edges during MIR→LIR isel. Liveness therefore
// sees ordinary def/use sites and does NOT model phi-specific edge
// liveness.

namespace dss {

// ── THE EXCEPTIONAL EDGE (D-LIR-NO-EXCEPTIONAL-EDGE-INTO-A-TRY-HANDLER) ──────
//
// ★★★ A LANDING BLOCK IS A SUCCESSOR OF EVERY BLOCK OF ITS GUARDED RUN, AND
// CONTROL REACHES IT THROUGH A PARTY THAT KEEPS ONLY WHAT A CALL KEEPS.
//
// A GUARDED RUN is a run of blocks, contiguous in the function's block order,
// from ANY instruction of which control may leave for the run's LANDING block
// with nothing of this function executed in between. The transfer is made by a
// party outside the function (an unwinder) that hands the landing block the
// frame as it stood at the transfer and the registers the calling convention
// keeps across a call; every other register arrives destroyed. A `__try`
// region's body and its `__except` handler are the first producer; nothing here
// names it, and any landing — a termination handler entered while unwinding, a
// catch block — is the same shape and inherits the same two consequences:
//
//   1. LIVENESS. The lowering's CFG has no instruction for the transfer (the
//      region is a table of addresses the party reads), so the edge is not a
//      terminator's successor. This analysis ADDS it: the landing block is an
//      exceptional successor of every block of the run, in the block order walk
//      and in the dataflow. A value read in the landing is then live through the
//      whole run, so nothing defined there can be handed its register or its
//      spill slot.
//   2. LOCATION. No code runs on the edge, so there is no place for a move or a
//      reload: such a value must sit in ONE location from before the run to the
//      landing's first instruction, and the party must keep that location. The
//      allocator reads `LirFuncLiveness::landingEntryPositions` beside the call
//      positions — a range that covers a landing entry is given what a range
//      that crosses a call is given (a register the convention saves across a
//      call, or a spill slot), by the one predicate both ask.
//
// ✔MEASURED 2026-10-08 (pe64, both configs), before this existed: a parameter,
// a `double` parameter, the hidden pointer of a struct return, a by-reference
// aggregate parameter and — in release — the shared address of a global, each
// read in a handler or after its region, arrived as whatever the dispatcher
// left in a volatile register. The same reads were right whenever the body
// happened to hold a call (the flat range from the definition to an ORPHAN
// handler block appended after every reachable block crossed that call): an
// accident of the instrument, never a rule.
//
// ⚠ THE RUN IS A RANGE BECAUSE THE TABLE IS A RANGE. What the party dispatches
// from is every address between the run's first block and the block laid out
// after its last one; that — not the source's idea of the body — is what a
// value must survive. The producer owes that the range and the body agree
// (`synthesizeSehFunclets` lays each body out contiguously and refuses a layout
// that does not); every LIR pass from here to emission keeps block order or
// publishes its block image (`lir_descriptor_blocks.hpp`).
//
// ★ ONE RECORD IS ONE RUN, AND A REGION MAY BE SEVERAL RECORDS. Nothing here asks
// which records make up "a region": a record is a run and the landing block it
// enters, two records that name one landing block are two runs that enter it,
// and every rule above holds per record. A record of ONE block
// (`firstBlockV == lastBlockV`) is a run like any other. Two records may be
// apart, touch, lie one inside the other, or hold the very same blocks with
// different landing blocks (a run that belongs to a region and to the region
// around it).
//
// ★ THE LIST IS THE TABLE, IN TABLE ORDER — the order the records are written in,
// which is the order the reading party tries them in: it gives a fault to the
// FIRST record whose range holds the address. The list a function's records
// arrive in is therefore checked as a table (`analyzeLiveness` refuses one that
// would hand a fault to the wrong landing): OF TWO RECORDS THAT SHARE A BLOCK,
// THE DEEPER REGION'S COMES FIRST.
struct DSS_EXPORT LirGuardedRegion {
    std::uint32_t funcIndex     = 0;  // index into `lir.funcAt(i)` of the function holding the run
    std::uint32_t firstBlockV   = 0;  // `LirBlockId.v` of the run's first block
    std::uint32_t lastBlockV    = 0;  // `LirBlockId.v` of its last block (inclusive)
    std::uint32_t landingBlockV = 0;  // `LirBlockId.v` of the landing block
};

// A vreg-id bitset. Bit `k` is set iff virtual register with id `k`
// is in the set. Virtual register id 0 is the invalid sentinel and is
// never set by well-formed producers; insertions of id 0 are silently
// no-op (mirroring the existing `LirReg::valid()` discipline). The
// bitset DOES bounds-check insertions: storage grows on demand so a
// stray out-of-range id never silently corrupts adjacent memory.
struct DSS_EXPORT VRegBitset {
    std::vector<std::uint64_t> bits;
    std::uint32_t              capacity = 0;  // logical vreg-id capacity

    void resizeForCapacity(std::uint32_t numVRegs);
    [[nodiscard]] bool contains(std::uint32_t vregId) const noexcept;
    void insert(std::uint32_t vregId);
    void erase(std::uint32_t vregId) noexcept;
    // Set-union `other` into `*this`. Returns true iff `*this` grew.
    //
    // ⚠ `[[nodiscard]]` because the "grew" answer is the DATAFLOW FIXPOINT'S
    // TERMINATION CONDITION, not a courtesy: `analyzeFuncLiveness`'s `while
    // (changed)` loop is driven by it, and a site that drops it silently
    // under-propagates liveness — a wrong live range, i.e. a register
    // allocated over a still-live value, which is a silent miscompile rather
    // than a crash. The one site that legitimately ignores it (`liveIn` is
    // rebuilt from scratch and compared word-wise afterwards) now says so with
    // an explicit `(void)`, so "ignored on purpose" and "forgotten" stop
    // looking identical. Sibling of the same class as
    // D-LIR-2ADDR-IGNORES-EMIT-TERMINATOR-FAILURE, found while closing it.
    [[nodiscard]] bool unionInPlace(VRegBitset const& other);
    // Set-difference: clear bits also set in `mask` from `*this`.
    void subtractInPlace(VRegBitset const& mask);
    // Reset all bits without releasing storage.
    void clear() noexcept;
};

// One live range for one virtual register over one function. The
// invariant `start < end` is enforced at construction via `make()`.
struct DSS_EXPORT LirLiveRange {
    LirReg        vreg{};   // virtual register (isPhysical == 0)
    std::uint32_t start = 0;  // inclusive: position of first def (or 0 if live-in)
    std::uint32_t end   = 0;  // exclusive: position past last use

    // Factory enforcing `start < end` AND `vreg.isPhysical == 0`.
    // The substrate is pre-regalloc-only; a physical-reg range here
    // signals a producer bug.
    [[nodiscard]] static LirLiveRange make(LirReg vreg,
                                           std::uint32_t start,
                                           std::uint32_t end);
};

// ── THE ONE INTERFERENCE PREDICATE (plan 22 OPT8 — register coalescing) ──────
//
// ★★★ TWO RANGES INTERFERE IFF THEIR HALF-OPEN POSITION INTERVALS OVERLAP, AND
// THIS FUNCTION IS THE ONLY PLACE THAT SENTENCE IS SPELLED. Three consumers ask
// it, and the whole safety argument for coalescing is that they ask the SAME
// one:
//
//   * the COALESCER (`lir_regalloc.cpp`) — may merge two live ranges into one
//     register only when this returns false;
//   * the ALLOCATOR's own register reuse — `expireActive` returns a register to
//     the free list when `active.range.end <= currentStart`, and because
//     `LirFuncLiveness::ranges` is sorted ascending by `start`, that condition
//     is exactly `!lirRangesInterfere(active.range, current)`. The linear scan
//     was ALREADY permitted to hand two ranges one register on precisely this
//     test; coalescing only makes it do so deliberately. **That is why
//     coalescing introduces no new class of hazard: every assignment it
//     produces is one the allocator could already have produced by luck.**
//   * the AUDITOR (`findAllocationConflict`, `lir_regalloc.hpp`) — re-derives
//     the question on the FINISHED assignment table, from liveness alone,
//     without consulting the coalescer's union-find. A coalescer that merged
//     two interfering ranges is a SILENT MISCOMPILE (two live values, one
//     register), so the check that catches it must not be able to inherit the
//     transform's own belief. That independence is the whole point, and it is
//     the `D-OPT-JCC-FALLTHROUGH` shape: one shared predicate, consulted by the
//     transform AND by a verifier that re-derives rather than re-reads.
//
// ⚠ THE PREDICATE IS AN OVER-APPROXIMATION OF LIVENESS, AND THAT DIRECTION IS
// THE SAFE ONE. `LirFuncLiveness` ships FLAT single-interval ranges (see the
// header note above), so a value dead in a hole is still reported live across
// it. Over-approximating liveness makes this return `true` where a
// split-interval analysis would return `false` — a MISSED coalesce, never an
// unsound one. When split-aware sub-intervals land (D-PLAN12-SPLIT-AWARE-SUB-INTERVAL-LIRLIVERANGE-LIST-CURRENTLY-FLAT) this predicate
// gains precision and every consumer gains it at once.
[[nodiscard]] constexpr bool
lirRangesInterfere(LirLiveRange const& a, LirLiveRange const& b) noexcept {
    return a.start < b.end && b.start < a.end;
}

// Per-function liveness result. Owned by the module-level wrapper.
//
// Invariants (asserted by the producer; consumers may rely):
//   - `liveIn.size() == liveOut.size() == blockOrder.size()`
//   - all `VRegBitset`s in `liveIn`/`liveOut` share the same capacity
//   - `positionToInst.size() == totalPositions`
//   - `totalPositions` is even (each inst occupies 2 slots)
//   - `positionToInst[2*N] == positionToInst[2*N + 1]`
//   - `ranges` is sorted ascending by `start`, then by `vreg.id`
//   - no range carries `vreg.id == 0` (sentinel exclusion)
//   - `start < end` for every range
struct DSS_EXPORT LirFuncLiveness {
    LirFuncId fn{};

    // Block visitation order (RPO). Index i is the "block-order index"
    // referenced by `liveIn`/`liveOut`.
    std::vector<LirBlockId> blockOrder;

    // Per-block live-in / live-out, indexed by block-order index.
    std::vector<VRegBitset> liveIn;
    std::vector<VRegBitset> liveOut;

    // Position-numbered live ranges, sorted ascending by `start`.
    std::vector<LirLiveRange> ranges;

    // Total number of position slots: 2 * (instruction count summed
    // over all blocks).
    std::uint32_t totalPositions = 0;

    // Position → LIR instruction id. Adjacent positions for the same
    // inst share an id (early/late slot of the N-th inst both point at
    // that inst).
    std::vector<LirInstId> positionToInst;

    // ── the guarded runs of this function (see `LirGuardedRegion`) ──
    // One entry per region the analysis was given for this function, in the
    // order given, in the vocabulary this result speaks: indices into
    // `blockOrder` and positions. EMPTY for a function analyzed without regions.
    struct GuardedRun {
        std::vector<std::uint32_t> blockOrderIndices;     // the run's blocks
        std::uint32_t landingOrderIndex    = 0;           // its landing block
        std::uint32_t landingEntryPosition = 0;           // EARLY slot of the landing's first instruction
    };
    std::vector<GuardedRun> guardedRuns;

    // The EARLY slot of the first instruction of every landing block, ascending,
    // each once. A range `r` is live INTO a landing iff some entry `p` has
    // `r.start < p && p < r.end` — the value is defined before the block's first
    // slot and read at or after it (`lirRangeEntersALanding`). ⚠ Unlike a call
    // position, where a value consumed BY the call is safe in a volatile
    // register, a value read by the landing's very first instruction has already
    // been through the transfer: the loss happens BEFORE the slot, not after it.
    std::vector<std::uint32_t> landingEntryPositions;
};

// True iff `r` is live into a landing block of its function: defined before the
// block's first slot and read at or after it. `landingEntryPositions` is
// `LirFuncLiveness::landingEntryPositions` (ascending). The ONE spelling of the
// question, asked by the allocator when it chooses a location and by the audit
// that re-reads the finished assignment.
[[nodiscard]] DSS_EXPORT bool
lirRangeEntersALanding(LirLiveRange const&               r,
                       std::span<std::uint32_t const>    landingEntryPositions) noexcept;

// Module-level wrapper. One `LirFuncLiveness` per function, in the
// same order as `lir.funcAt(i)`.
struct DSS_EXPORT LirLiveness {
    std::vector<LirFuncLiveness> perFunc;

    // Locate the per-function result for `fn`. Returns nullptr if no
    // result exists for that function (defensive — out-of-range or
    // cross-module misuse should not silently alias another function).
    [[nodiscard]] LirFuncLiveness const* forFunc(LirFuncId fn) const noexcept;
};

// Run liveness analysis over every function in `lir`. The caller owns
// `lir`; the analysis returns a freshly-allocated result. No
// `TargetSchema` parameter: def/use derivation is target-blind (def =
// every non-physical register `lirForEachInstDef` names — the result, plus a
// bundle's writing operands; use = every non-physical register
// `lirForEachInstUse` names).
[[nodiscard]] DSS_EXPORT LirLiveness
analyzeLiveness(Lir const& lir);

// The same analysis over a module that holds GUARDED RUNS (`LirGuardedRegion`):
// each run's landing block is an exceptional successor of every block of the
// run. ★ THIS IS THE OVERLOAD THE PIPELINE CALLS, with the regions MIR→LIR
// returned; the one above is for a module that has none.
//
// It REFUSES — a reported error, `std::nullopt` — instead of analyzing a region
// it cannot stand behind:
//   * a region whose function index, or any of whose three blocks, is not of
//     this module; whose first block is laid out after its last; or whose
//     landing block lies INSIDE its own run (a fault in the landing would be
//     delivered to the landing);
//   * two records of one function, LISTED IN AN ORDER THE FIRST-MATCH READING
//     GETS WRONG: they share a block and the earlier one's run does not lie
//     inside the later one's (the later is the deeper region's, or neither holds
//     the other); or they hold the same blocks and the earlier one's landing
//     block is not inside a run that enters the later one's (the earlier is the
//     OUTER region's). This is the whole of the order rule, checked where every
//     producer's records meet — the pass that orders regions and any tier that
//     adds a record for a run of its own making;
//   * a landing that READS A VALUE DEFINED INSIDE ITS RUN. Such a value cannot
//     be delivered: the transfer may precede the definition. The lowering's own
//     CFG forbids it by dominance (the landing hangs off the region's opening,
//     not off its body), and the edges added here must not turn it into
//     something liveness quietly accepts — an edge from a block's END would let
//     a definition inside that block "dominate" the landing.
[[nodiscard]] DSS_EXPORT std::optional<LirLiveness>
analyzeLiveness(Lir const&                          lir,
                std::span<LirGuardedRegion const>   regions,
                DiagnosticReporter&                 reporter);

// Run liveness analysis for a single function.
[[nodiscard]] DSS_EXPORT LirFuncLiveness
analyzeFuncLiveness(Lir const& lir, LirFuncId fn);

} // namespace dss
