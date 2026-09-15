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
# Exit codes: 0 OK - 2 not a repository / git refused / a path that cannot be
#             resolved - 3 MAX_PATH would be breached - 4 .worktrees/ is not
#             ignored - 5 usage - 6 the worktree is STILL ON DISK after remove,
#             prune and rm -rf - 7 EVIDENCE WOULD BE LOST: an evidence root
#             (scratchpad/ or .temp/) holds files and no decision was given, or a
#             preserve could not be proved -- a destination inside the worktree,
#             one already holding a same-named file with other bytes, a failed
#             copy, or a file that does not re-read identical (see `cmd_remove`) -
#             8 THE LANE'S WORK WOULD BE LOST: the worktree's own git status lists a
#             tracked modification or an untracked file that is not ignored, or its HEAD
#             holds a commit that no ref of the repository reaches -- or either cannot be
#             read -- and --discard-work was not given.
# ⚠ 6 and 7 were implemented and NOT listed here for two cycles; a reader of this
#   header learned nothing about the very refusal `cmd_remove` leads with. The
#   `.ps1` twin's header carries the same list, and the two must not drift again.
set -uo pipefail

MAX_PATH=260
# The longest build-relative suffix a worktree is expected to generate. Measured,
# not guessed (see the header). Raise it by MEASURING, never to make a red go away.
WORST_SUFFIX=163
# Refuse to hand back a root that only just fits: this repository's test names grow,
# and the suffix above is dominated by one. A margin is what keeps the next long
# test name from re-opening the anchored defect.
MARGIN=20

# The top-level directories a lane keeps its EVIDENCE in. Both are ignored by git, so a
# fold never carries them into the main tree, and both are deleted with the worktree.
# `cmd_remove` holds the measurement that made this a list of roots rather than one
# directory, and the `.ps1` twin declares the same two.
EVIDENCE_ROOTS="scratchpad .temp"

_say()  { printf 'lane-worktree: %s\n' "$*"; }
_die()  { code=$1; shift; for l in "$@"; do printf 'lane-worktree: %s\n' "$l" >&2; done; exit "$code"; }
# `git rev-list --oneline` lines -> one prefixed, indented line each: the first 10, then a count
# of the rest. The `.ps1` twin's `Format-CommitList` prints the same lines.
_lw_list_commits() {  # <lines>
  printf '%s\n' "$1" | head -10 | sed 's/^/lane-worktree:   /'
  _lc_n="$(printf '%s\n' "$1" | wc -l | tr -d ' ')"
  if [ "$_lc_n" -gt 10 ]; then printf 'lane-worktree:   ... and %s more\n' "$(( _lc_n - 10 ))"; fi
}

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
  #    work, so its honest manifest holds NO paths. An orchestrator that then seeds
  #    uncommitted work in re-writes it via `lane-fold.py seed <lane>`, which is the
  #    only other writer.
  #
  # ⚠⚠ AND THE MANIFEST RECORDS THE COMMIT THE LANE WAS CREATED AT (lane-fold's manifest
  #    format 2). A fold measures every path the lane did not seed against the blob at
  #    the LANE'S OWN base; it used to read the MAIN tree's HEAD at fold time instead.
  #    ✔REPRODUCED 2026-09-15 (P66) in a throwaway repository: two lanes created at one
  #    commit edited one file and one of them deleted another; once the first lane's fold
  #    was COMMITTED, the second fold exited 0 having overwritten the first lane's edit and
  #    DELETED its other edit -- because the main tree's bytes equalled its new HEAD again.
  #    ★ The worktree's own HEAD is that base only while nothing commits inside the lane,
  #    so the base is WRITTEN DOWN here, where it is known exactly, and the fold refuses a
  #    lane whose HEAD has moved away from it.
  #    ⓘ Compact JSON, keys in sorted order; the `.ps1` twin writes the identical string.
  base="$(git -C "$abs" rev-parse --verify HEAD 2>/dev/null)" \
    || _die 2 "could not read the new worktree's HEAD, so '$name' has no base commit to record."
  mkdir -p "$repo/.worktrees/.manifests" 2>/dev/null || :
  printf '{"base":"%s","format":2,"paths":{}}' "$base" > "$repo/.worktrees/.manifests/seed-$name.json" \
    || _die 2 "could not reset the seed manifest for '$name'."

  _say "created $rel at $(git -C "$abs" rev-parse --short HEAD)"
  _say "seed manifest reset: base recorded, no seeded paths (this lane starts from the commit, not from uncommitted work)"
  _say "build into $rel/build/$name -- never into the main tree's build/."
  # ⚠ THE FIRST BUILD OF A FRESH TREE NEEDS --build-type, AND THIS LINE EXISTS
  #    BECAUSE THE ORCHESTRATOR KEPT WRITING BRIEFS THAT OMITTED IT.
  #    `local-build.sh` maps only `dbg` -> Debug and `rel` -> Release, and REFUSES
  #    (rc 3) to guess one for any other tree name. That refusal is CORRECT and is
  #    not to be softened: guessing Debug for an unknown lane would hand a
  #    Release-intending caller a Debug tree silently, which is the
  #    fails-to-WRONG-ANSWER direction. ✔MEASURED (P63): a lane hit the rc 3, and
  #    the brief that sent it there had never been run by its author --
  #    [[D-CYCLE-BRIEF-STATED-AN-INVOCATION-ITS-AUTHOR-HAD-NEVER-RUN]], whose class
  #    this repository has now paid for more than once.
  #    ⇒ The command belongs where the lane name is KNOWN, which is here. A
  #    convention that lives in the orchestrator's head is a convention that
  #    erodes -- the same argument this file already makes about its own
  #    evidence-preserve step, one verb below.
  #    ⓘ `--configure` is NOT needed: an absent build.ninja enters the configure
  #    block on its own. Naming it would teach a redundant flag.
  _say "FIRST build of this tree:  DSS_JOBS=6 bash scripts/local-build/local-build.sh --tree $name --build-type Debug"
  _say "        every build after:  DSS_JOBS=6 bash scripts/local-build/local-build.sh --tree $name"
  printf '%s\n' "$abs"
}

