#!/usr/bin/env python3
# PURPOSE: refuse a tracked text blob that carries a CR, and a CR instrument that cannot see one.
"""check-line-endings.py -- the LF-contract guard, and the repository's CR instrument.

CONTRACT: NO tracked TEXT blob in this repository may contain a line-terminating CR -- not
in HEAD, and not staged in the index. A file that genuinely needs its 0x0D bytes preserved
declares itself `binary` in `.gitattributes`, which this guard honours through `git grep -I`
(that is how `examples/**/*.bin` keeps its `#embed` resource bytes). Further tiers judge the
WORKING TREE (checks D, E1, E2) and the INSTRUMENTS people use to ask the question (check F).

WHY A GATE AND NOT JUST THE PIN. `.gitattributes` pins `eol=lf` for the source tree, but a
pin is only as good as its GLOB COVERAGE: the pin that existed first covered configs, goldens
and harness drivers and still let six CRLF blobs land in `src/` and `tests/`, because no
`.cpp` was ever named. This check is the invariant the pin is only a mechanism for.

★★★ ONE PROGRAM SINCE 2026-09-21 (lane mig, part 4). It was a bash twin and a PowerShell
twin; the operator's order is that no `.sh`/`.ps1` lives under the actions directory. This
file carries the UNION of both twins' checks and every self-test arm of both, plus the arms
neither had (`.temp/lane-mig-findings.md` P4-5 is the transcription table).

★★★ THE BOUND, AND WHAT THE PORT CHANGED ABOUT IT. The guard gives up after 90 s and reds,
naming the query it was waiting on: a hang cannot be told apart from slow work, and three
ctest runs on this workstation were voided by exactly that ambiguity on 2026-09-07 (every
process at ZERO CPU, twice with the PowerShell twin stuck on a `git` query that answered by
hand in 29 ms). The mechanism was never root-caused. The PowerShell twin's own notes named
the best lead: every `git` child INHERITED the guard's stdout/stderr -- ctest's pipes -- so a
lingering git grandchild could hold ctest's pipe open with every process idle. ✔MEASURED on
main 2026-09-21: `line_endings_guard` timed out 3 of 3 under ctest at
"Enter-RepoTree (git rev-parse, inside repo-tree.ps1)" and passed by hand in 3.9 s.
⇒ Here EVERY child is started with `stdin=DEVNULL` and PIPE stdout/stderr (it never inherits
a handle of this process), is REGISTERED with the watchdog, and the activity is set to the
child's exact argv BEFORE the wait. On expiry the watchdog writes the refusal from its own
thread, kills every registered child's whole tree, and `os._exit(2)`. That removes the
inherited-handle hypothesis BY CONSTRUCTION; whether it was the cause is measured by running
the ported label under ctest (P4 findings), not claimed here.

THE BUDGET IS NOT SETTABLE: 90 s is ~4x the 23 s worst case measured under deliberate
git-lock contention and ~34x the guard's cost under ctest on a quiet tree. The only override
is `--watchdog-probe N`, a mode that checks NOTHING and cannot exit 0 -- a way to run nothing
with a chosen budget, never a way to run the guard with a longer one.

Usage:
    python check-line-endings.py                     verify the whole repository (self-test first)
    python check-line-endings.py --files PATH...     are THESE files clean? (0 clean, 1 a CR, 2 unmeasured)
    python check-line-endings.py --files-from FILE   the same, one path per line (`-` reads stdin)
    python check-line-endings.py --audit-instruments check F alone
    python check-line-endings.py --selftest          the self-test alone
    python check-line-endings.py --selftest-watchdog prove the timeout (its own ctest entry)
    python check-line-endings.py --help
Exit codes: 0 OK · 1 a CR (or a blind CR instrument) was found · 2 unmeasured, refused or
timed out -- never a pass over something this guard could not read.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

# A cp1252 console turns a printed glyph into a traceback; reconfigure before anything can
# print (the property `guard_output_encoding_guard` ratchets for every program).
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

WATCHDOG_BUDGET_S = 90
PROBE_DEFAULT_S = 5
SCAN_FLOOR = 1500
BINARY_PROBE = 8000     # git's own binary heuristic: a NUL in the first 8000 bytes
EXPECTED_SELFTEST_ARMS = 35
EXPECTED_WATCHDOG_ARMS = 6

# Check C's pinned set: the paths whose ATTRIBUTE column says `eol=lf` (git applied
# `.gitattributes` -- config-driven), UNION the glob list the twins spelled out, so the
# check is never narrower than it was (✔MEASURED 2026-09-21: 0 rows in either set).
LEGACY_PINNED_GLOBS = ("*.c", "*.h", "*.cpp", "*.hpp", "*.cmake", "CMakeLists.txt",
                       "*/CMakeLists.txt", "*.md", "VERSION")

# `.plans/**` is EXCLUDED from check F by scope ruling: it is the deferred-anchor REGISTRY,
# a historical record that necessarily QUOTES the broken instruments it recorded; nothing
# runs or copies it, and a record is never rewritten to satisfy a guard.
CR_AUDIT_SCOPE = ":(exclude).plans/"

# The in-band escape any file may use to QUOTE a blind idiom on purpose. The region forms
# are built here, never spelled whole in this file, so no line of the source opens or
# closes a region by accident.
MARKER = "CR-INSTRUMENT-QUOTED"
REGION_BEGIN = MARKER + ":BEGIN"
REGION_END = MARKER + ":END"

# CR-INSTRUMENT-QUOTED:BEGIN — the instrument notes and check F's own patterns QUOTE the
# blind idioms in order to explain and refuse them; nothing in this region runs one as a
# measurement. Check F honours this region from any file, and uses it here rather than
# exempting itself by path — a guard that needs a private escape cannot be held to its rule.
#
# ⚠ INSTRUMENT NOTES, ✔MEASURED on the Windows workstation (2026-08-06, re-measured
# 2026-08-27, cycle P42) against a control written as bytes `a\r\nb\n` and verified to hold
# exactly one CR:
#   TRAP 1 — FALSE POSITIVE, on a CLEAN file. The token `$'\r'` written INSIDE a bash command
#     substitution expands to the EMPTY STRING, so `n=$(grep -c $'\r' f)` runs `grep -c ''`
#     and returns the file's LINE COUNT — 2 for the CRLF control and 2 for its pure-LF twin.
#   TRAP 2 — FALSE NEGATIVE, the dangerous one. GNU grep and sed on that host open files in
#     TEXT MODE and strip the trailing CR BEFORE matching:
#         grep -c "$CR"   -> 0     grep -U -c "$CR" -> 1   (-U is the fix)
#         grep -a -c "$CR"-> 0     awk '/\r$/'      -> 0   (-a does NOT help)
#         sed -n '/\r/p'  -> 0     tr -dc '\r'|wc -c-> 1   (correct)
#     A MID-LINE CR is found by every one of them — the blindness is aimed precisely at the
#     only CR anyone hunts, which is how a lane certified a tree "pure LF" with `awk '/\r$/'`
#     while measuring nothing.
#   ★★ AND THIS IS WHY IT SURVIVED: under WSL/Linux every one of those instruments is CORRECT.
#     An instrument verified on the wrong leg is verified nowhere.
#   ⇒ This program counts CR as BYTES (`bytes.count(b"\r")`), and check F refuses the blind
#     spellings repo-wide; `--files` exists so nobody needs to hand-roll one.
#
# THE REFUSED SHAPE: a DETECTOR VERB and a CR PATTERN on one line, in either order. The verb
# co-requirement keeps prose out: `.gitattributes` discusses `$'\r'` with no verb, and a
# Windows path `'Z:\home\rafael\test'` carries a bare `\r` that is no CR pattern. These are
# the ERE the twins handed to `git grep -E`, byte for byte.
CR_VERB = "(grep|egrep|fgrep|rg|awk|sed|findstr|Select-String)"
CR_PAT = "(\\$'(\\\\r|\\\\015)'|['\"/]\\\\r\\$|['\"/]\\\\r['\"/])"
CR_BLIND = "(" + CR_VERB + ".*" + CR_PAT + "|" + CR_PAT + ".*" + CR_VERB + ")"
# THE EXEMPTIONS — measured-safe forms, not conveniences: `git grep/ls-files/diff/cat-file`
# read BLOBS (never stdio text mode); `grep -U`/`--binary` disable the CR stripping (`-a`
# does NOT, and is deliberately absent); `sub(`/`gsub(`/`s/\r`/`-replace`/`tr -d`/
# `${v%$'\r'}` are CONVERTERS that rewrite a CR rather than ask whether one is there.
CR_EXEMPTIONS = (
    ("git-blob-reader", r"git +(grep|ls-files|diff|cat-file)"),
    ("grep-U", r"-U "),
    ("binary", r"--binary"),
    ("sub", r"sub\("),
    ("gsub", r"gsub\("),
    ("sed-s-r", r"s/\\r"),
    ("replace", r"-replace"),
    ("tr-d", r"tr +-d"),
    ("strip-suffix", r"%\$'"),
)
# CR-INSTRUMENT-QUOTED:END
# `gsub(` always contains `sub(`, so its alternative can never be the ONLY one saving a line:
# it is kept (it names the converter a reader writes) and declared here so the self-test's
# "every alternative is live" arm knows which one cannot be.
SUBSUMED_EXEMPTIONS = {"gsub": "sub"}
# A detector verb as a WORD, for scoping exemptions to the command that holds it. (The
# blindness test itself stays the twins' unanchored ERE, run by `git grep`.)
_VERB_WORD_RE = re.compile(r"(?<![A-Za-z0-9_])(?:grep|egrep|fgrep|rg|awk|sed|findstr|Select-String)"
                           r"(?![A-Za-z0-9_])")


class Unmeasured(Exception):
    """This guard could not answer -- exit 2, never a pass."""


# ── the watchdog: every wait is bounded, and every child is registered ────────────────

class Watchdog:
    """ONE constant budget for the whole run; a daemon thread that, on expiry, writes the
    refusal, kills every registered child's tree and ends the process with exit 2."""

    def __init__(self, budget):
        self.budget = budget
        self.started = time.monotonic()
        self.activity = "starting up (no query issued yet)"
        self._children = set()
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None

    def note(self, what):
        self.activity = what

    def register(self, proc):
        with self._lock:
            self._children.add(proc)

    def unregister(self, proc):
        with self._lock:
            self._children.discard(proc)

    def start(self):
        self._thread = threading.Thread(target=self._run, name="line-endings-watchdog",
                                        daemon=True)
        self._thread.start()

    def _run(self):
        if self._stop.wait(self.budget):
            return
        elapsed = int(time.monotonic() - self.started)
        text = "\n".join([
            "line-endings: FAIL — TIMED OUT after %ds. This guard was still waiting, so it" % self.budget,
            "  measured NOTHING and its silence must not be read as a clean tree.",
            "  it was waiting on : %s" % (self.activity or "<no activity recorded>"),
            "  elapsed           : %ds since the guard started" % elapsed,
            "  ⚠ A hang is indistinguishable from slow work, which is why this is a RED and not a",
            "    longer wait. ✔REPORTED 2026-09-07: three ctest runs on this host hung with every",
            "    process at ZERO CPU, twice with a `git` query that answered by hand in 29 ms.",
            "    The mechanism is NOT root-caused; this bound only makes it visible.",
            "    Re-run once — if it recurs at the same activity, say so in the row.",
        ]) + "\n"

        def _write():
            try:
                os.write(2, text.encode("utf-8", "replace"))
            except OSError:
                pass

        writer = threading.Thread(target=_write, daemon=True)
        writer.start()
        writer.join(2.0)
        with self._lock:
            kids = list(self._children)
        for proc in kids:
            kill_tree(proc)
        os._exit(2)


