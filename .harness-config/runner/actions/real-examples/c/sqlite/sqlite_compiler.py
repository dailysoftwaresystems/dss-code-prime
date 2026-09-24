#!/usr/bin/env python3
"""sqlite_compiler.py -- Step 5 of the SQLite corpus harness: WHICH dsscp this run uses, proved.

ONE policy on every host since 2026-09-21 (lane mig, part 4; the transcription is P4-11.2's
R78–R89 / S5-* row). The two drivers it replaces disagreed: the bash driver ALWAYS configured
and rebuilt `build/rel` and read neither `DSS_BIN` nor `SKIP_DSS_BUILD`; the PowerShell driver
honoured `DSS_BIN`, reused a Release binary under `SKIP_DSS_BUILD=1`, and otherwise REFRESHED the
located Release tree (or built one). The union is the PowerShell policy, because it is the one
that can be told what to do and still proves what it got:
  * `DSS_BIN`         an explicitly named binary is USED, never searched for; a name that is not a
                      file is a refusal (never silently replaced by a searched binary);
  * `SKIP_DSS_BUILD=1` reuse the eligible candidate, never build; none eligible is a refusal;
  * default           refresh the located Release tree through ITS OWNER (`refresh_argv`): a tree
                      DssHarness built for a leg whose toolchain declares a developer environment
                      (MSVC) is rebuilt by `dssharness build --legs <that leg>`, which enters it;
                      any other tree by `cmake --build <its tree> --config Release --target
                      dsscp`. With nothing eligible, configure and build `build/rel`. A FAILED
                      refresh is FATAL -- falling back to the binary found is precisely the defect
                      of a reused compiler older than the sources it compiles.
Every branch reaches ONE gate: the build type is READ from the binary's own tree (never inferred
from a directory name or from the command that preceded it), printed beside the path, and a
non-Release binary is refused unless `DSS_ALLOW_NONRELEASE_COMPILER` says otherwise -- which then
marks every report line. Then the config tree is PINNED (`DSS_CONFIG_ROOT` names the checkout
that CONTAINS `src/dss-config`), and the pair is proved current: one `speedtest1_bench.py
--preflight-dss` probe per distinct selected target, where ONLY exit 1 accuses the compiler.

The build-type decision itself is `read_build_type` in `profile-compile/profile-compile-support.py`
(its one owner, also used by `compile-bench`); this module only adds WHERE the tree is (for the
refresh and the rebuild instruction) and the multi-config note the PowerShell driver printed.
"""
from __future__ import annotations

import collections
import datetime
import importlib.util
import os
import re
import shutil
import sys

import sqlite_common as C

Candidate = collections.namedtuple("Candidate", ["path", "mtime", "type", "tree", "source", "detail",
                                                 "image"])
Compiler = collections.namedtuple("Compiler", ["path", "type", "source", "detail", "tree", "origin",
                                                "built", "build_type_note"])

# The roots searched for an existing binary, each with `bin/dss` below it -- the PowerShell
# driver's list, unchanged (the harness's own build trees; a DssHarness variant tree is built
# under a developer environment a plain refresh cannot reproduce, so it is named by DSS_BIN).
SEARCH_ROOTS = ("build/rel", "build/dbg", "build-rel", "build", "build-dbg")
BINARY_NAMES = ("dsscp.exe", "dsscp")
# ★ THE COMPILER'S CODE IS NOT ALWAYS THE FILE THAT RUNS. The build puts it in a shared library of the
# executable's own name beside a small launcher. ✔MEASURED 2026-09-22: `dsscp.exe` 11 KB, built 13:58,
# beside `dsscp.dll` rebuilt 22:56 by a later build that left the launcher alone -- and the report said
# "built 13:58" (the WSL tree's pair is `dsscp` + `libdsscp.so`). So a candidate's stamp is its CODE's:
# the newest of these libraries that exist beside it, or the executable itself when none does (then it
# IS the code). The launcher's own time is never the stamp -- it orders the candidates too, and a
# rebuilt launcher over older code must not win the selection. ✔READ 2026-09-23, one spelling per
# toolchain family this project builds with: MSVC `dsscp.dll`, MinGW `libdsscp.dll` (build/mig,
# build/dbg), ELF `libdsscp.so` (the WSL tree), Mach-O `libdsscp.dylib`.
COMPANION_LIBRARIES = ("{stem}.dll", "lib{stem}.dll", "lib{stem}.so", "lib{stem}.dylib")
# DssHarness's own marker in a build directory it made: line 2 is the leg variant the tree was built
# for (✔READ in build/x86_64-msvc-release: `clean`, then `x86_64-msvc-release`, then input digests).
HARNESS_BUILD_MARKER = ".harness-build"
HARNESS_NAMES = ("dssharness", "DssHarness")

