// UCRT-P4 (D-FFI-PE-CRT-UCRT-MIGRATION + D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE):
// THE REAL-INPUT-PATH WITNESSES for the pe program-entry argv spine.
//
// ★★★ WHY THIS FILE EXISTS AT THE `tests/program/` TIER AND NOT AS AN EXAMPLE.
// The subject under test is "does the OPERATING SYSTEM's command line reach
// `main(int, char**)` intact". Its REAL INPUT PATH is therefore the process
// command line, and nothing else is a substitute: a test that hands the program a
// re-typed `argv` vector, or that only checks the emitted bytes, is testing
// something the OS is not involved in. The examples runner has NO argv-passing
// knob — every example is spawned with no arguments — so `examples/c-subset/
// main_argc_argv` can only ever witness `argc == 1`. Passing REAL arguments needs
// a test that owns the spawn, which is this one.
//
// The three things pinned here, none of which the MIR-tier pins in
// tests/mir/test_mir_merge.cpp can reach:
//   1. RealCommandLineReachesMainByteExact — argc and every argv element,
//      byte-exact, through a genuine CreateProcess command line: three arguments,
//      one containing a SPACE (the quoting path) and one shaped like a GLOB.
//   2. GlobShapedArgumentArrivesLiteral — the `*.c` argument must arrive
//      UNEXPANDED. This is the `argvMode` pin, and it is a BEHAVIOUR claim rather
//      than a config one: MEASURED 2026-08-10, `_crt_argv_mode` 1 delivers `*.c`
//      literally while mode 2 expands it against the cwd, so a one-character
//      config edit silently changes what every pe program sees in argv. c111's
//      `_dowildcard = 0` was unexpanded, so mode 1 is the behaviour-preserving
//      value and this test is what makes that concrete.
//   3. EmittedImageImportsUcrtAccessorsAndNotMsvcrt — the emitted PE import table
//      names the UCRT accessor triple and NOTHING from the legacy CRT. Asserted on
//      NAMES PRESENT + NAMES ABSENT, never a count: an import COUNT goes inert the
//      moment the mechanism grows or loses a symbol.
//
// WINDOWS-ONLY, and the gate is honest about what it is: a pe64 image only RUNS on
// Windows, and the UCRT accessor mechanism only exists there. Every other leg
// compiles this file and skips the bodies; the mechanism's host-independent shape
// is pinned at the MIR tier so no leg is left with zero coverage.

#include "core/types/diagnostic_reporter.hpp"
#include "image_dependency_table.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using dss::Program;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

#if defined(_WIN32)

// The witness program. It ECHOES every argv element through `puts` and returns
// `argc`, so the TEST does the byte-exact comparison rather than the program — the
// program must not be allowed to decide whether it saw the right bytes.
constexpr char const* kEchoArgvSource =
    "#include <stdio.h>\n"
    "int main(int argc, char** argv) {\n"
    "    int i = 0;\n"
    "    while (i < argc) { puts(argv[i]); i = i + 1; }\n"
    "    return argc;\n"
    "}\n";

// Compile `source` for pe64 and return the emitted .exe path (empty on failure).
[[nodiscard]] fs::path buildPeExe(ScratchDir& scratch, char const* stem,
                                  char const* source) {
    auto const src = scratch.path() / (std::string{stem} + ".c");
    {
        std::ofstream out(src, std::ios::binary);
        out << source;
    }
    scratch.useAsCwd();
    Program prog;
    int const rc = prog.compileFiles({src.generic_string()}, "c-subset",
                                     {"x86_64:pe64-x86_64-windows-exec"});
    if (rc != 0) return {};
    return scratch.path() / "target" / "pe64-x86_64-windows-exec"
           / (std::string{stem} + ".exe");
}

[[nodiscard]] std::vector<std::uint8_t> readAllBytes(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>()};
}

