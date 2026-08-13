// Self-test for the ONE test-side environment override
// (`tests/test_support/scoped_env.hpp`, hoisted at
// `D-TEST-SCOPED-ENV-DUPLICATED-THREE-WAYS` closure).
//
// ★ WHY A HOISTED HELPER EARNS ITS OWN PIN. Its whole job is RESTORING state,
// and a restore that is subtly wrong does not fail here — it fails somewhere
// else, later, in a sibling test that happened to run afterwards in the same
// binary and inherited a mutated environment. That is the "silence instead of a
// verdict" shape: the symptom appears arbitrarily far from the cause. Five
// hand-copied versions of this class existed across the suite and NONE of them
// was ever exercised directly; the restore semantics were assumed, never
// measured. They are measured here.
//
// The distinction the third pin draws is the load-bearing one: a variable that
// was ABSENT must be restored to ABSENT, never to the empty string. Those are
// different states to `std::getenv` (nullptr vs ""), and the callers that use
// the construct-to-CLEAR form exist precisely because they must prove a lookup
// candidate is absent rather than merely empty.

#include "repo_root.hpp"
#include "scoped_env.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using dss::test_support::ScopedEnv;

namespace {

// The variable is named after this file so no real test can collide with it.
constexpr char const* kVar = "DSS_TEST_SCOPED_ENV_PROBE";

[[nodiscard]] char const* raw() { return std::getenv(kVar); }

} // namespace

TEST(ScopedEnvTest, OverrideIsVisibleInsideTheScope) {
    ASSERT_EQ(raw(), nullptr) << "fixture is dirty: the probe variable must "
                                 "start absent";
    {
        ScopedEnv e{kVar, "hello"};
        ASSERT_NE(raw(), nullptr);
        EXPECT_EQ(std::string{raw()}, "hello");
    }
    EXPECT_EQ(raw(), nullptr);
}

TEST(ScopedEnvTest, APriorValueIsRestoredExactly) {
    ScopedEnv outer{kVar, "original"};
    {
        ScopedEnv inner{kVar, "shadowed"};
        ASSERT_NE(raw(), nullptr);
        EXPECT_EQ(std::string{raw()}, "shadowed");
    }
    ASSERT_NE(raw(), nullptr)
        << "the outer value must come BACK, not merely be un-shadowed";
    EXPECT_EQ(std::string{raw()}, "original");
}

// ★★ ABSENT IS NOT EMPTY. Restoring an absent variable to "" would leave every
// later `std::getenv` returning a non-null empty string — which reads as "the
// caller set it to nothing" rather than "the caller never set it", and that is
// exactly the discrimination the construct-to-clear callers depend on.
TEST(ScopedEnvTest, AnAbsentVariableIsRestoredToAbsentNotToEmpty) {
    ASSERT_EQ(raw(), nullptr);
    {
        ScopedEnv e{kVar, "transient"};
        ASSERT_NE(raw(), nullptr);
    }
    EXPECT_EQ(raw(), nullptr)
        << "restored to `"
        << (raw() == nullptr ? std::string{"<null>"} : std::string{raw()})
        << "` — an empty string is a THIRD state no caller means";
}

TEST(ScopedEnvTest, ConstructToClearRemovesAValueAndPutsItBack) {
    ScopedEnv outer{kVar, "present"};
    ASSERT_NE(raw(), nullptr);
    {
        ScopedEnv cleared{kVar};                 // construct-to-CLEAR
        EXPECT_EQ(raw(), nullptr)
            << "construct-to-clear must UNSET, not set-to-empty";
    }
    ASSERT_NE(raw(), nullptr);
    EXPECT_EQ(std::string{raw()}, "present");
}

TEST(ScopedEnvTest, ConstructToClearOnAnAlreadyAbsentVariableIsANoOp) {
    ASSERT_EQ(raw(), nullptr);
    {
        ScopedEnv cleared{kVar};
        EXPECT_EQ(raw(), nullptr);
    }
    EXPECT_EQ(raw(), nullptr);
}

