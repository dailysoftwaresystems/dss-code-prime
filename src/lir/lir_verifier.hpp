#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "mir/mir.hpp"

#include <cstdint>

// `LirVerifier` (plan 12 §2.9, ML5 cycle 3e) — substrate-tier
// invariant checker for the frozen `Lir` module. Mirrors `MirVerifier`
// (ML3 cycle 1): a class instantiated once per verify, runs a sequence
// of rule families, emits `L_VerifierFailure`-shaped diagnostics into
// the reporter, returns `true` iff no error-severity diagnostics were
// added during the run.
//
// Cycle 3e ships THREE rule families anchored by the cycle-3c review
// and the cycle-3d FPR-class plumbing review:
//
//   1. `checkMemOperandPairing` — every LIR memory-bearing instruction
//      (Load/Store/Lea) must end with the `[Reg base, ..., MemBase,
//      MemOffset]` operand triple/quad. Catches a malformed builder
//      that emits Load with 2 operands (missing MemOffset) or Store
//      with operand kinds out of order.
//
//   2. `checkStoreRegClassMatchesMirType` — for each Store inst, the
//      value operand's vreg class must equal the regClassForCoreType
//      of the MIR pointee type. Closes the silent-corruption hazard
//      where an F64 Load + F64 Store chain could route through GPR
//      vregs and corrupt at AS1 encoding.
//
//   3. `checkVregClassMatchesMirType` — for every Lir inst that
//      results from a MIR inst (via the lowerer's `valueToReg`
//      mapping), the LirReg's class must equal regClassForCoreType of
//      the MIR result type. The cycle-3d review caught the cycle-3c
//      lowerLoad / prepassAllocatePhis / emitPhiMovesForEdge gap
//      where some methods hardcoded GPR; this verifier rule catches
//      any future regression of the same shape.
//
// Cycle 3e is the FIRST consumer of `regClassForCoreType` (cycle 3d
// substrate-tier helper). Adding the verifier completes the cycle:
// substrate function → lowerer + verifier both consume.
//
// The verifier requires the `Mir` source (to read `instType` for
// each LIR-producing MIR inst) and the `TypeInterner` (to read
// `kind(typeId)`). It does NOT require a `valueToReg` mapping — for
// cycle-3e checks, the verifier walks both Mir + Lir in matching
// order (the lowerer emits instructions in MIR-block order, so
// LIR-block-N corresponds to MIR-block-N; rules 2+3 cross-reference
// at the block-tier).

namespace dss {

struct DSS_EXPORT LirVerifyResult {
    bool ok = true;
};

// Run all rule families. The caller owns `lir`, `mir`, `interner`,
// and `reporter`; the verifier does not mutate any of them. Returns
// `ok=true` iff zero error-severity diagnostics were added during the
// run (delta-on-errorCount discipline mirroring ML2/ML3/cycle-3a-3d).
[[nodiscard]] DSS_EXPORT LirVerifyResult
verifyLir(Lir const&          lir,
          Mir const&          mir,
          TypeInterner const& interner,
          DiagnosticReporter& reporter);

} // namespace dss
