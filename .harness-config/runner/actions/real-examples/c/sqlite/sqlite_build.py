#!/usr/bin/env python3
"""sqlite_build.py -- Steps 6b, 7 and 7b of the SQLite corpus harness.

  * the PER-TARGET HEADERS (`stage-zinc.py`): one zlib header dir per declared
    recipeTransform and one sqlite_cfg.h per declared target OS, then the deriving host's own
    `sqlite_cfg.h` REMOVED (a quote include searches the includer's own directory first, and
    `ctime.c` -- TU #1 of both artefacts -- sits beside it);
  * every leg's INCLUDE LIST, composed the same way on every host:
    `[cfg dir] + recipe -I dirs + [Tcl headers] (fixture only) + [zinc dir] + [build dir]
    (+ the macOS SDK include dir, LAST, so it can shadow nothing)`;
  * Step 7, the full-source `testfixture` per leg, and Step 7b, the `sqlite3` CLI per leg --
    both through ONE manifest generator (`gen-pe64-manifest.py`) and ONE build reader
    (`sqlite_base.build_artifact`: the compiler REPORTS the artefact it wrote; the driver never
    assembles a name), the same-platform ORACLE built from the same manifest, the per-TU
    build ATTRIBUTION asked of the resolver, and the ACQUIRED libraries staged beside every
    artefact (a recorded `@loader_path/<name>` is true only when the file sits there).

★ EVERY SELECTED LEG IS BUILT ON EVERY HOST: nothing here asks what kind of box this is,
except the macOS fresh-inode install, which is about the box that will EXEC the file.
★ A BUILD FAILURE IS `poisoned` FOR THAT LEG AND THE RUN CONTINUES.

The union of `build-and-test.sh` Steps 6–7b and `build-and-test.ps1` Steps 3+4 (staging) and
7–7b (lane mig, part 4, 2026-09-21). Nothing runs at import.
"""
from __future__ import annotations

import json
import os
import shutil
import stat
import sys

import sqlite_common as C

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass
sys.dont_write_bytecode = True

import sqlite_base as B          # noqa: E402  (after the bytecode switch)
import sqlite_launch as L        # noqa: E402
import sqlite_procs as P         # noqa: E402


def _indent(text, pad="      "):
    return "\n".join(pad + ln for ln in (text or "").rstrip().splitlines())


def _field(obj, name):
    """A StageResult field, whether the stage hands a record or a mapping."""
    if isinstance(obj, dict):
        return obj.get(name)
    return getattr(obj, name)


