// `core::PathIdentity` — the ONE path-identity chokepoint.
//
// ★★★ WHAT THIS PINS, AND WHY EACH ARM EXISTS. The mechanism replaced 14 sites
// that keyed path identity on `fs::weakly_canonical` or (twice) on a raw
// `generic_string()`. Two spellings of one file surviving as two keys means a
// header preprocessed twice past its `#pragma once` and a duplicate CU with a
// duplicate-symbol link error naming no manifest, so the property is
// correctness-critical and every arm below breaks if the mechanism regresses.
//
// ⚠ THE TRAP THIS FILE EXISTS TO AVOID, STATED FIRST BECAUSE IT ALMOST LANDED.
// A test that merely HOPES to meet a short name passes silently on any volume
// with 8.3 generation disabled — a false green of exactly the family that
// produced the defect (the MSVC STL happens to normalize 8.3, which is why the
// property looked held while it was only accidentally satisfied). So the 8.3
// arms CONSTRUCT the condition, assert it materialised, and SKIP LOUDLY when the
// host cannot stage it. A skip is never a pass here.

#include "core/substrate/path_identity.hpp"

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_set>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace fs = std::filesystem;
using dss::core::PathIdentity;

namespace {

// The 8.3 spelling of `dir`, or empty when this host cannot produce one.
// Windows-only by construction: no other platform has the hazard.
[[nodiscard]] fs::path shortSpellingOf([[maybe_unused]] fs::path const& dir) {
#ifdef _WIN32
    std::wstring const in = dir.wstring();
    DWORD const        n  = ::GetShortPathNameW(in.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring out(n, L'\0');
    DWORD const written = ::GetShortPathNameW(in.c_str(), out.data(), n);
    if (written == 0 || written >= n) return {};
    out.resize(written);
    return fs::path{out};
#else
    return {};
#endif
}

}  // namespace

// ── The property, on a path that EXISTS ────────────────────────────────────
TEST(PathIdentitySubstrate, TwoSpellingsOfOneDirectoryShareOneIdentity) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "path-identity"};
    // A component longer than 8 characters is what makes an 8.3 alias exist at
    // all; a short one would be its own short name and stage nothing.
    fs::path const  probe = sd.path() / "a-deliberately-long-component-name";
    std::error_code ec;
    fs::create_directories(probe, ec);
    ASSERT_FALSE(ec) << "could not create the probe dir: " << ec.message();

    fs::path const shortForm = shortSpellingOf(probe);
    if (shortForm.empty() || shortForm == probe) {
        GTEST_SKIP() << "this host produces no divergent 8.3 spelling for '"
                     << probe.generic_string()
                     << "' (not Windows, or 8.3 generation disabled on this "
                        "volume — `fsutil 8dot3name query`), so the hazard "
                        "cannot be staged. NOT a pass: the pin simply has no "
                        "experiment to run here.";
    }
    ASSERT_NE(shortForm.generic_string(), probe.generic_string())
        << "the staged spelling is byte-identical to the long one, so this "
           "test would be comparing a path with itself";

    EXPECT_EQ(PathIdentity::of(shortForm), PathIdentity::of(probe))
        << "two spellings of ONE directory produced TWO identities — this is "
           "the defect the type exists to make impossible.\n  short: "
        << PathIdentity::of(shortForm).string()
        << "\n  long : " << PathIdentity::of(probe).string();

    // And the containers built on it agree, which is the form every converted
    // site actually uses.
    std::unordered_set<PathIdentity> seen;
    EXPECT_TRUE(seen.insert(PathIdentity::of(probe)).second);
    EXPECT_FALSE(seen.insert(PathIdentity::of(shortForm)).second)
        << "the set admitted a second entry for one directory";
}

