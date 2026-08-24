#!/usr/bin/env python3
# PURPOSE: refuse a script that no index documents, and an index entry that no script backs.
"""check-scripts-index.py -- the SCRIPT INDEX guard.

★★★ WHY THIS EXISTS, and it is a measured gap rather than a tidiness rule.
Until 2026-08-19 this repository had eighteen scripts and NO index of them: no
`README` in the script root, and nothing in the `/dss-cycle` skill that said what
existed. They were named only piecemeal, across eight different reference files,
wherever some gate step happened to need one. ✔The cost is the ordinary one and
it had already been paid: work gets redone because the tool that already does it
is invisible, and a tool with a defect gets worked around instead of repaired,
because the reader does not know it is a shared tool at all.

★★ THE OPERATOR'S RULE THIS GUARD ENFORCES (2026-08-19): *"if a tool has a
problem, fix before using again, not workaround an own tool. reusable tools
exists to avoid bunch of problems like mangling or edge cases."* An index is how
that rule becomes reachable -- you cannot be told to prefer the existing tool if
nothing lists the existing tools.

★★★ WHY THE INDEX IS GENERATED AND NOT WRITTEN. Two documents describe the same
set from two audiences (the repository's own `scripts/README.md`, and the
cycle-facing reference the `/dss-cycle` skill reads). A hand-written pair is two
copies of one fact, and this project has already measured what that costs -- the
whole `_deferred-anchor-registry` discipline exists because a second copy goes
stale silently. So the fact lives ONCE, in each script's own `PURPOSE:` line, and
both documents are generated from it and verified against it here.
⚠ A free-form header first line could not have been used: three different header
shapes are already in use (a `# name.sh -- ...` comment, a bare module docstring,
and PowerShell `.SYNOPSIS`), so "compare the doc to the header" would have been a
regex against prose. `PURPOSE:` is one grammar, declared once per script.

THE CONTRACT, and every clause is a way the index can lie:
  1. every directory under `scripts/` has a PRIMARY script (`<dir>/<dir>.sh`,
     else `.py`, else `.ps1`) -- a directory that is not addressable by its own
     name is not a script, it is a folder someone left behind;
  2. that primary declares EXACTLY ONE `PURPOSE:` line;
  3. both indexes list exactly the directories that exist -- no missing entry
     (a script nothing documents) and no surplus entry (a document promising a
     script that was deleted);
  4. each entry's purpose text matches the script's own declaration exactly,
     after trimming the whitespace around it, so editing a script's purpose
     without updating the indexes is a red rather than a silent divergence;
  5. a declaration is non-empty, carries no raw `|` (it lands in a markdown
     cell) and no index marker (it lands between them);
  6. a sibling may omit its declaration but may not CONTRADICT the primary's;
  7. the layout holds: no script loose at the top of `scripts/`, none buried in
     a subdirectory -- either is invisible to an index keyed on directories;
  8. each document carries EXACTLY ONE marker pair, so a second, unverified
     index cannot sit below the real one;
  9. the scan has a FLOOR. A guard whose enumeration collapses to nothing
     reports a clean pass over a corpus it never read -- the exact failure this
     repository has measured more than once -- so too few scripts is a refusal.

⚠ Clauses 5-8 exist because an INDEPENDENT AUDIT got the first draft of this
guard to report GREEN over each of them, and clause 9's arm was passing for the
wrong reason. Every refusal now has a self-test arm that asserts the MESSAGE, and
each was verified by sabotage: delete the refusal, and the self-test fails.

Exit codes: 0 OK · 1 index disagrees with the tree · 2 the scan collapsed
(structural failure: fix the scan, never lower the floor) · 3 usage error.

Usage:
    python scripts/check-scripts-index/check-scripts-index.py            # verify
    python scripts/check-scripts-index/check-scripts-index.py --write    # regenerate
    python scripts/check-scripts-index/check-scripts-index.py --selftest # prove it fails
"""
from __future__ import annotations

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile

