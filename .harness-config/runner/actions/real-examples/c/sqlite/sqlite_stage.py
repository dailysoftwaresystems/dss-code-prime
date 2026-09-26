#!/usr/bin/env python3
"""sqlite_stage.py -- the POSIX half of the SQLite corpus harness: fetch the shared sqlite clone,
configure it, build the two reference oracles, derive the full-source recipes, find the Tcl and
zlib headers, and -- for a Windows host -- stage all of it where the Windows side can read it.

ONE implementation since 2026-09-21 (lane mig, part 4; the transcription rows are P4-11.2's
R50-R77 and the S34-* / derive checks of report 08). It replaces:
  * build-and-test.sh Step 3, Step 4 and the header half of Step 6. On a POSIX host the build
    reads the clone IN PLACE, under the clone WRITE lock the driver holds (it downgrades that
    lock to READ for the corpus, so the lock is the driver's and is passed in here);
  * build-and-test.ps1 Step "3+4/9" and the bash `derive.sh` it generated (with the .sh's
    `dss:clone-lock` region spliced in) and ran as `wsl.exe -e bash -l -c`. On a Windows host
    the SAME code now runs inside WSL as `python3 sqlite_stage.py derive ...`: it takes and
    releases the clone WRITE lock itself, copies the tree into the Windows-visible stage, and
    writes `<stage>/derive-result.json` -- every path spelled for the Windows side.
The union rule decided each place the two drivers disagreed; the decision is stated at its site.

Public API: `StageConfig`, `stage(cfg, log=LOG, lock=None) -> StageResult`,
`StageResult.to_json()` / `StageResult.from_json()`, the `derive` CLI (exit 0; 1 refusal on
stderr; 2 usage; 3 with the FIRST stderr line `DSS-CLONE-LOCK-BLOCKED`), and `--self-test`
(summary `passed=N failed=N skipped=N`). Siblings it codes against by name only: sqlite_base
(`emit_recipe`), sqlite_coherence (`run_check`) and sqlite_procs (`CloneLock`), each imported
when first needed. Importing this module has no side effect beyond the stream block and
`sys.dont_write_bytecode`.
"""
from __future__ import annotations

import sys

# BEFORE any sibling import: the action is `requireInputsUnmoved`, so no __pycache__ may appear.
sys.dont_write_bytecode = True

import argparse  # noqa: E402
import collections  # noqa: E402
import contextlib  # noqa: E402
import fnmatch  # noqa: E402
import glob  # noqa: E402
import importlib  # noqa: E402
import io  # noqa: E402
import json  # noqa: E402
import os  # noqa: E402
import posixpath  # noqa: E402
import re  # noqa: E402
import shutil  # noqa: E402
import subprocess  # noqa: E402
import tempfile  # noqa: E402
import time  # noqa: E402
import traceback  # noqa: E402
import types  # noqa: E402

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

import sqlite_common as C  # noqa: E402 -- after dont_write_bytecode, deliberately

HERE = os.path.dirname(os.path.realpath(__file__))

DEFAULT_SQLITE_REPO_URL = "https://github.com/sqlite/sqlite.git"
DEFAULT_TIER = "veryquick"
RESULT_SCHEMA = "dss-sqlite-stage-result/1"
RESULT_FILE = "derive-result.json"
# ★ WHERE A RUN'S STAGE LIVES, ON EVERY HOST: `<output tree>/stage/` holds the derive's lists and logs and
# its persisted result (RESULT_FILE) -- and, only when the stage is derived through WSL for a Windows host,
# the staged COPY of the sources (a POSIX host's build reads its clone in place). One rule, read by the
# driver's Steps 3-4 (`StageConfig.from_run`; the WSL derive's --out) and by the round-close recompile.
STAGE_SUBDIR = "stage"
# Written into every stage this module creates; its presence is what lets a later derive WIPE the
# directory (a CLI that takes --out must never `rm -rf` a directory it cannot identify as its own).
STAGE_MARKER = ".dss-sqlite-stage"
OLD_STAGE_MARK = "testdir.win.txt"      # what the retired PowerShell driver's stage always carried
CLONE_LOCK_BLOCKED = "DSS-CLONE-LOCK-BLOCKED"
DERIVE_TOOLS = ("git", "gcc", "make", "ar", "tclsh")
STAGE_STAMP = ".dss-stage-identity"
STAGE_STAMP_LEGACY = ".dss-tcl-identity"
FIXTURE_MIN_TUS, FIXTURE_MIN_DEFINES = 150, 18
CLI_MIN_TUS, CLI_MIN_DEFINES = 100, 18
# The roots the .sh searched, in its precedence; the macOS keg / SDK roots are APPENDED on a darwin
# host only (brew and xcrun do not exist elsewhere, so on Linux the lists are exactly these).
BASE_INC_ROOTS = ("/usr/include", "/usr/local/include", "/opt/homebrew/include")
BASE_LIB_ROOTS = ("/usr/lib", "/lib", "/usr/local/lib", "/opt/homebrew/lib")
BASE_CFG_ROOTS = ("/usr/lib", "/usr/lib64", "/usr/local/lib", "/opt/homebrew/lib")
KEG_FORMULAE = ("zlib", "tcl-tk", "tcl-tk@8")
# `brew` where a PATH does not name it: Homebrew's own default prefixes (Apple Silicon, then Intel).
# A NON-LOGIN shell -- how a harness reaches a Mac over ssh -- never sources Homebrew's shellenv, so
# its PATH has no brew even where Homebrew is installed (✔MEASURED 2026-09-23: that Mac's leg PATH
# is the system dirs plus emsdk's, while /opt/homebrew/lib/tclConfig.sh is there). Candidates,
# tried IN ORDER ON EVERY HOST the way `ldconfig_dirs` tries /sbin/ldconfig: a hit is used, a miss
# costs nothing -- never a decision keyed on the host's name.
BREW_CANDIDATES = ("brew", "/opt/homebrew/bin/brew", "/usr/local/bin/brew")
# sqlite resolves a third of its corpus relative to testdir (`$testdir/../ext/<dir>/*.test` under
# `glob -nocomplain`): a stage whose test dir is not ext's sibling silently runs none of them.
TESTDIR_SIBLINGS = ("rtree", "fts5/test", "session")

_j = posixpath.join


def stage_dir_of(out_root):
    """`<out_root>/stage`: where the stage of the run whose output tree is `out_root` lives, on every host
    (`STAGE_SUBDIR`). The HOST's own join: the driver calls it with a host path."""
    return os.path.join(out_root, STAGE_SUBDIR)


def _abs(p):
    """Absolute and normalised, spelled with '/': a no-op on POSIX (where this module runs); on a
    Windows host only the self-test gets here, and every Windows API accepts '/'."""
    a = os.path.abspath(p)
    return a.replace("\\", "/") if os.sep == "\\" else a


def identity(path):
    """`translate_for_host` on a POSIX host: the driver reads the clone in place."""
    return path


class WslPathTranslator:
    """`translate_for_host` inside WSL for a Windows host: `wslpath -m` (the `C:/...` form; a
    Linux-side path becomes `//wsl.localhost/<distro>/...`). Every path it is handed is ABSOLUTE
    and POSIX: `wslpath` takes anything else as relative and returns nonsense (MEASURED:
    `wslpath -m C:/Users/x` answered `<cwd>/C/Users/x`), so a relative path is refused here."""

    def __init__(self, env=None):
        self.env = C.child_env(base=env)
        self.cache = {}

    def __call__(self, path):
        if path in self.cache:
            return self.cache[path]
        if not path.startswith("/"):
            C.die("INTERNAL: '%s' reached the host-path translation, which takes only absolute POSIX "
                  "paths (wslpath would read it as relative and answer a wrong path)." % path)
        r = C.capture(["wslpath", "-m", path], env_=self.env, timeout=60)
        out = (r.out or "").replace("\0", "").strip().splitlines()
        if r.rc != 0 or not out:
            C.die("could not spell %s for the Windows host (wslpath -m exited %d: %s)"
                  % (path, r.rc, ((r.err or r.out) or "").replace("\0", "").strip()[:200]))
        self.cache[path] = out[-1].strip()
        return self.cache[path]


def _rm_f(path):
    """`rm -f`: gone afterwards, whether or not it was there."""
    try:
        os.remove(path)
    except FileNotFoundError:
        pass


def _indent(text, pad="      "):
    return "\n".join(pad + ln for ln in (text or "").rstrip("\n").split("\n"))


def run_to_log(argv, cwd, log_path, env):
    """Run `argv` in `cwd` with stdout AND stderr written to `log_path` as RAW BYTES (the .sh's
    `> log 2>&1`) -> its exit code, never raising on it. A program that cannot start is rc 127,
    with the reason written into the log."""
    os.makedirs(posixpath.dirname(log_path) or ".", exist_ok=True)
    with open(log_path, "wb") as fh:
        try:
            p = subprocess.run([str(a) for a in argv], cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                               stdout=fh, stderr=subprocess.STDOUT)
        except OSError as exc:
            fh.write(("cannot start %s: %s\n" % (argv[0], exc)).encode("utf-8", "replace"))
            return 127
    return p.returncode


def read_list(path):
    """A one-entry-per-line list file (CR tolerated, blank lines skipped)."""
    with open(path, "rb") as fh:
        text = fh.read().decode("utf-8", "replace")
    return [ln.rstrip("\r") for ln in text.split("\n") if ln.rstrip("\r")]


def write_list(path, lines):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        for ln in lines:
            fh.write(ln + "\n")


def write_text_atomic(path, text):
    tmp = "%s.tmp.%d" % (path, os.getpid())
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    os.replace(tmp, path)


# ── the pure pieces ─────────────────────────────────────────────────────────────────

def mk_var(makefile, name):
    """`NAME = value` exactly as configure writes it: the FIRST definition, value trimmed of
    surrounding whitespace (the .sh's `sed -n "s/^NAME[[:space:]]*=[[:space:]]*//p" | sed
    's/[[:space:]]*$//' | sed -n 1p`). `NAME := v`, `NAME ?= v` and a longer name sharing the
    prefix never match; a missing file or name answers ""."""
    try:
        with open(makefile, "rb") as fh:
            text = fh.read().decode("utf-8", "replace")
    except OSError:
        return ""
    rx = re.compile(r"%s[ \t\r\f\v]*=[ \t\r\f\v]*(.*)$" % re.escape(name))
    for line in text.split("\n"):
        m = rx.match(line)
        if m:
            return m.group(1).rstrip(" \t\r\f\v")
    return ""


_TCL_H_VERSION_RE = re.compile(r'#[ \t\r\f\v]*define[ \t\r\f\v]+TCL_VERSION[ \t\r\f\v]+"([0-9][0-9.]*)"')


def tcl_h_version(path):
    """The `#define TCL_VERSION "x.y"` a header declares -- 8.6's `#define` and Tcl 9's indented
    `#   define` alike; the first one wins; "" when there is none or the file is unreadable."""
    try:
        with open(path, "rb") as fh:
            text = fh.read().decode("latin-1")
    except OSError:
        return ""
    for line in text.split("\n"):
        m = _TCL_H_VERSION_RE.match(line)
        if m:
            return m.group(1)
    return ""


_AWK_NUM_RE = re.compile(r"\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)")


def awk_num(s):
    """awk's `s+0`: the longest numeric prefix, 0 when there is none."""
    m = _AWK_NUM_RE.match(s or "")
    return float(m.group(1)) if m else 0.0


def tcl_cfg_for(inventory, version):
    """The first tclConfig.sh (inventory order = code-point path order) declaring `version`."""
    for ver, cfg in inventory:
        if ver == version:
            return cfg
    return ""


def _sort_n(field):
    m = re.match(r"\s*(-?\d+)", field)
    return int(m.group(1)) if m else 0


def _tcl_order(entry):
    """The .sh's `sort -t. -k1,1nr -k2,2nr` key over a "<version> <path>" line: numeric major, then
    numeric minor, both descending; a tie goes to the code-point-smallest line."""
    line = "%s %s" % entry
    parts = line.split(".")
    return (-_sort_n(parts[0]), -_sort_n(parts[1] if len(parts) > 1 else ""), line)


def highest_tcl(inventory):
    """The .sh's `sort -t. -k1,1nr -k2,2nr | sed -n 1p` over "<version> <path>" lines: numeric
    major, then numeric minor, both descending (never `sort -V`, and never a string sort, which
    puts 9.1 above 10.0 and 8.6 above 8.10); a tie goes to the code-point-smallest line."""
    if not inventory:
        return ("", "")
    return sorted(inventory, key=_tcl_order)[0]


def select_tcl(inventory, pin, tclsh_ver):
    """ONE Tcl, deterministically -> (version, tclConfig.sh, how): the pin, EXACTLY (`pinned`, or
    `pin-missing`, which the caller refuses); else the installation matching the tclsh configure
    ran under (`tclsh`); else the highest installed (`highest`, which the caller warns about);
    else `none`."""
    if pin:
        cfg = tcl_cfg_for(inventory, pin)
        return (pin, cfg, "pinned") if cfg else (pin, "", "pin-missing")
    cfg = tcl_cfg_for(inventory, tclsh_ver) if tclsh_ver else ""
    if cfg:
        return tclsh_ver, cfg, "tclsh"
    if inventory:
        ver, cfg = highest_tcl(inventory)
        return ver, cfg, "highest"
    return "", "", "none"


def _sh_dq(s):
    """`s` made literal inside POSIX sh double quotes."""
    return "".join("\\" + ch if ch in '\\"$`' else ch for ch in s)


def pin_shim_text(interpreter):
    """The one-line exec shim that makes the PLAIN name `tclsh` the pinned interpreter. `exec` of
    the absolute path keeps argv[0] absolute, so Tcl finds its init.tcl beside the REAL
    installation. (The .sh interpolated the path raw; a `"` or `$` in it broke the shim.)"""
    return '#!/bin/sh\nexec "%s" "$@"\n' % _sh_dq(interpreter)


def write_pin_shim(interpreter, out_dir, env):
    """`<out>/tcl-pin/tclsh`, executable, put FIRST on the run's PATH (`env`, never os.environ:
    the pin belongs to this run's children, not to the process that called stage()) -> its dir."""
    d = _j(out_dir, "tcl-pin")
    os.makedirs(d, exist_ok=True)
    path = _j(d, "tclsh")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(pin_shim_text(interpreter))
    os.chmod(path, os.stat(path).st_mode | 0o111)
    env["PATH"] = d + os.pathsep + env.get("PATH", "")
    return d


def stage_stamp_text(tclsh_ver, configure_args, make_options):
    """What the build dir was built WITH: the interpreter, the configure argv (pin arguments
    included), the make OPTIONS. Identical for both old drivers when unpinned, so a clone shared
    with a run of the other one is not rebuilt for nothing."""
    return "tclsh=%s configure=%s options=%s" % (
        tclsh_ver, " ".join(configure_args) or "<default>", make_options or "<none>")


def read_stamp(path):
    """`$(cat stamp 2>/dev/null || true)`: the content without its trailing newlines, "" if absent."""
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace").rstrip("\n")
    except OSError:
        return ""


def stamp_decision(bld, sqlite_dir, now):
    """-> (decision, was). `keep` when the stamp matches. Otherwise -- a CHANGED stamp, and an
    ABSENT one too (a tree whose configuration cannot be proved is exactly the tree that links a
    mixed fixture) -- `wipe` when the build dir is identifiably this harness's own (it is
    `<sqlite>/bld-dss`, and holds a Makefile, a stamp, the legacy Tcl stamp, or nothing), else
    `refuse` (R62): `SQLITE_DIR` is operator-settable, so it can name a bld-dss nobody here built."""
    was = read_stamp(_j(bld, STAGE_STAMP))
    if was == now:
        return "keep", was
    ours = (_abs(bld) == _abs(_j(sqlite_dir, "bld-dss")) and os.path.isdir(bld)
            and (os.path.isfile(_j(bld, "Makefile")) or os.path.isfile(_j(bld, STAGE_STAMP))
                 or os.path.isfile(_j(bld, STAGE_STAMP_LEGACY)) or not os.listdir(bld)))
    return ("wipe" if ours else "refuse"), was


def missing_required_defines(optflags, required, make_options):
    """R65: every required define must reach OPT_FEATURE_FLAGS as `-DNAME` or `-DNAME=...`,
    unless it arrives through make OPTIONS (those never appear there; they are checked on the
    derived recipes instead). Word-bounded: `-DX_PARENTHESIS` does not satisfy `X`."""
    opts, flags = " %s " % (make_options or ""), " %s " % (optflags or "")
    missing = []
    for d in required:
        if " -D%s " % d in opts:
            continue
        if " -D%s " % d in flags or " -D%s=" % d in flags:
            continue
        missing.append(d)
    return missing


def missing_capabilities(defines, required):
    """The required defines absent from a derived recipe, where `NAME` and `NAME=VALUE` both
    count (the recipe reader emits a valued define as `NAME=VALUE`, and the configure check
    already accepts `-DNAME=`)."""
    return [d for d in required if not any(x == d or x.startswith(d + "=") for x in defines)]


def assert_recipe_capabilities(label, recipe, defines, required, make_options):
    """R77 -- the ONE check that can see the make-OPTIONS defines at all, asserted on BOTH recipes
    because the defect it was written for WAS the asymmetry between them."""
    missing = missing_capabilities(defines, required)
    if missing:
        C.die("the %s recipe is MISSING declared capabilities:%s\n"
              "      declared (legs.json stageBuild.requiredDefines): %s\n"
              "      make OPTIONS passed:                            %s\n"
              "      derived from:                                   %s\n"
              "      Every one of these was asked for by name. A missing one means the library DSS is about to\n"
              "      build does not have that capability, while this run would go on to report every one of its\n"
              "      test files as 'completed' — with nothing asserted in any of them. The fixture and the CLI\n"
              "      are checked SEPARATELY on purpose: they are derived from two different make targets, and\n"
              "      the two disagreeing is the exact defect this exists to catch."
              % (label, "".join(" " + d for d in missing), " ".join(required),
                 make_options or "<none>", recipe))


def tcl_libs_ldflags(tcl_libs, probe, libdir_for, cc_shown, n_lib_roots):
    """The -L that TCL_LIBS needs but does not carry (Tcl 9 externalised tommath, and main.mk passes
    TCL_LIBS to the testfixture link verbatim) -> (ldflags, notes, warnings). PROBE-GATED: a `-l`
    that already links adds NOTHING, so a Tcl 8.6 host configures exactly once. `probe(args)`
    links a trivial program with the Makefile's own CC; `libdir_for(name)` finds a directory."""
    out, notes, warns = [], [], []
    for tok in (tcl_libs or "").split():
        if not (tok.startswith("-l") and len(tok) > 2):
            continue          # -framework X, -pthread: no search-path question to answer
        name = tok[2:]
        if probe(["-l" + name]):
            continue
        d = libdir_for(name)
        if not d:
            warns.append("tclConfig.sh declares -l%s in TCL_LIBS, which %s cannot resolve and neither "
                         "pkg-config, brew, ldconfig, the compiler's own search dirs nor %d library "
                         "roots can locate. The reference testfixture -- the ATTRIBUTION ORACLE -- will "
                         "NOT link. Install lib%s." % (name, cc_shown, n_lib_roots, name))
            continue
        if probe(["-L" + d, "-l" + name]):
            if "-L" + d not in out:
                out.append("-L" + d)
            notes.append("-l%s is not on a default search path -- adding -L%s" % (name, d))
        else:
            warns.append("lib%s was found under %s, yet %s still cannot link -l%s against it (wrong "
                         "architecture?). The reference testfixture will NOT link."
                         % (name, d, cc_shown, name))
    return " ".join(out), notes, warns


def _tree_matches(root, name, maxdepth=None):
    """[(depth, path)] of every entry NAMED `name` below `root`, as `find -P` sees the tree: a
    symlinked root or directory is not descended, an unreadable directory is skipped (find's
    stderr was discarded). An explicit stack, never recursion; the traversal order does not
    matter because every caller orders the matches itself."""
    hits = []
    if not root or not os.path.isdir(root) or os.path.islink(root):
        return hits
    stack = [(root, 0)]
    while stack:
        d, depth = stack.pop()
        try:
            with os.scandir(d) as it:
                for e in it:
                    if e.name == name:
                        hits.append((depth + 1, _j(d, e.name)))
                    if (maxdepth is None or depth + 1 < maxdepth) and e.is_dir(follow_symlinks=False):
                        stack.append((_j(d, e.name), depth + 1))
        except OSError:
            continue
    return hits


def find_matches(roots, name, maxdepth=None, path_glob=None):
    """[(root index, depth, path)] of every entry NAMED `name` -- and, with `path_glob`, whose WHOLE
    path matches it (find's `-path`) -- under the roots that exist."""
    hits = []
    for i, root in enumerate(roots):
        for depth, p in _tree_matches(root, name, maxdepth):
            if path_glob is None or fnmatch.fnmatchcase(p, path_glob):
                hits.append((i, depth, p))
    return hits


def find_first(roots, name, maxdepth=None, path_glob=None):
    """The .sh's `find_in <roots> -- ... | sed -n 1p`, made deterministic: the first ROOT (in
    declared order) holding a match, then the SHALLOWEST match in it, then code-point order.
    `find` itself answered in readdir order, which can return a vendored copy nested below a root
    (a zlib.h under some library's include subdirectory) ahead of the system header beside it."""
    hits = find_matches(roots, name, maxdepth, path_glob)
    return min(hits)[2] if hits else ""


def find_ordered(roots, name, maxdepth=None):
    """Every match, in find_first's order."""
    return [h[2] for h in sorted(find_matches(roots, name, maxdepth))]


def find_all_sorted(roots, name, maxdepth=None):
    """Every match, `sort -u`'d (code-point order)."""
    return sorted(set(h[2] for h in find_matches(roots, name, maxdepth)))


def remap_path(path, pairs):
    """`path` moved from the POSIX side's tree into the stage by the FIRST (source prefix, staged
    prefix) pair it lies under -- the pairs are ordered most specific first -- or None."""
    for src, dst in pairs:
        s = src.rstrip("/")
        if path == s:
            return dst
        if path.startswith(s + "/"):
            return dst + path[len(s):]
    return None


def stage_pairs(sqlite_dir, bld, tcl_inc, stage_dir):
    """The remap table: the build dir BEFORE the clone (it lies inside it), the chosen Tcl header
    dir, the clone; then the same three under their real spellings, for a recipe make wrote
    through a symlinked path. Paths BELOW the build dir keep their relative path (the .ps1
    flattened `$BLD/*` to a basename, so a TU in a build-dir SUBDIRECTORY silently became a
    same-named top-level file)."""
    s_sqlite = _j(stage_dir, "sqlite")
    pairs = [(bld, _j(s_sqlite, "bld"))]
    if tcl_inc:
        pairs.append((tcl_inc, _j(stage_dir, "tclinc")))
    pairs.append((sqlite_dir, s_sqlite))
    for src, dst in list(pairs):
        real = _abs(os.path.realpath(src))
        if real != src:
            pairs.append((real, dst))
    return pairs


def remap_list(paths, pairs, want_dir, translate):
    """-> (host lines, lost, passed_through). A path under a staged root is moved there; a path
    under none is kept as it is (the .ps1's `*) echo "$p"`) and REPORTED; an entry whose staged
    location is not a file (TUs) / directory (-I dirs) is lost/dropped. Order is kept."""
    lines, lost, passed = [], [], []
    for p in paths:
        staged = remap_path(p, pairs)
        if staged is None:
            staged = p
            passed.append(p)
        present = os.path.isdir(staged) if want_dir else os.path.isfile(staged)
        if present:
            lines.append(translate(staged))
        else:
            lost.append(p)
    return lines, lost, passed


def default_jobs():
    """`nproc` (the CPUs this process may run on), else the machine's count, else 4."""
    if hasattr(os, "sched_getaffinity"):
        try:
            n = len(os.sched_getaffinity(0))
            if n > 0:
                return n
        except OSError:
            pass
    return os.cpu_count() or 4


# ── the package manager (the .sh's pkg_install / ensure_cmd) ─────────────────────────

class PkgInstaller:
    """`pkg_install <apt-pkg> [<brew-pkg>]` with the .sh's semantics: Homebrew on a darwin host,
    idempotent through `brew list`; apt-get elsewhere, through `sudo` unless root, `update` ONCE per
    installer, DEBIAN_FRONTEND=noninteractive. Output discarded; a failure is a named refusal.
    Which manager is a HOST fact; it never decides which leg is built."""

    def __init__(self, host_os, log=None, env=None):
        self.host_os = host_os
        self.log = log if log is not None else C.LOG
        self.env = env if env is not None else dict(os.environ)
        self.apt_updated = False

    def __call__(self, apt_pkg, brew_pkg=None):
        brew_pkg = brew_pkg or apt_pkg
        manager = "brew" if self.host_os == "darwin" else "apt-get"
        path = self.env.get("PATH") or os.defpath
        exe = (resolve_brew(lambda n: shutil.which(n, path=path)) if manager == "brew"
               else shutil.which(manager, path=path))
        if not exe:
            if manager == "brew":
                C.die("Homebrew not found — on PATH or at %s — install from https://brew.sh, then re-run "
                      "(needed for: %s)." % (", ".join(BREW_CANDIDATES[1:]), brew_pkg))
            C.die("apt-get not found — this harness targets Debian/Ubuntu/WSL + macOS\n"
                  "      (host OS identified as: %s). Install manually: %s"
                  % (self.host_os or "UNIDENTIFIED", apt_pkg))
        if manager == "brew":
            self.log.info("installing (brew): %s" % brew_pkg)
            if C.capture([exe, "list", brew_pkg], env_=self.env).rc != 0:
                C.run_checked([exe, "install", brew_pkg], "brew install %s" % brew_pkg, env_=self.env)
            return
        root = hasattr(os, "geteuid") and os.geteuid() == 0
        sudo = [] if root else ["sudo"]
        if not self.apt_updated:
            C.run_checked(sudo + ["apt-get", "update", "-y"], "apt-get update", env_=self.env)
            self.apt_updated = True
        self.log.info("installing (apt): %s" % apt_pkg)
        env = dict(self.env, DEBIAN_FRONTEND="noninteractive")
        # sudo resets the environment, so the variable rides as sudo's own VAR=value argument.
        argv = sudo + (["DEBIAN_FRONTEND=noninteractive"] if sudo else []) + ["apt-get", "install", "-y", apt_pkg]
        C.run_checked(argv, "apt-get install %s" % apt_pkg, env_=env)


