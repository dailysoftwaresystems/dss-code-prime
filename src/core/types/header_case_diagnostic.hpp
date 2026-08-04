#pragma once

// D-PP-HEADER-CASE-INSENSITIVE-PE — the ONE way a header-name fold COLLISION is
// reported, shared by every tier that can detect one.
//
// ★ WHY THIS IS A SHARED FUNCTION AND NOT SIX INLINE `emit` CALLS. The
// collision is detectable from six places (the import resolver's angle +
// quote arms, the preprocessor's angle `#include` arm, both authoritative
// `__has_include` arms, and the `#embed` directive) and the FIRST cut of this
// change open-coded the emit at each one. Two consequences, both real:
//   * the wording and the CODE could drift between tiers, so a user hitting
//     the same tree defect through `#include` and through `__has_include`
//     would get two different-looking failures; and
//   * a fold collision CANNOT BE CONSTRUCTED ON NTFS OR A DEFAULT APFS VOLUME
//     (the two names cannot coexist), so on the Windows gate leg every one of
//     those six sites was unreachable and therefore UNTESTED. One shared
//     function is one thing to test, and `makeHeaderCaseAmbiguityDiagnostic`
//     is callable with a synthesized verdict on ANY host — which is how the
//     Windows leg stops being blind about the code, the severity and the
//     "names every candidate" contract.
//
// The reporting tiers (`reportDriver` in the import resolver, `emitPP` in the
// preprocessor) are byte-identical field-fillers over `DiagnosticReporter`, so
// one helper serves both without either giving anything up.

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace dss {

// Render the collision as a diagnostic message body: the requested spelling
// plus EVERY colliding on-disk path, one per line, in the order given (the
// resolver sorts them, so the text is reproducible run to run).
[[nodiscard]] DSS_EXPORT std::string
headerCaseAmbiguityMessage(std::string_view                       requested,
                           std::span<std::filesystem::path const> candidates);

// Build the `F_HeaderNameCaseAmbiguous` diagnostic. Exposed separately from
// the reporting call so a test can assert the CODE, the SEVERITY and the
// message contents on a host where no colliding directory can exist.
[[nodiscard]] DSS_EXPORT ParseDiagnostic
makeHeaderCaseAmbiguityDiagnostic(BufferId buffer, SourceSpan span,
                                  std::string_view requested,
                                  std::span<std::filesystem::path const> candidates);

// Emit it. Every tier calls THIS — never a hand-rolled `emit`/`report` with
// the code spelled out again.
DSS_EXPORT void
reportHeaderCaseAmbiguity(DiagnosticReporter& reporter, BufferId buffer,
                          SourceSpan span, std::string_view requested,
                          std::span<std::filesystem::path const> candidates);

} // namespace dss
