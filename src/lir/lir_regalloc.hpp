#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
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
// spill the current range. (Wimmer-Franz interval splitting is NOT
// implemented — see "sub-interval splitting" note below.)
//
// **Call-aware constraint**: a range that survives past the late
// slot of a call instruction MUST NOT be assigned to a caller-saved
// physical register (per the active `TargetCallingConvention`). The
// allocator preserves the constraint by preferring callee-saved
// regs for call-crossing ranges and falling back to spilling when
// callee-saved is exhausted. Call detection is target-agnostic via
// `TargetOpcodeInfo::isCall`. Sub-interval splitting would refine
// this — splitting a range at the call site lets the across-call
// portion go to callee-saved while the live-before / live-after
// portions use any class. Not implemented; flat-interval allocation
// is conservative but always correct.
//
// **Reserved registers**: registers that appear in no calling-
// convention saved/arg/return set (e.g. `rsp`, `rflags`) are filtered
// out of the allocator's pool. Allocating `rsp` would clobber the
// stack pointer mid-function — fatal at runtime.
//
// **Target-blind**: depends only on `TargetSchema.registers()` +
// `callingConventions()` + per-opcode `isCall` flag. No mnemonic
// checks, no per-target if-arms.
//
// **Source-language-blind**: consumes only `Lir` + `LirLiveness`.

namespace dss {

// Per-vreg assignment. Exactly one of `physReg.valid()` (assigned a
// physical register) OR `spillSlot != UINT32_MAX` (assigned a stack
// slot) holds for a well-formed assignment. Default-constructed
// slots (vreg.id == 0) are the unfilled sentinel.
//
// Preconditions (process aborts on violation via the factories):
//   makePhys:  vreg.isPhysical == 0; phys.isPhysical == 1;
//              vreg.regClass() == phys.regClass()
//   makeSpill: vreg.isPhysical == 0; slot != UINT32_MAX
struct DSS_EXPORT LirRegAssignment {
    LirReg        vreg{};          // input virtual register
    LirReg        physReg{};       // output physical register (InvalidLirReg if spilled)
    std::uint32_t spillSlot = UINT32_MAX;

    [[nodiscard]] bool isSpilled() const noexcept {
        return spillSlot != UINT32_MAX;
    }

    [[nodiscard]] static LirRegAssignment makePhys(LirReg vreg, LirReg phys);
    [[nodiscard]] static LirRegAssignment makeSpill(LirReg vreg, std::uint32_t slot);
};

// Per-function allocation result. `assignments[i]` is the assignment
// for vreg id `i` (slot 0 is the default-constructed sentinel — every
// well-formed vreg has id ≥ 1; the substrate guarantees dense vreg
// ids from 1 so the vector indexed by id has no holes beyond the
// sentinel). `numSpillSlots` is the total stack slot count needed for
// this function (one slot per spilled vreg — coalescing not yet
// implemented).
struct DSS_EXPORT LirFuncAllocation {
    LirFuncId                     fn{};
    std::vector<LirRegAssignment> assignments;
    std::uint32_t                 numSpillSlots = 0;
    // Cached index of the calling convention used to drive this
    // allocation. The downstream prologue/epilogue emitter reads this
    // so it doesn't re-derive the cc choice. Currently always 0 —
    // per-function cc selection requires functions to carry an
    // explicit cc attribute and is not yet implemented; reordering
    // calling-convention entries in a target JSON file will change
    // every function's allocation here.
    std::uint16_t                 callingConventionIndex = 0;

    // Find the assignment for the given vreg id, or nullptr if none.
    [[nodiscard]] LirRegAssignment const*
    forVReg(std::uint32_t vregId) const noexcept;
};

// Module-level wrapper. Per-function entries in the same order as
// `lir.funcAt(i)`.
struct DSS_EXPORT LirAllocation {
    std::vector<LirFuncAllocation> perFunc;

    [[nodiscard]] LirFuncAllocation const* forFunc(LirFuncId fn) const noexcept;
};

// Allocate physical registers for every function in `lir`. Reads
// `schema.registers()` + `schema.callingConventions()` and the
// liveness side-tables. Returns a freshly-allocated per-function
// side-table; the caller owns the result. The LIR module is NOT
// mutated — a separate rewrite pass + spill-load/store insertion
// consumes the side-table.
[[nodiscard]] DSS_EXPORT LirAllocation
allocateRegisters(Lir const&          lir,
                  TargetSchema const& schema,
                  LirLiveness const&  liveness);

// Allocate for a single function. The caller must supply the matching
// `LirFuncLiveness` produced over the same `lir`.
[[nodiscard]] DSS_EXPORT LirFuncAllocation
allocateFuncRegisters(Lir const&             lir,
                      TargetSchema const&    schema,
                      LirFuncLiveness const& flow);

} // namespace dss
