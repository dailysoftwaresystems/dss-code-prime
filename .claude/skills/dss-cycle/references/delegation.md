# Delegation is the default

### Step C.0 — DELEGATION IS THE DEFAULT (read before every step below)

**Every step of this cycle that is not inherently serial runs in a SUBAGENT. This is not an
optimization — it is how the cycle is meant to execute, and it does not require the user to
ask for it each time.** Some host configurations ship an ambient "don't spawn agents unless
asked" default; **this skill overrides it.** A project rule beats an ambient default, and
the reason is concrete: the orchestrator's context is the cycle's scarcest resource. Reading
300 lines of a pass to learn one API, or grepping four files to find a config field, burns
budget that should be spent on judgment — deciding, gating, and landing. An agent absorbs
that reading and hands back a conclusion.

**The failure this prevents, observed repeatedly:** the orchestrator does everything inline,
runs low on context mid-cycle, and ends the turn with design notes instead of shipped code —
cycle after cycle. If you find yourself opening a large file mainly to *learn* something,
that is a delegation you skipped.

**Delegate by default:**

| step | agent | notes |
|---|---|---|
| 1–2 pick / clear blockers | `Explore` or `general-purpose` | when it means sweeping plans, the registry, or `src/` to locate work |
| 3 plan | `Plan` / `feature-dev:code-architect` | returns the execution plan; you judge it |
| **3.5 design-audit** | **independent `general-purpose`** | MUST be a fresh agent — the point is that it did not author the plan |
| 4 implement | one agent per DISJOINT file set, **in parallel** | see the parallelism rule below |
| 5 review & fold | `pr-review-toolkit:*` / `feature-dev:code-reviewer` | |
| 7–8 deferrals + cross-plan | `general-purpose` | mechanical registry/plan reconciliation |
| **8.5 code-audit** | **independent `general-purpose`, READ-ONLY** | must not be the agent that wrote the code |

**PARALLELISM:** when a step splits into disjoint file sets — engine `.cpp` vs `*.json`
config vs `examples/` corpus vs `tests/` — launch those agents IN ONE MESSAGE so they run
concurrently, and tell each agent explicitly which paths it owns and which it must not
touch.

### ★★★ AT MOST **4** REASONING AGENTS AT A TIME (operator instruction, 2026-08-19)

**The cap is on CONCURRENCY, not on the total per cycle.** Run more than four lanes' worth of
work by running it in **waves of at most four**, each wave launched in one message, each wave
joined before the next starts.

**What counts against the cap: a REASONING agent** — anything spawned through the Agent tool
that thinks for itself (`Explore`, `Plan`, `general-purpose`, `feature-dev:code-architect`, the
reviewers, the independent design- and code-auditors). One `Agent` call = one slot, for as long
as it is live.

**What does NOT count: pure script execution.** Builds, `ctest` runs, the guard scripts, `rsync`,
a remote leg over a carriage, a watcher — background or foreground, however many are in flight.
They consume machine, not judgment, so run as many as the machine and the tree can take.

⚠⚠ **BUT "AS MANY AS THE MACHINE CAN TAKE" IS A BUDGET THE ORCHESTRATOR MUST DIVIDE, AND UNTIL
2026-08-23 NOTHING DIVIDED IT.** ✔MEASURED (cycle P28,
`D-CYCLE-LANE-CTEST-PARALLELISM-IS-UNBOUNDED-IN-AGGREGATE`): P17 made `-j 8` the default for
`run-gate` and `local-build`, which is right for ONE gate. With four lanes each running its own
`ctest -j 8` in its own build tree, the aggregate is **32 concurrent test processes**, and a lane
measured single example tests going from **~6 s to ~200 s** — a ~33x degradation. One lane
**abandoned a 639-test gate** it had already earned, because at that rate it projected to ~4 hours
and was starving its siblings. The process list confirmed three concurrent `ctest -j 8` runs; this
is measured, not inferred from the slowdown.
★ **The lane cannot see this and must not be asked to.** From inside a lane, a slow suite is
indistinguishable from a slow machine. Only the orchestrator knows the lane count.
⇒ **THE ORCHESTRATOR ASSIGNS EACH LANE ITS `-j` IN THE BRIEF**, and the brief states the number
rather than leaving the default.
⚠⚠ **AND `cores / N` IS NOT THE RIGHT NUMBER — THAT WAS MEASURED WRONG THE SAME DAY IT WAS
WRITTEN.** ✔The host has **32** logical processors, so four lanes at `-j 8` IS `cores / N` exactly,
and it is the configuration that produced the 33x degradation above. **A ctest ENTRY IS NOT A
JOB.** This suite's entries spawn subprocesses of their own — `integrated_tests` is 616 entries
each invoking the CLI, and the examples runner compiles — so `N x j` counts *supervisors*, not the
compilers underneath them, and lane BUILDS compete for the same cores at the same time.
⇒ Size it **well below** `cores / N`: with N lanes that also build, `cores / (2N)` is the starting
point, and the orchestrator raises it only after watching a run rather than lowering it after a
lane gives up. ★ The number to watch is not CPU% — it is **per-test wall clock against its own
baseline**: this project's example tests run in ~6 s uncontended, so a 20 s example is the signal,
long before anything looks saturated.
⛔ Do NOT fix this by serialising the lanes: the operator rejected serialisation by name in
2026-07-31 (*"I HATE WORKAROUNDS"*), and the defect is not that lanes run at once, it is that
nobody divided the budget.
★ Corollary for the orchestrator's own gate: the attributable whole-tree run happens **after** the
fold, when no lane is live, so it gets the whole machine and the full `-j`.

