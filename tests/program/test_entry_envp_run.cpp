// D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: THE REAL-INPUT-PATH WITNESSES for the
// environment entry forms, on EVERY host.
//
// ★★ WHY THIS TIER. The subject is "does the PROCESS ENVIRONMENT reach
// `main(int, char**, char**)` intact", so its real input is the environment block a
// spawned process inherits. The examples runner can neither pass arguments nor set a
// variable, so `examples/c/entry_main_envp` can only check the vector's structure
// against getenv. This test owns the spawn, so it can do the two things that example
// cannot:
//   1. SET a variable the child must see — a value the TEST chose, byte-exact,
//      containing a space and a second '=' so neither the name/value split nor the
//      quoting is taken on trust;
//   2. pass REAL ARGUMENTS, so argc is not 1. The elf form computes envp as
//      argv + (argc + 1) slots from the RUNTIME argc; with argc fixed at 1 an init
//      that added a constant 2 would pass everything the example checks.
//
// ★ LIVE ON EVERY HOST, never skipped. It builds for the host's own exec format
// (`hostNativeTarget()`), which is a different mechanism on each: the elf entry
// stack, the pe UCRT environment accessors, Mach-O's dyld registers. The two
// platform-specific forms are asserted on every host too — RUN where the host's
// format realizes them, and REFUSED (K_ProgramEntryUndefined) where it does not —
// because "this form is not a program entry here" is behaviour, not an absence of
// coverage.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "diagnostic_count.hpp"
#include "host_native_target.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using dss::DiagnosticCode;
using dss::DiagnosticReporter;
using dss::Program;
using dss::test_support::hostNativeTarget;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;

namespace {

// The variable the child must see. A space and a SECOND '=' in the value, so a
// consumer that split on the last '=' or on whitespace reads a different entry.
constexpr char const* kWitnessName  = "DSS_ENVP_WITNESS";
constexpr char const* kWitnessValue = "alpha beta=gamma";

constexpr char const* kArg1 = "alpha";
constexpr char const* kArg2 = "two words";
constexpr char const* kArg3 = "*.c";

// Echo argv, a separator, then every envp entry, and return argc — so the TEST
// compares the bytes rather than the program deciding whether it saw the right ones.
constexpr char const* kEchoEnvpSource =
    "#include <stdio.h>\n"
    "int main(int argc, char **argv, char **envp) {\n"
    "    int i = 0;\n"
    "    while (i < argc) { puts(argv[i]); i = i + 1; }\n"
    "    puts(\"--\");\n"
    "    while (*envp) { puts(*envp); envp = envp + 1; }\n"
    "    return argc;\n"
    "}\n";

// Darwin's four-parameter form: echo every `apple` entry after the separator.
constexpr char const* kEchoAppleSource =
    "#include <stdio.h>\n"
    "int main(int argc, char **argv, char **envp, char **apple) {\n"
    "    (void)argv; (void)envp;\n"
    "    puts(\"--\");\n"
    "    while (*apple) { puts(*apple); apple = apple + 1; }\n"
    "    return argc;\n"
    "}\n";

// MSVC's three-parameter `wmain`: find the witness in the WIDE environment and
// compare it unit by unit, returning 42 on a byte-exact match and a distinct code
// for each way it can fail (the program cannot print a wide string portably).
constexpr char const* kWideWitnessSource =
    "static int matches(unsigned short const *w, char const *s) {\n"
    "    while (*s) { if (*w != (unsigned short)(unsigned char)*s) return 0;\n"
    "                 w = w + 1; s = s + 1; }\n"
    "    return 1;\n"
    "}\n"
    "int wmain(int argc, unsigned short **argv, unsigned short **envp) {\n"
    "    int found = 0;\n"
    "    if (argc != 4 || !argv[3]) return 90;\n"
    "    while (*envp) {\n"
    "        unsigned short const *e = *envp;\n"
    "        if (matches(e, \"DSS_ENVP_WITNESS=\")) {\n"
    "            unsigned short const *v = e + 17;\n"
    "            if (!matches(v, \"alpha beta=gamma\") || v[16] != 0) return 91;\n"
    "            found = found + 1;\n"
    "        }\n"
    "        envp = envp + 1;\n"
    "    }\n"
    "    return found == 1 ? 42 : 92;\n"
    "}\n";

struct Built {
    int      rc = -1;
    fs::path exe;
};

// Compile `source` for the host's exec format with a caller-owned reporter, so a
// refusal can be asserted BY CODE rather than by a bare non-zero exit.
[[nodiscard]] Built buildForHost(ScratchDir& scratch, char const* stem,
                                 char const* source, DiagnosticReporter& rep) {
    auto const src = scratch.path() / (std::string{stem} + ".c");
    {
        std::ofstream out(src, std::ios::binary);
        out << source;
    }
    scratch.useAsCwd();
    std::string_view const spec = hostNativeTarget().execTarget;
    std::string const format{spec.substr(spec.find(':') + 1)};
    Program prog;
    Built b;
    b.rc = prog.compileFiles({src.generic_string()}, "c", {std::string{spec}}, rep);
    b.exe = scratch.path() / "target" / format
          / dss::test_support::hostExeArtifact(stem);
    return b;
}

// Split captured stdout into lines, normalizing the CRT's text-mode CRLF to LF
// (the translation is the Windows FILE layer's, not the program's).
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

[[nodiscard]] bool hostRealizes(std::string_view formatFamilyPrefix) {
    return std::string_view{hostNativeTarget().execTarget}.find(formatFamilyPrefix)
        != std::string_view::npos;
}

}  // namespace

