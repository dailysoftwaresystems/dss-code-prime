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

#include "core/substrate/os_path_spelling.hpp"
#include "scratch_dir.hpp"
#include "unc_spelling.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

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

// ── [[D-PATH-MULTI-SEPARATOR-ROOT-COLLAPSED-BY-STDLIB-PATH-TRANSFORMS]] ─────
//
// THE `u8` SPELLING LOSES THE RUN EXACTLY AS THE NARROW ONE DOES, and that is
// not obvious from the name — `generic_u8string()` reads like the LOSSLESS
// member, and it is, but only along the ENCODING axis.
//
// ★★★ ✔MEASURED 2026-08-28, one path object, all three renderings printed side
// by side on a REACHABLE UNC directory:
//     .string()           '//localhost/C$/Source/DailySoftware'   run 2
//     .generic_string()   '/localhost/C$/Source/DailySoftware'    run 1
//     .generic_u8string() '/localhost/C$/Source/DailySoftware'    run 1
// `renderCandidatePath` in the header-case diagnostic had moved to the `u8` form
// to stop `std::system_error` on a name the active code page cannot encode
// ([[D-PP-HEADER-CASE-NON-ASCII-NAME-NARROWING-THROW]]) — a real repair on a
// DIFFERENT axis, which left the report naming the wrong MACHINE while spelling
// the filename perfectly.
TEST(PathIdentitySubstrate, GenericSpellingU8KeepsAMultiSeparatorRoot) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "generic-u8-keeping-root"};
    fs::path const unc = dss::test_support::uncSpellingOf(sd.path());
    if (unc.empty())
        GTEST_SKIP()
            << "D-PATH-MULTI-SEPARATOR-ROOT-COLLAPSED-BY-STDLIB-PATH-TRANSFORMS"
               ": this host offers no reachable multi-separator spelling of '"
            << sd.path().string()
            << "', so the u8 rendering WAS NOT MEASURED on this leg. This is an "
               "UNMEASURED property, not a passing one.";
    ASSERT_GE(dss::test_support::leadingSeparatorRun(unc), 2u);

    std::u8string const got = dss::core::genericSpellingU8(unc);
    std::string const   narrow(reinterpret_cast<char const*>(got.data()),
                             got.size());
    EXPECT_GE(dss::test_support::leadingSeparatorRun(fs::path{narrow}), 2u)
        << "the u8 spelling collapsed the leading run, so a diagnostic built "
           "from it names a path on this drive instead of the host that was "
           "asked about: " << narrow;
    // The generic CONVENTION still holds — this is a spelling fix, not a
    // licence to leak native separators into a rendered path.
    EXPECT_EQ(narrow.find('\\'), std::string::npos)
        << "a rendered spelling must use one separator convention: " << narrow;
}