# _lw_preserve <abs-worktree> <rel> <destination> <files-counted>
# Copies every file under every evidence root to <destination>/<root>/<same path>, and
# REFUSES (exit 7) on anything that would lose evidence. It deletes nothing, ever.
_lw_preserve() {
  _p_wt="$1"; _p_rel="$2"; _p_dest="$3"; _p_counted="$4"
  for _p_t in find xargs cksum sort comm cut cmp cp mkdir mktemp wc head tr; do
    command -v "$_p_t" >/dev/null 2>&1 \
      || _die 7 "cannot prove a preserve: '$_p_t' is not on PATH. Nothing was copied or removed."
  done
  mkdir -p "$_p_dest" 2>/dev/null \
    || _die 7 "could not create '$_p_dest'; nothing was removed."
  _p_dest_real="$(cd "$_p_dest" 2>/dev/null && pwd -P)" \
    || _die 7 "could not resolve '$_p_dest'; nothing was removed."
  _p_wt_real="$(cd "$_p_wt" 2>/dev/null && pwd -P)" \
    || _die 7 "could not resolve '$_p_wt'; nothing was removed."
  # ⚠ A DESTINATION INSIDE THE TREE ABOUT TO BE DELETED IS A COPY INTO NOWHERE.
  #   ✔REPRODUCED (P66): "preserved 1 ... (verified 1 present)", then the copy went with
  #   the worktree. Resolved-prefix containment, never a substring test.
  case "$_p_dest_real/" in
    "$_p_wt_real"/*)
      _die 7 "REFUSING: --preserve-to '$_p_dest' resolves to '$_p_dest_real', which is INSIDE '$_p_rel'." \
             "This verb is about to delete that tree, so the copy would be destroyed with the" \
             "evidence it was taken from -- after being reported as verified. Nothing was removed." ;;
  esac
  _p_work="$(mktemp -d 2>/dev/null)" \
    || _die 7 "mktemp failed, so the preserve cannot be staged; nothing was removed."
  # ONE list, NUL-separated and worktree-relative, taken ONCE: the count, the clash check
  # and the verify all read these same paths in this same order.
  for _p_r in $EVIDENCE_ROOTS; do
    if [ -d "$_p_wt/$_p_r" ]; then ( cd "$_p_wt" && find "$_p_r" -type f -print0 ); fi
  done > "$_p_work/list0"
  if ! ( cd "$_p_wt" && xargs -0 cksum < "$_p_work/list0" ) > "$_p_work/src.sum" 2> "$_p_work/src.err"; then
    _p_why="$(head -3 "$_p_work/src.err" | tr '\n' ' ')"; rm -rf "$_p_work"
    _die 7 "could not re-read the evidence under '$_p_rel': $_p_why" "Nothing was copied or removed."
  fi
  _p_n="$(wc -l < "$_p_work/src.sum" | tr -d ' ')"
  if [ "$_p_n" -ne "$_p_counted" ]; then
    rm -rf "$_p_work"
    _die 7 "counted $_p_counted evidence file(s) under '$_p_rel' but re-read $_p_n: the tree changed" \
           "underneath this verb. Nothing was copied or removed."
  fi
  # ⚠ A FILE ALREADY AT THE DESTINATION UNDER THE SAME PATH, WITH OTHER BYTES, IS SOMEBODY
  #   ELSE'S EVIDENCE. ✔REPRODUCED (P66): two lanes preserved into one directory, and the
  #   second silently overwrote the first lane's findings while the count still "verified".
  #   `cksum` prints only the files it can read, so this lists exactly the paths that
  #   already exist there; any that is not byte-identical to its source is a clash.
  ( cd "$_p_dest_real" && xargs -0 cksum < "$_p_work/list0" ) > "$_p_work/pre.sum" 2>/dev/null
  LC_ALL=C sort "$_p_work/src.sum" > "$_p_work/src.sorted"
  LC_ALL=C sort "$_p_work/pre.sum" > "$_p_work/pre.sorted"
  _p_clash="$(LC_ALL=C comm -23 "$_p_work/pre.sorted" "$_p_work/src.sorted" | cut -d' ' -f3- | head -5)"
  if [ -n "$_p_clash" ]; then
    rm -rf "$_p_work"
    _die 7 "REFUSING: '$_p_dest_real' already holds a file at the same path with DIFFERENT bytes, so" \
           "copying would silently OVERWRITE evidence that is already there. Nothing was copied and" \
           "nothing was removed; preserve into a fresh directory. First clash(es):" \
           "$_p_clash"
  fi
  # Each root is copied UNDER ITS OWN NAME, so `scratchpad/x` and `.temp/x` can never land on
  # one path, and the destination reads as the worktree did.
  for _p_r in $EVIDENCE_ROOTS; do
    [ -d "$_p_wt/$_p_r" ] || continue
    if ! { mkdir -p "$_p_dest_real/$_p_r" && cp -R "$_p_wt/$_p_r/." "$_p_dest_real/$_p_r/"; }; then
      rm -rf "$_p_work"
      _die 7 "copy of '$_p_rel/$_p_r' -> '$_p_dest_real/$_p_r' FAILED; nothing was removed."
    fi
  done
  # ⚠ VERIFY EVERY FILE, NEVER A COUNT. The count this replaced passed over a destination
  #   that already held enough files of its own. Size and CRC, re-read at the destination
  #   for exactly the list that was copied, in the same order.
  ( cd "$_p_dest_real" && xargs -0 cksum < "$_p_work/list0" ) > "$_p_work/dst.sum" 2> "$_p_work/dst.err"
  if ! cmp -s "$_p_work/src.sum" "$_p_work/dst.sum"; then
    _p_why="$(head -3 "$_p_work/dst.err" | tr '\n' ' ')"; rm -rf "$_p_work"
    _die 7 "preserve VERIFY FAILED: the $_p_n evidence file(s) under '$_p_rel' do not all re-read" \
           "identical (size and CRC, file by file) under '$_p_dest_real'. $_p_why" \
           "REFUSING to remove '$_p_rel' -- the evidence would be lost."
  fi
  rm -rf "$_p_work"
  _say "preserved $_p_n evidence file(s) -> $_p_dest_real (every file re-read at the destination: size and CRC match)"
}

# ⚠⚠ A LANE'S EVIDENCE IS WHAT ITS ROW CITES, AND THIS VERB USED TO DELETE IT WITHOUT
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
#    whose evidence roots hold files cannot be removed silently. `--preserve-to <dir>`
#    makes the COPY this script's job so it cannot fail quietly; `--discard-evidence`
#    is the explicit "I do not want it", which is a decision rather than an accident.
#
# ⚠⚠ AND THE GATE THAT CLOSED P50 LOOKED IN ONE DIRECTORY WHILE THE LANES HAD MOVED TO
#    ANOTHER. ✔MEASURED 2026-09-15 (P66), read-only, on the five live lanes: every one
#    keeps its evidence under `.temp/<lane>-scratch/` (137 to 20,512 files each) and NONE
#    has a `scratchpad/` at all -- so this gate counted ZERO for all five, and `remove`
#    would have deleted 26,466 evidence files with no refusal. Lane `rc`'s 601-line
#    findings file had already died that way earlier in the same cycle. ✔REPRODUCED on
#    both twins in a throwaway repository: evidence under `.temp/`, no flag, rc=0,
#    "VERIFIED absent".
#    ★ THE GATE IS DEFINED BY ROOTS, NOT BY A NAMING CONVENTION: `EVIDENCE_ROOTS` names
#    both top-level scratch directories `.gitignore` sets aside, and ALL of each is gated.
#    Gating `.temp/<lane>-scratch/` alone would repeat the P50 mistake one convention
#    later -- a lane that names its directory differently is unguarded again. The cost
#    is measured and accepted: `.temp/` also holds a guard's fixture scratch
#    (`test-run-gate-scratch/`, 132 files in every live lane that ran the suite), so a
#    preserve copies that too. "Too much preserved" fails as a copy; "too little" fails
#    as evidence nobody can get back. ⓘ No `.temp/` or `scratchpad/` exists BELOW the top
#    level of any live lane (✔MEASURED the same day), so these two are every location
#    lanes use.
#    ★ AND THE PRESERVE COULD REPORT A VERIFIED COPY OF EVIDENCE IT THEN LOST, in two more
#    ways, both ✔REPRODUCED on both twins: a destination INSIDE the worktree, and a
#    destination already holding a same-named file. `_lw_preserve` refuses both, and its
#    verify re-reads every file rather than counting them.
cmd_remove() {  # <name> [--discard-work] [--preserve-to <dir> | --discard-evidence]
  name="${1:-}"; [ -n "$name" ] || _die 5 "usage: lane-worktree.sh remove <name> [--discard-work] [--preserve-to <dir> | --discard-evidence]"
  shift || true
  preserve_to=""; discard=0; discard_work=0
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --preserve-to) preserve_to="${2:-}"; [ -n "$preserve_to" ] || _die 5 "--preserve-to needs a directory"; shift 2 ;;
      --discard-evidence) discard=1; shift ;;
      --discard-work) discard_work=1; shift ;;
      --discard-scratchpad)
        # ⚠ RETIRED, AND REFUSED RATHER THAN KEPT AS AN ALIAS. It meant "delete the
        #   scratchpad" when the scratchpad was the only thing gated. Honouring it now would
        #   silently WIDEN a destructive flag to `.temp/` as well, so a caller who typed it
        #   meaning one directory would lose the other.
        _die 5 "--discard-scratchpad is retired: the gate now covers every evidence root ($EVIDENCE_ROOTS)," \
               "so a flag named for one of them would silently discard the others." \
               "Say which you mean: --discard-evidence deletes ALL of it; --preserve-to <dir> keeps it." ;;
      *) _die 5 "unknown option '$1' (expected --discard-work, --preserve-to <dir> or --discard-evidence)" ;;
    esac
  done
  [ "$discard" -eq 1 ] && [ -n "$preserve_to" ] \
    && _die 5 "--preserve-to and --discard-evidence contradict each other; pick one."
  # ⚠⚠ THE SAME NAME VALIDATION `cmd_add` PERFORMS, AND IT MATTERS MORE HERE, because
  # this verb now DELETES a directory tree. `cmd_add` refused a name with a slash or a
  # leading dot and `cmd_remove` did not -- harmless while the only verb was
  # `git worktree remove`, which simply declines an unknown path, and NOT harmless the
  # moment an `rm -rf` stands behind it. `remove ../..` must never resolve anywhere.
  case "$name" in
    */*|.*) _die 5 "lane name must be a single path component and must not start with '.': '$name'" ;;
  esac
  repo="$(_repo_root)"; rel=".worktrees/$name"; abs="$repo/$rel"

  # ── THE WORK GATE, FIRST ────────────────────────────────────────────────────
  # ⚠⚠ A LANE'S WORK IS WHAT `git worktree remove --force` DELETES, AND THIS VERB PASSED
  #    `--force` WITHOUT LOOKING. ✔REPRODUCED 2026-09-15 (P66) on BOTH twins in a throwaway
  #    repository: a lane whose own status read ` M tracked.txt` and `?? new-work.txt`,
  #    removed with no flag, rc=0 "removed ... (VERIFIED absent)" -- the edit and the new
  #    file gone. The evidence gate below guards the IGNORED scratch roots; the part of a
  #    lane git TRACKS, or would add, had no guard at all.
  # ★ THIS VERB ASKS ONE QUESTION: "does this worktree's OWN `git status` list a tracked
  #   modification, or an untracked file that is not ignored?" It never asks whether that
  #   work was folded into the main tree -- that is `scripts/lane-fold/lane-fold.py`'s
  #   measurement (the seed manifest, the lane's base commit, settled paths), and a second
  #   owner of it in two shells is how two answers would start to disagree. So a SEEDED path
  #   counts as work too, and the caller that KNOWS the work is folded says so with
  #   `--discard-work`: `lane-fold.py land` builds that flag only from its own "nothing left
  #   to fold" measurement.
  # ⚠ `--show-prefix` IS HOW "IS THIS DIRECTORY A WORKTREE ROOT?" IS ASKED WITHOUT COMPARING
  #   TWO SPELLINGS OF ONE PATH: it prints nothing at a root, and `.worktrees/<name>/` when
  #   git has walked up to the PARENT repository -- the P46 shape, a lane whose `.git` was
  #   emptied. Asked there, `git status` answers with the MAIN tree's changes, so a status
  #   that cannot be read at the worktree's own root is refused as well.
  # ⓘ `--no-optional-locks`: this is a question; it must not take the index lock of a
  #   worktree another process may be using.
  # ⚠⚠ AND A COMMIT IS WORK TOO, WHICH `git status` NEVER LISTS. ✔REPRODUCED 2026-09-15 (P66,
  #    round 3) on BOTH twins in a throwaway repository: a file committed inside a lane left
  #    its status EMPTY, `remove` with no flag exited 0, and the commit's only holder -- this
  #    worktree's HEAD, in its admin directory under the common git dir -- went with it.
  #    Afterwards no ref reached the commit, `git fsck --unreachable --no-reflogs` listed it,
  #    and `git prune --expire=now` deleted it.
  # ★ SO THE GATE ASKS A SECOND QUESTION: "which commits does this worktree's HEAD hold that no
  #   ref of the repository reaches?" -- `git rev-list <HEAD> --not --glob=refs/*`, asked AT THE
  #   REPOSITORY ROOT. Both halves of that spelling were measured on the same fixture:
  #   `--glob=refs/*` and NOT `--branches --tags --remotes`, because a commit that a shared ref
  #   outside those three still holds (`refs/keep/...`) survives the removal, and refusing it
  #   refuses a removal that loses nothing; AT THE ROOT and NOT inside the worktree, because
  #   asked inside it the glob also counts the worktree's OWN per-worktree refs
  #   (`refs/bisect/*`), which die with it -- a commit held only by one read as safe and was
  #   pruned. A lane still at its base holds none, and so does an unborn HEAD.
  # ⚠ A COMMIT LIST GIT CANNOT PRODUCE IS REFUSED, NOT READ AS EMPTY. ✔MEASURED: with one of a
  #   lane's commit objects missing, its `status` still exits 0 while the rev-list exits 128 --
  #   "cannot tell" is not "nothing to lose".
  if [ -e "$abs" ]; then
    work_readable=1; work=""; commits_readable=1; commits=""
    work_prefix="$(git --no-optional-locks -C "$abs" rev-parse --show-prefix 2>/dev/null)" || work_readable=0
    [ -n "$work_prefix" ] && work_readable=0
    if [ "$work_readable" -eq 1 ]; then
      work="$(git --no-optional-locks -C "$abs" status --porcelain --untracked-files=all 2>/dev/null)" \
        || work_readable=0
    fi
    if [ "$work_readable" -eq 1 ]; then
      work_head="$(git --no-optional-locks -C "$abs" rev-parse --verify -q HEAD 2>/dev/null)" || work_head=""
      if [ -n "$work_head" ]; then
        commits="$(git --no-optional-locks -C "$repo" rev-list --oneline "$work_head" --not --glob='refs/*' 2>/dev/null)" \
          || commits_readable=0
      fi
    fi
    if [ "$work_readable" -eq 0 ]; then
      [ "$discard_work" -eq 1 ] || _die 8 \
        "cannot read the git status of '$rel' at its own root, so it cannot be shown to carry no" \
        "uncommitted work (git cannot open it as a working tree there, and asked from inside it" \
        "git answers about a parent repository instead). REFUSING to delete it." \
        "  pass --discard-work to remove it anyway, deliberately."
      _say "DISCARDING '$rel', whose git status cannot be read at its own root, as instructed"
    elif [ "$commits_readable" -eq 0 ]; then
      [ "$discard_work" -eq 1 ] || _die 8 \
        "cannot list the commits of '$rel' that no ref of the repository reaches (git rev-list" \
        "failed at the repository root), so it cannot be shown to hold none. REFUSING to delete it." \
        "  pass --discard-work to remove it anyway, deliberately."
      _say "DISCARDING '$rel', whose commits cannot be listed, as instructed"
    else
      work_n=0; commit_n=0
      if [ -n "$work" ]; then work_n="$(printf '%s\n' "$work" | wc -l | tr -d ' ')"; fi
      if [ -n "$commits" ]; then commit_n="$(printf '%s\n' "$commits" | wc -l | tr -d ' ')"; fi
      if [ "$discard_work" -eq 0 ] && [ $(( work_n + commit_n )) -gt 0 ]; then
        {
          if [ "$work_n" -gt 0 ]; then
            printf 'lane-worktree: %s\n' \
              "'$rel' carries $work_n path(s) of UNCOMMITTED WORK -- tracked modifications, or untracked" \
              "files that are not ignored, by its own git status -- and would be DELETED with them." \
              "first path(s):"
            # One prefixed line per path, exactly as the `.ps1` twin prints them.
            printf '%s\n' "$work" | head -5 | sed 's/^/lane-worktree: /'
          fi
          if [ "$commit_n" -gt 0 ]; then
            printf 'lane-worktree: %s\n' \
              "'$rel' holds $commit_n COMMIT(S) that no branch, tag or other ref of the repository reaches --" \
              "only this worktree's HEAD holds them, and removing it would ORPHAN them:"
            _lw_list_commits "$commits"
          fi
          printf 'lane-worktree: %s\n' \
            "This verb cannot tell whether that work is already folded; that is lane-fold's measurement:" \
            "  python scripts/lane-fold/lane-fold.py land $name <production|harness> --apply" \
            "    (folds uncommitted work, passes --discard-work only after measuring nothing left to fold," \
            "     and refuses a lane that committed)" \
            "  git branch <branch> <commit>   keeps a commit, so the removal no longer orphans it" \
            "  --discard-work                 delete it deliberately"
        } >&2
        exit 8
      fi
      if [ "$work_n" -gt 0 ]; then
        _say "DISCARDING $work_n path(s) of uncommitted work under $rel, as instructed"
      fi
      if [ "$commit_n" -gt 0 ]; then
        _say "DISCARDING $commit_n commit(s) under $rel that no ref of the repository reaches, as instructed:"
        _lw_list_commits "$commits"
      fi
    fi
  fi

  # ── THE EVIDENCE GATE, BEFORE ANY DELETION ──────────────────────────────────
  # Counted with `find -type f`, so an empty directory tree is correctly "nothing to
  # preserve" and a single file under either root is enough to stop the removal.
  ev_total=0; ev_where=""
  for r in $EVIDENCE_ROOTS; do
    n=0
    if [ -d "$abs/$r" ]; then n="$(find "$abs/$r" -type f 2>/dev/null | wc -l | tr -d ' ')"; fi
    ev_total=$(( ev_total + n ))
    ev_where="$ev_where $r/=$n"
  done
  ev_where="${ev_where# }"
  if [ "$ev_total" -gt 0 ] && [ "$discard" -eq 0 ] && [ -z "$preserve_to" ]; then
    _die 7 "'$rel' holds $ev_total evidence file(s) ($ev_where) and would be DELETED with them." \
           "A lane's evidence is what its registry row cites -- findings logs, mutant" \
           "transcripts, gate logs, row cells. Choose explicitly:" \
           "  --preserve-to <dir>      copy ALL of it there FIRST; every file is re-read and matched" \
           "  --discard-evidence       delete it deliberately" \
           "This gate exists because a hand-rolled 'cp && remove' lost lane t2's evidence in P50," \
           "and because the gate that closed that looked only at scratchpad/ while every P66" \
           "lane kept its evidence under .temp/."
  fi
  if [ "$ev_total" -gt 0 ] && [ -n "$preserve_to" ]; then
    _lw_preserve "$abs" "$rel" "$preserve_to" "$ev_total"
  elif [ "$ev_total" -gt 0 ]; then
    _say "DISCARDING $ev_total evidence file(s) under $rel ($ev_where), as instructed"
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
            "                         remove <name> [--discard-work] [--preserve-to <dir> | --discard-evidence] |" \
            "                         list}" \
            "" \
            "The tree acted on defaults to the one THIS SCRIPT LIVES IN, never the" \
            "caller's cwd. --repo <path> names another tree deliberately." ;;
esac
