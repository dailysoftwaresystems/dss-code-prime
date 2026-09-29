#!/usr/bin/env python3
# PURPOSE: name the DSS tree a script's own file lives in -- walked up from that file, never taken from the caller's working directory or git environment.
"""owning-tree.py -- the Python owner of "WHICH DSS TREE DOES THIS SCRIPT BELONG TO?", of
"WHICH GIT WORKING TREE CONTAINS THIS PATH, AND WHAT IS ITS IDENTITY?", and of asking git
about a tree without the caller's git environment.

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
either direction. The same class was closed for the lane verbs in P53, where the lane
worktree's repository root had been keyed on the caller's cwd.

★★ THE RULE: walk up from the script's own file to the NEAREST directory holding BOTH
`.plans/` and `.harness-config/` -- the rule
`.harness-config/runner/actions/anchors/anchors.py` has always used, with its second
marker retargeted when the repository's programs moved out of the retired `scripts/`
directory into `.harness-config/runner/actions/` (2026-09-18, lane mig). The marker is
the directory that HOLDS the programs, so the answer is still "the tree whose program
directory this file sits in". The answer is a property of the file's PATH and of nothing
the caller controls.
⚠ THE RETARGET WAS FORCED, NOT CHOSEN: with `scripts/` gone, the old pair matched NO
ancestor of any program, so every consumer of this walk would have refused its own tree.
⚠ AN EMPTY PATH IS REFUSED, AND SO IS A PATH THAT DOES NOT EXIST (2026-09-21, lane mig).
`os.path.realpath('')` IS the working directory and `git -C ''` leaves git in it, so an
empty argument would have answered for whatever the caller stands in -- the exact class
this file exists to close, reachable through its own front door. The retired shell owner
refused both; this file now does too (`UsageRefusal`).

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
note in `.harness-config/runner/actions/corpus-census/corpus-census.py`). The walk survives a move.

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
scripts that read their subject through git run it, and `run_unsteered()` runs any
OTHER program (gh, a consumer's child) under the same rule. Configuration passed through
`GIT_CONFIG_PARAMETERS` / `GIT_CONFIG_COUNT` is in that list too and does not reach these
calls; nothing in this repository's CI sets either (✔MEASURED under .github/ and docker/).
★ A git child never inherits the caller's STDIN either (`stdin=DEVNULL` by default): a
guard runs under ctest, whose pipes are not a terminal, and a child that waited on them
would hang the guard with nothing on screen (the 2026-09-07 hang class, never root-caused).

★★ WHERE A CALLER READS ITS SUBJECT *THROUGH* GIT, git must be answering about the
same tree. `resolve(path, reads_git=True)` refuses unless git's top level for that root
IS the root -- the rule `check-line-endings` states as "the enumeration root and the read
root must be the same root". Asked through `run_git`, a caller's environment neither
steers nor trips it; what it refuses is a tree copied inside another checkout, whose
files would otherwise be read against the enclosing checkout's git view.

★★ THE TREE'S GIT IDENTITY, INCLUDING THE CASE GIT ITSELF CANNOT FOLLOW. `identity(tree)`
answers root, gitdir, HEAD sha and branch in three ordered cases: (1) git answers for the
directory as it stands; (2) only when `.git` is a FILE, its `gitdir:` value is resolved by
this file -- a Windows drive path read on a POSIX host is translated with `wslpath -u`
(the lane worktrees Windows creates, read from WSL: lane `gw`, P51), a POSIX absolute path
read on Windows is refused by name, and a RELATIVE value is joined to the WORKTREE, never
to the process's cwd. `tree_git(identity, args)` then asks git through that identity, so
no caller ever re-derives the gitdir, and `tree_git_lines` REFUSES an exit code the caller
did not declare acceptable (a corrupt index is "cannot run", never "nothing found").
`owning_root(path)` answers the ADJACENT question -- which git working tree contains a
path -- and refuses a DSS tree nested inside another checkout, where git answers for the
enclosing one. `assert_one_root` / `resolve_path` / `enter` keep the ENUMERATION root (what
git lists) and the READ root (where the bytes are opened) the same root: git run with its
own `-C` while a file is opened relative to the process cwd reads one tree's listing
against another tree's bytes.
These were the shell and PowerShell helpers `leg-tree.sh` and `repo-tree.ps1` until
2026-09-21 (lane mig, part 4: "no .sh/.ps1 under .harness-config/runner/actions"); their
carriage half (`prepare`/`restore`/the remote loader) had no caller and retired with them.

★ `root_arms` IS WHAT EACH CONSUMER'S SELF-TEST RUNS AGAINST ITS OWN RESOLVER, so a
consumer whose root is reverted to a cwd-keyed call, or whose git agreement is dropped,
reddens its own guard by arm name. `steering_env` and `caller_environment` let a consumer
pin that its own git calls ignore a steering environment, and `sandbox()` is the fenced
scratch box a consumer builds its fixtures in. This file's `--self-test` proves every
answer and proves the arms themselves can fail.

Usage:
    python .harness-config/runner/actions/owning-tree/owning-tree.py --self-test
Exit codes: 0 OK · 1 self-test failed · 3 usage.
"""
from __future__ import annotations

import collections
import json
import os
import re
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

MARKERS = (".plans", ".harness-config")

# Where every program this walk serves sits, relative to its tree: the actions directory
# DssHarness defines (`help layout`). `root_arms` derives its ORACLE from this LAYOUT
# position, never from the walk above, so a pin cannot pass by agreeing with itself.
ACTIONS_SEGMENTS = (".harness-config", "runner", "actions")

# What `steering_env` commits in its decoy repository, so a consumer can assert a steered
# listing would have named them and its own listing does not.
STEER_DECOY = "zz-owning-tree-steer-decoy.txt"
STEER_PLANS_DECOY = ".plans/zz-owning-tree-steer-decoy.md"

# ⚠ Raising this is the claim that the arms you added actually RUN.
EXPECTED_ARMS = 54

# A `gitdir:` value that is a Windows drive path (`C:/...` or `C:\...`).
_WINDOWS_DRIVE_PATH = re.compile(r"^[A-Za-z]:[\\/]")


class Refusal(Exception):
    """No DSS tree contains the path, or git answers for a tree other than the one that does."""


class UsageRefusal(Refusal):
    """A call that names NOTHING -- an empty path, an empty argv, no identity -- refused
    before it can answer for whatever the caller happens to stand in."""


def _require_path(path, what):
    """`path` as a string, or `UsageRefusal` when it is None, empty or only whitespace."""
    if path is None or not str(os.fspath(path)).strip():
        raise UsageRefusal("%s needs a path -- an empty one would answer for whatever the caller "
                           "is standing in (realpath('') is the working directory, git -C '' "
                           "stays in it)" % what)
    return os.fspath(path)


def same_path(a, b):
    """True when two spellings name one file or directory -- case, separators and links folded.

    Two paths that both EXIST are compared by identity (`os.path.samefile`), which also
    folds case on a case-insensitive macOS volume, where `normcase` does not; otherwise
    their normalized real paths are compared.
    """
    try:
        if os.path.exists(a) and os.path.exists(b):
            return os.path.samefile(a, b)
    except OSError:
        pass
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def is_within(child, parent, strict=True):
    """True when `child` sits inside `parent` (by filesystem identity, through links and case).

    `strict=True` excludes `child == parent`. Every ancestor of `child`'s real path is
    compared with `same_path`, so a spelling difference cannot fake or hide containment.
    """
    c = os.path.realpath(child)
    if not strict and same_path(c, parent):
        return True
    d = os.path.dirname(c)
    while True:
        if same_path(d, parent):
            return True
        up = os.path.dirname(d)
        if up == d:
            return False
        d = up