// The CONTROL for the u8 spelling: a path with NO multi-separator root must come
// back byte-identical to what it always was, so the new transform cannot be the
// reason any existing diagnostic's wording moved.
TEST(PathIdentitySubstrate, GenericSpellingU8LeavesAnOrdinaryPathAlone) {
    dss::test_support::ScratchDir sd{dss::test_support::Location::Temp,
                                     "generic-u8-control"};
    std::u8string const got = dss::core::genericSpellingU8(sd.path());
    std::string const   narrow(reinterpret_cast<char const*>(got.data()),
                             got.size());
    EXPECT_EQ(narrow, sd.path().generic_string())
        << "on a path with a leading run below 2 the two spellings must agree "
           "exactly — this arm is what keeps the fix inert everywhere else";
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

// ── THE ROOT'S ONE PARSE AND THE ONE NORMALIZATION, AS TEXT, IN BOTH GRAMMARS, ON EVERY LEG (P69 H6) ────────────────
//
// The table IS the universe of `rootSpelling`'s header -- every Win32 prefix (none, a drive, drive-relative, rooted, a
// share, `\\.\`, `//./`, `//?/`, `\\?\`, with `UNC`, `Volume{G}` and `GLOBALROOT` in either case), a `..` that would
// climb into each root, and the names Win32 trims -- and each expected value is what Win32 itself did with the same
// spelling (✔MEASURED 2026-11-04, probe-reference-cc runs 20261104-204243-d5c02d5f, 20261104-205708-1394b8a2 and
// 20261104-210843-61c9329b: GetFullPathNameW and the opened handle; under `\\?\` the handle, which opens the rest
// literally while GetFullPathNameW collapses it). The two grammars are pure text, so every leg asks both; the POSIX
// rows hold POSIX's own rules: `//` is kept apart, three or more slashes are one (POSIX 4.13), and nothing is trimmed.
namespace {
struct RootRow {
    std::wstring_view            spelling;
    dss::core::RootSpelling::Kind kind;
    std::size_t                  prefix;
    std::size_t                  volume;
    bool                         authority;
    std::wstring_view            normalized;
};
using RK = dss::core::RootSpelling::Kind;
// A table row is ASCII, so each code unit IS its character: shown narrow, one unit at a time.
[[nodiscard]] std::string ascii(std::wstring_view w) {
    std::string s;
    for (wchar_t const c : w) s.push_back(static_cast<char>(c));
    return s;
}
}  // namespace

TEST(PathIdentitySubstrate, EveryWin32RootIsParsedOnceAndNormalizedAsWin32Does) {
    RootRow const rows[] = {
        // no root, a drive, drive-relative, rooted: the library's own model agrees on these (MEASURED both)
        {L"x\\y", RK::Relative, 0, 0, false, L"x\\y"},
        {L"a\\..\\..\\b", RK::Relative, 0, 0, false, L"..\\b"},
        {L"C:x", RK::DriveRelative, 2, 2, false, L"C:x"},
        {L"C:..\\x", RK::DriveRelative, 2, 2, false, L"C:..\\x"},
        {L"\\x\\..\\..\\y", RK::Rooted, 1, 1, false, L"\\y"},
        {L"C:\\a\\..\\..\\x", RK::Drive, 3, 3, false, L"C:\\x"},   // the drive holds its `..`
        {L"c:/a/b", RK::Drive, 3, 3, false, L"c:\\a\\b"},
        // a share holds its `..` (`\\localhost\C$\..\x` opened `\\localhost\C$\x`)
        {L"\\\\server\\share\\a\\..\\..\\x", RK::Share, 15, 15, true, L"\\\\server\\share\\x"},
        {L"//server/share/x", RK::Share, 15, 15, true, L"\\\\server\\share\\x"},
        {L"\\\\server", RK::Share, 8, 8, true, L"\\\\server"},
        // a device path is normalized, and its `..` climbs over the volume to the four-character prefix
        {L"\\\\.\\C:\\a\\..\\..\\x", RK::Device, 4, 7, false, L"\\\\.\\x"},
        {L"//./C:/x", RK::Device, 4, 7, false, L"\\\\.\\C:\\x"},
        {L"//?/C:/sub/../x", RK::Device, 4, 7, false, L"\\\\?\\C:\\x"},   // `//?/` is NOT verbatim; its `?` stays
        {L"//?/C:/x/..", RK::Device, 4, 7, false, L"\\\\?\\C:"},
        {L"\\\\.\\C:\\d\\..", RK::Device, 4, 7, false, L"\\\\.\\C:"},   // a last `..` leaves no separator
        {L"\\\\.\\C:\\d\\..\\", RK::Device, 4, 7, false, L"\\\\.\\C:\\"},
        {L"\\\\.\\GLOBALROOT\\Device\\HarddiskVolume3\\..\\x", RK::Device, 4, 38, false,
         L"\\\\.\\GLOBALROOT\\Device\\x"},
        {L"\\\\.\\UNC\\h\\s\\..\\x", RK::Device, 4, 12, true, L"\\\\.\\UNC\\h\\x"},
        {L"\\\\.\\Volume{G}\\x", RK::Device, 4, 14, false, L"\\\\.\\Volume{G}\\x"},
        // verbatim: the handle opens the rest literally, so nothing of it is normalized
        {L"\\\\?\\C:\\a\\..\\x", RK::Verbatim, 7, 7, false, L"\\\\?\\C:\\a\\..\\x"},
        {L"\\\\?\\C:\\a\\.\\x", RK::Verbatim, 7, 7, false, L"\\\\?\\C:\\a\\.\\x"},
        {L"\\\\?\\C:/a", RK::Verbatim, 8, 8, false, L"\\\\?\\C:/a"},   // `/` is a character: one segment
        {L"\\\\?\\C:\\x.txt.", RK::Verbatim, 7, 7, false, L"\\\\?\\C:\\x.txt."},
        {L"\\\\?\\UNC\\h\\s\\x", RK::Verbatim, 12, 12, true, L"\\\\?\\UNC\\h\\s\\x"},
        {L"\\\\?\\unc\\h\\s\\x", RK::Verbatim, 12, 12, true, L"\\\\?\\unc\\h\\s\\x"},
        {L"\\\\?\\Volume{G}\\x", RK::Verbatim, 14, 14, false, L"\\\\?\\Volume{G}\\x"},
        {L"\\\\?\\volume{g}\\x", RK::Verbatim, 14, 14, false, L"\\\\?\\volume{g}\\x"},
        {L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume3\\x", RK::Verbatim, 38, 38, false,
         L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume3\\x"},
        {L"\\\\?\\globalroot\\Device\\V\\x", RK::Verbatim, 24, 24, false, L"\\\\?\\globalroot\\Device\\V\\x"},
        // what the normalized namespace trims (run 20261104-210843-61c9329b), and what it keeps
        {L"C:\\d.\\x.txt", RK::Drive, 3, 3, false, L"C:\\d\\x.txt"},
        {L"C:\\d..\\x.txt", RK::Drive, 3, 3, false, L"C:\\d..\\x.txt"},
        {L"C:\\d \\x.txt", RK::Drive, 3, 3, false, L"C:\\d \\x.txt"},
        {L"C:\\d. \\x.txt", RK::Drive, 3, 3, false, L"C:\\d. \\x.txt"},
        {L"C:\\d .\\x.txt", RK::Drive, 3, 3, false, L"C:\\d \\x.txt"},
        {L"C:\\d\\...\\x.txt", RK::Drive, 3, 3, false, L"C:\\d\\...\\x.txt"},
        {L"C:\\x.txt..", RK::Drive, 3, 3, false, L"C:\\x.txt"},
        {L"C:\\x.txt. .", RK::Drive, 3, 3, false, L"C:\\x.txt"},
        {L"C:\\d \\", RK::Drive, 3, 3, false, L"C:\\d \\"},
        {L"C:\\d.\\", RK::Drive, 3, 3, false, L"C:\\d\\"},
        {L"C:\\d\\...", RK::Drive, 3, 3, false, L"C:\\d\\"},
        {L"C:\\d\\   ", RK::Drive, 3, 3, false, L"C:\\d\\"},
        {L"C:\\sub\\.. \\x.txt", RK::Drive, 3, 3, false, L"C:\\sub\\.. \\x.txt"},
        {L"C:\\sub\\. \\x.txt", RK::Drive, 3, 3, false, L"C:\\sub\\. \\x.txt"},
        {L"C:\\d.\\..\\x.txt", RK::Drive, 3, 3, false, L"C:\\x.txt"},
        {L"\\\\h\\s\\x.txt.", RK::Share, 6, 6, true, L"\\\\h\\s\\x.txt"},
        {L"\\\\.\\C:\\d.\\x.txt", RK::Device, 4, 7, false, L"\\\\.\\C:\\d\\x.txt"},
        {L"f3probe\\x.txt.", RK::Relative, 0, 0, false, L"f3probe\\x.txt"},
        {L"C:f3probe\\x.txt.", RK::DriveRelative, 2, 2, false, L"C:f3probe\\x.txt"},
        // where a separator ends it: only where the spelling ended in one or in a name the trim removed -- a last `.`
        // or `..` leaves none, a root its own (runs 20261104-222037-6ef9b98e and 20261104-223135-5f04ee9f)
        {L"C:\\d\\.", RK::Drive, 3, 3, false, L"C:\\d"},
        {L"C:\\a\\b\\..", RK::Drive, 3, 3, false, L"C:\\a"},
        {L"C:\\a\\b\\..\\", RK::Drive, 3, 3, false, L"C:\\a\\"},
        {L"C:\\a\\.\\.\\", RK::Drive, 3, 3, false, L"C:\\a\\"},
        {L"C:\\d\\..", RK::Drive, 3, 3, false, L"C:\\"},
        {L"\\\\server\\share\\..", RK::Share, 15, 15, true, L"\\\\server\\share"},
        {L"\\\\server\\share\\..\\", RK::Share, 15, 15, true, L"\\\\server\\share\\"},
        {L"\\\\server\\share\\", RK::Share, 15, 15, true, L"\\\\server\\share\\"},
        // a last name `NUL`, trimmed of periods, spaces and colons, is the device -- below every root but a share's
        // and a device path's (the same runs); this release maps no other reserved name, and neither model claims one
        {L"C:\\d\\NUL", RK::Drive, 3, 3, false, L"\\\\.\\NUL"},
        {L"C:\\d\\Nul", RK::Drive, 3, 3, false, L"\\\\.\\Nul"},
        {L"C:\\d\\NUL.:", RK::Drive, 3, 3, false, L"\\\\.\\NUL"},
        {L"C:\\d\\x\\..\\NUL", RK::Drive, 3, 3, false, L"\\\\.\\NUL"},
        {L"\\d\\NUL", RK::Rooted, 1, 1, false, L"\\\\.\\NUL"},
        {L"C:NUL", RK::DriveRelative, 2, 2, false, L"\\\\.\\NUL"},
        {L"d\\NUL", RK::Relative, 0, 0, false, L"\\\\.\\NUL"},
        {L"C:\\d\\NUL.txt", RK::Drive, 3, 3, false, L"C:\\d\\NUL.txt"},
        {L"C:\\d\\NUL:x", RK::Drive, 3, 3, false, L"C:\\d\\NUL:x"},
        {L"C:\\d\\NUL\\", RK::Drive, 3, 3, false, L"C:\\d\\NUL\\"},
        {L"C:\\d\\NUL\\x", RK::Drive, 3, 3, false, L"C:\\d\\NUL\\x"},
        {L"\\\\h\\s\\NUL", RK::Share, 6, 6, true, L"\\\\h\\s\\NUL"},
        {L"\\\\.\\C:\\d\\NUL", RK::Device, 4, 7, false, L"\\\\.\\C:\\d\\NUL"},
        {L"C:\\d\\CON", RK::Drive, 3, 3, false, L"C:\\d\\CON"},
        // what is left of a relative spelling, as the OS leaves it below the directory it resolves against (run
        // 20261104-223135-5f04ee9f: `a\.` is `<dir>\a`, `..\` is `<parent>\`, `a\..` is `<dir>`)
        {L"", RK::Relative, 0, 0, false, L""},
        {L".", RK::Relative, 0, 0, false, L"."},
        {L"a\\.", RK::Relative, 0, 0, false, L"a"},
        {L"a\\b\\..", RK::Relative, 0, 0, false, L"a"},
        {L"a\\b\\..\\", RK::Relative, 0, 0, false, L"a\\"},
        {L"a\\..", RK::Relative, 0, 0, false, L"."},
        {L"..\\", RK::Relative, 0, 0, false, L"..\\"},
        {L"a\\\\b", RK::Relative, 0, 0, false, L"a\\b"},
    };
    for (RootRow const& row : rows) {
        std::string const shown = ascii(row.spelling);
        SCOPED_TRACE(shown);
        dss::core::RootSpelling const root = dss::core::rootSpelling(row.spelling, dss::core::PathGrammar::Win32);
        EXPECT_EQ(root.kind, row.kind);
        EXPECT_EQ(root.prefix, row.prefix);
        EXPECT_EQ(root.volume, row.volume);
        EXPECT_EQ(root.authority, row.authority);
        std::wstring const normal = dss::core::normalizedSpelling(row.spelling, dss::core::PathGrammar::Win32);
        EXPECT_EQ(normal, row.normalized) << "normalized as " << ascii(normal);
    }
}

TEST(PathIdentitySubstrate, EveryPosixRootIsParsedOnceAndNormalizedAsPosixSays) {
    struct Row {
        std::string_view spelling;
        RK               kind;
        std::size_t      volume;
        bool             authority;
        std::string_view normalized;
    };
    Row const rows[] = {
        {"/a/../../x", RK::Rooted, 1, false, "/x"},
        {"//x/y/../z", RK::Authority, 2, true, "//x/z"},
        {"///x", RK::Rooted, 3, false, "/x"},
        {"a/./b", RK::Relative, 0, false, "a/b"},
        {"../x/..", RK::Relative, 0, false, ".."},
        {"x.c.", RK::Relative, 0, false, "x.c."},   // POSIX trims nothing
        {"a\\b/..", RK::Relative, 0, false, "."},   // a backslash is a name's character
        {"C:/x", RK::Relative, 0, false, "C:/x"},
        {"", RK::Relative, 0, false, ""},
    };
    for (Row const& row : rows) {
        SCOPED_TRACE(std::string{row.spelling});
        dss::core::RootSpelling const root = dss::core::rootSpelling(row.spelling, dss::core::PathGrammar::Posix);
        EXPECT_EQ(root.kind, row.kind);
        EXPECT_EQ(root.prefix, row.volume);
        EXPECT_EQ(root.volume, row.volume);
        EXPECT_EQ(root.authority, row.authority);
        EXPECT_EQ(dss::core::normalizedSpelling(row.spelling, dss::core::PathGrammar::Posix), row.normalized);
    }
}

// When a verbatim final path may be spelled in the DOS form: exactly when the one normalization leaves its tail as it
// is and no component is a reserved device's name (taken with any extension, as older Windows takes it).
TEST(PathIdentitySubstrate, ADosSpellingIsUsedOnlyWhereItNamesTheSameFile) {
    struct Row { std::wstring_view tail; bool same; };
    Row const rows[] = {
        {L"", true},           {L"a\\b", true},     {L"a\\", true},     {L"d \\x", true},
        {L"d..\\x", true},     {L"COM10", true},    {L"x.", false},     {L"x ", false},
        {L"d.\\x", false},     {L"a\\\\b", false},  {L".\\a", false},   {L"a\\..", false},
        {L"a/b", false},       {L"NUL", false},     {L"d\\nul.txt", false}, {L"COM1", false},
        {L"lpt9\\x", false},
    };
    for (Row const& row : rows) {
        SCOPED_TRACE(ascii(row.tail));
        EXPECT_EQ(dss::core::dosSpellingNamesTheSameFile(row.tail), row.same);
    }
}

// The host's own normalization IS the one above, on a key: the lexical keys of `x.c.` and `x.c` are one key where
// the host's grammar is Win32's (the OS opens `x.c` for both), and two where it is POSIX's (two names).
TEST(PathIdentitySubstrate, TheLexicalKeyOfANameWin32TrimsIsTheTrimmedNamesKey) {
    fs::path const dir = fs::temp_directory_path() / "dss-no-such-dir-ever-h6";
    ASSERT_FALSE(fs::exists(dir));
    auto const trimmed  = PathIdentity::of(dir / "x.c");
    auto const trailing = PathIdentity::of(fs::path{(dir / "x.c").native() + fs::path::string_type(1, '.')});
    if constexpr (dss::core::kHostPathGrammar == dss::core::PathGrammar::Win32) {
        EXPECT_EQ(trailing, trimmed) << trailing.string() << " vs " << trimmed.string();
    } else {
        EXPECT_NE(trailing, trimmed) << "POSIX names `x.c.` and `x.c` two files";
    }
}

// ★ THE SPELLING A FILE CALL TAKES, AT ANY LENGTH (P69 H6, [[D-LINK-A-DEEP-OUTPUT-DIRECTORY-FAILS-THE-WINDOWS-IMAGE-WRITE]]):
// every Win32 spelling to the one extended-length spelling that opens what it means -- the OS's full spelling of it
// (GetFullPathNameW, os_path_spelling.hpp), then `\\?\` / `\\?\UNC\`, a device's `\\.\` replaced. Kept as written: a
// verbatim spelling and an empty one. POSIX: every spelling as given. A reserved name other than `NUL` is no row: which
// of them the OS maps is the release's (os_path_spelling.hpp), and the rule follows the OS either way.
TEST(PathIdentitySubstrate, EverySpellingTakesTheExtendedLengthSpellingThatOpensWhatItMeans) {
    using dss::core::extendedLengthSpelling;
#ifdef _WIN32
    {
        struct Row { std::wstring_view spelling; std::wstring_view extended; };
        Row const rows[] = {
            {L"C:\\d\\x.c", L"\\\\?\\C:\\d\\x.c"},
            {L"C:/d/x.c", L"\\\\?\\C:\\d\\x.c"},
            {L"C:\\d\\.\\sub\\..\\x.c. ", L"\\\\?\\C:\\d\\x.c"},          // the trim and the steps, as the OS
            {L"C:\\d.\\x.c", L"\\\\?\\C:\\d\\x.c"},                         // an intermediate name trims ONE period
            {L"C:\\", L"\\\\?\\C:\\"},
            {L"C:\\d\\NUL", L"\\\\?\\NUL"},                                  // the plain spelling names the DEVICE
            {L"\\\\server\\share\\d\\x.c", L"\\\\?\\UNC\\server\\share\\d\\x.c"},
            {L"//server/share/d/x.c", L"\\\\?\\UNC\\server\\share\\d\\x.c"},
            {L"\\\\server\\share\\..\\x.c", L"\\\\?\\UNC\\server\\share\\x.c"},   // a share holds its `..`
            {L"\\\\server\\share\\NUL", L"\\\\?\\UNC\\server\\share\\NUL"},  // a share's NUL is a name to the OS
            {L"\\\\?\\C:\\d\\..\\x.c.", L"\\\\?\\C:\\d\\..\\x.c."},          // verbatim: already literal
            {L"//?/C:/d/sub/../x.c", L"\\\\?\\C:\\d\\x.c"},                  // the OS's answer is verbatim: taken
            {L"\\\\?\\UNC\\server\\share\\x.c", L"\\\\?\\UNC\\server\\share\\x.c"},
            {L"\\\\.\\C:\\d\\x.c", L"\\\\?\\C:\\d\\x.c"},
            {L"\\\\.\\C:\\..\\C:\\d\\x.c.", L"\\\\?\\C:\\d\\x.c"},          // the climb over the volume, as the OS
            {L"//./C:/d/x.c", L"\\\\?\\C:\\d\\x.c"},
            {L"\\\\.\\C:\\d\\NUL", L"\\\\?\\C:\\d\\NUL"},                  // a FILE under `\\.\` as under `\\?\`
            {L"\\\\.\\NUL", L"\\\\?\\NUL"},
            {L"\\\\.\\pipe\\dss-x", L"\\\\?\\pipe\\dss-x"},
            {L"\\\\.\\UNC\\h\\s\\x.c", L"\\\\?\\UNC\\h\\s\\x.c"},
            {L"\\\\.\\GLOBALROOT\\Device\\HarddiskVolume3\\d\\x.c",
             L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume3\\d\\x.c"},
            {L"", L""},
        };
        for (Row const& row : rows) {
            SCOPED_TRACE(ascii(row.spelling));
            std::wstring const got = extendedLengthSpelling(fs::path{std::wstring{row.spelling}}).native();
            EXPECT_EQ(got, row.extended) << "spelled " << ascii(got);
        }
        // A relative and a rooted spelling are made absolute where the call would resolve them, then the same rule.
        std::wstring const cwd = fs::current_path().native();
        EXPECT_EQ(extendedLengthSpelling(fs::path{L"sub\\x.c"}).native(), L"\\\\?\\" + cwd + L"\\sub\\x.c");
        EXPECT_EQ(extendedLengthSpelling(fs::path{L"\\d\\x.c"}).native(),
                  L"\\\\?\\" + cwd.substr(0, 2) + L"\\d\\x.c");
        // At any length: a spelling of 300 characters takes the same rule as a short one.
        std::wstring const deep = L"C:\\" + std::wstring(296, L'a') + L"\\x";
        EXPECT_EQ(extendedLengthSpelling(fs::path{deep}).native(), L"\\\\?\\" + deep);
    }
#else
    for (char const* spelling : {"/a/b.c", "//host/share/x", "rel/x.c", "x.c.", ""}) {
        EXPECT_EQ(extendedLengthSpelling(fs::path{spelling}).native(), fs::path{spelling}.native()) << spelling;
    }
#endif
}

// ★ THE ONE NORMALIZATION IS THE OS'S ANSWER (P69 H6): over F3's universe the Win32 grammar's text normalization
// EQUALS GetFullPathNameW -- the OS asked live, through the rule's one home (os_path_spelling.hpp) -- on every
// Windows leg, so the model a host without Win32 answers by and the OS a Windows host asks cannot drift apart. The
// cells are every absolute cell the OS was measured on (runs 20261104-222037-6ef9b98e and 20261104-223135-5f04ee9f:
// each root kind, its climbs, the trim, where a separator ends the answer, the device name `NUL`), a relative spelling
// joined below this process's directory, and a 300-character spelling. ⚠ Not a cell: a reserved name other than the
// forms of `NUL` every release maps -- `CON`, `COM1`, `NUL.txt`, ... are names to this Windows 11 and devices to older
// ones (DOCUMENTED, "Naming Files, Paths, and Namespaces"), the RELEASE's answer, which a Windows host's key takes from
// the OS itself (`normalizeKeepingRoot`).
TEST(PathIdentitySubstrate, TheWin32NormalizationEqualsTheOsFullSpellingOfEveryCell) {
#ifdef _WIN32
    std::wstring const cells[] = {
        L"C:\\a\\..\\..\\x", L"c:/a/b", L"C:\\", L"C:\\x", L"C:/", L"C:\\a\\.\\b\\", L"C:\\a\\\\b", L"C:\\\\",
        L"C:\\a\\\\", L"C:\\a\\\\\\",
        L"\\\\server\\share\\a\\..\\..\\x", L"//server/share/x", L"\\\\server", L"\\\\server\\", L"\\\\server\\share",
        L"\\\\server\\share\\", L"\\\\server\\share\\..", L"\\\\h\\s\\x.txt.", L"\\\\h\\s\\d.\\x",
        L"\\\\server\\share\\.", L"\\\\server\\share\\..\\", L"\\\\server\\share\\.\\", L"\\\\server\\share\\a\\..",
        L"\\\\server\\share\\a\\..\\", L"\\\\server\\share\\\\", L"\\\\server\\share\\a\\.", L"\\\\server\\share\\a\\",
        L"\\\\.\\C:\\a\\..\\..\\x", L"//./C:/x", L"//?/C:/sub/../x", L"//?/C:/x", L"\\\\.\\C:\\x", L"\\\\.\\C:",
        L"\\\\.\\GLOBALROOT\\Device\\HarddiskVolume3\\..\\x", L"\\\\.\\UNC\\h\\s\\..\\x", L"\\\\.\\Volume{G}\\x",
        L"\\\\.\\C:\\d.\\x.txt", L"\\\\.\\C:\\d\\NUL", L"\\\\.\\pipe\\x", L"\\\\.\\NUL", L"\\\\.\\",
        L"\\\\.\\C:\\..", L"\\\\.\\C:\\.", L"\\\\.\\C:\\d\\..", L"\\\\.\\C:\\d\\..\\", L"\\\\.\\C:\\d\\.", L"\\\\.\\C:\\",
        L"\\\\.\\C:\\d\\", L"\\\\.\\C:\\..\\..", L"\\\\.\\UNC\\h\\s\\..", L"\\\\.\\x\\..",
        L"//?/C:/x/", L"//?/C:/x/.", L"//?/C:/x/..", L"//?/C:/", L"//./C:/x/..",
        L"C:\\d.\\x.txt", L"C:\\d..\\x.txt", L"C:\\d \\x.txt", L"C:\\d. \\x.txt", L"C:\\d .\\x.txt",
        L"C:\\d\\...\\x.txt", L"C:\\x.txt..", L"C:\\x.txt. .", L"C:\\d \\", L"C:\\d.\\", L"C:\\d\\...", L"C:\\d\\   ",
        L"C:\\sub\\.. \\x.txt", L"C:\\sub\\. \\x.txt", L"C:\\d.\\..\\x.txt", L"C:\\d\\..", L"C:\\d\\.",
        L"C:\\d\\x.\\", L"C:\\d\\x. \\", L"C:\\d\\...\\",
        L"C:\\a\\b\\..\\", L"C:\\a\\b\\..", L"C:\\a\\.\\", L"C:\\a\\.", L"C:\\.", L"C:\\..", L"C:\\.\\", L"C:\\..\\",
        L"C:\\a\\b\\..\\.", L"C:\\a\\.\\.\\", L"C:\\a\\..\\b\\.",
        L"C:\\d\\NUL", L"C:\\d\\nul", L"C:\\d\\Nul", L"C:\\d\\NUL.", L"C:\\d\\NUL ", L"C:\\d\\NUL. ", L"C:\\d\\NUL..",
        L"C:\\d\\NUL:", L"C:\\d\\NUL::", L"C:\\d\\NUL.:", L"C:\\d\\NUL :", L"C:\\d\\.\\NUL", L"C:\\d\\x\\..\\NUL",
        L"C:\\NUL", L"C:\\NUL.", L"C:\\d\\NUL\\", L"C:\\d\\NUL\\x",
        L"\\\\h\\s\\NUL", L"\\\\h\\s\\d\\NUL",
        L"C:\\d\\x:stream", L"C:\\d\\x::$DATA",
        L"C:\\" + std::wstring(296, L'a') + L"\\b\\..\\x.c. ",
    };
    std::wstring dir = fs::current_path().native();
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    std::vector<std::wstring> asked(std::begin(cells), std::end(cells));
    for (wchar_t const* relative : {L"a\\.", L"a\\b\\..", L"a\\..", L"..\\", L"a\\\\b", L"a\\..\\..\\b", L"a\\.\\",
                                    L"a\\b\\..\\", L"d\\NUL", L"x.c. "}) {
        asked.push_back(dir + relative);
    }
    for (std::wstring const& cell : asked) {
        SCOPED_TRACE(ascii(cell));
        std::wstring const os = dss::substrate::osFullPathSpelling(cell);
        ASSERT_FALSE(os.empty()) << "the OS resolved no full spelling of the cell";
        std::wstring const ours = dss::core::normalizedSpelling(cell, dss::core::PathGrammar::Win32);
        EXPECT_EQ(ours, os) << "normalized as " << ascii(ours) << ", the OS's " << ascii(os);
    }
#else
    GTEST_SKIP() << "GetFullPathNameW is Win32's: this host has no OS answer to equal (the text rows above hold the "
                    "Win32 grammar here)";
#endif
}
