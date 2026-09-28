#!/usr/bin/env python3
"""sqlite_common.py -- the shared spine of the SQLite corpus harness.

`build_and_test.py` is the ONE driver (it replaced `build-and-test.sh` and its Windows twin
`build-and-test.ps1` on 2026-09-21, lane mig, part 4: no `.sh`/`.ps1` under the actions
directory). Every module of the port imports this file for the same six things, so none of
them grows a private copy:
  * the LOG vocabulary (`step`/`info`/`ok`/`warn` on stdout, `die` = `HarnessDie`, exit 1);
  * the CONFIGURATION read from the environment, validated up front (a malformed number or a
    three-state value outside its spellings is a named refusal, never a crash mid-run);
  * CHILD PROCESSES: argument lists only, `stdin=DEVNULL`, explicit `env=` dicts in which an
    EMPTY value means DELETED (an empty-but-set `SQLITE_TEST_PATTERN_LIST` carried across
    WSLENV selected zero files and read as green -- MEASURED on the .ps1 side), UTF-8 decoding,
    `sys.executable` for every Python child;
  * the RESOLVER (`harness_legs.py`), always through its verb CLI with `--format json`, so its
    exit-code contracts (0/2/3/4) stay the ones its own self-test proves;
  * the LEG LEDGER: the closed verdict vocabulary guards BOTH recorders -- an empty or unknown
    token becomes `poisoned` with `HARNESS DEFECT: …`, is counted as unclassified, and the run
    cannot exit 0 (the union of the .sh's unit-level guard and the .ps1's leg-level one);
  * the HOST'S CAPABILITIES: a WSL that is USABLE, not merely present (`wsl_usable`), and ONE
    mapping of a missing capability (`capability_arm`) -- a FAIL on a harness host, which every host
    is unless it declares `DSS_SQLITE_HARNESS_HOST=0`, and a named SKIP on a host that does.

Nothing here runs at import (the programs beside it import it; `check-guard-output-encoding`
imports every primary program in a child).
"""
from __future__ import annotations

import collections
import contextlib
import importlib.util
import json
import os
import platform
import shutil
import subprocess
import sys

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass
# The action is `requireInputsUnmoved`: no `__pycache__` may appear beside its programs.
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.realpath(__file__))
LEGS_JSON = os.path.join(HERE, "legs.json")
HARNESS_LEGS = os.path.join(HERE, "harness_legs.py")
MANIFEST_GEN = os.path.join(HERE, "gen-pe64-manifest.py")
CLI_SMOKE = os.path.join(HERE, "cli-smoke.py")
BENCH_CORE = os.path.join(HERE, "speedtest1_bench.py")
STAGE_ZINC = os.path.join(HERE, "stage-zinc.py")
OWNING_TREE = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))),
                           "owning-tree", "owning-tree.py")

# The em dash the ledger lines carry (`not run [<tok>] — <detail>`); Step 9 and the tests
# read it byte for byte.
DASH = "—"


class HarnessDie(Exception):
    """A run-fatal refusal: printed as `✗ ERROR: …` on stderr, exit 1."""
    exit_code = 1


class CloneLockBlocked(HarnessDie):
    """Another harness run owns the shared sqlite clone: exit 3, first stderr line
    `DSS-CLONE-LOCK-BLOCKED` (a contract callers read)."""
    exit_code = 3


# ── the log ─────────────────────────────────────────────────────────────────────────

class Log:
    """step/info/ok/warn go to STDOUT (warn included, as both drivers did), die raises."""

    def __init__(self, stream=None):
        self.stream = stream or sys.stdout
        tty = hasattr(self.stream, "isatty") and self.stream.isatty()
        self.c = {"rst": "\033[0m", "red": "\033[31m", "grn": "\033[32m", "ylw": "\033[33m",
                  "blu": "\033[34;1m"} if tty else collections.defaultdict(str)

    def _p(self, text):
        print(text, file=self.stream, flush=True)

    def step(self, msg):
        self._p("\n%s== %s ==%s" % (self.c["blu"], msg, self.c["rst"]))

    def info(self, msg):
        for line in str(msg).split("\n"):
            self._p("   %s" % line)

    def ok(self, msg):
        self._p("%s ✓ %s%s" % (self.c["grn"], msg, self.c["rst"]))

    def warn(self, msg):
        self._p("%s ! %s%s" % (self.c["ylw"], msg, self.c["rst"]))

    @staticmethod
    def die(msg):
        raise HarnessDie(msg)


LOG = Log()


def die(msg):
    raise HarnessDie(msg)


# ── configuration ───────────────────────────────────────────────────────────────────

TRISTATE_ON = ("1", "true", "TRUE", "yes")
TRISTATE_OFF = ("", "0", "false", "FALSE", "no")


def env(name, default=""):
    v = os.environ.get(name)
    return default if v is None else v


def path_knob(value, name):
    """A path-valued knob (SRC_DIR, OUT_DIR), normalised ONCE for every mode that reads it: "" when unset or empty,
    the value without its surrounding blanks otherwise, and a value of blanks alone REFUSED -- it names nothing,
    and read as a relative path it named a directory of blanks under the working directory. (P68 round 13's audit,
    F3-A-SP-8: the driver passed OUT_DIR / SRC_DIR raw and the benchmark stripped them, so one padded value gave the
    two modes two trees and two run locks.)"""
    v = value or ""
    if v and not v.strip():
        die("%s=%r names nothing but blanks: unset it, or name a directory." % (name, v))
    return v.strip()


