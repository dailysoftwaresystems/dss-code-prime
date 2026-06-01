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
// Post-fold: `applyPolicy` consults `isUnsuppressable(d.code)` BEFORE
// the suppress check; unsuppressable codes pass through unchanged
// (also bypass `overrides` severity-demotion + `warningsAsErrors`, both
// of which would defeat the gate in different ways). Post-fold #11
// silent-failure F1: `report()` itself ALSO bypasses cap / dedup /
// maxPerCode / maxDiagnostics for unsuppressable codes — the four
// silent-drop gates around applyPolicy would otherwise re-open the
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
//   - LIR verifier / lowering structural invariants (9 L_* codes):
//     L_RequiredLirOpcodeMissing, L_VirtualRegInPostRegalloc,
//     L_MemOperandMalformed, L_PhysRegOrdinalOutOfRange,
//     L_InvalidSpillSlotSentinel, L_MoveCycleUnsupported,
//     L_IndirectCallUnsupported, L_StackPassedArgUnsupported,
//     L_CcRegLookupFailed.
//   - Regalloc Error-severity invariants (3 R_* codes; Info-
//     severity spillage codes intentionally OUT):
//     R_NoCallingConventions, R_CallingConventionLookupFailed,
//     R_VRegHasNoClass.
//   - Assembler / encoding bytes-on-disk invariants (5 A_* codes):
//     A_LirToMirSizeMismatch, A_NoMatchingEncodingVariant,
//     A_RoundTripMismatch, A_NoEncodingDeclared,
//     A_NoEncodingShapeWalker.
//
// Adding a code here is a commitment: this code's emission MUST be visible
// to the build pipeline regardless of any --suppress policy. New entries
// land paired with the fold that introduces the underlying invariant.
[[nodiscard]] DSS_EXPORT bool
isUnsuppressable(DiagnosticCode code) noexcept;

// Public view of the closed-table for introspection (tests, --help,
// future diagnostic-policy validation at CLI parse time).
[[nodiscard]] DSS_EXPORT std::span<DiagnosticCode const>
unsuppressableCodes() noexcept;

} // namespace dss
