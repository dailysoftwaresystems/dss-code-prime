#!/usr/bin/env python3
# PURPOSE: report the registry's DEBT the balance gate does not -- a closure verdict under a non-closure marker, a gated row with no open opener, a blocker already discharged, an unclassifiable closing-work marker, OPEN-vs-GATED prose, a `[[...]]` pointer that names no row -- and refuse what a change introduced that way.
"""anchor-debt.py -- the registry's DEBT: the row-vocabulary arms no DssHarness verb makes.

★★★ WHAT THIS WAS, AND WHY ITS NAME CHANGED (2026-09-30, cycle P69). This program was `check-anchor-balance`,
the balance gate. The gate moved to DssHarness (`dssharness check-anchor-balance`), and three of the arms this
program ran became the tool's too -- each ✔MEASURED that day by planting the defect in a throwaway repository:
  * the BALANCE itself (a new OPEN row since the base: `check-anchor-balance` exit 1; a new 🔵 DISCLOSED one is not
    counted -- by its STATUS cell, where this program read the Trigger excerpt, the round-12 audit's defect);
  * the registry PARTITION (a CLOSED row in the pending registry, an OPEN one in the done registry: exit 1, and
    `read-anchors --lint` reports the first);
  * ARM 6, a Status column contradicting the verdict leading its Trigger (`read-anchors --lint` reports one written
    by hand; the door refuses one written through it, exit 10 -- `anchors.triggerCarriesVerdict: true`).
Those arms, the `--denominator` / `--breakdown` / `--per-bucket` modes (they counted plan-side homes, retired
2026-09-25, and a `harness` bucket, retired 2026-09-16) and the balance arithmetic are DELETED. The name went with
them: a program called `check-anchor-balance` beside a DssHarness verb of that name that does something else is a
trap. What stays is what no DssHarness verb reports -- every arm a DIFFERENTIAL against `--base` (a row that
already existed there is DEBT, printed on every run so the number cannot quietly become normal; one the change
added or edited is REFUSED):
  ARM 2  a row open with a CLOSURE VERDICT while its marker says otherwise;
  ARM 3  a GATED row must name the row (or plan phase) that opens it, and that opener must be OPEN;
  ARM 4  a row whose stated blocker is discharged is UNBLOCKED, not opened (reported);
  ARM 5  a closing-work lead marker this instrument cannot classify;
  ARM 7  a row whose Status and Trigger prose disagree about OPEN vs GATED;
  ARM 8  a `[[...]]` pointer in a registry row must name a row of either registry;
plus the scan's own FATALS (an anchor table shape it cannot count, an orphan row): never skipped silently.
(ARM 9 -- a row CLOSED at the base that neither registry holds now -- was DELETED 2026-10-07: since
DssHarness 0.6.9 `dssharness check-anchor-balance` fails a lost row, open or closed, ✔MEASURED that day in
throwaway repositories, and an arm stating the tool's rule a second time is a second owner of it.)
It is ALSO the row-vocabulary LIBRARY the anchors family loads by path (`anchors`, `burndown-queue`,
`check-emitted-anchor-ids`, `check-stale-refusal-citations`, `check-wrapped-anchor-ids`): `is_closed`, `split_row`,
`row_name`, `ANCHOR_TOKEN`, `scan_worktree` and the rest -- one owner of what an open row is.

THE RULE IS INVERTED, AND THAT INVERSION IS THE WHOLE POINT: a row is OPEN unless its status cell carries an
explicit CLOSED marker. A glyph nobody has thought of yet counts as open, which is the safe direction -- the
instrument over-reports rather than waving work through. Enumerating the open glyphs is the same mistake as
enumerating build-directory layouts: define the complement, not the variants. (✔MEASURED once: an inline grep that
enumerated open glyphs saw 269 open rows where there were 579.)

HOW TABLES ARE RECOGNIZED -- BY HEADER SHAPE, NEVER BY HEADING NUMBER. Heading numbers are decoration; the COLUMN
SHAPE is the contract. So tables are matched on their normalized header cells, and anything unrecognized that still
looks like an anchor table is REPORTED AND FAILS THE RUN (the severity rule lives in scan_document's docstring: FATAL
iff the MEASUREMENT is incomplete) -- a silently skipped table is the defect the whole instrument exists to stop.

USE -- the action's steps (`dssharness run anchor-debt`): `self-test`, then `debt` (`--base`, default HEAD).
"""
import argparse
import importlib.util
import io
import os
import re
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

# ── OUTPUT ENCODING — AND HERE THE GLYPH **IS** THE FACT ────────────────────────
# ⚠ A GATE THAT CRASHES WHILE REPORTING IS WORSE THAN ONE THAT SAYS NOTHING.
# ✔MEASURED 2026-08-12 (the provenance of the single-stream call this replaced):
# this script died with UnicodeEncodeError on a Windows cp1252 console the first
# time it had to print an OPENED row, because the status excerpt began with 🟠
# (U+1F7E0). Every prior run had printed only counts, so the reporting path had
# never been exercised with real content -- the crash was latent for exactly as
# long as the gate was passing.
#
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, BOTH streams PIPES with
# `PYTHONIOENCODING` unset — exactly how ctest runs a guard): `sys.stdout` comes up
# `encoding='cp1252' errors='surrogateescape'` and RAISES on an ordinary
# unencodable character; `sys.stderr` comes up `errors='backslashreplace'` and
# survives with the glyph mangled to its escape. `surrogateescape` rescues only lone
# surrogates left by an earlier decode -- it does nothing for a real character.
#
# ★★★ WHY `errors="replace"` ALONE WAS NOT ENOUGH **IN THIS GUARD SPECIFICALLY**,
# and it is the whole reason this block replaced the single-stream call that used to
# sit in `main()`. What this instrument prints excerpts of is a registry STATUS
# CELL, and in that registry THE GLYPH IS THE STATUS: ✅ closed, 🔵 disclosed, ⏳/⛔
# gated. Re-encoding to cp1252 with `replace` does not degrade the formatting -- it
# turns the reported fact into `?`, in the one instrument whose job is to report
# exactly that fact. ✔MEASURED side by side on this registry, same input, cp1252
# pipe: no reconfigure ⇒ `UnicodeEncodeError`, nothing printed; `errors="replace"`
# alone ⇒ survives and prints `? **OPEN — named 2026-08-23`; this form ⇒ the glyph
# arrives intact. A report that erases what it quotes is a fail-loud violation, not
# a cosmetic one.
#
# ★ AT IMPORT, NOT IN `main()`, AND BOTH STREAMS. The call this replaced ran AFTER
# `parse_args()`, so every path that prints before it was unprotected -- ✔MEASURED
# at the base ref: `--help` through a cp1252 pipe died with
# `UnicodeEncodeError: 'charmap' codec can't encode character '✔'`, rc=1, ZERO
# bytes of help printed. The `sys.exit(msg)` arms that interpolate git's stderr
# print on stderr and were likewise uncovered.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass

PLANS_DIR = ".plans"
REG_REL = ".plans/_deferred-anchor-registry-production.md"
# ⚠ THE REGISTRY IS TWO FILES SINCE 2026-08-25 (operator: production vs
# tools/harness). `REG_REL` stays as the SELF-TEST document label; the PREFIX below
# is what decides whether a scanned row lives in a registry, and it is a prefix
# rather than a list of two paths so a third split costs nothing and no reader can
# silently see half the registry. `scan_worktree` needed no change at all -- it
# already enumerates every `.md` under `.plans/`.
REG_PREFIX = ".plans/_deferred-anchor-registry"

# The ONLY marker that means closed. Everything else in a status cell is open.
CLOSED_MARK = "✅"  # white heavy check mark
# A row that DISCLOSES debt which already existed rather than CREATING new debt.
# Operator ruling 2026-08-14: "the balance gate forbids a cycle that OPENS NEW debt;
# it does not forbid a cycle that DISCLOSES PRE-EXISTING debt. Those are different
# quantities and the gate should count them separately." Before this, the gate
# counted them as one -- so a cycle that honestly wrote up a defect it merely FOUND
# was punished exactly like one that shipped new deferrals, and the cheapest way to
# pass was to not write the row. That is the precise dishonesty this gate exists to
# prevent, produced BY the gate. A disclosed row is still OPEN work and still counts
# in every total; it is exempt only from the net-increase FAILURE.
# star IT IS NOT A LOOPHOLE, AND THE ASYMMETRY IS THe POINT: the marker asserts the
# defect PRE-DATES this cycle, which is checkable -- the reviewer can look for it in
# the base ref. Marking a defect you introduced is a false statement about history,
# not a formatting choice.
DISCLOSED_MARK = "🔵"  # large blue circle
# ── THE MIRROR OF THE DISCLOSED MARK, ON THE CLOSED SIDE (2026-08-23) ──────────
# The disclosed mark says "this OPEN row's debt predates this cycle". Nothing said
# the same thing about a CLOSURE. A row whose work finished cycles ago but whose
# status cell never got a readable closed marker counts OPEN at the base ref, so
# repairing that glyph today registers as a CLOSURE BY THIS CYCLE and SUBTRACTS
# from its net -- while closing nothing. That rewards bookkeeping over work, which
# is the fourth shape of the same failure the direction note in is_closed()
# already records three of.
#
# star THE ARITHMETIC IS NET-NEUTRAL, AND IT IS DSSHARNESS'S. A bookkeeping
# closure -- its Trigger opening with the pair `✅🧾`, the closure mark FIRST --
# does BOTH of these, and they must not be separated:
#   * it REDUCES the OPEN population by one -- the row really is closed, and the
#     denominator must stop counting it, or the gate keeps over-reporting;
#   * it ADDS one back to the cycle's net -- so the cycle is credited with
#     NOTHING for it.
# That arithmetic lived in this program's balance, which retired to DssHarness on
# 2026-09-30. DssHarness 0.6.6 credited the pair as work (✔MEASURED 2026-10-01, two
# throwaway repositories, one OPEN seed closed since the base with `✅ **CLOSED**`
# in one and `✅🧾 **CLOSED**` in the other: "1 closed ... counted -1" for BOTH),
# reported to repo-harness, and 0.6.7 keeps it net-neutral: ✔MEASURED 2026-10-06
# in the same setup, `change 1 closed (1 bookkeeping) ... counted 0`, the row
# listed `[bookkeeping: not credited]`, the real closure still `counted -1`. This
# program's interim -- its `debt` step listing every `✅🧾` closure since the base
# as NOT PROGRESS -- retired that day: DssHarness reads the pair where it opens the
# Trigger (emphasis before it and a blank between the marks ignored), and is its
# one statement.
#
# star IT IS SPELLED **AFTER** THE CLOSURE MARK, NOT BEFORE, AND THAT IS A
# DELIBERATE ORDERING RATHER THAN A TASTE: is_closed() reads the same leading `✅`
# it always read, so a bookkeeping closure counts CLOSED in this scan's population
# with no second lead-in, and every historical count stands (self_test()'s (f)
# cases pin it). A `🧾` anywhere but straight after the leading closure mark claims
# nothing -- the "satisfied by a mention" failure this registry has recorded twice.
# star AND IT IS A CHECKABLE CLAIM, NOT A FORMATTING CHOICE: it asserts the work
# PRE-DATES this cycle, so a reviewer can look for it in the base ref. Marking a
# closure you actually earned is a false statement about history.

# A markdown table separator row: | --- | :--: | ... |
SEP_ROW = re.compile(r"^\s*\|[\s:|-]+\|\s*$")

# An HTML comment occupying whole lines. ✔MEASURED 2026-08-13: one sits INSIDE the
# body of `22-optimizer-plan` section 3.1 (line 387), severing nine anchor rows from
# their header. See the "transparent, but never silent" note in scan_document().
COMMENT_OPEN = re.compile(r"^\s*<!--")
COMMENT_CLOSE = re.compile(r"-->\s*$")

# star THE ANCHOR TOKEN IS DELIBERATELY WIDER THAN THE OLD ROW REGEX.
# The previous gate identified rows with `^\| `(D-[A-Z0-9-]+)` \|` -- backticked, no
# underscore, no dot, no strikethrough. ✔MEASURED 2026-08-13: that shape was blind to
# TEN registry rows, two of them OPEN (both ids carried an UNDERSCORE, which the
# old character class excluded). An open row the gate cannot SEE is a hole an opened
# row can slip through, so the shape now admits `_`, `.` and decoration.
# This token is used for two jobs only: NAMING a row, and deciding whether an
# UNRECOGNIZED table looks like an anchor table. Inclusion of a row in a RECOGNIZED
# table never depends on it -- every data row of a recognized deferral table counts,
# so no row can be invisible because of how it is spelled.
ANCHOR_TOKEN = re.compile(r"D-[A-Za-z0-9_]+(?:[-.][A-Za-z0-9_]+)+")


class Shape(object):
    """A recognized deferral-table shape: how to find its status cell."""

    def __init__(self, kind, status_col, trigger_col=None, closing_col=None):
        self.kind = kind
        # 1-based index into the row's cells, or None when the shape HAS no status
        # cell (see RESERVED below).
        self.status_col = status_col
        # ★★ THE VERDICT CELL AND THE PROSE CELLS BECAME DIFFERENT CELLS ON 2026-09-01,
        # and conflating them would have moved four arms onto the wrong column. In the
        # 4-cell shape one cell carries all three roles: it opens with the verdict glyph
        # and continues as the trigger prose. The 6-cell registry shape separates them --
        # `Status` is a three-value controlled vocabulary, `Trigger` is the prose -- so:
        #   status_col  : what `is_closed` reads. THE verdict. Nothing else may.
        #   trigger_col : the prose `is_gated` / `is_mismarked_closure` read.
        #   closing_col : the closing-work cell, whose LEAD MARKER arm 5 classifies.
        # Defaulting the two prose columns to the verdict's own position is what keeps
        # every 4-cell table -- the plan-side §3.1 registries, plan-17 §5.4 -- reading
        # exactly as before, with no fixture in the self-test changed.
        # ⓘ RESERVED has no status cell at all (`status_col is None`), so the two prose
        # columns stay None with it rather than becoming arithmetic on a missing index.
        # The three status-cell arms are already silent for that shape.
        self.trigger_col = status_col if trigger_col is None else trigger_col
        if closing_col is not None:
            self.closing_col = closing_col
        elif self.trigger_col is None:
            self.closing_col = None
        else:
            self.closing_col = self.trigger_col + 1


# ── The sanctioned deferral-table shapes, ✔MEASURED across all 39 files in .plans/ ──
#
# 1. REGISTRY-SHAPED  `| Anchor | Trigger | Closing work | Cross-refs |`
#    `_deferred-anchor-registry*.md` (both "Anchor Index" tables) and
#    `17-shader-gpu-plan` section 5.4, which says so in prose: "Column shape matches
#    `_deferred-anchor-registry*.md`". The HEADER says cell 2 is "Trigger"; in PRACTICE
#    every row leads cell 2 with its status, and that de-facto usage is the contract
#    the old gate already relied on.
#
# 2. DEFERRED-ITEMS  `| # | Deferred item | ... |`
#    Plans 12 / 13 / 14 / 22 (their section 3.1 "Deferred-items registry" tables) and
#    plan-00 section 0.2. The TAIL columns differ between plans and that is fine --
#    only the first two cells are load-bearing:
#       12: `# | Deferred item | Source | Real blocker | Landing window`
#       13/14/22: `# | Deferred item | Why deferred (not a silent gap) | Owner / closure | Trigger`
#       00 s0.2: `# | Deferred item | Why deferred (not a silent gap) | Class | Owner / closure | Trigger`
#    So the match is a PREFIX match on ("#", "deferred item"), not an exact header.
#    ⚠ plan-00 section 0.2 numbers its rows `D1`, `D2` ... -- NOT `D-*-*-*` at all. No
#    anchor regex would ever have found them; counting every data row of a recognized
#    table is what makes them visible.
#
# 3. RESERVED  `| Anchor | Owns |`
#    Plans 09.5 section 9, 24 section 6, 28 section 12 -- anchors reserved for a plan
#    that has not opened yet, which those plans say "move into
#    `_deferred-anchor-registry` as active rows" when it does. There is NO status
#    column, so `status_col` is None and every row is unconditionally OPEN. That is
#    both the safe direction AND the correct one: it is what makes the eventual MOVE
#    into the registry arithmetically neutral instead of a +1 regression.
#
# 1b. REGISTRY-SHAPED, SIX CELLS (2026-09-01)
#    `| Anchor | Priority | Status | Trigger | Closing work | Cross-refs |`
#    The three `_deferred-anchor-registry*.md` documents. Operator: *"add columns for
#    priority and status ... this way we always have clean statuses."* Before this the
#    verdict was the first glyph of a prose blob that also carried the trigger, the
#    history and the retraction, and every reader had to agree on where the verdict
#    stopped -- which is exactly the agreement that failed four times in this file's
#    own comment history.
#    ⚠ THE STATUS CELL STILL LEADS WITH THE GLYPH (`✅ CLOSED`, `🟠 OPEN`, `⏳ GATED`).
#    A column holding the bare word `CLOSED` would have made `is_closed` -- which tests
#    for a LEADING ✅, the one definition this whole battery shares -- return False for
#    1,555 closed rows at once. The word is for the reader; the glyph is the contract.
#    ★ THE 4-CELL SHAPE IS NOT RETIRED. Plan-side §3.1 tables and plan-17 §5.4 still use
#    it, and plan-17's prose says its shape "matches `_deferred-anchor-registry*.md`" --
#    a sentence that is now about the registry's ANCESTOR. Both shapes are recognized;
#    only the registry documents were migrated.
SHAPES_EXACT = {
    ("anchor", "trigger", "closing work", "cross-refs"): Shape("registry", 2),
    ("anchor", "priority", "status", "trigger", "closing work", "cross-refs"):
        Shape("registry6", 3, trigger_col=4, closing_col=5),
    ("anchor", "owns"): Shape("reserved", None),
}
SHAPES_PREFIX = [
    (("#", "deferred item"), Shape("deferred-items", 2)),
]

# ── Tables that carry an anchor-looking token in cell 1 but are NOT deferral tables ──
# NAMED, not guessed. Each was read before being listed here:
#   `pr | title | scope`        13-assembler section 3 PR breakdown; one row's cell 1 is
#                               "~~AS3~~ ... (substrate slice + binary ops + D-PLAN12-ARM64-STACKPOINTER-REGISTRATION-ABI-GOLDEN-TESTS)"
#                               -- a PR title that MENTIONS an anchor.
#   `phase | what | closes`     23-full-c cluster tables; the anchors live in the
#                               "Closes" column, and cell 1 is a phase id like "**FC8**".
#   `pattern | reason`          the registry's own "Allowlist (code-internal pins, NOT
#                               deferrals)" table -- it says NOT DEFERRALS on the tin.
#   `order | gap to address | resolution`   v2-gap-catalog; cell 1 is an ordinal.
# ⚠ THIS LIST IS AN ENUMERATION, AND ENUMERATIONS ARE HOW THIS GATE WENT WRONG TWICE.
# It is safe here ONLY because it is not the complement of anything: a header that is
# in NEITHER the recognized nor the excluded list is not ignored, it FAILS THE RUN.
# The residual is loud, so the enumeration cannot hide a table -- it can only decide
# whether a KNOWN table is counted. Keep it that way.
EXCLUDED_HEADERS = {
    ("pr", "title", "scope"),
    ("phase", "what", "closes"),
    ("pattern", "reason"),
    ("order", "gap to address", "resolution"),
    # ── `.plans/_handoff.md` prose tables (added 2026-08-13, read before adding) ──
    # The handoff is a STATE document, not an anchor home: no row of these tables
    # declares a deferral, and none has a status cell for this gate to read. They
    # trip the "looks like an anchor table" heuristic only because a first cell may
    # MENTION a `D-*` name -- e.g. "FC18 — D-DIAG-CORPUS-EVERY-CODE" naming the
    # phase, or a priority row naming the anchor that blocks it.
    # ★ Excluded BY HEADER SHAPE, deliberately, NOT by filename. If a future handoff
    # ever grows a real 4-column anchor table, it still gets counted -- excluding the
    # whole file would have created exactly the silent skip this gate exists to stop.
    ("destination", "the named gap"),
    ("pr", "branch", "what it is doing", "last update"),
    ("date", "commit", "what shipped", "gate"),
    # ── P65's §0.2 through-line table (added 2026-09-09, read before adding) ──
    # Three columns of NARRATIVE ABOUT rows that had ALREADY closed: cell 1 names
    # the row, cells 2-3 say what its instrument reached and what still shipped.
    # No cell declares a deferral and none carries a status to read -- and the
    # anchors it names are counted in the registries, so counting them here would
    # DOUBLE-count them, which is the opposite of the miss this gate guards.
    # ⚠ It reached HEAD red because the handoff is written at the END of a cycle,
    # AFTER the balance gate has run: a gate is only as good as the last tree it
    # saw. The cycle order now runs this gate again after the handoff is rewritten.
    ("row that read closed", "what it had actually converted", "what still shipped"),
}


def strip_decoration(text):
    """Drop markdown emphasis/strikethrough/code ticks and collapse whitespace.

    ⚠ Does NOT lowercase. Row IDENTITY must preserve case -- lowercasing it once
    silently mapped `D-B` to `d-b`, which the self-test caught before it shipped.
    Header MATCHING lowercases separately, in norm_cell().
    """
    return " ".join(text.replace("*", "").replace("`", "").replace("~", "").split())


def norm_cell(text):
    """Strip markdown decoration, collapse whitespace, fold case: for header matching."""
    return strip_decoration(text).lower()


# The words a cell may LEAD with to state a verdict. `DEFERRED` is a third spelling of GATED
# that the 2026-06-era rows use freely, and a reader treats it as one. `DISCLOSED` is the
# door's status cell for OPEN work whose debt pre-dates the cycle (`🔵 DISCLOSED`, written by
# `dssharness write-anchor --status disclosed`): without it here that cell stated NO verdict,
# so ARM 7 could not see a disclosed row whose prose says GATED. ✔MEASURED 2026-09-23 on
# main: 24 such rows, each read as None before this entry and OPEN after it.
_VERDICT_WORDS = {"CLOSED": "CLOSED", "OPEN": "OPEN", "GATED": "GATED", "DEFERRED": "GATED",
                  "DISCLOSED": "OPEN"}


