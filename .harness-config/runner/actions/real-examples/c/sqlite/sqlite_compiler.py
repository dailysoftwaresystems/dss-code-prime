#!/usr/bin/env python3
"""sqlite_compiler.py -- Step 5 of the SQLite corpus harness: the dsscp this run was GIVEN, proved.

★★ THE COMPILER IS NAMED, AND NAMING IT IS MANDATORY (2026-09-26). Step 5 compiles with exactly the
binary `--dss` names -- what every step of the sqlite action passes: the leg's own `{product}`, the
one file DssHarness declares the leg's build makes, which the runner's `requireBuild` has just built
-- or, by hand, `DSS_BIN` (`sqlite_common.Config` reads both, once). A run that names none is
REFUSED by name, before Step 0 spends anything (`named_compiler`, called first by `run_all`); a
name that is not a file is REFUSED; a named binary is used AS NAMED -- never searched for, never
rebuilt. Nothing in this module builds a compiler: no configure, no `cmake --build`, no nested
`dssharness build`. A build goes through DssHarness and nowhere else (the operator's rule,
2026-09-24), and a driver that picks or rebuilds the compiler it measures measures a compiler
nobody named.
  ⓘ WHAT IT REPLACED. Until 2026-09-26 a run that named no compiler SEARCHED `build/*/bin/dss` for
  the newest Release dsscp and REFRESHED the tree it found from inside the step -- a plain `cmake
  --build`, or a nested `dssharness build` of a developer-environment leg's tree -- or, finding
  none, configured and built `build/rel`; `SKIP_DSS_BUILD=1` reused a searched binary instead.
  Naming `{product}` in every step (2026-09-25) had left that default in place for any run without
  `--dss`; the round's independent audit found it, and the default, `SKIP_DSS_BUILD` and every
  function only they used are gone.

The named binary then reaches ONE gate: its build type is READ from its own tree (never inferred
from a directory name), printed beside the path, and a non-Release binary is refused unless
`DSS_ALLOW_NONRELEASE_COMPILER` says otherwise -- which then marks every report line. Then the
config tree is PINNED (`DSS_CONFIG_ROOT` names the checkout that CONTAINS `src/dss-config`), and the
pair is proved current: one `speedtest1_bench.py --preflight-dss` probe per distinct selected
target, where ONLY exit 1 accuses the compiler -- and a stale binary is refused with DssHarness's
rebuild instruction (`rebuild_command`), never a build of this driver's own.

The build-type decision itself is `read_build_type` in `profile-compile/profile-compile-support.py`
(its one owner, also used by `compile-bench`); this module only adds WHERE the binary's tree is (the
recompile pairs a config with it, and the rebuild instruction names it), the stamp of the code that
runs, and the multi-config note the PowerShell driver printed. The speedtest1 benchmark names its
compiler under the same rule, with the same words (`require_named`, which `named_compiler` and
`benchmark_speedtest1.select_dss` both call): since P68 round 13 nothing in this action searches
`build/` for a dsscp, and the search that did (`search_roots`, `find_candidates`, `select_compiler`,
`format_candidates`, and the constants `SEARCH_ROOTS` and `BINARY_NAMES`) was deleted with its last
caller.
"""
from __future__ import annotations

import collections
import datetime
import importlib.util
import os
import re
import sys

import sqlite_common as C

Candidate = collections.namedtuple("Candidate", ["path", "mtime", "type", "tree", "source", "detail",
                                                 "image"])
Compiler = collections.namedtuple("Compiler", ["path", "type", "source", "detail", "tree", "origin",
                                                "built", "build_type_note"])

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


def _stamp(t):
    return datetime.datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M:%S") if t else "<unknown>"


def built_stamp(cand):
    """When the candidate's CODE was built, naming the image file that says so."""
    return "%s (%s)" % (_stamp(cand.mtime), os.path.basename(cand.image or cand.path))


def require_named(named, by):
    """-> (path, channel) for the dsscp a run NAMES -- `named` (blank-stripped) and the channel `by` that named it:
    `--dss`, what every harness step of the sqlite action passes (the leg's own `{product}`), or DSS_BIN by hand.
    REFUSES when it names none, in ONE set of words for every mode that takes a compiler -- the driver's run and
    its recompile (`named_compiler`) and the speedtest1 benchmark (`benchmark_speedtest1.select_dss`): a run is
    given its compiler, and nothing in this action would find or build one in its place."""
    named = (named or "").strip()
    if not named:
        C.die("no dsscp was named, and this run compiles with a GIVEN one: pass --dss <path> -- every "
              "harness step of the sqlite action passes the leg's own, {product}, which its runner's "
              "requireBuild has just built -- or set DSS_BIN.\n      It is never searched for and never "
              "built here: a build goes through DssHarness (`dssharness build --legs <leg>`) and nowhere "
              "else, and a driver that picked or rebuilt the compiler it measures would measure a "
              "compiler nobody named.")
    return named, by


