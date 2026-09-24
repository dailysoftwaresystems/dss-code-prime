#!/usr/bin/env python3
"""sqlite_recompile.py -- the ROUND-CLOSE RECOMPILE: `build_and_test.py --recompile <leg>`.

WHAT IT ANSWERS. A round close compiles one leg's testfixture manifest with the dsscp under
review AND with that leg's same-platform reference, and a translation unit the reference compiles
that dsscp refuses is a MERGE BLOCKER. ✔MEASURED P68 round 8: a green unit gate shipped a
const-qualification regression (`T *const p[]` then `p++`) that broke every leg's testfixture,
and the unit gate compiles none of sqlite's 189 TUs. This mode is that check as ONE command:

    DSS_BIN=<the dsscp under review> python3 build_and_test.py --recompile <leg>

  R0  Step 0 (the harness's own self-tests), exactly as a run applies it;
  R1  the leg, from the resolver's plan (the SAME legs on every host; an unknown label refused);
  R2  the dsscp DSS_BIN names -- REQUIRED: never searched for and never built -- paired with the
      config tree it was built with (DSS_CONFIG_ROOT, else the source tree its OWN build tree was
      configured from), and the pair PROVED by the driver's currency pre-flight: ✔MEASURED P68
      round 8, a fold landed between a build and its recompile and the binary refused the live
      config at load. Its build type is printed and NOT gated: this verdict is acceptance, not time;
  R3  the STAGED sqlite state a run left under OUT_DIR, reused only when it is CURRENT
      (`stage_findings`), refused otherwise with every reason named -- this mode never re-derives,
      and it holds the output tree's RUN LOCK so a run cannot re-stage it underneath;
  R4  the leg's include list, composed from its VERIFIED per-target headers exactly as Step 7
      composes it, and its (tcl, z) libraries resolved exactly as Step 6 resolves them;
  R5  the manifest through `sqlite_build.fixture_manifest` (the ONE composition Step 7 uses), the
      reference oracle from it (`sqlite_build.build_oracle`), and dsscp on it
      (`sqlite_base.build_artifact`) asked for its WHOLE diagnostic stream (`DIAGNOSTIC_CAP`);
  R6  the per-TU census, asked of its owner (`harness_legs.py --recompile-verdicts`) and printed
      verbatim: the table, every INCOMPLETE reason, and the summary line LAST --
      `recompile: <leg> tus=N reference_ok=N dss_ok=N blockers=N`.

Everything it writes is under `<OUT_DIR>/recompile/<leg>/`, never a run's own leg directory.
Exit 0 only when the census is clean (no blocker, and nothing it could not see); 1 otherwise, every
reason printed. Nothing runs at import.
"""
from __future__ import annotations

import importlib.util
import json
import os
import sys
import traceback

import sqlite_common as C

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass
sys.dont_write_bytecode = True

import sqlite_base as B          # noqa: E402  (after the bytecode switch)
import sqlite_build as BLD       # noqa: E402
import sqlite_compiler as CMP    # noqa: E402
import sqlite_libs as LIBS       # noqa: E402
import sqlite_procs as P         # noqa: E402
import sqlite_stage as S         # noqa: E402

# ★ THE WHOLE STREAM. dsscp caps its diagnostics RUN-WIDE (50 per code, 1000 in all:
# DiagnosticReporter::Config), so a regression that raises one code in many TUs hides the LAST
# TUs whole -- and a census reading that log would count them accepted. The census refuses a
# stream whose cap still fired (`harness_legs.recompile_verdicts`); this is the request that keeps
# it from firing: a count no real build approaches, passed as BOTH caps.
DIAGNOSTIC_CAP = 1000000
# The mode's own subtree of the output tree a run left: `<OUT_DIR>/recompile/<leg>/`.
RECOMPILE_DIR = "recompile"
# The census's outcome for the reference when the oracle never reported one. Any status outside
# the two that mean "it ran" is, to the census, a reference that did not run.
ORACLE_NOT_RUN = "not-run"


# ── R2: the dsscp under review, and the config tree it was built with ─────────────────

