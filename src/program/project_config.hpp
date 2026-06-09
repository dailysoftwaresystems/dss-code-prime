#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Project-config loader + artifact-profile driver enforcement
// (plan 06 AP2). A `.dss-project.json` (plan 06 §2.2) points the
// driver at a language, the desired artifact profile, the targets,
// the source files, and an output hint. `Program::compileProject`
// loads it, enforces the profile against the language's declared set
// (AP1's `GrammarSchema::artifactProfiles()`), then delegates to the
// existing compile path. Threading the resolved profile to codegen is
// AP3/AP4 (D-AP2-COMPILATION-CONTEXT) — AP2 validates + delegates.

namespace dss {

// A parsed `.dss-project.json`. Field semantics (plan 06 §2.2):
//   * `language`        — resolves to a shipped `.lang.json`.
//   * `artifactProfile` — must be ∈ the language's `artifactProfiles[]`.
//   * `targets`         — `<targetName>:<formatName>` specs (the
//                         as-built driver convention; the §2.2 example's
//                         bare-name form + default-format resolution is
//                         deferred, D-AP2-TARGET-NAME-DEFAULT-FORMAT).
//                         Spec FORMAT is validated downstream by the
//                         delegated compile path (D_InvalidTargetSpec).
//   * `sources`         — literal source paths. Glob expansion (e.g.
//                         `src/**/*.c`) is deferred (D-AP2-SOURCES-GLOB);
//                         AP2 routes over the literal file count.
//   * `output`          — artifact output hint. Parsed + type-validated
//                         (a user-authored schema field); its path
//                         ROUTING is deferred (D-AP2-OUTPUT-ROUTING) — AP2
//                         uses the existing per-target output convention.
struct DSS_EXPORT ProjectConfig {
    std::string              language;
    std::string              artifactProfile;
    std::vector<std::string> targets;
    std::vector<std::string> sources;
    std::optional<std::string> output;   // nullopt iff the field is absent
};

// Parse a project config from JSON text. `sourceLabel` names the input
// in diagnostics (typically the file path). Fails loud via `rep`:
//   * malformed JSON / non-object root            → C_MalformedJson
//   * missing `language`/`artifactProfile`        → C_MissingField
//   * `targets`/`sources` missing or empty        → C_MissingField
//   * any field with the wrong JSON type, or an
//     empty string where a non-empty one is
//     required, or an empty array entry           → C_MalformedJson
// Returns nullopt on the FIRST error (a diagnostic was emitted); the
// user fixes and re-runs. Spec-format / path-existence checks are NOT
// done here — they belong to the delegated compile path.
[[nodiscard]] DSS_EXPORT std::optional<ProjectConfig>
parseProjectConfig(std::string_view jsonText,
                   std::string_view sourceLabel,
                   DiagnosticReporter& rep);

// Read + parse a project config from a file path. Emits D_FileNotFound
// if the file cannot be opened, else delegates to parseProjectConfig.
// Returns nullopt on any error.
[[nodiscard]] DSS_EXPORT std::optional<ProjectConfig>
loadProjectConfig(std::filesystem::path const& path,
                  DiagnosticReporter& rep);

// Pure membership predicate: is `profile` in the language's `declared`
// artifact-profile set? Returns false on an EMPTY set — the fail-CLOSED
// reject (plan 06 §2.1 trajectory: a language must declare ≥1 profile to
// be project-buildable). ONE predicate serves both the mismatch and the
// empty-set cases; there is NO per-profile-name branch (agnostic — a
// generic string-set lookup over config vocabulary).
[[nodiscard]] DSS_EXPORT bool
artifactProfileSupported(std::span<std::string const> declared,
                         std::string_view profile) noexcept;

// The AP2 driver gate. Returns true iff `profile` is supported by the
// language's `declared` set. On rejection emits exactly one
// `D_ArtifactProfileNotSupported` — the message discriminates the
// empty-set sub-case ("declares no artifact profiles") from the plain
// mismatch ("not supported … supported: …") — and returns false.
// `language` names the language in the message.
[[nodiscard]] DSS_EXPORT bool
enforceArtifactProfile(std::span<std::string const> declared,
                       std::string_view profile,
                       std::string_view language,
                       DiagnosticReporter& rep);

} // namespace dss
