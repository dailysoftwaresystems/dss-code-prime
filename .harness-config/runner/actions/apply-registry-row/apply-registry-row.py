#!/usr/bin/env python3
# PURPOSE: replace one deferred-anchor registry row with a lane's verbatim row text from a file.
"""apply-registry-row.py -- put ONE lane's row into the registry, from the lane's own row file.

★★★ THE ROW IS NEVER RETYPED, AND THAT IS THE WHOLE REASON THIS EXISTS. A retyped row
can WRAP AN ANCHOR ID, and a wrapped id **does not fail**: it goes INVISIBLE to every
grep, to `check-anchor-registry`, and to `check-anchor-balance` -- and it MINTS a false
id at the same time. In a fail-loud project that is the one defect class that cannot be
caught by watching for a failure. ✔MEASURED 2026-08-20: of the 78 distinct `D-*` ids
cited on one cycle's added lines, **17 were wrapped**, and 16 were harmless only
because the same id appeared unwrapped nearby.
⇒ The lane writes its row to a FILE as one physical line; this reads its cells from it.

★★ AND A DUPLICATE IS REFUSED, NEVER RESOLVED. Parallel lanes give one anchor TWO
renditions -- typically an OPENER from one lane and a CLOSER from another. Settling
that pair by POSITION (or by sort order) wrote the OPEN rendition back over a fixed
defect **three times in cycle P42**. The door refuses and names the count; a human
reads both and decides.

WHAT IS VALIDATED BEFORE ANYTHING IS WRITTEN -- each clause is a way a bad row lands
looking fine:
  1. the row file is UTF-8 and holds EXACTLY ONE physical line (a wrapped row is the
     defect above);
  2. it splits into EXACTLY 6 content cells on UNESCAPED pipes, between a leading and a
     trailing pipe -- a raw pipe inside a cell must be backslash-escaped or the registry
     table silently gains a column;
  3. the first cell is a BACKTICKED anchor id equal to the one named on the command
     line -- so a row cannot be applied to the wrong anchor by a slip in either place;
  4. no registry holds more than ONE row for that anchor (the door refuses it);
  5. the destination is inside the repository, compared by RESOLVED PATH PREFIX.

⚠ SIX CELLS SINCE 2026-09-01, not four -- `| Anchor | Priority | Status | Trigger |
Closing work | Cross-refs |` -- so the verdict is a cell of its own rather than the first
glyph of the trigger prose. A row written to the old four-cell shape would land with its
TRIGGER sitting in the STATUS column, which is why the count is a refusal and not a warning.

★★★ IT DOES NOT PLACE THE ROW ITSELF: THE DOOR DOES. A row is written by `dssharness
write-anchor` (new, with `--insert`) or `set-anchor` (existing), through the one launcher
both of this tree's row callers share, `anchors.door_write` -- which also performs the
move-on-close: a row whose status is closed is DELETED from the working registry and
appended to the archive, and a reopened one moves back. This file keeps what is uniquely
its own: the validation of a lane's one-line row file. Its cells are handed to the door
UN-escaped (the door escapes every pipe again, so an escaped pipe round-trips to the same
bytes), and an update names only the fields that CHANGE, so a stored cell the row file did
not change keeps its bytes.

The door PRINTS the status transition and any MOVE, so the caller sees whether this
application actually closed anything. ⓘ A row is CLOSED iff its STATUS cell begins with
the closure mark after stripping `*_ ` -- the complement is defined, never enumerated,
exactly as `check-anchor-balance` defines it, and this file does not define it a second
time.

Never stages, never runs a git write verb.

ⓘ NO `.ps1` TWIN, DELIBERATELY. This is a `.py`, which runs unchanged on the Windows leg
and on every POSIX leg, so a PowerShell sibling would be a SECOND IMPLEMENTATION of
something that was never split. The omission is stated rather than merely taken, because
a gate cannot tell a deliberate portable-only script from a forgotten twin.
Exit codes: 0 OK (or a clean dry run) · 2 refused (nothing written) · 3 usage error.

Usage:
    python .harness-config/runner/actions/apply-registry-row/apply-registry-row.py <registry-rel> <anchor> <row-file>
    python .harness-config/runner/actions/apply-registry-row/apply-registry-row.py ... --apply     # write
    python .harness-config/runner/actions/apply-registry-row/apply-registry-row.py ... --insert    # NEW row
    python .harness-config/runner/actions/apply-registry-row/apply-registry-row.py --self-test
"""
from __future__ import annotations

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
    except (AttributeError, ValueError):  # pragma: no cover
        pass

