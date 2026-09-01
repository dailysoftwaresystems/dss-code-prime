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
defect **three times in cycle P42**. The shared writer refuses and names the count; a
human reads both and decides.

WHAT IS VALIDATED BEFORE ANYTHING IS WRITTEN -- each clause is a way a bad row lands
looking fine:
  1. the row file holds EXACTLY ONE physical line (a wrapped row is the defect above);
  2. it splits into EXACTLY 6 content cells on UNESCAPED pipes, between a leading and a
     trailing pipe -- a raw pipe inside a cell must be backslash-escaped or the registry
     table silently gains a column;
  3. the first cell is a BACKTICKED anchor id equal to the one named on the command
     line -- so a row cannot be applied to the wrong anchor by a slip in either place;
  4. no registry holds more than ONE row for that anchor (see the duplicate rule);
  5. the destination is inside the repository, compared by RESOLVED PATH PREFIX.

⚠ SIX CELLS SINCE 2026-09-01, not four. The registry gained explicit `Priority` and
`Status` columns -- `| Anchor | Priority | Status | Trigger | Closing work | Cross-refs |`
-- so the verdict is a cell of its own rather than the first glyph of the trigger prose.
A row written to the old four-cell shape would land with its TRIGGER sitting in the
STATUS column, which is why the count is a refusal and not a warning.

★★★ AND SINCE THE SAME DATE IT DOES NOT PLACE THE ROW ITSELF -- `scripts/anchors/anchors.py`
DOES. The registry became THREE documents (production / harness / done) with a
move-on-close rule: a row whose status is closed is DELETED from its working registry and
appended to the archive's matching table, and a reopened one moves back. That routing is
ONE decision, and two programs that both write the registry are two programs that will
eventually disagree about it. This file keeps what is uniquely its own -- the validation
of a lane's VERBATIM row file, where the bytes are copied and never retyped -- and hands
the placement to the shared writer. `anchors.py write` is the same act from PARAMETERS
instead of from a file; `anchors.py set` patches named fields on a row that already
exists.

The status transition and any MOVE are PRINTED, so the caller sees whether this
application actually closed anything. ⓘ A row is CLOSED iff its STATUS cell begins with
the closure mark after stripping `*_ ` -- the complement is defined, never enumerated,
exactly as `check-anchor-balance` defines it, and this file does not define it a second
time.

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

import importlib.util
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

# A cell separator is a pipe that is NOT backslash-escaped. The lookbehind is the whole
# grammar: an escaped pipe inside a cell is content, a bare one between cells is
# structure.
SPLIT = re.compile(r"(?<!\\)\|")
CELLS = 6


class Refused(Exception):
    pass


def repo_root():
    """From git, never hardcoded -- a predecessor pinned one absolute Windows path."""
    out = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True)
    if out.returncode != 0:
        raise Refused("not inside a git repository.")
    return os.path.realpath(out.stdout.decode("utf-8", "surrogateescape").strip())


# The shared writer, resolved from THIS FILE rather than from the tree being edited.
# ⚠ THE DISTINCTION IS LOAD-BEARING: `apply_row` takes a repository ROOT (which the
# self-test points at a temporary fixture), while the writer is a SIBLING SCRIPT that
# always lives beside this one. Resolving it from the root would mean any tree without a
# `scripts/anchors/` had no writer -- including every fixture.
ANCHORS_PY = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "anchors", "anchors.py")


def _anchors():
    """The shared writer. Imported by path -- a hyphen is not a module name.

    ⚠ FAILS LOUD rather than falling back to a local copy of the placement rule. A
    fallback here would be a second, quieter answer to "where does a closed row go",
    which is precisely what centralising it was for.
    """
    path = ANCHORS_PY
    if not os.path.isfile(path):
        raise Refused("cannot find scripts/anchors/anchors.py -- this tool VALIDATES a "
                      "row file and delegates the PLACEMENT to it; it does not carry a "
                      "second copy of the move-on-close rule.")
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
    raw = io.open(rowfile, encoding="utf-8", newline="").read()
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


