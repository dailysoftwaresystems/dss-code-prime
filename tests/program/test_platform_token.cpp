// The host-platform token vocabulary — `src/program/platform_token.hpp`.
//
// The subject is a THREE-WORD closed vocabulary (`windows` / `linux` /
// `darwin`) shared by the `.dss-project.json` build-script gate
// (`"runOn": [...]`), the 1868 `runOn` entries across the example manifests,
// and the corpus runners' host chokepoint. Three words look too small to test.
// They are not, because every way this can break is silent:
//
//   * A CASE-INSENSITIVE compare would accept `"Linux"` in a project file. The
//     gate would then fire on a host the author never sanctioned, or — with the
//     comparison written the other way — never fire at all, and a build that
//     quietly skips its pre-build script produces a subtly wrong artifact with
//     no diagnostic anywhere.
//   * An ALIAS (`macos`, `osx`, `win32`) accepted "for convenience" splits the
//     corpus vocabulary in two: two spellings for one host, one of which
//     `currentHostOs()` never returns, so half the manifests silently stop
//     matching.
//   * A FOURTH TOKEN added without a matching `currentHostOs()` arm, or an arm
//     added without a token, makes the two halves disagree — and the disagreement
//     shows up as an unexplained skip, never as an error.
//
// So the pins here are deliberately literal: an exact membership TABLE (the
// three that must pass and the eight near-misses that must fail), byte-exact
// equality on the diagnostic list, a cardinality `static_assert` that turns
// silent vocabulary growth into a BUILD error, and the cross-pin that
// `isValidRunOnToken(currentHostOs())` — the validator and the host detector —
// can never disagree.
//
// RED-ON-DISABLE (the lever these pins hang on): change the `==` inside
// `isValidRunOnToken` to any case-folding compare and `RejectsNearMissSpellings`
// / `MembershipIsConstexprAndCaseSensitive` go red on `"Windows"`, `"LINUX"` and
// `"macOS"` — the last as a compile error, since the membership table is also
// asserted at compile time. Add a fourth entry to `kRunOnPlatformTokens` and the
// `static_assert` below fails the BUILD before any test runs. Hand-write
// `runOnTokenList()`'s string instead of deriving it and `TokenListIsDerived`
// catches the drift the moment the array and the message disagree.

#include "program/platform_token.hpp"

// The de-duplication this vocabulary paid for: `dss::test_support::currentHostOs()`
// is now a forwarder, and `ForwarderAgreesWithSourceOfTruth` is what keeps it one.
#include "arm_verdict_ledger.hpp"

#include <gtest/gtest.h>

#include <cstddef>   // std::size_t
#include <iterator>  // std::size — the cardinality pin
#include <string>
#include <string_view>

using dss::currentHostOs;
using dss::isValidRunOnToken;
using dss::kRunOnPlatformTokens;
using dss::runOnTokenList;

// ── Cardinality: silent vocabulary growth is a BUILD error ──────────────────
//
// Not an EXPECT. A fourth token is a repo-wide decision (every manifest, both
// corpus runners, the sqlite harness drivers and this file's expectations all
// speak this vocabulary), so it must stop the compile at the moment it is
// typed rather than surface as one red ctest entry among hundreds.
static_assert(std::size(kRunOnPlatformTokens) == 3,
              "The runOn vocabulary is EXACTLY windows/linux/darwin. Adding a "
              "token is a corpus-wide change: update the membership table, "
              "runOnTokenList()'s expected string and currentHostOs()'s #ifdef "
              "ladder in the same commit.");

// The three, in canonical order — the order `runOnTokenList()` prints.
static_assert(kRunOnPlatformTokens[0] == "windows");
static_assert(kRunOnPlatformTokens[1] == "linux");
static_assert(kRunOnPlatformTokens[2] == "darwin");

// ── Membership, at COMPILE time ─────────────────────────────────────────────
//
// `isValidRunOnToken` is `constexpr` so a caller can validate a literal without
// running anything; these assert that property AND the case-sensitivity, so a
// case-folding regression cannot even compile. The runtime table below repeats
// them on purpose — these fail the BUILD, those name the offending spelling in
// the ctest output.
static_assert(isValidRunOnToken("windows"));
static_assert(isValidRunOnToken("linux"));
static_assert(isValidRunOnToken("darwin"));
static_assert(!isValidRunOnToken("Windows"));
static_assert(!isValidRunOnToken("macOS"));
static_assert(!isValidRunOnToken(""));

// ── The exact membership table ──────────────────────────────────────────────

TEST(PlatformToken, AcceptsExactlyTheThreeTokens) {
    EXPECT_TRUE(isValidRunOnToken("windows"));
    EXPECT_TRUE(isValidRunOnToken("linux"));
    EXPECT_TRUE(isValidRunOnToken("darwin"));

    // Every array entry must be accepted by the validator that guards it —
    // an entry the validator rejects would be an unreachable token.
    for (std::string_view const token : kRunOnPlatformTokens) {
        EXPECT_TRUE(isValidRunOnToken(token))
            << "kRunOnPlatformTokens entry '" << token
            << "' is not accepted by its own validator";
    }
}