# A cell separator is a pipe that is NOT backslash-escaped. The lookbehind is the whole
# grammar: an escaped pipe inside a cell is content, a bare one between cells is
# structure.
SPLIT = re.compile(r"(?<!\\)\|")
CELLS = 6


class Refused(Exception):
    pass


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
            raise Refused("cannot find %s -- this tool's root is resolved there and nowhere "
                          "else." % path)
        spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _OWNING_TREE = mod
    return _OWNING_TREE


def repo_root():
    """The tree THIS FILE lives in -- never the tree the caller's shell is standing in.

    ⚠ A predecessor pinned one absolute Windows path; its replacement, a bare
    `git rev-parse --show-toplevel`, answered from the CALLER's cwd -- and this tool
    WRITES under the root. ✔MEASURED 2026-09-15 (P66): run by path with its cwd inside a
    different repository, a dry run ACCEPTED a row that exists only in THAT repository's
    registry, so `--apply` would have written there. The root is now answered by the same
    walk the launcher's own file uses, owned by `.harness-config/runner/actions/owning-tree/owning-tree.py`.
    """
    ot = _owning_tree()
    try:
        return ot.resolve(__file__)
    except ot.Refusal as exc:
        raise Refused(str(exc))


# The launcher, resolved from THIS FILE rather than from the tree being edited.
# ⚠ THE DISTINCTION IS LOAD-BEARING: `apply_row` takes a repository ROOT (which the
# self-test points at a temporary fixture), while the launcher is a SIBLING PROGRAM that
# always lives beside this one. Resolving it from the root would mean any tree without a
# `.harness-config/runner/actions/anchors/` had no launcher -- including every fixture.
ANCHORS_PY = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "anchors", "anchors.py")


def _anchors():
    """The registry reader and the door's one launcher. Imported by path -- a hyphen is not a
    module name.

    ⚠ FAILS LOUD rather than falling back to a local copy of the placement rule. A
    fallback here would be a second, quieter answer to "where does a closed row go",
    which is precisely what the one door is for.
    """
    path = ANCHORS_PY
    if not os.path.isfile(path):
        raise Refused("cannot find .harness-config/runner/actions/anchors/anchors.py -- this tool "
                      "VALIDATES a row file and hands its cells to the door through that file's "
                      "launcher; it does not carry a second copy of the move-on-close rule.")
    spec = importlib.util.spec_from_file_location("dss_anchors", path)
    mod = importlib.util.module_from_spec(spec)
    held, sys.argv = sys.argv, [path]
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.argv = held
    return mod


def read_row(rowfile, anchor):
    """-> the validated row text and its cells. Raises Refused with the reason."""
    if not os.path.isfile(rowfile):
        raise Refused("no row file at %s" % rowfile)
    try:
        raw = io.open(rowfile, encoding="utf-8", errors="strict", newline="").read()
    except UnicodeDecodeError as exc:
        raise Refused("the row file %s is not UTF-8 (%s)" % (rowfile, exc))
    row = raw.strip("\r\n")
    if "\n" in row or "\r" in row:
        raise Refused("the row file holds more than one physical line. A registry row "
                      "is ONE line -- a wrapped anchor id goes invisible to every grep "
                      "instead of failing.")
    cells = SPLIT.split(row)
    if len(cells) != CELLS + 2 or cells[0].strip() or cells[-1].strip():
        raise Refused(
            "%d part(s) after splitting on UNESCAPED pipes; a row must have exactly %d "
            "content cells between a leading and a trailing pipe -- `| Anchor | Priority "
            "| Status | Trigger | Closing work | Cross-refs |`. A literal pipe inside a "
            "cell must be backslash-escaped. (The registry grew the Priority and Status "
            "columns on 2026-09-01; a four-cell row would land with its trigger sitting "
            "in the status column.)" % (len(cells), CELLS))
    first = cells[1].strip()
    if first != "`%s`" % anchor:
        raise Refused("first cell is %r, expected a backticked %s" % (first, anchor))
    return row, cells


def row_fields(an, cells):
    """The row file's cells -> the door's fields: priority, status, and the three prose cells
    UN-escaped (the door escapes every pipe again, so an escaped pipe round-trips)."""
    def prose(cell):
        return cell.replace("\\|", "|").strip()
    return {"priority": cells[an.C_PRIORITY].strip(), "status": cells[an.C_STATUS].strip(),
            "trigger": prose(cells[an.C_TRIGGER]), "closing": prose(cells[an.C_CLOSING]),
            "cross_refs": prose(cells[an.C_XREF])}


