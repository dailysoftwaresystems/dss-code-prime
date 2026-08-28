#!/usr/bin/env python3
# PURPOSE: time dsscp against gcc/clang/MSVC/tcc on ONE host over a subject size ladder, naming every reference it could not find.
"""compile-bench.py -- THE COMPILER AXIS. One host, many compilers, one ladder.

WHY THIS IS A SIBLING OF `profile-compile` AND NOT AN EXTENSION OF IT
--------------------------------------------------------------------
`scripts/profile-compile/` answers the OTHER half of the same question, and its
declared purpose says so in one line: "compile one fixed subject with a RELEASE
dsscp on this host and report where the time went, so the HOST is the only
variable across legs." Its whole contract is that the COMPILER is held fixed
(dsscp, Release, one build) and the HOST moves. This tool inverts that: the HOST
is held fixed and the COMPILER moves. Bolting a compiler axis onto a tool whose
header commits, in capitals, to "the only variable is the host" would not extend
that contract, it would void it.

Three further facts make them different programs rather than two modes of one:
  * its SUBJECT is a `--kit` materialized from a staged sqlite manifest, and it
    refuses without one -- so it cannot run on a leg that has not staged sqlite;
    every subject here is either in-repo or generated, so any leg can run this
    with nothing staged;
  * its RUN MODEL is one traced multi-minute compile (deliberately: one run
    carries both payloads, because DSS_OPT_TRACE costs ~1.5%); this one repeats
    a sub-second compile N times and reports a median, which is the only way a
    100 ms difference is a measurement rather than an anecdote;
  * it profiles WHERE the time went inside dsscp; this one has no access to the
    inside of gcc and does not pretend to -- it times the whole task from the
    outside, which is the only thing that is comparable across vendors.

★★ WHAT IS REUSED RATHER THAN REWRITTEN. The "is this actually a Release build?"
assertion is `read_build_type` from `profile-compile-support.py`, IMPORTED FROM
THAT FILE by path rather than reimplemented -- it already knows that a
multi-config generator hides the answer in a path component and a single-config
one hides it in `CMAKE_BUILD_TYPE`, and a second copy of that knowledge is a
second thing to keep true. If that function is wrong, it is wrong in one place.

WHAT MAKES A NUMBER HERE WORTH ANYTHING
---------------------------------------
★★★ EVERY REFERENCE THIS HOST DID NOT MEASURE IS PRINTED, WITH THE REASON --
`ABSENT` if nothing by that name resolves, `UNUSABLE` if something does and will
not run. A benchmark that quietly drops MSVC reads EXACTLY like one where MSVC
was fast. This repository has been bitten more than once by an instrument
reporting success over something it could not observe, so the reference table
lists every compiler this tool knows how to drive, present or not, and the
unmeasured ones carry the probe that failed.

★★★ A CELL IS `ok` ONLY IF AN ARTIFACT APPEARED. The output directory is emptied
before every single run and must contain at least one regular file afterwards.
An exit code of 0 over a compile that emitted nothing is not a compile, and
timing it would publish a number for work that did not happen. (✔MEASURED while
building this tool: a 17-file dsscp invocation returned rc=1 having opened none
of its inputs -- a CR that a redirected Windows `python` had appended to every
path. Every cell in that column would have been "fast".)

★★★ AN UNKNOWN FLAG IS A REFUSAL. A run that silently measures something other
than what its command line said is the failure mode this repository cares most
about.

★ THE CLOCK IS `time.monotonic()`, READ TWICE IN THIS ONE PROCESS. WSL2's
CLOCK_REALTIME on this workstation oscillates by tens of seconds every few
seconds (project_wsl2_clock_realtime_broken_2026_08_01); a wall-clock difference
taken across that has already produced a compile that took MINUS 11.7 seconds.
monotonic()'s epoch is documented as undefined, so both reads must happen inside
one process -- which they do.

★ EXECUTABLE PATHS ARE ABSOLUTE AND NATIVE. ✔MEASURED on this Windows host while
building this tool: `subprocess.call(['build/p35-rel/bin/dss/dsscp.exe', ...])`
raises FileNotFoundError on a path that provably exists and opens, because
CreateProcess does not resolve a forward-slashed relative program name the way
`open()` does. Every program this tool launches is resolved to an absolute,
os.sep-normalised path first.

WHAT IS AND IS NOT COMPARABLE
-----------------------------
Stated in the OUTPUT as well as here, because a caveat that lives only in a
source comment is a caveat the reader of the number never saw. See CAVEATS.

Usage:
    python scripts/compile-bench/compile-bench.py --dsscp <path-to-dsscp> [options]

    --dsscp PATH        the compiler under test. REQUIRED; refused if absent.
    --target SPEC       <target>:<format>; default derived from this host and PRINTED.
    --language NAME     source language for dsscp (default: c).
    --runs N            measured runs per cell (default: 10).
    --warmup N          discarded runs per cell (default: 2).
    --jobs N            dsscp --jobs for the parallel multi-TU arm (default: 6).
    --subjects LIST     comma-separated subject ids (default: every built-in).
    --multi-tus LIST    TU counts for the generated multi-TU ladder (default: 1,4,17).
    --extra-subject ID=PATH[,PATH...]   add a real subject of your own (repeatable).
    --out DIR           scratch for generated sources, artifacts and logs.
    --label NAME        names this leg in the report (default: derived from the host).
    --cc NAME=PATH      override where a reference compiler lives (repeatable).
    --require-build-type NAME   refuse unless dsscp was built this way (default: Release).
    --json PATH         also write the whole reading as JSON, for a before/after diff.
    --help
"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = "compile-bench"


def die(msg):
    """Refuse, loudly, on stderr, and never leave a partial reading looking whole."""
    sys.stderr.write("%s: REFUSED: %s\n" % (TOOL, msg))
    raise SystemExit(2)


# ── the repository ──────────────────────────────────────────────────────────
# Walk up for the shipped-config tree rather than counting `..` from __file__:
# the count is a claim about this file's depth that nothing rechecks, and this
# repository has already paid for one of those (a help window pinned to a line
# range that a later inserted header line silently slid off the end of).
def repo_root():
    d = HERE
    while True:
        if os.path.isdir(os.path.join(d, "src", "dss-config")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            die("no src/dss-config in any ancestor of %s -- that is not a dsscp "
                "checkout, and this tool will not measure a tree it cannot name" % HERE)
        d = parent


# ── REUSE, NOT REIMPLEMENTATION ─────────────────────────────────────────────
# `read_build_type` already knows the two ways CMake records a build type and
# which one a given tree uses. Importing the file it lives in keeps that
# knowledge in one place; `dont_write_bytecode` keeps this import from dropping a
# __pycache__ into a directory this tool does not own.
def load_build_type_reader(repo):
    path = os.path.join(repo, "scripts", "profile-compile", "profile-compile-support.py")
    if not os.path.isfile(path):
        die("scripts/profile-compile/profile-compile-support.py is missing under %s. "
            "The build-type assertion is not optional -- a non-Release timing "
            "published beside Release ones is worse than no timing -- so this "
            "refuses rather than guess how the compiler was built." % repo)
    saved = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec = importlib.util.spec_from_file_location("dss_profile_compile_support", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    finally:
        sys.dont_write_bytecode = saved
    if not hasattr(mod, "read_build_type"):
        die("%s no longer exports read_build_type. Fix that file rather than "
            "growing a second copy of the check here." % path)
    return mod.read_build_type


# ── the host, and the target that makes gcc a fair opponent ─────────────────
# gcc compiles for the machine it runs on. To compare whole tasks, dsscp must be
# aimed at that same machine, so the default target is DERIVED from this host and
# PRINTED -- it is a benchmark-fairness decision, not compiler logic, and
# `--target` overrides it on any leg whose default is wrong.
HOST_TARGETS = {
    ("win32", "x86_64"):  "x86_64:pe64-x86_64-windows-exec",
    ("linux", "x86_64"):  "x86_64:elf64-x86_64-linux-exec",
    ("linux", "arm64"):   "arm64:elf64-aarch64-linux-exec",
    ("darwin", "arm64"):  "arm64:macho64-arm64-darwin-exec",
    ("darwin", "x86_64"): "x86_64:macho64-x86_64-darwin-exec",
}
MACHINE_ALIASES = {
    "amd64": "x86_64", "x86_64": "x86_64", "x64": "x86_64",
    "aarch64": "arm64", "arm64": "arm64",
}


def host_key():
    osname = "win32" if sys.platform.startswith("win") else \
             "darwin" if sys.platform == "darwin" else \
             "linux" if sys.platform.startswith("linux") else sys.platform
    mach = MACHINE_ALIASES.get(platform.machine().lower(), platform.machine().lower())
    return osname, mach


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def native(path):
    """Absolute and os.sep-normalised. See the header note on CreateProcess."""
    return os.path.normpath(os.path.abspath(path))


# ── the host's own busy-ness, measured rather than assumed ──────────────────
# ★★ WHY A BENCHMARK MUST MEASURE ITS OWN HOST. ✔MEASURED while building this
# tool: the same tiny compile, same binary, same flags, read 178 ms when the
# orchestrator ran it on a quiet box and 220 ms here with three other build lanes
# running -- a 24% shift that has nothing to do with the compiler. An A/B of every
# harness variable (cwd, DSS_CONFIG_ROOT, log redirection) moved the number by
# less than the noise, so the harness was not the cause and the LOAD was.
# A before/after comparison taken across two different load conditions would
# publish that difference as a fix, or hide a fix under it. This calibration is a
# fixed integer workload timed at the start and again at the end: it does not say
# what else is running, but it says whether THIS machine was equally able to run
# the two ends of the reading, which is the part that invalidates a comparison.
#
# ⚠ IT DOES NOT SAY *WHY*, AND THE REPORT MUST NOT EITHER. Sustained drift can be
# another workload on the box OR this benchmark's own minutes of compiling moving
# the CPU off its boost clocks, and nothing here distinguishes them. Naming a
# cause would be an inference wearing a measurement's clothes.
#
# ★ THE MEDIAN OF THREE, NOT ONE. ✔MEASURED on this host: twelve back-to-back
# reps of this loop in one quiet process ran 115..142 ms, so a single rep can be
# 20% off on its own and a one-versus-one comparison would raise a false alarm
# about the machine roughly as often as a true one.
def calibrate(reps=3):
    out = []
    for _ in range(reps):
        t0 = time.monotonic()
        acc = 0
        for i in range(3000000):
            acc = (acc + i * 7) & 0xFFFFFFFF
        out.append((time.monotonic() - t0) * 1000.0)
    return statistics.median(out)


# ── the references ──────────────────────────────────────────────────────────
# Each entry says how to BUILD a whole-task command line for that vendor and how
# to ask it its version. `opt` is the vendor's OWN release switch; `None` means
# the vendor has no such notion, which is printed as such rather than faked.
class Reference(object):
    def __init__(self, name, opt_flag, version_argv, out_flag_style, note=""):
        self.name = name
        self.opt_flag = opt_flag
        self.version_argv = version_argv
        self.out_flag_style = out_flag_style
        self.note = note
        self.path = None
        self.version = None
        self.absent_reason = None
        # ★ PRESENT-BUT-UNUSABLE IS ITS OWN STATE AND GETS ITS OWN WORD.
        # "this host has no clang" and "this host has something called clang that
        # will not run" are both unmeasured, but they call for different action --
        # one is a machine to provision, the other is an install to repair. Folding
        # them into one label would send the reader to the wrong one.
        self.state = "ABSENT"

    @property
    def present(self):
        return self.path is not None

    def command(self, sources, out_dir, opt):
        exe = self.path
        if self.out_flag_style == "unix":
            argv = [exe]
            if opt and self.opt_flag:
                argv.append(self.opt_flag)
            return argv + list(sources) + ["-o", os.path.join(out_dir, "cb_ref.exe")]
        if self.out_flag_style == "msvc":
            argv = [exe, "/nologo"]
            if opt and self.opt_flag:
                argv.append(self.opt_flag)
            # /Fo MUST end in a separator or cl treats it as a file name and
            # every object collides on the last one compiled.
            return argv + list(sources) + [
                "/Fe:" + os.path.join(out_dir, "cb_ref.exe"),
                "/Fo:" + os.path.join(out_dir, ""),
            ]
        die("internal: unknown out_flag_style %r for %s" % (self.out_flag_style, self.name))


def known_references():
    return [
        Reference("gcc",   "-O2", ["--version"], "unix"),
        Reference("clang", "-O2", ["--version"], "unix"),
        Reference("cl",    "/O2", [],            "msvc",
                  "MSVC only resolves from a Developer Command Prompt; a plain "
                  "shell will not find it even on a machine that has it"),
        Reference("tcc",   None,  ["-v"],        "unix",
                  "tcc has no optimization levels, so it has no release arm"),
    ]


def probe_references(overrides):
    refs = known_references()
    for r in refs:
        forced = overrides.get(r.name)
        if forced:
            p = native(forced)
            if not os.path.isfile(p):
                die("--cc %s=%s names a file that does not exist" % (r.name, forced))
            r.path = p
        else:
            found = shutil.which(r.name)
            r.path = native(found) if found else None
        if not r.present:
            r.state = "ABSENT"
            r.absent_reason = "not resolvable on PATH by name '%s'" % r.name
            if r.note:
                r.absent_reason += " (%s)" % r.note
            continue
        try:
            out = subprocess.run([r.path] + r.version_argv, capture_output=True,
                                 text=True, timeout=60)
            blob = (out.stdout or "") + (out.stderr or "")
            r.version = next((ln.strip() for ln in blob.splitlines() if ln.strip()), "?")
        except Exception as e:
            where = r.path
            r.path = None
            r.state = "UNUSABLE"
            r.absent_reason = ("found at %s but it would not answer its own version "
                               "(%s) -- a binary that cannot do that will not compile "
                               "anything either" % (where, e))
    return refs


# ── the subjects ────────────────────────────────────────────────────────────
# THE LADDER, AND WHY EACH RUNG IS ON IT. The defect under repair is a FIXED
# per-invocation cost with a per-TU component on top; a single subject cannot
# separate those two, and separating them is what says whether DSS's
# disadvantage is a startup problem or a throughput problem -- different fixes.
#
#   tiny   one line, no headers      the floor, with nothing else in it
#   hello  three shipped headers     the commonest real shape there is; the
#                                    difference from `tiny` IS the header cost
#   mid    a real 400-line example   real code, no preprocessor
#   large  the largest single TU
#          examples/c ships          bulk through the whole pipeline
#   multiN N units, ONE invocation   fixed floor vs per-TU slope, both vendors
#
# `tiny`, `hello` and `multiN` are GENERATED, deterministically, from the text in
# this file: a subject that is generated identically on every leg is a subject
# two legs can be compared on, whereas a subject copied around is one more thing
# that can silently differ. Every subject prints an md5 over its concatenated
# bytes so that "the same subject" is a checkable claim rather than an assumption.
TINY_SRC = "int main(void) { return 0; }\n"

HELLO_SRC = """#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s-%d", "compile-bench", 42);
    return (int)strlen(buf) > 0 ? 0 : 1;
}
"""

# One synthetic unit. Deliberately NOT a one-liner: it carries a struct, an
# array, a loop and a switch, so a per-TU cost measured on it is a cost paid on
# code rather than on an empty file. Everything is parameterised by the unit
# index so that N units are N distinct symbol sets and link into one image --
# which is exactly why the multi-TU subject cannot be N real examples: each of
# those defines `main`, and N of them do not link.
MULTI_UNIT_SRC = """/* compile-bench synthetic unit %(i)d -- generated; do not edit by hand */
struct cb_pair%(i)d { int lo; int hi; };