def kill_tree(proc):
    """Kill `proc` and everything below it: its process group on POSIX (children are started
    in their own session), `taskkill /T /F` on Windows -- git's launcher there spawns the real
    git, and killing the launcher alone would leave the grandchild."""
    try:
        if os.name == "nt":
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=15)
        else:
            os.killpg(proc.pid, signal.SIGKILL)
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        proc.kill()
    except OSError:
        pass


_WD = None      # the run's watchdog; `spawn` registers every child with it


def spawn(argv, cwd=None, env=None, what=None, input_bytes=None):
    """THE ONE WAY this program starts a child: no inherited handle (stdin DEVNULL or a pipe,
    stdout/stderr PIPEs), registered with the watchdog, the activity set to its argv before
    the wait. -> (rc, stdout bytes, stderr bytes)."""
    if _WD is not None:
        _WD.note(what or " ".join(str(a) for a in argv))
    kw = {"cwd": cwd, "env": env, "stdout": subprocess.PIPE, "stderr": subprocess.PIPE,
          "stdin": subprocess.PIPE if input_bytes is not None else subprocess.DEVNULL}
    if os.name != "nt":
        kw["start_new_session"] = True
    try:
        proc = subprocess.Popen(list(argv), **kw)
    except OSError as exc:
        raise Unmeasured("cannot start %s (%s)" % (argv[0], exc))
    if _WD is not None:
        _WD.register(proc)
    try:
        out, err = proc.communicate(input_bytes)
    finally:
        if _WD is not None:
            _WD.unregister(proc)
    return proc.returncode, out or b"", err or b""


# ── the tree, and git asked through its identity ──────────────────────────────────────

_OT = None


def owning_tree_module():
    """`owning-tree.py`, loaded as a SIBLING by path. A missing owner is exit 2 -- never a
    fallback to the caller's cwd."""
    global _OT
    if _OT is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                            "owning-tree", "owning-tree.py")
        if not os.path.isfile(path):
            raise Unmeasured("cannot find %s -- this guard's tree is resolved there and nowhere "
                             "else" % path)
        spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _OT = mod
    return _OT


def git(ident, args, ok=(0,), env_extra=None, input_bytes=None):
    """`git <args>` THROUGH `ident`, in git's unsteered environment, through `spawn`. An exit
    code outside `ok` is UNMEASURED, carrying git's own first line -- a git that cannot run is
    never read as "nothing found" (`git grep` passes `ok=(0, 1)`: 1 means no match)."""
    ot = owning_tree_module()
    argv, cwd = ot.tree_git_argv(ident, args)
    env = ot.git_environment(env_extra)
    rc, out, err = spawn(argv, cwd=cwd, env=env, what="git " + " ".join(args),
                         input_bytes=input_bytes)
    if rc not in ok:
        first = err.decode("utf-8", "replace").strip().split("\n")[0][:200]
        raise Unmeasured("`git %s` exited %d%s -- this guard cannot read the tree through it, so "
                         "it refuses to report a verdict" % (" ".join(args), rc,
                                                             (": " + first) if first else ""))
    return out


def nul_fields(out):
    """Fields of a NUL-terminated git output, decoded without loss."""
    return [f.decode("utf-8", "surrogateescape") for f in out.split(b"\0") if f]


def read_tree_bytes(ident, rel):
    """The bytes of `rel` read at the tree's ROOT -- never relative to the process cwd."""
    with open(owning_tree_module().resolve_path(ident, rel), "rb") as fh:
        return fh.read()


def cr_count(data):
    """The one correct instrument: CR BYTES, counted. Never a line-based reader."""
    return data.count(b"\r")


def self_rel():
    """This program's path relative to the tree it lives in -- its LAYOUT position, which a
    fixture tree that carries a copy of it reproduces."""
    root = owning_tree_module().owning_tree(__file__)
    return os.path.relpath(os.path.realpath(__file__), root).replace(os.sep, "/")


# ── `--files`: the per-file question ────────────────────────────────────────────────────

