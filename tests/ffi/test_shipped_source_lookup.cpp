// Shipped-source resolution reports WHY it failed, and never invents an absence.
//
// ★★ WHAT THIS PINS, AND WHY A MESSAGE TEST IS THE RIGHT SHAPE HERE. The defect was
// not a wrong branch; it was a RETURN TYPE that could not carry the difference.
// `resolveShippedSourcePath` returned `optional<path>` and collapsed three outcomes
// into one `nullopt` -- config root undiscovered, nothing at the path, and "the
// filesystem could not answer" -- after which all three callers stated ABSENCE as
// fact. ✔MEASURED 2026-08-25 on the Windows gate under concurrent load: two entries
// reported `runtime/platform/src/unistd.c` and `dirent.c` as files that are not
// there, while both were present, regular and readable seconds later.
//
// ⚠ So the load-bearing assertion is a NEGATIVE one: the QueryFailed message must not
// claim the file is missing. A test that only checked "the status enum round-trips"
// would pass with the old wording restored and pin nothing a user can see.
//
// ⓘ QueryFailed is exercised by CONSTRUCTING the lookup rather than by making the
// filesystem fail. Provoking a real EACCES/sharing-violation portably (Windows +
// Linux + macOS, in-process, without elevation) is not reliable, and a test that
// silently degrades to "could not provoke it, so pass" is the vacuous-skip this
// project refuses. The struct is public precisely so the message is testable.

#include "ffi/shipped_lib_descriptor.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using dss::ffi::describeShippedSourceLookup;
using dss::ffi::resolveShippedSource;
using dss::ffi::ShippedSourceLookup;
using dss::DiagnosticCode;
using dss::ffi::ShippedSourceResolution;

[[nodiscard]] bool containsCI(std::string const& haystack, std::string_view needle) {
    auto const lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    return lower(haystack).find(lower(std::string{needle})) != std::string::npos;
}

// ── the arm the whole change exists for ──────────────────────────────────────
TEST(ShippedSourceLookup, AQueryFailureNeverReportsTheFileAsMissing) {
    ShippedSourceLookup look;
    look.status = ShippedSourceResolution::QueryFailed;
    look.path   = "C:/anywhere/runtime/platform/src/unistd.c";
    look.error  = std::make_error_code(std::errc::permission_denied);

    std::string const msg = describeShippedSourceLookup(look, "runtime/platform/src/unistd.c");

    // The exact sentence a reader was sent hunting by. It must be gone.
    EXPECT_FALSE(containsCI(msg, "no file exists"))
        << "a file the filesystem could not be asked about was reported as ABSENT: " << msg;
    EXPECT_FALSE(containsCI(msg, "no readable file is there"))
        << "the pre-fix wording is back: " << msg;
    // And it must say what actually happened, including the OS's own reason.
    EXPECT_TRUE(containsCI(msg, "i/o failure")) << msg;
    EXPECT_TRUE(containsCI(msg, "not a missing file")) << msg;
    EXPECT_TRUE(containsCI(msg, look.error.message())) << msg;
}

// ── and the arm that keeps the fix from over-correcting ──────────────────────
// A genuine absence must still be reported AS an absence. A change that made every
// failure read "could not be examined" would pass the arm above and be just as wrong
// in the other direction.
TEST(ShippedSourceLookup, AGenuineAbsenceIsStillReportedAsOne) {
    auto const look = resolveShippedSource(
        "runtime/platform/src/dss-no-such-runtime-unit-exists.c");
    ASSERT_EQ(look.status, ShippedSourceResolution::NotPresent)
        << describeShippedSourceLookup(look, "<absent>");
    std::string const msg = describeShippedSourceLookup(look, "<absent>");
    EXPECT_TRUE(containsCI(msg, "no file exists")) << msg;
    EXPECT_FALSE(containsCI(msg, "i/o failure")) << msg;
}

TEST(ShippedSourceLookup, ARealShippedUnitResolvesToARegularFile) {
    auto const look = resolveShippedSource("runtime/platform/src/unistd.c");
    ASSERT_TRUE(look.resolved())
        << "the shipped runtime unit this project's own gate compiles did not "
           "resolve: " << describeShippedSourceLookup(look, "runtime/platform/src/unistd.c");
    EXPECT_FALSE(look.path.empty());
    EXPECT_EQ(look.error, std::error_code{});
}

// A DIRECTORY is not a missing file either -- the third way the old `nullopt` lied.
TEST(ShippedSourceLookup, ADirectoryIsNotReportedAsAMissingFile) {
    auto const look = resolveShippedSource("runtime/platform/src");
    ASSERT_EQ(look.status, ShippedSourceResolution::NotAFile)
        << describeShippedSourceLookup(look, "runtime/platform/src");
    EXPECT_TRUE(containsCI(describeShippedSourceLookup(look, "runtime/platform/src"),
                           "not a regular file"));
}