def lead_verdict_word(text):
    """-> the verdict a cell states in its FIRST word, or None.

    ★★★ THE PREDICATE MUST TEST THE WORD, NOT THE GLYPH, and the gap between the two is
    the whole reason ARM 7 exists as a separate arm. ✔MEASURED 2026-09-03 over the two
    working registries: **304 of 490 rows** lead with a glyph differing from their status
    cell's, and almost all of them mislead nobody -- `⏳ **OPEN (...)**` against a
    `🟠 OPEN` column agrees in WORDS, and ~60 rows open with `🟢` that is P42 audit
    BANDING rather than a shipped-work claim. The WORD-level figure over the same
    population was **65**, and every one of those was a claim a reader would act on. A
    guard built on the glyph would refuse 304 rows on day one and be softened within a
    cycle; one built on the word refuses only what is actually wrong.

    ⚠⚠ AND IT READS ONLY THE FIRST WORD, WHICH IS THE THIRD NARROWING OF THIS PREDICATE
    -- each earlier one produced a FALSE CONTRADICTION against a row that was correct:
      1. scanning the first four words matched `(a) CLOSED 2026-07-30 ... ROW STAYS OPEN
         as the CLASS index`;
      2. requiring a capitalised run of length >= 3 still matched `BUG FIXED ... CLASS
         OPEN ON LIMBS (B)+(C)` and `PARTIALLY CLOSED ... THE DERIVATION HALF REMAINS
         OPEN`.
    Both of those rows state honestly that PART of their subject shipped while the row
    stays open, and "fixing" either would have destroyed the record. A qualifier before
    the verdict (`BUG`, `PARTIALLY`, `LIMB (A)`) changes what the verdict is ABOUT, and
    no scanner can enumerate which qualifiers do that. ⇒ Reading only the first word
    cannot err in that direction: it under-counts a row that says `STILL OPEN`, and never
    fabricates a contradiction against an author who was being precise. Under-counting is
    the safe direction for a ratchet; a false accusation is what gets a guard turned off.
    """
    m = re.search(r"[A-Za-z]+", strip_decoration(text))
    return _VERDICT_WORDS.get(m.group(0).upper()) if m else None


# ⚠⚠ AN ESCAPED PIPE IS NOT A CELL SEPARATOR, AND READING IT AS ONE SHIFTED EVERY
# CELL AFTER IT. `\|` is the house spelling for a literal pipe inside a cell -- the
# registry guard's PURPOSE line is literally "refuse a markdown table row whose
# unescaped pipes would silently drop cells" -- and this splitter was a bare
# `line.split("|")`, which treats the escape as a separator.
# ✔MEASURED 2026-08-23 at 6dc63be0 over the 2,233 anchor-bearing table rows in
# `.plans/`: **161 carry an escaped pipe**; for every one of them the
# CLOSING-WORK cell was a FRAGMENT of the real cell, and for 131 the STATUS cell
# was truncated at the escape. The ANCHOR cell moved in **zero** rows, which is
# why the OPEN population (706) and the mis-marked population (0) are byte-identical
# before and after -- the defect was silent, not visible, and that is the point.
# ★★ ITS ONE LIVE CONSEQUENCE WAS A BLIND SPOT, NOT A WRONG NUMBER, AND IT WAS
# MEASURED: `D-PP-HAS-EXTENSION-BUILTIN-ABSENT` and the (since retired) WSL2 clock row
# both led their CLOSING-WORK cell with a gate marker, and both were invisible to
# `is_gated` because the cell this splitter handed it was `0.1 s\` and `\`
# respectively.
# ★ The escape is UNDONE in the returned text: a caller matching on cell content
# should see the pipe the author wrote, not its markdown spelling.
_UNESCAPED_PIPE = re.compile(r"(?<!\\)\|")


def split_row(line):
    """Cells of a markdown table row, 1-based (index 0 is the empty pre-pipe lead).

    Splits on UNESCAPED pipes only; `\\|` is a literal pipe and is un-escaped in
    the result. See the note above for the 161-row measurement behind that.
    """
    return [c.replace("\\|", "|") for c in _UNESCAPED_PIPE.split(line)]


def is_closed(cell):
    """A status cell is CLOSED iff it OPENS with the closure mark.

    Everything else -- every other glyph, every novel glyph, no glyph at all -- is
    OPEN.  The complement is defined, never the variants, so a marker nobody has
    thought of yet counts as open, which is the safe direction.

    star **THE LEADING-POSITION TEST IS THE FIX FOR A MEASURED UNDERCOUNT, 2026-08-12.**
    This previously asked `CLOSED_MARK not in status` -- the mark ANYWHERE in the cell.
    Open rows legitimately use the check mark mid-prose to flag a DONE HALF or a piece
    of good news, e.g. a row about a release arm not compiled when its baseline did not run
    opened `orange **OPEN -- normal ...` and later said `... the arms themselves are GOOD`
    with a check mark.  The substring test read that as CLOSED.  MEASURED at the time of
    the fix by running BOTH rules over the registry at HEAD: the substring rule reported
    577 open, the leading-position rule reports 642 -- EXACTLY 65 rows hidden, 11.3%.
    star CORRECTED 2026-08-12, hours after this docstring was first written, by an
    independent audit.  It originally read "49 rows ... of which ~39 were genuinely open
    ... ~7%".  Those came from a scratch regex that only matched rows whose cell HEAD
    carried an EXPLICIT open glyph -- so rows with no marker at all plus a mid-prose check
    mark were invisible to the very scan that was measuring the blind spot.  The registry
    row that recorded this undercount carried the right numbers
    and this docstring did not, which is the worse way round: this is the surface a future
    maintainer reads FIRST.  Measure with the tool, never with a scratch grep that may
    share the bug you are hunting.
    star star AND NOTE THE DIRECTION, BECAUSE IT IS NOW THE SAME DIRECTION THREE TIMES.
    Version 1 of this gate enumerated the OPEN glyphs and was blind to the hourglass,
    reporting 269 open where there were 579.  That was fixed by inverting to "open unless
    closed" -- and the inversion introduced the mid-prose blind spot (577 vs 642).  Fixing
    THAT left the denominator itself too narrow: registry-only, blind to every plan-side
    home (2026-08-13).  All three errors
    flattered the cycle by hiding open work.  An instrument that keeps failing toward
    "you are doing fine" is the one to distrust; when you change this rule, ask which
    direction the new version errs in and pin it in self_test().

    ⚠ The leading-strip set is `*_ ` and is deliberately UNCHANGED from the 2026-08-12
    version, so a count taken with it then and now agrees byte for byte. ✔MEASURED
    2026-08-13: no status cell in ANY recognized table begins with a
    strikethrough, so widening the strip set would buy nothing and would only risk
    turning an open row closed.
    """
    return cell.lstrip().lstrip("*_ ").startswith(CLOSED_MARK)


def is_disclosed(cell):
    """A status cell DISCLOSES pre-existing debt iff it opens with the disclosed mark.

    Same leading-position test as is_closed, and for the same reason: a mark anywhere
    in the prose would let a row that merely MENTIONS disclosure claim the exemption.
    """
    return cell.lstrip().lstrip("*_ ").startswith(DISCLOSED_MARK)


# ── ARM 2: A STATUS CELL WHOSE OWN OPENING VERDICT CONTRADICTS ITS MARKER ──────
# The row that asked for this arm asked for a gate rather than a sweep, and the
# argument is recurrence: the identical defect -- a closure mark in the wrong cell --
# was swept on 2026-08-19 and was back eleven days later. A sweep fixes the rows that exist; a gate fixes
# the class.
#
# THE SHAPE IS DECIDABLE: the cell's FIRST WORD is a closure verdict while its
# leading marker is not the closure mark. Nothing else is inferred -- in
# particular this never claims the row's WORK is done, only that the row
# contradicts itself, which is repairable either way (mark it closed, or reword
# the opening so it stops claiming closure).
CLOSURE_VERDICTS = frozenset((
    "CLOSED", "FIXED", "DONE", "RESOLVED", "REFUTED", "COMPLETE", "COMPLETED",
    "SUPERSEDED", "WITHDRAWN", "OBSOLETE", "LANDED", "SHIPPED", "DISCHARGED",
    "RETIRED",
))
# ⚠ THE WALK-BACK LIST IS AN ENUMERATION, AND UNLIKE THE OPEN-GLYPH ENUMERATIONS
# THIS FILE'S HISTORY WARNS ABOUT, IT ERRS IN THE SAFE DIRECTION -- which is why
# it is allowed to exist and why it should stay GENEROUS. A word missing from it
# makes the arm ACCUSE a row that walks its own closure back; a word wrongly in it
# makes the arm stay SILENT and the row keeps counting OPEN. Over-reporting the
# OPEN population is this gate's safe direction everywhere else and it is the safe
# direction here too, so when in doubt ADD the word.
# ✔MEASURED 2026-08-23 at cf27fe8b: `residue` alone (the wording the registry row
# proposed) leaves `D-PP-LINE-DIRECTIVE` accused, whose own cell says the residual
# lives in a NAMED SUCCESSOR row -- so `RESIDUAL` was added.
WALK_BACK = (
    "PARTIAL", "STAYS OPEN", "STAY OPEN", "REMAINS OPEN", "STILL OPEN",
    "NOT CLOSED", "NOT YET", "RESIDUE", "RESIDUAL", "HALF", "REOPENED",
    "NOT DISCHARGED", "SCOPED TO", "ONLY THE",
)
# ★★ WHERE THE WALK-BACK IS SEARCHED IS AS LOAD-BEARING AS THE WORD LIST, AND IT
# WAS SIZED BY MEASUREMENT AFTER THE OBVIOUS IMPLEMENTATION MEASURED NEARLY
# VACUOUS. ✔MEASURED 2026-08-23 at cf27fe8b over 2,130 status-bearing rows: the
# mean status cell is 1,881 characters and the longest is 39,753, so ANY of these
# words turns up somewhere in a long row by accident -- searching the whole row
# exonerated 9 of 14 candidates, five of them provably complete rows whose only
# hit was `ONLY THE`, `HALF` or `RESIDUAL` buried in a historical recap.
#   window        accused
#   0 / 120         9
#   200             8      <- chosen
#   500             7      (loses D-PP-LINE-DIRECTIVE, whose residual is carried by
#                           a NAMED SUCCESSOR row, so it has nothing left itself)
#   whole cell      5      (loses D-MIR-COMPILERBARRIER-DCE-REFUTED to `ONLY THE`
#                           and D-CSUBSET-C11-THREADS-HEADER to `RESIDUAL`)
# ⇒ A ROW'S RETRACTION LIVES WHERE ITS VERDICT LIVES: in the opening of the status
# cell, or in the CLOSING-WORK cell, which is by definition the statement of what
# remains and is short enough to search whole. The cross-refs cell and the
# historical recap at the tail of a status cell are neither.
VERDICT_WINDOW = 200
_LEAD_NON_ALPHA = re.compile(r"^[^A-Za-z]+")
_FIRST_WORD = re.compile(r"[A-Za-z]+")


def opening_verdict(cell):
    """The first WORD of a status cell, glyphs and markdown decoration removed.

    star A HYPHENATED COMPOUND IS NOT A VERDICT, AND THIS IS THE SAME BOUNDARY
    LESSON `check-anchor-registry`'s leading `\\<` records, pointing the other way.
    ✔MEASURED 2026-08-23 at cf27fe8b: plan-00 section 0.2's `D12` opens its status
    cell with *"Shipped-lib FFI = Model 3 ..."* -- `Shipped` there is the head of a
    compound NOUN, and reading it as the verdict `SHIPPED` accused a row that is
    explicitly *"NOT a deferral -- a section B architectural decision"*. A guard's
    first false accusation is the one that gets it turned off, so the compound is
    refused rather than trimmed.
    """
    text = _LEAD_NON_ALPHA.sub("", strip_decoration(cell))
    m = _FIRST_WORD.match(text)
    if not m:
        return ""
    tail = text[m.end():]
    if tail[:1] == "-" and tail[1:2].isalpha():
        return ""
    return m.group(0).upper()


def is_mismarked_closure(status, closing_work):
    """The row's opening verdict is a closure while its leading marker is not.

    star THE WALK-BACK TEST READS THE CLOSING-WORK CELL TOO, NOT ONLY THE STATUS
    CELL, AND THAT IS A MEASURED CORRECTION rather than caution. ✔MEASURED
    2026-08-23 at cf27fe8b: `D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME` opens
    `SHIPPED 2026-08-05` and its CLOSING-WORK cell then says *"NOT YET DISCHARGED
    ... this row stays open until that confirmation lands"*. A status-cell-only
    test accuses it; reading the closing-work cell correctly leaves it alone. A row
    is one claim spread over its cells, and the cell that says what REMAINS is
    exactly where a retraction belongs.
    star AND THE STATUS SIDE IS WINDOWED -- see VERDICT_WINDOW for why reading the
    whole status cell instead measured nearly vacuous.
    """
    if is_closed(status):
        return False
    if opening_verdict(status) not in CLOSURE_VERDICTS:
        return False
    hay = (strip_decoration(status)[:VERDICT_WINDOW] + " "
           + strip_decoration(closing_work)).upper()
    return not any(w in hay for w in WALK_BACK)


# ── ARM 3: A GATED ROW MUST NAME WHAT OPENS IT ─────────────────────────────────
# ⓘ This header read "MUST NAME THE ROW" until the 2026-08-24 ruling widened the
# opener to a typed reference into EITHER namespace -- an anchor row or a plan phase.
# The declaration half below is unchanged; the resolution half now lives beside
# PLAN_PHASE_REF, where the widening and its one asymmetry are argued.
# Operator ruling 2026-08-23: *"A ⛔ MUST-NOT-BUILD ROW MUST NAME THE ROW THAT
# OPENS IT. A gate whose opener is an unowned event is unfalsifiable and will sit
# forever; a gate whose opener is another ROW is a dependency, which is
# schedulable, sizable, and visible in the queue. A ⛔ pointing at nothing is a
# load error for the registry, the same way a precondition naming an unknown
# symbol is a load error for a descriptor."*
#
# star★ THE PREDICATE IS THE **DECLARATION**, NOT THE ⛔ GLYPH, AND THE CENSUS IS
# WHY. ✔MEASURED 2026-08-23 over 2,130 status-bearing rows at cf27fe8b: ⛔ leads
# exactly SIX status cells and only ONE of the six is a must-not-build row -- the
# other five spell "REFUTED-DESIGN", "NEGATIVE RESULT" and "SUPERSEDED", i.e. they
# use ⛔ to mean *do not re-propose this*, which has no opener and never will.
# Keying on the glyph would demand an opener from five rows that cannot have one.
# The rows the ruling is ABOUT declare themselves in words -- `TRIGGER-GATED`,
# `MUST-NOT-BUILD`, `trigger-gated`, `TRIGGER-NOT-FIRED` -- and there are 62 open
# ones, which is also why this arm is a DIFFERENTIAL and not a day-one refusal.
#
# ⚠⚠ THE SEPARATOR CLASS IS PART OF THE DECLARATION, AND OMITTING THE HOUSE COLON
# HID NINETEEN GENUINE GATES: the declaration could not spell the house colon.
# The original class was `[\s-]*`, so `TRIGGER-NOT-FIRED` matched but `Trigger: NOT
# FIRED` -- which is how the registry actually writes a trigger field, `Trigger: <x>`
# -- did not. ✔MEASURED 2026-08-23 over the worktree's 944 OPEN status-bearing rows:
# admitting `:` and the em-dash, plus the `has`/`yet` forms the same field uses, moves
# the gated population 72 -> 91, and all 19 added rows were READ. Every one declares a
# real unfired trigger in the registry's own field syntax -- e.g.
# `D-CSUBSET-ATTRIBUTE-BEFORE-EXTERN-KEYWORD` (*"Trigger: NOT FIRED, and do not pull it
# forward"*) and `D-DIAG-CODE-RANGE-0X5XXX-DOUBLE-ALLOCATED` (*"Trigger has NOT fired as a
# live bug"*).
# ★ THE DIRECTION IS WHAT MAKES IT A DEFECT RATHER THAN A REFINEMENT: it under-reported,
# which is the direction that flatters the cycle, and it is the fifth time this
# instrument has erred that way (see the module docstring's three, plus the escape-aware
# splitter above).
GATED_DECL = re.compile(
    r"MUST[\s:\u2014-]*NOT[\s:\u2014-]*BUILD"
    r"|TRIGGER[\s:\u2014-]*GATED"
    r"|TRIGGER[\s:\u2014-]*(?:HAS[\s:\u2014-]*)?NOT[\s:\u2014-]*(?:YET[\s:\u2014-]*)?FIRED",
    re.IGNORECASE)
# ★★★ A **NEGATED** DECLARATION IS NOT A DECLARATION, AND THE NOTE THAT USED TO SIT
# BELOW THIS SAID THE OPPOSITE, so `is_gated` accused a row that declares no gate.
# The old note claimed ✔MEASURED that no row under `.plans/` writes a negated form. That
# measurement was about the FIRED test ("not already fired", "never fired") and it still
# holds there; the negation of the **declaration** was never considered, and a row
# reading *"no longer merely trigger-gated"* was accused on the strength of the words
# inside its own denial. ⇒ CORRECTED HERE rather than left to be inherited.
# ⓘ ✔MEASURED 2026-08-23 in the worktree: this arm changes the accused population by
# **0** rows TODAY, because the one live instance (`D-FFI-OFFSETOF-MACRO`) was reworded
# out during the P29 registry sweep -- which is itself the outcome the FAIL text below
# tells authors NOT to take. The shape is real, it recurred once, and it is pinned
# synthetically so it cannot come back unseen.
# ⚠ The window looks only BEHIND the hit, so `TRIGGER NOT FIRED` -- a declaration that
# CONTAINS a negator -- is unaffected: its `NOT` is part of the matched text, not before it.
_DECL_NEGATOR = re.compile(
    r"(?:NO\s+LONGER|NOT\s+MERELY|NOT\s+JUST|NEVER|IS\s+NOT|WAS\s+NOT|ARE\s+NOT"
    r"|WERE\s+NOT|STOPPED\s+BEING|CEASED\s+TO\s+BE)[\s\w,]{0,24}$", re.IGNORECASE)


def declares_gate(flat):
    """A NON-NEGATED gate declaration appears in `flat` (already decoration-stripped)."""
    for m in GATED_DECL.finditer(flat):
        if not _DECL_NEGATOR.search(flat[max(0, m.start() - 34):m.start()]):
            return True
    return False
# ★★★ A ROW WHOSE TRIGGER HAS **ALREADY FIRED** IS NOT GATED, WHATEVER ELSE IT
# SAYS, AND THIS IS DEFINITIONAL RATHER THAN AN ESCAPE HATCH: "gated" means
# waiting on an event that has not happened. Once the event happens the row is
# actionable, not blocked, and demanding an opener from it asks which row will
# cause something that already occurred.
#
# ★★ IT IS ALSO THE FIX FOR A DEFECT THIS ARM SHIPPED WITH, AND THE DEFECT IS THE
# THIRD INSTANCE OF ONE SHAPE IN A SINGLE CYCLE: **a description of a class being
# classified as a member of it.** The first two were in this very file -- a FAIL
# help string that acquired a three-segment anchor placeholder, and a docstring
# that spelled out one of the eleven renamed fixtures in order to EXPLAIN the
# rename. The third was the registry row recording this arm's own 59-row census:
# its prose necessarily QUOTES `TRIGGER-GATED / MUST-NOT-BUILD / TRIGGER-NOT-FIRED`
# while its own verdict says `Trigger: ALREADY FIRED`. ⇒ A ROW MUST BE ABLE TO NAME
# THE THING IT IS ABOUT; a guard that forbids that is the guard being wrong, so the
# repair is here and not in the row.
#
# ⚠⚠ AND THE OBVIOUS ALTERNATIVE -- WINDOWING THE DECLARATION THE WAY
# `VERDICT_WINDOW` WINDOWS THE CLOSURE TEST -- WAS MEASURED AND IS WRONG HERE, FOR
# A STRUCTURAL REASON WORTH KEEPING. ✔MEASURED 2026-08-23 at cf27fe8b: windowing
# drops the gated population from 63 to **37**, and the 26 lost are overwhelmingly
# GENUINE gates -- `D-FFI-STDINT-PTR-WIDTH-ILP32`, `D-FFI-WCHAR-WIDTH`,
# `D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC`, the four `D-CSUBSET-COMPLEX-*` rows,
# and twenty more. The reason is that in the REGISTRY shape this cell is the
# header's **Trigger** column, and a trigger DESCRIPTION legitimately occupies the
# whole cell, whereas a closure VERDICT is by convention the first thing in it.
# ⇒ SAME CELL, TWO CONVENTIONS: the window is right for the closure test and wrong
# for this one. By contrast the fired test costs exactly **2 rows** (63 -> 61 in the
# worktree, 63 -> 62 at the base ref) and both are correct exclusions.
# ⓘ ✔MEASURED: no row anywhere under `.plans/` writes a NEGATED form of THIS phrase
# ("not already fired", "never fired"), so the match has no known false-exclusion path
# today. ⚠ THAT SENTENCE USED TO BE WRITTEN ABOUT NEGATION IN GENERAL AND WAS FALSE AS
# SUCH: the negation of the DECLARATION -- *"no longer merely trigger-gated"* -- does
# occur, and reading it as a declaration is a live defect. It is handled at
# `_DECL_NEGATOR` above; this note is now scoped to the phrase it was actually measured
# over, which is what it should always have said.
# star Like the disclosed mark, this is a CHECKABLE CLAIM rather than a formatting
# choice: writing "already fired" when it has not is a false statement about the
# world, and a reader can look. The direction of error is also the safe one -- this
# can only ever SHRINK the accused set, and an escaped gate joins the reported DEBT
# population, whereas a false accusation is what gets a guard turned off.
#
# ★★★ AND IT RECOGNIZED **ONE SPELLING OUT OF SIX**. The phrase above
# was `ALREADY FIRED` and nothing else, but the registry's own trigger FIELD spells the
# same claim without the adverb. The un-adverbed spelling is the registry's COMMONEST
# and this pattern matched none of it, which is the finding; the arithmetic below is
# only how it was noticed.
# ⚠ THE ORIGINAL SPLIT PUBLISHED HERE DOES NOT REPRODUCE AND IS CORRECTED IN PLACE,
# because a wrong number in the file that explains a fix is how the next reader
# re-derives the wrong lesson. It read `Trigger: FIRED` **317** / `Trigger fired` 56 /
# `the trigger fired` 15 / `Trigger HAS fired` 8 / `TRIGGER-FIRED` 1 against
# `ALREADY FIRED` **84**. ✔RE-MEASURED 2026-08-24 over the guard's OWN harvested
# population (`scan_worktree`, 2,299 recognized deferral-table rows, open and closed,
# case-insensitive): **105 / 73 / 16 / 10 / 3** against **77**. ★ The mechanism of the
# error is worth more than the digits: **317 was a GREEDY `Trigger:.*FIRED` count** --
# a real total for "some fired-spelling appears after a `Trigger:` label", silently
# attributed to the FIRST bucket alone, so the split over-counted its lead term ~3x
# while the sum stayed honest. A total that is right makes a wrong split look checked.
# ★ THE CONSEQUENCE WAS A FALSE ACCUSATION, NOT A COUNT: three OPEN rows state their own
# trigger fired and were still asked to name an opener, and one of them is exactly the
# exclusion the P29 audit was arguing about -- the WSL2 clock row (since retired),
# whose closing cell ended *"Trigger: FIRED (measured). Priority: HIGH."* while leading
# with a ⛔ that means *do not fix this in the compiler*, not *do not build this yet*.
# ⚠⚠ THE NEGATED FORMS ARE THE WHOLE DIFFICULTY AND THEY ARE STRUCTURAL, NOT LISTED:
# `NOT` / `NOT YET` / `HAS NOT` may not appear between the noun and `FIRED`, so the
# pattern admits only the words that can legitimately sit there. ✔MEASURED: **40** rows
# carry a not-fired spelling and NONE is exonerated -- including
# `D-LSP-DIAGNOSTIC-RENDERED-AGAINST-THE-OPEN-DOCUMENT-IGNORING-ITS-BUFFER`
# (*"TRIGGER-GATED, TRIGGER NOT FIRED"*), `D-AS4-ARM64-INDEXED-LEA-SCALE` and
# `D-FF1-MACHO-SECT-KIND` (*"STILL GATED (trigger NOT fired)"*), all three of which
# stay gated.
TRIGGER_FIRED = re.compile(
    r"TRIGGER(?:'S)?[\s:\u2014,-]*(?:HAS|HAVE|HAD)?[\s:\u2014,-]*(?:ALREADY)?"
    r"[\s:\u2014,-]*FIRED"
    r"|ALREADY[\s-]*FIRED", re.IGNORECASE)