def files_mode(paths, invoked_from, out=None, err=None):
    """0 all clean · 1 a CR was found · 2 a path could not be measured (or none was given)."""
    out = out or sys.stdout
    err = err or sys.stderr
    n = bad = unmeasured = 0
    for p in paths:
        n += 1
        absolute = p if os.path.isabs(p) else os.path.join(invoked_from, p)
        if os.path.isdir(absolute):
            print("  UNMEASURED  %s — is a directory, not a file" % p, file=err)
            unmeasured += 1
            continue
        if not os.path.lexists(absolute):
            print("  UNMEASURED  %s — no such file" % p, file=err)
            unmeasured += 1
            continue
        if not os.path.isfile(absolute):
            print("  UNMEASURED  %s — not a regular file" % p, file=err)
            unmeasured += 1
            continue
        try:
            with open(absolute, "rb") as fh:
                cr = cr_count(fh.read())
        except OSError:
            print("  UNMEASURED  %s — not readable" % p, file=err)
            unmeasured += 1
            continue
        if cr == 0:
            print("  LF          %s" % p, file=out)
        else:
            print("  CR  %d    %s" % (cr, p), file=out)
            bad += 1
    if n == 0:
        print("line-endings: FAIL — --files was given no paths. Refusing to report a", file=err)
        print("  pass over an empty list; that is a vacuous green, not a clean tree.", file=err)
        return 2
    rc = 0
    if unmeasured:
        print("line-endings: FAIL — %d of %d path(s) could NOT be measured (above)." % (unmeasured, n),
              file=err)
        print("  A guard that cannot read a file must say so, never imply it was clean.", file=err)
        rc = 2
    if bad:
        print("line-endings: FAIL — %d of %d file(s) carry a CR." % (bad, n), file=err)
        print("  Convert with `python -c \"import sys; p=sys.argv[1]; b=open(p,'rb').read(); "
              "open(p,'wb').write(b.replace(b'\\r\\n', b'\\n'))\" f`, or see --help.", file=err)
        rc = rc or 1
    if rc == 0:
        print("line-endings: OK (%d file(s), none carries a CR; measured as bytes)" % n, file=out)
    return rc


def read_path_list(source, stdin=None):
    """The paths of a `--files-from` list: one per line, a CR-terminated line tolerated, empty
    lines dropped. `-` reads stdin. Raises Unmeasured when the list cannot be read."""
    if source == "-":
        data = (stdin or sys.stdin.buffer).read()
    else:
        try:
            with open(source, "rb") as fh:
                data = fh.read()
        except OSError:
            raise Unmeasured("cannot read path list '%s'" % source)
    paths = []
    for line in data.decode("utf-8", "surrogateescape").split("\n"):
        if line.endswith("\r"):
            line = line[:-1]
        if line:
            paths.append(line)
    return paths


# ── check F: refuse a CR instrument that cannot see a CR ───────────────────────────────

def blind_grep_args():
    """The one `git grep` check F asks -- the self-test asks THIS, never a copy of it."""
    return ["grep", "-n", "-z", "-I", "-E", CR_BLIND, "--", ".", CR_AUDIT_SCOPE]


def parse_grep_n_z(out):
    """`path\\0lineno\\0content\\n` records -> [(path, lineno, content)]."""
    hits, pos = [], 0
    while pos < len(out):
        a = out.find(b"\0", pos)
        if a < 0:
            break
        b = out.find(b"\0", a + 1)
        if b < 0:
            break
        c = out.find(b"\n", b + 1)
        c = len(out) if c < 0 else c
        hits.append((out[pos:a].decode("utf-8", "surrogateescape"), int(out[a + 1:b]),
                     out[b + 1:c].decode("utf-8", "surrogateescape")))
        pos = c + 1
    return hits


def _drop_trailing_comment(segment):
    """A segment without its trailing `#` comment (an unquoted `#` at the start or after
    whitespace): an exemption token inside a COMMENT does not make the command safe."""
    quote = None
    for i, ch in enumerate(segment):
        if quote:
            if ch == quote:
                quote = None
            continue
        if ch in "'\"":
            quote = ch
        elif ch == "#" and (i == 0 or segment[i - 1] in " \t"):
            return segment[:i]
    return segment


def command_segments(content):
    """`content` split into command segments at `|`, `||`, `&&`, `;`, a backtick and `$(` --
    OUTSIDE quotes, so an alternation inside an awk or grep pattern stays in its command."""
    segs, cur, quote, i, n = [], [], None, 0, len(content)
    while i < n:
        ch = content[i]
        if quote:
            cur.append(ch)
            if ch == quote:
                quote = None
            i += 1
            continue
        if ch in "'\"":
            quote = ch
            cur.append(ch)
            i += 1
            continue
        if content.startswith(("||", "&&", "$("), i):
            segs.append("".join(cur))
            cur = []
            i += 2
            continue
        if ch in "|;`":
            segs.append("".join(cur))
            cur = []
            i += 1
            continue
        cur.append(ch)
        i += 1
    segs.append("".join(cur))
    return segs


def exempt_by_token(content, exemptions=CR_EXEMPTIONS):
    """True when EVERY command segment of `content` that holds a detector verb (as a word)
    also carries an exemption token outside a trailing comment. Content only: the PATH never
    exempts. A line whose verb appears only inside a word (the blindness ERE is unanchored)
    has no command to scope to, so there the token may sit anywhere in its code.

    ⚠ Never weaker than the twins' line-level rule: whatever this exempts carries a token in
    its content, which the twins exempted too. What it no longer exempts is a token in the
    path, in another pipeline stage, or in a comment (report 02 D.2: all three MEASURED false
    negatives)."""
    token_res = [re.compile(rx) for _name, rx in exemptions]
    verb_segments = [s for s in command_segments(content) if _VERB_WORD_RE.search(s)]
    if not verb_segments:
        code = _drop_trailing_comment(content)
        return any(t.search(code) for t in token_res)
    for seg in verb_segments:
        code = _drop_trailing_comment(seg)
        if not any(t.search(code) for t in token_res):
            return False
    return True


def region_state(lines, line_no):
    """(inside a quoted region at `line_no`, a region still open at EOF). Inclusive of both
    marker lines, numbered on `\\n` only -- git's own numbering."""
    inside, found = False, False
    for n, line in enumerate(lines, 1):
        if REGION_BEGIN in line:
            inside = True
        if n == line_no:
            found = inside
        if REGION_END in line:
            inside = False
    return found, inside


def instrument_audit(ident, exemptions=CR_EXEMPTIONS, env_extra=None, positive_control=True):
    """-> (offences, marked_files, raw_hits). Raises Unmeasured when git cannot answer, when a
    quoted region is left open at EOF in a file with a hit, or -- the POSITIVE CONTROL -- when
    the audit cannot see this guard's own source, which quotes the blind idioms by design.
    ✔MEASURED 2026-09-21 (brief): with a moved file not yet in main's index, `git grep` found
    NO marked file; the PowerShell twin crashed and the bash twin printed "0 file(s)" and
    PASSED. An audit that sees nothing proves nothing."""
    raw = parse_grep_n_z(git(ident, blind_grep_args(), ok=(0, 1), env_extra=env_extra))
    census_out = git(ident, ["grep", "-c", "-z", "-I", "-e", MARKER, "--", ".", CR_AUDIT_SCOPE],
                     ok=(0, 1), env_extra=env_extra)
    marked = []
    for rec in census_out.split(b"\n"):
        if b"\0" in rec:
            marked.append(rec.split(b"\0", 1)[0].decode("utf-8", "surrogateescape"))
    file_lines = {}

    def lines_of(path):
        if path not in file_lines:
            try:
                data = read_tree_bytes(ident, path)
            except OSError as exc:
                raise Unmeasured("check F could not read %s to judge its quoted regions (%s)"
                                 % (path, exc))
            file_lines[path] = data.decode("utf-8", "surrogateescape").split("\n")
        return file_lines[path]

    # Every file that carries the marker is checked for a region left OPEN at end of file --
    # it would silence every line after it, including lines nobody has written yet.
    for path in marked:
        _inside, open_at_eof = region_state(lines_of(path), 0)
        if open_at_eof:
            raise Unmeasured("%s leaves a %s region OPEN at end of file, which would silence "
                             "every line after it. Close it with %s." % (path, REGION_BEGIN,
                                                                          REGION_END))
    offences = []
    for path, line_no, content in raw:
        if MARKER in content or exempt_by_token(content, exemptions):
            continue
        inside, _open = region_state(lines_of(path), line_no)
        if not inside:
            offences.append("%s:%d:%s" % (path, line_no, content))
    if positive_control:
        me = self_rel()
        mine = [h for h in raw if h[0] == me]
        if not marked or me not in marked or not mine:
            p = subprocess.run(owning_tree_module().tree_git_argv(
                ident, ["ls-files", "--error-unmatch", "--", me])[0],
                env=owning_tree_module().git_environment(env_extra),
                stdin=subprocess.DEVNULL, capture_output=True)
            raise Unmeasured(
                "check F could not see its own source: the marker is in %d file(s)%s; %d raw "
                "hit(s) in %s, which quotes the blind idioms by design. `git grep` reads only "
                "paths the index holds (`git ls-files --error-unmatch %s`: %s). This does NOT "
                "mean the tree is clean."
                % (len(marked), "" if me in marked else ", none of them %s" % me, len(mine), me,
                   me, "tracked" if p.returncode == 0 else "NOT tracked"))
    return offences, marked, raw