# ── OUTPUT ENCODING — NOT COSMETIC, AND THE STREAM IS HALF THE FACT ─────────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, BOTH streams PIPES, which is
# exactly how ctest runs every guard): `sys.stdout` comes up
# `encoding='cp1252' errors='surrogateescape'` and `sys.stderr` comes up
# `errors='backslashreplace'`. `surrogateescape` rescues only lone surrogates left
# by an earlier decode; it does NOTHING for an ordinary unencodable character. So a
# report printed on STDOUT — where this guard names every index document that
# disagrees with the tree —
# raises `UnicodeEncodeError` and kills the guard INSIDE ITS OWN REPORT: the run
# still reds, but the finding is lost and the traceback names a `print` rather than
# the thing that was wrong. STDERR merely mangles the glyph into an escape.
# ⚠ Paths are ASCII in this tree TODAY, which makes this prophylactic rather than a
# live red — and one non-ASCII script name away from not being.
# Applied at IMPORT, so every path this module can print on is covered.
# D-GATE-PYTHON-GUARD-DIES-PRINTING-TREE-TEXT-ON-A-WINDOWS-PIPE
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass


# The two documents. Both are generated between the markers and hand-written
# outside them, so the prose that explains the index is never machine-owned.
README_REL = os.path.join("scripts", "README.md")
SKILL_REL = os.path.join(".claude", "skills", "dss-cycle", "references", "scripts.md")
DOC_RELS = (README_REL, SKILL_REL)

BEGIN = "<!-- BEGIN GENERATED SCRIPT INDEX -->"
END = "<!-- END GENERATED SCRIPT INDEX -->"

PURPOSE_MARK = "PURPOSE: "
HEADER_LINES = 40  # a declaration further down than this is not a header

# Far below the live figure (19 on 2026-08-19) so ordinary churn never trips it,
# and high enough that a collapsed enumeration cannot masquerade as a pass.
SCRIPT_FLOOR = 12

SCRIPT_EXTS = (".sh", ".ps1", ".py")

EXIT_OK, EXIT_DISAGREE, EXIT_COLLAPSE, EXIT_USAGE = 0, 1, 2, 3


class Collapse(Exception):
    """The enumeration failed structurally. Never reported as a clean pass."""


class Entry:
    __slots__ = ("name", "primary", "siblings", "purpose")

    def __init__(self, name, primary, siblings, purpose):
        self.name = name
        self.primary = primary
        self.siblings = siblings
        self.purpose = purpose


def repo_root():
    """The checkout root, asked of git rather than derived from `__file__`.

    ★ Deliberate: this script's own depth under the repo is exactly the fact that
    changed on 2026-08-19 when `tools/` was merged into `scripts/`, and every
    guard that derived its root by counting `..` had to be edited by hand. A
    guard that asks git cannot be broken by moving it.
    """
    try:
        p = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                           capture_output=True, text=True, check=False)
    except OSError as exc:
        # Found by audit: an absent git raised through main() as a traceback and
        # exit 1 -- the code reserved for "the index disagrees with the tree",
        # so the ctest log told a maintainer their documents were stale when the
        # truth was that the environment had no git.
        raise Collapse("cannot run git (%s). Pass the repo root as an argument "
                       "instead: check-scripts-index.py <root>" % exc)
    if p.returncode != 0:
        raise Collapse("not inside a git checkout: " + p.stderr.strip())
    return p.stdout.strip()


def primary_script(scripts_dir, name):
    """`<dir>/<dir>.sh`, else `.py`, else `.ps1` -- ONE rule, not a lookup table.

    A per-script table would be a third place to forget to update, which is the
    class of defect this whole guard exists to close.
    """
    for ext in (".sh", ".py", ".ps1"):
        cand = os.path.join(scripts_dir, name, name + ext)
        if os.path.isfile(cand):
            return cand
    return None