// ── the resolver's QueryFailed branch, exercised for REAL where it is reachable ──
// An over-long component exceeds NAME_MAX, which is a query FAILURE rather than an
// absence -- but only where the platform says so.
// ✔MEASURED 2026-08-25 ON ALL THREE PLATFORMS, which is why this arm asserts a
// DISJUNCTION rather than one status:
//   Linux/libstdc++   over-long -> type=none, ec=36 ENAMETOOLONG  => QueryFailed
//   macOS/AppleClang  over-long -> type=none, ec=63 ENAMETOOLONG  => QueryFailed
//   Windows/MinGW     over-long -> type=not_found, ec=2           => NotPresent
// So the resolver's QueryFailed branch is pinned FOR REAL on the WSL, arm64 and macOS
// legs; Windows is the sole outlier because its `status()` maps path-SHAPE problems to
// not_found rather than to an error. The three-way split was measured, not assumed --
// macOS was checked on the operator's own Mac rather than inferred from Linux.
// ★ A plain absence, a directory and a regular file classify IDENTICALLY on all three
// (not_found / directory / regular), so the other arms pin the same behaviour everywhere.
// ⚠ Both outcomes are correct; asserting either one alone would make this test red on
// half the gate for no defect. What is asserted UNCONDITIONALLY is the property that
// matters: whatever the status, the description must agree with it -- so the arm can
// never pass by reporting an I/O failure as an absence.
TEST(ShippedSourceLookup, AnUnaskableQueryIsNeverDressedUpAsAnAbsence) {
    std::string const overLong(300, 'x');
    auto const        look = resolveShippedSource(overLong + ".c");
    std::string const msg  = describeShippedSourceLookup(look, overLong + ".c");

    ASSERT_FALSE(look.resolved()) << msg;
    ASSERT_TRUE(look.status == ShippedSourceResolution::QueryFailed
                || look.status == ShippedSourceResolution::NotPresent)
        << "unexpected status " << static_cast<int>(look.status) << ": " << msg;

    if (look.status == ShippedSourceResolution::QueryFailed) {
        EXPECT_NE(look.error, std::error_code{})
            << "QueryFailed must carry the reason it failed: " << msg;
        EXPECT_FALSE(containsCI(msg, "no file exists"))
            << "a query failure was reported as an absence: " << msg;
        EXPECT_TRUE(containsCI(msg, "not a missing file")) << msg;
    } else {
        EXPECT_TRUE(containsCI(msg, "no file exists")) << msg;
    }
}

// ── the CODE, which is a claim in the same way the message is ────────────────
// Anything filtering, counting or suppressing `D_FileNotFound` was counting I/O
// failures as missing files; no wording change reaches that, so the code is pinned
// separately from the sentence.
TEST(ShippedSourceLookup, OnlyAQueryFailureEarnsTheReadFailureCode) {
    auto codeFor = [](ShippedSourceResolution s) {
        ShippedSourceLookup look;
        look.status = s;
        return dss::ffi::diagnosticCodeForShippedSourceLookup(look);
    };
    EXPECT_EQ(codeFor(ShippedSourceResolution::QueryFailed),
              DiagnosticCode::D_FileReadFailed);
    // The other three ARE statements that the body is genuinely not there.
    EXPECT_EQ(codeFor(ShippedSourceResolution::NotPresent),
              DiagnosticCode::D_FileNotFound);
    EXPECT_EQ(codeFor(ShippedSourceResolution::NoConfigRoot),
              DiagnosticCode::D_FileNotFound);
    EXPECT_EQ(codeFor(ShippedSourceResolution::NotAFile),
              DiagnosticCode::D_FileNotFound);
}

// The four statuses must not share a sentence: a caller that prints the description
// verbatim is the only thing a user sees, so two outcomes reading alike is the same
// defect in a new place.
TEST(ShippedSourceLookup, EachOutcomeSaysSomethingDifferent) {
    auto describe = [](ShippedSourceResolution s) {
        ShippedSourceLookup look;
        look.status = s;
        look.path   = "C:/anywhere/x.c";
        look.error  = std::make_error_code(std::errc::io_error);
        return describeShippedSourceLookup(look, "x.c");
    };
    std::string const a = describe(ShippedSourceResolution::NoConfigRoot);
    std::string const b = describe(ShippedSourceResolution::NotPresent);
    std::string const c = describe(ShippedSourceResolution::NotAFile);
    std::string const d = describe(ShippedSourceResolution::QueryFailed);
    EXPECT_NE(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
    EXPECT_NE(b, c);
    EXPECT_NE(b, d);
    EXPECT_NE(c, d);
    // NoConfigRoot is a statement about the ENVIRONMENT and must say so rather than
    // implicating the descriptor.
    EXPECT_TRUE(containsCI(a, "environment")) << a;
}

}  // namespace
