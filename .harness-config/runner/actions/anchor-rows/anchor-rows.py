#!/usr/bin/env python3
# PURPOSE: stage, check and apply one fold's deferred-anchor rows as a batch through the door, rehearsed in a throwaway repository first, all or nothing, every row read back.
"""anchor-rows.py -- ONE FOLD'S REGISTRY ROWS, AS A BATCH, THROUGH THE DOOR.

★★★ WHY THIS EXISTS. A lane files the rows its work opened or closed as cell files, and the orchestrator
applies them when it folds the lane. Until 2026-09-25 that was five scripts in a session toolkit (copy the
rows turning pre-escaped pipes back, prune them to the fold's ids, precheck each cell against the stored
row, diff the cells that lose text, pre-flight and apply through the door), and every defect below was
paid for once in them:
  * a loop that wrote as it went stopped with SOME rows applied when the door refused a later one;
  * a `--dry-run` that was not an option was silently ignored, and the "dry run" APPLIED;
  * a row's status printed through a cp1252 console killed the loop after its first write;
  * a composer that joined wrapped lines with a space stored `tests/hir/ test_x.cpp` in the archive.
This is their one successor: a CALLER of the door in the anchors family, beside `lane-fold` and
`apply-registry-row`, reaching it only through `anchors.door_write`, the one launcher the three share.

THE ROW DIRECTORY, the one format a lane writes and this reads:

    <rows dir>/<ANCHOR ID>/{priority,status,trigger,closing,crossrefs}.txt

UTF-8 (a byte-order mark is refused: the door refuses it too). `cross-refs.txt` is read as crossrefs, and a
row holding BOTH spellings is refused rather than settled by preference. Any other file is refused -- a
draft beside real cells is how a row goes wrong unseen -- except that `stage --live-only` skips the
`*.applied` files a lane renamed after an earlier application. A directory name is an anchor id under the
grammar the tree's config.json declares (`anchors.idPrefix`, `minimumIdSegments`: `anchors.id_grammar`, never
a pattern typed here). A NEW row needs `priority`, `status` and `trigger`: the door requires the first and the
third, and a born-closed row whose `status.txt` is missing would land OPEN. A STAGED row may also hold
`declared-new`, the marker `stage` writes for an id its `new` input names; in a lane's rows it is refused -- a
row is declared new by the fold, at stage, never by a file a lane left.

THE THREE VERBS -- each writes nothing it was not asked to, and each refuses before it writes:

  stage <lane rows> <staged dir> --only <ID>... [--new <ID>...] [--live-only]
      Copies EXACTLY the named rows into a NEW directory, each pre-escaped pipe `\\|` turned back into
      `|` and reported per cell (the door escapes every pipe itself, and refuses a pipe already escaped
      because it cannot tell it from a deliberate backslash). A named id the lane never wrote is refused
      -- a typo would silently drop a row -- and every `--only` given counts: a second one ADDS its ids
      (a repeated option that kept only its last value would silently drop the first one's rows). The
      ids `--new` names are the rows the batch may CREATE: each is marked in the staged record, and one
      `--only` does not carry is refused. The lane's rows it did NOT carry are named. The lane's own files
      are never edited, and nothing is pruned in place: the staged directory is the fold's record of what
      it carried, written as `<staged>.partial`, every file read back, then renamed -- a stage that stops
      part-way leaves no staged directory for `check` or `apply` to take as the whole batch.
  check <staged dir>
      READ-ONLY. A staged directory holding no row is refused (a vacuous batch is not a clean one). Every
      row is validated (the vocabulary through `anchors.normalise_status` / `normalise_priority`, the
      content refusals through `anchors.cell_refusals`, judged as the door will store the text), found in
      the registries -- EXISTING, or NEW only when the batch DECLARED it new (an id no registry holds and
      nobody declared is a typo that would mint a second row: refused, `apply-registry-row`'s rule; a row
      declared new that a registry holds other than as declared is refused too -- one standing exactly as
      declared is the batch's own, landed; an id with two rows is refused: a human settles a duplicate) --
      every id a row NEWLY cites must resolve, and each supplied cell of an existing row is
      judged against the stored one:
          SAME      the door would store exactly what is stored
          RESPACED  the words agree, the spacing does not (the stored bytes change, the words do not)
          KEPT      the stored text survives, verbatim and word for word, inside the new -- an addendum
          FILLED    the stored cell was empty
          LOST      the stored text does NOT survive: history would be rewritten
          CHANGED   (status, priority) a different value
      Every LOST cell is printed as a WORD DIFF (what it removes, and what replaces it), so the reviewer
      reads the loss itself rather than two blobs. Then the batch is REHEARSED (below). The apply step it
      will need, with its `acceptLost` input, is printed last.
  apply <staged dir> [--accept-lost <ID>:<cell>]...
      Everything `check` does -- the rehearsal must pass -- and then: every LOST cell must be named (the
      step's `acceptLost` input), and one naming a cell that is not LOST is refused (stale, or a typo);
      the registries must still hold the bytes the plan was made from (another writer since then:
      refused); both registries are snapshotted; the rows are written in ORDER, each read back; and a
      failure -- a door refusal, a row that does not read back, registries that do not lint afterwards, or
      an exception between two writes -- restores both registries byte for byte and says what had been
      written. The restore is refused, and nothing written, when the registries then hold a change that is
      not one of this batch's rows: another writer's work would be discarded, so a human reads them first.

THE REHEARSAL. A per-row `--anchor-dry-run` cannot see the batch as a whole and cannot be read back, so the
batch is written for real first into a THROWAWAY git repository holding the tree's two registries (their
current bytes, committed) and config.json's `anchors` section: every row through `anchors.door_write`, in
apply order, each read back with `dssharness read-anchor --json`; then `read-anchors --lint` and
`check-anchor-balance` there, whose base is the registries as they stand, so its receipt is THIS batch's
delta (opened, closed), printed with the gate's own verdict. A net increase is the ROUND gate's to judge (a
batch may open disclosed or gated work), so it is said and does not fail the rehearsal; an unreadable receipt,
a misfiled or malformed registry, or a failure this program cannot name does. The real registries are not
touched by `check` at all.

THE ORDER, AND WHAT RESOLVES A CITATION. NEW rows are written before the updates, each group by id: a batch
interrupted part-way (a killed process, a lost machine) then never leaves a stored row citing a row that is
not there yet -- CRASH CONSISTENCY, not enforcement. ✔MEASURED 2026-09-25 on DssHarness 0.5.12, in a
throwaway repository: `write-anchor` and `set-anchor` do not check the ids a cell cites -- a cell naming a row
no registry holds is written, dry run and real alike. And NO GUARD READS THE REGISTRIES' OWN CITATIONS:
`dssharness check-anchor-citations` resolves ids cited under `anchors.citationRoots`, which holds no `.plans`,
and `check-anchor-registry` scans `src/`, `examples/` and `docs/` (✔READ 2026-09-26, both tools' own words). So
this program resolves them itself: every id a row of the batch NEWLY cites -- in a new row, every id it cites;
in an existing row, every id its new text cites that the stored cell did not -- must be a row of either
registry or of the batch (`anchors.cited_ids`; a family mention such as `<prefix>-AREA-*` cites nothing), and
`check` and `apply` refuse one that is not. A citation already stored is history and is not re-judged:
✔MEASURED 2026-09-26, 616 stored citations resolve to no row, most of them to the retired harness registry.

READ-BACK. What the door stores is compared with what was declared, as the door stores it: a line break
collapses, with the whitespace either side of it, into one space; every other run of spaces or tabs is kept;
the value's ends are trimmed (`anchors.door_form`, ✔MEASURED on 0.5.12 and pinned against the real door by a
parity arm, so a door that changes its rule reds this self-test rather than a fold). `read-anchor --json`
returns cells UNESCAPED, so no pipe rule enters the comparison. An update names only the cells whose stored
form would CHANGE, so a cell the lane did not change keeps its stored bytes.

THE HARNESS STEPS. The three verbs are MANUAL steps of anchor-rows.yml, run through DssHarness on the `rows`
runner -- ONE leg, this machine's own tree, run in place: the registries a fold writes are that tree's, and a
host's synced copy holds no lane's rows directory (`.worktrees` and `.temp` are ignored, and only what git does not
ignore travels). A step's run line hands every input over as `<input>=<value>`, defaults included, and `--step`
turns them into the verb's own arguments; STEP_INPUTS is the one statement of each step's inputs, and the self-test
holds anchor-rows.yml to it. A list input is comma-separated, or `@<file>` naming a list file, one item a line
(blank lines and `#` comments skipped); a path is relative to the tree unless absolute. Every remedy this program
prints names the step, never the program.

Exit codes: 0 OK (for `check`, a batch that rehearses clean) · 2 refused, nothing written (or, for
`apply`, everything written restored) · 3 usage error.

Usage -- through the harness (`-C <tree>` names the tree):
    dssharness run rows --manual-step stage --input rows=<lane rows> --input staged=<staged> --input only=<ID>,...
                        [--input new=<ID>,...]
    dssharness run rows --manual-step check --input staged=<staged>
    dssharness run rows --manual-step apply --input staged=<staged> [--input acceptLost=<ID>:<cell>,...]
    dssharness run anchor-rows                     (the self-test, on both local legs; ctest runs it too)
What each step runs -- the program's own verbs:
    anchor-rows.py stage <lane rows> <staged> --only ID [ID ...] [--new ID [ID ...]] [--live-only]
    anchor-rows.py check <staged>
    anchor-rows.py apply <staged> [--accept-lost ID:cell ...]
    anchor-rows.py --step <stage|check|apply> <input>=<value>...
    anchor-rows.py --self-test
`--only` and `--new` may each be given more than once; every id they name counts. `--repo <path>` names another tree
deliberately, on EITHER side of the verb (the rule lane-fold and lane-worktree follow: a flag one program of the
family takes after its verb, the next must not refuse there); without it the tree acted on is the one this file
lives in. A step takes no `--repo`: it acts on the tree the harness runs it in.
"""
from __future__ import annotations

import collections
import difflib
import hashlib
import importlib.util
import io
import json
import os
import re
import sys
import tempfile
sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

HERE = os.path.dirname(os.path.realpath(__file__))
ACTIONS = os.path.dirname(HERE)

REFUSED, USAGE = 2, 3


class Refused(Exception):
    """A refusal of this program's own: printed, exit 2."""


# ──────────────────────────────── the two owners it loads ────────────────────────────────

def _load(path, name, why):
    """A sibling program, loaded BY PATH (a hyphen is not a module name), once. Missing, it is a
    refusal naming what lives there -- never a local respelling of it."""
    if name in sys.modules:
        return sys.modules[name]
    if not os.path.isfile(path):
        raise SystemExit("anchor-rows: cannot find %s -- %s" % (path, why))
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    held, sys.argv = sys.argv, [path]
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.argv = held
    sys.modules[name] = mod
    return mod


def ot():
    """`owning-tree`: which tree a file lives in, and git asked without the caller's git environment."""
    return _load(os.path.join(ACTIONS, "owning-tree", "owning-tree.py"), "anchor_rows_owning_tree",
                 "which tree this file lives in, and every git question it asks, are answered there")


def anchors():
    """`anchors`: the registry READER, the status and priority VOCABULARY, the content refusals, and
    `door_write`, the ONE launcher of the door. Used, never re-typed here."""
    return _load(os.path.join(ACTIONS, "anchors", "anchors.py"), "anchor_rows_anchors",
                 "the registry reader, the vocabulary and the door's launcher live there")


def repo_root(override=None):
    """The tree acted on: the one THIS FILE lives in (never the caller's working directory), or
    `override`'s git top level, named deliberately with `--repo`."""
    if override is None:
        try:
            return os.path.realpath(ot().resolve(__file__, reads_git=True))
        except ot().Refusal as exc:
            raise Refused("%s\n  pass --repo <path> to name a tree deliberately." % exc)
    top, why = ot().git_top_level(override)
    if top is None:
        raise Refused("no git working tree contains %s (%s)" % (override, why))
    return os.path.realpath(top)


# ──────────────────────────────── the row directory ────────────────────────────────

CELLS = ("priority", "status", "trigger", "closing", "crossrefs")
PROSE = ("trigger", "closing", "crossrefs")
ALIAS = {"cross-refs": "crossrefs"}
# a cell's name in `door_write`'s fields and in `read-anchor --json`
FIELD = {"priority": "priority", "status": "status", "trigger": "trigger", "closing": "closing",
         "crossrefs": "cross_refs"}
NEW_NEEDS = ("priority", "status", "trigger")
APPLIED = ".applied"
BOM = b"\xef\xbb\xbf"
ESCAPED_PIPE = "\\" + "|"
# The marker `stage` writes into a STAGED row its `new` input names: the batch may CREATE that row. Refused in a
# lane's rows -- the fold declares what is new, at stage, never a file a lane left (the audit's F1-A4).
NEW_MARKER = "declared-new"
NEW_MARKER_TEXT = b"declared NEW by the stage step's `new` input\n"

Row = collections.namedtuple("Row", "anchor cells new")    # cells: {cell: text}; new: declared NEW at stage


