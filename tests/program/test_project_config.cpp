// Plan 06 AP2 — project-config loader + artifact-profile driver
// enforcement. Pins:
//   * Loader (parseProjectConfig): valid parse (with/without `output`),
//     each required-field failure (missing / empty / wrong-type), array
//     validation, malformed JSON, non-object root.
//   * Predicate (artifactProfileSupported): in-set true, not-in-set
//     false, and the EMPTY-SET fail-closed (false).
//   * Driver gate (enforceArtifactProfile): accept emits nothing; the
//     unsupported case AND the empty-set case both emit exactly one
//     D_ArtifactProfileNotSupported.  ← the RED-on-disable levers: make
//     artifactProfileSupported() always return true and the
//     Unsupported*/EmptySet*/CRejectsGui pins all go green.
//   * Integration (real shipped c grammar): cli accepted, gui
//     rejected — proves the real grammar's declared span flows through.
//   * Routing (routesToMultiUnit): the shared >1 threshold.
//   * Diagnostic-code name/prefix round-trip.
//
// Plan 06 AP4 — per-language ONBOARDING matrix (appended at the end).
// AP2/AP3 exercised the gates with c ONLY; AP4 completes the
// matrix across ALL THREE shipped languages (toy / c / tsql-
// subset) and adds the single real end-to-end emit through
// compileProject. See the "AP4" banner below for the scope + honesty
// notes (no shipped format serves a non-cli profile; the profile does
// NOT yet drive artifact shape — that is D-AP2-COMPILATION-CONTEXT).

#include "core/substrate/path_identity.hpp"   // genericSpelling — the lossless spelling
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/glob_match.hpp"          // D-AP2-SOURCES-GLOB: matcher + expander
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/project_config.hpp"  // the loader under test (moved from
                                          // `program/` — see the header's own
                                          // note on why it lives in `core`)
#include "link/object_format_schema.hpp"  // ObjectFormatSchema::loadShipped (AP3 format-gate integration)
#include "program/program.hpp"          // Program, routesToMultiUnit
#include "program/project_sources.hpp"  // AP6 M4: expandAndDedupProjectSources
#include "host_native_target.hpp"       // hostNativeTarget (build-and-spawn on EVERY leg)
#include "run_binary.hpp"               // runBinary (behavioral exit-code proof)
#include "scratch_dir.hpp"
#include "unc_spelling.hpp"             // the ONE multi-separator-root fixture

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>                       // std::snprintf (JSON control-char escaping)
#include <cstdlib>                      // std::atoi (the build-hook fixture's exit-code arg)
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace dss;

namespace {

std::size_t countCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code == code) ++n;
    }
    return n;
}

bool sawCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    return countCode(rep, code) > 0;
}

// First human message carried for `code` (stored in `actual` by the
// `report()` shim) — for the §7-#3 actionable-message pin and the
// loader's unknown-key "recognized fields" pin.
std::string firstMessageForCode(DiagnosticReporter const& rep,
                                DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return d.actual;
    }
    return {};
}

std::span<std::string const> asSpan(std::vector<std::string> const& v) {
    return std::span<std::string const>{v};
}

// A complete, well-formed project config.
constexpr std::string_view kValidJson = R"({
  "language": "c",
  "artifactProfile": "cli",
  "targets": ["x86_64:elf64-x86_64-linux-exec"],
  "sources": ["main.c"],
  "output": "dist/myprog"
})";

} // namespace

// ── Loader: happy path ─────────────────────────────────────────

TEST(ProjectConfigLoader, ValidConfigParsesAllFields) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(kValidJson, "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(pc->language, "c");
    EXPECT_EQ(pc->artifactProfile, "cli");
    ASSERT_EQ(pc->targets.size(), 1u);
    EXPECT_EQ(pc->targets[0], "x86_64:elf64-x86_64-linux-exec");
    ASSERT_EQ(pc->sources.size(), 1u);
    EXPECT_EQ(pc->sources[0], "main.c");
    ASSERT_TRUE(pc->output.has_value());
    EXPECT_EQ(*pc->output, "dist/myprog");
}

TEST(ProjectConfigLoader, ValidConfigWithoutOutputParses) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["a.c", "b.c"]
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(pc->output.has_value());
    EXPECT_EQ(pc->sources.size(), 2u);
}

// ── Loader: required-field failures (C_MissingField) ────────────

TEST(ProjectConfigLoader, MissingLanguageFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "artifactProfile": "cli", "targets": ["t:f"], "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 1u);
}

TEST(ProjectConfigLoader, MissingArtifactProfileFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "targets": ["t:f"], "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 1u);
}

TEST(ProjectConfigLoader, MissingTargetsFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli", "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 1u);
}

TEST(ProjectConfigLoader, EmptyTargetsFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": [], "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 1u);
}

TEST(ProjectConfigLoader, MissingSourcesFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli", "targets": ["t:f"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 1u);
}

TEST(ProjectConfigLoader, EmptySourcesFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": []
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 1u);
}

TEST(ProjectConfigLoader, EmptyLanguageStringFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 1u);
}

// ── Loader: malformed / wrong-type (C_MalformedJson) ────────────

TEST(ProjectConfigLoader, MalformedJsonFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig("{ not valid json ", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, NonObjectRootFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig("[1, 2, 3]", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, WrongTypeLanguageFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": 42, "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, WrongTypeTargetsFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": "not-an-array", "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, NonStringTargetEntryFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": [42], "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, WrongTypeOutputFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "output": 42
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, EmptyOutputStringFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "output": ""
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// ── Loader: OPTIONAL `artifactName` (Cycle B — D-AP2-OUTPUT-ROUTING) ─────
// A bare NAME for the emitted binary (no extension / path separators). Absent
// ⇒ nullopt (the source stem names the artifact). Present must be a non-empty
// string with no `/` or `\` (else C_MalformedJson).

TEST(ProjectConfigLoader, ArtifactNamePresentParses) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"],
      "artifactName": "myapp"
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_TRUE(pc->artifactName.has_value());
    EXPECT_EQ(*pc->artifactName, "myapp");
}

TEST(ProjectConfigLoader, ArtifactNameAbsentIsNullopt) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"]
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(pc->artifactName.has_value());
}

TEST(ProjectConfigLoader, EmptyArtifactNameFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "artifactName": ""
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, WrongTypeArtifactNameFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "artifactName": 42
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// A path-like value is rejected — artifactName is a bare NAME, not a path
// (the directory comes from `--output` + the per-format subdir). Both
// separators are rejected so it never silently escapes the routed output tree.
TEST(ProjectConfigLoader, ArtifactNameWithForwardSlashFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "artifactName": "foo/bar"
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, ArtifactNameWithBackslashFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "artifactName": "a\\b"
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// ── Loader: OPTIONAL compile-flag arrays (includes/defines/resolveLibraries)
// The file-driven counterparts of the CLI `-I` / `--define` /
// `--resolve-library`. Absent ⇒ empty; a present `[]` ⇒ empty (no error);
// a present value must be an array of non-empty strings (else C_MalformedJson).

// Populated → each field parses EXACTLY (sizes AND contents).
TEST(ProjectConfigLoader, FlagArraysPopulatedParseExactly) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"],
      "includes": ["inc", "vendor/include"],
      "defines": ["NDEBUG", "MAX=64"],
      "resolveLibraries": ["libfoo.so", "libbar.a"]
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(pc->includes.size(), 2u);
    EXPECT_EQ(pc->includes[0], "inc");
    EXPECT_EQ(pc->includes[1], "vendor/include");
    ASSERT_EQ(pc->defines.size(), 2u);
    EXPECT_EQ(pc->defines[0], "NDEBUG");
    EXPECT_EQ(pc->defines[1], "MAX=64");
    ASSERT_EQ(pc->resolveLibraries.size(), 2u);
    EXPECT_EQ(pc->resolveLibraries[0].path, "libfoo.so");
    EXPECT_EQ(pc->resolveLibraries[1].path, "libbar.a");
    // D-FFI-DECLARED-IMPORT-NAME: a PLAIN string entry states NOTHING — the
    // byte-for-byte pre-existing meaning every shipped manifest relies on.
    EXPECT_TRUE(pc->resolveLibraries[0].declaredImportName.empty());
    EXPECT_TRUE(pc->resolveLibraries[1].declaredImportName.empty());
}

// Each field ABSENT → empty vector (the default; no error).
TEST(ProjectConfigLoader, FlagArraysAbsentAreEmpty) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"]
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_TRUE(pc->includes.empty());
    EXPECT_TRUE(pc->defines.empty());
    EXPECT_TRUE(pc->resolveLibraries.empty());
}

// A present-but-empty `[]` → empty vector, NO diagnostic (the one allowed
// difference from a REQUIRED array, which rejects `[]` as C_MissingField).
TEST(ProjectConfigLoader, FlagArraysEmptyBracketsAllowedNoDiagnostic) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"],
      "includes": [], "defines": [], "resolveLibraries": []
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_TRUE(pc->includes.empty());
    EXPECT_TRUE(pc->defines.empty());
    EXPECT_TRUE(pc->resolveLibraries.empty());
}

// REJECT: a non-array value (a string where an array is required).
TEST(ProjectConfigLoader, NonArrayIncludesFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "includes": "not-an-array"
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// REJECT: a non-string entry (a number in the array).
TEST(ProjectConfigLoader, NonStringDefineEntryFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "defines": [1]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// REJECT: an empty-string entry (fails loud like the required-array helper).
TEST(ProjectConfigLoader, EmptyStringIncludeEntryFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "includes": [""]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// REJECT: a non-array resolveLibraries (the third field, same rule).
TEST(ProjectConfigLoader, NonArrayResolveLibrariesFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "resolveLibraries": 42
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// ── D-FFI-DECLARED-IMPORT-NAME: the extended `resolveLibraries` entry ────────
//
// Each entry is EITHER a plain path string (nothing stated) OR an object
// `{"path", "importName"}` that additionally STATES the runtime identity to
// record — the manifest spelling of the CLI's `<path>=<import-name>` suffix,
// and (having no separator character) the escape hatch for a path containing
// `=`. The two forms MIX freely in one array.

TEST(ProjectConfigLoader, ResolveLibrariesMixedPlainAndExtendedEntriesParse) {
    // The plain form must survive BYTE-FOR-BYTE alongside the new object form
    // — many shipped manifests are all-plain, and a loader that only accepted
    // one shape (or that quietly rewrote the other) would break them.
    // RED-ON-DISABLE: revert the entry reader to the plain-string-only helper
    // and the object entry fails C_MalformedJson instead of parsing.
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["arm64:macho64-arm64-darwin-exec"], "sources": ["main.c"],
      "resolveLibraries": [
        "libplain.so",
        {"path": "/opt/local/lib/libtcl8.6.dylib",
         "importName": "@rpath/libtcl8.6.dylib"},
        {"path": "/weird/dir=name/libz.dylib", "importName": "/usr/lib/libz.1.dylib"}
      ]
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(pc->resolveLibraries.size(), 3u);

    EXPECT_EQ(pc->resolveLibraries[0].path, "libplain.so");
    EXPECT_TRUE(pc->resolveLibraries[0].declaredImportName.empty())
        << "a plain string states NOTHING";

    EXPECT_EQ(pc->resolveLibraries[1].path, "/opt/local/lib/libtcl8.6.dylib");
    EXPECT_EQ(pc->resolveLibraries[1].declaredImportName,
              "@rpath/libtcl8.6.dylib");

    // The object form has no separator, so a path containing `=` round-trips
    // intact — the very case the CLI's last-`=` split cannot express.
    EXPECT_EQ(pc->resolveLibraries[2].path, "/weird/dir=name/libz.dylib");
    EXPECT_EQ(pc->resolveLibraries[2].declaredImportName, "/usr/lib/libz.1.dylib");
}

TEST(ProjectConfigLoader, ResolveLibrariesExtendedEntryMissingImportNameFailsLoud) {
    // The object form exists SOLELY to state an identity, so an object without
    // one is a typo or noise and the plain string says it better. One spelling
    // per meaning — the degenerate variant rejects rather than aliasing.
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "resolveLibraries": [{"path": "libfoo.so"}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, ResolveLibrariesExtendedEntryMissingPathFailsLoud) {
    // An identity for NO file reads no export surface at all.
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "resolveLibraries": [{"importName": "libfoo.so.1"}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, ResolveLibrariesExtendedEntryEmptyMemberFailsLoud) {
    // An EMPTY importName would record a DT_NEEDED / LC_LOAD_DYLIB the loader
    // can never resolve: a link that succeeds and an artifact that dies at
    // load. Same rule as the plain form's empty-string reject.
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "resolveLibraries": [{"path": "libfoo.so", "importName": ""}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, ResolveLibrariesExtendedEntryUnknownMemberFailsLoud) {
    // Same rule + same rationale as the top-level unknown-key gate: a mistyped
    // `"importname"` would otherwise SILENTLY DROP the identity the entry
    // exists to state, and the build would link against the stand-in's own
    // embedded soname with no diagnostic at all.
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "resolveLibraries": [{"path": "libfoo.so", "importname": "libfoo.so.1"}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, ResolveLibrariesNonStringNonObjectEntryFailsLoud) {
    // Neither shape ⇒ reject. Never a silent skip of the entry.
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "resolveLibraries": [42]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, ResolveLibrariesEmptyPlainStringEntryFailsLoud) {
    // The plain form's own degenerate case, unchanged by the extension.
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "resolveLibraries": [""]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// Regression: an UNKNOWN top-level key is STILL rejected after the three
// new recognized fields were added to kKnownKeys (a typo must not slip in
// alongside the new vocabulary).
TEST(ProjectConfigLoader, UnknownKeyStillRejected) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "includ": ["x"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// ── Loader: OPTIONAL `stackReserve` (D-SQLITE-PE64-FULL-TIER-STACK-DEPTH) ──
// A per-PROGRAM stack-reserve request in BYTES — the file-driven twin of the
// CLI `--stack-reserve`. Absent ⇒ nullopt (the object format's declared
// default stands). Present is validated for SHAPE ONLY: a POSITIVE unsigned
// JSON integer (so a negative, a float, a string, and 0 each fail
// C_MalformedJson). RANGE + ALIGNMENT belong to the linker gate, against the
// bounds the chosen FORMAT declares — deliberately not decided here.

TEST(ProjectConfigLoader, StackReservePresentParsesExactValue) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"],
      "stackReserve": 4194304
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_TRUE(pc->stackReserveBytes.has_value());
    EXPECT_EQ(*pc->stackReserveBytes, std::uint64_t{4194304});   // 4 MiB
}

// A >4 GiB value must round-trip EXACTLY (2^33 does not fit in a 32-bit
// `unsigned`): a narrowing of the field or of the `get<>` target would
// truncate the request silently — the exact-value compare is the lever.
TEST(ProjectConfigLoader, StackReserveAboveFourGiBRoundTripsExactly) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"],
      "stackReserve": 8589934592
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_TRUE(pc->stackReserveBytes.has_value());
    EXPECT_EQ(*pc->stackReserveBytes, std::uint64_t{8589934592});   // 2^33
}

TEST(ProjectConfigLoader, StackReserveAbsentIsNullopt) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"]
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(pc->stackReserveBytes.has_value())
        << "absent ⇒ nullopt; the object format's declared default stands";
}

// 0 is a shape-legal unsigned integer but cannot start a program — rejected
// explicitly (never silently normalized to "no request").
TEST(ProjectConfigLoader, ZeroStackReserveFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "stackReserve": 0
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(rep.errorCount(), 1u) << "exactly one Error, no diagnostic noise";
}

// A negative must NOT wrap into a huge u64 — `is_number_unsigned()` types
// `-4096` as a SIGNED integer, so it is rejected before any `get<uint64_t>`.
TEST(ProjectConfigLoader, NegativeStackReserveFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "stackReserve": -4096
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(rep.errorCount(), 1u);
}

// A float is not a byte count — rejected rather than truncated toward 4096.
TEST(ProjectConfigLoader, FloatStackReserveFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "stackReserve": 4096.5
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(rep.errorCount(), 1u);
}

// A STRING (the natural typo — every other manifest field is a string) must
// reject, never be coerced by a lenient parse.
TEST(ProjectConfigLoader, StringStackReserveFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "stackReserve": "4096"
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(rep.errorCount(), 1u);
}

// The unknown-key guard STILL fails loud after `stackReserve` joined the
// closed key set — a TYPO on the new key ("stackReserv") must not slip in
// beside it — AND the "recognized fields" remediation list must NAME the new
// key (a key added to kKnownKeys but not to the message leaves the user with
// a reject they cannot act on).
TEST(ProjectConfigLoader, UnknownKeyStillRejectedAndMessageNamesStackReserve) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "stackReserv": 4194304
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    ASSERT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    std::string const m =
        firstMessageForCode(rep, DiagnosticCode::C_MalformedJson);
    EXPECT_NE(m.find("stackReserv'"), std::string::npos)
        << "the message must quote the offending typo; got: " << m;
    EXPECT_NE(m.find("stackReserve"), std::string::npos)
        << "the recognized-fields list must name the new key; got: " << m;
}

// ════════════════════════════════════════════════════════════════════════════
// LOADER TIER — OPTIONAL `preBuildScripts` / `postBuildScripts` / `dependsOn`,
// the `$`-documentation carve-out, and the drift-proof recognized-field pin.
//
// Scope note: these are PARSER pins only. The loader spawns nothing, resolves
// nothing, and touches no filesystem — argv words and dependency paths are
// kept VERBATIM exactly as `sources[]` is. The lanes that spawn/resolve own
// those behaviours and their own tests.
//
// Every fail-loud path below carries the SAME three-sided assertion:
//   (a) the expected code is present EXACTLY once,
//   (b) the other plausible code (C_MissingField — these are all *Malformed*
//       shapes, not absent required fields) is ABSENT, and
//   (c) the TOTAL error count is exactly one — no diagnostic noise, and no
//       second reject riding along that would mask a wrong first one.
// ════════════════════════════════════════════════════════════════════════════

// ── `preBuildScripts` / `postBuildScripts` — happy paths ────────────────────

// FULL STRUCT EQUALITY, not "it parsed": the argv words must survive in ORDER
// and VERBATIM (a loader that re-split, trimmed, or shell-quoted them would
// still "parse"), and `runOn` must be empty exactly when the member is absent.
// RED-ON-DISABLE for the whole feature: delete the two readOptionalScripts
// calls in parseProjectConfig and this pin goes red on empty vectors.
TEST(ProjectConfigLoader, ScriptArraysPopulatedParseToExactStructs) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"],
      "preBuildScripts": [
        {"run": ["bash", "gen.sh"], "runOn": ["linux", "darwin"]},
        {"run": ["python3", "-c", "one argv word, spaces and all"]}
      ],
      "postBuildScripts": [
        {"run": ["pwsh", "-File", "pack.ps1"], "runOn": ["windows"]}
      ]
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);

    std::vector<ScriptEntry> const expectedPre = {
        ScriptEntry{{"bash", "gen.sh"}, {"linux", "darwin"}},
        // `runOn` ABSENT ⇒ empty ⇒ runs on EVERY platform (the unfiltered
        // default). The argv element containing spaces and quotes must arrive
        // as ONE word — the whole reason `run` is a vector and not a string.
        ScriptEntry{{"python3", "-c", "one argv word, spaces and all"}, {}},
    };
    EXPECT_EQ(pc->preBuildScripts, expectedPre);

    std::vector<ScriptEntry> const expectedPost = {
        ScriptEntry{{"pwsh", "-File", "pack.ps1"}, {"windows"}},
    };
    EXPECT_EQ(pc->postBuildScripts, expectedPost);
}

// All three new fields ABSENT ⇒ EMPTY, never an error. This is the shape of
// essentially every manifest in the tree, so it must stay perfectly silent.
TEST(ProjectConfigLoader, ScriptAndDependencyFieldsAbsentAreEmptyNotAnError) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(kValidJson, "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(pc->preBuildScripts, std::vector<ScriptEntry>{});
    EXPECT_EQ(pc->postBuildScripts, std::vector<ScriptEntry>{});
    EXPECT_EQ(pc->dependsOn, std::vector<DependencyEntry>{});
}

// A present-but-empty `[]` ⇒ empty, NO diagnostic — declaring no hooks and no
// dependencies explicitly is not a mistake (same allowance as the flag arrays).
TEST(ProjectConfigLoader, ScriptAndDependencyEmptyBracketsAllowedNoDiagnostic) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "preBuildScripts": [], "postBuildScripts": [], "dependsOn": []
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(pc->preBuildScripts, std::vector<ScriptEntry>{});
    EXPECT_EQ(pc->postBuildScripts, std::vector<ScriptEntry>{});
    EXPECT_EQ(pc->dependsOn, std::vector<DependencyEntry>{});
}

// ── `preBuildScripts` / `postBuildScripts` — fail-loud paths ────────────────

// A hook with no command is not a hook. RED-ON-DISABLE: drop the
// `e.contains("run")` guard and this parses to an entry that spawns nothing.
TEST(ProjectConfigLoader, ScriptEntryMissingRunFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "preBuildScripts": [{"runOn": ["linux"]}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'preBuildScripts' entry [0] is missing required "
              "member 'run'");
}

// `"run": []` is shape-legal JSON but names no executable — rejected rather
// than normalized to a silent no-op entry.
TEST(ProjectConfigLoader, ScriptEntryEmptyRunArrayFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "preBuildScripts": [{"run": []}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'preBuildScripts' entry [0] member 'run' must "
              "contain at least one entry (run[0] is the executable to spawn)");
}

// A number where an argv WORD belongs. The element index is in the message —
// with a long argv, "somewhere in run" is not actionable.
TEST(ProjectConfigLoader, ScriptEntryNonStringRunElementFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "preBuildScripts": [{"run": ["bash", 7]}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'preBuildScripts' entry [0] member 'run' element "
              "[1] must be a string");
}

// An EMPTY argv word is never meaningful: as run[0] it spawns nothing, and as
// an argument it hands the child a blank parameter it will almost certainly
// misread. Rejected, never silently dropped from the vector.
TEST(ProjectConfigLoader, ScriptEntryEmptyStringRunElementFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "preBuildScripts": [{"run": ["bash", ""]}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'preBuildScripts' entry [0] member 'run' element "
              "[1] must be a non-empty string");
}

// ── THE UNPRINTABLE WORD ───────────────────────────────────────────────────
//
// JSON permits an escaped U+0000 and nlohmann stores the resulting byte in the
// `std::string`, so `is_string()` is true and the non-empty check above passes
// it: until the loader grew a NUL guard, a `run` word carrying one was a VALID
// manifest. ✔MEASURED by reverting that guard (see RED-ON-DISABLE below):
// the parse succeeds and the entry lands with the NUL sitting inside the word.
//
// What the OS then does with that word is not what the file says. An argv word
// crosses the exec boundary as a NUL-TERMINATED C string, so POSIX `execv`
// truncates it and Windows ends the whole command line at the NUL — silently
// discarding EVERY LATER ARGUMENT. The hook still runs, plausibly exits 0, and
// the build goes green having executed something other than what was declared.
// That is the failure shape with no diagnostic attached to it anywhere.
//
// The JSON below is a NON-raw C++ literal precisely so the DOUBLED backslash
// reaches the parser as the six-character JSON escape: the subject is what the
// LOADER does with a NUL, not what the C++ compiler does with one.
//
// RED-ON-DISABLE: delete the NUL guard in `stringArrayMember` and this parses
// CLEAN — `pc.has_value()` true, zero diagnostics.
TEST(ProjectConfigLoader, ScriptEntryRunWordWithEmbeddedNulFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(
        "{\"language\": \"c\", \"artifactProfile\": \"cli\","
        " \"targets\": [\"t:f\"], \"sources\": [\"a.c\"],"
        " \"preBuildScripts\": [{\"run\": [\"gen\", \"--out=a\\u0000b\"]}]}",
        "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);

    std::string const m =
        firstMessageForCode(rep, DiagnosticCode::C_MalformedJson);
    EXPECT_EQ(m,
              "p.json: field 'preBuildScripts' entry [0] member 'run' element "
              "[1] contains an embedded NUL byte (not echoed — a NUL is "
              "unprintable). A manifest word is read as a C string the moment "
              "it is used: an argv word is handed to the OS NUL-terminated, so "
              "this word would reach the program truncated on POSIX and would "
              "end the whole command line on Windows, silently dropping every "
              "argument after it.");
    // Stated apart from the equality because it is the contract, not the
    // wording: the message must not carry the offending byte. A diagnostic
    // holding a raw NUL truncates in whatever reads it next, so the two
    // INDICES are the entire locator the author is left with.
    EXPECT_EQ(m.find('\0'), std::string::npos)
        << "the diagnostic echoed the unprintable word back at the user";
}

// `run[0]` is the PROGRAM, and there a NUL does not mangle an argument — it
// selects a DIFFERENT EXECUTABLE, because the name resolves as its prefix, so
// the build would spawn a program the manifest does not name. Pinned on the
// SECOND field as well, which must name ITSELF: one reader serves both.
TEST(ProjectConfigLoader, ScriptEntryRunProgramWithEmbeddedNulFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(
        "{\"language\": \"c\", \"artifactProfile\": \"cli\","
        " \"targets\": [\"t:f\"], \"sources\": [\"a.c\"],"
        " \"postBuildScripts\": [{\"run\": [\"pack\\u0000-signed\"]}]}",
        "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'postBuildScripts' entry [0] member 'run' element "
              "[0] contains an embedded NUL byte (not echoed — a NUL is "
              "unprintable). A manifest word is read as a C string the moment "
              "it is used: an argv word is handed to the OS NUL-terminated, so "
              "this word would reach the program truncated on POSIX and would "
              "end the whole command line on Windows, silently dropping every "
              "argument after it.");
}

// The CLOSED entry key set. A mistyped `"runon"` would otherwise silently
// discard the platform filter and run a linux-only script on Windows.
TEST(ProjectConfigLoader, ScriptEntryUnknownMemberFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "preBuildScripts": [{"run": ["bash", "gen.sh"], "runon": ["linux"]}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'preBuildScripts' entry [0] has unknown member "
              "'runon' (recognized members: run, runOn)");
}

// `"runOn": []` means "run nowhere" — which is what DELETING the entry says,
// more clearly. The degenerate spelling rejects rather than aliasing, and the
// message states the ACTUAL way to mean "everywhere" (omit the member).
TEST(ProjectConfigLoader, ScriptEntryEmptyRunOnArrayFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "preBuildScripts": [{"run": ["bash", "gen.sh"], "runOn": []}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'preBuildScripts' entry [0] member 'runOn' must "
              "contain at least one platform token when present — omit the "
              "member to run on every platform");
}

// THE headline guard. `"macos"` is the natural guess and matches NO host, so
// an accepting loader yields a post-build step that silently never runs. The
// message must name BOTH the offending token AND the full vocabulary — a
// reject that does not say the right spelling just moves the guessing.
// RED-ON-DISABLE: drop the isValidRunOnToken loop and this parses clean.
TEST(ProjectConfigLoader, ScriptEntryInvalidRunOnTokenFailsLoudNamingTokenAndVocabulary) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "postBuildScripts": [{"run": ["pack.sh"], "runOn": ["linux", "macos"]}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    std::string const m =
        firstMessageForCode(rep, DiagnosticCode::C_MalformedJson);
    // Full equality — the index, the token, and the vocabulary, all pinned.
    EXPECT_EQ(m,
              "p.json: field 'postBuildScripts' entry [0] member 'runOn' "
              "element [1] has unknown platform token 'macos' (recognized: "
              "windows, linux, darwin)");
    // Stated separately because these two are the CONTRACT, independent of any
    // future rewording around them.
    EXPECT_NE(m.find("'macos'"), std::string::npos)
        << "the message must name the offending token; got: " << m;
    EXPECT_NE(m.find("windows, linux, darwin"), std::string::npos)
        << "the message must list runOnTokenList(); got: " << m;
}

// A non-string `runOn` element cannot be a token at all — caught before the
// token check, with its own message.
TEST(ProjectConfigLoader, ScriptEntryNonStringRunOnElementFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "preBuildScripts": [{"run": ["gen.sh"], "runOn": [true]}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'preBuildScripts' entry [0] member 'runOn' "
              "element [0] must be a string");
}

// `"run"` as a shell STRING is the single most likely authoring mistake, and
// it must reject rather than be split by this loader — see the ScriptEntry
// header note (two quoting dialects, one manifest).
TEST(ProjectConfigLoader, ScriptEntryRunAsShellStringFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "preBuildScripts": [{"run": "bash gen.sh"}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'preBuildScripts' entry [0] member 'run' must be "
              "an array of non-empty strings (an argv vector: run[0] is the "
              "executable, spawned directly — never a shell command line)");
}

// `runOn` as a bare string (rather than an array of tokens).
TEST(ProjectConfigLoader, ScriptEntryRunOnAsBareStringFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "preBuildScripts": [{"run": ["gen.sh"], "runOn": "linux"}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'preBuildScripts' entry [0] member 'runOn' must "
              "be an array of platform tokens (recognized: windows, linux, "
              "darwin)");
}

// The FIELD itself is not an array.
TEST(ProjectConfigLoader, NonArrayPreBuildScriptsFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "preBuildScripts": "gen.sh"
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'preBuildScripts' must be an array of "
              "{\"run\", \"runOn\"} objects");
}

// The SECOND script field is wired to the SAME validation and names ITSELF in
// the diagnostic — a reader shared by two fields must not report the wrong one.
TEST(ProjectConfigLoader, NonArrayPostBuildScriptsFailsLoudNamingItsOwnField) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "postBuildScripts": 42
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'postBuildScripts' must be an array of "
              "{\"run\", \"runOn\"} objects");
}

// An ENTRY that is not an object at all (a bare command string in the array —
// the other natural authoring shortcut).
TEST(ProjectConfigLoader, ScriptEntryNonObjectFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "preBuildScripts": ["gen.sh"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'preBuildScripts' entry [0] must be an object "
              "{\"run\": [...], \"runOn\": [...]}");
}

// ── `dependsOn` — happy path ────────────────────────────────────────────────

// FULL STRUCT EQUALITY across all three legal shapes: local path, git with a
// pinned ref, git without one. Values are kept VERBATIM — a loader that
// normalized `"../libfoo"` or parsed the URL would fail this.
TEST(ProjectConfigLoader, DependsOnPopulatedParsesToExactStructs) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"],
      "dependsOn": [
        {"path": "../libfoo"},
        {"git": "git@github.com:org/bar.git", "ref": "v1.2.0"},
        {"git": "https://example.invalid/baz.git"}
      ]
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);

    std::vector<DependencyEntry> const expected = {
        DependencyEntry{std::string{"../libfoo"}, std::nullopt, std::nullopt},
        DependencyEntry{std::nullopt, std::string{"git@github.com:org/bar.git"},
                        std::string{"v1.2.0"}},
        DependencyEntry{std::nullopt,
                        std::string{"https://example.invalid/baz.git"},
                        std::nullopt},
    };
    EXPECT_EQ(pc->dependsOn, expected);
}

