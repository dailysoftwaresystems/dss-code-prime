#!/usr/bin/env python3
"""Mechanical staleness scans for dss-plan-sweep (taxonomy classes 2, 3, 4, 5).

Run at workflow step 4 (find) and again at step 7 (verify). Identical both times —
step 7 passes when this exits 0.

    python scan_staleness.py [repo-root] [--quiet]

Exit 0 = no mechanical divergence found. Exit 1 = hits printed. Exit 2 = bad usage.
NOTE: capture the exit code directly, never after a pipe — `| head` reports head's rc.

SCOPE, and why it differs per class:
  * dead links      -> .plans/ + README.md + .claude/skills/  (any file can rot a link)
  * push / ctest / anchor-collision -> .plans/ + README.md ONLY. The skill files
    DESCRIBE these patterns ("a `commit-pending` row that is already pushed"), so
    scanning them matches the documentation rather than a stale claim.

DELIBERATELY NOT CHECKED HERE:
  * anchor open/closed COUNTS — `scripts/check-anchor-registry/check-anchor-registry.{ps1,sh}` owns that.
    This script never re-derives them; a second counting instrument is exactly how
    that number has gone wrong before.
  * whether a ctest claim is CORRECT — that needs a live run. Claims are listed so
    a human can diff them against the baseline taken at workflow step 2.
  * class 4 (closed anchor also described as open) — MEASURED UNMECHANIZABLE at a
    line granularity in this repo, 2026-08-13. A first implementation flagged 2929
    "collisions": the plans carry single table rows holding dozens of anchor IDs
    alongside words like OPEN, so an anchor-ID x open-word pairing explodes. An
    instrument that over-reports by ~100x trains its reader to ignore it, which is
    worse than having none. Class 4 stays a read, against the anchor guard, per the
    taxonomy. Do not re-add it as a line grep.
  * taxonomy classes 1, 6, 7, 8, 9, 10 — they need reading, not grepping.
"""
import os
import re
import sys
from urllib.parse import unquote

CLAIM_ROOTS = [".plans", "README.md"]
LINK_ROOTS = [".plans", "README.md", os.path.join(".claude", "skills")]
SKIP_DIRS = {".git", "node_modules", "__pycache__", "worktrees", "build",
             "build-dbg", "build-rel"}

PENDING_PUSH = re.compile(r"pending[- ]push|commit[- ]pending", re.IGNORECASE)
# Tight adjacency on purpose: a suite count is `ctest 838/838` or `838/838 ctest`, and
# both halves are 3+ digits. A loose 40-char window matched 219 lines on this repo —
# almost all prose that merely mentions ctest somewhere on a very long line.
CTEST_CLAIM = re.compile(r"\b\d{3,5}\s*/\s*\d{3,5}\b[^\n]{0,12}?ctest", re.IGNORECASE)
CTEST_CLAIM_ALT = re.compile(r"ctest[^\n]{0,12}?\b\d{3,5}\s*/\s*\d{3,5}\b", re.IGNORECASE)
# Markdown links only: no whitespace in the target, which excludes prose/code in parens.
MD_LINK = re.compile(r"\[[^\]]*\]\((?!#)([^)\s]+)\)")
# A target is only treated as a path if it looks like one — kills `[n](45)`-shaped prose.
PATHISH = re.compile(r"[/.]")
# Repo convention: links may carry a `:line` suffix (`foo.hpp:1541`). Strip before resolving.
LINE_SUFFIX = re.compile(r":\d+$")


def iter_files(root, entries):
    seen = set()
    for entry in entries:
        path = os.path.join(root, entry)
        if os.path.isfile(path):
            seen.add(path)
            continue
        if not os.path.isdir(path):
            continue
        for dirpath, dirnames, filenames in os.walk(path):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for fn in filenames:
                if fn.endswith(".md"):
                    seen.add(os.path.join(dirpath, fn))
    return sorted(seen)


def rel(root, path):
    return os.path.relpath(path, root).replace("\\", "/")


def read_lines(path):
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read().split("\n")


