#!/usr/bin/env bash
# ssh-arm64-vps.sh — reach the native aarch64 Linux VPS. Sibling of ssh-arm64-vps.ps1;
# CAPABILITY-PAIRED (a change to one lands in the other, or the pair is broken).
#
# WHY. Every arm64 result in this project used to come from qemu, which says nothing
# about real hardware. This box is native aarch64 Ubuntu and is where the `.sh` driver
# was first exercised end-to-end (2026-08-04: 331,330 tests, 1 known non-DSS confound,
# elf64-arm64 running NATIVELY). It is also a genuinely third host for the
# de-host-locking property: the leg SET must match Windows and WSL; only RUN verdicts differ.
#
# ★ NO HOST DETAILS ARE TRACKED. Precedence: CLI env > .secrets/arm64-vps.env > FAIL LOUD.
#   `.secrets/` is gitignored because this repo is slated to go public (PR #37).
# ★ KEY-BASED ONLY. `BatchMode=yes` makes ssh FAIL rather than sit at a password prompt.
#   This script never accepts, stores or forwards a password, and no key material lives
#   in the repo — `.secrets/` holds the key's PATH, never the key.
#
# Usage: tools/ssh-arm64-vps.sh              # interactive
#        tools/ssh-arm64-vps.sh uname -m     # run a command, exit with ITS status
set -uo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
CONF=$REPO/.secrets/arm64-vps.env

# ★ ENV MUST WIN OVER THE CONFIG FILE — see the twin block in `ssh-macos.sh` for the
# measurement. `.` sources into THIS shell, so a bare `. "$CONF"` overwrites what the
# caller exported, reversing the precedence stated at the top of this file. Here the
# contradiction is written three lines apart: the failure message below says "or set
# those in the environment", which the sourcing made impossible. Fixed in both `.sh`
# scripts together because they are one capability pair with the `.ps1` siblings, and
# the siblings were already correct.
_envHost=${DSS_VPS_HOST:-} ; _envUser=${DSS_VPS_USER:-} ; _envKey=${DSS_VPS_KEY:-}
# shellcheck disable=SC1090
[ -f "$CONF" ] && . "$CONF"
if [ -n "$_envHost" ]; then DSS_VPS_HOST=$_envHost ; fi
if [ -n "$_envUser" ]; then DSS_VPS_USER=$_envUser ; fi
if [ -n "$_envKey"  ]; then DSS_VPS_KEY=$_envKey   ; fi
unset _envHost _envUser _envKey

: "${DSS_VPS_HOST:=}" ; : "${DSS_VPS_USER:=}" ; : "${DSS_VPS_KEY:=}"
DSS_VPS_KEY=$(eval printf '%s' "\"$DSS_VPS_KEY\"")   # allow $HOME in the config

# ★★★ THE SCRIPT FINDS THE KEY. THE CALLER NEVER DOES ANYTHING MANUALLY.
# Operator instruction 2026-08-17: "you must be able to get everything from the
# tool scripts, which goes inside .secrets and find the key. never manually."
#
# ⚠ WHY A REPO-RELATIVE KEY IS THE ONLY FORM THAT WORKS EVERYWHERE, measured:
# `$HOME` names a DIFFERENT directory in each shell this repo is driven from —
# `/home/<user>` in WSL, `/c/Users/<user>` in Git Bash, `C:\Users\<user>` in
# PowerShell. ✔MEASURED 2026-08-17: the key existed ONLY under WSL's $HOME, so a
# `$HOME`-relative config made this carriage work from WSL and fail from the other
# two with "no private key at …" — a host reachable or not depending on which
# shell you happened to start in. `$REPO/.secrets/` is the SAME directory in all
# three, so resolving here removes the shell from the answer.
# `.secrets/` is gitignored, so key material never reaches a commit.
#
# Precedence, and the order is deliberate: an explicit DSS_VPS_KEY that EXISTS
# always wins (a caller pointing at a specific key means it); otherwise the
# repo-local key is used; only then do we fail. A non-existent explicit path does
# NOT silently fall through to a different key — that would answer a question the
# caller did not ask — but it DOES fall through to discovery, and says so.
_repo_key=$REPO/.secrets/arm64-vps.key
if [ -n "$DSS_VPS_KEY" ] && [ ! -f "$DSS_VPS_KEY" ] && [ -f "$_repo_key" ]; then
    echo "ssh-arm64-vps: DSS_VPS_KEY='$DSS_VPS_KEY' does not exist; using the repo-local key $_repo_key" >&2
    DSS_VPS_KEY=$_repo_key
