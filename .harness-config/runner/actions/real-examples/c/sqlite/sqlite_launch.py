#!/usr/bin/env python3
"""sqlite_launch.py -- how one leg's fixture is handed to its launcher, and how one segment runs.

FOUR NAMESPACES, each a question the leg's LAUNCHER declaration answers (legs.json `launchers`,
resolved per host by `harness_legs.py`) and this module only ASKS -- it knows nothing about
`wslpath`, `/mnt/c`, `--cd`, `/tmp`, `WSLENV` or which host it is on:

  * PATHS (`pathTranslation`): an argument spelled the way the launcher reads it. An
    untranslated path is not rejected as a bad path -- the callee opens a RELATIVE file of that
    name, misses, and the run reads as a broken binary. So every path is translated at
    construction (`launch_path`) AND the whole argv is asserted at the one spawn point
    (`assert_translated`).
  * ENVIRONMENT (`envTransfer`): a launcher in another OS namespace does not inherit this
    driver's environment. ✔MEASURED 2026-08-04: a wsl.exe-launched fixture saw
    SQLITE_TEST_PATTERN_LIST as EMPTY and the resume engine re-ran the corpus from the start.
    ★★ ONLY A VARIABLE THAT IS SET IN THE CHILD'S ENVIRONMENT IS CARRIED: naming an unset one in
    WSLENV materialises it EMPTY-BUT-EXISTING, `info exists ::env(SQLITE_TEST_PATTERN_LIST)` is
    then true, the tier selects ZERO files and the run printed green (MEASURED). A value that is
    a PATH crosses through `--forward-path` (translated), a value already in the launcher's
    namespace through `--forward-declared` (verbatim), anything else through `--forward`.
  * FILESYSTEM (`runFilesystem`): where the launched fixture writes its databases. /mnt/c is
    9p/DrvFs with no `metadata` option, so `chmod 644` reads back 777 and 60 permission tests
    failed identically under GCC; the resolver names a run directory on the launcher's own
    filesystem and the argv prefixes that clear, create and populate it.
  * THE LOADER'S SEARCH VARIABLE: a property of the TARGET's loader (ld.so reads
    LD_LIBRARY_PATH, dyld DYLD_LIBRARY_PATH and ignores the ELF one), built element by element
    through the same path door, joined by the TARGET's separator.

Plus the segment RUNNER (stall/cap bounds on the log's byte size, tree kill, sweep, settle) and
the execution-evidence MONITORS (a clock row excuses a failure only on evidence from that
failure's own execution). The union of `build-and-test.sh` and `build-and-test.ps1` (lane mig,
part 4, 2026-09-21: no `.sh`/`.ps1` under the actions directory).

Nothing here runs at import.
"""
from __future__ import annotations

import collections
import json
import os
import subprocess
import sys
import time

import sqlite_common as C

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass
sys.dont_write_bytecode = True

# ── the three forward groups (a name in no group is never carried) ─────────────────────
# NAMESPACE-NEUTRAL values: a list of file names, a list of test names.
FORWARD_PLAIN = ("SQLITE_TEST_PATTERN_LIST", "QUICKTEST_OMIT")
# Values that are PATHS in THIS driver's namespace (translated as they cross).
FORWARD_PATHS = ("TCL_LIBRARY",)

# tester.tcl's cmdlinearg(testdir) default: the fixture `file mkdir`s this subdirectory of its
# CWD and cd's into it before any .test body runs, so a test's relative `./libtestloadext.so`
# (`./testloadext.dll` on a Windows Tcl) resolves THERE. Named once.
SQLITE_TESTDIR_SUBDIR = "testdir"

# The keys this driver reads off `--run-dir-plan`; a missing one is a contract break.
RUN_DIR_KEYS = ("runFilesystem", "launcherPath", "launcher", "rmTreeArgv", "mkdirArgv", "copyArgv")


def _verb(value, default):
    v = (value or "").strip()
    return v if v else default


def path_verb(leg):
    return _verb(leg.run.get("pathTranslation"), "none")


def env_verb(leg):
    return _verb(leg.run.get("envTransfer"), "inherit")


# ── PATHS ─────────────────────────────────────────────────────────────────────────────

