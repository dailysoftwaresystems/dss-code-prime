// The COMPILER'S BUILD STAMP — `src/program/dss_build_stamp.hpp` plus the
// generator behind it (`cmake/DssBuildStamp.cmake`).
//
// WHAT IS ACTUALLY AT RISK HERE. The stamp is a build-system product, and a
// build-system product fails in a shape unit tests usually never see: it is
// still THERE, still a string, still compiles — it has just gone empty, or
// picked up a trailing newline from a command's output, or lost the version
// component because a `-D` was dropped. None of those break the build. All of
// them break the runtime object cache's key, and they break it in the silent
// direction: an empty or constant stamp collapses every compiler identity onto
// ONE key, so a cache lookup starts serving artifacts compiled by a different
// compiler. That is why these pins are about the stamp's SHAPE rather than its
// value — the value legitimately differs on every machine, the shape may not.

#include "program/dss_build_stamp.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

// The repo-root `VERSION` file's contents, read from DISK at run time.
//
// ★ AN INDEPENDENT SOURCE OF TRUTH, deliberately. The generated header could
// just as easily have carried a second macro with the version in it for this
// test to compare against — and that comparison would pass even when the
// generator was handed the WRONG version, because it would only prove the
// header agrees with itself. Re-reading `VERSION` re-asks the question of the
// one file the top-level CMakeLists calls the single source of truth.
[[nodiscard]] std::string readRepoVersion(fs::path const& root) {
    std::ifstream in(root / "VERSION", std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string const text = buf.str();
    // The same trim CMake's `string(STRIP ...)` applies at configure time: the
    // file ends with a newline and the stamp must not.
    std::size_t const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    std::size_t const last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

} // namespace

TEST(BuildStamp, IsANonEmptyWhitespaceFreeToken) {
    std::string_view const stamp = dss::runtime::kBuildStamp;
    ASSERT_FALSE(stamp.empty())
        << "an EMPTY stamp is the worst available value: it compiles, it is a "
           "valid string, and it collapses every compiler identity onto one "
           "cache key";
    EXPECT_EQ(stamp, std::string_view{DSS_BUILD_STAMP})
        << "the exposed constant and the macro must be the same bytes — a "
           "second spelling that could drift is not an accessor";

    // The stamp is hashed into, and named by, a cache PATH. A stray space or a
    // trailing `\n` picked up from a command's output therefore does not
    // produce a wrong string, it produces a path that cannot be looked up (or
    // one that splits at a shell boundary), and it does so only on the machine
    // whose git happened to emit it. Every byte must be printable and non-space.
    for (std::size_t i = 0; i < stamp.size(); ++i) {
        auto const c = static_cast<unsigned char>(stamp[i]);
        EXPECT_EQ(std::isspace(c), 0)
            << "whitespace at index " << i << " of stamp '" << stamp << "'";
        EXPECT_TRUE(c >= 0x21 && c <= 0x7E)
            << "byte " << unsigned{c} << " at index " << i
            << " is outside printable non-space ASCII, in stamp '" << stamp << "'";
    }
}

TEST(BuildStamp, LeadsWithTheRepoVersion) {
    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    std::string const version = readRepoVersion(*root);
    // Prove the reference was actually read before believing the compare — a
    // guard whose own input is empty passes vacuously against anything.
    ASSERT_FALSE(version.empty())
        << "the repo-root VERSION file at '" << (*root / "VERSION").string()
        << "' is empty or unreadable";

    std::string_view const stamp = dss::runtime::kBuildStamp;
    EXPECT_NE(stamp.find(version), std::string_view::npos)
        << "stamp '" << stamp << "' does not contain VERSION '" << version
        << "' — the generator was handed the wrong value or none at all";
    EXPECT_EQ(stamp.rfind(version, 0), 0u)
        << "...and it must LEAD with it. The version is the stamp's first "
           "component by construction, so a match found anywhere else is a "
           "coincidence inside a commit id or a digest, which would let a "
           "version change go unnoticed the moment the coincidence ends";
}