⚠ **Why a cap at all, and it is not politeness to the host.** Two costs rise with lane count and
neither is visible from inside a lane:
- **The shared tree.** The section above measures it: ownership partitions WRITES, not the
  compiler, the build directory, or the scratchpad. Every additional live lane multiplies the
  window in which a sibling's half-written state makes some other lane's measurement false.
- **The orchestrator.** It must hold every live lane's brief, its owned paths, and its returned
  result well enough to judge them. Past four, the orchestrator stops judging and starts
  relaying — which is exactly the failure `⚠⚠ A LANE BRIEF THAT PRESCRIBES THE FIX` warns about,
  arriving from the other direction.

★ **Fewer is often right.** Four is a ceiling, not a target: two well-briefed lanes on genuinely
independent file sets beat four lanes that have to be told about each other.

⚠⚠ **DISJOINTNESS IS NOT ENOUGH, AND THIS LINE USED TO CLAIM IT WAS.** It said *"overlapping
writes are the only real hazard; disjointness removes it."* ✔MEASURED 2026-08-15 and that is
false: a lane writing **only to files it owns** still breaks every sibling, because the shared
tree is a shared *build input*. Three independent lanes hit it in one cycle — one could not
reach `rc=0` on the shared `build/` for reasons entirely in another lane's files; one had its
own private `build-warn/` reddened by 23 errors in files it did not own; and one had its
scratchpad script overwritten mid-run by a sibling. **Ownership partitions WRITES; it does not
partition the COMPILER, the build dir, or the scratchpad.**

★★ **A BYTE-CHANGING MEASUREMENT GOES IN A `git worktree` — PASTE `worktrees.md` §H.0 INTO THE
BRIEF, DO NOT CITE IT.** §H.0 already names *"red-on-disable mutants"* explicitly and predates
the incidents above, so the gap is not knowledge — it is that a brief which says *"prove
red-on-disable"* and never says *where* leaves a lane to mutate shipped source in the shared
tree, which is the obvious reading. ✔The cost of getting this right is trivial and measured: a
sibling lane the same day hit the wall, moved to a throwaway worktree, verified, and
`git worktree prune`d.
- ⚠ **The orchestrator's half: DO NOT RUN TREE-READING GATES WHILE LANES ARE LIVE.** Red-on-disable
  *requires* knowingly-wrong bytes in shipped source, so every correct application of §A.5 opens a
  window in which every measurement of the worktree is false — **the more rigorous the lane, the
  wider the window.** ✔The orchestrator was fooled by exactly this: a gate run mid-window reported
  a clean verdict and a next-free diagnostic ordinal ~200 slots off, from bytes existing in no real
  tree. Nothing was wrong with the instrument.
- Give each lane a **lane-private scratch subdirectory**; the session scratchpad is shared.
- See [[D-GATE-H0-WORKTREE-RULE-IS-WRITTEN-AND-UNENFORCED]] for the full measurement.

**★ DO NOT DELEGATE — the orchestrator keeps these:**
- **Step 6, the gate** (builds, ctest, the 3-leg run, the sqlite re-probe). A delegated
  build/gate agent reliably **yields mid-build** — it kicks the build off, reports "standing
  by", and leaves an orphaned detached job. This has bitten this project repeatedly. Drive
  builds yourself, FOREGROUND-BLOCKING, or via a harness-tracked `run_in_background` command
  that re-invokes you on exit. If you *do* hand a build to an agent, say FOREGROUND-BLOCKING
  in the prompt and treat a "standing by" reply as a failed step.
- **Step 9, commit & push**, and every §B decision. Those are judgment and authority.
- **Verifying an agent's claim.** An agent reporting "done, all green" is a claim, not
  evidence (§B: green ≠ clean). Re-run the gate yourself. A delegated agent that refutes its
  own pre-registered hypothesis mid-task is doing it RIGHT — read its reasoning, do not
  rubber-stamp it.

**Prompt quality is the whole game.** A vague prompt returns vague work you must redo. Give
each agent: the exact files it owns, the invariants it must not break (§A — agnosticism,
fail-loud, strict tests), the house comment style, the specific traps already known (the
eager-import law, closed descriptor key sets, `--define` not `-D`, capture rc directly), and
what to REPORT BACK. Tell it what NOT to do as explicitly as what to do.

★★★ **AND EVERY BRIEF MUST SAY "ANCHOR *AND CLOSE*" — "anchor every issue you find", written
alone, is an instruction to generate OPEN ROWS.** ✔MEASURED 2026-08-11: that exact sentence went
into four consecutive lane briefs; the lanes obeyed it perfectly and the branch ended with 22 open
rows, nine of them from a lane whose assignment WAS the thing it was documenting. The lanes were
not at fault — the brief asked for anchors and got anchors. Put this in every brief, verbatim:

> Anchor every issue you come across — and **close it in this cycle**. A row you hand me OPEN is
> work you are asking someone else to do; hand me rows **born ✅ CLOSED**, with the fix and its
> verification in them. If something is genuinely out of reach it needs a **NAMED blocker** — a
> specific missing prerequisite, an unfired trigger, or a decision only the operator can make.
> "Out of scope", "bigger than this cycle", "a follow-up" and "the natural next step" are not
> blockers. Bring me the decision; do not park it in the registry.

Then CHECK the returned rows before you write them. An agent reporting "anchored 6 findings" is
reporting six unfinished jobs unless every one says CLOSED.