def read_row(path, live_only=False, staged=False):
    """-> ({cell: text}, [problem], [skipped name], declared new) for ONE row directory. Reads, never writes."""
    anchor = os.path.basename(path)
    cells, problems, skipped, spelled, new = {}, [], [], {}, False
    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        if os.path.isdir(full):
            problems.append("%s/%s is a directory -- a row directory holds cell files only" % (anchor, name))
            continue
        if name == NEW_MARKER:
            if staged:
                new = True
            else:
                problems.append("%s/%s is the staged NEW marker -- in a lane's rows it is refused: the fold declares a "
                                "row new with the stage step's `new` input" % (anchor, name))
            continue
        if live_only and name.endswith(APPLIED):
            skipped.append(name)
            continue
        stem, ext = os.path.splitext(name)
        cell = ALIAS.get(stem, stem)
        if ext != ".txt" or cell not in CELLS:
            problems.append("%s/%s is not a cell file (%s.txt, or cross-refs.txt)%s"
                            % (anchor, name, ".txt, ".join(CELLS),
                               " -- an *.applied file needs --live-only" if name.endswith(APPLIED) else ""))
            continue
        if cell in spelled:
            problems.append("%s holds BOTH %s and %s -- two crossrefs cells are a contradiction, refused "
                            "rather than settled by preference" % (anchor, spelled[cell], name))
            continue
        spelled[cell] = name
        with io.open(full, "rb") as fh:
            raw = fh.read()
        if raw.startswith(BOM):
            problems.append("%s/%s opens with a byte-order mark -- the door refuses one" % (anchor, name))
            continue
        try:
            cells[cell] = raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            problems.append("%s/%s is not UTF-8 (%s)" % (anchor, name, exc))
    if not cells and not problems:
        problems.append("%s holds no cell file%s" % (anchor, " (only *.applied ones)" if skipped else ""))
    return cells, problems, skipped, new


def load_rows(rows_dir, grammar, only=None, live_only=False, staged=False, show=str):
    """-> ([Row] in id order, [problem], {anchor: [skipped]}). `grammar` is the tree's id grammar
    (`anchors.id_grammar`): a directory whose name is not one whole id under it is refused. `only`: the ids to take
    (all when None); an id `only` names that the directory lacks is a problem, every one named. A directory holding
    no row is a problem too: a vacuous batch must not read as a clean one (the audit's F1-A6). `staged` reads the
    NEW marker `stage` writes; `show` spells a path for a message (tree-relative, from the verbs)."""
    if not os.path.isdir(rows_dir):
        return [], ["no rows directory at %s" % show(rows_dir)], {}
    row_dir = re.compile(r"^%s$" % anchors().id_pattern(grammar))
    problems, rows, skipped = [], [], {}
    names = []
    for name in sorted(os.listdir(rows_dir)):
        if os.path.isdir(os.path.join(rows_dir, name)):
            names.append(name)
        else:
            problems.append("%s sits beside the row directories -- a rows directory holds one directory "
                            "per anchor and nothing else" % name)
    if not names and not problems:
        problems.append("%s holds no rows -- a batch of nothing is refused, never reported clean" % show(rows_dir))
    if only is not None:
        missing = sorted(set(only) - set(names))
        if missing:
            problems.append("--only names %d row(s) %s does not hold: %s -- a typo would silently drop a row"
                            % (len(missing), show(rows_dir), ", ".join(missing)))
        names = [n for n in names if n in set(only)]
    for name in names:
        if not row_dir.match(name):
            problems.append("%s is not an anchor id (%s-<AREA>-<TOPIC>..., at least %d segments: config.json "
                            "`anchors`)" % (name, grammar.prefix, grammar.min_segments))
            continue
        cells, probs, skip, new = read_row(os.path.join(rows_dir, name), live_only, staged)
        problems += probs
        if skip:
            skipped[name] = skip
        if not probs:
            rows.append(Row(name, cells, new))
    return rows, problems, skipped


# ──────────────────────────────── the door's form ────────────────────────────────
# What the door STORES for a cell is `anchors.door_form` (✔MEASURED 2026-09-25, DssHarness 0.5.12; pinned against
# the real door by self-test arm (p1)): the launcher's content refusals judge that same form, so it has ONE owner.

def collapse(text):
    return " ".join(text.split())


def unescape(cell):
    """A stored cell as `read-anchor --json` returns it: every `\\|` a pipe again (M3)."""
    return cell.replace(ESCAPED_PIPE, "|")


def cell_verdict(cell, new, stored):
    """-> SAME / RESPACED / KEPT / FILLED / LOST for a prose cell; SAME / CHANGED for status and priority.
    `new` is the declared text (status and priority already normalised); `stored` what the row holds. KEPT is WORD
    for word: the stored words must be a contiguous run of the new words, so an extended first or last word
    ("fixed" -> "unfixed", an id re-pointed by a suffix) is LOST, never an addendum (the audit's F1-A10)."""
    if cell not in PROSE:
        return "SAME" if new == stored else "CHANGED"
    form = anchors().door_form(new)
    if form == stored:
        return "SAME"
    if not stored.strip():
        return "FILLED"
    if collapse(form) == collapse(stored):
        return "RESPACED"
    if " %s " % collapse(stored) in " %s " % collapse(form):
        return "KEPT"
    return "LOST"


def word_diff(old, new):
    """-> [(removed, replacement)] -- every run of words `old` has that `new` drops, with what stands in
    its place ("" for a pure deletion)."""
    a, b = old.split(), new.split()
    out = []
    for op, i1, i2, j1, j2 in difflib.SequenceMatcher(a=a, b=b, autojunk=False).get_opcodes():
        if op in ("replace", "delete"):
            out.append((" ".join(a[i1:i2]), " ".join(b[j1:j2]) if op == "replace" else ""))
    return out


# ──────────────────────────────── the plan ────────────────────────────────

Plan = collections.namedtuple("Plan", "anchor kind cells stored verdicts fields diffs")
# kind: "new" (write-anchor), "update" (set-anchor), "landed" (every supplied cell already SAME)


def validate(row, roots, grammar):
    """-> ({cell: normalised value}, [problem]): the vocabulary, the pre-escaped pipe and the content
    refusals the door does not make (judged as the door will store the text, under the tree's id `grammar`),
    each named with the row and the cell."""
    A = anchors()
    values, problems = {}, []
    for cell, text in row.cells.items():
        if cell == "status":
            try:
                values[cell] = A.normalise_status(text.strip())
            except A.Refused as exc:
                problems.append("%s/status: %s" % (row.anchor, exc))
        elif cell == "priority":
            try:
                values[cell] = A.normalise_priority(text)
            except A.Refused as exc:
                problems.append("%s/priority: %s" % (row.anchor, exc))
        else:
            if ESCAPED_PIPE in text:
                problems.append("%s/%s holds a pre-escaped pipe -- the door escapes every pipe itself; "
                                "`stage` turns a lane's back into a bare one" % (row.anchor, cell))
            for why in A.cell_refusals(text, roots, grammar):
                problems.append("%s/%s %s" % (row.anchor, cell, why))
            values[cell] = text
    return values, problems


def plan_rows(root, rows):
    """-> ([Plan] in APPLY ORDER, [problem]). Reads the registries once; writes nothing. Refused, each named: a row
    `validate` refuses; an id with two rows; an id no registry holds that the batch did not DECLARE new (a mistyped
    id would mint a second row and leave the real one untouched -- `apply-registry-row`'s `--insert` rule, the
    audit's F1-A4); a row declared new that a registry already holds OTHER than as declared (one standing exactly
    as declared is this batch's own, landed: a re-run writes nothing); a NEW row missing a cell the door requires;
    and an id a row NEWLY cites that is a row of neither registry nor the batch (the door checks no cited id and no
    guard reads the registries' own citations -- the audit's F1-A3). A citation already stored is history, never
    re-judged: in an existing row only the ids its new text cites and its stored cell did not are resolved."""
    A = anchors()
    try:
        grammar, roots = A.id_grammar(root), A.tree_dirs(root)
    except A.Refused as exc:
        return [], ["the rows cannot be checked against this tree (%s)" % exc]
    try:
        index = {}
        for r in A.read_rows(root):
            index.setdefault(r.name, []).append(r)
    except A.Refused as exc:
        return [], ["the registries cannot be read (%s)" % exc]
    known = set(index) | set(row.anchor for row in rows)
    plans, problems = [], []
    for row in rows:
        values, probs = validate(row, roots, grammar)
        if probs:
            problems += probs
            continue
        found = index.get(row.anchor, [])
        if len(found) > 1:
            problems.append("%s has %d rows (%s) -- a duplicate is settled by a human, never by a batch"
                            % (row.anchor, len(found), ", ".join(sorted({r.rel for r in found}))))
            continue
        if not found:
            if not row.new:
                problems.append("%s is a row of neither registry, and the batch did not declare it NEW -- a mistyped "
                                "id would mint a second row and leave the real one untouched; if it IS new, stage it "
                                "with the step's `new` input naming it" % row.anchor)
                continue
            missing = [c for c in NEW_NEEDS if c not in values]
            if missing:
                problems.append("%s is NEW, and a new row needs %s -- missing %s"
                                % (row.anchor, ", ".join("%s.txt" % c for c in NEW_NEEDS),
                                   ", ".join("%s.txt" % c for c in missing)))
                continue
            fields = dict((FIELD[c], values[c]) for c in values)
            cites = dict((c, A.cited_ids(A.door_form(values[c]), grammar)) for c in PROSE if c in values)
            plan = Plan(row.anchor, "new", values, None, dict((c, "NEW") for c in values), fields, {})
        else:
            r = found[0]
            stored = {"status": r.status, "priority": r.priority,
                      "trigger": unescape(r.cell(A.C_TRIGGER).strip()),
                      "closing": unescape(r.cell(A.C_CLOSING).strip()),
                      "crossrefs": unescape(r.cell(A.C_XREF).strip())}
            verdicts, fields, diffs = {}, {}, {}
            for cell in CELLS:
                if cell not in values:
                    continue
                verdict = cell_verdict(cell, values[cell], stored[cell])
                verdicts[cell] = verdict
                if verdict != "SAME":
                    fields[FIELD[cell]] = values[cell]
                if verdict == "LOST":
                    diffs[cell] = word_diff(stored[cell], A.door_form(values[cell]))
            if row.new and fields:
                # a row declared new that already stands EXACTLY as declared is this batch's own, landed (a fold
                # re-run); one that differs is another row under that id
                problems.append("%s was declared NEW, but %s already holds it, and differently (%s) -- a new row over "
                                "an existing one is refused; to update it, stage it without declaring it new"
                                % (row.anchor, r.rel, ", ".join("%s %s" % (c, verdicts[c]) for c in CELLS
                                                                if verdicts.get(c, "SAME") != "SAME")))
                continue
            cites = dict((c, A.cited_ids(A.door_form(values[c]), grammar) - A.cited_ids(stored[c], grammar))
                         for c in PROSE if c in values and verdicts[c] != "SAME")
            plan = Plan(row.anchor, "update" if fields else "landed", values, stored, verdicts, fields, diffs)
        dangling = [(cell, cid) for cell in sorted(cites) for cid in sorted(cites[cell] - known)]
        for cell, cid in dangling:
            problems.append("%s/%s cites %s, a row of neither registry nor this batch -- a citation nothing resolves "
                            "(the door checks no cited id, and no guard reads the registries' own citations): name "
                            "the row it means, or add that row to the batch" % (row.anchor, cell, cid))
        if not dangling:
            plans.append(plan)
    order = {"new": 0, "update": 1, "landed": 2}
    plans.sort(key=lambda p: (order[p.kind], p.anchor))
    return plans, problems


def lost_cells(plans):
    return sorted("%s:%s" % (p.anchor, c) for p in plans for c, v in p.verdicts.items() if v == "LOST")


# ──────────────────────────────── the door, read and written ────────────────────────────────

def _door(tree, args):
    """-> (rc, stdout, stderr) of one DssHarness reader in `tree`, without the caller's git environment."""
    proc = ot().run_unsteered([anchors().door_executable()] + list(args) + ["-C", tree], capture_output=True)
    return (proc.returncode, proc.stdout.decode("utf-8", "replace"), proc.stderr.decode("utf-8", "replace"))


def read_back(tree, anchor):
    """-> (the row as `read-anchor --json` returns it, "") or (None, why). STDOUT alone is parsed: a
    failed read prints `[]` there and its FAIL line on stderr (✔MEASURED, P68 round 8)."""
    rc, out, err = _door(tree, ["read-anchor", anchor, "--json"])
    if rc != 0:
        return None, (err.strip().splitlines() or ["read-anchor exited %d" % rc])[0]
    try:
        rows = json.loads(out)
    except ValueError as exc:
        return None, "read-anchor --json printed no JSON (%s)" % exc
    if not isinstance(rows, list) or len(rows) != 1:
        return None, "read-anchor --json answered %r, not one row" % (type(rows).__name__,)
    return rows[0], ""


