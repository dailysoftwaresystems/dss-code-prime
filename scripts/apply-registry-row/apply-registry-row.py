#!/usr/bin/env python3
# PURPOSE: replace one deferred-anchor registry row with a lane's verbatim row text from a file.
"""apply-registry-row.py -- put ONE lane's row into the registry, byte for byte.

★★★ THE ROW IS NEVER RETYPED, AND THAT IS THE WHOLE REASON THIS EXISTS. A retyped row
can WRAP AN ANCHOR ID, and a wrapped id **does not fail**: it goes INVISIBLE to every
grep, to `check-anchor-registry`, and to `check-anchor-balance` -- and it MINTS a false
id at the same time. In a fail-loud project that is the one defect class that cannot be
caught by watching for a failure. ✔MEASURED 2026-08-20: of the 78 distinct `D-*` ids
cited on one cycle's added lines, **17 were wrapped**, and 16 were harmless only
because the same id appeared unwrapped nearby.
⇒ The lane writes its row to a FILE as one physical line; this copies the bytes.

★★ AND A DUPLICATE IS REFUSED, NEVER RESOLVED. Parallel lanes give one anchor TWO
renditions -- typically an OPENER from one lane and a CLOSER from another. Settling
that pair by POSITION (or by sort order) wrote the OPEN rendition back over a fixed
defect **three times in cycle P42**. This tool refuses and names the count; a human
reads both and decides.

WHAT IS VALIDATED BEFORE ANYTHING IS WRITTEN -- each clause is a way a bad row lands
looking fine:
  1. the row file holds EXACTLY ONE physical line (a wrapped row is the defect above);
  2. it splits into EXACTLY 4 content cells on UNESCAPED pipes, between a leading and a
     trailing pipe -- a `|` inside a cell must be written `\\|` or the registry table
     silently gains a column;
  3. the first cell is a BACKTICKED anchor id equal to the one named on the command
     line -- so a row cannot be applied to the wrong anchor by a slip in either place;
  4. the destination holds EXACTLY ONE row for that anchor (see the duplicate rule);
  5. the destination is inside the repository, compared by RESOLVED PATH PREFIX.

The status transition is PRINTED (old marker -> new marker) so the caller sees whether
this application actually closed anything. ⓘ A row is CLOSED iff its status cell begins
with the closure mark after stripping `*_ ` -- the complement is defined, never
enumerated, exactly as `check-anchor-balance` defines it.

Write-temp + `os.replace`. Never stages, never runs a git write verb.


ⓘ NO `.ps1` TWIN, DELIBERATELY. This is a `.py`, which runs unchanged on the
Windows leg and on every POSIX leg, so a PowerShell sibling would be a SECOND
IMPLEMENTATION of something that was never split -- two programs to keep in
step where one has no host to fail on. The omission is stated rather than
merely taken, because a gate cannot tell a deliberate POSIX-or-portable-only
script from a forgotten twin.
Exit codes: 0 OK (or a clean dry run) · 2 refused (nothing written) · 3 usage error.

Usage:
    python scripts/apply-registry-row/apply-registry-row.py <registry-rel> <anchor> <row-file>
    python scripts/apply-registry-row/apply-registry-row.py ... --apply     # write
    python scripts/apply-registry-row/apply-registry-row.py ... --insert    # NEW row
    python scripts/apply-registry-row/apply-registry-row.py --self-test
"""
from __future__ import annotations

import io
import os
import re
import subprocess
import sys
import tempfile

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):  # pragma: no cover
        pass

CLOSED_MARK = "✅"
# A cell separator is a pipe that is NOT backslash-escaped. The lookbehind is the whole
# grammar: `\|` inside a cell is content, `|` between cells is structure.
SPLIT = re.compile(r"(?<!\\)\|")


class Refused(Exception):
    pass


def repo_root():
    """From git, never hardcoded -- a predecessor pinned one absolute Windows path."""
    out = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True)
    if out.returncode != 0:
        raise Refused("not inside a git repository.")
    return os.path.realpath(out.stdout.decode("utf-8", "surrogateescape").strip())


def status_mark(cell):
    """The leading marker of a status cell, with emphasis stripped."""
    return cell.lstrip("*_ ")[:1]