def given_compiler(log):
    """The binary DSS_BIN names, as a `sqlite_compiler.Compiler`. REQUIRED, used as named: a
    recompile that SEARCHED for a binary would judge a compiler nobody put under review."""
    named = C.env("DSS_BIN").strip()
    if not named:
        C.die("--recompile compiles with a GIVEN dsscp: set DSS_BIN to the binary under review.\n"
              "      It is never searched for and never built here -- a recompile that picked a binary "
              "would judge a compiler nobody named.")
    if not os.path.isfile(named):
        C.die("DSS_BIN='%s' does not name an existing file." % named)
    info = CMP.build_type(named)
    comp = CMP.Compiler(info.path, info.type, info.source, info.detail, info.tree,
                        "named by DSS_BIN -- NOT built by this run", CMP.built_stamp(info),
                        "  (compiler build type: %s)" % info.type)
    log.info("compiler  : %s  (built %s)" % (comp.path, comp.built))
    log.info("build type: %s  -- read from %s; printed, NOT gated: a recompile judges which TUs "
             "compile, not how fast" % (comp.type, comp.source))
    return comp


def own_source_tree(compiler):
    """The source tree the binary's OWN build tree was configured from (`CMAKE_HOME_DIRECTORY` in
    its CMakeCache.txt), or "" when it has no build tree (a copy) or the cache names none."""
    if not compiler.tree:
        return ""
    try:
        with open(os.path.join(compiler.tree, "CMakeCache.txt"), encoding="utf-8",
                  errors="replace") as fh:
            for line in fh:
                if line.startswith("CMAKE_HOME_DIRECTORY:"):
                    return line.split("=", 1)[1].strip() if "=" in line else ""
    except OSError:
        return ""
    return ""


def pair_config_root(compiler, log):
    """DSS_CONFIG_ROOT (the operator's pairing, honoured), else the binary's own source tree --
    the config it was built beside -- else REFUSED: a binary with no tree names no config, and the
    compiler's own cwd walk would pick one silently. Verified and exported by the driver's ONE
    owner of that (`sqlite_compiler.pin_config_root`)."""
    if not C.env("DSS_CONFIG_ROOT").strip():
        own = own_source_tree(compiler)
        if not own:
            C.die("DSS_BIN=%s has no build tree that names the source tree it was built from (no "
                  "CMakeCache.txt above it, or one without CMAKE_HOME_DIRECTORY), so the config tree "
                  "it pairs with is unknown.\n      Set DSS_CONFIG_ROOT to the checkout whose "
                  "src/dss-config that binary was built with." % compiler.path)
        log.info("config    : the binary's own source tree (CMAKE_HOME_DIRECTORY of %s)"
                 % compiler.tree)
        return CMP.pin_config_root(own, log=log)
    log.info("config    : DSS_CONFIG_ROOT (the operator's pairing)")
    return CMP.pin_config_root("", log=log)


# ── R3: is the staged sqlite state CURRENT? ───────────────────────────────────────────

def configure_flags_applied(configure_args, flags):
    """Do the declared configure `flags` appear, in order and contiguous, in the argv the stage
    was configured with? (The derive adds the Tcl pin before them and an LDFLAGS after them.)"""
    args, flags = list(configure_args or []), list(flags or [])
    if not flags:
        return False
    return any(args[i:i + len(flags)] == flags for i in range(len(args) - len(flags) + 1))


def _stage_zinc():
    """`stage-zinc.py` loaded by path (the owner of the per-target header shapes and of their
    VERIFICATION); bytecode writing is already off."""
    spec = importlib.util.spec_from_file_location("dss_stage_zinc_recompile", C.STAGE_ZINC)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def header_stage_dirs(st, leg):
    """The leg's staged zlib-header and sqlite_cfg.h directories, where Step 6 writes them
    (`sqlite_build.stage_headers`: beside the stage's zinc source) -> (zinc dir, cfg dir)."""
    base = os.path.dirname(os.path.abspath(BLD._field(st, "zinc_src")))
    return (os.path.join(base, "zinc", leg.build.get("headerStageKey") or "?"),
            os.path.join(base, "cfg", leg.build.get("configStageKey") or "?"))