static int cb_fold%(i)d(int v) {
    switch (v & 7) {
        case 0: return v + %(i)d;
        case 1: return v - %(i)d;
        case 2: return v * 3;
        case 3: return v / 2;
        case 4: return v ^ %(i)d;
        case 5: return v | 1;
        case 6: return v & 0x7f;
        default: return v;
    }
}

static int cb_scan%(i)d(const struct cb_pair%(i)d *p, int n) {
    int acc = 0;
    for (int k = 0; k < n; ++k) {
        acc += cb_fold%(i)d(p[k].lo) - cb_fold%(i)d(p[k].hi);
        if (acc > 100000) acc >>= 1;
    }
    return acc;
}

int cb_unit%(i)d(int seed) {
    struct cb_pair%(i)d table[16];
    for (int k = 0; k < 16; ++k) {
        table[k].lo = seed + k;
        table[k].hi = seed - k;
    }
    return cb_scan%(i)d(table, 16) + %(i)d;
}
"""

# The two real rungs. Named by repo-relative path so that a rename reds here
# instead of silently swapping the subject under a published number.
REAL_MID = os.path.join("examples", "c", "switch_vdbe_dispatch", "main.c")
REAL_LARGE = os.path.join("examples", "c", "switch_wide_bound", "main.c")


class Subject(object):
    def __init__(self, sid, sources, provenance, parallel_arms=False):
        self.sid = sid
        self.sources = [native(s) for s in sources]
        self.provenance = provenance
        self.parallel_arms = parallel_arms
        self.bytes = 0
        self.lines = 0
        h = hashlib.md5()
        for s in self.sources:
            with open(s, "rb") as f:
                data = f.read()
            h.update(data)
            self.bytes += len(data)
            self.lines += data.count(b"\n")
        self.md5 = h.hexdigest()

    @property
    def tus(self):
        return len(self.sources)


def write_text(path, text):
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    # newline='\n' unconditionally: a generated subject whose bytes depend on the
    # host that generated it is not one subject, and on Windows the default
    # translates every '\n' into CRLF.
    with io.open(path, "w", encoding="ascii", newline="\n") as f:
        f.write(text)
    return path


def generate_multi(src_dir, n_units):
    """n_units workers plus one driver: n_units + 1 translation units."""
    files = []
    for i in range(n_units):
        files.append(write_text(os.path.join(src_dir, "cb_unit%02d.c" % i),
                                MULTI_UNIT_SRC % {"i": i}))
    decls = "".join("int cb_unit%d(int);\n" % i for i in range(n_units))
    calls = "".join("    acc += cb_unit%d(acc & 0xff);\n" % i for i in range(n_units))
    files.append(write_text(
        os.path.join(src_dir, "cb_main.c"),
        decls + "\nint main(void) {\n    int acc = 1;\n" + calls +
        "    return acc != 0 ? 0 : 1;\n}\n"))
    return files


def build_subjects(repo, out, wanted, multi_tus, extra):
    gen = os.path.join(out, "subjects")
    catalog = []

    def real(sid, rel, why):
        p = os.path.join(repo, rel)
        if not os.path.isfile(p):
            die("subject '%s' wants %s and it is not there. Either the example was "
                "renamed -- in which case fix this tool rather than drop the rung -- "
                "or this is not the tree it was written against." % (sid, rel))
        return Subject(sid, [p], "%s (%s)" % (rel.replace(os.sep, "/"), why))

    catalog.append(Subject("tiny", [write_text(os.path.join(gen, "tiny", "tiny.c"), TINY_SRC)],
                           "GENERATED -- one line, no headers: the per-invocation floor"))
    catalog.append(Subject("hello", [write_text(os.path.join(gen, "hello", "hello.c"), HELLO_SRC)],
                           "GENERATED -- 3 standard headers: hello minus tiny IS the header cost"))
    catalog.append(real("mid", REAL_MID, "real, ~400 lines, sqlite VDBE dispatch shape"))
    catalog.append(real("large", REAL_LARGE, "real, the largest single TU examples/c ships"))
    for n in multi_tus:
        sid = "multi%02d" % n
        catalog.append(Subject(sid, generate_multi(os.path.join(gen, sid), n - 1),
                               "GENERATED -- %d TU%s in ONE invocation"
                               % (n, "" if n == 1 else "s"),
                               parallel_arms=True))
    for sid, paths in extra:
        for p in paths:
            if not os.path.isfile(p):
                die("--extra-subject %s names %s, which does not exist" % (sid, p))
        catalog.append(Subject(sid, paths, "CALLER-SUPPLIED", parallel_arms=len(paths) > 1))

    by_id = dict((s.sid, s) for s in catalog)
    if wanted is None:
        return catalog
    picked = []
    for sid in wanted:
        if sid not in by_id:
            die("unknown subject '%s'. This host offers: %s"
                % (sid, ", ".join(s.sid for s in catalog)))
        picked.append(by_id[sid])
    return picked


# ── the measurement ─────────────────────────────────────────────────────────
class Cell(object):
    def __init__(self, compiler, arm, jobs):
        self.compiler = compiler
        self.arm = arm
        self.jobs = jobs
        self.samples = []
        self.status = "ok"
        self.detail = ""

    @property
    def ok(self):
        return self.status == "ok" and len(self.samples) > 0

    @property
    def median(self):
        return statistics.median(self.samples)

    @property
    def lo(self):
        return min(self.samples)

    @property
    def spread(self):
        return max(self.samples) - min(self.samples)


def artifact_present(probe):
    """The assertion that makes a timing mean something. A directory must hold at
    least one file; a named file must exist and be non-empty. Anything else is an
    exit-0 over work that did not happen."""
    if os.path.isdir(probe):
        return sum(len(fs) for _, _, fs in os.walk(probe)) > 0
    return os.path.isfile(probe) and os.path.getsize(probe) > 0


def timed_run(argv, run_dir, probe, log_path, env):
    """One run. Returns (ms, rc, produced). The run directory is emptied FIRST, so
    a run can never be credited with the previous run's artifact."""
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)
    with open(log_path, "wb") as log:
        t0 = time.monotonic()
        rc = subprocess.call(argv, stdout=log, stderr=subprocess.STDOUT,
                             cwd=run_dir, env=env)
        ms = (time.monotonic() - t0) * 1000.0
    return ms, rc, artifact_present(probe)