TEST(EntryEnvpRun, TheProcessEnvironmentReachesMainWithRealArguments) {
    ScopedEnv const witness{kWitnessName, kWitnessValue};
    ScratchDir scratch{Location::InsideRepo, "entry-envp"};
    DiagnosticReporter rep;
    auto const b = buildForHost(scratch, "echo_envp", kEchoEnvpSource, rep);
    ASSERT_EQ(b.rc, 0) << "the 3-parameter main must BUILD for "
                       << hostNativeTarget().execTarget;
    ASSERT_TRUE(fs::exists(b.exe)) << b.exe.string();

    auto const run = dss::test_support::runBinary(
        b.exe, dss::test_support::kRunBudget, /*captureStdout=*/true,
        /*launcherPrefix=*/{}, /*programArgs=*/{kArg1, kArg2, kArg3});
    ASSERT_TRUE(run.spawned) << run.diagnostic;
    ASSERT_FALSE(run.timedOut) << run.diagnostic;
    EXPECT_EQ(run.exitCode, 4u)
        << "argc must be 4 (image path + 3 arguments); stdout was:\n"
        << run.capturedStdout;

    auto const lines = splitLines(run.capturedStdout);
    ASSERT_GE(lines.size(), 5u) << run.capturedStdout;
    EXPECT_EQ(lines[1], kArg1);
    EXPECT_EQ(lines[2], kArg2);
    EXPECT_EQ(lines[3], kArg3);
    ASSERT_EQ(lines[4], "--")
        << "argv must end after argc entries — the separator is where argv[argc]'s "
           "NULL stopped the walk; stdout was:\n" << run.capturedStdout;

    // The environment: the variable THIS test set appears EXACTLY ONCE, byte-exact.
    // On elf the vector starts argc + 1 slots past argv, so with argc == 4 an init
    // that ignored the runtime argc would land inside argv and print the arguments
    // again here instead. (Each entry's NAME=VALUE structure is checked in-process by
    // examples/c/entry_main_envp; a line-based check here would misread a value that
    // itself contains a line end, which a CI host's environment may carry.)
    std::string const want = std::string{kWitnessName} + "=" + kWitnessValue;
    std::size_t hits = 0;
    for (std::size_t i = 5; i < lines.size(); ++i) {
        if (lines[i] == want) ++hits;
        EXPECT_NE(lines[i], kArg2)
            << "an argument reappeared after the separator: the environment "
               "vector was placed inside argv";
    }
    EXPECT_GT(lines.size(), 5u) << "the environment vector must not be empty";
    EXPECT_EQ(hits, 1u)
        << "the variable the test set (" << want << ") must reach envp exactly "
           "once; stdout was:\n" << run.capturedStdout;
}

TEST(EntryEnvpRun, DarwinsFourthVectorRunsOnMachoAndIsRefusedElsewhere) {
    ScratchDir scratch{Location::InsideRepo, "entry-envp-apple"};
    DiagnosticReporter rep;
    auto const b = buildForHost(scratch, "echo_apple", kEchoAppleSource, rep);
    if (!hostRealizes("macho64-")) {
        // No ELF or Windows loader hands an entry a fourth vector, so the host's
        // format does not realize `argc-argv-envp-apple`: the signature is legal
        // (the language declares the row) but no candidate survives.
        EXPECT_NE(b.rc, 0) << "the four-parameter main must be REFUSED on "
                           << hostNativeTarget().execTarget;
        EXPECT_EQ(dss::test_support::countCode(
                      rep, DiagnosticCode::K_ProgramEntryUndefined), 1u);
        EXPECT_FALSE(fs::exists(b.exe)) << "a refused build must write no image";
        return;
    }
    ASSERT_EQ(b.rc, 0);
    auto const run = dss::test_support::runBinary(
        b.exe, dss::test_support::kRunBudget, /*captureStdout=*/true);
    ASSERT_TRUE(run.spawned) << run.diagnostic;
    EXPECT_EQ(run.exitCode, 1u) << run.capturedStdout;
    auto const lines = splitLines(run.capturedStdout);
    ASSERT_GE(lines.size(), 2u) << run.capturedStdout;
    EXPECT_EQ(lines[0], "--");
    bool sawPath = false;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].rfind("executable_path=", 0) == 0) sawPath = true;
    }
    EXPECT_TRUE(sawPath)
        << "dyld's apple vector names the executable (`executable_path=`); a "
           "fourth register the trampoline did not carry would not. stdout:\n"
        << run.capturedStdout;
}

TEST(EntryEnvpRun, TheWideEnvironmentReachesWmainOnPeAndIsRefusedElsewhere) {
    ScopedEnv const witness{kWitnessName, kWitnessValue};
    ScratchDir scratch{Location::InsideRepo, "entry-wenvp"};
    DiagnosticReporter rep;
    auto const b = buildForHost(scratch, "wide_envp", kWideWitnessSource, rep);
    if (!hostRealizes("pe64-")) {
        // `wmain` is a program entry only where a format realizes a wide verb, and
        // no ELF or Mach-O format does.
        EXPECT_NE(b.rc, 0);
        EXPECT_EQ(dss::test_support::countCode(
                      rep, DiagnosticCode::K_ProgramEntryUndefined), 1u);
        return;
    }
    ASSERT_EQ(b.rc, 0) << "MSVC's 3-parameter wmain must BUILD on pe64";
    auto const run = dss::test_support::runBinary(
        b.exe, dss::test_support::kRunBudget, /*captureStdout=*/false,
        /*launcherPrefix=*/{}, /*programArgs=*/{kArg1, kArg2, kArg3});
    ASSERT_TRUE(run.spawned) << run.diagnostic;
    EXPECT_EQ(run.exitCode, 42u)
        << "90 = argc or argv wrong; 91 = the witness's WIDE value differs from what "
           "the test set; 92 = the witness is missing or appears twice";
}