def tristate(name):
    """A three-state switch read EXACTLY as both drivers read it: an ON spelling, an OFF
    spelling (unset/empty included), or a refusal -- a typo is never silently OFF."""
    v = env(name, "")
    if v in TRISTATE_ON:
        return True
    if v in TRISTATE_OFF:
        return False
    die("%s='%s' is not a value this harness recognises. ON is one of %s; OFF is one of %s "
        "(or unset)." % (name, v, " ".join(TRISTATE_ON), " ".join(repr(x) for x in TRISTATE_OFF)))


def int_env(name, default, minimum=0):
    """An integer environment value, validated BEFORE anything runs (the .sh died in shell
    arithmetic mid-run, the .ps1 threw from an `[int]` cast)."""
    v = env(name, "").strip()
    if v == "":
        return default
    try:
        n = int(v)
    except ValueError:
        die("%s='%s' is not an integer." % (name, v))
    if n < minimum:
        die("%s=%d is below its minimum of %d." % (name, n, minimum))
    return n


def split_list(value):
    """A comma- or whitespace-separated list, verbatim tokens, no globbing (the .sh word-split
    AND glob-expanded; the .ps1 split on `[,\\s]+`)."""
    return [t for t in value.replace(",", " ").split() if t]


# ── child processes ─────────────────────────────────────────────────────────────────

Result = collections.namedtuple("Result", ["rc", "out", "err"])


def child_env(overrides=None, base=None, python=False):
    """A child's environment: `base` (default this process's) with `overrides` applied, where
    None or an EMPTY string DELETES the name. Python children get UTF-8 I/O and no bytecode."""
    e = dict(os.environ if base is None else base)
    for k, v in (overrides or {}).items():
        if v is None or v == "":
            e.pop(k, None)
        else:
            e[k] = str(v)
    if python:
        e["PYTHONUTF8"] = "1"
        e["PYTHONDONTWRITEBYTECODE"] = "1"
        e["PYTHONIOENCODING"] = "utf-8"
    return e


def capture(argv, cwd=None, env_=None, timeout=None, input_text=None, merge=False):
    """Run `argv`; NEVER raise on its exit code. -> Result(rc, stdout, stderr) as text
    (UTF-8, errors replaced). A program that cannot start is rc 127 with the reason in `err`;
    a timeout is rc 124. `merge=True` folds stderr into stdout (the order the child wrote)."""
    try:
        p = subprocess.run(list(argv), cwd=cwd, env=env_, timeout=timeout,
                           input=input_text if input_text is not None else None,
                           stdin=None if input_text is not None else subprocess.DEVNULL,
                           stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT if merge else subprocess.PIPE,
                           text=True, encoding="utf-8", errors="replace")
    except FileNotFoundError as exc:
        return Result(127, "", "cannot start %s: %s" % (argv[0], exc))
    except OSError as exc:
        return Result(127, "", "cannot start %s: %s" % (argv[0], exc))
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout if isinstance(exc.stdout, str) else (exc.stdout or b"").decode(
            "utf-8", "replace")
        return Result(124, out, "timed out after %ss" % timeout)
    return Result(p.returncode, p.stdout or "", "" if merge else (p.stderr or ""))


def run_checked(argv, what, **kw):
    """`capture`, and a run-fatal refusal naming `what` when the exit code is not 0 -- the
    Python form of every command the .sh ran under its ERR trap."""
    r = capture(argv, **kw)
    if r.rc != 0:
        tail = "\n".join((r.err or r.out).strip().splitlines()[-12:])
        die("%s FAILED (rc=%d): %s\n%s" % (what, r.rc, " ".join(str(a) for a in argv), tail))
    return r


def python_argv(script, *args):
    """A Python child: THIS interpreter, never `python3` by name (on Windows `python3` can be the
    Store stub; on a macOS login shell it can be another interpreter)."""
    return [sys.executable, script] + [str(a) for a in args]


def first_lines(text, n=6):
    return "\n".join((text or "").strip().splitlines()[:n])


def last_lines(text, n=6):
    return "\n".join((text or "").strip().splitlines()[-n:])


def tail_file(path, n=30):
    """The last `n` lines of a (possibly huge) log, read from its end, bytes decoded leniently."""
    try:
        with open(path, "rb") as fh:
            fh.seek(0, os.SEEK_END)
            size = fh.tell()
            block = min(size, 256 * 1024)
            fh.seek(size - block)
            data = fh.read()
    except OSError:
        return ""
    return "\n".join(data.decode("utf-8", "replace").replace("\r", "").split("\n")[-n:])


# ── the host ────────────────────────────────────────────────────────────────────────

def host_os():
    """`linux`, `darwin` or `windows` -- the resolver's vocabulary; anything else refused."""
    s = platform.system()
    table = {"Linux": "linux", "Darwin": "darwin", "Windows": "windows"}
    if s not in table:
        die("unsupported host OS: platform.system() = %r (this harness runs on linux, darwin "
            "and windows hosts)" % s)
    return table[s]


