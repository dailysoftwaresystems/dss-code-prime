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
#include <string_view>
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
//      (Load/Store/Lea) must carry exactly one of the TWO addressing
//      modes the shipped lowering emits: a base+displacement address
//      (operand list ends `[..., MemBase, MemOffset]`, names no symbol)
//      or a symbol-addressed one (names a `SymbolRef`, carries no
//      MemBase/MemOffset). Catches malformed addressing-mode operand
//      lists, including one that MIXES the two.
//      ⚠ Until 2026-08-15 this rule knew only the first form and was
//      therefore FALSE about the LIR this compiler builds — it is the
//      reason `verifyLir` could not be wired
//      (D-LIR-VERIFY-MEM-OPERAND-PAIRING-RULE-IS-FALSE). See the long
//      note over the rule in `lir_verifier.cpp` before narrowing it.
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
//      result type. Same mapping-driven walk as rule 2. Minted to close
//      the cycle-3d "lowerLoad / prepassAllocatePhis /
//      emitPhiMovesForEdge hardcoded GPR" silent-corruption hazard.
//      ⚠⚠ NOT RUN BY `verifyLir` — measured false about the shipped
//      lowering (2026-08-15). It lives behind
//      `verifyLirVregClassesAgainstMir`; see that declaration below.
//
// Cycle 3e takes `TargetSchema const&` from the caller (was previously
// hardcoded `loadShipped("x86_64")` — architect-flagged target-
// agnosticism violation, now closed). The `lirToMirMap` parameter is
// the cycle-3e-added side-table from `MirToLirResult.lirToMir`; the
// verifier walks the LIR module and uses the mapping to fetch the
// source MIR inst per LIR inst.

// ★★ THIS TIER RUNS ON EVERY REAL COMPILE — as of 2026-08-15, and NOT
// before. It is worth recording what it cost to get here, because the
// gap was invisible from inside this header. Until 2026-08-14 every
// entrypoint below had ZERO call sites outside `tests/`: the whole LIR
// rule set was inert on the real path (`lowerWideCallArgs` → regalloc →
// `rewriteWithAllocation` → `legalizeTwoAddress` →
// `materializeCallingConvention` → assembler), so
// `L_VirtualRegInPostRegalloc`, `L_InvalidSpillSlotSentinel`,
// `L_MemOperandMalformed`, `L_TerminatorSuccessorMismatch` and the three
// `L_SideStructure*` codes could not fire during a compile — even though
// `unsuppressable_codes.cpp` justifies their membership with "silenced,
// the violation reaches the assembler", and `lir_rewrite.hpp` had told
// its callers to run `verifyLirPostRegalloc` since ML6 while none did.
//
// `verifyLirRebuild` + `verifyLirPostRegalloc` were wired first; `verifyLir`
// could not join them because Rule 1 was FALSE about the shipped lowering
// (D-LIR-VERIFY-MEM-OPERAND-PAIRING-RULE-IS-FALSE) and reddened the
// examples corpus wholesale. With that rule corrected, all four run from
// `src/program/compile_pipeline.cpp`.
//
// ⚠ THE STANDING OBLIGATION THIS LEAVES: a rule added here is now live on
// every compile, so it must be true of what the shipped lowering EMITS,
// not only of what a hand-built test module contains. Rule 1 was wrong for
// its whole lifetime for exactly that reason. Pin a new rule against a
// module produced by the real `lowerToLir`, not by `Lir`'s arena ctor.

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

// Rule 3 in isolation — the vreg-class-vs-MIR-result-type cross-check.
//
// ⚠⚠ SEPARATE FROM `verifyLir` BECAUSE IT IS **MEASURED FALSE** ABOUT THE
// SHIPPED LOWERING (✔2026-08-15: 31 of 594 `examples/` red, all
// float-bearing). `lirToMir` is MANY-to-one — MIR→LIR expands one MIR inst
// into several LIR insts and records the same `MirInstId` against all of
// them — and this rule assumes one-to-one, so it judges ADDRESS-materializing
// helpers (`lea_frame_slot` → gpr for a MIR `load` of an F64) and wide-float
// carriers (`fldur_q` → vr for a MIR `fptosi` returning I32) against a type
// that belongs to a sibling instruction.
//
// Closing it needs a ONE-TO-ONE channel the lowering does not publish today
// (`MirToLirResult` exposes only `lirToMir`); that is a change to
// `mir_to_lir.cpp`, not to the verifier. Until then this entrypoint keeps the
// rule named, greppable and unit-testable instead of silently deleted — the
// hazard it guards (a hardcoded GPR class on a float value) is real.
// ⚠ Do NOT call this from `compile_pipeline.cpp`.
[[nodiscard]] DSS_EXPORT bool
verifyLirVregClassesAgainstMir(Lir const&                 lir,
                               Mir const&                 mir,
                               TypeInterner const&        interner,
                               TargetSchema const&        schema,
                               std::span<MirInstId const> lirToMirMap,
                               DiagnosticReporter&        reporter);

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

// Text-load verifier (ML8 cycle 2). Runs the subset of `verifyLir`
// rules that don't require an MIR cross-reference — currently just
// `checkMemOperandPairing` (Rule 1). Rules 2–4 are MIR-dependent and
// are out of scope for a text-loaded module: the source MIR isn't
// part of `.dsslir`, so cross-references would necessarily be empty.
// Future rules added to `verifyLir` should join here too if they are
// LIR-only. Returns `false` on any violation.
[[nodiscard]] DSS_EXPORT bool
verifyLirText(Lir const& lir, TargetSchema const& schema,
              DiagnosticReporter& reporter);

// Paired REBUILD verifier (D-LIR-PER-INST-REG-CONSTRAINTS).
// Call it with the module a rebuilding pass consumed and the module it
// produced; `passName` is the prefix in any diagnostic (e.g. "rewrite",
// "callconv") so the offending pass is identifiable.
//
// It asserts the three ways a rebuild can lose a module SIDE STRUCTURE —
// the wide-literal pool and the per-instruction register-constraint pool,
// both referenced BY INDEX from the instruction stream a rebuild
// re-creates:
//
//   * a pool with FEWER entries than it went in with — the signature of a
//     forgotten `lir_pass_util::copyModuleSideStructures`
//     (`L_SideStructurePoolShrank`);
//   * a reference pointing outside its pool, plus a constraint-pool entry
//     no instruction names — the module-local rules, run on the OUTPUT
//     (`L_SideStructureIndexDangling`, `L_SideStructureReferenceLost`);
//   * FEWER references than went in (`L_SideStructureReferenceLost`).
//     ★ This last one is the only instrument that catches a dropped
//     LITERAL reference: the pool is intact and every surviving index
//     resolves, so the count is the only place the loss is visible.
//
// The module-local half runs unpaired inside `verifyLir`,
// `verifyLirText` and `verifyLirPostRegalloc` and needs no caller
// cooperation. This entrypoint exists for the checks that genuinely need
// the before/after pair. Returns `false` on any violation.
[[nodiscard]] DSS_EXPORT bool
verifyLirRebuild(Lir const& before, Lir const& after,
                 std::string_view passName, DiagnosticReporter& reporter);

} // namespace dss