def read_purpose(path):
    """The single `PURPOSE:` declaration in a script's header.

    Raises on zero or many: an ambiguous declaration is worse than none, because
    a reader believes the first one they find.
    """
    raw = io.open(path, "r", encoding="utf-8", newline="").read()
    found = []
    for line in raw.split("\n")[:HEADER_LINES]:
        at = line.find(PURPOSE_MARK)
        if at >= 0:
            found.append(line[at + len(PURPOSE_MARK):].strip())
    if not found:
        raise Collapse(
            "%s declares no `%s` line in its first %d lines. Every script "
            "declares its purpose once, in its own header; both indexes are "
            "generated from that declaration." % (path, PURPOSE_MARK.strip(), HEADER_LINES))
    if len(found) > 1:
        raise Collapse(
            "%s declares %d `%s` lines. Exactly one is required -- a reader "
            "believes the first one they find." % (path, len(found), PURPOSE_MARK.strip()))
    purpose = found[0]

    # THREE REFUSALS ON THE TEXT ITSELF, each one found by audit as a way a
    # declaration can satisfy this guard while telling the reader nothing, or
    # telling the DOCUMENT something it cannot survive.
    if not purpose:
        raise Collapse(
            "%s declares an EMPTY purpose. Declaring nothing is not declaring "
            "a purpose -- the byte-identity check would then hold vacuously "
            "against a blank table cell." % path)
    if "|" in purpose:
        raise Collapse(
            "%s declares a purpose containing a raw pipe: %s. The purpose is "
            "interpolated into a markdown table cell, and a stray pipe makes the "
            "renderer SILENTLY DROP every cell after it -- the defect class this "
            "repository already pins as D-PLANS-REGISTRY-ROWS-WITH-EXTRA-CELLS-"
            "STILL-LIVE. Rewrite the sentence; escaping it here would only move "
            "the surprise into the generated document." % (path, purpose))
    for _mark in (BEGIN, END):
        if _mark in purpose:
            raise Collapse(
                "%s declares a purpose containing the generated-index marker %s. "
                "Writing it into the body would create a second marker and the "
                "index would never converge." % (path, _mark))
    return purpose


def scan(root):
    """Every script directory, with its primary, its siblings and its purpose."""
    scripts_dir = os.path.join(root, "scripts")
    if not os.path.isdir(scripts_dir):
        raise Collapse("no scripts/ directory under %s" % root)

    # A SCRIPT THAT IS NOT IN A DIRECTORY OF ITS OWN IS INVISIBLE TO AN INDEX
    # KEYED ON DIRECTORIES -- so it is refused, not skipped. Found by audit:
    # `scripts/loose-tool.sh` passed while documented nowhere, which is exactly
    # the condition this guard's own PURPOSE line says it refuses. A migration
    # is when a stray file gets left at the top.
    loose = sorted(f for f in os.listdir(scripts_dir)
                   if os.path.isfile(os.path.join(scripts_dir, f))
                   and os.path.splitext(f)[1] in SCRIPT_EXTS)
    if loose:
        raise Collapse(
            "script file(s) sit directly under scripts/ instead of in a directory "
            "of their own: %s. The layout is one directory per script "
            "(scripts/<name>/<name>.{sh,ps1,py}); a loose file is indexed by "
            "nothing." % ", ".join(loose))

    names = sorted(n for n in os.listdir(scripts_dir)
                   if os.path.isdir(os.path.join(scripts_dir, n)) and n != "__pycache__")
    entries = []
    for name in names:
        prim = primary_script(scripts_dir, name)
        if prim is None:
            raise Collapse(
                "scripts/%s/ has no primary script (expected %s.sh, %s.py or %s.ps1). "
                "A directory that is not addressable by its own name is not a script."
                % (name, name, name, name))
        here = os.path.join(scripts_dir, name)
        sibs = sorted(f for f in os.listdir(here)
                      if os.path.isfile(os.path.join(here, f))
                      and os.path.splitext(f)[1] in SCRIPT_EXTS)

        # A script buried one level deeper is as invisible as a loose one, and
        # os.listdir was blind to both. Assets in subdirectories stay fine --
        # only executables are refused.
        for sub_dir, _dirs, files in os.walk(here):
            if os.path.abspath(sub_dir) == os.path.abspath(here):
                continue
            buried = sorted(f for f in files if os.path.splitext(f)[1] in SCRIPT_EXTS)
            if buried:
                raise Collapse(
                    "scripts/%s/ buries script(s) in a subdirectory: %s. Siblings "
                    "live BESIDE the primary, not under it -- a buried script is "
                    "indexed by nothing and governed by nothing."
                    % (name, ", ".join(
                        os.path.join(os.path.relpath(sub_dir, here), f).replace(os.sep, "/")
                        for f in buried)))

        purpose = read_purpose(prim)

        # A SIBLING MAY STAY SILENT, BUT IT MAY NOT DISAGREE. Requiring every
        # sibling to repeat the declaration would be sixteen copies of one
        # sentence; letting one CONTRADICT the primary would let a pair this
        # project calls capability-paired describe two different capabilities.
        # Found by audit: a .ps1 twin declaring the opposite purpose passed.
        for sib in sibs:
            sib_path = os.path.join(here, sib)
            if os.path.abspath(sib_path) == os.path.abspath(prim):
                continue
            head = io.open(sib_path, "r", encoding="utf-8", newline="").read()
            if PURPOSE_MARK not in "\n".join(head.split("\n")[:HEADER_LINES]):
                continue
            declared = read_purpose(sib_path)
            if declared != purpose:
                raise Collapse(
                    "scripts/%s/%s declares a purpose that differs from its primary "
                    "(%s).\n    primary : %s\n    sibling : %s\n  A sibling may omit "
                    "the declaration; it may not contradict it."
                    % (name, sib, os.path.basename(prim), purpose, declared))

        entries.append(Entry(name, os.path.relpath(prim, root).replace(os.sep, "/"),
                             sibs, purpose))

    if len(entries) < SCRIPT_FLOOR:
        raise Collapse(
            "found only %d script directories under %s, floor is %d. The "
            "enumeration COLLAPSED -- fix the scan, do not lower the floor."
            % (len(entries), scripts_dir, SCRIPT_FLOOR))
    return entries


