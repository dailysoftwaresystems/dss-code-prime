#!/usr/bin/env python3
# PURPOSE: seed a lane worktree from the main tree, fold only that lane's real changes back, and land it with its rows applied and its evidence preserved.
"""lane-fold.py -- SEED a lane worktree, FOLD exactly what that lane changed, then LAND it.

★★★ WHY THIS IS A REPOSITORY SCRIPT AND NOT A SCRATCH FILE. Every cycle that runs
parallel lanes needs these verbs, and every cycle before 2026-08-29 rewrote them
from scratch in a session-scoped scratchpad that does not survive the session. The
`/dss-cycle` handoff of 2026-08-28 recorded that waste explicitly and left the
promotion as a decision to take; the retired WSL leg driver's own header recorded the
identical waste happening three times inside ONE session. The operator's standing rule
is *"if a tool has a problem, fix before using again, not workaround an own tool.
reusable tools exists to avoid bunch of problems like mangling or edge cases"* -- and a
tool that is retyped every session re-opens every edge case below at once.
⚠ AND THE SAME HAPPENED AGAIN ONE STEP LATER, IN P66: the steps AFTER a fold -- apply
the lane's rows, re-read them, keep the lane's evidence, remove the worktree -- lived in
two session-scratchpad scripts (`land-lane.sh`, `apply-lane-rows.sh`) with the repository
root hard-coded. See LANDING below; they are the `apply-rows` and `land` verbs now.

────────────────────────────────────────────────────────────────────────────────
THE PROBLEM THE `seed` VERB SOLVES, and it is not "copy some files"

A lane worktree is created by `git worktree add` at a COMMIT. When a cycle lands its
first set of lanes into the main tree WITHOUT committing yet, the next set of lanes
must start from that landed state -- otherwise each lane rediscovers a conflict that
was already resolved. So the seed copies the main tree's uncommitted state in.

⚠ AND THAT IS PRECISELY WHAT MAKES THE FOLD DANGEROUS. A seeded worktree reports every
seeded path as "modified" in its own `git status`, because its HEAD does not carry
them. A fold driven by that status would copy all of them back over the main tree --
silently REVERTING whatever a SIBLING lane landed in the meantime -- and report
success. ✔That is not hypothetical: it is why the manifest exists.

⇒ **A LANE'S REAL CONTRIBUTION IS A MEASUREMENT, NOT AN ASSUMPTION:**

    (its `git status` set)  MINUS  (seeded paths whose md5 is UNCHANGED from the seed)

The seed therefore records an md5 per seeded path, and the fold subtracts.

────────────────────────────────────────────────────────────────────────────────
THE MANIFEST, and the one fact it records beside the paths

`.worktrees/.manifests/seed-<lane>.json` is FORMAT 2:

    {"base": "<the commit the lane's worktree was created at>", "format": 2,
     "paths": {"<seeded path>": "<md5 at hand-off>", ...}}

`lane-worktree add` writes it with no paths; `seed` rewrites it with the
paths it carried in. A FORMAT-1 manifest -- the flat `{path: md5}` map every lane carried
before P66 -- is still read: its base is taken from the lane's HEAD, and the fold says so.

────────────────────────────────────────────────────────────────────────────────
THE FOLD'S REFUSALS -- each one is a way a fold can destroy work while looking clean

  * the destination's CURRENT md5 differs from the SEED md5 -> something changed it
    since seeding (a sibling lane's fold, or the orchestrator), and writing would
    destroy that change UNSEEN. Refuse the whole fold; nothing is written.
  * a path absent from the manifest is measured against the blob at the LANE'S OWN BASE
    COMMIT -- the commit the manifest records -- read from the lane's repository.
    ⚠ ABSENT FROM THE MANIFEST DOES NOT MEAN NEW -- the manifest holds only paths that
    differed from HEAD at seeding time, so an ordinary tracked file no prior lane
    touched is simply not in it. An earlier draft called those "lane-new" and refused
    four of a lane's honestly-modified files.
    ⚠⚠ AND "THE HEAD BLOB" WAS THE WRONG HEAD UNTIL P66. It was read from the MAIN
    tree's HEAD at fold time, so once a sibling lane's fold was COMMITTED, the main
    tree's bytes equalled that HEAD again and the check passed: the second lane's copy
    -- older than the commit -- was written straight over it. ✔REPRODUCED 2026-09-15 in
    a throwaway repository: two lanes created at one commit, the first fold committed,
    and the second fold exited 0 having OVERWRITTEN one of the first lane's edits and
    DELETED another -- the copy branch and the deletion branch both read `HEAD:`.
    Uncommitted, the same sequence refused loudly, which is exactly why it looked safe.
    A drift refusal now also says HOW the main tree moved -- by a COMMIT, or by an
    UNCOMMITTED EDIT -- because the two are reconciled differently.
  * the lane's HEAD is not the base its manifest records -> a COMMIT inside the lane
    hides its changes from `git status`, the only list a fold reads, so folding would
    carry part of the lane and silently drop the rest. Refused.
  * a path that ESCAPES the repository, compared by RESOLVED PATH PREFIX, never by
    substring. (A substring test accepts `C:/Source/.../dss-code-prime-evil/x`.)

★ A LANE PATH THE MAIN TREE ALREADY HOLDS BYTE-IDENTICALLY IS ALREADY LANDED, not drift.
Writing it would change nothing, so nothing can be lost by skipping it -- and without
this a landing that failed after its fold could never be re-run: the second fold would
refuse its own first write. ✔REPRODUCED with the scratchpad landing script in P66:
"untracked at HEAD yet present in the main tree: work-l5.txt".

★ REFUSALS ARE ALL-OR-NOTHING AND ARE COLLECTED BEFORE ANYTHING IS WRITTEN. A fold
that writes nine files and then refuses the tenth leaves a tree nobody can reason
about; a fold that refuses before writing leaves a tree that is still exactly what it
was.

★ AND A COLLISION IS SUPPOSED TO FAIL LOUD. When two lanes edit ONE shared document
(`src/dss-config/sources/c.lang.json` is the usual one), the second fold REFUSES with
`main tree DRIFTED`. That is the designed behaviour: the orchestrator then merges the
second lane's declared keys by hand. Silently overwriting would revert the first lane
with no diff to show for it.

────────────────────────────────────────────────────────────────────────────────
TWO SMALLER TRAPS, both ✔MEASURED and both worth keeping

⚠ `git status --porcelain` C-QUOTES a path that needs quoting, and this repository
holds one: `.plans/23-full-c-plan - tbd.md` (spaces). Three predecessor instruments
read the path as `line[3:]`, so the seeder tried to create a directory literally named
`".plans` and died. `-z` is the FIX rather than an unquoter: with `-z` git emits
NUL-separated records and NEVER quotes, so no quoting convention is left to
reimplement. ⓘ `core.quotePath=false` would NOT have sufficed -- it governs non-ASCII
bytes, not the quoting a space triggers. On earlier seeds the naive parse did not even
error: it SILENTLY OMITTED that path.

⚠ THE MANIFEST LIVES UNDER `.worktrees/`, NOT IN A SCRATCHPAD. `/.worktrees/` is
gitignored (so the manifest never travels to a gate host and never lands in a commit),
it is repo-relative (so a new session finds it), and it outlives the lane directory it
describes -- which matters because `lane-worktree remove` deletes the lane.

────────────────────────────────────────────────────────────────────────────────
LANDING -- `apply-rows` and `land`

A lane is not done when it reports; it is done when its work is FOLDED, its registry
rows are APPLIED, and only then is its worktree removed ("complete means folded",
operator, 2026-08-28). Those steps lived in a session scratchpad, and each defect below
was ✔REPRODUCED against copies of those scripts in a throwaway repository (P66):

  * every evidence file except `findings.md` died with the worktree -- mutant
    transcripts, gate logs, row cells;
  * a row whose cells had no `.status` was never examined, and died with the worktree;
  * a `.status` the script could not parse was silently written as OPEN;
  * a `.bucket` on an existing row, and a `.priority` on a new one, were silently ignored;
  * a later row the WRITER refused left the earlier rows written -- a partial
    application, after the fold had already landed the lane's code;
  * and a corrected re-run then refused at the fold, which could not recognise its own
    earlier write.

The rules the two verbs encode:

  * ALL OR NOTHING. `row/` is parsed strictly -- every entry is `<ANCHOR>.<status |
    trigger | closing | crossrefs | bucket | priority>`, and every anchor has all four of
    status, trigger, closing and crossrefs -- and EVERY row is dry-run through the door
    before ANY is written. A write that still fails RESTORES the registries byte-for-byte.
  * THROUGH THE ONE DOOR, CELLS BY FILE: `dssharness write-anchor` for a new row and
    `set-anchor` for an existing one, composed by the target tree's launcher
    (`anchors.door_write`), never a hand-assembled row. An update names only the fields that
    CHANGE, so a cell the lane did not change keeps its stored bytes. The status and
    priority vocabularies are that file's own, loaded, never re-typed here; a NEW row with no
    `.priority` is given the burndown sieve's band (the door requires one), and says so.
  * BUCKETS: a row that exists keeps its own home, and a `.bucket` contradicting it
    REFUSES; an ARCHIVED row needs an explicit `.bucket`, naming its archive table; a
    NEW row takes its `.bucket` or the verb's default. `.priority` wins over the stored
    band; otherwise the stored band is carried forward.
  * A row whose status, priority, home and cells already equal the declaration is
    ALREADY LANDED and is not written again -- so a landing can be re-run.
  * EVERY ROW IS RE-READ after the write and compared field by field; the writer's exit
    code is not taken as proof.
  * REMOVAL IS GATED on all of that AND on the fold re-measuring as "nothing left to
    fold", and it is done by `lane-worktree remove --discard-work --preserve-to`. The
    discard flag is built from that measurement and nothing else -- `lane-worktree`
    refuses a worktree whose own `git status` lists uncommitted work without it -- and the
    preserve keeps the lane's WHOLE evidence set (`scratchpad/` and `.temp/`) and re-reads
    every file; this
    verb then re-checks each file's md5 at the destination itself. The default
    destination is `.worktrees/.evidence/<lane>-<UTC stamp>/` -- ignored, repo-relative,
    and outliving the lane, like `.manifests/`.

Every write is write-temp + `os.replace`. This script NEVER runs a git write verb,
never stages, and never touches `.git/`. (`land` asks `lane-worktree`, the one owner of
removal, to remove the worktree; it does not remove anything itself.)

Exit codes: 0 OK · 2 refused (nothing written, or -- for `land` -- stopped with the
worktree kept, and the message says what already landed) · 3 usage error.

⚠ THE TREE ACTED ON IS THE ONE THIS SCRIPT LIVES IN, never the caller's cwd (a
cwd-keyed root was a measured defect here); see `repo_root`. `--repo <path>`
names another tree deliberately, and works with every verb. And NO git question this
tool asks, nor any process it starts, is answered in the caller's git environment: the
root and `git_run` come from `.harness-config/runner/actions/owning-tree/owning-tree.py`.

Usage:
    python .harness-config/runner/actions/lane-fold/lane-fold.py seed <lane>           # carry the main
                                          #   tree's uncommitted state into the lane
    python .harness-config/runner/actions/lane-fold/lane-fold.py seed <lane> --empty   # created at HEAD and
                                          #   given nothing: record the manifest only
    python .harness-config/runner/actions/lane-fold/lane-fold.py fold <lane> [--apply]     # dry run without --apply
    python .harness-config/runner/actions/lane-fold/lane-fold.py refresh-plans <lane> [--apply]
                                          #   re-copy .plans/ into a LIVE lane and
                                          #   update its manifest, so a row applied
                                          #   mid-cycle stops reddening its guards
    python .harness-config/runner/actions/lane-fold/lane-fold.py apply-rows <lane> <production> [--apply]
                                          #   apply the lane's row/ cells, all or nothing
    python .harness-config/runner/actions/lane-fold/lane-fold.py land <lane> <production> [--apply]
                                          [--settled <path>]... [--preserve-to <dir>]
                                          #   fold, apply rows, verify both, keep the
                                          #   evidence, THEN remove the worktree
    python .harness-config/runner/actions/lane-fold/lane-fold.py list
    python .harness-config/runner/actions/lane-fold/lane-fold.py --self-test
"""
from __future__ import annotations

import collections
import contextlib
import datetime
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

# ── OUTPUT ENCODING ────────────────────────────────────────────────────────────
# Under ctest both streams are PIPES, and on Windows that brings the console
# codepage up as the encoding -- so a path or a message carrying a non-ASCII byte
# raises UnicodeEncodeError from inside a print, which reads as a tool crash rather
# than as the encoding fact it is. Reconfigure both, at import, before anything can
# print.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):  # pragma: no cover - very old interpreters
        pass

# Sibling programs are loaded by path below (owning-tree, lane-worktree); a by-path load writes
# bytecode beside its source unless told not to, and this action is `requireInputsUnmoved`.
sys.dont_write_bytecode = True

# Where this repository's programs live, relative to a tree root: DssHarness's actions
# directory (`dssharness help layout`). The row writer a fold drives is the TARGET tree's
# copy, found here. Spelled ONCE in this file.
ACTIONS_REL = os.path.join(".harness-config", "runner", "actions")
# ★ WHERE THE LANES, THE SEED MANIFESTS AND THE KEPT EVIDENCE LIVE IS READ, NEVER SPELLED HERE:
# `layout(root)`. Until 2026-09-23 this file hard-coded `.worktrees`, `.worktrees/.manifests`,
# `.worktrees/.evidence` and the evidence roots, while `lane-worktree` read them from the
# configuration -- one edit there and the two tools would look in different places.
# This program's own configuration: the two bookkeeping directories beside the lanes.
LANE_FOLD_CONFIG_REL = os.path.join(ACTIONS_REL, "lane-fold", "lane-fold-config.json")
LANE_FOLD_CONFIG_KEYS = ("manifests", "evidence")
# `worktrees`, `manifests`, `evidence`: repository-relative directories, forward-slashed;
# `evidence_roots`: the lane-relative directories a lane keeps evidence in.
Layout = collections.namedtuple("Layout", "worktrees manifests evidence evidence_roots")

MANIFEST_FORMAT = 2
_SHA = re.compile(r"^[0-9a-f]{40}(?:[0-9a-f]{24})?$")
_MD5 = re.compile(r"^[0-9a-f]{32}$")

ROW_SUFFIXES = ("status", "trigger", "closing", "crossrefs", "bucket", "priority")
REQUIRED_CELLS = ("status", "trigger", "closing", "crossrefs")
ROW_CELL = re.compile(r"^(.+)\.(%s)$" % "|".join(ROW_SUFFIXES))


def die(msg, code=2):
    print("lane-fold: REFUSED -- %s" % msg)
    sys.exit(code)


_OWNING_TREE = None


def _owning_tree():
    """`.harness-config/runner/actions/owning-tree/owning-tree.py` -- the ONE owner of "which tree is this file in?" and
    of asking git about a tree without the caller's git environment.

    Loaded by path from this file's sibling directory (a hyphen is not a module name), once. It
    FAILS LOUD when absent rather than falling back to a local spelling: a second copy of either
    answer is the drift that owner exists to end.
    """
    global _OWNING_TREE
    if _OWNING_TREE is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                            "owning-tree", "owning-tree.py")
        if not os.path.isfile(path):
            die("cannot find %s -- this tool's root, and every git question it asks, are "
                "resolved there and nowhere else." % path, 3)
        spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _OWNING_TREE = mod
    return _OWNING_TREE


_LANE_WORKTREE = None


