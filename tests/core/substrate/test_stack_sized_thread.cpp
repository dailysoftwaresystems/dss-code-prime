// StackSizedThread — the thread whose stack size DSS states rather than inherits.
//
// ★★ WHY THE FIRST TEST ASKS FOR 64 MiB AND THEN TOUCHES 16 MiB OF IT:
// so that it is RED ON EVERY HOST if the size is not really applied. The
// documented secondary-thread defaults are 8 MiB (Linux), 1 MiB (Windows) and
// 512 KiB (macOS) — all of them BELOW 16 MiB. A test that touched, say, 1 MiB
// would pass on Linux with the fix reverted, and would then be evidence of
// nothing on the leg most of this work happens on. The bound is chosen to sit
// above the most generous host default, not above the failure we happened to
// find.
//
// ⚠ Reverting `stack_sized_thread.cpp` to a plain `std::thread` does not make
// this test FAIL — it makes it CRASH (SIGSEGV/SIGBUS, or a Windows stack
// overflow). That is the correct and expected shape: a stack overflow has no
// failing assertion to report, which is the entire reason
// D-TEST-LSP-HARNESS-RAN-THE-SERVER-LOOP-ON-A-HOST-DEFAULT-STACK went unseen
// for as long as it did. ctest reports the crash as a failed entry either way.

#include "core/substrate/stack_sized_thread.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>

using dss::substrate::StackSizedThread;

namespace {

// Per-frame stack burn. `volatile` and the write-then-read keep the compiler
// from eliding the buffer: an optimized-away array would make this test pass
// on a thread that never had the stack, which is the one outcome that would
// make it worse than no test at all.
constexpr std::size_t kFrameBytes = 64u * 1024u;
constexpr std::size_t kDepth      = 256u;             // 256 x 64 KiB = 16 MiB
constexpr std::size_t kRequested  = 64u * 1024u * 1024u;

std::size_t burnStack(std::size_t depth) {
    volatile unsigned char buf[kFrameBytes];
    buf[0]               = static_cast<unsigned char>(depth & 0xFFu);
    buf[kFrameBytes - 1] = static_cast<unsigned char>((depth >> 8) & 0xFFu);
    if (depth == 0) {
        return static_cast<std::size_t>(buf[0]) + static_cast<std::size_t>(buf[kFrameBytes - 1]);
    }
    return static_cast<std::size_t>(buf[0]) + burnStack(depth - 1);
}

} // namespace

// The load-bearing one: 16 MiB of touched stack on a thread that asked for
// 64 MiB. Above EVERY host's default, so it discriminates everywhere.
TEST(StackSizedThread, TouchesMoreStackThanAnyHostDefaultProvides) {
    bool             ran   = false;
    std::size_t      total = 0;
    StackSizedThread t{kRequested, [&] {
                           total = burnStack(kDepth);
                           ran   = true;
                       }};
    t.join();
    EXPECT_TRUE(ran);
    EXPECT_GT(total, 0u);   // consume the result so the burn cannot be dropped
}

// A callable that throws is reported on the JOINING thread, not swallowed and
// not `std::terminate` — the contract `runOnLargeStack` has always had, now
// owned by the primitive underneath it.
TEST(StackSizedThread, JoinRethrowsWhatTheCallableThrew) {
    StackSizedThread t{1u * 1024u * 1024u,
                       [] { throw std::runtime_error("from the worker"); }};
    try {
        t.join();
        ADD_FAILURE() << "join() returned normally; the worker's exception was lost";
    } catch (std::runtime_error const& e) {
        EXPECT_EQ(std::string{e.what()}, "from the worker");
    }
    EXPECT_FALSE(t.joinable());
}

// join() is idempotent, and a second join does not re-throw a captured
// exception — otherwise the destructor-side "join if joinable" in every RAII
// user would be a second, surprising throw site.
TEST(StackSizedThread, JoinIsIdempotentAndDoesNotRethrowTwice) {
    StackSizedThread t{1u * 1024u * 1024u,
                       [] { throw std::runtime_error("once"); }};
    EXPECT_THROW(t.join(), std::runtime_error);
    EXPECT_NO_THROW(t.join());
    EXPECT_FALSE(t.joinable());
}

// A default-constructed thread is inert: not joinable, and join() is a no-op
// rather than an abort. This is what lets the type be a member that a later
// assignment fills in.
TEST(StackSizedThread, DefaultConstructedIsInert) {
    StackSizedThread t;
    EXPECT_FALSE(t.joinable());
    EXPECT_NO_THROW(t.join());
}

// Moving transfers ownership: the source becomes inert, the destination owns
// the join. The worker reads the heap-allocated Impl, so the move does not
// move the storage out from under it.
TEST(StackSizedThread, MoveTransfersTheJoinObligation) {
    bool             ran = false;
    StackSizedThread src{1u * 1024u * 1024u, [&] { ran = true; }};
    StackSizedThread dst{std::move(src)};
    EXPECT_FALSE(src.joinable());   // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(dst.joinable());
    dst.join();
    EXPECT_TRUE(ran);
}