def owning_tree(path):
    """The nearest ancestor of `path` (a directory counts as its own ancestor) holding every MARKER."""
    path = _require_path(path, "owning_tree")
    start = os.path.realpath(path)
    if not os.path.lexists(start):
        raise Refusal("no such path: %s -- the walk starts from a file or directory that exists, "
                      "never from where a missing one would have been" % path)
    d = start if os.path.isdir(start) else os.path.dirname(start)
    while True:
        if all(os.path.isdir(os.path.join(d, m)) for m in MARKERS):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            raise Refusal("no DSS tree contains %s -- no ancestor holds BOTH %s"
                          % (start, " and ".join(m + "/" for m in MARKERS)))
        d = parent


def layout_tree(path):
    """The tree `path` sits in BY LAYOUT, or None: the directory above the LAST
    `.harness-config/runner/actions` run of segments, when an action directory and a file
    follow it.

    Deliberately NOT `owning_tree`: this is the ORACLE `root_arms` judges a resolver
    against, and an oracle derived through the walk it pins would agree with any walk.
    Segments compare through `normcase`, so a case-folding filesystem has one answer.
    """
    parts = []
    head = os.path.realpath(path)
    while True:
        head, tail = os.path.split(head)
        if not tail:
            parts.append(head)          # the drive or the filesystem root
            break
        parts.append(tail)
    parts.reverse()
    want = tuple(os.path.normcase(s) for s in ACTIONS_SEGMENTS)
    n = len(want)
    for i in range(len(parts) - n - 2, 0, -1):
        if tuple(os.path.normcase(p) for p in parts[i:i + n]) == want:
            return os.path.join(*parts[:i])
    return None


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
                               stdin=subprocess.DEVNULL, text=True, encoding="utf-8",
                               errors="replace")
        except OSError as exc:
            raise Refusal("cannot run git (%s)" % exc)
        names = p.stdout.split() if p.returncode == 0 else []
        if "GIT_DIR" not in names:
            raise Refusal("`git rev-parse --local-env-vars` did not name GIT_DIR (rc=%d), so a "
                          "git call here cannot be kept from the caller's git environment"
                          % p.returncode)
        _LOCAL_GIT_ENV_NAMES = frozenset(names)
    return _LOCAL_GIT_ENV_NAMES


def _strip_git(env):
    names = local_git_env_names()
    return {k: v for k, v in env.items() if k not in names}


def git_environment(extra=None):
    """This process's environment WITHOUT anything that selects a git repository, plus `extra`."""
    env = _strip_git(os.environ)
    env.update(extra or {})
    return env


def _no_inherited_stdin(kwargs):
    """A child reads no stdin it did not ask for (unless the caller passes `input`/`stdin`)."""
    if "input" not in kwargs and "stdin" not in kwargs:
        kwargs["stdin"] = subprocess.DEVNULL
    return kwargs


def run_git(args, cwd=None, **kwargs):
    """`git <args>` in `cwd`, in `git_environment()` -- the ONE way git is asked about a tree here."""
    return subprocess.run(["git"] + list(args), cwd=cwd, env=git_environment(),
                          **_no_inherited_stdin(kwargs))


def run_unsteered(argv, env=None, **kwargs):
    """Run ANY program (gh, a consumer, a child interpreter) with git's repository-selecting
    names removed from its environment (`env`, else this process's).

    Refuses an empty argv or an empty program: `subprocess.run([])` would raise a bare
    OSError on one host and something else on another, and the retired shell owner's
    `exec` of nothing reported SUCCESS.
    """
    argv = list(argv or [])
    if not argv or not str(argv[0]).strip():
        raise UsageRefusal("run_unsteered needs a command -- an empty argv would run nothing and "
                           "could read as success")
    return subprocess.run(argv, env=_strip_git(os.environ if env is None else env),
                          **_no_inherited_stdin(kwargs))


