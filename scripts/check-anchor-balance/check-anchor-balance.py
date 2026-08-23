#!/usr/bin/env python3
# PURPOSE: refuse a cycle that ends with more OPEN deferral-registry rows than it began.
"""Anchor balance gate: a cycle may not end with more OPEN deferral rows than it began.

WHY THIS IS A SCRIPT AND NOT AN INLINE GREP
-------------------------------------------
The first version of this gate was an inline
    grep -cE '\\| `D-[A-Z0-9-]+` \\| (RED|ORANGE|WARN)'
which ENUMERATED the glyphs that mean open. It was blind to the hourglass, and the
hourglass is what `D-OPT6-LICM-SPECULATIVE-LOAD-HOIST` carried -- so the gate saw
269 open rows where there were 579, and a cycle could close one orange row, open one
hourglass row, and be congratulated for it.

So the rule here is INVERTED and that inversion is the whole point: a row is OPEN
unless its status cell carries an explicit CLOSED marker. A glyph nobody has thought
of yet counts as open, which is the safe direction -- the gate over-reports rather
than waving work through. Enumerating the open glyphs is the same mistake as
enumerating build-directory layouts: define the complement, not the variants.

The comparison is by ROW NAME, not by count, because "which rows moved" is the
question a reader actually has, and a count cannot answer it.

star star star WHAT THE DENOMINATOR IS, AND WHY IT IS NOT JUST THE REGISTRY (2026-08-13)
------------------------------------------------------------------------------------
`dss-cycle` SKILL.md section F.2 sanctions MORE THAN ONE HOME for a deferral:
a feature-area anchor belongs in its plan's own deferred-items table, a project-level
known-open item in plan-00 section 0.2, an orphan/cross-cutting anchor in
`_deferred-anchor-registry.md`.  Section F.4 then lets a `src/` citation resolve to
EITHER home.  This tool used to count ONLY registry rows -- so a cycle that closed a
registry row and deferred the actual work into a plan-side row was reported as an
IMPROVEMENT.  That is `D-GATE-BALANCE-COUNTS-ONLY-THE-REGISTRY`, and it is the THIRD
time this instrument has flattered the cycle (see the direction note in is_closed()).

The fix is to count EVERY SANCTIONED HOME, so that MOVING a deferral between homes is
arithmetically NEUTRAL instead of looking like progress.  ✔MEASURED 2026-08-13: the
plan-side homes hold 378 distinct anchors, only 8 of which also have a registry row --
so this is a real population, not a rounding error.

star THE ONE-TIME JUMP IS DECOMPOSED, NEVER SILENT.  Widening a denominator makes the
headline number leap for entirely historical reasons.  The report therefore always
prints registry-side and plan-side counts SEPARATELY and names which denominator
gated, so the jump is attributable rather than mysterious.  Note that the gate itself
does NOT need a stored baseline: both sides of the comparison are counted with the
same rule, so widening changes the HEADLINE but not the DELTA.  `--denominator
registry` reproduces the old headline on demand.

star HOW TABLES ARE RECOGNIZED -- BY HEADER SHAPE, NEVER BY HEADING NUMBER.
The obvious implementation is "parse the table under each plan's section 3.1".  It is
wrong, and ✔MEASURED wrong on this tree:
  - `17-shader-gpu-plan` keeps its anchor table under section **5.4**, not 3.1;
  - `09.5`, `24`, `28` keep theirs under sections **9 / 6 / 12**;
  - `17-shader-gpu-plan` section **3.1 is not an anchor table at all** (it is a
    "Tier | Example | External tools?" prose table);
  - `23-full-c-plan` section **3.1 is not even a table** -- it is a prose paragraph
    that happens to name a dozen anchors.
Heading numbers are decoration; the COLUMN SHAPE is the contract.  So tables are
matched on their normalized header cells, and anything unrecognized that still looks
like an anchor table is REPORTED AND FAILS THE RUN (the severity rule lives in
scan_document's docstring: FATAL iff the MEASUREMENT is incomplete) -- a silently
skipped table is the exact defect this whole anchor is about.
"""
import argparse
import io
import os
import re
import subprocess
import sys

PLANS_DIR = ".plans"
REG_REL = ".plans/_deferred-anchor-registry.md"

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
# `D-GATE-BALANCE-EXEMPTS-A-DISCLOSED-OPENING-BUT-NOT-A-BOOKKEEPING-CLOSURE`.
# The disclosed mark says "this OPEN row's debt predates this cycle". Nothing said
# the same thing about a CLOSURE. A row whose work finished cycles ago but whose
# status cell never got a readable closed marker counts OPEN at the base ref, so
# repairing that glyph today registers as a CLOSURE BY THIS CYCLE and SUBTRACTS
# from its net -- while closing nothing. That rewards bookkeeping over work, which
# is the fourth shape of the same failure the direction note in is_closed()
# already records three of.
#
# star THE ARITHMETIC IS NET-NEUTRAL, AND "NEUTRAL" IS THE WHOLE SPECIFICATION.
# A bookkeeping closure does BOTH of these and they must not be separated:
#   * it REDUCES the OPEN population by one -- the row really is closed, and the
#     denominator must stop counting it, or the gate keeps over-reporting;
#   * it ADDS one back to the cycle's net -- so the cycle is credited with
#     NOTHING for it.
# Pinned in self_test() by `_case_net_neutral`, which runs the real arithmetic
# over synthetic before/after documents and asserts the net is UNCHANGED.
#
# star IT IS SPELLED **AFTER** THE CLOSURE MARK, NOT BEFORE, AND THAT IS A
# DELIBERATE ORDERING RATHER THAN A TASTE. is_closed() is left BYTE-IDENTICAL by
# this change, so every row that carries no new marker produces exactly the number
# it produced before -- the property a change to this instrument owes the tree,
# because the alternative is discovering later that the headline moved for a
# reason nobody isolated. A leading `🧾` would have forced is_closed() to learn a
# second lead-in and put every historical count at risk for no gain.
# ⚠ LEADING POSITION IS LOAD-BEARING, exactly as it is for is_disclosed: the pair
# must be the FIRST thing in the cell. A mark accepted mid-prose could be claimed
# by any row that merely writes about bookkeeping, which is the "satisfied by a
# mention" failure this registry has already recorded twice.
# star AND IT IS A CHECKABLE CLAIM, NOT A FORMATTING CHOICE: it asserts the work
# PRE-DATES this cycle, so a reviewer can look for it in the base ref. Marking a
# closure you actually earned is a false statement about history.
BOOKKEEPING_MARK = "🧾"  # receipt: the mark is being REPAIRED, not EARNED

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
# TEN registry rows, two of them OPEN (`D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY` and
# `D-TEST-CORPUS-NO-QEMU-X86_64-ON-ARM64-HOST` -- both carry an UNDERSCORE, which the
# old character class excluded). An open row the gate cannot SEE is a hole an opened
# row can slip through, so the shape now admits `_`, `.` and decoration.
# This token is used for two jobs only: NAMING a row, and deciding whether an
# UNRECOGNIZED table looks like an anchor table. Inclusion of a row in a RECOGNIZED
# table never depends on it -- every data row of a recognized deferral table counts,
# so no row can be invisible because of how it is spelled.
ANCHOR_TOKEN = re.compile(r"D-[A-Za-z0-9_]+(?:[-.][A-Za-z0-9_]+)+")


