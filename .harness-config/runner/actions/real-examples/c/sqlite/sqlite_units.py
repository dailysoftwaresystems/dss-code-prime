#!/usr/bin/env python3
"""sqlite_units.py -- Step 8 of the SQLite corpus harness: the .test UNIT CORPUS, per runnable leg.

For every leg whose fixture BUILT and that this host can EXECUTE (`run.mode`, never a host test):
  1. its RUN DIRECTORY on the launcher's OWN filesystem (declared, `--run-dir-plan`);
  2. the LOADEXT helper, built by DSS for the leg's `sharedLibFormat` and carried into that
     directory (`--build-loadext-helper`: 0 staged / 4 environmental skip / 3 or anything else
     poisoned, a non-JSON report included);
  3. the leg's confound rows CORROBORATED against that directory, then the supply refused unless
     the plan measured (`confoundGating` probed, `runDirectoryGating` not-required|measured);
  4. THE RESUME ENGINE: segment 0 is the tier script exactly as always; an ABORT (no summary
     line) is named, reported and RESUMED PAST through sqlite's own hooks
     (`permutations.test <perm>` with SQLITE_TEST_PATTERN_LIST = the files after the boundary,
     then `<tier>.test --start=<nextperm>:`), the boundary STRICTLY advancing, bounded by
     DSS_MAX_RESUMES; two consecutive zero-progress segments ending IDENTICALLY are a
     PRECONDITION FAILURE (the fixture never started), not a resumable crash;
  5. the UNION of the segments, the classification (by name, then per failure on clock evidence
     from its own execution), the per-unit ledger file, the capability witnesses, the aborts
     classified earned/unearned (`--classify-abort`), the VERDICT LADDER, and the registry pointer
     on any FAIL.

★ Every child gets its environment as an explicit dict: SQLITE_TEST_PATTERN_LIST exists only for a
permutation segment (an empty-but-set one selected ZERO files and read as green -- MEASURED), and
the launcher's carrier forwards only what is SET in that segment's dict.
★ A leg that cannot run records a NAMED not-run from the closed vocabulary (`Ledger.unit_not_run`)
and the run continues to every other leg.

The union of `build-and-test.sh` and `build-and-test.ps1` Step 8 (lane mig, part 4, 2026-09-21),
with the .sh's verdict ladder (the .ps1 tested segment 0's summary alone, which made an
earned-abort PASS unreachable). Nothing runs at import.
"""
from __future__ import annotations

import collections
import json
import os
import shutil
import sys

import sqlite_common as C

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass
sys.dont_write_bytecode = True

import sqlite_corpus as K        # noqa: E402
import sqlite_launch as L        # noqa: E402
import sqlite_procs as P         # noqa: E402
import sqlite_verdicts as V      # noqa: E402

# One queued fixture invocation. `script` is the .test file the fixture sources (its Tcl
# `$argv0`), ALREADY spelled in the launcher's namespace; `arg` is a bare Tcl word or a
# tester.tcl flag and never a path. `patterns` is the SQLITE_TEST_PATTERN_LIST file list, or
# None for a segment that must run WITHOUT the variable.
Segment = collections.namedtuple("Segment", ["kind", "perm", "label", "patterns", "script", "arg"])

NotReached = collections.namedtuple("NotReached", ["owner", "text"])


def _field(obj, name):
    return obj.get(name) if isinstance(obj, dict) else getattr(obj, name)


def _rmtree(path):
    if os.path.lexists(path):
        shutil.rmtree(path)


def _json(text):
    try:
        v = json.loads(text)
        return v if isinstance(v, dict) else None
    except ValueError:
        return None


# ── the pieces of one leg's setup ─────────────────────────────────────────────────────

def stage_loadext(run, leg, rundir):
    """-> (code, why, crosscheck, staged): 0 staged, 1 poisoned, 2 skipped-build-input-missing.
    The DECISION is the resolver's; this turns ONE report into ONE verdict and never raises."""
    dstdir = os.path.join(rundir, L.SQLITE_TESTDIR_SUBDIR)
    work = os.path.join(run.leg_out(leg), "loadext-helper")
    try:
        os.makedirs(dstdir, exist_ok=True)
        os.makedirs(work, exist_ok=True)
    except OSError as exc:
        return 1, ("could not create the run's testdir (%s) or the helper's work dir (%s): %s"
                   % (dstdir, work, exc)), "", ""
    st = run.stage
    r = run.resolver.call(["--build-loadext-helper", leg.label, "--helper-builder",
                           run.loadext_builder, "--dss", run.compiler.path,
                           "--sqlite-src", _field(st, "src"), "--sqlite-bld", _field(st, "bld"),
                           "--dest-dir", dstdir, "--work-dir", work,
                           "--dss-config", run.cfg.dss_config,
                           "--reference-cc", "\t".join(leg.cc),
                           "--reference-machine", leg.cc_machine])
    rep = _json(r.out)
    if rep is None:
        # rc 0 with no readable report is NOT "staged": an unreadable outcome is not evidence
        # the helper is there (the old bash driver called it staged).
        return 1, ("the helper build exited %d and printed something this driver could not read as "
                   "a report: %s" % (r.rc, " ".join((r.out + " " + r.err).split())[:600])), "", ""
    why = " ".join(str(rep.get("detail") or "").split())
    cross = " ".join(str(rep.get("crossCheck") or "").split())
    staged = str(rep.get("staged") or "")
    if r.rc == 0:
        run.log.info("[%s] loadext helper -> %s — %s" % (leg.label, staged, why))
        if cross:
            run.log.info("      %s" % cross)
        return 0, why, cross, staged
    if r.rc == 4:
        return 2, why, cross, ""
    if r.rc == 3:
        return 1, why, cross, ""
    return 1, ("the helper build exited %d, which this driver does not recognise as a verdict class "
               "('%s'). Treating it as a failure rather than assuming the helper was staged. %s"
               % (r.rc, rep.get("verdictClass"), why)), cross, ""


def classify_abort(run, leg, name, log_path):
    """-> (earned, provenance text) from `--classify-abort`; a leg declaring no abort row is
    UNEARNED without asking."""
    if not [p for p in (leg.d.get("abortConfounds") or []) if p]:
        return False, "this leg declares no `matches: abort-file` row"
    r = run.resolver.call(["--classify-abort", leg.label, "--abort", name,
                           "--abort-log", log_path or os.devnull] + run.resolver.host_args)
    text = ((r.out or "") + (r.err or "")).strip()
    return r.rc == 0, text


# ── the resume decision ───────────────────────────────────────────────────────────────

AbortPlan = collections.namedtuple("AbortPlan", [
    "perm", "perm_inferred", "abort_file", "abort_source", "boundary", "forced", "tail"])