def log_tail(path, n=12):
    try:
        with io.open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except Exception:
        return "(no log)"
    return " | ".join(lines[-n:])[:600]


def measure(cell, argv, run_dir, probe, log_path, runs, warmup, env=None):
    for i in range(warmup + runs):
        ms, rc, produced = timed_run(argv, run_dir, probe, log_path, env)
        if rc != 0:
            cell.status = "FAILED"
            cell.detail = "rc=%d on run %d: %s" % (rc, i + 1, log_tail(log_path))
            return cell
        if not produced:
            cell.status = "NO-ARTIFACT"
            cell.detail = ("rc=0 but %s is empty/absent after run %d -- nothing was "
                           "compiled, so nothing is being timed: %s"
                           % (probe, i + 1, log_tail(log_path)))
            return cell
        if i >= warmup:
            cell.samples.append(ms)
    return cell


def dsscp_command(dsscp, sources, image_dir, target, language, release, jobs):
    argv = [dsscp, "--compile"] + list(sources) + ["--language", language,
                                                   "--target", target]
    if release:
        argv.append("--config=release")
    if jobs is not None:
        argv += ["--jobs", str(jobs)]
    return argv + ["--output", image_dir]


# ── the report ──────────────────────────────────────────────────────────────
CAVEATS = [
 ("WHOLE TASK, SOURCE -> EXECUTABLE, AND NOTHING NARROWER.",
  "gcc/clang/cl fork `as` and `ld`/`link.exe` as separate processes; dsscp "
  "assembles and links IN-PROCESS. Source-to-executable is the only slice where "
  "both sides do the same job, so it is the only slice measured here. A "
  "`-c`-only or `-S`-only comparison would be two different jobs wearing one number."),
 ("THE ARTIFACTS ARE NOT THE SAME ARTIFACT.",
  "dsscp links its own shipped source units; gcc/clang/cl link the platform's "
  "libc (UCRT, glibc, libSystem). Same source, different runtime, different "
  "binary. This is a comparison of WORK DONE, not of output equivalence."),
 ("OPTIMIZATION LEVELS ARE NOT EQUIVALENT ACROSS VENDORS.",
  "`--config=release` is DSS's own pipeline, `-O2` is gcc's and `/O2` is MSVC's. "
  "Rows are paired arm-to-arm by INTENT (each vendor's default, then each "
  "vendor's own release switch), never by any claim that the transform sets match."),
 ("THE MULTI-TU ROWS COMPARE ONE COMMAND LINE, NOT PER-TU THROUGHPUT.",
  "gcc given N files on one command line compiles them SERIALLY -- one cc1 "
  "process per file -- then links. dsscp compiles all N inside ONE process on a "
  "thread pool. The dsscp jobs=1 row is the serial like-for-like; the jobs=N row "
  "is what a user typing one command actually waits for. A `make -jN` user gets "
  "gcc parallelism that this tool does not measure, and the production row "
  "D-PERF-CU-POOL-SCALES-HALF-AS-WELL-AS-SEPARATE-PROCESSES is where that "
  "question lives."),
 ("NO RATIO IS PRINTED WHERE A RATIO WOULD MISLEAD.",
  "A ratio appears only where both sides completed the SAME subject in the SAME "
  "arm on this run. Where a reference is ABSENT, or a cell FAILED, the cell says "
  "so and no ratio is derived from it."),
 ("`large` IS BULK, NOT A TYPICAL MIX.",
  "It is the largest single translation unit examples/c ships and it is "
  "switch-dense by construction. It measures how each compiler scales with TU "
  "size; it does not claim to represent an average program."),
 ("ONE HOST ONLY, AND ONE LOAD CONDITION ONLY.",
  "Everything above is a property of this machine, this dsscp binary and these "
  "reference versions. MEASURED while this tool was built: the identical tiny "
  "compile read 178 ms on a quiet box and 220 ms with other build lanes running "
  "-- a 24% shift owed entirely to the host, and an A/B of every harness "
  "variable (cwd, DSS_CONFIG_ROOT, log redirection) moved it by less than the "
  "noise. Compare the calibration figure at the top against the reading you "
  "intend to diff this one against, and prefer `min` over `median` when the two "
  "were not equally quiet: the minimum is the run that got the most machine."),
 ("THE CALIBRATION IS A CPU PROBE, SO A FLAT DRIFT IS NECESSARY AND NOT SUFFICIENT.",
  "It times an integer loop. It therefore does NOT see process-creation latency, "
  "on-access virus scanning, or filesystem-cache state -- and on Windows those "
  "dominate a ~100 ms compile, not arithmetic. MEASURED across three readings on "
  "this host in one hour: gcc's `tiny` median moved 113 -> 166 ms while the "
  "calibration stayed flat. So a flat drift does not license a diff; treat any "
  "before/after difference smaller than the `spread` column as unproven, and "
  "raise `--runs` rather than believe a narrow gap."),
]


