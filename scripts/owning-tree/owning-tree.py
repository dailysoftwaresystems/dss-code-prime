#!/usr/bin/env python3
# PURPOSE: name the DSS tree a script's own file lives in -- walked up from that file, never taken from the caller's working directory or git environment.
"""owning-tree.py -- the Python owner of "WHICH DSS TREE DOES THIS SCRIPT BELONG TO?", and of
asking git about that tree without the caller's git environment.

★★★ WHY THIS EXISTS. Eight repository guards and tools derived their root from a bare
`git rev-parse --show-toplevel`, and three more read a path relative to the process
working directory. Both answer "what is my CALLER standing in?" -- never "which tree
do I belong to?". ✔MEASURED 2026-09-15 (P66, lane rr), each script run by absolute
path with its working directory inside a DIFFERENT git repository, a clone carrying
decoys that only a scan of that clone could name:
  * `check-anchor-balance` counted the other tree's registries and named its decoy row;
  * `apply-registry-row` ACCEPTED a row that exists only in the other tree's registry
    (a dry run -- with `--apply` the write lands there);
  * `check-diagnostic-codes` and `check-stale-refusal-citations` said OK over the
    other tree's file counts, silently;
  * `check-plan-citations` and `check-wrapped-anchor-ids` reddened on the other
    tree's decoy document, and `check-scripts-index` and
    `check-guard-output-encoding` collapsed on a directory only the other tree lacks;
and from a directory inside NO repository every one of them refused the tree it
lives in. ctest pins `WORKING_DIRECTORY` to the source tree, so no gate could see
either direction. The same class, closed for the lane verbs in P53:
[[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]].

★★ THE RULE: walk up from the script's own file to the NEAREST directory holding BOTH
`.plans/` and `scripts/` -- the rule `scripts/anchors/anchors.py` has always used. The
answer is a property of the file's PATH and of nothing the caller controls.

⚠ WHY NOT `git -C <the script's directory> rev-parse --show-toplevel`, the spelling the
lane verbs adopted. ✔MEASURED on the same probe, four resolvers against one anchor
directory (`bare` is the defect, `walk` is this file):
                                                bare      git -C      walk
    cwd = its own tree                          own       own         own
    cwd = another git repository                OTHER     own         own
    cwd = no repository                         refuses   own         own
    caller exports GIT_DIR                      OTHER     a non-root  own
    caller exports GIT_DIR and GIT_WORK_TREE    OTHER     OTHER       own
    an untracked copy of the tree, nested       OTHER     OTHER       own
    WSL git over a Windows-created worktree     refuses   refuses     own
⚠ AND WHY NOT A FIXED `__file__/../..`: that spelling has already named a wrong root
silently in this repository, when a script family moved one level deeper (the TF-C87
note in `scripts/corpus-census/corpus-census.py`). The walk survives a move.

★★★ AND GIT IS NEVER ASKED IN THE CALLER'S GIT ENVIRONMENT. `git -C` moves git's cwd and
nothing else: an exported `GIT_DIR`, `GIT_WORK_TREE` or `GIT_INDEX_FILE` still decides
which repository answers. ✔MEASURED 2026-09-15 (P66 lane rr, round 2), queries only:
  * with another repository's `GIT_INDEX_FILE` or `GIT_DIR` exported, `git ls-files
    --cached --others --exclude-standard` in this tree listed THAT repository's paths,
    and `check-stale-refusal-citations` reported OK over 3350 files instead of 3349, rc=0;
  * with its `GIT_DIR` exported, `check-anchor-balance` read THAT repository's HEAD as its
    baseline ("OPEN at HEAD registry=0 ... opened 447"), and the agreement check below
    passed, because `GIT_DIR` alone makes git call the `-C` directory its own top level;
  * a pre-commit hook receives `GIT_INDEX_FILE` -- ABSOLUTE during a partial
    `git commit -- <path>` -- so such a caller is not hypothetical.
⇒ `git_environment()` is this process's environment minus every name
`git rev-parse --local-env-vars` lists: git's own definition of "selects a repository",
the set git clears itself when it enters a submodule. `run_git()` is the ONE way the
scripts that read their subject through git run it. Configuration passed through
`GIT_CONFIG_PARAMETERS` / `GIT_CONFIG_COUNT` is in that list too and does not reach these
calls; nothing in this repository's CI sets either (✔MEASURED under .github/ and docker/).

★★ WHERE A CALLER READS ITS SUBJECT *THROUGH* GIT, git must be answering about the
same tree. `resolve(path, reads_git=True)` refuses unless git's top level for that root
IS the root -- the rule `check-line-endings` states as "the enumeration root and the read
root must be the same root". Asked through `run_git`, a caller's environment neither
steers nor trips it; what it refuses is a tree copied inside another checkout, whose
files would otherwise be read against the enclosing checkout's git view.

★ `root_arms` IS WHAT EACH CONSUMER'S SELF-TEST RUNS AGAINST ITS OWN RESOLVER, so a
consumer whose root is reverted to a cwd-keyed call, or whose git agreement is dropped,
reddens its own guard by arm name. `steering_env` and `caller_environment` let a consumer
pin that its own git calls ignore a steering environment. This file's `--self-test`
proves the arms themselves can fail.

ⓘ NO `.sh` / `.ps1` TWIN, DELIBERATELY: every consumer is Python, and a `.py` runs
unchanged on the Windows leg and on every POSIX leg. The shell and PowerShell owners of
the ADJACENT git question ("which git working tree contains <path>?") are
`leg_tree_owning_root` in `scripts/leg-tree/leg-tree.sh` and `Get-RepoTreeOwningRoot`
in `scripts/repo-tree/repo-tree.ps1`, and they carry the same two rules in their own
languages: `leg_tree_git_unsteered` and `Invoke-RepoTreeUnsteeredGit`.

Usage:
    python scripts/owning-tree/owning-tree.py --self-test
Exit codes: 0 OK · 1 self-test failed · 3 usage.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

# A cp1252 console turns a printed glyph into a traceback; reconfigure before anything
# can print (the property `guard_output_encoding_guard` ratchets for every script).
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

MARKERS = (".plans", "scripts")

# What `steering_env` commits in its decoy repository, so a consumer can assert a steered
# listing would have named them and its own listing does not.
STEER_DECOY = "zz-owning-tree-steer-decoy.txt"
STEER_PLANS_DECOY = ".plans/zz-owning-tree-steer-decoy.md"

# ⚠ Raising this is the claim that the arms you added actually RUN.
EXPECTED_ARMS = 13


class Refusal(Exception):
    """No DSS tree contains the path, or git answers for a tree other than the one that does."""


def same_path(a, b):
    """True when two spellings name one directory -- case, separators and links folded."""
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def owning_tree(path):
    """The nearest ancestor of `path` (a directory counts as its own ancestor) holding every MARKER."""
    start = os.path.realpath(path)
    d = start if os.path.isdir(start) else os.path.dirname(start)
    while True:
        if all(os.path.isdir(os.path.join(d, m)) for m in MARKERS):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            raise Refusal("no DSS tree contains %s -- no ancestor holds BOTH %s"
                          % (start, " and ".join(m + "/" for m in MARKERS)))
        d = parent


# ── git, asked without the caller's git environment ──────────────────────────────────

_LOCAL_GIT_ENV_NAMES = None


def local_git_env_names():
    """The names git calls repository-local (`git rev-parse --local-env-vars`), resolved once.

    ✔MEASURED 2026-09-15: the query answers (15 names) even with GIT_DIR pointing nowhere.
    ⚠ REFUSES when the list does not even name GIT_DIR: without git's own list, a git call
    could not be kept from the caller's environment, and guessing the list would be a
    second definition of a fact git owns.
    """
    global _LOCAL_GIT_ENV_NAMES
    if _LOCAL_GIT_ENV_NAMES is None:
        try:
            p = subprocess.run(["git", "rev-parse", "--local-env-vars"], capture_output=True,
                               text=True, encoding="utf-8", errors="replace")
        except OSError as exc:
            raise Refusal("cannot run git (%s)" % exc)
        names = p.stdout.split() if p.returncode == 0 else []
        if "GIT_DIR" not in names:
            raise Refusal("`git rev-parse --local-env-vars` did not name GIT_DIR (rc=%d), so a "
                          "git call here cannot be kept from the caller's git environment"
                          % p.returncode)
        _LOCAL_GIT_ENV_NAMES = frozenset(names)
    return _LOCAL_GIT_ENV_NAMES


def git_environment(extra=None):
    """This process's environment WITHOUT anything that selects a git repository, plus `extra`."""
    names = local_git_env_names()
    env = {k: v for k, v in os.environ.items() if k not in names}
    env.update(extra or {})
    return env


