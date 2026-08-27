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

_repo_root() {
  git rev-parse --show-toplevel 2>/dev/null \
    || _die 2 "not inside a git repository -- cannot place a lane worktree."
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
  _say "created $rel at $(git -C "$abs" rev-parse --short HEAD)"
  _say "build into $rel/build/<lane> -- never into the main tree's build/."
  printf '%s\n' "$abs"
}

cmd_remove() {  # <name>
  name="${1:-}"; [ -n "$name" ] || _die 5 "usage: lane-worktree.sh remove <name>"
  repo="$(_repo_root)"; rel=".worktrees/$name"
  # --force because a lane worktree always carries an ignored build/ tree; without
  # it git refuses and the caller is tempted to `rm -rf`, which leaves the
  # registration behind in .git/worktrees/ where `git status` never shows it.
  git -C "$repo" worktree remove --force "$rel" 2>/dev/null \
    || _say "worktree remove declined for '$rel' (already gone?) -- pruning anyway"
  git -C "$repo" worktree prune
  # Drop the container only when WE emptied it; never disturb a sibling lane's.
  rmdir "$repo/.worktrees" 2>/dev/null && _say "removed the now-empty .worktrees/"
  _say "removed $rel and pruned stale registrations"
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

case "${1:-}" in
  add)    shift; cmd_add    "$@" ;;
  remove) shift; cmd_remove "$@" ;;
  list)   shift; cmd_list   "$@" ;;
  *) _die 5 "usage: lane-worktree.sh {add <name> [committish] | remove <name> | list}" ;;
esac