def host_arch():
    """`x86_64` or `arm64`, the OPERATING SYSTEM's view (an x64 Python on an ARM64 Windows
    reports AMD64 as its own machine; `PROCESSOR_ARCHITEW6432` names the OS's)."""
    if platform.system() == "Windows":
        m = os.environ.get("PROCESSOR_ARCHITEW6432") or os.environ.get(
            "PROCESSOR_ARCHITECTURE") or platform.machine()
    else:
        m = platform.machine()
    table = {"x86_64": "x86_64", "amd64": "x86_64", "x64": "x86_64",
             "arm64": "arm64", "aarch64": "arm64"}
    got = table.get(m.lower())
    if got is None:
        die("unsupported host arch: %r (this harness knows x86_64 and arm64)" % m)
    return got


def host_is_wsl():
    """For the banner only: a Linux kernel built by Microsoft."""
    try:
        with open("/proc/version", "r", encoding="utf-8", errors="replace") as fh:
            v = fh.read().lower()
        return "microsoft" in v or "wsl" in v
    except OSError:
        return False


class PosixSide:
    """Where this harness's POSIX half runs: in THIS process on a POSIX host, and inside WSL
    (`wsl.exe -e …`, never a login shell, never `wsl.exe --`) on a Windows host -- the ONE
    host-keyed switch the Windows driver had (`HostNeedsWsl`), which decides WHERE the POSIX
    toolchain runs and never which legs exist."""

    def __init__(self, host):
        self.needs_wsl = host == "windows"

    def argv(self, args):
        return (["wsl.exe", "-e"] + [str(a) for a in args]) if self.needs_wsl else [str(a) for a in args]

    def to_posix(self, path):
        """A host path in the POSIX side's namespace (`wslpath -a -u` inside WSL)."""
        if not self.needs_wsl:
            return path
        r = capture(["wsl.exe", "-e", "wslpath", "-a", "-u", path], timeout=60)
        out = r.out.replace("\0", "").strip().splitlines()
        if r.rc != 0 or not out:
            die("could not translate '%s' into WSL's namespace (wsl.exe -e wslpath -a -u exited %d: %s)"
                % (path, r.rc, (r.err or r.out).replace("\0", "").strip()[:200]))
        return out[-1].strip()

    def to_host(self, path):
        """A POSIX-side path in THIS host's namespace (`wslpath -m`, forward slashes)."""
        if not self.needs_wsl:
            return path
        r = capture(["wsl.exe", "-e", "wslpath", "-m", path], timeout=60)
        out = r.out.replace("\0", "").strip().splitlines()
        if r.rc != 0 or not out:
            die("could not translate the WSL path '%s' back to this host (wslpath -m exited %d)"
                % (path, r.rc))
        return out[-1].strip()


def which(name):
    return shutil.which(name)


# ── what this host can do, and whether it must ──────────────────────────────────────
#
# ★ PRESENCE IS NOT USE. Windows ships `wsl.exe` whether or not a distribution is installed:
# ✔MEASURED 2026-09-28 on GitHub's windows-latest runner (Pipeline run 36427818588) --
# `shutil.which("wsl.exe")` answered, and every command through it exited 4294967295 with
# "Windows Subsystem for Linux has no installed distributions". Arms that asked only for the
# program failed there, and a section crashed translating a path through it.
#
# ★ A HARNESS HOST REQUIRES WHAT A TEST HOST MAY LACK. The hosts this harness RUNS on -- the gate's
# four -- need what it runs there: git on every one (it clones and pins the subject); a usable WSL on
# Windows; on a POSIX host make, cc and a Tcl the stage can run, with its development files. There an arm that finds one missing FAILS, naming it: a
# gate host that lost it is a red gate, not a quiet skip. A host that only runs the tree's tests -- a
# CI runner -- DECLARES so with `DSS_SQLITE_HARNESS_HOST=0` (the CI workflow's Test step does), and
# there the same arm is a counted SKIP that names what is missing, so the run says what it could not
# prove instead of failing on a host the harness never runs on.
# ★ EVERY HOST IS A HARNESS HOST UNTIL IT DECLARES OTHERWISE, because a declaration can be LOST. The
# first draft (P68's PR exit, lane ci59) read an unset variable as "a test host" and gave each gate
# host `=1` in its DssHarness `env`. Its independent review showed where that fails toward clean: a
# host added without the entry, a more specific env layer replacing it, ctest run by hand on the gate
# machine itself -- each turned every capability arm on a gate host into a skip, and the run stayed
# green, which is the one outcome this policy exists to prevent. Now a lost declaration fails toward
# RED: a CI runner that lost its `0` fails, naming the capability it lacks and the variable.
# ★ ONE MAPPING, `capability_arm`: a site hands it its self-test's own check and skip, so no site
# decides on its own what a missing capability means; each site's arm is pinned by injecting the lack.

HARNESS_HOST_ENV = "DSS_SQLITE_HARNESS_HOST"
TEST_ONLY_HOST = "0"
# the words that end a test-only host's skip, and only that skip (`site_pinned` reads them)
TEST_ONLY_SKIP_MARK = "UNPROVEN here rather than failed"
WSL_ANSWER = "dss-wsl-answers"
WSL_PROBE_TIMEOUT_S = 120
_WSL_USABLE = None


def harness_host():
    """True unless this process DECLARES a host that only runs the tree's tests
    (`DSS_SQLITE_HARNESS_HOST=0`, as the CI workflow's Test step does). Unset, empty and `1` all read as
    a HARNESS HOST -- the strict reading, so a declaration that never arrives fails toward red. Any
    other spelling is refused, never read as either."""
    value = os.environ.get(HARNESS_HOST_ENV)
    if value is None or value in ("", "1"):
        return True
    if value == TEST_ONLY_HOST:
        return False
    die("%s=%r: its only spellings are %s (a host that only runs the tree's tests, as the CI workflow's Test step "
        "declares) and 1, empty or unset (a host this harness runs on, the default)"
        % (HARNESS_HOST_ENV, value, TEST_ONLY_HOST))