def stage_findings(st, stage_build, leg, verify_guards, verify_answers, coherence):
    """Every reason the staged sqlite state `st` (a `sqlite_stage.StageResult`) is NOT what a run
    would compile for `leg` today; [] = current. `stage_build` is the catalogue's CURRENT
    declaration (`sqlite_stage.parse_stage_build`); `verify_guards` / `verify_answers` are
    stage-zinc's own verifiers; `coherence(dirs) -> (ok, text)` runs the one-vintage gate."""
    why = []
    sb = stage_build
    if not configure_flags_applied(st.configure_args, sb["configure_flags"]):
        why.append("the stage was configured with %s, and the catalogue now declares the configure "
                   "flags %s" % (" ".join(st.configure_args) or "<nothing>",
                                 " ".join(sb["configure_flags"])))
    if (st.make_options or "") != (sb["make_options"] or ""):
        why.append("the stage was built with make OPTIONS=%r, and the catalogue now declares %r"
                   % (st.make_options, sb["make_options"]))
    if list(st.required_defines) != list(sb["required_defines"]):
        why.append("the stage asserted the defines %s, and the catalogue now requires %s"
                   % (" ".join(st.required_defines), " ".join(sb["required_defines"])))
    if dict(st.witnesses) != dict(sb["witnesses"]):
        why.append("the stage recorded the capability witnesses %s, and the catalogue now declares %s"
                   % (json.dumps(st.witnesses, sort_keys=True),
                      json.dumps(sb["witnesses"], sort_keys=True)))
    fx = st.fixture_recipe
    for what, path in (("TU list", fx.tus), ("define list", fx.defines)):
        if not os.path.isfile(path):
            why.append("the stage's fixture %s %s is missing" % (what, path))
    if os.path.isfile(fx.tus):
        tus = BLD._read_list(fx.tus)
        gone = [t for t in tus if not os.path.isfile(t)]
        if not tus:
            why.append("the stage's fixture TU list %s is EMPTY" % fx.tus)
        if gone:
            why.append("%d of the stage's %d fixture TU(s) are gone (first: %s)"
                       % (len(gone), len(tus), gone[0]))
    if not os.path.isfile(os.path.join(st.tcl_inc, "tcl.h")):
        why.append("the stage's Tcl headers are gone (%s has no tcl.h)" % st.tcl_inc)
    if os.path.isfile(st.sqlite_cfg_h):
        why.append("the DERIVING host's sqlite_cfg.h is still at %s: this stage never had its "
                   "per-target headers staged (Step 6 removes it), so ctime.c -- which sits beside it "
                   "-- would read that host's answers ahead of the whole include list"
                   % st.sqlite_cfg_h)
    zinc, cfgd = header_stage_dirs(st, leg)
    for label, path, verify, declared in (
            ("zlib header (zconfGuards)", os.path.join(zinc, "zconf.h"), verify_guards,
             leg.build.get("zconfGuards") or {}),
            ("sqlite_cfg.h (configureAnswers)", os.path.join(cfgd, "sqlite_cfg.h"), verify_answers,
             leg.build.get("configureAnswers") or {})):
        if not os.path.isfile(path):
            why.append("the leg's staged %s is missing: %s" % (label, path))
            continue
        try:
            verify(path, declared)
        except Exception as exc:  # noqa: BLE001 -- stage-zinc's StageError, or any read failure
            why.append("the leg's staged %s does not carry its CURRENT declaration %s: %s"
                       % (label, json.dumps(declared, sort_keys=True), exc))
    if not os.path.isfile(os.path.join(zinc, "zlib.h")):
        why.append("the leg's staged zlib.h is missing from %s" % zinc)
    ok, text = coherence([st.bld, st.src])
    if not ok:
        why.append("the staged sources are not ONE vintage:\n%s" % text)
    return why


def coherence_gate(label):
    """`coherence(dirs) -> (ok, report)`: `sqlite_coherence.py` over the STAGED directories, with
    no --checkout -- the staged copy is self-contained, and the shared clone may have moved on."""
    def run(dirs):
        r = C.capture(C.python_argv(os.path.join(C.HERE, "sqlite_coherence.py"), "--label", label,
                                    *dirs), env_=C.child_env(python=True), merge=True)
        return r.rc == 0, "\n".join("      " + ln for ln in (r.out or "").strip().splitlines())
    return run


