#!/usr/bin/env python3
"""sqlite_procs.py -- the SQLite corpus harness's PROCESSES: enumeration, the leftover-fixture
sweep, the tree kill, start markers, the output-tree RUN LOCK and the shared-clone lock.

It replaces the process half of the two old drivers (lane mig, part 4, 2026-09-21): the bash
twin's `ps_enum_available` / `our_fixture_pids` / `stop_our_fixtures` / `proc_start_marker`, the
`dss:run-lock` region and the `dss:clone-lock` region; the PowerShell twin's
`Get-OurFixtureProcesses(Under)` / `Stop-FixtureProcesses` / `Stop-OurFixtures(Under)` and its
run-lock liveness (`Get-LockOwner`, `Test-LockOwnerAlive`). The union, with two defects closed:
  * the PowerShell sweep compared two WINDOWS spellings of a fixture that a WSL launcher runs as
    a LINUX process, so on WSL legs it matched nothing; a launched fixture is now swept INSIDE its
    own kernel (the plan's `kernelEntryArgv`, e.g. `wsl.exe -e`), matching the path spelled in
    that kernel's namespace;
  * the bash sweep excluded only itself; this process AND its ancestors are excluded now, and an
    enumeration that cannot run is UNVERIFIED (warned), never "none found".

HOW A PROCESS IS SEEN, per host: POSIX `ps -eo pid=,ppid=,args=` (a fixture is matched by its
COMMAND LINE, so one hosted by qemu/wine is found as that launcher's argument); Windows the
Toolhelp32 snapshot through stdlib ctypes plus `QueryFullProcessImageNameW` (a native fixture is
matched by its IMAGE path; separators and case normalised, since that filesystem folds case).
No psutil, no PowerShell/CIM.

Nothing runs at import. `python3 sqlite_procs.py --self-test` prints `passed=N failed=N
skipped=N` last and exits 0 only when nothing failed.
"""
from __future__ import annotations

import collections
import datetime
import glob
import ntpath
import os
import re
import shutil
import signal
import subprocess
import sys
import time

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass
# The action is `requireInputsUnmoved`: no `__pycache__` may appear beside its programs, and
# the sibling import below would write one before its own guard ran.
sys.dont_write_bytecode = True

import sqlite_common as C  # noqa: E402

# One enumeration: `procs` is a list of (pid, ppid, text) -- the command line on POSIX, the full
# image path on Windows (the bare exe name where the OS denies the path); `verified` False means
# the enumeration could NOT run, and `why` says why (or what it saw).
Enumeration = collections.namedtuple("Enumeration", ["procs", "verified", "why"])

PS_ARGV = ("ps", "-eo", "pid=,ppid=,args=")
ANCESTOR_LEVELS = 16          # this process + up to 15 ancestors (the PowerShell twin's walk)
TERM_GRACE_S = 15             # TERM, then this long, then KILL (both twins)
KILL_GRACE_S = 5
SETTLE_STEP_S = 1.0
SETTLE_TAIL_S = 2.0           # the measured floor after a kill (`test.db` still held otherwise)
POLL_S = 0.1
LAUNCHED_POLL_S = 0.5         # each poll through a launcher is a process start
ENUM_TIMEOUT_S = 120          # a cold WSL start is slow; a hung one must still end
SHORT_TIMEOUT_S = 60


class Sweep(list):
    """What one leftover sweep did: the pids it KILLED (a plain list for its callers), plus
    `verified`/`why` (could it look at all?) and `failed` [(pid, reason)] for a kill that did not
    take -- each one already WARNED."""

    def __init__(self, verified=True, why=""):
        super().__init__()
        self.verified = verified
        self.why = why
        self.failed = []


def _clean(text):
    """A child's message for a log line: NULs dropped (wsl.exe's own messages are UTF-16LE),
    whitespace collapsed."""
    return " ".join((text or "").replace("\0", "").split())


def _run(argv, timeout=SHORT_TIMEOUT_S, c_locale=True):
    """`C.capture` with an explicit environment (LC_ALL=C unless the caller must match another
    program's locale)."""
    return C.capture([str(a) for a in argv], timeout=timeout,
                     env_=C.child_env({"LC_ALL": "C"} if c_locale else None))


# ── Windows: the Toolhelp32 snapshot and the process handle, through stdlib ctypes ─────────

_WIN = []


def _win():
    """The Win32 entry points this module uses, declared ONCE (argtypes/restype; HANDLE is
    c_void_p) on first use -- never at import."""
    if _WIN:
        return _WIN[0]
    import ctypes
    import types
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                    ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
                    ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]

    class FILETIME(ctypes.Structure):
        _fields_ = [("dwLowDateTime", wintypes.DWORD), ("dwHighDateTime", wintypes.DWORD)]

    def proto(name, restype, *argtypes):
        fn = getattr(k32, name)
        fn.restype = restype
        fn.argtypes = list(argtypes)
        return fn

    h = ctypes.c_void_p
    ns = types.SimpleNamespace(
        ctypes=ctypes, wintypes=wintypes, PROCESSENTRY32W=PROCESSENTRY32W, FILETIME=FILETIME,
        INVALID_HANDLE=ctypes.c_void_p(-1).value, TH32CS_SNAPPROCESS=0x00000002,
        PROCESS_QUERY_LIMITED_INFORMATION=0x1000, SYNCHRONIZE=0x00100000, STILL_ACTIVE=259,
        WAIT_TIMEOUT=0x102, ERROR_NO_MORE_FILES=18, ERROR_ACCESS_DENIED=5,
        CreateToolhelp32Snapshot=proto("CreateToolhelp32Snapshot", h, wintypes.DWORD,
                                       wintypes.DWORD),
        Process32FirstW=proto("Process32FirstW", wintypes.BOOL, h,
                              ctypes.POINTER(PROCESSENTRY32W)),
        Process32NextW=proto("Process32NextW", wintypes.BOOL, h, ctypes.POINTER(PROCESSENTRY32W)),
        CloseHandle=proto("CloseHandle", wintypes.BOOL, h),
        OpenProcess=proto("OpenProcess", h, wintypes.DWORD, wintypes.BOOL, wintypes.DWORD),
        QueryFullProcessImageNameW=proto("QueryFullProcessImageNameW", wintypes.BOOL, h,
                                         wintypes.DWORD, wintypes.LPWSTR,
                                         ctypes.POINTER(wintypes.DWORD)),
        GetProcessTimes=proto("GetProcessTimes", wintypes.BOOL, h, ctypes.POINTER(FILETIME),
                              ctypes.POINTER(FILETIME), ctypes.POINTER(FILETIME),
                              ctypes.POINTER(FILETIME)),
        GetExitCodeProcess=proto("GetExitCodeProcess", wintypes.BOOL, h,
                                 ctypes.POINTER(wintypes.DWORD)),
        WaitForSingleObject=proto("WaitForSingleObject", wintypes.DWORD, h, wintypes.DWORD))
    _WIN.append(ns)
    return ns


def _win_open(pid, access):
    w = _win()
    handle = w.OpenProcess(access, False, int(pid))
    return handle, (0 if handle else w.ctypes.get_last_error())


def _win_image_path(pid, buf=None):
    """The FULL image path of `pid`, or "" when the OS denies it (a foreign or protected process)."""
    w = _win()
    handle, _err = _win_open(pid, w.PROCESS_QUERY_LIMITED_INFORMATION)
    if not handle:
        return ""
    try:
        if buf is None:
            buf = w.ctypes.create_unicode_buffer(32768)
        size = w.wintypes.DWORD(len(buf))
        if not w.QueryFullProcessImageNameW(handle, 0, buf, w.ctypes.byref(size)):
            return ""
        return buf.value
    finally:
        w.CloseHandle(handle)


def _win_creation(pid):
    """The process's creation FILETIME as a decimal string, or ""."""
    w = _win()
    handle, _err = _win_open(pid, w.PROCESS_QUERY_LIMITED_INFORMATION)
    if not handle:
        return ""
    try:
        times = [w.FILETIME() for _ in range(4)]
        if not w.GetProcessTimes(handle, *[w.ctypes.byref(t) for t in times]):
            return ""
        return str((times[0].dwHighDateTime << 32) | times[0].dwLowDateTime)
    finally:
        w.CloseHandle(handle)


def _win_alive(pid):
    w = _win()
    handle, err = _win_open(pid, w.PROCESS_QUERY_LIMITED_INFORMATION | w.SYNCHRONIZE)
    if not handle:
        handle, err = _win_open(pid, w.PROCESS_QUERY_LIMITED_INFORMATION)
        if not handle:
            # The process EXISTS when the OS refuses us access to it; any other refusal (an
            # invalid parameter) means there is no such process.
            return err == w.ERROR_ACCESS_DENIED
        try:
            code = w.wintypes.DWORD(0)
            return bool(w.GetExitCodeProcess(handle, w.ctypes.byref(code))) \
                and code.value == w.STILL_ACTIVE
        finally:
            w.CloseHandle(handle)
    try:
        return w.WaitForSingleObject(handle, 0) == w.WAIT_TIMEOUT
    finally:
        w.CloseHandle(handle)


def _enumerate_windows():
    w = _win()
    ctypes = w.ctypes
    snap = w.CreateToolhelp32Snapshot(w.TH32CS_SNAPPROCESS, 0)
    if not snap or snap == w.INVALID_HANDLE:
        return Enumeration([], False, "CreateToolhelp32Snapshot failed (WinError %d)"
                           % ctypes.get_last_error())
    rows = []
    try:
        entry = w.PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(w.PROCESSENTRY32W)
        more = w.Process32FirstW(snap, ctypes.byref(entry))
        if not more:
            return Enumeration([], False, "Process32FirstW failed (WinError %d)"
                               % ctypes.get_last_error())
        while more:
            rows.append((int(entry.th32ProcessID), int(entry.th32ParentProcessID),
                         entry.szExeFile))
            more = w.Process32NextW(snap, ctypes.byref(entry))
        err = ctypes.get_last_error()
        if err != w.ERROR_NO_MORE_FILES:
            return Enumeration([], False, "Process32NextW stopped early (WinError %d)" % err)
    finally:
        w.CloseHandle(snap)
    buf = ctypes.create_unicode_buffer(32768)
    procs, denied = [], 0
    for pid, ppid, exe in rows:
        image = _win_image_path(pid, buf) if pid else ""
        if not image:
            denied += 1
        procs.append((pid, ppid, image or exe))
    if not any(p[0] == os.getpid() for p in procs):
        return Enumeration([], False, "the Toolhelp32 snapshot does not contain this process "
                           "(pid %d), so it cannot vouch for any other" % os.getpid())
    return Enumeration(procs, True, "Toolhelp32 snapshot: %d process(es), %d without a readable "
                       "image path (a foreign or protected process)" % (len(procs), denied))


