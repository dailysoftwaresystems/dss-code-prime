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
# shellcheck disable=SC1090
[ -f "$CONF" ] && . "$CONF"

: "${DSS_VPS_HOST:=}" ; : "${DSS_VPS_USER:=}" ; : "${DSS_VPS_KEY:=}"
DSS_VPS_KEY=$(eval printf '%s' "\"$DSS_VPS_KEY\"")   # allow $HOME in the config

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

args=(-i "$DSS_VPS_KEY" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=25
      -o ServerAliveInterval=30 -o BatchMode=yes "$DSS_VPS_USER@$DSS_VPS_HOST")
[ $# -gt 0 ] && args+=("$@")
ssh "${args[@]}"
exit $?
