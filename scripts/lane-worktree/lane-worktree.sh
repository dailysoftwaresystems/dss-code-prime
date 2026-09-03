#!/usr/bin/env bash
# PURPOSE: create and remove lane worktrees inside the ignored .worktrees/, refusing any root that would exceed Windows MAX_PATH.
#
# ★★★ THE OPERATOR RULING THIS OWNS (2026-08-26):
#   "I want the worktrees implementation to be inside the project root, .worktrees
#    directory (where 100% of it's internal content ignored by .gitignore). This
#    way we stop contaminating builds outside repository bounds."
#   ... and, the same day, as an absolute:
#   "worktrees MUST be ignored by ALL host copies to run legs"
#
# Before this, lanes took worktrees at short absolute roots outside the repository
# (C:/dssp40k, C:/dssp40l, ...). That kept the tree clean but scattered full
# checkouts -- each with its own build/ -- across the filesystem, where nothing
# owned them, no guard could see them, and `git worktree list` was the only record
# that they existed at all. Inside the root they are enumerable and removable.
#
# ★ ONE OWNER, the same shape as scripts/leg-tree/. Hand-rolling `git worktree add`
#   in a lane is how the location rule erodes: the rule is only as good as the last
#   person who remembered it, and this repository has already measured that a rule
#   living only in a document "has no teeth at the moment of the decision".
#
# ⚠⚠ THE MAX_PATH PREFLIGHT IS THE LOAD-BEARING HALF, AND IT IS NOT DEFENSIVE
#    PROGRAMMING -- IT IS A REGRESSION GUARD FOR AN ANCHORED DEFECT.
#    `D-CYCLE-WORKTREE-UNDER-THE-SESSION-SCRATCH-PATH-CANNOT-BE-BUILT-ON-WINDOWS`
#    (cycle P29): a worktree under the ~150-char session scratch directory cannot
#    be BUILT on Windows, because the generated `.obj.d` dependency paths exceed
#    MAX_PATH. ★ The dangerous part is the failure MODE: not a link error at the
#    end, but a per-TU compile error in files the lane never touched
#    ("fatal error: opening dependency file tests\core\CMakeFiles\...obj.d"), so it
#    reads as somebody else's breakage and sends the lane into an unrelated
#    subsystem. Moving worktrees from a 10-char root into the repository root
#    SPENDS 46 characters of that budget, so the budget stops being slack nobody
#    tracks and becomes a number this script checks before it hands back a path.
#
# ✔MEASURED 2026-08-26, inside a live lane worktree (C:/dssp40l/build/dbg):
#     longest build-relative suffix = 163 chars
#       /build/dbg/tests/analysis/preprocess/CMakeFiles/
#       dss_analysis_preprocess_test_include_bare_relative_includer_dir.dir/
#       test_include_bare_relative_includer_dir.cpp.obj
#     C:/dssp40k                                 root=10 -> 173  (87 spare)
#     <repo>/.worktrees/lane-k                   root=56 -> 219  (41 spare)
#     <repo>/.worktrees/k                        root=51 -> 214  (46 spare)
#   Both fit. The session-scratch root that produced the anchor does NOT, and this
#   script now refuses it by arithmetic instead of discovering it by cryptic red.
#
# Exit codes: 0 OK - 2 not a repository / git refused - 3 MAX_PATH would be
#             breached - 4 .worktrees/ is not ignored - 5 usage.
set -uo pipefail

MAX_PATH=260
# The longest build-relative suffix a worktree is expected to generate. Measured,
# not guessed (see the header). Raise it by MEASURING, never to make a red go away.
WORST_SUFFIX=163
# Refuse to hand back a root that only just fits: this repository's test names grow,
# and the suffix above is dominated by one. A margin is what keeps the next long
# test name from re-opening the anchored defect.
MARGIN=20

_say()  { printf 'lane-worktree: %s\n' "$*"; }
_die()  { code=$1; shift; for l in "$@"; do printf 'lane-worktree: %s\n' "$l" >&2; done; exit "$code"; }

