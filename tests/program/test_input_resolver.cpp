// InputResolver tests — plan 14 LK10 cycle 3 (D-LK10-1 closure).
//
// Pins:
//   * `resolveDirectory` filters by extension, sorts, dedupes.
//   * Recursive vs Flat mode is the policy axis (D-LK10-1 trigger).
//   * Missing directory fires `D_FileNotFound`.
//   * Empty match-set fires `D_EmptyInput`.

#include "core/types/parse_diagnostic.hpp"
#include "program/input_resolver.hpp"
#include "os_link.hpp"   // links made by the OS: MinGW's library makes none
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>   // mkfifo: the special file a POSIX host can make
#endif

namespace fs = std::filesystem;
using namespace dss;

// In-repo scratch dir (test artifacts; auto-removed by dtor). This file used to
// carry its OWN copy of `ScratchDir`, populating a fixed scratch path; that
// duplicate is now deleted in favour of the shared
// `tests/test_support/scratch_dir.hpp`.
//
// The copy was not a live collision: it already seeded the path with the PID.
// It was weaker in the CLAIM. It took the slot with PLURAL
// `create_directories`, which reports SUCCESS when the directory already
// exists, so it could silently share a stale directory left behind by a killed
// run that drew the same PID (PIDs recycle). The shared type claims with
// SINGULAR `create_directory` in a loop — true only for the caller that
// actually created the directory, atomic at the OS level — so it is race-free
// AND it steps over stale slots. `Location::InsideRepo, "input-resolver"`
// reproduces the exact base this file used, `<cwd>/test-scratch/input-resolver`.
//
// MEASURED: two concurrent processes of this binary, `--gtest_repeat=12
// --gtest_shuffle`, 6 rounds — green BOTH before and after, which is the
// expected result for the one site here that was already PID-seeded. It served
// as the negative control for the three counter-only fixtures that did fail.
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

void writeFile(fs::path const& p, std::string_view content = "x") {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p);
    f << content;
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) if (d.code == code) ++n;
    return n;
}

[[nodiscard]] bool sawTextContaining(DiagnosticReporter const& rep,
                                     std::string_view needle) {
    for (auto const& d : rep.all()) {
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

[[nodiscard]] std::size_t countSeverity(DiagnosticReporter const& rep,
                                        DiagnosticSeverity sev) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) if (d.severity == sev) ++n;
    return n;
}

} // namespace

// ── resolveDirectory ────────────────────────────────────────

TEST(InputResolver, RecursiveScanPicksUpAllMatchingFiles) {
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    writeFile(scratch.path() / "a.c");
    writeFile(scratch.path() / "sub" / "b.c");
    writeFile(scratch.path() / "sub" / "deep" / "c.c");
    writeFile(scratch.path() / "ignored.txt");

    std::vector<std::string> exts{".c"};
    std::vector<std::string> out;
    DiagnosticReporter rep;
    bool const ok = InputResolver::resolveDirectory(
        scratch.path(), exts, InputResolver::Mode::Recursive, out, rep);
    EXPECT_TRUE(ok);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(out.size(), 3u);
}

