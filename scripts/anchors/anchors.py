#!/usr/bin/env python3
# PURPOSE: read and write deferred-anchor registry rows in the one canonical form, so a row is never hand-assembled.
"""anchors.py -- THE DETERMINISTIC DOOR TO THE THREE ANCHOR REGISTRIES.

Operator, 2026-09-01, in three instructions that this one file answers together:
  * *"we need to make this deterministic: inside scripts we must have anchors directory,
    inside it (all with options like --done, --harness or --production): write-anchor,
    read-anchor and read-anchors. Write you pass the parameters and it writes in the
    correct form. read 1 brings the full anchor result, read all brings the name,
    priority and status only. we must also ensure now that the anchor format is correct
    so the read does not fail."*
  * *"add columns for priority and status ... then the write explicitly writes it
    correctly, this way we always have clean statuses."*
  * *"set-anchor, where you can set anything on an existing anchor, by name"* --
    *"because when setting as done, it automatically moves to the done anchors."*

THE ROW SHAPE, since 2026-09-01:

    | Anchor | Priority | Status | Trigger | Closing work | Cross-refs |

`Priority` is `P0`..`P5`; `Status` is a three-value controlled vocabulary spelled
`✅ CLOSED`, `🟠 OPEN`, `⏳ GATED`.
⚠ THE STATUS CELL KEEPS ITS GLYPH AND THAT IS THE CONTRACT, NOT DECORATION. This
project's one definition of closed is *"the cell OPENS with ✅ after stripping `*_ `"* --
the complement defined, never the variants, so a glyph nobody has thought of yet counts
OPEN. A column holding the bare word `CLOSED` would make `is_closed` false for every
closed row at once. The word is for the reader; the glyph is what the battery agrees on.

★★★ WHY A WRITER THAT TAKES PARAMETERS. A registry row is a markdown table row, and
every way of producing one by hand has already failed here in a way that does not look
like a failure:
  * a WRAPPED anchor id goes invisible to every grep, to `check-anchor-registry` and to
    `check-anchor-balance` -- and MINTS a false id at the same time. ✔MEASURED
    2026-08-20: 17 of 78 ids cited on one cycle's added lines were wrapped.
  * a raw `|` inside a cell silently ADDS a column, so every cell after it shifts and
    the row's status is read from its closing work.
  * a short row renders with blank trailing columns -- correct, but every reader must
    then length-test each index, and the ones that did not were wrong quietly.
This tool takes the FIELDS and produces the row, so none of those is expressible.

★★ AND IT IS THE SAME WRITER `apply-registry-row` USES. That tool's input is a lane's
verbatim row FILE (bytes copied, never retyped); this one's input is parameters. Two
input shapes, ONE placement routine -- `place_row` -- because two programs that both
write the registry are two programs that will disagree about where a closed row goes.

★★★ THE MOVE-ON-CLOSE RULE IS PERFORMED, NOT DOCUMENTED. The destination is a FUNCTION
OF THE STATUS, never of the flag:
  * a CLOSED row lands in `-done.md`, in the table matching the working registry it
    came from, and is DELETED from that working registry;
  * an OPEN or GATED row lands in the working registry, and is DELETED from `-done.md`
    (a reopen is a MOVE BACK, never an edit in place);
  * `--done` for a row that is not closed is REFUSED. The archive is not a place work
    can hide: every queue in this project reads the two working registries only, so a
    live row filed there can never be picked up.
`check-anchor-balance`'s partition arm re-checks the same invariant over the whole tree,
so a hand edit that breaks it fails the gate rather than surviving unnoticed.

⚠ NOTHING HERE DECIDES WHAT "CLOSED" MEANS, WHAT A ROW IS NAMED, OR HOW A PRIORITY IS
SEEDED. `is_closed`, `split_row` and `row_name` come from `check-anchor-balance`; the
suggested band comes from `burndown-queue`. Each carries a comment history of defects
that re-typing would re-open, and the standing order is explicit: use the script that
exists, and fix it rather than routing around it.

★ THE READ NEVER FAILS ON A HISTORICAL ROW, AND IT DOES NOT PRETEND THEY ARE FINE.
✔MEASURED 2026-09-01 over all 2,078 rows: after normalisation every row carries exactly
six cells, and exactly FIVE -- all in the archive, all closed -- carry a cell 1 that is
not a bare backticked id (a slash inside the id, two `~~struck~~` retirements, one cell
naming two ids, one whose cell 1 is prose that mints the name in a `[[...]]` link).
Rewriting any of those would MINT an id or destroy a citation, so they are read through
`row_name` (which strips decoration and never fails) and REPORTED by `--lint`. The
writer refuses to create a sixth.

⚠ NO `.sh`/`.ps1` REIMPLEMENTATION -- THE EIGHT SIBLINGS ARE LAUNCHERS OVER THIS FILE.
The operator asked for a `.sh` and a `.ps1` per verb, and this repository's own rule says
a pair must not drift while conceding that no gate can decide equivalence of two
arbitrary programs. Both are satisfied by ONE implementation with eight entry points: the
pair EXISTS on both hosts, takes the same flags, returns the same exit codes, and cannot
diverge in behaviour because there is only one behaviour. A second hand-written
markdown-table writer in PowerShell is precisely the thing this file's third paragraph
is about.

Exit codes: 0 OK (or a clean dry run) · 1 not found / lint findings · 2 refused
(nothing written) · 3 usage error.

Usage:
    anchors.py write --production D-<AREA>-<NAME> --status open --priority P1 --trigger '...'
    anchors.py set   D-<AREA>-<NAME> --status closed --closing '...'      # moves to the archive
    anchors.py read  D-<AREA>-<NAME> [--production|--harness|--done]
    anchors.py list  [--production|--harness|--done] [--band P0 P1] [--open] [--lint]
    anchors.py --self-test
"""
from __future__ import annotations

import argparse
import importlib.util
import io
import json
import os
import re
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - odd stream
        pass

HERE = os.path.dirname(os.path.abspath(__file__))


