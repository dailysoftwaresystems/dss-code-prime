---
name: dss-cycle
description: >
  Advances the DSS Code Prime compiler by exactly ONE development cycle — picks the next priority
  from the plan-00 §0.1 stepper, clears its blockers first, plans it, design-audits the plan,
  implements the best long-term solution, reviews, passes the fail-loud gate, pins deferrals,
  updates the plans, self-audits, then commits and pushes. Use when the user asks to run a cycle, do
  the next cycle, continue the compiler work, advance the plan, work the stepper, or pick up the
  next priority — and whenever /loop drives continuous autonomous progress — even if they never say
  "skill". One cycle per invocation. It DECIDES forks and records why, pausing only on the five
  cases of its decision gate; it never guesses, never workarounds, and never breaks source (language) /
  target (processor) / linker (object-format) agnosticism. NOT for judging finished work (use dss-audit),
  reconciling plan staleness (use dss-plan-sweep), or running the multi-host matrix (use dss-cross-leg-test).
user-invocable: true
argument-hint: "[optional: specific priority or anchor to take this cycle]"
---

# DSS Code Prime — Development Cycle Loop

One cycle per invocation: pick → clear blockers → plan → design-audit → implement → review → gate →
pin → cross-plan → self-audit → commit → push.

This file holds the checklist, the hard rules (one line each) and the map; every ruling is kept whole,
verbatim and with its measurements, in the reference its line names — open it before acting on the
details. Read `references/the-bar.md` at the start of every cycle.

## When to use

- Advancing the compiler by one real priority, autonomously or under `/loop`.
- Finishing a `… WIP` cycle already in flight — that *is* this cycle's priority.

**Not this skill:** judging finished work → `dss-audit`. Plan staleness → `dss-plan-sweep`.
Multi-host matrix → `dss-cross-leg-test`.

**Conventions authority:** the `dss-code-prime` skill wins on any conflict.

## The cycle — copy this checklist and tick it off

```
Cycle <id>:
- [ ] 0  Orient — handoff, git status, plan-00 §0.1, the WORKING registry only, green baseline, READ CI
- [ ] 1  Pick the next priority — §0.1 top-to-bottom; candidates come from the PRODUCTION registry
- [ ] 2  Clear blockers FIRST — the highest-priority blocker before the priority itself
- [ ] 3  Plan it — delegated; the orchestrator judges the plan
- [ ] 4  Design-audit the plan before lock — an INDEPENDENT subagent applies the dss-audit bar
- [ ] 5  Implement — lanes by disjoint FILE sets, at most four reasoning agents live
- [ ] 6  Review and fold — re-review to a fixed point; passes that never converge → pause, do not grind
- [ ] 7  Fail-loud gate — full battery + anchor balance; a red you cannot self-repair → STOP, never push
- [ ] 8  Pin every deferral — a row closes by MOVING (`set-anchor`)
- [ ] 9  Cross-plan update — including the rewrite of `.plans/_handoff.md`, same commit as the code
- [ ] 10 Self-audit before lock — INDEPENDENT; a finding → back to 5 and re-flow; a design choice → decided, reported
- [ ] 11 Commit (`-s`, the handoff staged in THIS commit) and push — once per completed, folded lane set
- [ ] 12 Report (the output contract below) and end
```

Full text: `references/workflow-steps.md`, whose crosswalk maps the older numbering several references
use (3.5 design-audit, 6 gate, 8 cross-plan, 8.5 self-audit, 9 commit, 10 report). Files below are in `references/`:

