#!/usr/bin/env python3
# PURPOSE: refuse a new `path:line` citation in the plans -- a citation names a stable reference, never a line number.
"""check-plan-citations.py -- the CITATION STABILITY guard.

★★★ WHY THIS EXISTS, and it is a measured recurrence rather than a style rule.
**A line number is a claim about a file that nothing rechecks.** Insert one line
above it and the citation silently moves off its subject -- it still resolves, it
still looks like evidence, and it now points at unrelated prose. A citation that
is merely BROKEN gets noticed; one that is WRONG does not.

✔MEASURED, twice in a single cycle (P17, 2026-08-19):
  1. inserting a `# PURPOSE:` line at the top of eighteen scripts moved **16**
     citations in the plans off their subjects. `check-anchor-registry.sh:29` had
     pointed at *"…staleness sweep commit message…"* and pointed instead at
     *"Same pattern the developer-side audit grep uses…"*;
  2. the rows written to RECORD that defect then shipped **three more** wrong
     line numbers of their own, each pointing at the first line of an
     explanatory comment rather than at the code it explains.

Both were found by independent audit, neither by any gate.

★★ THE OPERATOR'S RULE THIS ENFORCES (2026-08-19): *"we must never document line
numbers, we must document method names, comment ids or defined anchors.
everything that changes is unreliable."*

So a citation names something the file CARRIES rather than a position it happens
to occupy:

    ✗ `src/mir/lowering.cpp:412`
    ✓ `src/mir/lowering.cpp` `lowerCallArgs()`
    ✓ `tests/CMakeLists.txt` (the `no RUN_SERIAL` rationale)
    ✓ [[D-TEST-INTEGRATED-FIXED-TEMP-PATH-COLLIDES]]

A symbol survives every edit above it; a line number survives none.

★★★ WHY A RATCHET AND NOT A BAN. ✔MEASURED at first run: **2374** line-number
citations already exist across **575** distinct cited files, 572 of them in the
deferred-anchor registry alone -- overwhelmingly inside CLOSED rows describing
what was true at a commit years of cycles ago. Rewriting them wholesale would be
a vast, low-value edit that also destroys the historical record's own precision.
So the inventory records what exists per document, and a ceiling may only come
DOWN: a new citation reds immediately, and a converted one reds until its ceiling
is lowered in the same commit. Unclaimed headroom is where the next regression
hides -- the same reasoning `check-no-abort-in-tests` states for `abort()`.

⚠ THE INVENTORY IS DEBT, NOT A PASS. A green run here means *no NEW positional
citation landed*, never *the plans cite stably*.

Exit codes: 0 OK · 1 a ceiling was exceeded or is stale · 2 the scan collapsed
· 3 usage error.

Usage:
    python scripts/check-plan-citations/check-plan-citations.py            # verify
    python scripts/check-plan-citations/check-plan-citations.py --write    # re-baseline
    python scripts/check-plan-citations/check-plan-citations.py --selftest # prove it fails
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

INVENTORY_REL = os.path.join("scripts", "check-plan-citations", "inventory.json")

# Documents this guard governs. The plans are the point; the skills are included
# because they give the same instructions to the next cycle.
SCAN_ROOTS = (".plans", ".claude")

# A citation is a repo-ish path with a file extension, followed by `:<digits>`.
# Deliberately NOT anchored to real files on disk: a citation into a file that
# was since deleted is exactly as unreliable, and excluding it would let a stale
# one hide.
CITATION = re.compile(
    r"[A-Za-z0-9_][A-Za-z0-9_./+-]*\."
    r"(?:cpp|hpp|h|c|cc|py|sh|ps1|json|md|txt|yml|yaml|cmake|S|asm|mjs|js|ts)"
    r":\d+")

# A URL carries `:<digits>` as a port and may carry a whole path after it, so a
# citation-shaped substring can sit ANYWHERE inside one. Convicting it would
# teach readers that this guard cries wolf, which is how a guard stops being
# read at all.
# ★ Matched as a SPAN rather than a fixed-width lookbehind: the first version
# looked back 12 characters, which does not reach `http://` from the end of
# `http://localhost:8080/x.py:12`. Its own self-test caught that.
URL_SPAN = re.compile(r"https?://\S+")

EXIT_OK, EXIT_RATCHET, EXIT_COLLAPSE, EXIT_USAGE = 0, 1, 2, 3

# Far below the live figure (2374 at first baseline) so ordinary churn never
# trips it, and high enough that a collapsed enumeration cannot pass as clean.
DOC_FLOOR = 40


class Collapse(Exception):
    """The scan failed structurally. Never reported as a clean pass."""


def repo_root():
    try:
        p = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                           capture_output=True, check=False)
    except OSError as exc:
        raise Collapse("cannot run git (%s)" % exc)
    if p.returncode != 0:
        raise Collapse("not inside a git checkout")
    return p.stdout.decode("utf-8", "replace").strip()


def documents(root):
    """Every governed markdown document, in a stable order."""
    out = []
    for rel_root in SCAN_ROOTS:
        base = os.path.join(root, rel_root)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, files in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in ("__pycache__", "worktrees")]
            for f in files:
                if f.endswith(".md"):
                    rel = os.path.relpath(os.path.join(dirpath, f), root)
                    out.append(rel.replace(os.sep, "/"))
    return sorted(out)


def count_in(path):
    """Positional citations in one document."""
    n = 0
    with io.open(path, "r", encoding="utf-8", newline="") as fh:
        for line in fh:
            urls = [(u.start(), u.end()) for u in URL_SPAN.finditer(line)]
            for m in CITATION.finditer(line):
                if any(a <= m.start() < b for a, b in urls):
                    continue
                n += 1
    return n


def census(root):
    docs = documents(root)
    if len(docs) < DOC_FLOOR:
        raise Collapse(
            "found only %d governed document(s) under %s, floor is %d. The scan "
            "COLLAPSED -- fix the scan, do not lower the floor."
            % (len(docs), " + ".join(SCAN_ROOTS), DOC_FLOOR))
    return {d: count_in(os.path.join(root, d)) for d in docs if count_in(os.path.join(root, d))}


def load_inventory(root):
    path = os.path.join(root, INVENTORY_REL)
    if not os.path.isfile(path):
        raise Collapse(
            "the inventory %s does not exist. Without it every count is "
            "unconstrained and this guard asserts nothing." % INVENTORY_REL)
    with io.open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)["ceilings"]


def write_inventory(root, ceilings):
    path = os.path.join(root, INVENTORY_REL)
    body = {
        "_comment": [
            "Per-document ceilings for POSITIONAL citations (`path:line`).",
            "A ceiling may only come DOWN. Convert citations to a stable reference -- a",
            "symbol name, a comment id, or a [[D-ANCHOR]] -- and lower the ceiling in the",
            "same commit. A raised ceiling is a new unreliable citation and reds the gate.",
            "This file is DEBT, not a pass: green means no NEW positional citation landed.",
        ],
        "ceilings": dict(sorted(ceilings.items())),
    }
    tmp = path + ".tmp"
    with io.open(tmp, "w", encoding="utf-8", newline="") as fh:
        json.dump(body, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    os.replace(tmp, path)


def run(root, write):
    now = census(root)
    if write:
        write_inventory(root, now)
        print("check-plan-citations: re-baselined %d document(s), %d citation(s)"
              % (len(now), sum(now.values())))
        return EXIT_OK

    ceilings = load_inventory(root)
    raised, stale = [], []
    for doc, n in sorted(now.items()):
        ceiling = ceilings.get(doc, 0)
        if n > ceiling:
            raised.append((doc, ceiling, n))
    for doc, ceiling in sorted(ceilings.items()):
        if now.get(doc, 0) < ceiling:
            stale.append((doc, ceiling, now.get(doc, 0)))

    if raised:
        print("check-plan-citations: FAIL -- new positional citation(s) landed:")
        for doc, c, n in raised:
            print("    %s: %d -> %d" % (doc, c, n))
        print("")
        print("  A `path:line` citation is a claim about a file that nothing rechecks.")
        print("  Insert one line above it and it silently points at unrelated prose while")
        print("  still reading as evidence. Cite something the file CARRIES instead:")
        print("      a symbol      src/mir/lowering.cpp `lowerCallArgs()`")
        print("      a rationale   tests/CMakeLists.txt (the `no RUN_SERIAL` block)")
        print("      an anchor     [[D-TEST-INTEGRATED-FIXED-TEMP-PATH-COLLIDES]]")
        return EXIT_RATCHET

    if stale:
        print("check-plan-citations: FAIL -- ceiling(s) above the live count:")
        for doc, c, n in stale:
            print("    %s: ceiling %d, actual %d -- lower it in this commit" % (doc, c, n))
        print("")
        print("  Unclaimed headroom is where the next one hides. If you converted a")
        print("  citation, lower the ceiling in the same commit:")
        print("      python scripts/check-plan-citations/check-plan-citations.py --write")
        return EXIT_RATCHET

    total = sum(now.values())
    print("check-plan-citations: OK (%d positional citation(s) across %d document(s), all "
          "within the inventory ratchet). DEBT, not a pass - the plans still cite %d line "
          "numbers." % (total, len(now), total))
    return EXIT_OK


# ── RED-ON-DISABLE SELF-TEST ────────────────────────────────────────────────
# Every arm asserts the MESSAGE, not merely a non-zero exit: this guard has two
# distinct failures (ceiling raised / ceiling stale) that share an exit code, and
# an arm that checks only the code cannot tell which one it proved. That mistake
# was measured in a sibling guard in this same cycle.

EXPECTED_ARMS = 9
_RAN = None


def _arm(label, root, expect, says=None, not_says=None):
    if _RAN is not None:
        _RAN.append(label)
    buf = io.StringIO()
    detail = ""
    try:
        with contextlib.redirect_stdout(buf):
            rc = run(root, write=False)
    except Collapse as exc:
        rc = EXIT_COLLAPSE
        detail = str(exc)
    text = buf.getvalue() + detail
    ok, why = rc == expect, ""
    if not ok:
        why = "EXPECTED rc=%d" % expect
    elif says is not None and says not in text:
        ok, why = False, "rc right but the message never said %r" % says
    elif not_says is not None and not_says in text:
        ok, why = False, "rc right but the message said %r -- wrong refusal" % not_says
    first = (detail or buf.getvalue()).split("\n")[0][:72]
    print("plan-citations: self-test arm %-24s rc=%d %s%s"
          % (label, rc, "as expected" if ok else why, (" (" + first + ")") if first else ""))
    return ok


def selftest(root):
    tmp = tempfile.mkdtemp(prefix="plan-citations-selftest-")
    ok = True
    ran = []
    try:
        globals()["_RAN"] = ran
        for rel_root in SCAN_ROOTS:
            src = os.path.join(root, rel_root)
            if os.path.isdir(src):
                shutil.copytree(src, os.path.join(tmp, rel_root),
                                ignore=shutil.ignore_patterns("__pycache__", "worktrees", "*.pyc"))
        os.makedirs(os.path.dirname(os.path.join(tmp, INVENTORY_REL)), exist_ok=True)
        shutil.copyfile(os.path.join(root, INVENTORY_REL), os.path.join(tmp, INVENTORY_REL))

        subject = os.path.join(tmp, ".plans", "_handoff.md")
        pristine = io.open(subject, encoding="utf-8", newline="").read()
        inv_path = os.path.join(tmp, INVENTORY_REL)
        pristine_inv = io.open(inv_path, encoding="utf-8", newline="").read()

        ok &= _arm("0 GREEN-CONTROL", tmp, EXIT_OK)

        # a new positional citation
        io.open(subject, "w", encoding="utf-8", newline="").write(
            pristine + "\nSee src/core/zz_selftest.cpp:1234 for details.\n")
        ok &= _arm("1 NEW-CITATION", tmp, EXIT_RATCHET,
                   says="new positional citation", not_says="above the live count")
        io.open(subject, "w", encoding="utf-8", newline="").write(pristine)
        ok &= _arm("1b RESTORED", tmp, EXIT_OK)

        # a converted citation with the ceiling left behind. ★ The mutation STRIPS
        # the `:<line>` suffix, which is exactly what converting to a stable
        # reference does. An earlier version renamed a directory instead and
        # removed no citation at all, so the arm proved nothing -- and said so.
        converted = CITATION.sub(lambda m: m.group(0).rsplit(":", 1)[0], pristine, count=3)
        assert converted != pristine, "arm 2 mutation removed no citation"
        io.open(subject, "w", encoding="utf-8", newline="").write(converted)
        ok &= _arm("2 CEILING-NOW-STALE", tmp, EXIT_RATCHET,
                   says="above the live count", not_says="new positional citation")
        io.open(subject, "w", encoding="utf-8", newline="").write(pristine)
        ok &= _arm("2b RESTORED", tmp, EXIT_OK)

        # the inventory itself
        os.remove(inv_path)
        ok &= _arm("3 INVENTORY-MISSING", tmp, EXIT_COLLAPSE, says="does not exist")
        io.open(inv_path, "w", encoding="utf-8", newline="").write(pristine_inv)

        # a URL's port is not a citation
        io.open(subject, "w", encoding="utf-8", newline="").write(
            pristine + "\nSee http://localhost:8080/x.py:12 and 14:30 UTC.\n")
        ok &= _arm("4 PORT-IS-NOT-A-CITATION", tmp, EXIT_OK)
        io.open(subject, "w", encoding="utf-8", newline="").write(pristine)

        # the scan collapses
        held = tempfile.mkdtemp(prefix="plan-citations-held-")
        shutil.move(os.path.join(tmp, ".plans"), os.path.join(held, ".plans"))
        ok &= _arm("5 SCAN-COLLAPSED", tmp, EXIT_COLLAPSE, says="floor is")
        shutil.move(os.path.join(held, ".plans"), os.path.join(tmp, ".plans"))
        shutil.rmtree(held, ignore_errors=True)

        ok &= _arm("6 GREEN-AFTER-RESTORE", tmp, EXIT_OK)
    finally:
        globals()["_RAN"] = None
        shutil.rmtree(tmp, ignore_errors=True)

    if len(ran) != EXPECTED_ARMS:
        print("plan-citations: self-test FAILED -- %d arms ran, %d expected."
              % (len(ran), EXPECTED_ARMS))
        return EXIT_COLLAPSE
    if not ok:
        print("plan-citations: self-test FAILED -- this guard is NOT proven able to fail.")
        return EXIT_COLLAPSE
    print("plan-citations: self-test OK - %d arms exercised, every red arm asserting the "
          "MESSAGE of the refusal it names; this guard is PROVEN able to fail." % len(ran))
    return EXIT_OK


def main(argv):
    write = "--write" in argv[1:]
    self_ = "--selftest" in argv[1:]
    unknown = [a for a in argv[1:] if a not in ("--write", "--selftest")]
    if unknown:
        print("check-plan-citations: unknown argument(s): %s" % " ".join(unknown))
        return EXIT_USAGE
    try:
        root = repo_root()
        if self_:
            return selftest(root)
        if write:
            return run(root, write=True)
        rc = run(root, write=False)
        if rc != EXIT_OK:
            return rc
        return selftest(root)
    except Collapse as exc:
        print("check-plan-citations: FAIL (structural) -- %s" % exc)
        print("  This does NOT mean the plans are clean - it means the SCAN COLLAPSED.")
        return EXIT_COLLAPSE


if __name__ == "__main__":
    sys.exit(main(sys.argv))