def apply_row(root, rel, anchor, rowfile, write, insert=False):
    dest_decl = os.path.realpath(os.path.join(root, rel))
    if not (dest_decl + os.sep).startswith(root + os.sep):
        raise Refused("destination escapes the repository: %s" % rel)
    if not os.path.isfile(dest_decl):
        raise Refused("no registry at %s" % rel)

    an = _anchors()
    row, _cells = read_row(rowfile, anchor)

    # ⚠ THE ARGUMENT NAMES THE BUCKET, NOT THE FILE THE ROW ENDS UP IN. A caller says
    # "this is a production row"; where it LANDS follows from its status, and the
    # archive is derived rather than declared -- see anchors.place_row.
    # Resolved-path comparison, not a string match on `rel`: the same registry can be
    # named `.plans/x.md` or `./.plans/x.md` and only one of those would match by text.
    bucket = next((b for b, r in an.REL.items()
                   if os.path.realpath(os.path.join(root, r)) == dest_decl), None)
    if bucket is None:
        raise Refused("%s is not one of the three anchor registries (%s)."
                      % (rel, ", ".join(sorted(an.REL.values()))))
    if bucket == "done":
        raise Refused(
            "the archive is not a destination a caller declares. Name the WORKING "
            "registry the row belongs to (%s); a row whose status is closed is routed "
            "into the archive from there, and a reopened one is moved back out."
            % ", ".join(an.WORKING))

    try:
        dest = an.place_row(root, bucket, anchor, row, write=write, insert=insert)
    except an.Refused as exc:
        raise Refused(str(exc))
    print("  size    %d chars" % len(row))
    if not write:
        print("apply-registry-row: dry run. pass --apply to write.")
    else:
        print("apply-registry-row: WROTE %s" % dest)
    return 0


# ────────────────────────────────── self-test ──────────────────────────────────