def render(entries):
    """The generated table body. Identical in both documents by construction."""
    out = [
        "| Script | Runs as | Purpose |",
        "| --- | --- | --- |",
    ]
    for e in entries:
        runs = ", ".join("`" + s + "`" for s in e.siblings)
        out.append("| **`%s`** | %s | %s |" % (e.name, runs, e.purpose))
    return "\n".join(out)


def splice(doc_text, body, doc_rel):
    b = doc_text.find(BEGIN)
    t = doc_text.find(END)
    if b < 0 or t < 0 or t < b:
        raise Collapse(
            "%s is missing its generated-index markers (%s / %s). Without them "
            "there is nothing to verify and the document silently stops being "
            "an index." % (doc_rel, BEGIN, END))
    # EXACTLY ONE PAIR. `find` takes the FIRST of each, so a second pair appended
    # below the real one is never looked at: found by audit, a fabricated block
    # naming a script that has never existed passed cleanly -- and whether it
    # passed depended on whether the forgery sat above or below the real one.
    if doc_text.count(BEGIN) != 1 or doc_text.count(END) != 1:
        raise Collapse(
            "%s carries %d BEGIN and %d END index markers; exactly one of each is "
            "required. Only the first pair is ever verified, so a second block is "
            "an UNCHECKED index inside a document that claims to be machine-checked."
            % (doc_rel, doc_text.count(BEGIN), doc_text.count(END)))
    return doc_text[:b] + BEGIN + "\n" + body + "\n" + doc_text[t:]


