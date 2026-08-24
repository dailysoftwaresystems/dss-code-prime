#!/usr/bin/env python3
# PURPOSE: refuse a Python script whose report cannot carry a non-cp1252 character through a pipe.
"""check-guard-output-encoding.py -- the OUTPUT-PATH ratchet for Python scripts.

D-GATE-NOTHING-RATCHETS-A-NEW-GUARD-INTO-RECONFIGURING-ITS-STREAMS -- the row
this guard closes.
D-GATE-PYTHON-GUARD-DIES-PRINTING-TREE-TEXT-ON-A-WINDOWS-PIPE -- the instance it
generalises.

★★★ WHY THIS EXISTS. On Windows, a Python child whose stdout is a PIPE comes up
`encoding='cp1252' errors='surrogateescape'`, and `surrogateescape` rescues only
lone surrogates left by an earlier decode -- it does NOTHING for an ordinary
unencodable character. So `print()` of a box-drawing rule, a MEASURED tick, or a
registry status glyph raises `UnicodeEncodeError` and kills the script INSIDE ITS
OWN REPORT: the run still reds, so nothing is silent, but the FINDING is lost and
the traceback names a `print` rather than the thing that was wrong. `sys.stderr`
comes up `errors='backslashreplace'`, which survives but MANGLES -- and when the
thing being printed is a status glyph, mangling erases the fact.
✔MEASURED 2026-08-23 (CPython 3.14.3, this Windows host, both streams pipes).

Twelve scripts were fixed one at a time in cycle P29. Nothing made the thirteenth
carry the block, the omission produces no symptom until a script has a finding to
report AND that finding carries a non-ASCII byte, and "the class is closed by
inspection" is exactly the state that let the population accumulate the first
time. This guard is the ratchet.

★★★ IT TESTS THE **PROPERTY**, NEVER THE SOURCE TEXT, and that is the whole design.
A guard that greps for the five-line block passes the day somebody writes an
equivalent differently and reds the day somebody reformats it -- it would be
pinning a SPELLING, and this repository has paid for that mistake in three other
instruments. So each subject is IMPORTED IN A CHILD PROCESS whose two streams are
forced to `cp1252` / `strict` and are PIPES, and the child then writes U+1F7E0 --
the registry's own OPEN glyph, the exact character the sibling defect erased -- to
BOTH streams. The parent asserts the UTF-8 bytes arrive on both pipes.
  * FORCING the child's streams to cp1252/strict rather than relying on the host
    is what keeps the arm from being VACUOUS on the Linux and macOS legs, where a
    pipe is already UTF-8 and every subject would pass without doing anything.
  * `strict` rather than the real-world `backslashreplace` on stderr is
    deliberate: the property is "arrives INTACT", and `backslashreplace` survives
    while destroying the character. A binary outcome is the honest test.
  * Two self-test arms pin the property-not-text rule from both sides: a fixture
    that reaches the property by REPLACING `sys.stdout` wholesale (no
    `reconfigure` call anywhere) PASSES, and a fixture that carries the literal
    block inside a string but never executes it FAILS.

★★ AT IMPORT, NOT INSIDE `main()`. The measured form covers argument parsing,
`--help`, and any death during module initialisation. ✔MEASURED at the P29 base
ref: `check-anchor-balance --help` through a cp1252 pipe died with
`UnicodeEncodeError` and printed ZERO bytes of help, precisely because its call
sat after `parse_args()`. Importing the module and then writing is therefore the
right probe: it measures the state every path of that module would print into.

── THE GOVERNED SET, STATED EXPLICITLY ─────────────────────────────────────────
Every `.py` PRIMARY under `scripts/`, taken from `check-scripts-index.scan()` --
IMPORTED, never re-implemented. That guard already owns the definition of "what
scripts exist and which file is a directory's primary"; a second copy here is how
two instruments start disagreeing about the same tree.
  * NOT "every `scripts/check-*` directory". A name predicate would let a guard
    named differently escape, and `refresh_landing_log` and `sqlite-runtime-bench`
    print tree-derived text on stdout exactly like a guard does. Define the
    complement -- every Python primary -- never the variants.
  * SIBLINGS ARE NOT PROBED. A sibling is reachable only through its primary's
    directory and is not an entry point ctest runs; the primary is the contract.
  * A FLOOR, because a guard whose enumeration collapses reports a clean pass over
    a corpus it never read.

── THE RATCHET, AND WHY IT IS NOT A BAN ────────────────────────────────────────
✔MEASURED 2026-08-23, after this cycle's twelve fixes: FOUR Python primaries are
still unprotected at import (`check-retyped-closed-sets` and
`check-shell-portability` reconfigure inside `main()`; `refresh_landing_log` and
`sqlite-runtime-bench` not at all). Their live `--help` and bad-flag paths were
probed through a cp1252 pipe and none died TODAY, so they are latent rather than
live -- but latent is what `check-anchor-balance` was until the day it wasn't.
They are recorded in `inventory.json` as DEBT, and the list may only get SHORTER:
  * a subject that fails and is NOT in the inventory is a REFUSAL -- the
    thirteenth script does not get to land;
  * a subject in the inventory that now PASSES is also a refusal, with the fix
    being to delete the entry in the same commit. A ratchet that lets a repaired
    entry linger stops measuring anything.
The burn-down of those four is tracked by
D-GATE-FOUR-PYTHON-PRIMARIES-REACH-UTF-8-TOO-LATE-OR-NEVER
which stays OPEN until `unprotected` is empty. ⚠ The same id is written in
`inventory.json`, and ✔MEASURED that citation is INVISIBLE to
`anchor_registry_guard`: its `scripts/` filter set is drivers-and-prose and
deliberately excludes `*.json` as fixture data. The enforced citation is this one,
in the driver.

── ATTRIBUTION: TRANSITIVE PROTECTION IS DISCLOSED, NOT SILENTLY ACCEPTED ──────
A module can be protected because a module it imports reconfigured the streams.
✔MEASURED: `check-wall-clock-in-tests` carries no block of its own and is safe
only because its module-level `_load_stripper()` executes
`check-no-abort-in-tests`. The child therefore records, for every `reconfigure`
call made during the import, WHICH FILE made it -- by wrapping the two stream
objects, so the record is a RUNTIME observation and not another source match.
Each subject is then reported as SELF, TRANSITIVE (naming the file that actually
did it), or OTHER (protected with no `reconfigure` call seen at all).
★ TRANSITIVE IS A PASS, and the reasoning is stated rather than assumed: this
guard RE-MEASURES the property on every run, so the day that import goes away the
property goes away and THIS GUARD REDS. The hazard the sibling row named was that
the protection could vanish SILENTLY; the ratchet is what removes the silence. It
is disclosed by name on every run so no reader has to rediscover it.

Exit codes: 0 OK · 1 a subject is unprotected, or an inventory entry is stale ·
2 the scan collapsed (structural: fix the scan, never lower the floor) · 3 usage.

Usage:
    python scripts/check-guard-output-encoding/check-guard-output-encoding.py
    python scripts/check-guard-output-encoding/check-guard-output-encoding.py --list
    python scripts/check-guard-output-encoding/check-guard-output-encoding.py --write
    python scripts/check-guard-output-encoding/check-guard-output-encoding.py --self-test
The no-argument form verifies the tree AND runs the self-test, honouring both
statuses -- a ctest entry that passes no flag must not be able to execute zero
arms (D-TEST-NONFATAL-GUARD-DEGRADES-TO-A-VACUOUS-PASS).
"""

