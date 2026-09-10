#!/usr/bin/env python3
# PURPOSE: seed a lane worktree from the main tree and fold only that lane's real changes back.
"""lane-fold.py -- SEED a lane worktree, then FOLD exactly what that lane changed.

★★★ WHY THIS IS A REPOSITORY SCRIPT AND NOT A SCRATCH FILE. Every cycle that runs
parallel lanes needs these two verbs, and every cycle before 2026-08-29 rewrote them
from scratch in a session-scoped scratchpad that does not survive the session. The
`/dss-cycle` handoff of 2026-08-28 recorded that waste explicitly and left the
promotion as a decision to take; `scripts/wsl-leg/wsl-leg.sh`'s own header records the
identical waste happening three times inside ONE session. The operator's standing rule
is *"if a tool has a problem, fix before using again, not workaround an own tool.
reusable tools exists to avoid bunch of problems like mangling or edge cases"* -- and a
tool that is retyped every session re-opens every edge case below at once.

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
THE FOLD'S REFUSALS -- each one is a way a fold can destroy work while looking clean

  * the destination's CURRENT md5 differs from the SEED md5 -> something changed it
    since seeding (a sibling lane's fold, or the orchestrator), and writing would
    destroy that change UNSEEN. Refuse the whole fold; nothing is written.
  * a path absent from the manifest is measured against the HEAD BLOB instead.
    ⚠ ABSENT FROM THE MANIFEST DOES NOT MEAN NEW -- the manifest holds only paths that
    differed from HEAD at seeding time, so an ordinary tracked file no prior lane
    touched is simply not in it. An earlier draft called those "lane-new" and refused
    four of a lane's honestly-modified files.
  * a path that ESCAPES the repository, compared by RESOLVED PATH PREFIX, never by
    substring. (A substring test accepts `C:/Source/.../dss-code-prime-evil/x`.)

★ REFUSALS ARE ALL-OR-NOTHING AND ARE COLLECTED BEFORE ANYTHING IS WRITTEN. A fold
that writes nine files and then refuses the tenth leaves a tree nobody can reason
about; a fold that refuses before writing leaves a tree that is still exactly what it
was.

★ AND A COLLISION IS SUPPOSED TO FAIL LOUD. When two lanes edit ONE shared document
(`src/dss-config/sources/c.lang.json` is the usual one), the second fold REFUSES with
`main tree DRIFTED since seeding`. That is the designed behaviour: the orchestrator
then merges the second lane's declared keys by hand. Silently overwriting would revert
the first lane with no diff to show for it.

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
describes -- which matters because `lane-worktree.sh remove` deletes the lane.

Every write is write-temp + `os.replace`. This script NEVER runs a git write verb,
never stages, and never touches `.git/`.


ⓘ NO `.ps1` TWIN, DELIBERATELY. This is a `.py`, which runs unchanged on the
Windows leg and on every POSIX leg, so a PowerShell sibling would be a SECOND
IMPLEMENTATION of something that was never split -- two programs to keep in
step where one has no host to fail on. The omission is stated rather than
merely taken, because a gate cannot tell a deliberate POSIX-or-portable-only
script from a forgotten twin.
Exit codes: 0 OK · 2 refused (nothing written) · 3 usage error.

⚠ THE TREE ACTED ON IS THE ONE THIS SCRIPT LIVES IN, never the caller's cwd --
[[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]], see `repo_root`. `--repo <path>`
names another tree deliberately, and works with every verb.

Usage:
    python scripts/lane-fold/lane-fold.py seed <lane>           # carry the main
                                          #   tree's uncommitted state into the lane
    python scripts/lane-fold/lane-fold.py seed <lane> --empty   # created at HEAD and
                                          #   given nothing: record the manifest only
    python scripts/lane-fold/lane-fold.py fold <lane> [--apply]     # dry run without --apply
    python scripts/lane-fold/lane-fold.py refresh-plans <lane> [--apply]
                                          #   re-copy .plans/ into a LIVE lane and
                                          #   update its manifest, so a row applied
                                          #   mid-cycle stops reddening its guards
    python scripts/lane-fold/lane-fold.py list
    python scripts/lane-fold/lane-fold.py --self-test
"""
from __future__ import annotations