def read_row(rowfile, anchor):
    """-> the validated row text. Raises Refused with the reason."""
    if not os.path.isfile(rowfile):
        raise Refused("no row file at %s" % rowfile)
    raw = io.open(rowfile, encoding="utf-8", newline="").read()
    row = raw.strip("\r\n")
    if "\n" in row or "\r" in row:
        raise Refused("the row file holds more than one physical line. A registry row "
                      "is ONE line -- a wrapped anchor id goes invisible to every grep "
                      "instead of failing.")
    cells = SPLIT.split(row)
    # `| a | b | c | d |` splits to ['', a, b, c, d, ''] -- 6 parts, 4 content cells.
    if len(cells) != 6 or cells[0].strip() or cells[-1].strip():
        raise Refused("%d part(s) after splitting on UNESCAPED pipes; a row must have "
                      "exactly 4 content cells between a leading and a trailing pipe. "
                      "A literal pipe inside a cell must be written as an escaped pipe."
                      % len(cells))
    first = cells[1].strip()
    if first != "`%s`" % anchor:
        raise Refused("first cell is %r, expected a backticked %s" % (first, anchor))
    return row, cells


ANCHOR_TABLE_HEADER = "| Anchor | Trigger | Closing work | Cross-refs |"


def anchor_table_end(lines):
    """Index AFTER the last row of the file's anchor table. Raises Refused.

    A new row is APPENDED to the end of the anchor table rather than sorted into it.
    ⚠ Sorting is exactly what must not happen: an alphabetical fold is how an
    opener/closer pair gets settled by position, which wrote an OPEN row back over a
    fixed defect three times in cycle P42. Append is order-preserving and reviewable
    as a one-line diff.

    ⚠ AND THE TABLE IS LOCATED, NEVER GUESSED. If a registry ever grows a SECOND
    anchor table this refuses rather than picking one -- an insert into the wrong
    table is invisible to a reader and still counted by the gate, so silence here
    would be worse than a red.
    """
    heads = [i for i, ln in enumerate(lines) if ln.strip() == ANCHOR_TABLE_HEADER]
    if len(heads) != 1:
        raise Refused("found %d anchor-table header(s) (%r); an insert needs exactly "
                      "one, and picking between two would be invisible to a reader."
                      % (len(heads), ANCHOR_TABLE_HEADER))
    i = heads[0] + 1
    if i >= len(lines) or set(lines[i].replace("|", "").strip()) - set("- :"):
        raise Refused("the line after the anchor-table header is not a separator row.")
    i += 1
    last = None
    while i < len(lines) and lines[i].startswith("|"):
        if lines[i].startswith("| `D-"):
            last = i
        i += 1
    if last is None:
        raise Refused("the anchor table holds no rows -- refusing to insert into a "
                      "table this scan could not read. Fix the scan, never the floor.")
    return last + 1