def plan_abort(facts, seg, corpus, tier_perms, last_boundary):
    """The pure half of an abort: which permutation, which file, and the new boundary (STRICTLY
    past `last_boundary`; '' = the end of the corpus). The resume segments are built by the caller.
    -> AbortPlan."""
    perm = facts.permutation or seg.perm or ""
    if not perm and len(tier_perms) == 1:
        perm = tier_perms[0]
    inferred = ""
    if not perm and facts.last_test:
        for p in tier_perms:
            if facts.last_test.startswith(p + "."):
                perm, inferred = p, "from the test-name prefix"
                break
    if not perm and facts.last_test and seg.kind == "tier" and not seg.perm and tier_perms:
        perm = tier_perms[0]
        inferred = ("INFERRED — no permutation prefix ever appeared, so the run never left '%s', the "
                    "tier's first suite" % tier_perms[0])
    # THE TRACEBACK FIRST (the fixture SAYING which file it was in), the last test name second
    # (a proxy, and measured-wrong once; kept because a KILLED segment prints no traceback).
    abort_file, source = "", ""
    if facts.abort_file:
        abort_file = K.resolve_abort_file(facts.abort_file, corpus) or ""
        if abort_file:
            source = "named by the Tcl traceback (%s)" % facts.abort_file
    if not abort_file and facts.last_test:
        abort_file = K.resolve_abort_file(facts.last_test, corpus) or ""
        if abort_file:
            source = "INFERRED from the last test name (%s)" % facts.last_test
    boundary, forced = abort_file, False
    done = facts.last_file or ""
    if not boundary or not boundary > done:
        boundary = done
    if not boundary > (last_boundary or ""):
        forced = True
        boundary = K.first_file_after(last_boundary or "", corpus) or ""
    return AbortPlan(perm, inferred, abort_file, source, boundary, forced, None)


# ── one leg ───────────────────────────────────────────────────────────────────────────

class LegRun:
    """The per-leg state of one corpus run (what the old drivers kept in ~20 parallel arrays)."""

    def __init__(self):
        self.seg_logs, self.seg_labels, self.seg_rcs, self.seg_counts = [], [], [], []
        self.facts = []
        self.aborts, self.abort_logs, self.abort_rows = [], [], []
        self.not_reached = []          # [NotReached]
        self.hygiene, self.calibration = [], []
        self.resumes = 0
        self.files_done = self.files_inert = 0
        self.total_tests = self.total_errors = 0
        self.sum_tests = self.sum_errors = self.n_summarised = 0
        self.der_tests = self.der_errors = self.n_derived = 0
        self.seg_summary = ""
        self.failures = []
        self.precondition = ""

    def note(self, owner, text):
        self.not_reached.append(NotReached(owner, text))


def corpus_entry(run, leg):
    """-> True when this leg's corpus may start. Otherwise the leg's NAMED not-run is recorded
    (from the closed vocabulary, through the one guarded recorder) and the caller moves on: none
    of these is a host test -- they are the recorded OUTCOMES of Steps 6 and 7."""
    log, ledger = run.log, run.ledger
    if not leg.tcl_lib:
        ledger.unit_not_run(leg, leg.verdict, leg.verdict_detail)
        return False
    if not leg.fixture_built:
        ledger.unit_not_run(leg, leg.verdict, "step 7 did not produce a fixture")
        log.warn("[%s] corpus skipped — step 7 did not compile the fixture" % leg.label)
        return False
    if C.run_is_skipped(leg):
        detail = "%s  (the fixture DID build: %s)" % (leg.run.get("detail") or "", leg.fixture)
        ledger.set_leg(leg, leg.run.get("verdict") or "", "BUILT OK (%s) but NOT RUN on this host — %s"
                       % (leg.fixture, leg.run.get("detail") or ""))
        ledger.unit_not_run(leg, leg.run.get("verdict") or "", detail)
        log.info("[%s] fixture built but NOT RUN here [%s]: %s"
                 % (leg.label, leg.run.get("verdict"), leg.run.get("detail")))
        return False
    if not leg.cc:
        log.info("[%s] no CONTROL compiler on this host — the corpus RUNS anyway (the loadext helper "
                 "comes from DSS); only the helper's cross-check against a second toolchain is lost."
                 % leg.label)
    return True


def poison_unrun(run, leg, why_leg, why_units, headline):
    """A leg whose fixture BUILT but whose corpus cannot start: `poisoned`, its units not run,
    counted as a STAGING failure (neither a compile failure nor a unit failure), and the run
    continues."""
    run.counts["staging"] += 1
    run.ledger.set_leg(leg, "poisoned", why_leg)
    run.ledger.unit_not_run(leg, "poisoned", why_units)
    run.log.warn("[%s] POISONED — %s; this leg's corpus is NOT run, the rest of the run CONTINUES:"
                 % (leg.label, headline))


def prepare_run_dir(run, leg, rundir, plan):
    """The DECLARED run directory on the launcher's own filesystem, cleared and created through the
    resolver's argv prefixes -> True when the corpus may run there. Never a fallback to this
    driver's own directory."""
    log = run.log
    launch_run = plan.get("launcherPath") or ""
    if not launch_run:
        log.info("[%s] the corpus runs in this driver's own filesystem (runFilesystem '%s') — %s"
                 % (leg.label, plan.get("runFilesystem"), rundir))
        return True
    log.info("[%s] the launcher writes onto ITS OWN filesystem (runFilesystem '%s') — the corpus runs "
             "in %s" % (leg.label, plan.get("runFilesystem"), launch_run))
    log.info("      NOT in %s, which that launcher reaches only through a compatibility mount whose "
             "POSIX file modes are synthesised from one host attribute." % rundir)
    ok, why = L.run_dir_argv(plan.get("rmTreeArgv"), [launch_run],
                             "clear the run directory %s" % launch_run)
    if ok:
        ok, why = L.run_dir_argv(plan.get("mkdirArgv"), [launch_run + "/" + L.SQLITE_TESTDIR_SUBDIR],
                                 "create the run directory %s" % launch_run)
    if ok:
        return True
    poison_unrun(run, leg, "the fixture BUILT (%s), but this leg's DECLARED run directory could not be "
                 "prepared, so its corpus was NOT run and this run covers NONE of its units. %s"
                 % (leg.fixture, why), "run directory preparation FAILED: %s" % why,
                 "could not prepare the declared run directory")
    log.warn("      %s" % why)
    return False