def harness_host_line():
    """What a self-test prints FIRST about this host, read EAGERLY: a malformed declaration is refused
    before any arm runs, not only where a capability happens to be missing."""
    if harness_host():
        return "a harness host: yes (%s declares no test-only host)" % HARNESS_HOST_ENV
    return "a harness host: no (%s=%s: a host that only runs the tree's tests)" % (HARNESS_HOST_ENV, TEST_ONLY_HOST)


def capability_verdict(what, have, why):
    """One verdict for an arm that needs a HOST capability -> (verdict, detail): ("ok", "") when the
    host has it; else ("fail", …) on a harness host, which needs it, and ("skip", …) on any other,
    each naming `what` is missing and `why`, the measured reason."""
    if have:
        return "ok", ""
    if harness_host():
        return "fail", ("this is a HARNESS HOST (%s is not %s), and it lacks %s: %s -- a host that only runs "
                        "the tree's tests declares %s=%s"
                        % (HARNESS_HOST_ENV, TEST_ONLY_HOST, what, why, HARNESS_HOST_ENV, TEST_ONLY_HOST))
    return "skip", ("this host lacks %s (%s); it declares that it only runs the tree's tests (%s=%s), so this is "
                    "%s" % (what, why, HARNESS_HOST_ENV, TEST_ONLY_HOST, TEST_ONLY_SKIP_MARK))


def capability_arm(check, skip, label, what, have, why, facts=""):
    """Record ONE capability arm through a self-test's own `check(label, ok, detail)` and
    `skip(label, why)` -> its verdict: a PASS where the host has `what`; a FAIL naming it on a
    harness host that lacks it (`facts` appended: what a failing host arm prints about the host); a
    counted SKIP naming it on a host that declares it only runs the tree's tests."""
    verdict, detail = capability_verdict(what, have, why)
    if verdict == "skip":
        skip(label, detail)
    else:
        check(label, verdict == "ok", detail + (facts if verdict == "fail" else ""))
    return verdict


@contextlib.contextmanager
def declared_host(value):
    """The self-tests' injection: the body runs as a host whose `DSS_SQLITE_HARNESS_HOST` is `value`
    (None = unset); the variable is restored however the body ends."""
    saved = os.environ.get(HARNESS_HOST_ENV)
    try:
        if value is None:
            os.environ.pop(HARNESS_HOST_ENV, None)
        else:
            os.environ[HARNESS_HOST_ENV] = value
        yield
    finally:
        if saved is None:
            os.environ.pop(HARNESS_HOST_ENV, None)
        else:
            os.environ[HARNESS_HOST_ENV] = saved


class ArmRecorder:
    """A self-test's `check` and `skip` that RECORD instead of reporting: a site's capability arm run
    under an injected lack is read back from `rows` -- (label, "pass" | "fail" | "skip", detail) -- so
    the site's own mapping is pinned without its injected FAIL reaching the run's report."""

    def __init__(self):
        self.rows = []

    def check(self, label, ok, detail=""):
        self.rows.append((label, "pass" if ok else "fail", str(detail)))

    def skip(self, label, why):
        self.rows.append((label, "skip", str(why)))

    def outcomes(self):
        return [(label, how) for label, how, _d in self.rows]


def site_pinned(site, lack):
    """A capability SITE, run twice under an injected lack -> (held, detail). `site(recorder)` records
    through `recorder.check`/`recorder.skip`; `lack` is the reason text the injection hands it. It holds
    when, as an undeclared host (a harness host), the site FAILS an arm naming the lack and passes none,
    and, declared a test-only host, it SKIPS those arms naming the lack and fails none."""
    runs = {}
    for value in (None, TEST_ONLY_HOST):
        rec = ArmRecorder()
        with declared_host(value):
            site(rec)
        runs[value] = rec
    strict, test_only = runs[None].rows, runs[TEST_ONLY_HOST].rows
    failed = [label for label, how, d in strict if how == "fail" and lack in d and HARNESS_HOST_ENV in d]
    skipped = [label for label, how, d in test_only if how == "skip" and lack in d]
    # a harness host never takes the TEST-ONLY skip -- not even in a site with a second capability arm
    # beside one that did fail (a site's dependent skips name the arm that judged, not the declaration)
    strict_test_only_skips = [label for label, how, d in strict if how == "skip" and TEST_ONLY_SKIP_MARK in d]
    held = (bool(failed) and not any(how == "pass" for _l, how, _d in strict) and not strict_test_only_skips
            and set(failed) <= set(skipped) and not any(how == "fail" for _l, how, _d in test_only))
    return held, "as a harness host: %r\ndeclared test-only: %r" % (runs[None].outcomes(),
                                                                    runs[TEST_ONLY_HOST].outcomes())