def _read_list(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return [ln.rstrip("\r\n") for ln in fh if ln.strip()]


def _write_list(path, items):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        for it in items:
            fh.write("%s\n" % it)


# ── the per-target headers ────────────────────────────────────────────────────────────

def stage_headers(run):
    """`stage-zinc.py` -> run.zinc_stage_dirs / run.cfg_stage_dirs; then the deriving host's
    sqlite_cfg.h removed. Refuses when either family produced nothing."""
    log, st = run.log, run.stage
    zinc_src = _field(st, "zinc_src")
    cfg_h = _field(st, "sqlite_cfg_h")
    base = os.path.dirname(os.path.abspath(zinc_src))
    zinc_root, cfg_root = os.path.join(base, "zinc"), os.path.join(base, "cfg")
    os.makedirs(zinc_root, exist_ok=True)
    os.makedirs(cfg_root, exist_ok=True)
    if not os.path.isfile(cfg_h):
        C.die("the generated sqlite_cfg.h is not at %s.\n      sqlite's ./configure writes it and the "
              "recipe's _HAVE_SQLITE_CONFIG_H makes every TU include it; each leg is staged its OWN "
              "copy from it (build.configureAnswers), so its absence is fatal for the ENTIRE run "
              "rather than for one leg." % cfg_h)
    r = C.capture(C.python_argv(C.STAGE_ZINC, "--zlib-h", os.path.join(zinc_src, "zlib.h"),
                                "--zconf-h", os.path.join(zinc_src, "zconf.h"),
                                "--dest", zinc_root, "--sqlite-cfg-h", cfg_h,
                                "--cfg-dest", cfg_root, "--catalogue", C.LEGS_JSON),
                  env_=C.child_env(python=True), merge=True)
    zinc, cfg = {}, {}
    for line in r.out.splitlines():
        line = line.rstrip("\r")
        if line.startswith("ZINC-STAGE-OK="):
            k, d, guards, note = (line[len("ZINC-STAGE-OK="):].split("|", 3) + ["", "", ""])[:4]
            zinc[k] = d
            log.info("zinc stage '%s' -> %s   [%s]" % (k, d, guards))
            if note:
                log.info("      note: %s" % note)
        elif line.startswith("ZINC-STAGE-FAIL="):
            k, _, why = line[len("ZINC-STAGE-FAIL="):].partition("|")
            log.warn("zinc stage '%s' COULD NOT BE PRODUCED — %s" % (k, why))
        elif line.startswith("ZINC-STAGES="):
            log.info("zinc stages: %s produced" % line[len("ZINC-STAGES="):])
        elif line.startswith("CFG-STAGE-OK="):
            k, d, ans, note = (line[len("CFG-STAGE-OK="):].split("|", 3) + ["", "", ""])[:4]
            cfg[k] = d
            log.info("sqlite config stage '%s' -> %s   [%s]" % (k, d, ans))
            if note:
                log.info("      note: %s" % note)
        elif line.startswith("CFG-STAGE-FAIL="):
            k, _, why = line[len("CFG-STAGE-FAIL="):].partition("|")
            log.warn("sqlite config stage '%s' COULD NOT BE PRODUCED — %s" % (k, why))
        elif line.startswith("CFG-STAGES="):
            log.info("sqlite config stages: %s produced" % line[len("CFG-STAGES="):])
        elif line.strip():
            log.info("      %s" % line)
    if not zinc:
        C.die("stage-zinc.py produced NO per-target zlib header dir (rc=%d):\n%s" % (r.rc, r.out))
    if not cfg:
        C.die("stage-zinc.py produced NO per-target sqlite_cfg.h (rc=%d).\n      Every leg would then "
              "fall back to the DERIVING host's copy on the build dir's include path, which is "
              "exactly how a Mach-O leg inherited the deriving Linux host's configure probes:\n%s"
              % (r.rc, r.out))
    # ★★ The deriving host's copy is REMOVED: "the staged dir is FIRST on the include list"
    # does not cover a TU whose OWN directory holds a sqlite_cfg.h (C 6.10.2p3 searches the
    # includer's directory before the list), and `ctime.c` does. It is this harness's own
    # ./configure output, already rewritten per target above -- not upstream source.
    os.remove(cfg_h)
    log.info("removed the deriving host's %s — %d per-target copy/copies replace it; a quote include "
             "searches the includer's OWN dir first, so leaving it there let ctime.c (TU #1) read "
             "this machine's answers ahead of the whole include list" % (cfg_h, len(cfg)))
    run.zinc_stage_dirs, run.cfg_stage_dirs = zinc, cfg


def _sdk_include():
    """The macOS SDK include dir (a HOST fact: only a Mac has an Xcode SDK), or ""."""
    r = C.capture(["xcrun", "--show-sdk-path"], timeout=60)
    p = r.out.strip()
    inc = os.path.join(p, "usr", "include") if (r.rc == 0 and p) else ""
    return inc if inc and os.path.isdir(inc) else ""


def write_include_lists(run):
    """One fixture list and one CLI list per (zinc stage, config stage) PAIR the declared legs
    actually use; each leg gets the pair carrying its OWN headers, or none (the Step-7/7b
    blockers then poison it by name -- there is no fallback to a sibling stage's copy)."""
    st = run.stage
    fx, cli = _field(st, "fixture_recipe"), _field(st, "cli_recipe")
    head = _read_list(_field(fx, "includes")) + [_field(st, "tcl_inc")]
    cli_head = _read_list(_field(cli, "includes"))
    tail = [_field(st, "bld")]
    if run.host == "darwin":
        sdk = _sdk_include()
        if sdk:
            tail.append(sdk)
    pairs = set()
    for leg in run.legs:
        k, c = leg.build.get("headerStageKey") or "", leg.build.get("configStageKey") or ""
        if k in run.zinc_stage_dirs and c in run.cfg_stage_dirs:
            pairs.add((k, c))
    for k, c in sorted(pairs):
        _write_list(os.path.join(run.out_dir, "recipe-includes.%s.%s.txt" % (k, c)),
                    [run.cfg_stage_dirs[c]] + head + [run.zinc_stage_dirs[k]] + tail)
        _write_list(os.path.join(run.out_dir, "cli-includes.%s.%s.txt" % (k, c)),
                    [run.cfg_stage_dirs[c]] + cli_head + [run.zinc_stage_dirs[k]] + tail)
    for leg in run.legs:
        k, c = leg.build.get("headerStageKey") or "", leg.build.get("configStageKey") or ""
        if (k, c) not in pairs:
            continue
        leg.inc_file = os.path.join(run.out_dir, "recipe-includes.%s.%s.txt" % (k, c))
        leg.cli_inc_file = os.path.join(run.out_dir, "cli-includes.%s.%s.txt" % (k, c))
        leg.zinc_dir, leg.cfg_dir = run.zinc_stage_dirs[k], run.cfg_stage_dirs[c]


# ── shared pieces of both artefact loops ──────────────────────────────────────────────

def generator_caps(run):
    """Does the manifest generator take the leg's own transform and stack reserve? PROBED,
    never assumed: an older generator applied the WINDOWS transform unconditionally."""
    r = C.capture(C.python_argv(C.MANIFEST_GEN, "--help"), env_=C.child_env(python=True),
                  merge=True)
    caps = {"recipeTransform": "--recipe-transform" in r.out,
            "stackReserve": "--stack-reserve" in r.out,
            "tuPreludes": "--tu-preludes" in r.out}
    run.log.info("manifest generator: %s  --recipe-transform:%s  --stack-reserve:%s  --tu-preludes:%s"
                 % (os.path.basename(C.MANIFEST_GEN), "YES" if caps["recipeTransform"] else "no",
                    "YES" if caps["stackReserve"] else "no", "YES" if caps["tuPreludes"] else "no"))
    return caps


def write_tu_preludes(leg, outd):
    """The leg's declared `build.tuPreludes`, written as `<outd>/tu-preludes.json` for the manifest
    generator -> its path; "" for a leg that declares none (whose generator argv stays the one it
    always had). Both compilers read the manifest the generator writes, so the declaration reaches
    dsscp and the same-platform reference alike."""
    entries = leg.build.get("tuPreludes")
    if not entries:
        return ""
    path = os.path.join(outd, "tu-preludes.json")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(entries, fh, indent=2)
        fh.write("\n")
    return path


def fixture_manifest(run, leg, outd, manifest, tokens):
    """THE ONE COMPOSITION of a leg's testfixture manifest, written to `manifest`: the stage's
    fixture recipe (TUs, defines), the leg's include list, its resolved-library argv `tokens`, its
    recipe transform, stack reserve and declared TU preludes, through the ONE generator. Step 7
    and the round-close recompile (`sqlite_recompile`) both call it, so what the recompile
    judges is exactly what a run builds. -> the generator's Result."""
    fx = _field(run.stage, "fixture_recipe")
    return B.generate_manifest(C.MANIFEST_GEN, manifest, "testfixture", leg.spec,
                               _field(fx, "tus"), leg.inc_file, _field(fx, "defines"),
                               leg.build.get("recipeTransform") or "none",
                               leg.build.get("stackReserveBytes") or 0, tokens,
                               tu_preludes=write_tu_preludes(leg, outd))


def manifest_blockers(leg, caps, include_file):
    blockers = []
    t = leg.build.get("recipeTransform") or ""
    if t != "windows-selfconfig" and not (caps["recipeTransform"] and caps["stackReserve"]):
        blockers.append("this leg declares recipeTransform='%s' + stackReserveBytes=%s, but %s applies "
                        "'windows-selfconfig' + an 8 MiB reserve UNCONDITIONALLY; generating this "
                        "leg's manifest would silently apply the WINDOWS transform to a non-Windows "
                        "target — a cross-compile category error, so it is refused"
                        % (t, leg.build.get("stackReserveBytes"), os.path.basename(C.MANIFEST_GEN)))
    if leg.build.get("tuPreludes") and not caps.get("tuPreludes"):
        blockers.append("this leg declares build.tuPreludes for %s, but %s does not take "
                        "--tu-preludes; building it would compile those TUs WITHOUT the prelude "
                        "its leg declares for them, so it is refused"
                        % (", ".join("<%s>" % e.get("tu") for e in leg.build["tuPreludes"]
                                     if isinstance(e, dict)), os.path.basename(C.MANIFEST_GEN)))
    if not include_file:
        blockers.append("this leg has no include list: its staged zlib header dir 'zinc/%s' (declared "
                        "zconfGuards: %s) and/or its staged sqlite config dir 'cfg/%s' (declared "
                        "configureAnswers: %s) was NOT produced — see the ZINC-STAGE-FAIL / "
                        "CFG-STAGE-FAIL line above. Compiling it against another target's zlib "
                        "header, or against the DERIVING host's sqlite_cfg.h, is refused."
                        % (leg.build.get("headerStageKey") or "?",
                           json.dumps(leg.build.get("zconfGuards") or {}, sort_keys=True),
                           leg.build.get("configStageKey") or "?",
                           json.dumps(leg.build.get("configureAnswers") or {}, sort_keys=True)))
    return blockers


def library_argv(run, leg, which, log_path):
    """The argv that hands DSS this leg's resolved libraries (`which` in ("tcl", "z")), built by
    the RESOLVER (it owns the flag and PROBES the compiler for a declared runtime identity).
    -> (tokens, why). A refusal is `poisoned` for the leg: dropping a declared identity would
    build an artefact that links clean here and fails in the target's loader."""
    libs = leg.build.get("libraries") or {}
    tokens, errs, notes = [], [], []
    for w in which:
        path = leg.tcl_lib_any if w == "tcl" else leg.z_lib_any
        imp = libs.get("tclImportName" if w == "tcl" else "zImportName") or ""
        call = ["--resolve-library-argv", path]
        if imp:
            call += ["--import-name", imp, "--dss", run.compiler.path]
        r = run.resolver.call(call)
        if r.err:
            errs.append(r.err)
        toks = [t.rstrip("\r") for t in r.out.split("\n")]
        while toks and toks[-1] == "":
            toks.pop()
        if r.rc != 0:
            _save(log_path, "\n".join(errs))
            return None, ("the resolver REFUSED to build the library argv for this leg's %s library "
                          "(rc=%d; path %s%s) — %s" % (w, r.rc, path, "; declared runtime identity "
                                                       "'%s'" % imp if imp else "",
                                                       " ".join((r.err or "").split()) or
                                                       "<the resolver refused with no diagnostic "
                                                       "on stderr — see %s>" % log_path))
        if len(toks) < 2 or [t for t in toks if not t]:
            _save(log_path, "\n".join(errs))
            return None, ("the resolver exited 0 for this leg's %s library but printed %d usable "
                          "token(s) (expected at least the flag and its value, one per line)"
                          % (w, len([t for t in toks if t])))
        tokens += toks
        if imp:
            notes.append("%s recorded as '%s' (the leg's DECLARED runtime identity)" % (w, imp))
    _save(log_path, "\n".join(errs))
    if not tokens:
        return None, "no library matched the requested set '%s'" % ",".join(which)
    return tokens, ("; ".join(notes) if notes else
                    "no runtime-identity override declared — each library is recorded under its "
                    "own embedded identity")


def _save(path, text):
    try:
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text or "")
    except OSError:
        pass


