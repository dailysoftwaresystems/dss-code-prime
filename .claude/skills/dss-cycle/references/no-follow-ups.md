# No follow-ups — a row you open, you close

The rule, the gate that measures it, the shipped-but-unmarked class that inflates the count, and what
happens when the lane that owns a row is gone.

## Contents
- The rule
- `DISCLOSED` is not closed — and "close anchors" means the disclosed ones too (2026-10-08)
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

### ★★★★ `DISCLOSED` IS NOT CLOSED — AND "CLOSE ANCHORS" MEANS THE DISCLOSED ONES TOO — operator ruling 2026-10-08

> *"disclosed is not closed, and when I tell to close anchors, is to not be a fucking lazy to open
> disclosed fucking issues when closing other issues. Try the best to close disclosed issues along
> the way, unless it's really a big thing not doable in the context, rare exceptions, stuff like
> that"*

✔MEASURED at the moment of the ruling (cycle P69, round 1, `dssharness check-anchor-balance` against
the cycle's start commit): **"24 closed, 22 opened (1 created, 21 disclosed); counted -23"** — the
gate read **−23** while the registry had gone from **620 open rows to 618**. From outside, a round
that had closed two dozen rows had done almost nothing, and the gate said it was the best round in
the project. Several of the 21 had been filed BY RULING — "scheduled for a later round for cycle size
alone" — on work the lane that found it had sized at one configuration key and about 150 lines.
`DISCLOSED` exists so that a pre-existing defect is written down instead of hidden (see
`registry-and-priority.md`); it was being used as the cheap way to finish a finding.

1. **A disclosed row is OPEN WORK, and filing it finishes nothing.** The gate's exemption — a
   disclosed row is not a *rise* — is an honesty device, never a discharge. The number the operator
   reads is the REAL open count.
2. **A defect met while closing another row is FIXED in the same wave, by the lane that met it** —
   pre-existing or not. "It pre-dates the cycle" decides which STATUS word a row carries *if a row
   must exist at all*; it never decides whether the work is done now. Rule 2 above ("found
   something new → DO IT") binds HEAD's debt exactly as it binds the cycle's own.
3. **A new disclosed row is a RARE EXCEPTION**, admissible only when the fix is really big and not
   doable in the context: a mechanism that needs its own plan and design audit, a prerequisite that
   another lane must build first, a file another lane holds and cannot merge in time.
   ⛔ **"Scheduled for a later round for cycle size alone" is NOT a blocker**, and it is no longer an
   acceptable closing cell for work a lane can do inside its wave. Neither is "it is not what this
   lane was briefed for".
4. **The lane does not grant itself the exception, and the orchestrator does not grant it to keep a
   round short.** The lane states the MEASURED size — the files, what must be built, what must be
   measured first — and the orchestrator rules; the default answer is *close it now*. A long round
   that closes what it finds is the instruction; a short round that files what it finds is the
   failure this section names.
5. **Existing disclosed rows are closed along the way.** A lane that touches the file or the
   mechanism a disclosed row names closes that row in the same wave; a new round takes the disclosed
   rows FIRST, highest band first, before any new capability; and a brief for a lane names the
   disclosed rows in its path as work, not as background.
6. **Report three numbers, never the counted one alone:** closed · opened, split into created and
   disclosed · the REAL net (open now minus open at the cycle's start). A report that says only "the
   balance holds" has hidden exactly the figure being asked about.
7. **A reviewer treats a disclosed row left open without a real blocker as a finding**, and so does
   the orchestrator's own fold check: read each disclosed row a lane files as a claim that the work
   could not be done, and ask for the size before accepting it.

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
⚠ **And the gate's own "counted" figure is NOT that delta**: it leaves the rows born `🔵 DISCLOSED`
out, so a round can print a large negative number while the registry barely moves (✔MEASURED
2026-10-08: counted −23, real −2). Read the `change` line it prints — *closed, opened (created,
disclosed)* — and report the REAL net beside the counted one (the section above).

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
without filing its row — the orchestrator SPAWNS A NEW LANE to close the remnants**, naming them
explicitly: a new agent of the same orchestrator, briefed as lane `<o>/<a>`, which runs `/dss-cycle` AS A
LANE and never orchestrates (orchestration.md). "Next cycle will notice" is the follow-up culture this section ends.

⚠⚠ **AND THE BALANCE GATE IS BLIND TO HALF OF IT. `dssharness check-anchor-balance` COUNTS *ROWS*, so an
anchor cited in source with NO ROW IS INVISIBLE TO IT.** ✔MEASURED P40: it reported **"opened 0"**
while a finished lane had cited a minted anchor id across **six** files with
no row anywhere in `.plans/`. Only `.harness-config/runner/actions/check-anchor-registry/check-anchor-registry.py` catches
that class, and there it is the one telling the truth. [→ run it as `dssharness run check-anchor-registry`, which accepts an id found anywhere in `.plans/`; beside it, `dssharness check-anchor-citations --current-tree` resolves a cited id only to a row of the two registries, the only home a row can have](actions.md)
⇒ **Run BOTH before calling a cycle clean. A green balance is NOT evidence that nothing was
opened.**

✔MEASURED P40, the sideways-move class: a row was **re-verdicted** from
*"⏳ DEFERRED, trigger-gated"* to plain 🟠 OPEN by a lane that then exited — better described, still
open. **"Re-verdicted" is not closed**, exactly as *"refused but not fixed"* is not.