# ★★★ A ROW THAT DECLARES IT HAS **NO TRIGGER** IS NOT GATED, AND IT IS THE ESCAPE THE
# FIRED TEST CANNOT PROVIDE; without it `is_gated` accused a row that declares no gate.
# The FAIL text below tells a row that is *about* gated rows to state its own verdict
# instead of rewording, and offers exactly one: `ALREADY FIRED`. ⚠ **A ROW WITH NO
# TRIGGER CANNOT HONESTLY CLAIM ITS TRIGGER FIRED** -- so for a pointer row, a rule, or
# a census the only sanctioned escape was a false statement, and the cheapest way out
# was to reword, which is what the same paragraph forbids.
# The row that motivated it is `D-LK11-FAMILY-IS-TRACKED-IN-PLAN-14-NOT-HERE`, a signpost
# whose status cell reads *"Trigger: none — informational, permanent"* and whose closing
# cell reads *"Nothing to do."*, accused solely for quoting the status of the two rows it
# points at (*"both OPEN, trigger-gated"*). Same shape as the census case pinned below: a
# description of a class classified as a member of it.
# ⚠ THIS COMMENT SAID "exactly **one** live row takes it" AND THAT WAS ALREADY WRONG WHEN
# IT WAS WRITTEN. ✔RE-MEASURED 2026-08-24, and STATE THE PREDICATE WITH THE NUMBER,
# because two independent measurements got 9 and 2 and BOTH ARE CORRECT — they count
# different things, which is exactly how a bare integer in a comment misleads:
#   · **9** rows carry an own-verdict no-trigger declaration at all (open AND closed);
#   · **2** live OPEN rows take the escape LOAD-BEARINGLY, i.e. would otherwise be
#     gated and asked for an opener — the OPT7 inline-legality gate row (since retired)
#     and `D-LK11-FAMILY-IS-TRACKED-IN-PLAN-14-NOT-HERE`.
# ⇒ re-derive whichever you mean rather than quoting either: a figure in a comment is a
# measurement with no instrument attached, and a figure whose PREDICATE is unstated is
# not even wrong.
NO_TRIGGER = re.compile(r"TRIGGER:?\s*(?:IS\s+)?NONE", re.IGNORECASE)
# ⚠⚠⚠ AND THE EXONERATION ABOVE HAD THE EXACT DEFECT IT WAS WRITTEN TO CURE.
# ✔MEASURED 2026-08-24 (cycle P29, independent step-10 audit): the test was a bare
# `NO_TRIGGER.search(flat)` over the WHOLE row, so an ATTRIBUTIVE mention of some
# OTHER row's "trigger: none" exonerated THIS one -- and A/B measurement showed it
# flipped all four genuine gate shapes to not-gated. The paragraph above congratulates
# itself on catching "a description of a class classified as a member of it", and then
# the regex it introduces reads a description of ANOTHER ROW'S state as this row's own
# verdict. ⇒ the no-trigger escape read ANOTHER row's verdict as the row's own.
# ★ THE DISCRIMINATOR IS WHOSE VERDICT IT IS, and it is structural rather than a word
# list: an attributive mention NAMES the row it is about, so if an anchor id sits between
# the nearest clause boundary and the phrase, the phrase is talking about SOMEBODY ELSE.
# A row's own verdict says "Trigger: none", never "[[D-OTHER]], whose trigger is none".
_CLAUSE_BREAK = re.compile(r"[.;—]|\*\*")
_ANCHOR_TOKEN = re.compile(r"D-[A-Z0-9]+(?:-[A-Z0-9]+)+")


def declares_no_trigger(flat):
    """True only when the row declares that ITS OWN trigger is none.

    ⛔ DO NOT collapse this back to `NO_TRIGGER.search(...)`. The bare search is
    what let one row's quotation of another row's state silence this guard, and a
    row silenced here is reported NOWHERE -- not gated, not DEBT -- which is the
    one outcome a fail-loud instrument must never produce.
    """
    for m in NO_TRIGGER.finditer(flat):
        head = flat[:m.start()]
        breaks = list(_CLAUSE_BREAK.finditer(head))
        clause = head[breaks[-1].end():] if breaks else head
        if _ANCHOR_TOKEN.search(clause):
            continue                       # attributive: it is about another row
        return True
    return False
# ⓘ The opener SPELLING reuses the registry's existing `[[anchor-name]]` link
# form -- deliberately NOT a new syntax, because one already resolves. What is
# added is a required LABEL in front of it, because a bare `[[...]]` is satisfied
# by any cross-reference in the row and this repository has already been bitten
# twice by a detector that a mere MENTION can satisfy.
# ⚠ A wrapped id is invisible to this regex, as it is to every grep -- but a
# markdown table row is ONE LINE by construction, so an id inside a registry row
# cannot be wrapped in the first place. The residual hazard is a MISTYPED or
# TRUNCATED id, and that is caught by the resolution half rather than the syntax
# half: an opener that names no row is refused exactly like an absent one.
OPENER_REF = re.compile(r"OPEN(?:ED|S)?[\s-]*BY[\s:]*\[\[([^\]|]+)\]\]",
                        re.IGNORECASE)


# ── ARM 8: A ROW'S `[[...]]` POINTER MUST NAME A ROW ─────────────────────────
# `[[<anchor id>]]` is the registry's link form -- "go and read that row". The door
# refuses a cell that NEWLY cites an id no row holds (`dssharness write-anchor` /
# `set-anchor`, exit 13), but it judges only the cell it is writing: a row DELETED or
# RENAMED under a pointer that resolved leaves the pointer dangling, and no verb reads a
# registry cell again (`check-anchor-citations` reads `anchors.citationRoots`, which
# holds no `.plans`).
# ✔MEASURED 2026-10-06 (cycle P69, lane `hm`) by this arm's own definition, the same
# on HEAD 71648598 and on the main tree: 317 pointers -- distinct (row, target) pairs --
# in 188 rows name no row. 316 sit in 187 rows of the done registry: 284 name a row cycle
# P67 deleted with the harness registry (288a6be0), 5 a plan-step id, 27 an id no
# registry ever held. 1 sits in the pending registry.
# So the arm is a DIFFERENTIAL like the others: a pointer that dangled at the base too is
# DEBT, printed on every run; one that dangles only now -- in a row the change added or
# edited, or under a target the change deleted or renamed -- is REFUSED. Only an
# ANCHOR-SHAPED target is a pointer: `[[...]]` also links memory files and plan phases
# (ARM 3's second namespace, `plan-22 OPT8`), and neither names a row. A target resolves
# EXACTLY, against the rows of the two registries -- the rule the door and `read-anchor`
# apply -- so a plan-step id (an id, a dot, a step number) never resolves: no row id
# carries a dot.
# ⓘ THE ARCHIVE'S DEBT IS REPORTED, NEVER REPAIRED HERE: operator ruling 2026-09-16,
# *"-done.md is past anyway"* (recorded in `.plans/_handoff.md`). The pending
# registry's pointers are live work, so its debt is listed row by row.
# ★ A LINK IS JUDGED; A MENTION IS READ. `dssharness check-anchor-citations` reads every
# id-shaped word as a citation, and the door refuses a NEW one naming no row; but a
# stored cell may name, on purpose, something that is no row, and no grammar can tell
# which. ✔MEASURED 2026-10-06 with the tool's own verdict (a probe repository whose one
# root is the pending registry, DssHarness 0.6.7): 50 citations in 37 rows name no row,
# 42 distinct (row, id) pairs. One is a `[[...]]` pointer; the 41 mentions, read one by
# one: 36 name on purpose something that is no row -- a plan-12 step the row was
# migrated from (`formerly D-ML…` plus the step's number, 26 rows), a row's own earlier
# spelling (4), an id a row names AS retired (4), closed plan steps and a plan's label
# lineage (2) -- and 5 are stale references (a sixth sits in a row counted above), each
# now corrected at the end of its own cell. So this arm judges the registry's LINK
# form, the one that claims "a row is here", and leaves a mention to its reader. (This
# comment spells none of those ids: this file is a citation root, and the tool reads
# `D-` and two segments as an id.)
POINTER_REF = re.compile(r"\[\[([^\]|]+)\]\]")


# ── THE OPENER IS A **TYPED REFERENCE TO EITHER NAMESPACE** ───────────────────
# Operator ruling 2026-08-24, widening the 2026-08-23 one recorded above ARM 3:
# *"I wrote 'name the ROW.' I meant 'name something OWNED AND SCHEDULABLE.' A plan
# phase is both -- it has an owner and a position in a sequence, which is MORE
# schedulable than an anchor row, not less. Placeholder rows would be pure registry
# pollution: rows that are not defects, existing only to satisfy a reference."*
# ⇒ AN OPENER RESOLVES IN THE NAMESPACE ITS OWN SYNTAX NAMES: `D-...` in the
# registry, `plan NN <phase>` under `.plans/`. A reference that names neither
# namespace, or names one and does not resolve there, is refused exactly as before,
# so this is a WIDENING and can never hide a row the old rule accused (pinned).
#
# ★★★ WHAT THE PLANS ACTUALLY WRITE IS NOT WHAT THE RULING SPELLS, AND THE PLANS WIN.
# The ruling's `plan-NN §X` is INTENT; the plans' own heading and table syntax is the
# FACT, and ✔MEASURED 2026-08-24 over the three plans the openerless gated rows wait on
# they disagree:
#   * plan 16 schedules `CS1`..`CS9` in the FIRST COLUMN of its section-3 PR table
#     (`| CS1 | Crypto substrate: vendor + wrap BearSSL ... |`) and puts the phase in
#     a heading only parenthetically (`### 2.10 Cryptographic substrate (CS1)`);
#   * plan 22 schedules `OPT1`..`OPT10` (plus `OPT5+`) in the first column of TWO
#     tables -- its section-0.1 stepper overview and its section-3 PR breakdown;
#   * plan 27 declares NO alphanumeric phase ids at all -- its sequencing table
#     numbers phases `1`..`6`, and every other thing it owns is a SECTION.
# ⇒ a phase reference resolves against BOTH surfaces -- a numbered HEADING and a
# leading TABLE-CELL id -- because admitting only one of them would force a reword on
# rows whose trigger already names the other in words (`Trigger = Plan 16 CS1 lands`),
# and rewording a row to satisfy an instrument is the outcome ARM 3's own FAIL text
# tells authors not to take.
#
# ⚠⚠ AND THE ASYMMETRY IS REAL AND IS NOT PAPERED OVER: in the registry namespace this
# gate refuses an opener that has ALREADY CLOSED, and in the plan namespace it CANNOT,
# because there is no mechanical closure vocabulary to read. ✔MEASURED: plan 22's
# stepper writes phase status as `🟩 **c1+c2 DONE**` / `⏳ **planned (v1.x)**` -- no
# closure mark, so `is_closed` correctly says nothing about it -- and plan 16's PR
# table has no status column at all. Guessing which column is the status would be
# schema invention of exactly the kind the ruling refuses. So the plan half checks
# RESOLUTION ONLY, that limit is stated here rather than discovered later, and the
# consequence is recorded in the report: an opener naming a FINISHED plan phase passes.
# ⚠⚠ THE SEPARATOR BETWEEN THE NUMBER AND THE PHASE IS MANDATORY, AND MAKING IT
# OPTIONAL WAS A LIVE DEFECT IN THIS REGEX'S FIRST DRAFT -- caught by a CONTROL, not
# by a test written for it. With `[\s–—:]?` the reference `plan-16`, which
# names a whole plan and NO phase, backtracked into plan `1` phase `6` -- and plan 01
# really does have a section 6, so a reference that schedules nothing RESOLVED. That
# is the exact failure this arm exists to prevent, arrived at from the other side.
# ⇒ a phase reference must SEPARATE its two parts; `plan-16` alone is refused.
PLAN_PHASE_REF = re.compile(
    r"^\s*plan[\s.\-]*([0-9]+(?:\.[0-9]+)*)"
    r"(?:\s+|\s*[–—:]\s*|(?=§))"
    r"(?:§\s*)?([A-Za-z0-9][A-Za-z0-9.+_]*)\s*$", re.IGNORECASE)
_PLAN_FILE_NUMBER = re.compile(r"^([0-9]+(?:\.[0-9]+)*)-")
_PLAN_HEADING_NUMBER = re.compile(r"^#{1,6}\s+([0-9]+(?:\.[0-9]+)*)(?=[.\s])")
_PHASE_ID_CELL = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.+_-]{0,15}$")
_HAS_DIGIT = re.compile(r"[0-9]")


def canonical_plan_number(text):
    """`06` -> `6`, `08.5` -> `8.5`, `22` -> `22`; "" when `text` is not a number.

    Filenames PAD (`06-artifact-profile-plan`) and prose does not always
    (`plan 6 §2.3`), so both sides fold to one key instead of one being trusted to
    match the other's spelling. ⚠ The fold is on the plan NUMBER only -- a phase
    token is compared verbatim, because `2.10` and `2.1` are different sections and
    normalising them would merge two real phases into one.
    """
    parts = text.split(".")
    if not parts[0] or not all(p.isdigit() for p in parts):
        return ""
    return ".".join(str(int(p)) for p in parts)


def plan_number_of(relpath):
    """The plan number a `.plans/` filename declares, or "" for an unnumbered file.

    ⓘ Unnumbered files -- the registry itself, the handoff, `ZZ-final-goal` -- declare
    no phases and are simply absent from the index, which is why a reference to them
    cannot resolve. Two files may share a number (`.plans/` really does carry two
    `24-` plans); the index UNIONS them rather than letting the later one win.
    """
    m = _PLAN_FILE_NUMBER.match(os.path.basename(relpath))
    return canonical_plan_number(m.group(1)) if m else ""


def phase_tokens(text):
    """Every phase one plan document DECLARES: numbered headings + phase-table ids.

    ★ THE TABLE HALF NEEDS THE DIGIT TEST, AND THE PLANS THEMSELVES SUPPLY THE REASON.
    A leading table cell is a phase id only when it marks A POSITION IN A SEQUENCE --
    the operator's own criterion -- and ✔MEASURED, every phase id in the three plans
    above carries a digit (`CS1`, `OPT5`, `OPT5+`, `1`..`6`) while the header words
    sharing that column do not (`PR`, `Tier`, `#`, `Anchor`). Without the test those
    header words would resolve and `plan-22 PR` would satisfy this gate.
    ⚠ ONE REAL PHASE IS THEREFORE UNRESOLVABLE, AND IT IS RECORDED HERE RATHER THAN
    QUIETLY ADMITTED: plan 22's section-3 table lists `SimplifyCFG` as a PR. That plan
    states in its own footnote that the row is NOT a numbered step -- it is a recurring
    cleanup rather than a position in the OPT-N sequence, which is why it carries no
    integer -- so refusing it as an opener is the plan's verdict, not this file's.
    Widen only if a gated row ever genuinely needs to name it.
    ⓘ A plan whose TITLE leads with its own number (`# 27 — GUI Plan`) therefore also
    declares that number as a section. Harmless -- it is a real heading in the file --
    and left alone rather than special-cased, because a title-shape exception is the
    kind of enumeration this file's docstring says never to write.
    """
    tokens = set()
    for line in text.split("\n"):
        m = _PLAN_HEADING_NUMBER.match(line)
        if m:
            tokens.add(m.group(1).upper())
            continue
        if not line.lstrip().startswith("|"):
            continue
        cells = split_row(line)
        if len(cells) < 3:
            continue
        cell = strip_decoration(cells[1])
        if _PHASE_ID_CELL.match(cell) and _HAS_DIGIT.search(cell):
            tokens.add(cell.upper())
    return tokens


# ★★★ THE DECLARATION LIVES IN EITHER CELL, AND KEYING ON THE STATUS CELL ALONE
# WAS A MEASURED BLIND SPOT: `is_gated` was blind to the remedy cell.
# The operator's rule is about the SEMANTICS ("a ⛔ MUST-NOT-BUILD row must name the
# row that opens it"), and the house style writes the gate into the CLOSING-WORK
# cell -- `⏳ **TRIGGER: …**`, `⏳ trigger = a shipped __finally consumer`,
# `⛔ **DO NOT PATCH LOCALLY.**`. A phrase test over the status cell alone reports
# OK while rows the rule condemns sit unseen.
# ✔MEASURED 2026-08-23 across `.plans/`, on top of the escape-aware `split_row`
# above: the gated population moves **64 → 97**, **33 added and 0 dropped**, and
# EVERY added row was read before this landed. Four of them are exactly the shape
# the ruling is about and none was visible before: `D-PP-HAS-EXTENSION-BUILTIN-ABSENT`
# (⏳ PENDING A USER DECISION), `D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME`
# (⏳ NOT YET DISCHARGED, waiting on an operator confirmation),
# `D-TARGET-ARM64-W-CONSTRAINT-BINDS-A-CLASS-NO-C-VALUE-EVER-LIVES-IN`
# (⏳ BLOCKER: AN OPERATOR DECISION ONLY) and `D-TARGET-NO-CROSS-CLASS-MOVE-VERB`
# (⛔ DO NOT PATCH LOCALLY -- bring it as a §B).
# ⚠⚠ THOSE TWO ABSOLUTE FIGURES DO NOT REPRODUCE AND MUST NOT BE RE-QUOTED -- they
# were taken mid-cycle, BEFORE the P29 registry sweep landed `ALREADY FIRED` verdicts
# on ~29 rows, and a population figure is a property of the tree it was read on.
# ✔RE-MEASURED the same day after the sweep: **35 → 72, +37 added and 0 dropped.**
# The DIFFERENTIAL is what the claim rests on and it reproduced exactly: **0 dropped**,
# i.e. the widening only ever ADDS, so it cannot have hidden a row the old rule saw.
# ★ AND ONE OF THE FOUR IS WRONG ON RE-READING, which is why re-measuring beat
# re-quoting: `D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME`'s own cell ends *"Trigger:
# FIRED — it exists to fix the measured $INODE64 misbinding"*, so by this file's own
# definitional rule it is NOT gated. It is exonerated by TRIGGER_FIRED and reported by
# ARM 4 instead, because a ⏳ lead over a fired trigger is a row that needs re-verdicting,
# not one that needs an opener.
#
# ⚠⚠ AND THE GLYPH IS READ ONLY IN THE **CLOSING-WORK** CELL, NEVER IN THE STATUS
# CELL, WHICH IS THE OPPOSITE OF WHAT A NAIVE WIDENING WOULD DO. The measurement
# recorded above GATED_DECL still stands: ⛔ leads six STATUS cells and five of them
# mean *do not re-propose this* (REFUTED-DESIGN / NEGATIVE RESULT / SUPERSEDED),
# which have no opener and never will. In the CLOSING-WORK cell the same glyph means
# *do not build this yet*. SAME GLYPH, TWO CELLS, TWO MEANINGS -- and this is the
# second time this file has had to record that a cell's convention is per-cell
# (see the VERDICT_WINDOW note, which reaches the mirror-image conclusion).
#
# ★★★ AND AN ENUMERATION OF GLYPHS IS THE ONE THING THIS FILE'S DOCSTRING SAYS NEVER TO
# WRITE, and `GATE_LEAD_MARKS` was one: a silent glyph enumeration. The
# first version of this gate enumerated the OPEN glyphs and was blind to the hourglass;
# the rule learned from it is *define the complement, never enumerate the variants*, and
# a two-element tuple of gate glyphs breaks it in the same direction: the day somebody
# writes a third one, the row is silently classified NOT-GATED and never asked for an
# opener. ✔MEASURED 2026-08-23: **11 distinct glyphs** lead a closing-work cell on an
# OPEN row (★ ⚠ ✅ 🔴 ⛔ ⏳ 🟡 — ⏭ ⓘ §) and **20** lead a status cell, so the vocabulary
# is demonstrably wider than two and still growing -- `⏸` already leads
# `D-AS4-ARM64-INDEXED-LEA-SCALE`'s status cell meaning exactly *paused*.
#
# ★★ THE COMPLEMENT CANNOT BE DEFINED HERE -- SO THE **RESIDUAL IS MADE LOUD INSTEAD**,
# WHICH IS THE PATTERN `EXCLUDED_HEADERS` ALREADY USES AND SAYS WHY. "Gated" is a
# positive declaration; there is no "everything that is not X" to invert, because a
# closing-work cell may honestly lead with anything. So BOTH sides are enumerated and
# NAMED -- gate leads here, non-gate leads below, each one READ before it was listed --
# and a lead marker in NEITHER list is not silently treated as prose: it is RECORDED and
# REPORTED, exactly as an unrecognized table header is. The enumeration then cannot hide
# a glyph; it can only decide whether a KNOWN one is counted. Keep it that way.
# ⓘ ✔MEASURED 2026-08-23: with these two lists the residual over the whole worktree is
# **0**, so the arm is affordable at FATAL for a row this cycle touched (see ARM 5).
GATE_LEAD_MARKS = ("⏳", "⛔")     # hourglass, no-entry
# ── The measured non-gate leads. Each was READ, and each is a VERDICT or an emphasis
# mark rather than a deferral: ★ emphasis, ⚠ caution, ✅ a closed sub-item, 🔴/🟡 a
# priority, — an em-dash lead-in, ⏭ "skipped/next", ⓘ a note, § a section pointer.
# ⚠ `⏸` is deliberately ABSENT from both lists: it means *paused* and would plausibly be
# a gate, but ✔MEASURED it never leads a closing-work cell today, so listing it either
# way would be a guess. Leaving it unclassified is the point -- the first row that uses
# it there gets reported instead of silently classified.
NON_GATE_LEAD_MARKS = ("★", "⚠", "✅", "🔴", "🟡", "—", "⏭", "ⓘ", "§")
# A cell that opens with ordinary prose, a bullet, a quote or a parenthesis carries no
# lead marker at all and is none of this arm's business.
_NO_LEAD_MARKER = "(-\"'"