# ⚠⚠ ANCHORED ON THIS FILE'S OWN LOCATION, RESOLVED BEFORE ANYTHING CAN `cd`.
# [[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]] -- see `_repo_root` below.
_LW_HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd -P)" \
  || _die 2 "cannot resolve this script's own directory."

# ★★ THE `.sh` OWNER OF "WHICH TREE CONTAINS THIS PATH?", REUSED RATHER THAN RESPELT.
# ⚠ THE EMPTY ARGUMENT IS LOAD-BEARING: `.` forwards the CALLER's positional
# parameters, so a plain `. leg-tree.sh` while `$1` is `add` reaches leg-tree.sh's
# bottom dispatch as an unknown subcommand and exits 4. ✔MEASURED 2026-09-02, both
# spellings. Five other scripts in this repository source it exactly this way.
# shellcheck source=../leg-tree/leg-tree.sh
. "$_LW_HERE/../leg-tree/leg-tree.sh" "" \
  || _die 2 "cannot load scripts/leg-tree/leg-tree.sh"

# Set by the `--repo <path>` pre-pass in the dispatch at the bottom of this file.
LW_REPO_OVERRIDE=""

# ★★★ WHICH TREE THIS VERB IS ABOUT, AND THE ANSWER IS NOT "WHERE AM I STANDING".
# [[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]]
#
# This was a bare `git rev-parse --show-toplevel`, which answers "what repository is
# my CALLER'S SHELL in?" -- so `abs`, the scratchpad gate's `pad`, and the `rm -rf`
# target were all rooted at whichever repository somebody happened to have cd'd into.
# ✔MEASURED 2026-09-02, driving this file out of `.worktrees/lw` from a throwaway
# repository outside the checkout: `list` reported the THROWAWAY repository's
# `.worktrees/`. ✔MEASURED in P52 for real: a run whose fixtures were seeded in the
# WSL leg clone was answered about the driver clone, and five of nine assertions
# reported a gate that had not fired -- a true answer about a tree nobody asked about.
#
# ★ THE QUESTION IS "WHICH TREE DOES MY OWN FILE BELONG TO?", and the two rejected
# answers are worth naming because each is defensible until it is measured:
#
#   * `$PWD`'s tree (what this did) -- a property of the caller's shell, not of the
#     verb. Refuted above.
#   * THE MAIN CHECKOUT -- "only the primary worktree owns `.worktrees/`", reached by
#     `--git-common-dir` or `git worktree list`. ⛔ REFUTED, and in the dangerous
#     direction. ✔MEASURED 2026-09-02: from `.worktrees/lw`, that answer resolves
#     `remove io` to `<main>/.worktrees/io` -- A LIVE SIBLING LANE'S UNCOMMITTED WORK
#     -- and would delete it, while this answer resolves it to
#     `.worktrees/lw/.worktrees/io`, which does not exist, and refuses. A resolver
#     whose mistake reaches ANOTHER tree is the failure direction this row exists to
#     close. ✔MEASURED the same day: nested worktrees are ordinary git (a linked
#     worktree adds one under itself and it works), so "only the main checkout owns
#     `.worktrees/`" is this repository's CONVENTION, not a fact about git -- and a
#     convention belongs in who runs the verb, not welded into its resolver.
#     ⓘ And it is the answer that would have re-created the P52 mismatch in the other
#     direction: `test-lane-worktree.sh` seeds its fixtures at ITS OWN
#     `$_here/../..`, so a script resolving to the main checkout would once again
#     look somewhere the test did not write.
#
# ⇒ the anchor is `_LW_HERE`, this file's own directory, and the tree is derived from
# it by `leg_tree_owning_root`. `--repo <path>` stays as the explicit way to mean
# another tree, so the CAPABILITY survives while the DEFAULT stops being an accident.
_repo_root() {
  if [ -n "$LW_REPO_OVERRIDE" ]; then
    _lw_r="$(cd "$LW_REPO_OVERRIDE" 2>/dev/null && pwd -P)" \
      || _die 2 "--repo '$LW_REPO_OVERRIDE': no such directory."
    leg_tree_owning_root "$_lw_r" \
      || _die 2 "--repo '$LW_REPO_OVERRIDE' is not inside a git working tree."
    return 0
  fi
  leg_tree_owning_root "$_LW_HERE" \
    || _die 2 "not inside a git repository -- cannot place a lane worktree." \
              "This script resolves the tree IT LIVES IN ($_LW_HERE), never the caller's" \
              "cwd; pass --repo <path> to name a different tree deliberately."
}

