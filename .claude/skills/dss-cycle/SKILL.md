---
name: dss-cycle
description: >
  Advance the DSS Code Prime compiler by exactly ONE development cycle — pick the next priority from
  the plan-00 §0.1 stepper, clear its blockers first, plan it, design-audit the plan before locking
  it, implement the best long-term solution, review, pass the fail-loud gate, pin deferrals, update
  the plans, self-audit before lock, then commit and push. Use this whenever the
  user asks to run a cycle, do the next cycle, continue the compiler work, advance the plan, work the
  stepper, or pick up the next priority — and whenever /loop drives continuous autonomous progress —
  even if they never say "skill". One cycle per invocation. It PAUSES and asks on any pending
  definition, architectural fork, gated anchor, or hard-stop boundary; it never guesses, never
  workarounds, and never breaks source (language) / target (processor) / linker (object-format)
  agnosticism. NOT for judging finished work (use dss-audit), reconciling plan staleness (use
  dss-plan-sweep), or running the multi-host matrix (use dss-cross-leg-test).
user-invocable: true
argument-hint: "[optional: specific priority or anchor to take this cycle]"
---

# DSS Code Prime — Development Cycle Loop

One cycle per invocation: pick → clear blockers → plan → design-audit → implement → review → gate →
pin → cross-plan → self-audit → commit → push.

## When to use

- Advancing the compiler by one real priority, autonomously or under `/loop`.
- Finishing a `… WIP` cycle already in flight — that *is* this cycle's priority.

**Not this skill:** judging finished work → `dss-audit`. Plan staleness → `dss-plan-sweep`.
Multi-host matrix → `dss-cross-leg-test`.

**Conventions authority:** the `dss-code-prime` skill wins on any conflict.

## ★★★ THE GOAL IS TO *WORK* — one working reference makes the behaviour REQUIRED

Operator ruling 2026-08-19: *"we must never crash on correct code, even if gcc fails, we must do it right. … If we have a reference that works, we must too (of course, with the implementation always following our project's best practices)."*

**The test is the DISJUNCTION, not the consensus.** If ANY reference (gcc, clang, MSVC, …) compiles and runs a correct construct, DSS must too. A reference's FAILURE is therefore never evidence against DSS — when DSS accepts what one reference rejects and another accepts, DSS is **right**, and the divergence is a **NON-DSS CONFOUND** to attribute and record. **Never make DSS fail in order to match a failing reference.** This bounds the bidirectional rule: "accepting what no reference accepts is a defect" turns on **NO** — not one. The implementation still owes the full bar (agnostic, config-driven, best-long-term, fail-loud, strictly tested): "it works" is the requirement, not the excuse. ⚠ Probe references **separately** — "the reference" is not one voice, and P14 nearly narrowed a working header chain because only gcc's failure was on file and MSVC's success was not. Full case: `references/the-bar.md` §A.3b.

### ★★★★ THE DISJUNCTION DECIDES **ACCEPTANCE**, NOT **MEANING** — operator ruling 2026-08-28

**The rule above settles whether a construct is ACCEPTED. It does NOT automatically settle what a
program MEANS when the references disagree about the meaning itself.** ✔The case that produced this,
measured with each reference probed separately: `#pragma once` — gcc dedups two DIFFERENT files with
byte-identical contents (**content-keyed**); clang 18.1.3 and MSVC 19.51 do not (**identity-keyed**).
The unanimous rows (`./h.h`, `sub/../h.h`, symlink, hard link — all DEDUP everywhere) make
`core::PathIdentity` REQUIRED; the split row decides the KEY.

A mechanical reading of the disjunction picks **gcc, the minority**. ⇒ **THE OPERATOR RULED
IDENTITY-KEYED**: DSS refuses a program gcc compiles (a vendored/copied header), deliberately.

**The two reasons, because they generalise:**
1. **Content-keying does not merely accept MORE — it SILENTLY OMITS TEXT** in a constructible case
   (two byte-identical headers whose meaning differs because a macro was redefined between the
   `#include`s). "Accept more" is not a virtue when the extra acceptance is bought by DROPPING code,
   and a silent omission is the class the bar most abhors.
2. **2 of 3, including the vendor that invented the pragma.** A minority-of-one winning on the
   disjunction deserves a second look — not a veto, a prompt to state the trade-off.

⇒ **Before invoking the disjunction, ask which question the references are splitting on.**
Accept-vs-refuse ⇒ the disjunction governs and the accepting reference wins. Disagreement about what
a valid program MEANS ⇒ **that is an architectural fork: PAUSE and ask.**
⇒ **Record the refusal cost in the row**, or a later cycle applying the disjunction by reflex will
"fix" it back. This ruling is exactly that shape.

### ★★★★ THE UNION IS OVER WHAT **WORKS**, NOT ONLY OVER WHAT IS ACCEPTED — operator ruling 2026-09-02

> *"our own readme says: `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C`. We are meant to be the best of our
> references. Everything that works in one of the vertices, must work here. That's it, so the
> tiebreaker is not clang, is whether one of the references make it work or not."*
> … *"Last resort is check against iso C, since our goal is to work if any references work."*
> … **"We always aim to work, to have quality."**

**THERE IS NO PRIVILEGED REFERENCE. THERE IS NO TIEBREAKER VERTEX.** ⚠ This section previously said
"clang breaks the tie" and that was WRONG — a general rule mistaken for a special case, corrected by
the operator the same day it was written. Clang was named in the ruling as *an example of who to ask
for another opinion*, not as an arbiter. **The union already decides it.**

**The rule, and it is the one the README has always stated:** the union is taken over references
that **WORK**, not merely over references that ACCEPT. Acceptance is the weaker reading — a
reference can accept a program and then emit code that faults, tears, or silently drops the meaning.
⇒ **If ANY vertex compiles a correct construct AND the result WORKS, DSS must work too**, whichever
vertex that turns out to be. A vertex that accepts-but-does-not-work casts no vote for its own
output; it is simply not a working reference for that construct.

| the split is about | what governs |
|---|---|
| **ACCEPT vs REFUSE** | the union — any reference that accepts a correct construct makes it REQUIRED |
| **QUALITY — one reference WORKS and another silently does not** | **the union again, read over WORKING**: match the one that works, whichever it is |
| **What a valid program MEANS**, both readings defensible and both working | an architectural fork — **PAUSE and ask** |

⚠ **A quality split is NOT a meaning fork, and must not be escalated as one.** A meaning fork is two
defensible readings of the same program (`#pragma once` content-keyed vs identity-keyed) where both
implementations deliver their own reading correctly. A QUALITY split has a right answer: the
references agree on what the program means and one of them fails to deliver it. **Measure which one
works and match it.**

✔**THE CASE THAT PRODUCED THIS, and it corrected a call I had already escalated.** A packed
`_Atomic int` at offset 1 on x86_64: clang emits the generic `__atomic_load`/`__atomic_store`
libcall, **gcc INLINES**, MSVC abstains (refusing `_Atomic` even aligned). I read that as 1–1 with
no tiebreaker and asked the operator. It never was a tie: gcc's `LOCK`-prefixed store IS atomic,
but its paired plain `movl` LOAD is not when the 4-byte access spans a cache line, so **gcc's route
silently loses atomicity** — strictly worse than the arm64 case, where the same shape SIGBUSes
loudly. gcc is therefore not a working reference here, clang is, and the union settles it with no
adjudication needed.

★★ **AND THE MEASUREMENT NEEDS A CONTROL, OR IT IS NOT ONE.** ✔MEASURED `clang 18.1.3 -O1 -S`:
`x86_64-pc-windows-msvc`, `x86_64-w64-windows-gnu` and `x86_64-pc-linux-gnu` **all three** emit the
libcall for the under-aligned member — and **the naturally-aligned control emits NO libcall and an
inline `xchgl` on all three.** Without that control, "clang emits a libcall" is equally consistent
with *"this target lowers all atomics through libcalls"*, which would say nothing about alignment.
⇒ **Probe the DEFECTIVE case and the HEALTHY case, on every target you intend to rule for.**

⚠ **A reference's output is not evidence of the property you care about until you NAME the
property.** *"DSS runs rc 42 on x86_64"* proved **no fault**; it did not prove **atomicity**. Two
different questions, and only one of them was the row's subject. ⇒ State which property you are
measuring before you read the result.

⇒ **Record in the row WHICH reference was the working one and on what measurement**, so a later
cycle applying the union by reflex over ACCEPTANCE cannot quietly revert it to the majority answer.

## ★★★ PRODUCTION ANCHORS ARE THE PRIORITY, ALWAYS — operator ruling 2026-08-25

> *"the priority is always production anchors. ALWAYS. harness we fix as we need when we face the
> problem (NEVER LATER)."*

The registry is **two files** — one WORKING list and one ARCHIVE:

| file | holds |
|---|---|
| `.plans/_deferred-anchor-registry-production.md` | every **still-open** row |
| `.plans/_deferred-anchor-registry-done.md` | every **CLOSED** row, in one table. **Nothing here is work.** |

