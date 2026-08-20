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

# ★★★ THE CODE IS GOVERNED TOO, AND UNTIL 2026-08-19 IT WAS NOT.
# The operator's rule -- *"we must never document line numbers, we must document
# method names, comment ids or defined anchors. everything that changes is
# unreliable"* -- is about DOCUMENTATION, and a comment in a `.cpp` is
# documentation. This guard read `.md` under two roots, so every `path:line` in
# shipped source and in `.lang.json` config was invisible to it.
# ✔THE EVIDENCE IS A CITATION THAT WENT STALE INSIDE ONE CYCLE: `mir_to_lir.cpp`
# cited a `mir_opcode.hpp` line, a sibling lane's edit moved the row, and nothing
# could have reported it. That is the whole failure mode, observed rather than
# argued.
#
# ★★★ AND MARKDOWN OUTSIDE THE TWO DOC ROOTS WAS GOVERNED BY NOTHING AT ALL.
# `SCAN_ROOTS` reads `.md` under `.plans` + `.claude`; this family read
# `CODE_EXTS`, which did not list `.md`. A markdown document anywhere else fell
# between the two and no guard ever opened it.
# ✔THE EVIDENCE IS THE SAME ROT IN AN AUTHOR-FACING SCHEMA DOCUMENT:
# `examples/README.md` carried EIGHT positional citations. Six were rechecked
# against the tree on 2026-08-20 and exactly ONE still resolved to the code it
# claimed -- the rest had drifted onto unrelated comments, one onto a bare `}`.
# ★ MARKDOWN JOINS *THIS* FAMILY, NOT THE DOCUMENT FAMILY, AND THAT IS MEASURED
# RATHER THAN TIDY. `DOC_FLOOR` is calibrated to `.plans` (41 documents) +
# `.claude` (32) alone. Fold the other twelve markdown files into that family and
# losing `.plans` entirely still enumerates 44 -- ABOVE the floor -- so the
# doc-collapse detector stops detecting. That is exactly the "one family
# vanishing behind the other's size" failure `CODE_FLOOR` exists to prevent,
# reproduced at a fiftieth of the scale. Two families, two floors, and markdown
# outside the documentation roots belongs to the tree it lives in.
# ★ `docs`, `packaging` and `.github` join the roots for the same reason: each
# carries author-facing markdown that no root named. ✔MEASURED 2026-08-20 they
# carry ZERO positional citations, so the ceiling that arrives with them is zero
# -- and before the first one lands is the only cheap moment to start a ratchet.
# ★ `integrated_tests` AND `real-examples` JOINED 2026-08-20, AND LEAVING THEM
# OUT WAS THIS GUARD BREAKING THE PROJECT'S OWN RULE. There are two corpus
# runners -- `tests/examples` in-process and `integrated_tests` by CLI subprocess
# -- and a capability landing in one but not its sibling is a silent harness bug
# by standing rule. A citation guard that watched one and not the other WAS that
# bug, inside the guard. `real-examples` holds the sqlite harness, which is the
# project's headline goal.
# ✔MEASURED 2026-08-20, and identical at HEAD and in the working tree, so no
# sibling lane's in-flight edit is being baselined: `integrated_tests` 8
# positional citations, `real-examples` 50. That is DEBT entering the ratchet at
# its live count, which is what the inventory is for -- from here it only falls.
# ⚠ AND THE UPSTREAM CORPUS IS IN NEITHER, WHICH WAS CHECKED BEFORE TAKING THEM.
# The sqlite harness stages upstream under `build/real-examples/...`, never
# beside its own scripts -- ✔16 tracked files under `real-examples`, 18 on disk.
# A root that acquired a third-party tree at test time would bake somebody
# else's line numbers into our inventory, where no cycle could ever lower them.
CODE_ROOTS = ("src", "tests", "scripts", "examples", "docs", "packaging", ".github",
              "integrated_tests", "real-examples")
