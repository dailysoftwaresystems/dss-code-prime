#pragma once

#include <chrono>

// ONE wall-clock budget for every test that WAITS for something the code under
// test should already have done — a server to exit, a future to become ready, a
// notification to arrive.
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

}  // namespace dss::test_support
