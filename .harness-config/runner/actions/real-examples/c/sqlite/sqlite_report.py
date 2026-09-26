#!/usr/bin/env python3
"""sqlite_report.py -- Step 9 of the SQLite corpus harness: what this run PROVED, and its exit code.

  * PROVENANCE: the compiler with the checkout HEAD it came from, how far the built sources sit
    from that HEAD, the binary's build type, build time, ORIGIN and the config tree it was PROVED
    against (a `<path> @ <head>` line alone printed the same whether the binary was built a
    minute ago from this commit or four cycles back from another);
  * the ORACLE per leg, DERIVED by the resolver (`--oracle-report`) from the reference fixture's
    MEASURED target against the leg's declared spec -- a leg whose reference is another
    platform's binary reports NO ORACLE, in those terms; the report's exit code is CHECKED;
  * one line per declared leg for BOTH artefacts (the fixture and the sqlite3 CLI), the verdict
    counts in `ArmVerdictLedger::renderCountsLine()`'s words (classes read from the resolver),
    one reason line per non-verified leg;
  * the EXIT: every failure class collected and printed (the union of both old drivers' reasons),
    then exit 1; a capability gap is judged BEFORE the green lines (the PowerShell driver printed
    GREEN and then died); success prints the closing claim bounded by the ledger, carrying the
    phrase `declared leg(s) VERIFIED` the action's success pattern matches.

The union of `build-and-test.sh` and `build-and-test.ps1` Step 9 (lane mig, part 4, 2026-09-21).
Nothing runs at import.
"""
from __future__ import annotations

import os
import sys

import sqlite_common as C

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass
sys.dont_write_bytecode = True

import sqlite_smoke as S         # noqa: E402
import sqlite_verdicts as V      # noqa: E402

ENVIRONMENTAL = "environmental"


def _field(obj, name, default=None):
    if obj is None:
        return default
    if isinstance(obj, dict):
        return obj.get(name, default)
    return getattr(obj, name, default)