# ★ `.yml`/`.yaml` were the EXTENSION SET LAGGING BEHIND THE ROOT. `CITATION` has
# always matched a `.yml` path, so a workflow could be cited unreliably and could
# equally cite unreliably, while nothing ever opened a file with that suffix --
# `.github` was taken as a root and its workflows still went unread.
# ✔MEASURED 2026-08-20: 13 tracked yml/yaml files, all under `.github`, carrying
# ZERO positional citations, so these too start at a ceiling of zero.
CODE_EXTS = (".cpp", ".hpp", ".h", ".c", ".cc", ".json", ".py", ".sh", ".ps1",
             ".cmake", ".txt", ".s", ".S", ".md", ".yml", ".yaml")

# ⚠ A CITATION OF A FILE OUTSIDE THIS REPOSITORY IS COUNTED TOO, AND THAT IS
# DELIBERATE. The tempting carve-out is "an SDK header is not ours to convert",
# but ranking those SAFER inverts the truth: `SDK/usr/include/sys/mount.h:366` is
# a claim about a file that differs between machines and between SDK versions,
# where a claim about our own file at least drifts under version control. The
# remedy is identical in both cases -- cite the DECLARATION, not the line.
# ★ It also keeps this guard's stated philosophy intact: citations are not
# anchored to files on disk, because a citation into a deleted file is exactly as
# unreliable as one into a moved line.

# A citation is a repo-ish path with a file extension, followed by `:<digits>`.
# Deliberately NOT anchored to real files on disk: a citation into a file that
# was since deleted is exactly as unreliable, and excluding it would let a stale
# one hide.
CITATION = re.compile(
    r"[A-Za-z0-9_][A-Za-z0-9_./+-]*\."
    r"(?:cpp|hpp|h|c|cc|py|sh|ps1|json|md|txt|yml|yaml|cmake|S|asm|mjs|js|ts)"
    r":\d+")

# ★★★ A CONTINUATION CITATION — the shape `<file>.cpp:<line>/:<line>/:<line>`.
# ⚠ SPELLED WITH PLACEHOLDERS ON PURPOSE: writing a real one here would make
# this guard's own comment carry four citations of the kind it refuses, and
# the first draft of this paragraph DID — caught by re-deriving the inventory
# and seeing this file's own ceiling move 3 → 7. Every
# reference after the first omits the filename, so `CITATION` sees ONE and the
# other two are invisible. ✔MEASURED 2026-08-20 across all 288 inventoried
# documents: **35** such references in **19** documents, none of them counted by
# anything. They are positional citations by every reading of the rule this guard
# enforces, and the idiom is the natural one to reach for when citing three sites
# in one file, so the blind spot sat exactly where the rule is most tempting to
# break.
#
# ⚠ THE SEPARATOR IS MANDATORY, AND THAT IS THE WHOLE PRECISION OF THIS PATTERN.
# Without it `<file>.cpp:<line>:<col>` — a line:COLUMN reference, ONE citation —
# would count as two, and this guard's numbers would stop meaning what the
# inventory says they mean. With it, `:100/:120`, `:100, :120` and `:100 :120`
# are caught while `:100:24` stays a single citation. A trailing `-<digits>`
# range (`:1239-1245`) is likewise part of ONE citation and is consumed, not
# recounted.
#
# ⚠ Found by an INDEPENDENT AUDIT of this cycle's own repair, not by the guard:
# the audit disputed a count in `examples/README.md`, and the disagreement was
# real — 12 line references, 5 of them visible here. Widening the roots does not
# help if the matcher inside them is narrower than the rule.
CONTINUATION = re.compile(r"(?:\s*[,;/·]\s*|\s+):\d+(?:-\d+)?")

# A URL carries `:<digits>` as a port and may carry a whole path after it, so a
# citation-shaped substring can sit ANYWHERE inside one. Convicting it would
# teach readers that this guard cries wolf, which is how a guard stops being
# read at all.
# ★ Matched as a SPAN rather than a fixed-width lookbehind: the first version
# looked back 12 characters, which does not reach `http://` from the end of
# `http://localhost:8080/x.py:12`. Its own self-test caught that.
URL_SPAN = re.compile(r"https?://\S+")