def run_git(args, cwd=None, **kwargs):
    """`git <args>` in `cwd`, in `git_environment()` -- the ONE way git is asked about a tree here."""
    return subprocess.run(["git"] + list(args), cwd=cwd, env=git_environment(), **kwargs)


def bare_git(args, cwd, env):
    """`git <args>` as the DEFECT ran it: with `env` exported over the environment.

    For arms only -- the negative an arm must prove real before it trusts its own verdict.
    """
    return subprocess.run(["git"] + list(args), cwd=cwd, env=git_environment(env),
                          capture_output=True, text=True, encoding="utf-8", errors="replace")


def git_top_level(root):
    """-> (top level, "") as git reports it for `root`, or (None, why git could not say)."""
    try:
        p = run_git(["-C", root, "rev-parse", "--show-toplevel"], capture_output=True,
                    text=True, encoding="utf-8", errors="replace")
    except OSError as exc:
        return None, "cannot run git (%s)" % exc
    if p.returncode != 0:
        lines = (p.stderr or "").strip().splitlines()
        return None, lines[0] if lines else "git exited %d" % p.returncode
    return p.stdout.strip(), ""


def resolve(script_file, reads_git=False):
    """The root a script's own file belongs to. Raises Refusal, never guesses.

    `reads_git=True` is for a caller whose subject is read through git (`ls-files`,
    `ls-tree`, `show`): it additionally requires git's top level for that root to BE
    that root.
    """
    root = owning_tree(script_file)
    if reads_git:
        top, why = git_top_level(root)
        if top is None or not same_path(top, root):
            raise Refusal(
                "git does not answer for the tree this script lives in.\n"
                "  tree          : %s\n"
                "  git top level : %s\n"
                "  This script reads its subject THROUGH git, so it will not read one tree's "
                "files against another tree's git view. A copy of the tree nested inside "
                "another checkout, or a namespace this git cannot follow, produces exactly this."
                % (root, top if top is not None else "<none> -- " + why))
    return root


