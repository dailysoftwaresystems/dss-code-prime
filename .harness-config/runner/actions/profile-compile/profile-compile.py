#!/usr/bin/env python3
# PURPOSE: compile one fixed subject with a RELEASE dsscp on this host and report where the time went, so the HOST is the only variable across legs.
"""profile-compile.py -- compile ONE fixed subject on THIS host with a RELEASE dsscp
and report where the time went. Run it on every leg with the same kit and the same
target, and the HOST is the only thing that moves.

★★★ ONE PROGRAM, NO TWIN, AND THAT IS A DECISION RATHER THAN AN OMISSION.
This tool exists because a measurement was taken with an uncontrolled variable; a
second implementation of it would be a second contract, and the defect that
produced this tool's own reason for existing was exactly that: the sqlite
harness's shell and PowerShell drivers implemented "which compiler do we time"
differently, one always Release and one always newest-wins, and the difference
between -O0 and -O3 was published as a property of the Windows HOST (~8x; ~2.1x
once controlled). A profiler whose whole value is "the only variable is the host"
must not have a per-host implementation. It is one Python file that every leg runs
natively -- the Windows leg included, which until 2026-09-21 ran a bash script of
this program under Git Bash -- and every path it hands a native tool is an
absolute path in the host's own form.

── HOW TO RUN THE FULL FOUR-LEG PROFILE FROM A COLD START ───────────────────
 0. Once, on a host that has a STAGED subject (the Windows box, after a sqlite run
    has staged sqlite and emitted a manifest), the action's `kit` step -- its kit is
    the step's kept output, `<the step's build directory>/kit`:
      dssharness run profile-compile --manual-step kit \
          --input manifest=build/real-examples/c/sqlite/windows/sqlite3.elf64-x86_64.dss-project.json \
          --input "roots=stage=build/real-examples/c/sqlite/windows/stage;libs=<the harness libraries>"
    The kit is COPIED, never re-staged: the sqlite harness pulls upstream on
    every run, so a host that stages for itself is not compiling the same
    program as its peers.
 1. Every leg, through the action runner (the kit path is the action's `kit` input,
    read from the runner value directory `.harness-config/runner/.env/`, or given as
    `dssharness run profile-compile --input kit=<dir>`; see `dssharness help runners`):
      dssharness run profile-compile --legs linux-x86_64-release
    Only a RELEASE leg that the `profile-compile` runner in `.harness-config/config.json`
    declares: the program refuses any dsscp that is not a Release build (below).
 2. The yardstick, on any leg with gcc (the same TUs, the same manifest): the same step with
    `--input gccReference=true`, the reference compiler, its optimization and its link
    libraries being the step's `referenceCc`, `referenceOpt` and `referenceLinkLibs` inputs
    (gcc, -O2, -lz -lm -lpthread -ldl unless a run says otherwise).
    ✔MEASURED 2026-08-18 on WSL, 103 TUs, gcc 13 -O2: -j1 21.5 s, -j32 4.8 s,
    against DSS's 1m40.8 s on that same host.

Usage:
  profile-compile.py --kit <dir> --target <spec> --label <name>
                     [--build-dir <dir>] [--no-build]
                     [--out <dir>] [--jobs N] [--gcc-reference true|false]
                     [--reference-cc <cc> --reference-opt <flag> --reference-link-libs <libs>]

  --kit <dir>        a materialized kit (`profile-compile-support.py kit`). REQUIRED.
  --target <spec>    the ONE <target>:<format> every leg compiles. REQUIRED.
  --label <name>     names this leg in the report and in the witness line. REQUIRED.
  --build-dir <dir>  the compiler's build tree (default: <repo>/build/rel).
  --no-build         time the dsscp already in the build tree; configure nothing.
  --out <dir>        logs, the subject manifest and the image (default:
                     <repo>/build/perf/<label>).
  --jobs N           build and gcc-reference parallelism (default: DSS_JOBS, then 6).
  --gcc-reference B  true also times gcc over the same TUs from the same manifest.
  --reference-cc C, --reference-opt F, --reference-link-libs L
                     the yardstick's compiler, optimization flag and link libraries --
                     the step's inputs, REQUIRED with --gcc-reference true (a value that
                     begins with `-` is still the option's value).
  -h, --help         show this help and exit.
  --self-test        prove the command line's refusals (no kit, no build), one verdict line per arm; the
                     action's `self-test` step and the ctest entry `harness/profile_compile_selftest` run it.
  A relative --kit, --out or --build-dir is taken from the repository root -- the
  tree this file lives in. Every value flag needs a non-empty value; anything else
  on the command line is refused.

THE HOST-EQUALITY GATE (P69, 2026-11-16):
  profile-compile.py --host-equality <label>,<label>[,...]
  Every leg profiles the SAME kit for the SAME target, so the image must be the same bytes on every leg: an image
  is a function of its inputs and its target, never of the host whose toolchain built the compiler. (MEASURED
  2026-11-16: the kit image differed in 4,879,353 of 6,019,864 bytes between the MSVC-built and the GCC-built
  compiler. An optimizer pass walked an unordered container while minting phis.)
  This mode reads `build/perf/<label>/image/` for each label named: this leg's own, and every other leg's, each
  brought back first with `dssharness sync --legs <leg> --pull build/perf/<leg>/image/`. It compares every
  file's name, size and md5, writes what it read to `build/perf/host-equality.txt`, and prints `HOST-EQUALITY-OK
  <md5 of each file>` when every leg holds the same files with the same bytes. Otherwise it prints
  `HOST-EQUALITY-FAILED` with each leg's files and exits 1. It REFUSES (exit 1) before reading any image: fewer
  than two labels, an empty or repeated label, and a label whose image directory holds no file.

Output: the last line is `PROFILE-LEG-OK <label>   (out: <dir>)` on success, and
`PROFILE-LEG-FAILED <label> rc=<rc>  (log: <file>)` when the compile failed -- the
failure token never contains the success token.
Exit codes: 0 profiled · 1 refused (a bad argument, a missing input, a compiler
that is not a Release build, a failed build, kit or gcc reference) · otherwise the
compile's own exit code, or 3 when it exited 0 without its `compile time` witness.
"""
import importlib.util
import os
import platform
import re
import shutil
import subprocess
import sys
sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

