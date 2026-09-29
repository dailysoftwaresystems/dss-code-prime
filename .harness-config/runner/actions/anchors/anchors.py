#!/usr/bin/env python3
# PURPOSE: read and lint deferred-anchor registry rows, and launch the one door that writes them, dssharness write-anchor and set-anchor.
"""anchors.py -- THE READER OF THE TWO ANCHOR REGISTRIES, AND THE ONE LAUNCHER OF THEIR DOOR.

Operator, 2026-09-01, in three instructions this tool has answered together ever since:
  * *"we need to make this deterministic: inside scripts we must have anchors directory,
    inside it (all with options like --done or --production): write-anchor,
    read-anchor and read-anchors. Write you pass the parameters and it writes in the
    correct form. read 1 brings the full anchor result, read all brings the name,
    priority and status only. we must also ensure now that the anchor format is correct
    so the read does not fail."*
  * *"add columns for priority and status ... then the write explicitly writes it
    correctly, this way we always have clean statuses."*
  * *"set-anchor, where you can set anything on an existing anchor, by name"* --
    *"because when setting as done, it automatically moves to the done anchors."*

★★★ THE DOOR IS `dssharness write-anchor` / `set-anchor`, AND NOTHING ELSE WRITES A ROW.
Since the DssHarness migration the tool owns the registries' write path: it assembles the
six cells, escapes the pipes, refuses a pre-escaped pipe, an empty Trigger and a new id no
guard could resolve, refuses a duplicate, and MOVES a row between the working registry and
the archive by its status. This file used to be that writer, and a second writer is two
programs that will one day disagree about where a closed row goes -- so its write path is
DELETED. What stays is what a reader, a guard or a caller of the door needs:
  * the READER -- `read_rows`, `find`, `lint`, and the `read` / `list` verbs;
  * the VOCABULARY -- the status words and cells, the bands, the table shape -- so a caller
    maps a lane's text to the door's words through one table;
  * the LAUNCHER -- `door_write`, the one composition of a door call that `lane-fold`,
    `apply-registry-row` and `anchor-rows` share: the cells go by FILE (a 48 KB row once crossed
    Windows' 32,767-character command line), an update names only the fields that CHANGE (a cell the
    door is not asked to write keeps its bytes), and the call runs without the caller's git
    selection. It refuses, BEFORE the door, what is this repository's vocabulary (a status or band
    outside it, the retired disclosed spelling), an update naming no field, and text the door would
    store BROKEN without seeing it, judged AS THE DOOR WILL STORE IT (`door_form`): an anchor id broken
    by a line break or a joined-line space, and a path cut the same way (`cell_refusals`, keyed on the
    id grammar the tree's config.json declares, `id_grammar`). Its
    report-#7 refusals (a Status/Trigger verdict split, in-line whitespace the door collapsed, a
    leading BOM) were DELETED when DssHarness 0.5.9 closed that gap: the door owns them now,
    the verdict rule through `anchors.triggerCarriesVerdict: true`.

THE ROW SHAPE, since 2026-09-01:

    | Anchor | Priority | Status | Trigger | Closing work | Cross-refs |

`Priority` is `P0`..`P5`; `Status` is the door's controlled vocabulary, spelled `✅ CLOSED`,
`🟠 OPEN`, `⏳ GATED` and `🔵 DISCLOSED` -- the last is open work whose debt PRE-DATES this
cycle: OPEN in every count, exempt only from the balance gate's net-increase refusal.
⚠ THE STATUS CELL KEEPS ITS GLYPH AND THAT IS THE CONTRACT, NOT DECORATION. This project's one
definition of closed is *"the cell OPENS with ✅ after stripping `*_ `"* -- the complement
defined, never the variants, so a glyph nobody has thought of yet counts OPEN.

⚠ NOTHING HERE DECIDES WHAT "CLOSED" MEANS, WHAT A ROW IS NAMED, OR HOW A PRIORITY IS
SEEDED. `is_closed`, `split_row` and `row_name` come from `check-anchor-balance`; the
suggested band comes from `burndown-queue`. Each carries a comment history of defects that
re-typing would re-open, and the standing order is explicit: use the program that exists,
and fix it rather than routing around it.

★ THE READ NEVER FAILS ON A HISTORICAL ROW, AND IT DOES NOT PRETEND THEY ARE FINE.
✔MEASURED 2026-09-01 over all 2,078 rows: after normalisation every row carries exactly
six cells, and exactly FIVE -- all in the archive, all closed -- carry a cell 1 that is not
a bare backticked id. Rewriting any of those would MINT an id or destroy a citation, so they
are read through `row_name` (which strips decoration and never fails) and REPORTED by
`--lint`.

Exit codes: 0 OK · 1 not found / lint findings · 2 refused (nothing written) · 3 usage error.

Usage:
    anchors.py read  D-<AREA>-<NAME> [--production|--done] [--json]
    anchors.py list  [--production|--done] [--band P0 P1] [--open|--closed] [--json] [--lint]
    anchors.py --self-test
A row is WRITTEN with `dssharness write-anchor` (new) or `dssharness set-anchor` (existing).
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
import io
import json
import os
import re
import shutil
import sys
import tempfile
sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - odd stream
        pass

HERE = os.path.dirname(os.path.abspath(__file__))


def _owning_tree():
    """`.harness-config/runner/actions/owning-tree/owning-tree.py` -- the one owner of "which tree is this file in?".

    Loaded by path from this file's sibling directory (a hyphen is not a module name). It FAILS
    LOUD when absent rather than falling back to a local walk: a second copy of the answer is the
    drift that owner exists to end.
    """
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                        "owning-tree", "owning-tree.py")
    if not os.path.isfile(path):
        sys.exit("anchors: cannot find %s -- this tool's root is resolved there and nowhere else"
                 % path)
    spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def repo_root():
    """The tree THIS FILE lives in: the nearest ancestor holding BOTH `.plans/` and `.harness-config/`.

    Not a fixed number of `dirname` calls -- the depth-hardcoding defect the P17
    consolidation had to repair in seventeen scripts at once. The walk has ONE owner,
    `owning-tree.py`, so no second spelling of it can drift.
    ⚠ STATED PLAINLY, BECAUSE A PIN IS EASY TO OVER-READ: the root is keyed on this file and
    never on the caller's working directory; the self-test's root arms pin that no cwd-keyed
    root can replace it unseen.
    """
    ot = _owning_tree()
    try:
        return ot.resolve(__file__)
    except ot.Refusal as exc:
        sys.exit("anchors: %s" % exc)


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
bal = _load(ROOT, ".harness-config/runner/actions/check-anchor-balance/check-anchor-balance.py",
            "this tool REUSES its row vocabulary (is_closed / split_row / row_name) "
            "and must not re-implement it.")
queue = _load(ROOT, ".harness-config/runner/actions/burndown-queue/burndown-queue.py",
              "this tool REUSES its priority banding and must not re-implement it.")

PLANS = ".plans"
# ★★★ TWO REGISTRIES SINCE 2026-09-16, AND THE THIRD IS NOT COMING BACK.
# The harness registry was retired by the DssHarness migration: the harness becomes that
# tool's responsibility, and every anchor from here on is a PRODUCTION anchor. Its rows are
# readable in git at the parent of the commit that deleted them.
# ⚠ THESE ARE THE DOOR'S FILES TOO -- `anchors.pendingAnchorsPath` / `doneAnchorsPath` in
# `.harness-config/config.json` -- and the self-test proves the two agree, because a reader
# of one pair of files beside a door writing another would read a registry nobody writes.
BUCKETS = ("production", "done")
WORKING = ("production",)
REL = {b: "%s/_deferred-anchor-registry-%s.md" % (PLANS, b) for b in BUCKETS}
CONFIG_REL = ".harness-config/config.json"
DOOR_CONFIG_KEYS = {"production": "pendingAnchorsPath", "done": "doneAnchorsPath"}

TABLE_HEADER = "| Anchor | Priority | Status | Trigger | Closing work | Cross-refs |"
SEP_ROW_TEXT = "|---|---|---|---|---|---|"
CELL_TITLE = ("Anchor", "Priority", "Status", "Trigger", "Closing work", "Cross-refs")
# 1-based cell indices into `split_row`'s output. Named once; every reader uses these rather
# than a literal, because a literal 2 was the whole of the old four-cell shape.
C_ANCHOR, C_PRIORITY, C_STATUS, C_TRIGGER, C_CLOSING, C_XREF = 1, 2, 3, 4, 5, 6
FIELD_COL = {"priority": C_PRIORITY, "status": C_STATUS, "trigger": C_TRIGGER,
             "closing": C_CLOSING, "cross_refs": C_XREF}

# The archive keeps one table per ORIGIN bucket, so a reopened row knows where it goes
# back to. The heading is the routing key, declared once.
DONE_TABLE = {"production": "## Closed — Production"}

# ★ THE STATUS VOCABULARY IS THE DOOR'S: the four words `dssharness write-anchor --status`
# takes, and the four cells it stores for them. Spelled glyph-first because `is_closed` tests
# the LEADING character; spelled with the word because a reader greps for `CLOSED`, not for a
# codepoint. `GATED` is OPEN as far as every count is concerned -- it says *why* the row cannot
# be picked up.
# ★★ `disclosed` IS COMPOSED FROM THE GATE'S MARK, never re-typed: `check-anchor-balance`
# exempts a row whose status cell OPENS with `bal.DISCLOSED_MARK` from its net-increase
# refusal, and the self-test asks the GATE about this cell, so the vocabulary and the gate
# have one owner between them. A DISCLOSED row is OPEN WORK and nothing about it is softened:
# every count counts it, and the claim it makes -- the debt PRE-DATES this cycle -- is checkable
# against the base ref, so marking a defect you introduced is a false statement about history.
# ⚠ THE RETIRED SPELLING `🔵 🟠 OPEN (DISCLOSED)` IS REFUSED: the door stores `🔵 DISCLOSED`,
# and `dssharness read-anchors --lint` reports the old cell (✔MEASURED 2026-09-23, 0.5.8).
STATUS = {"open": "🟠 OPEN", "gated": "⏳ GATED", "closed": "✅ CLOSED",
          "disclosed": "%s DISCLOSED" % bal.DISCLOSED_MARK}
STATUS_WORDS = tuple(STATUS)
RETIRED_DISCLOSED = "%s %s (DISCLOSED)" % (bal.DISCLOSED_MARK, STATUS["open"])

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
    """-> the band `burndown-queue`'s sieve would pick, for SEEDING a new row's priority.

    ⚠ A SUGGESTION, NEVER A VERDICT, and the column exists precisely to end its reign.
    That instrument's own docstring says the band is *"a sort key, not a verdict"*: a
    census of "103 misglyphed rows" built from the same kind of keyword sieve turned out
    to be 4. Once written into the `Priority` cell the value is a DECLARATION -- a human
    may correct it, and the correction survives, which re-running a sieve never does.
    The door requires a priority, so a caller that has none seeds it here and SAYS so.
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