def fmt_ms(x):
    return "%8.1f" % x


def print_report(o, meta, refs, results, args):
    w = o.write
    w("\n")
    w("=" * 78 + "\n")
    w("COMPILE-BENCH -- one host, many compilers, one subject ladder\n")
    w("=" * 78 + "\n")
    w("leg           : %s\n" % meta["label"])
    w("when          : %s\n" % meta["when"])
    w("host          : %s %s / %s, %s cores\n"
      % (meta["system"], meta["release"], meta["machine"], meta["cores"]))
    w("dsscp         : %s\n" % meta["dsscp"])
    w("  build type  : %s   [%s]\n" % (meta["build_type"], meta["build_type_how"]))
    w("  md5         : %s\n" % meta["dsscp_md5"])
    w("target        : %s%s\n" % (meta["target"],
                                  "" if meta["target_explicit"] else
                                  "   (DERIVED from this host so that dsscp aims where gcc aims)"))
    w("language      : %s\n" % args["language"])
    w("runs per cell : %d measured, %d warm-up discarded\n" % (args["runs"], args["warmup"]))
    w("parallel arm  : dsscp --jobs %d (multi-TU subjects only)\n" % args["jobs"])
    w("DSS_CONFIG_ROOT: %s   (stated, not inherited from the scratch cwd)\n"
      % meta.get("config_root", "?"))
    w("scratch       : %s\n" % meta["out"])
    cb, ca = meta.get("calib_before"), meta.get("calib_after")
    if cb and ca:
        drift = abs(ca - cb) / max(cb, ca) * 100.0
        w("host calibration: %.0f ms before, %.0f ms after "
          "(median of 3 reps of a fixed integer workload)\n" % (cb, ca))
        if drift > 15.0:
            w("  ** THE MACHINE WAS NOT EQUALLY ABLE TO RUN BOTH ENDS OF THIS\n"
              "     READING (%.0f%% drift). This tool does NOT know why: another\n"
              "     workload on the box and this benchmark's own sustained load\n"
              "     moving the CPU off its boost clocks look identical from here.\n"
              "     Either way the medians below are inflated relative to a quiet\n"
              "     host, and diffing them against a reading with a different\n"
              "     calibration would credit the difference to the compiler.\n" % drift)
        else:
            w("  drift %.0f%% -- the machine ran both ends of this reading about\n"
              "  equally well. That is NOT a claim that it was idle: compare this\n"
              "  figure against the one on the reading you intend to diff against.\n"
              % drift)

    w("\n-- REFERENCES ON THIS HOST " + "-" * 51 + "\n")
    w("  %-8s %-9s %s\n" % ("name", "state", "version / why not"))
    for r in refs:
        if r.present:
            w("  %-8s %-9s %s\n" % (r.name, "present", r.version))
            w("  %-8s %-9s %s\n" % ("", "", r.path))
        else:
            w("  %-8s %-9s %s\n" % (r.name, r.state, r.absent_reason))
    absent = ["%s (%s)" % (r.name, r.state) for r in refs if not r.present]
    if absent:
        w("  ^ %d reference(s) UNMEASURED on this host: %s.\n"
          % (len(absent), ", ".join(absent)))
        w("    They are printed rather than skipped: a benchmark that quietly drops\n")
        w("    a compiler reads exactly like one where that compiler was fast.\n")

    w("\n-- SUBJECTS " + "-" * 66 + "\n")
    w("  %-9s %4s %9s %8s  %s\n" % ("id", "TUs", "bytes", "lines", "md5"))
    for s, _ in results:
        w("  %-9s %4d %9d %8d  %s\n" % (s.sid, s.tus, s.bytes, s.lines, s.md5))
        w("  %-9s %s\n" % ("", s.provenance))
    w("  The md5 is over the concatenated subject bytes: two legs that print the\n")
    w("  same md5 compiled the same program, and two that do not, did not.\n")

    for s, cells in results:
        head = "-- %s   (%d TU, %d bytes, %d line%s) " % (
            s.sid, s.tus, s.bytes, s.lines, "" if s.lines == 1 else "s")
        w("\n" + head + "-" * max(3, 78 - len(head)) + "\n")
        w("  %-8s %-9s %5s %8s %8s %8s %4s  %s\n"
          % ("compiler", "arm", "jobs", "median", "min", "spread", "n", "status"))
        explained = set()
        for c in cells:
            jobs = "-" if c.jobs is None else str(c.jobs)
            if c.ok:
                w("  %-8s %-9s %5s %s %s %s %4d  ok\n"
                  % (c.compiler, c.arm, jobs, fmt_ms(c.median), fmt_ms(c.lo),
                     fmt_ms(c.spread), len(c.samples)))
            else:
                w("  %-8s %-9s %5s %8s %8s %8s %4s  %s\n"
                  % (c.compiler, c.arm, jobs, "-", "-", "-", "-", c.status))
                # The reason is printed ONCE per compiler per subject: repeating
                # it on every arm buries the table it is meant to annotate, and a
                # reason nobody reads is a reason nobody acts on.
                key = (c.compiler, c.status)
                if c.detail and key not in explained:
                    explained.add(key)
                    w("  %-8s %s\n" % ("", c.detail[:220]))
        for line in ratio_lines(s, cells):
            w("  %s\n" % line)

    slope = slope_lines(results)
    if slope:
        w("\n-- FIXED FLOOR vs PER-TU SLOPE " + "-" * 47 + "\n")
        for line in slope:
            w("  %s\n" % line)

    w("\n-- WHAT IS AND IS NOT COMPARABLE " + "-" * 45 + "\n")
    for i, (head, body) in enumerate(CAVEATS, 1):
        w("  %d. %s\n" % (i, head))
        for ln in wrap(body, 72):
            w("     %s\n" % ln)
    w("\n")