# CR-INSTRUMENT-QUOTED:BEGIN — the refusal must SHOW the spellings it refuses, or the reader
# cannot recognise their own line in it.
FIX_F = """
These spellings do not measure what they appear to measure on this host
(both directions are MEASURED in the notes at the top of this program):
  · `grep -c $'\\r'`  returns the LINE COUNT — of a CLEAN file too;
  · `awk '/\\r$/'`, `sed -n '/\\r/p'`, `grep -P '\\r$'` return 0 over a
    file that is entirely CRLF, because the reader strips the CR first.
Use instead:
  (a) `python3 .harness-config/runner/actions/check-line-endings/check-line-endings.py --files PATH...`
      — the supported way to ask about specific files; or
  (b) count CR BYTES (`tr -dc '\\r' < f | wc -c`, expect 0) if you must inline it; or
  (c) `git grep`/`git ls-files --eol`, which read blobs and are unaffected.
If the line is DOCUMENTATION that quotes the idiom on purpose, put the marker
CR-INSTRUMENT-QUOTED on it, or wrap the block in that marker's BEGIN/END pair.
An exemption token counts only in the command that holds the detector verb:
not in the path, not in another pipeline stage, not in a trailing comment."""
# CR-INSTRUMENT-QUOTED:END


def run_audit_mode(ident):
    offences, marked, _raw = instrument_audit(ident)
    if offences:
        print("line-endings: FAIL — a CR instrument that cannot see a CR:", file=sys.stderr)
        print("", file=sys.stderr)
        for o in offences:
            print("  " + o, file=sys.stderr)
        print(FIX_F, file=sys.stderr)
        return 1
    print("line-endings: Check F OK (no blind CR instrument; %d file(s) carry the quoted-idiom "
          "marker, this guard's own source among them)" % len(marked))
    return 0


# ── the byte tiers ─────────────────────────────────────────────────────────────────────

def positive_control(counts, floor=SCAN_FLOOR):
    """Refuse a scan that saw fewer text blobs than its floor: the offender scans PASS by
    returning nothing, which is exactly what a broken scan returns too."""
    for what, n in counts:
        if n < floor:
            raise Unmeasured(
                "the %s scan saw only %d text blobs, below its floor of %d. This does NOT mean "
                "the tree is clean — it means the SCAN collapsed (git built without PCRE for -P, "
                "an unresolvable ref, or a moved tree). Fix the scan; never lower the floor."
                % (what, n, floor))


def parse_eol_rows(out):
    """`git ls-files --eol -z` -> [(index_col, worktree_col, attr_text, path)]."""
    rows = []
    for rec in out.split(b"\0"):
        if not rec:
            continue
        head, _tab, path = rec.partition(b"\t")
        cols = head.decode("utf-8", "replace").split()
        index_col = cols[0] if cols else ""
        work_col = cols[1] if len(cols) > 1 and cols[1].startswith("w/") else ""
        rows.append((index_col, work_col, head.decode("utf-8", "replace"),
                     path.decode("utf-8", "surrogateescape")))
    return rows


def _pinned(attr_text):
    return "eol=lf" in attr_text


def _legacy_glob(path):
    import fnmatch
    for g in LEGACY_PINNED_GLOBS:
        if "/" in g or "*" not in g:
            if fnmatch.fnmatchcase(path, g) or path == g:
                return True
        elif fnmatch.fnmatchcase(path.rsplit("/", 1)[-1], g):
            return True
    return False


def check_c(rows):
    """Binary-detected (`i/-text`) inside the eol=lf pin: the `-I` blob scans never opened it."""
    return [p for (i, _w, attr, p) in rows if i == "i/-text" and (_pinned(attr) or _legacy_glob(p))]


def check_d(rows):
    """A PINNED file rewritten CRLF on disk -- `git diff` shows nothing. A CLOSED set of worktree
    states (`w/crlf`, `w/mixed`): an unmeasured state is never guessed into a red."""
    return [p for (_i, w, attr, p) in rows if _pinned(attr) and w in ("w/crlf", "w/mixed")]


def check_e1(rows):
    """Tracked TEXT with NO eol=lf pin, CRLF on disk; binary (`i/-text`) and empty (`i/none`)
    excluded BY NAME."""
    return [p for (i, w, attr, p) in rows
            if not _pinned(attr) and i not in ("i/-text", "i/none") and w in ("w/crlf", "w/mixed")]


def autocrlf_is_true(value):
    """git's own boolean spellings of true (`true`, `yes`, `on`, `1`, any case)."""
    return value.strip().lower() in ("true", "yes", "on", "1")


def untracked_offender(data, attrs):
    """E2's decision for one untracked file: skip binary (a NUL in the first 8000 bytes, git's
    heuristic) and empty files, skip `-text`/`binary` and `eol=lf` declarations; else a CR."""
    if not data or b"\0" in data[:BINARY_PROBE]:
        return False
    if attrs.get("text") == "unset":
        return False
    if attrs.get("eol") == "lf":
        return False
    return cr_count(data) > 0


def stale_evidence(offenders, disk_bytes):
    """The first history offender that carries ZERO CR on disk: proof that this tree's .git
    does not describe its files (a tree synced without `.git`). None when there is none."""
    for path in offenders:
        data = disk_bytes(path)
        if data is not None and cr_count(data) == 0:
            return path
    return None