# ──────────────────────────────── vocabulary ──────────────────────────────────

def normalise_priority(value):
    v = str(value).strip().upper()
    if v not in queue.BANDS:
        raise Refused("priority %r is not one of %s. The band is a DECLARATION now, not "
                      "a sieve result -- pick the one that is true and it survives every "
                      "later edit." % (value, " ".join(queue.BANDS)))
    return v


def normalise_status(value):
    """-> the canonical status CELL. Accepts the word, the cell the door stores, or `done`."""
    v = str(value).strip()
    key = v.lower().lstrip("*_ ")
    if key in ("done", "close"):
        key = "closed"
    if key in STATUS:
        return STATUS[key]
    for canon in STATUS.values():
        if v == canon:
            return canon
    if bal.strip_decoration(v) == bal.strip_decoration(RETIRED_DISCLOSED):
        raise Refused(
            "status %r is the RETIRED spelling of `disclosed`: the door stores %r, and "
            "`dssharness read-anchors --lint` reports the old cell. Write the word `disclosed`."
            % (value, STATUS["disclosed"]))
    raise Refused(
        "status %r is not one of %s. The column is a CONTROLLED VOCABULARY on purpose: "
        "before 2026-09-01 the verdict was the first glyph of a prose blob that also "
        "carried the trigger, the history and the retraction, and every reader had to "
        "agree where the verdict stopped. `disclosed` is OPEN work whose debt PRE-DATES "
        "this cycle -- it counts as open everywhere and is exempt from the balance "
        "gate's net-increase FAILURE and from nothing else; claiming it for a defect "
        "this cycle introduced is a false statement about history, and the reviewer can "
        "check it against the base ref." % (value, "/".join(STATUS_WORDS)))


def status_word(cell):
    """-> the door's WORD for a canonical status cell (`normalise_status`'s output)."""
    for word, canon in STATUS.items():
        if cell == canon:
            return word
    raise Refused("%r is not a status cell the door stores" % (cell,))


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
        # The verdict lives in two places, so they can disagree; the gate refuses this too
        # (ARM 6). Reported here so a lane sees it before the gate does.
        prose = r.cell(C_TRIGGER)
        if prose.strip() and bal.is_closed(prose) != r.closed:
            out.append((r.rel, r.line_no,
                        "the Status column and the verdict leading the Trigger prose "
                        "contradict each other"))
    return out


