# Lane sets — "complete" means folded, and a folded set is a commit point

When a lane's work counts as landed, and when a set of lanes is committed, pushed and succeeded by
the next set.

## Contents
- EVERY FOLD is a commit + push — a checkpoint between waves (operator ruling 2026-10-08)
- A completed set of lanes is a commit point (operator ruling 2026-08-28)
- "Complete" means FOLDED — a lane that has reported is not done; the sequence at a set boundary; why
  the next set is created only after the commit

## ★★★★ EVERY FOLD IS A COMMIT + PUSH — A CHECKPOINT BETWEEN WAVES — operator ruling 2026-10-08

> *"please commit + push between waves, It's totally wrong 600+ files without push"* — operator, on a
> round whose main tree held four folds and 665 uncommitted paths for a day and a half.

The set boundary below is the ROUND's commit. It is no longer the first one. **A lane's fold is
committed and pushed as soon as its fold checks pass** — while the other lanes of the set are still
working. Unpushed work is lost with one disk and invisible to the operator; the size of a fold is the
most the main tree may ever hold uncommitted.

1. **The order is fold → checks → refresh the siblings → write the inventories → commit → push.** The
   checks are the fold's own: the anchor balance against the cycle's start commit, the registry lint,
   `check-anchor-citations`, `check-anchor-registry`, and — where the fold changed
   `tests/mutations/arms.registry` — the registry's validation and the state of every arm the fold added:
   `passed` on the fold's bytes with its run id, or named as owed and on which leg (an arm that names a
   Windows leg is swept here, on the main tree: a worker does not fit beside a lane's worktree there —
   mutation-arms.md). The siblings are refreshed BEFORE the commit:
   ✔MEASURED 2026-10-08, `refresh-agent` sees only the main tree's UNCOMMITTED changes — after the
   commit it answers "holds the main tree's copy" for a path whose lane copy is older (reported to the
   tool's own repository). A sibling in the middle of a run is refreshed when the run ends; the commit
   waits for it rather than leaving it stale.
2. **The inventory baselines are written at every fold**, not at the round's end — the emitted anchor
   ids, the doc census, the plan citations — so a checkpoint's guards are green.
3. **A checkpoint is not a gate, and its message says so.** It states what ran (each lane's five legs
   on its own tree, the fold checks) and what did not (the eight-run gate on the combined tree, the
   sqlite recompile, the final review), names every known red, carries the anchor line with numbers,
   and says *do not run the pipes on this commit* when a red is known. The subject is
   `Cycle <id> round <n>, checkpoint <k>: <what folded>`. `.plans/_handoff.md` carries a short
   in-flight block at the top of its state section in the same commit — what the checkpoint carries,
   what it does not, where the round stands — and is REWRITTEN only at the round's close.
4. **The agents stay open across a checkpoint.** A lane's worktree stays on the base commit it was
   created on; the main tree's HEAD moves away from it. So nothing the orchestrator runs may read "the
   main tree's HEAD" as a lane's base — the base is the lane worktree's own HEAD, and the balance is
   always taken with `--base <the cycle's start commit>`. Lanes read registry rows from the MAIN tree;
   their worktree's copy of the registry is not refreshed.
5. **Lanes hand back at each natural fold point** — a coherent item set that is pinned, its arms
   declared and `passed`, and green on five legs, the P0 work first so it is pushed first — instead of accumulating one large
   wave. The independent re-review follows the commit; its findings lead the lane's next fold.
6. **The round's close is unchanged**: every lane re-reviewed clean, the eight runs, the sweep of every
   mutation arm on the leg it names, the sqlite recompile, the final review, the agents deleted, the handoff rewritten — one more commit and push.

## ★★★ A COMPLETED SET OF LANES IS A COMMIT POINT — operator ruling 2026-08-28

> *"once a set of 4 lanes are fully done and green, that's a good time to commit + push (and create
> the PR if not done yet) before the next set of 4 lanes in the loop"*
> … *"complete means folded"* — operator, clarifying the same day.

## ★★★★ "COMPLETE" MEANS **FOLDED**. A LANE THAT HAS REPORTED IS NOT DONE.

**A lane's report is a claim about ITS OWN WORKTREE. Until its work is folded into the main tree,
the project has nothing.** A reported-but-unfolded lane is still IN FLIGHT, and every one of these
is FALSE of it: "the lane is done", "that work has landed", "the set is complete", "we can count
those rows".

⚠ **THIS IS A DEFINITION, NOT AN EMPHASIS, AND IT BINDS EVERY USE OF THE WORD.** The failure it
prevents is silent and reads as success: a lane reports a green tree, the orchestrator writes it
into the ledger as finished, the set is declared complete, and the commit ships WITHOUT it. Nothing
reddens, because the tests that would have failed are in the worktree that was never folded.

