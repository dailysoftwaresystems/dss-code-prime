#!/usr/bin/env python3
"""Anchor balance gate: a cycle may not end with more OPEN registry rows than it began.

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
"""
import argparse
import io
import os
import re
import subprocess
import sys

REG_REL = ".plans/_deferred-anchor-registry.md"
ROW = re.compile(r"^\| `(D-[A-Z0-9-]+)` \|(.*)$")

# The ONLY marker that means closed. Everything else in a status cell is open.
CLOSED_MARK = "✅"  # white heavy check mark


def repo_root():
    p = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        sys.exit("not inside a git repository")
    return p.stdout.strip()


def open_rows(text):
    """name -> status excerpt, for every row whose status cell is not marked closed.

    A row is CLOSED iff its status cell OPENS with the closure mark (after any
    leading markdown emphasis/whitespace).  Everything else -- every other glyph,
    every novel glyph, no glyph at all -- is OPEN.  The complement is defined, never
    the variants, so a marker nobody has thought of yet counts as open, which is the
    safe direction.

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
    maintainer of open_rows() reads FIRST.  Measure with the tool, never with a scratch
    grep that may share the bug you are hunting.
    star star AND NOTE THE DIRECTION, BECAUSE IT IS THE SAME DIRECTION TWICE.  Version 1 of
    this gate enumerated the OPEN glyphs and was blind to the hourglass, reporting 269
    open where there were 579.  That was fixed by inverting to "open unless closed" --
    and the inversion introduced THIS blind spot.  Both errors flattered the cycle by
    hiding open rows.  An instrument that keeps failing toward "you are doing fine" is
    the one to distrust; when you change this function, ask which direction the new
    rule errs in and pin it in self_test().
    """
    out = {}
    for line in text.split("\n"):
        m = ROW.match(line)
        if not m:
            continue
        name, rest = m.group(1), m.group(2)
        status = rest.split("|")[0]
        if not status.lstrip().lstrip("*_ ").startswith(CLOSED_MARK):
            out[name] = " ".join(status.split())[:80]
    return out


def at_ref(root, ref):
    p = subprocess.run(["git", "show", "%s:%s" % (ref, REG_REL)], cwd=root,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        sys.exit("cannot read %s at %s: %s" % (REG_REL, ref, (p.stderr or "").strip()[:200]))
    return p.stdout


def self_test():
    """Red-on-disable for the instrument itself: the glyph-agnostic rule must hold.

    If someone 'helpfully' rewrites open_rows() to enumerate open glyphs, the novel
    glyph case below fails -- which is the exact regression this file exists to stop.
    """
    cases = [
        ("| `D-A` | ✅ **CLOSED** | done | refs |", set(), "a closed row is not open"),
        ("| `D-B` | \U0001f7e0 **OPEN** | work | refs |", {"D-B"}, "orange is open"),
        ("| `D-C` | ⏳ **OPEN** | work | refs |", {"D-C"}, "HOURGLASS is open (the miss)"),
        ("| `D-D` | \U0001f534 **OPEN** | work | refs |", {"D-D"}, "red is open"),
        ("| `D-E` | ⚠ **OPEN** | work | refs |", {"D-E"}, "warning is open"),
        ("| `D-F` | \U0001f9ff **NOVEL GLYPH** | work | refs |", {"D-F"},
         "a glyph nobody enumerated is open -- the whole point"),
        ("| `D-G` | no glyph at all | work | refs |", {"D-G"}, "no marker at all is open"),
        # A closed row may legitimately mention an open sibling's glyph in LATER cells.
        ("| `D-H` | ✅ **CLOSED** | supersedes a \U0001f7e0 row | refs |", set(),
         "only the STATUS cell decides"),
        ("not a row at all", set(), "non-rows are ignored"),
        # star THE 2026-08-12 UNDERCOUNT, pinned. An OPEN row routinely uses the check
        # mark mid-prose for a done HALF or a piece of good news. A substring test read
        # these as closed and hid 65 open rows. Leading position is what decides.
        ("| `D-I` | \U0001f7e0 **OPEN** -- half ✅ done, half not | work | refs |",
         {"D-I"}, "mid-prose check mark does NOT close an open row (the 2nd miss)"),
        ("| `D-J` | ⏳ **PARTIALLY CLOSED -- ✅ part (1); part (2) open** | work | refs |",
         {"D-J"}, "PARTIALLY closed is OPEN, however many marks it carries"),
        # ... while the genuinely-closed spelling that RECAPS its own history still closes.
        ("| `D-K` | ✅ **CLOSED 2026-01-01.** *(Was: OPEN -- \U0001f534 HIGH.)* | done | refs |",
         set(), "a closed row may recap 'Was: OPEN' and stays closed"),
        ("| `D-L` | **✅ CLOSED** | done | refs |", set(),
         "leading markdown emphasis before the mark still closes"),
    ]
    failed = 0
    for text, expect, why in cases:
        got = set(open_rows(text))
        ok = got == expect
        if not ok:
            failed += 1
        print("  %-4s %-58s expected=%s got=%s"
              % ("ok" if ok else "FAIL", why, sorted(expect) or "-", sorted(got) or "-"))
    print("self-test: %d case(s), %d failed" % (len(cases), failed))
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default="HEAD",
                    help="git ref the cycle started from (default HEAD)")
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
    before = open_rows(at_ref(root, args.base))
    with io.open(os.path.join(root, REG_REL.replace("/", os.sep)),
                 encoding="utf-8") as fh:
        after = open_rows(fh.read())

    closed = sorted(set(before) - set(after))
    opened = sorted(set(after) - set(before))

    print("anchor-balance: OPEN at %s = %d" % (args.base, len(before)))
    print("anchor-balance: OPEN now       = %d   (net %+d)"
          % (len(after), len(after) - len(before)))
    print("anchor-balance: closed %d, opened %d" % (len(closed), len(opened)))
    for n in closed:
        print("  - %s" % n)
    for n in opened:
        print("  + %s   %s" % (n, after[n]))

    if len(after) > len(before):
        print()
        print("anchor-balance: FAIL - this cycle leaves %d more row(s) OPEN than it found."
              % (len(after) - len(before)))
        print("  Close what you opened, or take it to the operator as a decision (dss-cycle "
              "section B). Do NOT widen this gate to fit the cycle.")
        return 1
    print("anchor-balance: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