★★★ **THE HARNESS REGISTRY RETIRED 2026-09-16, AND THE RULING ABOVE STILL GOVERNS.** The harness
moved to `DssHarness` (operator, 2026-09-15: *"we'll start using our new dotnet tool as harness ...
this being working will be that repo responsibility"*), and with it the third file: a defect in the
harness is repo-harness's to fix, and a defect in THIS repository's build wiring, tests or plans is a
production row like any other. Its 187 open rows and the archive's 544 closed ones are readable in git
at the parent of the commit that deleted them. **What did not change is the priority**: a defect a
user of the compiler could hit outranks one only we can hit, every time — the registry simply stopped
being where that distinction is recorded. See `references/dss-harness.md` for the tool, its verbs,
this repository's configuration, and which of this repository's own programs (its ACTIONS) are still the only way to do their job.

### ★★★ MOVE ON CLOSE — a closed row does not stay where it was

> *"the `_deferred-anchor-registry-{harness|production}.md` is a list of remaining items, that
> always delete a done item and put into `_deferred-anchor-registry-done.md` once finished."*
> — operator, 2026-09-01

- **Closing a row MOVES it.** It is deleted from its working registry and appended to the archive's
  matching table. Reopening moves it BACK. Neither is an edit in place.
- **You do not do this by hand.** The door — `dssharness set-anchor`, and `write-anchor` for a new
  row — performs the move as part of writing the row; `apply-registry-row` and `lane-fold` hand
  their rows to it. Hand-editing the tables is how the two halves drift.
- **`check-anchor-balance` refuses both directions** (ARM 6's sibling, the partition arm): a CLOSED
  row left in a working registry, or an OPEN row filed in the archive. The second is the dangerous
  one — every queue in this project reads the two working registries ONLY, so a live row filed in
  the archive can never be picked up.
- ⚠ **The audit trail is NOT deleted, it is RELOCATED.** "Never delete a closed row" still holds;
  the archive is where it goes.
- ★ **RESOLUTION reads both; ORIENTATION reads only the working one.** A `D-*` cited in `src/` must
  resolve wherever its row now lives, so every guard that RESOLVES a citation globs
  `_deferred-anchor-registry*.md`. Everything that asks *what is left* — `burndown-queue`, Step 1's
  priority pick, this skill — reads production and stops there.

⚠ A row's bucket follows the **DEFECT, never the instrument that found it**. `D-CONFIG-*` and
`D-DIAG-*` are PRODUCTION deliberately: in this architecture a `.lang/.target/.format.json` document
IS the compiler's behaviour, and a diagnostic IS its output to a user.

### ★★★ THE ROW SHAPE IS SIX CELLS, AND TWO OF THEM ARE DECLARATIONS

    | Anchor | Priority | Status | Trigger | Closing work | Cross-refs |

Operator, 2026-09-01: *"add columns for priority and status ... then the write explicitly writes it
correctly, this way we always have clean statuses."* `Priority` is `P0`..`P5`; `Status` is a
controlled vocabulary — `✅ CLOSED` / `🟠 OPEN` / `⏳ GATED` / `🔵 DISCLOSED`. `--status` or a
`--status-file` holds one of those cells or its bare word (`closed`, `open`, `gated`, `disclosed`);
`DssHarness write-anchor` refuses anything else, the retired `🔵 🟠 OPEN (DISCLOSED)` included.

- ★★★ **`DISCLOSED` is for debt this cycle FOUND, not debt it CREATED, and it exists to remove an
  incentive rather than to grant an excuse.** The balance gate forbids a cycle that OPENS new debt;
  it does not forbid one that DISCLOSES pre-existing debt. Without the word, the cheapest way to
  pass the gate is to not write the row at all — which is the precise dishonesty the gate exists to
  prevent, produced BY the gate. A disclosed row is **OPEN WORK**: it counts in every total, it
  files in the working registry, and `--done` refuses it. It is exempt from the net-increase
  FAILURE and from nothing else.
- ⚠ **The claim is checkable, so claiming it falsely is a lie about history, not a formatting
  choice.** It asserts the defect PRE-DATES this cycle, and a reviewer can look for it in the base
  ref. Use it for a defect you merely faced; never for one you introduced.

- ⚠ **The status cell keeps its glyph, and the glyph is the contract.** A row is CLOSED iff its
  status cell OPENS with ✅ after stripping `*_ ` — the complement defined, never the variants. A
  column holding the bare word `CLOSED` would make that test false for every closed row at once.
- ⚠ **`Priority` is a DECLARATION, not a sieve result.** `burndown-queue` seeds it on a new row and
  then READS it; its own docstring warns a band is *"a sort key, not a verdict"* because a census
  built from that keyword sieve reported 103 where the truth was 4. Correct the cell and the
  correction survives.
- ⚠ **`check-anchor-balance` ARM 6 refuses a row whose `Status` column contradicts the verdict
  leading its `Trigger` prose.** Two cells now state the same fact, so they can disagree — silently,
  because the gate would believe the column while every human reads the prose.
- ★ **Plan-side §3.1 tables were NOT migrated** and still use the four-cell shape. Both are
  recognized; only the registry documents changed.

### The door — `dssharness write-anchor` / `set-anchor`

A row is WRITTEN by DssHarness's `write-anchor` (a new row) or `set-anchor` (an existing one), and by
nothing else; `read-anchor` / `read-anchors` read it (`references/anchors-and-deferrals.md`).
`.harness-config/runner/actions/anchors/anchors.py` has no write verb: it is the reader
(`read` / `list [--lint]`, each taking `--production` / `--done` and only those two — the harness
registry retired on 2026-09-16) and the one launcher that `lane-fold` and `apply-registry-row` call
the door through. That launcher refuses, before the door, a write the gate would fail (a `Status`
contradicting the verdict leading its `Trigger`) or the door would rewrite (in-line whitespace in a
cell it writes), and an update names only the fields that change.

```
DssHarness write-anchor  D-<AREA>-<NAME> --priority P1 --status open \
                                 --trigger '...' --closing '...' --cross-refs '...'   # writes; --anchor-dry-run previews
DssHarness set-anchor    D-<AREA>-<NAME> --status closed --closing '...'           # MOVES it
DssHarness read-anchor   D-<AREA>-<NAME>                    # the full row, field by field
DssHarness read-anchors  --pending --band P0       # name + priority + status only
DssHarness read-anchors  --lint                       # every row a reader cannot key on
```

⚠ **Never hand-assemble a row.** The door takes the FIELDS, so a wrapped anchor id (invisible to
every grep, and it MINTS a false id), an unescaped `|` (silently adds a column) and a wrong cell
count are all inexpressible. `set-anchor` is the ordinary way to close a row: it patches only the
fields you name, preserves the rest byte-for-byte, and performs the move.

⚠⚠ **AND NEVER HAND-*READ* ONE EITHER — `read-anchor <ID> --json` IS THE ONLY WAY TO GET A CELL'S
VALUE.** A lane that preserves a row's existing evidence must first read it, and reading it with
`grep`/`sed`/`awk` off the raw table line returns the STORED form, not the value: storage escapes
every `|` as `\|`, and handing that back escapes it again. ✔MEASURED 2026-09-02 (P54): **35 pipes
across 14 rows** had already been stored doubled and rendered a stray backslash where their author
wrote a bar — a C `||`, a shell `||`, `awk -F"|"`, a regex alternation. Two callers produce it and
neither was detectable downstream: an author PRE-ESCAPING by hand, and the raw-line read, which
**compounds — one more backslash on every re-close**.
- `make_cell` now REFUSES a pre-escaped pipe (self-test arms 6b–6d), so a raw-line read no longer
  corrupts quietly, it FAILS on you. To display a backslash before a bar deliberately, spell the
  bar `[|]`.
- ★ **THE CONTROL IS THE LESSON.** The writer's pre-existing round-trip pin stayed GREEN through
  all of it, because it reads its cell through the un-escaping path — it could not see the class it
  was there to protect. 35 pipes rotted under a passing self-test. **When a pin and the defect it
  guards share a helper, the pin is testing the helper, not the property.**
- ⓘ It surfaced only because a lane's preserved cell was compared against the registry's CURRENT
  text instead of trusting the lane's own *"preserved byte-for-byte"* claim. **A lane's report is a
  claim about the text it READ** — and if the orchestrator edited the row after briefing the lane,
  a faithful lane silently reverts that edit. Verify the prefix, every time.

**How the ruling binds this loop, clause by clause:**

1. **Step 1 picks from PRODUCTION.** A harness row is never picked *because it is next*. If §0.1 is
   dry, promote an eligible **production** anchor. `.harness-config/runner/actions/burndown-queue/burndown-queue.py`
   already bands production errors highest — the ruling makes the FILE the outer sort key, above
   any band.
2. **"NEVER LATER" is the load-bearing half.** A harness defect is fixed **at the moment it is
   faced** — this cycle, in the lane that hit it, as part of that lane's work. A gate that lies, a
   guard blind to its subject, a script that blocks the work in front of you: fix it NOW. **Filing
   it and routing around it is exactly the failure this ruling names.**
3. **A harness row is a RECORD, not a backlog entry.** Harness work is drained by encounter, not by
   scheduling, and such a row is normally written already ✅ CLOSED, naming a fix that landed in the
   same cycle. ⚠ **It no longer has a registry of its own** — since 2026-09-16 a defect in the
   TOOL is repo-harness's to fix and is reported there, and a defect in THIS repository's build
   wiring, tests or plans is an ordinary production row.
4. **Still file the row.** *Anchor every issue found* is not repealed — a harness defect fixed
   silently teaches nobody, and the row is what makes the fix auditable. This ruling governs what
   gets **SCHEDULED**, not what gets **RECORDED**.
5. **Step 2's "clear blockers FIRST" is unchanged, and is now the ONLY route a harness row takes
   into a cycle** — it is worked because it BLOCKS the production priority, never on its own ticket.
6. **The cycle report states production movement SEPARATELY**, because a single total cannot answer
   the question the operator is actually asking. ★ A cycle whose closures are all harness rows has
   hardened the workshop and shipped nothing; say so plainly rather than letting a healthy total
   imply otherwise.

⚠ **Do not quote a per-bucket count from here or from the handoff — re-derive it.** ✔MEASURED
2026-08-25, and the correction is the reason this warning is here: the P34 handoff's own
"475 production OPEN" was wrong by 20, caught only by cross-checking the per-bucket split against
`check-anchor-balance`'s registry total. The instrument, which reuses that gate's own row scanner
rather than re-typing the "is this row open" vocabulary:

```
python .harness-config/runner/actions/check-anchor-balance/check-anchor-balance.py --breakdown --denominator registry
```

⚠ That gate canonicalises BOTH registry files to ONE key — deliberately, so that MOVING a row
between buckets is correctly a no-op — so its breakdown gives the registry TOTAL, and a per-bucket
split must sum to it. ★ **That sum is the cross-check that catches a mis-bucketed or double-counted
row, and it is the only reason the P34 error surfaced at all.**

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

`.harness-config/runner/actions/check-anchor-balance/check-anchor-balance.py` **already** implements
*"a cycle may not end with more OPEN deferral rows than it began"*, comparing **by row name**
across every sanctioned home. Run it against the cycle's OWN start commit and treat a rise as a
**HARD STOP**, not a note:

```
python .harness-config/runner/actions/check-anchor-balance/check-anchor-balance.py --base <cycle-start-sha> --breakdown
```

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
main tree and its rows applied. See *A COMPLETED SET OF LANES IS A COMMIT POINT* below for the
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
that class, and there it is the one telling the truth.
⇒ **Run BOTH before calling a cycle clean. A green balance is NOT evidence that nothing was
opened.**

✔MEASURED P40, the sideways-move class: a row was **re-verdicted** from
*"⏳ DEFERRED, trigger-gated"* to plain 🟠 OPEN by a lane that then exited — better described, still
open. **"Re-verdicted" is not closed**, exactly as *"refused but not fixed"* is not.

## ★★★ A GATE HOST HOLDS THE REPO AND NOTHING ELSE — operator ruling 2026-08-25

*"keep macos and vps linux arm64 updated with our repo files, and free of stale files/worktrees.
you own the cleanup."*

**The remote checkout is not a place work accumulates. It is a MIRROR of the tree under test,
and the cycle owns keeping it one.** A leg that runs against a host holding anything else is
not testing the tree it reports on.

⚠ **✔MEASURED 2026-08-25 (cycle P34), and it produced a RED that looked like a code defect:**
the macOS host held **16,312 files against a local 6,660**. `--push` is a `tar` extract and
**tar extraction never deletes**, so that tree was the UNION of every tree ever pushed —
including `.plans/_deferred-anchor-registry.md`, deleted locally in the same cycle and still
sitting there at 6.7 MB. `plan_citations_guard` counted **4908 citations across 213 documents**
where the live tree has **2853 across 212**, and reddened. The identical guard was `rc=0`
locally. ★ **The failure named the guard, and the guard was innocent** — hours can go into a
subject that was never wrong, because a stale remote file is invisible from the driver.

⚠ **AND 9,638 OF THOSE FILES WERE `.claude/worktrees/**` — A FULL COPY OF THE REPO PER LIVE
AGENT, shipped to the gate host on every push.** That is not merely transport cost: the
examples runner **globs `examples/<lang>/*`**, and a worktree carries its own `examples/`
tree, so a gate host holding one can run a corpus belonging to somebody's uncommitted lane
and report the result as the cycle's.

**The three rules:**
- **A push is a SYNC, never an accumulation.** `dssharness sync` deletes what the source no
  longer has and verifies the copy afterwards — one transport, every host. A transport that only
  adds is a transport that silently diverges.
- **Worktrees are excluded at the transport, on BOTH carriages.** An agent worktree never
  belongs on a gate host. ⓘ rsync does NOT delete excluded paths, so adding the exclude does
  not clean a host that already holds one — that needs an explicit removal, once.
- **The cleanup is the CYCLE's job, not a thing to notice later.** Before a leg is trusted,
  the host holds the repo and nothing else.

★ **The general form, which is the part worth carrying: ask what the remote tree IS, not what
you last sent it.** Every reasoning error here came from thinking about the PUSH — "I sent the
right files" — when the question is what the far side now CONTAINS. The same distinction that
makes `git status` worth reading after a merge you are sure about.

⚠ **This narrows, and does not repeal, the standing order against cleaning those hosts.** No
`git clean`, no `reset --hard`, no `checkout --` on either machine unless the operator names it
(a deliberate reset stays opt-in for exactly that reason). What is authorised is removing
what the repo does not have: stale files and worktrees.
⇒ ★★★ **THE OPERATOR NAMED IT ON 2026-08-26. Read the next section — restoring a leg clone is
now REQUIRED where this paragraph once forbade it, and the transport is the only thing that may
do it — `dssharness sync` today.**

## ★★★ EVERY LEG HOST KEEPS A CLONE, AND THE LEG CLEANS UP AFTER ITSELF — operator ruling 2026-08-26

> *"we should use already cloned repo in each leg [...] you can keep using the sync process you
> already use, but CLEAN UP the changes after you finish them (you can also use worktree in leg
> host if needed for parallel legs, clean up also needed). [...] don't forget to check each leg
> branch before working in it, and also clean up the changes after finished. that's the standard"*

| host | leg repository |
|---|---|
| WSL | `~/src/dss-code-prime` |
| arm64 VPS | `~/src/Github/dss-code-prime` |
| macOS | `~/src/dss-code-prime` |

**The shape, and all four steps are the standard:**
1. **PREPARE** — `git fetch`, put the host's clone on the DRIVER's branch at the DRIVER's commit,
   `git clean -fd`. *This is the "check each leg branch before working in it" clause, and it is
   done by MOVING the host rather than by asserting about it.*
2. **SYNC** — the existing rsync/tar carriage, `.git` withheld on **all three** now. The host's
   own history is the authority; the sync supplies only the working tree.
3. **RUN** — build + ctest. Repo guards on the **root host only**.
4. **RESTORE** — `git reset --hard`, `git clean -fd`, `git worktree prune`. On **every** exit
   path, `die` included.

★ **THE CARRIAGE IS `dssharness sync`.** The repository's own shell owner of these steps,
`leg-tree` (its prepare and restore verbs inlined into each remote leg's payload), was retired on
2026-09-21, when no leg called it any more; its tree half lives on in
`.harness-config/runner/actions/owning-tree/owning-tree.py`. **Never hand-roll these git commands in
a leg** — that is the four-hand-written-exclude-lists mistake with a destructive verb attached.

⚠ **`-fd`, NEVER `-fdx`.** Ignored paths (`build/`, the ccache) are the leg's own working state;
deleting them buys tidiness and costs every leg a cold rebuild.

⚠ **CLEANUP BELONGS TO THE MODE THAT MADE THE MESS, not to every mode that runs afterwards.**
✔Caught while wiring this: a sync-only mode exists to LEAVE a host staged for a manual probe, and
a test-only mode runs over whatever is already there. A restore on exit would have deleted the
staging as the command returned, and a prepare would have made test-only test HEAD while reporting
on the staged tree. ⇒ the three shapes must stay distinct. Today they are `dssharness sync`,
`dssharness test --use-staged` and `dssharness test --no-build`.

⚠ **WHY THIS REPLACED THE OLD ARRANGEMENT.** ✔MEASURED 2026-08-26: the macOS clone sat on branch
`…-3` at `8cb9afbd` (**three commits back**) with a **2,696-path index under a 2,759-path working
tree**, while two carriages withheld `.git` and one shipped it. ★ **The cost is not the disk — it
is that guards ask git questions.** `check-line-endings` reads `git ls-files --eol`;
`check-shell-portability` (retired since, with the shell programs) had ALREADY been rewritten in 2026-08-22 to stop asking `git ls-files`
because this very host answered about a commit that deleted `tools/*.sh` in P17 and produced
**seven violations against files that do not exist**. A host whose git disagrees with its files
makes every git-reading guard a coin flip, and the flip is invisible from the driver.

★★ **`git worktree prune` IS PART OF RESTORE, and it is the clause a human cleanup cannot cover.**
✔MEASURED 2026-08-26: both remote hosts carried a registered, `prunable` `dss-probe-6f4aab73`
worktree from a cycle that never cleaned up — and it **survived the operator's own manual pass**,
because a stale worktree registration lives in `.git/worktrees/` and **never appears in
`git status`**. A parallel lane may take a worktree on a leg host; the lane that takes it owns
removing it.

⚠ **AND THE TILDE DOES NOT EXPAND.** ✔MEASURED against the live VPS on the first run: every leg
names its repo `~/src/…`, and `cd "$var"` does **not** expand a tilde held in a variable — `~` is
expanded only where it appears unquoted in the source text. The retired `leg-tree` normalised the
path once, where both of its verbs shared it. ★ The dangerous half was not the failed `cd`: `restore` returned 0 when
the directory is missing, so an unexpanded path would have left every host dirty forever while
every leg reported success.

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
   either — spawn its remnant lane first (see the no-follow-ups ruling above).
2. Apply every lane's ROWS, then re-derive the balance with
   `check-anchor-balance --base <cycle-start-sha>` and run `check-anchor-registry.py`. **Both**, for
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
⇒ ★ **AND WHAT IT CONSISTS OF IS NOW EIGHT RUNS, NOT FOUR** — see the next section.

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
     now checkable rather than advisory.
     ⚠ **The count is not typed here, and the migration is why that matters more than ever:** it
     moves by whole waves. `ctest -N -L repo-guard` prints it, and so does the configure line
     `repo-guard label applied to N test(s)` — which is what the root `CMakeLists.txt` says to read,
     in those words, having already gone stale by eight entries once. This sentence has said **18**,
     then **31**, then **40**; **re-derive it at the commit in front of you.**

★★★ **THE SHAPE IS THIS CYCLE'S OWN THROUGH-LINE, TWICE MORE: THE RULE A DEFECT CITED WAS TRUE — OF
THE THING NEXT DOOR.** *"`#deps 0` is never legitimate"* is true of **gcc** and false of **MSVC**.
*"`comm` names the image"* is true on **Linux** and false on **macOS**, where `ps -eo comm=` yields a
truncated ABSOLUTE PATH (✔650/662 rows contain `/`; Linux control 0/39). ⇒ Before trusting a rule a
guard cites, ask **which toolchain, and which platform, it was measured on.**

## ★★★ A LANE WORKTREE LIVES INSIDE THE REPO, AT `.worktrees/<short-name>` — operator ruling 2026-08-26

> *"I want the worktrees implementation to be inside the project root, .worktrees directory (where
> 100% of it's internal content ignored by .gitignore). This way we stop contaminating builds
> outside repository bounds."* … *"worktrees MUST be ignored by ALL host copies to run legs"*

**This SUPERSEDES the short-absolute-root convention** (`C:/dssp40k`, `C:/dss-<cycle><lane>-rod`).
A worktree outside the repository is a full checkout — plus its `build/` — that nothing owns and no
guard can see. ✔MEASURED 2026-08-26: the tree was already carrying **9,661 files / 410 MB** of
orphaned checkouts under `.claude/worktrees/`, three full copies, one of them 287 MB, and
**`git worktree list` knew about none of them**.

**★ ONE OWNER: `.harness-config/runner/actions/lane-worktree/lane-worktree.py`** (`add` / `remove` / `list`), one
Python program on every host: **never hand-roll `git worktree add` in a lane.** Every directory it
and `lane-fold.py` use is read from configuration (`references/worktrees.md`).

```bash
python3 .harness-config/runner/actions/lane-worktree/lane-worktree.py add k      # -> <repo>/.worktrees/k
python3 .harness-config/runner/actions/lane-worktree/lane-worktree.py remove k   # removes AND prunes
```

Four clauses, and each is measured rather than asserted — detail in `references/worktrees.md` §H.0b:

1. **`.gitignore`'s `/.worktrees/` rule is what satisfies the "ALL host copies" clause.** The
   transport derives what it carries from git, so that one line is what stops four full repo copies
   riding to macOS and the arm64 VPS on every push. ★ It is ALSO declared independently of git:
   ✔MEASURED 2026-09-23, `.harness-config/config.json`'s `sync.neverTransfer` names `.worktrees`,
   `.claude/worktrees`, `.temp`, `build`, `scratchpad` and `test-scratch`, and `.secrets` at any
   depth (`**/.secrets`, which covers the root one too) — once, for every host. `dssharness sync --dry-run` lists every path it would write, which is how to CHECK
   this rather than trust it.
2. ⚠ **The MAX_PATH budget is now SPENT, not slack.** The move costs **46 characters** on every
   build path: longest build-relative suffix **163**, so `C:/dssp40k` had **87 spare** and
   `.worktrees/k` has **46**. `lane-worktree.py` refuses by arithmetic (exit 3) a name whose longest
   build path would not stay under `worktrees.pathLimit`, every term read from
   `.harness-config/config.json` and named in the refusal — the defect it prevents fails as a per-TU
   compile error in files the lane never touched.
   ⇒ **Keep lane names SHORT** (`k`, `l`, `rod`); a descriptive name spends that margin.
3. **The lane that takes a worktree owns removing it**, via `remove` — which prunes, because a stale
   registration lives in `.git/worktrees/` and **never appears in `git status`**.
4. ⚠ **`-fd`, never `-fdx`.** `git clean -fd` does not delete ignored paths, so a live worktree
   survives a leg restore; `-fdx` would destroy it mid-build.

✔**The move was measured, not hoped:** a real 2,792-file worktree at `.worktrees/probe` moved
**zero** of the 16 runnable registered guards — identical verdicts and output volume — and
`git status` reported 0 lines for it.

## The pause-and-ask gate — the most important behavioral rule

The loop is autonomous for **execution** and escalates **decisions**. When any of these appears,
**PAUSE and ask the user — do not assume a default, do not guess, do not pivot to other work:**

- **A pending definition or ambiguity** — any requirement, behaviour, naming, scope, or schema shape
  that is underspecified.
- **An architectural fork** — a fork is *real* only if you can state ≥2 concrete, defensible
  long-term designs. If you cannot articulate a genuine second option there is no fork, and the hard
  part lands this cycle. **Never invent a fork to escape the work.**
- **A gated anchor** whose trigger has not fired, or a correctness-critical anchor whose negative
  miscompile-pin cannot be constructed.
- **A hard-stop boundary** (see below).

**How to present a decision, always this shape:** (1) the problem and why it blocks, in one or two
sentences; (2) 2–4 candidate **long-term** solutions, each no-workaround and agnostic — if one breaks
agnosticism say so and why it is still listed, usually to be rejected; (3) each one's trade-off —
what it costs, buys, forecloses; (4) a **recommendation** with reasoning; (5) the ask. If a fact is
missing, ask for the fact — never invent it. When the user answers, capture the decision and its
rationale in the owning plan this cycle so it is not re-litigated next invocation.

The loop resumes only after the user answers. **While paused, do not start a different cycle.**

### ★★★ A §B TRIGGER IS A PREDICATE, NOT A RITUAL (operator ruling 2026-08-17)

A row pinned "§B — operator decision" is gated on a **stated reason**. If a lane MEASURES that
reason to be FALSE, the §B **was never triggered** — discharging it is not closing a §B on the
lane's own authority, it is discovering the gate does not apply.

**The rule, and all three clauses are load-bearing:**
- every §B row states its trigger as a **testable predicate**, not as a mood;
- a lane MAY discharge a §B by **measuring the predicate false**, provided it records the
  measurement **in the row** and **flags it in the cycle report** so the operator can veto;
- an **INCONCLUSIVE** measurement escalates. **Silence is never a discharge.**

✔The case that produced this ruling: bare `asm` was pinned §B because the spelling
"needs a new standard-mode axis". A lane measured that DSS already declares GNU mode in the
reference compilers' own machine-readable spelling (defines `__GNUC__`/`__clang__`, does **not**
define `__STRICT_ANSI__`) — so no axis exists or was added, and the predicate was false. It also
measured that DSS was **accepting `int asm = 42;`**, which no reference compiler accepts in GNU
mode: the pinned state was shipping an *invented extension*, not merely withholding a feature.
Operator ruling: **keep it.** *"Reverting sound, measured, conformance-correcting work to
re-present it as a brief would destroy value to satisfy a ritual."*

⚠ **Without this rule the next lane's only options are an unauthorized close or a wrong revert** —
which is why the procedural hole, not the keyword row, was the real deliverable of that cycle.
⚠ Discharging a predicate does not discharge what the predicate did not cover. In that same case
the references' acceptance genuinely IS conditional (under `__STRICT_ANSI__` they require
`__asm__`), so a **trigger-gated** row was opened for the day a strict-conformance mode ships —
the §B's original concern preserved and made testable, without holding the fix hostage to it.

### ★★★★ A TRIGGER WAITING ON A COMPONENT *WE* BUILD IS NOT A GATE (operator ruling 2026-08-26)

> *"if C has any intrinsic, good time to build the intrinsic then the OPT"* … *"if [it] needs
> something also to be built that can be built, build the one that can be built, then the
> [gated row]"*

Said while overruling the two rows a cycle had defended as *"legitimately gated"*.

**The distinction, and it silently re-verdicts a large slice of the registry's ⏳ rows —
classify every gated row by WHO OWNS THE TRIGGER:**
- **Outside us** → genuinely gated: an operator decision, a profile from a real workload, an
  upstream project, a hardware platform we do not have.
- **A component of THIS compiler** → **NOT gated.** It is a two-part task that was written down
  as one part. **Build the prerequisite, then close the row — in the same cycle.**

⚠ **§E#5 (don't build a consumer-less mechanism) DOES NOT APPLY when the missing consumer is
itself something we are supposed to build.** That is the exact misreading this ruling corrects.
The test is still *"does a consumer exist?"* — what changed is that **a consumer we are committed
to building counts**, so the honest response is to build it now, not to record that it is absent.
⇒ And filing the prerequisite as its own new row is the follow-up habit wearing a different hat.

★ **Prefer a prerequisite that unblocks MORE THAN ONE row, and name which when you pick it.**
✔The case that produced the ruling: an escape-analysis / points-to substrate for `mirMayAlias` is
the stated trigger for the MemorySSA walk-past-precision row **and** the stated prerequisite for
clause (c) of the **OPT7 / inlining** legality gate. One substrate, two rows.

⚠ **CHECK THE TREE BEFORE CALLING THE PREREQUISITE MISSING.** ✔MEASURED 2026-08-26: DSS's HIR
intrinsic registry was about to be described as absent. It EXISTS — `HirIntrinsicRegistry`,
`Hir::intrinsicRegistry()`, `makeIntrinsicCall`, and a shipped `umulh`
builtin-intrinsic node. Only the *routing* of a shipped C construct through it is missing, which
is a far smaller job than the row implied.

⚠ **AND THE MIRROR-IMAGE ERROR, corrected by the operator the SAME DAY: scheduling lives in the
PLANS, not in the tree.** A cycle concluded from `src/dss-config/targets/` holding only
little-endian `x86_64` and `arm64` that the big-endian trigger *"had not fired"* — while it is
operator-sequenced as plan 23 **FC19** with a toolchain verified by execution. ★ **The absence of
a thing in the tree is not evidence that it is unscheduled.** Read a trigger predicate against
the plans AND the tree, never the tree alone.

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
   baseline (`cmake --build build`, then full `ctest`). **A red baseline with no WIP-repair context
   is itself a pause gate** — present it; do not silently "fix it".

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
     hard stop applies to repairing it (see *Hard stops* below). It goes in front of §0.1.
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
   harness defect you FACE is fixed in this cycle, never filed for later. See the ruling above.
2. **Clear blockers FIRST** — from the §0.1 "Blocked by" column *and* the registry *and* any
   "requires deferrals" note. Highest-priority blocker first, before the priority itself.
3. **Plan it** — delegate to `/feature-dev:feature-dev` or a `Plan` / `code-architect` agent.
4. **Design-audit the plan before lock** — an **independent** subagent applies the `dss-audit` bar to
   the *plan*. An agnosticism break, a tight slice, a speculative build, or a weak-test plan caught
   here is far cheaper than after the diff lands. Scale the rigor: trivial mechanical cycle → quick
   self-check; new engine mechanism → full independent review **and** a §B pause to the user.
5. **Implement** — delegate in parallel by **disjoint file sets** (engine `.cpp/.hpp` vs
   `src/dss-config/**.json` vs `examples/` vs `tests/`), one agent per set, launched in one message.
   Name each agent's owned and forbidden paths. ★★ **At most FOUR reasoning agents live at once**
   (operator instruction 2026-08-19) — more work than that runs in waves of four. Script execution
   (builds, `ctest`, the guards, a remote leg) does **not** count against the cap; see
   `references/delegation.md`. Build the best long-term agnostic solution: extend
   config vocabulary, never branch the engine on identity. Any new `D-*` cited in `src/` is
   registered in the same commit.
   ⚠⚠ **CONTENTION IS PER *FILE*, NOT PER DIRECTORY — AND A DIRECTORY-SHAPED
   FORBIDDEN LIST COSTS A LANE ITS WHOLE RUN.** ✔MEASURED 2026-08-28 (P44): lane `h` was
   forbidden `src/core/**`, `src/ffi/**` and `src/program/**` because a sibling owned *some*
   files beneath them. It needed three specific files, **none of which the sibling touched**,
   and it stopped rather than edit a forbidden path — correctly, by the rule as written
   — after a ~27-minute run that produced zero edits. Justifying the grant then took one
   `grep`. ⇒ **State each lane's owned and forbidden sets as PATHS, and when a lane asks
   for a file inside a forbidden directory, MEASURE whether any sibling touches that FILE
   before refusing.** A directory-shaped forbidden list is a GUESS about contention, not a
   measurement of it — and the guess fails in the expensive direction, because a lane that
   obeys it looks compliant while doing nothing. ★ The repair is cheap and does not restart
   the work: grant the file, say why it is safe, and **resume the SAME agent** rather than
   spawning a fresh one that has to re-derive everything.

   ★★ **THE ORCHESTRATOR IS A LANE TOO — ITS OWN EDITS OBEY THE SAME OWNERSHIP.**
   `src/dss-config/**` is a FILE SET like any other, and a config document is an INPUT to
   every lane's build. Editing one while a lane is running does not merely risk a merge
   conflict — it changes what that lane's binaries MEAN between two runs.
   ⚠ ✔MEASURED 2026-08-20 (cycle P22): the orchestrator
   corrected a relocation `nativeId` while a lane was mid
   red-on-disable run. A test's verdict flipped between two runs of the same binary, and the
   lane reported a stale tree as a defect in its final report. **The damage is not the wasted
   report — it is that a red-on-disable observation is the ONE measurement this project
   treats as proof, and a config edit underneath one silently corrupts it.**
   ⇒ Announce the orchestrator's own owned paths alongside the lanes'; hold a config edit
   until the lanes that read it have reported, or hand it to a lane that owns it. Re-measure
   anything a lane reported across such an edit before acting on it — and when a lane's
   report and the tree disagree, suspect the SEQUENCING before suspecting the lane.
   ★★★★ **HANDING `src/dss-config/**` TO A LANE DOES NOT FIX THIS — IT ONLY MOVES WHOSE
   HAND IS ON IT, AND ✔THE HAZARD RECURRED THAT WAY ON 2026-08-26 (cycle P38).** The P22
   row above is CLOSED and the
   mechanism it built is sound; what recurred was the SCHEDULING. A lane was given
   `src/dss-config/targets/**` + `src/core/types/target_schema.*` and run CONCURRENTLY
   with three lanes that gate — so `test_support/test_config_snapshot` reddened in one
   lane and `core/test_target_schema` was momentarily UNCOMPILABLE in another, neither
   caused by the lane reporting it. ★ **`test_config_snapshot` WAS RIGHT AND MUST NOT BE
   "FIXED": it deliberately compares the run's snapshot against the LIVE tree, which is
   the only clause proving the copy is still taken at ctest RUN time.** Softening it to
   stop the flap would delete the mechanism's honesty check to hide an orchestration
   error — the *guard weakened every time it fires* failure, exactly.
   ⇒ **THE RULE: at most ONE lane may hold `src/dss-config/**` or `src/core/types/*schema*`
   at a time, and NO OTHER LANE'S ctest VERDICT IS TRUSTWORTHY WHILE IT DOES.** Either
   sequence that lane alone, or treat the concurrent lanes' gates as PROVISIONAL and
   re-gate the integrated tree once it is quiescent. ★ A lane reporting "N-1/N, the one
   red is another lane's config edit" has diagnosed it correctly — that report is a
   SEQUENCING finding, and the integration gate, not the lane, is what settles it.
   ★★ **AND THE TREE THAT RULE NAMES IS TOO NARROW: `.plans/**` IS AN INPUT TO A
   GUARD, AND A GUARD IS A CTEST ENTRY, SO EVERY LANE'S GATE READS IT.**
   ⚠ ✔MEASURED 2026-08-24 (cycle P31):
   a lane's `plan_citations_guard` was RED in one gate and GREEN in the next **with no edit
   of its own in between**, because the orchestrator applied registry rows and re-baselined
   the citation ratchet while that gate was in flight. `anchor_registry_guard`,
   `plan_citations_guard`, `check-anchor-balance`, `check-stale-refusal-citations` and
   `check-retyped-closed-sets` all take `.plans/**` as their SUBJECT ⇒ a row written
   mid-gate moves a lane's verdict exactly as a config edit moves a lane's binary.
   ★ **THE DIRECTION THAT COSTS SOMETHING IS THE FLATTERING ONE.** That guard went
   red→GREEN, so the lane could have concluded its earlier red was a flake and stopped
   looking. It measured instead and named the mechanism, which is the only reason this is
   written down rather than sitting in a wrong number.
   ⇒ the orchestrator announces `src/dss-config/**` **and `.plans/**`** among its owned
   paths, and holds a row application or a ratchet re-baseline until the lanes whose gates
   read them have reported — the same hold it already owes a config edit.
   ★★ **THE GENERAL FORM, WHICH IS THE PART WORTH CARRYING: ASK WHAT A FILE IS AN
   INPUT TO, NOT WHICH DIRECTORY IT LIVES IN.** Both instances of this defect came from
   reasoning about the directory — the first framed the hazard as *a config document is
   an input to the compiler* and so stopped at `src/dss-config/**`. Any tree a GUARD takes
   as its subject is a shared input, whatever it is called.
   ★★ **A LANE THAT BUILDS GETS ITS OWN BUILD TREE.** File ownership is not enough, because
   two lanes with disjoint FILE sets still collide in a shared `build/`: one relinks the DLL
   while the other is mid-`ctest`. ⚠ ✔MEASURED 2026-08-20 (cycle P22): `0xc0000043`
   (STATUS_SHARING_VIOLATION) mid-suite, plus a set of failures that appeared and vanished
   between two runs of the same binary. **A gate result taken from a shared build tree is not
   attributable to anything** — which makes it worthless exactly when it matters, during a
   red-on-disable observation.
   ⇒ Name the lane's build tree in its brief (`build/<lane>`), and clear it once green (the
   one-root rule). `dssharness build` gives each leg its own variant-keyed directory INSIDE the
   tree it is run in, so a lane worktree isolates itself — ✔MEASURED from one:
   `-S <lane>/. -B <lane>/build/x86_64-mingw-gcc-debug`.
   ★★ **AND A LANE THAT WRITES SCRATCH FILES GETS ITS OWN SCRATCH DIRECTORY.** The per-lane
   BUILD tree isolates artifacts; it isolates neither the scratchpad nor the working tree.
   ⚠ ✔MEASURED 2026-08-20 (cycle P23):
   four lanes were given one `scratchpad/<cycle>/` directory, one lane's
   mutation harness was OVERWRITTEN by another lane's file of the same name mid-run, and the
   next three red-on-disable cycles executed the WRONG SCRIPT with the first lane's arguments.
   Nothing was corrupted only because that harness restored its subject from a `finally` and
   verified the hash. ⇒ Name `scratchpad/<cycle>/<lane>/` in the brief.
   ★★★ **AND THE BRIEF MUST REQUIRE THE LANE TO *WRITE ITS ROW TO A FILE* THERE, NOT MERELY TO
   EMIT IT.** ✔MEASURED TWICE in cycle P44: a finished lane's task transcript came back **0 BYTES**,
   so its row — correct, complete, already written — reached the orchestrator not at all. The
   second time it was a lane that had emitted the row as a TOOL OUTPUT rather than as prose, which
   a text-only harvester silently drops. **Both failures look identical from the orchestrator's
   side: a harvest that simply reports one fewer row, indistinguishable from a lane that produced
   none.** ⇒ Every brief names an exact path (`…/<lane>/row.md`) and says: write the row there
   VERBATIM as one physical line, and reply with the path and a byte count. ⚠ The recovery is
   always to ASK THE LANE TO WRITE IT — **never to retype the row from a report**, because a
   retyped row can WRAP an anchor id, and a wrapped id does not fail: it goes invisible to every
   grep and MINTS a false one.
   ★★ **AN ANCHOR ID IS NEVER LINE-WRAPPED, AND THIS CLAUSE IS THE PROOF OF WHY.** The row
   above was cited here for hours WITHOUT EXISTING, and the step-10 audit was the first thing
   to notice — because the id was split across two lines, so neither the registry guard nor a
   human's grep could match it. ✔MEASURED 2026-08-20: **17 of the 78** distinct `D-*` ids cited
   on that cycle's added lines were wrapped; 16 were harmless only because the same id appears
   unwrapped nearby. ★ **A wrapped id does not fail — it becomes INVISIBLE**, which is the one
   failure mode a fail-loud project cannot detect by watching for a failure. Break the line
   BEFORE the id or AFTER it, never inside it — the convention the harness scripts already
   spell as `ANCHOR, ONE LINE, DO NOT WRAP`.
   ⚠ **The same measurement carries a second, larger consequence: a WHOLE-TREE gate number
   taken by any lane is not attributable to that lane**, because the source tree still holds
   every other lane's uncommitted edits. A lane scopes its gate with `-R` to its own subjects
   and treats a failure outside them as somebody else's until proven otherwise; the ONLY
   attributable whole-tree number is the orchestrator's, after the fold.
   ★★ **A BRIEF MAY STATE AN INTERFACE ONLY IF ITS AUTHOR HAS RUN IT** — the same standard as
   §5's "a measurement is stated only with the instrument that produced it", one level up: an
   invocation is a claim about the world, and writing one from memory is writing an
   unmeasured fact into the place a lane trusts most. ⚠ ✔MEASURED 2026-08-20 (cycle P23): the
   orchestrator's own common brief spelled the witness gate as `-- ctest …`; its real interface was
   `<log-path> <success-regex> <command> [args...]`. TWO lanes hit it, it refused
   (fail-closed, correctly), and one left a file literally named `--` in the repo root. The
   fix is one command: run the invocation once before pasting it into a brief.
   ★★ **AND THE SAME STANDARD BINDS A MECHANISM, NOT ONLY AN INTERFACE: A BRIEF THAT NAMES THE
   FIELD A DECISION READS, OR THE ROLE A VALUE CARRIES, IS MAKING A MEASUREMENT AND OWES AN
   INSTRUMENT.** ⚠ ✔MEASURED 2026-08-20 (cycle P23): a brief told a lane
   to route the COFF weak-external decision on the auxiliary record's `Characteristics` field. gcc
   emits `Characteristics = 1` for **all four** weak shapes, so routing on it would have classified
   every gcc weak DEFINITION as unresolvable — *precisely the defect the lane existed to fix*. The
   field that discriminates is the record's own `TagIndex`. ★ **This is the same trap as the
   Mach-O `isData` no-call-signal case (a relocation's arithmetic substituted for its role) and as
   the PE `/ALTERNATENAME` declare-and-refuse revisit condition (a front-end feature substituted
   for the existence of a caller). The trap is not any particular field — it is reaching for
   whichever field sits nearest the decision and assuming it carries it.** Where a brief cannot
   supply an instrument, it says *"unmeasured, verify first"* rather than stating the fact flat.
   ★★ **AND THE LANE THAT REFUTES ITS BRIEF IS THE CONTROL LOOP WORKING, NOT A LANE GOING
   OFF-BRIEF** — say so in the brief, so the lane knows a refutation is a deliverable.
   ⚠ **AND THE FIRST WRITE-UP OF THIS RULE MISSTATED ITS OWN MEASUREMENT** — it said
   that invocation exits 127 with an empty log. ✔RE-MEASURED: it exits **2**, with a named
   refusal. The 127-and-empty-log shape is the DIFFERENT invocation `bash <C:/.../script.sh>`,
   where bash cannot open the SCRIPT (see below). Two failures that look alike were being
   described as one, inside the rule that exists to stop exactly that.
   ★★ **A BRIEF THAT ASSIGNS `tests/<dir>/` GRANTS THAT DIRECTORY'S `CMakeLists.txt` AS
   APPEND-ONLY — AND SAYS SO.** A new `test_*.cpp` cannot RUN without a `dss_add_test` block, and
   that file belongs to the directory rather than to any lane, so a brief that lists the test file
   and not its registration leaves the lane a choice between not landing the test and editing an
   unowned file. ⚠ ✔MEASURED 2026-08-20 (cycle P23): four lanes added tests and
   three shared `CMakeLists.txt` files were each edited by lanes that had not been given them.
   Append-only edits merged cleanly; the damage came from ONE lane rewriting a whole file in CRLF,
   reddening `line_endings_guard` for three other lanes' work and leaving a diff nobody could claim.
   ⇒ **Append a block; never reorder, reformat, or rewrite the file whole.** Append-only is what
   makes a shared file safe under concurrency, and it is also what makes a violation visible.
   ★★ **A MESSAGE TO A LIVE LANE RE-STATES THAT LANE'S SUBJECT AND OWNED PATHS, IN ITS
   OPENING LINES.** A lane handle is an opaque id; several lanes run at once; and a message from
   the orchestrator carries the orchestrator's authority. ⚠ ✔MEASURED 2026-08-20 (cycle
   P23): an ownership-NARROWING message
   — reassigning a file set and asserting *"your scope was always X"* — was delivered to
   the wrong lane. Had it been obeyed, two lanes would have edited one file set and BOTH reports
   would have become unattributable, which is the same damage class as editing a lane's config
   underneath it. **It did no damage for exactly one reason: the recipient's BRIEF named its own
   subject and listed those paths as FORBIDDEN**, so the instruction contradicted a written
   boundary instead of arriving into a vacuum — and the lane refused it and answered with a
   measurement (`git status --short` + `stat -c %y`) rather than a denial.
   ★ The reusable half: **an instruction that names the recipient's scope can be REFUTED by the
   recipient; one that only names the work cannot.** Redundancy in the addressing is what makes
   mis-delivery detectable at the destination, which is the only place it can still be caught.
   ★★ **THE DELIVERABLE TRAVELS IN THE REPORT, NEVER AS A PATH — AND THE BRIEF SAYS SO.** A lane's
   registry row text, its red-on-disable transcript, its md5s and any number the fold will quote come
   back INLINE in the reply. `scratchpad/<cycle>/<lane>/` keeps its P23 job — a private place for
   harnesses and intermediates — and stops being a place a RESULT is left.
   ⚠ ✔MEASURED 2026-08-24 (cycle P31): TWO lanes in one
   cycle reported by citing a path, and both paths were empty when the orchestrator read them — one
   of them holding the lane's **registry row**, which IS that lane's deliverable, and the other a
   483-row byte-identity baseline taken at a named commit.
   ★ **The mechanism is an interaction between two rules that are each correct alone**, which is why
   neither side looked wrong: `scratchpad/` is gitignored, and a `git worktree` gets **no copy of an
   ignored directory** — so a lane working in a worktree writes into a scratchpad the main tree does
   not have, while the orchestrator reads one the lane never wrote to. Do NOT "fix" this by
   un-ignoring `scratchpad/`: it holds build spill and half-written harnesses, and it would not help
   the worktree half at all, because the ignore rule is not what separates the two trees.
   ⇒ **The one-line test to put in the brief:** *if the orchestrator would have to open a file to
   fold your work, the work is not reported yet.*
   ⇒ **A lane that uses a `git worktree` NAMES IT in its report**, because the orchestrator must
   `git worktree remove` it at the fold and cannot remove one it does not know about.
   ★★ **AND BEFORE EDITING A FILE YOU OWN, COPY IT INTO YOUR SCRATCH DIRECTORY — that
   copy is your ONLY sanctioned undo.** The standing order forbids `git stash` / `checkout --` /
   `clean` / `reset` because the tree is shared, and that prohibition is correct and stays
   BLANKET. ⚠ But it was SILENT about a need it creates: a lane that corrupts its own
   exclusively-owned file has no way back except the one thing it is forbidden to do.
   ✔MEASURED 2026-08-24 (cycle P31):
   a lane ran `git checkout -- <its own config file>` to undo a malformed patch of its own, then
   disclosed it unprompted. ★ **The disclosure is the only reason anyone knows** — a restored
   file looks exactly like a file that was never edited, so this violation leaves nothing in any
   diff, which makes it the one class of rule-break that cannot be caught after the fact.
   ⇒ restore from your scratch copy: it restores exactly one file, cannot reach another lane's
   work even by mistake, and needs no judgement about what `--` would have swept.
   ★ The distinction to hold: **the ban is on the INSTRUMENT, not on the intent.** Undoing your
   own bad edit is legitimate; doing it with a tree-wide tool is not. And the prohibition keeps NO
   carve-out for "only my own files" — a tired lane applies that to a file it merely BELIEVES
   it owns, which is the case the rule exists for.
   ★★ **A BYTE-IDENTITY BASELINE IS TAKEN AS AN ISOLATING PAIR, NEVER INHERITED —
   AND IN A SHARED TREE ITS SHELF LIFE IS MEASURED IN HOURS.**
   ⚠ ✔MEASURED 2026-08-24 (cycle P31):
   a lane diffed a predecessor's 483-row baseline, taken two hours earlier at the same commit, and
   got **13 differing lines with 8 examples flipping to NO-ARTIFACT — none of them its own**. A
   sibling lane's front-end work had landed in between, while the instrument's `cfgroot` snapshot
   still pinned HEAD's language document.
   ★ **The trap is that both failure modes produce the SAME diff:** *"my change moved these
   bytes"* and *"the world moved underneath my baseline"* are indistinguishable by looking, and only
   one is a defect. A lane that trusts an inherited baseline either hunts a regression it did not
   cause, or — worse — accepts 13 moved rows as noise and misses a real one.
   ⇒ **Take BOTH halves yourself:** revert only YOUR files to HEAD with
   `git cat-file -p HEAD:<path>` (never `checkout --` or `stash`, which reach the whole shared
   tree), leave every other lane's work in place, take the BEFORE; restore your files, take the
   AFTER. Both runs then see the same sibling state, so your diff is the only variable left. ✔That
   is what produced that lane's result: **zero differing lines in 486 pre-existing rows**, the final
   manifest differing by exactly one ADDED row for its new example.
   ★ Two corollaries, each paid for: **an inherited baseline is usable only with a CONTROL** that
   re-derives a handful of its rows against the live tree — cheap, and it separates stale from
   broken in one run; and **a baseline's identity is the CONFIG SNAPSHOT plus the commit**, not the
   commit alone, so an instrument pinning a `cfgroot` must record which one and a reader must never
   assume HEAD.
   ⭐ **AND A PATH THAT SOMEBODY ELSE MUST RESOLVE IS ABSOLUTE, OR NAMES ITS ROOT.** TWO roots
   answer to the name `scratchpad/`: the repository's (gitignored) and the SESSION's, under
   `…/AppData/Local/Temp/claude/<project>/<session>/scratchpad/`, which is outside the repo
   entirely. A lane writing to one and reporting a bare relative path sends the orchestrator to the
   other, and both readings are plausible.
   ★★★ **AND THE CLAUSE THE ORCHESTRATOR'S OWN ERROR HERE ADDS, WHICH BINDS EVERY
   PARTY: A NEGATIVE RESULT CARRIES THE SCOPE IT WAS TAKEN OVER.** ⚠ ✔MEASURED 2026-08-24
   (cycle P31): the orchestrator ran `find` over the REPO root, found none of a lane's seven
   instruments, and told that live lane *"✔MEASURED just now: none of them exists"*. They were
   intact in the session root the whole time — the search could not have seen them. The lane
   began rebuilding a 483-row byte-identity baseline on that word, and the next hazard was a
   reconstructed baseline reconciled against a real one: a claim with two provenances and no way to
   separate them. ⇒ *"not found under `<root>`"* is a measurement; *"does not exist"* is a claim
   the instrument did not make. Before telling a lane something of its own is missing, search every
   root that could hold it — and prefer **asking the lane where it put the thing**, since it is
   the one party that knows. ★ This is the same species as a lane's vacuous key-name scan and as
   a guard clause that cannot fire on the gating leg: **a SCOPED instrument reporting an UNSCOPED
   claim.** It is worse from the orchestrator, because a lane can refute its brief, while a lane
   cannot easily refute a measurement handed down as fact.
   ★★ **AND A BRIEF THAT RELAYS A PRIOR LANE'S ARTIFACT MUST OPEN ONE OF THEM FIRST**, or say
   *"unverified, rebuild your own"*. Same measurement: the replacement brief for that second lane
   asserted its scratchpad *"ALREADY CONTAINS the instruments and baselines"* and named seven files,
   relayed from the prior lane's report with none of them opened — this section's own
   run-it-before-you-write-it rule, violated one level up by the party that enforces it. ★ The
   damage that was nearly done is the instructive part: not wasted effort, but a **RECONSTRUCTED
   baseline presented as the prior lane's** — a byte-identity claim with no provenance, which is
   evidence-shaped and worth nothing.
