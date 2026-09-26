#!/usr/bin/env python3
"""sqlite_libs.py -- Step 6 of the SQLite corpus harness: EACH LEG'S DECLARED build inputs.

Every declared leg names its library PROVIDER in legs.json; this module dispatches on it and
resolves the leg's (libtcl, libz) pair, then asks the shared resolver the three per-run
questions that depend on the pairs: does the staged Tcl HEADER agree with every leg's libtcl
(`--tcl-coherence`, measured from each library's own bytes), which arm stages the loadext
helper (`--loadext-builder`), and which verified CONTROL compiler each runnable leg has
(`--resolve-target-cc`, information, never a verdict).

★ ONE VERDICT PER LEG, AND THE RUN CONTINUES. A leg whose declared inputs are absent from this
machine is `skipped-build-input-missing` (ENVIRONMENTAL: warns, reds under
DSS_STRICT_ARM_VERDICTS=1) naming everything searched; a leg declaring a provider this driver
has no dispatch arm for is `poisoned` (a HARNESS defect, never an environment fact); neither
costs another leg anything. Nothing here is keyed on the host: the provider is a property of
the LEG.

The union of `build-and-test.sh` Step 6 and `build-and-test.ps1` Step 6 (lane mig, part 4,
2026-09-21). Nothing runs at import.
"""
from __future__ import annotations

import json
import os
import sys

import sqlite_common as C

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass
sys.dont_write_bytecode = True

# The acquisition report's key for the staged Tcl SCRIPT library (optional per contract).
ACQ_SCRIPT_LIBRARY_KEY = "scriptLibraryDir"

# `host-system` candidate ROOTS: legs.json declares none for it (its lint forbids them), so
# they come from here -- CANDIDATES, every one tried on every host (a miss costs a stat).
# The union of both old drivers' lists.
HOST_LIB_ROOTS = ("/usr/lib", "/usr/lib64", "/lib", "/lib64", "/usr/local/lib",
                  "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu",
                  "/usr/local/opt/tcl-tk/lib", "/opt/homebrew/lib", "/opt/local/lib")
# The environment variables whose directories a `host-system` search tries FIRST, split on
# THIS host's path-list separator (the .ps1 split on `[:;]`, which cut `C:\x` into `C` and
# `\x` -- report 08 finding 5).
HOST_LIB_ENV = ("DSS_HOST_LIBDIR", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH",
                "DYLD_FALLBACK_LIBRARY_PATH")


def lib_name_family(name):
    """The OBJECT-FORMAT family a library FILE NAME belongs to (keyed on the name, never the
    host): dll | dylib | so | ""."""
    if name.endswith(".dll"):
        return "dll"
    if name.endswith(".dylib"):
        return "dylib"
    if name.endswith(".so") or ".so." in name:
        return "so"
    return ""


def host_system_tcl_names(declared, tcl_lib_file, tcl_version):
    """A `host-system` leg's Tcl candidates in PRECEDENCE order: the names DERIVED from this
    installation's own tclConfig.sh (`TCL_LIB_FILE`, then `<file>.0`, then the version triple)
    filtered to the object-format families the LEG declares -- derivation first keeps the
    harness version-agnostic, the family filter keeps a Linux `.so` away from a macho leg --
    then the leg's DECLARED names as the backstop. Duplicates dropped, order kept."""
    derived = []
    if tcl_lib_file:
        derived += [tcl_lib_file, tcl_lib_file + ".0"]
    if tcl_version:
        derived += ["libtcl%s.so" % tcl_version, "libtcl%s.so.0" % tcl_version,
                    "libtcl%s.dylib" % tcl_version]
    families = {lib_name_family(n) for n in declared}
    out = []
    for n in derived:
        if n and lib_name_family(n) in families and n not in out:
            out.append(n)
    for n in declared:
        if n and n not in out:
            out.append(n)
    return out


def _readable_file(path):
    return os.path.isfile(path) and os.access(path, os.R_OK)


def find_first_in(dirs, names):
    """The first READABLE file `<dir>/<name>`, directory-major (a declared path order is a
    preference order), name order breaking ties. ★ Readability, not existence: a dangling
    symlink (macOS's shared-cache-only libz) is listed by a directory walk and cannot be
    opened by the compiler."""
    for d in dirs:
        if not d or not os.path.isdir(d):
            continue
        for n in names:
            if n and _readable_file(os.path.join(d, n)):
                return os.path.abspath(os.path.join(d, n))
    return ""