def launch_path(resolver, verb, path):
    """`path` spelled in the launcher's namespace (identity for `none`)."""
    if not verb or verb == "none":
        return str(path)
    r = resolver.call(["--path-translation", verb, "--translate-path", str(path)])
    lines = [ln.strip() for ln in r.out.splitlines() if ln.strip()]
    if r.rc != 0 or not lines:
        C.die("could not translate '%s' into the launcher's path namespace (pathTranslation "
              "'%s', rc=%d):\n      %s\n      The leg's DECLARED launcher cannot be handed a "
              "path at all, so this run stops here rather than spawn it with one its callee "
              "would silently read as a relative filename."
              % (path, verb, r.rc, (r.err or r.out).strip() or "<no diagnostic>"))
    return lines[0]


def assert_translated(resolver, verb, args):
    """The net under "translate at construction": EVERY argument, at the one spawn point.

    ★ `--assert-translated=` uses the `=` form deliberately: a real fixture argv carries
    `--start=full:`, which the space form would have the resolver parse as an option."""
    if not verb or verb == "none":
        return
    call = ["--path-translation", verb] + ["--assert-translated=%s" % a for a in args]
    r = resolver.call(call)
    if r.rc != 0:
        C.die("REFUSING to spawn the leg's launcher — an argument is still in THIS driver's path "
              "namespace, not the launcher's:\n      %s"
              % ((r.err or r.out).strip() or "<no diagnostic>"))


# ── ENVIRONMENT ───────────────────────────────────────────────────────────────────────

def carrier_name(resolver, verb):
    """The carrier variable of an envTransfer verb (`""` for `inherit`), from the resolver."""
    if not verb or verb == "inherit":
        return ""
    r = resolver.call(["--env-transfers"])
    if r.rc != 0:
        C.die("could not read the environment-transfer vocabulary (rc=%d):\n      %s"
              % (r.rc, (r.err or r.out).strip() or "<no diagnostic>"))
    carrier = ""
    for line in r.out.splitlines():
        parts = line.rstrip("\r").split("\t")
        if len(parts) >= 2 and parts[0].strip() == verb:
            carrier = parts[1].strip()
    if not carrier:
        C.die("envTransfer '%s' declares no carrier variable, yet it is not 'inherit'.\n"
              "      The resolver and this driver disagree about the vocabulary." % verb)
    return carrier


def carrier_assignments(resolver, verb, pverb, env, plain, paths, declared, current):
    """The `(name, value)` assignments that carry the SET names of `env` across `verb`.

    THE FILTER is the load-bearing line: a name is offered only when `env` holds a NON-EMPTY
    value for it; nothing set means nothing carried (not an empty carrier)."""
    if not verb or verb == "inherit":
        return []
    call = ["--env-transfer", verb, "--path-translation", pverb or "none",
            "--carrier-current", current or ""]
    carried = False
    for n in plain:
        if n and env.get(n):
            call += ["--forward", n]
            carried = True
    for n in paths:
        v = env.get(n) if n else None
        if v:
            # ONE token with the `=` form: a path may contain spaces, and the resolver splits
            # on the FIRST `=` only.
            call.append("--forward-path=%s=%s" % (n, v))
            carried = True
    for n in declared:
        if n and env.get(n):
            call += ["--forward-declared", n]
            carried = True
    if not carried:
        return []
    r = resolver.call(call)
    if r.rc != 0:
        C.die("could not resolve the launcher's environment transfer (envTransfer '%s', rc=%d):\n"
              "      %s\n      Without it the launched fixture runs with an EMPTY run environment, "
              "which does not fail — it silently changes what the corpus does."
              % (verb, r.rc, (r.err or r.out).strip() or "<no diagnostic>"))
    out = []
    for line in r.out.splitlines():
        line = line.rstrip("\r")
        if not line.strip():
            continue
        name, sep, value = line.partition("=")
        if not sep or not name.strip():
            C.die("the environment transfer (envTransfer '%s') printed a line that is not an "
                  "assignment: %r" % (verb, line))
        out.append((name.strip(), value))
    return out


# ── THE LOADER'S SEARCH VARIABLE ──────────────────────────────────────────────────────

