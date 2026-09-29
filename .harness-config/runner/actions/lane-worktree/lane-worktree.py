#!/usr/bin/env python3
# PURPOSE: create and remove lane worktrees inside the ignored .worktrees/, refusing any root that would exceed Windows MAX_PATH.
r"""lane-worktree.py -- CREATE a lane worktree inside the repository's ignored worktrees root, and
REMOVE one without losing its evidence, its uncommitted work or its commits.

★★★ THE OPERATOR RULING THIS OWNS (2026-08-26):
  "I want the worktrees implementation to be inside the project root, .worktrees
   directory (where 100% of it's internal content ignored by .gitignore). This
   way we stop contaminating builds outside repository bounds."
  ... and, the same day, as an absolute:
  "worktrees MUST be ignored by ALL host copies to run legs"
Before it, lanes took worktrees at short absolute roots outside the repository (C:/dssp40k,
C:/dssp40l, ...): full checkouts, each with its own build/, that nothing owned, no guard could
see, and only `git worktree list` recorded. Inside the root they are enumerable and removable.

★ ONE OWNER. Hand-rolling `git worktree add` in a lane is how the location rule erodes: a rule
that lives only in a document "has no teeth at the moment of the decision".
★ AND ONE PROGRAM (2026-09-21, lane mig; the operator: "I don't want .sh/.ps1 files inside
.harness-config\runner\actions"). This file replaces `lane-worktree.sh`, `lane-worktree.ps1` and
the bash self-test that drove both. Two implementations were two places to fix, and the gap was
paid for: ✔MEASURED 2026-09-07 (P63), three fixes whose rows read CLOSED -- the evidence gate
(P50), remove-then-verify (P46) and the seed-manifest reset (P57) -- had landed in the `.sh`
only, so the Windows entry point ran without any of them.

⚠⚠ THE PATH-BUDGET PREFLIGHT IS THE LOAD-BEARING HALF OF `add`, AND IT IS A REGRESSION GUARD FOR
A MEASURED DEFECT (P29): a worktree under the ~150-character session scratch directory could not
be BUILT on Windows, because the generated `.obj.d` dependency paths exceed MAX_PATH. The failure
MODE is the dangerous part: not a link error at the end, but a per-TU compile error ("opening
dependency file" in a test's CMakeFiles directory) in files the lane never touched, which reads
as somebody else's breakage and sends the lane into an unrelated subsystem.
  ✔MEASURED 2026-08-26 inside a live lane: the longest build-relative suffix was 163 characters
  under `build/dbg/`; `C:/dssp40k` fit with 87 spare, `<repo>/.worktrees/k` with 46.
  ⚠ Both twins kept that 163 (and a margin of 20) as literals after builds moved to
  `build/<processor>-<toolchain>-<config>/`: at the main checkout they admitted names of up to 27
  characters where 10 can build (report 05, finding 4). ⇒ EVERY TERM IS NOW READ FROM
  `.harness-config/config.json`, and nothing has a default:
      len(<repo>/<worktrees.root>/<name>) + len("/build/") + len(<longest variant>) + len("/")
        + worktrees.pathBudgetReserve + worktrees.pathBudgetMargin   <   worktrees.pathLimit
  <longest variant> is the longest `<processor>-<toolchain>-<config>` over EVERY entry of `legs`
  -- the variant rule the config's own `{buildDir}` comment states. DssHarness counts only the
  variants the machine it runs on builds; the maximum over every leg is never smaller, so this
  verb never admits a name the harness would refuse. The reserve (168: the longest file below
  `build/<variant>/`, 166, + 2 for its transient `.obj.d`) was ✔MEASURED 2026-09-21 in every
  Windows build directory; the file carries that measurement. A name must also match DssHarness's
  own grammar (`dssharness help worktrees`): lowercase letters and digits joined by single
  hyphens, at most worktrees.maxNameLength characters -- which is also what keeps `..`, a path,
  a hidden entry and a whitespace name away from the recursive delete `remove` performs.

★★★ THE TREE ACTED ON IS THE ONE THIS FILE LIVES IN, never the caller's cwd (see `repo_root`).
And every git question -- all twelve the twins asked, of which they kept only the root query
away from an exported GIT_DIR / GIT_WORK_TREE / GIT_INDEX_FILE (report 05, finding 3) -- goes
through `owning-tree.py`'s `run_git`, so a caller's git environment steers none of them.
✔MEASURED 2026-09-21 on the `.sh` twin, in a throwaway repository, with those three names
exported the way a git hook exports them: `remove` of a lane holding ` M tracked.txt` read the
OTHER repository's clean status, exited 0 "(VERIFIED absent)", deleted the edit, and left the
lane registered.

`remove` asks, in this order, and refuses before it deletes anything:
  1. is `<root>/<name>` a directory that IS itself, strictly inside the worktrees root -- not a
     file, not a link that git's own removal would follow elsewhere (exit 2);
  2. THE WORK GATE: does the worktree's own `git status` list uncommitted work, or does its HEAD
     hold commits no ref of the repository reaches -- or can either not be read (exit 8,
     `--discard-work` overrides; see `work_gate`);
  3. THE EVIDENCE GATE: do the configured evidence roots hold files (exit 7 unless
     `--preserve-to <dir>`, which copies and re-reads every file first, or `--discard-evidence`;
     see `preserve_evidence`);
  4. then REMOVE, VERIFY, SPEAK: `git worktree remove --force`, prune, the recursive delete, and
     the refusal (exit 6) when the directory -- or git's registration of it -- survives.

Exit codes: 0 OK - 2 not a repository / git refused / a path that cannot be resolved / the
            configuration is missing, unreadable or ill-typed / a lane path that is not a
            directory, or that resolves elsewhere than itself inside the worktrees root
            - 3 the path budget would be breached - 4 the worktrees root is not ignored by git
            - 5 usage, including a lane name outside the grammar - 6 the worktree is STILL ON
            DISK after worktree-remove, prune and a recursive delete, or git STILL REGISTERS it
            - 7 EVIDENCE WOULD BE LOST: an evidence root holds files and no decision was given,
              the evidence could not be counted, or a preserve could not be proved -- a
              destination inside the worktree, one already holding a same-named file with other
              bytes, a count that changed, a failed copy, or a file that does not re-read
              identical
            - 8 THE LANE'S WORK WOULD BE LOST: its own git status lists a tracked modification
              or an untracked file that is not ignored, or its HEAD holds a commit no ref of
              the repository reaches -- or either cannot be read -- and --discard-work was not
              given.

Usage:
    python3 .harness-config/runner/actions/lane-worktree/lane-worktree.py [--repo <dir> | --repo=<dir>]
            {add <name> [committish] |
             remove <name> [--discard-work] [--preserve-to <dir> | --discard-evidence] |
             list}
    python3 .harness-config/runner/actions/lane-worktree/lane-worktree.py --self-test
"""

import collections
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import traceback
sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

# ── OUTPUT ENCODING ──────────────────────────────────────────────────────────────────
# Under ctest both streams are PIPES, and on Windows that brings them up as cp1252: a path or a
# message carrying a non-ASCII character raises inside a print, which reads as a crash rather
# than as the refusal it was printing. Reconfigured at IMPORT, before anything can print (the
# property `guard_output_encoding_guard` ratchets for every primary program).
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

PROG = "lane-worktree"

# DssHarness's layout (`dssharness help layout`): the tree's configuration, relative to its root.
CONFIG_SEGMENTS = (".harness-config", "config.json")
# What `{buildDir}` names below a tree: `build/<processor>-<toolchain>-<config>` (DssHarness's
# own rule, stated in config.json). The budget reserves `/build/<longest variant>/` itself.
BUILD_DIR_NAME = "build"
# lane-fold's OWN configuration (the action's config file, not DssHarness's): the bookkeeping
# directories lane-fold keeps beside the lanes. This program reads ONE key there, `manifests`,
# because `add` WRITES that directory (an empty seed); lane-fold reads the same file, so one edit
# moves both programs and they can never look in different places. No default.
LANE_FOLD_CONFIG_SEGMENTS = (".harness-config", "runner", "actions", "lane-fold",
                             "lane-fold-config.json")
MANIFEST_FORMAT = 2

# DssHarness's worktree-name grammar (`dssharness help worktrees`), spelled once.
NAME_RULE = re.compile(r"\A[a-z0-9]+(?:-[a-z0-9]+)*\Z")
NAME_RULE_TEXT = "lowercase letters and digits joined by single hyphens"
_SHA = re.compile(r"\A(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")
# The `os` value a leg declares, for the one hint `add` prints (never for a decision).
_HOST_OS = {"win32": "windows", "linux": "linux", "darwin": "macos"}

_NO_DEFAULT = ("Every directory and number this verb uses is read from that file; a missing or "
               "ill-typed key is refused, never replaced by a default.")

USAGE = (
    "usage: lane-worktree.py [--repo <dir> | --repo=<dir>]",
    "                        {add <name> [committish] |",
    "                         remove <name> [--discard-work] [--preserve-to <dir> | --discard-evidence] |",
    "                         list}",
    "       lane-worktree.py --self-test",
    "",
    "The tree acted on defaults to the one THIS PROGRAM LIVES IN, never the caller's",
    "cwd. --repo <dir> names another tree deliberately.",
)
_ADD_USAGE = "usage: lane-worktree.py add <name> [committish]"
_REMOVE_USAGE = ("usage: lane-worktree.py remove <name> [--discard-work] "
                 "[--preserve-to <dir> | --discard-evidence]")


class Refused(Exception):
    """A refusal. `code` is the exit code; `lines` are printed on stderr, one prefixed line each.

    A class rather than `sys.exit`, so the self-test can drive `main()` IN-PROCESS and read the
    code back -- and `str()` of it is its whole text, which is what `root_arms` reads."""

    def __init__(self, code, *lines):
        Exception.__init__(self, "\n".join(lines))
        self.code = code
        self.lines = list(lines)


def say(*lines):
    for line in lines:
        print("%s: %s" % (PROG, line))


# ── the owner of "which tree", and of asking git ─────────────────────────────────────

_OT = None


def _ot():
    """`../owning-tree/owning-tree.py`, loaded by path as a SIBLING (a hyphen is not a module
    name), once. Missing, it is a refusal -- never a local respelling of either answer it owns."""
    global _OT
    if _OT is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                            "owning-tree", "owning-tree.py")
        if not os.path.isfile(path):
            raise Refused(2, "cannot find %s -- this program's tree, and every git question it "
                             "asks, are resolved there and nowhere else." % path)
        spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _OT = mod
    return _OT