// ── `dependsOn` — fail-loud paths ───────────────────────────────────────────

// EXACTLY ONE source. Two sources for one dependency is ambiguous, and
// silently preferring one makes the other a no-op the user cannot see.
TEST(ProjectConfigLoader, DependsOnBothPathAndGitFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "dependsOn": [{"path": "../libfoo", "git": "git@github.com:org/bar.git"}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'dependsOn' entry [0] states both 'path' and "
              "'git' — a dependency has exactly one source; use 'path' for a "
              "local checkout or 'git' for a remote one");
}

// An entry naming no source at all is noise, not a dependency.
TEST(ProjectConfigLoader, DependsOnNeitherPathNorGitFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "dependsOn": [{}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'dependsOn' entry [0] states neither 'path' nor "
              "'git' — a dependency needs exactly one source");
}

// A `ref` on a local `path` has no meaning. Accepting it would SILENTLY IGNORE
// the revision pin the user wrote — the exact silent-drop class this loader
// exists to prevent. RED-ON-DISABLE: delete the `dep.ref && !dep.git` branch
// and this parses clean with the pin discarded.
TEST(ProjectConfigLoader, DependsOnRefWithoutGitFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "dependsOn": [{"path": "../libfoo", "ref": "v1.2.0"}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'dependsOn' entry [0] states 'ref' without 'git' "
              "— a ref names a revision in a git remote and has no meaning for "
              "a local 'path' dependency");
}

// The CLOSED entry key set: `"url"` is the natural guess for a remote and
// would otherwise leave an entry that resolves nothing.
TEST(ProjectConfigLoader, DependsOnUnknownMemberFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "dependsOn": [{"url": "https://example.invalid/baz.git"}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'dependsOn' entry [0] has unknown member 'url' "
              "(recognized members: path, git, ref)");
}

// A non-string member value.
TEST(ProjectConfigLoader, DependsOnNonStringMemberFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "dependsOn": [{"path": ["../libfoo"]}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'dependsOn' entry [0] member 'path' must be a "
              "string");
}

// An EMPTY string is present-but-says-nothing: it would slip past the
// exactly-one-source rule while naming no dependency. Rejected at the member.
TEST(ProjectConfigLoader, DependsOnEmptyStringMemberFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "dependsOn": [{"git": "git@github.com:org/bar.git", "ref": ""}]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'dependsOn' entry [0] member 'ref' must be a "
              "non-empty string");
}

// The FIELD itself is not an array.
TEST(ProjectConfigLoader, NonArrayDependsOnFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "dependsOn": {"path": "../libfoo"}
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'dependsOn' must be an array of {\"path\"} or "
              "{\"git\", \"ref\"} objects");
}

// An ENTRY that is not an object (a bare path string — the natural shortcut,
// deliberately NOT accepted: unlike `resolveLibraries`, a bare string here
// could not distinguish a local path from a git URL).
TEST(ProjectConfigLoader, DependsOnNonObjectEntryFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "dependsOn": ["../libfoo"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: field 'dependsOn' entry [0] must be an object "
              "{\"path\": …} or {\"git\": …, \"ref\": …}");
}

// ════════════════════════════════════════════════════════════════════════════
// ✅ D-AP2-PROJECT-MANIFEST-REJECTS-COMMENT-KEY — CLOSED here.
//
// ✔MEASURED before the fix: a `--project` manifest carrying `$comment` was
// REJECTED with `unknown field '$comment' (recognized fields: …)`, while
// shipped FFI descriptors ACCEPT `$comment` and the convention is documented
// for them — the same `$`-prefixed convention blessed in one schema and
// required-absent in its sibling. The cost was that a project manifest could
// not carry its own provenance.
//
// The fix routes the top-level (and every nested) closed-key check through
// `dss::detail::isDocumentationKey`, the SHARED `$`-prefix predicate the
// grammar / target / format loaders already use — deliberately broader than a
// `$comment`-only carve-out, which would still reject `$sourcesComment` and
// re-open the inconsistency for the next annotation someone writes.
// ════════════════════════════════════════════════════════════════════════════

// The closure pin: `$`-prefixed documentation keys parse CLEAN, at the top
// level, and the real fields around them are unaffected.
// RED-ON-DISABLE: remove the `isDocumentationKey` skip in `firstUnknownKey`
// and this immediately reds with C_MalformedJson on `$comment`.
TEST(ProjectConfigLoader, DollarPrefixedDocumentationKeysParseClean) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "$comment": "these CLI builds are --project builds, not harness-leg builds",
      "language": "c",
      "artifactProfile": "cli",
      "$sourcesComment": "pinned sqlite vintage 3.45.0",
      "targets": ["x86_64:elf64-x86_64-linux-exec"],
      "sources": ["main.c"],
      "output": "dist/myprog"
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    // The prose keys are SKIPPED, not absorbed — every real field still lands.
    EXPECT_EQ(pc->language, "c");
    EXPECT_EQ(pc->artifactProfile, "cli");
    EXPECT_EQ(pc->targets, std::vector<std::string>{"x86_64:elf64-x86_64-linux-exec"});
    EXPECT_EQ(pc->sources, std::vector<std::string>{"main.c"});
    ASSERT_TRUE(pc->output.has_value());
    EXPECT_EQ(*pc->output, "dist/myprog");
}

// The carve-out is a WIDENING BY ONE CONVENTION, not a relaxation: a plain
// (non-`$`) typo still fails loud, exactly as before. This is the guard that
// stops the anchor closure from becoming an open key set.
TEST(ProjectConfigLoader, NonDollarTypoStillFailsLoudAfterCommentCarveOut) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "comment": "no leading $ — this is an ordinary unknown key",
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_NE(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson)
                  .find("unknown field 'comment'"),
              std::string::npos);
}

// The same carve-out reaches every NESTED closed key set in this loader. A
// nested set that still rejected `$comment` would just reinstate the anchor
// one level down — the annotation a user most wants to write is next to the
// entry it explains, not only at the root.
TEST(ProjectConfigLoader, DollarPrefixedKeysAlsoAcceptedInsideEveryNestedEntry) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"],
      "resolveLibraries": [
        {"$comment": "stand-in; real soname stated below",
         "path": "libfoo.so", "importName": "libfoo.so.1"}
      ],
      "preBuildScripts": [
        {"$comment": "regenerates the amalgamation", "run": ["bash", "gen.sh"]}
      ],
      "dependsOn": [
        {"$comment": "sibling checkout during development", "path": "../libfoo"}
      ]
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(pc->resolveLibraries.size(), 1u);
    EXPECT_EQ(pc->resolveLibraries[0].path, "libfoo.so");
    EXPECT_EQ(pc->resolveLibraries[0].declaredImportName, "libfoo.so.1");
    EXPECT_EQ(pc->preBuildScripts,
              (std::vector<ScriptEntry>{ScriptEntry{{"bash", "gen.sh"}, {}}}));
    EXPECT_EQ(pc->dependsOn,
              (std::vector<DependencyEntry>{DependencyEntry{
                  std::string{"../libfoo"}, std::nullopt, std::nullopt}}));
}

// ════════════════════════════════════════════════════════════════════════════
// THE DRIFT-PROOF PIN.
//
// The recognized-field list used to be hand-written TWICE — the `kKnownKeys`
// table the check loops over, and a string literal inside the "unknown field"
// message — with NOTHING asserting the two agreed. The silent direction is the
// dangerous one: a key added to the table but not to the message leaves a user
// staring at a reject whose remediation list omits the field they need.
//
// The message is now DERIVED from the table, and this test reads the REAL
// table through `projectConfigKnownKeys()` rather than re-typing it —
// re-typing the list here would simply recreate the drift one layer out.
// ════════════════════════════════════════════════════════════════════════════
TEST(ProjectConfigLoader, UnknownKeyMessageIsDerivedFromTheRealKnownKeyTable) {
    // (1) The joined list is EXACTLY the exported table, joined. Built by
    //     iterating the accessor — no literal list anywhere in this test.
    std::string expectedList;
    for (std::string_view k : projectConfigKnownKeys()) {
        if (!expectedList.empty()) expectedList += ", ";
        expectedList += k;
    }
    EXPECT_EQ(projectConfigKnownKeyList(), expectedList);

    // (2) Non-degeneracy. Without this, an accessor returning an EMPTY span
    //     would satisfy (1) and (3) trivially while the message listed nothing.
    //     Bumping this number is the intended, visible cost of adding a key.
    //     13 -> 14 on 2026-08-31: `dependencyArtifactCache`
    //     (D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C)).
    EXPECT_EQ(projectConfigKnownKeys().size(), 14u);
    for (std::string_view k : projectConfigKnownKeys()) {
        EXPECT_FALSE(k.empty()) << "a zero-filled table entry would whitelist "
                                   "the empty key (DSS_CHECK_KEY_VOCABULARY)";
    }

    // (3) The diagnostic is that list, verbatim — FULL equality, not a
    //     substring probe. A key present in the table but missing from the
    //     message (the old silent drift) reds here.
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "preBuildScript": []
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    ASSERT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(firstMessageForCode(rep, DiagnosticCode::C_MalformedJson),
              "p.json: unknown field 'preBuildScript' (recognized fields: "
                  + expectedList + ")");
}

// ── Predicate: artifactProfileSupported ─────────────────────────

TEST(ArtifactProfilePredicate, SupportedProfileInSet) {
    std::vector<std::string> declared = {"cli", "lib", "staticlib"};
    EXPECT_TRUE(artifactProfileSupported(asSpan(declared), "cli"));
    EXPECT_TRUE(artifactProfileSupported(asSpan(declared), "staticlib"));
}

TEST(ArtifactProfilePredicate, UnsupportedProfileNotInSet) {
    std::vector<std::string> declared = {"cli", "lib", "staticlib"};
    EXPECT_FALSE(artifactProfileSupported(asSpan(declared), "gui"));
    EXPECT_FALSE(artifactProfileSupported(asSpan(declared), "script"));
}

// The empty-set fail-closed rule: a language declaring NO profiles
// supports none — ANY requested profile is unsupported.
TEST(ArtifactProfilePredicate, EmptySetRejectsAnyProfile) {
    std::vector<std::string> declared = {};
    EXPECT_FALSE(artifactProfileSupported(asSpan(declared), "cli"));
    EXPECT_FALSE(artifactProfileSupported(asSpan(declared), "gui"));
}

// ── Driver gate: enforceArtifactProfile ─────────────────────────

TEST(EnforceArtifactProfile, AcceptsSupportedProfileSilently) {
    std::vector<std::string> declared = {"cli", "lib", "staticlib"};
    DiagnosticReporter rep;
    EXPECT_TRUE(enforceArtifactProfile(asSpan(declared), "cli", "c", rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(sawCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported));
}

// RED-on-disable lever: disable artifactProfileSupported() (always
// return true) and this expectation flips green.
TEST(EnforceArtifactProfile, RejectsUnsupportedProfileFailLoud) {
    std::vector<std::string> declared = {"cli", "lib", "staticlib"};
    DiagnosticReporter rep;
    EXPECT_FALSE(enforceArtifactProfile(asSpan(declared), "gui", "c", rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 1u);
}

// Empty-set DIAGNOSTIC: the fail-closed reject emits the same code.
TEST(EnforceArtifactProfile, RejectsEmptySetFailLoud) {
    std::vector<std::string> declared = {};
    DiagnosticReporter rep;
    EXPECT_FALSE(enforceArtifactProfile(asSpan(declared), "cli", "no-profiles-lang", rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 1u);
}

// ── Integration: the real shipped c grammar ──────────────
// c declares ["cli","lib","staticlib"] (AP1). Proves the real
// grammar's artifactProfiles() span flows through the gate.

TEST(EnforceArtifactProfileShipped, CAcceptsCli) {
    auto g = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(g.has_value());
    DiagnosticReporter rep;
    EXPECT_TRUE(enforceArtifactProfile((*g)->artifactProfiles(), "cli", "c", rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(EnforceArtifactProfileShipped, CRejectsGui) {
    auto g = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(g.has_value());
    // Sanity: the real grammar declares a non-empty set excluding gui.
    EXPECT_FALSE((*g)->artifactProfiles().empty());
    DiagnosticReporter rep;
    EXPECT_FALSE(enforceArtifactProfile((*g)->artifactProfiles(), "gui", "c", rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 1u);
}

// ── Integration: Program::compileProject (the AP2 wiring) ───────
// Drive the real load → grammar → enforce → route chain via the
// rep-injection overload so the EXACT emitted code is observable —
// the unit tests above cover the pieces; these pin the wiring that
// connects them (a regression that routed on targets.size(), swapped
// language/profile, or dropped the enforce call would be green-but-
// broken without these).

namespace {
std::filesystem::path writeProjectFile(std::filesystem::path const& dir,
                                       std::string_view content) {
    auto p = dir / "test.dss-project.json";
    std::ofstream out{p, std::ios::binary};
    out << content;
    return p;
}
} // namespace

TEST(CompileProjectIntegration, MissingProjectFileFailsLoud) {
    Program prog;
    DiagnosticReporter rep;
    EXPECT_EQ(prog.compileProject("nonexistent-xyz.dss-project.json", rep), 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_FileNotFound), 1u);
}

TEST(CompileProjectIntegration, MalformedConfigFailsLoud) {
    dss::test_support::ScratchDir scratch{
        dss::test_support::Location::Temp, "program"};
    auto path = writeProjectFile(scratch.path(), "{ not valid json ");
    Program prog;
    DiagnosticReporter rep;
    EXPECT_EQ(prog.compileProject(path.string(), rep), 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// The AP2 deliverable end-to-end: a real project config requesting a
// profile c doesn't declare → D_ArtifactProfileNotSupported,
// rejected BEFORE any compile (the source path need not exist).
TEST(CompileProjectIntegration, UnsupportedProfileRejectedBeforeCompile) {
    dss::test_support::ScratchDir scratch{
        dss::test_support::Location::Temp, "program"};
    auto path = writeProjectFile(scratch.path(), R"({
      "language": "c", "artifactProfile": "gui",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"]
    })");
    Program prog;
    DiagnosticReporter rep;
    EXPECT_EQ(prog.compileProject(path.string(), rep), 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 1u);
}

// A SUPPORTED profile passes the gate and the wiring PROCEEDS to the
// compile path; the build then fails on the (deliberately missing)
// source — but NOT via the profile gate. Proves the gate accepted +
// control flowed past enforcement to delegation (1 source → the
// single-CU `compileFiles` route).
TEST(CompileProjectIntegration, SupportedProfileProceedsPastGate) {
    dss::test_support::ScratchDir scratch{
        dss::test_support::Location::Temp, "program"};
    auto path = writeProjectFile(scratch.path(), R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["nonexistent-src.c"]
    })");
    Program prog;
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(path.string(), rep), 0);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 0u)
        << "a supported profile must NOT trip the gate — failure is downstream";
}

// ── AP3: enforceArtifactProfileFormat (the FORMAT-side gate) ────
// The format-side twin of the AP2 language gate — same generic
// `artifactProfileSupported` predicate, distinct code/message.
//
// ⚠ The gate's REJECT arm SPLIT IN TWO (closing
// D-AP3-UNSERVED-PROFILE-MISREPORTS-AS-A-FORMAT-MISMATCH). The
// `servingFormats` argument — the shipped formats that DO serve the profile,
// MEASURED by the caller — picks which reject is true, so these cells now
// state that measurement explicitly instead of leaving one code to answer two
// different questions. See the dedicated split section below.

namespace {
// The measured serving sets these unit cells hand the gate. They are NOT the
// authority (the driver measures the real inventory — pinned end-to-end in the
// split section below); they are the two SHAPES the gate must distinguish.
std::vector<std::string> const kSomeFormatsServeLib = {
    "elf64-x86_64-linux-dyn", "pe64-x86_64-windows-dll"};
std::vector<std::string> const kNoFormatServesIt = {};
} // namespace

TEST(EnforceArtifactProfileFormat, ServedProfileAcceptedSilently) {
    std::vector<std::string> served = {"cli"};
    DiagnosticReporter rep;
    EXPECT_TRUE(enforceArtifactProfileFormat(asSpan(served), "cli",
                                             "elf64-x86_64-linux-exec",
                                             asSpan(kSomeFormatsServeLib), rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

// RED-on-disable: make artifactProfileSupported always-true → this flips green.
TEST(EnforceArtifactProfileFormat, UnservedProfileFailsLoud) {
    std::vector<std::string> served = {"cli"};  // an exec format
    DiagnosticReporter rep;
    EXPECT_FALSE(enforceArtifactProfileFormat(asSpan(served), "lib",
                                              "elf64-x86_64-linux-exec",
                                              asSpan(kSomeFormatsServeLib), rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
}

// A relocatable / backend-less format serves NOTHING → rejects any profile
// (fail-closed, the format-side twin of the empty-language-set reject). The
// profile IS served elsewhere, so this is still a genuine mismatch — the
// empty set here is the CHOSEN FORMAT's, which is a different axis from the
// empty `servingFormats` that selects the "no backend anywhere" arm.
TEST(EnforceArtifactProfileFormat, EmptyServedSetRejects) {
    std::vector<std::string> served = {};
    std::vector<std::string> const servingCli = {"elf64-x86_64-linux-exec"};
    DiagnosticReporter rep;
    EXPECT_FALSE(enforceArtifactProfileFormat(asSpan(served), "cli",
                                              "elf64-x86_64-linux",
                                              asSpan(servingCli), rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
    EXPECT_NE(firstMessageForCode(rep,
                                  DiagnosticCode::D_ArtifactProfileFormatMismatch)
                  .find("serves no artifact profiles"),
              std::string::npos)
        << "the chosen format's EMPTY served set stays discriminated in the "
           "wording; got: "
        << firstMessageForCode(rep,
                               DiagnosticCode::D_ArtifactProfileFormatMismatch);
}

// Integration with the REAL shipped format's served set.
TEST(EnforceArtifactProfileFormatShipped, ExecServesCliRejectsLib) {
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(f.has_value());
    EXPECT_FALSE((*f)->artifactProfiles().empty());
    DiagnosticReporter rep1;
    EXPECT_TRUE(enforceArtifactProfileFormat((*f)->artifactProfiles(), "cli",
                                             "elf64-x86_64-linux-exec",
                                             asSpan(kSomeFormatsServeLib), rep1));
    DiagnosticReporter rep2;
    EXPECT_FALSE(enforceArtifactProfileFormat((*f)->artifactProfiles(), "lib",
                                              "elf64-x86_64-linux-exec",
                                              asSpan(kSomeFormatsServeLib), rep2));
    EXPECT_EQ(countCode(rep2, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
}

// ── AP3: compileProject end-to-end format gate (the deliverable) ────
// (reuses writeProjectFile + ScratchDir from the integration section above)

// A profile served by the chosen format passes BOTH gates → proceeds to the
// compile (which then fails downstream on the missing source — NOT via a
// profile gate). Proves the format gate accepts + control flowed past it.
TEST(CompileProjectIntegration, CliProfilePassesFormatGate) {
    dss::test_support::ScratchDir scratch{
        dss::test_support::Location::Temp, "program"};
    auto path = writeProjectFile(scratch.path(), R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["nonexistent-src.c"]
    })");
    Program prog;
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(path.string(), rep), 0);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 0u)
        << "cli IS served by an exec format — the format gate must not trip";
}

// The AP3 deliverable end-to-end: a profile the LANGUAGE declares (lib ∈
// c) but the CHOSEN FORMAT (an executable) does NOT serve →
// D_ArtifactProfileFormatMismatch, rejected before any compile. The AP2
// language gate must NOT fire (lib is declared by c) — proving the two
// gates are distinct.
TEST(CompileProjectIntegration, LibProfileOnExecFormatMismatch) {
    dss::test_support::ScratchDir scratch{
        dss::test_support::Location::Temp, "program"};
    auto path = writeProjectFile(scratch.path(), R"({
      "language": "c", "artifactProfile": "lib",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"]
    })");
    Program prog;
    DiagnosticReporter rep;
    EXPECT_EQ(prog.compileProject(path.string(), rep), 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 0u);
    // `lib` IS served — by the shared-library formats — so this stays the
    // GENUINE mismatch arm and must not be reported as "no backend exists".
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNoServingFormat), 0u);
}

// N1: multi-target — one serving, one not — fails the build with the mismatch
// (the gate checks EVERY target; order-independent). The relocatable format
// serves nothing, so the cli profile trips on that target.
TEST(CompileProjectIntegration, MultiTargetOneMismatchFailsLoud) {
    dss::test_support::ScratchDir scratch{
        dss::test_support::Location::Temp, "program"};
    auto path = writeProjectFile(scratch.path(), R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec", "x86_64:elf64-x86_64-linux"],
      "sources": ["main.c"]
    })");
    Program prog;
    DiagnosticReporter rep;
    EXPECT_EQ(prog.compileProject(path.string(), rep), 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
}

TEST(ProjectConfigDiagnostics, DArtifactProfileFormatMismatchRoundTrip) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_ArtifactProfileFormatMismatch),
              "D_ArtifactProfileFormatMismatch");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::D_ArtifactProfileFormatMismatch),
              "D0011");
}

TEST(ProjectConfigDiagnostics, DArtifactNameEscapesOutputDirRoundTrip) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_ArtifactNameEscapesOutputDir),
              "D_ArtifactNameEscapesOutputDir");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::D_ArtifactNameEscapesOutputDir),
              "D0015");
}