def apply_row(root, rel, anchor, rowfile, write, insert=False):
    dest = os.path.realpath(os.path.join(root, rel))
    if not (dest + os.sep).startswith(root + os.sep):
        raise Refused("destination escapes the repository: %s" % rel)
    if not os.path.isfile(dest):
        raise Refused("no registry at %s" % rel)

    row, cells = read_row(rowfile, anchor)
    new_mark = status_mark(cells[2])

    text = io.open(dest, encoding="utf-8", newline="").read()
    lines = text.split("\n")
    hits = [i for i, ln in enumerate(lines) if ln.startswith("| `%s`" % anchor)]
    if len(hits) > 1:
        raise Refused("%d existing row(s) for %s in %s -- a duplicate is read by a "
                      "human, never settled here. Two lanes give one anchor two "
                      "renditions; picking by position has written an OPEN row back "
                      "over a fixed defect." % (len(hits), anchor, rel))
    # ⚠ INSERT AND REPLACE ARE DECLARED, NEVER INFERRED FROM WHETHER THE ROW HAPPENS
    # TO EXIST. A typo in the anchor name would otherwise MINT a new row that looks
    # exactly like the one somebody meant to update -- and both would then sit in the
    # registry, the stale one still OPEN. Each mode refuses the other's world.
    if not hits and not insert:
        raise Refused("no row for %s in %s. If this row is NEW, say so with --insert; "
                      "otherwise check the anchor spelling -- a mistyped id here mints "
                      "a second row and leaves the real one untouched." % (anchor, rel))
    if hits and insert:
        raise Refused("--insert was given but %s already has a row in %s. Drop "
                      "--insert to replace it." % (anchor, rel))

    print("apply-registry-row: %s in %s" % (anchor, rel))
    if hits:
        i = hits[0]
        old_mark = status_mark(SPLIT.split(lines[i])[2])
        print("  status  %r -> %r   (%s -> %s)"
              % (old_mark, new_mark,
                 "CLOSED" if old_mark == CLOSED_MARK else "OPEN",
                 "CLOSED" if new_mark == CLOSED_MARK else "OPEN"))
        print("  size    %d chars -> %d chars" % (len(lines[i]), len(row)))
    else:
        i = anchor_table_end(lines)
        print("  INSERT  new row, status %r (%s), %d chars, appended to the anchor table"
              % (new_mark, "CLOSED" if new_mark == CLOSED_MARK else "OPEN", len(row)))

    if not write:
        print("apply-registry-row: dry run. pass --apply to write.")
        return 0

    if hits:
        lines[i] = row
    else:
        lines.insert(i, row)
    tmp = dest + ".row-tmp"
    with io.open(tmp, "w", encoding="utf-8", newline="") as fh:
        fh.write("\n".join(lines))
    os.replace(tmp, dest)
    print("apply-registry-row: WROTE %s" % rel)
    return 0


# ────────────────────────────────── self-test ──────────────────────────────────

