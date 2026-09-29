#!/usr/bin/env python3
"""sqlite_smoke.py -- Step 7c of the SQLite corpus harness: the sqlite3 CLI SMOKE GATE, per leg.

★ WHY IT EXISTS: the unit corpus runs through `testfixture` (a Tcl interpreter linking the sqlite
LIBRARY) and NEVER executes shell.c, so argv handling, the dot-commands, the `.dump` writer and the
startup version guard are covered by nothing else. The fourteen assertions live in `cli-smoke.py`;
this module feeds it MEASURED facts only:
  * the expectation from the staged `sqlite3.h` the binaries were compiled against;
  * each binary's target read out of its OWN header (`--identify-binary`), never the declaration
    twice -- the gate compares the two and reports a wrong-target build as its own non-verdict;
  * the gcc reference's launcher from the catalogue, keyed on the reference's MEASURED target
    (`--launcher-for-target`), never from this host's identity (that branch is what once ran the
    reference host-native x86_64 against a DSS arm64 run and charged every difference to DSS);
  * the leg's DECLARED run environment through the one shared builder (`leg_launch_env`) -- the
    smoke step once built its own copy, forgot QEMU_LD_PREFIX and "charged" 14 failures to DSS on
    a binary that never launched.
★ EVERY rc THE GATE CAN RETURN HAS ITS OWN ARM (0 PASS, 1 CHARGED TO DSS, 3 NOT DSS, 4 NOT A
VERDICT, 2 HARNESS ARGV DEFECT); the last arm says "unknown rc", never names a culprit.

The union of `build-and-test.sh` and `build-and-test.ps1` Step 7c (lane mig, part 4,
2026-09-21). Nothing runs at import.
"""
from __future__ import annotations

import os
import re
import shlex
import shutil
import sys

import sqlite_common as C

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass
sys.dont_write_bytecode = True

import sqlite_launch as L        # noqa: E402

VERSION_RE = re.compile(r'^#define SQLITE_VERSION\s+"(.+)"')
SOURCE_ID_RE = re.compile(r'^#define SQLITE_SOURCE_ID\s+"(.+)"')


def _field(obj, name):
    return obj.get(name) if isinstance(obj, dict) else getattr(obj, name)


