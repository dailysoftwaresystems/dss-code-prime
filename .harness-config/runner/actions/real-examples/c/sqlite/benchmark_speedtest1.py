#!/usr/bin/env python3
"""benchmark_speedtest1.py -- benchmark DSS Code Prime against the reference C compilers building
and running SQLite's OWN performance program, `test/speedtest1.c`, FROM FULL SOURCE (the ~103 real
translation units of the full-source CLI, never the amalgamation).

It replaced `benchmark-speedtest1.sh` (the POSIX driver, which was also the derive half of the
Windows one) and `benchmark-speedtest1.ps1` (the Windows driver) on 2026-09-21 (lane mig, part 4:
no `.sh`/`.ps1` under the actions directory, "they are specific per OS"). ONE program on every
host, carrying the UNION of both twins' checks; where they disagreed the decision is stated below.

★★★ WHY THE SUBJECT IS DERIVED AND NOT WRITTEN DOWN. Upstream's own `speedtest1` make target is
an AMALGAMATION build (`main.mk` links `sqlite3.c`, `Makefile.msc` links `$(SQLITE3C)`; READ
2026-08-21), and a hand-kept list of 103 sources goes stale the first time upstream adds one. What
does exist is the full-source CLI target `sqlite3d$(T.exe): shell.c $(LIBOBJS0)`, whose recipe
`sqlite_base.emit_recipe` derives. speedtest1 is that program with ONE translation unit swapped --
`shell.c` out, `test/speedtest1.c` in, both a `main()` over the same library -- and the swap is
ASSERTED, never assumed.

★★ ALL ARMS COMPILE ONE DEFINE SET. The recipe comes from a POSIX `configure`, so its `-D` set
carries that configure's answers; the manifest generator applies the leg's declared recipe
transform ONCE, and the plan's sources/includes/defines are then READ BACK OUT OF THE MANIFEST and
handed to every compiler -- one set, produced by one program, or "DSS is faster" could just mean
"DSS compiled less".

★ WHAT IS NOT EQUALIZED, and the report says so: dsscp compiles every CU inside ONE process on a
worker pool (`--jobs N`); gcc/clang/cl run as N concurrent `-c` processes plus a link. That
difference IS the architecture under measurement.

THE PHASES
  RESOLVE  on the MEASURING host (the host whose compilers are timed): the subject tree; dsscp,
           chosen by the BUILD TYPE read from its own CMake tree (never by a directory name --
           the .ps1's defect); the config root (DSS_CONFIG_ROOT honoured on every host -- the
           .ps1 ignored it); the reference C compilers, discovered from ONE catalogue on every
           host (the .ps1 took the first of gcc.exe/clang.exe); the target's leg facts -- recipe
           transform, stack reserve, reference link flags -- from legs.json through harness_legs.py
           (the .sh copied them into `case` tables, and its macho table said `-lm` where legs.json
           says `-lm -lpthread`: the macho legs now link `-lpthread`, as legs.json declares); and
           the pre-flight, whose "the compiler REFUSED" (exit 1) and "the check COULD NOT RUN"
           (exit 3) stay two different answers (both twins collapsed them).
  DERIVE   where SQLite's POSIX build runs (autosetup + make + tclsh): configure; the reference
           `sqlite3d` + `libsqlite3.a` with USE_AMALGAMATION=0; the executable-suffix probe; the
           recipe (link-line, -B, recipe scope, archive recovery, floors 100/18 -- the .sh's
           call); SQLITE_CORE by name AND every `stageBuild.requiredDefines` (the .sh checked only
           SQLITE_CORE); `shell.c` -> `speedtest1.c`; the build dir appended LAST to the include
           list; the manifest. In THIS process on a POSIX host. On a Windows host inside WSL as
               wsl.exe -e python3 <this file's WSL path> --derive-only --path-style windows ...
           -- THIS program, never a second implementation -- whose manifest comes back with every
           path spelled for the Windows host (`wslpath -w`, once per path, a double translation
           REFUSED), beside `<out>/derive-result.json`.
  PLAN     on the measuring host: the plan `speedtest1_bench.py` reads (json.dump), its subject
           read back out of the manifest; on Windows every translated path must EXIST there first.
  MEASURE  `speedtest1_bench.py --plan`, natively; its exit code is this program's.

WHAT THE UNION DECIDED (each pinned by a self-test arm):
  * dsscp candidates are `sqlite_compiler.find_candidates`'s -- the fixed build roots AND every
    `build/*/bin/dss` tree (the .sh searched every `build/*/`: MEASURED 2026-08-26, the arm64 VPS
    keeps its Release tree in `build/bench-rel`; DssHarness builds each leg variant in
    `build/<processor>-<toolchain>-<config>`), one owner for the driver and this program alike --
    every one judged by `sqlite_compiler.build_type` / `select_compiler` /
    `is_release` -- the newest RELEASE wins; a non-Release one is eligible only under
    DSS_ALLOW_NONRELEASE_COMPILER and is then SAID; an explicit --dss / DSS_BIN passes the SAME
    gate and still reports its build type.
  * `CC` still pins one reference (the .sh), but the NOT PROBED note names the variable that did
    it (the .sh blamed `--cc`); a pin that is neither a file nor a name on PATH is refused.
  * Every reference that resolved but would not answer `--version` is UNUSABLE, and "none usable"
    is its own refusal (the .sh called that "a bug in the discovery block").
  * The plan writer has its own refusal ids (W1-W3; the .sh reused `R7`, which is
    speedtest1_bench's repeats refusal).
  * A UNC sqlite dir or out dir on a Windows host is refused by name before anything is spent.
  * `--path-style windows` on a POSIX host is the derive half ONLY: it needs --derive-only,
    --sqlite-dir, --out and --target, and refuses the measuring host's flags (a plan derived there
    would carry that host's compilers -- the .sh wrote `/usr/bin/gcc` into a Windows plan unless
    the .ps1 pinned `--cc`); `--path-style posix` on a Windows host is a usage error.
  * Usage errors exit 2 on every host (the .ps1 exited 1).

Refusal ids of the plan writer: W1 no reference compiler record reached the writer · W2 the
manifest is unreadable or lacks sources/includes/defines · W3 the plan cannot be written.

CLI (the .sh's flags): see USAGE below, or `--help`. Exit codes: 0 measured (or `--derive-only`
complete) · 1 a refusal · 2 usage · 3 no compiler produced a binary (the measurement core's).
Environment: SQLITE_DIR, SRC_DIR (default: the tree this file lives in), DSS_BIN, CC,
DSS_ALLOW_NONRELEASE_COMPILER, DSS_CONFIG_ROOT.

`--self-test` (alias `--selftest`): every check of both retired drivers plus the negatives they
lacked, counted against EXPECTED_ARMS; summary `passed=N failed=N skipped=N`, exit 0 only when
failed=0. Importing this module has no side effect beyond the stream block and
`sys.dont_write_bytecode` (the action is `requireInputsUnmoved`).
"""
from __future__ import annotations

import sys

# BEFORE any sibling import: the action is `requireInputsUnmoved`, so no __pycache__ may appear.
sys.dont_write_bytecode = True

import argparse  # noqa: E402
import collections  # noqa: E402
import contextlib  # noqa: E402
import importlib.util  # noqa: E402
import io  # noqa: E402
import json  # noqa: E402
import os  # noqa: E402
import re  # noqa: E402
import shutil  # noqa: E402
import subprocess  # noqa: E402
import tempfile  # noqa: E402
import time  # noqa: E402
import traceback  # noqa: E402

# A cp1252 console turns a printed glyph into a traceback; reconfigure before anything can print.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

HERE = os.path.dirname(os.path.realpath(__file__))
THIS = os.path.realpath(__file__)


def _sibling(name):
    """A sibling module of this action, loaded BY PATH and registered under its own name, so this
    file works when it is run from its directory, imported by path from anywhere else, or run
    inside WSL from `/mnt/c` -- and so the siblings' own `import sqlite_common` finds THIS copy.
    A DIFFERENT file already registered under the name is refused rather than mixed in."""
    path = os.path.join(HERE, name + ".py")
    mod = sys.modules.get(name)
    if mod is not None:
        have = getattr(mod, "__file__", "") or ""
        if have and os.path.normcase(os.path.realpath(have)) != os.path.normcase(path):
            raise ImportError("benchmark_speedtest1.py must run against its own sibling %s, but a "
                              "DIFFERENT %s is already loaded (%s)" % (path, name, have))
        return mod
    if not os.path.isfile(path):
        raise ImportError("benchmark_speedtest1.py needs its sibling %s, which is missing" % path)
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    try:
        spec.loader.exec_module(mod)
    except BaseException:
        sys.modules.pop(name, None)
        raise
    return mod


C = _sibling("sqlite_common")
COMP = _sibling("sqlite_compiler")
BASE = _sibling("sqlite_base")

_STAGE = None


def _stage():
    """`sqlite_stage` -- the owner of the stage-build shape check and of the "is this declared
    capability in the derived define set" rule -- loaded when a derivation first needs it."""
    global _STAGE
    if _STAGE is None:
        _STAGE = _sibling("sqlite_stage")
    return _STAGE


DASH = C.DASH
ARTIFACT_NAME = "speedtest1"
MAIN_TU = "speedtest1.c"
CLI_MAIN_TU = "shell.c"
CLI_TARGET_STEM = "sqlite3d"
AMALGAMATION_OBJECT = "sqlite3.o"
CORE_DEFINE = "SQLITE_CORE"
# The .sh's floors on the derived recipe: the full-source CLI is ~103 TUs and ~30 defines on every
# leg; a parse that breaks yields a SHORT list, never an error.
MIN_TUS, MIN_DEFINES = 100, 18
# What the derivation needs on the PLAIN PATH of the host it runs on (tclsh generates opcodes.c /
# parse.c; `ar` lists the archive the core TUs are recovered from). Python is this interpreter.
DERIVE_TOOLS = ("make", "tclsh", "ar")
DERIVE_RESULT = "derive-result.json"
DERIVE_SCHEMA = "dss-speedtest1-derive/1"
PLAN_FILE = "benchmark-plan.json"
MANIFEST_FILE = "speedtest1.dss-project.json"
PROBE_TARGET = "__dss_texe_probe"
VERSION_TIMEOUT = 60
DEFAULTS = {"size": 25, "build_repeats": 3, "run_repeats": 5, "jobs_arms": [1, 4]}
# The measuring host's flags: meaningless to the derive half, so refused there (never ignored).
MEASURING_HOST_FLAGS = (("dss", "--dss"), ("dss_src", "--dss-src"), ("cc", "--cc"),
                        ("plan", "--plan"), ("size", "--size"), ("testset", "--testset"),
                        ("build_repeats", "--build-repeats"), ("run_repeats", "--run-repeats"),
                        ("jobs_arms", "--jobs-arms"))
# git's repository-local variables: a hook-exported GIT_DIR must not answer for the sqlite clone.
GIT_LOCAL_ENV = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
                 "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_COMMON_DIR", "GIT_PREFIX")

# ── THE REFERENCE C COMPILERS, PLURAL ────────────────────────────────────────────────
# Every reference found is measured, each its own arm; every catalogue name NOT measured is
# printed with the state that stopped it (compile-bench's words): ABSENT (nothing by that name on
# PATH), UNUSABLE (it will not answer `--version`, so it will not compile anything either),
# DUPLICATE (a compiler already measured, by path or by version line), NOT PROBED (one reference
# was pinned). THE ORDER IS LOAD-BEARING: gcc and clang before cc, so a `cc` that IS one of them is
# the one reported DUPLICATE. `cl.exe` is not here: MSVC does not resolve by name from a plain
# process, so the measurement core resolves it (vswhere + vcvarsall) and skips it by name.
REF_CC_CATALOGUE = (
    ("gcc", "the GNU C compiler, and the reference every number in this repository's benchmark "
            "history was taken against"),
    ("clang", "LLVM's C compiler. On macOS it is also what 'gcc' and 'cc' resolve to, which is why "
              "the dedupe is by VERSION and not only by path"),
    ("cc", "the POSIX name for 'the system C compiler'. Usually a link to one of the two above, and "
           "then reported DUPLICATE; it is the ONLY name that resolves on a host shipping neither"),
    ("tcc", "the Tiny C Compiler. Rarely installed -- it is on the list so a host that HAS it "
            "yields a row instead of silence"),
)

USAGE = r"""usage: benchmark_speedtest1.py [flags]

Benchmark DSS Code Prime against the reference C compilers (the catalogue gcc, clang, cc, tcc --
whichever this host has -- and MSVC cl.exe on Windows) building and running SQLite's own
test/speedtest1.c FROM FULL SOURCE, and print the measurement.

  --sqlite-dir DIR      the SQLite checkout, used AS-IS (never switched, pulled or cleaned).
                        Default: $SQLITE_DIR, else ~/src/sqlite (POSIX) or C:\Source\sqlite
                        (Windows). On Windows it must be on a LOCAL disk: a UNC path is refused.
  --dss-src DIR         the DSS checkout whose build trees hold dsscp and whose src/dss-config is
                        the default config root. Default: $SRC_DIR, else the tree this file is in.
  --dss PATH            the dsscp to measure. Default: $DSS_BIN, else the newest RELEASE dsscp
                        under <dss-src>/build, its build type READ from its own CMake tree.
  --out DIR             every derived file and the report. Default: <sqlite-dir>/bld-dss-bench.
  --plan FILE           where the plan is written. Default: <out>/benchmark-plan.json.
  --target SPEC         the <arch>:<format> of the DSS arm. Default: the one leg of legs.json that
                        runs natively on this host.
  --recipe-transform T  override the target leg's declared recipe transform (legs.json).
  --stack-reserve N     override the target leg's declared stack reserve, in bytes (legs.json).
  --cc PATH             measure exactly ONE reference compiler, not the catalogue. Default: $CC.
  --size N              speedtest1 --size (default 25).
  --testset NAME        speedtest1 --testset (default: speedtest1's own).
  --build-repeats N     cold builds per arm and worker count (default 3).
  --run-repeats N       timed runs per arm (default 5).
  --jobs-arms "N M"     the worker counts to measure (default "1 4").
  --derive-only         stop once the plan is written; the measurement is the caller's.
  --path-style S        posix|windows. `windows` on a POSIX host makes THIS process the derive
                        half of a Windows measuring host: it needs --derive-only, --sqlite-dir,
                        --out and --target, writes <out>/derive-result.json beside a manifest
                        spelled for Windows, and refuses the measuring host's flags.
  --self-test           (alias --selftest) prove every check refuses what it must.
  -h, --help            this text.

environment: SQLITE_DIR, SRC_DIR, DSS_BIN, CC, DSS_ALLOW_NONRELEASE_COMPILER (1/true/yes: a
non-Release dsscp becomes ELIGIBLE, never preferred, and is said), DSS_CONFIG_ROOT (the directory
that CONTAINS src/dss-config; honoured on every host).
exit: 0 measured (or --derive-only complete) - 1 refused - 2 usage - 3 no compiler produced a
binary (the measurement core's)."""


class UsageError(Exception):
    """Malformed arguments: exit 2."""
    exit_code = 2


def die(msg):
    raise C.HarnessDie(msg)


# ── small I/O ─────────────────────────────────────────────────────────────────────────

def _read_list(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return [ln.strip() for ln in fh.read().splitlines() if ln.strip()]


def _write_list(path, items):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("".join("%s\n" % i for i in items))


def _write_json(path, doc):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=2)
        fh.write("\n")


def _rm_f(path):
    try:
        os.remove(path)
    except FileNotFoundError:
        pass


def _run_to_log(argv, cwd, log_path):
    """Run `argv` in `cwd`, stdout and stderr merged into `log_path` as RAW bytes -> exit code (127
    and the reason IN the log when it cannot start). Never raises on the child."""
    with open(log_path, "wb") as fh:
        try:
            return subprocess.run(list(argv), cwd=cwd, stdin=subprocess.DEVNULL, stdout=fh,
                                  stderr=subprocess.STDOUT, env=C.child_env()).returncode
        except OSError as exc:
            fh.write(("benchmark_speedtest1: could not start %s: %s\n" % (argv[0], exc))
                     .encode("utf-8"))
            return 127


# ── RESOLVE: the subject ──────────────────────────────────────────────────────────────

def default_sqlite_dir(host):
    """The retired twins' defaults, one per host: the .ps1's `C:\\Source\\sqlite`, the .sh's
    `~/src/sqlite`."""
    if host == "windows":
        return "C:\\Source\\sqlite"
    return os.path.expanduser(os.path.join("~", "src", "sqlite"))


def is_unc(path):
    return path.startswith("\\\\") or path.startswith("//")


def refuse_unc(path, host, what):
    """R4, asserted where it is still cheap. Compiling across `\\\\wsl$` (9P) costs several times
    local-disk I/O and does NOT cost every toolchain the same, so a build-TIME comparison taken
    there measures the filesystem. Only a Windows host can be handed such a path."""
    if host == "windows" and is_unc(path):
        die("%s is on a UNC share: %s\n      Compiling across \\\\wsl$ (9P) costs several times "
            "local-disk I/O and does NOT cost every toolchain\n      the same, so a build-TIME "
            "comparison taken there measures the filesystem rather than the compilers.\n      Put "
            "it on a local disk. (This refuses rather than warns.)" % (what, path))


def check_subject(sqlite_dir):
    """-> the path of test/speedtest1.c, or a refusal naming what is missing."""
    if not os.path.isdir(sqlite_dir):
        die("no SQLite checkout at %s\n      Point --sqlite-dir (or $SQLITE_DIR) at one, or clone "
            "https://github.com/sqlite/sqlite there.\n      The checkout is used AS-IS and never "
            "switched or pulled -- a probe measures the tree exactly as it stands." % sqlite_dir)
    speedtest = os.path.join(sqlite_dir, "test", MAIN_TU)
    if not os.path.isfile(speedtest):
        die("the benchmark's subject is missing: %s\n      That file IS SQLite's own performance "
            "program; without it there is nothing to measure\n      and no substitute worth "
            "inventing." % speedtest)
    return speedtest


