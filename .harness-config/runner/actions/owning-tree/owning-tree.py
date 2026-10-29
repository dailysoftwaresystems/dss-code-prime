#!/usr/bin/env python3
# PURPOSE: name the DSS tree a script's own file lives in -- walked up from that file, never taken from the caller's working directory or git environment -- and what a walk of that tree leaves out: its own .gitignore, as git reads it.
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
    dssharness run owning-tree      # its one step: --self-test (and ctest's owning_tree_selftest_guard)
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
EXPECTED_ARMS = 97

# A `gitdir:` value that is a Windows drive path (`C:/...` or `C:\...`).
_WINDOWS_DRIVE_PATH = re.compile(r"^[A-Za-z]:[\\/]")


class Refusal(Exception):
    """No DSS tree contains the path, or git answers for a tree other than the one that does."""


class UsageRefusal(Refusal):
    """A call that names NOTHING -- an empty path, an empty argv, no identity -- refused
    before it can answer for whatever the caller happens to stand in; and a command line
    that names an OPTION where a path or a text belongs (`read_path`, `written_path`,
    `path_part`, `text`)."""


def _require_path(path, what):
    """`path` as a string, or `UsageRefusal` when it is None, empty or only whitespace."""
    if path is None or not str(os.fspath(path)).strip():
        raise UsageRefusal("%s needs a path -- an empty one would answer for whatever the caller "
                           "is standing in (realpath('') is the working directory, git -C '' "
                           "stays in it)" % what)
    return os.fspath(path)


# ── A PATH A COMMAND LINE NAMES ─────────────────────────────────────────────────────────────────────
# ★ NEVER ONE THAT BEGINS WITH `-`. A value flag reads the word after it as its value, so `--out --help`
# hands a program `--help` as the place to write -- a request for help become a file or a directory in
# the caller's working directory, or in the tree a program resolves its paths from. (2026-10-15: a file
# named `--help` appeared at the root of a checkout that way.) No path a command line names begins with
# `-`: one that does is an option taken for a value, and it is refused naming the flag, in either
# spelling (`--out x`, `--out=x`). A file truly so named is still reachable as `./-x`. An empty value
# names no place and is refused too.
# ★ THREE KINDS, ONE RULE: a path a program WRITES (`written_path`); a path it READS -- a file, a
# directory, the program a child starts (`read_path`); and a PART of one -- a name or a relative path it
# joins under a place it names itself (`path_part`), refused as well when it would name ANOTHER place:
# absolute, on a drive (`C:x`, on every host), or holding a `..` component.
# ★ WHO ASKS: a program that reads its command line BY HAND asks the kind's function for each such value;
# one that reads it with argparse gives EVERY argument that takes a value its kind -- one of these three,
# or one of the three below (A VALUE A COMMAND LINE NAMES THAT IS NO PATH) -- as `type=<kind>_arg(<its
# flag>)`, and `check-scripts-index` (its clause 14) refuses an argument without one. ARGPARSE ALONE IS NO
# GUARD: it refuses `--out --help` (a value it reads as an option) and takes `--out=--help` and `--out -`
# verbatim.
# ★ AN EMPTY VALUE THAT MEANS "NOT GIVEN" IS SAID SO: `empty_means_absent=True` keeps '' as '' and still
# refuses `-x`. argparse runs `type=` over a string DEFAULT too, and a step hands an empty input on as
# `--flag ''`: an argument whose '' means absent would otherwise be refused on every run that omits it.
def _named_path(value, flag, what):
    """`value` as `what` (a phrase: "the path to write") `flag` names, or `UsageRefusal` when it is empty or
    begins with `-`."""
    if value is None or not str(value).strip():
        raise UsageRefusal("%s names no %s" % (flag, what.split(" ", 1)[1]))
    value = str(value)
    if value.startswith("-"):
        raise UsageRefusal("%s takes '%s' as %s, and it begins with `-`: an option taken for a value. Name it "
                           "(`./%s` if that is truly its name)" % (flag, value, what, value))
    return value


def written_path(value, flag):
    """`value` as the path `flag` names for writing, or `UsageRefusal` when it is empty or begins with `-`."""
    return _named_path(value, flag, "the path to write")


def read_path(value, flag):
    """`value` as the path `flag` names for reading -- a file, a directory, a program to start -- or
    `UsageRefusal` when it is empty or begins with `-`."""
    return _named_path(value, flag, "the path to read")


_DRIVE_SPELLING = re.compile(r"^[A-Za-z]:")


def path_part(value, flag):
    """`value` as a part of a path -- a name, or a relative path, the program joins under a place it names --
    or `UsageRefusal` when it is empty, begins with `-`, or names another place: absolute, on a drive (on every
    host: a part is joined the same way wherever it runs), or holding a `..` component."""
    value = _named_path(value, flag, "a part of a path")
    if value[0] in "/\\" or _DRIVE_SPELLING.match(value) or os.path.isabs(value):
        raise UsageRefusal("%s takes '%s' as a part of a path, and it is absolute: a part is joined under the "
                           "place the program names, and this one would replace it" % (flag, value))
    if ".." in re.split(r"[\\/]", value):
        raise UsageRefusal("%s takes '%s' as a part of a path, and it holds `..`: it would leave the place it "
                           "is joined under" % (flag, value))
    return value


def _path_arg(kind, flag, name, empty_means_absent):
    """argparse's `type=` asking `kind(value, flag)`, its refusal raised as `argparse.ArgumentTypeError`, which
    argparse prints as its usage error, naming the option, and exits 2."""
    import argparse

    def path_type(value):
        if empty_means_absent and value == "":
            return value
        try:
            return kind(value, flag)
        except UsageRefusal as exc:
            raise argparse.ArgumentTypeError(str(exc))
    path_type.__name__ = name   # argparse names a type by it in an error it makes itself
    return path_type


def written_path_arg(flag, empty_means_absent=False):
    """argparse's `type=` for a path a program writes (`written_path`)."""
    return _path_arg(written_path, flag, "path to write", empty_means_absent)


def read_path_arg(flag, empty_means_absent=False):
    """argparse's `type=` for a path a program reads (`read_path`)."""
    return _path_arg(read_path, flag, "path to read", empty_means_absent)


def path_part_arg(flag, empty_means_absent=False):
    """argparse's `type=` for a part of a path a program joins under a place it names (`path_part`)."""
    return _path_arg(path_part, flag, "part of a path", empty_means_absent)


# ── A VALUE A COMMAND LINE NAMES THAT IS NO PATH ────────────────────────────────────────────────────
# ★ EVERY VALUE HAS A KIND. An argument that takes a value names a path (the three kinds above), a number
# (`type=int` / `float`) or one word of a closed `choices` set -- both of which argparse already holds to
# values no option can be -- or one of these three, and it SAYS which (clause 14 of check-scripts-index
# refuses one that says none, and cross-checks what it says against where the value goes).
# ★ TEXT (`text`): a value that names no path -- a label, a leg, a key, a hash, a list of names. It never
# begins with `-` either: `--label=--help` is the same option taken for a value. An EMPTY one passes -- it
# is no option, and argparse runs `type=` over a string default. A program's own conversion (a number at
# least 1, a list of bands) is handed as `then`, and asked only AFTER the refusal.
# ★ OPTION TEXT (`option_text`): a text value that may BEGIN with `-` by its nature -- a pattern looked for in a
# log (`--match=-O2`), URL-safe base64 (its alphabet holds `-`). And COMMAND WORDS (`command_words`): a word
# of a CHILD'S command line or of its options (`--launcher=qemu-s390x`, `--flags=-std=c2x`), which the child
# judges.
# Both take a leading `-` on purpose, and both are safe for one reason: a value beginning with `-` handed
# as a SEPARATE word is refused LOUD by argparse (`--match -O2`: "expected one argument"), so it reaches
# the program only in the `--flag=value` form, which a caller writes on purpose. They are escapes from the
# refusal all the same, so clause 14 pins how many arguments take each.
def text(value, flag):
    """`value` as the text `flag` names, or `UsageRefusal` when it begins with `-`. An empty value passes."""
    value = "" if value is None else str(value)
    if value.startswith("-"):
        raise UsageRefusal("%s takes '%s', and it begins with `-`: an option taken for a value -- a value that "
                           "may truly begin with one is declared so (`option_text_arg`, `command_words_arg`)"
                           % (flag, value))
    return value


