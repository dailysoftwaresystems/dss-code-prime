// InputResolver tests — plan 14 LK10 cycle 3 (D-LK10-1 closure).
//
// Pins:
//   * `resolveDirectory` filters by extension, sorts, dedupes.
//   * Recursive vs Flat mode is the policy axis (D-LK10-1 trigger).
//   * Missing directory fires `D_FileNotFound`.
//   * Empty match-set fires `D_EmptyInput`.

#include "core/types/parse_diagnostic.hpp"
#include "program/input_resolver.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;

// In-repo scratch dir (test artifacts; auto-removed by dtor). This file used to
// carry its OWN copy of `ScratchDir` — that duplicate is what
// D-TEST-FIXED-SCRATCH-PATH-POPULATION calls out here, and it is now deleted in
// favour of the shared `tests/test_support/scratch_dir.hpp`.
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