class Shape(object):
    """A recognized deferral-table shape: how to find its status cell."""

    def __init__(self, kind, status_col):
        self.kind = kind
        # 1-based index into the row's cells, or None when the shape HAS no status
        # cell (see RESERVED below).
        self.status_col = status_col


# ── The sanctioned deferral-table shapes, ✔MEASURED across all 39 files in .plans/ ──
#
# 1. REGISTRY-SHAPED  `| Anchor | Trigger | Closing work | Cross-refs |`
#    `_deferred-anchor-registry.md` (both "Anchor Index" tables) and
#    `17-shader-gpu-plan` section 5.4, which says so in prose: "Column shape matches
#    `_deferred-anchor-registry.md`". The HEADER says cell 2 is "Trigger"; in PRACTICE
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
SHAPES_EXACT = {
    ("anchor", "trigger", "closing work", "cross-refs"): Shape("registry", 2),
    ("anchor", "owns"): Shape("reserved", None),
}
SHAPES_PREFIX = [
    (("#", "deferred item"), Shape("deferred-items", 2)),
]

# ── Tables that carry an anchor-looking token in cell 1 but are NOT deferral tables ──
# NAMED, not guessed. Each was read before being listed here:
#   `pr | title | scope`        13-assembler section 3 PR breakdown; one row's cell 1 is
#                               "~~AS3~~ ... (substrate slice + binary ops + D-ML7-2.1)"
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


def split_row(line):
    """Cells of a markdown table row, 1-based (index 0 is the empty pre-pipe lead)."""
    return line.split("|")