def loadext_verdict(run, leg, code, why):
    """ONE report -> ONE verdict: 0 staged (the corpus may run); 2 ENVIRONMENTAL (only reachable
    when the operator asked for the control arm; not counted); anything else `poisoned` and
    counted. -> True when the corpus may run."""
    if code == 0:
        return True
    if code == 2:
        run.ledger.marks_missing(leg, "its corpus is NOT run (the fixture DID build: %s)" % leg.fixture,
                                 "DSS_LOADEXT_HELPER=%s was requested and this host cannot provide that "
                                 "arm. %s" % (run.loadext_builder, why))
        run.ledger.unit_not_run(leg, "skipped-build-input-missing", why)
        return False
    poison_unrun(run, leg, "the fixture BUILT (%s), but this leg's loadext helper extension ('%s') could "
                 "not be staged, so its corpus was NOT run and this run covers NONE of its units. %s"
                 % (leg.fixture, leg.build.get("loadExtHelperName") or "<undeclared>", why),
                 "loadext helper staging FAILED: %s" % why, "loadext helper staging FAILED")
    run.log.warn("      %s" % why)
    return False


def carry_helper(run, leg, plan, staged):
    """Carry the staged helper into the launcher's OWN filesystem when the corpus runs there
    (through the resolver's copy argv; the source spelled in the launcher's namespace). A failed
    copy is `poisoned` and counted. -> True when the corpus may run."""
    launch_run = plan.get("launcherPath") or ""
    if not launch_run or not staged:
        return True
    dest = "%s/%s/%s" % (launch_run, L.SQLITE_TESTDIR_SUBDIR, os.path.basename(staged))
    ok, why = L.run_dir_argv(plan.get("copyArgv"),
                             [L.launch_path(run.resolver, L.path_verb(leg), staged), dest],
                             "copy the loadext helper into %s/%s" % (launch_run, L.SQLITE_TESTDIR_SUBDIR))
    if ok:
        run.log.info("[%s] loadext helper carried into the launcher's filesystem -> %s" % (leg.label, dest))
        return True
    poison_unrun(run, leg, "the fixture BUILT (%s) and its loadext helper was produced, but the helper "
                 "could not be carried into this leg's DECLARED run directory, so its corpus was NOT run. "
                 "%s" % (leg.fixture, why), "loadext helper transfer FAILED: %s" % why,
                 "the loadext helper could not reach the run directory")
    run.log.warn("      %s" % why)
    return False


def corroborate(run, leg, rundir):
    """The leg's confound rows CORROBORATED against its OWN run directory (which exists by now):
    the survivors REPLACE the by-name supply, the abort rows and the run-directory gating. -> the
    corroboration record (its `reportText` is printed by the caller)."""
    if run.cfg.confounds_override is None:
        for field in ("confoundsByName", "confoundsByEvidence", "executionEvidence"):
            if leg.d.get(field) is None:
                C.die("[%s] the resolved leg plan carries NO `%s` field. harness_legs.py emits it on "
                      "every planned leg, so its absence is a transport defect between the resolver "
                      "and this driver." % (leg.label, field))
    corr = L.run_dir_corroboration(run.resolver, leg, rundir, leg.d.get("confoundsByName") or [],
                                   leg.d.get("abortConfounds") or [])
    for key in ("confounds", "abortConfounds", "runDirectoryGating", "reportText"):
        if key not in corr:
            C.die("the corroboration does not carry the field this driver reads ('%s'; have: %s). That "
                  "is a contract break between harness_legs.py and this driver."
                  % (key, ", ".join(sorted(corr))))
    leg.d = dict(leg.d)
    leg.d["confoundsByName"] = list(corr["confounds"] or [])
    leg.d["abortConfounds"] = list(corr["abortConfounds"] or [])
    leg.d["runDirectoryGating"] = corr["runDirectoryGating"]
    return corr


def announce_supply(run, leg, patterns, override, corr):
    """Say which patterns are in force and WHY (the three kinds of empty supply read differently),
    then print the resolver's two accounts verbatim."""
    log = run.log
    evidence = [p for p in (leg.d.get("confoundsByEvidence") or []) if p]
    declared = len(leg.d.get("confoundRows") or [])
    if patterns:
        log.info("[%s] confound patterns in force (%d): %s%s"
                 % (leg.label, len(patterns), " ".join(patterns),
                    "   [operator DSS_CONFOUNDS — applied to EVERY leg]" if override is not None else
                    "   [EARNED on this leg — legs.json `confounds`, provenance per pattern]"))
    elif override is None and evidence:
        log.info("[%s] NO confound pattern excuses a failure BY NAME on this leg; its ARMED row(s) (%s) "
                 "excuse a failure only on clock evidence from that failure's own execution — see the "
                 "per-row account immediately below." % (leg.label, " ".join(evidence)))
    elif declared > 0:
        log.info("[%s] NO confound patterns IN FORCE, and this is NOT a catalogue that declares none: %d "
                 "row(s) ARE declared for this leg and every one of them was gated OFF for this run — "
                 "see the per-row account immediately below. Every failure here counts, and a "
                 "clock-family failure here reads as GENUINE." % (leg.label, declared))
    else:
        log.info("[%s] NO confound patterns: this leg's catalogue entry declares `confounds: []` (0 rows "
                 "declared), i.e. nothing has ever been measured as a non-DSS confound HERE, and a "
                 "confound must be EARNED per platform, never copied from a sibling leg. Every failure "
                 "here counts." % leg.label)
    V.print_confound_report(leg.label, "\n".join(leg.d.get("confoundReport") or []), log)
    V.print_confound_report(leg.label, str(corr.get("reportText") or ""), log)


def unit_leg(run, leg, ctx):
    log, cfg = run.log, run.cfg
    if not corpus_entry(run, leg):
        return
    verb = L.path_verb(leg)
    log.step("8/9  [%s] %s — %s (%s%s%s)"
             % (leg.label, leg.spec, cfg.corpus_label(), leg.run_mode,
                (": " + " ".join(leg.launcher)) if leg.launcher else "",
                ("; paths -> '%s' via '%s'" % (verb, " ".join(leg.run.get("pathTranslator") or [])))
                if verb != "none" else ""))
    rundir = os.path.join(run.leg_out(leg), "run")
    _rmtree(rundir)
    os.makedirs(rundir)
    plan = L.run_dir_plan(run.resolver, leg, rundir)
    leg.rundir_plan = plan
    if not prepare_run_dir(run, leg, rundir, plan):
        return
    kentry = [str(a) for a in plan.get("kernelEntryArgv") or []]
    launch_bin = L.launch_path(run.resolver, verb, leg.fixture)
    if verb != "none":
        log.info("[%s] the launcher addresses files in ANOTHER namespace (pathTranslation '%s' via "
                 "'%s') — fixture %s -> %s" % (leg.label, verb,
                                               " ".join(leg.run.get("pathTranslator") or []),
                                               leg.fixture, launch_bin))
    carrier = L.carrier_name(run.resolver, L.env_verb(leg))
    if carrier:
        log.info("[%s] the launcher does NOT inherit this driver's environment (envTransfer '%s') — "
                 "variables that are SET at spawn time cross via %s; candidates: %s"
                 % (leg.label, L.env_verb(leg), carrier,
                    " ".join(list(L.FORWARD_PLAIN) + list(L.FORWARD_PATHS) +
                             list(L.declared_env(leg)))))
    code, why, _cross, staged = stage_loadext(run, leg, rundir)
    if not loadext_verdict(run, leg, code, why):
        return
    if not carry_helper(run, leg, plan, staged):
        return
    corr = corroborate(run, leg, rundir)
    override = cfg.confounds_override
    patterns = V.confound_supply(leg, override)
    announce_supply(run, leg, patterns, override, corr)
    run_corpus(run, leg, ctx, rundir, plan, launch_bin, kentry, patterns)