EXIT_OK, EXIT_RATCHET, EXIT_COLLAPSE, EXIT_USAGE = 0, 1, 2, 3

# ★ A DOCUMENT COUNT, NOT A CITATION COUNT -- `census()` compares this against
# `len(documents())`. ⚠ The value stood at 40 justifying itself against "2374 at
# first baseline", which is the CITATION total and not the quantity this floor
# ever compared. ✔MEASURED 2026-08-20 the live figure is 73 documents (`.plans`
# 41 + `.claude` 32), and the wrong yardstick left a real hole: a scan that lost
# `.claude` and kept `.plans` enumerated 41 and passed 40 as clean. 45 catches
# EITHER root vanishing alone while sitting 28 documents below the live count,
# which no ordinary churn reaches -- documents are added here, not deleted in
# dozens.
DOC_FLOOR = 45

# ★ THE CODE FAMILY GETS ITS OWN FLOOR, NOT A SHARED ONE. A single total
# would let one family collapse entirely while the other's size covered for it --
# ✔the live counts are ~73 documents in the two doc roots against ~2,310 files in
# this family, so a combined floor of 45 would be satisfied by the doc roots alone
# with every source file gone. Two floors make each family's collapse its own
# refusal. ⚠ Unchanged by both 2026-08-20 widenings -- markdown, then
# `integrated_tests` + `real-examples` + `.yml` -- which together moved this
# family ~2,267 -> ~2,313 and move nothing about where this floor sits.
# ★ AND IT IS A FAMILY-COLLAPSE DETECTOR, NEVER A PER-ROOT ONE. That is
# structural rather than an omission: catching the loss of `real-examples` (16
# files) or `integrated_tests` (2) would need a floor within a dozen of the live
# count, and ✔MEASURED this family grew by five files from sibling lanes during a
# single session -- such a floor would red on ordinary churn instead of on a
# collapse. Small roots are held by their own self-test arms, not by this number.
CODE_FLOOR = 400


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


def _walk(root, rel_roots, keep):
    """Every file under `rel_roots` whose name satisfies `keep`, repo-relative."""
    out = []
    for rel_root in rel_roots:
        base = os.path.join(root, rel_root)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, files in os.walk(base):
            # `build` is generated and enormous; `worktrees` is another checkout
            # and would double-count every file it holds.
            dirnames[:] = [d for d in dirnames
                           if d not in ("__pycache__", "worktrees", "build", ".git")]
            for f in files:
                if keep(f):
                    rel = os.path.relpath(os.path.join(dirpath, f), root)
                    out.append(rel.replace(os.sep, "/"))
    return sorted(out)


def documents(root):
    """Every markdown document in the two DOCUMENTATION roots, in a stable order.

    Markdown elsewhere in the tree is governed too, by `code_files()` -- see the
    `CODE_EXTS` rationale for why it belongs to that family rather than this one.
    """
    return _walk(root, SCAN_ROOTS, lambda f: f.endswith(".md"))


def code_files(root):
    """Every governed source, config and markdown file outside the doc roots."""
    # ★ THE REPOSITORY'S OWN TOP-LEVEL FILES COUNT TOO -- `README.md`,
    # `CONTRIBUTING.md`, `CMakeLists.txt`. They are enumerated NON-RECURSIVELY
    # here rather than by naming the repository root in `CODE_ROOTS`, because
    # descending from there would sweep in `build/`, `target/`, `.temp/` and
    # every other generated or gitignored tree.
    # ⚠ THAT EXCLUSION IS STRUCTURAL, NOT A NAME THIS SCRIPT REMEMBERS: the walk
    # is an ALLOW-LIST of roots, so scratch is out because nothing named it, and
    # a scratch tree invented tomorrow is out for the same reason. ✔MEASURED
    # 2026-08-20: `.temp/` alone holds 227 positional citations across 40
    # markdown files, none of them documentation -- governing it would bury the
    # ratchet under numbers no cycle could ever lower.
    top = sorted(f for f in os.listdir(root)
                 if f.endswith(CODE_EXTS) and os.path.isfile(os.path.join(root, f)))
    return top + _walk(root, CODE_ROOTS, lambda f: f.endswith(CODE_EXTS))