def wrap(text, width):
    out, line = [], ""
    for word in text.split():
        if line and len(line) + 1 + len(word) > width:
            out.append(line)
            line = word
        else:
            line = word if not line else line + " " + word
    if line:
        out.append(line)
    return out


def pick(cells, compiler, arm, jobs=None):
    for c in cells:
        if c.compiler == compiler and c.arm == arm and (jobs is None or c.jobs == jobs):
            return c
    return None


def ratio_lines(subject, cells):
    """A ratio ONLY where both sides completed the same subject in the same arm.

    Everything else is named as NOT DERIVED, with the reason -- a missing ratio
    that is merely absent from the page is indistinguishable from a ratio of 1."""
    lines, undrawn = [], []
    job_keys = sorted(set(d.jobs for d in cells if d.compiler == "dsscp"),
                      key=lambda j: (j is not None, j))
    for arm, label in (("default", "default arm"), ("release", "release arm")):
        for c in cells:
            if c.compiler == "dsscp" or c.arm != arm:
                continue
            if not c.ok:
                tag = "%s (%s)" % (c.compiler, c.status)
                if tag not in undrawn:
                    undrawn.append(tag)
                continue
            for jobs in job_keys:
                d = pick(cells, "dsscp", arm, jobs)
                if d is None or not d.ok:
                    continue
                if not subject.parallel_arms:
                    how = "whole task, one command line each"
                    who = "dsscp"
                elif jobs == 1:
                    how = "SERIAL like-for-like (dsscp jobs=1 vs %s serial)" % c.compiler
                    who = "dsscp(jobs=1)"
                else:
                    how = ("one-command-line wall clock (dsscp on %d threads vs %s "
                           "serial)" % (jobs, c.compiler))
                    who = "dsscp(jobs=%s)" % jobs
                lines.append("RATIO %-14s / %-5s %-12s %5.2fx   -- %s"
                             % (who, c.compiler, label, d.median / c.median, how))
    if undrawn:
        lines.append("RATIO not derived for: %s -- see the reference table above."
                     % ", ".join(undrawn))
    return lines


