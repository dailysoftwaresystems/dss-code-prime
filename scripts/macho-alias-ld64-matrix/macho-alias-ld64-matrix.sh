#!/usr/bin/env bash
# PURPOSE: measure what Apple's ld64 does with a SECOND defined symbol at the same address as a canonical one, with and without -dead_strip.
#
# WHY THIS EXISTS. DSS's Mach-O object writer declares MH_SUBSECTIONS_VIA_SYMBOLS
# in its object schema, which is a LICENCE for ld64 to dead-strip at SYMBOL
# granularity. The moment a writer emits an ALIAS -- a second defined symbol at
# the same address as a canonical one (D-LK-ALIAS-NAME-ABSENT-FROM-REEMITTED-OBJECT-SYMTAB)
# -- ld64 either treats it as another name for the same atom
# (benign) or mints a ZERO-LENGTH atom at that address (dangerous: a reference
# that reaches only the alias keeps the zero-length atom while the body is
# stripped -- perfect bytes, wrong program).
#
# DSS has no opinion to consult. The writer copies `MachOIdentity::flags`
# verbatim and no C++ in the tree reads the value. An unmeasured belief about a
# loader is treated here as a defect, so this measures it.
#
# ★ rc=0 IS NOT AN OBSERVABLE. Every cell requires: both names present in the
#   LINKED IMAGE, both resolving to the SAME address, and the program RUNNING
#   and returning 42. A stripped body behind a zero-length atom links fine.
#
# ★ ATTRIBUTION CONTROL. The same matrix runs over a CLANG-authored alias
#   (`.globl` + `.set`). If a cell fails there too, the behaviour belongs to
#   ld64, not to DSS's emission. (P22's lesson: a reference control must match
#   the target.)
#
# ★ NO `.ps1` TWIN, AND THIS LINE IS THE DECLARATION THE PAIRING RULE ASKS FOR.
# Execution is POSIX-ONLY BY NATURE: every command that produces a measurement
# here -- clang, ld64, nm, otool, and the running binary -- executes in a shell
# ON THE MAC, and the mac-side half (`.remote.sh`) is a POSIX script by
# necessity. The local half is argument marshalling and base64, nothing more, so
# a PowerShell twin would be a second implementation of the ssh carriage rather
# than a second way to take the measurement. An operator on PowerShell reaches
# the same host through `scripts/ssh-macos/ssh-macos.ps1`.
# ⚠ This is a MEASUREMENT INSTRUMENT, not a gate: no ctest entry depends on it,
# so no leg loses coverage by its absence from a Windows shell.
#
# Usage: scripts/macho-alias-ld64-matrix/macho-alias-ld64-matrix.sh
# Exits non-zero if any cell fails. Needs the macOS carriage to be UP; a
# connect failure is the EXPECTED state (the Mac is a personal machine, usually
# off) and is reported as such rather than as a measurement.
#
# ⚠ TRANSPORT. The mac-side half is a sibling file, base64'd into ONE ssh
# argument. That is deliberate: `ssh-macos.sh --rsync` needs a real rsync (absent
# from Git Bash), and ssh joins its remaining arguments with spaces, so anything
# carrying quotes, pipes or redirection has to arrive as a single argument.
# base64 is quote-free by construction, which removes the whole class.
set -uo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$HERE/../.." && pwd)
CARRIAGE=$REPO/scripts/ssh-macos/ssh-macos.sh
REMOTE=$HERE/macho-alias-ld64-matrix.remote.sh

[ -f "$REMOTE" ] || { echo "missing $REMOTE" >&2; exit 2; }

b64=$(base64 -w0 < "$REMOTE" 2>/dev/null || base64 < "$REMOTE" | tr -d '\n')

"$CARRIAGE" "printf %s '$b64' | base64 -d > /tmp/dss-macho-alias-matrix.sh && bash /tmp/dss-macho-alias-matrix.sh"
rc=$?
if [ $rc -eq 255 ]; then
    echo "macho-alias-ld64-matrix: could not reach the macOS carriage (rc 255)." >&2
    echo "That is the EXPECTED state when the Mac is off. NOT a measurement." >&2
fi
exit $rc
