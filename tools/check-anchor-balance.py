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
    """name -> status excerpt, for every row whose status cell is not marked closed."""
    out = {}
    for line in text.split("\n"):
        m = ROW.match(line)
        if not m:
            continue
        name, rest = m.group(1), m.group(2)
        status = rest.split("|")[0]
        if CLOSED_MARK not in status:
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
