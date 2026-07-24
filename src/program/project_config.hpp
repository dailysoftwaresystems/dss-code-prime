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
//   * `artifactName`    — OPTIONAL base NAME for the emitted binary (no
//                         extension, no path separators — a name, not a path).
//                         Absent ⇒ the source stem (unchanged). A project build
//                         routes each target's artifact to `<outputDir>/
//                         <formatName>/<artifactName-or-stem><ext>` — the
//                         per-platform subdir applies to EVERY project build
//                         (single- or multi-target). This is the artifactName +
//                         subdir half of D-AP2-OUTPUT-ROUTING (the
//                         `output`-field-as-output-dir half stays deferred).
//   * `includes`        — OPTIONAL quote-include search dirs; the file-driven
//                         counterpart of the CLI `-I <dir>`. Empty when absent.
//   * `defines`         — OPTIONAL `NAME[=VALUE]` macros; the counterpart of
//                         the CLI `--define`. Empty when absent.
//   * `resolveLibraries`— OPTIONAL library paths whose export surfaces resolve
//                         this build's externs; the counterpart of the CLI
//                         `--resolve-library <path>`. Empty when absent.
//   The three flag arrays MERGE (append) onto the Program's current state in
//   `Program::compileProject` — they ADD to any CLI-provided flags, never
//   replace them. A present-but-empty `[]` is allowed (⇒ empty list).
struct DSS_EXPORT ProjectConfig {
    std::string              language;
    std::string              artifactProfile;
    std::vector<std::string> targets;
    std::vector<std::string> sources;
    std::optional<std::string> output;   // nullopt iff the field is absent
    // OPTIONAL base NAME for the emitted binary (no extension / path
    // separators). nullopt ⇒ the source stem. In a project build each target's
    // artifact routes to `<outputDir>/<formatName>/<artifactName-or-stem><ext>`
    // (the artifactName + per-format-subdir half of D-AP2-OUTPUT-ROUTING).
    std::optional<std::string> artifactName;  // nullopt iff the field is absent
    // OPTIONAL compile-flag fields (empty when the field is absent). Each maps
    // to the same Program state as the matching CLI flag; see the field notes
    // above. Threaded (merge/append) by Program::compileProject.
    std::vector<std::string> includes;         // → setIncludeDirs      (CLI -I <dir>)
    std::vector<std::string> defines;          // → setUserDefines      (CLI --define NAME[=VALUE])
    std::vector<std::string> resolveLibraries; // → setResolveLibraries (CLI --resolve-library <path>)
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

// Pure membership predicate: is `profile` in a `declared` artifact-profile
// set? Returns false on an EMPTY set — the fail-CLOSED reject. ONE generic
// predicate serves BOTH callers (no per-profile-name branch; a string-set
// lookup over config vocabulary):
//   * the LANGUAGE set (AP1, `grammar.artifactProfiles()`) — which profiles
//     the language SUPPORTS (via `enforceArtifactProfile`);
//   * the FORMAT set (AP3, `format.artifactProfiles()`) — which profiles the
//     object format SERVES (via `enforceArtifactProfileFormat`).
// Empty-set ⇒ false aligns both: a language that declares no profiles isn't
// project-buildable; a format that serves no profiles can't be targeted.
[[nodiscard]] DSS_EXPORT bool
artifactProfileSupported(std::span<std::string const> declared,
                         std::string_view profile) noexcept;

// The AP2 driver gate (LANGUAGE side). Returns true iff `profile` is in the
// language's `declared` set. On rejection emits exactly one
// `D_ArtifactProfileNotSupported` — the message discriminates the empty-set
// sub-case ("declares no artifact profiles") from the plain mismatch
// ("not supported … supported: …") — and returns false. `language` names
// the language in the message.
[[nodiscard]] DSS_EXPORT bool
enforceArtifactProfile(std::span<std::string const> declared,
                       std::string_view profile,
                       std::string_view language,
                       DiagnosticReporter& rep);

// The AP3 driver gate (FORMAT side). Returns true iff `profile` is SERVED by
// the chosen object format's `served` set. On rejection emits exactly one
// `D_ArtifactProfileFormatMismatch` — the message names the format + its
// served set (empty-set discriminated as "serves no artifact profiles") —
// and returns false. `formatName` names the object format in the message.
// Calls the SAME `artifactProfileSupported` predicate as the language gate;
// only the diagnostic code + message differ (remediation-distinct).
[[nodiscard]] DSS_EXPORT bool
enforceArtifactProfileFormat(std::span<std::string const> served,
                             std::string_view profile,
                             std::string_view formatName,
                             DiagnosticReporter& rep);

} // namespace dss