def _count_lines(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return sum(1 for ln in fh if ln.strip())
    except (OSError, TypeError):
        return 0


def provenance(run):
    log, st, comp = run.log, run.stage, run.compiler
    pv = run.provenance
    log.info("compiler : %s @ %s%s%s" % (comp.path, pv.get("head_short", "UNKNOWN"),
                                         pv.get("diverge_note", ""), comp.build_type_note))
    log.info("  binary : built %s — %s" % (comp.built, comp.origin))
    log.info("  config : %s" % run.currency_note)
    log.info("sqlite   : %s @ %s%s" % (run.sqlite_dir_posix, _field(st, "sqlite_head", "UNKNOWN"),
                                       ("   (staged: %s)" % run.stage_dir) if run.stage_dir else ""))
    fx, cli = _field(st, "fixture_recipe"), _field(st, "cli_recipe")
    log.info("recipe   : %d TUs, %d defines" % (_count_lines(_field(fx, "tus")),
                                                _count_lines(_field(fx, "defines"))))
    log.info("cli recipe: %d TUs, %d defines" % (_count_lines(_field(cli, "tus")),
                                                 _count_lines(_field(cli, "defines"))))
    ref_cli = _field(st, "reference_cli")
    if ref_cli and os.path.isfile(ref_cli):
        log.info("cli oracle: %s  (gcc `make sqlite3d` — the SAME %d TUs from the SAME staged tree; the "
                 "compiler and the -D split differ)" % (ref_cli, _count_lines(_field(cli, "tus"))))
    else:
        log.warn("cli oracle: ABSENT — no smoke failure this run could be attributed. %s"
                 % (_field(st, "reference_cli_why") or ""))


def oracle_lines(run, reasons):
    log, st = run.log, run.stage
    ref = _field(st, "reference_fixture") or ""
    target = ""
    if ref and os.path.isfile(ref):
        target, why = S.identify_binary(run.resolver, ref)
        if not target:
            log.warn("the reference testfixture could not be IDENTIFIED — %s" % why)
            log.warn("      Every leg therefore reports NO ORACLE: a control whose platform is unknown is "
                     "not a control.")
    else:
        log.warn("oracle   : no run reference survived this run. %s"
                 % (_field(st, "reference_fixture_why") or ""))
        ref = ""
    for leg in run.legs:
        o = leg.oracle or {}
        r = run.resolver.call(["--oracle-report", leg.label, "--reference-target", target,
                               "--reference-path", ref, "--leg-oracle", o.get("path", ""),
                               "--leg-oracle-cc", o.get("cc", ""),
                               "--leg-oracle-triple", o.get("triple", ""),
                               "--oracle-status", o.get("status", "")])
        for line in ((r.out or "") + (r.err or "")).splitlines():
            if line.strip():
                log.info("oracle   : %s" % line.rstrip())
        if r.rc != 0:
            reasons.append("the oracle report for leg %s could not be produced (harness_legs.py "
                           "--oracle-report, rc=%d) — a report this run cannot write is a harness "
                           "defect, never a clean line" % (leg.label, r.rc))
        if leg.build_attribution:
            log.info("oracle   : %s build attribution: %s" % (leg.label, leg.build_attribution))


def leg_lines(run):
    log, cfg = run.log, run.cfg
    excl = ("  [NOT FULL COVERAGE: %d file pattern(s) EXCLUDED from the %s tier via QUICKTEST_OMIT -- %s]"
            % (len(cfg.tier_excludes), cfg.tier, " ".join(cfg.tier_excludes))) if cfg.tier_excludes else ""
    log.info("corpus   : %s   outputs: %s" % (cfg.corpus_label(), run.out_dir))
    log.info("excluded : %s" % (("%s   (operator DSS_TIER_EXCLUDES -> QUICKTEST_OMIT; dropped from every "
                                  "$allquicktests-derived permutation, still run under 'full')"
                                  % " ".join(cfg.tier_excludes)) if cfg.tier_excludes
                                 else "(none — the full tier ran)"))
    for leg in run.legs:
        rep = leg.unit_report if isinstance(leg.unit_report, dict) else {}
        if leg.fixture_built:
            if rep and (rep.get("segments", 1) > 1 or rep.get("aborts") or rep.get("not_reached")
                        or rep.get("hygiene")):
                log.info("%-14s segments : %s (%s resume(s) of max %d)   %s test file(s) completed (%s "
                         "asserted NOTHING)   ledger: %s"
                         % (leg.label, rep.get("segments"), rep.get("resumes"), cfg.max_resumes,
                            rep.get("files_done"), rep.get("files_inert"), rep.get("ledger")))
                for a in rep.get("aborts") or []:
                    if a in (rep.get("earned") or []):
                        log.info("%-14s aborted  : %s — PROVEN NOT DSS (earned `matches: abort-file` row); "
                                 "NOT charged, and its remaining cases still did NOT run" % (leg.label, a))
                    else:
                        log.info("%-14s aborted  : %s — its remaining cases did NOT run" % (leg.label, a))
                for n in rep.get("not_reached") or []:
                    log.info("%-14s NOT RUN  : %s" % (leg.label, n))
                for h in rep.get("hygiene") or []:
                    log.info("%-14s hygiene  : %s" % (leg.label, h))
            log.info("%-14s (%s): compiled   units: %s%s" % (leg.label, leg.spec,
                                                           leg.unit_verdict or "-", excl))
        elif leg.verdict == "poisoned":
            plog = os.path.join(run.leg_out(leg), "compile.log")
            if os.path.isfile(plog):
                log.info("%-14s (%s): COMPILE FAILED   see %s — %s" % (leg.label, leg.spec, plog,
                                                                     leg.verdict_detail))
            else:
                log.info("%-14s (%s): POISONED [no compile was attempted] — %s"
                         % (leg.label, leg.spec, leg.verdict_detail or "<no reason recorded>"))
        else:
            log.info("%-14s (%s): NOT BUILT [%s] — %s" % (leg.label, leg.spec, leg.verdict or "<NO VERDICT>",
                                                        leg.verdict_detail or "<no reason recorded>"))


def cli_lines(run):
    log = run.log
    cli = _field(run.stage, "cli_recipe")
    log.info("--- sqlite3 CLI (full TU: shell.c + the %d library TUs recovered from libsqlite3.a) ---"
             % max(0, _count_lines(_field(cli, "tus")) - 1))
    selected = {lg.label for lg in run.selected()}
    for leg in run.legs:
        got = run.artifacts.get(leg.label, "sqlite3")
        if leg.cli_bin:
            log.info("%-14s (%s): built   smoke: %s" % (leg.label, leg.spec,
                                                      leg.smoke_verdict or "<NO SMOKE VERDICT>"))
        elif got is None:
            if leg.label in selected:
                log.warn("%-14s (%s): ★ NO CLI VERDICT — this leg WAS selected and the CLI loop still "
                         "recorded nothing for it. That is a harness bug; see the ledger check below."
                         % (leg.label, leg.spec))
            else:
                log.info("%-14s (%s): not processed [not-selected-by-runner] — DSS_LEGS='%s' did not "
                         "select this leg" % (leg.label, leg.spec, C.env("DSS_LEGS")))
        else:
            log.info("%-14s (%s): NOT BUILT [%s] — %s" % (leg.label, leg.spec, got[0], got[1]))
    return [lg.label for lg in run.selected() if run.artifacts.get(lg.label, "sqlite3") is None]


def step9(run):
    """-> the process exit code (0 or 1). Prints every failure reason before exiting 1."""
    log, cfg = run.log, run.cfg
    log.step("9/9  Results")
    reasons = []
    provenance(run)
    oracle_lines(run, reasons)
    leg_lines(run)
    cli_holes = cli_lines(run)
    classes = V.verdict_classes(run.resolver)
    lc = V.ledger_counts(run.legs, classes)
    line = V.counts_line(lc)
    log.info(line)
    if lc.accounted != lc.total:
        log.warn("★ LEDGER ACCOUNTING HOLE: %d of %d declared legs fall in a reported class — the rest "
                 "belong to NO class and have VANISHED from the line above" % (lc.accounted, lc.total))
        if lc.unnamed:
            log.warn("  with NO verdict at all : %s" % " ".join(lc.unnamed))
        if lc.bogus:
            log.warn("  with a verdict OUTSIDE the closed vocabulary: %s" % " ".join(lc.bogus))
        log.warn("  the closed vocabulary is: %s" % " ".join(classes))
    for leg in run.legs:
        if classes.get(leg.verdict) == "verified":
            continue
        log.info("[%s] %s spec=%s%s — %s" % (leg.verdict or "<NO VERDICT>", leg.label, leg.spec,
                                            " (BUILT)" if leg.fixture_built else "",
                                            leg.verdict_detail or "<no reason recorded>"))
    filtered = [lg.label for lg in run.legs if not lg.selected]
    if filtered:
        log.warn("coverage : DSS_LEGS/DSS_RUN_FIDELITY restricted this run to %d of %d declared legs — "
                 "NOT EXERCISED: %s" % (len(run.selected()), len(run.legs), " ".join(filtered)))
    env_skips = [lg.label for lg in run.legs if classes.get(lg.verdict) == ENVIRONMENTAL]
    if env_skips and not cfg.strict:
        log.warn("%d leg(s) were skipped for an ENVIRONMENTAL reason — this machine could not supply a "
                 "DECLARED input: %s" % (len(env_skips), " ".join(env_skips)))
        log.warn("      Those targets are NOT covered by this run. Set DSS_STRICT_ARM_VERDICTS=1 to make "
                 "it a hard failure.")
    # ── every failure class, in the old ladder's order ──
    if run.ledger.unclassified:
        reasons.append("%d leg(s) had their UNIT CORPUS skipped with a verdict token this driver could not "
                       "classify: %s. Each one is warned above with what the driver DID say and the run "
                       "mode the resolver planned for it — a HARNESS defect, not a compiler result. The "
                       "closed vocabulary is: %s" % (len(run.ledger.unclassified),
                                                     " ".join(run.ledger.unclassified),
                                                     " ".join(run.ledger.vocabulary)))
    if lc.accounted != lc.total:
        reasons.append("THE LEDGER DOES NOT ADD UP — a declared leg reached no named verdict (%d of %d "
                       "accounted). That is a HARNESS defect, not a compiler result."
                       % (lc.accounted, lc.total))
    if cli_holes:
        reasons.append("THE sqlite3 CLI LEDGER DOES NOT ADD UP — no CLI verdict was ever recorded for the "
                       "SELECTED leg(s): %s. Every selected leg must reach a named CLI verdict (built / "
                       "poisoned / skipped-build-input-missing)." % " ".join(cli_holes))
    if run.counts["compile"]:
        reasons.append("%d leg(s) failed to compile the testfixture — inspect the compile.log "
                       "diagnostics." % run.counts["compile"])
    if run.counts["staging"]:
        reasons.append("%d leg(s) BUILT their testfixture but could not stage the loadext helper the corpus "
                       "dlopen()s (or prepare its run directory) — their units did NOT run. Each one is "
                       "named [poisoned] above with the exact reason." % run.counts["staging"])
    if run.counts["cli"]:
        reasons.append("%d leg(s) did not produce a sqlite3 CLI — each one's reason is on its CLI ledger "
                       "line above; where a compile was attempted the diagnostics are in "
                       "%s/<leg>/cli/compile.log." % (run.counts["cli"], run.out_dir))
    if run.selected() and not [lg for lg in run.selected() if lg.cli_bin]:
        reasons.append("NOT ONE sqlite3 CLI was built, on any of the %d selected leg(s). A zero-artefact run "
                       "proves nothing and must not exit 0." % len(run.selected()))
    if run.counts["smoke"]:
        reasons.append("%d leg(s) failed the sqlite3 CLI smoke gate — inspect %s/<leg>/cli-smoke/smoke.log. "
                       "Exonerated is still red: it names WHO is at fault, not that it passed."
                       % (run.counts["smoke"], run.out_dir))
    unit_fail = [lg.label for lg in run.legs if lg.unit_fail]
    if unit_fail:
        reasons.append("%d leg(s) had genuine unit failures (non-confound) — the corpus is not green: %s"
                       % (len(unit_fail), " ".join(unit_fail)))
    poisoned = [lg.label for lg in run.legs if lg.verdict == "poisoned"]
    if poisoned:
        reasons.append("%d leg(s) POISONED: %s — `poisoned` is the closed vocabulary's FAILURE class (\"no "
                       "artifact was exercised and the reason is OURS\")." % (len(poisoned),
                                                                             " ".join(poisoned)))
    if env_skips and cfg.strict:
        reasons.append("%d ENVIRONMENTAL skip(s) and DSS_STRICT_ARM_VERDICTS=1: %s — each is a DECLARED "
                       "input this machine could not supply." % (len(env_skips), " ".join(env_skips)))
    if lc.verified == 0:
        if lc.environmental > 0:
            reasons.append("NO declared leg reached a VERIFIED verdict, and %d non-verification(s) are "
                           "ENVIRONMENTAL — this machine could have produced evidence and did not."
                           % lc.environmental)
        else:
            log.warn("no declared leg reached a VERIFIED verdict on this host — every non-verification is "
                     "STRUCTURAL. Builds were still attempted; this run proves nothing about EXECUTION on "
                     "any target.")
    if run.capability_gaps:
        for g in run.capability_gaps:
            log.warn("capability gap — %s" % g)
        reasons.append("the run built a sqlite that does NOT have capabilities this harness declares. Every "
                       "gap above is a test file that completed and asserted nothing, for a capability "
                       "legs.json stageBuild names explicitly; the corpus totals above OVERSTATE coverage.")
    if reasons:
        print("", flush=True)
        for r in reasons:
            log.warn("✗ %s" % r)
        log.warn("✗ sqlite harness FAILED — %s" % line)
        return 1
    log.ok("%d of %d declared leg(s) VERIFIED: compiled the full-source testfixture + ran the unit corpus %s "
           "GREEN — SQLite units pass with dsscp.  (%d skipped: %d structural, %d environmental, %d harness "
           "— each named above; %d poisoned)"
           % (lc.verified, lc.total, cfg.corpus_label(), lc.skipped, lc.structural, lc.environmental,
              lc.harness,
              lc.failed))
    built = [lg for lg in run.legs if lg.cli_bin]
    smoked = [lg for lg in built if (lg.smoke_verdict or "").startswith("PASS")]
    cli = _field(run.stage, "cli_recipe")
    log.ok("sqlite3 CLI: BUILT on %d of %d declared leg(s) from %d full-source TUs; the 14-assertion smoke "
           "gate passed on %d (of the %d built, %d were NOT executed here — each named above; the %d that "
           "did not build are named there too)"
           % (len(built), lc.total, _count_lines(_field(cli, "tus")), len(smoked), len(built),
              len(built) - len(smoked), lc.total - len(built)))
    return 0