import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile

# ── OUTPUT ENCODING — this guard is subject to its own rule ─────────────────────
# Placed at IMPORT and above the lazy sibling import below, so that this file is
# protected BY ITSELF and its own probe reports it as SELF rather than leaning on
# the enumeration guard it imports. A ratchet that only passed transitively would
# be the joke this repository has already lived through once.
# D-GATE-PYTHON-GUARD-DIES-PRINTING-TREE-TEXT-ON-A-WINDOWS-PIPE
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass


EXIT_OK, EXIT_UNPROTECTED, EXIT_COLLAPSE, EXIT_USAGE = 0, 1, 2, 3

INVENTORY_REL = os.path.join(
    "scripts", "check-guard-output-encoding", "inventory.json")

# The probe character: U+1F7E0, the registry's own OPEN status glyph. Unencodable
# in cp1252 and 4 bytes in UTF-8, so a mangling is as visible as a death.
GLYPH = "\U0001F7E0"
GLYPH_UTF8 = GLYPH.encode("utf-8")
OUT_MARK = b"<<<OUT:" + GLYPH_UTF8 + b">>>"
ERR_MARK = b"<<<ERR:" + GLYPH_UTF8 + b">>>"

# Far below the live figure (14 Python primaries ✔MEASURED 2026-08-23) so ordinary
# churn cannot trip it, high enough that a collapsed enumeration cannot pass.
PY_FLOOR = 8

PROBE_TIMEOUT_S = 120