def load_stage(run, leg):
    """R3: the stage a run left at `<stage root>/stage/`, refused unless it is current."""
    log = run.log
    path = os.path.join(run.stage_root, "stage", S.RESULT_FILE)
    if not os.path.isfile(path):
        C.die("no staged sqlite state at %s.\n      A run of this driver writes it (Steps 3-4: the "
              "derive persists its result there); the recompile reuses it and never re-derives. Run "
              "the driver once for this output tree, or point OUT_DIR at the output tree of a run "
              "that staged it." % path)
    with open(path, "r", encoding="utf-8") as fh:
        st = S.StageResult.from_json(fh.read())
    run.stage_build = run.resolver.json(["--stage-build", "--format", "json"],
                                        "the sqlite stage-build configuration (--stage-build)")
    zinc = _stage_zinc()
    why = stage_findings(st, S.parse_stage_build(run.stage_build), leg, zinc.verify_guards,
                         zinc.verify_answers, coherence_gate("staged sqlite (recompile)"))
    if why:
        C.die("the staged sqlite state at %s is NOT CURRENT for leg %s -- %d reason(s):\n%s\n      "
              "The recompile never re-derives. Re-stage it with a run of this driver (its Steps 3-6), "
              "then recompile." % (path, leg.label, len(why),
                                   "\n".join("      - %s" % w for w in why)))
    log.info("stage     : %s" % path)
    log.info("            sqlite %s (%s), %s; %d fixture TU(s); CURRENT for %s"
             % (st.sqlite_head, st.sqlite_branch, st.stage_identity, st.fixture_recipe.n_tus,
                leg.label))
    return st


# ── the mode ──────────────────────────────────────────────────────────────────────────