def is_closed(cell):
    """A status cell is CLOSED iff it OPENS with the closure mark.

    Everything else -- every other glyph, every novel glyph, no glyph at all -- is
    OPEN.  The complement is defined, never the variants, so a marker nobody has
    thought of yet counts as open, which is the safe direction.

    star **THE LEADING-POSITION TEST IS THE FIX FOR A MEASURED UNDERCOUNT, 2026-08-12.**
    This previously asked `CLOSED_MARK not in status` -- the mark ANYWHERE in the cell.
    Open rows legitimately use the check mark mid-prose to flag a DONE HALF or a piece
    of good news, e.g. D-EXAMPLES-RELEASE-ARM-NOT-COMPILED-WHEN-ITS-BASELINE-DID-NOT-RUN
    opens `orange **OPEN -- normal ...` and later says `... the arms themselves are GOOD`
    with a check mark.  The substring test read that as CLOSED.  MEASURED at the time of
    the fix by running BOTH rules over the registry at HEAD: the substring rule reported
    577 open, the leading-position rule reports 642 -- EXACTLY 65 rows hidden, 11.3%.
    star CORRECTED 2026-08-12, hours after this docstring was first written, by an
    independent audit.  It originally read "49 rows ... of which ~39 were genuinely open
    ... ~7%".  Those came from a scratch regex that only matched rows whose cell HEAD
    carried an EXPLICIT open glyph -- so rows with no marker at all plus a mid-prose check
    mark were invisible to the very scan that was measuring the blind spot.  The registry
    row D-GATE-ANCHOR-BALANCE-DECORATIVE-CLOSE-MARK-UNDERCOUNTS carried the right numbers
    and this docstring did not, which is the worse way round: this is the surface a future
    maintainer reads FIRST.  Measure with the tool, never with a scratch grep that may
    share the bug you are hunting.
    star star AND NOTE THE DIRECTION, BECAUSE IT IS NOW THE SAME DIRECTION THREE TIMES.
    Version 1 of this gate enumerated the OPEN glyphs and was blind to the hourglass,
    reporting 269 open where there were 579.  That was fixed by inverting to "open unless
    closed" -- and the inversion introduced the mid-prose blind spot (577 vs 642).  Fixing
    THAT left the denominator itself too narrow: registry-only, blind to every plan-side
    home (`D-GATE-BALANCE-COUNTS-ONLY-THE-REGISTRY`, 2026-08-13).  All three errors
    flattered the cycle by hiding open work.  An instrument that keeps failing toward
    "you are doing fine" is the one to distrust; when you change this rule, ask which
    direction the new version errs in and pin it in self_test().

    ⚠ The leading-strip set is `*_ ` and is deliberately UNCHANGED from the 2026-08-12
    version, so `--denominator registry` reproduces the historical headline byte for
    byte. ✔MEASURED 2026-08-13: no status cell in ANY recognized table begins with a
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


def is_bookkeeping_closure(cell):
    """A CLOSED status cell whose work PRE-DATES this cycle: `✅🧾 ...`.

    The pair must LEAD the cell, closure mark first. Two properties follow and
    both are asserted in self_test():
      * the row still reads as CLOSED, because is_closed() is untouched and sees
        the same leading `✅` it always saw -- so the OPEN population drops by one;
      * the cycle is credited with nothing, because main() adds the row back into
        `net_new` (see the BOOKKEEPING_MARK note and `balance()`).

    ⚠ Only the ADJACENT pair counts. `✅ **CLOSED** ... 🧾 bookkeeping` is a closure
    that CLAIMS the exemption in prose and does not get it, for the same reason
    is_disclosed refuses a mid-prose disclosure mark.
    """
    head = cell.lstrip().lstrip("*_ ")
    if not head.startswith(CLOSED_MARK):
        return False
    return head[len(CLOSED_MARK):].lstrip().startswith(BOOKKEEPING_MARK)


# ── ARM 2: A STATUS CELL WHOSE OWN OPENING VERDICT CONTRADICTS ITS MARKER ──────
# `D-GATE-BALANCE-EXEMPTS-A-DISCLOSED-OPENING-BUT-NOT-A-BOOKKEEPING-CLOSURE` asks
# for a gate rather than a sweep, and the argument is recurrence: the identical
# defect was swept on 2026-08-19 as `D-PLANS-REGISTRY-CLOSURE-MARK-IN-WRONG-CELL`
# and was back eleven days later. A sweep fixes the rows that exist; a gate fixes
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


# ── ARM 3: A GATED ROW MUST NAME THE ROW THAT OPENS IT ─────────────────────────
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
GATED_DECL = re.compile(
    r"MUST[\s-]*NOT[\s-]*BUILD|TRIGGER[\s-]*GATED|TRIGGER[\s-]*NOT[\s-]*FIRED",
    re.IGNORECASE)
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
# ⓘ ✔MEASURED: no row anywhere under `.plans/` writes a NEGATED form ("not already
# fired", "never fired"), so the match has no known false-exclusion path today.
# star Like the disclosed mark, this is a CHECKABLE CLAIM rather than a formatting
# choice: writing "already fired" when it has not is a false statement about the
# world, and a reader can look. The direction of error is also the safe one -- this
# can only ever SHRINK the accused set, and an escaped gate joins the reported DEBT
# population, whereas a false accusation is what gets a guard turned off.
TRIGGER_FIRED = re.compile(r"ALREADY[\s-]*FIRED", re.IGNORECASE)
# ⓘ The opener SPELLING reuses the registry's existing `[[anchor-name]]` link
# form -- deliberately NOT a new syntax, because one already resolves. What is
# added is a required LABEL in front of it, because a bare `[[...]]` is satisfied
# by any cross-reference in the row and this repository has already been bitten
# twice by a detector that a mere MENTION can satisfy
# (D-GATE-ANCHOR-CITATION-RESOLVES-VIA-ITS-OWN-BUG-REPORT).
# ⚠ A wrapped id is invisible to this regex, as it is to every grep -- but a
# markdown table row is ONE LINE by construction, so an id inside a registry row
# cannot be wrapped in the first place. The residual hazard is a MISTYPED or
# TRUNCATED id, and that is caught by the resolution half rather than the syntax
# half: an opener that names no row is refused exactly like an absent one.
OPENER_REF = re.compile(r"OPEN(?:ED|S)?[\s-]*BY[\s:]*\[\[([^\]|]+)\]\]",
                        re.IGNORECASE)


def is_gated(status):
    """The row DECLARES ITSELF trigger-gated / must-not-build, and is still waiting.

    Two conditions, and the second is what separates a row that declares itself
    gated from one that merely MENTIONS the vocabulary: a row stating its trigger
    has ALREADY FIRED is not gated, because a fired gate is not a gate. See the
    TRIGGER_FIRED note for the measurement, and for why windowing the declaration
    is the wrong discriminator here even though it is the right one for closures.
    """
    flat = strip_decoration(status)
    if not GATED_DECL.search(flat):
        return False
    return not TRIGGER_FIRED.search(flat)


def opener_of(whole_row):
    """The anchor id this row names as its opener, or "" when it names none."""
    m = OPENER_REF.search(strip_decoration(whole_row))
    return m.group(1).strip() if m else ""


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
    bookkeeping: {"relpath#name"} whose status cell is a `✅🧾` bookkeeping closure
    mismarked  : {"relpath#name": status_excerpt} contradicting their own verdict
    gated_rows : {"relpath#name": (opener_id, status_excerpt)}; opener_id is ""
                 when the row names none.
    """

    def __init__(self):
        self.rows = {}
        self.findings = []
        self.names = set()
        self.bookkeeping = set()
        self.mismarked = {}
        self.gated_rows = {}

    def merge(self, other):
        self.rows.update(other.rows)
        self.findings.extend(other.findings)
        self.names |= other.names
        self.bookkeeping |= other.bookkeeping
        self.mismarked.update(other.mismarked)
        self.gated_rows.update(other.gated_rows)
        return self


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
    (`D-PLANS-REGISTRY-MALFORMED-ROW-CELLS` records that malformed rows really do occur
    in this registry, so this is a live hazard, not a hypothetical one.)
    """
    scan = Scan()
    rows = scan.rows
    findings = scan.findings
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
                key = "%s#%s" % (relpath, name)
                scan.names.add(key)
                if shape.status_col is None:
                    # No status column exists in this shape, so nothing can close a
                    # row in place. Unconditionally OPEN -- and a check mark sitting
                    # in some OTHER column must NOT close it (pinned in self_test).
                    # The three status-cell arms below are silent here for the same
                    # reason: there is no cell for them to read.
                    opened, excerpt = True, "(reserved anchor - no status column)"
                else:
                    status = cells[shape.status_col] if len(cells) > shape.status_col else ""
                    opened = not is_closed(status)
                    excerpt = " ".join(status.split())[:80]
                    closing = (cells[shape.status_col + 1]
                               if len(cells) > shape.status_col + 1 else "")
                    if is_bookkeeping_closure(status):
                        scan.bookkeeping.add(key)
                    if is_mismarked_closure(status, closing):
                        scan.mismarked[key] = excerpt
                    if opened and is_gated(status):
                        scan.gated_rows[key] = (opener_of(raw), excerpt)
                # Only OPEN rows are recorded, so when one name carries several rows in
                # the same file (the registry has a few), the name counts as OPEN if
                # ANY of its rows is open. Same behaviour as the predecessor, and it is
                # the safe direction: a closed duplicate cannot mask an open original.
                if opened:
                    rows["%s#%s" % (relpath, name)] = (True, excerpt, shape.kind)
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


def repo_root():
    p = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        sys.exit("not inside a git repository")
    return p.stdout.strip()


def plan_files_at(root, ref):
    """Depth-1 `.plans/*.md` as of `ref`. Fails loud rather than scanning nothing."""
    p = subprocess.run(["git", "ls-tree", "--name-only", "%s:%s" % (ref, PLANS_DIR)],
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
        p = subprocess.run(["git", "show", "%s:%s" % (ref, rel)], cwd=root,
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
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


def is_registry(key):
    return key.startswith(REG_REL + "#")


def split_homes(rows):
    reg = {k: v for k, v in rows.items() if is_registry(k)}
    plan = {k: v for k, v in rows.items() if not is_registry(k)}
    return reg, plan


def gated_set(rows, denominator):
    if denominator == "registry":
        return {k: v for k, v in rows.items() if is_registry(k)}
    return rows


class Balance(object):
    """The gate's arithmetic, as data, so the self-test can assert on it."""

    def __init__(self, closed, opened, disclosed, bookkept, before, after):
        self.closed = closed          # names OPEN at base and not OPEN now
        self.opened = opened          # names OPEN now and not OPEN at base
        self.disclosed = disclosed    # opened rows that DISCLOSE pre-existing debt
        self.bookkept = bookkept      # closed rows whose closure was BOOKKEEPING
        self.before = before          # gated OPEN count at base
        self.after = after            # gated OPEN count now

    @property
    def created(self):
        return [n for n in self.opened if n not in set(self.disclosed)]

    @property
    def net_new(self):
        """The gated quantity. star THE TWO CORRECTIONS PULL IN OPPOSITE DIRECTIONS
        AND THAT IS THE DESIGN.

        `after - before` is the raw movement. A DISCLOSED opening is subtracted
        because it records debt that already existed, so it must not be punished.
        A BOOKKEEPING closure is ADDED BACK because the row's work already existed
        too, so it must not be rewarded -- the population legitimately drops by
        one while the cycle is credited with nothing. Without the `+`, repairing a
        stale glyph would buy a cycle one free new deferral, which is precisely the
        motivated measurement `D-GATE-BALANCE-EXEMPTS-A-DISCLOSED-OPENING-BUT-NOT-A-BOOKKEEPING-CLOSURE`
        refused to make.
        """
        return (self.after - self.before) - len(self.disclosed) + len(self.bookkept)