_BT_READER = None


def _read_build_type():
    global _BT_READER
    if _BT_READER is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(C.HERE))),
                            "profile-compile", "profile-compile-support.py")
        if not os.path.isfile(path):
            C.die("%s is missing. The build-type assertion is not optional -- a non-Release timing "
                  "published beside Release ones is worse than no timing -- so this refuses rather "
                  "than guess how the compiler was built." % path)
        spec = importlib.util.spec_from_file_location("dss_profile_compile_support", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        if not hasattr(mod, "read_build_type"):
            C.die("%s no longer exports read_build_type. Fix that file rather than growing a second "
                  "copy of the check here." % path)
        _BT_READER = mod.read_build_type
    return _BT_READER


def build_tree(binary):
    """The nearest ancestor of `binary` holding a CMakeCache.txt, or ""."""
    d = os.path.dirname(os.path.abspath(binary))
    while d:
        if os.path.isfile(os.path.join(d, "CMakeCache.txt")):
            return d
        up = os.path.dirname(d)
        if up == d:
            return ""
        d = up
    return ""


def build_type(binary):
    """-> Candidate for `binary`: its TYPE and where that was read (`read_build_type`), its tree,
    and -- for a multi-config tree that ALSO carries CMAKE_BUILD_TYPE -- the note that the
    generator ignores that entry (printed so the disagreement is visible)."""
    btype, source = _read_build_type()(binary)
    tree = build_tree(binary)
    detail = ""
    if tree:
        try:
            with open(os.path.join(tree, "CMakeCache.txt"), encoding="utf-8", errors="replace") as fh:
                cache = fh.read()
        except OSError:
            cache = ""
        cfgs = re.search(r"^CMAKE_CONFIGURATION_TYPES:[^=]*=(.*)$", cache, re.M)
        bt = re.search(r"^CMAKE_BUILD_TYPE:[^=]*=(.*)$", cache, re.M)
        if cfgs and cfgs.group(1).strip() and bt and bt.group(1).strip():
            detail = ("the cache also carries CMAKE_BUILD_TYPE=%s, which a MULTI-config generator "
                      "IGNORES -- it is not the answer here and is printed only so the "
                      "disagreement is visible" % bt.group(1).strip())
    stamped = []
    for image in image_files(binary):
        try:
            stamped.append((os.path.getmtime(image), image))
        except OSError:
            pass
    mtime, image = max(stamped) if stamped else (0.0, os.path.abspath(binary))
    return Candidate(os.path.abspath(binary), mtime, btype, tree, source, detail, image)


def image_files(binary):
    """The files holding the compiler's CODE: each `COMPANION_LIBRARIES` spelling of the executable's
    own name that exists beside it -- or, when none does, the executable itself."""
    path = os.path.abspath(binary)
    folder = os.path.dirname(path)
    stem = os.path.splitext(os.path.basename(path))[0]
    beside = [os.path.join(folder, pattern.format(stem=stem)) for pattern in COMPANION_LIBRARIES]
    return [p for p in beside if os.path.isfile(p)] or [path]


def is_release(btype):
    """Case-INSENSITIVE on purpose: CMake uppercases the build type to find
    CMAKE_<LANG>_FLAGS_<CFG>, so `release` selects the identical flags as `Release`."""
    return (btype or "").lower() == "release"


def search_roots(repo_root):
    """The fixed build roots, then EVERY `build/<name>` one level down, in name order. DssHarness
    keys its build directories by leg variant (`build/<processor>-<toolchain>-<config>`), and a
    Release tree kept under any other name (MEASURED 2026-08-26: `build/bench-rel` on the arm64
    VPS) was invisible to a list of names -- the retired bash benchmark searched every
    `build/*/` for that reason, and this is now the ONE place that decides where to look."""
    roots = list(SEARCH_ROOTS)
    build = os.path.join(repo_root, "build")
    try:
        names = sorted(n for n in os.listdir(build) if os.path.isdir(os.path.join(build, n)))
    except OSError:
        names = []
    for n in names:
        if "build/" + n not in roots:
            roots.append("build/" + n)
    return roots


def find_candidates(repo_root):
    """-> (candidates newest first, the directories searched). Every root `search_roots` names;
    both executable spellings on every host; each root's `bin/dss` walked at any depth (a
    multi-config generator lands the binary in a per-config subdirectory); a file seen twice is
    one candidate. WHERE to look is decided here only; HOW a binary is judged is `build_type`."""
    searched, seen, cands = [], set(), []
    for r in search_roots(repo_root):
        bin_dir = os.path.join(repo_root, *r.split("/"), "bin", "dss")
        searched.append(bin_dir)
        if not os.path.isdir(bin_dir):
            continue
        for dirpath, dirs, files in os.walk(bin_dir):
            dirs.sort()
            for f in sorted(files):
                if f in BINARY_NAMES:
                    full = os.path.abspath(os.path.join(dirpath, f))
                    key = os.path.normcase(full)
                    if key in seen:
                        continue
                    seen.add(key)
                    cands.append(build_type(full))
    cands.sort(key=lambda c: -c.mtime)
    return cands, searched


def select_compiler(cands, allow_nonrelease):
    """The newest RELEASE candidate; with `allow_nonrelease`, the newest of any when no Release
    exists. The switch makes a non-Release binary ELIGIBLE, never PREFERRED."""
    for c in cands:
        if is_release(c.type):
            return c
    if allow_nonrelease and cands:
        return cands[0]
    return None


def _stamp(t):
    return datetime.datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M:%S") if t else "<unknown>"


def built_stamp(cand):
    """When the candidate's CODE was built, naming the image file that says so."""
    return "%s (%s)" % (_stamp(cand.mtime), os.path.basename(cand.image or cand.path))


def format_candidates(cands):
    if not cands:
        return "        <none>"
    return "\n".join("        %s\n            build type: %s   built: %s\n            read from : %s"
                     % (c.path, c.type, built_stamp(c), c.source) for c in cands)


def search_note(searched):
    return ("searched at any depth under: %s (for dsscp.exe or dsscp) -- the fixed build roots, then "
            "every build/<name>, which is where DssHarness builds each leg variant. A multi-config "
            "generator lands it in a per-config subdirectory (bin/dss/Release); a single-config one in bin/dss. "
            "Only a RELEASE binary is eligible, and each candidate's build type is read from its own "
            "tree's CMakeCache.txt. Set DSS_BIN to name a binary outside these roots."
            % "; ".join(searched))


def refresh_located(tree, binary, built_when, jobs, invoke_build):
    """Rebuild the tree a LOCATED binary came from. `invoke_build(tree, jobs) -> int` (injected, so
    the contract tests drive this without cmake). No tree, a non-integer answer or a non-zero
    exit are each FATAL -- never a fallback to the binary that was found."""
    if not tree:
        C.die("the located Release compiler's build TREE could not be determined, so this run cannot "
              "refresh it\n      and cannot know that its compiler embodies the sources it is about "
              "to compile.\n      binary : %s (built %s)\n      Name one with DSS_BIN, or build a "
              "Release tree (cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release)." % (binary, built_when))
    rc = invoke_build(tree, jobs)
    if not isinstance(rc, int) or isinstance(rc, bool):
        C.die("the injected build did not return an EXIT CODE, so this run cannot tell a successful "
              "rebuild from a failed one.\n      tree     : %s\n      returned : %r" % (tree, rc))
    if rc != 0:
        C.die("the LOCATED Release compiler could not be rebuilt (exit %d), so this run cannot know "
              "that its\n      compiler embodies the sources it is about to compile.\n"
              "      tree   : %s\n      binary : %s (built %s)\n"
              "      This is FATAL rather than a fallback: reusing the binary found here is precisely "
              "the defect\n      of a reused release binary older than the sources it compiles.\n"
              "      Repair the tree (cmake -S . -B %s -DCMAKE_BUILD_TYPE=Release), point this run at "
              "another\n      one, or set SKIP_DSS_BUILD=1 to reuse a binary DELIBERATELY — that path "
              "says so in every\n      report line of the run." % (rc, tree, binary, built_when, tree))
    return tree


def harness_executable(environ=None):
    """The DssHarness executable -> a path, or "" when none is installed: PATH first, then the tool
    installer's own directory (`<home>/.dotnet/tools`), both spellings of the name -- the places the
    root CMakeLists.txt's `find_program` searches, for the reason recorded there (the directory is on
    a LOGIN path only, and the file's case followed the release). Both searches are `shutil.which`'s,
    so the host's own executable suffixes (PATHEXT on Windows) decide the file name, as `find_program`
    does -- no suffix is spelled here."""
    env = os.environ if environ is None else environ
    for name in HARNESS_NAMES:
        hit = shutil.which(name, path=env.get("PATH", os.defpath))
        if hit:
            return hit
    for var in ("HOME", "USERPROFILE"):
        home = env.get(var, "")
        if not home:
            continue
        for name in HARNESS_NAMES:
            hit = shutil.which(name, path=os.path.join(home, ".dotnet", "tools"))
            if hit:
                return hit
    return ""


def _host_leg_os(host_os):
    """The driver's host-OS word -> the word a DssHarness leg declares (`darwin` is `macos` there)."""
    return {"darwin": "macos"}.get(host_os, host_os)


def refresh_argv(repo_root, tree, jobs, host_os=None, harness=None):
    """-> (argv, why): HOW a located Release tree is refreshed, decided by the tree's OWNER.

    ★ A TREE DSSHARNESS BUILT FOR A LEG WHOSE TOOLCHAIN DECLARES A DEVELOPER ENVIRONMENT IS REBUILT BY
    DSSHARNESS. ✔MEASURED (lane mig, 2026-09-22): without `DSS_BIN` on a Windows host, Step 5 selects
    `build/x86_64-msvc-release` -- MSVC under the Visual Studio developer environment -- and a plain
    `cmake --build` of it needs that environment whenever anything is stale (`cl.exe` finds no headers
    without it), so the refresh failed. The tree's marker names the variant it was built for; the leg
    declared for that variant on this host names its toolchain; the toolchain's `developerEnvironment`
    in `.harness-config/config.json` says whether one is needed -- every fact read, none typed here.
    ⚠ `dssharness build` builds the leg's whole project, and rebuilds from CLEAN when any input changed
    (DssHarness report #2), so this refresh can cost a full MSVC build where an incremental `cmake
    --build` would not -- that cost is the tool's to fix, and the instruction stays correct.
    Every other tree -- no marker, a toolchain with no developer environment, or a variant no single
    leg on this host declares -- is refreshed as it always was: `cmake --build <tree> --config Release
    --target dsscp`, whose failure is as FATAL as the tool's."""
    plain = (["cmake", "--build", tree, "--config", "Release", "--target", "dsscp", "-j", str(jobs)])
    marker = os.path.join(tree, HARNESS_BUILD_MARKER)
    try:
        with open(marker, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return plain, "no DssHarness marker in the tree: a plain incremental build"
    variant = lines[1].strip() if len(lines) > 1 else ""
    cfg = _harness_config(repo_root, tree)
    legs = cfg.get("legs") or {}
    toolchains = cfg.get("toolchains") or {}
    want_os = _host_leg_os(host_os or C.host_os())
    named = sorted(name for name, leg in legs.items() if isinstance(leg, dict)
                   and leg.get("os") == want_os
                   and "%s-%s-%s" % (leg.get("processor"), leg.get("toolchain"), leg.get("config")) == variant)
    if len(named) != 1:
        return plain, ("DssHarness built this tree for variant %r, which %d declared leg(s) on this host "
                       "name (%s): a plain incremental build" % (variant, len(named), ", ".join(named) or "none"))
    leg = named[0]
    toolchain = legs[leg].get("toolchain")
    environment = (toolchains.get(toolchain) or {}).get("developerEnvironment")
    if not environment:
        return plain, ("DssHarness built this tree for leg %s, whose toolchain %s declares no developer "
                       "environment: a plain incremental build" % (leg, toolchain))
    exe = harness or harness_executable()
    if not exe:
        C.die("the located Release tree %s was built by DssHarness for leg %s, whose toolchain %s needs the "
              "developer environment %r -- a plain `cmake --build` outside it cannot compile -- and no "
              "DssHarness is installed here to enter it. Install it (`dotnet tool install --global "
              "DssHarness`), name a compiler with DSS_BIN, or set SKIP_DSS_BUILD=1 to reuse one on purpose."
              % (tree, leg, toolchain, environment))
    return ([exe, "build", "--legs", leg, "-C", repo_root, "--no-prompt"],
            "DssHarness built this tree for leg %s, whose toolchain %s needs the developer environment "
            "%r: the tool rebuilds it inside that environment" % (leg, toolchain, environment))


def _refresh_build(repo_root, tree, jobs, log=C.LOG):
    """The real refresh: `refresh_argv`'s command for the tree, its output to the log -> its exit code."""
    argv, why = refresh_argv(repo_root, tree, jobs)
    log.info("refresh: %s" % why)
    log.info(" ".join(argv))
    r = C.capture(argv, merge=True)
    for line in C.last_lines(r.out, 30).splitlines():
        log.info("   " + line)
    return r.rc


def obtain(repo_root, jobs, allow_nonrelease, log=C.LOG, invoke_build=None):
    """-> Compiler: the one this run uses, by the union policy above, through the ONE gate.
    `invoke_build(tree, jobs) -> exit code` refreshes a located tree; by default through the tree's
    owner (`refresh_argv`) -- injected by the contract tests, which drive this without a build."""
    if invoke_build is None:
        invoke_build = lambda tree, j: _refresh_build(repo_root, tree, j, log)  # noqa: E731
    origin = "origin UNSTATED — a branch of Step 5 did not say how it obtained this binary"
    info, cands, searched = None, [], []
    named = C.env("DSS_BIN").strip()
    if named:
        if not os.path.isfile(named):
            C.die("DSS_BIN='%s' does not name an existing file. It is an override, not a hint: it is "
                  "never silently replaced by a searched binary." % named)
        info = build_type(named)
        origin = "named by DSS_BIN — NOT built by this run"
        log.info("using DSS_BIN — %s" % info.path)
    elif C.env("SKIP_DSS_BUILD") == "1":
        cands, searched = find_candidates(repo_root)
        info = select_compiler(cands, allow_nonrelease)
        if info is None:
            reason = ("NO dsscp binary exists under any eligible root at all." if not cands else
                      "Every candidate below was rejected on BUILD TYPE — only a Release compiler "
                      "is eligible.")
            hatch = (", or set DSS_ALLOW_NONRELEASE_COMPILER=1 to reuse the newest candidate above "
                     "ANYWAY — with every report line of the run saying so"
                     if cands and not allow_nonrelease else "")
            C.die("SKIP_DSS_BUILD=1 but no eligible dsscp exists, and SKIP_DSS_BUILD forbids building "
                  "one.\n      %s\n      candidates found (build type read from each tree's "
                  "CMakeCache.txt):\n%s\n      %s\n      Build one (cmake -B build/rel "
                  "-DCMAKE_BUILD_TYPE=Release && cmake --build build/rel --target dsscp),\n      or "
                  "unset SKIP_DSS_BUILD and let this step do it%s."
                  % (reason, format_candidates(cands), search_note(searched), hatch))
        origin = "REUSED under SKIP_DSS_BUILD=1 — NOT built by this run"
        log.info("SKIP_DSS_BUILD=1 — reusing %s" % info.path)
    else:
        cands, searched = find_candidates(repo_root)
        info = select_compiler(cands, allow_nonrelease)
        if info is None:
            if cands:
                log.warn("no RELEASE dsscp under any eligible root — the following exist and were "
                         "REJECTED on build type:\n" + format_candidates(cands))
            else:
                log.info("no dsscp binary under any eligible root")
            rel = "build-rel" if os.path.isdir(os.path.join(repo_root, "build-rel")) else "build/rel"
            bdir = os.path.join(repo_root, *rel.split("/"))
            log.info("configuring + building Release (%s)" % rel)
            # -DCMAKE_BUILD_TYPE=Release on EVERY configure: an existing tree configured Debug would
            # otherwise keep its cached answer, and the gate below re-READS the result regardless.
            C.run_checked(["cmake", "-S", repo_root, "-B", bdir, "-DCMAKE_BUILD_TYPE=Release"],
                          "cmake configure")
            C.run_checked(["cmake", "--build", bdir, "--config", "Release", "--target", "dsscp",
                           "-j", str(jobs)], "dsscp build")
            cands, searched = find_candidates(repo_root)
            info = select_compiler(cands, allow_nonrelease)
            origin = "BUILT by this run"
        else:
            log.info("refreshing the located Release compiler (%s) — a located binary is not "
                     "evidence it was built from these sources" % info.tree)
            refresh_located(info.tree, info.path, built_stamp(info), jobs, invoke_build)
            # RE-READ rather than assume the build moved it: the timestamp reported must be the
            # one on disk NOW, and a build that landed elsewhere must not be reported as this one.
            cands, searched = find_candidates(repo_root)
            info = select_compiler(cands, allow_nonrelease)
            origin = "LOCATED under an eligible build root, then REBUILT by this run (incremental)"
    if info is None or not os.path.isfile(info.path):
        C.die("no RELEASE dsscp binary after the build step.\n      %s\n      candidates found (build "
              "type read from each tree's CMakeCache.txt):\n%s"
              % (search_note(searched), format_candidates(cands)))
    built = built_stamp(info)
    log.info("compiler  : %s  (built %s)" % (info.path, built))
    log.info("build type: %s" % info.type)
    log.info("  read from: %s" % info.source)
    if info.detail:
        log.info("  note     : %s" % info.detail)
    note = "  (compiler build type: %s)" % info.type
    if not is_release(info.type):
        if not allow_nonrelease:
            C.die("this run would be timed against a NON-RELEASE compiler. It REFUSES rather than "
                  "proceed quietly.\n      compiler   : %s\n      build type : %s\n      read from  : "
                  "%s\n      note       : %s\n      A Debug dsscp is -g, no -O and no NDEBUG: it "
                  "compiles the same program correctly and takes several times as long, and the "
                  "difference lands in whatever the run is being read for.\n      Either build a "
                  "Release compiler (cmake -B build/rel -DCMAKE_BUILD_TYPE=Release && cmake --build "
                  "build/rel --target dsscp), or set DSS_ALLOW_NONRELEASE_COMPILER=1 to proceed with "
                  "THIS binary — the run then says so on every report line."
                  % (info.path, info.type, info.source, info.detail or "(none)"))
        note = "  (compiler build type: %s — NOT Release, DSS_ALLOW_NONRELEASE_COMPILER=1)" % info.type
        log.warn("DSS_ALLOW_NONRELEASE_COMPILER=1 — proceeding with a %s compiler. TIMINGS FROM THIS "
                 "RUN ARE NOT COMPARABLE with any other run, which always times a Release compiler."
                 % info.type)
    return Compiler(info.path, info.type, info.source, info.detail, info.tree, origin, built, note)


def pin_config_root(repo_root, log=C.LOG):
    """`DSS_CONFIG_ROOT` (the operator's, honoured) or the repository root -- the directory that
    CONTAINS `src/dss-config` -- verified, exported for every child, and returned. A set-but-miss
    would fall through to the compiler's cwd walk SILENTLY by documented design."""
    root = C.env("DSS_CONFIG_ROOT").strip() or repo_root
    if not os.path.isdir(os.path.join(root, "src", "dss-config")):
        C.die("no dss config tree at %s\n      DSS_CONFIG_ROOT names the checkout root that CONTAINS "
              "src/dss-config, not that directory itself." % os.path.join(root, "src", "dss-config"))
    os.environ["DSS_CONFIG_ROOT"] = root
    log.info("config    : %s  (pinned for every compile in this run)"
             % os.path.join(root, "src", "dss-config"))
    return root


def assert_current(core, compiler, config_root, specs, rebuild_cmd, python=sys.executable):
    """One `--preflight-dss` probe per DISTINCT target spec (deduplicated, in order). rc 0 = proved;
    ONLY rc 1 accuses the compiler (stale-binary refusal); any other rc -- or a missing core -- is
    COULD NOT RUN, which never tells the operator to rebuild. -> the specs proved, ", "-joined."""
    if not os.path.isfile(core):
        C.die("the compiler-currency pre-flight CANNOT RUN, so this run does not know whether its "
              "compiler is current.\n      missing   : %s\n      It holds the ONE implementation of "
              "that check (--preflight-dss), shared with the benchmark\n      drivers. This is a fact "
              "about the checkout, NOT about the compiler: nothing here says the\n      binary is "
              "stale, and rebuilding it would not change this answer." % core)
    uniq = list(dict.fromkeys(s for s in specs if s))
    if not uniq:
        C.die("no leg target specs to pre-flight the compiler against — the resolved plan carries no "
              "specs, so the currency check would examine nothing and report success.")
    checked = []
    for spec in uniq:
        r = C.capture([python, core, "--preflight-dss", compiler.path, "--config-root", config_root,
                       "--preflight-target", spec], merge=True, env_=C.child_env(python=True))
        text = "\n".join("      " + ln for ln in r.out.strip().splitlines())
        if r.rc == 0:
            checked.append(spec)
            continue
        if r.rc != 1:
            C.die("the compiler-currency pre-flight COULD NOT RUN for target %s (exit %d), so this run "
                  "does not know whether its compiler is current.\n%s\n      compiler  : %s\n      "
                  "built     : %s\n      origin    : %s\n      config    : %s\n      NOTHING above "
                  "says that binary is stale. Fix what the check names and ask again."
                  % (spec, r.rc, text, compiler.path, compiler.built, compiler.origin,
                     os.path.join(config_root, "src", "dss-config")))
        C.die("THE COMPILER CANNOT COMPILE THREE LINES against this run's own config tree, for target "
              "%s.\n      This run is REFUSED here rather than after it has compiled every "
              "translation unit it\n      was asked to and reported every result poisoned (a "
              "reused binary older than its config).\n%s\n      compiler  : %s\n      built     : "
              "%s\n      origin    : %s\n      config    : %s\n      target    : %s\n      An "
              "error[C_Invalid...] / C_MalformedJson diagnostic naming an unknown key, pragma effect "
              "or\n      attribute effect is the STALE-BINARY signature: the config tree has grown "
              "vocabulary this\n      binary does not know, and refusing an unrecognised key is "
              "correct compiler behaviour.\n      REBUILD IT: %s\n      ⚠ REUSING A BINARY ON "
              "PURPOSE DOES NOT EXEMPT IT FROM THIS CHECK (the driver's SKIP_DSS_BUILD=1,\n      a "
              "DSS_BIN, the benchmark's --dss). Each is an instruction not to BUILD, never an "
              "instruction\n      to TRUST: a binary that fails here is unusable and the run stops "
              "with this message instead\n      of reusing it."
              % (spec, text, compiler.path, compiler.built, compiler.origin,
                 os.path.join(config_root, "src", "dss-config"), spec, rebuild_cmd))
    return ", ".join(checked)


def rebuild_command(compiler, repo_root):
    """The rebuild instruction for THIS binary's own tree (never a spelling of where a Release
    tree is usually kept -- a DSS_BIN from another checkout reaches this line too): the command
    `refresh_argv` would run for it, so the advice and the refresh cannot disagree."""
    tree = compiler.tree or os.path.join(repo_root, "build", "rel")
    argv, _why = refresh_argv(repo_root, tree, "<jobs>", harness=harness_executable() or "dssharness")
    return " ".join(argv)


def _harness_config(repo_root, tree):
    """`.harness-config/config.json` of the tree under test -- REQUIRED once a tree carries DssHarness's
    marker: which leg built it, and whether that leg needs a developer environment, are read there."""
    ot = C.owning_tree_module()
    try:
        return ot.load_jsonc(os.path.join(repo_root, ".harness-config", "config.json"))
    except ot.Refusal as exc:
        C.die("the located Release tree %s was built by DssHarness, and how to refresh it is decided by "
              "the leg declared for it in .harness-config/config.json, which cannot be read: %s"
              % (tree, exc))