# ── the arms every consumer's self-test runs against ITS OWN resolver ────────────────

def remove_tree(path):
    """Remove a temp tree -- git's READ-ONLY object files included. True when it is gone.

    ✔MEASURED 2026-09-15 (Windows 11, Python 3.14.3): `shutil.rmtree(box, ignore_errors=True)`
    over a box holding one `git commit` left the box behind with its four loose objects
    read-only -- git writes them so, and Windows refuses to unlink a read-only file -- and
    this owner's own arm (10) reddened.
    """
    def _writable_then_retry(func, p, _exc):
        try:
            os.chmod(p, 0o700)
            func(p)
        except OSError:
            pass

    if path and os.path.lexists(path):
        try:
            if sys.version_info >= (3, 12):
                shutil.rmtree(path, onexc=_writable_then_retry)
            else:  # pragma: no cover - a pre-3.12 interpreter
                shutil.rmtree(path, onerror=_writable_then_retry)
        except OSError:
            pass
    return not (path and os.path.lexists(path))


class steering:
    """`with steering() as env:` -- `steering_env` built in a fresh temp box, removed on exit."""

    def __init__(self):
        self.box = None

    def __enter__(self):
        self.box = tempfile.mkdtemp(prefix="owning-tree-steer-")
        try:
            return steering_env(self.box)
        except BaseException:
            remove_tree(self.box)
            raise

    def __exit__(self, *exc):
        remove_tree(self.box)
        return False


class caller_environment:
    """`with caller_environment(env):` -- export `env` into THIS process, then restore exactly.

    What a steering caller does, reproduced for an arm. A variable absent before is REMOVED
    afterwards, never left set to empty: an empty `GIT_INDEX_FILE` is itself a steering value.
    """

    def __init__(self, env):
        self.env = dict(env)
        self.held = {}

    def __enter__(self):
        self.held = {k: os.environ.get(k) for k in self.env}
        os.environ.update(self.env)
        return self

    def __exit__(self, *exc):
        for k, v in self.held.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        return False