def changed_fields(an, fields, stored):
    """Only the fields that differ from the stored row -- a cell the door is not asked to
    write keeps its bytes. Prose is compared with its whitespace runs collapsed, the way a
    re-read compares it, so a cell that differs only in spacing is left exactly as stored."""
    flat = lambda s: " ".join(str(s).split())
    out = {}
    if an.normalise_status(fields["status"]) != stored.status:
        out["status"] = fields["status"]
    if fields["priority"].upper() != stored.priority:
        out["priority"] = fields["priority"]
    for key, column in (("trigger", an.C_TRIGGER), ("closing", an.C_CLOSING),
                        ("cross_refs", an.C_XREF)):
        if flat(fields[key]) != flat(stored.cell(column)):
            out[key] = fields[key]
    return out


def apply_row(root, rel, anchor, rowfile, write, insert=False):
    dest_decl = os.path.realpath(os.path.join(root, rel))
    if not (dest_decl + os.sep).startswith(root + os.sep):
        raise Refused("destination escapes the repository: %s" % rel)
    if not os.path.isfile(dest_decl):
        raise Refused("no registry at %s" % rel)

    an = _anchors()
    _row, cells = read_row(rowfile, anchor)

    # ⚠ THE ARGUMENT NAMES THE BUCKET, NOT THE FILE THE ROW ENDS UP IN. A caller says
    # "this is a production row"; where it LANDS follows from its status -- the door
    # derives the archive. Resolved-path comparison, not a string match on `rel`: the same
    # registry can be named `.plans/x.md` or `./.plans/x.md` and only one would match by text.
    bucket = next((b for b, r in an.REL.items()
                   if os.path.realpath(os.path.join(root, r)) == dest_decl), None)
    if bucket is None:
        raise Refused("%s is not one of the anchor registries (%s)."
                      % (rel, ", ".join(sorted(an.REL.values()))))
    if bucket not in an.WORKING:
        raise Refused(
            "the archive is not a destination a caller declares. Name the WORKING "
            "registry the row belongs to (%s); a row whose status is closed is routed "
            "into the archive from there, and a reopened one is moved back out."
            % ", ".join(an.WORKING))

    try:
        fields = row_fields(an, cells)
        stored = an.find(root, anchor)
        if not stored and not insert:
            raise Refused("no row for %s in any registry. If this row is NEW, say so with "
                          "--insert; otherwise check the spelling -- a mistyped id mints a "
                          "second row and leaves the real one untouched." % anchor)
        target = stored[0] if len(stored) == 1 and not insert else None
        if target is not None:
            fields = changed_fields(an, fields, target)
            if not fields:
                print("apply-registry-row: %s already reads as this row -- nothing to write."
                      % anchor)
                return 0
    except an.Refused as exc:
        raise Refused(str(exc))
    # A duplicate (the id in both registries) and `--insert` over an existing row both reach the
    # door, which refuses them in its own words: one owner for those refusals.
    rc, out = an.door_write(root, anchor, fields, insert, write)
    for line in out.strip().splitlines():
        print("  " + line)
    if rc != 0:
        raise Refused("the door refused %s (rc %d) -- nothing was written:\n%s"
                      % (anchor, rc, out.strip()))
    print("apply-registry-row: %s" % ("WROTE %s through the door" % anchor if write
                                      else "dry run. pass --apply to write."))
    return 0


# ────────────────────────────────── self-test ──────────────────────────────────