def wsl_usable(runner=None, which_=None):
    """(usable, why) for this host's WSL: usable when `wsl.exe` is on PATH AND runs a command in a
    distribution -- `wsl.exe -e echo <WSL_ANSWER>` exits 0 and prints the token. `why` is the measured
    reason it is not, in wsl.exe's own words (its UTF-16 output read without the NULs). The real host
    is probed once per process; `runner` (argv -> Result) and `which_` are the self-tests' injections."""
    global _WSL_USABLE
    real = runner is None and which_ is None
    if real and _WSL_USABLE is not None:
        return _WSL_USABLE
    if not (which_ or shutil.which)("wsl.exe"):
        got = (False, "wsl.exe is not on PATH")
    else:
        argv = ["wsl.exe", "-e", "echo", WSL_ANSWER]
        r = (runner or (lambda a: capture(a, timeout=WSL_PROBE_TIMEOUT_S)))(argv)
        if r.rc == 0 and WSL_ANSWER in (r.out or "").replace("\0", ""):
            got = (True, "")
        else:
            said = " ".join(("%s %s" % (r.err or "", r.out or "")).replace("\0", "").split())[:300]
            # rc 124 is `capture`'s timeout: a wsl.exe that never answered, not one that ran nothing
            how = ("did not answer within %ds" % WSL_PROBE_TIMEOUT_S) if r.rc == 124 else "runs nothing"
            got = (False, "wsl.exe is on PATH but %s: `%s` exited %d%s"
                   % (how, " ".join(argv), r.rc, (": " + said) if said else " and printed nothing"))
    if real:
        _WSL_USABLE = got
    return got


# ── the tree this harness lives in ──────────────────────────────────────────────────

_OT = None


def owning_tree_module():
    """`owning-tree.py`, loaded as a sibling by path (the one owner of "which tree am I in")."""
    global _OT
    if _OT is None:
        if not os.path.isfile(OWNING_TREE):
            die("cannot find %s -- the harness names its own tree there and nowhere else"
                % OWNING_TREE)
        spec = importlib.util.spec_from_file_location("dss_owning_tree", OWNING_TREE)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _OT = mod
    return _OT


def driver_tree():
    """The DSS tree this harness file lives in, or None when it lives in none (a copy)."""
    ot = owning_tree_module()
    try:
        return ot.owning_tree(__file__)
    except ot.Refusal:
        return None


# ── the output tree, and the one run lock on it ─────────────────────────────────────

# The run lock of an output tree (`sqlite_procs.RunLock`), one name for every mode that takes it: the
# driver's run, the round-close recompile and the speedtest1 benchmark all hold `<output tree>/<RUN_LOCK>`,
# so two of them on one tree SERIALIZE -- on every host, Windows included (atomic mkdir, liveness-checked).
RUN_LOCK = ".harness-lock"


def output_tree(repo_root, host, out_dir=""):
    """The sqlite harness's OUTPUT TREE, the ONE rule every mode reads: OUT_DIR (`out_dir`, when set -- as the
    variable holds it: `path_knob` normalises it here, once, for every caller), else
    `<repo_root>/build/real-examples/c/sqlite`, under `windows/` on a Windows host. The driver's run and its
    stage live under it, the recompile finds that stage there, and the speedtest1 benchmark keeps its pinned
    checkout, its scratch and its default output there -- inside the tree, never beside it."""
    out_dir = path_knob(out_dir, "OUT_DIR")
    if out_dir:
        return os.path.abspath(out_dir)
    return os.path.join(repo_root, "build", "real-examples", "c", "sqlite",
                        *(["windows"] if host == "windows" else []))


# ── the resolver ────────────────────────────────────────────────────────────────────

class Resolver:
    """`harness_legs.py`, asked through its verb CLI. `call` returns the Result untouched (the
    caller maps the resolver's exit codes to verdicts); `json` refuses anything but JSON."""

    def __init__(self, host, arch, catalogue=LEGS_JSON, script=HARNESS_LEGS):
        self.host, self.arch = host, arch
        self.catalogue, self.script = catalogue, script

    @property
    def host_args(self):
        return ["--host-os", self.host, "--host-arch", self.arch]

    def call(self, args, catalogue=True, timeout=None, input_text=None, env_overrides=None):
        argv = [sys.executable, self.script]
        if catalogue:
            argv += ["--catalogue", self.catalogue]
        argv += [str(a) for a in args]
        return capture(argv, timeout=timeout, input_text=input_text,
                       env_=child_env(env_overrides, python=True))

    def json(self, args, what, catalogue=True, ok=(0,)):
        r = self.call(args, catalogue=catalogue)
        if r.rc not in ok:
            die("%s: the resolver FAILED (rc=%d)\n%s" % (what, r.rc, last_lines(r.err or r.out, 12)))
        try:
            return json.loads(r.out)
        except ValueError:
            die("%s: the resolver exited %d but printed no JSON this driver can read:\n%s"
                % (what, r.rc, first_lines(r.out, 8)))


# ── the legs and their ledger ───────────────────────────────────────────────────────