// ════════════════════════════════════════════════════════════════
// THE AP3 REJECT SPLIT — which reject is TRUE is a MEASUREMENT
// (closes D-AP3-UNSERVED-PROFILE-MISREPORTS-AS-A-FORMAT-MISMATCH)
// ════════════════════════════════════════════════════════════════
//
// `D_ArtifactProfileFormatMismatch` used to answer two different questions and
// answered one of them with a confident falsehood: it reads "you picked the
// wrong format", which is a true statement about the wrong thing when NO
// shipped format implements the profile anywhere. The remediation differs
// completely — pick another target vs. ship a backend — so the two facts are
// two codes, chosen by the SHIPPED-FORMAT INVENTORY the driver measures:
//   * ≥1 format serves it, not this one ⇒ 0xD011, NAMING the ones that do;
//   * ZERO formats serve it             ⇒ 0xD028.
// ✔RE-MEASURED 2026-08-15 over all 24 `src/dss-config/object-formats/
// *.format.json`: the served union is `{cli, lib, staticlib, module}` (cli 7
// formats, lib 5, staticlib 5, module 10 = the lib+staticlib union, and 7
// formats serve nothing), so the second arm's population is exactly `gui`,
// `script`, `sproc`, `transpile`, `shader`, `hdl`.

TEST(EnforceArtifactProfileFormat, NoShippedFormatServesItReportsTheOtherCode) {
    std::vector<std::string> served = {"cli"};
    DiagnosticReporter rep;
    EXPECT_FALSE(enforceArtifactProfileFormat(asSpan(served), "shader",
                                              "elf64-x86_64-linux-exec",
                                              asSpan(kNoFormatServesIt), rep));
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNoServingFormat), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 0u);
    std::string const m =
        firstMessageForCode(rep, DiagnosticCode::D_ArtifactProfileNoServingFormat);
    EXPECT_NE(m.find("shader"), std::string::npos) << m;
    EXPECT_NE(m.find("no shipped object format implements it yet"),
              std::string::npos)
        << m;
}

// The actionable half of the genuine mismatch: EVERY serving format is named.
// Without this the reader is told where the profile is NOT served and left to
// search the target matrix for where it is.
TEST(EnforceArtifactProfileFormat, GenuineMismatchNamesTheFormatsThatDoServeIt) {
    std::vector<std::string> served = {"cli"};
    DiagnosticReporter rep;
    EXPECT_FALSE(enforceArtifactProfileFormat(asSpan(served), "lib",
                                              "elf64-x86_64-linux-exec",
                                              asSpan(kSomeFormatsServeLib), rep));
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNoServingFormat), 0u);
    std::string const m =
        firstMessageForCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch);
    for (auto const& f : kSomeFormatsServeLib) {
        EXPECT_NE(m.find(f), std::string::npos)
            << "every serving format must be named; missing '" << f
            << "' in: " << m;
    }
    EXPECT_NE(m.find("elf64-x86_64-linux-exec"), std::string::npos)
        << "…and the format the user actually chose; got: " << m;
}

// ★ THE DRIVER MEASURES THE REAL INVENTORY. The unit cells above hand the gate
// a list; this one proves the DRIVER computes one — the message names a real
// shipped shared-library format that this test never mentioned to it. A
// hardcoded or empty list would fail here, and a regression that stopped
// scanning would report the wrong arm entirely.
TEST(CompileProjectIntegration, TheServingFormatListIsMeasuredFromTheShippedSet) {
    dss::test_support::ScratchDir scratch{
        dss::test_support::Location::Temp, "program"};
    auto path = writeProjectFile(scratch.path(), R"({
      "language": "c", "artifactProfile": "lib",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"]
    })");
    Program prog;
    DiagnosticReporter rep;
    EXPECT_EQ(prog.compileProject(path.string(), rep), 1);
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
    std::string const m =
        firstMessageForCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch);
    // Cross-checked against the shipped documents themselves rather than a
    // re-typed list: whatever declares `lib` must appear in the message.
    std::size_t named = 0;
    for (std::string_view fmt : {"elf64-x86_64-linux-dyn", "elf64-aarch64-linux-dyn",
                                 "macho64-arm64-darwin-dylib",
                                 "macho64-x86_64-darwin-dylib",
                                 "pe64-x86_64-windows-dll"}) {
        auto f = ObjectFormatSchema::loadShipped(std::string{fmt});
        ASSERT_TRUE(f.has_value()) << fmt;
        ASSERT_TRUE(artifactProfileSupported((*f)->artifactProfiles(), "lib"))
            << fmt << " no longer declares `lib` — re-derive this pin";
        EXPECT_NE(m.find(fmt), std::string::npos)
            << "a format that DOES serve `lib` is missing from the "
               "remediation; got: "
            << m;
        ++named;
    }
    EXPECT_EQ(named, 5u);
}

// ── the new code's name / prefix round-trip ─────────────────────────────────

TEST(ProjectConfigDiagnostics, DArtifactProfileNoServingFormatRoundTrip) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_ArtifactProfileNoServingFormat),
              "D_ArtifactProfileNoServingFormat");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::D_ArtifactProfileNoServingFormat),
              "D0028");
}

// ── Routing: routesToMultiUnit (the shared >1 threshold) ────────

TEST(RoutesToMultiUnit, SingleAndZeroRouteToSingleUnit) {
    EXPECT_FALSE(routesToMultiUnit(0));
    EXPECT_FALSE(routesToMultiUnit(1));
}

TEST(RoutesToMultiUnit, MultipleSourcesRouteToMultiUnit) {
    EXPECT_TRUE(routesToMultiUnit(2));
    EXPECT_TRUE(routesToMultiUnit(5));
}

// ── Diagnostic-code name / prefix round-trip ────────────────────

TEST(ProjectConfigDiagnostics, DArtifactProfileNotSupportedRoundTrip) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_ArtifactProfileNotSupported),
              "D_ArtifactProfileNotSupported");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::D_ArtifactProfileNotSupported),
              "D0010");
}

// ════════════════════════════════════════════════════════════════
// AP4 — per-language onboarding matrix (plan 06 §5 AP4 / §7)
// ════════════════════════════════════════════════════════════════
//
// AP2/AP3 exercised the two driver gates with c ONLY. AP4
// completes the per-language matrix: each shipped language driven
// through `compileProject` across its declared / undeclared profiles
// and the served / unserved format axis. The shipped declared SETS
// themselves are already exact-pinned in tests/core
// (`GrammarSchema.ShippedConfigsDeclareArtifactProfiles`) — these
// cells pin the DRIVER's gate OUTCOME per language instead.
//
// Each cell is a THREE-SIDED assertion so a regression is RED, never
// silently green:
//   (a) the EXACT expected gate-code count == 1,
//   (b) the OTHER gate's code count == 0  (gate distinctness),
//   (c) the return code (1 = the gate STOPPED the build).
// A swap of the two codes, extra diagnostic noise, or a failure to
// stop the build each breaks at least one side.
//
// HONESTY NOTES (the matrix's scope — stated so no reader over-reads):
//   * ⚠ RE-MEASURED 2026-08-15 over all 24 shipped
//     `src/dss-config/object-formats/*.format.json`: the union of their
//     `artifactProfiles[]` is {cli, lib, staticlib, module} — cli by 7
//     formats, lib by 5 (dyn/dylib/dll), staticlib by 5 (the archive
//     formats), module by all 10 of those (a module IS a library), and
//     7 formats serve nothing at all. (The note that used to stand here
//     said no format serves a non-cli profile; that was true when
//     written and the shared-library / archive backends have shipped
//     since.) Every cell below still points at ONE exec target, so
//     lib/staticlib/module are REJECTED here — but as a GENUINE
//     mismatch (a serving format exists, and the message names it),
//     which is a different arm from script/sproc, where none exists.
//   * script/sproc/gui/transpile/shader/hdl are served by NOTHING, so
//     they have no positive format-gate cell anywhere. A future
//     SQL-emit / gui backend would add the serving format + the
//     positive cell with ZERO gate-code change (the gate is generic
//     set-membership, never a format-name branch).
//   * toy & tsql-subset are onboarded here in the GATE sense only —
//     they emit no artifact in this matrix. c is the sole real
//     end-to-end emit (`RealCliProjectEmitsElfExecutable`, last test).
//   * The profile does NOT yet drive artifact SHAPE (entry-symbol /
//     PE subsystem / extension); that codegen-threading is deferred
//     (D-AP2-COMPILATION-CONTEXT, trigger-gated on a non-format-
//     redundant consumer, e.g. a gui profile). AP4 therefore asserts
//     GATE behavior — never a profile-driven shape, which today would
//     be VACUOUS (the shape comes from the (target:format) spec, so
//     such a test would pass even if the profile were ignored).

namespace {

// One exec format used for every matrix cell. It serves ["cli"] and is
// host-agnostic to EMIT (DSS cross-compiles); cells that reach the
// build use a guaranteed-absent source so they fail downstream, never
// on the gate.
constexpr std::string_view kExecTarget = "x86_64:elf64-x86_64-linux-exec";

std::string matrixProjectJson(std::string_view language,
                              std::string_view profile,
                              std::string_view targetSpec) {
    // A deliberately-absent source: gate-reject cells never read it,
    // and the one "passes both gates" cell must fail DOWNSTREAM (not on
    // a gate) when the build tries to open it.
    return std::string{"{\n  \"language\": \""} + std::string{language}
        + "\",\n  \"artifactProfile\": \"" + std::string{profile}
        + "\",\n  \"targets\": [\"" + std::string{targetSpec}
        + "\"],\n  \"sources\": [\"ap4-absent-source.c\"]\n}";
}

// Drive `compileProject` on a scratch project file. Location::Temp + NO
// useAsCwd: every cell either rejects at a gate BEFORE the compile (so
// the source need not exist) or fails downstream on the absent source —
// and `loadShipped` walks UP from the unchanged test cwd to find
// src/dss-config/ (exactly like the AP2/AP3 cells above). `rep` is the
// caller's, so the emitted codes outlive the scratch dir's teardown.
int runMatrixCell(std::string_view language, std::string_view profile,
                  std::string_view targetSpec, DiagnosticReporter& rep) {
    dss::test_support::ScratchDir scratch{
        dss::test_support::Location::Temp, "program"};
    auto path = writeProjectFile(
        scratch.path(), matrixProjectJson(language, profile, targetSpec));
    Program prog;
    return prog.compileProject(path.string(), rep);
}

} // namespace

// ── toy: declares ["cli"] ───────────────────────────────────────

// cli IS declared by toy AND served by the exec format → BOTH gates
// pass; the build then fails DOWNSTREAM on the absent source (not a
// gate). Proves toy flows THROUGH compileProject (toy had never been
// driven through it before AP4).
TEST(ArtifactProfileMatrix, ToyCliPassesBothGatesThenFailsDownstream) {
    DiagnosticReporter rep;
    int const rc = runMatrixCell("toy", "cli", kExecTarget, rep);
    EXPECT_NE(rc, 0) << "absent source must fail the build downstream";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 0u);
}

TEST(ArtifactProfileMatrix, ToyGuiRejectedByLanguageGate) {
    DiagnosticReporter rep;
    int const rc = runMatrixCell("toy", "gui", kExecTarget, rep);
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 0u);
}

TEST(ArtifactProfileMatrix, ToyLibRejectedByLanguageGate) {
    DiagnosticReporter rep;
    int const rc = runMatrixCell("toy", "lib", kExecTarget, rep);
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 0u);
}

// ── c: declares ["cli","lib","staticlib"] ────────────────
// (AP2/AP3 already cover cli-passes + lib-mismatch; AP4 adds staticlib
//  + the §7-#3 actionable-message pin.)

TEST(ArtifactProfileMatrix, CStaticlibMismatchByFormatGate) {
    DiagnosticReporter rep;
    int const rc = runMatrixCell("c", "staticlib", kExecTarget, rep);
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 0u)
        << "staticlib IS declared by c — the LANGUAGE gate must not fire";
    // The OTHER side of the split: `staticlib` IS served (by the archive
    // formats), so this cell must keep the genuine-mismatch code. The pair of
    // cells — this one and TsqlScript… above — is what proves the driver is
    // MEASURING the inventory rather than reporting a constant.
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNoServingFormat), 0u);
}

// §7 criterion #3: the language-gate message must NAME the language and
// LIST the supported set (actionable remediation) — not just a code.
// (red-on-disable: if enforceArtifactProfile dropped the list, the
// "staticlib" find() fails.)
TEST(ArtifactProfileMatrix, CGuiMessageNamesLanguageAndSupportedSet) {
    DiagnosticReporter rep;
    (void) runMatrixCell("c", "gui", kExecTarget, rep);
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 1u);
    std::string const m =
        firstMessageForCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported);
    EXPECT_NE(m.find("c"), std::string::npos)
        << "message must name the language; got: " << m;
    EXPECT_NE(m.find("staticlib"), std::string::npos)
        << "message must list the supported profiles; got: " << m;
}

// ── tsql-subset: declares ["script","sproc"] ────────────────────
// (never driven through compileProject before AP4.)

// script IS declared by tsql → the LANGUAGE gate passes; but NO shipped
// format serves script → the FORMAT gate rejects. Proves the two gates
// are distinct AND that a declared-but-unserved profile reaches (and is
// caught by) the format gate.
//
// ⚠ WHICH REJECT CHANGED, AND IT WAS NOT WEAKENED. ZERO shipped formats serve
// `script`, so the truthful verdict is "no backend implements this yet"
// (`D_ArtifactProfileNoServingFormat`), NOT "you picked the wrong format".
// The cell asserts the more precise code AND, unlike before, the TEXT — the
// old assertion could not tell the two facts apart, which is the whole defect
// being closed. The mismatch code is now asserted ABSENT, so a regression that
// re-merged the arms is red rather than silently acceptable.
TEST(ArtifactProfileMatrix, TsqlScriptRejectedAsUnimplementedByEveryFormat) {
    DiagnosticReporter rep;
    int const rc = runMatrixCell("tsql-subset", "script", kExecTarget, rep);
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNoServingFormat), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 0u)
        << "no shipped format serves `script` — 'pick a different format' is a "
           "confidently wrong remediation here";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 0u)
        << "script IS declared by tsql-subset — the LANGUAGE gate must not fire";
    std::string const m =
        firstMessageForCode(rep, DiagnosticCode::D_ArtifactProfileNoServingFormat);
    EXPECT_NE(m.find("script"), std::string::npos) << m;
    EXPECT_NE(m.find("no shipped object format implements it yet"),
              std::string::npos)
        << "the message must state the actual fact; got: " << m;
}

TEST(ArtifactProfileMatrix, TsqlSprocRejectedAsUnimplementedByEveryFormat) {
    DiagnosticReporter rep;
    int const rc = runMatrixCell("tsql-subset", "sproc", kExecTarget, rep);
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNoServingFormat), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 0u);
}

// cli is NOT declared by tsql-subset → the LANGUAGE gate rejects first
// (the format gate never runs).
TEST(ArtifactProfileMatrix, TsqlCliRejectedByLanguageGate) {
    DiagnosticReporter rep;
    int const rc = runMatrixCell("tsql-subset", "cli", kExecTarget, rep);
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 0u);
}

// ── the ONE real end-to-end emit through compileProject ─────────
// Every other compileProject test (AP2/AP3 + the matrix above) uses an
// ABSENT source and asserts only gate behavior. This one proves the
// project-config path actually COMPILES + EMITS: a real cli c
// program → a real ELF executable on disk. Host-agnostic — DSS cross-
// emits, so we assert the bytes (ELF magic), never RUN. It is also the
// positive cli FORMAT-gate cell, end-to-end. Uses Location::InsideRepo
// + useAsCwd (the cwd-rooted output + schema-loader walk contract).
TEST(CompileProjectIntegration, RealCliProjectEmitsElfExecutable) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::InsideRepo, "program"};
    {
        std::ofstream f{scratch.path() / "main.c", std::ios::binary};
        f << "int main() { return 42; }\n";
    }
    auto path = writeProjectFile(scratch.path(), R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"]
    })");
    scratch.useAsCwd();

    Program prog;
    DiagnosticReporter rep;
    int const rc = prog.compileProject(path.string(), rep);
    ASSERT_EQ(rc, 0)
        << "a real cli c project must compile + emit via compileProject";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 0u);

    // ELF Exec ⇒ no output extension (TargetSpec::outputExtension); the
    // artifact stem is the source stem ("main").
    auto const out =
        scratch.path() / "target" / "elf64-x86_64-linux-exec" / "main";
    ASSERT_TRUE(std::filesystem::exists(out))
        << "expected artifact at " << out.string();
    ASSERT_GT(std::filesystem::file_size(out), 0u);
    std::ifstream in{out, std::ios::binary};
    unsigned char hdr[4] = {0};
    in.read(reinterpret_cast<char*>(hdr), 4);
    EXPECT_EQ(hdr[0], 0x7Fu);
    EXPECT_EQ(hdr[1], static_cast<unsigned char>('E'));
    EXPECT_EQ(hdr[2], static_cast<unsigned char>('L'));
    EXPECT_EQ(hdr[3], static_cast<unsigned char>('F'));
}

// ════════════════════════════════════════════════════════════════
// ★★ A MODULE IS A LIBRARY — the profile / composition split, both
//    halves, driven end-to-end (operator ruling 2026-08-15)
// ════════════════════════════════════════════════════════════════
//
// `artifactProfile` and `DependencyComposition` answer two ORTHOGONAL
// questions, and B.12 was withdrawn for conflating them:
//   * artifactProfile — what does this project build ITSELF as? `module`
//     builds as a LIBRARY. It declares `targets[]`, it is served by every
//     format that serves `lib` or `staticlib`, and it produces an artifact.
//   * DependencyComposition — how is it CONSUMED? `module` is `SourceMerge`:
//     a consumer takes its SOURCES and ignores its artifact entirely.
//
// The two facts are pinned TOGETHER, on the same module source, because
// either one alone is satisfied by a wrong implementation: a build that
// refused the standalone compile would still pass the consumption pin, and a
// consumer that linked the module's archive instead of merging its sources
// would still pass the standalone pin. This is also the pin that was
// IMPOSSIBLE while the withdrawn rule stood — it made the standalone half a
// load error.

TEST(ModuleIsALibrary, StandaloneModuleBuildEmitsAnArchive) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::InsideRepo, "program"};
    {
        std::ofstream f{scratch.path() / "scale.c", std::ios::binary};
        f << "int dss_scale(int n) { return n * 7; }\n";
    }
    // A `-staticlib` target: the profile says "I am a library", the target
    // spec says "in an archive container" — the same division `lib` and
    // `staticlib` already use, so `module` needed no new mechanism.
    auto path = writeProjectFile(scratch.path(), R"({
      "language": "c", "artifactProfile": "module",
      "targets": ["x86_64:elf64-x86_64-linux-staticlib"], "sources": ["scale.c"]
    })");
    scratch.useAsCwd();

    Program prog;
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(path.string(), rep), 0)
        << "a `module` project must build STANDALONE — otherwise it cannot be "
           "type-checked, codegen'd or diagnosed until some consumer imports "
           "it, and its errors then surface inside the CONSUMER's build";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 0u)
        << "the archive formats SERVE `module` — the AP3 gate must pass";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNoServingFormat), 0u);

    auto const out = scratch.path() / "target"
                   / "elf64-x86_64-linux-staticlib" / "scale.a";
    ASSERT_TRUE(std::filesystem::exists(out))
        << "expected the archive at " << out.string();
    ASSERT_GT(std::filesystem::file_size(out), 0u);
    // The BYTES, not merely a file: an `ar` archive starts with "!<arch>\n".
    std::ifstream in{out, std::ios::binary};
    char magic[8] = {0};
    in.read(magic, 8);
    EXPECT_EQ(std::string_view(magic, 8), "!<arch>\n")
        << "a module built against an archive format must BE an archive";
}

