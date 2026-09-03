// WHICH CONFIG TREE ANSWERED — the end-to-end pin for
// [[D-PROGRAM-CONFIG-DIR-WALK-RESOLVES-A-FOREIGN-TREE]].
//
// ── WHAT WAS BROKEN, ✔MEASURED THROUGH THE SHIPPED CLI 2026-09-02 ──────────
// A binary built from tree A, invoked with a working directory inside tree B,
// reads TREE B's `src/dss-config` — and then answers every vocabulary question
// out of it, correctly, about config the caller did not think they were using.
// One variable changed (the process working directory), same binary,
// `DSS_CONFIG_ROOT` unset in BOTH arms, a second tree planted outside the
// repository with the `fadd` row removed from its arm64 dialect document:
//
//     cwd = the build tree → rc 0, artifact written
//     cwd = tree B         → rc 1
//       error[A_AsmTextUnsupported]: unknown mnemonic 'fadd' … (assembly
//       dialect 'AsmArm64Gas', target 'arm64')
//
// The refusal was TRUE and completely unattributable. A dialect NAME is
// identical in every checkout, so that message reads exactly like "the row you
// just added does not work" — which is how cycle P54's lane `el` read it, and
// it cost a full cycle of measurement.
//
// ── WHAT IS PINNED HERE, AND WHY EACH IS THE ONE THAT WOULD HAVE CAUGHT IT ──
//
//   1. THE REFUSAL NAMES BOTH DOCUMENTS BY PATH. The path is the ONLY part of
//      that sentence that differs between two checkouts, so it is the only part
//      that can distinguish the two readings. Asserted against the paths
//      composed from `repoRoot()` — i.e. the test states independently where
//      the documents must have come from, rather than accepting whatever the
//      compiler printed.
//   2. THE INVOCATION SAYS SO WHEN THE WALK ANSWERED A FOREIGN TREE, through
//      BOTH entry points that can reach config: `Program::run` (the whole
//      dispatch fork) and `dumpPredefinedMacros` (answered ahead of `Program`
//      in `main`, so the other site cannot cover it).
//   3. THE CONTROL — with `DSS_CONFIG_ROOT` set, exactly as `dss_add_test`
//      sets it for every entry in this suite, NOTHING is printed. This one is
//      load-bearing rather than decorative: a report that fired for an ordinary
//      in-tree build would put a line on every compile in the project, which is
//      the "fail-more" the row explicitly forbids.
//
// ⚠ NOTHING HERE REFUSES ANYTHING, and that is the design rather than a gap.
// Running a binary against another tree's config is LEGITIMATE —
// `DSS_CONFIG_ROOT` exists to do it — so the remedy is attribution, never a
// rejection. A test asserting a non-zero rc for a cross-tree invocation would
// be pinning a regression.
//
// ⓘ (2) and (3) plant a tree whose `src/dss-config` is EMPTY, and that is
// sufficient BY CONSTRUCTION: the report is emitted before any document is
// loaded, so what follows it (a loud config-not-found) is not what is being
// measured and is deliberately not asserted on.
//
// The arm-by-arm behaviour of the note itself lives one tier down in
// `tests/core/test_config_path_walk.cpp` (`ConfigRootProvenance*`), which can
// drive the INSTALLED arm that no development build can otherwise reach.
//
// Compile-only + a fixed cross-target, so this is HOST-AGNOSTIC and runs on
// every leg: no artifact is executed here.

#include "core/substrate/path_identity.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "program/cli_args.hpp"
#include "program/dump_predefined_macros.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace fs = std::filesystem;

namespace {

// A fixed arm64 ELF target: nothing is RUN, so this cross-compiles on every
// host. The dialect that spells arm64 assembly is `asm-arm64-gas.lang.json`,
// which is the document whose path claim (1) is about.
constexpr char const* kTarget = "arm64:elf64-aarch64-linux-exec";

// An UNDECLARED mnemonic. `frobnicate` is not in any shipped dialect and never
// will be, so this reaches the unknown-mnemonic refusal without depending on
// which real instructions happen to be spelled today — a source that used a
// real-but-currently-unspelled mnemonic would go green the day someone declared
// it, and would then be pinning nothing.
constexpr char const* kSource =
    "int main(void) { __asm__(\"frobnicate\"); return 0; }\n";

// RAII cwd pin. `ScratchDir::useAsCwd` REFUSES `Location::Temp` because a temp
// scratch is outside the repo and would break an ordinary schema lookup; here
// the outside-the-repo cwd IS the experiment, so this file pins cwd itself —
// the same reason `tests/program/test_system_dirs_cwd_independent.cpp` does.
class ScopedCwd {
public:
    explicit ScopedCwd(fs::path const& to) : prev_(fs::current_path()) {
        fs::current_path(to);
    }
    ~ScopedCwd() {
        std::error_code ec;
        fs::current_path(prev_, ec);   // a dtor must not throw
    }
    ScopedCwd(ScopedCwd const&)            = delete;
    ScopedCwd& operator=(ScopedCwd const&) = delete;

private:
    fs::path prev_;
};

// RAII stderr capture. The provenance line is a REPORT line, not a diagnostic
// (a `Warning` would be promoted to an error by `--warnings-as-errors`, and an
// `Info` diagnostic is droppable through three gates in
// `DiagnosticReporter::report`), so a reporter cannot observe it — the stream
// is the only place it exists.
class CapturedCerr {
public:
    CapturedCerr() : prev_(std::cerr.rdbuf(buf_.rdbuf())) {}
    ~CapturedCerr() { std::cerr.rdbuf(prev_); }
    CapturedCerr(CapturedCerr const&)            = delete;
    CapturedCerr& operator=(CapturedCerr const&) = delete;

