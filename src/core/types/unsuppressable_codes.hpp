#pragma once

#include "core/export.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <span>

namespace dss {

// D-FF2-UNSUPP: closed-table of DiagnosticCodes whose emission MUST reach the
// reporter regardless of any `--suppress` policy. These codes' emission
// gates `ok` / `errorCount()` / exit-code semantics — suppressing them
// would silently re-open the failure surface they were introduced to
// close.
//
// Pre-fold: `--suppress=H_ExternHasInitializer` would silently drop the
// extern-init reject, the lowering's `ok` would stay false but the
// reporter's `errorCount()` would return 0 (the diagnostic never landed
// in `all_`), and any caller reading `errorCount()` to gate exit code
// would see green — the silent-drop the H_ExternHasInitializer fold was
// meant to permanently close would be re-opened with one CLI flag.
//
// Post-fold (eb2c6c7 refinement): `applyPolicy` consults
// `isUnsuppressable(d.code)` BEFORE the suppress check; unsuppressable
// codes bypass SILENCING mutations (`--suppress` drops + `overrides`
// demotion) but NOT severity ELEVATION (`--warnings-as-errors`) —
// elevation strengthens the signal rather than defeating the gate, so
// strict-mode operators get fail-loud exit codes on Warning-severity
// unsuppressable codes like F_BinaryReaderPartialCorruption.
// Post-fold #11 silent-failure F1: `report()` itself ALSO bypasses cap /
// dedup / maxPerCode / maxDiagnostics for unsuppressable codes — the
// four silent-drop gates around applyPolicy would otherwise re-open the
// surface even when policy correctly let the code through.
//
// Membership tiers (informational — the closed-table at the .cpp is
// the single source of truth):
//   - Permanent architectural exclusions / wrong-machine-code
//     surfaces: D_TargetAbiModelUnsupportedByDriver,
//     D_TargetMachineCodeMismatch (D-LK6-8.2 SIGILL),
//     D_TargetAbiModelMismatch (D-LK6-8.2 SIGILL),
//     F_FfiIngestAbiModelUnsupported, F_FfiIngestEmptyCanonical,
//     H_ExternHasInitializer.
//   - Pending-plan announcement (suppressing misleads the user):
//     D_PlanNotLanded.
//   - Lowering / verifier structural invariants (cannot reach
//     codegen): H_UnsupportedLoweringForKind, H_ExternDeclMalformed,
//     H_VerifierFailure, H_TypeUnresolved + ALL 12 I_* MIR-verifier
//     codes (frozen-module structural / SSA / dominance invariants).
//   - Linker fail-loud (image refused / undefined extern / image-
//     write contract violation / format-walker invariants):
//     K_SymbolUndefined, K_ImageNotOk, K_ImageEmpty + 4
//     K_ImageWrite* codes (LK10 contract), plus
//     K_NoMatchingObjectFormat, K_FormatLacksImportSupport,
//     K_RelocationKindMismatch, K_WalkerInputContractViolation
//     (format-walker dispatch / extern / reloc invariants).
//   - LIR verifier / lowering structural invariants (14 L_* codes):
//     L_UnsupportedLoweringForOpcode (MIR→LIR coverage-gap fail-loud),
//     L_RequiredLirOpcodeMissing, L_VirtualRegInPostRegalloc,
//     L_MemOperandMalformed, L_PhysRegOrdinalOutOfRange,
//     L_InvalidSpillSlotSentinel, L_MoveCycleUnsupported,
//     L_IndirectCallUnsupported,
//     L_IndirectCalleeClobberedByArgSetup (FC4 c2 — the indirect-
//     callee/arg-setup collision backstop), L_StackPassedArgUnsupported,
//     L_CcRegLookupFailed, L_VlaDynamicAllocaUnsupported,
//     L_VlaNonLeafFrameUnsupported,
//     L_TerminatorSuccessorMismatch (D-LIR-TEXT-CONDBR-BLOCKREF-OPERANDS-
//     DROPPED — the terminator's recorded successors and its own BlockRef
//     operands disagree).
//     ⚠ THIS ENUMERATION WAS STALE AND SAID "11" WHILE THE ARRAY HELD 13:
//     the two VLA codes were appended to `kUnsuppressableCodes` without
//     being named here, so the header under-reported the band by two for
//     two cycles. Counted from the array, not from this list.
//   - Regalloc Error-severity invariants (3 R_* codes; Info-
//     severity spillage codes intentionally OUT):
//     R_NoCallingConventions, R_CallingConventionLookupFailed,
//     R_VRegHasNoClass.
//   - Assembler / encoding bytes-on-disk invariants (5 A_* codes):
//     A_LirToMirSizeMismatch, A_NoMatchingEncodingVariant,
//     A_RoundTripMismatch, A_NoEncodingDeclared,
//     A_NoEncodingShapeWalker.
//   - AUTHORED ABORT (D-CPP-ERROR-WARNING) — the first and only P_*
//     member, and a DELIBERATE break in the D_/F_/H_/I_/K_/L_/R_/A_/S_
//     family pattern above, so it is spelled out rather than left to
//     look like a stray: P_PreprocessorErrorDirective. Every other
//     entry in this table is a MACHINE-detected invariant (the
//     compiler found something it must not ship). A reached `#error`
//     (C23 6.10.5) is the opposite direction — the SOURCE AUTHOR's own
//     abort, written precisely because the configuration being built
//     is one their code cannot correctly build. Suppressing it hides
//     no compiler opinion; it silently BUILDS the configuration the
//     header author declared invalid, and since the reject is the only
//     thing failing that build, it would do so GREEN. (It fires only
//     when conditional inclusion REACHES the directive — an `#error`
//     in a not-taken branch never emits, so membership costs nothing
//     on the skipped-guard shape that dominates real headers.) Its
//     twin P_PreprocessorWarningDirective (C23 6.10.6) is deliberately
//     NOT a member: translation continues, no wrong bytes ship, and
//     `--suppress` must stay able to silence exactly that advisory
//     class — the S_DeprecatedSymbolUsed / S_UnknownAttribute posture.
//
// Adding a code here is a commitment: this code's emission MUST be visible
// to the build pipeline regardless of any --suppress policy. New entries
// land paired with the fold that introduces the underlying invariant.
//
// Anchored sub-row D-FF2-UNSUPP-INFO-WAE-ASYMMETRY: the silencing-vs-
// elevation refinement gates Warning→Error via warningsAsErrors but
// the elevation arm does NOT promote Info → Warning/Error. If a future
// Info-severity unsuppressable producer lands, --warnings-as-errors
// strict mode would NOT fail-loud on its emission (Info diagnostics
// don't bump errorCount, and the elevation gate keys on severity ==
// Warning). Trigger: first Info-severity entry added to the closed-
// table. Resolution at that point: either extend the elevation gate
// to also promote Info, or harden `unsuppressable_codes.cpp` with a
// consteval check forbidding Info-severity members (requires
// augmenting `kUnsuppressableCodes` into a `{code, severity}` table
// so introspection is compile-time). Today (2026-06-01) no Info-
// severity unsuppressable producer exists; the asymmetry is dormant.
[[nodiscard]] DSS_EXPORT bool
isUnsuppressable(DiagnosticCode code) noexcept;

// Public view of the closed-table for introspection (tests, --help,
// future diagnostic-policy validation at CLI parse time).
[[nodiscard]] DSS_EXPORT std::span<DiagnosticCode const>
unsuppressableCodes() noexcept;

} // namespace dss