def recompile(run, label, driver):
    log, cfg = run.log, run.cfg
    log.step("R0  Step 0 -- the harness's own logic, as a run applies it")
    driver.step0(run)
    log.step("R1  The leg (%s)" % os.path.basename(C.LEGS_JSON))
    plan = run.resolver.json(["--plan"] + run.resolver.host_args + ["--format", "json"],
                             "the leg plan (--plan)")
    legs = [C.Leg(d) for d in (plan.get("legs") or [])]
    leg = next((lg for lg in legs if lg.label == label), None)
    if leg is None:
        C.die("--recompile names leg '%s', which %s does not declare (declared: %s)."
              % (label, C.LEGS_JSON, " ".join(lg.label for lg in legs) or "<none>"))
    run.plan, run.legs = plan, [leg]
    run.ledger = C.Ledger(driver.read_vocabulary(run.resolver), log)
    log.info("leg       : %s  %s" % (leg.label, leg.spec))
    log.step("R2  The dsscp under review (DSS_BIN) and the config tree it was built with")
    run.compiler = given_compiler(log)
    run.config_root = pair_config_root(run.compiler, log)
    proved = CMP.assert_current(C.BENCH_CORE, run.compiler, run.config_root, [leg.spec],
                                CMP.rebuild_command(run.compiler, run.repo_root))
    log.ok("the pair compiles against its config for: %s" % proved)
    log.step("R3  The staged sqlite state (reused only when CURRENT)")
    run.run_lock = P.RunLock(os.path.join(run.stage_root, ".harness-lock"))
    stolen = run.run_lock.acquire(log)
    if stolen:
        log.warn("took over a STALE run lock left by PID %s" % stolen)
    run.stage = load_stage(run, leg)
    log.step("R4  The leg's build inputs (its verified headers, its libraries)")
    zinc, cfgd = header_stage_dirs(run.stage, leg)
    run.zinc_stage_dirs = {leg.build.get("headerStageKey"): zinc.replace("\\", "/")}
    run.cfg_stage_dirs = {leg.build.get("configStageKey"): cfgd.replace("\\", "/")}
    os.makedirs(run.out_dir, exist_ok=True)
    BLD.write_include_lists(run)
    LIBS.resolve_leg(run, leg, LIBS.library_providers(run))
    if not leg.tcl_lib:
        C.die("[%s] this leg's (tcl, z) libraries could not be resolved on this host [%s]: %s"
              % (leg.label, leg.verdict or "<no verdict>", leg.verdict_detail or "<no detail>"))
    LIBS.tcl_coherence(run)
    log.step("R5  The manifest, then BOTH compilers on it")
    outd = run.leg_out(leg)
    if os.path.normcase(os.path.dirname(os.path.abspath(outd))) != os.path.normcase(
            os.path.abspath(run.out_dir)):
        C.die("internal: refusing to reset '%s' -- it is not a per-leg directory under %s"
              % (outd, run.out_dir))
    BLD._rmtree(outd)
    os.makedirs(outd, exist_ok=True)
    blockers = BLD.manifest_blockers(leg, BLD.generator_caps(run), leg.inc_file)
    if blockers:
        C.die("[%s] this driver cannot express the leg's manifest correctly: %s"
              % (leg.label, "  ALSO: ".join(blockers)))
    tokens, why = BLD.library_argv(run, leg, ("tcl", "z"),
                                   os.path.join(outd, "resolve-library-argv.log"))
    if tokens is None:
        C.die("[%s] the DSS argv for this leg's resolved libraries could not be built -- %s"
              % (leg.label, why))
    manifest = os.path.join(outd, "%s.dss-project.json" % leg.label)
    g = BLD.fixture_manifest(run, leg, outd, manifest, tokens)
    if g.rc != 0:
        C.die("[%s] manifest generation FAILED (rc=%d):\n%s" % (leg.label, g.rc, BLD._indent(g.out)))
    for line in (g.out or "").splitlines():
        if line.strip():
            log.info("[%s] manifest: %s" % (leg.label, line.strip()))
    log.info("[%s] manifest -> %s" % (leg.label, manifest))
    BLD.build_oracle(run, leg, manifest, outd)
    log_path = os.path.join(outd, "compile.log")
    res = B.build_artifact(run.compiler.path, manifest, cfg.dss_config, outd, log_path, leg.spec,
                           diagnostic_cap=DIAGNOSTIC_CAP)
    dss_build = "built" if res.code == 0 else ("errors" if res.code == 3 else "failed")
    log.info("[%s] dsscp --config=%s: %s%s  (%s)"
             % (leg.label, cfg.dss_config, dss_build, res.time_suffix,
                res.path or res.error or log_path))
    log.step("R6  The per-TU census")
    r = run.resolver.call(["--recompile-verdicts", leg.label, "--manifest", manifest,
                           "--compile-log", log_path, "--dss-build", dss_build,
                           "--dss-build-detail", res.error or "",
                           "--oracle-log", leg.oracle.get("log") or os.path.join(
                               outd, "reference-oracle.log"),
                           "--oracle-status", leg.oracle.get("status") or ORACLE_NOT_RUN])
    rec = BLD._json_record(r.out) if r.rc in (0, 3) else None
    if rec is None or "report" not in rec:
        C.die("the census (harness_legs.py --recompile-verdicts) did not answer (rc=%d):\n%s"
              % (r.rc, BLD._indent((r.err or r.out or "").strip() or "<no output>")))
    with open(os.path.join(outd, "recompile-verdicts.json"), "w", encoding="utf-8",
              newline="\n") as fh:
        json.dump(rec, fh, indent=1, sort_keys=True)
        fh.write("\n")
    for line in rec["report"]:
        print(line, flush=True)
    return 0 if (r.rc == 0 and rec.get("clean") is True) else 1


def main(label, driver):
    """`build_and_test.py --recompile <leg>`. `driver` is the build_and_test module that called
    (its Step 0, its vocabulary reader and its placement rule), passed rather than re-imported."""
    run = None
    try:
        run = C.Run(C.Config())
        driver.place_run(run)
        run.stage_root = run.out_dir
        run.out_dir = os.path.join(run.stage_root, RECOMPILE_DIR)
        return recompile(run, label, driver)
    except C.HarnessDie as exc:
        print(" ✗ ERROR: %s" % exc, file=sys.stderr, flush=True)
        return exc.exit_code
    except KeyboardInterrupt:
        print(" ✗ ERROR: interrupted", file=sys.stderr, flush=True)
        return 130
    except Exception:  # noqa: BLE001 -- never a silent exit
        print(" ✗ ERROR: the recompile failed with an unexpected exception:\n%s"
              % traceback.format_exc(), file=sys.stderr, flush=True)
        return 1
    finally:
        if run is not None and run.run_lock is not None:
            try:
                run.run_lock.release()
            except Exception:  # noqa: BLE001 -- a release failure must not mask the verdict
                pass