# ── OUTPUT ENCODING, AT IMPORT ───────────────────────────────────────────────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, both streams pipes -- how the
# action runner starts this file): stdout comes up cp1252, so a report line holding
# a character outside it raises `UnicodeEncodeError` inside the report, and stderr
# mangles it into an escape. Reconfigured before anything can print.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - an odd stream
        pass

TOOL = "profile-compile"

# The two witness tokens. The runner's success pattern is `PROFILE-LEG-OK ` (the
# space included), and the failure token must never contain it as a substring.
OK_TOKEN = "PROFILE-LEG-OK"
FAILED_TOKEN = "PROFILE-LEG-FAILED"

# ★ `--gcc-reference` TAKES `true` OR `false` (2026-09-30): the `profile` step hands every input over, its
# default included, and a switch cannot be passed conditionally. `--repo` is RETIRED the same day: the tree
# profiled is the one this file lives in, which is the tree the harness step runs in.
# ★ THE YARDSTICK'S KNOBS ARE THE STEP'S INPUTS (2026-10-01, the P69 review's NIT 7): `--reference-cc`,
# `--reference-opt` and `--reference-link-libs`, handed to the support's `gcc-reference` -- as compile-bench made
# its own knobs inputs, so no value a measurement depends on is fixed inside a program.
VALUE_FLAGS = ("--kit", "--target", "--label", "--build-dir", "--out", "--jobs", "--gcc-reference",
               "--reference-cc", "--reference-opt", "--reference-link-libs")
SWITCHES = ("--no-build",)
DEFAULT_JOBS = 6

# The support program, taken from the REPOSITORY being profiled -- the tree this file lives in.
SUPPORT_REL = (".harness-config", "runner", "actions", "profile-compile",
               "profile-compile-support.py")

# ★ PROBE THE FILESYSTEM, NEVER A PATH LOOKUP, FOR THE TOOLCHAIN DIRECTORIES.
# ✔MEASURED on the Mac, twice: its login profile REPLACES $PATH outright (an emsdk
# block sets PATH=<emsdk dirs>:/usr/bin:/bin:/usr/sbin:/sbin), so a
# non-interactive ssh loses /opt/homebrew/bin entirely and a PATH lookup of cmake
# answers "not installed" about a machine that has it. A directory either exists
# or it does not, and $PATH cannot corrupt that question. Prepended in this order,
# so /usr/local/bin ends up first -- what the retired shell program did.
PATH_PROBES = ("/opt/homebrew/bin", "/usr/local/bin")

# The `--time` report dsscp writes (program.cpp `emitPhaseTimeReport`): its first line `dsscp: compile time ...`, then
# a table whose header and every row -- each phase, `[other]`, a violation's `!` line, the pre-scan memo, a batch --
# begin `dsscp:   `, and, on a run whose accounting is broken, the violation BANNER `dsscp: *** ...` before the `!`
# lines. The report is that BLOCK: the first line and every line after it that begins with either prefix. ⚠ The pattern this replaced
# matched `^dsscp:   phase`, the HEADER alone, so the report printed a table's head with no row under it (P69 H6).
_REPORT_FIRST = re.compile(r"^dsscp: compile time ")
_REPORT_ROW   = "dsscp:   "
# The writer's OTHER prefix inside the block (program.cpp `emitPhaseTimeReport`): the invariant-violation banner,
# `dsscp: *** --time INVARIANT VIOLATION ... ***`, one space after the colon -- its `!` lines, the pre-scan memo row
# and the per-batch rows follow it under `_REPORT_ROW` (P69 H6b, the review of H6's F6: a block ended at the banner
# showed a broken run's report as a clean table missing its `[other]` row).
_REPORT_BANNER = "dsscp: *** "


def _flush():
    """Flush both streams, so a child writing to the same pipe cannot overtake them."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.flush()
        except (AttributeError, ValueError, OSError):
            pass


def die(msg):
    """Refuse: `\\n[X] profile-compile: <msg>` on stderr, exit 1."""
    _flush()
    sys.stderr.write("\n[X] %s: %s\n" % (TOOL, msg))
    _flush()
    raise SystemExit(1)


def say(label, msg):
    sys.stdout.write("\n=== [%s] %s ===\n" % (label or "?", msg))


def info(msg):
    sys.stdout.write("   %s\n" % msg)


def _status(rc):
    """A child's exit status as a shell reports it: death by signal N is 128+N."""
    return 128 - rc if rc < 0 else rc


