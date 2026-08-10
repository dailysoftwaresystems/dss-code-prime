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
//     Unsupported*/EmptySet*/CSubsetRejectsGui pins all go green.
//   * Integration (real shipped c-subset grammar): cli accepted, gui
//     rejected — proves the real grammar's declared span flows through.
//   * Routing (routesToMultiUnit): the shared >1 threshold.
//   * Diagnostic-code name/prefix round-trip.
//
// Plan 06 AP4 — per-language ONBOARDING matrix (appended at the end).
// AP2/AP3 exercised the gates with c-subset ONLY; AP4 completes the
// matrix across ALL THREE shipped languages (toy / c-subset / tsql-
// subset) and adds the single real end-to-end emit through
// compileProject. See the "AP4" banner below for the scope + honesty
// notes (no shipped format serves a non-cli profile; the profile does
// NOT yet drive artifact shape — that is D-AP2-COMPILATION-CONTEXT).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/glob_match.hpp"          // D-AP2-SOURCES-GLOB: matcher + expander
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "link/object_format_schema.hpp"  // ObjectFormatSchema::loadShipped (AP3 format-gate integration)
#include "program/program.hpp"          // Program, routesToMultiUnit
#include "program/project_config.hpp"
#include "run_binary.hpp"               // runBinary (behavioral exit-code proof)
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
  "language": "c-subset",
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
    EXPECT_EQ(pc->language, "c-subset");
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "targets": ["t:f"], "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 1u);
}

TEST(ProjectConfigLoader, MissingTargetsFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli", "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 1u);
}

TEST(ProjectConfigLoader, EmptyTargetsFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli",
      "targets": [], "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 1u);
}

TEST(ProjectConfigLoader, MissingSourcesFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli", "targets": ["t:f"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MissingField), 1u);
}

TEST(ProjectConfigLoader, EmptySourcesFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
      "targets": "not-an-array", "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, NonStringTargetEntryFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli",
      "targets": [42], "sources": ["a.c"]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, WrongTypeOutputFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "output": 42
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, EmptyOutputStringFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"]
    })", "p.json", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(pc->artifactName.has_value());
}

