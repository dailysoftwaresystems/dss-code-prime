// Direct unit tests for the hoisted `tests/test_support/scratch_dir.hpp`
// substrate (D-LK10-6 closure + post-fold #1 guards).
//
// Pins:
//   * Ctor throws on filesystem error (post-fold #1 silent-failure
//     CRITICAL — prior to the fold, ec was silently swallowed).
//   * `useAsCwd()` rejects `Location::Temp` (post-fold #1 architect
//     Q6 — Temp puts the scratch outside the repo tree, breaking
//     the schema-loader cwd-walk).
//   * `useAsCwd()` succeeds with `Location::InsideRepo`.
//   * Dtor restores `originalCwd_` before remove_all.
//   * Group subdir lets parallel test binaries coexist.

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

TEST(ScratchDirSubstrate, TempBasePathExistsAfterCtor) {
    ScratchDir sd{Location::Temp, "scratch-dir-self-test"};
    EXPECT_TRUE(fs::is_directory(sd.path()));
}

TEST(ScratchDirSubstrate, InsideRepoBasePathExistsAfterCtor) {
    ScratchDir sd{Location::InsideRepo, "scratch-dir-self-test"};
    EXPECT_TRUE(fs::is_directory(sd.path()));
}

// post-fold #1 (architect Q6): useAsCwd MUST reject Location::Temp —
// the schema-loader walks UP from cwd; temp paths break the walk.
TEST(ScratchDirSubstrate, UseAsCwdRejectsLocationTemp) {
    ScratchDir sd{Location::Temp, "scratch-dir-self-test"};
    EXPECT_THROW(sd.useAsCwd(), std::runtime_error);
}

// Happy path for the InsideRepo arm.
TEST(ScratchDirSubstrate, UseAsCwdAcceptsLocationInsideRepo) {
    auto const cwdBefore = fs::current_path();
    {
        ScratchDir sd{Location::InsideRepo, "scratch-dir-self-test"};
        EXPECT_NO_THROW(sd.useAsCwd());
        EXPECT_EQ(fs::current_path(), sd.path());
    }
    // Dtor restored cwd.
    EXPECT_EQ(fs::current_path(), cwdBefore);
}

// Group subdir keeps distinct test groups from sharing the unique-
// path slot. Two ScratchDirs in the same group still get distinct
// paths via the atomic counter.
TEST(ScratchDirSubstrate, MultipleScratchDirsGetDistinctPaths) {
    ScratchDir a{Location::Temp, "scratch-dir-self-test"};
    ScratchDir b{Location::Temp, "scratch-dir-self-test"};
    EXPECT_NE(a.path(), b.path());
}

TEST(ScratchDirSubstrate, DifferentGroupsLandUnderDifferentSubdirs) {
    ScratchDir a{Location::Temp, "scratch-dir-group-alpha"};
    ScratchDir b{Location::Temp, "scratch-dir-group-beta"};
    EXPECT_NE(a.path().parent_path(), b.path().parent_path());
}

// originalCwd accessor pins the captured cwd for tests that need
// to construct paths relative to the pre-cwd-change directory.
TEST(ScratchDirSubstrate, OriginalCwdMatchesCurrentPathAtCtor) {
    auto const cwdAtCtor = fs::current_path();
    ScratchDir sd{Location::InsideRepo, "scratch-dir-self-test"};
    EXPECT_EQ(sd.originalCwd(), cwdAtCtor);
}