def _value_arg(kind, flag, name, then):
    """argparse's `type=` asking `kind(value, flag)` -- its refusal raised as `argparse.ArgumentTypeError` --
    then handing what it kept to `then`, the program's own conversion, when there is one."""
    import argparse

    def value_type(value):
        try:
            value = kind(value, flag)
        except UsageRefusal as exc:
            raise argparse.ArgumentTypeError(str(exc))
        return value if then is None else then(value)
    value_type.__name__ = name   # argparse names a type by it in an error it makes itself
    return value_type


def text_arg(flag, then=None):
    """argparse's `type=` for a value that names no path (`text`), converted by `then` when one is given."""
    return _value_arg(text, flag, "text", then)


def option_text_arg(flag):
    """argparse's `type=` for a text value that may begin with `-` by its nature (`--match=-O2`, URL-safe
    base64): kept as given."""
    return _value_arg(lambda value, _flag: str(value), flag, "option text", None)


def command_words_arg(flag):
    """argparse's `type=` for a word of a child's command line or options (`--launcher=qemu-s390x`): kept as
    given -- the child judges it."""
    return _value_arg(lambda value, _flag: str(value), flag, "command word", None)


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


# ── WHAT A WALK OF A TREE LEAVES OUT: what git ignores there (`TreeIgnores`) ─────────────────────
# ★★ ONE ANSWER FOR EVERY PROGRAM THAT WALKS A TREE FROM ITS ROOT. Such a walk reads the tree's
# CONTENT, and what git ignores there is not content: the copies and workers DssHarness keeps in
# a tree (`.legs/`, `.mutations/`, beside `.worktrees/` and `.orchestrators/` -- each a whole tree
# of its own, `dssharness help layout`), its run records and locks, a session's scratch, a build.
# A census walking a leg copy counts every example twice and may write those figures; a
# case-collision scan walking a sweep worker meets files that vanish under it. The statement is
# the tree's own `.gitignore`: the file the tool's `init` writes its block into and git obeys. No
# list is kept here, so a directory the tool or the repository adds there is left out by every
# walker at once. Its readers: `check-doc-census` (both of its walks) and its `source-census.py`
# provider, the redactor's vocabulary (`redact.py`), `check-anchor-registry`, `check-plan-citations`,
# `check-scripts-index`, `check-export-macro-placement`, and -- in C++, with this grammar and these
# refusals -- the repository-wide scan of `tests/core/test_header_name_matching.cpp`
# (`tests/test_support/tree_ignores.hpp`). A walker keeps no list of its own: a directory it must
# also skip that git does NOT ignore is that program's own rule, stated beside it with its reason.
# ★ READ AS GIT READS IT, BYTE FOR BYTE, WITH core.ignorecase FALSE -- and a path whose answer the
# other setting would change is REFUSED (below): every rule in order, the last that matches
# deciding; `!` re-includes; a trailing `/` matches a directory only -- never a symbolic link,
# which git records as a file and which no walk descends; a `/` at the start or in the middle
# anchors the rule to the tree's root, and a rule with none matches that name at any depth. A
# name's pattern is matched against the name's BYTES (its UTF-8), as git's wildmatch matches
# them: `*` is any run of bytes within one name, `?` is one byte, and `[...]` is a class that
# takes one byte -- `!` or `^` first negates it, a `]` right after the opening is itself, `a-z` is
# a range of bytes, a `-` first, last, or right after a range or a POSIX class is itself, a `[:`
# that no `:]` closes before the class's `]` is a `[` member, and the twelve POSIX classes
# `[:alnum:]` `[:alpha:]` `[:blank:]` `[:cntrl:]` `[:digit:]` `[:graph:]` `[:lower:]` `[:print:]`
# `[:punct:]` `[:space:]` `[:upper:]` `[:xdigit:]` take what git's own ASCII table puts in them (a
# byte from 0x80 up is in none; `[:space:]` is tab, line feed, carriage return and space). A
# `**` segment is any number of directories -- one or more at a rule's end; inside a name it is a
# `*`. Trailing SPACES are dropped, as git drops them (a tab stays); a byte order mark before the
# first rule and a carriage return before a line feed are no part of a rule. A directory left out
# takes everything under it: nothing below it can be re-included, as in git. `.git`, git's own,
# is always left out, as a directory or as a worktree's `.git` file.
# ★ CASE: A PATH A RULE MATCHES UNDER ONE SETTING OF core.ignorecase AND NOT THE OTHER IS A
# REFUSAL NAMING THE PATH AND THE RULE, on every host -- and so is a name that is `.git` only when
# case is folded. git folds ASCII letters when core.ignorecase is true (the setting a clone takes
# on Windows and macOS), and the two settings disagree both ways (✔MEASURED, git 2.55: `*.LOG`
# takes `a.log` only when folded, and `[A]1` takes `A1` only when NOT folded -- git folds the
# name, never a class's member). Refusing every such path, these readers give the one answer
# every host's git gives, and no host diverges in silence.
# ★ THE ROOT'S `.gitignore` ALONE: it is the only one git tracks here (✔MEASURED 2026-10-15,
# `git ls-files`), and git's rules there leave out no tracked file (`git ls-files -ci
# --exclude-standard` lists none), so a walk loses nothing the tree tracks. One a tool writes
# inside a directory the root's rules already leave out changes nothing a walk sees.
# ⚠ A RULE THIS READER CANNOT READ AS GIT DOES IS A REFUSAL NAMING ITS LINE: a backslash (an
# escape), a `[` class never closed, a POSIX class git does not name (git abandons the whole match
# there), an empty name between two `/`, a rule that is nothing but `/` or `!`. Read some other way
# it would leave out another set than git does, in silence.
# ⚠ A NAME HOLDING A BACKSLASH IS A REFUSAL NAMING IT: no Windows host can hold such a name, and a
# reader on one would split it where git on another does not.
# ⓘ THE ONE CASE TABLE, `tests/test_support/tree_ignores_cases.json`, states this grammar path by
# path; this program's self-test and the C++ reader's suite each ask every scenario of it of their
# reader, and each asks git itself the same paths (`git check-ignore`) under both settings.
# ⓘ A tree with no `.gitignore` declares nothing ignored: only `.git` is left out.
# ⓘ `keep` names root-relative directories walked WHOLE whatever the rules say. None is kept by
# default; the redactor keeps the tool's build directory, whose output names its exemption reads.

IGNORE_FILE = ".gitignore"
GIT_OWN = ".git"
CASE_TABLE = ("tests", "test_support", "tree_ignores_cases.json")
_ANY_DIRS = ("**",)                                  # a `**` segment of a rule
_LIT, _ONE, _STAR, _CLASS = range(4)                 # the tokens of one name's pattern
_MEMBER, _RANGE, _POSIX = range(3)                   # the items of a class


def _ascii_upper(b):
    return 65 <= b <= 90


def _ascii_lower(b):
    return 97 <= b <= 122


def _ascii_alpha(b):
    return _ascii_upper(b) or _ascii_lower(b)


def _ascii_digit(b):
    return 48 <= b <= 57


