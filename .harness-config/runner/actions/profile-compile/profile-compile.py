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
 0. Once, on a host that has a STAGED subject (the Windows box, after a
    real-examples/c/sqlite run has staged sqlite and emitted a manifest):
      python .harness-config/runner/actions/profile-compile/profile-compile-support.py kit \
          --manifest build/real-examples/c/sqlite/windows/sqlite3.elf64-x86_64.dss-project.json \
          --root stage=build/real-examples/c/sqlite/windows/stage \
          --root libs="$USERPROFILE/.cache/dsscp/harness-libs" \
          --out build/perf/kit
    The kit is COPIED, never re-staged: the sqlite harness pulls upstream on
    every run, so a host that stages for itself is not compiling the same
    program as its peers.
 1. Every leg, through the action runner (the kit path is the action's `kit` input,
    read from the runner value directory `.harness-config/runner/.env/`, or given as
    `dssharness run profile-compile --input kit=<dir>`; see `dssharness help runners`):
      dssharness run profile-compile --legs linux-x86_64-release
    Only a RELEASE leg that the `profile-compile` runner in `.harness-config/config.json`
    declares: the program refuses any dsscp that is not a Release build (below).
    ⚠ A leg that needs `sync` (WSL, ssh) waits on DssHarness round four, whose sync
    ships actions.
    By hand on one host, the same program:
      python3 .harness-config/runner/actions/profile-compile/profile-compile.py --kit build/perf/kit \
          --label win-x86_64 --target x86_64:elf64-x86_64-linux-exec
 2. The yardstick, on any host with gcc (the same TUs, the same manifest):
      python3 .harness-config/runner/actions/profile-compile/profile-compile-support.py gcc-reference \
          --manifest build/perf/wsl-x86_64/subject.dss-project.json \
          --out build/perf/wsl-x86_64 --jobs 32
    ✔MEASURED 2026-08-18 on WSL, 103 TUs, gcc 13 -O2: -j1 21.5 s, -j32 4.8 s,
    against DSS's 1m40.8 s on that same host.

Usage:
  profile-compile.py --kit <dir> --target <spec> --label <name>
                     [--repo <dir>] [--build-dir <dir>] [--no-build]
                     [--out <dir>] [--jobs N] [--gcc-reference]

  --kit <dir>        a materialized kit (`profile-compile-support.py kit`). REQUIRED.
  --target <spec>    the ONE <target>:<format> every leg compiles. REQUIRED.
  --label <name>     names this leg in the report and in the witness line. REQUIRED.
  --repo <dir>       the dsscp checkout (default: the tree this file lives in).
  --build-dir <dir>  the compiler's build tree (default: <repo>/build/rel).
  --no-build         time the dsscp already in the build tree; configure nothing.
  --out <dir>        logs, the subject manifest and the image (default:
                     <repo>/build/perf/<label>).
  --jobs N           build and gcc-reference parallelism (default: DSS_JOBS, then 6).
  --gcc-reference    also time gcc over the same TUs from the same manifest.
  -h, --help         show this help and exit.
  A relative --kit, --out or --build-dir is taken from the repository root; a
  relative --repo from the directory the program is run in. Every value flag
  needs a non-empty value; anything else on the command line is refused.

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

VALUE_FLAGS = ("--kit", "--target", "--label", "--repo", "--build-dir", "--out", "--jobs")
SWITCHES = ("--no-build", "--gcc-reference")
DEFAULT_JOBS = 6

# The support program, taken from the REPOSITORY being profiled (as it always was),
# not from beside this file: `--repo` names the tree whose tools run.
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

_PHASE_LINE = re.compile(r"compile time|^dsscp:   phase")


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
    o = {"kit": "", "target": "", "label": "", "repo": "", "build_dir": "", "out": "",
         "jobs": "", "no_build": False, "gcc_reference": False, "help": False}
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
    if not o["kit"]:
        die("--kit is required")
    if not o["target"]:
        die("--target is required (e.g. x86_64:elf64-x86_64-linux-exec)")
    if not o["label"]:
        die("--label is required (it names this leg in the report)")
    if o["jobs"]:
        _positive_jobs(o["jobs"], "--jobs")
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


def _owning_tree_root():
    """The tree this file lives in, by `owning-tree.py`'s walk, loaded as a SIBLING.

    ⓘ Never by counting `..` from this file: that count named `.harness-config/runner`
    the day this program moved out of `scripts/` (2026-09-18). A missing owner or a
    walk that finds nothing is a refusal by name, never a fallback to the caller's cwd.
    """
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                        "owning-tree", "owning-tree.py")
    if not os.path.isfile(path):
        die("cannot find %s -- the tree this program lives in is named there and "
            "nowhere else (or pass --repo)" % path)
    saved = sys.dont_write_bytecode
    sys.dont_write_bytecode = True          # no __pycache__ in a directory not ours
    try:
        spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    finally:
        sys.dont_write_bytecode = saved
    try:
        return mod.owning_tree(__file__)
    except mod.Refusal as exc:
        die("cannot name the tree this program lives in: %s" % exc)


def resolve_repo(opt_repo):
    """The dsscp checkout to profile: `--repo` (from the caller's directory), else the
    tree this file lives in -- a profiler that has to be told where it is can be
    pointed at a different checkout than the compiler it just built."""
    repo = os.path.abspath(opt_repo) if opt_repo else _owning_tree_root()
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


def phase_report(log):
    """The compile log's `compile time` line and its `dsscp:   phase` lines."""
    try:
        with open(log, "rb") as fh:
            text = fh.read().decode("utf-8", "replace")
    except OSError:
        return
    for line in text.splitlines():
        if _PHASE_LINE.search(line):
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


# ── the run ─────────────────────────────────────────────────────────────────────
def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    o = parse_cli(argv)
    if o["help"]:
        sys.stdout.write(__doc__ or "profile-compile: no help text (python -OO)\n")
        return 0

    augment_path()
    repo = resolve_repo(o["repo"])
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
                                             "--jobs", str(effective_jobs(o["jobs"]))])
        if rc != 0:
            die("the gcc reference failed")

    sys.stdout.write("\n%s %s   (out: %s)\n" % (OK_TOKEN, label, out))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