def repo_root():
    """Walk up until `.plans/` and `scripts/` are both present.

    Not a fixed number of `dirname` calls -- the depth-hardcoding defect the P17
    consolidation had to repair in seventeen scripts at once.
    """
    d = HERE
    while True:
        if os.path.isdir(os.path.join(d, ".plans")) and \
           os.path.isdir(os.path.join(d, "scripts")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            sys.exit("anchors: no repository root above %s (looked for a directory "
                     "holding BOTH .plans/ and scripts/)" % HERE)
        d = parent


def _load(root, rel, why):
    """Import a hyphen-named sibling by path. Fails loud rather than re-implementing."""
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        sys.exit("anchors: cannot find %s -- %s" % (rel, why))
    spec = importlib.util.spec_from_file_location(re.sub(r"\W", "_", rel), path)
    mod = importlib.util.module_from_spec(spec)
    held, sys.argv = sys.argv, [path]
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.argv = held
    return mod


ROOT = repo_root()
bal = _load(ROOT, "scripts/check-anchor-balance/check-anchor-balance.py",
            "this tool REUSES its row vocabulary (is_closed / split_row / row_name) "
            "and must not re-implement it.")
queue = _load(ROOT, "scripts/burndown-queue/burndown-queue.py",
              "this tool REUSES its priority banding and must not re-implement it.")

PLANS = ".plans"
BUCKETS = ("production", "harness", "done")
WORKING = ("production", "harness")
REL = {b: "%s/_deferred-anchor-registry-%s.md" % (PLANS, b) for b in BUCKETS}

TABLE_HEADER = "| Anchor | Priority | Status | Trigger | Closing work | Cross-refs |"
SEP_ROW_TEXT = "|---|---|---|---|---|---|"
CELL_TITLE = ("Anchor", "Priority", "Status", "Trigger", "Closing work", "Cross-refs")
# 1-based cell indices into `split_row`'s output. Named once; every reader below uses
# these rather than a literal, because a literal 2 was the whole of the old shape.
C_ANCHOR, C_PRIORITY, C_STATUS, C_TRIGGER, C_CLOSING, C_XREF = 1, 2, 3, 4, 5, 6
FIELD_COL = {"priority": C_PRIORITY, "status": C_STATUS, "trigger": C_TRIGGER,
             "closing": C_CLOSING, "cross_refs": C_XREF}

# The archive keeps one table per ORIGIN bucket, so a reopened row knows where it goes
# back to. The heading is the routing key, declared once.
DONE_TABLE = {"production": "## Closed — Production", "harness": "## Closed — Harness"}

# ★ THE THREE-VALUE STATUS VOCABULARY. Spelled glyph-first because `is_closed` tests the
# LEADING character; spelled with the word because a reader greps for `CLOSED`, not for
# a codepoint. `GATED` is OPEN as far as every count is concerned -- it says *why* the
# row cannot be picked up, which is the distinction `--schedulable` already draws.
STATUS = {"open": "🟠 OPEN", "gated": "⏳ GATED", "closed": "✅ CLOSED"}
STATUS_WORDS = tuple(STATUS)

# ★ THE ID SHAPE IS THE GUARD'S, NOT A NEW ONE. `check-anchor-registry` resolves `D-`
# plus THREE OR MORE `-`-separated segments; a two-segment `D-OPT` is an informal feature
# label it ignores, so a row named that way would be unreachable by the guard that exists
# to keep rows reachable. Spell a compound feature word as ONE segment (`ALWAYSINLINE`,
# not `ALWAYS-INLINE`) -- see D-PLANS-ANCHOR-NAME-SEGMENT-COUNT-GATE.
ANCHOR_ID = re.compile(r"^D-[A-Z0-9_]+(?:-[A-Za-z0-9_]+){2,}$")

# ⚠⚠ THE SEGMENT COUNT IS A **MINTING** RULE, NOT A MAINTENANCE ONE.
# [D-GATE-ANCHORS-WRITER-CANNOT-MAINTAIN-A-ROW-THE-REGISTRY-ALREADY-HOLDS]
# ✔MEASURED 2026-09-01 (P49): `set` refused `D-CSUBSET-VLA` -- an EXISTING row, cited 275
# times across 82 files -- so a finished closure could not be recorded by the only
# sanctioned writer. A writer that cannot maintain a row the registry already contains is
# not finished, and it fails in the FLATTERING-LOOKING direction: the rows it refuses to
# maintain are exactly the ones no guard is watching, so it seizes up hardest where the
# registry is weakest.
# ⇒ MINT (`write --insert`) keeps ANCHOR_ID whole: a NEW name must be guard-resolvable,
#   no exceptions. UPDATE (`write` without `--insert`, and every `set`) checks only that
#   the id is WELL-FORMED, because the identity did not come from the caller -- it came
#   from the registry.
# ★ A TYPO STILL CANNOT MINT ON THIS PATH, and this is the reason the count is not needed
#   as belt-and-braces: `place_row` already refuses an update whose anchor has NO row
#   ("no row for %s in any registry ... a mistyped id mints a second row"). *The id exists
#   in the registry* is a STRICTLY STRONGER identity check than the segment count ever was
#   here. Do not re-add the count -- it only re-breaks maintenance.
# ⓘ The threshold itself is the real defect and is sized in
#   [D-GATE-ANCHOR-REGISTRY-SEGMENT-THRESHOLD-HIDES-SEVENTY-ROWS]; this split is correct
#   whatever that threshold becomes, so it is not a placeholder for it.
ANCHOR_ID_WELLFORMED = re.compile(r"^D-[A-Za-z0-9_]+(?:-[A-Za-z0-9_]+)*$")

BACKTICKED_ID = re.compile(r"^`(D-[A-Za-z0-9_]+(?:-[A-Za-z0-9_]+)*)`$")


class Refused(Exception):
    pass


# ────────────────────────────────── reading ───────────────────────────────────

class Row(object):
    __slots__ = ("rel", "bucket", "table", "line_no", "raw", "cells", "name")

    def __init__(self, rel, bucket, table, line_no, raw, cells, name):
        self.rel, self.bucket, self.table = rel, bucket, table
        self.line_no, self.raw, self.cells, self.name = line_no, raw, cells, name

    def cell(self, idx):
        return self.cells[idx] if len(self.cells) > idx else ""

    @property
    def closed(self):
        return bal.is_closed(self.cell(C_STATUS))

    @property
    def status(self):
        return self.cell(C_STATUS).strip()

    @property
    def priority(self):
        return self.cell(C_PRIORITY).strip()


def read_rows(root, buckets=BUCKETS):
    """-> [Row] for every data row of every named registry, in file order.

    ⚠ TABLE-BASED, exactly like `scan_document`. A line-based reader counts a row that
    has drifted out of its table, which is how the predecessor gate traded one blind
    spot for another; here a drifted row simply is not found, and `--lint` says so.
    """
    out = []
    for b in buckets:
        rel = REL[b]
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            raise Refused("no registry at %s" % rel)
        lines = io.open(path, encoding="utf-8", newline="").read().split("\n")
        table = b
        i = 0
        while i < len(lines):
            for origin, heading in DONE_TABLE.items():
                if lines[i].strip() == heading:
                    table = origin
            if lines[i].strip() == TABLE_HEADER and i + 1 < len(lines) \
                    and bal.SEP_ROW.match(lines[i + 1]):
                i += 2
                while i < len(lines) and lines[i].lstrip().startswith("|"):
                    cells = bal.split_row(lines[i])
                    out.append(Row(rel, b, table, i + 1, lines[i], cells,
                                   bal.row_name(cells[1]) if len(cells) > 1 else ""))
                    i += 1
                continue
            i += 1
    return out


def find(root, anchor, buckets=BUCKETS):
    return [r for r in read_rows(root, buckets) if r.name == anchor]


def suggest_band(name, raw, table):
    """-> the band `burndown-queue`'s sieve would pick, for SEEDING a new row.

    ⚠ A SUGGESTION, NEVER A VERDICT, and the column exists precisely to end its reign.
    That instrument's own docstring says the band is *"a sort key, not a verdict"*: a
    census of "103 misglyphed rows" built from the same kind of keyword sieve turned out
    to be 4. Once written into the `Priority` cell the value is a DECLARATION -- a human
    may correct it, and the correction survives, which re-running a sieve never does.
    """
    b = queue.BUCKET_PRODUCTION if table == "production" else queue.BUCKET_HARNESS
    band, why, _demoted = queue.band_of(name, bal.strip_decoration(raw), b)
    return band, why


def render_full(row):
    out = ["anchor      : %s" % row.name,
           "registry    : %s%s" % (row.rel, ("  (table: %s)" % row.table)
                                   if row.bucket == "done" else ""),
           "priority    : %s" % (row.priority or "(unset)"),
           "status      : %s   -> %s" % (row.status or "(unset)",
                                         "CLOSED" if row.closed else "OPEN"),
           ""]
    for k in (C_TRIGGER, C_CLOSING, C_XREF):
        cell = row.cell(k).strip()
        out.append("%s:" % CELL_TITLE[k - 1])
        out.append("  " + (cell if cell else "(empty)"))
        out.append("")
    return "\n".join(out).rstrip() + "\n"


# ────────────────────────────────── writing ───────────────────────────────────

def make_cell(text):
    """One field -> one cell body. The three ways a hand-written cell breaks a row.

    1. NEWLINES ARE COLLAPSED. A row is ONE physical line; a wrapped anchor id does not
       fail, it disappears from every grep and mints a false id at the same time.
    2. EVERY RAW `|` IS ESCAPED to `\\|`. Unescaped it silently adds a column, shifting
       the status cell into the closing-work position for every reader.
    3. The result is padded with single spaces so the table reads as the file's rows do.

    ⚠⚠ THE INPUT IS RAW TEXT -- PIPES AS PIPES -- AND THAT IS WHAT MAKES THE ROUND TRIP
    WORK. `split_row` UNDOES `\\|` when it reads a cell, so a caller sees the pipe the
    author wrote; escaping every pipe on the way back out is exactly the inverse, which
    is what lets `set-anchor` carry an untouched cell through unchanged. An earlier
    draft skipped a pipe that already had a backslash before it, reasoning that this
    made the function idempotent. It does -- and idempotence is not the contract: under
    it, raw text containing a literal backslash before a pipe emits an UNESCAPED
    separator, which is the exact silent column-shift this function exists to prevent.
    Correctness on the declared input beats safety on an input that never arrives.
    """
    flat = " ".join(str(text).split())
    return " %s " % flat.replace("|", "\\|") if flat else " "


def normalise_priority(value):
    v = str(value).strip().upper()
    if v not in queue.BANDS:
        raise Refused("priority %r is not one of %s. The band is a DECLARATION now, not "
                      "a sieve result -- pick the one that is true and it survives every "
                      "later edit." % (value, " ".join(queue.BANDS)))
    return v


def normalise_status(value):
    """-> the canonical status cell. Accepts the word, the glyph form, or `done`."""
    v = str(value).strip()
    key = v.lower().lstrip("*_ ")
    if key in ("done", "close"):
        key = "closed"
    if key in STATUS:
        return STATUS[key]
    for canon in STATUS.values():
        if v == canon:
            return canon
    raise Refused(
        "status %r is not one of %s. The column is a three-value controlled vocabulary "
        "on purpose: before 2026-09-01 the verdict was the first glyph of a prose blob "
        "that also carried the trigger, the history and the retraction, and every reader "
        "had to agree where the verdict stopped." % (value, "/".join(STATUS_WORDS)))


def make_row(anchor, priority, status, trigger, closing="", cross_refs="", minting=True):
    """The six fields -> the canonical row text. Raises Refused.

    `minting` selects WHICH id rule applies -- see the ANCHOR_ID_WELLFORMED note. A NEW
    name must be guard-resolvable; an EXISTING one must only be well-formed, because
    `place_row` has already proved it names a row that exists."""
    if minting:
        if not ANCHOR_ID.match(anchor):
            raise Refused(
                "%r is not a well-formed anchor id. The guard resolves `D-` plus THREE or "
                "more `-`-separated segments; a two-segment name is an informal label it "
                "ignores, so a row named that way is unreachable by the guard that exists to "
                "keep rows reachable. Spell a compound feature word as ONE segment "
                "(ALWAYSINLINE, not ALWAYS-INLINE)." % anchor)
    elif not ANCHOR_ID_WELLFORMED.match(anchor):
        raise Refused(
            "%r is not a well-formed anchor id: it must be `D-` plus `-`-separated "
            "alphanumeric segments, on one line, with no whitespace and no `|`. This is "
            "the UPDATE path, so the segment COUNT is deliberately not re-litigated -- "
            "`place_row` refuses an id that names no existing row, which is a stronger "
            "identity check than counting segments. What is refused here is a name no "
            "registry could hold at all." % anchor)
    if not str(trigger).strip():
        raise Refused("the Trigger cell is empty. A row states what is wrong and what "
                      "would change it; a status glyph alone explains nothing to the "
                      "next reader.")
    row = "|" + "|".join([" `%s` " % anchor,
                          " %s " % normalise_priority(priority),
                          " %s " % normalise_status(status),
                          make_cell(trigger), make_cell(closing),
                          make_cell(cross_refs)]) + "|"
    # Self-check the product against the reader that will consume it, rather than
    # trusting the assembly above. Cheap, and the only thing that would catch a future
    # edit to `make_cell` that reintroduced a separator.
    cells = bal.split_row(row)
    if len(cells) != 8:
        raise Refused("assembled row splits into %d content cell(s), not 6 -- a field "
                      "carried a separator this writer failed to escape."
                      % (len(cells) - 2))
    if bal.row_name(cells[C_ANCHOR]) != anchor:
        raise Refused("assembled row reads back as %r, not %r"
                      % (bal.row_name(cells[C_ANCHOR]), anchor))
    return row


def _table_bounds(lines, heading=None):
    """-> (first_row_index, end_index) of the anchor table, optionally under `heading`.

    ⚠ THE TABLE IS LOCATED, NEVER GUESSED. The archive carries one table per origin
    bucket; picking the wrong one is invisible to a reader and still counted by every
    gate, so an ambiguous location refuses rather than choosing.
    """
    start = 0
    if heading is not None:
        hits = [i for i, ln in enumerate(lines) if ln.strip() == heading]
        if len(hits) != 1:
            raise Refused("found %d heading(s) %r; the archive's tables are addressed by "
                          "heading and picking between two would be invisible to a "
                          "reader." % (len(hits), heading))
        start = hits[0]
    heads = [i for i in range(start, len(lines))
             if lines[i].strip() == TABLE_HEADER
             and i + 1 < len(lines) and bal.SEP_ROW.match(lines[i + 1])]
    if not heads:
        raise Refused("no anchor table below %r" % (heading or "the file start"))
    i = heads[0] + 2
    first = i
    while i < len(lines) and lines[i].lstrip().startswith("|"):
        i += 1
    return first, i


def _rewrite(path, lines, write):
    if not write:
        return
    tmp = path + ".anchors-tmp"
    with io.open(tmp, "w", encoding="utf-8", newline="") as fh:
        fh.write("\n".join(lines))
    os.replace(tmp, path)


def place_row(root, working, anchor, row, write, insert=False, report=print):
    """Put `row` where its STATUS says it belongs, and remove it from anywhere else.

    `working` is `production` or `harness` -- the bucket decision, which no tool can make
    and which the caller therefore always states. The DESTINATION is derived: closed rows
    go to the archive's matching table, open and gated rows to the working registry.

    ⚠ APPEND, NEVER SORT. An alphabetical fold is how an opener/closer pair gets settled
    by position, which wrote an OPEN row back over a fixed defect three times in P42.

    -> the destination relpath. Raises Refused. Writes only when `write`.
    """
    if working not in WORKING:
        raise Refused("the bucket must be one of %s; the archive is a destination this "
                      "tool DERIVES, never one a caller declares for a live row."
                      % ", ".join(WORKING))
    closed = bal.is_closed(bal.split_row(row)[C_STATUS])
    dest = REL["done"] if closed else REL[working]
    heading = DONE_TABLE[working] if closed else None

    existing = {}
    for r in read_rows(root):
        if r.name == anchor:
            existing.setdefault(r.rel, []).append(r)
    dup = {rel: rows for rel, rows in existing.items() if len(rows) > 1}
    if dup:
        raise Refused(
            "%s already has %s -- a duplicate is read by a human, never settled here. Two "
            "lanes give one anchor two renditions; picking by position has written an "
            "OPEN row back over a fixed defect."
            % (anchor, "; ".join("%d rows in %s" % (len(v), k)
                                 for k, v in sorted(dup.items()))))
    if len(existing) > 1:
        raise Refused("%s has a row in %s. One id, one home -- which of them is the real "
                      "row is a human decision." % (anchor, " AND ".join(sorted(existing))))
    if not existing and not insert:
        raise Refused("no row for %s in any registry. If this row is NEW, say so with "
                      "--insert; otherwise check the spelling -- a mistyped id mints a "
                      "second row and leaves the real one untouched." % anchor)
    if existing and insert:
        raise Refused("--insert was given but %s already has a row in %s. Drop --insert "
                      "to replace it." % (anchor, ", ".join(sorted(existing))))

    was = next(iter(existing.values()))[0] if existing else None
    report("anchors: %s" % anchor)
    report("  status  %s -> %s"
           % ("(new)" if was is None else was.status,
              bal.split_row(row)[C_STATUS].strip()))
    report("  home    %s -> %s" % ("(new)" if was is None else was.rel, dest))
    if was is not None and was.rel != dest:
        report("  MOVE    deleted from %s, appended to %s" % (was.rel, dest))

    # ⚠⚠ A MOVE IS TWO FILE WRITES AND IT USED TO DELETE FIRST, SO A FAILURE BETWEEN
    # THEM LOST THE ROW ENTIRELY. [[D-GATE-ANCHORS-A-MOVE-IS-NOT-ATOMIC-AND-LOST-A-ROW]]
    # ✔MEASURED 2026-09-01 (P50) closing D-CSUBSET-NORETURN-NON-FUNCTION-OBJECT: the
    # delete landed, the append did not (rc=1, no `WROTE` line), and the row was left in
    # NO registry at all -- recovered only because HEAD still held it.
    # ★ THE DANGEROUS PART IS WHICH WAY IT FAILS. `check-anchor-balance` counts OPEN rows
    # BY NAME, so a row that vanished from both files reads exactly like a row that was
    # CLOSED: silent loss reported as progress, in the one direction a gate must never
    # fail in.
    # ⇒ TWO CHANGES, BOTH ABOUT ORDER RATHER THAN ABOUT LOCKING. (1) Every destination's
    # new content is computed BEFORE anything is written, so a malformed table or a
    # missing heading raises while the tree is still untouched. (2) The APPEND is written
    # FIRST and the deletions after, so an interrupted move leaves the row in BOTH places
    # -- which THIS function refuses loudly on its next call ("One id, one home") and a
    # human then settles. A duplicate is visible; a disappearance is not.
    # ⓘ This is not a transaction and does not pretend to be one: two files cannot be
    # renamed atomically. What it guarantees is that the surviving state always has AT
    # LEAST one copy of the row.
    pending = []

    dest_path = os.path.join(root, dest)
    dest_lines = io.open(dest_path, encoding="utf-8", newline="").read().split("\n")
    here = existing.get(dest)
    if here:
        dest_lines[here[0].line_no - 1] = row
    else:
        _first, end = _table_bounds(dest_lines, heading)
        dest_lines.insert(end, row)
    pending.append((dest_path, dest_lines))

    for rel, rows in sorted(existing.items()):
        if rel == dest:
            continue
        path = os.path.join(root, rel)
        lines = io.open(path, encoding="utf-8", newline="").read().split("\n")
        keep = [ln for k, ln in enumerate(lines) if k + 1 != rows[0].line_no]
        if len(keep) != len(lines) - 1:
            raise Refused("deleting %s from %s did not remove exactly one line"
                          % (anchor, rel))
        pending.append((path, keep))

    # `pending[0]` is the destination by construction above -- the append goes first.
    for path, lines in pending:
        _rewrite(path, lines, write)
    return dest


# ─────────────────────────────────── lint ─────────────────────────────────────

def lint(root):
    """-> [(rel, line_no, what)] every row a reader cannot key on with confidence."""
    out = []
    for r in read_rows(root):
        if len(r.cells) != 8:
            out.append((r.rel, r.line_no,
                        "%d content cells, not 6 -- the trailing columns are dropped or "
                        "shifted" % (len(r.cells) - 2)))
        first = r.cell(C_ANCHOR).strip()
        if not BACKTICKED_ID.match(first):
            out.append((r.rel, r.line_no,
                        "cell 1 is not a bare backticked id: %s" % first[:70]))
        if r.priority not in queue.BANDS:
            out.append((r.rel, r.line_no,
                        "Priority %r is not one of %s" % (r.priority,
                                                          " ".join(queue.BANDS))))
        if r.status not in STATUS.values():
            out.append((r.rel, r.line_no,
                        "Status %r is not one of %s"
                        % (r.status, " / ".join(STATUS.values()))))
        if not r.cell(C_TRIGGER).strip():
            out.append((r.rel, r.line_no, "empty Trigger cell -- the row explains nothing"))
        if r.bucket == "done" and not r.closed:
            out.append((r.rel, r.line_no, "OPEN row in the archive -- invisible to every "
                                          "queue in this project"))
        if r.bucket in WORKING and r.closed:
            out.append((r.rel, r.line_no, "CLOSED row in a working registry -- it belongs "
                                          "in the archive"))
        # The verdict now lives in two places, so they can disagree; the gate refuses
        # this too (ARM 6). Reported here so a lane sees it before the gate does.
        prose = r.cell(C_TRIGGER)
        if prose.strip() and bal.is_closed(prose) != r.closed:
            out.append((r.rel, r.line_no,
                        "the Status column and the verdict leading the Trigger prose "
                        "contradict each other"))
    return out


# ─────────────────────────────────── verbs ────────────────────────────────────

def _bucket_flags(ap, required=False):
    g = ap.add_mutually_exclusive_group(required=required)
    for b in BUCKETS:
        g.add_argument("--%s" % b, dest="bucket", action="store_const", const=b,
                       help="the %s registry" % b)


def _origin_of(anchor, bucket):
    """-> the WORKING registry a row belongs to. Raises Refused when it cannot be known.

    The archive is never a filing decision: a closed row's home there is derived from the
    working registry it came from, which is the fact `--done` cannot supply for a row
    that has never existed.
    """
    if bucket in WORKING:
        return bucket
    rows = find(ROOT, anchor)
    if not rows:
        raise Refused("--done needs an existing row to take its origin table from; a new "
                      "row is filed with --production or --harness and routed to the "
                      "archive from there when its status is closed.")
    return rows[0].table


def cmd_write(argv):
    ap = argparse.ArgumentParser(prog="write-anchor", add_help=True)
    _bucket_flags(ap, required=True)
    ap.add_argument("anchor")
    ap.add_argument("--priority", help="P0..P5; derived and PRINTED when omitted")
    ap.add_argument("--status", default="open",
                    help="one of %s (default open)" % "/".join(STATUS_WORDS))
    ap.add_argument("--trigger", required=True, help="the Trigger cell (the prose)")
    ap.add_argument("--closing", default="", help="the Closing work cell")
    ap.add_argument("--cross-refs", dest="cross_refs", default="")
    ap.add_argument("--insert", action="store_true", help="declare a NEW row")
    ap.add_argument("--apply", action="store_true", help="write; otherwise dry run")
    a = ap.parse_args(argv)

    working = _origin_of(a.anchor, a.bucket)
    status = normalise_status(a.status)
    if a.bucket == "done" and not bal.is_closed(status):
        raise Refused(
            "--done was given for a row whose status is %s. The archive is not a place "
            "work can hide: every queue in this project reads the two working registries "
            "only, so a live row filed there can never be picked up." % status)
    priority = a.priority
    if priority is None:
        probe = "| x | x | x | %s | %s | %s |" % (a.trigger, a.closing, a.cross_refs)
        priority, why = suggest_band(a.anchor, probe, working)
        print("anchors: priority not given, seeded %s from the burndown sieve (%s)."
              % (priority, why))
        print("  ⚠ A SUGGESTION. The column is a declaration -- correct it with "
              "`set-anchor --priority` and the correction survives every later edit.")
    # MINT only when --insert: otherwise this is an UPDATE of a row place_row
    # will refuse unless it already exists. See ANCHOR_ID_WELLFORMED.
    row = make_row(a.anchor, priority, status, a.trigger, a.closing, a.cross_refs,
                   minting=bool(a.insert))
    dest = place_row(ROOT, working, a.anchor, row, write=a.apply, insert=a.insert)
    print("  row     %d chars" % len(row))
    print("anchors: WROTE %s" % dest if a.apply
          else "anchors: dry run. pass --apply to write.")
    return 0


def cmd_set(argv):
    """Patch named fields on an EXISTING row; everything unnamed survives verbatim."""
    ap = argparse.ArgumentParser(prog="set-anchor", add_help=True)
    _bucket_flags(ap)
    ap.add_argument("anchor")
    ap.add_argument("--priority")
    ap.add_argument("--status", help="one of %s; `closed` MOVES the row to the archive"
                                     % "/".join(STATUS_WORDS))
    ap.add_argument("--trigger")
    ap.add_argument("--closing")
    ap.add_argument("--cross-refs", dest="cross_refs")
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args(argv)

    rows = find(ROOT, a.anchor, (a.bucket,) if a.bucket else BUCKETS)
    if not rows:
        raise Refused("no row for %s in %s. `set-anchor` edits an EXISTING row; a new one "
                      "is `write-anchor --insert`, which is a different act and says so."
                      % (a.anchor, a.bucket or "any registry"))
    if len(rows) > 1:
        raise Refused("%d rows carry %s (%s). A duplicate hands a reader two histories "
                      "under one name; it is read by a human, never settled by a tool."
                      % (len(rows), a.anchor, ", ".join(sorted({r.rel for r in rows}))))
    row = rows[0]
    given = {k: v for k, v in (("priority", a.priority), ("status", a.status),
                               ("trigger", a.trigger), ("closing", a.closing),
                               ("cross_refs", a.cross_refs)) if v is not None}
    if not given:
        raise Refused("nothing to set. Name at least one of --priority --status "
                      "--trigger --closing --cross-refs.")
    fields = {"priority": row.priority, "status": row.status,
              "trigger": row.cell(C_TRIGGER), "closing": row.cell(C_CLOSING),
              "cross_refs": row.cell(C_XREF)}
    for k, v in given.items():
        print("  %-10s %r -> %r" % (k, " ".join(str(fields[k]).split())[:48],
                                    " ".join(str(v).split())[:48]))
        fields[k] = v
    # `set` NEVER mints: it read this row's own cells above, so the id came from the
    # registry rather than from the caller.
    new = make_row(a.anchor, fields["priority"], fields["status"], fields["trigger"],
                   fields["closing"], fields["cross_refs"], minting=False)
    dest = place_row(ROOT, row.table, a.anchor, new, write=a.apply)
    print("anchors: WROTE %s" % dest if a.apply
          else "anchors: dry run. pass --apply to write.")
    return 0


def cmd_read(argv):
    ap = argparse.ArgumentParser(prog="read-anchor", add_help=True)
    _bucket_flags(ap)
    ap.add_argument("anchor")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    rows = find(ROOT, a.anchor, (a.bucket,) if a.bucket else BUCKETS)
    if not rows:
        print("anchors: no row for %s in %s." % (a.anchor, a.bucket or "any registry"))
        # Not silence: a near miss is the usual cause and the reader cannot see it.
        prefix = "-".join(a.anchor.split("-")[:2])
        near = sorted({r.name for r in read_rows(ROOT) if r.name.startswith(prefix)})
        if near:
            print("  %d id(s) share its namespace: %s"
                  % (len(near), ", ".join(near[:8]) + (" ..." if len(near) > 8 else "")))
        return 1
    if a.json:
        print(json.dumps([{"anchor": r.name, "registry": r.rel, "table": r.table,
                           "priority": r.priority, "status": r.status,
                           "closed": r.closed,
                           "trigger": r.cell(C_TRIGGER).strip(),
                           "closing": r.cell(C_CLOSING).strip(),
                           "cross_refs": r.cell(C_XREF).strip()}
                          for r in rows], ensure_ascii=False, indent=2))
        return 0
    for r in rows:
        sys.stdout.write(render_full(r))
        if r is not rows[-1]:
            print("-" * 78)
    if len(rows) > 1:
        print("⚠ %d rows carry this id. A duplicate hands a reader two histories under "
              "one name; it is read by a human, never settled by a tool." % len(rows))
    return 0


def cmd_list(argv):
    ap = argparse.ArgumentParser(prog="read-anchors", add_help=True)
    _bucket_flags(ap)
    ap.add_argument("--band", nargs="+", choices=queue.BANDS)
    ap.add_argument("--open", dest="only_open", action="store_true",
                    help="live rows only (open and gated)")
    ap.add_argument("--closed", dest="only_closed", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--lint", action="store_true",
                    help="report every row a reader cannot key on with confidence")
    a = ap.parse_args(argv)

    if a.lint:
        findings = lint(ROOT)
        for rel, line_no, what in findings:
            print("%s:%d   %s" % (rel, line_no, what))
        print("anchors: %d finding(s)." % len(findings))
        return 1 if findings else 0

    rows = read_rows(ROOT, (a.bucket,) if a.bucket else BUCKETS)
    if a.only_open:
        rows = [r for r in rows if not r.closed]
    if a.only_closed:
        rows = [r for r in rows if r.closed]
    items = [{"anchor": r.name, "priority": r.priority, "status": r.status,
              "registry": r.bucket, "table": r.table}
             for r in rows if not a.band or r.priority in a.band]
    if a.json:
        print(json.dumps(items, ensure_ascii=False, indent=2))
        return 0
    for it in items:
        print("%-3s %-9s %s" % (it["priority"], it["status"], it["anchor"]))
    print("anchors: %d row(s)%s." % (len(items), " in %s" % a.bucket if a.bucket else ""))
    return 0


# ────────────────────────────────── self-test ─────────────────────────────────

def self_test():
    """Red-on-disable for the writer. Every arm is a refusal or a MOVE.

    A tool that writes a good row into the right file is right by construction; what has
    to be exercised is each way a bad row lands looking fine, and each direction of the
    move-on-close rule. The controls are here so a guard that refuses EVERYTHING cannot
    produce this same clean transcript.
    """
    import tempfile
    failed = [0]
    # ⚠⚠ THE FIXTURE IDS ARE ASSEMBLED FROM FRAGMENTS, NEVER WRITTEN WHOLE.
    # `scripts/` is a scanned root for `check-anchor-registry`, so a three-segment `D-*`
    # written as ONE literal in this file is a CITATION that must resolve to a registry
    # row -- and these are INPUT DATA to a parser test, not citations. ✔MEASURED: written
    # whole they red the entire tree. The project already settled this class -- make the
    # fixture stop being anchor-shaped, never allowlist the name, because an Allowlist
    # entry silences a name repo-wide and forever and would blind the guard to a real
    # future anchor. The VALUE is still well-formed at runtime, which is what `make_row`
    # validates; only the LITERAL is split.
    _FX = "D-" + "FIXTURE-ANCHORS"
    A_ = _FX + "-ALPHA"
    B_ = _FX + "-BETA"
    N_ = _FX + "-NOSUCH"
    CC_ = _FX + "-CELLS"
    HG_ = _FX + "-HGAMMA"


    def pin(ok, why, detail=""):
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", why,
                               ("   " + detail) if detail else ""))
        if not ok:
            failed[0] += 1

    O = "| `" + _FX + "-%s` | P2 | 🟠 OPEN | 🟠 **OPEN** | w | r |"
    C = "| `" + _FX + "-%s` | P2 | ✅ CLOSED | ✅ **CLOSED** | - | r |"

    def box(tmp):
        os.makedirs(os.path.join(tmp, PLANS), exist_ok=True)

        def doc(rel, *body):
            with io.open(os.path.join(tmp, rel), "w", encoding="utf-8", newline="") as f:
                f.write("\n".join(body) + "\n")
        doc(REL["production"], "# p", "", TABLE_HEADER, SEP_ROW_TEXT,
            O % "ALPHA", O % "BETA", "")
        doc(REL["harness"], "# h", "", TABLE_HEADER, SEP_ROW_TEXT,
            "| `" + HG_ + "` | P3 | 🟠 OPEN | 🟠 **OPEN** | w | r |", "")
        doc(REL["done"], "# d", "", DONE_TABLE["production"], "", TABLE_HEADER,
            SEP_ROW_TEXT, C % "OLDP", "", DONE_TABLE["harness"], "", TABLE_HEADER,
            SEP_ROW_TEXT, "| `" + _FX + "-OLDH` | P3 | ✅ CLOSED | ✅ **CLOSED** | - | r |", "")

    def refuse(fn, *a, **k):
        try:
            fn(*a, **k)
            return None
        except Refused as exc:
            return str(exc)

    # ── the row ASSEMBLER refuses what a hand-written row gets wrong ──────────
    pin("not a well-formed anchor id" in (refuse(make_row, "D-TWO", "P1", "open", "t") or ""),
        "(1) a two-segment id is REFUSED when MINTING -- the guard would never resolve it")
    # ── (1b/1c/1d) THE SEGMENT COUNT IS A MINTING RULE, NOT A MAINTENANCE ONE ──────
    # [D-GATE-ANCHORS-WRITER-CANNOT-MAINTAIN-A-ROW-THE-REGISTRY-ALREADY-HOLDS]
    # ⚠ (1d) IS THE ARM THAT MAKES THE OTHER TWO MEAN ANYTHING. Without a control that a
    # guard-resolvable id STILL MINTS, (1b) and (1c) both pass over a predicate deleted
    # outright -- the fixture would be measuring nothing, which is the exact class this
    # cycle closed in `lowerCToLir`.
    pin(refuse(make_row, "D-TWO", "P1", "open", "t", minting=False) is None,
        "(1b) the SAME two-segment id UPDATES -- identity came from the registry, and "
        "place_row already refuses an id that names no row")
    pin("not a well-formed anchor id" in (refuse(make_row, "D-TWO", "P1", "open", "t",
                                                 minting=True) or ""),
        "(1c) ... and is still REFUSED on the minting path, so --insert cannot smuggle "
        "an unresolvable NEW name in")
    pin(refuse(make_row, _FX + "-MINTABLE", "P1", "open", "t", minting=True) is None,
        "(1d) CONTROL: a guard-resolvable id still MINTS -- without this, (1b)+(1c) pass "
        "over a predicate that was deleted rather than moved")
    pin("not a well-formed anchor id" in (refuse(make_row, "D-has space", "P1", "open",
                                                 "t", minting=False) or ""),
        "(1e) the UPDATE path still refuses a name no registry could hold at all")
    pin("Trigger cell is empty" in (refuse(make_row, CC_, "P1", "open", " ") or ""),
        "(2) an empty Trigger cell is REFUSED")
    pin("is not one of" in (refuse(make_row, CC_, "P9", "open", "t") or ""),
        "(3) a priority outside P0..P5 is REFUSED")
    pin("controlled vocabulary" in (refuse(make_row, CC_, "P1", "wibble", "t") or ""),
        "(4) a status outside the three-value vocabulary is REFUSED")
    r = make_row(CC_, "P0", "closed", "verdict | with a raw pipe", "a\nb", "")
    c = bal.split_row(r)
    pin(len(c) == 8 and "with a raw pipe" in c[C_TRIGGER]
        and c[C_CLOSING].strip() == "a b" and bal.is_closed(c[C_STATUS]),
        "(5) a raw pipe is ESCAPED and a newline COLLAPSED -- neither can add a column "
        "or wrap an id", "cells=%d" % (len(c) - 2))
    # ★ THE ROUND TRIP AS A PROPERTY, not as an escape spelling: read a cell back out,
    # feed it straight in again, and the cell must be identical. That is exactly what
    # `set-anchor` does to every field it was not asked to change.
    _rt = bal.split_row(make_row(CC_, "P1", "open", "a | b", "c | d"))
    _rt2 = bal.split_row(make_row(CC_, "P1", "open",
                                  _rt[C_TRIGGER], _rt[C_CLOSING]))
    pin(_rt[C_TRIGGER] == _rt2[C_TRIGGER] and _rt[C_CLOSING] == _rt2[C_CLOSING]
        and _rt[C_TRIGGER].strip() == "a | b",
        "(6) a cell READ from a row and written straight back is IDENTICAL -- the round "
        "trip `set-anchor` depends on", "got=%r" % _rt2[C_TRIGGER])
    pin(bal.split_row(make_row(CC_, "P1", "done", "t"))[C_STATUS].strip()
        == STATUS["closed"],
        "(7) `done` is accepted as a spelling of `closed` -- the operator's own word")

    with tempfile.TemporaryDirectory() as tmp:
        box(tmp)
        quiet = lambda *a, **k: None

        # ── CLOSING a row MOVES it out of the working registry ────────────────
        dest = place_row(tmp, "production", A_,
                         make_row(A_, "P1", "closed", "✅ **CLOSED**", "w", "r"),
                         write=True, report=quiet)
        prod = io.open(os.path.join(tmp, REL["production"]), encoding="utf-8").read()
        done = io.open(os.path.join(tmp, REL["done"]), encoding="utf-8").read()
        pin(dest == REL["done"] and A_ not in prod and A_ in done,
            "(8) a CLOSED row is DELETED from the working registry and appended to the "
            "archive", "dest=%s" % dest)
        pin(done.index(A_) < done.index(DONE_TABLE["harness"]),
            "(9) ...into the PRODUCTION table of the archive, not the harness one")
        pin(B_ in prod and HG_ in
            io.open(os.path.join(tmp, REL["harness"]), encoding="utf-8").read(),
            "(10) the sibling rows are untouched")

        # ── REOPENING moves it BACK ───────────────────────────────────────────
        dest = place_row(tmp, "production", A_,
                         make_row(A_, "P1", "open", "🟠 **OPEN -- regressed**"),
                         write=True, report=quiet)
        prod = io.open(os.path.join(tmp, REL["production"]), encoding="utf-8").read()
        done = io.open(os.path.join(tmp, REL["done"]), encoding="utf-8").read()
        pin(dest == REL["production"] and A_ in prod
            and A_ not in done,
            "(11) reopening moves the row BACK out of the archive, never edits it in "
            "place", "dest=%s" % dest)
        # ...and a GATED row is live, so it goes to the working registry too.
        dest = place_row(tmp, "production", A_,
                         make_row(A_, "P1", "gated", "🟠 **OPEN -- gated**"),
                         write=True, report=quiet)
        pin(dest == REL["production"],
            "(12) GATED is LIVE -- it stays in the working registry, where a queue can "
            "see why it cannot be picked up")

        # ── the refusals ──────────────────────────────────────────────────────
        msg = refuse(place_row, tmp, "production", N_,
                     make_row(N_, "P1", "open", "t"), True, report=quiet)
        pin(msg is not None and "--insert" in msg,
            "(13) a row that does not exist is REFUSED unless --insert is declared")
        msg = refuse(place_row, tmp, "production", B_,
                     make_row(B_, "P1", "open", "t"), True, insert=True,
                     report=quiet)
        pin(msg is not None and "already has a row" in msg,
            "(14) --insert over an EXISTING row is REFUSED")
        msg = refuse(place_row, tmp, "done", B_,
                     make_row(B_, "P1", "closed", "t"), True, report=quiet)
        pin(msg is not None and "must be one of" in msg,
            "(15) the archive is never declared as a destination -- it is DERIVED")

        # ── a DUPLICATE across two files is refused, never settled ────────────
        lines = io.open(os.path.join(tmp, REL["harness"]), encoding="utf-8",
                        newline="").read().split("\n")
        lines.insert(5, O % "BETA")
        io.open(os.path.join(tmp, REL["harness"]), "w", encoding="utf-8",
                newline="").write("\n".join(lines))
        pin(len({r.bucket for r in find(tmp, B_)}) == 2,
            "(16) an id filed in TWO registries is FOUND in both, never silently halved")
        msg = refuse(place_row, tmp, "production", B_,
                     make_row(B_, "P1", "open", "t"), True, report=quiet)
        pin(msg is not None and "One id, one home" in msg,
            "(17) ...and writing it is REFUSED rather than picking a file")

        # ── the LINT arms, each with its control ──────────────────────────────
        box(tmp)
        pin(lint(tmp) == [], "(18) the CONTROL: a clean fixture lints clean, so every "
                             "arm below is not firing on everything")
        for label, rel, at, row, expect in (
                ("(19) a CLOSED row left in a working registry", REL["production"], 5,
                 C % "STUCK", "CLOSED row in a working registry"),
                ("(20) an OPEN row in the ARCHIVE -- the dangerous direction",
                 REL["done"], 7, O % "HIDDEN", "OPEN row in the archive"),
                ("(21) a Priority outside the band vocabulary", REL["production"], 5,
                 "| `" + _FX + "-BAND` | P9 | 🟠 OPEN | t | w | r |", "Priority"),
                ("(22) a Status outside the three-value vocabulary", REL["production"],
                 5, "| `" + _FX + "-VOCAB` | P2 | ORANGE | t | w | r |", "Status"),
                ("(23) a Status column contradicting its own Trigger prose",
                 REL["production"], 5,
                 "| `" + _FX + "-SPLIT` | P2 | ✅ CLOSED | 🟠 **OPEN** | w | r |",
                 "contradict")):
            box(tmp)
            lines = io.open(os.path.join(tmp, rel), encoding="utf-8",
                            newline="").read().split("\n")
            lines.insert(at, row)
            io.open(os.path.join(tmp, rel), "w", encoding="utf-8",
                    newline="").write("\n".join(lines))
            pin(any(expect in f[2] for f in lint(tmp)), "%s is a LINT FINDING" % label,
                "got=%r" % [f[2][:40] for f in lint(tmp)][:3])

        # ── a DRY RUN writes nothing ──────────────────────────────────────────
        box(tmp)
        before = io.open(os.path.join(tmp, REL["production"]), encoding="utf-8").read()
        place_row(tmp, "production", B_,
                  make_row(B_, "P1", "closed", "t"), write=False, report=quiet)
        pin(io.open(os.path.join(tmp, REL["production"]), encoding="utf-8").read()
            == before, "(24) a dry run writes nothing at all")

        # ── (25)(26) A MOVE NEVER LOSES THE ROW, EVEN INTERRUPTED ─────────────
        # [[D-GATE-ANCHORS-A-MOVE-IS-NOT-ATOMIC-AND-LOST-A-ROW]]. The order is the
        # whole fix: the APPEND is written first, so an interruption leaves the row
        # in BOTH files -- refused loudly on the next call -- instead of in NEITHER,
        # which the balance gate would read as a closure. Simulated by failing the
        # SECOND write, which is where the old order lost the row.
        box(tmp)
        real_rewrite = globals()["_rewrite"]
        calls = []

        def _fail_after_first(path, lines, write):
            calls.append(path)
            if len(calls) > 1:
                raise IOError("simulated failure on the second write of the move")
            return real_rewrite(path, lines, write)

        globals()["_rewrite"] = _fail_after_first
        try:
            place_row(tmp, "production", B_, make_row(B_, "P1", "closed", "t"),
                      write=True, report=quiet)
        except IOError:
            pass
        finally:
            globals()["_rewrite"] = real_rewrite
        prod = io.open(os.path.join(tmp, REL["production"]), encoding="utf-8").read()
        done = io.open(os.path.join(tmp, REL["done"]), encoding="utf-8").read()
        pin(("`%s`" % B_) in prod or ("`%s`" % B_) in done,
            "(25) an INTERRUPTED move leaves the row SOMEWHERE, never nowhere")
        pin(("`%s`" % B_) in done,
            "(26) ... and specifically in the DESTINATION, because the append goes first")

    print("anchors self-test: %d failed" % failed[0])
    return 1 if failed[0] else 0


VERBS = {"write": cmd_write, "set": cmd_set, "read": cmd_read, "list": cmd_list}


def main(argv):
    if "--self-test" in argv or "--selftest" in argv:
        return self_test()
    if not argv or argv[0] not in VERBS:
        print(__doc__)
        return 3
    try:
        return VERBS[argv[0]](argv[1:])
    except Refused as exc:
        print("anchors: REFUSED -- %s" % exc)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