def verify(ident):
    """The default mode after the preconditions and the self-test. -> exit code."""
    root = ident.root
    head_n = len(nul_fields(git(ident, ["grep", "-z", "-I", "-l", "-P", "^.", "HEAD"], ok=(0, 1))))
    index_n = len(nul_fields(git(ident, ["grep", "-z", "--cached", "-I", "-l", "-P", "^."],
                                 ok=(0, 1))))
    positive_control([("HEAD", head_n), ("index", index_n)])

    head_off = [p[len("HEAD:"):] if p.startswith("HEAD:") else p for p in nul_fields(
        git(ident, ["grep", "-z", "-I", "-l", "-P", "\\r$", "HEAD"], ok=(0, 1)))]
    index_off = nul_fields(git(ident, ["grep", "-z", "--cached", "-I", "-l", "-P", "\\r$"],
                               ok=(0, 1)))

    def disk(path):
        full = owning_tree_module().resolve_path(ident, path)
        if not os.path.isfile(full):
            return None
        with open(full, "rb") as fh:
            return fh.read()

    stale = stale_evidence(sorted(set(head_off) | set(index_off)), disk)
    skip_history = stale is not None
    if skip_history:
        short = git(ident, ["rev-parse", "--short", "HEAD"], ok=(0,)).decode().strip()
        for line in (
                "line-endings: HISTORY SCAN SKIPPED — this work tree's .git does not describe its files.",
                "    evidence: '%s' is recorded as CRLF in HEAD/index but carries ZERO CR on disk." % stale,
                "    HEAD here is %s." % short,
                "    This is the expected shape of a tree synced WITHOUT .git: convicting on that history",
                "    would report violations belonging to another commit — so checks A, B and C are",
                "    suspended here. Run this guard on the AUTHORITATIVE checkout for history hygiene."):
            print(line, file=sys.stderr)
        head_off, index_off = [], []

    report = ["  committed (HEAD): %s" % p for p in head_off]
    report += ["  staged (index): %s" % p for p in index_off]

    rows = parse_eol_rows(git(ident, ["ls-files", "--eol", "-z"], ok=(0,)))
    if len(rows) < SCAN_FLOOR:
        raise Unmeasured("`git ls-files --eol` returned only %d rows, below its floor of %d. Checks "
                         "D and E answer from that table, so an empty one makes BOTH report a clean "
                         "worktree over files they never looked at. Fix the scan." % (len(rows),
                                                                                       SCAN_FLOOR))
    if not skip_history:
        report += ["  staged (index): binary-detected inside the eol=lf pin, so the blob scan above "
                   "never opened it: %s" % p for p in check_c(rows)]
    report += ["  working (tracked, eol=lf pinned): rewritten to CRLF on disk — git diff shows "
               "NOTHING to review: %s" % p for p in check_d(rows)]

    autocrlf = git(ident, ["config", "core.autocrlf"], ok=(0, 1)).decode("utf-8", "replace").strip()
    autocrlf_shown = autocrlf or "<unset>"
    for p in check_e1(rows):
        if autocrlf_is_true(autocrlf):
            print("line-endings: NOTE — '%s' is CRLF on disk and carries NO eol=lf pin, but "
                  "core.autocrlf=%s," % (p, autocrlf), file=sys.stderr)
            print("    so a CRLF checkout is the expected result here and this is NOT convicted. Add "
                  "an eol=lf pin", file=sys.stderr)
            print("    for its extension if this repo should own its bytes regardless of a host's "
                  "git config.", file=sys.stderr)
        else:
            report.append("  working (tracked, NOT covered by an eol=lf pin): CRLF on disk and "
                          "core.autocrlf=%s, so `git add` will NOT normalise it — this WILL land "
                          "CRLF in the commit: %s" % (autocrlf_shown, p))

    untracked = nul_fields(git(ident, ["ls-files", "-z", "--others", "--exclude-standard"],
                               ok=(0,)))
    candidates = {}
    for p in untracked:
        data = disk(p)
        if data is None or not data or b"\0" in data[:BINARY_PROBE]:
            continue
        candidates[p] = data
    attrs = {}
    if candidates:
        payload = b"".join(p.encode("utf-8", "surrogateescape") + b"\0" for p in candidates)
        fields = nul_fields(git(ident, ["check-attr", "-z", "--stdin", "text", "eol"], ok=(0,),
                                input_bytes=payload))
        for i in range(0, len(fields) - 2, 3):
            attrs.setdefault(fields[i], {})[fields[i + 1]] = fields[i + 2]
    for p, data in sorted(candidates.items()):
        if untracked_offender(data, attrs.get(p, {})):
            report.append("  working (untracked, not yet added): carries CR and NO eol=lf pin "
                          "covers it, so `git add` will NOT normalise it — this WILL land CRLF in "
                          "the commit: %s" % p)

    offences, marked, _raw = instrument_audit(ident)
    if offences:
        print("line-endings: FAIL — a CR instrument that cannot see a CR:", file=sys.stderr)
        print("", file=sys.stderr)
        for o in offences:
            print("  " + o, file=sys.stderr)
        print(FIX_F, file=sys.stderr)
    else:
        print("line-endings: Check F OK (no blind CR instrument; %d file(s) carry the quoted-idiom "
              "marker, this guard's own source among them)" % len(marked))

    if not report and not offences:
        if skip_history:
            print("line-endings: OK (WORKTREE ONLY — the history scan was SKIPPED, see above; the "
                  "%d HEAD / %d index blobs were NOT judged; %d working-tree paths were)"
                  % (head_n, index_n, len(rows)))
        else:
            print("line-endings: OK (committed %d + staged %d text blobs, working tree %d tracked "
                  "paths + untracked, core.autocrlf=%s; none carries CR)"
                  % (head_n, index_n, len(rows), autocrlf_shown))
        return 0
    if not report:
        return 1
    print("line-endings: FAIL — the LF contract is violated (tier named per line):")
    print("")
    for line in report:
        print(line)
    print("")
    for line in (
            "Fix:",
            "  ★ A `working (...)` line is the CHEAP one: the bytes are only on your disk, nothing",
            "    is committed yet, and converting the file to LF right now costs one command. A",
            "    `committed (HEAD)` line is the same defect after it became history.",
            "  (a) convert the file to LF (`--files PATH` shows which) and commit that rewrite ON ITS",
            "      OWN, never beside real changes; a whole-file EOL diff next to logic is unreviewable;",
            "  (b) add an `eol=lf` pin for its extension in `.gitattributes`, so a tool's platform",
            "      default can never decide this repo's bytes again (`pathlib.write_text` on Windows",
            "      is the measured culprit); OR",
            "  (c) if the file genuinely REQUIRES its 0x0D bytes, declare it `binary` in",
            "      `.gitattributes` (the `examples/**/*.bin` precedent). Not `eol=crlf`: ✔MEASURED, a",
            "      `text eol=crlf` file still stages as an LF blob.",
            "",
            "This is machine-checked because .gitattributes once pinned eol for configs",
            "but not for sources, and nothing noticed the difference."):
        print(line)
    return 1


# ── the self-test: every arm synthesizes its NEGATIVE ────────────────────────────────

def _selftest_root():
    """The guard's resolver, judged by `root_arms` (module-level: the nested-copy arm calls it
    by name inside a copy of this file)."""
    return owning_tree_module().resolve(__file__, reads_git=True)


def _fragments():
    """The blind and safe fixture lines, ASSEMBLED FROM FRAGMENTS at run time: a literal blind
    line in this file would be a true positive for check F scanning this very program."""
    q, d, b, bs2 = "'", "$", "\\", "\\"
    blind = [
        "n=%s(grep -c %s%s%sr%s f)" % (d, d, q, b, q),
        "awk %s/%sr%s/ {bad++}%s f" % (q, b, d, q),
        "sed -n %s/%sr/p%s f | wc -l" % (q, b, q),
    ]
    safe = {
        "git-blob-reader": "git grep -I -l -P %s%sr%s%s HEAD" % (q, b, d, q),
        "grep-U": "grep -U -c %s%sr%s%s f" % (q, b, d, q),
        "binary": "grep --binary -c %s%sr%s%s f" % (q, b, d, q),
        "sub": "awk %s{ sub(/%sr%s/, x) }%s f" % (q, b, d, q),
        "gsub": "awk %s{ gsub(/%sr%s/, x) }%s f" % (q, b, d, q),
        "sed-s-r": "sed %ss/%sr%s//%s f" % (q, b, d, q),
        "replace": "(Select-String -Path f -Pattern %sx%s) -replace %s%sr%s%s, %s%s" % (
            q, q, q, b, d, q, q, q),
        "tr-d": "grep -c %s%sr%s%s f tr -d" % (q, b, d, q),
        "strip-suffix": "grep -c \"%s{v%%%s%s%sr%s}\" f" % (d, d, q, bs2, q),
    }
    return blind, safe