TEST(PlatformToken, RejectsNearMissSpellings) {
    // CASE. The vocabulary is case-SENSITIVE; these are the spellings a human
    // most plausibly types into a `.dss-project.json` by hand.
    EXPECT_FALSE(isValidRunOnToken("Windows"));
    EXPECT_FALSE(isValidRunOnToken("LINUX"));

    // ALIASES. `darwin` is the kernel name `uname -s` prints and the one the
    // corpus uses; the friendlier product names are NOT synonyms here.
    EXPECT_FALSE(isValidRunOnToken("macos"));
    EXPECT_FALSE(isValidRunOnToken("macOS"));
    EXPECT_FALSE(isValidRunOnToken("osx"));
    EXPECT_FALSE(isValidRunOnToken("win32"));

    // WHITESPACE. A trailing space is what a JSON author's stray keystroke
    // produces, and it must not be silently trimmed into a match: trimming
    // here would mean the config layer and this validator disagree about what
    // the token actually is.
    EXPECT_FALSE(isValidRunOnToken("darwin "));

    // EMPTY. `"runOn": [""]` is a malformed manifest, not a wildcard.
    EXPECT_FALSE(isValidRunOnToken(""));
}

// ── The diagnostic list ─────────────────────────────────────────────────────

TEST(PlatformToken, TokenListIsDerived) {
    // Byte-exact. This string goes verbatim into the rejection diagnostic a
    // developer reads when their `runOn` token is refused, so its separator and
    // ordering are part of the contract, not incidental formatting.
    EXPECT_EQ(runOnTokenList(), "windows, linux, darwin");
}

TEST(PlatformToken, TokenListNamesEveryTokenAndNothingElse) {
    // The list is DERIVED, and this is what proves it stays derived rather than
    // becoming a hand-maintained copy: every array entry must appear in the
    // string, and the string must be exactly as long as the entries plus their
    // separators — so a fourth token cannot be added to the array while the
    // message still lists three, nor a fourth name typed into a literal message
    // that the validator does not implement.
    std::string const list = runOnTokenList();

    std::size_t expectedLength = 0;
    for (std::string_view const token : kRunOnPlatformTokens) {
        EXPECT_NE(list.find(token), std::string::npos)
            << "token '" << token << "' missing from runOnTokenList(): " << list;
        expectedLength += token.size();
    }
    expectedLength += 2 * (std::size(kRunOnPlatformTokens) - 1);  // ", " joins

    EXPECT_EQ(list.size(), expectedLength)
        << "runOnTokenList() carries text beyond the vocabulary it derives "
           "from: " << list;
}

// ── The host detector ───────────────────────────────────────────────────────

TEST(PlatformToken, CurrentHostOsIsAValidToken) {
    // THE CROSS-PIN. The validator and the host detector are two `#ifdef`-free
    // and `#ifdef`-ful halves of one vocabulary; if they ever disagree, every
    // `runOn` gate silently evaluates to "never", which is the failure mode
    // that produces a green run with the work skipped.
    EXPECT_TRUE(isValidRunOnToken(currentHostOs()))
        << "currentHostOs() returned '" << currentHostOs()
        << "', which is not in the runOn vocabulary (" << runOnTokenList() << ")";
}

TEST(PlatformToken, CurrentHostOsNamesThisHost) {
    // The identifiable host. Each leg of the gate runs exactly one of these
    // arms, and between the three CI/host legs all three are live — so a ladder
    // arm that names the wrong OS cannot survive a full matrix run. Written
    // with the same predicates the header's ladder uses, deliberately: any
    // rewrite of that ladder has to keep agreeing with these.
#if defined(_WIN32)
    EXPECT_EQ(currentHostOs(), "windows");
#elif defined(__APPLE__)
    EXPECT_EQ(currentHostOs(), "darwin");
#elif defined(__linux__)
    EXPECT_EQ(currentHostOs(), "linux");
#else
    FAIL() << "unrecognised host — platform_token.hpp should not have compiled";
#endif
}

TEST(PlatformToken, ForwarderAgreesWithSourceOfTruth) {
    // `tests/test_support/arm_verdict_ledger.hpp` deleted its private `#ifdef`
    // ladder and forwards here. This pin is what stops a second ladder from
    // growing back: the two corpus runners decide `SkippedByRunOn` through the
    // forwarder while `.dss-project.json` gates through `dss::currentHostOs()`,
    // and a divergence between them would mean the harness and the driver
    // disagree about what machine they are on.
    EXPECT_EQ(dss::test_support::currentHostOs(), std::string{currentHostOs()});
    EXPECT_TRUE(isValidRunOnToken(dss::test_support::currentHostOs()));
}
