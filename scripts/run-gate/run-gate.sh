#!/usr/bin/env bash
# PURPOSE: run a gate command and REFUSE to report success without evidence that it ran.
# run-gate.sh — run a gate command and REFUSE to report success without evidence
# that it actually ran.
#
# ★★★ WHY THIS EXISTS: a gate reporting exit 0 that never executed has now
# happened THREE times in this project's record, each time with a different
# mechanism and each time caught only by a human reading the log:
#   1. a test suite printing `failed=0` while exiting 2 (weeks undetected);
#   2. a probe whose rc was read AFTER a pipe, so the pipe's status was reported;
#   3. `cd build-dbg && ctest ... ; echo "RC=$?"` — the `cd` failed because the
#      shell was already there, and the TRAILING `echo` succeeded, so the whole
#      chain exited 0 having run no tests at all.
# Vigilance is the wrong mechanism for a recurring failure: the previous two
# occurrences each produced a resolution to be careful, and the third happened
# anyway. This converts "remember to read the log" into "the gate cannot report
# success without evidence".
#
# ★★ THE CONTRACT, AND BOTH HALVES ARE LOAD-BEARING:
#   * rc is captured DIRECTLY from the command, never after a pipe and never
#     from a following statement — `$?` belongs to whatever ran last, which is
#     precisely how occurrence (3) happened;
#   * rc == 0 is NOT sufficient. The output must ALSO match a caller-supplied
#     success witness (e.g. "tests passed"). A command that exits 0 without
#     producing its own evidence of work is treated as a FAILURE, because that
#     is indistinguishable from not having run.
#
# Usage:
#   scripts/run-gate/run-gate.sh <log-path> <success-regex> <command> [args...]
#
# Example:
#   scripts/run-gate/run-gate.sh /tmp/ctest.log '100% tests passed' \
#       ctest --test-dir build/dbg --output-on-failure
set -u

if [ "$#" -lt 3 ]; then
    echo "run-gate.sh: usage: <log-path> <success-regex> <command> [args...]" >&2
    exit 2
fi

log="$1";     shift
witness="$1"; shift

# ── DEFAULT TEST PARALLELISM ────────────────────────────────────────────────
# ★★★ WHY THIS IS HERE AND NOT IN THE CALLER'S COMMAND LINE. This wrapper runs
# an ARBITRARY command, so splicing `-j 8` into someone else's argv would be
# wrong for every gate that is not ctest and could collide with a caller's own
# flag. `CTEST_PARALLEL_LEVEL` is ctest's own channel for the same fact: it is
# ignored by everything else, and an explicit `-j` on the command line still
# beats it, so this is a DEFAULT rather than a policy.
#
# ✔MEASURED 2026-08-19 (ctest 4.3.2, 16C/32T host), six example tests:
#     no level given .............. 9741 ms   <- what every gate here was doing
#     CTEST_PARALLEL_LEVEL=8 ...... 2648 ms
#     explicit -j 8 ............... 2446 ms   <- the env var is honoured
#     CTEST_PARALLEL_LEVEL=8 -j 1 . 9669 ms   <- an explicit flag still wins
# The full Windows suite measured 899 tests / 2602 s with no level at all, of
# which ONE test (`integrated_tests`) is 566 s -- so the suite's floor under any
# parallelism is that single test, and everything above it was pure waiting.
#
# ★ 8, not "all cores": operator instruction 2026-08-19. This project runs the
# Windows ctest leg and a WSL leg CONCURRENTLY on the same machine on purpose
# (serializing them was rejected by name), so the default leaves headroom for
# the other leg instead of claiming the box.
: "${CTEST_PARALLEL_LEVEL:=8}"
export CTEST_PARALLEL_LEVEL

# ★★ AND ACROSS THE WSL BOUNDARY, which an export alone does NOT cross.
# Windows->WSL forwards only the variables `WSLENV` names, so a gate invoked
# as `run-gate.sh … wsl.exe -e ctest …` ran SERIALLY while this script
# believed it had set the level -- ✔MEASURED by audit: the WSL child read it
# UNSET. Appending is deliberate; overwriting WSLENV would silently drop
# whatever the caller was already forwarding. Harmless off Windows, where
# nothing reads WSLENV.
case ":${WSLENV:-}:" in
    *:CTEST_PARALLEL_LEVEL:*) ;;
    *) export WSLENV="${WSLENV:+${WSLENV}:}CTEST_PARALLEL_LEVEL" ;;
esac

# ⓘ WHAT THIS STILL DOES NOT REACH, stated rather than left to be discovered:
# an `ssh` child. ssh forwards no environment without `SendEnv`/`AcceptEnv`
# on both ends, so a gate run through the ssh-arm64-vps or ssh-macos carriage
# takes the REMOTE default, not this one. Set it there if it matters.

# Truncate rather than append: a stale log from a previous run is itself a way
# to "find" a success witness that this invocation never produced.
: > "$log" || { echo "run-gate.sh: cannot write log '$log'" >&2; exit 2; }

"$@" >>"$log" 2>&1
rc=$?

{
    echo "--- run-gate.sh ---"
    echo "command : $*"
    echo "rc      : $rc"
} >> "$log"

if [ "$rc" -ne 0 ]; then
    echo "run-gate.sh: FAIL — command exited $rc (log: $log)" >&2
    tail -20 "$log" >&2
    exit "$rc"
fi

if ! grep -qE "$witness" "$log"; then
    echo "run-gate.sh: FAIL — command exited 0 but its output never matched the" >&2
    echo "  success witness /$witness/, so there is NO EVIDENCE it did any work." >&2
    echo "  An exit code alone cannot distinguish 'passed' from 'never ran'." >&2
    echo "  (log: $log)" >&2
    tail -20 "$log" >&2
    exit 1
fi

echo "run-gate.sh: OK — rc=0 and the success witness /$witness/ was present."
exit 0