def count_in(path):
    """Positional citations in one document."""
    n = 0
    # ⚠ `errors="replace"`, added when the roots widened to code: the markdown
    # corpus is all UTF-8, the code corpus is NOT — ✔MEASURED, a fixture under
    # the new roots carries a 0x97 byte and the strict decode raised
    # `UnicodeDecodeError` mid-census, which this guard would have reported as a
    # crash rather than as a count. Replacement cannot invent a citation (the
    # pattern needs ASCII path bytes followed by `:<digits>`) and cannot hide one
    # (only the offending bytes become U+FFFD), so the count is unchanged wherever
    # the decode would have succeeded.
    with io.open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        for line in fh:
            urls = [(u.start(), u.end()) for u in URL_SPAN.finditer(line)]
            # `consumed` is why this is a walk and not a `findall`: a citation
            # head is followed by any number of bare continuations, and each one
            # must be counted ONCE and then skipped over, never re-entered from
            # the outer scan.
            consumed = 0
            for m in CITATION.finditer(line):
                if m.start() < consumed:
                    continue
                if any(a <= m.start() < b for a, b in urls):
                    continue
                n += 1
                consumed = m.end()
                while True:
                    cont = CONTINUATION.match(line, consumed)
                    if cont is None:
                        break
                    # A continuation inside a URL is still a port or a path
                    # fragment, so it takes the same exemption its head takes.
                    if any(a <= cont.start() < b for a, b in urls):
                        break
                    n += 1
                    consumed = cont.end()
    return n


def census(root):
    docs = documents(root)
    if len(docs) < DOC_FLOOR:
        raise Collapse(
            "found only %d governed document(s) under %s, floor is %d. The scan "
            "COLLAPSED -- fix the scan, do not lower the floor."
            % (len(docs), " + ".join(SCAN_ROOTS), DOC_FLOOR))
    code = code_files(root)
    if len(code) < CODE_FLOOR:
        raise Collapse(
            "found only %d governed code file(s) under %s, floor is %d. The scan "
            "COLLAPSED -- fix the scan, do not lower the floor."
            % (len(code), " + ".join(CODE_ROOTS), CODE_FLOOR))
    # ★ COUNTED ONCE PER FILE. The first version called `count_in` twice per
    # document -- once for the filter and once for the value -- which was merely
    # wasteful here but is the shape that lets a filter and a value disagree.
    out = {}
    for rel in docs + code:
        n = count_in(os.path.join(root, rel))
        if n:
            out[rel] = n
    return out


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

EXPECTED_ARMS = 25
_RAN = None