// A LINK IS AN INPUT BY WHAT IT REACHES, AND A DIRECTORY LINK IS NEVER WALKED
// THROUGH, on every build. ✔MEASURED 2026-10-20 (src/core/substrate/path_identity.hpp):
// libstdc++ on Windows cannot tell a link from what it reaches, so the MinGW
// build's recursive scan DESCENDED a directory link the MSVC build's did not (one
// directory, two input lists) and took a dangling `.c` link for a source. The
// links are made by the OS (`os_link.hpp`): that build's library makes none.
TEST(InputResolver, RecursiveScanNeverDescendsALinkAndTakesALinkByWhatItReaches) {
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    ScratchDir elsewhere{Location::Temp, "input-resolver-elsewhere"};
    writeFile(scratch.path() / "a.c");
    writeFile(elsewhere.path() / "beyond.c");
    writeFile(elsewhere.path() / "named.c");
    using dss::test_support::LinkTo;
    DSS_ASSERT_LINK_MADE(dss::test_support::makeSymbolicLink(elsewhere.path(), scratch.path() / "linked-dir",
                                                             LinkTo::Directory));
    DSS_ASSERT_LINK_MADE(dss::test_support::makeSymbolicLink(elsewhere.path() / "named.c", scratch.path() / "via-link.c",
                                                             LinkTo::File));
    DSS_ASSERT_LINK_MADE(dss::test_support::makeSymbolicLink(scratch.path() / "gone.c", scratch.path() / "dangling.c",
                                                             LinkTo::File));

    std::vector<std::string> exts{".c"};
    std::vector<std::string> out;
    DiagnosticReporter rep;
    bool const ok = InputResolver::resolveDirectory(
        scratch.path(), exts, InputResolver::Mode::Recursive, out, rep);
    EXPECT_TRUE(ok);
    EXPECT_EQ(rep.errorCount(), 0u);
    std::vector<std::string> names;
    std::string              took;
    for (auto const& s : out) names.push_back(fs::path{s}.lexically_relative(scratch.path()).generic_string());
    std::sort(names.begin(), names.end());
    for (auto const& n : names) took += " " + n;
    EXPECT_EQ(names, (std::vector<std::string>{"a.c", "via-link.c"}))
        << "the scan took " << names.size() << " inputs:" << took
        << " -- a directory link was walked through, or a dangling link was taken for a source";
}

// A SPECIAL FILE IS NO INPUT, whatever it is named: the scan takes an entry that reaches a regular file and
// nothing else, as it did when it asked the library for `regular`. POSIX only: Windows makes no FIFO in its
// filesystem.
TEST(InputResolver, AFifoNamedLikeASourceIsNoInput) {
#if defined(_WIN32)
    GTEST_SKIP() << "Windows makes no FIFO in its filesystem; the POSIX legs read it";
#else
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    writeFile(scratch.path() / "a.c");
    ASSERT_EQ(::mkfifo((scratch.path() / "pipe.c").c_str(), 0600), 0) << "this host made no FIFO";

    std::vector<std::string> exts{".c"};
    std::vector<std::string> out;
    DiagnosticReporter rep;
    bool const ok = InputResolver::resolveDirectory(
        scratch.path(), exts, InputResolver::Mode::Recursive, out, rep);
    EXPECT_TRUE(ok);
    EXPECT_EQ(rep.errorCount(), 0u);
    std::vector<std::string> names;
    std::string              took;
    for (auto const& s : out) names.push_back(fs::path{s}.lexically_relative(scratch.path()).generic_string());
    std::sort(names.begin(), names.end());
    for (auto const& n : names) took += " " + n;
    EXPECT_EQ(names, (std::vector<std::string>{"a.c"}))
        << "the scan took " << names.size() << " inputs:" << took << " -- a FIFO was taken for a source";
#endif
}

TEST(InputResolver, FlatScanIgnoresSubdirectories) {
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    writeFile(scratch.path() / "a.c");
    writeFile(scratch.path() / "sub" / "b.c");
    writeFile(scratch.path() / "sub" / "deep" / "c.c");

    std::vector<std::string> exts{".c"};
    std::vector<std::string> out;
    DiagnosticReporter rep;
    bool const ok = InputResolver::resolveDirectory(
        scratch.path(), exts, InputResolver::Mode::Flat, out, rep);
    EXPECT_TRUE(ok);
    EXPECT_EQ(out.size(), 1u);
    EXPECT_NE(out[0].find("a.c"), std::string::npos);
}