def _sweep(run, leg, fixture, launch_bin, kentry, why, lr):
    """Kill leftovers of THIS fixture, natively and in the fixture's own kernel (each sweep WARNS
    what it did). Everything a sweep learnt is ALSO a hygiene fact of the leg, carried into its
    Step-9 verdict: a kill; a kill that did NOT take (the leftover may still hold this leg's
    files); and a sweep that could not LOOK at all (a stray fixture would go unnoticed) -- the
    last recorded once per place, not once per segment.
    ✔MEASURED 2026-09-22 (helper p4s6, driving the real `run_corpus`): flattening each Sweep with
    `list(...)` kept only its kills, so an unkillable leftover and a blind in-kernel sweep reached
    the log and never the verdict -- the leg read `PASS` with no hygiene."""
    sweeps = [("on this host", P.stop_our_fixtures(fixture, why, settle_s=0, log=run.log))]
    if kentry:
        sweeps.append(("inside the fixture's own kernel (via `%s`)" % " ".join(kentry),
                       P.stop_our_fixtures(launch_bin, why, launcher_prefix=kentry, settle_s=0,
                                           log=run.log)))
    for where, sw in sweeps:
        for pid in sw:
            lr.hygiene.append("%s — killed pid %s" % (why, pid))
        for pid, how in sw.failed:
            lr.hygiene.append("%s — FAILED to kill leftover pid %s %s (%s): it may still hold this "
                              "leg's files" % (why, pid, where, how))
        blind = "leftover-fixture sweep UNVERIFIED %s" % where
        if not sw.verified and not any(h.startswith(blind) for h in lr.hygiene):
            lr.hygiene.append("%s (%s; first at %s) — a stray fixture would go unnoticed"
                              % (blind, sw.why, why))


def _any_left(fixture, launch_bin, kentry):
    if P.our_fixture_pids(fixture).procs:
        return True
    return bool(kentry) and bool(P.our_fixture_pids(launch_bin, launcher_prefix=kentry).procs)


def run_one_segment(run, leg, seg, seglog, rundir, launcher, launch_bin, kentry, loader_dirs, base,
                    omit, lr, runner=None):
    """ONE fixture invocation: its environment built by the ONE builder (SQLITE_TEST_PATTERN_LIST
    only for a permutation segment; QUICKTEST_OMIT only when exclusions were asked for), every
    argument asserted to be in the launcher's namespace, the execution monitors armed on the
    emptied log BEFORE the fixture starts and stopped AFTER it -- even when the run raises.
    `runner` is `sqlite_launch.run_segment` (injectable)."""
    log, cfg = run.log, run.cfg
    runner = runner or L.run_segment
    verb = L.path_verb(leg)
    argv_rest = [seg.script] + ([seg.arg] if seg.arg else [])
    extra = {"SQLITE_TEST_PATTERN_LIST": " ".join(seg.patterns) if seg.patterns else None,
             "QUICKTEST_OMIT": omit}
    env = L.leg_launch_env(run.resolver, leg, base, loader_dirs,
                           tcl_library=leg.tcl_script_dir or None, extra=extra, log=log)
    # THE CHOKE POINT for the launcher's path namespace, in the FOREGROUND: every argument the
    # child receives, asserted before it is spawned.
    if leg.run_mode == "launched":
        L.assert_translated(run.resolver, verb, [launch_bin] + argv_rest)
    argv = L.launch_argv(launcher, launch_bin, argv_rest)
    mons = L.evidence_start(run.resolver, leg, seglog, cfg.segment_timeout,
                            cfg.confounds_override is not None, log)
    try:
        return runner(argv, rundir, env, seglog, cfg.segment_stall, cfg.segment_timeout,
                      cfg.kill_settle, P.kill_tree,
                      lambda: _sweep(run, leg, leg.fixture, launch_bin, kentry, "segment timeout", lr),
                      lambda: _any_left(leg.fixture, launch_bin, kentry))
    finally:
        L.evidence_stop(leg.label, mons, log)