def preflight_sweep(run, leg, directory, why, kernel_entry, launch_directory):
    """Kill anything still EXECUTING out of the directory the build is about to overwrite (we
    hold the run lock, so a match is a leftover by construction) -- natively, and inside the
    launched fixture's kernel when it has its own (a WSL fixture is a Linux process)."""
    needle = directory.rstrip("/\\") + os.sep
    killed = P.stop_our_fixtures(needle, why, settle_s=run.cfg.kill_settle, log=run.log) or []
    if kernel_entry and launch_directory:
        killed += P.stop_our_fixtures(launch_directory.rstrip("/") + "/", why,
                                      launcher_prefix=kernel_entry,
                                      settle_s=run.cfg.kill_settle, log=run.log) or []
    for k in killed:
        leg.hygiene.append("%s: killed leftover pid %s" % (why, k))
    return killed


def _rmtree(path):
    def onerror(func, p, _exc):
        try:
            os.chmod(p, stat.S_IWRITE | stat.S_IREAD)
            func(p)
        except OSError:
            raise
    if os.path.lexists(path):
        shutil.rmtree(path, onerror=onerror)


def stage_acquired(leg, directory):
    """Copy every ACQUIRED library beside the artefact under the name the leg declared; -> the
    names that could NOT be staged (the copy keeps mode and mtime, as `cp -p` did)."""
    bad = []
    for as_name, src in leg.acq_libs:
        if not as_name or not src:
            bad.append(as_name or "<unnamed>")
            continue
        dst = os.path.join(directory, as_name)
        try:
            shutil.copy2(src, dst)
            if not os.path.isfile(dst):
                bad.append(as_name)
        except OSError:
            bad.append(as_name)
    return bad