TEST(InputResolver, ExtensionFilterRejectsNonMatches) {
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    writeFile(scratch.path() / "src.c");
    writeFile(scratch.path() / "src.h");
    writeFile(scratch.path() / "src.cpp");
    writeFile(scratch.path() / "src.txt");

    std::vector<std::string> exts{".c", ".h"};
    std::vector<std::string> out;
    DiagnosticReporter rep;
    bool const ok = InputResolver::resolveDirectory(
        scratch.path(), exts, InputResolver::Mode::Flat, out, rep);
    EXPECT_TRUE(ok);
    EXPECT_EQ(out.size(), 2u);
}

TEST(InputResolver, OutputSortedForDeterminism) {
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    writeFile(scratch.path() / "z.c");
    writeFile(scratch.path() / "m.c");
    writeFile(scratch.path() / "a.c");
    writeFile(scratch.path() / "q.c");

    std::vector<std::string> exts{".c"};
    std::vector<std::string> out;
    DiagnosticReporter rep;
    EXPECT_TRUE(InputResolver::resolveDirectory(
        scratch.path(), exts, InputResolver::Mode::Flat, out, rep));
    ASSERT_EQ(out.size(), 4u);
    for (std::size_t i = 1; i < out.size(); ++i) {
        EXPECT_LT(out[i - 1], out[i])
            << "InputResolver output must be sorted ascending";
    }
}

TEST(InputResolver, MissingDirectoryFiresFileNotFound) {
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    auto const ghost = scratch.path() / "does-not-exist";
    std::vector<std::string> exts{".c"};
    std::vector<std::string> out;
    DiagnosticReporter rep;
    EXPECT_FALSE(InputResolver::resolveDirectory(
        ghost, exts, InputResolver::Mode::Recursive, out, rep));
    EXPECT_GT(countCode(rep, DiagnosticCode::D_FileNotFound), 0u);
}

TEST(InputResolver, EmptyMatchSetFiresEmptyInput) {
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    writeFile(scratch.path() / "ignored.txt");
    std::vector<std::string> exts{".c"};
    std::vector<std::string> out;
    DiagnosticReporter rep;
    EXPECT_FALSE(InputResolver::resolveDirectory(
        scratch.path(), exts, InputResolver::Mode::Flat, out, rep));
    EXPECT_GT(countCode(rep, DiagnosticCode::D_EmptyInput), 0u);
}

// ── validateFiles ────────────────────────────────────────────

