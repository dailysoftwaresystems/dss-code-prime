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

// ══ THE DURATIONS BELOW ARE THIS FILE'S SUBJECT, NOT ITS BUDGETS ══════════════
//
// ★★★ EVERY ONE OF THEM IS AN `ALLOWLIST`-BY-PROOF ENTRY IN
// `scripts/check-wall-clock-in-tests/check-wall-clock-in-tests.py`, KEYED BY
// `path::symbol`, and the proof each entry carries is the sentence beside it
// here. They are NOT routed through `test_wait_budget.hpp`, and routing them
// there would destroy the tests: a test OF a deadline clock has to name
// durations, because the duration is the thing under test. `kSpawnBudget` in
// particular is the deadline the pin exists to prove fires — replacing it with
// the shared 5 s `kRunBudget` would put the budget ABOVE the child's 4 s hang and
// the child would exit normally, so the assertion that a deadline fires would
// pass by never being reached.
//
// ⚠ THE TEST THAT MAKES A CLAIM ABOUT A LITERAL MUST STILL SURVIVE A SLOW HOST,
// and that is checked per constant rather than asserted for the group. The
// distinction used below: a STIMULUS (something this test causes) can only make
// the assertions MORE true when the host is slow; a BOUND (something this test
// asserts) is where a slow host reds. Bounds here come in THREE kinds, and the
// third is why this sentence is stated as three rather than two: a lower bound,
// safe under load by construction; a bound compared against another CLOCK
// rather than against a number; and exactly ONE upper bound against a number,
// `EXPECT_LT(awakeElapsed, kSpawnBudget + kKillSlack)`, whose slack is 20 s
// against a 400 ms budget precisely so a loaded host cannot reach it — see
// `kKillSlack` below, which names itself as that one and says what it is and is
// not for.
// ⚠ AN EARLIER DRAFT OF THIS PARAGRAPH SAID *EVERY* BOUND WAS ONE OF THE FIRST
// TWO, and `kKillSlack` — added to this file by the SAME commit, forty lines
// down — already described itself as *the only bound in this file that is an
// upper bound against a number*. The file contradicted itself in one diff. Kept
// and corrected rather than quietly reworded, because the lesson is the shape:
// a UNIVERSAL claim over the contents of a file is falsified by anything the
// same commit adds to that file, and nothing mechanical checks it
// ([[D-COMMENT-A-CLAIM-TRUE-WHEN-TYPED-AND-FALSE-WHEN-THE-COMMIT-LANDED]]).

// STIMULUS. The child sleeps this long; the parent's budget is a small fraction
// of it, so the kill is unambiguous even on a loaded box. A slow host makes the
// child MORE certainly outlive the budget, never less, so this cannot red under
// load. Small enough that the pin costs well under a second when it passes.
constexpr std::chrono::milliseconds kChildHang{4000};

// THE SUBJECT. This is the deadline whose firing is under test, and it must stay
// well below `kChildHang` or there is nothing to fire. The only assertion that
// reads it as a lower bound is `EXPECT_GE(awakeElapsed, kSpawnBudget)`, which a
// slow host also only makes more true.
constexpr std::chrono::milliseconds kSpawnBudget{400};

// STIMULUS. The sleep the advance pin measures the clock across; the assertions
// that read it DERIVE from it (`kProbeSleep / 2`) rather than restating a second
// number, so the relationship cannot drift.
constexpr auto kProbeSleep = std::chrono::milliseconds{80};

// THE READ-JITTER TOLERANCE, and it is a DISCRIMINATOR rather than a budget: it
// separates "two clocks are the same clock" from "two clocks are different
// clocks", and it sits FAR below the thing it must discriminate — 50 ms against
// the 144661 s of recorded suspend on the host this defect was found on, a factor
// of ~2.9 million. (An earlier comment here said "four orders of magnitude"; the
// arithmetic says more than six, and the direction of that error was to
// understate the margin.) A slow host can delay two adjacent `now()` calls, but
// it cannot move two clock ids apart by hours, which is what the arms below are
// looking for.
constexpr auto kReadJitter = std::chrono::milliseconds{50};

// THE SUSPEND FLOOR — "has this host actually slept?", a fact about the MACHINE'S
// HISTORY rather than about how long anything may take. Below it the two clocks
// cannot be told apart by reading them and the arm says so instead of asserting;
// above it they MUST differ. Nothing here is a deadline and no elapsed time is
// being bounded.
constexpr auto kSuspendFloor = std::chrono::seconds{1};