def steering_env(box):
    """A repository in `box` no consumer's tree contains, and the environment that steers git at it.

    It commits STEER_DECOY and STEER_PLANS_DECOY, so a steered `ls-files` names the one and a
    steered `ls-tree HEAD:.plans` the other. GIT_DIR, GIT_WORK_TREE and GIT_INDEX_FILE are ALL
    exported -- the combination root_arms' steering arm uses -- so a helper that removed only
    some of them is still handed a steering value.
    """
    decoy = os.path.join(box, "steer-decoy")
    os.makedirs(os.path.join(decoy, ".plans"))
    for rel in (STEER_DECOY, STEER_PLANS_DECOY):
        with open(os.path.join(decoy, rel.replace("/", os.sep)), "w", encoding="utf-8",
                  newline="\n") as fh:
            fh.write("a decoy only a steered git would name\n")
    for args in (["init", "-q", decoy], ["-C", decoy, "add", "-A"],
                 ["-C", decoy, "-c", "user.email=owning-tree@example.invalid",
                  "-c", "user.name=owning-tree", "-c", "commit.gpgsign=false",
                  "commit", "-q", "--no-verify", "-m", "decoy"]):
        run_git(args, capture_output=True)
    return {"GIT_DIR": os.path.join(decoy, ".git"), "GIT_WORK_TREE": decoy,
            "GIT_INDEX_FILE": os.path.join(decoy, ".git", "index")}


def _bare_top_level(cwd, env=None):
    """What a bare `git rev-parse --show-toplevel` says from `cwd` -- the answer the arms prove WRONG."""
    try:
        p = bare_git(["rev-parse", "--show-toplevel"], cwd, env or {})
    except OSError:
        return None
    return p.stdout.strip() if p.returncode == 0 else None


def _resolve_from(resolver, refusals, cwd, env=None):
    """`resolver()` with the PROCESS cwd at `cwd` and `env` exported; both restored before returning.

    ⚠⚠ THE ANSWER IS MADE ABSOLUTE HERE, WHILE THE PROCESS STILL STANDS IN `cwd`. A resolver
    that answers with a RELATIVE path -- `"."`, or the parent chain of a cwd-relative `Path` --
    names a directory only relative to where it was asked; compared after the cwd is restored,
    it silently names the RESTORED directory instead. ✔MEASURED 2026-09-15 (P66): exactly that
    kept `check-pkg-pipeline`'s guard GREEN with its workflow path reverted to a cwd-relative
    one -- the arms judged `"."` against the source tree ctest runs in. Self-test arm (8b).
    """
    held_cwd = os.getcwd()
    try:
        os.chdir(cwd)
        with caller_environment(env or {}):
            try:
                got = resolver()
            except refusals as exc:
                return None, str(exc)
            return (os.path.abspath(str(got)) if got is not None else None), ""
    finally:
        os.chdir(held_cwd)


def _first_line(text):
    return (text or "").strip().split("\n")[0][:160]


# Loads a COPIED consumer file and calls its root function by name, in a child process.
_NESTED_DRIVER = r'''
import importlib.util, sys
path, name = sys.argv[1], sys.argv[2]
try:
    spec = importlib.util.spec_from_file_location("nested_subject", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    print("ROOT=%s" % getattr(mod, name)())
except SystemExit as exc:
    print("REFUSED: %s" % exc)
except BaseException as exc:
    print("REFUSED: %s: %s" % (type(exc).__name__, exc))
'''