def target_os(leg):
    """The leg's TARGET OS, from the resolved plan, CROSS-CHECKED against its object format
    (`<container><bits>-<arch>-<os>-<kind>`: the OS is the second-to-last token)."""
    os_ = (leg.build.get("configStageKey") or "").strip()
    parts = (leg.format or "").split("-")
    fmt_os = parts[-2] if len(parts) >= 3 else ""
    if not os_:
        C.die("[%s] the resolved plan carries no target OS (build.configStageKey is empty). The "
              "runtime loader's search variable is a property of the TARGET; choosing one "
              "without knowing the target is how LD_LIBRARY_PATH came to be exported for a "
              "Darwin leg that cannot read it." % leg.label)
    if os_ != fmt_os:
        C.die("[%s] the resolved plan disagrees with itself about this leg's target OS: "
              "configStageKey says '%s', the object format '%s' says '%s'. One of them decides "
              "which loader variable this leg's libraries are exported under."
              % (leg.label, os_, leg.format, fmt_os))
    return os_


def loader_spec(leg):
    """`(name, separator)` of the variable the TARGET's loader searches, or None when the leg
    DECLARES none. Keyed on (target OS, run mode):
      linux   -> LD_LIBRARY_PATH ':'      darwin -> DYLD_LIBRARY_PATH ':'
      windows + native   -> PATH ';'   (the Windows loader reads PATH; the .ps1's native pe64)
      windows + launched -> none       (the launcher is Wine: the loader searches the
                                        executable's own directory, where Step 7 staged the DLLs,
                                        and a PATH mutated with staged dirs or a `;` would break
                                        Wine's OWN lookup before the fixture started -- the .sh)
    A target OS without a declared answer is REFUSED: defaulting to one spelling is the defect
    this function exists to end."""
    os_ = target_os(leg)
    if os_ == "linux":
        return ("LD_LIBRARY_PATH", ":")
    if os_ == "darwin":
        return ("DYLD_LIBRARY_PATH", ":")
    if os_ == "windows":
        return ("PATH", ";") if leg.run_mode == "native" else None
    C.die("[%s] target OS '%s' has no declared runtime-loader search variable in this driver. "
          "Known: linux (LD_LIBRARY_PATH) | darwin (DYLD_LIBRARY_PATH) | windows (PATH when run "
          "natively, none under a launcher). A new target OS must DECLARE its answer."
          % (leg.label, os_))


def stages_libraries(leg):
    """Only a leg whose libraries the harness STAGED gets a loader path: a `host-system` leg's
    libraries are, by definition, already where this machine's loader looks."""
    lib = leg.build.get("libraries") or {}
    return (lib.get("provider") or "") != "host-system"


def loader_search_path(resolver, leg, dirs, env, log=C.LOG):
    """`(name, value)` for the leg's loader variable, or None. Every element goes through the
    leg's DECLARED path translation; the value already in `env` is merged ONLY when the launcher
    shares this driver's namespace (`pathTranslation: none`) -- otherwise it would reach the
    target's loader as directories it cannot open, so it is DROPPED and said out loud."""
    spec = loader_spec(leg)
    if spec is None:
        return None
    name, sep = spec
    verb = path_verb(leg)
    parts = []
    for d in dirs:
        if not d:
            continue
        t = launch_path(resolver, verb, d)
        if t not in parts:
            parts.append(t)
    cur = env.get(name, "")
    if cur:
        if verb == "none":
            parts.append(cur)
        else:
            log.info("[%s] %s is SET in this driver's own environment and is NOT carried into the "
                     "launcher's: its value is spelled in THIS namespace and the launcher declares "
                     "pathTranslation '%s', so it would reach the target's loader as directories it "
                     "cannot open. Only this leg's staged library directories cross."
                     % (leg.label, name, verb))
    return (name, sep.join(parts)) if parts else None


def declared_env(leg):
    """The launcher's DECLARED environment (legs.json `launchers[].env`, e.g. QEMU_LD_PREFIX)."""
    e = leg.run.get("env") or {}
    return {str(k): str(v) for k, v in e.items() if k}