def _lane_worktree():
    """`../lane-worktree/lane-worktree.py`, this file's sibling action, loaded by path once: the ONE
    reader of where a tree keeps its lanes (`load_settings`), which `lane-worktree` runs for every
    verb itself. Missing, it is a refusal, never a local respelling of what it reads."""
    global _LANE_WORKTREE
    if _LANE_WORKTREE is None:
        path = lane_worktree_path()
        if not os.path.isfile(path):
            die("cannot find %s -- where this tree keeps its lanes is read there and nowhere "
                "else." % path, 3)
        spec = importlib.util.spec_from_file_location("dss_lane_worktree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _LANE_WORKTREE = mod
    return _LANE_WORKTREE


def layout(root):
    """-> Layout of the tree at `root`, READ each time, with no default anywhere.

    `worktrees` and `evidence_roots` are DssHarness's `worktrees.root` and
    `worktrees.evidenceRoots` in `<root>/.harness-config/config.json`; `manifests` and
    `evidence` are the two keys of this program's own `lane-fold-config.json` in the same tree,
    each ONE dot-led name beside the lanes. The config.json keys and `manifests` come through
    lane-worktree's `load_settings`, the reader `lane-worktree` itself runs -- so the
    directory `add` writes a seed into is, by construction, the one `fold` reads -- and
    `evidence` through the name rule that reader applies to `manifests`. A missing, ill-typed
    or unknown key, or two keys naming one directory, is refused with exit 2."""
    lw = _lane_worktree()
    cfg_path = os.path.join(root, LANE_FOLD_CONFIG_REL)
    try:
        s = lw.load_settings(root)
        cfg = _owning_tree().load_jsonc(cfg_path)
    except lw.Refused as exc:
        die(str(exc), 2)
    except _owning_tree().Refusal as exc:
        die("cannot read lane-fold's configuration: %s" % exc, 2)
    if not isinstance(cfg, dict):
        die("%s is not a JSON object." % cfg_path, 2)
    unknown = sorted(k for k in cfg if k not in LANE_FOLD_CONFIG_KEYS and k != "$comment")
    if unknown:
        die("%s: unknown key(s) %s -- the keys are %s (and `$comment`); a misspelt key would "
            "otherwise be ignored while its value is taken from nowhere."
            % (cfg_path, ", ".join(unknown), ", ".join(LANE_FOLD_CONFIG_KEYS)), 2)
    if "evidence" not in cfg:
        die("%s: evidence is MISSING -- the directory `land` keeps a removed lane's evidence in "
            "is read from here, never assumed." % cfg_path, 2)
    try:
        evidence = lw.bookkeeping_name(cfg_path, "evidence", cfg["evidence"])
    except lw.Refused as exc:
        die(str(exc), 2)
    if os.path.normcase(evidence) == os.path.normcase(s.manifests):
        die("%s: manifests and evidence both name '%s' -- the seed manifests and a removed lane's "
            "evidence would share one directory, and `list` could not tell them apart."
            % (cfg_path, evidence), 2)
    worktrees = "/".join(s.root)
    return Layout(worktrees, worktrees + "/" + s.manifests, worktrees + "/" + evidence,
                  tuple("/".join(r) for r in s.evidence_roots))


def _rel_path(root, rel):
    """`root` joined with a forward-slashed repository-relative path."""
    return os.path.join(root, *rel.split("/"))


def git_run(args, **kwargs):
    """`git <args>` WITHOUT the caller's git environment -- `owning-tree.py`'s `run_git`, the one
    owner of that stripping; this is only the name every call below uses.

    ✔MEASURED 2026-09-15 (P66 round 3) before it existed, in a throwaway box: under a caller's
    GIT_DIR + GIT_WORK_TREE naming another repository, `list` printed THAT repository's lanes,
    and this file's self-test crashed after committing into that other repository.
    """
    return _owning_tree().run_git(args, **kwargs)


def repo_root(anchor=None):
    """The root of the working tree that CONTAINS THIS SCRIPT (or `anchor`).

    ⚠ A predecessor hardcoded `C:\\Source\\DailySoftware\\dss-code-prime`, which makes
    the tool unusable from a worktree, from any clone, and on every non-Windows leg.

    ★★★ AND ITS REPLACEMENT -- a bare `git rev-parse --show-toplevel` -- TRADED THAT
    FOR A SUBTLER WRONG ANSWER: a CWD-KEYED root.
    A bare `rev-parse` answers "what repository is my CALLER'S SHELL in?", so `wt`,
    `mpath`, every `os.path.join(root, rel)` a fold WRITES to, and every path it
    REMOVES were rooted at whichever repository somebody happened to have cd'd into.
    ✔MEASURED 2026-09-02, live, on this repository's own orchestrator: a shell that
    had drifted into `.worktrees/io` ran the MAIN tree's copy of this script, and
    `fold io --apply` refused with
        no worktree at <repo>\\.worktrees\\io\\.worktrees\\io
    -- the loud direction, by luck of the doubled path. ✔MEASURED the same day from a
    throwaway repository outside the checkout: `list` reported THAT repository's
    `.worktrees/`, which is the quiet direction, and a `fold` from there would have
    measured one tree and written into another.

    ★ THE QUESTION IS "WHICH TREE DOES MY OWN FILE BELONG TO?" -- `__file__`, not
    `os.getcwd()`. `$PWD` is a property of the caller's shell; the script's path is a
    property of the script, and only the second survives a `cd`. In the measured
    incident this is exactly right: the orchestrator invoked the MAIN tree's copy, so
    `__file__` names the main checkout no matter where the shell had wandered.
    The rejected alternative -- "the MAIN checkout, because only it owns
    `.worktrees/`" -- is recorded with its measurement in `lane-worktree.py`'s
    `repo_root`; briefly, from a lane it aims a removal at a live SIBLING lane, and
    for a submodule checkout it names a directory inside `.git`.

    ★★ AND ONE OWNER ANSWERS IT FOR EVERY PYTHON SCRIPT: `.harness-config/runner/actions/owning-tree/owning-tree.py`
    walks up from `__file__` to the nearest directory holding `.plans/` and `.harness-config/`, and
    asks git -- WITHOUT the caller's git environment -- whether its top level is that tree,
    because this tool reads everything through git. ✔MEASURED 2026-09-15 (P66 round 3), in a
    throwaway box, on the spelling this replaced (`git -C <this directory> rev-parse
    --show-toplevel` in the caller's environment): from another repository's cwd and from no
    repository it already named its own tree, but a caller's GIT_DIR made the root THIS
    SCRIPT'S DIRECTORY, and GIT_DIR + GIT_WORK_TREE made it ANOTHER repository, whose lanes
    `list` then printed. `--repo <path>` still names another tree deliberately: git's top level
    for that path, asked the same unsteered way. Self-test arms (i0)-(i4) and (j).
    """
    ot = _owning_tree()
    if anchor is None:
        try:
            return os.path.realpath(ot.resolve(__file__, reads_git=True))
        except ot.Refusal as exc:
            die("%s\n  pass --repo <path> to name a different tree deliberately." % exc, 3)
    top, why = ot.git_top_level(anchor)
    if top is None:
        die("no git working tree contains %s (%s).\n"
            "  pass --repo <path> naming a directory inside the tree to act on." % (anchor, why), 3)
    return os.path.realpath(top)


def changed_paths(cwd):
    """Every changed path under `cwd`, expanded through directories, forward-slashed.

    Records are `XY <path>`; a rename or copy adds a SECOND record holding the source
    path, which is why the loop takes RECORDS rather than pairs -- both spellings are
    paths a fold has to reason about.
    """
    out = git_run(["-C", cwd, "status", "--porcelain", "-z"],
                  capture_output=True, check=True).stdout.decode(
                      "utf-8", "surrogateescape")
    paths = []
    for rec in out.split("\0"):
        if not rec:
            continue
        # 'XY PATH'; a rename's SOURCE record arrives bare, so only strip the status
        # prefix when it is actually present.
        p = rec[3:] if len(rec) > 3 and rec[2] == " " else rec
        p = p.strip()
        if not p:
            continue
        base = os.path.join(cwd, p)
        if p.endswith("/") or os.path.isdir(base):
            # ⚠⚠ AN UNTRACKED DIRECTORY IS ENUMERATED BY GIT, NEVER BY `os.walk`.
            # `git status --porcelain` reports a bare directory only when it is
            # UNTRACKED, and the question that has to be asked of its contents is
            # "untracked AND NOT IGNORED" -- which is exactly
            # `ls-files --others --exclude-standard`. A walk answers a DIFFERENT
            # question (every file on disk) and answers it in the direction that
            # fails toward folding MORE than the lane did.
            # ✔MEASURED 2026-08-29 on the live fold of lane `cx`: the walk offered
            # `.harness-config/runner/actions/lane-fold/__pycache__/*.pyc` as the lane's own work, because
            # the script directory is new and therefore untracked, and `.pyc` is
            # gitignored but the walk never asked. A fold would have written build
            # artefacts into the main tree, walking an untracked directory past
            # `.gitignore`.
            # ⓘ This does NOT make the `is_lane_tree` floor below redundant, and the
            # two must not be confused: git's answer is about what is IGNORED, the
            # floor is about the worktree machinery, and a repository whose
            # `.gitignore` lost its `/.worktrees/` line would have git list a
            # sibling lane as plain untracked work.
            listed = git_run(
                ["-C", cwd, "ls-files", "--others", "--exclude-standard",
                 "-z", "--", p.rstrip("/")],
                capture_output=True, check=True).stdout.decode(
                    "utf-8", "surrogateescape")
            for q in listed.split("\0"):
                if q:
                    paths.append(q.replace("\\", "/"))
        else:
            paths.append(p.replace("\\", "/"))
    return paths


def is_lane_tree(rel, lanes):
    """A path that belongs to the worktree machinery itself: git's own, or anything under
    `lanes`, the tree's worktrees root (`layout(root).worktrees`). -> bool

    ⚠⚠ THIS FLOOR IS NOT REDUNDANT WITH `.gitignore`, AND THE SELF-TEST BELOW IS WHAT
    PROVED IT. `/.worktrees/` is gitignored in this repository, so `git status` in the
    main tree does not list a lane -- which means the predecessor of this script was
    safe purely BY ACCIDENT OF A FILE IT DOES NOT OWN. The self-test builds a
    temporary repository with no `.gitignore` at all, and the seeder immediately tried
    to copy 26 of `.worktrees/x/.git/**` plus every other lane's checkout INTO the
    lane it was seeding.
    ⇒ The failure mode if that ignore line is ever edited away is not a red gate: it is
    every lane silently receiving a full copy of every sibling lane, `.git` included,
    which then folds back. So the exclusion is asserted HERE, where the copy happens,
    exactly as the retired carriage-exclude derivation pinned `.worktrees/` in its own
    MUST_NEVER_TRAVEL floor rather than trusting the same line.
    """
    return rel == lanes or rel.startswith(lanes + "/") \
        or rel == ".git" or rel.startswith(".git/")


def md5_file(path):
    with io.open(path, "rb") as fh:
        return hashlib.md5(fh.read()).hexdigest()


def inside(root, path):
    """Resolved-PREFIX containment. Never a substring test."""
    return (os.path.realpath(path) + os.sep).startswith(root + os.sep)


def manifest_path(root, lane):
    return os.path.join(_rel_path(root, layout(root).manifests), "seed-%s.json" % lane)


def worktree_path(root, lane):
    return os.path.join(_rel_path(root, layout(root).worktrees), lane)


def write_atomic(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".lane-fold-tmp"
    with io.open(tmp, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    os.replace(tmp, path)


def copy_atomic(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + ".lane-fold-tmp"
    shutil.copyfile(src, tmp)
    os.replace(tmp, dst)


# ─────────────────────────────── base and manifest ─────────────────────────────

def _blob_ids(repo, rev, rels):
    """-> {rel: the id of the blob commit `rev` holds at `rel` in `repo`, or None when it holds
    no blob there}. ONE `git cat-file --batch-check` process for every path.

    ★ IDS, NEVER RAW BYTES: the other side of every comparison is `_worktree_ids`, which hashes
    a working file through the clean filters `git add` would apply -- see there for why.
    """
    rels = list(rels)
    if not rels:
        return {}
    request = "".join("%s:%s\n" % (rev, rel) for rel in rels)
    proc = git_run(["-C", repo, "cat-file", "--batch-check=%(objecttype) %(objectname)"],
                   input=request.encode("utf-8", "surrogateescape"), capture_output=True)
    lines = proc.stdout.decode("utf-8", "surrogateescape").splitlines()
    if proc.returncode != 0 or len(lines) != len(rels):
        die("`git cat-file --batch-check` in %s answered %d line(s) for %d path(s) (rc=%d): %s"
            % (repo, len(lines), len(rels), proc.returncode,
               proc.stderr.decode("utf-8", "replace").strip()))
    ids = {}
    for rel, line in zip(rels, lines):
        kind, _, name = line.partition(" ")
        ids[rel] = name if kind == "blob" and _SHA.match(name) else None
    return ids


def _worktree_ids(repo, rels):
    """-> {rel: the blob id git would store for the working file `repo/rel`}. ONE process.

    ⚠⚠ NOT AN md5 OF THE FILE, AND THIS IS WHY. A checked-out file is not its blob: under
    `core.autocrlf=true`, or a `text`/`eol` attribute, the working tree holds CRLF where the
    blob holds LF, so the raw bytes differ while git itself calls the file unchanged. Compared
    by raw bytes, every such path read as DRIFTED -- a false refusal on any host whose git
    converts line endings. Found by an independent review in P66 (every self-test fixture
    pinned autocrlf off, so nothing had exercised it) and pinned by arms (t1)-(t3), which turn
    it on. `git hash-object --stdin-paths` hashes each file through the clean filters its OWN
    path selects -- the comparison `git status` makes.
    """
    rels = list(rels)
    if not rels:
        return {}
    request = "".join("%s\n" % rel for rel in rels)
    proc = git_run(["-C", repo, "hash-object", "--stdin-paths"],
                   input=request.encode("utf-8", "surrogateescape"), capture_output=True)
    lines = proc.stdout.decode("utf-8", "surrogateescape").splitlines()
    if (proc.returncode != 0 or len(lines) != len(rels)
            or not all(_SHA.match(line) for line in lines)):
        die("`git hash-object --stdin-paths` in %s answered %d line(s) for %d path(s) (rc=%d): %s"
            % (repo, len(lines), len(rels), proc.returncode,
               proc.stderr.decode("utf-8", "replace").strip()))
    return dict(zip(rels, lines))


def _head(repo):
    out = git_run(["-C", repo, "rev-parse", "--verify", "HEAD"], capture_output=True)
    head = out.stdout.decode("ascii", "replace").strip()
    return head if out.returncode == 0 and _SHA.match(head) else None


def lane_head(wt):
    head = _head(wt)
    if head is None:
        die("cannot read the HEAD of %s -- a lane's base commit is what every path it did "
            "not seed is measured against, so there is nothing to fold against." % wt)
    return head


Manifest = collections.namedtuple("Manifest", "base paths")


def _is_path_map(obj):
    return isinstance(obj, dict) and all(
        isinstance(k, str) and isinstance(v, str) and _MD5.match(v) for k, v in obj.items())


def load_manifest(path):
    """-> Manifest(base, paths). `base` is None for a FORMAT-1 manifest.

    ⚠ ANYTHING ELSE IS REFUSED, NEVER GUESSED AT. A manifest is the one record of what a
    lane was HANDED; reading a malformed one "as best we can" is how a fold ends up
    subtracting the wrong set -- which drops a lane's work silently.
    """
    try:
        with io.open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as exc:
        die("seed manifest %s is unreadable: %s" % (path, exc))
    if isinstance(data, dict) and "format" in data:
        base, paths = data.get("base"), data.get("paths")
        if (data.get("format") != MANIFEST_FORMAT or set(data) != {"format", "base", "paths"}
                or not isinstance(base, str) or not _SHA.match(base)
                or not _is_path_map(paths)):
            die("seed manifest %s declares a format but is not a well-formed format-%d "
                "manifest {\"base\": <commit>, \"format\": %d, \"paths\": {path: md5}}; "
                "refusing to guess what this lane was handed."
                % (path, MANIFEST_FORMAT, MANIFEST_FORMAT))
        return Manifest(base, dict(paths))
    if _is_path_map(data):
        return Manifest(None, dict(data))
    die("seed manifest %s is neither format %d nor the older flat {path: md5} map; "
        "refusing to guess what this lane was handed." % (path, MANIFEST_FORMAT))


def save_manifest(path, manifest):
    # A format-1 manifest stays format 1: writing a base it never recorded would turn
    # "unknown" into an assertion nobody measured.
    body = manifest.paths if manifest.base is None else {
        "format": MANIFEST_FORMAT, "base": manifest.base, "paths": manifest.paths}
    write_atomic(path, json.dumps(body, indent=1, sort_keys=True))


def commits_no_ref_reaches(root, head):
    """-> the `git rev-list --oneline` lines, newest first, for the commits `head` holds that no
    ref of the repository reaches -- or None when git cannot list them.

    ★ The question `lane-worktree remove` asks, spelled and placed the same way. ✔MEASURED
    2026-09-15 (P66, round 3) on a throwaway repository: `--glob=refs/*` and not
    `--branches --tags --remotes`, because a commit a shared ref outside those three still
    holds is not lost; AT THE REPOSITORY ROOT and not inside the lane, because there the glob
    also counts the lane's own per-worktree refs (`refs/bisect/*`), which die with its worktree.
    ⚠ None, never [], when git fails: with one of a lane's commit objects missing the rev-list
    exits 128 while the lane's status still reads, and "cannot tell" is not "no commits".
    """
    proc = git_run(["-C", root, "rev-list", "--oneline", head, "--not", "--glob=refs/*"],
                   capture_output=True)
    if proc.returncode != 0:
        return None
    return [ln for ln in proc.stdout.decode("utf-8", "replace").splitlines() if ln.strip()]


def lane_base(root, lane, wt, manifest):
    """-> the commit this lane's unseeded paths are measured against. Refuses a moved HEAD: one
    past the recorded base (format 2), or one holding commits no ref reaches (format 1)."""
    head = lane_head(wt)
    if manifest.base is None:
        # ⚠⚠ A FORMAT-1 MANIFEST NEVER RECORDED ITS BASE, AND THAT MADE A COMMIT INSIDE THE LANE
        # INVISIBLE. ✔MEASURED 2026-09-15 (P66, round 3), read-only: all seven live lanes'
        # manifests were format 1. ✔REPRODUCED in a throwaway repository on this file's round-2
        # bytes: `land` on a format-1 lane holding a commit exited 0 "LANDED" -- the fold carried
        # its uncommitted file, `lane-worktree remove -DiscardWork` removed the worktree, and the
        # committed file never reached the main tree while the commit was left unreachable.
        # ★ WITHOUT A RECORDED BASE THE CHECK IS THE ONE THE REPOSITORY CAN STILL ANSWER: a lane
        # is created at a commit the repository's refs reach, so a HEAD holding commits no ref
        # reaches is a lane that committed. That is this fold's own precondition -- HEAD is the
        # base the lane's paths are measured against -- and it is why `land`, which builds
        # --discard-work, never reaches a removal that would orphan a commit. Self-test arms
        # (o3), (o4) and (l8b).
        orphans = commits_no_ref_reaches(root, head)
        if orphans is None:
            die("cannot list the commits lane %s's HEAD %s holds that no ref of the repository "
                "reaches (git rev-list failed), so its format-1 manifest, which records no base, "
                "cannot be shown to match that HEAD. This fold will not guess." % (lane, head[:12]))
        if orphans:
            die("lane %s's manifest is format 1 (it records no base), and its HEAD %s holds %d "
                "commit(s) that no branch, tag or other ref of the repository reaches:\n%s\n"
                "  A COMMIT inside the lane hides its changes from `git status`, which is the only\n"
                "  list of paths a fold reads -- folding now would carry part of the lane and\n"
                "  silently drop the rest. Lanes never commit; how to recover this one is the\n"
                "  orchestrator's decision, and this fold will not guess it."
                % (lane, head[:12], len(orphans),
                   "\n".join(["    " + ln for ln in orphans[:10]]
                             + (["    ... and %d more" % (len(orphans) - 10)]
                                if len(orphans) > 10 else []))))
        print("lane-fold: ⓘ lane %s's manifest is format 1 (written before bases were "
              "recorded): its base is taken from the lane's HEAD %s, which the repository's "
              "refs already reach -- no commit was made inside the lane." % (lane, head[:12]))
        return head
    if head != manifest.base:
        die("lane %s's HEAD is %s, but its manifest records the base %s.\n"
            "  A COMMIT inside the lane hides its changes from `git status`, which is the only\n"
            "  list of paths a fold reads -- folding now would carry part of the lane and\n"
            "  silently drop the rest. Lanes never commit; how to recover this one is the\n"
            "  orchestrator's decision, and this fold will not guess it."
            % (lane, head[:12], manifest.base[:12]))
    return head


# ──────────────────────────────────── seed ─────────────────────────────────────

def cmd_seed(root, lane, empty=False, force=False):
    wt = worktree_path(root, lane)
    if not os.path.isdir(wt):
        die("no worktree at %s\n"
            "  create it first: python3 .harness-config/runner/actions/lane-worktree/lane-worktree.py add %s"
            % (wt, lane))
    if not inside(root, wt):
        die("worktree escapes the repository: %s" % wt)

    # THE ONE WAY THIS TOOL CAN DESTROY A LANE IS SEEDING A WORKTREE THAT IS ALREADY
    # WORKING. The copy overwrites by path, so seeding a LIVE lane replaces files that
    # lane is mid-edit on -- and the lane will not notice, because a lane never re-reads
    # a file it believes it owns. Same defect class as an orchestrator editing config
    # underneath a running lane, with a bulk copy attached.
    # => `seed` runs at the MOMENT the worktree is created, before the lane starts. A
    # worktree already carrying changes of its own is refused here.
    # The guard is on the COPY, so `--empty` -- which copies nothing -- is exempt by
    # construction rather than by an override. `--force` stays for the rare case of
    # deliberately re-seeding a lane that has started.
    lanes = layout(root).worktrees
    own = [q for q in changed_paths(wt) if not is_lane_tree(q, lanes)]
    if own and not force and not empty:
        die("worktree %s already carries %d changed path(s) of its own -- seeding "
            "now would OVERWRITE a running lane's work."
            "\n  Seed at the moment the worktree is created, before the lane starts."
            "\n  If this lane was created at HEAD and given nothing, record that with"
            " `seed %s --empty`, which copies nothing."
            "\n  first few: %s"
            % (os.path.relpath(wt, root).replace(os.sep, "/"), len(own), lane,
               ", ".join(sorted(own)[:5])))

    # `--empty` RECORDS A MANIFEST WITHOUT COPYING, and it is not a shortcut: it is the
    # TRUE description of a lane whose worktree was created at a commit and handed
    # nothing. The manifest answers *which paths did this lane not author, and what
    # were their bytes at hand-off* -- for such a lane the honest answer is the empty
    # set, and `fold` then measures every changed path against the lane's base commit,
    # which is exactly right.
    paths = [] if empty else sorted(
        q for q in set(changed_paths(root)) if not is_lane_tree(q, lanes))
    # ⓘ AN EMPTY SEED IS LEGITIMATE AND MUST NOT REFUSE. The FIRST lane set of a cycle
    # starts from a freshly committed tree, so there is nothing beyond HEAD to carry
    # in. A predecessor refused here, which forced the caller to skip the seed
    # entirely -- and then the fold had no manifest and could not run at all.
    manifest = {}
    for rel in paths:
        src = os.path.join(root, rel)
        if not os.path.isfile(src):
            continue                      # a deletion: nothing to carry into the lane
        dst = os.path.join(wt, rel)
        copy_atomic(src, dst)
        manifest[rel] = md5_file(dst)

    # ★ THE BASE IS RECORDED BESIDE THE PATHS -- see "THE FOLD'S REFUSALS" in the module
    # docstring for the committed-sibling overwrite it closes.
    base = lane_head(wt)
    mpath = manifest_path(root, lane)
    save_manifest(mpath, Manifest(base, manifest))
    print("lane-fold: seeded lane %s with %d path(s) from the main tree"
          % (lane, len(manifest)))
    print("lane-fold: manifest %s (base %s)"
          % (os.path.relpath(mpath, root).replace("\\", "/"), base[:12]))
    main = _head(root)
    if paths and main is not None and main != base:
        print("lane-fold: ⚠ the lane's base %s is not the main tree's HEAD %s: a path "
              "committed between the two is NOT carried in, and a fold refuses any such "
              "path this lane edits." % (base[:12], main[:12]))
    return 0


# ─────────────────────────────── refresh-plans ─────────────────────────────────

# ★★ THE ONE TREE A LIVE LANE MAY BE RE-SEEDED FROM, AND WHY IT IS SAFE WHERE A BULK
# RE-SEED IS NOT. `seed` refuses a working worktree because the copy overwrites files
# the lane is mid-edit on and the lane never re-reads a file it believes it owns. That
# reasoning is about SOURCE. `.plans/**` is different in the one way that matters: no
# lane owns it (the orchestrator does), nothing compiles it, and no lane's binaries can
# change because of it.
#
# ⚠ ✔MEASURED 2026-09-02 (P54, lane `ar`): a lane's `anchor_registry_guard` reds for an
# anchor whose row IS registered in the main tree, because the lane holds the snapshot
# `seed` took before the orchestrator applied that row. **A false red every lane hits**,
# and the dangerous half is that a lane learns to discount that guard — which is the one
# instrument that catches an id cited in `src/` with no row anywhere.
#
# ★ AND UPDATING THE MANIFEST IS HALF THE FIX, not bookkeeping. A refreshed path whose
# manifest md5 is also updated becomes INHERITED at fold time, so the fold subtracts it
# by construction. Without that, the refresh would make `.plans/` look like the lane's
# own change and the fold would try to write a stale registry back over the live one —
# which is what `--settled` has been working around by hand, once per lane, all cycle.
def cmd_refresh_plans(root, lane, apply_it):
    wt = worktree_path(root, lane)
    mpath = manifest_path(root, lane)
    if not os.path.isdir(wt):
        die("no worktree at %s" % wt)
    if not os.path.isfile(mpath):
        die("no seed manifest at %s -- refresh only makes sense for a seeded lane"
            % mpath)
    loaded = load_manifest(mpath)
    seed = loaded.paths

    live = sorted(q for q in set(changed_paths(root))
                  if q.startswith(".plans/") and os.path.isfile(os.path.join(root, q)))
    if not live:
        print("lane-fold: the main tree has no changed .plans/ path -- nothing to refresh")
        return 0

    moved = []
    for rel in live:
        src, dst = os.path.join(root, rel), os.path.join(wt, rel)
        if os.path.isfile(dst) and md5_file(dst) == md5_file(src):
            continue
        moved.append(rel)

    # ⚠ THE LANE MUST NOT HAVE EDITED IT. If the worktree's copy differs from BOTH the
    # seed md5 and the main tree's, something wrote it there -- refuse rather than
    # silently discard a lane's edit to a file it was told not to touch.
    conflicts = [rel for rel in moved
                 if rel in seed and os.path.isfile(os.path.join(wt, rel))
                 and md5_file(os.path.join(wt, rel)) != seed[rel]]
    if conflicts:
        die("the lane's own copy of %d path(s) differs from what it was seeded with, so "
            "refreshing would DISCARD an edit made inside the worktree: %s"
            % (len(conflicts), ", ".join(conflicts[:4])))

    print("lane-fold: %d .plans/ path(s) would refresh into lane %s:" % (len(moved), lane))
    for rel in moved:
        print("   %s" % rel)
    if not apply_it:
        print("lane-fold: dry run. pass --apply to write.")
        return 0

    for rel in moved:
        copy_atomic(os.path.join(root, rel), os.path.join(wt, rel))
        seed[rel] = md5_file(os.path.join(wt, rel))
    save_manifest(mpath, Manifest(loaded.base, seed))
    print("lane-fold: REFRESHED %d path(s) and updated the manifest, so the fold now "
          "subtracts them as INHERITED." % len(moved))
    return 0


# ──────────────────────────────────── fold ─────────────────────────────────────

Classification = collections.namedtuple(
    "Classification", "mine deleted inherited refusals settled converged")


def _drift(consequence, root, base, rel, base_id):
    """The refusal for a main-tree path that no longer holds the lane's base content, naming
    HOW it moved -- a commit and an uncommitted edit are reconciled differently."""
    head_id = _blob_ids(root, "HEAD", [rel]).get(rel)
    if head_id != base_id:
        how = "by a COMMIT (the main tree's HEAD no longer holds the lane's base bytes)"
    else:
        how = "by an UNCOMMITTED EDIT"
    return "main tree DRIFTED from the lane's base %s %s -- %s: %s" % (
        base[:12], how, consequence, rel)


def classify(root, wt, seed, settled=(), base=None):
    """-> Classification(mine, deleted, inherited, refusals, settled, converged).
    Pure measurement; writes nothing.

    `seed` is the manifest's path map; `base` is the lane's base commit (None reads the
    lane's HEAD, which is what a caller holding no manifest means).

    ⚠⚠ `deleted` EXISTS BECAUSE A LANE'S DELETION USED TO VANISH AT THE FOLD.
    `git status` reports a removed path
    as `D <path>`, so `changed_paths` always offered it -- and this function then hit
    `if not os.path.isfile(src): continue` and dropped it on the floor. The fold
    reported success, having silently kept the file.
    ✔MEASURED 2026-09-01 folding lane `al`, which REPLACED an error example whose
    subject its own change had made legal: 17 paths copied, the removal skipped, and
    the surviving example asserts a refusal that no longer happens -- a RED the next
    gate would have charged to the lane's code rather than to this tool. It fails in
    the direction that keeps stale assertions alive, which is the direction that
    looks like nothing happened.
    ⓘ A deletion is DESTRUCTIVE, so it carries the SAME baseline proof a copy does:
    the main tree's content must still match the seed (or the lane's base blob), or the
    fold REFUSES the whole batch rather than destroying work it cannot account for.

    ⚠⚠ BOTH OF THOSE BASELINES USED TO READ THE MAIN TREE'S `HEAD:` -- see the module
    docstring. The copy branch and the deletion branch lost a committed sibling edit
    the same way, so both now read the blob at the lane's own base.

    ★ `converged` -- a lane path whose bytes the main tree already holds -- is neither
    written nor refused: writing it changes nothing, and refusing it would make every
    re-run of an interrupted landing refuse its own first write.

    ⓘ An unseeded path is compared by BLOB ID, never by raw bytes: `_blob_ids` for the
    lane's base, `_worktree_ids` for the main tree's file -- see the latter for the
    line-ending conversion that made raw bytes a false DRIFT."""
    if base is None:
        base = lane_head(wt)
    lanes = layout(root).worktrees
    candidates = sorted(q for q in set(changed_paths(wt)) if not is_lane_tree(q, lanes))
    for rel in candidates:
        if "\n" in rel:
            die("a changed path contains a newline, which git's batch protocols cannot "
                "carry: %r" % rel)
    # ONE `cat-file` and ONE `hash-object` process for every unseeded path this lane
    # changed, rather than one or two per path -- a lane routinely carries a hundred.
    unseeded = [rel for rel in candidates if rel not in seed and rel not in settled]
    base_ids = _blob_ids(wt, base, unseeded)
    main_ids = _worktree_ids(root, [rel for rel in unseeded
                                    if inside(root, os.path.join(root, rel))
                                    and os.path.isfile(os.path.join(root, rel))])
    mine, deleted, inherited, refusals, settled_paths, converged = [], [], [], [], [], []
    for rel in candidates:
        src = os.path.join(wt, rel)
        dest = os.path.join(root, rel)
        if not inside(root, dest):
            refusals.append("escapes the repository: %s" % rel)
            continue
        if rel in settled:
            # ★★ DECLARED SETTLED BY HAND -- the ONE escape from an all-or-nothing
            # refusal, and it exists because the refusal message PROMISED it and the
            # tool did not provide it. ✔MEASURED 2026-09-02 (P54): the message says
            # "merge the second lane's changes by hand, then re-run this fold: the
            # remaining paths still land automatically". They cannot. The drift test
            # compares the DESTINATION against the SEED, so a hand-merge makes the
            # destination differ MORE, and re-running refuses identically -- twice in
            # one cycle, on `.plans/` documents two lanes had both written.
            # ⚠ IT IS NOT A --force. It drops ONE named path from this lane's change
            # set so the OTHER paths can land; nothing about that path is written,
            # and the caller is asserting they have already reconciled it themselves.
            # Every settled path is REPORTED, because a silent skip is how a lane's
            # work goes missing while the fold says it succeeded.
            # ⚠⚠ AND IT IS ASKED BEFORE THE DELETION BRANCH, NOT AFTER IT. An independent
            # review (P66) found a lane's DELETION could not be settled at all: this check
            # sat below the deletion branch, whose refusals `continue` first, so
            # `--settled <path>` re-ran into the identical refusal while the refusal
            # message printed it as the remedy -- and, all-or-nothing, the lane's other
            # work stayed blocked behind it. Arm (k3) pins it.
            settled_paths.append(rel)
            continue
        if not os.path.isfile(src):
            # The lane removed it (or it never existed). Only a path the MAIN TREE
            # still holds is a deletion this fold has to carry out.
            if not os.path.exists(dest):
                continue                      # already absent both sides: nothing to do
            baseline = seed.get(rel)
            if baseline is None:
                if base_ids.get(rel) is None:
                    refusals.append(
                        "lane deleted a path its base %s does not hold, so there is no "
                        "baseline to prove it is safe to remove: %s" % (base[:12], rel))
                    continue
                if main_ids.get(rel) != base_ids[rel]:
                    refusals.append(_drift("refusing to DELETE it", root, base, rel,
                                           base_ids[rel]))
                    continue
            elif md5_file(dest) != baseline:
                refusals.append("main tree DRIFTED since seeding; refusing to DELETE it: %s"
                                % rel)
                continue
            deleted.append(rel)
            continue
        if rel in seed and md5_file(src) == seed[rel]:
            inherited.append(rel)
            continue
        if os.path.isfile(dest) and md5_file(dest) == md5_file(src):
            converged.append(rel)             # the main tree already holds these bytes
            continue
        if rel in seed:
            if not os.path.isfile(dest):
                refusals.append("seeded path vanished from the main tree: %s" % rel)
                continue
            if md5_file(dest) != seed[rel]:
                refusals.append("main tree DRIFTED since seeding (would be lost): %s"
                                % rel)
                continue
        else:
            # Baseline for an unseeded path is the blob at the LANE'S BASE -- see the
            # module docstring for why "absent from the manifest" does not mean "new",
            # and for the committed-sibling overwrite the main tree's HEAD produced.
            base_id = base_ids.get(rel)
            if base_id is not None and os.path.isfile(dest):
                if main_ids.get(rel) != base_id:
                    refusals.append(_drift("would be lost", root, base, rel, base_id))
                    continue
            elif base_id is not None:
                refusals.append("present at the lane's base %s but missing from the main "
                                "tree: %s" % (base[:12], rel))
                continue
            elif os.path.exists(dest):
                refusals.append("absent at the lane's base %s yet present in the main "
                                "tree: %s" % (base[:12], rel))
                continue
        mine.append(rel)
    return Classification(mine, deleted, inherited, refusals, settled_paths, converged)


def _open_lane(root, lane):
    wt = worktree_path(root, lane)
    mpath = manifest_path(root, lane)
    if not os.path.isdir(wt):
        die("no worktree at %s" % wt)
    if not os.path.isfile(mpath):
        die("no seed manifest at %s\n"
            "  run `lane-fold.py seed %s` at the moment the worktree is created -- the "
            "manifest is what separates this lane's work from what it inherited."
            % (mpath, lane))
    return wt, load_manifest(mpath)


def _print_fold_refusals(refusals):
    print("lane-fold: REFUSED -- nothing written. %d problem(s):" % len(refusals))
    for r in refusals:
        print("   " + r)
    print("  A DRIFT refusal is usually TWO LANES ON ONE FILE -- by an UNCOMMITTED EDIT when "
          "the\n  other lane's fold is still in the tree, by a COMMIT once it was committed "
          "(which\n  used to be written over SILENTLY). Merge the second lane's declared "
          "changes into the\n  main tree by hand, then re-run naming each reconciled path\n"
          "  `--settled <path>` (repeatable), which drops JUST those paths from "
          "this lane's\n  change set so the rest can land. ⚠ A bare re-run will "
          "refuse identically: the\n  drift test compares the DESTINATION against "
          "the lane's baseline, so merging by hand\n  makes the destination differ MORE, "
          "not less. `--settled` is an assertion that YOU\n  have already reconciled "
          "that path; it is not a --force, and nothing is written for it.")


def _print_fold_plan(lane, seed, cls):
    if cls.settled:
        # Never silent: a skipped path is how a lane's work goes missing while the
        # fold reports success.
        print("lane-fold: %d path(s) DECLARED SETTLED BY HAND -- not written, not "
              "compared:" % len(cls.settled))
        for rel in cls.settled:
            print("   %s" % rel)
    print("lane-fold: lane %s -- %d inherited path(s) skipped, %d path(s) are this "
          "lane's:" % (lane, len(cls.inherited), len(cls.mine)))
    for rel in cls.mine:
        # ⚠ THE LABEL ANSWERS "WAS THIS PATH SEEDED?", NOT "IS THIS FILE NEW?". An
        # earlier wording said the second, and three files it marked `(new)` were
        # TRACKED AT HEAD all along -- which briefly made an unchanged test count look
        # like a lane's tests had gone missing.
        print("   %s%s" % (rel, "" if rel in seed
                                else "   (not seeded -- no prior lane touched it)"))
    # ⚠ DELETIONS ARE LISTED SEPARATELY AND LOUDLY. They are the destructive half of
    # a fold, and a reader scanning the copy list would not otherwise see them at all
    # -- which is exactly how a lane's dropped deletion stayed
    # invisible: nothing printed, so nothing looked wrong.
    if cls.deleted:
        print("lane-fold: and %d path(s) this lane DELETED:" % len(cls.deleted))
        for rel in cls.deleted:
            print("   %s   (will be REMOVED from the main tree)" % rel)
    if cls.converged:
        print("lane-fold: and %d path(s) ALREADY LANDED -- the main tree holds these bytes, "
              "nothing to write:" % len(cls.converged))
        for rel in cls.converged:
            print("   %s" % rel)


def _apply_fold(root, wt, cls):
    for rel in cls.mine:
        copy_atomic(os.path.join(wt, rel), os.path.join(root, rel))
    for rel in cls.deleted:
        os.remove(os.path.join(root, rel))
        # Take the directory too once it is empty, so a removed example leaves no
        # husk -- but never recursively, and never past the repository root.
        d = os.path.dirname(os.path.join(root, rel))
        while inside(root, d) and os.path.realpath(d) != os.path.realpath(root):
            if os.listdir(d):
                break
            os.rmdir(d)
            d = os.path.dirname(d)
    print("lane-fold: WROTE %d path(s) into the main tree%s."
          % (len(cls.mine), (" and REMOVED %d" % len(cls.deleted)) if cls.deleted else ""))


def cmd_fold(root, lane, apply_it, settled=()):
    wt, manifest = _open_lane(root, lane)
    base = lane_base(root, lane, wt, manifest)
    cls = classify(root, wt, manifest.paths, settled, base=base)
    if cls.refusals:
        _print_fold_refusals(cls.refusals)
        return 2
    _print_fold_plan(lane, manifest.paths, cls)
    if not apply_it:
        print("lane-fold: dry run. pass --apply to write.")
        return 0
    _apply_fold(root, wt, cls)
    return 0


# ═══════════════════════════════════ LANDING ═══════════════════════════════════

RowPlan = collections.namedtuple(
    "RowPlan", "anchor files flat status_cell status_word bucket priority "
               "priority_source action")
ApplyResult = collections.namedtuple(
    "ApplyResult", "ok written failed output restored restore_failed")


def row_dir(wt, lane):
    """Where a lane files its row cells: `<worktree>/.temp/<lane>-scratch/row/`."""
    return os.path.join(wt, ".temp", "%s-scratch" % lane, "row")


def load_anchors_module(root):
    """-> the TARGET tree's `.harness-config/runner/actions/anchors/anchors.py`, loaded as a module.

    ★ ITS VOCABULARY IS USED, NEVER RE-TYPED HERE: `normalise_status`,
    `normalise_priority`, `WORKING`, `find`, the cell indices, `suggest_band`. Rows are
    WRITTEN by the door, `dssharness write-anchor` / `set-anchor`, through that file's one
    launcher (`door_write`) -- one door call per row, cells by file -- so every refusal the
    door and the launcher own lives in one place.
    ⓘ The target tree's copy, not this file's sibling: `anchors.py` has no `--repo`, and
    its registries are the ones beside its own file.
    """
    path = os.path.join(root, ACTIONS_REL, "anchors", "anchors.py")
    if not os.path.isfile(path):
        die("no row writer at %s -- rows are written only through .harness-config/runner/actions/anchors/anchors.py."
            % path)
    name = "lane_fold_anchors_%s" % hashlib.md5(
        os.path.realpath(path).encode("utf-8", "surrogateescape")).hexdigest()
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    held, sys.argv = sys.argv, [path]
    try:
        spec.loader.exec_module(module)
    except SystemExit as exc:
        die("the row writer at %s refused to load: %s" % (path, exc))
    finally:
        sys.argv = held
    sys.modules[name] = module
    return module


def registry_paths(root, anchors):
    return [os.path.join(root, *anchors.REL[b].split("/")) for b in anchors.BUCKETS]


def registry_md5(root, anchors):
    return dict((p, md5_file(p) if os.path.isfile(p) else None)
                for p in registry_paths(root, anchors))


def _flat(text):
    """The writer's own whitespace rule for a cell: every run of whitespace is one space."""
    return " ".join(str(text).split())


def _read_cell(path):
    with io.open(path, "r", encoding="utf-8", errors="strict", newline="") as fh:
        return fh.read()


def plan_rows(root, wt, lane, default_bucket, anchors):
    """-> ([RowPlan], [problem]). Validates every row, then dry-runs every row through the
    writer. WRITES NOTHING. Any problem means no row may be written."""
    problems, plans = [], []
    rdir = row_dir(wt, lane)
    shown = os.path.relpath(rdir, root).replace(os.sep, "/")
    if not os.path.isdir(rdir):
        return plans, ["no row directory at %s -- a lane files each row there as cell files "
                       "(<ANCHOR>.status, .trigger, .closing, .crossrefs)" % shown]
    groups = {}
    for name in sorted(os.listdir(rdir)):
        path = os.path.join(rdir, name)
        match = ROW_CELL.match(name)
        if os.path.isdir(path):
            problems.append("%s/%s is a DIRECTORY -- keep drafts OUT of row/" % (shown, name))
        elif not match:
            # ✔REPRODUCED (P66): a draft beside real cells, and a cell named `.status`
            # with no anchor at all. Neither may be skipped silently.
            problems.append("%s/%s is not a row cell <ANCHOR>.<%s> -- keep drafts OUT of row/"
                            % (shown, name, "|".join(ROW_SUFFIXES)))
        elif not match.group(1).startswith("D-") or any(c.isspace() for c in match.group(1)):
            problems.append("%s/%s: %r is not an anchor id" % (shown, name, match.group(1)))
        else:
            groups.setdefault(match.group(1), {})[match.group(2)] = path
    if not groups and not problems:
        problems.append("%s holds no rows" % shown)
    to_word = dict((cell, word) for word, cell in anchors.STATUS.items())
    for anchor in sorted(groups):
        have = groups[anchor]
        missing = [s for s in REQUIRED_CELLS if s not in have]
        if missing:
            problems.append("%s: incomplete row -- missing %s. A row with no .status is an "
                            "ORPHAN no step would ever apply, and it would die with the "
                            "worktree." % (anchor, ", ".join("%s.%s" % (anchor, s)
                                                              for s in missing)))
            continue
        texts = {}
        try:
            for suffix, path in have.items():
                texts[suffix] = _read_cell(path)
        except (OSError, UnicodeDecodeError) as exc:
            problems.append("%s: a cell file cannot be read as UTF-8 (%s)" % (anchor, exc))
            continue
        try:
            status_cell = anchors.normalise_status(texts["status"].strip())
        except anchors.Refused as exc:
            # ✔REPRODUCED (P66): the scratchpad applier wrote an unparseable status as OPEN.
            problems.append("%s.status: %s" % (anchor, exc))
            continue
        declared_bucket = texts["bucket"].strip() if "bucket" in texts else None
        if declared_bucket is not None and declared_bucket not in anchors.WORKING:
            problems.append("%s.bucket holds %r; it must name one of: %s"
                            % (anchor, declared_bucket, ", ".join(anchors.WORKING)))
            continue
        declared_priority = None
        if "priority" in texts:
            try:
                declared_priority = anchors.normalise_priority(texts["priority"])
            except anchors.Refused as exc:
                problems.append("%s.priority: %s" % (anchor, exc))
                continue
        flat = dict((k, _flat(texts[k])) for k in ("trigger", "closing", "crossrefs"))
        try:
            rows = anchors.find(root, anchor)
        except anchors.Refused as exc:
            problems.append("%s: the registries cannot be read (%s)" % (anchor, exc))
            continue
        if len(rows) > 1:
            problems.append("%s already has %d rows (%s) -- a duplicate is settled by a "
                            "human, never by a landing"
                            % (anchor, len(rows), ", ".join(sorted(set(r.rel for r in rows)))))
            continue
        if rows:
            row = rows[0]
            home = row.bucket if row.bucket in anchors.WORKING else row.table
            if declared_bucket is not None and declared_bucket != home:
                # ✔REPRODUCED (P66): the scratchpad applier ignored this declaration
                # without a word. Honouring it would RE-FILE a row, which is not what a
                # landing does; ignoring it silently discards what the lane said.
                problems.append("%s.bucket declares %s, but the row already lives in %s (%s). "
                                "A landing never re-files a row: move it deliberately with "
                                "write-anchor --%s, or delete the .bucket file."
                                % (anchor, declared_bucket, home, row.rel, declared_bucket))
                continue
            priority = declared_priority or row.priority
            source = "declared" if declared_priority else "carried from the registry"
            try:
                anchors.normalise_priority(priority)
            except anchors.Refused:
                problems.append("%s: its stored priority %r is not a band, so it cannot be "
                                "carried forward -- declare %s.priority"
                                % (anchor, priority, anchor))
                continue
            same = (row.status == status_cell and row.priority == priority
                    and _flat(row.cell(anchors.C_TRIGGER)) == flat["trigger"]
                    and _flat(row.cell(anchors.C_CLOSING)) == flat["closing"]
                    and _flat(row.cell(anchors.C_XREF)) == flat["crossrefs"]
                    and home in anchors.WORKING
                    and (row.bucket == "done") == bool(row.closed))
            if same:
                action = "landed"
            elif row.bucket not in anchors.WORKING and declared_bucket is None:
                problems.append("%s is ARCHIVED in %s (table: %s), and applying it re-files it "
                                "-- write %s.bucket naming its working list, so that decision "
                                "is declared, never inferred."
                                % (anchor, row.rel, row.table, anchor))
                continue
            else:
                action = "replace"
            bucket = home
        else:
            bucket = declared_bucket or default_bucket
            priority = declared_priority
            source = "declared"
            if not priority:
                # The door REQUIRES a priority. The sieve's band is a SUGGESTION, written as
                # the declaration a human then corrects -- and said here, so it is never silent.
                probe = "| x | x | x | %s | %s | %s |" % (flat["trigger"], flat["closing"],
                                                         flat["crossrefs"])
                priority, why = anchors.suggest_band(anchor, probe, bucket)
                source = ("none declared -- SEEDED from the burndown sieve (%s); correct it with "
                          "`dssharness set-anchor %s --priority <band>`" % (why, anchor))
            action = "insert"
        plans.append(RowPlan(anchor,
                             dict((k, have[k]) for k in ("trigger", "closing", "crossrefs")),
                             flat, status_cell, to_word[status_cell], bucket, priority,
                             source, action))
    if problems:
        return plans, problems
    # ★ THE WRITER'S OWN REFUSALS, BEFORE ANYTHING IS WRITTEN. ✔REPRODUCED (P66): a
    # pre-check that modelled only stem shape and cell presence let a later row the
    # WRITER refuses (a pre-escaped pipe) land after the earlier rows were written.
    for plan in plans:
        if plan.action == "landed":
            continue
        rc, out = _run_writer(root, plan, False)
        if rc != 0:
            problems.append("%s: the row writer REFUSED it on a dry run (rc=%s):\n%s"
                            % (plan.anchor, rc,
                               "\n".join("      " + ln for ln in out.strip().splitlines())))
    return plans, problems


def _run_writer(root, plan, apply_it):
    """-> (returncode, output) of ONE door call for `plan` -- `dssharness write-anchor` for a
    NEW row, `set-anchor` for an existing one -- through the target tree's launcher,
    `anchors.door_write`. The only place a row is written.

    ★ AN UPDATE NAMES ONLY WHAT CHANGES. A field whose declaration already equals the stored
    row (cells compared the way `verify_rows` compares them) is not sent, so the door carries
    the stored cell through byte for byte -- a run of spaces or a tab a lane did not touch is
    never rewritten.
    ★ CELLS GO BY FILE, NEVER BY ARGUMENT (the launcher writes them). ✔MEASURED 2026-09-15
    (P66, lane `bl`): a row of about 48 KB crossed Windows' 32,767-character command line and
    died with exit 126 before the writer ever ran.
    ⓘ A module-level function on purpose: the self-test replaces it to inject the failures
    a dry run cannot foresee. It reads the cells from `plan.files` at CALL time, so an
    injected plan is written as injected.
    """
    anchors = load_anchors_module(root)
    try:
        texts = dict((k, _read_cell(plan.files[k])) for k in ("trigger", "closing", "crossrefs"))
    except (OSError, UnicodeDecodeError) as exc:
        return 2, "a cell file cannot be read as UTF-8 (%s)" % exc
    fields = {"status": plan.status_word, "trigger": texts["trigger"],
              "closing": texts["closing"], "cross_refs": texts["crossrefs"]}
    if plan.priority:
        fields["priority"] = plan.priority
    stored = None
    if plan.action != "insert":
        rows = anchors.find(root, plan.anchor)
        stored = rows[0] if len(rows) == 1 else None
    if stored is not None:
        if stored.status == plan.status_cell:
            del fields["status"]
        if fields.get("priority") == stored.priority:
            del fields["priority"]
        for key, column in (("trigger", anchors.C_TRIGGER), ("closing", anchors.C_CLOSING),
                            ("cross_refs", anchors.C_XREF)):
            if _flat(stored.cell(column)) == _flat(fields[key]):
                del fields[key]
    return anchors.door_write(root, plan.anchor, fields, plan.action == "insert", apply_it)


def _restore_registries(snapshot):
    restored, failed = [], []
    for path, data in sorted(snapshot.items()):
        try:
            with io.open(path, "rb") as fh:
                if fh.read() == data:
                    continue
            tmp = path + ".lane-fold-tmp"
            with io.open(tmp, "wb") as fh:
                fh.write(data)
            os.replace(tmp, path)
            with io.open(path, "rb") as fh:
                (restored if fh.read() == data else failed).append(path)
        except OSError:
            failed.append(path)
    return restored, failed


def apply_rows(root, plans, anchors, before):
    """Writes every row that is not already landed, ALL OR NOTHING. -> ApplyResult.

    `before` is the registries' md5 map taken before the rows were validated: a registry
    that moved since then was written by something else, and applying over it would mix
    two writers' work into one snapshot."""
    paths = registry_paths(root, anchors)
    now = registry_md5(root, anchors)
    if now != before:
        moved = [os.path.relpath(p, root).replace(os.sep, "/") for p in paths
                 if now.get(p) != before.get(p)]
        return ApplyResult(False, [], None,
                           "the registries changed after the rows were validated (%s) -- "
                           "something else is writing them; nothing was written"
                           % ", ".join(moved), [], [])
    snapshot = {}
    for p in paths:
        with io.open(p, "rb") as fh:
            snapshot[p] = fh.read()
    written = []
    for plan in plans:
        if plan.action == "landed":
            continue
        rc, out = _run_writer(root, plan, True)
        if rc != 0:
            # ⚠ A WRITE THAT FAILS AFTER AN EARLIER ROW LANDED IS A PARTIAL APPLICATION --
            # the defect this verb exists to end -- so the earlier rows are UNDONE, byte
            # for byte, rather than left for somebody to notice.
            restored, failed = _restore_registries(snapshot)
            return ApplyResult(False, written, plan.anchor, out, restored, failed)
        written.append(plan.anchor)
    return ApplyResult(True, written, None, "", [], [])


def verify_rows(root, plans, anchors):
    """-> [problem]. Re-reads every row AFTER the write and compares it, field by field, with
    what the lane declared. The writer's exit code is not taken as proof."""
    problems = []
    for plan in plans:
        try:
            rows = anchors.find(root, plan.anchor)
        except anchors.Refused as exc:
            problems.append("%s: the registries cannot be read back (%s)" % (plan.anchor, exc))
            continue
        if len(rows) != 1:
            problems.append("%s: %d row(s) after the write, not exactly 1"
                            % (plan.anchor, len(rows)))
            continue
        row = rows[0]
        wrong = []
        if row.status != plan.status_cell:
            wrong.append("status %r, declared %r" % (row.status, plan.status_cell))
        if plan.priority and row.priority != plan.priority:
            wrong.append("priority %r, declared %r" % (row.priority, plan.priority))
        where = (row.bucket, row.table) if row.closed else (row.bucket,)
        expect = ("done", plan.bucket) if row.closed else (plan.bucket,)
        if where != expect:
            wrong.append("home %s, expected %s" % ("/".join(where), "/".join(expect)))
        for key, column in (("trigger", anchors.C_TRIGGER), ("closing", anchors.C_CLOSING),
                            ("crossrefs", anchors.C_XREF)):
            if _flat(row.cell(column)) != plan.flat[key]:
                wrong.append("the %s cell does not re-read as %s.%s" % (key, plan.anchor, key))
        if wrong:
            problems.append("%s: %s" % (plan.anchor, "; ".join(wrong)))
    return problems


def _print_row_plans(plans):
    what = {"insert": "NEW row", "replace": "REPLACES its row",
            "landed": "ALREADY LANDED -- identical, nothing to write"}
    for p in plans:
        print("   %s" % p.anchor)
        print("      %s, %s -> %s, priority %s (%s)"
              % (p.status_cell, what[p.action], p.bucket, p.priority or "unset",
                 p.priority_source))


def _print_problems(what, problems):
    print("lane-fold: REFUSED -- %d problem(s) with the %s:" % (len(problems), what))
    for p in problems:
        print("   " + p)


def _apply_and_verify_rows(root, plans, anchors, before):
    result = apply_rows(root, plans, anchors, before)
    if not result.ok:
        print("lane-fold: REFUSED -- the row application FAILED%s:"
              % (" at %s" % result.failed if result.failed else ""))
        for ln in result.output.strip().splitlines():
            print("      " + ln)
        shown = lambda ps: ", ".join(os.path.relpath(p, root).replace(os.sep, "/") for p in ps)
        print("   rows written before the failure: %s" % (", ".join(result.written) or "none"))
        if result.restored:
            print("   RESTORED byte-for-byte: %s -- no row of this lane is left applied."
                  % shown(result.restored))
        if result.restore_failed:
            print("   ⚠⚠ RESTORE FAILED for %s -- those registries are NOT in their "
                  "pre-apply state; read them before anything else." % shown(result.restore_failed))
        if not (result.written or result.restored or result.restore_failed):
            print("   nothing was written.")
        return 2
    problems = verify_rows(root, plans, anchors)
    if problems:
        print("lane-fold: REFUSED -- VERIFY FAILED: %d row(s) do not re-read as the lane "
              "declared them:" % len(problems))
        for p in problems:
            print("   " + p)
        return 2
    landed = sum(1 for p in plans if p.action == "landed")
    print("lane-fold: APPLIED %d row(s), %d already landed; every row re-read: status, "
          "priority, home and cells match." % (len(result.written), landed))
    return 0


def cmd_apply_rows(root, lane, default_bucket, apply_it):
    wt = worktree_path(root, lane)
    if not os.path.isdir(wt):
        die("no worktree at %s" % wt)
    anchors = load_anchors_module(root)
    before = registry_md5(root, anchors)
    plans, problems = plan_rows(root, wt, lane, default_bucket, anchors)
    print("lane-fold: rows of lane %s (%d):" % (lane, len(plans)))
    _print_row_plans(plans)
    if problems:
        _print_problems("rows -- nothing written", problems)
        return 2
    if not apply_it:
        print("lane-fold: dry run -- every row passed the writer's own dry run. "
              "pass --apply to write.")
        return 0
    return _apply_and_verify_rows(root, plans, anchors, before)


def evidence_digests(wt, roots):
    """-> {worktree-relative path: md5} for every file under every evidence root (`roots`:
    `layout(root).evidence_roots`, the `worktrees.evidenceRoots` lane-worktree gates)."""
    found = {}
    for top_name in roots:
        top = _rel_path(wt, top_name)
        if not os.path.isdir(top):
            continue
        for dirpath, _dirs, files in os.walk(top):
            for name in files:
                path = os.path.join(dirpath, name)
                if os.path.isfile(path):
                    found[os.path.relpath(path, wt).replace(os.sep, "/")] = md5_file(path)
    return found


def default_evidence_destination(root, lane):
    """`<worktrees>/<evidence>/<lane>-<UTC stamp>` (both READ, `layout`): ignored with the lanes,
    repository-relative, outliving the lane, and never listed as one."""
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    first = os.path.join(_rel_path(root, layout(root).evidence), "%s-%s" % (lane, stamp))
    candidate, n = first, 1
    while os.path.exists(candidate):
        n += 1
        candidate = "%s-%d" % (first, n)
    return candidate


def lane_worktree_path():
    """`../lane-worktree/lane-worktree.py`, this file's SIBLING action -- derived from where this
    file sits, never from a tree root counted in `..`."""
    return os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                        "lane-worktree", "lane-worktree.py")


def lane_worktree_remove_argv(root, lane, destination, verified):
    """-> the argv that asks `lane-worktree`, the ONE owner of removal, to preserve the lane's
    evidence, DISCARD its uncommitted work, and remove its worktree: the same argv on every host,
    started by THIS interpreter.

    ★★ `verified` IS WHAT THE DISCARD FLAG IS BUILT FROM. `lane-worktree remove` refuses
    (exit 8) a worktree whose own `git status` lists uncommitted work -- every lane's does,
    its own work and its seeded paths alike -- because it cannot tell folded work from
    unfolded work, and only this file's measurement can. So `--discard-work` is added HERE,
    and only when `verified` is the post-fold `classify` result with nothing left to fold: no
    lane path still to write, none still to delete, no refusal. Anything else refuses, so no
    caller can build a removal that discards work without that measurement in hand.
    Self-test arms (l7a)-(l7c) pin the order and the refusal.

    ⚠ The same flag also discards the commits a lane's HEAD holds that no ref of the repository
    reaches -- `lane-worktree` refuses those with exit 8 too, and a fold cannot see them. `land`
    never builds it for such a lane: `lane_base` refuses first, a HEAD past the recorded base or,
    for a format-1 manifest, a HEAD holding such commits. Self-test arms (l8) and (l8b) pin that
    no removal is even built.

    ⓘ `lane-worktree` is one Python program on every host (2026-09-21, lane mig), so there is
    nothing to probe: `sys.executable` runs it. A missing file stops the landing with the
    worktree kept, rather than failing inside the child.
    """
    if (not isinstance(verified, Classification)
            or verified.mine or verified.deleted or verified.refusals):
        die("refusing to build a removal that DISCARDS lane %s's uncommitted work: it was not "
            "handed a fold measurement showing nothing left to fold (%s). The worktree is kept."
            % (lane, "no measurement at all" if not isinstance(verified, Classification) else
               "%d path(s) still to write, %d to delete, %d refusal(s)"
               % (len(verified.mine), len(verified.deleted), len(verified.refusals))))
    subject = lane_worktree_path()
    if not os.path.isfile(subject):
        die("cannot find %s -- the worktree cannot be removed by its owner, so the landing stops "
            "here with the worktree kept." % subject)
    return [sys.executable, subject, "--repo", root, "remove", lane, "--discard-work",
            "--preserve-to", destination]


def cmd_land(root, lane, default_bucket, apply_it, settled=(), preserve_to=None):
    wt, manifest = _open_lane(root, lane)
    base = lane_base(root, lane, wt, manifest)
    anchors = load_anchors_module(root)
    print("lane-fold: LAND lane %s (base %s)" % (lane, base[:12]))

    # ── 1. MEASURE EVERYTHING; WRITE NOTHING ────────────────────────────────────
    cls = classify(root, wt, manifest.paths, settled, base=base)
    before = registry_md5(root, anchors)
    plans, problems = plan_rows(root, wt, lane, default_bucket, anchors)
    evidence_roots = layout(root).evidence_roots
    evidence = evidence_digests(wt, evidence_roots)
    destination = preserve_to or default_evidence_destination(root, lane)
    print("lane-fold: [1/5] the fold")
    if not cls.refusals:
        _print_fold_plan(lane, manifest.paths, cls)
    print("lane-fold: [2/5] the rows (%d)" % len(plans))
    _print_row_plans(plans)
    print("lane-fold: [3/5] the evidence: %d file(s) under %s, to be kept at %s"
          % (len(evidence), " and ".join(r + "/" for r in evidence_roots), destination))
    if cls.refusals or problems:
        if cls.refusals:
            _print_fold_refusals(cls.refusals)
        if problems:
            _print_problems("rows", problems)
        print("lane-fold: NOT LANDED -- nothing written, nothing removed; the worktree stays.")
        return 2
    if not apply_it:
        print("lane-fold: dry run -- the fold measured clean and every row passed the "
              "writer's dry run. pass --apply to land.")
        return 0

    # ── 2. THE FOLD, THEN THE ROWS ──────────────────────────────────────────────
    _apply_fold(root, wt, cls)
    if _apply_and_verify_rows(root, plans, anchors, before) != 0:
        print("lane-fold: NOT LANDED -- the fold above IS applied and the worktree is KEPT. "
              "Correct the cause and re-run `land`: the fold re-measures its own write as "
              "ALREADY LANDED, and rows that already match are not written again.")
        return 2

    # ── 3. NOTHING LEFT TO FOLD ─────────────────────────────────────────────────
    after = classify(root, wt, manifest.paths, settled, base=base)
    if after.mine or after.deleted or after.refusals:
        print("lane-fold: NOT LANDED -- FOLD VERIFY FAILED: after writing, the lane still "
              "differs from the main tree: %s. The worktree is KEPT."
              % ", ".join(after.mine + after.deleted + after.refusals))
        return 2
    print("lane-fold: [4/5] the fold re-measured: nothing left to fold (%d path(s) landed)"
          % len(after.converged))

    # ── 4. THE EVIDENCE KEPT AND THE WORKTREE REMOVED -- BY THE OWNER OF REMOVAL ──
    # ★ `after` is handed over: the discard-work flag is built from it and from nothing else.
    argv = lane_worktree_remove_argv(root, lane, destination, after)
    print("lane-fold: [5/5] %s" % " ".join('"%s"' % a if " " in a else a for a in argv))
    # ★ THE CHILD DOES NOT INHERIT THE CALLER'S GIT ENVIRONMENT: handed a steering GIT_DIR, its
    # own `git worktree remove` and `prune` would act on another repository and leave this one
    # registering a worktree that is gone. Self-test arm (e4).
    proc = subprocess.run(argv, capture_output=True, env=_owning_tree().git_environment())
    for ln in (proc.stdout + proc.stderr).decode("utf-8", "replace").splitlines():
        print("      " + ln)
    if proc.returncode != 0:
        print("lane-fold: NOT LANDED -- lane-worktree declined to remove the worktree (rc=%d). "
              "The fold and the rows ARE landed; the worktree and its evidence are KEPT. "
              "Fix the cause and re-run `land`." % proc.returncode)
        return 2
    # ⚠ REMOVE, THEN VERIFY, THEN SPEAK -- and this verb verifies what IT claims.
    if os.path.exists(wt):
        print("lane-fold: NOT LANDED -- lane-worktree exited 0 but %s is still on disk." % wt)
        return 2
    lost = [rel for rel, digest in sorted(evidence.items())
            if not os.path.isfile(os.path.join(destination, *rel.split("/")))
            or md5_file(os.path.join(destination, *rel.split("/"))) != digest]
    if lost:
        print("lane-fold: ⚠⚠ EVIDENCE CHECK FAILED -- %d of %d file(s) are not at %s "
              "byte-identical: %s" % (len(lost), len(evidence), destination,
                                      ", ".join(lost[:5])))
        return 2
    print("lane-fold: LANDED lane %s -- %d path(s) written and %d removed; %d row(s) applied "
          "and %d already landed, every row re-read; %d evidence file(s) at %s, each "
          "re-read." % (lane, len(cls.mine), len(cls.deleted),
                        sum(1 for p in plans if p.action != "landed"),
                        sum(1 for p in plans if p.action == "landed"),
                        len(evidence), destination))
    return 0


# ──────────────────────────────────── list ─────────────────────────────────────

def cmd_list(root):
    lay = layout(root)
    wdir = _rel_path(root, lay.worktrees)
    lanes = sorted(n for n in os.listdir(wdir)
                   if not n.startswith(".") and os.path.isdir(os.path.join(wdir, n))
                   ) if os.path.isdir(wdir) else []
    mdir = _rel_path(root, lay.manifests)
    manifests = sorted(n[len("seed-"):-len(".json")] for n in os.listdir(mdir)
                       if n.startswith("seed-") and n.endswith(".json")
                       ) if os.path.isdir(mdir) else []
    print("lane-fold: worktrees under %s/: %s"
          % (lay.worktrees, ", ".join(lanes) if lanes else "(none)"))
    print("lane-fold: seed manifests:      %s"
          % (", ".join(manifests) if manifests else "(none)"))
    # ★ A manifest with no worktree is not junk -- it is the record of a lane that was
    # folded and removed, and deleting it would erase what that lane was seeded with.
    for lane in manifests:
        if lane not in lanes:
            print("   ⓘ %s: manifest kept, worktree already removed" % lane)
    for lane in lanes:
        if lane not in manifests:
            print("   ⚠ %s: worktree with NO manifest -- `fold` will refuse it" % lane)
    edir = _rel_path(root, lay.evidence)
    kept = sorted(os.listdir(edir)) if os.path.isdir(edir) else []
    print("lane-fold: evidence kept by `land`: %s" % (", ".join(kept) if kept else "(none)"))
    return 0


# ────────────────────────────────── self-test ──────────────────────────────────

def _fixture_layout(repo, worktrees=None, evidence_roots=None, lane_fold=None):
    """Give a self-test repository the two files `layout` reads: THIS tree's own
    `.harness-config/config.json` and this program's own `lane-fold-config.json`, byte for byte --
    so every arm runs on the files that really ship -- unless an arm overrides a value: then
    config.json is re-serialised with `worktrees.root` / `worktrees.evidenceRoots` replaced, and
    `lane_fold` (a dict, or the TEXT of the file) replaces lane-fold-config.json whole."""
    real_cfg = os.path.join(_owning_tree().owning_tree(__file__), ".harness-config", "config.json")
    real_lf = os.path.join(os.path.dirname(os.path.realpath(__file__)), "lane-fold-config.json")
    cfg_dst = os.path.join(repo, ".harness-config", "config.json")
    lf_dst = os.path.join(repo, LANE_FOLD_CONFIG_REL)
    if worktrees is None and evidence_roots is None:
        copy_atomic(real_cfg, cfg_dst)
    else:
        cfg = _owning_tree().load_jsonc(real_cfg)
        if worktrees is not None:
            cfg["worktrees"]["root"] = worktrees
        if evidence_roots is not None:
            cfg["worktrees"]["evidenceRoots"] = list(evidence_roots)
        write_atomic(cfg_dst, json.dumps(cfg, indent=2) + "\n")
    if lane_fold is None:
        copy_atomic(real_lf, lf_dst)
    else:
        write_atomic(lf_dst, lane_fold if isinstance(lane_fold, str)
                     else json.dumps(lane_fold, indent=2) + "\n")


def self_test():
    """Red-on-disable for the instrument, on REAL temporary repositories.

    ⚠ EVERY ARM HERE IS A REFUSAL. A fold that copies the right files is right by
    construction and proves nothing; what has to be exercised is each way a fold can
    destroy work while reporting success. The four that matter most:
      (a) an INHERITED path -- seeded, untouched by the lane -- is NOT folded back
          (this is the one that would silently revert a sibling);
      (b) a DRIFTED destination refuses, and refuses the WHOLE fold;
      (c) an unseeded path that the main tree still holds at the lane's base folds fine
          ("absent from the manifest" does not mean "new");
      (d) a quoted path -- one containing a SPACE -- survives the status parse;
    and, since P66, (n) a sibling's COMMITTED fold is never written over, (o) a lane
    whose HEAD moved is refused, and (r)/(l) a landing is all-or-nothing, re-runnable,
    and never removes a worktree whose rows or evidence did not land.
    """
    failed = [0]
    pins = [0]

    def pin(ok, why, detail=""):
        pins[0] += 1
        if ok and detail:
            # A passing pin names its measurement on ONE line; the multi-line text a
            # failure needs (a verb's whole output) is printed only when it fails.
            detail = detail.splitlines()[0]
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", why,
                               ("   " + detail) if detail else ""))
        if not ok:
            failed[0] += 1

    with tempfile.TemporaryDirectory() as tmp:
        root = os.path.realpath(tmp)
        run = lambda *a: git_run(["-C", root] + list(a), capture_output=True, check=True)
        git_run(["init", "-q", root], capture_output=True, check=True)
        run("config", "user.email", "selftest@example.invalid")
        run("config", "user.name", "lane-fold self-test")
        for rel, body in (("tracked.txt", "base\n"),
                          ("shared.json", "{}\n"),
                          # ⓘ THE SPACE IS THE POINT: git C-quotes this path under
                          # `--porcelain`, and the predecessor's `line[3:]` parse
                          # silently omitted the repository's real equivalent.
                          ("a file - with spaces.md", "base\n")):
            write_atomic(os.path.join(root, rel), body)
        # Committed with the base, so they are part of HEAD and never seeded.
        _fixture_layout(root)
        run("add", "-A")
        run("commit", "-q", "-m", "base")

        # The main tree carries one uncommitted edit: that is what gets seeded.
        write_atomic(os.path.join(root, "shared.json"), '{"from":"lane-one"}\n')

        wt = worktree_path(root, "x")
        os.makedirs(wt)
        # A worktree is a checkout; the self-test only needs a directory git can read,
        # so make it a repository of its own at the same content.
        git_run(["init", "-q", wt], capture_output=True, check=True)
        for rel in ("tracked.txt", "a file - with spaces.md"):
            copy_atomic(os.path.join(root, rel), os.path.join(wt, rel))
        git_run(["-C", wt, "config", "user.email", "s@e.invalid"], capture_output=True, check=True)
        git_run(["-C", wt, "config", "user.name", "s"], capture_output=True, check=True)
        git_run(["-C", wt, "add", "-A"], capture_output=True, check=True)
        git_run(["-C", wt, "commit", "-q", "-m", "base"], capture_output=True, check=True)

        cmd_seed(root, "x")
        seed = load_manifest(manifest_path(root, "x")).paths
        pin(list(seed) == ["shared.json"],
            "seed records exactly the main tree's uncommitted paths", "got=%s" % list(seed))

        # (a) the lane leaves the seeded file alone and edits two of its own.
        write_atomic(os.path.join(wt, "tracked.txt"), "lane edit\n")
        write_atomic(os.path.join(wt, "a file - with spaces.md"), "lane edit\n")
        mine, _del, inherited, refusals, _s, _c = classify(root, wt, seed)
        pin(not refusals, "a clean fold refuses nothing", "refusals=%s" % refusals)
        pin(inherited == ["shared.json"],
            "(a) a seeded path the lane never touched is INHERITED, not folded back",
            "inherited=%s" % inherited)
        pin(mine == ["a file - with spaces.md", "tracked.txt"],
            "(c)+(d) unseeded paths fold, and a C-QUOTED path is not lost",
            "mine=%s" % mine)

        # (b) the main tree drifts under the lane.
        write_atomic(os.path.join(root, "tracked.txt"), "someone else\n")
        mine2, _del2, _inh2, refusals2, _s2, _c2 = classify(root, wt, seed)
        pin(len(refusals2) == 1 and "DRIFTED from the lane's base" in refusals2[0]
            and "by an UNCOMMITTED EDIT" in refusals2[0],
            "(b) a destination that drifted from the lane's base is REFUSED, and the "
            "refusal says it was an uncommitted edit", "got=%s" % refusals2)
        pin(cmd_fold(root, "x", apply_it=True) == 2,
            "(b) and the refusal is ALL-OR-NOTHING -- the fold returns 2", "")
        pin(io.open(os.path.join(root, "a file - with spaces.md"),
                    encoding="utf-8").read() == "base\n",
            "(b) nothing was written before the refusal", "")
        pin(mine2 == ["a file - with spaces.md"],
            "(b) the non-drifted paths are still measured as this lane's", "got=%s" % mine2)

        # the seeded document drifts too -- the two-lanes-on-one-file case.
        write_atomic(os.path.join(wt, "shared.json"), '{"from":"lane-two"}\n')
        write_atomic(os.path.join(root, "shared.json"), '{"from":"folded-one"}\n')
        _m3, _d3, _i3, refusals3, _s3, _c3 = classify(root, wt, seed)
        pin(any("DRIFTED since seeding" in r and "shared.json" in r for r in refusals3),
            "two lanes on ONE shared document REFUSES rather than reverting the first",
            "got=%s" % refusals3)

        # ── `--settled`, and the arm ORDER is the argument for it ──────────────────
        # (k) The refusal above used to end the story: the message told the caller to
        # merge by hand and re-run, and a bare re-run REFUSES IDENTICALLY because the
        # drift test compares the DESTINATION to the SEED -- a hand-merge moves the
        # destination FURTHER from the seed, never back. ✔MEASURED 2026-09-02 (P54),
        # twice in one cycle. So the promised remedy is pinned here as a real one.
        _m4, _d4, _i4, refusals4, settled4, _c4 = classify(
            root, wt, seed, settled=("shared.json",))
        # ⚠ The assertion is "no refusal NAMES shared.json", not "no refusals at all":
        # by this point the fixture carries an unrelated drift on `tracked.txt` from an
        # earlier arm, and a blanket `not refusals4` would pass or fail on that instead
        # of on the property under test.
        pin(not any("shared.json" in r for r in refusals4) and settled4 == ["shared.json"],
            "(k) --settled drops the reconciled path so the lane's OTHER work can land",
            "refusals=%s settled=%s" % (refusals4, settled4))
        pin(_m4 == ["a file - with spaces.md"],
            "(k2) ...and the paths it did NOT name are still folded",
            "got=%s" % _m4)
        # (l) THE CONTROL, because a flag that silently skips is worse than a refusal:
        # the settled path must NOT be written, and the destination must keep the
        # content the hand-merge left there.
        cmd_fold(root, "x", apply_it=True, settled=("shared.json",))
        pin(io.open(os.path.join(root, "shared.json"), encoding="utf-8").read()
            == '{"from":"folded-one"}\n',
            "(l) a --settled path is NOT written -- the hand-merged content survives",
            "got=%r" % io.open(os.path.join(root, "shared.json"),
                               encoding="utf-8").read())
        write_atomic(os.path.join(root, "shared.json"), '{"from":"folded-one"}\n')

        # ── `refresh-plans`, and the arm ORDER carries the argument ────────────────
        # (m) The orchestrator applies a row to `.plans/` MID-CYCLE, so a live lane's
        # copy goes stale and its `anchor_registry_guard` reds for an anchor that IS
        # registered. Refreshing must fix that WITHOUT the fold then trying to write the
        # lane's stale registry back over the live one -- which is why the manifest is
        # updated in the same step, making the path INHERITED by construction.
        os.makedirs(os.path.join(root, ".plans"), exist_ok=True)
        os.makedirs(os.path.join(wt, ".plans"), exist_ok=True)
        write_atomic(os.path.join(root, ".plans", "reg.md"), "row A\nrow B\n")
        write_atomic(os.path.join(wt, ".plans", "reg.md"), "row A\n")
        seed_before = load_manifest(manifest_path(root, "x")).paths
        cmd_refresh_plans(root, "x", apply_it=True)
        seed_after = load_manifest(manifest_path(root, "x")).paths
        pin(io.open(os.path.join(wt, ".plans", "reg.md"), encoding="utf-8").read()
            == "row A\nrow B\n",
            "(m) refresh-plans carries a mid-cycle registry edit into a LIVE lane")
        pin(seed_after.get(".plans/reg.md") != seed_before.get(".plans/reg.md")
            and seed_after.get(".plans/reg.md") is not None,
            "(m2) ...and UPDATES the manifest, so the fold subtracts it as inherited",
            "before=%r after=%r" % (seed_before.get(".plans/reg.md"),
                                    seed_after.get(".plans/reg.md")))
        _m5, _d5, inh5, _r5, _s5, _c5 = classify(
            root, wt, load_manifest(manifest_path(root, "x")).paths)
        pin(".plans/reg.md" in inh5,
            "(m3) CONTROL: the refreshed path is INHERITED at fold time, not written back",
            "inherited=%s" % [q for q in inh5 if q.startswith(".plans/")])
        # (m4) THE REFUSAL: if the LANE itself edited the file, refreshing would discard
        # that edit -- so it must refuse rather than silently overwrite.
        write_atomic(os.path.join(root, ".plans", "reg.md"), "row A\nrow B\nrow C\n")
        write_atomic(os.path.join(wt, ".plans", "reg.md"), "the lane wrote this\n")
        try:
            cmd_refresh_plans(root, "x", apply_it=True)
            refused_lane_edit = False
        except SystemExit as exc:
            refused_lane_edit = exc.code == 2
        pin(refused_lane_edit
            and io.open(os.path.join(wt, ".plans", "reg.md"),
                        encoding="utf-8").read() == "the lane wrote this\n",
            "(m4) ...and REFUSES when the lane's own copy diverged from its seed, "
            "leaving that copy untouched")

        # (e) THE REFUSAL THAT PROTECTS A RUNNING LANE. By this point the lane has
        # edits of its own, so a second `seed` must refuse rather than overwrite
        # them. This is the arm with teeth: every other failure here costs a rerun,
        # this one costs a lane its work with no diff to show for it.
        try:
            cmd_seed(root, "x")
            refused_live = False
        except SystemExit as exc:
            refused_live = exc.code == 2
        pin(refused_live,
            "(e) seeding a worktree that already has its OWN changes is REFUSED",
            "")

        # (f) `--empty` records without copying, and it does NOT trip (e): a lane
        # created at a commit and handed nothing is exactly what it describes.
        before = md5_file(os.path.join(wt, "tracked.txt"))
        cmd_seed(root, "x", empty=True)
        empty_manifest = load_manifest(manifest_path(root, "x"))
        pin(empty_manifest.paths == {} and empty_manifest.base == lane_head(wt)
            and md5_file(os.path.join(wt, "tracked.txt")) == before,
            "(f) --empty writes an EMPTY manifest that still RECORDS the lane's base, "
            "copies nothing, and does NOT trip (e) -- the guard is on the copy, not on "
            "the verb", "manifest=%r" % (empty_manifest,))

        # (g) AN UNTRACKED DIRECTORY IS ENUMERATED BY GIT, SO `.gitignore` IS
        # HONOURED INSIDE IT.
        #
        # ⚠ `git status --porcelain` reports an untracked directory as ONE bare
        # entry, never as its files. The predecessor answered that with `os.walk`,
        # which takes everything on disk and asks git nothing -- so a lane's build
        # droppings folded alongside its work.
        # ✔MEASURED 2026-08-29 on the live fold of lane `cx`: two
        # `__pycache__/*.pyc` were offered as the lane's own work, gitignored and
        # invisible to `git status` in the main tree, because the script directories
        # holding them were new and therefore untracked, and the fold walked them
        # past `.gitignore`.
        #
        # ⚠ THE IGNORE FILE GOES IN THE LANE WORKTREE ONLY. The root fixture still
        # has none, and that absence is load-bearing: it is what proves the
        # `is_lane_tree` floor does not lean on an ignore file it does not own.
        # ★ MEMBERSHIP, NOT EQUALITY, and placed last: three arms above assert
        # equality on the measured set, and this one must not disturb what they
        # are about.
        write_atomic(os.path.join(wt, ".gitignore"), "__pycache__/\n*.pyc\n")
        write_atomic(os.path.join(wt, "newdir", "kept.txt"), "lane work\n")
        write_atomic(os.path.join(wt, "newdir", "__pycache__", "dropped.pyc"),
                     "build artefact\n")
        seen = set(changed_paths(wt))
        pin("newdir/kept.txt" in seen
            and "newdir/__pycache__/dropped.pyc" not in seen,
            "(g) an untracked directory yields its real file and NOT the path its "
            "own .gitignore excludes",
            "kept=%s dropped=%s"
            % ("newdir/kept.txt" in seen,
               "newdir/__pycache__/dropped.pyc" in seen))

        # (h) A DELETION THE LANE MADE IS CARRIED, AND A DELETION OVER DRIFT IS NOT.
        # Before this, `classify` hit
        # `if not os.path.isfile(src): continue` and the removal simply vanished --
        # the fold reported success having kept the file. ✔That happened for real on
        # lane `al`, whose replaced error example survived a fold that printed 17
        # written paths and no mention of the removal.
        # ★ The REMOVE direction is the whole point: the arm deletes the file in the
        # lane and requires the fold to SEE it, then dirties the main tree's copy and
        # requires the fold to REFUSE. A pin that only checked the happy path would
        # pass over a tool that deletes drifted work.
        # ⓘ The fixture's root and worktree are INDEPENDENT repositories, so the
        # subject has to be committed in each before the lane can delete it -- that
        # is what makes `git status` in the lane say `D` rather than "untracked".
        # ⚠ Assertions here name the SUBJECT PATH rather than requiring an empty
        # refusal set: arm (b) deliberately leaves the main tree drifted, and a pin
        # that demanded global cleanliness would be measuring that instead of this.
        for repo in (root, wt):
            write_atomic(os.path.join(repo, "todelete.txt"), "doomed\n")
            git_run(["-C", repo, "add", "todelete.txt"], capture_output=True, check=True)
            git_run(["-C", repo, "commit", "-q", "-m", "add todelete"],
                    capture_output=True, check=True)
        os.remove(os.path.join(wt, "todelete.txt"))
        m_del, d_del, _i, r_del, _s4, _c4 = classify(root, wt, {})
        pin("todelete.txt" in d_del and "todelete.txt" not in m_del
            and not [x for x in r_del if "todelete.txt" in x],
            "(h) a path the lane DELETED is measured as a deletion, not dropped",
            "deleted=%s mine=%s refusals=%s" % (d_del, m_del, r_del))

        write_atomic(os.path.join(root, "todelete.txt"), "someone else edited this\n")
        _m, d_drift, _i2, r_drift, _s5, _c5 = classify(root, wt, {})
        pin("todelete.txt" not in d_drift
            and any("refusing to DELETE" in x and "todelete.txt" in x for x in r_drift),
            "(h) a deletion whose destination DRIFTED is REFUSED, not carried out",
            "deleted=%s refusals=%s" % (d_drift, r_drift))

        # (i0)-(i4) THE ROOT IS THE TREE THIS FILE LIVES IN -- whatever the caller's cwd, and
        # whatever git environment the caller exported.
        # Arms, oracle and synthesized negatives are owned by .harness-config/runner/actions/owning-tree/owning-tree.py:
        # (i0) the CONTROL from this tree; (i1) a cwd inside ANOTHER repository; (i2) a cwd inside
        # NO repository; (i3) a caller's GIT_DIR + GIT_WORK_TREE + GIT_INDEX_FILE naming another
        # tree; (i4) this file copied into a tree nested inside another checkout, REFUSED.
        # ✔MEASURED 2026-09-15 (P66 round 3), before `repo_root` moved onto that owner: the cwd
        # cases held, and a steering caller moved the root into this script's own directory or
        # into another repository. ★ Printed when they HOLD as well, so a red-on-disable transcript
        # SHOWS the control staying green.
        for n, (ok, why, detail) in enumerate(
                _owning_tree().root_arms(repo_root, (SystemExit,), True, __file__)):
            pin(ok, "(i%d) %s" % (n, why), "" if ok else detail)

        # (j) `--repo <path>` IS THE EXPLICIT ESCAPE HATCH, and the CONTROL for (i).
        # ⚠ Without it, (i) passes over a `repo_root` that had simply stopped being
        # able to reach any tree but its own -- the capability the old cwd-keying
        # provided BY ACCIDENT would have been removed rather than named, and
        # nothing would have measured the difference.
        pin(os.path.realpath(repo_root(root)) == os.path.realpath(root),
            "(j) CONTROL: --repo <path> still reaches another tree, deliberately",
            "got=%s wanted=%s" % (repo_root(root), root))

    # ══ THE LANE'S BASE, AND LANDING -- on REAL `git worktree` lanes ══════════════
    # ⓘ A SECOND temporary directory, so nothing above can see these repositories in its
    # own `git status`, and nothing here inherits a drift an arm above left on purpose.
    with tempfile.TemporaryDirectory() as tmp2:
        top = os.path.realpath(tmp2)
        IGNORE = "/.worktrees/\n.temp/\nscratchpad/\n__pycache__/\n"

        def git(repo, *args):
            return git_run(["-C", repo] + list(args), capture_output=True, check=True)

        def make_repo(path, files, **fixture_layout):
            os.makedirs(path, exist_ok=True)
            git_run(["init", "-q", path], capture_output=True, check=True)
            git(path, "config", "user.email", "selftest@example.invalid")
            git(path, "config", "user.name", "lane-fold self-test")
            # ⓘ Pinned LOCALLY, so a host whose global config converts line endings on
            # checkout cannot make a lane's files differ from the blobs they came from.
            git(path, "config", "core.autocrlf", "false")
            for rel, body in files:
                write_atomic(os.path.join(path, *rel.split("/")), body)
            # ★ Every repository carries the files `layout` reads -- THIS tree's own unless an
            # arm overrides a value -- committed with the base, as a real tree tracks them.
            _fixture_layout(path, **fixture_layout)
            git(path, "add", "-A")
            git(path, "commit", "-q", "-m", "base")

        def make_lane(repo, lane):
            git(repo, "worktree", "add", "--detach", "%s/%s" % (layout(repo).worktrees, lane),
                "HEAD")
            with contextlib.redirect_stdout(io.StringIO()):
                cmd_seed(repo, lane, empty=True)
            return worktree_path(repo, lane)

        def quiet(fn, *a, **k):
            """-> (return value, or the SystemExit code; everything it printed)."""
            sink = io.StringIO()
            try:
                with contextlib.redirect_stdout(sink):
                    rv = fn(*a, **k)
            except SystemExit as exc:
                rv = exc.code
            return rv, sink.getvalue()

        def text(path):
            with io.open(path, encoding="utf-8", newline="") as fh:
                return fh.read()

        # ── (y1)-(y7): WHERE THE LANES AND THE BOOKKEEPING LIVE IS READ, NEVER ASSUMED ───────
        # ★ The red-on-disable for `layout`: a tree whose files name OTHER directories -- lanes
        # under `lanes/`, seeds in `.seeds`, kept evidence in `.kept`, evidence under `notes/` --
        # and every verb must go THERE. A tool that kept `.worktrees`, `.manifests`, `.evidence`
        # or the old evidence roots reds on every pin below; so does one that reads its seed
        # directory anywhere but where `lane-worktree add` writes it.
        ry = os.path.join(top, "layout")
        make_repo(ry, [(".gitignore", "/lanes/\nnotes/\n__pycache__/\n"), ("Y.txt", "y base\n")],
                  worktrees="lanes", evidence_roots=("notes",),
                  lane_fold={"manifests": ".seeds", "evidence": ".kept"})
        lay_y = layout(ry)
        pin(lay_y == Layout("lanes", "lanes/.seeds", "lanes/.kept", ("notes",)),
            "(y1) the layout is READ from the tree's own files: lanes/, lanes/.seeds, lanes/.kept, "
            "and evidence under notes/", "got=%r" % (lay_y,))
        wy = make_lane(ry, "y")
        pin(wy == os.path.join(ry, "lanes", "y")
            and os.path.isfile(os.path.join(ry, "lanes", ".seeds", "seed-y.json"))
            and not os.path.exists(os.path.join(ry, ".worktrees"))
            and not os.path.exists(os.path.join(ry, "lanes", ".manifests")),
            "(y2) the lane is under the configured root and its seed in the configured directory "
            "-- nothing under .worktrees/ or .manifests/", "wt=%s" % wy)
        write_atomic(os.path.join(wy, "notes", "n.log"), "a note\n")
        write_atomic(os.path.join(wy, "scratchpad", "s.log"), "not evidence in this tree\n")
        ev_y = evidence_digests(wy, lay_y.evidence_roots)
        pin(sorted(ev_y) == ["notes/n.log"],
            "(y3) evidence is measured under the CONFIGURED roots only: notes/, and not "
            "scratchpad/, which this tree does not name", "got=%s" % sorted(ev_y))
        dest_y = default_evidence_destination(ry, "y")
        pin(dest_y.startswith(os.path.join(ry, "lanes", ".kept", "y-")),
            "(y4) `land` keeps a removed lane's evidence in the configured directory by default",
            "got=%s" % dest_y)
        pin(is_lane_tree("lanes/y/Y.txt", lay_y.worktrees)
            and not is_lane_tree(".worktrees/y/Y.txt", lay_y.worktrees),
            "(y5) seed and fold never carry a path under the CONFIGURED lanes root -- and a "
            "directory merely sharing the old name is ordinary content", "")
        rv, out = quiet(cmd_list, ry)
        pin(rv == 0 and "worktrees under lanes/: y" in out and "seed manifests:      y" in out,
            "(y6) `list` reads the lanes and the seeds where the configuration puts them",
            "rv=%r out=%s" % (rv, out.strip()))
        lf_y = os.path.join(ry, LANE_FOLD_CONFIG_REL)
        good_lf = text(lf_y)
        for label, cfg, needle in (
                ("(y7) a lane-fold-config.json with no `evidence` is refused (exit 2), naming it",
                 {"manifests": ".seeds"}, "evidence is MISSING"),
                ("(y7) an `evidence` that is not dot-led is refused (exit 2) -- it could be taken "
                 "for a lane", {"manifests": ".seeds", "evidence": "kept"}, "evidence is str 'kept'"),
                ("(y7) an UNKNOWN key is refused (exit 2) -- a misspelt key is never ignored",
                 {"manifests": ".seeds", "evidence": ".kept", "evidance": ".x"},
                 "unknown key(s) evidance"),
                ("(y7) `manifests` and `evidence` naming ONE directory is refused (exit 2)",
                 {"manifests": ".seeds", "evidence": ".seeds"}, "both name '.seeds'"),
                ("(y7) a `manifests` lane-worktree refuses is refused here too (exit 2): one rule, "
                 "one reader", {"manifests": "seeds", "evidence": ".kept"},
                 "manifests is str 'seeds'")):
            write_atomic(lf_y, json.dumps(cfg, indent=2) + "\n")
            rv, out = quiet(cmd_list, ry)
            pin(rv == 2 and needle in out, label, "rv=%r out=%s" % (rv, out.strip()[-300:]))
        write_atomic(lf_y, good_lf)
        rv, out = quiet(cmd_list, ry)
        pin(rv == 0 and "worktrees under lanes/: y" in out,
            "(y7) CONTROL: with the tree's own file back, `list` answers again",
            "rv=%r out=%s" % (rv, out.strip()))

        # ── (q)(p)(n)(o): THE BASE COMMIT ────────────────────────────────────────
        r2 = os.path.join(top, "base")
        make_repo(r2, [(".gitignore", IGNORE), ("F.txt", "line1\nline2\nline3\n"),
                       ("G.txt", "G base\n"), ("H.txt", "H base\n"), ("I.txt", "I base\n")])
        wp, wq, wr = make_lane(r2, "p"), make_lane(r2, "q"), make_lane(r2, "r")

        # (q1) the manifest RECORDS the base -- the one fact every arm below depends on.
        raw_q = json.loads(text(manifest_path(r2, "q")))
        pin(raw_q.get("format") == MANIFEST_FORMAT and raw_q.get("base") == lane_head(wq)
            and raw_q.get("paths") == {},
            "(q1) seed writes manifest format %d, recording the lane's own base commit"
            % MANIFEST_FORMAT, "got=%r" % raw_q)

        write_atomic(os.path.join(wp, "F.txt"), "line1 EDITED-BY-P\nline2\nline3\n")
        write_atomic(os.path.join(wp, "G.txt"), "G EDITED-BY-P\n")
        rv, out = quiet(cmd_fold, r2, "p", True)
        pin(rv == 0 and text(os.path.join(r2, "F.txt")).startswith("line1 EDITED-BY-P"),
            "(n0) setup: lane p folds its two edits", "rv=%r out=%s" % (rv, out[-300:]))

        # (p1) CONVERGENCE, taken BEFORE the commit, where the old code refused: lane p's
        # own paths now equal the main tree's bytes, which is what a re-run of an
        # interrupted landing meets.
        mp = load_manifest(manifest_path(r2, "p"))
        cp = classify(r2, wp, mp.paths, base=mp.base)
        pin(cp.converged == ["F.txt", "G.txt"] and not cp.mine and not cp.refusals,
            "(p1) a lane path the main tree ALREADY holds byte-identically is ALREADY "
            "LANDED, not drift -- so a landing that failed after its fold can be re-run",
            "converged=%s mine=%s refusals=%s" % (cp.converged, cp.mine, cp.refusals))

        git(r2, "commit", "-q", "-am", "fold lane p")

        # Lane q overlaps lane p on F (an edit) and G (a deletion), edits H alone, and
        # edits I, which the main tree ALSO edits without committing.
        write_atomic(os.path.join(wq, "F.txt"), "line1\nline2\nline3 EDITED-BY-Q\n")
        os.remove(os.path.join(wq, "G.txt"))
        write_atomic(os.path.join(wq, "H.txt"), "H EDITED-BY-Q\n")
        write_atomic(os.path.join(wq, "I.txt"), "I EDITED-BY-Q\n")
        write_atomic(os.path.join(r2, "I.txt"), "I EDITED IN MAIN, uncommitted\n")
        mq = load_manifest(manifest_path(r2, "q"))
        cq = classify(r2, wq, mq.paths, base=mq.base)
        pin("F.txt" not in cq.mine
            and any("F.txt" in x and "by a COMMIT" in x for x in cq.refusals),
            "(n1) a path a COMMITTED sibling fold changed since this lane's base is "
            "REFUSED, never written over the commit", "mine=%s refusals=%s"
            % (cq.mine, cq.refusals))
        pin("G.txt" not in cq.deleted
            and any("G.txt" in x and "refusing to DELETE" in x and "by a COMMIT" in x
                    for x in cq.refusals),
            "(n2) ...and the DELETION branch refuses the same way -- the committed edit "
            "is not deleted", "deleted=%s refusals=%s" % (cq.deleted, cq.refusals))
        pin(any("I.txt" in x and "by an UNCOMMITTED EDIT" in x for x in cq.refusals)
            and not any("I.txt" in x and "by a COMMIT" in x for x in cq.refusals),
            "(n3) the refusal NAMES the kind of drift: an uncommitted edit is not "
            "reported as a commit", "refusals=%s" % cq.refusals)
        # ⓘ MEMBERSHIP of its own path, not equality of the whole set: a control that also
        # asserts what (n1) is about goes red WITH (n1) and stops being a control (✔MEASURED
        # by mutant M1a, whose F.txt joined `mine` and took this arm down beside (n1)).
        pin("H.txt" in cq.mine and not any("H.txt" in x for x in cq.refusals),
            "(n4) CONTROL: a path only this lane changed still folds -- the base check "
            "does not refuse everything", "mine=%s" % cq.mine)
        rv, out = quiet(cmd_fold, r2, "q", True)
        pin(rv == 2 and "EDITED-BY-P" in text(os.path.join(r2, "F.txt"))
            and os.path.isfile(os.path.join(r2, "G.txt"))
            and text(os.path.join(r2, "G.txt")) == "G EDITED-BY-P\n"
            and text(os.path.join(r2, "H.txt")) == "H base\n",
            "(n5) and the fold writes NOTHING: both of lane p's committed edits survive",
            "rv=%r" % rv)

        # (k3) `--settled` REACHES A REFUSED DELETION TOO -- the remedy the refusal prints.
        # [found by an independent review, P66] The settled check used to sit below the
        # deletion branch, whose refusals `continue` first, so a deletion could never be
        # settled and the lane's other work stayed blocked behind it. Lane q's deletion of
        # G.txt is refused above (n2); here the orchestrator has kept lane p's committed G.txt.
        settled_q = classify(r2, wq, mq.paths, settled=("G.txt",), base=mq.base)
        pin("G.txt" in settled_q.settled and "G.txt" not in settled_q.deleted
            and not any("G.txt" in x for x in settled_q.refusals)
            and any("F.txt" in x for x in settled_q.refusals),
            "(k3) --settled drops a refused DELETION from the lane's change set, and only that "
            "path", "settled=%s refusals=%s" % (settled_q.settled, settled_q.refusals))

        # (o1) A COMMIT INSIDE A LANE. Its change is no longer in `git status`, so the
        # old fold reported "0 path(s) are this lane's" and exited 0 over lost work.
        write_atomic(os.path.join(wr, "J.txt"), "J committed inside the lane\n")
        git(wr, "add", "J.txt")
        git(wr, "commit", "-q", "-m", "a commit a lane should never make")
        base_r = load_manifest(manifest_path(r2, "r")).base
        rv, out = quiet(cmd_fold, r2, "r", False)
        pin(rv == 2 and "HEAD" in out and base_r[:12] in out
            and not os.path.exists(os.path.join(r2, "J.txt")),
            "(o1) a lane whose HEAD moved past its recorded base is REFUSED -- its "
            "committed work is invisible to `git status`", "rv=%r out=%s" % (rv, out[-300:]))

        # (o2) CONTROL: a FORMAT-1 manifest -- what every live lane carried before P66 --
        # still folds, and says where its base came from.
        write_atomic(manifest_path(r2, "p"), "{}")
        rv, out = quiet(cmd_fold, r2, "p", False)
        pin(rv == 0 and "format 1" in out,
            "(o2) CONTROL: a format-1 manifest still folds, and says its base is the lane's "
            "HEAD", "rv=%r out=%s" % (rv, out[-300:]))

        # (o3) ...AND A FORMAT-1 MANIFEST NO LONGER HIDES THAT COMMIT. It records no base, so the
        # HEAD must hold no commit that no ref of the repository reaches -- and lane r's holds one.
        # ✔REPRODUCED before the fix: `land` on such a lane exited 0 and orphaned the commit.
        # The manifest is put back afterwards: (s1) reads lane r's manifest byte for byte.
        manifest_r_o3 = text(manifest_path(r2, "r"))
        write_atomic(manifest_path(r2, "r"), "{}")
        rv, out = quiet(cmd_fold, r2, "r", False)
        write_atomic(manifest_path(r2, "r"), manifest_r_o3)
        pin(rv == 2 and "format 1" in out and "a commit a lane should never make" in out
            and not os.path.exists(os.path.join(r2, "J.txt")),
            "(o3) a FORMAT-1 lane whose HEAD holds a commit no ref reaches is REFUSED, naming the "
            "commit -- the manifest records no base to compare, and `git status` still cannot "
            "see it", "rv=%r out=%s" % (rv, out[-300:]))

        # (o4) ...and when git CANNOT list those commits (one of their objects is missing), the
        # format-1 lane is REFUSED too, never taken as holding none.
        r5 = os.path.join(top, "orphans")
        make_repo(r5, [(".gitignore", IGNORE), ("A.txt", "a\n")])
        w5 = make_lane(r5, "m")
        for n in ("one", "two"):
            write_atomic(os.path.join(w5, n + ".txt"), n + "\n")
            git(w5, "add", n + ".txt")
            git(w5, "commit", "-q", "-m", "lane m commit " + n)
        older = git(w5, "rev-parse", "HEAD~1").stdout.decode("ascii", "replace").strip()
        loose = os.path.join(r5, ".git", "objects", older[:2], older[2:])
        constructed = os.path.isfile(loose)
        if constructed:
            os.chmod(loose, 0o666)
            os.remove(loose)
        write_atomic(manifest_path(r5, "m"), "{}")
        rv, out = quiet(cmd_fold, r5, "m", False)
        pin(constructed and rv == 2 and "cannot list the commits" in out,
            "(o4) ...and a format-1 lane whose commits git CANNOT list (a missing object) is "
            "REFUSED, not taken as holding none", "constructed=%s rv=%r out=%s"
            % (constructed, rv, out[-300:]))

        # (q2) a manifest that CLAIMS format 2 without a base is refused, never guessed at.
        write_atomic(manifest_path(r2, "q"), '{"format": 2, "paths": {}}')
        rv, out = quiet(load_manifest, manifest_path(r2, "q"))
        pin(rv == 2 and "format" in out,
            "(q2) a manifest declaring format 2 with no base is REFUSED", "rv=%r out=%s"
            % (rv, out))

        # (s1)-(s3) AN OPTION A VERB DOES NOT KNOW IS REFUSED, NEVER IGNORED.
        # ⓘ Lane r is the subject on purpose: its one change was committed, so its `git status`
        # is clean, and the main tree still carries an uncommitted I.txt -- a `seed` that
        # ignored `--emtpy` WOULD copy I.txt in and rewrite the manifest, which is what lets
        # (s1) fail.
        manifest_r = text(manifest_path(r2, "r"))
        rv, out = quiet(main, ["--repo", r2, "seed", "r", "--emtpy"])
        pin(rv == 3 and "--emtpy" in out and text(manifest_path(r2, "r")) == manifest_r
            and text(os.path.join(wr, "I.txt")) == "I base\n",
            "(s1) `seed <lane> --emtpy` is REFUSED as an unknown option, and nothing is copied",
            "rv=%r out=%s" % (rv, out[-300:]))
        rv, out = quiet(main, ["--repo", r2, "fold", "p", "--aply"])
        pin(rv == 3 and "--aply" in out,
            "(s2) `fold <lane> --aply` is REFUSED -- it used to be a silent dry run",
            "rv=%r out=%s" % (rv, out[-300:]))
        rv, out = quiet(main, ["--repo", r2, "fold", "p"])
        pin(rv == 0,
            "(s3) CONTROL: the same verb with a valid invocation still runs",
            "rv=%r out=%s" % (rv, out[-300:]))

        # (t0)-(t3) A LINE-ENDING CONVERSION IS NOT DRIFT, AND A REAL EDIT STILL IS.
        # [found by an independent review, P66] Unseeded paths were compared as RAW bytes, so a
        # main tree CHECKED OUT under core.autocrlf=true -- CRLF on disk over LF blobs, files
        # `git status` calls unchanged -- read as DRIFTED. The main tree here is a CLONE made with
        # autocrlf ON (every other fixture in this self-test pins it off).
        # ⚠ A CLONE, NOT CRLF BYTES WRITTEN OVER A JUST-COMMITTED FILE: that construction leaves
        # the index holding the LF size, and `git status` then reports ` M` from the size change
        # alone (✔MEASURED while reproducing the finding), so it would not be the case under test.
        # (t0) checks the fixture really is that case before anything is measured against it.
        src4 = os.path.join(top, "eol-src")
        make_repo(src4, [(".gitignore", IGNORE), ("T.txt", "a\nb\n"), ("U.txt", "x\n"),
                         ("V.txt", "v\nw\n")])
        r4 = os.path.join(top, "eol")
        git_run(["clone", "-q", "-c", "core.autocrlf=true", src4, r4],
                capture_output=True, check=True)
        git(r4, "config", "user.email", "selftest@example.invalid")
        git(r4, "config", "user.name", "lane-fold self-test")
        status4 = git(r4, "status", "--porcelain").stdout.decode("utf-8", "replace")
        with io.open(os.path.join(r4, "T.txt"), "rb") as fh:
            t_bytes = fh.read()
        pin(t_bytes == b"a\r\nb\r\n" and status4.strip() == "",
            "(t0) CONTROL: the fixture is the case under test -- CRLF on disk, and `git status` "
            "calls every file unchanged", "bytes=%r status=%r" % (t_bytes, status4))
        w4 = make_lane(r4, "e")
        write_atomic(os.path.join(r4, "V.txt"), "v\r\nEDITED IN MAIN\r\n")
        write_atomic(os.path.join(w4, "T.txt"), "a\nb\nlane edit\n")
        os.remove(os.path.join(w4, "U.txt"))
        write_atomic(os.path.join(w4, "V.txt"), "v\nw\nlane edit\n")
        m4 = load_manifest(manifest_path(r4, "e"))
        c4 = classify(r4, w4, m4.paths, base=m4.base)
        pin("T.txt" in c4.mine and not any("T.txt" in x for x in c4.refusals),
            "(t1) a main-tree file holding the committed content with CRLF endings under "
            "core.autocrlf=true is NOT drift -- the lane's edit folds",
            "mine=%s refusals=%s" % (c4.mine, c4.refusals))
        pin("U.txt" in c4.deleted and not any("U.txt" in x for x in c4.refusals),
            "(t2) ...and the same holds for a DELETION of such a file",
            "deleted=%s refusals=%s" % (c4.deleted, c4.refusals))
        pin(any("V.txt" in x and "by an UNCOMMITTED EDIT" in x for x in c4.refusals)
            and "V.txt" not in c4.mine,
            "(t3) CONTROL: a REAL edit in the main tree, under the same conversion, is REFUSED",
            "refusals=%s" % c4.refusals)

        # ── (r)(l): LANDING, against FIXTURE registries and the REAL row writer ─────
        # ⚠ The registries are fixtures in a throwaway repository, never `.plans/` of any
        # real tree. The writer is a COPY of this tree's own `.harness-config/runner/actions/anchors/` together
        # with EVERY script it loads, so the arms exercise the bytes that will really run.
        # ★ THE LIST IS THE WRITER'S LOAD CLOSURE, AND `load_anchors_module` BELOW VERIFIES
        # IT: a directory missing here makes the copied writer refuse to load, loudly.
        # ✔MEASURED 2026-09-15 (P66): this listed three directories. Lane `rr` moved
        # `check-anchor-balance` and `burndown-queue` onto `owning-tree`, and lane `ge` moved
        # `anchors` itself, so on the tree holding every lane the copy refused ("cannot find
        # .../.harness-config/runner/actions/owning-tree/owning-tree.py"). No lane could see it: `lf`'s own tree held
        # no `owning-tree` consumer, and the lanes that added them gated an older lane-fold.
        # ⚠ Fixture anchor ids are ASSEMBLED from fragments: an id written whole here would
        # be a citation to every anchor scan that reads this file.
        # ★ The writer's closure is copied from THIS file's own sibling directories -- the
        # actions directory it lives in -- into the SAME layout under the fixture, never
        # from a tree root derived by counting `..` (that count named
        # `.harness-config/runner` the day these programs moved out of `scripts/`).
        here_actions = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
        r3 = os.path.join(top, "land")
        for d in ("anchors", "check-anchor-balance", "burndown-queue", "owning-tree"):
            shutil.copytree(os.path.join(here_actions, d),
                            os.path.join(r3, ACTIONS_REL, d),
                            ignore=shutil.ignore_patterns("__pycache__", "build", "artifacts"))
        # ★ `land` removes through `lane-worktree`, which reads the worktrees root, the evidence
        # roots and the seed directory from the ACTED-ON tree (`.harness-config/config.json` and
        # `lane-fold-config.json`) and has no defaults -- so the fixture carries THIS tree's own
        # files (`make_repo` puts them there), and a root they drop fails (l1b) here.
        FX = "D-" + "FIXTURE-LANDFOLD"
        HDR = "| Anchor | Priority | Status | Trigger | Closing work | Cross-refs |"
        SEP = "|---|---|---|---|---|---|"
        registries = [
            (".plans/_deferred-anchor-registry-production.md", "\n".join([
                "# p", "", HDR, SEP,
                "| `%s-EXISTING` | P2 | 🟠 OPEN | 🟠 **OPEN** existing row | w | r |" % FX,
                "| `%s-SECOND` | P2 | 🟠 OPEN | 🟠 **OPEN** second existing row | w | r |" % FX,
                ""])),
            (".plans/_deferred-anchor-registry-done.md", "\n".join([
                "# d", "", "## Closed — Production", "", HDR, SEP,
                "| `%s-OLDP` | P2 | ✅ CLOSED | ✅ **CLOSED** old | - | r |" % FX, "",
                ""]))]
        make_repo(r3, [(".gitignore", IGNORE), ("tracked.txt", "base\n")] + registries)
        A3 = load_anchors_module(r3)
        REGS = [os.path.join(r3, *rel.split("/")) for rel, _ in registries]

        def reg_now():
            return [text(p) for p in REGS]

        def cells(wt_, lane_, anchor, status, trigger, closing="closing text",
                  crossrefs="cross refs", bucket=None, priority=None):
            d = row_dir(wt_, lane_)
            os.makedirs(d, exist_ok=True)
            for suffix, body in (("status", status), ("trigger", trigger),
                                 ("closing", closing), ("crossrefs", crossrefs),
                                 ("bucket", bucket), ("priority", priority)):
                p = os.path.join(d, "%s.%s" % (anchor, suffix))
                if body is None:
                    if os.path.exists(p):
                        os.remove(p)
                else:
                    write_atomic(p, body + "\n")

        def drop(wt_, lane_, anchor):
            for suffix in ROW_SUFFIXES:
                p = os.path.join(row_dir(wt_, lane_), "%s.%s" % (anchor, suffix))
                if os.path.exists(p):
                    os.remove(p)

        real_writer = globals()["_run_writer"]
        writes = []

        def spy(root_, plan_, apply_it_):
            if apply_it_:
                writes.append(plan_.anchor)
            return real_writer(root_, plan_, apply_it_)

        wrows = make_lane(r3, "rows")
        NEW = FX + "-NEW"
        cells(wrows, "rows", NEW, "🟠 OPEN", "🟠 **OPEN** a new row | with a raw pipe",
              bucket="production", priority="P5")
        globals()["_run_writer"] = spy

        def run_rows(apply_it):
            """ONE rows arm's own run -> (rv, output, registries unchanged by THIS run).

            ⚠ Each arm takes its OWN registry snapshot and starts an EMPTY write log. ✔MEASURED
            by mutants M3a and M3d: with one snapshot and one log for the whole block, a mutant
            that made an early arm write took every later arm down with it -- the (r1g) and
            (r1b) CONTROLS included -- so a control measured a sibling's leftovers instead of
            its own run.
            """
            writes[:] = []
            snapshot = reg_now()
            rv_, out_ = quiet(cmd_apply_rows, r3, "rows", "production", apply_it)
            return rv_, out_, reg_now() == snapshot

        try:
            # (r1a) a stray draft beside a valid row.
            write_atomic(os.path.join(row_dir(wrows, "rows"), "lead.md"), "a draft\n")
            rv, out, unchanged = run_rows(True)
            pin(rv == 2 and "lead.md" in out and unchanged and not writes,
                "(r1a) a stray draft in row/ REFUSES the whole application and NO row is "
                "written", "rv=%r writes=%s out=%s" % (rv, writes, out[-300:]))
            os.remove(os.path.join(row_dir(wrows, "rows"), "lead.md"))

            # (r1b) an ORPHAN: every cell but `.status`.
            cells(wrows, "rows", FX + "-ORPHAN", "✅ CLOSED", "✅ **CLOSED** orphan row")
            os.remove(os.path.join(row_dir(wrows, "rows"), "%s-ORPHAN.status" % FX))
            rv, out, unchanged = run_rows(True)
            pin(rv == 2 and "ORPHAN.status" in out and unchanged and not writes,
                "(r1b) cells with NO .status -- an orphan row -- REFUSE, instead of silently "
                "never landing", "rv=%r out=%s" % (rv, out[-300:]))
            drop(wrows, "rows", FX + "-ORPHAN")

            # (r1c) a status the vocabulary cannot read.
            cells(wrows, "rows", FX + "-DRAFT", "draft -- decide later", "🟠 **OPEN** draft")
            rv, out, unchanged = run_rows(True)
            pin(rv == 2 and "DRAFT.status" in out and unchanged and not writes,
                "(r1c) a .status the registry vocabulary cannot read REFUSES -- it is never "
                "silently OPEN", "rv=%r out=%s" % (rv, out[-300:]))
            drop(wrows, "rows", FX + "-DRAFT")

            # (r1d) a `.bucket` that contradicts where an existing row lives. Since the
            # harness registry retired, the contradiction to construct is the ARCHIVE: an
            # OPEN row in the working registry, re-declared as if it were closed there.
            cells(wrows, "rows", FX + "-EXISTING", "🟠 OPEN",
                  "🟠 **OPEN** existing row, re-declared", bucket="done")
            rv, out, unchanged = run_rows(True)
            pin(rv == 2 and "EXISTING.bucket" in out and unchanged and not writes,
                "(r1d) a .bucket contradicting an EXISTING row's home REFUSES -- never "
                "silently ignored", "rv=%r out=%s" % (rv, out[-300:]))
            drop(wrows, "rows", FX + "-EXISTING")

            # ⓘ (r1e) RETIRED 2026-09-16, and this note is the arm. It pinned a refusal
            # on an ARCHIVED row re-filed with no `.bucket`: with TWO working registries the
            # archive could not derive which one a reopened row came from, so a missing
            # declaration was ambiguous. There is ONE working registry now, so the answer is
            # derivable, and refusing it would be refusing a question that has an answer. If a
            # second working registry ever returns, this arm returns with it.

            # (r1f) THE WRITER'S OWN REFUSAL on a LATER row: the valid row sorts first,
            # and it must not be written -- the partial application the scratchpad
            # applier still produced after its 2026-09-15 pre-check. ⓘ "No write call
            # during THIS run and the registries unchanged by it" is the whole property.
            cells(wrows, "rows", FX + "-ZZREFUSED", "✅ CLOSED",
                  "✅ **CLOSED** a pre-escaped \\| pipe")
            rv, out, unchanged = run_rows(True)
            pin(rv == 2 and "ZZREFUSED" in out and unchanged and not writes,
                "(r1f) a row the WRITER refuses stops EVERY row -- the valid row before it "
                "is never written", "rv=%r writes=%s out=%s" % (rv, writes, out[-300:]))
            drop(wrows, "rows", FX + "-ZZREFUSED")

            # (r1g) CONTROL: the same row/ with only a valid row passes its dry run.
            rv, out, unchanged = run_rows(False)
            pin(rv == 0 and unchanged and not writes,
                "(r1g) CONTROL: a row/ holding only valid rows passes the dry run -- the "
                "refusals above are not firing on everything", "rv=%r out=%s"
                % (rv, out[-300:]))
        finally:
            globals()["_run_writer"] = real_writer

        # (r2) THE APPLICATION: a new row, an existing row closed, an archived row amended.
        cells(wrows, "rows", FX + "-EXISTING", "✅ CLOSED",
              "✅ **CLOSED 2026-09-15** existing row closed", closing="closed by the self-test")
        cells(wrows, "rows", FX + "-OLDH", "✅ CLOSED", "✅ **CLOSED** old, amended",
              closing="amended closing", bucket="production")
        rv, out = quiet(cmd_apply_rows, r3, "rows", "production", True)
        new, ex, oh = A3.find(r3, NEW), A3.find(r3, FX + "-EXISTING"), A3.find(r3, FX + "-OLDH")
        pin(rv == 0 and len(new) == 1 and new[0].bucket == "production"
            and new[0].priority == "P5",
            "(r2a) a NEW row's .bucket and .priority are HONOURED",
            "rv=%r new=%s out=%s" % (rv, [(r.bucket, r.priority) for r in new], out[-400:]))
        pin(len(ex) == 1 and ex[0].bucket == "done" and ex[0].table == "production"
            and ex[0].priority == "P2",
            "(r2b) an EXISTING row keeps its own home and CARRIES its priority; closed, it "
            "moved to that home's archive table",
            "got=%s" % [(r.bucket, r.table, r.priority) for r in ex])
        pin(len(oh) == 1 and oh[0].table == "production"
            and _flat(oh[0].cell(A3.C_CLOSING)) == "amended closing",
            "(r2c) an ARCHIVED row with its declared bucket is replaced in its own table",
            "got=%s" % [(r.table, r.cell(A3.C_CLOSING)) for r in oh])
        pin(len(new) == 1
            and _flat(new[0].cell(A3.C_TRIGGER)) == "🟠 **OPEN** a new row | with a raw pipe",
            "(r2d) a stored cell re-reads EQUAL to its file -- a raw pipe included")

        # (r2e) A RE-RUN WRITES NOTHING -- including for the row that is now ARCHIVED and
        # has no `.bucket`: it already matches, so no filing decision is being made.
        after_r2 = reg_now()
        writes[:] = []
        globals()["_run_writer"] = spy
        try:
            rv, out = quiet(cmd_apply_rows, r3, "rows", "production", True)
        finally:
            globals()["_run_writer"] = real_writer
        pin(rv == 0 and reg_now() == after_r2 and not writes and "ALREADY LANDED" in out,
            "(r2e) a re-run finds every row ALREADY LANDED and writes nothing",
            "rv=%r writes=%s out=%s" % (rv, writes, out[-300:]))

        # (r3) ALL OR NOTHING SURVIVES A FAILURE THE DRY RUN COULD NOT FORESEE: the second
        # real write fails after the first one landed.
        # ⓘ A SECOND PRODUCTION ROW, not a harness one: the harness registry retired on
        # 2026-09-16 and `anchors.py` knows two buckets now. This arm never pinned the
        # bucket — it pins that a write failing AFTER an earlier one landed restores every
        # registry byte for byte, and it needs two closable rows to do that.
        cells(wrows, "rows", FX + "-SECOND", "✅ CLOSED", "✅ **CLOSED** a second existing row",
              priority="P4")
        cells(wrows, "rows", FX + "-NEW2", "✅ CLOSED", "✅ **CLOSED** a second new row")
        before_r3 = reg_now()
        calls = []

        def fail_second_write(root_, plan_, apply_it_):
            if apply_it_:
                calls.append(plan_.anchor)
                if len(calls) == 2:
                    return 1, "simulated failure on the second write of the application"
            return real_writer(root_, plan_, apply_it_)

        globals()["_run_writer"] = fail_second_write
        try:
            rv, out = quiet(cmd_apply_rows, r3, "rows", "production", True)
        finally:
            globals()["_run_writer"] = real_writer
        pin(rv == 2 and len(calls) == 2 and reg_now() == before_r3 and "RESTORED" in out,
            "(r3) a write that fails AFTER an earlier row landed RESTORES the registries "
            "byte-for-byte", "rv=%r calls=%s out=%s" % (rv, calls, out[-400:]))

        # (r4) THE WRITER'S SUCCESS IS NOT TAKEN ON TRUST: a row stored with cells other
        # than the lane's files fails verification.
        def wrong_closing(root_, plan_, apply_it_):
            if apply_it_ and plan_.anchor == FX + "-NEW2":
                alt = os.path.join(top, "wrong.closing")
                write_atomic(alt, "NOT what the lane wrote\n")
                files = dict(plan_.files)
                files["closing"] = alt
                return real_writer(root_, plan_._replace(files=files), apply_it_)
            return real_writer(root_, plan_, apply_it_)

        globals()["_run_writer"] = wrong_closing
        try:
            rv, out = quiet(cmd_apply_rows, r3, "rows", "production", True)
        finally:
            globals()["_run_writer"] = real_writer
        pin(rv == 2 and "VERIFY FAILED" in out and "NEW2" in out,
            "(r4) a row whose stored cells do not re-read as the lane's files FAILS "
            "verification", "rv=%r out=%s" % (rv, out[-400:]))

        # (r5) AN UPDATE NAMES ONLY WHAT CHANGES. A stored row whose closing cell holds a run
        # of spaces and an escaped pipe is CLOSED by the lane, whose closing file says the
        # same words with ONE space (a lane file's own spelling). That cell did not change, so
        # the door is not asked to write it, and its stored bytes survive the MOVE to the
        # archive. ⓘ Injected as RAW TEXT inside the table, the way the stored rows holding a
        # run were written, so the door is measured on a row it did not make.
        RUNS_ = FX + "-RUNS"
        raw_runs = ("| `%s` | P2 | 🟠 OPEN | 🟠 **OPEN** a runs row | see  for details \\| piped "
                    "| r |" % RUNS_)
        prod_text = text(REGS[0])
        write_atomic(REGS[0], prod_text.replace(SEP + "\n", SEP + "\n" + raw_runs + "\n", 1))
        # A lane of its own: the `rows` lane still holds (r4)'s deliberately mis-verified row.
        wrows2 = make_lane(r3, "rows2")
        cells(wrows2, "rows2", RUNS_, "✅ CLOSED", "✅ **CLOSED** a runs row",
              closing="see for details | piped", crossrefs="r")
        # (r6) ...and a NEW row with no `.priority` is given the sieve's band -- the door
        # requires one -- and the application SAYS it was seeded.
        cells(wrows2, "rows2", FX + "-SEEDED", "🟠 OPEN", "🟠 **OPEN** a row with no declared band")
        rv, out = quiet(cmd_apply_rows, r3, "rows2", "production", True)
        moved = [ln for ln in text(REGS[1]).split("\n") if (RUNS_ + "`") in ln]
        pin(rv == 0 and len(moved) == 1 and "| see  for details \\| piped |" in moved[0]
            and not any((RUNS_ + "`") in ln for ln in text(REGS[0]).split("\n")),
            "(r5) a lane closing a row does not rewrite a cell it did not change -- the stored "
            "run of spaces and escaped pipe survive the move byte for byte",
            "rv=%r moved=%s out=%s" % (rv, moved, out[out.find("REFUSED"):][:900] if "REFUSED" in out
                                       else out[-400:]))
        seeded = A3.find(r3, FX + "-SEEDED")
        pin(rv == 0 and len(seeded) == 1 and seeded[0].priority in ("P0", "P1", "P2", "P3", "P4", "P5")
            and "SEEDED from the burndown sieve" in out,
            "(r6) a NEW row with no .priority gets the burndown sieve's band, and the output "
            "says so", "rows=%s out=%s" % ([(r.priority, r.status) for r in seeded], out[-300:]))

        # (l1) THE WHOLE LANDING.
        wl1 = make_lane(r3, "land1")
        write_atomic(os.path.join(wl1, "work-land1.txt"), "lane land1 work\n")
        evidence = {".temp/land1-scratch/findings.log": "findings\n",
                    ".temp/land1-scratch/mutants/m1.log": "mutant transcript\n",
                    "scratchpad/pad.log": "pad evidence\n"}
        for rel, body in evidence.items():
            write_atomic(os.path.join(wl1, *rel.split("/")), body)
        cells(wl1, "land1", FX + "-LANDONE", "✅ CLOSED",
              "✅ **CLOSED 2026-09-15** landed by the self-test", bucket="production",
              priority="P3")
        dest1 = os.path.join(top, "evidence-land1")
        real_classify = globals()["classify"]
        real_remove_argv = globals()["lane_worktree_remove_argv"]
        events = []

        def spy_classify(*a, **k):
            result = real_classify(*a, **k)
            events.append(("classify", result))
            return result

        def spy_remove_argv(root_, lane_, destination_, verified_):
            # Recorded BEFORE delegating, so a call that then refuses is still seen.
            events.append(("remove-argv", verified_))
            argv_ = real_remove_argv(root_, lane_, destination_, verified_)
            events.append(("remove-argv-built", argv_))
            return argv_

        globals()["classify"] = spy_classify
        globals()["lane_worktree_remove_argv"] = spy_remove_argv
        try:
            rv, out = quiet(cmd_land, r3, "land1", "production", True, (), dest1)
        finally:
            globals()["classify"] = real_classify
            globals()["lane_worktree_remove_argv"] = real_remove_argv
        got = A3.find(r3, FX + "-LANDONE")
        pin(rv == 0 and os.path.isfile(os.path.join(r3, "work-land1.txt"))
            and len(got) == 1 and got[0].table == "production" and not os.path.exists(wl1),
            "(l1) land folds the work, applies and verifies the row, then removes the worktree",
            "rv=%r out=%s" % (rv, out[-700:]))
        pin(all(os.path.isfile(os.path.join(dest1, *rel.split("/")))
                and text(os.path.join(dest1, *rel.split("/"))) == body
                for rel, body in evidence.items()),
            "(l1b) ...and EVERY evidence file, under .temp/ AND scratchpad/, is at the "
            "destination byte-identical", "found=%s" % sorted(
                os.path.relpath(os.path.join(dp, f), dest1).replace(os.sep, "/")
                for dp, _d, fs in os.walk(dest1) for f in fs)[:8])

        # (l7b) THE DISCARD FLAG IS BUILT FROM THE POST-FOLD MEASUREMENT, RIGHT AFTER IT IS TAKEN.
        # `lane-worktree remove` refuses a worktree whose own `git status` lists uncommitted work
        # -- every lane's does -- unless told --discard-work. So the order must be:
        # the post-fold `classify` finds nothing left to fold, and the removal is built from THAT
        # result object, with the flag in it.
        kinds = [e[0] for e in events]
        built = [e[1] for e in events if e[0] == "remove-argv-built"]
        at = kinds.index("remove-argv") if "remove-argv" in kinds else -1
        post = events[at - 1][1] if at >= 1 and events[at - 1][0] == "classify" else None
        pin(kinds.count("remove-argv") == 1 and post is not None and events[at][1] is post
            and not (post.mine or post.deleted or post.refusals) and len(built) == 1
            and "--discard-work" in built[0],
            "(l7b) land builds its removal, WITH the discard-work flag, from the post-fold "
            "measurement that found nothing left to fold, immediately after taking it",
            "events=%s" % kinds)

        # (l2) A ROW THAT CANNOT BE APPLIED STOPS THE LANDING BEFORE THE FOLD WRITES.
        wl2 = make_lane(r3, "land2")
        write_atomic(os.path.join(wl2, "work-land2.txt"), "lane land2 work\n")
        cells(wl2, "land2", FX + "-LANDTWO", "draft", "🟠 **OPEN** a bad status")
        before_l2 = reg_now()
        rv, out = quiet(cmd_land, r3, "land2", "production", True, (),
                        os.path.join(top, "evidence-land2"))
        pin(rv == 2 and not os.path.exists(os.path.join(r3, "work-land2.txt"))
            and reg_now() == before_l2 and os.path.isdir(wl2),
            "(l2) a row that cannot be applied stops the landing BEFORE the fold writes, "
            "and the worktree is KEPT", "rv=%r out=%s" % (rv, out[-400:]))

        # (l3) A ROW WRITE THAT FAILS AFTER THE FOLD, THEN A RE-RUN THAT FINISHES.
        wl3 = make_lane(r3, "land3")
        write_atomic(os.path.join(wl3, "work-land3.txt"), "lane land3 work\n")
        write_atomic(os.path.join(wl3, ".temp", "land3-scratch", "findings.log"), "f3\n")
        cells(wl3, "land3", FX + "-LANDTHREE", "✅ CLOSED", "✅ **CLOSED** land3",
              bucket="production", priority="P3")

        def fail_every_write(root_, plan_, apply_it_):
            if apply_it_:
                return 1, "simulated failure writing the row"
            return real_writer(root_, plan_, apply_it_)

        globals()["_run_writer"] = fail_every_write
        try:
            rv, out = quiet(cmd_land, r3, "land3", "production", True, (),
                            os.path.join(top, "evidence-land3"))
        finally:
            globals()["_run_writer"] = real_writer
        pin(rv == 2 and os.path.isfile(os.path.join(r3, "work-land3.txt"))
            and not A3.find(r3, FX + "-LANDTHREE") and os.path.isdir(wl3),
            "(l3) a row write that fails AFTER the fold keeps the worktree, with the fold "
            "applied and no row", "rv=%r out=%s" % (rv, out[-400:]))
        rv, out = quiet(cmd_land, r3, "land3", "production", True, (),
                        os.path.join(top, "evidence-land3"))
        pin(rv == 0 and len(A3.find(r3, FX + "-LANDTHREE")) == 1 and not os.path.exists(wl3),
            "(l3b) ...and RE-RUNNING the landing completes it: the fold re-measures its own "
            "earlier write as ALREADY LANDED instead of refusing it",
            "rv=%r out=%s" % (rv, out[-500:]))

        # (l4) REMOVAL IS GATED ON THE EVIDENCE: a destination that already holds a
        # same-named file with other bytes makes lane-worktree refuse, and the worktree stays.
        wl4 = make_lane(r3, "land4")
        write_atomic(os.path.join(wl4, "work-land4.txt"), "w4\n")
        write_atomic(os.path.join(wl4, ".temp", "land4-scratch", "findings.log"),
                     "lane land4 findings\n")
        cells(wl4, "land4", FX + "-LANDFOUR", "✅ CLOSED", "✅ **CLOSED** land4",
              bucket="production", priority="P3")
        clash = os.path.join(top, "evidence-clash")
        write_atomic(os.path.join(clash, ".temp", "land4-scratch", "findings.log"),
                     "somebody else's findings\n")
        rv, out = quiet(cmd_land, r3, "land4", "production", True, (), clash)
        pin(rv == 2 and os.path.isdir(wl4)
            and text(os.path.join(clash, ".temp", "land4-scratch", "findings.log"))
            == "somebody else's findings\n",
            "(l4) when the evidence cannot be preserved the worktree is KEPT, and the "
            "evidence already at the destination is untouched", "rv=%r out=%s"
            % (rv, out[-500:]))
        dest4 = os.path.join(top, "evidence-land4")
        rv, out = quiet(cmd_land, r3, "land4", "production", True, (), dest4)
        pin(rv == 0 and not os.path.exists(wl4)
            and text(os.path.join(dest4, ".temp", "land4-scratch", "findings.log"))
            == "lane land4 findings\n" and "ALREADY LANDED" in out,
            "(l4b) ...and a re-run to a fresh destination lands it without writing the "
            "already-landed row again", "rv=%r out=%s" % (rv, out[-500:]))

        # (l7a) A FOLD THAT DID NOT LAND IS NEVER FOLLOWED BY A REMOVAL. `_apply_fold` is replaced
        # by one that writes nothing, so the lane's work is still unfolded when `land` re-measures,
        # and no removal may even be BUILT.
        wl7 = make_lane(r3, "land7")
        write_atomic(os.path.join(wl7, "work-land7.txt"), "w7\n")
        write_atomic(os.path.join(wl7, ".temp", "land7-scratch", "findings.log"), "f7\n")
        cells(wl7, "land7", FX + "-LANDSEVEN", "✅ CLOSED", "✅ **CLOSED** land7",
              bucket="production", priority="P3")
        real_apply_fold = globals()["_apply_fold"]
        calls7 = []

        def spy_remove_argv7(root_, lane_, destination_, verified_):
            calls7.append(verified_)
            return real_remove_argv(root_, lane_, destination_, verified_)

        globals()["_apply_fold"] = lambda root_, wt_, cls_: None
        globals()["lane_worktree_remove_argv"] = spy_remove_argv7
        try:
            rv, out = quiet(cmd_land, r3, "land7", "production", True, (),
                            os.path.join(top, "evidence-land7"))
        finally:
            globals()["_apply_fold"] = real_apply_fold
            globals()["lane_worktree_remove_argv"] = real_remove_argv
        pin(rv == 2 and "FOLD VERIFY FAILED" in out and not calls7 and os.path.isdir(wl7)
            and os.path.isfile(os.path.join(wl7, "work-land7.txt")),
            "(l7a) when the post-fold measurement still finds the lane's work unfolded, land stops "
            "and NO removal is built -- the worktree and its work are kept",
            "rv=%r removal-calls=%d out=%s" % (rv, len(calls7), out[-400:]))
        rv, out = quiet(cmd_land, r3, "land7", "production", True, (),
                        os.path.join(top, "evidence-land7"))
        pin(rv == 0 and not os.path.exists(wl7)
            and os.path.isfile(os.path.join(r3, "work-land7.txt")),
            "(l7a2) ...and once the fold really lands, the same landing completes",
            "rv=%r out=%s" % (rv, out[-400:]))

        # (l7c) THE REMOVAL BUILDER ITSELF REFUSES A MEASUREMENT THAT IS NOT "NOTHING LEFT".
        dirty = Classification(["work.txt"], [], [], [], [], [])
        rv_dirty, out_dirty = quiet(lane_worktree_remove_argv, r3, "x",
                                    os.path.join(top, "e7c"), dirty)
        rv_none, out_none = quiet(lane_worktree_remove_argv, r3, "x",
                                  os.path.join(top, "e7c"), None)
        pin(rv_dirty == 2 and "DISCARDS" in out_dirty and rv_none == 2 and "DISCARDS" in out_none,
            "(l7c) the removal builder REFUSES a measurement with work left, and no measurement "
            "at all", "dirty=%r none=%r" % (rv_dirty, rv_none))
        clean = Classification([], [], [], [], [], ["converged.txt"])
        rv_clean, _out_clean = quiet(lane_worktree_remove_argv, r3, "x",
                                     os.path.join(top, "e7c"), clean)
        pin(rv_clean == [sys.executable, lane_worktree_path(), "--repo", r3, "remove", "x",
                         "--discard-work", "--preserve-to", os.path.join(top, "e7c")],
            "(l7c2) CONTROL: handed nothing left to fold, it builds the removal with the discard "
            "flag -- the one argv, on every host, through this interpreter", "got=%r" % (rv_clean,))

        # (l8) A COMMIT INSIDE A LANE NEVER REACHES A REMOVAL THROUGH `land`. `land` builds
        # --discard-work, and `lane-worktree remove` lets that flag discard commits no ref reaches,
        # so `land` must refuse such a lane before it writes anything -- with the base RECORDED
        # (format 2), and with it NOT recorded (format 1, what every live lane carried).
        # ✔REPRODUCED before the fix: the format-1 half exited 0 "LANDED" and orphaned the commit.
        wl8 = make_lane(r3, "land8")
        write_atomic(os.path.join(wl8, "work-land8.txt"), "w8 uncommitted\n")
        write_atomic(os.path.join(wl8, "committed-land8.txt"), "w8 committed\n")
        git(wl8, "add", "committed-land8.txt")
        git(wl8, "commit", "-q", "-m", "land8 commits inside the lane")
        head8 = lane_head(wl8)
        write_atomic(os.path.join(wl8, ".temp", "land8-scratch", "findings.log"), "f8\n")
        cells(wl8, "land8", FX + "-LANDEIGHT", "✅ CLOSED", "✅ **CLOSED** land8",
              bucket="production", priority="P3")
        before_l8 = reg_now()
        calls8 = []

        def spy_remove_argv8(root_, lane_, destination_, verified_):
            calls8.append(verified_)
            return real_remove_argv(root_, lane_, destination_, verified_)

        def land8():
            calls8[:] = []
            globals()["lane_worktree_remove_argv"] = spy_remove_argv8
            try:
                return quiet(cmd_land, r3, "land8", "production", True, (),
                             os.path.join(top, "evidence-land8"))
            finally:
                globals()["lane_worktree_remove_argv"] = real_remove_argv

        def kept8():
            return (not calls8 and os.path.isdir(wl8) and lane_head(wl8) == head8
                    and not os.path.exists(os.path.join(r3, "work-land8.txt"))
                    and not os.path.exists(os.path.join(r3, "committed-land8.txt"))
                    and reg_now() == before_l8)

        rv, out = land8()
        pin(rv == 2 and "HEAD" in out and kept8(),
            "(l8) land REFUSES a lane whose HEAD holds a commit past its RECORDED base (format 2) "
            "before it writes anything or builds a removal -- the worktree and the commit are kept",
            "rv=%r removal-calls=%d out=%s" % (rv, len(calls8), out[-400:]))
        write_atomic(manifest_path(r3, "land8"), "{}")
        rv, out = land8()
        pin(rv == 2 and "land8 commits inside the lane" in out and kept8(),
            "(l8b) ...and with a FORMAT-1 manifest, which records no base, land REFUSES it too, "
            "naming the commit no ref reaches, before any write or removal",
            "rv=%r removal-calls=%d out=%s" % (rv, len(calls8), out[-400:]))

        # (e1) EVERY GIT QUESTION A FOLD ASKS IS ASKED WITHOUT THE CALLER'S GIT ENVIRONMENT.
        # ✔MEASURED 2026-09-15 (P66 round 3), before the move: under a caller's GIT_DIR +
        # GIT_WORK_TREE naming another repository, `list` printed THAT repository's lanes and
        # `fold` looked for its lane there; under GIT_DIR alone the root was this script's own
        # directory. A dry run of `fold` walks the whole reading path -- `rev-parse`, `status`,
        # `ls-files`, `cat-file`, `hash-object` -- so steered it must print exactly what it prints
        # unsteered. The negative is proven first: the same `status`, asked the way the defect
        # asked it, does not name this lane's file.
        ot = _owning_tree()
        r6 = os.path.join(top, "steer")
        make_repo(r6, [(".gitignore", IGNORE), ("S.txt", "s base\n")])
        w6 = make_lane(r6, "s")
        write_atomic(os.path.join(w6, "S.txt"), "s edited by the lane\n")
        write_atomic(os.path.join(w6, "NEW-IN-LANE.txt"), "new in the lane\n")
        plain_rv, plain_out = quiet(cmd_fold, r6, "s", False)
        with ot.steering() as steer:
            neg = ot.bare_git(["status", "--porcelain", "-z"], w6, steer)
            real6 = neg.returncode != 0 or "NEW-IN-LANE.txt" not in neg.stdout
            with ot.caller_environment(steer):
                try:
                    steered_rv, steered_out = quiet(cmd_fold, r6, "s", False)
                except Exception as exc:   # a steered git call that raises IS the defect
                    steered_rv, steered_out = "raised", "%s: %s" % (type(exc).__name__, exc)
        pin(real6 and plain_rv == 0 and "NEW-IN-LANE.txt" in plain_out
            and steered_rv == plain_rv and steered_out == plain_out,
            "(e1) a fold dry run under a caller's GIT_DIR + GIT_WORK_TREE + GIT_INDEX_FILE naming "
            "another repository reads THIS lane, exactly as it does unsteered",
            "negative-synthesized=%s rv=%r steered-rv=%r steered-out=%s"
            % (real6, plain_rv, steered_rv, str(steered_out)[-300:]))

        # (e4) ...AND NO PROCESS `land` STARTS INHERITS IT EITHER. The removal runs
        # `lane-worktree` as a child; handed a steering environment, that child's own
        # `git worktree remove` and `prune` would act on another repository and leave THIS one
        # registering a worktree that is gone. The negative is proven first: `worktree list`,
        # asked the defect's way, does not see this repository's lane at all.
        wl9 = make_lane(r3, "land9")
        write_atomic(os.path.join(wl9, "work-land9.txt"), "w9\n")
        cells(wl9, "land9", FX + "-LANDNINE", "✅ CLOSED", "✅ **CLOSED** land9",
              bucket="production", priority="P3")
        with ot.steering() as steer:
            neg = ot.bare_git(["worktree", "list", "--porcelain"], r3, steer)
            real9 = neg.returncode != 0 or "land9" not in neg.stdout
            with ot.caller_environment(steer):
                try:
                    rv, out = quiet(cmd_land, r3, "land9", "production", True, (),
                                    os.path.join(top, "evidence-land9"))
                except Exception as exc:   # a steered git call that raises IS the defect
                    rv, out = "raised", "%s: %s" % (type(exc).__name__, exc)
        listed9 = git_run(["-C", r3, "worktree", "list", "--porcelain"], capture_output=True,
                          text=True, encoding="utf-8", errors="replace").stdout
        pin(real9 and rv == 0 and not os.path.exists(wl9) and "land9" not in listed9,
            "(e4) land under a caller's steering git environment removes the lane from THIS "
            "repository's worktree registrations -- the lane-worktree child does not inherit it",
            "negative-synthesized=%s rv=%r registered-after=%s out=%s"
            % (real9, rv, "land9" in listed9, str(out)[-400:]))

        # (l5) a lane with no row/ directory is not landed.
        wl5 = make_lane(r3, "land5")
        rv, out = quiet(cmd_land, r3, "land5", "production", True, (),
                        os.path.join(top, "evidence-land5"))
        pin(rv == 2 and os.path.isdir(wl5) and "no row directory" in out,
            "(l5) a lane with no row/ directory is NOT landed", "rv=%r out=%s"
            % (rv, out[-300:]))

        # (l6) CONTROL: a dry run of a valid landing writes and removes nothing.
        wl6 = make_lane(r3, "land6")
        write_atomic(os.path.join(wl6, "work-land6.txt"), "w6\n")
        cells(wl6, "land6", FX + "-LANDSIX", "🟠 OPEN", "🟠 **OPEN** land6")
        before_l6 = reg_now()
        rv, out = quiet(cmd_land, r3, "land6", "production", False, (),
                        os.path.join(top, "evidence-land6"))
        pin(rv == 0 and not os.path.exists(os.path.join(r3, "work-land6.txt"))
            and reg_now() == before_l6 and os.path.isdir(wl6),
            "(l6) CONTROL: a dry run of a valid landing passes and writes, applies and "
            "removes nothing", "rv=%r out=%s" % (rv, out[-300:]))

    print("lane-fold self-test: %d pin(s), %d failed" % (pins[0], failed[0]))
    return 1 if failed[0] else 0


# ──────────────────────────────────── main ─────────────────────────────────────

def _dispatch_landing(root, verb, lane, rest):
    """`apply-rows <lane> <bucket> [--apply]` and `land <lane> <bucket> [--apply]
    [--settled <path>]... [--preserve-to <dir>]`. ⚠ STRICT: an option either verb does not
    know is REFUSED -- a landing that silently ignored a mistyped flag would do something
    other than what its caller said."""
    if len(rest) < 2 or rest[1].startswith("-"):
        die("verb %s needs a lane and a default bucket: %s <lane> <production> "
            "[--apply]" % (verb, verb), 3)
    bucket = rest[1]
    if bucket not in ("production", "harness"):
        die("the default bucket must be production or harness, not %r -- it is where a NEW "
            "row goes when its row/ declares no .bucket." % bucket, 3)
    apply_it, settled, preserve_to = False, [], None
    j = 2
    while j < len(rest):
        arg = rest[j]
        if arg == "--apply":
            apply_it, j = True, j + 1
            continue
        if verb == "land" and arg in ("--settled", "--preserve-to"):
            if j + 1 >= len(rest) or not rest[j + 1]:
                die("%s needs a value." % arg, 3)
            if arg == "--settled":
                settled.append(rest[j + 1].replace(os.sep, "/"))
            else:
                preserve_to = os.path.abspath(rest[j + 1])
            j += 2
            continue
        if verb == "land" and arg.startswith("--settled="):
            value = arg[len("--settled="):]
            if not value:
                die("--settled needs a repo-relative path.", 3)
            settled.append(value.replace(os.sep, "/"))
            j += 1
            continue
        if verb == "land" and arg.startswith("--preserve-to="):
            value = arg[len("--preserve-to="):]
            if not value:
                die("--preserve-to needs a directory.", 3)
            preserve_to = os.path.abspath(value)
            j += 1
            continue
        die("unknown option %r for %s -- a landing refuses what it does not understand "
            "rather than guessing." % (arg, verb), 3)
    if verb == "apply-rows":
        return cmd_apply_rows(root, lane, bucket, apply_it)
    return cmd_land(root, lane, bucket, apply_it, tuple(settled), preserve_to)


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if not argv:
        print(__doc__)
        return 3
    # ── `--repo <path>` ─────────────────────────────────────────────────────────
    # ⚠ EXTRACTED FROM THE WHOLE ARGUMENT LIST, BEFORE THE VERB IS READ, so it works
    # on either side of the verb. A first draft scanned only `argv[1:]`, and
    # `lane-fold.py --repo <path> fold lw` then died with "unknown verb '--repo'"
    # while `lane-worktree` accepted the same spelling -- two halves of one tool
    # disagreeing about their own grammar, which is a thing a caller discovers at the
    # moment they most want the escape hatch.
    # ⓘ THE FLAG IS THE CAPABILITY THE OLD CWD-KEYED BEHAVIOUR PROVIDED BY ACCIDENT:
    # driving the verb at another tree used to be done by cd'ing there and hoping.
    # Saying it out loud is the difference between a decision and a side effect.
    override = None
    kept = []
    i = 0
    while i < len(argv):
        if argv[i] == "--repo":
            if i + 1 >= len(argv):
                die("--repo needs a directory.", 3)
            override, i = argv[i + 1], i + 2
            continue
        if argv[i].startswith("--repo="):
            override, i = argv[i][len("--repo="):], i + 1
            if not override:
                die("--repo needs a directory.", 3)
            continue
        kept.append(argv[i])
        i += 1
    if not kept:
        print(__doc__)
        return 3
    verb, rest = kept[0], kept[1:]
    if override is not None and not os.path.isdir(override):
        die("--repo %r: no such directory." % override, 3)
    root = repo_root(override)
    if verb == "list":
        return cmd_list(root)
    if verb not in ("seed", "fold", "refresh-plans", "apply-rows", "land"):
        die("unknown verb %r -- expected seed, fold, refresh-plans, apply-rows, land or list."
            % verb, 3)
    if not rest or rest[0].startswith("-"):
        die("verb %s needs a lane name." % verb, 3)
    lane = rest[0]
    if os.sep in lane or "/" in lane or lane.startswith("."):
        die("lane name %r is not a bare directory name." % lane, 3)
    if verb in ("apply-rows", "land"):
        return _dispatch_landing(root, verb, lane, rest)
    # ⚠ AN OPTION A VERB DOES NOT KNOW IS REFUSED, NEVER IGNORED. ✔MEASURED by reading this
    # function in P66: `seed <lane> --emtpy` performed a FULL seed where `--empty` was meant,
    # and `fold <lane> --aply` was a silent dry run -- a mistyped flag doing something other
    # than what its caller said, with nothing on the screen to show it. The two landing verbs
    # refuse the same way (`_dispatch_landing`); self-test arms (s1)-(s3) pin it.
    known = {"seed": ("--empty", "--force"), "refresh-plans": ("--apply",),
             "fold": ("--apply", "--settled")}[verb]
    j = 1
    while j < len(rest):
        arg = rest[j]
        if verb == "fold" and arg == "--settled":
            j += 2                    # its value is read, and refused when absent, below
            continue
        if verb == "fold" and arg.startswith("--settled="):
            j += 1
            continue
        if arg not in known:
            die("unknown option %r for %s -- expected %s. A verb refuses what it does not "
                "understand rather than guessing." % (arg, verb, ", ".join(known)), 3)
        j += 1
    if verb == "seed":
        return cmd_seed(root, lane, empty="--empty" in rest,
                        force="--force" in rest)
    if verb == "refresh-plans":
        return cmd_refresh_plans(root, lane, apply_it="--apply" in rest)
    # `--settled <path>` is repeatable; see `classify` for why it exists and for the
    # measurement that the refusal message previously promised something impossible.
    settled, j = [], 0
    while j < len(rest):
        if rest[j] == "--settled":
            if j + 1 >= len(rest):
                die("--settled needs a repo-relative path.", 3)
            settled.append(rest[j + 1].replace(os.sep, "/"))
            j += 2
            continue
        if rest[j].startswith("--settled="):
            value = rest[j][len("--settled="):]
            if not value:
                die("--settled needs a repo-relative path.", 3)
            settled.append(value.replace(os.sep, "/"))
        j += 1
    return cmd_fold(root, lane, apply_it="--apply" in rest, settled=tuple(settled))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