def lead_mark_class(closing_work):
    """-> "gate" / "none" / the UNCLASSIFIED marker itself.

    "none" means the cell opens with ordinary text or a listed non-gate mark. A
    returned marker means this instrument does not know what the author meant, and
    the caller MUST report it rather than assume.
    """
    text = strip_decoration(closing_work).lstrip("*_ ")
    if not text:
        return "none"
    if text.startswith(GATE_LEAD_MARKS):
        return "gate"
    if text.startswith(NON_GATE_LEAD_MARKS):
        return "none"
    ch = text[0]
    if ch.isalnum() or ch in _NO_LEAD_MARKER:
        return "none"
    return ch
# ★★★ A ROW WITH NO CLOSING WORK IS NOT A GATE, AND THIS IS DEFINITIONAL LIKE
# `ALREADY FIRED` RATHER THAN AN ESCAPE HATCH: a gate DEFERS work, so a row that
# declares it has none is a record, not a deferral, and asking which row will open
# it asks which row will cause something that will never happen.
# ✔MEASURED 2026-08-23: seven OPEN rows declare it, in two spellings
# (`NO CLOSING WORK`, `Closing work: none`), and every one is a refutation, a
# negative result or a consequence-record -- e.g.
# a macOS Gatekeeper-admission row, since retired (*"THERE IS NOTHING TO FIX. The row
# exists to STOP the investigation from recurring"*) and
# `D-CONFIG-TOOLCHAIN-AXIS-REFUTED`. ⚠ Without this arm both would be accused, and a
# false accusation is what gets a guard turned off.
# ★ The direction of error is the safe one, and it was CHECKED rather than assumed:
# ✔MEASURED, this arm removes **nothing** from the population the old rule counted
# (33 added, 0 dropped) -- it only declines to add two rows the widening would
# otherwise have swept in. Like the fired-trigger test it is a CHECKABLE CLAIM: a row
# saying it has no closing work while carrying some is a false statement a reader can
# look at.
NO_CLOSING_WORK = re.compile(
    r"NO\s+CLOSING\s+WORK|CLOSING\s+WORK:?\s*(?:IS\s+)?NONE", re.IGNORECASE)


def is_gated(status, closing_work=""):
    """The row DECLARES ITSELF trigger-gated / must-not-build, and is still waiting.

    Reads BOTH cells, because a row is one claim spread over its cells and the
    house style puts the trigger in the closing-work one -- the same correction
    `is_mismarked_closure` already had to make for the walk-back test.

    FOUR EXONERATIONS, AND EVERY ONE IS THE ROW'S OWN CHECKABLE CLAIM rather than
    an escape hatch -- which is the property that lets them exist at all:
      * its trigger has FIRED          -> a fired gate is not a gate;
      * it has NO TRIGGER              -> nothing can open what nothing blocks, and a
                                          row with no trigger cannot honestly claim
                                          its trigger fired, so it needs its own words;
      * it has NO CLOSING WORK         -> a gate defers work;
      * the declaration is NEGATED     -> a denial is not a declaration.
    Only then does a non-negated declaration in either cell, or a gate marker leading
    the closing-work cell, make the row gated.
    """
    flat = strip_decoration(status) + " " + strip_decoration(closing_work)
    closing = strip_decoration(closing_work)
    if TRIGGER_FIRED.search(flat):
        return False
    if declares_no_trigger(flat):
        return False
    if NO_CLOSING_WORK.search(closing):
        return False
    if declares_gate(flat):
        return True
    return lead_mark_class(closing_work) == "gate"


def opener_of(whole_row):
    """The REFERENCE this row names as its opener, or "" when it names none.

    ⚠ Deliberately untyped here: the syntax inside the brackets is what names the
    namespace, and deciding that is `opener_state`'s job. Returning the raw text
    keeps the two halves separable -- and keeps a MISTYPED reference visible as a
    dangling opener instead of vanishing at the parse.
    """
    m = OPENER_REF.search(strip_decoration(whole_row))
    return m.group(1).strip() if m else ""


def opener_state(opener, names, open_names, phases=None):
    """-> "none" / "dangling" / "closed" / "open" for a gated row's named opener.

    ★ THREE OUTCOMES WHERE THERE USED TO BE TWO, and the new one is the point:
    resolving to a row is not the same as resolving to a LIVE dependency. `names`
    holds every data row open or closed; `open_names` holds the open ones. A name
    in the first and not the second is a row all of whose homes are closed --
    and a closed row cannot open anything.

    ★★★ AND THE REFERENCE IS TYPED: IT RESOLVES IN THE NAMESPACE IT NAMES (operator
    2026-08-24; see PLAN_PHASE_REF for the ruling and the measurement behind the
    spelling). `plan NN <phase>` resolves against `phases`, the phase index built
    from `.plans/`; anything else resolves against the registry population, exactly
    as before. Neither namespace forgives a dangling reference.

    ⚠ `phases` DEFAULTS TO EMPTY, AND THE DEFAULT ERRS IN THE SAFE DIRECTION ON
    PURPOSE: a caller that forgets to pass the index refuses every plan-phase opener
    loudly rather than accepting it silently. Do not "fix" that by defaulting to a
    permissive value -- the whole point of this arm is that an unresolvable opener
    is a load error, and a silent pass is the one outcome it must never produce.
    ⚠ The plan half returns "open" or "dangling" ONLY -- never "closed" -- because
    the plan namespace has no mechanical closure vocabulary to read. That limit is
    argued at PLAN_PHASE_REF; it is a known asymmetry, not an oversight.
    """
    if not opener:
        return "none"
    ref = opener.strip()
    m = PLAN_PHASE_REF.match(ref)
    if m:
        declared = (phases or {}).get(canonical_plan_number(m.group(1)), ())
        return "open" if m.group(2).upper() in declared else "dangling"
    if ref not in names:
        return "dangling"
    return "open" if ref in open_names else "closed"


def row_name(cell1):
    """Stable identity for a row: its anchor token, else its decoration-stripped cell.

    Decoration is stripped so that STRIKING a row through (`D1` -> `~~D1~~`, the house
    style for "landed") does not read as one row closing and a different row opening.

    ⓘ The fallback is not a defect to be regexed away. ANCHOR_TOKEN deliberately
    requires two segments, so a hypothetical `D-B` and plan-00's real `D7` both take
    the fallback -- and the fallback returns exactly `D-B` / `D7` anyway. Widening the
    token to admit one-segment names would buy nothing and would start matching prose.
    """
    m = ANCHOR_TOKEN.search(cell1)
    if m:
        return m.group(0)
    return strip_decoration(cell1)[:80] or "<blank>"


class Scan(object):
    """Everything one pass over a set of documents learned.

    ⓘ An OBJECT rather than a widening tuple, because three arms were added to
    this scanner in one change and `rows, findings, book, mismarked, gated, names`
    at four call sites is a positional-argument bug waiting to be written. The
    fields are deliberately plain containers -- callers merge them.

    rows       : {"relpath#name": (is_open, status_excerpt, shape_kind)}  OPEN ONLY
    findings   : [(relpath, line_no, severity, what)]
    names      : {"relpath#name"} for EVERY data row of a recognized table, open
                 or closed -- the population an opener reference resolves against.
    mismarked  : {"relpath#name": status_excerpt} contradicting their own verdict
    gated_rows : {"relpath#name": (opener_id, status_excerpt)}; opener_id is ""
                 when the row names none.
    unclassified: {"relpath#name": (marker, closing_excerpt)} whose CLOSING-WORK
                 cell leads with a marker in neither GATE_LEAD_MARKS nor
                 NON_GATE_LEAD_MARKS -- i.e. rows this instrument could not
                 classify. Recorded so the residual of an enumeration is never
                 silent; see ARM 5.
    unblocked  : {"relpath#name": reason} -- the row still PRESENTS as blocked but
                 the blocker it names has been discharged. See ARM 4.
    pointers   : {"relpath#name": {anchor id}} -- the anchor-shaped `[[...]]` targets a
                 REGISTRY row carries, in any cell. See ARM 8.
    pointer_home: {"relpath#name": relpath} -- the registry file that row was read from.
    plan_phases: {plan_number: {PHASE_TOKEN}} -- the SECOND namespace an opener may
                 resolve in. Populated per document from its numbered headings and
                 its phase-table ids; unnumbered files contribute nothing. Merged by
                 UNION, never by overwrite, because two files can share a number.
    """

    def __init__(self):
        self.rows = {}
        self.findings = []
        self.names = set()
        self.mismarked = {}
        self.gated_rows = {}
        self.unclassified = {}
        self.unblocked = {}
        self.split_gate = {}
        self.pointers = {}
        self.pointer_home = {}
        self.plan_phases = {}

    def merge(self, other):
        self.rows.update(other.rows)
        self.findings.extend(other.findings)
        self.names |= other.names
        self.mismarked.update(other.mismarked)
        self.gated_rows.update(other.gated_rows)
        self.unclassified.update(other.unclassified)
        self.unblocked.update(other.unblocked)
        self.split_gate.update(other.split_gate)
        for k, targets in other.pointers.items():
            self.pointers.setdefault(k, set()).update(targets)
        self.pointer_home.update(other.pointer_home)
        # ⚠ UNION, not `update`: `.plans/` really does carry two `24-` plans, and an
        # overwrite would silently delete one of them from the namespace an opener
        # resolves against -- a dangling verdict on a phase that exists.
        for number, tokens in other.plan_phases.items():
            self.plan_phases.setdefault(number, set()).update(tokens)
        return self


def row_key(relpath, name):
    """The identity of a row, for the purpose of "did this row open or close".

    ⚠⚠ EVERY REGISTRY FILE CANONICALISES TO ONE KEY PATH, AND THAT IS THE WHOLE POINT.
    The registry became TWO files on 2026-08-25 (production / tools-harness). A row's
    identity is its ANCHOR ID -- which file currently holds it is a filing decision, not a
    fact about the defect. Keying on the real path made the split read as
    `closed 692, opened 689`: every row deleted from the old file and added to a new one,
    a 1400-row phantom churn that would have buried any real movement completely.
    ★ It also means MOVING a row between the two registries is correctly a no-op here,
    which is what lets the buckets be corrected later without the gate objecting.
    """
    if relpath.startswith(REG_PREFIX):
        return "%s#%s" % (REG_PREFIX, name)
    return "%s#%s" % (relpath, name)


def dangling_pointers(scan):
    """-> {(row name, target)} for every anchor-shaped `[[...]]` target a registry row carries that is no row of
    either registry, matched EXACTLY. See ARM 8."""
    rows = set(k.split("#", 1)[1] for k in scan.names if k.startswith(REG_PREFIX + "#"))
    return set((k.split("#", 1)[1], t) for k, targets in scan.pointers.items() for t in targets
               if t not in rows)


def pointer_verdict(before, after):
    """ARM 8's differential -> (refused, debt), each a sorted list of (row name, target). A pointer naming no
    row now is DEBT when it named none at the base either, and REFUSED when that is new -- a row the change
    added or edited to point at nothing, or a row the change deleted or renamed out from under a pointer that
    resolved. The opener of a gated row the change ADDED is ARM 3's to refuse (`OPENER RESOLVES TO NOTHING`),
    so it is not refused a second time here."""
    now, then = dangling_pointers(after), dangling_pointers(before)
    arm3 = set((k.split("#", 1)[1], v[0]) for k, v in after.gated_rows.items()
               if k not in before.gated_rows and v[0])
    return sorted(p for p in now - then if p not in arm3), sorted(now & then)


def pointer_report(before, after, base, out=print):
    """ARM 8's report -> True when it REFUSES. The debt is one line of counts per registry, then each pending-
    registry pair (live work); the archive's pairs are counted, never listed (ARM 8's note). A refused pair is
    printed with what to do about it."""
    refused, debt = pointer_verdict(before, after)
    if debt:
        homes = {}
        for name, target in debt:
            homes.setdefault(after.pointer_home.get("%s#%s" % (REG_PREFIX, name), "?"), []).append((name, target))
        out("")
        out("anchor-debt: DEBT - %d `[[...]]` pointer(s) in %d row(s) name no row of either registry "
            "(pre-existing at %s, not refused here): %s"
            % (len(debt), len(set(n for n, _t in debt)), base,
               ", ".join("%d in %s" % (len(homes[h]), h) for h in sorted(homes))))
        for home in sorted(homes):
            if home.endswith("-done.md"):
                continue
            for name, target in homes[home]:
                out("  ~ %s   -> [[%s]]" % (name, target))
    if refused:
        out("")
        out("anchor-debt: FAIL - %d `[[...]]` pointer(s) this change introduced name no row of either "
            "registry:" % len(refused))
        for name, target in refused:
            out("  ! POINTER RESOLVES TO NOTHING  %s   -> [[%s]]" % (name, target))
        out("  `[[...]]` sends a reader to a row, and these name none: the id is mistyped, or the row it named "
            "was deleted or renamed by this change. Point each at the row its text means (`dssharness "
            "read-anchor <ID>` confirms one) -- or, where that row is gone for good, write the fact it held in "
            "place of the pointer, as cycle P67 did for the rows it deleted. Do not mint a row to be pointed at.")
    return bool(refused)


def scan_document(text, relpath):
    """-> Scan

    rows          : {"relpath#name": (is_open, status_excerpt, shape_kind)}
    findings      : [(relpath, line_no, severity, what)]

    star THE SEVERITY RULE, AND IT IS NOT A SOFTENING: **FATAL iff the MEASUREMENT is
    incomplete.** An unrecognized table shape and an orphan row both mean rows exist
    that this gate could not count -- the instrument cannot do its job, so the run
    fails. A table interrupted by an HTML comment is different in kind: those rows WERE
    counted, the denominator is whole, and what remains is a rendering defect in
    someone else's file. Making that fatal would conflate "I cannot measure" with "your
    markdown is untidy", and a gate whose failures mean two different things gets
    ignored for both. It is printed on EVERY run and carries its own anchor instead.

    Every data row of a RECOGNIZED table is counted. Recognition is by column shape,
    never by section number -- see the module docstring for why that distinction was
    measured, not assumed.

    star THE ORPHAN CHECK EXISTS BECAUSE THIS SCANNER IS TABLE-BASED AND ITS PREDECESSOR
    WAS LINE-BASED.  The old gate matched `^\\| `D-X` \\|` anywhere in the file, so a row
    that had drifted outside its table -- a broken separator, a stray blank line, a row
    pasted under a heading -- was still counted.  A table-based reader would drop it
    SILENTLY, trading one blind spot for another.  So any line that looks like an anchor
    row and was not consumed as part of some table is reported and fails the run.
    (Malformed rows really have occurred in this registry, so this is a live hazard,
    not a hypothetical one.)
    """
    scan = Scan()
    rows = scan.rows
    findings = scan.findings
    # The phase namespace is a property of the WHOLE document, not of its recognized
    # anchor tables, so it is read here rather than inside the shape-matching loop --
    # plan 27's sequencing table and plan 16's PR table are not anchor tables and
    # never will be, and a phase they schedule is still a phase.
    plan_number = plan_number_of(relpath)
    if plan_number:
        scan.plan_phases[plan_number] = phase_tokens(text)
    lines = text.split("\n")
    consumed = set()
    i = 0
    while i < len(lines):
        line = lines[i]
        if not (line.lstrip().startswith("|")
                and i + 1 < len(lines) and SEP_ROW.match(lines[i + 1])):
            i += 1
            continue

        header = tuple(norm_cell(c) for c in split_row(line)[1:-1])
        j = i + 2
        data = []
        consumed.add(i)
        consumed.add(i + 1)
        while j < len(lines):
            if lines[j].lstrip().startswith("|"):
                data.append(lines[j])
                consumed.add(j)
                j += 1
                continue
            # star TRANSPARENT FOR COUNTING, BUT NEVER SILENT.
            # A whole-line HTML comment inside a table body ends the table as far as a
            # markdown RENDERER is concerned, which would strand every row beneath it.
            # Dropping those rows would understate the denominator -- the very failure
            # this gate exists to prevent -- so the comment is stepped over and the
            # rows are COUNTED. The interruption is reported as a WARN on every run,
            # not a FATAL, because nothing was lost from the count: see the severity
            # rule in this function's docstring (FATAL iff the MEASUREMENT is
            # incomplete). The markdown really is malformed and the fix -- move the
            # comment above or below the table -- belongs to whoever owns the plan.
            # Counting them is not forgiving the defect; it is refusing to let the
            # defect corrupt the measurement while it is being fixed.
            if COMMENT_OPEN.match(lines[j]):
                k = j
                while k < len(lines) and not COMMENT_CLOSE.search(lines[k]):
                    k += 1
                if k < len(lines) and k + 1 < len(lines) \
                        and lines[k + 1].lstrip().startswith("|"):
                    for c in range(j, k + 1):
                        consumed.add(c)
                    findings.append(
                        (relpath, j + 1, "WARN",
                         "TABLE BODY INTERRUPTED BY AN HTML COMMENT (rows below it "
                         "were counted, but they render OUTSIDE the table)"))
                    j = k + 1
                    continue
            break

        shape = SHAPES_EXACT.get(header)
        if shape is None:
            for prefix, cand in SHAPES_PREFIX:
                if header[:len(prefix)] == prefix:
                    shape = cand
                    break

        if shape is not None:
            for raw in data:
                cells = split_row(raw)
                if len(cells) < 3:
                    continue
                name = row_name(cells[1])
                key = row_key(relpath, name)
                scan.names.add(key)
                if relpath.startswith(REG_PREFIX):
                    targets = set(t for t in (m.group(1).strip() for m in POINTER_REF.finditer(raw))
                                  if ANCHOR_TOKEN.fullmatch(t))
                    if targets:
                        scan.pointers.setdefault(key, set()).update(targets)
                        scan.pointer_home[key] = relpath
                if shape.status_col is None:
                    # No status column exists in this shape, so nothing can close a
                    # row in place. Unconditionally OPEN -- and a check mark sitting
                    # in some OTHER column must NOT close it (pinned in self_test).
                    # The three status-cell arms below are silent here for the same
                    # reason: there is no cell for them to read.
                    opened, excerpt = True, "(reserved anchor - no status column)"
                else:
                    def _cell(idx):
                        return cells[idx] if len(cells) > idx else ""
                    # ★★ ONE SPLIT, STATED ONCE: `is_closed` reads the VERDICT cell and
                    # every other predicate reads the PROSE cell. In the 4-cell shape
                    # they are the SAME cell, so that path is byte-for-byte what it was
                    # -- deliberately, because `is_mismarked_closure` and `is_gated` are
                    # windowed regex families whose comments record six defects, and
                    # widening what they are handed would silently re-open them.
                    # In the 6-cell registry shape `Status` is a three-value controlled
                    # vocabulary and `Trigger` still carries the glyph-led prose, so the
                    # walk-back retraction and the TRIGGER-GATED declaration stay exactly
                    # where those predicates already look (the bookkeeping pair `✅🧾`
                    # leading it is DssHarness's balance's to read since 0.6.7).
                    status = _cell(shape.status_col)     # THE verdict. Only is_closed.
                    prose = _cell(shape.trigger_col)     # the trigger prose
                    closing = _cell(shape.closing_col)
                    opened = not is_closed(status)
                    excerpt = " ".join(prose.split())[:80]
                    if is_mismarked_closure(prose, closing):
                        scan.mismarked[key] = excerpt
                    # (ARM 6, the Status/Trigger split, was recorded here until 2026-09-30: the DOOR refuses
                    # it since DssHarness 0.5.9 -- `anchors.triggerCarriesVerdict` -- and `read-anchors --lint`
                    # reports one written by hand, ✔MEASURED in a throwaway repository. "ARM 6"
                    # below names that rule, wherever it runs.)
                    # ⚠ A SEVENTH ARM, AND IT IS A DIFFERENTIAL WHERE ARM 6 IS A
                    # DAY-ONE REFUSAL. ARM 6 compares only CLOSED vs NOT-CLOSED, so
                    # the whole OPEN-vs-GATED axis is invisible to it -- and BOTH
                    # directions hide work: a row reading GATED is skipped by every
                    # queue-by-eye, and a row reading OPEN while marked GATED invites
                    # a lane to pick up something the registry says is blocked. ✔The
                    # cost was paid on 2026-09-03, when the orchestrator read
                    # `D-PP-SYNTHBUILDER-PREDEFINED-DEFINEDNESS` as gated from its
                    # trigger, briefed a lane on it, and the lane refuted it in its
                    # first minutes -- the exact failure the six-cell shape exists to
                    # prevent, committed by the party that enforces it.
                    #
                    # ⚠⚠ WHY IT IS NOT FOLDED INTO ARM 6, and this is the load-bearing
                    # difference: ARM 6's own rationale is that it CANNOT inherit debt,
                    # because the Status column was seeded on 2026-09-01 FROM
                    # `is_closed`. That argument does not carry here -- `is_closed`
                    # never distinguished gated from open, so the GATED word was seeded
                    # from nothing and the skew IS inherited. Widening ARM 6 would
                    # refuse, on day one, exactly the debt its own rationale says it
                    # cannot have, and block every commit until the backlog cleared:
                    # which is how a guard gets softened instead of obeyed.
                    if (shape.trigger_col != shape.status_col
                            and prose.strip()
                            and not is_closed(status) and not is_closed(prose)):
                        sw = lead_verdict_word(status)
                        tw = lead_verdict_word(prose)
                        # ⚠⚠ THE ESCAPE IS DIRECTIONAL, and it was NOT on the first
                        # draft of this arm -- which made the arm UNTESTABLE and was
                        # caught trying to prove it could fail. Every row this project
                        # mints declares `Trigger: ALREADY FIRED`, because the sibling
                        # arm refuses gate-like prose that names no opener. An
                        # undirected escape therefore exonerated every NEW row at
                        # once: the ratchet could not refuse anything it was built for.
                        # ⇒ A fired trigger excuses the PROSE saying GATED (a row must
                        # be able to name the gate it is about). It does NOT excuse the
                        # COLUMN saying GATED while the prose says OPEN-and-fired --
                        # that is the column being stale, which is exactly the half a
                        # human never reads and every queue does.
                        if ({sw, tw} == {"OPEN", "GATED"}
                                and not (tw == "GATED" and TRIGGER_FIRED.search(
                                    strip_decoration(prose) + " "
                                    + strip_decoration(closing)))):
                            scan.split_gate[key] = (
                                " ".join(status.split())[:24],
                                " ".join(strip_decoration(prose).split())[:60])
                    if opened:
                        gated = is_gated(prose, closing)
                        if gated:
                            scan.gated_rows[key] = (opener_of(raw), excerpt)
                        marker = lead_mark_class(closing)
                        if marker not in ("gate", "none"):
                            scan.unclassified[key] = (
                                marker, " ".join(closing.split())[:80])
                        # ARM 4 (b): the row says its trigger FIRED and still leads its
                        # closing-work cell with a gate marker. It is not gated -- its
                        # own verdict says so -- but it still READS as blocked, so it
                        # needs re-verdicting rather than silently leaving the arm.
                        elif (not gated and marker == "gate"
                                and TRIGGER_FIRED.search(
                                    strip_decoration(status) + " "
                                    + strip_decoration(closing))):
                            scan.unblocked[key] = "trigger declared FIRED"
                # Only OPEN rows are recorded, so when one name carries several rows in
                # the same file (the registry has a few), the name counts as OPEN if
                # ANY of its rows is open. Same behaviour as the predecessor, and it is
                # the safe direction: a closed duplicate cannot mask an open original.
                if opened:
                    rows[row_key(relpath, name)] = (True, excerpt, shape.kind)
        elif header not in EXCLUDED_HEADERS:
            # Unrecognized shape. Only a table that LOOKS like an anchor table is a
            # finding -- ordinary prose tables are none of this gate's business.
            if any(len(split_row(r)) > 1 and ANCHOR_TOKEN.search(split_row(r)[1])
                   for r in data):
                findings.append((relpath, i + 1, "FATAL",
                                 "UNRECOGNIZED TABLE SHAPE: "
                                 + " ".join(line.split())[:110]))
        i = j

    for n, raw in enumerate(lines):
        if n in consumed or not raw.lstrip().startswith("|"):
            continue
        cells = split_row(raw)
        if len(cells) > 2 and ANCHOR_TOKEN.search(cells[1]):
            findings.append((relpath, n + 1, "FATAL",
                             "ORPHAN ANCHOR ROW (in no table): "
                             + " ".join(raw.split())[:100]))
    return scan