def run_corpus(run, leg, ctx, rundir, plan, launch_bin, kentry, patterns, runner=None):
    """THE RESUME ENGINE over one leg's corpus, then the leg's verdict (`judge_leg`). `runner` is
    the segment runner (`sqlite_launch.run_segment` by default; injectable, so the REAL loop can be
    driven with scripted segment logs)."""
    log, cfg = run.log, run.cfg
    outd = run.leg_out(leg)
    verb = L.path_verb(leg)
    runlog = os.path.join(outd, "corpus.log")
    ledger_file = os.path.join(outd, "corpus-units.txt")
    scratch = os.path.join(outd, ".corpus")
    _rmtree(scratch)
    os.makedirs(scratch)
    corpus = ctx["corpus"]
    tier_perms = ctx["tier_perms"]
    prefixes = ctx["prefixes"]
    # ★ THE INVARIANT THE SEGMENT QUEUE RESTS ON: a segment's FIRST argument is always the .test
    # SCRIPT the fixture sources (its Tcl `$argv0`), and therefore a PATH -- translated into the
    # launcher's namespace HERE, once -- while every later argument is a bare Tcl word or a
    # tester.tcl flag and never one. Every `Segment(` below takes one of these two scripts.
    tier_script = L.launch_path(run.resolver, verb, ctx["test_file"])
    perm_script = L.launch_path(run.resolver, verb, os.path.join(ctx["testdir"], "permutations.test"))
    tier_name = os.path.basename(ctx["test_file"])
    # A SINGLE-FILE run (DSS_TEST_FILE): one segment sourcing one `.test` file, judged on its own terms --
    # its summary completes its file (`credit_single_file`) and an abort inside it has nothing to resume.
    single = bool(cfg.test_file)
    launcher = [str(a) for a in plan.get("launcher") or []] or list(leg.launcher)
    loader_dirs = [os.path.dirname(leg.tcl_lib), os.path.dirname(leg.z_lib)]
    base = ctx["base_env"]
    lr = LegRun()
    lr.hygiene += list(leg.hygiene)
    # A sweep that cannot LOOK -- natively or inside the fixture's kernel -- is recorded by
    # `_sweep` itself (it replaced a native-only enumeration here, which could not see the
    # fixture's own kernel).
    _sweep(run, leg, leg.fixture, launch_bin, kentry, "pre-corpus", lr)
    lr.hygiene += list(run.hygiene)
    queue = [Segment("tier", "", tier_name, None, tier_script, "")]
    seg_i = 0
    last_boundary = ""
    prev_zero_sig = ""
    omit = ",".join(cfg.tier_excludes) if cfg.tier_excludes else None
    while seg_i < len(queue):
        seg = queue[seg_i]
        if seg_i == 0:
            seglog = runlog
            log.info("[%s] running %s%s…" % (
                leg.label, cfg.corpus_label(),
                (" (under the declared launcher: %s%s)" % (" ".join(leg.launcher),
                                                           (", paths -> %s" % verb) if verb != "none"
                                                           else "")) if leg.launcher else ""))
        else:
            seglog = os.path.join(outd, "corpus.resume%d.log" % seg_i)
            log.info("[%s] segment %d: %s%s" % (
                leg.label, seg_i + 1, seg.label,
                ("  (SQLITE_TEST_PATTERN_LIST: %d candidate file(s))" % len(seg.patterns))
                if seg.patterns is not None else ""))
        res = run_one_segment(run, leg, seg, seglog, rundir, launcher, launch_bin, kentry,
                              loader_dirs, base, omit, lr, runner=runner)
        if res.kill_reason:
            log.warn("[%s] segment %d HUNG — killed: %s" % (leg.label, seg_i + 1, res.kill_reason))
            lr.hygiene.append("segment %d TIMED OUT and was killed — %s" % (seg_i + 1, res.kill_reason))
        _sweep(run, leg, leg.fixture, launch_bin, kentry, "after segment %d" % (seg_i + 1), lr)
        # ★ The four per-segment lists are appended TOGETHER, here, before any branch can leave
        # the loop (a PRECONDITION break once skipped one and crashed the reporter).
        lr.seg_logs.append(seglog)
        lr.seg_labels.append(seg.label)
        lr.seg_rcs.append(res.rc)
        lr.seg_counts.append("tests: (none counted) / errors: (none counted)   [this segment produced "
                             "no countable output — see its log]")
        facts = K.parse_segment(seglog)
        if single and seg.kind == "tier":
            K.credit_single_file(facts, tier_name)
        lr.facts.append(facts)
        lr.files_done += facts.n_files
        lr.files_inert += facts.n_inert
        lr.failures += list(facts.failures)
        derived = facts.derived_count
        seg_i += 1
        if facts.summary:
            lr.seg_summary = facts.summary
            e, c = facts.errors or 0, facts.total or 0
            lr.sum_errors += e
            lr.sum_tests += c
            lr.n_summarised += 1
            lr.seg_counts[-1] = ("tests: %s / errors: %d   [source: sqlite's own summary line; per-test "
                                 "derivation independently gives %s]"
                                 % (K.group_digits(c), e, K.group_digits(derived)))
            if derived != c:
                lr.calibration.append("%s: sqlite says %d tests, the per-test derivation says %d "
                                      "(delta %d)" % (seg.label, c, derived, derived - c))
            lr.total_errors += e
            lr.total_tests += c
            if facts.gave_up:
                log.warn("[%s] segment %d stopped EARLY at the --maxerror cap ('*** Giving up...') — "
                         "this is NOT full coverage" % (leg.label, seg_i))
                lr.note("", "every file after %s in '%s' — the fixture hit its --maxerror cap and "
                        "finalised early (raise it with --maxerror=N)"
                        % (facts.last_file or "(none)", seg.label))
            continue
        # ── ABORT ── first: is it an abort at all, or a PRECONDITION FAILURE?
        zero_sig = K.zero_progress_signature(facts)
        if K.is_precondition_failure(prev_zero_sig, facts):
            lr.precondition = zero_sig
            try:
                size = "%d byte(s)" % os.path.getsize(seglog)
            except OSError:
                size = "size unknown"
            log.warn("[%s] PRECONDITION FAILURE — the fixture completed ZERO test files in TWO "
                     "consecutive segments, both ending IDENTICALLY." % leg.label)
            log.warn("      This is NOT a resumable fixture crash: nothing the resume engine can do "
                     "changes it,")
            log.warn("      so the remaining %d resume(s) are NOT spent on it." %
                     (cfg.max_resumes - lr.resumes))
            log.warn("      what both segments ended with, verbatim, from %s:" % seglog)
            log.warn("        %s" % zero_sig)
            log.info("      first lines of that log (%s):" % size)
            for line in _head(seglog, 6):
                log.info("        %s" % line)
            lr.note("", "EVERY unit of the '%s' corpus — the fixture never completed a single file. "
                    "PRECONDITION FAILURE: %s" % (seg.perm or cfg.tier, zero_sig))
            break
        prev_zero_sig = zero_sig if facts.n_files == 0 else ""
        lr.der_tests += derived
        lr.der_errors += facts.fail_markers
        lr.n_derived += 1
        lr.total_tests += derived
        lr.total_errors += facts.fail_markers
        lr.seg_counts[-1] = ("tests: %s / errors: %d   [source: DERIVED from per-test lines — %s ' Ok' + "
                             "%d '! expected:' + 1; this segment aborted and printed no summary]"
                             % (K.group_digits(derived), facts.fail_markers,
                                K.group_digits(facts.ok), facts.fail_markers))
        ap = plan_abort(facts, seg, corpus, tier_perms, last_boundary)
        name = "%s/%s" % (ap.perm or "?", ap.abort_file or "?")
        lr.aborts.append(name)
        lr.abort_logs.append(seglog)
        how = ("KILLED: %s" % res.kill_reason) if res.kill_reason else "rc=%d" % res.rc
        lr.abort_rows.append("segment %d: permutation '%s' file '%s' after test '%s' (%s) -> %s"
                             % (seg_i, ap.perm or "?", ap.abort_file or "?",
                                facts.last_test or "?", how, seglog))
        if res.kill_reason:
            log.warn("[%s] ABORT #%d — segment %d ('%s') was KILLED after it %s"
                     % (leg.label, len(lr.aborts), seg_i, seg.label, res.kill_reason))
        else:
            log.warn("[%s] ABORT #%d — segment %d ('%s') exited rc=%d with NO summary line"
                     % (leg.label, len(lr.aborts), seg_i, seg.label, res.rc))
        log.info("        permutation        : %s%s" % (ap.perm or "(UNDETERMINED)",
                                                     ("   [%s]" % ap.perm_inferred) if ap.perm_inferred
                                                     else ""))
        log.info("        last file completed: %s" % (facts.last_file or "(none)"))
        log.info("        died inside file   : %s   last test: %s"
                 % (ap.abort_file or "(unresolved)", facts.last_test or "(none)"))
        log.info("        how it was named   : %s" % (ap.abort_source or "(could not be named — no "
                                                    "traceback frame and no resolvable test name)"))
        if single:
            lr.note(name, "the REMAINDER of %s — a single-file run (DSS_TEST_FILE) aborted inside its one "
                    "file (last test emitted: %s), and nothing is left to resume" % (tier_name,
                                                                         facts.last_test or "none"))
            for line in C.tail_file(seglog, 6).splitlines():
                log.info("      %s" % line)
            log.warn("[%s] a single-file run has nothing to resume: the abort inside %s IS its verdict."
                     % (leg.label, tier_name))
            continue
        if ap.abort_file:
            lr.note(name, "the REMAINDER of %s under permutation '%s' (%s; last test emitted: %s)"
                    % (ap.abort_file, ap.perm or "?", ap.abort_source or "source unrecorded",
                       facts.last_test or "none"))
        else:
            what = (("the resume boundary was FORCED to %s, so that one file may have been skipped "
                     "without a verdict" % (ap.boundary or "the end of the corpus")) if ap.forced else
                    ("the next segment resumes from %s and will RE-ATTEMPT it"
                     % (ap.boundary or "the end of the corpus")))
            lr.note("", "the UNNAMED file that aborted under permutation '%s' after %s — the log named "
                    "no resolvable corpus file (last test: %s; traceback frame: %s); %s"
                    % (ap.perm or "?", facts.last_file or "the start of the permutation",
                       facts.last_test or "none", facts.abort_file or "none", what))
        for line in C.tail_file(seglog, 6).splitlines():
            log.info("      %s" % line)
        if not ap.boundary:
            log.warn("[%s] the abort is at the END of the corpus file list — nothing left to resume."
                     % leg.label)
            continue
        if not ap.perm:
            log.warn("[%s] CANNOT RESUME — the aborting permutation could not be determined from the log."
                     % leg.label)
            lr.note("", "every unit after %s — no resume was possible (permutation undetermined; see %s)"
                    % (ap.boundary, seglog))
            continue
        perm_idx = tier_perms.index(ap.perm) if ap.perm in tier_perms else -1
        if lr.resumes >= cfg.max_resumes:
            log.warn("[%s] RESUME BUDGET EXHAUSTED (%d) — stopping. Raise DSS_MAX_RESUMES to go further."
                     % (leg.label, cfg.max_resumes))
            rest = ""
            if 0 <= perm_idx < len(tier_perms) - 1:
                rest = " and every permutation after '%s' (%s)" % (ap.perm,
                                                                  " ".join(tier_perms[perm_idx + 1:]))
            lr.note("", "every unit after %s in '%s'%s — resume budget (%d) exhausted"
                    % (ap.boundary, ap.perm, rest, cfg.max_resumes))
            continue
        lr.resumes += 1
        last_boundary = ap.boundary
        after = K.files_after(ap.boundary, corpus)
        with open(os.path.join(scratch, "after.%d" % lr.resumes), "w", encoding="utf-8",
                  newline="\n") as fh:
            fh.write("".join("%s\n" % f for f in after))
        log.info("        -> resume %d/%d: permutations.test %s, corpus files after %s"
                 % (lr.resumes, cfg.max_resumes, ap.perm, ap.boundary))
        tail = [Segment("perm", ap.perm, "permutations.test %s (after %s)" % (ap.perm, ap.boundary),
                        list(after), perm_script, ap.perm)]
        if seg.kind != "perm":
            if perm_idx < 0:
                log.warn("[%s] permutation '%s' is not named by %s — cannot continue the tier past it."
                         % (leg.label, ap.perm, tier_name))
                lr.note("", "every permutation after '%s' in %s — '%s' is not one of its run_test_suite "
                        "entries" % (ap.perm, tier_name, ap.perm))
            elif perm_idx < len(tier_perms) - 1:
                nxt = tier_perms[perm_idx + 1]
                tail.append(Segment("tier", nxt, "%s --start=%s:" % (tier_name, nxt), None,
                                    tier_script, "--start=%s:" % nxt))
                log.info("        -> then: %s --start=%s:  (permutations %s..%s)"
                         % (tier_name, nxt, nxt, tier_perms[-1]))
        queue[seg_i:seg_i] = tail
    judge_leg(run, leg, ctx, lr, patterns, runlog, ledger_file)