# ── POSIX (and any kernel entered through a launcher prefix): ps ───────────────────────

_PS_ROW = re.compile(r"^[ \t]*([0-9]+)[ \t]+([0-9]+)(?:[ \t]+(.*))?$")


def _parse_ps(text):
    rows = []
    for line in (text or "").split("\n"):
        m = _PS_ROW.match(line.rstrip("\r"))
        if m:
            rows.append((int(m.group(1)), int(m.group(2)), m.group(3) or ""))
    return rows


def _enumerate_ps(prefix, runner):
    argv = list(prefix) + list(PS_ARGV)
    r = runner(argv)
    shown = " ".join(argv)
    if r.rc != 0:
        return Enumeration([], False, "`%s` exited %d: %s" % (shown, r.rc,
                                                            _clean(r.err or r.out)[:300]))
    rows = _parse_ps(r.out)
    if not rows:
        return Enumeration([], False, "`%s` exited 0 but listed no process at all (not even "
                           "itself)" % shown)
    # ★ AN ENUMERATION MUST PROVE IT SEES: this process in its own kernel, or -- through a launcher,
    # whose kernel this process is not in -- the `ps` it just ran.
    if prefix:
        seen = any(t.startswith(" ".join(PS_ARGV)) or (" " + " ".join(PS_ARGV)) in t
                   for _p, _pp, t in rows)
        what = "the `%s` it ran" % " ".join(PS_ARGV)
    else:
        seen = any(p == os.getpid() for p, _pp, _t in rows)
        what = "this process (pid %d)" % os.getpid()
    if not seen:
        return Enumeration([], False, "`%s` listed %d process(es) but not %s, so it cannot vouch "
                           "for any other" % (shown, len(rows), what))
    return Enumeration(rows, True, "`%s`: %d process(es)" % (shown, len(rows)))


def _default_runner(argv):
    return _run(argv, timeout=ENUM_TIMEOUT_S)


def enumerate_processes(launcher_prefix=None, runner=None):
    """Every process this host shows -> Enumeration. With `launcher_prefix` (the plan's
    `kernelEntryArgv`, e.g. `["wsl.exe", "-e"]`) the listing is taken INSIDE that kernel with
    `ps`; without it, natively (ps on POSIX, the Toolhelp32 snapshot on Windows). `runner` is the
    ps spawner (injectable; default = `C.capture` under LC_ALL=C)."""
    prefix = [str(a) for a in (launcher_prefix or [])]
    if prefix or os.name != "nt":
        return _enumerate_ps(prefix, runner or _default_runner)
    try:
        return _enumerate_windows()
    except OSError as exc:  # pragma: no cover - a broken kernel32
        return Enumeration([], False, "the Windows process snapshot could not run: %s" % exc)


# ── this process and its ancestors ────────────────────────────────────────────────────

def _ancestor_chain(start, parent_of, limit=ANCESTOR_LEVELS, is_real_parent=None):
    """[start, parent, grandparent, …], at most `limit` long, stopping at pid 0, an unknown
    parent, a cycle, or a parent `is_real_parent(child, parent)` rejects."""
    chain, seen, cur = [], set(), start
    while cur and cur not in seen and len(chain) < limit:
        chain.append(cur)
        seen.add(cur)
        nxt = parent_of.get(cur)
        if not nxt or (is_real_parent is not None and not is_real_parent(cur, nxt)):
            break
        cur = nxt
    return chain


def _win_real_parent(child, parent):
    """A Windows ppid is recorded at creation and can outlive the parent, so a REUSED pid would
    graft a stranger onto the chain; a real parent was created before its child."""
    c, p = _win_creation(child), _win_creation(parent)
    if not c or not p:
        return True          # unknowable: keep excluding (the safe direction)
    return int(p) <= int(c)


def self_and_ancestors(enumeration=None):
    """{this pid, its parent, …} up to ANCESTOR_LEVELS entries. The sweep never matches these: the
    driver's own tree can carry a fixture path in its argv, and a sweep that found the harness
    would kill it (MEASURED on the bash twin as a self-match)."""
    me = os.getpid()
    en = enumeration if enumeration is not None else enumerate_processes()
    if not en.verified:
        out = {me}
        try:
            out.add(os.getppid())
        except (AttributeError, OSError):  # pragma: no cover
            pass
        return out
    parent_of = dict((pid, ppid) for pid, ppid, _t in en.procs)
    return set(_ancestor_chain(me, parent_of, ANCESTOR_LEVELS,
                               _win_real_parent if os.name == "nt" else None))


# ── matching ────────────────────────────────────────────────────────────────────────

def _win_path_key(path, keep_trailing):
    p = str(path)
    if p.startswith("\\\\?\\"):
        p = p[4:]
    trailing = keep_trailing and p.endswith(("\\", "/"))
    p = ntpath.normpath(p)
    if trailing and not p.endswith("\\"):
        p += "\\"
    return ntpath.normcase(p)


def _matcher(fixture_path, launched):
    """Native Windows: the IMAGE path contains the fixture's path (both absolute, separators and
    case normalised, a directory's trailing separator kept so `…\\pe64\\` never matches
    `…\\pe64-x\\`). Everywhere else: the COMMAND LINE contains the path, byte for byte."""
    if launched or os.name != "nt":
        return lambda text: fixture_path in text
    trailing = str(fixture_path).endswith(("\\", "/"))
    needle = _win_path_key(ntpath.abspath(fixture_path) + ("\\" if trailing else ""), True)
    return lambda text: bool(text) and needle in _win_path_key(text, False)


def our_fixture_pids(fixture_path, launcher_prefix=None, enumerator=None):
    """The processes still running OUR fixture -> Enumeration(procs, verified, why), `procs` the
    matching (pid, ppid, text) rows. Scoped to the EXACT path (or a directory with its trailing
    separator), never to an image NAME: a developer's own testfixture, qemu or wine is never
    touched, and under the run lock anything running THIS path is a leftover by construction.
    Natively this process and its ancestors are excluded. With `launcher_prefix` the listing is
    taken INSIDE the fixture's own kernel and `fixture_path` must be spelled in THAT namespace
    (`/mnt/c/…`); nothing is excluded there, because this process's pids name nothing in that
    kernel -- excluding them would only risk sparing a leftover whose pid happens to coincide.
    An empty path is refused as UNVERIFIED (an empty needle would match every process)."""
    if not fixture_path:
        return Enumeration([], False, "an EMPTY fixture path was given, so nothing was searched "
                           "(an empty needle would match every process)")
    prefix = [str(a) for a in (launcher_prefix or [])]
    en = (enumerator or enumerate_processes)(prefix)
    if not en.verified:
        return Enumeration([], False, en.why)
    skip = set() if prefix else self_and_ancestors(en)
    match = _matcher(fixture_path, bool(prefix))
    return Enumeration([row for row in en.procs if row[0] not in skip and match(row[2])],
                       True, en.why)


# ── liveness and start markers ──────────────────────────────────────────────────────

def pid_alive(pid):
    """Does `pid` name a process that has not exited? A process we may not signal EXISTS. ⚠ Never
    `os.kill(pid, 0)` on Windows: there it TERMINATES the process."""
    try:
        pid = int(pid)
    except (TypeError, ValueError):
        return False
    if pid <= 0:
        return False
    if os.name == "nt":
        return _win_alive(pid)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return not _is_zombie(pid)
    except OSError:
        return False
    # A ZOMBIE HAS EXITED: `kill -0` still answers for it until its parent reaps it. ✔MEASURED
    # 2026-09-22 (arm SM03b, WSL): this returned True for one, so a lock whose holder crashed
    # into an unreaped zombie read as HELD for as long as the zombie lived -- while Windows
    # (`_win_alive`) already answered "exited" for an exited process object.
    return not _is_zombie(pid)


def proc_start_marker(pid):
    """When `pid` started -- the PID-reuse guard: Linux `/proc/<pid>/stat` field 22 (clock ticks
    since boot), Windows the creation FILETIME, elsewhere `ps -o lstart=` (squeezed, C locale).
    "" when there is no such process or it cannot be read."""
    try:
        pid = int(pid)
    except (TypeError, ValueError):
        return ""
    if pid <= 0:
        return ""
    if os.name == "nt":
        return _win_creation(pid)
    if os.path.exists("/proc/self/stat"):
        try:
            with open("/proc/%d/stat" % pid, "rb") as fh:
                raw = fh.read().decode("ascii", "replace")
        except OSError:
            return ""
        rest = raw[raw.rfind(")") + 2:].split()
        return rest[19] if len(rest) > 19 else ""
    r = _run(["ps", "-o", "lstart=", "-p", str(pid)], timeout=SHORT_TIMEOUT_S)
    return " ".join(r.out.split()) if r.rc == 0 else ""


def _is_zombie(pid):
    """An exited child nobody has reaped yet answers `kill -0`; it is not running anything."""
    if os.path.exists("/proc/self/stat"):
        try:
            with open("/proc/%d/stat" % pid, "rb") as fh:
                raw = fh.read().decode("ascii", "replace")
        except OSError:
            return False
        rest = raw[raw.rfind(")") + 2:].split()
        return bool(rest) and rest[0] == "Z"
    r = _run(["ps", "-o", "stat=", "-p", str(pid)])
    return r.rc == 0 and r.out.strip().startswith("Z")


def _native_gone(pid):
    if os.name == "nt":
        return not _win_alive(pid)
    return not pid_alive(pid) or _is_zombie(pid)


def _native_cmdline(pid):
    """(state, text) for one native pid right now: state `present` / `absent` / `unknown`."""
    if os.name == "nt":
        if not _win_alive(pid):
            return "absent", ""
        return "present", _win_image_path(pid)
    if os.path.exists("/proc/self/stat"):
        try:
            with open("/proc/%d/cmdline" % pid, "rb") as fh:
                raw = fh.read()
        except FileNotFoundError:
            return "absent", ""
        except OSError:
            return "unknown", ""
        if not raw:                       # a zombie or a kernel thread: nothing to run
            return "absent", ""
        return "present", raw.rstrip(b"\0").replace(b"\0", b" ").decode("utf-8", "replace")
    r = _run(["ps", "-o", "args=", "-p", str(pid)])
    if r.rc == 0 and r.out.strip():
        return "present", r.out.strip()
    return ("absent", "") if r.rc == 1 else ("unknown", "")


