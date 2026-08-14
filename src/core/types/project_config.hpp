#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/resolve_library_spec.hpp"  // ResolveLibrarySpec (shared
                                                // with the CLI surface)

#include <cstdint>
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
//
// ★ WHY THIS LIVES IN `core` AND NOT IN `program`
// (D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-CONSUMERS, closed 2026-08-13). It was
// born in `src/program/` because the DRIVER was its only reader. Then the LSP
// tier needed the same answer — `src/lsp/workspace_project.cpp` reads a
// workspace's `*.dss-project.json` to learn which assembly dialect a `.s` file
// speaks — and reached UP into `program/` for it. Writing a second reader would
// have been strictly worse (an editor and a compiler that disagree about what a
// manifest MEANS), so the reuse was right and only the LOCATION was wrong:
// `program` already declares `target_link_libraries(program PUBLIC ... lsp)`,
// so the two OBJECT libs referenced each other's headers. Moving the parser
// down to `core` — the tier BOTH depend on — makes the dependency one-way
// again without duplicating a single line of parsing. Nothing here knows what a
// CLI flag, an editor, or a compile pipeline is; it turns manifest TEXT into a
// value type and reports what it could not read.

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
//   * `sources`         — source paths OR glob patterns. The LOADER keeps each
//                         entry VERBATIM (a glob string is a valid non-empty
//                         source string — no filesystem here). `Program::
//                         compileProject` then EXPANDS any entry containing a
//                         glob metacharacter (`* ? [`) against the filesystem
//                         BEFORE the routing count is taken (D-AP2-SOURCES-GLOB,
//                         via `core/types/glob_match.hpp`); a metacharacter-free
//                         entry stays literal. Base = the process working
//                         directory; a zero-match pattern fails loud there.
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
//                         D-FFI-DECLARED-IMPORT-NAME: each entry is EITHER a
//                         plain non-empty STRING (the path; nothing stated —
//                         the byte-for-byte pre-existing form every shipped
//                         manifest uses) OR an extended OBJECT
//                         `{"path": <non-empty>, "importName": <non-empty>}`
//                         that additionally STATES the runtime identity to
//                         record (DT_NEEDED / LC_LOAD_DYLIB / PE import name),
//                         outranking the binary's own embedded soname. The
//                         object form is the manifest spelling of the CLI's
//                         `<path>=<import-name>` suffix — and, having no
//                         separator character, it is also the escape hatch for
//                         a path that itself contains `=`.
//                         `importName` is REQUIRED in the object form: an
//                         object stating nothing is either a typo or noise,
//                         and the plain string says it better. Both an unknown
//                         key inside the object and a missing/empty member
//                         fail loud `C_MalformedJson` — never a silent drop of
//                         the very identity the entry exists to carry.
//   The three flag arrays MERGE (append) onto the Program's current state in
//   `Program::compileProject` — they ADD to any CLI-provided flags, never
//   replace them. A present-but-empty `[]` is allowed (⇒ empty list).

// ── OPTIONAL build-lifecycle script entry (`preBuildScripts` /
//    `postBuildScripts`) ─────────────────────────────────────────────────────
//
// One command to run around the build, declared in the manifest so it travels
// with the project rather than with an invocation.
//
// `run` is an ARGV VECTOR, not a shell command line: `run[0]` is the
// executable and is SPAWNED DIRECTLY, the remaining elements are its arguments
// passed one-for-one. This is a deliberate type choice, not a convenience —
// a single command STRING would have to be split by somebody, and every
// splitter is a different quoting dialect (`cmd.exe` vs POSIX `sh`), which is
// both a portability hole (the same manifest means two different things on two
// hosts) and an injection surface (a path containing a space or a `;` becomes
// executable text). An argv vector has exactly ONE meaning on every host.
//
// `runOn` is the OPTIONAL platform filter, over the `kRunOnPlatformTokens`
// vocabulary in `program/platform_token.hpp` (the single source of truth for
// the token set, shared with `currentHostOs()` so the manifest's spelling and
// the host probe's spelling cannot drift). ABSENT ⇒ the script runs on EVERY
// platform — the unfiltered default. PRESENT ⇒ it must list at least one
// token: an empty `[]` would mean "run nowhere", which is what DELETING the
// entry says, more clearly, so the degenerate spelling rejects rather than
// aliasing (same rule + rationale as the `resolveLibraries` object form).
//
// The LOADER neither spawns nor resolves anything — see the pure-parser note
// on `parseProjectConfig`. It parses and validates the SHAPE only.
struct DSS_EXPORT ScriptEntry {
    std::vector<std::string> run;     // argv; run[0] is the executable
    std::vector<std::string> runOn;   // empty ⇒ member absent ⇒ every platform
    friend bool operator==(ScriptEntry const&, ScriptEntry const&) = default;
};

