#!/usr/bin/env python3
"""build_and_test.py -- the SQLite UNIT-CORPUS harness for DSS Code Prime, full source, no amalgamation.

ONE command proves dsscp builds SQLite from its REAL sources into the Tcl `testfixture` and the
`sqlite3` CLI, and runs SQLite's own `.test` corpus green, for EVERY leg `legs.json` declares, on
whatever host runs it -- Linux, macOS, an arm64 VPS or Windows (the POSIX half then runs inside
WSL through `wsl.exe -e`). It replaced `build-and-test.sh` and its Windows twin `build-and-test.ps1`
on 2026-09-21 (lane mig, part 4: the operator's order that no `.sh`/`.ps1` lives under the actions
directory), and carries the UNION of both drivers' checks.

★★ TARGET-KEYED, NEVER HOST-KEYED. Which legs exist is declared, host-free, in `legs.json` and
resolved by `harness_legs.py` -- the SAME legs on every host, every one of them BUILT. The only
host question is "can this host EXECUTE this leg", answered by the plan's `run.mode` (a launcher
is qemu for a cross-arch host, Wine for a cross-OS one, `wsl.exe` for a Linux leg on Windows),
never by an OS test in leg selection. The one host switch left is WHERE the POSIX toolchain runs
(in this process on a POSIX host, inside WSL on a Windows one).

The pipeline (each step in its own module beside this file):
  0  refuse to start when the harness's own logic is broken (the Python suites, the resolver's
     self-test and lint);
  1  identify the host, resolve the leg plan, apply DSS_LEGS / DSS_RUN_FIDELITY, check every
     launcher's DECLARED prerequisites (`sqlite_common`, here);
  2  VERIFY the dsscp checkout (never switched or pulled), take the RUN LOCK;
  3–4 fetch sqlite, configure it, derive the full-source recipes, build the reference oracles,
     stage sources and headers (`sqlite_stage`, in WSL on Windows);
  5  take the dsscp the run was GIVEN (--dss / DSS_BIN; REQUIRED, and a run naming none is refused
     before Step 0), through the ONE Release gate, and prove it current (`sqlite_compiler`);
  6  stage the per-target headers, resolve each leg's DECLARED (tcl, z) libraries
     (`sqlite_build`, `sqlite_libs`);
  7/7b build the testfixture and the sqlite3 CLI per leg (`sqlite_build`);
  7c the CLI smoke gate (`sqlite_smoke`);
  8  the unit corpus with its resume engine (`sqlite_units`);
  9  the ledger and the exit code (`sqlite_report`).

Exit codes: 0 every selected leg verified green; 1 anything else (every reason printed);
2 a malformed command line; 3 another run holds the shared sqlite clone (first stderr line
`DSS-CLONE-LOCK-BLOCKED`). The environment knobs are read and validated up front
(`sqlite_common.Config`).

The command line (2026-09-25), what sqlite.yml's steps pass from their inputs -- each `--name=value`
or `--name value`, at most once:
  --tier T         the unit-corpus tier, <sqlite>/test/<T>.test (DSS_TIER by hand; veryquick)
  --dss-config C   the dsscp configuration the artifacts are compiled with (DSS_CONFIG; release)
  --test-file F    ONE .test file run alone instead of the tier; empty = the tier (DSS_TEST_FILE)
  --dss PATH       the dsscp this run uses, as named -- never searched for, never rebuilt (DSS_BIN);
                   REQUIRED, one channel or the other; every step passes the leg's own, `{product}`
  --recompile L    the round-close recompile of leg L (below); it takes --dss and --dss-config
A flag and its environment variable naming different values is refused.

`--self-test` runs Step 0 ALONE and exits (0 every check held, 1 not): the gate's entry
(`harness/sqlite_driver_selftest`) runs exactly the list and the judgement a real run applies
before it starts, so the two cannot come to disagree about what is checked.

`--recompile <leg>` is the ROUND-CLOSE RECOMPILE (`sqlite_recompile.py`), the `recompile` manual step
of sqlite.yml: the leg's testfixture manifest, composed exactly as Step 7 composes it from THIS
tree's staged sqlite state (re-staged by the mode itself when missing or not current), compiled by
the dsscp `--dss` names (the step passes the leg's own, `{product}`; DSS_BIN by hand) and by the
leg's same-platform reference, then a per-TU census and ONE summary line
`recompile: <leg> sqlite=<sha12> tus=N reference_ok=N dss_ok=N blockers=N`, naming the pinned sqlite
commit (legs.json `stageBuild.sqliteCommit`). A blocker is a TU the reference
compiles and dsscp refuses. Exit 0 only with no blocker and a census that saw every TU.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import traceback
import urllib.error
import urllib.request

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))

import sqlite_common as C        # noqa: E402

HERE = C.HERE
SELF_TESTS = ("test_confound_scope.py", "test_driver_contracts.py")
# `gen-pe64-manifest.py` joined 2026-09-23 with its first self-test (the TU-prelude wrapper and its
# refusals): a self-test behind a flag no gate passes proves nothing.
MODULE_SELF_TESTS = ("sqlite_base.py", "sqlite_coherence.py", "sqlite_corpus.py", "sqlite_procs.py",
                     "sqlite_stage.py", "gen-pe64-manifest.py")


# ── Step 0 ────────────────────────────────────────────────────────────────────────────

def _summary(out, strict_zero_failed):
    """The LAST `passed=N failed=N skipped=N` (or `passed=N failed=0`) line -> (passed, skipped)."""
    for line in reversed(out.splitlines()):
        parts = line.strip().split()
        kv = dict(p.split("=", 1) for p in parts if "=" in p)
        if parts and parts[0].startswith("passed=") and "failed" in kv:
            try:
                passed, failed = int(kv["passed"]), int(kv["failed"])
                skipped = int(kv.get("skipped", "0"))
            except ValueError:
                return None
            if strict_zero_failed and failed != 0:
                return None
            return passed, skipped
    return None


def _failure_report(out, tail=20, detail_cap=80):
    """What a failed self-test's output must show: EVERY line that reports a failure (`FAIL`,
    wherever it sits) WITH ITS DETAIL -- the non-blank lines right after it that are indented
    deeper than it, which is where an arm prints what it OBSERVED and EXPECTED -- then the tail.
    ✔MEASURED 2026-09-22: the last 40 lines alone left the failing arm of a 134-arm self-test out
    of the report, while the refusal below claimed the output named it. ✔MEASURED 2026-09-23: the
    failing lines alone left out every arm's detail, so four macOS failures reached the gate as
    four bare names, from a host the gate can only reach through a runner. A detail longer than
    `detail_cap` lines is cut, and the cut says how many lines it dropped."""
    lines = (out or "").splitlines()
    blocks, i = [], 0
    while i < len(lines):
        if "FAIL" not in lines[i]:
            i += 1
            continue
        head = lines[i]
        depth = len(head) - len(head.lstrip())
        j = i + 1
        while j < len(lines) and lines[j].strip() and len(lines[j]) - len(lines[j].lstrip()) > depth:
            j += 1
        detail = lines[i + 1:j]
        if len(detail) > detail_cap:
            detail = detail[:detail_cap] + ["%s... %d more detail line(s) not shown"
                                            % (" " * (depth + 2), len(detail) - detail_cap)]
        blocks.append("\n".join([head] + detail))
        i = j
    parts = []
    if blocks:
        parts.append("the failing line(s), each with its detail:\n" + "\n".join(blocks))
    parts.append("the last %d line(s):\n%s" % (tail, "\n".join(lines[-tail:])))
    return "\n".join(parts)


def step0(run):
    """Refuse to start when the late-stage logic is broken. DSS_SKIP_SELFTEST=1 skips RUNNING the
    suites and the resolver's self-test, never the existence checks and never the lint.
    ★ EVERY suite runs, and the lint, before the ONE refusal that names each failure. ✔MEASURED
    2026-09-23: stopping at the first failing suite hid the next one -- on a host reached only
    through a runner, the Mac's `sqlite_stage.py` failures surfaced one gate AFTER the
    `sqlite_base.py` fix, a whole round trip later."""
    log, cfg = run.log, run.cfg
    for path, what in ((C.HARNESS_LEGS, "leg resolver"), (C.LEGS_JSON, "leg catalogue"),
                       (C.MANIFEST_GEN, "manifest generator"), (C.CLI_SMOKE, "CLI smoke gate"),
                       (C.STAGE_ZINC, "per-target header stager"),
                       (C.BENCH_CORE, "compiler-currency pre-flight")):
        if not os.path.isfile(path):
            C.die("%s missing: %s\n      It is part of this harness; there is no fallback, by design."
                  % (what, path))
    failed = []
    if cfg.skip_selftest:
        log.warn("driver self-tests SKIPPED (DSS_SKIP_SELFTEST=1) — a late-stage defect will not surface "
                 "until the end of the run.")
    else:
        for name in SELF_TESTS + MODULE_SELF_TESTS:
            path = os.path.join(HERE, name)
            if not os.path.isfile(path):
                C.die("driver self-test missing: %s\n      This guard is what stops a defect in the "
                      "END-OF-RUN classifier from costing you the entire run. Restore the file, or set "
                      "DSS_SKIP_SELFTEST=1 knowing that a classifier fault will surface only after the "
                      "corpus has finished." % path)
            args = [] if name in SELF_TESTS else ["--self-test"]
            r = C.capture(C.python_argv(path, *args), env_=C.child_env(python=True), merge=True)
            if r.rc != 0:
                print(_failure_report(r.out), file=sys.stderr)
                print(" ✗ DRIVER SELF-TEST FAILED (%s, rc=%d) — its failing assertions are above."
                      % (name, r.rc), file=sys.stderr, flush=True)
                failed.append("%s (rc=%d)" % (name, r.rc))
                continue
            got = _summary(r.out, strict_zero_failed=True)
            if got is None:
                print(C.last_lines(r.out, 20), file=sys.stderr)
                print(" ✗ driver self-test %s exited 0 but printed no readable 'passed=N failed=0 "
                      "skipped=N' line. A self-test whose RESULT cannot be read proves nothing." % name,
                      file=sys.stderr, flush=True)
                failed.append("%s (exit 0, no readable result line)" % name)
                continue
            passed, skipped = got
            if skipped:
                log.warn("driver self-test %s: OK (%d assertions) — but %d assertion(s) SKIPPED on this "
                         "host; those are UNPROVEN for this run." % (name, passed, skipped))
                for line in r.out.splitlines():
                    if line.lstrip().upper().startswith("SKIP"):
                        log.warn("      %s" % line.strip())
            else:
                log.info("driver self-test %s: OK (%d assertions, 0 skipped)" % (name, passed))
        r = run.resolver.call(["--self-test"])
        got = _summary(r.out, strict_zero_failed=True)
        if r.rc != 0 or got is None:
            print(_failure_report((r.out or "") + (r.err or "")), file=sys.stderr)
            print(" ✗ LEG-PLAN SELF-TEST FAILED (rc=%d) — the leg resolver or the catalogue it reads is "
                  "broken, so a run would build a leg set nobody declared." % r.rc, file=sys.stderr,
                  flush=True)
            failed.append("the leg plan's self-test (rc=%d)" % r.rc)
        else:
            log.info("leg-plan self-test: OK (%d assertions)" % got[0])
    r = run.resolver.call(["--lint"])
    if r.rc != 0:
        print(C.last_lines((r.out or "") + (r.err or ""), 40), file=sys.stderr)
        print(" ✗ THE LEG CATALOGUE DOES NOT LINT — see %s." % C.LEGS_JSON, file=sys.stderr, flush=True)
        failed.append("the leg catalogue's lint (rc=%d)" % r.rc)
    else:
        log.info("leg catalogue: lints clean (%s)" % C.LEGS_JSON)
    if failed:
        C.die("DRIVER SELF-TEST FAILED: %d of Step 0's checks — %s — refusing to start.\n      Late-stage "
              "driver logic is broken, so this run would execute the whole corpus and then misclassify "
              "it. The output above names every failing assertion." % (len(failed), "; ".join(failed)))


# ── Step 1 ────────────────────────────────────────────────────────────────────────────

def step1(run):
    log, cfg = run.log, run.cfg
    log.step("1/9  Host identification + leg plan (from legs.json), online")
    if run.host == "linux":
        log.info("host: %s Linux (%s)" % ("WSL" if C.host_is_wsl() else "native", run.arch))
    else:
        log.info("host: %s (%s)" % (run.host, run.arch))
    plan = run.resolver.json(["--plan"] + run.resolver.host_args + ["--format", "json"],
                             "the leg plan (--plan)")
    legs = plan.get("legs") if isinstance(plan, dict) else None
    if not legs:
        C.die("the leg plan parsed but declares ZERO legs — see %s. An empty plan would leave this run "
              "with ZERO legs and a summary that says nothing failed." % C.LEGS_JSON)
    run.plan = plan
    run.legs = [C.Leg(d) for d in legs]
    run.ledger = C.Ledger(read_vocabulary(run.resolver), log)
    log.info("legs declared by %s (the SAME set on every host): %s"
             % (os.path.basename(C.LEGS_JSON), " ".join(lg.label for lg in run.legs)))
    for lg in run.legs:
        if lg.run_mode == "native":
            log.info("  %s  %s  — build + run NATIVELY here" % (lg.label, lg.spec))
        elif lg.run_mode == "launched":
            verb = lg.run.get("pathTranslation") or "none"
            log.info("  %s  %s  — build here, run under '%s'%s"
                     % (lg.label, lg.spec, " ".join(lg.launcher),
                        (" [paths -> %s]" % verb) if verb != "none" else ""))
        elif lg.run_mode == "skip":
            log.info("  %s  %s  — build here; NOT runnable on this host [%s]: %s"
                     % (lg.label, lg.spec, lg.run.get("verdict"), lg.run.get("detail")))
        else:
            C.die("leg '%s' has an unknown run mode '%s' — the resolver and this driver disagree about "
                  "the vocabulary." % (lg.label, lg.run_mode))
    select_legs(run)
    if cfg.strict:
        log.warn("DSS_STRICT_ARM_VERDICTS=1 — every ENVIRONMENTAL skip (a missing launcher, a missing "
                 "declared build input) will FAIL this run.")
    launcher_prereq_gate(run)
    online_check(run)


def read_vocabulary(resolver):
    """The CLOSED verdict vocabulary, read from its owner (`--verdict-vocabulary`) -- never a
    driver-local copy. Each token is stripped of its line end (a Windows Python child wrote
    `ran\\r\\n`, and a kept CR made every legitimate token "unknown"); a failing resolver is refused."""
    r = resolver.call(["--verdict-vocabulary"])
    if r.rc != 0:
        C.die("the leg resolver could not state the CLOSED verdict vocabulary (rc=%d):\n      %s\n"
              "      Without it this driver cannot tell a classified skip from an unclassified one."
              % (r.rc, (r.err or r.out).strip() or "<no diagnostic>"))
    return [ln.strip() for ln in r.out.replace("\r", "\n").split("\n") if ln.strip()]


def select_legs(run):
    log, cfg = run.log, run.cfg
    labels = [lg.label for lg in run.legs]
    if cfg.legs_filter:
        keep = [lg for lg in run.legs if lg.label in cfg.legs_filter]
        if not keep:
            C.die("DSS_LEGS='%s' matched no declared leg (have: %s)." % (C.env("DSS_LEGS"),
                                                                       " ".join(labels)))
        dropped = [lg for lg in run.legs if lg not in keep]
        if dropped:
            log.warn("DSS_LEGS='%s' DESELECTED %d declared leg(s) — this run does NOT cover them:"
                     % (C.env("DSS_LEGS"), len(dropped)))
        for lg in dropped:
            lg.selected = False
            run.ledger.set_leg(lg, "not-selected-by-runner", "deselected by DSS_LEGS='%s' — %s was NOT "
                               "built and NOT verified by this run" % (C.env("DSS_LEGS"), lg.spec))
            log.warn("      %s (%s) — not built, not verified" % (lg.label, lg.spec))
    if cfg.fidelity_filter:
        r = run.resolver.call(["--run-fidelities"])
        known = [ln.strip() for ln in r.out.splitlines() if ln.strip()]
        if r.rc != 0 or not known:
            C.die("could not read the run-fidelity vocabulary from harness_legs.py (rc=%d)" % r.rc)
        for f in cfg.fidelity_filter:
            if f not in known:
                C.die("DSS_RUN_FIDELITY names '%s', which is not a run fidelity this harness declares.\n"
                      "      Known: %s\n      A value nothing matches would silently select ZERO legs, "
                      "which reads exactly like a host that can execute nothing." % (f, " ".join(known)))
        keep = [lg for lg in run.selected() if (lg.fidelity or "") in cfg.fidelity_filter]
        if not keep:
            C.die("DSS_RUN_FIDELITY='%s' selected NO leg on this host.\n      Per-leg fidelity here: %s"
                  % (C.env("DSS_RUN_FIDELITY"), " ".join("%s=%s" % (lg.label, lg.fidelity or
                                                                    "<never runs>")
                                                         for lg in run.selected())))
        for lg in run.selected():
            if lg not in keep:
                lg.selected = False
                run.ledger.set_leg(lg, "not-selected-by-runner",
                                   "deselected by DSS_RUN_FIDELITY='%s' — this leg's run fidelity on "
                                   "this host is '%s', so %s was NOT built and NOT verified by this run"
                                   % (C.env("DSS_RUN_FIDELITY"), lg.fidelity or "<never runs here>",
                                      lg.spec))
                log.warn("      %s (%s) — fidelity '%s', not built, not verified"
                         % (lg.label, lg.spec, lg.fidelity or "<never runs here>"))
    log.info("legs selected: %s   corpus: %s" % (" ".join(lg.label for lg in run.selected()),
                                                 cfg.corpus_label()))
    for lg in run.selected():
        log.info("   %s: run mode '%s', fidelity '%s'" % (lg.label, lg.run_mode or "<unset>",
                                                        lg.fidelity or "<never runs here>"))


def launcher_prereq_rows(report):
    out = []
    for row in report.get("missing") or []:
        out.append("MISSING [%s] %s" % (row.get("kind", "?"), row.get("path", "?")))
        out.append("      provides: %s" % (row.get("provides") or "<not declared>"))
        out.append("      why     : %s" % (row.get("why") or "<not declared>"))
        out.append("      install : %s" % (row.get("install") or "<not declared>"))
        if row.get("probe"):
            out.append("      probed  : %s" % " ".join(str(p) for p in row["probe"]))
    for u in report.get("uncovered") or []:
        out.append("UNCOVERED %s" % u)
    return out


def launcher_prereq_gate(run):
    """`--check-launcher` EXECUTES each launched leg's DECLARED prerequisites in the launcher's own
    namespace: rc 0 met; rc 3 unmet (JSON report) -> the leg is still BUILT and its run SKIPPED
    as `skipped-launcher-prerequisite-missing`; anything else (an unreadable rc-3 report included)
    -> `poisoned`, never assumed benign."""
    log = run.log
    unmet = 0
    for lg in run.selected():
        if lg.run_mode != "launched":
            continue
        r = run.resolver.call(["--check-launcher", lg.label] + run.resolver.host_args)
        if r.rc == 0:
            log.info("[%s] launcher '%s': every DECLARED prerequisite is present on this machine"
                     % (lg.label, " ".join(lg.launcher)))
            continue
        unmet += 1
        report = None
        if r.rc == 3:
            try:
                report = json.loads(r.out)
            except ValueError:
                report = None
        if r.rc == 3 and isinstance(report, dict):
            rows = launcher_prereq_rows(report)
            n = len([x for x in rows if x.startswith("MISSING ")])
            log.warn("[%s] LAUNCHER PREREQUISITE MISSING — this host HAS '%s', and does NOT have "
                     "everything that launcher DECLARES it needs." % (lg.label, " ".join(lg.launcher)))
            for row in rows:
                log.warn("      %s" % row)
            log.warn("      This leg is STILL BUILT. Its sqlite3 CLI smoke gate and its ENTIRE unit "
                     "corpus are NOT run on this machine.")
            detail = ("the DECLARED launcher '%s' is present on this host but %d of its DECLARED "
                      "prerequisite(s) are not — see the rows above for what each one provides and how "
                      "to install it" % (" ".join(lg.launcher), n))
            token = "skipped-launcher-prerequisite-missing"
        else:
            why = " ".join((r.err or r.out).split()) or "<no diagnostic>"
            log.warn("[%s] the launcher-prerequisite check exited %d%s, which this driver does not "
                     "recognise as a verdict." % (lg.label, r.rc,
                                                  " with a report that is not JSON" if r.rc == 3 else ""))
            log.warn("      %s" % why)
            log.warn("      Treating it as a FAILURE rather than assuming the launcher is fine.")
            detail = ("harness_legs.py --check-launcher exited %d for this leg (%s), so whether its "
                      "launcher can start the artefact is UNKNOWN on this machine" % (r.rc, why))
            token = "poisoned"
        lg.run = dict(lg.run, mode="skip", verdict=token, detail=detail)
        lg.run_mode = "skip"
        run.ledger.set_leg(lg, token, detail)
    if unmet:
        log.warn("%d leg(s) will NOT be executed on this machine because a DECLARED launcher "
                 "prerequisite is absent or unreadable. They are still BUILT, and Step 9 names each one."
                 % unmet)


def online_check(run):
    req = urllib.request.Request("https://github.com", method="HEAD")
    try:
        with urllib.request.urlopen(req, timeout=20):
            pass
    except (urllib.error.URLError, OSError) as exc:
        C.die("offline — cannot reach https://github.com (%s)." % exc)
    run.log.ok("%s/%s host is online" % (run.host, run.arch))


# ── Step 2 ────────────────────────────────────────────────────────────────────────────

def git(tree, *args):
    return C.capture(["git", "-C", tree] + list(args), timeout=600)


def provenance(tree):
    """The checkout's HEAD (short/full), branch, and how far the working tree diverges from HEAD
    -- every field a NON-EMPTY, self-describing string (`UNKNOWN(<why>)`), and CR-only
    modifications excluded from the count (a Windows->Linux synced tree reported 2420 against a
    true ~60).
    Both git answers are read NUL-separated (`-z`), a rename's or copy's second path skipped:
    ✔MEASURED 2026-09-21 (arm PV21) that `diff --ignore-cr-at-eol --name-only` still lists a
    CR-only change on git 2.43 while `--numstat` honours the flag on 2.43 and 2.55, and that a
    name git QUOTES (`café.c`) never matched its unquoted porcelain spelling, so a real change
    to it was counted as CR-only -- a false clean."""
    pv = {}
    if not os.path.lexists(os.path.join(tree, ".git")):
        pv.update(head_short="UNKNOWN(no .git under %s)" % tree, head_long="", branch="UNKNOWN",
                  diverge="", diverge_note=" (divergence from HEAD UNVERIFIED — no .git under %s)" % tree)
        return pv
    r = git(tree, "rev-parse", "--short", "HEAD")
    pv["head_short"] = r.out.strip() if r.rc == 0 and r.out.strip() else \
        "UNKNOWN(rev-parse HEAD failed in %s)" % tree
    r = git(tree, "rev-parse", "HEAD")
    pv["head_long"] = r.out.strip() if r.rc == 0 else ""
    r = git(tree, "rev-parse", "--abbrev-ref", "HEAD")
    b = r.out.strip() if r.rc == 0 else ""
    pv["branch"] = "DETACHED-HEAD" if b == "HEAD" else (b or "UNKNOWN")
    st = git(tree, "--no-optional-locks", "status", "--porcelain", "-z")
    if st.rc != 0:
        st = git(tree, "status", "--porcelain", "-z")
    if st.rc != 0:
        pv["diverge"], pv["diverge_note"] = "", " (divergence from HEAD UNVERIFIED — git status failed in %s)" % tree
        return pv
    toks = st.out.split("\0")
    lines = []
    i = 0
    while i < len(toks):
        tok = toks[i]
        i += 1
        if not tok:
            continue
        lines.append((tok[:2], tok[3:]))
        if "R" in tok[:2] or "C" in tok[:2]:
            i += 1   # the ORIGINAL path of a rename/copy follows as its own field
    semantic_tracked = git(tree, "--no-optional-locks", "diff", "--ignore-cr-at-eol", "--numstat", "-z", "HEAD")
    if semantic_tracked.rc != 0:
        semantic_tracked = git(tree, "diff", "--ignore-cr-at-eol", "--numstat", "-z", "HEAD")
    if semantic_tracked.rc != 0:
        n, cr_only = len(lines), 0
    else:
        tracked = set()
        parts = semantic_tracked.out.split("\0")
        j = 0
        while j < len(parts):
            fields = parts[j].split("\t", 2)
            j += 1
            if len(fields) != 3:
                continue
            if fields[2] == "":        # a rename: `a\td\t` then OLD, NEW as their own fields
                tracked.add(parts[j + 1] if j + 1 < len(parts) else "")
                j += 2
            else:
                tracked.add(fields[2])
        n = sum(1 for xy, path in lines if xy == "??" or path in tracked)
        cr_only = max(0, len(lines) - n)
    pv["diverge"] = str(n)
    cr_note = ("; %d CR-only difference(s) excluded as non-semantic" % cr_only) if cr_only else ""
    pv["diverge_note"] = ("" if n == 0 else
                          " (+%d file(s) differ from HEAD — the sources built are NOT exactly this "
                          "commit%s)" % (n, cr_note))
    return pv


def _dir_has_entries(d):
    try:
        return bool(os.listdir(d))
    except OSError:
        return False


def step2(run):
    """VERIFY the checkout under test, then take the RUN LOCK on the output tree."""
    source_gate(run)
    take_run_lock(run)


def source_gate(run):
    """VERIFY the checkout under test (never switch or pull it): the source gate's three shapes --
    a checkout (a `.git` FILE counts: a git worktree), a populated non-checkout (refused, never
    cloned over), an absent directory (refused unless DSS_ALLOW_FRESH_CLONE=1) -- then DSS_BRANCH /
    DSS_COMMIT asserted and the divergence from HEAD REPORTED (a dirty tree is the normal shape of a
    pre-commit probe; it is never a blocker)."""
    log, cfg = run.log, run.cfg
    src = run.repo_root
    self_repo = run.driver_tree or ""
    if os.path.lexists(os.path.join(src, ".git")):
        log.step("2/9  Use dsscp at %s (current checkout, untouched)" % src)
    elif _dir_has_entries(src):
        C.die("%s exists and is NOT a git checkout (no .git entry).\n      That is the tree an rsync with "
              "--exclude=/.git produces: real sources and no repository. The harness will NOT clone over "
              "it.\n      Point SRC_DIR at a real checkout:\n        SRC_DIR=%s python3 build_and_test.py\n"
              "      or, if this tree is disposable, remove it and opt in to a clone:\n        "
              "DSS_ALLOW_FRESH_CLONE=1 DSS_BRANCH=<branch> python3 build_and_test.py"
              % (src, self_repo or "/path/to/dss-code-prime"))
    elif cfg.allow_fresh_clone:
        log.step("2/9  Clone dsscp -> %s (DSS_ALLOW_FRESH_CLONE=1, branch: %s)"
                 % (src, cfg.dss_branch or "<repo default>"))
        import sqlite_stage as S  # the ONE clone/update implementation
        S.clone_or_update(cfg.dss_repo_url, src, cfg.dss_branch, log=log)
    else:
        C.die("no dsscp checkout at %s, and this harness will NOT clone one silently.\n      A fresh clone "
              "takes %s at its DEFAULT branch unless told otherwise — so an unattended multi-hour corpus "
              "run would validate a compiler that is not the branch you are working on. Say which you "
              "want:\n        SRC_DIR=%s python3 build_and_test.py   # use a checkout you have\n        "
              "DSS_ALLOW_FRESH_CLONE=1 DSS_BRANCH=<branch> python3 build_and_test.py   # clone, and NAME "
              "the branch" % (src, cfg.dss_repo_url, self_repo or "/path/to/your/checkout"))
    pv = provenance(src)
    run.provenance = pv
    log.info("  at %s on %s%s" % (pv["head_short"], pv["branch"], pv["diverge_note"]))
    if cfg.dss_branch and pv["branch"] != cfg.dss_branch:
        C.die("DSS_BRANCH='%s' but %s is on '%s'.\n      Refusing to spend a multi-hour corpus run on a "
              "branch you did not ask for. Check the branch out yourself — this harness NEVER switches "
              "our own repo — or drop DSS_BRANCH." % (cfg.dss_branch, src, pv["branch"]))
    if cfg.dss_commit:
        if not os.path.lexists(os.path.join(src, ".git")):
            C.die("DSS_COMMIT='%s' cannot be verified: %s is not a git checkout." % (cfg.dss_commit, src))
        r = git(src, "rev-parse", "--verify", "--quiet", "%s^{commit}" % cfg.dss_commit)
        full = r.out.strip() if r.rc == 0 else ""
        if not full:
            C.die("DSS_COMMIT='%s' does not resolve to a commit in %s — fetch it, or fix the value."
                  % (cfg.dss_commit, src))
        if not pv["head_long"]:
            C.die("DSS_COMMIT='%s' cannot be verified: rev-parse HEAD failed in %s." % (cfg.dss_commit, src))
        if full != pv["head_long"]:
            C.die("DSS_COMMIT='%s' (%s) but %s is at %s.\n      The run would have validated the checkout, "
                  "not the commit you named." % (cfg.dss_commit, full, src, pv["head_long"]))
        log.info("  DSS_COMMIT verified: HEAD is %s" % full)
    if pv["diverge"] == "":
        log.warn("could not measure how far %s diverges from HEAD — the commit in the Step-9 verdict is "
                 "UNVERIFIED." % src)
    elif pv["diverge"] != "0":
        log.warn("%s file(s) in %s differ from HEAD (%s) — the compiler this run builds is NOT that commit.\n"
                 "      Not an error, and never a blocker: uncommitted work you are gating and a STALE .git "
                 "beside fresh sources are INDISTINGUISHABLE from inside the tree, so the count rides along "
                 "on the Step-9 verdict." % (pv["diverge"], src, pv["head_short"]))
    log.ok("dsscp checkout ready")


def take_run_lock(run):
    """Single instance per output tree, LIVENESS-based (pid + that process's start marker), so a
    crashed run never wedges the next one: a stale owner is taken over and SAID."""
    log = run.log
    import sqlite_procs as P
    run.run_lock = P.RunLock(os.path.join(run.out_dir, ".harness-lock"))
    stolen = run.run_lock.acquire(log)
    if stolen:
        run.hygiene.append("took over a STALE run lock left by PID %s" % stolen)
    log.info("run lock: %s (pid %d)" % (os.path.join(run.out_dir, ".harness-lock"), os.getpid()))


# ── Steps 3–4 ─────────────────────────────────────────────────────────────────────────

def _relay(argv, log, env=None):
    """Run `argv`, relaying its merged output line by line as it arrives. -> (rc, captured text)."""
    proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    captured = []
    for raw in proc.stdout:
        line = raw.decode("utf-8", "replace").rstrip("\r\n")
        captured.append(line)
        log.info("   " + line)
    return proc.wait(), "\n".join(captured)


def posix_sqlite_dir(run):
    """The shared sqlite clone, spelled on the POSIX side (`~` expanded THERE)."""
    d = run.cfg.sqlite_dir or "~/src/sqlite"
    if not run.posix.needs_wsl:
        return os.path.abspath(os.path.expanduser(d))
    if d.startswith("~"):
        # Asked THROUGH the POSIX side, the one route into it (no private `wsl.exe` argv here).
        r = C.capture(run.posix.argv(["printenv", "HOME"]), timeout=60)
        home = r.out.replace("\0", "").strip()
        if r.rc != 0 or not home.startswith("/"):
            C.die("could not read HOME on the POSIX side (`%s` exited %d) to place the sqlite clone; "
                  "set SQLITE_DIR to its POSIX path." % (" ".join(run.posix.argv(["printenv", "HOME"])),
                                                         r.rc))
        d = home + d[1:]
    return d


def step34(run):
    log, cfg = run.log, run.cfg
    import sqlite_stage as S
    run.stage_build = run.resolver.json(["--stage-build", "--format", "json"],
                                        "the sqlite stage-build configuration (--stage-build)")
    run.sqlite_dir_posix = posix_sqlite_dir(run)
    if not run.posix.needs_wsl:
        import sqlite_procs as P
        log.step("3/9  Fetch sqlite/sqlite -> %s (default branch)" % run.sqlite_dir_posix)
        run.clone_lock = P.CloneLock(run.sqlite_dir_posix)
        run.clone_lock.write("build_and_test.py (fetch/pull + configure + stage)", log)
        scfg = S.StageConfig.from_run(run)
        run.stage = S.stage_and_persist(scfg, log=log, lock=run.clone_lock)
        return
    log.step("3+4/9  Derive full-source testfixture recipe + stage sources/headers (WSL)")
    run.stage_dir = S.stage_dir_of(run.out_dir)
    os.makedirs(run.stage_dir, exist_ok=True)
    fd, sb_path = tempfile.mkstemp(prefix="stage-build-", suffix=".json", dir=run.out_dir)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(run.stage_build, fh, indent=1, sort_keys=True)
    try:
        argv = ["python3", run.posix.to_posix(os.path.join(C.HERE, "sqlite_stage.py")), "derive",
                "--out", run.posix.to_posix(run.stage_dir), "--sqlite-dir", run.sqlite_dir_posix,
                "--jobs", str(cfg.jobs), "--stage-build-json", run.posix.to_posix(sb_path),
                "--tier", cfg.tier]
        if cfg.tcl_version:
            argv += ["--tcl-version", cfg.tcl_version]
        if cfg.test_file:
            argv += ["--test-file", run.posix.to_posix(cfg.test_file)]
        rc, out = _relay(run.posix.argv(argv), log)
    finally:
        try:
            os.remove(sb_path)
        except OSError:
            pass
    if rc == 3 or "DSS-CLONE-LOCK-BLOCKED" in out:
        raise C.CloneLockBlocked("DSS-CLONE-LOCK-BLOCKED\n      another harness run holds the shared sqlite "
                                 "clone %s inside WSL — see the derive's output above."
                                 % run.sqlite_dir_posix)
    if rc != 0:
        C.die("the WSL derive FAILED (rc=%d) — see its output above." % rc)
    result = os.path.join(run.stage_dir, S.RESULT_FILE)
    try:
        with open(result, "r", encoding="utf-8") as fh:
            run.stage = S.StageResult.from_json(fh.read())
    except (OSError, ValueError) as exc:
        C.die("the WSL derive exited 0 but its result %s could not be read (%s)." % (result, exc))


# ── main ──────────────────────────────────────────────────────────────────────────────

def run_all(run):
    import sqlite_build as BLD
    import sqlite_compiler as CMP
    import sqlite_libs as LIBS
    import sqlite_report as REP
    import sqlite_smoke as SMK
    import sqlite_units as UNITS
    import sqlite_base as B
    # ★ THE COMPILER IS GIVEN, AND A RUN THAT NAMES NONE STOPS HERE: Step 5 uses the dsscp --dss or
    # DSS_BIN names and never finds or builds one, so its absence is refused before Steps 0-4 spend
    # their minutes on the self-tests, the leg plan and staging sqlite.
    CMP.named_compiler(run.cfg)
    step0(run)
    step1(run)
    step2(run)
    step34(run)
    run.log.step("5/9  The dsscp this run was GIVEN (RELEASE — build type READ from its own tree)")
    run.compiler = CMP.obtain(run.cfg, log=run.log)
    run.config_root = CMP.pin_config_root(run.repo_root, log=run.log)
    specs = [lg.spec for lg in run.selected()]
    proved = CMP.assert_current(C.BENCH_CORE, run.compiler, run.config_root, specs,
                                CMP.rebuild_command(run.compiler, run.repo_root))
    run.currency_note = ("%s — this binary PROVED it compiles against it for %s"
                         % (os.path.join(run.config_root, "src", "dss-config"), proved))
    run.log.ok("compiler current for: %s" % proved)
    run.log.step("6/9  Per-target headers + each leg's DECLARED build inputs (tcl + z)")
    BLD.stage_headers(run)
    LIBS.step6(run)
    run.artifacts = B.VerdictLedger()
    BLD.step7(run)
    BLD.step7b(run)
    SMK.step7c(run)
    UNITS.step8(run)
    return REP.step9(run)


def self_test():
    """`--self-test`: Step 0 alone. DSS_SKIP_SELFTEST is IGNORED here, and said: this mode exists
    only to run the self-tests, so a knob that skips them would turn the gate's entry into a pass
    that checked nothing."""
    try:
        cfg = C.Config()
        run = C.Run(cfg)
        if cfg.skip_selftest:
            run.log.warn("DSS_SKIP_SELFTEST=1 is IGNORED by --self-test: this mode exists only to run "
                         "the self-tests.")
            cfg.skip_selftest = False
        step0(run)
    except C.HarnessDie as exc:
        print(" ✗ ERROR: %s" % exc, file=sys.stderr, flush=True)
        print("build_and_test.py --self-test: FAILED", flush=True)
        return 1
    print("build_and_test.py --self-test: OK (%d driver self-test(s), the leg plan's self-test and "
          "lint)" % len(SELF_TESTS + MODULE_SELF_TESTS), flush=True)
    return 0


def place_run(run):
    """Where a run's trees are, the ONE rule for every mode: the DSS tree (SRC_DIR, else the tree
    this harness ships in) and the output tree (OUT_DIR, else `<tree>/build/real-examples/c/sqlite`,
    under `windows/` on a Windows host). The recompile finds the STAGE a run left there."""
    cfg = run.cfg
    run.driver_tree = C.driver_tree()
    run.repo_root = os.path.abspath(cfg.src_dir) if cfg.src_dir else (run.driver_tree or "")
    if not run.repo_root:
        C.die("this copy of the harness lives in no DSS tree, and SRC_DIR is not set — name the "
              "checkout to build with SRC_DIR=<path>.")
    run.out_dir = os.path.abspath(cfg.out_dir) if cfg.out_dir else os.path.join(
        run.repo_root, "build", "real-examples", "c", "sqlite",
        *(["windows"] if run.host == "windows" else []))
    run.registry_glob = os.path.join(run.driver_tree or os.path.join(HERE, "no-dss-tree"),
                                     ".plans", "_deferred-anchor-registry*.md")


# ── the command line ──────────────────────────────────────────────────────────────────
# The values a harness STEP hands this driver (sqlite.yml): a run's knobs and the leg's own dsscp, or a
# recompile's leg. Every other knob of a run stays an environment variable (`sqlite_common.Config`).
VALUE_FLAGS = ("--tier", "--dss-config", "--test-file", "--dss", "--recompile")
KNOB_ATTRS = {"--tier": "tier", "--dss-config": "dss_config", "--test-file": "test_file", "--dss": "dss_bin"}
EMPTY_MEANS_NONE = ("--test-file",)     # an empty test file runs the tier -- the step's default


def parse_cli(args):
    """-> {flag: value} for the VALUE_FLAGS `args` gives, each `--name=value` or `--name value` and at
    most once; raises ValueError naming the first defect."""
    got, i = {}, 0
    while i < len(args):
        name, eq, value = args[i].partition("=")
        if name not in VALUE_FLAGS:
            raise ValueError("unknown argument '%s'" % args[i])
        if not eq:
            if i + 1 >= len(args):
                raise ValueError("%s needs a value" % name)
            i += 1
            value = args[i]
        if name in got:
            raise ValueError("%s is given twice" % name)
        if not value.strip() and name not in EMPTY_MEANS_NONE:
            raise ValueError("%s names nothing (an empty value)" % name)
        got[name] = value
        i += 1
    if "--recompile" in got:
        idle = [f for f in ("--tier", "--test-file") if f in got]
        if idle:
            raise ValueError("%s name%s nothing in a recompile, which runs no corpus"
                             % (" and ".join(idle), "" if len(idle) > 1 else "s"))
    return got


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if args in (["-h"], ["--help"]):
        print(__doc__)
        return 0
    if args == ["--self-test"]:
        return self_test()
    try:
        flags = parse_cli(args)
    except ValueError as exc:
        print("build_and_test.py: %s.\n      It takes `--self-test` (Step 0 alone); a run with --dss PATH "
              "[--tier T] [--dss-config C] [--test-file F]; or `--recompile <leg> --dss PATH "
              "[--dss-config C]` -- the compiler REQUIRED, by --dss or DSS_BIN. Every other knob is an "
              "environment variable (sqlite_common.Config)."
              % exc, file=sys.stderr)
        return 2
    knobs = dict((KNOB_ATTRS[f], v) for f, v in flags.items() if f in KNOB_ATTRS)
    if "--recompile" in flags:
        import sqlite_recompile as RC
        return RC.main(flags["--recompile"], sys.modules[__name__], knobs)
    run = None
    try:
        cfg = C.Config(knobs)
        run = C.Run(cfg)
        place_run(run)
        return run_all(run)
    except C.CloneLockBlocked as exc:
        print(str(exc), file=sys.stderr, flush=True)
        return exc.exit_code
    except C.HarnessDie as exc:
        print(" ✗ ERROR: %s" % exc, file=sys.stderr, flush=True)
        return exc.exit_code
    except KeyboardInterrupt:
        print(" ✗ ERROR: interrupted", file=sys.stderr, flush=True)
        return 130
    except Exception:  # noqa: BLE001 -- the ERR trap's successor: never a silent exit
        print(" ✗ ERROR: the harness failed with an unexpected exception:\n%s"
              % traceback.format_exc(), file=sys.stderr, flush=True)
        return 1
    finally:
        if run is not None:
            for lock in (run.clone_lock, run.run_lock):
                if lock is not None:
                    try:
                        lock.release()
                    except Exception:  # noqa: BLE001 -- a release failure must not mask the verdict
                        pass


if __name__ == "__main__":
    sys.exit(main())
