#!/usr/bin/env sh
# PURPOSE: print the `.secrets` directory a checkout must read, following a lane worktree back to the main checkout that holds it.
#
# ─────────────────────────────────────────────────────────────────────────────
# WHY THIS EXISTS
#
# `.secrets/` holds connection data and key PATHS for the real gate hosts, and it is
# GITIGNORED on purpose (this repo is slated to go public). Being gitignored is
# exactly what makes it invisible to a linked worktree: `git worktree add` writes only
# TRACKED files, so `.worktrees/<lane>/.secrets/` does not exist and cannot exist.
#
# ★★★ AND `.worktrees/` IS THE SANCTIONED HOME FOR LANE WORKTREES (operator ruling
# 2026-08-26, re-stated in `scripts/carriage-excludes/`'s MUST_NEVER_TRAVEL floor), so
# every lane runs from a checkout with no `.secrets/` in it.
# ✔MEASURED 2026-08-31 (P47), from inside WSL, both remote carriages, first call:
#     ssh-macos: connection data missing.
#       Create /mnt/c/.../.worktrees/sq/.secrets/macos.env with DSS_MACOS_HOST, ...
#     ssh-arm64-vps: connection data missing.
#       Create /mnt/c/.../.worktrees/sq/.secrets/arm64-vps.env with DSS_VPS_HOST, ...
# -- rc=3 from both, naming a path that is correct about the checkout and wrong about
# the machine. ⇒ NO LANE COULD REACH THE arm64 VPS OR THE MAC AT ALL, which is two of
# the four gate hosts, and the message invited the one repair that must never be made:
# copying key material into a second directory.
#   D-SCRIPT-CARRIAGES-LOOK-FOR-SECRETS-INSIDE-A-LANE-WORKTREE
#
# ★ THE ANSWER IS RESOLVED, NEVER COPIED. A lane worktree and its main checkout are
# ONE checkout as far as host credentials are concerned; the fix is to look in the
# other half of it, not to duplicate `.secrets/` per lane. A duplicate would multiply
# the number of places a private key sits on disk by the number of live lanes, and
# `.worktrees/` is a directory this project deletes without ceremony.
#
# ★ NO `.ps1` TWIN OF *THIS FILE*, AND THAT IS THE JUDGEMENT NOT AN OMISSION -- but it
# is a NARROW judgement and the reason matters. The rule it encodes DOES have to reach
# the Windows leg, and it does: `ssh-macos.ps1` and `ssh-arm64-vps.ps1` carry the same
# three-case resolution inline, in PowerShell, as a MIRRORED REGION -- the pattern this
# harness already uses where the two callers are in two languages. What would be wrong
# is a `.ps1` here that shells out or re-implements the file: PowerShell cannot SOURCE
# a POSIX script, so a `.ps1` sibling could only be a second implementation wearing the
# same name, which is the thing one owner is supposed to prevent. Kept in step BY
# REVIEW, pinned by the same measurement, and both halves name this file.
#
# USAGE
#   secrets_dir=$(sh scripts/repo-secrets/repo-secrets.sh <repo-root>)   # as a program
#   . scripts/repo-secrets/repo-secrets.sh ""                            # for the fn
#   repo_secrets_dir <repo-root>
#
# ★ THE EMPTY ARGUMENT IS LOAD-BEARING when sourcing, exactly as it is for
# `scripts/leg-tree/leg-tree.sh`: `.` forwards the CALLER's positional parameters
# unless given its own, so without it this file would read the caller's first argument
# as a repo root and print an answer nobody asked for.
#
# ALWAYS PRINTS A PATH AND ALWAYS EXITS 0 when given a root. A caller's own
# "connection data missing" message is the fail-loud point, and it must be able to
# name a concrete directory; a refusal here would replace a message that says which
# file to create with one that says a resolver gave up.

# repo_secrets_dir <repo-root>
repo_secrets_dir() {
    _rs_repo="$1"
    [ -n "$_rs_repo" ] || return 1

    # The ordinary checkout, and the deliberate short-circuit: a main checkout never
    # pays for any of the resolution below.
    if [ -d "$_rs_repo/.secrets" ]; then
        printf '%s\n' "$_rs_repo/.secrets"
        return 0
    fi

    # A linked worktree names its gitdir in a `.git` FILE.
    if [ -f "$_rs_repo/.git" ]; then
        _rs_gd=$(sed -n 's/^gitdir: *//p' "$_rs_repo/.git" | head -1)
        if [ -n "$_rs_gd" ] && [ ! -d "$_rs_gd" ]; then
            # ⚠ THE ORDER OF THESE THREE CASES IS THE WHOLE OF IT, and it is the same
            # order (and the same measurement) as `leg_tree_driver_identity`'s: a
            # Windows-created worktree's `.git` names a WINDOWS-ABSOLUTE gitdir, and
            # testing "not POSIX-absolute" first turns `C:/...` into
            # `<worktree>/C:/...` -- reproducing the mangling this exists to undo.
            case "$_rs_gd" in
                [A-Za-z]:[/\\]*)
                    if command -v wslpath >/dev/null 2>&1; then
                        _rs_gd=$(wslpath -u "$_rs_gd" 2>/dev/null) || _rs_gd=""
                    else
                        _rs_gd=""
                    fi
                    ;;
                /*) ;;                                  # absolute and absent
                *)  _rs_gd="$_rs_repo/$_rs_gd" ;;       # git allows it relative
            esac
        fi
        if [ -n "$_rs_gd" ] && [ -d "$_rs_gd" ] && [ -f "$_rs_gd/commondir" ]; then
            # `commondir` names the MAIN checkout's `.git`, relative to the gitdir.
            # ✔MEASURED: it reads `../..` for a worktree under `<main>/.git/worktrees/`.
            _rs_common=$(head -1 "$_rs_gd/commondir")
            case "$_rs_common" in
                /*) ;;
                *)  _rs_common="$_rs_gd/$_rs_common" ;;
            esac
            if [ -d "$_rs_common" ]; then
                # `<main>/.git` -> `<main>`. `cd`+`pwd` rather than a text `dirname`,
                # so `../..` is collapsed by the filesystem and not by string surgery.
                _rs_main=$(cd "$_rs_common/.." 2>/dev/null && pwd)
                if [ -n "$_rs_main" ] && [ -d "$_rs_main/.secrets" ]; then
                    printf '%s\n' "$_rs_main/.secrets"
                    return 0
                fi
            fi
        fi
    fi

    # Nothing resolved: hand back the caller's own path so its fail-loud message names
    # the directory the operator would actually create.
    printf '%s\n' "$_rs_repo/.secrets"
    return 0
}

# ── dispatch, so this file is BOTH sourceable and runnable ───────────────────
case "${1:-}" in
    "") ;;                       # sourced: define the function and do nothing
    *)  repo_secrets_dir "$1" ;;
esac