def fresh_inode(path):
    """macOS: give the file a BRAND-NEW inode (copy beside, rename over) before anything execs
    it -- AppleSystemPolicy pins a permanent exec DENY to an inode, and dsscp's O_TRUNC rebuild
    reuses the one a previous run exec'd (✔MEASURED: 137 on every exec, cleared only by a new
    inode). -> the new inode; raises HarnessDie when it did not change."""
    old = os.stat(path).st_ino
    tmp = "%s.freshinode.%d" % (path, os.getpid())
    if os.path.lexists(tmp):
        os.remove(tmp)
    shutil.copy2(path, tmp)
    if not os.access(tmp, os.X_OK):
        os.remove(tmp)
        C.die("could not install %s on a FRESH INODE: the copy is not executable" % path)
    os.replace(tmp, path)
    new = os.stat(path).st_ino
    if new == old:
        C.die("could not install %s on a FRESH INODE (the inode did not change). macOS pins a "
              "permanent exec DENY to the INODE, and an in-place rebuild inherits it — the binary "
              "would be SIGKILLed (137) on every exec with no output at all." % path)
    return new


def _first_errors(log_path, n=3):
    out = []
    try:
        with open(log_path, "rb") as fh:
            for raw in fh:
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
                if "error[" in line:
                    out.append(line)
                    if len(out) >= n:
                        break
    except OSError:
        pass
    if not out:
        out = C.tail_file(log_path, n).splitlines()[:n]
    return out


def _json_record(text):
    """The resolver's JSON record: the whole stdout, else its LAST line that opens an object."""
    try:
        v = json.loads(text)
        return v if isinstance(v, dict) else None
    except ValueError:
        pass
    for line in reversed((text or "").splitlines()):
        if line.lstrip().startswith("{"):
            try:
                v = json.loads(line)
                return v if isinstance(v, dict) else None
            except ValueError:
                return None
    return None


