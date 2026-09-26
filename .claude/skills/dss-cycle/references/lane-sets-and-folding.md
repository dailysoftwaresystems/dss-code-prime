# Lane sets — "complete" means folded, and a folded set is a commit point

When a lane's work counts as landed, and when a set of lanes is committed, pushed and succeeded by
the next set.

## Contents
- A completed set of lanes is a commit point (operator ruling 2026-08-28)
- "Complete" means FOLDED — a lane that has reported is not done; the sequence at a set boundary; why
  the next set is seeded only after the commit

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
queue it, not to dispatch the next lane. Fold, then apply the lane's rows, then re-derive the
balance. A row held out of the registry reads as `closed 0, opened 0`: no fix at all.

⇒ **A lane that has reported but cannot yet be folded** (a sibling is mid-edit in the same files, a
gate is running against the tree) **is still in flight and stays counted against the ≤4 cap.** Say
"reported, not folded" — never "done".

**The unit of shipping is the LANE SET, not the cycle.** When the four (or fewer) lanes in flight
have all reported **AND BEEN FOLDED**, and the tree is green, that is a landing point: **commit,
push, and open the PR if one is not open yet — THEN seed the next set.** Under `/loop` this repeats;
a long loop therefore produces a series of pushed commits on one PR rather than one enormous commit
at the end.

**The sequence at a set boundary:**
1. Every lane in the set has reported **AND BEEN FOLDED** — folded is the test, reported is not
   (see the definition above). Verify by looking at the MAIN tree, never by re-reading the lane's
   report: `git status` there is the evidence, and a lane's own green figure is evidence about a
   worktree that may no longer exist. A lane that exited without discharging its rows is not "done"
   either — spawn its remnant lane first (see the no-follow-ups ruling above) [→ no-follow-ups.md](no-follow-ups.md).
2. Apply every lane's ROWS, then re-derive the balance with
   `dssharness check-anchor-balance --base <cycle-start-sha>` and run `dssharness run check-anchor-registry`. [→ and `dssharness check-anchor-citations --current-tree`, which resolves a cited id only to a row of the two registries](no-follow-ups.md) **Both**, for
   the reason that section gives: a green balance is not evidence that nothing was opened.
3. The full gate on the FOLDED tree — the leg matrix, not one host.
4. `.plans/_handoff.md` rewritten in the SAME commit.
5. Commit `-s`, push (`-u` on the first push of a branch), open the PR if absent.
6. **Only then** create and seed the next set of worktrees.

⚠ **WHY THIS ORDER, AND NOT "SEED THE NEXT SET WHILE THE GATE RUNS".** A lane worktree is SEEDED
from the main tree's current state. Seeding before the commit means the next set inherits
uncommitted work whose provenance is a scratchpad manifest rather than a commit — and if the gate
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