// The other half, same profile: CONSUMED, the module contributes SOURCES and
// no second artifact is produced or linked. The consumer's binary resolves a
// symbol only the module defines, which is what proves the merge happened —
// and the module's OWN artifact is asserted ABSENT, which is what proves the
// consumer ignored it rather than building and linking it.
TEST(ModuleIsALibrary, ConsumedModuleMergesSourcesAndEmitsNoSecondArtifact) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    auto const dep = dir / "scalemod";
    std::error_code mkEc;
    std::filesystem::create_directories(dep, mkEc);
    ASSERT_FALSE(mkEc) << mkEc.message();

    auto write = [](std::filesystem::path const& p, std::string_view text) {
        std::ofstream f{p, std::ios::binary};
        f << text;
    };
    write(dep / "scale.c", "int dss_scale(int n) { return n * 7; }\n");
    // The SAME manifest shape as the standalone pin above — targets and all.
    write(dep / ".dss-project.json", R"({
      "language": "c", "artifactProfile": "module",
      "targets": ["x86_64:elf64-x86_64-linux-staticlib"], "sources": ["scale.c"]
    })");
    write(dir / "main.c",
          "extern int dss_scale(int);\n"
          "int main(void){ return dss_scale(6); }\n");
    auto const proj = dir / "app.dss-project.json";
    write(proj, R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"],
      "dependsOn": [{ "path": "scalemod" }]
    })");
    scratch.useAsCwd();

    Program prog;
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << "the consumer must build: `SourceMerge` supplies the definition "
           "`main` calls";
    auto const consumerOut =
        dir / "target" / "elf64-x86_64-linux-exec" / "main";
    EXPECT_TRUE(std::filesystem::exists(consumerOut))
        << "expected the consumer artifact at " << consumerOut.string();
    // ★ NO second artifact. The dependency declares a `-staticlib` target of
    // its own and the consumer must IGNORE it: a resolver that built the
    // module as a library and linked it would leave this archive behind.
    EXPECT_FALSE(std::filesystem::exists(
        dep / "target" / "elf64-x86_64-linux-staticlib" / "scale.a"))
        << "a source-merged dependency must not be BUILT — its artifact "
           "emission is ignored, which is exactly what `SourceMerge` means";
}

// ════════════════════════════════════════════════════════════════
// Behavioral threading proof — the manifest's OPTIONAL compile-flag
// arrays (includes / defines / resolveLibraries) actually TAKE EFFECT
// end-to-end through compileProject, not merely PARSE (the loader tests
// above). Each has a RED-ON-DISABLE arm: drop the merge-stamp in
// Program::compileProject and the "on" case regresses to the "off"
// outcome. These reach the SAME Program state the CLI stamps
// (setIncludeDirs / setUserDefines / setResolveLibraries), which
// compileFiles/compileUnits read at CU-build time.
// ════════════════════════════════════════════════════════════════

namespace {

// A JSON array literal from string entries (each quoted). Empty ⇒ "[]".
std::string jsonStrArray(std::vector<std::string> const& items) {
    std::string s = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) s += ", ";
        s += "\"" + items[i] + "\"";
    }
    s += "]";
    return s;
}

void writeText(std::filesystem::path const& p, std::string_view text) {
    std::ofstream f{p, std::ios::binary};
    f << text;
}

// True iff ANY emitted diagnostic's message text contains `needle` — the
// resolveLibraries compose proof scans for the pre-stamped CLI library's name
// across every F_FileOpenFailed (order-independent).
bool anyMessageContains(DiagnosticReporter const& rep, std::string_view needle) {
    for (auto const& d : rep.all()) {
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ── `defines` — the real exit-code proof (host-native exec) ──────
// A source whose exit code REFLECTS the define: 42 when DSS_PROJ_GATE is
// defined, 7 otherwise. Built host-native (PE on Windows, ELF on Linux-
// x86_64) and RUN, so exit 42 vs 7 is the exact behavioral discriminator.
// Compiled out on other hosts (arm64/macOS) exactly like the ELF FFI round-
// trip; the compile-only includes/resolveLibraries proofs below run on every
// leg (so the merge-stamp still has cross-target coverage there).
#if defined(_WIN32) || (defined(__linux__) && (defined(__x86_64__) || defined(__amd64__)))
namespace {
#if defined(_WIN32)
constexpr std::string_view kHostExecSpec     = "x86_64:pe64-x86_64-windows-exec";
constexpr std::string_view kHostExecArtifact = "gate.exe";  // PE ⇒ .exe
constexpr std::string_view kHostExecFormat   = "pe64-x86_64-windows-exec";
constexpr std::string_view kHostExecExt      = ".exe";
#else
constexpr std::string_view kHostExecSpec     = "x86_64:elf64-x86_64-linux-exec";
constexpr std::string_view kHostExecArtifact = "gate";      // ELF exec ⇒ no ext
constexpr std::string_view kHostExecFormat   = "elf64-x86_64-linux-exec";
constexpr std::string_view kHostExecExt      = "";
#endif

constexpr std::string_view kGateSrc =
    "int main(void){\n"
    "#ifdef DSS_PROJ_GATE\n"
    "  return 42;\n"
    "#else\n"
    "  return 7;\n"
    "#endif\n"
    "}\n";

// A source whose exit code is the manifest-supplied VALUE of RETVAL — proves
// NAME=VALUE substitution (not just presence). Without the define, RETVAL is an
// undeclared identifier → the source does not compile (the acceptable negative).
constexpr std::string_view kValueSrc =
    "int main(void){ return RETVAL; }\n";

// Build `src` through compileProject with a manifest carrying `defines`, into
// `dir` (setOutputDir → the Cycle-B per-format-subdir artifact at
// <dir>/<formatName>/gate[.exe] — a project build forces the <formatName>/
// subdir for EVERY platform, including single-target). The manifest source path
// is ABSOLUTE (generic separators — valid in JSON and on Windows) so no cwd
// change is needed. Returns the compileProject rc.
int buildDefineProject(std::filesystem::path const& dir,
                       std::string_view src,
                       std::vector<std::string> const& defines,
                       DiagnosticReporter& rep) {
    auto const srcPath = dir / "gate.c";
    writeText(srcPath, src);
    std::string const manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"" + std::string{kHostExecSpec} + "\"],\n"
        + "  \"sources\": [\"" + srcPath.generic_string() + "\"],\n"
        + "  \"defines\": " + jsonStrArray(defines) + "\n}";
    auto const projPath = dir / "gate.dss-project.json";
    writeText(projPath, manifest);

    Program prog;
    prog.setOutputDir(dir);   // project build ⇒ artifact at <dir>/<formatName>/<stem><ext>
    return prog.compileProject(projPath.string(), rep);
}
}  // namespace

TEST(CompileProjectManifestFlags, DefinesThreadToCompileExitCode) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    using dss::test_support::runBinary;

    // (on) manifest defines:["DSS_PROJ_GATE"] → the #ifdef branch → exit 42.
    {
        ScratchDir scratch{Location::InsideRepo, "program"};
        DiagnosticReporter rep;
        ASSERT_EQ(buildDefineProject(scratch.path(), kGateSrc, {"DSS_PROJ_GATE"}, rep), 0)
            << "the gated source must compile with the manifest define";
        auto const exe = scratch.path() / std::string{kHostExecFormat}
                       / std::string{kHostExecArtifact};
        ASSERT_TRUE(std::filesystem::exists(exe)) << exe.string();
        auto const r = runBinary(exe);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u)
            << "manifest defines:[DSS_PROJ_GATE] must define the gate → exit 42";
    }
    // (off) RED-ON-DISABLE: the SAME source, define REMOVED from the manifest
    // → the #else branch → exit 7. If the merge-stamp were dropped (defines
    // never threaded), the (on) case above would ALSO fall to #else and return
    // 7 — so 42-vs-7 is the exact discriminator that the define took effect.
    {
        ScratchDir scratch{Location::InsideRepo, "program"};
        DiagnosticReporter rep;
        ASSERT_EQ(buildDefineProject(scratch.path(), kGateSrc, {}, rep), 0)
            << "the gated source must still compile with no manifest define";
        auto const exe = scratch.path() / std::string{kHostExecFormat}
                       / std::string{kHostExecArtifact};
        ASSERT_TRUE(std::filesystem::exists(exe)) << exe.string();
        auto const r = runBinary(exe);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 7u)
            << "no manifest define → the #else branch → exit 7 (red-on-disable)";
    }
}

// ── `defines` NAME=VALUE substitution — the VALUE reaches codegen ─
// DefinesThreadToCompileExitCode proves a bare `#ifdef` sees the define;
// this proves the VALUE substitutes: manifest defines:["RETVAL=42"] with
// `int main(){ return RETVAL; }` → exit 42. RED-ON-DISABLE (acceptable
// negative): WITHOUT the define, RETVAL is undeclared → the source does not
// compile, so the 42 provably came from the manifest value. Host-gated exactly
// like DefinesThreadToCompileExitCode (it RUNS the artifact).
TEST(CompileProjectManifestFlags, DefineValueSubstitutesExitCode) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    using dss::test_support::runBinary;

    // (value present) RETVAL=42 substitutes → exit 42.
    {
        ScratchDir scratch{Location::InsideRepo, "program"};
        DiagnosticReporter rep;
        ASSERT_EQ(buildDefineProject(scratch.path(), kValueSrc, {"RETVAL=42"}, rep), 0)
            << "the source must compile with the manifest NAME=VALUE define";
        auto const exe = scratch.path() / std::string{kHostExecFormat}
                       / std::string{kHostExecArtifact};
        ASSERT_TRUE(std::filesystem::exists(exe)) << exe.string();
        auto const r = runBinary(exe);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u)
            << "manifest defines:[RETVAL=42] must substitute the VALUE 42 → exit 42";
    }
    // (value absent) RED-ON-DISABLE: no manifest define → RETVAL undeclared →
    // the source does not compile (so the 42 above came from the manifest).
    {
        ScratchDir scratch{Location::InsideRepo, "program"};
        DiagnosticReporter rep;
        EXPECT_NE(buildDefineProject(scratch.path(), kValueSrc, {}, rep), 0)
            << "without the manifest define, RETVAL is undeclared → no compile";
    }
}

// ── `artifactName` + per-platform subdir routing (Cycle B) ───────
namespace {
// Build `int main(){return 42;}` through compileProject with a manifest carrying
// an OPTIONAL artifactName, into `dir` via setOutputDir. RUNS-capable artifact
// (host-native exec). The source stem is "app", so the no-artifactName arm's
// default-stem artifact is <formatName>/app<ext>. Returns the compileProject rc.
int buildRoutingProject(std::filesystem::path const& dir,
                        std::optional<std::string> const& artifactName,
                        DiagnosticReporter& rep) {
    auto const srcPath = dir / "app.c";
    writeText(srcPath, "int main(void){ return 42; }\n");
    std::string manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"" + std::string{kHostExecSpec} + "\"],\n"
        + "  \"sources\": [\"" + srcPath.generic_string() + "\"]";
    if (artifactName) {
        manifest += ",\n  \"artifactName\": \"" + *artifactName + "\"";
    }
    manifest += "\n}";
    auto const projPath = dir / "app.dss-project.json";
    writeText(projPath, manifest);
    Program prog;
    prog.setOutputDir(dir);
    return prog.compileProject(projPath.string(), rep);
}
}  // namespace

// A project build routes each target's artifact to
// <output>/<formatName>/<artifactName-or-stem><ext> — the per-format subdir is
// forced for EVERY project build, INCLUDING single-target. Host-gated (RUNS the
// artifact) exactly like the defines exit-code proof.
//
// RED-ON-DISABLE (two independent levers, both caught by asserting the routed
// path EXISTS + the flat path does NOT):
//   * artifactName threading dropped → the binary keeps the SOURCE STEM
//     ("app") ⇒ <formatName>/myapp<ext> is absent ⇒ the "with" arm goes RED.
//   * perFormatOutputSubdir dropped → the subdir decision falls back to
//     multiTargetBuild-only ⇒ a single-target build lands FLAT at
//     <dir>/myapp<ext> (or <dir>/app<ext>) ⇒ the routed <formatName>/ path is
//     absent AND the asserted-absent flat path now EXISTS ⇒ both arms go RED.
TEST(CompileProjectManifestFlags, ArtifactNameRoutesToPerFormatSubdir) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    using dss::test_support::runBinary;

    // (with artifactName "myapp") → <dir>/<formatName>/myapp<ext>, runs → 42.
    {
        ScratchDir scratch{Location::InsideRepo, "program"};
        auto const dir = scratch.path();
        DiagnosticReporter rep;
        ASSERT_EQ(buildRoutingProject(dir, std::string{"myapp"}, rep), 0)
            << "the artifactName project must compile + emit";
        auto const routed = dir / std::string{kHostExecFormat}
                          / (std::string{"myapp"} + std::string{kHostExecExt});
        ASSERT_TRUE(std::filesystem::exists(routed))
            << "artifactName must name the binary AND land under <formatName>/; "
               "expected " << routed.string();
        // Forced-subdir proof: a "multiTargetBuild only" regression would place
        // a single-target build FLAT here — assert it is NOT.
        auto const flat = dir / (std::string{"myapp"} + std::string{kHostExecExt});
        EXPECT_FALSE(std::filesystem::exists(flat))
            << "a single-target PROJECT build must be under <formatName>/, not flat";
        auto const r = runBinary(routed);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "the routed artifact must run → exit 42";
    }
    // (no artifactName) → the SOURCE STEM ("app") names the binary, STILL under
    // <formatName>/ (the forced subdir). myapp-vs-app is the name discriminator;
    // the subdir presence is the forced-subdir proof.
    {
        ScratchDir scratch{Location::InsideRepo, "program"};
        auto const dir = scratch.path();
        DiagnosticReporter rep;
        ASSERT_EQ(buildRoutingProject(dir, std::nullopt, rep), 0)
            << "the no-artifactName project must compile + emit";
        auto const routed = dir / std::string{kHostExecFormat}
                          / (std::string{"app"} + std::string{kHostExecExt});
        ASSERT_TRUE(std::filesystem::exists(routed))
            << "default-stem artifact must land under <formatName>/; expected "
            << routed.string();
        auto const flat = dir / (std::string{"app"} + std::string{kHostExecExt});
        EXPECT_FALSE(std::filesystem::exists(flat))
            << "a single-target PROJECT build must be under <formatName>/, not flat";
    }
}

#endif  // host-native exec (Windows or Linux-x86_64)

// ── CLI path unchanged — single-target `--compile` stays FLAT ────
// Cycle B forces the per-format subdir for PROJECT builds only. The CLI path
// (compileFiles/compileUnits with setOutputDir, NO setPerFormatOutputSubdir /
// artifactName — exactly what Program::run stamps) must be byte-identical: a
// single-target compile still emits FLAT at <outputDir>/<sourceStem><ext>, NOT
// under a <formatName>/ subdir. Host-agnostic (fixed cross-target ELF, never
// run) so it pins the CLI layout on every leg. The disk-side pipeline twin is
// test_compile_pipeline.cpp's
// `Program_CompileFiles.OutputFlagSingleTargetPlacesArtifactFlat`.
TEST(CompileProjectManifestFlags, CliSingleTargetStaysFlatNotSubdir) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    std::string const target = "x86_64:elf64-x86_64-linux-exec";  // ELF exec ⇒ no ext

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    auto const srcPath = dir / "cliflat.c";
    writeText(srcPath, "int main(void){ return 0; }\n");
    auto const outDir = dir / "out";

    Program prog;
    prog.setOutputDir(outDir);   // NO setPerFormatOutputSubdir / setArtifactName
    DiagnosticReporter rep;
    int const rc = prog.compileFiles({srcPath.string()}, "c", {target}, rep);
    ASSERT_EQ(rc, 0) << "the CLI single-target compile must succeed";
    // FLAT: <outDir>/cliflat (source stem, no <formatName>/ subdir interposed).
    EXPECT_TRUE(std::filesystem::exists(outDir / "cliflat"))
        << "CLI single-target must stay flat at <outDir>/<stem>";
    EXPECT_FALSE(std::filesystem::exists(
        outDir / "elf64-x86_64-linux-exec" / "cliflat"))
        << "the project per-format subdir must NOT leak into the CLI path";
}

// ── `artifactName` containment BOUNDARY (Cycle B robustness) ─────
// The loader validates `artifactName` as a bare name (rejects '/' and '\'), but
// a separator denylist does NOT prove containment: a bare ".." has no separator,
// so it SURVIVES the loader, yet `outDir / ".."` normalizes to outDir's PARENT —
// an escape from the routed output tree. The routing site's containment postcheck
// (compileOneTarget) must reject it fail-loud. Host-agnostic (fixed cross-target
// ELF exec ⇒ ext = "" so ".." stays "..", never run) so it pins the boundary on
// every leg. The same postcheck also covers the Windows drive-relative ("D:app")
// and NTFS-ADS vectors uniformly (a lexically-normalized parent-path compare, not
// a per-vector denylist) — not separately unit-tested here to avoid drive-letter
// fragility.
//
// RED-ON-DISABLE: delete the containment postcheck and this exact-code count
// drops to 0 — the routing would instead carry `<outDir>/..` to the linker,
// which then fails with an unrelated write error (or, on a writable escape
// target, LEAKS a file outside --output). The `D_ArtifactNameEscapesOutputDir`
// count is the lever.
TEST(CompileProjectManifestFlags, ArtifactNameDotDotEscapeFailsLoud) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    std::string const target = "x86_64:elf64-x86_64-linux-exec";  // ELF exec ⇒ no ext

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    auto const srcPath = dir / "esc.c";
    writeText(srcPath, "int main(void){ return 0; }\n");
    auto const outDir = dir / "out";
    std::string const manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"" + target + "\"],\n"
        + "  \"sources\": [\"" + srcPath.generic_string() + "\"],\n"
        + "  \"artifactName\": \"..\"\n}";
    auto const proj = dir / "esc.dss-project.json";
    writeText(proj, manifest);

    Program prog;
    prog.setOutputDir(outDir);
    DiagnosticReporter rep;
    int const rc = prog.compileProject(proj.string(), rep);
    EXPECT_NE(rc, 0)
        << "a '..' artifactName escapes --output and must fail the build";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactNameEscapesOutputDir), 1u)
        << "the routing containment boundary must reject the '..' escape";
    // No artifact leaked: the '..' resolves toward <out> (the routed subdir's
    // parent); assert <out> holds only the <formatName>/ subdir, no emitted file.
    bool leaked = false;
    if (std::filesystem::exists(outDir)) {
        for (auto const& e : std::filesystem::directory_iterator(outDir)) {
            if (e.is_regular_file()) { leaked = true; break; }
        }
    }
    EXPECT_FALSE(leaked)
        << "no artifact must be written outside the routed <formatName>/ subdir";
}

// ── `includes` — compile-only proof (host-agnostic, every leg) ───
// The test_include_dirs.cpp pattern driven through compileProject: a quote-
// include resolvable ONLY via the manifest include dir. A fixed cross-target
// ELF (never RUN), so it compiles on every host/leg. RED-ON-DISABLE: drop the
// includes merge-stamp and the "with" case regresses to the P0016 of "without".
TEST(CompileProjectManifestFlags, IncludesThreadToCompile) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    std::string const target = "x86_64:elf64-x86_64-linux-exec";

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    std::filesystem::create_directories(dir / "inc");
    writeText(dir / "inc" / "gate_hdr.h", "#define ANSWER 42\n");
    auto const srcPath = dir / "uses_inc.c";
    writeText(srcPath,
              "#include \"gate_hdr.h\"\nint main(void){ return ANSWER; }\n");

    auto manifest = [&](std::vector<std::string> const& includes) {
        return std::string{"{\n  \"language\": \"c\",\n"}
            + "  \"artifactProfile\": \"cli\",\n"
            + "  \"targets\": [\"" + target + "\"],\n"
            + "  \"sources\": [\"" + srcPath.generic_string() + "\"],\n"
            + "  \"includes\": " + jsonStrArray(includes) + "\n}";
    };

    // (without) the manifest include dir → the quote-include fails loud P0016.
    {
        auto const proj = dir / "no_inc.dss-project.json";
        writeText(proj, manifest({}));
        Program prog;
        prog.setOutputDir(dir / "out_no");
        DiagnosticReporter rep;
        int const rc = prog.compileProject(proj.string(), rep);
        EXPECT_NE(rc, 0)
            << "a quote-include unreachable without the manifest include dir must fail";
        EXPECT_GT(countCode(rep, DiagnosticCode::P_PreprocessorIncludeError), 0u)
            << "the fail-loud must be the P0016 quote-include-not-found";
    }
    // (with) the manifest include dir → resolves + compiles clean (rc 0).
    {
        auto const proj = dir / "with_inc.dss-project.json";
        writeText(proj, manifest({(dir / "inc").generic_string()}));
        Program prog;
        prog.setOutputDir(dir / "out_yes");
        DiagnosticReporter rep;
        int const rc = prog.compileProject(proj.string(), rep);
        ASSERT_EQ(rc, 0) << "the manifest include dir must resolve the quote-include";
        EXPECT_EQ(countCode(rep, DiagnosticCode::P_PreprocessorIncludeError), 0u);
    }
}

// ── `resolveLibraries` — compile-only proof (host-agnostic) ──────
// The eager --resolve-library path probe opens every named library at compile
// time, EVEN when the TU has no externs (the FFI MissingResolveLibraryPath...
// pin). So a manifest naming a NONEXISTENT library must fail loud
// F_FileOpenFailed — proving resolveLibraries threaded to the compile.
// RED-ON-DISABLE: with resolveLibraries absent/[] the same source compiles
// clean. Fixed cross-target ELF (never RUN).
TEST(CompileProjectManifestFlags, ResolveLibrariesThreadToCompile) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    std::string const target = "x86_64:elf64-x86_64-linux-exec";

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    auto const srcPath = dir / "noextern.c";
    writeText(srcPath, "int main(void){ return 7; }\n");
    auto const missing = dir / "does_not_exist_lib.bin";  // never created

    auto manifest = [&](std::vector<std::string> const& libs) {
        return std::string{"{\n  \"language\": \"c\",\n"}
            + "  \"artifactProfile\": \"cli\",\n"
            + "  \"targets\": [\"" + target + "\"],\n"
            + "  \"sources\": [\"" + srcPath.generic_string() + "\"],\n"
            + "  \"resolveLibraries\": " + jsonStrArray(libs) + "\n}";
    };

    // (with a MISSING lib) → the eager path probe fails loud F_FileOpenFailed.
    {
        auto const proj = dir / "with_lib.dss-project.json";
        writeText(proj, manifest({missing.generic_string()}));
        Program prog;
        prog.setOutputDir(dir / "out_lib");
        DiagnosticReporter rep;
        int const rc = prog.compileProject(proj.string(), rep);
        EXPECT_NE(rc, 0)
            << "a missing manifest resolveLibraries path must fail the compile";
        EXPECT_GT(countCode(rep, DiagnosticCode::F_FileOpenFailed), 0u)
            << "the eager --resolve-library probe must see the manifest lib (threaded)";
    }
    // (without) RED-ON-DISABLE: no manifest library → the same source compiles
    // clean (no eager probe fires).
    {
        auto const proj = dir / "no_lib.dss-project.json";
        writeText(proj, manifest({}));
        Program prog;
        prog.setOutputDir(dir / "out_nolib");
        DiagnosticReporter rep;
        int const rc = prog.compileProject(proj.string(), rep);
        EXPECT_EQ(rc, 0)
            << "no manifest resolveLibraries → clean compile (red-on-disable)";
        EXPECT_EQ(countCode(rep, DiagnosticCode::F_FileOpenFailed), 0u);
    }
}

