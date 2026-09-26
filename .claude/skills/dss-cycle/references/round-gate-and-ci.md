# The end-of-round gate — eight runs — and CI: never run it, always read it

What a round of lanes owes before its commit is called green, and how CI is used: read, never run.
Reading CI at step 0 with `dssharness check-ci-legs` is in `workflow-steps.md` (step 0); a red CI leg as
a hard stop is in `triggers-and-hard-stops.md`.

## Contents
- The end-of-round gate is `{Debug, Release} × four legs` — eight runs (operator ruling 2026-09-14)
- CI is expensive to RUN and free to READ — and the two rules are opposite
- The measurement that produced the ruling — and the three corrections it took

## ★★★★ THE END-OF-ROUND GATE IS `{Debug, Release} × FOUR LEGS` — **EIGHT RUNS** — operator ruling 2026-09-14

> *"dss-cycle skill does not work aligned with CI because it's expensive. We enable 'Run Pipes' when
> about to merge the PR to ensure it's green. We should run the debug and release units in all legs
> at least once in the end of every dss cycle round of 4 lanes."*

**At the end of every round of four lanes, the gate is EIGHT runs, not four:**

|  | Windows | WSL x86_64 | macOS arm64 | arm64 VPS |
|---|---|---|---|---|
| **Debug** | ✔ | ✔ | ✔ | ✔ |
| **Release** | ✔ | ✔ | ✔ | ✔ |

★ **"At least once in the end of every round" is the cadence.** A lane iterating on its own tree uses
whichever build type it is working in; what this ruling fixes is what a ROUND owes before its commit
is called green. ⚠ **Report eight numbers with their build type beside each** — a four-number gate
line is now an incomplete gate that reads as complete.
ⓘ `dssharness test --legs <leg>-release` takes the remote half; the build type is part of the
leg NAME now, and `dssharness legs` lists every one that can run.
★ **The whole gate is ONE invocation: `dssharness test --legs gate`** — `legSets.gate` in
`.harness-config/config.json` names the eight legs, and each leg's ledger carries its run's count.

### ⚠⚠ CI IS EXPENSIVE TO **RUN** AND FREE TO **READ** — AND THE TWO RULES ARE OPPOSITE

- ⛔ **NEVER make CI run.** The Pipeline workflow is `pull_request`-triggered and gated on a
  **`Run Pipes` label the operator enables only when a PR is about to merge**; there is no
  `workflow_dispatch`. Never add, remove or toggle that label, never push to re-trigger a run, and
  never touch the PR to obtain a green. **A cycle therefore cannot DEPEND on CI having run** — most
  commits have no verdict at all, which is exactly why the eight-run local gate above is what a
  round owes.
- ✅ **ALWAYS read whatever verdict already exists.** Reading costs nothing, and step 0 now does it
  with `dssharness check-ci-legs`. A red leg is a HARD STOP on *proceeding*, never on *fixing*.

★ **This corrects a framing I first wrote into a row, and the correction is the point.** I filed
*"no step of `/dss-cycle` reads CI at all"* as though the READING were the defect, and then
over-corrected to *"a cycle must not consult CI"*. **Both were wrong.** Consulting is free and was
always right; what is expensive is *running*, and what was actually broken is that **the local gate
covered ONE build type while reporting as though it covered the configuration space.**

### ✔THE MEASUREMENT THAT PRODUCED THE RULING — AND THE THREE CORRECTIONS IT TOOK

2026-09-14, PR #57: two legs red at HEAD, the failing step `Test` in both —
`run-tests (windows-msvc-release, …, Release, …)` and `run-tests (macos-clang-release, …, Release, …)` —
while the local four-leg Debug gate was **2179 / 2178 / 2148 / 2148 GREEN on that exact commit.**