// ── The trap: a short PREFIX with a tail that does not exist yet ───────────
//
// ★★★ THIS IS THE ARM THAT ALMOST SHIPPED BROKEN. `GetLongPathNameW` fails
// WHOLESALE when any component is absent, so the obvious one-call composition
// no-ops on exactly the paths that do not exist yet — every output artifact,
// every not-yet-generated source — while looking perfectly correct on every
// input path that happens to exist. ✔MEASURED before the code was written:
//     DSS-SC~1                   ->  <expanded>     (correct)
//     DSS-SC~1\not\yet\built.o   ->  DSS-SC~1\...   (UNCHANGED)
// RED-ON-DISABLE: replace the longest-existing-prefix walk in
// `expandShortComponents` with a single `longNameOrEmpty(in)` and this fails
// while every other arm in this file still passes.
TEST(PathIdentitySubstrate, ShortPrefixIsExpandedEvenWhenTheTailDoesNotExist) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "path-identity"};
    fs::path const  probe = sd.path() / "another-long-component-for-8dot3";
    std::error_code ec;
    fs::create_directories(probe, ec);
    ASSERT_FALSE(ec) << "could not create the probe dir: " << ec.message();

    fs::path const shortForm = shortSpellingOf(probe);
    if (shortForm.empty() || shortForm == probe) {
        GTEST_SKIP() << "no divergent 8.3 spelling available on this host, so "
                        "the non-existent-tail hazard cannot be staged. NOT a "
                        "pass.";
    }

    // Nothing creates these — that is the point.
    fs::path const longTail  = probe / "not" / "yet" / "built.o";
    fs::path const shortTail = shortForm / "not" / "yet" / "built.o";
    ASSERT_FALSE(fs::exists(shortTail));

    EXPECT_EQ(PathIdentity::of(shortTail), PathIdentity::of(longTail))
        << "an output path under a short-spelled directory kept its short "
           "spelling, so every not-yet-created artifact keys differently from "
           "the same artifact named the long way.\n  short-tail: "
        << PathIdentity::of(shortTail).string()
        << "\n  long-tail : " << PathIdentity::of(longTail).string();
}

// ── Degradation, not exceptions ────────────────────────────────────────────
TEST(PathIdentitySubstrate, AbsentPathStillYieldsAUsableIdentity) {
    fs::path const ghost = fs::temp_directory_path()
                         / "dss-no-such-dir-ever" / "x" / "y.o";
    ASSERT_FALSE(fs::exists(ghost));
    auto const id = PathIdentity::of(ghost);
    EXPECT_FALSE(id.string().empty())
        << "identity must be computable BEFORE a path exists — output "
           "artifacts, `path` dependencies resolved before the directory is "
           "confirmed, and the cycle key needed for a reject's own message are "
           "all keyed while absent";
    EXPECT_EQ(id, PathIdentity::of(ghost)) << "identity is not deterministic";
}

TEST(PathIdentitySubstrate, DotAndDotDotAreResolved) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "path-identity"};
    fs::path const  dir = sd.path() / "sub";
    std::error_code ec;
    fs::create_directories(dir, ec);
    ASSERT_FALSE(ec);

    EXPECT_EQ(PathIdentity::of(dir),
              PathIdentity::of(sd.path() / "." / "sub"));
    EXPECT_EQ(PathIdentity::of(dir),
              PathIdentity::of(dir / ".." / "sub"));
}

TEST(PathIdentitySubstrate, DifferentFilesKeepDifferentIdentities) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "path-identity"};
    EXPECT_NE(PathIdentity::of(sd.path() / "a.c"),
              PathIdentity::of(sd.path() / "b.c"))
        << "collapsing distinct files into one identity would silently DROP a "
           "translation unit — the opposite failure, and the worse one";
}

// ── The error-reporting arm exists and reports ─────────────────────────────
//
// `project_sources.cpp` fails the build loud on a filesystem refusal here, so
// the overload must actually leave `ec` alone for the ordinary cases. An `ec`
// that were set by mere absence would turn every not-yet-generated source into
// a hard build error.
TEST(PathIdentitySubstrate, AbsenceIsNotAnError) {
    fs::path const  ghost = fs::temp_directory_path() / "dss-absent" / "z.c";
    std::error_code ec;
    auto const      id = PathIdentity::of(ghost, ec);
    EXPECT_FALSE(ec) << "a path that merely does not exist was reported as a "
                        "filesystem failure (" << ec.message()
                     << "), which would fail builds on every generated source";
    EXPECT_FALSE(id.string().empty());
}

// ── Separator convention is uniform ───────────────────────────────────────
//
// The sites this replaced were SPLIT between `.string()` and
// `.generic_string()`. Two containers keying one file with different separators
// disagree about identity for a reason no diagnostic can explain.
TEST(PathIdentitySubstrate, IdentityUsesOneSeparatorConvention) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "path-identity"};
    auto const id = PathIdentity::of(sd.path() / "sub" / "leaf.c");
    EXPECT_EQ(id.string().find('\\'), std::string::npos)
        << "identity keys must use one separator convention: " << id.string();
}