_OWNING_TREE = None


def _owning_tree():
    """`.harness-config/runner/actions/owning-tree/owning-tree.py` -- the one owner of "which tree is this file in?".

    Loaded by path from this file's sibling directory (a hyphen is not a module name). It
    FAILS LOUD when absent rather than falling back to a local walk: a second copy of the
    answer is the drift that owner exists to end.
    """
    global _OWNING_TREE
    if _OWNING_TREE is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                            "owning-tree", "owning-tree.py")
        if not os.path.isfile(path):
            sys.exit("anchor-debt: cannot find %s -- this guard's root is resolved there "
                     "and nowhere else" % path)
        spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _OWNING_TREE = mod
    return _OWNING_TREE


def repo_root():
    """The tree THIS FILE lives in -- never the tree the caller's shell is standing in.

    ⚠ This was a bare `git rev-parse --show-toplevel`. ✔MEASURED 2026-09-15 (P66): run by
    path with its cwd inside a different repository, it counted THAT repository's
    registries and named a row only that repository holds; from a directory inside no
    repository it would not run at all. ctest pins `WORKING_DIRECTORY`, so no gate saw
    either. The walk, the measurement that chose it, and why git must agree are in
    `.harness-config/runner/actions/owning-tree/owning-tree.py`. `reads_git`, because the baseline half of this
    gate is read through `git ls-tree` / `git show` at `--base`.
    """
    ot = _owning_tree()
    try:
        return ot.resolve(__file__, reads_git=True)
    except ot.Refusal as exc:
        sys.exit("anchor-debt: %s" % exc)