def leg_launch_env(resolver, leg, base, loader_dirs, tcl_library=None, extra=None, log=C.LOG):
    """The child environment of ONE spawn of this leg's fixture or CLI -- the single place the
    decision exists (✔MEASURED 2026-08-07: the smoke step once built its own copy, forgot
    QEMU_LD_PREFIX, and failed every assertion with `Could not open '/lib/ld-linux-aarch64.so.1'`).

    `base` is copied, `extra` applied (None/"" DELETES a name), then the loader variable (staged
    providers only), the launcher's declared variables, TCL_LIBRARY (the acquired script library,
    when the leg has one), and LAST the carrier, which reads everything set above."""
    env = dict(base)
    for k, v in (extra or {}).items():
        if v is None or v == "":
            env.pop(k, None)
        else:
            env[k] = str(v)
    loader_name = ""
    if stages_libraries(leg):
        ls = loader_search_path(resolver, leg, loader_dirs, env, log)
        if ls is not None:
            loader_name, value = ls
            env[loader_name] = value
    declared = declared_env(leg)
    env.update(declared)
    if tcl_library:
        env["TCL_LIBRARY"] = tcl_library
    verb = env_verb(leg)
    carrier = carrier_name(resolver, verb)
    if carrier:
        names = list(declared) + ([loader_name] if loader_name else [])
        for n, v in carrier_assignments(resolver, verb, path_verb(leg), env, FORWARD_PLAIN,
                                        FORWARD_PATHS, names, env.get(carrier, "")):
            env[n] = v
    return env


# ── FILESYSTEM ────────────────────────────────────────────────────────────────────────

def run_dir_plan(resolver, leg, driver_run_dir):
    """This leg's RUN DIRECTORY plan (`--run-dir-plan`): refused unless every read key is there."""
    r = resolver.call(["--run-dir-plan", leg.label] + resolver.host_args +
                      ["--driver-run-dir", str(driver_run_dir), "--format", "json"])
    if r.rc != 0 or not r.out.strip():
        C.die("[%s] could not resolve this leg's RUN DIRECTORY (harness_legs.py --run-dir-plan, "
              "rc=%d):\n      %s\n      Which filesystem a launched leg runs on is DECLARED "
              "(legs.json `launchers[].runFilesystem`), never assumed — and the assumption is what "
              "put a Linux sqlite corpus onto DrvFs."
              % (leg.label, r.rc, (r.err or r.out).strip() or "<no diagnostic>"))
    try:
        plan = json.loads(r.out)
    except ValueError:
        C.die("[%s] harness_legs.py --run-dir-plan exited 0 but did not print the JSON this driver "
              "reads. Output was:\n%s" % (leg.label, C.first_lines(r.out, 20)))
    missing = [k for k in RUN_DIR_KEYS if k not in plan]
    if missing:
        C.die("the run-dir plan does not carry the field(s) this driver reads (%s; have: %s). That "
              "is a contract break between harness_legs.py and this driver, not a property of this "
              "host." % (", ".join(missing), ", ".join(sorted(plan))))
    return plan


def run_dir_corroboration(resolver, leg, driver_run_dir, supplied, supplied_abort):
    """★★★ A confound row may be corroborated against THIS LEG's own run directory: the driver
    hands over the supply it holds and takes back what survived (`--corroborate-run-dir`)."""
    argv = ["--corroborate-run-dir", leg.label] + resolver.host_args + [
        "--driver-run-dir", str(driver_run_dir), "--format", "json"]
    for p in supplied:
        if p:
            argv += ["--supplied", p]
    for p in supplied_abort:
        if p:
            argv += ["--supplied-abort", p]
    r = resolver.call(argv)
    if r.rc != 0 or not r.out.strip():
        C.die("[%s] could not CORROBORATE this leg's confound rows against its own run directory "
              "(harness_legs.py --corroborate-run-dir, rc=%d):\n      %s\n      A row declaring "
              "`requiresRunDirectory` is honoured ONLY where THIS RUN measured the precondition on "
              "THIS LEG'S own run directory. Continuing without the measurement would either excuse "
              "a failure on evidence nobody gathered, or withhold an earned excusal and report it as "
              "a compiler regression." % (leg.label, r.rc, (r.err or r.out).strip() or "<no diagnostic>"))
    try:
        return json.loads(r.out)
    except ValueError:
        C.die("[%s] harness_legs.py --corroborate-run-dir exited 0 but did not print the JSON this "
              "driver reads. Output was:\n%s" % (leg.label, C.first_lines(r.out, 20)))