| step | what must hold | open |
|---|---|---|
| 0 | the handoff is the previous cycle's claim, not ground truth; orient from `_deferred-anchor-registry-production.md` (`DssHarness read-anchors --pending`), never the `-done.md` archive; a red baseline with no WIP-repair context is decided and reported veto-able, never silently (AMENDED 2026-09-21); READ CI (`dssharness check-ci-legs`): a red leg is a HARD STOP on picking work, and repairing it is a FIX | `workflow-steps.md`, `round-gate-and-ci.md` |
| 1 | an explicit argument overrides the auto-pick, never the bar, the pause gate or the hard stops; a dry §0.1 promotes an ELIGIBLE anchor (unconditional, or its trigger fired); a harness row never enters on its own ticket | `workflow-steps.md`, `registry-and-priority.md` |
| 2 | blockers come from §0.1's "Blocked by" column, the registry, and any "requires deferrals" note | `workflow-steps.md` |
| 3 | delegate the plan (`/feature-dev:feature-dev`, a `Plan` or `code-architect` agent) | `delegation.md` |
| 4 | independence is the point; a new engine mechanism also gets a decision brief to the user, veto-able | `workflow-steps.md`, `full-procedure.md` |
| 5 | each lane's owned and forbidden PATHS named; its own build tree and scratch directory; a new `D-*` cited in `src/` registered in the same commit | `delegation.md`, `lane-discipline.md`, `worktrees.md`, `build-layout.md` |
| 6 | `/pr-review-toolkit:review-pr`, the agnosticism pass and the CI-hazard screen | `workflow-steps.md`, `full-procedure.md` |
| 7 | net OPEN ≤ 0 against the cycle's start commit, and `check-anchor-registry` run too; a round of lanes owes EIGHT runs | `gate-and-cross-plan.md`, `round-gate-and-ci.md`, `no-follow-ups.md` |
| 8 | `DssHarness set-anchor <ANCHOR> --status closed --closing '...'`; a new row is `DssHarness write-anchor`; both WRITE unless given `--anchor-dry-run` | `anchors-and-deferrals.md`, `registry-and-priority.md` |
| 9 | plans updated in the SAME commit as the code; the handoff answers its five questions | `gate-and-cross-plan.md` |
| 10 | the `dss-audit` rule-lens and guardrails on the complete, gate-passed cycle | `workflow-steps.md`, `full-procedure.md` |
| 11 | subject `Cycle <id>: <concise summary>`; the `Co-authored-by:` trailer from the session, never hardcoded; pushing does NOT start CI; open the PR if absent; seed the next lane set only after this step | `workflow-steps.md`, `lane-sets-and-folding.md` |
| 12 | the anchor line carries numbers; `next:` matches the handoff's top NEXT entry | `output-contract.md` |

## Hard rules — one line each