6. **Review and fold** — `/pr-review-toolkit:review-pr`, plus the agnosticism pass and the CI-hazard
   screen. ⓘ There is no twin parity to review any more: a program under
   `.harness-config/runner/actions` is ONE `.py`, and `scripts_index_guard` refuses a `.sh` or `.ps1`
   there (see the layout section below). **Re-review the fold** if folding changed logic; iterate to a fixed point. Passes that
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
    this gate until clean. A finding implying a *design choice* is a pause gate, not a loop.
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
    *A COMPLETED SET OF LANES IS A COMMIT POINT* above. A cycle running several sets of lanes lands
    several commits on one PR, and **the next set is seeded only AFTER this step**, so every lane
    can name a COMMIT as the tree it started from.
12. **Report and end** (contract below). The invocation ends here; under `/loop` the next invocation
    begins the next cycle with fresh context.

## Output contract

★★★ **OPERATOR INSTRUCTION 2026-08-17 — SILENCE IS THE DEFAULT. EMIT NOTHING THE OPERATOR DOES NOT
NEED IN ORDER TO PROCEED.** Verbatim: *"requires no output tokens unless what I need to know to
proceed (failures, done/not done, final report, etc)"*.

**The complete list of things worth emitting:**
1. **A failure or a blocker** — something is red, refused, or cannot proceed. State it, with the
   measurement, and what you are doing about it.