def run_dir_argv(prefix, args, what, timeout=900):
    """Run one of the resolver's argv PREFIXES + `args` -> (ok, why). An EMPTY prefix means the
    launcher shares this driver's filesystem and the caller does the operation natively, so it
    is a real answer and spawns NOTHING (whatever `args` holds).

    ★ IT RETURNS A VERDICT, never raises: a run directory that could not be prepared costs THAT
    leg its corpus and must not delete the other legs' evidence. ⛔ And there is no fallback to
    this driver's own directory."""
    prefix = [str(p) for p in (prefix or []) if p]
    if not prefix:
        return True, ""
    argv = prefix + [str(a) for a in args if a]
    r = C.capture(argv, timeout=timeout, merge=True)
    if r.rc == 0:
        return True, ""
    return False, ("could not %s in the launcher's own filesystem — `%s` exited %d: %s"
                   % (what, " ".join(argv), r.rc, " | ".join(r.out.strip().splitlines()) or
                      "<no diagnostic>"))


# ── THE REGISTRY, AT THE POINT OF FAILURE ─────────────────────────────────────────────

def registry_controls(resolver, registry, leg_label, failing, log=C.LOG, limit=12):
    """A POINTER, never a verdict, and FAIL-SOFT by construction: this runs on a failure path and
    must never become one (every error is swallowed; a run that already failed keeps its
    report). Bounded to `limit` names -- the resolver says how many rows it held back."""
    try:
        call = ["--registry-controls", registry]
        if leg_label:
            call += ["--for-leg", leg_label]
        n = 0
        for t in failing:
            if not t:
                continue
            if n >= limit:
                break
            call += ["--for-test", t]
            n += 1
        r = resolver.call(call, catalogue=False, timeout=300)
        out = (r.out or "").strip() or (r.err or "").strip()
        if not out:
            return
        log.info("      ── registry rows naming this leg / these tests (a POINTER, not a verdict — "
                 "read before commissioning an experiment):")
        for line in out.splitlines():
            log.info("      %s" % line.rstrip("\r"))
    except Exception as exc:  # noqa: BLE001 -- fail-soft is the contract
        log.info("      (registry lookup unavailable: %s)" % exc)


# ── THE SEGMENT RUNNER ────────────────────────────────────────────────────────────────

SegmentResult = collections.namedtuple("SegmentResult", ["rc", "kill_reason", "seconds"])


def launch_argv(launcher, launch_bin, args):
    """The child's argv: the (run-directory-spliced) launcher, the fixture spelled in the
    launcher's namespace, the segment's arguments."""
    return [str(a) for a in (launcher or []) if a] + [str(launch_bin)] + [str(a) for a in args]


def _terminate(proc):
    try:
        proc.terminate()
    except OSError:
        pass