def refuse_install(apt_pkg, brew_pkg=None):
    """The derive's pkg_install: it runs under `wsl.exe -e` with no terminal for sudo to ask on,
    so a missing package is a refusal naming the command (the PowerShell derive never installed)."""
    C.die("'%s' is needed and is not installed on the POSIX side, and the derive does not install\n"
          "      packages (it runs under `wsl.exe -e`, with no terminal for sudo to ask on). Install it\n"
          "      inside the distro, then re-run:\n          sudo apt-get install -y %s" % (apt_pkg, apt_pkg))


def ensure_cmd(cmd, apt_pkg, brew_pkg=None, which=None, pkg_install=None):
    """`ensure_cmd <command> <apt-pkg> [<brew-pkg>]`."""
    which = which or shutil.which
    if not which(cmd):
        (pkg_install or PkgInstaller(C.host_os()))(apt_pkg, brew_pkg or apt_pkg)


# ── configuration ──────────────────────────────────────────────────────────────────

_CONTRACT = ("That is a contract break between the resolver and this driver, not a property of "
             "this host.")


def _str_list(x):
    return isinstance(x, list) and bool(x) and all(isinstance(i, str) and i for i in x)


def parse_stage_build(obj):
    """`harness_legs.py --stage-build --format json` (the text or the parsed object), checked for
    SHAPE -> {configure_flags, make_options, required_defines, witnesses, option_defines}. The
    resolver owns the policy (its charset checks); what crosses a process -- and, from a Windows
    host, a file -- boundary is re-checked here, because a MISSING declaration must never read as
    "no extensions wanted" (362 of 1,241 corpus files once completed asserting nothing)."""
    if isinstance(obj, bytes):
        obj = obj.decode("utf-8-sig", "replace")
    if isinstance(obj, str):
        try:
            obj = json.loads(obj.lstrip("\ufeff"))
        except ValueError as exc:
            C.die("harness_legs.py --stage-build exited 0 but did not print the JSON this driver reads "
                  "(%s). %s" % (exc, _CONTRACT))
    if not isinstance(obj, dict):
        C.die("harness_legs.py --stage-build answered %s, not the JSON object this driver reads. %s"
              % (type(obj).__name__, _CONTRACT))
    flags, req = obj.get("configureFlags"), obj.get("requiredDefines")
    if not _str_list(flags) or not _str_list(req):
        C.die("harness_legs.py --stage-build returned a configuration with no configureFlags or no "
              "requiredDefines. %s\n      It is the ONE declaration of which extensions the corpus "
              "tests; configuring without it builds a sqlite with fts5/fts3/rtree/session OFF, and "
              "the corpus then reports their test files as 'completed' having asserted nothing."
              % _CONTRACT)
    for f in flags + req:
        if any(ch.isspace() for ch in f):
            C.die("harness_legs.py --stage-build returned '%s', which carries whitespace; each entry is "
                  "ONE argv word. %s" % (f, _CONTRACT))
    mo = obj.get("makeOptions", "")
    wit = obj.get("capabilityWitnesses", {})
    opt = obj.get("optionDefines", [])
    if not isinstance(mo, str) or not isinstance(wit, dict) or not isinstance(opt, list):
        C.die("harness_legs.py --stage-build returned makeOptions/capabilityWitnesses/optionDefines of "
              "the wrong type (%s/%s/%s). %s" % (type(mo).__name__, type(wit).__name__,
                                                  type(opt).__name__, _CONTRACT))
    commit = obj.get("sqliteCommit")
    if not isinstance(commit, str) or not re.match(r"^[0-9a-f]{40}$", commit):
        C.die("harness_legs.py --stage-build returned no FULL sqliteCommit (got %r). %s\n      It is the ONE "
              "sqlite revision every stage compiles; without it the stage pulls whatever upstream's default "
              "branch is that day, and the round-close recompile's subject moves under it." % (commit, _CONTRACT))
    return {"configure_flags": list(flags), "make_options": mo, "required_defines": list(req),
            "witnesses": dict(wit), "option_defines": list(opt), "sqlite_commit": commit}


def _parse_jobs(value):
    v = (value or "").strip()
    if not v:
        return default_jobs()
    try:
        n = int(v)
    except ValueError:
        C.die("JOBS='%s' is not an integer." % v)
    if n < 1:
        C.die("JOBS=%d is below its minimum of 1." % n)
    return n


class StageConfig:
    """Every input the POSIX half reads, validated once; `stage()` reads nothing else (no globals,
    no os.environ). Build it with `from_env` (a POSIX host's driver) or through the `derive` CLI."""

    def __init__(self, *, sqlite_dir, out_dir, stage_build, tier=DEFAULT_TIER, test_file="",
                 jobs=None, tcl_version="", sqlite_repo_url=DEFAULT_SQLITE_REPO_URL,
                 copy_to_stage=False, translate_for_host=identity, host_os="linux",
                 environ=None, pkg_install=None, lock_what=""):
        if host_os not in ("linux", "darwin"):
            C.die("the POSIX half runs on a POSIX host (this one is '%s'): in-process on linux/darwin, "
                  "and inside WSL through `wsl.exe -e python3 sqlite_stage.py derive ...` for a Windows "
                  "host." % host_os)
        if not sqlite_dir or not out_dir:
            C.die("INTERNAL: StageConfig needs both sqlite_dir and out_dir.")
        self.host_os = host_os
        self.sqlite_dir = _abs(sqlite_dir)
        self.out_dir = _abs(out_dir)
        sb = parse_stage_build(stage_build)
        self.configure_flags = sb["configure_flags"]
        self.make_options = sb["make_options"]
        self.required_defines = sb["required_defines"]
        self.witnesses = sb["witnesses"]
        self.option_defines = sb["option_defines"]
        self.sqlite_commit = sb["sqlite_commit"]
        self.tier = (tier or DEFAULT_TIER).strip()
        if not self.tier or "/" in self.tier or any(ch.isspace() for ch in self.tier):
            C.die("DSS_TIER='%s' is not a tier name (it names <sqlite>/test/<tier>.test)." % tier)
        self.test_file = test_file or ""
        self.jobs = default_jobs() if jobs is None else int(jobs)
        if self.jobs < 1:
            C.die("jobs=%d is below its minimum of 1." % self.jobs)
        self.tcl_version = (tcl_version or "").strip()
        self.sqlite_repo_url = sqlite_repo_url or DEFAULT_SQLITE_REPO_URL
        self.copy_to_stage = bool(copy_to_stage)
        self.translate_for_host = translate_for_host or identity
        if not self.copy_to_stage and self.translate_for_host is not identity:
            C.die("INTERNAL: translate_for_host must be the identity when the build reads the clone in "
                  "place (copy_to_stage False): only a STAGED tree is ever spelled for another host.")
        self.environ = dict(os.environ if environ is None else environ)
        self.pkg_install = pkg_install
        self.lock_what = lock_what or ("sqlite_stage.py staging/build — tier %s" % self.tier)
        clone, out = self.sqlite_dir.rstrip("/"), self.out_dir.rstrip("/")
        if out == clone or clone.startswith(out + "/"):
            C.die("the output directory %s is the sqlite clone or contains it; it would be written (and, "
                  "when staging, wiped) through the checkout the harness pulls." % self.out_dir)
        if self.copy_to_stage and out.startswith(clone + "/"):
            C.die("the stage %s lies inside the sqlite clone %s; a stage is wiped and re-copied on every "
                  "run and must never live inside the checkout it is copied from." % (self.out_dir, clone))

    @classmethod
    def from_env(cls, *, out_dir, stage_build, environ=None, **overrides):
        """The driver's variables: SQLITE_DIR ($HOME/src/sqlite), SQLITE_REPO_URL, DSS_JOBS (else JOBS,
        else nproc -- `sqlite_common.Config`'s order, the one rule for that knob), DSS_TCL_VERSION,
        DSS_TIER (veryquick), DSS_TEST_FILE."""
        e = dict(os.environ if environ is None else environ)
        home = e.get("HOME") or os.path.expanduser("~")
        kw = dict(sqlite_dir=e.get("SQLITE_DIR") or _j(home, "src", "sqlite"),
                  sqlite_repo_url=e.get("SQLITE_REPO_URL") or DEFAULT_SQLITE_REPO_URL,
                  jobs=_parse_jobs((e.get("DSS_JOBS") or "").strip() or e.get("JOBS") or ""),
                  tcl_version=e.get("DSS_TCL_VERSION", ""),
                  tier=e.get("DSS_TIER") or DEFAULT_TIER,
                  test_file=e.get("DSS_TEST_FILE", ""),
                  environ=e)
        if "host_os" not in overrides:
            kw["host_os"] = C.host_os()
        kw.update(overrides)
        return cls(out_dir=out_dir, stage_build=stage_build, **kw)

    @classmethod
    def from_run(cls, run, **overrides):
        """The POSIX host's driver: its validated `Config` (`run.cfg`: sqlite_repo_url, jobs,
        tcl_version, tier, test_file), the clone's POSIX path it resolved (`run.sqlite_dir_posix`),
        its output tree (`run.out_dir`; the stage is written under its `stage/`, `stage_dir_of`), the
        resolver's `--stage-build` answer (`run.stage_build`) and
        the host (`run.host`). Nothing is defaulted silently: a missing one is refused by name."""
        c = run.cfg
        for name in ("sqlite_dir_posix", "out_dir", "stage_build", "host"):
            if not getattr(run, name, None):
                C.die("INTERNAL: StageConfig.from_run needs run.%s, which is not set yet." % name)
        kw = dict(sqlite_dir=run.sqlite_dir_posix, out_dir=stage_dir_of(run.out_dir), stage_build=run.stage_build,
                  tier=c.tier, test_file=c.test_file, jobs=c.jobs, tcl_version=c.tcl_version,
                  sqlite_repo_url=c.sqlite_repo_url, host_os=run.host, environ=dict(os.environ),
                  lock_what="build_and_test.py staging/build — tier %s" % c.tier)
        kw.update(overrides)
        return cls(**kw)


# ── the result ─────────────────────────────────────────────────────────────────────

class RecipeFiles:
    """One derived recipe as the driver reads it: the `tus` / `defines` / `includes` list FILES and
    the `recipe` (make -n output) file, spelled for the host, plus the summary and the counts."""
    KEYS = ("tus", "defines", "includes", "recipe", "summary", "n_tus", "n_defines", "n_includes")
    PATH_KEYS = ("tus", "defines", "includes", "recipe")

    def __init__(self, **kw):
        if set(kw) != set(self.KEYS):
            C.die("INTERNAL: a recipe record needs exactly %s (got %s)" % (", ".join(self.KEYS),
                                                                        ", ".join(sorted(kw))))
        for k in self.KEYS:
            setattr(self, k, kw[k])

    def to_dict(self):
        return dict((k, getattr(self, k)) for k in self.KEYS)

    @classmethod
    def from_dict(cls, d, where):
        if not isinstance(d, dict) or set(d) != set(cls.KEYS):
            C.die("derive result: %s is not a recipe record (keys %s)"
                  % (where, sorted(d) if isinstance(d, dict) else type(d).__name__))
        for k in cls.PATH_KEYS + ("summary",):
            if not isinstance(d[k], str):
                C.die("derive result: %s.%s is not a string" % (where, k))
        for k in ("n_tus", "n_defines", "n_includes"):
            if not isinstance(d[k], int) or isinstance(d[k], bool):
                C.die("derive result: %s.%s is not an integer" % (where, k))
        return cls(**d)

    def translated(self, fn):
        d = self.to_dict()
        for k in self.PATH_KEYS:
            d[k] = fn(d[k])
        return RecipeFiles(**d)

    def __eq__(self, other):
        return isinstance(other, RecipeFiles) and self.to_dict() == other.to_dict()

    __hash__ = None

    def __repr__(self):
        return "RecipeFiles(%r)" % self.to_dict()


class StageResult:
    """Every value the driver needs from the POSIX half. HOST paths are spelled for the host that
    consumes them (`translate_for_host`); an OPTIONAL host path is None (reference_*) or ""
    (test_file: no DSS_TEST_FILE) when absent; a field ending in `_posix` is the POSIX side's own
    spelling BY CONTRACT (for a launcher, or for a command run back on that side) and is never
    translated; `tcl_lib_file` is a file NAME, never translated."""
    HOST_PATHS = ("out_dir", "sqlite_dir", "bld", "src", "ext", "testdir", "tier_file", "tcl_inc",
                  "zinc_src", "sqlite_cfg_h", "reference_build_log", "reference_cli_log",
                  "amalgamation_log", "configure_log")
    OPTIONAL_HOST_PATHS = ("reference_fixture", "reference_cli", "test_file")
    POSIX_PATHS = ("out_dir_posix", "sqlite_dir_posix", "bld_posix", "tclsh_posix",
                   "tcl_config_posix", "reference_fixture_posix", "reference_cli_posix")
    RECIPES = ("fixture_recipe", "cli_recipe")
    STRINGS = ("tier", "tcl_version", "tcl_lib_file", "reference_fixture_why",
               "reference_cli_why", "amalgamation_regen", "sqlite_head", "sqlite_branch",
               "stage_identity", "make_options")
    LISTS = ("configure_args", "required_defines", "clone_lock_notes", "ref_link_notes",
             "ref_link_warnings", "warnings")
    OTHER = ("copy_to_stage", "witnesses")
    FIELDS = HOST_PATHS + OPTIONAL_HOST_PATHS + POSIX_PATHS + RECIPES + STRINGS + LISTS + OTHER

    def __init__(self, **kw):
        if set(kw) != set(self.FIELDS):
            miss = sorted(set(self.FIELDS) - set(kw))
            extra = sorted(set(kw) - set(self.FIELDS))
            C.die("INTERNAL: a StageResult needs exactly its fields (missing %s, unknown %s)" % (miss, extra))
        for k in self.FIELDS:
            setattr(self, k, kw[k])

    def to_dict(self):
        d = {"schema": RESULT_SCHEMA}
        for k in self.FIELDS:
            v = getattr(self, k)
            if k in self.RECIPES:
                v = v.to_dict()
            elif k in self.LISTS:
                v = list(v)
            elif k == "witnesses":
                v = dict(v)
            d[k] = v
        return d

    def to_json(self):
        return json.dumps(self.to_dict(), indent=1, sort_keys=True) + "\n"

    @classmethod
    def from_dict(cls, d):
        if not isinstance(d, dict):
            C.die("derive result: not a JSON object")
        if d.get("schema") != RESULT_SCHEMA:
            C.die("derive result: schema %r, this driver reads %r -- the derive and the driver come from "
                  "different trees." % (d.get("schema"), RESULT_SCHEMA))
        keys = set(d) - {"schema"}
        if keys != set(cls.FIELDS):
            C.die("derive result: fields missing %s, unknown %s -- a contract break between the derive and "
                  "this driver." % (sorted(set(cls.FIELDS) - keys), sorted(keys - set(cls.FIELDS))))
        kw = {}
        for k in cls.FIELDS:
            v = d[k]
            if k in cls.RECIPES:
                v = RecipeFiles.from_dict(v, k)
            elif k in cls.HOST_PATHS or k in cls.STRINGS:
                if not isinstance(v, str):
                    C.die("derive result: %s is not a string" % k)
            elif k in cls.OPTIONAL_HOST_PATHS or k in cls.POSIX_PATHS:
                if v is not None and not isinstance(v, str):
                    C.die("derive result: %s is neither a string nor null" % k)
            elif k in cls.LISTS:
                if not isinstance(v, list) or not all(isinstance(i, str) for i in v):
                    C.die("derive result: %s is not a list of strings" % k)
            elif k == "witnesses":
                if not isinstance(v, dict):
                    C.die("derive result: witnesses is not an object")
            elif k == "copy_to_stage":
                if not isinstance(v, bool):
                    C.die("derive result: copy_to_stage is not a boolean")
            kw[k] = v
        return cls(**kw)

    @classmethod
    def from_json(cls, text):
        try:
            d = json.loads(text.lstrip("\ufeff") if isinstance(text, str) else text)
        except ValueError as exc:
            C.die("derive result: not JSON (%s)" % exc)
        return cls.from_dict(d)

    def translated(self, fn):
        """A copy with `fn` applied to EVERY host path (None stays None) and to every recipe file."""
        d = dict((k, getattr(self, k)) for k in self.FIELDS)
        for k in self.HOST_PATHS:
            d[k] = fn(d[k])
        for k in self.OPTIONAL_HOST_PATHS:
            d[k] = fn(d[k]) if d[k] else d[k]
        for k in self.RECIPES:
            d[k] = d[k].translated(fn)
        return StageResult(**d)

    def __eq__(self, other):
        return isinstance(other, StageResult) and self.to_dict() == other.to_dict()

    __hash__ = None


# ── one run's context ──────────────────────────────────────────────────────────────

class _Ctx:
    """One stage() run: its config, log, CHILD environment (the Tcl pin and the macOS keg bin go
    on this PATH, never on os.environ), the discovered roots and the notes it collects."""

    def __init__(self, cfg, log):
        self.cfg, self.log = cfg, log
        self.env = dict(cfg.environ)
        self._install = cfg.pkg_install or PkgInstaller(cfg.host_os, log, self.env)
        self.install_attempted = False
        self.warnings, self.ref_link_notes, self.ref_link_warnings = [], [], []
        self.path_prefix = []
        self.extra_bin = []
        self.inc_roots, self.lib_roots, self.cfg_roots = BASE_INC_ROOTS, BASE_LIB_ROOTS, BASE_CFG_ROOTS
        self.inventory = []
        self._which = {}

    def pkg_install(self, apt_pkg, brew_pkg=None):
        # Recorded BEFORE the call: an install that ran and then failed may still have changed
        # what is installed, which is exactly when Step 6 must re-take the Tcl inventory.
        self.install_attempted = True
        self._which.clear()
        return self._install(apt_pkg, brew_pkg)

    def which(self, name):
        """shutil.which over THIS run's PATH, remembered per (name, PATH): inside WSL every miss
        stats ~35 Windows directories through interop (MEASURED: 0.3 s a lookup)."""
        path = self.env.get("PATH") or os.defpath
        key = (name, path)
        if key not in self._which:
            self._which[key] = shutil.which(name, path=path)
        return self._which[key]

    def run(self, argv, **kw):
        kw.setdefault("env_", self.env)
        return C.capture([str(a) for a in argv], **kw)

    def host(self, path):
        return self.cfg.translate_for_host(path) if path else path

    def prepend_path(self, d):
        self.env["PATH"] = d + os.pathsep + self.env.get("PATH", "")
        self.path_prefix.insert(0, d)

    def warn(self, text, reason=None):
        for line in text.split("\n"):
            self.log.warn(line)
        self.warnings.append(reason or text.split("\n")[0].strip())


def _sibling(name, what):
    """A sibling module of this action, imported when first needed. Missing is a named refusal:
    each is part of the harness and has no skip path by design."""
    try:
        return importlib.import_module(name)
    except ImportError as exc:
        C.die("%s (%s.py beside this file) could not be imported: %s\n      It is part of this harness "
              "and has no skip path by design; restore the file rather than routing around it."
              % (what, name, exc))


def resolve_brew(which):
    """The first of BREW_CANDIDATES that `which` finds, "" when none is there."""
    for cand in BREW_CANDIDATES:
        found = which(cand)
        if found:
            return found
    return ""


def brew_prefix(ctx, formula):
    """`brew --prefix <f>` -- the WOULD-BE prefix even when the formula is not installed, "" when
    there is no brew (so on Linux every caller degrades to nothing)."""
    brew = resolve_brew(ctx.which)
    if not brew:
        return ""
    return ctx.run([brew, "--prefix", formula], timeout=120).out.strip()


def sdk_prefix(ctx):
    if not ctx.which("xcrun"):
        return ""
    return ctx.run(["xcrun", "--show-sdk-path"], timeout=120).out.strip()


def discover_roots(ctx):
    """INC / LIB / CFG roots: the fixed lists, plus -- on a darwin host, a HOST fact -- the keg
    prefixes of zlib / tcl-tk / tcl-tk@8 and the SDK include dir, APPENDED (the legacy roots keep
    their precedence)."""
    extra_inc, extra_lib, extra_bin = [], [], []
    if ctx.cfg.host_os == "darwin":
        for f in KEG_FORMULAE:
            p = brew_prefix(ctx, f)
            if p:
                extra_inc.append(_j(p, "include"))
                extra_lib.append(_j(p, "lib"))
                extra_bin.append(_j(p, "bin"))
        sdk = sdk_prefix(ctx)
        if sdk:
            extra_inc.append(_j(sdk, "usr", "include"))
    ctx.extra_bin = extra_bin
    ctx.inc_roots = BASE_INC_ROOTS + tuple(extra_inc)
    ctx.lib_roots = BASE_LIB_ROOTS + tuple(extra_lib)
    ctx.cfg_roots = BASE_CFG_ROOTS + tuple(extra_lib)


_CFG_NAME_RE = re.compile(r"^[A-Z_][A-Z0-9_]*$")


def tcl_config_values(ctx, cfg_path, names):
    """What a tclConfig.sh ASSIGNS, read by SOURCING it in `sh` -- it is shell code, its values can
    be built from other variables -- never by parsing it. A file that cannot be sourced answers ""
    for every name (the .sh's `|| ver=""`); trailing newlines are stripped as `$(...)` did."""
    names = tuple(names)
    for n in names:
        if not _CFG_NAME_RE.match(n):
            C.die("INTERNAL: %r is not a shell variable name" % n)
    empty = dict((n, "") for n in names)
    if not cfg_path:
        return empty
    script = ('. "$1" >/dev/null 2>&1; printf \'%s\\037\' '
              + " ".join('"${%s:-}"' % n for n in names))
    r = ctx.run(["sh", "-c", script, "sh", cfg_path], timeout=60)
    if r.rc != 0:
        return empty
    parts = r.out.split("\x1f")
    if len(parts) < len(names) + 1:
        return empty
    return dict((n, parts[i].rstrip("\n")) for i, n in enumerate(names))


def tcl_inventory(ctx):
    """[(TCL_VERSION, tclConfig.sh)] of every installation under the CFG roots, in code-point path
    order -- version-agnostic, straight from each installation's own description of itself."""
    inv = []
    for cfg in find_all_sorted(ctx.cfg_roots, "tclConfig.sh"):
        ver = tcl_config_values(ctx, cfg, ("TCL_VERSION",))["TCL_VERSION"]
        if ver:
            inv.append((ver, cfg))
    return inv


def tclsh_version(ctx, interpreter="tclsh"):
    """`echo 'puts $tcl_version' | <tclsh> 2>/dev/null || true`."""
    return ctx.run([interpreter], input_text="puts $tcl_version\n", timeout=60).out.strip()


def tclsh_bin_for(ctx, want):
    """An interpreter REPORTING `want`, the most authoritative first: the installation whose
    tclConfig.sh declares it (TCL_EXEC_PREFIX/bin, then the keg layout beside it), `tclsh<want>` on
    PATH, the keg bin roots, plain `tclsh`. Each candidate is ASKED -- a file name is no proof."""
    cands = []
    cfg = tcl_cfg_for(ctx.inventory, want)
    if cfg:
        pfx = tcl_config_values(ctx, cfg, ("TCL_EXEC_PREFIX",))["TCL_EXEC_PREFIX"]
        if pfx:
            cands += [_j(pfx, "bin", "tclsh" + want), _j(pfx, "bin", "tclsh")]
        top = posixpath.dirname(posixpath.dirname(cfg))
        cands += [_j(top, "bin", "tclsh" + want), _j(top, "bin", "tclsh")]
    cands.append(ctx.which("tclsh" + want) or "")
    for r in ctx.extra_bin:
        if r:
            cands += [_j(r, "tclsh" + want), _j(r, "tclsh")]
    cands.append(ctx.which("tclsh") or "")
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK) and tclsh_version(ctx, c) == want:
            return c
    return ""


def installed_tclsh(ctx):
    """-> (interpreter, version, tclConfig.sh) of the NEWEST installed Tcl >= 8.6 in the run's
    inventory whose OWN interpreter answers its version (`tclsh_bin_for`), or ("", "", "")."""
    for ver, _cfg in sorted(ctx.inventory, key=_tcl_order):
        if awk_num(ver) < 8.6:
            continue
        sh = tclsh_bin_for(ctx, ver)
        if sh:
            return sh, ver, tcl_cfg_for(ctx.inventory, ver)
    return "", "", ""


def choose_tclsh(ctx):
    """The tclsh an UNPINNED stage runs -> (interpreter, version, how): PATH's `tclsh` when it
    reports >= 8.6 (`path`); else the newest INSTALLED Tcl >= 8.6 whose own interpreter answers
    (`installed`) -- ✔MEASURED 2026-09-23 on the Mac a harness reaches: PATH's tclsh is Apple's
    8.5 while /opt/homebrew/lib/tclConfig.sh declares 9.0, and installing over an installed Tcl
    was the old answer; else nothing (`none`: the caller installs)."""
    path_sh = ctx.which("tclsh") or ""
    ver = tclsh_version(ctx) if path_sh else ""
    if ver and awk_num(ver) >= 8.6:
        return path_sh, ver, "path"
    sh, inst_ver, _cfg = installed_tclsh(ctx)
    if sh:
        return sh, inst_ver, "installed"
    return "", ver, "none"