def balance(before, after, after_bookkeeping, denominator):
    """Compare two scans. `before`/`after` are Scan objects."""
    gate_before = gated_set(before.rows, denominator)
    gate_after = gated_set(after.rows, denominator)
    closed = sorted(set(gate_before) - set(gate_after))
    opened = sorted(set(gate_after) - set(gate_before))
    disclosed = sorted(n for n in opened if is_disclosed(after.rows[n][1]))
    # ⚠ ONLY A ROW THAT ACTUALLY MOVED CAN BE A BOOKKEEPING CLOSURE. A row that
    # was ALREADY closed at the base ref and merely carries the mark is not this
    # cycle's business, and adding it to the net would charge the cycle for
    # somebody else's repair.
    bookkept = sorted(n for n in closed if n in after_bookkeeping)
    return Balance(closed, opened, disclosed, bookkept,
                   len(gate_before), len(gate_after))


# ─────────────────────────────── self-test ────────────────────────────────────

def _doc(*rows):
    return "\n".join(rows)


REG_HDR = ["| Anchor | Trigger | Closing work | Cross-refs |", "|---|---|---|---|"]
DEF_HDR = ["| # | Deferred item | Why deferred | Owner / closure | Trigger |",
           "|---|---|---|---|---|"]
P00_HDR = ["| # | Deferred item | Why deferred | Class | Owner / closure | Trigger |",
           "|---|---|---|---|---|---|"]
RES_HDR = ["| Anchor | Owns |", "|--------|------|"]