def _fixture_repo(ot, box, name, files, add=True):
    repo = os.path.join(box, name)
    for rel, data in files.items():
        full = os.path.join(repo, *rel.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as fh:
            fh.write(data if isinstance(data, bytes) else data.encode("utf-8"))
    ot.run_git(["init", "-q", repo], capture_output=True)
    if add:
        ot.run_git(["-C", repo, "-c", "core.autocrlf=false", "add", "-A"], capture_output=True)
    return ot.TreeIdentity(os.path.realpath(repo), None, "", "")


def selftest(ident, quiet=False):
    """-> 0 when every arm held, 2 otherwise. Prints `line-endings: self-test OK (N arms)`."""
    ot = owning_tree_module()
    ran, failed = [], []

    def arm(label, fn):
        try:
            res = fn()
            ok, detail = res if isinstance(res, tuple) else (bool(res), "")
        except Exception as exc:  # noqa: BLE001 - a crashing arm FAILS, it never aborts the rest
            ok, detail = False, "CRASHED %s: %s" % (type(exc).__name__, exc)
        ran.append(label)
        if not ok:
            failed.append(label)
            print("  FAIL %s   [%s]" % (label, detail), file=sys.stderr)
        elif not quiet:
            print("  ok   %s" % label)

    def silent(fn, *a, **k):
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
            return fn(*a, **k), sink.getvalue()

    held_cwd = os.getcwd()
    box = os.path.realpath(tempfile.mkdtemp(prefix="line-endings-selftest-"))
    try:
        if ot.is_within(box, ident.root, strict=False):
            raise Unmeasured("selftest temp dir '%s' is inside the repository" % box)
        crlf, lf = os.path.join(box, "ctl_crlf.txt"), os.path.join(box, "ctl_lf.txt")
        with open(crlf, "wb") as fh:
            fh.write(bytes([97, 13, 10, 98, 10]))
        with open(lf, "wb") as fh:
            fh.write(bytes([97, 10, 98, 10]))

        def raw_bytes(p):
            with open(p, "rb") as fh:
                return fh.read()
        arm("S0a the CRLF control is exactly `a CR LF b LF` (checked as bytes, not by the counter)",
            lambda: raw_bytes(crlf) == b"a\r\nb\n")
        arm("S0b the LF twin is exactly `a LF b LF`", lambda: raw_bytes(lf) == b"a\nb\n")
        arm("S1 cr_count SEES the CR of the CRLF control", lambda: cr_count(raw_bytes(crlf)) == 1)
        arm("S2 cr_count invents no CR on the LF twin", lambda: cr_count(raw_bytes(lf)) == 0)
        arm("S3 --files over the CRLF control exits 1",
            lambda: silent(files_mode, [crlf], box)[0] == 1)
        arm("S4 --files over the LF twin exits 0", lambda: silent(files_mode, [lf], box)[0] == 0)
        arm("S5 --files over a missing path is UNMEASURED (2), never a pass",
            lambda: silent(files_mode, [os.path.join(box, "nope.txt")], box)[0] == 2)

        # ── check F, through the AUDIT'S OWN engine: one fixture repository ──────────
        blind, safe = _fragments()
        marked_line = blind[1] + "   " + MARKER
        # a blind reader whose ONLY exemption token sits in another pipeline stage
        pipeline_line = "n=$(" + blind[0].split("$(", 1)[1].rstrip(")") + " | tr -d ' ')"
        fx_files = {
            "blind.txt": "\n".join(blind) + "\n",
            "safe.txt": "\n".join(safe[k] for k in sorted(safe)) + "\n",
            "marked.txt": marked_line + "\n",
            "unmarked.txt": blind[1] + "\n",
            "region.txt": "intro\n# " + REGION_BEGIN + " quoted\n" + blind[2] + "\n# " + REGION_END
                          + "\n" + blind[0] + "\n",
            "sub(path-is-not-content).txt": blind[1] + "\n",
            "pipeline.txt": pipeline_line + "\n",
            # the audit's own positive control needs this guard's source in the fixture index
            self_rel(): raw_bytes(os.path.realpath(__file__)),
        }
        fx = _fixture_repo(ot, box, "fx", fx_files)
        fx_raw = parse_grep_n_z(git(fx, blind_grep_args(), ok=(0, 1)))
        fx_offences, _m, _r = instrument_audit(fx)

        def hits_in(path):
            return [h for h in fx_raw if h[0] == path]

        def offences_in(path):
            return [o for o in fx_offences if o.startswith(path + ":")]
        arm("S6a check F's own `git grep` FIRES on the 3 blind lines (the engine the audit runs)",
            lambda: (len(hits_in("blind.txt")) == 3 and len(offences_in("blind.txt")) == 3,
                     "raw=%d offences=%d" % (len(hits_in("blind.txt")), len(offences_in("blind.txt")))))

        def s6b():
            raw_safe = hits_in("safe.txt")
            if len(raw_safe) != len(safe) or offences_in("safe.txt"):
                return False, "raw=%d/%d offences=%r" % (len(raw_safe), len(safe),
                                                         offences_in("safe.txt"))
            dead = []
            for name, line in safe.items():
                if name in SUBSUMED_EXEMPTIONS:
                    continue
                without = tuple(e for e in CR_EXEMPTIONS if e[0] != name)
                if exempt_by_token(line, without):
                    dead.append(name)
            return not dead, "alternatives no line needs: %r" % dead
        arm("S6b every SAFE form is a real blind hit, exempted, and each exemption alternative is "
            "the ONLY one saving its line (removing it reds)", s6b)
        arm("S7 the same-line marker exempts a quoted idiom",
            lambda: len(hits_in("marked.txt")) == 1 and not offences_in("marked.txt"))
        arm("S7b the SAME line without the marker is refused",
            lambda: len(offences_in("unmarked.txt")) == 1)
        # An offence reads `<path>:<line>:<text>`; the prefix is assembled so this source
        # carries no `path:line`-shaped literal (the plan-citations ratchet counts those).
        def offence_at(path, line_no):
            return "%s:%d:" % (path, line_no)

        arm("S7c a line inside a BEGIN..END region is exempt (inclusive)",
            lambda: all(not o.startswith(offence_at("region.txt", 3)) for o in fx_offences)
            and bool([h for h in hits_in("region.txt") if h[1] == 3]))
        arm("S7d the line AFTER the region's END is refused",
            lambda: bool([o for o in offences_in("region.txt")
                          if o.startswith(offence_at("region.txt", 5))]))

        def s7e():
            fx_open = _fixture_repo(ot, box, "fx-open", {
                "open.txt": "# " + REGION_BEGIN + " never closed\n" + blind[1] + "\n",
                self_rel(): raw_bytes(os.path.realpath(__file__))})
            try:
                instrument_audit(fx_open)
            except Unmeasured as exc:
                return "OPEN at end of file" in str(exc), str(exc)[:100]
            return False, "an unclosed region was accepted"
        arm("S7e a quoted region still OPEN at end of file is refused by name", s7e)

        def s7f():
            path = "sub(path-is-not-content).txt"
            old_rule = re.search("|".join(r for _n, r in CR_EXEMPTIONS),
                                 "%s:1:%s" % (path, blind[1])) is not None
            return old_rule and len(offences_in(path)) == 1, "old-rule-exempted=%s" % old_rule
        arm("S7f an exemption token in the PATH does not exempt (the twins' rule did)", s7f)

        def s7g():
            line = [h for h in hits_in("pipeline.txt")]
            old_rule = bool(line) and re.search("|".join(r for _n, r in CR_EXEMPTIONS),
                                                line[0][2]) is not None
            return old_rule and len(offences_in("pipeline.txt")) == 1, "old-rule-exempted=%s" % old_rule
        arm("S7g an exemption token in ANOTHER pipeline stage does not exempt (the twins' rule did)",
            s7g)

        # ── one root ──────────────────────────────────────────────────────────────
        def s8a():
            try:
                ot.assert_one_root(ident)
                return True, ""
            except ot.Refusal as exc:
                return False, str(exc)[:120]
        arm("S8a git's top level for the guard's tree IS the tree its own file lives in", s8a)

        def s8b():
            me = self_rel()
            full = ot.resolve_path(ident, me)
            tracked = git(ident, ["ls-files", "-z", "--error-unmatch", "--", me], ok=(0, 1))
            return (os.path.isfile(full) and ot.same_path(full, __file__)
                    and bool(nul_fields(tracked)), "rel=%s" % me)
        arm("S8b its own file, named relative to that root, is a regular file there AND in the index",
            s8b)

        def s8c():
            me = self_rel()
            decoy_root = os.path.join(box, "decoy-cwd")
            decoy = os.path.join(decoy_root, *me.split("/"))
            os.makedirs(os.path.dirname(decoy))
            with open(decoy, "wb") as fh:
                fh.write(b"decoy bytes\n")
            os.chdir(decoy_root)
            try:
                with open(me, "rb") as fh:
                    bare = fh.read()
                got = read_tree_bytes(ident, me)
            finally:
                os.chdir(held_cwd)
            return (bare == b"decoy bytes\n" and got == raw_bytes(os.path.realpath(__file__)),
                    "negative-synthesized=%s" % (bare == b"decoy bytes\n"))
        arm("S8c standing in a decoy tree, read_tree_bytes reads the GUARD's tree (a bare relative "
            "read gets the decoy)", s8c)
        for ok, label, detail in ot.root_arms(_selftest_root, (Unmeasured, ot.Refusal), True,
                                              __file__):
            arm("S8 " + label, lambda ok=ok, detail=detail: (ok, detail))

        # ── check F's positive control ──────────────────────────────────────────
        def s9(kind):
            me = self_rel()
            files = {"plain.txt": "nothing to see\n"}
            if kind in ("control", "untracked"):
                files[me] = raw_bytes(os.path.realpath(__file__))
            repo = _fixture_repo(ot, box, "pc-" + kind, files, add=(kind != "untracked"))
            if kind == "untracked":
                with open(os.path.join(repo.root, "plain.txt"), "wb") as fh:
                    fh.write(b"nothing\n")
                ot.run_git(["-C", repo.root, "add", "plain.txt"], capture_output=True)
            try:
                instrument_audit(repo)
            except Unmeasured as exc:
                return kind != "control", str(exc)[:120]
            return kind == "control", "the audit reported OK"
        arm("S9a check F positive control: a tree whose index holds this guard -> OK",
            lambda: s9("control"))
        arm("S9b ...the guard present but NOT in the index -> UNMEASURED, naming its own file",
            lambda: s9("untracked"))
        arm("S9c ...the marker in NO file -> UNMEASURED, never OK", lambda: s9("absent"))

        def s10():
            nonrepo = os.path.join(box, "nonrepo")
            os.makedirs(nonrepo)
            fake = ot.TreeIdentity(nonrepo, None, "", "")
            try:
                git(fake, blind_grep_args(), ok=(0, 1),
                    env_extra={"GIT_CEILING_DIRECTORIES": box})
            except Unmeasured as exc:
                return "exited" in str(exc), str(exc)[:100]
            return False, "git grep over a non-repository answered"
        arm("S10 a git that cannot run is UNMEASURED, never an empty answer", s10)

        # ── the byte tiers as pure functions ──────────────────────────────────────
        def s11():
            rows = parse_eol_rows(b"\0".join([
                b"i/lf    w/crlf  attr/text eol=lf      \tpinned-crlf.cpp",
                b"i/lf    w/mixed attr/text eol=lf      \tpinned-mixed.cpp",
                b"i/lf    w/lf    attr/text eol=lf      \tpinned-lf.cpp",
                b"i/lf    w/-text attr/text eol=lf      \tpinned-binary-worktree.cpp",
                b"i/lf            attr/text eol=lf      \tpinned-missing.cpp",
                b"i/lf    w/crlf  attr/                 \tunpinned-crlf.txt",
                b"i/-text w/crlf  attr/                 \tunpinned-binary.dat",
                b"i/none  w/crlf  attr/                 \tunpinned-empty.txt",
                b"i/-text w/-text attr/text eol=lf      \tpinned-binary-index.cpp",
                b"i/-text w/-text attr/                 \tsrc/legacy-glob.hpp",
            ]) + b"\0")
            d, e1, c = check_d(rows), check_e1(rows), check_c(rows)
            return (d == ["pinned-crlf.cpp", "pinned-mixed.cpp"] and e1 == ["unpinned-crlf.txt"]
                    and c == ["pinned-binary-index.cpp", "src/legacy-glob.hpp"],
                    "D=%r E1=%r C=%r" % (d, e1, c))
        arm("S11 D/E1/C take CLOSED row sets: only w/crlf|w/mixed convict, i/-text and i/none leave "
            "E1, the legacy glob list stays inside C", s11)
        arm("S12 core.autocrlf is read as git reads a boolean",
            lambda: all(autocrlf_is_true(v) for v in ("true", "TRUE", "yes", "on", "1"))
            and not any(autocrlf_is_true(v) for v in ("false", "", "input", "0", "no")))

        def s13():
            nul_late = b"a\r\n" + b"x" * (BINARY_PROBE + 5) + b"\0"
            return (not untracked_offender(b"a\r\n\0", {})
                    and untracked_offender(nul_late, {})
                    and not untracked_offender(b"", {})
                    and not untracked_offender(b"a\r\n", {"text": "unset"})
                    and not untracked_offender(b"a\r\n", {"eol": "lf"})
                    and untracked_offender(b"a\r\n", {"text": "set", "eol": "unspecified"})
                    and not untracked_offender(b"a\n", {}))
        arm("S13 E2 skips binary (NUL in the first 8000 bytes), empty, -text and eol=lf files and "
            "convicts a CR otherwise", s13)

        def s14():
            disk = {"stale.c": b"a\nb\n", "live.c": b"a\r\n"}
            return (stale_evidence(["live.c", "stale.c"], disk.get) == "stale.c"
                    and stale_evidence(["live.c"], disk.get) is None
                    and stale_evidence(["gone.c"], disk.get) is None)
        arm("S14 stale-checkout evidence: a history offender with ZERO CR on disk, and only that", s14)

        def s15():
            try:
                positive_control([("HEAD", 5)], floor=10)
            except Unmeasured as exc:
                return "below its floor of 10" in str(exc), str(exc)[:80]
            return False, "a collapsed scan passed"
        arm("S15 a scan below its floor is UNMEASURED", s15)

        def s16():
            a, b = os.path.join(box, "cwd-a"), os.path.join(box, "cwd-b")
            os.makedirs(a)
            os.makedirs(b)
            with open(os.path.join(a, "x.txt"), "wb") as fh:
                fh.write(b"a\r\n")
            with open(os.path.join(b, "x.txt"), "wb") as fh:
                fh.write(b"a\n")
            return silent(files_mode, ["x.txt"], a)[0] == 1 and silent(files_mode, ["x.txt"], b)[0] == 0
        arm("S16 a relative --files path resolves against where the CALLER stood", s16)

        def s17():
            lst = os.path.join(box, "list.txt")
            with open(lst, "wb") as fh:
                fh.write(b"one.txt\r\n\r\ntwo.txt\r\n")
            empty = os.path.join(box, "empty-list.txt")
            with open(empty, "wb") as fh:
                fh.write(b"\n\n")
            got = read_path_list(lst)
            return (got == ["one.txt", "two.txt"] and silent(files_mode, read_path_list(empty), box)[0] == 2,
                    "got=%r" % got)
        arm("S17 --files-from: a CRLF list and empty lines read cleanly; an empty list exits 2", s17)
    except Exception as exc:  # noqa: BLE001 - a FIXTURE crash stops the arms after it, by name
        failed.append("fixture construction")
        print("  FAIL fixture construction crashed after %d arm(s): %s: %s"
              % (len(ran), type(exc).__name__, exc), file=sys.stderr)
    finally:
        os.chdir(held_cwd)
        owning_tree_module().remove_tree(box)

    if len(ran) != EXPECTED_SELFTEST_ARMS:
        print("line-endings: FAIL — the self-test ran %d arm(s), %d expected. An arm that silently "
              "stops running is a property that silently stops being proven."
              % (len(ran), EXPECTED_SELFTEST_ARMS), file=sys.stderr)
        return 2
    if failed:
        print("line-endings: FAIL — the SELF-TEST failed (%d of %d arms, above). This guard cannot "
              "be trusted until it passes; do not silence it." % (len(failed), len(ran)),
              file=sys.stderr)
        return 2
    print("line-endings: self-test OK (%d arms)" % len(ran))
    return 0


# ── proving the bound ────────────────────────────────────────────────────────────────

def _alive(pid):
    """Whether a process id is still running (POSIX signal 0; Windows OpenProcess + wait)."""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        kernel32 = ctypes.windll.kernel32
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        handle = kernel32.OpenProcess(0x00100000 | 0x1000, False, pid)  # SYNCHRONIZE | QUERY_LIMITED
        if not handle:
            return False
        try:
            return kernel32.WaitForSingleObject(handle, 0) != 0   # WAIT_OBJECT_0 == exited
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def watchdog_selftest():
    """W1–W6: a hanging probe is cut short within its budget and NAMES what it waited on; a
    named control still passes; and a probe blocked on a GRANDCHILD dies with nothing left
    holding its pipe. Every child is waited on through `spawn`, so this run is bounded too."""
    me = os.path.realpath(__file__)
    fails = []

    def check(ok, label):
        if not ok:
            fails.append(label)
            print("line-endings: FAIL — watchdog selftest: %s" % label, file=sys.stderr)

    t0 = time.monotonic()
    rc, out, err = spawn([sys.executable, me, "--watchdog-probe", str(PROBE_DEFAULT_S)],
                         what="watchdog self-test W1: waiting on the hanging probe")
    elapsed = time.monotonic() - t0
    text = (out + err).decode("utf-8", "replace")
    check(rc == 2, "W1 the hanging probe exited %s, not 2 — the bound did not hold" % rc)
    check(elapsed <= 60, "W2 a %d s budget took %.0fs to fire" % (PROBE_DEFAULT_S, elapsed))
    check("TIMED OUT after %ds" % PROBE_DEFAULT_S in text,
          "W3 the refusal never said it timed out:\n    " + text.replace("\n", "\n    "))
    check("it was waiting on : SYNTHETIC watchdog probe" in text,
          "W4 the refusal did not name the activity it was waiting on")

    box = os.path.realpath(tempfile.mkdtemp(prefix="line-endings-watchdog-"))
    try:
        ctl = os.path.join(box, "ctl.txt")
        with open(ctl, "wb") as fh:
            fh.write(b"a\nb\n")
        t0 = time.monotonic()
        rc, out, err = spawn([sys.executable, me, "--files", ctl],
                             what="watchdog self-test W5: waiting on the control run")
        elapsed = time.monotonic() - t0
        check(rc == 0 and elapsed <= 60,
              "W5 CONTROL: an ordinary --files run over a clean file returned rc=%d in %.0fs" % (
                  rc, elapsed))
    finally:
        owning_tree_module().remove_tree(box)

    t0 = time.monotonic()
    rc, out, err = spawn([sys.executable, me, "--watchdog-probe-child", str(PROBE_DEFAULT_S)],
                         what="watchdog self-test W6: waiting on a probe blocked on a grandchild")
    elapsed = time.monotonic() - t0
    text = (out + err).decode("utf-8", "replace")
    m = re.search(r"GRANDCHILD-PID=(\d+)", text)
    pid = int(m.group(1)) if m else None
    gone = False
    if pid is not None:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and _alive(pid):
            time.sleep(0.2)
        gone = not _alive(pid)
    check(rc == 2 and elapsed <= 60 and "sleep(3600)" in text and gone,
          "W6 a probe blocked on a registered grandchild: rc=%s elapsed=%.0fs named-its-argv=%s "
          "grandchild-gone=%s (pid %s)" % (rc, elapsed, "sleep(3600)" in text, gone, pid))
    if fails:
        print("line-endings: FAIL — the WATCHDOG self-test failed (%d of %d arms, above)."
              % (len(fails), EXPECTED_WATCHDOG_ARMS), file=sys.stderr)
        return 2
    print("line-endings: watchdog OK (%d arms: a hanging probe was cut short inside its budget, "
          "the refusal named the timeout AND its subject, a named CONTROL run still passed, and "
          "a probe blocked on a grandchild died with nothing left holding its pipe)"
          % EXPECTED_WATCHDOG_ARMS)
    return 0


# ── entry ────────────────────────────────────────────────────────────────────────────

USAGE = __doc__.split("Usage:", 1)[1].strip()


def _budget_arg(rest):
    if not rest:
        return PROBE_DEFAULT_S
    if len(rest) > 1 or not rest[0].isdigit() or int(rest[0]) < 1:
        raise ValueError("a watchdog probe takes one integer budget >= 1")
    return int(rest[0])


def main(argv):
    global _WD
    mode = argv[0] if argv else ""
    rest = argv[1:]
    known = ("", "--help", "-h", "--files", "--files-from", "--audit-instruments", "--selftest",
             "--selftest-watchdog", "--watchdog-probe", "--watchdog-probe-child")
    if mode not in known:
        print("line-endings: FAIL — unknown argument '%s' (see --help)" % mode, file=sys.stderr)
        return 2
    if mode in ("", "--help", "-h", "--audit-instruments", "--selftest", "--selftest-watchdog") and rest:
        print("line-endings: FAIL — %s takes no further arguments, got %r (see --help)"
              % (mode or "the default mode", rest), file=sys.stderr)
        return 2
    budget = WATCHDOG_BUDGET_S
    if mode in ("--watchdog-probe", "--watchdog-probe-child"):
        try:
            budget = _budget_arg(rest)
        except ValueError as exc:
            print("line-endings: FAIL — %s (see --help)" % exc, file=sys.stderr)
            return 2
    _WD = Watchdog(budget)
    _WD.start()
    invoked_from = os.getcwd()
    try:
        if mode in ("--help", "-h"):
            print(USAGE)
            return 0
        if mode == "--files":
            return files_mode(rest, invoked_from)
        if mode == "--files-from":
            if len(rest) != 1:
                print("line-endings: FAIL — --files-from needs a FILE (or -)", file=sys.stderr)
                return 2
            return files_mode(read_path_list(rest[0]), invoked_from)
        if mode == "--watchdog-probe":
            _WD.note("SYNTHETIC watchdog probe — this run checks nothing and is waiting to be killed")
            for _ in range(budget * 3 + 30):
                time.sleep(1)
            print("line-endings: FAIL — the watchdog did NOT fire within its budget; the bound is "
                  "not holding (budget %ds, this probe waited %ds)." % (budget, budget * 3 + 30),
                  file=sys.stderr)
            return 2
        if mode == "--watchdog-probe-child":
            argv_child = [sys.executable, "-c", "import time; time.sleep(3600)"]
            proc = subprocess.Popen(argv_child, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE,
                                    **({} if os.name == "nt" else {"start_new_session": True}))
            _WD.register(proc)
            print("GRANDCHILD-PID=%d" % proc.pid, file=sys.stderr, flush=True)
            _WD.note("SYNTHETIC grandchild wait: %s" % " ".join(argv_child))
            proc.communicate()
            print("line-endings: FAIL — the grandchild returned before the watchdog fired.",
                  file=sys.stderr)
            return 2
        if mode == "--selftest-watchdog":
            return watchdog_selftest()

        _WD.note("owning-tree: resolving which tree this guard lives in")
        ot = owning_tree_module()
        try:
            root = ot.owning_tree(__file__)
        except ot.Refusal as exc:
            raise Unmeasured(str(exc))
        if not shutil.which("git"):
            raise Unmeasured("git is not on PATH. This guard reads BLOBS, so it cannot fall back to "
                             "the working tree (a CRLF checkout would false-red and an LF checkout "
                             "would false-green). Refusing to report a pass.")
        _WD.note("owning-tree: `git rev-parse --local-env-vars`, then the tree's identity "
                 "(`git rev-parse HEAD`)")
        try:
            ident = ot.identity(root)
        except ot.Refusal as exc:
            raise Unmeasured("HEAD does not resolve; this is not a git work tree with a commit "
                             "(%s). Refusing to report a pass over a tree it cannot read." % exc)
        if mode == "--audit-instruments":
            return run_audit_mode(ident)
        if mode == "--selftest":
            return selftest(ident)
        git(ident, ["rev-parse", "--verify", "--quiet", "HEAD"], ok=(0,))
        _WD.note("owning-tree: git's top level through the identity (the one-root check)")
        try:
            ot.assert_one_root(ident)
        except ot.Refusal as exc:
            raise Unmeasured(str(exc))
        if selftest(ident, quiet=True) != 0:
            return 2
        return verify(ident)
    except Unmeasured as exc:
        print("line-endings: FAIL — %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    try:
        code = main(sys.argv[1:])
    except Exception as exc:  # noqa: BLE001 - a crash is UNMEASURED (2), never a finding (1)
        print("line-endings: FAIL — crashed: %s: %s" % (type(exc).__name__, exc), file=sys.stderr)
        code = 2
    sys.stdout.flush()
    sys.stderr.flush()
    sys.exit(code)
