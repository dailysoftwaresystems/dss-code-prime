# The cycle, step by step — the full text of steps 0–12

The steps as `SKILL.md` numbers them, in full. Step 5's lane rules are in `lane-discipline.md`; the
fail-loud gate battery (step 7) and the cross-plan update with the handoff (step 9) are in
`gate-and-cross-plan.md`; the pause-and-ask gate is in `SKILL.md`.

## Contents
- Step-number crosswalk — the older numbering several references use
- Workflow — steps 0–4, step 5's opening, then steps 6–12

## Step-number crosswalk

| `SKILL.md` step | the same step in `full-procedure.md`, `gate-and-cross-plan.md`, `delegation.md`, `anchors-and-deferrals.md` and `the-bar.md` |
|---|---|
| 0 Orient | Step 0 |
| 1 Pick the next priority | Step 1 |
| 2 Clear blockers FIRST | Step 2 |
| 3 Plan it | Step 3 |
| 4 Design-audit the plan before lock | Step 3.5 |
| 5 Implement | Step 4 |
| 6 Review and fold | Step 5 |
| 7 Fail-loud gate | Step 6 |
| 8 Pin every deferral | Step 7 |
| 9 Cross-plan update (with the handoff) | Step 8 (8.1–8.4 the handoff) |
| 10 Self-audit before lock | Step 8.5 |
| 11 Commit and push | Step 9 |
| 12 Report and end | Step 10 |

`build-layout.md` and the file map in `SKILL.md` already use the `SKILL.md` numbering.

## Workflow

**Delegation is the default** — see the file map. The orchestrator judges; it should not be the one
hand-typing every edit or reading every subsystem.

0. **Orient.** Read `.plans/_handoff.md` first — the previous cycle's claim, not ground truth; where
   it disagrees with your own measurements, say so and correct it this cycle. Check `git status`,
   branch, last commit subject. Read plan-00 §0.1 and skim the anchor registry.
   ⚠ **ORIENTATION READS THE WORKING REGISTRY ONLY** — since 2026-09-16 that is ONE document,
   `-production.md`, which holds everything that is LEFT, and
   `DssHarness read-anchors --pending` is the whole list in one screen.
   (This line named `-harness.md` as a second working registry until the harness registry retired.)
   **Do not read `_deferred-anchor-registry-done.md` to choose work**: it is the archive, it is by
   far the larger of the two, and reading it to orient is how a closed row got recommended three
   times in this project's history. Establish a green
   baseline (`dssharness test --legs windows-x86_64-debug --json --time` — it builds first, and
   `--filter <regex>` iterates but never concludes). **A red baseline with no WIP-repair context
   is itself a pause gate** — present it; do not silently "fix it". AMENDED 2026-09-21 by "you do everything. I'm not your babysitter." — the agent decides it by measurement, writes why into the row and reports it veto-able, never silently; it pauses only for the cases of the decision gate (see SKILL.md)

   ★★★ **AND READ CI. NO STEP OF THIS SKILL USED TO, AND TWO RELEASE LEGS STAYED RED FOR TEN DAYS
   — SIX MATRIX RUNS — WHILE EVERY LOCAL LEG WAS GREEN.**
   ✔MEASURED 2026-09-14: the PR's Pipeline had been red on **every** run that executed the matrix,
   with two legs failing at `Test` while the four-leg local gate was 2179/2178/2148/2148 GREEN at
   the very same commit — because **no configuration either red leg runs in is one the local gate
   builds.** The operator had to point at it.

       dssharness check-ci-legs --branch <this branch>

   ★ ✔MEASURED 2026-09-17, same branch and same run id: the retired shell twins printed
   *"THE MATRIX DID NOT RUN … says NOTHING about the tree"* and then **exited 0**; the verb
   REFUSES, *"no job … is a leg, so nothing was verified about the tree. This is not a pass"*,
   **exit 2**. A nothing-was-verified run must never read as a pass to a caller that tests the
   exit code.

   - **A red leg is a HARD STOP the cycle reads before picking work**, and it is a FIX, so no other
     hard stop applies to repairing it (see *Hard stops* below) [→ SKILL.md *Hard stops*; full text in triggers-and-hard-stops.md](triggers-and-hard-stops.md). It goes in front of §0.1.
   - ⛔ **Never label, re-run, or push to re-trigger CI.** That is the operator's call. Read the
     verdict; reproduce the leg LOCALLY.
   - ⚠ **THE EVIDENCE HAS A THREE-DAY SHELF LIFE.** `gh run view --log-failed` answers **HTTP 410**
     on an expired run and the uploaded `test-logs-*` artefacts report `expired: true` after
     **three** days, not the `retention-days: 7` the workflow asks for — a repository setting
     silently overrides it. The instrument above reads job METADATA, which does not expire with the
     logs; a cycle that waits for the next red will be diagnosing it without logs.
   - ⚠ **`gh run list` alone is not evidence about the tree.** A run that failed at `label-check`
     has `run-tests` **skipped** and says nothing; the instrument reports that case by name.
   - ⚠ **A failed `Test` step is TWO hypotheses with opposite remedies** — a real failure, or a
     ctest `--stop-time` budget overrun. The instrument separates them by DURATION. **A real
     failure is FIXED. A budget is never raised to hide a suite that got slower.**
