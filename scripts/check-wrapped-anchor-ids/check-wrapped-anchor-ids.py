#!/usr/bin/env python3
# PURPOSE: refuse a NEW anchor id split across a line break, which no grep can ever return.
"""check-wrapped-anchor-ids.py -- refuse a NEW line-wrapped anchor id.

D-ANCHOR-ID-WRAPPED-ACROSS-A-LINE-BREAK-IS-INVISIBLE-TO-EVERY-GREP -- the row
this guard is step 1 of.

★★★ WHY THIS EXISTS, AND WHY IT IS THE ONE FAILURE MODE A FAIL-LOUD PROJECT
CANNOT CATCH BY WATCHING FOR A FAILURE. An anchor id written across a line break
-- the line ends `...D-SOME-ANCHOR-` and the next opens `PART-OF-THE-ID` --
still READS as a citation to a human. But no grep for the JOINED name
will ever return it, so the row loses an inbound link and starts looking
unreferenced. A wrapped id does not FAIL. It DISAPPEARS.

★★ AND THE TWO GUARDS THAT ALREADY EXIST BOTH TOLERATE IT, BY DESIGN.
  * `anchor_registry_guard` RECOVERS a wrapped citation (its `WRAP_JOIN_AWK`
    joins the fragment with the head token of the next line) and then resolves
    the joined name. Recovery, not refusal -- deliberately, because refusing
    would red hundreds of lines of honest text that no author can be held to.
    ⇒ a wrap that joins to a REAL id resolves, and nothing reds. That tolerance
    is exactly why the population accumulated.
  * `check-anchor-balance` reads WHOLE-LINE ids (`ANCHOR_TOKEN` matches within
    one line by construction), so a wrapped citation is simply not there.
⇒ The gap is not "no guard reads wrapped ids"; it is that **nothing counts
them**. This guard is the counter, and its only verb is a RATCHET.

★★★ WHAT THIS GUARD IS **NOT**, and the distinction is load-bearing.
It does NOT tighten `check-anchor-registry` to whole-token resolution. That
would red every wrapped citation in the tree at once, with no ratchet, and would
break a tolerance the gate reference calls load-bearing. The subject here is the
COUNT, which may only fall.

── THE GOVERNED SET, STATED EXPLICITLY (a guard whose scope is implicit drifts) ─
Every file `git ls-files --cached --others --exclude-standard` reports: each
file the repository tracks, plus each untracked file it does not ignore.
  * WHY GIT AND NOT A ROOT LIST: the repository already owns a definition of
    what it contains, and `.gitignore` already excludes `build/`, `scratchpad/`,
    `test-scratch/` and `.temp/`. A root list inside this guard would be a
    SECOND definition that drifts away from the first -- the duplicated-fact
    shape this repository closes everywhere else. It also means a lane's brand
    new, not-yet-added file is scanned (`--others`), so a wrap cannot land by
    arriving before its `git add`.
  * IT IS A STRICT SUPERSET of the seven roots `check-anchor-registry` governs
    (`src`, `examples`, `tests`, `integrated_tests`, `real-examples`, `scripts`,
    `.claude`), plus `.plans/` -- where the rows cross-reference each other and
    a wrap is equally invisible -- plus the repo-root files, one of which
    (`CMakeLists.txt`) carries a wrap today.
  * NO EXTENSION FILTER. ✔MEASURED 2026-08-23: excluding the six binary files
    git tracks (`.png`/`.jpg`/`.so`/`.bin`) changes the census by ZERO, so the
    filter would be an enumeration that buys nothing and can only go stale.
    Files are decoded with `errors="replace"`, so a binary cannot abort the scan.

── THE KEY SET, STATED EXPLICITLY ───────────────────────────────────────────────
An anchor id, for this guard, is any id the plans DECLARE or CARRY:
  (a) the row name of every data row of every recognized anchor table under
      `.plans/` -- taken from `check-anchor-balance.scan_worktree`, the same
      population its own opener-resolution arm resolves against; UNION
  (b) every `check-anchor-balance.ANCHOR_TOKEN` match in every `.plans/*.md`.
BOTH halves are IMPORTED from `check-anchor-balance`, never re-implemented: a
second copy of "what an anchor id looks like" is how two instruments start
disagreeing about the same tree.
  * WHY (b) IS NEEDED ON TOP OF (a): ✔MEASURED 2026-08-23 -- the registry row
    whose anchor cell reads `` `D-LK4-DATA-PRODUCER` / `D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL` ``
    declares TWO ids in one cell, and `row_name` returns only the first. Its
    sibling is cited (wrapped) in `src/asm/asm.cpp` and in
    `src/hir/lowering/cst_to_hir.cpp`; without (b) those two sites are invisible
    to the instrument built to see invisible sites.
  * WHY MEMBERSHIP IS TESTED AT ALL: it is what separates a real citation from a
    hyphenated English word that happens to break across lines. The join is only
    a finding when the joined string IS an anchor id.

── THE JOIN RULE, AND WHERE IT COMES FROM ───────────────────────────────────────
Transcribed from `check-anchor-registry.sh`'s `WRAP_JOIN_AWK`, whose two
sub-rules were themselves DERIVED FROM THE TREE rather than invented:
  * the trailing fragment must be anchor-shaped AND preceded by a non-word
    character (the awk spelling of `\\<`), and the search LOOPS -- the leftmost
    `D-` on a line can be an inner fragment of a longer word while a genuine
    anchor sits later on the same line;
  * the continuation is found by dropping every leading NON-ALPHANUMERIC byte
    (comment markers, indentation, box drawing) and then requiring the first
    surviving character to be UPPER CASE -- a continuation that resumes in lower
    case is prose, not a name.
★ THE ONE PLACE THIS GUARD GOES FURTHER, AND IT IS NOT SPECULATIVE: the join is
**N-line**, not two-line. `WRAP_JOIN_AWK` has a one-line lookahead and is blind
to an id broken across THREE lines. ✔MEASURED 2026-08-23: two such sites exist
(`src/analysis/semantic/semantic_analyzer.cpp` splitting
`D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME` three ways, and
`src/core/types/object_format_kind.hpp` splitting
`D-FFI-ABI-CATALOG-SELECTS-CALLING-CONVENTION-BY-FORMAT-IDENTITY`), and the
two-line form reports them as unresolvable fragments instead of as wraps.
The walk continues only while the line it just consumed ALSO ends mid-name, so
it is self-limiting; the joined string must still be an exact key to fire.

── FAIL-CLOSED ──────────────────────────────────────────────────────────────────
An empty scan is a COLLAPSE, not a pass (D-GATE-ANCHOR-GUARD-FAILS-OPEN-ON-MISSING-ROOT):
too few files scanned, or too few keys harvested, is a refusal with exit 2. So is
a missing inventory -- without it every count is unconstrained and this guard
asserts nothing.

── NO LINE NUMBERS, ANYWHERE, INCLUDING IN THIS GUARD'S OWN OUTPUT ─────────────
A finding names the file, the joined id, and the text on both sides of the break.
That is enough to find it, and it stays true when the file moves. The operator
rule (2026-08-19) binds a guard's output for the same reason it binds a comment:
a positional reference that silently becomes wrong still reads as evidence.

── NO `.ps1` TWIN ───────────────────────────────────────────────────────────────
A `.py` already runs on both hosts, and D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED
(closed by operator ruling 2026-08-19) records that a twin is a second
implementation of something that was never split. Eight of this repository's
Python guards have no twin for the same reason.

Usage:
    python scripts/check-wrapped-anchor-ids/check-wrapped-anchor-ids.py
    python scripts/check-wrapped-anchor-ids/check-wrapped-anchor-ids.py --write
    python scripts/check-wrapped-anchor-ids/check-wrapped-anchor-ids.py --baseline
    python scripts/check-wrapped-anchor-ids/check-wrapped-anchor-ids.py --selftest

★★ THE NO-ARGUMENT FORM (the ctest form) VERIFIES THE TREE **AND THEN RUNS THE
SELF-TEST**, honouring both statuses. ✔MEASURED 2026-08-22 on
`enum_name_table_guard`: a guard whose `main()` self-tests only behind a flag,
registered by an entry that passes no flag, proves nothing while a comment three
lines above claims it does (D-GATE-ENUM-NAME-TABLE-CTEST-FORM-NEVER-SELF-TESTED).

★★ TWO WRITE VERBS, AND THE SPLIT IS THE WHOLE SAFETY PROPERTY.
  * `--write` is the BURN-DOWN verb and it CAN ONLY LOWER: it refuses to raise a
    ceiling or to introduce a file. That is what stops the tool a lane reaches
    for after un-wrapping from silently laundering a regression it was pointed
    at. (A rename that carries wraps therefore needs a hand edit of the JSON,
    which is visible in review.)
  * `--baseline` establishes NEW ground unconditionally. It exists because a
    strictly-lowering tool cannot write the first inventory, and a guard whose
    baseline no in-repo tool can regenerate is a guard one lost file away from
    being un-rebuildable. It prints a loud warning and is meant to be reviewed
    AS A DIFF -- every raised ceiling is visible there.
⚠ Do NOT reach for `--baseline` to make a red go away. It is the bootstrap verb,
not the burn-down verb, and using it that way is exactly the ratchet defeat the
split exists to prevent.

Exit codes: 0 clean · 1 a new wrap / a stale inventory · 2 the scan collapsed.
"""

