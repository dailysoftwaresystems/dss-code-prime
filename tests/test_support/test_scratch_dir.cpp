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
#include <fstream>
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

// TF-C58 (`D-TEST-EXAMPLES-RUNNER-PARALLEL-CONTENTION-FLAKE`) red-on-disable.
// A STALE directory sitting on the slot the ctor is about to draw must be
// STEPPED OVER, never reused. `MultipleScratchDirsGetDistinctPaths` above does
// NOT catch this — the counter advances anyway, so it stayed green through the
// entire lifetime of the bug. The real failure mode is a directory left behind
// by a killed run (or a recycled PID), which the old `create_directories` call
// reported as success: the new ScratchDir then SHARED it and the first
// `copy_file` died with "File exists", surfacing as a non-deterministic ctest
// red on a different example each run.
//
// The slot is predicted from a probe ScratchDir's own filename, so this pins
// real ctor behaviour rather than a re-derived path formula.
// RED-ON-DISABLE: restore `create_directories(path_, ec)` — the ctor then
// hands back the pre-created stale path and BOTH expectations fail.
TEST(ScratchDirSubstrate, StaleDirectoryOnTheNextSlotIsNotReused) {
    fs::path base;
    std::uint64_t nextIdx = 0;
    std::string   pidPart;
    {
        ScratchDir probe{Location::Temp, "scratch-dir-stale-slot"};
        base = probe.path().parent_path();
        auto const name = probe.path().filename().string();
        auto const dash = name.rfind('-');
        ASSERT_NE(dash, std::string::npos) << "unexpected slot name: " << name;
        pidPart = name.substr(0, dash);
        nextIdx = std::stoull(name.substr(dash + 1)) + 1;
    }

    // Plant a stale directory (with content, like a killed run would leave)
    // exactly where the next ScratchDir would land.
    fs::path const stale = base / (pidPart + "-" + std::to_string(nextIdx));
    std::error_code ec;
    fs::create_directories(stale, ec);
    ASSERT_FALSE(ec) << "could not plant stale dir: " << ec.message();
    { std::ofstream marker{stale / "main.c"}; marker << "stale\n"; }
    ASSERT_TRUE(fs::exists(stale / "main.c"));

    {
        ScratchDir fresh{Location::Temp, "scratch-dir-stale-slot"};
        EXPECT_NE(fresh.path(), stale)
            << "ctor reused a stale directory instead of claiming a new slot";
        EXPECT_TRUE(fs::is_empty(fresh.path()))
            << "scratch dir must start empty; got a directory holding "
               "another run's leftovers";
    }

    fs::remove_all(stale, ec);
}

// originalCwd accessor pins the captured cwd for tests that need
// to construct paths relative to the pre-cwd-change directory.
TEST(ScratchDirSubstrate, OriginalCwdMatchesCurrentPathAtCtor) {
    auto const cwdAtCtor = fs::current_path();
    ScratchDir sd{Location::InsideRepo, "scratch-dir-self-test"};
    EXPECT_EQ(sd.originalCwd(), cwdAtCtor);
}
