#pragma once

#include <chrono>

// ══ THE TEST SUITE'S WALL-CLOCK BUDGET VOCABULARY ═════════════════════════════
//
// EVERY measured wall-clock budget the test suite has is DEFINED here, and every
// other file POINTS at one. That is the whole arrangement, and it is why
// `check-wall-clock-in-tests` allowlists this path by proof: a literal here is
// the thing every other literal is supposed to be replaced BY, so refusing it
// would refuse the fix.
//
// ★★ THE FILE HOLDS MORE THAN ONE BUDGET BECAUSE THERE IS MORE THAN ONE
// QUESTION, and collapsing them would be the same defect in the other direction.
// "How long may a compiled program take to terminate" (5 s), "how long may the
// OPERATING SYSTEM take to admit a freshly written binary" (30 s), "how long may
// a helper script doing real work take" (120 s) and "how long may a test wait for
// something that has already happened" (60 s) are four different measurements of
// four different things. ✔MEASURED 2026-08-23 what happens when they are
// conflated: routing a program-spawn deadline through `kWaitBudget` would have
// multiplied ~618 per-example hang ceilings by twelve and described a run as a
// wait ([[D-TEST-WALL-CLOCK-ROW-REMEDY-SANCTIONS-A-SHAPE-ITS-OWN-GUARD-COUNTS]]).
//
// ⇒ ADDING A BUDGET HERE IS ALLOWED; ADDING ONE WITHOUT ITS MEASUREMENT IS NOT.
// Each block below states WHAT is being waited for, WHAT sized the number, and
// WHAT it means when it is exceeded. A constant with a name and no measurement is
// the same unmeasured number wearing a hat, and the guard counts it either way.
//
// ── kWaitBudget ───────────────────────────────────────────────────────────────
// ONE wall-clock budget for every test that WAITS for something the code under
// test should already have done — a server to exit, a future to become ready, a
// notification to arrive, a subprocess that only reads a file and prints.
//
// ★★★ WHY THIS IS A SHARED CONSTANT AND NOT A LITERAL PER SITE.
// ✔MEASURED 2026-08-22 on CI run 32585879580, `linux-clang-asan`:
// `WorkspaceProjectE2E.ASaveThatChangesNoManifestRepublishesNothing` FAILED with
// `h.runUntilExit()` returning -1 — the timeout sentinel — against a hard-coded
// TWO-SECOND deadline. It is not a hang and not a miscompile:
//
//   * ✔the same test, rebuilt with the CI leg's exact sanitizer configuration
//     (`clang-19`, Debug, `-fsanitize=address,undefined`, 665 `__asan` symbols in
//     the binary) passes in **622 ms** on an idle host;
//   * ✔the same binary, with the CPU oversubscribed 3x, takes **1912 ms** — 3.1x,
//     and within **88 ms** of the deadline;
//   * CI is worse than that experiment: a 4-vCPU runner executing
//     `ctest --parallel 4` of sanitized binaries. It crossed, at 2204 ms.
//
// ⇒ The deadline was sized on an idle developer machine and had no margin for the
// slowest host that runs it. That is the same defect the CI budgets carried one
// level up (D-CI-BUILD-AND-CTEST-BUDGETS-WERE-ONE-NUMBER-FOR-FIVE-LEGS), and it
// fails in the worst direction: a red that names an exit code, on a leg where
// nothing is actually wrong.
//
// ★ A BUDGET HERE IS A CAP, NOT A RESERVATION. Every consumer waits on a
// CONDITION and returns the moment it holds, so a healthy run never spends this
// time — enlarging it costs nothing but the delay before a genuinely stuck test
// reports. 60 s is ~30x the measured loaded case and still far below the
// per-test ceiling ctest enforces.
//
// ⚠ DO NOT SHAVE THIS BACK TOWARDS THE MEASURED TIME. The measured time is what
// a HEALTHY run takes on ONE host; the budget exists for the unhealthy run on the
// slowest one. A cap set near the observed mean is a flaky test with extra steps.
//
// ★★ AND A TIMEOUT MUST SAY IT TIMED OUT. `runUntilExit` returns -1 on expiry,
// which every call site compares against an expected EXIT CODE — so the failure
// reads `Which is: -1` and sends the reader looking for a wrong exit status. The
// helper now adds a named failure alongside it. See
// D-TEST-LSP-WAIT-DEADLINE-IS-SIZED-FOR-AN-IDLE-HOST.

