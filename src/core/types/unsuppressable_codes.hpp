#pragma once

#include "core/export.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <span>

namespace dss {

// D-FF2-4: closed-table of DiagnosticCodes whose emission MUST reach the
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
// of which would defeat the gate in different ways).
//
// Membership tiers (informational):
//   - Permanent architectural exclusions (suppressing → wrong-machine-code
//     surface):
//     `D_TargetAbiModelUnsupportedByDriver`, `F_FfiIngestAbiModelUnsupported`,
//     `F_FfiIngestEmptyCanonical`, `H_ExternHasInitializer`.
//   - Pending-plan announcements (suppressing → misleading "why did my
//     build fail?" UX): `D_PlanNotLanded`.
//   - Lowering / verifier structural invariants (cannot reach codegen):
//     `H_UnsupportedLoweringForKind`, `H_VerifierFailure`, `H_TypeUnresolved`,
//     `I_VerifierFailure`, `I_NoEntryBlock`, `I_MultipleEntryBlocks`,
//     `I_EntryBlockNotFirst`, `I_NotDominated`.
//   - Linker fail-loud (image refused / undefined symbol):
//     `K_SymbolUndefined`, `K_ImageNotOk`.
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