import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

EXIT_OK, EXIT_RATCHET, EXIT_COLLAPSE = 0, 1, 2

INVENTORY_REL = os.path.join("scripts", "check-wrapped-anchor-ids", "inventory.json")

# Floors catch a COLLAPSED scan, never drift. Set far below the live figures
# (2605 files / 2427 keys ✔MEASURED 2026-08-23) so ordinary churn cannot trip them.
FILE_FLOOR = 1500
KEY_FLOOR = 1000


# ── output encoding ─────────────────────────────────────────────────────────
# ⚠ NOT COSMETIC. This guard prints SOURCE TEXT, and this tree's comments carry
# box-drawing and typographic characters. On a Windows console `sys.stdout` is
# cp1252, and printing `│` raises UnicodeEncodeError -- which kills the guard
# WHILE IT IS REPORTING A FINDING. The run still reds, so nothing is silent, but
# the finding itself is lost and the traceback names the print, not the wrap.
# ✔MEASURED while censusing this population: the first census run died exactly
# this way on `.plans/16-codesign-publish-plan - tbd.md`.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):        # pragma: no cover - old/odd stream
        pass


class Collapse(Exception):
    """The scan could not be trusted. Distinct from a finding."""


# ── the join rule (provenance in the module docstring) ──────────────────────
_TRAILING_FRAGMENT = re.compile(r"D-[A-Z0-9_]+(?:-[A-Z0-9_]+)*$")
_CONTINUATION = re.compile(r"^[A-Z0-9_]+(?:-[A-Z0-9_]+)*")
_LEADING_NON_WORD = re.compile(r"^[^A-Za-z0-9_]+")
_WORD_CHAR = re.compile(r"[A-Za-z0-9_]")