    [[nodiscard]] std::string text() const { return buf_.str(); }

private:
    std::ostringstream buf_;
    std::streambuf*    prev_;
};

// Every diagnostic's rendered body, concatenated — what the operator reads.
[[nodiscard]] std::string allDiagnosticText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        out += d.actual;
        out += '\n';
    }
    return out;
}

[[nodiscard]] bool contains(std::string const& haystack,
                            std::string const& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Plant `<root>/src/dss-config/` as an EMPTY directory — enough for the cwd
// walk to accept `<root>` as a config root, which is all the provenance report
// needs to fire. See the ⓘ note at the top for why nothing more is planted.
fs::path plantEmptyConfigRoot(fs::path const& root) {
    fs::path const dir = root / "src" / "dss-config";
    std::error_code ec;
    fs::create_directories(dir, ec);
    EXPECT_FALSE(ec) << "plantEmptyConfigRoot: " << ec.message();
    return dir;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// (1) THE REFUSAL NAMES THE DOCUMENTS IT CONSULTED
// ════════════════════════════════════════════════════════════════════════════
//
// RED ON DISABLE: revert `AsmDiagnosticSink::pairSuffix` to the name-only form
// it had before this row closed and this test fails on both path assertions,
// while the mnemonic assertion below — the control — stays green. That
// asymmetry is the whole point: the refusal was always correct, it was only
// ever unattributable.
TEST(ConfigRootAttribution, AsmRefusalNamesTheDialectAndTargetDocuments) {
    fs::path const repo = dss::test::repoRoot();   // resolve BEFORE moving cwd
    ScratchDir     scratch(Location::Temp, "config-root-attr");
    fs::path const src = scratch.path() / "main.c";
    std::ofstream(src) << kSource;

    ScopedEnv env("DSS_CONFIG_ROOT", repo.string());

    Program p;
    p.setOutputDir(scratch.path() / "out");
    DiagnosticReporter rep;
    int const rc = p.compileFiles(std::vector<std::string>{src.string()}, "c",
                                  std::vector<std::string>{kTarget}, rep);
    ASSERT_NE(rc, 0) << "an undeclared mnemonic must be refused, not guessed at";

    std::string const text = allDiagnosticText(rep);

    // The control: the refusal itself, unchanged by this row.
    EXPECT_TRUE(contains(text, "unknown mnemonic 'frobnicate'")) << text;

    // The claim: it names the tree that answered, for BOTH documents the
    // sentence tells the reader to open.
    std::string const dialectDoc = dss::core::genericSpelling(
        repo / "src" / "dss-config" / "sources" / "asm-arm64-gas.lang.json");
    std::string const targetDoc = dss::core::genericSpelling(
        repo / "src" / "dss-config" / "targets" / "arm64.target.json");
    EXPECT_TRUE(contains(text, dialectDoc))
        << "the refusal must name the DIALECT document that answered — a name "
           "alone is identical in every checkout, so it cannot distinguish "
           "'this row is wrong' from 'you are reading another tree'.\nwanted: "
        << dialectDoc << "\ngot:\n"
        << text;
    EXPECT_TRUE(contains(text, targetDoc))
        << "the refusal already promises the reader TWO documents to open; "
           "naming one by path and the other by name leaves half of that pair "
           "unopenable.\nwanted: "
        << targetDoc << "\ngot:\n"
        << text;
}

// ════════════════════════════════════════════════════════════════════════════
// (2) THE INVOCATION SAYS WHICH TREE IT WALKED TO
// ════════════════════════════════════════════════════════════════════════════
//
// RED ON DISABLE: remove the `reportConfigRootProvenance` call from
// `Program::run` and this fails with an empty capture, while the control below
// stays green.
TEST(ConfigRootAttribution, ProgramRunReportsAWalkedForeignTree) {
    ScratchDir     foreign(Location::Temp, "config-root-attr");
    fs::path const foreignConfig = plantEmptyConfigRoot(foreign.path());

    ScopedEnv env("DSS_CONFIG_ROOT");        // construct-to-clear: force the walk
    ScopedCwd cwd(foreign.path());

    std::string captured;
    {
        CapturedCerr cap;
        char const*  argv[] = {"dsscp",   "--compile", "nonexistent.c",
                               "--language", "c",      "--target", kTarget};
        Program      p;
        // The rc is deliberately NOT asserted: the tree is empty, so the run
        // fails loudly afterwards. What is measured is the line emitted BEFORE
        // any of that.
        (void)p.run(static_cast<int>(std::size(argv)),
                    const_cast<char**>(argv));
        captured = cap.text();
    }

    EXPECT_TRUE(contains(captured, "dsscp: config root"))
        << "an invocation whose config came from a walked foreign tree must say "
           "so:\n"
        << captured;
    EXPECT_TRUE(contains(captured, dss::core::genericSpelling(foreignConfig)))
        << "the report must name the tree that ANSWERED:\n"
        << captured;
    EXPECT_TRUE(contains(captured, "DSS_CONFIG_ROOT"))
        << "the report must say how to choose deliberately:\n"
        << captured;
}

// The OTHER entry point. `--dump-predefined-macros` is answered in `main`
// AHEAD of `Program`, so the site inside `Program::run` cannot reach it — and
// the one instrument whose whole job is to report what a triple resolved to
// must not be the one instrument that cannot say which tree resolved it.
//
// RED ON DISABLE: remove the `reportConfigRootProvenance` call from
// `dumpPredefinedMacros` and only this test fails.
TEST(ConfigRootAttribution, DumpPredefinedMacrosReportsAWalkedForeignTree) {
    ScratchDir     foreign(Location::Temp, "config-root-attr");
    fs::path const foreignConfig = plantEmptyConfigRoot(foreign.path());

    ScopedEnv env("DSS_CONFIG_ROOT");
    ScopedCwd cwd(foreign.path());

    CliArgs args;
    args.dumpPredefinedMacros = true;
    args.languageName         = "c";
    args.targets              = {kTarget};

    std::ostringstream out;
    std::ostringstream err;
    (void)dumpPredefinedMacros(args, out, err);

    EXPECT_TRUE(contains(err.str(), "dsscp: config root")) << err.str();
    EXPECT_TRUE(contains(err.str(),
                         dss::core::genericSpelling(foreignConfig)))
        << err.str();
}

// ════════════════════════════════════════════════════════════════════════════
// (3) THE CONTROL — AN ORDINARY INVOCATION SAYS NOTHING
// ════════════════════════════════════════════════════════════════════════════
//
// `DSS_CONFIG_ROOT` set is what `dss_add_test` does for every entry in this
// suite and what the sqlite drivers and the examples runner see, so this arm is
// the ordinary case. It is the assertion that keeps the fix FAIL-LOUD rather
// than FAIL-MORE: a report line on every compile would be noise, and noise on
// an attribution instrument teaches the reader to ignore it.
TEST(ConfigRootAttribution, AnExplicitOverrideReportsNothing) {
    fs::path const repo = dss::test::repoRoot();
    ScratchDir     scratch(Location::Temp, "config-root-attr");

    ScopedEnv env("DSS_CONFIG_ROOT", repo.string());
    ScopedCwd cwd(scratch.path());

    std::ostringstream out;
    std::ostringstream err;
    CliArgs            args;
    args.dumpPredefinedMacros = true;
    args.languageName         = "c";
    args.targets              = {kTarget};
    int const rc = dumpPredefinedMacros(args, out, err);

    EXPECT_EQ(rc, 0) << err.str();
    EXPECT_FALSE(contains(err.str(), "dsscp: config root"))
        << "an operator who named the tree must not be told which tree they "
           "named:\n"
        << err.str();

    // And the same run makes the resolved root INSPECTABLE without provoking an
    // error — the surface that answers "which src/dss-config is this?" without
    // needing something to go wrong first.
    EXPECT_TRUE(contains(out.str(), "config-root="))
        << "the dump's section header must carry the tree it described:\n"
        << out.str();
    EXPECT_TRUE(contains(out.str(),
                         dss::core::genericSpelling(repo / "src"
                                                    / "dss-config")))
        << "and it must be the tree that actually answered, not a re-walk:\n"
        << out.str();
}