# ── the command line ────────────────────────────────────────────────────────────
def parse_cli(argv):
    """argv -> a dict of options. An unknown argument (`--`, a `--flag=value` form, a
    misspelling) is a REFUSAL, never a shrug: silently ignoring one is how a run ends
    up not measuring what its command line said it measured. A value flag with a
    missing or EMPTY value is refused by name."""
    o = {"kit": "", "target": "", "label": "", "build_dir": "", "out": "",
         "jobs": "", "no_build": False, "gcc_reference": "false", "reference_cc": "", "reference_opt": "",
         "reference_link_libs": "", "help": False}
    i, n = 0, len(argv)
    while i < n:
        a = argv[i]
        if a in ("-h", "--help"):
            o["help"] = True
            return o
        if a in VALUE_FLAGS:
            if i + 1 >= n or argv[i + 1] == "":
                die("%s needs a non-empty value. See --help." % a)
            o[a[2:].replace("-", "_")] = argv[i + 1]
            i += 2
            continue
        if a in SWITCHES:
            o[a[2:].replace("-", "_")] = True
            i += 1
            continue
        die("unknown argument '%s'. See --help." % a)
    # ★ A PATH TO WRITE NEVER BEGINS WITH `-` -- owning-tree's `written_path` says so for every program: `--out --help`
    # took `--help` for the report directory, an option read as a value. A value that is no path may begin with one
    # (`--reference-opt -O2`), so only these two are asked.
    for flag in ("--out", "--build-dir"):
        key = flag[2:].replace("-", "_")
        if o[key]:
            owner = _owning_tree_module()
            try:
                o[key] = owner.written_path(o[key], flag)
            except owner.Refusal as exc:
                die("%s. See --help." % exc)
    if not o["kit"]:
        die("--kit is required")
    if not o["target"]:
        die("--target is required (e.g. x86_64:elf64-x86_64-linux-exec)")
    if not o["label"]:
        die("--label is required (it names this leg in the report)")
    if o["jobs"]:
        _positive_jobs(o["jobs"], "--jobs")
    if o["gcc_reference"] not in ("true", "false"):
        die("--gcc-reference is true or false, got '%s'" % o["gcc_reference"])
    o["gcc_reference"] = o["gcc_reference"] == "true"
    if o["gcc_reference"]:
        for flag in ("--reference-cc", "--reference-opt", "--reference-link-libs"):
            if not o[flag[2:].replace("-", "_")]:
                die("%s is required with --gcc-reference true (the yardstick's knobs are the step's inputs)" % flag)
    return o


def _positive_jobs(value, source):
    """★NEW: the shell program handed any text on to `cmake -j` and `gcc-reference`,
    which failed on it later and less clearly; a parallelism that is not a whole number
    of at least 1 is now refused by name before anything is built with it."""
    try:
        jobs = int(value)
    except ValueError:
        jobs = 0
    if jobs < 1:
        die("%s must be a whole number of at least 1, got '%s'" % (source, value))
    return jobs


def effective_jobs(opt_jobs):
    """--jobs, else DSS_JOBS, else 6. ★★★ OPERATOR RULING 2026-08-25: "never use all
    CPUS, the idea is to keep build + tests + run always at 4 cpus", AMENDED the same
    day to "make it 6 cores, not 4, everywhere". Checked where it is USED, so a stray
    DSS_JOBS never refuses a run that builds nothing."""
    if opt_jobs:
        return _positive_jobs(opt_jobs, "--jobs")
    env = os.environ.get("DSS_JOBS", "")
    if env:
        return _positive_jobs(env, "DSS_JOBS")
    return DEFAULT_JOBS


# ── the host ────────────────────────────────────────────────────────────────────
def augment_path(environ=None, isdir=os.path.isdir):
    """Prepend each of PATH_PROBES that EXISTS as a directory and is not already on
    PATH (a POSIX host's question -- those directories mean nothing on Windows). The
    change is made to this process's environment, so every child inherits it."""
    environ = os.environ if environ is None else environ
    if os.name != "posix":
        return
    raw = environ.get("PATH", "")
    entries = raw.split(os.pathsep) if raw else []
    for d in PATH_PROBES:
        if isdir(d) and d not in entries:
            entries.insert(0, d)
    environ["PATH"] = os.pathsep.join(entries)


_OWNER = []


def _owning_tree_module():
    """`owning-tree.py`, loaded once as a SIBLING: it names the tree this file lives in, and says what a path a
    command line names for writing may be. A missing owner is a refusal by name."""
    if not _OWNER:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                            "owning-tree", "owning-tree.py")
        if not os.path.isfile(path):
            die("cannot find %s -- the tree this program lives in is named there and "
                "nowhere else" % path)
        saved = sys.dont_write_bytecode
        sys.dont_write_bytecode = True          # no __pycache__ in a directory not ours
        try:
            spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
        finally:
            sys.dont_write_bytecode = saved
        _OWNER.append(mod)
    return _OWNER[0]