class Leg:
    """One declared leg: the resolver's plan object (`d`, read-only) and what this run records."""

    def __init__(self, d):
        self.d = d
        self.label = d["label"]
        self.spec = d["spec"]
        self.format = d["format"]
        self.arch = d.get("targetArch", "")
        run = d.get("run") or {}
        self.run = run
        self.run_mode = run.get("mode") or ""
        self.launcher = list(run.get("launcher") or [])
        self.fidelity = run.get("fidelity")
        self.build = d.get("build") or {}
        # the ledger: seeded from the plan's RUN verdict, overwritten as the run learns more
        self.verdict = run.get("verdict") or ""
        self.verdict_detail = run.get("detail") or ""
        self.unit_verdict = ""
        self.selected = True
        self.notes = []            # hygiene notes carried to the leg's report
        # ── what the run resolves for this leg (ONE spelling shared by every module) ──
        self.cc = []               # the CONTROL compiler's argv (Step 6), [] when none
        self.cc_machine = ""       # its reported target machine/triple
        self.cc_why = ""           # the resolver's candidate ladder (information, never a verdict)
        self.tcl_lib = ""          # the FIXTURE's libtcl (only when both tcl AND z resolved)
        self.z_lib = ""            # libz
        self.tcl_lib_any = ""      # libtcl even when zlib did not resolve (coherence check)
        self.z_lib_any = ""        # libz even when libtcl did not resolve (the CLI needs only z)
        self.lib_detail = ""       # the provider's account of how the pair was resolved
        self.acq_dir = ""          # pinned-archive: where acquisition landed
        self.acq_libs = []         # pinned-archive: [(as, path)] to stage beside artefacts
        self.tcl_script_dir = ""   # the acquired Tcl SCRIPT library (TCL_LIBRARY), or ""
        self.zinc_dir = ""         # per-leg staged zlib header dir (stage-zinc.py)
        self.cfg_dir = ""          # per-leg staged sqlite_cfg.h dir
        self.inc_file = ""         # the fixture's include-list file
        self.cli_inc_file = ""     # the CLI's include-list file
        self.manifest = ""         # the fixture manifest this run generated
        self.fixture = ""          # the built testfixture (the path the COMPILER reported)
        self.fixture_built = False
        self.cli_bin = ""          # the built sqlite3 CLI
        self.cli_verdict = ""      # the CLI artefact's ledger verdict
        self.cli_detail = ""
        self.oracle = {}           # the same-platform reference oracle record (Step 7)
        self.build_attribution = ""  # Step 7's per-TU attribution summary (a failed build only)
        self.rundir_plan = None    # `--run-dir-plan` (cached; runnable legs only)
        self.smoke_verdict = ""    # Step 7c's CLI smoke verdict line
        self.unit_fail = False     # Step 8's verdict ladder reached FAIL
        self.unit_report = []      # Step 8's report lines for Step 9
        self.hygiene = []          # leftover-process / stale-lock notes for the report

    def __repr__(self):
        return "Leg(%s)" % self.label


class Ledger:
    """The closed-vocabulary verdict ledger of every DECLARED leg."""

    def __init__(self, vocabulary, log=LOG):
        self.vocabulary = [t for t in vocabulary if t]
        if not self.vocabulary:
            die("could not state the CLOSED verdict vocabulary (the resolver answered nothing), "
                "so no verdict this run records could be checked")
        self.log = log
        self.unclassified = []     # labels whose recorded token this run could not classify

    def known(self, token):
        return token in self.vocabulary

    def _defect(self, leg, why, detail):
        if leg.label not in self.unclassified:
            self.unclassified.append(leg.label)
        mode = leg.run_mode or "<unset>"
        launcher = (", declared launcher '%s'" % " ".join(leg.launcher)) if leg.launcher else ""
        self.log.warn("[%s] HARNESS DEFECT %s %s." % (leg.label, DASH, why))
        self.log.warn("      what it did say  : %s" % detail)
        self.log.warn("      resolved run plan: mode='%s'%s" % (mode, launcher))

    def set_leg(self, leg, token, detail):
        """Record a LEG verdict; an empty or unknown token is `poisoned` with HARNESS DEFECT."""
        detail = detail or "<no reason recorded>"
        if not token:
            why = "this driver recorded a verdict with an EMPTY token"
        elif not self.known(token):
            why = ("this driver recorded the verdict token '%s', which is OUTSIDE the closed "
                   "vocabulary (%s)" % (token, " ".join(self.vocabulary)))
        else:
            leg.verdict, leg.verdict_detail = token, detail
            return
        self._defect(leg, why, detail)
        leg.verdict = "poisoned"
        leg.verdict_detail = "HARNESS DEFECT: %s. %s" % (why, detail)

    def unit_not_run(self, leg, token, detail):
        """Record that the leg's WHOLE unit corpus did not run; guarded like `set_leg`, and an
        unclassifiable token poisons the LEG too (the .sh's rule, the stricter one)."""
        detail = detail or "<no reason recorded>"
        if not token:
            why = "this driver recorded a NOT-RUN with an EMPTY verdict token"
        elif not self.known(token):
            why = ("this driver recorded a NOT-RUN with the token '%s', which is OUTSIDE the "
                   "closed vocabulary (%s)" % (token, " ".join(self.vocabulary)))
        else:
            leg.unit_verdict = "not run [%s] %s %s" % (token, DASH, detail)
            return
        self._defect(leg, why, detail)
        self.log.warn("      This leg's ENTIRE unit corpus did not run and the run cannot say "
                      "under which class.")
        leg.verdict = "poisoned"
        leg.verdict_detail = "HARNESS DEFECT: %s. %s" % (why, detail)
        leg.unit_verdict = "not run [poisoned] %s HARNESS DEFECT: %s. %s" % (DASH, why, detail)

    def marks_missing(self, leg, what, why):
        """`skipped-build-input-missing`, KEEPING the displaced run verdict in the detail (the
        .sh; the .ps1 overwrote it)."""
        extra = ("  [and this host could not RUN it either: %s]" % leg.run.get("verdict")
                 if leg.run.get("verdict") else "")
        self.set_leg(leg, "skipped-build-input-missing", why + extra)
        self.log.warn("[%s] BUILD INPUT MISSING %s %s: %s" % (leg.label, DASH, what, why))

    def marks_harness_defect(self, leg, token, detail):
        self.set_leg(leg, token, detail)
        self.log.warn("[%s] HARNESS DEFECT [%s] %s %s" % (leg.label, token, DASH, detail))
        self.log.warn("      This leg is NOT built and NOT run here. The other legs are unaffected "
                      "and the run")
        self.log.warn("      CONTINUES %s but it CANNOT exit 0, because what failed is ours, not "
                      "this machine's." % DASH)


