// [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]] — a header named from a ROOT
// resolves where it points, on every path model, in every spelling of the root.
//
// ★★★ WHY THIS FILE EXISTS ONLY NOW. The first cut of the fix made a UNC include
// resolve 23 times in 30 and the lane that wrote it DELIBERATELY SHIPPED NO
// TEST, on the stated grounds that "a test that passes four times in five is
// indistinguishable from a flaky harness, and a green run would be read as a
// close". That judgement was right. The intermittency has since been
// root-caused — undefined behaviour in `isListedInItsParent`, a reference bound
// to the `native()` of a temporary `fs::path`, which fails toward a SPURIOUS
// "yes, that ancestor is listed" and collapses the root prefix — so the
// behaviour is deterministic and a pin is now both possible and owed.
//
// ★★ EVERY ARM REPEATS, AND THE REPETITION IS THE ASSERTION. The defect these
// pin was NOT a wrong answer every time; it was a wrong answer some of the time,
// decided by whether a freed heap block still held the bytes it used to. A
// single successful resolution would have passed against the broken code roughly
// half the runs. Asserting that N consecutive resolutions ALL succeed is the
// honest shape for pinning a UB whose symptom is intermittent, and it names the
// iteration that broke.
//
// ⚠ THE SKIP IS LOUD AND NAMED. A host with no reachable UNC spelling cannot
// measure the multi-separator arm at all, and a silent skip there would be a
// licence to drop the whole property. The skip says which anchor went
// unmeasured.

#include "core/types/include_path_resolve.hpp"

#include "scratch_dir.hpp"
#include "unc_spelling.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using dss::HeaderNameMatching;
using dss::HeaderSearchStatus;
using dss::isRootedPath;
using dss::resolveInDir;
using dss::resolveIncludePath;
using dss::test_support::leadingSeparatorRun;
using dss::test_support::uncSpellingOf;

namespace {
constexpr int kRepeats = 30;
}  // namespace

// ── The predicate itself, on every host ────────────────────────────────────
//
// This is the arm that needs no share, and it is the one that would silently
// regress: `is_absolute()` reads FALSE for a multi-separator root on libstdc++
// (MEASURED, Strawberry g++ 13.2), TRUE on the MS STL. A tier that re-derived
// the question as a bare `is_absolute()` would therefore be correct on one build
// of DSS and broken on another, from the same source.
TEST(RootedPathPredicate, AMultiSeparatorRootIsRooted) {
    EXPECT_TRUE(isRootedPath(fs::path{"//server/share/hdr.h"}))
        << "a path naming a location from a root was read as RELATIVE, so it "
           "would be searched against the include dirs instead of resolved "
           "where it points";
    EXPECT_TRUE(isRootedPath(fs::path{"/rooted/hdr.h"}));
}

// The CONTROL for the arm above: the predicate must not start calling ordinary
// relative names rooted, which would skip the include-dir search entirely.
TEST(RootedPathPredicate, RelativeNamesStayRelative) {
    EXPECT_FALSE(isRootedPath(fs::path{"hdr.h"}));
    EXPECT_FALSE(isRootedPath(fs::path{"sub/hdr.h"}));
    EXPECT_FALSE(isRootedPath(fs::path{"./hdr.h"}));
    // A drive-RELATIVE spelling has a root NAME and no root DIRECTORY, and it
    // really is relative — to that drive's working directory.
    EXPECT_FALSE(isRootedPath(fs::path{"C:hdr.h"}));
}

// ── End to end, through the shared resolver ────────────────────────────────
//
// `includingDir` and `includeDirs` are BOTH EMPTY on purpose: with nothing to
// search, the ONLY way any of these can resolve is the rooted arm. A pin that
// left an include dir populated would still pass if the rooted arm were deleted.
class RootedIncludeResolution
    : public ::testing::TestWithParam<HeaderNameMatching> {};

TEST_P(RootedIncludeResolution, ALocalAbsoluteHeaderResolvesEveryTime) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "rooted-include"};
    fs::path const header = sd.path() / "rooted_probe.h";
    { std::ofstream f{header}; f << "int rooted_marker = 7;\n"; }
    ASSERT_TRUE(fs::exists(header));

    for (int i = 0; i < kRepeats; ++i) {
        auto const r = resolveIncludePath(header.string(), {}, {}, GetParam());
        ASSERT_EQ(r.status, HeaderSearchStatus::Found)
            << "iteration " << i << ": a local absolute include stopped "
               "resolving — this is the CONTROL arm and it must be inert in "
               "every state of the rooted-path code";
    }
}