SELF, TRANSITIVE, OTHER, UNPROTECTED = "SELF", "TRANSITIVE", "OTHER", "UNPROTECTED"


class Collapse(Exception):
    """The scan could not be trusted. Distinct from a finding."""


# ── the child probe ─────────────────────────────────────────────────────────────
# ★ THE TWO STREAM OBJECTS ARE WRAPPED, NOT THE CLASS: `io.TextIOWrapper` is a C
# type and will not accept a patched method, and wrapping the objects is also the
# more faithful observation -- it sees exactly what the subject sees under
# `sys.stdout`. After the import the wrapper is UNWRAPPED ONLY IF IT IS STILL
# INSTALLED: a subject that made its streams safe by REPLACING `sys.stdout`
# outright must be measured through its replacement, or the probe would undo the
# very fix it is testing.
_PROBE = r'''
import importlib.util, json, os, sys

CALLERS = []


class _Rec(object):
    def __init__(self, real):
        object.__setattr__(self, "_real", real)

    def __getattr__(self, name):
        return getattr(object.__getattribute__(self, "_real"), name)

    def reconfigure(self, *a, **k):
        CALLERS.append(sys._getframe(1).f_code.co_filename)
        return object.__getattribute__(self, "_real").reconfigure(*a, **k)


subject, report = sys.argv[1], sys.argv[2]

real_out, real_err = sys.stdout, sys.stderr
for _s in (real_out, real_err):
    try:
        _s.reconfigure(encoding="cp1252", errors="strict")
    except Exception:
        pass
sys.stdout, sys.stderr = _Rec(real_out), _Rec(real_err)

status = {"imported": False, "error": "", "callers": []}
try:
    spec = importlib.util.spec_from_file_location("_subject_under_probe", subject)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["_subject_under_probe"] = mod
    spec.loader.exec_module(mod)
    status["imported"] = True
except BaseException as exc:
    status["error"] = "%s: %s" % (type(exc).__name__, exc)

status["callers"] = sorted(set(CALLERS))

if isinstance(sys.stdout, _Rec):
    sys.stdout = object.__getattribute__(sys.stdout, "_real")
if isinstance(sys.stderr, _Rec):
    sys.stderr = object.__getattribute__(sys.stderr, "_real")

for stream, mark in ((sys.stdout, "<<<OUT:"), (sys.stderr, "<<<ERR:")):
    try:
        stream.write(mark + "\U0001F7E0" + ">>>")
        stream.flush()
    except BaseException:
        pass

with open(report, "w", encoding="utf-8", newline="\n") as fh:
    json.dump(status, fh)
'''


def repo_root():
    """The checkout root, asked of git rather than derived from `__file__`.

    Same reasoning as the sibling guards: a script's own depth under the repo is
    exactly the fact that changed when `tools/` was merged into `scripts/`.
    """
    try:
        p = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                           capture_output=True, text=True, check=False)
    except OSError as exc:
        raise Collapse("cannot run git (%s). Pass the repo root as an argument "
                       "instead." % exc)
    if p.returncode != 0:
        raise Collapse("not inside a git checkout: " + (p.stderr or "").strip())
    return p.stdout.strip()