# git's own table (sane_ctype, and wildmatch's use of it): ASCII only. Under core.ignorecase the
# name's byte is already folded to lower case, which is why `upper` also takes a lower-case byte.
_POSIX_CLASSES = {
    b"alnum": lambda b, fold: _ascii_alpha(b) or _ascii_digit(b),
    b"alpha": lambda b, fold: _ascii_alpha(b),
    b"blank": lambda b, fold: b in (9, 32),
    b"cntrl": lambda b, fold: b < 32 or b == 127,
    b"digit": lambda b, fold: _ascii_digit(b),
    b"graph": lambda b, fold: 33 <= b <= 126,
    b"lower": lambda b, fold: _ascii_lower(b),
    b"print": lambda b, fold: 32 <= b <= 126,
    b"punct": lambda b, fold: 33 <= b <= 126 and not (_ascii_alpha(b) or _ascii_digit(b)),
    b"space": lambda b, fold: b in (9, 10, 13, 32),
    b"upper": lambda b, fold: _ascii_upper(b) or (fold and _ascii_lower(b)),
    b"xdigit": lambda b, fold: _ascii_digit(b) or 65 <= b <= 70 or 97 <= b <= 102,
}


def _read_class(pat, at):
    """The class opening at `pat[at]` (a `[`), as wildmatch reads it: `(end, negated, items)` with `end` just past
    its `]`, or a string saying why git could not read it."""
    n = len(pat)
    i = at + 1
    c = pat[i] if i < n else None
    if c == 0x5E:                                    # `^` negates as `!` does
        c = 0x21
    negated = c == 0x21
    if negated:
        i += 1
        c = pat[i] if i < n else None
    prev, items = 0, []
    while True:
        if c is None:
            return "opens a `[` class it never closes"
        if c == 0x2D and prev and i + 1 < n and pat[i + 1] != 0x5D:
            i += 1
            items.append((_RANGE, prev, pat[i]))
            c = 0                                    # a `-` right after a range is itself
        elif c == 0x5B and i + 1 < n and pat[i + 1] == 0x3A:
            j = i + 2
            while j < n and pat[j] != 0x5D:
                j += 1
            if j >= n:
                return "opens a `[` class it never closes"
            if j - i - 3 < 0 or pat[j - 1] != 0x3A:
                items.append((_MEMBER, 0x5B))        # no `:]`: the `[` is a member, and the `:` after it
            else:
                name = bytes(pat[i + 2:j - 1])
                if name not in _POSIX_CLASSES:
                    return "names the class [:%s:], which git does not know" % name.decode("utf-8", "replace")
                items.append((_POSIX, name))
                i, c = j, 0                          # a `-` right after a POSIX class is itself
        else:
            items.append((_MEMBER, c))
        prev = c
        i += 1
        c = pat[i] if i < n else None
        if c == 0x5D:
            return i + 1, negated, tuple(items)


def _read_name_pattern(pat):
    """One name of a rule (bytes) as a `_NamePattern`, or a string saying why git could not read it."""
    out, i = [], 0
    while i < len(pat):
        c = pat[i]
        if c == 0x2A:
            if not out or out[-1][0] != _STAR:       # `**` inside a name is a `*`
                out.append((_STAR,))
            i += 1
        elif c == 0x3F:
            out.append((_ONE,))
            i += 1
        elif c == 0x5B:
            cls = _read_class(pat, i)
            if isinstance(cls, str):
                return cls
            i, negated, items = cls
            out.append((_CLASS, negated, items))
        else:
            out.append((_LIT, c))
            i += 1
    return _NamePattern(tuple(out))


def _byte_hit(tok, t, fold):
    """Does one token take the name's byte `t`? Under `fold`, as wildmatch folds: the name's byte and a literal
    of the rule to lower case, a range also asked with the byte in upper case -- a class's member is never folded."""
    kind = tok[0]
    if kind == _ONE:
        return True
    if fold and _ascii_upper(t):
        t += 32
    if kind == _LIT:
        p = tok[1]
        return t == (p + 32 if fold and _ascii_upper(p) else p)
    hit = False
    for item in tok[2]:
        if item[0] == _MEMBER:
            hit = t == item[1]
        elif item[0] == _RANGE:
            hit = item[1] <= t <= item[2] or (fold and _ascii_lower(t) and item[1] <= t - 32 <= item[2])
        else:
            hit = _POSIX_CLASSES[item[1]](t, fold)
        if hit:
            break
    return hit != tok[1]


def _compile_name(toks, fold):
    """The tokens as one bytes regex: a literal or a class becomes the set of the bytes `_byte_hit` says it takes
    (a set of none matches nothing), `?` any byte, `*` any run -- so the regex answers exactly what the tokens say,
    at the regex engine's pace. Under `fold` it is asked of the name already folded to lower case."""
    parts = []
    for tok in toks:
        if tok[0] == _STAR:
            parts.append(b".*")
        elif tok[0] == _ONE:
            parts.append(b".")
        else:
            taken = b"".join(re.escape(bytes([b])) for b in range(256) if _byte_hit(tok, b, fold))
            parts.append(b"[" + taken + b"]" if taken else b"(?!)")
    return re.compile(b"".join(parts), re.DOTALL)


class _NamePattern:
    """One name of a rule: its tokens, and the two compiled forms that ask them of a name's bytes -- `exact`, and
    `folded`, asked of the name already folded to lower case (wildmatch folds the name's byte first)."""
    __slots__ = ("toks", "exact", "folded")

    def __init__(self, toks):
        self.toks, self.exact, self.folded = toks, _compile_name(toks, False), _compile_name(toks, True)


def name_matches(pat, name, fold=False):
    """One NAME (bytes; already folded to lower case when `fold`) against one `_NamePattern`."""
    return (pat.folded if fold else pat.exact).fullmatch(name) is not None


def names_match(pats, names, fold=False):
    """A rule's name patterns against a path's names (bytes; already folded to lower case when `fold`): a `**`
    pattern is any number of names (one or more when it is the last). The two shapes nearly every rule has are
    asked directly -- no `**` (one name per pattern) and a rule with no slash (`**` then one pattern: the last
    name) -- and any other by a table filled from the end: no recursion proportional to the path."""
    m, n = len(pats), len(names)
    if not any(p is _ANY_DIRS for p in pats):
        return m == n and all(name_matches(p, x, fold) for p, x in zip(pats, names))
    if m == 2 and pats[0] is _ANY_DIRS and pats[1] is not _ANY_DIRS:
        return n >= 1 and name_matches(pats[1], names[-1], fold)
    ok = [[False] * (n + 1) for _ in range(m + 1)]
    ok[m][n] = True
    for i in range(m - 1, -1, -1):
        for j in range(n, -1, -1):
            if pats[i] is _ANY_DIRS:
                ok[i][j] = (j < n) if i == m - 1 else (ok[i + 1][j] or (j < n and ok[i][j + 1]))
            else:
                ok[i][j] = j < n and name_matches(pats[i], names[j], fold) and ok[i + 1][j + 1]
    return ok[0][0]


class IgnoreRule:
    """One rule of a `.gitignore`, as git reads it: `pats` are its name patterns as tokens, `**` among them."""
    __slots__ = ("text", "line", "negated", "dir_only", "pats")

    def __init__(self, text, line, negated, dir_only, pats):
        self.text, self.line, self.negated, self.dir_only, self.pats = text, line, negated, dir_only, pats

    def __repr__(self):
        return "%s (line %d)" % (self.text, self.line)


