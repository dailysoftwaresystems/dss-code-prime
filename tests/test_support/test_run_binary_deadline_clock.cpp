// D-TEST-RUN-HARNESS-DEADLINE-COUNTS-HOST-SUSPEND — the permanent pin.
//
// THE DEFECT (measured 2026-08-13). `runBinary`'s POSIX arm took
// `steady_clock::now()` and killed the child at `start + timeout`. "Has this
// child made progress" is a question about AWAKE time, and on Darwin
// `steady_clock` is `CLOCK_MONOTONIC`, which is documented to keep incrementing
// while the system sleeps — so a napping laptop was billed to the child. Same
// host, same day: a run killed at `child timed out after 120000 ms` after
// 421710 ms of wall, and a probe whose own `windowSeconds` was 20.15 reported
// against 131 s of wall. Every python child bounds itself on
// `time.monotonic()`, which on Darwin is `mach_absolute_time` and does NOT
// count sleep, so parent and child disagreed about a second by the nap.
//
// ★★ WHAT CAN AND CANNOT BE PINNED, SAID PLAINLY RATHER THAN PAPERED OVER. A
// host suspend cannot be induced from inside a test — the Mac naps when it is
// left alone, not on demand, and Windows' modern standby cannot be entered from
// a test run on the operator's own machine at all. So this file does NOT fake a
// suspend and then assert on the fake. It pins the two things that are real:
//
//   (1) WHICH CLOCK IS SELECTED, asserted against a clock id read directly here
//       — so reverting `AwakeClock`'s Darwin arm to `steady_clock` reds this,
//       measurably, on a Mac that has actually slept (this one has slept
//       144661 s since boot; the two clocks are 40 hours apart).
//   (2) THAT THE DEADLINE STILL FIRES, end to end, through the real
//       `runBinary` on a real hanging child, with the elapsed bound stated in
//       AWAKE seconds.
//
// The third binding is not a test at all and is the strongest of the three:
// `AwakeClock::time_point` does not compare against `steady_clock::time_point`,
// so a future edit that recomputes the deadline from `steady_clock::now()`
// FAILS TO COMPILE rather than silently regressing. What remains uncovered, and
// is stated here rather than implied: nothing reds if someone deletes the
// deadline pair outright, and Windows' `WaitForSingleObject` arm is UNMEASURED
// across modern standby — it was not changed, and no honest table row can be
// written for it from this box.

#include "run_binary.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(_WIN32)
  #include <time.h>
#endif

namespace detail_pin {

// "Does `A >= B` compile?", as a value — see the static_assert below for why
// this is not spelled as a `requires` expression.
template <class A, class B, class = void>
inline constexpr bool kOrderable = false;
template <class A, class B>
inline constexpr bool kOrderable<
    A, B, std::void_t<decltype(std::declval<A const&>()
                               >= std::declval<B const&>())>> = true;

}  // namespace detail_pin

namespace {

using dss::test_support::AwakeClock;
using Nanos = std::chrono::nanoseconds;

constexpr std::string_view kHangFlag = "--dss-hang-ms=";

// The child sleeps this long; the parent's budget is a small fraction of it, so
// the kill is unambiguous even on a loaded box. Both are small enough that the
// pin costs well under a second when it passes.
constexpr std::chrono::milliseconds kChildHang{4000};
constexpr std::chrono::milliseconds kSpawnBudget{400};

[[nodiscard]] Nanos absDiff(Nanos a, Nanos b) {
    return (a > b) ? (a - b) : (b - a);
}

[[nodiscard]] Nanos steadySinceItsEpoch() {
    return std::chrono::duration_cast<Nanos>(
        std::chrono::steady_clock::now().time_since_epoch());
}

#if !defined(_WIN32)
// A clock id read straight from the OS, so the pin compares `AwakeClock`
// against the KERNEL rather than against another copy of its own arithmetic.
[[nodiscard]] Nanos posixClockNs(clockid_t id) {
    struct timespec ts {};
    if (::clock_gettime(id, &ts) != 0) return Nanos::zero();
    return std::chrono::seconds{ts.tv_sec} + Nanos{ts.tv_nsec};
}
#endif

std::filesystem::path selfExecutable() {
    return std::filesystem::path{::testing::internal::GetArgvs().empty()
                                     ? std::string{}
                                     : ::testing::internal::GetArgvs()[0]};
}

}  // namespace

