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

#include "repo_root.hpp"
#include "scoped_env.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

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
// ⚠ Compared against the CANONICALIZED cwd since 2026-08-17: the ctor now
// resolves `originalCwd_` for the same reason it resolves the base (below), so
// the invariant is "the canonical spelling of the cwd at ctor", not "whatever
// byte-string `current_path()` happened to return".
TEST(ScratchDirSubstrate, OriginalCwdMatchesCurrentPathAtCtor) {
    auto const cwdAtCtor = dss::test_support::canonicalizeLikeTheProduct(
        fs::current_path());
    ScratchDir sd{Location::InsideRepo, "scratch-dir-self-test"};
    EXPECT_EQ(sd.originalCwd(), cwdAtCtor);
}

// ─────────────────────────────────────────────────────────────────────────────
// PATH-SPELLING PINS — added 2026-08-17 after `windows-msvc-release` CI failed
// 2 tests / 5 cases that were green on ALL FOUR local legs.
//
// The defect was never in the product. `temp_directory_path()` returns whatever
// `TMP`/`TEMP` holds, and GitHub's Windows runner holds the 8.3 SHORT spelling
// (`C:/Users/RUNNER~1/...` for user `runneradmin`); the product resolves paths
// with `fs::weakly_canonical` and so REPORTS the long spelling. Assertions of
// the form `message.find(fixturePath) != npos` then compared two spellings of
// one directory and correctly found no match.
//
// ★★ WHY A PIN AND NOT JUST A FIX: the local gate is structurally incapable of
// catching this. 8.3 shortening applies only to a component longer than 8
// characters, and the local user directory is `rafae` (5) — short spelling ==
// long spelling, so every such assertion is trivially satisfied here and would
// stay green through any regression. A fix with no pin would be re-broken by
// the next person who adds a fixture path, and re-discovered by CI.
// ─────────────────────────────────────────────────────────────────────────────

// The invariant the ctor now establishes, stated directly and host-independent:
// a scratch path is a FIXED POINT of the resolution the product applies. True on
// every platform; the adversarial pin below is what gives it teeth.
TEST(ScratchDirSubstrate, ScratchPathIsAFixedPointOfCanonicalization) {
    for (auto loc : {Location::Temp, Location::InsideRepo}) {
        ScratchDir sd{loc, "scratch-dir-self-test"};
        EXPECT_EQ(sd.path(),
                  dss::test_support::canonicalizeLikeTheProduct(sd.path()))
            << "a scratch path that is not canonical will not string-match a "
               "product diagnostic naming the same directory; loc="
            << static_cast<int>(loc);
        EXPECT_EQ(sd.originalCwd(),
                  dss::test_support::canonicalizeLikeTheProduct(sd.originalCwd()));
    }
}