# `.worktrees/` must be IGNORED, and that is checked rather than assumed: it is the
# single rule that keeps N full checkouts off every gate host, because the carriages
# derive their exclude list from git (scripts/carriage-excludes/).
# ⚠ The trailing slash is required -- `git check-ignore .worktrees` answers
#   NOT-IGNORED for a directory that does not exist yet, while `.worktrees/` answers
#   correctly. ✔MEASURED 2026-08-26, both spellings, absent directory.
_assert_ignored() {
  git -C "$1" check-ignore -q -- ".worktrees/" || _die 4 \
    ".worktrees/ is NOT ignored by git." \
    "A lane worktree there would be committed, and -- worse -- would ride the" \
    "carriage to every gate host, where the examples runner globs examples/<lang>/*" \
    "and would run somebody's uncommitted corpus as if it were the cycle's." \
    "Restore the '/.worktrees/' rule in .gitignore before creating any worktree."
}

_assert_path_budget() {  # <candidate-root>
  root="$1"; len=${#root}; total=$(( len + WORST_SUFFIX ))
  if [ "$(( total + MARGIN ))" -gt "$MAX_PATH" ]; then
    _die 3 \
      "REFUSING: '$root' is $len chars; + $WORST_SUFFIX for the longest build path" \
      "= $total, leaving $(( MAX_PATH - total )) under MAX_PATH ($MAX_PATH), below the" \
      "required margin of $MARGIN." \
      "This is D-CYCLE-WORKTREE-UNDER-THE-SESSION-SCRATCH-PATH-CANNOT-BE-BUILT-ON-WINDOWS." \
      "It would NOT fail as a link error -- it fails as a per-TU compile error in files" \
      "you never touched, and reads as somebody else's breakage. Use a shorter lane name."
  fi
  _say "path budget OK: root=$len + suffix=$WORST_SUFFIX = $total ($(( MAX_PATH - total )) spare)"
}

cmd_add() {  # <name> [committish]
  name="${1:-}"; [ -n "$name" ] || _die 5 "usage: lane-worktree.sh add <name> [committish]"
  case "$name" in
    */*|.*) _die 5 "lane name must be a single path component and must not start with '.': '$name'" ;;
  esac
  repo="$(_repo_root)"; _assert_ignored "$repo"
  rel=".worktrees/$name"; abs="$repo/$rel"
  _assert_path_budget "$abs"
  [ -e "$abs" ] && _die 5 "'$rel' already exists -- remove it first, or pick another name."
  at="${2:-HEAD}"
  git -C "$repo" worktree add --detach "$rel" "$at" >/dev/null \
    || _die 2 "git worktree add failed for '$rel' at '$at'."

  # ⚠⚠ RESET THIS LANE NAME'S SEED MANIFEST, AND IT IS A CORRECTNESS FIX RATHER THAN
  #    TIDINESS. `scripts/lane-fold/lane-fold.py` adjudicates a fold as
  #      (the lane's `git status` set) MINUS (seeded paths whose md5 is UNCHANGED),
  #    reading `.worktrees/.manifests/seed-<name>.json`. That file is keyed by LANE
  #    NAME ONLY -- it carries no cycle and no commit -- and lane names here are two
  #    letters, so they are reused constantly.
  #    ✔MEASURED 2026-09-03 (cycle P57): four worktrees were created with this verb on
  #    a clean tree at `fcb3a9d7`, and ALL FOUR silently inherited manifests written on
  #    Sep 1-2 by earlier lanes of the same name. `seed-ld.json` held 82 entries of
  #    which 37 disagreed with the main tree -- including `CMakeLists.txt`,
  #    `src/asm/asm.cpp` and `lane-fold.py` itself, files no lane this cycle touched.
  #    The fold then REFUSED `src/lir/lowering/mir_to_lir.cpp` as "main tree DRIFTED
  #    since seeding" against a tree that was byte-identical to HEAD.
  #    ★ THE FALSE REFUSAL IS THE CHEAP FAILURE. The expensive one is the other
  #    direction: a stale entry that HAPPENS to equal the lane's own file marks real
  #    lane work as "untouched seed" and the fold SILENTLY DROPS IT -- a lane's whole
  #    change vanishing while every report reads clean, which is the class this
  #    project treats as worst.
  #    ⇒ A worktree created here is a checkout of a COMMIT and carries no uncommitted
  #    work, so its honest manifest is EMPTY: `lane-fold` measures an absent path
  #    against the HEAD BLOB, which is exactly the right question for such a lane. An
  #    orchestrator that then seeds uncommitted work in re-writes it via
  #    `lane-fold.py seed <lane>`, which is the only other writer.
  mkdir -p "$repo/.worktrees/.manifests" 2>/dev/null || :
  printf '{}' > "$repo/.worktrees/.manifests/seed-$name.json" \
    || _die 2 "could not reset the seed manifest for '$name'."

  _say "created $rel at $(git -C "$abs" rev-parse --short HEAD)"
  _say "seed manifest reset to empty (this lane starts from the commit, not from uncommitted work)"
  _say "build into $rel/build/<lane> -- never into the main tree's build/."
  printf '%s\n' "$abs"
}

# ⚠⚠ A LANE'S SCRATCHPAD IS ITS EVIDENCE, AND THIS VERB USED TO DELETE IT WITHOUT
#    ASKING. [[D-CYCLE-LANE-WORKTREE-REMOVE-DISCARDS-AN-UNPRESERVED-SCRATCHPAD]]
#    ✔MEASURED 2026-09-01 (cycle P50): the orchestrator ran
#      cp -r .worktrees/t2/scratchpad/p50/t2 scratchpad/p50/t2 && echo preserved
#      bash scripts/lane-worktree/lane-worktree.sh remove t2
#    on ONE command line. `scratchpad/p50/` did not exist in the main tree, so `cp`
#    failed, `&& echo` printed nothing, and the ABSENCE of output read as "fine" --
#    then the very next command destroyed the only copy. Lane `t2`'s 14 result JSONs
#    and its md5 ledger are gone, and the row that cites them had to be amended to
#    admit it. ★ THE PRESERVE STEP WAS A CONVENTION LIVING IN THE ORCHESTRATOR'S
#    HEAD, and this repository has already measured that a rule with no teeth at the
#    moment of the decision is a rule that erodes. ⇒ the tool now owns it: a lane
#    whose scratchpad holds files cannot be removed silently. `--preserve-to <dir>`
#    makes the COPY this script's job so it cannot fail quietly (it copies, counts
#    both sides, and REFUSES on any mismatch); `--discard-scratchpad` is the explicit
#    "I do not want it", which is a decision rather than an accident.
cmd_remove() {  # <name> [--preserve-to <dir> | --discard-scratchpad]
  name="${1:-}"; [ -n "$name" ] || _die 5 "usage: lane-worktree.sh remove <name> [--preserve-to <dir> | --discard-scratchpad]"
  shift || true
  preserve_to=""; discard=0
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --preserve-to) preserve_to="${2:-}"; [ -n "$preserve_to" ] || _die 5 "--preserve-to needs a directory"; shift 2 ;;
      --discard-scratchpad) discard=1; shift ;;
      *) _die 5 "unknown option '$1' (expected --preserve-to <dir> or --discard-scratchpad)" ;;
    esac
  done
  [ "$discard" -eq 1 ] && [ -n "$preserve_to" ] \
    && _die 5 "--preserve-to and --discard-scratchpad contradict each other; pick one."
  # ⚠⚠ THE SAME NAME VALIDATION `cmd_add` PERFORMS, AND IT MATTERS MORE HERE, because
  # this verb now DELETES a directory tree. `cmd_add` refused a name with a slash or a
  # leading dot and `cmd_remove` did not -- harmless while the only verb was
  # `git worktree remove`, which simply declines an unknown path, and NOT harmless the
  # moment an `rm -rf` stands behind it. `remove ../..` must never resolve anywhere.
  case "$name" in
    */*|.*) _die 5 "lane name must be a single path component and must not start with '.': '$name'" ;;
  esac
  repo="$(_repo_root)"; rel=".worktrees/$name"; abs="$repo/$rel"

  # ── THE SCRATCHPAD GATE, BEFORE ANY DELETION ────────────────────────────────
  # Counted with `find -type f`, so an empty directory tree is correctly "nothing to
  # preserve" and a single file is enough to stop the removal.
  pad="$abs/scratchpad"
  pad_files=0
  if [ -d "$pad" ]; then
    pad_files="$(find "$pad" -type f 2>/dev/null | wc -l | tr -d ' ')"
  fi
  if [ "$pad_files" -gt 0 ] && [ "$discard" -eq 0 ] && [ -z "$preserve_to" ]; then
    _die 7 "'$rel' holds a scratchpad with $pad_files file(s) and would be DELETED with it." \
           "A lane's scratchpad is its EVIDENCE -- probes, transcripts, mutant logs, the" \
           "artefacts its registry row cites. Choose explicitly:" \
           "  --preserve-to <dir>      copy it there FIRST; this script verifies the copy" \
           "  --discard-scratchpad     delete it deliberately" \
           "This gate exists because a hand-rolled 'cp && remove' one-liner lost lane t2's" \
           "entire evidence tree in P50 when the destination's parent did not exist."
  fi
  if [ "$pad_files" -gt 0 ] && [ -n "$preserve_to" ]; then
    mkdir -p "$preserve_to" || _die 7 "could not create '$preserve_to'"
    # `cp -R <src>/. <dst>/` copies the CONTENTS, so an existing destination is filled
    # rather than nested one level deeper -- the shape a re-run needs.
    cp -R "$pad/." "$preserve_to/" || _die 7 "copy of '$pad' -> '$preserve_to' FAILED; nothing was removed."
    got="$(find "$preserve_to" -type f 2>/dev/null | wc -l | tr -d ' ')"
    # ⚠ VERIFY, DO NOT ASSUME. The whole defect this gate closes was a copy that
    # failed while the caller read silence as success.
    [ "$got" -ge "$pad_files" ] \
      || _die 7 "preserve VERIFY FAILED: $pad_files file(s) under '$pad' but only $got under" \
                "'$preserve_to'. REFUSING to remove '$rel' -- the evidence would be lost."
    _say "preserved $pad_files scratchpad file(s) -> $preserve_to (verified $got present)"
  elif [ "$pad_files" -gt 0 ]; then
    _say "DISCARDING $pad_files scratchpad file(s) under $rel, as instructed"
  fi

  # --force because a lane worktree always carries an ignored build/ tree; without
  # it git refuses and the caller is tempted to `rm -rf`, which leaves the
  # registration behind in .git/worktrees/ where `git status` never shows it.
  git -C "$repo" worktree remove --force "$rel" 2>/dev/null \
    || _say "worktree remove declined for '$rel' (already gone?) -- pruning anyway"
  git -C "$repo" worktree prune
  # ⚠⚠ THE GIT VERB CAN DECLINE AND LEAVE THE ENTIRE TREE ON DISK, AND THIS FUNCTION
  # USED TO REPORT SUCCESS ANYWAY. ✔MEASURED 2026-08-31 (cycle P46): lane `cm` was
  # folded mid-flight and its `.git` emptied, so `git worktree remove` could not see it
  # and exited non-zero; control fell through to `prune` and printed
  # "removed .worktrees/cm and pruned stale registrations" over **4.4 GB that was still
  # there**. ★ An instrument reporting a pass over work it did not do is this project's
  # worst class -- and the failure is invisible, because the caller's next `git worktree
  # list` agrees the worktree is gone. ⇒ REMOVE, THEN VERIFY, THEN SPEAK.
  if [ -e "$abs" ]; then
    # Belt and braces over the name check: resolve both sides and require the target to
    # sit STRICTLY inside the repository's .worktrees/. A symlink is the one way a
    # single-component name could still land elsewhere.
    real="$(cd "$abs" 2>/dev/null && pwd -P)" || real=""
    container="$(cd "$repo/.worktrees" 2>/dev/null && pwd -P)" || container=""
    if [ -z "$real" ] || [ -z "$container" ]; then
      _die 2 "refusing to delete '$abs': could not resolve it or its container."
    fi
    case "$real" in
      "$container"/?*) rm -rf "$abs" ;;
      *) _die 2 "refusing to delete '$abs': it resolves to '$real', which is not" \
                "strictly inside '$container'." ;;
    esac
  fi
  if [ -e "$abs" ]; then
    _die 6 "'$rel' is STILL ON DISK after worktree-remove, prune and rm -rf." \
           "REFUSING to report success over work that did not happen." \
           "A locked file is the likely cause -- a stalled ctest holding libdsscp.dll" \
           "is this repository's known instance. Close it and re-run."
  fi
  git -C "$repo" worktree prune
  # Drop the container only when WE emptied it; never disturb a sibling lane's.
  rmdir "$repo/.worktrees" 2>/dev/null && _say "removed the now-empty .worktrees/"
  _say "removed $rel (VERIFIED absent) and pruned stale registrations"
}

