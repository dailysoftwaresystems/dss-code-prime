# Anchor registries and the no-follow-ups rule

The two registry files, the six-cell row, move-on-close, the production-first priority, and the
no-follow-ups rule — restated here for readers of this skill. The cycle-side mechanics (the door's
commands, the gate, lane remnants) are in the `dss-cycle` skill.

## Contents
- Two anchor registries — one working list and an archive
  - The row shape is six cells, and two of them are declarations
  - Move on close, and never hand-write a row — and the production-first rule (operator, 2026-08-25)
- No follow-ups — a row you open, you close (operator ruling 2026-08-26)

## ★★★ TWO ANCHOR REGISTRIES — ONE WORKING LIST AND AN ARCHIVE

Deferrals live in **two** files under `.plans/`. The archive was carved out on 2026-09-01 (*"split
what's done from what's to be done. this way we adjust our skills to only read what's yet to be
done"*). ⚠ **It was three until 2026-09-16**, when the harness registry retired with the move to
`DssHarness` — the production/harness split of 2026-08-25 (*"the priority is real errors, not
cosmetics"*) survives as a PRIORITY, not as a second document:

| file | holds |
|---|---|
| `_deferred-anchor-registry-production.md` | every **still-open** row. Since 2026-09-16 that is ALL of them: the harness registry retired with the move to `DssHarness`, and a defect only we can hit is now either that tool's to fix or a production row here |
| `_deferred-anchor-registry-done.md` | every **CLOSED** row, in one table. **Nothing here is work.** |

⚠ A row's bucket follows the **DEFECT, never the instrument that found it**. `D-CONFIG-*` and
`D-DIAG-*` are PRODUCTION deliberately: in this architecture a `.lang/.target/.format.json` document
IS the compiler's behaviour, and a diagnostic IS its output to a user.

### The row shape is SIX cells, and two of them are declarations

    | Anchor | Priority | Status | Trigger | Closing work | Cross-refs |

`Priority` is `P0`..`P5`; `Status` is `✅ CLOSED` / `🟠 OPEN` / `⏳ GATED` / `🔵 DISCLOSED`, and
`--status` or a `--status-file` holds one of those cells or its bare word (`closed`, `open`, `gated`,
`disclosed`); `DssHarness write-anchor` refuses anything else, the retired `🔵 🟠 OPEN (DISCLOSED)`
included.
The status cell keeps its glyph because the project's one definition of closed is *"the cell OPENS
with ✅"* — a column holding the bare word would make that test false for every closed row at once.
`DISCLOSED` marks OPEN work whose debt PRE-DATES this cycle: it counts as open everywhere and is
exempt only from the balance gate's net-increase refusal, so writing up a defect you merely FOUND is
not punished like shipping a new deferral.

### Move on close, and never hand-write a row

- **Closing a row MOVES it** out of its working registry into the archive; reopening moves it back.
  `check-anchor-balance` fails the tree for a closed row left behind, for an open row filed in the
  archive, and for a `Status` column that contradicts its own `Trigger` prose.
- **`dssharness`** is the door — `write-anchor`, `set-anchor`, `read-anchor`, `read-anchors`. The
  writer takes the FIELDS, so a wrapped anchor id (invisible to every grep, and it mints a false
  id), an unescaped `|` and a wrong cell count are inexpressible. ⚠ The registry selector is
  `--pending`, not `--production`; `--done` is unchanged.
  ⏳ **SCRIPT-ERA, and the predicate has fired:** the eight `.harness-config/runner/actions/anchors/*.{sh,ps1}` launchers
  this line used to name are DELETED. `.harness-config/runner/actions/anchors/anchors.py` itself survives — it is still the
  subject of `anchors_selftest_guard` — but it is no longer a door anyone should reach for.
- ★ **RESOLUTION reads both files; ORIENTATION reads only the working one.** A `D-*` cited in `src/`
  must resolve wherever its row lives, so resolvers glob `_deferred-anchor-registry*.md`. Anything
  asking *what is left* reads production and stops there.
- ⚠ **`--harness` is no longer a flag.** ✔MEASURED 2026-09-16 at `305604f1`: every verb's usage line
  reads `(--production | --done)`, and nothing else is accepted. [→ that usage is the surviving `anchors.py` reader's, still true of it (✔MEASURED 2026-09-25); the live door, `dssharness read-anchors`, takes `--pending`, `--done`, `--band`, `--open`, `--closed`, `--json` and `--lint`](../../dss-cycle/references/dss-harness.md)

**★★★ THE RULE (operator, 2026-08-25):** *"the priority is always production anchors. ALWAYS.
harness we fix as we need when we face the problem (NEVER LATER)."*

- **Production is the only queue.** Harness work is never picked *because it is next*.
- **A harness defect is fixed AT THE MOMENT IT IS FACED** — in that cycle, in that lane. "NEVER
  LATER" is the whole instruction: a gate that lies, a guard blind to its subject, a script that
  blocks the work in front of you, gets fixed NOW. Filing it and routing around it is the failure
  the ruling names. ⚠ **Since 2026-09-16 a defect in `DssHarness` ITSELF is repo-harness's to fix
  and is reported there** — reproduce it, state the measurement, and never work around it here.
- **So a harness row is a RECORD, not a backlog entry** — drained by encounter, not by scheduling,
  and normally written already ✅ CLOSED, naming a fix that landed the same cycle. It goes in the
  production registry now; there is no separate list for it.
- ⚠ **This does not repeal "anchor every issue found".** Still file the row: a harness defect fixed
  silently teaches nobody. The ruling governs what gets **SCHEDULED**, not what gets **RECORDED**.
- ★ **The measure of a cycle is its production movement.** One whose closures are all harness-shaped
  rows has hardened the workshop and shipped nothing.

Every RESOLVER globs `_deferred-anchor-registry*.md`, so `check-anchor-balance` and the registry
guard read both with no flag — but **a human reading the archive is not reading what is left**.
⚠ `burndown-queue` deliberately does NOT band a row from the archive: it exits loudly instead,
because a live row filed there is invisible to every queue in the project and quietly coping is how
an invariant stops being one. ⚠ **Never quote a count from prose; re-derive it**
(`check-anchor-balance.py --breakdown --denominator registry` ⏳ SCRIPT-ERA (superseded 2026-09-17: the live instruments are `dssharness check-anchor-balance` for the balance and `dssharness read-anchors --pending --open --band <P>` for a band, whose listing ends with its count; see dss-cycle/references/registry-and-priority.md)) — the P34 handoff's own production
figure was wrong by 20, and the breakdown is what caught it.

## ★★★★ NO FOLLOW-UPS — A ROW YOU OPEN, YOU CLOSE (operator ruling 2026-08-26)

> *"THIS MUST STOP NOW."* — *"opened anchors that are not closed in the immediate cycle or the
> next are brutally rare exceptions now, not the rule. found something new that must be done? DO
> IT."* — *"If I keep seeing anchors rising after implementations or fixes I'll be really
> pissed!"*

**The full ruling, the gate command, and the shipped-but-unmarked failure class live in
`dss-cycle` SKILL.md [→ now `dss-cycle/references/no-follow-ups.md`](../../dss-cycle/references/no-follow-ups.md) — this is the same rule, restated where a reader of THIS skill will meet it.**
It was a standing operator ruling from 2026-08-24 (*"every time opens more anchors than closes"*)
that had never been written into either skill; ✔MEASURED 2026-08-26. That omission is why it kept
eroding.

- **Close it this cycle, or the next.** Anything longer is a rare exception that must NAME itself
  as one, with the predicate that will close it.
- **Found new work? DO IT.** A new row is the LAST RESORT, not the first response to a finding.
- **Production first, then harness — and a harness defect that BLOCKS you is fixed the moment you
  face it,** in the lane that hit it, never later. A defect in `DssHarness` itself is reported to
  repo-harness instead, and still never routed around here.
- **"Refused but not fixed" is not closed.** Nor is "measured", nor "the row now states the real
  scope". A row closes when the BEHAVIOUR changed.
- **NET OPEN must be ≤ 0 for the cycle**, measured — not asserted — against the cycle's own start
  commit, and reported as closed / opened / net rather than one flattering total:
  `dssharness check-anchor-balance --base <cycle-start-sha>`
- ⚠ **Mark a shipped row ✅ THE MOMENT IT SHIPS.** The gate counts a row OPEN unless its status
  cell explicitly says closed — correct polarity, never to be softened — so a 🟢 "DESIGN RECORD —
  SHIPPED" row inflates the OPEN count for free. Three `D-OPT*` rows were doing exactly that on
  2026-08-26.
