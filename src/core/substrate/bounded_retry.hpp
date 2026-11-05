#pragma once

// ── ONE RETRY: BOUNDED BY ITS CALLER, LOUD WHEN IT RUNS OUT (cycle P69 L2, lane `lm`) ──────────
//
// Some work is tried again until it succeeds: a fresh directory name is drawn until one does not
// exist yet. Such a loop is not a walk over a structure. Nothing it walks bounds it, so its bound
// is the caller's to choose and is passed in. What must never happen is the loop running out in
// SILENCE. Two hand-written loops of 64 attempts each returned an empty answer when they ran out,
// and nothing said why.
//
// `retryUpTo(attempts, operation, attempt)` calls `attempt(index, whyNot)` until it answers, at
// most `attempts` times. The outcome holds the answer, or a sentence naming the operation, the
// attempts and the last attempt's reason. An outcome that ran out and whose sentence nobody read
// stops the process when it is destroyed, naming the operation. The caller decides what an
// exhaustion means (a refusal, a fallback it states), but it cannot drop one unread.
//
// Usage:
//
//   auto made = substrate::retryUpTo(64, "making a fresh staging directory",
//       [&](std::size_t, std::string& whyNot) -> std::optional<fs::path> { … });
//   if (!made.succeeded()) return refuse(made.failure());
//   use(made.value());

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace dss::substrate {

// The ONE answer of an exhausted retry nobody read, shaped like the arena fatals
// (`arena_tag.hpp`): the operation's name first.
[[noreturn]] inline void failOnUnreadRetryExhaustion(char const* operation, std::size_t attempts) {
    std::fprintf(stderr, "dss::substrate fatal: %s: %zu attempts failed and nobody read why\n",
                 operation, attempts);
    std::abort();
}

// A retry used against its contract: no attempt asked for, the value of an exhausted retry, the
// failure of a successful one.
[[noreturn]] inline void failOnRetryMisuse(char const* operation, char const* what) {
    std::fprintf(stderr, "dss::substrate fatal: %s: %s\n", operation, what);
    std::abort();
}

template <class T>
class [[nodiscard]] RetryOutcome {
public:
    RetryOutcome(RetryOutcome&& other) noexcept
        : value_{std::move(other.value_)}, failure_{std::move(other.failure_)},
          operation_{other.operation_}, attempts_{other.attempts_}, read_{other.read_} {
        other.read_ = true;   // the moved-from outcome owes nothing
    }
    RetryOutcome& operator=(RetryOutcome&&) = delete;
    RetryOutcome(RetryOutcome const&) = delete;
    RetryOutcome& operator=(RetryOutcome const&) = delete;

    ~RetryOutcome() {
        if (!value_.has_value() && !read_) failOnUnreadRetryExhaustion(operation_, attempts_);
    }

    [[nodiscard]] bool succeeded() const noexcept { return value_.has_value(); }
    // The attempts made: the one that answered, or every one.
    [[nodiscard]] std::size_t attemptsMade() const noexcept { return attempts_; }

    [[nodiscard]] T& value() {
        if (!value_.has_value()) failOnRetryMisuse(operation_, "the value of a retry that ran out was asked for");
        return *value_;
    }

    // Why it ran out: the operation, the attempts and the last attempt's reason. Reading it is
    // what discharges the exhaustion.
    [[nodiscard]] std::string const& failure() {
        if (value_.has_value()) failOnRetryMisuse(operation_, "the failure of a retry that succeeded was asked for");
        read_ = true;
        return failure_;
    }

private:
    template <class Attempt>
    friend auto retryUpTo(std::size_t, char const*, Attempt&&);

    RetryOutcome(std::optional<T> value, std::string failure, char const* operation, std::size_t attempts)
        : value_{std::move(value)}, failure_{std::move(failure)}, operation_{operation},
          attempts_{attempts} {}

    std::optional<T> value_;
    std::string      failure_;
    char const*      operation_;
    std::size_t      attempts_;
    bool             read_ = false;
};

// Calls `attempt(index, whyNot)` — `index` counting from 0, `whyNot` for the attempt to say why it
// did not answer — until it returns an engaged `std::optional<T>`, at most `attempts` times.
template <class Attempt>
[[nodiscard]] auto retryUpTo(std::size_t attempts, char const* operation, Attempt&& attempt) {
    using Answer = std::invoke_result_t<Attempt&, std::size_t, std::string&>;
    using T      = typename Answer::value_type;
    if (attempts == 0) failOnRetryMisuse(operation, "a retry of no attempts was asked for");
    std::string whyNot;
    for (std::size_t i = 0; i < attempts; ++i) {
        whyNot.clear();
        Answer answer = attempt(i, whyNot);
        if (answer.has_value()) return RetryOutcome<T>{std::move(answer), {}, operation, i + 1};
    }
    std::string failure = std::string{operation} + ": no attempt of " + std::to_string(attempts)
                          + " succeeded (the last: "
                          + (whyNot.empty() ? std::string{"it gave no reason"} : whyNot) + ")";
    return RetryOutcome<T>{std::nullopt, std::move(failure), operation, attempts};
}

} // namespace dss::substrate
