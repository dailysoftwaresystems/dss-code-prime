// D-TEST-RUN-HARNESS-DRAIN-AFTER-EXIT-DEADLOCKS — the permanent pin.
//
// THE DEFECT (measured 2026-08-04, TF-C114). `runBinary(captureStdout=true)`
// created an anonymous pipe, spawned the child, waited for the child to EXIT,
// and only then drained the pipe. A child that writes more than the pipe's
// kernel buffer blocks in write() long before it can exit, so:
//
//     child:  blocked in write(), buffer full, nobody reading
//     parent: blocked in WaitForSingleObject / waitpid, waiting for the exit
//             that the child cannot reach
//
// …until the timeout fires and the parent KILLS a child that was working
// perfectly. The reported symptom is `timedOut=1`, i.e. "the child hung" —
// exactly backwards, and the reason this survived: every existing caller
// printed a few bytes, and the one comment that noticed the buffer was bounded
// concluded the risk was TRUNCATION (which cannot happen, because the child is
// blocked) rather than DEADLOCK (which is what actually happens).
//
// MEASURED, before → after:
//   * Windows (this pin's host): the sqlite-harness leg gate's two plan tests
//     each burned their full 120 s budget; the suite ran 240,700 ms and
//     reported 4 failures. After: 1,851 ms, and those two tests pass.
//   * WSL / Ubuntu 24.04, standalone probe, `cat` of a 200,000-byte file:
//     before `spawned=1 timedOut=1 exit=0 bytes=0 elapsed_ms=20000`;
//     after   `spawned=1 timedOut=0 exit=0 bytes=200000 elapsed_ms=10`.
//
// WHY THIS FILE EXISTS RATHER THAN THE MEASUREMENT ALONE. A one-off probe
// proves the fix today; it does not stop the next author from "simplifying" the
// drain thread away, and the resulting red would be a TIMEOUT in some unrelated
// suite three cycles later. This asserts the property directly, on the shared
// spawn chokepoint both corpus harnesses use (`tests/examples/examples_runner`
// in-process and `integrated_tests/runner` CLI-subprocess both call
// `runBinary`, so both inherit the fix and both would inherit its loss).
//
// THE PIN IS DELIBERATELY TWO-AXIS. `timedOut == false` AND the exact byte
// count. Reverting the ordering trips the first; a "fix" that drains
// concurrently but drops the tail trips the second. Either alone could be
// satisfied by a wrong implementation.
//
// SELF-SPAWN, so the pin has no external dependency. The test executable is its
// own payload generator: `main` below intercepts `--dss-emit-bytes=<N>` before
// gtest sees it and writes N bytes to stdout. That keeps the pin portable to
// every host in the matrix (Windows has no `cat`; a Mac has no `/proc`) and
// makes the payload size a property of this file rather than of the machine.

#include "run_binary.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Above BOTH kernel buffers by a wide margin: Windows' anonymous-pipe default
// is ~4 KiB and Linux's is 64 KiB, so a payload that only cleared the smaller
// one would leave the POSIX arm of this pin vacuous — and the POSIX arm is the
// one that took a 20-second timeout to discover.
constexpr std::size_t kPayloadBytes = 200000;

constexpr std::string_view kEmitFlag = "--dss-emit-bytes=";

// The last byte is a distinct sentinel, so "the count is right" and "the TAIL
// arrived" are not the same assertion. A drain that races the child and stops
// early would otherwise be able to produce a plausible-looking short read that
// only the count catches.
constexpr char kFill     = 'A';
constexpr char kSentinel = 'Z';

std::filesystem::path selfExecutable() {
    return std::filesystem::path{::testing::internal::GetArgvs().empty()
                                     ? std::string{}
                                     : ::testing::internal::GetArgvs()[0]};
}

}  // namespace

// ── The pin ────────────────────────────────────────────────────────────────