# ── Step 7 — the testfixture, per leg ─────────────────────────────────────────────────

def step7(run):
    log, cfg, st = run.log, run.cfg, run.stage
    log.step("7/9  Build the full-source testfixture (dsscp --project, %s), per leg" % cfg.dss_config)
    coherence_gate(run, "staged sqlite (Step 7, pre-build)", require_cli=False)
    write_include_lists(run)
    run.gen_caps = generator_caps(run)
    fx = _field(st, "fixture_recipe")
    n_tus = len(_read_list(_field(fx, "tus")))
    for leg in run.selected():
        if not leg.tcl_lib or not leg.z_lib:
            log.warn("[%s] build NOT ATTEMPTED [%s] — %s" % (leg.label, leg.verdict or "<none>",
                                                           leg.verdict_detail or "<no reason>"))
            continue
        blockers = manifest_blockers(leg, run.gen_caps, leg.inc_file)
        if blockers:
            run.counts["compile"] += 1
            run.ledger.set_leg(leg, "poisoned", "this driver cannot express this leg's manifest "
                               "correctly: %s" % "  ALSO: ".join(blockers))
            log.warn("[%s] POISONED — %s" % (leg.label, leg.verdict_detail))
            continue
        outd = run.leg_out(leg)
        fmt_dir = os.path.join(outd, leg.format)
        plan = leg_run_dir_plan(run, leg)
        kentry = plan.get("kernelEntryArgv") if plan else []
        launch_fmt_dir = (L.launch_path(run.resolver, L.path_verb(leg), fmt_dir)
                          if kentry else "")
        preflight_sweep(run, leg, fmt_dir, "pre-flight", kentry, launch_fmt_dir)
        # SCOPED TO THIS LEG: a fresh output directory, so nothing a previous run left can be
        # read back as this run's (the artefact's NAME is still the compiler's to report).
        if os.path.normcase(os.path.abspath(outd)) == os.path.normcase(os.path.abspath(run.out_dir)) \
                or not leg.label:
            C.die("internal: refusing to wipe '%s' for leg '%s' — it is not a per-leg directory under "
                  "%s." % (outd, leg.label, run.out_dir))
        try:
            _rmtree(outd)
            os.makedirs(outd, exist_ok=True)
        except OSError as exc:
            run.counts["compile"] += 1
            run.ledger.set_leg(leg, "poisoned", "the leg's output directory %s could not be reset: %s"
                               % (outd, exc))
            log.warn("[%s] POISONED — %s" % (leg.label, leg.verdict_detail))
            continue
        log_path = os.path.join(outd, "compile.log")
        manifest = os.path.join(outd, "%s.dss-project.json" % leg.label)
        log.info("[%s] %s — %d TUs → testfixture (resolve: %s, %s; transform: %s; stackReserve: %s)"
                 % (leg.label, leg.spec, n_tus, os.path.basename(leg.tcl_lib),
                    os.path.basename(leg.z_lib), leg.build.get("recipeTransform"),
                    leg.build.get("stackReserveBytes")))
        log.info("[%s] zlib headers: %s  [%s]" % (leg.label, leg.zinc_dir,
                                                 json.dumps(leg.build.get("zconfGuards") or {},
                                                            sort_keys=True)))
        log.info("[%s] sqlite config header: %s  [%s]"
                 % (leg.label, leg.cfg_dir, json.dumps(leg.build.get("configureAnswers") or {},
                                                       sort_keys=True)))
        tokens, why = library_argv(run, leg, ("tcl", "z"), os.path.join(outd, "resolve-library-argv.log"))
        if tokens is None:
            run.counts["compile"] += 1
            run.ledger.set_leg(leg, "poisoned", "the DSS argv for this leg's resolved libraries could "
                               "not be built — %s" % why)
            log.warn("[%s] POISONED — %s" % (leg.label, leg.verdict_detail))
            continue
        log.info("[%s] resolve-library: %s" % (leg.label, why))
        g = fixture_manifest(run, leg, outd, manifest, tokens)
        if g.rc != 0:
            run.counts["compile"] += 1
            run.ledger.set_leg(leg, "poisoned", "manifest generation FAILED (%s, rc=%d): %s"
                               % (os.path.basename(C.MANIFEST_GEN), g.rc,
                                  " / ".join((g.out or "").strip().splitlines()[-6:])))
            log.warn("[%s] POISONED — manifest generation failed:\n%s" % (leg.label, _indent(g.out)))
            continue
        for line in (g.out or "").splitlines():
            if line.strip():
                log.info("[%s] manifest: %s" % (leg.label, line.strip()))
        log.info("[%s] manifest → %s" % (leg.label, manifest))
        build_oracle(run, leg, manifest, outd)
        leg.manifest = manifest
        res = B.build_artifact(run.compiler.path, manifest, cfg.dss_config, outd, log_path, leg.spec)
        code = res.code
        if code == 0 and not os.access(res.path, os.X_OK):
            code = 5
        if code != 0:
            run.counts["compile"] += 1
            detail = "the fixture did not build for %s — see %s" % (leg.spec, log_path)
            if code == 3:
                log.warn("[%s] build FAILED%s — first diagnostics (%s):" % (leg.label, res.time_suffix,
                                                                          log_path))
                for line in _first_errors(log_path):
                    log.info("      %s" % line)
                summary = attribute_build(run, leg, log_path, manifest, outd)
                if summary:
                    detail = "the fixture did not build for %s — %s.  See %s" % (leg.spec, summary,
                                                                              log_path)
            elif code == 2:
                log.warn("[%s] build FAILED%s — the build log reports MORE THAN ONE artefact for %s; "
                         "see %s (%s)" % (leg.label, res.time_suffix, leg.spec, log_path, res.error))
            elif code == 1:
                log.warn("[%s] build FAILED%s — 0 error[ and the build reported NO artefact for %s "
                         "(expected a '%s%s <path>' line in %s)"
                         % (leg.label, res.time_suffix, leg.spec, B.ARTIFACT_MARKER, leg.spec, log_path))
            elif code == 4:
                log.warn("[%s] build FAILED%s — 0 error[ but the artefact the build REPORTED is not "
                         "there: %s" % (leg.label, res.time_suffix, res.path))
            elif code == 5:
                log.warn("[%s] build FAILED%s — 0 error[ but the artefact the build REPORTED is not an "
                         "executable file: %s" % (leg.label, res.time_suffix, res.path))
            else:
                log.warn("[%s] build FAILED — the compiler could not be run: %s"
                         % (leg.label, res.error))
                detail = "the compiler could not be run for %s — %s" % (leg.spec, res.error)
            run.ledger.set_leg(leg, "poisoned", detail)
            continue
        bad = stage_acquired(leg, os.path.dirname(res.path))
        if bad:
            run.counts["compile"] += 1
            run.ledger.set_leg(leg, "poisoned", "the fixture built for %s, but its ACQUIRED librar(y/ies) "
                               "%s could not be staged into %s. The artefact records "
                               "'@loader_path/<name>' for them, so without the copies it fails in "
                               "the target's loader." % (leg.spec, " ".join(bad),
                                                         os.path.dirname(res.path)))
            log.warn("[%s] POISONED — %s" % (leg.label, leg.verdict_detail))
            continue
        for as_name, src in leg.acq_libs:
            log.info("[%s] staged beside the artefact: %s  (from %s)" % (leg.label, as_name, src))
        if run.host == "darwin":
            log.info("[%s] fresh-inode install: %s now inode %s"
                     % (leg.label, res.path, fresh_inode(res.path)))
        leg.fixture, leg.fixture_built = res.path, True
        log.ok("[%s] testfixture -> %s%s" % (leg.label, res.path, res.time_suffix))
    built = [lg for lg in run.selected() if lg.fixture_built]
    log.info("built %d of %d buildable leg(s); %d declared"
             % (len(built), len([lg for lg in run.selected() if lg.tcl_lib]), len(run.legs)))


