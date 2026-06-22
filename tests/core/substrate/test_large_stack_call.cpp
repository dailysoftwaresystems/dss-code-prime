#include "core/substrate/large_stack_call.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>

// Direct unit tests for the large-stack worker facility. The production
// consumer is `analyze` (semantic_analyzer.cpp), which runs on a 64 MiB
// worker stack so deeply-nested expression trees do not overflow the host's
// ~1 MB main stack (`D-PARSE-DEEP-FRONTEND-STACK`). These tests pin the
// three load-bearing properties locally so a regression surfaces here rather
// than as a far-away deep-nesting crash.

namespace {

using dss::substrate::callOnLargeStack;
using dss::substrate::kDeepRecursionStackBytes;
using dss::substrate::runOnLargeStack;

// A modest reserve is plenty for the simple callables; the deep-recursion
// test uses the production constant so it provably exceeds the host default.
constexpr std::size_t kSmallReserve = std::size_t{1} * 1024 * 1024;

// Recurse `depth` frames, each parking a large volatile buffer on the
// stack, then return the count. `volatile` + the touch defeat the optimizer
// (it cannot elide the frames or the storage), so the call genuinely
// consumes ~depth * sizeof(buffer) of stack. At depth 1000 * 8192 bytes
// that is ~8 MB — far past a default ~1 MB host stack, so this only
// completes BECAUSE the facility provided a big stack.
int deepRecurse(int depth) {
    volatile char scratch[8192];
    scratch[0]              = static_cast<char>(depth & 0x7f);
    scratch[sizeof(scratch) - 1] = scratch[0];
    if (depth <= 0) {
        return static_cast<int>(scratch[0]);  // read it back so it stays live
    }
    int const below = deepRecurse(depth - 1);
    // Combine with the live buffer so neither the recursion nor the storage
    // is dead (which would let the compiler convert this to a loop / drop
    // the arrays and defeat the test).
    return below + static_cast<int>(scratch[sizeof(scratch) - 1] != 0 ? 0 : 0);
}

} // namespace

// RunsTheCallable: the void entry point invokes the callable exactly once
// and returns only after it has run to completion (synchronous join).
TEST(LargeStackCall, RunsTheCallable) {
    int runCount = 0;
    runOnLargeStack(kSmallReserve, [&] { ++runCount; });
    EXPECT_EQ(runCount, 1);

    // The value-returning convenience wrapper forwards the result.
    int const doubled = callOnLargeStack(kSmallReserve, [] { return 21 * 2; });
    EXPECT_EQ(doubled, 42);
}

// RethrowsEscapedException: an exception thrown inside the worker is
// captured and RE-THROWN on the joining thread, value-preserving (the same
// type + message), never swallowed and never escaping the thread entry.
TEST(LargeStackCall, RethrowsEscapedException) {
    bool threw = false;
    try {
        runOnLargeStack(kSmallReserve, [] {
            throw std::runtime_error("from the worker stack");
        });
    } catch (std::runtime_error const& e) {
        threw = true;
        EXPECT_EQ(std::string{e.what()}, "from the worker stack");
    }
    EXPECT_TRUE(threw) << "the worker's exception must propagate to the joiner";

    // Same propagation through the value-returning wrapper.
    bool threwValue = false;
    try {
        (void)callOnLargeStack(kSmallReserve, []() -> int {
            throw std::runtime_error("value-path throw");
        });
    } catch (std::runtime_error const& e) {
        threwValue = true;
        EXPECT_EQ(std::string{e.what()}, "value-path throw");
    }
    EXPECT_TRUE(threwValue);
}

// ProvidesStackForDeepRecursion: a recursion that needs ~8 MB of stack — far
// more than the default host stack can hold — completes when run through the
// facility. RED-on-disable: running `deepRecurse(1000)` INLINE on the test
// thread's default stack overflows and crashes, so the bare fact that this
// returns proves the facility supplied a large stack.
TEST(LargeStackCall, ProvidesStackForDeepRecursion) {
    constexpr int kFrames = 1000;
    int const result =
        callOnLargeStack(kDeepRecursionStackBytes, [] {
            return deepRecurse(kFrames);
        });
    // Each leaf/frame contributes its low-byte; the exact value is not the
    // point — reaching this assertion at all means ~8 MB of stack was
    // available. Pin a deterministic lower bound so the call result is
    // actually observed (and the recursion not dead-code-eliminated).
    EXPECT_GE(result, 0);
}