cmd_list() {
  repo="$(_repo_root)"
  _say "registered worktrees:"
  git -C "$repo" worktree list | sed 's/^/  /'
  if [ -d "$repo/.worktrees" ]; then
    _say "under .worktrees/:"
    for d in "$repo"/.worktrees/*/; do
      [ -d "$d" ] || continue
      p="${d%/}"; printf '  %-50s %s files, %s spare under MAX_PATH\n' \
        "${p#"$repo/"}" "$(find "$p" -type f 2>/dev/null | wc -l)" \
        "$(( MAX_PATH - ${#p} - WORST_SUFFIX ))"
    done
  else
    _say "under .worktrees/: (absent -- no lane worktrees)"
  fi
}

# ── `--repo <path>` PRE-PASS ────────────────────────────────────────────────────
# Extracted from ANYWHERE in the argument list, before the verb dispatch, so each
# verb keeps the argument grammar it already had -- `cmd_remove`'s option loop in
# particular still refuses every option it does not know.
# ⓘ THIS IS THE CAPABILITY THE OLD BEHAVIOUR PROVIDED BY ACCIDENT. Driving the verb
# at another tree used to be done by cd'ing there and hoping; it is now said out
# loud, which is the difference between a decision and a side effect.
_lw_args=()
while [ "$#" -gt 0 ]; do
  case "$1" in
    --repo)
      [ "$#" -ge 2 ] || _die 5 "--repo needs a directory"
      LW_REPO_OVERRIDE="$2"; shift 2 ;;
    --repo=*)
      LW_REPO_OVERRIDE="${1#--repo=}"
      [ -n "$LW_REPO_OVERRIDE" ] || _die 5 "--repo needs a directory"
      shift ;;
    *) _lw_args+=("$1"); shift ;;
  esac
done
set -- ${_lw_args+"${_lw_args[@]}"}

case "${1:-}" in
  add)    shift; cmd_add    "$@" ;;
  remove) shift; cmd_remove "$@" ;;
  list)   shift; cmd_list   "$@" ;;
  *) _die 5 "usage: lane-worktree.sh [--repo <path>]" \
            "                        {add <name> [committish] |" \
            "                         remove <name> [--preserve-to <dir> | --discard-scratchpad] |" \
            "                         list}" \
            "" \
            "The tree acted on defaults to the one THIS SCRIPT LIVES IN, never the" \
            "caller's cwd. --repo <path> names another tree deliberately." ;;
esac