def _head(path, n):
    out = []
    try:
        with open(path, "rb") as fh:
            for raw in fh:
                out.append(raw.decode("utf-8", "replace").rstrip("\r\n"))
                if len(out) >= n:
                    break
    except OSError:
        pass
    return out


# ── the verdict of one leg ────────────────────────────────────────────────────────────

def judge_leg(run, leg, ctx, lr, patterns, runlog, ledger_file):
    log, cfg = run.log, run.cfg
    nseg = len(lr.seg_logs)
    if not (len(lr.seg_counts) == len(lr.seg_labels) == len(lr.seg_rcs) == len(lr.facts) == nseg):
        C.die("[%s] INTERNAL: the per-segment lists are not index-parallel — logs=%d labels=%d rcs=%d "
              "counts=%d facts=%d." % (leg.label, nseg, len(lr.seg_labels), len(lr.seg_rcs),
                                       len(lr.seg_counts), len(lr.facts)))
    union = "%d errors out of %s tests (union of %d segment(s))" % (
        lr.total_errors, K.group_digits(lr.total_tests), nseg)
    summary = lr.seg_summary if nseg == 1 else union
    derivation = ""
    if lr.n_derived > 0:
        derivation = ("%d segment summary/summaries: %s test(s), %d error(s) · %d ABORTED segment(s): %s "
                      "test(s), %d error(s) counted from per-test lines (' Ok' + '! <name> expected:' + "
                      "1 — an aborted segment prints no summary)"
                      % (lr.n_summarised, K.group_digits(lr.sum_tests), lr.sum_errors, lr.n_derived,
                         K.group_digits(lr.der_tests), lr.der_errors))
        if lr.calibration:
            derivation += ("  [!! the derivation DISAGREES with sqlite on %d segment(s) that did report — "
                           "treat the aborted-segment figures as APPROXIMATE: %s]"
                           % (len(lr.calibration), " ".join(lr.calibration)))
    mode = V.leg_mode(leg)
    cls = V.classify(lr.failures, patterns, ctx["prefixes"], mode)
    real, confound = list(cls.real), list(cls.confound)
    V.warn_scoped(leg.label, cls.scoped, mode, log)
    evidence_excused = []
    if real and cfg.confounds_override is None and leg.d.get("confoundsByEvidence"):
        excused = L.evidence_attribute(run.resolver, leg, mode, lr.seg_logs, ctx["prefixes"], real,
                                       False, log)
        keep = []
        for t in real:
            if t in excused:
                confound.append(t)
                evidence_excused.append(t)
            else:
                keep.append(t)
        real = keep
    if evidence_excused:
        log.info("[%s] %d failure(s) excused PER FAILURE on clock evidence recorded inside their own "
                 "execution: %s" % (leg.label, len(evidence_excused), " ".join(evidence_excused)))
    write_ledger(run, leg, lr, summary, derivation, ledger_file)
    # ── did the DECLARED capabilities reach the tests? ──
    witnesses = ctx["witnesses"]
    if witnesses and nseg > 0:
        ran = [f for fx in lr.facts for f in fx.files]
        inert = [f for fx in lr.facts for f in fx.inert]
        w = V.witness_check(witnesses, ran, inert)
        if w.absent and cfg.test_file:
            log.info("[%s] a single-file run (DSS_TEST_FILE) proves nothing about the %d capability witness(es) "
                     "its file is not: %s" % (leg.label, len(w.absent), " ".join(w.absent)))
        elif w.absent:
            log.warn("[%s] %d of %d capability witness(es) were NOT IN THIS RUN'S CORPUS, so nothing was "
                     "proved about them: %s\n      A witness file that never appears is not a passing "
                     "witness. Either the tier does not include\n      it, or the corpus this leg was "
                     "handed is missing the directory it lives in."
                     % (leg.label, w.declared - w.checked, w.declared, " ".join(w.absent)))
        if w.gaps:
            run.capability_gaps.append("%s: %s" % (leg.label, " ".join(w.gaps)))
            log.warn("[%s] DECLARED CAPABILITIES DID NOT REACH THE TESTS — %s" % (leg.label,
                                                                             " ".join(w.gaps)))
            log.warn("      Each of those files ran to completion and asserted NOTHING: it returned at "
                     "its")
            log.warn("      `ifcapable` gate. The define reached the compiler (Step 4 proved that), so "
                     "the")
            log.warn("      library was built without the capability the flag was supposed to enable, "
                     "or the")
            log.warn("      fixture linked objects from an older configuration. Reported at the end of "
                     "the run.")
        else:
            log.ok("[%s] every capability witness that was IN THIS CORPUS reached the tests — %d of %d "
                   "declared (witnesses: %s)" % (leg.label, w.checked, w.declared,
                                                 " ".join("%s -> %s" % cw for cw in witnesses)))
    # ── EARNED vs UNEARNED aborts ──
    earned, unearned, provenance = [], [], []
    for i, a in enumerate(lr.aborts):
        ok, text = classify_abort(run, leg, a, lr.abort_logs[i] if i < len(lr.abort_logs) else "")
        if ok:
            earned.append(a)
            provenance.append((a, text))
        else:
            unearned.append(a)
    for a, text in provenance:
        log.warn("[%s] ABORT %s — PROVEN NOT DSS's, and it still cost the rest of that file:"
                 % (leg.label, a))
        for line in text.splitlines():
            if line.strip():
                log.info("        %s" % line)
    faillist = sorted(set(f for f in lr.failures if f))
    fail = True
    if lr.precondition:
        verdict = ("FAIL:PRECONDITION FAILURE — THE FIXTURE NEVER STARTED: it completed ZERO test files "
                   "in %d consecutive segment(s), each ending the same way: %s  (this is not a "
                   "resumable crash; the remaining resume budget was NOT spent on it — see %s)"
                   % (nseg, lr.precondition, runlog))
        log.warn("[%s] corpus FAIL — PRECONDITION FAILURE, no unit of this leg's corpus ever ran."
                 % leg.label)
        log.warn("      %s" % lr.precondition)
        log.info("      %d segment(s), %d test file(s) completed (%d of them asserted NOTHING), %d of %d "
                 "resume(s) used." % (nseg, lr.files_done, lr.files_inert, lr.resumes, cfg.max_resumes))
        log.info("      per-unit ledger: %s" % ledger_file)
    elif unearned:
        verdict = ("FAIL:%d fixture ABORT(s) [%s]; recovered by %d resume(s); union: %s"
                   % (len(unearned), " ".join(unearned), lr.resumes, union))
        if derivation:
            verdict += " [%s]" % derivation
        if real:
            verdict += "; %d genuine unit failure(s): %s" % (len(real), " ".join(real))
        if lr.not_reached:
            verdict += "; %d unit group(s) NOT REACHED — see %s" % (len(lr.not_reached), ledger_file)
        log.warn("[%s] corpus FAIL — %d UNEARNED abort(s): %s%s"
                 % (leg.label, len(unearned), " ".join(unearned),
                    ("   (plus %d PROVEN-not-DSS abort(s), reported above and NOT charged: %s)"
                     % (len(earned), " ".join(earned))) if earned else ""))
        log.info("      union across %d segment(s): %s; %d test file(s) completed (%d of them asserted "
                 "NOTHING)" % (nseg, union, lr.files_done, lr.files_inert))
        if derivation:
            log.info("        derived from: %s" % derivation)
        if real:
            log.info("      %d UNCLASSIFIED failure(s) — not matched by any earned confound, NOT yet "
                     "attributed to DSS: %s" % (len(real), " ".join(real)))
        for n in lr.not_reached:
            log.warn("      NOT REACHED: %s" % n.text)
        log.info("      per-unit ledger: %s" % ledger_file)
    elif not summary:
        verdict = "FAIL:fixture did not complete the suite (crash?) — see %s" % runlog
        log.warn("[%s] corpus FAIL — no summary line (fixture crashed mid-suite); tail:" % leg.label)
        for line in C.tail_file(runlog, 4).splitlines():
            log.info("      %s" % line)
    elif lr.files_done == 0:
        verdict = ("FAIL:the fixture completed ZERO test files yet printed a summary (%s) — a suite that "
                   "ran nothing is not a pass; see %s" % (summary, runlog))
        log.warn("[%s] corpus FAIL — 0 test file(s) completed, though the fixture printed '%s'."
                 % (leg.label, summary))
        log.info("      A tier that selects no files still finalises and reports a summary. That is not "
                 "a run.")
        for line in C.tail_file(runlog, 4).splitlines():
            log.info("      %s" % line)
    elif cfg.test_file and lr.files_inert >= lr.files_done:
        verdict = ("FAIL:the single file %s asserted NOTHING (%s) — every result it printed was the "
                   "harness's own teardown, so it returned at an `ifcapable` gate; a run that asserted "
                   "nothing is not a pass; see %s" % (os.path.basename(cfg.test_file), summary, runlog))
        log.warn("[%s] corpus FAIL — the single file asserted NOTHING, though the fixture printed '%s'."
                 % (leg.label, summary))
    elif lr.total_errors > 0 and not faillist:
        verdict = ("FAIL:%d error(s) but no failure markers ('Failures on these tests:' / '! <name>') to "
                   "classify — see %s" % (lr.total_errors, runlog))
        log.warn("[%s] corpus FAIL — %s (unclassifiable — no failure markers)" % (leg.label, summary))
    elif not real:
        fail = False
        if not confound:
            verdict = "PASS (%s)" % summary
            log.ok("[%s] corpus GREEN — %s" % (leg.label, summary))
        else:
            verdict = "PASS (%s; %d known non-DSS confound(s): %s)" % (summary, len(confound),
                                                                     " ".join(confound))
            log.ok("[%s] corpus GREEN — %s; all %d failure(s) are known non-DSS confounds: %s"
                   % (leg.label, summary, len(confound), " ".join(confound)))
    else:
        verdict = "FAIL:%d genuine unit failure(s): %s" % (len(real), " ".join(real))
        log.warn("[%s] corpus FAIL — %s; %d UNCLASSIFIED failure(s) — run each against the gcc "
                 "reference fixture before charging it to DSS: %s"
                 % (leg.label, summary, len(real), " ".join(real)))
        if confound:
            log.info("      (+%d known confound(s) ignored: %s)" % (len(confound), " ".join(confound)))
    if fail:
        run.counts["unit"] += 1
        L.registry_controls(run.resolver, run.registry_glob, leg.label, real, log)
    if lr.hygiene:
        verdict += "  [PROCESS HYGIENE: %d event(s) — %s]" % (len(lr.hygiene), " ".join(lr.hygiene))
        for h in lr.hygiene:
            log.warn("[%s] HYGIENE: %s" % (leg.label, h))
    # A NOT-REACHED group is a coverage hole even when nothing failed. An EARNED abort accounts
    # for exactly ONE group -- the remainder of the file it died in -- and nothing else.
    if lr.not_reached and not unearned:
        unaccounted = [n for n in lr.not_reached if not (n.owner and n.owner in earned)]
        verdict += "  [NOT FULL COVERAGE: %d unit group(s) NOT REACHED — see %s]" % (
            len(lr.not_reached), ledger_file)
        if unaccounted:
            if not fail:
                run.counts["unit"] += 1
                fail = True
        else:
            log.info("[%s] the %d NOT-REACHED group(s) are each the remainder of a PROVEN-not-DSS abort, "
                     "so they do not fail this leg — they are still a coverage hole and are named below."
                     % (leg.label, len(lr.not_reached)))
        for n in lr.not_reached:
            log.warn("[%s] NOT REACHED: %s" % (leg.label, n.text))
    leg.unit_fail = fail
    leg.unit_verdict = verdict
    leg.unit_report = {"segments": nseg, "resumes": lr.resumes, "files_done": lr.files_done,
                       "files_inert": lr.files_inert, "ledger": ledger_file, "aborts": list(lr.aborts),
                       "earned": list(earned), "not_reached": [n.text for n in lr.not_reached],
                       "hygiene": list(lr.hygiene)}
    # `ran` whether GREEN or RED: a failing assertion is still an assertion.
    run.ledger.set_leg(leg, "ran", verdict)