import hashlib
import io
import json
import os
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

WORKTREES_DIR = ".worktrees"
MANIFEST_DIR = os.path.join(WORKTREES_DIR, ".manifests")


def die(msg, code=2):
    print("lane-fold: REFUSED -- %s" % msg)
    sys.exit(code)


def repo_root(anchor=None):
    """The root of the working tree that CONTAINS THIS SCRIPT (or `anchor`).

    ⚠ A predecessor hardcoded `C:\\Source\\DailySoftware\\dss-code-prime`, which makes
    the tool unusable from a worktree, from any clone, and on every non-Windows leg.

    ★★★ AND ITS REPLACEMENT -- a bare `git rev-parse --show-toplevel` -- TRADED THAT
    FOR A SUBTLER WRONG ANSWER. [[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]]
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
    `.worktrees/`" -- is recorded with its measurement in `lane-worktree.sh`'s
    `_repo_root`; briefly, from a lane it aims a removal at a live SIBLING lane, and
    for a submodule checkout it names a directory inside `.git`.

    ⓘ ONE SPELLING PER LANGUAGE. The `.sh` and `.ps1` halves of this fix route
    through their existing owners (`leg_tree_owning_root`, `Get-RepoTreeOwningRoot`).
    Python's named owner is `scripts/carriage-excludes/carriage-excludes.py`, whose
    `_git_prefix`/`_git` answer "how do I run git against a tree I have already
    identified" -- the same adjacent question the other two owners' identity
    functions answer, and not this one. This is the only Python caller that needs the
    derivation, so it lives here, once, rather than in a shared module with one user.
    """
    if anchor is None:
        anchor = os.path.dirname(os.path.realpath(__file__))
    out = subprocess.run(["git", "-C", anchor, "rev-parse", "--show-toplevel"],
                         capture_output=True)
    if out.returncode != 0:
        die("no git working tree contains %s.\n"
            "  This script resolves the tree IT LIVES IN, never the caller's cwd;\n"
            "  pass --repo <path> to name a different tree deliberately." % anchor, 3)
    return os.path.realpath(out.stdout.decode("utf-8", "surrogateescape").strip())


