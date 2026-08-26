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

    # ★ THE BRANCH CHECK THE OPERATOR ASKED FOR, and it is done by MOVING the host
    # rather than by asserting about it: `-B` puts the branch at the driver's commit
    # whether the host was behind, ahead, or on something else entirely.
    if [ -n "$_lt_sha" ] && git cat-file -e "${_lt_sha}^{commit}" 2>/dev/null; then
        git checkout --quiet -B "$_lt_branch" "$_lt_sha" 2>/dev/null \
            || leg_tree_die "could not put $_lt_repo on $_lt_branch at $_lt_sha" 3
        _lt_at="$_lt_sha"
    else
        git checkout --quiet "$_lt_branch" 2>/dev/null \
            || leg_tree_die "could not check out '$_lt_branch' in $_lt_repo (fetch=$_lt_fetch)" 3
        _lt_at=$(git rev-parse --short HEAD 2>/dev/null)
        printf '! leg-tree: the driver HEAD %s is NOT on this host (fetch=%s).\n' \
            "${_lt_sha:-<unset>}" "$_lt_fetch" >&2
        printf '  Continuing at %s: the sync overwrites every tracked file, so the TREE under\n' "$_lt_at" >&2
        printf '  test is still correct -- but this host git compares it against a different\n' >&2
        printf '  baseline, so read `dirty` below as relative to %s and not to the driver.\n' "$_lt_at" >&2
    fi

    git reset --hard --quiet HEAD 2>/dev/null || true
    # `-fd`, never `-fdx`: ignored paths (`build/`, the ccache) are the leg's own
    # working state and re-making them costs a cold build for no correctness gain.
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