def write_ledger(run, leg, lr, summary, derivation, path):
    cfg = run.cfg
    nseg = len(lr.seg_logs)
    lines = ["sqlite unit ledger — leg '%s', tier '%s', %d segment(s), %d resume(s)"
             % (leg.label, cfg.tier, nseg, lr.resumes), "union: %s" % summary]
    if derivation:
        lines.append("   derived from: %s" % derivation)
    for k in range(nseg):
        fx = lr.facts[k]
        lines += ["", "== segment: %s   rc=%s   %s" % (lr.seg_labels[k], lr.seg_rcs[k],
                                                     fx.summary or "ABORTED (no summary line)"),
                  "   log: %s" % lr.seg_logs[k], "   %s" % lr.seg_counts[k],
                  "   files completed (%d): %s" % (fx.n_files, " ".join(fx.files)),
                  "   of those, files that ASSERTED NOTHING (%d): %s" % (fx.n_inert, " ".join(fx.inert))]
        fails = sorted(set(fx.failures))
        if fails:
            lines.append("   failing test(s) seen here: %s" % " ".join(fails))
    for title, rows in (("derivation calibration MISMATCH", lr.calibration), ("aborts", lr.abort_rows),
                        ("NOT REACHED (no verdict)", [n.text for n in lr.not_reached]),
                        ("process hygiene", lr.hygiene)):
        if rows:
            lines += ["", "== %s ==" % title] + ["   %s" % r for r in rows]
    if cfg.tier_excludes:
        lines += ["", "== EXCLUDED by operator (DSS_TIER_EXCLUDES -> QUICKTEST_OMIT) ==",
                  "   %s" % " ".join(cfg.tier_excludes)]
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