def self_test():
    """Red-on-disable for the instrument itself.

    Families of case, and ALL must hold:
      (a) the glyph-agnostic inversion -- if someone 'helpfully' rewrites is_closed()
          to enumerate open glyphs, the novel-glyph cases fail;
      (b) the widened denominator -- if someone narrows the scan back to the registry,
          or lets an unknown table shape pass quietly, those cases fail;
      (f) the BOOKKEEPING closure -- leading-position only, still CLOSED for the
          population, and NET-NEUTRAL for the cycle (the arithmetic is run, not
          asserted, in `_case_net_neutral`);
      (g) a status cell whose opening VERDICT is a closure while its marker is not;
      (h) a gated row that must NAME the row which opens it.

    star EVERY CASE ASSERTS ALL FOUR OUTPUTS -- the open set AND the three new
    per-row sets -- rather than only the one it was written for. A case that
    asserted just its own subject would let a new arm fire spuriously on twenty
    unrelated fixtures and still print `0 failed`, which is the vacuous green this
    file exists to refuse.

    ⚠ THE FIXTURE NAMES ARE DELIBERATELY **NOT** ANCHOR-SHAPED, and that is a
    contract, not a style choice (`D-GATE-ANCHOR-BALANCE-SELFTEST-FIXTURES-ARE-ANCHOR-SHAPED`).
    `scripts/` is a scanned root for `check-anchor-registry`, whose grammar demands
    THREE hyphen-separated segments (`D-[A-Z0-9_]+(-[A-Z0-9_]+){2,}`), so a
    three-segment fixture name reads to that guard as a citation of a deferral that
    does not exist. Eleven of them did, and they were green only because the
    registry row REPORTING them quoted their names -- one tidy-up away from reding
    the tree on eleven strings that are parser INPUT DATA.
    ⚠ AND DO NOT SPELL ONE OUT HERE TO ILLUSTRATE THE POINT, which this docstring
    did in its first draft: writing the old name into the explanation re-creates
    the citation the rename just removed, and Lane G's guard run caught exactly
    that shape in this file's own help text on 2026-08-23. Any placeholder stays
    UNDER the threshold, the same rule `check-anchor-registry` states about its own
    `D-XX-EXAMPLE`.
    ⇒ **THE CONTRACT IS A SEGMENT COUNT: a synthetic fixture name carries EXACTLY
    TWO hyphen-separated segments** (`D-XX-BLANKLINE`, `D-PL-HALF`), which is
    invisible to that guard by construction and still matched by this file's own
    wider ANCHOR_TOKEN (which needs only two), so the orphan-row and
    unrecognized-table arms below still fire. A NEW FIXTURE MUST KEEP THAT SHAPE.
    Do not allowlist instead: an Allowlist entry would assert these are code-internal
    pins, which is false, and would keep asserting it if a future cycle ever opened
    one of the names for real.
    ⓘ THREE names below are deliberately NOT synthetic and stay three-segment --
    `D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY`, `D-LK-DYN-RODATA-ITEM-RELOC` and
    `D-AXIS-ASYNC-DI`. Each names a row that REALLY EXISTS and each case is about
    that specific row's shape, so they are citations, not dangling input data --
    which is the whole distinction the eleven violated. ✔MEASURED 2026-08-23 at
    cf27fe8b: all three resolve.
    """
    cases = []

    def case(doc, expect, why, path=REG_REL, expect_fatal=0, expect_warn=0,
             book=(), mismark=(), gated=None):
        cases.append((doc, expect, why, path, expect_fatal, expect_warn,
                      set(book), set(mismark), dict(gated or {})))

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
    # star The 3rd miss (`D-GATE-BALANCE-COUNTS-ONLY-THE-REGISTRY`): a plan-side row
    # was worth ZERO to this gate, so moving work out of the registry looked like
    # progress. These pin that a plan row counts exactly like a registry row.
    case(_doc(*DEF_HDR, "| D-XX-PLOPEN | plain prose, no glyph | why | owner | trig |"),
         {"D-XX-PLOPEN"}, "plan sec3.1 row with no glyph is OPEN", path=".plans/22-x.md")
    case(_doc(*DEF_HDR, "| ~~D-XX-PLDONE~~ | ✅ **CLOSED 2026-01-01** | why | own | t |"),
         set(), "plan sec3.1 leading check mark closes (cell-1 strikethrough ignored)",
         path=".plans/22-x.md")
    # ★ REQUIRED CASE 1: mid-prose check mark in a plan-shaped row.
    case(_doc(*DEF_HDR,
              "| D-PL-HALF | **OPEN** -- the ✅ layout half landed; va_arg half open | w | o | t |"),
         {"D-PL-HALF"},
         "plan row: mid-prose check mark does NOT close (the 2nd miss, plan side)",
         path=".plans/14-x.md")
    # ★ REQUIRED CASE 2: a glyph nobody has enumerated, on the plan-side path.
    case(_doc(*DEF_HDR, "| D-PL-NOVEL | \U0001fae0 **melting face status** | w | o | t |"),
         {"D-PL-NOVEL"}, "plan row: a novel glyph is OPEN (the whole point, plan side)",
         path=".plans/13-x.md")
    # ✔MEASURED shape of plan 12: doneness recorded in the LAST cell, not the status
    # cell. That row reads OPEN. It is the SAFE direction and it is left that way on
    # purpose -- moving the test to "a check mark anywhere in the row" would re-import
    # the exact 2026-08-12 undercount. Pinned so nobody 'fixes' it.
    case(_doc(*DEF_HDR, "| D-PL-LATE | hardening item | cosmetic | none. | ML6 c1 ✅ done |"),
         {"D-PL-LATE"},
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
    case(_doc(*RES_HDR, "| `D-AXIS-DONE` | ✅ shipped, honest |"), {"D-AXIS-DONE"},
         "a check mark in a NON-status column cannot close a reserved row",
         path=".plans/24-x.md")

    # ── (c) blind spots of the OLD row regex, now counted ──
    case(_doc(*REG_HDR, "| `D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY` | ⚠ OPEN | w | r |"),
         {"D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY"},
         "UNDERSCORE in the name: invisible to the old regex, counted now")
    case(_doc(*REG_HDR, "| ~~`D-LK-DYN-RODATA-ITEM-RELOC`~~ | ⚠ still open | w | r |"),
         {"D-LK-DYN-RODATA-ITEM-RELOC"},
         "STRIKETHROUGH name: invisible to the old regex, counted now")

    # ── (d) fail-loud on shapes, and NO false alarm on the named exclusions ──
    case(_doc("| Widget | Notes |", "|---|---|",
              "| `D-XX-NEWTABLE` | invented table |"),
         set(), "an UNRECOGNIZED anchor-looking table is REPORTED, never skipped",
         path=".plans/99-x.md", expect_fatal=1)
    case(_doc("| PR | Title | Scope |", "|---|---|---|",
              "| ~~AS3~~ | cycle 3 landed (binary ops + D-ML7-2.1) | s |"),
         set(), "a NAMED non-deferral table is excluded, and raises no false alarm",
         path=".plans/13-x.md")
    case(_doc("| Tier | Example | External tools? |", "|---|---|---|",
              "| **Production pipeline** | dss-code-prime compiling sqlite | Zero. |"),
         set(), "an ordinary prose table with no anchors is simply ignored",
         path=".plans/17-x.md")
    # star The line-based predecessor would have counted these; a table-based reader
    # must not lose them to silence. Both are REPORTED, which fails the run.
    case(_doc("## A heading, and then a row with no table header at all",
              "| `D-XX-ORPHANROW` | \U0001f7e0 **OPEN** | work | refs |"),
         set(), "an ORPHAN anchor row outside any table is REPORTED, never dropped",
         path=".plans/99-y.md", expect_fatal=1)
    case(_doc(*REG_HDR, "| `D-XX-INTABLE` | ⚠ OPEN | w | r |", "",
              "| `D-XX-BLANKLINE` | ⚠ OPEN | w | r |"),
         {"D-XX-INTABLE"},
         "a row severed from its table by a blank line is REPORTED, not silently lost",
         expect_fatal=1)
    # star 22-optimizer section 3.1's REAL defect, pinned: an HTML comment mid-table.
    # The rows below it must still be COUNTED (dropping them understates the
    # denominator) AND the interruption must still be REPORTED (the file is malformed).
    case(_doc(*DEF_HDR, "| D-XX-BEFORECOMMENT | still open | w | o | t |",
              "<!-- a user-supervised note parked inside the table body -->",
              "| D-XX-AFTERCOMMENT | also still open | w | o | t |",
              "| D-XX-AFTERCLOSED | ✅ **CLOSED** | w | o | t |"),
         {"D-XX-BEFORECOMMENT", "D-XX-AFTERCOMMENT"},
         "rows under a mid-table HTML comment are COUNTED and the break is REPORTED",
         path=".plans/22-x.md", expect_warn=1)
    case(_doc(*DEF_HDR, "| D-XX-LASTROW | open | w | o | t |",
              "<!-- a comment that legitimately FOLLOWS the table -->",
              "some prose"),
         {"D-XX-LASTROW"},
         "a comment AFTER the last row is not an interruption (no false alarm)",
         path=".plans/22-x.md")

    # ── (f) THE BOOKKEEPING CLOSURE MARK ──
    # star Leading position, closure mark FIRST. The row must read CLOSED for the
    # population; the exemption is a separate fact carried alongside, never instead.
    case(_doc(*REG_HDR, "| `D-XX-BOOKKEPT` | ✅🧾 **CLOSED 2026-01-01, mark repaired** | - | r |"),
         set(), "a bookkeeping closure is CLOSED for the population",
         book={"D-XX-BOOKKEPT"})
    case(_doc(*REG_HDR, "| `D-XX-BOOKEMPH` | **✅🧾 CLOSED** | - | r |"), set(),
         "leading emphasis before the pair still counts", book={"D-XX-BOOKEMPH"})
    case(_doc(*REG_HDR, "| `D-XX-BOOKPROSE` | ✅ **CLOSED** -- 🧾 pure bookkeeping, honest | - | r |"),
         set(), "MID-PROSE bookkeeping mark claims nothing (leading position only)")
    # star THIS CASE WAS FIRST WRITTEN EXPECTING NO SECOND FINDING, AND THE SELF-TEST
    # CORRECTED THE EXPECTATION RATHER THAN THE RULE -- the same way the `std :: abort
    # ()` case did in `check-no-abort-in-tests`. A cell leading with the bookkeeping
    # mark alone is OPEN (that mark is not a closure marker) AND its opening verdict
    # is `CLOSED`, so it is exactly the self-contradiction arm (g) exists to catch.
    # Both facts are true at once and both are now pinned.
    case(_doc(*REG_HDR, "| `D-XX-BOOKONLY` | 🧾 **CLOSED 2026-01-01** | - | r |"),
         {"D-XX-BOOKONLY"},
         "the bookkeeping mark ALONE does not close, and is then self-contradicting",
         mismark={"D-XX-BOOKONLY"})
    case(_doc(*REG_HDR, "| `D-XX-BOOKGAP` | ✅ 🧾 **CLOSED, mark repaired** | - | r |"),
         set(), "whitespace between the two marks is tolerated",
         book={"D-XX-BOOKGAP"})

    # ── (g) A STATUS CELL WHOSE OPENING VERDICT CONTRADICTS ITS MARKER ──
    case(_doc(*REG_HDR, "| `D-XX-MISMARK` | **FIXED 2026-01-01** -- the driver was repaired | - | r |"),
         {"D-XX-MISMARK"}, "opening verdict FIXED with no closure marker is caught",
         mismark={"D-XX-MISMARK"})
    case(_doc(*REG_HDR, "| `D-XX-MISGLYPH` | \U0001f534 **CLOSED 2026-01-01** | - | r |"),
         {"D-XX-MISGLYPH"}, "a closure verdict behind an OPEN glyph is caught",
         mismark={"D-XX-MISGLYPH"})
    # ★ THE WALK-BACK MAY LIVE IN ANY CELL -- ✔MEASURED on
    # D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME, whose status cell opens `SHIPPED`
    # and whose CLOSING-WORK cell says "NOT YET DISCHARGED ... stays open".
    case(_doc(*REG_HDR,
              "| `D-XX-WALKED` | **SHIPPED 2026-01-01** | NOT YET discharged; verify first | r |"),
         {"D-XX-WALKED"},
         "a walk-back in ANOTHER cell exonerates the row (the measured shape)")
    case(_doc(*REG_HDR, "| `D-XX-PARTIAL` | **CLOSED** for the rodata path; PARTIAL | w | r |"),
         {"D-XX-PARTIAL"}, "a scoped/PARTIAL closure is not accused")
    case(_doc(*REG_HDR, "| `D-XX-PROSE` | \U0001f7e0 **OPEN** -- the FIXED half landed | w | r |"),
         {"D-XX-PROSE"}, "a closure word MID-cell is not an opening verdict")
    case(_doc(*REG_HDR, "| `D-XX-REALCLOSE` | ✅ **CLOSED 2026-01-01** | - | r |"), set(),
         "a properly marked closure is never accused")
    # ★ The measured false accusation: plan-00 section 0.2's `D12` opens
    # "Shipped-lib FFI = Model 3" and is explicitly NOT a deferral.
    case(_doc(*P00_HDR,
              "| D-XX-COMPOUND | **Shipped-lib FFI = Model 3** -- chosen 2026-01-01 | w | HIGH | o | t |"),
         {"D-XX-COMPOUND"}, "a HYPHENATED COMPOUND is not an opening verdict",
         path=".plans/00-x.md")

    # ── (h) A GATED ROW MUST NAME THE ROW THAT OPENS IT ──
    case(_doc(*REG_HDR, "| `D-XX-GATEDBARE` | \U0001f7e0 **OPEN -- TRIGGER-GATED** | w | r |"),
         {"D-XX-GATEDBARE"}, "a gated row with no opener is recorded with opener ''",
         gated={"D-XX-GATEDBARE": ""})
    case(_doc(*REG_HDR,
              "| `D-XX-GATEDOK` | \U0001f7e0 **OPEN -- TRIGGER-GATED**, opened by [[D-XX-OPENER]] | w | r |"),
         {"D-XX-GATEDOK"}, "an opener reference is read from the row",
         gated={"D-XX-GATEDOK": "D-XX-OPENER"})
    # star A BARE CROSS-REFERENCE IS NOT AN OPENER. Every registry row carries
    # `[[...]]` links; accepting one would make this arm satisfiable by any row that
    # merely mentions a neighbour -- the "satisfied by a mention" failure this
    # registry has already recorded twice.
    case(_doc(*REG_HDR,
              "| `D-XX-GATEDXREF` | \U0001f7e0 **OPEN -- TRIGGER-GATED**; see [[D-XX-OTHER]] | w | r |"),
         {"D-XX-GATEDXREF"}, "a bare cross-reference does NOT count as an opener",
         gated={"D-XX-GATEDXREF": ""})
    case(_doc(*REG_HDR, "| `D-XX-GATEDSHUT` | ✅ **CLOSED, was TRIGGER-GATED** | - | r |"),
         set(), "a CLOSED gated row is not asked for an opener")
    case(_doc(*REG_HDR, "| `D-XX-MUSTNOT` | \U0001f7e0 **OPEN -- MUST-NOT-BUILD** | w | r |"),
         {"D-XX-MUSTNOT"}, "MUST-NOT-BUILD is the same declaration as TRIGGER-GATED",
         gated={"D-XX-MUSTNOT": ""})
    case(_doc(*REG_HDR, "| `D-XX-UNGATED` | \U0001f7e0 **OPEN -- normal** | w | r |"),
         {"D-XX-UNGATED"}, "an ordinary open row is not a gated row")
    # star star star THE PAIR THAT PINS "A DESCRIPTION OF A CLASS IS NOT A MEMBER OF IT".
    # The first fixture has the shape of the real registry row that recorded this
    # arm's own census: it must QUOTE the vocabulary in order to say what it
    # measured, and its own verdict is that its trigger has fired. The second is a
    # real gate carrying the same words, and it must STILL be asked for an opener --
    # without it the first case would pass just as well with `is_gated` deleted.
    case(_doc(*REG_HDR,
              "| `D-XX-CENSUS` | \U0001f535 **OPEN -- DISCLOSED.** MEASURED: 62 rows declare "
              "themselves TRIGGER-GATED / MUST-NOT-BUILD / TRIGGER-NOT-FIRED and name no "
              "opener. Trigger: ALREADY FIRED. | sweep them | r |"),
         {"D-XX-CENSUS"},
         "a row DESCRIBING the gated class is not a member of it")
    case(_doc(*REG_HDR,
              "| `D-XX-STILLGATED` | \U0001f7e0 **OPEN -- TRIGGER-GATED.** The first ILP32 "
              "target lands. | w | r |"),
         {"D-XX-STILLGATED"},
         "and a real gate carrying the same words IS still asked for an opener",
         gated={"D-XX-STILLGATED": ""})
    # A fired trigger exonerates wherever it sits: the claim is about the WORLD, not
    # about where in the cell it was written (same stance as the disclosed mark).
    case(_doc(*REG_HDR,
              "| `D-XX-FIREDLATE` | \U0001f7e0 **OPEN -- TRIGGER-GATED** when opened; the "
              "trigger has ALREADY FIRED, so the work is actionable now. | w | r |"),
         {"D-XX-FIREDLATE"},
         "a fired trigger exonerates from anywhere in the cell")

    def bare(keys):
        """Row keys are `relpath#name`; the cases assert on the NAME half."""
        return set(k.split("#", 1)[1] for k in keys)

    failed = 0
    for (doc, expect, why, path, expect_fatal, expect_warn,
         exp_book, exp_mismark, exp_gated) in cases:
        s = scan_document(doc, path)
        got = bare(s.rows)
        # star SEVERITY IS ASSERTED, NOT JUST THE COUNT. FATAL means "rows exist that
        # I could not count"; WARN means "the file is malformed but nothing was lost".
        # A case that produced the right number of findings at the WRONG severity would
        # silently turn a gate failure into a note, so both are pinned.
        n_fatal = sum(1 for f in s.findings if f[2] == "FATAL")
        n_warn = sum(1 for f in s.findings if f[2] == "WARN")
        got_book = bare(s.bookkeeping)
        got_mismark = bare(s.mismarked)
        got_gated = dict((k.split("#", 1)[1], v[0]) for k, v in s.gated_rows.items())
        ok = (got == expect and n_fatal == expect_fatal and n_warn == expect_warn
              and got_book == exp_book and got_mismark == exp_mismark
              and got_gated == exp_gated)
        if not ok:
            failed += 1
        extra = ""
        if (n_fatal, n_warn) != (expect_fatal, expect_warn):
            extra += "  findings expected=%d fatal/%d warn got=%d/%d" % (
                expect_fatal, expect_warn, n_fatal, n_warn)
        if got_book != exp_book:
            extra += "  book expected=%s got=%s" % (sorted(exp_book), sorted(got_book))
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
    a = scan_document(_doc(*REG_HDR, "| `D-XX-DUPROW` | ⚠ OPEN | w | r |"), REG_REL)
    b = scan_document(_doc(*DEF_HDR, "| D-XX-DUPROW | still open here | w | o | t |"),
                      ".plans/14-x.md")
    merged = dict(a.rows)
    merged.update(b.rows)
    pin(len(merged) == 2, "the same anchor in two homes stays two distinct rows",
        "expected=2 got=%d" % len(merged))
    extra_total += 1

    # ── (f2) NET-NEUTRALITY, run through the real arithmetic rather than asserted ──
    # star THIS IS THE PIN THE WHOLE MARKER EXISTS FOR. Two worlds differing in ONE
    # edit: whether a row whose work predates the cycle gets its stale glyph repaired.
    #   * the OPEN population MUST differ (3 -> 2 vs 3 -> 3) -- the repair is real;
    #   * the NET must NOT (0 either way) -- the cycle is credited with nothing.
    # And the correction is shown to be LOAD-BEARING: without `+ len(bookkept)` the
    # repaired world scores -1, i.e. it would have bought the cycle one free new
    # deferral, which is exactly the motivated measurement the registry row refused.
    base_doc = _doc(*REG_HDR,
                    "| `D-XX-STAYOPEN` | \U0001f7e0 **OPEN** | work | refs |",
                    "| `D-XX-STALEMARK` | **FIXED 2026-01-01** | none | refs |",
                    "| `D-XX-WORKDONE` | \U0001f7e0 **OPEN** | work | refs |")
    repaired = _doc(*REG_HDR,
                    "| `D-XX-STAYOPEN` | \U0001f7e0 **OPEN** | work | refs |",
                    "| `D-XX-STALEMARK` | ✅🧾 **CLOSED 2026-01-01 (mark repaired)** | - | r |",
                    "| `D-XX-WORKDONE` | ✅ **CLOSED by this cycle** | - | refs |",
                    "| `D-XX-NEWDEBT` | \U0001f7e0 **OPEN** | work | refs |")
    unrepaired = _doc(*REG_HDR,
                      "| `D-XX-STAYOPEN` | \U0001f7e0 **OPEN** | work | refs |",
                      "| `D-XX-STALEMARK` | **FIXED 2026-01-01** | none | refs |",
                      "| `D-XX-WORKDONE` | ✅ **CLOSED by this cycle** | - | refs |",
                      "| `D-XX-NEWDEBT` | \U0001f7e0 **OPEN** | work | refs |")
    b0 = scan_document(base_doc, REG_REL)
    a1 = scan_document(repaired, REG_REL)
    a2 = scan_document(unrepaired, REG_REL)
    bal1 = balance(b0, a1, a1.bookkeeping, "registry+plans")
    bal2 = balance(b0, a2, a2.bookkeeping, "registry+plans")
    uncorrected = (bal1.after - bal1.before) - len(bal1.disclosed)
    pin(bal1.after == 2 and bal2.after == 3,
        "a bookkeeping closure DOES reduce the OPEN population",
        "expected 2 vs 3, got %d vs %d" % (bal1.after, bal2.after))
    pin(len(bal1.bookkept) == 1 and len(bal2.bookkept) == 0,
        "the repaired row is recognised as a bookkeeping closure",
        "expected 1 vs 0, got %d vs %d" % (len(bal1.bookkept), len(bal2.bookkept)))
    pin(bal1.net_new == bal2.net_new == 0,
        "NET-NEUTRAL: the repair leaves the cycle net UNCHANGED",
        "expected 0 == 0, got %d vs %d" % (bal1.net_new, bal2.net_new))
    pin(uncorrected == -1,
        "and the +bookkept correction is LOAD-BEARING (would score -1 without it)",
        "expected -1, got %d" % uncorrected)
    extra_total += 4

    # A bookkeeping mark on a row that was ALREADY closed at the base ref is not
    # this cycle's business: it never entered `closed`, so it cannot be credited.
    b3 = scan_document(_doc(*REG_HDR,
                            "| `D-XX-PRECLOSED` | ✅ **CLOSED long ago** | - | r |"), REG_REL)
    a3 = scan_document(_doc(*REG_HDR,
                            "| `D-XX-PRECLOSED` | ✅🧾 **CLOSED long ago** | - | r |"), REG_REL)
    bal3 = balance(b3, a3, a3.bookkeeping, "registry+plans")
    pin(len(bal3.bookkept) == 0 and bal3.net_new == 0,
        "a mark added to an ALREADY-closed row credits nothing",
        "bookkept=%d net=%d" % (len(bal3.bookkept), bal3.net_new))
    extra_total += 1

    failed += extra_failed[0]
    print("self-test: %d case(s), %d failed" % (len(cases) + extra_total, failed))
    return 1 if failed else 0


# ──────────────────────────────────── main ─────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default="HEAD",
                    help="git ref the cycle started from (default HEAD)")
    ap.add_argument("--denominator", choices=("registry", "registry+plans"),
                    default="registry+plans",
                    help="which home(s) the gate counts. 'registry' reproduces the "
                         "pre-2026-08-13 headline; the default counts every home "
                         "SKILL.md section F.2 sanctions.")
    ap.add_argument("--breakdown", action="store_true",
                    help="also print OPEN rows per plan file")
    ap.add_argument("--self-test", action="store_true",
                    help="check the instrument, not the registry")
    args = ap.parse_args()

    # ⚠ A GATE THAT CRASHES WHILE REPORTING IS WORSE THAN ONE THAT SAYS NOTHING.
    # ✔MEASURED 2026-08-12: this script died with UnicodeEncodeError on a Windows
    # cp1252 console the first time it had to print an OPENED row, because the
    # status excerpt began with 🟠 (U+1F7E0). Every prior run had printed only
    # counts, so the reporting path had never been exercised with real content —
    # the crash was latent for exactly as long as the gate was passing. The
    # excerpt is diagnostic prose; an unencodable glyph must degrade to a
    # placeholder, never take the gate down.
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError):          # pragma: no cover - old python
        pass

    if args.self_test:
        return self_test()

    root = repo_root()
    before = scan_at_ref(root, args.base)
    after = scan_worktree(root)
    findings = list(after.findings)

    b_reg, b_plan = split_homes(before.rows)
    a_reg, a_plan = split_homes(after.rows)
    bal = balance(before, after, after.bookkeeping, args.denominator)

    print("anchor-balance: denominator = %s   (SKILL.md sec F.2 sanctions BOTH homes; "
          "use --denominator registry for the pre-2026-08-13 headline)" % args.denominator)
    print("anchor-balance: OPEN at %-10s registry=%-5d plans=%-5d total=%d"
          % (args.base, len(b_reg), len(b_plan), len(before.rows)))
    print("anchor-balance: OPEN now %-13s registry=%-5d plans=%-5d total=%d"
          % ("", len(a_reg), len(a_plan), len(after.rows)))
    print("anchor-balance: GATED count %d -> %d   (net %+d)"
          % (bal.before, bal.after, bal.after - bal.before))
    print("anchor-balance: closed %d, opened %d  (created %d, disclosed-pre-existing %d, "
          "bookkeeping-only closures %d)"
          % (len(bal.closed), len(bal.opened), len(bal.created), len(bal.disclosed),
             len(bal.bookkept)))
    for n in bal.closed:
        print("  - %s%s" % (n, "   [bookkeeping: net-neutral]" if n in bal.bookkept else ""))
    for n in bal.opened:
        print("  + %s   %s" % (n, after.rows[n][1]))

    if args.breakdown:
        print()
        per = {}
        for k in after.rows:
            per[k.split("#", 1)[0]] = per.get(k.split("#", 1)[0], 0) + 1
        for f in sorted(per, key=lambda x: (-per[x], x)):
            print("  %-52s %d OPEN" % (f, per[f]))

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
    # sweep, the row D-GATE-BALANCE-EXEMPTS-A-DISCLOSED-OPENING-BUT-NOT-A-BOOKKEEPING-CLOSURE,
    # and the 7 marks the sweep actually landed. A wrong number inside the
    # instrument is the shape this file's own docstring warns about three times
    # -- and it reached the one place a later cycle would trust without
    # re-measuring. Re-derive it, never re-quote it.
    new_mismarked = sorted(k for k in after.mismarked if k not in before.mismarked)
    old_mismarked = sorted(k for k in after.mismarked if k in before.mismarked)
    if old_mismarked:
        print()
        print("anchor-balance: DEBT - %d row(s) open with a CLOSURE VERDICT while their "
              "marker says otherwise (pre-existing at %s, not this cycle's doing):"
              % (len(old_mismarked), args.base))
        for k in old_mismarked:
            print("  ~ %s   %s" % (k, after.mismarked[k]))
        print("  Each counts OPEN, so the population is OVER-reported until the mark is "
              "repaired with the net-neutral bookkeeping pair or the opening verdict is "
              "reworded.")

    # == ARM 3: A GATED ROW MUST NAME THE ROW THAT OPENS IT ====================
    # Operator ruling 2026-08-23. Differential for the same reason as arm 2 -- and
    # the census makes it mandatory rather than merely tidy: 62 open gated rows at
    # cf27fe8b, essentially none carrying an opener, so a day-one refusal would red
    # the tree for every lane over debt none of them created.
    unopened, dangling = [], []
    known = set(n.split("#", 1)[1] for n in after.names)
    for k in sorted(after.gated_rows):
        opener, excerpt = after.gated_rows[k]
        if k in before.gated_rows:
            continue                       # pre-existing: DEBT, reported below
        if not opener:
            unopened.append((k, excerpt))
        elif opener not in known:
            dangling.append((k, opener))
    stale_gated = [k for k in after.gated_rows if k in before.gated_rows]
    if stale_gated:
        print()
        print("anchor-balance: DEBT - %d gated row(s) predate %s; %d of them name no "
              "opener." % (len(stale_gated), args.base,
                           sum(1 for k in stale_gated if not after.gated_rows[k][0])))

    # ⚠ FAIL LOUD ON AN UNPARSED TABLE. A silently skipped anchor table is the exact
    # defect D-GATE-BALANCE-COUNTS-ONLY-THE-REGISTRY names, so an unknown shape that
    # LOOKS like an anchor table fails the run even when the balance itself is fine.
    # Fix it by teaching SHAPES_* the new shape, or by NAMING it in EXCLUDED_HEADERS
    # after reading it -- never by deleting this check.
    warns = [f for f in findings if f[2] == "WARN"]
    fatals = [f for f in findings if f[2] == "FATAL"]

    # Printed on EVERY run, pass or fail: the file is malformed, but the rows were
    # counted, so the denominator is whole and the balance verdict stands.
    if warns:
        print()
        print("anchor-balance: WARN - %d malformed structure(s); rows were COUNTED, "
              "but the markdown is wrong:" % len(warns))
        for rel, line_no, _sev, what in warns:
            print("  %s:%d   %s" % (rel, line_no, what))
        print("  TABLE BODY INTERRUPTED -> move the HTML comment above or below the "
              "table. Not a gate failure: nothing was lost from the count.")

    # ⚠ FAIL LOUD WHEN THE MEASUREMENT ITSELF IS INCOMPLETE. A silently skipped anchor
    # table is the exact defect D-GATE-BALANCE-COUNTS-ONLY-THE-REGISTRY names, so an
    # unknown shape or an orphan row fails the run even when the balance is fine.
    # Fix it by teaching the tool the shape, or by NAMING it in EXCLUDED_HEADERS after
    # reading it -- never by deleting this check.
    if fatals:
        print()
        print("anchor-balance: FAIL - %d anchor-bearing structure(s) this gate could "
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

    # FAIL LOUD ON A **NEW** SELF-CONTRADICTING OR UNOPENED GATED ROW. Both are
    # measurement corruption rather than untidiness: a mis-marked closure inflates
    # the OPEN denominator with a row that is already closed, and a gated row with
    # no opener is a deferral nothing can ever discharge -- "a precondition with no
    # cause attached is indistinguishable from a permanent block" (operator,
    # 2026-08-23). Neither is fixable by widening this gate.
    if new_mismarked or unopened or dangling:
        print()
        print("anchor-balance: FAIL - %d row(s) this cycle added or edited are "
              "self-contradicting:" % (len(new_mismarked) + len(unopened) + len(dangling)))
        for k in new_mismarked:
            print("  ! MIS-MARKED CLOSURE  %s   %s" % (k, after.mismarked[k]))
        for k, excerpt in unopened:
            print("  ! GATED, NO OPENER    %s   %s" % (k, excerpt))
        for k, opener in dangling:
            print("  ! OPENER RESOLVES TO NOTHING  %s   -> [[%s]]" % (k, opener))
        if new_mismarked:
            print("  MIS-MARKED CLOSURE -> the cell's first word is a closure verdict "
                  "but its leading marker is not the closure mark. Either mark it closed "
                  "(lead the cell with the closure mark followed by the bookkeeping mark "
                  "when the work predates this cycle - that closure is net-neutral), or "
                  "reword the opening so it stops claiming a closure it did not make.")
        if unopened or dangling:
            print("  A GATED ROW NAMES ITS OPENER as `opened by [[D-XX-OPENER]]`, "
                  "reusing the registry's own link form. The opener must RESOLVE to a "
                  "row: a gate whose opener is an unowned event is unfalsifiable and "
                  "will sit forever, while a gate whose opener is a ROW is a dependency "
                  "-- schedulable, sizable, and visible in the queue.")
            print("  IF THIS ROW IS *ABOUT* GATED ROWS RATHER THAN BEING ONE -- a census, "
                  "a rule, a report that has to quote the vocabulary -- do NOT reword it "
                  "to dodge this check. A row must be able to name the thing it is about. "
                  "State its own verdict instead: a row whose trigger has ALREADY FIRED "
                  "is not gated, and saying so is a claim a reader can check.")
        return 1

    net_new = bal.net_new
    disclosed = bal.disclosed
    if disclosed:
        print("anchor-balance: %d disclosed-pre-existing row(s) are EXEMPT from the net "
              "increase (they record debt that already existed)." % len(disclosed))
    if bal.bookkept:
        print("anchor-balance: %d bookkeeping-only closure(s) are NET-NEUTRAL - the OPEN "
              "population drops, the cycle is credited with nothing." % len(bal.bookkept))
    if net_new > 0:
        print()
        print("anchor-balance: FAIL - this cycle CREATED %d more OPEN row(s) than it closed."
              % net_new)
        print("  Close what you opened, or take it to the operator as a decision (dss-cycle "
              "section B). Do NOT widen this gate to fit the cycle.")
        return 1
    print("anchor-balance: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