def _owning_tree_root():
    """The tree this file lives in, by `owning-tree.py`'s walk.

    ⓘ Never by counting `..` from this file: that count named `.harness-config/runner`
    the day this program moved out of `scripts/` (2026-09-18). A missing owner or a
    walk that finds nothing is a refusal by name, never a fallback to the caller's cwd.
    """
    mod = _owning_tree_module()
    try:
        return mod.owning_tree(__file__)
    except mod.Refusal as exc:
        die("cannot name the tree this program lives in: %s" % exc)


def resolve_repo():
    """The dsscp checkout to profile: the tree this file lives in -- a profiler that has to be told
    where it is can be pointed at a different checkout than the compiler it just built (`--repo` did
    exactly that until its retirement on 2026-09-30)."""
    repo = _owning_tree_root()
    if not os.path.isdir(os.path.join(repo, "src", "dss-config")):
        die("%s is missing — that is not a dsscp checkout"
            % os.path.join(repo, "src", "dss-config"))
    return repo


def _under(repo, path):
    """`path` taken from the repository root when relative -- what the retired shell
    program's `cd "$REPO"` made of it -- as an absolute native path."""
    return os.path.normpath(path if os.path.isabs(path) else os.path.join(repo, path))


def _cores():
    try:
        return str(len(os.sched_getaffinity(0)))     # what `nproc` reports on Linux
    except (AttributeError, OSError):
        return str(os.cpu_count() or "?")