def link_resolves(root, src_file, target):
    """A link resolves if it exists relative to its own file OR to the repo root.
    Percent-encoding is decoded first — plan filenames contain spaces (`%20`)."""
    t = unquote(target.strip().split("#", 1)[0].strip())
    if not t or "://" in t or t.startswith("mailto:"):
        return True
    if not PATHISH.search(t):
        return True
    t = LINE_SUFFIX.sub("", t)
    if os.path.exists(os.path.normpath(os.path.join(os.path.dirname(src_file), t))):
        return True
    return os.path.exists(os.path.normpath(os.path.join(root, t)))


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    quiet = "--quiet" in argv[1:]
    root = os.path.abspath(args[0]) if args else os.getcwd()

    if not os.path.isdir(os.path.join(root, ".plans")):
        print(f"ERROR: no .plans/ under {root} — is this the repo root?", file=sys.stderr)
        return 2

    link_files = iter_files(root, LINK_ROOTS)
    claim_files = iter_files(root, CLAIM_ROOTS)
    if not link_files:
        print(f"ERROR: no markdown found under {root}", file=sys.stderr)
        return 2

    dead_links = []
    for path in link_files:
        try:
            lines = read_lines(path)
        except (OSError, UnicodeDecodeError) as exc:
            print(f"ERROR: cannot read {rel(root, path)}: {exc}", file=sys.stderr)
            return 2
        for i, line in enumerate(lines, 1):
            for target in MD_LINK.findall(line):
                if not link_resolves(root, path, target):
                    dead_links.append((f"{rel(root, path)}:{i}", target))

    pending, ctest_claims = [], []
    for path in claim_files:
        try:
            lines = read_lines(path)
        except (OSError, UnicodeDecodeError) as exc:
            print(f"ERROR: cannot read {rel(root, path)}: {exc}", file=sys.stderr)
            return 2
        for i, line in enumerate(lines, 1):
            loc = f"{rel(root, path)}:{i}"
            if PENDING_PUSH.search(line):
                pending.append((loc, line.strip()[:110]))
            # Report the MATCHED CLAIM, not the line. Plan lines here are whole
            # paragraphs, so a line excerpt is unreadable and unactionable; the
            # claim text ("838/838 ctest") is what gets diffed against the baseline.
            for rx in (CTEST_CLAIM, CTEST_CLAIM_ALT):
                for m in rx.finditer(line):
                    claim = " ".join(m.group(0).split())
                    ctest_claims.append((claim, loc))

    def section(title, rows, fmt):
        if not rows:
            if not quiet:
                print(f"  {title}: clean")
            return 0
        print(f"  {title}: {len(rows)} hit(s)")
        for r in rows:
            print(f"    {fmt(r)}")
        return len(rows)

    print(f"Mechanical staleness scan — {len(claim_files)} plan file(s), "
          f"{len(link_files)} file(s) link-checked, under {root}")
    n = 0
    n += section("class 3  pending-push markers", pending, lambda r: f"{r[0]}  {r[1]}")
    n += section("class 5  dead relative links", dead_links, lambda r: f"{r[0]}  -> {r[1]}")
    print("  class 4  closed-anchor/open-mention: NOT scanned here — read it against "
          "the anchor guard (see module docstring)")

    if ctest_claims:
        grouped = {}
        for claim, loc in ctest_claims:
            grouped.setdefault(claim, []).append(loc)
        print(f"  class 2  distinct ctest claims: {len(grouped)} "
              f"({len(ctest_claims)} occurrence(s)) — diff against the step-2 baseline")
        for claim in sorted(grouped):
            locs = grouped[claim]
            shown = ", ".join(locs[:3]) + (f", +{len(locs) - 3} more" if len(locs) > 3 else "")
            print(f"    {claim!r}  x{len(locs)}  [{shown}]")
    elif not quiet:
        print("  class 2  ctest claims: none found")

    print()
    if n:
        print(f"RESULT: {n} mechanical divergence(s) — sweep is NOT finished.")
        return 1
    print("RESULT: mechanical classes clean."
          + (" Verify the ctest claims above against the live count." if ctest_claims else ""))
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, OSError):
        pass
    sys.exit(main(sys.argv))