def self_test():
    """Red-on-disable for the instrument. EVERY arm is a refusal.

    A tool that copies a good row into the right place is right by construction; what
    has to be exercised is each way a BAD row lands looking fine. The happy path is
    here only as the CONTROL that proves the refusals are not refusing everything --
    a guard that refuses unconditionally reports the same clean transcript as one that
    works.

    ⚠ THE PLACEMENT ARMS LIVE IN `anchors.py --self-test`, WHERE THE PLACEMENT LIVES.
    Re-asserting the move-on-close rule here would be a second pin on a behaviour this
    file no longer owns -- and a pin that keeps passing after the real one is deleted is
    worse than no pin. What IS pinned here is the hand-off: that a validated row reaches
    the shared writer, and that an absent writer is a REFUSAL rather than a local
    fallback.
    """
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
    G_ = _FX + "-GAMMA"
    CC_ = _FX + "-CELLS"
    HG_ = _FX + "-HGAMMA"


    def pin(ok, why, detail=""):
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", why,
                               ("   " + detail) if detail else ""))
        if not ok:
            failed[0] += 1

    an = _anchors()
    HDR, SEP = an.TABLE_HEADER, an.SEP_ROW_TEXT
    GOOD = "| `" + A_ + "` | P1 | ✅ CLOSED | ✅ **shipped** | w | r |"

    def fixture(box):
        os.makedirs(os.path.join(box, ".plans"), exist_ok=True)

        def doc(rel, *body):
            with io.open(os.path.join(box, rel), "w", encoding="utf-8",
                         newline="") as fh:
                fh.write("\n".join(body) + "\n")
        doc(an.REL["production"], "# p", "", HDR, SEP,
            "| `" + A_ + "` | P1 | 🟠 OPEN | 🟠 **OPEN** | w | r |",
            "| `" + B_ + "` | P2 | 🟠 OPEN | 🟠 **OPEN** | w | r |", "")
        doc(an.REL["harness"], "# h", "", HDR, SEP,
            "| `" + HG_ + "` | P3 | 🟠 OPEN | 🟠 **OPEN** | w | r |", "")
        doc(an.REL["done"], "# d", "", an.DONE_TABLE["production"], "", HDR, SEP,
            "| `" + _FX + "-OLDP` | P1 | ✅ CLOSED | ✅ **CLOSED** | - | r |", "",
            an.DONE_TABLE["harness"], "", HDR, SEP,
            "| `" + _FX + "-OLDH` | P3 | ✅ CLOSED | ✅ **CLOSED** | - | r |", "")

    def refusal(box, rel, anchor, body):
        path = os.path.join(box, "row.md")
        with io.open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(body)
        try:
            # ⚠ DRY RUN. Every refusal below is raised BEFORE any write, so a dry run
            # exercises it exactly; and an arm that is supposed to be ACCEPTED (2b)
            # must not mutate the fixture, or the (C0) "nothing was written" control
            # is measuring this helper rather than the tool.
            apply_row(box, rel, anchor, path, write=False)
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

        # (2b) an ESCAPED pipe is CONTENT, so it must still be accepted. Without this
        # the obvious 'fix' for (2) is to split on every pipe, which would refuse every
        # legitimate row containing one.
        msg = refusal(box, rel, A_,
                      "| `" + A_ + "` | P1 | ✅ CLOSED | a \\| b | w | r |")
        pin(msg is None, "(2b) an ESCAPED pipe is cell CONTENT, not a separator",
            "got=%r" % msg)

        # (3) the row's own anchor must equal the one named on the command line.
        msg = refusal(box, rel, B_, GOOD)
        pin(msg is not None and "expected a backticked" in msg,
            "(3) a row applied to the WRONG anchor is REFUSED", "got=%r" % msg)

        # (4) a duplicate is refused, never settled by position.
        with io.open(os.path.join(box, an.REL["harness"]), encoding="utf-8",
                     newline="") as fh:
            hl = fh.read().split("\n")
        hl.insert(5, "| `" + A_ + "` | P1 | 🟠 OPEN | 🟠 **OPEN again** | w | r |")
        io.open(os.path.join(box, an.REL["harness"]), "w", encoding="utf-8",
                newline="").write("\n".join(hl))
        msg = refusal(box, rel, A_, GOOD)
        pin(msg is not None and "One id, one home" in msg,
            "(4) one anchor with rows in TWO registries is REFUSED, never settled by "
            "position", "got=%r" % msg)
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
        # ⓘ This asserts the HAND-OFF, not the routing rule: that a validated row
        # reaches the shared writer at all. The routing itself is pinned where it lives.
        path = os.path.join(box, "row.md")
        io.open(path, "w", encoding="utf-8", newline="").write(GOOD + "\n")
        rc = apply_row(box, rel, A_, path, write=True)
        prod = io.open(os.path.join(box, rel), encoding="utf-8").read()
        done = io.open(os.path.join(box, an.REL["done"]), encoding="utf-8").read()
        pin(rc == 0 and A_ not in prod and GOOD in done
            and B_ in prod,
            "(C) the control lands: the CLOSED row reached the shared writer, which "
            "moved it to the archive, and the sibling row is untouched")

        # (6) INSERT is DECLARED. A missing row without --insert is refused, because
        # the alternative -- inserting whatever anchor was typed -- turns a typo into a
        # second row while the real one stays stale and OPEN.
        fixture(box)
        io.open(path, "w", encoding="utf-8", newline="").write(
            "| `" + G_ + "` | P1 | 🟠 OPEN | 🟠 **OPEN** | w | r |\n")
        msg = refusal(box, rel, G_,
                      "| `" + G_ + "` | P1 | 🟠 OPEN | 🟠 **OPEN** | w | r |")
        pin(msg is not None and "--insert" in msg,
            "(6) a row that does not exist is REFUSED unless --insert is declared",
            "got=%r" % msg)

        # (7) ...and --insert over a row that DOES exist is refused just as loudly.
        io.open(path, "w", encoding="utf-8", newline="").write(GOOD + "\n")
        try:
            apply_row(box, rel, A_, path, write=False, insert=True)
            msg = None
        except Refused as exc:
            msg = str(exc)
        pin(msg is not None and "already has a row" in msg,
            "(7) --insert over an EXISTING row is REFUSED -- each mode refuses the "
            "other's world", "got=%r" % msg)

        # (8) THE HAND-OFF ITSELF: with no shared writer this REFUSES rather than
        # falling back to a local copy of the placement rule.
        # The RESOLVER CONSTANT is what is probed, because the resolver is what would
        # have to be edited to reintroduce a fallback.
        global ANCHORS_PY
        held = ANCHORS_PY
        try:
            ANCHORS_PY = os.path.join(box, "no-such-anchors.py")
            fixture(box)
            io.open(path, "w", encoding="utf-8", newline="").write(GOOD + "\n")
            try:
                apply_row(box, rel, A_, path, write=False)
                msg = None
            except Refused as exc:
                msg = str(exc)
            pin(msg is not None and "scripts/anchors/anchors.py" in msg,
                "(8) an absent shared writer is a REFUSAL, never a local fallback copy "
                "of the move-on-close rule", "got=%r" % msg)
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