def _memory():
    try:
        with open("/proc/meminfo", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith("MemTotal:"):
                    return "%.1f GiB" % (int(line.split()[1]) / 1048576.0)
    except (OSError, ValueError, IndexError):
        pass
    if os.path.isfile("/usr/sbin/sysctl"):
        try:
            p = subprocess.run(["/usr/sbin/sysctl", "-n", "hw.memsize"], capture_output=True,
                               text=True, stdin=subprocess.DEVNULL)
            if p.returncode == 0 and p.stdout.strip().isdigit():
                return "%.1f GiB" % (int(p.stdout.strip()) / 1073741824.0)
        except OSError:
            pass
    return "?"


def _cxx_version():
    for name in ("c++", "g++"):
        exe = shutil.which(name)
        if not exe:
            continue
        try:
            p = subprocess.run([exe, "--version"], capture_output=True, text=True,
                               encoding="utf-8", errors="replace", stdin=subprocess.DEVNULL)
        except OSError:
            continue
        lines = (p.stdout or "").splitlines()
        if p.returncode == 0 and lines:
            return lines[0]
    return "?"


def host_report(label, config_root):
    """Informational only: what this leg ran on."""
    say(label, "host")
    u = platform.uname()
    sys.stdout.write("%s %s %s %s %s\n" % (u.system, u.node, u.release, u.version, u.machine))
    sys.stdout.write("cores : %s\n" % _cores())
    sys.stdout.write("mem   : %s\n" % _memory())
    sys.stdout.write("cxx   : %s\n" % _cxx_version())
    sys.stdout.write("config: %s\n" % config_root)


# ── children ────────────────────────────────────────────────────────────────────
def tail(path, n):
    """The last `n` lines of a log, on stdout. A log that is not there says so."""
    try:
        with open(path, "rb") as fh:
            lines = fh.read().decode("utf-8", "replace").splitlines()
    except OSError:
        info("(no log at %s)" % path)
        return
    for line in lines[-n:]:
        sys.stdout.write(line + "\n")


def _run_logged(argv, log, cwd):
    """`argv` with stdout and stderr into `log`, from `cwd` -> its exit status."""
    _flush()
    try:
        with open(log, "wb") as fh:
            p = subprocess.run(argv, cwd=cwd, stdin=subprocess.DEVNULL, stdout=fh,
                               stderr=subprocess.STDOUT)
    except OSError as exc:
        die("could not run %s (%s)" % (argv[0], exc))
    return _status(p.returncode)


def build_compiler(repo, build_dir, out, jobs):
    """Configure and build a RELEASE dsscp into `build_dir`; the logs land in `out`."""
    cmake = shutil.which("cmake")
    if not cmake:
        die("cmake not found on PATH -- it builds the dsscp this leg times (or pass "
            "--no-build to time the one already in %s)" % build_dir)
    cmake = os.path.abspath(cmake)
    log = os.path.join(out, "cmake-configure.log")
    rc = _run_logged([cmake, "-S", repo, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release"],
                     log, repo)
    if rc != 0:
        tail(log, 20)
        die("cmake configure failed (rc=%d)" % rc)
    log = os.path.join(out, "cmake-build.log")
    rc = _run_logged([cmake, "--build", build_dir, "--config", "Release", "--target",
                      "dsscp", "-j", str(jobs)], log, repo)
    if rc != 0:
        tail(log, 30)
        die("dsscp build failed (rc=%d)" % rc)


def find_dsscp(build_dir):
    """The first regular file named `dsscp` or `dsscp.exe` in a SORTED walk of
    `build_dir`, or None.

    BOTH spellings on EVERY host: the executable suffix is a fact about the machine
    the compiler RUNS on, and probing for a name that cannot exist here costs
    nothing -- whereas assuming one name is how the predecessor of this program once
    reported "no dsscp" over a build that had just succeeded. The walk is sorted so
    two runs over one tree pick the same file (the shell program took whichever
    `find` met first); the build type is still ASSERTED below, whichever it is.
    """
    names = ("dsscp", "dsscp.exe")
    for dirpath, dirnames, filenames in os.walk(build_dir):
        dirnames.sort()
        for f in sorted(filenames):
            p = os.path.join(dirpath, f)
            if f in names and os.path.isfile(p) and not os.path.islink(p):
                return p
    return None


def support(repo, support_path, argv, extra_env=None, capture=False):
    """`profile-compile-support.py <argv>` with THIS interpreter, from the repository
    root -> (exit status, captured stdout or None).

    Its environment is this process's (DSS_CONFIG_ROOT and the augmented PATH
    included) plus `extra_env`, PYTHONIOENCODING=utf-8 and PYTHONUNBUFFERED=1. Its
    output shares this program's streams, which carry UTF-8, and `agg-trace`'s is
    captured and decoded as UTF-8: ✔MEASURED 2026-09-21 (Windows, locale cp1252),
    without the variable that capture lost both of its em dashes to U+FFFD. And a
    child whose stdout is block-buffered on a pipe prints its refusal on stderr BEFORE
    the lines that explain it (✔MEASURED the same day: `build-type`'s FATAL came out
    above the build type it had read).
    """
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    env["PYTHONUNBUFFERED"] = "1"
    env.update(extra_env or {})
    _flush()
    try:
        p = subprocess.run([sys.executable, support_path] + list(argv), cwd=repo, env=env,
                           stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE if capture else None)
    except OSError as exc:
        die("could not start %s (%s)" % (support_path, exc))
    text = p.stdout.decode("utf-8", "replace") if capture else None
    return _status(p.returncode), text


def report_lines(text):
    """The `--time` report's lines in `text`: its `compile time` line and the table under it, header and rows."""
    out, inside = [], False
    for line in text.splitlines():
        if _REPORT_FIRST.match(line):
            out.append(line)
            inside = True
        elif inside and (line.startswith(_REPORT_ROW) or line.startswith(_REPORT_BANNER)):
            out.append(line)
        else:
            inside = False
    return out


def phase_report(log):
    """The compile log's `--time` report, whole: its `compile time` line and every row of the table under it."""
    try:
        with open(log, "rb") as fh:
            text = fh.read().decode("utf-8", "replace")
    except OSError:
        return
    for line in report_lines(text):
        sys.stdout.write(line + "\n")


def list_artifacts(image_dir, limit):
    """The first `limit` regular files under `image_dir`, sorted, with their sizes."""
    shown = 0
    for dirpath, dirnames, filenames in os.walk(image_dir):
        dirnames.sort()
        for f in sorted(filenames):
            p = os.path.join(dirpath, f)
            if not os.path.isfile(p):
                continue
            sys.stdout.write("%12d  %s\n" % (os.path.getsize(p), p))
            shown += 1
            if shown >= limit:
                return


# ── THE HOST-EQUALITY GATE (see the header) ─────────────────────────────────────────
HOST_OK_TOKEN = "HOST-EQUALITY-OK"
HOST_FAILED_TOKEN = "HOST-EQUALITY-FAILED"
HOST_RECORD = "host-equality.txt"


class HostEqualityRefusal(Exception):
    """A request the gate refuses before it reads any image."""


def image_files(image_dir):
    """[(name relative to `image_dir`, size, md5)] for every regular file under it, sorted by name."""
    import hashlib
    found = []
    for dirpath, dirnames, filenames in os.walk(image_dir):
        dirnames.sort()
        for f in sorted(filenames):
            p = os.path.join(dirpath, f)
            if not os.path.isfile(p):
                continue
            h = hashlib.md5()
            with open(p, "rb") as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
            found.append((os.path.relpath(p, image_dir).replace(os.sep, "/"), os.path.getsize(p), h.hexdigest()))
    return found


def host_equality(perf_dir, labels_value):
    """(equal, report lines) for the images `build/perf/<label>/image/` of every label in `labels_value`; raises
    HostEqualityRefusal before reading any image when the request cannot be judged."""
    labels = labels_value.split(",") if labels_value else []
    if len(labels) < 2:
        raise HostEqualityRefusal("--host-equality compares two legs or more: name at least two labels, "
                                  "comma-separated (got %r)" % labels_value)
    if "" in labels:
        raise HostEqualityRefusal("--host-equality names an empty label in %r" % labels_value)
    if len(set(labels)) != len(labels):
        raise HostEqualityRefusal("--host-equality names a label twice: %r" % labels_value)
    dirs = [(label, os.path.join(perf_dir, label, "image")) for label in labels]
    for label, d in dirs:
        if not os.path.isdir(d) or not any(files for _, _, files in os.walk(d)):
            raise HostEqualityRefusal(
                "leg %s has no image to compare: %s holds no file. Profile it, and bring a remote leg's image back "
                "with `dssharness sync --legs %s --pull build/perf/%s/image/`" % (label, d, label, label))
    listings = [(label, image_files(d)) for label, d in dirs]
    lines = []
    for label, files in listings:
        lines.append("host-equality: %s: %d file(s): %s" % (
            label, len(files), ", ".join("%s %d byte(s) md5 %s" % f for f in files)))
    equal = all(files == listings[0][1] for _, files in listings[1:])
    if equal:
        lines.append("%s %s  (%s)" % (HOST_OK_TOKEN, " ".join("%s=%s" % (n, d) for n, _, d in listings[0][1]),
                                      ", ".join(labels)))
    else:
        lines.append("%s the legs' images differ (%s): an image must be a function of its inputs and its target, "
                     "never of the host that built the compiler" % (HOST_FAILED_TOKEN, ", ".join(labels)))
    return equal, lines


def run_host_equality(labels_value):
    """The gate's run -> its exit code: the lines printed and written to build/perf/host-equality.txt."""
    perf_dir = os.path.join(resolve_repo(), "build", "perf")
    try:
        equal, lines = host_equality(perf_dir, labels_value)
    except HostEqualityRefusal as exc:
        die(str(exc))
    with open(os.path.join(perf_dir, HOST_RECORD), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    for line in lines:
        sys.stdout.write(line + "\n")
    return 0 if equal else 1


# ── the run ─────────────────────────────────────────────────────────────────────
# ── THE COMMAND LINE'S OWN SELF-TEST: no kit, no build ───────────────────────────
SELFTEST_MARKER = "profile-compile self-test: "
# ⚠ Raising this is the claim that the arms you added actually RUN.
EXPECTED_ARMS = 15
# A compile log's `--time` report as dsscp writes it (program.cpp `emitPhaseTimeReport`'s own lines), with a trace line
# before it and an optimizer trace line after it, which are no part of it.
_REPORT_SAMPLE = (
    "opt: pass inline 3 changed\n"
    "dsscp: compile time 1.25s  (process wall)\n"
    "dsscp:   phase                         cpu        wall   peak    runs\n"
    "dsscp:   parse                       0.10s       0.10s      1       1\n"
    "dsscp:   optimize                    0.80s       0.81s      1       1\n"
    "dsscp:   [other]                                 0.34s\n"
    "opt: fixpoint release converged after 2 iteration(s)\n")
# The same report from a run whose accounting is broken (program.cpp's own lines): the banner, its `!` lines, then the
# pre-scan memo and a batch row -- the rows a block ended at the banner dropped.
_REPORT_VIOLATION_SAMPLE = (
    "dsscp: compile time 1.25s  (process wall)\n"
    "dsscp:   phase                         cpu        wall   peak    runs\n"
    "dsscp:   parse                       0.10s       0.10s      1       1\n"
    "dsscp: *** --time INVARIANT VIOLATION — the phase accounting is inconsistent with itself, so the unattributed "
    "remainder is NOT reported ***\n"
    "dsscp:   ! 1 phase scope(s) are still LIVE at the report\n"
    "dsscp:   pre-scan memo             3 built      9 hit   (12 include request(s), 75.0% served)\n"
    "dsscp:   compile-units        width 4    jobs 2     batch      0.50s  Σjobs      0.90s\n"
    "opt: fixpoint release converged after 2 iteration(s)\n")
_REQUIRED =["--kit", "kit", "--target", "x86_64:elf64-x86_64-linux-exec", "--label", "leg"]


def self_test():
    """Each arm parses one command line and reads what it did; a refusal is read from the program's own words."""
    import contextlib
    import io
    ran = []

    def arm(label, ok, detail=""):
        ran.append(bool(ok))
        if ok:
            print("%sok      %s" % (SELFTEST_MARKER, label))
        else:
            print("%sFAILED  %s -- %s" % (SELFTEST_MARKER, label, (detail or "no detail").replace("\n", " ")[:600]))

    def parsed(argv):
        said = io.StringIO()
        try:
            with contextlib.redirect_stderr(said):
                return parse_cli(_REQUIRED + argv), None, ""
        except SystemExit as exc:
            return None, exc.code, said.getvalue().strip()

    def refused(argv, flag):
        got, code, text = parsed(argv)
        if got is not None:
            return False, "parsed, no refusal: %s=%r" % (flag, got[flag[2:].replace("-", "_")])
        return (code == 1 and ("%s takes '" % flag) in text and "begins with `-`" in text,
                "exit %r: %s" % (code, text[:300]))

    arm("--out --help is an option taken for the directory to write: refused, naming --out",
        *refused(["--out", "--help"], "--out"))
    arm("--build-dir -x is an option taken for the build tree to write: refused, naming --build-dir",
        *refused(["--build-dir", "-x"], "--build-dir"))
    got, code, text = parsed(["--out=--help"])
    arm("--out=--help is no spelling of a value here: refused as an unknown argument",
        got is None and code == 1 and "unknown argument '--out=--help'" in text, "exit %r: %s" % (code, text[:300]))
    got, code, text = parsed(["--out", "build/perf/x-y", "--build-dir", "build/rel-2"])
    arm("a path holding a dash is kept (the control)",
        got is not None and (got["out"], got["build_dir"]) == ("build/perf/x-y", "build/rel-2"),
        "exit %r: %s" % (code, text[:300]) if got is None else "got %r" % ((got["out"], got["build_dir"]),))
    got, code, text = parsed(["--gcc-reference", "true", "--reference-cc", "gcc", "--reference-opt", "-O2",
                              "--reference-link-libs", "-lm"])
    arm("a value that is no path may begin with `-` (the control)",
        got is not None and (got["reference_opt"], got["reference_link_libs"]) == ("-O2", "-lm"),
        "exit %r: %s" % (code, text[:300]) if got is None else "got %r" % ((got["reference_opt"],),))
    shown = report_lines(_REPORT_SAMPLE)
    rows = [l for l in shown if l.startswith(_REPORT_ROW)]
    arm("the phase report shows the table's header AND every row under it",
        len(rows) == 4 and rows[0].startswith(_REPORT_ROW + "phase") and any("optimize" in r for r in rows)
        and any("[other]" in r for r in rows), "reported %r" % (shown,))
    arm("the phase report shows the `compile time` line and nothing of the traces around it (the control)",
        shown[:1] == ["dsscp: compile time 1.25s  (process wall)"] and not any(l.startswith("opt:") for l in shown),
        "reported %r" % (shown,))
    broken = report_lines(_REPORT_VIOLATION_SAMPLE)
    arm("a broken run's report shows the violation banner, each `!` line, and the memo and batch rows after them",
        any(l.startswith(_REPORT_BANNER) and "INVARIANT VIOLATION" in l for l in broken)
        and any(l.startswith(_REPORT_ROW + "! ") for l in broken)
        and any("pre-scan memo" in l for l in broken) and any("compile-units" in l for l in broken),
        "reported %r" % (broken,))
    arm("a broken run's report still ends at the first line of neither prefix (the control)",
        len(broken) == 7 and not any(l.startswith("opt:") for l in broken), "reported %r" % (broken,))

    # The host-equality gate, over images this self-test writes itself.
    import tempfile
    with tempfile.TemporaryDirectory(prefix="dss-profile-compile-") as perf:
        def image(label, files):
            for name, data in files.items():
                p = os.path.join(perf, label, "image", "fmt", name)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb") as fh:
                    fh.write(data)

        def judged(labels):
            try:
                return host_equality(perf, labels)
            except HostEqualityRefusal as exc:
                return "REFUSED: %s" % exc

        image("a", {"app": b"same bytes"})
        image("b", {"app": b"same bytes"})
        image("c", {"app": b"other bytes"})
        image("d", {"app": b"same bytes", "app.map": b"x"})
        os.makedirs(os.path.join(perf, "empty", "image"))
        got = judged("a,b")
        arm("host-equality: two legs whose image is the same bytes pass, naming the file's md5",
            isinstance(got, tuple) and got[0] and got[1][-1].startswith(HOST_OK_TOKEN + " fmt/app=")
            and got[1][-1].endswith("(a, b)"), "got %r" % (got,))
        got = judged("a,c")
        import hashlib
        md5s = (hashlib.md5(b"same bytes").hexdigest(), hashlib.md5(b"other bytes").hexdigest())
        arm("host-equality: two legs whose image differs FAIL, each leg's md5 said",
            isinstance(got, tuple) and not got[0] and got[1][-1].startswith(HOST_FAILED_TOKEN)
            and got[1][0].startswith("host-equality: a: ") and md5s[0] in got[1][0]
            and got[1][1].startswith("host-equality: c: ") and md5s[1] in got[1][1], "got %r" % (got,))
        got = judged("a,d")
        arm("host-equality: a leg that left another file beside the same image FAILS",
            isinstance(got, tuple) and not got[0], "got %r" % (got,))
        got = judged("a")
        arm("host-equality: one label is REFUSED before any image is read",
            isinstance(got, str) and "at least two labels" in got, "got %r" % (got,))
        got = judged("a,a")
        arm("host-equality: a label named twice is REFUSED",
            isinstance(got, str) and "names a label twice" in got, "got %r" % (got,))
        got = judged("a,empty")
        arm("host-equality: a leg whose image directory holds no file is REFUSED by name, with the pull to run",
            isinstance(got, str) and "leg empty has no image" in got and "--pull build/perf/empty/image/" in got,
            "got %r" % (got,))
    failed = ran.count(False)
    if len(ran) != EXPECTED_ARMS:
        print("%s%d arm(s) ran, %d expected -- FAIL" % (SELFTEST_MARKER, len(ran), EXPECTED_ARMS))
        return 1
    print("%s%d arm(s), %d failed%s" % (SELFTEST_MARKER, len(ran), failed, "" if not failed else " -- FAIL"))
    return 0 if not failed else 1


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv == ["--self-test"]:
        return self_test()
    if argv[:1] == ["--host-equality"]:
        if len(argv) != 2:
            die("--host-equality takes one value, the labels, comma-separated, and nothing else. See --help.")
        return run_host_equality(argv[1])
    o = parse_cli(argv)
    if o["help"]:
        sys.stdout.write(__doc__ or "profile-compile: no help text (python -OO)\n")
        return 0

    augment_path()
    repo = resolve_repo()
    # ★★★ NAME THE CONFIG TREE. `findShippedConfig` prefers $DSS_CONFIG_ROOT and
    # otherwise WALKS UP FROM THE CWD, so a leg driven over ssh (cwd = $HOME) finds
    # nothing and the compile dies, while a leg driven from Windows INTO WSL silently
    # reads the WINDOWS tree across the 9p mount. That second one is not merely the
    # wrong tree: it puts every config and shipped-header read on a filesystem ~10x
    # slower than the one the sources are on, and the cost lands in phases nobody
    # would think to suspect. ✔MEASURED, and it VOIDED the first cross-leg run of this
    # tool's cycle: WSL reported preprocess-splice 1m00.2s and [other] 51.5s against
    # the Windows host's 9.8s and 5.8s -- an artefact of the READER, not a property of
    # the host. The variable is STATED here rather than inherited, for every child.
    os.environ["DSS_CONFIG_ROOT"] = repo

    label = o["label"]
    kit = _under(repo, o["kit"])
    out = _under(repo, o["out"]) if o["out"] else os.path.join(repo, "build", "perf", label)
    build_dir = (_under(repo, o["build_dir"]) if o["build_dir"]
                 else os.path.join(repo, "build", "rel"))
    try:
        os.makedirs(out, exist_ok=True)
    except OSError as exc:
        die("cannot create %s (%s)" % (out, exc))
    if not sys.executable:
        die("this interpreter does not know its own path (sys.executable is empty), so "
            "the support program cannot be started with it")
    support_path = os.path.join(repo, *SUPPORT_REL)
    if not os.path.isfile(support_path):
        die("missing %s" % support_path)

    host_report(label, repo)

    # ── the compiler: ALWAYS Release, and the build type is READ, not assumed ──
    say(label, "the compiler (build dir: %s)" % build_dir)
    if not o["no_build"]:
        build_compiler(repo, build_dir, out, effective_jobs(o["jobs"]))
    else:
        info("--no-build: reusing whatever is already in %s" % build_dir)
    dss = find_dsscp(build_dir)
    if dss is None:
        die("no dsscp under %s (looked for both dsscp and dsscp.exe)" % build_dir)
    # ★★ THE ASSERTION THAT MAKES THE NUMBER MEAN ANYTHING. --require Release exits
    # non-zero and says what it read and where it read it from; there is no flag to
    # proceed anyway, because a non-Release timing published beside Release ones is
    # the exact defect this tool was promoted out of.
    rc, _ = support(repo, support_path, ["build-type", dss, "--require", "Release"])
    if rc != 0:
        die("the compiler is not a Release build")
    info("compiler : %s" % dss)

    # ── the subject: the kit, rewritten onto this host's paths ──
    say(label, "materialize the subject")
    man = os.path.join(out, "subject.dss-project.json")
    rc, _ = support(repo, support_path, ["manifest", "--kit", kit, "--target", o["target"],
                                         "--out", man])
    if rc != 0:
        die("could not materialize the kit manifest on this host")

    # ── the measurement ──
    # ONE run carries both payloads: DSS_OPT_TRACE costs ~1.5% (✔MEASURED on the
    # Windows host: 3m29.7s traced against 3m32.9s clean, i.e. inside the noise), so a
    # second untraced run would buy nothing and cost another full compile.
    # rc comes back from timed-gate, which captured it DIRECTLY from the compiler and
    # refuses (rc 3) an exit-0 that produced no `compile time` report of its own.
    say(label, "compile  (target=%s, --config=release)" % o["target"])
    log = os.path.join(out, "compile.log")
    rc, _ = support(repo, support_path,
                    ["timed-gate", "--log", log, "--witness", "compile time", "--",
                     dss, "--project", man, "--config=release", "--time", "--output",
                     os.path.join(out, "image")],
                    extra_env={"DSS_OPT_TRACE": "1"})
    if rc != 0:
        # ⚠ NO SUCCESS TOKEN ON THIS PATH, EVER. The first version of this tool emitted
        # `PROFILE-LEG-OK vps-arm64 rc=1` over a compile that had died before parsing a
        # single file -- a success string the program wrote about itself, which is what
        # a witness gate exists to refuse.
        sys.stdout.write("%s %s rc=%d  (log: %s)\n" % (FAILED_TOKEN, label, rc, log))
        tail(log, 30)
        return rc

    say(label, "phase report")
    phase_report(log)
    say(label, "optimizer passes")
    _, text = support(repo, support_path, ["agg-trace", log], capture=True)
    for line in (text or "").splitlines()[:40]:
        sys.stdout.write(line + "\n")
    say(label, "artifact")
    list_artifacts(os.path.join(out, "image"), 5)

    if o["gcc_reference"]:
        say(label, "gcc yardstick (same TUs, same manifest)")
        rc, _ = support(repo, support_path, ["gcc-reference", "--manifest", man, "--out", out,
                                             "--jobs", str(effective_jobs(o["jobs"])),
                                             "--cc=%s" % o["reference_cc"], "--opt=%s" % o["reference_opt"],
                                             "--link-libs=%s" % o["reference_link_libs"]])
        if rc != 0:
            die("the gcc reference failed")

    sys.stdout.write("\n%s %s   (out: %s)\n" % (OK_TOKEN, label, out))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
