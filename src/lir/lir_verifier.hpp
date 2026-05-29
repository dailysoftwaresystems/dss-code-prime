#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "mir/mir.hpp"

#include <cstdint>
#include <span>
#include <vector>

// `LirVerifier` (plan 12 §2.9) — substrate-tier invariant checker
// for the frozen `Lir` module. Mirrors `MirVerifier`: runs a sequence
// of rule families, emits `L_*`-family diagnostics into the reporter,
// returns `true` iff no error-severity diagnostics were added during
// the run. Pre-regalloc rules emit `L_UnsupportedLoweringForOpcode`
// (the existing rule-family code); the post-regalloc rule emits
// `L_VirtualRegInPostRegalloc` for virtual-reg violations and
// `L_InvalidSpillSlotSentinel` for frame-op payload-0 violations.
//
// Three rule families:
//
//   1. `checkMemOperandPairing` — every LIR memory-bearing instruction
//      (Load/Store/Lea) must end with the `[MemBase, MemOffset]`
//      operand pair. Catches malformed addressing-mode operand lists.
//
//   2. `checkStoreRegClassMatchesMirType` — for each LIR `store` inst
//      whose source MIR inst is a MIR `Store`, the value operand's
//      vreg class must equal regClassForCoreType of the MIR pointee
//      type. Uses the `lirToMir` mapping (see below) to drive the
//      cross-reference per LIR inst, NOT positional MIR-vs-LIR walk
//      (cycle-3e fix-up: positional walk silently skipped switch-
//      bearing functions whose LIR has extra synthetic blocks).
//
//   3. `checkVregClassMatchesMirType` — for every LIR inst with both
//      a valid result vreg AND a recorded source MIR inst, the
//      LirReg's class must match regClassForCoreType of the MIR
//      result type. Same mapping-driven walk as rule 2. Closes the
//      cycle-3d "lowerLoad / prepassAllocatePhis / emitPhiMovesForEdge
//      hardcoded GPR" silent-corruption hazard at the verifier tier.
//
// Cycle 3e takes `TargetSchema const&` from the caller (was previously
// hardcoded `loadShipped("x86_64")` — architect-flagged target-
// agnosticism violation, now closed). The `lirToMirMap` parameter is
// the cycle-3e-added side-table from `MirToLirResult.lirToMir`; the
// verifier walks the LIR module and uses the mapping to fetch the
// source MIR inst per LIR inst.

namespace dss {

struct DSS_EXPORT LirVerifyResult {
    bool ok = true;
};

// Run all rule families. The caller owns `lir`, `mir`, `interner`,
// `schema`, and `reporter`; the verifier does not mutate any of them.
// `lirToMirMap` is `MirToLirResult.lirToMir` — indexed by LirInstId.v,
// returning `InvalidMirInst` for LIR insts with no source MIR inst.
// Returns `ok=true` iff zero error-severity diagnostics were added.
[[nodiscard]] DSS_EXPORT LirVerifyResult
verifyLir(Lir const&             lir,
          Mir const&             mir,
          TypeInterner const&    interner,
          TargetSchema const&    schema,
          std::span<MirInstId const> lirToMirMap,
          DiagnosticReporter&    reporter);

// Post-regalloc verifier. Checks that the LIR module contains no
// virtual registers in any result or operand position AND that every
// `frame_load`/`frame_store` carries a non-zero `LirSpillSlot` payload
// (slot 0 is the invalid sentinel). Returns `false` on any violation;
// emits `L_VirtualRegInPostRegalloc` for virtual-reg violations and
// `L_InvalidSpillSlotSentinel` for frame-op payload-0 violations.
// Separate from `verifyLir` because pre-rewrite LIR legitimately
// contains virtuals.
[[nodiscard]] DSS_EXPORT bool
verifyLirPostRegalloc(Lir const& lir, TargetSchema const& schema,
                      DiagnosticReporter& reporter);

} // namespace dss