// ── OPTIONAL project dependency entry (`dependsOn`) ──────────────────────────
//
// One prerequisite project, named by EXACTLY ONE source:
//   * `path` — a LOCAL checkout, e.g. `"../libfoo"`;
//   * `git`  — a REMOTE, e.g. `"git@github.com:org/bar.git"`, optionally
//              pinned to a `ref` (branch / tag / commit).
// Both stated ⇒ reject (two sources for one dependency is ambiguous, and
// silently preferring one would make the other a no-op the user cannot see).
// Neither stated ⇒ reject (an entry naming nothing is noise).
// `ref` without `git` ⇒ reject: a ref names a revision inside a git remote and
// has no meaning for a local path, so accepting it would silently ignore the
// pin the user wrote — the exact class of silent drop this loader exists to
// prevent.
//
// `path` is kept VERBATIM, exactly as `sources[]` is: no filesystem access, no
// normalization, no resolution here (a different lane resolves them).
struct DSS_EXPORT DependencyEntry {
    std::optional<std::string> path;  // nullopt iff the member is absent
    std::optional<std::string> git;   // nullopt iff the member is absent
    std::optional<std::string> ref;   // nullopt iff the member is absent
    friend bool operator==(DependencyEntry const&, DependencyEntry const&) = default;
};

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
    // → setResolveLibraries (CLI --resolve-library <path>[=<import-name>]).
    // The SAME type the CLI parses into, so the declared import name cannot be
    // lost at the manifest/CLI join where the two sources merge.
    std::vector<ResolveLibrarySpec> resolveLibraries;
    // OPTIONAL per-PROGRAM stack reserve in BYTES (manifest key
    // `stackReserve`; D-SQLITE-PE64-FULL-TIER-STACK-DEPTH). nullopt iff the
    // field is absent ⇒ the object format's declared default stands.
    //
    // Unlike the three arrays above, this is a SCALAR and therefore cannot
    // merge — so it has an explicit PRECEDENCE rule instead: the CLI
    // `--stack-reserve` WINS; the manifest value applies only when the CLI
    // supplied none (`Program::compileProject`). Rationale: an explicit
    // command-line argument overriding a committed file is the universal
    // convention, and it is the only rule that lets a user probe a different
    // reserve without editing (and risking committing) the manifest.
    std::optional<std::uint64_t> stackReserveBytes;
    // OPTIONAL build-lifecycle hooks + project prerequisites. ALL THREE are
    // absent ⇒ EMPTY, never an error — a manifest that declares no scripts and
    // no dependencies is the overwhelmingly common case and must stay silent.
    // Shape-validated here only; SPAWNING the scripts and RESOLVING the
    // dependencies belong to their own lanes (this loader is a pure parser).
    std::vector<ScriptEntry>     preBuildScripts;   // run BEFORE the build
    std::vector<ScriptEntry>     postBuildScripts;  // run AFTER the build
    std::vector<DependencyEntry> dependsOn;         // prerequisite projects
};

// Parse a project config from JSON text. `sourceLabel` names the input
// in diagnostics (typically the file path). Fails loud via `rep`:
//   * malformed JSON / non-object root            → C_MalformedJson
//   * missing `language`/`artifactProfile`        → C_MissingField
//   * `targets`/`sources` missing or empty        → C_MissingField
//   * any field with the wrong JSON type, or an
//     empty string where a non-empty one is
//     required, or an empty array entry           → C_MalformedJson
//   * an UNKNOWN top-level key                    → C_MalformedJson
//   * a malformed `preBuildScripts` /
//     `postBuildScripts` / `dependsOn` entry      → C_MalformedJson
// Returns nullopt on the FIRST error (a diagnostic was emitted); the
// user fixes and re-runs. Spec-format / path-existence checks are NOT
// done here — they belong to the delegated compile path.
//
// A top-level key beginning with `$` (`$comment`, `$…Comment`) is PROSE and is
// SKIPPED by the unknown-key check — the codebase-wide documentation-key
// convention, via `dss::detail::isDocumentationKey`. This CLOSES
// D-AP2-PROJECT-MANIFEST-REJECTS-COMMENT-KEY; see the note at the check itself.
//
// ★ PURE JSON PARSER. This function touches NO filesystem, spawns NO process,
// and resolves NO path. Globs, script argv words, and dependency paths are
// kept VERBATIM exactly as `sources[]` already is; the lanes that expand,
// spawn, and resolve them own those decisions with the context (working
// directory, host OS, output dir) that this function deliberately lacks.
[[nodiscard]] DSS_EXPORT std::optional<ProjectConfig>
parseProjectConfig(std::string_view jsonText,
                   std::string_view sourceLabel,
                   DiagnosticReporter& rep);

// ── the CLOSED top-level key vocabulary, exported ────────────────────────────
//
// The recognized top-level manifest keys, in the order the unknown-key
// diagnostic lists them, and that same list comma-joined for a message.
//
// WHY THESE ARE EXPORTED AT ALL: the recognized-field list used to be
// hand-written TWICE — once as the `kKnownKeys` table the check loops over and
// once as a literal string inside the "unknown field" message — with nothing
// asserting the two agreed. That is a drift hazard with a nasty failure mode:
// the SILENT direction is a key present in the table but missing from the
// message, which leaves a user staring at a reject whose remediation list omits
// the very field they need. `projectConfigKnownKeyList()` now DERIVES the
// message text from the table (the `registeredArtifactProfileList()` pattern in
// `core/types/artifact_profile.hpp`), so the two cannot disagree, and
// `projectConfigKnownKeys()` lets the test pin the message against the REAL
// table instead of a re-typed copy — re-typing the list in a test would just
// recreate the drift one layer out.
[[nodiscard]] DSS_EXPORT std::span<std::string_view const>
projectConfigKnownKeys() noexcept;

[[nodiscard]] DSS_EXPORT std::string projectConfigKnownKeyList();

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