// Split captured stdout into lines, normalizing the CRT's text-mode CRLF back to
// a bare LF. The translation is the CRT's, not the program's — `puts` writes one
// `\n` and the Windows FILE layer expands it — so normalizing here compares the
// bytes the PROGRAM produced instead of pinning a stdio implementation detail.
// (That the expansion happens at all is separately useful: it confirms the output
// really flowed through the CRT's FILE layer.)
[[nodiscard]] std::vector<std::string> splitLines(std::string const& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char const c : s) {
        if (c == '\r') continue;
        if (c == '\n') { out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// The three arguments. Chosen, not arbitrary:
//   "alpha"      — the plain case.
//   "two words"  — contains a SPACE, so it exercises the quoting path the CRT's
//                  command-line parser has to undo. A mechanism that split on
//                  whitespace would report argc 5 and four wrong strings.
//   "*.c"        — GLOB-shaped, so it pins `argvMode` (see the file header).
constexpr char const* kArg1 = "alpha";
constexpr char const* kArg2 = "two words";
constexpr char const* kArg3 = "*.c";

struct RunOutcome {
    bool                     built = false;
    dss::test_support::RunResult run;
    std::vector<std::string> lines;
};

[[nodiscard]] RunOutcome buildAndRunWithArgs(ScratchDir& scratch,
                                             char const* stem) {
    RunOutcome o;
    auto const exe = buildPeExe(scratch, stem, kEchoArgvSource);
    if (exe.empty() || !fs::exists(exe)) return o;
    o.built = true;
    o.run = dss::test_support::runBinary(
        exe, dss::test_support::kRunBudget, /*captureStdout=*/true,
        /*launcherPrefix=*/{},
        /*programArgs=*/{kArg1, kArg2, kArg3});
    o.lines = splitLines(o.run.capturedStdout);
    return o;
}

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

TEST(EntryArgvRun, RealCommandLineReachesMainByteExact) {
    ScratchDir scratch{Location::InsideRepo, "entry-argv"};
    auto const o = buildAndRunWithArgs(scratch, "echo_argv");
    ASSERT_TRUE(o.built) << "the pe64 witness must build";
    ASSERT_TRUE(o.run.spawned) << o.run.diagnostic;
    ASSERT_FALSE(o.run.timedOut) << o.run.diagnostic;

    // argc, via the program's exit status. FOUR: the image path plus three
    // arguments. Before UCRT-P4 the accessor mechanism did not exist; before c111
    // the pe entry read whatever the argument registers held (the c87-witnessed
    // argc=846361312 class), so this number is the whole mechanism in one integer.
    EXPECT_EQ(o.run.exitCode, 4u)
        << "argc must be 4 (image path + 3 arguments). stdout was:\n"
        << o.run.capturedStdout;

    ASSERT_EQ(o.lines.size(), 4u)
        << "the program must echo exactly argc lines; stdout was:\n"
        << o.run.capturedStdout;

    // argv[0] is the image path. NOT byte-pinned, and the reason is stated so the
    // omission does not read as laziness: WHICH spelling of the path lands in
    // argv[0] is the OS's and the launcher's choice (quoted vs unquoted, short vs
    // long), not the mechanism's. What the mechanism owns is that argv[0] is a real
    // non-empty string naming THIS image, and that is what is asserted.
    EXPECT_FALSE(o.lines[0].empty()) << "argv[0] must not be empty";
    EXPECT_NE(o.lines[0].find("echo_argv"), std::string::npos)
        << "argv[0] must name this image; got '" << o.lines[0] << "'";

    // argv[1..3] are BYTE-EXACT. These the mechanism owns completely.
    EXPECT_EQ(o.lines[1], kArg1);
    EXPECT_EQ(o.lines[2], kArg2)
        << "an argument containing a SPACE must arrive as ONE element with the "
           "space intact — a mechanism that split on whitespace would pass the "
           "argc check on a different command line but fail here";
    EXPECT_EQ(o.lines[3], kArg3);
}

TEST(EntryArgvRun, GlobShapedArgumentArrivesLiteral) {
    // The `argvMode` pin, run in a directory that CONTAINS matching files so the
    // expansion this forbids would actually have something to expand to. Without
    // the decoy files the test would pass under mode 2 as well — an expansion with
    // nothing to match leaves the pattern in place — which is exactly the
    // fail-closed hole this setup closes.
    ScratchDir scratch{Location::InsideRepo, "entry-argv-glob"};
    for (char const* decoy : {"decoy_one.c", "decoy_two.c"}) {
        std::ofstream out(scratch.path() / decoy, std::ios::binary);
        out << "int unused_" << 1 << "(void) { return 0; }\n";
    }
    auto const o = buildAndRunWithArgs(scratch, "glob_argv");
    ASSERT_TRUE(o.built) << "the pe64 witness must build";
    ASSERT_TRUE(o.run.spawned) << o.run.diagnostic;

    // Two decoys + the witness source itself are `.c` files in the cwd, so mode 2
    // would deliver at least three extra elements in place of the pattern.
    EXPECT_EQ(o.run.exitCode, 4u)
        << "argc must still be 4: `" << kArg3 << "` is ONE argument. A glob-"
           "expanding argv mode would report more. stdout was:\n"
        << o.run.capturedStdout;
    ASSERT_EQ(o.lines.size(), 4u) << o.run.capturedStdout;
    EXPECT_EQ(o.lines[3], kArg3)
        << "the glob-shaped argument must arrive LITERAL. MEASURED 2026-08-10: "
           "`_crt_argv_mode` 1 delivers it literally, mode 2 EXPANDS it — so this "
           "assertion is what stops a one-character config edit from silently "
           "changing what every pe program sees in argv";
}

TEST(EntryArgvRun, EmittedImageImportsUcrtAccessorsAndNotMsvcrt) {
    ScratchDir scratch{Location::InsideRepo, "entry-argv-imports"};
    auto const exe = buildPeExe(scratch, "imports_argv", kEchoArgvSource);
    ASSERT_FALSE(exe.empty()) << "the pe64 witness must build";
    ASSERT_TRUE(fs::exists(exe));

    auto const bytes = readAllBytes(exe);
    ASSERT_FALSE(bytes.empty());

    auto const libs = dss::test_support::peImportedLibraries(bytes);
    auto const syms = dss::test_support::peImportedSymbols(bytes);
    ASSERT_FALSE(libs.empty())
        << "the import-table extractor must find SOMETHING — an empty table here "
           "would make every assertion below vacuous";
    std::string const libText = dss::test_support::joinDependencies(libs);
    std::string const symText = dss::test_support::joinDependencies(syms);

    // PRESENT: the UCRT accessor triple, from the role-resolved image.
    EXPECT_EQ(dss::test_support::dependencyOccurrences(libs, "ucrtbase.dll"), 1u)
        << "libraries: " << libText;
    for (char const* want : {"_configure_narrow_argv", "__p___argc",
                             "__p___argv"}) {
        EXPECT_GE(dss::test_support::dependencyOccurrences(syms, want), 1u)
            << want << " must appear in the emitted import table; symbols: "
            << symText;
    }

    // ABSENT: the legacy CRT, and the msvcrt-only names it used to supply. This is
    // the MECHANICAL EXIT CRITERION of the pe CRT migration, asserted on the
    // EMITTED BYTES rather than on the config that produced them.
    EXPECT_EQ(dss::test_support::dependencyOccurrences(libs, "msvcrt.dll"), 0u)
        << "no pe image may import the legacy CRT; libraries: " << libText;
    for (char const* forbidden : {"__getmainargs", "__wgetmainargs"}) {
        EXPECT_EQ(dss::test_support::dependencyOccurrences(syms, forbidden), 0u)
            << forbidden << " is an msvcrt-ONLY export (MEASURED: ucrtbase "
               "exports NEITHER) and must be gone; symbols: " << symText;
    }
    // ABSENT: the WIDE twin. A narrow `main` must not drag in the wide configure
    // call — binding both would populate two argv states and leave which one the
    // program reads to luck.
    for (char const* forbidden : {"_configure_wide_argv", "__p___wargv"}) {
        EXPECT_EQ(dss::test_support::dependencyOccurrences(syms, forbidden), 0u)
            << forbidden << " must NOT be imported by a narrow `main` entry; "
               "symbols: " << symText;
    }
    // ABSENT: the `_o_`-prefixed downlevel forwarders. ucrtbase exports
    // `_o___p___argc` (695), `_o___p___argv` (696) and `_o__configure_narrow_argv`
    // (776) as OS-INTERNAL aliases; binding one would work today and is not a
    // supported interface. Named here so a future "the accessor is missing, try the
    // other spelling" fix reds instead of shipping.
    for (char const* forbidden : {"_o___p___argc", "_o___p___argv",
                                  "_o__configure_narrow_argv"}) {
        EXPECT_EQ(dss::test_support::dependencyOccurrences(syms, forbidden), 0u)
            << forbidden << " is an OS-internal downlevel forwarder, not a "
               "supported interface; symbols: " << symText;
    }
}

#else

TEST(EntryArgvRun, SkippedOffWindows) {
    GTEST_SKIP() << "a pe64 image only RUNS on Windows, and the UCRT argv-accessor "
                    "mechanism exists only there. The mechanism's host-independent "
                    "shape (the entry-shape gate, the narrow/wide arm selection, "
                    "the exact import set) is pinned on EVERY leg by "
                    "tests/mir/test_mir_merge.cpp's RealizeEntryShape suite, so no "
                    "leg is left with zero coverage of it.";
}

#endif  // _WIN32