// ── (3) The structural binding, checked by the compiler ────────────────────
//
// These are the reason `AwakeClock` is a clock TYPE and not a free function
// returning nanoseconds: a distinct `time_point` is what makes the wrong clock
// a build error at the deadline instead of a behaviour change nobody sees.
static_assert(!std::is_same_v<AwakeClock, std::chrono::steady_clock>,
              "the deadline clock must be its own type; if it is an alias for "
              "steady_clock then the Darwin arm has been lost");
// Written as the void_t detection idiom rather than a `requires` expression
// because MSVC 14.51 reports the failed operator lookup inside `requires` as a
// hard C2678 instead of an unsatisfied constraint — the check would then be
// unbuildable on the very leg that gates this repo.
static_assert(!detail_pin::kOrderable<AwakeClock::time_point,
                                      std::chrono::steady_clock::time_point>,
              "an AwakeClock deadline must NOT be comparable against a "
              "steady_clock reading — that incomparability is the whole "
              "protection against the deadline being recomputed on the clock "
              "that counts host suspend");
static_assert(detail_pin::kOrderable<AwakeClock::time_point,
                                     AwakeClock::time_point>,
              "...and it must still be comparable against ITSELF, or the check "
              "above passes for the trivial reason that nothing compares");
static_assert(AwakeClock::is_steady);
static_assert(std::is_same_v<AwakeClock::duration, std::chrono::nanoseconds>);