namespace dss::test_support {

inline constexpr std::chrono::seconds kWaitBudget{60};

// ── kRunBudget / kAdmissionBudget (TF-C84) ────────────────────────────────────
// MOVED HERE 2026-08-23 from `run_binary.hpp`, where they were the only two
// measured budgets living outside this file — so the one place a reader could go
// to learn what time budgets the suite has did not in fact hold them
// ([[D-TEST-WALL-CLOCK-LITERAL-INVENTORY-IS-DEBT]]). `runBinary`'s own docblock
// still carries the TWO-PHASE DESIGN; what moved is the SIZING.
//
// TWO different things need bounding, and conflating them into one bare `5000`
// literal is what made the examples suite flaky:
//
//   kRunBudget       — how long the COMPILED PROGRAM may take to terminate. This
//                      is the hang detector: an infinite loop in emitted code
//                      must trip it.
//   kAdmissionBudget — how long the OPERATING SYSTEM may take to ADMIT a freshly
//                      written binary for execution, before a single instruction
//                      of the program has run. This is not the program's cost and
//                      must not be charged to the program's budget.
//
// Why the split exists — ✔MEASURED 2026-07-29 on macOS 26.5.2 / arm64 T8132
// (10 cpu, 4 P-cores), replicating a real emitted example binary:
//
//   per-exec latency (ms)      1st exec of a NEW file      re-exec, same file
//   serial                            265 – 303                   ~15
//   8-way concurrent            2607 med / 4899 max         394 med / 776 max
//   16-way concurrent           5095 med / 6841 max        1168 med / 2263 max
//
// The program itself runs in ~15 ms. Everything above that is the OS admitting
// the binary, and macOS's unified log names the mechanism: the first exec of a
// freshly written, ad-hoc-signed Mach-O makes syspolicyd run a Gatekeeper scan
// that issues a NETWORK REQUEST to Apple's notarization service before the
// child's main() is entered —
//   syspolicyd [syspolicy.exec] GK performScan: PST: (path: …)
//   syspolicyd [CFNetwork:Summary] transaction_duration_ms=1527, status=200
//   syspolicyd [syspolicy.exec] GatekeeperPolicyScanError Code=-67018
//                               "Code did not match any currently allowed policy"
// The scan always FAILS (our output is ad-hoc signed, never notarized) and the
// exec is then allowed anyway (no com.apple.quarantine xattr) — but the round
// trip is paid in full, every time, per NEW code-signature. Measured 103 such
// scans in 3 minutes across only 3 TLS connections: they multiplex over one
// HTTP/3 connection inside the single syspolicyd process, so they SERIALIZE. That
// is why the cost explodes with ctest -j parallelism while a serial rerun of the
// very same tests always passes.
//
// Hence the fix is NOT a bigger number: the cost being absorbed is somebody
// else's network and is therefore unbounded, whereas the thing the timeout exists
// to catch — a program that never terminates — is bounded and small. `runBinary`
// instead performs an UNTIMED warm-up exec so the timed window measures program
// runtime only, and kRunBudget can stay tight.
//
// kRunBudget = 5000 ms: >2x the worst WARM latency ever measured here (2263 ms at
// 16-way concurrency, which is pure CPU contention on 4 P-cores), and ~300x the
// ~15 ms a real example actually needs.
inline constexpr std::chrono::milliseconds kRunBudget{5000};

// kAdmissionBudget = 30000 ms: >4x the worst ADMISSION latency measured (6841 ms
// at 16-way concurrency). Generous on purpose — it is charged only when a program
// genuinely never terminates, and it must dominate an external, network-dependent
// cost that a slow or offline link can inflate (CFNetwork's own per-request
// timeout is 3 s, and requests queue).
inline constexpr std::chrono::milliseconds kAdmissionBudget{30000};

// ── kHelperScriptBudget ───────────────────────────────────────────────────────
// How long a HELPER SCRIPT spawned by a test may take to do REAL WORK — stage a
// source tree, run a gate's own self-test. Distinct from all three budgets above,
// and the distinction is what sized it:
//
//   * NOT kRunBudget (5 s). That bounds an emitted example program whose whole
//     job is to return an exit code; these children copy files, parse JSON and
//     execute a python interpreter's start-up.
//   * NOT kWaitBudget (60 s). That is for a wait on something that has ALREADY
//     happened, where any elapsed time at all is a symptom. Here elapsed time is
//     the work.
//   * NOT kAdmissionBudget (30 s). That is the OS's cost for a binary this
//     project just wrote; a system `python3` was admitted long ago.
//
// ✔MEASURED 2026-08-23 on this Windows box (GNU 13.2.0, Debug, `ctest -V` over
// the `harness/test_sqlite_harness_legs` entry alone, i.e. SERIAL and unloaded),
// the two gtest cases that own the spawns it bounds:
// `HarnessLegs.TheCliSmokeGateSelfTestPasses` **4289 ms** and
// `HarnessLegs.StageZincWritesOneHeaderPerTargetAndEveryLegGetsItsOwn`
// **147 ms** — each figure INCLUDING the case's own staging and assertions, so
// the spawn itself is strictly less than the number quoted. 120 s is therefore
// ~28x the slower of the two on an idle host.
// ⚠ THE MEASUREMENT IS SERIAL AND THE BUDGET IS NOT SIZED FROM IT ALONE. A
// `ctest -j` run of the same case competes with the rest of the suite, and the
// admission table above — the one place in this repo where idle-vs-contended was
// measured properly — records a **22x** spread on a FIRST exec (303 ms serial
// worst against 6841 ms at 16-way) and **151x** on a warm one (~15 ms against
// 2263 ms). A budget sized on the idle number alone would be a flaky test with
// extra steps.
//
// ⚠ IT WAS 120 s BEFORE THIS MEASUREMENT TOO, and that is the honest report: the
// number was not sized by anything, it was written twice by copy-paste. It is
// KEPT rather than shrunk because a budget must be sized for the slowest host
// that runs it, never for the box that measured it — python start-up on a cold
// CI runner under `--parallel 4` is the case this has to survive, and shaving a
// budget towards an observed local mean is the exact defect
// [[D-TEST-LSP-WAIT-DEADLINE-IS-SIZED-FOR-AN-IDLE-HOST]] records. What changed is
// that the number is now ONE decision with its evidence attached instead of two
// unexamined copies.
inline constexpr std::chrono::seconds kHelperScriptBudget{120};

}  // namespace dss::test_support