def plan_files_at(root, ref):
    """Depth-1 `.plans/*.md` as of `ref`. Fails loud rather than scanning nothing."""
    # ★ THROUGH `run_git`: a caller's exported GIT_DIR otherwise names the repository whose
    # `ref` is read. ✔MEASURED 2026-09-15 (P66 lane rr): under another repository's GIT_DIR
    # this gate read THAT repository's HEAD as its baseline ("OPEN at HEAD registry=0 ...
    # opened 447"). The helper, and why, are in .harness-config/runner/actions/owning-tree/.
    p = _owning_tree().run_git(["ls-tree", "--name-only", "%s:%s" % (ref, PLANS_DIR)],
                               cwd=root, capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
    if p.returncode != 0:
        sys.exit("cannot list %s at %s: %s" % (PLANS_DIR, ref, (p.stderr or "").strip()[:200]))
    names = [n for n in p.stdout.split("\n") if n.strip().endswith(".md")]
    if not names:
        sys.exit("no .md files found under %s at %s - the scan collapsed, which is "
                 "NOT the same as a clean tree" % (PLANS_DIR, ref))
    return ["%s/%s" % (PLANS_DIR, n.strip()) for n in names]


def scan_at_ref(root, ref):
    scan = Scan()
    for rel in plan_files_at(root, ref):
        p = _owning_tree().run_git(["show", "%s:%s" % (ref, rel)], cwd=root,
                                   capture_output=True, text=True, encoding="utf-8",
                                   errors="replace")
        if p.returncode != 0:
            sys.exit("cannot read %s at %s: %s" % (rel, ref, (p.stderr or "").strip()[:200]))
        scan.merge(scan_document(p.stdout, rel))
    return scan


def scan_worktree(root):
    scan = Scan()
    plans = os.path.join(root, PLANS_DIR)
    if not os.path.isdir(plans):
        sys.exit("no %s directory in %s" % (PLANS_DIR, root))
    names = sorted(n for n in os.listdir(plans) if n.endswith(".md"))
    if not names:
        sys.exit("no .md files under %s - the scan collapsed, which is NOT a clean tree"
                 % PLANS_DIR)
    for n in names:
        rel = "%s/%s" % (PLANS_DIR, n)
        with io.open(os.path.join(plans, n), encoding="utf-8") as fh:
            scan.merge(scan_document(fh.read(), rel))
    return scan


def _doc(*rows):
    return "\n".join(rows)


REG_HDR = ["| Anchor | Trigger | Closing work | Cross-refs |", "|---|---|---|---|"]
DEF_HDR = ["| # | Deferred item | Why deferred | Owner / closure | Trigger |",
           "|---|---|---|---|---|"]
P00_HDR = ["| # | Deferred item | Why deferred | Class | Owner / closure | Trigger |",
           "|---|---|---|---|---|---|"]
RES_HDR = ["| Anchor | Owns |", "|--------|------|"]


EXPECTED_CASES = 106  # 112 while ARM 9's six stood (retired 2026-10-07 with DssHarness 0.6.9); 98 until
                      # 2026-10-06: arm (f2) retired with the bookkeeping listing (DssHarness 0.6.7); 97 until
                      # ARM 8's nine, the same day


def self_test():
    """Red-on-disable for the instrument itself.

    Families of case, and ALL must hold:
      (a) the glyph-agnostic inversion -- if someone 'helpfully' rewrites is_closed()
          to enumerate open glyphs, the novel-glyph cases fail;
      (b) the widened denominator -- if someone narrows the scan back to the registry,
          or lets an unknown table shape pass quietly, those cases fail;
      (f) the BOOKKEEPING closure `✅🧾` -- still CLOSED for the population, the
          mark alone no closure (whether it is credited is DssHarness's balance's);
      (g) a status cell whose opening VERDICT is a closure while its marker is not;
      (h) a gated row that must NAME the row which opens it;
      (i) an opener that resolves in EITHER namespace -- an anchor row or a plan
          phase -- and is refused in both when it resolves in neither.

    star EVERY CASE ASSERTS EVERY OUTPUT -- the open set, the finding counts AND
    every per-row set -- rather than only the one it was written for. A case that
    asserted just its own subject would let a new arm fire spuriously on twenty
    unrelated fixtures and still print `0 failed`, which is the vacuous green this
    file exists to refuse.

    ⚠ THE FIXTURE NAMES ARE DELIBERATELY **NOT** ANCHOR-SHAPED IN THIS FILE'S
    SOURCE, and that is a contract, not a style choice. This directory is a
    citation root (`anchors.citationRoots`), so a fixture name the citation
    grammar matches reads as a citation of a deferral that does not exist. Eleven
    three-segment names did exactly that under the guard's ORIGINAL `{2,}`
    grammar, green only because the registry row REPORTING them quoted their
    names -- one tidy-up away from reding the tree on eleven strings that are
    parser INPUT DATA.
    ⇒ **THE CONTRACT SINCE P50 IS A SEAM, NOT A SEGMENT COUNT.** The guard's
    threshold widened to `{1,}` on 2026-09-01 (the old one hid seventy
    registered rows), which took
    the old rule -- "exactly two hyphen-separated segments is invisible by
    construction" -- from load-bearing to FALSE: two-segment names are formal
    citations now. So every fixture id below is ASSEMBLED FROM ADJACENT STRING
    LITERALS, seamed after the head segment, exactly as the sibling guards'
    self-tests have always spelled their over-threshold anchors. The RUNTIME
    value is unchanged and still matched by this file's own two-segment
    ANCHOR_TOKEN, so the orphan-row and unrecognized-table arms below still
    fire; the SOURCE never carries a whole anchor-shaped token. A NEW FIXTURE
    MUST KEEP THAT SPELLING -- and note the seam INSIDE a docstring or comment
    is the same two-quote mark spelled verbatim, which both kills the shape for
    the scanner and flags the convention to a reader.
    Do not allowlist instead: an Allowlist entry would assert these are code-internal
    pins, which is false, and would keep asserting it if a future cycle ever opened
    one of the names for real.
    ⓘ `D-AXIS-ASYNC-DI` stays whole in its case below: it names a row that REALLY
    EXISTS and the case is about that specific row's shape, so it is a citation,
    not dangling input data -- which is the whole distinction the eleven violated.
    The underscore and strikethrough cases used to borrow real rows too (the
    second one `D-LK-DYN-RODATA-ITEM-RELOC`'s tail); both carry synthetic names
    now, because the underscore row was retired with the harness registry on
    2026-09-16 and a fixture must not depend on the registry it is not testing.
    ✔MEASURED 2026-09-18 with `dssharness read-anchor`: both named rows resolve.
    """
    cases = []

    def case(doc, expect, why, path=REG_REL, expect_fatal=0, expect_warn=0,
             mismark=(), gated=None, unclass=(), unblock=()):
        cases.append((doc, expect, why, path, expect_fatal, expect_warn,
                      set(mismark), dict(gated or {}),
                      set(unclass), set(unblock)))

    # ── (a) the inversion, on the REGISTRY shape (all pre-existing pins, preserved) ──
    case(_doc(*REG_HDR, "| `D-A` | ✅ **CLOSED** | done | refs |"), set(),
         "a closed row is not open")
    case(_doc(*REG_HDR, "| `D-B` | \U0001f7e0 **OPEN** | work | refs |"), {"D-B"},
         "orange is open")
    case(_doc(*REG_HDR, "| `D-C` | ⏳ **OPEN** | work | refs |"), {"D-C"},
         "HOURGLASS is open (the 1st miss)")
    case(_doc(*REG_HDR, "| `D-D` | \U0001f534 **OPEN** | work | refs |"), {"D-D"},
         "red is open")
    case(_doc(*REG_HDR, "| `D-E` | ⚠ **OPEN** | work | refs |"), {"D-E"},
         "warning is open")
    case(_doc(*REG_HDR, "| `D-F` | \U0001f9ff **NOVEL GLYPH** | work | refs |"), {"D-F"},
         "a glyph nobody enumerated is open -- the whole point")
    case(_doc(*REG_HDR, "| `D-G` | no glyph at all | work | refs |"), {"D-G"},
         "no marker at all is open")
    case(_doc(*REG_HDR, "| `D-H` | ✅ **CLOSED** | supersedes a \U0001f7e0 row | refs |"),
         set(), "only the STATUS cell decides")
    case(_doc(*REG_HDR, "not a row at all"), set(), "non-rows are ignored")
    case(_doc(*REG_HDR, "| `D-I` | \U0001f7e0 **OPEN** -- half ✅ done, half not | w | r |"),
         {"D-I"}, "mid-prose check mark does NOT close (the 2nd miss)")
    case(_doc(*REG_HDR, "| `D-J` | ⏳ **PARTIALLY CLOSED -- ✅ part (1); (2) open** | w | r |"),
         {"D-J"}, "PARTIALLY closed is OPEN, however many marks it carries")
    case(_doc(*REG_HDR,
              "| `D-K` | ✅ **CLOSED 2026-01-01.** *(Was: OPEN -- \U0001f534 HIGH.)* | d | r |"),
         set(), "a closed row may recap 'Was: OPEN' and stays closed")
    case(_doc(*REG_HDR, "| `D-L` | **✅ CLOSED** | done | refs |"), set(),
         "leading markdown emphasis before the mark still closes")

    # ── (b) THE WIDENED DENOMINATOR: plan-side shapes ──
    # star The 3rd miss: a plan-side row
    # was worth ZERO to this gate, so moving work out of the registry looked like
    # progress. These pin that a plan row counts exactly like a registry row.
    case(_doc(*DEF_HDR, "| D-XX" "-PLOPEN | plain prose, no glyph | why | owner | trig |"),
         {"D-XX" "-PLOPEN"}, "plan sec3.1 row with no glyph is OPEN", path=".plans/22-x.md")
    case(_doc(*DEF_HDR, "| ~~D-XX" "-PLDONE~~ | ✅ **CLOSED 2026-01-01** | why | own | t |"),
         set(), "plan sec3.1 leading check mark closes (cell-1 strikethrough ignored)",
         path=".plans/22-x.md")
    # ★ REQUIRED CASE 1: mid-prose check mark in a plan-shaped row.
    case(_doc(*DEF_HDR,
              "| D-PL" "-HALF | **OPEN** -- the ✅ layout half landed; va_arg half open | w | o | t |"),
         {"D-PL" "-HALF"},
         "plan row: mid-prose check mark does NOT close (the 2nd miss, plan side)",
         path=".plans/14-x.md")
    # ★ REQUIRED CASE 2: a glyph nobody has enumerated, on the plan-side path.
    case(_doc(*DEF_HDR, "| D-PL" "-NOVEL | \U0001fae0 **melting face status** | w | o | t |"),
         {"D-PL" "-NOVEL"}, "plan row: a novel glyph is OPEN (the whole point, plan side)",
         path=".plans/13-x.md")
    # ✔MEASURED shape of plan 12: doneness recorded in the LAST cell, not the status
    # cell. That row reads OPEN. It is the SAFE direction and it is left that way on
    # purpose -- moving the test to "a check mark anywhere in the row" would re-import
    # the exact 2026-08-12 undercount. Pinned so nobody 'fixes' it.
    case(_doc(*DEF_HDR, "| D-PL" "-LATE | hardening item | cosmetic | none. | ML6 c1 ✅ done |"),
         {"D-PL" "-LATE"},
         "check mark in a LATER cell does not close (plan 12's real shape)",
         path=".plans/12-x.md")
    # plan-00 section 0.2: 6 columns, a `Class` column, and ids that are not anchors.
    case(_doc(*P00_HDR, "| D7 | **Some project-level item** | why | HIGH | plan 14 | trig |"),
         {"D7"}, "plan-00 sec0.2 `D7` row counts even though it is not a D-*-* anchor",
         path=".plans/00-x.md")
    case(_doc(*P00_HDR, "| ~~D2~~ | ✅ **closed 2026-05-26.** | why | - | - | - |"),
         set(), "plan-00 sec0.2 closed row is closed", path=".plans/00-x.md")
    # RESERVED tables have no status column at all.
    case(_doc(*RES_HDR, "| `D-AXIS-ASYNC-DI` | language-side async DI API |"),
         {"D-AXIS-ASYNC-DI"}, "reserved-anchor row is OPEN (no status column)",
         path=".plans/24-x.md")
    case(_doc(*RES_HDR, "| `D-AXIS" "-DONE` | ✅ shipped, honest |"), {"D-AXIS" "-DONE"},
         "a check mark in a NON-status column cannot close a reserved row",
         path=".plans/24-x.md")

    # ── (c) blind spots of the OLD row regex, now counted ──
    # ⚠ ASSEMBLED FROM FRAGMENTS, like the `D-XX""-NEWTABLE` case below and for the same
    # reason: this directory is a citation root, so a real row id written
    # whole here is a CITATION rather than test data. Both of these WERE real ids, and one
    # of them went unnoticed only because its strikethrough made the row unreadable -- until
    # the DssHarness migration repaired that cell and the fixture's "still open" text became
    # a refusal citing a closed row. The VALUE is still a well-formed name at runtime, which
    # is the whole of what these cases measure.
    _UNDERSCORE = "D-FIXTURE" + "-QEMU_LD_PREFIX-AMBIENT-ONLY"
    _STRUCK = "D-FIXTURE" + "-DYN-RODATA-ITEM-RELOC"
    case(_doc(*REG_HDR, "| `" + _UNDERSCORE + "` | ⚠ OPEN | w | r |"),
         {_UNDERSCORE},
         "UNDERSCORE in the name: invisible to the old regex, counted now")
    case(_doc(*REG_HDR, "| ~~`" + _STRUCK + "`~~ | ⚠ still open | w | r |"),
         {_STRUCK},
         "STRIKETHROUGH name: invisible to the old regex, counted now")

    # ── (d) fail-loud on shapes, and NO false alarm on the named exclusions ──
    case(_doc("| Widget | Notes |", "|---|---|",
              "| `D-XX" "-NEWTABLE` | invented table |"),
         set(), "an UNRECOGNIZED anchor-looking table is REPORTED, never skipped",
         path=".plans/99-x.md", expect_fatal=1)
    case(_doc("| PR | Title | Scope |", "|---|---|---|",
              "| ~~AS3~~ | cycle 3 landed (binary ops + D-PLAN12-ARM64-STACKPOINTER-REGISTRATION-ABI-GOLDEN-TESTS) | s |"),
         set(), "a NAMED non-deferral table is excluded, and raises no false alarm",
         path=".plans/13-x.md")
    case(_doc("| Tier | Example | External tools? |", "|---|---|---|",
              "| **Production pipeline** | dsscp compiling sqlite | Zero. |"),
         set(), "an ordinary prose table with no anchors is simply ignored",
         path=".plans/17-x.md")
    # star The line-based predecessor would have counted these; a table-based reader
    # must not lose them to silence. Both are REPORTED, which fails the run.
    case(_doc("## A heading, and then a row with no table header at all",
              "| `D-XX" "-ORPHANROW` | \U0001f7e0 **OPEN** | work | refs |"),
         set(), "an ORPHAN anchor row outside any table is REPORTED, never dropped",
         path=".plans/99-y.md", expect_fatal=1)
    case(_doc(*REG_HDR, "| `D-XX" "-INTABLE` | ⚠ OPEN | w | r |", "",
              "| `D-XX" "-BLANKLINE` | ⚠ OPEN | w | r |"),
         {"D-XX" "-INTABLE"},
         "a row severed from its table by a blank line is REPORTED, not silently lost",
         expect_fatal=1)
    # star 22-optimizer section 3.1's REAL defect, pinned: an HTML comment mid-table.
    # The rows below it must still be COUNTED (dropping them understates the
    # denominator) AND the interruption must still be REPORTED (the file is malformed).
    case(_doc(*DEF_HDR, "| D-XX" "-BEFORECOMMENT | still open | w | o | t |",
              "<!-- a user-supervised note parked inside the table body -->",
              "| D-XX" "-AFTERCOMMENT | also still open | w | o | t |",
              "| D-XX" "-AFTERCLOSED | ✅ **CLOSED** | w | o | t |"),
         {"D-XX" "-BEFORECOMMENT", "D-XX" "-AFTERCOMMENT"},
         "rows under a mid-table HTML comment are COUNTED and the break is REPORTED",
         path=".plans/22-x.md", expect_warn=1)
    case(_doc(*DEF_HDR, "| D-XX" "-LASTROW | open | w | o | t |",
              "<!-- a comment that legitimately FOLLOWS the table -->",
              "some prose"),
         {"D-XX" "-LASTROW"},
         "a comment AFTER the last row is not an interruption (no false alarm)",
         path=".plans/22-x.md")

    # ── (f) THE BOOKKEEPING CLOSURE MARK ──
    # star Leading position, closure mark FIRST. The row must read CLOSED for the
    # population; whether its closure is CREDITED is DssHarness's balance's to say
    # (0.6.7), never this scan's.
    case(_doc(*REG_HDR, "| `D-XX" "-BOOKKEPT` | ✅🧾 **CLOSED 2026-01-01, mark repaired** | - | r |"),
         set(), "a bookkeeping closure is CLOSED for the population")
    case(_doc(*REG_HDR, "| `D-XX" "-BOOKEMPH` | **✅🧾 CLOSED** | - | r |"), set(),
         "leading emphasis before the pair still reads CLOSED")
    case(_doc(*REG_HDR, "| `D-XX" "-BOOKPROSE` | ✅ **CLOSED** -- 🧾 pure bookkeeping, honest | - | r |"),
         set(), "a MID-PROSE bookkeeping mark leaves a closure CLOSED")
    # star THIS CASE WAS FIRST WRITTEN EXPECTING NO SECOND FINDING, AND THE SELF-TEST
    # CORRECTED THE EXPECTATION RATHER THAN THE RULE -- the same way the `std :: abort
    # ()` case did in `check-no-abort-in-tests`. A cell leading with the bookkeeping
    # mark alone is OPEN (that mark is not a closure marker) AND its opening verdict
    # is `CLOSED`, so it is exactly the self-contradiction arm (g) exists to catch.
    # Both facts are true at once and both are now pinned.
    case(_doc(*REG_HDR, "| `D-XX" "-BOOKONLY` | 🧾 **CLOSED 2026-01-01** | - | r |"),
         {"D-XX" "-BOOKONLY"},
         "the bookkeeping mark ALONE does not close, and is then self-contradicting",
         mismark={"D-XX" "-BOOKONLY"})
    case(_doc(*REG_HDR, "| `D-XX" "-BOOKGAP` | ✅ 🧾 **CLOSED, mark repaired** | - | r |"),
         set(), "a blank between the two marks still reads CLOSED")

    # ── (g) A STATUS CELL WHOSE OPENING VERDICT CONTRADICTS ITS MARKER ──
    case(_doc(*REG_HDR, "| `D-XX" "-MISMARK` | **FIXED 2026-01-01** -- the driver was repaired | - | r |"),
         {"D-XX" "-MISMARK"}, "opening verdict FIXED with no closure marker is caught",
         mismark={"D-XX" "-MISMARK"})
    case(_doc(*REG_HDR, "| `D-XX" "-MISGLYPH` | \U0001f534 **CLOSED 2026-01-01** | - | r |"),
         {"D-XX" "-MISGLYPH"}, "a closure verdict behind an OPEN glyph is caught",
         mismark={"D-XX" "-MISGLYPH"})
    # ★ THE WALK-BACK MAY LIVE IN ANY CELL -- ✔MEASURED on
    # D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME, whose status cell opens `SHIPPED`
    # and whose CLOSING-WORK cell says "NOT YET DISCHARGED ... stays open".
    case(_doc(*REG_HDR,
              "| `D-XX" "-WALKED` | **SHIPPED 2026-01-01** | NOT YET discharged; verify first | r |"),
         {"D-XX" "-WALKED"},
         "a walk-back in ANOTHER cell exonerates the row (the measured shape)")
    case(_doc(*REG_HDR, "| `D-XX" "-PARTIAL` | **CLOSED** for the rodata path; PARTIAL | w | r |"),
         {"D-XX" "-PARTIAL"}, "a scoped/PARTIAL closure is not accused")
    case(_doc(*REG_HDR, "| `D-XX" "-PROSE` | \U0001f7e0 **OPEN** -- the FIXED half landed | w | r |"),
         {"D-XX" "-PROSE"}, "a closure word MID-cell is not an opening verdict")
    case(_doc(*REG_HDR, "| `D-XX" "-REALCLOSE` | ✅ **CLOSED 2026-01-01** | - | r |"), set(),
         "a properly marked closure is never accused")
    # ★ The measured false accusation: plan-00 section 0.2's `D12` opens
    # "Shipped-lib FFI = Model 3" and is explicitly NOT a deferral.
    case(_doc(*P00_HDR,
              "| D-XX" "-COMPOUND | **Shipped-lib FFI = Model 3** -- chosen 2026-01-01 | w | HIGH | o | t |"),
         {"D-XX" "-COMPOUND"}, "a HYPHENATED COMPOUND is not an opening verdict",
         path=".plans/00-x.md")

    # ── (h) A GATED ROW MUST NAME THE ROW THAT OPENS IT ──
    case(_doc(*REG_HDR, "| `D-XX" "-GATEDBARE` | \U0001f7e0 **OPEN -- TRIGGER-GATED** | w | r |"),
         {"D-XX" "-GATEDBARE"}, "a gated row with no opener is recorded with opener ''",
         gated={"D-XX" "-GATEDBARE": ""})
    case(_doc(*REG_HDR,
              "| `D-XX" "-GATEDOK` | \U0001f7e0 **OPEN -- TRIGGER-GATED**, opened by [[D-XX" "-OPENER]] | w | r |"),
         {"D-XX" "-GATEDOK"}, "an opener reference is read from the row",
         gated={"D-XX" "-GATEDOK": "D-XX" "-OPENER"})
    # star A BARE CROSS-REFERENCE IS NOT AN OPENER. Every registry row carries
    # `[[...]]` links; accepting one would make this arm satisfiable by any row that
    # merely mentions a neighbour -- the "satisfied by a mention" failure this
    # registry has already recorded twice.
    case(_doc(*REG_HDR,
              "| `D-XX" "-GATEDXREF` | \U0001f7e0 **OPEN -- TRIGGER-GATED**; see [[D-XX" "-OTHER]] | w | r |"),
         {"D-XX" "-GATEDXREF"}, "a bare cross-reference does NOT count as an opener",
         gated={"D-XX" "-GATEDXREF": ""})
    case(_doc(*REG_HDR, "| `D-XX" "-GATEDSHUT` | ✅ **CLOSED, was TRIGGER-GATED** | - | r |"),
         set(), "a CLOSED gated row is not asked for an opener")
    # ★★★ THE `NO TRIGGER` ESCAPE, PINNED FROM BOTH SIDES -- IT SHIPPED WITH NEITHER.
    # It was the only one of the four exonerations with no mirror arm, and it was a bare
    # search over the whole row, so it read ANOTHER row's verdict as this row's own. The
    # first arm is the escape doing its job; the second is the abuse it must refuse, and
    # WITHOUT THE SECOND the first passes just as well with the attributive check deleted.
    # ⚠ An exonerated row is reported NOWHERE -- not gated, not DEBT -- so a false
    # exoneration here is silent, which is why the negative arm carries the weight.
    case(_doc(*REG_HDR,
              "| `D-XX" "-NOTRIG` | \U0001f7e0 **OPEN -- Trigger: none, informational and permanent** | w | r |"),
         {"D-XX" "-NOTRIG"},
         "a row declaring ITS OWN trigger is none is not gated")
    case(_doc(*REG_HDR,
              "| `D-XX" "-NOTRIGXREF` | \U0001f7e0 **OPEN -- TRIGGER-GATED**; unlike [[D-XX" "-OTHER]], whose trigger is none, this one waits | w | r |"),
         {"D-XX" "-NOTRIGXREF"},
         "quoting ANOTHER row's 'trigger: none' does NOT exonerate this one",
         gated={"D-XX" "-NOTRIGXREF": ""})
    case(_doc(*REG_HDR, "| `D-XX" "-MUSTNOT` | \U0001f7e0 **OPEN -- MUST-NOT-BUILD** | w | r |"),
         {"D-XX" "-MUSTNOT"}, "MUST-NOT-BUILD is the same declaration as TRIGGER-GATED",
         gated={"D-XX" "-MUSTNOT": ""})
    case(_doc(*REG_HDR, "| `D-XX" "-UNGATED` | \U0001f7e0 **OPEN -- normal** | w | r |"),
         {"D-XX" "-UNGATED"}, "an ordinary open row is not a gated row")
    # star star star THE PAIR THAT PINS "A DESCRIPTION OF A CLASS IS NOT A MEMBER OF IT".
    # The first fixture has the shape of the real registry row that recorded this
    # arm's own census: it must QUOTE the vocabulary in order to say what it
    # measured, and its own verdict is that its trigger has fired. The second is a
    # real gate carrying the same words, and it must STILL be asked for an opener --
    # without it the first case would pass just as well with `is_gated` deleted.
    case(_doc(*REG_HDR,
              "| `D-XX" "-CENSUS` | \U0001f535 **OPEN -- DISCLOSED.** MEASURED: 62 rows declare "
              "themselves TRIGGER-GATED / MUST-NOT-BUILD / TRIGGER-NOT-FIRED and name no "
              "opener. Trigger: ALREADY FIRED. | sweep them | r |"),
         {"D-XX" "-CENSUS"},
         "a row DESCRIBING the gated class is not a member of it")
    case(_doc(*REG_HDR,
              "| `D-XX" "-STILLGATED` | \U0001f7e0 **OPEN -- TRIGGER-GATED.** The first ILP32 "
              "target lands. | w | r |"),
         {"D-XX" "-STILLGATED"},
         "and a real gate carrying the same words IS still asked for an opener",
         gated={"D-XX" "-STILLGATED": ""})
    # A fired trigger exonerates wherever it sits: the claim is about the WORLD, not
    # about where in the cell it was written (same stance as the disclosed mark).
    case(_doc(*REG_HDR,
              "| `D-XX" "-FIREDLATE` | \U0001f7e0 **OPEN -- TRIGGER-GATED** when opened; the "
              "trigger has ALREADY FIRED, so the work is actionable now. | w | r |"),
         {"D-XX" "-FIREDLATE"},
         "a fired trigger exonerates from anywhere in the cell")

    # ── (h2) THE DECLARATION MAY LIVE IN THE **CLOSING-WORK** CELL ──
    # Every arm below was
    # RED before the widening: the house style writes the gate into the remedy cell
    # and a status-cell-only test reported OK over all of it.
    case(_doc(*REG_HDR,
              "| `D-XX" "-GATEDREMEDY` | \U0001f7e0 **OPEN -- normal** | ⏳ **TRIGGER: an "
              "ILP32 target lands.** | r |"),
         {"D-XX" "-GATEDREMEDY"},
         "an hourglass LEADING the closing-work cell is a gate declaration",
         gated={"D-XX" "-GATEDREMEDY": ""})
    case(_doc(*REG_HDR,
              "| `D-XX" "-GATEDSTOP` | \U0001f7e0 **OPEN -- normal** | ⛔ **DO NOT PATCH "
              "LOCALLY** -- bring it as a section B. | r |"),
         {"D-XX" "-GATEDSTOP"},
         "a no-entry sign LEADING the closing-work cell is a gate declaration",
         gated={"D-XX" "-GATEDSTOP": ""})
    case(_doc(*REG_HDR,
              "| `D-XX" "-GATEDWORDS` | \U0001f7e0 **OPEN -- normal** | Implement it. Trigger: "
              "trigger-gated. Priority: low. | r |"),
         {"D-XX" "-GATEDWORDS"},
         "the DECLARATION WORDS in the closing-work cell count too",
         gated={"D-XX" "-GATEDWORDS": ""})
    # star THE MIRROR-IMAGE CASE, AND IT IS THE ONE THAT KEEPS THE GLYPH RULE HONEST.
    # ⛔ LEADING A **STATUS** CELL overwhelmingly means "do not re-propose this"
    # (REFUTED-DESIGN / NEGATIVE RESULT / SUPERSEDED) -- five of the six live
    # instances. Reading the glyph there would demand an opener from rows that
    # cannot have one, so the glyph is read ONLY in the closing-work cell.
    case(_doc(*REG_HDR,
              "| `D-XX" "-REFUTED` | ⛔ **REFUTED-DESIGN -- do not re-propose.** | w | r |"),
         {"D-XX" "-REFUTED"},
         "a no-entry sign leading the STATUS cell is NOT read as a gate")
    # star A ROW WITH NO CLOSING WORK IS NOT A GATE. Both live spellings are pinned;
    # without this arm the widening accuses every negative-result row in the registry.
    case(_doc(*REG_HDR,
              "| `D-XX" "-NOWORK` | \U0001f7e0 **OPEN -- NEGATIVE RESULT** | ⛔ **NO CLOSING "
              "WORK -- there is nothing to fix.** | r |"),
         {"D-XX" "-NOWORK"},
         "a row declaring NO CLOSING WORK is not a gate, whatever glyph it leads with")
    case(_doc(*REG_HDR,
              "| `D-XX" "-NOWORK2` | \U0001f7e0 **OPEN** | ⏳ Closing work: none -- recorded "
              "so nobody re-investigates. | r |"),
         {"D-XX" "-NOWORK2"},
         "... and the second live spelling, `Closing work: none`, too")
    # star THE ESCAPED PIPE: split_row used to treat an escaped pipe as a separator.
    # 161 live rows carry one; before the fix everything after it shifted one cell
    # left, so the closing-work cell handed to the two arms above was a FRAGMENT.
    # ⚠ The fixture puts the escape in the STATUS cell and the gate in the CLOSING
    # cell -- the exact live shape of `D-PP-HAS-EXTENSION-BUILTIN-ABSENT`. With a
    # naive split the gate marker lands in cell 4 and is never read.
    case(_doc(*REG_HDR,
              "| `D-XX" "-ESCPIPE` | \U0001f7e0 **OPEN** -- the shape is `a \\| b` | "
              "⏳ **TRIGGER: a consumer needs it.** | r |"),
         {"D-XX" "-ESCPIPE"},
         "an ESCAPED pipe does not shift the cells (the gate is still found)",
         gated={"D-XX" "-ESCPIPE": ""})
    case(_doc(*REG_HDR,
              "| `D-XX" "-ESCPIPECLOSED` | ✅ **CLOSED 2026-01-01** -- pattern `x \\| y` | "
              "- | r |"),
         set(), "an escaped pipe in a CLOSED row still leaves it closed")

    # ── (h3) THE FOUR EXONERATIONS, EACH PINNED WITH ITS MIRROR ────────────────
    # Every arm below has a partner case that must STAY gated, because an
    # exoneration with no counter-case passes just as well with the predicate
    # deleted -- the same pairing the census/still-gated fixtures above use.
    #
    # star THE FIRED TEST RECOGNIZED ONE SPELLING OF SIX:
    # `Trigger: FIRED` without the adverb is the registry's commonest spelling
    # and matched nothing. (No count here on purpose: the 317 this comment used to
    # quote was a greedy `Trigger:.*FIRED` total mis-attributed to one bucket --
    # see the note above `TRIGGER_FIRED`. The claim that carries the arm is
    # "commonest and unmatched", which no arithmetic can rot.)
    case(_doc(*REG_HDR,
              "| `D-XX" "-FIREDFIELD` | \U0001f7e0 **OPEN -- TRIGGER-GATED** when opened. "
              "Trigger: FIRED (measured). | w | r |"),
         {"D-XX" "-FIREDFIELD"},
         "`Trigger: FIRED` -- no adverb -- exonerates, like ALREADY FIRED")
    case(_doc(*REG_HDR,
              "| `D-XX" "-FIREDVERB` | \U0001f7e0 **OPEN -- MUST-NOT-BUILD** until then; the "
              "trigger fired on 2026-01-01. | w | r |"),
         {"D-XX" "-FIREDVERB"}, "... and so does `the trigger fired`")
    case(_doc(*REG_HDR,
              "| `D-XX" "-NOTFIRED` | \U0001f7e0 **OPEN** | Implement it. Trigger: NOT "
              "FIRED -- no consumer yet. | r |"),
         {"D-XX" "-NOTFIRED"},
         "`Trigger: NOT FIRED` is a GATE, never an exoneration",
         gated={"D-XX" "-NOTFIRED": ""})
    case(_doc(*REG_HDR,
              "| `D-XX" "-NOTYETFIRED` | \U0001f7e0 **OPEN** -- the trigger has NOT yet "
              "fired. | w | r |"),
         {"D-XX" "-NOTYETFIRED"},
         "... and so is `the trigger has NOT yet fired` (the house colon form)",
         gated={"D-XX" "-NOTYETFIRED": ""})
    # star is_gated ACCUSED A ROW THAT DECLARES NO GATE, arm 1:
    # a row with NO trigger cannot honestly claim its trigger fired, so it needs
    # its own words. The live instance is a POINTER row accused for quoting the
    # status of the rows it points at.
    case(_doc(*REG_HDR,
              "| `D-XX" "-POINTER` | \U0001f4cd **POINTER ROW** -- the family lives in "
              "plan 14 (both OPEN, trigger-gated). Trigger: none -- informational, "
              "permanent. | Nothing to do. | r |"),
         {"D-XX" "-POINTER"},
         "a row declaring `Trigger: none` is not gated by what it QUOTES")
    # star ... arm 2: a NEGATED declaration is not a declaration. The live instance
    # was reworded out during the P29 sweep, which is what the FAIL text forbids --
    # so the shape is pinned here instead of relying on a row to carry it.
    case(_doc(*REG_HDR,
              "| `D-XX" "-NEGATED` | \U0001f7e0 **OPEN -- HIGH.** Priority raised: no longer "
              "merely trigger-gated, a real consumer exists. | w | r |"),
         {"D-XX" "-NEGATED"},
         "a NEGATED declaration is not a declaration")
    case(_doc(*REG_HDR,
              "| `D-XX" "-NEGATEDPAIR` | \U0001f7e0 **OPEN -- HIGH.** It is trigger-gated, a "
              "real consumer is still missing. | w | r |"),
         {"D-XX" "-NEGATEDPAIR"},
         "... and the SAME words un-negated still gate (the negator is not a hole)",
         gated={"D-XX" "-NEGATEDPAIR": ""})

    # ── (i) THE LEAD-MARKER ENUMERATION MUST NEVER BE SILENT ──────────────────
    # A glyph in neither list is REPORTED, not assumed -- the EXCLUDED_HEADERS
    # pattern. Without this the third gate glyph anybody invents is silently
    # classified as prose, in the direction that flatters the cycle.
    case(_doc(*REG_HDR,
              "| `D-XX" "-NOVELLEAD` | \U0001f7e0 **OPEN** | \U0001f6a7 **BLOCKED on a "
              "decision.** | r |"),
         {"D-XX" "-NOVELLEAD"},
         "a lead marker in NEITHER list is REPORTED, never assumed to be prose",
         unclass={"D-XX" "-NOVELLEAD"})
    case(_doc(*REG_HDR,
              "| `D-XX" "-KNOWNLEAD` | \U0001f7e0 **OPEN** | ★ Implement the widening. | r |"),
         {"D-XX" "-KNOWNLEAD"},
         "a NAMED non-gate lead marker is classified, not reported")
    # star ARM 4: the row's stated blocker is gone, so it is UNBLOCKED, not gated.
    # A fired trigger under a ⏳ lead is the live shape
    # (`D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME`, and the since-retired WSL2 clock row).
    case(_doc(*REG_HDR,
              "| `D-XX" "-STALEGATE` | \U0001f7e0 **OPEN** | ⏳ NOT YET DISCHARGED -- verify "
              "first. Trigger: FIRED (measured). | r |"),
         {"D-XX" "-STALEGATE"},
         "a FIRED trigger under a gate lead is UNBLOCKED, and is REPORTED as such",
         unblock={"D-XX" "-STALEGATE"})

    def bare(keys):
        """Row keys are `relpath#name`; the cases assert on the NAME half."""
        return set(k.split("#", 1)[1] for k in keys)

    failed = 0
    for (doc, expect, why, path, expect_fatal, expect_warn,
         exp_mismark, exp_gated, exp_unclass, exp_unblock) in cases:
        s = scan_document(doc, path)
        got = bare(s.rows)
        # star SEVERITY IS ASSERTED, NOT JUST THE COUNT. FATAL means "rows exist that
        # I could not count"; WARN means "the file is malformed but nothing was lost".
        # A case that produced the right number of findings at the WRONG severity would
        # silently turn a gate failure into a note, so both are pinned.
        n_fatal = sum(1 for f in s.findings if f[2] == "FATAL")
        n_warn = sum(1 for f in s.findings if f[2] == "WARN")
        got_mismark = bare(s.mismarked)
        got_gated = dict((k.split("#", 1)[1], v[0]) for k, v in s.gated_rows.items())
        got_unclass = bare(s.unclassified)
        got_unblock = bare(s.unblocked)
        ok = (got == expect and n_fatal == expect_fatal and n_warn == expect_warn
              and got_mismark == exp_mismark
              and got_gated == exp_gated and got_unclass == exp_unclass
              and got_unblock == exp_unblock)
        if not ok:
            failed += 1
        extra = ""
        if got_unclass != exp_unclass:
            extra += "  unclassified expected=%s got=%s" % (sorted(exp_unclass),
                                                            sorted(got_unclass))
        if got_unblock != exp_unblock:
            extra += "  unblocked expected=%s got=%s" % (sorted(exp_unblock),
                                                         sorted(got_unblock))
        if (n_fatal, n_warn) != (expect_fatal, expect_warn):
            extra += "  findings expected=%d fatal/%d warn got=%d/%d" % (
                expect_fatal, expect_warn, n_fatal, n_warn)
        if got_mismark != exp_mismark:
            extra += "  mismark expected=%s got=%s" % (sorted(exp_mismark),
                                                       sorted(got_mismark))
        if got_gated != exp_gated:
            extra += "  gated expected=%s got=%s" % (sorted(exp_gated.items()),
                                                     sorted(got_gated.items()))
        print("  %-4s %-62s expected=%s got=%s%s"
              % ("ok" if ok else "FAIL", why, sorted(expect) or "-", sorted(got) or "-",
                 extra))

    # ── Pins that need MORE than one document, so they cannot ride `cases` ──
    # A one-element list rather than a `nonlocal`: this file still runs on the
    # oldest interpreter any leg offers, and the counter is the only shared state.
    extra_total = 0
    extra_failed = [0]

    def pin(ok, why, detail=""):
        extra_failed[0] += 0 if ok else 1
        print("  %-4s %-62s %s" % ("ok" if ok else "FAIL", why, detail))

    # ── (e) namespacing: the same anchor in two homes is two rows of bookkeeping ──
    a = scan_document(_doc(*REG_HDR, "| `D-XX" "-DUPROW` | ⚠ OPEN | w | r |"), REG_REL)
    b = scan_document(_doc(*DEF_HDR, "| D-XX" "-DUPROW | still open here | w | o | t |"),
                      ".plans/14-x.md")
    merged = dict(a.rows)
    merged.update(b.rows)
    pin(len(merged) == 2, "the same anchor in two homes stays two distinct rows",
        "expected=2 got=%d" % len(merged))
    extra_total += 1

    # ── (h4) AN OPENER MUST RESOLVE TO AN **OPEN** ROW ────────────────────────
    # Needs two rows
    # in one document -- the gated row and the row it points at -- so it cannot ride
    # `cases`. All three outcomes are pinned in one scan, because a test that only
    # showed the closed one would pass with `opener_state` returning "closed" always.
    d4 = scan_document(_doc(*REG_HDR,
                            "| `D-XX" "-GATEDCLOSED` | \U0001f7e0 **OPEN -- TRIGGER-GATED**, "
                            "opened by [[D-XX" "-DEADOPENER]] | w | r |",
                            "| `D-XX" "-GATEDLIVE` | \U0001f7e0 **OPEN -- TRIGGER-GATED**, "
                            "opened by [[D-XX" "-LIVEOPENER]] | w | r |",
                            "| `D-XX" "-GATEDGHOST` | \U0001f7e0 **OPEN -- TRIGGER-GATED**, "
                            "opened by [[D-XX" "-NOSUCHROW]] | w | r |",
                            "| `D-XX" "-DEADOPENER` | ✅ **CLOSED 2026-01-01** | - | r |",
                            "| `D-XX" "-LIVEOPENER` | \U0001f7e0 **OPEN** | work | r |"),
                       REG_REL)
    names4 = set(k.split("#", 1)[1] for k in d4.names)
    open4 = set(k.split("#", 1)[1] for k in d4.rows)
    states = dict((k.split("#", 1)[1], opener_state(v[0], names4, open4))
                  for k, v in d4.gated_rows.items())
    want4 = {"D-XX" "-GATEDCLOSED": "closed", "D-XX" "-GATEDLIVE": "open",
             "D-XX" "-GATEDGHOST": "dangling"}
    pin(states == want4,
        "an opener resolves to OPEN / CLOSED / DANGLING, not merely 'resolves'",
        "expected=%s got=%s" % (sorted(want4.items()), sorted(states.items())))
    pin(opener_state("", names4, open4) == "none",
        "and a row naming no opener at all is 'none'",
        "got=%s" % opener_state("", names4, open4))
    extra_total += 2

    # ── (i) AN OPENER RESOLVES IN **EITHER** NAMESPACE ────────────────────────
    # Operator ruling 2026-08-24 (argued at PLAN_PHASE_REF). Four properties, and
    # three of the four are NEGATIVE, which is where this arm's value is: the
    # positive half of a widening is trivially green, and a widening that forgot to
    # keep refusing would be indistinguishable from deleting the arm.
    #   * a plan document declares its numbered HEADINGS and its phase-table IDS,
    #     and declares neither a header word nor a separator row;
    #   * a resolvable phase reference is an opener, in both spellings;
    #   * an UNDECLARED phase, and a phase in an UNSCANNED plan, are both refused
    #     exactly like a dangling anchor id;
    #   * the anchor namespace is BYTE-FOR-BYTE unaffected -- the same three
    #     fixtures from (h4), re-resolved with a populated phase index, must give
    #     the same three verdicts.
    # ⚠ The fixture plan's own table is deliberately UNRECOGNIZED as an anchor shape
    # and carries no anchor-shaped ids, so it raises no finding -- a phase table is
    # not an anchor table and must not have to become one to be readable.
    d5 = scan_document(_doc("# A fixture plan with no number in its title",
                            "",
                            "## 2.9 A section that really exists",
                            "",
                            "| PR | Scope |",
                            "|----|-------|",
                            "| XY1 | a scheduled phase |",
                            "| SomeWord | a word, not a position in a sequence |"),
                       ".plans/99-fixture-plan.md")
    # ⚠ THE SECOND FIXTURE IS NOT DECORATION -- IT IS WHAT MAKES THE SEPARATOR PIN
    # BELOW LOAD-BEARING. Without a plan `9` that really declares a section `9`, the
    # bad split of `plan-99` would land on an empty index and read as dangling for the
    # WRONG reason, and the fixture would pass while asserting nothing. It must be
    # possible for the defect to produce a WRONG PASS, or the pin is theatre.
    d5.merge(scan_document(_doc("## 9 A section of a DIFFERENT plan"),
                           ".plans/9-other-fixture-plan.md"))
    pin(d5.plan_phases == {"99": {"2.9", "XY1"}, "9": {"9"}},
        "a plan declares its numbered headings and its phase-table ids, and nothing else",
        "got=%r" % (d5.plan_phases,))

    d5b = scan_document(_doc(*REG_HDR,
                             "| `D-XX" "-GATEDPHASE` | \U0001f7e0 **OPEN -- TRIGGER-GATED**, "
                             "opened by [[plan-99 XY1]] | w | r |",
                             "| `D-XX" "-GATEDSECTION` | \U0001f7e0 **OPEN -- TRIGGER-GATED**, "
                             "opened by [[plan-99 §2.9]] | w | r |",
                             "| `D-XX" "-GATEDNOPHASE` | \U0001f7e0 **OPEN -- TRIGGER-GATED**, "
                             "opened by [[plan-99 XY7]] | w | r |",
                             "| `D-XX" "-GATEDNOPLAN` | \U0001f7e0 **OPEN -- TRIGGER-GATED**, "
                             "opened by [[plan-98 XY1]] | w | r |",
                             # A WHOLE PLAN IS NOT A PHASE. This fixture pins the
                             # separator defect recorded at PLAN_PHASE_REF: `plan-99`
                             # names no position in any sequence, and the digits must
                             # never be split to invent one.
                             "| `D-XX" "-GATEDWHOLEPLAN` | \U0001f7e0 **OPEN -- TRIGGER-GATED**, "
                             "opened by [[plan-99]] | w | r |"),
                        REG_REL)
    names5 = set(k.split("#", 1)[1] for k in d5b.names)
    open5 = set(k.split("#", 1)[1] for k in d5b.rows)
    states5 = dict((k.split("#", 1)[1], opener_state(v[0], names5, open5, d5.plan_phases))
                   for k, v in d5b.gated_rows.items())
    want5 = {"D-XX" "-GATEDPHASE": "open", "D-XX" "-GATEDSECTION": "open",
             "D-XX" "-GATEDNOPHASE": "dangling", "D-XX" "-GATEDNOPLAN": "dangling",
             "D-XX" "-GATEDWHOLEPLAN": "dangling"}
    pin(states5 == want5,
        "a plan-phase opener resolves, and a DANGLING one is refused like a dangling row",
        "expected=%s got=%s" % (sorted(want5.items()), sorted(states5.items())))

    bare5 = dict((k.split("#", 1)[1], opener_state(v[0], names5, open5))
                 for k, v in d5b.gated_rows.items())
    pin(set(bare5.values()) == {"dangling"},
        "with NO phase index every plan-phase opener is refused -- the widening only ADDS",
        "got=%s" % sorted(bare5.items()))

    anchors5 = dict((k.split("#", 1)[1], opener_state(v[0], names4, open4, d5.plan_phases))
                    for k, v in d4.gated_rows.items())
    pin(anchors5 == want4,
        "and a populated phase index does not perturb ANCHOR resolution",
        "expected=%s got=%s" % (sorted(want4.items()), sorted(anchors5.items())))
    extra_total += 4

    # ── THE SIX-CELL REGISTRY SHAPE (2026-09-01) ─────────────────────────────
    # Every arm here would pass over a shape that was silently unrecognized -- an
    # unrecognized table contributes no rows, and "no rows" is indistinguishable from
    # "no findings" in a count. So the first case pins the COUNT, and the rest pin
    # that each predicate reads the cell the six-cell shape moved it to.
    REG6 = ["| Anchor | Priority | Status | Trigger | Closing work | Cross-refs |",
            "|---|---|---|---|---|---|"]
    s6 = scan_document(_doc(*(REG6 + [
        "| `D-XX" "-SIXOPEN` | P1 | \U0001f7e0 OPEN | \U0001f7e0 **OPEN -- normal** | w | r |",
        "| `D-XX" "-SIXSHUT` | P0 | ✅ CLOSED | ✅ **CLOSED 2026-09-01** | - | r |"])), REG_REL)
    pin(sorted(n.split("#")[1] for n in s6.names) == ["D-XX" "-SIXOPEN", "D-XX" "-SIXSHUT"]
        and sorted(n.split("#")[1] for n in s6.rows) == ["D-XX" "-SIXOPEN"]
        and not [f for f in s6.findings if f[2] == "FATAL"],
        "six-cell: the shape is RECOGNIZED and its verdict read -- an unrecognized "
        "table would contribute zero rows and zero findings, which reads as clean",
        "names=%r rows=%r" % (sorted(s6.names), sorted(s6.rows)))

    # `is_gated` and the closing-work lead marker must follow the PROSE cell, three
    # columns to the right of where they used to sit.
    s6d = scan_document(_doc(*(REG6 + [
        "| `D-XX" "-SIXGATE` | P2 | ⏳ GATED | \U0001f7e0 **OPEN -- TRIGGER-GATED: not "
        "yet** | ⛔ opened by [[D-XX" "-SIXOPEN]] | r |",
        "| `D-XX" "-SIXOPEN` | P2 | \U0001f7e0 OPEN | \U0001f7e0 **OPEN** | w | r |"])),
        REG_REL)
    pin(sorted(n.split("#")[1] for n in s6d.gated_rows) == ["D-XX" "-SIXGATE"]
        and s6d.gated_rows[REG_PREFIX + "#D-XX" "-SIXGATE"][0] == "D-XX" "-SIXOPEN",
        "six-cell: is_gated reads the TRIGGER prose and the opener the CLOSING cell, "
        "both moved right by two columns",
        "gated=%r" % {k: v[0] for k, v in s6d.gated_rows.items()})
    extra_total += 2

    # ── ARM 7: OPEN-vs-GATED, THE WORD AND NOT THE GLYPH ─────────────────────
    # ⓘ These pin the RECORDER, which is where every way of being wrong lives: the
    # differential itself (`after - before`) is three lines in main and is the same
    # shape arms 2-5 already use. What is worth a red arm is WHICH ROWS GET RECORDED,
    # because each non-recording arm below corresponds to a false contradiction this
    # predicate actually produced while it was being built.
    def gate_probe(row):
        return scan_document(_doc(*(REG6 + [row])), REG_REL).split_gate

    pin(list(gate_probe(
        "| `D-XX" "-G1` | P2 | \U0001f7e0 OPEN | ⏳ **GATED (opened by [[D-XX" "-Z]])** "
        "| w | r |")) == [REG_PREFIX + "#D-XX" "-G1"],
        "arm 7: status OPEN + prose GATED is RECORDED -- the direction that hides "
        "schedulable work behind a stale gate marker")
    pin(list(gate_probe(
        "| `D-XX" "-G2` | P2 | ⏳ GATED | \U0001f7e0 **OPEN -- do this now** | w | r |"))
        == [REG_PREFIX + "#D-XX" "-G2"],
        "arm 7: status GATED + prose OPEN is RECORDED -- the direction that invites a "
        "lane to start something the column says is blocked")
    pin(list(gate_probe(
        "| `D-XX" "-G3` | P2 | \U0001f7e0 OPEN | ⏳ **DEFERRED (surfaced 2026-06-29)** "
        "| w | r |")) == [REG_PREFIX + "#D-XX" "-G3"],
        "arm 7: DEFERRED is a THIRD SPELLING of GATED -- the 2026-06 rows use it, and "
        "a predicate knowing only two spellings would call a third of the class clean")
    pin(not gate_probe(
        "| `D-XX" "-G4` | P2 | \U0001f7e0 OPEN | ⏳ **GATED -- Trigger: ALREADY FIRED** "
        "| w | r |"),
        "arm 7: the ESCAPE -- a row declaring its trigger ALREADY FIRED is not gated by "
        "definition, and a row ABOUT gated rows must be able to name them")
    pin(not gate_probe(
        "| `D-XX" "-G5` | P2 | \U0001f7e0 OPEN | ⏳ **OPEN (still)** | w | r |"),
        "arm 7: THE WORD, NOT THE GLYPH -- ⏳ against a 🟠 column agrees in words and "
        "misleads nobody; 304 of 490 rows skew by glyph where only 65 skewed by word")
    pin(not gate_probe(
        "| `D-XX" "-G6` | P2 | \U0001f7e0 OPEN | \U0001f534 **PARTIALLY CLOSED 2026-08-06 "
        "-- THE DERIVATION HALF REMAINS OPEN** | w | r |"),
        "arm 7: a PARTIAL-CLOSURE row is not a contradiction -- reading past the "
        "qualifier is the false positive that narrowed this predicate to one word")
    pin(not gate_probe(
        "| `D-XX" "-G7` | P2 | ✅ CLOSED | ⏳ **GATED** | w | r |"),
        "arm 7: a CLOSED row is OUT OF SCOPE here -- the door owns the closed-vs-not axis "
        "(`anchors.triggerCarriesVerdict`; ARM 6 until 2026-09-30), and two owners of one "
        "row's defect would name it twice")
    pin(list(gate_probe(
        "| `D-XX" "-G8` | P2 | ⏳ GATED | \U0001f7e0 **OPEN -- Trigger: ALREADY FIRED** "
        "| w | r |")) == [REG_PREFIX + "#D-XX" "-G8"],
        "arm 7: THE ESCAPE IS DIRECTIONAL -- a fired trigger excuses PROSE that says "
        "GATED, never a COLUMN that does; undirected, it exonerated every new row at "
        "once because every row this project mints declares its trigger fired")
    pin(list(gate_probe(
        "| `D-XX" "-G9` | P2 | \U0001f535 DISCLOSED | ⏳ **GATED (opened by [[D-XX" "-Z]])** "
        "| w | r |")) == [REG_PREFIX + "#D-XX" "-G9"],
        "arm 7: the door's DISCLOSED cell states OPEN -- beside GATED prose it is RECORDED; "
        "before `DISCLOSED` was a verdict word the cell stated nothing and ARM 7 was blind "
        "to every disclosed row")
    pin(not gate_probe(
        "| `D-XX" "-G10` | P2 | \U0001f535 DISCLOSED | \U0001f7e0 **OPEN** debt that pre-dates "
        "the cycle | w | r |"),
        "arm 7: CONTROL -- a DISCLOSED row whose prose reads OPEN agrees, and is not recorded")
    extra_total += 10

    # ── ARM 8: A `[[...]]` POINTER MUST NAME A ROW ────────────────────────────
    # The recorder (which targets are pointers, in which documents), the differential
    # (`pointer_verdict`) and the report `main` returns (`pointer_report`), each with its
    # negative SYNTHESIZED: every refusal below is a pair the arm could have produced and
    # must not, or must and did.
    def reg(rel, *rows):
        return scan_document(_doc(*(REG6 + list(rows))), rel)
    six = "| `%s` | P2 | %s | %s | %s | %s |"
    done_rel = REG_PREFIX + "-done.md"
    pa, pb, pc, pz = "D-XX" "-PTRA", "D-XX" "-PTRB", "D-XX" "-PTRC", "D-XX" "-PTRGONE"
    open_, shut = "\U0001f7e0 OPEN", "✅ CLOSED"
    s8 = reg(REG_REL, six % (pa, open_, "\U0001f7e0 **OPEN** see [[%s]]" % pb, "w [[%s]]" % pz,
                             "[[feedback-a-memory-link]] · [[plan-22 OPT8]] · `%s`" % pz))
    pin(s8.pointers.get(REG_PREFIX + "#" + pa) == {pb, pz},
        "arm 8: a registry row's pointers are read from EVERY cell, and only an anchor-shaped "
        "target is one -- a memory link, a plan phase and a backticked mention are not",
        "pointers=%r" % s8.pointers)
    s8p = scan_document(_doc(*DEF_HDR, "| D-XX" "-PLPTR | see [[%s]] | why | owner | trig |" % pz),
                        ".plans/22-x.md")
    pin(not s8p.pointers and ".plans/22-x.md#D-XX" "-PLPTR" in s8p.names,
        "arm 8: only a REGISTRY row's pointers are judged -- a plan document's table is read (its row "
        "counted) and is not a registry",
        "pointers=%r names=%r" % (s8p.pointers, sorted(s8p.names)))
    short = "D-XX" "-PTR"
    both = Scan().merge(reg(REG_REL, six % (pa, open_, "\U0001f7e0 **OPEN** [[%s]] [[%s]] [[%s]] [[%s]]"
                                          % (pb, pz, pb + "X", short), "w", "r")))
    both.merge(reg(done_rel, six % (pb, shut, "✅ **CLOSED 2026-01-01**", "-", "r")))
    pin(dangling_pointers(both) == {(pa, pz), (pa, pb + "X"), (pa, short)},
        "arm 8: a target resolves EXACTLY, against the rows of BOTH registries -- a closed row "
        "resolves; a name no row has, a longer id than a row's and a shorter one do not",
        "dangling=%r" % sorted(dangling_pointers(both)))
    step = Scan().merge(reg(REG_REL, six % (pa, open_, "\U0001f7e0 **OPEN** [[%s.2]]" % pb, "w", "r"),
                            six % (pb, open_, "\U0001f7e0 **OPEN**", "w", "r")))
    pin(dangling_pointers(step) == {(pa, pb + ".2")},
        "arm 8: a plan-step id (an id, a dot, a step) never resolves -- no row id carries a dot, "
        "even where the id before the dot is a row",
        "dangling=%r" % sorted(dangling_pointers(step)))
    row_a = six % (pa, open_, "\U0001f7e0 **OPEN** [[%s]] and [[%s]]" % (pz, pb), "w", "r")
    row_b = six % (pb, shut, "✅ **CLOSED 2026-01-01**", "-", "r")
    base8 = Scan().merge(reg(REG_REL, row_a)).merge(reg(done_rel, row_b))
    gone = Scan().merge(reg(REG_REL, row_a)).merge(reg(done_rel))
    refused8, debt8 = pointer_verdict(base8, gone)
    pin(refused8 == [(pa, pb)] and debt8 == [(pa, pz)],
        "arm 8: a pointer that dangled at the base too is DEBT; one whose TARGET the change deleted "
        "is REFUSED, though its own row was never edited",
        "refused=%r debt=%r" % (refused8, debt8))
    added = Scan().merge(reg(REG_REL, row_a, six % (pc, open_, "\U0001f7e0 **OPEN** [[%s]]" % pz,
                                                  "w", "r"))).merge(reg(done_rel, row_b))
    refused8, debt8 = pointer_verdict(base8, added)
    pin(refused8 == [(pc, pz)] and debt8 == [(pa, pz)],
        "arm 8: a row the change ADDED, pointing at nothing, is refused -- beside the debt it inherits",
        "refused=%r debt=%r" % (refused8, debt8))
    gated = "| `%s` | P2 | ⏳ GATED | \U0001f7e0 **OPEN -- TRIGGER-GATED: not yet** | ⛔ opened by [[%s]] | r |"
    opener = Scan().merge(reg(REG_REL, row_a, gated % (pc, pz))).merge(reg(done_rel, row_b))
    refused8, debt8 = pointer_verdict(base8, opener)
    pin(refused8 == [] and debt8 == [(pa, pz)]
        and opener.gated_rows.get(REG_PREFIX + "#" + pc, ("",))[0] == pz,
        "arm 8: the opener of a gated row the change ADDED is ARM 3's to refuse, never a second time "
        "here -- the row is recorded gated, its opener read",
        "refused=%r gated=%r" % (refused8, {k: v[0] for k, v in opener.gated_rows.items()}))
    # The REPORT `main` prints and the verdict it returns, both ways.
    row_d = six % (pb, shut, "✅ **CLOSED 2026-01-01** [[%s]]" % (pz + "D"), "-", "r")
    debt_only = Scan().merge(reg(REG_REL, row_a)).merge(reg(done_rel, row_d))
    said = []
    refused_flag = pointer_report(debt_only, debt_only, "BASE", said.append)
    text8 = "\n".join(said)
    pin(refused_flag is False
        and ("anchor-debt: DEBT - 2 `[[...]]` pointer(s) in 2 row(s) name no row of either registry "
             "(pre-existing at BASE, not refused here): 1 in %s, 1 in %s" % (done_rel, REG_REL)) in text8
        and ("  ~ %s   -> [[%s]]" % (pa, pz)) in text8 and ("-> [[%s]]" % (pz + "D")) not in text8
        and "FAIL" not in text8,
        "arm 8: the report states the debt per registry, lists the PENDING registry's pairs and counts "
        "the archive's, and refuses nothing",
        repr(said))
    said = []
    refused_flag = pointer_report(base8, gone, "BASE", said.append)
    text8 = "\n".join(said)
    pin(refused_flag is True and ("  ! POINTER RESOLVES TO NOTHING  %s   -> [[%s]]" % (pa, pb)) in text8
        and "anchor-debt: FAIL - 1 `[[...]]` pointer(s) this change introduced" in text8,
        "arm 8: a pointer the change left dangling is REFUSED by the report -- the verdict `main` "
        "returns, with the pair and what to do about it",
        repr(said))
    extra_total += 9

    # ── (s) THE BASELINE IS READ FROM THIS TREE'S GIT, whatever the caller exported ──
    # ✔MEASURED 2026-09-15 (P66 lane rr): under another repository's GIT_DIR, `scan_at_ref`
    # read THAT repository's `.plans` at HEAD. A synthetic repository commits one registry row;
    # the steered negative is proven by a bare `git ls-tree` naming the decoy's plan document.
    ot = _owning_tree()
    open6 = "| `D-XX-%s` | P2 | \U0001f7e0 OPEN | \U0001f7e0 **OPEN** | w | r |"
    fx = tempfile.mkdtemp(prefix="anchor-debt-steer-")
    real, clean_keys, steered_keys, why = False, [], None, ""
    try:
        os.makedirs(os.path.join(fx, PLANS_DIR))
        with io.open(os.path.join(fx, REG_REL), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(_doc(*(REG6 + [open6 % "STEEROWN"])) + "\n")
        for args in (["init", "-q", fx], ["-C", fx, "add", "-A"],
                     ["-C", fx, "-c", "user.email=anchor-debt@example.invalid",
                      "-c", "user.name=anchor-debt", "-c", "commit.gpgsign=false",
                      "commit", "-q", "--no-verify", "-m", "fixture"]):
            ot.run_git(args, capture_output=True)
        clean_keys = sorted(scan_at_ref(fx, "HEAD").rows)
        with ot.steering() as steer:
            neg = ot.bare_git(["ls-tree", "--name-only", "HEAD:" + PLANS_DIR], fx, steer)
            real = os.path.basename(ot.STEER_PLANS_DECOY) in neg.stdout.split("\n")
            with ot.caller_environment(steer):
                try:
                    steered_keys = sorted(scan_at_ref(fx, "HEAD").rows)
                except SystemExit as exc:
                    why = str(exc)
    finally:
        ot.remove_tree(fx)
    pin(real and any(k.endswith("#D-XX" "-STEEROWN") for k in clean_keys)
        and steered_keys == clean_keys,
        "baseline: `scan_at_ref` IGNORES a caller's GIT_DIR + GIT_WORK_TREE + GIT_INDEX_FILE "
        "naming another repository -- it reads this tree's own ref",
        "negative-synthesized=%s clean=%r steered=%r %s"
        % (real, clean_keys, steered_keys, why[:120]))
    extra_total += 1

    # ── (r) THE ROOT IS THE TREE THIS FILE LIVES IN, whatever the caller's cwd ──
    # ★ `--self-test` is the form ctest runs, and it never reaches `repo_root()` on its
    # own -- so without these arms the only gate that runs this file could not see a
    # root taken from the caller's working directory. Arms, oracle and synthesized
    # negatives are owned by .harness-config/runner/actions/owning-tree/owning-tree.py.
    for ok, why, detail in _owning_tree().root_arms(repo_root, (SystemExit,), True, __file__):
        pin(ok, why, "" if ok else detail)
        extra_total += 1

    failed += extra_failed[0]
    total = len(cases) + extra_total
    # ★ AN EXACT RATCHET (the round-12 audit: the count was printed and never checked, so a lost arm passed).
    if total != EXPECTED_CASES:
        failed += 1
        print("self-test: CASE COUNT %d, expected %d -- EXPECTED_CASES is the ratchet" % (total, EXPECTED_CASES))
    print("self-test: %d case(s), %d failed" % (total, failed))
    return 1 if failed else 0


# ──────────────────────────────────── main ─────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default="HEAD",
                    help="the git ref a change is judged against: a row that already existed there is DEBT, one the "
                         "change added or edited is refused (default HEAD)")
    ap.add_argument("--self-test", action="store_true",
                    help="check the instrument, not the registry")
    args = ap.parse_args()

    # ⓘ The stream reconfiguration that used to sit here now runs at IMPORT, above
    # the constants -- see the block there for the measurement that moved it and
    # for why `errors="replace"` alone erased the fact this guard reports.

    if args.self_test:
        return self_test()

    root = repo_root()
    before = scan_at_ref(root, args.base)
    after = scan_worktree(root)
    findings = list(after.findings)

    print("anchor-debt: the registries read at %s and now; the balance and the registry partition are "
          "`dssharness check-anchor-balance`'s, and the Status/Trigger split is the door's"
          % args.base)

    # (A bookkeeping closure since the base, `✅🧾`, was listed here as NOT PROGRESS until 2026-10-06: DssHarness 0.6.7's
    # balance reports it itself, `N closed (K bookkeeping)`, each one `[bookkeeping: not credited]` -- see the header's
    # note on the pair.)

    # == ARM 2: A ROW WHOSE OPENING VERDICT CONTRADICTS ITS OWN MARKER =========
    # star A DIFFERENTIAL, NOT A HAND-KEPT INVENTORY, and the reason is the one
    # `check-diagnostic-codes` already records: a hand-kept pin can only check the
    # rows somebody remembered to add to it, and it must then be edited in lockstep
    # with a file it does not own. Both refs are already scanned here, so the
    # pre-existing population defines itself and a REPAIR can never leave a stale
    # entry behind. MEASURED 2026-08-23 at cf27fe8b with THIS predicate: **7**
    # pre-existing, printed as DEBT on every run so the number cannot quietly
    # become the normal state.
    # ⚠ This sentence said **10** until the P28 step-10 audit re-measured it.
    # Three independent readings say 7: the guard's own DEBT line before the
    # sweep, the row that asked for this arm,
    # and the 7 marks the sweep actually landed. A wrong number inside the
    # instrument is the shape this file's own docstring warns about three times
    # -- and it reached the one place a later cycle would trust without
    # re-measuring. Re-derive it, never re-quote it.
    new_mismarked = sorted(k for k in after.mismarked if k not in before.mismarked)
    old_mismarked = sorted(k for k in after.mismarked if k in before.mismarked)
    if old_mismarked:
        print()
        print("anchor-debt: DEBT - %d row(s) open with a CLOSURE VERDICT while their "
              "marker says otherwise (pre-existing at %s, not this cycle's doing):"
              % (len(old_mismarked), args.base))
        # ⚠ THE CLAIM IS CONDITIONED, BECAUSE ONE SENTENCE DESCRIBED TWO SHAPES AND
        # WAS TRUE OF ONLY ONE. In the FOUR-cell shape the prose IS the status cell, so a
        # closure verdict under a non-closure marker genuinely counts OPEN and the
        # population really is inflated. In the SIX-cell registry shape `Status` decides
        # and the prose glyph moves nothing -- the row is counted CLOSED and the only
        # defect is that two cells disagree, which the door refuses on its own terms (ARM 6's rule).
        # ✔MEASURED 2026-09-08: three six-cell rows sat here while the registry read
        # 451 open; repairing their prose left the count at 451, exactly as this split
        # now predicts. An instrument that overstates its own skew teaches a later cycle
        # to distrust a number that was right. (This reporting TEXT is not pinned by
        # self_test, which pins `scan_document` rather than main()'s printing -- stated
        # rather than quietly assumed.)
        counted = [k for k in old_mismarked if k in after.rows]
        for k in old_mismarked:
            print("  ~ %s%s   %s"
                  % (k, "" if k in after.rows else "  [Status column already reads CLOSED]",
                     after.mismarked[k]))
        if counted:
            print("  %d of them COUNT OPEN, so the population is OVER-reported by that "
                  "many until the mark is repaired -- with the bookkeeping pair `✅🧾` when "
                  "the work predates this cycle, a closure `dssharness check-anchor-balance` "
                  "counts but credits the cycle nothing for -- or the opening verdict is "
                  "reworded." % len(counted))
        if len(counted) != len(old_mismarked):
            print("  %d carry a `Status` column that already reads CLOSED, so they are "
                  "counted CLOSED and the population is NOT skewed by them; reword the "
                  "opening verdict so the two cells stop disagreeing."
                  % (len(old_mismarked) - len(counted)))

    # == ARM 3: A GATED ROW MUST NAME THE ROW THAT OPENS IT ====================
    # Operator ruling 2026-08-23. Differential for the same reason as arm 2 -- and
    # the census makes it mandatory rather than merely tidy: 62 open gated rows at
    # cf27fe8b, essentially none carrying an opener, so a day-one refusal would red
    # the tree for every lane over debt none of them created.
    #
    # ★★★ AND THE OPENER MUST RESOLVE TO AN **OPEN** ROW, NOT MERELY TO A ROW --
    # it used to accept an opener that is already closed. The arm used to
    # check resolution against `after.names`, which holds every data row open OR
    # CLOSED, so `opened by [[X]]` was satisfied by an X that finished cycles ago.
    # **A CLOSED ROW CANNOT OPEN ANYTHING.** ✔MEASURED 2026-08-23 over the worktree:
    # of the 11 gated rows that name an opener, **9 name a row that is already ✅
    # CLOSED** -- the three `D-FULLC-STDBIT-*` rows, three C-attribute (`CSUBSET-ATTRIBUTE-*`)
    # rows, two `D-CSUBSET-CONSTEXPR-*` rows and `D-CSUBSET-TYPEOF-UNQUAL-GNU-SPELLING`
    # -- so nine rows satisfied the SYNTAX the operator's ruling asked for while
    # carrying no live dependency at all, each one's real gate stated only in prose.
    # ⓘ THE REFERENCE IS NOT DELETED AND MUST NOT BE: it records which cycle CO-OPENED
    # the row, which is genuine history. What changes is the VERDICT -- a gated row
    # whose opener has closed is not opened, it is **UNBLOCKED**, and it belongs in
    # ARM 4 so somebody re-verdicts it instead of leaving it gated forever.
    unopened, dangling, closed_opener = [], [], []
    known = set(n.split("#", 1)[1] for n in after.names)
    known_open = set(n.split("#", 1)[1] for n in after.rows)
    # ⓘ BOTH namespaces resolve against `after`, never against the base ref, and for
    # the same reason: an opener is a claim about what will open the row NEXT, so it
    # has to resolve in the tree as it stands now. `before.plan_phases` is built and
    # deliberately unused -- deleting a phase from a plan must red the rows citing it.
    unblocked = dict(after.unblocked)
    for k in sorted(after.gated_rows):
        opener, excerpt = after.gated_rows[k]
        # `after.rows` holds OPEN rows only, so a name in `known` but not in
        # `known_open` is a row every one of whose homes is CLOSED.
        state = opener_state(opener, known, known_open, after.plan_phases)
        if k in before.gated_rows:
            # Pre-existing: DEBT either way. A verdict that went stale under its
            # author is not a contradiction the author wrote.
            if state == "closed":
                unblocked[k] = "opener [[%s]] is already CLOSED" % opener
            continue
        if state == "closed":
            closed_opener.append((k, opener))
        elif state == "none":
            unopened.append((k, excerpt))
        elif state == "dangling":
            dangling.append((k, opener))
    stale_gated = [k for k in after.gated_rows if k in before.gated_rows]
    if stale_gated:
        print()
        print("anchor-debt: DEBT - %d gated row(s) predate %s; %d of them name no "
              "opener." % (len(stale_gated), args.base,
                           sum(1 for k in stale_gated if not after.gated_rows[k][0])))

    # == ARM 4: A ROW WHOSE STATED BLOCKER IS GONE IS **UNBLOCKED**, NOT OPENED =====
    # Two detectors, one finding: the row still READS as blocked while the thing it
    # names as blocking it has been discharged. Advisory by construction -- nothing
    # here is a contradiction the author introduced, it is a verdict that has gone
    # stale under them, and the remedy is a re-read rather than an edit to this gate.
    # ⚠ WITHOUT THIS ARM BOTH SHAPES VANISH SILENTLY: a closed-opener row satisfies
    # arm 3 and a fired-trigger row is exonerated by `is_gated`, so in both cases the
    # gate goes quiet on a row that is now actionable and nobody is told.
    if unblocked:
        print()
        print("anchor-debt: DEBT - %d row(s) still present as blocked while the "
              "blocker they name is already discharged (UNBLOCKED, not opened):"
              % len(unblocked))
        for k in sorted(unblocked):
            print("  = %s   %s" % (k, unblocked[k]))
        print("  Re-verdict each: it is schedulable NOW. A gate whose opener has closed "
              "and a gate whose trigger has fired are both work waiting to be picked "
              "up, not deferrals -- leaving the marker in place hides them from the "
              "queue for as long as nobody re-reads the row.")

    # == ARM 5: A CLOSING-WORK LEAD MARKER THIS INSTRUMENT CANNOT CLASSIFY =========
    # The residual of the GATE_LEAD_MARKS / NON_GATE_LEAD_MARKS enumeration, made
    # loud for the reason EXCLUDED_HEADERS is: an enumeration is only safe when what
    # falls outside it is REPORTED rather than assumed. A glyph in neither list would
    # otherwise be silently read as "not a gate", which is the direction that flatters
    # the cycle -- the exact failure this instrument's first version shipped.
    # ★ DIFFERENTIAL FOR THE SAME REASON THE OTHER ARMS ARE: a novel glyph on a row
    # somebody wrote months ago is DEBT, but one on a row THIS cycle touched is a
    # measurement this run could not make, and that fails.
    new_unclass = sorted(k for k in after.unclassified if k not in before.unclassified)
    old_unclass = sorted(k for k in after.unclassified if k in before.unclassified)
    if old_unclass:
        print()
        print("anchor-debt: DEBT - %d row(s) lead their closing-work cell with a "
              "marker this gate cannot classify (pre-existing at %s):"
              % (len(old_unclass), args.base))
        for k in old_unclass:
            print("  ? %s   %r  %s" % (k, after.unclassified[k][0],
                                       after.unclassified[k][1]))

    # ⚠ FAIL LOUD ON AN UNPARSED TABLE. A silently skipped anchor table is the exact
    # defect of counting only the registry, so an unknown shape that
    # LOOKS like an anchor table fails the run even when the balance itself is fine.
    # Fix it by teaching SHAPES_* the new shape, or by NAMING it in EXCLUDED_HEADERS
    # after reading it -- never by deleting this check.
    warns = [f for f in findings if f[2] == "WARN"]
    fatals = [f for f in findings if f[2] == "FATAL"]

    # Printed on EVERY run, pass or fail: the file is malformed, but the rows were
    # counted, so the denominator is whole and the balance verdict stands.
    if warns:
        print()
        print("anchor-debt: WARN - %d malformed structure(s); rows were COUNTED, "
              "but the markdown is wrong:" % len(warns))
        for rel, line_no, _sev, what in warns:
            print("  %s:%d   %s" % (rel, line_no, what))
        print("  TABLE BODY INTERRUPTED -> move the HTML comment above or below the "
              "table. Not a gate failure: nothing was lost from the count.")

    # ⚠ FAIL LOUD WHEN THE MEASUREMENT ITSELF IS INCOMPLETE. A silently skipped anchor
    # table is the exact defect of counting only the registry, so an
    # unknown shape or an orphan row fails the run even when the balance is fine.
    # Fix it by teaching the tool the shape, or by NAMING it in EXCLUDED_HEADERS after
    # reading it -- never by deleting this check.
    if fatals:
        print()
        print("anchor-debt: FAIL - %d anchor-bearing structure(s) this gate could "
              "NOT COUNT:" % len(fatals))
        for rel, line_no, _sev, what in fatals:
            print("  %s:%d   %s" % (rel, line_no, what))
        print("  UNRECOGNIZED TABLE SHAPE -> teach SHAPES_EXACT/SHAPES_PREFIX the shape, "
              "or NAME it in EXCLUDED_HEADERS after reading it.")
        print("  ORPHAN ANCHOR ROW -> the row has drifted out of its table (broken "
              "separator or stray blank line); put it back.")
        print("  Do NOT let either be skipped - a silently skipped row is the exact "
              "defect this gate exists to stop.")
        return 1

    # == ARM 8: A `[[...]]` POINTER MUST NAME A ROW ============================
    # Reported only once the scan is known WHOLE (the fatals above), because a pointer
    # resolves against rows and a row this run could not count would read as a dangle.
    # The differential, its one exclusion and this report live in `pointer_verdict` and
    # `pointer_report`, where the self-test pins them -- the verdict here is not left to
    # the unpinned body of `main`.
    pointer_refused = pointer_report(before, after, args.base)

    # FAIL LOUD ON A **NEW** SELF-CONTRADICTING OR UNOPENED GATED ROW. Both are
    # measurement corruption rather than untidiness: a mis-marked closure inflates
    # the OPEN denominator with a row that is already closed, and a gated row with
    # no opener is a deferral nothing can ever discharge -- "a precondition with no
    # cause attached is indistinguishable from a permanent block" (operator,
    # 2026-08-23). Neither is fixable by widening this gate.
    if new_mismarked or unopened or dangling or closed_opener or new_unclass:
        print()
        print("anchor-debt: FAIL - %d row(s) this cycle added or edited are "
              "self-contradicting:"
              % (len(new_mismarked) + len(unopened) + len(dangling)
                 + len(closed_opener) + len(new_unclass)))
        for k in new_mismarked:
            print("  ! MIS-MARKED CLOSURE  %s   %s" % (k, after.mismarked[k]))
        for k, excerpt in unopened:
            print("  ! GATED, NO OPENER    %s   %s" % (k, excerpt))
        for k, opener in dangling:
            print("  ! OPENER RESOLVES TO NOTHING  %s   -> [[%s]]" % (k, opener))
        for k, opener in closed_opener:
            print("  ! OPENER IS ALREADY CLOSED    %s   -> [[%s]]" % (k, opener))
        for k in new_unclass:
            print("  ! UNCLASSIFIED LEAD MARKER    %s   %r  %s"
                  % (k, after.unclassified[k][0], after.unclassified[k][1]))
        if new_mismarked:
            print("  MIS-MARKED CLOSURE -> the cell's first word is a closure verdict "
                  "but its leading marker is not the closure mark. Either mark it closed "
                  "(lead the cell with the closure mark followed by the bookkeeping mark "
                  "when the work predates this cycle -- `dssharness check-anchor-balance` "
                  "then credits the cycle nothing for that closure), or reword the "
                  "opening so it stops claiming a closure it did not make.")
        if unopened or dangling or closed_opener:
            print("  A GATED ROW NAMES ITS OPENER as `opened by [[...]]`, reusing the "
                  "registry's own link form. The opener is a TYPED reference and must "
                  "RESOLVE IN THE NAMESPACE IT NAMES -- either an ANCHOR ROW (`D-XX" "-OPENER`, "
                  "which must resolve to an OPEN row) or a PLAN PHASE (`plan-22 OPT8`, "
                  "`plan-16 CS1`, `plan-27 §11`, which must resolve to a real numbered "
                  "heading or phase-table id in that plan). A gate whose opener is an "
                  "unowned event is unfalsifiable and will sit forever, and one whose "
                  "opener has ALREADY CLOSED is not gated at all -- it is unblocked, and "
                  "saying otherwise hides schedulable work. A gate whose opener is an open "
                  "ROW or a scheduled PHASE is a dependency -- owned, sizable, and visible "
                  "in the queue. DO NOT mint a placeholder row to satisfy this: a row that "
                  "is not a defect, existing only to be pointed at, is registry pollution.")
            print("  IF THIS ROW IS *ABOUT* GATED ROWS RATHER THAN BEING ONE -- a census, "
                  "a rule, a report that has to quote the vocabulary -- do NOT reword it "
                  "to dodge this check. A row must be able to name the thing it is about. "
                  "State its own verdict instead, in whichever of these is TRUE: its "
                  "trigger has ALREADY FIRED, or it has no trigger (`Trigger: none`), or "
                  "it has no closing work. Each is a claim a reader can check.")
        if new_unclass:
            print("  UNCLASSIFIED LEAD MARKER -> the closing-work cell opens with a "
                  "glyph this gate has never been taught. It will NOT guess: add the "
                  "marker to GATE_LEAD_MARKS if it means *do not build this yet*, or to "
                  "NON_GATE_LEAD_MARKS if it is emphasis or a verdict -- after reading "
                  "the row. Do not delete this check: an enumeration whose residual is "
                  "silent is how this instrument under-counted four times before.")
        return 1

    # == ARM 7: OPEN-vs-GATED, A RATCHET RATHER THAN A DAY-ONE REFUSAL ========
    # Shaped like arms 2-5: the whole-population figure is reported as DEBT, and only
    # rows THIS CYCLE ADDED OR EDITED are refused. See the recorder above for why this
    # cannot be folded into ARM 6. The escape is the same one ARM 6's sibling already
    # offers -- a row that declares `Trigger: ALREADY FIRED` or `Trigger: none` is not
    # gated by definition, and a row ABOUT gated rows must be able to name them.
    new_gate_skew = sorted(k for k in after.split_gate if k not in before.split_gate)
    old_gate_skew = sorted(k for k in after.split_gate if k in before.split_gate)
    if old_gate_skew:
        print("anchor-debt: DEBT - %d row(s) predating %s disagree with their own "
              "trigger about OPEN vs GATED (inherited; not refused here)."
              % (len(old_gate_skew), args.base))
    if new_gate_skew:
        print()
        print("anchor-debt: FAIL - %d row(s) this cycle added or edited state one "
              "verdict in their `Status` column and the OTHER in their `Trigger` prose:"
              % len(new_gate_skew))
        for k in new_gate_skew:
            col, prose = after.split_gate[k]
            print("  ! %s\n      Status column: %s\n      Trigger prose: %s"
                  % (k, col, prose))
        print("  BOTH DIRECTIONS HIDE WORK, which is why this is refused and not noted:")
        print("  a row whose prose reads GATED is skipped by every read-by-eye queue even")
        print("  though the column says it is schedulable; a row whose prose reads OPEN")
        print("  while the column says GATED invites a lane to start something blocked.")
        print("  Decide which half is TRUE. If the row is genuinely gated, correct the")
        print("  COLUMN (which also moves the row):")
        print("      dssharness set-anchor <anchor> --status gated")
        print("  If the gate has fired or was never a gate, reword the PROSE's leading")
        print("  verdict -- and say WHY in the row, so the next cycle does not undo it.")
        print("  A row that is ABOUT gated rows may declare `Trigger: ALREADY FIRED`.")
        return 1

    if pointer_refused:
        return 1

    print("anchor-debt: OK -- nothing a change added or edited is refused; every DEBT line above was "
          "already there at %s" % args.base)
    return 0


if __name__ == "__main__":
    sys.exit(main())