TEST(ProjectConfigLoader, EmptyArtifactNameFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "artifactName": ""
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, WrongTypeArtifactNameFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "artifactName": "foo/bar"
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

TEST(ProjectConfigLoader, ArtifactNameWithBackslashFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "includes": "not-an-array"
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// REJECT: a non-string entry (a number in the array).
TEST(ProjectConfigLoader, NonStringDefineEntryFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "defines": [1]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// REJECT: an empty-string entry (fails loud like the required-array helper).
TEST(ProjectConfigLoader, EmptyStringIncludeEntryFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli",
      "targets": ["t:f"], "sources": ["a.c"], "includes": [""]
    })", "p.json", rep);
    EXPECT_FALSE(pc.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// REJECT: a non-array resolveLibraries (the third field, same rule).
TEST(ProjectConfigLoader, NonArrayResolveLibrariesFailsLoud) {
    DiagnosticReporter rep;
    auto pc = parseProjectConfig(R"({
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
      "language": "c-subset", "artifactProfile": "cli",
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
    EXPECT_TRUE(enforceArtifactProfile(asSpan(declared), "cli", "c-subset", rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(sawCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported));
}

// RED-on-disable lever: disable artifactProfileSupported() (always
// return true) and this expectation flips green.
TEST(EnforceArtifactProfile, RejectsUnsupportedProfileFailLoud) {
    std::vector<std::string> declared = {"cli", "lib", "staticlib"};
    DiagnosticReporter rep;
    EXPECT_FALSE(enforceArtifactProfile(asSpan(declared), "gui", "c-subset", rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 1u);
}

// Empty-set DIAGNOSTIC: the fail-closed reject emits the same code.
TEST(EnforceArtifactProfile, RejectsEmptySetFailLoud) {
    std::vector<std::string> declared = {};
    DiagnosticReporter rep;
    EXPECT_FALSE(enforceArtifactProfile(asSpan(declared), "cli", "no-profiles-lang", rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 1u);
}

// ── Integration: the real shipped c-subset grammar ──────────────
// c-subset declares ["cli","lib","staticlib"] (AP1). Proves the real
// grammar's artifactProfiles() span flows through the gate.

TEST(EnforceArtifactProfileShipped, CSubsetAcceptsCli) {
    auto g = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(g.has_value());
    DiagnosticReporter rep;
    EXPECT_TRUE(enforceArtifactProfile((*g)->artifactProfiles(), "cli", "c-subset", rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(EnforceArtifactProfileShipped, CSubsetRejectsGui) {
    auto g = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(g.has_value());
    // Sanity: the real grammar declares a non-empty set excluding gui.
    EXPECT_FALSE((*g)->artifactProfiles().empty());
    DiagnosticReporter rep;
    EXPECT_FALSE(enforceArtifactProfile((*g)->artifactProfiles(), "gui", "c-subset", rep));
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
// profile c-subset doesn't declare → D_ArtifactProfileNotSupported,
// rejected BEFORE any compile (the source path need not exist).
TEST(CompileProjectIntegration, UnsupportedProfileRejectedBeforeCompile) {
    dss::test_support::ScratchDir scratch{
        dss::test_support::Location::Temp, "program"};
    auto path = writeProjectFile(scratch.path(), R"({
      "language": "c-subset", "artifactProfile": "gui",
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
      "language": "c-subset", "artifactProfile": "cli",
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

TEST(EnforceArtifactProfileFormat, ServedProfileAcceptedSilently) {
    std::vector<std::string> served = {"cli"};
    DiagnosticReporter rep;
    EXPECT_TRUE(enforceArtifactProfileFormat(asSpan(served), "cli",
                                             "elf64-x86_64-linux-exec", rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

// RED-on-disable: make artifactProfileSupported always-true → this flips green.
TEST(EnforceArtifactProfileFormat, UnservedProfileFailsLoud) {
    std::vector<std::string> served = {"cli"};  // an exec format
    DiagnosticReporter rep;
    EXPECT_FALSE(enforceArtifactProfileFormat(asSpan(served), "lib",
                                              "elf64-x86_64-linux-exec", rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
}

// A relocatable / backend-less format serves NOTHING → rejects any profile
// (fail-closed, the format-side twin of the empty-language-set reject).
TEST(EnforceArtifactProfileFormat, EmptyServedSetRejects) {
    std::vector<std::string> served = {};
    DiagnosticReporter rep;
    EXPECT_FALSE(enforceArtifactProfileFormat(asSpan(served), "cli",
                                              "elf64-x86_64-linux", rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
}

// Integration with the REAL shipped format's served set.
TEST(EnforceArtifactProfileFormatShipped, ExecServesCliRejectsLib) {
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(f.has_value());
    EXPECT_FALSE((*f)->artifactProfiles().empty());
    DiagnosticReporter rep1;
    EXPECT_TRUE(enforceArtifactProfileFormat((*f)->artifactProfiles(), "cli",
                                             "elf64-x86_64-linux-exec", rep1));
    DiagnosticReporter rep2;
    EXPECT_FALSE(enforceArtifactProfileFormat((*f)->artifactProfiles(), "lib",
                                              "elf64-x86_64-linux-exec", rep2));
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
      "language": "c-subset", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["nonexistent-src.c"]
    })");
    Program prog;
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(path.string(), rep), 0);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 0u)
        << "cli IS served by an exec format — the format gate must not trip";
}

// The AP3 deliverable end-to-end: a profile the LANGUAGE declares (lib ∈
// c-subset) but the CHOSEN FORMAT (an executable) does NOT serve →
// D_ArtifactProfileFormatMismatch, rejected before any compile. The AP2
// language gate must NOT fire (lib is declared by c-subset) — proving the two
// gates are distinct.
TEST(CompileProjectIntegration, LibProfileOnExecFormatMismatch) {
    dss::test_support::ScratchDir scratch{
        dss::test_support::Location::Temp, "program"};
    auto path = writeProjectFile(scratch.path(), R"({
      "language": "c-subset", "artifactProfile": "lib",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"]
    })");
    Program prog;
    DiagnosticReporter rep;
    EXPECT_EQ(prog.compileProject(path.string(), rep), 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 0u);
}

// N1: multi-target — one serving, one not — fails the build with the mismatch
// (the gate checks EVERY target; order-independent). The relocatable format
// serves nothing, so the cli profile trips on that target.
TEST(CompileProjectIntegration, MultiTargetOneMismatchFailsLoud) {
    dss::test_support::ScratchDir scratch{
        dss::test_support::Location::Temp, "program"};
    auto path = writeProjectFile(scratch.path(), R"({
      "language": "c-subset", "artifactProfile": "cli",
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
// AP2/AP3 exercised the two driver gates with c-subset ONLY. AP4
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
//   * No shipped object format SERVES a non-cli profile (the four exec
//     formats serve ["cli"]; relocatable/spirv/wasm serve nothing). So
//     lib/staticlib/script/sproc have NO positive format-gate cell —
//     here they can only ever be REJECTED. A future shared-library /
//     SQL-emit backend would add the serving format + the positive
//     cell with ZERO gate-code change (the gate is generic set-
//     membership, never a format-name branch).
//   * toy & tsql-subset are onboarded here in the GATE sense only —
//     they emit no artifact in this matrix. c-subset is the sole real
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

// ── c-subset: declares ["cli","lib","staticlib"] ────────────────
// (AP2/AP3 already cover cli-passes + lib-mismatch; AP4 adds staticlib
//  + the §7-#3 actionable-message pin.)

TEST(ArtifactProfileMatrix, CSubsetStaticlibMismatchByFormatGate) {
    DiagnosticReporter rep;
    int const rc = runMatrixCell("c-subset", "staticlib", kExecTarget, rep);
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 0u)
        << "staticlib IS declared by c-subset — the LANGUAGE gate must not fire";
}

// §7 criterion #3: the language-gate message must NAME the language and
// LIST the supported set (actionable remediation) — not just a code.
// (red-on-disable: if enforceArtifactProfile dropped the list, the
// "staticlib" find() fails.)
TEST(ArtifactProfileMatrix, CSubsetGuiMessageNamesLanguageAndSupportedSet) {
    DiagnosticReporter rep;
    (void) runMatrixCell("c-subset", "gui", kExecTarget, rep);
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 1u);
    std::string const m =
        firstMessageForCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported);
    EXPECT_NE(m.find("c-subset"), std::string::npos)
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
TEST(ArtifactProfileMatrix, TsqlScriptMismatchByFormatGate) {
    DiagnosticReporter rep;
    int const rc = runMatrixCell("tsql-subset", "script", kExecTarget, rep);
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 0u)
        << "script IS declared by tsql-subset — the LANGUAGE gate must not fire";
}

TEST(ArtifactProfileMatrix, TsqlSprocMismatchByFormatGate) {
    DiagnosticReporter rep;
    int const rc = runMatrixCell("tsql-subset", "sproc", kExecTarget, rep);
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileFormatMismatch), 1u);
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
// project-config path actually COMPILES + EMITS: a real cli c-subset
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
      "language": "c-subset", "artifactProfile": "cli",
      "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"]
    })");
    scratch.useAsCwd();

    Program prog;
    DiagnosticReporter rep;
    int const rc = prog.compileProject(path.string(), rep);
    ASSERT_EQ(rc, 0)
        << "a real cli c-subset project must compile + emit via compileProject";
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
        std::string{"{\n  \"language\": \"c-subset\",\n"}
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
        auto const r = runBinary(exe, std::chrono::milliseconds{5000});
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
        auto const r = runBinary(exe, std::chrono::milliseconds{5000});
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
        auto const r = runBinary(exe, std::chrono::milliseconds{5000});
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
        std::string{"{\n  \"language\": \"c-subset\",\n"}
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
        auto const r = runBinary(routed, std::chrono::milliseconds{5000});
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
    int const rc = prog.compileFiles({srcPath.string()}, "c-subset", {target}, rep);
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
        std::string{"{\n  \"language\": \"c-subset\",\n"}
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
        return std::string{"{\n  \"language\": \"c-subset\",\n"}
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
        return std::string{"{\n  \"language\": \"c-subset\",\n"}
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
        return std::string{"{\n  \"language\": \"c-subset\",\n"}
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
        return std::string{"{\n  \"language\": \"c-subset\",\n"}
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
        std::string{"{\n  \"language\": \"c-subset\",\n"}
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
        std::string{"{\n  \"language\": \"c-subset\",\n"}
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
      "language": "c-subset", "artifactProfile": "cli",
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
        std::string{"{\n  \"language\": \"c-subset\",\n"}
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
        std::string{"{\n  \"language\": \"c-subset\",\n"}
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
        std::string{"{\n  \"language\": \"c-subset\",\n"}
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
        std::string{"{\n  \"language\": \"c-subset\",\n"}
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
        std::string{"{\n  \"language\": \"c-subset\",\n"}
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
    auto const r = runBinary(exe, std::chrono::milliseconds{5000});
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
        std::string{"{\n  \"language\": \"c-subset\",\n"}
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
    auto const r = runBinary(exe, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "the deduped union (main.c once + other.c once) links + runs → 42";
}
#endif  // host-native exec (Windows or Linux-x86_64)