def run_is_skipped(leg):
    """The ONE run decision: `skip` → True; `native` → False; `launched` needs a launcher argv
    (an empty one is a transport defect, refused); any other mode is refused."""
    mode = leg.run_mode
    if mode == "skip":
        return True
    if mode == "native":
        return False
    if mode == "launched":
        if not leg.launcher:
            die("[%s] the resolved plan says run mode 'launched' but carries an EMPTY launcher argv.\n"
                "      A leg cannot be both runnable and unrunnable. That is a transport defect between\n"
                "      harness_legs.py and this driver, not a property of this machine." % leg.label)
        return False
    die("[%s] has an unknown run mode '%s' %s the resolver and this driver disagree about the "
        "vocabulary." % (leg.label, mode or "<empty>", DASH))


# ── the run's configuration and state ───────────────────────────────────────────────

def cpu_count():
    """`nproc`'s answer where the OS has one (the affinity mask), else the CPU count, else 4."""
    try:
        return max(1, len(os.sched_getaffinity(0)))
    except (AttributeError, OSError):
        return os.cpu_count() or 4


class Config:
    """Every environment knob of both old drivers, read and VALIDATED once, before Step 1 does
    anything (a malformed number died in shell arithmetic or an `[int]` cast hours in).
    ★ FOUR OF THEM ALSO TAKE A COMMAND-LINE VALUE (2026-09-25), what the sqlite action's steps pass:
    --tier, --dss-config and --test-file (the build-and-test step's inputs) and --dss (every step's
    `{product}`, the one file the leg's build is declared to make). `knobs` maps each attribute to the
    value its flag gave (a flag not given is absent); a flag and its environment variable naming
    DIFFERENT values is refused -- one value, named once -- and `by` records which channel set each,
    for the lines that report it."""

    KNOBS = (("tier", "--tier", "DSS_TIER"), ("dss_config", "--dss-config", "DSS_CONFIG"),
             ("test_file", "--test-file", "DSS_TEST_FILE"), ("dss_bin", "--dss", "DSS_BIN"))

    def __init__(self, knobs=None):
        self.src_dir = path_knob(env("SRC_DIR"), "SRC_DIR")    # "" = the tree this harness ships in
        self.dss_repo_url = env("DSS_REPO_URL",
                                "git@github.com:dailysoftwaresystems/dss-code-prime.git")
        self.sqlite_repo_url = env("SQLITE_REPO_URL", "https://github.com/sqlite/sqlite.git")
        # THE POSIX-SIDE path of the shared sqlite clone (the .ps1 named it SQLITE_WSL_DIR).
        a, b = env("SQLITE_DIR"), env("SQLITE_WSL_DIR")
        if a and b and a != b:
            die("SQLITE_DIR='%s' and SQLITE_WSL_DIR='%s' name two different clones; both name the "
                "POSIX-side sqlite clone of this run, so set one of them." % (a, b))
        self.sqlite_dir = a or b                           # "" = ~/src/sqlite on the POSIX side
        self.out_dir = path_knob(env("OUT_DIR"), "OUT_DIR")    # "" = derived from the tree (Run)
        # DSS_JOBS (the .ps1's name) wins over JOBS (the .sh's); both validated.
        jobs_name = "DSS_JOBS" if env("DSS_JOBS").strip() else "JOBS"
        self.jobs = int_env(jobs_name, cpu_count(), 1)
        self.dss_branch = env("DSS_BRANCH")
        self.dss_commit = env("DSS_COMMIT")
        # Only the literal `1` opts in (the .sh); anything else keeps the refusal.
        self.allow_fresh_clone = env("DSS_ALLOW_FRESH_CLONE", "0") == "1"
        self.tcl_version = env("DSS_TCL_VERSION")
        self.tier = env("DSS_TIER", "veryquick") or "veryquick"
        self.test_file = env("DSS_TEST_FILE")
        self.dss_config = env("DSS_CONFIG", "release") or "release"
        # The operator OVERRIDE of every leg's earned confound list; None = no override.
        ov = env("DSS_CONFOUNDS")
        self.confounds_override = ov.split() if ov.strip() else None
        ex = env("DSS_TIER_EXCLUDES")
        self.tier_excludes = ex.split() if ex.strip() else []
        self.max_resumes = int_env("DSS_MAX_RESUMES", 10, 0)
        self.segment_stall = int_env("DSS_SEGMENT_STALL", 1800, 0)
        # How often, at MOST, a running corpus reports its progress on this driver's output
        # (`sqlite_units.CorpusProgress`: a line only when the segment log GREW since the last one). It
        # must stay well under every silence bound a caller applies to that output -- the step's
        # (sqlite.yml build-and-test `stallSeconds`) and the segment's own (DSS_SEGMENT_STALL), which is
        # refused here: an interval as long as the stall would let the output fall silent for the
        # whole bound while the corpus moves.
        self.progress_interval = int_env("DSS_PROGRESS_INTERVAL", 60, 1)
        if self.segment_stall and self.progress_interval >= self.segment_stall:
            die("DSS_PROGRESS_INTERVAL=%d is not shorter than DSS_SEGMENT_STALL=%d: a corpus that moves would "
                "then report nothing for as long as the stall bound, and a bound on this driver's silence "
                "(the step's) would end a run that is working." % (self.progress_interval, self.segment_stall))
        self.segment_timeout = int_env("DSS_SEGMENT_TIMEOUT", 0, 0)
        self.kill_settle = int_env("DSS_KILL_SETTLE", 20, 0)
        self.strict = tristate("DSS_STRICT_ARM_VERDICTS")
        self.allow_nonrelease = tristate("DSS_ALLOW_NONRELEASE_COMPILER")
        self.legs_filter = split_list(env("DSS_LEGS"))
        self.fidelity_filter = split_list(env("DSS_RUN_FIDELITY"))
        self.skip_selftest = env("DSS_SKIP_SELFTEST") == "1"
        self.config_root = env("DSS_CONFIG_ROOT")
        # The compiler Step 5 USES, REQUIRED (`sqlite_compiler.named_compiler`): --dss (every harness step's
        # `{product}`) or DSS_BIN by hand. There is no knob to search for or build one instead -- the old
        # SKIP_DSS_BUILD only chose between those two, and both are gone (2026-09-26).
        self.dss_bin = env("DSS_BIN")
        self.tcl_dll = env("TCL_DLL")
        self.zlib_dll = env("ZLIB_DLL")
        self.host_libdir = env("DSS_HOST_LIBDIR")
        # LAST, over every value read above: a flag's value replaces its variable's, and a variable naming
        # a DIFFERENT value beside it is refused rather than silently lost.
        self.by = dict((attr, var) for attr, _flag, var in self.KNOBS)
        knobs = dict(knobs or {})
        for attr, flag, var in self.KNOBS:
            if attr not in knobs:
                continue
            given, named = knobs[attr], env(var).strip()
            if named and named != given:
                die("%s='%s' and %s='%s' name two different values: the command line is the channel a "
                    "harness step uses -- unset %s." % (flag, given, var, named, var))
            setattr(self, attr, given)
            self.by[attr] = flag

    def corpus_label(self):
        """What the unit corpus of this run IS, named ONCE for every line that reports it: the one file a
        single-file run (DSS_TEST_FILE) sources, or the file of the tier. A single-file run that named the
        tier in its header, its per-leg step and its verdict read as a tier run that passed (found by lane
        xa, 2026-09-24: `veryquick.test` in six lines while select1.test ran)."""
        if self.test_file:
            return "%s — ONE file, %s" % (os.path.basename(self.test_file), self.by["test_file"])
        return "%s.test" % self.tier