def _nested_copy_arm(resolver, script_file, oracle, box):
    """The consumer's OWN file, copied into an untracked DSS-shaped tree inside another checkout, must refuse.

    ★ A child process runs the COPY, so the resolver is judged exactly as it would run from
    there -- its `__file__`, its sibling owner, its import-time code. git's own answer for the
    copy is proven to be the enclosing checkout first, or the arm reports that instead.
    """
    label = ("root: its file COPIED into an untracked tree nested inside another checkout is "
             "REFUSED -- git answers for the enclosing one")
    name = getattr(resolver, "__name__", "")
    if not name.isidentifier():
        return (False, label, "the resolver has no module-level name (%r), so its copy cannot be "
                              "called" % name)
    me = os.path.realpath(script_file)
    outer = os.path.join(box, "outer")
    copy = os.path.join(outer, "copy")
    os.makedirs(os.path.join(copy, ".plans"))
    dest = os.path.join(copy, os.path.relpath(me, oracle))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copyfile(me, dest)
    owner = os.path.realpath(__file__)
    if not same_path(owner, me):
        odest = os.path.join(copy, "scripts", "owning-tree", "owning-tree.py")
        os.makedirs(os.path.dirname(odest), exist_ok=True)
        shutil.copyfile(owner, odest)
    run_git(["init", "-q", outer], capture_output=True)
    top, _why = git_top_level(copy)
    real = top is not None and same_path(top, outer)
    try:
        p = subprocess.run([sys.executable, "-c", _NESTED_DRIVER, dest, name], cwd=copy,
                           env=git_environment(), capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=180)
        said = (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        said = "TIMEOUT"
    refused = "REFUSED" in said and "does not answer" in said
    return (real and refused, label,
            "negative-synthesized=%s driver said %r" % (real, _first_line(said)))


def root_arms(resolver, refusals, reads_git, script_file):
    """-> [(ok, label, detail)]: `resolver()` names `script_file`'s OWN tree from any cwd.

    `resolver` is the consumer's own root function, called with no arguments; `refusals`
    is the tuple of exception types it refuses with (SystemExit for a `sys.exit`).
    Three arms, five when `reads_git`:
      CONTROL  cwd = its own tree                                     -> its own tree
               cwd inside ANOTHER git repository                      -> its own tree
               cwd inside NO git repository                           -> its own tree
      (git)    GIT_DIR + GIT_WORK_TREE + GIT_INDEX_FILE name another tree -> IGNORED: its own tree
      (git)    its file copied into an untracked tree inside another checkout -> REFUSED
    ⚠ THE ORACLE IS THE FILE'S LAYOUT POSITION, `<tree>/scripts/<name>/<file>` -- NOT this
    module's walk. A pin that derived its expectation through the helper it pins would
    pass whatever the helper said.
    ★ EVERY FOREIGN CONDITION IS PROVEN REAL BEFORE IT IS TRUSTED: a bare `rev-parse`
    must name the other repository from inside it, must fail from the non-repository
    directory, must follow the steering environment, and must name the enclosing checkout
    for the nested copy. An arm whose negative did not materialise reports that, instead of
    passing vacuously.
    """
    me = os.path.realpath(script_file)
    scripts_dir = os.path.dirname(os.path.dirname(me))
    oracle = os.path.dirname(scripts_dir)
    if os.path.basename(scripts_dir) != "scripts":
        return [(False, "root arms: the subject sits at <tree>/scripts/<name>/<file>",
                 "it sits at %s, so no oracle can name its tree" % me)]
    refusals = tuple(refusals)
    arms = []
    box = tempfile.mkdtemp(prefix="owning-tree-arms-")
    try:
        foreign = os.path.join(box, "foreign")
        nonrepo = os.path.join(box, "nonrepo")
        os.makedirs(foreign)
        os.makedirs(nonrepo)
        run_git(["init", "-q", foreign], capture_output=True)
        # ★ A ceiling at the box keeps `nonrepo` outside every repository even on a host
        # whose temp directory sits inside a checkout -- and the arm still PROVES it.
        ceiling = {"GIT_CEILING_DIRECTORIES": box}
        steer = {"GIT_DIR": os.path.join(foreign, ".git"), "GIT_WORK_TREE": foreign,
                 "GIT_INDEX_FILE": os.path.join(foreign, ".git", "index")}

        def own(got):
            return got is not None and same_path(got, oracle)

        got, why = _resolve_from(resolver, refusals, oracle)
        arms.append((own(got), "root CONTROL: cwd = its own tree -> its own tree",
                     "got=%s %s" % (got, _first_line(why))))

        neg = _bare_top_level(foreign)
        real = neg is not None and same_path(neg, foreign)
        got, why = _resolve_from(resolver, refusals, foreign)
        arms.append((real and own(got),
                     "root: cwd inside ANOTHER git repository -> still its own tree",
                     "negative-synthesized=%s got=%s %s" % (real, got, _first_line(why))))

        neg = _bare_top_level(nonrepo, ceiling)
        got, why = _resolve_from(resolver, refusals, nonrepo, ceiling)
        arms.append((neg is None and own(got),
                     "root: cwd inside NO git repository -> still its own tree",
                     "negative-synthesized=%s got=%s %s" % (neg is None, got, _first_line(why))))

        if reads_git:
            neg = _bare_top_level(oracle, steer)
            real = neg is not None and same_path(neg, foreign)
            got, why = _resolve_from(resolver, refusals, oracle, steer)
            arms.append((real and own(got),
                         "root: a caller's GIT_DIR + GIT_WORK_TREE + GIT_INDEX_FILE naming another "
                         "tree are IGNORED -> still its own tree",
                         "negative-synthesized=%s got=%s %s" % (real, got, _first_line(why))))
            arms.append(_nested_copy_arm(resolver, script_file, oracle, box))
    finally:
        remove_tree(box)
    return arms


# ── this owner's own self-test ────────────────────────────────────────────────────────
# Module-level on purpose: the nested-copy arm calls a resolver BY NAME inside a copy of
# this file, so the resolvers the self-test judges must be addressable from outside.

def _selftest_with_agreement():
    """The real resolver: the walk, with git's agreement asked through `run_git`."""
    return resolve(__file__, reads_git=True)


def _selftest_without_agreement():
    """The walk alone -- what dropping `reads_git=True` leaves."""
    return owning_tree(__file__)


def _selftest_agreement_in_callers_env():
    """The walk plus git's agreement asked in the CALLER's environment -- the round-1 spelling."""
    root = owning_tree(__file__)
    p = subprocess.run(["git", "-C", root, "rev-parse", "--show-toplevel"], capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    top = p.stdout.strip() if p.returncode == 0 else None
    if top is None or not same_path(top, root):
        raise Refusal("git does not answer for the tree this script lives in (asked in the "
                      "caller's environment)")
    return root


def _selftest_cwd_keyed():
    """The defect itself: a bare `git rev-parse --show-toplevel` from wherever the process stands."""
    p = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise Refusal("not inside a git repository")
    return p.stdout.strip()


def _selftest_relative():
    """A RELATIVE answer: `.` names wherever it was asked."""
    return "."


def _touch(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("")
    return path


def _ancestors(path):
    d = os.path.realpath(path)
    while True:
        yield d
        parent = os.path.dirname(d)
        if parent == d:
            return
        d = parent


def self_test():
    """Red-on-disable for the owner: the walk, git's agreement, the unsteered git, and the arms' ability to FAIL."""
    ran, failed = [], []

    def pin(ok, label, detail=""):
        ran.append(label)
        if not ok:
            failed.append(label)
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", label,
                               ("   [" + detail + "]") if detail and not ok else ""))

    def verdicts(resolver):
        return [a[0] for a in root_arms(resolver, (Refusal,), True, __file__)]

    box = tempfile.mkdtemp(prefix="owning-tree-selftest-")
    try:
        tree = os.path.join(box, "tree")
        probe = _touch(os.path.join(tree, "scripts", "probe", "probe.py"))
        os.makedirs(os.path.join(tree, ".plans"))
        pin(same_path(owning_tree(probe), tree),
            "(1) the walk names the directory holding BOTH .plans/ and scripts/")

        half = _touch(os.path.join(tree, "half", "sub", "x.py"))
        os.makedirs(os.path.join(tree, "half", ".plans"))
        pin(same_path(owning_tree(half), tree),
            "(2) .plans/ WITHOUT scripts/ is not a tree -- the walk continues to the real one")

        copy = os.path.join(tree, ".temp", "copy")
        cprobe = _touch(os.path.join(copy, "scripts", "probe", "probe.py"))
        os.makedirs(os.path.join(copy, ".plans"))
        pin(same_path(owning_tree(cprobe), copy),
            "(3) a tree copied INSIDE another tree is its own tree -- the NEAREST one wins")

        lone = os.path.join(box, "lone", "deep")
        os.makedirs(lone)
        clean = not any(all(os.path.isdir(os.path.join(a, m)) for m in MARKERS)
                        for a in _ancestors(lone))
        try:
            owning_tree(lone)
            msg = ""
        except Refusal as exc:
            msg = str(exc)
        pin(clean and "no DSS tree contains" in msg,
            "(4) a path under NO tree is REFUSED, naming what the walk looked for",
            "negative-synthesized=%s msg=%r" % (clean, msg[:80]))

        run_git(["init", "-q", tree], capture_output=True)
        try:
            got, why = resolve(probe, reads_git=True), ""
        except Refusal as exc:
            got, why = None, str(exc)
        pin(got is not None and same_path(got, tree),
            "(5) reads_git: a tree that IS its own git top level resolves", _first_line(why))

        try:
            resolve(cprobe, reads_git=True)
            why = ""
        except Refusal as exc:
            why = str(exc)
        pin("does not answer" in why,
            "(6) reads_git: a tree nested inside another checkout is REFUSED -- git answers "
            "for the enclosing one", "got=%r" % _first_line(why))

        steer = steering_env(box)
        listing = ["ls-files", "-z", "--cached", "--others", "--exclude-standard"]
        neg = bare_git(listing, tree, steer)
        real = STEER_DECOY in neg.stdout.split("\0")
        with caller_environment(steer):
            got = run_git(listing, cwd=tree, capture_output=True, text=True, encoding="utf-8",
                          errors="replace")
        listed = got.stdout.split("\0")
        pin(real and got.returncode == 0 and STEER_DECOY not in listed
            and "scripts/probe/probe.py" in listed,
            "(6b) run_git IGNORES a caller's GIT_DIR + GIT_WORK_TREE + GIT_INDEX_FILE: this "
            "tree's listing names none of another repository's paths",
            "negative-synthesized=%s rc=%d decoy-listed=%s" % (real, got.returncode,
                                                                STEER_DECOY in listed))

        arms = root_arms(_selftest_with_agreement, (Refusal,), True, __file__)
        pin(len(arms) == 5 and all(a[0] for a in arms),
            "(7) root_arms against THIS owner's resolver: control, another repository, no "
            "repository, a steering environment and a nested copy all hold",
            "; ".join("%s -> %s" % (a[1], a[2]) for a in arms if not a[0]))

        v = verdicts(_selftest_cwd_keyed)
        pin(v == [True, False, False, False, False],
            "(8) root_arms CAN FAIL: a cwd-keyed resolver passes only the CONTROL",
            "verdicts=%r" % v)

        # ★ A RELATIVE answer is the cwd-keyed defect wearing a different spelling: `.` names
        # wherever it was asked. It passes the steering arm, which is asked from the tree itself.
        v = verdicts(_selftest_relative)
        pin(v == [True, False, False, True, False],
            "(8b) root_arms CAN FAIL on a RELATIVE answer: `.` passes only the arms asked from "
            "the tree itself", "verdicts=%r" % v)

        v = verdicts(_selftest_without_agreement)
        pin(v == [True, True, True, True, False],
            "(9) root_arms CAN FAIL: a resolver that skips git's agreement fails only the "
            "nested-copy arm", "verdicts=%r" % v)

        v = verdicts(_selftest_agreement_in_callers_env)
        pin(v == [True, True, True, False, True],
            "(9b) root_arms CAN FAIL: git's agreement asked in the CALLER's environment fails "
            "only the steering arm", "verdicts=%r" % v)
    finally:
        remove_tree(box)
    pin(not os.path.exists(box), "(10) the self-test's temp trees were removed")

    if len(ran) != EXPECTED_ARMS:
        print("owning-tree self-test: FAIL -- %d arm(s) ran, %d expected. An arm that silently "
              "stops running is a property that silently stops being proven." % (len(ran), EXPECTED_ARMS))
        return 1
    if failed:
        print("owning-tree self-test: FAIL -- %d of %d arm(s)" % (len(failed), len(ran)))
        return 1
    print("owning-tree self-test: OK - %d arm(s), including the four that prove root_arms can fail."
          % len(ran))
    return 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    print(__doc__.rsplit("Usage:", 1)[-1].strip())
    return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
