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
  * default           refresh the located Release tree (`cmake --build <its tree> --config Release
                      --target dsscp`), or configure and build `build/rel` when nothing is
                      eligible. A FAILED refresh is FATAL -- falling back to the binary found is
                      precisely the defect of a reused compiler older than the sources it compiles.
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
import sys

import sqlite_common as C

Candidate = collections.namedtuple("Candidate", ["path", "mtime", "type", "tree", "source", "detail"])
Compiler = collections.namedtuple("Compiler", ["path", "type", "source", "detail", "tree", "origin",
                                                "built", "build_type_note"])

# The roots searched for an existing binary, each with `bin/dss` below it -- the PowerShell
# driver's list, unchanged (the harness's own build trees; a DssHarness variant tree is built
# under a developer environment a plain refresh cannot reproduce, so it is named by DSS_BIN).
SEARCH_ROOTS = ("build/rel", "build/dbg", "build-rel", "build", "build-dbg")
BINARY_NAMES = ("dsscp.exe", "dsscp")

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
    try:
        mtime = os.path.getmtime(binary)
    except OSError:
        mtime = 0.0
    return Candidate(os.path.abspath(binary), mtime, btype, tree, source, detail)


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


def format_candidates(cands):
    if not cands:
        return "        <none>"
    return "\n".join("        %s\n            build type: %s   built: %s\n            read from : %s"
                     % (c.path, c.type, _stamp(c.mtime), c.source) for c in cands)


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


def _cmake_build(tree, jobs):
    """The real refresh: an incremental Release build of the `dsscp` target, output to the log."""
    C.LOG.info("cmake --build %s --config Release --target dsscp -j %d" % (tree, jobs))
    r = C.capture(["cmake", "--build", tree, "--config", "Release", "--target", "dsscp",
                   "-j", str(jobs)], merge=True)
    for line in C.last_lines(r.out, 30).splitlines():
        C.LOG.info("   " + line)
    return r.rc


def obtain(repo_root, jobs, allow_nonrelease, log=C.LOG, invoke_build=_cmake_build):
    """-> Compiler: the one this run uses, by the union policy above, through the ONE gate."""
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
            refresh_located(info.tree, info.path, _stamp(info.mtime), jobs, invoke_build)
            # RE-READ rather than assume the build moved it: the timestamp reported must be the
            # one on disk NOW, and a build that landed elsewhere must not be reported as this one.
            cands, searched = find_candidates(repo_root)
            info = select_compiler(cands, allow_nonrelease)
            origin = "LOCATED under an eligible build root, then REBUILT by this run (incremental)"
    if info is None or not os.path.isfile(info.path):
        C.die("no RELEASE dsscp binary after the build step.\n      %s\n      candidates found (build "
              "type read from each tree's CMakeCache.txt):\n%s"
              % (search_note(searched), format_candidates(cands)))
    built = _stamp(info.mtime)
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
    tree is usually kept -- a DSS_BIN from another checkout reaches this line too)."""
    tree = compiler.tree or os.path.join(repo_root, "build", "rel")
    return ("cmake --build %s --config Release --target dsscp   (or: dssharness build --legs "
            "<a release leg>)" % tree)