def sqlite_head(sqlite_dir):
    """-> (short HEAD or "UNKNOWN", why-unknown). Recorded on the MEASURING host, and a failure is
    STATED (the .sh's `|| echo UNKNOWN` said nothing about why)."""
    git = shutil.which("git")
    if not git:
        return "UNKNOWN", "git is not on this host's PATH"
    r = C.capture([git, "-C", sqlite_dir, "rev-parse", "--short", "HEAD"], timeout=120,
                  env_=C.child_env({n: None for n in GIT_LOCAL_ENV}))
    head = (r.out or "").strip().splitlines()
    if r.rc == 0 and head:
        return head[0].strip(), ""
    return "UNKNOWN", ("git rev-parse --short HEAD exited %d: %s"
                       % (r.rc, C.first_lines(r.err or r.out, 1) or "(no output)"))


def require_tools(names, which=shutil.which):
    missing = [t for t in names if not which(t)]
    if missing:
        die("MISSING tool%s on this host's PATH: %s\n      SQLite's own build needs them: tclsh "
            "generates opcodes.c/parse.c, make runs the reference\n      build and the recipe dry "
            "run, ar lists the archive the core sources are recovered from.\n      (speedtest1 "
            "itself links no Tcl -- these are BUILD-HOST tools.) On a Windows host this half\n      "
            "runs under `wsl.exe -e` with NO login shell, so a tool only a login profile puts on\n"
            "      PATH is missing here too. apt: make tcl binutils. brew: make tcl-tk."
            % ("s" if len(missing) > 1 else "", " ".join(missing)))


# ── RESOLVE: the reference compilers ─────────────────────────────────────────────────

Reference = collections.namedtuple("Reference", ["id", "bin", "label", "version"])
Skip = collections.namedtuple("Skip", ["id", "state", "why"])

_LABEL_SEP = re.compile(r"[^a-z0-9_+-]")
_VERSION_WORD = re.compile(r" version .*")
_PAREN_TAIL = re.compile(r" \(.*")
_SHORT_VERSION = re.compile(r"(\d+\.\d+(?:\.\d+)?)")


def reference_label(invoked, version):
    """The name to publish for a reference: the INVOKED name when the compiler's own version line
    confirms it as a WHOLE WORD, else the compiler's self-description (`Apple clang` for a `gcc`
    that is Apple clang), else the invoked name. Whole-word, because `gcc (Ubuntu ...)` contains
    the SUBSTRING `cc`, and a `cc` that is gcc must publish as `gcc`. `.` separates words, so MinGW's
    `gcc.exe (...)` still confirms `gcc`."""
    norm = " %s " % _LABEL_SEP.sub(" ", (version or "").lower())
    if " %s " % invoked.lower() in norm:
        return invoked
    label = _PAREN_TAIL.sub("", _VERSION_WORD.sub("", version or ""))
    return label or invoked


def short_version(text):
    """The FIRST dotted number of a vendor version line -- right for all three shipped spellings
    (`gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`, MinGW's `gcc.exe (MinGW-W64 x86_64-ucrt-posix-
    seh, ...) 13.2.0`, `clang version 18.1.3 (...)`); a positional pick once published
    `x86_64-ucrt-posix-seh,` as a version."""
    m = _SHORT_VERSION.search(text or "")
    return m.group(1) if m else ""


def _invoked_id(path):
    base = os.path.basename(path)
    return base[:-4] if base.lower().endswith(".exe") else base


def version_probe(path):
    """-> (first line of `<path> --version`, why-not): the line is "" when the compiler would not
    answer (a non-zero exit, a launch failure, or no output)."""
    r = C.capture([path, "--version"], timeout=VERSION_TIMEOUT, env_=C.child_env())
    lines = (r.out or "").splitlines()
    first = lines[0].strip() if lines else ""
    if r.rc != 0:
        return "", "exit %d%s" % (r.rc, (": " + C.first_lines(r.err, 1)) if r.err.strip() else "")
    if not first:
        return "", "it exited 0 and printed nothing"
    return first, ""


def resolve_pinned(pinned, pinned_by, which=shutil.which, isfile=os.path.isfile):
    """A pinned reference (`--cc` or `CC`): a file path as given, else a name on PATH; neither is a
    refusal naming WHICH setting pinned it (a CC carrying flags names no single program)."""
    if isfile(pinned):
        return os.path.abspath(pinned)
    found = which(pinned)
    if found:
        return found
    die("the reference compiler pinned by %s ('%s') is neither a file nor a program on PATH.\n"
        "      It must name ONE program (a value carrying flags names none). Unset it to measure "
        "the\n      catalogue (%s), or name an existing compiler."
        % (pinned_by, pinned, ", ".join(n for n, _w in REF_CC_CATALOGUE)))


def discover_references(pinned="", pinned_by="", which=shutil.which, version_of=version_probe,
                        realpath=os.path.realpath):
    """-> (references measured, skips): the catalogue on EVERY host (or the one pinned
    reference), deduplicated by resolved path AND by version line, each labelled by
    `reference_label`. A pinned reference records the catalogue as NOT PROBED, naming the
    setting that pinned it. Nothing usable is a refusal that says which of the two it was."""
    skips, cands = [], []
    if pinned:
        skips.append(Skip(" ".join(n for n, _w in REF_CC_CATALOGUE), "NOT PROBED",
                          "%s pinned exactly one reference (%s), so no catalogue name was resolved"
                          % (pinned_by, pinned)))
        cands.append((_invoked_id(pinned), pinned))
    else:
        for name, why in REF_CC_CATALOGUE:
            path = which(name)
            if not path:
                skips.append(Skip(name, "ABSENT", "no '%s' resolves on PATH %s %s" % (name, DASH, why)))
                continue
            cands.append((name, path))
    if not cands:
        die("no reference C compiler resolved on this host.\n%s\n      Pass --cc <path>. A benchmark "
            "with no reference is not a comparison." % _skip_lines(skips))
    refs, seen = [], set()
    for ident, path in cands:
        real = realpath(path)
        if os.path.normcase(real) in seen:
            skips.append(Skip(ident, "DUPLICATE", "%s resolves to %s, which is already measured"
                              % (path, real)))
            continue
        ver, why_not = version_of(path)
        if not ver:
            skips.append(Skip(ident, "UNUSABLE", "found at %s but it would not answer '--version' "
                              "(%s) %s a binary that cannot do that will not compile anything "
                              "either" % (path, why_not, DASH)))
            continue
        if any(r.version == ver for r in refs):
            skips.append(Skip(ident, "DUPLICATE", "%s is the same compiler as one already measured "
                              "%s '%s'" % (path, DASH, ver)))
            continue
        seen.add(os.path.normcase(real))
        refs.append(Reference(ident, path, reference_label(ident, ver), ver))
    if not refs:
        die("no reference C compiler on this host is USABLE: every candidate that resolved refused "
            "to name itself.\n%s\n      Pass --cc <path> naming a working compiler. A benchmark with "
            "no reference is not a comparison." % _skip_lines(skips))
    return refs, skips


def _skip_lines(skips):
    return "\n".join("      not measured: %s %s %s: %s" % (s.id, DASH, s.state, s.why) for s in skips)


def report_references(refs, skips, log):
    """Every measured reference, then the NOT MEASURED list -- printed UNCONDITIONALLY: silence
    would have two meanings ("nothing was skipped" and "the skip reporting broke")."""
    for r in refs:
        if r.label == r.id:
            log.info("reference : %s  (%s)" % (r.bin, r.version))
        else:
            log.info("reference : %s  (%s)  %s labelled '%s', because the binary is not what its "
                     "name says" % (r.bin, r.version, DASH, r.label))
    if skips:
        for s in skips:
            log.info("not measured: %s %s %s: %s" % (s.id, DASH, s.state, s.why))
    else:
        log.info("not measured: none %s every catalogue reference resolved and was measured" % DASH)


# ── RESOLVE: which dsscp ─────────────────────────────────────────────────────────────

def select_dss(repo_root, explicit, explicit_by, allow_nonrelease, log):
    """-> sqlite_compiler.Compiler. ONE gate for every branch: the build type is READ and printed
    beside the path, and a non-Release binary is refused unless DSS_ALLOW_NONRELEASE_COMPILER --
    a benchmark cannot tell a slow compiler from a wrongly-selected one (MEASURED 2026-08-26: a
    Debug dsscp was once benchmarked and nothing said so)."""
    if explicit:
        if not os.path.isfile(explicit):
            die("the dsscp path given by %s is not a file: %s\n      (the compiler is named "
                "'dsscp[.exe]', not 'dss')" % (explicit_by, explicit))
        if not os.access(explicit, os.X_OK):
            die("the dsscp path given by %s is not an executable file: %s" % (explicit_by, explicit))
        info = COMP.build_type(explicit)
        origin = "named by %s %s NOT selected by this program" % (explicit_by, DASH)
    else:
        cands, searched = COMP.find_candidates(repo_root)
        info = COMP.select_compiler(cands, allow_nonrelease)
        if info is None:
            if cands:
                die("the only dsscp binaries under %s are NOT Release builds:\n%s\n      A debug "
                    "compiler's build time is not a number worth publishing, and a\n      benchmark "
                    "cannot tell a slow compiler from a wrongly-selected one.\n      Build a release "
                    "one:  dssharness build --legs <a release leg; dssharness legs lists them>\n"
                    "      Pass one explicitly:  --dss <path>\n      Or override on purpose: "
                    "DSS_ALLOW_NONRELEASE_COMPILER=1"
                    % (os.path.join(repo_root, "build"), COMP.format_candidates(cands)))
            die("no dsscp binary found.\n      Pass --dss <path>, or build one: dssharness build "
                "--legs <a release leg; dssharness legs lists them>\n      Searched for dsscp[.exe] "
                "at any depth under: %s" % "; ".join(searched))
        origin = "the newest eligible of %d candidate(s), selected by BUILD TYPE" % len(cands)
    # The CODE's stamp, naming the image file it came from -- the one rule Step 5 reports by
    # (`sqlite_compiler.built_stamp`), never the launcher's own time.
    built = COMP.built_stamp(info)
    note = "  (compiler build type: %s)" % info.type
    if not COMP.is_release(info.type):
        if not allow_nonrelease:
            die("this benchmark would time a NON-RELEASE compiler, and refuses.\n      compiler   : "
                "%s\n      build type : %s\n      read from  : %s\n      origin     : %s\n      A "
                "benchmark cannot tell a slow compiler from a wrongly-selected one. Build a\n      "
                "Release dsscp, or set DSS_ALLOW_NONRELEASE_COMPILER=1 to time THIS one on purpose."
                % (info.path, info.type, info.source, origin))
        note = "  (compiler build type: %s %s NOT Release, DSS_ALLOW_NONRELEASE_COMPILER)" % (info.type, DASH)
        log.warn("measuring a NON-RELEASE compiler (%s) because DSS_ALLOW_NONRELEASE_COMPILER is "
                 "set: %s" % (info.type, info.path))
    log.info("dss       : %s  (build type: %s)" % (info.path, info.type))
    log.info("  read from: %s" % info.source)
    log.info("  origin   : %s" % origin)
    if info.detail:
        log.info("  note     : %s" % info.detail)
    return COMP.Compiler(info.path, info.type, info.source, info.detail, info.tree, origin, built, note)


def preflight(compiler, config_root, spec, repo_root, log, core=None, python=sys.executable):
    """`speedtest1_bench.py --preflight-dss` through `sqlite_compiler.assert_current`: exit 1 is
    "THE COMPILER REFUSED" (the stale-binary signature), any other failure is "THE CHECK COULD NOT
    RUN" (nothing learnt about the binary) -- two answers with two remedies. Run on the measuring
    host FOR THE TARGET the measurement will build, before a configure is paid for."""
    COMP.assert_current(core or C.BENCH_CORE, compiler, config_root, [spec],
                        COMP.rebuild_command(compiler, repo_root), python=python)
    log.info("preflight : OK %s %s compiles three lines against %s for %s"
             % (DASH, compiler.path, os.path.join(config_root, "src", "dss-config"), spec))


# ── RESOLVE: the leg facts, from the catalogue ───────────────────────────────────────

LegFacts = collections.namedtuple("LegFacts", ["label", "spec", "transform", "reserve", "link_flags"])


def leg_catalogue(resolver):
    """Every declared leg, as the resolver plans it for THIS host -- structurally (no launcher or
    environment probe is run: only the declared build facts and the native-run decision are read)."""
    plan = resolver.json(["--plan", "--format", "json", "--launchers-none",
                          "--environment-probes", "skip"] + resolver.host_args,
                         "the leg catalogue (harness_legs.py --plan)")
    legs = plan.get("legs") if isinstance(plan, dict) else None
    if not isinstance(legs, list) or not legs:
        die("harness_legs.py --plan answered no legs %s a contract break between the resolver and this "
            "program, not a property of this host." % DASH)
    return legs


def native_leg(legs, host_desc):
    """The ONE declared leg the resolver plans to run NATIVELY on this host (its `runOn` holds the
    host OS and its target arch is the host's) -- the leg's own declaration, never a `uname` table."""
    native = [lg for lg in legs if (lg.get("run") or {}).get("mode") == "native"]
    if len(native) == 1:
        return native[0]
    declared = ", ".join(lg.get("spec", "?") for lg in legs)
    if not native:
        die("no leg of legs.json runs natively on this host (%s), so there is no default target.\n"
            "      Pass --target <spec> (declared: %s)." % (host_desc, declared))
    die("%d legs of legs.json claim to run natively on this host (%s): %s. The default target must "
        "be ONE leg;\n      pass --target <spec>." % (len(native), host_desc,
                                                     ", ".join(lg.get("label", "?") for lg in native)))


def leg_facts(legs, spec):
    """-> LegFacts of the leg declaring `spec`: its recipe transform, stack reserve and reference
    link flags AS legs.json DECLARES THEM. A spec no leg declares is refused -- a silently empty
    link line fails at the linker with an undefined symbol that reads like a codegen bug."""
    hits = [lg for lg in legs if lg.get("spec") == spec]
    if not hits:
        die("the target '%s' is declared by no leg of legs.json.\n      Its recipe transform, stack "
            "reserve and reference link flags are the catalogue's facts,\n      never guessed from "
            "the spec's spelling. Declared: %s" % (spec, ", ".join(lg.get("spec", "?") for lg in legs)))
    if len(hits) > 1:
        die("the target '%s' is declared by %d legs of legs.json (%s); its facts would be ambiguous."
            % (spec, len(hits), ", ".join(lg.get("label", "?") for lg in hits)))
    lg = hits[0]
    b = lg.get("build") or {}
    t, r, f = b.get("recipeTransform"), b.get("stackReserveBytes"), b.get("referenceLinkFlags")
    if not isinstance(t, str) or not t:
        die("leg '%s' declares no build.recipeTransform %s a contract break." % (lg.get("label"), DASH))
    if not isinstance(r, int) or isinstance(r, bool) or r < 0:
        die("leg '%s' declares build.stackReserveBytes %r, not a non-negative integer." % (lg.get("label"), r))
    if not isinstance(f, list) or not all(isinstance(x, str) and x for x in f):
        die("leg '%s' declares no build.referenceLinkFlags list %s the reference arms' link line would "
            "be a guess." % (lg.get("label"), DASH))
    return LegFacts(lg.get("label", "?"), spec, t, r, list(f))


def with_overrides(facts, transform, reserve, log):
    """The leg's facts with --recipe-transform / --stack-reserve applied; an override is REPORTED
    beside the value it replaced."""
    t = facts.transform if transform is None else transform
    r = facts.reserve if reserve is None else reserve
    log.info("target    : %s  (leg %s)" % (facts.spec, facts.label))
    log.info("  recipe transform : %s%s" % (t, "" if t == facts.transform else
                                            "   (OVERRIDES legs.json's '%s')" % facts.transform))
    log.info("  stack reserve    : %s%s" % (r, "" if r == facts.reserve else
                                            "   (OVERRIDES legs.json's %s)" % facts.reserve))
    log.info("  reference links  : %s   (legs.json build.referenceLinkFlags)"
             % (" ".join(facts.link_flags) or "<none>"))
    return facts._replace(transform=t, reserve=r)


_GEN_TRANSFORMS = None


def generator_transforms():
    """The recipe transforms the manifest generator implements (its RECIPE_TRANSFORMS), read from
    the generator itself -- so an override it cannot honour is refused before a configure runs."""
    global _GEN_TRANSFORMS
    if _GEN_TRANSFORMS is None:
        spec = importlib.util.spec_from_file_location("dss_gen_pe64_manifest", C.MANIFEST_GEN)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _GEN_TRANSFORMS = tuple(mod.RECIPE_TRANSFORMS)
    return _GEN_TRANSFORMS


# ── DERIVE ───────────────────────────────────────────────────────────────────────────

class TexeRefused(Exception):
    """The executable-suffix probe did not answer: code 3 no answer came back (the likeliest cause:
    `make` is not GNU make), 4 that Makefile defines no T.exe, 5 make failed (or the probe file
    could not be written). `transcript` is make's whole output."""

    def __init__(self, code, transcript):
        super().__init__("T.exe probe refused (code %d)" % code)
        self.code, self.transcript = code, transcript


class SubstitutionRefused(Exception):
    """The main-TU substitution: 3 the list does not carry exactly one shell.c, 4 shell.c survived
    the rewrite, 5 speedtest1.c is not in the rewritten list."""

    def __init__(self, code, detail):
        super().__init__("main-TU substitution refused (code %d): %s" % (code, detail))
        self.code, self.detail = code, detail