def self_test():
    """Red-on-disable for the instrument. EVERY arm is a refusal.

    A tool that copies a good row into the right place is right by construction; what
    has to be exercised is each way a BAD row lands looking fine. The happy path is
    here only as the CONTROL that proves the refusals are not refusing everything --
    a guard that refuses unconditionally reports the same clean transcript as one that
    works.
    """
    failed = [0]

    def pin(ok, why, detail=""):
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", why,
                               ("   " + detail) if detail else ""))
        if not ok:
            failed[0] += 1

    def refusal(root, rel, anchor, body):
        path = os.path.join(root, "row.md")
        with io.open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(body)
        try:
            # ⚠ DRY RUN. Every refusal below is raised BEFORE any write, so a dry run
            # exercises it exactly; and an arm that is supposed to be ACCEPTED (2b)
            # must not mutate the fixture, or the (C0) "nothing was written" control
            # is measuring this helper rather than the tool.
            apply_row(root, rel, anchor, path, write=False)
            return None
        except Refused as exc:
            return str(exc)

    GOOD = "| `D-XX-ALPHA` | ✅ **CLOSED -- shipped** | w | r |"
    with tempfile.TemporaryDirectory() as tmp:
        root = os.path.realpath(tmp)
        rel = "reg.md"
        original = ("| Anchor | Trigger | Closing work | Cross-refs |\n"
                    "|---|---|---|---|\n"
                    "| `D-XX-ALPHA` | \U0001f7e0 **OPEN** | w | r |\n"
                    "| `D-XX-BETA` | \U0001f7e0 **OPEN** | w | r |\n")
        with io.open(os.path.join(root, rel), "w", encoding="utf-8", newline="") as fh:
            fh.write(original)

        # (1) a WRAPPED row -- the defect this tool exists for.
        msg = refusal(root, rel, "D-XX-ALPHA",
                      "| `D-XX-ALPHA` | ✅ **CLOSED -- shipped\nmore** | w | r |")
        pin(msg is not None and "more than one physical line" in msg,
            "(1) a row spanning two physical lines is REFUSED", "got=%r" % msg)

        # (2) a stray UNESCAPED pipe silently adds a column.
        msg = refusal(root, rel, "D-XX-ALPHA",
                      "| `D-XX-ALPHA` | ✅ a | b | c | d |")
        pin(msg is not None and "4 content cells" in msg,
            "(2) a row with the wrong cell count is REFUSED", "got=%r" % msg)

        # ...and an ESCAPED pipe is CONTENT, so it must still be accepted. Without this
        # the obvious 'fix' for (2) is to split on every pipe, which would refuse every
        # legitimate row containing one.
        msg = refusal(root, rel, "D-XX-ALPHA",
                      "| `D-XX-ALPHA` | ✅ a \\| b | w | r |")
        pin(msg is None, "(2b) an ESCAPED pipe is cell CONTENT, not a separator",
            "got=%r" % msg)

        # (3) the row's own anchor must equal the one named on the command line.
        msg = refusal(root, rel, "D-XX-BETA", GOOD)
        pin(msg is not None and "expected a backticked" in msg,
            "(3) a row applied to the WRONG anchor is REFUSED", "got=%r" % msg)

        # (4) a duplicate is refused, never settled by position.
        with io.open(os.path.join(root, "dup.md"), "w", encoding="utf-8",
                     newline="") as fh:
            fh.write(original + "| `D-XX-ALPHA` | \U0001f7e0 **OPEN again** | w | r |\n")
        msg = refusal(root, "dup.md", "D-XX-ALPHA", GOOD)
        pin(msg is not None and "2 existing row(s)" in msg,
            "(4) TWO rows for one anchor are REFUSED, never settled by position",
            "got=%r" % msg)

        # (5) a destination outside the repository.
        msg = refusal(root, "../escape.md", "D-XX-ALPHA", GOOD)
        pin(msg is not None and "escapes the repository" in msg,
            "(5) a destination outside the repo is REFUSED", "got=%r" % msg)

        # (C) THE CONTROL: the good row lands, replaces exactly one line, and the
        # SIBLING row is untouched.
        after = io.open(os.path.join(root, rel), encoding="utf-8", newline="").read()
        pin(after == original,
            "(C0) not one byte was written by any of the refusals above", "")
        path = os.path.join(root, "row.md")
        with io.open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(GOOD + "\n")
        rc = apply_row(root, rel, "D-XX-ALPHA", path, write=True)
        after = io.open(os.path.join(root, rel), encoding="utf-8", newline="").read()
        pin(rc == 0 and GOOD in after and "D-XX-BETA` | \U0001f7e0 **OPEN**" in after
            and len(after.split("\n")) == len(original.split("\n")),
            "(C) the control lands: one line replaced, the sibling row untouched", "")

        # (6) INSERT is DECLARED. A missing row without --insert is refused, because
        # the alternative -- inserting whatever anchor was typed -- turns a typo into a
        # second row while the real one stays stale and OPEN.
        with io.open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write("| `D-XX-GAMMA` | \u2705 **CLOSED** | w | r |\n")
        try:
            apply_row(root, rel, "D-XX-GAMMA", path, write=True)
            msg = None
        except Refused as exc:
            msg = str(exc)
        pin(msg is not None and "--insert" in msg,
            "(6) a row that does not exist is REFUSED unless --insert is declared",
            "got=%r" % msg)

        # (7) ...and --insert over a row that DOES exist is refused just as loudly.
        with io.open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(GOOD + "\n")
        try:
            apply_row(root, rel, "D-XX-ALPHA", path, write=True, insert=True)
            msg = None
        except Refused as exc:
            msg = str(exc)
        pin(msg is not None and "already has a row" in msg,
            "(7) --insert over an EXISTING row is REFUSED -- each mode refuses the "
            "other's world", "got=%r" % msg)

        # (8) THE CONTROL FOR THE INSERT PATH: the new row lands at the END of the
        # anchor table, and nothing already there moves.
        before_lines = io.open(os.path.join(root, rel), encoding="utf-8",
                               newline="").read().split("\n")
        with io.open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write("| `D-XX-GAMMA` | \u2705 **CLOSED** | w | r |\n")
        rc = apply_row(root, rel, "D-XX-GAMMA", path, write=True, insert=True)
        after_lines = io.open(os.path.join(root, rel), encoding="utf-8",
                              newline="").read().split("\n")
        added = [ln for ln in after_lines if ln not in before_lines]
        pin(rc == 0 and len(after_lines) == len(before_lines) + 1
            and added == ["| `D-XX-GAMMA` | \u2705 **CLOSED** | w | r |"]
            and after_lines[:len(before_lines) - 1] == before_lines[:-1],
            "(8) --insert appends ONE row at the table's end and moves nothing else",
            "added=%r" % added)

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