def slope_lines(results):
    """Least squares over the multi-TU ladder: ms = floor + per_tu * TUs.

    This is the decomposition a single-subject benchmark cannot produce, and it
    is the one that says whether a disadvantage is startup or throughput."""
    ladder = [(s, cells) for s, cells in results if s.sid.startswith("multi")]
    if len(ladder) < 2:
        return []
    keys = []
    for _, cells in ladder:
        for c in cells:
            k = (c.compiler, c.arm, c.jobs)
            if k not in keys:
                keys.append(k)
    out = ["fit of median wall clock against TU count over the multi ladder (%s):"
           % ", ".join("%d TU" % s.tus for s, _ in ladder),
           "  %-8s %-9s %5s %12s %12s  %s"
           % ("compiler", "arm", "jobs", "floor(ms)", "per-TU(ms)", "points")]
    unfittable = []
    for compiler, arm, jobs in keys:
        pts = []
        for s, cells in ladder:
            c = pick(cells, compiler, arm, jobs)
            if c is not None and c.ok:
                pts.append((float(s.tus), c.median))
        if len(pts) < 2:
            # Named once, below, rather than as a row of dashes per arm: a table
            # whose rows are mostly holes hides the rows that carry a number.
            tag = "%s/%s" % (compiler, arm)
            if tag not in unfittable:
                unfittable.append(tag)
            continue
        n = len(pts)
        sx = sum(p[0] for p in pts)
        sy = sum(p[1] for p in pts)
        sxx = sum(p[0] * p[0] for p in pts)
        sxy = sum(p[0] * p[1] for p in pts)
        denom = n * sxx - sx * sx
        b = (n * sxy - sx * sy) / denom
        a = (sy - b * sx) / n
        out.append("  %-8s %-9s %5s %12.1f %12.1f  %d"
                   % (compiler, arm, "-" if jobs is None else jobs, a, b, n))
    if unfittable:
        out.append("not fitted (fewer than 2 measured points): %s"
                   % ", ".join(unfittable))
    out.append("floor is the per-invocation cost paid before any translation unit is")
    out.append("read; per-TU is what each additional unit adds. A large floor is a")
    out.append("startup problem, a large slope is a throughput problem, and they have")
    out.append("different fixes -- which is why the ladder exists.")
    return out