def find_first_under(roots, names):
    """`host-system`: a name found DIRECTLY in a root first (the .ps1), then anywhere BELOW one
    (the .sh's `find`, which is what reaches a multiarch subdirectory), name-major."""
    hit = find_first_in(roots, names)
    if hit:
        return hit
    for n in names:
        if not n:
            continue
        for r in roots:
            if not r or not os.path.isdir(r):
                continue
            for dirpath, dirnames, filenames in os.walk(r):
                dirnames.sort()
                if n in filenames and _readable_file(os.path.join(dirpath, n)):
                    return os.path.abspath(os.path.join(dirpath, n))
    return ""


def host_system_roots(cfg_host_libdir=None, environ=None):
    environ = os.environ if environ is None else environ
    roots = []
    for var in HOST_LIB_ENV:
        v = cfg_host_libdir if (var == "DSS_HOST_LIBDIR" and cfg_host_libdir is not None) \
            else environ.get(var, "")
        for p in (v or "").split(os.pathsep):
            if p and p not in roots:
                roots.append(p)
    for p in HOST_LIB_ROOTS:
        if p not in roots:
            roots.append(p)
    return roots


def unknown_library_provider_verdict(driver, leg, provider, tcl_names, z_names, known):
    """`(verdict, detail)` for a provider this driver has no dispatch arm for. A pure function
    of its arguments, ASCII, and `poisoned` -- the closed vocabulary's FAILURE class: it is a
    harness/catalogue defect, never an absent build input (an environmental skip would have
    let a bug exit 0)."""
    return ("poisoned",
            "HARNESS DEFECT: leg '%s' declares library provider '%s', which %s has no dispatch "
            "arm for, so this driver cannot obtain that leg's DECLARED inputs (tcl: %s / z: %s). "
            "ACQUISITION IS NOT DRIVER-LOCAL - pinned-archive is performed by harness_legs.py "
            "--acquire, on every host - so NO declared provider should reach this arm; reaching "
            "it means the catalogue or LIBRARY_PROVIDERS grew a provider and this driver was not "
            "extended in the same change. Add its dispatch arm here in that change: a provider "
            "the catalogue declares and the driver cannot dispatch is a capability that exists "
            "on paper only, this project's canonical silent harness bug. Known providers: %s "
            "(printed by harness_legs.py --library-providers, never copied here)."
            % (leg, provider, driver, tcl_names, z_names, known))


def _names(libs, key):
    return [str(n) for n in (libs.get(key) or []) if n]


def _indent(text, pad="      "):
    return "\n".join(pad + ln for ln in (text or "").rstrip().splitlines())


