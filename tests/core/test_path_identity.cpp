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
#include "unc_spelling.hpp"

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

// [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]] — nor is a path the LIBRARY
// cannot walk but the OS opens without trouble.
//
// ✔MEASURED: `fs::weakly_canonical` fails ENOENT 200 times out of 200 on every
// spelling of a reachable path with a multi-separator root, while `exists()`,
// `file_size()` and a directory walk of its parent all answer normally. That is
// the path MODEL failing, not the host refusing, and forwarding it as `ec` made
// the caller above — which fails the whole build loud on this flag, and whose
// own comment asserts this "means a genuine filesystem failure" — reject a
// perfectly readable source.
TEST(PathIdentitySubstrate, AReachablePathIsNeverReportedAsAFilesystemFailure) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "path-identity"};
    fs::path const local = sd.path() / "reachable.c";
    { std::ofstream f{local}; f << "int x;\n"; }
    ASSERT_TRUE(fs::exists(local));

    fs::path const unc = dss::test_support::uncSpellingOf(local);
    if (unc.empty())
        GTEST_SKIP()
            << "D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED: this host offers "
               "no reachable multi-separator-root spelling of '"
            << local.string()
            << "', so this arm WAS NOT MEASURED on this leg. Unmeasured is not "
               "passing.";
    ASSERT_TRUE(fs::exists(unc)) << unc.string();

    std::error_code ec;
    auto const      id = PathIdentity::of(unc, ec);
    EXPECT_FALSE(ec) << "a path the OS opens was reported as a filesystem "
                        "refusal (" << ec.message()
                     << "), which fails the build for a source that is right "
                        "there: " << unc.string();
    EXPECT_FALSE(id.string().empty());
}

// ── [[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]] ──────────────────────
//
// A path whose spelling begins with a run of TWO OR MORE separators names a
// location on ANOTHER machine on the path models that give the authority no
// `root_name()` of its own. Both halves of the shipped derivation deleted that
// run — `lexically_normal()` on the degraded arm and `generic_string()` on the
// way out — so the key named a directory on the LOCAL drive root instead.
//
// ★ NO SHARE IS NEEDED TO PIN THIS, and that is not a convenience — it is where
// the defect is WORST. When the authority is unreachable, `weakly_canonical`
// does not fail: it SUCCEEDS, having quietly re-rooted the path onto the local
// drive (MEASURED: `//no-such-server/share/x.h` -> `C:\no-such-server\share\x.h`,
// no error). The key it produced was then byte-identical to a real local file's,
// which is a collision — two distinct files, one identity — and the maps this
// type keys would serve one file's contents for the other.
TEST(PathIdentitySubstrate, AMultiSeparatorRootIsNotCollapsedOntoTheLocalDrive) {
    fs::path const authority{"//dss-no-such-server/share/hdr.h"};
    fs::path const localSameName{"/dss-no-such-server/share/hdr.h"};
    EXPECT_NE(PathIdentity::of(authority), PathIdentity::of(localSameName))
        << "a path naming another machine and a path naming the local drive "
           "root collapsed onto ONE identity ("
        << PathIdentity::of(authority).string()
        << ") — that is a wrong-content hazard, not a duplicate-work one";

    // ★ THE EXACT COLLISION AS MEASURED, and it needs no drive letter written
    // here: whatever root NAME this host qualifies its paths with is asked for
    // rather than assumed, so the arm degenerates to the one above on a host
    // that has none. ✔MEASURED against the shipped derivation, byte-identical
    // keys for two different locations:
    //     '//no-such-server/share/x.h'    -> 'C:/no-such-server/share/x.h/'
    //     'C:/no-such-server/share/x.h'   -> 'C:/no-such-server/share/x.h/'
    fs::path const driveQualified =
        fs::current_path().root_name() / localSameName;
    EXPECT_NE(PathIdentity::of(authority), PathIdentity::of(driveQualified))
        << "a path naming another machine keyed identically to a LOCAL file of "
           "the same name ("
        << PathIdentity::of(authority).string()
        << "), so every map this type keys would serve one file's contents for "
           "the other";
}

// The two spellings of one such root are the SAME location and must key alike;
// this is the other direction of the arm above, and a fix that merely preserved
// the separators verbatim would fail it.
TEST(PathIdentitySubstrate, BothSpellingsOfOneRootShareOneIdentity) {
    fs::path const forward{"//dss-no-such-server/share/hdr.h"};
    fs::path const preferred = fs::path{forward}.make_preferred();
    EXPECT_EQ(PathIdentity::of(forward), PathIdentity::of(preferred))
        << "one location spelled two ways produced two identities: '"
        << PathIdentity::of(forward).string() << "' vs '"
        << PathIdentity::of(preferred).string() << "'";
}