class Run:
    """The state the steps share. Each step reads what earlier steps recorded here and records
    what it learnt; nothing is carried in module globals."""

    def __init__(self, cfg, log=LOG):
        self.cfg = cfg
        self.log = log
        self.host = host_os()
        self.arch = host_arch()
        self.posix = PosixSide(self.host)
        self.resolver = Resolver(self.host, self.arch)
        self.repo_root = ""        # the DSS tree under test (Step 2)
        self.driver_tree = None    # the tree this harness ships in (None for a copy)
        self.out_dir = ""          # every artefact of this run lives under it
        self.registry_glob = ""    # the deferred-anchor registry, consulted on a failure
        self.plan = {}             # the resolver's plan (`--plan --format json`)
        self.legs = []             # every DECLARED leg, in catalogue order
        self.ledger = None         # the closed-vocabulary verdict ledger
        self.provenance = {}       # Step 2's account of the checkout under test
        self.run_lock = None
        self.clone_lock = None
        self.stage = None          # sqlite_stage.StageResult (Steps 3–4 + header discovery)
        self.stage_dir = ""        # where a Windows host's staged copy lives
        self.sqlite_dir_posix = "" # the shared sqlite clone, spelled on the POSIX side
        self.zinc_stage_dirs = {}  # headerStageKey -> the staged zlib header dir
        self.cfg_stage_dirs = {}   # configStageKey -> the staged sqlite_cfg.h dir
        self.gen_caps = {}         # what the manifest generator accepts (probed)
        self.stage_build = {}      # `--stage-build --format json` (configure flags, witnesses…)
        self.artifacts = None      # sqlite_base.VerdictLedger: per (leg, artifact) verdicts
        self.compiler = None       # sqlite_compiler.Compiler (Step 5)
        self.config_root = ""
        self.currency_note = ""
        self.loadext_builder = ""  # `dss` or `reference` (Step 6)
        self.counts = collections.Counter()   # compile / cli / smoke / unit / staging failures
        self.hygiene = []          # run-level hygiene notes (stale locks taken over, …)
        self.capability_gaps = []  # declared capabilities that did not reach the tests

    def selected(self):
        return [lg for lg in self.legs if lg.selected]

    def leg(self, label):
        for lg in self.legs:
            if lg.label == label:
                return lg
        die("no declared leg named '%s'" % label)

    def leg_out(self, leg):
        return os.path.join(self.out_dir, leg.label)