def leg_run_dir_plan(run, leg):
    """The leg's run-directory plan (cached): needed by the pre-flight sweep (which kernel to
    enter) and by Step 8. None for a leg this host cannot run."""
    if C.run_is_skipped(leg):
        return None
    if getattr(leg, "rundir_plan", None) is None:
        leg.rundir_plan = L.run_dir_plan(run.resolver, leg, os.path.join(run.leg_out(leg), "run"))
    return leg.rundir_plan


def build_oracle(run, leg, manifest, outd):
    """THE SAME-PLATFORM ATTRIBUTION ORACLE, from the SAME manifest: the leg's verified target
    compiler builds exactly what dsscp is handed. Never fatal and never silent; its STATUS is
    kept on every outcome (`build-failed` = the control RAN and agreed with us, which is
    evidence; `no-reference-compiler` = no control at all)."""
    log = run.log
    olog = os.path.join(outd, "reference-oracle.log")
    r = run.resolver.call(["--build-reference-oracle", leg.label, "--manifest", manifest,
                           "--oracle-dir", outd, "--oracle-log", olog])
    _save(os.path.join(outd, "reference-oracle.stderr"), r.err)
    rec = _json_record(r.out) or {}
    leg.oracle = {"status": str(rec.get("status") or ""), "log": olog, "path": "", "cc": "",
                  "triple": ""}
    if r.rc == 0:
        leg.oracle.update(path=str(rec.get("path") or ""), cc=str(rec.get("cc") or ""),
                          triple=str(rec.get("triple") or ""))
        log.info("[%s] same-platform ORACLE built by %s (%s) → %s"
                 % (leg.label, leg.oracle["cc"], leg.oracle["triple"], leg.oracle["path"]))
        return
    if r.rc == 3:
        log.info("[%s] the same-platform ORACLE also FAILED to build these sources — its diagnostics "
                 "are the CONTROL for this leg's build (%s)" % (leg.label, olog))
    for line in (r.err or "").splitlines():
        if line.strip():
            log.warn("[%s] oracle: %s" % (leg.label, line.rstrip()))