// ════════════════════════════════════════════════════════════════
// MERGE (append), not replace — the locked threading semantics. The
// strong proof is COMPOSITION of two NON-EMPTY sets: a pre-stamped CLI
// value (as if `Program::run` stamped a CLI flag) and a DIFFERENT
// manifest value, where BOTH are required for success. A guarded-replace
// regression — e.g. `if (!pc.includes.empty()) setIncludeDirs(pc.includes)`
// — would drop the pre-stamped CLI value yet pass an empty-array test, so
// the empty-array case alone is NOT sufficient. One compose test per field.
// ════════════════════════════════════════════════════════════════

// `includes` compose: pre-stamp CLI dirA (holds a.h) + manifest dirB (holds
// b.h); a source needs a header from EACH. Both resolve → the two dir sets
// composed. RED-ON-DISABLE (manifest-only replace): dirA dropped → a.h
// unresolved → P0016 (asserted directly as the "no pre-stamp" negative below).
TEST(CompileProjectManifestFlags, IncludesMergeComposesPreexistingWithManifest) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    std::string const target = "x86_64:elf64-x86_64-linux-exec";

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    std::filesystem::create_directories(dir / "dirA");
    std::filesystem::create_directories(dir / "dirB");
    writeText(dir / "dirA" / "a.h", "#define A_OK 1\n");
    writeText(dir / "dirB" / "b.h", "#define B_OK 1\n");
    auto const srcPath = dir / "uses_both.c";
    // a.h resolves ONLY via dirA (the pre-stamped CLI dir); b.h ONLY via dirB
    // (the manifest dir) — both are required, so the compile succeeds iff the
    // two dir sets composed.
    writeText(srcPath,
              "#include \"a.h\"\n#include \"b.h\"\n"
              "int main(void){ return A_OK + B_OK - 2; }\n");

    auto manifest = [&](std::vector<std::string> const& includes) {
        return std::string{"{\n  \"language\": \"c\",\n"}
            + "  \"artifactProfile\": \"cli\",\n"
            + "  \"targets\": [\"" + target + "\"],\n"
            + "  \"sources\": [\"" + srcPath.generic_string() + "\"],\n"
            + "  \"includes\": " + jsonStrArray(includes) + "\n}";
    };

    // (compose) CLI dirA pre-stamped + manifest dirB → BOTH a.h and b.h resolve.
    {
        auto const proj = dir / "compose.dss-project.json";
        writeText(proj, manifest({(dir / "dirB").generic_string()}));
        Program prog;
        prog.setIncludeDirs({(dir / "dirA").generic_string()});  // as if CLI -I dirA
        prog.setOutputDir(dir / "out_compose");
        DiagnosticReporter rep;
        int const rc = prog.compileProject(proj.string(), rep);
        ASSERT_EQ(rc, 0)
            << "the pre-stamped CLI dir (dirA) and the manifest dir (dirB) must "
               "COMPOSE — both a.h and b.h resolve (append, not replace)";
        EXPECT_EQ(countCode(rep, DiagnosticCode::P_PreprocessorIncludeError), 0u);
    }
    // (negative — the manifest-only replace direction) NO pre-stamp → only
    // dirB is in effect → a.h is unreachable → P0016. This is exactly what a
    // guarded-replace regression would reduce the compose case to.
    {
        auto const proj = dir / "noprestamp.dss-project.json";
        writeText(proj, manifest({(dir / "dirB").generic_string()}));
        Program prog;  // no setIncludeDirs — only the manifest dirB
        prog.setOutputDir(dir / "out_noprestamp");
        DiagnosticReporter rep;
        int const rc = prog.compileProject(proj.string(), rep);
        EXPECT_NE(rc, 0)
            << "with only the manifest dir (dirB), a.h is unreachable → fail";
        EXPECT_GT(countCode(rep, DiagnosticCode::P_PreprocessorIncludeError), 0u);
    }
}

// `defines` compose: pre-stamp CLI `CLI_ONE=1` + manifest `MANIFEST_TWO=2`, with
// a source that references BOTH macros. Compose → `return 1 + 2 - 3;` compiles.
// RED-ON-DISABLE (manifest-only replace): CLI_ONE dropped → an undeclared
// identifier → compile error (asserted as the "no pre-stamp" negative below).
// Host-agnostic (compile-only, cross-target ELF).
TEST(CompileProjectManifestFlags, DefinesMergeComposesPreexistingWithManifest) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    std::string const target = "x86_64:elf64-x86_64-linux-exec";

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    auto const srcPath = dir / "uses_both_defs.c";
    // Both macros must be defined or the identifier is undeclared (compile
    // error) — the compose requirement. (=0 keeps the exit clean if ever run.)
    writeText(srcPath, "int main(void){ return CLI_ONE + MANIFEST_TWO - 3; }\n");

    auto manifest = [&](std::vector<std::string> const& defines) {
        return std::string{"{\n  \"language\": \"c\",\n"}
            + "  \"artifactProfile\": \"cli\",\n"
            + "  \"targets\": [\"" + target + "\"],\n"
            + "  \"sources\": [\"" + srcPath.generic_string() + "\"],\n"
            + "  \"defines\": " + jsonStrArray(defines) + "\n}";
    };

    // (compose) CLI CLI_ONE=1 pre-stamped + manifest MANIFEST_TWO=2 → both
    // substitute → the source compiles.
    {
        auto const proj = dir / "compose_def.dss-project.json";
        writeText(proj, manifest({"MANIFEST_TWO=2"}));
        Program prog;
        prog.setUserDefines({"CLI_ONE=1"});  // as if CLI --define CLI_ONE=1
        prog.setOutputDir(dir / "out_compose_def");
        DiagnosticReporter rep;
        int const rc = prog.compileProject(proj.string(), rep);
        ASSERT_EQ(rc, 0)
            << "the pre-stamped CLI define (CLI_ONE) and the manifest define "
               "(MANIFEST_TWO) must COMPOSE — both substitute (append, not replace)";
    }
    // (negative — the manifest-only replace direction) NO pre-stamp → CLI_ONE
    // undeclared → compile fails.
    {
        auto const proj = dir / "nocli_def.dss-project.json";
        writeText(proj, manifest({"MANIFEST_TWO=2"}));
        Program prog;  // no setUserDefines — only the manifest define
        prog.setOutputDir(dir / "out_nocli_def");
        DiagnosticReporter rep;
        int const rc = prog.compileProject(proj.string(), rep);
        EXPECT_NE(rc, 0)
            << "with only the manifest define, CLI_ONE is undeclared → fail";
    }
}

// `resolveLibraries` compose: pre-stamp CLI `cli_only_lib.bin` + manifest
// `manifest_only_lib.bin`, BOTH nonexistent. The eager --resolve-library probe
// opens EVERY entry of the merged list, so under compose the CLI path is probed
// and its name appears in a F_FileOpenFailed diagnostic. RED-ON-DISABLE
// (manifest-only replace): the CLI path is dropped → never probed → its name
// never appears (the CLI path is FIRST in the merge, so this holds whether the
// probe reports all failures or stops at the first). Host-agnostic.
TEST(CompileProjectManifestFlags, ResolveLibrariesMergeComposesPreexistingWithManifest) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    std::string const target = "x86_64:elf64-x86_64-linux-exec";

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    auto const srcPath = dir / "noextern2.c";
    writeText(srcPath, "int main(void){ return 0; }\n");
    auto const cliLib      = dir / "cli_only_lib.bin";       // never created
    auto const manifestLib = dir / "manifest_only_lib.bin";  // never created

    std::string const manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"" + target + "\"],\n"
        + "  \"sources\": [\"" + srcPath.generic_string() + "\"],\n"
        + "  \"resolveLibraries\": "
        + jsonStrArray({manifestLib.generic_string()}) + "\n}";
    auto const proj = dir / "compose_lib.dss-project.json";
    writeText(proj, manifest);

    Program prog;
    prog.setResolveLibraries({cliLib});  // as if CLI --resolve-library cli_only_lib.bin
    prog.setOutputDir(dir / "out_compose_lib");
    DiagnosticReporter rep;
    int const rc = prog.compileProject(proj.string(), rep);
    EXPECT_NE(rc, 0) << "both missing libs must fail the eager probe";
    EXPECT_GT(countCode(rep, DiagnosticCode::F_FileOpenFailed), 0u);
    EXPECT_TRUE(anyMessageContains(rep, "cli_only_lib"))
        << "the pre-stamped CLI library must SURVIVE the manifest merge — its "
           "path is probed (append, not replace); a manifest-only replace would "
           "drop it and its name would never appear in a diagnostic";
}

// ════════════════════════════════════════════════════════════════
// `stackReserve` PRECEDENCE — the CLI `--stack-reserve` WINS
// (D-SQLITE-PE64-FULL-TIER-STACK-DEPTH)
//
// The three flag ARRAYS above MERGE because appending composes. A SCALAR
// cannot compose — one of the two numbers must be the answer — so it carries
// an explicit rule instead: `Program::run` stamps the CLI flag, and
// `compileProject` applies the manifest's `stackReserve` ONLY IF that stamp
// left it unset. These tests drive the rule at the Program tier by
// pre-stamping `setStackReserveBytes` exactly as `Program::run` would.
//
// TWO independent assertions per case, because the getter alone would still
// pass if the value never reached the pipeline:
//   (a) `prog.stackReserveBytes()` holds the WINNING number after the call;
//   (b) the number the LINKER was handed is that same winner — read back out
//       of the linker's own refusal message, which quotes the requested byte
//       count. So the winner is proven to have travelled Program → CU build →
//       ImageRequest → linker, not merely to have survived in a member.
//
// The (b) channel exists because the fixed cross-target here is an ELF exec,
// which declares NO `stackReserveControl` capability — so ANY request is
// REFUSED fail-loud (K_FormatLacksStackReserveControl) rather than silently
// dropped. That refusal is exactly what makes the requested value observable
// on every leg without running a binary. (If ELF ever gained the capability,
// these two arms would need re-pointing at a format that has none — the
// third case below is unaffected.)
//
// The NEITHER case is the control: it compiles CLEAN (rc 0, zero refusals),
// proving the failures above are CAUSED by the threaded request and that a
// build with no request is unchanged behavior.
// ════════════════════════════════════════════════════════════════

namespace {

// Build `int main(void){return 0;}` through compileProject against a fixed
// cross-target ELF exec (host-agnostic, never RUN), with an OPTIONAL manifest
// `stackReserve` and an OPTIONAL pre-stamped CLI value. `prog` is the
// CALLER's so `stackReserveBytes()` is readable after the call. Returns the
// compileProject rc.
int buildStackReserveProject(std::filesystem::path const& dir,
                             std::optional<std::uint64_t> manifestReserve,
                             std::optional<std::uint64_t> cliReserve,
                             Program& prog,
                             DiagnosticReporter& rep) {
    std::string const target = "x86_64:elf64-x86_64-linux-exec";
    auto const srcPath = dir / "stack.c";
    writeText(srcPath, "int main(void){ return 0; }\n");
    std::string manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"" + target + "\"],\n"
        + "  \"sources\": [\"" + srcPath.generic_string() + "\"]";
    if (manifestReserve) {
        manifest += ",\n  \"stackReserve\": " + std::to_string(*manifestReserve);
    }
    manifest += "\n}";
    auto const projPath = dir / "stack.dss-project.json";
    writeText(projPath, manifest);

    if (cliReserve) {
        prog.setStackReserveBytes(cliReserve);  // as if CLI --stack-reserve <n>
    }
    prog.setOutputDir(dir / "out");
    return prog.compileProject(projPath.string(), rep);
}

constexpr std::uint64_t kCliReserve      = 8388608;  // 8 MiB — the CLI number
constexpr std::uint64_t kManifestReserve = 4194304;  // 4 MiB — the manifest's

}  // namespace

// (CLI + manifest, DIFFERENT values) → the CLI value wins, end to end.
// RED-ON-DISABLE: drop the `!stackReserveBytes().has_value()` guard in
// `Program::compileProject` (i.e. let the manifest clobber unconditionally)
// and BOTH the getter and the linker message flip to the manifest's 4194304.
TEST(CompileProjectManifestFlags, StackReserveCliWinsOverManifest) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    ScratchDir scratch{Location::InsideRepo, "program"};
    Program prog;
    DiagnosticReporter rep;
    int const rc = buildStackReserveProject(
        scratch.path(), kManifestReserve, kCliReserve, prog, rep);

    // (a) the surviving stamp is the CLI value — the manifest never overwrote it.
    ASSERT_TRUE(prog.stackReserveBytes().has_value());
    EXPECT_EQ(*prog.stackReserveBytes(), kCliReserve)
        << "the CLI --stack-reserve must WIN over the manifest 'stackReserve'";

    // (b) that same value is what reached the linker (quoted in its refusal).
    EXPECT_NE(rc, 0)
        << "an ELF image declares no stackReserveControl — the request must be "
           "REFUSED, never silently dropped";
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_FormatLacksStackReserveControl), 1u);
    EXPECT_TRUE(anyMessageContains(rep, std::to_string(kCliReserve)))
        << "the linker must have been handed the CLI value (8388608)";
    EXPECT_FALSE(anyMessageContains(rep, std::to_string(kManifestReserve)))
        << "the manifest value (4194304) must never reach the linker when the "
           "CLI supplied one";
}

// (manifest only, NO CLI stamp) → the manifest value lands, end to end.
// RED-ON-DISABLE: drop the manifest stamp in `Program::compileProject` and the
// getter falls to nullopt AND the refusal disappears (the build goes green).
TEST(CompileProjectManifestFlags, StackReserveManifestLandsWithoutCliStamp) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    ScratchDir scratch{Location::InsideRepo, "program"};
    Program prog;   // no setStackReserveBytes — the manifest is the only source
    DiagnosticReporter rep;
    int const rc = buildStackReserveProject(
        scratch.path(), kManifestReserve, std::nullopt, prog, rep);

    ASSERT_TRUE(prog.stackReserveBytes().has_value())
        << "with no CLI stamp the manifest 'stackReserve' must apply";
    EXPECT_EQ(*prog.stackReserveBytes(), kManifestReserve);

    EXPECT_NE(rc, 0);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_FormatLacksStackReserveControl), 1u);
    EXPECT_TRUE(anyMessageContains(rep, std::to_string(kManifestReserve)))
        << "the linker must have been handed the manifest value (4194304)";
}

// (NEITHER) the control — no request anywhere ⇒ nullopt, the build compiles
// CLEAN, and the format-capability gate never fires. Without this arm the two
// cases above could not distinguish "the request was threaded" from "this
// build fails for some unrelated reason".
TEST(CompileProjectManifestFlags, StackReserveAbsentEverywhereCompilesClean) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    ScratchDir scratch{Location::InsideRepo, "program"};
    Program prog;
    DiagnosticReporter rep;
    int const rc = buildStackReserveProject(
        scratch.path(), std::nullopt, std::nullopt, prog, rep);

    EXPECT_FALSE(prog.stackReserveBytes().has_value())
        << "no CLI flag and no manifest key ⇒ nullopt ⇒ the format default stands";
    EXPECT_EQ(rc, 0) << "an unrequested stack reserve must not disturb the build";
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_FormatLacksStackReserveControl), 0u);
}

// ════════════════════════════════════════════════════════════════
// D-AP2-SOURCES-GLOB — `sources[]` glob expansion (Cycle C)
//
// A manifest source entry may be a glob PATTERN (`"src/**/*.c"`); the driver
// (`Program::compileProject`) expands it against the filesystem BEFORE the
// multi-vs-single-CU routing count is taken, so a pattern routes exactly as if
// its matches had been listed literally. The LOADER stays a pure JSON parser
// (holds the raw pattern); the filesystem side is a driver pre-pass over the
// `core/types/glob_match.hpp` matcher + expander.
//
// Three test tiers, cheapest first:
//   1. the LOADER keeps a glob string verbatim (no expansion);
//   2. DIRECT unit tests of the pure matcher (`globMatch` / `hasGlobMetacharacters`)
//      and the filesystem expander (`expandGlob`) — the strongest coverage;
//   3. BEHAVIORAL compileProject proofs (route/compile-level host-agnostic; one
//      host-gated build+run of a 2-file glob that proves BOTH TUs linked).
// ════════════════════════════════════════════════════════════════

// ── 1. Loader keeps a glob string verbatim ──────────────────────
// A glob string is a valid non-empty source string; the loader must NOT expand
// it (that is the driver's job). Pins that pc.sources holds the raw pattern.
TEST(ProjectConfigLoader, GlobSourceStringKeptVerbatim) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["src/**/*.c"]
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(pc->sources.size(), 1u);
    EXPECT_EQ(pc->sources[0], "src/**/*.c")
        << "the loader must NOT expand — it holds the raw pattern (expansion is "
           "the driver's job in compileProject)";
}

// ── 2a. The literal-vs-glob predicate (hasGlobMetacharacters) ────
TEST(GlobMatch, HasMetacharactersDetectsGlobVsLiteral) {
    EXPECT_TRUE(hasGlobMetacharacters("*.c"));
    EXPECT_TRUE(hasGlobMetacharacters("a?b.c"));
    EXPECT_TRUE(hasGlobMetacharacters("[abc].c"));
    EXPECT_TRUE(hasGlobMetacharacters("src/**/*.c"));
    // literals — kept verbatim by the driver (no filesystem walk).
    EXPECT_FALSE(hasGlobMetacharacters("main.c"));
    EXPECT_FALSE(hasGlobMetacharacters("src/sub/file-name_1.2.c"));
    EXPECT_FALSE(hasGlobMetacharacters("/abs/path/to/main.c"));
    EXPECT_FALSE(hasGlobMetacharacters("C:/win/path/main.c"));
}

// ── 2b. `*` stays within one path segment (never crosses '/') ────
TEST(GlobMatch, StarStaysWithinOneSegment) {
    EXPECT_TRUE(globMatch("*.c", "x.c"));
    EXPECT_TRUE(globMatch("*.c", "main.c"));
    EXPECT_TRUE(globMatch("a*", "abc"));
    EXPECT_TRUE(globMatch("a*d", "ad"));       // '*' matches empty
    EXPECT_FALSE(globMatch("*.c", "x.h"));
    EXPECT_FALSE(globMatch("*.c", "sub/x.c"))
        << "a single '*' must NOT cross a path separator";
    EXPECT_TRUE(globMatch("src/*.c", "src/x.c"));
    EXPECT_FALSE(globMatch("src/*.c", "src/sub/x.c"));
}

// ── 2c. `**` crosses segments (recursive) ───────────────────────
TEST(GlobMatch, DoubleStarCrossesSegments) {
    EXPECT_TRUE(globMatch("**/*.c", "x.c"));       // ** matches zero segments
    EXPECT_TRUE(globMatch("**/*.c", "sub/x.c"));
    EXPECT_TRUE(globMatch("**/*.c", "a/b/c/x.c"));
    EXPECT_TRUE(globMatch("a/**/b", "a/b"));       // zero intermediate
    EXPECT_TRUE(globMatch("a/**/b", "a/x/b"));
    EXPECT_TRUE(globMatch("a/**/b", "a/x/y/b"));
    EXPECT_FALSE(globMatch("a/**/b", "a/x/y/c"));
    EXPECT_FALSE(globMatch("**/*.c", "x.h"));
}

// ── 2d. `?` and `[...]` character classes ───────────────────────
TEST(GlobMatch, QuestionAndCharClasses) {
    EXPECT_TRUE(globMatch("?.c", "a.c"));
    EXPECT_FALSE(globMatch("?.c", "ab.c"));        // '?' is EXACTLY one char
    EXPECT_TRUE(globMatch("[a-c].c", "b.c"));
    EXPECT_FALSE(globMatch("[a-c].c", "d.c"));
    EXPECT_TRUE(globMatch("[!a-c].c", "d.c"));     // negation
    EXPECT_FALSE(globMatch("[!a-c].c", "a.c"));
    EXPECT_TRUE(globMatch("[abc]x", "bx"));
    EXPECT_TRUE(globMatch("v[0-9].c", "v7.c"));
    EXPECT_FALSE(globMatch("v[0-9].c", "vx.c"));
}

// ── 2e. Literal segments + anchored-vs-floating patterns ────────
TEST(GlobMatch, LiteralAndAnchoredVsFloating) {
    EXPECT_TRUE(globMatch("main.c", "main.c"));
    EXPECT_FALSE(globMatch("main.c", "main.h"));
    EXPECT_FALSE(globMatch("main.c", "x/main.c"))   // anchored — no leading dirs
        << "a literal pattern is anchored: it must match the WHOLE relative path";
    EXPECT_TRUE(globMatch("**/main.c", "x/main.c")); // floating via **
    EXPECT_TRUE(globMatch("src/*/main.c", "src/a/main.c"));
    EXPECT_FALSE(globMatch("src/*/main.c", "src/main.c"));
}

// ── 2e². Multi-`*` backtracking within a segment ────────────────
// Two-pointer wildcard backtracking must handle several stars in one segment.
TEST(GlobMatch, MultiStarBacktrackingWithinSegment) {
    EXPECT_TRUE(globMatch("a*b*c", "abc"));       // both stars empty
    EXPECT_TRUE(globMatch("a*b*c", "axxbyyc"));
    EXPECT_TRUE(globMatch("*a*", "baaa"));
    EXPECT_TRUE(globMatch("a*a*a", "aaaa"));       // greedy first star must backtrack
    EXPECT_TRUE(globMatch("a*a*a", "aXaYa"));
    EXPECT_FALSE(globMatch("a*b*c", "axxbyy"));    // no trailing 'c'
    EXPECT_FALSE(globMatch("a*a*a", "aa"));        // too few 'a's for a-a-a
}

// ── 2e³. Multi-`**` backtracking across segments ────────────────
TEST(GlobMatch, MultiDoubleStarBacktracking) {
    EXPECT_TRUE(globMatch("**/a/b", "a/x/a/b"));   // the ** must re-anchor 'a/b'
    EXPECT_TRUE(globMatch("**/a/**/b", "x/a/y/z/b"));
    EXPECT_TRUE(globMatch("**/a/b", "a/b"));       // ** matches zero segments
    EXPECT_FALSE(globMatch("**/a/b", "a/x/b"));    // no 'a' immediately before 'b'
    EXPECT_FALSE(globMatch("**/a/b", "a/b/c"));    // trailing 'c'
}

// ── 2e⁴. Char-class forms: `[^…]`, literal-`]`-first, unterminated `[` ─
// These forms are implemented; pin each so a matcher regression is caught.
TEST(GlobMatch, CharClassNegationCaretAndLiteralBracketForms) {
    // `[^...]` negation — the same semantics as `[!...]`, the other spelling.
    EXPECT_TRUE(globMatch("[^a-c].c", "d.c"));
    EXPECT_FALSE(globMatch("[^a-c].c", "b.c"));
    // a literal ']' as the FIRST class member: `[]abc]` matches ']' or a/b/c.
    EXPECT_TRUE(globMatch("[]abc]", "]"));
    EXPECT_TRUE(globMatch("[]abc]", "b"));
    EXPECT_FALSE(globMatch("[]abc]", "d"));
    // an UNTERMINATED '[' (no closing ']') is a literal '['.
    EXPECT_TRUE(globMatch("[abc", "[abc"));
    EXPECT_FALSE(globMatch("[abc", "a"));
}