def verify(plan, got, done_rel, pending_rel):
    """-> [problem]: every cell the plan DECLARED, as the door returned it, against its declaration."""
    wrong = []
    for cell, value in plan.cells.items():
        have = got.get(FIELD[cell], "")
        want = anchors().door_form(value) if cell in PROSE else value
        if have != want:
            wrong.append("%s reads back %r, declared %r" % (cell, have[:120], want[:120]))
    closed = anchors().bal.is_closed(got.get("status", ""))
    home = done_rel if closed else pending_rel
    if got.get("registry", "").replace("\\", "/") != home:
        wrong.append("it lives in %s, not %s" % (got.get("registry"), home))
    return wrong


def write_in_order(tree, plans, cfg, door=None, written=None):
    """Write every plan that has fields, in order, each read back. -> ([written anchor], None) or
    ([written anchor], (anchor, why)) at the first failure. `written`, when given, is the list each anchor is
    appended to AS IT LANDS, so a caller whose batch an exception interrupts still knows what landed. `door` is the
    self-test's stand-in for `anchors.door_write`."""
    write = door or anchors().door_write
    written = [] if written is None else written
    for p in plans:
        if p.kind == "landed":
            continue
        rc, out = write(tree, p.anchor, p.fields, p.kind == "new", True)
        if rc != 0:
            lines = [ln for ln in out.strip().splitlines() if ln.strip()]
            return written, (p.anchor, "the door refused it (rc=%d)%s" % (rc, (": " + lines[-1]) if lines else ""))
        written.append(p.anchor)
        got, why = read_back(tree, p.anchor)
        if got is None:
            return written, (p.anchor, "written, but it does not read back: %s" % why)
        wrong = verify(p, got, cfg["doneAnchorsPath"], cfg["pendingAnchorsPath"])
        if wrong:
            return written, (p.anchor, "written, but it does not read back as declared: %s" % "; ".join(wrong))
    for p in plans:
        if p.kind != "landed":
            continue
        got, why = read_back(tree, p.anchor)
        wrong = [why] if got is None else verify(p, got, cfg["doneAnchorsPath"], cfg["pendingAnchorsPath"])
        if wrong:
            return written, (p.anchor, "already landed by the plan, but it does not read back as declared: %s"
                             % "; ".join(wrong))
    return written, None


def anchors_config(root):
    cfg = ot().load_jsonc(os.path.join(root, ".harness-config", "config.json"))
    section = cfg.get("anchors") if isinstance(cfg, dict) else None
    if not isinstance(section, dict) or not section.get("pendingAnchorsPath") or not section.get("doneAnchorsPath"):
        raise Refused("%s declares no `anchors` registries, so the door has none"
                      % os.path.join(root, ".harness-config", "config.json"))
    return section


def registry_bytes(root, cfg):
    out = {}
    for rel in (cfg["pendingAnchorsPath"], cfg["doneAnchorsPath"]):
        with io.open(os.path.join(root, *rel.split("/")), "rb") as fh:
            out[rel] = fh.read()
    return out


def foreign_changes(before, after, batch):
    """-> [what `after` (registry bytes, per registry) holds that `before` did not, OUTSIDE the rows of `batch` (the
    ids this batch writes)] -- empty when the registries hold exactly the snapshot plus rows of this batch. Lines are
    compared without their line ending; a row is recognised by the reader's own `split_row` / `row_name`."""
    bal = anchors().bal

    def outside_batch(data):
        out = []
        for ln in data.decode("utf-8", "replace").split("\n"):
            ln = ln.rstrip("\r")
            if ln.lstrip().startswith("|"):
                cells = bal.split_row(ln)
                if len(cells) > 1 and bal.row_name(cells[1]) in batch:
                    continue
            out.append(ln)
        return out
    found = []
    for rel in sorted(before):
        a, b = outside_batch(before[rel]), outside_batch(after.get(rel, b""))
        if a != b:
            first = next((y for x, y in zip(a, b) if x != y), (b[len(a)] if len(b) > len(a) else "(a line removed)"))
            found.append("%s changed outside this batch's rows (%r)" % (rel, first[:100]))
    return found


def restore(root, snapshot, batch):
    """Put every registry back to `snapshot`'s bytes -> ([restored], [failed, each with its reason], why not).
    ONLY when the registries hold exactly the snapshot plus rows of `batch` (`foreign_changes`): a change another
    writer made since the snapshot would be DISCARDED by a restore, so then nothing is written and `why not` says
    what moved -- a human reads the registries first (the audit's F1-A5). A failed read or write keeps its reason."""
    current, failed = {}, []
    for rel in sorted(snapshot):
        try:
            with io.open(os.path.join(root, *rel.split("/")), "rb") as fh:
                current[rel] = fh.read()
        except OSError as exc:
            failed.append("%s (%s)" % (rel, exc))
    if failed:
        return [], failed, ""
    foreign = foreign_changes(snapshot, current, batch)
    if foreign:
        return [], [], "; ".join(foreign)
    restored = []
    for rel, data in sorted(snapshot.items()):
        if current[rel] == data:
            continue
        path = os.path.join(root, *rel.split("/"))
        try:
            tmp = path + ".anchor-rows-tmp"
            with io.open(tmp, "wb") as fh:
                fh.write(data)
            os.replace(tmp, path)
            with io.open(path, "rb") as fh:
                if fh.read() == data:
                    restored.append(rel)
                else:
                    failed.append("%s (it reads back other bytes)" % rel)
        except OSError as exc:
            failed.append("%s (%s)" % (rel, exc))
    return restored, failed, ""


def restore_report(restored, failed, foreign):
    """The sentence a failed batch ends with: what the restore did, or why it did nothing."""
    if foreign:
        return ("⚠⚠ NOT RESTORED -- the registries hold a change that is not one of this batch's rows (%s): another "
                "writer's work, which a restore would discard. Read both registries before anything else." % foreign)
    if failed:
        return "⚠⚠ RESTORE FAILED for %s -- read those registries before anything else." % ", ".join(failed)
    if restored:
        return "RESTORED byte for byte: %s." % ", ".join(restored)
    return "Nothing was left written."


# ──────────────────────────────── the rehearsal ────────────────────────────────

Rehearsal = collections.namedtuple("Rehearsal", "ok written failure lint balance")


def balance_verdict(rc, out):
    """-> (text, ok) for the rehearsal's `check-anchor-balance --json` receipt: its numbers AND the gate's own
    verdict (the audit's F1-A7: the verdict was discarded, and an unreadable receipt still read "rehearses clean").
    A net increase is the ROUND gate's to judge -- a batch may open disclosed or gated work -- so it is SAID and does
    not fail the rehearsal; an unreadable receipt, a finding (a misfiled or malformed registry), a row missing at the
    base, or a failure this program cannot name DOES."""
    try:
        doc = json.loads(out)
        text = "%d open before, %d after (%d closed, %d opened)" % (
            doc["openAtBase"], doc["openNow"], len(doc["closed"]), len(doc["opened"]))
        passed, net, findings, missing = doc["passed"], doc["netNew"], doc["findings"], doc["missingAtBase"]
    except (ValueError, KeyError, TypeError) as exc:
        return "UNREADABLE -- check-anchor-balance exited %d and its receipt could not be read (%s: %s)" % (
            rc, type(exc).__name__, exc), False
    if findings:
        return "%s; balance gate FAIL: %d finding(s), the first %s" % (
            text, len(findings), json.dumps(findings[0], ensure_ascii=False)[:200]), False
    if missing:
        return "%s; balance gate FAIL: %d row(s) missing at the base (%s)" % (
            text, len(missing), json.dumps(missing[:3], ensure_ascii=False)[:200]), False
    if passed is True and rc == 0:
        return "%s; balance gate PASS" % text, True
    if passed is False and isinstance(net, int) and not isinstance(net, bool) and net > 0:
        return "%s; balance gate FAIL (net +%d) -- judged at the round gate, not here" % (text, net), True
    return "%s; balance gate %s (exit %d) for a reason this program cannot name" % (
        text, "FAIL" if passed is False else "answered passed=%r" % (passed,), rc), False


def _balance_receipt(tree):
    return _door(tree, ["check-anchor-balance", "--json"])