def run(root, write):
    # ★★★ THE GUARD'S OWN CONFIGURATION IS PART OF WHAT IT CHECKS. Both documents
    # are named constants, and dropping either from DOC_RELS would stop verifying
    # it while every message still claimed both were machine-checked. ✔MEASURED
    # by audit: halving this tuple left the whole self-test green. Pinned here
    # rather than in the self-test so it reds on a REAL run too.
    for required in (README_REL, SKILL_REL):
        if required not in DOC_RELS:
            raise Collapse(
                "%s is not in DOC_RELS, so it is no longer verified -- while this "
                "guard, both documents, and the /dss-cycle skill all still say it "
                "is. Verifying fewer documents is a change to the contract, not a "
                "configuration tweak." % required)
    entries = scan(root)
    body = render(entries)
    problems = []
    for rel in DOC_RELS:
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            raise Collapse("index document %s does not exist" % rel)
        cur = io.open(path, "r", encoding="utf-8", newline="").read()
        want = splice(cur, body, rel)
        if cur == want:
            continue
        if write:
            tmp = path + ".tmp"
            with io.open(tmp, "w", encoding="utf-8", newline="") as f:
                f.write(want)
            os.replace(tmp, path)
            print("check-scripts-index: rewrote %s" % rel)
        else:
            problems.append(rel)

    if problems:
        print("check-scripts-index: FAIL -- the index disagrees with the tree in:")
        for rel in problems:
            print("    %s" % rel)
        print("")
        print("  The scripts under scripts/ and the rows in these documents must name the")
        print("  same set, and each row's purpose must be the script's own `PURPOSE:` line.")
        print("  A script that no index documents is a script the next reader re-implements;")
        print("  an entry that no script backs sends them looking for a file that is gone.")
        print("")
        print("  Regenerate with:")
        print("      python scripts/check-scripts-index/check-scripts-index.py --write")
        return EXIT_DISAGREE

    print("check-scripts-index: OK (%d scripts, both indexes agree with the tree "
          "and with each script's own PURPOSE line)" % len(entries))
    return EXIT_OK


# ── RED-ON-DISABLE SELF-TEST ────────────────────────────────────────────────
# ★★★ The guard PROVES it can fail, and it does so on a MIRROR of the tree, never
# on the tree itself: a self-test that mutates the working copy is one crash away
# from leaving a repository in the mutated state. Every arm asserts an EXIT CODE,
# not the absence of an exception -- "it did not throw" is exactly how a guard
# that stopped checking anything reports success.