// ── 2f. expandGlob: two matches, SORTED + deduped, feed the route ─
// The EXPANDED count is what `routesToMultiUnit` sees — the crux of this cycle.
TEST(ExpandGlob, TwoMatchesSortedDedupedRoutesMulti) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::Temp, "program"};
    auto const dir = scratch.path();
    writeText(dir / "b.c", "int b(void){return 0;}\n");
    writeText(dir / "a.c", "int a(void){return 0;}\n");
    writeText(dir / "note.txt", "not a source\n");   // must NOT match *.c

    std::vector<std::string> out;
    std::error_code ec;
    ASSERT_TRUE(expandGlob((dir / "*.c").generic_string(), out, ec));
    EXPECT_FALSE(ec);
    ASSERT_EQ(out.size(), 2u) << "the glob matches a.c + b.c, excludes note.txt";
    EXPECT_EQ(std::filesystem::path{out[0]}.filename().string(), "a.c")
        << "sorted lexicographically → a.c before b.c (deterministic CU order)";
    EXPECT_EQ(std::filesystem::path{out[1]}.filename().string(), "b.c");
    EXPECT_TRUE(routesToMultiUnit(out.size()))
        << "2 matches → the multi-CU route, exactly as two literal sources would";
}

// ── [[D-PATH-MULTI-SEPARATOR-ROOT-COLLAPSED-BY-STDLIB-PATH-TRANSFORMS]] ─────
//
// A GLOB OVER A SHARE MUST YIELD SOURCE PATHS THAT STILL NAME THE SHARE. What
// `expandGlob` appends is not a message — it is the list of files the compiler
// then OPENS, so a collapsed authority here is a build that cannot find its own
// sources, reported as a per-file open failure naming a local path nobody wrote.
//
// ★★★ THE DEFECT, ✔MEASURED 2026-08-28 on a REACHABLE UNC directory with the
// toolchain that builds DSS. The match was rendered
// `it->path().lexically_normal().generic_string()` — BOTH transforms, and each
// removes one separator of the run:
//     child                        '//localhost/C$/…/leaf.c'    run 2
//     child.lexically_normal()     '\localhost\C$\…\leaf.c'     run 1
//     …then .generic_string()      '/localhost/C$/…/leaf.c'     run 1
// ⚠ FIXING EITHER ONE ALONE READS EXACTLY LIKE FIXING BOTH, because both wrong
// spellings are equally absent from disk — which is why the pin is on the RUN.
//
// ⚠ THE TAIL KEY IS DELIBERATELY NOT PART OF THIS CLAIM. `lexically_relative`
// yields a path with NO root (✔MEASURED: `leaf.c`, run 0), so the plain spelling
// is exact there and this test must not be read as covering it.
TEST(ExpandGlob, MatchKeepsAMultiSeparatorRoot) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::Temp, "program-glob-unc"};
    writeText(scratch.path() / "a.c", "int a(void){return 0;}\n");

    std::filesystem::path const uncDir =
        dss::test_support::uncSpellingOf(scratch.path());
    if (uncDir.empty())
        GTEST_SKIP()
            << "D-PATH-MULTI-SEPARATOR-ROOT-COLLAPSED-BY-STDLIB-PATH-TRANSFORMS"
               ": this host offers no reachable multi-separator spelling of '"
            << scratch.path().string()
            << "', so the glob expander's rendering WAS NOT MEASURED on this "
               "leg. This is an UNMEASURED property, not a passing one.";
    ASSERT_GE(dss::test_support::leadingSeparatorRun(uncDir), 2u);

    std::vector<std::string> out;
    std::error_code          ec;
    ASSERT_TRUE(expandGlob(dss::core::genericSpelling(uncDir / "*.c"), out, ec));
    EXPECT_FALSE(ec) << ec.message();
    ASSERT_EQ(out.size(), 1u)
        << "the glob must still walk the tree it was pointed at";
    EXPECT_GE(dss::test_support::leadingSeparatorRun(
                  std::filesystem::path{out[0]}),
              2u)
        << "the matched SOURCE PATH lost its authority, so the compiler would "
           "go on to open it on this drive: " << out[0];
    std::error_code existsEc;
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path{out[0]}, existsEc))
        << "a match the compiler cannot open is not a match: " << out[0];

    // ⚠ THE SECOND RENDERING, WHICH THE ARM ABOVE CANNOT REACH. `expandGlob`
    // has a metacharacter-free path that renders `rebase()`'s result instead of
    // the walk's, and it is a SEPARATE `generic_string()` call. Reaching it
    // needs a RELATIVE pattern against a multi-separator BASE — the one shape
    // where re-basing happens AND the result carries an authority. Without this
    // the fix on that line would be untested and would read as covered.
    std::vector<std::string> lit;
    std::error_code          litEc;
    ASSERT_TRUE(expandGlob("a.c", lit, litEc, uncDir));
    EXPECT_FALSE(litEc) << litEc.message();
    ASSERT_EQ(lit.size(), 1u);
    EXPECT_GE(dss::test_support::leadingSeparatorRun(
                  std::filesystem::path{lit[0]}),
              2u)
        << "the re-based literal lost its authority: " << lit[0];
}

// ── [[D-PATH-MULTI-SEPARATOR-ROOT-COLLAPSED-BY-STDLIB-PATH-TRANSFORMS]] ─────
//
// A LITERAL SOURCE THAT NAMES ANOTHER MACHINE MUST NOT BE RE-BASED ONTO THE
// MANIFEST'S DIRECTORY. `expandAndDedupProjectSources` classified with a bare
// `is_absolute()`, and ✔MEASURED 2026-08-28 that predicate answers FALSE for a
// path whose root is a multi-separator authority:
//     '//localhost/C$/…'.is_absolute()          -> FALSE
//     'C:/some/consumer/project' / that         -> 'C://localhost/C$/…'
// So an already-exact source was treated as relative and had the consuming
// manifest's DRIVE glued in front of the authority — the silent relocation the
// re-basing rule exists to prevent, in the one case the rule cannot see.
//
// ⚠ TWO DEFECTS ON ONE LINE: the rendering that followed was `generic_string()`,
// which collapses the run again. A correctly-classified literal still came out
// naming the local drive, so neither repair alone is observable.
TEST(ProjectSources, LiteralWithAMultiSeparatorRootIsNotRebased) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::Temp, "program-sources-unc"};
    writeText(scratch.path() / "a.c", "int a(void){return 0;}\n");

    std::filesystem::path const uncDir =
        dss::test_support::uncSpellingOf(scratch.path());
    if (uncDir.empty())
        GTEST_SKIP()
            << "D-PATH-MULTI-SEPARATOR-ROOT-COLLAPSED-BY-STDLIB-PATH-TRANSFORMS"
               ": this host offers no reachable multi-separator spelling of '"
            << scratch.path().string()
            << "', so the literal-source classification WAS NOT MEASURED on "
               "this leg. This is an UNMEASURED property, not a passing one.";
    ASSERT_GE(dss::test_support::leadingSeparatorRun(uncDir), 2u);

    // A base directory that is DELIBERATELY somewhere else: if the literal is
    // misclassified as relative it acquires THIS path's root, which is exactly
    // what the assertion below refuses.
    std::filesystem::path const otherBase = scratch.path() / "unrelated_base";
    DiagnosticReporter          rep;
    auto const expanded = expandAndDedupProjectSources(
        {dss::core::genericSpelling(uncDir / "a.c")}, otherBase, rep);
    ASSERT_TRUE(expanded.has_value()) << "errors: " << rep.errorCount();
    ASSERT_EQ(expanded->size(), 1u);
    EXPECT_GE(dss::test_support::leadingSeparatorRun(
                  std::filesystem::path{(*expanded)[0]}),
              2u)
        << "the literal was re-based or re-spelled onto the local drive: "
        << (*expanded)[0];
    std::error_code existsEc;
    EXPECT_TRUE(
        std::filesystem::exists(std::filesystem::path{(*expanded)[0]}, existsEc))
        << "the expanded source must still name the file that was declared: "
        << (*expanded)[0];

    // ⚠ THE OTHER HALF OF THE SAME LINE, and the arm above CANNOT reach it: a
    // rooted literal is returned verbatim, so the re-basing RENDERING never
    // runs. A relative literal against a multi-separator BASE is the only shape
    // that exercises it — and it is exactly where `generic_string()` would
    // silently hand back a source on the local drive.
    DiagnosticReporter rebaseRep;
    auto const rebased =
        expandAndDedupProjectSources({"a.c"}, uncDir, rebaseRep);
    ASSERT_TRUE(rebased.has_value()) << "errors: " << rebaseRep.errorCount();
    ASSERT_EQ(rebased->size(), 1u);
    EXPECT_GE(dss::test_support::leadingSeparatorRun(
                  std::filesystem::path{(*rebased)[0]}),
              2u)
        << "the re-based literal lost its authority: " << (*rebased)[0];
}

// The CONTROL for both arms above, and it is chosen to be INERT: a RELATIVE
// literal has no root at all, so it must still be re-based against `baseDir`
// exactly as before. This is what proves the fix is a discrimination on the
// separator RUN and not a blanket "stop re-basing" regression — `isRootedPath`
// answers FALSE here and the re-basing arm must still run.
TEST(ProjectSources, RelativeLiteralIsStillRebasedAgainstTheBase) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::Temp, "program-sources-rebase"};
    DiagnosticReporter rep;
    auto const expanded =
        expandAndDedupProjectSources({"src/lib.c"}, scratch.path(), rep);
    ASSERT_TRUE(expanded.has_value());
    ASSERT_EQ(expanded->size(), 1u);
    EXPECT_EQ(std::filesystem::path{(*expanded)[0]},
              std::filesystem::path{
                  dss::core::genericSpelling(scratch.path() / "src" / "lib.c")})
        << "a relative literal stopped being resolved against the manifest's "
           "own directory: " << (*expanded)[0];
}

// ── 2g. expandGlob: `**` descends subdirectories ────────────────
TEST(ExpandGlob, RecursiveDoubleStarDescendsSubdirs) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::Temp, "program"};
    auto const dir = scratch.path();
    writeText(dir / "top.c", "int t(void){return 0;}\n");
    std::filesystem::create_directories(dir / "sub" / "deep");
    writeText(dir / "sub" / "mid.c", "int m(void){return 0;}\n");
    writeText(dir / "sub" / "deep" / "low.c", "int l(void){return 0;}\n");

    std::vector<std::string> out;
    std::error_code ec;
    ASSERT_TRUE(expandGlob((dir / "**" / "*.c").generic_string(), out, ec));
    EXPECT_FALSE(ec);
    EXPECT_EQ(out.size(), 3u)
        << "** must match top.c, sub/mid.c, AND sub/deep/low.c (recursive)";
}

// ── 2h. expandGlob: a single `*` does NOT descend ───────────────
TEST(ExpandGlob, SingleStarDoesNotDescend) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::Temp, "program"};
    auto const dir = scratch.path();
    writeText(dir / "top.c", "int t(void){return 0;}\n");
    std::filesystem::create_directories(dir / "sub");
    writeText(dir / "sub" / "nested.c", "int n(void){return 0;}\n");

    std::vector<std::string> out;
    std::error_code ec;
    ASSERT_TRUE(expandGlob((dir / "*.c").generic_string(), out, ec));
    EXPECT_FALSE(ec);
    ASSERT_EQ(out.size(), 1u)
        << "'*.c' matches only the top level (sub/nested.c is one level too deep)";
    EXPECT_EQ(std::filesystem::path{out[0]}.filename().string(), "top.c");
}

// ── 2i. expandGlob: zero match is NOT an error at the helper level ─
// (The DRIVER turns an empty expansion into a fail-loud diagnostic — the helper
// itself returns true + appends nothing, leaving the policy to the caller.)
TEST(ExpandGlob, ZeroMatchAppendsNothingNotAnError) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::Temp, "program"};
    auto const dir = scratch.path();
    writeText(dir / "a.c", "int a(void){return 0;}\n");
    std::vector<std::string> out;
    std::error_code ec;
    ASSERT_TRUE(expandGlob((dir / "*.cpp").generic_string(), out, ec));
    EXPECT_FALSE(ec);
    EXPECT_TRUE(out.empty()) << "no .cpp files ⇒ nothing appended, no I/O error";
}

// A base directory that does not exist is likewise ZERO matches, not an I/O
// error (distinct from an unreadable dir, which fails loud).
TEST(ExpandGlob, NonexistentBaseIsZeroMatchNotError) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::Temp, "program"};
    auto const dir = scratch.path();
    std::vector<std::string> out;
    std::error_code ec;
    ASSERT_TRUE(expandGlob((dir / "nope" / "*.c").generic_string(), out, ec));
    EXPECT_FALSE(ec);
    EXPECT_TRUE(out.empty());
}

// expandGlob APPENDS (never clears) — the InputResolver convention.
TEST(ExpandGlob, AppendsToExistingOutput) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::Temp, "program"};
    auto const dir = scratch.path();
    writeText(dir / "one.c", "int one(void){return 0;}\n");
    std::vector<std::string> out{"pre-existing.c"};
    std::error_code ec;
    ASSERT_TRUE(expandGlob((dir / "*.c").generic_string(), out, ec));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "pre-existing.c") << "the pre-existing entry is preserved";
    EXPECT_EQ(std::filesystem::path{out[1]}.filename().string(), "one.c");
}

// ── 3. Behavioral proofs through compileProject ─────────────────

// A zero-match source glob FAILS LOUD (D_FileNotFound naming the pattern) — a
// source pattern that names nothing is a mistake, not an empty no-op. Host-
// agnostic (rejected before any emit).
TEST(CompileProjectGlob, ZeroMatchPatternFailsLoud) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    std::string const target = "x86_64:elf64-x86_64-linux-exec";
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "real.c", "int main(void){return 0;}\n");  // exists, but no *.zzz
    std::string const manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"" + target + "\"],\n"
        + "  \"sources\": [\"" + (dir / "*.zzz").generic_string() + "\"]\n}";
    auto const proj = dir / "zero.dss-project.json";
    writeText(proj, manifest);
    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_EQ(prog.compileProject(proj.string(), rep), 1)
        << "a zero-match source glob must fail the build";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_FileNotFound), 1u)
        << "the zero-match fail-loud uses D_FileNotFound, naming the pattern";
}

// A LITERAL entry (no glob metacharacter) is kept VERBATIM — unchanged behavior:
// an existing literal source still resolves + compiles, routing by literal count.
// Host-agnostic (fixed cross-target ELF, never run).
TEST(CompileProjectGlob, LiteralEntryUnchanged) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    std::string const target = "x86_64:elf64-x86_64-linux-exec";
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    auto const src = dir / "solo.c";
    writeText(src, "int main(void){return 0;}\n");
    std::string const manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"" + target + "\"],\n"
        + "  \"sources\": [\"" + src.generic_string() + "\"]\n}";  // literal, no metachar
    auto const proj = dir / "lit.dss-project.json";
    writeText(proj, manifest);
    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << "a metacharacter-free source is kept verbatim + still compiles";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_FileNotFound), 0u);
    // 1 literal → single-CU route → the per-format-subdir artifact <formatName>/solo.
    EXPECT_TRUE(std::filesystem::exists(
        dir / "out" / "elf64-x86_64-linux-exec" / "solo"));
}

// A `**` recursive glob spanning a subdirectory expands + compiles: main.c (top)
// calls other() defined in sub/other.c; the glob matches BOTH → 2 CUs → the
// linker merges them → a real ELF executable on disk. Host-agnostic (asserts the
// artifact bytes exist; DSS cross-emits, so never run here).
// RED-ON-DISABLE: without expansion, "<dir>/**/*.c" is ONE bogus literal "file"
// → count 1 → compileFiles opens it → file-not-found → rc ≠ 0.
TEST(CompileProjectGlob, RecursiveGlobExpandsAndCompiles) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    std::string const target = "x86_64:elf64-x86_64-linux-exec";
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "main.c",
              "int other(void);\nint main(void){ return other(); }\n");
    std::filesystem::create_directories(dir / "sub");
    writeText(dir / "sub" / "other.c", "int other(void){ return 42; }\n");
    std::string const manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"" + target + "\"],\n"
        + "  \"sources\": [\"" + (dir / "**" / "*.c").generic_string() + "\"]\n}";
    auto const proj = dir / "rec.dss-project.json";
    writeText(proj, manifest);
    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << "** must expand to main.c + sub/other.c (2 CUs) and link";
    // main.c sorts before sub/other.c → source stem "main".
    EXPECT_TRUE(std::filesystem::exists(
        dir / "out" / "elf64-x86_64-linux-exec" / "main"))
        << "the merged 2-CU image must emit";
}

// A MIX of one LITERAL and one GLOB entry: the literal is kept + the glob is
// expanded, and the combined count routes correctly (2 → multi-CU). Host-
// agnostic. RED-ON-DISABLE: without expansion the glob is a bogus "file" → the
// build opens it → rc ≠ 0.
TEST(CompileProjectGlob, MixLiteralAndGlobCombine) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    std::string const target = "x86_64:elf64-x86_64-linux-exec";
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    // literal main.c at the top; the glob covers a SEPARATE subdir (so the two
    // entries never overlap into a duplicate source).
    writeText(dir / "main.c",
              "int helper(void);\nint main(void){ return helper(); }\n");
    std::filesystem::create_directories(dir / "lib");
    writeText(dir / "lib" / "helper.c", "int helper(void){ return 42; }\n");
    std::string const manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"" + target + "\"],\n"
        + "  \"sources\": [\"" + (dir / "main.c").generic_string() + "\", \""
        + (dir / "lib" / "*.c").generic_string() + "\"]\n}";
    auto const proj = dir / "mix.dss-project.json";
    writeText(proj, manifest);
    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << "the literal main.c + the expanded lib/*.c must combine into 2 CUs";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_FileNotFound), 0u);
    // the literal entry keeps its position (index 0) → source stem "main".
    EXPECT_TRUE(std::filesystem::exists(
        dir / "out" / "elf64-x86_64-linux-exec" / "main"));
}

// ── Host-gated build+RUN: the 2-match glob links BOTH translation units ──
// The strongest routing proof — a small 2-file program where main (TU1) calls a
// function defined in the second file (TU2). Built host-native + RUN: exit 42
// proves the glob expanded to 2 concrete sources that were BOTH compiled + linked
// (the multi-CU route). Mirrors the Cycle-A/B defines build+run host gate.
// RED-ON-DISABLE: without glob expansion, "<dir>/*.c" is ONE bogus literal "file"
// → count 1 → compileFiles opens "<dir>/*.c" literally → file-not-found → rc ≠ 0
// (so this ASSERT_EQ(...,0) itself goes red).
#if defined(_WIN32) || (defined(__linux__) && (defined(__x86_64__) || defined(__amd64__)))
namespace {
#if defined(_WIN32)
constexpr std::string_view kGlobHostSpec   = "x86_64:pe64-x86_64-windows-exec";
constexpr std::string_view kGlobHostFormat = "pe64-x86_64-windows-exec";
constexpr std::string_view kGlobHostExt    = ".exe";
#else
constexpr std::string_view kGlobHostSpec   = "x86_64:elf64-x86_64-linux-exec";
constexpr std::string_view kGlobHostFormat = "elf64-x86_64-linux-exec";
constexpr std::string_view kGlobHostExt    = "";
#endif
}  // namespace

TEST(CompileProjectGlob, MultiCuGlobBuildsAndRunsBothTus) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    using dss::test_support::runBinary;

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    // main.c sorts first + calls other(), which is defined in the SECOND file.
    writeText(dir / "main.c",
              "int other(void);\nint main(void){ return other(); }\n");
    writeText(dir / "other.c", "int other(void){ return 42; }\n");
    writeText(dir / "README.txt", "not a source\n");  // the *.c glob must exclude it

    std::string const manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"" + std::string{kGlobHostSpec} + "\"],\n"
        + "  \"sources\": [\"" + (dir / "*.c").generic_string() + "\"]\n}";
    auto const proj = dir / "multi.dss-project.json";
    writeText(proj, manifest);

    Program prog;
    prog.setOutputDir(dir);
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << "the *.c glob must expand to main.c + other.c (2 CUs) and link";
    auto const exe = dir / std::string{kGlobHostFormat}
                   / (std::string{"main"} + std::string{kGlobHostExt});
    ASSERT_TRUE(std::filesystem::exists(exe)) << exe.string();
    auto const r = runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "both TUs linked: main (TU1) calls other (TU2) → 42 (red-on-disable "
           "if the glob were treated as one bogus literal file)";
}

// Cross-entry DEDUP: a literal `<dir>/./main.c` overlaps a glob `<dir>/*.c` that
// ALSO matches main.c — the SHARED file must compile ONCE. The literal keeps its
// verbatim `./` spelling while the glob emits the NORMALIZED path, so the two
// point at the same file via DIFFERENT strings — a plain string-dedup would miss
// it; the normalized-key dedup catches it. The union (main.c once + other.c once)
// links + runs → 42.
//
// RED-ON-DISABLE — TWO independent levers, both caught by this ASSERT_EQ(...,0):
//   * remove the cross-entry dedup block  → main.c is TWO CUs (literal + glob),
//   * weaken it to a plain STRING dedup    → `./main.c` != `main.c` still TWO CUs,
// either way ⇒ duplicate `main` symbol ⇒ a LINK error ⇒ rc ≠ 0.
TEST(CompileProjectGlob, OverlappingLiteralAndGlobDedupByNormalizedPath) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    using dss::test_support::runBinary;

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "main.c",
              "int other(void);\nint main(void){ return other(); }\n");
    writeText(dir / "other.c", "int other(void){ return 42; }\n");

    // Entry 0: a literal with a `./` component (kept verbatim, un-normalized).
    // Entry 1: a glob matching BOTH main.c and other.c (emits normalized paths).
    std::string const literalDotMain = dir.generic_string() + "/./main.c";
    std::string const manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"" + std::string{kGlobHostSpec} + "\"],\n"
        + "  \"sources\": [\"" + literalDotMain + "\", \""
        + (dir / "*.c").generic_string() + "\"]\n}";
    auto const proj = dir / "dedup.dss-project.json";
    writeText(proj, manifest);

    Program prog;
    prog.setOutputDir(dir);
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << "the shared main.c (literal ./main.c + glob *.c) must dedup to ONE CU "
           "by normalized path — no duplicate symbols";
    auto const exe = dir / std::string{kGlobHostFormat}
                   / (std::string{"main"} + std::string{kGlobHostExt});
    ASSERT_TRUE(std::filesystem::exists(exe)) << exe.string();
    auto const r = runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "the deduped union (main.c once + other.c once) links + runs → 42";
}
#endif  // host-native exec (Windows or Linux-x86_64)