# ── arguments ───────────────────────────────────────────────────────────────
def parse_args(argv):
    a = {
        "dsscp": None, "target": None, "language": "c", "runs": 10, "warmup": 2,
        "jobs": 6, "subjects": None, "multi_tus": [1, 4, 17], "extra": [],
        "out": None, "label": None, "cc": {}, "require_build_type": "Release",
        "json": None,
    }
    i = 0
    while i < len(argv):
        t = argv[i]

        def val():
            if i + 1 >= len(argv):
                die("%s wants a value" % t)
            return argv[i + 1]

        if t in ("-h", "--help"):
            sys.stdout.write(__doc__)
            raise SystemExit(0)
        elif t == "--dsscp":
            a["dsscp"] = val(); i += 2
        elif t == "--target":
            a["target"] = val(); i += 2
        elif t == "--language":
            a["language"] = val(); i += 2
        elif t == "--runs":
            a["runs"] = int(val()); i += 2
        elif t == "--warmup":
            a["warmup"] = int(val()); i += 2
        elif t == "--jobs":
            a["jobs"] = int(val()); i += 2
        elif t == "--subjects":
            a["subjects"] = [x.strip() for x in val().split(",") if x.strip()]; i += 2
        elif t == "--multi-tus":
            a["multi_tus"] = [int(x) for x in val().replace(" ", "").split(",") if x]; i += 2
        elif t == "--extra-subject":
            spec = val()
            if "=" not in spec:
                die("--extra-subject wants ID=PATH[,PATH...], got %r" % spec)
            sid, paths = spec.split("=", 1)
            a["extra"].append((sid, [native(p) for p in paths.split(",") if p]))
            i += 2
        elif t == "--out":
            a["out"] = val(); i += 2
        elif t == "--label":
            a["label"] = val(); i += 2
        elif t == "--cc":
            spec = val()
            if "=" not in spec:
                die("--cc wants NAME=PATH, got %r" % spec)
            k, v = spec.split("=", 1)
            a["cc"][k] = v
            i += 2
        elif t == "--require-build-type":
            a["require_build_type"] = val(); i += 2
        elif t == "--json":
            a["json"] = val(); i += 2
        else:
            # ★★★ NEVER A SHRUG. Ignoring an unknown flag is how a run ends up
            # not measuring what its command line said it measured.
            die("unknown argument %r. See --help." % t)
    if a["runs"] < 1:
        die("--runs must be at least 1; a single reading is already thin")
    if a["warmup"] < 0:
        die("--warmup cannot be negative")
    if any(n < 1 for n in a["multi_tus"]):
        die("--multi-tus wants TU counts of 1 or more")
    return a