# ───────────────────────────────── the door ───────────────────────────────────
#
# ★★★ THE ONE COMPOSITION OF A ROW WRITE. `lane-fold` (a lane's landing), `apply-registry-row`
# (a lane's one-line row file) and `anchor-rows` (a fold's batch of cell files) all end here, so
# the argv, the cell files, the executable and the refusals below exist once.
# ✔MEASURED 2026-09-23 on DssHarness 0.5.8, in git fixture trees: the door refuses a
# pre-escaped pipe, an empty Trigger, a new id no guard could resolve, a priority or status
# outside the vocabulary, `write-anchor` over an existing row, `set-anchor` on a missing one,
# and one id in both registries; it escapes a raw pipe, joins a cell's lines, moves a row by
# its status, requires a priority, and carries every cell it was not asked to write VERBATIM.
# What it does NOT do is below, each refusal named after the upstream gap it covers.

DOOR_NAMES = ("dssharness", "DssHarness")
DOOR_VERBS = {True: "write-anchor", False: "set-anchor"}
DOOR_REFUSED = 2          # this launcher's own refusal: nothing was asked of the door
DOOR_SCALARS = (("priority", "--priority"), ("status", "--status"))
DOOR_CELLS = (("trigger", "--trigger-file"), ("closing", "--closing-file"),
              ("cross_refs", "--cross-refs-file"))
BOM = "﻿"


def door_executable(environ=None):
    """-> the DssHarness executable: PATH first, then the tool installer's own directory.

    ⚠ NOT A BARE NAME ON EVERY HOST. ✔MEASURED (the root CMakeLists.txt keeps the history):
    the file's CASE followed the release (`DssHarness` through 0.5.5, `dssharness` from 0.5.6),
    and `<home>/.dotnet/tools` is on the LOGIN PATH only, so a non-login shell on Linux finds
    neither spelling. Both names are tried, on PATH and in that directory. A host without the
    tool REFUSES: nothing may be written, and the refusal says how to install it.
    """
    env = os.environ if environ is None else environ
    for name in DOOR_NAMES:
        hit = shutil.which(name, path=env.get("PATH", os.defpath))
        if hit:
            return hit
    tried = []
    for var in ("HOME", "USERPROFILE"):
        home = env.get(var, "")
        if not home:
            continue
        for name in DOOR_NAMES:
            for leaf in (name, name + ".exe"):
                cand = os.path.join(home, ".dotnet", "tools", leaf)
                tried.append(cand)
                if os.path.isfile(cand) and os.access(cand, os.X_OK):
                    return cand
    raise Refused(
        "no DssHarness on this host (searched PATH for %s, and %s). Every registry row is "
        "written by `dssharness write-anchor` / `set-anchor` and by nothing else, so nothing "
        "was written. Install it with `dotnet tool install --global DssHarness`."
        % (" and ".join(DOOR_NAMES), ", ".join(tried) or "no HOME / USERPROFILE to look under"))


def _anchors_section(root):
    """-> the `anchors` section of `root`'s `.harness-config/config.json`, the section the door reads. A file that
    cannot be read or parsed, or a section that is missing, is REFUSED -- with THIS module's `Refused`, so a caller
    catches one class (owning-tree's is a fresh class per load)."""
    ot = _owning_tree()
    try:
        cfg = ot.load_jsonc(os.path.join(root, *CONFIG_REL.split("/")))
    except ot.Refusal as exc:
        raise Refused(str(exc))
    anchors = cfg.get("anchors") if isinstance(cfg, dict) else None
    if not isinstance(anchors, dict):
        raise Refused("%s declares no `anchors` section, so the door has no registries" % CONFIG_REL)
    return anchors


def door_config(root):
    """-> {bucket: rel} the door writes, read from `root`'s `.harness-config/config.json`."""
    anchors = _anchors_section(root)
    return dict((b, str(anchors.get(key, ""))) for b, key in DOOR_CONFIG_KEYS.items())


# ⓘ REPORT #7 IS THE DOOR'S (DssHarness 0.5.9, ✔MEASURED 2026-09-24). This launcher refused, before
# asking the door, a row whose Status and Trigger state DIFFERENT verdicts (the rows
# `check-anchor-balance` ARM 6 fails), a cell whose in-line whitespace 0.5.8 would collapse, and a
# cell led by a byte-order mark. 0.5.9 owns all three: with `anchors.triggerCarriesVerdict: true`
# (config.json) write-anchor and set-anchor refuse a verdict split either way round (exit 10), a
# non-UTF-8 or BOM-led cell file is refused (exit 10), and runs of spaces, TABs and NBSPs are KEPT
# (only a line break, with the whitespace beside it, becomes one space). The copies here were
# deleted: one owner per rule, and the whitespace one had turned WRONG (it refused text the door
# now stores byte for byte). Self-test arm (14) pins the config key the door's check rides on.


# ★ WHAT THE DOOR STORES (`door_form`; ✔MEASURED 2026-09-25 on DssHarness 0.5.12, and pinned against the REAL door
# by anchor-rows' parity arm (p1)): a line break collapses, with the whitespace either side of it, into ONE space;
# every other run of spaces or tabs is kept; the value's ends are trimmed. EVERY CONTENT REFUSAL BELOW JUDGES A CELL
# AS THE DOOR WILL STORE IT: a check of the raw text cannot see the break the door is about to join -- round 13's
# path-cut refusal matched a literal space, so a path wrapped at its `/` held a line break, passed, and would have
# been stored cut (P68 round 13's audit, F1-A1).
_BREAK = re.compile(r"\s*(?:\r\n|\r|\n)\s*")


def door_form(text):
    """What the door STORES for `text`: each line break, with the whitespace either side of it, becomes one space;
    every other run of spaces or tabs is kept; the ends are trimmed."""
    return _BREAK.sub(" ", text).strip()


# ★ THE ID GRAMMAR IS CONFIG'S: `anchors.idPrefix` and `anchors.minimumIdSegments` in the tree's own config.json --
# the keys the door reads -- composed here, never typed (round 13 had typed it three more times; the audit's F1-A9).
# A segment is `ID_SEGMENT` (✔MEASURED 2026-09-26 over both registries, 2,271 rows: upper case, digits and `_`, and
# lower case in two ids; no `.`); the prefix is the id's first segment, so a minimum of 3 is `<prefix>-A-B`.
ID_SEGMENT = r"[A-Za-z0-9_]+"
IdGrammar = collections.namedtuple("IdGrammar", "prefix min_segments")
_NOT_AFTER_ID_CHAR = r"(?<![A-Za-z0-9_-])"


def id_grammar(root):
    """-> IdGrammar(prefix, min_segments) from `root`'s config.json `anchors` section; REFUSED when either key is
    missing or malformed -- a grammar nobody declared is never a default."""
    section = _anchors_section(root)
    prefix, n = section.get("idPrefix"), section.get("minimumIdSegments")
    if not isinstance(prefix, str) or not re.fullmatch(r"[A-Za-z0-9]+", prefix):
        raise Refused("%s `anchors.idPrefix` is %r, not the letters and digits an id opens with"
                      % (CONFIG_REL, prefix))
    if isinstance(n, bool) or not isinstance(n, int) or n < 2:
        raise Refused("%s `anchors.minimumIdSegments` is %r, not a count of at least 2" % (CONFIG_REL, n))
    return IdGrammar(prefix, n)