⇒ **On a lane's report, the orchestrator's next action is to FOLD it** — not to summarise it, not to
queue it, not to dispatch the next lane. `dssharness fold-agent <o> <a>`, read the dry run, then `--apply`
with `--new` for the report's new ids and `--accept-lost` for each lost cell read — it checks every row, writes
the lane's work, then applies its rows all or nothing;
a file that changes under it stops it part way (21), and the same command run again finishes it — then
re-derive the balance. A row held out of the registry reads as `closed 0, opened 0`: no fix at all
(orchestration.md).

⇒ **A lane that has reported but cannot yet be folded** (a sibling is mid-edit in the same files, a
gate is running against the tree) **is still in flight and stays counted against the ≤4 cap.** Say
"reported, not folded" — never "done". A folded lane keeps its agent — and its slot under `--parallel` —
until `delete-agent` after its last review.

**The unit of shipping is the LANE SET, not the cycle.** When the four (or fewer) lanes in flight
have all reported **AND BEEN FOLDED**, and the tree is green, that is a landing point: **commit,
push, and open the PR if one is not open yet — THEN, with the finished set's agents already deleted after
their last review (step 5 below), create the next set.** Under `/loop` this repeats;
a long loop therefore produces a series of pushed commits on one PR rather than one enormous commit
at the end.

**The sequence at a set boundary:**
1. Every lane in the set has reported **AND BEEN FOLDED** — folded is the test, reported is not
   (see the definition above). Verify by looking at the MAIN tree, never by re-reading the lane's
   report: `git status` there is the evidence, and a lane's own green figure is evidence about a
   worktree that may no longer exist. A lane that exited without discharging its rows is not "done"
   either — spawn its remnant lane first (see the no-follow-ups ruling above) [→ no-follow-ups.md](no-follow-ups.md).
2. Every lane's rows went in with its fold; re-derive the balance with
   `dssharness check-anchor-balance --base <cycle-start-sha>` and run `dssharness run check-anchor-registry`. [→ and `dssharness check-anchor-citations --current-tree`, which resolves a cited id only to a row of the two registries](no-follow-ups.md) **Both**, for
   the reason that section gives: a green balance is not evidence that nothing was opened. Then the DEBT the
   balance does not report, on this machine's tree: `dssharness run anchor-debt-debt --input
   base=<cycle-start-sha>` — it refuses a row the cycle added or edited into debt.
3. The full gate on the FOLDED tree — the leg matrix, not one host.
4. The review to a fixed point and the independent self-audit (checklist steps 6 and 10). A finding on a
   lane's work sends the lane back to its SAME open agent — its subagent resumed, then `refresh-agent` where
   the main tree moved under it, then fold again — never to a new agent while the old one is open. ANY change
   a finding brings into the main tree, the orchestrator's own fixes included, sends the set back to step 2:
   the balance, the registry check and the full gate re-run on the tree as it now stands (workflow-steps.md
   step 10, re-flow).
5. After each lane's LAST review, `dssharness delete-agent <o> <a>`: read the dry run — anything left to fold,
   or a row still to apply, means the gate did not see the lane's final state: fold it and re-gate — then
   `--apply`. It keeps the lane's evidence and transcripts and removes its worktree and every
   host copy (orchestration.md).
6. `.plans/_handoff.md` rewritten in the SAME commit.
7. Commit `-s`, push (`-u` on the first push of a branch), open the PR if absent.
8. **Only then** create the next set's agents (`create-agent` seeds each one).

⚠ **WHY THIS ORDER, AND NOT "CREATE THE NEXT SET WHILE THE GATE RUNS".** `create-agent` SEEDS a lane
from the main tree's current state. Creating it before the commit means the next set inherits
uncommitted work whose provenance is the agent's seed record rather than a commit — and if the gate
then reddens and the tree changes, every seeded lane is measuring a tree that will never exist.
Seeding from a COMMITTED tree makes "what did this lane start from" answerable by `git`, which is
the only durable answer.

⚠ **A SET BOUNDARY IS NOT A LICENCE TO STOP MID-LANE.** Do not interrupt lanes that are still
running to force a boundary. The boundary is where the set *finishes*; the rule sets the CADENCE of
committing, it does not add a new reason to pause. Work already in flight continues to completion.

⚠ **AND IT DOES NOT WEAKEN THE GATE.** "Green" here means the gate every commit already owes —
this ruling changes HOW OFTEN that gate runs and a commit lands, never what the gate consists of.
A set that cannot go green does not get committed because a boundary arrived.
⇒ ★ **AND WHAT IT CONSISTS OF IS NOW EIGHT RUNS, NOT FOUR** — see the next section [→ round-gate-and-ci.md](round-gate-and-ci.md).
