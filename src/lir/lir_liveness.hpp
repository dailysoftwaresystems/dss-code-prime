#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "lir/lir.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
#include <vector>

// `LirLiveness` (plan 12 §2.8) — per-function liveness analysis over the
// frozen LIR module. Substrate-tier; consumed by the linear-scan
// allocator (§2.8 cycle 2) and by post-regalloc spill scheduling.
//
// Target-blind: the analysis depends only on the LIR module's CFG +
// per-instruction operand shape (Reg-kind operand → use; result vreg
// → def). It never inspects opcode semantics, mnemonics, or
// `TargetSchema`. Source-language-blind: input is LIR; the analysis
// never touches MIR/HIR types.
//
// Position numbering: blocks are visited in reverse post-order (RPO);
// each instruction gets TWO position slots (early/late half-steps) so
// uses (read at the early half) and defs (written at the late half)
// at the same instruction don't collide. Position `p` for the N-th
// instruction in RPO order: `early = 2*N`, `late = 2*N + 1`. The
// substrate ships flat single-interval ranges per vreg today; the
// allocator co-designs split-aware sub-intervals with this substrate
// — see plan 12 §3.1 ML6 deferral D-ML6-1.1.
//
// LIR has no Phi opcode: MIR Phis were resolved into parallel-copy
// `mov`s on predecessor edges during MIR→LIR isel. Liveness therefore
// sees ordinary def/use sites and does NOT model phi-specific edge
// liveness.

namespace dss {

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
    bool unionInPlace(VRegBitset const& other);
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
};

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
// `instResult(id).valid() && !isPhysical`; use = Reg-kind operand
// with a valid non-physical reg).
[[nodiscard]] DSS_EXPORT LirLiveness
analyzeLiveness(Lir const& lir);

// Run liveness analysis for a single function.
[[nodiscard]] DSS_EXPORT LirFuncLiveness
analyzeFuncLiveness(Lir const& lir, LirFuncId fn);

} // namespace dss
