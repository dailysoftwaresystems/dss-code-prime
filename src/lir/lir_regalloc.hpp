#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
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
// physical register (per the active `TargetCallingConvention`). The
// allocator's tryAllocate always tries the callee-saved partition
// first; for ranges that do NOT cross a call it then falls back to
// caller-saved; for cross-call ranges it falls back directly to
// spilling. Call detection is target-agnostic via
// `TargetOpcodeInfo::isCall`.
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
    // Cached index of the calling convention used to drive this
    // allocation. The downstream prologue/epilogue emitter reads this
    // so it doesn't re-derive the cc choice. Hazard: reordering
    // calling-convention entries in a target JSON file will change
    // every function's allocation here unless callers also re-pin by
    // name.
    std::uint16_t                 callingConventionIndex = 0;

    // Find the assignment for the given vreg id, or nullptr if none.
    [[nodiscard]] LirRegAssignment const*
    forVReg(std::uint32_t vregId) const noexcept;
};

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
// `LirFuncLiveness` produced over the same `lir`. The schema-wide
// validity check (≥1 calling convention) is repeated here for callers
// that bypass `allocateRegisters`.
[[nodiscard]] DSS_EXPORT LirFuncAllocation
allocateFuncRegisters(Lir const&             lir,
                      TargetSchema const&    schema,
                      LirFuncLiveness const& flow,
                      DiagnosticReporter&    reporter);

} // namespace dss
