// InputResolver tests — plan 14 LK10 cycle 3 (D-LK10-1 closure).
//
// Pins:
//   * `resolveDirectory` filters by extension, sorts, dedupes.
//   * Recursive vs Flat mode is the policy axis (D-LK10-1 trigger).
//   * Missing directory fires `D_FileNotFound`.
//   * Empty match-set fires `D_EmptyInput`.

#include "core/types/parse_diagnostic.hpp"
#include "program/input_resolver.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;

namespace {

// In-repo scratch dir (test artifacts; auto-removed by dtor).
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<std::uint64_t> counter{0};
#ifdef _WIN32
        auto const pid = static_cast<std::uint64_t>(_getpid());
#else
        auto const pid = static_cast<std::uint64_t>(getpid());
#endif
        std::error_code ec;
        originalCwd_ = fs::current_path(ec);
        auto const base = originalCwd_ / "test-scratch" / "input-resolver";
        fs::create_directories(base, ec);
        path_ = base / (std::to_string(pid) + "-"
                        + std::to_string(counter.fetch_add(1)));
        fs::create_directories(path_, ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::current_path(originalCwd_, ec);
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] fs::path const& path() const noexcept { return path_; }
private:
    fs::path path_;
    fs::path originalCwd_;
};

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
    ScratchDir scratch;
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
    ScratchDir scratch;
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
    ScratchDir scratch;
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
    ScratchDir scratch;
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
    ScratchDir scratch;
    auto const ghost = scratch.path() / "does-not-exist";
    std::vector<std::string> exts{".c"};
    std::vector<std::string> out;
    DiagnosticReporter rep;
    EXPECT_FALSE(InputResolver::resolveDirectory(
        ghost, exts, InputResolver::Mode::Recursive, out, rep));
    EXPECT_GT(countCode(rep, DiagnosticCode::D_FileNotFound), 0u);
}

TEST(InputResolver, EmptyMatchSetFiresEmptyInput) {
    ScratchDir scratch;
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
    ScratchDir scratch;
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
    ScratchDir scratch;
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