def changed_paths(cwd):
    """Every changed path under `cwd`, expanded through directories, forward-slashed.

    Records are `XY <path>`; a rename or copy adds a SECOND record holding the source
    path, which is why the loop takes RECORDS rather than pairs -- both spellings are
    paths a fold has to reason about.
    """
    out = subprocess.run(["git", "-C", cwd, "status", "--porcelain", "-z"],
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
            # `scripts/lane-fold/__pycache__/*.pyc` as the lane's own work, because
            # the script directory is new and therefore untracked, and `.pyc` is
            # gitignored but the walk never asked. A fold would have written build
            # artefacts into the main tree --
            # D-CYCLE-LANE-FOLD-WALKS-AN-UNTRACKED-DIRECTORY-PAST-GITIGNORE.
            # ⓘ This does NOT make the `is_lane_tree` floor below redundant, and the
            # two must not be confused: git's answer is about what is IGNORED, the
            # floor is about the worktree machinery, and a repository whose
            # `.gitignore` lost its `/.worktrees/` line would have git list a
            # sibling lane as plain untracked work.
            listed = subprocess.run(
                ["git", "-C", cwd, "ls-files", "--others", "--exclude-standard",
                 "-z", "--", p.rstrip("/")],
                capture_output=True, check=True).stdout.decode(
                    "utf-8", "surrogateescape")
            for q in listed.split("\0"):
                if q:
                    paths.append(q.replace("\\", "/"))
        else:
            paths.append(p.replace("\\", "/"))
    return paths


def is_lane_tree(rel):
    """A path that belongs to the worktree machinery itself. -> bool

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
    exactly as `scripts/carriage-excludes/` pins `.worktrees/` in its own
    MUST_NEVER_TRAVEL floor rather than trusting the same line.
    """
    return rel == WORKTREES_DIR or rel.startswith(WORKTREES_DIR + "/") \
        or rel == ".git" or rel.startswith(".git/")


def md5_file(path):
    with io.open(path, "rb") as fh:
        return hashlib.md5(fh.read()).hexdigest()


def inside(root, path):
    """Resolved-PREFIX containment. Never a substring test."""
    return (os.path.realpath(path) + os.sep).startswith(root + os.sep)


def manifest_path(root, lane):
    return os.path.join(root, MANIFEST_DIR, "seed-%s.json" % lane)


def worktree_path(root, lane):
    return os.path.join(root, WORKTREES_DIR, lane)


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


# ──────────────────────────────────── seed ─────────────────────────────────────

def cmd_seed(root, lane, empty=False, force=False):
    wt = worktree_path(root, lane)
    if not os.path.isdir(wt):
        die("no worktree at %s\n"
            "  create it first: bash scripts/lane-worktree/lane-worktree.sh add %s"
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
    own = [q for q in changed_paths(wt) if not is_lane_tree(q)]
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
    # set, and `fold` then measures every changed path against the HEAD blob, which is
    # exactly right.
    paths = [] if empty else sorted(
        q for q in set(changed_paths(root)) if not is_lane_tree(q))
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

    mpath = manifest_path(root, lane)
    write_atomic(mpath, json.dumps(manifest, indent=1, sort_keys=True))
    print("lane-fold: seeded lane %s with %d path(s) from the main tree"
          % (lane, len(manifest)))
    print("lane-fold: manifest %s" % os.path.relpath(mpath, root).replace("\\", "/"))
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
    seed = json.load(io.open(mpath, encoding="utf-8"))

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
    write_atomic(mpath, json.dumps(seed, indent=1, sort_keys=True))
    print("lane-fold: REFRESHED %d path(s) and updated the manifest, so the fold now "
          "subtracts them as INHERITED." % len(moved))
    return 0


# ──────────────────────────────────── fold ─────────────────────────────────────

def classify(root, wt, seed, settled=()):
    """-> (mine, deleted, inherited, refusals). Pure measurement; writes nothing.

    ⚠⚠ `deleted` EXISTS BECAUSE A LANE'S DELETION USED TO VANISH AT THE FOLD.
    [D-CYCLE-LANE-FOLD-DROPS-A-LANE-S-DELETION] `git status` reports a removed path
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
    the main tree's bytes must still match the seed (or the HEAD blob), or the fold
    REFUSES the whole batch rather than destroying work it cannot account for."""
    mine, deleted, inherited, refusals = [], [], [], []
    settled_paths = []
    for rel in sorted(set(changed_paths(wt))):
        if is_lane_tree(rel):
            continue          # a nested lane tree is never this lane's contribution
        src = os.path.join(wt, rel)
        dest = os.path.join(root, rel)
        if not os.path.isfile(src):
            # The lane removed it (or it never existed). Only a path the MAIN TREE
            # still holds is a deletion this fold has to carry out.
            if not inside(root, dest):
                refusals.append("escapes the repository: %s" % rel)
                continue
            if not os.path.exists(dest):
                continue                      # already absent both sides: nothing to do
            baseline = seed.get(rel)
            if baseline is None:
                blob = subprocess.run(["git", "-C", root, "show", "HEAD:%s" % rel],
                                      capture_output=True)
                if blob.returncode != 0:
                    refusals.append(
                        "lane deleted an UNTRACKED path the main tree holds, so there "
                        "is no baseline to prove it is safe to remove: %s" % rel)
                    continue
                baseline = hashlib.md5(blob.stdout).hexdigest()
            if md5_file(dest) != baseline:
                refusals.append("main tree DRIFTED; refusing to DELETE it: %s" % rel)
                continue
            deleted.append(rel)
            continue
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
            settled_paths.append(rel)
            continue
        if rel in seed and md5_file(src) == seed[rel]:
            inherited.append(rel)
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
            # Baseline for an unseeded path is the HEAD BLOB -- see the module
            # docstring for why "absent from the manifest" does not mean "new".
            blob = subprocess.run(["git", "-C", root, "show", "HEAD:%s" % rel],
                                  capture_output=True)
            tracked = blob.returncode == 0
            if tracked and os.path.isfile(dest):
                if hashlib.md5(blob.stdout).hexdigest() != md5_file(dest):
                    refusals.append("main tree DRIFTED from HEAD (would be lost): %s"
                                    % rel)
                    continue
            elif tracked:
                refusals.append("tracked at HEAD but missing from the main tree: %s" % rel)
                continue
            elif os.path.exists(dest):
                refusals.append("untracked at HEAD yet present in the main tree: %s" % rel)
                continue
        mine.append(rel)
    return mine, deleted, inherited, refusals, settled_paths


def cmd_fold(root, lane, apply_it, settled=()):
    wt = worktree_path(root, lane)
    mpath = manifest_path(root, lane)
    if not os.path.isdir(wt):
        die("no worktree at %s" % wt)
    if not os.path.isfile(mpath):
        die("no seed manifest at %s\n"
            "  run `lane-fold.py seed %s` at the moment the worktree is created -- the "
            "manifest is what separates this lane's work from what it inherited."
            % (mpath, lane))
    seed = json.load(io.open(mpath, encoding="utf-8"))

    mine, deleted, inherited, refusals, settled_paths = classify(root, wt, seed, settled)

    if refusals:
        print("lane-fold: REFUSED -- nothing written. %d problem(s):" % len(refusals))
        for r in refusals:
            print("   " + r)
        print("  A DRIFT refusal is usually TWO LANES ON ONE FILE. Merge the second "
              "lane's declared changes into the main tree by hand, then re-run this "
              "fold: then re-run naming each reconciled path\n"
              "  `--settled <path>` (repeatable), which drops JUST those paths from "
              "this lane's\n  change set so the rest can land. ⚠ A bare re-run will "
              "refuse identically: the\n  drift test compares the DESTINATION against "
              "the SEED, so merging by hand makes\n  the destination differ MORE, not "
              "less. `--settled` is an assertion that YOU have\n  already reconciled "
              "that path; it is not a --force, and nothing is written for it.")
        return 2

    if settled_paths:
        # Never silent: a skipped path is how a lane's work goes missing while the
        # fold reports success.
        print("lane-fold: %d path(s) DECLARED SETTLED BY HAND -- not written, not "
              "compared:" % len(settled_paths))
        for rel in settled_paths:
            print("   %s" % rel)
    print("lane-fold: lane %s -- %d inherited path(s) skipped, %d path(s) are this "
          "lane's:" % (lane, len(inherited), len(mine)))
    for rel in mine:
        # ⚠ THE LABEL ANSWERS "WAS THIS PATH SEEDED?", NOT "IS THIS FILE NEW?". An
        # earlier wording said the second, and three files it marked `(new)` were
        # TRACKED AT HEAD all along -- which briefly made an unchanged test count look
        # like a lane's tests had gone missing.
        print("   %s%s" % (rel, "" if rel in seed
                                else "   (not seeded -- no prior lane touched it)"))
    # ⚠ DELETIONS ARE LISTED SEPARATELY AND LOUDLY. They are the destructive half of
    # a fold, and a reader scanning the copy list would not otherwise see them at all
    # -- which is exactly how [D-CYCLE-LANE-FOLD-DROPS-A-LANE-S-DELETION] stayed
    # invisible: nothing printed, so nothing looked wrong.
    if deleted:
        print("lane-fold: and %d path(s) this lane DELETED:" % len(deleted))
        for rel in deleted:
            print("   %s   (will be REMOVED from the main tree)" % rel)

    if not apply_it:
        print("lane-fold: dry run. pass --apply to write.")
        return 0

    for rel in mine:
        copy_atomic(os.path.join(wt, rel), os.path.join(root, rel))
    for rel in deleted:
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
          % (len(mine), (" and REMOVED %d" % len(deleted)) if deleted else ""))
    return 0


# ──────────────────────────────────── list ─────────────────────────────────────

def cmd_list(root):
    wdir = os.path.join(root, WORKTREES_DIR)
    lanes = sorted(n for n in os.listdir(wdir)
                   if not n.startswith(".") and os.path.isdir(os.path.join(wdir, n))
                   ) if os.path.isdir(wdir) else []
    mdir = os.path.join(root, MANIFEST_DIR)
    manifests = sorted(n[len("seed-"):-len(".json")] for n in os.listdir(mdir)
                       if n.startswith("seed-") and n.endswith(".json")
                       ) if os.path.isdir(mdir) else []
    print("lane-fold: worktrees under %s/: %s"
          % (WORKTREES_DIR, ", ".join(lanes) if lanes else "(none)"))
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
    return 0


# ────────────────────────────────── self-test ──────────────────────────────────

def self_test():
    """Red-on-disable for the instrument, on a REAL temporary repository.

    ⚠ EVERY ARM HERE IS A REFUSAL. A fold that copies the right files is right by
    construction and proves nothing; what has to be exercised is each way a fold can
    destroy work while reporting success. The four that matter:
      (a) an INHERITED path -- seeded, untouched by the lane -- is NOT folded back
          (this is the one that would silently revert a sibling);
      (b) a DRIFTED destination refuses, and refuses the WHOLE fold;
      (c) an unseeded path that the main tree still holds at HEAD folds fine
          ("absent from the manifest" does not mean "new");
      (d) a quoted path -- one containing a SPACE -- survives the status parse.
    """
    failed = [0]

    def pin(ok, why, detail=""):
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", why,
                               ("   " + detail) if detail else ""))
        if not ok:
            failed[0] += 1

    with tempfile.TemporaryDirectory() as tmp:
        root = os.path.realpath(tmp)
        run = lambda *a: subprocess.run(["git", "-C", root] + list(a),
                                        capture_output=True, check=True)
        subprocess.run(["git", "init", "-q", root], capture_output=True, check=True)
        run("config", "user.email", "selftest@example.invalid")
        run("config", "user.name", "lane-fold self-test")
        for rel, body in (("tracked.txt", "base\n"),
                          ("shared.json", "{}\n"),
                          # ⓘ THE SPACE IS THE POINT: git C-quotes this path under
                          # `--porcelain`, and the predecessor's `line[3:]` parse
                          # silently omitted the repository's real equivalent.
                          ("a file - with spaces.md", "base\n")):
            write_atomic(os.path.join(root, rel), body)
        run("add", "-A")
        run("commit", "-q", "-m", "base")

        # The main tree carries one uncommitted edit: that is what gets seeded.
        write_atomic(os.path.join(root, "shared.json"), '{"from":"lane-one"}\n')

        wt = os.path.join(root, WORKTREES_DIR, "x")
        os.makedirs(wt)
        # A worktree is a checkout; the self-test only needs a directory git can read,
        # so make it a repository of its own at the same content.
        subprocess.run(["git", "init", "-q", wt], capture_output=True, check=True)
        for rel in ("tracked.txt", "a file - with spaces.md"):
            copy_atomic(os.path.join(root, rel), os.path.join(wt, rel))
        subprocess.run(["git", "-C", wt, "config", "user.email", "s@e.invalid"],
                       capture_output=True, check=True)
        subprocess.run(["git", "-C", wt, "config", "user.name", "s"],
                       capture_output=True, check=True)
        subprocess.run(["git", "-C", wt, "add", "-A"], capture_output=True, check=True)
        subprocess.run(["git", "-C", wt, "commit", "-q", "-m", "base"],
                       capture_output=True, check=True)

        cmd_seed(root, "x")
        seed = json.load(io.open(manifest_path(root, "x"), encoding="utf-8"))
        pin(list(seed) == ["shared.json"],
            "seed records exactly the main tree's uncommitted paths", "got=%s" % list(seed))

        # (a) the lane leaves the seeded file alone and edits two of its own.
        write_atomic(os.path.join(wt, "tracked.txt"), "lane edit\n")
        write_atomic(os.path.join(wt, "a file - with spaces.md"), "lane edit\n")
        mine, _del, inherited, refusals, _s = classify(root, wt, seed)
        pin(not refusals, "a clean fold refuses nothing", "refusals=%s" % refusals)
        pin(inherited == ["shared.json"],
            "(a) a seeded path the lane never touched is INHERITED, not folded back",
            "inherited=%s" % inherited)
        pin(mine == ["a file - with spaces.md", "tracked.txt"],
            "(c)+(d) unseeded paths fold, and a C-QUOTED path is not lost",
            "mine=%s" % mine)

        # (b) the main tree drifts under the lane.
        write_atomic(os.path.join(root, "tracked.txt"), "someone else\n")
        mine2, _del2, _inh2, refusals2, _s2 = classify(root, wt, seed)
        pin(len(refusals2) == 1 and "DRIFTED from HEAD" in refusals2[0],
            "(b) a destination that drifted from HEAD is REFUSED", "got=%s" % refusals2)
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
        _m3, _d3, _i3, refusals3, _s3 = classify(root, wt, seed)
        pin(any("DRIFTED since seeding" in r and "shared.json" in r for r in refusals3),
            "two lanes on ONE shared document REFUSES rather than reverting the first",
            "got=%s" % refusals3)

        # ── `--settled`, and the arm ORDER is the argument for it ──────────────────
        # (k) The refusal above used to end the story: the message told the caller to
        # merge by hand and re-run, and a bare re-run REFUSES IDENTICALLY because the
        # drift test compares the DESTINATION to the SEED -- a hand-merge moves the
        # destination FURTHER from the seed, never back. ✔MEASURED 2026-09-02 (P54),
        # twice in one cycle. So the promised remedy is pinned here as a real one.
        _m4, _d4, _i4, refusals4, settled4 = classify(
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
        seed_before = json.load(io.open(manifest_path(root, "x"), encoding="utf-8"))
        cmd_refresh_plans(root, "x", apply_it=True)
        seed_after = json.load(io.open(manifest_path(root, "x"), encoding="utf-8"))
        pin(io.open(os.path.join(wt, ".plans", "reg.md"), encoding="utf-8").read()
            == "row A\nrow B\n",
            "(m) refresh-plans carries a mid-cycle registry edit into a LIVE lane")
        pin(seed_after.get(".plans/reg.md") != seed_before.get(".plans/reg.md")
            and seed_after.get(".plans/reg.md") is not None,
            "(m2) ...and UPDATES the manifest, so the fold subtracts it as inherited",
            "before=%r after=%r" % (seed_before.get(".plans/reg.md"),
                                    seed_after.get(".plans/reg.md")))
        _m5, _d5, inh5, _r5, _s5 = classify(
            root, wt, json.load(io.open(manifest_path(root, "x"), encoding="utf-8")))
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
        empty_manifest = json.load(io.open(manifest_path(root, "x"),
                                          encoding="utf-8"))
        pin(empty_manifest == {}
            and md5_file(os.path.join(wt, "tracked.txt")) == before,
            "(f) --empty writes an EMPTY manifest, copies nothing, and does NOT "
            "trip (e) -- the guard is on the copy, not on the verb",
            "manifest=%r" % empty_manifest)

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
        # holding them were new and therefore untracked --
        # D-CYCLE-LANE-FOLD-WALKS-AN-UNTRACKED-DIRECTORY-PAST-GITIGNORE.
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
        # [D-CYCLE-LANE-FOLD-DROPS-A-LANE-S-DELETION] Before this, `classify` hit
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
            subprocess.run(["git", "-C", repo, "add", "todelete.txt"],
                           capture_output=True, check=True)
            subprocess.run(["git", "-C", repo, "commit", "-q", "-m", "add todelete"],
                           capture_output=True, check=True)
        os.remove(os.path.join(wt, "todelete.txt"))
        m_del, d_del, _i, r_del, _s4 = classify(root, wt, {})
        pin("todelete.txt" in d_del and "todelete.txt" not in m_del
            and not [x for x in r_del if "todelete.txt" in x],
            "(h) a path the lane DELETED is measured as a deletion, not dropped",
            "deleted=%s mine=%s refusals=%s" % (d_del, m_del, r_del))

        write_atomic(os.path.join(root, "todelete.txt"), "someone else edited this\n")
        _m, d_drift, _i2, r_drift, _s5 = classify(root, wt, {})
        pin("todelete.txt" not in d_drift
            and any("refusing to DELETE" in x and "todelete.txt" in x for x in r_drift),
            "(h) a deletion whose destination DRIFTED is REFUSED, not carried out",
            "deleted=%s refusals=%s" % (d_drift, r_drift))

        # (i) THE ROOT IS THIS SCRIPT'S OWN TREE, NOT THE CALLER'S CWD.
        # [[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]]
        #
        # ⚠⚠ THE ARM SETS A DIFFERENT CWD DELIBERATELY, AND THAT IS THE WHOLE REASON
        # IT EXISTS. Everything above runs with the cwd wherever the caller left it
        # and never notices, because a wrong root and a right one look identical
        # while they agree. `root` here is a REAL repository -- `git init` above --
        # so a cwd-keyed `repo_root()` succeeds and answers about IT, which is the
        # quiet direction this row is about.
        #
        # ★ BOTH HALVES ARE ASSERTED. The positive (it names the tree holding this
        # file) and the negative (it does NOT name the fixture) -- a positive-only
        # pin would pass on a resolver that somehow reached both, and the negative
        # alone would pass on one that returned nothing.
        # ⓘ `os.chdir` is restored in a `finally`: the arms above measured with the
        # original cwd and the temp directory is about to be deleted, so leaving the
        # process inside it would break the cleanup on Windows.
        mine_tree = os.path.realpath(
            os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", ".."))
        was = os.getcwd()
        try:
            os.chdir(root)
            got = repo_root()
        finally:
            os.chdir(was)
        pin(os.path.realpath(got) == mine_tree
            and os.path.realpath(got) != os.path.realpath(root),
            "(i) driven from ANOTHER repository's cwd, repo_root() still names the "
            "tree THIS FILE lives in",
            "got=%s wanted=%s cwd-was=%s" % (got, mine_tree, root))

        # (j) `--repo <path>` IS THE EXPLICIT ESCAPE HATCH, and the CONTROL for (i).
        # ⚠ Without it, (i) passes over a `repo_root` that had simply stopped being
        # able to reach any tree but its own -- the capability the old cwd-keying
        # provided BY ACCIDENT would have been removed rather than named, and
        # nothing would have measured the difference.
        pin(os.path.realpath(repo_root(root)) == os.path.realpath(root),
            "(j) CONTROL: --repo <path> still reaches another tree, deliberately",
            "got=%s wanted=%s" % (repo_root(root), root))

    print("lane-fold self-test: %d failed" % failed[0])
    return 1 if failed[0] else 0


# ──────────────────────────────────── main ─────────────────────────────────────

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
    # while the `.sh` twin accepted the same spelling -- two halves of one tool
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
    if verb not in ("seed", "fold", "refresh-plans"):
        die("unknown verb %r -- expected seed, fold, refresh-plans or list." % verb, 3)
    if not rest or rest[0].startswith("-"):
        die("verb %s needs a lane name." % verb, 3)
    lane = rest[0]
    if os.sep in lane or "/" in lane or lane.startswith("."):
        die("lane name %r is not a bare directory name." % lane, 3)
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