def expectations(header):
    """(version, source id) out of the staged sqlite3.h; refused when either is unreadable -- a
    gate that asserts nothing must never pass quietly."""
    version = source_id = ""
    try:
        with open(header, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if not version:
                    m = VERSION_RE.match(line)
                    if m:
                        version = m.group(1)
                if not source_id:
                    m = SOURCE_ID_RE.match(line)
                    if m:
                        source_id = m.group(1)
    except OSError as exc:
        C.die("the staged sqlite3.h is not readable: %s (%s) — the smoke gate has nothing to compare "
              "--version against." % (header, exc))
    if not version or not source_id:
        C.die("could not read SQLITE_VERSION / SQLITE_SOURCE_ID out of %s.\n      They are what the "
              "smoke gate compares the built CLI's --version against; without them the gate would "
              "be asserting nothing, which must never pass quietly." % header)
    return version, source_id


def identify_binary(resolver, path):
    """-> (triple or "", why). `<arch>\\t<container>\\t<targetOs>` read from the file's own
    header; a failure is NAMED, never defaulted."""
    r = resolver.call(["--identify-binary", path])
    if r.rc != 0:
        return "", "could not identify %s (rc=%d): %s" % (
            path, r.rc, (r.err or r.out).strip() or "<no diagnostic on stderr>")
    lines = [ln.strip() for ln in r.out.splitlines() if ln.strip()]
    if not lines:
        return "", ("harness_legs.py --identify-binary %s exited 0 and printed NOTHING — a contract "
                    "break, not a property of the file." % path)
    return lines[0].replace("\t", ":"), ""


def launcher_for_target(resolver, triple):
    """-> (rc, argv, why): rc 0 with the argv (EMPTY = native here), 3 = this host cannot run
    it, anything else = a malformed triple (OUR defect: it came from --identify-binary)."""
    r = resolver.call(["--launcher-for-target", triple] + resolver.host_args)
    why = " ".join((r.err or "").split())
    if r.rc != 0:
        return r.rc, [], why
    lines = [ln for ln in r.out.splitlines() if ln.strip()]
    try:
        argv = shlex.split(lines[0]) if lines else []
    except ValueError as exc:
        return 2, [], "the launcher argv for %s could not be split (%s): %r" % (triple, exc, lines[0])
    return 0, argv, why


def reference_cli(run):
    """The gcc reference CLI measured ONCE: -> (path for the gate, target, launcher) or
    ("", "", []) when this run has none, cannot identify it, or cannot execute it here."""
    log, st = run.log, run.stage
    host_path = _field(st, "reference_cli")
    gate_path = _field(st, "reference_cli_posix") or host_path
    if not host_path:
        log.warn("no gcc reference CLI (%s) — every smoke failure this run is UNATTRIBUTABLE and is "
                 "charged to DSS by design." % (_field(st, "reference_cli_why") or "none was built"))
        return "", "", []
    triple, why = identify_binary(run.resolver, host_path)
    if not triple:
        log.warn("the gcc reference CLI could not be IDENTIFIED — %s" % why)
        log.warn("      It is DROPPED for this run rather than passed with a guessed target: an "
                 "unattributable")
        log.warn("      smoke failure is an honest outcome, a fabricated control triple is not.")
        return "", "", []
    rc, argv, why = launcher_for_target(run.resolver, triple)
    if rc == 0:
        log.info("reference CLI target: %s (MEASURED from its own header) — %s" % (triple, why))
        if argv:
            log.info("reference CLI launcher: %s  (DECLARED by the catalogue for that target on this "
                     "host, never inferred from the host's identity)" % " ".join(argv))
        return gate_path, triple, argv
    if rc == 3:
        log.warn("this host cannot EXECUTE the gcc reference CLI (%s) — %s" % (triple, why))
        log.warn("      The reference is DROPPED: a control that cannot start would fail all fourteen "
                 "assertions for one")
        log.warn("      reason and EXONERATE every DSS failure on every leg against a binary that never "
                 "executed.")
    else:
        log.warn("harness_legs.py --launcher-for-target '%s' exited %d — %s"
                 % (triple, rc, why or "<no diagnostic>"))
        log.warn("      That triple came from --identify-binary, so a malformed one is OUR defect, not "
                 "this machine's.")
        log.warn("      The reference is DROPPED rather than run with an unknown launcher.")
    return "", "", []


def smoke_argv(resolver, leg, cli_target, version, source_id, smoke_dir, result_json, ref, ref_target,
               ref_launch):
    """The gate's argv: the leg's CLI spelled the way its LAUNCHER reads it, the MEASURED targets
    of subject and reference, and every launcher token in the `--opt=<tok>` form -- a token may
    begin with a dash (`arch -x86_64`), and the space form once killed the pe64 gate before one
    assertion ran. Nothing in it is keyed on this host's identity."""
    argv = C.python_argv(C.CLI_SMOKE,
                         "--cli", L.launch_path(resolver, L.path_verb(leg), leg.cli_bin),
                         "--expect-version", version, "--expect-source-id", source_id,
                         "--leg-spec", leg.spec, "--cli-target", cli_target,
                         "--workdir", smoke_dir, "--label", leg.label, "--json", result_json)
    argv += ["--launcher=%s" % t for t in leg.launcher if t]
    if ref:
        argv += ["--reference", ref, "--reference-target", ref_target]
        argv += ["--reference-launcher=%s" % t for t in ref_launch if t]
    return argv


def _shown_env(env, base, limit=160):
    """What actually differs in the child's environment -- read back, not restated."""
    out = []
    for k in sorted(env):
        if base.get(k) != env[k]:
            v = env[k]
            out.append("%s=%s" % (k, v if len(v) <= limit else v[:limit] + "…"))
    return "  ".join(out) or "<inherited unchanged>"


RC_TABLE = {
    0: None,
    1: ("FAIL — CHARGED TO DSS (a MATCHED gcc control passes the assertions this leg fails); see %s",
        "CLI smoke RED and CHARGED TO DSS — the reference targets this leg's own target, it launched, "
        "and it passes what this binary fails."),
    3: ("FAIL — NOT DSS (the gcc reference fails identically); see %s",
        "CLI smoke RED, but DSS is NOT implicated — the gcc reference fails the same assertions."),
    4: ("FAIL — NOT A VERDICT (unattributable); see %s",
        "CLI smoke RED, but this run is NOT A VERDICT about generated code — the subject never "
        "launched and/or there was no MATCHED control (the reference targets a different arch/format "
        "than this leg). See the result's 'controlState' + 'subjectLaunched'."),
    2: ("FAIL — HARNESS ARGV DEFECT (the gate rejected its own arguments); see %s",
        "CLI smoke could not run: the gate REJECTED THE ARGUMENTS THIS DRIVER PASSED IT. That is our "
        "defect, not the compiler's."),
}


def smoke_rc_verdict(rc, result_json, smoke_log):
    """-> (verdict line, warning or None) for the gate's exit code. rc 2 points at the log (the
    gate never wrote a result); an rc outside the table is a DRIVER defect, never DSS's."""
    if rc == 0:
        return "PASS (14/14)", None
    if rc in RC_TABLE:
        v, w = RC_TABLE[rc]
        return v % (smoke_log if rc == 2 else result_json), w
    return ("FAIL — UNKNOWN rc=%d from the smoke gate; see %s" % (rc, result_json),
            "CLI smoke returned rc=%d, which this driver has no arm for. NOT charged to DSS — an rc the "
            "driver does not understand is a driver defect. Add an arm here." % rc)


def step7c(run):
    log, st = run.log, run.stage
    log.step("7c/9  sqlite3 CLI smoke gate (14 assertions, attributed against gcc)")
    if not os.path.isfile(C.CLI_SMOKE):
        C.die("the CLI smoke gate is missing: %s" % C.CLI_SMOKE)
    header = os.path.join(_field(st, "bld"), "sqlite3.h")
    version, source_id = expectations(header)
    log.info("expecting version '%s' / source id '%s' (from %s)" % (version, source_id, header))
    ref, ref_target, ref_launch = reference_cli(run)
    base = dict(os.environ)
    for leg in run.selected():
        if not leg.cli_bin:
            got = run.artifacts.get(leg.label, "sqlite3")
            verdict, detail = (got[0], got[1]) if got else (
                "<no verdict>", "the CLI build loop never reached this leg")
            leg.smoke_verdict = "not run [%s] — %s" % (verdict, detail)
            continue
        if C.run_is_skipped(leg):
            leg.smoke_verdict = "built, NOT RUN here [%s] — %s" % (leg.run.get("verdict"),
                                                                  leg.run.get("detail"))
            log.warn("[%s] CLI smoke SKIPPED — built at %s but this host cannot execute it: %s"
                     % (leg.label, leg.cli_bin, leg.run.get("detail")))
            continue
        triple, why = identify_binary(run.resolver, leg.cli_bin)
        if not triple:
            run.counts["smoke"] += 1
            leg.smoke_verdict = ("FAIL — the built CLI could not be IDENTIFIED (%s); no smoke verdict "
                                 "was taken" % leg.cli_bin)
            log.warn("[%s] CLI smoke NOT RUN — %s" % (leg.label, why))
            log.warn("      This is RED and it is NOT charged to the compiler: the gate needs the "
                     "subject's MEASURED target and this")
            log.warn("      driver will not fabricate one. Counted as a failure so the run cannot exit "
                     "0 over a leg it never asserted about.")
            continue
        smoke_dir = os.path.join(run.leg_out(leg), "cli-smoke")
        if os.path.lexists(smoke_dir):
            shutil.rmtree(smoke_dir)
        os.makedirs(smoke_dir)
        result_json = os.path.join(smoke_dir, "result.json")
        smoke_log = os.path.join(smoke_dir, "smoke.log")
        argv = smoke_argv(run.resolver, leg, triple, version, source_id, smoke_dir, result_json,
                          ref, ref_target, ref_launch)
        # The CLI links zlib and does not embed Tcl: its loader path is zlib's directory (a leg
        # whose Tcl never resolved still reaches this loop); the leg's run environment is
        # otherwise applied IN FULL, TCL_LIBRARY included.
        loader_dirs = [os.path.dirname(leg.z_lib_any)] if leg.z_lib_any else []
        env = L.leg_launch_env(run.resolver, leg, base, loader_dirs,
                               tcl_library=leg.tcl_script_dir or None, log=log)
        log.info("[%s] launcher run environment: %s" % (leg.label, _shown_env(env, base)))
        env = C.child_env(None, base=env, python=True)
        r = C.capture(argv, env_=env, merge=True)
        with open(smoke_log, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(r.out)
        for line in r.out.splitlines():
            log.info("      %s" % line)
        verdict, warning = smoke_rc_verdict(r.rc, result_json, smoke_log)
        leg.smoke_verdict = verdict
        if r.rc == 0:
            log.ok("[%s] CLI smoke: 14/14" % leg.label)
        else:
            run.counts["smoke"] += 1
            log.warn("[%s] %s" % (leg.label, warning))