def attribute_build(run, leg, log_path, manifest, outd):
    """★★★ WHOSE FAILURE IS THIS? Asked only when dsscp emitted diagnostics; the decision is the
    resolver's. -> the one-line summary for the ledger, or "" (a refusal excuses nothing)."""
    log = run.log
    r = run.resolver.call(["--attribute-build", leg.label, "--compile-log", log_path,
                           "--oracle-log", leg.oracle.get("log", ""),
                           "--oracle-status", leg.oracle.get("status", ""),
                           "--manifest", manifest])
    _save(os.path.join(outd, "build-attribution.stderr"), r.err)
    rec = _json_record(r.out) if r.rc in (0, 3) else None
    if rec is None or "report" not in rec:
        for line in (r.err or r.out or "").splitlines():
            if line.strip():
                log.warn("[%s] attribution: %s" % (leg.label, line.rstrip()))
        log.warn("[%s] build attribution UNAVAILABLE (rc %d) — every diagnostic stays charged to dss, "
                 "which is the safe direction" % (leg.label, r.rc))
        return ""
    for line in rec.get("report") or []:
        if str(line).strip():
            log.info(str(line))
    charged = [str(t) for t in rec.get("chargedToDss") or []]
    tus = rec.get("tus") or []
    gaps = rec.get("dssStreamGaps") or []
    summary = "%d of %d rejected TU(s) charged to DSS%s%s" % (
        len(charged), len(tus),
        "" if not charged else ": " + " ".join(t.replace("\\", "/").rsplit("/", 1)[-1] for t in charged),
        "" if not gaps else " — INCOMPLETE: dsscp's stream was not whole (%d gap(s)), so a TU past it "
                            "shows no error" % len(gaps))
    leg.build_attribution = summary
    return summary


def coherence_gate(run, label, require_cli):
    """The staged-source coherence gate (run-wide: a mixed-vintage tree is refused), run where
    the clone lives -- in-process on a POSIX host, inside WSL on a Windows one."""
    st = run.stage
    dirs = [_field(st, "bld")]
    if run.posix.needs_wsl:
        dirs.append(_field(st, "src"))
    script = os.path.join(C.HERE, "sqlite_coherence.py")
    argv = [run.posix.to_posix(script)] if run.posix.needs_wsl else [script]
    args = ["--checkout", run.sqlite_dir_posix, "--label", label]
    if require_cli:
        args.append("--require-cli")
    args += [run.posix.to_posix(d) for d in dirs]
    if run.posix.needs_wsl:
        r = C.capture(run.posix.argv(["python3"] + argv + args), merge=True)
    else:
        r = C.capture([sys.executable] + argv + args, env_=C.child_env(python=True), merge=True)
    for line in r.out.splitlines():
        if line.strip():
            run.log.info("   %s" % line.rstrip())
    if r.rc != 0:
        C.die("staged sqlite tree is INCOHERENT (mixed vintage) — refusing to build (%s, rc=%d)"
              % (label, r.rc))


# ── Step 7b — the sqlite3 CLI, per leg ────────────────────────────────────────────────