TEST(InputResolver, ValidateFilesAcceptsExistingFiles) {
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    auto const a = scratch.path() / "a.c";
    auto const b = scratch.path() / "b.c";
    writeFile(a);
    writeFile(b);

    std::vector<std::string> inputs{a.generic_string(), b.generic_string()};
    std::vector<std::string> out;
    DiagnosticReporter rep;
    EXPECT_TRUE(InputResolver::validateFiles(inputs, out, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(out.size(), 2u);
}

TEST(InputResolver, ValidateFilesRejectsMissing) {
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    auto const real = scratch.path() / "real.c";
    auto const ghost = scratch.path() / "ghost.c";
    writeFile(real);

    std::vector<std::string> inputs{real.generic_string(),
                                     ghost.generic_string()};
    std::vector<std::string> out;
    DiagnosticReporter rep;
    EXPECT_FALSE(InputResolver::validateFiles(inputs, out, rep));
    EXPECT_GT(countCode(rep, DiagnosticCode::D_FileNotFound), 0u);
    EXPECT_EQ(out.size(), 1u);  // only the real file made it through
}

// ── checkSearchDirectoriesUsable ─────────────────────────────
//
// D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED, the ACCEPTANCE half. The row
// asks that a `-I` directory which cannot be enumerated be named AT THE POINT
// IT IS ACCEPTED, so the user is not left with only a missing-header error
// pointing at the `#include` while the fault is in the command line.

TEST(InputResolver, UsableSearchDirectoryIsSilent) {
    // ★ THE CONTROL. Without it the four cases below would all pass against a
    // function that warned unconditionally, which is the failure mode a
    // diagnostic test is most likely to have.
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    writeFile(scratch.path() / "h.h");
    std::vector<std::string> dirs{scratch.path().generic_string()};
    DiagnosticReporter rep;
    EXPECT_TRUE(InputResolver::checkSearchDirectoriesUsable(dirs, "-I", rep));
    EXPECT_EQ(rep.all().size(), 0u) << "an enumerable directory must say nothing";
}

TEST(InputResolver, AbsentSearchDirectoryIsNamedAtAcceptance) {
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    auto const ghost = scratch.path() / "no-such-include-dir";
    std::vector<std::string> dirs{ghost.generic_string()};
    DiagnosticReporter rep;
    EXPECT_FALSE(InputResolver::checkSearchDirectoriesUsable(dirs, "-I", rep));
    EXPECT_GT(countCode(rep, DiagnosticCode::D_FileNotFound), 0u);
    // NAMING the directory is the requirement, not merely reporting one.
    EXPECT_TRUE(sawTextContaining(rep, ghost.generic_string()));
    EXPECT_TRUE(sawTextContaining(rep, "-I"));
}

TEST(InputResolver, SearchDirectoryThatIsAFileIsNamedAtAcceptance) {
    // gcc's own case: `cc1: warning: <path>: not a directory`.
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    auto const notADir = scratch.path() / "regular.txt";
    writeFile(notADir);
    std::vector<std::string> dirs{notADir.generic_string()};
    DiagnosticReporter rep;
    EXPECT_FALSE(InputResolver::checkSearchDirectoriesUsable(dirs, "-I", rep));
    EXPECT_GT(countCode(rep, DiagnosticCode::D_DirectoryScanFailed), 0u);
    EXPECT_TRUE(sawTextContaining(rep, notADir.generic_string()));
}

TEST(InputResolver, EmptySearchDirectoryArgumentIsNamedAsEmpty) {
    // Reported as EMPTY rather than as an absent directory whose name is the
    // empty string, which would print a message with a hole in it.
    std::vector<std::string> dirs{std::string{}};
    DiagnosticReporter rep;
    EXPECT_FALSE(InputResolver::checkSearchDirectoriesUsable(dirs, "-I", rep));
    EXPECT_TRUE(sawTextContaining(rep, "EMPTY"));
}

TEST(InputResolver, UnusableSearchDirectoryWARNSAndNeverRefuses) {
    // ★★★ THE UNION PIN, AND THE REASON THIS TEST EXISTS AT ALL.
    // ✔MEASURED 2026-08-28: gcc 13.2.0 accepts `-I <absent>` SILENTLY (rc=0,
    // and `-v` shows the directory is not even listed in the search list),
    // still silent under `-Wall -Wextra -Werror`; it accepts `-I <a regular
    // file>` with a WARNING and rc=0. MSVC 14.44.35207 accepts all of them
    // silently, rc=0, including at `/W4 /WX`. NO reference refuses any of
    // them, so an ERROR here would put DSS ABOVE `(gcc ∪ clang ∪ MSVC) ∪
    // ISO C` — the union binds in BOTH directions. Every diagnostic this
    // function emits must therefore be a WARNING; a user who wants it fatal
    // has `--warnings-as-errors`.
    //
    // Without this case the severity is free to drift to Error on the next
    // edit and every other test here would stay green.
    ScratchDir scratch{Location::InsideRepo, "input-resolver"};
    auto const notADir = scratch.path() / "regular.txt";
    writeFile(notADir);
    std::vector<std::string> dirs{
        (scratch.path() / "absent-dir").generic_string(),
        notADir.generic_string(),
        std::string{}};
    DiagnosticReporter rep;
    EXPECT_FALSE(InputResolver::checkSearchDirectoriesUsable(dirs, "-I", rep));
    EXPECT_EQ(rep.errorCount(), 0u)
        << "an unusable -I must not REFUSE the compile: every reference "
           "toolchain accepts it, and refusing is DSS above the union";
    EXPECT_EQ(countSeverity(rep, DiagnosticSeverity::Warning), 3u)
        << "one warning per unusable directory, and nothing silent";
}
