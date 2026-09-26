# Production anchors first — the registry, its rows, and the door that writes them

What the cycle may pick (production, always), where rows live (a working list and an archive), what
a row is (six cells), how it closes (by MOVING), and the one door that writes and reads it.

## Contents
- Production anchors are the priority, ALWAYS (operator ruling 2026-08-25) — the two registry files;
  the harness registry retired on 2026-09-16
- Move on close — a closed row does not stay where it was
- The row shape is six cells, and two of them are declarations
- The door — `dssharness write-anchor` / `set-anchor`: never hand-assemble a row, never hand-read one
- How the ruling binds this loop, clause by clause
- Never quote a per-bucket count — re-derive it

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
this repository's configuration, and which of this repository's own programs (its ACTIONS) it runs, and how.

[→ the 2026-09-24 paragraph that stood here — build, sync, test and worktree management go through DssHarness, never a script — now opens the matching section of dss-harness.md](dss-harness.md)

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
`dssharness write-anchor` refuses anything else, the retired `🔵 🟠 OPEN (DISCLOSED)` included.

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
- ★ **The bands `P0`..`P5`** — as `burndown-queue.py` defines them (`BAND_TITLE`, highest first): `P0`
  WRONG-OUTPUT, ships a bad binary — silent wrongness, or a crash on legal input · `P1` REFUSED, real code
  that does not compile, link or parse today · `P2` DIVERGENT, a product-namespace divergence or an absent
  capability · `P3` HARNESS, the test / gate / build / cycle instruments · `P4` RECORD, the plans, the
  registry, the documentation · `P5` ENV, environment and upstream — not ours to fix in the compiler.
- ⚠ **`check-anchor-balance` ARM 6 refuses a row whose `Status` column contradicts the verdict
  leading its `Trigger` prose.** Two cells now state the same fact, so they can disagree — silently,
  because the gate would believe the column while every human reads the prose.
- ★ **Plan-side §3.1 tables were NOT migrated** and still use the four-cell shape. Both are
  recognized; only the registry documents changed. SUPERSEDED 2026-09-25 by the registries being the only home a row can have — a plan-side table is no longer a sanctioned home, and the live balance, `dssharness check-anchor-balance`, reads only the two registries (see no-follow-ups.md)

### The door — `dssharness write-anchor` / `set-anchor`

A row is WRITTEN by DssHarness's `write-anchor` (a new row) or `set-anchor` (an existing one), and by
nothing else; `read-anchor` / `read-anchors` read it (`references/anchors-and-deferrals.md`).
`.harness-config/runner/actions/anchors/anchors.py` has no write verb: it is the reader
(`read` / `list [--lint]`, each taking `--production` / `--done` and only those two — the harness
registry retired on 2026-09-16) and the one launcher that `lane-fold` and `apply-registry-row` call
the door through. [→ `anchor-rows` is its third caller: it applies a lane's rows directory as one batch, rehearsed first, all or nothing](lane-discipline.md) That launcher refuses, before the door, a status or band outside the vocabulary, an update naming no
field, and a cell the door would store broken, judged as the door will store it (a line break becomes a
space): an anchor id broken after a hyphen or wrapped inside a segment, or a path cut after its `/`. These
checks are keyed on the id grammar config.json declares (`anchors.idPrefix`, `minimumIdSegments`). An update
names only the fields that change. The verdict split and in-line whitespace have been the door's own checks
since DssHarness 0.5.9.

```
dssharness write-anchor  D-<AREA>-<NAME> --priority P1 --status open \
                                 --trigger '...' --closing '...' --cross-refs '...'   # writes; --anchor-dry-run previews
dssharness set-anchor    D-<AREA>-<NAME> --status closed --closing '...'           # MOVES it
dssharness read-anchor   D-<AREA>-<NAME>                    # the full row, field by field
dssharness read-anchors  --pending --band P0       # name + priority + status only
dssharness read-anchors  --lint                       # every row a reader cannot key on
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
python .harness-config/runner/actions/check-anchor-balance/check-anchor-balance.py --breakdown --denominator registry ⏳ SCRIPT-ERA (superseded 2026-09-17: a band's count is `dssharness read-anchors --pending --open --band <P>`, whose listing ends with it; see the paragraph below)
```

⚠ That gate canonicalises BOTH registry files to ONE key — deliberately, so that MOVING a row
between buckets is correctly a no-op — so its breakdown gives the registry TOTAL, and a per-bucket
split must sum to it. ★ **That sum is the cross-check that catches a mis-bucketed or double-counted
row, and it is the only reason the P34 error surfaced at all.** SUPERSEDED 2026-09-16 by the harness registry's retirement — one working registry is left, so the buckets are the Priority bands: `dssharness read-anchors --pending --open --band <P>` lists a band and ends with its count, and the six bands must sum to the count the unfiltered listing ends with, the same cross-check, ✔MEASURED to hold on 2026-09-25 (see dss-harness.md)