def wrap_prefix(line):
    """The anchor-shaped fragment a line ENDS mid-name with, or "".

    ★ THE LOOP IS LOAD-BEARING, and it is not defensive coding: the leftmost
    `D-` on a line can sit inside a longer hyphenated word while a genuine
    anchor fragment ends the line. Returning on the first word-boundary failure
    would miss it. Same reasoning, same shape, as `WRAP_JOIN_AWK::wrapPrefix`.
    """
    s = line.rstrip(" \t")
    if not s.endswith("-"):
        return ""
    s = s[:-1]
    offset, tail = 0, s
    while True:
        m = _TRAILING_FRAGMENT.search(tail)
        if not m:
            return ""
        start = offset + m.start()
        if start == 0 or not _WORD_CHAR.match(s[start - 1]):
            return s[start:]
        offset, tail = start + 1, s[start + 1:]


def continuation(line):
    """The head anchor token of a continuation line, or "".

    Every leading NON-ALPHANUMERIC byte is dropped first -- `// `, `# `, bare
    indentation and `│   │   #    ` box drawing all appear in this tree -- and
    the first surviving character must then be upper case, which is what keeps
    ordinary prose out.
    """
    m = _CONTINUATION.match(_LEADING_NON_WORD.sub("", line))
    return m.group(0) if m else ""


def wraps_in(lines, keys):
    """-> [(joined_id, opening_text, continuation_texts)] for one file's lines.

    Walks forward while each consumed line ALSO ends mid-name, so a three-line
    (or longer) split joins. Self-limiting: the walk stops at the first line
    that does not end in `-`, and the joined string must be an exact key.
    """
    found = []
    for i, line in enumerate(lines):
        prefix = wrap_prefix(line)
        if not prefix:
            continue
        acc, j, parts = prefix, i + 1, []
        while j < len(lines):
            cont = continuation(lines[j])
            if not cont:
                break
            parts.append(lines[j])
            joined = acc + "-" + cont
            if joined in keys:
                found.append((joined, line, list(parts)))
                break
            if not lines[j].rstrip(" \t").endswith("-"):
                break
            acc, j = joined, j + 1
    return found


# ── the key set: IMPORTED, never re-derived ─────────────────────────────────
def _load_anchor_balance(root):
    """`check-anchor-balance` as a module, or a loud death.

    Imported rather than copied for the reason `check-wall-clock-in-tests`
    imports its stripper from `check-no-abort-in-tests`: two copies of one
    definition is exactly the drift this whole registry discipline exists to
    stop. The import fails LOUD if the sibling moves.
    """
    sibling = os.path.join(root, "scripts", "check-anchor-balance",
                           "check-anchor-balance.py")
    if not os.path.isfile(sibling):
        raise Collapse(
            "cannot find the shared anchor vocabulary at %s.\n"
            "  This guard must take its key set and its anchor-token pattern from "
            "that script, or the two instruments start disagreeing about what an "
            "anchor id IS. Restore the sibling; do NOT copy its definitions here."
            % os.path.relpath(sibling, root).replace("\\", "/"))
    spec = importlib.util.spec_from_file_location("_anchor_balance", sibling)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def anchor_key_set(root):
    """Every anchor id the plans DECLARE (a) or CARRY (b). See the docstring."""
    ab = _load_anchor_balance(root)
    keys = set(n.split("#", 1)[1] for n in ab.scan_worktree(root).names)
    plans = os.path.join(root, ab.PLANS_DIR)
    for name in sorted(os.listdir(plans)):
        if not name.endswith(".md"):
            continue
        with io.open(os.path.join(plans, name), encoding="utf-8",
                     errors="replace") as fh:
            keys |= set(ab.ANCHOR_TOKEN.findall(fh.read()))
    keys = set(k for k in keys if k.startswith("D-"))
    if len(keys) < KEY_FLOOR:
        raise Collapse(
            "harvested only %d anchor id(s) from %s, below the floor of %d.\n"
            "  This does NOT mean the tree is clean -- it means THE KEY HARVEST "
            "COLLAPSED, and with no keys every join is silently a non-finding."
            % (len(keys), ab.PLANS_DIR, KEY_FLOOR))
    return keys