// ── (1) Which clock the deadline is spent on, per host ─────────────────────
//
// Bounded comparisons rather than equality on purpose, and this is the one
// place in the file where §7.1's "assert the exact value" does not apply: two
// reads of two clocks are separated by real time, so the exact difference is
// not a knowable constant. The bound is chosen four orders of magnitude below
// the thing it must discriminate — 50 ms of read jitter against 144661 s of
// recorded suspend on the host this defect was found on.
TEST(RunBinaryDeadlineClock, TheDeadlineIsSpentOnTheClockThatStopsWithTheMachine) {
    auto const awake  = AwakeClock::now().time_since_epoch();
    auto const steady = steadySinceItsEpoch();
    constexpr auto kReadJitter = std::chrono::milliseconds{50};

#if defined(__APPLE__)
    // Darwin publishes TWO continuous monotonic ids and one awake one, and
    // measurement — not the spelling — is what says which is which:
    // CLOCK_MONOTONIC and CLOCK_MONOTONIC_RAW both keep counting through a
    // sleep, CLOCK_UPTIME_RAW does not. The deadline must be on the last.
    auto const uptimeRaw    = posixClockNs(CLOCK_UPTIME_RAW);
    auto const monotonic    = posixClockNs(CLOCK_MONOTONIC);
    auto const monotonicRaw = posixClockNs(CLOCK_MONOTONIC_RAW);
    EXPECT_LT(absDiff(awake, uptimeRaw), kReadJitter)
        << "the deadline clock is not CLOCK_UPTIME_RAW. On Darwin that means it "
           "counts host suspend, and every spawned child is billed for time the "
           "machine spent asleep (D-TEST-RUN-HARNESS-DEADLINE-COUNTS-HOST-"
           "SUSPEND)";
    // WHAT steady_clock ACTUALLY IS HERE, asked of the kernel rather than
    // assumed — an earlier reading of this defect named CLOCK_MONOTONIC when
    // libc++ on this host in fact resolves to CLOCK_MONOTONIC_RAW (the two are
    // ~8.8 s apart by NTP adjustment; a nap DELTA cannot tell them apart
    // because both advance by the nap). The load-bearing claim is not WHICH of
    // the two it is, but that it is one of them and NOT the awake one, so that
    // is what is asserted: libc++ may retarget between continuous ids without
    // this reddening, and cannot quietly become suspend-excluding.
    EXPECT_TRUE(absDiff(steady, monotonic) < kReadJitter
                || absDiff(steady, monotonicRaw) < kReadJitter)
        << "steady_clock matches neither of Darwin's continuous monotonic ids, "
           "so nothing here knows what it is any more";
    ASSERT_GE(monotonic, uptimeRaw)
        << "CLOCK_MONOTONIC read BEHIND CLOCK_UPTIME_RAW, which is impossible: "
           "a suspend can only ADD to the clock that counts it";
    ASSERT_GE(monotonicRaw, uptimeRaw)
        << "CLOCK_MONOTONIC_RAW read BEHIND CLOCK_UPTIME_RAW, which is "
           "impossible for the same reason";
    auto const recordedSuspend = monotonic - uptimeRaw;
    std::cout << "[  CLOCK   ] Darwin: this host has recorded "
              << std::chrono::duration_cast<std::chrono::seconds>(
                     recordedSuspend)
                     .count()
              << " s of suspend since boot (CLOCK_MONOTONIC - CLOCK_UPTIME_RAW)"
              << std::endl;
    if (recordedSuspend > std::chrono::seconds{1}) {
        // The measured discriminator: on a Mac that has actually slept, the old
        // clock and the new one are far apart, so this reds the moment the
        // Darwin arm is reverted.
        EXPECT_GT(absDiff(awake, steady), std::chrono::seconds{1})
            << "this host recorded real suspend, so its awake clock and its "
               "steady_clock MUST read differently — reading the same means the "
               "deadline is back on the suspend-counting clock";
    } else {
        std::cout << "[  CLOCK   ] Darwin: no suspend recorded since boot, so "
                     "the two clocks cannot be told apart by reading them; the "
                     "CLOCK_UPTIME_RAW selection above is the whole of this arm "
                     "on this run"
                  << std::endl;
    }
#elif !defined(_WIN32)
    // Linux is the MIRROR IMAGE and already correct: its CLOCK_MONOTONIC
    // EXCLUDES suspend and CLOCK_BOOTTIME is the one that includes it, so
    // steady_clock is already the right clock and AwakeClock is it.
    auto const monotonic = posixClockNs(CLOCK_MONOTONIC);
    EXPECT_LT(absDiff(awake, monotonic), kReadJitter)
        << "the deadline clock is not CLOCK_MONOTONIC, which is the suspend-"
           "EXCLUDING clock on this platform";
    EXPECT_LT(absDiff(awake, steady), kReadJitter)
        << "on Linux steady_clock IS CLOCK_MONOTONIC; a gap here means one of "
           "the two is reading something else entirely";
  #if defined(CLOCK_BOOTTIME)
    auto const boottime = posixClockNs(CLOCK_BOOTTIME);
    ASSERT_GE(boottime, monotonic)
        << "CLOCK_BOOTTIME read BEHIND CLOCK_MONOTONIC, which is impossible: a "
           "suspend can only ADD to the clock that counts it";
    std::cout << "[  CLOCK   ] Linux: this host has recorded "
              << std::chrono::duration_cast<std::chrono::seconds>(boottime
                                                                  - monotonic)
                     .count()
              << " s of suspend since boot (CLOCK_BOOTTIME - CLOCK_MONOTONIC)"
              << std::endl;
  #endif
#else
    // ⚠ UNMEASURED, AND SAID SO. The Windows arm does not use this clock: the
    // deadline is `WaitForSingleObject`'s own, timed inside the kernel. Whether
    // that timeout is charged for modern standby is not known here, because
    // modern standby cannot be induced from a test run — and an assumed table
    // row is exactly what this project keeps paying for. What IS asserted is
    // that AwakeClock is the QueryPerformanceCounter-backed steady_clock, i.e.
    // that the Windows definition is the documented no-op it claims to be.
    EXPECT_LT(absDiff(awake, steady), kReadJitter)
        << "on Windows AwakeClock is defined AS steady_clock; a gap means the "
           "definition drifted from what this file documents";
    std::cout << "[  CLOCK   ] Windows: AwakeClock is QueryPerformanceCounter "
                 "and the spawn deadline is WaitForSingleObject's own. Neither "
                 "has been measured across modern standby; this arm asserts the "
                 "definition, not the suspend behaviour."
              << std::endl;
#endif
}