// ════════════════════════════════════════════════════════════════════════════
// BUILD-LIFECYCLE HOOKS + `dependsOn`, at the `compileProject` SEAM TIER
// ════════════════════════════════════════════════════════════════════════════
//
// `tests/program/test_build_scripts.cpp` already pins the RUNNER — no shell,
// manifest order, stop-at-first-failure, the `runOn` host filter, and the two
// remediation-distinct failure codes. NONE of that is re-tested here. What this
// section pins is the thing the runner cannot know about itself: WHERE IN
// `Program::compileProject` IT IS CALLED, and what the driver does with the
// answer. Every defect in that wiring is invisible to a runner-tier test and
// most of them are invisible to a diagnostic-counting test too:
//
//   * PRE-BUILD HOOKS CALLED BELOW THE GLOB EXPANSION. Every existing pin stays
//     green — the hook still runs, the codes are still right — while the single
//     most common use of the feature (generate the sources, then compile them)
//     becomes impossible, because expansion is fail-loud on zero matches and
//     would kill the build one statement before the generator ran. Only a test
//     that GENERATES a source and then RUNS the resulting program can tell the
//     two orderings apart.
//   * POST-BUILD HOOKS RUN AFTER A FAILED COMPILE. The build already returns
//     non-zero, so an rc assertion cannot see it. Only the ABSENCE of the
//     hook's marker file distinguishes "skipped" from "ran and was ignored" —
//     and the difference is a deploy step firing on a build that failed.
//   * A POST-BUILD FAILURE SWALLOWED INTO exit 0. The artifact exists, the
//     compile really did succeed, and every artifact-existence assertion passes;
//     only the rc says the packaging step failed. CI reads the rc.
//   * `dependsOn` ACCEPTED AND IGNORED. The greenest possible failure: the
//     manifest declares a prerequisite, the loader validates it, the driver
//     never reads it, and the build reports success for an artifact missing the
//     thing its author said it needs.
//
// ── THE FIXTURE MECHANISM: SELF-SPAWN ───────────────────────────────────────
//
// These pins need a hook program that (a) exits with a chosen status and
// (b) optionally CREATES A FILE AT A PATH RELATIVE TO ITS OWN WORKING
// DIRECTORY. The repo's established answer to "a test needs a fixture program"
// is to re-exec the TEST BINARY ITSELF with a private flag its `main`
// intercepts ahead of gtest — `tests/test_support/test_run_binary_capture.cpp`
// and `tests/program/test_build_scripts.cpp` both do it, and the reasoning is
// recorded at length in the latter: no external dependency (Windows has no
// `/bin/echo`), no new build target (hence no `$<TARGET_FILE:…>` define, no
// `add_dependencies` edge, and no row in the two `scripts/check-orphan-tests/check-orphan-tests`
// twins), and an EXACT path via `argv[0]`.
//
// The RELATIVE write path is not a convenience — it is half the subject. A
// pre-build hook that writes `generated/main.c` and a manifest that reads
// `generated/*.c` agree only if the hook's working directory is the same base
// the glob resolves against. Handing the fixture an absolute path would make
// the test pass under a driver that spawned hooks in any directory at all.
//
// The protocol is POSITIONAL: `argv[1]` is the exit-code flag (and is what
// `main` recognises to enter fixture mode), `argv[2]` the optional write-path
// flag, `argv[3]` the file's contents. A scan would let a payload that happens
// to start with a flag prefix be swallowed.

namespace {

constexpr std::string_view kHookExitFlag  = "--dss-project-hook-exit=";
constexpr std::string_view kHookWriteFlag = "--dss-project-hook-write=";

// Fixture-side protocol violations return codes in the 90s so a broken FIXTURE
// can never be mistaken for the SUBJECT misbehaving: each surfaces as a
// `D_ScriptExitedNonZero` naming the status, and no pin below expects one.
constexpr int kHookMalformedExit  = 90;
constexpr int kHookMalformedWrite = 91;
constexpr int kHookMissingContent = 92;
constexpr int kHookMkdirFailed    = 93;
constexpr int kHookWriteFailed    = 94;

// FIXTURE MODE. Optionally writes `argv[3]` to the path in `argv[2]` —
// RESOLVED AGAINST THIS PROCESS'S OWN WORKING DIRECTORY, which is the property
// under test — then exits with the status in `argv[1]`.
int runHookFixture(int argc, char** argv) {
    std::string const exitText{
        std::string_view{argv[1]}.substr(kHookExitFlag.size())};
    if (exitText.empty()) return kHookMalformedExit;
    for (char const c : exitText) {
        if (c < '0' || c > '9') return kHookMalformedExit;
    }
    int const requestedExit = std::atoi(exitText.c_str());

    if (argc >= 3) {
        std::string_view const writeArg{argv[2]};
        if (writeArg.substr(0, kHookWriteFlag.size()) != kHookWriteFlag) {
            return kHookMalformedWrite;
        }
        std::filesystem::path const target{
            std::string{writeArg.substr(kHookWriteFlag.size())}};
        if (target.empty()) return kHookMalformedWrite;
        if (argc < 4) return kHookMissingContent;

        // A real generator creates its own output directory; so does this one,
        // which is why `generated/` need not pre-exist in the manifest's tree.
        if (!target.parent_path().empty()) {
            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);
            if (ec) return kHookMkdirFailed;
        }
        std::ofstream out{target, std::ios::binary | std::ios::trunc};
        if (!out) return kHookWriteFailed;
        out << argv[3];
        out.flush();
        if (!out) return kHookWriteFailed;
    }
    return requestedExit;
}

// This test executable as an ABSOLUTE path, in GENERIC (forward-slash) form.
// Generic matters twice over: the string is interpolated into JSON, where a
// Windows `\` would be read as an escape introducer, and `CreateProcess`
// accepts forward slashes in a path just as happily as backslashes.
std::string selfExecutableForJson() {
    auto const& argvs = ::testing::internal::GetArgvs();
    if (argvs.empty()) return {};
    std::error_code ec;
    std::filesystem::path const abs =
        std::filesystem::absolute(std::filesystem::path{argvs[0]}, ec);
    return (ec ? std::filesystem::path{argvs[0]} : abs).generic_string();
}

// JSON string quoting. Three classes of character have to be handled and ALL
// THREE occur in the payloads below, which is why this is not the two-line
// version it looks like it should be:
//   * `\` — a Windows path interpolated into the manifest (the self-executable
//     is emitted in generic form, but a temp root can still surprise us);
//   * `"` — defensive; no current payload carries one;
//   * CONTROL CHARACTERS, and specifically `\n`. The generated C source ends in
//     a newline, and a RAW newline inside a JSON string is ILL-FORMED (RFC 8259
//     §7 excludes U+0000–U+001F). Omitting this arm does not produce a subtly
//     wrong manifest — it produces one the loader rejects outright, so every
//     hook pin would red with `C_MalformedJson` instead of the behaviour under
//     test, and the message would point at the test's own scaffolding.
//     ✔MEASURED: caught exactly that way, by round-tripping these manifests
//     through a strict JSON parser before trusting them.
std::string jsonQuote(std::string_view s) {
    std::string out = "\"";
    for (char const c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    // Any other control byte, spelled the one way JSON allows.
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(
                                      static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    out += '"';
    return out;
}

// One `preBuildScripts` / `postBuildScripts` entry spawning this binary in
// fixture mode. `writeTarget` empty ⇒ the hook only exits.
std::string hookEntryJson(int exitCode,
                          std::string_view writeTarget = {},
                          std::string_view contents    = {}) {
    std::string json = "{\"run\": [" + jsonQuote(selfExecutableForJson())
                     + ", " + jsonQuote(std::string{kHookExitFlag}
                                        + std::to_string(exitCode));
    if (!writeTarget.empty()) {
        json += ", " + jsonQuote(std::string{kHookWriteFlag}
                                 + std::string{writeTarget});
        json += ", " + jsonQuote(contents);
    }
    json += "]}";
    return json;
}

// A manifest with the OPTIONAL keys emitted only when non-empty — an omitted
// key and a present `[]` are two distinct inputs, and both are pinned below.
std::string hookManifest(std::string_view targetSpec,
                         std::string_view sourcesArrayJson,
                         std::string_view preArrayJson       = {},
                         std::string_view postArrayJson      = {},
                         std::string_view dependsOnArrayJson = {}) {
    std::string m = "{\n  \"language\": \"c\",\n";
    m += "  \"artifactProfile\": \"cli\",\n";
    m += "  \"targets\": [" + jsonQuote(targetSpec) + "],\n";
    m += "  \"sources\": " + std::string{sourcesArrayJson};
    if (!preArrayJson.empty()) {
        m += ",\n  \"preBuildScripts\": " + std::string{preArrayJson};
    }
    if (!postArrayJson.empty()) {
        m += ",\n  \"postBuildScripts\": " + std::string{postArrayJson};
    }
    if (!dependsOnArrayJson.empty()) {
        m += ",\n  \"dependsOn\": " + std::string{dependsOnArrayJson};
    }
    m += "\n}";
    return m;
}

// The format half of a `<target>:<format>` spec — the `<outputDir>/<format>/`
// subdir a PROJECT build always routes its artifact through.
std::string formatOf(std::string_view targetSpec) {
    auto const colon = targetSpec.find(':');
    return std::string{targetSpec.substr(colon + 1)};
}

// A fixed cross-target used by every pin that does NOT spawn its output. DSS
// cross-emits, so these stay live on every leg of the matrix.
constexpr std::string_view kHookElfSpec = "x86_64:elf64-x86_64-linux-exec";

}  // namespace

// ── 0. The fixture itself, so a fixture failure names itself ────────────────
// Without this, a lost `argv[0]` turns every pin below into a
// `D_ScriptSpawnFailed`, which reads as "the driver cannot spawn hooks" when
// the truth is "the test could not find its own fixture".
TEST(CompileProjectHooks, TheHookFixtureIsThisExecutable) {
    auto const self = selfExecutableForJson();
    ASSERT_FALSE(self.empty()) << "could not recover argv[0] from gtest";
    ASSERT_TRUE(std::filesystem::path{self}.is_absolute())
        << "argv[0] was not made absolute: " << self;
    ASSERT_TRUE(std::filesystem::exists(self))
        << "argv[0] does not resolve to a file: " << self;
}

// ── 1. ★ THE LOAD-BEARING PIN: a pre-build hook GENERATES the source ────────
//
// The manifest names NO source that exists when the build starts: `sources` is
// `["generated/*.c"]` and `generated/` is not even a directory yet. The
// pre-build hook creates `generated/main.c` — at a path relative to ITS OWN
// working directory — and only then does expansion have anything to match.
// The produced binary is SPAWNED and its exit code compared against the one
// the generated source returns, so nothing short of "the generated bytes were
// compiled and executed" can satisfy this.
//
// Built for the HOST's native target (`host_native_target.hpp`) so the pin
// stays LIVE on every leg rather than being `#if`-ed out on three of them —
// a test that spawns its own output must build for the machine it runs on.
//
// RED-ON-DISABLE, and this is THE seam-ORDER lever: move the
// `runBuildScripts(pc.preBuildScripts, …)` call in `Program::compileProject`
// to BELOW the glob-expansion block (or delete it) and this test reds at the
// `ASSERT_EQ(rc, 0)` with `D_FileNotFound` — "glob pattern 'generated/*.c'
// matched no files" — because the generator never ran. No other pin in this
// file or in `test_build_scripts.cpp` notices that move.
TEST(CompileProjectHooks, PreBuildScriptGeneratesSourceThatCompilesAndRuns) {
    using dss::test_support::hostNativeTarget;
    using dss::test_support::Location;
    using dss::test_support::runBinary;
    using dss::test_support::ScratchDir;

    // A DISTINCTIVE status — not 0 (which a do-nothing binary also returns) and
    // not 42 (the value every other pin in this file uses, so a copy-paste
    // could not have produced it by accident).
    constexpr std::uint32_t kGeneratedExit = 57;
    // Derived from the constant, never re-typed: a hand-written `57` in the
    // source could drift from the asserted value and turn the strongest pin in
    // this file into one that compares two unrelated numbers.
    std::string const generatedSource =
        "int main(void){ return " + std::to_string(kGeneratedExit) + "; }\n";

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    std::string const spec{hostNativeTarget().execTarget};

    std::string const manifest = hookManifest(
        spec,
        R"(["generated/*.c"])",
        "[" + hookEntryJson(0, "generated/main.c", generatedSource) + "]");
    auto const proj = dir / "gen.dss-project.json";
    writeText(proj, manifest);

    // The hook's `generated/main.c` and the manifest's `generated/*.c` are BOTH
    // relative — they name the same file only if the hook is spawned in the
    // process working directory, which is what `useAsCwd` makes the scratch dir.
    scratch.useAsCwd();
    ASSERT_FALSE(std::filesystem::exists(dir / "generated"))
        << "the generated tree must NOT exist before the build — otherwise the "
           "hook has nothing to prove";

    Program prog;
    prog.setOutputDir(dir);
    DiagnosticReporter rep;
    int const rc = prog.compileProject(proj.string(), rep);
    ASSERT_EQ(rc, 0)
        << "the pre-build hook must run BEFORE the sources[] glob is expanded, "
           "so the source it generates is there to be matched";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_FileNotFound), 0u)
        << "a zero-match glob here means the hook ran too late (or not at all)";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptSpawnFailed), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptExitedNonZero), 0u);

    ASSERT_TRUE(std::filesystem::exists(dir / "generated" / "main.c"))
        << "the hook did not write its source where the manifest looks for it";

    auto const exe = dir / formatOf(spec)
                   / dss::test_support::hostExeArtifact("main");
    ASSERT_TRUE(std::filesystem::exists(exe)) << exe.string();
    auto const r = runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut) << r.diagnostic;
    EXPECT_EQ(r.exitCode, kGeneratedExit)
        << "the GENERATED source returns 57 — this exit code is the proof that "
           "the bytes the hook wrote are the bytes that were compiled and run";
}

// ── 2. Ordering proof, NEGATIVE direction (host-agnostic) ───────────────────
//
// The mirror of pin 1, and it needs no compiler backend at all, so it holds the
// seam-order line on every leg including the ones pin 1's spawn depends on.
// The manifest is doubly broken: the pre-build hook FAILS, and `sources` names
// a glob that matches nothing. Exactly one of those two can be reported first,
// and WHICH one identifies the seam order unambiguously.
//
// THREE-SIDED on purpose: the script code exactly once, `D_FileNotFound`
// exactly zero times, and a non-zero rc. Counting only the script code would
// pass under a driver that reported BOTH.
//
// RED-ON-DISABLE: move the pre-build call below the expansion → the counts
// swap (D_FileNotFound 1, D_ScriptExitedNonZero 0) and both EXPECTs red.
TEST(CompileProjectHooks, FailingPreBuildScriptReportsBeforeGlobExpansion) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    std::string const manifest = hookManifest(
        kHookElfSpec,
        R"(["never-generated/*.c"])",          // a glob that matches nothing
        "[" + hookEntryJson(3) + "]");         // a hook that fails
    auto const proj = dir / "order.dss-project.json";
    writeText(proj, manifest);
    scratch.useAsCwd();

    Program prog;
    prog.setOutputDir(dir);
    DiagnosticReporter rep;
    EXPECT_EQ(prog.compileProject(proj.string(), rep), 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptExitedNonZero), 1u)
        << "the pre-build hook runs first, so ITS failure is the one reported";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_FileNotFound), 0u)
        << "the zero-match glob must never be reached — reporting it would mean "
           "the expansion ran ahead of the hook";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptSpawnFailed), 0u)
        << "the fixture IS spawnable; a spawn failure here is a broken fixture";
}

// ── 3. A post-build hook RUNS after a successful compile ────────────────────
//
// The positive half, and pin 4 is VACUOUS without it: "the hook did not run
// when the compile failed" is trivially satisfied by a driver that never runs
// post-build hooks at all. Host-agnostic (a cross-emitted ELF, never spawned).
TEST(CompileProjectHooks, PostBuildScriptRunsAfterSuccessfulCompile) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "main.c", "int main(void){ return 0; }\n");
    std::string const manifest = hookManifest(
        kHookElfSpec, R"(["main.c"])", /*pre=*/{},
        "[" + hookEntryJson(0, "post-ran.txt", "ran\n") + "]");
    auto const proj = dir / "post-ok.dss-project.json";
    writeText(proj, manifest);
    scratch.useAsCwd();

    Program prog;
    prog.setOutputDir(dir);
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0);
    EXPECT_TRUE(std::filesystem::exists(dir / formatOf(kHookElfSpec) / "main"));
    EXPECT_TRUE(std::filesystem::exists(dir / "post-ran.txt"))
        << "a successful compile must be followed by the post-build hooks";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptExitedNonZero), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptSpawnFailed), 0u);
}

// ── 4. A FAILING post-build hook fails the build — and KEEPS the artifact ───
//
// BOTH assertions carry weight and neither can be dropped:
//   * `rc == 1` is the one that CI reads. A driver that reported the diagnostic
//     and returned the compile's 0 would look identical in every other respect,
//     and the next pipeline stage would ship an unpackaged build.
//   * `exists(artifact)` is what proves the compile genuinely SUCCEEDED before
//     the hook ran. Without it this test would also pass if the post-build hook
//     had somehow run after a failed compile — i.e. it would be measuring
//     nothing about the gate at all.
//
// RED-ON-DISABLE: `return rc;` unconditionally (drop the post-build result) →
// the rc assertion reds while the artifact assertion stays green.
TEST(CompileProjectHooks, PostBuildScriptFailureFailsBuildButKeepsArtifact) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "main.c", "int main(void){ return 0; }\n");
    std::string const manifest = hookManifest(
        kHookElfSpec, R"(["main.c"])", /*pre=*/{},
        "[" + hookEntryJson(5) + "]");     // ran, said no
    auto const proj = dir / "post-fail.dss-project.json";
    writeText(proj, manifest);
    scratch.useAsCwd();

    Program prog;
    prog.setOutputDir(dir);
    DiagnosticReporter rep;
    EXPECT_EQ(prog.compileProject(proj.string(), rep), 1)
        << "a failing post-build hook must fail the BUILD — exit 0 here would "
           "tell CI the project is ready to ship";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptExitedNonZero), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptSpawnFailed), 0u);
    EXPECT_TRUE(std::filesystem::exists(dir / formatOf(kHookElfSpec) / "main"))
        << "the artifact is KEPT: the compile really did succeed, and its bytes "
           "are still correct — its presence is what proves the hook ran AFTER "
           "a successful build rather than instead of one";
}

// ── 5. Post-build hooks do NOT run when the compile FAILED ──────────────────
//
// The marker file's ABSENCE is the whole test. The hook here exits 0, so it
// would add no diagnostic and change no rc if it ran — a build that packaged
// and deployed the previous run's binary would look, from every other angle,
// exactly like this one.
//
// RED-ON-DISABLE: drop the `rc == 0 &&` guard → the marker appears.
TEST(CompileProjectHooks, PostBuildScriptSkippedWhenCompileFails) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    // Real source, real compile, real failure: `nosuchtype` is undeclared.
    writeText(dir / "main.c", "int main(void){ return nosuchtype; }\n");
    std::string const manifest = hookManifest(
        kHookElfSpec, R"(["main.c"])", /*pre=*/{},
        "[" + hookEntryJson(0, "post-ran.txt", "ran\n") + "]");
    auto const proj = dir / "post-skip.dss-project.json";
    writeText(proj, manifest);
    scratch.useAsCwd();

    Program prog;
    prog.setOutputDir(dir);
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0)
        << "the source does not compile — the build must fail";
    EXPECT_FALSE(std::filesystem::exists(dir / "post-ran.txt"))
        << "a post-build hook must NOT run after a failed compile: packaging or "
           "deploying here would operate on a stale or absent artifact";
    EXPECT_FALSE(std::filesystem::exists(dir / formatOf(kHookElfSpec) / "main"))
        << "sanity: the failed compile emitted no artifact";
}

// ── 6. `dependsOn` IS RESOLVED, AND A FAILING RESOLUTION HAS NO SIDE EFFECTS ─
//
// ★ WHAT THIS PIN USED TO BE, AND WHY IT CHANGED RATHER THAN BEING DELETED.
// Until the AP6 resolver landed, a non-empty `dependsOn` was REFUSED with
// `D_PlanNotLanded`, emitted FIRST — ahead of the profile gates and the
// pre-build hooks — so a build that could not happen had no side effects on the
// way to saying so. The refusal is gone; the PROPERTY it was protecting is not,
// and it is what this pin now asserts. A dependency the resolver cannot resolve
// (here: a `path` naming nothing) must still fail the build, still emit
// nothing but a real resolution diagnostic, and still leave the AUTHOR'S TREE
// UNTOUCHED — the root's pre-build hook must not have run.
//
// The third assertion is the one that pins the SEAM ORDER: resolution sits
// above the root's hooks. Without it, a driver that resolved after running the
// author's codegen step would look identical from the rc and the code alone.
//
// ⚠ `D_PlanNotLanded == 0` IS NOT ASSERTED ALONE — that is worthless by itself
// (it is also true of a driver that ignored the key entirely). It is paired
// with the POSITIVE that a real resolution diagnostic fired, which is what
// distinguishes "resolved and failed" from "never looked".
//
// RED-ON-DISABLE: move the `resolveProjectDependencies` call below the
// `runBuildScripts(pc.preBuildScripts, …)` call in `Program::compileProject`
// and the marker assertion reds while the rc and code assertions stay green.
TEST(CompileProjectHooks, UnresolvableDependsOnFailsWithoutRunningRootHooks) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "main.c", "int main(void){ return 0; }\n");
    std::string const manifest = hookManifest(
        kHookElfSpec, R"(["main.c"])",
        "[" + hookEntryJson(0, "pre-ran.txt", "ran\n") + "]",
        /*post=*/{},
        R"([{"path": "../no-such-dependency-directory"}])");
    auto const proj = dir / "deps.dss-project.json";
    writeText(proj, manifest);
    scratch.useAsCwd();

    Program prog;
    prog.setOutputDir(dir);
    DiagnosticReporter rep;
    EXPECT_EQ(prog.compileProject(proj.string(), rep), 1)
        << "a declared dependency the driver cannot resolve must FAIL the "
           "build, not silently build without it";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_FileNotFound), 1u)
        << "the resolver reports the real reason — the path names no directory";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_PlanNotLanded), 0u)
        << "resolution has LANDED; the pending-plan refusal must be gone";
    EXPECT_FALSE(std::filesystem::exists(dir / formatOf(kHookElfSpec) / "main"))
        << "nothing may be produced for a build that could not resolve";
    EXPECT_FALSE(std::filesystem::exists(dir / "pre-ran.txt"))
        << "resolution precedes the ROOT's pre-build hooks — a build that "
           "cannot happen must not write into the author's tree on the way to "
           "saying so";
}

// A `dependsOn` entry that DOES resolve builds normally — the positive polarity
// of the pin above, and the one that proves the refusal was replaced by a
// working mechanism rather than by nothing at all. The dependency is a `module`
// (source-merge), so its success is observable as a link that resolved a symbol
// the root's own sources do not define.
TEST(CompileProjectHooks, ResolvableDependsOnBuildsAndContributesItsSources) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    auto const dep = dir / "helpermod";
    // This file's `writeText` does NOT create parent directories, and the
    // dependency lives in one of its own — without this the manifest would name
    // a directory that is not there and the pin would measure `D_FileNotFound`
    // instead of the resolution it exists to prove.
    std::error_code mkEc;
    std::filesystem::create_directories(dep, mkEc);
    ASSERT_FALSE(mkEc) << "could not create the dependency directory: "
                       << mkEc.message();
    writeText(dep / "helper.c", "int helper_answer(void){ return 11; }\n");
    writeText(dep / ".dss-project.json",
              R"({"language":"c","artifactProfile":"module",)"
              R"("targets":[")" + std::string{kHookElfSpec}
                  + R"("],"sources":["helper.c"]})");
    writeText(dir / "main.c",
              "extern int helper_answer(void);\n"
              "int main(void){ return helper_answer(); }\n");
    auto const proj = dir / "ok-deps.dss-project.json";
    writeText(proj, hookManifest(kHookElfSpec, R"(["main.c"])", {}, {},
                                 R"([{"path": "helpermod"}])"));
    scratch.useAsCwd();

    Program prog;
    prog.setOutputDir(dir);
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << "a resolvable `dependsOn` must BUILD — the source-merge dependency "
           "supplies the definition `main` calls";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_PlanNotLanded), 0u);
    EXPECT_TRUE(std::filesystem::exists(dir / formatOf(kHookElfSpec) / "main"))
        << "the artifact is named from the ROOT's own first source (M4b), and "
           "its existence is what proves the dependency's sources were merged "
           "rather than dropped";
}