TEST_P(RootedIncludeResolution, AUncHeaderResolvesInBothSpellingsEveryTime) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "rooted-include"};
    fs::path const header = sd.path() / "rooted_probe.h";
    { std::ofstream f{header}; f << "int rooted_marker = 7;\n"; }
    ASSERT_TRUE(fs::exists(header));

    fs::path const forward = uncSpellingOf(header);
    if (forward.empty())
        GTEST_SKIP()
            << "D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED: this host offers "
               "no reachable UNC spelling of '"
            << header.string()
            << "', so the multi-separator-root arm WAS NOT MEASURED on this "
               "leg. This is an unmeasured property, NOT a passing one.";
    // The same file, spelled with the host's preferred separator instead. Built
    // by conversion rather than by writing a second literal, so the two arms
    // cannot drift apart.
    fs::path const preferred = fs::path{forward}.make_preferred();

    for (fs::path const& spelling : {forward, preferred}) {
        ASSERT_GE(leadingSeparatorRun(spelling), 2u)
            << "the fixture stopped producing a multi-separator root, so this "
               "test would pass without exercising the property: "
            << spelling.string();
        for (int i = 0; i < kRepeats; ++i) {
            auto const r =
                resolveIncludePath(spelling.string(), {}, {}, GetParam());
            ASSERT_EQ(r.status, HeaderSearchStatus::Found)
                << "iteration " << i << " of spelling '" << spelling.string()
                << "': a header the OS opens without trouble was reported "
                   "missing. An intermittent failure here is the signature of "
                   "the root-prefix walk reading freed memory — see "
                   "`isListedInItsParent`.";
            EXPECT_GE(leadingSeparatorRun(r.path), 2u)
                << "iteration " << i << ": the resolved path lost its leading "
                   "separator run, so it no longer names the machine that was "
                   "asked for: " << r.path.string();
        }
    }
}

// ── The UNC path as the SEARCH DIRECTORY, not as the include NAME ──────────
//
// ★★★ THIS IS THE ARM THE TWO ABOVE STRUCTURALLY CANNOT REACH, AND IT WAS THE
// ONE A USER ACTUALLY HIT. They hand the rooted path as the include NAME with
// `includeDirs` DELIBERATELY EMPTY — which is exactly right for pinning the
// rooted arm, and exactly why neither can see `-I //host/share/inc` with a
// RELATIVE `#include "hdr.h"`. That route never touches the rooted arm at all:
// it goes through `descend` from the search directory. ✔MEASURED against the
// composed binary before this arm existed — 0/30 for BOTH slash spellings while
// the local control sat at 30/30 and every test in this file was green.
//
// ⓘ The cause was one tier further out than this function: the driver ran every
// `-I` value through a bare `fs::absolute`, which on a path model that gives a
// UNC authority no `root_name()` does not fail but SUCCEEDS having re-rooted it
// onto the local drive (`//host/share/inc` -> `C:\host\share\inc`). The pin
// lives here anyway, because this is the layer whose contract is "resolve a
// relative name against this directory" and the property must hold whatever the
// caller's provenance. `absoluteKeepingRoot` is pinned separately in
// `test_path_identity.cpp`.
TEST_P(RootedIncludeResolution, AUncSearchDirectoryResolvesARelativeHeader) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "rooted-include-dir"};
    fs::path const header = sd.path() / "rooted_probe.h";
    { std::ofstream f{header}; f << "int rooted_marker = 7;\n"; }
    ASSERT_TRUE(fs::exists(header));

    fs::path const uncDir = uncSpellingOf(sd.path());
    if (uncDir.empty())
        GTEST_SKIP()
            << "D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED: this host offers "
               "no reachable UNC spelling of '"
            << sd.path().string()
            << "', so the UNC-SEARCH-DIRECTORY arm WAS NOT MEASURED on this "
               "leg. This is an unmeasured property, NOT a passing one.";

    for (fs::path const& spelling : {uncDir, fs::path{uncDir}.make_preferred()}) {
        ASSERT_GE(leadingSeparatorRun(spelling), 2u)
            << "the fixture stopped producing a multi-separator root, so this "
               "test would pass without exercising the property: "
            << spelling.string();
        for (int i = 0; i < kRepeats; ++i) {
            // The RELATIVE name — the shape a quote include actually carries.
            auto const r = resolveInDir(spelling, "rooted_probe.h", GetParam());
            ASSERT_EQ(r.status, HeaderSearchStatus::Found)
                << "iteration " << i << " of search dir '" << spelling.string()
                << "': a header sitting in a directory the OS enumerates was "
                   "reported missing. This is the `-I` route, and it does NOT "
                   "go through the rooted arm the other tests pin.";
            EXPECT_GE(leadingSeparatorRun(r.path), 2u)
                << "iteration " << i << ": the resolved path lost its leading "
                   "separator run, so it no longer names the machine that was "
                   "asked for: " << r.path.string();
        }
    }
}

INSTANTIATE_TEST_SUITE_P(BothMatchingPolicies, RootedIncludeResolution,
                         ::testing::Values(HeaderNameMatching::CaseSensitive,
                                           HeaderNameMatching::CaseInsensitive));