# ── the governed set ────────────────────────────────────────────────────────
def governed_files(root):
    p = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=root, capture_output=True, text=True, encoding="utf-8",
        errors="replace")
    if p.returncode != 0:
        raise Collapse("`git ls-files` failed in %s: %s\n"
                       "  The governed set IS the git listing; without it this guard "
                       "would scan an unknown subset and report a pass over it."
                       % (root, (p.stderr or "").strip()[:200]))
    files = [f for f in p.stdout.split("\0") if f]
    if len(files) < FILE_FLOOR:
        raise Collapse(
            "git listed only %d file(s), below the floor of %d.\n"
            "  This does NOT mean the tree is clean -- it means THIS SCAN COLLAPSED. "
            "Fix the scan; do not lower the floor." % (len(files), FILE_FLOOR))
    return files


def census(root, files, keys):
    """-> {relpath: [(joined_id, opening_line, [continuation_lines])]}"""
    per_file = {}
    for rel in files:
        try:
            with io.open(os.path.join(root, rel), encoding="utf-8",
                         errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue                      # deleted between the listing and the read
        if "D-" not in text:              # cheap reject; the join needs a `D-`
            continue
        hits = wraps_in([ln.rstrip("\r") for ln in text.split("\n")], keys)
        if hits:
            per_file[rel] = hits
    return per_file


# ── the inventory (per-file ceilings; DOWN only) ────────────────────────────
#
# ★★★ THIS LITERAL IS THE INVENTORY'S SOURCE OF TRUTH, AND THAT IS WHY A CLAIM
# ROTTING **HERE** IS WORSE THAN ONE ROTTING IN AN ORDINARY COMMENT: whatever
# these strings say, `--write` STAMPS ONTO `inventory.json`. A correction made in
# the JSON alone is undone by the next burn-down, silently, with rc=0.
# ⚠⚠ THAT IS NOT HYPOTHETICAL -- IT IS WHAT THIS BLOCK DID, AND THE RECORD IS
# KEPT RATHER THAN QUIETLY DELETED BECAUSE THE SHAPE IS THE REUSABLE PART.
# The last two elements formerly read *"Burn-down tracked by <this file's row>,"*
# / *"which stays OPEN until `ceilings` is empty."* -- two list elements, one
# sentence. It was true when it was typed. Then the burn-down landed, `ceilings`
# went empty, the row's status cell was marked closed on 2026-08-24, and the
# sentence became FALSE while still reading as a live blocker; the on-disk
# `inventory.json` was reworded to match reality and this literal was not, so the
# two DISAGREED and `--write` stood ready to restore the stale text over the
# corrected one. Species record:
# D-COMMENT-A-CLAIM-TRUE-WHEN-TYPED-AND-FALSE-WHEN-THE-COMMIT-LANDED.
# ★ THE REPAIR IS A CLAIM THAT CANNOT ROT AGAIN, not a fresher status word: the
# text below states the RULE (empty means zero headroom) and names the row as the
# species record rather than asserting anything about its status. A status word
# in prose is a measurement with no instrument attached -- the instrument is
# `python scripts/check-anchor-balance/check-anchor-balance.py`, and the row's own
# status cell is the answer.
_INVENTORY_COMMENT = [
    "Per-file ceilings for WRAPPED anchor ids -- an id split across a line break.",
    "A wrapped id does not fail, it DISAPPEARS: no grep returns it and no anchor",
    "guard counts it. A ceiling may only come DOWN. Un-wrap a site (put the id on",
    "one line of its own) and lower the number in the SAME commit; the guard prints",
    "the new value. Raising an entry, or adding a file, is a FAILURE -- that is the",
    "ratchet. This file is DEBT, not a pass: green means no NEW wrap landed.",
    "Burn-down tracked by D-ANCHOR-ID-WRAPPED-ACROSS-A-LINE-BREAK-IS-INVISIBLE-TO-EVERY-GREP.",
    "EMPTY as of cycle P29: 290 sites across 145 files were un-wrapped. Green now means ZERO, so a single new wrap reds immediately -- there is no headroom left to hide one in.",
]


def load_inventory(root):
    path = os.path.join(root, INVENTORY_REL)
    if not os.path.isfile(path):
        raise Collapse(
            "the inventory %s does not exist. Without it every count is "
            "unconstrained and this guard asserts nothing."
            % INVENTORY_REL.replace("\\", "/"))
    with io.open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)["ceilings"]