def named_compiler(cfg):
    """-> (path, channel): the dsscp the run's `sqlite_common.Config` names -- `dss_bin`, and the
    channel `by["dss_bin"]` says set it: `--dss`, what every harness step passes (the leg's own
    `{product}`), or DSS_BIN by hand. REFUSES when it names none (`require_named`): this run is given
    its compiler, and nothing here or in Step 5 would find or build one in its place. `run_all` asks this
    FIRST, so a run naming no compiler stops before Step 0 spends anything; `obtain` asks it again."""
    return require_named(cfg.dss_bin, cfg.by["dss_bin"])


def obtain(cfg, log=C.LOG):
    """-> Compiler: the dsscp the run's `sqlite_common.Config` names (`named_compiler`: REQUIRED),
    used AS NAMED -- never searched for, never rebuilt -- through the ONE gate. The environment is
    read once, by `Config`, never here."""
    named, by = named_compiler(cfg)
    if not os.path.isfile(named):
        C.die("%s='%s' does not name an existing file. It is the compiler this run measures, not a "
              "hint: nothing is ever searched for or built in its place." % (by, named))
    info = build_type(named)
    origin = "named by %s — NOT built by this run" % by
    log.info("using %s — %s" % (by, info.path))
    built = built_stamp(info)
    log.info("compiler  : %s  (built %s)" % (info.path, built))
    log.info("build type: %s" % info.type)
    log.info("  read from: %s" % info.source)
    if info.detail:
        log.info("  note     : %s" % info.detail)
    note = "  (compiler build type: %s)" % info.type
    if not is_release(info.type):
        if not cfg.allow_nonrelease:
            C.die("this run would be timed against a NON-RELEASE compiler. It REFUSES rather than "
                  "proceed quietly.\n      compiler   : %s\n      build type : %s\n      read from  : "
                  "%s\n      note       : %s\n      A Debug dsscp is -g, no -O and no NDEBUG: it "
                  "compiles the same program correctly and takes several times as long, and the "
                  "difference lands in whatever the run is being read for.\n      Either name a "
                  "RELEASE leg's dsscp -- the sqlite runner runs on the release legs, and its step "
                  "passes the leg's own {product}; by hand, `dssharness build --legs <a release leg>` "
                  "builds one -- or set DSS_ALLOW_NONRELEASE_COMPILER=1 to proceed with THIS binary — "
                  "the run then says so on every report line."
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


def assert_current(core, compiler, config_root, specs, rebuild_cmd, python=sys.executable, scratch=None):
    """One `--preflight-dss` probe per DISTINCT target spec (deduplicated, in order). rc 0 = proved;
    ONLY rc 1 accuses the compiler (stale-binary refusal); any other rc -- or a missing core -- is
    COULD NOT RUN, which never tells the operator to rebuild. -> the specs proved, ", "-joined.
    `scratch` names the directory the core writes its probe in (`--scratch`): the speedtest1 benchmark
    passes its own, inside its output tree; without it the probe goes to the system temp directory."""
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
                       "--preflight-target", spec] + (["--scratch", scratch] if scratch else []),
                      merge=True, env_=C.child_env(python=True))
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
              "correct compiler behaviour.\n      REBUILD IT: %s\n      ⚠ A NAMED BINARY IS NOT "
              "EXEMPT FROM THIS CHECK (--dss, which every harness step passes as {product},\n      or "
              "DSS_BIN by hand). Naming a compiler says which one to USE, never that it can be "
              "TRUSTED:\n      a binary that fails here is unusable, and the run stops with this "
              "message instead of using it."
              % (spec, text, compiler.path, compiler.built, compiler.origin,
                 os.path.join(config_root, "src", "dss-config"), spec, rebuild_cmd))
    return ", ".join(checked)


def rebuild_command(compiler, repo_root):
    """The rebuild instruction a STALE binary is refused with (`assert_current`): DssHarness's build of
    the leg, in the tree under test, then that leg's dsscp named again. A build goes through the tool
    and nowhere else (the operator's rule, 2026-09-24), so this never advises `cmake --build` or a
    configure -- until 2026-09-26 it did, for any tree DssHarness had not built. Which leg keys which
    build directory is the tool's to say (`dssharness legs` lists them), so the leg is left for the
    reader to name rather than guessed here from the tree's directory name or the tool's own marker."""
    where = compiler.tree or os.path.dirname(os.path.abspath(compiler.path))
    return ("dssharness build --legs <leg> -C %s -- <leg> being the one whose build made %s (dssharness "
            "legs lists them) -- then name that leg's dsscp with --dss, as every harness step does with "
            "{product}" % (repo_root, where))