// ── 7. An EMPTY or ABSENT `dependsOn` is exactly the old behavior ───────────
//
// The other polarity, and it is not decoration: a reject implemented as "the
// key is present" rather than "the list is non-empty" would break every
// manifest carrying a documented `"dependsOn": []`, and a reject that fired
// unconditionally would break every project in the corpus. Both spellings must
// produce a REAL, successful build — not merely "no D_PlanNotLanded".
TEST(CompileProjectHooks, EmptyAndAbsentDependsOnBuildNormally) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    // (a) present but EMPTY.
    {
        ScratchDir scratch{Location::InsideRepo, "program"};
        auto const dir = scratch.path();
        writeText(dir / "main.c", "int main(void){ return 0; }\n");
        writeText(dir / "empty-deps.dss-project.json",
                  hookManifest(kHookElfSpec, R"(["main.c"])", {}, {}, "[]"));
        scratch.useAsCwd();
        Program prog;
        prog.setOutputDir(dir);
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileProject(
                      (dir / "empty-deps.dss-project.json").string(), rep), 0)
            << R"("dependsOn": [] declares no prerequisite — it must build)";
        EXPECT_EQ(countCode(rep, DiagnosticCode::D_PlanNotLanded), 0u);
        EXPECT_TRUE(
            std::filesystem::exists(dir / formatOf(kHookElfSpec) / "main"));
    }
    // (b) ABSENT — the overwhelmingly common manifest.
    {
        ScratchDir scratch{Location::InsideRepo, "program"};
        auto const dir = scratch.path();
        writeText(dir / "main.c", "int main(void){ return 0; }\n");
        writeText(dir / "no-deps.dss-project.json",
                  hookManifest(kHookElfSpec, R"(["main.c"])"));
        scratch.useAsCwd();
        Program prog;
        prog.setOutputDir(dir);
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileProject(
                      (dir / "no-deps.dss-project.json").string(), rep), 0);
        EXPECT_EQ(countCode(rep, DiagnosticCode::D_PlanNotLanded), 0u);
        EXPECT_TRUE(
            std::filesystem::exists(dir / formatOf(kHookElfSpec) / "main"));
    }
}

// ── 8. NO hooks declared ⇒ byte-for-byte the pre-hook behavior ──────────────
//
// The regression floor for the whole feature: a manifest with none of the three
// keys must reach exactly the same outcome it did before the seams existed —
// a successful build, an artifact on disk, and NOT ONE diagnostic from any of
// the new codes. `runBuildScripts` returns true vacuously on an empty list, so
// the only way this reds is a seam that fabricated work out of an empty vector.
TEST(CompileProjectHooks, NoHooksDeclaredLeavesTheBuildUnchanged) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "main.c", "int main(void){ return 0; }\n");
    writeText(dir / "plain.dss-project.json",
              hookManifest(kHookElfSpec, R"(["main.c"])"));
    scratch.useAsCwd();

    Program prog;
    prog.setOutputDir(dir);
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(
                  (dir / "plain.dss-project.json").string(), rep), 0);
    EXPECT_TRUE(std::filesystem::exists(dir / formatOf(kHookElfSpec) / "main"));
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptSpawnFailed), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptExitedNonZero), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_PlanNotLanded), 0u);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "a hook-less project build must emit nothing at all";
}

// ════════════════════════════════════════════════════════════════
// AP6 M4 — `expandAndDedupProjectSources`: the base directory, the LITERAL
// half, and the dedup key
//
// The `sources[]` resolution moved out of `Program::compileProject` into
// `program/project_sources.hpp` so a DEPENDENCY manifest — which declares its
// sources relative to ITS OWN directory, not to whatever cwd the consumer's
// compiler is running in — can be resolved by the same code and the same
// policy. The pins below are the three things that extraction had to get right.
// ════════════════════════════════════════════════════════════════

namespace {

// A source list resolved with NO base — the pre-AP6 contract, which every
// existing caller (including the root manifest) still uses.
std::vector<std::string> resolveNoBase(std::vector<std::string> const& entries,
                                       DiagnosticReporter& rep) {
    auto r = expandAndDedupProjectSources(entries, std::filesystem::path{}, rep);
    return r.has_value() ? *r : std::vector<std::string>{};
}

// RAII cwd pin — a copy of the one in `test_system_dirs_cwd_independent.cpp`,
// for the tests below whose whole point is that the answer must NOT depend on
// the process working directory. `ScratchDir::useAsCwd` cannot serve them: it
// moves cwd to the SAME directory the sources live in, which is exactly the
// coincidence that would make a cwd-based resolution look correct.
class ScopedCwd {
public:
    explicit ScopedCwd(std::filesystem::path const& to)
        : prev_(std::filesystem::current_path()) {
        std::filesystem::current_path(to);
    }
    ~ScopedCwd() {
        std::error_code ec;
        std::filesystem::current_path(prev_, ec);  // a dtor must not throw
    }
    ScopedCwd(ScopedCwd const&)            = delete;
    ScopedCwd& operator=(ScopedCwd const&) = delete;
private:
    std::filesystem::path prev_;
};

}  // namespace

// ── An EMPTY base is byte-for-byte the pre-AP6 behaviour ────────────────
// The literal is kept with its EXACT characters — `./` and all — because the
// dedup contract downstream ("first occurrence keeps its original string")
// and every existing test's expectations rest on it.
TEST(ProjectSources, EmptyBaseKeepsLiteralEntriesCharacterForCharacter) {
    DiagnosticReporter rep;
    auto const out = resolveNoBase({"./main.c", "src/sub/other.c"}, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "./main.c")
        << "an empty base must not normalize, absolutize or otherwise touch a "
           "literal entry — pre-AP6 callers are byte-identical";
    EXPECT_EQ(out[1], "src/sub/other.c");
}

// ── ★ THE DEFECT: a LITERAL entry re-bases too ──────────────────────────
// ✔MEASURED before the fix: an entry with no glob metacharacter was pushed
// VERBATIM and later opened by `UnitBuilder::addFile` — i.e. against the
// PROCESS cwd. Re-basing only the glob expansion would leave `"src/lib.c"` (the
// overwhelmingly common form) resolving inside the CONSUMER's tree.
//
// The test is built so that cwd-resolution cannot pass by accident: BOTH
// directories contain a `src/lib.c`, cwd is the DECOY, and the base is the
// other one. A resolution that consults cwd returns the decoy and this test
// fails on the path it returns — it does not merely "find nothing".
//
// RED-ON-DISABLE (`src/program/project_sources.cpp`, the literal arm): make
// the literal arm push `entry` unconditionally, and the returned path is the
// DECOY's — `EXPECT_EQ(out[0], …)` reports the wrong directory, and the
// `decoy` guard below reports it a second time by name.
TEST(ProjectSources, BaseDirRebasesLiteralEntriesNotJustGlobs) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir consumerScratch{Location::InsideRepo, "program"};
    ScratchDir depScratch{Location::InsideRepo, "program"};
    auto const decoy = consumerScratch.path();
    auto const base  = depScratch.path();
    std::filesystem::create_directories(decoy / "src");
    std::filesystem::create_directories(base / "src");
    writeText(decoy / "src" / "lib.c", "int consumer_copy(void){return 1;}\n");
    writeText(base  / "src" / "lib.c", "int dependency_copy(void){return 2;}\n");

    ScopedCwd const cwd{decoy};   // the wrong answer is reachable from here
    DiagnosticReporter rep;
    auto const r = expandAndDedupProjectSources({"src/lib.c"}, base, rep);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ(std::filesystem::path{(*r)[0]}.lexically_normal(),
              (base / "src" / "lib.c").lexically_normal())
        << "a LITERAL relative entry must resolve against the manifest's base "
           "directory, not the process working directory";
    EXPECT_NE(std::filesystem::path{(*r)[0]}.lexically_normal(),
              (decoy / "src" / "lib.c").lexically_normal())
        << "the cwd copy of the same relative path must never be chosen";
}

// ── The GLOB half re-bases, and only the RELATIVE half of it ────────────
// An ABSOLUTE pattern already states its base; re-rooting it would be a silent
// relocation. Both arms in one test so the two cannot drift apart.
TEST(ProjectSources, BaseDirRebasesRelativeGlobsAndLeavesAbsoluteOnesAlone) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir baseScratch{Location::InsideRepo, "program"};
    ScratchDir elsewhereScratch{Location::InsideRepo, "program"};
    auto const base      = baseScratch.path();
    auto const elsewhere = elsewhereScratch.path();
    std::filesystem::create_directories(base / "src");
    writeText(base / "src" / "a.c", "int a(void){return 0;}\n");
    writeText(elsewhere / "far.c", "int far(void){return 0;}\n");

    ScopedCwd const cwd{elsewhere};
    DiagnosticReporter rep;
    auto const r = expandAndDedupProjectSources(
        {"src/*.c", (elsewhere / "*.c").generic_string()}, base, rep);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(r->size(), 2u);
    EXPECT_EQ(std::filesystem::path{(*r)[0]}.lexically_normal(),
              (base / "src" / "a.c").lexically_normal())
        << "a RELATIVE glob walks the base directory";
    EXPECT_EQ(std::filesystem::path{(*r)[1]}.lexically_normal(),
              (elsewhere / "far.c").lexically_normal())
        << "an ABSOLUTE glob keeps its own base — `baseDir` must not re-root it";
}

// ── An ABSOLUTE literal is likewise left where it points ────────────────
TEST(ProjectSources, AbsoluteLiteralEntryIsNeverRebased) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir baseScratch{Location::InsideRepo, "program"};
    ScratchDir otherScratch{Location::InsideRepo, "program"};
    auto const base  = baseScratch.path();
    auto const other = otherScratch.path();
    writeText(other / "abs.c", "int abs_src(void){return 0;}\n");

    DiagnosticReporter rep;
    auto const r = expandAndDedupProjectSources(
        {(other / "abs.c").generic_string()}, base, rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ(std::filesystem::path{(*r)[0]}.lexically_normal(),
              (other / "abs.c").lexically_normal())
        << "an absolute entry states its own base; joining `baseDir` onto it "
           "would silently relocate a path that was already exact";
}

// ── SF4: ABSOLUTE-vs-RELATIVE spellings of ONE file collapse ────────────
// The predecessor key (`lexically_normal`) explicitly conceded this pair as an
// "accepted un-caught extreme edge". Once one manifest contributes absolute
// paths and another relative ones, it is the NORMAL case — and its consequence
// is a DUPLICATE CU and a duplicate-symbol link error nothing can tie back to a
// manifest. `weakly_canonical` is what closes it.
//
// RED-ON-DISABLE (`src/program/project_sources.cpp`, the dedup key): key on
// `fs::path{s}.lexically_normal().generic_string()` instead and the relative
// spelling never matches the absolute one, so `r->size()` is 2, not 1.
TEST(ProjectSources, AbsoluteAndRelativeSpellingsOfOneFileDedupToOne) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "shared.c", "int shared(void){return 0;}\n");

    ScopedCwd const cwd{dir};
    DiagnosticReporter rep;
    // Entry 1 is RELATIVE (resolved against cwd, no base); entry 2 is the SAME
    // file spelled ABSOLUTELY. Lexical normalization cannot see through this —
    // it never learns the cwd.
    auto const r = expandAndDedupProjectSources(
        {"shared.c", (dir / "shared.c").generic_string()},
        std::filesystem::path{}, rep);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(r->size(), 1u)
        << "the same file under two spellings must compile ONCE — two CUs would "
           "be a duplicate-symbol link error two tiers from its cause";
    EXPECT_EQ((*r)[0], "shared.c")
        << "first occurrence wins AND keeps its original spelling";
}

// ── ORDER IS LOAD-BEARING (AP6 M4b, the artifact-name hazard) ───────────
// The artifact takes its name from the stem of `[0]` when no `artifactName` is
// stated, and archive member names follow it too. So the resolution must
// preserve MANIFEST order across entries even when a later entry sorts earlier;
// only the matches WITHIN one glob are sorted.
TEST(ProjectSources, ManifestOrderIsPreservedAcrossEntries) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "zeta.c",  "int z(void){return 0;}\n");
    std::filesystem::create_directories(dir / "sub");
    writeText(dir / "sub" / "alpha.c", "int a1(void){return 0;}\n");
    writeText(dir / "sub" / "beta.c",  "int b1(void){return 0;}\n");

    DiagnosticReporter rep;
    auto const r = expandAndDedupProjectSources(
        {(dir / "zeta.c").generic_string(),
         (dir / "sub" / "*.c").generic_string()},
        std::filesystem::path{}, rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 3u);
    EXPECT_EQ(std::filesystem::path{(*r)[0]}.filename().string(), "zeta.c")
        << "the FIRST manifest entry stays first — it is what names the "
           "artifact, so re-ordering this list renames the emitted binary";
    EXPECT_EQ(std::filesystem::path{(*r)[1]}.filename().string(), "alpha.c")
        << "matches WITHIN one glob are sorted (deterministic CU order)";
    EXPECT_EQ(std::filesystem::path{(*r)[2]}.filename().string(), "beta.c");
}

// ── The zero-match refusal NAMES the base it searched ───────────────────
// "matched no files" without saying WHERE it looked is unactionable the moment
// the base stops being the cwd — the author's next question is exactly which
// directory was searched.
TEST(ProjectSources, ZeroMatchFailsLoudNamingTheBaseItSearched) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const base = scratch.path();

    DiagnosticReporter rep;
    auto const r = expandAndDedupProjectSources({"src/*.zzz"}, base, rep);
    EXPECT_FALSE(r.has_value()) << "a pattern that matches nothing is an error";
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_FileNotFound), 1u);
    auto const msg = firstMessageForCode(rep, DiagnosticCode::D_FileNotFound);
    EXPECT_NE(msg.find("src/*.zzz"), std::string::npos)
        << "the message must name the pattern; got: " << msg;
    EXPECT_NE(msg.find(base.generic_string()), std::string::npos)
        << "and the BASE it searched, or the author cannot tell where to look; "
           "got: " << msg;
}

// The no-base spelling of the same refusal still says "the working directory"
// rather than an empty string — an error message must describe something.
TEST(ProjectSources, ZeroMatchWithNoBaseNamesTheWorkingDirectory) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::InsideRepo, "program"};
    ScopedCwd const cwd{scratch.path()};
    DiagnosticReporter rep;
    auto const r = resolveNoBase({"*.zzz"}, rep);
    EXPECT_TRUE(r.empty());
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_FileNotFound), 1u);
    EXPECT_NE(firstMessageForCode(rep, DiagnosticCode::D_FileNotFound)
                  .find("the working directory"),
              std::string::npos);
}

// ════════════════════════════════════════════════════════════════
// AP6 — `Program::artifactPaths()`: the driver states what it wrote
// ════════════════════════════════════════════════════════════════

// ── Index-parallel, engaged, and EQUAL to the real file on disk ─────────
// Two targets whose `formatName` DIFFERS, so the `<formatName>` component of
// the convention is actually exercised (one target could not prove it).
//
// RED-ON-DISABLE (`src/program/program.cpp`, the per-target loop): stop
// assigning `artifactsOut[i]` and the vector is all-`nullopt` — every
// `ASSERT_TRUE(paths[i].has_value())` fails while the build still returns 0,
// which is precisely the "the value exists but reaches nobody" shape.
TEST(ProgramArtifactPaths, IndexParallelToTargetsAndEqualToTheFilesOnDisk) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "twoway.c", "int main(void){ return 0; }\n");
    std::string const manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"x86_64:elf64-x86_64-linux-exec\", "
          "\"x86_64:pe64-x86_64-windows-exec\"],\n"
        + "  \"sources\": [\"" + (dir / "twoway.c").generic_string() + "\"]\n}";
    auto const proj = dir / "twoway.dss-project.json";
    writeText(proj, manifest);

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0);

    auto const& paths = prog.artifactPaths();
    ASSERT_EQ(paths.size(), 2u)
        << "one entry per target of the run, in the manifest's target order";
    ASSERT_TRUE(paths[0].has_value());
    ASSERT_TRUE(paths[1].has_value());
    EXPECT_EQ(paths[0]->lexically_normal(),
              (dir / "out" / "elf64-x86_64-linux-exec" / "twoway")
                  .lexically_normal())
        << "index 0 is the FIRST target's artifact, `<formatName>` included";
    EXPECT_EQ(paths[1]->lexically_normal(),
              (dir / "out" / "pe64-x86_64-windows-exec" / "twoway.exe")
                  .lexically_normal())
        << "index 1 carries the SECOND format's own extension — the two entries "
           "differ in both directory and suffix, so a broadcast of one answer "
           "to both targets cannot pass";
    EXPECT_TRUE(std::filesystem::exists(*paths[0]))
        << "the reported path must be the file that was actually written";
    EXPECT_TRUE(std::filesystem::exists(*paths[1]));
}

// ── A FAILED target reports `nullopt`, not a path ───────────────────────
// The half that makes the contract fail-closed: a consumer must not be handed a
// path for an artifact that was never written. Driven by the format-capability
// gate — ✔MEASURED, only the three `*-exec` PE/Mach-O formats declare
// `stackReserveControl`, so ONE request refuses on ELF and succeeds on PE
// inside a single run.
//
// RED-ON-DISABLE (`src/program/program.cpp`, the per-target loop): assign
// `artifactsOut[i]` unconditionally from `compileOneTarget`'s result and the
// ELF entry stops being `nullopt` — `EXPECT_FALSE(paths[1].has_value())` fails
// while the build's own non-zero rc is unchanged.
TEST(ProgramArtifactPaths, FailedTargetLeavesNulloptWhileItsSiblingSucceeds) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "halffail.c", "int main(void){ return 0; }\n");

    Program prog;
    prog.setOutputDir(dir / "out");
    prog.setStackReserveBytes(std::uint64_t{8u * 1024u * 1024u});
    DiagnosticReporter rep;
    // PE first, ELF second: PE declares stackReserveControl, ELF does not.
    int const rc = prog.compileFiles(
        {(dir / "halffail.c").generic_string()}, "c",
        {"x86_64:pe64-x86_64-windows-exec", "x86_64:elf64-x86_64-linux-exec"},
        rep);
    EXPECT_NE(rc, 0) << "the ELF target must refuse the reserve it cannot carry";
    ASSERT_EQ(countCode(rep, DiagnosticCode::K_FormatLacksStackReserveControl), 1u)
        << "exactly one target failed, and for the reason this test intends";

    auto const& paths = prog.artifactPaths();
    ASSERT_EQ(paths.size(), 2u)
        << "index-parallelism survives a partial failure";
    ASSERT_TRUE(paths[0].has_value())
        << "the target that succeeded still reports its artifact";
    EXPECT_TRUE(std::filesystem::exists(*paths[0]));
    EXPECT_FALSE(paths[1].has_value())
        << "a target that wrote nothing must report nothing — a path here would "
           "let a consumer link an artifact that does not exist";
}

// ── `--output` ABSENT ⇒ the base is `<cwd>/target/<formatName>` ─────────
// Previously an implicit `else` branch; it is now half of a returned contract,
// so it is stated and pinned. cwd is moved into the scratch dir so the
// convention's `<cwd>` component is a directory this test owns.
TEST(ProgramArtifactPaths, WithoutOutputDirTheBaseIsCwdSlashTarget) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "defaulted.c", "int main(void){ return 0; }\n");
    scratch.useAsCwd();

    Program prog;   // deliberately NO setOutputDir
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileFiles({"defaulted.c"}, "c",
                                {"x86_64:elf64-x86_64-linux-exec"}, rep), 0);
    auto const& paths = prog.artifactPaths();
    ASSERT_EQ(paths.size(), 1u);
    ASSERT_TRUE(paths[0].has_value());
    EXPECT_EQ(paths[0]->lexically_normal(),
              (dir / "target" / "elf64-x86_64-linux-exec" / "defaulted")
                  .lexically_normal())
        << "with no --output the artifacts land under <cwd>/target/<formatName>";
    EXPECT_TRUE(std::filesystem::exists(*paths[0]));
}

// ── The vector describes THE LAST RUN ───────────────────────────────────
// A second call that is rejected before any target is reached must not leave
// the first call's artifacts readable — a consumer would link a binary this
// invocation never built.
TEST(ProgramArtifactPaths, ARejectedSecondRunClearsThePreviousRunsPaths) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "once.c", "int main(void){ return 0; }\n");

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep1;
    ASSERT_EQ(prog.compileFiles({(dir / "once.c").generic_string()}, "c",
                                {"x86_64:elf64-x86_64-linux-exec"}, rep1), 0);
    ASSERT_EQ(prog.artifactPaths().size(), 1u);
    ASSERT_TRUE(prog.artifactPaths()[0].has_value());

    DiagnosticReporter rep2;
    EXPECT_NE(prog.compileFiles({}, "c",
                                {"x86_64:elf64-x86_64-linux-exec"}, rep2), 0);
    EXPECT_TRUE(prog.artifactPaths().empty())
        << "a run that never reached a target reports NO artifacts — stale "
           "paths from the previous run would be read as this run's output";
}

// ════════════════════════════════════════════════════════════════
// AP6 M4b — the artifact NAME follows the ROOT's OWN first source
// ════════════════════════════════════════════════════════════════

// ✔MEASURED: `sourceStem = fs::path{sourceFiles.front()}.stem()` names the
// artifact whenever the manifest states no `artifactName`, and names archive
// members too. Once merged dependency sources join the list, WHICHEVER SOURCE
// IS FIRST decides the binary's name — so a lane that prepends, appends-then-
// sorts, or otherwise re-orders the union silently RENAMES the output.
//
// The manifest below lists its sources in NON-alphabetical order on purpose:
// `zeta.c` is first and `alpha.c` second, so "the first entry wins" and "the
// lexicographically smallest wins" give DIFFERENT answers and the pin can tell
// them apart.
//
// RED-ON-DISABLE (`src/program/project_sources.cpp`): sort the returned list
// before returning it and the artifact becomes `alpha` — the `zeta` existence
// assertion fails, `artifactPaths()[0]` reports the `alpha` name, and the
// `alpha` non-existence assertion fails as well.
TEST(CompileProjectArtifactName, FollowsTheManifestsOwnFirstSourceNotSortOrder) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;
    std::string const target = "x86_64:elf64-x86_64-linux-exec";
    ScratchDir scratch{Location::InsideRepo, "program"};
    auto const dir = scratch.path();
    writeText(dir / "zeta.c",
              "int helper(void);\nint main(void){ return helper(); }\n");
    writeText(dir / "alpha.c", "int helper(void){ return 0; }\n");
    std::string const manifest =
        std::string{"{\n  \"language\": \"c\",\n"}
        + "  \"artifactProfile\": \"cli\",\n"
        + "  \"targets\": [\"" + target + "\"],\n"
        + "  \"sources\": [\"" + (dir / "zeta.c").generic_string() + "\", \""
        + (dir / "alpha.c").generic_string() + "\"]\n}";   // NOT alphabetical
    auto const proj = dir / "named.dss-project.json";
    writeText(proj, manifest);

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0);

    auto const outDir = dir / "out" / "elf64-x86_64-linux-exec";
    EXPECT_TRUE(std::filesystem::exists(outDir / "zeta"))
        << "the artifact is named from the stem of the manifest's FIRST source";
    EXPECT_FALSE(std::filesystem::exists(outDir / "alpha"))
        << "sorting the source list would rename the binary to 'alpha' — the "
           "silent rename a merged dependency source must never cause";
    auto const& paths = prog.artifactPaths();
    ASSERT_EQ(paths.size(), 1u);
    ASSERT_TRUE(paths[0].has_value());
    EXPECT_EQ(paths[0]->stem().string(), "zeta")
        << "and the driver's own report of what it wrote agrees";
}

// ── The fixture-mode entry point ────────────────────────────────────────────
//
// `argv[1]` is inspected BEFORE `InitGoogleTest`, which would otherwise reject
// the unknown flag. The check is on the FIRST argument only (the protocol is
// positional), so no gtest invocation and no payload argument can be mistaken
// for it. Defining `main` here suppresses the `GTest::gtest_main` archive
// member entirely — the same arrangement `tests/program/test_build_scripts.cpp`
// uses, for the same reason.
int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view{argv[1]}.substr(0, kHookExitFlag.size())
                         == kHookExitFlag) {
        return runHookFixture(argc, argv);
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