def rehearse(root, plans, door=None, balance=None):
    """Write the whole batch into a THROWAWAY git repository holding `root`'s two registries (their
    current bytes, committed) and config.json's `anchors` section, read every row back, then lint and
    balance there. -> Rehearsal. The real registries are only read. `door` and `balance` (the receipt's
    reader) are the self-test's stand-ins."""
    cfg = anchors_config(root)
    box = tempfile.mkdtemp(prefix="anchor-rows-rehearsal-")
    try:
        tree = os.path.join(box, "t")
        for rel, data in registry_bytes(root, cfg).items():
            path = os.path.join(tree, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with io.open(path, "wb") as fh:
                fh.write(data)
        os.makedirs(os.path.join(tree, ".harness-config"))
        with io.open(os.path.join(tree, ".harness-config", "config.json"), "w", encoding="utf-8") as fh:
            json.dump({"anchors": cfg}, fh, indent=1)
        for args in (["init", "-q", tree], ["-C", tree, "add", "-A"],
                     ["-C", tree, "-c", "user.email=anchor-rows@example.invalid", "-c", "user.name=anchor-rows",
                      "-c", "commit.gpgsign=false", "commit", "-q", "--no-verify", "-m", "the registries as they stand"]):
            proc = ot().run_git(args, capture_output=True)
            if proc.returncode != 0:
                raise Refused("the rehearsal repository could not be prepared: git %s -> %s"
                              % (" ".join("<rehearsal>" if a == tree else a for a in args[:3]),
                                 proc.stderr.decode("utf-8", "replace").strip().replace(box, "<rehearsal>")))
        written, failure = write_in_order(tree, plans, cfg, door)
        if failure is not None:
            failure = (failure[0], failure[1].replace(box, "<rehearsal>"))
        rc, out, err = _door(tree, ["read-anchors", "--lint"])
        lint = "clean" if rc == 0 else (out + err).strip().splitlines()[-1:] or ["lint exited %d" % rc]
        rc, out, _err = (balance or _balance_receipt)(tree)
        text, gate_ok = balance_verdict(rc, out)
        return Rehearsal(failure is None and lint == "clean" and gate_ok, written, failure, lint, text)
    finally:
        ot().remove_tree(box)


# ──────────────────────────────── the verbs ────────────────────────────────

def _write_bytes(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with io.open(path, "wb") as fh:
        fh.write(data)


def cmd_stage(root, src, dst, only, live_only, new=(), write=None):
    """Copy EXACTLY the rows `only` names from `src` into the NEW directory `dst`, each pre-escaped pipe
    turned back into a bare one, every cell under its canonical name, and each row `new` names marked DECLARED
    NEW (`NEW_MARKER`) -- `new` may name only rows `only` carries. Validates everything first. The record is
    written into `<dst>.partial`, every file read back against what was meant, and only then renamed to `dst`:
    a stage that stops part-way leaves no staged directory for `check` or `apply` to take as the whole batch
    (the audit's F1-A15). The lane's rows it did not carry are named. `write` is the self-test's stand-in for
    the file writer."""
    show = lambda p: _as_input(root, p)
    if os.path.exists(dst):
        raise Refused("%s exists -- a staged directory is the fold's record of what it carried, so it is "
                      "made fresh, never merged into" % show(dst))
    partial = dst + ".partial"
    if os.path.exists(partial):
        raise Refused("%s exists -- a stage that stopped part-way left it; remove it, then stage again"
                      % show(partial))
    grammar = anchors().id_grammar(root)
    rows, problems, skipped = load_rows(src, grammar, only, live_only, show=show)
    stray = sorted(set(new) - set(only))
    if stray:
        problems.append("new names %s, which only does not carry -- a row is declared new where it is staged"
                        % ", ".join(stray))
    unescaped = {}
    for row in rows:
        for cell, text in list(row.cells.items()):
            n = text.count(ESCAPED_PIPE)
            if n:
                row.cells[cell] = text.replace(ESCAPED_PIPE, "|")
                unescaped["%s/%s" % (row.anchor, cell)] = n
    if not problems:
        roots = anchors().tree_dirs(root)
        for row in rows:
            problems += validate(row, roots, grammar)[1]
    if problems:
        raise Refused("%d problem(s), nothing staged:\n   %s" % (len(problems), "\n   ".join(problems)))
    meant = {}
    for row in rows:
        for cell, text in row.cells.items():
            meant[os.path.join(row.anchor, cell + ".txt")] = text.encode("utf-8")
        if row.anchor in set(new):
            meant[os.path.join(row.anchor, NEW_MARKER)] = NEW_MARKER_TEXT
    try:
        for rel, data in sorted(meant.items()):
            (write or _write_bytes)(os.path.join(partial, rel), data)
        got = {}
        for d, _dirs, files in os.walk(partial):
            for f in files:
                with io.open(os.path.join(d, f), "rb") as fh:
                    got[os.path.relpath(os.path.join(d, f), partial)] = fh.read()
        wrong = sorted(set(got) ^ set(meant)) + sorted(k for k in set(got) & set(meant) if got[k] != meant[k])
        if wrong:
            raise Refused("the staged record does not read back as written (%s) -- nothing staged"
                          % ", ".join(wrong[:8]))
        os.rename(partial, dst)
    except BaseException:
        ot().remove_tree(partial)
        raise
    for key, n in sorted(unescaped.items()):
        print("anchor-rows: %s: %d pre-escaped pipe(s) turned back into bare ones" % (key, n))
    for anchor, names in sorted(skipped.items()):
        print("anchor-rows: %s: skipped %s (--live-only)" % (anchor, ", ".join(names)))
    left = sorted(n for n in os.listdir(src) if os.path.isdir(os.path.join(src, n)) and n not in set(only))
    print("anchor-rows: not carried (only does not name them): %s"
          % (", ".join(left) if left else "none -- every row of %s was carried" % show(src)))
    print("anchor-rows: staged %d row(s) into %s: %s%s"
          % (len(rows), show(dst), ", ".join(r.anchor for r in rows),
             ("; declared NEW: %s" % ", ".join(sorted(new))) if new else ""))
    return 0


def _print_plan(plans):
    for p in plans:
        what = {"new": "NEW", "update": "UPDATE", "landed": "ALREADY LANDED -- nothing to write"}[p.kind]
        print("   %-66s %s" % (p.anchor, what))
        if p.kind != "new":
            print("      %s" % ", ".join("%s %s" % (c, p.verdicts[c]) for c in CELLS if c in p.verdicts))
            for cell in ("status", "priority"):
                if p.verdicts.get(cell) == "CHANGED":
                    print("      %s: %s -> %s" % (cell, p.stored[cell], p.cells[cell]))
        for cell, pairs in sorted(p.diffs.items()):
            print("      LOST %s -- the words it removes:" % cell)
            for removed, replacement in pairs:
                print("        REMOVED: %s" % _clip(removed))
                if replacement:
                    print("        WITH   : %s" % _clip(replacement))


def _clip(text, n=600):
    return text if len(text) <= n else "%s ... (%d characters)" % (text[:n], len(text))


def _plan_or_refuse(root, staged):
    rows, problems, _skipped = load_rows(staged, anchors().id_grammar(root), staged=True,
                                         show=lambda p: _as_input(root, p))
    if not problems:
        plans, problems = plan_rows(root, rows)
    if problems:
        raise Refused("%d problem(s), nothing written:\n   %s" % (len(problems), "\n   ".join(problems)))
    return plans


def _print_rehearsal(r):
    print("anchor-rows: REHEARSAL in a throwaway repository: %d row(s) written and read back%s; lint %s; "
          "balance %s" % (len(r.written), "" if r.failure is None else ", then %s REFUSED: %s" % r.failure,
                          r.lint if r.lint == "clean" else "FAILED: %s" % " ".join(r.lint), r.balance))


def cmd_check(root, staged, balance=None):
    plans = _plan_or_refuse(root, staged)
    print("anchor-rows: %d row(s) in %s, in apply order:" % (len(plans), _as_input(root, staged)))
    _print_plan(plans)
    r = rehearse(root, plans, balance=balance)
    _print_rehearsal(r)
    if not r.ok:
        raise Refused("the rehearsal failed -- the batch as staged would not land as declared")
    lost = lost_cells(plans)
    if lost:
        print("anchor-rows: %d LOST cell(s): read each word diff above; applying the batch then names every one: %s"
              % (len(lost), step_form("apply", ("staged", _as_input(root, staged)), ("acceptLost", ",".join(lost)))))
    else:
        print("anchor-rows: the batch is applied with %s" % step_form("apply", ("staged", _as_input(root, staged))))
    print("anchor-rows: check OK -- the batch rehearses clean; the registries were not touched")
    return 0


def _lint(tree):
    return _door(tree, ["read-anchors", "--lint"])


def cmd_apply(root, staged, accepted, door=None, balance=None, lint=None):
    """`door`, `balance` and `lint` (the post-apply lint's reader) are the self-test's stand-ins."""
    cfg = anchors_config(root)
    before = registry_bytes(root, cfg)
    plans = _plan_or_refuse(root, staged)
    lost = set(lost_cells(plans))
    stale = sorted(set(accepted) - lost)
    if stale:
        raise Refused("--accept-lost names %s, which %s not LOST in this plan -- a stale acceptance, or a typo; "
                      "nothing written" % (", ".join(stale), "is" if len(stale) == 1 else "are"))
    unaccepted = sorted(lost - set(accepted))
    if unaccepted:
        _print_plan(plans)
        raise Refused("%d LOST cell(s) would rewrite what the registry says: read each word diff above, then name "
                      "every LOST cell: %s; nothing written"
                      % (len(unaccepted), step_form("apply", ("staged", _as_input(root, staged)),
                                                    ("acceptLost", ",".join(sorted(lost))))))
    print("anchor-rows: %d row(s) in %s, in apply order:" % (len(plans), _as_input(root, staged)))
    _print_plan(plans)
    r = rehearse(root, plans, door, balance)
    _print_rehearsal(r)
    if not r.ok:
        raise Refused("the rehearsal failed -- nothing written to the registries")
    if registry_bytes(root, cfg) != before:
        raise Refused("the registries changed while this batch was planned and rehearsed -- another writer; "
                      "nothing written")
    # ★ ALL OR NOTHING, WHATEVER STOPS IT: a door refusal, a row that does not read back, registries that do not
    # lint afterwards -- and an EXCEPTION between two writes (a door that cannot start, a reader refusal, an
    # interrupt), which until the audit's F1-A5 left the rows written so far in place, unrestored and unsaid.
    # Every path restores through `restore`, which refuses to discard another writer's change.
    batch = set(p.anchor for p in plans)
    written = []
    try:
        _w, failure = write_in_order(root, plans, cfg, door, written)
        if failure is not None:
            raise Refused("%s -- %s. Written before it: %s. %s"
                          % (failure[0], failure[1], ", ".join(written) or "none",
                             restore_report(*restore(root, before, batch))))
        rc, out, err = (lint or _lint)(root)
        if rc != 0:
            raise Refused("the registries do not lint after the batch (%s) -- %s"
                          % (" ".join((out + err).strip().splitlines()[-1:]),
                             restore_report(*restore(root, before, batch))))
    except Refused:
        raise
    except BaseException as exc:
        print("anchor-rows: the batch was INTERRUPTED (%s: %s) -- written before it: %s. %s"
              % (type(exc).__name__, exc, ", ".join(written) or "none",
                 restore_report(*restore(root, before, batch))))
        raise
    landed = sum(1 for p in plans if p.kind == "landed")
    print("anchor-rows: APPLIED %d row(s) (%d new, %d updated), %d already landed; every row read back as "
          "declared, and every id the batch newly cites resolved to a row of a registry or of the batch."
          % (len(written), sum(1 for p in plans if p.kind == "new"), sum(1 for p in plans if p.kind == "update"),
             landed))
    return 0


# ──────────────────────────────── self-test ────────────────────────────────

def self_test():
    """Red-on-disable, on git fixture repositories and the REAL door. Every refusal asserts its MESSAGE,
    and each family has a control that passes, so a program refusing everything cannot produce this
    transcript. ⚠ A host without DssHarness FAILS the door arms: they measure the door."""
    import contextlib
    failed = [0]
    count = [0]
    # ⚠ FIXTURE IDS ARE ASSEMBLED FROM FRAGMENTS: this directory is a citation root, and a whole
    # three-segment id written here would be a CITATION the anchor guards must resolve.
    fx = "D-" + "FIXTURE-ROWS"
    A_, B_, C_, N_ = fx + "-ALPHA", fx + "-BETA", fx + "-GAMMA", fx + "-NEWONE"
    A = anchors()

    tmp_root = tempfile.gettempdir()

    def pin(ok, why, detail=""):
        count[0] += 1
        # a failing arm's detail names fixture paths: the system temp directory is masked, because on Windows it lies
        # under the user profile and a run's log must not name the account (the audit's F1-A12)
        shown = str(detail).replace(tmp_root, "<temp>").replace(tmp_root.replace("\\", "\\\\"), "<temp>")
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", why, ("   " + shown[:300]) if detail and not ok else ""))
        if not ok:
            failed[0] += 1

    def refused(fn, *a, **k):
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                fn(*a, **k)
            return None
        except Refused as exc:
            return str(exc)

    def ran(fn, *a, **k):
        """-> (rc, what it printed, its refusal or ""): a verb that REFUSES where an arm expects it to pass is a
        FAIL of that arm, never a crash of the whole self-test -- and so is a USAGE exit, which argparse raises as
        SystemExit (✔MEASURED: a mutant that let `--repo` reach argparse ended the self-test at exit 3, naming
        no arm) -- and so is a refusal of the family's other types (`refusal_types`)."""
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                return fn(*a, **k), buf.getvalue(), ""
        except refusal_types() as exc:
            return REFUSED, buf.getvalue(), "%s: %s" % (type(exc).__name__, exc)
        except SystemExit as exc:
            return (exc.code if isinstance(exc.code, int) else USAGE), buf.getvalue(), "SystemExit(%r)" % (exc.code,)

    def write(path, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with io.open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)

    def rowdir(root, anchor, **cells):
        for cell, text in cells.items():
            write(os.path.join(root, anchor, "%s.txt" % cell.replace("_", "-")), text)

    def md5s(top):
        return dict((os.path.join(d, f), hashlib.md5(open(os.path.join(d, f), "rb").read()).hexdigest())
                    for d, _s, fs in os.walk(top) for f in fs)

    cfg = anchors_config(repo_root())
    hdr = A.TABLE_HEADER + "\n" + A.SEP_ROW_TEXT + "\n"
    boxes = []

    def fixture_tree(section=None):
        """A git repository holding THIS tree's `anchors` configuration (or `section`) and two small registries,
        with a `tests/` directory so the path-cut refusal has a root to see."""
        t = tempfile.mkdtemp(prefix="anchor-rows-fixture-")
        boxes.append(t)
        write(os.path.join(t, *cfg["pendingAnchorsPath"].split("/")), "# p\n\n" + hdr
              + "| `%s` | P2 | 🟠 OPEN | 🟠 **OPEN** the alpha work | w  keeps  its spacing | r |\n" % A_
              + "| `%s` | P3 | 🟠 OPEN | 🟠 **OPEN** the beta work | c | x \\| y |\n" % B_)
        write(os.path.join(t, *cfg["doneAnchorsPath"].split("/")),
              "# d\n\n" + A.DONE_TABLE["production"] + "\n\n" + hdr)
        write(os.path.join(t, ".harness-config", "config.json"), json.dumps({"anchors": section or cfg}))
        write(os.path.join(t, "tests", "hir", "keep.txt"), "x\n")
        for args in (["init", "-q", t], ["-C", t, "add", "-A"],
                     ["-C", t, "-c", "user.email=fx@example.invalid", "-c", "user.name=fx", "-c",
                      "commit.gpgsign=false", "commit", "-q", "--no-verify", "-m", "fixture"]):
            ot().run_git(args, capture_output=True, check=True)
        return t

    def reg(t):
        return registry_bytes(t, cfg)

    def rows_of(d, t):
        """The staged rows of `d`, read under tree `t`'s id grammar."""
        return load_rows(d, A.id_grammar(t), staged=True)[0]

    def declare_new(d, anchor):
        """What `stage` writes for a row its `new` input names, for a staged directory an arm builds by hand."""
        with io.open(os.path.join(d, anchor, NEW_MARKER), "wb") as fh:
            fh.write(NEW_MARKER_TEXT)

    try:
        # ── (s) STAGE ──────────────────────────────────────────────────────────────
        top = tempfile.mkdtemp(prefix="anchor-rows-stage-")
        boxes.append(top)
        tree = fixture_tree()
        src = os.path.join(top, "lane-rows")
        rowdir(src, A_, status="open", trigger="🟠 **OPEN** a " + ESCAPED_PIPE + " b", closing="c")
        rowdir(src, B_, status="open", trigger="🟠 **OPEN** beta", priority="P3", cross_refs="r")
        rowdir(src, C_, status="open", trigger="🟠 **OPEN** held back", priority="P3")
        src_md5 = md5s(src)
        os.makedirs(os.path.join(top, "exists"))
        why = refused(cmd_stage, tree, src, os.path.join(top, "exists"), [A_], False) or ""
        pin("exists" in why and "made fresh" in why,
            "(s1) a staged directory that EXISTS is refused, never merged into", why)
        why = refused(cmd_stage, tree, src, os.path.join(top, "s2"), [A_, fx + "-TYPO"], False) or ""
        pin("does not hold" in why and fx + "-TYPO" in why and not os.path.exists(os.path.join(top, "s2")),
            "(s2) --only naming a row the lane never wrote is refused, naming it, and nothing is staged", why)
        bad = os.path.join(top, "bad-rows")
        rowdir(bad, A_, status="open", trigger="t", notes="draft")
        why = refused(cmd_stage, tree, bad, os.path.join(top, "s3"), [A_], False) or ""
        pin("notes.txt is not a cell file" in why, "(s3) a draft file beside the cells is refused by name", why)
        both = os.path.join(top, "both-rows")
        rowdir(both, A_, status="open", trigger="t", cross_refs="r1")
        write(os.path.join(both, A_, "crossrefs.txt"), "r2")
        why = refused(cmd_stage, tree, both, os.path.join(top, "s4"), [A_], False) or ""
        pin("BOTH cross-refs.txt and crossrefs.txt" in why,
            "(s4) both crossrefs spellings are refused, never settled by preference", why)
        enc = os.path.join(top, "enc-rows")
        rowdir(enc, A_, status="open")
        with io.open(os.path.join(enc, A_, "trigger.txt"), "wb") as fh:
            fh.write(b"\xff\xfe not utf-8")
        with io.open(os.path.join(enc, A_, "closing.txt"), "wb") as fh:
            fh.write(BOM + b"c")
        why = refused(cmd_stage, tree, enc, os.path.join(top, "s5"), [A_], False) or ""
        pin("trigger.txt is not UTF-8" in why and "closing.txt opens with a byte-order mark" in why,
            "(s5) a non-UTF-8 cell and a BOM-led cell are both refused, each named", why)
        applied = os.path.join(top, "applied-rows")
        rowdir(applied, A_, status="open", trigger="🟠 **OPEN** t")
        write(os.path.join(applied, A_, "trigger.txt.applied"), "old")
        why = refused(cmd_stage, tree, applied, os.path.join(top, "s6"), [A_], False) or ""
        out = os.path.join(top, "s6b")
        rc, said, why2 = ran(cmd_stage, tree, applied, out, [A_], True)
        pin("needs --live-only" in why and rc == 0 and os.path.isdir(os.path.join(out, A_))
            and sorted(os.listdir(os.path.join(out, A_))) == ["status.txt", "trigger.txt"]
            and "skipped trigger.txt.applied" in said,
            "(s6) an *.applied cell is refused without --live-only, and skipped, said, with it", why + why2)
        # (s7) a path cut, in EVERY shape a lane's cell file holds it -- the line break the door will join included
        # (the audit's F1-A1: the first arm synthesized only the already-joined space, and the refusal could not see
        # a break).
        for label, text in (("(s7) the already-joined space", "see tests/hir/ test_brace.cpp for it"),
                            ("(s7b) a line break after the `/`", "see tests/hir/\ntest_brace.cpp for it"),
                            ("(s7c) a line break and an indent after the `/`", "see tests/hir/\n    test_brace.cpp"),
                            ("(s7d) a CRLF after the `/`", "see tests/hir/\r\ntest_brace.cpp for it")):
            cut = os.path.join(top, "cut-rows-%s" % label[1:4].strip(")"))
            rowdir(cut, A_, status="open", trigger="🟠 **OPEN** " + text)
            why = refused(cmd_stage, tree, cut, os.path.join(top, "s7-" + label[1:4].strip(")")), [A_], False) or ""
            pin("joined-line space" in why and "trigger" in why,
                "%s: a path cut there is refused at stage, naming the cell -- the door would store it cut" % label, why)
        good = os.path.join(top, "staged")
        rc, said, why = ran(cmd_stage, tree, src, good, [A_, B_], False)
        staged_a = (io.open(os.path.join(good, A_, "trigger.txt"), encoding="utf-8").read()
                    if os.path.isfile(os.path.join(good, A_, "trigger.txt")) else None)
        pin(rc == 0 and os.path.isdir(good) and sorted(os.listdir(good)) == sorted([A_, B_])
            and staged_a == "🟠 **OPEN** a | b"
            and os.path.isfile(os.path.join(good, B_, "crossrefs.txt"))
            and not any(NEW_MARKER in fs for _d, _s, fs in os.walk(good))
            and not os.path.exists(good + ".partial")
            and "1 pre-escaped pipe(s)" in said and md5s(src) == src_md5,
            "(s8) CONTROL: stage copies ONLY the named rows under canonical cell names, turns a pre-escaped pipe "
            "back into a bare one and says so, declares nothing new unasked, leaves no partial record, and leaves "
            "the lane's files byte-identical", said + why)
        pin("not carried (only does not name them): %s" % C_ in said,
            "(s9) stage NAMES the lane's rows it did not carry -- an id left out of `only` is never dropped unsaid",
            said)
        # (s10) a stage that stops part-way leaves NO staged directory (and no partial one) for check or apply to
        # take as the whole batch; one whose record reads back other bytes is refused the same way (F1-A15).
        calls = []

        def failing_writer(path, data):
            calls.append(path)
            if len(calls) == 3:
                raise OSError("an injected failure writing %s" % os.path.basename(path))
            _write_bytes(path, data)

        def lying_writer(path, data):
            _write_bytes(path, data + (b" (not what was meant)" if path.endswith("trigger.txt") else b""))
        s10, s10b = os.path.join(top, "s10"), os.path.join(top, "s10b")
        crash, crash_err = None, ""
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                cmd_stage(tree, src, s10, [A_, B_], False, write=failing_writer)
        except OSError as exc:
            crash_err = str(exc)
        except Refused as exc:
            crash = str(exc)
        why = refused(cmd_stage, tree, src, s10b, [A_, B_], False, write=lying_writer) or ""
        pin("injected failure" in crash_err and crash is None and len(calls) == 3
            and not os.path.exists(s10) and not os.path.exists(s10 + ".partial")
            and "does not read back as written" in why and not os.path.exists(s10b)
            and not os.path.exists(s10b + ".partial"),
            "(s10) a stage that stops part-way, or whose record reads back other bytes, leaves no staged directory "
            "and no partial one -- the record appears whole or not at all", (crash_err, crash, why, len(calls)))
        why = refused(cmd_stage, tree, src, os.path.join(top, "s11"), [A_], False, [A_, C_]) or ""
        pin("new names %s, which only does not carry" % C_ in why and not os.path.exists(os.path.join(top, "s11")),
            "(s11) `new` naming a row `only` does not carry is refused -- a row is declared new where it is staged",
            why)
        marked = os.path.join(top, "marked-rows")
        rowdir(marked, N_, priority="P3", status="open", trigger="🟠 **OPEN** a lane's own marker")
        declare_new(marked, N_)
        why = refused(cmd_stage, tree, marked, os.path.join(top, "s12"), [N_], False, [N_]) or ""
        pin("%s is the staged NEW marker" % NEW_MARKER in why,
            "(s12) a lane's rows holding the NEW marker are refused -- the fold declares what is new, at stage", why)

        # (u1) `--repo` ON EITHER SIDE OF THE VERB: the same staging, the flag first and the flag last. ✔MEASURED
        # 2026-09-25 before the fix: `stage ... --repo X` was refused "unrecognized arguments: --repo X".
        rowdir(os.path.join(top, "u-rows"), N_, status="open", trigger="🟠 **OPEN** either side", priority="P3")
        rc1, said1, _w1 = ran(main, ["--repo", tree, "stage", os.path.join(top, "u-rows"), os.path.join(top, "u1"),
                                     "--only", N_])
        rc2, said2, _w2 = ran(main, ["stage", os.path.join(top, "u-rows"), os.path.join(top, "u2"), "--only", N_,
                                     "--repo", tree])
        rel = lambda d: dict((os.path.relpath(k, d), v) for k, v in md5s(d).items()) if os.path.isdir(d) else None
        pin(rc1 == 0 and rc2 == 0 and rel(os.path.join(top, "u1")) == rel(os.path.join(top, "u2"))
            and rel(os.path.join(top, "u1")),
            "(u1) --repo is taken on EITHER side of the verb -- the family's rule -- and stages the same rows",
            (rc1, rc2, said1[-200:], said2[-200:]))
        rc3, said3, _w3 = ran(main, ["stage", os.path.join(top, "u-rows"), os.path.join(top, "u3"), "--only", N_,
                                     "--repo"])
        rc4, said4, _w4 = ran(main, ["--repo", tree, "check", os.path.join(top, "u1"), "--repo", tree])
        pin(rc3 == USAGE and "--repo needs a directory" in said3 and rc4 == USAGE and "given twice" in said4,
            "(u1) ...and a --repo with no value, or given twice, is a usage refusal", (rc3, said3, rc4, said4))
        # (u2) `--only` GIVEN TWICE ADDS. ✔MEASURED 2026-09-25 (Python 3.14.3): argparse's default `store` keeps only
        # the LAST occurrence of an `nargs="+"` option, so `--only A --only B` staged B alone and dropped A unsaid.
        rc5, said5, why5 = ran(main, ["--repo", tree, "stage", src, os.path.join(top, "u5"), "--only", A_,
                                      "--only", B_])
        pin(rc5 == 0 and os.path.isdir(os.path.join(top, "u5"))
            and sorted(os.listdir(os.path.join(top, "u5"))) == sorted([A_, B_]),
            "(u2) --only given twice carries BOTH ids -- a repeated option keeping only its last value would drop "
            "the first one's rows with nothing said", (rc5, said5[-200:], why5,
                                                        os.path.isdir(os.path.join(top, "u5"))
                                                        and sorted(os.listdir(os.path.join(top, "u5")))))

        # ── (c) CHECK: the verdicts, the refusals, the rehearsal ──────────────────
        t = fixture_tree()
        before = reg(t)
        staged = os.path.join(top, "c-rows")
        rowdir(staged, A_, trigger="🟠 **OPEN** the alpha work\n  continued", closing="w  keeps  its spacing",
               crossrefs="r and more", status="🟠 OPEN")
        rowdir(staged, B_, trigger="🟠 **OPEN** a different story", closing="c", crossrefs="x | y", priority="P1")
        rowdir(staged, N_, priority="P2", status="closed", trigger="✅ **CLOSED** born closed", closing="done",
               crossrefs="none")
        declare_new(staged, N_)
        plans, probs = plan_rows(t, rows_of(staged, t))
        by = dict((p.anchor, p) for p in plans)
        pin(not probs and [p.anchor for p in plans] == [N_, A_, B_] and by[N_].kind == "new",
            "(c1) the plan orders NEW rows first, then updates, each by id", probs or [p.anchor for p in plans])
        pin(by[A_].verdicts == {"status": "SAME", "trigger": "KEPT", "closing": "SAME", "crossrefs": "KEPT"}
            and sorted(by[A_].fields) == ["cross_refs", "trigger"],
            "(c2) KEPT (an addendum, across a line break) and SAME (a double space kept) are told apart, and only "
            "the changed cells are sent", (by[A_].verdicts, by[A_].fields))
        pin(by[B_].verdicts.get("trigger") == "LOST" and by[B_].verdicts.get("priority") == "CHANGED"
            and by[B_].verdicts.get("crossrefs") == "SAME"
            and by[B_].diffs.get("trigger") == [("the beta work", "a different story")],
            "(c3) a LOST cell carries a word diff naming the words it removes and what replaces them; an escaped "
            "stored pipe reads SAME as a bare one", (by[B_].verdicts, by[B_].diffs))
        pin(cell_verdict("trigger", "a b", "a  b") == "RESPACED" and cell_verdict("closing", "x", "") == "FILLED"
            and cell_verdict("closing", "", "x") == "LOST",
            "(c4) RESPACED (the words agree, the spacing does not), FILLED (empty before), and a BLANKED cell is "
            "LOST")
        rowdir(os.path.join(top, "c5"), N_, trigger="🟠 **OPEN** no priority")
        declare_new(os.path.join(top, "c5"), N_)
        _p, probs = plan_rows(t, rows_of(os.path.join(top, "c5"), t))
        pin(any("is NEW" in x and "missing priority.txt, status.txt" in x for x in probs),
            "(c5) a NEW row missing its priority and status is refused, naming both", probs)
        rowdir(os.path.join(top, "c6"), A_, trigger="🟠 **OPEN** " + "D-" + "FIXTURE-\nROWS-ALPHA wrapped")
        _p, probs = plan_rows(t, rows_of(os.path.join(top, "c6"), t))
        pin(any("broken after a hyphen" in x for x in probs),
            "(c6) an anchor id wrapped across a line break is refused, as the door would store it", probs)
        rowdir(os.path.join(top, "c7"), A_, status="wibble")
        _p, probs = plan_rows(t, rows_of(os.path.join(top, "c7"), t))
        pin(any("CONTROLLED VOCABULARY" in x for x in probs),
            "(c7) a status outside the vocabulary is refused before any door", probs)
        rowdir(os.path.join(top, "c7b"), A_, closing="a " + ESCAPED_PIPE + " b")
        _p, probs = plan_rows(t, rows_of(os.path.join(top, "c7b"), t))
        pin(any("pre-escaped pipe" in x for x in probs),
            "(c7b) a hand-made staged row holding a pre-escaped pipe is refused (stage turns a lane's back)", probs)
        dup = fixture_tree()
        with io.open(os.path.join(dup, *cfg["doneAnchorsPath"].split("/")), "a", encoding="utf-8",
                     newline="") as fh:
            fh.write("| `%s` | P2 | ✅ CLOSED | ✅ **CLOSED** t | c | r |\n" % A_)
        rowdir(os.path.join(top, "c8"), A_, closing="x")
        _p, probs = plan_rows(dup, rows_of(os.path.join(top, "c8"), dup))
        pin(any("has 2 rows" in x and "settled by a human" in x for x in probs),
            "(c8) an id with a row in BOTH registries is refused, never settled by the batch", probs)
        rc, said, why = ran(cmd_check, t, staged)
        said += why
        pin(rc == 0 and reg(t) == before and "REHEARSAL" in said and "lint clean" in said
            and "`dssharness run %s --manual-step apply --input " % ROWS_RUNNER in said and "staged=" in said
            and "--input acceptLost=%s:trigger`" % B_ in said and "--accept-lost" not in said
            and "(0 closed, 0 opened); balance gate PASS" in said
            and "REMOVED: the beta work" in said and "WITH   : a different story" in said,
            "(c9) CONTROL: check rehearses the batch clean in a throwaway repository, prints each LOST cell's word "
            "diff, the batch's own balance with the gate's verdict (a born-closed row opens nothing) and the APPLY "
            "STEP it needs -- the harness form, its acceptLost naming the LOST cell, never the program's flag -- and "
            "leaves the real registries byte-identical", said[-700:])
        split = os.path.join(top, "c10")
        rowdir(split, A_, status="closed")
        why = refused(cmd_check, t, split) or ""
        pin("rehearsal failed" in why and reg(t) == before,
            "(c10) a batch the DOOR refuses (status CLOSED beside an OPEN Trigger, the verdict split) fails the "
            "rehearsal, and the real registries stay untouched", why)

        # (n) NEW IS DECLARED, NEVER INFERRED (the audit's F1-A4: `apply-registry-row` refuses a row no registry
        # holds unless `--insert` declares it; a batch writer that inferred NEW from a failed lookup let a full row
        # under a misspelt id land as a second row while the real one stayed open).
        typo = os.path.join(top, "n1")
        rowdir(typo, A_ + "X", priority="P2", status="closed", trigger="✅ **CLOSED** closed under a typo",
               closing="done", crossrefs="none")
        why = refused(cmd_check, t, typo) or ""
        pin("%sX is a row of neither registry, and the batch did not declare it NEW" % A_ in why
            and reg(t) == before,
            "(n1) a FULL row under a misspelt id, not declared new, is REFUSED -- it would mint a second row and leave "
            "the real one open", why)
        over = os.path.join(top, "n2")
        rowdir(over, A_, priority="P2", status="open", trigger="🟠 **OPEN** the alpha work again")
        declare_new(over, A_)
        why = refused(cmd_check, t, over) or ""
        pin("%s was declared NEW, but" % A_ in why and "already holds it" in why,
            "(n2) a row declared NEW that a registry already holds is REFUSED", why)
        n3rows, n3 = os.path.join(top, "n3-rows"), os.path.join(top, "n3")
        rowdir(n3rows, N_, priority="P3", status="open", trigger="🟠 **OPEN** declared at stage")
        rowdir(n3rows, A_, closing="w  keeps  its spacing and more")
        rc, said, why = ran(cmd_stage, t, n3rows, n3, [N_, A_], False, [N_])
        _p, probs = plan_rows(t, rows_of(n3, t)) if rc == 0 else ([], [why])
        pin(rc == 0 and os.path.isfile(os.path.join(n3, N_, NEW_MARKER))
            and not os.path.exists(os.path.join(n3, A_, NEW_MARKER)) and "declared NEW: %s" % N_ in said
            and not probs and [(p.anchor, p.kind) for p in _p] == [(N_, "new"), (A_, "update")],
            "(n3) CONTROL: `new` carries the declaration through stage into the staged record, and check plans that "
            "row NEW beside an undeclared update", (said[-300:], probs, [(p.anchor, p.kind) for p in _p]))

        # (k) A CITATION THE BATCH INTRODUCES MUST RESOLVE (the audit's F1-A3: the door checks no cited id, and no
        # guard reads the registries' own citations -- `check-anchor-citations` reads `anchors.citationRoots`).
        dangling = fx + "-NOSUCH"
        k1 = os.path.join(top, "k1")
        rowdir(k1, B_, crossrefs="x | y and [[%s]]" % dangling)
        why = refused(cmd_check, t, k1) or ""
        pin("%s/crossrefs cites %s, a row of neither registry nor this batch" % (B_, dangling) in why
            and reg(t) == before,
            "(k1) a cell citing an id that is a row of neither registry nor the batch is REFUSED, naming the row, the "
            "cell and the id", why)
        hist = fixture_tree()
        pend = os.path.join(hist, *cfg["pendingAnchorsPath"].split("/"))
        body = io.open(pend, encoding="utf-8", newline="").read().replace(
            "| c | x \\| y |", "| c | x \\| y, once [[%s]] |" % dangling)
        io.open(pend, "w", encoding="utf-8", newline="").write(body)
        k2 = os.path.join(top, "k2")
        rowdir(k2, N_, priority="P3", status="open",
               trigger="🟠 **OPEN** blocked by %s and %s-* rows" % (A_, fx))
        declare_new(k2, N_)
        rowdir(k2, B_, crossrefs="x | y, once [[%s]] -- and now [[%s]]" % (dangling, N_))
        _p, probs = plan_rows(hist, rows_of(k2, hist))
        pin(not probs and sorted(p.anchor for p in _p) == sorted([N_, B_]),
            "(k2) CONTROL: a registry row, a row of the SAME batch and a family mention resolve, and a dangling "
            "citation the stored cell ALREADY held is history, kept verbatim and not re-judged", probs)
        empty = os.path.join(top, "e1")
        os.makedirs(empty)
        why = refused(cmd_check, t, empty) or ""
        why2 = refused(cmd_apply, t, empty, []) or ""
        pin("holds no rows" in why and "holds no rows" in why2 and reg(t) == before,
            "(e1) an EMPTY staged directory is refused by check and apply -- a batch of nothing is never reported "
            "clean (the audit's F1-A6)", (why, why2))

        # (b) THE REHEARSAL'S BALANCE RECEIPT, JUDGED (the audit's F1-A7): its numbers AND the gate's verdict.
        def receipt(doc, rc=0):
            return lambda _tree: (rc, doc if isinstance(doc, str) else json.dumps(doc), "")
        clean = {"openAtBase": 2, "openNow": 2, "netNew": 0, "passed": True, "closed": [], "opened": [],
                 "missingAtBase": [], "findings": []}
        rc, said, why = ran(cmd_check, t, staged, receipt("not a receipt", 1))
        pin(rc == REFUSED and "UNREADABLE" in said and "rehearsal failed" in why,
            "(b1) an UNREADABLE balance receipt FAILS the rehearsal -- never \"rehearses clean\"", (said[-300:], why))
        rc, said, why = ran(cmd_check, t, staged, receipt(dict(clean, passed=False, findings=[{"what": "misfiled"}]),
                                                          1))
        pin(rc == REFUSED and "balance gate FAIL: 1 finding(s)" in said and "rehearsal failed" in why,
            "(b2) a receipt carrying a FINDING (a misfiled or malformed registry) fails the rehearsal", said[-300:])
        rc, said, why = ran(cmd_check, t, staged, receipt(dict(clean, passed=False, netNew=1, openNow=3,
                                                               opened=[{"anchor": N_}]), 1))
        pin(rc == 0 and "balance gate FAIL (net +1) -- judged at the round gate, not here" in said
            and "check OK" in said,
            "(b3) CONTROL: a net increase is SAID as the gate's FAIL and left to the round gate -- a batch may open "
            "disclosed or gated work -- so it does not fail the rehearsal", (said[-300:], why))

        # (w) KEPT IS WORD FOR WORD (the audit's F1-A10): an extended first or last word is a LOST cell.
        pin(cell_verdict("closing", "unfixed", "fixed") == "LOST"
            and cell_verdict("closing", "nonexistent thing", "none") == "LOST"
            and cell_verdict("crossrefs", "see %s-EXTRA" % A_, "see %s" % A_) == "LOST"
            and cell_verdict("closing", "fixed, and more", "fixed,") == "KEPT"
            and cell_verdict("closing", "first fixed", "fixed") == "KEPT",
            "(w1) KEPT is WORD for word: \"fixed\" -> \"unfixed\", \"none\" -> \"nonexistent\" and an id re-pointed by a "
            "suffix are LOST; an addendum after, or before, the stored words is KEPT (control)")

        # (g) THE ID GRAMMAR IS CONFIG'S (the audit's F1-A9): a tree declaring the prefix Q stages Q rows only.
        qt = fixture_tree(dict(cfg, idPrefix="Q"))
        qrows = os.path.join(top, "g-rows")
        rowdir(qrows, "Q-FIXTURE-ROWS-QUEBEC", priority="P3", status="open", trigger="🟠 **OPEN** under Q")
        rowdir(qrows, A_, priority="P3", status="open", trigger="🟠 **OPEN** a D id under Q")
        rc, said, why = ran(cmd_stage, qt, qrows, os.path.join(top, "g1"), ["Q-FIXTURE-ROWS-QUEBEC"], False)
        why2 = refused(cmd_stage, qt, qrows, os.path.join(top, "g2"), [A_], False) or ""
        rc3, said3, _w3 = ran(main, ["--repo", qt, "apply", os.path.join(top, "g1"),
                                     "--accept-lost", "Q-FIXTURE-ROWS-QUEBEC:trigger"])
        pin(rc == 0 and "%s is not an anchor id (Q-" % A_ in why2 and "is not an anchor id (D-" not in why2
            and "USAGE" not in said3,
            "(g1) a tree whose config.json declares the prefix Q stages a Q row, refuses a D one as no anchor id, and "
            "takes a Q acceptance -- the grammar is config's, never typed here", (said[-200:], why, why2, said3[-200:]))

        # (m) A REFUSAL OF THE FAMILY'S OTHER TYPES IS A REFUSAL (the audit's F1-A8): exit 2 and its words, never a
        # traceback -- here a tree whose config.json declares no id grammar.
        mt = fixture_tree(dict((k, v) for k, v in cfg.items() if k != "idPrefix"))
        rc, said, why = ran(main, ["--repo", mt, "check", staged])
        pin(rc == REFUSED and "anchor-rows: REFUSED --" in said and "idPrefix" in said and not why,
            "(m1) a sibling refusal (anchors': the tree declares no id grammar) reaches the step as REFUSED, exit 2 "
            "-- never a traceback", (rc, said[-300:], why))

        # ── (a) APPLY -- each refusal arm on a FRESH fixture, so a mutant that lets one arm write cannot
        # redden the next for the wrong reason (only the named reason may go red) ───────────────
        a1 = fixture_tree()
        why = refused(cmd_apply, a1, staged, []) or ""
        pin("1 LOST cell(s)" in why and "--manual-step apply" in why and "--input acceptLost=%s:trigger`" % B_ in why
            and reg(a1) == before,
            "(a1) a LOST cell nobody accepted is refused, naming the apply step's acceptLost input, and nothing is "
            "written", why)
        a2 = fixture_tree()
        why = refused(cmd_apply, a2, staged, ["%s:trigger" % B_, "%s:closing" % A_]) or ""
        pin("stale acceptance" in why and "%s:closing" % A_ in why and reg(a2) == before,
            "(a2) an --accept-lost naming a cell that is not LOST is refused as stale", why)
        a3 = fixture_tree()

        def failing_second(tree_, anchor, fields, new, apply_it):
            if tree_ == a3 and anchor == A_:
                return 13, "set-anchor: FAIL - an injected refusal"
            return A.door_write(tree_, anchor, fields, new, apply_it)
        why = refused(cmd_apply, a3, staged, ["%s:trigger" % B_], door=failing_second) or ""
        pin(reg(a3) == before and "an injected refusal" in why and "Written before it: %s" % N_ in why
            and "RESTORED byte for byte" in why,
            "(a3) a door refusal after an earlier row landed RESTORES both registries byte for byte and says what had "
            "been written", why)
        a4 = fixture_tree()
        moved_once = [False]

        def another_writer(tree_, anchor, fields, new, apply_it):
            if tree_ != a4 and not moved_once[0]:
                moved_once[0] = True
                with io.open(os.path.join(a4, *cfg["pendingAnchorsPath"].split("/")), "ab") as fh:
                    fh.write(b"\n")
            return A.door_write(tree_, anchor, fields, new, apply_it)
        why = refused(cmd_apply, a4, staged, ["%s:trigger" % B_], door=another_writer) or ""
        pin(moved_once[0] and "another writer" in why and not A.find(a4, N_),
            "(a4) registries that moved while the batch was planned and rehearsed are refused (another writer), and "
            "nothing of the batch is written", why)
        a0 = fixture_tree()
        why = refused(cmd_apply, a0, split, []) or ""
        pin("rehearsal failed -- nothing written to the registries" in why and reg(a0) == before,
            "(a0) apply refuses a batch its REHEARSAL fails, before the real registries are opened", why)
        pending = os.path.join(t, *cfg["pendingAnchorsPath"].split("/"))
        rc, said, why = ran(cmd_apply, t, staged, ["%s:trigger" % B_])
        said += why
        got_a = [ln for ln in io.open(pending, encoding="utf-8") if ("`%s`" % A_) in ln]
        got_n = A.find(t, N_)
        pin(rc == 0 and "APPLIED 3 row(s) (1 new, 2 updated)" in said and len(got_n) == 1
            and got_n[0].bucket == "done" and len(got_a) == 1 and "| w  keeps  its spacing |" in got_a[0]
            and "every id the batch newly cites resolved" in said and "check-anchor-citations" not in said,
            "(a5) CONTROL: apply writes the batch, the born-closed row lands in the archive, a cell SAME to the stored "
            "one is not sent and keeps its double space, and the closing line says what was resolved -- never "
            "naming a guard that reads no registry", said[-500:])
        rc, said, why = ran(cmd_apply, t, staged, [])
        pin(rc == 0 and "APPLIED 0 row(s)" in said and "3 already landed" in said,
            "(a6) a batch that already landed writes nothing and says so, so a fold can be re-run", said + why)

        # (x) ALL OR NOTHING, WHATEVER STOPS IT, AND NEVER OVER ANOTHER WRITER (the audit's F1-A5)
        x1 = fixture_tree()

        def raising_second(tree_, anchor, fields, new, apply_it):
            if tree_ == x1 and anchor == A_:
                raise OSError("an injected launch failure")
            return A.door_write(tree_, anchor, fields, new, apply_it)
        buf, raised = io.StringIO(), ""
        try:
            with contextlib.redirect_stdout(buf):
                cmd_apply(x1, staged, ["%s:trigger" % B_], door=raising_second)
        except OSError as exc:
            raised = str(exc)
        except Refused as exc:
            raised = "Refused: %s" % exc
        said = buf.getvalue()
        pin(raised == "an injected launch failure" and reg(x1) == before
            and "INTERRUPTED (OSError: an injected launch failure)" in said and "written before it: %s" % N_ in said
            and "RESTORED byte for byte" in said,
            "(x1) an EXCEPTION between two writes restores both registries byte for byte, says what had landed, and "
            "is raised again -- never a batch half-applied and unsaid", (raised, said[-400:]))
        x2 = fixture_tree()

        def writer_then_refusal(tree_, anchor, fields, new, apply_it):
            if tree_ == x2 and anchor == A_:
                path = os.path.join(x2, *cfg["pendingAnchorsPath"].split("/"))
                text = io.open(path, encoding="utf-8", newline="").read()
                io.open(path, "w", encoding="utf-8", newline="").write(
                    text.replace("# p\n", "# p, edited by another writer\n", 1))
                return 13, "set-anchor: FAIL - an injected refusal"
            return A.door_write(tree_, anchor, fields, new, apply_it)
        why = refused(cmd_apply, x2, staged, ["%s:trigger" % B_], door=writer_then_refusal) or ""
        pend_x2 = io.open(os.path.join(x2, *cfg["pendingAnchorsPath"].split("/")), encoding="utf-8").read()
        pin("NOT RESTORED" in why and "another writer" in why and "edited by another writer" in pend_x2
            and len(A.find(x2, N_)) == 1,
            "(x2) the restore is REFUSED, writing nothing, when the registries hold a change that is not this batch's "
            "-- another writer's work is never discarded; what the batch wrote is left, said, for a human", why)
        # (v1) THE READ-BACK OF MOVE-ON-CLOSE: a door (stand-in) that writes a born-closed row but leaves it in the
        # WORKING registry is refused by `verify` -- the one check that a closed row really left it (F1-A14).
        v1 = fixture_tree()

        def misfiling_door(tree_, anchor, fields, new, apply_it):
            if anchor == N_:
                with io.open(os.path.join(tree_, *cfg["pendingAnchorsPath"].split("/")), "a", encoding="utf-8",
                             newline="") as fh:
                    fh.write("| `%s` | P2 | ✅ CLOSED | ✅ **CLOSED** born closed | done | none |\n" % N_)
                return 0, "(a stand-in that left the closed row in the working registry)"
            return A.door_write(tree_, anchor, fields, new, apply_it)
        v_plans = [p_ for p_ in plan_rows(v1, rows_of(staged, v1))[0] if p_.anchor == N_]
        _w, failure = write_in_order(v1, v_plans, cfg, door=misfiling_door)
        pin(len(v_plans) == 1 and failure is not None and failure[0] == N_
            and "it lives in %s, not %s" % (cfg["pendingAnchorsPath"], cfg["doneAnchorsPath"]) in failure[1],
            "(v1) a CLOSED row the door left in the WORKING registry is refused by the read-back -- move-on-close is "
            "checked, never assumed", failure)
        l1 = fixture_tree()
        why = refused(cmd_apply, l1, staged, ["%s:trigger" % B_],
                      lint=lambda _root: (1, "", "anchors: 1 finding(s)")) or ""
        pin("do not lint after the batch" in why and "RESTORED byte for byte" in why and reg(l1) == before,
            "(l1) registries that do not lint after the batch are RESTORED byte for byte, and the refusal says so", why)

        # ── (p) PARITY: door_form is what the real door stores ──────────────────────
        p = fixture_tree()
        shapes = ["🟠 **OPEN** a  b\tc \n   d\n\n\ne \r\n f  \n", "🟠 **OPEN** x \ny | z", "  🟠 **OPEN** lead\n"]
        agree = []
        for i, shape in enumerate(shapes):
            anchor = fx + "-PARITY%d" % i
            rc, _out = A.door_write(p, anchor, {"priority": "P3", "status": "open", "trigger": shape}, True, True)
            got, _why = read_back(p, anchor)
            agree.append((rc, got["trigger"] if got else None, A.door_form(shape)))
        pin(all(rc == 0 and have == want for rc, have, want in agree),
            "(p1) door_form is what the REAL door stores: breaks with their whitespace, blank lines, CRLF, a NBSP "
            "beside a break, runs and tabs kept, ends trimmed", agree)

        # ── (h) THE HARNESS STEPS: anchor-rows.yml held to STEP_INPUTS, and `--step` read strictly ─────────
        ysteps = yml_steps(os.path.join(HERE, "anchor-rows.yml"))
        manual = collections.OrderedDict((s, v) for s, v in ysteps.items() if v["manual"])
        problems = []
        if list(manual) != list(STEP_INPUTS):
            problems.append("manual steps: the yml declares %s, the table %s" % (list(manual), list(STEP_INPUTS)))
        for verb, v in manual.items():
            want = [n for n, _k in STEP_INPUTS.get(verb, ())]
            if v["inputs"] != want:
                problems.append("%s: the yml's inputs %s, the table's %s" % (verb, v["inputs"], want))
            expect = "python3 ./anchor-rows.py --step %s%s" % (verb, "".join(' "%s={%s}"' % (n, n) for n in want))
            if v["run"] != expect:
                problems.append("%s: the run line %r, expected %r" % (verb, v["run"], expect))
            if v["pattern"] != STEP_PATTERN % verb:
                problems.append("%s: the successPattern %r, expected %r" % (verb, v["pattern"], STEP_PATTERN % verb))
        pin(not problems and [s for s, v in ysteps.items() if not v["manual"]] == ["self-test"],
            "(h1) anchor-rows.yml's manual steps, their inputs, run lines and success patterns are EXACTLY "
            "STEP_INPUTS -- one statement of each step's grammar -- and the self-test stays the one step a plain run "
            "runs", "; ".join(problems) or "steps=%s" % list(ysteps))
        here = tempfile.mkdtemp(prefix="anchor-rows-steps-")
        boxes.append(here)
        write(os.path.join(here, "lists", "only.txt"), "# the fold's ids\n\n%s\n  %s  \n" % (A_, B_))
        got = step_argv(here, "stage", ["rows=.temp/lane rows", "staged=%s" % os.path.join(here, "abs"),
                                        "only=%s,%s" % (A_, B_), "new=%s" % B_, "liveOnly=true"])
        pin(got == ["stage", os.path.join(here, ".temp", "lane rows"), os.path.join(here, "abs"),
                    "--only", A_, "--only", B_, "--new", B_, "--live-only"],
            "(h2) --step turns every input into the verb's own argument: a relative path under the TREE (spaces "
            "kept), an absolute one as given, a comma list once per item, a true flag as its option", "got=%r" % got)
        got2 = step_argv(here, "stage", ["rows=r", "staged=s", "only=%s" % A_, "new=", "liveOnly=false"])
        got3 = step_argv(here, "apply", ["staged=s", "acceptLost="])
        pin(got2 == ["stage", os.path.join(here, "r"), os.path.join(here, "s"), "--only", A_]
            and got3 == ["apply", os.path.join(here, "s")],
            "(h2) ...and a false flag, or an empty list, gives NOTHING", "got=%r / %r" % (got2, got3))
        got = step_argv(here, "stage", ["rows=r", "staged=s", "only=@lists/only.txt", "new=", "liveOnly=false"])
        pin(got == ["stage", os.path.join(here, "r"), os.path.join(here, "s"), "--only", A_, "--only", B_],
            "(h3) an `@<file>` list reads one item a line, blank lines and `#` comments skipped, each item trimmed",
            "got=%r" % got)

        def step_refused(verb, tokens):
            try:
                step_argv(here, verb, tokens)
                return None
            except StepRefused as exc:
                return str(exc)
        for label, verb, tokens, needle in (
                ("(h4) a step the table lacks is REFUSED", "land", [], "declares no such step"),
                ("(h4) a token that is not <input>=<value> is REFUSED", "check", ["justaword"],
                 "is not <input>=<value>"),
                ("(h4) an input the step does not take is REFUSED", "check", ["staged=s", "bogus=1"],
                 "no input 'bogus'"),
                ("(h4) an input given twice is REFUSED", "check", ["staged=s", "staged=t"], "given twice"),
                ("(h4) an input NOT handed over is REFUSED -- the yml passes every one", "apply", ["staged=s"],
                 "not handed over"),
                ("(h4) an EMPTY directory is REFUSED", "check", ["staged="], "staged is empty"),
                ("(h4) an `only` naming no id is REFUSED -- stage carries exactly what it names", "stage",
                 ["rows=r", "staged=s", "only=", "new=", "liveOnly=false"], "only names no item"),
                ("(h4) a flag that is neither true nor false is REFUSED", "stage",
                 ["rows=r", "staged=s", "only=%s" % A_, "new=", "liveOnly=yes"], "a flag input is true or false"),
                ("(h4) an EMPTY list item (a stray comma) is REFUSED", "apply",
                 ["staged=s", "acceptLost=%s:trigger,," % A_], "EMPTY item"),
                ("(h4) an `@<file>` naming no file is REFUSED", "stage",
                 ["rows=r", "staged=s", "only=@nope.txt", "new=", "liveOnly=false"], "names no list file")):
            why = step_refused(verb, tokens) or ""
            pin(needle in why, label, why or "not refused")
        h5rows, h5staged = os.path.join(here, "h5-rows"), os.path.join(here, "h5-staged")
        rowdir(h5rows, N_, priority="P3", status="open", trigger="🟠 **OPEN** through the step")
        rowdir(h5rows, C_, priority="P3", status="open", trigger="🟠 **OPEN** and its sibling")
        rc, said, why = ran(main, ["--step", "stage", "rows=%s" % h5rows, "staged=%s" % h5staged,
                                   "only=%s,%s" % (N_, C_), "new=%s,%s" % (N_, C_), "liveOnly=false"])
        pin(rc == 0 and said.rstrip().splitlines()[-1:] == [STEP_DONE % "stage"]
            and os.path.isdir(h5staged) and sorted(os.listdir(h5staged)) == sorted([N_, C_])
            and all(os.path.isfile(os.path.join(h5staged, x, NEW_MARKER)) for x in (N_, C_)),
            "(h5) a step whose verb succeeds carries EVERY item of its list to the verb, and ends with its closing "
            "line, which its successPattern reads", "rc=%r out=%s %s staged=%s"
            % (rc, said.strip()[-300:], why, sorted(os.listdir(h5staged)) if os.path.isdir(h5staged) else None))
        rc, said, why = ran(main, ["--step", "check", "staged=%s" % os.path.join(here, "no-such-staged")])
        rc2, said2, why2 = ran(main, ["--step", "check", "bogus=1"])
        pin(rc == REFUSED and "no rows directory" in said and (STEP_DONE % "check") not in said
            and rc2 == USAGE and "no input 'bogus'" in said2 and (STEP_DONE % "check") not in said2,
            "(h5) ...and a step whose verb refuses (exit 2), or whose inputs are refused (exit 3), ends WITHOUT it",
            "rc=%r out=%s | rc=%r out=%s" % (rc, (said + why).strip()[-200:], rc2, (said2 + why2).strip()[-200:]))
        whole = ot().load_jsonc(os.path.join(repo_root(), ".harness-config", "config.json"))
        whole = whole if isinstance(whole, dict) else {}
        runner = (whole.get("predefinedRunners") or {}).get(ROWS_RUNNER)
        legs = (runner.get("legs") or []) if isinstance(runner, dict) else []
        leg = ((whole.get("legs") or {}).get(legs[0]) if len(legs) == 1 else None) or {}
        other_host = [k for k in (whole.get("hosts") or {}) if k != "local" and k in leg]
        pin(isinstance(runner, dict) and runner.get("action") == "anchor-rows/anchor-rows.yml" and len(legs) == 1
            and bool(leg) and not other_host,
            "(h6) config.json declares the `%s` runner every remedy names: anchor-rows.yml on ONE leg, and that "
            "leg runs on this machine's own tree (its definition names no other host) -- never a host's synced "
            "copy" % ROWS_RUNNER, "runner=%r leg=%r other host=%r" % (runner, leg, other_host))

        # ── (r) THE ROOT IS THE TREE THIS FILE LIVES IN ──────────────────────────────
        with contextlib.redirect_stderr(io.StringIO()):
            for n, (ok, label, detail) in enumerate(
                    ot().root_arms(repo_root, (Refused, SystemExit), True, __file__), start=1):
                pin(ok, "(r%d) %s" % (n, label), detail)
    finally:
        for b in boxes:
            ot().remove_tree(b)
    print("anchor-rows self-test: %d arm(s), %d failed" % (count[0], failed[0]))
    return 1 if failed[0] else 0


# ──────────────────────────────── the harness steps ────────────────────────────────
#
# ★ EVERY VERB IS A MANUAL STEP OF anchor-rows.yml, run through DssHarness on the `rows` runner
# (`dssharness run rows --manual-step <verb> --input <name>=<value> ...`), never this program started by hand: the
# operator's rule of 2026-09-24 routes the repository's operations through the harness, and work an action owns but
# a plain run must not do is a manual step of that action. A step's run line hands EVERY input over as
# `<input>=<value>`, defaults included (✔MEASURED 2026-09-25 on DssHarness 0.5.12: an input spliced inside a token
# fills in, an empty default arrives as `<input>=`, and a value with spaces or commas arrives as ONE token), and
# `--step` turns them into the verb's own arguments. THIS TABLE is the one statement of each step's inputs and how
# each is read; arm (h1) holds anchor-rows.yml to it, so a step or an input spelled differently on either side reds
# the self-test rather than a fold. Kinds: `dir` a path the verb takes as an argument, never empty; `ids` a list
# naming at least one item; `list` a list that may be empty; `flag` true or false. A path is relative to the TREE
# unless absolute (a step runs in the action's own directory, which no caller means). A list is comma-separated
# items, or `@<file>` naming a list file, one item a line (blank lines and `#` comments skipped), for a list too
# long to type. An input becomes its verb's option by its name in kebab case (`liveOnly` -> `--live-only`), given
# once per item of a list.

STEP_INPUTS = collections.OrderedDict([
    ("stage", (("rows", "dir"), ("staged", "dir"), ("only", "ids"), ("new", "list"), ("liveOnly", "flag"))),
    ("check", (("staged", "dir"),)),
    ("apply", (("staged", "dir"), ("acceptLost", "list"))),
])
# What a step prints last when its verb returned 0, and the successPattern anchor-rows.yml gives it (arm (h1)).
STEP_DONE = "anchor-rows step %s: done"
STEP_PATTERN = "^anchor-rows step %s: done\\b"
# The runner config.json declares for these steps (arm (h6) holds config.json to it): every remedy names it.
ROWS_RUNNER = "rows"


class StepRefused(Exception):
    """A step's inputs that do not make its verb's arguments: printed, exit 3."""


def step_form(verb, *inputs):
    """The harness command that runs `verb` as a manual step of the `rows` runner -- how every remedy names a verb,
    never as this program started by hand. `inputs` are (name, value) pairs; an empty value is left out, and one
    holding a space is quoted."""
    parts = []
    for name, value in inputs:
        if value:
            tok = "%s=%s" % (name, value)
            parts.append(' --input "%s"' % tok if " " in tok else " --input %s" % tok)
    return "`dssharness run %s --manual-step %s%s`" % (ROWS_RUNNER, verb, "".join(parts))


def _as_input(root, path):
    """A path as a step input names it: relative to the tree when it lies inside it, else as it is."""
    try:
        rel = os.path.relpath(path, root)
    except ValueError:   # another drive
        return path
    if rel == os.curdir or rel == os.pardir or rel.startswith(os.pardir + os.sep) or os.path.isabs(rel):
        return path
    return rel.replace(os.sep, "/")


def _kebab(name):
    return "--" + re.sub(r"([A-Z])", lambda m: "-" + m.group(1).lower(), name)


def _tree_path(root, value):
    """A path a step input names: absolute as given, else relative to the TREE."""
    return value if os.path.isabs(value) else os.path.join(root, *value.replace("\\", "/").split("/"))


def _step_items(root, verb, name, value):
    """-> the items of a list input: none for an empty value, the lines of `@<file>` (blank lines and `#` comments
    skipped), else the comma-separated items -- an empty item refused."""
    if not value:
        return []
    if value.startswith("@"):
        path = _tree_path(root, value[1:])
        if not os.path.isfile(path):
            raise StepRefused("--step %s: %s=%s names no list file (%s)" % (verb, name, value, path))
        with io.open(path, encoding="utf-8") as fh:
            return [ln.strip() for ln in fh if ln.strip() and not ln.lstrip().startswith("#")]
    items = [v.strip() for v in value.split(",")]
    if not all(items):
        raise StepRefused("--step %s: %s=%r holds an EMPTY item -- a stray comma; name every item, or use @<file>"
                          % (verb, name, value))
    return items


def step_argv(root, verb, tokens):
    """-> the argv of `verb` for a harness step's `<input>=<value>` tokens (STEP_INPUTS). Refused, naming it: a step
    the table lacks, a token that is not `<input>=<value>`, an input the step does not take, one given twice, one
    not handed over, an empty directory, a list that must name an item and names none, a flag that is neither true
    nor false, an empty list item, an `@<file>` naming no file."""
    if verb not in STEP_INPUTS:
        raise StepRefused("--step %r: anchor-rows.yml declares no such step (the steps: %s)"
                          % (verb, ", ".join(STEP_INPUTS)))
    spec = STEP_INPUTS[verb]
    kinds, values = dict(spec), {}
    for tok in tokens:
        name, eq, value = tok.partition("=")
        if not eq or not name:
            raise StepRefused("--step %s: %r is not <input>=<value>" % (verb, tok))
        if name not in kinds:
            raise StepRefused("--step %s: no input %r -- its inputs are %s" % (verb, name, ", ".join(kinds)))
        if name in values:
            raise StepRefused("--step %s: %s is given twice" % (verb, name))
        values[name] = value
    missing = [n for n, _k in spec if n not in values]
    if missing:
        raise StepRefused("--step %s: input(s) %s not handed over -- the step's run line passes every input, "
                          "defaults included" % (verb, ", ".join(missing)))
    positional, options = [verb], []
    for name, kind in spec:
        value = values[name]
        if kind == "dir":
            if not value:
                raise StepRefused("--step %s: %s is empty -- it names a directory" % (verb, name))
            positional.append(_tree_path(root, value))
        elif kind == "flag":
            if value not in ("true", "false"):
                raise StepRefused("--step %s: %s=%r -- a flag input is true or false" % (verb, name, value))
            if value == "true":
                options.append(_kebab(name))
        else:
            items = _step_items(root, verb, name, value)
            if kind == "ids" and not items:
                raise StepRefused("--step %s: %s names no item -- the step carries exactly what it names"
                                  % (verb, name))
            for item in items:
                options += [_kebab(name), item]
    return positional + options


def run_step(verb, tokens):
    """`anchor-rows.py --step <verb> <input>=<value>...`: the verb, run on the tree this file lives in, and -- only
    when it returns 0 -- the step's closing line, which its successPattern reads."""
    try:
        root = repo_root(None)
        argv = step_argv(root, verb, tokens)
    except StepRefused as exc:
        print("anchor-rows: USAGE -- %s" % exc)
        return USAGE
    except refusal_types() as exc:
        print("anchor-rows: REFUSED -- %s" % exc)
        return REFUSED
    shown = [_as_input(root, a) if os.path.isabs(a) else a for a in argv]
    print("anchor-rows step %s: anchor-rows.py %s" % (verb, " ".join('"%s"' % a if " " in a else a for a in shown)))
    rc = main(argv)
    if rc == 0:
        print(STEP_DONE % verb)
    return rc


def yml_steps(path):
    """-> {step: {manual, inputs, run, pattern}} of anchor-rows.yml: a reader of THIS file's own layout (a step at
    two spaces, its keys at four, its inputs at six, a one-line run block, comments skipped), enough for arm (h1) to
    hold the file to STEP_INPUTS. DssHarness reads the yml itself; nothing else here does."""
    steps, cur, where = collections.OrderedDict(), None, None
    with io.open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\r\n")
            if line.lstrip().startswith("#"):
                continue
            m = re.match(r"^  - name: (\S+)$", line)
            if m:
                cur, where = m.group(1), None
                steps[cur] = {"manual": False, "inputs": [], "run": "", "pattern": None}
                continue
            if cur is None:
                continue
            if line == "    manual: true":
                steps[cur]["manual"] = True
            m = re.match(r"^    successPattern: '(.*)'$", line)
            if m:
                steps[cur]["pattern"] = m.group(1)
            if re.match(r"^    \S", line):
                where = {"    inputs:": "inputs", "    run: |": "run"}.get(line)
                continue
            if where == "inputs":
                m = re.match(r"^      (\w+):$", line)
                if m:
                    steps[cur]["inputs"].append(m.group(1))
            elif where == "run" and line.strip():
                steps[cur]["run"] += line.strip()
    return steps


# ──────────────────────────────── main ────────────────────────────────

def _take_repo(argv):
    """-> (the `--repo` value or None, the arguments without it), taken from ANYWHERE in `argv`, before the verb is
    read; a `--repo` with no value, or given twice, is a usage refusal."""
    override, kept, i = None, [], 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--repo" or arg.startswith("--repo="):
            value = arg[len("--repo="):] if arg.startswith("--repo=") else (argv[i + 1] if i + 1 < len(argv) else "")
            if not value:
                raise SystemExit("--repo needs a directory")
            if override is not None:
                raise SystemExit("--repo is given twice")
            override = value
            i += 1 if arg.startswith("--repo=") else 2
            continue
        kept.append(arg)
        i += 1
    return override, kept


def main(argv):
    if argv[:1] in (["--self-test"], ["--selftest"]) and len(argv) == 1:
        return self_test()
    if argv[:1] == ["--step"]:
        if len(argv) < 2:
            print("anchor-rows: USAGE -- --step needs a step name (the steps: %s)" % ", ".join(STEP_INPUTS))
            return USAGE
        return run_step(argv[1], list(argv[2:]))
    try:
        override, argv = _take_repo(list(argv))
    except SystemExit as exc:
        print("anchor-rows: USAGE -- %s" % exc)
        return USAGE
    import argparse

    class _Parser(argparse.ArgumentParser):
        def error(self, message):
            print("anchor-rows: USAGE -- %s" % message)
            raise SystemExit(USAGE)
    ap = _Parser(prog="anchor-rows.py", add_help=True)
    sub = ap.add_subparsers(dest="verb", required=True, parser_class=_Parser)
    s = sub.add_parser("stage")
    s.add_argument("src")
    s.add_argument("dst")
    # `extend`, never argparse's default `store`: a second `--only` must ADD its ids. With `store` it REPLACED the
    # first one's, and `--only A --only B` staged B alone -- a row dropped with nothing said (arm (u2)).
    s.add_argument("--only", nargs="+", required=True, action="extend")
    # the ids the batch may CREATE (the audit's F1-A4): `extend`, like `--only`, so a second `--new` adds
    s.add_argument("--new", nargs="+", action="extend", default=[])
    s.add_argument("--live-only", action="store_true")
    c = sub.add_parser("check")
    c.add_argument("staged")
    a = sub.add_parser("apply")
    a.add_argument("staged")
    a.add_argument("--accept-lost", action="append", default=[])
    args = ap.parse_args(argv)
    try:
        root = repo_root(override)
        if args.verb == "stage":
            return cmd_stage(root, os.path.abspath(args.src), os.path.abspath(args.dst), args.only, args.live_only,
                             args.new)
        if args.verb == "check":
            return cmd_check(root, os.path.abspath(args.staged))
        shape = re.compile(r"^(?:%s):(?:%s)$" % (anchors().id_pattern(anchors().id_grammar(root)), "|".join(CELLS)))
        bad = [x for x in args.accept_lost if not shape.match(x)]
        if bad:
            print("anchor-rows: USAGE -- --accept-lost takes <ANCHOR ID>:<cell>, the cell one of %s: %s"
                  % (", ".join(CELLS), ", ".join(bad)))
            return USAGE
        return cmd_apply(root, os.path.abspath(args.staged), args.accept_lost)
    except refusal_types() as exc:
        print("anchor-rows: REFUSED -- %s" % exc)
        return REFUSED


def refusal_types():
    """Every refusal this program's verbs can meet, printed as a refusal (exit 2) and never as a traceback: its own,
    the anchors family's (`anchors.Refused`: no DssHarness, a tree whose id grammar is missing), owning-tree's (a
    config.json that does not parse) and an OSError (a registry or a directory that cannot be read) -- the audit's
    F1-A8. An `apply` interrupted by one has restored the registries, and said so, before it gets here."""
    return (Refused, anchors().Refused, ot().Refusal, OSError)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