def id_pattern(grammar):
    """-> the regular expression (source) of one WHOLE id: the prefix, then at least `min_segments - 1` segments."""
    return r"%s(?:-%s){%d,}" % (re.escape(grammar.prefix), ID_SEGMENT, grammar.min_segments - 1)


def cited_ids(text, grammar):
    """-> the set of ids `text` cites: each whole id-shaped token, except one followed by `-`, `*` or `{` -- a family
    or pattern mention (`<prefix>-HARNESS-*`), which names no row."""
    rx = re.compile(_NOT_AFTER_ID_CHAR + "(%s)(?![A-Za-z0-9_])" % id_pattern(grammar))
    return set(m.group(1) for m in rx.finditer(text) if text[m.end():m.end() + 1] not in ("-", "*", "{"))


def tree_dirs(root):
    """-> the top-level directory names of `root` (`.git` excepted): where a path a cell cites starts. A root that
    cannot be listed is REFUSED -- an empty answer would switch the path-cut refusal off unseen (the audit's F1-A8)."""
    try:
        return tuple(sorted(n for n in os.listdir(root)
                            if n != ".git" and os.path.isdir(os.path.join(root, n))))
    except OSError as exc:
        raise Refused("the top-level directories of %s cannot be listed (%s), so a path a cell cites cannot be "
                      "checked" % (root, exc))


# ★ WHAT THE DOOR WOULD STORE BROKEN, refused here because the door cannot see it (✔MEASURED, P68):
#   * an anchor id BROKEN by a line break or a joined-line space. After a hyphen (`<prefix>-AREA-` then the next
#     line), the door stores `<prefix>-AREA- TOPIC`, a FALSE id no grep for the real one returns: refused on the
#     STORED form, so the already-joined shape, an id holding `_` and a break right after the prefix are refused
#     too (the audit's F1-A2). INSIDE a segment the stored form cannot tell a break from prose (`<id> 2-TU` and
#     `<id> MF-4` are real, stored, one-line text), so that one is read on the raw text: an id-shaped token ENDING
#     a line whose next line opens with an upper-case hyphenated token;
#   * a PATH cut by a joined-line space -- a composer that joined wrapped lines with a space stored three paths
#     broken after a `/` (`tests/hir/ test_x.cpp`) in the archive (round 9, lane cs fold 2). A `<dir>/` followed,
#     AS STORED, by whitespace and a PATH-SHAPED token (one holding `_`, `.` or `-`) is refused; prose such as
#     "touches no src/hir/ or src/mir/" is not path-shaped and passes. The directories a path starts from are the
#     tree's own top level (`tree_dirs`), never a list typed here.
def cell_refusals(text, roots, grammar):
    """-> [why] for a prose cell the door would store BROKEN (see above); [] when it would store it whole."""
    out = []
    stored = door_form(text)
    prefix, seg = re.escape(grammar.prefix), ID_SEGMENT
    m = re.search(_NOT_AFTER_ID_CHAR + r"%s-(?:%s-)*\s+[A-Za-z0-9_]" % (prefix, seg), stored)
    if m:
        out.append("holds an anchor id broken after a hyphen (%r, as the door stores it): a line break becomes a "
                   "space, so the row would hold a FALSE id no grep for the real one returns -- keep every id "
                   "whole on one line" % m.group(0))
    m = re.search(_NOT_AFTER_ID_CHAR + r"%s-(?:%s-)*%s[ \t]*(?:\r\n|\r|\n)\s*[A-Z0-9_]+-[A-Za-z0-9_]"
                  % (prefix, seg, seg), text)
    if m:
        out.append("holds an anchor id wrapped inside a segment (%r): joined, it reads as a shorter FALSE id -- "
                   "keep every id whole on one line" % m.group(0))
    if roots:
        cut = re.compile(r"(?<![A-Za-z0-9_./-])(?:%s)/(?:[A-Za-z0-9_.-]+/)*[ \t]+[A-Za-z0-9]+[_.-][A-Za-z0-9_./-]*"
                         % "|".join(re.escape(r) for r in roots))
        m = cut.search(stored)
        if m:
            out.append("holds a path cut by a joined-line space (%r, as the door stores it): a line wrapped after "
                       "its `/` is joined with a space -- write the path whole" % m.group(0)[:80])
    return out


def door_write(root, anchor, fields, new, apply_it, exe=None, env=None):
    """ONE row write through the door -> (rc, output).

    `fields` holds only what is WRITTEN: `priority`, `status` (a word, `done`, or a stored
    cell), and the prose cells `trigger` / `closing` / `cross_refs` as TEXT. `new` picks
    `write-anchor` (the door requires a priority and a Trigger) or `set-anchor` (only the named
    fields change; every other cell keeps its bytes, and the door judges the row it would store,
    so a verdict split against a kept Trigger is refused there). Without `apply_it` the door is
    asked for a DRY RUN. This launcher's own refusals -- the controlled vocabulary, an update
    naming no field, a prose cell the door would store broken (`cell_refusals`, against `root`'s
    own id grammar and top-level directories, neither of which may be missing) -- come back as
    (DOOR_REFUSED, why) before the door is asked anything; the door's own code and words pass
    through otherwise.
    """
    fields = dict(fields)
    why = []
    try:
        if "status" in fields:
            fields["status"] = status_word(normalise_status(fields["status"]))
        if "priority" in fields:
            fields["priority"] = normalise_priority(fields["priority"])
    except Refused as exc:
        why.append(str(exc))
    if not new and not fields:
        why.append("nothing to write: an update names at least one field")
    try:
        grammar, roots = id_grammar(root), tree_dirs(root)
    except Refused as exc:
        why.append("the cells cannot be checked: %s" % exc)
    else:
        for name, _flag in DOOR_CELLS:
            if name in fields:
                why += ["the %s cell %s" % (name, w) for w in cell_refusals(str(fields[name]), roots, grammar)]
    if why:
        return DOOR_REFUSED, "\n".join(why)
    box = tempfile.mkdtemp(prefix="anchors-door-")
    try:
        argv = [exe or door_executable(env), DOOR_VERBS[bool(new)], anchor]
        for name, flag in DOOR_SCALARS:
            if name in fields:
                argv += [flag, fields[name]]
        for name, flag in DOOR_CELLS:
            if name in fields:
                path = os.path.join(box, "%s.cell" % name)
                with io.open(path, "w", encoding="utf-8", newline="") as fh:
                    fh.write(str(fields[name]))
                argv += [flag, path]
        argv += ["-C", root, "--no-prompt"]
        if not apply_it:
            argv.append("--anchor-dry-run")
        # ⓘ Without the caller's git selection, like every child this tree starts: the door
        # asks git which repository `root` is, and an exported GIT_DIR would answer for another.
        proc = _owning_tree().run_unsteered(argv, env=env, capture_output=True)
        return proc.returncode, (proc.stdout + proc.stderr).decode("utf-8", "replace")
    except Refused as exc:
        return DOOR_REFUSED, str(exc)
    except OSError as exc:
        return DOOR_REFUSED, "the door could not be started: %s" % exc
    finally:
        shutil.rmtree(box, ignore_errors=True)