1. **Pick the next priority** from §0.1, top-to-bottom. An explicit argument overrides the auto-pick
   but is still subject to the bar, the pause gate, and the hard-stop checks. If §0.1 is dry, promote
   an *eligible* anchor (unconditional, or trigger already fired) into §0.1, then pick it.
   ★★★ **The candidate set is `_deferred-anchor-registry-production.md` — ALWAYS** (operator, 2026-08-25). A harness row
   enters a cycle only by BLOCKING the production priority (step 2), never on its own ticket; and a
   harness defect you FACE is fixed in this cycle, never filed for later. See the ruling above. [→ registry-and-priority.md](registry-and-priority.md)
2. **Clear blockers FIRST** — from the §0.1 "Blocked by" column *and* the registry *and* any
   "requires deferrals" note. Highest-priority blocker first, before the priority itself.
3. **Plan it** — delegate to `/feature-dev:feature-dev` or a `Plan` / `code-architect` agent.
4. **Design-audit the plan before lock** — an **independent** subagent applies the `dss-audit` bar to
   the *plan*. An agnosticism break, a tight slice, a speculative build, or a weak-test plan caught
   here is far cheaper than after the diff lands. Scale the rigor: trivial mechanical cycle → quick
   self-check; new engine mechanism → full independent review **and** a §B pause to the user. AMENDED 2026-09-21 by the decision gate — a decision brief reported veto-able, not a pause (see SKILL.md)
5. **Implement** — delegate in parallel by **disjoint file sets** (engine `.cpp/.hpp` vs
   `src/dss-config/**.json` vs `examples/` vs `tests/`), one agent per set, launched in one message.
   Name each agent's owned and forbidden paths. ★★ **At most FOUR reasoning agents live at once**
   (operator instruction 2026-08-19) — more work than that runs in waves of four. Script execution
   (builds, `ctest`, the guards, a remote leg) does **not** count against the cap; see
   `references/delegation.md`. Build the best long-term agnostic solution: extend
   config vocabulary, never branch the engine on identity. Any new `D-*` cited in `src/` is
   registered in the same commit.

   [→ the rest of step 5 — the lane rules — is in lane-discipline.md](lane-discipline.md)

6. **Review and fold** — `/pr-review-toolkit:review-pr`, plus the agnosticism pass and the CI-hazard
   screen. ⓘ There is no twin parity to review any more: a program under
   `.harness-config/runner/actions` is ONE `.py`, and `scripts_index_guard` refuses a `.sh` or `.ps1`
   there (see the layout section below) [→ actions.md, its last section](actions.md). **Re-review the fold** if folding changed logic; iterate to a fixed point. Passes that
   keep surfacing logic findings without converging are a pause signal — stop and report, do not grind.
7. **Fail-loud gate** — the mechanical battery, including the anchor-balance gate.
8. **Pin every deferral** discovered this cycle — and **CLOSE by MOVING**, never by editing a status
   in place. `DssHarness set-anchor <ANCHOR> --status closed --closing '...'` rewrites
   the row and lifts it out of the working registry into `_deferred-anchor-registry-done.md`; a lane
   handing you a verbatim row FILE goes through `apply-registry-row`, which hands its cells to the
   same door. A NEW row is `DssHarness write-anchor <ID> ...` (it WRITES unless given
   `--anchor-dry-run`). ⚠ Never
   hand-edit a table: `check-anchor-balance`'s partition arm fails the tree for a closed row left
   behind or an open row filed in the archive, and its ARM 6 fails it for a `Status` column that
   contradicts its own `Trigger` prose.
9. **Cross-plan update**, including rewriting `.plans/_handoff.md`, in the same commit as the code.
10. **Self-audit before lock** — an **independent** subagent runs the `dss-audit` rule-lens and
    guardrails on the complete, gate-passed cycle. On findings, return to step 5 and re-flow through
    this gate until clean. A finding implying a *design choice* is a pause gate, not a loop. AMENDED 2026-09-21 by the decision gate — decided, written into the row and reported veto-able (see SKILL.md)
11. **Commit and push.** `.plans/_handoff.md` must be staged in THIS commit — it ships with the work
    it describes, never in a follow-up. Subject `Cycle <id>: <concise summary>`; body lists anchors
    closed/opened plus the test delta; end with the repo's standard `Co-authored-by:` trailer.
    ⛔ **THE MODEL NAME IS NOT WRITTEN HERE, ON PURPOSE.** A hardcoded one is stale the moment the
    model changes, and this line carried `Opus 4.8` for weeks after the commits had moved on. Take it
    from the session's own attribution instructions, or read the newest one off the tree:
    `git log --format='%b' -20 | grep -im1 '^[Cc]o-authored-by:'`. ✔MEASURED 2026-09-16 at
    `305604f1`: the last ten commits all carry the same trailer, and it is the one that instrument
    prints. Push immediately. ⚠ **Pushing does NOT start the test matrix** — `pipeline-pr.yml` is
    gated on the operator's `Run Pipes` label and only the ungated landing-log job runs; push because
    the work should be on the remote, not because it buys a CI verdict. Stay on the current feature
    branch. **Open the PR here if the branch does not have one yet.**
    ⚠ **THIS STEP IS REACHED ONCE PER COMPLETED LANE SET, NOT ONCE PER CYCLE** — see
    *A COMPLETED SET OF LANES IS A COMMIT POINT* [→ lane-sets-and-folding.md](lane-sets-and-folding.md) above. A cycle running several sets of lanes lands
    several commits on one PR, and **the next set is seeded only AFTER this step**, so every lane
    can name a COMMIT as the tree it started from.
12. **Report and end** (contract below) [→ SKILL.md *Output contract*; full text in output-contract.md](output-contract.md). The invocation ends here; under `/loop` the next invocation
    begins the next cycle with fresh context.
