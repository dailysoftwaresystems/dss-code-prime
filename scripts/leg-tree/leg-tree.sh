#!/usr/bin/env sh
# PURPOSE: put a gate host's own clone on the tree under test before a leg, and restore it to pristine afterwards.
#
# ★★★ OPERATOR RULING 2026-08-26, and it replaces the previous arrangement rather
# than extending it:
#
#   "we should use already cloned repo in each leg [...] you can keep using the sync
#    process you already use, but CLEAN UP the changes after you finish them (you can
#    also use worktree in leg host if needed for parallel legs, clean up also needed).
#    [...] don't forget to check each leg branch before working in it, and also clean
#    up the changes after finished. that's the standard"
#
# THE THREE LEG REPOSITORIES, which are clones and not sync targets:
#   WSL     ~/src/dss-code-prime
#   VPS     ~/src/Github/dss-code-prime
#   macOS   ~/src/dss-code-prime
#
# ── WHY A CLONE AND NOT A SYNCED `.git` ──────────────────────────────────────
# Two carriages withheld `.git` and one shipped it, so every host's git described a
# different thing and none of them described the tree under test.
# ✔MEASURED 2026-08-26, on the macOS host, before this landed: its `.git` sat on
# branch `feature/c23-conformance-burndown-3` at `8cb9afbd` (cycle P33, THREE commits
# back) with a 2,696-path index under a 2,759-path working tree, and 292 MB of history
# describing none of it.
# ★ THE COST IS NOT THE DISK, IT IS THAT GUARDS ASK GIT QUESTIONS. `check-line-endings`
# reads `git ls-files --eol`; `check-shell-portability` was ALREADY rewritten in
# 2026-08-22 to stop asking `git ls-files` because this very host answered about a
# commit that deleted `tools/*.sh` in P17 and produced SEVEN violations against files
# that do not exist. A host whose git disagrees with its files makes every git-reading
# guard a coin flip, and the flip is invisible from the driver.
# ⇒ the host's OWN clone is the authority: fetched, put on the driver's branch at the
# driver's commit, and then the working tree is synced over it. `git status` on the
# host then shows exactly what `git status` shows on the driver, which is the whole
# point -- attribution stops being a guess.
#
# ── WHY RESTORE IS NOT OPTIONAL ──────────────────────────────────────────────
# A sync leaves the clone dirty by construction. Left that way, the NEXT leg starts
# from a tree nobody described, and the one after that inherits both. ✔MEASURED the
# same day: both remote hosts carried a stale `dss-probe-6f4aab73` worktree at a
# detached HEAD, registered and `prunable`, from a cycle that never cleaned up after
# itself. ⚠ This is also the standing "no `git clean`/`reset --hard` on a gate host"
# order being NARROWED BY ITS AUTHOR: the operator named cleanup as the standard on
# 2026-08-26, so restoring a leg repo is now required where it was once forbidden.
# It stays scoped -- `-fd` and never `-fx`, so `build/` and the ccache survive and a
# leg does not pay for a cold rebuild to satisfy tidiness.
#
# Exit codes: 0 OK · 2 not a usable clone · 3 the branch could not be reached · 4 usage.

leg_tree_die() {
    printf '\n[X] leg-tree: %s\n' "$1" >&2
    exit "${2:-2}"
}

# ★★ TILDE EXPANSION IS DONE HERE, ONCE, BECAUSE THE SHELL WILL NOT DO IT.
# ✔MEASURED 2026-08-26 against the live VPS, first run of this file: every leg names
# its host repo as `~/src/...`, and `cd "$_lt_repo"` with the value in QUOTES does not
# expand the tilde -- `~` is expanded by the shell only when it appears UNQUOTED in the
# source text, never as the content of a variable. The result was
# `no such directory: ~/src/Github/dss-code-prime` on a host where that directory
# plainly exists, and the restore then "succeeded" by skipping.
# ★ THE SKIP IS THE DANGEROUS HALF, not the failed cd: `leg_tree_restore` returns 0
# when the directory is missing, so a typo'd or unexpanded path would leave every leg
# host dirty forever while every leg reported success. The path is normalised at the
# ONE place both verbs share, so neither can be given a path the other would reject.
# ⓘ The carriages hit the mirror image of this and solved it the other way -- they emit
# `LEG=` UNQUOTED so the remote shell expands it. That works for a value going INTO a
# shell; this is a value already inside one.
leg_tree_abs() {
    case "$1" in
        "~")   printf '%s\n' "$HOME" ;;
        "~/"*) printf '%s/%s\n' "$HOME" "${1#\~/}" ;;
        *)     printf '%s\n' "$1" ;;
    esac
}