// ── An identity must not change when the file comes into being ────────────
//
// The header's own note makes the not-yet-created path a FIRST-CLASS input
// (output artifacts, a `path` dependency resolved before its directory is
// confirmed, the cycle key needed for a reject's own message). ✔MEASURED before
// the fix, one path and two keys differing only in whether it existed yet:
//     before : 'C:/.../idtmp/notyet.o/'      <- trailing separator
//     after  : 'C:/.../idtmp/notyet.o'
// The short-name walk built its tail with `filename() / tail` against an EMPTY
// tail, and appending to an empty path appends a SEPARATOR.
TEST(PathIdentitySubstrate, CreatingTheFileDoesNotChangeItsIdentity) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "path-identity"};
    fs::path const artifact = sd.path() / "notyet.o";
    ASSERT_FALSE(fs::exists(artifact));
    auto const before = PathIdentity::of(artifact);
    { std::ofstream f{artifact}; f << "x"; }
    ASSERT_TRUE(fs::exists(artifact));
    EXPECT_EQ(before, PathIdentity::of(artifact))
        << "the identity of one path changed when the file was created ('"
        << before.string() << "' -> '"
        << PathIdentity::of(artifact).string()
        << "'), so anything keyed before the write and read after it holds TWO "
           "entries for one file";
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

// ── `absoluteKeepingRoot` — making a path absolute must not invent a drive ──
//
// ★★★ THE DEFECT, MEASURED BEFORE THIS PIN EXISTED. Four tiers called bare
// `fs::absolute` on a caller-supplied path (`-I` search dirs, the shipped-config
// walk, the artifact-written report, and the executable lookup in
// `process_spawn`). On a path model that gives a UNC authority no `root_name()`,
// `fs::absolute("//host/share/x")` does NOT fail — it returns
// `C:\host\share\x`, with NO error. A path naming another machine silently
// became a path on the local drive.
//
// ★★ THAT IS A DIFFERENT FAILURE DIRECTION FROM `weakly_canonical`, WHICH IS WHY
// ONLY ONE OF THEM NEEDED FIXING. ✔MEASURED on the same host, same path:
// `weakly_canonical` ERRORS ("No such file or directory"), so every caller's
// existing on-error-keep-the-original arm already did the right thing.
// `absolute` SUCCEEDS WRONGLY, which no error arm can catch. A helper that fails
// toward a wrong answer is the one that needs the guard.
TEST(PathIdentitySubstrate, AbsoluteDoesNotRerootAMultiSeparatorPath) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "abs-keeping-root"};
    fs::path const unc = dss::test_support::uncSpellingOf(sd.path());
    if (unc.empty())
        GTEST_SKIP()
            << "D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED: this host offers "
               "no reachable UNC spelling of '"
            << sd.path().string()
            << "', so the re-rooting arm WAS NOT MEASURED on this leg. This is "
               "an unmeasured property, NOT a passing one.";
    ASSERT_GE(dss::test_support::leadingSeparatorRun(unc), 2u);

    std::error_code ec;
    fs::path const got = dss::core::absoluteKeepingRoot(unc, ec);
    EXPECT_FALSE(ec) << "preserving a rooted path cannot fail: " << ec.message();
    EXPECT_GE(dss::test_support::leadingSeparatorRun(got), 2u)
        << "the leading separator run was collapsed, so this no longer names "
           "the machine that was asked for: " << got.string();
    std::error_code existsEc;
    EXPECT_TRUE(fs::exists(got, existsEc))
        << "the result must still name a directory that is there: "
        << got.string();
}

// The CONTROL, and it is the arm that stops the fix from becoming a regression.
// A run of ONE separator is a genuine location on the current drive and MUST
// still be made absolute — `isRootedPath` would have called it rooted and
// skipped it, which is why the discriminator is the separator RUN and not that
// predicate. A relative path must also still resolve against the cwd.
TEST(PathIdentitySubstrate, AbsoluteStillResolvesSingleSeparatorAndRelative) {
    std::error_code ec;
    fs::path const rel = dss::core::absoluteKeepingRoot(fs::path{"some_relative_leaf"}, ec);
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_TRUE(rel.is_absolute())
        << "a relative path stopped being resolved against the cwd: "
        << rel.string();

    ec.clear();
    fs::path const rooted = dss::core::absoluteKeepingRoot(fs::path{"/single_sep_leaf"}, ec);
    EXPECT_FALSE(ec) << ec.message();
    // On Windows this acquires the current drive; on POSIX it is already
    // absolute and comes back unchanged. Both are `is_absolute()`, and asking
    // only that keeps the arm free of a platform branch.
    EXPECT_TRUE(rooted.is_absolute())
        << "a single-separator root must still be made absolute — skipping it "
           "would leave the drive undecided: " << rooted.string();
}