def step7b(run):
    """★ A SEPARATE LOOP, AND THAT IS THE POINT: the CLI needs ZLIB and not TCL, so a leg whose
    Tcl could not be resolved can still produce a perfectly good sqlite3."""
    log, cfg, st = run.log, run.cfg, run.stage
    log.step("7b/9  Build the sqlite3 CLI (dsscp --project, %s), per leg" % cfg.dss_config)
    cli = _field(st, "cli_recipe")
    n_tus = len(_read_list(_field(cli, "tus")))
    ledger = run.artifacts
    for leg in run.selected():
        outd = os.path.join(run.leg_out(leg), "cli")
        log_path = os.path.join(outd, "compile.log")
        manifest = os.path.join(outd, "%s.sqlite3.dss-project.json" % leg.label)
        if not leg.z_lib_any:
            run.counts["cli"] += 1
            if leg.verdict == "poisoned":
                ledger.set(leg.label, "sqlite3", "poisoned",
                           "%s  (the CLI links zlib too, so it is lost to the same defect.)"
                           % (leg.verdict_detail or "<no reason recorded>"))
                log.warn("[%s] CLI POISONED — %s" % (leg.label, ledger.get(leg.label, "sqlite3")[1]))
                continue
            ledger.set(leg.label, "sqlite3", "skipped-build-input-missing",
                       "no zlib could be resolved for this leg on this host, and the CLI links zlib "
                       "(SQLITE_HAVE_ZLIB=1 reaches a live '#include <zlib.h>' in shell.c) — see "
                       "Step 6.")
            log.warn("[%s] CLI build NOT ATTEMPTED [skipped-build-input-missing] — %s"
                     % (leg.label, ledger.get(leg.label, "sqlite3")[1]))
            continue
        blockers = manifest_blockers(leg, run.gen_caps, leg.cli_inc_file)
        if blockers:
            run.counts["cli"] += 1
            ledger.set(leg.label, "sqlite3", "poisoned", "  ALSO: ".join(blockers))
            log.warn("[%s] CLI POISONED — %s" % (leg.label, ledger.get(leg.label, "sqlite3")[1]))
            continue
        os.makedirs(outd, exist_ok=True)
        fmt_dir = os.path.join(outd, leg.format)
        plan = leg_run_dir_plan(run, leg)
        kentry = plan.get("kernelEntryArgv") if plan else []
        launch_fmt_dir = (L.launch_path(run.resolver, L.path_verb(leg), fmt_dir) if kentry else "")
        preflight_sweep(run, leg, fmt_dir, "cli pre-flight", kentry, launch_fmt_dir)
        tokens, why = library_argv(run, leg, ("z",), os.path.join(outd, "resolve-library-argv.log"))
        if tokens is None:
            run.counts["cli"] += 1
            ledger.set(leg.label, "sqlite3", "poisoned",
                       "the DSS argv for this leg's resolved zlib could not be built — %s" % why)
            log.warn("[%s] CLI POISONED — %s" % (leg.label, ledger.get(leg.label, "sqlite3")[1]))
            continue
        log.info("[%s] %s — %d TUs → sqlite3 (resolve: %s; transform: %s)"
                 % (leg.label, leg.spec, n_tus, os.path.basename(leg.z_lib_any),
                    leg.build.get("recipeTransform")))
        g = B.generate_manifest(C.MANIFEST_GEN, manifest, "sqlite3", leg.spec, _field(cli, "tus"),
                                leg.cli_inc_file, _field(cli, "defines"),
                                leg.build.get("recipeTransform") or "none",
                                leg.build.get("stackReserveBytes") or 0, tokens,
                                tu_preludes=write_tu_preludes(leg, outd))
        if g.rc != 0:
            run.counts["cli"] += 1
            ledger.set(leg.label, "sqlite3", "poisoned", "CLI manifest generation FAILED (rc=%d): %s"
                       % (g.rc, " / ".join((g.out or "").strip().splitlines()[-6:])))
            log.warn("[%s] CLI POISONED — manifest generation failed:\n%s" % (leg.label, _indent(g.out)))
            continue
        for line in (g.out or "").splitlines():
            if line.strip():
                log.info("[%s] cli manifest: %s" % (leg.label, line.strip()))
        res = B.build_artifact(run.compiler.path, manifest, cfg.dss_config, outd, log_path, leg.spec)
        if res.code != 0:
            run.counts["cli"] += 1
            if res.code == 3:
                why = " ".join(_first_errors(log_path))
            elif res.code == 2:
                why = "the build log reports MORE THAN ONE artefact for %s — %s" % (leg.spec, res.error)
            elif res.code == 4:
                why = "0 error[ but the artefact the build REPORTED is not there: %s" % res.path
            elif res.code == 1:
                why = ("0 error[ and the build reported NO artefact for %s (expected a '%s%s <path>' "
                       "line in %s)" % (leg.spec, B.ARTIFACT_MARKER, leg.spec, log_path))
            else:
                why = "the compiler could not be run: %s" % res.error
            ledger.set(leg.label, "sqlite3", "poisoned",
                       "the sqlite3 CLI did not build for %s — %s  See %s" % (leg.spec, why, log_path))
            log.warn("[%s] CLI build FAILED%s — %s" % (leg.label, res.time_suffix, why))
            continue
        bad = stage_acquired(leg, os.path.dirname(res.path))
        if bad:
            run.counts["cli"] += 1
            ledger.set(leg.label, "sqlite3", "poisoned",
                       "the CLI built for %s, but its ACQUIRED librar(y/ies) %s could not be staged "
                       "into %s. The artefact records '@loader_path/<name>' for them, so without the "
                       "copies it fails in the target's loader."
                       % (leg.spec, " ".join(bad), os.path.dirname(res.path)))
            log.warn("[%s] CLI POISONED — %s" % (leg.label, ledger.get(leg.label, "sqlite3")[1]))
            continue
        if run.host == "darwin":
            log.info("[%s] fresh-inode install: %s now inode %s"
                     % (leg.label, res.path, fresh_inode(res.path)))
        leg.cli_bin = res.path
        ledger.set(leg.label, "sqlite3", "built", "sqlite3 -> %s" % res.path)
        log.ok("[%s] sqlite3 -> %s%s" % (leg.label, res.path, res.time_suffix))
    n = len(run.selected())
    log.info("sqlite3 CLI: built on %d of %d selected leg(s); %d declared"
             % (len([lg for lg in run.selected() if lg.cli_bin]), n, len(run.legs)))
