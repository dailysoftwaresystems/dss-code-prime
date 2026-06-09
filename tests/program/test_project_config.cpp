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

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "link/object_format_schema.hpp"  // ObjectFormatSchema::loadShipped (AP3 format-gate integration)
#include "program/program.hpp"          // Program, routesToMultiUnit
#include "program/project_config.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
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