def run_segment(argv, cwd, env, log_path, stall_s, cap_s, settle_s, kill_tree, sweep,
                any_left, poll_s=5.0, clock=time.monotonic, sleep=time.sleep):
    """Run ONE fixture segment: stdin at EOF, stdout+stderr MERGED into `log_path` (the order the
    fixture wrote them -- the Tcl traceback the parser reads is part of that stream), killed when
    the log stops GROWING for `stall_s` seconds or the segment exceeds `cap_s` (0 disables each).

    On a kill: TERM, 2 s, the whole tree (`kill_tree(proc)`), a sweep by PATH (`sweep()` -- a
    launched fixture can be a process the launcher's own tree does not contain), then SETTLE up
    to `settle_s` while `any_left()` says a match remains, plus 2 s. ✔MEASURED: without the settle
    the next segment died at tester.tcl's `reset_db` with `error deleting "test.db": permission
    denied` -- the killed fixture still held the handle.

    stdin at EOF is hardening, not a cure: the "aborted fixture blocks on the Tcl REPL" theory was
    TESTED and REFUTED (an aborted fixture exits rc=1 with an open, never-written stdin)."""
    t0 = clock()
    kill_reason = ""
    with open(log_path, "wb") as logfh:
        popen_kw = {}
        if os.name == "posix":
            popen_kw["start_new_session"] = True
        else:
            popen_kw["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        proc = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                stdout=logfh, stderr=subprocess.STDOUT, **popen_kw)
        last_len = -1
        last_grow = clock()
        while proc.poll() is None:
            sleep(poll_s)
            if proc.poll() is not None:
                break
            try:
                size = os.path.getsize(log_path)
            except OSError:
                size = last_len
            now = clock()
            if size != last_len:
                last_len, last_grow = size, now
            elif stall_s > 0 and now - last_grow >= stall_s:
                kill_reason = "produced no output for %ds (DSS_SEGMENT_STALL)" % stall_s
                break
            if cap_s > 0 and now - t0 >= cap_s:
                kill_reason = "exceeded the absolute cap of %ds (DSS_SEGMENT_TIMEOUT)" % cap_s
                break
        if kill_reason:
            _terminate(proc)
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
            if proc.poll() is None:
                kill_tree(proc)
            try:
                sweep()
            except C.HarnessDie:
                raise
            except Exception as exc:  # noqa: BLE001 -- a sweep failure is reported, not fatal here
                C.LOG.warn("the post-kill sweep could not run: %s" % exc)
            waited = 0
            while waited < settle_s and any_left():
                sleep(1)
                waited += 1
            sleep(2)
        try:
            rc = proc.wait(timeout=60 if kill_reason else None)
        except subprocess.TimeoutExpired:
            kill_tree(proc)
            rc = proc.wait()
    return SegmentResult(rc, kill_reason, clock() - t0)


# ── EXECUTION EVIDENCE ────────────────────────────────────────────────────────────────

Monitor = collections.namedtuple("Monitor", ["probe", "proc", "stop_file", "stop_within",
                                             "timeline"])

ARMED_MARK = '"kind": "armed"'


def _truncate(path):
    with open(path, "wb"):
        pass


def _remove(path):
    try:
        os.remove(path)
    except FileNotFoundError:
        pass
    except OSError:
        pass


def evidence_start(resolver, leg, log_path, cap_s, override_active, log=C.LOG,
                   sleep=time.sleep, clock=time.monotonic):
    """Arm this segment's execution monitors -> [Monitor]. The plan only ARMS a clock row
    (`confoundsByEvidence`); around EVERY segment a monitor planned by the resolver runs in the
    FIXTURE's own kernel and records each clock step with the log's size at that instant.
    ★ EVERY FAILURE PATH UN-EXCUSES AND SAYS SO and none stops the run: a monitor that cannot be
    planned, cannot arm or cannot stop leaves that segment's clock failures GENUINE."""
    if override_active or not [p for p in (leg.d.get("confoundsByEvidence") or []) if p]:
        return []
    probes = [p for p in (leg.d.get("executionEvidence") or []) if p]
    if not probes:
        log.warn("[%s] armed confound rows but NO execution-evidence probe in the plan — a "
                 "transport defect; every failure an armed row matches stays GENUINE" % leg.label)
        return []
    # ★ EMPTY, BEFORE THE MONITOR ARMS: the attributor REFUSES a timeline armed on a non-empty
    # log, because a stale file's offsets are not this segment's.
    _truncate(log_path)
    mons = []
    for probe in probes:
        r = resolver.call(["--execution-monitor-argv", "--run-filesystem",
                           leg.run.get("runFilesystem") or "", "--evidence-probe", probe,
                           "--watch-log", str(log_path), "--segment-cap-seconds", str(cap_s),
                           "--format", "json"])
        plan = None
        if r.rc == 0:
            try:
                plan = json.loads(r.out)
            except ValueError:
                plan = None
        if not isinstance(plan, dict) or not plan.get("argv") or not plan.get("timeline") \
                or not plan.get("stopFile"):
            log.warn("[%s] NO '%s' execution monitor for this segment (harness_legs.py "
                     "--execution-monitor-argv, rc=%d): %s"
                     % (leg.label, probe, r.rc, " ".join((r.err or r.out).split()) or
                        "<no diagnostic>"))
            log.warn("      every failure an armed '%s' row would match in this segment stays "
                     "GENUINE" % probe)
            continue
        timeline, stop_file = str(plan["timeline"]), str(plan["stopFile"])
        _remove(stop_file)
        _remove(timeline)
        errfh = open(timeline + ".stderr", "wb")
        try:
            popen_kw = {"start_new_session": True} if os.name == "posix" else {}
            proc = subprocess.Popen([str(a) for a in plan["argv"]], stdin=subprocess.DEVNULL,
                                    stdout=errfh, stderr=subprocess.STDOUT, **popen_kw)
        except OSError as exc:
            errfh.close()
            log.warn("[%s] the '%s' execution monitor could not start: %s" % (leg.label, probe, exc))
            log.warn("      every failure an armed '%s' row would match in this segment stays "
                     "GENUINE" % probe)
            continue
        errfh.close()
        arm_s = float(plan.get("armWithinSeconds") or 0)
        deadline = clock() + arm_s
        armed = False
        while clock() < deadline:
            try:
                with open(timeline, "r", encoding="utf-8", errors="replace") as fh:
                    if ARMED_MARK in fh.read():
                        armed = True
                        break
            except OSError:
                pass
            if proc.poll() is not None:
                break
            sleep(0.2)
        if armed:
            mons.append(Monitor(probe, proc, stop_file, float(plan.get("stopWithinSeconds") or 0),
                                timeline))
            log.info("[%s] execution monitor '%s' armed (pid %d) -> %s"
                     % (leg.label, probe, proc.pid, timeline))
        else:
            log.warn("[%s] the '%s' execution monitor did NOT arm within %gs (pid %d; stderr in "
                     "%s.stderr)" % (leg.label, probe, arm_s, proc.pid, timeline))
            log.warn("      every failure an armed '%s' row would match in this segment stays "
                     "GENUINE" % probe)
            _terminate(proc)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
    return mons


def evidence_stop(leg_label, mons, log=C.LOG):
    """Touch every stop file, wait each monitor's declared stop window, kill a laggard (its
    timeline keeps every record it flushed)."""
    for m in mons:
        try:
            _truncate(m.stop_file)
        except OSError as exc:
            log.warn("[%s] could not write the stop file of monitor '%s': %s"
                     % (leg_label, m.probe, exc))
    for m in mons:
        try:
            m.proc.wait(timeout=m.stop_within)
        except subprocess.TimeoutExpired:
            log.warn("[%s] execution monitor '%s' (pid %d) did not stop within %gs of its stop file "
                     "— killed; its timeline keeps every record it flushed"
                     % (leg_label, m.probe, m.proc.pid, m.stop_within))
            _terminate(m.proc)
            try:
                m.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                m.proc.kill()
                m.proc.wait()
        _remove(m.stop_file)


def evidence_attribute(resolver, leg, mode, seg_logs, tier_prefixes, failures, override_active,
                       log=C.LOG):
    """The per-failure clock attribution (`--attribute-unit-failures`) -> the set of names the
    resolver EXCUSED; its account is printed verbatim. A resolver that cannot run leaves every
    failure GENUINE (warned), never excused."""
    if override_active:
        return set()
    evidence = [p for p in (leg.d.get("confoundsByEvidence") or []) if p]
    if not evidence or not failures:
        return set()
    argv = ["--attribute-unit-failures", leg.label, "--leg-mode", mode]
    argv += ["--evidence-pattern=%s" % p for p in evidence]
    argv += ["--segment-log=%s" % p for p in seg_logs]
    argv += ["--tier-prefix=%s" % p for p in tier_prefixes]
    argv += ["--failure=%s" % t for t in failures]
    r = resolver.call(argv)
    if r.rc != 0:
        log.warn("[%s] per-failure clock attribution could NOT run (harness_legs.py "
                 "--attribute-unit-failures, rc=%d): %s"
                 % (leg.label, r.rc, " ".join((r.err or r.out).split()) or "<no diagnostic>"))
        log.warn("      every failure an armed row would match stays GENUINE")
        return set()
    excused = set()
    for line in r.out.splitlines():
        s = line.rstrip("\r")
        if s.startswith("EXCUSED\t"):
            excused.add(s[len("EXCUSED\t"):])
        elif s.startswith("REPORT\t"):
            log.info(s[len("REPORT\t"):])
    return excused