def ensure_tclsh(ctx):
    """R56-R59. PINNED (DSS_TCL_VERSION): EXACTLY that version on PATH, through the shim when the
    plain name is another one; nothing installed (an absent pin is the operator's decision).
    UNPINNED: `choose_tclsh` -- PATH's tclsh >= 8.6, else an INSTALLED one >= 8.6 put first on the
    run's PATH through the same shim a pin uses (and carried into configure, like a pin), else
    install tcl. -> (sh, cfg): what configure must be told, ("", "") when PATH's tclsh is used."""
    pin = ctx.cfg.tcl_version
    ver = tclsh_version(ctx) if ctx.which("tclsh") else ""
    if pin:
        pin_cfg = tcl_cfg_for(ctx.inventory, pin)
        if ver != pin:
            pin_sh = tclsh_bin_for(ctx, pin)
            if not pin_sh:
                C.die("DSS_TCL_VERSION=%s is pinned, but NO tclsh reporting %s was found.\n"
                      "      tclsh on PATH  : %s (reports %s)\n"
                      "      tclConfig.sh   : %s\n"
                      "      searched       : each config's TCL_EXEC_PREFIX/bin + <prefix>/bin, 'tclsh%s' on\n"
                      "                       PATH, %s, then PATH 'tclsh'\n"
                      "      Install it (apt: tcl%s — brew: 'tcl-tk@8' for 8.6 / 'tcl-tk' for 9.x, both\n"
                      "      KEG-ONLY), or unset DSS_TCL_VERSION. NOT continuing on a different Tcl: sqlite's configure,\n"
                      "      make -n and mksqlite3c.tcl all run under this interpreter and bake ITS -I dirs into the\n"
                      "      recipe, so a mismatched tclsh silently builds the fixture against two different Tcls."
                      % (pin, pin, ctx.which("tclsh") or "none", ver or "none",
                         ";".join("%s %s" % e for e in ctx.inventory) or "<none>", pin,
                         " ".join(ctx.extra_bin) or "<no keg bin roots>", pin))
            d = write_pin_shim(pin_sh, ctx.cfg.out_dir, ctx.env)
            ctx.path_prefix.insert(0, d)
            ver = tclsh_version(ctx)
        else:
            pin_sh = ctx.which("tclsh") or ""
        if ver != pin:
            C.die("tclsh is STILL %s after pinning to %s (shim: %s) — refusing to continue."
                  % (ver or "<none>", pin, _j(ctx.cfg.out_dir, "tcl-pin")))
        ctx.log.info("tclsh %s (%s) — PINNED by DSS_TCL_VERSION" % (ver, ctx.which("tclsh")))
        return pin_sh, pin_cfg
    chosen_sh, chosen_ver, how = choose_tclsh(ctx)
    if how == "installed":
        d = write_pin_shim(chosen_sh, ctx.cfg.out_dir, ctx.env)
        ctx.path_prefix.insert(0, d)
        chosen_cfg = tcl_cfg_for(ctx.inventory, chosen_ver)
        ctx.log.info("tclsh on PATH %s — using the INSTALLED Tcl %s: %s (%s)"
                     % ("reports %s (< 8.6)" % ver if ver else "is absent", chosen_ver, chosen_sh,
                        chosen_cfg))
        return chosen_sh, chosen_cfg
    if not ver or awk_num(ver) < 8.6:
        if ver:
            ctx.log.info("tclsh %s is < 8.6 and no installed Tcl >= 8.6 answers — installing a newer tcl"
                         % ver)
        ctx.pkg_install("tcl", "tcl-tk")
        if ctx.cfg.host_os == "darwin":
            # HOST fact: Homebrew's tcl-tk is KEG-ONLY, so its bin/ is not on PATH after an install.
            p = brew_prefix(ctx, "tcl-tk")
            if p and os.path.isdir(_j(p, "bin")):
                ctx.prepend_path(_j(p, "bin"))
    if not ctx.which("tclsh"):
        C.die("tclsh not found after install.")
    ver = tclsh_version(ctx)
    if awk_num(ver) < 8.6:
        C.die("tclsh %s is < 8.6." % (ver or "none"))
    ctx.log.info("tclsh %s (%s)" % (ver, ctx.which("tclsh")))
    return "", ""


def tcl_configure_args(tclsh, tcl_config):
    """What sqlite's configure is told about the Tcl this run chose -- a pin, or an INSTALLED Tcl
    taken over an older PATH tclsh (`ensure_tclsh`'s answer) -- as autosetup's --with-tclsh and
    --with-tcl, or configure may bake another Tcl's -I dirs into the recipe, ahead of the staged
    headers. [] when PATH's own tclsh is the one used."""
    args = []
    if tclsh:
        args.append("--with-tclsh=" + tclsh)
    if tcl_config:
        args.append("--with-tcl=" + posixpath.dirname(tcl_config))
    return args


def zlib_keg(ctx):
    """Homebrew's zlib keg when it holds zlib.h AND a libz, else "" (no brew, or not installed)."""
    p = brew_prefix(ctx, "zlib")
    if p and os.path.isfile(_j(p, "include", "zlib.h")) and dir_holds_lib(_j(p, "lib"), "z"):
        return p
    return ""


def ensure_dev_headers(ctx):
    """The Tcl dev files (tclConfig.sh: configure detects Tcl through it) and zlib's -- a HOST
    fact which package manager provides them. dpkg answers on Debian/Ubuntu/WSL; without dpkg the
    installer is asked (and refuses by name where it cannot install)."""
    if ctx.cfg.host_os == "darwin":
        # macOS ships NO libz a program can OPEN (only the dyld shared cache and .tbd stubs), so
        # Homebrew's zlib is a hard prerequisite, not an optional extra.
        # ★ What is ALREADY there is not installed again -- a host that has it all installs
        # nothing, as dpkg makes it so below (✔MEASURED 2026-09-23: this branch asked for both on
        # every run). The Tcl dev files are there when the Tcl the run uses has its tclConfig.sh in
        # the inventory; Homebrew's zlib when its keg holds zlib.h and a libz.
        if not tcl_cfg_for(ctx.inventory, tclsh_version(ctx)):
            ctx.pkg_install("tcl", "tcl-tk")
        if not zlib_keg(ctx):
            ctx.pkg_install("zlib1g-dev", "zlib")
        return
    for pkg, brew in (("tcl-dev", "tcl-tk"), ("zlib1g-dev", "zlib")):
        if not (ctx.which("dpkg") and ctx.run(["dpkg", "-s", pkg], timeout=120).rc == 0):
            ctx.pkg_install(pkg, brew)


