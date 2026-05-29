#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_regalloc.hpp"

// LIR post-regalloc rewrite pass. Consumes an `Lir` module + a
// matching `LirAllocation` side-table and produces a fresh `Lir`
// module where every virtual register has been replaced by the
// physical register assigned by the allocator. Spilled virtual
// registers are rewritten as a pair of frame pseudo-ops:
//
//   * USE of a spilled vreg → emit `frame_load <scratch>, slot`
//     BEFORE the instruction; the use operand becomes `<scratch>`.
//   * DEF of a spilled vreg → the instruction's `result` becomes
//     `<scratch>`; emit `frame_store <scratch>, slot` AFTER.
//
// **Scratch register pool**: per-class, populated from the target
// schema's register file filtered down to the active calling
// convention's allocatable pool (saved/arg/return sets) MINUS every
// register the allocator already assigned to a vreg in this function.
// Reserved-role registers (`rsp`, `rflags`) are absent from any cc
// pool and therefore never become eligible scratches.
//
// **Multi-spill-per-class**: each instruction holds a per-class
// cursor into the scratch pool. Successive spilled operands of the
// same class pull successive scratches — closing the silent-miscompile
// path where a single shared scratch would lose earlier loads'
// values to later loads' overwrites. If the pool runs out before
// every spill is serviced, the function emits
// `L_VirtualRegInPostRegalloc` Error and the rewrite fails loud.
//
// `frame_load` / `frame_store` are TARGET-BLIND pseudo-ops at LIR
// tier. A future per-target callconv lowering materializes them as
// `mov result, [stackPointer + slot.offset]` (or the AArch64
// equivalent). The slot offset comes from per-target callconv layout.
//
// **Correctness invariants** the rewrite preserves on success:
//   * Instruction count: rewritten function has ≥ original (only
//     frame_load/frame_store ops are inserted; no deletions).
//   * Block topology: every original block has a 1:1 counterpart in
//     the rewritten function, with the same successor edges. The
//     rewrite does NOT split blocks.
//   * No virtual registers remain anywhere in the output (verified
//     by `verifyLirPostRegalloc`).
//
// **Failure modes** (the result's `ok == false` is the ONLY reliable
// signal — callers must NOT consume the output module on failure):
//   * `LirFuncAllocation::ok == false` — that function's slot stays
//     empty in the output and `ok` flips false at module level.
//   * No scratch physical register available for a needed class →
//     `L_VirtualRegInPostRegalloc` Error; output `Lir` is empty.
//   * Calling convention lookup fails (target schema misshaped) →
//     `L_RequiredLirOpcodeMissing` Error; output `Lir` is empty.

namespace dss {

struct DSS_EXPORT LirRewriteResult {
    Lir  lir{};
    bool ok = true;
};

[[nodiscard]] DSS_EXPORT LirRewriteResult
rewriteWithAllocation(Lir const&          src,
                      TargetSchema const& schema,
                      LirAllocation const& alloc,
                      DiagnosticReporter& reporter);

} // namespace dss

// Post-regalloc verifier lives in `lir_verifier.hpp` —
// `verifyLirPostRegalloc(lir, schema, reporter)`.
