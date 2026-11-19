// P69 fold 3f: THE BODIES DSS SHIPS FOR A PLATFORM NAME, RUN AGAINST THE MEANING THE REFERENCES MEASURED.
//
// A pe name the platform image does not export under its own spelling is answered by a shipped body. What
// that body does is not DSS's to invent: it is the meaning the reference toolchains give the name, measured
// (probe-reference-cc, 2026-11-17, windows-x86_64-release cl 19.52 and windows-x86_64-debug mingw-w64 gcc
// 13.2). Each case builds a program for the HOST's own exec format and RUNS it, so it is live on every host:
// on pe it reaches DSS's bodies, elsewhere the platform C library's own functions, and the lines both print
// are the same except where a line is pe's alone.
//
//   * `swab`. ucrtbase exports `_swab` and no `swab`; cl reaches it through OLDNAMES and mingw-w64 through its
//     import library's alias, so a written `swab` IS `_swab` under both. Six bytes swap three pairs, an odd
//     count leaves its last byte unwritten, one and zero write nothing -- glibc and libSystem alike -- and on
//     pe a NEGATIVE count reaches the invalid-parameter handler and sets EINVAL (with no handler installed,
//     cl's image dies 0xC0000409). DSS's pe body forwards to `_swab`, so every count is the platform's.
//   * `access`. cl's `access` is OLDNAMES' alias of `_access`, which refuses mode X_OK through the
//     invalid-parameter handler (0xC0000409 with none installed); mingw-w64 binds its own `access`, which
//     answers it. One working reference makes the meaning required: on pe X_OK on an existing directory is
//     answered 0, as POSIX hosts answer it for a searchable one.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "host_native_target.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using dss::DiagnosticReporter;
using dss::Program;
using dss::test_support::hostNativeTarget;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// The platform's swap at an even, an odd, a one and a zero count -- and, on pe, at a negative one with a
// handler installed that returns, so the program goes on and prints errno. The destination is pre-filled
// with '.' so a byte the call leaves alone shows.
constexpr char const* kSwabSource =
    "#include <stdio.h>\n"
    "#include <string.h>\n"
    "#include <errno.h>\n"
    "#include <unistd.h>\n"
    "#ifdef _WIN32\n"
    "#include <stdlib.h>\n"
    "#include <stdint.h>\n"
    "#include <stddef.h>\n"
    "static void quiet(const wchar_t *e, const wchar_t *f, const wchar_t *fi, unsigned l, uintptr_t r) {\n"
    "    (void)e; (void)f; (void)fi; (void)l; (void)r;\n"
    "}\n"
    "#endif\n"
    "static void show(const char *tag, int n) {\n"
    "    char from[8] = \"ABCDEFG\";\n"
    "    char to[8];\n"
    "    memset(to, '.', 7);\n"
    "    to[7] = 0;\n"
    "    errno = 0;\n"
    "    swab(from, to, n);\n"
    "    printf(\"%s %s %d\\n\", tag, to, errno);\n"
    "}\n"
    "int main(void) {\n"
    "#ifdef _WIN32\n"
    "    _set_invalid_parameter_handler(quiet);\n"
    "#endif\n"
    "    show(\"even\", 6);\n"
    "    show(\"odd\", 5);\n"
    "    show(\"one\", 1);\n"
    "    show(\"zero\", 0);\n"
    "#ifdef _WIN32\n"
    "    show(\"neg\", -2);\n"
    "#endif\n"
    "    return 42;\n"
    "}\n";

