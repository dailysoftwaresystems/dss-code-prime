// ONE RETRY, BOUNDED BY ITS CALLER, LOUD WHEN IT RUNS OUT (cycle P69 L2, lane `lm`):
// `substrate::retryUpTo`.
//
// Two hand-written loops tried 64 directory names each and answered "empty" when they ran out,
// with nothing saying why. A retry is not a walk over a structure, so nothing bounds it but its
// caller, who passes the bound in. These cases pin that the first answer wins, that an exhaustion
// names the operation, the attempts and the last reason, and that an exhaustion nobody read stops
// the process, naming the operation.

#include "core/substrate/bounded_retry.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace {

using dss::substrate::retryUpTo;

// An attempt that answers on its `succeedOn`-th call (0-based), and says why not before.
struct Flaky {
    std::size_t  succeedOn;
    std::size_t* calls;
    std::optional<int> operator()(std::size_t index, std::string& whyNot) const {
        ++*calls;
        if (index == succeedOn) return static_cast<int>(index) * 10;
        whyNot = "attempt " + std::to_string(index) + " found the name taken";
        return std::nullopt;
    }
};

}  // namespace

TEST(BoundedRetry, TheFirstAttemptThatAnswersWinsAndNoLaterOneRuns) {
    std::size_t calls = 0;
    auto made = retryUpTo(64, "making a fresh staging directory", Flaky{2, &calls});
    ASSERT_TRUE(made.succeeded());
    EXPECT_EQ(made.value(), 20);
    EXPECT_EQ(made.attemptsMade(), 3u);
    EXPECT_EQ(calls, 3u);
}

TEST(BoundedRetry, AnExhaustionNamesTheOperationTheAttemptsAndTheLastReason) {
    std::size_t calls = 0;
    auto made = retryUpTo(4, "making a fresh staging directory", Flaky{99, &calls});
    ASSERT_FALSE(made.succeeded());
    EXPECT_EQ(calls, 4u) << "every attempt the caller allowed is made, and no more";
    EXPECT_EQ(made.attemptsMade(), 4u);
    EXPECT_EQ(made.failure(),
              "making a fresh staging directory: no attempt of 4 succeeded (the last: attempt 3 found "
              "the name taken)");
}

TEST(BoundedRetry, AnAttemptThatGivesNoReasonIsSaidToHaveGivenNone) {
    auto made = retryUpTo(2, "claiming a capture slot",
                          [](std::size_t, std::string&) -> std::optional<int> { return std::nullopt; });
    ASSERT_FALSE(made.succeeded());
    EXPECT_EQ(made.failure(), "claiming a capture slot: no attempt of 2 succeeded (the last: it gave no reason)");
}

// A moved outcome carries what it owes: the moved-from one says nothing when it goes, the
// moved-to one is read.
TEST(BoundedRetry, AMovedExhaustionIsOwedByWhereItWasMoved) {
    std::size_t calls = 0;
    auto first = retryUpTo(1, "claiming a capture slot", Flaky{99, &calls});
    auto moved = std::move(first);
    EXPECT_FALSE(moved.succeeded());
    EXPECT_FALSE(moved.failure().empty());
}

TEST(BoundedRetry, AnExhaustionNobodyReadStopsTheProcessNamingTheOperation) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            std::size_t calls = 0;
            auto made = retryUpTo(3, "making a fresh staging directory", Flaky{99, &calls});
            (void)made.succeeded();   // looked at, but the failure was never read
        },
        "dss::substrate fatal: making a fresh staging directory: 3 attempts failed and nobody read why");
}

TEST(BoundedRetry, ItsContractIsHeldLoud) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            std::size_t calls = 0;
            auto made = retryUpTo(0, "claiming a capture slot", Flaky{0, &calls});
            (void)made.failure();
        },
        "dss::substrate fatal: claiming a capture slot: a retry of no attempts was asked for");
    EXPECT_DEATH(
        {
            std::size_t calls = 0;
            auto made = retryUpTo(2, "claiming a capture slot", Flaky{99, &calls});
            (void)made.failure();
            (void)made.value();
        },
        "dss::substrate fatal: claiming a capture slot: the value of a retry that ran out was asked for");
    EXPECT_DEATH(
        {
            std::size_t calls = 0;
            auto made = retryUpTo(2, "claiming a capture slot", Flaky{0, &calls});
            (void)made.failure();
        },
        "dss::substrate fatal: claiming a capture slot: the failure of a retry that succeeded was asked for");
}