# leg_tree_driver_identity <src>
#
# Sets LEG_TREE_DRIVER_BRANCH, LEG_TREE_DRIVER_SHA and LEG_TREE_DRIVER_GIT_DIR from
# the DRIVER's checkout. Returns 0 on success, 1 when this namespace's git cannot
# describe <src> at all. LEG_TREE_DRIVER_GIT_DIR is EMPTY when a plain `git -C` sees
# the tree, and holds the resolved gitdir when it does not -- callers that need more
# than the identity go through `leg_tree_driver_git` rather than reading it.
#
# ★★★ WHY THIS IS NOT `git -C "$SRC" rev-parse` AT EACH CALL SITE, WHICH IS WHAT THE
# CARRIAGES USED TO DO. A LANE WORKTREE'S `.git` IS A FILE, AND ON A WINDOWS-CREATED
# WORKTREE THAT FILE NAMES A WINDOWS-ABSOLUTE GITDIR.
# ✔MEASURED 2026-08-31 (P47), from inside WSL against `.worktrees/sq`:
#     $ cat .worktrees/sq/.git
#     gitdir: C:/Source/DailySoftware/dss-code-prime/.git/worktrees/sq
#     $ git -C /mnt/c/.../.worktrees/sq rev-parse HEAD
#     fatal: not a git repository: /mnt/c/.../.worktrees/sq/C:/Source/.../.git/worktrees/sq
# -- because `C:/...` is not absolute to a POSIX git, so it is JOINED to the worktree
# path. Both POSIX carriages read the driver's identity from inside WSL over `/mnt/c`,
# so BOTH died before moving a byte (`cannot read the driver's branch from ...`), and
# `.worktrees/` is the SANCTIONED home for lane worktrees under the 2026-08-26 ruling.
# ⇒ no lane could run a WSL, VPS or macOS leg from its own worktree:
#   D-SCRIPT-CARRIAGES-CANNOT-IDENTIFY-A-CROSS-NAMESPACE-LANE-WORKTREE
#
# ★ THE FIX IS TO RESOLVE THE GITDIR, NOT TO LET THE CALLER DECLARE THE ANSWER. A
# `--branch`/`--sha` escape hatch would let a driver ASSERT an identity instead of
# READING one, and the identity is the single fact a leg's whole attribution rests on.
# ⓘ It also covers the ordinary POSIX worktree, whose `.git` file may name a RELATIVE
# gitdir -- resolved against <src>, which is what git itself does.
leg_tree_driver_identity() {
    _lt_src=$(leg_tree_abs "$1")
    LEG_TREE_DRIVER_BRANCH=''
    LEG_TREE_DRIVER_SHA=''
    LEG_TREE_DRIVER_GIT_DIR=''
    [ -n "$_lt_src" ] || return 1

    # The ordinary case: a real repository, or a worktree whose gitdir resolves here.
    if _lt_s=$(git -C "$_lt_src" rev-parse HEAD 2>/dev/null) && [ -n "$_lt_s" ]; then
        _lt_b=$(git -C "$_lt_src" rev-parse --abbrev-ref HEAD 2>/dev/null)
        [ -n "$_lt_b" ] || return 1
        LEG_TREE_DRIVER_SHA="$_lt_s"; LEG_TREE_DRIVER_BRANCH="$_lt_b"
        return 0
    fi

    # A worktree whose `.git` FILE names a gitdir this namespace cannot follow.
    [ -f "$_lt_src/.git" ] || return 1
    _lt_gd=$(sed -n 's/^gitdir: *//p' "$_lt_src/.git" | head -1)
    [ -n "$_lt_gd" ] || return 1
    # ★★ THE THREE CASES ARE TRIED IN THIS ORDER AND THE ORDER IS THE WHOLE OF IT.
    # ✔MEASURED 2026-08-31, by getting it wrong first: resolving "not POSIX-absolute"
    # as "relative to the worktree" BEFORE testing for a foreign absolute path turns
    # `C:/Source/.../.git/worktrees/sq` into
    # `/mnt/c/.../.worktrees/sq/C:/Source/.../.git/worktrees/sq` -- which is byte for
    # byte the mangling git itself performs, i.e. the resolver reproduced the very
    # defect it exists to undo, and returned "cannot describe this tree".
    #   1. already a directory here            -> take it (POSIX-absolute, or relative
    #                                             to a cwd that happens to be right)
    #   2. FOREIGN-ABSOLUTE (`X:/…` / `X:\…`)  -> translate; only wslpath is claimed
    #   3. anything else                       -> relative to the worktree, as git does
    if [ ! -d "$_lt_gd" ]; then
        case "$_lt_gd" in
            [A-Za-z]:[/\\]*)
                # ★ `wslpath` is WSL's own, shipped with the distro, and WSL is the
                # ONLY namespace crossing these carriages make. A second translator
                # here would be inventing a portability claim nothing has measured.
                command -v wslpath >/dev/null 2>&1 || return 1
                _lt_gd=$(wslpath -u "$_lt_gd" 2>/dev/null) || return 1
                ;;
            /*) ;;                              # POSIX-absolute and absent: nothing to try
            *)  _lt_gd="$_lt_src/$_lt_gd" ;;    # git allows a gitdir relative to the worktree
        esac
    fi
    [ -d "$_lt_gd" ] || return 1

    _lt_s=$(git --git-dir="$_lt_gd" --work-tree="$_lt_src" rev-parse HEAD 2>/dev/null) || return 1
    _lt_b=$(git --git-dir="$_lt_gd" --work-tree="$_lt_src" rev-parse --abbrev-ref HEAD 2>/dev/null) || return 1
    [ -n "$_lt_s" ] && [ -n "$_lt_b" ] || return 1
    LEG_TREE_DRIVER_SHA="$_lt_s"; LEG_TREE_DRIVER_BRANCH="$_lt_b"
    LEG_TREE_DRIVER_GIT_DIR="$_lt_gd"
    return 0
}

# leg_tree_driver_git <src> <git-arg>...
#
# Run ONE git command against the DRIVER's checkout, in whichever namespace can see
# it. Requires `leg_tree_driver_identity <src>` to have succeeded first -- it is that
# call which decides whether a `--git-dir` is needed, and re-deciding here would be a
# second answer to a question already answered.
# ★ THIS EXISTS SO NO CALLER READS `LEG_TREE_DRIVER_GIT_DIR` ITSELF. A carriage wants
# `git log -1` and `git status --porcelain` for its own report, and a bare `git -C`
# for those returned `(no git)` and `0 path(s)` on a cross-namespace worktree -- the
# second of which is byte-identical to the reading a genuinely pristine tree produces,
# which is the failure mode the WSL carriage already names in its own attribution
# block. One accessor keeps both halves of the report on the same git.
leg_tree_driver_git() {
    _lt_g_src=$(leg_tree_abs "$1"); shift
    if [ -n "${LEG_TREE_DRIVER_GIT_DIR:-}" ]; then
        git --git-dir="$LEG_TREE_DRIVER_GIT_DIR" --work-tree="$_lt_g_src" "$@"
    else
        git -C "$_lt_g_src" "$@"
    fi
}

# leg_tree_owning_root <path>
#
# Print the absolute root of the WORKING TREE THAT CONTAINS <path>. Returns 1 when
# no working tree contains it. <path> may be a file or a directory; a file is taken
# by its directory.
#
# ★★★ THIS ANSWERS A DIFFERENT QUESTION FROM `leg_tree_driver_identity`, AND THAT IS
# WHY IT EXISTS RATHER THAN BEING FOLDED INTO IT.
#   leg_tree_driver_identity <src>  -- "WHAT is the tree at <src>?"  (branch, sha, gitdir)
#   leg_tree_owning_root     <path> -- "WHICH tree contains <path>?" (a root)
# The identity resolver TAKES the tree as its argument: every caller must already
# know which tree it means. This one DERIVES that argument. Making the identity
# resolver return a root would have been a true answer to the wrong question --
# [[feedback-an-instrument-that-answers-an-adjacent-question]] -- so the reuse runs
# the other way: this helper is a thin layer ON the identity resolver, in the same
# file, and case 2 below is nothing but a call into it. There is no second resolver.
#
# ⚠⚠ THE CALLER THIS WAS ADDED FOR PASSES ITS OWN SCRIPT PATH, NOT `$PWD`, AND THE
# DIFFERENCE IS THE WHOLE POINT.
# [[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]] `scripts/lane-worktree/` and
# `scripts/lane-fold/` derived their root from a bare `git rev-parse --show-toplevel`,
# which answers "what repository am I STANDING in?" -- so every path they then
# computed, including an `rm -rf` target, was rooted at whichever repository the
# caller's shell happened to be in. ✔MEASURED 2026-09-02 from a throwaway repository
# outside this one: all three lane verbs reported that repository's `.worktrees/`
# while running out of this one's checkout.
# ⇒ a verb that manages `.worktrees/` must ask which tree ITS OWN FILE belongs to.
# `$PWD` is a property of the caller's shell; the script's path is a property of the
# script. Only the second is stable under `cd`.
#
# ⓘ THE ORDINARY ANSWER IS `--show-toplevel` RUN AT THE RIGHT PLACE -- with `-C
# <dir-of-my-file>` rather than at the cwd. That single change is the fix; case 2
# exists only for the namespace crossing the identity resolver already owns.
# ✔MEASURED 2026-09-02, why `--show-toplevel` and not `--git-common-dir`: for a
# SUBMODULE checkout (`.git` a file naming `<super>/.git/modules/<name>`),
# `--git-common-dir` names `<super>/.git/modules/<name>` and `git worktree list`
# names it too -- neither is a working tree at all, and rooting a removal there
# would aim it inside `.git`. `--show-toplevel` answers `<super>/child`, correctly.
# A bare repository has no working tree; `--show-toplevel` fatals there and case 2's
# `.git` test does not match, so this helper correctly returns 1 rather than handing
# back the bare directory as somewhere to put a checkout.
leg_tree_owning_root() {
    _lt_or_p=$(leg_tree_abs "$1")
    [ -n "$_lt_or_p" ] || return 1
    [ -d "$_lt_or_p" ] || _lt_or_p=$(dirname "$_lt_or_p")
    _lt_or_d=$(cd "$_lt_or_p" 2>/dev/null && pwd -P) || return 1

    # 1. The ordinary case, in every namespace that can simply see the tree.
    if _lt_or_t=$(git -C "$_lt_or_d" rev-parse --show-toplevel 2>/dev/null) \
       && [ -n "$_lt_or_t" ]; then
        printf '%s\n' "$_lt_or_t"
        return 0
    fi

    # 2. A worktree whose `.git` FILE names a gitdir THIS namespace cannot follow --
    # the Windows-created worktree read from inside WSL that
    # `leg_tree_driver_identity` was written for. `git -C` fails outright there, so
    # walk up to the directory that HOLDS the `.git` and let the identity resolver
    # prove it can describe it. The directory holding a resolvable `.git` IS the
    # root, so nothing here re-derives what case 1 would have printed.
    while :; do
        if [ -e "$_lt_or_d/.git" ] \
           && leg_tree_driver_identity "$_lt_or_d" >/dev/null 2>&1; then
            printf '%s\n' "$_lt_or_d"
            return 0
        fi
        _lt_or_up=$(dirname "$_lt_or_d")
        [ "$_lt_or_up" != "$_lt_or_d" ] || return 1
        _lt_or_d="$_lt_or_up"
    done
}

# leg_tree_prepare <repo> <branch> <sha>
#
# ⚠ `<sha>` is the DRIVER's HEAD. It is normally already on the remote, because a gate
# runs before a commit and the previous commit is pushed -- but "normally" is not
# "always", and an unpushed HEAD must NOT be fatal: the sync overwrites every tracked
# file anyway, so the working tree under test is correct either way. What changes is
# only the BASELINE the host's git compares against, so that case is reported loudly
# and the leg continues. A leg that refuses to run because of an attribution detail
# would be trading a real measurement for a bookkeeping one.
leg_tree_prepare() {
    _lt_repo=$(leg_tree_abs "$1"); _lt_branch="$2"; _lt_sha="$3"
    [ -n "$_lt_repo" ] && [ -n "$_lt_branch" ] || leg_tree_die "prepare needs <repo> <branch> <sha>" 4

    cd "$_lt_repo" 2>/dev/null || leg_tree_die "no such directory: $_lt_repo"
    git rev-parse --is-inside-work-tree >/dev/null 2>&1 \
        || leg_tree_die "$_lt_repo is not a git work tree. This standard requires a real CLONE on every leg host -- clone it there once rather than letting a sync invent one."

    _lt_was_branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo '<none>')
    _lt_was_head=$(git rev-parse --short HEAD 2>/dev/null || echo '<none>')
    _lt_was_dirty=$(git status --porcelain 2>/dev/null | wc -l | tr -d ' ')

    # Drop registrations for worktrees whose directory is gone. Cheap, and it is the
    # half of worktree hygiene that cannot be done from the driver.
    git worktree prune 2>/dev/null || true

    # Non-fatal: a host that cannot reach origin can still test the synced tree.
    if git fetch --quiet origin 2>/dev/null; then
        _lt_fetch=ok
    else
        _lt_fetch=FAILED
    fi

    # ★★ THE WORKING TREE IS DROPPED **BEFORE** THE CHECKOUT, NOT AFTER, AND THE
    # ORDER IS THE WHOLE POINT.
    # ✔MEASURED 2026-08-28, on the live VPS and then reproduced in a throwaway
    # repo: `git checkout -B <branch> <sha>` REFUSES over local modifications --
    #     error: Your local changes to the following files would be overwritten
    #            by checkout: ... Aborting            (rc=1)
    # -- and this function used to reset only afterwards, so it could not move a
    # host whose tree was dirty. ★ That is exactly the host it most needs to
    # move: `--mode sync-only` leaves a clone dirty BY CONTRACT, and a leg that
    # dies before its restore leaves the dirtiest tree of all. The arm64 VPS sat
    # at 2853 modified + 78 untracked paths after a failed sqlite leg, and
    # `prepare` answered `could not put ... on ... at ...` with rc=3 -- naming
    # the branch, which was innocent. Resetting first, the identical checkout
    # returns 0 and lands clean.
    # ⚠ Discarding here is prepare's CONTRACT ("pristine before sync"), not a
    # liberty: `_lt_was_dirty` is already captured above, so the count survives
    # into the report even though the paths do not. `-fd`, never `-fdx`.
    git reset --hard --quiet HEAD 2>/dev/null || true
    git clean -fdq 2>/dev/null || true

    # ★ THE BRANCH CHECK THE OPERATOR ASKED FOR, and it is done by MOVING the host
    # rather than by asserting about it: `-B` puts the branch at the driver's commit
    # whether the host was behind, ahead, or on something else entirely.
    # ⚠ git's own stderr is NOT swallowed on the failing path. It was, and the
    # operator-visible result was a bare "could not put" with the cause deleted
    # -- an error that hides its own diagnosis, which this project treats as a
    # defect in its own right.
    #
    # ★★★ A DETACHED DRIVER IS THE LANE-WORKTREE CASE, AND IT IS THE NORMAL CASE --
    # NOT AN ERROR. Every one of the four carriages derives the branch it asks for
    # with `git rev-parse --abbrev-ref HEAD`, and that command prints the LITERAL
    # STRING `HEAD` when the driver is detached. A lane worktree created by
    # `scripts/lane-worktree/` is detached BY CONSTRUCTION, and `.worktrees/` is the
    # SANCTIONED home for lane worktrees under the 2026-08-26 ruling -- so the branch
    # value that reaches this function from a lane is `HEAD`, on every carriage.
    # ✔MEASURED 2026-08-31 (P47) in a throwaway repo outside the repository:
    #     git checkout -B HEAD <sha>   ->   rc=128
    #     fatal: 'HEAD' is not a valid branch name
    # so the `-B` arm below could not move a detached driver's leg host at all, and
    # every carriage died at `could not put <repo> on HEAD at <sha>` (rc 3) naming a
    # branch that CANNOT exist -- git refuses `HEAD` as a branch name outright, which
    # is what makes the string unambiguous here rather than merely conventional.
    # ⇒ NO LANE COULD RUN A LEG. That is the whole reason this arm exists, and it is
    # a REAL defect in the standard rather than a convenience:
    #   D-SCRIPT-LEG-TREE-REFUSES-A-DETACHED-DRIVER
    # ★ THE LEG HOST IS PUT DETACHED AT THE SAME COMMIT, which is the honest mirror of
    # what the driver IS. Inventing a branch name here would be worse than failing:
    # the leg host's `git log` is the ONLY record of which commit a leg measured, and
    # a fabricated branch would make two different lanes' trees answer to one name.
    # ⚠ NO FALLBACK ARM, deliberately: a detached driver has no branch to fall back
    # to, so a host that does not carry the commit is refused rather than silently
    # measured at whatever it happened to be sitting on. That is the opposite trade
    # from the branch case below, and it is forced -- there, `checkout <branch>` still
    # lands on a tree the sync then overwrites and the ONLY casualty is the baseline
    # `dirty` is read against; here there is no such second-best target to name.
    if [ "$_lt_branch" = "HEAD" ]; then
        [ -n "$_lt_sha" ] \
            || leg_tree_die "the driver is DETACHED (branch reads as the literal 'HEAD') but named no commit. A detached driver has no branch to fall back to, so the sha is the only thing that can identify the tree under test." 4
        git cat-file -e "${_lt_sha}^{commit}" 2>/dev/null \
            || leg_tree_die "the driver is DETACHED at $_lt_sha and this host does not carry that commit (fetch=$_lt_fetch). Push it, or run the leg from a checkout that is on a branch -- there is no branch here to fall back to." 3
        git checkout --quiet --detach "$_lt_sha" \
            || leg_tree_die "could not put $_lt_repo at detached $_lt_sha (git's reason is directly above)" 3
        _lt_at="$_lt_sha"
        printf '! leg-tree: the driver is DETACHED, so %s is put DETACHED at %s rather than on a branch.\n' \
            "$_lt_repo" "$_lt_sha" >&2
        printf '  That is the honest mirror of the driver (a lane worktree), not a degraded mode.\n' >&2
    elif [ -n "$_lt_sha" ] && git cat-file -e "${_lt_sha}^{commit}" 2>/dev/null; then
        git checkout --quiet -B "$_lt_branch" "$_lt_sha" \
            || leg_tree_die "could not put $_lt_repo on $_lt_branch at $_lt_sha (git's reason is directly above)" 3
        _lt_at="$_lt_sha"
    else
        git checkout --quiet "$_lt_branch" \
            || leg_tree_die "could not check out '$_lt_branch' in $_lt_repo (fetch=$_lt_fetch; git's reason is directly above)" 3
        _lt_at=$(git rev-parse --short HEAD 2>/dev/null)
        printf '! leg-tree: the driver HEAD %s is NOT on this host (fetch=%s).\n' \
            "${_lt_sha:-<unset>}" "$_lt_fetch" >&2
        printf '  Continuing at %s: the sync overwrites every tracked file, so the TREE under\n' "$_lt_at" >&2
        printf '  test is still correct -- but this host git compares it against a different\n' >&2
        printf '  baseline, so read `dirty` below as relative to %s and not to the driver.\n' "$_lt_at" >&2
    fi

    # The second pass, after the branch has moved. Ordinarily a no-op now that the
    # first one runs above, and kept deliberately: `-B` can land on a commit whose
    # tree differs from the one just cleaned, and this function's whole promise to
    # its caller is "pristine before sync". Cheap insurance on a safety-critical path.
    # `-fd`, never `-fdx`: ignored paths (`build/`, the ccache) are the leg's own
    # working state and re-making them costs a cold build for no correctness gain.
    git reset --hard --quiet HEAD 2>/dev/null || true
    git clean -fdq 2>/dev/null || true

    printf 'leg-tree: prepared %s\n' "$_lt_repo"
    printf '  was    : %s @ %s, %s dirty path(s)\n' "$_lt_was_branch" "$_lt_was_head" "$_lt_was_dirty"
    printf '  now    : %s @ %s, fetch=%s, pristine before sync\n' \
        "$(git rev-parse --abbrev-ref HEAD 2>/dev/null)" \
        "$(git rev-parse --short HEAD 2>/dev/null)" "$_lt_fetch"
}

# leg_tree_restore <repo> [sha]
#
# ★ Called on EVERY exit path, a failure included. A leg that dies half way leaves the
# dirtiest tree of all, which is exactly when the next leg most needs a clean one.
leg_tree_restore() {
    _lt_repo=$(leg_tree_abs "$1"); _lt_sha="${2:-}"
    cd "$_lt_repo" 2>/dev/null || { printf '! leg-tree: restore skipped, no %s\n' "$_lt_repo" >&2; return 0; }
    git rev-parse --is-inside-work-tree >/dev/null 2>&1 \
        || { printf '! leg-tree: restore skipped, %s is not a work tree\n' "$_lt_repo" >&2; return 0; }

    _lt_dirty=$(git status --porcelain 2>/dev/null | wc -l | tr -d ' ')
    if [ -n "$_lt_sha" ] && git cat-file -e "${_lt_sha}^{commit}" 2>/dev/null; then
        git reset --hard --quiet "$_lt_sha" 2>/dev/null || true
    else
        git reset --hard --quiet HEAD 2>/dev/null || true
    fi
    git clean -fdq 2>/dev/null || true
    git worktree prune 2>/dev/null || true

    printf 'leg-tree: restored %s -- %s dirty path(s) discarded, now %s dirty, at %s\n' \
        "$_lt_repo" "$_lt_dirty" \
        "$(git status --porcelain 2>/dev/null | wc -l | tr -d ' ')" \
        "$(git rev-parse --short HEAD 2>/dev/null)"

    # ★★ NAME THE BUILD ROOTS THIS RESTORE JUST ORPHANED.
    # `clean -fd` (never `-fdx`) deliberately SPARES ignored paths, so `build/`
    # survives and a leg skips a cold rebuild -- a good trade whose unstated cost
    # is that a binary built from the SYNCED tree is now sitting over sources
    # rolled back to $_lt_sha. Nothing is broken until somebody uses that binary,
    # and then it fails somewhere far away wearing a diagnostic about the CONFIG.
    # ✔MEASURED 2026-08-26 (D-BENCH-COMPILER-AND-CONFIG-MAY-COME-FROM-DIFFERENT-COMMITS):
    # exactly this pairing cost a macOS benchmark leg its DSS arm, and the error
    # named `/opcodes/10/encoding/variants/0/resultSlot` -- the config, which was
    # innocent.
    # ⚠ DELIBERATELY A DIAGNOSTIC AND NOT A DELETION. Removing the build roots
    # would make every leg pay a cold rebuild to prevent a mistake only some legs
    # can make, and the enforcing check belongs where the binary is USED (the
    # benchmark's pre-flight now refuses a compiler that cannot compile for the
    # target it is about to be measured on). What restore owes is VISIBILITY at
    # the moment the state is created, not a silent tidy-up.
    if [ "$_lt_dirty" != "0" ] && [ -d build ]; then
        _lt_roots=$(find build -maxdepth 2 -name CMakeCache.txt -print 2>/dev/null \
                    | sed 's|/CMakeCache.txt$||' | tr '\n' ' ')
        if [ -n "$_lt_roots" ]; then
            printf '! leg-tree: these build root(s) were built from the tree just discarded and now sit over %s: %s\n' \
                "$(git rev-parse --short HEAD 2>/dev/null)" "$_lt_roots" >&2
            printf '! leg-tree: they are KEPT on purpose (cold rebuilds are expensive); rebuild before trusting a binary from them.\n' >&2
        fi
    fi
}

# ── dispatch, so this file is BOTH inlineable and runnable ───────────────────
# The two remote carriages inline this text into their payload and call the
# functions; the WSL leg runs the file. One owner either way.
case "${1:-}" in
    prepare) shift; leg_tree_prepare "$@" ;;
    restore) shift; leg_tree_restore "$@" ;;
    "")      ;;  # sourced or inlined: define the functions and do nothing
    *)       leg_tree_die "unknown subcommand '$1' (expected: prepare, restore)" 4 ;;
esac