// `access` on a file the program made (F_OK, R_OK), X_OK on the working directory, and a missing path.
constexpr char const* kAccessSource =
    "#include <stdio.h>\n"
    "#include <errno.h>\n"
    "#include <unistd.h>\n"
    "int main(void) {\n"
    "    FILE *f = fopen(\"dss_access_probe.txt\", \"w\");\n"
    "    int r;\n"
    "    if (!f) return 90;\n"
    "    fputs(\"x\", f);\n"
    "    fclose(f);\n"
    "    errno = 0; r = access(\"dss_access_probe.txt\", F_OK); printf(\"F_OK file %d %d\\n\", r, errno);\n"
    "    errno = 0; r = access(\"dss_access_probe.txt\", R_OK); printf(\"R_OK file %d %d\\n\", r, errno);\n"
    "    errno = 0; r = access(\".\", X_OK); printf(\"X_OK dir %d %d\\n\", r, errno);\n"
    "    errno = 0; r = access(\"dss_no_such_file.txt\", F_OK); printf(\"missing %d %d\\n\", r, errno);\n"
    "    remove(\"dss_access_probe.txt\");\n"
    "    return 42;\n"
    "}\n";

[[nodiscard]] bool hostIsPe() {
    return std::string_view{hostNativeTarget().execTarget}.find(":pe") != std::string_view::npos;
}

// Every diagnostic of a build, one per line, for a failure's message.
[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all())
        s += "\n  " + std::string{dss::diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

// Split captured stdout into lines, dropping the CRT's text-mode CR (the Windows FILE layer's, not the
// program's).
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

// Build `source` for the host's exec format in `scratch` (its working directory too), run it, and return
// its lines; every refusal and every way the run can end otherwise fails the case by name.
[[nodiscard]] std::vector<std::string> buildAndRun(ScratchDir& scratch, char const* stem,
                                                   char const* source) {
    auto const src = scratch.path() / (std::string{stem} + ".c");
    {
        std::ofstream out(src, std::ios::binary);
        out << source;
    }
    scratch.useAsCwd();
    std::string_view const spec = hostNativeTarget().execTarget;
    std::string const format{spec.substr(spec.find(':') + 1)};
    DiagnosticReporter rep;
    Program prog;
    int const rc = prog.compileFiles({src.generic_string()}, "c", {std::string{spec}}, rep);
    EXPECT_EQ(rc, 0) << "the program must BUILD for " << spec << diagnosticsOf(rep);
    fs::path const exe = scratch.path() / "target" / format / dss::test_support::hostExeArtifact(stem);
    if (rc != 0 || !fs::exists(exe)) return {};
    auto const run = dss::test_support::runBinary(exe, dss::test_support::kRunBudget,
                                                  /*captureStdout=*/true);
    EXPECT_TRUE(run.spawned) << run.diagnostic;
    EXPECT_FALSE(run.timedOut) << run.diagnostic;
    EXPECT_EQ(run.exitCode, 42u)
        << "the program must run to its end (42): a platform body refused a call the reference answers "
           "(0xC0000409 = 3221226505 is the invalid-parameter fast fail); stdout was:\n"
        << run.capturedStdout;
    return splitLines(run.capturedStdout);
}

}  // namespace

TEST(PlatformBodiesRun, SwabIsThePlatformsOwnAtEveryCount) {
    ScratchDir scratch{Location::InsideRepo, "platform-bodies-swab"};
    auto const lines = buildAndRun(scratch, "swab_counts", kSwabSource);
    std::vector<std::string> want{"even BADCFE. 0", "odd BADC... 0", "one ....... 0", "zero ....... 0"};
    if (hostIsPe()) want.emplace_back("neg ....... 22");
    EXPECT_EQ(lines, want)
        << "swab must be the platform's own at every count; on pe a negative count must reach the "
           "invalid-parameter handler and set EINVAL (22), as UCRT's _swab does under cl and mingw-w64";
}

TEST(PlatformBodiesRun, AccessAnswersExecuteOnADirectoryAsTheWorkingReferenceDoes) {
    ScratchDir scratch{Location::InsideRepo, "platform-bodies-access"};
    auto const lines = buildAndRun(scratch, "access_modes", kAccessSource);
    std::vector<std::string> const want{"F_OK file 0 0", "R_OK file 0 0", "X_OK dir 0 0", "missing -1 2"};
    EXPECT_EQ(lines, want)
        << "access must answer X_OK on an existing directory (0), as POSIX hosts and mingw-w64 do, and "
           "every other mode as before";
}