// ── ★★ THE CENSUS: THE DUPLICATION CANNOT COME BACK ─────────────────────────
//
// A hoist with no enforcement is a hoist that gets undone. This suite's own
// history proves it: the row tracking these copies said THREE, and a
// `grep -rn 'class ScopedEnv' tests/` found FIVE — the count was written from
// memory once and never re-measured, so two copies were invisible for as long
// as the row existed.
//
// ── WHAT CHANGED, AND WHY THIS PIN SURVIVED THE DAY IT WENT GREEN ──────────
// It used to carry an EXACT expected list of the two known stragglers
// (`tests/program/test_asm_dialect_per_target.cpp`,
// `tests/program/test_system_dirs_cwd_independent.cpp`) so that repointing
// either one reddened the pin and forced the list to be updated. Both were
// repointed at `D-TEST-SCOPED-ENV-STRAGGLERS-IN-TESTS-PROGRAM` closure and the
// list emptied.
//
// ★★ AN EMPTY EXPECTED LIST IS NOT NOTHING — IT IS THE STRONGEST FORM OF THE
// FIRST HALF OF THE CONTRACT ("a new local copy anywhere under tests/ ⇒ RED").
// Deleting the pin on the day it finally reached its goal would discard the
// guard exactly when it started guarding, which is the same shape as weakening
// a guard because it fired.
//
// ★★ BUT AN EMPTY EXPECTATION DOES INTRODUCE A REAL VACUITY, AND IT IS CLOSED
// HERE RATHER THAN ARGUED AWAY. With a NON-empty expected list, a walk that
// scanned nothing — a broken root lookup, a matcher that stopped matching, a
// directory iterator that yielded no `.cpp` — produced a RED, because the
// expected files were missing from the result. With an empty list that same
// broken walk produces a GREEN: "found nothing" and "looked at nothing" become
// indistinguishable, and the pin rots into decoration without anyone noticing.
// So the walk now carries its own POSITIVE CONTROL: `scoped_env.hpp` is the one
// file that MUST match, and the pin asserts it did. The matcher is therefore
// proven live by the same walk that reports the duplicates, in the same run.
TEST(ScopedEnvTest, TheOnlyDefinitionOfThisHelperIsTheCanonicalOne) {
    const auto root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value())
        << "cannot locate the repo root, so this census would silently pass "
           "having scanned nothing — which is the failure it exists to prevent";
    const auto testsDir = *root / "tests";
    ASSERT_TRUE(std::filesystem::exists(testsDir)) << testsDir.string();

    // The DEFINITION a hand-rolled copy inevitably spells — the trailing brace
    // is load-bearing twice over: it keeps `using dss::test_support::ScopedEnv;`
    // out of the result set, and it keeps every PROSE mention of the class out
    // too (this file is full of them).
    //
    // ★ AND THE LITERAL IS SPLIT ACROSS TWO TOKENS ON PURPOSE. Concatenation
    // happens at translation, so the matcher's own bytes never appear
    // contiguously in this file's source. ✔MEASURED: written as one token, the
    // census matched ITSELF and reported this file as a duplicate. The
    // tempting fix — adding this file to `expected` — would have been the guard
    // re-cut to fit its first firing; the honest fix is a matcher that cannot
    // see a mention. Note there is deliberately NO path-based self-exemption
    // here: a census that skips a file is a census with a blind spot.
    constexpr std::string_view kDecl = "class Scoped" "Env {";

    std::vector<std::string> found;
    bool                     sawCanonical = false;
    std::size_t              scanned      = 0;
    std::error_code ec;
    for (auto const& entry :
         std::filesystem::recursive_directory_iterator(testsDir, ec)) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") continue;
        std::ifstream in{entry.path(), std::ios::binary};
        if (!in) continue;
        ++scanned;
        std::string text{std::istreambuf_iterator<char>{in}, {}};
        if (text.find(kDecl) == std::string::npos) continue;
        // The canonical definition is the ONE file that is supposed to carry
        // it — and it is NOT skipped before reading, because it is this walk's
        // positive control. Everything else that matches is a duplicate.
        // (There is deliberately NO path-based exemption beyond this one: a
        // census that skips a file is a census with a blind spot, and the
        // census's own file is excluded by the split matcher above rather than
        // by a path test.)
        if (entry.path().filename() == "scoped_env.hpp") {
            sawCanonical = true;
            continue;
        }
        found.push_back(
            std::filesystem::relative(entry.path(), *root).generic_string());
    }
    ASSERT_FALSE(ec) << "the tests/ walk failed: " << ec.message();
    std::sort(found.begin(), found.end());

    // ── the positive control, asserted BEFORE the result it validates ──────
    // Without these two the expectation below is satisfied by a walk that read
    // nothing at all, which is the one way an all-clear census can be a lie.
    EXPECT_GT(scanned, 100u)
        << "only " << scanned
        << " source files were read under tests/ — the walk is not covering "
           "the tree, so the all-clear below would be vacuous";
    EXPECT_TRUE(sawCanonical)
        << "the walk never matched `scoped_env.hpp`, the one file that "
           "DEFINES the helper. Either the matcher no longer matches the "
           "declaration it is looking for, or the canonical header moved — "
           "either way this census can no longer see a duplicate, and a green "
           "result here would mean nothing";

    // ⚠ EXACT, AND IT IS EMPTY. Add nothing here to make a red go away —
    // repoint the offending file at `scoped_env.hpp` instead. The two entries
    // this list used to carry (`tests/program/test_asm_dialect_per_target.cpp`,
    // `tests/program/test_system_dirs_cwd_independent.cpp`) were repointed at
    // `D-TEST-SCOPED-ENV-STRAGGLERS-IN-TESTS-PROGRAM` closure.
    EXPECT_EQ(found, std::vector<std::string>{})
        << "a file under tests/ carries its OWN copy of the helper. Delete the "
           "local class, `#include \"scoped_env.hpp\"`, and add "
           "`using dss::test_support::ScopedEnv;` — the hoisted one is the "
           "only version whose restore semantics are measured.";
}

// An empty override is a legitimate value a caller may want to set, and it must
// survive as an empty STRING rather than being confused with absence. This is
// the mirror of `AnAbsentVariableIsRestoredToAbsentNotToEmpty` and it is what
// makes that pin a discrimination rather than a coincidence.
//
// ⚠ MEASURED PLATFORM DIVERGENCE, stated rather than papered over: Windows
// `_putenv_s(name, "")` is documented to REMOVE the variable, so on Windows an
// empty override is indistinguishable from an unset one. POSIX `setenv(name,
// "", 1)` keeps an empty string. The helper does not pretend to unify them —
// no shipped caller sets an empty value, and inventing a per-platform shim to
// hide a difference nothing depends on would be a fiction in the substrate.
TEST(ScopedEnvTest, AnEmptyOverrideFollowsThePlatformAndSaysSo) {
    ASSERT_EQ(raw(), nullptr);
    {
        ScopedEnv e{kVar, ""};
#ifdef _WIN32
        EXPECT_EQ(raw(), nullptr)
            << "Windows `_putenv_s(name, \"\")` removes the variable";
#else
        ASSERT_NE(raw(), nullptr)
            << "POSIX `setenv(name, \"\", 1)` keeps an empty string";
        EXPECT_EQ(std::string{raw()}, "");
#endif
    }
    EXPECT_EQ(raw(), nullptr);
}