def self_test():
    """Red-on-disable for the instrument. EVERY arm is a refusal, or a control for one.

    A tool that copies a good row into the right place is right by construction; what
    has to be exercised is each way a BAD row lands looking fine. The happy path is
    here only as the CONTROL that proves the refusals are not refusing everything --
    a guard that refuses unconditionally reports the same clean transcript as one that
    works.

    ⚠ THE PLACEMENT ARMS LIVE WITH THE DOOR AND ITS LAUNCHER (`anchors.py --self-test`
    drives every one of them into `dssharness`). What IS pinned here is the hand-off: that
    a validated row reaches the door, that an update carries only what changed, and that an
    absent launcher is a REFUSAL rather than a local fallback. The fixtures are git
    repositories holding this tree's own `anchors` configuration, because the door refuses
    a directory that is none -- and a host without DssHarness FAILS these arms.
    """
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
    G_ = _FX + "-GAMMA"
    R_ = _FX + "-RUNS"

    def pin(ok, why, detail=""):
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", why,
                               ("   " + detail) if detail else ""))
        if not ok:
            failed[0] += 1

    an = _anchors()
    ot = _owning_tree()
    HDR, SEP = an.TABLE_HEADER, an.SEP_ROW_TEXT
    GOOD = "| `" + A_ + "` | P1 | ✅ CLOSED | ✅ **shipped** | w | r |"
    RUNS = ("| `" + R_ + "` | P2 | 🟠 OPEN | 🟠 **OPEN** the list"
            " | see  for details \\| piped | `4  +  38` |")
    door_cfg = ot.load_jsonc(os.path.join(repo_root(), ".harness-config",
                                          "config.json")).get("anchors")

    def fixture(box):
        os.makedirs(os.path.join(box, ".plans"), exist_ok=True)
        os.makedirs(os.path.join(box, ".harness-config"), exist_ok=True)
        with io.open(os.path.join(box, ".harness-config", "config.json"), "w",
                     encoding="utf-8") as fh:
            json.dump({"anchors": door_cfg}, fh)

        def doc(rel, *body):
            with io.open(os.path.join(box, rel), "w", encoding="utf-8",
                         newline="") as fh:
                fh.write("\n".join(body) + "\n")
        doc(an.REL["production"], "# p", "", HDR, SEP,
            "| `" + A_ + "` | P1 | 🟠 OPEN | 🟠 **OPEN** | w | r |",
            "| `" + B_ + "` | P2 | 🟠 OPEN | 🟠 **OPEN** | w | r |", RUNS, "")
        doc(an.REL["done"], "# d", "", an.DONE_TABLE["production"], "", HDR, SEP,
            "| `" + _FX + "-OLDP` | P1 | ✅ CLOSED | ✅ **CLOSED** | - | r |", "")
        if not os.path.isdir(os.path.join(box, ".git")):
            ot.run_git(["init", "-q", box], capture_output=True, check=True)

    def refusal(box, rel, anchor, body, insert=False):
        path = os.path.join(box, "row.md")
        with io.open(path, "wb") as fh:
            fh.write(body if isinstance(body, bytes) else body.encode("utf-8"))
        try:
            # ⚠ DRY RUN. Every refusal below is raised BEFORE any write, so a dry run
            # exercises it exactly; and an arm that is supposed to be ACCEPTED (2b)
            # must not mutate the fixture, or the (C0) "nothing was written" control
            # is measuring this helper rather than the tool.
            apply_row(box, rel, anchor, path, write=False, insert=insert)
            return None
        except Refused as exc:
            return str(exc)

    with tempfile.TemporaryDirectory() as tmp:
        box = os.path.realpath(tmp)
        fixture(box)
        rel = an.REL["production"]
        original = io.open(os.path.join(box, rel), encoding="utf-8",
                           newline="").read()

        # (1) a WRAPPED row -- the defect this tool exists for.
        msg = refusal(box, rel, A_,
                      "| `" + A_ + "` | P1 | ✅ CLOSED | ✅ **shipped\nmore** | w | r |")
        pin(msg is not None and "more than one physical line" in msg,
            "(1) a row spanning two physical lines is REFUSED", "got=%r" % msg)

        # (1b) a row file that is not UTF-8 is refused, never read with replacement characters.
        msg = refusal(box, rel, A_, b"| `" + A_.encode() + b"` | P1 | \xe9 | t | w | r |")
        pin(msg is not None and "not UTF-8" in msg,
            "(1b) a row file that is not UTF-8 is REFUSED -- the door would store U+FFFD",
            "got=%r" % msg)

        # (2) a stray UNESCAPED pipe silently adds a column.
        msg = refusal(box, rel, A_,
                      "| `" + A_ + "` | P1 | ✅ CLOSED | a | b | c | d |")
        pin(msg is not None and "6 content cells" in msg,
            "(2) a row with the wrong cell count is REFUSED", "got=%r" % msg)

        # (2a) ...and the OLD four-cell shape is exactly that failure, named.
        msg = refusal(box, rel, A_,
                      "| `" + A_ + "` | ✅ **CLOSED** | w | r |")
        pin(msg is not None and "6 content cells" in msg,
            "(2a) a row in the RETIRED four-cell shape is REFUSED, not silently landed "
            "with its trigger in the status column", "got=%r" % msg)

        # (2b) an ESCAPED pipe is CONTENT, so it must still be accepted -- all the way
        # through the door's dry run. Without this the obvious 'fix' for (2) is to split on
        # every pipe, which would refuse every legitimate row containing one.
        msg = refusal(box, rel, A_,
                      "| `" + A_ + "` | P1 | ✅ CLOSED | ✅ a \\| b | w | r |")
        pin(msg is None, "(2b) an ESCAPED pipe is cell CONTENT, not a separator",
            "got=%r" % msg)

        # (3) the row's own anchor must equal the one named on the command line.
        msg = refusal(box, rel, B_, GOOD)
        pin(msg is not None and "expected a backticked" in msg,
            "(3) a row applied to the WRONG anchor is REFUSED", "got=%r" % msg)

        # (4) a duplicate is refused, never settled by position: a row OPEN in the working
        # registry and in the ARCHIVE at once, where settling by position would decide
        # whether the work is done by which document was read first.
        with io.open(os.path.join(box, an.REL["done"]), encoding="utf-8",
                     newline="") as fh:
            hl = fh.read().split("\n")
        hl.insert(6, "| `" + A_ + "` | P1 | 🟠 OPEN | 🟠 **OPEN again** | w | r |")
        io.open(os.path.join(box, an.REL["done"]), "w", encoding="utf-8",
                newline="").write("\n".join(hl))
        msg = refusal(box, rel, A_, GOOD)
        pin(msg is not None and "has 2 rows" in msg,
            "(4) one anchor with rows in TWO registries is REFUSED by the door, never settled "
            "by position", "got=%r" % msg)
        fixture(box)

        # (5) a destination outside the repository.
        msg = refusal(box, "../escape.md", A_, GOOD)
        pin(msg is not None and "escapes the repository" in msg,
            "(5) a destination outside the repo is REFUSED", "got=%r" % msg)

        # (5a) ...and the ARCHIVE is not a destination a caller may declare.
        msg = refusal(box, an.REL["done"], A_, GOOD)
        pin(msg is not None and "not a destination a caller declares" in msg,
            "(5a) naming the archive is REFUSED -- it is DERIVED from the status",
            "got=%r" % msg)

        # (C0) THE CONTROL: not one byte was written by any refusal above.
        pin(io.open(os.path.join(box, rel), encoding="utf-8", newline="").read()
            == original,
            "(C0) not one byte was written by any of the refusals above")

        # (C) THE CONTROL: the good row lands, and because it is CLOSED it MOVES.
        # ⓘ This asserts the HAND-OFF, not the routing rule: that a validated row reaches the
        # door at all. The routing itself is pinned where it lives.
        path = os.path.join(box, "row.md")
        io.open(path, "w", encoding="utf-8", newline="").write(GOOD + "\n")
        rc = apply_row(box, rel, A_, path, write=True)
        prod = io.open(os.path.join(box, rel), encoding="utf-8").read()
        done = io.open(os.path.join(box, an.REL["done"]), encoding="utf-8").read()
        pin(rc == 0 and ("`%s`" % A_) not in prod and GOOD in done and B_ in prod,
            "(C) the control lands: the CLOSED row reached the door, which moved it to the "
            "archive, and the sibling row is untouched")

        # (C2) AN UPDATE CARRIES ONLY WHAT CHANGED: the row file CLOSES a stored row -- its
        # status and its trigger change -- whose closing and cross-refs cells hold runs of
        # spaces and an escaped pipe; those cells did not change, so they are not sent, and
        # they keep their bytes through the MOVE to the archive.
        runs_closed = RUNS.replace("🟠 OPEN | 🟠 **OPEN** the", "✅ CLOSED | ✅ **CLOSED** the", 1)
        io.open(path, "w", encoding="utf-8", newline="").write(runs_closed + "\n")
        try:
            rc, why = apply_row(box, rel, R_, path, write=True), ""
        except Refused as exc:
            rc, why = None, str(exc)
        done = io.open(os.path.join(box, an.REL["done"]), encoding="utf-8").read()
        pin(rc == 0 and runs_closed in done,
            "(C2) an update sends ONLY the changed fields -- the unchanged closing and "
            "cross-refs cells, runs of spaces and an escaped pipe included, land byte for byte",
            "rc=%r %s" % (rc, why[:160]))

        # (C3) ...and a CHANGED cell holding a run is WRITTEN with the run KEPT. DssHarness 0.5.8
        # collapsed every in-line whitespace run (report #7), so the launcher refused such a cell;
        # 0.5.9 stores in-line whitespace as written (only a line break becomes one space), and
        # that refusal is deleted. Written for real and read back, so a door that collapses again
        # turns this arm red.
        io.open(path, "w", encoding="utf-8", newline="").write(
            "| `" + B_ + "` | P2 | 🟠 OPEN | 🟠 **OPEN** now  with a run | w | r |\n")
        try:
            rc, why = apply_row(box, rel, B_, path, write=True), ""
        except Refused as exc:
            rc, why = None, str(exc)
        prod = io.open(os.path.join(box, an.REL["production"]), encoding="utf-8").read()
        pin(rc == 0 and "🟠 **OPEN** now  with a run" in prod,
            "(C3) a CHANGED cell holding a run of spaces is WRITTEN with the run kept -- the "
            "door's own whitespace rule replaced the launcher's refusal", "rc=%r %s" % (rc, why[:160]))

        # (6) INSERT is DECLARED. A missing row without --insert is refused, because
        # the alternative -- inserting whatever anchor was typed -- turns a typo into a
        # second row while the real one stays stale and OPEN.
        fixture(box)
        msg = refusal(box, rel, G_,
                      "| `" + G_ + "` | P1 | 🟠 OPEN | 🟠 **OPEN** | w | r |")
        pin(msg is not None and "--insert" in msg,
            "(6) a row that does not exist is REFUSED unless --insert is declared",
            "got=%r" % msg)

        # (6b) ...and with --insert the same row is accepted by the door's dry run.
        msg = refusal(box, rel, G_,
                      "| `" + G_ + "` | P1 | 🟠 OPEN | 🟠 **OPEN** | w | r |", insert=True)
        pin(msg is None, "(6b) CONTROL: the same NEW row with --insert passes the door's dry run",
            "got=%r" % msg)

        # (7) ...and --insert over a row that DOES exist is refused just as loudly.
        msg = refusal(box, rel, A_, GOOD, insert=True)
        pin(msg is not None and "already has a row" in msg,
            "(7) --insert over an EXISTING row is REFUSED by the door -- each mode refuses "
            "the other's world", "got=%r" % msg)

        # (8) THE HAND-OFF ITSELF: with no launcher this REFUSES rather than falling back to
        # a local copy of the placement rule. The RESOLVER CONSTANT is what is probed,
        # because the resolver is what would have to be edited to reintroduce a fallback.
        global ANCHORS_PY
        held = ANCHORS_PY
        try:
            ANCHORS_PY = os.path.join(box, "no-such-anchors.py")
            io.open(path, "w", encoding="utf-8", newline="").write(GOOD + "\n")
            try:
                apply_row(box, rel, A_, path, write=False)
                msg = None
            except Refused as exc:
                msg = str(exc)
            pin(msg is not None and ".harness-config/runner/actions/anchors/anchors.py" in msg,
                "(8) an absent launcher is a REFUSAL, never a local fallback copy of the "
                "move-on-close rule", "got=%r" % msg)
        finally:
            ANCHORS_PY = held
        # ...and the CONTROL for that arm: with the resolver restored the same call
        # succeeds, so arm (8) is measuring the resolver rather than a broken fixture.
        try:
            apply_row(box, rel, A_, path, write=False)
            msg = None
        except Refused as exc:
            msg = str(exc)
        pin(msg is None,
            "(8b) the CONTROL: with the resolver restored the same call is accepted",
            "got=%r" % msg)

    # (R) THE ROOT IS THE TREE THIS FILE LIVES IN, whatever the caller's cwd -- every
    # `--apply` lands under it. Arms, oracle and synthesized negatives are owned by
    # .harness-config/runner/actions/owning-tree/owning-tree.py.
    for ok, why, detail in _owning_tree().root_arms(repo_root, (Refused,), False, __file__):
        pin(ok, "(R) " + why, "" if ok else detail)

    print("apply-registry-row self-test: %d failed" % failed[0])
    return 1 if failed[0] else 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    positional = [a for a in argv if not a.startswith("--")]
    if len(positional) != 3:
        print(__doc__)
        return 3
    try:
        return apply_row(repo_root(), positional[0], positional[1], positional[2],
                         write="--apply" in argv, insert="--insert" in argv)
    except Refused as exc:
        print("apply-registry-row: REFUSED -- %s" % exc)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