# ─────────────────────────────────── verbs ────────────────────────────────────

def _bucket_flags(ap, required=False):
    g = ap.add_mutually_exclusive_group(required=required)
    for b in BUCKETS:
        g.add_argument("--%s" % b, dest="bucket", action="store_const", const=b,
                       help="the %s registry" % b)


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
                    help="live rows only (open, gated and disclosed)")
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
    """Red-on-disable for the reader, the vocabulary and the launcher -- and PARITY for the door.

    Every refusal the deleted writer made is driven through `door_write` into the REAL door, in a
    git fixture tree carrying THIS tree's own `anchors` configuration, and must still happen;
    the launcher's own refusals are driven the same way; and a cell the door was not asked to
    write must keep its bytes. The controls are here so a guard that refuses EVERYTHING cannot
    produce this same clean transcript. ⚠ A host without DssHarness FAILS the door arms: they
    measure the door, and a door nobody ran is not a door that refused.
    """
    import contextlib
    failed = [0]
    # ⚠⚠ THE FIXTURE IDS ARE ASSEMBLED FROM FRAGMENTS, NEVER WRITTEN WHOLE.
    # This directory is a citation root (`anchors.citationRoots` names `.harness-config`),
    # so a three-segment `D-*` written as ONE literal in this file is a CITATION that
    # `dssharness check-anchor-citations` must resolve to a registry row -- and these are
    # INPUT DATA to a parser test, not citations. ✔MEASURED: written whole they red the
    # entire tree. Make the fixture stop being anchor-shaped, never allowlist the name.
    _FX = "D-" + "FIXTURE-ANCHORS"
    A_ = _FX + "-ALPHA"
    B_ = _FX + "-BETA"
    N_ = _FX + "-NOSUCH"
    HG_ = _FX + "-GAMMA"
    ot = _owning_tree()

    def pin(ok, why, detail=""):
        # a detail is printed as text, the system temp directory masked: on Windows it lies under the user
        # profile, and a fixture path must not name the account in a run's log
        _tmp = tempfile.gettempdir()
        detail = (str(detail).replace(_tmp, "<temp>").replace(_tmp.replace("\\", "\\\\"), "<temp>")
                  if detail else "")
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
            O % "ALPHA", O % "BETA",
            "| `" + HG_ + "` | P3 | 🟠 OPEN | 🟠 **OPEN** | w | r |", "")
        doc(REL["done"], "# d", "", DONE_TABLE["production"], "", TABLE_HEADER,
            SEP_ROW_TEXT, C % "OLDP", "")

    def refuse(fn, *a, **k):
        try:
            fn(*a, **k)
            return None
        except Refused as exc:
            return str(exc)

    # ── (1)..(6) THE VOCABULARY IS THE DOOR'S, AND THE GATE READS IT ─────────────────
    pin(STATUS_WORDS == ("open", "gated", "closed", "disclosed")
        and normalise_status("done") == STATUS["closed"]
        and all(normalise_status(w) == STATUS[w] and normalise_status(STATUS[w]) == STATUS[w]
                and status_word(STATUS[w]) == w for w in STATUS_WORDS),
        "(1) the four words the door takes, each spelled as a word, `done`, or the stored cell")
    pin(bal.is_disclosed(STATUS["disclosed"]) and not bal.is_disclosed(STATUS["open"])
        and not bal.is_closed(STATUS["disclosed"])
        and bal.lead_verdict_word(STATUS["disclosed"]) == "OPEN",
        "(2) `disclosed` is what the GATE reads as disclosed, OPEN work whose verdict WORD is "
        "OPEN (ARM 7) -- asked through the gate, never compared with a re-typed glyph",
        "cell=%r word=%r" % (STATUS["disclosed"], bal.lead_verdict_word(STATUS["disclosed"])))
    _retired = refuse(normalise_status, RETIRED_DISCLOSED) or ""
    pin("RETIRED" in _retired and "disclosed" in _retired,
        "(3) the retired `🔵 🟠 OPEN (DISCLOSED)` spelling is REFUSED, naming the word to write",
        _retired[:90])
    pin("CONTROLLED VOCABULARY" in (refuse(normalise_status, "wibble") or ""),
        "(4) a status outside the vocabulary is REFUSED")
    pin("is not one of" in (refuse(normalise_priority, "P9") or "")
        and normalise_priority(" p2 ") == "P2",
        "(5) a priority outside P0..P5 is REFUSED; a lower-case band is read as the band")
    cfg_bad = None
    try:
        door_rel = door_config(ROOT)
    except (Refused, ot.Refusal) as exc:
        door_rel, cfg_bad = {}, str(exc)
    pin(door_rel == REL,
        "(6) the files the DOOR writes (config.json `anchors`) are the files this READER reads",
        "door=%r reader=%r %s" % (door_rel, REL, cfg_bad or ""))

    # ── (7)..(13) THE LINT, each arm with the control ───────────────────────────────
    with tempfile.TemporaryDirectory() as tmp:
        box(tmp)
        pin(lint(tmp) == [], "(7) CONTROL: a clean fixture lints clean, so every arm below is "
                             "not firing on everything")
        for label, rel, at, row, expect in (
                ("(8) a CLOSED row left in a working registry", REL["production"], 5,
                 C % "STUCK", "CLOSED row in a working registry"),
                ("(9) an OPEN row in the ARCHIVE -- the dangerous direction",
                 REL["done"], 7, O % "HIDDEN", "OPEN row in the archive"),
                ("(10) a Priority outside the band vocabulary", REL["production"], 5,
                 "| `" + _FX + "-BAND` | P9 | 🟠 OPEN | t | w | r |", "Priority"),
                ("(11) a Status outside the controlled vocabulary", REL["production"],
                 5, "| `" + _FX + "-VOCAB` | P2 | ORANGE | t | w | r |", "Status"),
                ("(12) the RETIRED disclosed cell", REL["production"], 5,
                 "| `" + _FX + "-OLDDISC` | P2 | " + RETIRED_DISCLOSED + " | t | w | r |", "Status"),
                ("(13) a Status column contradicting its own Trigger prose",
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
        pin(len({r.bucket for r in find(tmp, _FX + "-SPLIT")}) == 1 and find(tmp, N_) == [],
            "(13b) `find` answers one row for one id, and none for an id no registry holds")

    # ── (14)..(21) THE LAUNCHER'S OWN REFUSALS: nothing reaches the door ────────────
    # `exe` names a program that does not exist, so an arm that wrongly REACHES the door
    # fails with an OSError refusal instead of passing -- and (21) is the control that
    # proves the same launcher does reach a door when nothing is wrong. The launcher reads the
    # id grammar from the root it launches in, so every arm launches in a box holding THIS
    # tree's `anchors` section and a `src/` and a `tests/` directory (the roots a path a cell
    # cites starts from, `tree_dirs`).
    nowhere = os.path.join(tempfile.gettempdir(), "anchors-no-such-door-%d" % os.getpid())
    this_anchors = ot.load_jsonc(os.path.join(ROOT, *CONFIG_REL.split("/"))).get("anchors")
    launch_box = tempfile.mkdtemp(prefix="anchors-launch-")

    def config_box(path, section):
        os.makedirs(os.path.join(path, ".harness-config"), exist_ok=True)
        with io.open(os.path.join(path, *CONFIG_REL.split("/")), "w", encoding="utf-8") as fh:
            json.dump({"anchors": section}, fh)
    config_box(launch_box, this_anchors)
    for _d in ("src", "tests"):
        os.makedirs(os.path.join(launch_box, _d))

    def launch(fields, new=True, root=None):
        return door_write(root or launch_box, A_, fields, new, False, exe=nowhere)

    def says(result, *needles):
        rc, out = result
        return rc == DOOR_REFUSED and all(n in out for n in needles)
    # (14) The verdict rule this launcher enforced until DssHarness 0.5.9 is the DOOR's now, and it
    # rides on ONE config key: without `anchors.triggerCarriesVerdict: true` the door reads a row's
    # verdict from its Status alone and writes a Status/Trigger split silently (the P64 rows
    # check-anchor-balance ARM 6 fails). Arms (15)..(18) were the launcher's own report-#7
    # refusals (a verdict split either way, a split against a KEPT Trigger, in-line whitespace, a
    # leading BOM); 0.5.9 refuses the split and the BOM with exit 10, judges the row it would
    # store on set-anchor, and keeps in-line whitespace as written (✔MEASURED 2026-09-24 on
    # 0.5.9, write-anchor and set-anchor, on throwaway repositories).
    _anchors_cfg = _owning_tree().load_jsonc(
        os.path.join(ROOT, *CONFIG_REL.split("/"))).get("anchors", {})
    pin(_anchors_cfg.get("triggerCarriesVerdict") is True,
        "(14) THIS tree's config sets `anchors.triggerCarriesVerdict: true` -- the door's verdict "
        "check that replaced this launcher's (arms 15..18 retired with it)",
        "got=%r" % _anchors_cfg.get("triggerCarriesVerdict"))
    pin(says(launch({"priority": "P1", "status": RETIRED_DISCLOSED, "trigger": "t"}), "RETIRED")
        and says(launch({"priority": "P9", "trigger": "t"}), "is not one of"),
        "(19) the vocabulary refuses before the door: the retired disclosed cell, a band P9")
    pin(says(launch({}, new=False), "nothing to write"),
        "(20) an update that names no field is REFUSED rather than sent as a no-op")
    _ok = launch({"priority": "P1", "status": "gated",
                  "trigger": "🟠 **OPEN -- TRIGGER-GATED** first line\n  second line",
                  "closing": "c", "cross_refs": "r"})
    pin(_ok[0] == DOOR_REFUSED and "could not be started" in _ok[1]
        and "verdict" not in _ok[1] and "whitespace" not in _ok[1],
        "(21) CONTROL: GATED beside OPEN prose, and a cell whose only change is its LINES "
        "joined, pass every launcher check and go to the door (here a door that does not "
        "exist, so its absence is the only answer)", _ok[1][:120])
    _none = refuse(door_executable, {"PATH": tempfile.gettempdir(), "HOME": nowhere,
                                     "USERPROFILE": nowhere})
    pin(_none is not None and "dotnet tool install --global DssHarness" in _none,
        "(21b) a host WITHOUT the tool is a refusal that says how to install it -- never a "
        "fallback writer", (_none or "")[:120])
    # (21c)..(21h) the content the door would store BROKEN, refused by the launcher AS THE DOOR WILL STORE IT
    # (`door_form`) -- every shape a lane's cell file really holds, the line break included, never only the
    # already-joined one (P68 round 13's audit, F1-A1/A2: the first arms synthesized only the joined shape, and the
    # refusal they pinned could not see a line break). `pre` is the id prefix alone: this file is a citation root,
    # so no whole three-segment id is ever written as one literal here.
    pre = "D" + "-"
    try:
        _shapes = (("\\n", "FIXTURE-\nANCHORS-GAMMA"), ("\\n + indent", "FIXTURE-\n    ANCHORS-GAMMA"),
                   ("\\r\\n", "FIXTURE-\r\nANCHORS-GAMMA"), ("already joined", "FIXTURE- ANCHORS-GAMMA"),
                   ("an id holding _", "FIX_TURE-\nANCHORS-GAMMA"), ("right after the prefix", "\nFIXTURE-ANCHORS"))
        _passed = [label for label, tail in _shapes
                   if not says(launch({"priority": "P1", "trigger": "🟠 **OPEN** waits on " + pre + tail}),
                               "the trigger cell", "broken after a hyphen")]
        pin(not _passed, "(21c) an anchor id BROKEN after a hyphen is refused in every shape a cell file holds -- a "
                         "line break, one with the next line indented, CRLF, the already-joined space, an id "
                         "holding `_`, a break right after the prefix", "passed: %s" % _passed)
        pin(says(launch({"priority": "P1", "trigger": "🟠 **OPEN** waits on " + pre + "FIXTURE-ANCH\nORS-GAMMA"}),
                 "wrapped inside a segment"),
            "(21c2) an anchor id wrapped INSIDE a segment is refused -- joined, it reads as a shorter false id")
        for label, cut in (("(21d) the already-joined shape", "see tests/hir/ test_brace_element.cpp"),
                           ("(21d2) a line break after the `/`", "see tests/hir/\ntest_brace_element.cpp"),
                           ("(21d3) a line break and an indent after the `/`",
                            "see tests/hir/\n    test_brace_element.cpp"),
                           ("(21d4) a CRLF after the `/`", "see tests/hir/\r\ntest_brace_element.cpp")):
            _r = launch({"priority": "P1", "trigger": "🟠 **OPEN** t", "closing": cut})
            pin(says(_r, "the closing cell", "joined-line space"),
                "%s: a path cut there is refused, naming the cell -- the door would store it cut" % label,
                _r[1][:160])
        _prose = launch({"priority": "P1", "trigger": "🟠 **OPEN** touches no src/hir/ or src/mir/",
                         "cross_refs": "tests/hir/test_x.cpp\nand src/\nmore, " + pre + "FIXTURE-*",
                         "closing": "blocked by " + pre + "FIXTURE-ANCHORS-GAMMA\nand more; " + pre
                                    + "FIXTURE-ANCHORS-GAMMA 2-TU probe"})
        pin(_prose[0] == DOOR_REFUSED and "could not be started" in _prose[1],
            "(21e) CONTROL: prose naming directories, whole paths across a line break, a family mention, an id "
            "ending a line before lower-case prose, and an id followed on ONE line by an upper-case hyphenated "
            "token (a stored shape, ✔MEASURED) all reach the door (here one that does not exist, so its absence is "
            "the only answer)", _prose[1][:200])
        _unlisted = refuse(tree_dirs, os.path.join(launch_box, "no-such-root"))
        pin(_unlisted is not None and "cannot be listed" in _unlisted,
            "(21f) a root whose directories cannot be listed is a REFUSAL -- never an empty answer that switches the "
            "path-cut refusal off unseen", _unlisted)
        q_box = tempfile.mkdtemp(dir=launch_box)
        config_box(q_box, dict(this_anchors, idPrefix="Q"))
        _gq = refuse(id_grammar, q_box) or id_grammar(q_box)
        _q_ok = isinstance(_gq, IdGrammar) and _gq.prefix == "Q"
        pin(_q_ok and bool(cell_refusals("waits on Q-FIXTURE-\nANCHORS-GAMMA", (), _gq))
            and not cell_refusals("waits on " + pre + "FIXTURE-\nANCHORS-GAMMA", (), _gq)
            and cited_ids("Q-FIXTURE-ALPHA, and " + pre + "FIXTURE-ALPHA", _gq) == {"Q-FIXTURE-ALPHA"}
            and re.fullmatch(id_pattern(_gq), "Q-A-B") and not re.fullmatch(id_pattern(_gq), "Q-A"),
            "(21g) the id grammar is config.json's (`anchors.idPrefix` / `minimumIdSegments`): a tree declaring the "
            "prefix Q breaks, cites and counts segments by Q, and the D of this tree is prose there", _gq)
        bad_box = tempfile.mkdtemp(dir=launch_box)
        config_box(bad_box, dict((k, v) for k, v in this_anchors.items() if k != "idPrefix"))
        _nogrammar = launch({"priority": "P1", "trigger": "🟠 **OPEN** t"}, root=bad_box)
        _noconfig = launch({"priority": "P1", "trigger": "🟠 **OPEN** t"}, root=tempfile.mkdtemp(dir=launch_box))
        pin(says(_nogrammar, "cannot be checked", "idPrefix") and says(_noconfig, "cannot be checked"),
            "(21h) a root declaring no id grammar, or no config at all, is refused before the door -- a grammar "
            "nobody declared is never a default", (_nogrammar[1][:120], _noconfig[1][:120]))
        cit = cited_ids("see [[" + pre + "FIXTURE-ALPHA]], `" + pre + "FIXTURE-BETA`; " + pre + "FIXTURE-GAMMA.1, "
                        "the " + pre + "FIXTURE-ROWS-* family, " + pre + "FIXTURE-ROWS-{A,B} and " + pre + "TWO",
                        IdGrammar("D", 3))
        pin(cit == {pre + "FIXTURE-ALPHA", pre + "FIXTURE-BETA", pre + "FIXTURE-GAMMA"},
            "(21i) cited_ids reads a wiki-link, a backticked id and an id before a dot as citations, and a family "
            "mention (`-*`, `-{..}`) and a two-segment token as none", sorted(cit))
    finally:
        ot.remove_tree(launch_box)

    # ── (22)..(36) PARITY: every refusal the deleted writer made, in the REAL door ───
    fx_root = tempfile.mkdtemp(prefix="anchors-door-fixture-")
    try:
        door_cfg = ot.load_jsonc(os.path.join(ROOT, *CONFIG_REL.split("/"))).get("anchors")

        def door_tree():
            """A fresh git repository holding THIS tree's own `anchors` config and the fixture
            registries -- the door refuses a directory that is no repository."""
            path = tempfile.mkdtemp(dir=fx_root)
            box(path)
            os.makedirs(os.path.join(path, ".harness-config"))
            with io.open(os.path.join(path, *CONFIG_REL.split("/")), "w", encoding="utf-8") as fh:
                json.dump({"anchors": door_cfg}, fh)
            ot.run_git(["init", "-q", path], capture_output=True, check=True)
            return path

        def text(path, rel):
            return io.open(os.path.join(path, rel), encoding="utf-8", newline="").read()

        def door(path, anchor, fields, new=False, apply_it=True):
            return door_write(path, anchor, fields, new, apply_it)

        def inject(path, rel, row):
            """`row` as RAW TEXT, first in the table -- inside it, where a reader and the door
            both look (a line after the table's blank line is outside every table)."""
            body = text(path, rel)
            if body.count(SEP_ROW_TEXT + "\n") != 1:
                raise AssertionError("the fixture table is not where inject expects it")
            io.open(os.path.join(path, rel), "w", encoding="utf-8", newline="").write(
                body.replace(SEP_ROW_TEXT + "\n", SEP_ROW_TEXT + "\n" + row + "\n", 1))

        t = door_tree()
        rc, out = door(t, _FX + "-PIPES", {"priority": "P1", "trigger": "🟠 **OPEN** a \\| b"},
                       new=True)
        rc2, out2 = door(t, _FX + "-PIPES", {"priority": "P1", "trigger": "🟠 **OPEN** t",
                                              "closing": "c \\| d"}, new=True)
        rc3, out3 = door(t, _FX + "-PIPES", {"priority": "P1", "trigger": "🟠 **OPEN** t",
                                              "cross_refs": "x \\| y"}, new=True)
        pin(all(r != 0 and "backslash immediately before a pipe" in o
                for r, o in ((rc, out), (rc2, out2), (rc3, out3))) and not find(t, _FX + "-PIPES"),
            "(22) a PRE-ESCAPED pipe is REFUSED by the door in EVERY prose cell -- the one place "
            "that can still tell it from a deliberate pipe", out[:120])
        rc, out = door(t, _FX + "-EMPTY", {"priority": "P1", "trigger": " "}, new=True)
        pin(rc != 0 and "empty" in out.lower() and not find(t, _FX + "-EMPTY"),
            "(23) an empty Trigger is REFUSED", out[:120])
        rc, out = door(t, "D-" + "TWO", {"priority": "P1", "trigger": "t"}, new=True)
        rc2, out2 = door(t, _FX + "-MINTABLE", {"priority": "P1", "trigger": "t"}, new=True)
        pin(rc != 0 and rc2 == 0 and len(find(t, _FX + "-MINTABLE")) == 1,
            "(24) a NEW two-segment id is REFUSED -- no guard would resolve it -- while a "
            "guard-resolvable id mints (the control)", "%s / %s" % (out[:80], out2[:80]))
        two = "D-" + "TWO"
        inject(t, REL["production"], "| `%s` | P2 | 🟠 OPEN | 🟠 **OPEN** t | w | r |" % two)
        rc, out = door(t, two, {"closing": "maintained"})
        pin(rc == 0 and find(t, two)[0].cell(C_CLOSING).strip() == "maintained",
            "(25) an EXISTING two-segment row UPDATES -- its identity came from the registry",
            out[:120])
        rc, out = door(t, _FX + "-VOCAB", {"priority": "P1", "status": "wibble",
                                            "trigger": "t"}, new=True)
        pin(rc == DOOR_REFUSED and "CONTROLLED VOCABULARY" in out,
            "(26) a status outside the vocabulary is REFUSED before the door")
        t = door_tree()
        big = "🟠 **OPEN** a cell | with a pipe, " + "x" * 40000 + "\nand a second line\n"
        rc, out = door(t, _FX + "-BIG", {"priority": "P2", "status": "open", "trigger": big,
                                          "closing": "a\nb"}, new=True)
        got = find(t, _FX + "-BIG")
        raw = [ln for ln in text(t, REL["production"]).split("\n") if (_FX + "-BIG`") in ln]
        pin(rc == 0 and len(got) == 1 and len(raw) == 1 and len(bal.split_row(raw[0])) == 8
            and got[0].cell(C_TRIGGER).strip() == " ".join(big.split())
            and got[0].cell(C_CLOSING).strip() == "a b",
            "(27) a raw pipe is ESCAPED and a line break JOINED -- the row stays ONE line of six "
            "cells, and a cell longer than the whole Windows command line lands intact",
            "rc=%d %s" % (rc, out[:120]))
        rc, out = door(t, A_, {"status": "closed", "trigger": "✅ **CLOSED** shipped"})
        prod, done = text(t, REL["production"]), text(t, REL["done"])
        pin(rc == 0 and ("`%s`" % A_) not in prod and ("`%s`" % A_) in done
            and done.index(DONE_TABLE["production"]) < done.index("`%s`" % A_)
            and ("`%s`" % B_) in prod and ("`%s`" % HG_) in prod,
            "(28) CLOSING moves the row out of the working registry into the archive, under its "
            "heading, and the sibling rows are untouched", out[:120])
        rc, out = door(t, A_, {"status": "open", "trigger": "🟠 **OPEN -- regressed**"})
        rc2, out2 = door(t, A_, {"status": "gated"})
        got = find(t, A_)
        pin(rc == 0 and rc2 == 0 and len(got) == 1 and got[0].bucket == "production"
            and got[0].status == STATUS["gated"],
            "(29) REOPENING moves it back, and GATED is live -- it stays where a queue can see it",
            "%s / %s" % (out[:60], out2[:60]))
        rc, out = door(t, N_, {"closing": "c"})
        pin(rc != 0 and not find(t, N_),
            "(30) an update of a row that does not exist is REFUSED -- a mistyped id never "
            "mints a second row", out[:120])
        rc, out = door(t, B_, {"priority": "P1", "trigger": "🟠 **OPEN** again"}, new=True)
        pin(rc != 0 and len(find(t, B_)) == 1,
            "(31) a NEW row over an EXISTING one is REFUSED -- each verb refuses the other's world",
            out[:120])
        lines = text(t, REL["done"]).split("\n")
        lines.insert(6, O % "BETA")
        io.open(os.path.join(t, REL["done"]), "w", encoding="utf-8", newline="").write(
            "\n".join(lines))
        rc, out = door_write(t, B_, {"priority": "P1"}, False, True)
        pin(len({r.bucket for r in find(t, B_)}) == 2 and rc != 0,
            "(32) one id in BOTH registries is FOUND in both and every write to it is REFUSED -- "
            "never settled by picking a file", out[:120])
        t = door_tree()
        before = (text(t, REL["production"]), text(t, REL["done"]))
        rc, out = door(t, B_, {"status": "closed", "trigger": "✅ **CLOSED** t"}, apply_it=False)
        pin(rc == 0 and (text(t, REL["production"]), text(t, REL["done"])) == before,
            "(33) a DRY RUN writes nothing at all", out[:120])
        rc, out = door(t, B_, {"status": "done", "trigger": "✅ **CLOSED** t"})
        pin(rc == 0 and find(t, B_)[0].bucket == "done",
            "(34) `done` is accepted as a spelling of `closed` -- the operator's own word")
        rc, out = door(t, _FX + "-DISC", {"priority": "P1", "status": "disclosed",
                                           "trigger": "🟠 **OPEN** the debt pre-dates this cycle"},
                       new=True)
        got = find(t, _FX + "-DISC")
        pin(rc == 0 and len(got) == 1 and got[0].bucket == "production"
            and got[0].status == STATUS["disclosed"] and not lint(t),
            "(35) a DISCLOSED row files in the WORKING registry with the door's cell, and lints "
            "clean -- the exemption touches the FAILURE only", out[:120])
        # (36) AN UNTOUCHED CELL KEEPS ITS BYTES. The row is injected as RAW TEXT, the way the
        # stored rows holding a run were written, so the door is measured on a row it did not
        # make; only the PRIORITY is named, and every other cell must come back byte-identical.
        run_ = _FX + "-SPACERUNS"
        raw_row = ("| `" + run_ + "` | P2 | 🟠 OPEN | 🟠 **OPEN** so `4  +  38` IS the list, a\ttab"
                   " | see  for details \\| piped | `DCO  fail` |")
        inject(t, REL["production"], raw_row)
        rc, out = door(t, run_, {"priority": "P3"})
        after = [ln for ln in text(t, REL["production"]).split("\n") if (run_ + "`") in ln]
        pin(rc == 0 and len(after) == 1
            and after[0] == raw_row.replace("| P2 |", "| P3 |", 1),
            "(36) a cell the door was not asked to write keeps its BYTES -- a run of spaces, a "
            "tab and an escaped pipe included -- so an update names only what changes",
            "rc=%d %r" % (rc, after[0][:140] if after else out[:140]))
    finally:
        ot.remove_tree(fx_root)

    # ── (37)..(39) THE ROOT IS THE TREE THIS FILE LIVES IN, FROM ANY WORKING DIRECTORY ──
    # The arms `.harness-config/runner/actions/owning-tree/owning-tree.py` hands every consumer, run against THIS
    # file's own `repo_root`: cwd = its own tree (the CONTROL), cwd inside another git
    # repository, cwd inside no repository -- each foreign condition proven real first.
    # ⚠ They cannot tell the old local walk from the owner (both are file-keyed); they redden
    # on a root keyed on the CALLER, which is the class they exist to keep out.
    with contextlib.redirect_stderr(io.StringIO()):
        for _n, (_ok, _label, _detail) in enumerate(
                ot.root_arms(repo_root, (SystemExit,), False, __file__), start=37):
            pin(_ok, "(%d) %s" % (_n, _label), _detail)

    print("anchors self-test: %d failed" % failed[0])
    return 1 if failed[0] else 0


VERBS = {"read": cmd_read, "list": cmd_list}


def main(argv):
    if "--self-test" in argv or "--selftest" in argv:
        return self_test()
    if argv[:1] in (["write"], ["set"]):
        print("anchors: `%s` is gone -- a row is written by the door, `dssharness %s-anchor`, "
              "and by nothing else." % (argv[0], argv[0]))
        return 3
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