def _mirror(root, dst):
    os.makedirs(os.path.join(dst, "scripts"), exist_ok=True)
    src_scripts = os.path.join(root, "scripts")
    for name in os.listdir(src_scripts):
        s = os.path.join(src_scripts, name)
        if os.path.isdir(s) and name != "__pycache__":
            shutil.copytree(s, os.path.join(dst, "scripts", name),
                            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    for rel in DOC_RELS:
        d = os.path.join(dst, rel)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(os.path.join(root, rel), d)


_RAN = None   # set by selftest() so every arm is counted where it is judged


def _arm(label, root, expect, says=None, not_says=None):
    """Run the guard against a mutated mirror and judge the WHOLE verdict.

    ★★★ `says` / `not_says` are the load-bearing halves, not decoration.
    EXIT_COLLAPSE is now shared by nine distinct refusals, so an arm that
    asserts only an exit code proves that SOMETHING refused -- not that the
    mechanism it names did. ✔MEASURED by an independent audit 2026-08-19: the
    floor arm passed with the floor DELETED, because moving every entry out of
    the mirror also moved the index document that lives there, and the run
    collapsed on the missing document instead. The sibling guard
    `check-orphan-tests` already carries this discipline in its `_st_says`
    helper; this one was written without it and the gap was invisible.
    """
    buf = io.StringIO()
    detail = ""
    try:
        with contextlib.redirect_stdout(buf):
            rc = run(root, write=False)
    except Collapse as exc:
        rc = EXIT_COLLAPSE
        detail = str(exc)
    text = buf.getvalue() + detail

    if _RAN is not None:
        _RAN.append(label)
    ok, why = rc == expect, ""
    if not ok:
        why = "EXPECTED rc=%d" % expect
    elif says is not None and says not in text:
        ok, why = False, "rc was right but the message never said %r" % says
    elif not_says is not None and not_says in text:
        ok, why = False, ("rc was right but the message said %r, so this arm "
                          "proved a DIFFERENT refusal than it claims" % not_says)

    first = (detail or buf.getvalue()).split("\n")[0][:78]
    print("scripts-index: self-test arm %-26s rc=%d %s%s"
          % (label, rc, "as expected" if ok else why,
             (" (" + first + ")") if first else ""))
    return ok


def _mirror(root, dst):
    os.makedirs(os.path.join(dst, "scripts"), exist_ok=True)
    src_scripts = os.path.join(root, "scripts")
    for name in os.listdir(src_scripts):
        s_ = os.path.join(src_scripts, name)
        if os.path.isdir(s_) and name != "__pycache__":
            shutil.copytree(s_, os.path.join(dst, "scripts", name),
                            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    for rel in DOC_RELS:
        d = os.path.join(dst, rel)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(os.path.join(root, rel), d)


def _read(p):
    return io.open(p, "r", encoding="utf-8", newline="").read()


def _write(p, text):
    with io.open(p, "w", encoding="utf-8", newline="") as f:
        f.write(text)


# ★★ THE EXPECTED ARM COUNT IS A CONSTANT THE RUN CHECKS, NOT A SENTENCE IT
# PRINTS. The summary used to assert "11 arms exercised" in prose while the code
# ran a different number, and an audit found two of the counts in it wrong. It is
# derived from DOC_RELS so that removing a document from the tuple -- which would
# silently stop verifying that document -- changes the arithmetic and reds here.
# A HARD constant. It was briefly derived as `fixed + 2 * len(DOC_RELS)`, which
# defeated its own purpose: deleting a document from DOC_RELS then lowered BOTH
# sides of the comparison and the sabotage passed. An expectation that follows
# the change it is meant to catch is not an expectation.
EXPECTED_ARMS = 28


def selftest(root):
    tmp = tempfile.mkdtemp(prefix="scripts-index-selftest-")
    ok = True
    ran = []
    try:
        globals()["_RAN"] = ran
        _mirror(root, tmp)
        prim = os.path.join(tmp, "scripts", "run-gate", "run-gate.sh")
        pristine = _read(prim)
        readme = os.path.join(tmp, README_REL)

        ok &= _arm("0 GREEN-CONTROL", tmp, EXIT_OK)

        # ── the index and the tree disagree (EXIT_DISAGREE) ──────────────────
        newdir = os.path.join(tmp, "scripts", "zz-selftest-newcomer")
        os.makedirs(newdir)
        _write(os.path.join(newdir, "zz-selftest-newcomer.sh"),
               "#!/usr/bin/env bash\n# PURPOSE: exist, undocumented.\n")
        ok &= _arm("1 SCRIPT-NOT-IN-INDEX", tmp, EXIT_DISAGREE, says=README_REL)
        shutil.rmtree(newdir)
        ok &= _arm("1b RESTORED", tmp, EXIT_OK)

        # ★ Moved OUT of scripts/, not renamed in place: a rename leaves a
        # directory not addressable by its own name, which trips the structural
        # refusal instead of the disagreement this arm exists to prove.
        gone = os.path.join(tmp, "scripts", "run-gate")
        stash = tempfile.mkdtemp(prefix="scripts-index-stash-")
        shutil.move(gone, os.path.join(stash, "run-gate"))
        ok &= _arm("2 INDEX-ENTRY-NOT-A-SCRIPT", tmp, EXIT_DISAGREE, says=README_REL)
        shutil.move(os.path.join(stash, "run-gate"), gone)
        shutil.rmtree(stash, ignore_errors=True)
        ok &= _arm("2b RESTORED", tmp, EXIT_OK)

        _write(prim, pristine.replace(PURPOSE_MARK, PURPOSE_MARK + "MUTATED -- ", 1))
        ok &= _arm("3 PURPOSE-DRIFTED", tmp, EXIT_DISAGREE, says=README_REL)
        _write(prim, pristine)
        ok &= _arm("3b RESTORED", tmp, EXIT_OK)

        # ★★ EACH DOCUMENT IS CHECKED SEPARATELY. Every arm above invalidates
        # BOTH documents at once, so an implementation that verified only the
        # README would be indistinguishable from one that verified both --
        # ✔MEASURED by audit: halving DOC_RELS left the self-test green, and the
        # skill reference would have drifted silently while claiming otherwise.
        # ★★ ONE ARM PER DOCUMENT, GENERATED FROM DOC_RELS. Every other arm
        # invalidates BOTH documents at once, so an implementation that verified
        # only the first would be indistinguishable from one that verified both
        # -- ✔MEASURED by audit: halving DOC_RELS left the self-test green while
        # the skill reference silently stopped being checked. Each arm asserts
        # its own document is named AND that the other one is not.
        pristine_docs = {rel: _read(os.path.join(tmp, rel)) for rel in DOC_RELS}
        for i, rel in enumerate(DOC_RELS):
            others = [o for o in DOC_RELS if o != rel]
            _write(os.path.join(tmp, rel),
                   pristine_docs[rel].replace("| **`run-gate`**", "| **`run-gate-TYPO`**", 1))
            ok &= _arm("4.%d ONLY-%s-DRIFTS" % (i, os.path.basename(rel)), tmp,
                       EXIT_DISAGREE, says=rel,
                       not_says=others[0] if len(others) == 1 else None)
            _write(os.path.join(tmp, rel), pristine_docs[rel])
            ok &= _arm("4.%db RESTORED" % i, tmp, EXIT_OK)
        pristine_readme = pristine_docs[README_REL]

        # ── the declaration itself (EXIT_COLLAPSE) ───────────────────────────
        _write(prim, pristine.replace("# " + PURPOSE_MARK, "# (removed) ", 1))
        ok &= _arm("6 NO-PURPOSE-DECLARED", tmp, EXIT_COLLAPSE, says="declares no")
        _write(prim, pristine)
        ok &= _arm("6b RESTORED", tmp, EXIT_OK)

        _write(prim, pristine.replace("# " + PURPOSE_MARK,
                                      "# " + PURPOSE_MARK + "one.\n# " + PURPOSE_MARK, 1))
        ok &= _arm("7 TWO-PURPOSE-LINES", tmp, EXIT_COLLAPSE, says="Exactly one is required")
        _write(prim, pristine)

        _write(prim, pristine.replace("# " + PURPOSE_MARK + "run a gate",
                                      "# " + PURPOSE_MARK + "\n# was: run a gate", 1))
        ok &= _arm("8 EMPTY-PURPOSE", tmp, EXIT_COLLAPSE, says="EMPTY purpose")
        _write(prim, pristine)

        _write(prim, pristine.replace("# " + PURPOSE_MARK,
                                      "# " + PURPOSE_MARK + "a | b ", 1))
        ok &= _arm("9 PIPE-IN-PURPOSE", tmp, EXIT_COLLAPSE, says="raw pipe")
        _write(prim, pristine)

        _write(prim, pristine.replace("# " + PURPOSE_MARK,
                                      "# " + PURPOSE_MARK + END + " ", 1))
        ok &= _arm("10 MARKER-IN-PURPOSE", tmp, EXIT_COLLAPSE, says="never converge")
        _write(prim, pristine)
        ok &= _arm("10b RESTORED", tmp, EXIT_OK)

        # ── the layout (EXIT_COLLAPSE) ───────────────────────────────────────
        orphan = os.path.join(tmp, "scripts", "zz-selftest-folder")
        os.makedirs(orphan)
        _write(os.path.join(orphan, "helper.sh"), "#!/usr/bin/env bash\n")
        ok &= _arm("11 NO-PRIMARY-SCRIPT", tmp, EXIT_COLLAPSE, says="no primary script")
        shutil.rmtree(orphan)

        loose = os.path.join(tmp, "scripts", "zz-loose.sh")
        _write(loose, "#!/usr/bin/env bash\n# PURPOSE: sit where no index looks.\n")
        ok &= _arm("12 LOOSE-SCRIPT", tmp, EXIT_COLLAPSE, says="directly under scripts/")
        os.remove(loose)

        buried_dir = os.path.join(tmp, "scripts", "run-gate", "sub")
        os.makedirs(buried_dir)
        _write(os.path.join(buried_dir, "sub.sh"), "#!/usr/bin/env bash\n")
        ok &= _arm("13 BURIED-SCRIPT", tmp, EXIT_COLLAPSE, says="buries script")
        shutil.rmtree(buried_dir)

        twin = os.path.join(tmp, "scripts", "run-gate", "run-gate.ps1")
        pristine_twin = _read(twin)
        _write(twin, "# " + PURPOSE_MARK + "something else entirely.\n" + pristine_twin)
        ok &= _arm("14 SIBLING-CONTRADICTS", tmp, EXIT_COLLAPSE, says="differs from its primary")
        _write(twin, pristine_twin)
        ok &= _arm("14b RESTORED", tmp, EXIT_OK)

        # ── the documents' structure (EXIT_COLLAPSE) ─────────────────────────
        _write(readme, pristine_readme.replace(BEGIN, "<!-- gone -->", 1))
        ok &= _arm("15 MARKERS-MISSING", tmp, EXIT_COLLAPSE, says="missing its generated-index")
        _write(readme, pristine_readme)

        _write(readme, pristine_readme + "\n" + BEGIN + "\n| **`ghost`** | `ghost.sh` | a "
               "script that has never existed. |\n" + END + "\n")
        ok &= _arm("16 DUPLICATE-MARKERS", tmp, EXIT_COLLAPSE,
                   says="index markers", not_says="missing its generated-index")
        _write(readme, pristine_readme)

        os.remove(readme)
        ok &= _arm("17 INDEX-DOC-DELETED", tmp, EXIT_COLLAPSE, says="does not exist")
        _write(readme, pristine_readme)

        # ── THE FLOOR, isolated ──────────────────────────────────────────────
        # ★★★ Only DIRECTORIES move. The index document lives inside scripts/,
        # so moving it too is what let this arm pass with the floor deleted.
        # `not_says` pins that: if the document ever goes missing again, this arm
        # fails instead of quietly proving the wrong thing.
        keep = os.path.join(tmp, "scripts")
        held = tempfile.mkdtemp(prefix="scripts-index-held-")
        for name in os.listdir(keep):
            if os.path.isdir(os.path.join(keep, name)):
                shutil.move(os.path.join(keep, name), os.path.join(held, name))
        ok &= _arm("18 SCAN-COLLAPSED", tmp, EXIT_COLLAPSE,
                   says="floor is", not_says="does not exist")
        for name in os.listdir(held):
            shutil.move(os.path.join(held, name), os.path.join(keep, name))
        shutil.rmtree(held, ignore_errors=True)

        ok &= _arm("19 GREEN-AFTER-RESTORE", tmp, EXIT_OK)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if len(ran) != EXPECTED_ARMS:
        print("scripts-index: self-test FAILED -- %d arms ran, %d expected. An arm that "
              "silently stops running is a property that silently stops being proven; if "
              "the change was deliberate, update EXPECTED_ARMS in the same commit.\n"
              "  ran: %s" % (len(ran), EXPECTED_ARMS, ", ".join(ran)))
        return EXIT_COLLAPSE
    if not ok:
        print("scripts-index: self-test FAILED -- this guard is NOT proven able to fail.")
        return EXIT_COLLAPSE
    print("scripts-index: self-test OK - %d arms exercised, every red arm asserting the "
          "MESSAGE of the refusal it names rather than merely a non-zero exit; this guard "
          "is PROVEN able to fail." % len(ran))
    return EXIT_OK


def main(argv):
    write = "--write" in argv[1:]
    self_ = "--selftest" in argv[1:]
    unknown = [a for a in argv[1:] if a not in ("--write", "--selftest")]
    if unknown:
        print("check-scripts-index: unknown argument(s): %s" % " ".join(unknown))
        print(__doc__.rsplit("Usage:", 1)[-1].strip())
        return EXIT_USAGE
    try:
        root = repo_root()
        if self_:
            return selftest(root)
        if write:
            return run(root, write=True)
        # ★★ THE NO-ARGUMENT FORM — the one ctest uses — VERIFIES THE REAL TREE
        # AND THEN PROVES IT CAN FAIL, in that order. Same shape as
        # check-orphan-tests, and for the same reason: an entry that only
        # verified would pass identically if every check inside it had been
        # commented out, so the ctest run itself has to witness the red arms.
        rc = run(root, write=False)
        if rc != EXIT_OK:
            return rc
        return selftest(root)
    except Collapse as exc:
        print("check-scripts-index: FAIL (structural) -- %s" % exc)
        print("  This does NOT mean the index is clean - it means the SCAN COLLAPSED.")
        print("  Refusing to report a pass; fix the scan, do not lower the floor.")
        return EXIT_COLLAPSE


if __name__ == "__main__":
    sys.exit(main(sys.argv))
