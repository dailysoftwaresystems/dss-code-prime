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

`land` adds ONE key the moment before it asks for the worktree's removal, and drops it once
the removal is complete: `"landed": {"at": "<UTC>", "evidence": "<where the evidence was kept>",
"worktree": "<the identity of the lane's git worktree>", "lane": {"<path>": "<md5>" | null}}`.
While it is there the lane is CLOSED: nothing folds it, applies its rows or seeds over it, and a
re-run of `land` finishes the removal or names the command that does (see LANDING). `seed`
resets it for a NEW worktree of the name (another identity) and, for the landed one, only while
that lists no change.

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
describes -- which matters because `land` deletes the lane.

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
    fold". The lane's WHOLE evidence set is copied first, by `lane-worktree`'s own
    copy-and-re-read, over the evidence roots the configuration named BEFORE the fold and
    the ones it names AFTER it (a lane may change `worktrees.evidenceRoots`, and the fold
    then carries that change into the tree DssHarness reads); then this verb re-reads every
    file at the destination against the digests it took before the fold -- and only when
    all of that holds does DssHarness remove the worktree and the copies its record holds on
    hosts: `dssharness delete-worktree <lane> --discard-uncommitted --delete-evidence`.
    ⚠ ✔MEASURED 2026-09-29 (P68's PR exit, the review of lane `lf`): the md5 check used to
    run AFTER that removal, so evidence the copy missed was found missing only once
    `--delete-evidence` had deleted it -- a check after an irreversible step is a
    post-mortem. The discard flag is built from the measurement and nothing else --
    DssHarness refuses a worktree whose own `git status` lists uncommitted work without it.
    ★ ONLY `land` REMOVES, the step after the lane's LAST review: `fold` never does, so a
    lane a review sends back keeps its worktree and its host copies. The default
    destination is `.worktrees/.evidence/<lane>-<UTC stamp>/` -- ignored, repo-relative,
    and outliving the lane, like `.manifests/`.
  * ★★ THE MANIFEST IS MARKED LANDED BEFORE THE REMOVAL IS ASKED FOR, and a marked lane
    is never folded again. A removal can stop part way -- ✔MEASURED 2026-09-29 with
    DssHarness 0.6.1: a worktree holding a directory junction came back exit 20, "git
    reported worktree 'jx' removed, but '<repo>/.worktrees/jx' still exists", its `.git` file
    and git's record gone and the junction left -- and a re-run that folded what was left
    would read DEBRIS as the lane's intent: missing files as its deletions, and, in a
    directory that is no longer a worktree, the MAIN repository's own status, git having
    walked up to it. So a re-run of a marked lane compares the lane with the mark's record of
    it (a file deleted since is the removal's debris; one changed or new since is work, and
    stops it), keeps the evidence again into a fresh directory beside the first, and asks for
    the removal again -- never with `--force`: a directory with no `.git` of its own is named,
    with the command, for a person to look at, since a directory made at that path since reads
    the same. And EVERY verb refuses a lane directory that is not its own registered git
    worktree, marked or not: `_require_worktree`.
  * REMOVE, THEN VERIFY, THEN SPEAK: gone from disk, gone from git's worktree list, and no
    copy of it left in DssHarness's record (`dssharness list-worktree --json`, which asks no
    host). A copy DssHarness did not make, or one on a host no configuration declares, it
    leaves in place and forgets, saying so in the lines this verb prints.

Every write is write-temp + `os.replace`. This script NEVER runs a git write verb,
never stages, and never touches `.git/`. (`land` asks DssHarness, the one owner of
worktree removal, to remove the worktree and its host copies; it does not remove anything
itself.)

Exit codes: 0 OK · 2 refused, nothing removed (nothing written; or -- for `land` -- stopped
before the removal with the worktree kept, and the message says what already landed; a
re-run finishes it) · 3 usage error · 4 `land` only: LANDING INCOMPLETE -- the fold and the
rows are in, the evidence is kept and the removal began, but something is left, which the
message names with the command that settles it; re-running `land` is safe, because the lane
is marked landed and is never folded again. (`land` on a marked lane whose directory is a NEW
worktree of the name answers 2: it is not the landed lane, and `seed` starts it clean.)

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


# `landed`: None, or `Landed` -- set by `land` the moment before it asks for the removal.
Manifest = collections.namedtuple("Manifest", "base paths landed", defaults=(None,))
# `worktree`: the identity of the lane's git worktree when it was marked (`_worktree_identity`), which
#   a new worktree of the same name does not share; `lane`: {path: md5, or None where the lane held no
#   file} for every path the landing measured -- what a re-run compares the lane with, never the main tree.
Landed = collections.namedtuple("Landed", "at evidence worktree lane")


def _is_path_map(obj):
    return isinstance(obj, dict) and all(
        isinstance(k, str) and isinstance(v, str) and _MD5.match(v) for k, v in obj.items())


def load_manifest(path):
    """-> Manifest(base, paths, landed). `base` is None for a FORMAT-1 manifest; `landed` is a
    `Landed` once `land` began removing the lane, else None.

    ⚠ ANYTHING ELSE IS REFUSED, NEVER GUESSED AT. A manifest is the one record of what a
    lane was HANDED; reading a malformed one "as best we can" is how a fold ends up
    subtracting the wrong set -- which drops a lane's work silently. A malformed `landed`
    is refused the same way: read as absent, it would let a lane whose removal began be
    folded again.
    """
    try:
        with io.open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as exc:
        die("seed manifest %s is unreadable: %s" % (path, exc))
    if isinstance(data, dict) and "format" in data:
        base, paths, landed = data.get("base"), data.get("paths"), data.get("landed")
        if (data.get("format") != MANIFEST_FORMAT
                or set(data) - {"landed"} != {"format", "base", "paths"}
                or not isinstance(base, str) or not _SHA.match(base)
                or not _is_path_map(paths)
                or ("landed" in data and not (
                    isinstance(landed, dict) and set(landed) == {"at", "evidence", "worktree", "lane"}
                    and all(isinstance(landed[k], str) and landed[k] for k in ("at", "evidence", "worktree"))
                    and isinstance(landed["lane"], dict)
                    and all(isinstance(p, str) and p and (d is None or (isinstance(d, str) and _MD5.match(d)))
                            for p, d in landed["lane"].items())))):
            die("seed manifest %s declares a format but is not a well-formed format-%d "
                "manifest {\"base\": <commit>, \"format\": %d, \"paths\": {path: md5}} "
                "(with, while `land` is removing the lane, \"landed\": {\"at\": <UTC>, \"evidence\": "
                "<directory>, \"worktree\": <identity>, \"lane\": {path: md5 or null}}); refusing to "
                "guess what this lane was handed." % (path, MANIFEST_FORMAT, MANIFEST_FORMAT))
        return Manifest(base, dict(paths),
                        Landed(landed["at"], landed["evidence"], landed["worktree"], dict(landed["lane"]))
                        if "landed" in data else None)
    if _is_path_map(data):
        return Manifest(None, dict(data))
    die("seed manifest %s is neither format %d nor the older flat {path: md5} map; "
        "refusing to guess what this lane was handed." % (path, MANIFEST_FORMAT))


def save_manifest(path, manifest):
    # A format-1 manifest stays format 1: writing a base it never recorded would turn
    # "unknown" into an assertion nobody measured. (A landing writes format 2 with the base
    # `lane_base` MEASURED that moment -- see `cmd_land` -- so a mark never needs format 1.)
    if manifest.base is None:
        if manifest.landed is not None:
            die("internal: a landing mark needs a format-2 manifest, with a measured base.", 70)
        body = manifest.paths
    else:
        body = {"format": MANIFEST_FORMAT, "base": manifest.base, "paths": manifest.paths}
        if manifest.landed is not None:
            body["landed"] = {"at": manifest.landed.at, "evidence": manifest.landed.evidence,
                              "worktree": manifest.landed.worktree, "lane": dict(manifest.landed.lane)}
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