def load_scripts_index(path=None):
    """The enumeration guard, IMPORTED. A loud death if it is not where it lives.

    The alternative -- re-listing `scripts/` here -- would be a second definition
    of what a script is, drifting away from the first the moment either changes.
    """
    if path is None:
        path = os.path.join(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))), "check-scripts-index",
            "check-scripts-index.py")
    if not os.path.isfile(path):
        raise Collapse(
            "cannot find the script enumeration at %s.\n"
            "  This guard governs every Python PRIMARY under scripts/, and the "
            "definition of 'primary' belongs to check-scripts-index. Restore that "
            "script or move the enumeration somewhere both can reach; do NOT "
            "re-implement it here." % path)
    spec = importlib.util.spec_from_file_location("_scripts_index", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def subjects(root, index_path=None):
    """Every `.py` primary under `scripts/`, repo-relative, sorted."""
    idx = load_scripts_index(index_path)
    try:
        entries = idx.scan(root)
    except idx.Collapse as exc:
        raise Collapse(
            "the script enumeration collapsed, so the governed set is unknown: %s\n"
            "  Fix that first -- scripts_index_guard reports the same failure."
            % exc)
    return sorted(e.primary for e in entries if e.primary.endswith(".py"))


def probe(root, rel_or_abs):
    """Import the subject in a cp1252/strict child and see what reaches the pipes.

    Returns (verdict, callers, error). `verdict` is one of SELF / TRANSITIVE /
    OTHER / UNPROTECTED. A subject that cannot be imported is a COLLAPSE, never a
    pass: an unjudgeable module must not be reported as clean.
    """
    path = rel_or_abs if os.path.isabs(rel_or_abs) else os.path.join(root, rel_or_abs)
    fd, report = tempfile.mkstemp(prefix="guard-output-encoding-", suffix=".json")
    os.close(fd)
    try:
        try:
            p = subprocess.run([sys.executable, "-c", _PROBE, path, report],
                               cwd=root, capture_output=True,
                               timeout=PROBE_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            raise Collapse("probing %s did not finish in %d s. A module that hangs "
                           "at import cannot be judged." % (rel_or_abs, PROBE_TIMEOUT_S))
        try:
            with io.open(report, encoding="utf-8") as fh:
                status = json.load(fh)
        except (OSError, ValueError):
            raise Collapse(
                "probing %s produced no report. The child exited %d.\n  stderr: %s"
                % (rel_or_abs, p.returncode,
                   p.stderr.decode("utf-8", "replace").strip()[-400:]))
    finally:
        if os.path.exists(report):
            os.remove(report)

    if not status["imported"]:
        raise Collapse(
            "%s could not be imported, so its output path cannot be judged: %s\n"
            "  A module that raises at import is not clean -- it is unmeasurable."
            % (rel_or_abs, status["error"]))

    intact = (OUT_MARK in p.stdout) and (ERR_MARK in p.stderr)
    callers = [os.path.normcase(os.path.abspath(c)) for c in status["callers"]]
    if not intact:
        return UNPROTECTED, callers, ""
    if os.path.normcase(os.path.abspath(path)) in callers:
        return SELF, callers, ""
    if callers:
        return TRANSITIVE, callers, ""
    return OTHER, callers, ""


def load_inventory(root):
    path = os.path.join(root, INVENTORY_REL)
    if not os.path.isfile(path):
        raise Collapse(
            "no inventory at %s. The ratchet's baseline is a tracked file; a "
            "missing one would let every unprotected script through as if it had "
            "always been allowed." % INVENTORY_REL)
    with io.open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    return list(data.get("unprotected", []))


def run(root, subject_list, inventory, floor=PY_FLOOR, out=None):
    """Probe every subject and apply the ratchet. Returns an exit code."""
    out = out or sys.stdout
    if len(subject_list) < floor:
        raise Collapse(
            "found only %d Python primary script(s), floor is %d. The enumeration "
            "COLLAPSED -- fix the scan, do not lower the floor."
            % (len(subject_list), floor))

    verdicts = {}
    for rel in subject_list:
        verdicts[rel] = probe(root, rel)

    inv = set(inventory)
    new_unprotected = [r for r in subject_list
                       if verdicts[r][0] == UNPROTECTED and r not in inv]
    stale = [r for r in subject_list if verdicts[r][0] != UNPROTECTED and r in inv]
    ghost = [r for r in inv if r not in subject_list]
    transitive = [r for r in subject_list if verdicts[r][0] == TRANSITIVE]
    debt = [r for r in subject_list if verdicts[r][0] == UNPROTECTED and r in inv]

    for rel in transitive:
        who = ", ".join(os.path.basename(c) for c in verdicts[rel][1])
        print("guard-output-encoding: DISCLOSURE - %s is protected only "
              "TRANSITIVELY, by %s. It carries no reconfiguration of its own, so "
              "the protection ends the day that import does -- which THIS guard "
              "would then refuse, because it re-measures the property every run."
              % (rel, who), file=out)

    if debt:
        print("guard-output-encoding: DEBT - %d inventoried script(s) still cannot "
              "carry a non-cp1252 character through a pipe: %s. The list may only "
              "get shorter." % (len(debt), ", ".join(debt)), file=out)

    failed = False
    if new_unprotected:
        failed = True
        print("guard-output-encoding: FAIL - %d script(s) lose or mangle a "
              "non-cp1252 character on a pipe and are NOT in the inventory:"
              % len(new_unprotected), file=out)
        for rel in new_unprotected:
            print("  ! UNPROTECTED  %s" % rel, file=out)
        print("  Give the module this block, at IMPORT and covering BOTH streams:\n"
              "      for _stream in (sys.stdout, sys.stderr):\n"
              "          try:\n"
              "              _stream.reconfigure(encoding=\"utf-8\", errors=\"replace\")\n"
              "          except (AttributeError, ValueError, OSError):\n"
              "              pass\n"
              "  Inside main() is NOT enough: argument parsing and --help print "
              "before it. Adding the file to inventory.json instead is what this "
              "ratchet exists to refuse.", file=out)

    if stale:
        failed = True
        print("guard-output-encoding: FAIL - %d inventory entr(y/ies) now PASS. "
              "That is good news, and the fix is to delete them in the same "
              "commit; a ratchet that lets a repaired entry linger stops "
              "measuring anything:" % len(stale), file=out)
        for rel in stale:
            print("  = REPAIRED, STILL LISTED  %s" % rel, file=out)

    if ghost:
        failed = True
        print("guard-output-encoding: FAIL - %d inventory entr(y/ies) name no "
              "Python primary in the tree: %s. An entry for a file that no longer "
              "exists is a ceiling nothing can ever lower."
              % (len(ghost), ", ".join(ghost)), file=out)

    if failed:
        return EXIT_UNPROTECTED

    counts = {}
    for rel in subject_list:
        counts[verdicts[rel][0]] = counts.get(verdicts[rel][0], 0) + 1
    print("guard-output-encoding: OK (%d Python primary script(s) probed through a "
          "cp1252 pipe; %s)"
          % (len(subject_list),
             ", ".join("%d %s" % (counts[k], k) for k in sorted(counts))), file=out)
    return EXIT_OK


def write_inventory(root, subject_list, inventory):
    """Regenerate the inventory DOWNWARD only. Growing it is a refusal.

    ★★★ THE ON-DISK DOCUMENT IS READ AND ONLY THE `unprotected` LIST IS
    REPLACED, WHICH IS WHY THIS GUARD HAS NO `_INVENTORY_COMMENT` LITERAL AND
    MUST NOT GROW ONE. Its siblings `check-plan-citations`,
    `check-stale-refusal-citations` and `check-wrapped-anchor-ids` all SERIALIZE
    a fresh body from an in-code literal, so for them the file and the code are
    two copies of one text that can silently disagree -- and all three now refuse
    on that disagreement. Here there is exactly ONE copy: the `_comment` in the
    JSON round-trips through this function untouched, so there is nothing to
    diverge FROM. ✔MEASURED 2026-08-24 (cycle P30, lane F): the module defines no
    name containing COMMENT, and a `--write` over the live inventory leaves the
    `_comment` block byte-identical while rewriting `unprotected`.
    ⚠⚠ THE HAZARD THAT REMAINS IS THE OPPOSITE ONE, AND IT IS WHY THIS IS
    SPELLED OUT RATHER THAN LEFT TO BE REDISCOVERED: "simplifying" this to
    `json.dump({"unprotected": now}, ...)` would DELETE the sole copy of the
    ratchet's rules and of its measured record, silently, with rc=0 and nothing
    left to read. The self-test arm `17 WRITE-PRESERVES-THE-COMMENT` pins it.
    """
    now = sorted(r for r in subject_list if probe(root, r)[0] == UNPROTECTED)
    added = sorted(set(now) - set(inventory))
    if added:
        print("guard-output-encoding: REFUSED to write - %d script(s) would be "
              "ADDED to the inventory: %s.\n  The ratchet only comes down. Fix the "
              "script; do not baseline it." % (len(added), ", ".join(added)),
              file=sys.stderr)
        return EXIT_UNPROTECTED
    path = os.path.join(root, INVENTORY_REL)
    with io.open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    data["unprotected"] = now
    with io.open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    print("guard-output-encoding: inventory rewritten, %d entr(y/ies) remain."
          % len(now))
    return EXIT_OK


# ── fixtures + self-test ────────────────────────────────────────────────────────
# Fixtures are SYNTHESIZED into a temp tree and deleted. An on-disk unprotected
# fixture would be a file the next sweep of this battery "fixes", silently turning
# the red arms green.

_BLOCK = ('import sys\n'
          'for _stream in (sys.stdout, sys.stderr):\n'
          '    try:\n'
          '        _stream.reconfigure(encoding="utf-8", errors="replace")\n'
          '    except (AttributeError, ValueError, OSError):\n'
          '        pass\n')

FIXTURES = {
    # canonical: the block, executed at import
    "protected.py": _BLOCK + 'VALUE = 1\n',
    # no protection at all
    "bare.py": 'import sys\nVALUE = 2\n',
    # THE PROPERTY, REACHED A DIFFERENT WAY: no `reconfigure` call anywhere.
    # A source-matching guard would call this a violation. It is not.
    "replaced.py": ('import io, sys\n'
                    'sys.stdout = io.TextIOWrapper(sys.stdout.buffer, '
                    'encoding="utf-8", errors="replace")\n'
                    'sys.stderr = io.TextIOWrapper(sys.stderr.buffer, '
                    'encoding="utf-8", errors="replace")\n'),
    # THE MIRROR IMAGE: the literal block is present as TEXT and never runs.
    # A grep would pass this file.
    "quoted.py": ('import sys\n'
                  'DOCS = """\n' + _BLOCK + '"""\n'
                  '# for _stream in (sys.stdout, sys.stderr):\n'
                  '#     _stream.reconfigure(encoding="utf-8", errors="replace")\n'),
    # transitive: executes a protected sibling at module level, has no block
    "borrower.py": ('import importlib.util, os, sys\n'
                    '_p = os.path.join(os.path.dirname(os.path.abspath(__file__)), '
                    '"protected.py")\n'
                    '_s = importlib.util.spec_from_file_location("_lender", _p)\n'
                    '_m = importlib.util.module_from_spec(_s)\n'
                    '_s.loader.exec_module(_m)\n'),
    # unimportable: must COLLAPSE, never pass
    "explodes.py": 'raise RuntimeError("fixture refuses to import")\n',
}


def _fixture_tree():
    box = tempfile.mkdtemp(prefix="guard-output-encoding-selftest-")
    for name, body in FIXTURES.items():
        with io.open(os.path.join(box, name), "w", encoding="utf-8",
                     newline="\n") as fh:
            fh.write(body)
    return box


def _capture(fn):
    """Run `fn(out)` collecting its report; returns (code_or_exc, text)."""
    buf = io.StringIO()
    try:
        return fn(buf), buf.getvalue()
    except Collapse as exc:
        return exc, buf.getvalue()


def _arm(results, label, got, expect_code, says=None, not_says=None):
    code, text = got
    ok = True
    detail = ""
    if isinstance(expect_code, type) and issubclass(expect_code, BaseException):
        if not isinstance(code, expect_code):
            ok, detail = False, "expected %s, got %r" % (expect_code.__name__, code)
        else:
            text = text + str(code)
    elif code != expect_code:
        ok, detail = False, "expected exit %s, got %r" % (expect_code, code)
    if ok and says and says not in text:
        ok, detail = False, "message did not contain %r" % says
    if ok and not_says and not_says in text:
        ok, detail = False, "message unexpectedly contained %r" % not_says
    results.append((ok, label, detail, text))
    return ok


# ★★★ THE ARM COUNT IS PINNED, AND UNTIL 2026-08-24 IT WAS NOT. Every sibling in
# this battery pins one; this guard reported `len(results)` and compared it to
# nothing, so an arm silently dropped by an early `return`, an exception swallowed
# in a refactor, or a block moved inside a branch that stopped being taken would
# have left the suite GREEN while asserting less -- the vacuity
# D-TEST-NONFATAL-GUARD-DEGRADES-TO-A-VACUOUS-PASS names, reached by subtraction
# instead of by short-circuit. ⚠ Raising this number is not a formality: it is the
# claim that the arms you added actually RUN.
EXPECTED_ARMS = 21


def self_test():
    """Every refusal, and both halves of the property-not-text rule."""
    box = _fixture_tree()
    results = []
    try:
        F = lambda n: os.path.join(box, n)                       # noqa: E731
        good, bare = F("protected.py"), F("bare.py")
        replaced, quoted = F("replaced.py"), F("quoted.py")
        borrower, explodes = F("borrower.py"), F("explodes.py")

        # -- the probe's own verdicts -------------------------------------------
        _arm(results, "0  PROBE: the canonical block reports SELF",
             (probe(box, good)[0], ""), SELF)
        _arm(results, "1  PROBE: no block at all reports UNPROTECTED",
             (probe(box, bare)[0], ""), UNPROTECTED)
        _arm(results, "2  PROBE: an EQUIVALENT spelling passes (property, not text)",
             (probe(box, replaced)[0], ""), OTHER)
        _arm(results, "3  PROBE: the block present only as TEXT fails (a grep would not)",
             (probe(box, quoted)[0], ""), UNPROTECTED)
        _arm(results, "4  PROBE: a borrowed block reports TRANSITIVE",
             (probe(box, borrower)[0], ""), TRANSITIVE)
        lender = [os.path.basename(c) for c in probe(box, borrower)[1]]
        _arm(results, "5  PROBE: and the LENDER is named, not merely the fact",
             (lender, ""), ["protected.py"])

        subs = sorted(FIXTURES)
        allp = [s for s in subs if s != "explodes.py"]

        # -- the ratchet ---------------------------------------------------------
        _arm(results, "6  RATCHET: a NEW unprotected script is refused",
             _capture(lambda o: run(box, allp, [], floor=1, out=o)),
             EXIT_UNPROTECTED, says="! UNPROTECTED  bare.py")
        _arm(results, "7  RATCHET: and the refusal names the remedy, not just the file",
             _capture(lambda o: run(box, allp, [], floor=1, out=o)),
             EXIT_UNPROTECTED, says="Inside main() is NOT enough")
        _arm(results, "8  RATCHET: an inventoried script is DEBT, not a refusal",
             _capture(lambda o: run(box, ["protected.py", "bare.py", "quoted.py"],
                                    ["bare.py", "quoted.py"], floor=1, out=o)),
             EXIT_OK, says="DEBT - 2 inventoried script(s)")
        _arm(results, "9  RATCHET: a REPAIRED entry still listed is refused",
             _capture(lambda o: run(box, ["protected.py"], ["protected.py"],
                                    floor=1, out=o)),
             EXIT_UNPROTECTED, says="REPAIRED, STILL LISTED  protected.py")
        _arm(results, "10 RATCHET: an entry naming no script in the tree is refused",
             _capture(lambda o: run(box, ["protected.py"], ["gone.py"],
                                    floor=1, out=o)),
             EXIT_UNPROTECTED, says="name no Python primary")
        _arm(results, "11 DISCLOSURE: transitive protection is reported by name",
             _capture(lambda o: run(box, ["protected.py", "borrower.py"], [],
                                    floor=1, out=o)),
             EXIT_OK, says="borrower.py is protected only TRANSITIVELY, by "
                           "protected.py")
        _arm(results, "12 GREEN CONTROL: a wholly protected set passes",
             _capture(lambda o: run(box, ["protected.py", "replaced.py"], [],
                                    floor=1, out=o)),
             EXIT_OK, says="OK (2 Python primary script(s) probed",
             not_says="FAIL")

        # -- fail-closed ---------------------------------------------------------
        _arm(results, "13 COLLAPSE: too few subjects is a collapse, not a pass",
             _capture(lambda o: run(box, ["protected.py"], [], floor=9, out=o)),
             Collapse, says="do not lower the floor")
        _arm(results, "14 COLLAPSE: a module that cannot be imported is unmeasurable",
             _capture(lambda o: run(box, subs, [], floor=1, out=o)),
             Collapse, says="could not be imported")
        _arm(results, "15 COLLAPSE: a missing enumeration is a loud death",
             _capture(lambda o: subjects(box, os.path.join(box, "nope.py"))),
             Collapse, says="do NOT re-implement it here")

        # -- this guard holds itself to its own rule -----------------------------
        _arm(results, "16 SELF-APPLICATION: this guard protects ITSELF, not transitively",
             (probe(os.path.dirname(os.path.abspath(__file__)),
                    os.path.abspath(__file__))[0], ""), SELF)

        # -- the inventory's own prose survives the write verb -------------------
        # ★★★ THIS IS THE PROPERTY THE SIBLING RATCHETS EXPRESS AS A DIVERGENCE
        # REFUSAL, WRITTEN THE ONLY WAY IT CAN BE WRITTEN HERE. They serialize a
        # fresh body from an in-code literal, so their file and their code are
        # two copies that can disagree, and each now refuses on the disagreement.
        # This guard keeps ONE copy -- `write_inventory` reads the document and
        # replaces `unprotected` alone -- so there is nothing to compare against
        # and minting a second copy to compare with would CREATE the hazard in
        # order to guard it. What is real here is the DELETION direction: a
        # `write_inventory` rewritten to serialize a fresh dict would drop the
        # `_comment` silently, with rc=0, and take the ratchet's rules and its
        # measured record with it. So the arm asserts the block SURVIVES a write,
        # byte for byte, on a fixture inventory that carries prose no code knows.
        inv_box = os.path.join(box, os.path.dirname(INVENTORY_REL))
        os.makedirs(inv_box, exist_ok=True)
        inv_path = os.path.join(box, INVENTORY_REL)
        # ★ Prose invented HERE, not copied from the shipped inventory: a fixture
        # quoting the real file would pass the day `write_inventory` started
        # regenerating that exact text from a literal, which is the defect.
        _FIXTURE_COMMENT = ["prose that exists only in this JSON",
                            "and in no literal anywhere in the module."]
        # ⚠ THE FIXTURE INVENTORY LISTS A SCRIPT THAT NOW PASSES, so the write is
        # a REAL lowering rather than a no-op. An arm whose write changed nothing
        # would prove the `_comment` survives doing nothing -- vacuously true of
        # a `write_inventory` that never ran at all.
        with io.open(inv_path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"_comment": _FIXTURE_COMMENT,
                       "unprotected": ["bare.py", "protected.py", "quoted.py"]},
                      fh, indent=2)
            fh.write("\n")
        write_inventory(box, ["protected.py", "bare.py", "quoted.py"],
                        ["bare.py", "protected.py", "quoted.py"])
        _written = json.load(io.open(inv_path, encoding="utf-8"))
        _arm(results, "17 WRITE-PRESERVES-THE-COMMENT: the sole copy of the "
             "ratchet's prose survives a `--write` byte for byte",
             (_written.get("_comment"), ""), _FIXTURE_COMMENT)
        _arm(results, "18 WRITE-STILL-LOWERED: and the write it survived was a "
             "real one -- the repaired entry is gone",
             (_written.get("unprotected"), ""), ["bare.py", "quoted.py"])

        # ★★ THE RATCHET DIRECTION OF THE WRITE VERB, WHICH HAD NO ARM AT ALL.
        # `write_inventory` refusing to ADD is the whole reason the inventory can
        # be trusted as DEBT rather than as a permission list, and it was the one
        # refusal in this file that nothing exercised -- so "refused" and
        # "refused but wrote anyway" were indistinguishable here. The arm asserts
        # BOTH the exit code and the FILE, because only the file separates them.
        _before = io.open(inv_path, "rb").read()
        _rc = write_inventory(box, ["protected.py", "bare.py", "quoted.py"], [])
        _arm(results, "19 WRITE-REFUSES-A-RAISE: adding a script to the inventory "
             "is refused, not baselined", (_rc, ""), EXIT_UNPROTECTED)
        _arm(results, "19b WRITE-WROTE-NOTHING: and the refusal left the file "
             "byte-identical",
             (io.open(inv_path, "rb").read() == _before, ""), True)
    finally:
        shutil.rmtree(box, ignore_errors=True)

    bad = [r for r in results if not r[0]]
    for ok, label, detail, text in results:
        if ok:
            print("  ok   %s" % label)
        else:
            print("  FAIL %s -- %s" % (label, detail))
            print("       report was: %s" % text.strip()[:400])
    if len(results) != EXPECTED_ARMS:
        print("  FAIL arm count -- expected %d arm(s), ran %d. An arm was dropped "
              "or added without updating EXPECTED_ARMS."
              % (EXPECTED_ARMS, len(results)))
        bad = bad + [(False, "arm count", "", "")]
    print("guard-output-encoding: self-test %s - %d arm(s) exercised, every red arm "
          "asserting the MESSAGE of the refusal it names rather than merely a "
          "non-zero exit; this guard is PROVEN able to fail."
          % ("OK" if not bad else "FAILED", len(results)))
    return EXIT_OK if not bad else EXIT_UNPROTECTED


def main(argv):
    known = ("--self-test", "--selftest", "--write", "--list", "--help", "-h")
    unknown = [a for a in argv[1:] if a not in known]
    if unknown:
        print("check-guard-output-encoding: unknown argument(s): %s"
              % " ".join(unknown), file=sys.stderr)
        print(__doc__.rsplit("Usage:", 1)[-1].strip(), file=sys.stderr)
        return EXIT_USAGE
    if "--help" in argv[1:] or "-h" in argv[1:]:
        print(__doc__)
        return EXIT_OK

    try:
        root = repo_root()
        if "--self-test" in argv[1:] or "--selftest" in argv[1:]:
            return self_test()
        subs = subjects(root)
        if "--list" in argv[1:]:
            for rel in subs:
                verdict, callers, _e = probe(root, rel)
                print("  %-58s %-11s %s"
                      % (rel, verdict,
                         ", ".join(os.path.basename(c) for c in callers) or "-"))
            return EXIT_OK
        if "--write" in argv[1:]:
            return write_inventory(root, subs, load_inventory(root))
        # ★ THE NO-ARGUMENT FORM DOES BOTH, and honours both statuses: a ctest
        # entry that passes no flag must not be able to execute zero arms.
        tree = run(root, subs, load_inventory(root))
        arms = self_test()
        return tree or arms
    except Collapse as exc:
        print("check-guard-output-encoding: COLLAPSE - %s" % exc, file=sys.stderr)
        return EXIT_COLLAPSE


if __name__ == "__main__":
    sys.exit(main(sys.argv))