def write_inventory(root, ceilings):
    path = os.path.join(root, INVENTORY_REL)
    body = {"_comment": _INVENTORY_COMMENT,
            "ceilings": dict(sorted(ceilings.items()))}
    tmp = path + ".tmp"
    with io.open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(body, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    os.replace(tmp, path)


def _site_text(opening, conts):
    """The break, rendered without a line number. Trimmed, never truncated blind."""
    out = ["      ...%s" % opening.strip()[-72:]]
    for c in conts:
        out.append("      %s..." % c.strip()[:72])
    return "\n".join(out)


def run(root, write, baseline=False):
    keys = anchor_key_set(root)
    files = governed_files(root)
    now = census(root, files, keys)
    counts = dict((rel, len(hits)) for rel, hits in now.items())

    if baseline:
        write_inventory(root, counts)
        print("wrapped-anchor-ids: BASELINED %d file(s), %d wrap site(s) over %d "
              "governed file(s) and %d anchor id(s)."
              % (len(counts), sum(counts.values()), len(files), len(keys)))
        print("  ⚠ This establishes NEW ground and can RAISE ceilings. Review the "
              "diff: every raised number is a wrap that is now permitted. Use "
              "`--write` for burn-down; it can only lower.")
        return EXIT_OK

    if write:
        ceilings = load_inventory(root)
        raised = sorted(f for f, n in counts.items() if n > ceilings.get(f, 0))
        if raised:
            print("wrapped-anchor-ids: REFUSING to re-baseline -- %d file(s) are ABOVE "
                  "their ceiling:" % len(raised), file=sys.stderr)
            for f in raised:
                print("    %s: ceiling %d, actual %d" % (f, ceilings.get(f, 0),
                                                         counts[f]), file=sys.stderr)
            print("  `--write` is a burn-down tool: it may only LOWER. Un-wrap the new "
                  "site instead. (A rename that carries wraps needs a hand edit of the "
                  "JSON, which is visible in review.)", file=sys.stderr)
            return EXIT_RATCHET
        write_inventory(root, counts)
        print("wrapped-anchor-ids: re-baselined %d file(s), %d wrap site(s)"
              % (len(counts), sum(counts.values())))
        return EXIT_OK

    ceilings = load_inventory(root)
    new, stale = [], []
    for rel in sorted(counts):
        ceiling = ceilings.get(rel, 0)
        if counts[rel] > ceiling:
            new.append((rel, ceiling, counts[rel]))
    for rel in sorted(ceilings):
        if counts.get(rel, 0) < ceilings[rel]:
            stale.append((rel, ceilings[rel], counts.get(rel, 0)))

    if new:
        print("wrapped-anchor-ids: FAIL - a NEW line-wrapped anchor id landed:",
              file=sys.stderr)
        for rel, ceiling, n in new:
            print("    %s: %d wrapped id(s), inventory allows %d" % (rel, n, ceiling),
                  file=sys.stderr)
            for joined, opening, conts in now[rel][ceiling:]:
                print("      -> %s" % joined, file=sys.stderr)
                print(_site_text(opening, conts), file=sys.stderr)
        print("  A split id still READS as a citation, but no grep for the WHOLE id "
              "will ever return it, and neither anchor guard can count it. It does "
              "not fail; it disappears.", file=sys.stderr)
        print("  FIX: reflow the comment so the id sits WHOLE on one line of its own. "
              "Break the line BEFORE the id or AFTER it, never inside it.",
              file=sys.stderr)
        print("  Do NOT raise the ceiling to make this pass - the ceiling only ever "
              "comes DOWN. That is the whole point.", file=sys.stderr)
        return EXIT_RATCHET

    if stale:
        print("wrapped-anchor-ids: FAIL - the inventory is STALE and now grants unused "
              "headroom:", file=sys.stderr)
        for rel, ceiling, n in stale:
            print("    %s: %d wrapped id(s) now, ceiling still says %d -> lower it to "
                  "%d%s" % (rel, n, ceiling, n,
                            " (or delete the entry)" if n == 0 else ""), file=sys.stderr)
        print("  You un-wrapped sites without lowering the ceiling. Unclaimed headroom "
              "is exactly where the next wrap hides. Re-baseline in the same commit:",
              file=sys.stderr)
        print("      python scripts/check-wrapped-anchor-ids/check-wrapped-anchor-ids.py "
              "--write", file=sys.stderr)
        return EXIT_RATCHET

    total = sum(counts.values())
    if total:
        print("wrapped-anchor-ids: OK (%d file(s) scanned against %d anchor id(s); %d "
              "wrapped id(s) across %d file(s), all within the inventory ratchet). "
              "DEBT, not a pass - see "
              "D-ANCHOR-ID-WRAPPED-ACROSS-A-LINE-BREAK-IS-INVISIBLE-TO-EVERY-GREP."
              % (len(files), len(keys), total, len(counts)))
    else:
        print("wrapped-anchor-ids: OK (%d file(s) scanned against %d anchor id(s); 0 "
              "wrapped ids)" % (len(files), len(keys)))
    return EXIT_OK


# ═══════════════════════════ SELF-TEST ═══════════════════════════════════════
#
# ★★★ THE FOUR CASES THE ROW DEMANDS ARE DRIVEN THROUGH THE **REAL** SCAN, NOT
# THROUGH THE MATCHER, and their fixtures are SYNTHESIZED AT RUN TIME in a temp
# directory that is created and deleted here.
# ⛔ NO ON-DISK WRAPPED FIXTURE EXISTS ANYWHERE IN THE GOVERNED SET, and that is a
# decision rather than an omission: a fixture wrap is a deliberate wrap, the
# burn-down lane would un-wrap it and silently break this self-test, and the
# ratchet would have to carry the fixture forever. Synthesizing removes the
# interaction entirely.
# ★ EVERY RED ARM ASSERTS THE MESSAGE, not merely a non-zero exit: this guard has
# two distinct failures that share exit 1 (a new wrap / a stale ceiling), and an
# arm that checks only the code cannot tell which one it proved. That exact
# mistake was measured in a sibling guard.

EXPECTED_ARMS = 35


def _tmp_repo(files, ceilings):
    """A throwaway git repo with `files` and an inventory. Returns its path."""
    root = tempfile.mkdtemp(prefix="wrapped-anchor-ids-selftest-")
    subprocess.run(["git", "init", "-q"], cwd=root, capture_output=True)
    for rel, text in files.items():
        full = os.path.join(root, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with io.open(full, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    if ceilings is not None:
        inv = os.path.join(root, INVENTORY_REL)
        os.makedirs(os.path.dirname(inv), exist_ok=True)
        with io.open(inv, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"_comment": _INVENTORY_COMMENT,
                       "ceilings": dict(sorted(ceilings.items()))}, fh, indent=2)
    return root


def _run_capture(root, write=False, baseline=False):
    """`run()` against a synthetic root, with stdout+stderr captured."""
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        try:
            rc = run(root, write, baseline)
        except Collapse as exc:
            print("wrapped-anchor-ids: FATAL - %s" % exc)
            rc = EXIT_COLLAPSE
    return rc, buf.getvalue()


def selftest():
    bad = 0
    arms = 0

    def check(label, cond):
        nonlocal bad, arms
        arms += 1
        if not cond:
            bad += 1
        print("  [%s] %s" % ("ok " if cond else "FAIL", label))

    # ── A. the matcher, in isolation ────────────────────────────────────────
    # ⚠⚠ EVERY SYNTHETIC NAME HERE IS DELIBERATELY **THREE SEGMENTS**, WHICH IS
    # UNDER `anchor_registry_guard`'s COLLECTION THRESHOLD, AND THAT IS NOT
    # COSMETIC. A four-segment `D-…` literal in this file is a citation of an
    # anchor with no row, inside the guard that reports wrapped citations -- the
    # same booby-trap `check-anchor-registry`'s own header records catching in
    # its `.ps1` twin, where it blocked `scripts/` from being scanned at all.
    # ✔MEASURED: with this file's first-draft names, that guard reported FIVE
    # unresolvable anchors, every one of them a fixture.
    # ★ `_LONG` is ASSEMBLED rather than written, because the three-line fixture
    # NEEDS four segments and there is no spelling of that which is also inert.
    # No grep sees the whole token; the fixture builds it at run time.
    _LONG = "D-XX" + "-P" + "-Q" + "-R"
    KEYS = {"D-XX-FOO", "D-A-B", _LONG}

    def hits(text, keys=KEYS):
        return wraps_in([l.rstrip("\r") for l in text.split("\n")], keys)

    check("a wrapped id joins across two lines",
          [h[0] for h in hits("// see D-XX-\n// FOO for the rest\n")]
          == ["D-XX-FOO"])
    check("a THREE-line split joins (WRAP_JOIN_AWK's one-line lookahead cannot)",
          [h[0] for h in hits("// D-XX-\n// P-Q-\n// R.\n")] == [_LONG])
    check("the SAME id unwrapped is not a finding",
          hits("// see D-XX-FOO for the rest\n") == [])
    check("a hyphenated English word split across lines is not a finding",
          hits("the well-\nKNOWN case\n") == [])
    check("a D-...- split whose join is not a key is not a finding",
          hits("// D-XX-\n// QUUX\n") == [])
    check("a continuation resuming in LOWER case is prose, not a name",
          hits("// D-XX-\n// baz\n") == [])
    check("box-drawing / comment junk before the continuation is stripped",
          [h[0] for h in hits("#  D-XX-\n│   │   #    FOO\n")]
          == ["D-XX-FOO"])
    check("a line NOT ending in `-` never starts a join",
          hits("// D-XX-FOO\n// FOO\n") == [])
    check("the word-boundary test refuses a fragment inside a longer word",
          hits("// XD-XX-\n// FOO\n") == [])
    check("the loop finds a real fragment after a false leading `D-`",
          [h[0] for h in hits("// AD-NOPE and D-XX-\n// FOO\n")]
          == ["D-XX-FOO"])
    check("a trailing CR does not hide the break (CRLF file)",
          [h[0] for h in hits("// D-XX-\r\n// FOO\r\n")] == ["D-XX-FOO"])
    check("the opening line and every continuation are reported for the finding",
          hits("// D-XX-\n// FOO\n")[0][1].strip() == "// D-XX-"
          and hits("// D-XX-\n// FOO\n")[0][2][0].strip() == "// FOO")

    # ── B. the four row-mandated cases, through the REAL scan ───────────────
    # A synthetic repo whose `.plans/` declares one anchor, so the key set is
    # real for that tree; the FLOORS are the only thing standing in the way, so
    # they are lowered for the duration by patching the module globals -- the
    # alternative is 1500 synthetic files per arm.
    global FILE_FLOOR, KEY_FLOOR
    saved_floors = (FILE_FLOOR, KEY_FLOOR)
    FILE_FLOOR, KEY_FLOOR = 1, 1
    PLAN = ("| Anchor | Trigger | Closing work | Cross-refs |\n"
            "| --- | --- | --- | --- |\n"
            "| `D-XX-WRAPCASE` | OPEN | w | r |\n"
            "| `D-XX-OTHERCASE` | OPEN | w | r |\n")
    try:
        roots = []

        def synth(body, ceilings={}):
            r = _tmp_repo({".plans/00-synthetic.md": PLAN,
                           "scripts/check-anchor-balance/check-anchor-balance.py":
                               io.open(os.path.join(REPO, "scripts",
                                                    "check-anchor-balance",
                                                    "check-anchor-balance.py"),
                                       encoding="utf-8").read(),
                           "src/subject.cpp": body}, ceilings)
            roots.append(r)
            return r

        rc, out = _run_capture(synth("// a wrapped id: D-XX-\n// WRAPCASE ends it\n"))
        check("(1) a SYNTHETIC wrapped id in the governed set goes RED",
              rc == EXIT_RATCHET)
        check("(1) ... and the finding NAMES the joined id and the file",
              "D-XX-WRAPCASE" in out and "src/subject.cpp" in out)
        check("(1) ... and says what a wrapped id does",
              "disappears" in out)
        check("(1) ... and cites NO line number for the site",
              not re.search(r"src/subject\.cpp:\d", out))

        rc, out = _run_capture(synth("// a whole id: D-XX-WRAPCASE ends it\n"))
        check("(2) the SAME id UNWRAPPED is GREEN", rc == EXIT_OK)
        check("(2) ... and the green line reports a real scan",
              "wrapped-anchor-ids: OK" in out and "0 wrapped ids" in out)

        rc, out = _run_capture(synth("the well-\nKNOWN english case\n"))
        check("(3) a hyphenated English word split across lines is GREEN",
              rc == EXIT_OK)

        rc, out = _run_capture(synth("// D-XX-\n// NOTAKEY here\n"))
        check("(4) a `D-...-` split whose join is NOT an anchor id is GREEN",
              rc == EXIT_OK)

        # ── C. the ratchet, both directions ─────────────────────────────────
        body2 = ("// D-XX-\n// WRAPCASE one\n"
                 "// D-XX-\n// OTHERCASE two\n")
        rc, out = _run_capture(synth(body2, {"src/subject.cpp": 2}))
        check("a wrap AT its ceiling is GREEN and reported as DEBT",
              rc == EXIT_OK and "DEBT, not a pass" in out)
        rc, out = _run_capture(synth(body2, {"src/subject.cpp": 1}))
        check("one wrap OVER the ceiling is RED", rc == EXIT_RATCHET)
        check("... naming it as a NEW wrapped id", "a NEW line-wrapped anchor id" in out)
        rc, out = _run_capture(synth(body2, {"src/subject.cpp": 3}))
        check("a ceiling ABOVE the live count is RED (unclaimed headroom)",
              rc == EXIT_RATCHET and "STALE" in out)
        rc, out = _run_capture(synth("// nothing here\n", {"src/subject.cpp": 1}))
        check("a ceiling for a file with NO wraps left is RED, and says to delete it",
              rc == EXIT_RATCHET and "or delete the entry" in out)

        # ── D. `--write` may only LOWER ─────────────────────────────────────
        r = synth(body2, {"src/subject.cpp": 1})
        rc, out = _run_capture(r, write=True)
        check("`--write` REFUSES to raise a ceiling", rc == EXIT_RATCHET
              and "REFUSING to re-baseline" in out)
        r = synth(body2, {"src/subject.cpp": 5})
        rc, out = _run_capture(r, write=True)
        check("`--write` lowers a stale ceiling", rc == EXIT_OK
              and "re-baselined" in out)
        check("... and the lowered ceiling is what the tree actually holds",
              json.load(io.open(os.path.join(r, INVENTORY_REL), encoding="utf-8"))
              ["ceilings"] == {"src/subject.cpp": 2})
        r = synth(body2, {"src/subject.cpp": 1})
        rc, out = _run_capture(r, baseline=True)
        check("`--baseline` DOES raise, and says so loudly", rc == EXIT_OK
              and "BASELINED" in out and "can RAISE ceilings" in out)
        check("... writing exactly the live census",
              json.load(io.open(os.path.join(r, INVENTORY_REL), encoding="utf-8"))
              ["ceilings"] == {"src/subject.cpp": 2})

        # ── E. fail-closed ──────────────────────────────────────────────────
        r = synth(body2, None)
        rc, out = _run_capture(r)
        check("a MISSING inventory is a COLLAPSE, never a pass",
              rc == EXIT_COLLAPSE and "does not exist" in out)

        FILE_FLOOR = 10 ** 6
        rc, out = _run_capture(synth(body2, {"src/subject.cpp": 2}))
        check("a file count below the FLOOR is a COLLAPSE, not a clean tree",
              rc == EXIT_COLLAPSE and "below the floor" in out)
        FILE_FLOOR = 1
        KEY_FLOOR = 10 ** 6
        rc, out = _run_capture(synth(body2, {"src/subject.cpp": 2}))
        check("a key harvest below the FLOOR is a COLLAPSE, not a clean tree",
              rc == EXIT_COLLAPSE and "KEY HARVEST" in out)
        KEY_FLOOR = 1

        r = _tmp_repo({".plans/00-synthetic.md": PLAN,
                       "src/subject.cpp": body2}, {"src/subject.cpp": 2})
        roots.append(r)
        rc, out = _run_capture(r)
        check("a MISSING check-anchor-balance is a COLLAPSE (the import fails loud)",
              rc == EXIT_COLLAPSE and "shared anchor vocabulary" in out)
    finally:
        FILE_FLOOR, KEY_FLOOR = saved_floors
        for r in roots:
            shutil.rmtree(r, ignore_errors=True)
        # ★ Assert the RESTORE succeeded: a self-test that leaves the floors
        # patched turns every later run into a vacuous pass.
        leaked = [r for r in roots if os.path.exists(r)]
        check("every synthetic root was removed and the floors restored",
              not leaked and (FILE_FLOOR, KEY_FLOOR) == saved_floors)

    if arms != EXPECTED_ARMS:
        print("  [FAIL] expected %d arms, ran %d - an arm was dropped or added "
              "without updating EXPECTED_ARMS" % (EXPECTED_ARMS, arms))
        bad += 1
    print("wrapped-anchor-ids selftest: %s (%d arm(s), %d failure(s))"
          % ("FAIL" if bad else "OK", arms, bad))
    return EXIT_RATCHET if bad else EXIT_OK


def repo_root():
    p = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    if p.returncode != 0:
        sys.exit("wrapped-anchor-ids: FATAL - not inside a git repository")
    return p.stdout.strip()


REPO = repo_root()


def main(argv):
    unknown = [a for a in argv
               if a not in ("--write", "--baseline", "--selftest")]
    if unknown:
        print("wrapped-anchor-ids: unknown argument(s): %s" % " ".join(unknown),
              file=sys.stderr)
        return EXIT_COLLAPSE
    if "--selftest" in argv:
        return selftest()
    try:
        rc = run(REPO, "--write" in argv, "--baseline" in argv)
    except Collapse as exc:
        print("wrapped-anchor-ids: FATAL - %s" % exc, file=sys.stderr)
        return EXIT_COLLAPSE
    if "--write" in argv or "--baseline" in argv:
        return rc
    # ★ The no-argument form verifies the TREE and then proves the guard can
    # FAIL. Either half alone is a vacuous pass.
    # ⚠ BOTH HALVES RUN UNCONDITIONALLY -- `return rc or selftest()` would
    # SHORT-CIRCUIT the self-test whenever the tree check reds, so a broken
    # instrument would stay hidden behind exactly the failure it was supposed to
    # be trusted to report. ✔Caught by a red-on-disable mutant whose self-test
    # arms never ran because the same mutant also reddened the tree.
    print()
    rc_self = selftest()
    return rc or rc_self


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