def bare_git(args, cwd, env):
    """`git <args>` as the DEFECT ran it: with `env` exported over the environment.

    For arms only -- the negative an arm must prove real before it trusts its own verdict.
    """
    return subprocess.run(["git"] + list(args), cwd=cwd, env=git_environment(env),
                          stdin=subprocess.DEVNULL, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


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


# ── a tree's git identity, and git asked THROUGH it ──────────────────────────────────

# ⚠ A NAMEDTUPLE, NOT A DATACLASS, AND THE REASON IS HOW THIS FILE IS LOADED: every consumer
# loads it with `spec_from_file_location` + `exec_module` and never registers it in
# `sys.modules`, and `@dataclasses.dataclass` looks its own module up THERE -- ✔MEASURED
# 2026-09-21 (Python 3.14.3): "AttributeError: 'NoneType' object has no attribute '__dict__'"
# at import, in every consumer at once.
TreeIdentity = collections.namedtuple("TreeIdentity", ["root", "git_dir", "sha", "branch"])
TreeIdentity.__doc__ = (
    "Which git answers for a working tree: `root` (real path), `git_dir` (None when a plain "
    "`git -C root` answers), HEAD `sha`, and `branch` (the literal `HEAD` when detached).")


def _first_line(text):
    return (text or "").strip().split("\n")[0][:160]


def _git_answer(prefix, args, cwd=None):
    """(stdout stripped, "") when `git <prefix> <args>` succeeds, else (None, why)."""
    try:
        p = run_git(list(prefix) + list(args), cwd=cwd, capture_output=True, text=True,
                    encoding="utf-8", errors="replace")
    except OSError as exc:
        return None, "cannot run git (%s)" % exc
    if p.returncode != 0 or not p.stdout.strip():
        return None, _first_line(p.stderr) or ("git exited %d" % p.returncode)
    return p.stdout.strip(), ""


def _gitdir_value(dotgit):
    """The `gitdir:` value of a `.git` FILE (the first such line, CR and blanks stripped), or None."""
    try:
        with open(dotgit, "rb") as fh:
            for raw in fh.read().decode("utf-8", "surrogateescape").split("\n"):
                line = raw.strip()
                if line.startswith("gitdir:"):
                    value = line[len("gitdir:"):].strip()
                    return value or None
    except OSError:
        return None
    return None


def _wslpath_translate(tool, raw):
    p = subprocess.run([tool, "-u", raw], capture_output=True, text=True, encoding="utf-8",
                       errors="replace", stdin=subprocess.DEVNULL)
    return p.stdout.strip() if p.returncode == 0 else ""


def gitdir_candidate(root, raw, host_is_windows=None, which=shutil.which,
                     translate=_wslpath_translate):
    """Where the `gitdir:` value `raw` of the worktree at `root` points, in THIS host's namespace.

    ★ HOST-AWARE, never shape-only: a Windows drive path is native on Windows and FOREIGN on a
    POSIX host (translated with `wslpath -u` when it exists, refused by name when it does
    not); a POSIX absolute path is native on POSIX and refused by name on Windows; anything
    else is RELATIVE and joined to the WORKTREE -- never tested against the process's cwd,
    where a directory of the same relative name would be accepted by accident.
    """
    if host_is_windows is None:
        host_is_windows = os.name == "nt"
    if _WINDOWS_DRIVE_PATH.match(raw):
        if host_is_windows:
            return os.path.normpath(raw)
        tool = which("wslpath")
        if not tool:
            raise Refusal("%s is a worktree whose gitdir %r is a Windows drive path, absolute in "
                          "another namespace, and no wslpath is available on this host to "
                          "translate it" % (root, raw))
        got = translate(tool, raw)
        if not got:
            raise Refusal("wslpath could not translate the gitdir %r named by %s"
                          % (raw, os.path.join(root, ".git")))
        return got
    if raw.startswith("/") and not raw.startswith("//"):
        if host_is_windows:
            raise Refusal("%s is a worktree whose gitdir %r is a POSIX absolute path, which this "
                          "Windows host cannot follow" % (root, raw))
        return raw
    if raw.startswith("//") or raw.startswith("\\\\"):
        if host_is_windows:
            return raw
        raise Refusal("%s is a worktree whose gitdir %r is a UNC path, which this POSIX host "
                      "cannot follow" % (root, raw))
    return os.path.normpath(os.path.join(root, raw))


def identity(tree):
    """The git identity of the working tree AT `tree` (see `TreeIdentity`). Raises Refusal."""
    tree = _require_path(tree, "identity")
    root = os.path.realpath(tree)
    if not os.path.isdir(root):
        raise Refusal("no such directory: %s" % root)
    sha, why = _git_answer(["-C", root], ["rev-parse", "HEAD"])
    if sha:
        branch, _ = _git_answer(["-C", root], ["rev-parse", "--abbrev-ref", "HEAD"])
        return TreeIdentity(root, None, sha, branch or "HEAD")
    dotgit = os.path.join(root, ".git")
    if not os.path.isfile(dotgit):
        raise Refusal("not a git work tree with a resolvable HEAD: %s (%s)" % (root, why))
    raw = _gitdir_value(dotgit)
    if raw is None:
        raise Refusal("%s is a file but names no gitdir: %s" % (dotgit, root))
    gd = gitdir_candidate(root, raw)
    if not os.path.isdir(gd):
        raise Refusal("%s names a gitdir this namespace cannot reach: %s" % (dotgit, raw))
    prefix = ["--git-dir=" + gd, "--work-tree=" + root]
    sha, why = _git_answer(prefix, ["rev-parse", "HEAD"], cwd=root)
    if not sha:
        raise Refusal("resolved the gitdir to %s but git still cannot describe %s (%s)"
                      % (gd, root, why))
    branch, _ = _git_answer(prefix, ["rev-parse", "--abbrev-ref", "HEAD"], cwd=root)
    return TreeIdentity(root, gd, sha, branch or "HEAD")


def _require_identity(ident, what):
    if ident is None or not getattr(ident, "root", None):
        raise UsageRefusal("%s needs a tree identity -- without one git would answer for whatever "
                           "repository the caller is standing in" % what)


def tree_git_argv(ident, args):
    """-> (argv, cwd): the full `git …` command that asks through `ident`, and the directory to
    run it in (None when `-C` carries it). For a caller that must spawn git ITSELF -- to
    register the child with its own watchdog -- without re-deriving how an identity is asked;
    run it with `env=git_environment()`."""
    _require_identity(ident, "tree_git_argv")
    if ident.git_dir:
        return (["git", "--git-dir=" + ident.git_dir, "--work-tree=" + ident.root] + list(args),
                ident.root)
    return ["git", "-C", ident.root] + list(args), None


def tree_git(ident, args, **kwargs):
    """`git <args>` asked THROUGH `ident` (`-C root`, or `--git-dir/--work-tree` with cwd=root).

    Output is captured as BYTES unless the caller says otherwise; the exit code is the
    caller's to judge -- `tree_git_lines` is the form that refuses an unexpected one.
    """
    argv, cwd = tree_git_argv(ident, args)
    if cwd is not None:
        kwargs.setdefault("cwd", cwd)
    if "stdout" not in kwargs and "stderr" not in kwargs:
        kwargs.setdefault("capture_output", True)
    return run_git(argv[1:], **kwargs)


def tree_git_lines(ident, args, ok_codes=(0,)):
    """The non-empty output lines of `git <args>` through `ident`; `Refusal` on an exit code
    outside `ok_codes` (e.g. `(0, 1)` for `git grep`, whose 1 means "no match"), naming git's
    own first stderr line -- a git that cannot run is never read as "nothing found"."""
    p = tree_git(ident, args)
    if p.returncode not in ok_codes:
        err = _first_line((p.stderr or b"").decode("utf-8", "replace"))
        raise Refusal("git %s exited %d%s -- git cannot answer for %s, so nothing it printed is a "
                      "verdict" % (" ".join(args), p.returncode, (": " + err) if err else "",
                                   ident.root))
    out = (p.stdout or b"").decode("utf-8", "surrogateescape")
    return [line[:-1] if line.endswith("\r") else line
            for line in out.split("\n") if line.strip()]


def owning_root(path):
    """The git working tree G that contains `path` -- refused when a DSS tree A sits nested
    INSIDE G without being its root (git answers for the enclosing checkout there).

    G comes from git (`--show-toplevel`), else from walking up to the first directory whose
    `.git` `identity()` can follow (a Windows-created worktree read from WSL). A = the DSS
    tree `owning_tree` names, when there is one. No G → refused; no A → G; G is A or sits
    inside A → G; A inside G → refused.
    """
    path = _require_path(path, "owning_root")
    p = os.path.realpath(path)
    if not os.path.lexists(p):
        raise Refusal("no such path: %s" % path)
    d = p if os.path.isdir(p) else os.path.dirname(p)
    top, why = git_top_level(d)
    g = os.path.realpath(top) if top is not None else None
    if g is None:
        walk = d
        while True:
            if os.path.lexists(os.path.join(walk, ".git")):
                try:
                    identity(walk)
                    g = walk
                    break
                except Refusal:
                    pass
            up = os.path.dirname(walk)
            if up == walk:
                break
            walk = up
    if g is None:
        raise Refusal("no git working tree contains %s (%s)" % (path, why))
    try:
        a = owning_tree(d)
    except Refusal:
        return g
    if same_path(g, a) or is_within(g, a):
        return g
    raise Refusal("'%s' is a DSS tree nested inside the checkout '%s', which it is not the root "
                  "of. git answers for the enclosing checkout there -- it does not answer for the "
                  "DSS tree itself -- so no owning root is named." % (a, g))


def resolve_path(ident, rel):
    """`rel` read against `ident`'s ROOT (absolute paths unchanged) -- never the process cwd."""
    _require_identity(ident, "resolve_path")
    if rel is None or not str(rel).strip():
        raise UsageRefusal("resolve_path needs a path -- an empty one names the root itself only "
                           "by accident")
    rel = os.fspath(rel)
    if os.path.isabs(rel):
        return rel
    return os.path.join(ident.root, *[s for s in rel.replace("\\", "/").split("/") if s])


def assert_one_root(ident, check_cwd=False):
    """Refuse unless git's top level (asked through `ident`) is `ident.root` -- and, with
    `check_cwd`, unless this process stands there too. The enumeration root and the read
    root must be the same root."""
    _require_identity(ident, "assert_one_root")
    reasons = []
    p = tree_git(ident, ["rev-parse", "--show-toplevel"])
    top = (p.stdout or b"").decode("utf-8", "replace").strip() if p.returncode == 0 else None
    if not top:
        reasons.append("git names no top level for it (%s)"
                       % (_first_line((p.stderr or b"").decode("utf-8", "replace"))
                          or "exit %d" % p.returncode))
    elif not same_path(top, ident.root):
        reasons.append("git enumerates from '%s'" % top)
    if check_cwd and not same_path(os.getcwd(), ident.root):
        reasons.append("this process reads at '%s'" % os.getcwd())
    if reasons:
        raise Refusal("the enumeration root and the read root are NOT the same root. Expected "
                      "'%s', but %s. Refusing to report a verdict about a tree this process "
                      "cannot agree on." % (ident.root, "; ".join(reasons)))


def enter(tree):
    """`identity(tree)`, then change into its root and prove the two roots agree. Returns it."""
    ident = identity(tree)
    os.chdir(ident.root)
    assert_one_root(ident, check_cwd=True)
    return ident


# ── the tree's own configuration (JSONC) ─────────────────────────────────────────────

def strip_jsonc(text, source="<text>"):
    """JSONC as strict JSON: `//` and `/* */` comments and trailing commas removed, strings kept.

    `.harness-config/config.json` is JSONC because DssHarness accepts comments and trailing
    commas (`help config`: "the file is meant to be edited"). Both removals track string
    state, so a `//` inside a description or a URL is content, never a comment. ONE reader
    for every program in this tree (it was private to `check-scripts-index` until
    2026-09-21, lane mig, when `lane-worktree` needed the same file): a second copy would be
    a second definition of what the tool accepts.
    """
    out, i, n, in_str = [], 0, len(text), False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
        elif c == "/" and text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        elif c == "/" and text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                raise Refusal("%s has an unterminated /* comment" % source)
            i = j + 2
            continue
        out.append(c)
        i += 1
    s = "".join(out)
    out, i, n, in_str = [], 0, len(s), False
    while i < n:
        c = s[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(s[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
        elif c == ",":
            j = i + 1
            while j < n and s[j] in " \t\r\n":
                j += 1
            if j < n and s[j] in "}]":
                i += 1
                continue
        out.append(c)
        i += 1
    return "".join(out)


def load_jsonc(path):
    """The parsed content of the JSONC file at `path`; `Refusal` naming the file when it cannot
    be read or does not parse -- a configuration that cannot be read is never a default."""
    path = _require_path(path, "load_jsonc")
    try:
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
    except OSError as exc:
        raise Refusal("cannot read %s (%s)" % (path, exc))
    try:
        return json.loads(strip_jsonc(text, path))
    except ValueError as exc:
        raise Refusal("%s is not valid JSONC: %s" % (path, exc))


def probe_child(argv, cwd=None, env=None, timeout=120):
    """Run a child and ALWAYS return one line describing it -- never raise: an arm that asks
    what a child did must get an answer it can compare, not a traceback."""
    try:
        p = subprocess.run(list(argv), cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                           capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "CHILD-EXIT-TIMEOUT(%ss)" % timeout
    except OSError as exc:
        return "CHILD-EXIT-UNLAUNCHED: %s" % exc
    return "CHILD-EXIT-%d: stdout='%s' stderr='%s'" % (
        p.returncode, (p.stdout or b"").decode("utf-8", "replace").strip(),
        (p.stderr or b"").decode("utf-8", "replace").strip())


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


def fence(box, tree=None):
    """Refuse a scratch `box` inside the DSS tree `tree` (default: the one this file lives in)
    or inside ANY git repository (proven by a bare `rev-parse`, which is exactly what a
    fixture's own git would find)."""
    if tree is None:
        try:
            tree = owning_tree(__file__)
        except Refusal:
            tree = None
    if tree is not None and (same_path(box, tree) or is_within(box, tree)):
        raise Refusal("the scratch box %s is inside the DSS tree %s -- fixtures are never built in "
                      "the tree they test" % (box, tree))
    top = _bare_top_level(box)
    if top is not None:
        raise Refusal("the scratch box %s is inside the git repository %s -- a fixture's git would "
                      "answer for that repository instead" % (box, top))


class sandbox:
    """`with sandbox() as sb:` -- a fenced scratch box (`sb.box`) outside the tree and outside
    every git repository, with `sb.env` isolating git's configuration (a global and a system
    config that do not exist); removed on exit, and only ever under the temp root."""

    def __init__(self, prefix="owning-tree-box-"):
        self.prefix = prefix
        self.box = None
        self.env = {}

    def __enter__(self):
        box = os.path.realpath(tempfile.mkdtemp(prefix=self.prefix))
        try:
            fence(box)
        except BaseException:
            remove_tree(box)
            raise
        self.box = box
        missing = os.path.join(box, "no-such-gitconfig")
        self.env = {"GIT_CONFIG_GLOBAL": missing, "GIT_CONFIG_SYSTEM": missing,
                    "GIT_CONFIG_NOSYSTEM": "1"}
        return self

    def __exit__(self, *exc):
        if self.box and is_within(self.box, os.path.realpath(tempfile.gettempdir())):
            remove_tree(self.box)
        return False


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


_FIXTURE_GIT = ["-c", "user.email=owning-tree@example.invalid", "-c", "user.name=owning-tree",
                "-c", "commit.gpgsign=false", "-c", "core.autocrlf=false", "-c", "core.hooksPath="]


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
                 ["-C", decoy] + _FIXTURE_GIT + ["commit", "-q", "--no-verify", "-m", "decoy"]):
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
        # The consumer loads its owner as a SIBLING of its own directory, so the owner's copy
        # goes beside the consumer's copy -- derived from where the copy landed, never spelled.
        odest = os.path.join(os.path.dirname(os.path.dirname(dest)), "owning-tree",
                             "owning-tree.py")
        os.makedirs(os.path.dirname(odest), exist_ok=True)
        shutil.copyfile(owner, odest)
    run_git(["init", "-q", outer], capture_output=True)
    top, _why = git_top_level(copy)
    real = top is not None and same_path(top, outer)
    try:
        p = subprocess.run([sys.executable, "-c", _NESTED_DRIVER, dest, name], cwd=copy,
                           env=git_environment(), stdin=subprocess.DEVNULL, capture_output=True,
                           text=True, encoding="utf-8", errors="replace", timeout=180)
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
    ⚠ THE ORACLE IS THE FILE'S LAYOUT POSITION,
    `<tree>/.harness-config/runner/actions/[<group>/...]<name>/<file>` -- NOT this module's
    walk. A pin that derived its expectation through the helper it pins would pass whatever
    the helper said. The tree is the directory above the LAST `.harness-config/runner/actions`
    run of segments in the file's path, so an action grouped at any depth has one answer.
    ★ EVERY FOREIGN CONDITION IS PROVEN REAL BEFORE IT IS TRUSTED: a bare `rev-parse`
    must name the other repository from inside it, must fail from the non-repository
    directory, must follow the steering environment, and must name the enclosing checkout
    for the nested copy. An arm whose negative did not materialise reports that, instead of
    passing vacuously.
    """
    me = os.path.realpath(script_file)
    oracle = layout_tree(me)
    if oracle is None:
        return [(False, "root arms: the subject sits at "
                        "<tree>/.harness-config/runner/actions/<name>/<file>",
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
                       stdin=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace")
    top = p.stdout.strip() if p.returncode == 0 else None
    if top is None or not same_path(top, root):
        raise Refusal("git does not answer for the tree this script lives in (asked in the "
                      "caller's environment)")
    return root


def _selftest_cwd_keyed():
    """The defect itself: a bare `git rev-parse --show-toplevel` from wherever the process stands."""
    p = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True,
                       stdin=subprocess.DEVNULL, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise Refusal("not inside a git repository")
    return p.stdout.strip()


def _selftest_relative():
    """A RELATIVE answer: `.` names wherever it was asked."""
    return "."


def _touch(path, text=""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    return path


def _ancestors(path):
    d = os.path.realpath(path)
    while True:
        yield d
        parent = os.path.dirname(d)
        if parent == d:
            return
        d = parent


def _raises(fn, exc_type):
    """(True, message) when `fn()` raises `exc_type`, else (False, what it did instead)."""
    try:
        got = fn()
    except exc_type as exc:
        return True, str(exc)
    except Exception as exc:  # noqa: BLE001 - the arm reports the WRONG exception by name
        return False, "raised %s: %s" % (type(exc).__name__, exc)
    return False, "returned %r" % (got,)


def _commit_repo(path, files):
    """`git init` + commit `files` ({rel: text}) at `path`. Returns the HEAD sha."""
    for rel, text in files.items():
        _touch(os.path.join(path, *rel.split("/")), text)
    run_git(["init", "-q", path], capture_output=True)
    run_git(["-C", path, "add", "-A"], capture_output=True)
    run_git(["-C", path] + _FIXTURE_GIT + ["commit", "-q", "--no-verify", "-m", "fixture"],
            capture_output=True)
    p = run_git(["-C", path, "rev-parse", "HEAD"], capture_output=True, text=True,
                encoding="utf-8", errors="replace")
    return p.stdout.strip()


# The two generated consumers of the C arms: one asks through this file, one asks bare git.
_HELPER_CONSUMER = r'''
import importlib.util, os, sys
here = os.path.dirname(os.path.realpath(__file__))
spec = importlib.util.spec_from_file_location("ot", os.path.join(here, "owning-tree.py"))
ot = importlib.util.module_from_spec(spec); spec.loader.exec_module(ot)
try:
    ident = ot.identity(sys.argv[1])
    loose = ot.tree_git_lines(ident, ["ls-files", "--others", "--exclude-standard"])
except ot.Refusal as exc:
    print("CANNOT RUN: %s" % exc); sys.exit(2)
print("LOOSE: " + " ".join(loose) if loose else "no loose files")
sys.exit(1 if loose else 0)
'''
_PLAIN_CONSUMER = r'''
import subprocess, sys
p = subprocess.run(["git", "-C", sys.argv[1], "ls-files", "--others", "--exclude-standard"],
                   capture_output=True, text=True, stdin=subprocess.DEVNULL)
loose = [l for l in p.stdout.split("\n") if l.strip()]
print("LOOSE: " + " ".join(loose) if loose else "no loose files")
sys.exit(1 if loose else 0)
'''

# A child that reports the three steering names it can see, then exits 7 (the U arms).
_ENV_REPORTER = ("import os,sys; print('|'.join(os.environ.get(k,'unset') for k in "
                 "('GIT_DIR','GIT_WORK_TREE','GIT_INDEX_FILE'))); sys.exit(7)")


def self_test():
    """Red-on-disable for the owner: the walk, git's agreement, the unsteered git and programs,
    the identity and its three cases, git through an identity, the one-root rule, the owning
    root, the probe and the fence -- and the arms' ability to FAIL. Every arm runs even when
    another raised: an exception fails that arm only, by name."""
    ran, failed = [], []

    def pin(label, fn):
        try:
            res = fn()
            ok, detail = (res if isinstance(res, tuple) else (bool(res), ""))
        except Exception as exc:  # noqa: BLE001 - an arm that crashes FAILS, it never aborts the rest
            ok, detail = False, "CRASHED %s: %s" % (type(exc).__name__, exc)
        ran.append(label)
        if not ok:
            failed.append(label)
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", label,
                               ("   [" + str(detail) + "]") if detail and not ok else ""))

    def verdicts(resolver):
        return [a[0] for a in root_arms(resolver, (Refusal,), True, __file__)]

    held_cwd = os.getcwd()
    box = os.path.realpath(tempfile.mkdtemp(prefix="owning-tree-selftest-"))
    try:
        # Every fixture below is judged by what git finds from it, so the box must sit outside
        # this tree and outside every repository -- proven, not assumed (a refusal here names
        # the host, and no arm runs over a fixture git would answer for wrongly).
        fence(box)
        probe_rel = os.path.join(*(ACTIONS_SEGMENTS + ("probe", "probe.py")))
        tree = os.path.join(box, "tree")
        probe = _touch(os.path.join(tree, probe_rel))
        os.makedirs(os.path.join(tree, ".plans"))

        # ── W: the walk ──────────────────────────────────────────────────────────
        pin("W1 the walk names the directory holding BOTH .plans/ and .harness-config/",
            lambda: same_path(owning_tree(probe), tree))

        half = _touch(os.path.join(tree, "half", "sub", "x.py"))
        os.makedirs(os.path.join(tree, "half", ".plans"))
        pin("W2 .plans/ WITHOUT .harness-config/ is not a tree -- the walk continues to the real one",
            lambda: same_path(owning_tree(half), tree))

        cfg = _touch(os.path.join(tree, "cfg", "sub", "y.py"))
        os.makedirs(os.path.join(tree, "cfg", ".harness-config"))
        pin("W3 .harness-config/ WITHOUT .plans/ is not a tree -- the walk continues to the real one",
            lambda: same_path(owning_tree(cfg), tree))

        copy = os.path.join(tree, ".temp", "copy")
        cprobe = _touch(os.path.join(copy, probe_rel))
        os.makedirs(os.path.join(copy, ".plans"))
        pin("W4 a tree copied INSIDE another tree is its own tree -- the NEAREST one wins",
            lambda: same_path(owning_tree(cprobe), copy))

        def w5():
            lone = os.path.join(box, "lone", "deep")
            os.makedirs(lone)
            clean = not any(all(os.path.isdir(os.path.join(a, m)) for m in MARKERS)
                            for a in _ancestors(lone))
            ok, msg = _raises(lambda: owning_tree(lone), Refusal)
            return (clean and ok and "no DSS tree contains" in msg,
                    "negative-synthesized=%s msg=%r" % (clean, msg[:80]))
        pin("W5 a path under NO tree is REFUSED, naming what the walk looked for", w5)

        def w6():
            outs = []
            for bad in ("", "   ", None):
                ok, msg = _raises(lambda b=bad: owning_tree(b), UsageRefusal)
                outs.append(ok)
            missing = os.path.join(tree, "no-such-file.py")
            ok, msg = _raises(lambda: owning_tree(missing), Refusal)
            # the negative this closes: realpath('') IS the cwd, so an empty path would answer
            real = same_path(os.path.realpath(""), os.getcwd())
            return (all(outs) and ok and "no such path" in msg and real,
                    "empty/blank/None refused=%s missing=%r negative-synthesized=%s"
                    % (outs, msg[:60], real))
        pin("W6 an EMPTY, blank or None path is a UsageRefusal and a MISSING path is refused -- "
            "never the caller's cwd", w6)

        # ── R: git's agreement ──────────────────────────────────────────────────
        tree_sha = _commit_repo(tree, {"witness.txt": "MAIN-ROOT\n"})

        def r1():
            try:
                return same_path(resolve(probe, reads_git=True), tree), ""
            except Refusal as exc:
                return False, _first_line(str(exc))
        pin("R1 reads_git: a tree that IS its own git top level resolves", r1)

        def r2():
            ok, why = _raises(lambda: resolve(cprobe, reads_git=True), Refusal)
            return ok and "does not answer" in why, "got=%r" % _first_line(why)
        pin("R2 reads_git: a tree nested inside another checkout is REFUSED -- git answers for "
            "the enclosing one", r2)

        # ── U: nothing git-steering reaches a child ────────────────────────────────
        steer = steering_env(box)

        def u1():
            listing = ["ls-files", "-z", "--cached", "--others", "--exclude-standard"]
            neg = bare_git(listing, tree, steer)
            real = STEER_DECOY in neg.stdout.split("\0")
            with caller_environment(steer):
                got = run_git(listing, cwd=tree, capture_output=True, text=True,
                              encoding="utf-8", errors="replace")
            listed = got.stdout.split("\0")
            return (real and got.returncode == 0 and STEER_DECOY not in listed
                    and "witness.txt" in listed,
                    "negative-synthesized=%s rc=%d decoy-listed=%s"
                    % (real, got.returncode, STEER_DECOY in listed))
        pin("U1 run_git IGNORES a caller's GIT_DIR + GIT_WORK_TREE + GIT_INDEX_FILE", u1)

        def u2():
            argv = [sys.executable, "-c", _ENV_REPORTER]
            with caller_environment(steer):
                neg = subprocess.run(argv, capture_output=True, text=True, env=dict(os.environ),
                                     stdin=subprocess.DEVNULL)
                got = run_unsteered(argv, capture_output=True, text=True)
                untouched = all(os.environ.get(k) == v for k, v in steer.items())
            real = "unset" not in neg.stdout
            return (real and got.returncode == 7 and got.stdout.strip() == "unset|unset|unset"
                    and untouched,
                    "negative-synthesized=%s got=%r rc=%d caller-env-untouched=%s"
                    % (real, got.stdout.strip(), got.returncode, untouched))
        pin("U2 run_unsteered: the child sees none of the three names, its exit code passes "
            "through, and the CALLER's environment is untouched", u2)

        def u3():
            a, am = _raises(lambda: run_unsteered([]), UsageRefusal)
            b, bm = _raises(lambda: run_unsteered([""]), UsageRefusal)
            return a and b, "empty=%r blank=%r" % (am[:50], bm[:50])
        pin("U3 run_unsteered REFUSES an empty argv and an empty program", u3)

        def u4():
            # ⚠ The negative passes the environment EXPLICITLY: on Windows, putting an empty
            # value into os.environ deletes the variable from the PROCESS block (CRT putenv
            # semantics), so only an explicit block can hand a child an empty-but-set name.
            argv = [sys.executable, "-c", _ENV_REPORTER]
            with caller_environment({"GIT_INDEX_FILE": ""}):
                neg = subprocess.run(argv, capture_output=True, text=True, env=dict(os.environ),
                                     stdin=subprocess.DEVNULL)
                got = run_unsteered(argv, capture_output=True, text=True)
            real = neg.stdout.strip().split("|")[-1] == ""
            return (real and got.stdout.strip().split("|")[-1] == "unset",
                    "negative-synthesized=%s got=%r" % (real, got.stdout.strip()))
        pin("U4 an EMPTY GIT_INDEX_FILE is removed too -- an empty value still steers git", u4)

        # ── A: root_arms can fail ──────────────────────────────────────────────
        def a1():
            arms = root_arms(_selftest_with_agreement, (Refusal,), True, __file__)
            return (len(arms) == 5 and all(a[0] for a in arms),
                    "; ".join("%s -> %s" % (a[1], a[2]) for a in arms if not a[0]))
        pin("A1 root_arms against THIS owner's resolver: control, another repository, no "
            "repository, a steering environment and a nested copy all hold", a1)

        def a2():
            v = verdicts(_selftest_cwd_keyed)
            return v == [True, False, False, False, False], "verdicts=%r" % v
        pin("A2 root_arms CAN FAIL: a cwd-keyed resolver passes only the CONTROL", a2)

        # ★ A RELATIVE answer is the cwd-keyed defect wearing a different spelling: `.` names
        # wherever it was asked. It passes the steering arm, which is asked from the tree itself.
        def a3():
            v = verdicts(_selftest_relative)
            return v == [True, False, False, True, False], "verdicts=%r" % v
        pin("A3 root_arms CAN FAIL on a RELATIVE answer: `.` passes only the arms asked from "
            "the tree itself", a3)

        def a4():
            v = verdicts(_selftest_without_agreement)
            return v == [True, True, True, True, False], "verdicts=%r" % v
        pin("A4 root_arms CAN FAIL: a resolver that skips git's agreement fails only the "
            "nested-copy arm", a4)

        def a5():
            v = verdicts(_selftest_agreement_in_callers_env)
            return v == [True, True, True, False, True], "verdicts=%r" % v
        pin("A5 root_arms CAN FAIL: git's agreement asked in the CALLER's environment fails "
            "only the steering arm", a5)

        # ── I: a tree's identity ───────────────────────────────────────────────
        def i1():
            ident = identity(tree)
            br = run_git(["-C", tree, "rev-parse", "--abbrev-ref", "HEAD"], capture_output=True,
                         text=True, encoding="utf-8").stdout.strip()
            return (ident.git_dir is None and ident.sha == tree_sha and ident.branch == br
                    and same_path(ident.root, tree), "ident=%r" % (ident,))
        pin("I1 identity of a plain checkout: git answers as it stands (no gitdir), HEAD sha and "
            "branch", i1)

        lane = os.path.join(box, "lane")
        run_git(["-C", tree, "worktree", "add", "-q", "--detach", lane], capture_output=True)
        _touch(os.path.join(lane, "witness.txt"), "LANE-ROOT\n")

        def i2():
            is_worktree = os.path.isfile(os.path.join(lane, ".git"))
            ident = identity(lane)
            return (is_worktree and ident.sha == tree_sha and ident.branch == "HEAD"
                    and same_path(ident.root, lane),
                    "lane/.git is a file=%s ident=%r" % (is_worktree, ident))
        pin("I2 identity of a detached worktree (a `.git` FILE): its HEAD, branch reported as HEAD",
            i2)

        def i3():
            root = os.path.join(box, "i3")
            a, am = _raises(lambda: gitdir_candidate(root, "C:/x/y", host_is_windows=False,
                                                     which=lambda _n: None), Refusal)
            got_b = gitdir_candidate(root, "C:/x/y", host_is_windows=False,
                                     which=lambda _n: "/usr/bin/wslpath",
                                     translate=lambda _t, raw: "/mnt/c/x/y")
            c, cm = _raises(lambda: gitdir_candidate(root, "/mnt/c/x", host_is_windows=True),
                            Refusal)
            got_d = gitdir_candidate(root, "C:/x/y", host_is_windows=True)
            # the negative this closes: a shape-only join names a path UNDER the worktree
            joined = os.path.normpath(os.path.join(root, "C:/x/y"))
            real = joined.startswith(os.path.normpath(root)) or os.name == "nt"
            return (a and "wslpath" in am and got_b == "/mnt/c/x/y" and c and "POSIX" in cm
                    and not is_within(got_d, root, strict=False) and real,
                    "posix-no-wslpath=%r translated=%r windows-posix=%r windows-native=%r"
                    % (am[:40], got_b, cm[:40], got_d))
        pin("I3 a FOREIGN absolute gitdir is judged by the HOST: translated with wslpath or "
            "refused by name, never joined under the worktree", i3)

        def i4():
            root = os.path.join(box, "i4", "tree")
            os.makedirs(root)
            decoy_cwd = os.path.join(box, "i4-decoy", "sub")
            os.makedirs(decoy_cwd)
            os.makedirs(os.path.join(box, "i4-decoy", "gd"))
            os.chdir(decoy_cwd)
            try:
                real = os.path.isdir(os.path.join("..", "gd"))
                got = gitdir_candidate(root, "../gd", host_is_windows=(os.name == "nt"))
            finally:
                os.chdir(held_cwd)
            want = os.path.normpath(os.path.join(root, "..", "gd"))
            return (real and got == want,
                    "negative-synthesized=%s got=%r want=%r" % (real, got, want))
        pin("I4 a RELATIVE gitdir resolves against the WORKTREE -- a decoy of the same name "
            "beside the process's cwd is not consulted", i4)

        def i5():
            plain = os.path.join(box, "i5-plain")
            os.makedirs(plain)
            ok, msg = _raises(lambda: identity(plain), Refusal)
            return ok and "not a git work tree" in msg, msg[:80]
        pin("I5 identity of a plain directory is REFUSED", i5)

        def i6():
            bad = os.path.join(box, "i6-bad")
            _touch(os.path.join(bad, ".git"), "gitdir: %s\n"
                   % os.path.join(box, "definitely-not-here").replace("\\", "/"))
            ok, msg = _raises(lambda: identity(bad), Refusal)
            return ok and "cannot reach" in msg, msg[:80]
        pin("I6 a `.git` file naming a gitdir that does not exist is REFUSED", i6)

        def i7():
            ok, msg = _raises(lambda: identity(""), UsageRefusal)
            return ok, msg[:60]
        pin("I7 identity('') is a UsageRefusal", i7)

        # ── G: git asked THROUGH an identity ────────────────────────────────────
        def g_arm(names):
            env = {k: steer[k] for k in names}
            listing = ["ls-files", "-z", "--cached", "--others", "--exclude-standard"]
            neg = bare_git(listing, tree, env)
            real = STEER_DECOY in neg.stdout.split("\0")
            ident = identity(tree)
            with caller_environment(env):
                got = tree_git_lines(ident, ["ls-files", "--cached", "--others",
                                             "--exclude-standard"])
            return (real and STEER_DECOY not in got and "witness.txt" in got,
                    "negative-synthesized=%s decoy-listed=%s" % (real, STEER_DECOY in got))
        pin("G1 tree_git ignores a GIT_INDEX_FILE naming another repository's index",
            lambda: g_arm(["GIT_INDEX_FILE"]))
        pin("G2 tree_git ignores a GIT_DIR naming another repository",
            lambda: g_arm(["GIT_DIR"]))

        def g3():
            ident = identity(tree)
            p = tree_git(ident, ["rev-parse", "--verify", "no-such-ref-anywhere"])
            captured = p.returncode != 0 and bool((p.stderr or b"").strip())
            ok, msg = _raises(lambda: tree_git_lines(ident, ["rev-parse", "--verify",
                                                             "no-such-ref-anywhere"]), Refusal)
            ok2 = tree_git_lines(ident, ["rev-parse", "--verify", "no-such-ref-anywhere"],
                                 ok_codes=(0, 128)) == []
            return captured and ok and ok2, "captured=%s refused=%r" % (captured, msg[:60])
        pin("G3 a failing git: its exit code and stderr come back, and tree_git_lines REFUSES "
            "it unless the caller declared that code acceptable", g3)

        def g4():
            a, _ = _raises(lambda: tree_git(None, ["status"]), UsageRefusal)
            b, _ = _raises(lambda: tree_git(TreeIdentity("", None, "", ""), ["status"]),
                           UsageRefusal)
            return a and b, "none=%s empty-root=%s" % (a, b)
        pin("G4 tree_git without an identity is a UsageRefusal", g4)

        # ── O: the enumeration root and the read root are one root ─────────────────
        def o1():
            ident = identity(lane)
            os.chdir(tree)
            try:
                with open(resolve_path(ident, "witness.txt"), encoding="utf-8") as fh:
                    text = fh.read().strip()
            finally:
                os.chdir(held_cwd)
            return text == "LANE-ROOT", "read %r" % text
        pin("O1 standing in ANOTHER tree, resolve_path reads the identity's bytes", o1)

        def o2():
            os.chdir(tree)
            try:
                with open("witness.txt", encoding="utf-8") as fh:
                    text = fh.read().strip()
            finally:
                os.chdir(held_cwd)
            return text == "MAIN-ROOT", "read %r" % text
        pin("O2 CONTROL: a bare relative read from there reads the OTHER tree's bytes", o2)

        def o3():
            ident = identity(lane)
            os.chdir(tree)
            try:
                ok, msg = _raises(lambda: assert_one_root(ident, check_cwd=True), Refusal)
            finally:
                os.chdir(held_cwd)
            return ok and "NOT the same root" in msg, msg[:80]
        pin("O3 assert_one_root(check_cwd) REFUSES a process standing in another tree", o3)

        def o4():
            try:
                ident = enter(lane)
                here = os.getcwd()
            finally:
                os.chdir(held_cwd)
            return same_path(here, lane) and same_path(ident.root, lane), "cwd=%s" % here
        pin("O4 enter(): identity, change into it, and the two roots agree", o4)

        def o5():
            ident = identity(lane)
            rel = resolve_path(ident, "a/b.txt")
            absolute = os.path.join(box, "elsewhere.txt")
            empty, _ = _raises(lambda: resolve_path(ident, ""), UsageRefusal)
            return (rel == os.path.join(ident.root, "a", "b.txt")
                    and resolve_path(ident, absolute) == absolute and empty,
                    "rel=%r" % rel)
        pin("O5 resolve_path: relative under the root, absolute unchanged, empty refused", o5)

        # ── T: which git working tree contains a path ────────────────────────────
        pin("T1 owning_root CONTROL: a DSS tree that is its own checkout",
            lambda: same_path(owning_root(probe), tree))

        foreign = os.path.join(box, "t-foreign")
        _commit_repo(foreign, {"f.txt": "foreign\n"})

        def t_steer(env, label_neg):
            # The negative is asked from the PROBE's directory: GIT_DIR alone makes git call
            # whatever directory it stands in its top level -- from the tree root that would
            # coincide with the right answer and prove nothing.
            neg = _bare_top_level(os.path.dirname(probe), env)
            real = neg is not None and not same_path(neg, tree)
            with caller_environment(env):
                got = owning_root(probe)
            return (real and same_path(got, tree),
                    "negative-synthesized=%s (%s) got=%s" % (real, label_neg, got))
        pin("T2 owning_root under a caller's GIT_DIR naming another repository -> still its own",
            lambda: t_steer({"GIT_DIR": os.path.join(foreign, ".git")}, "GIT_DIR"))
        pin("T3 owning_root under GIT_DIR + GIT_WORK_TREE naming another repository -> still its own",
            lambda: t_steer({"GIT_DIR": os.path.join(foreign, ".git"),
                             "GIT_WORK_TREE": foreign}, "GIT_DIR+GIT_WORK_TREE"))

        def t4():
            outer = os.path.join(box, "t-outer")
            _commit_repo(outer, {"o.txt": "outer\n"})
            ncopy = os.path.join(outer, ".temp", "copy")
            nprobe = _touch(os.path.join(ncopy, probe_rel))
            os.makedirs(os.path.join(ncopy, ".plans"))
            top, _ = git_top_level(ncopy)
            real = top is not None and same_path(top, outer)
            ok, msg = _raises(lambda: owning_root(nprobe), Refusal)
            return (real and ok and "nested inside the checkout" in msg,
                    "negative-synthesized=%s msg=%r" % (real, msg[:80]))
        pin("T4 an untracked DSS tree nested inside another checkout is REFUSED", t4)

        def t5():
            plain = os.path.join(box, "t-plain")
            _commit_repo(plain, {"p.txt": "p\n"})
            return same_path(owning_root(os.path.join(plain, "p.txt")), plain)
        pin("T5 a plain repository with no DSS tree around it is its own owning root", t5)

        def t6():
            inner = os.path.join(tree, ".temp", "inner")
            _commit_repo(inner, {"i.txt": "i\n"})
            return same_path(owning_root(os.path.join(inner, "i.txt")), inner)
        pin("T6 a repository INSIDE a DSS tree is its own owning root", t6)

        def t7():
            ok, msg = _raises(lambda: owning_root(""), UsageRefusal)
            return ok, msg[:60]
        pin("T7 owning_root('') is a UsageRefusal", t7)

        # ── C: a consumer that asks through this file vs one that asks bare git ────
        cons = os.path.join(box, "cons")
        _commit_repo(cons, {"README.md": "committed\n"})
        _touch(os.path.join(cons, "loose.txt"), "untracked\n")
        cdir = os.path.join(box, "consumers")
        os.makedirs(cdir)
        shutil.copyfile(os.path.realpath(__file__), os.path.join(cdir, "owning-tree.py"))
        helper = _touch(os.path.join(cdir, "helper_consumer.py"), _HELPER_CONSUMER)
        plain_c = _touch(os.path.join(cdir, "plain_consumer.py"), _PLAIN_CONSUMER)
        # The decoy's index TRACKS `loose.txt`, so a consumer steered at it reads the loose
        # file as committed; steered at its directory, it reads a tree with nothing loose.
        decoy_repo = os.path.join(box, "c-decoy")
        _commit_repo(decoy_repo, {"loose.txt": "decoy\n", "d.txt": "d\n"})

        def run_consumer(script, env, repo=cons):
            full = dict(os.environ)
            full.update(env)
            return subprocess.run([sys.executable, script, repo], capture_output=True,
                                  text=True, env=full, stdin=subprocess.DEVNULL, timeout=180)

        def c_pair(env, which):
            neg = bare_git(["ls-files", "--others", "--exclude-standard"], cons, env)
            real = "loose.txt" not in neg.stdout
            h = run_consumer(helper, env)
            p = run_consumer(plain_c, env)
            if which == "helper":
                return (real and h.returncode == 1 and "loose.txt" in h.stdout
                        and "README.md" not in h.stdout,
                        "negative-synthesized=%s rc=%d out=%r" % (real, h.returncode,
                                                                  h.stdout.strip()[:80]))
            return (real and not (p.returncode == 1 and "loose.txt" in p.stdout),
                    "negative-synthesized=%s plain rc=%d out=%r"
                    % (real, p.returncode, p.stdout.strip()[:80]))

        idx_env = {"GIT_INDEX_FILE": os.path.join(decoy_repo, ".git", "index")}
        dir_env = {"GIT_DIR": os.path.join(decoy_repo, ".git"), "GIT_WORK_TREE": decoy_repo}
        pin("C1 under a steering GIT_INDEX_FILE the helper consumer still names loose.txt",
            lambda: c_pair(idx_env, "helper"))
        pin("C2 NEGATIVE: under the same steering the plain consumer does NOT get it right",
            lambda: c_pair(idx_env, "plain"))
        pin("C3 under a steering GIT_DIR + GIT_WORK_TREE the helper consumer still names loose.txt",
            lambda: c_pair(dir_env, "helper"))
        pin("C4 NEGATIVE: under the same steering the plain consumer does NOT get it right",
            lambda: c_pair(dir_env, "plain"))

        corrupt = os.path.join(box, "corrupt")
        _commit_repo(corrupt, {"README.md": "committed\n"})
        with open(os.path.join(corrupt, ".git", "index"), "wb") as fh:
            fh.write(b"not an index")

        def c5():
            neg = bare_git(["status"], corrupt, {})
            real = neg.returncode != 0
            h = run_consumer(helper, {}, corrupt)
            return (real and h.returncode == 2 and "CANNOT RUN" in h.stdout,
                    "negative-synthesized=%s rc=%d out=%r" % (real, h.returncode,
                                                              h.stdout.strip()[:80]))
        pin("C5 a corrupt index: the helper consumer says CANNOT RUN (exit 2), never 'clean'", c5)

        def c6():
            p = run_consumer(plain_c, {}, corrupt)
            return (p.returncode == 0 and "no loose files" in p.stdout,
                    "plain rc=%d out=%r" % (p.returncode, p.stdout.strip()[:80]))
        pin("C6 NEGATIVE: the plain consumer reads the same corrupt index as 'no loose files'", c6)

        # ── P: a child probe always answers ─────────────────────────────────────
        pin("P1 probe_child reports a child's output",
            lambda: probe_child([sys.executable, "-c", "print('PROBE-ECHO')"])
            == "CHILD-EXIT-0: stdout='PROBE-ECHO' stderr=''")

        def p2():
            got = probe_child([sys.executable, "-c", "import sys; sys.exit(3)"])
            return got == "CHILD-EXIT-3: stdout='' stderr=''", "got=%r" % got
        pin("P2 a silent failing child is reported exactly, never raised", p2)

        # ── J: the tree's JSONC configuration ──────────────────────────────────
        def j1():
            text = ('{\n  // a comment\n  "url": "https://example.invalid/x", /* block */\n'
                    '  "q": "say \\"//not a comment\\"",\n  "list": [1, 2,],\n}\n')
            got = json.loads(strip_jsonc(text))
            return (got == {"url": "https://example.invalid/x", "q": 'say "//not a comment"',
                            "list": [1, 2]}, "got=%r" % (got,))
        pin("J1 strip_jsonc: comments and trailing commas go, `//` inside a string stays", j1)

        def j2():
            ok, msg = _raises(lambda: strip_jsonc('{"a": 1 /* never closed', "fixture.json"),
                              Refusal)
            return ok and "unterminated" in msg and "fixture.json" in msg, msg[:80]
        pin("J2 an unterminated /* comment is REFUSED naming its source", j2)

        def j3():
            bad = _touch(os.path.join(box, "j3", "bad.json"), '{"a": }\n')
            ok, msg = _raises(lambda: load_jsonc(bad), Refusal)
            missing, mm = _raises(lambda: load_jsonc(os.path.join(box, "j3", "none.json")),
                                  Refusal)
            return ok and "bad.json" in msg and missing, "%r / %r" % (msg[:60], mm[:60])
        pin("J3 load_jsonc REFUSES a file that does not parse or cannot be read, naming it", j3)

        def j4():
            cfg = load_jsonc(os.path.join(owning_tree(__file__), ".harness-config",
                                          "config.json"))
            return (isinstance(cfg, dict) and isinstance(cfg.get("worktrees"), dict)
                    and isinstance(cfg.get("legs"), dict), "keys=%r" % sorted(cfg)[:8])
        pin("J4 CONTROL: this tree's own config.json parses, with its worktrees and legs", j4)

        # ── X: the fence and the cleanup ──────────────────────────────────────
        def x2():
            inside, im = _raises(lambda: fence(os.path.join(tree, "half"), tree), Refusal)
            in_repo, rm = _raises(lambda: fence(os.path.join(foreign), None), Refusal)
            outside = os.path.join(box, "x2-outside")
            os.makedirs(outside)
            ok_out = True
            try:
                fence(outside, tree)
            except Refusal as exc:
                ok_out, rm = False, str(exc)
            return (inside and in_repo and ok_out,
                    "inside-tree=%s inside-repo=%s control-outside=%s %r"
                    % (inside, in_repo, ok_out, rm[:60]))
        pin("X2 the sandbox fence refuses a box inside the tree and inside a repository, and "
            "accepts one outside both (the control)", x2)
    except Exception as exc:  # noqa: BLE001 - a FIXTURE that crashes stops the arms after it
        failed.append("fixture construction")
        print("  FAIL fixture construction crashed after %d arm(s): %s: %s -- the arms after it "
              "did not run, and the count below says so" % (len(ran), type(exc).__name__, exc))
    finally:
        os.chdir(held_cwd)
        remove_tree(box)
    pin("X1 the self-test's temp trees were removed", lambda: not os.path.exists(box))

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