def probe_link_l(ctx, probe_cc, args):
    """True iff `<probe_cc> probe.c <args> -o probe.bin` LINKS (its exit status, directly)."""
    tmp = tempfile.mkdtemp(prefix="dss-probe-")
    try:
        src = _j(_abs(tmp), "probe.c")
        with open(src, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("int main(void){return 0;}\n")
        r = ctx.run(list(probe_cc) + [src] + list(args) + ["-o", _j(_abs(tmp), "probe.bin")], timeout=300)
        return r.rc == 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def pkgcfg_libdir(ctx, module):
    if not ctx.which("pkg-config"):
        return ""
    return ctx.run(["pkg-config", "--variable=libdir", module], timeout=60).out.strip()


def dir_holds_lib(d, name):
    """lib<name>.{dylib,so,so.*,tbd,a} in `d` (a dangling symlink does not count)."""
    if not d or not os.path.isdir(d):
        return False
    for f in ("lib%s.dylib", "lib%s.so", "lib%s.tbd", "lib%s.a"):
        if os.path.exists(_j(d, f % name)):
            return True
    return any(os.path.exists(p) for p in glob.glob(_j(glob.escape(d), "lib%s.so.*" % glob.escape(name))))


def ldconfig_dirs(ctx, name):
    """The dynamic linker's OWN answer (its cache), which is what makes a multiarch directory
    findable at all; /sbin is off a non-root PATH on Debian, hence the absolute candidates."""
    ldc = ""
    for c in ("ldconfig", "/sbin/ldconfig", "/usr/sbin/ldconfig"):
        ldc = ctx.which(c) or ""
        if ldc:
            break
    if not ldc:
        return []
    rx = re.compile(r".*=> (/.*)/lib%s\.so" % re.escape(name))
    dirs = set()
    for line in ctx.run([ldc, "-p"], timeout=60).out.splitlines():
        m = rx.match(line)
        if m:
            dirs.add(m.group(1))
    return sorted(dirs)


def compiler_search_dirs(ctx, probe_cc):
    """The compiler's own `-print-search-dirs` library list."""
    dirs = set()
    for line in ctx.run(list(probe_cc) + ["-print-search-dirs"], timeout=60).out.splitlines():
        if line.startswith("libraries:"):
            for d in line[len("libraries:"):].lstrip(" ").lstrip("=").split(":"):
                d = d.rstrip("/")
                if d:
                    dirs.add(d)
    return sorted(dirs)


def libdir_for(ctx, name, probe_cc):
    """A directory holding lib<name>.*, DERIVED, never hardcoded -- the union of both old chains:
    pkg-config's libdir (lib<name>, <name>) -> the Homebrew keg -> ldconfig's cache -> the
    compiler's search dirs -> the LIB roots. Each candidate is checked for the library itself."""
    for mod in ("lib" + name, name):
        d = pkgcfg_libdir(ctx, mod)
        if dir_holds_lib(d, name):
            return d
    for f in ("lib" + name, name):
        p = brew_prefix(ctx, f)
        if p and dir_holds_lib(_j(p, "lib"), name):
            return _j(p, "lib")
    for d in ldconfig_dirs(ctx, name) + compiler_search_dirs(ctx, probe_cc) + list(ctx.lib_roots):
        if dir_holds_lib(d, name):
            return d
    return ""


# ── git ────────────────────────────────────────────────────────────────────────────

def _git_env(env=None, **extra):
    # GIT_TERMINAL_PROMPT=0: with stdin=DEVNULL a credential prompt would otherwise HANG the run.
    e = dict(os.environ if env is None else env)
    e["GIT_TERMINAL_PROMPT"] = "0"
    e.update(extra)
    return e


def git_head_short(d, env=None):
    """short sha | UNKNOWN(<why>) -- never empty (a provenance field that silently prints nothing
    reads as fine). `.git` may be a FILE (worktree/submodule); without one, `git -C` would walk UP
    and name an enclosing repository's commit."""
    if not os.path.exists(_j(d, ".git")):
        return "UNKNOWN(no .git under %s)" % d
    r = C.capture(["git", "-C", d, "rev-parse", "--short", "HEAD"], env_=_git_env(env), timeout=120)
    sha = r.out.strip() if r.rc == 0 else ""
    return sha or "UNKNOWN(rev-parse HEAD failed in %s)" % d


def git_head_branch(d, env=None):
    """branch | DETACHED-HEAD | UNKNOWN."""
    r = C.capture(["git", "-C", d, "rev-parse", "--abbrev-ref", "HEAD"], env_=_git_env(env), timeout=120)
    b = r.out.strip() if r.rc == 0 else ""
    if not b:
        return "UNKNOWN"
    return "DETACHED-HEAD" if b == "HEAD" else b


def default_branch(d, env=None):
    """origin/HEAD, resolved locally (sqlite's default is master/trunk, never assumed to be main)."""
    r = C.capture(["git", "-C", d, "symbolic-ref", "--short", "refs/remotes/origin/HEAD"],
                  env_=_git_env(env), timeout=120)
    b = r.out.strip() if r.rc == 0 else ""
    return b[len("origin/"):] if b.startswith("origin/") else b


def remote_head_branch(d, env=None):
    """The fallback: `git remote show origin`'s "HEAD branch:" line -- under LC_ALL=C, because git
    translates that label under other locales and the line would silently not be found."""
    r = C.run_checked(["git", "-C", d, "remote", "show", "origin"],
                      "git remote show origin (resolving the default branch in %s)" % d,
                      env_=_git_env(env, LC_ALL="C", LANGUAGE="C"))
    for line in r.out.splitlines():
        m = re.match(r".*HEAD branch: (.*)$", line)
        if m:
            return m.group(1).strip()
    return ""


def _has_commit(dest, commit, genv):
    """Whether the checkout at `dest` already holds `commit` (a local object lookup; no network)."""
    r = C.capture(["git", "-C", dest, "cat-file", "-e", commit + "^{commit}"], env_=genv, timeout=120)
    return r.rc == 0


def _pinned_checkout(url, dest, commit, log, genv):
    """THE PINNED CHECKOUT (2026-09-25): `dest` on EXACTLY `commit`, DETACHED. The commit is fetched only
    when the clone does not hold it -- `fetch --all` first (the pin lies on upstream's history), then the
    sha itself -- and nothing is pulled, so a fetch can never move a pinned subject. A populated directory
    that is not a checkout cannot be put on a commit, so it is REFUSED (the unpinned path uses such a
    source tree as-is). -> [] (no warning: the subject is exactly the declared one)."""
    name = posixpath.basename(dest.rstrip("/"))
    if os.path.exists(_j(dest, ".git")):
        log.info("pinning %s in %s to %s" % (name, dest, commit[:12]))
    elif os.path.isdir(dest) and os.listdir(dest):
        C.die("%s is not a git checkout, and the sqlite subject is PINNED to %s (legs.json "
              "stageBuild.sqliteCommit): a source tree of unknown revision cannot be put on a commit.\n"
              "      Point the run at a clone, or remove this directory by hand once you have confirmed what "
              "it is." % (dest, commit))
    else:
        log.info("cloning %s -> %s" % (url, dest))
        os.makedirs(posixpath.dirname(dest.rstrip("/")) or "/", exist_ok=True)
        C.run_checked(["git", "clone", "--quiet", url, dest], "git clone %s" % url, env_=genv)
    if not _has_commit(dest, commit, genv):
        log.info("  the pinned %s is not in the clone: fetching" % commit[:12])
        C.run_checked(["git", "-C", dest, "fetch", "--all", "--prune", "--quiet"],
                      "git fetch (for the pinned %s, in %s)" % (commit[:12], dest), env_=genv)
        if not _has_commit(dest, commit, genv):
            C.run_checked(["git", "-C", dest, "fetch", "--quiet", "origin", commit],
                          "git fetch origin %s (the pin, in %s)" % (commit, dest), env_=genv)
    if not _has_commit(dest, commit, genv):
        C.die("the pinned sqlite commit %s is not in %s even after fetching %s: legs.json "
              "stageBuild.sqliteCommit names a revision its origin does not have." % (commit, dest, url))
    C.run_checked(["git", "-C", dest, "checkout", "--quiet", "--detach", commit],
                  "git checkout --detach %s (in %s)" % (commit[:12], dest), env_=genv)
    got = C.capture(["git", "-C", dest, "rev-parse", "HEAD"], env_=genv, timeout=120).out.strip()
    if got != commit:
        C.die("the pinned checkout of %s in %s landed on %s" % (commit, dest, got or "<nothing>"))
    log.info("  at %s (DETACHED, pinned by legs.json stageBuild.sqliteCommit)" % commit[:12])
    return []


def clone_or_update(url, dest, want="", log=None, env=None, commit=""):
    """A checkout, current -- the ONE implementation (the sqlite clone here; the driver's opt-in
    fresh DSS clone too). An existing checkout (`.git` a dir OR a file) is fetched, put on `want`
    or on origin's DEFAULT branch (the two old drivers shared the sqlite clone, so without the
    checkout one's corpus ran whatever branch the other left behind) and pulled; an absent or
    empty directory is cloned. Every git step is checked: a failed update continuing on a stale
    checkout while the report names a version the run never got is the silent-provenance defect
    this closes. A populated directory that is not a checkout is refused, unless it holds a
    `./configure` (a source tree, e.g. a tarball), which is used as-is with a warning.
    -> the warning reasons it logged ([] normally).
    `commit` (2026-09-25) PINS the checkout instead: the ONE sqlite revision the harness compiles
    (legs.json `stageBuild.sqliteCommit`), put on EXACTLY, detached (`_pinned_checkout`)."""
    log = log if log is not None else C.LOG
    genv = _git_env(env)
    if commit:
        return _pinned_checkout(url, dest, commit, log, genv)
    if os.path.exists(_j(dest, ".git")):
        log.info("updating %s in %s" % (posixpath.basename(dest.rstrip("/")), dest))
        C.run_checked(["git", "-C", dest, "fetch", "--all", "--prune", "--quiet"],
                      "git fetch (updating %s)" % dest, env_=genv)
        branch = want or default_branch(dest, env)
        if not branch:
            branch = remote_head_branch(dest, env)
        if not branch or branch == "(unknown)":
            C.die("could not resolve the default branch in %s (refs/remotes/origin/HEAD is unset and "
                  "`git remote show origin` names none)." % dest)
        C.run_checked(["git", "-C", dest, "checkout", "--quiet", branch],
                      "git checkout %s (in %s)" % (branch, dest), env_=genv)
        C.run_checked(["git", "-C", dest, "pull", "--rebase", "--quiet"],
                      "git pull --rebase (in %s)" % dest, env_=genv)
    elif os.path.isdir(dest) and os.listdir(dest):
        if os.path.isfile(_j(dest, "configure")):
            reason = "%s is not a git checkout -- its source tree is used as-is, not updated" % dest
            log.warn("%s is not a git checkout; using the source tree there AS-IS (it cannot be updated, "
                     "and its provenance is UNKNOWN)." % dest)
            return [reason]
        C.die("%s exists and is NOT a git checkout (it has no .git, and no ./configure either) — refusing "
              "to clone over it.\n      Point the run at another directory, or remove this one by hand once "
              "you have confirmed what it is." % dest)
    else:
        log.info("cloning %s -> %s" % (url, dest))
        os.makedirs(posixpath.dirname(dest.rstrip("/")) or "/", exist_ok=True)
        C.run_checked(["git", "clone", "--quiet", url, dest], "git clone %s" % url, env_=genv)
        if want:
            C.run_checked(["git", "-C", dest, "checkout", "--quiet", want],
                          "git checkout %s (in %s)" % (want, dest), env_=genv)
    log.info("  at %s on %s" % (git_head_short(dest, env), git_head_branch(dest, env)))
    return []


# ── the steps ──────────────────────────────────────────────────────────────────────

def prepare_stage_dir(out):
    """The stage, emptied (only now that the clone lock is held: a run refused at the lock still
    has its previous stage) and marked as this module's. It is wiped only when it is identifiably
    a stage: empty, or carrying this module's marker, a derive result, or the retired PowerShell
    driver's `testdir.win.txt`. Anything else is refused -- `--out` is an argument, and an argument
    must never be able to `rm -rf` a directory this harness did not make."""
    if os.path.lexists(out):
        if os.path.islink(out) or not os.path.isdir(out):
            C.die("the stage path %s exists and is not a directory this harness made — refusing to "
                  "replace it." % out)
        entries = os.listdir(out)
        if entries and not any(m in entries for m in (STAGE_MARKER, RESULT_FILE, OLD_STAGE_MARK)):
            C.die("refusing to wipe the stage directory %s: it is not empty and carries none of the marks "
                  "of a stage this harness made (%s, %s, or the old driver's %s).\n      Point --out at a "
                  "directory this harness owns, or remove it by hand once you have confirmed what it is."
                  % (out, STAGE_MARKER, RESULT_FILE, OLD_STAGE_MARK))
        try:
            shutil.rmtree(out)
        except OSError as exc:
            C.die("could not wipe the stage %s: %s" % (out, exc))
    os.makedirs(out)
    with open(_j(out, STAGE_MARKER), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("a stage written by sqlite_stage.py: wiped and re-staged by every derive\n")


def preserve_binary(src, dst):
    """Copy an oracle OUT of the make target's path (the target is deleted next, so make -n prints
    its recipe) -> (ok, why). Data first; mode and times best-effort (DrvFs cannot always honour
    them, and a copy that landed must not be reported missing); then the exec bit is ASSERTED --
    an oracle announced without one fails every use for a reason unrelated to either binary."""
    try:
        shutil.copyfile(src, dst)
    except OSError as exc:
        return False, "the copy failed: %s" % exc
    try:
        shutil.copystat(src, dst)
    except OSError:
        pass
    try:
        os.chmod(dst, os.stat(dst).st_mode | 0o111)
    except OSError:
        pass
    if not os.access(dst, os.X_OK):
        return False, ("it was COPIED to %s but is not executable there (chmod +x did not take on this "
                       "filesystem), so it cannot be run as an oracle" % dst)
    return True, ""


def run_configure(ctx, sqlite_dir, bld, args, log_path):
    """`(cd BLD && <sqlite>/configure <args>)`, its output kept in `log_path` (raw bytes)."""
    argv = [_j(sqlite_dir, "configure")] + list(args)
    rc = run_to_log(argv, bld, log_path, ctx.env)
    if rc != 0:
        C.die("sqlite's ./configure FAILED (rc=%d) in %s\n      argv: %s\n      log : %s\n%s"
              % (rc, bld, " ".join(argv), ctx.host(log_path), _indent(C.tail_file(log_path, 25))))


def check_tcl_headers(tcl_inc, ver, cfg_path, recipe_dirs, why):
    """R94-R96 -> the staged Tcl version (the header's own when no tclConfig.sh named one).
    `why` supplies the message context: roots, tclsh, tclsh_ver, bld."""
    if not (tcl_inc and os.path.isfile(_j(tcl_inc, "tcl.h"))):
        C.die("tcl.h not found — install the Tcl DEV files (apt: tcl-dev / tcl8.6-dev; brew: tcl-tk).\n"
              "      This is fatal for the ENTIRE run, not for one leg: every leg parses this same header.\n"
              "      roots searched: %s" % " ".join(why["roots"]))
    inc_ver = tcl_h_version(_j(tcl_inc, "tcl.h"))
    if ver and inc_ver and ver != inc_ver:
        C.die("Tcl staging is INCOHERENT: tclConfig.sh reports %s (%s) but %s/tcl.h reports %s.\n"
              "      A fixture built against one Tcl's headers and another's library links clean and then fails at\n"
              "      run time. Pin one with DSS_TCL_VERSION, or remove the stray installation."
              % (ver, cfg_path or "<none>", tcl_inc, inc_ver))
    ver = ver or inc_ver
    for d in recipe_dirs:
        if not d or not os.path.isfile(_j(d, "tcl.h")):
            continue
        rv = tcl_h_version(_j(d, "tcl.h"))
        if rv and rv != ver:
            C.die("RECIPE/STAGING Tcl MISMATCH — the fixture would compile against TWO Tcls.\n"
                  "      recipe -I dir : %s  (tcl.h reports %s)\n"
                  "      staged Tcl    : %s  (%s)\n"
                  "      The recipe dir comes FIRST in the Step-7 include list, so its headers would WIN over the\n"
                  "      staged ones while the fixture links %s's library.\n"
                  "      'configure' chose that dir; it follows the tclsh on PATH (%s, %s).\n"
                  "      Fix: run with DSS_TCL_VERSION=%s (stage what the recipe uses) or DSS_TCL_VERSION=%s\n"
                  "      (which also passes --with-tclsh/--with-tcl to configure so the recipe follows the pin), then\n"
                  "      delete %s so configure re-runs."
                  % (d, rv, ver, cfg_path or "<none>", ver, why["tclsh"] or "none",
                     why["tclsh_ver"] or "none", rv, ver, why["bld"]))
    return ver


def stage_zlib_headers(inc_roots, bld):
    """zlib.h + zconf.h -> `<bld>/zinc-src/`, VERBATIM (the deriving host's configured copy: each
    leg's own stage is written from it later, and no leg includes this one), and the stale
    top-level `<bld>/zinc/*.h` of the retired single-stage layout removed. -> (zinc_src, zh, zch)."""
    zinc_src, zinc_root = _j(bld, "zinc-src"), _j(bld, "zinc")
    os.makedirs(zinc_src, exist_ok=True)
    os.makedirs(zinc_root, exist_ok=True)
    for stale in sorted(glob.glob(_j(glob.escape(zinc_root), "*.h"))):
        if os.path.isfile(stale):
            os.remove(stale)
    roots = " ".join(inc_roots)
    zh = find_first(inc_roots, "zlib.h", maxdepth=3)
    if not zh:
        C.die("zlib.h not found — install zlib1g-dev (or 'brew install zlib').\n"
              "      This is fatal for the ENTIRE run, not for one leg: every leg parses this same header.\n"
              "      roots searched: %s" % roots)
    shutil.copyfile(zh, _j(zinc_src, "zlib.h"))
    zch = ""
    # zconf.h beside zlib.h first (they are a matched pair); the sweep is the fallback.
    for zc in [_j(posixpath.dirname(zh), "zconf.h")] + find_ordered(inc_roots, "zconf.h", maxdepth=3):
        if os.path.isfile(zc):
            shutil.copyfile(zc, _j(zinc_src, "zconf.h"))
            zch = zc
            break
    if not zch:
        C.die("zconf.h not found beside %s nor anywhere under %s.\n"
              "      zlib.h includes it and every ./configure guard lives in it; without it no leg's\n"
              "      zlib header can be configured for its target." % (zh, roots))
    return zinc_src, zh, zch


def _copy_data_and_mode(src, dst):
    """`cp` without -p: the data, and the mode best-effort (what DrvFs honours)."""
    shutil.copyfile(src, dst)
    try:
        shutil.copymode(src, dst)
    except OSError:
        pass


def _copy_tree(src, dst):
    """`cp -r src/. dst/` with an explicit stack; a symlink is followed (the Windows side must
    read real files), a directory reached twice through links is copied once; EVERY failure is
    collected and refused by name (the .ps1 tolerated test/ with `|| true`: a lost corpus file
    is silent coverage loss)."""
    if not os.path.isdir(src):
        C.die("staging: %s is not a directory -- the sqlite checkout is incomplete." % src)
    errors, seen = [], set()
    stack = [(src, dst)]
    while stack:
        s, d = stack.pop()
        real = os.path.realpath(s)
        if real in seen:
            continue
        seen.add(real)
        try:
            os.makedirs(d, exist_ok=True)
            with os.scandir(s) as it:
                names = sorted(e.name for e in it)
        except OSError as exc:
            errors.append("%s: %s" % (s, exc))
            continue
        for name in names:
            sp, dp = _j(s, name), _j(d, name)
            if os.path.isdir(sp):
                stack.append((sp, dp))
                continue
            try:
                _copy_data_and_mode(sp, dp)
            except OSError as exc:
                errors.append("%s: %s" % (sp, exc))
    if errors:
        C.die("staging could not copy %d entr%s from %s into %s -- the Windows side would build or run a "
              "tree with holes in it:\n%s" % (len(errors), "y" if len(errors) == 1 else "ies", src, dst,
                                              "\n".join("      " + e for e in errors[:20])))


def _copy_top(src, dst, pick):
    """The regular files directly in `src` whose name `pick` accepts (never a dotfile: a shell
    glob does not match one), copied into `dst`; a failure is refused by name."""
    for name in sorted(os.listdir(src)):
        sp = _j(src, name)
        if name.startswith(".") or not pick(name) or not os.path.isfile(sp):
            continue
        try:
            _copy_data_and_mode(sp, _j(dst, name))
        except OSError as exc:
            C.die("staging could not copy %s into %s: %s" % (sp, dst, exc))


def stage_tree(sqlite_dir, bld, tcl_inc, zinc_src, stage_dir):
    """The copies the PowerShell derive made, into the Windows-visible stage: src/ and ext/, the
    build dir's generated *.c + *.h, test/ as ext/'s SIBLING (sqlite globs `$testdir/../ext/...`),
    the chosen Tcl headers into tclinc/, zlib's pair into zinc-src/. -> the staged directories."""
    s = {"sqlite": _j(stage_dir, "sqlite")}
    s["bld"], s["src"] = _j(s["sqlite"], "bld"), _j(s["sqlite"], "src")
    s["ext"], s["test"] = _j(s["sqlite"], "ext"), _j(s["sqlite"], "test")
    s["tclinc"], s["zinc_src"] = _j(stage_dir, "tclinc"), _j(stage_dir, "zinc-src")
    for k in ("bld", "tclinc", "zinc_src", "test"):
        os.makedirs(s[k], exist_ok=True)
    _copy_tree(_j(sqlite_dir, "src"), s["src"])
    _copy_tree(_j(sqlite_dir, "ext"), s["ext"])
    _copy_top(bld, s["bld"], lambda n: n.endswith(".c") or n.endswith(".h"))
    _copy_tree(_j(sqlite_dir, "test"), s["test"])
    _copy_top(tcl_inc, s["tclinc"], lambda n: n.endswith(".h"))
    _copy_top(zinc_src, s["zinc_src"], lambda n: n in ("zlib.h", "zconf.h"))
    for d in TESTDIR_SIBLINGS:
        # asked in sqlite's own terms: `$testdir/../ext/<dir>`
        if not os.path.isdir(_j(s["test"], "..", "ext", d)):
            C.die("the staged test dir has no ../ext/%s, so sqlite's permutations.test would glob it to "
                  "NOTHING and drop that whole family from the corpus without any error -- 'testdir' and "
                  "'ext' must be siblings" % d)
    for h in ("zlib.h", "zconf.h"):
        if not os.path.isfile(_j(s["zinc_src"], h)):
            C.die("the derivation did not stage %s into %s — every leg parses this one header, so there is "
                  "no leg to build on any host." % (h, s["zinc_src"]))
    if not os.path.isfile(_j(s["bld"], "sqlite_cfg.h")):
        C.die("the derivation did not stage sqlite_cfg.h into %s -- sqlite's ./configure writes it and the "
              "recipe's _HAVE_SQLITE_CONFIG_H makes every TU include it, so each leg's own copy is "
              "rewritten from it. Without it there is no leg to build on any host." % s["bld"])
    return s


def stage_recipe_lists(ctx, label, tus_file, incs_file, pairs, staged_tus, staged_incs):
    """Remap one recipe's TU and -I lists into the stage, spelled for the host. EVERY TU must
    survive (a remap that silently drops one yields a smaller program that still compiles and
    links); a -I dir whose staged location does not exist is dropped (the .ps1's `[ -d ]`), and
    one that exists here but not there is said out loud. -> (n_tus, n_includes)."""
    tus = read_list(tus_file)
    lines, lost, passed = remap_list(tus, pairs, want_dir=False, translate=ctx.host)
    if lost or len(lines) != len(tus):
        C.die("staging lost %s TUs: derived %d, staged %d -- no staged file for:\n%s"
              % (label, len(tus), len(lines), "\n".join("      " + p for p in lost[:20])))
    for p in passed:
        ctx.warn("staging: the %s TU %s lies under no staged root; the host will read it IN PLACE as %s"
                 % (label, p, ctx.host(p)))
    write_list(staged_tus, lines)
    incs = read_list(incs_file)
    ilines, dropped, ipassed = remap_list(incs, pairs, want_dir=True, translate=ctx.host)
    for p in dropped:
        if os.path.isdir(p):
            ctx.warn("staging: the %s -I dir %s exists here but not in the stage -- dropped from the staged "
                     "include list" % (label, p))
    for p in ipassed:
        if p not in dropped:
            ctx.warn("staging: the %s -I dir %s lies under no staged root; the host will read it IN PLACE "
                     "as %s" % (label, p, ctx.host(p)))
    write_list(staged_incs, ilines)
    return len(lines), len(ilines)


def _coherence_gate(log, dirs, checkout, label):
    """R70/R71 (and the PowerShell driver's S34-24/25): every artifact carrying a SQLITE_SOURCE_ID
    agrees, and agrees with the checkout. Run-wide and unskippable: an incoherent stage invalidates
    the ONE input all five legs share."""
    coh = _sibling("sqlite_coherence", "the staged-source coherence gate")
    stream = getattr(log, "stream", sys.stdout)
    rc = coh.run_check(list(dirs), checkout=checkout, require_cli=True, label=label, out=stream, err=stream)
    if rc == 0:
        log.ok("staged sqlite sources coherent — %s" % label)
        return
    if rc == 1:
        C.die("staged sqlite tree is INCOHERENT (mixed vintage) — refusing to build.\n"
              "      label : %s\n      dirs  : %s\n"
              "      Every artifact carrying a SQLITE_SOURCE_ID must agree, and agree with the checkout; the report\n"
              "      above names the divergent files and both ids. Re-staging will NOT fix it: the orphans live in\n"
              "      the build dir (sqlite3.c / shell.c / tclsqlite3.c / tsrc/ are prerequisites of no target the\n"
              "      harness asks make for). Regenerate them there from the current source state:\n"
              "          make sqlite3.c shell.c tclsqlite3.c" % (label, "  ".join(dirs)))
    C.die("the staged-source coherence gate could not run (rc=%d: usage or I/O) — label %s, dirs %s. There is "
          "no skip path for it by design." % (rc, label, "  ".join(dirs)))


def stage(cfg, log=None, lock=None):
    """The whole POSIX half, in the .sh's order (see the module docstring). `lock`: a clone lock the
    caller ALREADY HOLDS for WRITE (the POSIX driver, which downgrades it for the corpus) -- used,
    never taken or released here; with None, the WRITE lock is taken and released around the run.
    -> StageResult, every host path spelled by `cfg.translate_for_host`."""
    log = log if log is not None else C.LOG
    if not isinstance(cfg, StageConfig):
        C.die("INTERNAL: stage() takes a StageConfig")
    own = lock is None
    if own:
        procs = _sibling("sqlite_procs", "the shared-clone lock")
        lock = procs.CloneLock(cfg.sqlite_dir)
        lock.write(cfg.lock_what, log)
        log.info("clone lock: WRITE on %s" % cfg.sqlite_dir)
    try:
        return _stage_locked(cfg, log, lock)
    finally:
        if own:
            lock.release()


def stage_and_persist(cfg, log=None, lock=None, stage_fn=None):
    """`stage()` with its result PERSISTED: the ONE writer of `<cfg.out_dir>/derive-result.json`, for the WSL
    derive (a Windows host) and a POSIX host's driver alike, so a later mode that REUSES the stage (the
    round-close recompile) finds it on every host. A result an earlier run left is removed FIRST: a stage
    that fails part-way must leave none to be read as this one's. `stage_fn` is the self-test's stand-in."""
    path = _j(cfg.out_dir, RESULT_FILE)
    _rm_f(path)
    result = (stage_fn or stage)(cfg, log, lock=lock)
    write_text_atomic(path, result.to_json())
    return result


def _stage_locked(cfg, log, lock):
    ctx = _Ctx(cfg, log)
    clone, out = cfg.sqlite_dir, cfg.out_dir
    mo = cfg.make_options
    if cfg.copy_to_stage:
        prepare_stage_dir(out)
    else:
        os.makedirs(out, exist_ok=True)

    # ── Step 3: the clone, on the PINNED commit; the tier exists ─────────────────────
    ctx.warnings.extend(clone_or_update(cfg.sqlite_repo_url, clone, log=log, env=ctx.env,
                                        commit=cfg.sqlite_commit))
    configure = _j(clone, "configure")
    if not (os.path.isfile(configure) and os.access(configure, os.X_OK)):
        C.die("no ./configure in %s — not a SQLite checkout" % clone)
    if not (os.path.isfile(_j(clone, "test", cfg.tier + ".test")) or cfg.test_file):
        C.die("tier '%s' has no %s (expected veryquick|quick|full|all)."
              % (cfg.tier, _j(clone, "test", cfg.tier + ".test")))
    log.ok("sqlite ready")

    # ── Step 4: the interpreter and the dev files (one Tcl inventory, shared with Step 6) ──
    discover_roots(ctx)
    ctx.inventory = tcl_inventory(ctx)
    pin_sh, pin_cfg = ensure_tclsh(ctx)
    ensure_dev_headers(ctx)
    bld = _j(clone, "bld-dss")
    os.makedirs(bld, exist_ok=True)
    configure_args = tcl_configure_args(pin_sh, pin_cfg)
    if cfg.tcl_version or configure_args:
        log.info("configure: %s sqlite's Tcl detection — %s"
                 % ("pinning" if cfg.tcl_version else "carrying the chosen Tcl into",
                    " ".join(configure_args) or "<none resolvable>"))
    configure_args += cfg.configure_flags
    log.info("configure: stage capabilities — %s%s" % (
        " ".join(cfg.configure_flags), ("   make OPTIONS=" + mo) if mo else ""))

    # ── the build configuration behind BLD: stamp, and wipe only what is provably ours ──
    stamp_now = stage_stamp_text(tclsh_version(ctx), configure_args, mo)
    decision, was = stamp_decision(bld, clone, stamp_now)
    if decision == "refuse":
        C.die("the build configuration behind %s changed, and that build directory could not be\n"
              "      identified as this driver's own — refusing to delete it.\n"
              "      was: %s\n      now: %s\n"
              "      Expected $SQLITE_DIR/bld-dss carrying a Makefile or a previous stamp. Remove it by hand\n"
              "      once you have confirmed what it is:  rm -rf '%s'   then re-run."
              % (bld, was or "<no stamp: provenance unknown>", stamp_now, bld))
    if decision == "wipe":
        ctx.warn("the build configuration behind %s changed — REBUILDING IT FROM SCRATCH.\n"
                 "      was: %s\n      now: %s\n"
                 "      Re-running configure rewrites the Makefile but not the .o timestamps, so make would\n"
                 "      SKIP every object and link a fixture from a MIXTURE of the two configurations — and\n"
                 "      parse.c / keywordhash.h are generated with OPT_FEATURE_FLAGS too, so the mixture\n"
                 "      would reach the parser itself. Wiping is the only honest way to change it."
                 % (bld, was or "<no stamp: this tree predates the configuration stamp>", stamp_now),
                 "build dir %s rebuilt from scratch (its configuration stamp %s)"
                 % (bld, "changed" if was else "was absent"))
        shutil.rmtree(bld)
        os.makedirs(bld)
    if not configure_args:
        C.die("INTERNAL: CONFIGURE_ARGS is empty at the configure step.\n"
              "      It must always carry legs.json's declared stageBuild.configureFlags by this point. Configuring\n"
              "      without them would build a sqlite with the extensions OFF and the corpus would then report\n"
              "      every one of their test files as 'completed' having asserted nothing.")
    configure_log = _j(out, "configure.log")
    run_configure(ctx, clone, bld, configure_args, configure_log)
    with open(_j(bld, STAGE_STAMP), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(stamp_now + "\n")
    _rm_f(_j(bld, STAGE_STAMP_LEGACY))

    # ── R65: did the capabilities take? (asked of the Makefile, before anything compiles) ──
    makefile = _j(bld, "Makefile")
    optflags = mk_var(makefile, "OPT_FEATURE_FLAGS")
    missing = missing_required_defines(optflags, cfg.required_defines, mo)
    if missing:
        C.die("configure accepted its flags and did NOT produce the defines they exist for.\n"
              "      missing from OPT_FEATURE_FLAGS:%s\n"
              "      flags passed: %s\n"
              "      OPT_FEATURE_FLAGS: %s\n"
              "      A flag can be accepted and do nothing (measured: --memsys3), and upstream can rename or\n"
              "      retire one at any pull. Without this stop the run would build a library WITHOUT these\n"
              "      capabilities and then report every one of their test files as 'completed' — having asserted\n"
              "      nothing at all." % ("".join(" " + d for d in missing), " ".join(configure_args) or "<none>",
                                          optflags or "<empty>"))
    log.info("configure: all %s accounted for" % ", ".join(cfg.required_defines))

    # ── R66/R67: the -L TCL_LIBS needs but does not carry; probes use the Makefile's OWN CC ──
    probe_cc = mk_var(makefile, "CC").split() or (ctx.env.get("CC") or "cc").split()
    tcl_cfg_sh = mk_var(makefile, "TCL_CONFIG_SH")
    ref_ldflags = ""
    if tcl_cfg_sh and os.path.isfile(tcl_cfg_sh):
        tcl_libs = tcl_config_values(ctx, tcl_cfg_sh, ("TCL_LIBS",))["TCL_LIBS"]
        ref_ldflags, notes, warns = tcl_libs_ldflags(
            tcl_libs, lambda a: probe_link_l(ctx, probe_cc, a),
            lambda n: libdir_for(ctx, n, probe_cc), " ".join(probe_cc), len(ctx.lib_roots))
        for n in notes:
            log.info("reference link: " + n)
            ctx.ref_link_notes.append(n)
        for w in warns:
            log.warn("reference link: " + w)
            ctx.ref_link_warnings.append(w)
    configure_ldflags_log = _j(out, "configure-ldflags.log")
    if ref_ldflags:
        log.info("configure: reference link needs %s — re-running configure with LDFLAGS" % ref_ldflags)
        ctx.ref_link_notes.append("re-running configure with LDFLAGS=%s to supply the missing search path"
                                  % ref_ldflags)
        configure_args.append("LDFLAGS=" + ref_ldflags)
        run_configure(ctx, clone, bld, configure_args, configure_ldflags_log)
        try:
            with open(makefile, "rb") as fh:
                landed = ref_ldflags.encode("utf-8") in fh.read()
        except OSError:
            landed = False
        if landed:
            log.info("configure: LDFLAGS.configure now carries %s" % ref_ldflags)
            ctx.ref_link_notes.append("LDFLAGS.configure now carries %s" % ref_ldflags)
        else:
            w = ("configure did NOT carry LDFLAGS='%s' into %s -- the reference testfixture will almost "
                 "certainly fail to link; sqlite's LDFLAGS.configure / @LDFLAGS@ substitution may have "
                 "changed shape upstream." % (ref_ldflags, makefile))
            log.warn("reference link: " + w)
            ctx.ref_link_warnings.append(w)

    # ── R68: regenerate the amalgamation orphans BEFORE both reference builds (tolerated) ──
    amalg_log = _j(out, "amalgamation-regen.log")
    log.info("regenerating the amalgamation orphans (sqlite3.c / shell.c / tclsqlite3.c) so the stage is ONE vintage")
    rc = run_to_log(["make", "sqlite3.c", "shell.c", "tclsqlite3.c", "OPTIONS=" + mo], bld, amalg_log, ctx.env)
    if rc == 0:
        amalgamation_regen = "ok"
        log.info("      amalgamation regenerated (log: %s)" % ctx.host(amalg_log))
    else:
        amalgamation_regen = ("FAILED (rc=%d; tolerated here -- the coherence gate renders the verdict). "
                              "Log: %s" % (rc, ctx.host(amalg_log)))
        ctx.warn("regenerating the amalgamation orphans FAILED (tolerated here — the coherence gate\n"
                 "      at the end of this step is what renders the verdict). Log: %s" % ctx.host(amalg_log),
                 "amalgamation regeneration " + amalgamation_regen)

    # ── R69: the reference testfixture, the ATTRIBUTION ORACLE, preserved (tolerated) ──
    ref_fixture_keep = _j(out, "reference-testfixture")
    ref_build_log = _j(out, "reference-build.log")
    _rm_f(ref_fixture_keep)          # a stale oracle attributes against sources no longer under test
    log.info("building the reference testfixture (generates derived sources + libsqlite3.a)")
    rc = run_to_log(["make", "-s", "testfixture", "USE_AMALGAMATION=0", "OPTIONS=" + mo, "-j%d" % cfg.jobs],
                    bld, ref_build_log, ctx.env)
    ref_fixture, ref_fixture_why = None, ""
    if rc == 0:
        ok, why = preserve_binary(_j(bld, "testfixture"), ref_fixture_keep)
        if ok:
            ref_fixture = ref_fixture_keep
            log.info("reference gcc testfixture built + preserved -> %s  (usable as an ATTRIBUTION ORACLE)"
                     % ctx.host(ref_fixture_keep))
        else:
            ref_fixture_why = ("reference testfixture LINKED but could NOT be preserved to %s -- it is about "
                               "to be deleted to expose the recipe, so no oracle survives this run: %s"
                               % (ctx.host(ref_fixture_keep), why))
            ctx.warn("reference testfixture LINKED but could NOT be preserved to %s —\n"
                     "      it is about to be deleted to expose the recipe, so no oracle will survive this\n"
                     "      run (%s). Check permissions / free space on %s." % (ctx.host(ref_fixture_keep), why,
                                                                               ctx.host(out)),
                     ref_fixture_why)
    else:
        ref_fixture_why = ("reference gcc testfixture did not fully link (rc=%d; tolerated -- byproducts + "
                           "recipe still harvested). Log KEPT at %s -- READ IT.%s"
                           % (rc, ctx.host(ref_build_log),
                              (" NOTE: link-path repair WAS in effect (LDFLAGS=%s), so this is a DIFFERENT "
                               "miss." % ref_ldflags) if ref_ldflags else ""))
        ctx.warn("reference gcc testfixture did not fully link (tolerated — harvesting generated sources + recipe)\n"
                 "      log kept: %s — READ IT. A working reference is what lets a corpus\n"
                 "      failure be ATTRIBUTED instead of argued about.%s"
                 % (ctx.host(ref_build_log),
                    ("\n      link-path repair WAS in effect (LDFLAGS=%s) — so this is a DIFFERENT miss."
                     % ref_ldflags) if ref_ldflags else ""),
                 ref_fixture_why)

    # ── R70/R71: the staged-tree coherence gate, WITH --require-cli ───────────────────
    _coherence_gate(log, [bld], clone, "staged sqlite (Step 4)")

    # ── R72: the reference gcc sqlite3 CLI (upstream's full-source sqlite3d), preserved ──
    ref_cli_keep = _j(out, "reference-sqlite3")
    ref_cli_log = _j(out, "reference-cli-build.log")
    _rm_f(ref_cli_keep)
    log.info("building the reference gcc sqlite3 CLI (upstream's own 'sqlite3d' target)")
    rc = run_to_log(["make", "-s", "sqlite3d", "OPTIONS=" + mo, "-j%d" % cfg.jobs], bld, ref_cli_log, ctx.env)
    built = _j(bld, "sqlite3d")
    ref_cli, ref_cli_why = None, ""
    if rc == 0 and os.path.isfile(built) and os.access(built, os.X_OK):
        ok, why = preserve_binary(built, ref_cli_keep)
        if ok:
            ref_cli = ref_cli_keep
            log.info("reference gcc sqlite3 CLI built + preserved -> %s  (the CLI ATTRIBUTION ORACLE)"
                     % ctx.host(ref_cli_keep))
        else:
            ref_cli_why = ("the reference sqlite3 CLI LINKED but could not be preserved to %s -- it is about to "
                           "be deleted to expose its recipe, so no CLI oracle survives this run: %s"
                           % (ctx.host(ref_cli_keep), why))
            ctx.warn("the reference sqlite3 CLI LINKED but could NOT be preserved to %s — it is\n"
                     "      about to be deleted to expose its recipe, so no CLI oracle survives this run (%s)."
                     % (ctx.host(ref_cli_keep), why), ref_cli_why)
    else:
        ref_cli_why = ("the reference gcc sqlite3 CLI did not build (rc=%d; tolerated -- the CLI legs still "
                       "build). Log KEPT at %s -- READ IT. Without it NO smoke failure can be EXONERATED."
                       % (rc, ctx.host(ref_cli_log)))
        ctx.warn("the reference gcc sqlite3 CLI did not build (tolerated — the CLI legs still build).\n"
                 "      Log KEPT at %s — READ IT. Without it NO smoke failure on any leg can be\n"
                 "      EXONERATED, and cli-smoke.py charges an unattributable failure to DSS by design."
                 % ctx.host(ref_cli_log), ref_cli_why)
    _rm_f(built)

    # ── the recipes: `make -n` prints a recipe only for a MISSING target -- this rm is LOAD-BEARING
    # (the oracle was copied out above precisely so the target can be deleted here) ──
    _rm_f(_j(bld, "testfixture"))
    ar = _j(bld, ".libs", "libsqlite3.a")
    if not os.path.isfile(ar):
        ar = _j(bld, "libsqlite3.a")
    base = _sibling("sqlite_base", "the shared recipe core")
    make_argv = ("make",)
    if ctx.env.get("PATH") != os.environ.get("PATH"):
        # emit_recipe takes no env: hand make this run's PATH (the pin) the way the .sh's export did.
        make_argv = ("env", "PATH=" + ctx.env.get("PATH", ""), "make")
    roots = (_j(clone, "src"), _j(clone, "ext"), bld)
    f_recipe, f_tus, f_defs, f_incs = (_j(out, n) for n in (
        "testfixture-recipe.txt", "tus.base.txt", "defines.base.txt", "recipe-includes.base.txt"))
    try:
        f_res = base.emit_recipe(
            build_dir=bld, make_target="testfixture", recipe_file=f_recipe,
            out_tus=f_tus, out_defines=f_defs, out_includes=f_incs,
            make_vars=("USE_AMALGAMATION=0", "OPTIONS=" + mo), search_roots=roots,
            prereq_mode="link-line", always_make=True, token_scope="recipe",
            archive=ar, archive_from_span=False,
            min_tus=FIXTURE_MIN_TUS, min_defines=FIXTURE_MIN_DEFINES, make_argv=make_argv)
    except (base.RecipeRefused, base.HarnessUsageError) as exc:
        C.die("the testfixture recipe derivation FAILED — see %s and the diagnostic above.\n"
              "      A short parse does not error on its own: it yields a smaller TU set that compiles,\n"
              "      links, and fails much later looking like a codegen bug. That is what the floors and\n"
              "      the drop ledger turn into this stop.\n      refusal: %s" % (ctx.host(f_recipe), exc))
    log.ok("recipe: %s" % f_res.summary)
    c_recipe, c_tus, c_defs, c_incs = (_j(out, n) for n in (
        "sqlite3-cli-recipe.txt", "cli-tus.base.txt", "cli-defines.base.txt", "cli-includes.base.txt"))
    try:
        c_res = base.emit_recipe(
            build_dir=bld, make_target="sqlite3d", recipe_file=c_recipe,
            out_tus=c_tus, out_defines=c_defs, out_includes=c_incs,
            make_vars=("OPTIONS=" + mo,), search_roots=roots,
            prereq_mode="link-line", always_make=True, token_scope="recipe",
            archive=ar, archive_from_span=True,
            min_tus=CLI_MIN_TUS, min_defines=CLI_MIN_DEFINES, make_argv=make_argv)
    except (base.RecipeRefused, base.HarnessUsageError) as exc:
        C.die("the sqlite3 CLI recipe derivation FAILED — see %s and the diagnostic above.\n"
              "      This is NOT skippable: a CLI leg that silently does not build is exactly the\n"
              "      'a capability in one driver and not the other' failure this work closed.\n"
              "      refusal: %s" % (ctx.host(c_recipe), exc))
    log.ok("cli recipe: %s" % c_res.summary)

    # ── R75-R77: asserted BY NAME, because a count cannot say which file or define went missing ──
    fix_tus, fix_defs, fix_incs = read_list(f_tus), read_list(f_defs), read_list(f_incs)
    cli_tus, cli_defs, cli_incs = read_list(c_tus), read_list(c_defs), read_list(c_incs)
    if not any(re.search(r"/shell\.c$", t) for t in cli_tus):
        C.die("the CLI TU set has no shell.c — it is the CLI's only entry point (sqlite3.c has no main).\n"
              "      Derived from: %s" % ctx.host(c_recipe))
    if "SQLITE_CORE" not in cli_defs:
        C.die("the CLI define set has no SQLITE_CORE (%d defines derived).\n"
              "      Without it ext/icu/icu.c stops compiling to nothing and demands <unicode/*.h>,\n"
              "      which fails as 'error[F001A] got unicode/utypes.h' — a derivation bug wearing a\n"
              "      missing-dependency costume. It is contributed by the library COMPILE lines, so\n"
              "      this means the -D tokens were read from the link line alone: check that\n"
              "      always_make and token_scope='recipe' survived. Derived from: %s"
              % (len(cli_defs), ctx.host(c_recipe)))
    assert_recipe_capabilities("testfixture", ctx.host(f_recipe), fix_defs, cfg.required_defines, mo)
    assert_recipe_capabilities("sqlite3 CLI", ctx.host(c_recipe), cli_defs, cfg.required_defines, mo)
    log.ok("capabilities: both recipes carry all %s" % ", ".join(cfg.required_defines))

    # ── Step 6's header half: ONE Tcl, from the SAME inventory Step 4 used -- re-taken when Step 4
    # attempted an install, since that is what can have changed it (the .sh always re-took it;
    # the walk costs seconds where /usr/lib holds a Windows mount, as inside WSL) ──
    if ctx.install_attempted:
        ctx.inventory = tcl_inventory(ctx)
    tclsh_ver = tclsh_version(ctx)
    tcl_ver, tcl_cfg, how = select_tcl(ctx.inventory, cfg.tcl_version, tclsh_ver)
    if how == "pin-missing":
        C.die("DSS_TCL_VERSION=%s is pinned, but no Tcl %s is installed.\n"
              "      tclConfig.sh found: %s\n"
              "      roots searched   : %s\n"
              "      Install it (apt: tcl%s-dev — brew: 'tcl-tk' for 9.x, 'tcl-tk@8' for 8.6; both are\n"
              "      KEG-ONLY, which is why their own prefixes are searched), or unset DSS_TCL_VERSION to take\n"
              "      whatever Tcl this host has."
              % (tcl_ver, tcl_ver, ";".join("%s %s" % e for e in ctx.inventory) or "<none>",
                 " ".join(ctx.cfg_roots), tcl_ver))
    if how == "pinned":
        log.info("tcl: PINNED to %s by DSS_TCL_VERSION" % tcl_ver)
    elif how == "highest":
        ctx.warn("no tclConfig.sh matches the tclsh on PATH (%s) — falling back to the highest installed Tcl (%s)."
                 % (tclsh_ver or "none", tcl_ver))
    if tcl_ver and tclsh_ver and tcl_ver != tclsh_ver:
        if cfg.tcl_version:
            C.die("PINNED Tcl skew after Step 4: staging %s but tclsh on PATH reports %s\n"
                  "      (pin=%s, interpreter=%s). ensure_tclsh guarantees these agree,\n"
                  "      so this is a harness regression, not a host problem. Refusing to build against two Tcls."
                  % (tcl_ver, tclsh_ver, cfg.tcl_version, ctx.which("tclsh")))
        ctx.warn("tcl SKEW: staging headers+library for %s while tclsh on PATH is %s — the sources\n"
                 "      Step 4 generated came from the latter. Set DSS_TCL_VERSION=%s to pin BOTH end-to-end."
                 % (tcl_ver, tclsh_ver, tcl_ver))
    vals = tcl_config_values(ctx, tcl_cfg, ("TCL_INCLUDE_SPEC", "TCL_LIB_FILE"))
    tcl_inc = ""
    spec = vals["TCL_INCLUDE_SPEC"]
    if spec:
        tcl_inc = spec[2:] if spec.startswith("-I") else spec
    if not (tcl_inc and os.path.isfile(_j(tcl_inc, "tcl.h"))):
        hit = find_first(ctx.inc_roots, "tcl.h", path_glob="*tcl%s*" % (tcl_ver or "[0-9]"))
        tcl_inc = posixpath.dirname(hit) if hit else ""
    if not (tcl_inc and os.path.isfile(_j(tcl_inc, "tcl.h"))):
        hit = find_first(ctx.inc_roots, "tcl.h")
        tcl_inc = posixpath.dirname(hit) if hit else ""
    tcl_ver = check_tcl_headers(tcl_inc, tcl_ver, tcl_cfg, fix_incs, {
        "roots": ctx.inc_roots, "tclsh": ctx.which("tclsh"), "tclsh_ver": tclsh_ver, "bld": bld})
    zinc_src, zh, _zch = stage_zlib_headers(ctx.inc_roots, bld)
    sqlite_cfg_h = _j(bld, "sqlite_cfg.h")
    if not os.path.isfile(sqlite_cfg_h):
        C.die("the generated sqlite_cfg.h is not in %s.\n"
              "      sqlite's ./configure writes it there and the recipe's _HAVE_SQLITE_CONFIG_H makes every TU\n"
              "      include it; each leg is staged its OWN copy from it (build.configureAnswers), so its absence\n"
              "      is fatal for the ENTIRE run rather than for one leg." % bld)
    log.info("tcl %s headers: %s   zlib source headers: %s (from %s)" % (tcl_ver, tcl_inc, zinc_src, zh))

    # ── the result, POSIX spelling; staged + host-spelled below when copying ─────────
    fixture = dict(tus=f_tus, defines=f_defs, includes=f_incs, recipe=f_recipe,
                   summary=f_res.summary, n_tus=len(fix_tus), n_defines=len(fix_defs),
                   n_includes=len(fix_incs))
    cli = dict(tus=c_tus, defines=c_defs, includes=c_incs, recipe=c_recipe,
               summary=c_res.summary, n_tus=len(cli_tus), n_defines=len(cli_defs),
               n_includes=len(cli_incs))
    where = dict(bld=bld, src=_j(clone, "src"), ext=_j(clone, "ext"), testdir=_j(clone, "test"),
                 tcl_inc=tcl_inc, zinc_src=zinc_src, sqlite_cfg_h=sqlite_cfg_h)
    if cfg.copy_to_stage:
        log.info("staging sources + headers into %s" % ctx.host(out))
        s = stage_tree(clone, bld, tcl_inc, zinc_src, out)
        pairs = stage_pairs(clone, bld, tcl_inc, out)
        n, ni = stage_recipe_lists(ctx, "fixture", f_tus, f_incs, pairs,
                                   _j(out, "tus.staged.txt"), _j(out, "recipe-includes.staged.txt"))
        fixture.update(tus=_j(out, "tus.staged.txt"), includes=_j(out, "recipe-includes.staged.txt"),
                       n_tus=n, n_includes=ni)
        n, ni = stage_recipe_lists(ctx, "CLI", c_tus, c_incs, pairs,
                                   _j(out, "cli-tus.staged.txt"), _j(out, "cli-includes.staged.txt"))
        cli.update(tus=_j(out, "cli-tus.staged.txt"), includes=_j(out, "cli-includes.staged.txt"),
                   n_tus=n, n_includes=ni)
        where = dict(bld=s["bld"], src=s["src"], ext=s["ext"], testdir=s["test"], tcl_inc=s["tclinc"],
                     zinc_src=s["zinc_src"], sqlite_cfg_h=_j(s["bld"], "sqlite_cfg.h"))
        # What the Windows side compiles is the COPY: gate it too (the .ps1 gated only the copy).
        _coherence_gate(log, [s["bld"], s["src"]], clone, "staged sqlite (Step 4, staged copy)")
        log.ok("staged: %d fixture TUs, %d CLI TUs (each equal to its derived count)"
               % (fixture["n_tus"], cli["n_tus"]))

    result = StageResult(
        out_dir=out, sqlite_dir=clone, tier_file=_j(where["testdir"], cfg.tier + ".test"),
        reference_build_log=ref_build_log, reference_cli_log=ref_cli_log,
        amalgamation_log=amalg_log, configure_log=configure_log,
        reference_fixture=ref_fixture, reference_cli=ref_cli,
        out_dir_posix=out, sqlite_dir_posix=clone, bld_posix=bld,
        tclsh_posix=ctx.which("tclsh") or "", tcl_config_posix=tcl_cfg or "",
        reference_fixture_posix=ref_fixture, reference_cli_posix=ref_cli,
        fixture_recipe=RecipeFiles(**fixture), cli_recipe=RecipeFiles(**cli),
        tier=cfg.tier, test_file=cfg.test_file, tcl_version=tcl_ver, tcl_lib_file=vals["TCL_LIB_FILE"],
        reference_fixture_why=ref_fixture_why, reference_cli_why=ref_cli_why,
        amalgamation_regen=amalgamation_regen,
        sqlite_head=git_head_short(clone, ctx.env), sqlite_branch=git_head_branch(clone, ctx.env),
        stage_identity=stamp_now, make_options=mo,
        configure_args=configure_args, required_defines=list(cfg.required_defines),
        clone_lock_notes=[str(n) for n in (getattr(lock, "notes", None) or [])],
        ref_link_notes=ctx.ref_link_notes, ref_link_warnings=ctx.ref_link_warnings,
        warnings=ctx.warnings, copy_to_stage=cfg.copy_to_stage, witnesses=dict(cfg.witnesses),
        **where)
    return result.translated(cfg.translate_for_host)


# ── the derive CLI (a Windows host's WSL hop) ──────────────────────────────────────

class _UsageError(Exception):
    pass


class _Parser(argparse.ArgumentParser):
    def error(self, message):
        raise _UsageError(message)


def _report(err, exc):
    """A refusal on stderr; a blocked clone lock's FIRST line is exactly DSS-CLONE-LOCK-BLOCKED
    (a contract the Windows driver reads), whatever the lock's own message starts with."""
    text = str(exc).strip("\n")
    if getattr(exc, "exit_code", 1) == 3:
        lines = text.split("\n")
        if lines and lines[0].strip() == CLONE_LOCK_BLOCKED:
            lines = lines[1:]
        rest = "\n".join(lines).strip("\n")
        err.write(CLONE_LOCK_BLOCKED + "\n")
        # the lock's own message carries its ` [X] ERROR:` framing; a bare one gets this module's
        err.write((rest if "ERROR:" in rest else "✗ ERROR: " + rest) + "\n")
    else:
        err.write("✗ ERROR: %s\n" % text)
    err.flush()


def _default_lock_factory(clone):
    return _sibling("sqlite_procs", "the shared-clone lock").CloneLock(clone)


_VAR_RE = re.compile(r"\$(\w+)|\$\{(\w+)\}")


def _expand(path, env):
    """What bash did to SQLITE_WSL_DIR inside the old derive's double quotes, from THIS side's
    environment: `$VAR` / `${VAR}` (an unknown one is left as written), then a leading `~`. The
    default must be the POSIX side's own home, never the Windows one (report 08 E.18)."""
    def sub(m):
        name = m.group(1) or m.group(2)
        return env[name] if name in env else m.group(0)
    p = _VAR_RE.sub(sub, path)
    if p == "~" or p.startswith("~/"):
        p = (env.get("HOME") or os.path.expanduser("~")) + p[1:]
    return p


def derive_main(args, which=None, lock_factory=None, translator=None, stage_fn=None, out=None,
                err=None, environ=None, host_os=None):
    """`derive --out <stage> [--sqlite-dir D] [--sqlite-repo-url U] [--tcl-version V] [--jobs N]
    --stage-build-json F --tier T [--test-file F]` -> exit code (0; 1 refusal; 2 usage; 3 a blocked
    clone lock, stderr line 1 `DSS-CLONE-LOCK-BLOCKED`). Every path argument is in the POSIX side's
    own spelling (--out and --test-file must be absolute); the result, `<out>/derive-result.json`,
    spells every host path for Windows. It must run under the PLAIN PATH of `wsl.exe -e` (the
    PowerShell driver used a login shell), so the toolchain is checked BY NAME first. It takes the
    clone WRITE lock itself and releases it however the stage ends. The injectable parameters exist
    for the self-test."""
    out = out if out is not None else sys.stdout
    err = err if err is not None else sys.stderr
    which = which or shutil.which
    env = dict(os.environ if environ is None else environ)
    p = _Parser(prog="sqlite_stage.py derive", add_help=False)
    p.add_argument("--out", required=True)
    p.add_argument("--sqlite-dir", default="")
    p.add_argument("--sqlite-repo-url", default=DEFAULT_SQLITE_REPO_URL)
    p.add_argument("--tcl-version", default="")
    p.add_argument("--jobs", default="")
    p.add_argument("--stage-build-json", required=True)
    p.add_argument("--tier", required=True)
    p.add_argument("--test-file", default="")
    try:
        a = p.parse_args(args)
    except _UsageError as exc:
        err.write("sqlite_stage.py derive: %s\n%s" % (exc, p.format_usage()))
        return 2
    log = C.Log(out)
    try:
        missing = [t for t in DERIVE_TOOLS if not which(t)]
        if missing:
            C.die("MISSING tool%s on the POSIX side's PATH: %s — the derive runs under `wsl.exe -e` (no login "
                  "shell), so each must be on that plain PATH. Install the recipe toolchain, e.g.:\n"
                  "    sudo apt-get install -y git build-essential tcl tcl-dev zlib1g-dev"
                  % ("s" if len(missing) > 1 else "", ", ".join(missing)))
        if translator is None:
            if not which("wslpath"):
                C.die("wslpath is not on PATH: the derive spells every path for a WINDOWS host and must run "
                      "inside WSL. On a POSIX host the driver calls stage() in-process instead.")
            translator = WslPathTranslator(env)
        for flag, value in (("--out", a.out), ("--test-file", a.test_file)):
            # on the POSIX side (the only side the derive runs on) absolute means a leading '/'
            if value and not (value.startswith("/") or os.path.isabs(value)):
                C.die("%s '%s' is not an absolute POSIX path. The derive takes every path in the POSIX "
                      "side's own spelling and spells it for the host itself (wslpath reads anything else "
                      "as RELATIVE and answers a wrong path)." % (flag, value))
        try:
            with open(a.stage_build_json, "rb") as fh:
                sb_text = fh.read()
        except OSError as exc:
            C.die("could not read the stage build configuration %s: %s" % (a.stage_build_json, exc))
        home = env.get("HOME") or os.path.expanduser("~")
        sqlite_dir = _expand(a.sqlite_dir, env) if a.sqlite_dir else _j(home, "src", "sqlite")
        cfg = StageConfig(sqlite_dir=sqlite_dir, out_dir=a.out, stage_build=sb_text, tier=a.tier,
                          test_file=a.test_file, jobs=_parse_jobs(a.jobs), tcl_version=a.tcl_version,
                          sqlite_repo_url=a.sqlite_repo_url, copy_to_stage=True,
                          translate_for_host=translator,
                          host_os=host_os if host_os is not None else C.host_os(),
                          environ=env, pkg_install=refuse_install,
                          lock_what="sqlite_stage.py derive for a Windows host (fetch/pull + configure + "
                                    "stage copy) — tier %s" % a.tier)
        # A result from an earlier run must never be read as this one's, whatever happens next -- so it is
        # removed before the clone lock is even asked for (`stage_and_persist` owns the write).
        _rm_f(_j(cfg.out_dir, RESULT_FILE))
        lock = (lock_factory or _default_lock_factory)(cfg.sqlite_dir)
        lock.write(cfg.lock_what, log)
        log.info("clone lock: WRITE on %s (held for the derive, released when it ends)" % cfg.sqlite_dir)
        try:
            result = stage_and_persist(cfg, log, lock=lock, stage_fn=stage_fn)
        finally:
            lock.release()
        path = _j(cfg.out_dir, RESULT_FILE)
    except C.HarnessDie as exc:
        _report(err, exc)
        return getattr(exc, "exit_code", 1)
    log.ok("derive: %d fixture TUs, %d CLI TUs staged; result -> %s"
           % (result.fixture_recipe.n_tus, result.cli_recipe.n_tus, path))
    return 0


# ── the self-test ──────────────────────────────────────────────────────────────────

ARMS = ("mk_var", "tcl_h_version", "tcl_select", "pin_shim", "stamp", "required_defines",
        "capabilities", "ldflags", "find", "zlib_headers", "tcl_headers", "stage_build", "config",
        "stage_dir", "staging", "json", "translate", "derive_cli", "git", "sourcing", "tcl_choice",
        "tclsh_real", "probe_link", "pin_exec", "orchestration")

_GOOD_SB = {"configureFlags": ["--enable-all", "--fts3"], "makeOptions": "-DSQLITE_ENABLE_STAT4",
            "optionDefines": ["SQLITE_ENABLE_STAT4"],
            "requiredDefines": ["SQLITE_ENABLE_FTS5", "SQLITE_ENABLE_RTREE", "SQLITE_ENABLE_STAT4"],
            "capabilityWitnesses": {"fts5": {"define": "SQLITE_ENABLE_FTS5", "file": "fts5aa"}},
            "sqliteCommit": "0123456789abcdef0123456789abcdef01234567"}


class _T:
    def __init__(self, out):
        self.out = out
        self.passed = self.failed = self.skipped = 0
        self.arm_name = "?"
        self.ran = []

    def check(self, label, cond, detail=""):
        if cond:
            self.passed += 1
            self.out.write("  ok   [%s] %s\n" % (self.arm_name, label))
        else:
            self.failed += 1
            self.out.write("  FAIL [%s] %s%s\n" % (self.arm_name, label, (" -- " + str(detail)) if detail else ""))

    def skip(self, label, why):
        self.skipped += 1
        self.out.write("  SKIP [%s] %s -- %s\n" % (self.arm_name, label, why))

    @contextlib.contextmanager
    def arm(self, name):
        self.arm_name = name
        self.ran.append(name)
        self.out.write("-- arm %s\n" % name)
        t0 = time.time()
        try:
            yield
        except Exception:  # an arm that crashes is a FAILURE, never a pass
            self.failed += 1
            self.out.write("  FAIL [%s] the arm CRASHED:\n%s\n" % (name, _indent(traceback.format_exc(), "      ")))
        self.out.write("   (%s: %.1fs)\n" % (name, time.time() - t0))


def _dies(fn, *a, **kw):
    """-> (raised HarnessDie?, its message, its exit code)."""
    try:
        fn(*a, **kw)
    except C.HarnessDie as exc:
        return True, str(exc), getattr(exc, "exit_code", 1)
    return False, "", 0


def _w(path, text, mode=None):
    os.makedirs(posixpath.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    if mode is not None:
        os.chmod(path, mode)
    return path


def _tmp(prefix):
    return _abs(tempfile.mkdtemp(prefix="p4s2-" + prefix + "-"))


def _cfg(tmp, **kw):
    base = dict(sqlite_dir=_j(tmp, "clone"), out_dir=_j(tmp, "out"), stage_build=dict(_GOOD_SB),
                host_os="linux", environ=dict(os.environ), jobs=2)
    base.update(kw)
    return StageConfig(**base)


def _ctx(tmp, **kw):
    log = C.Log(io.StringIO())
    return _Ctx(_cfg(tmp, **kw), log), log


def _posix():
    return os.name == "posix"


@contextlib.contextmanager
def _stub_modules(**mods):
    saved = dict((k, sys.modules.get(k)) for k in mods)
    try:
        for k, m in mods.items():
            sys.modules[k] = m
        yield
    finally:
        for k, m in saved.items():
            if m is None:
                sys.modules.pop(k, None)
            else:
                sys.modules[k] = m


def _hermetic_git_env(tmp):
    cfgf = _w(_j(tmp, "gitconfig-empty"), "")
    e = dict(os.environ)
    for k in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
        e.pop(k, None)
    e.update(GIT_CONFIG_GLOBAL=cfgf, GIT_CONFIG_NOSYSTEM="1", GIT_TERMINAL_PROMPT="0",
             GIT_AUTHOR_NAME="p4s2 self-test", GIT_AUTHOR_EMAIL="self-test@example.invalid",
             GIT_COMMITTER_NAME="p4s2 self-test", GIT_COMMITTER_EMAIL="self-test@example.invalid")
    return e


def _git(env, *args):
    r = C.capture(["git"] + list(args), env_=env, timeout=120)
    if r.rc != 0:
        raise RuntimeError("git %s failed (%d): %s" % (" ".join(args), r.rc, (r.err or r.out).strip()))
    return r.out.strip()


def _make_origin(tmp, env, files, branch="trunk"):
    """A bare origin whose default branch is `branch`, seeded from `files` {rel: (text, mode)}."""
    os.makedirs(tmp, exist_ok=True)
    bare, work = _j(tmp, "origin.git"), _j(tmp, "work")
    _git(env, "init", "--quiet", "--bare", bare)
    _git(env, "--git-dir", bare, "symbolic-ref", "HEAD", "refs/heads/" + branch)
    _git(env, "init", "--quiet", work)
    _git(env, "-C", work, "checkout", "--quiet", "-b", branch)
    for rel, (text, mode) in sorted(files.items()):
        _w(_j(work, rel), text, mode)
    _git(env, "-C", work, "add", "-A")
    _git(env, "-C", work, "commit", "--quiet", "-m", "c1")
    _git(env, "-C", work, "remote", "add", "origin", bare)
    _git(env, "-C", work, "push", "--quiet", "origin", branch)
    return bare, work


def _st_pure(t):
    with t.arm("mk_var"):
        tmp = _tmp("mk")
        mf = _w(_j(tmp, "Makefile"),
                "CC = gcc -O2   \nCCFLAGS = nope\nOPT_FEATURE_FLAGS=-DA -DB\nOPT_FEATURE_FLAGS = second\n"
                "TCL_CONFIG_SH\t=\t/usr/lib/tclConfig.sh\r\nEMPTY =\nX := y\n")
        t.check("first definition, trailing blanks trimmed", mk_var(mf, "CC") == "gcc -O2", repr(mk_var(mf, "CC")))
        t.check("a longer name sharing the prefix is its own variable", mk_var(mf, "CCFLAGS") == "nope")
        t.check("the FIRST of two definitions wins", mk_var(mf, "OPT_FEATURE_FLAGS") == "-DA -DB")
        t.check("tabs and a CR are trimmed", mk_var(mf, "TCL_CONFIG_SH") == "/usr/lib/tclConfig.sh")
        t.check("an empty value is empty", mk_var(mf, "EMPTY") == "")
        t.check("`X := y` is not `X = ...` (sed semantics)", mk_var(mf, "X") == "")
        t.check("an absent name answers ''", mk_var(mf, "NOPE") == "")
        t.check("a missing file answers ''", mk_var(_j(tmp, "nope.mk"), "CC") == "")
        shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("tcl_h_version"):
        tmp = _tmp("th")
        a = _w(_j(tmp, "a.h"), '#define TCL_MAJOR_VERSION 8\n#define TCL_VERSION\t    "8.6"\n')
        b = _w(_j(tmp, "b.h"), '#ifndef X\n#   define TCL_VERSION "9.0"\n#endif\n')
        c = _w(_j(tmp, "c.h"), '#define TCL_VERSIONX "1.0"\n#define TCL_VERSION "8.6"\n#define TCL_VERSION "9.0"\n')
        d = _w(_j(tmp, "d.h"), "/* nothing */\n")
        e = _w(_j(tmp, "e.h"), '#define TCL_VERSION "8.6"\r\n')
        t.check("8.6's `#define`", tcl_h_version(a) == "8.6")
        t.check("Tcl 9's indented `#   define`", tcl_h_version(b) == "9.0")
        t.check("TCL_VERSIONX is not TCL_VERSION, and the FIRST real define wins", tcl_h_version(c) == "8.6")
        t.check("no define answers ''", tcl_h_version(d) == "")
        t.check("a CRLF header", tcl_h_version(e) == "8.6")
        t.check("a missing file answers ''", tcl_h_version(_j(tmp, "none.h")) == "")
        shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("tcl_select"):
        inv = [("8.6", "/usr/lib/tcl8.6/tclConfig.sh"), ("9.0", "/opt/tcl9/lib/tclConfig.sh"),
               ("8.6", "/usr/lib/x86_64-linux-gnu/tcl8.6/tclConfig.sh")]
        t.check("the pin selects EXACTLY its version", select_tcl(inv, "9.0", "8.6") == ("9.0", "/opt/tcl9/lib/tclConfig.sh", "pinned"))
        t.check("an uninstalled pin is `pin-missing` (refused by the caller)", select_tcl(inv, "8.5", "8.6")[2] == "pin-missing")
        t.check("unpinned: the tclsh's own version, first in path order",
                select_tcl(inv, "", "8.6") == ("8.6", "/usr/lib/tcl8.6/tclConfig.sh", "tclsh"))
        t.check("unpinned, tclsh unmatched: the HIGHEST", select_tcl(inv, "", "8.4") == ("9.0", "/opt/tcl9/lib/tclConfig.sh", "highest"))
        t.check("unpinned, no tclsh: the highest", select_tcl(inv, "", "")[2] == "highest")
        t.check("nothing installed: none", select_tcl([], "", "8.6") == ("", "", "none"))
        num = [("8.6", "/a"), ("10.0", "/b"), ("9.1", "/c")]
        t.check("numeric major: 10.0 above 9.1 (a string sort puts 9.1 first)", highest_tcl(num) == ("10.0", "/b"))
        t.check("the negative is real: the string-sort answer differs", sorted(num, reverse=True)[0] != ("10.0", "/b"))
        t.check("numeric minor: 8.10 above 8.6", highest_tcl([("8.6", "/a"), ("8.10", "/b")]) == ("8.10", "/b"))
        t.check("a tie goes to the code-point-smallest path", highest_tcl([("9.0", "/z"), ("9.0", "/a")]) == ("9.0", "/a"))
        t.check("tcl_cfg_for answers the FIRST path declaring it", tcl_cfg_for(inv, "8.6") == "/usr/lib/tcl8.6/tclConfig.sh")

    with t.arm("pin_shim"):
        tmp = _tmp("pin")
        t.check("the shim text", pin_shim_text("/usr/bin/tclsh8.6") == '#!/bin/sh\nexec "/usr/bin/tclsh8.6" "$@"\n')
        t.check("a quote, a dollar and a backtick are made literal",
                pin_shim_text('/a/"b$c`d') == '#!/bin/sh\nexec "/a/\\"b\\$c\\`d" "$@"\n')
        env = {"PATH": "/usr/bin"}
        before = os.environ.get("PATH")
        d = write_pin_shim("/usr/bin/tclsh9.0", _j(tmp, "out"), env)
        with open(_j(d, "tclsh"), "rb") as fh:
            data = fh.read()
        t.check("the shim file carries exactly the text, LF only", data == b'#!/bin/sh\nexec "/usr/bin/tclsh9.0" "$@"\n')
        t.check("the shim dir is FIRST on the run's PATH", env["PATH"] == d + os.pathsep + "/usr/bin")
        t.check("os.environ's PATH is untouched (the pin belongs to the run's children)", os.environ.get("PATH") == before)
        if _posix():
            t.check("the shim is executable", os.access(_j(d, "tclsh"), os.X_OK))
        else:
            t.skip("the shim is executable", "no POSIX exec bit on this host")
        shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("stamp"):
        tmp = _tmp("stamp")
        clone = _j(tmp, "clone")
        bld = _j(clone, "bld-dss")
        os.makedirs(bld)
        now = stage_stamp_text("8.6", ["--a", "--b"], "-DX")
        t.check("the stamp text", now == "tclsh=8.6 configure=--a --b options=-DX")
        t.check("empty argv and options", stage_stamp_text("", [], "") == "tclsh= configure=<default> options=<none>")
        t.check("an ABSENT stamp on an EMPTY build dir fires, and wipes", stamp_decision(bld, clone, now) == ("wipe", ""))
        _w(_j(bld, "junk.o"), "x")
        t.check("an unidentified non-empty build dir is REFUSED", stamp_decision(bld, clone, now)[0] == "refuse")
        _w(_j(bld, "Makefile"), "all:\n")
        t.check("an ABSENT stamp with a Makefile fires (unknown provenance rebuilds)", stamp_decision(bld, clone, now)[0] == "wipe")
        _w(_j(bld, STAGE_STAMP), now + "\n\n")
        t.check("a matching stamp (trailing newlines stripped, as `$(cat)`) keeps", stamp_decision(bld, clone, now) == ("keep", now))
        t.check("a stamp differing by one character does not keep", stamp_decision(bld, clone, now + "x")[0] == "wipe")
        os.remove(_j(bld, "Makefile"))
        os.remove(_j(bld, STAGE_STAMP))
        _w(_j(bld, STAGE_STAMP_LEGACY), "tclsh=8.6\n")
        t.check("the legacy Tcl stamp alone identifies the dir", stamp_decision(bld, clone, now)[0] == "wipe")
        other = _j(clone, "elsewhere")
        os.makedirs(other)
        _w(_j(other, "Makefile"), "all:\n")
        t.check("a build dir that is not <sqlite>/bld-dss is REFUSED", stamp_decision(other, clone, now)[0] == "refuse")
        shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("required_defines"):
        tmp = _tmp("req")
        mf = _w(_j(tmp, "Makefile"), "OPT_FEATURE_FLAGS = -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE=1 "
                                     "-DSQLITE_ENABLE_FTS3_PARENTHESIS $(OPTIONS)\n")
        flags = mk_var(mf, "OPT_FEATURE_FLAGS")
        req = ["SQLITE_ENABLE_FTS5", "SQLITE_ENABLE_RTREE", "SQLITE_ENABLE_STAT4", "SQLITE_ENABLE_FTS3"]
        m = missing_required_defines(flags, req, "-DSQLITE_ENABLE_STAT4")
        t.check("exactly FTS3 is missing (FTS3_PARENTHESIS does not satisfy it)", m == ["SQLITE_ENABLE_FTS3"], m)
        t.check("`-DNAME=1` satisfies NAME", "SQLITE_ENABLE_RTREE" not in m)
        t.check("a define arriving through make OPTIONS is not asked of OPT_FEATURE_FLAGS", "SQLITE_ENABLE_STAT4" not in m)
        m2 = missing_required_defines(flags, req, "")
        t.check("without the OPTIONS it IS missing (the negative is producible)", "SQLITE_ENABLE_STAT4" in m2)
        t.check("an empty OPT_FEATURE_FLAGS misses everything not in OPTIONS",
                missing_required_defines("", req, "") == req)
        shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("capabilities"):
        req = ["A", "B"]
        t.check("NAME and NAME=VALUE both count", missing_capabilities(["A", "B=1", "CX"], req) == [])
        t.check("NAMEX does not satisfy NAME", missing_capabilities(["A", "BX"], req) == ["B"])
        died, msg, _ = _dies(assert_recipe_capabilities, "sqlite3 CLI", "/r.txt", ["A", "CX"], ["A", "C"], "-DC")
        t.check("a missing capability is refused, naming it and the recipe label",
                died and "the sqlite3 CLI recipe is MISSING declared capabilities: C" in msg and "/r.txt" in msg, msg[:200])
        died, _, _ = _dies(assert_recipe_capabilities, "testfixture", "/r.txt", ["A", "B=2"], req, "")
        t.check("a complete recipe passes", not died)

    with t.arm("ldflags"):
        probes = []

        def probe(args):
            probes.append(tuple(args))
            ok_bare = {"-ldl", "-lz", "-lpthread", "-lm"}
            if len(args) == 1:
                return args[0] in ok_bare
            return args[0] == "-L/opt/t" and args[1] in ("-ltommath", "-lbaz")

        dirs = {"tommath": "/opt/t", "bar": "/opt/t", "baz": "/opt/t", "foo": ""}
        flags, notes, warns = tcl_libs_ldflags(
            "-ldl -lz  -lpthread -framework CoreFoundation -ltommath -lfoo -lbar -lbaz -l",
            probe, lambda n: dirs.get(n, ""), "cc", 5)
        t.check("one -L for two libraries in the same dir", flags == "-L/opt/t", flags)
        t.check("a note for each repaired library", len(notes) == 2 and "-ltommath" in notes[0], notes)
        t.check("an unresolvable -l is WARNED, not added", any("-lfoo" in w and "will NOT link" in w for w in warns), warns)
        t.check("found-but-unlinkable is WARNED", any("libbar was found under /opt/t" in w for w in warns), warns)
        t.check("-framework / CoreFoundation / a bare -l are never probed",
                not any(a[-1] in ("-framework", "-lCoreFoundation", "-l") for a in probes))
        t.check("libraries that already link add NOTHING", ("-L/opt/t", "-ldl") not in probes)
        t.check("no TCL_LIBS, no flags", tcl_libs_ldflags("", probe, lambda n: "", "cc", 5) == ("", [], []))

    with t.arm("find"):
        tmp = _tmp("find")
        r1, r2 = _j(tmp, "r1"), _j(tmp, "r2")
        _w(_j(r1, "node", "zlib.h"), "nested vendored copy\n")
        _w(_j(r1, "zlib.h"), "system\n")
        _w(_j(r1, "a", "b", "c", "deep.h"), "4 deep\n")
        _w(_j(r2, "zlib.h"), "second root\n")
        _w(_j(r1, "tcl8.6", "tcl.h"), "")
        _w(_j(r1, "tcl9.0", "tcl.h"), "")
        t.check("the SHALLOWEST match wins inside a root", find_first([r1], "zlib.h") == _j(r1, "zlib.h"))
        t.check("the negative: the nested copy exists and IS found by the full listing",
                _j(r1, "node", "zlib.h") in find_all_sorted([r1], "zlib.h"))
        t.check("roots keep their declared precedence", find_first([_j(tmp, "absent"), r2, r1], "zlib.h") == _j(r2, "zlib.h"))
        t.check("-maxdepth 3 excludes depth 4", find_first([r1], "deep.h", maxdepth=3) == "")
        t.check("no maxdepth finds depth 4", find_first([r1], "deep.h") == _j(r1, "a", "b", "c", "deep.h"))
        t.check("-path selects by the whole path", find_first([r1], "tcl.h", path_glob="*tcl8.6*") == _j(r1, "tcl8.6", "tcl.h"))
        t.check("a [0-9] class works in the -path glob", find_first([r1], "tcl.h", path_glob="*tcl[0-9]*") == _j(r1, "tcl8.6", "tcl.h"))
        if _posix():
            os.symlink(r1, _j(tmp, "link"))
            t.check("a symlinked ROOT is not descended (find -P)", find_first([_j(tmp, "link")], "zlib.h") == "")
        else:
            t.skip("a symlinked ROOT is not descended (find -P)", "symlinks need privileges on this host")
        shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("zlib_headers"):
        tmp = _tmp("zlib")
        inc, bld = _j(tmp, "inc"), _j(tmp, "bld")
        _w(_j(inc, "zlib.h"), "zlib\n")
        _w(_j(inc, "zconf.h"), "zconf beside\n")
        _w(_j(inc, "x", "zconf.h"), "zconf nested\n")
        _w(_j(bld, "zinc", "old.h"), "stale\n")
        _w(_j(bld, "zinc", "k", "keep.h"), "a stage\n")
        zs, zh, zc = stage_zlib_headers([inc], bld)
        t.check("zlib.h and zconf.h copied verbatim into zinc-src",
                open(_j(zs, "zlib.h")).read() == "zlib\n" and open(_j(zs, "zconf.h")).read() == "zconf beside\n")
        t.check("zconf.h BESIDE zlib.h is preferred", zc == _j(inc, "zconf.h"))
        t.check("the stale top-level zinc/*.h is gone, a stage dir's header kept",
                not os.path.exists(_j(bld, "zinc", "old.h")) and os.path.exists(_j(bld, "zinc", "k", "keep.h")))
        os.remove(_j(inc, "zconf.h"))
        zs, zh, zc = stage_zlib_headers([inc], bld)
        t.check("the sweep finds zconf.h elsewhere when none is beside", zc == _j(inc, "x", "zconf.h"))
        os.remove(_j(inc, "x", "zconf.h"))
        died, msg, _ = _dies(stage_zlib_headers, [inc], bld)
        t.check("no zconf.h anywhere is REFUSED", died and "zconf.h not found beside" in msg, msg[:120])
        os.remove(_j(inc, "zlib.h"))
        died, msg, _ = _dies(stage_zlib_headers, [inc], bld)
        t.check("no zlib.h is REFUSED", died and "zlib.h not found" in msg, msg[:120])
        shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("tcl_headers"):
        tmp = _tmp("tclh")
        inc86 = _j(tmp, "tcl8.6")
        _w(_j(inc86, "tcl.h"), '#define TCL_VERSION "8.6"\n')
        rec9 = _j(tmp, "rec9")
        _w(_j(rec9, "tcl.h"), '#define TCL_VERSION "9.0"\n')
        recq = _j(tmp, "recq")
        _w(_j(recq, "tcl.h"), "/* no version */\n")
        why = {"roots": ("/r",), "tclsh": "/usr/bin/tclsh", "tclsh_ver": "8.6", "bld": "/b"}
        t.check("agreeing header: the version stands", check_tcl_headers(inc86, "8.6", "/c", [recq], why) == "8.6")
        t.check("no tclConfig version: the header's own is taken", check_tcl_headers(inc86, "", "", [], why) == "8.6")
        died, msg, _ = _dies(check_tcl_headers, inc86, "9.0", "/c", [], why)
        t.check("tclConfig 9.0 vs tcl.h 8.6 is INCOHERENT (R95)", died and "Tcl staging is INCOHERENT" in msg)
        died, msg, _ = _dies(check_tcl_headers, inc86, "8.6", "/c", [rec9], why)
        t.check("a recipe -I dir holding a 9.0 tcl.h is a MISMATCH (R96)", died and "RECIPE/STAGING Tcl MISMATCH" in msg and rec9 in msg)
        died, msg, _ = _dies(check_tcl_headers, _j(tmp, "none"), "8.6", "/c", [], why)
        t.check("no tcl.h is fatal for the whole run (R94)", died and "tcl.h not found" in msg)
        died, msg, _ = _dies(check_tcl_headers, "", "8.6", "/c", [], why)
        t.check("an EMPTY header dir is not the cwd (the .sh's `dirname ''` = '.')", died)
        shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("stage_build"):
        sb = parse_stage_build(json.dumps(_GOOD_SB))
        t.check("a good answer parses", sb["configure_flags"] == ["--enable-all", "--fts3"] and sb["make_options"] == "-DSQLITE_ENABLE_STAT4")
        died, msg, _ = _dies(parse_stage_build, "{not json")
        t.check("not JSON is refused", died and "did not print the JSON" in msg)
        bad = dict(_GOOD_SB)
        bad.pop("configureFlags")
        died, msg, _ = _dies(parse_stage_build, bad)
        t.check("no configureFlags is a contract break", died and "no configureFlags or no requiredDefines" in msg)
        bad = dict(_GOOD_SB, requiredDefines=[])
        t.check("an EMPTY requiredDefines is a contract break", _dies(parse_stage_build, bad)[0])
        bad = dict(_GOOD_SB, configureFlags=["--a b"])
        t.check("a flag carrying whitespace is refused (one argv word each)", _dies(parse_stage_build, bad)[0])
        bad = dict(_GOOD_SB, makeOptions=["-DX"])
        t.check("a non-string makeOptions is refused", _dies(parse_stage_build, bad)[0])
        bad = dict(_GOOD_SB)
        bad.pop("sqliteCommit")
        died, msg, _ = _dies(parse_stage_build, bad)
        t.check("no sqliteCommit is a contract break (the subject would be whatever master is that day)",
                died and "sqliteCommit" in msg, msg[:160])
        died, msg, _ = _dies(parse_stage_build, dict(_GOOD_SB, sqliteCommit="d21bd37c7c"))
        t.check("... and an ABBREVIATED one too: the pin is the FULL sha", died and "sqliteCommit" in msg, msg[:160])
        t.check("a UTF-8 BOM is tolerated (a file written on Windows)", parse_stage_build(b"\xef\xbb\xbf" + json.dumps(_GOOD_SB).encode())["required_defines"][0] == "SQLITE_ENABLE_FTS5")

    with t.arm("config"):
        tmp = _tmp("cfg")
        e = {"HOME": _j(tmp, "home"), "PATH": os.environ.get("PATH", "")}
        c = StageConfig.from_env(out_dir=_j(tmp, "out"), stage_build=_GOOD_SB, environ=e, host_os="linux")
        t.check("SQLITE_DIR defaults to $HOME/src/sqlite", c.sqlite_dir == _abs(_j(tmp, "home", "src", "sqlite")))
        t.check("the sqlite URL defaults to https", c.sqlite_repo_url == DEFAULT_SQLITE_REPO_URL)
        t.check("the tier defaults to veryquick", c.tier == "veryquick")
        t.check("jobs default to a positive count", c.jobs >= 1)
        e2 = dict(e, SQLITE_DIR=_j(tmp, "s"), JOBS="3", DSS_TIER="quick", DSS_TCL_VERSION=" 9.0 ", DSS_TEST_FILE="/x.test")
        c = StageConfig.from_env(out_dir=_j(tmp, "out"), stage_build=_GOOD_SB, environ=e2, host_os="linux")
        t.check("the environment is honoured", (c.sqlite_dir, c.jobs, c.tier, c.tcl_version, c.test_file)
                == (_abs(_j(tmp, "s")), 3, "quick", "9.0", "/x.test"))
        t.check("DSS_JOBS wins over JOBS (sqlite_common.Config's order)", StageConfig.from_env(
            out_dir=_j(tmp, "out"), stage_build=_GOOD_SB, environ=dict(e, DSS_JOBS="5", JOBS="3"),
            host_os="linux").jobs == 5)
        t.check("a blank DSS_JOBS falls through to JOBS", StageConfig.from_env(
            out_dir=_j(tmp, "out"), stage_build=_GOOD_SB, environ=dict(e, DSS_JOBS=" ", JOBS="3"),
            host_os="linux").jobs == 3)
        died, msg, _ = _dies(StageConfig.from_env, out_dir=_j(tmp, "out"), stage_build=_GOOD_SB, environ=dict(e, JOBS="x"), host_os="linux")
        t.check("a non-integer JOBS is refused up front", died and "JOBS='x'" in msg)
        died, msg, _ = _dies(_cfg, tmp, translate_for_host=lambda p: p)
        t.check("a translator without copy_to_stage is refused", died and "identity" in msg)
        died, msg, _ = _dies(_cfg, tmp, out_dir=tmp)
        t.check("an out dir CONTAINING the clone is refused", died and "contains it" in msg)
        died, msg, _ = _dies(_cfg, tmp, out_dir=_j(tmp, "clone", "stage"), copy_to_stage=True, translate_for_host=lambda p: p)
        t.check("a stage INSIDE the clone is refused", died and "inside the sqlite clone" in msg)
        died, msg, _ = _dies(_cfg, tmp, host_os="windows")
        t.check("a Windows host is refused (the POSIX half runs on the POSIX side)", died and "POSIX host" in msg)
        died, msg, _ = _dies(_cfg, tmp, tier="../x")
        t.check("a tier that is not a name is refused", died)
        rc_ = types.SimpleNamespace(sqlite_repo_url="https://x/s.git", jobs=4, tcl_version="8.6", tier="quick",
                                    test_file="")
        run = types.SimpleNamespace(cfg=rc_, sqlite_dir_posix=_j(tmp, "s"), out_dir=_j(tmp, "o"),
                                    stage_build=dict(_GOOD_SB), host="linux")
        c = StageConfig.from_run(run)
        t.check("from_run takes the driver's validated Config and resolved paths -- the stage under the "
                "output tree's `stage/`, as on every host",
                (c.sqlite_dir, c.out_dir, c.jobs, c.tier, c.tcl_version, c.sqlite_repo_url, c.copy_to_stage)
                == (_abs(_j(tmp, "s")), _abs(_j(tmp, "o", STAGE_SUBDIR)), 4, "quick", "8.6", "https://x/s.git",
                    False))
        run.sqlite_dir_posix = ""
        died, msg, _ = _dies(StageConfig.from_run, run)
        t.check("from_run refuses an unset clone path by name (never a silent default)",
                died and "run.sqlite_dir_posix" in msg, msg)
        # stage_and_persist: the ONE writer of the result, for a POSIX host's driver as for the WSL derive.
        pcfg = _cfg(tmp)
        _w(_j(pcfg.out_dir, RESULT_FILE), '{"stale": true}\n')

        def dying_stage(cfg_, log_, lock=None):
            C.die("the stage died part-way (the self-test)")
        died, msg, _ = _dies(stage_and_persist, pcfg, C.Log(io.StringIO()), None, dying_stage)
        t.check("stage_and_persist removes an earlier run's result BEFORE staging: a stage that dies "
                "part-way leaves none behind", died and not os.path.exists(_j(pcfg.out_dir, RESULT_FILE)), msg)

        def fine_stage(cfg_, log_, lock=None):
            os.makedirs(cfg_.out_dir, exist_ok=True)
            return _canned_result("/p")
        res = stage_and_persist(pcfg, C.Log(io.StringIO()), None, fine_stage)
        rpath = _j(pcfg.out_dir, RESULT_FILE)
        back = None
        if os.path.isfile(rpath):
            with open(rpath, encoding="utf-8") as fh:
                back = StageResult.from_json(fh.read()).to_dict()
        t.check("stage_and_persist writes the result it returns, where the recompile reads it",
                back == res.to_dict(), "no result was written at %s" % rpath if back is None else back)
        got = []
        ctx = _Ctx(_cfg(tmp, pkg_install=lambda a, b=None: got.append((a, b))), C.Log(io.StringIO()))
        ctx.which("no-such-tool-p4s2")
        t.check("a run that installed nothing does not re-take the Tcl inventory", not ctx.install_attempted)
        ctx.pkg_install("tcl-dev", "tcl-tk")
        t.check("an install ATTEMPT is recorded (Step 6 re-takes the inventory) and reaches the installer",
                ctx.install_attempted and got == [("tcl-dev", "tcl-tk")])
        t.check("... and forgets every remembered PATH lookup", ctx._which == {})
        shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("stage_dir"):
        tmp = _tmp("sd")
        s = _j(tmp, "stage")
        prepare_stage_dir(s)
        t.check("a fresh stage is created and marked", os.path.isfile(_j(s, STAGE_MARKER)))
        _w(_j(s, "old", "file.c"), "x")
        prepare_stage_dir(s)
        t.check("a MARKED stage is wiped", not os.path.exists(_j(s, "old")) and os.path.isfile(_j(s, STAGE_MARKER)))
        old = _j(tmp, "oldps1")
        _w(_j(old, OLD_STAGE_MARK), "C:/x\n")
        _w(_j(old, "sqlite", "a.c"), "x")
        prepare_stage_dir(old)
        t.check("the old PowerShell driver's stage is recognised and wiped", not os.path.exists(_j(old, "sqlite")))
        foreign = _j(tmp, "foreign")
        _w(_j(foreign, "precious.txt"), "do not delete\n")
        died, msg, _ = _dies(prepare_stage_dir, foreign)
        t.check("an UNMARKED non-empty directory is refused", died and "refusing to wipe" in msg)
        t.check("... and left intact", os.path.isfile(_j(foreign, "precious.txt")))
        empty = _j(tmp, "empty")
        os.makedirs(empty)
        prepare_stage_dir(empty)
        t.check("an empty directory is taken", os.path.isfile(_j(empty, STAGE_MARKER)))
        shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("staging"):
        _st_staging(t)

    with t.arm("json"):
        r = _canned_result("/p")
        back = StageResult.from_json(r.to_json())
        t.check("to_json -> from_json round-trips every field", back == r)
        t.check("tcl_lib_file survives the round trip", back.tcl_lib_file == "libtcl8.6.so")
        t.check("a None path survives as None", back.reference_cli is None and back.reference_cli_posix is None)
        d = r.to_dict()
        d.pop("tcl_lib_file")
        died, msg, _ = _dies(StageResult.from_dict, d)
        t.check("a MISSING field is a contract break", died and "tcl_lib_file" in msg)
        d = r.to_dict()
        d["surprise"] = 1
        died, msg, _ = _dies(StageResult.from_dict, d)
        t.check("an UNKNOWN field is a contract break", died and "surprise" in msg)
        d = r.to_dict()
        d["schema"] = "dss-sqlite-stage-result/0"
        t.check("another schema is refused", _dies(StageResult.from_dict, d)[0])
        d = r.to_dict()
        d["fixture_recipe"]["n_tus"] = "189"
        t.check("a mistyped recipe count is refused", _dies(StageResult.from_dict, d)[0])
        t.check("not JSON is refused", _dies(StageResult.from_json, "{")[0])

    with t.arm("translate"):
        r = _canned_result("/p")
        tr = r.translated(lambda p: "C:/host" + p)
        d = tr.to_dict()
        for k in StageResult.HOST_PATHS:
            t.check("host path %s translated" % k, d[k].startswith("C:/host/"), d[k])
        t.check("an optional host path that is set is translated", d["reference_fixture"].startswith("C:/host/"))
        t.check("an absent optional path stays None", d["reference_cli"] is None)
        t.check("DSS_TEST_FILE, when set, is spelled for the host too", d["test_file"] == "C:/host/p/ops/my.test")
        r0 = StageResult(test_file="", **dict((k, getattr(r, k)) for k in StageResult.FIELDS if k != "test_file"))
        t.check("... and an unset one stays ''", r0.translated(lambda p: "C:/host" + p).test_file == "")
        for rk in StageResult.RECIPES:
            for k in RecipeFiles.PATH_KEYS:
                t.check("%s.%s translated" % (rk, k), d[rk][k].startswith("C:/host/"), d[rk][k])
        for k in StageResult.POSIX_PATHS:
            t.check("%s kept in the POSIX spelling" % k, d[k] == getattr(r, k))
        t.check("tcl_lib_file (a NAME) untouched", d["tcl_lib_file"] == "libtcl8.6.so")
        left = [k for k in StageResult.HOST_PATHS + StageResult.OPTIONAL_HOST_PATHS
                if isinstance(d[k], str) and d[k].startswith("/p/")]
        t.check("NO host path is left in the POSIX form", not left, left)

    with t.arm("derive_cli"):
        _st_derive(t)


def _canned_result(root):
    rf = lambda n: RecipeFiles(tus=root + "/o/%s-tus.txt" % n, defines=root + "/o/%s-defs.txt" % n,
                               includes=root + "/o/%s-incs.txt" % n, recipe=root + "/o/%s-recipe.txt" % n,
                               summary="%s: 1 TUs" % n, n_tus=1, n_defines=2, n_includes=3)
    return StageResult(
        out_dir=root + "/o", sqlite_dir=root + "/s", bld=root + "/s/bld-dss", src=root + "/s/src",
        ext=root + "/s/ext", testdir=root + "/s/test", tier_file=root + "/s/test/veryquick.test",
        tcl_inc=root + "/usr/include/tcl8.6", zinc_src=root + "/s/bld-dss/zinc-src",
        sqlite_cfg_h=root + "/s/bld-dss/sqlite_cfg.h", reference_build_log=root + "/o/reference-build.log",
        reference_cli_log=root + "/o/reference-cli-build.log", amalgamation_log=root + "/o/amalgamation-regen.log",
        configure_log=root + "/o/configure.log", reference_fixture=root + "/o/reference-testfixture",
        reference_cli=None, out_dir_posix=root + "/o", sqlite_dir_posix=root + "/s", bld_posix=root + "/s/bld-dss",
        tclsh_posix="/usr/bin/tclsh", tcl_config_posix="/usr/lib/tclConfig.sh",
        reference_fixture_posix=root + "/o/reference-testfixture", reference_cli_posix=None,
        fixture_recipe=rf("fixture"), cli_recipe=rf("cli"), tier="veryquick", test_file=root + "/ops/my.test",
        tcl_version="8.6", tcl_lib_file="libtcl8.6.so", reference_fixture_why="",
        reference_cli_why="did not build", amalgamation_regen="ok", sqlite_head="abc1234",
        sqlite_branch="master", stage_identity="tclsh=8.6 configure=--enable-all options=<none>",
        make_options="-DSQLITE_ENABLE_STAT4", configure_args=["--enable-all"],
        required_defines=["SQLITE_ENABLE_FTS5"], clone_lock_notes=["stole a STALE clone WRITE lock"],
        ref_link_notes=[], ref_link_warnings=["w"], warnings=["x"], copy_to_stage=True,
        witnesses={"fts5": {"define": "SQLITE_ENABLE_FTS5", "file": "fts5aa"}})


def _fake_clone_tree(root):
    """A synthetic clone + build dir + Tcl/zlib header dirs for the staging arm."""
    clone = _j(root, "clone")
    bld = _j(clone, "bld-dss")
    for rel in ("src/main.c", "src/sqliteInt.h", "ext/misc/m.c", "ext/rtree/r.c", "ext/fts5/test/f.test",
                "ext/session/s.c", "test/veryquick.test", "bld-dss/parse.c", "bld-dss/sqlite3.h",
                "bld-dss/sqlite_cfg.h", "bld-dss/tsrc/sub.c", "bld-dss/.hidden.c"):
        _w(_j(clone, rel), "/* %s */\n" % rel)
    tclinc = _j(root, "usr", "include", "tcl8.6")
    _w(_j(tclinc, "tcl.h"), '#define TCL_VERSION "8.6"\n')
    _w(_j(tclinc, "tclDecls.h"), "/* decls */\n")
    zinc = _j(bld, "zinc-src")
    _w(_j(zinc, "zlib.h"), "zlib\n")
    _w(_j(zinc, "zconf.h"), "zconf\n")
    return clone, bld, tclinc, zinc


def _st_staging(t):
    tmp = _tmp("stg")
    clone, bld, tclinc, zinc = _fake_clone_tree(tmp)
    stage_dir = _j(tmp, "stage")
    prepare_stage_dir(stage_dir)
    s = stage_tree(clone, bld, tclinc, zinc, stage_dir)
    t.check("src/, ext/ and test/ copied", all(os.path.isfile(_j(s["sqlite"], r)) for r in (
        "src/main.c", "ext/misc/m.c", "ext/fts5/test/f.test", "test/veryquick.test")))
    t.check("the build dir's top-level *.c + *.h copied", os.path.isfile(_j(s["bld"], "parse.c"))
            and os.path.isfile(_j(s["bld"], "sqlite_cfg.h")))
    t.check("... but not its subdirectories, nor a dotfile (a shell glob matches neither)",
            not os.path.exists(_j(s["bld"], "tsrc")) and not os.path.exists(_j(s["bld"], ".hidden.c")))
    t.check("the Tcl headers staged into tclinc/", os.path.isfile(_j(s["tclinc"], "tcl.h")) and os.path.isfile(_j(s["tclinc"], "tclDecls.h")))
    t.check("zlib's pair staged into zinc-src/", os.path.isfile(_j(s["zinc_src"], "zlib.h")))
    t.check("test/ is ext/'s SIBLING", os.path.isdir(_j(s["test"], "..", "ext", "fts5", "test")))
    pairs = stage_pairs(clone, bld, tclinc, stage_dir)
    tr = lambda p: "H:" + p
    tus = [_j(clone, "src/main.c"), _j(clone, "ext/misc/m.c"), _j(bld, "parse.c")]
    incs = [_j(clone, "src"), _j(clone, "nonexistent"), tclinc, _j(clone, "ext/rtree"), _j(clone, "tool")]
    tus_f, incs_f = _w(_j(tmp, "tus.txt"), "\n".join(tus) + "\n"), _w(_j(tmp, "incs.txt"), "\n".join(incs) + "\n")
    os.makedirs(_j(clone, "tool"))          # exists HERE, is never staged
    log = C.Log(io.StringIO())
    ctx = _Ctx(_cfg(tmp, sqlite_dir=clone, out_dir=stage_dir, copy_to_stage=True, translate_for_host=tr), log)
    n, ni = stage_recipe_lists(ctx, "fixture", tus_f, incs_f, pairs, _j(tmp, "st.txt"), _j(tmp, "si.txt"))
    got = read_list(_j(tmp, "st.txt"))
    t.check("every TU remapped into the stage and spelled for the host",
            got == ["H:" + _j(stage_dir, "sqlite/src/main.c"), "H:" + _j(stage_dir, "sqlite/ext/misc/m.c"),
                    "H:" + _j(stage_dir, "sqlite/bld/parse.c")], got)
    t.check("the staged TU count EQUALS the derived one", n == len(tus))
    gi = read_list(_j(tmp, "si.txt"))
    t.check("-I dirs remapped, order kept, a missing one dropped, the Tcl dir -> tclinc",
            gi == ["H:" + _j(stage_dir, "sqlite/src"), "H:" + _j(stage_dir, "tclinc"),
                   "H:" + _j(stage_dir, "sqlite/ext/rtree")], gi)
    t.check("a dir that exists HERE but not in the stage is dropped OUT LOUD",
            any("tool" in w and "dropped" in w for w in ctx.warnings), ctx.warnings)
    t.check("the lists hold NO POSIX-side path", not any(l.startswith(clone) or l.startswith(stage_dir) for l in got + gi))
    lost_f = _w(_j(tmp, "tus2.txt"), "\n".join(tus + [_j(bld, "tsrc/sub.c")]) + "\n")
    died, msg, _ = _dies(stage_recipe_lists, ctx, "fixture", lost_f, incs_f, pairs, _j(tmp, "x.txt"), _j(tmp, "y.txt"))
    t.check("a TU in a build-dir SUBDIRECTORY (not staged) is LOST and refused by name",
            died and "staging lost fixture TUs: derived 4, staged 3" in msg and "tsrc/sub.c" in msg, msg[:200])
    t.check("... and never silently mapped to a same-named top-level file",
            remap_path(_j(bld, "tsrc/sub.c"), pairs) == _j(stage_dir, "sqlite/bld/tsrc/sub.c"))
    outside = _w(_j(tmp, "elsewhere", "x.c"), "x\n")
    pt_f = _w(_j(tmp, "tus3.txt"), "\n".join(tus + [outside]) + "\n")
    ctx.warnings[:] = []
    n, _ni = stage_recipe_lists(ctx, "CLI", pt_f, incs_f, pairs, _j(tmp, "p.txt"), _j(tmp, "q.txt"))
    t.check("a TU under no staged root is kept, spelled for the host, and REPORTED",
            n == 4 and read_list(_j(tmp, "p.txt"))[-1] == "H:" + outside
            and any("IN PLACE" in w and outside in w for w in ctx.warnings), ctx.warnings)
    shutil.rmtree(_j(clone, "ext", "session"))
    prepare_stage_dir(stage_dir)
    died, msg, _ = _dies(stage_tree, clone, bld, tclinc, zinc, stage_dir)
    t.check("a stage whose test dir has no ../ext/session is REFUSED",
            died and "the staged test dir has no ../ext/session" in msg, msg[:160])
    shutil.rmtree(tmp, ignore_errors=True)


class _FakeLock:
    def __init__(self, blocked=None):
        self.blocked, self.writes, self.releases, self.notes = blocked, 0, 0, ["stole a STALE clone WRITE lock"]

    def write(self, what, log):
        self.writes += 1
        if self.blocked is not None:
            raise C.CloneLockBlocked(self.blocked)

    def release(self):
        self.releases += 1


def _st_derive(t):
    tmp = _tmp("drv")
    sbj = _w(_j(tmp, "sb.json"), json.dumps(_GOOD_SB))
    stage_dir = _j(tmp, "stage")
    base = ["--out", stage_dir, "--sqlite-dir", _j(tmp, "clone"), "--stage-build-json", sbj, "--tier", "veryquick"]
    calls = []

    def fake_stage(cfg, log, lock=None):
        calls.append((cfg, lock))
        os.makedirs(cfg.out_dir, exist_ok=True)
        return _canned_result("/p").translated(cfg.translate_for_host)

    def run(args, which=None, lock=None, stage_fn=fake_stage):
        out, err = io.StringIO(), io.StringIO()
        rc = derive_main(args, which=which or (lambda n: "/usr/bin/" + n), lock_factory=lambda c: lock,
                         translator=lambda p: "C:/fake" + p, stage_fn=stage_fn, out=out, err=err,
                         environ={"HOME": _j(tmp, "home"), "PATH": "/usr/bin"}, host_os="linux")
        return rc, out.getvalue(), err.getvalue()

    lock = _FakeLock()
    rc, o, e = run(base, which=lambda n: None if n == "ar" else "/usr/bin/" + n, lock=lock)
    t.check("a missing tool is refused BY NAME (exit 1)", rc == 1 and "MISSING tool on the POSIX side's PATH: ar" in e, e)
    t.check("... before the lock is taken or anything staged", lock.writes == 0 and not calls)
    rc, o, e = run(base, which=lambda n: None if n in ("gcc", "tclsh") else "/usr/bin/" + n, lock=_FakeLock())
    t.check("every missing tool is named", rc == 1 and "gcc, tclsh" in e, e)
    _w(_j(stage_dir, RESULT_FILE), '{"stale": true}\n')
    lock = _FakeLock(blocked="DSS-CLONE-LOCK-BLOCKED\n\n [X] ERROR: another dss harness run is MUTATING this sqlite clone")
    rc, o, e = run(base, lock=lock)
    lines = e.split("\n")
    t.check("a blocked clone lock exits 3", rc == 3, rc)
    t.check("its FIRST stderr line is exactly DSS-CLONE-LOCK-BLOCKED", lines[0] == CLONE_LOCK_BLOCKED, lines[:2])
    t.check("the token is not repeated on the next line", lines[1] != CLONE_LOCK_BLOCKED, lines[:2])
    t.check("the lock's own ` [X] ERROR:` framing is kept, not doubled", " [X] ERROR: another" in e
            and "✗ ERROR:  [X]" not in e, e[:200])
    t.check("... nothing staged, nothing released that was never held", not calls and lock.releases == 0)
    t.check("a stale derive-result.json is removed before anything else can fail", not os.path.exists(_j(stage_dir, RESULT_FILE)))
    rc, o, e = run(base, lock=_FakeLock(blocked="the holder forgot the token"))
    t.check("the first line is the token even when the lock's message lacks it", rc == 3 and e.split("\n")[0] == CLONE_LOCK_BLOCKED, e[:80])

    def failing_stage(cfg, log, lock=None):
        C.die("configure accepted its flags and did NOT produce the defines they exist for.")

    lock = _FakeLock()
    rc, o, e = run(base, lock=lock, stage_fn=failing_stage)
    t.check("a refusal inside the stage exits 1 with the message on stderr", rc == 1 and "did NOT produce the defines" in e, e)
    t.check("... and the lock it took is RELEASED, once", lock.writes == 1 and lock.releases == 1)
    rc, o, e = run(base + ["--test-file", "ops/my.test"], lock=_FakeLock())
    t.check("a RELATIVE --test-file is refused (wslpath would mangle it)", rc == 1 and "not an absolute POSIX path" in e, e)
    if _posix():
        rc, o, e = run(base + ["--test-file", "C:/ops/my.test"], lock=_FakeLock())
        t.check("a --test-file in a WINDOWS spelling is refused on the POSIX side",
                rc == 1 and "not an absolute POSIX path" in e, e)
    else:
        t.skip("a --test-file in a WINDOWS spelling is refused on the POSIX side", "this host is not the POSIX side")
    lock = _FakeLock()
    rc, o, e = run(base + ["--test-file", "/mnt/c/ops/my.test", "--jobs", "3", "--tcl-version", "8.6"], lock=lock)
    t.check("a good derive exits 0", rc == 0, e)
    cfg = calls[-1][0]
    t.check("it stages (copy_to_stage) with the injected translator and NO installer",
            cfg.copy_to_stage and cfg.translate_for_host("/x") == "C:/fake/x" and cfg.pkg_install is refuse_install)
    t.check("the flags reached the config", (cfg.test_file, cfg.jobs, cfg.tcl_version) == ("/mnt/c/ops/my.test", 3, "8.6"))
    t.check("the lock is held for the stage and released, once", calls[-1][1] is lock and lock.releases == 1)
    path = _j(stage_dir, RESULT_FILE)
    ok = os.path.isfile(path)
    t.check("derive-result.json written", ok)
    if ok:
        with open(path, "rb") as fh:
            raw = fh.read()
        back = StageResult.from_json(raw.decode("utf-8"))
        t.check("it parses back into a StageResult, every host path in the host's form",
                back.sqlite_dir.startswith("C:/fake/") and back.fixture_recipe.tus.startswith("C:/fake/"))
        t.check("it is written with LF line ends", b"\r\n" not in raw)
    rc, o, e = run(["--out", stage_dir, "--tier", "veryquick"], lock=_FakeLock())
    t.check("a usage error exits 2", rc == 2 and "--stage-build-json" in e, e)
    bad = _w(_j(tmp, "bad.json"), "{nope")
    rc, o, e = run(["--out", stage_dir, "--stage-build-json", bad, "--tier", "veryquick"], lock=_FakeLock())
    t.check("an unreadable stage-build answer is refused (exit 1)", rc == 1 and "did not print the JSON" in e, e)
    rc, o, e = run(["--out", stage_dir, "--sqlite-dir", "$HOME/sq", "--stage-build-json", sbj, "--tier", "veryquick"],
                   lock=_FakeLock())
    t.check("--sqlite-dir's $HOME is THIS side's home (the old derive's bash expansion)",
            rc == 0 and calls[-1][0].sqlite_dir == _abs(_j(tmp, "home", "sq")), calls[-1][0].sqlite_dir)
    rc, o, e = run(["--out", stage_dir, "--sqlite-dir", "~/sq2", "--stage-build-json", sbj, "--tier", "veryquick"],
                   lock=_FakeLock())
    t.check("... and a leading ~ too", rc == 0 and calls[-1][0].sqlite_dir == _abs(_j(tmp, "home", "sq2")))
    rc, o, e = run([a for a in base if a not in ("--sqlite-dir", _j(tmp, "clone"))], lock=_FakeLock())
    t.check("no --sqlite-dir means the POSIX side's own $HOME/src/sqlite",
            rc == 0 and calls[-1][0].sqlite_dir == _abs(_j(tmp, "home", "src", "sqlite")), calls[-1][0].sqlite_dir)
    shutil.rmtree(tmp, ignore_errors=True)


def _st_git(t):
    with t.arm("git"):
        git = shutil.which("git")
        if not git:
            t.skip("clone_or_update / default_branch against local repositories", "git is not on PATH")
            return
        tmp = _tmp("git")
        env = _hermetic_git_env(tmp)
        bare, work = _make_origin(tmp, env, {"configure": ("#!/bin/sh\n", 0o755), "README": ("x\n", None)})
        log = C.Log(io.StringIO())
        dest = _j(tmp, "dest", "sqlite")
        t.check("a fresh clone answers no warning", clone_or_update(bare, dest, log=log, env=env) == [])
        t.check("a fresh clone lands on origin's default branch (trunk, not main)",
                _git(env, "-C", dest, "rev-parse", "--abbrev-ref", "HEAD") == "trunk")
        t.check("the clone was announced", "cloning " in log.stream.getvalue())
        _git(env, "-C", dest, "checkout", "--quiet", "-b", "feature")
        _w(_j(work, "README"), "y\n")
        _git(env, "-C", work, "commit", "--quiet", "-am", "c2")
        _git(env, "-C", work, "push", "--quiet", "origin", "trunk")
        c2 = _git(env, "-C", work, "rev-parse", "HEAD")
        clone_or_update(bare, dest, log=log, env=env)
        t.check("an existing clone on a FEATURE branch is put back on the default branch",
                _git(env, "-C", dest, "rev-parse", "--abbrev-ref", "HEAD") == "trunk")
        t.check("... and pulled to origin's new commit", _git(env, "-C", dest, "rev-parse", "HEAD") == c2)
        t.check("default_branch resolves origin/HEAD locally", default_branch(dest, env) == "trunk")
        _git(env, "-C", dest, "remote", "set-head", "origin", "-d")
        # a newer git's fetch would re-create origin/HEAD and bypass the fallback under test
        _git(env, "-C", dest, "config", "remote.origin.followRemoteHEAD", "never")
        t.check("with origin/HEAD unset the local answer is empty (the fallback is needed)", default_branch(dest, env) == "")
        _git(env, "-C", dest, "checkout", "--quiet", "feature")
        clone_or_update(bare, dest, log=log, env=env)
        t.check("the `remote show origin` fallback still reaches the default branch",
                _git(env, "-C", dest, "rev-parse", "--abbrev-ref", "HEAD") == "trunk")
        t.check("... and it WAS the fallback (origin/HEAD is still unset)", default_branch(dest, env) == "")
        head = git_head_short(dest, env)
        t.check("git_head_short names the commit", c2.startswith(head) and len(head) >= 7, head)
        t.check("git_head_branch names the branch", git_head_branch(dest, env) == "trunk")
        _git(env, "-C", work, "checkout", "--quiet", "-b", "wanted")
        _git(env, "-C", work, "push", "--quiet", "origin", "wanted")
        clone_or_update(bare, dest, want="wanted", log=log, env=env)
        t.check("a WANTED branch (DSS_BRANCH for a fresh DSS clone) is checked out instead of the default",
                git_head_branch(dest, env) == "wanted")
        _git(env, "-C", dest, "checkout", "--quiet", "--detach")
        t.check("a detached HEAD reads DETACHED-HEAD", git_head_branch(dest, env) == "DETACHED-HEAD")
        # ── THE PIN (2026-09-25): EXACTLY the declared commit, DETACHED; fetched only when absent; never pulled ──
        pinned = _j(tmp, "dest", "pinned")
        t.check("a PINNED fresh clone answers no warning", clone_or_update(bare, pinned, log=log, env=env,
                                                                          commit=c2) == [])
        t.check("... and sits on EXACTLY the pin, detached",
                _git(env, "-C", pinned, "rev-parse", "HEAD") == c2
                and git_head_branch(pinned, env) == "DETACHED-HEAD")
        _git(env, "-C", work, "checkout", "--quiet", "trunk")
        _w(_j(work, "README"), "z\n")
        _git(env, "-C", work, "commit", "--quiet", "-am", "c3")
        _git(env, "-C", work, "push", "--quiet", "origin", "trunk")
        c3 = _git(env, "-C", work, "rev-parse", "HEAD")
        clone_or_update(bare, pinned, log=log, env=env, commit=c2)
        t.check("a pinned clone is NOT moved when origin moves on (nothing is pulled)",
                _git(env, "-C", pinned, "rev-parse", "HEAD") == c2)
        t.check("... and a pin the clone already holds fetches NOTHING (origin's new commit is not in it)",
                C.capture(["git", "-C", pinned, "cat-file", "-e", c3 + "^{commit}"], env_=env,
                          timeout=120).rc != 0)
        clone_or_update(bare, pinned, log=log, env=env, commit=c3)
        t.check("a pin the clone LACKS is fetched, then checked out", _git(env, "-C", pinned, "rev-parse", "HEAD") == c3)
        died, msg, _ = _dies(clone_or_update, bare, pinned, log=log, env=env, commit="de" * 20)
        t.check("a pin origin does not have is REFUSED, naming it", died and "de" * 20 in msg, msg[:200])
        pinned_tree = _j(tmp, "pinned-tarball")
        _w(_j(pinned_tree, "configure"), "#!/bin/sh\n", 0o755)
        died, msg, _ = _dies(clone_or_update, bare, pinned_tree, log=log, env=env, commit=c2)
        t.check("a source tree that is not a checkout is REFUSED under a pin (it cannot be put on a commit)",
                died and "PINNED" in msg and not os.path.exists(_j(pinned_tree, ".git")), msg[:200])
        _git(env, "-C", dest, "checkout", "--quiet", "trunk")
        _git(env, "-C", dest, "remote", "set-url", "origin", _j(tmp, "no-such-origin.git"))
        died, msg, _ = _dies(clone_or_update, bare, dest, log=log, env=env)
        t.check("a failing fetch is REFUSED (never a silent stale checkout)", died and "git fetch" in msg, msg[:160])
        plain = _j(tmp, "plain")
        os.makedirs(plain)
        t.check("no .git: UNKNOWN(no .git under ...)", git_head_short(plain, env) == "UNKNOWN(no .git under %s)" % plain)
        _w(_j(plain, ".git"), "gitdir: /nowhere/at/all\n")
        t.check("a broken .git FILE: UNKNOWN(rev-parse HEAD failed in ...)",
                git_head_short(plain, env) == "UNKNOWN(rev-parse HEAD failed in %s)" % plain)
        tarball = _j(tmp, "tarball")
        _w(_j(tarball, "configure"), "#!/bin/sh\n", 0o755)
        w = clone_or_update(bare, tarball, log=log, env=env)
        t.check("a non-git source tree with ./configure is used AS-IS, with a warning returned",
                not os.path.exists(_j(tarball, ".git")) and len(w) == 1 and "as-is" in w[0], w)
        junk = _j(tmp, "junk")
        _w(_j(junk, "notes.txt"), "mine\n")
        died, msg, _ = _dies(clone_or_update, bare, junk, log=log, env=env)
        t.check("a populated non-checkout without ./configure is NOT cloned over",
                died and "refusing to clone over it" in msg and os.path.isfile(_j(junk, "notes.txt")))
        shutil.rmtree(tmp, ignore_errors=True)


def _st_posix_only(t):
    """The arms that need sh / tclsh / cc / make: counted, and a NAMED skip where the tool is absent."""
    have_sh = _posix() and bool(shutil.which("sh"))
    with t.arm("sourcing"):
        if not have_sh:
            t.skip("tclConfig.sh values are read by SOURCING it", "needs a POSIX `sh` (this module runs on the POSIX side)")
        else:
            tmp = _tmp("src")
            cfgf = _w(_j(tmp, "tclConfig.sh"),
                      "TCL_MAJOR=8\nTCL_MINOR=6\nTCL_VERSION=\"$TCL_MAJOR.$TCL_MINOR\"\n"
                      "TCL_LIBS='-ldl -lz  -lm'\nTCL_LIB_FILE='libtcl8.6.so'\necho noise\n")
            ctx, _log = _ctx(tmp)
            v = tcl_config_values(ctx, cfgf, ("TCL_VERSION", "TCL_LIBS", "TCL_LIB_FILE", "TCL_ABSENT"))
            t.check("a value BUILT from other variables comes out expanded (a parser would say $TCL_MAJOR...)",
                    v["TCL_VERSION"] == "8.6", v)
            t.check("inner spacing is kept", v["TCL_LIBS"] == "-ldl -lz  -lm")
            t.check("an unset name answers ''", v["TCL_ABSENT"] == "")
            t.check("the file's own stdout does not leak into a value", "noise" not in "".join(v.values()))
            bad = _w(_j(tmp, "broken.sh"), "if then fi (\n")
            t.check("an unsourceable file answers '' for everything",
                    tcl_config_values(ctx, bad, ("TCL_VERSION",)) == {"TCL_VERSION": ""})
            t.check("a missing file answers ''", tcl_config_values(ctx, _j(tmp, "nope.sh"), ("TCL_VERSION",)) == {"TCL_VERSION": ""})
            died, _m, _c = _dies(tcl_config_values, ctx, cfgf, ("bad name; rm",))
            t.check("a name that is not a shell identifier is refused (it is spliced into sh text)", died)
            shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("tcl_choice"):
        # The Mac's case, built from FAKE interpreters so every POSIX host proves it: PATH's tclsh
        # reports 8.5, an installed Tcl 9.0 answers from its own prefix, brew is off the PATH.
        if not have_sh:
            t.skip("choose_tclsh / ensure_tclsh / the darwin dev files over FAKE interpreters",
                   "needs a POSIX `sh` (this module runs on the POSIX side)")
        else:
            tmp = _tmp("tc")
            _w(_j(tmp, "old", "tclsh"), "#!/bin/sh\necho 8.5\n", 0o755)
            keg = _j(tmp, "keg")
            new_sh = _w(_j(keg, "bin", "tclsh9.0"), "#!/bin/sh\necho 9.0\n", 0o755)
            cfgf = _w(_j(keg, "lib", "tclConfig.sh"), "TCL_VERSION='9.0'\nTCL_EXEC_PREFIX='%s'\n" % keg)
            base_path = os.pathsep.join([_j(tmp, "old"), "/usr/bin", "/bin"])

            def ctx_for(path, host_os="linux", roots=(_j(keg, "lib"),)):
                got = []
                c, _l = _ctx(tmp, environ=dict(os.environ, PATH=path), host_os=host_os,
                             pkg_install=lambda a, b=None: got.append((a, b)))
                c.cfg_roots = tuple(roots)
                c.inventory = tcl_inventory(c)
                return c, got
            ctx, installs = ctx_for(base_path)
            t.check("an older PATH tclsh (8.5) gives way to the INSTALLED Tcl 9.0 whose own interpreter answers",
                    choose_tclsh(ctx) == (new_sh, "9.0", "installed"), choose_tclsh(ctx))
            chosen = ensure_tclsh(ctx)
            t.check("...ensure_tclsh USES it -- nothing installed -- and configure is told its tclsh and tclConfig.sh",
                    chosen == (new_sh, cfgf) and installs == []
                    and tcl_configure_args(*chosen) == ["--with-tclsh=" + new_sh, "--with-tcl=" + _j(keg, "lib")],
                    (chosen, installs))
            t.check("...and the plain name `tclsh` on the run's PATH now answers 9.0 (the pin shim)",
                    tclsh_version(ctx) == "9.0", tclsh_version(ctx))
            ok_dir = _j(tmp, "ok")
            _w(_j(ok_dir, "tclsh"), "#!/bin/sh\necho 8.6\n", 0o755)
            ctx2, _i2 = ctx_for(ok_dir + os.pathsep + base_path)
            t.check("CONTROL: a PATH tclsh >= 8.6 is used as it is, and configure is told nothing",
                    choose_tclsh(ctx2)[1:] == ("8.6", "path") and tcl_configure_args(*ensure_tclsh(ctx2)) == [],
                    choose_tclsh(ctx2))
            ctx3, _i3 = ctx_for(base_path, roots=(_j(tmp, "no-such-root"),))
            t.check("with no installed Tcl >= 8.6 the choice is `none` -- the caller installs",
                    choose_tclsh(ctx3)[2] == "none", choose_tclsh(ctx3))
            zkeg = _j(tmp, "zlib-keg")
            _w(_j(zkeg, "include", "zlib.h"), "/* zlib */\n")
            libz = _w(_j(zkeg, "lib", "libz.a"), "")
            brewbin = _j(tmp, "brewbin")
            fake_brew = _w(_j(brewbin, "brew"),
                           '#!/bin/sh\n[ "$1" = --prefix ] && [ "$2" = zlib ] && { echo "%s"; exit 0; }\n'
                           'echo "%s/$2"\n' % (zkeg, _j(tmp, "no-such-keg")), 0o755)
            ctxd, inst = ctx_for(brewbin + os.pathsep + base_path, host_os="darwin")
            ensure_tclsh(ctxd)
            ensure_dev_headers(ctxd)
            t.check("darwin: a host whose Tcl has its tclConfig.sh and whose Homebrew zlib is there installs NOTHING",
                    inst == [], inst)
            os.remove(libz)
            ctxz, instz = ctx_for(brewbin + os.pathsep + base_path, host_os="darwin")
            ensure_tclsh(ctxz)
            ensure_dev_headers(ctxz)
            t.check("CONTROL: without Homebrew's libz, exactly zlib is installed (and Tcl is not)",
                    instz == [("zlib1g-dev", "zlib")], instz)
            saved = globals()["BREW_CANDIDATES"]
            globals()["BREW_CANDIDATES"] = ("brew", fake_brew)
            try:
                ctxb, _ib = ctx_for(base_path)
                found = resolve_brew(ctxb.which)
            finally:
                globals()["BREW_CANDIDATES"] = saved
            t.check("brew OFF the PATH is found at a default prefix (a candidate path), as the Mac's ssh PATH needs",
                    found == fake_brew and not ctxb.which("brew"), found)
            shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("tclsh_real"):
        why = _posix_skip_reason(have_sh, ("tclsh",))
        if why:
            t.skip("the real tclsh and Tcl inventory", why)
        else:
            tmp = _tmp("tr")
            ctx, _log = _ctx(tmp)
            discover_roots(ctx)
            ctx.inventory = inv = tcl_inventory(ctx)
            sh, ver, how = choose_tclsh(ctx)
            t.check("the tclsh the stage would run (PATH's when it reports >= 8.6, else the newest INSTALLED Tcl "
                    "whose own interpreter answers) reports a version >= 8.6",
                    awk_num(ver) >= 8.6 and how != "none",
                    "%r via %s (%s)%s" % (ver, sh or "nothing", how, _host_tcl_facts(ctx)))
            if not inv:
                t.skip("the inventory holds the tclsh's own installation", "no tclConfig.sh under the CFG roots")
            else:
                t.check("the inventory is in code-point path order", [c for _v, c in inv] == sorted(c for _v, c in inv))
                t.check("every entry's version is what sourcing it says",
                        all(tcl_config_values(ctx, c, ("TCL_VERSION",))["TCL_VERSION"] == v for v, c in inv))
                sel = select_tcl(inv, "", ver)
                t.check("that tclsh's own installation is selected", sel[0] == ver and sel[2] == "tclsh",
                        "%r%s" % (sel, _host_tcl_facts(ctx)))
            shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("probe_link"):
        why = _posix_skip_reason(have_sh, ("cc",))
        if why:
            t.skip("probe_link_l links for real", why)
        else:
            cc = shutil.which("cc")
            tmp = _tmp("pl")
            ctx, _log = _ctx(tmp)
            t.check("-lm links", probe_link_l(ctx, [cc], ["-lm"]))
            t.check("a library that does not exist does not", not probe_link_l(ctx, [cc], ["-lno_such_lib_p4s2"]))
            dirs = ldconfig_dirs(ctx, "m")
            if not dirs:
                t.skip("dir_holds_lib finds libm where ldconfig's cache says it is", "no ldconfig cache entry for libm")
            else:
                t.check("dir_holds_lib finds libm where ldconfig's cache says it is",
                        any(dir_holds_lib(d, "m") for d in dirs), dirs)
            t.check("... and not a library that is nowhere", not any(dir_holds_lib(d, "no_such_lib_p4s2") for d in dirs))
            t.check("the compiler names its own library search dirs", len(compiler_search_dirs(ctx, [cc])) > 0)
            shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("pin_exec"):
        why = _posix_skip_reason(have_sh, ("tclsh",))
        if why:
            t.skip("the pin shim EXECUTES the pinned interpreter", why)
        else:
            tmp = _tmp("pe")
            fake_bin = _j(tmp, "bin")
            _w(_j(fake_bin, "tclsh9.9"), "#!/bin/sh\ncat >/dev/null\necho 9.9\n", 0o755)
            env = dict(os.environ)
            env["PATH"] = fake_bin + os.pathsep + env.get("PATH", "")
            ctx, log = _ctx(tmp, environ=env, tcl_version="9.9")
            pin_sh, _pc = ensure_tclsh(ctx)
            t.check("the pin found tclsh9.9 BY ASKING it", pin_sh == _j(fake_bin, "tclsh9.9"), pin_sh)
            t.check("the plain name `tclsh` now IS the pinned interpreter (through the shim)", tclsh_version(ctx) == "9.9")
            t.check("the shim sits first on the RUN's PATH only", ctx.env["PATH"].startswith(_j(ctx.cfg.out_dir, "tcl-pin"))
                    and not os.environ.get("PATH", "").startswith(_j(ctx.cfg.out_dir, "tcl-pin")))
            _w(_j(fake_bin, "tclsh9.8"), "#!/bin/sh\ncat >/dev/null\necho 9.7\n", 0o755)
            ctx2, _l2 = _ctx(tmp, environ=env, tcl_version="9.8", out_dir=_j(tmp, "out2"))
            died, msg, _ = _dies(ensure_tclsh, ctx2)
            t.check("a candidate NAMED tclsh9.8 that reports 9.7 is not taken: R56 refuses",
                    died and "NO tclsh reporting 9.8 was found" in msg, msg[:160])
            shutil.rmtree(tmp, ignore_errors=True)

    with t.arm("orchestration"):
        _st_orchestration(t, have_sh)


_FAKE_CONFIGURE = r"""#!/bin/sh
# a fake sqlite configure: writes the Makefile + sqlite_cfg.h a real one would, into the cwd
top=$(cd "$(dirname "$0")" && pwd)
flags=""
ldflags=""
for a in "$@"; do
  case "$a" in
    --enable-all) flags="$flags -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE" ;;
    LDFLAGS=*) ldflags="${a#LDFLAGS=}" ;;
  esac
done
{
  printf 'CC = cc\n'
  printf 'TOP = %s\n' "$top"
  printf 'OPT_FEATURE_FLAGS = %s $(OPTIONS)\n' "$flags"
  printf 'TCL_CONFIG_SH = %s/fake-tclConfig.sh\n' "$top"
  printf 'LDFLAGS.configure = %s\n' "$ldflags"
  printf 'include $(TOP)/main.mk\n'
} > Makefile
printf '#define HAVE_FAKE 1\n' > sqlite_cfg.h
echo "configure $*" >> "$top/../configure-calls.log"
"""

_FAKE_MAIN_MK = (
    "all:\n\t@true\n"
    "sqlite3.c shell.c tclsqlite3.c:\n\t@echo amalg-$@ >> $(TOP)/../make-calls.log\n\t@printf '/* %s */\\n' $@ > $@\n"
    "testfixture:\n\t@echo testfixture >> $(TOP)/../make-calls.log\n\t@test ! -f $(TOP)/FAIL_FIXTURE\n"
    "\t@printf '/* parse */\\n' > parse.c\n\t@printf '/* hdr */\\n' > sqlite3.h\n"
    "\t@printf '#!/bin/sh\\necho fixture\\n' > $@ && chmod +x $@\n"
    "sqlite3d:\n\t@echo sqlite3d >> $(TOP)/../make-calls.log\n"
    "\t@printf '#!/bin/sh\\necho cli\\n' > $@ && chmod +x $@\n")


def _stub_base(record):
    m = types.ModuleType("sqlite_base")

    class RecipeRefused(Exception):
        pass

    class HarnessUsageError(Exception):
        pass

    RecipeResult = collections.namedtuple("RecipeResult", "summary tus defines includes drops recipe_file")

    def emit_recipe(**kw):
        record.append(kw)
        bld = kw["build_dir"]
        if os.path.exists(_j(bld, kw["make_target"])):
            record.append(("TARGET PRESENT", kw["make_target"]))   # the load-bearing rm was skipped
        clone = posixpath.dirname(bld)
        tus = [_j(clone, "src/main.c"), _j(clone, "ext/misc/m.c"), _j(bld, "parse.c")]
        defs = list(record_defs.get(kw["make_target"], []))
        if kw["make_target"] == "sqlite3d":
            tus.append(_j(bld, "shell.c"))
        incs = [_j(clone, "src"), _j(clone, "ext/rtree")] + list(record_incs)
        write_list(kw["out_tus"], tus)
        write_list(kw["out_defines"], defs)
        write_list(kw["out_includes"], incs)
        _w(kw["recipe_file"], "fake make -n output\n")
        return RecipeResult("%s: %d TUs, %d defines, %d -I dirs (mode %s)" % (
            kw["make_target"], len(tus), len(defs), len(incs), kw["prereq_mode"]), tus, defs, incs, [], kw["recipe_file"])

    record_defs = {}
    record_incs = []
    m.RecipeRefused, m.HarnessUsageError, m.RecipeResult, m.emit_recipe = RecipeRefused, HarnessUsageError, RecipeResult, emit_recipe
    m.defs, m.incs = record_defs, record_incs
    return m


def _stub_coherence(record, rc=0):
    m = types.ModuleType("sqlite_coherence")

    def run_check(dirs, checkout=None, require_cli=False, label=None, out=None, err=None):
        record.append((list(dirs), checkout, require_cli, label))
        return m.rc

    m.rc = rc
    m.run_check = run_check
    return m


def _stub_procs(locks):
    m = types.ModuleType("sqlite_procs")

    def CloneLock(clone):
        lk = _FakeLock()
        locks.append((clone, lk))
        return lk

    m.CloneLock = CloneLock
    return m


def _posix_skip_reason(have_sh, tools):
    """None when an arm that needs the POSIX side and `tools` can run here, else the NAMED reason."""
    if not have_sh:
        return "this host is not the POSIX side (no `sh`); the POSIX half runs there -- run this self-test in WSL"
    absent = [n for n in tools if not shutil.which(n)]
    return ("absent on PATH: %s" % ", ".join(absent)) if absent else None


def _host_tcl_facts(ctx):
    """What a failing HOST arm prints -- the only way to read a host reached through a runner:
    the PATH, the tclsh on it and what it reports, every installed Tcl with its own interpreter,
    where brew is and its keg prefixes, Homebrew's zlib, and the SDK. One line each, indented
    under the FAIL line so the driver's Step 0 report carries them."""
    lines = ["PATH: %s" % ctx.env.get("PATH", "")]
    path_sh = ctx.which("tclsh") or ""
    lines.append("tclsh on PATH: %s%s" % (path_sh or "none",
                                          (" reports %r" % tclsh_version(ctx)) if path_sh else ""))
    lines.append("CFG roots: %s" % " ".join(ctx.cfg_roots))
    for ver, cfg in (ctx.inventory or tcl_inventory(ctx)):
        pfx = tcl_config_values(ctx, cfg, ("TCL_EXEC_PREFIX",))["TCL_EXEC_PREFIX"]
        lines.append("installed Tcl %s: %s (TCL_EXEC_PREFIX %r) -> its own interpreter: %s"
                     % (ver, cfg, pfx, tclsh_bin_for(ctx, ver) or "none that answers"))
    lines.append("brew: %s (candidates: %s)" % (resolve_brew(ctx.which) or "none", ", ".join(
        "%s %s" % (c, "found" if ctx.which(c) else "absent") for c in BREW_CANDIDATES)))
    for f in KEG_FORMULAE:
        p = brew_prefix(ctx, f)
        lines.append("brew --prefix %s: %r (%s)" % (f, p, "a directory" if p and os.path.isdir(p) else "absent"))
    lines.append("Homebrew zlib (zlib.h + libz): %s" % (zlib_keg(ctx) or "none"))
    lines.append("xcrun SDK: %s" % (sdk_prefix(ctx) or "none"))
    return "".join("\n        " + ln for ln in lines)


def _st_orchestration(t, have_sh):
    why = _posix_skip_reason(have_sh, ("git", "make", "tclsh", "cc"))
    if why:
        t.skip("stage() + the derive CLI end to end over a FAKE sqlite", why)
        return
    # ONE full inventory walk (the tclsh_real arm proves it); every stage() below then searches only
    # the chosen installation's own directory -- the full walk crosses a Windows mount inside WSL.
    probe = _tmp("orch-inv")
    pctx, _pl = _ctx(probe)
    chosen = select_tcl(tcl_inventory(pctx), "", tclsh_version(pctx))[1]
    shutil.rmtree(probe, ignore_errors=True)
    if not chosen:
        t.skip("stage() + the derive CLI end to end over a FAKE sqlite", "no tclConfig.sh for the tclsh on PATH")
        return
    saved = globals()["BASE_CFG_ROOTS"]
    globals()["BASE_CFG_ROOTS"] = (posixpath.dirname(chosen),)
    try:
        _st_orchestration_body(t)
    finally:
        globals()["BASE_CFG_ROOTS"] = saved


def _st_orchestration_body(t):
    tmp = _tmp("orch")
    env = _hermetic_git_env(tmp)
    files = {"configure": (_FAKE_CONFIGURE, 0o755), "main.mk": (_FAKE_MAIN_MK, None),
             "fake-tclConfig.sh": ("TCL_LIBS='-lm -lno_such_lib_p4s2'\n", None),
             "src/main.c": ("/* main */\n", None), "ext/misc/m.c": ("/* m */\n", None),
             "ext/rtree/r.c": ("/* r */\n", None), "ext/fts5/test/f.test": ("# f\n", None),
             "ext/session/s.c": ("/* s */\n", None), "test/veryquick.test": ("# vq\n", None)}
    bare, _work = _make_origin(_j(tmp, "o"), env, files, branch="master")
    # the stage is PINNED (2026-09-25): here, to the fake origin's one commit
    sb_pin = dict(_GOOD_SB, sqliteCommit=_git(env, "-C", _work, "rev-parse", "HEAD"))
    clone = _j(tmp, "clone")
    installs = []
    emits, cohs, locks = [], [], []
    base = _stub_base(emits)
    req = _GOOD_SB["requiredDefines"]
    base.defs["testfixture"] = req
    base.defs["sqlite3d"] = req + ["SQLITE_CORE"]
    coh = _stub_coherence(cohs)
    procs = _stub_procs(locks)
    tcl_inc_real = ""
    with _stub_modules(sqlite_base=base, sqlite_coherence=coh, sqlite_procs=procs):
        cfg = StageConfig(sqlite_dir=clone, out_dir=_j(tmp, "out"), stage_build=sb_pin,
                          sqlite_repo_url=bare, jobs=2, host_os=C.host_os(), environ=env,
                          pkg_install=lambda a, b=None: installs.append((a, b)))
        log = C.Log(io.StringIO())
        r = stage(cfg, log)
        text = log.stream.getvalue()
        bld = _j(clone, "bld-dss")
        calls = read_list(_j(clone, "..", "make-calls.log")) if os.path.exists(_j(tmp, "make-calls.log")) else []
        t.check("make ran in the .sh's ORDER: amalgamation, reference fixture, reference CLI",
                calls == ["amalg-sqlite3.c", "amalg-shell.c", "amalg-tclsqlite3.c", "testfixture", "sqlite3d"], calls)
        t.check("the lock was taken for WRITE and released (lock=None)", len(locks) == 1 and locks[0][1].writes == 1 and locks[0][1].releases == 1)
        t.check("the target was MISSING at every recipe derivation (the load-bearing rm)",
                not any(isinstance(e, tuple) for e in emits))
        fx = [e for e in emits if isinstance(e, dict) and e["make_target"] == "testfixture"]
        cl = [e for e in emits if isinstance(e, dict) and e["make_target"] == "sqlite3d"]
        exp_common = dict(prereq_mode="link-line", always_make=True, token_scope="recipe",
                          search_roots=(_j(clone, "src"), _j(clone, "ext"), bld), archive=_j(bld, "libsqlite3.a"))
        t.check("the fixture recipe got EXACTLY the .sh's flags", len(fx) == 1 and all(fx[0][k] == v for k, v in exp_common.items())
                and fx[0]["make_vars"] == ("USE_AMALGAMATION=0", "OPTIONS=-DSQLITE_ENABLE_STAT4")
                and fx[0]["archive_from_span"] is False and (fx[0]["min_tus"], fx[0]["min_defines"]) == (150, 18), fx)
        t.check("the CLI recipe got EXACTLY the .sh's flags", len(cl) == 1 and all(cl[0][k] == v for k, v in exp_common.items())
                and cl[0]["make_vars"] == ("OPTIONS=-DSQLITE_ENABLE_STAT4",)
                and cl[0]["archive_from_span"] is True and (cl[0]["min_tus"], cl[0]["min_defines"]) == (100, 18), cl)
        t.check("the coherence gate ran on the build dir WITH --require-cli against the checkout",
                cohs == [([bld], clone, True, "staged sqlite (Step 4)")], cohs)
        t.check("both oracles preserved and executable", r.reference_fixture == _j(tmp, "out", "reference-testfixture")
                and os.access(r.reference_fixture, os.X_OK) and os.access(r.reference_cli, os.X_OK))
        t.check("... and deleted from the make targets' paths",
                not os.path.exists(_j(bld, "testfixture")) and not os.path.exists(_j(bld, "sqlite3d")))
        t.check("the amalgamation regenerated", r.amalgamation_regen == "ok")
        t.check("an ABSENT stamp fired: the build dir was rebuilt, said out loud", any("rebuilt from scratch" in w for w in r.warnings), r.warnings)
        t.check("the stamp was written", read_stamp(_j(bld, STAGE_STAMP)) == r.stage_identity and r.stage_identity.startswith("tclsh="))
        t.check("an unresolvable TCL_LIBS -l is a reference-link WARNING, not a stop",
                any("no_such_lib_p4s2" in w for w in r.ref_link_warnings), r.ref_link_warnings)
        def host_facts():
            fctx = _Ctx(cfg, C.Log(io.StringIO()))
            discover_roots(fctx)
            fctx.inventory = tcl_inventory(fctx)
            return _host_tcl_facts(fctx)
        t.check("nothing was installed on a host that has it all", installs == [],
                ("%r%s" % (installs, host_facts())) if installs else "")
        t.check("the Tcl header dir holds tcl.h and agrees with the version",
                os.path.isfile(_j(r.tcl_inc, "tcl.h")) and tcl_h_version(_j(r.tcl_inc, "tcl.h")) == r.tcl_version)
        t.check("tcl_lib_file is the chosen tclConfig.sh's TCL_LIB_FILE",
                r.tcl_lib_file == tcl_config_values(_Ctx(cfg, log), r.tcl_config_posix, ("TCL_LIB_FILE",))["TCL_LIB_FILE"])
        t.check("zlib's pair is in <bld>/zinc-src", os.path.isfile(_j(r.zinc_src, "zlib.h")) and os.path.isfile(_j(r.zinc_src, "zconf.h")))
        t.check("sqlite_cfg.h is where the result says", r.sqlite_cfg_h == _j(bld, "sqlite_cfg.h") and os.path.isfile(r.sqlite_cfg_h))
        t.check("sqlite_head is a short sha", re.match(r"^[0-9a-f]{7,}$", r.sqlite_head) is not None, r.sqlite_head)
        t.check("the in-place result names the LIVE tree", (r.bld, r.src, r.testdir) == (bld, _j(clone, "src"), _j(clone, "test")))
        t.check("the derived recipe files carry the stub's lists", read_list(r.fixture_recipe.tus)[0] == _j(clone, "src/main.c")
                and r.fixture_recipe.n_tus == 3 and r.cli_recipe.n_tus == 4)
        t.check("the progress went to the log", "sqlite ready" in text and "recipe: testfixture" in text)
        tcl_inc_real = r.tcl_inc

        # the derive CLI over the SAME clone: stamp kept, staged, translated, gated twice
        del cohs[:]
        base.incs[:] = [tcl_inc_real]
        sbj = _w(_j(tmp, "sb.json"), json.dumps(sb_pin))
        stage_dir = _j(tmp, "stage")
        lk = _FakeLock()
        out, err = io.StringIO(), io.StringIO()
        rc = derive_main(["--out", stage_dir, "--sqlite-dir", clone, "--stage-build-json", sbj, "--tier", "veryquick",
                          "--sqlite-repo-url", bare, "--jobs", "2"],
                         lock_factory=lambda c: lk, translator=lambda p: "W:" + p, out=out, err=err, environ=env,
                         host_os=C.host_os())
        t.check("the derive exits 0", rc == 0, (err.getvalue()[-600:] + host_facts()) if rc != 0 else "")
        if rc == 0:
            d = StageResult.from_json(open(_j(stage_dir, RESULT_FILE), encoding="utf-8").read())
            t.check("the stamp matched: no rebuild this time", not any("rebuilt from scratch" in w for w in d.warnings), d.warnings)
            t.check("every host path is in the host's spelling",
                    all(getattr(d, k).startswith("W:") for k in StageResult.HOST_PATHS))
            t.check("bld / tcl_inc / testdir are the STAGED ones",
                    (d.bld, d.tcl_inc, d.testdir) == ("W:" + _j(stage_dir, "sqlite/bld"), "W:" + _j(stage_dir, "tclinc"),
                                                      "W:" + _j(stage_dir, "sqlite/test")))
            stl = read_list(_j(stage_dir, "tus.staged.txt"))
            t.check("the staged fixture list is the derived one, remapped and spelled for the host",
                    stl == ["W:" + _j(stage_dir, "sqlite/src/main.c"), "W:" + _j(stage_dir, "sqlite/ext/misc/m.c"),
                            "W:" + _j(stage_dir, "sqlite/bld/parse.c")], stl)
            sti = read_list(_j(stage_dir, "recipe-includes.staged.txt"))
            t.check("the staged -I list: recipe dirs only, the Tcl dir -> tclinc, no bld prepended",
                    sti == ["W:" + _j(stage_dir, "sqlite/src"), "W:" + _j(stage_dir, "sqlite/ext/rtree"),
                            "W:" + _j(stage_dir, "tclinc")], sti)
            t.check("the defines file is carried unchanged", d.fixture_recipe.defines == "W:" + _j(stage_dir, "defines.base.txt"))
            t.check("the reference CLI in both spellings", d.reference_cli == "W:" + _j(stage_dir, "reference-sqlite3")
                    and d.reference_cli_posix == _j(stage_dir, "reference-sqlite3"))
            t.check("the gate ran on the build dir AND on the staged copy",
                    [c[3] for c in cohs] == ["staged sqlite (Step 4)", "staged sqlite (Step 4, staged copy)"]
                    and cohs[1][0] == [_j(stage_dir, "sqlite/bld"), _j(stage_dir, "sqlite/src")], cohs)
            t.check("the derive's own lock was held and released once", lk.writes == 1 and lk.releases == 1)
            t.check("clone-lock notes ride into the result", d.clone_lock_notes == lk.notes)

        # a tolerated failure: the reference fixture does not link, the run goes on
        _w(_j(clone, "FAIL_FIXTURE"), "x\n")
        base.incs[:] = []
        log = C.Log(io.StringIO())
        r2 = stage(cfg, log)
        t.check("a reference fixture that does not link is TOLERATED, with its reason",
                r2.reference_fixture is None and "did not fully link" in r2.reference_fixture_why, r2.reference_fixture_why)
        t.check("... and the recipes were still derived", r2.fixture_recipe.n_tus == 3)
        os.remove(_j(clone, "FAIL_FIXTURE"))

        # negatives through the same pipeline
        coh.rc = 1
        died, msg, _ = _dies(stage, cfg, C.Log(io.StringIO()))
        t.check("an INCOHERENT tree stops the run", died and "INCOHERENT (mixed vintage)" in msg, msg[:160])
        coh.rc = 0
        base.defs["sqlite3d"] = [x for x in req if x != "SQLITE_ENABLE_RTREE"] + ["SQLITE_CORE"]
        died, msg, _ = _dies(stage, cfg, C.Log(io.StringIO()))
        t.check("a capability missing from the CLI recipe only is refused (the asymmetry)",
                died and "the sqlite3 CLI recipe is MISSING declared capabilities: SQLITE_ENABLE_RTREE" in msg, msg[:200])
        base.defs["sqlite3d"] = req
        died, msg, _ = _dies(stage, cfg, C.Log(io.StringIO()))
        t.check("a CLI define set without SQLITE_CORE is refused", died and "no SQLITE_CORE" in msg, msg[:120])
        base.defs["sqlite3d"] = req + ["SQLITE_CORE"]
        sb_bad = dict(sb_pin, requiredDefines=req + ["SQLITE_ENABLE_NOTHING"])
        cfg_bad = StageConfig(sqlite_dir=clone, out_dir=_j(tmp, "out"), stage_build=sb_bad, sqlite_repo_url=bare,
                              jobs=2, host_os=C.host_os(), environ=env, pkg_install=lambda a, b=None: None)
        died, msg, _ = _dies(stage, cfg_bad, C.Log(io.StringIO()))
        t.check("a required define configure did not produce is refused (R65)",
                died and "missing from OPT_FEATURE_FLAGS: SQLITE_ENABLE_NOTHING" in msg, msg[:200])
    shutil.rmtree(tmp, ignore_errors=True)


def self_test(out=None):
    out = out if out is not None else sys.stdout
    t = _T(out)
    _st_pure(t)
    _st_git(t)
    _st_posix_only(t)
    if sorted(t.ran) != sorted(ARMS) or len(t.ran) != len(set(t.ran)):
        t.failed += 1
        out.write("  FAIL the arms that ran (%s) are not the declared ARMS (%s)\n" % (t.ran, list(ARMS)))
    out.write("arms=%d passed=%d failed=%d skipped=%d\n" % (len(t.ran), t.passed, t.failed, t.skipped))
    out.write("passed=%d failed=%d skipped=%d\n" % (t.passed, t.failed, t.skipped))
    return 0 if t.failed == 0 else 1


USAGE = ("usage: sqlite_stage.py derive --out <stage dir, POSIX form> [--sqlite-dir <dir>] "
         "[--sqlite-repo-url <url>] [--tcl-version V] [--jobs N] --stage-build-json <file> --tier <t> "
         "[--test-file F]\n       sqlite_stage.py --self-test\n")


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv[:1] in (["--self-test"], ["--selftest"]) and len(argv) == 1:
        return self_test()
    if argv[:1] == ["derive"]:
        try:
            return derive_main(argv[1:])
        except Exception:  # a defect of this program: say so, never a silent success
            sys.stderr.write("✗ ERROR: sqlite_stage.py derive failed unexpectedly:\n%s" % traceback.format_exc())
            return 1
    sys.stderr.write(USAGE)
    return 2


if __name__ == "__main__":
    sys.exit(main())