def read_ignore_rules(text, source=IGNORE_FILE):
    """[IgnoreRule] of a `.gitignore`'s text, in order. A rule this reader cannot read as git does is a Refusal
    naming its line (see the section above)."""
    rules = []
    if text.startswith("\ufeff"):
        text = text[1:]
    for number, raw in enumerate(text.split("\n"), 1):
        line = raw[:-1] if raw.endswith("\r") else raw
        if not line or line.startswith("#"):
            continue
        line = line.rstrip(" ")
        if not line:
            continue

        def refuse(why):
            return Refusal("%s line %d: the rule %r %s, so it cannot be read as git reads it; every walk of this "
                           "tree is refused rather than leaving out another set than git does" % (
                               source, number, line, why))
        if "\\" in line:
            raise refuse("holds a backslash (an escape)")
        negated = line.startswith("!")
        body = line[1:] if negated else line
        dir_only = body.endswith("/")
        if dir_only:
            body = body[:-1]
        anchored = "/" in body
        if body.startswith("/"):
            body = body[1:]
        names = body.split("/")
        if not body or any(not name for name in names):
            raise refuse("names nothing between two separators" if body else "names nothing")
        pats = []
        for name in names:
            if name == "**":
                pats.append(_ANY_DIRS)
                continue
            toks = _read_name_pattern(name.encode("utf-8"))
            if isinstance(toks, str):
                raise refuse(toks)
            pats.append(toks)
        rules.append(IgnoreRule(line, number, negated, dir_only, pats if anchored else [_ANY_DIRS] + pats))
    return rules


def _name_bytes(name, path):
    """A walked name as the bytes git compares; a name holding a backslash is a Refusal naming the path."""
    if "\\" in name:
        raise Refusal("the name %r in %s holds a backslash: no Windows host can hold it, and a reader on one splits "
                      "it where git on another does not, so the walk is refused rather than read as one host reads "
                      "it" % (name, path))
    try:
        return os.fsencode(name)
    except UnicodeError as exc:
        raise Refusal("the name %r in %s has no UTF-8 spelling (%s): git could not be asked about it" % (
            name, path, exc))


def _is_link(path):
    """A symbolic link (or a Windows junction): never a directory to the rules, never descended."""
    isjunction = getattr(os.path, "isjunction", None)
    return os.path.islink(path) or bool(isjunction and isjunction(path))