def git(args, cwd=None):
    """`git <args>` through owning-tree's `run_git`: the caller's GIT_DIR / GIT_WORK_TREE /
    GIT_INDEX_FILE (and every other name `git rev-parse --local-env-vars` lists) never reach it.
    Output captured as text; the exit code is the caller's to judge."""
    ot = _ot()
    try:
        return ot.run_git(list(args), cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    except ot.Refusal as exc:
        raise Refused(2, str(exc))
    except OSError as exc:
        raise Refused(2, "cannot run git (%s)" % exc)


def _git_said(proc, limit=3):
    lines = [ln for ln in ((proc.stderr or "") + (proc.stdout or "")).splitlines() if ln.strip()]
    return ["  git: " + ln for ln in lines[:limit]]


def default_root():
    """The tree THIS FILE lives in, with git's agreement that it is its own top level.

    Module-level and argument-free on purpose: `owning-tree`'s `root_arms` calls it from other
    working directories, under a steering git environment, and by NAME inside a copy of this file
    nested in another checkout."""
    ot = _ot()
    try:
        return os.path.realpath(ot.resolve(__file__, reads_git=True))
    except ot.Refusal as exc:
        raise Refused(2, "not inside a git repository this verb can answer for -- cannot place a "
                         "lane worktree.",
                      "This program resolves the tree IT LIVES IN (%s), never the caller's cwd; "
                      "pass --repo <dir> to name a different tree deliberately." % __file__,
                      str(exc))


def repo_root(override=None):
    """The tree this verb acts on: the one this file lives in, or `--repo <dir>`'s.

    ★★★ THE QUESTION IS "WHICH TREE DOES MY OWN FILE BELONG TO?", NOT "WHERE AM I STANDING".
    This was a bare `git rev-parse --show-toplevel`, which answers for the CALLER'S SHELL, so
    the lane path, the evidence gate and the recursive delete were all rooted wherever somebody
    had cd'd. ✔MEASURED 2026-09-02, driving the verb out of `.worktrees/lw` from a throwaway
    repository: `list` reported the THROWAWAY repository's `.worktrees/`. ✔MEASURED in P52 for
    real: fixtures seeded in the WSL leg clone were answered about the driver clone, and five of
    nine assertions reported a gate that had not fired.
    ⛔ AND NOT "THE MAIN CHECKOUT" (`--git-common-dir`), which is REFUTED in the dangerous
    direction: ✔MEASURED 2026-09-02, from `.worktrees/lw` that answer resolves `remove io` to
    `<main>/.worktrees/io` -- A LIVE SIBLING LANE'S UNCOMMITTED WORK -- where this answer resolves
    `.worktrees/lw/.worktrees/io`, which does not exist. Nested worktrees are ordinary git; "only
    the main checkout owns the worktrees root" is a convention of whoever runs the verb.
    ⇒ `default_root()` walks up from this file (owning-tree's `resolve`, git's agreement asked
    unsteered). `--repo <dir>` is the explicit way to mean another tree: a DIRECTORY, whose owning
    git working tree is taken -- and refused when a DSS tree sits nested inside a checkout it is
    not the root of, where git would answer for the enclosing one.
    """
    if override is None:
        return default_root()
    ot = _ot()
    if not os.path.lexists(override):
        raise Refused(2, "--repo '%s': no such directory." % override)
    if not os.path.isdir(override):
        raise Refused(2, "--repo '%s' is not a directory -- name the tree to act on by a "
                         "directory inside it." % override)
    try:
        return os.path.realpath(ot.owning_root(override))
    except ot.Refusal as exc:
        raise Refused(2, "--repo '%s' is not inside a git working tree this verb can answer for:"
                      % override, str(exc))


# ── the configuration: every directory and number, no defaults ───────────────────────

Settings = collections.namedtuple(
    "Settings", "path root evidence_roots max_name reserve margin limit variant legs manifests")

_MISSING = object()


def _describe(value):
    if value is _MISSING:
        return "MISSING"
    return "%s %r" % (type(value).__name__, value)


def _whole(path, key, value, minimum):
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise Refused(2, "%s: %s is %s -- expected a whole number >= %d." % (
            path, key, _describe(value), minimum), _NO_DEFAULT)
    return value


def _segments(path, key, value):
    """A repository-relative directory as its segments; refused unless it is exactly one."""
    if not isinstance(value, str) or not value.strip() or value != value.strip():
        raise Refused(2, "%s: %s is %s -- expected a repository-relative directory such as "
                         "'.worktrees'." % (path, key, _describe(value)), _NO_DEFAULT)
    segs = tuple(value.replace("\\", "/").split("/"))
    if any(s in ("", ".", "..") or ":" in s for s in segs):
        raise Refused(2, "%s: %s is %r -- expected a directory INSIDE the repository, spelled with "
                         "no '.', '..', drive, leading, trailing or doubled separator."
                      % (path, key, value), _NO_DEFAULT)
    return segs


def bookkeeping_name(path, key, value):
    """A bookkeeping directory's name (lane-fold's `manifests` or `evidence`): ONE path segment
    beginning with '.', so it can never be a lane (a lane name is `NAME_RULE`) nor leave the
    worktrees root. -> the name; refused (exit 2) otherwise, naming the key. lane-fold applies
    this same rule to the file's other key."""
    if not isinstance(value, str) or len(value) < 2 or value[0] != "." or value == ".." \
            or value != value.strip() or any(c in value for c in "/\\:"):
        raise Refused(2, "%s: %s is %s -- expected ONE name beginning with '.', such as "
                         "'.manifests': it sits beside the lanes under worktrees.root, and a dot-led "
                         "name can never be taken for a lane." % (path, key, _describe(value)),
                      _NO_DEFAULT)
    return value


def load_settings(repo):
    """-> Settings, read from `<repo>/.harness-config/config.json` (owning-tree's JSONC reader,
    the one this tree has), and `manifests` from lane-fold's own
    `<repo>/.harness-config/runner/actions/lane-fold/lane-fold-config.json` -- the directory `add`
    writes a seed into is lane-fold's to name. Every key is required and typed; nothing falls back
    to a literal."""
    ot = _ot()
    path = os.path.join(repo, *CONFIG_SEGMENTS)
    try:
        cfg = ot.load_jsonc(path)
    except ot.Refusal as exc:
        raise Refused(2, "cannot read this tree's configuration: %s" % exc, _NO_DEFAULT)
    if not isinstance(cfg, dict):
        raise Refused(2, "%s is %s -- expected a JSON object." % (path, _describe(cfg)))
    wt = cfg.get("worktrees", _MISSING)
    if not isinstance(wt, dict):
        raise Refused(2, "%s: worktrees is %s -- expected an object declaring root, evidenceRoots, "
                         "maxNameLength, pathBudgetReserve, pathBudgetMargin and pathLimit."
                      % (path, _describe(wt)), _NO_DEFAULT)
    root = _segments(path, "worktrees.root", wt.get("root", _MISSING))
    raw_roots = wt.get("evidenceRoots", _MISSING)
    if not isinstance(raw_roots, list) or not raw_roots:
        raise Refused(2, "%s: worktrees.evidenceRoots is %s -- expected a NON-EMPTY list of "
                         "repository-relative directories: an empty list would gate no evidence "
                         "at all, the P50 and P66 defect." % (path, _describe(raw_roots)),
                      _NO_DEFAULT)
    roots = tuple(_segments(path, "worktrees.evidenceRoots[%d]" % i, v)
                  for i, v in enumerate(raw_roots))
    folded = [tuple(os.path.normcase(s) for s in r) for r in roots]
    for i, a in enumerate(folded):
        for j, b in enumerate(folded):
            if i != j and b[:len(a)] == a:
                raise Refused(2, "%s: worktrees.evidenceRoots lists '%s' and '%s', and the second is "
                                 "the first or inside it -- a file under both would be counted "
                                 "and copied twice." % (path, "/".join(roots[i]), "/".join(roots[j])),
                              _NO_DEFAULT)
    max_name = _whole(path, "worktrees.maxNameLength", wt.get("maxNameLength", _MISSING), 1)
    reserve = _whole(path, "worktrees.pathBudgetReserve", wt.get("pathBudgetReserve", _MISSING), 0)
    margin = _whole(path, "worktrees.pathBudgetMargin", wt.get("pathBudgetMargin", _MISSING), 0)
    limit = _whole(path, "worktrees.pathLimit", wt.get("pathLimit", _MISSING), 1)
    legs = cfg.get("legs", _MISSING)
    if not isinstance(legs, dict) or not legs:
        raise Refused(2, "%s: legs is %s -- expected an object naming at least one leg: the path "
                         "budget reserves room for the LONGEST build variant any leg builds."
                      % (path, _describe(legs)), _NO_DEFAULT)
    variants = []
    for leg_name, leg in legs.items():
        if not isinstance(leg, dict):
            raise Refused(2, "%s: legs.%s is %s -- expected an object." % (
                path, leg_name, _describe(leg)), _NO_DEFAULT)
        parts = []
        for key in ("processor", "toolchain", "config"):
            v = leg.get(key, _MISSING)
            if not isinstance(v, str) or not v.strip() or "/" in v or "\\" in v:
                raise Refused(2, "%s: legs.%s.%s is %s -- expected a non-empty name: it is one part "
                                 "of that leg's build directory." % (path, leg_name, key,
                                                                    _describe(v)), _NO_DEFAULT)
            parts.append(v)
        variants.append("-".join(parts))
    variant = max(variants, key=len)          # the first of the longest, in the file's order
    lf_path = os.path.join(repo, *LANE_FOLD_CONFIG_SEGMENTS)
    try:
        lf = ot.load_jsonc(lf_path)
    except ot.Refusal as exc:
        raise Refused(2, "cannot read lane-fold's configuration: %s" % exc, _NO_DEFAULT)
    if not isinstance(lf, dict):
        raise Refused(2, "%s is %s -- expected a JSON object." % (lf_path, _describe(lf)),
                      _NO_DEFAULT)
    manifests = bookkeeping_name(lf_path, "manifests", lf.get("manifests", _MISSING))
    return Settings(path, root, roots, max_name, reserve, margin, limit, variant, legs, manifests)


def _local_leg(s):
    """The first leg (in the file's order) declaring THIS host's `os` -- for a hint only."""
    host = _HOST_OS.get(sys.platform)
    for leg_name, leg in s.legs.items():
        if leg.get("os") == host:
            return leg_name
    return None


# ── names and the path budget ────────────────────────────────────────────────────────

def check_name_grammar(name):
    """DssHarness's grammar. It is also the ONLY thing between a name and the recursive delete
    `remove` performs: harmless while the only verb was `git worktree remove`, which declines an
    unknown path, and not harmless once an `rm -rf` stood behind it -- `remove ../..` must never
    resolve anywhere. Union of the twins (report 05 D.1/D.2): `/` and `\\` refused on every host
    (the `.sh` accepted `a\\b`), and whitespace too (the `.sh` accepted a blank name)."""
    if not NAME_RULE.match(name):
        raise Refused(5, "lane name %r is refused: a lane name is %s (DssHarness's own worktree-name "
                         "grammar), so it can never be '..', a path, a hidden entry, whitespace, or "
                         "a spelling two hosts read differently." % (name, NAME_RULE_TEXT))


def check_name_length(name, s):
    if len(name) > s.max_name:
        raise Refused(5, "lane name %r is %d characters; worktrees.maxNameLength is %d (%s)."
                      % (name, len(name), s.max_name, s.path),
                      "The limit keeps a lane's build tree under the path budget; use a shorter "
                      "name.")


def path_budget(repo, s, name):
    """-> (lane path, its total against the budget, the build part, the longest name that fits)."""
    lane = os.path.join(repo, *(s.root + (name,)))
    build = len(os.sep + BUILD_DIR_NAME + os.sep) + len(s.variant) + len(os.sep)
    total = len(lane) + build + s.reserve + s.margin
    fits = s.limit - 1 - (len(lane) - len(name) + build + s.reserve + s.margin)
    return lane, total, build, fits


def assert_path_budget(repo, s, name):
    """Exit 3 unless the lane's deepest build path stays UNDER `worktrees.pathLimit` (the NUL
    counts, so equal is over). The refusal names every term and how long a name still fits."""
    lane, total, build, fits = path_budget(repo, s, name)
    if total >= s.limit:
        raise Refused(
            3,
            "REFUSING: the lane root '%s' is %d chars; + '/%s/' %d + the longest build variant "
            "'%s' %d + '/' 1" % (lane, len(lane), BUILD_DIR_NAME, len(BUILD_DIR_NAME) + 2,
                                 s.variant, len(s.variant)),
            "+ worktrees.pathBudgetReserve %d + worktrees.pathBudgetMargin %d = %d, which is not "
            "under worktrees.pathLimit %d (%s)." % (s.reserve, s.margin, total, s.limit, s.path),
            "A worktree this deep cannot be BUILT on Windows: generated paths exceed the limit.",
            "It would NOT fail as a link error -- it fails as a per-TU compile error in files",
            "you never touched, and reads as somebody else's breakage.",
            ("A name of at most %d character(s) still fits at this root; use a shorter lane name."
             % fits) if fits >= 1 else
            ("NO name fits at this root: '%s' alone spends the budget -- create lanes from a "
             "shallower checkout." % os.path.join(repo, *s.root)))
    say("path budget OK: root=%d + /%s/%s/=%d + reserve=%d + margin=%d = %d < pathLimit %d "
        "(the name could grow by %d)" % (len(lane), BUILD_DIR_NAME, s.variant, build, s.reserve,
                                         s.margin, total, s.limit, s.limit - 1 - total))
    return lane


def assert_ignored(repo, s):
    """The worktrees root must be IGNORED, and it is checked rather than assumed: it is the one
    rule that keeps N full checkouts off every gate host (the carriages derive what they carry
    from git, and the harness names the root in `sync.neverTransfer`).
    ⚠ The trailing slash is required -- `git check-ignore .worktrees` answers NOT-IGNORED for a
    directory that does not exist yet, while `.worktrees/` answers correctly (✔MEASURED
    2026-08-26, both spellings, absent directory). Exit 1 is "not ignored" (4); any other code is
    git failing to answer (2), never read as either verdict."""
    root_rel = "/".join(s.root)
    p = git(["-C", repo, "check-ignore", "-q", "--", root_rel + "/"])
    if p.returncode == 0:
        return
    if p.returncode == 1:
        raise Refused(4, "%s/ is NOT ignored by git." % root_rel,
                      "A lane worktree there would be committed, and -- worse -- would ride the",
                      "carriage to every gate host, where the examples runner globs examples/<lang>/*",
                      "and would run somebody's uncommitted corpus as if it were the cycle's.",
                      "Restore the '/%s/' rule in .gitignore before creating any worktree." % root_rel)
    raise Refused(2, "git check-ignore could not answer for '%s/' in '%s' (exit %d)."
                  % (root_rel, repo, p.returncode), *_git_said(p))


# ── add ──────────────────────────────────────────────────────────────────────────────

def write_seed_manifest(repo, s, name, base):
    """Reset `<root>/<manifests>/seed-<name>.json` (`manifests` is lane-fold's key, read by
    `load_settings`) to `{"base":"<sha>","format":2,"paths":{}}`, those bytes exactly, no newline
    -- written to a temporary file and moved into place.

    ⚠⚠ A CORRECTNESS FIX, NOT TIDINESS. `lane-fold.py` adjudicates a fold as (the lane's
    `git status` set) MINUS (seeded paths whose md5 is UNCHANGED), reading this file, which is
    keyed by LANE NAME alone -- and lane names are two letters, reused constantly. ✔MEASURED
    2026-09-03 (P57): four worktrees created on a clean tree ALL silently inherited manifests
    written days earlier by lanes of the same name; one held 82 entries of which 37 disagreed
    with the main tree, and the fold refused a file byte-identical to HEAD as DRIFTED. The
    expensive direction is the other one: a stale entry that happens to equal the lane's file
    marks real work as untouched seed, and the fold SILENTLY DROPS it.
    ⚠⚠ AND IT RECORDS THE COMMIT THE LANE WAS CREATED AT (format 2): a fold measures every
    unseeded path against the blob at the LANE'S OWN base. ✔REPRODUCED 2026-09-15 (P66): with
    the main tree's HEAD read at fold time instead, two lanes created at one commit, the first
    fold committed, and the second fold exited 0 having OVERWRITTEN the first lane's edit and
    DELETED its other one. The base is written down HERE, where it is known exactly.
    ⓘ Atomic (report 05 D.18): the twins wrote in place, so a failed write could leave the old
    stale manifest -- the P57 defect -- behind a refusal."""
    folder = os.path.join(repo, *(s.root + (s.manifests,)))
    path = os.path.join(folder, "seed-%s.json" % name)
    tmp = "%s.tmp-%d" % (path, os.getpid())
    body = ('{"base":"%s","format":%d,"paths":{}}' % (base, MANIFEST_FORMAT)).encode("ascii")
    try:
        os.makedirs(folder, exist_ok=True)
        with open(tmp, "wb") as fh:
            fh.write(body)
        os.replace(tmp, path)
    except OSError as exc:
        with contextlib.suppress(OSError):
            os.remove(tmp)
        raise Refused(2, "could not reset the seed manifest for '%s': %s" % (name, exc))


def cmd_add(opts):
    repo = repo_root(opts.repo)
    s = load_settings(repo)
    check_name_length(opts.name, s)
    assert_ignored(repo, s)
    lane = assert_path_budget(repo, s, opts.name)
    rel = "/".join(s.root + (opts.name,))
    if os.path.lexists(lane):
        raise Refused(5, "'%s' already exists -- remove it first, or pick another name." % rel)
    p = git(["-C", repo, "worktree", "add", "--detach", rel, opts.committish])
    if p.returncode != 0:
        raise Refused(2, "git worktree add failed for '%s' at '%s'." % (rel, opts.committish),
                      *_git_said(p))
    p = git(["-C", lane, "rev-parse", "--verify", "HEAD"])
    base = (p.stdout or "").strip() if p.returncode == 0 else ""
    if not _SHA.match(base):
        raise Refused(2, "could not read the new worktree's HEAD, so '%s' has no base commit to "
                         "record." % opts.name, *_git_said(p))
    write_seed_manifest(repo, s, opts.name, base)
    p = git(["-C", lane, "rev-parse", "--short", "HEAD"])
    short = (p.stdout or "").strip() if p.returncode == 0 else base[:12]
    # ⓘ THE BUILD COMMAND IS PRINTED HERE, WHERE THE LANE IS KNOWN, because the orchestrator kept
    # writing briefs that omitted it (P63: a brief stated an invocation its author never ran).
    # DssHarness takes the build type from the LEG NAME and builds each leg in its own
    # `<tree>/build/<variant>/`, so nothing is inferred from a tree name. The leg named is the
    # first one config.json declares for this host's `os`.
    say("created %s at %s" % (rel, short),
        "seed manifest reset: base recorded, no seeded paths (this lane starts from the commit, "
        "not from uncommitted work)",
        "build inside %s, never into the main tree's build/: each leg builds in "
        "%s/%s/<processor>-<toolchain>-<config>/" % (rel, rel, BUILD_DIR_NAME),
        "build this tree:  cd %s && dssharness build --legs %s" % (lane, _local_leg(s) or "<leg>"),
        "     its legs:    dssharness legs")
    print(lane)
    return 0


# ── remove: the lane path, the work gate, the evidence gate, the verified preserve ────

def _is_link(path):
    """A symbolic link or (Windows) a directory junction: never followed, never counted."""
    if os.path.islink(path):
        return True
    isjunction = getattr(os.path, "isjunction", None)
    return bool(isjunction and isjunction(path))


def _kind(path):
    if os.path.islink(path):
        return "a link to a file" if os.path.exists(path) else "a link to nothing"
    return "a file" if os.path.isfile(path) else "not a directory"


def assert_is_lane_path(lane, container, afterwards):
    """`lane` must BE `<container>/<name>`: strictly inside the worktrees root by FILESYSTEM
    IDENTITY (owning-tree's `is_within`, not a case-folded prefix -- report 05 D.9), and no link.

    ⚠ A LINK IS THE ONE WAY A SINGLE-COMPONENT NAME COULD STILL LAND ELSEWHERE, and containment
    alone does not close it: a link to a SIBLING lane is strictly inside the root, and
    `git worktree remove` resolves the path it is given. ✔MEASURED 2026-09-21 on the `.sh` twin
    (whose containment check ran only before its own `rm -rf`, AFTER git's removal): `remove
    <link to a sibling>` exited 0 "(VERIFIED absent)" with the SIBLING's directory and
    registration gone and the link itself still on disk. So the lane's real path must also equal
    the root's real path joined with the name. Asked BEFORE anything is read through the path,
    and again right before the recursive delete."""
    ot = _ot()
    try:
        real = os.path.realpath(lane)
        container_real = os.path.realpath(container)
        inside = ot.is_within(lane, container, strict=True)
    except (OSError, ValueError) as exc:
        raise Refused(2, "refusing to delete '%s': could not resolve it or its container (%s). %s"
                      % (lane, exc, afterwards))
    if not inside:
        raise Refused(2, "refusing to delete '%s': it resolves to '%s', which is not" % (lane, real),
                      "strictly inside '%s'. %s" % (container_real, afterwards))
    expected = os.path.join(container_real, os.path.basename(lane))
    if os.path.normcase(real) != os.path.normcase(expected):
        raise Refused(2, "refusing to delete '%s': it is a LINK resolving to '%s', not a lane "
                         "directory -- git's own worktree removal would follow it to whatever it "
                         "names. %s" % (lane, real, afterwards))


def _commit_lines(commits):
    return (["  " + c for c in commits[:10]]
            + (["  ... and %d more" % (len(commits) - 10)] if len(commits) > 10 else []))


def work_gate(repo, lane, rel, name, discard_work):
    """Refuse (8) a worktree whose OWN git status lists uncommitted work, or whose HEAD holds
    commits no ref of the repository reaches -- or when either cannot be read.

    ⚠⚠ `git worktree remove --force` DELETES a lane's work, and the verb passed `--force` without
    looking. ✔REPRODUCED 2026-09-15 (P66) on both twins: a lane whose status read ` M tracked.txt`
    and `?? new-work.txt`, removed with no flag, exited 0 "VERIFIED absent" -- both gone.
    ★ THE QUESTION IS ONLY "does this worktree's own status list a tracked modification, or an
    untracked file that is not ignored?" -- never "was it folded", which is lane-fold's
    measurement; a caller that KNOWS the work is folded says so with `--discard-work`
    (`lane-fold.py land` builds that flag only from its own "nothing left to fold").
    ⚠ `--show-prefix` asks "is this directory a worktree ROOT?" without comparing two spellings of
    one path: it prints nothing at a root, and `<root>/<name>/` when git walked up to the PARENT
    repository (the P46 shape, a lane whose `.git` was emptied) -- where `status` would answer
    with the MAIN tree's changes. So that is refused too.
    ⚠⚠ A COMMIT IS WORK TOO, AND STATUS NEVER LISTS IT. ✔REPRODUCED 2026-09-15 (P66, round 3):
    a file committed in a lane left its status empty, `remove` exited 0, and the worktree's HEAD
    -- the commit's only holder -- went with it; `git prune --expire=now` then deleted the commit.
    ★ `git rev-list <HEAD> --not --glob=refs/*`, asked AT THE REPOSITORY ROOT: `--glob=refs/*`
    and not `--branches --tags --remotes`, because a commit a shared ref such as `refs/keep/...`
    holds is not lost; at the root and not in the lane, because there the glob also counts the
    lane's OWN per-worktree refs (`refs/bisect/*`), which die with it.
    ⚠ A LIST GIT CANNOT PRODUCE IS REFUSED, NOT READ AS EMPTY: with one of a lane's commit objects
    missing, its status still exits 0 while the rev-list exits 128.
    ⓘ `--no-optional-locks`: these are questions; they must not take a worktree's index lock."""
    ro = ["--no-optional-locks"]
    readable, work, listable, commits = True, [], True, []
    p = git(ro + ["-C", lane, "rev-parse", "--show-prefix"])
    if p.returncode != 0 or (p.stdout or "").strip():
        readable = False
    if readable:
        p = git(ro + ["-C", lane, "status", "--porcelain", "--untracked-files=all"])
        if p.returncode != 0:
            readable = False
        else:
            work = [ln for ln in p.stdout.splitlines() if ln.strip()]
    if readable:
        p = git(ro + ["-C", lane, "rev-parse", "--verify", "-q", "HEAD"])
        head = (p.stdout or "").strip() if p.returncode == 0 else ""
        if head:
            p = git(ro + ["-C", repo, "rev-list", "--oneline", head, "--not", "--glob=refs/*"])
            if p.returncode != 0:
                listable = False
            else:
                commits = [ln for ln in p.stdout.splitlines() if ln.strip()]
    if not readable:
        if not discard_work:
            raise Refused(8, "cannot read the git status of '%s' at its own root, so it cannot be "
                             "shown to carry no" % rel,
                          "uncommitted work (git cannot open it as a working tree there, and asked "
                          "from inside it",
                          "git answers about a parent repository instead). REFUSING to delete it.",
                          "  pass --discard-work to remove it anyway, deliberately.")
        say("DISCARDING '%s', whose git status cannot be read at its own root, as instructed" % rel)
        return
    if not listable:
        if not discard_work:
            raise Refused(8, "cannot list the commits of '%s' that no ref of the repository reaches "
                             "(git rev-list" % rel,
                          "failed at the repository root), so it cannot be shown to hold none. "
                          "REFUSING to delete it.",
                          "  pass --discard-work to remove it anyway, deliberately.")
        say("DISCARDING '%s', whose commits cannot be listed, as instructed" % rel)
        return
    if (work or commits) and not discard_work:
        lines = []
        if work:
            lines += ["'%s' carries %d path(s) of UNCOMMITTED WORK -- tracked modifications, or "
                      "untracked" % (rel, len(work)),
                      "files that are not ignored, by its own git status -- and would be DELETED "
                      "with them.",
                      "first path(s):"] + work[:5]
        if commits:
            lines += ["'%s' holds %d COMMIT(S) that no branch, tag or other ref of the repository "
                      "reaches --" % (rel, len(commits)),
                      "only this worktree's HEAD holds them, and removing it would ORPHAN them:"
                      ] + _commit_lines(commits)
        lines += ["This verb cannot tell whether that work is already folded; that is lane-fold's "
                  "measurement:",
                  "  python3 .harness-config/runner/actions/lane-fold/lane-fold.py land %s "
                  "<production|harness> --apply" % name,
                  "    (folds uncommitted work, passes --discard-work only after measuring nothing "
                  "left to fold,",
                  "     and refuses a lane that committed)",
                  "  git branch <branch> <commit>   keeps a commit, so the removal no longer "
                  "orphans it",
                  "  --discard-work                 delete it deliberately"]
        raise Refused(8, *lines)
    if work:
        say("DISCARDING %d path(s) of uncommitted work under %s, as instructed" % (len(work), rel))
    if commits:
        say("DISCARDING %d commit(s) under %s that no ref of the repository reaches, as "
            "instructed:" % (len(commits), rel), *_commit_lines(commits))


def _walk(top):
    """`os.walk(top)` that RAISES on a listing error -- the one walker the evidence count and the
    preserve list share. Both twins read errors away (`2>/dev/null`, `SilentlyContinue`), which
    undercounts toward "no evidence" (report 05 D.16)."""
    def _raise(exc):
        raise exc
    return os.walk(top, onerror=_raise, followlinks=False)


def evidence_files(lane, roots):
    """-> [(lane-relative path, absolute path)] of every evidence file, in one stable order.

    ⓘ Every non-directory entry counts, a symbolic link to a file included and its CONTENT copied
    (⚠DECISION: the `.ps1` counted one, `find -type f` dropped it -- counting it is the direction
    that cannot lose evidence). A link to a DIRECTORY is neither followed nor counted: removing
    the lane removes the link, never what it names. Raises OSError."""
    out = []
    for segs in roots:
        top_rel = "/".join(segs)
        top = os.path.join(lane, *segs)
        if not os.path.lexists(top):
            continue
        if not os.path.isdir(top):
            out.append((top_rel, top))
            continue
        for dirpath, dirnames, filenames in _walk(top):
            dirnames[:] = sorted(d for d in dirnames if not _is_link(os.path.join(dirpath, d)))
            for f in sorted(filenames):
                path = os.path.join(dirpath, f)
                out.append((top_rel + "/" + os.path.relpath(path, top).replace(os.sep, "/"), path))
    return out


def count_evidence(lane, roots):
    """-> [(root, number of evidence files under it)]; exit 7 when the count cannot be taken."""
    try:
        files = evidence_files(lane, roots)
    except OSError as exc:
        raise Refused(7, "could not COUNT the evidence under '%s': %s" % (lane, exc),
                      "A count that skips what it cannot read is biased toward 'nothing to lose', "
                      "so none is taken. Nothing was copied or removed.")
    counts = []
    for segs in roots:
        top = "/".join(segs)
        counts.append((top, sum(1 for rel, _ in files if rel == top or rel.startswith(top + "/"))))
    return counts


def _digest(path):
    """(size, SHA-256) of the bytes a read of `path` returns -- through a link, its target's."""
    h, size = hashlib.sha256(), 0
    with open(path, "rb") as fh:
        while True:
            block = fh.read(1 << 20)
            if not block:
                break
            size += len(block)
            h.update(block)
    return size, h.hexdigest()


def _copy_evidence_file(src, dst):
    """One evidence file, to a temporary name then moved into place."""
    tmp = dst + ".lane-worktree-tmp"
    try:
        shutil.copyfile(src, tmp)
        os.replace(tmp, dst)
    except OSError:
        with contextlib.suppress(OSError):
            os.remove(tmp)
        raise


def preserve_evidence(lane, rel, destination, counted, roots):
    """Copy every evidence file to `<destination>/<root>/<same path>` and REFUSE (7) on anything
    that would lose evidence. It deletes nothing, ever.

    ★ ONE list, taken once: the clash check, the copy and the verify all read these same files in
    this same order, and a list that no longer matches the gate's count is refused (the tree
    changed underneath this verb).
    ⚠ A DESTINATION INSIDE THE TREE ABOUT TO BE DELETED IS A COPY INTO NOWHERE -- ✔REPRODUCED
    (P66): "preserved 1 ... (verified 1 present)", then the copy went with the worktree. Checked
    by filesystem identity before anything is created.
    ⚠ A FILE ALREADY AT THE DESTINATION UNDER THE SAME PATH, WITH OTHER BYTES, IS SOMEBODY ELSE'S
    EVIDENCE -- ✔REPRODUCED (P66): two lanes preserved into one directory, and the second silently
    overwrote the first lane's findings while the count still "verified". Identical bytes (a
    re-run) are not a clash.
    ⚠ VERIFY EVERY FILE, NEVER A COUNT: size and SHA-256 re-read at the destination (the `.ps1`
    form; the `.sh` used CRC), naming up to five files with the reason each failed."""
    ot = _ot()
    dest = os.path.abspath(destination)
    lane_real = os.path.realpath(lane)

    def inside_refusal(real):
        return Refused(7, "REFUSING: --preserve-to '%s' resolves to '%s', which is INSIDE '%s'."
                       % (destination, real, rel),
                       "This verb is about to delete that tree, so the copy would be destroyed with "
                       "the",
                       "evidence it was taken from -- after being reported as verified. Nothing was "
                       "removed.")

    if ot.is_within(dest, lane_real, strict=False):
        raise inside_refusal(os.path.realpath(dest))
    try:
        os.makedirs(dest, exist_ok=True)
    except OSError as exc:
        raise Refused(7, "could not create '%s' (%s); nothing was removed." % (destination, exc))
    dest_real = os.path.realpath(dest)
    if ot.is_within(dest_real, lane_real, strict=False):
        raise inside_refusal(dest_real)
    try:
        entries = [(r, src, _digest(src)) for r, src in evidence_files(lane, roots)]
    except OSError as exc:
        raise Refused(7, "could not re-read the evidence under '%s': %s" % (rel, exc),
                      "Nothing was copied or removed.")
    if len(entries) != counted:
        raise Refused(7, "counted %d evidence file(s) under '%s' but re-read %d: the tree changed"
                      % (counted, rel, len(entries)),
                      "underneath this verb. Nothing was copied or removed.")
    clash = []
    for r, _src, digest in entries:
        dst = os.path.join(dest_real, *r.split("/"))
        if os.path.lexists(dst):
            try:
                if not os.path.isfile(dst) or _digest(dst) != digest:
                    clash.append(r)
            except OSError as exc:
                clash.append("%s (unreadable: %s)" % (r, exc))
    if clash:
        raise Refused(7, "REFUSING: '%s' already holds a file at the same path with DIFFERENT bytes, "
                         "so" % dest_real,
                      "copying would silently OVERWRITE evidence that is already there. Nothing was "
                      "copied and",
                      "nothing was removed; preserve into a fresh directory. First clash(es):",
                      *clash[:5])
    try:
        for r, src, _digest_unused in entries:
            dst = os.path.join(dest_real, *r.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            _copy_evidence_file(src, dst)
    except OSError as exc:
        raise Refused(7, "copy of the evidence under '%s' -> '%s' FAILED; nothing was removed: %s"
                      % (rel, dest_real, exc))
    bad = []
    for r, _src, (size, sha) in entries:
        dst = os.path.join(dest_real, *r.split("/"))
        try:
            if not os.path.isfile(dst):
                bad.append("%s (absent)" % r)
                continue
            got_size, got_sha = _digest(dst)
            if got_size != size:
                bad.append("%s (size %d, expected %d)" % (r, got_size, size))
            elif got_sha != sha:
                bad.append("%s (content differs)" % r)
        except OSError as exc:
            bad.append("%s (unreadable: %s)" % (r, exc))
    if bad:
        raise Refused(7, "preserve VERIFY FAILED: the %d evidence file(s) under '%s' do not all "
                         "re-read" % (len(entries), rel),
                      "identical (size and SHA-256, file by file) under '%s':" % dest_real,
                      *(bad[:5] + ["REFUSING to remove '%s' -- the evidence would be lost." % rel]))
    say("preserved %d evidence file(s) -> %s (every file re-read at the destination: size and "
        "SHA-256 match)" % (len(entries), dest_real))


# ── remove: the removal itself, and its verification ─────────────────────────────────

def _delete_tree(path):
    """The ONE recursive delete this verb performs: owning-tree's `remove_tree`, which also
    clears the read-only files git writes (Windows refuses to unlink those)."""
    return _ot().remove_tree(path)


def _unlock_worktree(repo, lane):
    return git(["-C", repo, "worktree", "unlock", lane])


def _registration(repo, lane):
    """-> (registered, locked, lock reason) for `lane` in `git worktree list --porcelain`."""
    ot = _ot()
    p = git(["-C", repo, "worktree", "list", "--porcelain"])
    if p.returncode != 0:
        raise Refused(6, "cannot read git's worktree registrations in '%s' (exit %d), so the "
                         "removal of '%s' cannot be VERIFIED." % (repo, p.returncode, lane),
                      *_git_said(p))
    for block in p.stdout.replace("\r", "").split("\n\n"):
        path, locked, reason = None, False, ""
        for line in block.split("\n"):
            if line.startswith("worktree "):
                path = line[len("worktree "):]
            elif line == "locked" or line.startswith("locked "):
                locked, reason = True, line[len("locked"):].strip()
        if path and ot.same_path(path, lane):
            return True, locked, reason
    return False, False, ""


def settle_registration(repo, lane, rel):
    """git's record of the lane must be GONE, verified in `worktree list --porcelain`.

    ⚠ A LOCKED REGISTRATION SURVIVES BOTH PRUNES. ✔MEASURED 2026-09-21 (git 2.55): `worktree
    remove --force` declines a locked worktree ("cannot remove a locked working tree"), and
    `worktree prune` keeps a locked entry whose directory is gone -- while both twins deleted the
    directory and printed "(VERIFIED absent) and pruned stale registrations" over it. A lane is
    left locked by a killed `add` (git's own "initializing" lock; P66 found a probe "locked
    initializing"). Its directory is gone by now, so it is unlocked and pruned, then re-read."""
    registered, locked, reason = _registration(repo, lane)
    if registered and locked:
        say("'%s' was LOCKED (%s) and its directory is gone: unlocking and pruning its "
            "registration" % (rel, reason or "no reason given"))
        _unlock_worktree(repo, lane)
        git(["-C", repo, "worktree", "prune"])
        registered, locked, reason = _registration(repo, lane)
    if registered:
        raise Refused(6, "'%s' is gone from disk, but git STILL REGISTERS it (`worktree list "
                         "--porcelain` names it%s)" % (rel, ", locked" if locked else ""),
                      "after worktree-remove, prune, a recursive delete and prune again.",
                      "REFUSING to report success: the registration keeps the lane's name and HEAD "
                      "alive in the",
                      "repository's shared worktree records. `git worktree unlock` and "
                      "`git worktree prune` settle it by hand.")


def cmd_remove(opts):
    repo = repo_root(opts.repo)
    s = load_settings(repo)
    check_name_length(opts.name, s)
    container = os.path.join(repo, *s.root)
    rel = "/".join(s.root + (opts.name,))
    lane = os.path.join(container, opts.name)

    # ── 1. THE PATH IS A LANE: a directory that is itself, inside the root ──────────────
    # ⚠ The `.sh` could not cd into a FILE there and refused (2); the `.ps1` deleted it
    # (report 05 D.10). A lane is a directory; this verb deletes only what it can prove is one.
    if os.path.lexists(lane) and not os.path.isdir(lane):
        raise Refused(2, "'%s' exists but is %s -- a lane is a directory, and this verb deletes "
                         "only what it can prove is one. Nothing was read, copied or removed."
                      % (rel, _kind(lane)))
    if os.path.lexists(lane):
        assert_is_lane_path(lane, container, "Nothing was read, copied or removed.")

    # ── 2. THE WORK GATE, FIRST -- before any evidence is copied ────────────────────────
    if os.path.isdir(lane):
        work_gate(repo, lane, rel, opts.name, opts.discard_work)

    # ── 3. THE EVIDENCE GATE, BEFORE ANY DELETION ───────────────────────────────────────
    # ⚠⚠ A LANE'S EVIDENCE IS WHAT ITS ROW CITES, AND THIS VERB USED TO DELETE IT WITHOUT
    #    ASKING. ✔MEASURED 2026-09-01 (P50): a `cp -r <lane>/scratchpad/... && echo preserved`
    #    followed by `remove t2` on ONE command line; the copy failed, `&&` swallowed the echo,
    #    the absence of output read as success, and lane t2's 14 result JSONs and its md5 ledger
    #    were destroyed. ⇒ the tool owns the preserve: `--preserve-to` copies and re-reads,
    #    `--discard-evidence` is a decision rather than an accident.
    # ⚠⚠ AND THE GATE THAT CLOSED P50 LOOKED IN ONE DIRECTORY WHILE THE LANES HAD MOVED TO
    #    ANOTHER. ✔MEASURED 2026-09-15 (P66) on the five live lanes: every one kept its evidence
    #    under `.temp/<lane>-scratch/` (137 to 20,512 files each), none had a `scratchpad/`, and
    #    `remove` would have deleted 26,466 evidence files with no refusal.
    # ★ THE GATE IS DEFINED BY ROOTS, NOT BY A NAMING CONVENTION -- and the roots are
    #   `worktrees.evidenceRoots`, config.json's list, not a literal here. "Too much preserved"
    #   fails as a copy; "too little" fails as evidence nobody can get back.
    # ⓘ Counted as FILES, so an empty directory tree is correctly "nothing to preserve".
    if os.path.isdir(lane):
        counts = count_evidence(lane, s.evidence_roots)
    else:
        counts = [("/".join(r), 0) for r in s.evidence_roots]
    total = sum(n for _root, n in counts)
    where = " ".join("%s/=%d" % (r, n) for r, n in counts)
    if total and not opts.discard_evidence and opts.preserve_to is None:
        raise Refused(7, "'%s' holds %d evidence file(s) (%s) and would be DELETED with them."
                      % (rel, total, where),
                      "A lane's evidence is what its registry row cites -- findings logs, mutant",
                      "transcripts, gate logs, row cells. Choose explicitly:",
                      "  --preserve-to <dir>      copy ALL of it there FIRST; every file is re-read "
                      "and matched",
                      "  --discard-evidence       delete it deliberately",
                      "This gate exists because a hand-rolled 'cp && remove' lost lane t2's "
                      "evidence in P50,",
                      "and because the gate that closed that looked only at scratchpad/ while "
                      "every P66",
                      "lane kept its evidence under .temp/.")
    if total and opts.preserve_to is not None:
        preserve_evidence(lane, rel, opts.preserve_to, total, s.evidence_roots)
    elif total:
        say("DISCARDING %d evidence file(s) under %s (%s), as instructed" % (total, rel, where))

    # ── 4. REMOVE, THEN VERIFY, THEN SPEAK ──────────────────────────────────────────────
    # --force because a lane always carries an ignored build/ tree; without it git refuses and
    # the caller is tempted to delete by hand, which leaves the registration behind.
    # ⚠⚠ THE GIT VERB CAN DECLINE AND LEAVE THE WHOLE TREE ON DISK, AND THIS VERB USED TO REPORT
    # SUCCESS ANYWAY. ✔MEASURED 2026-08-31 (P46): lane `cm`'s `.git` had been emptied, `worktree
    # remove` could not see it, and "removed .worktrees/cm and pruned stale registrations" was
    # printed over 4.4 GB still there -- invisible, because the next `git worktree list` agreed.
    p = git(["-C", repo, "worktree", "remove", "--force", rel])
    if p.returncode != 0:
        say("worktree remove declined for '%s' (already gone?) -- pruning anyway" % rel)
    git(["-C", repo, "worktree", "prune"])
    if os.path.lexists(lane):
        assert_is_lane_path(lane, container, "Nothing was deleted.")
        _delete_tree(lane)
    if os.path.lexists(lane):
        raise Refused(6, "'%s' is STILL ON DISK after worktree-remove, prune and a recursive "
                         "delete." % rel,
                      "REFUSING to report success over work that did not happen.",
                      "A locked file is the likely cause -- a stalled ctest holding libdsscp.dll",
                      "is this repository's known instance. Close it and re-run.")
    git(["-C", repo, "worktree", "prune"])
    settle_registration(repo, lane, rel)
    # The container goes only when it is EMPTY (rmdir semantics): never a sibling lane's, and
    # never lane-fold's `.manifests`.
    try:
        os.rmdir(container)
        say("removed the now-empty %s/" % "/".join(s.root))
    except OSError:
        pass
    say("removed %s (VERIFIED absent) and pruned stale registrations" % rel)
    return 0


# ── list ─────────────────────────────────────────────────────────────────────────────

def _count_files(top):
    """-> (files under `top`, whether any part could not be read). For `list` only: informative."""
    n, errors = 0, []
    for dirpath, dirnames, filenames in os.walk(top, onerror=errors.append, followlinks=False):
        dirnames[:] = [d for d in dirnames if not _is_link(os.path.join(dirpath, d))]
        n += len(filenames)
    return n, bool(errors)


def cmd_list(opts):
    repo = repo_root(opts.repo)
    s = load_settings(repo)
    p = git(["-C", repo, "worktree", "list"])
    if p.returncode != 0:
        raise Refused(2, "git worktree list failed in '%s' (exit %d)." % (repo, p.returncode),
                      *_git_said(p))
    root_rel = "/".join(s.root)
    container = os.path.join(repo, *s.root)
    say("tree: %s" % repo, "registered worktrees:")
    for line in p.stdout.splitlines():
        if line.strip():
            print("  " + line)
    if os.path.isdir(container):
        say("under %s/:" % root_rel)
        for entry in sorted(os.listdir(container)):
            # ⚠ A LEADING DOT IS NEVER A LANE: `.manifests` (lane-fold's seed bookkeeping) and
            # `.evidence` (where `land` keeps a removed lane's evidence). ✔MEASURED 2026-09-07
            # (P63): the `.ps1` listed `.manifests` as a removable lane with 238 files in it.
            lane = os.path.join(container, entry)
            if entry.startswith(".") or not os.path.isdir(lane):
                continue
            files, unreadable = _count_files(lane)
            _lane, total, _build, _fits = path_budget(repo, s, entry)
            print("  %-50s %d files%s, %d spare under the path budget"
                  % (root_rel + "/" + entry, files, " (and unreadable parts)" if unreadable else "",
                     s.limit - 1 - total))
    else:
        say("under %s/: (absent -- no lane worktrees)" % root_rel)
    prefix = os.path.join(repo, *s.root) + os.sep
    _lane, total_max, build, fits = path_budget(repo, s, "x" * s.max_name)
    say("path budget at this root: '%s' %d + name + '/%s/%s/' %d + worktrees.pathBudgetReserve %d "
        "+ worktrees.pathBudgetMargin %d must stay under worktrees.pathLimit %d"
        % (prefix, len(prefix), BUILD_DIR_NAME, s.variant, build, s.reserve, s.margin, s.limit),
        "  a worktrees.maxNameLength name (%d characters) totals %d: %s"
        % (s.max_name, total_max, "fits" if total_max < s.limit else "does NOT fit"),
        "  the longest lane name this root admits: %s"
        % ("none -- no lane can be created at this root" if fits < 1 else
           "%d character(s) (the budget allows %d; worktrees.maxNameLength is %d)"
           % (min(fits, s.max_name), fits, s.max_name)))
    return 0


# ── the command line ─────────────────────────────────────────────────────────────────

Options = collections.namedtuple(
    "Options", "verb repo name committish discard_work discard_evidence preserve_to")


def parse_args(argv):
    """Strict: exact long options, every token accounted for, nothing given twice (exit 5).

    ⓘ Union of the twins (report 05 D.3/D.4): the `.sh` ignored extra words after `add` and
    `list`; the `.ps1` accepted abbreviations, ignored extras and exited 1 on a binding error; and
    BOTH read an empty `--repo` as "the tree this program lives in" -- an empty answer silently
    replaced by a default. `--repo` is taken from anywhere in the line, so each verb keeps its own
    grammar."""
    argv = list(argv)
    if "--self-test" in argv:
        raise Refused(5, "--self-test runs alone: lane-worktree.py --self-test")
    repo, rest, i = None, [], 0
    while i < len(argv):
        tok = argv[i]
        if tok == "--repo" or tok.startswith("--repo="):
            if tok == "--repo":
                if i + 1 >= len(argv):
                    raise Refused(5, "--repo needs a directory")
                value, i = argv[i + 1], i + 2
            else:
                value, i = tok[len("--repo="):], i + 1
            if repo is not None:
                raise Refused(5, "--repo given more than once ('%s', then '%s') -- name ONE tree."
                              % (repo, value))
            if not value.strip():
                raise Refused(5, "--repo needs a directory -- an empty one would silently mean the "
                                 "tree this program lives in.")
            if value.startswith("-"):
                raise Refused(5, "--repo needs a directory, and got the option-shaped %r -- a "
                                 "directory whose name starts with '-' is written ./%s."
                              % (value, value))
            repo = value
            continue
        rest.append(tok)
        i += 1
    if not rest:
        raise Refused(5, *USAGE)
    verb, args = rest[0], rest[1:]
    if verb == "add":
        positional = []
        for a in args:
            if a.startswith("-"):
                raise Refused(5, "unknown option %r -- add takes none. %s" % (a, _ADD_USAGE))
            positional.append(a)
        if not positional or not positional[0]:
            raise Refused(5, _ADD_USAGE)
        if len(positional) > 2:
            raise Refused(5, "unexpected argument %r. %s" % (positional[2], _ADD_USAGE))
        committish = positional[1] if len(positional) == 2 else "HEAD"
        if not committish.strip():
            raise Refused(5, "an empty committish names nothing. %s" % _ADD_USAGE)
        check_name_grammar(positional[0])
        return Options("add", repo, positional[0], committish, False, False, None)
    if verb == "remove":
        name, seen = None, set()
        discard_work = discard_evidence = False
        preserve_to = None
        j = 0
        while j < len(args):
            a = args[j]
            if a in ("--discard-work", "--discard-evidence", "--preserve-to"):
                if a in seen:
                    raise Refused(5, "%s given more than once." % a)
                seen.add(a)
                if a == "--preserve-to":
                    if j + 1 >= len(args) or not args[j + 1].strip():
                        raise Refused(5, "--preserve-to needs a directory")
                    if args[j + 1].startswith("-"):
                        raise Refused(5, "--preserve-to needs a directory, and got the "
                                         "option-shaped %r -- a directory whose name starts with "
                                         "'-' is written ./%s." % (args[j + 1], args[j + 1]))
                    preserve_to, j = args[j + 1], j + 2
                    continue
                if a == "--discard-work":
                    discard_work = True
                else:
                    discard_evidence = True
                j += 1
                continue
            if a == "--discard-scratchpad":
                # ⚠ RETIRED, AND REFUSED RATHER THAN KEPT AS AN ALIAS: it meant "delete the
                #   scratchpad" when that was the only root gated. Honouring it now would silently
                #   WIDEN a destructive flag to every evidence root.
                raise Refused(5, "--discard-scratchpad is retired: the gate now covers every "
                                 "evidence root (worktrees.evidenceRoots),",
                              "so a flag named for one of them would silently discard the others.",
                              "Say which you mean: --discard-evidence deletes ALL of it; "
                              "--preserve-to <dir> keeps it.")
            if a.startswith("-"):
                raise Refused(5, "unknown option %r (expected --discard-work, --preserve-to <dir> "
                                 "or --discard-evidence)" % a)
            if name is not None:
                raise Refused(5, "unexpected argument %r -- remove takes ONE lane name. %s"
                              % (a, _REMOVE_USAGE))
            name, j = a, j + 1
        if not name:
            raise Refused(5, _REMOVE_USAGE)
        if discard_evidence and preserve_to is not None:
            raise Refused(5, "--preserve-to and --discard-evidence contradict each other; pick one.")
        check_name_grammar(name)
        return Options("remove", repo, name, None, discard_work, discard_evidence, preserve_to)
    if verb == "list":
        if args:
            raise Refused(5, ("unknown option %r -- list takes none." if args[0].startswith("-")
                              else "unexpected argument %r -- list takes none.") % args[0])
        return Options("list", repo, None, None, False, False, None)
    if verb.startswith("-"):
        raise Refused(5, "unknown option %r." % verb, *USAGE)
    raise Refused(5, "unknown verb %r." % verb, *USAGE)


_VERBS = {"add": cmd_add, "remove": cmd_remove, "list": cmd_list}


def main(argv):
    """-> the exit code. RETURNS it, never `sys.exit`s, so the self-test drives it in-process."""
    argv = list(argv)
    if argv == ["--self-test"]:
        return self_test()
    try:
        opts = parse_args(argv)
        return _VERBS[opts.verb](opts)
    except Refused as exc:
        for line in exc.lines:
            print("%s: %s" % (PROG, line), file=sys.stderr)
        return exc.code
    except Exception as exc:  # noqa: BLE001 - a crash is exit 2 with its traceback, never exit 1
        traceback.print_exc(file=sys.stderr)
        print("%s: INTERNAL ERROR (%s: %s) -- stopped at the point above; exit 2."
              % (PROG, type(exc).__name__, exc), file=sys.stderr)
        return 2


# ═══ THE SELF-TEST ═══════════════════════════════════════════════════════════════════

# ⚠ Raising this is the claim that the assertions you added actually RUN. A host that cannot make
# a directory link records (X3)/(X3b) as NOT APPLICABLE -- named, and counted here all the same.
EXPECTED_ASSERTIONS = 160

_R = collections.namedtuple("_R", "rc out err")

# The fixture's legs. The LONGEST variant is deliberately NOT the first: a budget that took any
# leg but the longest moves the boundary the (B) arms pin.
_FIXTURE_LEGS = collections.OrderedDict([
    ("fx-arm64-release", collections.OrderedDict([
        ("os", "macos"), ("processor", "arm64"), ("toolchain", "clang"), ("config", "release")])),
    ("fx-x86_64-debug", collections.OrderedDict([
        ("os", "windows"), ("processor", "x86_64"), ("toolchain", "mingw-gcc"),
        ("config", "debug")])),
    ("fx-x86_64-release", collections.OrderedDict([
        ("os", "linux"), ("processor", "x86_64"), ("toolchain", "msvc"), ("config", "release")])),
])
# The ordinary arms' budget is generous, so it bites only where an arm makes it exact.
_FIXTURE_WORKTREES = collections.OrderedDict([
    ("root", ".worktrees"), ("evidenceRoots", ["scratchpad", ".temp"]), ("maxNameLength", 10),
    ("pathBudgetReserve", 168), ("pathBudgetMargin", 1), ("pathLimit", 100000)])
_FIXTURE_IGNORE = "/.worktrees/\n/lanes/\nbuild/\nscratchpad/\n.temp/\nkeep/\n__pycache__/\n"
# lane-fold's own file, as the fixture carries it (the real tree's names).
_FIXTURE_LANE_FOLD = collections.OrderedDict([("manifests", ".manifests"), ("evidence", ".evidence")])


class _FixtureError(Exception):
    """A fixture step (not an assertion) failed: the run stops, and the count says so."""


def _load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _read(path):
    with open(path, "rb") as fh:
        return fh.read()


def _said(r):
    return "rc=%r\n--- stdout ---\n%s\n--- stderr ---\n%s" % (r.rc, r.out[-2500:], r.err[-2500:])


def _make_dir_link(link, target):
    """-> (kind, why): a directory symlink, else (Windows) a junction, else (None, why not)."""
    try:
        os.symlink(target, link, target_is_directory=True)
        return "symlink", ""
    except (OSError, NotImplementedError, AttributeError) as exc:
        why = "os.symlink: %s" % exc
    if os.name == "nt":
        try:
            import _winapi
            _winapi.CreateJunction(target, link)
            return "junction", ""
        except (ImportError, AttributeError, OSError) as exc:
            why += "; _winapi.CreateJunction: %s" % exc
    return None, why


def _remove_link(link):
    try:
        os.unlink(link)
    except OSError:
        os.rmdir(link)


class _Fixture(object):
    """The self-test's throwaway repository, and the COPY of this program it drives in-process."""

    def __init__(self, ot, box, source_tree, program, owner):
        self.ot, self.box = ot, box
        self.root = os.path.join(box, "r")
        self.container = os.path.join(self.root, ".worktrees")
        self.manifests = os.path.join(self.container, _FIXTURE_LANE_FOLD["manifests"])
        hooks = os.path.join(box, "no-hooks")
        os.makedirs(self.root)
        os.makedirs(hooks)
        self.git_ok(["init", "-q", self.root])
        for key, value in (("user.email", "lane-worktree@example.invalid"),
                           ("user.name", "lane-worktree self-test"),
                           ("commit.gpgsign", "false"), ("core.autocrlf", "false"),
                           ("core.hooksPath", hooks)):
            self.git_ok(["-C", self.root, "config", key, value])
        self.write(".gitignore", _FIXTURE_IGNORE)
        os.makedirs(os.path.join(self.root, ".plans"))          # untracked: names the DSS tree
        self.write_config()
        self.write_lane_fold_config()
        self.program = None
        for src in (program, owner):
            rel = os.path.relpath(src, source_tree)
            dst = os.path.join(self.root, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
            if _read(src) != _read(dst):
                raise _FixtureError("the copy of %s is not byte-identical" % rel)
            if src == program:
                self.program = dst
        self.write("tracked.txt", "base\n")
        self.git_ok(["-C", self.root, "add", "-A"])
        self.git_ok(["-C", self.root, "commit", "-q", "--no-verify", "-m",
                     "lane-worktree self-test fixture"])
        self.head = self.git_ok(["-C", self.root, "rev-parse", "--verify", "HEAD"]).strip()
        self.subject = _load_module(self.program, "lane_worktree_subject")

    # ── plumbing ──
    def git(self, args):
        return self.ot.run_git(list(args), capture_output=True, text=True, encoding="utf-8",
                               errors="replace")

    def git_ok(self, args):
        p = self.git(args)
        if p.returncode != 0:
            raise _FixtureError("git %s exited %d: %s" % (" ".join(args), p.returncode,
                                                          (p.stderr or "").strip()[:300]))
        return p.stdout

    def path(self, rel):
        return rel if os.path.isabs(rel) else os.path.join(self.root, *rel.split("/"))

    def write(self, rel, text):
        path = self.path(rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
        return path

    def text(self, rel):
        try:
            with open(self.path(rel), "r", encoding="utf-8", newline="") as fh:
                return fh.read()
        except OSError:
            return None

    def config_text(self, worktrees=None, legs=None, drop=()):
        wt = collections.OrderedDict(_FIXTURE_WORKTREES)
        wt.update(worktrees or {})
        cfg = collections.OrderedDict([("worktrees", wt), ("legs", legs or _FIXTURE_LEGS)])
        for key in drop:
            if key in wt:
                del wt[key]
            cfg.pop(key, None)
        body = json.dumps(cfg, indent=2).rstrip()
        # JSONC, like the real file: a comment and a trailing comma the strict reader would refuse.
        return ("// the lane-worktree self-test fixture's own configuration\n"
                + body[:-1].rstrip() + ",\n}\n")

    def write_config(self, **kw):
        return self.write(".harness-config/config.json", self.config_text(**kw))

    def write_lane_fold_config(self, tree=None, **over):
        """lane-fold's own file in the fixture (or in `tree`): `over` replaces a key, and a value of
        None DROPS it."""
        cfg = collections.OrderedDict(_FIXTURE_LANE_FOLD)
        for key, value in over.items():
            if value is None:
                cfg.pop(key, None)
            else:
                cfg[key] = value
        return self.write(os.path.join(tree or self.root, *LANE_FOLD_CONFIG_SEGMENTS),
                          json.dumps(cfg, indent=2) + "\n")

    def lane(self, name):
        return os.path.join(self.container, name)

    def manifest(self, name, container=None, manifests=None):
        return os.path.join(container or self.container, manifests or _FIXTURE_LANE_FOLD["manifests"],
                            "seed-%s.json" % name)

    # ── the subject, in-process ──
    def run(self, *argv, **kw):
        cwd = kw.get("cwd")
        out, err = io.StringIO(), io.StringIO()
        held = os.getcwd()
        try:
            if cwd:
                os.chdir(cwd)
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                try:
                    rc = self.subject.main(list(argv))
                except KeyboardInterrupt:
                    raise
                except BaseException as exc:  # noqa: BLE001 - a subject must RETURN its code
                    rc = "RAISED %s: %s" % (type(exc).__name__, exc)
        finally:
            os.chdir(held)
        return _R(rc, out.getvalue(), err.getvalue())

    def add(self, name):
        r = self.run("add", name)
        if r.rc != 0 or not os.path.isdir(self.lane(name)):
            raise _FixtureError("add %s did not create the lane:\n%s" % (name, _said(r)))
        return self.lane(name)

    def cleanup(self, name, *flags):
        r = self.run("remove", name, *flags)
        if r.rc != 0 or os.path.lexists(self.lane(name)):
            raise _FixtureError("cleanup remove %s %s failed:\n%s" % (name, " ".join(flags),
                                                                      _said(r)))

    def commit_in(self, name, fname, subject):
        self.write(os.path.join(self.lane(name), fname), subject + "\n")
        self.git_ok(["-C", self.lane(name), "add", "--", fname])
        self.git_ok(["-C", self.lane(name), "commit", "-q", "--no-verify", "-m", subject])

    def head_of(self, path):
        p = self.git(["-C", path, "rev-parse", "--verify", "-q", "HEAD"])
        return p.stdout.strip() if p.returncode == 0 else None

    def drop_loose_object(self, name, rev):
        """Delete the LOOSE object of `rev` in the fixture's store: `rev-list` over it then fails
        while the lane's `status` still reads (✔MEASURED 2026-09-15: status 0, rev-list 128)."""
        p = self.git(["-C", self.lane(name), "rev-parse", "--verify", "-q", rev])
        sha = p.stdout.strip() if p.returncode == 0 else ""
        if not _SHA.match(sha):
            return False
        obj = os.path.join(self.root, ".git", "objects", sha[:2], sha[2:])
        if not os.path.isfile(obj):
            return False
        os.chmod(obj, 0o600)
        os.remove(obj)
        return not os.path.exists(obj)

    def registered(self, path, repo=None):
        """-> (registered, locked) for `path` in `repo`'s `worktree list --porcelain`."""
        p = self.git(["-C", repo or self.root, "worktree", "list", "--porcelain"])
        found = locked = False
        for block in p.stdout.replace("\r", "").split("\n\n"):
            lines = block.split("\n")
            wt = [ln[len("worktree "):] for ln in lines if ln.startswith("worktree ")]
            if wt and self.ot.same_path(wt[0], path):
                found = True
                locked = any(ln == "locked" or ln.startswith("locked ") for ln in lines)
        return found, locked

    def files_under(self, path):
        return sorted(os.path.relpath(os.path.join(d, f), path).replace(os.sep, "/")
                      for d, _ds, fs in os.walk(path) for f in fs) if os.path.isdir(path) else []


def self_test():
    """Red-on-disable for the verb, on a REAL throwaway repository, driven IN-PROCESS.

    ★★ WHY THIS EXISTS RATHER THAN A NOTE IN A ROW. ✔MEASURED 2026-09-01 (P50): a hand-rolled
    `cp ... && echo preserved` followed by `remove t2` destroyed lane t2's only evidence; the
    preserve step was a convention living in one head. The gate moved the rule into the tool, and
    these arms keep it there.
    ⚠⚠ THE TWIN-PARITY HALF IS GONE WITH THE TWINS. ✔MEASURED 2026-09-07 (P63): the rows read
    CLOSED while the `.ps1` -- the entry point the Windows host calls -- ran with no evidence gate
    at all, and a test that drove only the `.sh` could not see it; so the old test ran every arm
    twice (133 assertions, 46 pwsh starts, ✔MEASURED 115-244 s per debug run). One program now,
    so every arm runs once, against the one implementation, IN-PROCESS: the fixture's COPY is
    imported and its `main(argv)` called with its output captured -- 0 bash and 0 pwsh starts.
    ⚠⚠ EVERY PROBE LIVES IN A FIXTURE REPOSITORY. ✔MEASURED 2026-09-15 (P66): the main tree held
    110 probe manifests and a registered probe "locked initializing" from runs that made their
    lanes in the repository they lived in; a killed run's trap never ran. ⇒ the fixture is built
    in owning-tree's fenced `sandbox()` (outside every tree and every repository), with git's
    global and system configuration isolated and hooks disabled; it holds byte-identical copies of
    this program and of owning-tree at their real tree-relative paths, an untracked `.plans/` so
    the copy resolves the FIXTURE as its tree, and its OWN `.harness-config/config.json` (JSONC)
    under test control. (I1) requires the first probe to be there and nothing of it in the tree
    this file lives in; (I1c) sweeps every probe at the end.
    ⓘ Probe names stay short (`f<pid mod 1000><letter>`, ✔MEASURED P60: 14-character probes were
    refused by the budget from a lane), and the budget arms make it EXACT at the fixture's own
    path, so they mean the same on every host and at every temp-directory depth.
    ★ INTEGRATION ARMS ON PURPOSE: the defects were in what the verb DOES to a real tree, and an
    exit code alone cannot tell "refused for the evidence" from "refused for the work" -- so each
    arm asserts the message AND the exit code (★ (6) never checked its code), and every arm where
    the verb claims a removal checks INDEPENDENTLY that the directory is gone (★ the P46 lesson:
    (2), (4), (5), (20), (23), (33) trusted the verb's own "VERIFIED absent").
    ⚠ THE CONTROLS ARE NOT DECORATION: (5), (23), (33), (38), (B2), (N12) and (CF2) are what keep
    the refusals from passing over a verb that refuses everything.
    Retired with the twins (report 05 §E): (0a) (0b) the interpreter probe, (I2), (11)-(17b), the
    `.ps1` half of (18), (25)-(30), (41)-(50) -- each mirrored a ported arm 1:1 -- and the 27 N/A
    lines. ★NEW: (R1)-(R5) root arms, (S1)-(S3) steering, (P1)-(P18) strict parsing, (N1)-(N14)
    names, (G4) the ignore rule, (B1)(B2) the exact budget, (A5) an existing name, (CF1)-(CF8)
    the configuration, (D3)(D5) what --repo may name, (F1)-(F3)(E1r) faults injected into the
    copy, (K1)(K2) locked lanes, (X3)(X3b)(ND) what the lane path is.
    ✔ RED-ON-DISABLE, MEASURED 2026-09-21 (Windows, a scratch copy in a scratch tree per mutant,
    an unmutated control green in the same layout): the work-gate refusal off -> (31)(32)(32b)(34)
    (36)(37)(40)(S2); the evidence-gate refusal off -> (1)(2)(3)(18)(19)(20)(CF2); the per-file
    verify off -> (F1); the link identity check off -> (X3b), where `git worktree remove` given
    the link REMOVED THE SIBLING LANE, directory and registration; the name grammar off ->
    (N1)-(N10)(N13)(N14); the first leg's variant instead of the longest -> (B1)(B2); the verb's
    git not through `run_git` -> (S1)(S2)(S3) (a first (S2) passed VACUOUSLY there, over a lane
    the steered add never made -- hence its own lane); the registration check off -> (K1)(K2).
    ✔ AND BOTH DEFECTS THE LAST TWO PIN WERE LIVE IN THE `.sh` TWIN (MEASURED the same day in a
    throwaway repository): under a hook-style GIT_DIR/GIT_WORK_TREE/GIT_INDEX_FILE, `remove` of a
    lane holding ` M tracked.txt` exited 0 "(VERIFIED absent)" -- the edit deleted, the
    registration left behind; and `remove <link to a sibling lane>` exited 0 "(VERIFIED absent)"
    with the SIBLING's directory and registration gone and the link still on disk.
    """
    ot = _ot()
    here = os.path.realpath(__file__)
    owner = os.path.join(os.path.dirname(os.path.dirname(here)), "owning-tree", "owning-tree.py")
    counts = {"ok": 0, "fail": 0, "na": 0}

    def check(label, ok, detail=""):
        ok = bool(ok)
        counts["ok" if ok else "fail"] += 1
        print("  %-4s %s" % ("ok" if ok else "FAIL", label))
        if not ok and detail:
            for line in str(detail).splitlines()[:60]:
                print("       " + line)
        return ok

    def na(label, reason):
        counts["na"] += 1
        print("  n/a  %s\n       NOT APPLICABLE ON THIS HOST: %s" % (label, reason))

    def summary(crash=None):
        total = sum(counts.values())
        print("lane-worktree self-test: %d assertion(s) ran of %d expected: %d ok, %d failed, "
              "%d not applicable on this host" % (total, EXPECTED_ASSERTIONS, counts["ok"],
                                                  counts["fail"], counts["na"]))
        if crash or counts["fail"] or total != EXPECTED_ASSERTIONS:
            why = [w for w in (crash,
                               "%d assertion(s) failed" % counts["fail"] if counts["fail"] else "",
                               ("%d assertion(s) ran where %d are expected -- an assertion that "
                                "silently stops running is a property that silently stops being "
                                "proven" % (total, EXPECTED_ASSERTIONS))
                               if total != EXPECTED_ASSERTIONS else "") if w]
            print("lane-worktree self-test: FAILED - %s." % "; ".join(why))
            return 1
        print("lane-worktree self-test: OK - %d assertion(s): the work gate, the evidence gate over "
              "every configured root, the verified preserve, the root resolver, the path budget, "
              "the name grammar, the seed-manifest reset, the registration check and the fixture "
              "isolation do what they say, and this gate is PROVEN able to fail%s."
              % (total, "; %d not applicable on this host (named above)" % counts["na"]
                 if counts["na"] else ""))
        return 0

    try:
        real_repo = ot.owning_tree(here)
        real_settings = load_settings(real_repo)
    except (ot.Refusal, Refused) as exc:
        print("lane-worktree self-test: CANNOT RUN -- the tree this file lives in cannot be read: "
              "%s" % exc)
        return summary("the fixture could not be built")
    # Read BEFORE git's configuration is isolated, so a reading of the REAL tree keeps the
    # caller's global configuration (a `safe.directory`, say) exactly as a normal run would.
    real_env = ot.git_environment()

    def leaked(probe):
        """Where `probe` reached the tree this file lives in: directory, manifest, registration."""
        found = []
        for segs, manifests in ((real_settings.root, real_settings.manifests),
                                (tuple(_FIXTURE_WORKTREES["root"].split("/")),
                                 _FIXTURE_LANE_FOLD["manifests"])):
            base = os.path.join(real_repo, *segs)
            for p in (os.path.join(base, probe), os.path.join(base, manifests,
                                                             "seed-%s.json" % probe)):
                if os.path.lexists(p) and p not in found:
                    found.append(p)
        p = subprocess.run(["git", "-C", real_repo, "worktree", "list", "--porcelain"],
                           env=real_env, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        if p.returncode != 0:
            found.append("(the registrations could not be read: %s)" % p.stderr.strip()[:200])
        for line in p.stdout.splitlines():
            if line.startswith("worktree ") and os.path.basename(
                    line[len("worktree "):].rstrip("/\\")) == probe:
                found.append("a registered worktree %s" % line[len("worktree "):])
        return found

    # ── (R1)-(R5) THE ROOT, FROM ANY CWD AND UNDER ANY GIT ENVIRONMENT ─────────────────────
    # Owned by owning-tree: the control, a cwd in ANOTHER repository, a cwd in NO repository, a
    # caller's GIT_DIR + GIT_WORK_TREE + GIT_INDEX_FILE, and this file copied into a tree nested in
    # another checkout (REFUSED). Run before git's configuration is isolated, as a caller runs.
    for n, (ok, why, detail) in enumerate(ot.root_arms(default_root, (Refused,), True, __file__)):
        check("(R%d) %s" % (n + 1, why), ok, detail)

    sfx = os.getpid() % 1000

    def probe(prefix, letter):
        return "%s%d%s" % (prefix, sfx, letter)

    L1, L2, L3, L4, L5, L6, L7, L8 = (probe("f", c) for c in "abcdeijk")
    W1, W2, W3, W4 = (probe("f", c) for c in "pqrs")
    C1, C2, C3, C4 = (probe("f", c) for c in "lxyz")
    S1, B1, F1, F2, F3, K1, K2, R1, E1, X1, XL, XS, ND, G4, E1R, S2 = (
        probe("g", c) for c in "abcdefghijklmnop")
    M1 = probe("h", "a")
    probes = [L1, L2, L3, L4, L5, L6, L7, L8, W1, W2, W3, W4, C1, C2, C3, C4, S1, S2, B1, B1 + "x",
              F1, F2, F3, K1, K2, R1, E1, X1, XL, XS, ND, G4, E1R, M1]

    crash = None
    box = None
    try:
        with ot.sandbox(prefix="lw-") as sb:
            box = sb.box
            with ot.caller_environment(sb.env):
                fx = _Fixture(ot, box, real_repo, here, owner)
                print("lane-worktree self-test: fixture repository %s" % fx.root)
                crash = _self_test_arms(ot, fx, check, na, leaked, locals_=dict(
                    L1=L1, L2=L2, L3=L3, L4=L4, L5=L5, L6=L6, L7=L7, L8=L8, W1=W1, W2=W2, W3=W3,
                    W4=W4, C1=C1, C2=C2, C3=C3, C4=C4, S1=S1, S2=S2, B1=B1, F1=F1, F2=F2, F3=F3,
                    K1=K1, K2=K2, R1=R1, E1=E1, X1=X1, XL=XL, XS=XS, ND=ND, G4=G4, E1R=E1R,
                    M1=M1))
    except Exception as exc:  # noqa: BLE001 - a fixture that crashes stops the arms, by name
        crash = "the fixture crashed: %s: %s" % (type(exc).__name__, exc)
        print("  FAIL %s" % crash)
        traceback.print_exc(file=sys.stdout)

    # ── (I1c) NOTHING OF ANY PROBE REACHED THE TREE THIS FILE LIVES IN; (X1) THE BOX IS GONE ──
    swept = dict((p, leaked(p)) for p in probes)
    check("(I1c) no probe of this run -- directory, seed manifest or registration -- is in the "
          "repository this test lives in", not any(swept.values()),
          "; ".join("%s: %s" % (p, v) for p, v in swept.items() if v))
    check("(X1) the fixture's temporary tree was removed", box is not None
          and not os.path.lexists(box), "box=%s" % box)
    return summary(crash)


def _self_test_arms(ot, fx, check, na, leaked, locals_):
    """The arms, in order. -> None, or why they stopped (a `_FixtureError`)."""
    L1, L2, L3, L4, L5, L6, L7, L8 = (locals_[k] for k in
                                      ("L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8"))
    W1, W2, W3, W4 = (locals_[k] for k in ("W1", "W2", "W3", "W4"))
    C1, C2, C3, C4 = (locals_[k] for k in ("C1", "C2", "C3", "C4"))
    S1, S2, B1, F1, F2, F3, K1, K2, R1, E1, X1, XL, XS, ND, G4, E1R, M1 = (
        locals_[k] for k in ("S1", "S2", "B1", "F1", "F2", "F3", "K1", "K2", "R1", "E1", "X1",
                             "XL", "XS", "ND", "G4", "E1R", "M1"))
    box = fx.box
    subject = fx.subject

    def txt(r):
        return r.out + r.err

    def gone(name, container=None):
        return not os.path.lexists(os.path.join(container or fx.container, name))

    def removed(r, name, container=None):
        return r.rc == 0 and "VERIFIED absent" in r.out and gone(name, container)

    try:
        # ══ (I1) (1)-(3) (10) (10b) (A5): THE FIRST PROBE, AND A PRESERVE THAT CANNOT LIE ═════
        fx.write(fx.manifest(L1), '{"src/stale.cpp":"deadbeef"}')
        r = fx.run("add", L1)
        check("(I1) the first probe lane is created IN THE FIXTURE (add exits 0, the directory is "
              "there)", r.rc == 0 and os.path.isdir(fx.lane(L1)), _said(r))
        check("(I1) ... and nothing of it -- directory, seed manifest, registration -- is in the "
              "repository this test lives in", not leaked(L1), "; ".join(leaked(L1)))
        before = fx.head_of(fx.lane(L1))
        r = fx.run("add", L1)
        check("(A5) add of a name that already EXISTS is refused (exit 5) and the lane is untouched",
              r.rc == 5 and "already exists" in r.err and fx.head_of(fx.lane(L1)) == before
              and fx.text(os.path.join(fx.lane(L1), "tracked.txt")) == "base\n", _said(r))
        fx.write(os.path.join(fx.lane(L1), "scratchpad", "p", "lane", "probe.log"), "evidence\n")
        r = fx.run("remove", L1)
        check("(1) a scratchpad file and NO flag is REFUSED", "holds 1 evidence file(s)" in r.err,
              _said(r))
        check("(1) ... with the evidence exit code, not a generic one", r.rc == 7, _said(r))
        check("(1) ... and the worktree is STILL ON DISK", os.path.isdir(fx.lane(L1)))
        kept = os.path.join(box, "kept")
        r = fx.run("remove", L1, "--preserve-to", kept)
        check("(2) --preserve-to reports a VERIFIED copy", "preserved 1 evidence file(s)" in r.out,
              _said(r))
        check("(2) ... and only then removes the worktree: exit 0, 'VERIFIED absent'",
              r.rc == 0 and "VERIFIED absent" in r.out, _said(r))
        check("(2) ... and the directory is really GONE (checked here, not taken from the verb)",
              gone(L1))
        check("(3) the evidence really is at the destination, byte-identical, under scratchpad/",
              fx.text(os.path.join(kept, "scratchpad", "p", "lane", "probe.log")) == "evidence\n",
              "found=%s" % fx.files_under(kept))
        m = fx.text(fx.manifest(L1)) or "<<absent>>"
        check("(10) add RESETS a stale seed manifest -- the planted entry is gone",
              "src/stale.cpp" not in m, m)
        check("(10) ... to manifest format 2 with NO seeded paths", '"format":2,"paths":{}' in m, m)
        check("(10b) ... and RECORDS the commit the lane was created at: the file is exactly "
              '{"base":"<sha>","format":2,"paths":{}}, no newline',
              m == '{"base":"%s","format":2,"paths":{}}' % fx.head, "got=%r head=%s" % (m, fx.head))

        # ══ (4)-(6) ════════════════════════════════════════════════════════════════════════
        fx.add(L2)
        fx.write(os.path.join(fx.lane(L2), "scratchpad", "a.txt"), "x\n")
        r = fx.run("remove", L2, "--discard-evidence")
        check("(4) --discard-evidence names what it discards", "DISCARDING 1 evidence file(s)" in r.out,
              _said(r))
        check("(4) ... exits 0 and the directory is really GONE", removed(r, L2), _said(r))
        fx.add(L3)
        os.makedirs(os.path.join(fx.lane(L3), "scratchpad", "empty"))
        r = fx.run("remove", L3)
        check("(5) CONTROL: an EMPTY scratchpad removes with NO flag", r.rc == 0
              and "VERIFIED absent" in r.out, _said(r))
        check("(5) ... and the directory is really GONE", gone(L3))
        fx.add(L4)
        x_dest = os.path.join(box, "x")
        r = fx.run("remove", L4, "--preserve-to", x_dest, "--discard-evidence")
        check("(6) --preserve-to with --discard-evidence is a usage refusal",
              "contradict each other" in r.err, _said(r))
        check("(6) ... with the usage exit code, the lane kept and no destination created",
              r.rc == 5 and os.path.isdir(fx.lane(L4)) and not os.path.lexists(x_dest), _said(r))
        fx.cleanup(L4, "--discard-evidence")

        # ══ (7)-(9): THE ROOT IS THE PROGRAM'S OWN TREE, NOT THE CALLER'S CWD ═══════════════
        # ⚠ The verb runs from a FOREIGN repository holding a DECOY lane, so a cwd-keyed resolver
        # succeeds while answering about the wrong tree; nothing is removed by (7)-(9).
        foreign = os.path.join(box, "foreign")
        fx.git_ok(["init", "-q", foreign])
        fx.git_ok(["-C", foreign, "-c", "user.email=s@example.invalid", "-c", "user.name=s",
                   "commit", "-q", "--allow-empty", "--no-verify", "-m", "base"])
        fx.write(os.path.join(foreign, ".gitignore"), "/.worktrees/\n")
        fx.write(os.path.join(foreign, ".harness-config", "config.json"), fx.config_text())
        fx.write_lane_fold_config(tree=foreign)
        fx.write(os.path.join(foreign, ".worktrees", "decoylane", "marker.txt"), "decoy\n")
        outside = not ot.is_within(foreign, fx.root, strict=False)
        fx.add(L5)
        r = fx.run("list", cwd=foreign)
        check("(7) driven from a FOREIGN repository's cwd, the verb still answers about the tree it "
              "LIVES in", outside and r.rc == 0 and L5 in r.out, "outside-fixture=%s\n%s"
              % (outside, _said(r)))
        check("(7) ... and does NOT report the foreign cwd's lane", outside
              and "decoylane" not in txt(r), _said(r))
        fx.cleanup(L5, "--discard-evidence")
        r = fx.run("--repo", foreign, "list", cwd=fx.root)
        check("(8) --repo names another tree deliberately, from a cwd that is NOT it",
              r.rc == 0 and "decoylane" in r.out, _said(r))
        r = fx.run("list", "--repo=" + foreign, cwd=fx.root)
        check("(8b) ... and the --repo=<dir> spelling, after the verb, names it too",
              r.rc == 0 and "decoylane" in r.out, _said(r))
        check("(9) CONTROL: the decoy lane the negative half looks for really exists",
              os.path.isfile(os.path.join(foreign, ".worktrees", "decoylane", "marker.txt")))

        # ══ (19)-(24): THE EVIDENCE ROOTS, AND A PRESERVE THAT CANNOT LIE ═════════════════
        def plant_temp_evidence(name):
            base = os.path.join(fx.lane(name), ".temp", name + "-scratch")
            fx.write(os.path.join(base, "findings.log"), "findings\n")
            fx.write(os.path.join(base, "mutants", "m1.log"), "mutant transcript\n")

        fx.add(L6)
        plant_temp_evidence(L6)
        r = fx.run("remove", L6)
        check("(19) evidence ONLY under .temp/ and NO flag is REFUSED",
              "holds 2 evidence file(s)" in r.err, _said(r))
        check("(19) ... naming .temp/ as where it is", ".temp/=2" in r.err, _said(r))
        check("(19) ... with the evidence exit code", r.rc == 7, _said(r))
        check("(19) ... and the worktree is STILL ON DISK", os.path.isdir(fx.lane(L6)))

        # (18) `list` NAMES LANES, NEVER THE BOOKKEEPING BESIDE THEM. ✔MEASURED 2026-09-07 (P63):
        # the `.ps1` listed `.worktrees/.manifests` as a removable lane. The controls come first.
        fx.write(os.path.join(fx.container, ".evidence", "old-lane", "kept.txt"), "kept\n")
        r = fx.run("list")
        check("(18) CONTROL: .worktrees/.manifests and .worktrees/.evidence really are on disk",
              os.path.isdir(fx.manifests) and os.path.isdir(os.path.join(fx.container, ".evidence")))
        check("(18) CONTROL: list names the live lane", r.rc == 0
              and ".worktrees/%s" % L6 in r.out, _said(r))
        check("(18) list does not present .manifests as a lane", ".worktrees/.manifests" not in r.out,
              _said(r))
        check("(18) ... nor lane-fold's .evidence", ".worktrees/.evidence" not in r.out, _said(r))

        kept20 = os.path.join(box, "kept20")
        r = fx.run("remove", L6, "--preserve-to", kept20)
        check("(20) --preserve-to preserves every .temp/ file", "preserved 2 evidence file(s)" in r.out,
              _said(r))
        check("(20) ... and only then removes the worktree: exit 0, 'VERIFIED absent'",
              r.rc == 0 and "VERIFIED absent" in r.out, _said(r))
        check("(20) ... and the directory is really GONE", gone(L6))
        check("(20) the .temp/ evidence really is at the destination, byte-identical, under .temp/",
              fx.text(os.path.join(kept20, ".temp", L6 + "-scratch", "findings.log")) == "findings\n"
              and fx.text(os.path.join(kept20, ".temp", L6 + "-scratch", "mutants", "m1.log"))
              == "mutant transcript\n", "found=%s" % fx.files_under(kept20))

        fx.add(L7)
        fx.write(os.path.join(fx.lane(L7), "scratchpad", "e.log"), "evidence\n")
        inside = os.path.join(fx.lane(L7), "kept")
        r = fx.run("remove", L7, "--preserve-to", inside)
        check("(21) a --preserve-to INSIDE the worktree being removed is REFUSED",
              "INSIDE '.worktrees/%s'" % L7 in r.err, _said(r))
        check("(21) ... with the evidence exit code", r.rc == 7, _said(r))
        check("(21) ... and the worktree and its evidence SURVIVE, with nothing created inside it",
              fx.text(os.path.join(fx.lane(L7), "scratchpad", "e.log")) == "evidence\n"
              and not os.path.lexists(inside))
        fx.cleanup(L7, "--discard-evidence")

        clash = os.path.join(box, "clash")
        fx.write(os.path.join(clash, "scratchpad", "e.log"), "somebody else\n")
        fx.add(L8)
        fx.write(os.path.join(fx.lane(L8), "scratchpad", "e.log"), "evidence of the lane\n")
        r = fx.run("remove", L8, "--preserve-to", clash)
        check("(22) a destination file at the same path with DIFFERENT bytes is REFUSED",
              "DIFFERENT bytes" in r.err, _said(r))
        check("(22) ... with the evidence exit code", r.rc == 7, _said(r))
        check("(22) ... the evidence already at the destination is UNTOUCHED, and the worktree "
              "survives", fx.text(os.path.join(clash, "scratchpad", "e.log")) == "somebody else\n"
              and os.path.isdir(fx.lane(L8)))
        fx.write(os.path.join(clash, "scratchpad", "e.log"), "evidence of the lane\n")
        r = fx.run("remove", L8, "--preserve-to", clash)
        check("(23) CONTROL: identical bytes already at the destination preserve and remove",
              r.rc == 0 and "VERIFIED absent" in r.out, _said(r))
        check("(23) ... and the directory is really GONE", gone(L8))
        r = fx.run("remove", L8, "--discard-scratchpad")
        check("(24) the retired --discard-scratchpad is a usage refusal naming --discard-evidence",
              "--discard-evidence" in r.err, _said(r))
        check("(24) ... with the usage exit code", r.rc == 5, _said(r))

        # ══ (31)-(35b): THE WORK GATE ═══════════════════════════════════════════════════
        fx.add(W1)
        fx.write(os.path.join(fx.lane(W1), "tracked.txt"), "the lane edited this\n")
        r = fx.run("remove", W1)
        check("(31) a tracked modification and NO flag is REFUSED as uncommitted work",
              "carries 1 path(s) of UNCOMMITTED WORK" in r.err, _said(r))
        check("(31) ... with the work exit code", r.rc == 8, _said(r))
        check("(31) ... and the worktree and its edit SURVIVE",
              fx.text(os.path.join(fx.lane(W1), "tracked.txt")) == "the lane edited this\n")
        fx.write(os.path.join(fx.lane(W1), "new-work.txt"), "the lane wrote this\n")
        plant_temp_evidence(W1)
        k32 = os.path.join(box, "k32")
        r = fx.run("remove", W1, "--preserve-to", k32)
        check("(32) tracked + untracked work with a --preserve-to is REFUSED",
              "carries 2 path(s) of UNCOMMITTED WORK" in r.err, _said(r))
        check("(32) ... with the work exit code", r.rc == 8, _said(r))
        check("(32) ... and the work gate refused BEFORE the preserve copied anything",
              not fx.files_under(k32) and os.path.isfile(os.path.join(fx.lane(W1), "new-work.txt")),
              "found=%s" % fx.files_under(k32))
        fx.add(W3)
        fx.write(os.path.join(fx.lane(W3), "only-new.txt"), "only new\n")
        r = fx.run("remove", W3)
        check("(32b) an untracked, not-ignored file ALONE is REFUSED as uncommitted work",
              "carries 1 path(s) of UNCOMMITTED WORK" in r.err, _said(r))
        check("(32b) ... with the work exit code", r.rc == 8, _said(r))
        fx.cleanup(W3, "--discard-work")
        fx.add(W2)
        fx.write(os.path.join(fx.lane(W2), "build", "x.o"), "o\n")
        r = fx.run("remove", W2)
        check("(33) CONTROL: a worktree holding only IGNORED files removes with NO flag",
              r.rc == 0 and "VERIFIED absent" in r.out, _said(r))
        check("(33) ... and the directory is really GONE", gone(W2))
        k34 = os.path.join(box, "k34")
        r = fx.run("remove", W1, "--discard-work", "--preserve-to", k34)
        check("(34) --discard-work names the work it discards",
              "DISCARDING 2 path(s) of uncommitted work" in r.out, _said(r))
        check("(34) ... the evidence is still preserved beside it",
              "preserved 2 evidence file(s)" in r.out, _said(r))
        check("(34) ... and only then is the worktree removed: exit 0, 'VERIFIED absent'",
              r.rc == 0 and "VERIFIED absent" in r.out, _said(r))
        check("(34) ... and the directory is really GONE", gone(W1))
        fx.write(os.path.join(fx.lane(W4), "stray.txt"), "not a checkout\n")
        r = fx.run("remove", W4)
        check("(35) a directory whose git status cannot be read at its own root is REFUSED",
              "cannot read the git status" in r.err, _said(r))
        check("(35) ... with the work exit code", r.rc == 8, _said(r))
        check("(35) ... and it SURVIVES", os.path.isfile(os.path.join(fx.lane(W4), "stray.txt")))
        r = fx.run("remove", W4, "--discard-work")
        check("(35b) ... and --discard-work removes it deliberately: exit 0, 'VERIFIED absent'",
              r.rc == 0 and "VERIFIED absent" in r.out, _said(r))
        check("(35b) ... and the directory is really GONE", gone(W4))

        # ══ (36)-(40): THE WORK GATE, COMMITS ═══════════════════════════════════════════
        fx.add(C1)
        fx.commit_in(C1, "first.txt", "lane commit one")
        fx.commit_in(C1, "second.txt", "lane commit two")
        c1_head = fx.head_of(fx.lane(C1))
        r = fx.run("remove", C1)
        check("(36) a lane whose HEAD holds commits no ref reaches, and NO flag, is REFUSED",
              "holds 2 COMMIT(S) that no branch, tag or other ref" in r.err, _said(r))
        check("(36) ... with the work exit code", r.rc == 8, _said(r))
        check("(36) ... naming the newer commit", "lane commit two" in r.err, _said(r))
        check("(36) ... and the older one", "lane commit one" in r.err, _said(r))
        check("(36) ... and the worktree SURVIVES, its HEAD still holding them",
              c1_head is not None and fx.head_of(fx.lane(C1)) == c1_head)
        r = fx.run("remove", C1, "--discard-work")
        check("(37) --discard-work names the commits it discards", "DISCARDING 2 commit(s)" in r.out,
              _said(r))
        check("(37) ... each by its subject", "lane commit one" in r.out, _said(r))
        check("(37) ... and only then is the worktree removed: exit 0, 'VERIFIED absent'",
              r.rc == 0 and "VERIFIED absent" in r.out, _said(r))
        check("(37) ... and the directory is really GONE", gone(C1))
        fx.add(C2)
        fx.commit_in(C2, "older.txt", "lane commit whose object goes missing")
        fx.commit_in(C2, "newer.txt", "lane commit on top of it")
        if fx.drop_loose_object(C2, "HEAD~1"):
            r = fx.run("remove", C2)
            check("(36b) a lane whose commits cannot be listed (a missing object) is REFUSED",
                  "cannot list the commits" in r.err, _said(r))
            check("(36b) ... with the work exit code", r.rc == 8, _said(r))
            r = fx.run("remove", C2, "--discard-work")
            check("(36b) ... and --discard-work removes it deliberately: exit 0, the directory "
                  "really GONE", removed(r, C2), _said(r))
        else:
            for label in ("(36b) a lane whose commits cannot be listed (a missing object) is "
                          "REFUSED", "(36b) ... with the work exit code",
                          "(36b) ... and --discard-work removes it deliberately"):
                check(label, False, "CANNOT CONSTRUCT: the lane's older commit was not a loose "
                                    "object in the fixture, so no object could go missing")
            fx.cleanup(C2, "--discard-work")
        fx.add(C3)
        r = fx.run("remove", C3)
        check("(38) CONTROL: a lane still at its base, with no commits, removes with NO flag, the "
              "directory really GONE", removed(r, C3), _said(r))
        check("(38) ... and names no commit", "COMMIT(S)" not in txt(r), _said(r))
        fx.add(C4)
        fx.commit_in(C4, "kept.txt", "lane commit a shared ref keeps")
        c4_head = fx.head_of(fx.lane(C4))
        fx.git_ok(["-C", fx.lane(C4), "update-ref", "refs/keep/" + C4, "HEAD"])
        r = fx.run("remove", C4)
        check("(39) a commit refs/keep/<lane> still holds is NOT refused -- removing the lane loses "
              "nothing (exit 0, the directory really GONE)", removed(r, C4), _said(r))
        p = fx.git(["-C", fx.root, "for-each-ref", "--contains", c4_head or "HEAD",
                    "--format=%(refname)"])
        check("(39) ... and after the removal that ref still reaches the commit",
              c4_head is not None and p.stdout.strip() == "refs/keep/" + C4, p.stdout)
        fx.add(C1)
        fx.commit_in(C1, "bisected.txt", "lane commit only its own bisect ref holds")
        fx.git_ok(["-C", fx.lane(C1), "update-ref", "refs/bisect/bad", "HEAD"])
        r = fx.run("remove", C1)
        check("(40) a commit only the lane's OWN refs/bisect/bad holds is REFUSED -- that ref dies "
              "with the worktree", "holds 1 COMMIT(S)" in r.err, _said(r))
        check("(40) ... with the work exit code", r.rc == 8, _said(r))
        fx.cleanup(C1, "--discard-work")

        # ══ ★ (S1)(S2) A STEERING GIT ENVIRONMENT STEERS NONE OF THE VERB'S OWN GIT CALLS ══
        # ✔ The gap both twins had (report 05, finding 3): only their ROOT query ignored an
        # exported GIT_DIR / GIT_WORK_TREE / GIT_INDEX_FILE -- what a git hook exports.
        with ot.steering() as steer:
            decoy = steer["GIT_WORK_TREE"]
            neg = ot.bare_git(["rev-parse", "--show-toplevel"], fx.root, steer)
            real = neg.returncode == 0 and ot.same_path(neg.stdout.strip(), decoy)
            with ot.caller_environment(steer):
                r = fx.run("add", S1)
            here_reg = fx.registered(fx.lane(S1))[0]
            decoy_reg = fx.registered(fx.lane(S1), repo=decoy)[0]
            check("(S1) under a caller's GIT_DIR + GIT_WORK_TREE + GIT_INDEX_FILE naming another "
                  "repository, add creates the lane HERE and registers it HERE, not there",
                  real and r.rc == 0 and os.path.isdir(fx.lane(S1)) and here_reg
                  and not decoy_reg and os.path.isfile(fx.manifest(S1)),
                  "negative-synthesized=%s registered-here=%s registered-in-decoy=%s\n%s"
                  % (real, here_reg, decoy_reg, _said(r)))
            fx.cleanup(S1)
            # ⚠ The remove half gets its OWN lane, made unsteered, so it cannot pass over a lane
            # the steered add never created (✔MEASURED: with the verb's git NOT through run_git,
            # the steered add was refused -- the decoy does not ignore the root -- and a remove of
            # nothing "succeeded"). The lane carries a tracked edit: a steered `status` reads the
            # DECOY, which is clean, so the defect's answer is "no work" and the edit is deleted.
            fx.add(S2)
            fx.write(os.path.join(fx.lane(S2), "tracked.txt"), "edited in a steered lane\n")
            neg2 = ot.bare_git(["status", "--porcelain"], fx.lane(S2), steer)
            real2 = "tracked.txt" not in neg2.stdout
            with ot.caller_environment(steer):
                r = fx.run("remove", S2)
            check("(S2) under the same environment, remove still reads the LANE's own status: its "
                  "uncommitted edit is REFUSED (exit 8) and survives, never read as the decoy's "
                  "clean one", real2 and r.rc == 8 and "carries 1 path(s) of UNCOMMITTED WORK"
                  in r.err and fx.text(os.path.join(fx.lane(S2), "tracked.txt"))
                  == "edited in a steered lane\n",
                  "negative-synthesized=%s (a steered status said %r)\n%s"
                  % (real2, neg2.stdout.strip()[:80], _said(r)))
            with ot.caller_environment(steer):
                r = fx.run("remove", S2, "--discard-work")
            check("(S3) ... and --discard-work, under the same environment, removes it HERE: exit 0, "
                  "the directory gone and THIS repository's registration gone", removed(r, S2)
                  and not fx.registered(fx.lane(S2))[0], _said(r))
        fx.git_ok(["-C", fx.root, "worktree", "prune"])

        # ══ ★ (P1)-(P18) STRICT PARSING: exit 5, nothing guessed ═════════════════════════
        n = L1
        cases = [
            ("(P1) no arguments at all is a usage refusal, and the usage begins "
             "'usage: lane-worktree.py'", (), "lane-worktree: usage: lane-worktree.py", True),
            ("(P2) an unknown verb is refused", ("frobnicate",), "unknown verb 'frobnicate'", False),
            ("(P3) an unknown option is refused, never ignored", ("remove", n, "--nope"),
             "unknown option '--nope'", False),
            ("(P4) an ABBREVIATED option is refused -- exact long options only",
             ("remove", n, "--discard-evid"), "unknown option '--discard-evid'", False),
            ("(P5) an extra word after add's name and committish is refused",
             ("add", n, "HEAD", "extra"), "unexpected argument 'extra'", False),
            ("(P6) a second lane name for remove is refused", ("remove", n, "other"),
             "unexpected argument 'other'", False),
            ("(P7) list takes no argument", ("list", "extra"), "unexpected argument 'extra'", False),
            ("(P8) an EMPTY --repo is refused, never read as this program's own tree",
             ("--repo", "", "list"), "--repo needs a directory", False),
            ("(P9) an empty --repo= is refused", ("--repo=", "list"), "--repo needs a directory",
             False),
            ("(P10) --repo with no value is refused", ("list", "--repo"), "--repo needs a directory",
             False),
            ("(P11) a blank --repo is refused", ("--repo", "   ", "list"), "--repo needs a directory",
             False),
            ("(P12) --repo given twice is refused", ("--repo", box, "--repo", box, "list"),
             "more than once", False),
            ("(P13) --preserve-to with no value is refused", ("remove", n, "--preserve-to"),
             "--preserve-to needs a directory", False),
            ("(P14) --preserve-to followed by an option is refused, never taken as a directory",
             ("remove", n, "--preserve-to", "--discard-evidence"), "--preserve-to needs a directory",
             False),
            ("(P15) an option given twice is refused", ("remove", n, "--discard-work",
                                                       "--discard-work"), "more than once", False),
            ("(P16) add with no name prints its usage", ("add",),
             "usage: lane-worktree.py add <name>", False),
            ("(P17) remove with an EMPTY name prints its usage", ("remove", ""),
             "usage: lane-worktree.py remove <name>", False),
            ("(P18) --self-test runs alone -- never nested inside another command", ("--self-test",
                                                                                     "list"),
             "--self-test runs alone", False),
        ]
        for label, argv, needle, at_start in cases:
            r = fx.run(*argv)
            check(label, r.rc == 5 and (r.err.startswith(needle) if at_start else needle in r.err),
                  _said(r))

        # ══ ★ (N1)-(N14) THE NAME GRAMMAR, WITH WHAT A BAD NAME WOULD RESOLVE TO PLANTED ══
        # Each bad name is asked to be removed WITH both destructive flags, so only the grammar
        # stands between it and the recursive delete; the sentinel at the path it names survives.
        def sentinel(*parts):
            return os.path.join(*parts + ("sentinel.txt",))

        names = [
            ("(N1) '../..'", "../..", os.path.join(box, "sentinel-up.txt")),
            ("(N2) '..' (the fixture itself)", "..", os.path.join(fx.root, "tracked.txt")),
            ("(N3) 'a/b'", "a/b", sentinel(fx.container, "a", "b")),
            ("(N4) 'a\\b'", "a\\b", sentinel(fx.container, "a\\b")),
            ("(N5) '.x' (a hidden entry, the shape of .manifests)", ".x", sentinel(fx.container, ".x")),
            ("(N6) 'a b' (whitespace)", "a b", sentinel(fx.container, "a b")),
            ("(N7) a blank name", "   ", None),
            ("(N8) 'Ab' (uppercase)", "Ab", sentinel(fx.container, "Ab")),
            ("(N9) 'a--b' (a doubled hyphen)", "a--b", sentinel(fx.container, "a--b")),
            ("(N10) 'a-' (a trailing hyphen)", "a-", sentinel(fx.container, "a-")),
        ]
        for _label, _name, path in names:
            if path and not os.path.exists(path):
                fx.write(path, "sentinel\n")
        for label, bad, path in names:
            planted = path is None or os.path.isfile(path)
            r = fx.run("remove", bad, "--discard-work", "--discard-evidence")
            check("%s is REFUSED by the grammar (exit 5), with both destructive flags given, and "
                  "what it would name survives" % label, planted and r.rc == 5
                  and NAME_RULE_TEXT in r.err and (path is None or os.path.isfile(path)),
                  "planted=%s\n%s" % (planted, _said(r)))
        long_name = "abcdefghijk"
        long_path = sentinel(fx.container, long_name)
        fx.write(long_path, "sentinel\n")
        r = fx.run("remove", long_name, "--discard-work", "--discard-evidence")
        check("(N11) a name one character over worktrees.maxNameLength is REFUSED (exit 5) naming "
              "the key, and its directory survives", r.rc == 5 and "worktrees.maxNameLength" in r.err
              and os.path.isfile(long_path), _said(r))
        r = fx.run("remove", long_name[:-1])
        check("(N12) CONTROL: a name of exactly worktrees.maxNameLength passes the grammar (removing "
              "a lane that does not exist is exit 0)", r.rc == 0 and "VERIFIED absent" in r.out,
              _said(r))
        r = fx.run("add", "a/b")
        check("(N13) add refuses a path-shaped name (exit 5) and registers nothing",
              r.rc == 5 and NAME_RULE_TEXT in r.err
              and not fx.registered(os.path.join(fx.container, "a", "b"))[0], _said(r))
        r = fx.run("add", "Ab")
        check("(N14) add refuses an uppercase name (exit 5)", r.rc == 5 and NAME_RULE_TEXT in r.err,
              _said(r))
        for _label, _name, path in names + [("", long_name, long_path)]:
            if path and os.path.dirname(path) != fx.root and os.path.dirname(path) != box:
                top = os.path.relpath(path, fx.container).replace(os.sep, "/").split("/")[0]
                ot.remove_tree(os.path.join(fx.container, top))
        with contextlib.suppress(OSError):
            os.remove(os.path.join(box, "sentinel-up.txt"))

        # ══ ★ (G4) THE WORKTREES ROOT MUST BE IGNORED (exit 4) ════════════════════════════
        fx.write(".gitignore", _FIXTURE_IGNORE.replace("/.worktrees/\n", ""))
        neg = fx.git(["-C", fx.root, "check-ignore", "-q", "--", ".worktrees/"]).returncode
        r = fx.run("add", G4)
        created = (os.path.lexists(fx.lane(G4)) or fx.registered(fx.lane(G4))[0]
                   or os.path.lexists(fx.manifest(G4)))
        fx.write(".gitignore", _FIXTURE_IGNORE)
        ctl = fx.git(["-C", fx.root, "check-ignore", "-q", "--", ".worktrees/"]).returncode
        check("(G4) with the root's rule gone from .gitignore (check-ignore answers 1), add is "
              "REFUSED with exit 4 and creates nothing", neg == 1 and r.rc == 4
              and ".worktrees/ is NOT ignored" in r.err and not created,
              "check-ignore=%s created=%s\n%s" % (neg, created, _said(r)))
        check("(G4) CONTROL: with the rule restored, check-ignore answers 0", ctl == 0,
              "check-ignore=%s" % ctl)

        # ══ ★ (B1)(B2) THE PATH BUDGET, EXACT AT THE FIXTURE'S OWN PATH ══════════════════
        longest = max(("-".join((leg["processor"], leg["toolchain"], leg["config"]))
                       for leg in _FIXTURE_LEGS.values()), key=len)
        w = _FIXTURE_WORKTREES
        build = len("/build/") + len(longest) + len("/")
        total_b = len(fx.lane(B1)) + build + w["pathBudgetReserve"] + w["pathBudgetMargin"]
        limit = total_b + 1                                  # B1 fits with nothing to spare
        over = B1 + "x"
        fx.write_config(worktrees={"pathLimit": limit})
        r = fx.run("add", over)
        terms = ["the lane root '%s' is %d chars" % (fx.lane(over), len(fx.lane(over))),
                 "'/build/' 7", "the longest build variant '%s' %d" % (longest, len(longest)),
                 "worktrees.pathBudgetReserve %d" % w["pathBudgetReserve"],
                 "worktrees.pathBudgetMargin %d" % w["pathBudgetMargin"], "= %d" % (total_b + 1),
                 "not under worktrees.pathLimit %d" % limit,
                 "at most %d character(s) still fits" % len(B1)]
        check("(B1) a name ONE character over the exact budget is REFUSED with exit 3, and nothing "
              "is created", r.rc == 3 and not os.path.lexists(fx.lane(over))
              and not os.path.lexists(fx.manifest(over)), _said(r))
        check("(B1) ... the refusal names every term -- the lane root and its length, /build/, the "
              "LONGEST variant (not the first leg's), the reserve, the margin, the limit -- and how "
              "long a name still fits", all(t in r.err for t in terms),
              "missing=%s\n%s" % ([t for t in terms if t not in r.err], _said(r)))
        r = fx.run("add", B1)
        check("(B2) CONTROL: the name one character SHORTER is admitted at exactly the boundary",
              r.rc == 0 and "path budget OK" in r.out and "(the name could grow by 0)" in r.out
              and os.path.isdir(fx.lane(B1)), _said(r))
        fx.write_config()
        r = fx.run("remove", B1)
        check("(B2) ... and removes cleanly (exit 0, the directory really GONE)", removed(r, B1),
              _said(r))

        # ══ ★ (CF1)-(CF8) EVERY DIRECTORY AND NUMBER COMES FROM THE CONFIGURATION ══════════
        lanes_root = os.path.join(fx.root, "lanes")
        fx.write_config(worktrees={"root": "lanes"})
        r = fx.run("add", R1)
        check("(CF1) worktrees.root is READ from the configuration: 'lanes' puts the lane at "
              "lanes/<name>, its manifest in lanes/.manifests, and nothing under .worktrees/",
              r.rc == 0 and os.path.isdir(os.path.join(lanes_root, R1)) and gone(R1)
              and os.path.isfile(fx.manifest(R1, lanes_root)), _said(r))
        r = fx.run("remove", R1)
        check("(CF1) ... and remove finds and removes it there", removed(r, R1, lanes_root),
              _said(r))
        fx.write_config(worktrees={"evidenceRoots": ["scratchpad", ".temp", "keep"]})
        fx.add(E1)
        fx.write(os.path.join(fx.lane(E1), "keep", "k.log"), "kept by a configured root\n")
        r = fx.run("remove", E1)
        check("(CF2) an evidence root the CONFIGURATION adds is gated: keep/=1 refuses with exit 7",
              r.rc == 7 and "keep/=1" in r.err and os.path.isdir(fx.lane(E1)), _said(r))
        fx.write_config()
        r = fx.run("remove", E1)
        check("(CF2) CONTROL: without it in evidenceRoots, the same lane removes with no flag",
              removed(r, E1), _said(r))
        refusals = [
            ("(CF3) a MISSING key is refused with exit 2, naming it", dict(drop=("pathLimit",)),
             "worktrees.pathLimit is MISSING"),
            ("(CF4) an ill-typed key is refused with exit 2, naming it",
             dict(worktrees={"maxNameLength": "10"}), "worktrees.maxNameLength is str '10'"),
            ("(CF5) an EMPTY evidenceRoots is refused -- it would gate no evidence at all",
             dict(worktrees={"evidenceRoots": []}), "worktrees.evidenceRoots is list []"),
            ("(CF6) a worktrees.root outside the repository is refused",
             dict(worktrees={"root": "../outside"}), "worktrees.root is '../outside'"),
            ("(CF7) no legs is refused -- the budget needs the longest build variant",
             dict(drop=("legs",)), "legs is MISSING"),
        ]
        for label, kw, needle in refusals:
            fx.write_config(**kw)
            r = fx.run("list")
            check(label, r.rc == 2 and needle in r.err, _said(r))
        fx.write_config()
        noconf = os.path.join(box, "noconf")
        fx.git_ok(["init", "-q", noconf])
        r = fx.run("--repo", noconf, "list")
        check("(CF8) a tree with no .harness-config/config.json is refused with exit 2 naming the "
              "file, never given defaults", r.rc == 2 and "config.json" in r.err
              and "cannot read" in r.err, _said(r))

        # ══ ★ (CF9)-(CF11) THE SEED DIRECTORY IS lane-fold's TO NAME, READ FROM ITS OWN FILE ══
        # lane-fold READS `<root>/<manifests>/seed-<lane>.json`; `add` WRITES it. Both take the
        # name from lane-fold-config.json, so a tool that kept a constant here would put the seed
        # where lane-fold never looks -- which is what (CF9) reds on.
        fx.write_lane_fold_config(manifests=".seeds")
        r = fx.run("add", M1)
        check("(CF9) `manifests` is READ from lane-fold-config.json: '.seeds' puts the seed at "
              "<root>/.seeds/seed-<name>.json, and nothing under .manifests",
              r.rc == 0 and os.path.isfile(fx.manifest(M1, manifests=".seeds"))
              and not os.path.lexists(fx.manifest(M1)), _said(r))
        r = fx.run("remove", M1)
        check("(CF9) ... and the lane removes cleanly, its seed directory never taken for a lane",
              removed(r, M1) and ".seeds" not in fx.run("list").out, _said(r))
        fx.write_lane_fold_config()
        lf_rel = "/".join(LANE_FOLD_CONFIG_SEGMENTS)
        os.remove(fx.path(lf_rel))
        r = fx.run("list")
        check("(CF10) a tree with no lane-fold-config.json is refused with exit 2 naming the file, "
              "never given a default", r.rc == 2 and "lane-fold-config.json" in r.err
              and "cannot read lane-fold's configuration" in r.err, _said(r))
        for label, value, needle in (
                ("(CF11) a `manifests` that is not dot-led is refused with exit 2 -- it could be "
                 "taken for a lane", "manifests", "manifests is str 'manifests'"),
                ("(CF11) a `manifests` holding a separator is refused with exit 2 -- it would leave "
                 "the worktrees root", ".a/b", "manifests is str '.a/b'"),
                ("(CF11) a MISSING `manifests` is refused with exit 2, naming it", None,
                 "manifests is MISSING")):
            fx.write_lane_fold_config(manifests=value)
            r = fx.run("list")
            check(label, r.rc == 2 and needle in r.err, _said(r))
        fx.write_lane_fold_config()

        # ══ ★ (D3)(D5) WHAT --repo MAY NAME: a directory, of a tree git answers for ══════
        # ⓘ The `.ps1` took a FILE's directory; the `.sh` refused it -- directories only (report
        # 05 D.5). A DSS tree nested in a checkout it is not the root of is refused: git would
        # answer for the enclosing checkout (the A-inside-G rule, owning-tree's `owning_root`).
        r = fx.run("--repo", os.path.join(fx.root, "tracked.txt"), "list")
        check("(D3) --repo naming a FILE is refused with exit 2 -- a tree is named by a directory",
              r.rc == 2 and "is not a directory" in r.err, _said(r))
        r = fx.run("--repo", os.path.join(box, "no-such-directory"), "list")
        check("(D3) --repo naming nothing is refused with exit 2", r.rc == 2
              and "no such directory" in r.err, _said(r))
        outer = os.path.join(box, "outer")
        nested = os.path.join(outer, "copy")
        fx.git_ok(["init", "-q", outer])
        os.makedirs(os.path.join(nested, ".plans"))
        fx.write(os.path.join(nested, ".harness-config", "config.json"), fx.config_text())
        top, _why = ot.git_top_level(nested)
        real = top is not None and ot.same_path(top, outer)
        r = fx.run("--repo", nested, "list")
        check("(D5) --repo naming a DSS tree nested inside another checkout is refused with exit 2 "
              "-- git answers for the enclosing one", real and r.rc == 2
              and "nested inside the checkout" in r.err,
              "negative-synthesized=%s (git's top level for it: %s)\n%s" % (real, top, _said(r)))

        # ══ ★ (F1)-(F3) (E1r) FAULTS INJECTED INTO THE COPY, IN-PROCESS ═══════════════════
        fx.add(F1)
        fx.write(os.path.join(fx.lane(F1), "scratchpad", "e.log"), "lane evidence\n")
        kf1 = os.path.join(box, "kf1")
        real_copy = subject._copy_evidence_file

        def corrupt(src, dst):                  # same SIZE, other bytes: only the hash can see it
            with open(src, "rb") as fh:
                data = fh.read()
            with open(dst, "wb") as fh:
                fh.write(bytes(b ^ 0xFF for b in data))

        subject._copy_evidence_file = corrupt
        try:
            r = fx.run("remove", F1, "--preserve-to", kf1)
        finally:
            subject._copy_evidence_file = real_copy
        copied = _read(os.path.join(kf1, "scratchpad", "e.log")) if os.path.isfile(
            os.path.join(kf1, "scratchpad", "e.log")) else None
        check("(F1) a copy that does not re-read identical (same size, other bytes) is REFUSED with "
              "exit 7, naming the file and why, and the lane survives",
              copied is not None and copied != b"lane evidence\n" and r.rc == 7
              and "preserve VERIFY FAILED" in r.err and "scratchpad/e.log (content differs)" in r.err
              and os.path.isdir(fx.lane(F1)), "copied=%r\n%s" % (copied, _said(r)))
        fx.cleanup(F1, "--discard-evidence")

        fx.add(F2)
        fx.write(os.path.join(fx.lane(F2), "scratchpad", "a.log"), "a\n")
        kf2 = os.path.join(box, "kf2")
        real_count = subject.count_evidence

        def count_then_grow(lane, roots):
            got = real_count(lane, roots)
            fx.write(os.path.join(lane, "scratchpad", "late.log"), "late\n")
            return got

        subject.count_evidence = count_then_grow
        try:
            r = fx.run("remove", F2, "--preserve-to", kf2)
        finally:
            subject.count_evidence = real_count
        check("(F2) a file added between the count and the preserve is REFUSED with exit 7 (the "
              "tree changed underneath the verb), nothing copied, the lane kept",
              r.rc == 7 and "the tree changed" in r.err and not fx.files_under(kf2)
              and os.path.isdir(fx.lane(F2)), _said(r))
        fx.cleanup(F2, "--discard-evidence")

        fx.write(os.path.join(fx.lane(F3), "stray.txt"), "not a checkout\n")
        real_delete = subject._delete_tree
        subject._delete_tree = lambda path: True
        try:
            r = fx.run("remove", F3, "--discard-work")
        finally:
            subject._delete_tree = real_delete
        check("(F3) a delete that does nothing is caught: exit 6 'STILL ON DISK', never success",
              r.rc == 6 and "STILL ON DISK" in r.err and "VERIFIED absent" not in r.out
              and os.path.isdir(fx.lane(F3)), _said(r))
        fx.cleanup(F3, "--discard-work")

        fx.write(os.path.join(fx.lane(E1R), "scratchpad", "x.log"), "x\n")
        real_walk = subject._walk

        def unreadable(top):
            raise PermissionError(13, "injected by the self-test: permission denied", top)

        subject._walk = unreadable
        try:
            r = fx.run("remove", E1R, "--discard-work", "--discard-evidence")
        finally:
            subject._walk = real_walk
        check("(E1r) evidence that cannot be COUNTED is refused with exit 7 even with "
              "--discard-evidence -- never read as 'no evidence'",
              r.rc == 7 and "could not COUNT" in r.err and os.path.isdir(fx.lane(E1R)), _said(r))
        fx.cleanup(E1R, "--discard-work", "--discard-evidence")

        # ══ ★ (K1)(K2) A LOCKED LANE: its registration must be VERIFIED gone ═══════════════
        fx.add(K1)
        fx.git_ok(["-C", fx.root, "worktree", "lock", "--reason", "self-test", fx.lane(K1)])
        neg = fx.registered(fx.lane(K1))
        r = fx.run("remove", K1)
        after = fx.registered(fx.lane(K1))
        check("(K1) a LOCKED lane (a single --force declines it, prune keeps it) is removed: exit 0, "
              "the directory gone, and its registration VERIFIED gone from worktree list",
              neg == (True, True) and removed(r, K1) and "LOCKED" in r.out and after == (False, False),
              "before=%s after=%s\n%s" % (neg, after, _said(r)))
        fx.add(K2)
        fx.git_ok(["-C", fx.root, "worktree", "lock", "--reason", "self-test", fx.lane(K2)])
        real_unlock = subject._unlock_worktree
        subject._unlock_worktree = lambda repo, lane: None
        try:
            r = fx.run("remove", K2)
        finally:
            subject._unlock_worktree = real_unlock
        still = fx.registered(fx.lane(K2))
        check("(K2) a registration that SURVIVES the removal is refused with exit 6 -- never "
              "reported as removed", r.rc == 6 and "STILL REGISTERS" in r.err
              and "VERIFIED absent" not in r.out and still == (True, True),
              "registered=%s\n%s" % (still, _said(r)))
        r = fx.run("remove", K2)
        check("(K2) ... and a re-run with the real unlock settles it: exit 0, the registration gone",
              removed(r, K2) and fx.registered(fx.lane(K2)) == (False, False), _said(r))

        # ══ ★ (X3)(X3b)(ND) WHAT THE LANE PATH IS ═══════════════════════════════════════
        target = os.path.join(box, "x3-target")
        fx.write(os.path.join(target, "sentinel.txt"), "outside the worktrees root\n")
        os.makedirs(fx.container, exist_ok=True)
        kind, why = _make_dir_link(fx.lane(X1), target)
        if kind is None:
            na("(X3) a lane path that is a LINK resolving OUTSIDE the worktrees root is REFUSED "
               "with exit 2", "no directory link can be made here (%s)" % why)
            na("(X3b) a lane path that is a LINK to a SIBLING lane is REFUSED with exit 2",
               "no directory link can be made here (%s)" % why)
        else:
            neg = ot.same_path(os.path.realpath(fx.lane(X1)), target)
            r = fx.run("remove", X1, "--discard-work", "--discard-evidence")
            check("(X3) a lane path that is a LINK (%s) resolving OUTSIDE the worktrees root is "
                  "REFUSED with exit 2 before anything is read or removed; the target and the link "
                  "survive" % kind, neg and r.rc == 2 and "not" in r.err and "strictly inside"
                  in r.err and os.path.isfile(os.path.join(target, "sentinel.txt"))
                  and os.path.lexists(fx.lane(X1)), "negative-synthesized=%s\n%s" % (neg, _said(r)))
            _remove_link(fx.lane(X1))
            fx.add(XS)
            kind2, why2 = _make_dir_link(fx.lane(XL), fx.lane(XS))
            neg = kind2 is not None and ot.is_within(fx.lane(XL), fx.container, strict=True)
            r = fx.run("remove", XL, "--discard-work", "--discard-evidence")
            sibling = (os.path.isdir(fx.lane(XS)), fx.registered(fx.lane(XS))[0])
            check("(X3b) a lane path that is a LINK to a SIBLING lane -- strictly inside the root, "
                  "so containment alone passes it -- is REFUSED with exit 2, and the sibling "
                  "survives, still registered", neg and r.rc == 2 and "is a LINK" in r.err
                  and sibling == (True, True),
                  "negative-synthesized=%s %s sibling-directory-survives=%s "
                  "sibling-still-registered=%s\n%s" % (neg, why2, sibling[0], sibling[1],
                                                       _said(r)))
            if kind2 is not None:
                _remove_link(fx.lane(XL))
            fx.cleanup(XS)
        fx.write(fx.lane(ND), "i am not a lane\n")
        r = fx.run("remove", ND, "--discard-work", "--discard-evidence")
        check("(ND) a FILE at <root>/<name> is refused with exit 2 as not a directory, and "
              "survives", r.rc == 2 and "exists but is a file" in r.err
              and os.path.isfile(fx.lane(ND)), _said(r))
        os.remove(fx.lane(ND))
    except _FixtureError as exc:
        print("  FAIL fixture step: %s" % exc)
        return "a fixture step failed: %s" % str(exc).splitlines()[0]
    return None


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