def acquire(run, leg):
    """`pinned-archive`: the leg's DECLARED archives, PINNED digests, materialised by the
    resolver (`--acquire`) -- never by this driver, never a fallback to "whatever is on this
    machine". -> (report or None, stage, rc, why). stderr kept verbatim in a per-leg log."""
    log_path = os.path.join(run.out_dir, "acquire-%s.log" % leg.label)
    r = run.resolver.call(["--acquire", leg.label])
    try:
        with open(log_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(r.err or "")
    except OSError:
        pass
    if r.rc != 0:
        return None, "--acquire", r.rc, (r.err or r.out).strip(), log_path
    try:
        report = json.loads(r.out)
        cache_dir = report["cacheDir"]
        libraries = report["libraries"]
        if not cache_dir or not libraries:
            raise KeyError("cacheDir/libraries empty")
    except (ValueError, KeyError, TypeError) as exc:
        return None, "reading the --acquire report", 1, ("the acquisition report could not be "
                                                         "read (%s):\n%s" % (exc, C.first_lines(r.out, 20))), log_path
    return report, "--acquire", 0, "", log_path


def resolve_leg(run, leg, known_providers):
    """Dispatch on the leg's declared provider and record the outcome on `leg`."""
    log = run.log
    libs = leg.build.get("libraries") or {}
    provider = libs.get("provider") or ""
    tcl_names, z_names = _names(libs, "tclNames"), _names(libs, "zNames")
    tcl = z = ""
    searched = ""
    if provider == "host-system":
        st = run.stage
        tnames = host_system_tcl_names(tcl_names, getattr(st, "tcl_lib_file", "") or "",
                                       getattr(st, "tcl_version", "") or "")
        roots = host_system_roots(run.cfg.host_libdir or None)
        tcl = find_first_under(roots, tnames)
        z = find_first_under(roots, z_names)
        searched = ("provider 'host-system'; tcl names tried: %s; zlib names tried: %s; roots: %s "
                    "— this machine has no copy of that target's tcl/zlib runtime; put one "
                    "anywhere and name the directory in DSS_HOST_LIBDIR"
                    % (" ".join(tnames) or "<none>", " ".join(z_names) or "<none>", " ".join(roots)))
    elif provider == "search-paths":
        # TCL_DLL / ZLIB_DLL: the operator's EXPLICIT override. One that points at nothing is a
        # HARD error, never a skip: stated intent that cannot be honoured is not downgraded.
        if run.cfg.tcl_dll:
            if not os.path.isfile(run.cfg.tcl_dll):
                C.die("TCL_DLL='%s' does not name a file — fix it or unset it (leg '%s')."
                      % (run.cfg.tcl_dll, leg.label))
            tcl = os.path.abspath(run.cfg.tcl_dll)
        if run.cfg.zlib_dll:
            if not os.path.isfile(run.cfg.zlib_dll):
                C.die("ZLIB_DLL='%s' does not name a file — fix it or unset it (leg '%s')."
                      % (run.cfg.zlib_dll, leg.label))
            z = os.path.abspath(run.cfg.zlib_dll)
        # The resolver already expanded every ${env:…} and DROPPED the candidates whose
        # variable is unset, so what arrives here is real directories only.
        paths = []
        for p in libs.get("searchPaths") or []:
            if p and p not in paths:
                paths.append(str(p))
        tcl = tcl or find_first_in(paths, tcl_names)
        z = z or find_first_in(paths, z_names)
        searched = ("provider 'search-paths'; tcl names tried: %s; zlib names tried: %s; paths "
                    "searched: %s — install them, add a path via DSS_PE_LIBDIR, or point TCL_DLL / "
                    "ZLIB_DLL straight at them"
                    % (" ".join(tcl_names) or "<none>", " ".join(z_names) or "<none>",
                       " ; ".join(paths) or "<none declared or all ${env:...} unset>"))
    elif provider == "pinned-archive":
        log.info("[%s] provider 'pinned-archive' — acquiring this leg's DECLARED libraries via "
                 "harness_legs.py --acquire (cached after the first run; a cold cache downloads)"
                 % leg.label)
        report, stage, rc, why, acq_log = acquire(run, leg)
        acq_dir = ""
        if report is not None:
            acq_dir = str(report["cacheDir"])
            leg.acq_dir = acq_dir
            leg.acq_libs = [(str(x.get("as") or ""), str(x.get("path") or ""))
                            for x in report["libraries"] if x]
            for rem in report.get("remediated") or []:
                if rem:
                    log.warn("[%s] cache REMEDIATED — %s (restored from the pinned, digest-verified "
                             "archive)" % (leg.label, rem))
            for rec in report["libraries"]:
                if not rec:
                    continue
                displaced = ("displacing the packager's own '%s'" % rec.get("embeddedIdentity")
                             if rec.get("embeddedIdentity") else
                             "the file carries no embedded identity")
                a_sha = ("archive sha256 %s" % rec["archiveSha256"] if rec.get("archiveSha256")
                         else "archive sha256 <not reported by this resolver>")
                f_sha = ("file sha256 %s" % rec["fileSha256"] if rec.get("fileSha256")
                         else "file sha256 <not reported by this resolver>")
                log.info("[%s] acquired %s -> %s" % (leg.label, rec.get("as"), rec.get("path")))
                log.info("[%s]   recorded as '%s' (%s)" % (leg.label, rec.get("importName"), displaced))
                log.info("[%s]   %s; %s; from %s" % (leg.label, a_sha, f_sha, rec.get("sourceUrl")))
            # ── THE SCRIPT LIBRARY THE ACQUIRED Tcl CANNOT RUN WITHOUT ──
            # ⚠ ABSENT IS REPORTED, NEVER ASSUMED BENIGN: it does not fail the build nor a
            # directly-named .test file (the MAIN interpreter is initialised by then); it kills
            # the TIER, whose permutations run every unit in a fresh SLAVE interpreter.
            script_dir = str(report.get(ACQ_SCRIPT_LIBRARY_KEY) or "")
            if script_dir:
                leg.tcl_script_dir = script_dir
                log.info("[%s] Tcl script library (acquired): %s  — TCL_LIBRARY is pointed here for "
                         "THIS leg's children only" % (leg.label, script_dir))
            else:
                log.warn("[%s] the acquisition report stages NO Tcl script library (report field "
                         "'%s' is empty)." % (leg.label, ACQ_SCRIPT_LIBRARY_KEY))
                log.warn("      An acquired libtcl bakes in ITS PACKAGER'S script-library path, which "
                         "does not exist on this")
                log.warn("      machine. Individual .test files will still run; the TIER will abort "
                         "at the first")
                log.warn("      `interp create` with \"Can't find a usable init.tcl\" and NO unit "
                         "will get a verdict.")
                log.warn("      The fix belongs in the leg's `pinned-archive` declaration + "
                         "harness_legs.py, not here.")
            # Picked out of the acquired cache by the leg's OWN declared names.
            tcl = find_first_in([acq_dir], tcl_names)
            z = find_first_in([acq_dir], z_names)
        else:
            if stage == "--acquire":
                log.warn("[%s] harness_legs.py --acquire %s FAILED (rc=%d):" % (leg.label, leg.label, rc))
            else:
                log.warn("[%s] the acquisition report from harness_legs.py could not be read (rc=%d)"
                         % (leg.label, rc))
            if why:
                print(_indent(why), file=sys.stderr, flush=True)
        searched = ("provider 'pinned-archive' (%s rc=%d); tcl names tried: %s; zlib names tried: "
                    "%s; acquired under: %s"
                    % (stage, rc, " ".join(tcl_names) or "<none>", " ".join(z_names) or "<none>",
                       acq_dir or "<nothing acquired — see %s>" % acq_log))
    else:
        verdict, detail = unknown_library_provider_verdict(
            "build_and_test.py", leg.label, provider, " ".join(tcl_names), " ".join(z_names),
            known_providers or "<resolver printed nothing - read LIBRARY_PROVIDERS in it>")
        run.ledger.marks_harness_defect(leg, verdict, detail)
        return
    leg.tcl_lib_any = tcl
    leg.z_lib_any = z
    if not tcl or not z:
        lost = "libtcl and libz" if not tcl and not z else ("libz" if tcl else "libtcl")
        run.ledger.marks_missing(leg, "this leg is NOT built on this host",
                                 "no %s for %s on this machine — %s" % (lost, leg.spec, searched))
        return
    leg.tcl_lib, leg.z_lib = tcl, z
    leg.lib_detail = "provider '%s'" % provider
    log.info("[%s] libs (%s): %s  +  %s" % (leg.label, provider, tcl, z))


def library_providers(run):
    """The provider vocabulary, printed by its OWNER (never re-typed here)."""
    r = run.resolver.call(["--library-providers"])
    if r.rc != 0:
        return ""
    return " ".join(ln.strip() for ln in r.out.splitlines() if ln.strip())


def tcl_coherence(run):
    """★ THE ONLY PER-LEG Tcl COHERENCE CHECK: the staged HEADER (the one Tcl input taken from
    the HOST) against every leg's libtcl, measured from each library's own BYTES. FATAL, never
    a warn: a warn builds a binary that links clean and misbehaves (✔MEASURED 2026-08-06: a 9.0
    header over pinned 8.6 libraries -> four K_SymbolUndefined)."""
    legs = [lg for lg in run.selected() if lg.tcl_lib_any]
    if not legs:
        return
    tcl_h = os.path.join(run.stage.tcl_inc, "tcl.h")
    call = ["--tcl-coherence", "--staged-tcl-header", tcl_h]
    for lg in legs:
        call += ["--leg-tcl-library", "%s=%s" % (lg.label, lg.tcl_lib_any)]
    r = run.resolver.call(call)
    out = (r.out or "") + (r.err or "")
    if r.rc != 0:
        C.die("Tcl HEADER/LIBRARY COHERENCE FAILED (rc=%d) — refusing to build.\n%s\n      The staged "
              "headers come from THIS HOST (%s); every leg's library comes from its own declared "
              "provider. This driver will not compile a fixture against one Tcl and link another."
              % (r.rc, _indent(out), run.stage.tcl_inc))
    if out.strip():
        print(_indent(out), flush=True)
    run.log.info("tcl coherence: the staged %s headers match the libtcl of all %d resolved leg(s) — "
                 "measured from each library's OWN bytes, not its file name"
                 % (run.stage.tcl_version, len(legs)))


def loadext_builder(run):
    """WHICH ARM STAGES THE LOADEXT HELPER, resolved once by the shared resolver (an
    unrecognised DSS_LOADEXT_HELPER is refused by name, never read as the default)."""
    r = run.resolver.call(["--loadext-builder"])
    if r.rc != 0 or not r.out.strip():
        C.die("DSS_LOADEXT_HELPER='%s' is not a builder this harness implements:\n%s"
              % (C.env("DSS_LOADEXT_HELPER"), _indent((r.err or r.out).strip())))
    builder = r.out.strip().splitlines()[-1].strip()
    if builder == "reference":
        run.log.warn("DSS_LOADEXT_HELPER=reference — the loadext helper will be STAGED from each leg's")
        run.log.warn("      VERIFIED target C compiler instead of from DSS. This is the CONTROL arm: it "
                     "makes")
        run.log.warn("      the corpus itself the differential for 'is a loadext-* red the fixture or "
                     "the helper?',")
        run.log.warn("      and it re-introduces a host dependency ON PURPOSE — a leg with no such "
                     "compiler here")
        run.log.warn("      records skipped-build-input-missing (environmental; the default would have "
                     "run it).")
    return builder


def resolve_target_cc(run, leg):
    """The leg's verified CONTROL compiler: `<argv0>\\t…\\t<argvN>\\t<triple>` (the triple LAST;
    a candidate may be an argv). -> True when resolved. A failure is INFORMATION (kept in
    `leg.cc_why`), never a verdict and never fatal."""
    r = run.resolver.call(["--resolve-target-cc", leg.label])
    leg.cc_why = " ".join((r.err or "").split())
    if r.rc != 0:
        leg.cc, leg.cc_machine = [], ""
        return False
    line = (r.out or "").rstrip("\r\n")
    fields = line.split("\t")
    if len(fields) < 2 or not [f for f in fields[:-1] if f]:
        leg.cc_why = ("the resolver exited 0 but did not answer in the declared <argv>TAB…TAB<triple> "
                      "shape (got %d field(s): '%s')" % (len(fields), line))
        leg.cc, leg.cc_machine = [], ""
        return False
    leg.cc_machine = fields[-1].strip()
    leg.cc = [f for f in fields[:-1] if f]
    return True


def step6(run, pkg_install=None):
    """Resolve every SELECTED leg's inputs, check Tcl coherence, pick the loadext arm, resolve
    each runnable leg's control compiler; refuse when NOT ONE leg can be built."""
    log = run.log
    known = library_providers(run)
    for leg in run.selected():
        resolve_leg(run, leg, known)
    tcl_coherence(run)
    run.loadext_builder = loadext_builder(run)
    for leg in run.selected():
        if C.run_is_skipped(leg) or not leg.tcl_lib:
            continue
        pkg = ((leg.build.get("targetCc") or {}).get("package") or "")
        if not resolve_target_cc(run, leg) and run.loadext_builder == "reference" and pkg \
                and pkg_install is not None:
            log.warn("[%s] DSS_LOADEXT_HELPER=reference and no candidate compiler on PATH targets %s "
                     "— trying to install %s" % (leg.label, leg.spec, pkg))
            log.info("      %s" % leg.cc_why)
            try:
                pkg_install(pkg)
            except C.HarnessDie as exc:
                log.warn("[%s] could not install %s: %s" % (leg.label, pkg, exc))
            resolve_target_cc(run, leg)
        if not leg.cc:
            cands = " ".join(str(c) for c in ((leg.build.get("targetCc") or {}).get("candidates")
                                              or [])) or "<none declared>"
            log.info("[%s] no CONTROL compiler here (tried %s) — the loadext helper will be built by "
                     "DSS for %s, which needs nothing from this machine."
                     % (leg.label, cands, leg.build.get("sharedLibFormat") or
                        "<no sharedLibFormat declared>"))
            if leg.cc_why.strip():
                log.info("      the resolver's ladder: %s" % leg.cc_why)
            continue
        log.info("[%s] control cc: %s — it reports '%s', which is %s's arch+OS (asked, not assumed)"
                 % (leg.label, " ".join(leg.cc), leg.cc_machine, leg.spec))
        if leg.cc_why.strip():
            log.info("      candidates passed over: %s" % leg.cc_why)
    ready = [lg for lg in run.selected() if lg.tcl_lib]
    if not ready:
        C.die("NOT ONE of the %d selected leg(s) could resolve its declared (tcl, z) libraries on "
              "this host.\n      Every leg is reported above with the exact names and roots it "
              "searched. There is nothing\n      left to build, so this is fatal rather than a run "
              "that would report only skips and exit 0." % len(run.selected()))
    log.ok("build inputs resolved for %d of %d selected leg(s)" % (len(ready), len(run.selected())))