**The bar**
- Non-negotiable, re-read every cycle: agnostic (no language/arch/format identity branch; reuse the
  pipeline's verbs), best long-term with no workaround, the hard part lands this cycle, fail loud,
  strict red-on-disable tests, every issue anchored AND handled → `references/the-bar.md`.
- ★★★ **The goal is to WORK** (2026-08-19): if ANY reference (gcc, clang, MSVC) compiles and runs a
  correct construct, DSS must too; a reference's failure is never evidence against DSS; probe each
  reference separately → `references/reference-compilers.md`.
- ★★★★ **The disjunction decides ACCEPTANCE, not MEANING** (2026-08-28): references splitting on what
  a valid program means is a fork the agent DECIDES (2026-09-21) by measurement and the references' own
  documentation — the refusal cost in the row, the decision reported veto-able → same file.
- ★★★★ **The union is over what WORKS** (2026-09-02): no privileged reference, no tiebreaker vertex; a
  quality split is not a meaning fork — measure which reference works (the defective AND the healthy
  case, the property named first) and match it; record which one, on what measurement → same file.

**Priority and the registry**
- ★★★ **Production anchors are the priority, ALWAYS** (2026-08-25): step 1 picks from
  `_deferred-anchor-registry-production.md`; a harness defect is fixed the moment it is faced, NEVER
  later, and still gets its row → `references/registry-and-priority.md`.
- Two files: `_deferred-anchor-registry-production.md` (every still-open row — orient from it alone)
  and `_deferred-anchor-registry-done.md` (the archive — never read it to choose work). ★★★ **Closing a
  row MOVES it** there, reopening moves it back (2026-09-01); `check-anchor-balance` refuses both a
  closed row left behind and an open row filed in the archive.
- A row is six cells, `| Anchor | Priority | Status | Trigger | Closing work | Cross-refs |`;
  `🔵 DISCLOSED` is for debt this cycle FOUND, never for debt it created.
- The door is `DssHarness write-anchor` / `set-anchor`, which WRITE unless given `--anchor-dry-run`;
  a cell is read only with `read-anchor <ID> --json`; a row is never hand-assembled, hand-edited or
  hand-read → also `references/anchors-and-deferrals.md`.
- Never quote an anchor count — re-derive it: `dssharness check-anchor-balance` for the balance,
  `dssharness read-anchors --pending --open [--band <P>]` for the rows, whose listing ends with the count.
- A deferral is admissible only when all of §F.0 (a)–(c) hold — a NAMED blocker, deferring is the best
  long-term call, the reason survives the negative test; when in doubt, do the work →
  `references/anchors-and-deferrals.md`.
- ★★★★ **No follow-ups** (2026-08-26): a row you open, you close — this cycle or the next; found new
  work → DO IT; "refused but not fixed" is not closed; NET OPEN ≤ 0 against the cycle's start commit,
  and a rise is a HARD STOP; mark a shipped row ✅ the moment it ships → `references/no-follow-ups.md`.
- A lane that EXITS discharges nothing (2026-08-27): spawn a new `/dss-cycle` lane for its remnant
  rows; run BOTH `check-anchor-balance` and `check-anchor-registry` — a green balance is not evidence
  that nothing was opened → same file.

**Lanes**
- Delegation is the default; at most FOUR reasoning agents live at once (2026-08-19); scripts do not count,
  memory does: two heavy local jobs at most, each admitted below 76% committed memory, on the orchestrator's
  GO, cores from config, no `-j` (✔MEASURED P68 round 9); every brief says "anchor AND close" → `references/delegation.md`.
- Contention is per FILE; the orchestrator is a lane too; at most ONE lane holds `src/dss-config/**`
  or `src/core/types/*schema*`; `.plans/**` is a guard input; each lane gets its own build tree and
  scratch directory → `references/lane-discipline.md`.
- A lane's registry rows go to a ROWS DIRECTORY in its scratch, at an absolute path — anchor-rows'
  `<dir>/<ANCHOR ID>/<cell>.txt`, one file per cell, VERBATIM — the report naming the directory, the ids and each
  cell file's md5; all else travels INLINE (rule 9); a brief states an interface only if its author ran it;
  an anchor id is never line-wrapped → same file.
- ★★★★ **"Complete" means FOLDED** (2026-08-28): on a lane's report, fold it, apply its rows, re-derive the
  balance; a reported-but-unfolded lane is still in flight → `references/lane-sets-and-folding.md`.
- ★★★ **A completed, folded, green lane set is a commit point** (2026-08-28): commit, push, open the PR
  if absent — THEN seed the next set → same file.
- ⛔ **Never `git stash` / `checkout --` / `clean` / `reset` in the shared tree** — the ban is BLANKET;
  before editing a file you own, copy it into your scratch directory (the only sanctioned undo); stage by
  explicit path, never `git add -A` → `references/lane-discipline.md`, `references/gate-and-cross-plan.md`.
- ★★★ **A lane worktree lives at `<repo>/.worktrees/<short-name>`** (2026-08-26), made by
  `dssharness create-worktree`, then — MANDATORY, before any work — `lane-fold.py seed <lane>`, which resets
  a stale same-name seed manifest that would make the fold SILENTLY DROP the lane's work (P57); keep names
  short (≤10 characters, MAX_PATH); the lane that takes a worktree owns removing it; a byte-changing
  measurement runs in a worktree, never in the shared tree → `references/worktrees.md`.

**Gate, CI and hosts**
- ★★★★ **The end-of-round gate is `{Debug, Release} × four legs` — EIGHT runs** (2026-09-14), reported
  with the build type beside each number → `references/round-gate-and-ci.md`.
- ⛔ **Never make CI run** — no label, no re-run, no push to re-trigger; ✅ always READ it
  (`dssharness check-ci-legs`): a red leg is a HARD STOP on proceeding, never on fixing → same file.
- ★★★ **A gate host holds the repo and nothing else** (2026-08-25): a push is a SYNC, the main tree's sync
  never carries a worktree — a worktree's own host copy goes with `dssharness delete-worktree` — and the
  cleanup is the cycle's job; ★★★ **every leg host keeps a clone, and the leg cleans up after itself**
  (2026-08-26): PREPARE → SYNC → RUN → RESTORE, the restore on every exit path; `-fd`, never `-fdx` →
  `references/leg-hosts.md`.
- **One build root, `build/`** (2026-08-17): distinct builds are subdirectories — DssHarness names one
  per leg — and a lane builds inside its own worktree, whose build goes with it → `references/build-layout.md`.

**Tools**
- ⛔★★★ **Build, sync, test and worktree management go through DssHarness, never a script**
  (2026-09-24): the one exception is a DssHarness bug, reported, and the one NAMED INTERIM is the lane-fold
  program's `seed` / `land` until the action declares them as manual steps; a big, reusable or shell-hard
  program becomes an ACTION → `references/dss-harness.md`.
- ★★★ **Use the program that exists — fix it rather than routing around it** (2026-08-19) → `references/actions.md`.
- An action added, renamed, deleted or repurposed updates both indexes in the same commit
  (`dssharness run check-scripts-index --manual-step write`); no `.sh` and no `.ps1` under the actions
  root (2026-09-21) → same file.
- ★★★ **Any issue found in DssHarness is reported in the cycle that finds it** (2026-09-16): SENT by the agent to
  the repo-harness session — the local session working in the repo-harness checkout — the operator told it was
  sent (2026-09-21); never worked around locally → `references/output-contract.md`, `references/dss-harness.md`.

**What gets said, and decisions**
- ★★★ **Never cite a line number** (2026-08-19) — cite a method name, a comment id or a defined anchor;
  label every claim MEASURED / DOCUMENTED / INFERRED; grep the registry for a matched control before
  commissioning an experiment → `references/operator-discipline.md`.
- ★★★ **A §B trigger is a PREDICATE, not a ritual** (2026-08-17): measuring it false discharges it —
  recorded in the row, flagged in the report; an inconclusive measurement escalates. ★★★★ **A trigger
  waiting on a component WE build is not a gate** (2026-08-26): build the prerequisite, then close the row,
  in the same cycle → `references/triggers-and-hard-stops.md`.

## The decision gate — the most important behavioral rule (AMENDED 2026-09-21)

Operator ruling 2026-09-21, verbatim: *"you do everything. I'm not your babysitter."* The loop is
autonomous for execution AND for decisions (the gate as written before: `references/triggers-and-hard-stops.md`):

- **Decide, do not ask.** A fork (a MEANING fork included), a documented-behaviour change and a new engine
  mechanism at step 4 are DECIDED by the agent, by measurement and the references' own documentation. A fork
  is *real* only if you can state ≥2 concrete, defensible long-term designs; **never invent a fork to escape the work.**
- **Write the rationale into the row** (and the owning plan, so it is not re-litigated), and **report the decision,
  veto-able, in the decision-brief shape:** (1) the problem, in one or two sentences; (2) 2–4 candidate
  **long-term** solutions, each no-workaround and agnostic — if one breaks agnosticism say so and why it is still
  listed; (3) each one's trade-off — what it costs, buys, forecloses; (4) **the decision and why**. A missing
  fact is measured, never invented.
- **STILL PAUSE — one crisp question, never a list — for:** (a) what no measurement can answer and no
  standing order covers; (b) the OPT7 / inlining hard stop, a standing order the later ruling did not
  revoke; (c) a correctness-critical anchor whose negative miscompile-pin cannot be constructed; (d) a
  gate's ESCAPE HATCH — a deferral, a net-open rise carried, a ratchet baseline grown — which standing
  orders keep the operator's (close, do not file; no follow-ups; a ratchet only comes down); (e) review
  passes that never converge (step 6). While paused, do not start a different cycle. The execution
  stops — a red the cycle cannot self-repair (step 7), a red CI leg (step 0) — are unchanged.
- **A gated anchor whose trigger has not fired** is skipped and reported: "trigger not fired".

## Hard stops (full text: `references/triggers-and-hard-stops.md`)

★★★ **A hard stop gates OPENING A CAPABILITY, never FIXING A DEFECT** (operator ruling 2026-08-15):
*"does this make something CORRECT that is currently WRONG?"* → FIX, no gate; *"does this make DSS able
to do something it has never done?"* → capability, the gate applies.
- **OPT7 / inlining** — opening the arc is a supervised cycle, never autonomous; fixing a defect in
  inlining that already ships is not gated.
- **Trigger-gated anchors** — not a TODO: an unfired trigger means skip, and report "trigger not fired".
- **Correctness-critical anchors** — ship the negative miscompile-pin, or STOP and bring a decision brief.
- **A red CI leg** (step 0) — no new work until it is diagnosed; the repair is a FIX; never make CI run.

## Stop-command handling

On a stop mid-cycle: **finish the current cycle's full flow through commit and push**, then halt. Do
not begin a new cycle. Two things the stop does not override — **a gate you cannot reach cleanly**
(report, never push broken), and **an unanswered pause gate** (you cannot fabricate a resolution;
commit WIP only if legitimate, re-present the brief, halt). The stop tightens the loop to a close; it
never lowers the bar.

## Output contract (full text: `references/output-contract.md`)

★★★ **Silence is the default — emit nothing the operator does not need in order to proceed** (operator,
2026-08-17). The complete list of what is worth emitting:
1. **A failure or a blocker** — with the measurement, and what you are doing about it.
2. **A decision brief or a pause** — the one case where length is justified: options, trade-offs, and the decision and why (reported veto-able), or the one crisp question of a pause.
3. **Done / not done** — when the operator's next action depends on it.
4. **The final report** — below.
5. **A direct answer to a direct question.**
6. ★★★ **A DssHarness finding** — even when it neither fails nor blocks this cycle: what was run, what
   happened, what should have happened, and the repo-harness path + SYMBOL (never a line number) — SENT by
   the agent to the repo-harness session, the operator told it was sent (2026-09-21).

**Form, not just category:** the fact and its measurement, then stop — no significance commentary, no
meta, no derivation, no roads not taken, no relayed lane report; 1–3 lines unless it is a decision brief or a pause.
Rigor is unchanged: the detail goes into the row and the handoff, not the reply.

A one-line cycle summary — priority closed, anchors touched, test delta, commit hash — plus:

```
anchors: opened N, closed M, net ±K — OPEN was <before>, now <after>
next: <one line, matching the top NEXT entry in .plans/_handoff.md>
```

**The anchor line is MANDATORY and carries numbers, not an adjective.**

## File map

- Read `references/the-bar.md` **at the start of every cycle** — the six non-negotiables in full,
  with the worked cases behind each. This is the standard everything else here serves.
- Read `references/delegation.md` before steps 3–5 — what to delegate, how to split by disjoint file
  sets, and what the orchestrator keeps.
- Read `references/gate-and-cross-plan.md` at steps 7 and 9 — the full fail-loud gate battery and the
  cross-plan update including the handoff rewrite.
- Read `references/anchors-and-deferrals.md` at step 8, and whenever pinning a deferral or judging
  whether an anchor is eligible.
- Read `references/operator-discipline.md` when reporting or claiming anything — the bar applies to
  the operator, not only to the code, and it opens with the **never-cite-a-line-number** rule.
- Read `references/dss-harness.md` **before running anything that touches a leg, a worktree or an
  anchor** — the tool every build, sync, test and worktree operation goes through, and that file
  says which of its verbs exist, which of this repository's programs it runs as actions, what this
  repository's `.harness-config/config.json` declares, and the exit codes to act on. ⛔ A defect in
  the tool is a repo-harness issue, never a local workaround, **and it is REPORTED in the cycle that
  finds it** (ruling 2026-09-16) — SENT by the agent to the repo-harness session, the operator told it
  was sent (2026-09-21); see output-contract item 6.
- Read `references/actions.md` **before writing any script, probe, or one-off shell pipeline** —
  the index of every program this repository already ships, each with its purpose. Most of what a
  cycle needs is already there, and re-typing it inline re-opens the edge cases it was taught
  (`wsl.exe` quoting, heredocs eating backslashes, unanchored rsync excludes, ssh dropping PATH).
- Read `references/worktrees.md` before any byte-changing measurement or agent worktree operation.
- Read `references/build-layout.md` before creating ANY build tree (step 5) and before reporting a
  cycle complete (step 11) — **one root `build/`, subdirectories for distinct builds, and a lane's build
  goes with its worktree.** Operator instruction 2026-08-17; a lane worktree that survives its landing
  blocks the completion report the same way the anchor-balance gate does.
- Read `references/workflow-steps.md` when a checklist line is not enough — steps 0–12 in full.
- Read `references/full-procedure.md` for the longer, older-numbered pause gate, steps, hard stops, stop handling.
- Read `references/reference-compilers.md` before deciding what a reference's behaviour is evidence FOR.
- Read `references/registry-and-priority.md` at step 1 (what to pick) and step 8 (writing a row).
- Read `references/no-follow-ups.md` at steps 7–8, and whenever a lane finishes with rows still open.
- Read `references/lane-discipline.md` before a lane brief (step 5) or a message to a live lane, and when a lane reports.
- Read `references/lane-sets-and-folding.md` when a lane reports, and at every lane-set boundary.
- Read `references/round-gate-and-ci.md` at step 0 (reading CI) and at the end of every round (step 7).
- Read `references/leg-hosts.md` before trusting any remote leg.
- Read `references/triggers-and-hard-stops.md` when a row is gated, a §B trigger is live, or a hard stop may apply.
- Read `references/output-contract.md` before emitting anything, and at step 12.

## Failure modes this skill exists to prevent

- **Calling a lane "done" when it has only REPORTED.** *"Complete means folded"* (operator,
  2026-08-28). The report describes a worktree; until the fold, the main tree has nothing — and this
  one ships green, because the tests that would have caught the absence are in the tree that was
  never folded. Check the MAIN tree, not the report.
- **Guessing past a decision.** A real fork is DECIDED — by measurement and the references' own
  documentation, written into the row, reported veto-able — never guessed; an unfired trigger is
  skipped and reported; the decision gate's pause cases are a pause — not a default, not a pivot to other work.
- **Inventing a fork to avoid the hard part.** If you cannot state a second defensible design, the
  hard part lands this cycle.
- **Breaking agnosticism** by branching the engine on language, arch, or format identity instead of
  extending config vocabulary.
- **Shipping a test that passes both ways**, or closing a correctness-critical anchor without its
  negative miscompile-pin.
- **Auditing your own work.** Steps 4 and 10 are independent subagents precisely so they cannot
  rubber-stamp reasoning they authored.
- **Committing the handoff separately** — it then describes a tree that no longer exists.