# ★★★ ASK make WHAT IT CALLS AN EXECUTABLE, ON EVERY make THAT EXISTS. `main.mk` declares the
# target as `sqlite3d$(T.exe)`: `.exe` on a Windows-configured tree, empty on POSIX, and `make
# sqlite3d` on a Windows tree matches NO rule and answers "Nothing to be done" with exit 0. The
# question is asked with a PRINTER makefile read beside the real one (`$(info)` and repeated `-f`
# are GNU make 3.81 features; `--eval` is 4.0+ and macOS ships 3.81 -- MEASURED 2026-08-25), and an
# EMPTY answer is trusted only because `$(origin T.exe)` says the Makefile defines it.
# ⚠ THE ORDER OF THE THREE CHECKS IS AN ATTRIBUTION DECISION: `$(info)` fires while make PARSES, so
# a build dir with NO Makefile still prints `origin=<undefined>` on its way to exit 2 (MEASURED
# 2026-08-25 on 3.81 and 4.3; again 2026-09-21 on this Windows host's GNU make 4.4.1). The exit
# code is judged first, so a MISSING Makefile is a make failure (5), never "defines no T.exe" (4).
TEXE_PROBE = ("# GENERATED by benchmark_speedtest1.py -- safe to delete.\n"
              "# Read alongside the build directory's own Makefile so that make itself\n"
              "# answers what it calls the executable suffix. `$(info)` and repeated `-f`\n"
              "# are GNU make 3.81 features; `--eval` is 4.0+ and macOS ships 3.81.\n"
              "$(info __dss_texe=<$(T.exe)> origin=<$(origin T.exe)>)\n"
              "%s: ;\n" % PROBE_TARGET)
_TEXE_LINE = re.compile(r"^__dss_texe=<(.*?)> origin=<([a-z ]*)>$")