def _launched_query(pid, prefix, field):
    r = _run(list(prefix) + ["ps", "-o", "%s=" % field, "-p", str(pid)], timeout=SHORT_TIMEOUT_S)
    if r.rc == 0 and r.out.strip():
        return "present", r.out.strip()
    if r.rc == 1 and not r.out.strip():
        return "absent", ""
    return "unknown", _clean(r.err or r.out)[:200]


def _wait(pred, seconds, step, sleep):
    deadline = time.monotonic() + seconds
    while True:
        if pred():
            return True
        if time.monotonic() >= deadline:
            return False
        sleep(step)


# ── the kill ────────────────────────────────────────────────────────────────────────

def _posix_signal(pid, sig):
    """Signal a leftover -- and its whole process group when it LEADS one that is not ours (a
    fixture started in its own session takes its children with it)."""
    try:
        pgid = os.getpgid(pid)
    except OSError:
        pgid = None
    try:
        if pgid == pid and pgid != os.getpgrp():
            os.killpg(pgid, sig)
        else:
            os.kill(pid, sig)
    except OSError:
        pass


def _stop_one(pid, needle, prefix, sleep):
    """(killed, how). Re-checked right before the signal, so a pid that exited and was REUSED by
    an unrelated process in between is never hit."""
    if prefix:
        state, text = _launched_query(pid, prefix, "args")
        if state == "absent" or (state == "present" and needle not in text):
            return True, " (it was already gone when the signal was due)"
        _run(list(prefix) + ["kill", "-TERM", str(pid)], timeout=SHORT_TIMEOUT_S)

        def gone():
            st, stat = _launched_query(pid, prefix, "stat")
            return st == "absent" or (st == "present" and stat.startswith("Z"))
        if _wait(gone, TERM_GRACE_S, LAUNCHED_POLL_S, sleep):
            return True, ""
        _run(list(prefix) + ["kill", "-KILL", str(pid)], timeout=SHORT_TIMEOUT_S)
        if _wait(gone, KILL_GRACE_S, LAUNCHED_POLL_S, sleep):
            return True, " (SIGKILL)"
        return False, "it outlived SIGTERM, %d s and SIGKILL (or `%s` could no longer see it)" \
            % (TERM_GRACE_S, " ".join(prefix))
    state, text = _native_cmdline(pid)
    if state == "absent":
        return True, " (it was already gone when the signal was due)"
    if state == "present" and not _matcher(needle, False)(text):
        return True, " (it was already gone; its pid now names another process, left alone)"
    if os.name == "nt":
        r = _run(["taskkill", "/T", "/F", "/PID", str(pid)], timeout=SHORT_TIMEOUT_S)
        if _wait(lambda: _native_gone(pid), TERM_GRACE_S, POLL_S, sleep):
            return True, " (taskkill /T /F)"
        return False, "taskkill /T /F exited %d (%s) and it is still running %d s later" \
            % (r.rc, _clean(r.out or r.err)[:200], TERM_GRACE_S)
    _posix_signal(pid, signal.SIGTERM)
    if _wait(lambda: _native_gone(pid), TERM_GRACE_S, POLL_S, sleep):
        return True, ""
    _posix_signal(pid, signal.SIGKILL)
    if _wait(lambda: _native_gone(pid), KILL_GRACE_S, POLL_S, sleep):
        return True, " (SIGKILL)"
    return False, "it outlived SIGTERM, %d s and SIGKILL" % TERM_GRACE_S


def stop_our_fixtures(fixture_path, why, launcher_prefix=None, settle_s=20, log=C.LOG,
                      enumerator=None, sleep=time.sleep):
    """Kill every leftover of OUR fixture (see `our_fixture_pids`) -> Sweep, the killed pids.
    Per match: TERM, up to TERM_GRACE_S, KILL (natively a POSIX process group when the leftover
    leads one, `taskkill /T /F` on Windows; through the launcher prefix `kill` in the fixture's
    kernel). Every kill is WARNED as `LEFTOVER FIXTURE: …` -- a killed process is a fact the verdict
    carries -- and so is every kill that did not take. Then, when anything matched and
    `settle_s` > 0, wait up to `settle_s` while a match remains, plus SETTLE_TAIL_S (MEASURED:
    without it the next segment died deleting a `test.db` the killed fixture still held).
    `settle_s=0` never waits. An enumeration that cannot run is WARNED as UNVERIFIED, never
    reported as clean."""
    prefix = [str(a) for a in (launcher_prefix or [])]
    where = ("inside the fixture's own kernel (via `%s`)" % " ".join(prefix)) if prefix \
        else "on this host"
    en = our_fixture_pids(fixture_path, prefix, enumerator)
    out = Sweep(en.verified, en.why)
    if not en.verified:
        log.warn("LEFTOVER-FIXTURE SWEEP UNVERIFIED [%s] %s for %s: %s — a stray fixture of a dead "
                 "run would be neither found nor killed; this is NOT a clean bill."
                 % (why, where, fixture_path or "<empty path>", en.why))
        return out
    for pid, _ppid, text in en.procs:
        ok, how = _stop_one(pid, fixture_path, prefix, sleep)
        if ok:
            out.append(pid)
            log.warn("LEFTOVER FIXTURE: %s — killed pid %d%s %s: %s"
                     % (why, pid, how, where, text[:300]))
        else:
            out.failed.append((pid, how))
            log.warn("LEFTOVER FIXTURE: %s — FAILED to kill pid %d %s (%s): %s"
                     % (why, pid, where, how, text[:300]))
    if settle_s > 0 and en.procs:
        waited = 0
        while waited < settle_s:
            left = our_fixture_pids(fixture_path, prefix, enumerator)
            if not left.verified or not left.procs:
                break
            sleep(SETTLE_STEP_S)
            waited += 1
        sleep(SETTLE_TAIL_S)
    return out


def kill_tree(proc):
    """Kill a `Popen` we started AND everything under it: its process group on POSIX (the child
    must have been started with `start_new_session=True`; our own group is never signalled),
    `taskkill /T /F` on Windows. Never raises: this runs on the failure path."""
    if os.name == "nt":
        _run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], timeout=SHORT_TIMEOUT_S)
        if proc.poll() is None:
            try:
                proc.kill()
            except OSError:
                pass
        return
    try:
        pgid = os.getpgid(proc.pid)
    except OSError:
        pgid = None
    try:
        if pgid is not None and pgid == proc.pid and pgid != os.getpgrp():
            os.killpg(pgid, signal.SIGKILL)
        elif proc.poll() is None:
            proc.kill()
    except OSError:
        pass


# ── the RUN LOCK (one harness run per output tree) ──────────────────────────────────

LockOwner = collections.namedtuple("LockOwner", ["pid", "marker", "text"])


def _rm_tree(path):
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    elif os.path.lexists(path):
        os.remove(path)


class RunLock:
    """One harness run per OUTPUT TREE. MEASURED failure it prevents: two runs shared a tree, the
    second re-staged the .test corpus under the first's live fixture and then could not replace a
    testfixture that was still executing.

    `mkdir` is the atomic test-and-set; the owner file `owner.txt` is ASCII
    `pid|start-marker|iso-time`. The lock is LIVENESS-based: its owner is alive only when the pid
    is alive AND its start marker is the recorded one (a reused pid is a stranger), so a crashed
    run never wedges the next -- its lock is STOLEN and the theft reported. Two attempts.

    ★ THE LOCK OWNS ITS DIRECTORY: `acquire` creates the directory the lock lives in (`_prepare`,
    as CloneLock's does) before the test-and-set, which stays the bare `mkdir` of the lock itself.
    ✔MEASURED 2026-09-25: until then every caller had to create it first -- the normal mode did,
    the recompile did not, and on a tree that had never staged sqlite its R3 died with `could not
    create the run lock ... path not found`. Arms RL00 (a directory that does not exist yet is
    created) and RL10 (one that cannot be created is refused by name)."""

    OWNER = "owner.txt"
    OWNER_SETTLE_S = 2.0      # a lock taken a moment ago may not have its owner file yet

    def __init__(self, lock_dir):
        self.lock_dir = os.path.abspath(lock_dir)
        self.owner_file = os.path.join(self.lock_dir, self.OWNER)
        self.held = False
        self._marker = ""

    def _prepare(self):
        parent = os.path.dirname(self.lock_dir)
        try:
            os.makedirs(parent, exist_ok=True)
        except OSError as exc:
            C.die("could not create the directory the run lock %s lives in: %s" % (self.lock_dir, exc))

    def _mkdir(self):
        os.mkdir(self.lock_dir)

    def read_owner(self):
        """LockOwner, or None when there is no owner file; pid None when it is unreadable."""
        try:
            with open(self.owner_file, "rb") as fh:
                text = fh.read().decode("ascii", "replace").strip()
        except OSError:
            return None
        parts = text.split("|")
        try:
            pid = int(parts[0]) if len(parts) >= 2 else None
        except ValueError:
            pid = None
        if pid is not None and pid <= 0:
            pid = None
        return LockOwner(pid, parts[1] if pid is not None else "", text)

    def _settled_owner(self):
        deadline = time.monotonic() + self.OWNER_SETTLE_S
        while True:
            owner = self.read_owner()
            if (owner is not None and owner.text) or time.monotonic() >= deadline:
                return owner
            time.sleep(POLL_S)

    @staticmethod
    def owner_alive(owner):
        return bool(owner and owner.pid and owner.marker and pid_alive(owner.pid)
                    and proc_start_marker(owner.pid) == owner.marker)

    def acquire(self, log=C.LOG):
        """Take the lock -> None, or the PID (text) of the STALE owner it took over (warned).
        A LIVE owner is refused (HarnessDie)."""
        me = os.getpid()
        mark = proc_start_marker(me)
        if not mark:
            C.die("cannot read this process's own start marker (pid %d), so a run lock taken now "
                  "could not prove it is alive and would be stolen from this very run." % me)
        self._prepare()
        stolen = None
        for _attempt in (1, 2):
            try:
                self._mkdir()
            except FileExistsError:
                owner = self._settled_owner()
                if self.owner_alive(owner):
                    C.die("another dss sqlite harness run is ALREADY ACTIVE on this output tree.\n"
                          "      output tree : %s\n"
                          "      owner PID   : %d  (still running, and its start marker matches)\n"
                          "      Two invocations here corrupt each other: one re-stages the .test "
                          "corpus the\n"
                          "      other run's fixture is sourcing from, and neither can replace a "
                          "testfixture\n"
                          "      binary that is still executing. Wait for it, or set OUT_DIR "
                          "elsewhere.\n"
                          "      If you are certain that PID is dead, remove: %s"
                          % (os.path.dirname(self.lock_dir), owner.pid, self.lock_dir))
                if owner is not None and owner.pid:
                    stolen = str(owner.pid)
                    how = ("that PID is gone" if not pid_alive(owner.pid) else
                           "that PID is alive but its start marker is not the recorded one, so "
                           "it is a different process that reused the PID")
                else:
                    stolen = "? (an unreadable lock)"
                    how = "its owner file is missing or unreadable"
                log.warn("took over a STALE run lock left by PID %s (%s — that run died without "
                         "releasing it) — reported in the verdict." % (stolen, how))
                try:
                    _rm_tree(self.lock_dir)
                except OSError as exc:
                    C.die("could not remove the STALE run lock %s: %s" % (self.lock_dir, exc))
                continue
            except OSError as exc:
                C.die("could not create the run lock %s: %s" % (self.lock_dir, exc))
            stamp = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
            try:
                with open(self.owner_file, "w", encoding="ascii", newline="\n") as fh:
                    fh.write("%d|%s|%s\n" % (me, mark, stamp))
            except (OSError, UnicodeError) as exc:
                C.die("could not write the run lock's owner file %s: %s" % (self.owner_file, exc))
            self.held, self._marker = True, mark
            return stolen
        C.die("could not take the run lock %s after 2 attempts: another invocation keeps taking it "
              "first. Nothing was overwritten; run again once it settles." % self.lock_dir)

    def release(self):
        """Remove the lock -- only if it is still OURS (a lock another run legitimately took is
        never removed). A release that cannot remove it leaves a stale lock the next run steals."""
        if not self.held:
            return
        self.held = False
        owner = self.read_owner()
        if owner is not None and owner.pid == os.getpid() and owner.marker == self._marker:
            try:
                _rm_tree(self.lock_dir)
            except OSError:
                pass