class TreeIgnores:
    """What a walk of `tree` leaves out: what git ignores there, read from the tree's own `.gitignore`, and `.git`.
    `keep`: root-relative directories walked whole whatever the rules say."""

    def __init__(self, tree, keep=()):
        self.tree = os.path.realpath(_require_path(tree, "the tree a walk reads"))
        path = os.path.join(self.tree, IGNORE_FILE)
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except FileNotFoundError:
            data = b""
        except OSError as exc:
            raise Refusal("%s cannot be read (%s): what a walk of the tree leaves out is unknown" % (path, exc))
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise Refusal("%s is not UTF-8 (%s): what a walk of the tree leaves out is unknown" % (path, exc))
        self.rules = read_ignore_rules(text, path)
        self.keep = []
        for k in keep:
            names = k.replace("\\", "/").strip("/").split("/")
            if not k or any(n in ("", ".", "..") for n in names):
                raise Refusal("a directory walked whole must be named relative to the tree: %r" % (k,))
            self.keep.append(names)

    def _decide(self, names, is_dir):
        """The rule that leaves this path out (its ancestors already walked), or None. A rule whose match on it
        core.ignorecase would change, and a name that is `.git` only when folded, are a Refusal naming it."""
        if GIT_OWN in names:
            return GIT_OWN
        shown = "/".join(names)
        raw = [_name_bytes(x, shown) for x in names]
        for x in raw:
            if x.lower() == b".git":
                raise Refusal("the path %r holds %r, which git takes for its own directory only when case is "
                              "folded (core.ignorecase true): hosts would answer differently, so the walk is "
                              "refused" % (shown, os.fsdecode(x)))
        if any(names[:len(k)] == k for k in self.keep):
            return None
        low = [x.lower() for x in raw]                   # bytes: ASCII letters only, as git folds them
        decided = None
        for rule in self.rules:
            if rule.dir_only and not is_dir:
                continue
            hit = names_match(rule.pats, raw)
            if hit != names_match(rule.pats, low, True):
                raise Refusal("the rule %r (line %d) %s the path %r only when case is %s: git answers it one way "
                              "under core.ignorecase false and the other under true, so every host would not give "
                              "one answer and the walk is refused" % (
                                  rule.text, rule.line, "matches" if hit else "fails", shown,
                                  "NOT folded" if hit else "folded"))
            if hit:
                decided = None if rule.negated else rule
        return decided.text if decided is not None else None

    def rel(self, path):
        """`path` relative to the tree, `/`-joined ('' for the tree itself); a path outside it is a Refusal."""
        rel = os.path.relpath(os.path.realpath(path), self.tree).replace(os.sep, "/")
        if rel == ".":
            return ""
        if rel == ".." or rel.startswith("../") or os.path.isabs(rel):
            raise Refusal("%s is not inside the tree %s: what a walk of it leaves out is that tree's to say"
                          % (path, self.tree))
        return rel

    def leaves_out(self, rel, is_dir):
        """The rule that leaves the tree-relative, `/`-joined path `rel` out -- its own, or a parent directory's --
        or None. A name holding a backslash is a Refusal (see `_name_bytes`), never a second separator."""
        names = [n for n in rel.split("/") if n]
        for depth in range(1, len(names)):
            why = self._decide(names[:depth], True)
            if why:
                return why
        return self._decide(names, is_dir) if names else None

    def walk(self, top=None, onerror=None):
        """`os.walk(top)`, top-down, less what this tree leaves out: each `(dirpath, dirnames, filenames)` with the
        directories left out already taken from `dirnames` (the caller may prune or sort it further, in place) and
        the files left out taken from `filenames`. A `top` the tree leaves out is a Refusal: a walk of nothing.
        ★ A SYMBOLIC LINK IS NEVER DESCENDED AND IS NO DIRECTORY: a link to a directory leaves `dirnames` and is
        judged, and yielded, among the files -- git records a link as a file, and a walk through one would read
        another tree, or this one twice.
        ★ A DIRECTORY THE WALK CANNOT LIST IS A REFUSAL NAMING IT, the top among them: `os.walk` skips one in
        silence, and the walk then read less of the tree than every caller says it read (the C++ scan records the
        same as its `walkError`). A caller that means to go on past one passes `onerror`, as `os.walk` takes it."""
        top = self.tree if top is None else top
        base = self.rel(top)
        why = self.leaves_out(base, True) if base else None
        if why:
            raise Refusal("the walk's top %s is left out of the tree by %s" % (base, why))

        def unlisted(exc):
            raise Refusal("the walk cannot list %s (%s): a walk that went on would read less of the tree than it "
                          "says" % (getattr(exc, "filename", None) or top, exc.strerror or type(exc).__name__))
        for dirpath, dirnames, filenames in os.walk(top, onerror=onerror or unlisted):
            sub = os.path.relpath(dirpath, top).replace(os.sep, "/")
            rel = base if sub == "." else (base + "/" + sub if base else sub)
            names = rel.split("/") if rel else []
            links = [d for d in dirnames if _is_link(os.path.join(dirpath, d))]
            dirnames[:] = [d for d in dirnames if d not in links and not self._decide(names + [d], True)]
            yield dirpath, dirnames, [f for f in filenames + links if not self._decide(names + [f], False)]


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
            # `onexc` is 3.12's: every interpreter that runs this is at or above the repository's floor
            # (config.json's python3 minVersion), which every ctest entry and every runner step is held to.
            shutil.rmtree(path, onexc=_writable_then_retry)
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

    # ONE VERDICT LINE PER ARM, in the grammar `tests/test_support/script_selftest.hpp` reads: the ctest
    # entry `owning_tree_selftest_guard` is a test binary that runs this and holds a case per arm, so a
    # mutation of this file can be an arm of the mutation sweep. A failing arm's detail is put on ONE line.
    def pin(label, fn):
        try:
            res = fn()
            ok, detail = (res if isinstance(res, tuple) else (bool(res), ""))
        except Exception as exc:  # noqa: BLE001 - an arm that crashes FAILS, it never aborts the rest
            ok, detail = False, "CRASHED %s: %s" % (type(exc).__name__, exc)
        ran.append(label)
        if not ok:
            failed.append(label)
            said = (str(detail) or "no detail").replace("\r", " ").replace("\n", " ")
            print("owning-tree self-test: FAILED  %s -- %s" % (label, said))
        else:
            print("owning-tree self-test: ok      %s" % label)

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

        def j5():
            # The repository's Python floor is stated once, in the tree's config (the `tools` entry
            # `python3`, its `minVersion`), and the build's configure asks for an interpreter at or above
            # it (cmake/DssPython.cmake); this self-test runs on the interpreter that configure chose.
            cfg = load_jsonc(os.path.join(owning_tree(__file__), ".harness-config", "config.json"))
            floors = [t.get("minVersion") for t in cfg.get("tools", []) if t.get("name") == "python3"]
            if len(floors) != 1 or not re.match(r"^\d+\.\d+\.\d+$", str(floors[0])):
                return False, ("the tree's config states no single python3 minVersion of the form "
                               "major.minor.patch: %r" % (floors,))
            have = tuple(sys.version_info[:3])
            return (have >= tuple(int(x) for x in floors[0].split(".")),
                    "this interpreter is %s, below the floor %s the tree's config states" % (
                        ".".join(str(x) for x in have), floors[0]))
        pin("J5 the interpreter running this self-test meets the Python floor the tree's config states", j5)

        # ── L: what a walk of a tree leaves out (`TreeIgnores`) ────────────────
        # THE ONE CASE TABLE (`tests/test_support/tree_ignores_cases.json`) states every scenario: its rules and each
        # path's kind and answer, or the line a reader refuses. An L arm that names a scenario asks it of THIS
        # reader; the C++ reader's suite asks the same scenario of its own; L30 asks git itself. The table is read
        # on first use INSIDE an arm, so a missing or broken table fails the arms that ask it, each by name.
        w_cache, w_trees, w_made_trees, w_asked = {}, {}, {}, []

        def w_table():
            if "table" not in w_cache:
                with open(os.path.join(owning_tree(__file__), *CASE_TABLE), encoding="utf-8") as fh:
                    w_cache["table"] = json.load(fh)["scenarios"]
            return w_cache["table"]

        def w_scenario(name):
            found = [s for s in w_table() if s["name"] == name]
            if len(found) != 1:
                raise Refusal("the case table holds %d scenario(s) named %r" % (len(found), name))
            return found[0]

        def w_rules_text(sc):
            nl = sc.get("newline", "\n")
            return nl.join(sc["rules"]) + nl

        def w_tree_of(name):
            """A tree holding the scenario's `.gitignore` and nothing else (made once)."""
            if name not in w_trees:
                t = os.path.join(box, "w-rules-%d" % len(w_trees))
                os.makedirs(t)
                with open(os.path.join(t, IGNORE_FILE), "wb") as fh:
                    fh.write(w_rules_text(w_scenario(name)).encode("utf-8"))
                w_trees[name] = t
            return w_trees[name]

        def w_link(target, path):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            os.symlink(target, path, target_is_directory=True)

        def w_made(name):
            """The scenario's tree with its paths made -- each file, directory and link this host can hold (a
            refused path is never made): (tree, [(path, kind) made], [paths this host cannot hold])."""
            if name not in w_made_trees:
                sc = w_scenario(name)
                t = os.path.join(box, "w-made-%d" % len(w_made_trees))
                target = os.path.join(box, "w-made-target")
                os.makedirs(target, exist_ok=True)
                os.makedirs(t)
                with open(os.path.join(t, IGNORE_FILE), "wb") as fh:
                    fh.write(w_rules_text(sc).encode("utf-8"))
                made, cannot = [], []
                for path, kind, answer in sc["paths"]:
                    if isinstance(answer, dict):
                        continue
                    full = os.path.join(t, *path.split("/"))
                    try:
                        if kind == "dir":
                            os.makedirs(full, exist_ok=True)
                        elif kind == "link":
                            w_link(target, full)
                        else:
                            _touch(full, "x\n")
                        made.append((path, kind))
                    except OSError:
                        cannot.append(path)
                w_made_trees[name] = (t, made, cannot)
            return w_made_trees[name]

        def w_ask(ti, path, kind):
            """This reader's answer for one path: None (kept), the rule text, or {"refused": its message}."""
            try:
                return ti.leaves_out(path, kind == "dir")
            except Refusal as exc:
                return {"refused": str(exc)}

        def w_render(answer):
            if answer is None:
                return "kept"
            if isinstance(answer, dict):
                return "refused (%s)" % answer["refused"][:140]
            return "left out by %s" % answer

        def w_agrees(answer, got):
            if isinstance(answer, dict):
                return isinstance(got, dict) and answer["refused"] in got["refused"]
            return not isinstance(got, dict) and got == answer

        def w_answers(name):
            """Every path of one scenario asked of this reader, over the scenario's own `.gitignore` on disk."""
            w_asked.append(name)
            sc = w_scenario(name)
            ti = TreeIgnores(w_tree_of(name))
            wrong = ["%s (%s): answered %s, the table says %s" % (path, kind, w_render(w_ask(ti, path, kind)),
                                                                  w_render(answer))
                     for path, kind, answer in sc["paths"] if not w_agrees(answer, w_ask(ti, path, kind))]
            return not wrong, "%d of %d path(s) answered otherwise: %s" % (len(wrong), len(sc["paths"]),
                                                                          "; ".join(wrong))

        def w_refusals(name):
            """Every rule set of one scenario read by this reader: refused at the line the table names (saying
            what it says), or read whole where the table names no line."""
            w_asked.append(name)
            wrong = []
            for r in w_scenario(name)["refusals"]:
                shown = "[%s]" % " ; ".join(r["rules"])
                ok, said = _raises(lambda: read_ignore_rules("\n".join(r["rules"]) + "\n", "fixture-ignore"),
                                   Refusal)
                if r["line"] is None:
                    if ok:
                        wrong.append("%s was refused: %s" % (shown, said[:160]))
                elif not ok:
                    wrong.append("%s was read whole, the table says refused at line %d" % (shown, r["line"]))
                elif ("fixture-ignore line %d:" % r["line"]) not in said or r["says"] not in said:
                    wrong.append("%s was refused otherwise than at line %d for %s: %s" % (
                        shown, r["line"], r["says"], said[:160]))
            return not wrong, "%d rule set(s) read otherwise: %s" % (len(wrong), "; ".join(wrong))

        def w_walked(ti, top=None):
            """{(path, "dir" | "file")} the walk yields under `top` (a link is yielded among the files)."""
            got = set()
            for dirpath, dirnames, filenames in ti.walk(top):
                rel = ti.rel(dirpath)
                got.update(((rel + "/" + d) if rel else d, "dir") for d in dirnames)
                got.update(((rel + "/" + f) if rel else f, "file") for f in filenames)
            return got

        w_copies = "the copies the tool keeps in a tree, a session's scratch and a build"
        pin("L1 the copies the tool keeps in a tree, a session's scratch and a build are left out; the real files "
            "and each placeholder stay", lambda: w_answers(w_copies))
        pin("L2 a later rule decides, and a negation re-includes",
            lambda: w_answers("a later rule decides, and a negation re-includes"))
        pin("L3 a rule with no slash leaves out that name at any depth",
            lambda: w_answers("a rule with no slash matches that name at any depth"))
        pin("L4 a rule with a slash is anchored to the tree's root: that directory goes, a namesake elsewhere "
            "stays", lambda: w_answers("a rule with a slash is anchored to the tree's root"))
        pin("L5 a rule ending in a slash matches a directory only: a FILE of that name stays",
            lambda: w_answers("a rule ending in a slash matches a directory only"))
        pin("L6 a double-star segment is any number of directories",
            lambda: w_answers("a double-star name is any number of directories"))
        pin("L7 a rule ending in a slash never matches a symbolic link",
            lambda: w_answers("a rule ending in a slash never matches a symbolic link"))
        pin("L8 a star, a question mark and a class match the name's bytes, as git's wildmatch does",
            lambda: w_answers("a star, a question mark and a class match the name's bytes"))
        pin("L9 nothing under a directory left out is re-included",
            lambda: w_answers("nothing under a left-out directory is re-included"))
        pin("L10 git's own directory and a worktree's git file are left out",
            lambda: w_answers("git's own directory is always left out, as a directory or a worktree's file"))
        pin("L11 a rule holding a backslash is refused, naming its line",
            lambda: w_refusals("a rule holding a backslash is refused, naming its line"))
        pin("L12 a class opened and never closed is refused, naming its line",
            lambda: w_refusals("a class opened and never closed is refused, naming its line"))
        pin("L13 a rule naming nothing between two separators, or nothing at all, is refused",
            lambda: w_refusals("a rule naming nothing between two separators, or nothing at all, is refused"))

        def w14():
            t = os.path.join(box, "w14")
            for rel in ("a/b.md", "build/x", ".git/HEAD"):
                _touch(os.path.join(t, *rel.split("/")), "x\n")
            got = {p for p, kind in w_walked(TreeIgnores(t)) if kind == "file"}
            return got == {"a/b.md", "build/x"}, "walked=%r" % sorted(got)
        pin("L14 a tree with no .gitignore leaves out git's own directory alone", w14)

        def w15():
            t, _made, _cannot = w_made(w_copies)
            got = {p for p, kind in w_walked(TreeIgnores(t, ("build",))) if kind == "file"}
            want_in = ("build/v1/bin/dss/runner.json", "src/a.cpp")
            want_out = (".temp/x/notes.md", "tests/.temp/notes.md", ".legs/x/examples/README.md")
            lost, took = [p for p in want_in if p not in got], [p for p in want_out if p in got]
            return not lost and not took, "kept but not walked=%r; left out but walked=%r" % (lost, took)
        pin("L15 a directory kept is walked whole, whatever the rules say inside it", w15)

        def w16():
            t, _made, _cannot = w_made(w_copies)
            ti = TreeIgnores(t)
            top, tm = _raises(lambda: list(ti.walk(os.path.join(t, ".legs", "x"))), Refusal)
            out, om = _raises(lambda: ti.rel(box), Refusal)
            return top and "/.legs/*" in tm and out, "%r / %r" % (tm[:80], om[:80])
        pin("L16 a walk whose top is left out is refused, and so is a path outside the tree", w16)
        pin("L17 a path a rule matches under one setting of core.ignorecase and not the other is refused, naming "
            "it", lambda: w_answers("a path a rule matches under one setting of core.ignorecase and not the other "
                                    "is refused"))

        def w18():
            ti = TreeIgnores(owning_tree(__file__))
            answers = {d: (ti.leaves_out(d + "/x", True), ti.leaves_out(d + "/.gitkeep", False))
                       for d in (".worktrees", ".orchestrators", ".mutations", ".legs")}
            return (all(out and keep is None for out, keep in answers.values()),
                    "answers=%r" % (answers,))
        pin("L18 LIVE: this tree's own .gitignore leaves out every directory the tool keeps in a tree "
            "and keeps each placeholder", w18)

        def w19():
            # Every scenario's paths made on disk (what this host can hold) and walked: a path is walked exactly
            # when the question about it answers "kept".
            compared, cannot, disagree = 0, 0, []
            for sc in w_table():
                if "paths" not in sc:
                    continue
                t, made, unmade = w_made(sc["name"])
                cannot += len(unmade)
                ti = TreeIgnores(t)
                got = w_walked(ti)
                for path, kind in made:
                    compared += 1
                    asked_kept = ti.leaves_out(path, kind == "dir") is None
                    if asked_kept != ((path, "dir" if kind == "dir" else "file") in got):
                        disagree.append("%s (%s): asked %s, walked %s" % (
                            path, kind, "kept" if asked_kept else "left out", "no" if asked_kept else "yes"))
            return (compared > 0 and not disagree,
                    "%d path(s) compared (%d this host cannot hold); walk and question disagree on %d: %s" % (
                        compared, cannot, len(disagree), "; ".join(disagree)))
        pin("L19 the walk and the question about one path give one answer on every path", w19)

        def w20():
            tree = owning_tree(__file__)
            p = run_git(["ls-files", "-z"], cwd=tree, capture_output=True)
            if p.returncode != 0:
                return False, "git ls-files exited %d: %s" % (
                    p.returncode, p.stderr.decode("utf-8", "replace").strip()[:120])
            tracked = [t for t in p.stdout.decode("utf-8", "surrogateescape").split("\0") if t]
            ti = TreeIgnores(tree)
            out = [(t, ti.leaves_out(t, False)) for t in tracked]
            out = [o for o in out if o[1]]
            return len(tracked) > 100 and not out, "%d tracked, left out: %r" % (len(tracked), out[:5])
        pin("L20 LIVE: no file git tracks in this tree is left out of a walk -- a rule of its .gitignore "
            "cannot silence a tracked file", w20)

        def w21():
            # A directory listed and gone before the walk descends: the one shape of "cannot be listed" every
            # host makes alike (a permission is a system setting, and an administrator reads through it).
            t = os.path.join(box, "w21")
            for rel in ("a.md", "gone/inner/b.md"):
                _touch(os.path.join(t, *rel.split("/")), "x\n")
            walk = TreeIgnores(t).walk()
            listed = next(walk)[1]
            remove_tree(os.path.join(t, "gone"))
            refused, msg = _raises(lambda: list(walk), Refusal)
            return (refused and "gone" in listed and "cannot list" in msg and "gone" in msg,
                    "the walk listed %s, then %s" % (", ".join(sorted(listed)) or "nothing",
                                                    ("was refused: " + msg[:160]) if refused else "went on"))
        pin("L21 a directory the walk cannot list is refused by name, never skipped", w21)
        pin("L22 the twelve POSIX classes read as git reads them",
            lambda: w_answers("the twelve POSIX classes read as git reads them"))
        pin("L23 a dash, a bracket first, a range and a lone [: inside a class read as git reads them",
            lambda: w_answers("a dash, a bracket first, a range and a lone [: inside a class read as git reads "
                              "them"))
        pin("L24 a POSIX class git does not name is refused, naming its line",
            lambda: w_refusals("a POSIX class git does not name is refused, naming its line"))
        pin("L25 a name holding a backslash is refused, naming it",
            lambda: w_answers("a name holding a backslash is refused"))
        pin("L26 a byte order mark, a CR LF ending and trailing spaces are no part of a rule; a trailing tab is",
            lambda: w_answers("a byte order mark, a CR LF ending and trailing spaces are no part of a rule, a "
                              "trailing tab is"))
        pin("L27 a name that is git's own only when case is folded is refused",
            lambda: w_answers("a name that is git's own only when case is folded is refused"))

        def w28():
            t = os.path.join(box, "w28")
            _touch(os.path.join(t, "real", "x.md"), "x\n")
            _touch(os.path.join(t, IGNORE_FILE), "build/\n")
            try:
                for name in ("build", "elsewhere"):
                    w_link(os.path.join(t, "real"), os.path.join(t, name))
            except OSError as exc:
                return False, "a symbolic link cannot be made on this host (%s): the arm did not run" % exc
            got = w_walked(TreeIgnores(t))
            want = {(".gitignore", "file"), ("real", "dir"), ("real/x.md", "file"), ("build", "file"),
                    ("elsewhere", "file")}
            return got == want, "walked %s" % ", ".join("%s (%s)" % p for p in sorted(got))
        pin("L28 the walk never descends a symbolic link: a link to a directory is judged, and yielded, as a file",
            w28)

        def w29():
            t = os.path.join(box, "w29")
            os.makedirs(os.path.join(t, IGNORE_FILE))
            refused, msg = _raises(lambda: TreeIgnores(t), Refusal)
            return refused and "cannot be read" in msg, msg[:160]
        pin("L29 a .gitignore that is a directory is refused: what a walk leaves out is unknown", w29)

        def w30():
            # git itself, over every scenario's paths: one repository, each scenario in its own directory whose
            # .gitignore holds the scenario's rules (anchored to that directory exactly as a root rule is to the
            # root); a `dir` leaf made a directory and a `link` leaf a link (git decides a path it cannot stat as
            # no directory, so a `file` leaf needs no file). Asked under both settings of core.ignorecase, with no
            # excludes file of the host's. A path the table refuses for case must be one git answers differently
            # under the two; every other answered path must be git's under both.
            if not shutil.which("git"):
                return False, "no git on this host: the live comparison did not run, which is no pass"
            live = os.path.join(box, "w-live")
            repo, target = os.path.join(live, "repo"), os.path.join(live, "link-target")
            os.makedirs(target)
            p = run_git(["init", "-q", repo], capture_output=True)
            if p.returncode != 0:
                return False, "git init exited %d" % p.returncode
            asked, tops = [], {}
            for k, sc in enumerate(w_table()):
                if "paths" not in sc or sc.get("asked_of_git") is False:
                    continue
                top = tops[sc["name"]] = os.path.join(repo, "s%d" % k)
                os.makedirs(top)
                with open(os.path.join(top, IGNORE_FILE), "wb") as fh:
                    fh.write(w_rules_text(sc).encode("utf-8"))
                for path, kind, answer in sc["paths"]:
                    if isinstance(answer, dict) and answer["refused"] != "core.ignorecase":
                        continue
                    full = os.path.join(top, *path.split("/"))
                    if kind == "dir":
                        os.makedirs(full, exist_ok=True)
                    elif kind == "link":
                        try:
                            w_link(target, full)
                        except OSError as exc:
                            return False, "a symbolic link cannot be made on this host (%s): no live comparison" % exc
                    asked.append((sc["name"], path, kind, answer, "s%d/%s" % (k, path)))
            said = {}
            for fold in ("false", "true"):
                p = run_git(["-C", repo, "-c", "core.ignorecase=" + fold, "-c",
                             "core.excludesFile=" + os.path.join(live, "no-excludes-file"), "check-ignore",
                             "--no-index", "-v", "-n", "-z", "--stdin"],
                            input=b"".join(os.fsencode(a[4]) + b"\0" for a in asked), capture_output=True)
                if p.returncode not in (0, 1):
                    return False, "git check-ignore exited %d: %s" % (
                        p.returncode, p.stderr.decode("utf-8", "replace").strip()[:160])
                f = p.stdout.split(b"\0")
                said[fold] = {os.fsdecode(f[i + 3]): (None if not f[i + 2] or f[i + 2].startswith(b"!")
                                                      else f[i + 2].decode("utf-8", "replace"))
                              for i in range(0, len(f) - 3, 4)}
            wrong = []
            for name, path, kind, answer, as_asked in asked:
                gf, gt = said["false"].get(as_asked, "unanswered"), said["true"].get(as_asked, "unanswered")
                mine = w_ask(TreeIgnores(tops[name]), path, kind)
                if isinstance(answer, dict):
                    if not isinstance(mine, dict) or gf == gt:
                        wrong.append("%s (%s): this reader %s, git %s under false and %s under true" % (
                            path, kind, w_render(mine), w_render(gf), w_render(gt)))
                elif isinstance(mine, dict) or not mine == gf == gt:
                    wrong.append("%s (%s): this reader %s, git %s under false and %s under true" % (
                        path, kind, w_render(mine), w_render(gf), w_render(gt)))
            return (asked and not wrong, "%d of %d path(s) answered otherwise than git: %s" % (
                len(wrong), len(asked), "; ".join(wrong)))
        pin("L30 LIVE: every scenario's paths are answered as git check-ignore answers them, under either "
            "setting of core.ignorecase", w30)

        def w31():
            named = sorted(s["name"] for s in w_table())
            unasked = sorted(set(named) - set(w_asked))
            twice = sorted({n for n in w_asked if w_asked.count(n) > 1})
            return not unasked and not twice and len(set(named)) == len(named), \
                "scenarios no arm asks: %r; asked by two arms: %r" % (unasked, twice)
        pin("L31 every scenario of the case table is asked by one arm", w31)

        # ── D: a path a command line names for a program to write (`written_path`) ──
        def p_refused(value, flag):
            ok, msg = _raises(lambda: written_path(value, flag), UsageRefusal)
            return ok and flag in msg, msg[:120]

        def p_taken(values, said):
            """The detail's first clause: the values `written_path` RETURNED where it must refuse, each spelt
            without quotes (an empty one as `empty`, a blank one as its count of spaces) -- what a red of D1 or D2
            names first is the values that got through, never only that something was returned."""
            def shown(v):
                return "empty" if v == "" else ("%d spaces" % len(v)) if not v.strip() else v
            taken = [shown(v) for v, (_, msg) in zip(values, said) if v is not None and msg.startswith("returned ")]
            return ("taken as a path to write: %s; " % ", ".join(taken)) if taken else ""

        def p1():
            asked = (("--help", "--out"), ("-h", "--json"), ("-", "--build-dir"))
            said = [p_refused(v, f) for v, f in asked]
            return all(ok for ok, _ in said), "%s%r" % (p_taken([v for v, _ in asked], said), [m for _, m in said])
        pin("D1 a path to write that begins with `-` is an option taken for a value: refused, naming the flag", p1)

        def p2():
            asked = ("", "   ", None)
            said = [p_refused(v, "--out") for v in asked]
            return all(ok for ok, _ in said), "%s%r" % (p_taken(asked, said), [m for _, m in said])
        pin("D2 a path to write that is empty names no place: refused, naming the flag", p2)

        def p3():
            kept = ("build/compile-bench/x-y", "./-x", "report.json", "a-b")
            got = [written_path(v, "--out") for v in kept]
            return got == list(kept), "got %r" % (got,)
        pin("D3 a path that holds a dash, or spells a leading one as `./-x`, is kept as given (the control)", p3)

        # The two arms below parse with a FIXTURE parser. Its option is composed, never a literal: an
        # `add_argument` literal is read by the scripts index's verb census as an option THIS program's own
        # command line offers, and this one is no option of owning-tree's.
        fixture_flag = "--" + "out"

        def fixture_parser(prog):
            import argparse
            parser = argparse.ArgumentParser(prog=prog)
            parser.add_argument(fixture_flag, type=written_path_arg(fixture_flag))
            return parser

        def p4():
            import contextlib
            import io

            def parsed(argv):
                parser = fixture_parser("d4")
                err = io.StringIO()
                try:
                    with contextlib.redirect_stderr(err):
                        return parser.parse_args(argv).out, 0, ""
                except SystemExit as exc:
                    return None, exc.code, err.getvalue()
            said = [(argv, parsed(argv)) for argv in (["--out=--help"], ["--out", "-"], ["--out="])]
            wrong = ["%s exit %s" % (" ".join(a), r[1]) for a, r in said
                     if r[1] != 2 or "--out" not in r[2] or "path to write" not in r[2]]
            return not wrong, "not refused by the type: %s" % "; ".join(wrong)
        pin("D4 written_path_arg: argparse refuses a path to write that begins with `-`, in either spelling, and an "
            "empty one, by its usage error naming the flag", p4)

        def p5():
            import contextlib
            import io
            parser = fixture_parser("d5")
            got = []
            for argv in (["--out", "build/x-y"], ["--out=./-x"]):
                try:
                    with contextlib.redirect_stderr(io.StringIO()):
                        got.append(parser.parse_args(argv).out)
                except SystemExit as exc:
                    got.append("refused, exit %s" % exc.code)
            return got == ["build/x-y", "./-x"], "got %s" % ", ".join(got)
        pin("D5 written_path_arg: a path holding a dash, or spelling a leading one as `./-x`, passes the type as given "
            "(the control)", p5)

        def p6():
            asked = (("--help", "--tree"), ("-", "--manifest"), ("", "--dss"), (None, "--kit"))
            said = [(lambda v=v, f=f: _raises(lambda: read_path(v, f), UsageRefusal))() for v, f in asked]
            wrong = ["%r (%s)" % (v, m[:60]) for (v, f), (ok, m) in zip(asked, said) if not ok or f not in m]
            kept = [read_path(v, "--tree") for v in ("build/x-y", "./-x", "cc")]
            return not wrong and kept == ["build/x-y", "./-x", "cc"], "not refused naming the flag: %s; kept %r" % (
                "; ".join(wrong), kept)
        pin("D6 read_path: a path to read that begins with `-` or is empty is refused naming the flag; a dashed one, "
            "`./-x` and a bare program name are kept (the control)", p6)

        def p7():
            refused = ("-x", "", "/abs", "\\x", "C:\\x", "c:rel", "a/../b", "..", "a\\..\\b")
            said = [(v, _raises(lambda v=v: path_part(v, "--part"), UsageRefusal)) for v in refused]
            wrong = ["%r (%s)" % (v, m[:60]) for v, (ok, m) in said if not ok or "--part" not in m]
            kept_in = ("x86_64:pe64-x86_64-windows-exec", "a/b.c", "./-x", "8.6", "a..b")
            kept = [path_part(v, "--part") for v in kept_in]
            return not wrong and kept == list(kept_in), "not refused naming the flag: %s; kept %r" % (
                "; ".join(wrong), kept)
        pin("D7 path_part: a part that begins with `-`, is empty, is absolute or on a drive, or holds a `..` component "
            "is refused naming the flag; a name with `:` or `..` inside it, a relative path and `./-x` are kept (the "
            "control)", p7)

        def parsed_with(kind, argv, **options):
            import argparse
            import contextlib
            import io
            parser = argparse.ArgumentParser(prog="d8")
            parser.add_argument(fixture_flag, type=kind(fixture_flag, **options))
            err = io.StringIO()
            try:
                with contextlib.redirect_stderr(err):
                    return parser.parse_args(argv).out, 0, ""
            except SystemExit as exc:
                return None, exc.code, err.getvalue()

        def p8():
            wrong = []
            for kind, phrase in ((read_path_arg, "path to read"), (path_part_arg, "part of a path")):
                for argv in (["--out=--help"], ["--out", "-"], ["--out="]):
                    got = parsed_with(kind, argv)
                    if got[1] != 2 or "--out" not in got[2] or phrase not in got[2]:
                        wrong.append("%s %s exit %s" % (kind.__name__, " ".join(argv), got[1]))
                got = parsed_with(kind, ["--out", "name-x"])
                if got[0] != "name-x":
                    wrong.append("%s refused the control: %r" % (kind.__name__, got))
            return not wrong, "; ".join(wrong)
        pin("D8 read_path_arg / path_part_arg: argparse refuses a value that begins with `-`, in either spelling, "
            "and an empty one, by its usage error naming the flag; a plain name passes (the control)", p8)

        def p9():
            import argparse
            import contextlib
            import io
            wrong = []
            for kind in (read_path_arg, written_path_arg, path_part_arg):
                parser = argparse.ArgumentParser(prog="d9")
                parser.add_argument(fixture_flag, type=kind(fixture_flag, empty_means_absent=True), default="")
                with contextlib.redirect_stderr(io.StringIO()):
                    try:
                        omitted, given = parser.parse_args([]).out, parser.parse_args(["--out", ""]).out
                    except SystemExit as exc:
                        omitted = given = "refused, exit %s" % exc.code
                    try:
                        parser.parse_args(["--out=-x"])
                        dashed = "kept"
                    except SystemExit:
                        dashed = "refused"
                if (omitted, given, dashed) != ("", "", "refused"):
                    wrong.append("%s: omitted %r, given '' %r, `-x` %s" % (kind.__name__, omitted, given, dashed))
                strict = argparse.ArgumentParser(prog="d9")
                strict.add_argument(fixture_flag, type=kind(fixture_flag), default="")
                with contextlib.redirect_stderr(io.StringIO()):
                    try:
                        strict.parse_args([])
                        wrong.append("%s without empty_means_absent kept an empty default" % kind.__name__)
                    except SystemExit:
                        pass
            return not wrong, "; ".join(wrong)
        pin("D9 empty_means_absent: an empty value -- the default argparse runs the type over, or one given -- passes "
            "as '', `-x` is still refused, and without it the same empty default is refused (the pair)", p9)

        def p10():
            wrong = []
            for argv in (["--out=--help"], ["--out", "-"], ["--out=-1"]):
                got = parsed_with(text_arg, argv)
                if got[1] != 2 or "--out" not in got[2] or "begins with `-`" not in got[2]:
                    wrong.append("%s exit %s" % (" ".join(argv), got[1]))
            kept = [parsed_with(text_arg, argv)[0] for argv in (["--out", "leg-x"], ["--out="], ["--out", "a b"])]
            if kept != ["leg-x", "", "a b"]:
                wrong.append("kept %r" % (kept,))
            converted = parsed_with(text_arg, ["--out=7"], then=int)
            refused_first = parsed_with(text_arg, ["--out=-7"], then=int)
            if converted[0] != 7 or refused_first[1] != 2 or "begins with `-`" not in refused_first[2]:
                wrong.append("then: %r / %r" % (converted, refused_first))
            return not wrong, "; ".join(wrong)
        pin("D10 text_arg: a text that begins with `-` is refused naming the flag, in either spelling, BEFORE the "
            "program's own conversion (`then`) sees it; a word with a dash inside, an empty one and one with a space "
            "are kept, and `then` converts what passed (the control)", p10)

        def p11():
            import argparse
            import contextlib
            import io
            wrong = []
            match_flag = "--" + "match"     # spelled so: clause 14 reads a literal flag as a program's own argument
            for kind in (option_text_arg, command_words_arg):
                parser = argparse.ArgumentParser(prog="d11")
                parser.add_argument(match_flag, type=kind(match_flag))
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    try:
                        parser.parse_args(["--match", "-O2"])
                        separate = "kept"
                    except SystemExit as exc:
                        separate = "exit %s" % exc.code
                    joined = parser.parse_args(["--match=-O2"]).match
                if separate != "exit 2" or "expected one argument" not in err.getvalue() or joined != "-O2":
                    wrong.append("%s: `--match -O2` %s, `--match=-O2` -> %r" % (kind.__name__, separate, joined))
            return not wrong, "; ".join(wrong)
        pin("D11 option_text_arg / command_words_arg: a value beginning with `-` handed as a SEPARATE word is refused "
            "by argparse itself (`--match -O2`: expected one argument), and the `=` form keeps it as written "
            "(`--match=-O2`) -- the one spelling a caller writes on purpose", p11)

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
