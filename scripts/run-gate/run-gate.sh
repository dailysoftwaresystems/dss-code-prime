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

# ── WHICH SHELL IS ACTUALLY RUNNING THIS, NAMED IN EVERY REFUSAL ────────────
#
# ★★ A GATE THAT REFUSES MUST SAY WHICH REFUSAL IT IS. This wrapper's whole
# value is that its verdict is trustworthy; a refusal that misattributes its own
# cause spends that trust on a wild-goose chase.
#
# ✔MEASURED 2026-08-20 on the Windows workstation: from a WINDOWS-NATIVE parent
# process, `bash` resolves to `C:\WINDOWS\system32\bash.exe` — that is **WSL's**
# /bin/bash, NOT Git Bash — and it cannot open a `C:/...` path at all. Two of
# this script's refusals were anonymous about that:
#   * `bash run-gate.sh C:/x/y.log …`  -> exit 2,   "cannot write log", no log;
#   * `bash run-gate.sh out.log … cmd` -> exit 127, "cmd: command not found".
# Neither said WHICH bash it was, so both read as "the gate refused the run"
# when what happened was "the wrong bash ran". Two lanes in one cycle lost time
# to a 127 from this script with two indistinguishable causes.
#
# ⚠ AND THERE IS A THIRD SHAPE THIS SCRIPT CANNOT IMPROVE — stated here so the
# next reader stops looking for it inside the file. `bash C:/…/run-gate.sh …`
# also exits **127 with NO log**, and the message is `/bin/bash: C:/…: No such
# file or directory`: /bin/bash never opened THIS FILE, so no line of it runs
# and no diagnostic it contains can possibly be reached. ✔MEASURED the same day.
# That one is fixed at the CALL SITE — hand bash a path the bash you invoked can
# see (a repo-relative one works from either bash, because WSL translates the
# inherited cwd).
#
# ⓘ THIS NAMES, IT DOES NOT TRANSLATE. Turning `C:/x` into `/mnt/c/x` here would
# make this file a second path canonicaliser, which is exactly what
# `scripts/check-path-identity` exists to refuse. The refusal stands; it just
# stops being anonymous.
run_gate_shell_identity() {
    _rg_sh="${BASH:-<not bash>}"
    _rg_os="$(uname -s 2>/dev/null || echo '<uname unavailable>')"
    _rg_rel="$(uname -r 2>/dev/null || echo '')"
    # NAMED, never branched on: `-microsoft-standard-WSL2` in the kernel release
    # is how a WSL bash identifies itself, and the reader is the one who decides
    # what that means for the path they handed it.
    case "$_rg_rel" in *[Mm]icrosoft*) _rg_os="$_rg_os (a WSL distro: $_rg_rel)" ;; esac
    printf '%s on %s' "$_rg_sh" "$_rg_os"
}
# A path this shell was handed that begins with a DOS drive letter. Reported,
# not repaired — see the note above.
run_gate_looks_like_a_windows_path() {   # <path>
    case "$1" in [A-Za-z]:[/\\]*) return 0 ;; *) return 1 ;; esac
}

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
# ⚠ `{ …; } 2>/dev/null` and NOT `: > "$log" 2>/dev/null`. Redirections are set up
# LEFT TO RIGHT, so in the second form the failing `>` reports to the ORIGINAL
# stderr before `2>` is ever established — ✔MEASURED: bash's raw
# `line NNN: C:/…: No such file or directory` printed AHEAD of the named refusal
# below, which is the anonymous noise this whole block exists to replace. The
# group form establishes the group's stderr first, so only our sentence survives.
if ! { : > "$log"; } 2>/dev/null; then
    echo "run-gate.sh: FAIL — cannot create the log '$log', so nothing was run." >&2
    echo "  This refusal is about the LOG PATH, not about the gate command." >&2
    echo "  shell   : $(run_gate_shell_identity)" >&2
    echo "  script  : $0" >&2
    echo "  cwd     : $(pwd)" >&2
    if run_gate_looks_like_a_windows_path "$log"; then
        echo "  ⚠ that log path starts with a DOS DRIVE LETTER, and the shell named above is the" >&2
        echo "    one that has to open it. A WSL bash cannot see 'C:\\…' at all (its view of that" >&2
        echo "    volume is '/mnt/c/…'), and from a Windows-native parent a bare \`bash\` resolves to" >&2
        echo "    C:\\WINDOWS\\system32\\bash.exe — WSL's, not Git Bash's. Hand this script a path the" >&2
        echo "    bash you actually invoked can see; a repo-relative path works from either." >&2
        echo "    This script deliberately does NOT rewrite the path for you: one canonicaliser," >&2
        echo "    see scripts/check-path-identity." >&2
    else
        echo "  Check that the parent directory exists and is writable by this shell." >&2
    fi
    exit 2
fi

"$@" >>"$log" 2>&1
rc=$?

{
    echo "--- run-gate.sh ---"
    echo "command : $*"
    echo "rc      : $rc"
} >> "$log"

# ★ 127 IS ITS OWN REFUSAL, AND IT SAYS SO. rc 127 from a POSIX shell means the
# COMMAND WAS NOT FOUND — the gate never started, which is a categorically
# different fact from "the gate ran and failed". Reported under the generic
# heading it read as the latter, and the most common cause here is not a typo
# but the wrong bash: a Windows-only command (cmd, ctest.exe, a .bat) handed to
# WSL's /bin/bash. Same exit code, same fail-closed behaviour; only the sentence
# changes.
if [ "$rc" -eq 127 ]; then
    echo "run-gate.sh: FAIL — the gate command was NOT FOUND, so it never ran (rc=127)." >&2
    echo "  command : $*" >&2
    echo "  shell   : $(run_gate_shell_identity)" >&2
    echo "  This is NOT 'the gate failed' and NOT 'the witness was missing' — the shell named" >&2
    echo "  above could not resolve argv[0] on ITS OWN PATH. From a Windows-native parent a bare" >&2
    echo "  \`bash\` is WSL's /bin/bash, which has no cmd/.exe/.bat and its own PATH; from Git Bash" >&2
    echo "  it is MSYS's, which has no Linux distro packages. Check which of the two you wanted." >&2
    echo "  ⓘ A POSIX shell RESERVES 127 for 'not found', so a child that itself exited 127 is" >&2
    echo "    indistinguishable from one that never started — at this layer, not in this script." >&2
    echo "    The .ps1 twin CAN tell them apart (it resolves argv[0] first) and says which it is." >&2
    echo "  (log: $log)" >&2
    tail -20 "$log" >&2
    exit "$rc"
fi

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
