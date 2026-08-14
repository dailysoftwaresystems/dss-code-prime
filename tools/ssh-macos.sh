#!/usr/bin/env bash
# ssh-macos.sh — reach the operator's physical macOS host. Sibling of ssh-macos.ps1;
# CAPABILITY-PAIRED (a change to one lands in the other, or the pair is broken).
#
# WHY. macOS is the ONE target with no off-Mac emulator: nothing on Windows or Linux runs
# a Mach-O. Every other leg is verifiable where it is built (pe64 natively, elf64 under
# WSL, arm64 under qemu); a Darwin artefact can only be proven on real hardware. This is
# that carriage, made reproducible instead of living in one person's shell history.
#
# ★★ PERSONAL MACHINE, NOT CI. It is usually OFF. Never assume it is reachable, never wake
#    it, never run against it without the operator saying it is on. Failure to connect is
#    the EXPECTED state, not something to route around.
# ★ NO HOST DETAILS ARE TRACKED. Precedence: env > .secrets/macos.env > FAIL LOUD.
#   `.secrets/` is gitignored because this repo is slated to go public (PR #37).
# ★ KEY-BASED ONLY, DELIBERATELY. `BatchMode=yes` makes ssh FAIL rather than sit at a
#   password prompt. No password is accepted, stored or forwarded here — a password in a
#   repo script is a committed credential, and password auth cannot be automated without
#   putting the secret in the environment. Put the public key in the Mac's
#   ~/.ssh/authorized_keys; the operator performs that step, so no secret passes through
#   the tooling at all.
# ★ HOST-KEY POLICY matches the operator's original (StrictHostKeyChecking=no +
#   UserKnownHostsFile=/dev/null) because the Mac is a DHCP LAN host whose address moves.
#   STATED, not hidden: that forgoes MITM protection — defensible on a home LAN, not on an
#   untrusted network. Set DSS_MACOS_STRICT_HOSTKEY=1 there.
#
# Usage: tools/ssh-macos.sh                # interactive
#        tools/ssh-macos.sh uname -m       # run a command, exit with ITS status
set -uo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
CONF=$REPO/.secrets/macos.env

# ★ ENV MUST WIN OVER THE CONFIG FILE, AND THAT TAKES DELIBERATE CODE. `.` sources
# the file INTO THIS SHELL, so a bare `. "$CONF"` OVERWRITES whatever the caller put
# in the environment — the exact reverse of the precedence stated at the top of this
# file. ⚠ MEASURED 2026-08-13: `DSS_MACOS_HOST=<ip> tools/ssh-macos.sh` was silently
# ignored, and the script still tried to resolve the `.local` name from the config.
# That is the single worst place for this bug to live: the override it defeats is the
# one THIS SCRIPT'S OWN resolve-failure message instructs you to use ("Put a literal
# IP in DSS_MACOS_HOST"), on the WSL path where the same message says mDNS is expected
# to fail. So the documented workaround for the documented failure could not work.
# The PowerShell sibling always had this right (`if ($env:DSS_MACOS_HOST) {...} else
# {$conf[...]}`) — this is the CAPABILITY PAIR having drifted, which is precisely what
# the header's pairing note exists to prevent.
_envHost=${DSS_MACOS_HOST:-} ; _envUser=${DSS_MACOS_USER:-} ; _envKey=${DSS_MACOS_KEY:-}
# shellcheck disable=SC1090
[ -f "$CONF" ] && . "$CONF"
if [ -n "$_envHost" ]; then DSS_MACOS_HOST=$_envHost ; fi
if [ -n "$_envUser" ]; then DSS_MACOS_USER=$_envUser ; fi
if [ -n "$_envKey"  ]; then DSS_MACOS_KEY=$_envKey   ; fi
unset _envHost _envUser _envKey

: "${DSS_MACOS_HOST:=}" ; : "${DSS_MACOS_USER:=}" ; : "${DSS_MACOS_KEY:=}"
DSS_MACOS_KEY=$(eval printf '%s' "\"$DSS_MACOS_KEY\"")

if [ -z "$DSS_MACOS_HOST" ] || [ -z "$DSS_MACOS_USER" ]; then
    {
      echo "ssh-macos: connection data missing."
      echo "  Create $CONF with DSS_MACOS_HOST, DSS_MACOS_USER and DSS_MACOS_KEY (a key PATH)."
    } >&2
    exit 3
fi

# `.local` is mDNS/Bonjour: resolves on macOS and on Linux WITH avahi, and very often NOT
# from WSL, which has no mDNS responder. That is a real cross-host difference, so the
# diagnostic names the override rather than leaving it to be discovered.
target=$DSS_MACOS_HOST
if ! printf '%s' "$DSS_MACOS_HOST" | grep -qE '^[0-9]{1,3}(\.[0-9]{1,3}){3}$'; then
    resolved=$(getent hosts "$DSS_MACOS_HOST" 2>/dev/null | awk '{print $1; exit}')
    [ -z "$resolved" ] && resolved=$(ping -c1 -W1 "$DSS_MACOS_HOST" 2>/dev/null \
        | sed -n 's/.*(\([0-9.]\{7,15\}\)).*/\1/p' | head -1)
    if [ -z "$resolved" ]; then
        {
          echo "ssh-macos: cannot resolve '$DSS_MACOS_HOST' from this host."
          echo "  Most likely: the Mac is OFF or asleep — personal machine, not CI. Ask before waking it."
          echo "  Or:          no mDNS responder here (usual under WSL). Put a literal IP in DSS_MACOS_HOST."
        } >&2
        exit 3
    fi
    target=$resolved
fi

args=(-o ConnectTimeout=10 -o BatchMode=yes)
[ -n "$DSS_MACOS_KEY" ] && [ -f "$DSS_MACOS_KEY" ] && args+=(-i "$DSS_MACOS_KEY")
[ "${DSS_MACOS_STRICT_HOSTKEY:-0}" != "1" ] && \
    args+=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
args+=("$DSS_MACOS_USER@$target")
[ $# -gt 0 ] && args+=("$@")

ssh "${args[@]}"
exit $?