def main(argv):
    args = parse_args(argv)
    repo = repo_root()

    if not args["dsscp"]:
        die("--dsscp is required. This tool will not go looking for a compiler: "
            "which binary produced a number is half of what the number means.")
    dsscp = native(args["dsscp"])
    if not os.path.isfile(dsscp):
        die("--dsscp %s does not exist (resolved to %s). Nothing is measured over "
            "a compiler that is not there." % (args["dsscp"], dsscp))

    read_build_type = load_build_type_reader(repo)
    btype, how = read_build_type(dsscp)
    if args["require_build_type"] and \
            btype.lower() != args["require_build_type"].lower():
        die("dsscp at %s is a %r build, not %r.\n    read from: %s\n"
            "    A debug-build timing published beside release ones is not a "
            "comparison. Pass --require-build-type to state a different intent."
            % (dsscp, btype, args["require_build_type"], how))

    osname, mach = host_key()
    target = args["target"] or HOST_TARGETS.get((osname, mach))
    if not target:
        die("no default target for host (%s, %s). Pass --target <arch>:<format> "
            "-- this tool refuses to guess where to aim the compiler." % (osname, mach))

    label = args["label"] or ("%s-%s" % (osname, mach))
    out = native(args["out"] or os.path.join(repo, "build", "compile-bench", label))
    if not os.path.isdir(out):
        os.makedirs(out)

    refs = probe_references(args["cc"])
    subjects = build_subjects(repo, out, args["subjects"], args["multi_tus"],
                              args["extra"])

    meta = {
        "label": label,
        "when": time.strftime("%Y-%m-%d %H:%M:%S"),
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "cores": os.cpu_count(),
        "dsscp": dsscp,
        "dsscp_md5": md5_of(dsscp),
        "build_type": btype,
        "build_type_how": how,
        "target": target,
        "target_explicit": bool(args["target"]),
        "out": out,
    }

    # ★★★ NAME THE CONFIG TREE INSTEAD OF INHERITING IT. dsscp's shipped-config
    # lookup prefers $DSS_CONFIG_ROOT and otherwise WALKS UP FROM THE CWD, and
    # each cell runs in its own scratch directory. Leaving that to the walk makes
    # WHICH TREE WAS READ a property of where the scratch happened to land -- and
    # profile-compile has already measured what that costs when the answer is a
    # tree on a 9p mount: preprocess-splice 1m00s against 9.8s, an artefact of
    # the READER rather than a property of anything. Stated, not inherited.
    dss_env = dict(os.environ)
    dss_env["DSS_CONFIG_ROOT"] = repo
    meta["config_root"] = repo

    meta["calib_before"] = calibrate()

    results = []
    for s in subjects:
        cells = []
        work = os.path.join(out, "work", s.sid)
        # dsscp: each vendor's default, then each vendor's own release switch.
        # The jobs axis exists only where there is more than one TU to spread;
        # on a single-TU subject `--jobs` is not passed at all, so the row is the
        # plain command line a user would type.
        job_arms = [1, args["jobs"]] if (s.parallel_arms and args["jobs"] != 1) else \
                   ([args["jobs"]] if s.parallel_arms else [None])
        for arm, release in (("default", False), ("release", True)):
            for jobs in job_arms:
                cell = Cell("dsscp", arm, jobs)
                tag = "dsscp-%s-j%s" % (arm, "auto" if jobs is None else jobs)
                od = os.path.join(work, tag)
                image = os.path.join(od, "image")
                argv_c = dsscp_command(dsscp, s.sources, image, target,
                                       args["language"], release, jobs)
                sys.stderr.write("  ... %s / %s\n" % (s.sid, tag))
                measure(cell, argv_c, od, image, os.path.join(work, tag + ".log"),
                        args["runs"], args["warmup"], dss_env)
                cells.append(cell)
        for r in refs:
            for arm, release in (("default", False), ("release", True)):
                cell = Cell(r.name, arm, None)
                if not r.present:
                    cell.status = r.state
                    cell.detail = r.absent_reason
                    cells.append(cell)
                    continue
                if release and r.opt_flag is None:
                    cell.status = "n/a"
                    cell.detail = r.note or "this vendor has no release switch"
                    cells.append(cell)
                    continue
                od = os.path.join(work, "%s-%s" % (r.name, arm))
                sys.stderr.write("  ... %s / %s %s\n" % (s.sid, r.name, arm))
                measure(cell, r.command(s.sources, od, release), od,
                        os.path.join(od, "cb_ref.exe"),
                        os.path.join(work, "%s-%s.log" % (r.name, arm)),
                        args["runs"], args["warmup"])
                cells.append(cell)
        results.append((s, cells))

    meta["calib_after"] = calibrate()

    print_report(sys.stdout, meta, refs, results, args)

    if args["json"]:
        import json
        blob = {
            "meta": meta,
            "references": [{"name": r.name, "present": r.present,
                            "state": r.state, "version": r.version,
                            "absent_reason": r.absent_reason,
                            "path": r.path} for r in refs],
            "subjects": [{
                "id": s.sid, "tus": s.tus, "bytes": s.bytes, "lines": s.lines,
                "md5": s.md5, "provenance": s.provenance,
                "cells": [{"compiler": c.compiler, "arm": c.arm, "jobs": c.jobs,
                           "status": c.status, "detail": c.detail,
                           "samples_ms": c.samples,
                           "median_ms": c.median if c.ok else None,
                           "min_ms": c.lo if c.ok else None} for c in cells],
            } for s, cells in results],
        }
        with io.open(args["json"], "w", encoding="utf-8") as f:
            f.write(json.dumps(blob, indent=2, sort_keys=True))
        sys.stderr.write("%s: wrote %s\n" % (TOOL, args["json"]))

    # A cell that FAILED or produced NO ARTIFACT is a broken reading, not a slow
    # one, and the exit code says so -- a caller scripting a before/after diff
    # must not treat a table full of holes as a successful measurement. ABSENT
    # and n/a are FACTS ABOUT THE HOST, not failures, and do not red.
    broken = [(s.sid, c) for s, cells in results for c in cells
              if c.status in ("FAILED", "NO-ARTIFACT")]
    if broken:
        sys.stderr.write("%s: %d cell(s) did not measure: %s\n"
                         % (TOOL, len(broken),
                            ", ".join("%s/%s/%s" % (sid, c.compiler, c.arm)
                                      for sid, c in broken)))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
