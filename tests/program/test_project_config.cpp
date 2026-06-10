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

// First human message carried for `code` (stored in `actual` by the
// `report()` shim) — for the §7-#3 actionable-message pin.
std::string firstMessageForCode(DiagnosticReporter const& rep,
                                DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return d.actual;
    }
    return {};
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
