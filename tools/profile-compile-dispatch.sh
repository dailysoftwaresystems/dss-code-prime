#!/usr/bin/env bash
# profile-compile-dispatch.sh — ship the profiling kit + this checkout's runner to
# ONE remote leg and run it there.
#
# ★ WHY A SEPARATE FILE FROM profile-compile.sh: these are two different roles.
# profile-compile.sh runs ON the leg and knows nothing about any other machine;
# this runs on the CONTROL host and knows only about carriage. Folding the second
# into the first would make every leg carry a copy of the fleet's topology, which
# is how a host-keyed assumption gets into a target-keyed tool.
#
# ⚠ RUN THIS FROM WSL. The ssh carriage (tools/ssh-arm64-vps.sh,
# tools/ssh-macos.sh) works only there: `ssh dss` resolves nowhere from Windows,
# the key exists only inside WSL, and the .ps1 twins of those carriage scripts are
# known-broken. That is a property of this fleet's credentials, not of this tool.
#
# Usage:  profile-compile-dispatch.sh <wsl|vps|mac> [--target SPEC] [--kit DIR]
#
# The kit is COPIED to each leg rather than rebuilt there — see profile-compile.sh
# for why re-staging would silently change the subject.
set -uo pipefail

WHICH="${1:-}"; shift || true
die() { printf '\n[X] profile-compile-dispatch: %s\n' "$*" >&2; exit 1; }
[[ -n "$WHICH" ]] || die "which leg? one of: wsl vps mac"

# This checkout is the source of both the kit and the runner, so a leg can never
# run a runner from a different commit than the one dispatching it.
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET='x86_64:elf64-x86_64-linux-exec'   # ONE target for every leg: the HOST is
                                          # the variable, so the target must not be
KIT="$REPO/build/perf/kit"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target) TARGET="${2:?}"; shift 2 ;;
    --kit)    KIT="${2:?}"; shift 2 ;;
    *) die "unknown argument '$1'" ;;
  esac
done
[[ -d "$KIT" ]] || die "no kit at $KIT — build one with:
      python3 tools/profile-compile-support.py kit --manifest <a .dss-project.json> \\
              --root stage=<staged sources> --root libs=<harness-libs> --out $KIT"

RUNNER="$REPO/tools/profile-compile.sh"
SUPPORT="$REPO/tools/profile-compile-support.py"
GATE="$REPO/tools/run-gate.sh"
for f in "$RUNNER" "$SUPPORT" "$GATE"; do [[ -f "$f" ]] || die "missing $f"; done

# ⓘ The remote checkouts are where they are; these are the fleet's actual paths,
# not a guess. Each leg's repo already exists (the runner BUILDS the compiler
# there, it does not clone).
case "$WHICH" in
  wsl) SSH=""                            RREPO="$HOME/src/dss-code-prime" ;;
  vps) SSH="$REPO/tools/ssh-arm64-vps.sh" RREPO="src/Github/dss-code-prime" ;;
  mac) SSH="$REPO/tools/ssh-macos.sh"     RREPO="src/dss-code-prime" ;;
  *)   die "unknown leg '$WHICH' (wsl|vps|mac)" ;;
esac
LABEL="$WHICH-$( [[ "$WHICH" == "vps" || "$WHICH" == "mac" ]] && echo arm64 || echo x86_64 )"

if [[ -z "$SSH" ]]; then
  # The local POSIX leg. It still gets the kit COPIED into its own tree rather
  # than reading the Windows one across /mnt/c: that mount is ~10x slower, and a
  # subject read across it would charge the filesystem to the host.
  [[ -d "$RREPO" ]] || die "$RREPO does not exist on this machine"
  mkdir -p "$RREPO/build/perf"
  rsync -a --delete "$KIT/" "$RREPO/build/perf/kit/" || die "rsync of the kit failed"
  exec bash "$RREPO/tools/profile-compile.sh" --repo "$RREPO" \
       --kit "$RREPO/build/perf/kit" --target "$TARGET" --label "$LABEL"
fi

[[ -x "$SSH" ]] || die "carriage script $SSH is not executable (run this from WSL)"
echo "--- staging the kit onto $WHICH ($RREPO) ---"
"$SSH" "mkdir -p ~/$RREPO/build/perf" || die "cannot create the remote perf dir"
"$SSH" --rsync "$KIT/" "$RREPO/build/perf/kit/" || die "rsync of the kit failed"
# ★ THE RUNNER AND ITS SUPPORT GO OVER TOO, from THIS checkout. A leg running its
# own older copy would be a second implementation of the contract by the back
# door — the precise failure this tool was promoted out of.
"$SSH" --rsync "$RUNNER"  "$RREPO/tools/" || die "rsync of the runner failed"
"$SSH" --rsync "$SUPPORT" "$RREPO/tools/" || die "rsync of the support module failed"
"$SSH" --rsync "$GATE"    "$RREPO/tools/" || die "rsync of run-gate.sh failed"
exec "$SSH" "bash \$HOME/$RREPO/tools/profile-compile.sh --repo \$HOME/$RREPO \
     --kit \$HOME/$RREPO/build/perf/kit --target '$TARGET' --label '$LABEL'"