# ── the step ──────────────────────────────────────────────────────────────────────────

def step8(run):
    log, cfg, st = run.log, run.cfg, run.stage
    log.step("8/9  Run SQLite unit corpus (%s) on each leg + classify failures" % cfg.corpus_label())
    if run.clone_lock is not None:
        run.clone_lock.read("build_and_test.py corpus run — tier %s, legs %s"
                            % (cfg.tier, " ".join(lg.label for lg in run.selected())), log)
        log.info("clone lock: downgraded to READ for the corpus run")
    testdir = _field(st, "testdir")
    test_file = cfg.test_file or os.path.join(testdir, "%s.test" % cfg.tier)
    if not os.path.isfile(test_file):
        C.die("test file not found: %s (tier '%s')" % (test_file, cfg.tier))
    corpus_dir = os.path.dirname(os.path.abspath(test_file)) if cfg.test_file else testdir
    if cfg.tier_excludes:
        log.warn("tier EXCLUSIONS active — this is NOT full-corpus coverage")
        log.info("      QUICKTEST_OMIT=%s  (sqlite's own hook, test/permutations.test)"
                 % ",".join(cfg.tier_excludes))
        log.info("      drops these file(s) from every $allquicktests-derived permutation (still run "
                 "under 'full'): %s" % " ".join(cfg.tier_excludes))
    ctx = {
        "test_file": test_file,
        "testdir": corpus_dir,
        "corpus": K.corpus_files(corpus_dir),
        "tier_perms": K.tier_permutations(test_file),
        "prefixes": K.tier_prefixes(os.path.join(corpus_dir, "permutations.test")),
        "witnesses": V.parse_witnesses(run.stage_build.get("capabilityWitnesses") or {}),
        "base_env": dict(os.environ),
    }
    for leg in run.selected():
        unit_leg(run, leg, ctx)