def derive_make_texe(build_dir, probe_mk, make_argv=("make",)):
    """-> the value of $(T.exe) -- POSSIBLY EMPTY, the right answer on every POSIX host -- asked of
    the make that will run every recipe derived from this build dir. TexeRefused(3|4|5)."""
    try:
        with open(probe_mk, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(TEXE_PROBE)
    except OSError as exc:
        raise TexeRefused(5, "could not write the probe makefile %s: %s" % (probe_mk, exc))
    r = C.capture(list(make_argv) + ["-s", "-f", "Makefile", "-f", probe_mk, PROBE_TARGET],
                  cwd=build_dir, merge=True, env_=C.child_env())
    text = (r.out or "") + (r.err or "")
    if r.rc != 0:
        raise TexeRefused(5, text)
    hits = [m for m in (_TEXE_LINE.match(ln.strip("\r")) for ln in text.splitlines()) if m]
    if not hits:
        raise TexeRefused(3, text)
    if hits[-1].group(2) == "undefined":
        raise TexeRefused(4, text)
    return hits[-1].group(1)


_TEXE_REFUSALS = {
    3: ("the executable-suffix probe produced NO ANSWER, and this program does not guess one. make "
        "ran cleanly in %(bld)s\n      but printed nothing this probe recognises (its output: "
        "%(log)s).\n      ★ The likeliest cause is that 'make' here is NOT GNU make: the answer is "
        "emitted by $(info ...),\n      which a non-GNU make expands to nothing instead of failing. "
        "SQLite's own build needs GNU make\n      regardless (BSD hosts usually ship it as 'gmake'). "
        "An empty fallback would spell the target\n      'sqlite3d', which on a Windows-configured "
        "tree matches no rule and 'succeeds' compiling nothing."),
    4: ("the Makefile in %(bld)s does not define $(T.exe) at all (make answered 'origin: undefined').\n"
        "      That is not a POSIX host reporting an empty suffix -- it is this probe reading a "
        "Makefile that\n      is not the one sqlite's configure writes. See %(log)s and "
        "%(out)s/configure.log.\n      ★ This check is what makes an EMPTY answer trustworthy."),
    5: ("make exited non-zero while answering the executable-suffix probe in %(bld)s (its output: "
        "%(log)s).\n      Every recipe this program derives comes out of that same Makefile, so a "
        "make that cannot\n      evaluate one variable from it is not a tree worth deriving from."),
}


def substitute_main_tu(tus_file, speedtest1_c, write=None):
    """Swap the full-source CLI's `shell.c` for `speedtest1.c` IN `tus_file`, then RE-READ the file
    and prove it took: a substitution that silently did nothing would benchmark the sqlite3 CLI
    under the name speedtest1. -> the new list, or SubstitutionRefused(3|4|5). `write` is the
    list writer (the self-test injects one that loses the rewrite, to produce codes 4 and 5)."""
    lines = _read_list(tus_file)
    shells = [t for t in lines if os.path.basename(t) == CLI_MAIN_TU]
    if len(shells) != 1:
        raise SubstitutionRefused(3, "%d %s in the list (want exactly one)" % (len(shells), CLI_MAIN_TU))
    (write or _write_list)(tus_file, [t for t in lines if os.path.basename(t) != CLI_MAIN_TU]
                           + [speedtest1_c])
    after = _read_list(tus_file)
    if any(os.path.basename(t) == CLI_MAIN_TU for t in after):
        raise SubstitutionRefused(4, "%s survived the rewrite of %s" % (CLI_MAIN_TU, tus_file))
    if speedtest1_c not in after:
        raise SubstitutionRefused(5, "%s is not in the rewritten %s" % (speedtest1_c, tus_file))
    return after


def refuse_amalgamation(archive):
    """An archive built without USE_AMALGAMATION=0 holds ONE member, `sqlite3.o` -- the
    amalgamation, under exactly the right file name. Recovering the subject from it would benchmark
    the amalgamation under a full-source label, the one thing this benchmark exists not to do. A
    COUNT cannot say which thing is in there; the member NAME can. (The names are READ from the
    archive by `sqlite_base.archive_members`, the same on every host; a file that is not an archive
    is refused there -- it is never an empty archive.)"""
    if not os.path.isfile(archive):
        return
    try:
        members = BASE.archive_members(archive)
    except BASE.RecipeRefused as exc:
        die(str(exc))
    if AMALGAMATION_OBJECT in members:
        die("the archive %s contains '%s' %s that is the AMALGAMATION object, so this tree was built\n"
            "      without USE_AMALGAMATION=0. Recovering the subject from it would benchmark the\n"
            "      amalgamation under a full-source label, which is the one thing this benchmark "
            "exists\n      not to do. Delete the archive and re-run." % (archive, AMALGAMATION_OBJECT, DASH))


def check_capabilities(defines, required, make_options, recipe):
    """SQLITE_CORE by NAME (its absence is silent in every count and turns into
    `error[...] unicode/*.h` from ext/icu minutes later), then EVERY declared
    `stageBuild.requiredDefines` -- the retired benchmark checked only SQLITE_CORE, so it could
    measure a smaller SQLite than the corpus tests under the same name. The membership rule
    (`NAME` or `NAME=VALUE`) is `sqlite_stage.missing_capabilities`'s."""
    missing_core = _stage().missing_capabilities(defines, [CORE_DEFINE])
    if missing_core:
        die("the derived define set has no %s. Without it ext/icu/icu.c demands <unicode/*.h>.\n"
            "      That means the -D tokens came off the link line alone: check that the recipe was "
            "derived\n      with --always-make and the recipe token scope. Recipe: %s" % (CORE_DEFINE, recipe))
    missing = _stage().missing_capabilities(defines, required)
    if missing:
        die("the derived recipe is MISSING declared capabilities:%s\n      declared (legs.json "
            "stageBuild.requiredDefines): %s\n      make OPTIONS passed: %s\n      derived from: %s\n"
            "      Each was asked for by name. Without it the program measured is a SMALLER SQLite "
            "than the\n      one the corpus tests, published under the same name."
            % ("".join(" " + d for d in missing), " ".join(required), make_options or "<none>", recipe))


_WINDOWS_DRIVE = re.compile(r"^[A-Za-z]:[\\/]")


def looks_windows(path):
    return bool(_WINDOWS_DRIVE.match(path)) or is_unc(path)


def _wslpath_w(path):
    r = C.capture(["wslpath", "-w", path], timeout=60, env_=C.child_env())
    return r.rc, r.out, r.err


class WindowsSpelling:
    """A POSIX path spelled for the Windows host, by `wslpath -w` inside WSL -- ONCE per path.
    ★ NOT IDEMPOTENT, and it fails SILENTLY when applied twice: `wslpath -w` reads its argument as a
    POSIX path, so `C:\\Source\\x\\y.c` came back as `CSourcexy.c` with exit 0 (MEASURED 2026-08-21
    by the retired .sh, caught only by speedtest1_bench's R1). So a path that already looks like a
    Windows one is a REFUSED double translation, a relative one is refused (wslpath would resolve
    it against its own cwd), and an answer that is not a Windows path is refused.
    `-w` (backslashes), not `-m`: speedtest1_bench's R4 recognises a UNC share by its `\\\\`
    spelling, and `-m` would spell a WSL-side path `//wsl.localhost/...`."""

    def __init__(self, run=None):
        self._run = run or _wslpath_w
        self.cache = {}
        self.calls = 0

    def __call__(self, path):
        if path in self.cache:
            return self.cache[path]
        if looks_windows(path):
            die("a DOUBLE translation was refused: '%s' is ALREADY a Windows path, and `wslpath -w` "
                "would read it\n      as a POSIX one and answer a plausible, wrong path with exit 0."
                % path)
        if not path.startswith("/"):
            die("INTERNAL: '%s' reached the Windows spelling, which takes only ABSOLUTE POSIX paths "
                "(wslpath\n      would resolve a relative one against its own working directory)." % path)
        self.calls += 1
        rc, out, err = self._run(path)
        lines = (out or "").replace("\0", "").strip().splitlines()
        got = lines[-1].strip() if lines else ""
        if rc != 0 or not got:
            die("could not spell %s for the Windows host (wslpath -w exited %d: %s)"
                % (path, rc, ((err or out) or "").replace("\0", "").strip()[:200] or "(no output)"))
        if not looks_windows(got):
            die("wslpath -w answered '%s' for '%s', which is not a Windows path %s the translator ran "
                "but did not translate." % (got, path, DASH))
        self.cache[path] = got
        return got


def translate_manifest(manifest, spell):
    """The manifest's sources and includes spelled for the Windows host -- AFTER generation, never
    before (gen-pe64-manifest.py asserts every source exists, which only the side that can see the
    files can check), and ONCE (a second pass is refused by the double-translation rule). The
    manifest IS what dsscp reads, so translating only the plan's copy would leave the DSS arm
    reading POSIX paths (MEASURED 2026-08-21: `error[D_FileNotFound] cannot open /mnt/c/...`)."""
    with open(manifest, "r", encoding="utf-8") as fh:
        m = json.load(fh)
    m["sources"] = [spell(s) for s in m["sources"]]
    m["includes"] = [spell(i) for i in m["includes"]]
    _write_json(manifest, m)
    return len(m["sources"]) + len(m["includes"])


Tools = collections.namedtuple("Tools", ["make", "configure", "python", "gen"])
DeriveConfig = collections.namedtuple(
    "DeriveConfig", ["sqlite_dir", "out_dir", "spec", "transform", "reserve", "stage", "jobs",
                     "style", "tools", "spell", "log"])
Derived = collections.namedtuple(
    "Derived", ["manifest", "recipe", "tus_file", "defines_file", "includes_file", "make_target",
                "texe", "tu_count", "define_count", "include_count", "summary", "translated"])


def default_tools(sqlite_dir):
    return Tools(("make",), (os.path.join(sqlite_dir, "configure"),), sys.executable,
                 C.MANIFEST_GEN)


def stage_build(resolver):
    """The declared sqlite stage build (legs.json `stageBuild`), shape-checked by its owner
    (`sqlite_stage.parse_stage_build`): configure flags, make OPTIONS, required defines."""
    r = resolver.call(["--stage-build", "--format", "json"])
    if r.rc != 0:
        die("could not resolve the sqlite stage build configuration (harness_legs.py --stage-build "
            "exited %d):\n%s" % (r.rc, C.last_lines(r.err or r.out, 12)))
    return _stage().parse_stage_build(r.out)


def derive(cfg):
    """DERIVE, in THIS process (a POSIX host, or the WSL half of a Windows host) -> Derived."""
    log, sqlite, out = cfg.log, cfg.sqlite_dir, cfg.out_dir
    tools = cfg.tools
    speedtest = os.path.join(sqlite, "test", MAIN_TU)
    bld = os.path.join(out, "sqlite-build")
    os.makedirs(bld, exist_ok=True)
    flags, mopts = list(cfg.stage["configure_flags"]), cfg.stage["make_options"]
    log.info("capabilities: %s%s" % (" ".join(flags), ("   make OPTIONS=" + mopts) if mopts else ""))

    log.step("Configure SQLite and build the reference full-source CLI")
    cfg_log = os.path.join(out, "configure.log")
    rc = _run_to_log(list(tools.configure) + flags, bld, cfg_log)
    if rc != 0:
        die("sqlite configure FAILED (exit %d) %s see %s" % (rc, DASH, cfg_log))
    # The Makefile asked is the one THIS configure just wrote: a build dir shared across hosts may
    # hold another host's Makefile, and a grep of the text could not say which host wrote it.
    # The probe's log holds make's whole transcript on a refusal and is EMPTY on success (the .sh).
    probe_mk, probe_log = os.path.join(out, "texe-probe.mk"), os.path.join(out, "texe-probe.log")
    try:
        texe = derive_make_texe(bld, probe_mk, tools.make)
    except TexeRefused as exc:
        with open(probe_log, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(exc.transcript or "")
        die(_TEXE_REFUSALS[exc.code] % {"bld": bld, "log": probe_log, "out": out})
    with open(probe_log, "w", encoding="utf-8", newline="\n"):
        pass
    target = CLI_TARGET_STEM + texe
    log.info("make target: %s   (T.exe='%s', asked of make, not assumed)" % (target, texe))
    # `sqlite3d` generates the derived sources (parse.c, opcodes.c ...) the recipe names;
    # `libsqlite3.a` with USE_AMALGAMATION=0 is the archive the core sources are recovered from --
    # without it `make libsqlite3.a` holds ONE member, sqlite3.o (MEASURED 2026-08-21).
    ref_log = os.path.join(out, "reference-build.log")
    rc = _run_to_log(list(tools.make) + ["-s", target, "libsqlite3.a", "USE_AMALGAMATION=0",
                                         "OPTIONS=" + mopts, "-j%d" % cfg.jobs], bld, ref_log)
    if rc == 0:
        log.ok("reference full-source CLI built (its derived sources are now on disk)")
    else:
        log.warn("the reference %s did not fully link (exit %d) %s continuing, because what this" % (target, rc, DASH))
        log.warn("step is FOR is the generated sources; the recipe floors below are the honest")
        log.warn("gate on whether enough of it succeeded. See %s" % ref_log)

    log.step("Derive the full-source recipe and substitute %s for %s" % (MAIN_TU, CLI_MAIN_TU))
    recipe = os.path.join(out, "sqlite3d-recipe.txt")
    tus_file = os.path.join(out, "tus.txt")
    defs_file = os.path.join(out, "defines.txt")
    incs_file = os.path.join(out, "includes.txt")
    archive = os.path.join(bld, "libsqlite3.a")
    if os.path.isfile(os.path.join(bld, ".libs", "libsqlite3.a")):
        archive = os.path.join(bld, ".libs", "libsqlite3.a")
    refuse_amalgamation(archive)
    # The .sh's call, flag for flag: link-line + -B + the recipe token scope (without them the -D
    # set is read off the link line alone and loses SQLITE_CORE), the archive filtered to the
    # objects the link names, the three search roots, and its floors.
    try:
        res = BASE.emit_recipe(
            build_dir=bld, make_target=target, recipe_file=recipe,
            make_vars=["OPTIONS=" + mopts], prereq_mode="link-line", always_make=True,
            token_scope="recipe", archive=archive, archive_from_span=True,
            search_roots=[os.path.join(sqlite, "src"), os.path.join(sqlite, "ext"), bld],
            min_tus=MIN_TUS, min_defines=MIN_DEFINES, out_tus=tus_file, out_defines=defs_file,
            out_includes=incs_file, make_argv=tools.make)
    except BASE.RecipeRefused as exc:
        die("the full-source recipe derivation FAILED %s see %s\n%s" % (DASH, recipe, exc))
    except BASE.HarnessUsageError as exc:
        die("INTERNAL: this program's own emit_recipe call was refused as malformed: %s" % exc)
    log.ok("recipe: %s" % res.summary)
    check_capabilities(res.defines, cfg.stage["required_defines"], mopts, recipe)
    try:
        tus = substitute_main_tu(tus_file, speedtest)
    except SubstitutionRefused as exc:
        if exc.code == 3:
            die("the derived TU set does not carry exactly one %s (%s), so there is nothing to "
                "substitute.\n      That is a change in the reference recipe's shape, not a "
                "benchmark option. Recipe: %s" % (CLI_MAIN_TU, exc.detail, recipe))
        die("the main-TU substitution did not take effect on %s (%s) %s refusing to benchmark a "
            "subject whose\n      identity is unproven." % (tus_file, exc.detail, DASH))
    log.ok("subject: %d full-source TUs, main = test/%s" % (len(tus), MAIN_TU))
    # `make -n` compiles FROM the build dir, so the recipe spells it `-I.` -- meaningless anywhere
    # else, and dropped by the reader. Everything GENERATED lives there (sqlite3.h, opcodes.h,
    # parse.h, keywordhash.h, sqlite_cfg.h), so it is appended, LAST: a generated header must never
    # shadow a source directory's.
    incs = _read_list(incs_file)
    if bld not in incs:
        incs.append(bld)
        _write_list(incs_file, incs)
    if not os.path.isfile(os.path.join(bld, "sqlite3.h")):
        die("the generated %s is not there, so the reference build did not get far enough to "
            "produce it.\n      Every arm would fail on the first TU. See %s"
            % (os.path.join(bld, "sqlite3.h"), ref_log))
    log.info("includes  : %d dirs (the sqlite src/ext dirs + the generated-header dir)" % len(incs))

    log.step("Generate the DSS project manifest (it also fixes the shared define set)")
    manifest = os.path.join(out, MANIFEST_FILE)
    gen = BASE.generate_manifest(tools.gen, manifest, ARTIFACT_NAME, cfg.spec, tus_file, incs_file,
                                 defs_file, cfg.transform, cfg.reserve, python=tools.python)
    for line in (gen.out or "").strip().splitlines():
        log.info(line)
    err = BASE.manifest_error(gen)
    if err:
        die("manifest generation FAILED for %s: %s" % (cfg.spec, err))
    translated = 0
    if cfg.style == "windows":
        translated = translate_manifest(manifest, cfg.spell)
        log.info("manifest  : %d path(s) spelled for the Windows host (wslpath -w, once each)" % translated)
    return Derived(manifest, recipe, tus_file, defs_file, incs_file, target, texe, len(tus),
                   len(res.defines), len(incs), res.summary, translated)


def write_derive_result(path, derived, facts, spell):
    doc = {"schema": DERIVE_SCHEMA, "target": facts.spec, "recipeTransform": facts.transform,
           "stackReserve": facts.reserve, "manifest": spell(derived.manifest),
           "makeTarget": derived.make_target, "tuCount": derived.tu_count,
           "defineCount": derived.define_count, "includeCount": derived.include_count,
           "translated": derived.translated, "recipeSummary": derived.summary}
    _write_json(path, doc)
    return doc


def read_derive_result(path, expect_manifest, spec):
    """What the WSL half left behind, checked before a byte of it is trusted: THIS schema, THIS
    target, and the manifest the measuring host expects (never a stale one found lying there)."""
    if not os.path.isfile(path):
        die("the derivation reported success but wrote no result at %s.\n      A half that claims a "
            "file it did not produce is the one failure this program refuses to shrug at." % path)
    try:
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as exc:
        die("the derivation's result %s is not readable JSON: %s" % (path, exc))
    if not isinstance(doc, dict) or doc.get("schema") != DERIVE_SCHEMA:
        die("the derivation's result %s is not a '%s' document (schema %r)"
            % (path, DERIVE_SCHEMA, doc.get("schema") if isinstance(doc, dict) else None))
    if doc.get("target") != spec:
        die("the derivation's result %s is for target %r, not %r" % (path, doc.get("target"), spec))
    got = doc.get("manifest") or ""
    if os.path.normcase(os.path.abspath(got)) != os.path.normcase(os.path.abspath(expect_manifest)):
        die("the derivation's result names the manifest %r, not the one this host expects (%s)"
            % (got, expect_manifest))
    return doc


def verify_host_paths(manifest):
    """Every translated source must be a FILE and every include a DIRECTORY on the measuring host
    -- a wrong translation that resolves is worse than a stop, and --derive-only never reaches the
    measurement core's R1."""
    with open(manifest, "r", encoding="utf-8") as fh:
        m = json.load(fh)
    bad = ["source  %s" % s for s in m.get("sources", []) if not os.path.isfile(s)]
    bad += ["include %s" % i for i in m.get("includes", []) if not os.path.isdir(i)]
    if bad:
        die("%d translated path(s) of %s do not exist on this host:\n%s\n      The derive half spelled "
            "them for this host and they do not resolve here." % (
                len(bad), manifest, "\n".join("        " + b for b in bad[:8])))
    return len(m.get("sources", [])), len(m.get("includes", []))


def hop_argv(posix, this_posix, sqlite_posix, out_posix, facts):
    """The WSL hop: THIS program's derive half, argv only (`wsl.exe -e`, never a login shell, never
    `wsl.exe --`, never a shell string)."""
    return posix.argv(["python3", this_posix, "--derive-only", "--path-style", "windows",
                       "--sqlite-dir", sqlite_posix, "--out", out_posix, "--target", facts.spec,
                       "--recipe-transform", facts.transform, "--stack-reserve", str(facts.reserve)])


def stream(argv):
    """Run `argv`, printing its merged output AS IT ARRIVES (the .ps1 buffered the whole derive and
    printed it at the end) -> (exit code, the last 40 lines). NULs are stripped: wsl.exe's own
    messages arrive as UTF-16LE."""
    tail = collections.deque(maxlen=40)
    try:
        p = subprocess.Popen(list(argv), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, env=C.child_env())
    except OSError as exc:
        return 127, ["could not start %s: %s" % (argv[0], exc)]
    with p.stdout:
        for raw in iter(p.stdout.readline, b""):
            line = raw.decode("utf-8", "replace").replace("\0", "").rstrip("\r\n")
            tail.append(line)
            print(line, flush=True)
    return p.wait(), list(tail)


def wsl_derive(posix, sqlite_dir, out_dir, facts, log, run=stream):
    """DERIVE for a Windows host: THIS program inside WSL, whose manifest comes back spelled for
    Windows. The result and manifest of an earlier run are removed first, so nothing stale can be
    read back as this run's."""
    result = os.path.join(out_dir, DERIVE_RESULT)
    manifest = os.path.join(out_dir, MANIFEST_FILE)
    for p in (result, manifest):
        try:
            _rm_f(p)
        except OSError as exc:
            die("cannot remove the previous %s before deriving again: %s" % (p, exc))
    argv = hop_argv(posix, posix.to_posix(THIS), posix.to_posix(sqlite_dir), posix.to_posix(out_dir), facts)
    log.info("WSL hop   : %s" % " ".join(argv))
    rc, tail = run(argv)
    if rc == 2:
        die("INTERNAL: the derive half refused the arguments this host built (exit 2):\n%s"
            % "\n".join("      " + t for t in tail[-12:]))
    if rc != 0:
        die("the subject derivation inside WSL FAILED (exit %d) %s its own diagnostic is above.\n      "
            "The measurement is not attempted on an unproven subject." % (rc, DASH))
    return read_derive_result(result, manifest, facts.spec)


# ── PLAN ─────────────────────────────────────────────────────────────────────────────

def format_output_extension(config_root, spec):
    """The extension an artefact of `spec`'s object format carries -- `.exe` for a pe64
    executable, none for elf64/macho64 -- READ from that format's own config
    (`<config_root>/src/dss-config/object-formats/<format>.format.json`, `outputExtension`: the
    table dsscp itself names its outputs from), never spelled here. The measurement core names
    each arm's binary with it, so a binary this host must launch carries the suffix its format
    gives it. (Until 2026-09-22 the core spelled `.exe` itself, keyed on the host.)"""
    fmt = spec.split(":", 1)[1] if isinstance(spec, str) and ":" in spec else ""
    path = os.path.join(config_root, "src", "dss-config", "object-formats", fmt + ".format.json")
    try:
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as exc:
        die("W4 the object format of target '%s' has no readable config at %s (%s) %s the plan "
            "states each binary's extension FROM it." % (spec, path, exc, DASH))
    ext = doc.get("outputExtension") if isinstance(doc, dict) else None
    if not isinstance(ext, str):
        die("W4 %s declares no string 'outputExtension' %s the plan states each binary's extension "
            "FROM it." % (path, DASH))
    return ext


def write_plan(path, *, manifest, sqlite_dir, sqlite_head, dss, config_root, target, refs,
               link_flags, size, testset, build_repeats, run_repeats, jobs_arms, out_dir):
    """The plan `speedtest1_bench.py` reads. Its sources/includes/defines are READ BACK OUT OF THE
    MANIFEST -- the one set the declared transform produced, handed to every arm. One unix-cc arm
    per reference measured; the msvc arm always (the core resolves it or skips it by name)."""
    if not refs:
        die("W1 no reference compiler record reached the plan writer %s a plan with no reference "
            "is not a comparison." % DASH)
    try:
        with open(manifest, "r", encoding="utf-8") as fh:
            m = json.load(fh)
    except (OSError, ValueError) as exc:
        die("W2 the manifest %s is not readable JSON: %s" % (manifest, exc))
    for key in ("sources", "includes", "defines"):
        v = m.get(key) if isinstance(m, dict) else None
        if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
            die("W2 the manifest %s carries no '%s' list of strings %s the plan's subject is READ "
                "from it." % (manifest, key, DASH))
    exe_suffix = format_output_extension(config_root, target)
    plan = {
        "subject": {
            "tus": list(m["sources"]),
            "includes": list(m["includes"]),
            "defines": list(m["defines"]),
            "sqliteSrc": sqlite_dir,
            "upstreamCommit": sqlite_head,
        },
        "compilers": [
            {"id": "dss", "kind": "dss", "label": "DSS Code Prime", "bin": dss,
             "manifest": manifest, "config": "release", "configRoot": config_root,
             # the artefact path is read back from `dsscp: artifact <spec> <path>`, keyed on it
             "target": target, "artifactName": ARTIFACT_NAME,
             "optimizationLabel": "--config=release"},
        ] + [
            {"id": r.id, "kind": "unix-cc", "label": r.label, "version": short_version(r.version),
             "bin": r.bin, "optFlags": ["-O2"], "linkFlags": list(link_flags),
             "optimizationLabel": "-O2"} for r in refs
        ] + [
            {"id": "msvc", "kind": "msvc", "label": "MSVC cl.exe", "bin": "cl.exe",
             "optFlags": ["/O2"], "optimizationLabel": "/O2"},
        ],
        "workload": {"size": int(size), "testset": (testset or None), "verify": True},
        "repeats": {"build": int(build_repeats), "run": int(run_repeats)},
        "jobsArms": [int(j) for j in jobs_arms],
        "outDir": out_dir,
        "target": target,
        # the binaries' extension, from the target format's own config (format_output_extension)
        "exeSuffix": exe_suffix,
    }
    try:
        _write_json(path, plan)
    except OSError as exc:
        die("W3 the plan cannot be written to %s: %s" % (path, exc))
    return plan


# ── MEASURE ──────────────────────────────────────────────────────────────────────────

def measure(plan_path, out_dir, core=None, python=sys.executable):
    """`speedtest1_bench.py --plan`, natively, its output straight to this console -> its exit
    code (0 measured, 1 refused, 2 usage, 3 no arm produced a binary)."""
    argv = [python, core or C.BENCH_CORE, "--plan", plan_path,
            "--json-out", os.path.join(out_dir, "benchmark-speedtest1.json"),
            "--md-out", os.path.join(out_dir, "benchmark-speedtest1.md")]
    try:
        return subprocess.run(argv, stdin=subprocess.DEVNULL,
                              env=C.child_env(python=True)).returncode
    except OSError as exc:
        die("could not start the measurement core: %s: %s" % (argv[0], exc))


# ── the two ways this program runs ───────────────────────────────────────────────────

def run_measuring_host(args, host, log=C.LOG):
    arch = C.host_arch()
    log.step("1/4  Resolve the subject, the compiler and the reference toolchains (%s/%s host)"
             % (host, arch))
    sqlite_dir = os.path.abspath(os.path.expanduser(
        args.sqlite_dir or C.env("SQLITE_DIR").strip() or default_sqlite_dir(host)))
    refuse_unc(sqlite_dir, host, "the SQLite checkout")
    check_subject(sqlite_dir)
    head, why = sqlite_head(sqlite_dir)
    log.info("sqlite    : %s  (upstream %s%s)" % (sqlite_dir, head, (" %s %s" % (DASH, why)) if why else ""))
    if host == "windows":
        if not shutil.which("wsl.exe"):
            die("wsl.exe not found.\n      That is WHERE THIS HOST FINDS ITS POSIX TOOLCHAIN, not a "
                "statement about any target:\n      deriving the SQLite recipe needs make + tclsh + "
                "sqlite's autosetup configure, which\n      on a Windows host run in WSL. The "
                "MEASUREMENT still runs natively, here.")
    else:
        require_tools(DERIVE_TOOLS)
    repo_root = args.dss_src or C.env("SRC_DIR").strip() or C.driver_tree() or ""
    if not repo_root:
        die("this copy of the benchmark lives in no DSS tree, and neither --dss-src nor SRC_DIR names "
            "one:\n      name the checkout whose dsscp and config tree are measured.")
    repo_root = os.path.abspath(repo_root)
    allow = C.tristate("DSS_ALLOW_NONRELEASE_COMPILER")
    dss_flag, dss_env = args.dss, C.env("DSS_BIN").strip()
    compiler = select_dss(repo_root, dss_flag or dss_env, "--dss" if dss_flag else "DSS_BIN", allow, log)
    config_root = COMP.pin_config_root(repo_root, log)
    resolver = C.Resolver(host, arch)
    legs = leg_catalogue(resolver)
    spec = args.target or native_leg(legs, "%s/%s" % (host, arch))["spec"]
    facts = with_overrides(leg_facts(legs, spec), args.recipe_transform, args.stack_reserve, log)
    cc_flag, cc_env = args.cc, C.env("CC").strip()
    pinned, pinned_by = "", ""
    if cc_flag or cc_env:
        pinned_by = "--cc" if cc_flag else "the CC environment variable"
        pinned = resolve_pinned(cc_flag or cc_env, pinned_by)
    refs, skips = discover_references(pinned, pinned_by)
    report_references(refs, skips, log)
    preflight(compiler, config_root, spec, repo_root, log)
    msvc = C.capture([sys.executable, C.BENCH_CORE, "--resolve-msvc"], timeout=600,
                     env_=C.child_env(python=True))
    if msvc.rc == 0:
        log.info("msvc      : resolved (vswhere + vcvarsall)")
    else:
        try:
            reason = json.loads(msvc.out).get("reason") or "(no reason given)"
        except (ValueError, AttributeError):
            reason = C.first_lines(msvc.err or msvc.out, 2) or "exit %d" % msvc.rc
        log.info("msvc      : ABSENT %s %s (the plan still carries the arm; the core skips it by name)"
                 % (DASH, reason))
    out_dir = os.path.abspath(args.out) if args.out else os.path.join(sqlite_dir, "bld-dss-bench")
    refuse_unc(out_dir, host, "the output directory")
    os.makedirs(out_dir, exist_ok=True)
    plan_path = os.path.abspath(args.plan) if args.plan else os.path.join(out_dir, PLAN_FILE)
    log.info("output    : %s" % out_dir)

    if host == "windows":
        log.step("2/4  Derive the full-source subject inside WSL (THIS program's derive half)")
        log.info("SQLite configures with autosetup + make + tclsh, so the derivation runs in WSL;")
        log.info("the MEASUREMENT does not: it runs natively, below.")
        doc = wsl_derive(C.PosixSide(host), sqlite_dir, out_dir, facts, log)
        manifest = doc["manifest"]
        n_src, n_inc = verify_host_paths(manifest)
        log.ok("subject derived: %d TUs and %d include dirs, every one present on this host" % (n_src, n_inc))
    else:
        log.step("2/4  Derive the full-source subject (in this process)")
        stage = stage_build(resolver)
        derived = derive(DeriveConfig(sqlite_dir, out_dir, spec, facts.transform, facts.reserve,
                                      stage, C.cpu_count(), "posix", default_tools(sqlite_dir),
                                      None, log))
        manifest = derived.manifest

    log.step("3/4  Write the benchmark plan")
    plan = write_plan(plan_path, manifest=manifest, sqlite_dir=sqlite_dir, sqlite_head=head,
                      dss=compiler.path, config_root=config_root, target=spec, refs=refs,
                      link_flags=facts.link_flags,
                      size=args.size if args.size is not None else DEFAULTS["size"],
                      testset=args.testset,
                      build_repeats=args.build_repeats if args.build_repeats is not None else DEFAULTS["build_repeats"],
                      run_repeats=args.run_repeats if args.run_repeats is not None else DEFAULTS["run_repeats"],
                      jobs_arms=args.jobs_arms if args.jobs_arms is not None else DEFAULTS["jobs_arms"],
                      out_dir=out_dir)
    log.info("plan      : %s  (%d TUs, %d defines, %d include dirs, %d reference arm(s))"
             % (plan_path, len(plan["subject"]["tus"]), len(plan["subject"]["defines"]),
                len(plan["subject"]["includes"]), len(refs)))
    if args.derive_only:
        log.ok("derivation complete %s the plan is at %s" % (DASH, plan_path))
        log.info("(--derive-only: the measurement is the caller's: speedtest1_bench.py --plan <it>)")
        return 0
    log.step("4/4  Measure (natively)")
    return measure(plan_path, out_dir)


def run_derive_half(args, log=C.LOG):
    """`--derive-only --path-style windows` on a POSIX host: the derive half of a Windows measuring
    host. It resolves only what the derivation needs, and leaves the manifest (spelled for Windows)
    and `<out>/derive-result.json` behind for the host that writes the plan and measures."""
    host, arch = C.host_os(), C.host_arch()
    log.step("Derive half %s the POSIX side of a Windows measuring host (%s/%s)" % (DASH, host, arch))
    sqlite_dir = os.path.abspath(args.sqlite_dir)
    out_dir = os.path.abspath(args.out)
    check_subject(sqlite_dir)
    require_tools(DERIVE_TOOLS + ("wslpath",))
    resolver = C.Resolver(host, arch)
    if args.recipe_transform is None or args.stack_reserve is None:
        facts = leg_facts(leg_catalogue(resolver), args.target)
    else:
        facts = LegFacts("<the measuring host's>", args.target, None, None, [])
    facts = facts._replace(
        transform=args.recipe_transform if args.recipe_transform is not None else facts.transform,
        reserve=args.stack_reserve if args.stack_reserve is not None else facts.reserve)
    log.info("sqlite    : %s" % sqlite_dir)
    log.info("output    : %s" % out_dir)
    log.info("target    : %s  (recipe transform %s, stack reserve %s)" % (facts.spec, facts.transform, facts.reserve))
    log.info("dss pre-flight: not here %s the compiler is native to the CALLING host, which runs it" % DASH)
    os.makedirs(out_dir, exist_ok=True)
    stage = stage_build(resolver)
    spell = WindowsSpelling()
    derived = derive(DeriveConfig(sqlite_dir, out_dir, facts.spec, facts.transform, facts.reserve,
                                  stage, C.cpu_count(), "windows", default_tools(sqlite_dir), spell, log))
    result = os.path.join(out_dir, DERIVE_RESULT)
    doc = write_derive_result(result, derived, facts, spell)
    log.ok("derivation complete %s %s (manifest %s)" % (DASH, result, doc["manifest"]))
    return 0


# ── CLI ──────────────────────────────────────────────────────────────────────────────

class _Parser(argparse.ArgumentParser):
    def error(self, message):
        raise UsageError(message)


def _int_at_least(minimum, what):
    def conv(text):
        if not re.fullmatch(r"[0-9]+", text or ""):
            raise argparse.ArgumentTypeError("%s must be an integer >= %d (got '%s')" % (what, minimum, text))
        n = int(text)
        if n < minimum:
            raise argparse.ArgumentTypeError("%s must be >= %d (got %d)" % (what, minimum, n))
        return n
    return conv


def _jobs_arms(text):
    toks = C.split_list(text or "")
    if not toks:
        raise argparse.ArgumentTypeError("--jobs-arms needs at least one worker count (got '%s')" % text)
    conv = _int_at_least(1, "a --jobs-arms worker count")
    return [conv(t) for t in toks]


def _nonempty(what):
    def conv(text):
        if not (text or "").strip():
            raise argparse.ArgumentTypeError("%s must not be empty" % what)
        return text
    return conv


def build_parser():
    p = _Parser(prog="benchmark_speedtest1.py", add_help=False, allow_abbrev=False)
    for flag in ("--sqlite-dir", "--dss-src", "--dss", "--out", "--plan", "--target", "--cc"):
        p.add_argument(flag, type=_nonempty(flag))
    p.add_argument("--testset")
    p.add_argument("--size", type=_int_at_least(1, "--size"))
    p.add_argument("--build-repeats", type=_int_at_least(1, "--build-repeats"))
    p.add_argument("--run-repeats", type=_int_at_least(1, "--run-repeats"))
    p.add_argument("--jobs-arms", type=_jobs_arms)
    p.add_argument("--recipe-transform", type=_nonempty("--recipe-transform"))
    p.add_argument("--stack-reserve", type=_int_at_least(0, "--stack-reserve"))
    p.add_argument("--derive-only", action="store_true")
    p.add_argument("--path-style", choices=("posix", "windows"))
    p.add_argument("--self-test", "--selftest", dest="self_test", action="store_true")
    p.add_argument("-h", "--help", dest="help", action="store_true")
    return p


def parse_args(argv):
    args = build_parser().parse_args(list(argv))
    if args.self_test and len(argv) != 1:
        raise UsageError("--self-test takes no other argument")
    if args.recipe_transform is not None and args.recipe_transform not in generator_transforms():
        raise UsageError("--recipe-transform '%s' is not a transform the manifest generator implements "
                         "(%s)" % (args.recipe_transform, ", ".join(generator_transforms())))
    return args


def validate_mode(args, host):
    """-> the path style this invocation runs under, or UsageError."""
    style = args.path_style or ("windows" if host == "windows" else "posix")
    if host == "windows" and style == "posix":
        raise UsageError("--path-style posix on a Windows host: the plan this host measures carries "
                         "WINDOWS paths; the POSIX spelling exists only inside the derive half (WSL)")
    if host != "windows" and style == "windows":
        if not args.derive_only:
            raise UsageError("--path-style windows makes this process the DERIVE HALF of a Windows "
                             "measuring host, which never measures: it needs --derive-only")
        given = [flag for attr, flag in MEASURING_HOST_FLAGS if getattr(args, attr) is not None]
        if given:
            raise UsageError("%s belong%s to the MEASURING host, not to the derive half "
                             "(--path-style windows): the Windows host resolves the compilers and "
                             "writes the plan" % (", ".join(given), "s" if len(given) == 1 else ""))
        missing = [flag for attr, flag in (("sqlite_dir", "--sqlite-dir"), ("out", "--out"),
                                           ("target", "--target")) if not getattr(args, attr)]
        if missing:
            raise UsageError("the derive half (--path-style windows) needs %s from the measuring "
                             "host; it never defaults them" % ", ".join(missing))
    return style


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    try:
        args = parse_args(argv)
        if args.help:
            print(USAGE)
            return 0
        if args.self_test:
            return self_test()
        host = C.host_os()
        style = validate_mode(args, host)
    except UsageError as exc:
        print(" ✗ USAGE: %s\n   (--help lists what this accepts)" % exc, file=sys.stderr, flush=True)
        return 2
    except C.HarnessDie as exc:
        print(" ✗ ERROR: %s" % exc, file=sys.stderr, flush=True)
        return exc.exit_code
    try:
        if host != "windows" and style == "windows":
            return run_derive_half(args)
        return run_measuring_host(args, host)
    except C.HarnessDie as exc:
        print(" ✗ ERROR: %s" % exc, file=sys.stderr, flush=True)
        return exc.exit_code
    except KeyboardInterrupt:
        print(" ✗ ERROR: interrupted", file=sys.stderr, flush=True)
        return 130
    except Exception:  # noqa: BLE001 -- never a silent exit
        print(" ✗ ERROR: the benchmark failed with an unexpected exception:\n%s"
              % traceback.format_exc(), file=sys.stderr, flush=True)
        return 1


# ── SELF-TEST -- red-on-disable by construction ──────────────────────────────────────
#
# Labels: `shNN` = the retired .sh's 22 checks (labels kept; sh21 now demands the MAKE-FAILURE
# class), `psNN` = the retired .ps1's 6, `core01` = the measurement core's own self-test (the .sh
# counted it as one arm), `nNN` = new. Every external program is a real CHILD PROCESS: a fake
# make / configure / ar / measurement core is a Python script run by this interpreter; the real
# `make`, `wsl.exe` and the resolver run where they exist, and an arm that needs what this host
# lacks is a NAMED, counted SKIP. An arm asserting an ABSENCE also proves its negative can occur.

EXPECTED_ARMS = 105        # 22 sh + 6 ps + 1 core + 76 new

_SKIP = object()

_FAKE_MAKE = r'''import json, os, sys
cfg_path, a = sys.argv[1], sys.argv[2:]
with open(cfg_path, encoding="utf-8") as fh:
    cfg = json.load(fh)
if cfg.get("record"):
    with open(cfg["record"], "a", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps({"argv": a, "cwd": os.getcwd()}) + "\n")
if "__dss_texe_probe" in a:
    if cfg.get("texe") is not None:
        sys.stdout.write(cfg["texe"] + "\n")
    sys.exit(cfg.get("texe_exit", 0))
if "-n" in a:
    with open(cfg["recipe"], "rb") as fh:
        sys.stdout.buffer.write(fh.read())
    sys.exit(0)
for name, text in sorted(cfg.get("generate", {}).items()):
    with open(os.path.join(os.getcwd(), name), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
sys.exit(cfg.get("build_exit", 0))
'''

_FAKE_CONFIGURE = r'''import json, os, sys
with open(sys.argv[1], encoding="utf-8") as fh:
    cfg = json.load(fh)
if cfg.get("record"):
    with open(cfg["record"], "w", encoding="utf-8", newline="\n") as fh:
        json.dump({"argv": sys.argv[2:], "cwd": os.getcwd()}, fh)
with open("Makefile", "w", encoding="utf-8", newline="\n") as fh:
    fh.write("T.exe =\n")
sys.exit(cfg.get("exit", 0))
'''

_FAKE_CORE = r'''import sys
sys.stdout.write("fake measurement core: exit %d\n")
sys.exit(%d)
'''

_SILENT = "import sys\nsys.exit(0)\n"


class _Arms:
    def __init__(self):
        self.passed = self.failed = self.skipped = self.ran = 0
        self.labels = set()

    def arm(self, label, fn):
        """One counted arm: `fn()` returns a bool, (bool, detail), or (_SKIP, why)."""
        self.ran += 1
        if label in self.labels:
            self._fail(label, "DUPLICATE arm label (the count would lie)")
            return
        self.labels.add(label)
        try:
            r = fn()
        except Exception as exc:  # an arm that crashes is a FAILED arm, never a silent one
            self._fail(label, "raised %s: %s" % (type(exc).__name__, exc))
            return
        ok, detail = r if isinstance(r, tuple) else (r, "")
        if ok is _SKIP:
            self.skipped += 1
            print("  [SKIP] %s %s %s" % (label, DASH, detail), flush=True)
        elif ok:
            self.passed += 1
            print("  [PASS] %s" % label, flush=True)
        else:
            self._fail(label, detail)

    def _fail(self, label, detail):
        self.failed += 1
        print("  [FAIL] %s" % label)
        for line in str(detail).split("\n"):
            if line:
                print("         %s" % line)
        sys.stdout.flush()

    def section(self, name, fn, fx):
        try:
            fn(self, fx)
        except Exception:
            self.failed += 1
            print("  [FAIL] section %s CRASHED (its remaining arms did not run):" % name)
            for line in traceback.format_exc().rstrip().split("\n"):
                print("         %s" % line)

    def finish(self, expected):
        if self.ran != expected:
            self.failed += 1
            print("  [FAIL] ran %d arm(s), but EXPECTED_ARMS declares %d %s an arm was added, "
                  "removed or never reached" % (self.ran, expected, DASH))
        print("\npassed=%d failed=%d skipped=%d" % (self.passed, self.failed, self.skipped))
        return 0 if self.failed == 0 else 1


def _eq(want, got):
    return want == got, "want: %r\ngot : %r" % (want, got)


def _dies(fn):
    """The HarnessDie message `fn()` raised, or None when it returned."""
    try:
        fn()
    except C.HarnessDie as exc:
        return str(exc)
    return None


def _raised(fn, exc_type):
    try:
        fn()
    except exc_type as exc:
        return exc
    return None


def _put(path, data, mode=None):
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    with open(path, "wb") as fh:
        fh.write(data if isinstance(data, bytes) else data.encode("utf-8"))
    if mode is not None:
        os.chmod(path, mode)


@contextlib.contextmanager
def _env(**kw):
    """os.environ with each named variable set (a str) or DELETED (None), restored afterwards."""
    saved = {k: os.environ.get(k) for k in kw}
    try:
        for k, v in kw.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        yield
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def _run_main(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = main(list(argv))
    return rc, out.getvalue(), err.getvalue()


_REQUIRED = ["SQLITE_ENABLE_COLUMN_METADATA", "SQLITE_ENABLE_FTS3", "SQLITE_ENABLE_FTS4",
             "SQLITE_ENABLE_FTS5", "SQLITE_ENABLE_GEOPOLY", "SQLITE_ENABLE_MEMSYS5",
             "SQLITE_ENABLE_PREUPDATE_HOOK", "SQLITE_ENABLE_RTREE", "SQLITE_ENABLE_SESSION",
             "SQLITE_ENABLE_STAT4", "SQLITE_ENABLE_STMT_SCANSTATUS",
             "SQLITE_ENABLE_UPDATE_DELETE_LIMIT"]
_STAGE_FIXTURE = {"configure_flags": ["--enable-all", "--fts3"],
                  "make_options": "-DSQLITE_ENABLE_STAT4", "required_defines": list(_REQUIRED),
                  "witnesses": {}, "option_defines": ["SQLITE_ENABLE_STAT4"]}
_GCC_VER = "gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0"
_APPLE_VER = "Apple clang version 21.0.0 (clang-2100.1.1.101)"
_MINGW_VER = "gcc.exe (MinGW-W64 x86_64-ucrt-posix-seh, built by Brecht Sanders, r3) 13.2.0"
_CLANG_VER = "clang version 18.1.3 (1ubuntu1)"


class _Fx:
    """The self-test's scratch world: one temp root, the fake programs, fresh subdirectories."""

    def __init__(self, root):
        self.root, self.n = root, 0
        self.py = sys.executable
        self.host = C.host_os()
        self.fake_make = self.script("fake_make.py", _FAKE_MAKE)
        self.fake_configure = self.script("fake_configure.py", _FAKE_CONFIGURE)
        self.silent = self.script("silent.py", _SILENT)
        self.cores = {rc: self.script("core_%d.py" % rc, _FAKE_CORE % (rc, rc)) for rc in (0, 1, 3)}
        self.make = shutil.which("make")
        self.wsl = shutil.which("wsl.exe") if self.host == "windows" else None
        self._legs = {}

    def script(self, name, src):
        path = os.path.join(self.root, name)
        _put(path, src)
        return path

    def fresh(self, tag):
        self.n += 1
        d = os.path.join(self.root, "%03d-%s" % (self.n, tag))
        os.makedirs(d)
        return d

    @staticmethod
    def log():
        buf = io.StringIO()
        return C.Log(stream=buf), buf

    def archive_text(self, members):
        """A real GNU-format archive holding `members` (each body its name), as TEXT -- every byte
        of it is ASCII -- for the fake make to write where the reference build would."""
        path = os.path.join(self.fresh("archive"), "lib.a")
        BASE._write_gnu_ar(path, [(m, m.encode("ascii")) for m in members])
        with open(path, "rb") as fh:
            return fh.read().decode("ascii")

    def archive(self, members):
        """The same archive as a FILE, for the arms that hand refuse_amalgamation a path."""
        path = os.path.join(self.fresh("archive"), "libsqlite3.a")
        BASE._write_gnu_ar(path, [(m, m.encode("ascii")) for m in members])
        return path

    def legs(self, host_os, host_arch):
        key = (host_os, host_arch)
        if key not in self._legs:
            self._legs[key] = leg_catalogue(C.Resolver(host_os, host_arch))
        return self._legs[key]

    def dss_tree(self, repo, name, cache, rel_bin, mtime):
        root = os.path.join(repo, "build", name)
        _put(os.path.join(root, "CMakeCache.txt"), "\n".join(cache) + "\n")
        b = os.path.join(root, *rel_bin.split("/"))
        _put(b, "fake dsscp\n", 0o755)
        os.utime(b, (mtime, mtime))
        return os.path.abspath(b)

    def e2e(self, tag, *, drop=(), texe="__dss_texe=<> origin=<file>", configure_exit=0,
            build_exit=0, no_shell=False, generate_h=True, members=None, extra_link=(),
            style="posix", spell=None):
        """A complete fake SQLite world: 104 core sources recovered from a fake archive, the
        CLI's shell.c generated by the fake reference build, ≥18 defines, a real manifest generator."""
        root = self.fresh(tag)
        sqlite = os.path.join(root, "sqlite")
        names = ["t%03d" % i for i in range(104)]
        for n in names + ["zz_notlinked"]:
            _put(os.path.join(sqlite, "src", n + ".c"), "int %s;\n" % n)
        _put(os.path.join(sqlite, "ext", "misc", "e.c"), "int e;\n")
        _put(os.path.join(sqlite, "test", MAIN_TU), "int main(void){return 0;}\n")
        defines = [d for d in ([CORE_DEFINE] + _REQUIRED + [
            "NDEBUG", "SQLITE_THREADSAFE=1", "_HAVE_SQLITE_CONFIG_H", "SQLITE_DQS=0",
            "SQLITE_ENABLE_MATH_FUNCTIONS", "SQLITE_ENABLE_DBPAGE_VTAB", "SQLITE_ENABLE_DBSTAT_VTAB"])
            if d not in drop]
        compile_line = "cc %s -I. -Isrc -Iext -c src/t000.c -o t000.o" % " ".join("-D" + d for d in defines)
        objects = [n + ".o" for n in names] + list(extra_link)
        link = "cc -DSQLITE_THREADSAFE=1 -o sqlite3d %s%s -lm" % (
            "" if no_shell else "shell.c ", " ".join(objects))
        recipe = os.path.join(root, "recipe.txt")
        _put(recipe, "cc -o jimsh /tool/jimsh0.c\n%s\n%s\n" % (compile_line, link))
        gen = {"shell.c": "int main(void){return 0;}\n",
               "libsqlite3.a": self.archive_text(members if members is not None
                                                 else objects + ["zz_notlinked.o"])}
        if generate_h:
            gen["sqlite3.h"] = "#define SQLITE_VERSION \"x\"\n"
        rec = os.path.join(root, "make-calls.jsonl")
        make_cfg = os.path.join(root, "make.json")
        _put(make_cfg, json.dumps({"record": rec, "recipe": recipe, "texe": texe,
                                   "generate": gen, "build_exit": build_exit}))
        conf_rec = os.path.join(root, "configure-call.json")
        conf_cfg = os.path.join(root, "configure.json")
        _put(conf_cfg, json.dumps({"record": conf_rec, "exit": configure_exit}))
        tools = Tools((self.py, self.fake_make, make_cfg), (self.py, self.fake_configure, conf_cfg),
                      self.py, C.MANIFEST_GEN)
        log, buf = self.log()
        cfg = DeriveConfig(sqlite, os.path.join(root, "out"), "x86_64:elf64-x86_64-linux-exec", "none",
                           0, dict(_STAGE_FIXTURE), 2, style, tools, spell, log)
        return cfg, {"root": root, "make_rec": rec, "conf_rec": conf_rec, "buf": buf, "sqlite": sqlite}


def _calls(path):
    with open(path, encoding="utf-8") as fh:
        return [json.loads(ln) for ln in fh if ln.strip()]


def _same_dir(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


# ── the .sh's 22 ─────────────────────────────────────────────────────────────────────

def _st_substitution(A, fx):
    d = fx.fresh("subst")
    st1 = "/s/test/speedtest1.c"
    t1 = os.path.join(d, "tus")
    _write_list(t1, ["/s/src/main.c", "/s/src/shell.c", "/s/src/util.c"])
    e1 = _raised(lambda: substitute_main_tu(t1, st1), SubstitutionRefused)
    after = _read_list(t1)
    A.arm("sh01 substitution reports success on a well-formed TU list",
          lambda: (e1 is None, "raised %r" % e1))
    A.arm("sh02 shell.c is gone", lambda: (not any(t.endswith("/shell.c") for t in after), after))
    A.arm("sh03 speedtest1.c is in", lambda: (st1 in after, after))
    A.arm("sh04 the other TUs survive (and nothing else changed)",
          lambda: _eq(["/s/src/main.c", "/s/src/util.c", st1], after))
    t2 = os.path.join(d, "tus2")
    _write_list(t2, ["/s/src/main.c", "/s/src/util.c"])
    e2 = _raised(lambda: substitute_main_tu(t2, st1), SubstitutionRefused)
    A.arm("sh05 a TU list with no shell.c is REFUSED (code 3, the list untouched)",
          lambda: (e2 is not None and e2.code == 3 and _read_list(t2) == ["/s/src/main.c", "/s/src/util.c"],
                   "raised %r, list %r" % (e2, _read_list(t2))))
    t3 = os.path.join(d, "tus3")
    _write_list(t3, ["/s/src/shell.c", "/s/x/shell.c"])
    e3 = _raised(lambda: substitute_main_tu(t3, st1), SubstitutionRefused)
    A.arm("sh06 a TU list with TWO shell.c is REFUSED (code 3)",
          lambda: (e3 is not None and e3.code == 3, "raised %r" % e3))


def _st_presence(A, fx):
    A.arm("sh07 the shared measurement core is present",
          lambda: (os.path.isfile(C.BENCH_CORE), C.BENCH_CORE))
    A.arm("sh08 the manifest generator is present",
          lambda: (os.path.isfile(C.MANIFEST_GEN), C.MANIFEST_GEN))
    A.arm("sh09 the shared harness core exports the recipe derivation (sqlite_base.emit_recipe, "
          "generate_manifest, archive_members)",
          lambda: all(callable(getattr(BASE, n, None)) for n in
                      ("emit_recipe", "generate_manifest", "archive_members", "manifest_error")))


def _st_label(A, fx):
    def lbl(invoked, ver, want):
        return lambda: _eq(want, reference_label(invoked, ver))
    A.arm("sh10 gcc invoked as 'gcc' stays 'gcc'", lbl("gcc", _GCC_VER, "gcc"))
    A.arm("sh11 ★ 'cc' that IS gcc is labelled 'gcc', not 'cc'", lbl("cc", _GCC_VER, "gcc"))
    A.arm("sh12 'gcc' that is Apple clang is labelled 'Apple clang'", lbl("gcc", _APPLE_VER, "Apple clang"))
    A.arm("sh13 'cc' that is Apple clang is labelled 'Apple clang'", lbl("cc", _APPLE_VER, "Apple clang"))
    A.arm("sh14 clang invoked as 'clang' stays 'clang'", lbl("clang", _CLANG_VER, "clang"))
    A.arm("sh15 MinGW 'gcc.exe' still labels 'gcc'", lbl("gcc", _MINGW_VER, "gcc"))
    A.arm("sh16 tcc invoked as 'tcc' stays 'tcc'", lbl("tcc", "tcc version 0.9.27 (x86_64 Linux)", "tcc"))
    A.arm("sh17 an empty version line falls back to the invoked name", lbl("gcc", "", "gcc"))


def _st_texe_real(A, fx):
    labels = ("sh18 a Windows-configured Makefile answers '.exe' (the REAL make)",
              "sh19 a POSIX Makefile answers EMPTY, and that is success (the REAL make)",
              "sh20 ★ a Makefile that defines no T.exe is REFUSED (code 4), never read as empty",
              "sh21 a build dir with no Makefile at all is REFUSED as a MAKE FAILURE (code 5), not "
              "as 'no T.exe' (code 4)",
              "sh22 the probe leaves its makefile behind for a reader")
    if not fx.make:
        for label in labels:
            A.arm(label, lambda: (_SKIP, "no make on this PATH (the derive's own host runs these; on "
                                         "Windows that is WSL's --self-test)"))
        return
    d = fx.fresh("texe")
    for sub, text in (("mk-win", "T.exe = .exe\nall:\n"), ("mk-posix", "T.exe =\nall:\n"),
                      ("mk-other", "NOT_SQLITE = 1\nall:\n")):
        _put(os.path.join(d, sub, "Makefile"), text)
    os.makedirs(os.path.join(d, "mk-none"))

    def probe(sub, mk):
        try:
            return 0, derive_make_texe(os.path.join(d, sub), os.path.join(d, mk))
        except TexeRefused as exc:
            return exc.code, exc.transcript
    r1, r2, r3, r4 = probe("mk-win", "p1.mk"), probe("mk-posix", "p2.mk"), \
        probe("mk-other", "p3.mk"), probe("mk-none", "p4.mk")
    A.arm(labels[0], lambda: _eq((0, ".exe"), r1))
    A.arm(labels[1], lambda: _eq((0, ""), r2))
    A.arm(labels[2], lambda: (r3[0] == 4, "got %r" % (r3,)))
    A.arm(labels[3], lambda: (r4[0] == 5, "got %r" % (r4,)))
    A.arm(labels[4], lambda: (os.path.getsize(os.path.join(d, "p1.mk")) > 0, "p1.mk is empty"))


# ── the .ps1's 6 ─────────────────────────────────────────────────────────────────────

class _FakePosix:
    """A stand-in for sqlite_common.PosixSide on the Windows side of the hop."""

    def __init__(self):
        self.asked = []

    def to_posix(self, path):
        self.asked.append(path)
        return "/fake" + path.replace("\\", "/")

    @staticmethod
    def argv(args):
        return ["wsl.exe", "-e"] + [str(a) for a in args]


def _st_carriage(A, fx):
    posix, out = _FakePosix(), fx.fresh("hop-out")
    src = fx.fresh("hop-src")
    facts = LegFacts("pe64-x86_64", "x86_64:pe64-x86_64-windows-exec", "windows-selfconfig", 8388608, [])
    seen = {}

    def half(argv):
        """The derive half, faked: it leaves exactly what the real one leaves."""
        seen["argv"] = list(argv)
        tu = os.path.join(src, "a.c")
        _put(tu, "int a;\n")
        man = os.path.join(out, MANIFEST_FILE)
        _write_json(man, {"sources": [tu], "includes": [src], "defines": ["X"]})
        _write_json(os.path.join(out, DERIVE_RESULT),
                    {"schema": DERIVE_SCHEMA, "target": facts.spec, "manifest": man})
        return 0, []
    log, _buf = fx.log()
    doc = wsl_derive(posix, fx.root, out, facts, log, run=half)
    A.arm("ps01 the POSIX derivation driver is present: it is THIS program (the hop runs this file's "
          "--derive-only --path-style windows) and its result is read back",
          lambda: (seen["argv"][2:7] == ["python3", posix.to_posix(THIS), "--derive-only",
                                         "--path-style", "windows"]
                   and doc["manifest"] == os.path.join(out, MANIFEST_FILE), "argv %r" % seen.get("argv")))
    r = C.capture([fx.py, C.BENCH_CORE, "--help"], timeout=120, env_=C.child_env(python=True))
    A.arm("ps02 the shared measurement core runs under THIS interpreter (--help exits 0)",
          lambda: (r.rc == 0 and "--plan" in r.out, "rc=%d %s" % (r.rc, C.first_lines(r.err or r.out, 3))))
    if fx.host != "windows":
        why = "not a Windows host: the POSIX half runs in this process, no carriage is used"
        A.arm("ps03 wsl.exe is reachable (this host can derive)", lambda: (_SKIP, why))
        A.arm("ps04 the WSL carriage answers", lambda: (_SKIP, why))
        A.arm("ps05 wsl.exe -e suppresses the second expansion", lambda: (_SKIP, why))
    else:
        A.arm("ps03 wsl.exe is reachable (this host can derive) (control: an empty PATH finds none)",
              lambda: (bool(fx.wsl) and shutil.which("wsl.exe", path="") is None, "which: %r" % fx.wsl))
        if not fx.wsl:
            A.arm("ps04 the WSL carriage answers", lambda: (_SKIP, "no wsl.exe on this host"))
            A.arm("ps05 wsl.exe -e suppresses the second expansion", lambda: (_SKIP, "no wsl.exe on this host"))
        else:
            side = C.PosixSide("windows")

            def answers():
                ok = C.capture(side.argv(["echo", "posix-ok"]), timeout=120)
                bad = C.capture(side.argv(["false"]), timeout=120)
                return (ok.rc == 0 and "posix-ok" in ok.out and bad.rc != 0 and "posix-ok" not in bad.out,
                        "echo: rc=%d %r / false: rc=%d" % (ok.rc, ok.out[:80], bad.rc))
            A.arm("ps04 the WSL carriage answers (control: a failing command fails through it)", answers)

            def literal():
                lit = C.capture(side.argv(["printf", "[%s]\\n", "echo A=$(uname -m)"]), timeout=120)
                shell = C.capture(side.argv(["sh", "-c", 'printf "[%s]\\n" "echo A=$(uname -m)"']),
                                  timeout=120)
                return ("$(uname -m)" in lit.out and "$(uname" not in shell.out and "A=" in shell.out,
                        "-e: %r / a shell inside WSL: %r" % (lit.out[:80], shell.out[:80]))
            A.arm("ps05 wsl.exe -e suppresses the second expansion (control: a shell inside WSL DOES "
                  "expand it, so an expansion is visible)", literal)
    ok_py = C.capture([fx.py, "-c", "print('py-ok')"], timeout=60)
    missing_py = C.capture([os.path.join(fx.root, "no-such-python"), "-c", "print(1)"], timeout=60)
    A.arm("ps06 a native python is reachable (the measurement runs here; control: a missing one is "
          "rc 127, not a pass)",
          lambda: (ok_py.rc == 0 and "py-ok" in ok_py.out and missing_py.rc == 127,
                   "rc %d / %d" % (ok_py.rc, missing_py.rc)))


def _st_core(A, fx):
    r = C.capture([fx.py, C.BENCH_CORE, "--selftest"], timeout=600, env_=C.child_env(python=True))
    # The core's FAILING arms are named first: the tail alone left a red arm above it unnamed.
    failing = [ln.strip() for ln in (r.out + r.err).splitlines() if ln.strip().startswith("FAIL")]
    A.arm("core01 speedtest1_bench.py --selftest passes (the measurement core's own arms)",
          lambda: (r.rc == 0, "rc=%d; failing arm(s): %s\n%s"
                   % (r.rc, "; ".join(failing) or "<none named>", C.last_lines(r.out + r.err, 12))))


# ── new: substitution post-conditions and the fake-make probe ────────────────────────

def _st_post_conditions(A, fx):
    d = fx.fresh("post")
    st1 = "/s/test/speedtest1.c"
    t4 = os.path.join(d, "tus4")
    _write_list(t4, ["/s/src/a.c", "/s/src/shell.c"])
    e4 = _raised(lambda: substitute_main_tu(t4, st1, write=lambda p, items: _write_list(
        p, items + ["/s/src/shell.c"])), SubstitutionRefused)
    A.arm("n01 a rewrite that left shell.c in place is CAUGHT on re-read (code 4)",
          lambda: (e4 is not None and e4.code == 4, "raised %r" % e4))
    t5 = os.path.join(d, "tus5")
    _write_list(t5, ["/s/src/a.c", "/s/src/shell.c"])
    e5 = _raised(lambda: substitute_main_tu(t5, st1, write=lambda p, items: _write_list(
        p, [i for i in items if i != st1])), SubstitutionRefused)
    A.arm("n02 a rewrite that lost speedtest1.c is CAUGHT on re-read (code 5)",
          lambda: (e5 is not None and e5.code == 5, "raised %r" % e5))
    silent = (fx.py, fx.silent)
    e3 = _raised(lambda: derive_make_texe(d, os.path.join(d, "s.mk"), silent), TexeRefused)
    A.arm("n03 a make that prints NO answer (a non-GNU make) is REFUSED as code 3, never read as empty",
          lambda: (e3 is not None and e3.code == 3, "raised %r" % e3))
    cfg = os.path.join(d, "mk.json")
    _put(cfg, json.dumps({"texe": "__dss_texe=<.exe> origin=<file>\r"}))
    got = derive_make_texe(d, os.path.join(d, "c.mk"), (fx.py, fx.fake_make, cfg))
    A.arm("n04   (control) the same probe reads an answer when make prints one, even CR-terminated",
          lambda: _eq(".exe", got))
    bad_mk = os.path.join(d, "no-such-dir", "p.mk")
    e5b = _raised(lambda: derive_make_texe(d, bad_mk, silent), TexeRefused)
    A.arm("n05 a probe makefile that cannot be written is a MAKE-class refusal (code 5)",
          lambda: (e5b is not None and e5b.code == 5, "raised %r" % e5b))


# ── new: the reference catalogue ─────────────────────────────────────────────────────

def _disc(which_map, versions, real=None, pinned="", by=""):
    return discover_references(pinned, by, which=lambda n: which_map.get(n),
                               version_of=lambda p: versions.get(p, ("", "exit 1")),
                               realpath=lambda p: (real or {}).get(p, p))


def _st_references(A, fx):
    refs, skips = _disc({"gcc": "/u/gcc"}, {"/u/gcc": (_GCC_VER, "")})
    absent = {s.id: s for s in skips if s.state == "ABSENT"}
    A.arm("n06 an ABSENT catalogue name is recorded with its cause (control: the one that resolves "
          "is measured)",
          lambda: (sorted(absent) == ["cc", "clang", "tcc"] and "no 'tcc' resolves on PATH" in absent["tcc"].why
                   and "Tiny C Compiler" in absent["tcc"].why and [r.id for r in refs] == ["gcc"],
                   "refs %r skips %r" % (refs, skips)))
    refs, skips = _disc({"gcc": "/u/gcc", "tcc": "/u/tcc"}, {"/u/gcc": (_GCC_VER, "")})
    un = [s for s in skips if s.state == "UNUSABLE"]
    A.arm("n07 a compiler that will not answer --version is UNUSABLE, named with its reason",
          lambda: (len(un) == 1 and un[0].id == "tcc" and "would not answer '--version'" in un[0].why
                   and "exit 1" in un[0].why, "skips %r" % skips))
    refs, skips = _disc({"gcc": "/u/gcc", "cc": "/u/cc"}, {"/u/gcc": (_GCC_VER, ""), "/u/cc": (_GCC_VER, "")},
                        real={"/u/cc": "/u/gcc"})
    dup = [s for s in skips if s.state == "DUPLICATE"]
    A.arm("n08 a name resolving to an already-measured PATH is DUPLICATE (by path)",
          lambda: (len(dup) == 1 and dup[0].id == "cc" and "resolves to /u/gcc" in dup[0].why
                   and len(refs) == 1, "skips %r" % skips))
    refs, skips = _disc({"gcc": "/u/gcc", "clang": "/u/clang"},
                        {"/u/gcc": (_APPLE_VER, ""), "/u/clang": (_APPLE_VER, "")})
    refs2, _s2 = _disc({"gcc": "/u/gcc", "clang": "/u/clang"},
                       {"/u/gcc": (_GCC_VER, ""), "/u/clang": (_CLANG_VER, "")})
    dup = [s for s in skips if s.state == "DUPLICATE"]
    A.arm("n09 two DISTINCT files with one version line are one compiler: DUPLICATE by version "
          "(control: different versions are both measured)",
          lambda: (len(dup) == 1 and dup[0].id == "clang" and "same compiler" in dup[0].why
                   and refs[0].label == "Apple clang" and len(refs2) == 2, "skips %r / %r" % (skips, refs2)))
    refs, skips = _disc({}, {"/p/gcc": (_GCC_VER, "")}, pinned="/p/gcc", by="--cc")
    refs_cc, skips_cc = _disc({}, {"/p/gcc": (_GCC_VER, "")}, pinned="/p/gcc",
                              by="the CC environment variable")
    A.arm("n10 a pinned reference records the catalogue NOT PROBED, naming WHICH setting pinned it "
          "(--cc, or the CC variable -- the .sh blamed --cc for both)",
          lambda: (skips[0].state == "NOT PROBED" and skips[0].why.startswith("--cc pinned")
                   and skips_cc[0].why.startswith("the CC environment variable pinned")
                   and skips[0].id == "gcc clang cc tcc" and [r.bin for r in refs] == ["/p/gcc"],
                   "%r / %r" % (skips, skips_cc)))
    none = _dies(lambda: _disc({}, {}))
    A.arm("n11 no reference resolving at all is REFUSED, listing what was not measured",
          lambda: (none is not None and "no reference C compiler resolved" in none
                   and none.count("ABSENT") == 4, none))
    unusable = _dies(lambda: _disc({"gcc": "/u/gcc"}, {}))
    A.arm("n12 references that all refuse --version are REFUSED as NONE USABLE (the .sh called "
          "that a discovery bug)",
          lambda: (unusable is not None and "is USABLE" in unusable and "UNUSABLE" in unusable
                   and "bug" not in unusable, unusable))
    log, buf = fx.log()
    report_references([Reference("gcc", "/u/gcc", "gcc", _GCC_VER)], [], log)
    log2, buf2 = fx.log()
    report_references([Reference("gcc", "/u/gcc", "Apple clang", _APPLE_VER)],
                      [Skip("tcc", "ABSENT", "x")], log2)
    A.arm("n13 the not-measured list is printed UNCONDITIONALLY: 'none' is an answer (control: a "
          "skip is listed; a relabelled binary says so)",
          lambda: ("not measured: none" in buf.getvalue() and "not measured: tcc" in buf2.getvalue()
                   and "labelled 'Apple clang'" in buf2.getvalue(), buf.getvalue() + buf2.getvalue()))
    got = resolve_pinned("gcc", "--cc", which=lambda n: "/u/bin/gcc" if n == "gcc" else None,
                         isfile=lambda p: False)
    bad = _dies(lambda: resolve_pinned("gcc -m32", "the CC environment variable", which=lambda n: None,
                                       isfile=lambda p: False))
    A.arm("n14 a pinned NAME resolves on PATH; a pin naming no single program is refused naming "
          "the setting",
          lambda: (got == "/u/bin/gcc" and bad is not None and "the CC environment variable" in bad
                   and "'gcc -m32'" in bad, "%r / %r" % (got, bad)))
    A.arm("n15 short_version takes the FIRST dotted number of all three shipped spellings",
          lambda: _eq(["13.3.0", "13.2.0", "18.1.3", ""],
                      [short_version(_GCC_VER), short_version(_MINGW_VER), short_version(_CLANG_VER),
                       short_version("")]))


# ── new: which dsscp ─────────────────────────────────────────────────────────────────

def _st_dsscp(A, fx):
    repo = fx.fresh("repo")
    rel = fx.dss_tree(repo, "rel", ["CMAKE_BUILD_TYPE:STRING=Release"], "bin/dss/dsscp", 1000)
    fx.dss_tree(repo, "dbg", ["CMAKE_BUILD_TYPE:STRING=Debug"], "bin/dss/dsscp", 3000)
    bench = fx.dss_tree(repo, "bench-rel", ["CMAKE_BUILD_TYPE:STRING=Release"], "bin/dss/dsscp", 2000)
    log, buf = fx.log()
    got = select_dss(repo, "", "", False, log)
    A.arm("n16 the NEWEST Release dsscp is selected, its build type READ from its own CMakeCache "
          "and printed beside the path",
          lambda: (os.path.normcase(got.path) == os.path.normcase(bench) and got.type == "Release"
                   and "build type: Release" in buf.getvalue(), "%r\n%s" % (got, buf.getvalue())))
    outside = os.path.join(repo, "elsewhere", "bin", "dss", "dsscp")
    _put(outside, "fake dsscp\n", 0o755)
    _put(os.path.join(repo, "elsewhere", "CMakeCache.txt"), "CMAKE_BUILD_TYPE:STRING=Release\n")
    base_cands, _s = COMP.find_candidates(repo)
    A.arm("n17 a Release tree under ANY build/<name> (build/bench-rel, the VPS case) is found by "
          "sqlite_compiler.find_candidates ITSELF, the one owner of where to look (control: a tree "
          "outside build/ is not)",
          lambda: (any(os.path.normcase(c.path) == os.path.normcase(bench) for c in base_cands)
                   and not any(os.path.normcase(c.path) == os.path.normcase(outside)
                               for c in base_cands), [c.path for c in base_cands]))
    log, buf = fx.log()
    allow_pick = select_dss(repo, "", "", True, log)
    A.arm("n18 the escape hatch makes a newer Debug ELIGIBLE, never PREFERRED over a Release",
          lambda: (os.path.normcase(allow_pick.path) == os.path.normcase(bench), repr(allow_pick)))
    repo2 = fx.fresh("repo-dbg")
    dbg2 = fx.dss_tree(repo2, "dbg", ["CMAKE_BUILD_TYPE:STRING=Debug"], "bin/dss/dsscp", 1000)
    log, buf = fx.log()
    refused = _dies(lambda: select_dss(repo2, "", "", False, log))
    A.arm("n19 only a Debug dsscp: REFUSED, naming its build type and the escape hatch",
          lambda: (refused is not None and "NOT Release" in refused and "Debug" in refused
                   and "DSS_ALLOW_NONRELEASE_COMPILER=1" in refused, refused))
    log, buf = fx.log()
    hatch = select_dss(repo2, "", "", True, log)
    A.arm("n20   … and with DSS_ALLOW_NONRELEASE_COMPILER it is used AND said",
          lambda: (os.path.normcase(hatch.path) == os.path.normcase(dbg2)
                   and "NON-RELEASE compiler (Debug)" in buf.getvalue(), buf.getvalue()))
    repo3 = fx.fresh("repo-multi")
    multi = fx.dss_tree(repo3, "vs", ["CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release",
                                      "CMAKE_BUILD_TYPE:STRING=Debug",
                                      "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022"],
                        "bin/dss/Release/dsscp", 1000)
    log, buf = fx.log()
    mc = select_dss(repo3, "", "", False, log)
    A.arm("n21 a MULTI-config tree's binary is judged by its per-config directory (Release), not "
          "by the ignored CMAKE_BUILD_TYPE (Debug)",
          lambda: (os.path.normcase(mc.path) == os.path.normcase(multi) and mc.type == "Release"
                   and "IGNORES" in buf.getvalue(), "%r\n%s" % (mc, buf.getvalue())))
    log, buf = fx.log()
    ex = select_dss(repo2, rel, "--dss", False, log)
    ex_dbg = _dies(lambda: select_dss(repo, dbg2, "--dss", False, fx.log()[0]))
    A.arm("n22 an explicit --dss still REPORTS its build type, and passes the same gate (control: "
          "an explicit Debug one is refused)",
          lambda: (ex.type == "Release" and "build type: Release" in buf.getvalue()
                   and "named by --dss" in buf.getvalue() and ex_dbg is not None
                   and "NON-RELEASE" in ex_dbg, "%s\n%r" % (buf.getvalue(), ex_dbg)))
    nothing = _dies(lambda: select_dss(fx.fresh("repo-empty"), "", "", False, fx.log()[0]))
    missing = _dies(lambda: select_dss(repo, os.path.join(repo, "nope", "dsscp"), "DSS_BIN", False, fx.log()[0]))
    A.arm("n23 'nothing found' and 'the path given is not a file' are two DIFFERENT messages",
          lambda: (nothing is not None and "no dsscp binary found" in nothing and missing is not None
                   and "given by DSS_BIN is not a file" in missing, "%r / %r" % (nothing, missing)))
    if os.name == "nt":
        A.arm("n24 an explicit dsscp that is not EXECUTABLE is refused",
              lambda: (_SKIP, "Windows has no execute bit (os.access X_OK is true for every file)"))
    else:
        noexec = os.path.join(fx.fresh("noexec"), "dsscp")
        _put(noexec, "x", 0o644)
        ne = _dies(lambda: select_dss(repo, noexec, "--dss", False, fx.log()[0]))
        A.arm("n24 an explicit dsscp that is not EXECUTABLE is refused (control: the +x one above "
              "was accepted)", lambda: (ne is not None and "not an executable file" in ne, ne))


# ── new: the catalogue's leg facts ───────────────────────────────────────────────────

_NATIVE = {("linux", "x86_64"): "elf64-x86_64", ("linux", "arm64"): "elf64-arm64",
           ("darwin", "arm64"): "macho64-arm64", ("darwin", "x86_64"): "macho64-x86_64",
           ("windows", "x86_64"): "pe64-x86_64"}


def _st_legs(A, fx):
    got = {}
    for (hos, harch) in sorted(_NATIVE):
        got[(hos, harch)] = native_leg(fx.legs(hos, harch), "%s/%s" % (hos, harch)).get("label")
    with open(C.LEGS_JSON, encoding="utf-8") as fh:
        oracle = {lg["spec"]: lg for lg in json.load(fh)["legs"]}
    A.arm("n25 the native-leg table covers all 5 declared legs, one per host (the leg's runOn + "
          "target arch, never a uname table)",
          lambda: (got == _NATIVE and set(_NATIVE.values()) == {lg["label"] for lg in oracle.values()},
                   "got %r" % got))
    none = _dies(lambda: native_leg(fx.legs("windows", "arm64"), "windows/arm64"))
    A.arm("n26 a host no leg runs natively on (windows/arm64) is REFUSED, naming --target",
          lambda: (none is not None and "no leg of legs.json runs natively" in none and "--target" in none, none))
    legs = fx.legs("linux", "x86_64")
    mism = []
    for spec, lg in sorted(oracle.items()):
        f = leg_facts(legs, spec)
        b = lg["build"]
        if (f.transform, f.reserve, f.link_flags) != (b["recipeTransform"], b["stackReserveBytes"],
                                                      b["referenceLinkFlags"]):
            mism.append((spec, f))
    A.arm("n27 every leg's transform, stack reserve and reference link flags EQUAL legs.json "
          "(read directly as the oracle)", lambda: (not mism, "mismatches %r" % mism))
    macho = [leg_facts(legs, s).link_flags for s in sorted(oracle) if "macho64" in s]
    A.arm("n28 the macho legs now link -lpthread, as legs.json declares (the retired .sh's table "
          "said -lm alone) -- a deliberate, stated change",
          lambda: (len(macho) == 2 and all("-lpthread" in f and "-lm" in f for f in macho), macho))
    und = _dies(lambda: leg_facts(legs, "x86_64:coff-x86_64-windows-obj"))
    A.arm("n29 a spec no leg declares is REFUSED (no defaulting from the spec's spelling)",
          lambda: (und is not None and "declared by no leg" in und, und))
    log, buf = fx.log()
    base = leg_facts(legs, "x86_64:pe64-x86_64-windows-exec")
    same = with_overrides(base, None, None, log)
    log2, buf2 = fx.log()
    over = with_overrides(base, "none", 0, log2)
    A.arm("n30 an override is applied and REPORTED beside the value it replaced (control: no "
          "override reports none)",
          lambda: (same == base and over.transform == "none" and over.reserve == 0
                   and "OVERRIDES legs.json's 'windows-selfconfig'" in buf2.getvalue()
                   and "OVERRIDES" not in buf.getvalue(), buf.getvalue() + buf2.getvalue()))


# ── new: the Windows spelling ────────────────────────────────────────────────────────

def _st_translation(A, fx):
    calls = []

    def run(path):
        calls.append(path)
        return 0, "C:" + path[len("/mnt/c"):].replace("/", "\\") + "\n", ""
    sp = WindowsSpelling(run)
    ok = sp("/mnt/c/src/x.c")
    twice = sp("/mnt/c/src/x.c")
    dbl = _dies(lambda: sp("C:\\src\\x.c"))
    A.arm("n31 ★ a DOUBLE translation is REFUSED before the translator runs (control: the POSIX "
          "path translates, and a repeat is served from the cache)",
          lambda: (ok == "C:\\src\\x.c" and twice == ok and calls == ["/mnt/c/src/x.c"]
                   and dbl is not None and "DOUBLE translation" in dbl, "%r %r %r" % (ok, calls, dbl)))
    rel = _dies(lambda: sp("src/x.c"))
    A.arm("n32 a RELATIVE path is refused (wslpath would resolve it against its own cwd)",
          lambda: (rel is not None and "ABSOLUTE POSIX" in rel, rel))
    lazy = _dies(lambda: WindowsSpelling(lambda p: (0, p + "\n", ""))("/mnt/c/y"))
    failing = _dies(lambda: WindowsSpelling(lambda p: (1, "", "wslpath: boom"))("/mnt/c/y"))
    A.arm("n33 a translator that answers a non-Windows path, or fails, is REFUSED",
          lambda: (lazy is not None and "did not translate" in lazy and failing is not None
                   and "exited 1" in failing, "%r / %r" % (lazy, failing)))
    d = fx.fresh("xlate")
    man = os.path.join(d, "m.json")
    _write_json(man, {"sources": ["/mnt/c/s/a.c", "/mnt/c/s/b.c"], "includes": ["/mnt/c/s"],
                      "defines": ["X"]})
    sp2 = WindowsSpelling(run)
    n = translate_manifest(man, sp2)
    with open(man, encoding="utf-8") as fh:
        m = json.load(fh)
    again = _dies(lambda: translate_manifest(man, WindowsSpelling(run)))
    A.arm("n34 the manifest is translated ONCE per path, and a second pass is REFUSED as a double "
          "translation",
          lambda: (n == 3 and sp2.calls == 3 and m["sources"] == ["C:\\s\\a.c", "C:\\s\\b.c"]
                   and m["includes"] == ["C:\\s"] and m["defines"] == ["X"] and again is not None
                   and "DOUBLE translation" in again, "%r %r" % (m, again)))


# ── new: capabilities and the archive ────────────────────────────────────────────────

def _st_capabilities(A, fx):
    full = [CORE_DEFINE] + _REQUIRED[:-1] + [_REQUIRED[-1] + "=1"]
    A.arm("n35 the full declared set passes (NAME=VALUE counts as the capability)",
          lambda: (_dies(lambda: check_capabilities(full, _REQUIRED, "-DX", "r")) is None, "refused"))
    no_core = _dies(lambda: check_capabilities(_REQUIRED, _REQUIRED, "-DX", "r"))
    A.arm("n36 a define set without SQLITE_CORE is REFUSED by name",
          lambda: (no_core is not None and "has no SQLITE_CORE" in no_core, no_core))
    no_fts5 = _dies(lambda: check_capabilities([d for d in full if d != "SQLITE_ENABLE_FTS5"],
                                               _REQUIRED, "-DX", "r"))
    A.arm("n37 ★ a define set missing ONE declared requiredDefine is REFUSED naming it (the retired "
          "benchmark checked only SQLITE_CORE)",
          lambda: (no_fts5 is not None and "MISSING declared capabilities: SQLITE_ENABLE_FTS5" in no_fts5, no_fts5))
    d = fx.fresh("archive")
    amal = _dies(lambda: refuse_amalgamation(fx.archive(["sqlite3.o"])))
    full_src = _dies(lambda: refuse_amalgamation(fx.archive(["alter.o", "analyze.o"])))
    absent = _dies(lambda: refuse_amalgamation(os.path.join(d, "none.a")))
    A.arm("n38 an archive holding sqlite3.o (the AMALGAMATION) is REFUSED by member name (control: a "
          "full-source archive, and an absent one, pass here)",
          lambda: (amal is not None and "AMALGAMATION" in amal and full_src is None and absent is None, amal))
    not_archive = os.path.join(d, "libsqlite3.a")
    _put(not_archive, "not an archive\n")
    unreadable = _dies(lambda: refuse_amalgamation(not_archive))
    A.arm("n39 a file that is NOT an archive is refused, never read as an empty archive (the member "
          "names are read from the file, not asked of the host's ar)",
          lambda: (unreadable is not None and "NOT an empty archive" in unreadable, unreadable))


# ── new: the derivation end to end, every tool a fake child process ──────────────────

def _st_derive(A, fx):
    cfg, w = fx.e2e("e2e")
    d = derive(cfg)
    calls = _calls(w["make_rec"])
    bld = os.path.join(cfg.out_dir, "sqlite-build")
    with open(d.manifest, encoding="utf-8") as fh:
        man = json.load(fh)
    srcs = man["sources"]
    A.arm("n40 the derivation substitutes speedtest1.c for the generated shell.c (104 core TUs + it)",
          lambda: (d.tu_count == 105 and os.path.join(w["sqlite"], "test", MAIN_TU) in srcs
                   and not any(os.path.basename(s) == CLI_MAIN_TU for s in srcs), "%d %r" % (d.tu_count, srcs[-3:])))
    A.arm("n41 the archive is filtered to the objects the link line names (control: the tree holds "
          "zz_notlinked.c and the archive lists it)",
          lambda: (not any("zz_notlinked" in s for s in srcs)
                   and os.path.isfile(os.path.join(w["sqlite"], "src", "zz_notlinked.c")), srcs[:2]))
    A.arm("n42 the executable-suffix probe ran first, in the build dir, with the .sh's argv",
          lambda: (calls[0]["argv"] == ["-s", "-f", "Makefile", "-f",
                                        os.path.join(cfg.out_dir, "texe-probe.mk"), PROBE_TARGET]
                   and _same_dir(calls[0]["cwd"], bld), calls[0]))
    A.arm("n43 the reference build is sqlite3d + libsqlite3.a, USE_AMALGAMATION=0, the stage's make "
          "OPTIONS and -j",
          lambda: _eq(["-s", "sqlite3d", "libsqlite3.a", "USE_AMALGAMATION=0",
                       "OPTIONS=-DSQLITE_ENABLE_STAT4", "-j2"], calls[1]["argv"]))
    A.arm("n44 the recipe dry run is the .sh's (-n -B, never remaking the Makefile, the OPTIONS var)",
          lambda: _eq(["-n", "-B", "-o", "Makefile", "sqlite3d", "OPTIONS=-DSQLITE_ENABLE_STAT4"], calls[2]["argv"]))
    with open(w["conf_rec"], encoding="utf-8") as fh:
        conf = json.load(fh)
    A.arm("n45 configure runs in the build dir with the stage's configure flags",
          lambda: (conf["argv"] == ["--enable-all", "--fts3"] and _same_dir(conf["cwd"], bld), conf))
    incs = man["includes"]
    A.arm("n46 the build dir is appended LAST to the include list, once",
          lambda: (incs[-1] == bld and incs.count(bld) == 1 and incs[:-1] == ["ext", "src"], incs))
    A.arm("n47 the jimsh bootstrap's /tool/ source never reaches the TU set, and the define floor held",
          lambda: (not any("jimsh0" in s for s in srcs) and d.define_count >= MIN_DEFINES
                   and man["defines"].count(CORE_DEFINE) == 1, (d.define_count, srcs[:1])))
    with open(d.manifest, "rb") as fh:
        man_bytes = fh.read()
    A.arm("n48 the manifest names the target and the artefact",
          lambda: (man["targets"] == ["x86_64:elf64-x86_64-linux-exec"] and man["artifactName"] == ARTIFACT_NAME
                   and len(man_bytes) > 0, man.get("targets")))

    counted = []

    def spell(p):
        counted.append(p)
        return "W:" + p.replace("/", "\\")
    cfgw, ww = fx.e2e("e2e-win", style="windows", spell=spell)
    dw = derive(cfgw)
    with open(dw.manifest, encoding="utf-8") as fh:
        manw = json.load(fh)
    A.arm("n49 the windows style spells every manifest path ONCE, after generation (the defines "
          "untouched)",
          lambda: (dw.translated == len(manw["sources"]) + len(manw["includes"]) == len(counted)
                   == len(set(counted)) and all(s.startswith("W:") for s in manw["sources"] + manw["includes"])
                   and "SQLITE_CORE" in manw["defines"], (dw.translated, len(counted))))

    for label, kw, needle in (
            ("n50 a configure that FAILS is refused, naming its log", {"configure_exit": 1},
             "sqlite configure FAILED"),
            ("n51 the Makefile defining no T.exe is refused as such (the fake make)",
             {"texe": "__dss_texe=<> origin=<undefined>"}, "does not define $(T.exe) at all"),
            ("n52 a make printing no suffix answer is refused as 'NOT GNU make'", {"texe": None},
             "is NOT GNU make"),
            ("n53 a recipe without SQLITE_CORE is refused end to end", {"drop": (CORE_DEFINE,)},
             "has no SQLITE_CORE"),
            ("n54 ★ a recipe missing a declared capability is refused end to end, naming it",
             {"drop": ("SQLITE_ENABLE_RTREE",)}, "MISSING declared capabilities: SQLITE_ENABLE_RTREE"),
            ("n55 a link line without shell.c is refused (nothing to substitute)", {"no_shell": True},
             "does not carry exactly one shell.c"),
            ("n56 a reference build that left no sqlite3.h is refused", {"generate_h": False},
             "sqlite3.h is not there"),
            ("n57 the amalgamation archive is refused end to end", {"members": ["sqlite3.o"]},
             "AMALGAMATION"),
            ("n58 a linked archive member whose source is nowhere is refused (a LOST TU, never "
             "silently dropped)", {"extra_link": ("ghost.o",)}, "LOST 1 archive member"),
            ("n59 a collapsed recipe parse is refused by the TU floor (3 TUs < 100)",
             {"members": ["t000.o", "t001.o"]}, "yielded only 3 TUs")):
        c, _w = fx.e2e("e2e-neg", **kw)
        msg = _dies(lambda c=c: derive(c))
        A.arm(label, lambda msg=msg, needle=needle: (msg is not None and needle in msg, msg))
    cb, wb = fx.e2e("e2e-warn", build_exit=2)
    db = derive(cb)
    A.arm("n60 a reference build that exits non-zero only WARNS (the floors are the gate), and the "
          "derivation still completes",
          lambda: (db.tu_count == 105 and "did not fully link (exit 2)" in wb["buf"].getvalue(),
                   wb["buf"].getvalue()[-300:]))


# ── new: the plan, the hop's result, the measurement ────────────────────────────────

def _st_plan(A, fx):
    d = fx.fresh("plan")
    man = os.path.join(d, MANIFEST_FILE)
    _write_json(man, {"sources": ["/s/a.c", "/s/test/speedtest1.c"], "includes": ["/s", "/b"],
                      "defines": ["SQLITE_CORE", "X=1"]})
    refs = [Reference("gcc", "/u/gcc", "gcc", _GCC_VER), Reference("clang", "/u/clang", "clang", _CLANG_VER)]
    # A config root holding the two object formats the arms name, each declaring its extension.
    cfg_root = os.path.join(d, "cfg")
    fmts = os.path.join(cfg_root, "src", "dss-config", "object-formats")
    os.makedirs(fmts)
    _write_json(os.path.join(fmts, "elf64-x86_64-linux-exec.format.json"), {"outputExtension": ""})
    _write_json(os.path.join(fmts, "pe64-x86_64-windows-exec.format.json"),
                {"outputExtension": ".pe-from-config"})
    path = os.path.join(d, PLAN_FILE)
    plan = write_plan(path, manifest=man, sqlite_dir="/s", sqlite_head="abc1234", dss="/d/dsscp",
                      config_root=cfg_root, target="x86_64:elf64-x86_64-linux-exec", refs=refs,
                      link_flags=["-lm", "-ldl", "-lpthread"], size=25, testset="", build_repeats=3,
                      run_repeats=5, jobs_arms=[1, 4], out_dir=d)
    with open(path, "rb") as fh:
        raw = fh.read()
    back = json.loads(raw.decode("utf-8"))
    ids = [c["id"] for c in back["compilers"]]
    A.arm("n61 the plan's subject is READ BACK from the manifest, one unix-cc arm per reference, the "
          "msvc arm always, the dss arm pinned to its config root and target; LF, json round trip",
          lambda: (back == plan and back["subject"]["tus"] == ["/s/a.c", "/s/test/speedtest1.c"]
                   and back["subject"]["defines"] == ["SQLITE_CORE", "X=1"] and ids == ["dss", "gcc", "clang", "msvc"]
                   and back["compilers"][0]["configRoot"] == cfg_root and back["compilers"][0]["manifest"] == man
                   and back["compilers"][2]["version"] == "18.1.3" and back["compilers"][1]["linkFlags"]
                   == ["-lm", "-ldl", "-lpthread"] and back["workload"] == {"size": 25, "testset": None, "verify": True}
                   and b"\r" not in raw, ids))
    w1 = _dies(lambda: write_plan(os.path.join(d, "p1.json"), manifest=man, sqlite_dir="/s", sqlite_head="x",
                                  dss="/d", config_root="/r", target="t", refs=[], link_flags=[], size=1,
                                  testset="", build_repeats=1, run_repeats=1, jobs_arms=[1], out_dir=d))
    A.arm("n62 an empty reference list is refused under the WRITER's own id (W1), not R7",
          lambda: (w1 is not None and w1.startswith("W1 ") and "R7" not in w1, w1))
    bad = os.path.join(d, "bad.json")
    _write_json(bad, {"sources": ["/s/a.c"], "defines": []})
    w2 = _dies(lambda: write_plan(os.path.join(d, "p2.json"), manifest=bad, sqlite_dir="/s", sqlite_head="x",
                                  dss="/d", config_root="/r", target="t", refs=refs, link_flags=[], size=1,
                                  testset="", build_repeats=1, run_repeats=1, jobs_arms=[1], out_dir=d))
    A.arm("n63 a manifest without its includes list is refused (W2)",
          lambda: (w2 is not None and w2.startswith("W2 ") and "'includes'" in w2, w2))

    def exe_suffix_from_config():
        pe = write_plan(os.path.join(d, "p-pe.json"), manifest=man, sqlite_dir="/s", sqlite_head="x",
                        dss="/d", config_root=cfg_root, target="x86_64:pe64-x86_64-windows-exec",
                        refs=refs, link_flags=[], size=1, testset="", build_repeats=1,
                        run_repeats=1, jobs_arms=[1], out_dir=d)
        gone = _dies(lambda: write_plan(os.path.join(d, "p-w4.json"), manifest=man, sqlite_dir="/s",
                                        sqlite_head="x", dss="/d", config_root=cfg_root,
                                        target="arm64:macho64-arm64-darwin-exec", refs=refs,
                                        link_flags=[], size=1, testset="", build_repeats=1,
                                        run_repeats=1, jobs_arms=[1], out_dir=d))
        return (plan["exeSuffix"] == "" and pe["exeSuffix"] == ".pe-from-config"
                and gone is not None and gone.startswith("W4 "),
                (plan["exeSuffix"], pe["exeSuffix"], gone))
    A.arm("n63b the binaries' extension is READ from the target format's own config "
          "(`outputExtension`), per format (control: another format answers its own), and a "
          "format with no config is refused (W4)", exe_suffix_from_config)
    res = os.path.join(d, DERIVE_RESULT)
    facts = LegFacts("pe64-x86_64", "x86_64:pe64-x86_64-windows-exec", "windows-selfconfig", 8388608, [])
    derived = Derived(man, "r", "t", "d", "i", "sqlite3d", "", 2, 2, 2, "s", 5)
    write_derive_result(res, derived, facts, lambda p: p)
    ok_doc = read_derive_result(res, man, facts.spec)
    wrong_target = _dies(lambda: read_derive_result(res, man, "x86_64:elf64-x86_64-linux-exec"))
    stale = _dies(lambda: read_derive_result(res, os.path.join(d, "other.json"), facts.spec))
    _write_json(os.path.join(d, "wrong.json"), {"schema": "something-else/9"})
    schema = _dies(lambda: read_derive_result(os.path.join(d, "wrong.json"), man, facts.spec))
    missing = _dies(lambda: read_derive_result(os.path.join(d, "none.json"), man, facts.spec))
    A.arm("n64 the hop's result is trusted only for THIS schema, THIS target and THIS manifest, and "
          "a missing one is refused",
          lambda: (ok_doc["tuCount"] == 2 and all(x is not None for x in (wrong_target, stale, schema, missing))
                   and "wrote no result" in missing, "%r %r %r %r" % (wrong_target, stale, schema, missing)))
    real = os.path.join(d, "real.c")
    _put(real, "int x;\n")
    good_man = os.path.join(d, "good.json")
    _write_json(good_man, {"sources": [real], "includes": [d], "defines": []})
    bad_man = os.path.join(d, "badpaths.json")
    _write_json(bad_man, {"sources": [real, os.path.join(d, "CSourcexy.c")], "includes": [d], "defines": []})
    gone = _dies(lambda: verify_host_paths(bad_man))
    A.arm("n65 a translated path that does not exist on the measuring host is refused (control: "
          "existing ones pass)",
          lambda: (verify_host_paths(good_man) == (1, 1) and gone is not None and "CSourcexy.c" in gone, gone))

    def hop_fails(argv):
        return 1, ["boom"]

    def hop_silent(argv):
        return 0, []
    out = fx.fresh("hop2")
    f1 = _dies(lambda: wsl_derive(_FakePosix(), fx.root, out, facts, fx.log()[0], run=hop_fails))
    _write_json(os.path.join(out, DERIVE_RESULT), {"schema": DERIVE_SCHEMA, "target": facts.spec,
                                                   "manifest": os.path.join(out, MANIFEST_FILE)})
    f2 = _dies(lambda: wsl_derive(_FakePosix(), fx.root, out, facts, fx.log()[0], run=hop_silent))
    A.arm("n66 a failing hop is refused, and a hop that exits 0 but writes nothing is refused even "
          "when an OLD result lies there (it is removed first)",
          lambda: (f1 is not None and "FAILED (exit 1)" in f1 and f2 is not None
                   and "wrote no result" in f2, "%r / %r" % (f1, f2)))
    argv = hop_argv(C.PosixSide("windows"), "/mnt/c/x/benchmark_speedtest1.py", "/mnt/c/s", "/mnt/c/o", facts)
    parsed = parse_args(argv[4:])
    A.arm("n67 the hop argv is `wsl.exe -e python3 <this> ...` (no `--`, no login shell), and the "
          "derive half ACCEPTS exactly what the Windows side builds",
          lambda: (argv[:4] == ["wsl.exe", "-e", "python3", "/mnt/c/x/benchmark_speedtest1.py"]
                   and "--" not in argv and "-l" not in argv and "bash" not in argv
                   and validate_mode(parsed, "linux") == "windows"
                   and parsed.stack_reserve == 8388608, argv))
    rc3 = measure(path, d, core=fx.cores[3])
    rc0 = measure(path, d, core=fx.cores[0])
    A.arm("n68 the measurement core's exit code is passed through (3 no binary; 0 measured)",
          lambda: _eq((3, 0), (rc3, rc0)))


# ── new: the pre-flight, the config root, the UNC rule ───────────────────────────────

def _st_preflight(A, fx):
    root = fx.fresh("cfg")
    os.makedirs(os.path.join(root, "src", "dss-config"))
    comp = COMP.Compiler(os.path.join(root, "dsscp"), "Release", "x", "", root, "origin", "then", "")

    def pf(rc):
        return _dies(lambda: preflight(comp, root, "x86_64:elf64-x86_64-linux-exec", root,
                                       fx.log()[0], core=fx.cores[rc]))
    ok, refused, unrun = pf(0), pf(1), pf(3)
    A.arm("n69 ★ the pre-flight keeps 'the compiler REFUSED' (exit 1) and 'the check COULD NOT RUN' "
          "(exit 3) apart (control: exit 0 passes)",
          lambda: (ok is None and refused is not None and "CANNOT COMPILE THREE LINES" in refused
                   and unrun is not None and "COULD NOT RUN" in unrun and "CANNOT COMPILE" not in unrun,
                   "%r\n---\n%r" % (refused, unrun)))
    other = fx.fresh("cfg-other")
    os.makedirs(os.path.join(other, "src", "dss-config"))
    bare = fx.fresh("cfg-bare")
    with _env(DSS_CONFIG_ROOT=other):
        pinned = COMP.pin_config_root(root, fx.log()[0])
    with _env(DSS_CONFIG_ROOT=None):
        dflt = COMP.pin_config_root(root, fx.log()[0])
    with _env(DSS_CONFIG_ROOT=bare):
        bad = _dies(lambda: COMP.pin_config_root(root, fx.log()[0]))
    A.arm("n70 DSS_CONFIG_ROOT is HONOURED (the .ps1 ignored it); unset, the checkout is the pin; a "
          "pin without src/dss-config is refused",
          lambda: (pinned == other and dflt == root and bad is not None and "no dss config tree" in bad,
                   "%r %r %r" % (pinned, dflt, bad)))
    unc1 = _dies(lambda: refuse_unc("\\\\wsl$\\Ubuntu\\src\\sqlite", "windows", "the SQLite checkout"))
    unc2 = _dies(lambda: refuse_unc("//wsl.localhost/Ubuntu/x", "windows", "the output directory"))
    A.arm("n71 a UNC sqlite dir (either spelling) is REFUSED by name on a Windows host (control: a "
          "drive path passes; a POSIX host is never handed one)",
          lambda: (unc1 is not None and "UNC share" in unc1 and unc2 is not None
                   and _dies(lambda: refuse_unc("C:\\Source\\sqlite", "windows", "x")) is None
                   and _dies(lambda: refuse_unc("\\\\x\\y", "linux", "x")) is None, "%r %r" % (unc1, unc2)))


# ── new: the CLI ─────────────────────────────────────────────────────────────────────

def _st_cli(A, fx):
    cases = (["--nope"], ["--path-style", "sideways"], ["--size", "x"], ["--jobs-arms", "1 0"],
             ["--stack-reserve", "-1"], ["--recipe-transform", "bogus"], ["--self-test", "--size", "3"],
             ["--sqlite-dir"], ["--sqlite"])
    got = [_run_main(c) for c in cases]
    A.arm("n72 every malformed invocation exits 2 with a USAGE line (the .ps1 exited 1): unknown flag, "
          "bad style, non-integers, a zero worker count, a negative reserve, an unknown transform, "
          "--self-test with company, a missing value, an abbreviation",
          lambda: (all(rc == 2 and "USAGE" in err for rc, _o, err in got),
                   [(c, rc, err.strip()[:90]) for c, (rc, _o, err) in zip(cases, got) if rc != 2]))
    rc, out, _e = _run_main(["-h"])
    A.arm("n73 -h exits 0 and prints the usage (the flags and the exit codes)",
          lambda: (rc == 0 and "--path-style" in out and "exit:" in out, out[:120]))

    def mode(argv, host):
        try:
            return validate_mode(parse_args(argv), host)
        except UsageError as exc:
            return "USAGE: %s" % exc
    half = ["--derive-only", "--path-style", "windows", "--sqlite-dir", "/s", "--out", "/o", "--target", "t"]
    res = {"win-posix": mode(["--path-style", "posix"], "windows"),
           "win-default": mode([], "windows"),
           "half-no-derive": mode(["--path-style", "windows"], "linux"),
           "half-with-cc": mode(half + ["--cc", "gcc"], "linux"),
           "half-no-target": mode(half[:-2], "linux"),
           "half-ok": mode(half, "linux"),
           "posix-default": mode([], "linux")}
    A.arm("n74 the path style is decided once per host: posix on Windows is refused; the derive half "
          "needs --derive-only and its three inputs, and refuses the measuring host's flags",
          lambda: (res["win-posix"].startswith("USAGE") and res["win-default"] == "windows"
                   and "needs --derive-only" in res["half-no-derive"] and "--cc belongs to the MEASURING host"
                   in res["half-with-cc"] and "needs --target" in res["half-no-target"]
                   and res["half-ok"] == "windows" and res["posix-default"] == "posix", res))
    if fx.host == "windows" and fx.wsl:
        side = C.PosixSide("windows")
        r = C.capture(side.argv(["python3", side.to_posix(THIS), "--help"]), timeout=300)
        A.arm("n75 the derive half LOADS inside WSL: THIS file and its siblings import under WSL's "
              "python3 (`--help` through the carriage exits 0)",
              lambda: (r.rc == 0 and "--path-style" in r.out, "rc=%d %s" % (r.rc, C.first_lines(r.err or r.out, 4))))
    else:
        A.arm("n75 the derive half LOADS inside WSL (`--help` through the carriage)",
              lambda: (_SKIP, "not a Windows host with wsl.exe: this process IS the POSIX side"))


_SECTIONS = (("substitution", _st_substitution), ("presence", _st_presence), ("label", _st_label),
             ("texe-real", _st_texe_real), ("carriage", _st_carriage), ("core", _st_core),
             ("post-conditions", _st_post_conditions), ("references", _st_references),
             ("dsscp", _st_dsscp), ("legs", _st_legs), ("translation", _st_translation),
             ("capabilities", _st_capabilities), ("derive", _st_derive), ("plan", _st_plan),
             ("preflight", _st_preflight), ("cli", _st_cli))


def self_test():
    print("== benchmark_speedtest1.py --self-test ==")
    A = _Arms()
    root = tempfile.mkdtemp(prefix="dss-speedtest1-st-")
    try:
        fx = _Fx(root)
        print("   (host %s; make: %s; wsl.exe: %s)" % (fx.host, fx.make or "none", fx.wsl or "none"))
        for name, fn in _SECTIONS:
            A.section(name, fn, fx)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return A.finish(EXPECTED_ARMS)


if __name__ == "__main__":
    sys.exit(main())