⚠ **Three things I asserted about it were WRONG, and each is a reusable trap:**
1. ✗ *"Every run failed for ten days and ~20 commits."* Every run DID fail, but the runs before
   `de1e83ef` failed at **`label-check`** with `run-tests` **SKIPPED**. **The matrix has executed
   SIX times on this branch, not twenty**, and `windows-msvc-release` was last GREEN at `3d226255`
   (2026-09-02). ⇒ **`gh run list` conclusions are not evidence about the tree** — a run can be red
   without the tests having run at all.
2. ✗ *"The logs expired, so only a local reproduction can attribute it."* Half true. Logs expire
   (HTTP 410; artefacts report `expired: true` after **three** days, not the `retention-days: 7` the
   workflow asks for — a repository setting silently overrides it). **But job METADATA does not**,
   and a Test step's DURATION separates a real failure from a `--stop-time` budget overrun by
   itself: an overrun cannot take less than `ctest_budget_min`. ✔The worst red Test step here was
   **829 s against a 3000 s budget (27.6%)**. **No budget was ever near its cap; none was touched.**
3. ✗ *"These are Release-only failures, so a local Release leg would catch them."* **Neither was
   Release-dependent at all**, and a Release leg would have caught NEITHER:
   - Windows was `ninja-deps-freshness`. `check-ninja-deps.py` scopes its premise to `deps = gcc`
     (*"gcc lists the source itself"*) and then applies it unscoped; **`deps = msvc` parses
     `/showIncludes`, which reports headers ONLY**, so `#deps 0` is CORRECT for a TU with no
     `#include`. The local Windows gate is **MinGW GCC**, so the fact is invisible to it in *every*
     build type.
   - macOS was a repo-guard entry, proved **build-type independent** by running it from the repo
     root with no build tree at all. ★ **THE DURABLE FACT: a repo-guard is HOST-INDEPENDENT** — it reads
     the TREE, not the machine — **so which legs execute it is a CONFIGURATION decision, and a
     leg that skips it can never see a guard-only defect.** Derive the leg set from the run you
     are looking at (the per-leg totals differ by exactly the guard count), never from memory.
     ★★★ **AND THE LEG SET IS DECLARED, NOT DECIDED THREE TIMES.** ✔MEASURED 2026-09-16, while
     three hand-written drivers still existed: each chose its own default, so one passed
     `-LE repo-guard` on the two ssh legs while another ran every guard on WSL — Windows **2207**,
     WSL **2206**, macOS and the VPS **2167**. ⛔ This sentence asserted for a year that guards ran
     on exactly ONE local host and that all three indirect legs skipped them. Both halves were
     false: it was TWO local hosts and **four** of a round's eight runs. ★ **That class of bug
     needs two programs to disagree; one tool reading one configuration cannot have it** — which
     is the argument for the migration in one line, and the reason the durable sentence above is
     now checkable rather than advisory. SUPERSEDED 2026-09-24 by `remoteExcludes` in .harness-config/config.json — declared once, the repo-guard label runs on the two Windows legs and every remote leg, WSL included, leaves it out, so the eight-run gate is one invocation (see leg-hosts.md)
     ⚠ **The count is not typed here, and the migration is why that matters more than ever:** it
     moves by whole waves. `dssharness test --legs windows-x86_64-debug --label repo-guard` counts it in the
     leg's ledger, and so does the configure line
     `repo-guard label applied to N test(s)` — which is what the root `CMakeLists.txt` says to read,
     in those words, having already gone stale by eight entries once. This sentence has said **18**,
     then **31**, then **40**; **re-derive it at the commit in front of you.**

★★★ **THE SHAPE IS THIS CYCLE'S OWN THROUGH-LINE, TWICE MORE: THE RULE A DEFECT CITED WAS TRUE — OF
THE THING NEXT DOOR.** *"`#deps 0` is never legitimate"* is true of **gcc** and false of **MSVC**.
*"`comm` names the image"* is true on **Linux** and false on **macOS**, where `ps -eo comm=` yields a
truncated ABSOLUTE PATH (✔650/662 rows contain `/`; Linux control 0/39). ⇒ Before trusting a rule a
guard cites, ask **which toolchain, and which platform, it was measured on.**