2. **A pause gate** — a decision only the operator can make (§B, a pending definition, an unfired
   trigger, a hard stop). This is the one case where length is justified: options, trade-offs,
   recommendation.
3. **Done / not done** — a step's terminal state, when the operator's next action depends on it.
4. **The final report** — the output contract below.
5. **A direct answer to a direct question.**
6. ★★★ **A DssHarness finding** (operator ruling, 2026-09-16: *"please also put in dss-cycle skill
   that any issue found in DssHarness must be reported to me (the operator)"*). **Emit it even when
   it neither fails nor blocks this cycle** — which is the usual case, because the cycle routes
   around a tool defect by using the script that still exists, and the finding then matches none of
   categories 1–5 and dies in a lane report. That silence is what this item closes.
   - **Same FORM as everything else:** what was run, what happened, what should have happened, and
     the exact source symbol in repo-harness that produces it — **path + SYMBOL, never a line
     number**. A few lines. No significance commentary, no derivation.
   - **Report it in the cycle that FINDS it**, never filed for later — the same shape as *fix it when
     you face it*.
   - **A DssHarness defect does NOT become a `D-*` row here**; it is not this repository's defect.
     **The cycle report is its only route out**, which is why this item has to exist at all.
   - ⚠ **Reporting does not replace fixing our side.** Where the defect has a correct LOCAL
     expression that is not a workaround — spelling a `minVersion` with three components because the
     tool documents semantic versions, say — fix it here AND report the tool's part. Not alternatives.

⛔ **Everything else is noise, and the list of what NOT to emit is the useful half:** no progress
narration ("lane X is running", "starting the build"), no interim summaries of work that is not
finished, no restating a lane's report back, no explaining a finding that is already written into
the registry row and the handoff — **the row IS the deliverable; a prose retelling is a second copy
that will go stale**. Do not announce what you are about to do, then do it, then announce that you
did it. Do not re-report a number the operator has already been given.
⚠ **This does NOT license silent failure or thin measurement.** Rigor is unchanged: measure
everything, anchor everything, write the registry rows and the handoff in full. The instruction is
about the CHAT CHANNEL only — put the detail where it persists, not where it scrolls past.
★ Test to apply before emitting: *does the operator have to do something differently because of
this?* If no, it belongs in the row, not in the reply.

★★★ **OPERATOR CORRECTION 2026-08-17, SECOND PASS — THE CATEGORY LIST WAS NOT ENOUGH, BECAUSE THE
LEAK IS NOT *WHICH* ITEMS GET EMITTED, IT IS *HOW*.** Verbatim: *"silent mode is not working. You
need to talk only things I need to know (forks, errors, final reports, etc.), not your own
reasoning."* An emission can sit squarely in category 1 or 3 and still be almost entirely noise,
because the fact arrives wrapped in the reasoning that produced it. **The fact is the payload; the
reasoning is not.**

**FORM, not just category — an allowed emission carries the fact and its measurement, and stops:**
- ⛔ **No significance commentary.** Not *"that changes what delivered means"*, not *"this is worth
  flagging"*, not *"the interesting part is…"*. State the fact; the operator ranks it.
- ⛔ **No meta about the telling.** Not *"I'd rather say it now than at commit time"*, not *"stating
  it plainly rather than burying it"*, not *"before I put it to you"*. Just say it.
- ⛔ **No derivation.** The operator wants the conclusion and the number, not the path. *"886/886"*,
  not the reasoning that made you re-run it.
- ⛔ **No roads not taken.** What you considered and rejected belongs in the row, never in the reply.
- ⛔ **No relaying a lane's report.** A lane's findings go into the registry and the handoff. Emit
  only the part that changes the operator's next action, in your own one line.
- ★ **Length is the tell.** A failure, a done/not-done, or an answer is **1–3 lines**. If it runs
  longer and is not a §B pause gate, the excess is reasoning — cut it, do not compress it.

⚠ **The one exception stays the §B pause gate**, which needs options, trade-offs and a
recommendation. Everything else is a sentence or three.

A one-line cycle summary — priority closed, anchors touched, test delta, commit hash — plus:

```
anchors: opened N, closed M, net ±K — OPEN was <before>, now <after>
next: <one line, matching the top NEXT entry in .plans/_handoff.md>
```

**The anchor line is MANDATORY and carries numbers, not an adjective.** The gate already refuses
`after > before`, so this line is the receipt, not the check — "anchored a few follow-ups" is exactly
what the gate exists to make impossible to say. If the report and the handoff disagree about what
comes next, fix the handoff: it is the one a future reader will find.

## ★★★ USE THE SCRIPT THAT EXISTS — AND FIX IT RATHER THAN ROUTING AROUND IT

**Operator instruction 2026-08-19, verbatim:** *"if a tool has a problem, fix before using again, not
workaround an own tool. reusable tools exists to avoid bunch of problems like mangling or edge cases"*.

- **Look in `references/actions.md` first.** It indexes every program this repository ships — each a
  DssHarness action under `.harness-config/runner/actions/` — with its purpose. If one covers the
  job, invoke it — not "something like it" typed inline.
- **A defect in one of them is FIXED in the cycle that hits it.** A workaround at the call site leaves
  the defect for the next caller and forks the behaviour silently. This is a FIX, so by the 2026-08-15
  ruling **no hard stop gates it**, whatever subsystem it lands in.
- ⚠⚠ **AND `bash <script>` FROM A WINDOWS-NATIVE PARENT IS NOT THE BASH YOU MEAN.**
  ✔MEASURED 2026-08-20 (cycle P23): from a Windows-native process, `bash` resolves to
  `C:\WINDOWS\system32\bash.exe` — **WSL's** — which cannot open a `C:/...` path. Two
  distinct failures follow and they look alike:
  * `bash <relative-script> <C:/...log>` — the script RUNS and cannot write its log; a
    well-written one exits **2** with a named refusal that identifies the shell.
  * `bash <C:/.../script.sh>` — bash cannot open the SCRIPT, so **it never executes**.
    Exit **127**, empty log, and **no edit inside any script can ever improve this shape**.
    The only fix is at the CALL SITE: invoke it as a relative path from Git Bash. ⓘ No
    repository program is a shell script any more (2026-09-21), so this bites only a hand-typed
    command; the root `CMakeLists.txt`'s bash probe retired with the last bash-driven ctest entry.
  ★ Worth stating because the second shape reads as *"the gate refused"* when what happened
  is *"the wrong bash ran"* — an instrument that misattributes is the failure this project
  cares most about.
- ⚠ **The reason is measured, not aesthetic.** These programs hold this project's accumulated edge
  cases: a `wsl.exe bash -c` with a variable that once became `rsync -a --delete / /` and reported
  exit 0; quoted heredocs eating backslashes; unanchored rsync excludes that silently skipped a
  changed `.cpp`; `command -v` lying over non-interactive ssh on macOS. Re-typing the pipeline inline
  re-opens all of them at once.

### ★★ MANDATORY: an action added, renamed, deleted, or REPURPOSED updates the reference

In the **same commit**, exactly like `.plans/_handoff.md` — a reference that ships one commit late is
a reference the next reader cannot trust. This is enforced rather than asked: each action declares its
purpose once in a `PURPOSE:` comment line in its own `<name>.yml`, both indexes
(`.harness-config/runner/actions/README.md` and `references/actions.md`) are generated from those
declarations, and the `scripts_index_guard` ctest entry reds when the tree and the indexes disagree —
and when an action has no runner in `.harness-config/config.json`, or a runner names no action.

```bash
python .harness-config/runner/actions/check-scripts-index/check-scripts-index.py --write
```

⚠ A new program also inherits the repository's layout, and the guard checks it: one ACTION directory
per program, named for it, holding its `<name>.yml` and every sibling implementation
(`.harness-config/runner/actions/[<group>/...]<name>/<name>.{yml,py}`), assets alongside, and
nothing loose in a group directory, buried in a subdirectory, or nested inside another action. A
program finds the tree it lives in through the one owner, `owning-tree`, never by counting `..` —
the move out of `scripts/` broke every count at once.
### No `.sh` and no `.ps1` under the actions directory — one Python program per action

**Operator ruling 2026-09-21:** *"I don't want .sh/.ps1 files inside .harness-config\runner\actions.
entrypoint is .yml, you can call .py files, BUT NOT .sh/.ps1 please. They are specific per OS. I don't
want this anymore"*.

- Every step of every action starts `python3 <file>.py`; one program runs on every host, so there is
  no twin to write and no parity to keep. Where a host cannot run a half natively, the program says
  where it runs (the sqlite driver's POSIX half runs inside WSL through `wsl.exe -e` on Windows).
- **A gate, not a convention:** `scripts_index_guard` refuses a `.sh` or `.ps1` anywhere under the
  actions root, by name (the tool's own run directories excepted).
- The 2026-08-19 convention this replaced — a `.ps1` twin for every `.sh` that had to reach the
  Windows leg, the pair's parity checked in review — retired with the last pair on 2026-09-21; what
  each twin became is recorded in `cmake/DssHarnessDeletionInventory.md`.

## ★★★ NEVER CITE A LINE NUMBER — CITE SOMETHING THE FILE CARRIES

**Operator rule, 2026-08-19, verbatim:** *"we must never document line numbers, we must document
method names, comment ids or defined anchors. everything that changes is unreliable. so it's just a
matter of, when finding the path:line, replace the line number by a fixed reference."*

Applies to **every** artifact a cycle writes — registry rows, the handoff, plans, skill references,
commit messages, and code comments alike.

```
✗  src/mir/lowering.cpp  + a line number    <- moves the instant anything above it changes
✓  src/mir/lowering.cpp — lowerCallArgs()
✓  tests/CMakeLists.txt — the `no RUN_SERIAL` rationale block
✓  a defined anchor id, when a registry row is the subject
```

**A symbol survives every edit above it; a line number survives none** — and the failure mode is the
bad one: a citation that BREAKS gets noticed, while one that silently becomes WRONG still resolves,
still reads as evidence, and now points at unrelated prose.

⚠ **✔MEASURED twice inside one cycle (P17), which is why this is a rule and not advice.** Inserting a
one-line header into eighteen scripts moved **16** plan citations off their subjects. The rows then
written to RECORD that defect shipped **three more** wrong numbers of their own, each naming the
first line of an explanatory comment instead of the code it explained. Independent audit caught both;
no gate saw either.

**Enforced** by `plan_citations_guard` (ctest) over `.plans/**` and `.claude/**` as a **ratchet** —
the ~2365 pre-existing citations sit in a per-document inventory whose ceilings may only come
**DOWN**. A new one reds immediately; converting one reds until its ceiling is lowered in the same
commit, because unclaimed headroom is where the next one hides.

```bash
python .harness-config/runner/actions/check-plan-citations/check-plan-citations.py --write
```

⚠ **Green there means no NEW positional citation landed — never that the plans cite stably.** The
inventory is DEBT: burn it down in whatever document you are already editing.

## Hard stops — always route through the pause gate

★★★ **OPERATOR RULING 2026-08-15 — A HARD STOP GATES *OPENING A CAPABILITY*, NEVER *FIXING A DEFECT*.
THERE IS NO HARD STOP ON FIXES, ANYWHERE, AT ALL.** Verbatim: *"please remove the hard stop on FIXES
at all!"* If the work is repairing something already shipped that is wrong — a silent miscompile, a
crash on legal input, a false rule, a conformance divergence, a guard that asserts nothing — it is a
FIX, and **no hard stop applies to it** regardless of which subsystem it lands in. Fixes proceed
autonomously under the ordinary bar.
⚠ **Why this needed saying:** a hard stop is a scope guard against a cycle quietly starting a large
new arc. Applied to a fix it inverts into the opposite of its purpose — it becomes a reason to leave
known-broken shipped behaviour in place, which is precisely the deferral §A.7 forbids, wearing a
governance rule as a disguise. ✔The case that produced this ruling: `hwtime.h` was blocked by
`__inline__` handling, which touches the inliner; treating that as OPT7-gated would have parked a
measured defect behind a rule written to stop *new pass development*.
★ **The distinction to apply, and it is about the DELIVERABLE, not the file you edit:**
*"does this make something CORRECT that is currently WRONG?"* → **FIX, no gate.**
*"does this make DSS able to do something it has never done?"* → **capability, gate still applies.**
Touching a gated subsystem's source does not by itself make it a capability; the OPT7 gate is about
opening the inter-procedural *arc*, not about every line in `src/opt/`.

- **OPT7 / inlining** (`G-406`, plus its cross-CU sub-anchor) — first inter-procedural pass, touches
  linkage / DCE / cross-CU legality. A supervised cycle; **never open autonomously.**
  ⇒ **Gated: opening the arc.** ⇒ **NOT gated: fixing a defect in inlining that already ships**,
  per the ruling above.
- **Trigger-gated anchors** — NOT a TODO. "Do not build until the trigger fires." If it has not
  fired, skip and report "trigger not fired". Backlog ordering is sequencing guidance, not a closure
  license.
- **Correctness-critical anchors** (silent-miscompile class) — the closing cycle MUST ship a negative
  miscompile-pin that breaks iff the transform mis-fires. If the pin cannot be constructed, STOP and
  bring a decision brief. Never ship on review alone.
- ★★★ **A RED CI LEG — read at step 0 with `dssharness check-ci-legs`.** It stops the cycle from
  picking new work until it is diagnosed, and since repairing it is a FIX, **no other hard stop
  applies to the repair itself**. ⛔ The stop is on *proceeding past it*, never on fixing it, and
  **never** on touching the operator's PR: do not label, re-run or push to re-trigger CI.
  ⚠ **The local gate cannot substitute for it, and assuming otherwise is the defect.** ✔MEASURED
  2026-09-14: the two legs that were red run in configurations **no local leg builds** — the local
  Windows gate is **MinGW GCC**, so every `deps = msvc` fact is invisible to it, and the two SSH
  legs (macOS, arm64 VPS) skip `-LE repo-guard`, so a guard-only defect is invisible to THEM.
  ⚠ **WSL was NOT in that set** — ✔MEASURED 2026-09-16, the WSL driver ran every guard while the
  two ssh drivers excluded the label; the counts said so (Windows 2207, WSL 2206, macOS/VPS 2167).
  ★★★ **THE DURABLE RULE: a guard is HOST-INDEPENDENT — it reads the TREE — so which legs execute
  it is a CONFIGURATION decision, and a leg that skips it cannot see a guard-only defect.**
  **Re-derive the set from the run in front of you**, never from this page: the divergence above
  existed precisely because three programs each answered the question separately.
  ⚠ Read the count from `ctest -N -L repo-guard` or from that commit's own
  `repo-guard label applied to N test(s)` configure line — never from here.

## Stop-command handling

On a stop mid-cycle: **finish the current cycle's full flow through commit and push**, then halt. Do
not begin a new cycle. Two things the stop does not override — **a gate you cannot reach cleanly**
(report, never push broken), and **an unanswered pause gate** (you cannot fabricate a resolution;
commit WIP only if legitimate, re-present the brief, halt). The stop tightens the loop to a close; it
never lowers the bar.

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
  anchor** — `DssHarness` is the tool this repository's harness is moving to, and that file says which
  of its verbs exist today, which of this repository's own programs are still the only way to do
  their job, what this
  repository's `.harness-config/config.json` declares, and the exit codes to act on. ⛔ A defect in
  the tool is a repo-harness issue, never a local workaround, **and it is REPORTED TO THE OPERATOR in
  the cycle that finds it** (ruling 2026-09-16) — see output-contract item 6.
- Read `references/actions.md` **before writing any script, probe, or one-off shell pipeline** —
  the index of every program this repository already ships, each with its purpose. Most of what a
  cycle needs is already there, and re-typing it inline re-opens the edge cases it was taught
  (`wsl.exe` quoting, heredocs eating backslashes, unanchored rsync excludes, ssh dropping PATH).
- Read `references/worktrees.md` before any byte-changing measurement or agent worktree operation.
- Read `references/build-layout.md` before creating ANY build tree (step 5) and before reporting a
  cycle complete (step 11) — **one root `build/`, subdirectories for distinct builds, and lane builds
  cleared once the gate covering them is green.** Operator instruction 2026-08-17; a surviving
  `build/lane-*` blocks the completion report the same way the anchor-balance gate does.

## Failure modes this skill exists to prevent

- **Calling a lane "done" when it has only REPORTED.** *"Complete means folded"* (operator,
  2026-08-28). The report describes a worktree; until the fold, the main tree has nothing — and this
  one ships green, because the tests that would have caught the absence are in the tree that was
  never folded. Check the MAIN tree, not the report.
- **Guessing past a decision.** A pending definition, a real fork, an unfired trigger, or a hard stop
  is a pause — not a default, not a pivot to other work.
- **Inventing a fork to avoid the hard part.** If you cannot state a second defensible design, the
  hard part lands this cycle.
- **Breaking agnosticism** by branching the engine on language, arch, or format identity instead of
  extending config vocabulary.
- **Shipping a test that passes both ways**, or closing a correctness-critical anchor without its
  negative miscompile-pin.
- **Auditing your own work.** Steps 4 and 10 are independent subagents precisely so they cannot
  rubber-stamp reasoning they authored.
- **Committing the handoff separately** — it then describes a tree that no longer exists.