// The clock has to be a working clock, not a constant that satisfies every
// comparison above. A stub returning 0 would pass the selection arm on Linux
// (0 vs 0) and is exactly the shape of pin this project refuses to ship.
TEST(RunBinaryDeadlineClock, TheDeadlineClockAdvancesAcrossRealAwakeTime) {
    auto const before = AwakeClock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{80});
    auto const after = AwakeClock::now();
    EXPECT_GT(after - before, std::chrono::milliseconds{40})
        << "the deadline clock did not advance across an 80 ms sleep";
    EXPECT_LT(after - before, std::chrono::seconds{10})
        << "the deadline clock advanced absurdly across an 80 ms sleep — its "
           "unit conversion is wrong, which would make every spawn budget in "
           "the tree wrong by the same factor";
}

// ── (2) The deadline, end to end, through the real spawn path ──────────────
//
// SELF-SPAWN with the launcher-prefix form, which also SKIPS the untimed
// admission warm-up: the warm-up would otherwise run this deliberately hanging
// child for the full kAdmissionBudget before the timed run even started.
TEST(RunBinaryDeadlineClock, AHangingChildIsStillKilledAndTheBudgetIsAwakeTime) {
    auto const self = selfExecutable();
    ASSERT_TRUE(std::filesystem::exists(self)) << self;
    std::vector<std::string> const prefix{
        self.string(),
        std::string{kHangFlag} + std::to_string(kChildHang.count())};

    auto const awakeBefore  = AwakeClock::now();
    auto const steadyBefore = steadySinceItsEpoch();
    auto const result = dss::test_support::runBinary(self, kSpawnBudget,
                                                     /*captureStdout=*/false,
                                                     prefix);
    auto const awakeElapsed  = AwakeClock::now() - awakeBefore;
    auto const steadyElapsed = steadySinceItsEpoch() - steadyBefore;

    ASSERT_TRUE(result.spawned) << result.diagnostic;
    EXPECT_TRUE(result.timedOut)
        << "a child sleeping " << kChildHang.count() << " ms outlived a "
        << kSpawnBudget.count() << " ms budget and was NOT killed. The deadline "
                                   "loop no longer fires at all.  diagnostic: "
        << result.diagnostic;
    EXPECT_GE(awakeElapsed, kSpawnBudget)
        << "the child was killed BEFORE its budget expired";
    // Generous upper bound: this measures spawn + poll + SIGKILL + reap on a
    // box that may be running the rest of the suite at -j 8. It is here to
    // catch a deadline that has stopped bounding anything, not to time the OS.
    EXPECT_LT(awakeElapsed, kSpawnBudget + std::chrono::seconds{20})
        << "the kill took far longer than the budget it was given";
    // A suspend-EXCLUDING clock can never outrun one that counts suspend. If a
    // nap lands inside this spawn — which is exactly what happens on the
    // operator's Mac — this is where it shows, and the budget above is still
    // met in awake seconds.
    EXPECT_GE(steadyElapsed + std::chrono::milliseconds{50}, awakeElapsed)
        << "the awake clock outran steady_clock across one spawn";
    if (steadyElapsed > awakeElapsed + std::chrono::seconds{1}) {
        std::cout << "[  CLOCK   ] a host suspend of ~"
                  << std::chrono::duration_cast<std::chrono::seconds>(
                         steadyElapsed - awakeElapsed)
                         .count()
                  << " s landed INSIDE this spawn, and the child was still "
                     "given its full awake budget"
                  << std::endl;
    }
}

// ── main: hanging child OR test runner ─────────────────────────────────────
//
// Same shape and same reason as test_run_binary_capture.cpp: the flag is
// consumed before InitGoogleTest, which would reject it as unrecognised, and
// defining `main` here keeps gtest_main's object out of the link.
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string_view const arg{argv[i]};
        if (arg.substr(0, kHangFlag.size()) != kHangFlag) continue;
        auto const ms = std::strtoull(
            std::string{arg.substr(kHangFlag.size())}.c_str(), nullptr, 10);
        if (ms == 0) return 2;
        std::this_thread::sleep_for(std::chrono::milliseconds{ms});
        return 0;
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