elif [ -z "$DSS_VPS_KEY" ] && [ -f "$_repo_key" ]; then
    DSS_VPS_KEY=$_repo_key
fi
unset _repo_key

if [ -z "$DSS_VPS_HOST" ] || [ -z "$DSS_VPS_USER" ] || [ -z "$DSS_VPS_KEY" ]; then
    {
      echo "ssh-arm64-vps: connection data missing."
      echo "  Create $CONF with DSS_VPS_HOST, DSS_VPS_USER, DSS_VPS_KEY (a key PATH),"
      echo "  or set those in the environment. .secrets/ is gitignored on purpose."
    } >&2
    exit 3
fi
if [ ! -f "$DSS_VPS_KEY" ]; then
    # Name the cause: "permission denied" would send the reader hunting server-side.
    echo "ssh-arm64-vps: no private key at '$DSS_VPS_KEY' (DSS_VPS_KEY)." >&2
    exit 3
fi

# ★★ A KEY ON A WINDOWS DRIVE IS WORLD-READABLE TO WSL, AND ssh REFUSES IT.
# ✔MEASURED 2026-08-17: the repo lives on `/mnt/c`, whose DrvFs mount maps Windows
# ACLs to mode **0444** regardless of `icacls`, so from WSL ssh says
#   "WARNING: UNPROTECTED PRIVATE KEY FILE! … Permissions 0444 … are too open"
# and then fails as "Permission denied (publickey)" — a message that blames the
# SERVER for a LOCAL file-mode problem, which is exactly the wrong place to look.
# `chmod` cannot fix it: DrvFs ignores mode changes without the `metadata` mount
# option, so the fix has to be a private COPY rather than a permission change.
# ⇒ The script does this itself. The operator's rule is that the tooling resolves
# everything and the caller never does anything manually, so "chmod it yourself"
# is not an acceptable answer here — nor is it even possible on this mount.
if [ -r "$DSS_VPS_KEY" ]; then
    _mode=$(stat -c '%a' "$DSS_VPS_KEY" 2>/dev/null || echo '')
    case "$_mode" in
        ''|*00) : ;;                       # unknown, or already owner-only
        *)
            # Group or other bits are set. Try chmod first (works on a native fs);
            # only fall back to a private copy if the mode genuinely will not move.
            chmod 600 "$DSS_VPS_KEY" 2>/dev/null || true
            if [ "$(stat -c '%a' "$DSS_VPS_KEY" 2>/dev/null || echo 600)" != "600" ]; then
                _priv=$(mktemp "${TMPDIR:-/tmp}/.dss-vps-key.XXXXXX") || {
                    echo "ssh-arm64-vps: cannot create a private copy of the key." >&2; exit 3; }
                # Restrict BEFORE writing: a world-readable window, however brief, is
                # still a window.
                chmod 600 "$_priv" && cat "$DSS_VPS_KEY" > "$_priv" || {
                    rm -f "$_priv"; echo "ssh-arm64-vps: failed to stage a private key copy." >&2; exit 3; }
                trap 'rm -f "$_priv"' EXIT HUP INT TERM
                DSS_VPS_KEY=$_priv
            fi
            ;;
    esac
    unset _mode
fi

args=(-i "$DSS_VPS_KEY" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=25
      -o ServerAliveInterval=30 -o BatchMode=yes "$DSS_VPS_USER@$DSS_VPS_HOST")
[ $# -gt 0 ] && args+=("$@")
ssh "${args[@]}"
exit $?