TEST(RunBinaryCapture, ALargeChildIsDrainedConcurrentlyAndNotDeadlocked) {
    auto const self = selfExecutable();
    ASSERT_FALSE(self.empty()) << "could not recover argv[0]";
    ASSERT_TRUE(std::filesystem::exists(self))
        << "argv[0] does not resolve to a file: " << self;

    // argv becomes [self, --dss-emit-bytes=N, self]; the child's `main` sees the
    // flag and never reaches gtest. The trailing `self` is `runBinary`'s
    // `binaryPath` (its POSIX arm chmods that path, so it must be a real file —
    // the test executable already is one, and 0755 is what it already carries).
    std::vector<std::string> const prefix{
        self.string(), std::string{kEmitFlag} + std::to_string(kPayloadBytes)};

    // A bounded budget: if the ordering regresses, this pin must RED in a
    // minute rather than inherit whatever the default budget is.
    auto const t0 = std::chrono::steady_clock::now();
    auto const r  = dss::test_support::runBinary(
        self, std::chrono::seconds{60}, /*captureStdout=*/true, prefix);
    auto const elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count();

    ASSERT_TRUE(r.spawned) << r.diagnostic;

    // AXIS 1 — the deadlock itself.
    EXPECT_FALSE(r.timedOut)
        << "runBinary TIMED OUT capturing " << kPayloadBytes
        << " bytes after " << elapsedMs << " ms. That is the drain-after-exit"
           " deadlock (D-TEST-RUN-HARNESS-DRAIN-AFTER-EXIT-DEADLOCKS): the child"
           " is blocked writing into a pipe nobody is reading, and the harness"
           " then kills it and reports the stall as the CHILD's fault."
           "  diagnostic: "
        << r.diagnostic;

    // AXIS 2 — the whole payload, tail included.
    EXPECT_EQ(r.capturedStdout.size(), kPayloadBytes)
        << "captured " << r.capturedStdout.size() << " of " << kPayloadBytes
        << " bytes — a concurrent drain that stops early is the other half of"
           " the same defect";
    if (r.capturedStdout.size() == kPayloadBytes) {
        EXPECT_EQ(r.capturedStdout.back(), kSentinel)
            << "the LAST byte did not arrive";
        EXPECT_EQ(r.capturedStdout.find_first_not_of(kFill),
                  kPayloadBytes - 1u)
            << "the captured bytes are not the payload we sent — the capture is"
               " interleaved or corrupted, not merely short";
    }
    EXPECT_EQ(r.exitCode, 0u) << r.diagnostic;
}

// A small child must still work — the concurrent drain must not have broken the
// case every other caller in the tree relies on. Red-on-disable for a drain
// thread that is never joined, or joined before EOF: this loses the bytes.
TEST(RunBinaryCapture, ASmallChildStillRoundTripsItsBytes) {
    auto const self = selfExecutable();
    ASSERT_TRUE(std::filesystem::exists(self)) << self;
    std::vector<std::string> const prefix{self.string(),
                                          std::string{kEmitFlag} + "7"};
    auto const r = dss::test_support::runBinary(
        self, std::chrono::seconds{60}, /*captureStdout=*/true, prefix);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut) << r.diagnostic;
    EXPECT_EQ(r.capturedStdout, std::string(6, kFill) + kSentinel);
    EXPECT_EQ(r.exitCode, 0u);
}

// ── main: payload generator OR test runner ─────────────────────────────────
//
// Defining `main` here means the linker never pulls gtest_main's object out of
// the static library, so there is no duplicate symbol. The flag is consumed
// BEFORE InitGoogleTest, which would otherwise reject it as unrecognised.
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string_view const arg{argv[i]};
        if (arg.substr(0, kEmitFlag.size()) != kEmitFlag) continue;
        auto const n = static_cast<std::size_t>(
            std::strtoull(std::string{arg.substr(kEmitFlag.size())}.c_str(),
                          nullptr, 10));
        if (n == 0) return 2;
        // Binary mode matters on Windows: the default text mode would translate
        // any '\n' to CRLF and the byte count would not be the count we asked
        // for. The payload deliberately contains no newline at all, so the two
        // modes agree — but say so, because a future payload with a newline
        // would make this test fail for a reason that has nothing to do with
        // the defect it pins.
        std::string const payload = std::string(n - 1, kFill) + kSentinel;
        std::fwrite(payload.data(), 1, payload.size(), stdout);
        std::fflush(stdout);
        return 0;
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