def _landed_mark_of(mpath):
    """-> the `Landed` mark the manifest at `mpath` carries, or None -- read TOLERANTLY: `seed`
    exists to reset a manifest, so one it cannot parse is not a reason to refuse the reset."""
    try:
        with io.open(mpath, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None
    landed = data.get("landed") if isinstance(data, dict) else None
    if isinstance(landed, dict):
        lane = landed.get("lane")
        return Landed(str(landed.get("at")), str(landed.get("evidence")), str(landed.get("worktree")),
                      dict(lane) if isinstance(lane, dict) else {})
    return None


def _worktree_identity(wt):
    """-> "<file id>:<mtime in ns>" of the `commondir` file in lane `wt`'s own git administrative
    directory (`git rev-parse --absolute-git-dir`), or None when git cannot name one. Read, never
    written.

    ✔MEASURED 2026-09-29 (git for Windows, a scratch repository): that file keeps both across `git
    worktree repair`, `git worktree move` and a rewrite of the lane's `.git` file, and a NEW worktree
    at the same path gets another -- while the `.git` file's own time, the identity this used first,
    moved on the rewrite and on the move (the re-review of lane `lf`: a lane repaired after its
    removal stopped then read as a NEW lane, and `seed` would have wiped its mark). A time and a file
    id together, because a directory's creation time is not readable on Linux and an inode number
    alone can be reused there."""
    proc = git_run(["-C", wt, "rev-parse", "--absolute-git-dir"], capture_output=True, text=True,
                   encoding="utf-8", errors="replace")
    if proc.returncode != 0 or not proc.stdout.strip():
        return None
    try:
        st = os.stat(os.path.join(proc.stdout.strip(), "commondir"))
    except OSError:
        return None
    return "%d:%d" % (st.st_ino, st.st_mtime_ns)


def cmd_seed(root, lane, empty=False, force=False):
    wt = worktree_path(root, lane)
    if not os.path.isdir(wt):
        die("no worktree at %s\n"
            "  create it first: dssharness create-worktree %s" % (wt, lane))
    if not inside(root, wt):
        die("worktree escapes the repository: %s" % wt)
    _require_worktree(root, lane, wt)

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
    # ★ A MANIFEST MARKED LANDED IS RESET FOR A NEW WORKTREE OF THE NAME, AND FOR THE LANDED ONE ONLY
    # WHILE IT HAS NOTHING TO FOLD. A new worktree has an identity of its own (`_worktree_identity`),
    # so the reset P57 requires goes through for it whatever it holds; the landed lane itself, left
    # by a removal that stopped part way, still lists its work or its debris, and wiping its mark
    # would let the next fold carry that debris into the main tree. `--empty` and `--force` are NO
    # exemption: the danger is the mark, not the copy (✔MEASURED, the re-review of lane `lf`:
    # `seed --force` cleared the mark silently and the next fold REMOVED `tracked.txt` from the
    # main tree, git having deleted it from the lane before the removal stopped). A worktree whose
    # identity git cannot name cannot be told from the landed one, so it is refused like it.
    marked = _landed_mark_of(manifest_path(root, lane))
    if marked is not None and own:
        ident = _worktree_identity(wt)
        if ident is None or ident == marked.worktree:
            _refuse_landed(lane, marked)
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
    if loaded.landed is not None:
        _refuse_landed(lane, loaded.landed, wt)
    _require_worktree(root, lane, wt)
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
    line-ending conversion that made raw bytes a false DRIFT.

    ⚠⚠ EVERY SEEDED PATH IS A CANDIDATE, WHETHER OR NOT THE LANE'S `git status` LISTS IT -- the
    same family as the deletion above, and it failed the same way. ✔MEASURED 2026-09-28 (P68's
    PR exit, lane `ci59b`): seeded with 9 paths, the lane put `.harness-config/config.json` back
    to HEAD's bytes, so its `git status` no longer listed it; the fold reported "2 inherited
    path(s) skipped, 6 path(s) are this lane's" -- 8 of the 9 -- and wrote 6, keeping the main
    tree's seeded bytes over the lane's revert, with no warning. A seeded path whose lane bytes
    differ from its seed is the lane's change, HEAD's bytes included, under the same drift proof
    as any copy. ★ So every seeded path now lands in exactly one list -- this lane's, deleted,
    inherited, converged (absent on both sides included), settled, or refused -- and the fold's
    plan counts the seeded ones by list, so the reader sees all of them go somewhere."""
    if base is None:
        base = lane_head(wt)
    lanes = layout(root).worktrees
    candidates = sorted(q for q in set(changed_paths(wt)) | set(seed) if not is_lane_tree(q, lanes))
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
                converged.append(rel)         # absent on both sides: the main tree holds the lane's state
                continue
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


def _registered(root, wt):
    """-> (does git still record `wt` as a worktree of `root`?, "") -- or (None, why) when git
    cannot say. ⚠ A failing `git worktree list` is not "unregistered": read so, a removal that
    left a registration would verify as complete (the review of lane `lf`, 2026-09-29)."""
    proc = git_run(["-C", root, "worktree", "list", "--porcelain"], capture_output=True, text=True,
                   encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        lines = (proc.stderr or "").strip().splitlines()
        return None, "`git worktree list` exited %d%s" % (proc.returncode,
                                                          ": " + lines[0] if lines else "")
    return any(line.startswith("worktree ") and _owning_tree().same_path(line[len("worktree "):], wt)
               for line in proc.stdout.replace("\r", "").split("\n")), ""


def _worktree_state(root, wt):
    """-> ("worktree", "") when git names `wt` its own top level AND `root` lists it among its
    worktrees; ("husk", what git answers instead) when `wt` holds no `.git` of its own and git
    answers for it with ANOTHER tree, or not at all; ("foreign", why) for a repository of its own
    that `root` does not list; (None, why) when `root`'s own git cannot say, or when `wt`'s `.git`
    is there and git still cannot read the worktree from it.

    ⚠⚠ WHY A HUSK IS NEVER READ. A directory under the lanes' root whose `.git` file is gone is
    no git repository, so git walks UP from it -- to the main checkout that contains the lanes'
    root -- and every question a fold asks is answered by the MAIN TREE: its HEAD read as the
    lane's base, its uncommitted changes read as the lane's. ✔MEASURED 2026-09-29 (the review of
    lane `lf`, git 2.55.0.windows.5): folding such a directory removed an uncommitted edit and an
    untracked file from the main tree; and DssHarness 0.6.1 leaves exactly that directory when a
    worktree holds a directory junction ("git reported worktree 'jx' removed, but … still
    exists")."""
    listed, why = _registered(root, wt)
    if listed is None:
        return None, why
    top, why_top = _owning_tree().git_top_level(wt)
    if top is not None and _owning_tree().same_path(top, wt):
        if listed:
            return "worktree", ""
        return "foreign", ("it is a git repository of its own, and %s does not list it among its "
                           "worktrees" % root)
    answer = ("git answers %s for it" % top) if top is not None else (
        "git cannot answer from inside it (%s)" % why_top)
    # ★ A HUSK HAS NO `.git` OF ITS OWN. One whose `.git` is there but unreadable -- a transient read
    # failure, a gitdir spelled for another host -- is a worktree git cannot SEE right now, not a
    # husk, and nothing may be forced over it (✔MEASURED, the re-review of lane `lf`: such a lane,
    # still registered, was read as a husk and its later work would have gone with `--force`).
    if os.path.lexists(os.path.join(wt, ".git")):
        return None, "its `.git` is there, but %s%s" % (answer, "; git still lists it" if listed else "")
    return "husk", answer


def _require_worktree(root, lane, wt, husk_ok=False):
    """Refuse (exit 2) unless `wt` is a registered git worktree of `root` that is its own top level;
    -> True for a husk, which only `land`, finishing a lane it marked, passes `husk_ok` to accept."""
    state, why = _worktree_state(root, wt)
    if state == "worktree":
        return False
    if state == "husk" and husk_ok:
        return True
    if state is None:
        die("cannot tell whether lane %s's %s is a worktree of this repository: %s. Nothing was "
            "read, written or removed." % (lane, wt, why))
    if state == "foreign":
        die("lane %s: %s is not a worktree of this repository -- %s. Nothing was read, written or "
            "removed; it is not this tool's to fold or delete." % (lane, wt, why))
    die("lane %s: %s is not its own git worktree -- %s -- so every question a fold, a row or a "
        "removal asks about it would be answered by that tree instead. Nothing was read, written or "
        "removed. A worktree whose removal stopped part way (no `.git` file, no record) is finished "
        "by `dssharness delete-worktree %s --force` once what it holds is kept; one that was only "
        "moved is repaired by `git worktree repair`." % (lane, wt, why, lane))


def _open_lane(root, lane, closed_ok=False):
    """-> (worktree, manifest, state): state is "worktree", "husk" or "gone". Refuses a missing
    manifest, a lane marked landed (unless `closed_ok`, which only `land` passes, to FINISH one), a
    missing worktree (unless it is such a lane), and a directory that is not its own registered git
    worktree (unless it is such a lane: `_require_worktree`)."""
    wt = worktree_path(root, lane)
    mpath = manifest_path(root, lane)
    if not os.path.isfile(mpath):
        die("no seed manifest at %s\n"
            "  run `lane-fold.py seed %s` at the moment the worktree is created -- the "
            "manifest is what separates this lane's work from what it inherited."
            % (mpath, lane))
    manifest = load_manifest(mpath)
    finishing = closed_ok and manifest.landed is not None
    if manifest.landed is not None and not closed_ok:
        _refuse_landed(lane, manifest.landed, wt)
    if not os.path.isdir(wt):
        if finishing:
            return wt, manifest, "gone"
        die("no worktree at %s" % wt)
    return wt, manifest, ("husk" if _require_worktree(root, lane, wt, husk_ok=finishing)
                          else "worktree")


def _refuse_landed(lane, landed, wt=None):
    """Refuse (exit 2) to fold, seed over or give rows to a lane marked landed -- naming, when `wt`
    is a NEW worktree of the name (another identity), the `seed` that was skipped instead."""
    if wt is not None and os.path.isdir(wt):
        ident = _worktree_identity(wt)
        if ident is not None and ident != landed.worktree:
            die("lane %s: %s is a NEW worktree of this name, not the one landed at %s, and its seed "
                "manifest still carries that landing's mark -- the mandatory `seed` was skipped. Nothing "
                "was read, written or removed; `lane-fold.py seed %s` starts it clean."
                % (lane, wt, landed.at, lane))
    die("lane %s was LANDED at %s: its fold and its rows are in the main tree, its evidence is kept "
        "at %s, and its removal began. A landed lane is never folded, seeded over or given rows "
        "again -- what is left in it may be the debris of a removal that stopped part way. Finish "
        "it with `lane-fold.py land %s production --apply`." % (lane, landed.at, landed.evidence,
                                                                 lane))


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
    if seed:
        # ★ EVERY SEEDED PATH, COUNTED BY WHERE IT WENT: a seeded path once vanished from both lists
        # below, and the plan printed nothing that could show it (P68's PR exit).
        where = [sum(1 for rel in group if rel in seed)
                 for group in (cls.mine, cls.inherited, cls.converged, cls.deleted, cls.settled)]
        print("lane-fold: %d seeded path(s): %d this lane's, %d inherited, %d already landed, %d deleted, "
              "%d settled" % tuple([len(seed)] + where))
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
    wt, manifest, _state = _open_lane(root, lane)
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
    # The mark read STRICTLY, as `fold` and `land` read it: a malformed one refused, never taken for none.
    mpath = manifest_path(root, lane)
    if os.path.isfile(mpath) and load_manifest(mpath).landed is not None:
        _refuse_landed(lane, load_manifest(mpath).landed, wt)
    _require_worktree(root, lane, wt)
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


def _root_segments(roots):
    """Evidence roots as tuples of path segments -- `lane-worktree`'s form -- from segments or from
    forward-slashed strings (`layout`'s form)."""
    return [tuple(r.split("/")) if isinstance(r, str) else tuple(r) for r in roots]


def _evidence_root_union(*root_lists):
    """Every evidence root any of `root_lists` names, once, in first-seen order."""
    seen, out = set(), []
    for roots in root_lists:
        for segs in _root_segments(roots):
            if segs not in seen:
                seen.add(segs)
                out.append(segs)
    return out


def evidence_digests(wt, roots):
    """-> {worktree-relative path: md5} for every evidence file under `roots` -- ENUMERATED BY
    `lane-worktree`'s own `evidence_files`, the one list its count, its copy and its verify read,
    so the digests this verb checks name exactly the files the preserve takes. ✔MEASURED
    2026-09-29 (the review of lane `lf`): this used to be an `os.walk` of its own, which on
    Python 3.14 descends into a Windows directory junction that `lane-worktree` rightly does not
    follow -- counting files the copy never took, and then reporting them lost. A dangling link
    has no bytes to digest and is left out; the preserve itself refuses it. Raises OSError."""
    return {rel: md5_file(path) for rel, path in _lane_worktree().evidence_files(wt, _root_segments(roots))
            if os.path.isfile(path)}


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


def worktree_removal_argv(root, lane, verified, door):
    """-> the argv that asks DssHarness, the ONE owner of worktree removal (`dssharness help
    worktrees`), to delete lane `lane`'s worktree AND the copies its record holds on hosts
    (`delete-worktree`), discarding its uncommitted work and its evidence roots -- `land` has folded
    the one and preserved and re-read the other before it builds this.

    ⚠⚠ WHY DSSHARNESS, AND NOT `lane-worktree remove` ANY MORE. That verb ran `git worktree remove`,
    which knows nothing of the copies DssHarness makes of a worktree on the hosts its legs run on.
    ✔MEASURED 2026-09-28 (P68's PR exit): a probe lane synced to WSL was landed -- "LANDED lane
    lfprobe" -- and `dssharness delete-worktree lfprobe` then still removed its copy at
    `~/src/dss-code-prime.worktree-lfprobe`. Every landed lane had left a copy behind on every host
    it ran on. DssHarness asks only the hosts it recorded a copy on ("Nothing recorded, so nothing
    asked"), so a lane that never ran on a host costs no host a question.

    ★★ `verified` IS WHAT THE DISCARD FLAG IS BUILT FROM. `delete-worktree` refuses (exit 13) a
    worktree whose own `git status` lists uncommitted work -- every lane's does, its own work and
    its seeded paths alike -- because it cannot tell folded work from unfolded work, and only this
    file's measurement can. So `--discard-uncommitted` is added HERE, and only when `verified` is
    the post-fold `classify` result with nothing left to fold: no lane path still to write, none
    still to delete, no refusal. Anything else refuses, so no caller can build a removal that
    discards work without that measurement in hand. Self-test arms (l7a)-(l7c) pin the order and
    the refusal. `--delete-evidence` stands on the same footing: `land` builds this only after
    the evidence was copied and re-read at its destination.

    ⚠ A lane's HEAD holding commits no ref reaches is still REFUSED by `delete-worktree`, which
    has no flag for it short of `--force` -- and `land` never gets that far: `lane_base` refuses
    such a lane first. Self-test arms (l8) and (l8b) pin that no removal is even built.

    ★ `verified` MAY ALSO BE THE LANE'S `Landed` MARK -- only when `land` FINISHES a removal that
    stopped part way, and only once the lane measured unchanged against the mark's record. The mark
    is written only after that same clean measurement, the rows re-read and the evidence verified,
    so it is the measurement, persisted. ⛔ Never `--force`, which checks nothing: a directory git
    no longer knows as a worktree is left for a person to look at (`_finish_landing`).
    `--no-prompt`: nothing here can answer a question."""
    if isinstance(verified, Landed):
        pass
    elif (not isinstance(verified, Classification)
            or verified.mine or verified.deleted or verified.refusals):
        die("refusing to build a removal that DISCARDS lane %s's uncommitted work: it was not "
            "handed a fold measurement showing nothing left to fold (%s). The worktree is kept."
            % (lane, "no measurement at all" if not isinstance(verified, Classification) else
               "%d path(s) still to write, %d to delete, %d refusal(s)"
               % (len(verified.mine), len(verified.deleted), len(verified.refusals))))
    return [door, "delete-worktree", lane, "--discard-uncommitted", "--delete-evidence", "--no-prompt",
            "-C", root]


def _run_removal(argv, root):
    """Runs `worktree_removal_argv`'s argv in the main checkout -> (exit code, what it printed).
    ★ The child does NOT inherit the caller's git environment (`run_unsteered`: every name git
    calls repository-local, which DssHarness's own git client clears only three of), and reads no
    stdin. Module level, so the self-test can stand in for a host that cannot be reached; arm (e5)
    pins the child's environment, (e4) the whole landing under a steering caller."""
    proc = _owning_tree().run_unsteered(argv, cwd=root, capture_output=True)
    return proc.returncode, (proc.stdout + proc.stderr).decode("utf-8", "replace")


def _list_worktrees(argv, root):
    """Runs `dssharness list-worktree --json` (`argv`, built by `copies_left`) in the main checkout
    -> (exit code, stdout, stderr): DssHarness's RECORD of the copies hosts keep, read with no host
    asked. Module level, so the self-test can stand in for a record that still holds a copy;
    unsteered and stdin-less like `_run_removal` (arm (e5))."""
    proc = _owning_tree().run_unsteered(argv, cwd=root, capture_output=True)
    return (proc.returncode, proc.stdout.decode("utf-8", "replace"),
            proc.stderr.decode("utf-8", "replace"))


def copies_left(door, root, lane):
    """-> ([(host, path)], "") for every copy of lane `lane` DssHarness's record still holds -- or
    (None, why) when the record cannot be read, which is never taken for "none left".

    ★ WHY `land` READS THE RECORD RATHER THAN TRUSTING THE EXIT CODE. `delete-worktree` exits 0
    only once every copy it recorded was dealt with, and otherwise with the highest code a copy
    was left with -- but "REMOVE, THEN VERIFY, THEN SPEAK" holds for what this verb claims, and
    the record is where a copy left behind stays named (`gone`, with the command that removes it).
    DssHarness 0.6.1 made it readable: `list-worktree --json`, `worktrees` and `gone` each listing
    `copies` of `{host, path}`. ⓘ A copy DssHarness did not make, or one on a host no
    configuration declares, it leaves in place and forgets, saying so in its own lines."""
    rc, out, err = _list_worktrees([door, "list-worktree", "--json", "--no-prompt", "-C", root], root)
    if rc != 0:
        lines = (err or out).strip().splitlines()
        return None, "`dssharness list-worktree --json` exited %d%s" % (rc, ": " + lines[-1] if lines else "")
    try:
        doc = json.loads(out)
    except ValueError as exc:
        return None, "`dssharness list-worktree --json` printed no JSON document (%s)" % exc
    if not isinstance(doc, dict) or not all(isinstance(doc.get(k), list) for k in ("worktrees", "gone")):
        return None, "`dssharness list-worktree --json` holds no `worktrees` and `gone` lists"
    if doc.get("recordUnreadable"):
        return None, "DssHarness could not read its record of host copies: %s" % doc["recordUnreadable"]
    left = []
    for key in ("worktrees", "gone"):
        for entry in doc[key]:
            if isinstance(entry, dict) and entry.get("name") == lane:
                for copy in entry.get("copies") or []:
                    if isinstance(copy, dict):
                        left.append((str(copy.get("host")), str(copy.get("path"))))
                    else:
                        return None, "`dssharness list-worktree --json` lists a copy of %s that is " \
                                     "not a {host, path} object: %r" % (lane, copy)
    return left, ""


def _keep_evidence(root, wt, roots, destination, expected, verdict="NOT LANDED"):
    """Copy every evidence file under `roots` to `destination` through `lane-worktree`'s own
    copy-and-re-read, then re-read each one there -> how many were kept, or None having said
    `verdict` (NOT LANDED, or LANDING INCOMPLETE when finishing a marked lane).

    `expected`: {path: md5} taken before anything was written; a file whose bytes moved since, or
    that is gone, means the lane changed underneath the landing. ★ ALL OF IT BEFORE ANY REMOVAL
    -- this check used to run after `delete-worktree --delete-evidence`, where a file the copy
    missed was found missing only once it was gone (✔MEASURED, the review of lane `lf`)."""
    lw = _lane_worktree()
    rel = os.path.relpath(wt, root).replace(os.sep, "/")
    try:
        counted = sum(n for _top, n in lw.count_evidence(wt, roots))
        if counted:
            lw.preserve_evidence(wt, rel, destination, counted, roots)
        now = evidence_digests(wt, roots)
    except lw.Refused as exc:
        print("lane-fold: %s -- the evidence could not be preserved, so nothing was removed. "
              "The fold and the rows ARE landed; the worktree and its evidence are KEPT:" % verdict)
        for ln in exc.lines:
            print("      " + ln)
        return None
    except OSError as exc:
        print("lane-fold: %s -- the evidence could not be re-read (%s), so nothing was removed. "
              "The fold and the rows ARE landed; the worktree and its evidence are KEPT." % (verdict, exc))
        return None
    moved = sorted(p for p, digest in expected.items() if now.get(p) != digest)
    lost = sorted(p for p, digest in now.items()
                  if not os.path.isfile(os.path.join(destination, *p.split("/")))
                  or md5_file(os.path.join(destination, *p.split("/"))) != digest)
    if moved or lost:
        why = []
        if lost:
            why.append("%d of %d file(s) are not at %s byte-identical (%s)"
                       % (len(lost), len(now), destination, ", ".join(lost[:5])))
        if moved:
            why.append("%d file(s) changed in the lane since the landing measured them (%s)"
                       % (len(moved), ", ".join(moved[:5])))
        print("lane-fold: %s -- EVIDENCE CHECK FAILED, before any removal: %s. Nothing was "
              "removed; the fold and the rows ARE landed, and the worktree and its evidence are KEPT."
              % (verdict, "; ".join(why)))
        return None
    print("lane-fold:       the evidence: %d file(s) under %s kept at %s, each re-read"
          % (len(now), " and ".join("/".join(r) + "/" for r in roots), destination))
    return len(now)


def _settle_removal(root, lane, wt, argv, door, destination, rows_line):
    """Run the removal, then verify what `land` claims -> 0 LANDED, or 4 LANDING INCOMPLETE.

    ⚠ REMOVE, THEN VERIFY, THEN SPEAK -- and this verb verifies what IT claims: the directory gone,
    git's registration gone, and no copy left in DssHarness's record. Any of them left, or an exit
    code DssHarness gave with none left, is INCOMPLETE -- never LANDED -- and names the one remedy
    that is always safe from here: `land` again, which finishes a marked lane and folds nothing.
    `argv` None: nothing is left for DssHarness to remove, and it is not asked (it would answer
    "No worktree named '<lane>'", exit 13); the verification still runs."""
    if argv is None:
        print("lane-fold: [5/5] nothing is left to remove: no directory, no registration, no recorded copy")
        rc, said = 0, ""
    else:
        print("lane-fold: [5/5] %s" % " ".join('"%s"' % a if " " in a else a for a in argv))
        rc, said = _run_removal(argv, root)
    for ln in said.splitlines():
        print("      " + ln)
    left_over = []
    if os.path.lexists(wt):
        left_over.append("%s is still on disk" % wt)
    listed, why = _registered(root, wt)
    if listed is None:
        left_over.append("git cannot say whether it still records the worktree (%s)" % why)
    elif listed:
        left_over.append("git still records %s as a worktree" % wt)
    copies, unread = copies_left(door, root, lane)
    if copies is None:
        left_over.append("DssHarness's record of host copies could not be read (%s)" % unread)
    elif copies:
        left_over.append("DssHarness's record still holds %d copy(ies) of it: %s"
                         % (len(copies), ", ".join("%s: %s" % c for c in copies)))
    if rc != 0 and not left_over:
        left_over.append("DssHarness exited %d although the worktree, its registration and every "
                         "recorded copy are gone -- its lines above say why" % rc)
    if left_over:
        print("lane-fold: LANDING INCOMPLETE -- %s%s. The fold and the rows are in the main tree and "
              "the evidence is kept at %s; the lane is marked landed, so nothing will fold it again. "
              "Settle what DssHarness names above, then run `lane-fold.py land %s production --apply` "
              "again: it folds nothing, and finishes the removal or names the command that does."
              % ("; ".join(left_over), " (DssHarness exited %d)" % rc if rc != 0 else "", destination, lane))
        return 4
    print("lane-fold: LANDED lane %s -- %s; the worktree, its registration and every copy DssHarness "
          "recorded are gone, and the evidence is at %s." % (lane, rows_line, destination))
    return 0


def _refuse_standing_inside(lane, wt):
    """Refuse (exit 2) a landing started from INSIDE the lane: a removal cannot take a directory a
    process stands in -- DssHarness refuses one on Windows, naming its own directory rather than
    the caller's, and elsewhere the caller is left standing in a directory that is gone."""
    here = os.getcwd()
    if _owning_tree().is_within(here, wt, strict=False):
        die("this process's working directory %s is inside lane %s (%s), and the landing ends by "
            "removing that directory. Nothing was written or removed; run `land` from the main tree."
            % (here, lane, wt))


def _lane_record(root, wt, paths):
    """-> {path: md5, or None where the lane holds no file} for every path lane `wt`'s `git status`
    lists and every one of `paths` -- read from the LANE. What `land` marks, and what a re-run
    compares the lane with: never the main tree, which later lanes go on changing."""
    lanes = layout(root).worktrees
    record = {}
    for rel in sorted(set(changed_paths(wt)) | set(paths)):
        if is_lane_tree(rel, lanes):
            continue
        src = os.path.join(wt, *rel.split("/"))
        record[rel] = md5_file(src) if os.path.isfile(src) else None
    return record


def _changed_since_mark(root, wt, seed, record):
    """-> the paths lane `wt` holds NOW with bytes its landing did not measure: a file whose md5 is
    not its record's, or one the record holds no file for. A file the lane no longer holds is NOT
    listed: a removal only deletes, so a deletion since the mark is the debris of the removal that
    stopped part way (✔MEASURED 2026-09-29 with DssHarness 0.6.1: git deleted a worktree's files and
    then stopped), and taking it for work would leave every such lane unremovable."""
    now = _lane_record(root, wt, set(seed) | set(record))
    changed = sorted(rel for rel, digest in now.items() if digest is not None and record.get(rel) != digest)
    # ⚠ A PATH THE MAIN TREE'S RULES IGNORE IS NEVER WORK: a fold would not carry it. Asked of the MAIN
    # tree, because a removal that stops part way can delete the lane's own `.gitignore` first -- then
    # every evidence file and build artefact reads as new (✔MEASURED, the re-review of lane `lf`: such a
    # lane could never finish). The main tree's rules are the lane's, or what its fold made them.
    if not changed:
        return []
    ignored = _ignored_by(root, changed)
    return [rel for rel in changed if rel not in ignored]


def _ignored_by(root, rels):
    """-> the subset of `rels` (repository-relative) the tree at `root` ignores by its own rules,
    whether or not a file is there -- and never a TRACKED one: git consults the index, so a file
    tracked under an ignored directory, which a fold does carry, is not reported (✔MEASURED, the
    re-review of lane `lf`: with `--no-index` it was, and an edit to it made after the mark would have
    been removed with the lane). Refuses (exit 2) when git cannot say: an unknown answer is not
    "nothing ignored"."""
    proc = git_run(["-C", root, "check-ignore", "--stdin", "-z"], input="\0".join(rels),
                   capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode not in (0, 1):
        die("`git check-ignore` exited %d in %s, so which of the lane's paths are ignored cannot be told: %s"
            % (proc.returncode, root, (proc.stderr or "").strip()[:300]))
    return set(p for p in proc.stdout.split("\0") if p)


def _fresh_directory(parent, name):
    """`<parent>/<name>`, or `<name>-2`, `-3` ... -- the first that does not exist yet."""
    first = os.path.join(parent, name)
    candidate, n = first, 1
    while os.path.exists(candidate):
        n += 1
        candidate = "%s-%d" % (first, n)
    return candidate


def _clear_mark(root, lane, manifest):
    """The lane is gone everywhere: its manifest keeps what it was seeded with and drops the mark, so
    a new lane of the name starts unmarked."""
    save_manifest(manifest_path(root, lane), Manifest(manifest.base, manifest.paths))


def _finish_landing(root, lane, wt, manifest, state, door, apply_it, settled, preserve_to):
    """`land` on a lane MARKED landed: its fold, rows and evidence were done and verified before
    the mark was written, and its removal did not finish. -> 0 LANDED, 2 not this lane, 3 usage,
    4 LANDING INCOMPLETE.

    Nothing is folded again, and nothing is discarded unseen, measured against the LANE's own
    record in the mark, never the main tree (which later lanes go on changing -- ✔MEASURED, the
    re-review of lane `lf`: comparing with it refused a finished lane forever once another lane
    edited a path it had landed): a file the lane holds with bytes its landing did not measure is
    work done since, which a human copies out; a path the main tree's rules ignore never is. A NEW
    worktree of the name (another identity) is not the landed lane at all -- `seed` starts it clean.
    Evidence is kept again, into a FRESH directory beside the first (`--preserve-to` names another):
    a run that held a log when the removal was refused goes on writing it, and one destination would
    then refuse every re-run as a clash. A directory with no `.git` of its own has its evidence kept
    and is left, with the `--force` removal named, for a person to look at."""
    landed = manifest.landed
    if settled:
        die("--settled leaves paths out of a FOLD, and lane %s, landed at %s, is never folded again: "
            "nothing was read, written or removed." % (lane, landed.at), 3)
    print("lane-fold: LAND lane %s -- FINISHING: it was landed at %s (its fold and rows are in the main "
          "tree, its evidence is kept at %s), and its removal did not finish; %s"
          % (lane, landed.at, landed.evidence, {
              "gone": "its directory is gone", "husk": "its directory is no longer a git worktree",
              "worktree": "it is still a registered worktree"}[state]))
    if state == "worktree":
        ident = _worktree_identity(wt)
        if ident is None:
            print("lane-fold: NOT FINISHED -- git names no administrative directory for %s, so whether it is "
                  "the worktree landed at %s or a NEW one of this name cannot be told. Its mark is kept and "
                  "nothing of it was read or removed: a person decides, and once what it holds is copied out, "
                  "`dssharness delete-worktree %s --discard-uncommitted --delete-evidence` removes it."
                  % (wt, landed.at, lane))
            return 2
        if ident != landed.worktree:
            print("lane-fold: NOT FINISHED -- %s is not the worktree landed at %s: git knows it as another "
                  "worktree (its administrative directory was made since), so it is a NEW lane of this name "
                  "that skipped the mandatory `seed`. Nothing of it was read or removed; `lane-fold.py seed "
                  "%s` starts it clean (the landed lane's fold, rows and evidence are in already)."
                  % (wt, landed.at, lane))
            return 2
        changed = _changed_since_mark(root, wt, manifest.paths, landed.lane)
        if changed:
            print("lane-fold: LANDING INCOMPLETE -- lane %s holds %d file(s) its landing did not measure, "
                  "changed or new since %s: work done in a lane already landed, which nothing folds "
                  "again and nothing discards unseen. Copy it out, then remove the lane with "
                  "`dssharness delete-worktree %s --discard-uncommitted --delete-evidence`."
                  % (lane, len(changed), landed.at, lane))
            for rel in changed[:10]:
                print("   %s" % rel)
            return 4
    if not apply_it:
        print("lane-fold: dry run -- pass --apply to %s." % (
            "keep its evidence again and have the `--force` removal named for you: nothing here removes a "
            "directory with no `.git`" if state == "husk" else "finish the removal"))
        return 0
    destination = landed.evidence
    if state != "gone":
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        destination = preserve_to or _fresh_directory(landed.evidence, "finish-" + stamp)
        roots = _lane_worktree().load_settings(root).evidence_roots
        try:
            expected = evidence_digests(wt, roots)
        except OSError as exc:
            print("lane-fold: LANDING INCOMPLETE -- the evidence left in %s cannot be read (%s); nothing "
                  "was removed." % (wt, exc))
            return 4
        if _keep_evidence(root, wt, roots, destination, expected, "LANDING INCOMPLETE") is None:
            return 4
    if state == "husk":
        # ★ NEVER FORCED FROM HERE. `--force` checks nothing, and a directory with no `.git` of its own
        # is the husk a stopped removal leaves -- or a directory somebody made at that path since, which
        # nothing here can tell apart (✔MEASURED, the re-review of lane `lf`: a plain directory made
        # there after the worktree was gone read as a husk, and its files went with a forced delete).
        print("lane-fold: LANDING INCOMPLETE -- %s is no longer a git worktree (it holds no `.git` of its own): "
              "what a removal that stopped part way leaves, or a directory made there since. Its evidence is "
              "kept at %s (each run keeps a fresh copy). Look at what else it holds; once nothing in it is "
              "needed, `dssharness delete-worktree %s --force` removes it (DssHarness refuses it without "
              "`--force`, and nothing here forces it), and `lane-fold.py land %s production --apply` then "
              "drops the mark." % (wt, destination, lane, lane))
        return 4
    argv = worktree_removal_argv(root, lane, landed, door)
    if state == "gone" and _registered(root, wt)[0] is False and copies_left(door, root, lane)[0] == []:
        argv = None
    rc = _settle_removal(root, lane, wt, argv, door, destination,
                         "finished the removal its landing at %s began" % landed.at)
    if rc == 0:
        _clear_mark(root, lane, manifest)
    return rc


def cmd_land(root, lane, default_bucket, apply_it, settled=(), preserve_to=None):
    wt, manifest, state = _open_lane(root, lane, closed_ok=True)
    if apply_it:
        _refuse_standing_inside(lane, wt)
    anchors = load_anchors_module(root)
    try:
        door = anchors.door_executable()
    except anchors.Refused as exc:
        print("lane-fold: NOT LANDED -- %s Nothing was written or removed." % exc)
        return 2
    if manifest.landed is not None:
        return _finish_landing(root, lane, wt, manifest, state, door, apply_it, settled, preserve_to)
    base = lane_base(root, lane, wt, manifest)
    print("lane-fold: LAND lane %s (base %s)" % (lane, base[:12]))
    lw = _lane_worktree()

    # ── 1. MEASURE EVERYTHING; WRITE NOTHING ────────────────────────────────────
    cls = classify(root, wt, manifest.paths, settled, base=base)
    before = registry_md5(root, anchors)
    plans, problems = plan_rows(root, wt, lane, default_bucket, anchors)
    # ★ The evidence roots the configuration names NOW, before the fold -- step 4 adds the ones it
    #   names after it -- enumerated the way `lane-worktree` enumerates them (`evidence_digests`).
    roots_before = lw.load_settings(root).evidence_roots
    try:
        evidence = evidence_digests(wt, roots_before)
    except OSError as exc:
        print("lane-fold: NOT LANDED -- the lane's evidence cannot be read (%s); nothing written, nothing "
              "removed." % exc)
        return 2
    destination = preserve_to or default_evidence_destination(root, lane)
    print("lane-fold: [1/5] the fold")
    if not cls.refusals:
        _print_fold_plan(lane, manifest.paths, cls)
    print("lane-fold: [2/5] the rows (%d)" % len(plans))
    _print_row_plans(plans)
    print("lane-fold: [3/5] the evidence: %d file(s) under %s, to be kept at %s"
          % (len(evidence), " and ".join("/".join(r) + "/" for r in roots_before), destination))
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

    # ── 4. THE EVIDENCE KEPT AND RE-READ, THE LANE MARKED, THEN THE REMOVAL -- BY DSSHARNESS ──
    # ★ ONLY HERE, the step after the lane's LAST review. The operator, 2026-09-28: "only removes
    #   copies after last review is fine: while modifications are needed the files can't be removed"
    #   -- `fold` never removes anything, so a lane a review sends back keeps its worktree and copies.
    # The evidence FIRST, through `lane-worktree`'s own copy-and-re-read (its gate: a destination
    # inside the lane, a same-named file with other bytes, a count that moved -- each refused), over
    # the roots named before the fold AND after it: a lane that changed `worktrees.evidenceRoots`
    # has now carried that change into the tree DssHarness reads, and a root it dropped would
    # otherwise die unkept (✔MEASURED, the review of lane `lf`: `scratchpad/pad.log`, announced at
    # [3/5], was never copied and was gone after the removal). Every file is re-read against the
    # digests step 1 took BEFORE any removal is asked for.
    roots = _evidence_root_union(roots_before, lw.load_settings(root).evidence_roots)
    kept = _keep_evidence(root, wt, roots, destination, evidence)
    if kept is None:
        return 2
    # ★ `after` is handed over: the discard flag is built from it and from nothing else.
    argv = worktree_removal_argv(root, lane, after, door)
    # ★★ THE MARK, BEFORE THE REMOVAL IS ASKED FOR. From here a re-run of `land` finishes the
    #   removal and folds nothing (`_finish_landing`), and no verb folds this lane again: a removal
    #   that stops part way leaves debris a second fold would read as the lane's intent. It records
    #   which worktree this is (`_worktree_identity`) and the lane's own bytes, which a re-run compares the lane
    #   with; the base is the one `lane_base` measured above, so a format-1 lane's mark is format 2.
    #   A landing that completes clears it again (`_clear_mark`).
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    identity = _worktree_identity(wt)
    if identity is None:
        die("lane %s measured as a worktree, but git names no administrative directory for it to mark; "
            "nothing was removed." % lane)
    save_manifest(manifest_path(root, lane),
                  Manifest(base, manifest.paths,
                           Landed(stamp, destination, identity, _lane_record(root, wt, manifest.paths))))
    rc = _settle_removal(root, lane, wt, argv, door, destination,
                         "%d path(s) written and %d removed; %d row(s) applied and %d already landed, "
                         "every row re-read; %d evidence file(s) kept, each re-read"
                         % (len(cls.mine), len(cls.deleted),
                            sum(1 for p in plans if p.action != "landed"),
                            sum(1 for p in plans if p.action == "landed"), kept))
    if rc == 0:
        _clear_mark(root, lane, Manifest(base, manifest.paths))
    return rc


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
        # ★ The mark first: a landing that completes drops it, so a marked lane -- directory or not --
        # is one whose removal did not finish (a registration or a host copy can outlive the directory).
        if _landed_mark_of(os.path.join(mdir, "seed-%s.json" % lane)) is not None:
            print("   ⚠ %s: LANDED, but its removal did not finish%s -- `land %s production --apply` "
                  "finishes it or names what does" % (lane, "" if lane in lanes else " (its directory is gone)",
                                                     lane))
        elif lane not in lanes:
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
        # ★ A REAL `git worktree` of this repository, as every lane is. This fixture used to make
        # the lane a repository of its own at the same content, which every verb now refuses
        # (`_require_worktree`): a directory that is not a registered worktree of the tree it
        # folds into is exactly what a removal that stopped part way leaves behind.
        run("worktree", "add", "--detach", wt, "HEAD")

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
        # ⓘ The subject is committed on BOTH sides -- the main tree's branch and the lane's
        # detached HEAD, two commits of one repository -- before the lane deletes it: that is
        # what makes `git status` in the lane say `D` rather than "untracked", and the lane's
        # own HEAD the base `classify` measures it against.
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
            """The file's text, or None when there is none: a pin reading what a broken subject never
            wrote must come out red, not stop every arm after it with a traceback."""
            if not os.path.isfile(path):
                return None
            with io.open(path, encoding="utf-8", newline="") as fh:
                return fh.read()

        # (v1)-(v2) A SEEDED PATH THE LANE RESTORED TO HEAD'S BYTES IS THE LANE'S CHANGE. ✔MEASURED 2026-09-28
        # (P68's PR exit, lane `ci59b`): `classify` read only the lane's `git status`, which lists no path
        # whose bytes equal HEAD's, so the fold accounted for 8 of 9 seeded paths and kept the main
        # tree's seeded bytes over the lane's revert, saying nothing. The negative is measured first:
        # the lane's own status does not list the reverted path.
        rq = os.path.join(top, "revert")
        make_repo(rq, [(".gitignore", IGNORE), ("cfg.json", "{}\n"), ("kept.txt", "base\n")])
        write_atomic(os.path.join(rq, "cfg.json"), '{"seeded": true}\n')    # the main tree's uncommitted state
        write_atomic(os.path.join(rq, "kept.txt"), "main edit\n")
        write_atomic(os.path.join(rq, "gone.txt"), "a new file\n")         # untracked: seeded too
        write_atomic(os.path.join(rq, "drop.txt"), "another new file\n")   # untracked, seeded, and...
        git(rq, "worktree", "add", "--detach", "%s/q" % layout(rq).worktrees, "HEAD")
        with contextlib.redirect_stdout(io.StringIO()):
            cmd_seed(rq, "q")                                             # carries all four in
        wq = worktree_path(rq, "q")
        write_atomic(os.path.join(wq, "cfg.json"), "{}\n")                  # the lane puts it back to HEAD
        for tree in (rq, wq):                                             # and the new file goes from BOTH
            os.remove(os.path.join(tree, "gone.txt"))
        os.remove(os.path.join(wq, "drop.txt"))                           # ...deleted by the lane alone
        manifest_q = load_manifest(manifest_path(rq, "q"))
        status_q = changed_paths(wq)
        mine_q, del_q, inh_q, ref_q, _set_q, conv_q = classify(rq, wq, manifest_q.paths, base=manifest_q.base)
        pin(sorted(manifest_q.paths) == ["cfg.json", "drop.txt", "gone.txt", "kept.txt"]
            and "cfg.json" not in status_q and "drop.txt" not in status_q
            and mine_q == ["cfg.json"] and inh_q == ["kept.txt"] and conv_q == ["gone.txt"] and not ref_q,
            "(v1) a SEEDED path the lane restored to HEAD's bytes -- absent from its own git status -- is the "
            "lane's change, not dropped; the one it never touched is inherited, and one gone from both "
            "trees is already landed, not skipped unseen",
            "seeded=%s status=%s mine=%s inherited=%s converged=%s refusals=%s"
            % (sorted(manifest_q.paths), status_q, mine_q, inh_q, conv_q, ref_q))
        pin(del_q == ["drop.txt"],
            "(v3) a SEEDED untracked file the lane DELETED -- absent from its git status too, since git "
            "never tracked it -- is the lane's deletion, not a path already landed",
            "deleted=%s converged=%s" % (del_q, conv_q))
        rv_q, out_q = quiet(cmd_fold, rq, "q", True)
        pin(rv_q == 0 and text(os.path.join(rq, "cfg.json")) == "{}\n"
            and text(os.path.join(rq, "kept.txt")) == "main edit\n"
            and not os.path.exists(os.path.join(rq, "drop.txt"))
            and "4 seeded path(s): 1 this lane's, 1 inherited, 1 already landed, 1 deleted, 0 settled" in out_q,
            "(v2) ...the fold WRITES the revert, keeps the inherited edit, carries the deletion, and its plan "
            "counts every seeded path by where it went", "rv=%r out=%s" % (rv_q, out_q[-500:]))

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
                # declared by the arms whose subject is not the rows (`existing_row`); no arm changes it
                "| `%s-STANDING` | P2 | 🟠 OPEN | 🟠 **OPEN** a row no arm changes | w | r |" % FX,
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
        real_remove_argv = globals()["worktree_removal_argv"]
        events = []

        def spy_classify(*a, **k):
            result = real_classify(*a, **k)
            events.append(("classify", result))
            return result

        def spy_remove_argv(root_, lane_, verified_, door_):
            # Recorded BEFORE delegating, so a call that then refuses is still seen.
            events.append(("remove-argv", verified_))
            argv_ = real_remove_argv(root_, lane_, verified_, door_)
            events.append(("remove-argv-built", argv_))
            return argv_

        globals()["classify"] = spy_classify
        globals()["worktree_removal_argv"] = spy_remove_argv
        try:
            rv, out = quiet(cmd_land, r3, "land1", "production", True, (), dest1)
        finally:
            globals()["classify"] = real_classify
            globals()["worktree_removal_argv"] = real_remove_argv
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
        man1 = load_manifest(manifest_path(r3, "land1"))
        pin(man1.landed is None and man1.base is not None and "LANDED lane land1" in out,
            "(l1c) ...and a landing that completes drops the mark it wrote: the manifest keeps what the lane "
            "was seeded with, and a new lane of the name starts unmarked", "manifest=%r" % (man1,))

        # (l7b) THE DISCARD FLAG IS BUILT FROM THE POST-FOLD MEASUREMENT, RIGHT AFTER IT IS TAKEN.
        # `dssharness delete-worktree` refuses a worktree whose own `git status` lists uncommitted
        # work -- every lane's does -- unless told --discard-uncommitted. So the order must be:
        # the post-fold `classify` finds nothing left to fold, and the removal is built from THAT
        # result object, with the flag in it.
        kinds = [e[0] for e in events]
        built = [e[1] for e in events if e[0] == "remove-argv-built"]
        at = kinds.index("remove-argv") if "remove-argv" in kinds else -1
        post = events[at - 1][1] if at >= 1 and events[at - 1][0] == "classify" else None
        pin(kinds.count("remove-argv") == 1 and post is not None and events[at][1] is post
            and not (post.mine or post.deleted or post.refusals) and len(built) == 1
            and built[0][1:3] == ["delete-worktree", "land1"] and "--discard-uncommitted" in built[0]
            and built[0][0] == A3.door_executable(),
            "(l7b) land builds its removal -- DssHarness's delete-worktree, through the door the anchors "
            "module resolves, WITH the discard flag -- from the post-fold measurement that found nothing "
            "left to fold, immediately after taking it",
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

        def spy_remove_argv7(root_, lane_, verified_, door_):
            calls7.append(verified_)
            return real_remove_argv(root_, lane_, verified_, door_)

        globals()["_apply_fold"] = lambda root_, wt_, cls_: None
        globals()["worktree_removal_argv"] = spy_remove_argv7
        try:
            rv, out = quiet(cmd_land, r3, "land7", "production", True, (),
                            os.path.join(top, "evidence-land7"))
        finally:
            globals()["_apply_fold"] = real_apply_fold
            globals()["worktree_removal_argv"] = real_remove_argv
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
        rv_dirty, out_dirty = quiet(worktree_removal_argv, r3, "x", dirty, "DOOR")
        rv_none, out_none = quiet(worktree_removal_argv, r3, "x", None, "DOOR")
        pin(rv_dirty == 2 and "DISCARDS" in out_dirty and rv_none == 2 and "DISCARDS" in out_none,
            "(l7c) the removal builder REFUSES a measurement with work left, and no measurement "
            "at all", "dirty=%r none=%r" % (rv_dirty, rv_none))
        clean = Classification([], [], [], [], [], ["converged.txt"])
        rv_clean, _out_clean = quiet(worktree_removal_argv, r3, "x", clean, "DOOR")
        pin(rv_clean == ["DOOR", "delete-worktree", "x", "--discard-uncommitted", "--delete-evidence",
                         "--no-prompt", "-C", r3],
            "(l7c2) CONTROL: handed nothing left to fold, it builds DssHarness's removal of the worktree "
            "and its host copies, with the discard flags", "got=%r" % (rv_clean,))
        # (l7d) A REMOVAL BUILT FROM A LANDING MARK IS THE SAME CHECKED ONE: nothing here builds `--force`.
        mark7 = Landed("2026-09-29T00:00:00Z", os.path.join(top, "e7d"), "1:2", {})
        rv_fl, _out_fl = quiet(worktree_removal_argv, r3, "x", mark7, "DOOR")
        pin(rv_fl == ["DOOR", "delete-worktree", "x", "--discard-uncommitted", "--delete-evidence",
                      "--no-prompt", "-C", r3],
            "(l7d) a removal built from a landing mark is the same checked removal -- never `--force`",
            "mark=%r" % (rv_fl,))

        # (l8) A COMMIT INSIDE A LANE NEVER REACHES A REMOVAL THROUGH `land`. `land` builds a
        # removal that discards the lane's uncommitted work, and a commit no ref reaches would go
        # with the worktree, so `land` must refuse such a lane before it writes anything -- with the base RECORDED
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

        def spy_remove_argv8(root_, lane_, verified_, door_):
            calls8.append(verified_)
            return real_remove_argv(root_, lane_, verified_, door_)

        def land8():
            calls8[:] = []
            globals()["worktree_removal_argv"] = spy_remove_argv8
            try:
                return quiet(cmd_land, r3, "land8", "production", True, (),
                             os.path.join(top, "evidence-land8"))
            finally:
                globals()["worktree_removal_argv"] = real_remove_argv

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

        # (l10) A HOST DSSHARNESS CANNOT REACH. The real removal runs -- the worktree goes here -- and only its
        # exit code is the unreachable host's (15), while its record still holds that host's copy: `land`
        # must say the landing is INCOMPLETE, naming the copy and how to finish, never "LANDED".
        def existing_row(wt_, lane_):
            """The cells of a row the registry already holds, exactly: the landing plans it ALREADY
            LANDED and asks the door nothing -- for the arms whose subject is not the rows (each row
            written costs two `dssharness` runs, its dry run and its write)."""
            cells(wt_, lane_, FX + "-STANDING", "🟠 OPEN", "🟠 **OPEN** a row no arm changes", closing="w",
                  crossrefs="r")

        wl10 = make_lane(r3, "land10")
        write_atomic(os.path.join(wl10, "work-land10.txt"), "w10\n")
        write_atomic(os.path.join(wl10, ".temp", "land10-scratch", "findings.log"), "f10\n")
        existing_row(wl10, "land10")
        real_run_removal = globals()["_run_removal"]
        real_list = globals()["_list_worktrees"]

        def record_holding(name, host):
            """A record that still holds `host`'s copy of worktree `name`, as `list-worktree --json`
            prints one: under `gone`, with the command that removes it."""
            doc = {"worktrees": [], "elsewhere": [], "gone": [
                {"name": name, "tree": "t", "copies": [{"host": host, "path": "~/src/r.worktree-" + name}],
                 "deletedBy": "dssharness delete-worktree " + name}]}
            return lambda door_, root_: (0, json.dumps(doc), "")

        # ⓘ TWO TEST DOUBLES for arms whose subject is neither DssHarness's removal nor its record: git
        # removes the worktree (as DssHarness's own removal does, without a host to ask), and the record
        # holds nothing. Each real `delete-worktree` costs about 2 s; the arms that pin the real removal
        # and the real record keep them -- (l1), (l3b), (l4b), (l7a2), (l10e), (l11b), (l11d), (l14d),
        # (l15b), (e4).
        def fast_removal(argv_, root_):
            git(root_, "worktree", "remove", "--force", worktree_path(root_, argv_[2]))
            return 0, ""

        def empty_record(argv_, root_):
            return 0, json.dumps({"worktrees": [], "elsewhere": [], "gone": []}), ""

        def unreachable_host(argv_, root_):
            _rc, said_ = fast_removal(argv_, root_)
            return 15, said_ + "delete-worktree: ssh macos: could not be reached; its copy stays recorded\n"

        globals()["_run_removal"] = unreachable_host
        globals()["_list_worktrees"] = record_holding("land10", "ssh macos")
        try:
            rv, out = quiet(cmd_land, r3, "land10", "production", True, (),
                            os.path.join(top, "evidence-land10"))
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        kept10 = os.path.join(top, "evidence-land10", ".temp", "land10-scratch", "findings.log")
        pin(rv == 4 and not os.path.exists(wl10) and "LANDING INCOMPLETE" in out
            and "ssh macos: ~/src/r.worktree-land10" in out and "could not be reached" in out
            and "land land10 production --apply" in out
            and os.path.isfile(kept10) and text(kept10) == "f10\n",
            "(l10) a host DssHarness cannot reach: the worktree is removed here and its evidence kept, and "
            "land says the landing is INCOMPLETE, naming the copy DssHarness's record still holds and the "
            "command that finishes it", "rv=%r out=%s" % (rv, out[-600:]))
        rv_ls, out_ls = quiet(cmd_list, r3)
        pin(rv_ls == 0 and "land10: LANDED, but its removal did not finish (its directory is gone)" in out_ls,
            "(l10f) list names a landing whose removal did not finish, its directory gone or not",
            "out=%s" % out_ls[-400:])
        # (l10e) ...and `land` again, the record holding nothing now, finishes the removal and folds nothing.
        rv, out = quiet(cmd_land, r3, "land10", "production", True, (), None)
        pin(rv == 0 and "FINISHING" in out and "LANDED lane land10" in out and "WROTE" not in out
            and "the rows (" not in out,
            "(l10e) ...and `land` again finishes the removal a marked lane began, folding and applying "
            "nothing", "rv=%r out=%s" % (rv, out[-500:]))

        # (l10b) DSSHARNESS EXITS 0 AND ITS RECORD STILL HOLDS A COPY: only reading the record sees it.
        wl10b = make_lane(r3, "land10b")
        write_atomic(os.path.join(wl10b, "work-land10b.txt"), "w10b\n")
        existing_row(wl10b, "land10b")
        globals()["_run_removal"] = fast_removal
        globals()["_list_worktrees"] = record_holding("land10b", "wsl Ubuntu")
        try:
            rv, out = quiet(cmd_land, r3, "land10b", "production", True, (),
                            os.path.join(top, "evidence-land10b"))
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        pin(rv == 4 and not os.path.exists(wl10b) and "record still holds 1 copy" in out
            and "wsl Ubuntu: ~/src/r.worktree-land10b" in out and "LANDED lane" not in out,
            "(l10b) DssHarness exiting 0 while its record still holds a copy is INCOMPLETE, never LANDED -- "
            "the exit code alone could not have seen it", "rv=%r out=%s" % (rv, out[-500:]))

        # (l10c) A RECORD THAT CANNOT BE READ IS NEVER "NO COPY LEFT".
        wl10c = make_lane(r3, "land10c")
        write_atomic(os.path.join(wl10c, "work-land10c.txt"), "w10c\n")
        existing_row(wl10c, "land10c")
        globals()["_run_removal"] = fast_removal
        globals()["_list_worktrees"] = lambda argv_, root_: (12, "", "list-worktree: FAIL - unreadable\n")
        try:
            rv, out = quiet(cmd_land, r3, "land10c", "production", True, (),
                            os.path.join(top, "evidence-land10c"))
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        pin(rv == 4 and not os.path.exists(wl10c) and "could not be read" in out
            and "exited 12" in out and "LANDED lane" not in out,
            "(l10c) a record of host copies that cannot be read makes the landing INCOMPLETE, never LANDED",
            "rv=%r out=%s" % (rv, out[-500:]))

        # (l10d) A NON-ZERO EXIT WITH NOTHING LEFT ANYWHERE IS STILL SAID, NEVER READ AS SUCCESS.
        wl10d = make_lane(r3, "land10d")
        write_atomic(os.path.join(wl10d, "work-land10d.txt"), "w10d\n")
        existing_row(wl10d, "land10d")

        def fails_after(argv_, root_):
            _rc, said_ = fast_removal(argv_, root_)
            return 20, said_ + "delete-worktree: FAIL - after the removal\n"

        globals()["_run_removal"] = fails_after
        globals()["_list_worktrees"] = empty_record
        try:
            rv, out = quiet(cmd_land, r3, "land10d", "production", True, (),
                            os.path.join(top, "evidence-land10d"))
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        pin(rv == 4 and not os.path.exists(wl10d) and "DssHarness exited 20 although" in out
            and "LANDED lane" not in out,
            "(l10d) DssHarness exiting non-zero with the worktree, its registration and every recorded copy "
            "gone is INCOMPLETE, and says so", "rv=%r out=%s" % (rv, out[-500:]))

        # (l11) DSSHARNESS DECLINES THE REMOVAL (a run holds the worktree): nothing is removed; the lane is
        # marked already, so `land` says INCOMPLETE with DssHarness's exit code, and a re-run, DssHarness
        # willing, FINISHES it -- measuring the intact worktree "nothing left to fold" and folding nothing.
        wl11 = make_lane(r3, "land11")
        write_atomic(os.path.join(wl11, "work-land11.txt"), "w11\n")
        existing_row(wl11, "land11")
        globals()["_run_removal"] = lambda argv_, root_: (13, "delete-worktree: FAIL - a run holds it\n")
        globals()["_list_worktrees"] = empty_record
        try:
            rv, out = quiet(cmd_land, r3, "land11", "production", True, (),
                            os.path.join(top, "evidence-land11"))
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        pin(rv == 4 and os.path.isdir(wl11) and "LANDING INCOMPLETE" in out and "still on disk" in out
            and "(DssHarness exited 13)" in out
            and load_manifest(manifest_path(r3, "land11")).landed is not None,
            "(l11) DssHarness declining the removal leaves the worktree, marked landed, and land says "
            "INCOMPLETE with its exit code", "rv=%r out=%s" % (rv, out[-400:]))
        rv, out = quiet(cmd_land, r3, "land11", "production", True, (), None)
        pin(rv == 0 and not os.path.exists(wl11) and "FINISHING" in out and "WROTE" not in out
            and load_manifest(manifest_path(r3, "land11")).landed is None,
            "(l11b) ...and a re-run, DssHarness willing, finishes it without folding anything again, and "
            "drops the mark", "rv=%r out=%s" % (rv, out[-300:]))

        # (l11f)-(l11h) FINISHING MEASURES THE LANE AGAINST ITS OWN RECORD, KEEPS LATER EVIDENCE APART, AND
        # REFUSES A FOLD OPTION. ✔MEASURED (the re-review of lane `lf`): compared with the MAIN tree, a
        # declined lane stayed unfinishable once another lane edited a path it had landed; and a log a run
        # went on writing after the refusal made every re-run clash with the first evidence copy.
        wl11f = make_lane(r3, "land11f")
        write_atomic(os.path.join(wl11f, "work-land11f.txt"), "w11f\n")
        write_atomic(os.path.join(wl11f, ".temp", "land11f-scratch", "run.log"), "line 1\n")
        existing_row(wl11f, "land11f")
        dest11f = os.path.join(top, "evidence-land11f")
        globals()["_run_removal"] = lambda argv_, root_: (13, "delete-worktree: FAIL - a run holds it\n")
        globals()["_list_worktrees"] = empty_record
        try:
            rv, out = quiet(cmd_land, r3, "land11f", "production", True, (), dest11f)
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        write_atomic(os.path.join(r3, "work-land11f.txt"), "a later lane's edit\n")          # the main tree moves on
        write_atomic(os.path.join(wl11f, ".temp", "land11f-scratch", "run.log"), "line 1\nline 2\n")  # the run goes on
        rv_h, out_h = quiet(cmd_land, r3, "land11f", "production", True, ("work-land11f.txt",), None)
        rv_f, out_f = quiet(cmd_land, r3, "land11f", "production", True, (), None)
        finished = sorted(n for n in os.listdir(dest11f) if n.startswith("finish-")) if os.path.isdir(dest11f) else []
        pin(rv == 4 and rv_h == 3 and "never folded again" in out_h and rv_f == 0 and not os.path.exists(wl11f)
            and text(os.path.join(r3, "work-land11f.txt")) == "a later lane's edit\n",
            "(l11f) a declined lane FINISHES although the main tree moved on under it -- measured against its "
            "own record -- and nothing of it is folded over the later edit; a --settled on it is refused",
            "rv=%r settled=%r finish=%r out=%s" % (rv, rv_h, rv_f, out_f[-400:]))
        pin(text(os.path.join(dest11f, ".temp", "land11f-scratch", "run.log")) == "line 1\n"
            and len(finished) == 1
            and text(os.path.join(dest11f, finished[0], ".temp", "land11f-scratch", "run.log"))
            == "line 1\nline 2\n",
            "(l11g) ...its evidence written since is kept apart, in a fresh directory beside the first copy, "
            "never refused as a clash with it", "finish-dirs=%r" % (finished,))

        # (l11c) THE DIRECTORY GONE WHILE GIT STILL RECORDS THE WORKTREE IS NOT A LANDING.
        wl11c = make_lane(r3, "land11c")
        write_atomic(os.path.join(wl11c, "work-land11c.txt"), "w11c\n")
        existing_row(wl11c, "land11c")

        def removes_only_the_directory(argv_, root_):
            _owning_tree().remove_tree(wl11c)          # the files go; git's record of the worktree stays
            return 0, ""

        globals()["_run_removal"] = removes_only_the_directory
        globals()["_list_worktrees"] = empty_record
        try:
            rv, out = quiet(cmd_land, r3, "land11c", "production", True, (),
                            os.path.join(top, "evidence-land11c"))
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        pin(rv == 4 and not os.path.exists(wl11c) and "git still records" in out and "LANDED lane" not in out,
            "(l11c) the worktree's directory gone while git still records it is INCOMPLETE, never LANDED",
            "rv=%r out=%s" % (rv, out[-400:]))
        rv, out = quiet(cmd_land, r3, "land11c", "production", True, (), None)
        pin(rv == 0 and _registered(r3, wl11c) == (False, "") and "LANDED lane land11c" in out,
            "(l11d) ...and `land` again has DssHarness clear git's record of it", "rv=%r out=%s"
            % (rv, out[-400:]))
        # (l11e) A `git worktree list` THAT FAILS IS "CANNOT SAY", NEVER "UNREGISTERED" -- asked of a
        # directory outside every repository, where git exits 128.
        with _owning_tree().sandbox() as sb11:
            listed11, why11 = _registered(sb11.box, wl11c)
            state11, swhy11 = _worktree_state(sb11.box, wl11c)
        pin(listed11 is None and "exited" in why11 and state11 is None and swhy11 == why11,
            "(l11e) a worktree list git cannot give is reported as unknown, never as unregistered",
            "listed=%r why=%r state=%r" % (listed11, why11, state11))

        # (l12) ONLY `land` REMOVES. A fold is the step a lane takes before its last review, and a review may
        # still send it back (the operator, 2026-09-28): the fold writes the lane's work and asks for no
        # removal, so the worktree -- and every host's copy of it -- stays.
        wl12 = make_lane(r3, "land12")
        write_atomic(os.path.join(wl12, "work-land12.txt"), "w12\n")
        asked12 = []
        globals()["_run_removal"] = lambda argv_, root_: asked12.append(argv_) or (0, "")
        try:
            rv, out = quiet(cmd_fold, r3, "land12", True)
        finally:
            globals()["_run_removal"] = real_run_removal
        pin(rv == 0 and os.path.isdir(wl12) and not asked12
            and os.path.isfile(os.path.join(r3, "work-land12.txt")),
            "(l12) a FOLD writes the lane's work and asks for no removal: the worktree and its host copies "
            "stay for a review that may send the lane back", "rv=%r asked=%r out=%s" % (rv, asked12, out[-300:]))

        # (l13) EVIDENCE THE COPY DID NOT TAKE STOPS THE LANDING BEFORE ANY REMOVAL IS ASKED FOR. ✔MEASURED
        # (the review of lane `lf`): the md5 check ran after `delete-worktree --delete-evidence`, so a
        # file the copy missed was found missing once it was gone. A copy that takes nothing stands in.
        wl13 = make_lane(r3, "land13")
        write_atomic(os.path.join(wl13, "work-land13.txt"), "w13\n")
        write_atomic(os.path.join(wl13, ".temp", "land13-scratch", "findings.log"), "f13\n")
        existing_row(wl13, "land13")
        lw13 = _lane_worktree()
        real_preserve = lw13.preserve_evidence
        asked13 = []
        lw13.preserve_evidence = lambda *a_, **k_: None
        globals()["_run_removal"] = lambda argv_, root_: asked13.append(argv_) or (0, "")
        try:
            rv, out = quiet(cmd_land, r3, "land13", "production", True, (),
                            os.path.join(top, "evidence-land13"))
        finally:
            lw13.preserve_evidence = real_preserve
            globals()["_run_removal"] = real_run_removal
        pin(rv == 2 and os.path.isdir(wl13) and not asked13
            and "EVIDENCE CHECK FAILED, before any removal" in out
            and load_manifest(manifest_path(r3, "land13")).landed is None
            and text(os.path.join(wl13, ".temp", "land13-scratch", "findings.log")) == "f13\n",
            "(l13) evidence the copy did not take stops the landing BEFORE any removal is asked for: the "
            "worktree, its evidence and its unmarked manifest are kept",
            "rv=%r asked=%d out=%s" % (rv, len(asked13), out[-500:]))
        dest13 = os.path.join(top, "evidence-land13b")
        globals()["_run_removal"] = fast_removal
        globals()["_list_worktrees"] = empty_record
        try:
            rv, out = quiet(cmd_land, r3, "land13", "production", True, (), dest13)
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        pin(rv == 0 and not os.path.exists(wl13)
            and text(os.path.join(dest13, ".temp", "land13-scratch", "findings.log")) == "f13\n",
            "(l13b) ...and a re-run with the real copy lands it", "rv=%r out=%s" % (rv, out[-300:]))

        # (l14) A REMOVAL THAT STOPS PART WAY LEAVES DEBRIS A SECOND FOLD WOULD READ AS THE LANE'S INTENT.
        # git deleted one file of a still-registered worktree and stopped; the lane is marked, so nothing
        # folds it again -- the file stays in the main tree -- and `land` again finishes the removal, the
        # deletion being the removal's own debris.
        wl14 = make_lane(r3, "land14")
        write_atomic(os.path.join(wl14, "work-land14.txt"), "w14\n")
        existing_row(wl14, "land14")

        def deletes_part(argv_, root_):
            os.remove(os.path.join(wl14, "tracked.txt"))
            return 20, "delete-worktree: FAIL - git could not remove worktree 'land14'\n"

        globals()["_run_removal"] = deletes_part
        globals()["_list_worktrees"] = empty_record
        try:
            rv, out = quiet(cmd_land, r3, "land14", "production", True, (),
                            os.path.join(top, "evidence-land14"))
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        pin(rv == 4 and os.path.isdir(wl14) and not os.path.exists(os.path.join(wl14, "tracked.txt"))
            and "LANDING INCOMPLETE" in out,
            "(l14) a removal that stopped part way leaves the lane marked and the landing INCOMPLETE",
            "rv=%r out=%s" % (rv, out[-400:]))
        tracked14 = text(os.path.join(r3, "tracked.txt"))
        rv_f, out_f = quiet(cmd_fold, r3, "land14", True)
        rv_a, out_a = quiet(cmd_apply_rows, r3, "land14", "production", True)
        pin(rv_f == 2 and "was LANDED" in out_f and rv_a == 2 and "was LANDED" in out_a
            and text(os.path.join(r3, "tracked.txt")) == tracked14,
            "(l14c) fold and apply-rows refuse a lane marked landed: the file the stopped removal deleted is "
            "never carried into the main tree", "fold=%r rows=%r out=%s" % (rv_f, rv_a, out_f[-300:]))
        rv, out = quiet(cmd_land, r3, "land14", "production", True, (), None)
        pin(rv == 0 and not os.path.exists(wl14) and text(os.path.join(r3, "tracked.txt")) == tracked14
            and "FINISHING" in out and "WROTE" not in out,
            "(l14b) ...and `land` again finishes the removal: the file git deleted is its debris, and it "
            "stays in the main tree", "rv=%r out=%s" % (rv, out[-500:]))

        # (l14f) A STOPPED REMOVAL THAT DELETED THE LANE'S OWN `.gitignore` STILL FINISHES. ✔MEASURED (the
        # re-review of lane `lf`): read through the lane's own rules, every evidence file and build artefact
        # then looked new since the mark, and the lane could never finish. The main tree's rules decide.
        wl14f = make_lane(r3, "land14f")
        write_atomic(os.path.join(wl14f, "work-land14f.txt"), "w14f\n")
        write_atomic(os.path.join(wl14f, ".temp", "land14f-scratch", "notes.log"), "n14f\n")
        write_atomic(os.path.join(wl14f, "__pycache__", "m.pyc"), "bytecode\n")
        existing_row(wl14f, "land14f")

        def deletes_the_ignore_file(argv_, root_):
            for rel in (".gitignore", "tracked.txt"):
                os.remove(os.path.join(wl14f, rel))
            return 20, "delete-worktree: FAIL - git could not remove worktree 'land14f'\n"

        globals()["_run_removal"] = deletes_the_ignore_file
        globals()["_list_worktrees"] = empty_record
        try:
            rv, out = quiet(cmd_land, r3, "land14f", "production", True, (),
                            os.path.join(top, "evidence-land14f"))
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        rv2, out2 = quiet(cmd_land, r3, "land14f", "production", True, (), None)
        pin(rv == 4 and rv2 == 0 and not os.path.exists(wl14f) and text(os.path.join(r3, ".gitignore")) == IGNORE
            and os.path.isfile(os.path.join(r3, "tracked.txt")),
            "(l14f) a removal that stopped after deleting the lane's own .gitignore still finishes: the main "
            "tree's rules say what is ignored, and neither deletion reaches the main tree",
            "first=%r again=%r out=%s" % (rv, rv2, out2[-500:]))

        # (l14e) A FILE CHANGED OR NEW SINCE THE MARK IS WORK, NEVER DISCARDED UNSEEN.
        wl14e = make_lane(r3, "land14e")
        write_atomic(os.path.join(wl14e, "work-land14e.txt"), "w14e\n")
        existing_row(wl14e, "land14e")
        globals()["_run_removal"] = lambda argv_, root_: (13, "delete-worktree: FAIL - a run holds it\n")
        globals()["_list_worktrees"] = empty_record
        try:
            quiet(cmd_land, r3, "land14e", "production", True, (), os.path.join(top, "evidence-land14e"))
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        write_atomic(os.path.join(wl14e, "work-land14e.txt"), "w14e, edited after the landing\n")
        write_atomic(os.path.join(wl14e, "late.txt"), "written after the landing\n")
        rv, out = quiet(cmd_land, r3, "land14e", "production", True, (), None)
        pin(rv == 4 and os.path.isdir(wl14e) and "did not measure" in out and "late.txt" in out
            and "work-land14e.txt" in out
            and text(os.path.join(wl14e, "late.txt")) == "written after the landing\n",
            "(l14e) a marked lane holding a file changed or new since the mark is INCOMPLETE and kept, the "
            "files named", "rv=%r out=%s" % (rv, out[-500:]))
        rm14 = _owning_tree().run_unsteered(
            [A3.door_executable(), "delete-worktree", "land14e", "--discard-uncommitted", "--delete-evidence",
             "--no-prompt", "-C", r3], cwd=r3, capture_output=True)
        pin(rm14.returncode == 0 and not os.path.exists(wl14e),
            "(l14d) ...and the removal that refusal names settles it once the work is kept",
            "rc=%r said=%s" % (rm14.returncode, (rm14.stdout + rm14.stderr).decode("utf-8", "replace")[-300:]))

        # (l15) A HUSK: git deleted the `.git` file and its record and left the directory -- ✔MEASURED with
        # DssHarness 0.6.1 on a worktree holding a directory junction. Git walks UP from such a directory,
        # so read as a lane it would answer with the MAIN TREE. `land` again keeps its evidence and names the
        # `--force` removal for a person -- a directory made at that path since reads the same (the
        # re-review of lane `lf`) -- touching nothing of the main tree; once it is gone, `land` finishes.
        wl15 = make_lane(r3, "land15")
        write_atomic(os.path.join(wl15, "work-land15.txt"), "w15\n")
        existing_row(wl15, "land15")
        write_atomic(os.path.join(r3, "main-only.txt"), "the main tree's own untracked file\n")

        def husk_of(wt_, lane_):
            os.remove(os.path.join(wt_, ".git"))
            git(r3, "worktree", "prune")
            return 20, ("delete-worktree: FAIL - git reported worktree '%s' removed, but '%s' still exists.\n"
                        % (lane_, wt_))

        globals()["_run_removal"] = lambda argv_, root_: husk_of(wl15, "land15")
        globals()["_list_worktrees"] = empty_record
        try:
            rv, out = quiet(cmd_land, r3, "land15", "production", True, (),
                            os.path.join(top, "evidence-land15"))
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        main15 = {rel: text(os.path.join(r3, rel)) for rel in ("tracked.txt", "main-only.txt")}
        pin(rv == 4 and os.path.isdir(wl15) and not os.path.exists(os.path.join(wl15, ".git"))
            and "still on disk" in out,
            "(l15) a removal that left a husk -- no `.git` file, no record -- is INCOMPLETE",
            "rv=%r out=%s" % (rv, out[-400:]))
        asked15 = []
        globals()["_run_removal"] = lambda argv_, root_: asked15.append(argv_) or (0, "")
        try:
            rv, out = quiet(cmd_land, r3, "land15", "production", True, (), None)
        finally:
            globals()["_run_removal"] = real_run_removal
        pin(rv == 4 and os.path.isdir(wl15) and not asked15 and "no longer a git worktree" in out
            and "delete-worktree land15 --force" in out and "WROTE" not in out
            and all(text(os.path.join(r3, rel)) == body for rel, body in main15.items()),
            "(l15b) ...and `land` again asks for no removal of the husk: it names the `--force` removal for "
            "a person, and the main tree is untouched", "rv=%r asked=%d out=%s" % (rv, len(asked15), out[-500:]))
        rm15 = _owning_tree().run_unsteered(
            [A3.door_executable(), "delete-worktree", "land15", "--force", "--no-prompt", "-C", r3],
            cwd=r3, capture_output=True)
        rv, out = quiet(cmd_land, r3, "land15", "production", True, (), None)
        pin(rm15.returncode == 0 and rv == 0 and not os.path.exists(wl15)
            and load_manifest(manifest_path(r3, "land15")).landed is None
            and all(text(os.path.join(r3, rel)) == body for rel, body in main15.items()),
            "(l15c) ...and once that removal is run, `land` again finds nothing left and drops the mark",
            "rm=%r rv=%r out=%s" % (rm15.returncode, rv, out[-400:]))
        # (l15e) A `.git` THAT IS THERE BUT UNREADABLE IS NO HUSK: nothing is forced over it. ✔MEASURED (the
        # re-review of lane `lf`): a still-registered lane whose `.git` git could not read was taken for a
        # husk and would have been removed with `--force`, work written after the mark and all. The file is
        # broken, then REWRITTEN with its own bytes -- a new time, as `git worktree repair` leaves it -- and
        # the lane must still be the worktree the mark names (its identity is git's, not the `.git` file's).
        wl15e = make_lane(r3, "land15e")
        write_atomic(os.path.join(wl15e, "work-land15e.txt"), "w15e\n")
        existing_row(wl15e, "land15e")
        globals()["_run_removal"] = lambda argv_, root_: (13, "delete-worktree: FAIL - a run holds it\n")
        globals()["_list_worktrees"] = empty_record
        try:
            quiet(cmd_land, r3, "land15e", "production", True, (), os.path.join(top, "evidence-land15e"))
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        gitfile15e = os.path.join(wl15e, ".git")
        with io.open(gitfile15e, "rb") as fh:
            good15e = fh.read()
        asked15e = []
        globals()["_run_removal"] = lambda argv_, root_: asked15e.append(argv_) or (0, "")
        try:
            with io.open(gitfile15e, "r+b") as fh:
                fh.truncate(0)
                fh.write(("gitdir: %s\n" % os.path.join(top, "no-such-gitdir")).encode("utf-8"))
            rv, out = quiet(cmd_land, r3, "land15e", "production", True, (), None)
        finally:
            globals()["_run_removal"] = real_run_removal
            write_atomic(gitfile15e, good15e.decode("utf-8"))      # its own bytes, as a NEW file
        pin(rv == 2 and not asked15e and "cannot tell" in out and os.path.isdir(wl15e),
            "(l15e) a still-registered lane whose `.git` git cannot read is no husk: land cannot tell, and "
            "forces nothing over it", "rv=%r asked=%d out=%s" % (rv, len(asked15e), out[-400:]))
        rv, out = quiet(cmd_land, r3, "land15e", "production", True, (), None)
        pin(rv == 0 and not os.path.exists(wl15e) and "FINISHING" in out,
            "(l15f) ...and once git can read it again -- its `.git` a new file with its own bytes -- the SAME "
            "worktree finishes: the identity is git's administrative directory, not the `.git` file",
            "rv=%r out=%s" % (rv, out[-400:]))
        # (l15d) AN UNMARKED HUSK IS REFUSED BY EVERY VERB BEFORE GIT IS ASKED ANYTHING ABOUT IT.
        wl15d = make_lane(r3, "land15d")
        write_atomic(os.path.join(wl15d, "work-land15d.txt"), "w15d\n")
        os.remove(os.path.join(wl15d, ".git"))
        git(r3, "worktree", "prune")
        rv_f, out_f = quiet(cmd_fold, r3, "land15d", True)
        rv_l, out_l = quiet(cmd_land, r3, "land15d", "production", True, (),
                            os.path.join(top, "evidence-land15d"))
        rv_s, out_s = quiet(cmd_seed, r3, "land15d", True)
        pin(all(r == 2 for r in (rv_f, rv_l, rv_s))
            and all("is not its own git worktree" in o for o in (out_f, out_l, out_s))
            and all(text(os.path.join(r3, rel)) == body for rel, body in main15.items())
            and not os.path.exists(os.path.join(r3, "work-land15d.txt")) and os.path.isdir(wl15d),
            "(l15d) a lane directory that is not its own git worktree is refused by fold, land and seed "
            "alike, before git is asked anything about it: the main tree is untouched",
            "fold=%r land=%r seed=%r out=%s" % (rv_f, rv_l, rv_s, out_f[-300:]))
        _owning_tree().remove_tree(wl15d)
        os.remove(os.path.join(r3, "main-only.txt"))

        # (l16) A LANE WHOSE FOLD DROPS AN EVIDENCE ROOT KEEPS THAT ROOT'S EVIDENCE TOO. ✔MEASURED (the
        # review of lane `lf`): the lane set `worktrees.evidenceRoots` without `scratchpad`, the fold
        # carried that into the main tree, and the copy -- reading the roots only after the fold -- never
        # took `scratchpad/pad.log`, which the removal then deleted. The roots before AND after are kept.
        cfg16 = os.path.join(r3, ".harness-config", "config.json")
        with io.open(cfg16, "rb") as fh:
            cfg16_bytes = fh.read()
        cfg16_doc = _owning_tree().load_jsonc(cfg16)
        roots16 = list(cfg16_doc["worktrees"]["evidenceRoots"])
        wl16 = make_lane(r3, "land16")
        cfg16_doc["worktrees"]["evidenceRoots"] = [r for r in roots16 if r != "scratchpad"]
        write_atomic(os.path.join(wl16, ".harness-config", "config.json"), json.dumps(cfg16_doc, indent=2) + "\n")
        write_atomic(os.path.join(wl16, "scratchpad", "pad.log"), "pad16\n")
        write_atomic(os.path.join(wl16, ".temp", "land16-scratch", "f.log"), "f16\n")
        existing_row(wl16, "land16")
        dest16 = os.path.join(top, "evidence-land16")
        globals()["_run_removal"] = fast_removal
        globals()["_list_worktrees"] = empty_record
        try:
            rv, out = quiet(cmd_land, r3, "land16", "production", True, (), dest16)
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
            with io.open(cfg16, "wb") as fh:           # the fixture's own roots back, for every arm after
                fh.write(cfg16_bytes)
        pad16 = os.path.join(dest16, "scratchpad", "pad.log")
        pin("scratchpad" in roots16 and rv == 0 and not os.path.exists(wl16)
            and os.path.isfile(pad16) and text(pad16) == "pad16\n"
            and text(os.path.join(dest16, ".temp", "land16-scratch", "f.log")) == "f16\n",
            "(l16) a lane whose fold drops an evidence root keeps that root's evidence too: the roots the "
            "configuration names before the fold AND after it are copied, before any removal",
            "roots=%r rv=%r out=%s" % (roots16, rv, out[-500:]))

        # (l17) A LANDING STARTED FROM INSIDE THE LANE IS REFUSED BEFORE IT WRITES ANYTHING: its last step
        # removes the directory the process stands in.
        wl17 = make_lane(r3, "land17")
        write_atomic(os.path.join(wl17, "work-land17.txt"), "w17\n")
        existing_row(wl17, "land17")
        before_l17 = reg_now()
        here17 = os.getcwd()
        os.chdir(wl17)
        try:
            rv, out = quiet(cmd_land, r3, "land17", "production", True, (),
                            os.path.join(top, "evidence-land17"))
        finally:
            os.chdir(here17)
        pin(rv == 2 and "is inside lane land17" in out and os.path.isdir(wl17)
            and not os.path.exists(os.path.join(r3, "work-land17.txt")) and reg_now() == before_l17
            and load_manifest(manifest_path(r3, "land17")).landed is None,
            "(l17) land started from inside the lane is refused before it writes anything",
            "rv=%r out=%s" % (rv, out[-300:]))

        # (lm1)-(lm4) THE LANDED MARK: read back exactly, a malformed one refused, and `seed` resetting it for a
        # NEW worktree of the name, and for the landed one only while it lists no change. (Not (m1)-(m4): those
        # labels are refresh-plans' arms, and one label on two arms leaves a red ambiguous.)
        mp_m = os.path.join(top, "mark-seed.json")
        mark_m = Landed("2026-09-29T00:00:00Z", "C:/kept/evidence", "1:2", {"a.txt": "d" * 32, "b.txt": None})
        save_manifest(mp_m, Manifest("0" * 40, {"p.txt": "d" * 32}, mark_m))
        back_m = load_manifest(mp_m)
        with io.open(mp_m, encoding="utf-8") as fh:
            doc_m = json.load(fh)
        doc_m["landed"]["lane"] = ["a.txt"]                      # a list where the record belongs
        write_atomic(mp_m, json.dumps(doc_m))
        bad_rv, bad_out = quiet(load_manifest, mp_m)
        pin(back_m == Manifest("0" * 40, {"p.txt": "d" * 32}, mark_m) and bad_rv == 2
            and "not a well-formed" in bad_out,
            "(lm1) a manifest's landed mark is read back exactly, and a malformed one is refused",
            "back=%r bad=%r" % (back_m, bad_rv))
        wm = make_lane(r3, "markm")
        mpath_m = manifest_path(r3, "markm")
        base_m = load_manifest(mpath_m).base
        own_m = Landed("2026-09-29T00:00:00Z", "C:/kept", _worktree_identity(wm), {})
        save_manifest(mpath_m, Manifest(base_m, {}, own_m))
        rv_m2, _out_m2 = quiet(cmd_seed, r3, "markm", True)
        pin(rv_m2 == 0 and load_manifest(mpath_m).landed is None,
            "(lm2) seed RESETS the landed lane's mark while it lists no change -- nothing to fold",
            "rv=%r" % (rv_m2,))
        write_atomic(os.path.join(wm, "work-markm.txt"), "debris or work\n")
        save_manifest(mpath_m, Manifest(base_m, {}, own_m))
        rv_m3, out_m3 = quiet(cmd_seed, r3, "markm", True)
        rv_m3b, out_m3b = quiet(cmd_seed, r3, "markm", False, True)
        pin(rv_m3 == 2 and "was LANDED" in out_m3 and rv_m3b == 2 and "was LANDED" in out_m3b
            and load_manifest(mpath_m).landed is not None,
            "(lm3) ...and REFUSES to, `--empty` and `--force` alike, for the landed lane while it lists "
            "changes: wiping its mark would let the next fold carry them", "empty=%r force=%r out=%s"
            % (rv_m3, rv_m3b, out_m3b[-300:]))
        real_identity = _worktree_identity
        globals()["_worktree_identity"] = lambda wt_: None                 # git names no administrative directory
        try:
            rv_m3c, out_m3c = quiet(cmd_seed, r3, "markm", False, True)
            rv_m3d, out_m3d = quiet(cmd_land, r3, "markm", "production", True, (), None)
        finally:
            globals()["_worktree_identity"] = real_identity
        pin(rv_m3c == 2 and "was LANDED" in out_m3c and rv_m3d == 2 and "cannot be told" in out_m3d
            and load_manifest(mpath_m).landed == own_m
            and text(os.path.join(wm, "work-markm.txt")) == "debris or work\n",
            "(lm3b) ...and when git cannot name the lane's identity, `seed --force` refuses too and `land` will "
            "not finish it: a worktree that cannot be told from the landed one is never taken for a new one",
            "seed=%r land=%r out=%s" % (rv_m3c, rv_m3d, out_m3d[-300:]))
        other_m = Landed("2026-09-29T00:00:00Z", "C:/kept", "0:0", {})    # the mark of an EARLIER worktree
        save_manifest(mpath_m, Manifest(base_m, {}, other_m))
        rv_m4, out_m4 = quiet(cmd_land, r3, "markm", "production", True, (), None)
        rv_m4f, out_m4f = quiet(cmd_fold, r3, "markm", False)
        rv_m4s, _out_m4s = quiet(cmd_seed, r3, "markm", True)
        pin(rv_m4 == 2 and "NEW lane" in out_m4 and rv_m4f == 2 and "NEW worktree of this name" in out_m4f
            and os.path.isdir(wm) and text(os.path.join(wm, "work-markm.txt")) == "debris or work\n"
            and rv_m4s == 0 and load_manifest(mpath_m).landed is None,
            "(lm4) a mark left by an EARLIER worktree of the name: land and fold refuse the new one, naming the "
            "skipped seed, reading and removing nothing, and seed starts it clean whatever it holds",
            "land=%r fold=%r seed=%r out=%s" % (rv_m4, rv_m4f, rv_m4s, out_m4f[-300:]))

        # (l14g) A TRACKED FILE UNDER AN IGNORED DIRECTORY IS WORK A FOLD CARRIES, SO AN EDIT TO IT SINCE THE
        # MARK STOPS THE FINISH. ✔MEASURED (the re-review of lane `lf`): asked with `--no-index`, git called
        # such a file ignored, and the finish removed the lane edit and all. It commits to the fixture's main
        # tree, so the lanes made after it -- (e4)'s, (l5)'s and (l6)'s -- start from that commit.
        write_atomic(os.path.join(r3, "__pycache__", "kept-config.txt"), "v1\n")
        git(r3, "add", "-f", "__pycache__/kept-config.txt")
        git(r3, "-c", "user.email=s@e.invalid", "-c", "user.name=s", "commit", "-q", "-m", "a tracked file under an ignored directory")
        wl14g = make_lane(r3, "land14g")
        write_atomic(os.path.join(wl14g, "work-land14g.txt"), "w14g\n")
        existing_row(wl14g, "land14g")
        globals()["_run_removal"] = lambda argv_, root_: (13, "delete-worktree: FAIL - a run holds it\n")
        globals()["_list_worktrees"] = empty_record
        try:
            quiet(cmd_land, r3, "land14g", "production", True, (), os.path.join(top, "evidence-land14g"))
        finally:
            globals()["_run_removal"] = real_run_removal
            globals()["_list_worktrees"] = real_list
        write_atomic(os.path.join(wl14g, "__pycache__", "kept-config.txt"), "v2, edited after the landing\n")
        rv, out = quiet(cmd_land, r3, "land14g", "production", True, (), None)
        pin(rv == 4 and os.path.isdir(wl14g) and "__pycache__/kept-config.txt" in out
            and text(os.path.join(wl14g, "__pycache__", "kept-config.txt")) == "v2, edited after the landing\n",
            "(l14g) a TRACKED file under an ignored directory, edited since the mark, stops the finish: the "
            "main tree's rules ignore only what git does not track", "rv=%r out=%s" % (rv, out[-400:]))

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
        # DssHarness as a child; handed a steering environment, that child's own git
        # would act on another repository and leave THIS one
        # registering a worktree that is gone. The negative is proven first: `worktree list`,
        # asked the defect's way, does not see this repository's lane at all.
        # ⚠ GIT_COMMON_DIR joins the steering names here: DssHarness's own git client clears GIT_DIR,
        # GIT_WORK_TREE and GIT_INDEX_FILE and nothing else, so with only those three this arm could
        # not tell whether THIS program strips the environment (the review of lane `lf`).
        wl9 = make_lane(r3, "land9")
        write_atomic(os.path.join(wl9, "work-land9.txt"), "w9\n")
        cells(wl9, "land9", FX + "-LANDNINE", "✅ CLOSED", "✅ **CLOSED** land9",
              bucket="production", priority="P3")
        with ot.steering() as steer:
            steer = dict(steer, GIT_COMMON_DIR=steer["GIT_DIR"])
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
            "repository's worktree registrations -- the DssHarness child does not inherit it",
            "negative-synthesized=%s rv=%r registered-after=%s out=%s"
            % (real9, rv, "land9" in listed9, str(out)[-400:]))

        # (e5) ...AND EACH CHILD `land` STARTS SEES NONE OF THE NAMES GIT CALLS REPOSITORY-LOCAL, whatever
        # DssHarness itself clears: a probe child stands in for it through both seams, under a caller
        # exporting every such name.
        probe_env = [sys.executable, "-c",
                     "import json, os; print(json.dumps(sorted(k for k in os.environ if k.startswith('GIT'))))"]
        local_names = sorted(ot.local_git_env_names())
        with ot.caller_environment({n: os.path.join(top, "steer-" + n) for n in local_names}):
            seen_here = sorted(n for n in local_names if n in os.environ)
            rc_r, said_r = _run_removal(probe_env, r3)
            rc_l, said_l, _err_l = _list_worktrees(probe_env, r3)
        seen_r = json.loads(said_r.strip() or "null")
        seen_l = json.loads(said_l.strip() or "null")
        pin(seen_here == local_names and len(local_names) >= 3 and rc_r == 0 and rc_l == 0
            and isinstance(seen_r, list) and isinstance(seen_l, list)
            and not set(seen_r) & set(local_names) and not set(seen_l) & set(local_names),
            "(e5) the removal's child and the record's child see none of git's repository-local names a "
            "caller exported", "exported=%s removal-saw=%s record-saw=%s" % (seen_here, seen_r, seen_l))

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
