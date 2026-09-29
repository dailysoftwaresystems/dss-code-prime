# No follow-ups — a row you open, you close

The rule, the gate that measures it, the shipped-but-unmarked class that inflates the count, and what
happens when the lane that owns a row is gone.

## Contents
- The rule
- The gate — this is measured, not asserted
- A row that is SHIPPED but not marked ✅ is an anchor "rising" for free
- A lane that EXITS discharges nothing — and a lane that REPORTS has not landed

## ★★★★ NO FOLLOW-UPS. A ROW YOU OPEN, YOU CLOSE — operator ruling 2026-08-26

> *"I'M REALLY UPSET with the follow UPS.... OUR PROJECT SEEMS TO BE A FOLLOW UP!! THIS MUST STOP
> NOW."* … *"opened anchors that are not closed in the immediate cycle or the next are brutally
> rare exceptions now, not the rule. found something new that must be done? DO IT. Priority
> production, then harness. Fix harnesses immediately when they become a blocker."* … *"If I keep
> seeing anchors rising after implementations or fixes I'll be really pissed!"*

**This section is the enforcement of [[feedback-close-do-not-file]], which existed as a ruling
since 2026-08-24 and — ✔MEASURED 2026-08-26 — had NEVER BEEN WRITTEN INTO EITHER SKILL. That is
exactly why it eroded: a ruling that lives only in a memory index has no teeth at the moment of
the decision. The absence of this section IS the defect this section closes.**

### The rule

1. **A row you open, you close — in this cycle, or the next one.** Longer than that is a
   **brutally rare exception**, and an exception must be NAMED as one in the row, with the reason
   it could not be closed and the predicate that will close it.
2. **"Found something new that must be done?" → DO IT.** Discovering work is not a licence to
   file it. A new row is the LAST RESORT, never the first response to a finding.
3. **Priority is PRODUCTION, then harness. A harness defect that BLOCKS you is fixed the moment
   you face it** — in this cycle, in the lane that hit it. Never later.
4. **"Refused but not fixed" is NOT closed.** Neither is "measured and understood". Neither is
   "the row now describes the real scope". A row closes when the BEHAVIOUR changed.

### The gate — this is measured, not asserted

`dssharness check-anchor-balance` **already** implements *"a cycle may not end with more OPEN
deferral rows than it began"*, comparing **by id across both registries** — the only home a row can
have, because the door writes nothing else: a plan-side deferral table is no longer a sanctioned home
(✔MEASURED 2026-09-25, the program's plan-side count was 0). Run it against the cycle's OWN start
commit and treat a rise as a **HARD STOP**, not a note:

```
dssharness check-anchor-balance --base <cycle-start-sha>
```

ⓘ The per-bucket `--breakdown` this command used to carry went with the program: the buckets are the
Priority bands now, and `dssharness read-anchors --pending --open --band <P>` lists a band and ends with
its count (`registry-and-priority.md`). Of the program's DEBT lines, the Status-vs-Trigger disagreement
has a DssHarness form — the door refuses one, and `dssharness read-anchors --lint` reports one written by
hand (`anchors.triggerCarriesVerdict` in `.harness-config/config.json`); the rest — gated rows older than the
base, a row still blocked on a discharged blocker, an unclassifiable closing-work marker, an OPEN-vs-GATED
skew — have none.

⚠ **The delta the operator cares about is NET OPEN, and it must be ≤ 0.** A cycle that closes
three and opens three has, from outside, done nothing. Report closed / opened / **net** as three
separate numbers in the cycle report — a single total hides exactly the thing being asked about.

### ⚠ A row that is SHIPPED but not marked ✅ is an anchor "rising" for free

✔MEASURED 2026-08-26, and it is why this warning is here rather than in prose somewhere: of the
15 `D-OPT*` rows the instrument reported OPEN, **three were shipped work**, each whose own status cell opens with
*"🟢 DESIGN RECORD — SHIPPED 2026-07-29 (TF-C85), NOT deferred work"*, and each verified present
in the tree (`onFunctionNeutered`, `mandatoryNormalization`, the `funcNoOptimize` neuter, with
their pins). **They counted as OPEN solely because the status cell begins with 🟢 instead of ✅**
— the gate's deliberate "open unless explicitly closed" polarity, which is the right polarity and
must not be softened.

⇒ **The fix is never to relax the instrument. It is to mark a shipped row ✅ at the moment it
ships.** A design record of landed work is CLOSED work; if it must stay legible as a record, it
opens `✅ **CLOSED — DESIGN RECORD …**`. **Auditing for this class is part of the cycle**, because
an inflated OPEN count is indistinguishable from a cycle that is filing instead of fixing — and
the operator is reading that number.

### ⚠ A LANE THAT **EXITS** DISCHARGES NOTHING — operator, 2026-08-27
### ⚠ AND A LANE THAT **REPORTS** HAS NOT LANDED — *"complete means folded"*, operator 2026-08-28

**Both halves of the same rule, and neither is satisfied by a lane looking finished.** A lane's
report is a claim about ITS OWN WORKTREE; the project has nothing until the work is FOLDED into the
main tree and its rows applied. See *A COMPLETED SET OF LANES IS A COMMIT POINT* [→ lane-sets-and-folding.md](lane-sets-and-folding.md) below for the
definition and for what to do the moment a lane reports.

> *"if they don't address you must create another lane once they finish using /dss-cycle to
> address remanescent anchors from current running lanes"*

⚠ **This is not a new ruling — it is the rule above, applied to the one case the text did not
spell out: the lane that owns the row is GONE.** (Stated as such by the operator when it was first
written up as if it were new.) The rule binds the lane that opens a row; without this clause, a
lane's exit silently converts *"my row"* into *"nobody's row"*, and the anchor count stops falling
while every individual lane still looks compliant.

⇒ **When a lane finishes with assigned rows still OPEN — or having minted an anchor id in source
without filing its row — the orchestrator SPAWNS A NEW `/dss-cycle` LANE to close the remnants**,
naming them explicitly. "Next cycle will notice" is the follow-up culture this section ends.

⚠⚠ **AND THE BALANCE GATE IS BLIND TO HALF OF IT. `check-anchor-balance` COUNTS *ROWS*, so an
anchor cited in source with NO ROW IS INVISIBLE TO IT.** ✔MEASURED P40: it reported **"opened 0"**
while a finished lane had cited a minted anchor id across **six** files with
no row anywhere in `.plans/`. Only `.harness-config/runner/actions/check-anchor-registry/check-anchor-registry.py` catches
that class, and there it is the one telling the truth. [→ run it as `dssharness run check-anchor-registry`, which accepts an id found anywhere in `.plans/`; beside it, `dssharness check-anchor-citations --current-tree` resolves a cited id only to a row of the two registries, the only home a row can have](actions.md)
⇒ **Run BOTH before calling a cycle clean. A green balance is NOT evidence that nothing was
opened.**

✔MEASURED P40, the sideways-move class: a row was **re-verdicted** from
*"⏳ DEFERRED, trigger-gated"* to plain 🟠 OPEN by a lane that then exited — better described, still
open. **"Re-verdicted" is not closed**, exactly as *"refused but not fixed"* is not.