# ── the SHARED-CLONE lock (reader/writer), on-disk format of the bash twin kept ─────────

CLONE_LOCK_BLOCKED = "DSS-CLONE-LOCK-BLOCKED"
_KEY_BYTES = frozenset(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-")


def clone_lock_key(clone_dir, environ=None):
    """The lock directory of one sqlite clone: `${XDG_CACHE_HOME:-$HOME/.cache}/dsscp/clone-locks/`
    + the clone's REAL path with every byte outside `A-Za-z0-9._-` turned into `_` (the bash
    twin's `tr -c`). The real path is `cd <p> && pwd -P` for an existing directory; for one not
    created yet the bash twin used the literal string, which split the write and read locks of a
    first run across two directories when the path went through a symlink -- the real path of the
    existing prefix is used instead (⚠DECISION)."""
    env = os.environ if environ is None else environ
    base = env.get("XDG_CACHE_HOME") or ""
    if not base:
        home = env.get("HOME") or ""
        if not home:
            C.die("neither XDG_CACHE_HOME nor HOME is set, so the shared-clone lock has no home.")
        base = home + "/.cache"
    real = os.fsencode(os.path.realpath(clone_dir))
    mapped = "".join(chr(b) if b in _KEY_BYTES else "_" for b in real)
    return "%s/dsscp/clone-locks/%s" % (base, mapped)


def _lstart_marker(pid):
    """The clone lock's LINE-2 start marker, byte-compatible with the bash twin's
    `ps -p <pid> -o lstart= | tr -s ' '` under the caller's own locale (that line is shared with
    it): runs of spaces squeezed, trailing newlines dropped. "" when there is no such process.
    ⚠ NOT STABLE ON WSL2: Linux `ps` derives lstart from /proc/stat `btime`, which moves whenever
    CLOCK_REALTIME is stepped -- MEASURED 2026-09-21 on this project's WSL2 host, one live
    process's lstart moved 25 s between two samples 0.5 s apart while its /proc start ticks did
    not. See CloneLock.PROC_START."""
    r = C.capture(["ps", "-p", str(int(pid)), "-o", "lstart="], timeout=SHORT_TIMEOUT_S,
                  env_=C.child_env())
    return re.sub(" +", " ", r.out or "").rstrip("\n")


class CloneLock:
    """The reader/writer lock of the ONE sqlite clone every harness run shares.
      WRITE  short, mutating: fetch/pull, configure, the stage (a POSIX host's Steps 3-7; a
             Windows host's derive, inside WSL).
      READ   long, read-only: a POSIX host's corpus run reads the .test files straight out of the
             clone for hours; it takes WRITE first and DOWNGRADES to READ (marker first, then the
             writer released), so the long window blocks a mutator without blocking a reader.
    A live holder FAILS LOUD (`CloneLockBlocked`, exit 3, first line `DSS-CLONE-LOCK-BLOCKED`);
    staleness is liveness (pid + start marker), so a crashed run's lock is stolen and the theft
    noted. The state lives OUTSIDE the clone, keyed on its real path (`clone_lock_key`).
    ON-DISK FORMAT: `<key>/w.lock/owner` and `<key>/readers/<pid>.reader`, each
    `pid\\nlstart-marker\\nepoch\\nwhat\\n` -- the bash twin's four lines, unchanged, so a copy of
    that twin still reads this lock exactly as before -- plus a FIFTH line
    `proc-start=<proc_start_marker>` the bash twin never reads. ⚠DECISION (a defect of the twin,
    MEASURED): on WSL2 the line-2 lstart marker of a LIVE holder moves when the clock is stepped, so
    the twin judged live holders stale and removed their locks; a holder this module wrote is
    judged by its fifth line (/proc start ticks on Linux, immune to clock steps), and a four-line
    holder (written by the twin) by its pid ALONE (`holder_alive` says why: its line 2 is the very
    marker a clock step moves). POSIX only: it runs where the clone
    lives (in process on a POSIX host, inside WSL on a Windows one); a Windows construction is
    refused."""

    STEAL_ATTEMPTS = 3
    PROC_START = "proc-start="

    def __init__(self, clone_dir, environ=None):
        # Keyed on WHERE the POSIX half runs (`PosixSide`, the driver's one host switch), never on
        # an `os.name` test of its own: the lock is taken where the clone's other users take it.
        if C.PosixSide(C.host_os()).needs_wsl:
            C.die("CloneLock is POSIX-only: the shared sqlite clone and its lock live on the POSIX "
                  "side, which on this host is WSL -- the derive takes this lock there. A lock taken "
                  "from here would be one no other run of the clone could see.")
        self.clone = clone_dir
        self.lock_dir = clone_lock_key(clone_dir, environ)
        self.writer = self.lock_dir + "/w.lock"
        self.readers = self.lock_dir + "/readers"
        self.role = ""
        self.notes = []

    @property
    def notes_text(self):
        return "; ".join(self.notes)

    def _marker_path(self):
        return "%s/%d.reader" % (self.readers, os.getpid())

    def _mkdir_writer(self):
        os.mkdir(self.writer)

    @staticmethod
    def _owner_lines(path):
        try:
            with open(path, "rb") as fh:
                return fh.read().decode("utf-8", "replace").split("\n")
        except OSError:
            return []

    def _is_legacy(self, lines):
        """A FOUR-line owner with a pid: written by the retired bash driver, which never wrote the
        fifth `proc-start=` line."""
        return bool(lines) and lines[0].strip().isdigit() and not (
            len(lines) > 4 and lines[4].startswith(self.PROC_START))

    def holder_alive(self, owner_path):
        """Alive = the pid is alive AND, for a holder this module wrote, its fifth line's
        clock-step-proof start marker is the recorded one.
        A FOUR-line holder (the retired bash driver's) carries only line 2's lstart, which WSL2
        moves whenever CLOCK_REALTIME is stepped. ✔MEASURED 2026-09-23: the WSL proof run's Step 0
        judged a LIVE four-line holder stale by that line (arm CL09 red) — the twin's own rule
        would have let a writer in under a live holder. Without the fifth line a changed lstart
        cannot tell a clock step from a reused pid, so such a holder is judged by its pid ALONE: a
        live pid HOLDS. A false hold is loud and says how to clear it (`holder_desc`); a false
        steal is silent — a mutator rewriting .test files under a live corpus run."""
        lines = self._owner_lines(owner_path)
        if not lines or not os.path.isfile(owner_path):
            return False
        pid = lines[0].strip()
        if not pid.isdigit() or int(pid) <= 0 or not pid_alive(int(pid)):
            return False
        if len(lines) > 4 and lines[4].startswith(self.PROC_START):
            recorded = lines[4][len(self.PROC_START):]
            return bool(recorded) and proc_start_marker(int(pid)) == recorded
        return True

    def holder_desc(self, owner_path):
        """`pid <p> — <what> — holding for <h>h<mm>m` (a corrupt epoch reads as 0h00m). A four-line
        holder (`_is_legacy`) also says it is judged by its pid alone, and what to remove when that
        pid is no harness run (`holder_alive` says why)."""
        raw = self._owner_lines(owner_path)
        lines = raw + ["", "", "", ""]
        now = int(time.time())
        since = lines[2].strip()
        since = int(since) if since.isdigit() else now
        age = max(0, now - since)
        desc = "pid %s — %s — holding for %dh%02dm" % (lines[0].strip() or "?",
                                                     lines[3] or "unknown", age // 3600,
                                                     (age % 3600) // 60)
        if self._is_legacy(raw):
            entry = (os.path.dirname(owner_path) if os.path.basename(owner_path) == "owner"
                     else owner_path)
            desc += (" (a FOUR-line owner, the retired bash driver's, judged by its pid alone: WSL2 "
                     "clock steps move its lstart line. If pid %s is no sqlite harness run, remove %s)"
                     % (lines[0].strip(), entry))
        return desc

    def _own_marker(self):
        """(line-2 lstart marker, fifth-line proc-start marker) of THIS process; both required."""
        me = os.getpid()
        mark, start = _lstart_marker(me), proc_start_marker(me)
        if not mark or not start:
            C.die("cannot read this process's own start markers (pid %d: lstart %r, proc-start %r), "
                  "so a clone lock taken now could be stolen from this very run."
                  % (me, mark, start))
        return mark, start

    def _write_owner(self, what, path, mark):
        text = "%d\n%s\n%d\n%s\n%s%s\n" % (os.getpid(), mark[0], int(time.time()),
                                           " ".join(str(what).split("\n")), self.PROC_START,
                                           mark[1])
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)

    def _fail(self, headline, holder):
        raise C.CloneLockBlocked(
            "%s\n\n [X] ERROR: %s\n"
            "      clone  : %s\n"
            "      held by: %s\n"
            "      Every harness run shares this checkout: a POSIX host's run reads the .test "
            "corpus\n"
            "      DIRECTLY out of it for its whole corpus step, while a Windows host's derive "
            "(inside\n"
            "      WSL) fetches, pulls and checks it out during staging. Both at once rewrite "
            ".test files\n"
            "      under a live fixture — silently, and the corrupted run still reports a "
            "verdict.\n"
            "      Wait for the holder above, or point this run at a different checkout:\n"
            "        SQLITE_DIR=/path/to/another/sqlite"
            % (CLONE_LOCK_BLOCKED, headline, self.clone, holder))

    def _note(self, text, log):
        self.notes.append(text)
        if log is not None:
            log.warn("shared-clone lock: %s" % text)

    def _prepare(self):
        try:
            os.makedirs(self.readers, exist_ok=True)
        except OSError as exc:
            C.die("could not create the shared-clone lock directory %s: %s" % (self.readers, exc))

    def write(self, what, log=C.LOG):
        """Take the WRITE lock (a mutator). A live writer or a live reader blocks it."""
        mark = self._own_marker()
        self._prepare()
        owner = self.writer + "/owner"
        tries = 0
        while True:
            try:
                self._mkdir_writer()
                break
            except FileExistsError:
                pass
            except OSError as exc:
                C.die("could not create the clone write lock %s: %s" % (self.writer, exc))
            if self.holder_alive(owner):
                self._fail("another dss harness run is MUTATING this sqlite clone",
                           self.holder_desc(owner))
            self._note("stole a STALE clone WRITE lock (its holder is gone)", log)
            _rm_tree(self.writer)
            tries += 1
            if tries >= self.STEAL_ATTEMPTS:
                self._fail("could not take the clone write lock after %d attempts"
                           % self.STEAL_ATTEMPTS, "see " + self.writer)
        self._write_owner(what, owner, mark)
        # DRAIN READERS: a corpus run holds the clone read-only for hours.
        live = ""
        for marker in sorted(glob.glob(self.readers + "/*.reader")):
            if not os.path.exists(marker):
                continue
            if self.holder_alive(marker):
                live += self.holder_desc(marker) + "; "
            else:
                self._note("removed a STALE clone READ marker (%s)" % os.path.basename(marker), log)
                try:
                    os.remove(marker)
                except OSError:
                    pass
        if live:
            _rm_tree(self.writer)
            self._fail("this sqlite clone is being READ by a corpus run in progress", live)
        self.role = "write"

    def read(self, what, log=C.LOG):
        """Take the READ lock -- DOWNGRADING a held write lock (the reader marker first, then the
        writer released), or checking that no live writer holds the clone."""
        mark = self._own_marker()
        self._prepare()
        owner = self.writer + "/owner"
        if self.role != "write":
            if self.holder_alive(owner):
                self._fail("another dss harness run is MUTATING this sqlite clone",
                           self.holder_desc(owner))
            if os.path.isdir(self.writer):
                self._note("stole a STALE clone WRITE lock (its holder is gone)", log)
                _rm_tree(self.writer)
        mine = self._marker_path()
        self._write_owner(what, mine, mark)
        if self.role == "write":
            _rm_tree(self.writer)
        elif self.holder_alive(owner):          # a writer won the race: back out
            try:
                os.remove(mine)
            except OSError:
                pass
            self._fail("a mutating run took this sqlite clone first", self.holder_desc(owner))
        self.role = "read"

    def release(self):
        """Drop whatever this object holds; a writer lock is removed only while it is still OURS."""
        role, self.role = self.role, ""
        if role == "write":
            lines = self._owner_lines(self.writer + "/owner")
            if lines and lines[0].strip() == str(os.getpid()):
                _rm_tree(self.writer)
        elif role == "read":
            try:
                os.remove(self._marker_path())
            except OSError:
                pass


# ── self-test ────────────────────────────────────────────────────────────────────────

class _Checks:
    def __init__(self, out=None):
        self.out = out or sys.stdout
        self.passed = self.failed = self.skipped = 0
        self.labels = []        # the id (first word) of every arm that RAN, in order

    def section(self, title):
        print("-- %s" % title, file=self.out, flush=True)

    def check(self, label, ok, detail=""):
        self.labels.append((label.split() or [label])[0])
        if ok:
            self.passed += 1
            print("  ok   %s" % label, file=self.out, flush=True)
        else:
            self.failed += 1
            print("  FAIL %s%s" % (label, ("\n       " + detail) if detail else ""), file=self.out,
                  flush=True)

    def eq(self, label, want, got):
        self.check(label, want == got, "expected %r\n       got      %r" % (want, got))

    def skip(self, label, why):
        self.skipped += 1
        print("  skip %s — %s" % (label, why), file=self.out, flush=True)


class _CaptureLog:
    def __init__(self):
        self.lines = []

    def step(self, msg):
        self.lines.append(("step", str(msg)))

    def info(self, msg):
        self.lines.append(("info", str(msg)))

    def ok(self, msg):
        self.lines.append(("ok", str(msg)))

    def warn(self, msg):
        self.lines.append(("warn", str(msg)))

    def text(self, kind="warn"):
        return "\n".join(t for k, t in self.lines if k == kind)


_DECOY_CODE = ("import os,sys,time; f=open(sys.argv[1]+'.tmp','w'); f.write(str(os.getpid())); "
               "f.close(); os.replace(sys.argv[1]+'.tmp', sys.argv[1]); time.sleep(90)")


def _popen(argv, env=None, group=False, capture=False):
    kw = {}
    if group:
        if os.name == "nt":
            kw["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        else:
            kw["start_new_session"] = True
    return subprocess.Popen(argv, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                            stderr=subprocess.PIPE if capture else subprocess.DEVNULL,
                            env=C.child_env(env, python=True), **kw)


def _await_ready(path, proc, seconds=45.0):
    """The readiness handshake: the decoy writes its OWN pid (in its own kernel) atomically."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            with open(path, "r", encoding="ascii") as fh:
                return int(fh.read().strip())
        except (OSError, ValueError):
            pass
        if proc.poll() is not None:
            return None
        time.sleep(0.05)
    return None


def _reap(proc):
    if proc is None:
        return
    if proc.poll() is None:
        try:
            proc.kill()
        except OSError:
            pass
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:  # pragma: no cover
        pass


def _dead_pid():
    """A pid that WAS ours and is now gone (reaped)."""
    p = _popen([sys.executable, "-c", "pass"])
    p.wait(timeout=60)
    return p.pid


def _sleeper():
    return _popen([sys.executable, "-c", "import time; time.sleep(90)"])


def _selftest_enumeration(t):
    t.section("EN  enumeration: it must SEE, and an enumeration that cannot run is UNVERIFIED")
    en = enumerate_processes()
    t.check("EN01 the native enumeration is verified and contains this process",
            en.verified and any(p[0] == os.getpid() for p in en.procs), en.why)
    rows = _parse_ps("  101     1 /usr/bin/python3 -m x\n202 101\njunk line\n  7 3 a  b\r\n")
    t.eq("EN02 the ps parser: blanks, an empty command line, junk skipped, CR dropped",
         [(101, 1, "/usr/bin/python3 -m x"), (202, 101, ""), (7, 3, "a  b")], rows)
    fail = _enumerate_ps([], lambda argv: C.Result(127, "", "cannot start ps: no such file"))
    t.check("EN03 ps that cannot start -> UNVERIFIED with the reason (never 'none found')",
            not fail.verified and "cannot start ps" in fail.why and not fail.procs, fail.why)
    empty = _enumerate_ps([], lambda argv: C.Result(0, "", ""))
    t.check("EN04 ps that lists nothing -> UNVERIFIED", not empty.verified, empty.why)
    blind = _enumerate_ps([], lambda argv: C.Result(0, "  999999 1 other\n", ""))
    t.check("EN05 a listing that does not contain this process -> UNVERIFIED", not blind.verified,
            blind.why)
    seen = _enumerate_ps(["x"], lambda argv: C.Result(0, "  5 1 ps -eo pid=,ppid=,args=\n", ""))
    t.check("EN06 through a launcher, the listing must contain the ps it ran (and does here)",
            seen.verified, seen.why)
    unseen = _enumerate_ps(["x"], lambda argv: C.Result(0, "  5 1 other\n", ""))
    t.check("EN07 ...and one that does not is UNVERIFIED", not unseen.verified, unseen.why)


def _selftest_ancestors(t, here):
    t.section("AN  this process and its ancestors are never a match")
    table = dict((k, k + 1) for k in range(1, 40))
    t.eq("AN01 the chain stops at ANCESTOR_LEVELS", list(range(1, 1 + ANCESTOR_LEVELS)),
         _ancestor_chain(1, table))
    t.eq("AN02 ...at a cycle, and at an unknown parent", ([5, 6], [8, 9]),
         (_ancestor_chain(5, {5: 6, 6: 5}), _ancestor_chain(8, {8: 9})))
    t.eq("AN03 ...and where the real-parent guard rejects a (reused-pid) parent", [3, 4],
         _ancestor_chain(3, {3: 4, 4: 5, 5: 6}, is_real_parent=lambda c, p: p != 5))
    mine = self_and_ancestors()
    want = {os.getpid()} | ({os.getppid()} if os.name != "nt" else set())
    t.check("AN04 self_and_ancestors() holds this pid%s" % (" and its parent" if os.name != "nt"
                                                          else ""), want <= mine, repr(mine))
    # A CHILD asks from its side, with a needle BOTH it and its parent (this self-test) match:
    # the raw listing shows both (the control), the sweep spares both (the property).
    needle = sys.executable if os.name == "nt" else (sys.argv[0] or os.path.abspath(__file__))
    code = ("import json,os,sys; sys.path.insert(0, sys.argv[1]); import sqlite_procs as P; "
            "en = P.enumerate_processes(); n = sys.argv[2]; m = P._matcher(n, False); "
            "raw = sorted(p for p, pp, tx in en.procs if p in (os.getpid(), os.getppid()) and m(tx)); "
            "hit = [r[0] for r in P.our_fixture_pids(n).procs]; "
            "print(json.dumps({'me': os.getpid(), 'parent': os.getppid(), 'raw': raw, 'hits': hit}))")
    p = _popen([sys.executable, "-B", "-c", code, here, needle], capture=True)
    out, err = p.communicate(timeout=120)
    try:
        import json
        got = json.loads(out.decode("utf-8", "replace").strip().splitlines()[-1])
    except (ValueError, IndexError):
        got = None
    if got is None:
        t.check("AN05 the ancestor probe child answered", False, err.decode("utf-8", "replace")[-600:])
        return
    t.eq("AN05 control: the child AND its parent both match the needle in the raw listing",
         sorted([got["me"], got["parent"]]), got["raw"])
    t.check("AN06 ...and the sweep spares both (self and ancestor excluded)",
            got["me"] not in got["hits"] and got["parent"] not in got["hits"], repr(got))


def _make_native_decoy(work):
    """(popen, marker, ready) -- a sleeping decoy whose command line (POSIX) or IMAGE path
    (Windows: a COPY of python.exe placed at the marker path) carries a unique path."""
    import uuid
    tag = "dss-decoy-%s" % uuid.uuid4().hex[:12]
    ddir = os.path.join(work, tag)
    os.makedirs(ddir)
    ready = os.path.join(work, tag + ".ready")
    if os.name == "nt":
        marker = os.path.join(ddir, "testfixture.exe")
        shutil.copy2(sys.executable, marker)
        exe_dir = os.path.dirname(sys.executable)
        for name in os.listdir(exe_dir):
            if name.lower().endswith(".dll"):
                shutil.copy2(os.path.join(exe_dir, name), os.path.join(ddir, name))
        proc = _popen([marker, "-c", _DECOY_CODE, ready], env={"PYTHONHOME": sys.base_prefix})
    else:
        marker = os.path.join(ddir, "testfixture")
        proc = _popen([sys.executable, "-c", _DECOY_CODE, ready, marker])
    return proc, marker, ready


def _selftest_decoy(t, work):
    t.section("DC  the leftover sweep finds OUR decoy by its %s, and only it"
              % ("IMAGE path" if os.name == "nt" else "command line"))
    proc, marker, ready = _make_native_decoy(work)
    try:
        pid = _await_ready(ready, proc)
        t.check("DC01 the decoy is ALIVE (handshake done) before the sweep runs",
                pid is not None and proc.poll() is None, "decoy exited %r" % proc.poll())
        if pid is None:
            return
        found = our_fixture_pids(marker)
        t.check("DC02 the decoy is FOUND", found.verified and len(found.procs) >= 1, repr(found))
        t.eq("DC03 ...and the pid found is the decoy's", [proc.pid], [r[0] for r in found.procs])
        other = our_fixture_pids(os.path.join(work, "dss-decoy-unrelated", os.path.basename(marker)))
        t.check("DC04 an unrelated path matches NOTHING (DC02 is its positive)",
                other.verified and not other.procs, repr(other))
        if os.name == "nt":
            upper = our_fixture_pids(marker.upper().replace("\\", "/"))
            t.eq("DC05 Windows: the image match ignores case and separator spelling", [proc.pid],
                 [r[0] for r in upper.procs])
        else:
            t.skip("DC05 Windows: the image match ignores case and separator spelling",
                   "a POSIX host matches the command line byte for byte")
        cap = _CaptureLog()
        sleeps = []

        def sleep(d):
            sleeps.append(d)
            time.sleep(d)
        killed = stop_our_fixtures(marker, "self-test", settle_s=0, log=cap, sleep=sleep)
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            pass
        t.eq("DC06 stop_our_fixtures kills it and returns its pid", [proc.pid], list(killed))
        t.check("DC07 ...the kill is REPORTED as LEFTOVER FIXTURE", "LEFTOVER FIXTURE: self-test"
                in cap.text() and str(proc.pid) in cap.text(), cap.text())
        t.check("DC08 ...the decoy is dead", proc.poll() is not None)
        t.check("DC09 ...and settle_s=0 did not settle (no settle step, no tail)",
                SETTLE_STEP_S not in sleeps and SETTLE_TAIL_S not in sleeps, repr(sleeps))
        again = our_fixture_pids(marker)
        t.check("DC10 a second sweep finds nothing", again.verified and not again.procs, repr(again))
        cap2 = _CaptureLog()
        slept = len(sleeps)
        none = stop_our_fixtures(marker, "self-test", settle_s=5, log=cap2, sleep=sleep)
        t.check("DC11 a sweep with no match waits for nothing and reports nothing",
                list(none) == [] and none.verified and cap2.text() == "" and len(sleeps) == slept,
                repr((list(none), cap2.text(), sleeps[slept:])))
    finally:
        _reap(proc)
    proc2, marker2, ready2 = _make_native_decoy(work)
    try:
        if _await_ready(ready2, proc2) is not None:
            sleeps = []

            def sleep2(d):
                sleeps.append(d)
                time.sleep(d)
            stop_our_fixtures(marker2, "self-test", settle_s=3, log=_CaptureLog(), sleep=sleep2)
            t.check("DC12 control: with settle_s > 0 the settle tail IS taken (DC09's instrument "
                    "can see one)", SETTLE_TAIL_S in sleeps, repr(sleeps))
        else:
            t.check("DC12 control: the second decoy started", False)
    finally:
        _reap(proc2)


def _selftest_unverified(t):
    t.section("UV  an enumeration that cannot run is UNVERIFIED, never 'none found'")

    def broken(prefix):
        return Enumeration([], False, "injected: this enumerator cannot run")
    en = our_fixture_pids("/x/testfixture", enumerator=broken)
    t.check("UV01 our_fixture_pids passes UNVERIFIED through with its reason",
            not en.verified and "injected" in en.why, repr(en))
    cap = _CaptureLog()
    sw = stop_our_fixtures("/x/testfixture", "self-test", log=cap, enumerator=broken)
    t.check("UV02 stop_our_fixtures kills nothing and WARNS 'UNVERIFIED' with the reason",
            list(sw) == [] and not sw.verified and "UNVERIFIED" in cap.text()
            and "injected" in cap.text(), cap.text())
    real = our_fixture_pids("/x/testfixture-that-is-not-running")
    t.check("UV03 control: the real enumerator IS verified", real.verified, real.why)
    t.check("UV04 an EMPTY fixture path is UNVERIFIED (never 'matches everything')",
            not our_fixture_pids("").verified)


def _wsl_path(win_path):
    r = _run(["wsl.exe", "-e", "wslpath", "-a", "-u", win_path], timeout=ENUM_TIMEOUT_S,
             c_locale=False)
    out = r.out.replace("\0", "").strip()
    return out.splitlines()[-1].strip() if r.rc == 0 and out else ""


def _selftest_launched(t, work):
    t.section("LA  a LAUNCHED fixture is swept inside its own kernel, matched by ITS spelling")
    import uuid
    tag = "dss-decoy-%s" % uuid.uuid4().hex[:12]
    if os.name == "nt":
        if not shutil.which("wsl.exe"):
            for k in range(1, 8):
                t.skip("LA%02d" % k, "wsl.exe is not on PATH, so there is no WSL kernel to enter")
            return
        prefix = ["wsl.exe", "-e"]
        ready_win = os.path.join(work, tag + ".ready")
        ready = _wsl_path(ready_win)
        marker = "/tmp/%s/testfixture" % tag
        if not ready:
            t.check("LA00 wslpath translated the handshake path", False)
            return
        proc = _popen(prefix + ["python3", "-c", _DECOY_CODE, ready, marker])
        ready_local = ready_win
    else:
        prefix = ["env"]
        marker = os.path.join(work, tag, "testfixture")
        ready_local = os.path.join(work, tag + ".ready")
        proc = _popen([sys.executable, "-c", _DECOY_CODE, ready_local, marker])
    try:
        kpid = _await_ready(ready_local, proc, seconds=90.0)
        t.check("LA01 the decoy is ALIVE in the fixture's kernel (handshake: its own pid there)",
                kpid is not None, "the decoy exited %r" % proc.poll())
        if kpid is None:
            return
        found = our_fixture_pids(marker, launcher_prefix=prefix)
        t.check("LA02 the sweep through `%s` FINDS it by the spelling in that kernel"
                % " ".join(prefix), found.verified and len(found.procs) >= 1, repr(found))
        t.eq("LA03 ...and the pid found is the decoy's pid IN that kernel", [kpid],
             [r[0] for r in found.procs])
        if os.name == "nt":
            native = our_fixture_pids(marker)
            t.check("LA04 the NATIVE sweep cannot see it (the old PowerShell sweep's blindness, "
                    "reproduced)", native.verified and not native.procs, repr(native))
        else:
            t.skip("LA04 the NATIVE sweep cannot see it", "`env` enters this host's own kernel")
        other = our_fixture_pids("/tmp/dss-decoy-unrelated/testfixture", launcher_prefix=prefix)
        t.check("LA05 an unrelated path matches nothing there (LA02 is its positive)",
                other.verified and not other.procs, repr(other))
        cap = _CaptureLog()
        killed = stop_our_fixtures(marker, "self-test", launcher_prefix=prefix, settle_s=0, log=cap)
        t.eq("LA06 stop_our_fixtures kills it THROUGH the prefix", [kpid], list(killed))
        try:
            proc.wait(timeout=60)
            exited = True
        except subprocess.TimeoutExpired:
            exited = False
        t.check("LA07 ...reported as LEFTOVER FIXTURE, and the decoy (%s) is gone"
                % ("its wsl.exe carrier" if os.name == "nt" else "the process"),
                exited and "LEFTOVER FIXTURE" in cap.text(), cap.text())
    finally:
        _reap(proc)


def _selftest_kill_tree(t):
    t.section("KT  kill_tree takes the whole tree")
    code = ("import subprocess,sys,time; c = subprocess.Popen([sys.executable, '-c', "
            "'import time; time.sleep(90)']); print(c.pid, flush=True); time.sleep(90)")
    p = _popen([sys.executable, "-c", code], group=True, capture=True)
    try:
        line = p.stdout.readline().decode("ascii", "replace").strip()
        grand = int(line) if line.isdigit() else None
        t.check("KT01 the child started a grandchild", grand is not None and pid_alive(grand),
                repr(line))
        if grand is None:
            return
        kill_tree(p)
        p.wait(timeout=30)
        gone = _wait(lambda: _native_gone(grand), 15, POLL_S, time.sleep)
        t.check("KT02 kill_tree killed the child AND the grandchild", gone and p.poll() is not None)
    finally:
        _reap(p)
    done = _popen([sys.executable, "-c", "pass"])
    done.wait(timeout=60)
    try:
        kill_tree(done)
        ok = True
    except Exception:  # noqa: BLE001
        ok = False
    t.check("KT03 kill_tree on an exited process never raises", ok)


def _selftest_markers(t):
    t.section("SM  start markers and liveness")
    m1, m2 = proc_start_marker(os.getpid()), proc_start_marker(os.getpid())
    t.check("SM01 this process has a start marker, and it is stable", bool(m1) and m1 == m2,
            repr((m1, m2)))
    # ★ A GONE PID IS NEVER TAKEN FOR THE PROCESS IT WAS -- the property every lock here relies
    # on (`owner_alive`, `holder_alive`: alive AND its own marker). NOT "a gone pid answers no
    # marker": ✔MEASURED 2026-09-22 under load on Windows, a reaped child's pid answered a
    # creation FILETIME at once -- Windows hands a pid to the next process freely, and another
    # process holding the dead one's object open keeps it answering -- so that arm was a coin
    # flip, and it took the driver's Step 0 down with it.
    s = _sleeper()
    pid, own = s.pid, proc_start_marker(s.pid)
    _reap(s)
    del s          # our own Popen holds the process object open on Windows until it is freed
    now = (pid_alive(pid), proc_start_marker(pid))
    t.check("SM02 a gone pid is never taken for the process it was (never alive WITH its own "
            "marker: gone, or reused with another marker)", bool(own) and not (now[0] and now[1] == own),
            repr((own, now)))
    t.eq("SM02b a nonsense pid has no marker", ("", ""),
         (proc_start_marker(0), proc_start_marker("x")))
    t.eq("SM03 pid_alive: self yes; 0 and -1 no", [True, False, False],
         [pid_alive(os.getpid()), pid_alive(0), pid_alive(-1)])
    # ...and an EXITED process is not alive -- asked while its pid is PINNED, so no other process
    # can own it yet (Windows: the handle our Popen keeps; POSIX: an unreaped zombie), which is
    # the deterministic form of the reaped-child case SM03 used to race on.
    z = _popen([sys.executable, "-c", "pass"])
    try:
        if os.name == "nt":
            z.wait(timeout=60)
        else:
            deadline = time.time() + 60
            while time.time() < deadline and not _is_zombie(z.pid):
                time.sleep(0.05)
        pinned_exited = (z.pid, pid_alive(z.pid))
    finally:
        _reap(z)
    t.check("SM03b an EXITED process whose pid is still pinned (our handle / an unreaped zombie) is "
            "NOT alive", pinned_exited[1] is False, repr(pinned_exited))
    s = _sleeper()
    try:
        t.check("SM04 a live child is alive and has its own marker",
                pid_alive(s.pid) and bool(proc_start_marker(s.pid)))
    finally:
        _reap(s)


def _selftest_run_lock(t, work):
    t.section("RL  the run lock: a live owner refused, a stale one stolen and reported")
    # RL00 -- the NEGATIVE: a lock whose directory does not exist yet (a tree that never staged has
    # no stage root), two levels of it, proven absent before the acquire.
    fresh = os.path.join(work, "never-staged", "stage-root", ".harness-lock")
    absent = not os.path.lexists(os.path.join(work, "never-staged"))
    lk = RunLock(fresh)
    got = _acquire(lk, _CaptureLog())
    t.check("RL00 a lock whose DIRECTORY does not exist yet is taken: the lock creates it", absent
            and got is None and lk.held and os.path.isfile(lk.owner_file), repr((absent, got)))
    lk.release()
    # RL01-RL09 take the lock in a directory that EXISTS (the branch RL00 does not reach).
    lock_dir = os.path.join(work, "out", ".harness-lock")
    os.makedirs(os.path.dirname(lock_dir))
    lk = RunLock(lock_dir)
    cap = _CaptureLog()
    got = _acquire(lk, cap)
    try:
        with open(lk.owner_file, "rb") as fh:
            raw = fh.read()
    except OSError:
        raw = b""
    parts = raw.decode("ascii", "replace").strip().split("|")
    t.check("RL01 a fresh acquire steals nothing and writes ASCII `pid|marker|iso`",
            got is None and raw.isascii() and len(parts) == 3 and parts[0] == str(os.getpid())
            and parts[1] == proc_start_marker(os.getpid()) and _iso_ok(parts[2]), repr(raw))
    lk.release()
    t.check("RL02 release removes it", not os.path.exists(lock_dir))
    s = _sleeper()
    try:
        _put_owner(lock_dir, "%d|%s|2026-09-21T00:00:00+00:00\n" % (s.pid, proc_start_marker(s.pid)))
        before = open(os.path.join(lock_dir, RunLock.OWNER), "rb").read()
        try:
            RunLock(lock_dir).acquire(_CaptureLog())
            refused, msg = False, ""
        except C.HarnessDie as exc:
            refused, msg = True, str(exc)
        t.check("RL03 a LIVE owner is refused, named, with the .sh's sentence",
                refused and "ALREADY ACTIVE on this output tree" in msg and str(s.pid) in msg, msg)
        t.eq("RL04 ...and its lock is left untouched", before,
             open(os.path.join(lock_dir, RunLock.OWNER), "rb").read())
        _put_owner(lock_dir, "%d|%s|x\n" % (s.pid, "0" if proc_start_marker(s.pid) != "0" else "1"))
        cap = _CaptureLog()
        lk = RunLock(lock_dir)
        got = _acquire(lk, cap)
        t.check("RL05 a live pid with a MISMATCHED start marker (pid reuse) is stale: stolen",
                got == str(s.pid) and "different process" in cap.text(), repr((got, cap.text())))
        lk.release()
    finally:
        _reap(s)
    dead = _dead_pid()
    _put_owner(lock_dir, "%d|123|x\n" % dead)
    cap = _CaptureLog()
    lk = RunLock(lock_dir)
    got = _acquire(lk, cap)
    owner = lk.read_owner()
    t.check("RL06 a DEAD owner is stolen and REPORTED; the lock is ours now",
            got == str(dead) and "took over a STALE run lock left by PID %d" % dead in cap.text()
            and owner is not None and owner.pid == os.getpid(), repr((got, cap.text())))
    lk.release()
    _put_owner(lock_dir, "garbage without a separator\n")
    cap = _CaptureLog()
    lk = RunLock(lock_dir)
    got = _acquire(lk, cap)
    t.check("RL07 an UNREADABLE owner file is stolen, and said so", got is not None and
            "unreadable" in got and "unreadable" in cap.text(), repr((got, cap.text())))
    _put_owner(lock_dir, "%d|%s|x\n" % (dead, "someone-else"))
    lk.release()
    t.check("RL08 release never removes a lock that is no longer OURS", os.path.exists(lock_dir))
    _rm_tree(lock_dir)

    class Racing(RunLock):
        OWNER_SETTLE_S = 0.0

        def _mkdir(self):
            raise FileExistsError(self.lock_dir)
    try:
        Racing(lock_dir).acquire(_CaptureLog())
        msg = ""
    except C.HarnessDie as exc:
        msg = str(exc)
    t.check("RL09 two attempts exhausted is refused by name (never an overwrite)",
            "after 2 attempts" in msg, msg)
    blocked = os.path.join(work, "a-file-not-a-directory")
    with open(blocked, "wb") as fh:
        fh.write(b"x")
    stuck = os.path.join(blocked, ".harness-lock")
    got = _acquire(RunLock(stuck), _CaptureLog())
    t.check("RL10 a directory the lock cannot create is refused, naming the lock",
            str(got).startswith("REFUSED: could not create the directory the run lock %s lives in"
                                % os.path.abspath(stuck)), repr(got))


def _acquire(lock, log):
    """`lock.acquire(log)`, a refusal turned into a value so the arm asserting on it fails as
    itself instead of taking its whole group down."""
    try:
        return lock.acquire(log)
    except C.HarnessDie as exc:
        return "REFUSED: %s" % str(exc).split("\n")[0]


def _iso_ok(text):
    try:
        datetime.datetime.fromisoformat(text)
        return True
    except ValueError:
        return False


def _put_owner(lock_dir, text):
    os.makedirs(lock_dir, exist_ok=True)
    with open(os.path.join(lock_dir, RunLock.OWNER), "w", encoding="ascii", newline="\n") as fh:
        fh.write(text)


def _clone_owner(path, pid, mark, what, epoch=None, proc_start=None):
    """An owner file as a holder writes it: the bash twin's four lines, plus this module's fifth
    when `proc_start` is given."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("%s\n%s\n%s\n%s\n" % (pid, mark, int(time.time()) if epoch is None else epoch, what))
        if proc_start is not None:
            fh.write("%s%s\n" % (CloneLock.PROC_START, proc_start))


def _blocked(lock, act):
    try:
        getattr(lock, act)("x", _CaptureLog())
        return ""
    except C.CloneLockBlocked as exc:
        return str(exc)


# The shared-clone lock's POSIX-only arms, BY NAME: a Windows host skips exactly these (the lock
# lives where the clone lives), and a POSIX run proves it ran exactly these (arm CL20), so the
# by-name skips cannot drift from the arms.
_CL_POSIX_ARMS = tuple(["CL%02d" % k for k in range(1, 11)] + ["CL10b"]
                       + ["CL%02d" % k for k in range(11, 20)])


def _selftest_clone_lock(t, work):
    t.section("CL  the shared-clone lock (reader/writer, the bash twin's on-disk format)")
    if C.PosixSide(C.host_os()).needs_wsl:        # the constructor's own predicate
        try:
            CloneLock(work)
            msg = ""
        except C.HarnessDie as exc:
            msg = str(exc)
        t.check("CL00 constructing it on Windows is refused by name", "POSIX-only" in msg, msg)
        for label in _CL_POSIX_ARMS + ("CL20",):
            t.skip(label, "POSIX-only (it runs where the clone lives: in process on a POSIX "
                   "host, inside WSL on a Windows one) -- run this self-test inside WSL")
        return
    t.skip("CL00 constructing it on Windows is refused by name", "this is a POSIX host")
    cache = os.path.join(work, "cache")
    clone = os.path.join(work, "clone dir é")
    os.makedirs(clone)
    link = os.path.join(work, "clone-link")
    os.symlink(clone, link)
    env = {"XDG_CACHE_HOME": cache, "HOME": "/nonexistent-home"}
    real = os.path.realpath(clone)
    mapped = "".join(ch if (ch.isascii() and (ch.isalnum() or ch in "._-")) else
                     "_" * len(ch.encode("utf-8")) for ch in real)
    t.eq("CL01 key: XDG_CACHE_HOME, the REAL path, every other byte -> _ (a 2-byte char -> __)",
         cache + "/dsscp/clone-locks/" + mapped, clone_lock_key(clone, env))
    t.eq("CL02 key: through a symlink it is the same lock; an empty XDG falls back to "
         "$HOME/.cache",
         (clone_lock_key(clone, env), "/h/.cache/dsscp/clone-locks/" + mapped),
         (clone_lock_key(link, env), clone_lock_key(clone, {"XDG_CACHE_HOME": "", "HOME": "/h"})))
    lk = CloneLock(clone, env)
    cap = _CaptureLog()
    lk.write("self-test writer", cap)
    lines = open(lk.writer + "/owner", "rb").read().decode("utf-8").split("\n")
    t.check("CL03 write: w.lock/owner is the twin's `pid\\nlstart\\nepoch\\nwhat\\n` + "
            "`proc-start=<ticks>\\n`",
            len(lines) == 6 and lines[0] == str(os.getpid()) and lines[1] == _lstart_marker(
                os.getpid()) and lines[2].isdigit() and lines[3] == "self-test writer"
            and lines[4] == CloneLock.PROC_START + proc_start_marker(os.getpid())
            and lines[5] == "" and lk.role == "write", repr(lines))
    lk.read("self-test reader", cap)
    t.check("CL04 read after write DOWNGRADES: the reader marker exists, the writer is gone",
            os.path.isfile(lk._marker_path()) and not os.path.exists(lk.writer)
            and lk.role == "read", repr(os.listdir(lk.lock_dir)))
    lk.release()
    t.check("CL05 release (read) removes our marker", not os.path.exists(lk._marker_path()))
    s = _sleeper()
    try:
        smark, sstart = _lstart_marker(s.pid), proc_start_marker(s.pid)
        wowner = lk.writer + "/owner"
        _clone_owner(wowner, s.pid, smark, "a live writer", proc_start=sstart)
        for label, act in (("CL06 a LIVE writer blocks write()", "write"),
                           ("CL07 a LIVE writer blocks read()", "read")):
            msg = _blocked(CloneLock(clone, env), act)
            t.check(label + ": first line DSS-CLONE-LOCK-BLOCKED, MUTATING, the holder named",
                    msg.split("\n")[0] == CLONE_LOCK_BLOCKED and "MUTATING" in msg
                    and "pid %d — a live writer — holding for 0h00m" % s.pid in msg, msg)
        _clone_owner(wowner, s.pid, "Thu Jan 1 00:00:00 1970", "a clock-stepped writer",
                     proc_start=sstart)
        t.check("CL08 a clock step cannot make a live holder stale: a WRONG lstart line with the "
                "right proc-start line still blocks (the defect this line closes)",
                "MUTATING" in _blocked(CloneLock(clone, env), "write"))
        _clone_owner(wowner, s.pid, smark, "an old-format writer")
        t.check("CL09 a FOUR-line holder (the bash twin's) whose pid is LIVE blocks",
                "MUTATING" in _blocked(CloneLock(clone, env), "write"))
        _clone_owner(wowner, s.pid, "Thu Jan 1 00:00:00 1970", "an old-format clock-stepped writer")
        msg = _blocked(CloneLock(clone, env), "write")
        t.check("CL10 ...and STILL blocks when its lstart line is not the pid's: without the fifth "
                "line a clock step and a reused pid look alike (the twin's lstart rule let a writer "
                "in under a LIVE holder); the refusal says it is judged by its pid alone and what "
                "to remove", "MUTATING" in msg and "judged by its pid alone" in msg
                and ("remove %s)" % lk.writer) in msg, msg)
        dead_writer = _dead_pid()
        _clone_owner(wowner, dead_writer, smark, "an old-format writer that died")
        lk2 = CloneLock(clone, env)
        msg = _blocked(lk2, "write")
        t.check("CL10b ...while a FOUR-line holder whose pid is DEAD is stale: stolen and noted",
                not msg and "stole a STALE clone WRITE lock (its holder is gone)" in lk2.notes,
                repr((msg[:200], lk2.notes)))
        lk2.release()
        _clone_owner(wowner, s.pid, smark, "a reused pid", proc_start=sstart + "0")
        lk2 = CloneLock(clone, env)
        msg = _blocked(lk2, "write")
        t.check("CL11 a live pid whose proc-start is not the recorded one (pid reuse) is stale: "
                "stolen and noted, the lock ours",
                not msg and "stole a STALE clone WRITE lock (its holder is gone)" in lk2.notes and
                open(wowner).read().split("\n")[0] == str(os.getpid()),
                repr((msg[:200], lk2.notes)))
        lk2.release()
        t.check("CL12 release (write) removes our writer", not os.path.exists(lk2.writer))
        _rm_tree(lk.writer)     # (only a mutant leaves one; the arms below need a clean slate)
        reader = "%s/%d.reader" % (lk.readers, s.pid)
        _clone_owner(reader, s.pid, smark, "a live corpus run", proc_start=sstart)
        msg = _blocked(CloneLock(clone, env), "write")
        t.check("CL13 a LIVE reader blocks write(), and no writer is left behind",
                "being READ by a corpus run in progress" in msg and not os.path.exists(lk.writer),
                msg)
        lk3 = CloneLock(clone, env)
        lk3.read("second reader", _CaptureLog())
        t.check("CL14 ...while a second READER is admitted", lk3.role == "read")
        lk3.release()
        os.remove(reader)
    finally:
        _reap(s)
    dead = _dead_pid()
    _clone_owner("%s/%d.reader" % (lk.readers, dead), dead, "m", "a dead corpus run",
                 proc_start="1")
    lk4 = CloneLock(clone, env)
    lk4.write("x", _CaptureLog())
    t.check("CL15 a DEAD reader's marker is removed and noted",
            "removed a STALE clone READ marker (%d.reader)" % dead in lk4.notes, repr(lk4.notes))
    lk4.release()
    os.makedirs(lk.writer, exist_ok=True)
    lk5 = CloneLock(clone, env)
    lk5.read("x", _CaptureLog())
    t.check("CL16 read() over a writer dir with no live holder steals it",
            "stole a STALE clone WRITE lock (its holder is gone)" in lk5.notes
            and not os.path.exists(lk.writer), repr(lk5.notes))
    lk5.release()

    class LateWriter(CloneLock):
        def _write_owner(self, what, path, mark):
            CloneLock._write_owner(self, what, path, mark)
            if path.endswith(".reader"):
                _clone_owner(self.writer + "/owner", os.getpid(), mark[0], "a racing writer",
                             proc_start=mark[1])
    late = LateWriter(clone, env)
    msg = _blocked(late, "read")
    t.check("CL17 a writer that wins the race makes read() BACK OUT (marker removed, blocked)",
            "took this sqlite clone first" in msg and not os.path.exists(late._marker_path()), msg)
    _rm_tree(late.writer)

    class Jammed(CloneLock):
        def _mkdir_writer(self):
            raise FileExistsError(self.writer)
    jam = Jammed(clone, env)
    msg = _blocked(jam, "write")
    t.check("CL18 three steals that never win are refused by name (exit-3 class)",
            "after 3 attempts" in msg and len(jam.notes) == 3, repr((msg[:200], jam.notes)))
    corrupt = os.path.join(work, "corrupt-owner")
    with open(corrupt, "w") as fh:
        fh.write("\nm\nnot-a-number\n")
    t.eq("CL19 a corrupt owner reads as `pid ? — unknown — holding for 0h00m`",
         "pid ? — unknown — holding for 0h00m", lk.holder_desc(corrupt))
    ran = [x for x in t.labels if re.match(r"CL\d\d[a-z]?$", x) and x != "CL00"]
    t.eq("CL20 the lock arms this POSIX run ran are EXACTLY the ones a Windows host skips by name",
         list(_CL_POSIX_ARMS), ran)


def self_test():
    import tempfile
    import traceback
    here = os.path.dirname(os.path.abspath(__file__))
    t = _Checks()
    work = tempfile.mkdtemp(prefix="dss-procs-selftest-")
    try:
        for name, fn in (("enumeration", lambda: _selftest_enumeration(t)),
                         ("ancestors", lambda: _selftest_ancestors(t, here)),
                         ("decoy", lambda: _selftest_decoy(t, work)),
                         ("unverified", lambda: _selftest_unverified(t)),
                         ("launched", lambda: _selftest_launched(t, work)),
                         ("kill_tree", lambda: _selftest_kill_tree(t)),
                         ("markers", lambda: _selftest_markers(t)),
                         ("run lock", lambda: _selftest_run_lock(t, work)),
                         ("clone lock", lambda: _selftest_clone_lock(t, work))):
            try:
                fn()
            except Exception:  # noqa: BLE001 -- a crashed group is a FAILURE, never a skip
                t.check("group '%s' ran to completion" % name, False, traceback.format_exc())
    finally:
        for _ in range(40):
            try:
                shutil.rmtree(work)
                break
            except OSError:
                time.sleep(0.25)
    print("passed=%d failed=%d skipped=%d" % (t.passed, t.failed, t.skipped))
    return 0 if t.failed == 0 else 1


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if args == ["--self-test"]:
        return self_test()
    if args in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    print("usage: sqlite_procs.py --self-test   (a library of the sqlite harness; it has no other "
          "verb)", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