namespace {

// A spelling of `dir` that names the SAME directory by a different byte string,
// or empty when this platform/volume cannot produce one. Windows: the 8.3 short
// name (what the CI runner actually hands us). POSIX: a symlink, which is the
// same hazard class — `weakly_canonical` resolves both.
[[nodiscard]] fs::path divergentSpellingOf(fs::path const& dir) {
#ifdef _WIN32
    std::wstring const  in = dir.wstring();
    DWORD const         n  = ::GetShortPathNameW(in.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring out(n, L'\0');
    DWORD const written = ::GetShortPathNameW(in.c_str(), out.data(), n);
    if (written == 0 || written >= n) return {};
    out.resize(written);
    return fs::path{out};
#else
    fs::path const  link = dir.parent_path() / (dir.filename().string() + "-link");
    std::error_code ec;
    fs::remove(link, ec);
    ec.clear();
    fs::create_directory_symlink(dir, link, ec);
    return ec ? fs::path{} : link;
#endif
}

}  // namespace

// THE ADVERSARIAL PIN. Builds the divergence itself rather than waiting for a
// host that has one — the CI failure this replaces was invisible locally for
// exactly that reason.
//
// ⚠ FAIL-CLOSED, and this is the part that matters: the test ASSERTS the
// divergent spelling actually materialised and differs byte-wise before relying
// on it. A volume with 8.3 generation disabled, or a POSIX host that refuses the
// symlink, must SKIP LOUDLY — never pass quietly, which would make this pin read
// as coverage it does not have.
TEST(ScratchDirSubstrate, ShortPathSpellingIsDefeatedAtTheChokepoint) {
    std::error_code ec;
    fs::path const  probe = fs::temp_directory_path(ec)
                         / "dss-scratch-spelling-probe-longname";
    ASSERT_FALSE(ec) << "temp_directory_path failed: " << ec.message();
    fs::create_directories(probe, ec);
    ASSERT_FALSE(ec) << "could not create probe dir: " << ec.message();

    fs::path const divergent = divergentSpellingOf(probe);
    if (divergent.empty() || divergent == probe) {
        fs::remove_all(probe, ec);
        GTEST_SKIP() << "this volume produces no divergent spelling for '"
                     << probe.generic_string()
                     << "' (8.3 generation disabled, or symlink refused), so "
                        "the hazard cannot be staged here. NOT a pass — the "
                        "pin simply has no experiment to run on this host.";
    }
    // The experiment is only meaningful if the two spellings genuinely differ
    // AND genuinely name the same directory. Assert both.
    ASSERT_NE(divergent.generic_string(), probe.generic_string());
    ASSERT_EQ(dss::test_support::canonicalizeLikeTheProduct(divergent),
              dss::test_support::canonicalizeLikeTheProduct(probe))
        << "the staged spelling does not resolve to the probe directory, so "
           "this test would be proving nothing";

    {
        // Point the temp lookup at the divergent spelling. Windows consults TMP
        // then TEMP; POSIX consults TMPDIR. Set all three so the experiment does
        // not depend on which one this implementation reads first.
        dss::test_support::ScopedEnv tmp{"TMP", divergent.string()};
        dss::test_support::ScopedEnv temp{"TEMP", divergent.string()};
        dss::test_support::ScopedEnv tmpdir{"TMPDIR", divergent.string()};

        ScratchDir sd{Location::Temp, "scratch-dir-self-test"};

        EXPECT_EQ(sd.path(),
                  dss::test_support::canonicalizeLikeTheProduct(sd.path()))
            << "ctor handed back a non-canonical path under a divergent TEMP "
               "spelling — this is the exact CI failure, reproduced";
        EXPECT_TRUE(fs::is_directory(sd.path()))
            << "canonicalizing must not break the directory that was created";

        // The decisive assertion: the divergent spelling must be GONE. Comparing
        // fixed-point-ness alone could be satisfied by a path that never went
        // through the divergent root at all.
        std::string const leaf = divergent.filename().generic_string();
        EXPECT_EQ(sd.path().generic_string().find(leaf), std::string::npos)
            << "the divergent spelling '" << leaf << "' survived into the "
            << "scratch path '" << sd.path().generic_string()
            << "'; a product diagnostic naming this directory will not "
               "string-match it";
    }

    fs::remove_all(probe, ec);
#ifndef _WIN32
    fs::remove(divergent, ec);
#endif
}

namespace {

// `canonicalizeLikeTheProduct` is a deliberate COPY of the product's resolver
// (see the rationale on the function itself). A copy with no pin is how the two
// drift apart silently, so this reads the product's own text and asserts the
// two still agree on WHICH resolution is being performed.
[[nodiscard]] std::string productCanonicalizerBody() {
    fs::path const  src = dss::test::repoRoot() / "src" / "program"
                       / "dependency_resolver.cpp";
    std::ifstream in{src};
    if (!in) return {};
    std::string const text{std::istreambuf_iterator<char>{in},
                           std::istreambuf_iterator<char>{}};

    // The body between `canonicalize(fs::path const& p) {` and its closing
    // brace at column 0 — narrow enough that an unrelated `weakly_canonical`
    // elsewhere in the file cannot satisfy this pin.
    constexpr std::string_view kSig = "canonicalize(fs::path const& p) {";
    auto const                 at   = text.find(kSig);
    if (at == std::string::npos) return {};
    auto const end = text.find("\n}", at);
    return text.substr(at, end == std::string::npos ? std::string::npos
                                                    : end - at);
}

}  // namespace

TEST(ScratchDirSubstrate, MatchesTheProductsOwnCanonicalizer) {
    std::string const body = productCanonicalizerBody();
    ASSERT_FALSE(body.empty())
        << "could not locate `canonicalize` in src/program/dependency_resolver."
           "cpp. If it was renamed or moved, re-point this pin AND re-check "
           "that `canonicalizeLikeTheProduct` still mirrors it — do not delete "
           "this test to make the build green.";

    EXPECT_NE(body.find("weakly_canonical"), std::string::npos)
        << "the product no longer resolves with `weakly_canonical`, but the "
           "test fixture still does. Every fixture path in the tree is now "
           "spelled differently from what the product reports — the exact "
           "2026-08-17 CI failure, in the other direction.";
    EXPECT_NE(body.find("lexically_normal"), std::string::npos)
        << "the product dropped its degraded-key fallback; "
           "`canonicalizeLikeTheProduct` still has one.";
}

// The scanner's own positive control — an all-clear from a reader that reads
// nothing is worthless, and this repo has been burned by exactly that before.
TEST(ScratchDirSubstrate, TheCanonicalizerScannerActuallyDetects) {
    std::string const body = productCanonicalizerBody();
    ASSERT_FALSE(body.empty());
    std::string mutated = body;
    for (auto at = mutated.find("weakly_canonical");
         at != std::string::npos;
         at      = mutated.find("weakly_canonical", at + 1)) {
        mutated.replace(at, std::string_view{"weakly_canonical"}.size(),
                        "absolute________");
    }
    ASSERT_NE(mutated, body) << "the mutation did not land, so this control "
                                "proves nothing about the scanner";
    EXPECT_EQ(mutated.find("weakly_canonical"), std::string::npos)
        << "the matcher used by the pin above still finds the token in a text "
           "that no longer contains it";
}