// THE KILL SLACK. The only bound in this file that is an upper bound against a
// number, and it is deliberately LOOSE: it prices spawn + poll + SIGKILL + reap
// on a box that may be running the rest of the suite at -j 8, so it is here to
// catch a deadline that has stopped bounding anything at all, not to time the OS.
// ⚠ ITS DISCRIMINATING POWER IS ALREADY OWNED BY `EXPECT_TRUE(result.timedOut)`
// one line above it: a deadline that stopped firing lets the child run its full
// `kChildHang`, which is 4 s and would pass THIS bound. Said plainly rather than
// implied — this is a sanity rail, and the pin's teeth are the `timedOut`
// assertion and the `EXPECT_GE` lower bound.
constexpr auto kKillSlack = std::chrono::seconds{20};

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
    // ★ SUSPEND IS MEASURED FROM TWO CLOCKS IN THE SAME ADJUSTMENT DOMAIN, and
    // that is the whole correction. An earlier version asserted
    // `monotonic >= uptimeRaw` "because a suspend can only ADD to the clock
    // that counts it". The premise about suspend is true; the CONCLUSION does
    // not follow, because suspend is not the only difference between those two
    // ids. `CLOCK_MONOTONIC` is ADJUSTABLE — NTP slews it — while the two _RAW
    // ids never are, so their difference carries suspend PLUS an adjustment
    // term of unbounded sign. The comment a few lines above already recorded
    // that term ("~8.8 s apart by NTP adjustment") without anyone noticing it
    // refuted the assertion below it.
    //
    // ⚠ MEASURED on the macos-latest CI runner (Release), which is what caught
    // it: CLOCK_MONOTONIC read 764359588000 ns against CLOCK_UPTIME_RAW's
    // 764855952916 ns — 0.496 s BEHIND, i.e. "-0.496 s of recorded suspend",
    // on a freshly booted VM that had never slept. That is ~650 ppm of slew
    // over 764 s. The assertion survived every local run only because the host
    // it was written on had 144661 s of real suspend, which buried a
    // sub-second adjustment — the same shape as the defect this whole test
    // exists to catch, one term hiding another.
    //
    // So: `CLOCK_MONOTONIC_RAW - CLOCK_UPTIME_RAW` — both raw, one epoch, one
    // adjustment domain — is suspend AND NOTHING ELSE, and stays a STRICT
    // assertion. No tolerance band is introduced, deliberately: a band would
    // also have to admit the failure this arm is here to catch (the two ids
    // being swapped), which shows up as a difference of HOURS, not milliseconds.
    ASSERT_GE(monotonicRaw, uptimeRaw)
        << "CLOCK_MONOTONIC_RAW read BEHIND CLOCK_UPTIME_RAW. Both are RAW, so "
           "no adjustment can separate them and suspend can only ADD to the "
           "one that counts it — this ordering is genuinely impossible unless "
           "the two ids no longer mean what this test believes";
    auto const recordedSuspend = monotonicRaw - uptimeRaw;
    // CLOCK_MONOTONIC is still READ and still pinned above (it is one of the
    // two ids `steady_clock` may resolve to), but it is deliberately NOT
    // compared against an unadjusted clock any more.
    (void)monotonic;
    std::cout << "[  CLOCK   ] Darwin: this host has recorded "
              << std::chrono::duration_cast<std::chrono::seconds>(
                     recordedSuspend)
                     .count()
              << " s of suspend since boot (CLOCK_MONOTONIC - CLOCK_UPTIME_RAW)"
              << std::endl;
    if (recordedSuspend > kSuspendFloor) {
        // The measured discriminator: on a Mac that has actually slept, the old
        // clock and the new one are far apart, so this reds the moment the
        // Darwin arm is reverted.
        EXPECT_GT(absDiff(awake, steady), kSuspendFloor)
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
    // The steady window ENCLOSES the awake window, so the upper bound below is
    // an ordering fact rather than a timing guess.
    auto const steadyBefore = steadySinceItsEpoch();
    auto const before       = AwakeClock::now();
    std::this_thread::sleep_for(kProbeSleep);
    auto const after        = AwakeClock::now();
    auto const steadyAfter  = steadySinceItsEpoch();

    auto const awakeElapsed  = after - before;
    auto const steadyElapsed = steadyAfter - steadyBefore;

    EXPECT_GT(awakeElapsed, kProbeSleep / 2)
        << "the deadline clock did not advance across a " << kProbeSleep.count()
        << " ms sleep";
    // ★ THE UPPER BOUND IS AGAINST THE OTHER CLOCK, NOT AGAINST A NUMBER, and
    // that is a strictly stronger pin than the `< 10 s` it replaces. The defect
    // it catches is the same one — a unit conversion inside `AwakeClock::now()`
    // that inflates every reading, and therefore every spawn budget in the tree,
    // by the same factor. But a wall-clock ceiling sized on a developer machine
    // reds when the host deschedules this process for longer than the ceiling,
    // whereas a DESCHEDULE MOVES BOTH CLOCKS EQUALLY and cancels here. The
    // tolerance is `kReadJitter` — 50 ms, against the ~80 s a 1000x unit error
    // would produce here, i.e. a factor of ~1600 — instead of 10 s of
    // unexplained headroom that was both looser AND flakier.
    // Ordering makes this safe on every leg: `AwakeClock` IS `steady_clock` off
    // Darwin, and on Darwin it is CLOCK_UPTIME_RAW against CLOCK_MONOTONIC_RAW —
    // same rate, same adjustment domain, and the awake one additionally EXCLUDES
    // any suspend that lands inside the window.
    EXPECT_LE(awakeElapsed, steadyElapsed + kReadJitter)
        << "the deadline clock advanced further than steady_clock did across a "
           "window that CONTAINS it — its unit conversion is wrong, which would "
           "make every spawn budget in the tree wrong by the same factor";
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
    EXPECT_LT(awakeElapsed, kSpawnBudget + kKillSlack)
        << "the kill took far longer than the budget it was given";
    // A suspend-EXCLUDING clock can never outrun one that counts suspend. If a
    // nap lands inside this spawn — which is exactly what happens on the
    // operator's Mac — this is where it shows, and the budget above is still
    // met in awake seconds.
    EXPECT_GE(steadyElapsed + kReadJitter, awakeElapsed)
        << "the awake clock outran steady_clock across one spawn";
    if (steadyElapsed > awakeElapsed + kSuspendFloor) {
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
