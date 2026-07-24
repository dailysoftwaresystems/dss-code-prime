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

// Diagnostic (D-AS-REGALLOC-LOOP-CARRIED-SPILL-RELOAD-MISSING), ENV-GATED by
// `DSS_CHECK_LOOP_SPILL=1` and zero-cost when unset. Reports a loop-carried value
// whose dependency cycle is BROKEN: a register loaded from a spill slot OUTSIDE a
// loop, USED inside it, never REDEFINED inside it, while the loop STORES that slot —
// i.e. every iteration reads the same stale value and the update is lost. `stage`
// labels the pipeline point so the same check can bisect which post-regalloc pass
// (rewrite / two-address legalize / callconv) drops the loop-carried update.
DSS_EXPORT void
checkLoopCarriedSpills(Lir const& lir, TargetSchema const& schema,
                       char const* stage);

// Companion dump (same gating family): `DSS_DUMP_LIR_MIN_INSTS=<N>` appends every
// function with >= N instructions — blocks, successors, and each instruction's
// mnemonic/result/operands — to `DSS_DUMP_LIR_FILE`, prefixed by `stage`. Lets the
// LIR at a given pipeline point be diffed against the final assembly.
DSS_EXPORT void
dumpLirFuncs(Lir const& lir, TargetSchema const& schema, char const* stage);

} // namespace dss

// Post-regalloc verifier lives in `lir_verifier.hpp` —
// `verifyLirPostRegalloc(lir, schema, reporter)`.