# ⚠ THE MUTATION FIXTURE IS ASSEMBLED, NOT SPELLED OUT. A literal `path:line` in
# this file is inventoried like any other, so every arm added here used to raise
# THIS GUARD'S OWN ceiling -- and a raised ceiling is indistinguishable in a diff
# from the regression the guard exists to catch. `CITATION` needs `:<digits>` and
# cannot match `:%d`, so assembling the fixture makes new arms free and lets the
# ceiling only ever fall. ✔MEASURED 2026-08-20: 5 -> 3 while ADDING two arms.
_FIXTURE = "src/core/zz_selftest.cpp:%d"


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
        # ⚠ THE CODE ROOTS ARE COPIED WITH AN EXTENSION FILTER, NOT WHOLESALE.
        # A faithful replica is what makes arm 0 mean anything, but `src` + `tests`
        # + `examples` also carry generated trees; copying only the extensions the
        # census actually reads keeps the replica exact for this guard's purpose
        # while leaving `build/` behind.
        def _drop(dirpath, names):
            drop = []
            for n in names:
                full = os.path.join(dirpath, n)
                if os.path.isdir(full):
                    if n in ("__pycache__", "worktrees", "build", ".git"):
                        drop.append(n)
                # ⚠ `.md` STAYS SPELLED OUT HERE EVEN THOUGH `CODE_EXTS` NOW
                # LISTS IT. `documents()` reads markdown unconditionally, so the
                # replica owes it markdown regardless of what the OTHER family's
                # extension set happens to contain. ✔MEASURED 2026-08-20 by
                # deleting the redundancy: with `.md` dropped from `CODE_EXTS`,
                # the replica lost `.plans/_handoff.md` and the whole suite died
                # on a `FileNotFoundError` before arm 0 -- a suite that cannot
                # report is worse than one that fails.
                elif not n.endswith(CODE_EXTS + (".md",)):
                    drop.append(n)
            return drop

        for rel_root in SCAN_ROOTS + CODE_ROOTS:
            src = os.path.join(root, rel_root)
            if os.path.isdir(src):
                shutil.copytree(src, os.path.join(tmp, rel_root), ignore=_drop)
        # ★ AND THE TOP-LEVEL FILES, which `code_files()` reaches through a
        # different enumeration than it reaches a root. Without them arm 10 has
        # no subject and would raise rather than red -- a replica that omits a
        # mechanism cannot witness it.
        for name in sorted(os.listdir(root)):
            if name.endswith(CODE_EXTS) and os.path.isfile(os.path.join(root, name)):
                shutil.copyfile(os.path.join(root, name), os.path.join(tmp, name))
        os.makedirs(os.path.dirname(os.path.join(tmp, INVENTORY_REL)), exist_ok=True)
        shutil.copyfile(os.path.join(root, INVENTORY_REL), os.path.join(tmp, INVENTORY_REL))

        subject = os.path.join(tmp, ".plans", "_handoff.md")
        pristine = io.open(subject, encoding="utf-8", newline="").read()
        inv_path = os.path.join(tmp, INVENTORY_REL)
        pristine_inv = io.open(inv_path, encoding="utf-8", newline="").read()

        ok &= _arm("0 GREEN-CONTROL", tmp, EXIT_OK)

        # a new positional citation
        io.open(subject, "w", encoding="utf-8", newline="").write(
            pristine + ("\nSee %s for details.\n" % (_FIXTURE % 1234)))
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

        # a NEW citation in shipped SOURCE, not in a plan. ★ This arm is the
        # whole reason the roots were widened: before 2026-08-19 it passed GREEN,
        # because the guard never opened a `.cpp` at all.
        code_subject = os.path.join(tmp, "src", "mir", "mir_opcode.hpp")
        code_pristine = io.open(code_subject, encoding="utf-8", newline="").read()
        io.open(code_subject, "w", encoding="utf-8", newline="").write(
            code_pristine + ("\n// see %s\n" % (_FIXTURE % 4321)))
        ok &= _arm("7 NEW-CITATION-IN-CODE", tmp, EXIT_RATCHET,
                   says="new positional citation", not_says="above the live count")
        io.open(code_subject, "w", encoding="utf-8", newline="").write(code_pristine)
        ok &= _arm("7b RESTORED", tmp, EXIT_OK)

        # a NEW citation in markdown that lives OUTSIDE the two documentation
        # roots. ★ This arm is the whole reason `.md` joined `CODE_EXTS` and
        # `docs` joined the roots: before 2026-08-20 it passed GREEN, because
        # `docs/` was named by neither family and nothing opened the file.
        docs_subject = os.path.join(tmp, "docs", "tree-model.md")
        assert os.path.isfile(docs_subject), (
            "arm 9 has no subject: `docs` is absent from CODE_ROOTS, so the "
            "replica never copied it -- the coverage is MISSING, not unproven")
        docs_pristine = io.open(docs_subject, encoding="utf-8", newline="").read()
        io.open(docs_subject, "w", encoding="utf-8", newline="").write(
            docs_pristine + ("\nSee %s for the walk.\n" % (_FIXTURE % 5678)))
        ok &= _arm("9 NEW-CITATION-IN-DOCS", tmp, EXIT_RATCHET,
                   says="new positional citation", not_says="above the live count")
        io.open(docs_subject, "w", encoding="utf-8", newline="").write(docs_pristine)
        ok &= _arm("9b RESTORED", tmp, EXIT_OK)

        # a NEW citation in the repository's OWN top-level README. ★ A SEPARATE
        # arm from 9 rather than a second mutation inside it: `code_files()`
        # reaches the top level by a different enumeration than it reaches a
        # root, so one subject cannot witness both, and a shared arm would report
        # a pass for whichever half still worked.
        top_subject = os.path.join(tmp, "README.md")
        assert os.path.isfile(top_subject), (
            "arm 10 has no subject: the replica never copied the repository's "
            "top-level files -- the coverage is MISSING, not unproven")
        top_pristine = io.open(top_subject, encoding="utf-8", newline="").read()
        io.open(top_subject, "w", encoding="utf-8", newline="").write(
            top_pristine + ("\nSee %s for the entry point.\n" % (_FIXTURE % 8765)))
        ok &= _arm("10 NEW-CITATION-AT-ROOT", tmp, EXIT_RATCHET,
                   says="new positional citation", not_says="above the live count")
        io.open(top_subject, "w", encoding="utf-8", newline="").write(top_pristine)
        ok &= _arm("10b RESTORED", tmp, EXIT_OK)

        # ★ ONE HELPER FOR THE COVERAGE TAKEN ON 2026-08-20 rather than three
        # more hand-copied mutate/assert/restore blocks: the three differ only in
        # subject and comment syntax, and hand-copying is exactly where an arm
        # that mutates one file while asserting about another comes from.
        def _planted(label, rel, line, nth):
            subject = os.path.join(tmp, *rel)
            assert os.path.isfile(subject), (
                "%s has no subject: %s is absent from the replica, so the census "
                "never reads it -- the coverage is MISSING, not merely unproven"
                % (label, "/".join(rel)))
            was = io.open(subject, encoding="utf-8", newline="").read()
            io.open(subject, "w", encoding="utf-8", newline="").write(
                was + (line % (_FIXTURE % nth)))
            good = _arm(label, tmp, EXIT_RATCHET,
                        says="new positional citation", not_says="above the live count")
            io.open(subject, "w", encoding="utf-8", newline="").write(was)
            return good & _arm(label.split()[0] + "b RESTORED", tmp, EXIT_OK)

        # ⚠ THE INVENTORY ALONE WOULD NOT KEEP THESE ROOTS. `integrated_tests`
        # and `real-examples` enter carrying debt, so deleting either root TODAY
        # reds as a stale ceiling -- but paying that debt down is the whole point
        # of the ratchet, and the day the last citation there is converted the
        # entries vanish and the root becomes silently droppable again. These
        # arms outlive the debt; the ratchet's cover for them does not.
        ok &= _planted("12 NEW-CITATION-IN-ITESTS",
                       ("integrated_tests", "runner.cpp"), "\n// see %s\n", 1212)
        ok &= _planted("13 NEW-CITATION-IN-REALEX",
                       ("real-examples", "c", "sqlite", "base-harness.sh"),
                       "\n# see %s\n", 1313)
        # ★ AN EXTENSION, NOT A ROOT: `.github` was already taken and its
        # workflows were still unread, so this arm answers `CODE_EXTS`, not
        # `CODE_ROOTS`. ✔It reds only because `.yml` was added -- and because
        # these files carry ZERO citations today, NOTHING else would notice if
        # the suffix were dropped again.
        ok &= _planted("14 NEW-CITATION-IN-YAML",
                       (".github", "workflows", "dco.yml"), "\n# see %s\n", 1414)

        # ★★★ THE CONTINUATION MATCHER, PINNED AT THE COUNT AND NOT AT AN EXIT
        # CODE — because its two properties pull in OPPOSITE directions and a
        # ratchet arm cannot tell 3 from 2. A bare `:<line>` after a citation IS
        # a citation (undercounting is the defect these arms exist for: 35 of
        # them were invisible across 19 documents before 2026-08-20), while a
        # `:<col>` after a `:<line>` is NOT a second one (overcounting would
        # silently inflate every figure the inventory publishes). `count_in` is
        # the only place either question is decided, so it is what is asked.
        #
        # ⚠ The probe lives in its OWN temp directory, not in the replica: a
        # stray file under the replica root is inside the census's own sweep,
        # and an arm that changes what a LATER arm measures is a self-test that
        # reports on itself.
        def _matcher(label, body, expect_n):
            if _RAN is not None:
                _RAN.append(label)
            box = tempfile.mkdtemp(prefix="plan-citations-matcher-")
            try:
                probe = os.path.join(box, "probe.md")
                io.open(probe, "w", encoding="utf-8", newline="").write(body)
                got = count_in(probe)
            finally:
                shutil.rmtree(box, ignore_errors=True)
            good = got == expect_n
            print("plan-citations: self-test arm %-24s counted %d %s"
                  % (label, got, "as expected"
                     if good else "-- EXPECTED %d" % expect_n))
            return good

        ok &= _matcher(
            "15 CONTINUATION-COUNTED",
            "see %s/%s/%s\n" % (_FIXTURE % 1515, ":%d" % 1516, ":%d" % 1517), 3)
        ok &= _matcher(
            "16 LINE-COL-NOT-DOUBLED",
            "see %s%s\n" % (_FIXTURE % 1616, ":%d" % 24), 1)

        # the scan collapses -- once per FAMILY, because one floor covering both
        # would let the code family vanish behind the markdown family's size.
        held = tempfile.mkdtemp(prefix="plan-citations-held-")
        shutil.move(os.path.join(tmp, ".plans"), os.path.join(held, ".plans"))
        ok &= _arm("5 SCAN-COLLAPSED", tmp, EXIT_COLLAPSE, says="governed document(s)")
        shutil.move(os.path.join(held, ".plans"), os.path.join(tmp, ".plans"))

        # ⚠ AND ONCE PER DOCUMENTATION ROOT, not once per family. ✔MEASURED
        # 2026-08-20: `.plans` is 41 of the 73 governed documents and `.claude`
        # is 32, so a scan that loses only the LARGER root still enumerates 32
        # while one that loses only the smaller still enumerates 41. Arm 5 covers
        # the first; at the former floor of 40 the second passed as CLEAN, and
        # this arm is what holds `DOC_FLOOR` above it.
        shutil.move(os.path.join(tmp, ".claude"), os.path.join(held, ".claude"))
        ok &= _arm("11 DOC-HALF-COLLAPSED", tmp, EXIT_COLLAPSE,
                   says="governed document(s)", not_says="governed code file(s)")
        shutil.move(os.path.join(held, ".claude"), os.path.join(tmp, ".claude"))

        # ⚠ EVERY code root moves, not just `src`. ✔MEASURED: `src` alone is
        # ~550 of ~2,310 governed files, so holding back one root leaves the
        # others comfortably above the floor and the arm would prove nothing --
        # a collapse arm that cannot collapse is the vacuity this suite exists
        # to refuse.
        for rel_root in CODE_ROOTS:
            if os.path.isdir(os.path.join(tmp, rel_root)):
                shutil.move(os.path.join(tmp, rel_root), os.path.join(held, rel_root))
        ok &= _arm("8 CODE-SCAN-COLLAPSED", tmp, EXIT_COLLAPSE,
                   says="governed code file(s)", not_says="governed document(s)")
        for rel_root in CODE_ROOTS:
            if os.path.isdir(os.path.join(held, rel_root)):
                shutil.move(os.path.join(held, rel_root), os.path.join(tmp, rel_root))
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
