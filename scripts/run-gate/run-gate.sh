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
# ★ THE EXIT CODES ARE PART OF THE CONTRACT AND THE TWO TWINS MUST AGREE ON THEM:
#     0   rc was 0 AND the success witness was present
#     1   the command exited 0 but produced no witness — no evidence it ran
#     2   this wrapper refused before starting (bad usage, unwritable log/marker)
#     3   THE SOURCE TREE MOVED under the run — the verdict is not evidence
#     4   ANOTHER RUN WAS LIVE IN THE SAME BUILD DIRECTORY — likewise not evidence
#     *   otherwise, the command's own exit code (127 gets its own sentence)
#   ⚠ 3 and 4 are DELIBERATELY DIFFERENT NUMBERS. Both mean "this run has no
#   verdict", and a reader who cannot tell which of the two fired cannot tell
#   whether to settle the tree or wait for a sibling — two different remedies.
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
# ★★★ OPERATOR RULING 2026-08-25: "never use all CPUS, the idea is to keep build + tests + run always at 4 cpus", AMENDED same-day to "make it 6 cores, not 4, everywhere".
# The former 8 was a workstation number justified by "leaving headroom" for a
# concurrent leg. The ruling is stronger than headroom: never claim the box.
: "${CTEST_PARALLEL_LEVEL:=6}"
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

# ── DEFAULT: A FAILING TEST'S OWN OUTPUT GOES IN THE LOG ────────────────────
#
# ★★★ WHY THIS IS HERE. A gate that reds without saying WHY is a gate whose
# verdict cannot be acted on, and for a PROBABILISTIC red it is worse than that:
# the evidence is gone for good, because ctest's only other copy of a failing
# test's output is `<build>/Testing/Temporary/LastTest.log`, which the NEXT ctest
# run OVERWRITES. ✔MEASURED 2026-08-24 (P31): `ffi/test_c_header_parser` failed
# once at 8-way parallelism in a scoped gate; the confirming re-run four minutes
# later replaced `LastTest.log` with its own passing text, and the only surviving
# artefact was a 30-byte `LastTestsFailed.log` naming the test and nothing else.
# The flake had to be re-derived from scratch. The 30 bytes were the whole record
# of it.
#
# ★★ THE MECHANISM IS CTEST'S OWN ENV CHANNEL, NOT ARGV INJECTION, and that
# distinction is the same one the parallelism block above makes: this wrapper
# runs an ARBITRARY command, so splicing `--output-on-failure` into someone
# else's argv would be wrong for every gate that is not ctest.
# `CTEST_OUTPUT_ON_FAILURE` is ignored by everything that is not ctest, and an
# explicit flag on the command line still decides. ✔MEASURED (ctest 4.3.2,
# Windows) on a one-entry project whose only test prints a witness and exits 1:
#     no variable ................. the witness appears 0 times in ctest's stdout
#     CTEST_OUTPUT_ON_FAILURE=1 ... 1 time
#     --output-on-failure ......... 1 time
#
# ⓘ IT COSTS NOTHING ON A GREEN RUN — it prints only for tests that FAIL, so a
# passing gate's log is byte-identical to what it was before this block.
: "${CTEST_OUTPUT_ON_FAILURE:=1}"
export CTEST_OUTPUT_ON_FAILURE
case ":${WSLENV:-}:" in
    *:CTEST_OUTPUT_ON_FAILURE:*) ;;
    *) export WSLENV="${WSLENV:+${WSLENV}:}CTEST_OUTPUT_ON_FAILURE" ;;
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
# ── A LOG PATH THAT BEGINS WITH '-' IS REFUSED, BY NAME, BEFORE ANYTHING OPENS ──
#
# ★★★ THE OBSERVED FAILURE WAS SILENT REPO POLLUTION, NOT A USAGE ERROR.
# ✔MEASURED 2026-08-24 (P31): `run-gate.ps1 -LogPath <path> -SuccessRegex … -Command …`
# — named-parameter syntax, which this wrapper deliberately does NOT accept (the
# param block is empty ON PURPOSE; see the long comment above it in the .ps1, and
# the interface is POSITIONAL) — bound `-LogPath` as argv[0], and the wrapper then
# CREATED A FILE LITERALLY NAMED `-LogPath` IN THE REPO ROOT and wrote its refusal
# into it. Nothing said "you invoked this with named-parameter syntax". The caller
# reads a refusal about something else entirely and leaves a stray file behind.
#
# ★★ AND A LEADING '-' IS HOSTILE FAR BEYOND THIS SCRIPT. Every POSIX tool that
# later receives that path reads it as an OPTION: this file's own `grep -qE "$witness" "$log"`
# and `tail -20 "$log"` would parse it as flags, and so would every `rm`, `cat` or
# `cp` a reader reaches for afterwards.
#
# ★ THE RULE IS "FIRST CHARACTER IS '-'", AND THE NARROWER ONE WAS REJECTED.
# The obvious alternative is to match a PowerShell parameter SHAPE (`-[A-Za-z]…`).
# Rejected for two reasons: (1) it waves through `--output.log` and `-1.log`, which
# are exactly as hostile to the pipeline above — the defect is the leading dash, not
# the spelling after it; and (2) it would make BOTH twins reason about PowerShell's
# grammar, inside a file that also has to be right for POSIX. One rule, both shells,
# is what keeps the twins from disagreeing about what they accept. The refusal still
# NAMES the named-parameter case, because that is the one that actually happened.
#
# ⓘ THE ESCAPE IS THE STANDARD ONE and it works in both shells: spell it `./-name`.
# So a caller who genuinely wants such a file is not blocked, only slowed down.
case "$log" in
    -*)
        echo "run-gate.sh: FAIL — the log path '$log' begins with '-', so nothing was run." >&2
        echo "  This refusal is about the LOG PATH, not about the gate command." >&2
        echo "  shell   : $(run_gate_shell_identity)" >&2
        echo "  This wrapper's interface is POSITIONAL and it accepts NO named parameters:" >&2
        echo "      run-gate.sh <log-path> <success-regex> <command> [args...]" >&2
        echo "  If you meant '-LogPath'/'-SuccessPattern'/'-Command' as PowerShell named" >&2
        echo "  parameters, drop the names and pass the three values in that order — the" >&2
        echo "  .ps1 twin's param block is empty ON PURPOSE (declaring them breaks the" >&2
        echo "  argument pass-through it exists to preserve), so a name binds as a VALUE." >&2
        echo "  Refused rather than honoured because creating it would leave a stray file" >&2
        echo "  named '$log' behind, and every later tool that receives that path reads a" >&2
        echo "  leading '-' as an OPTION — including this script's own grep and tail." >&2
        echo "  If you really do want that filename, spell it './$log'." >&2
        exit 2
        ;;
esac

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

# ── THE RUN'S INPUTS MUST HOLD STILL, OR ITS VERDICT IS NOT EVIDENCE ────────
#
# ★★★ THE FOURTH WAY A GATE'S EXIT CODE CAN MEAN NOTHING, and unlike the three
# at the top of this file it does not need the gate to skip any work. This
# project's runners read `src/dss-config/**`, `tests/corpus/**` and `examples/**`
# from the SOURCE TREE at TEST TIME, not from the build directory. Edit one while
# a suite is in flight and the run measures a tree that never existed: some tests
# saw the old vocabulary, some the new, and the report names neither.
#
# ✔MEASURED 2026-09-05 (P62, and it is the reason this block exists). A whole-tree
# `ctest` reported 9 failures out of 2087. EIGHT of them were examples failing with
# `C_UnbackedPredefinedMacro` — "predefined macro '__MINGW32__' requires shipped
# header 'dirent.h' … but no descriptor for it is on the shipped-library search
# path" — which reads as a defect in the FFI/shipped-library work a sibling lane
# had just folded, and that is exactly where the investigation started. The real
# cause was the ORCHESTRATOR rewriting `src/dss-config/sources/c.lang.json` while
# the run was in flight. ✔All eight passed on the stable tree, unchanged, seconds
# later. The gate had no way to say so: a torn read of a config file is not a
# failure mode any assertion in the suite is written against.
#
# ★★ IT IS THE ORCHESTRATOR-LANE FORM OF A RULE THIS PROJECT ALREADY HAS —
# "a config edit under a running lane changes what its binaries MEAN; the
# orchestrator is a lane too". That rule was written down, and the edit happened
# anyway. Vigilance is the wrong mechanism for a recurring failure; this file
# already says so about its first three occurrences.
#
# ★ NO ESCAPE HATCH, DELIBERATELY. The obvious accommodation is an env var for
# "this gate legitimately rewrites config" — and an escape that every caller can
# set is an escape every caller sets, which refuses nothing. A command that
# rewrites these roots is a BUILD STEP, not a gate, and does not belong under a
# wrapper whose whole contract is that its verdict can be trusted.
#
# ⓘ MARKER + `find -newer`, not a hash: it needs no hashing tool at all (macOS
# has `md5`, Linux `md5sum`, and this script runs on both carriages), it is the
# same technique `scripts/local-build/local-build.sh` uses for the sibling
# question, and `-newer` is strictly-greater — which is the RIGHT direction here,
# because the marker is written BEFORE the run and an offending edit lands after.
# ⓘ A root that does not exist contributes nothing, so a lane worktree carrying a
# subset of the tree, or a synthetic self-test root, is not penalised for it.
# ✔MEASURED cost: 1971 files across the three roots, 0.31 s — against gates that
# run for a quarter of an hour.
# ── AND THE BUILD DIRECTORY MUST BE THIS RUN'S ALONE ────────────────────────
#
# ★★★ THE FIFTH WAY A GATE'S EXIT CODE CAN MEAN NOTHING, AND IT IS THE OTHER
# HALF OF THE BLOCK ABOVE. That one watches the SOURCE tree; a run can be lied
# to just as completely through the BUILD DIRECTORY, and nothing here could see
# it. D-GATE-RUN-GATE-BLIND-TO-A-SECOND-RUN-IN-THE-SAME-BUILD-DIRECTORY
#
# ✔MEASURED 2026-09-07 (P63, orchestrator, and it is the reason this block
# exists): `ctest --test-dir build/sh` was launched while a lane's own gate was
# ALREADY running against THAT SAME build directory. Result: **2100/2101, one
# red (`line_endings_guard`)** — and this wrapper printed `inputs : held still`.
# `Get-CimInstance Win32_Process` showed FOUR concurrent `ctest.exe`. Re-run in
# isolation that test PASSES. ⇒ the run had NO VERDICT, exactly like one taken
# over a moving source tree, and the instrument said it was fine.
# ⓘ It is the same family as the two OPEN rows that describe the contention
# itself from the other seat — [[D-GATE-SHARED-DLL-RELINK-UNDER-RUNNING-CTEST]]
# and [[D-BUILD-CONCURRENT-CMAKE-REGENERATE-IN-SHARED-BUILD-DIR]]. Those ask for
# the contention to be made impossible; this asks only that a gate STOP
# REPORTING A VERDICT while it is happening, which is this wrapper's job and not
# theirs.
#
# ★★ A SCAN, NOT A LOCK, AND THE CHOICE IS MEASURED. A marker file taken and
# released by this wrapper inside the build directory is race-free where a
# process scan is only a sample — but it sees ONLY runs that went through this
# wrapper, and ✔the run that caused the measurement above DID NOT: it was a bare
# `ctest --test-dir build/sh` typed at a shell. A lock would have been silent for
# precisely the case it was written for. The scan sees that run, so the scan is
# what ships. ✔MEASURED both ways in P63 with a two-arm probe: the lock detected
# a second run-gate run and was BLIND to a bare ctest; the scan saw both.
#
# ⚠ WHAT THE SCAN CANNOT SEE, stated here rather than left to be discovered:
#   · `cd <build> && ctest` — the directory is the process's WORKING DIRECTORY,
#     which is not readable from another process on Windows, and no flag names
#     it. Same for a bare `ninja` started inside the build directory with no
#     `-C`: ✔MEASURED, `cmake --build build/hg` spawns a child whose whole
#     command line is `ninja.exe -j 6`. That child is invisible; its `cmake`
#     PARENT is what this sees, and that parent lives for the whole build.
#   · a process whose command line cannot be read. ✔MEASURED on this host:
#     249 of 534 processes report an EMPTY `CommandLine` to `Win32_Process`
#     (protected/system processes), while ALL FOUR live build-tool processes
#     reported theirs. Unreadable CANDIDATES are COUNTED and NAMED in the log
#     and in the refusal — never silently dropped — but they cannot be judged.
#   · a contending run that starts after the pre-check and ends before the
#     post-check. That window is the price of a sample; it is why there are TWO
#     checks and not one.
#   · a run in another OS namespace (a WSL leg, a remote carriage). Those have
#     their own build trees and their own gates.
#
# ★ NO ESCAPE HATCH, and unlike an env var this one cannot be spelled at all.
# The check has a subject only when the gate command NAMES a build directory, so
# it is not something a caller opts out of — it is something the argv either
# says or does not. ✔COUNTED at the commit that added this, over every
# `run-gate` call site in the tree: `scripts/wsl-leg/wsl-leg.sh` reaches the
# refusal on BOTH of its two ctest invocations (`ctest --test-dir "$BUILD"`), as
# does every hand-run gate the cycle skill prescribes; `scripts/remote-leg`
# does NOT, because its argv[0] is `bash` and the build tree is on ANOTHER HOST
# where a local scan would be answering about the wrong machine. That is the
# check being directional, not an escape: two of three shipped call sites reach
# it and the third has no local subject to check.
run_gate_contention_exit=4
run_gate_build_dir=""
run_gate_contention_note=""

run_gate_is_windows() {
    case "$(uname -s 2>/dev/null || echo unknown)" in
        MINGW*|MSYS*|CYGWIN*) return 0 ;;
        *)                    return 1 ;;
    esac
}

# ONE SPELLING for a directory, so two processes' spellings can be compared at
# all. ⓘ This is NOT a general path canonicaliser and must not grow into one —
# see the note about `scripts/check-path-identity` further up this file. It
# normalises exactly three things and says so: separator, trailing slash, and
# (on Windows only, where the filesystem is case-insensitive) case.
run_gate_norm_dir() {   # <path>
    _rg_np="$(printf '%s' "$1" | tr '\\' '/')"
    while :; do
        case "$_rg_np" in
            ?*/) _rg_np="${_rg_np%/}" ;;
            *)   break ;;
        esac
    done
    if run_gate_is_windows; then
        _rg_np="$(printf '%s' "$_rg_np" | tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz')"
    fi
    printf '%s' "$_rg_np"
}

# A directory token made absolute in the spelling the OTHER processes on this
# host use. ⚠ `cygpath -m` is called rather than hand-rewriting `/c/x` into
# `C:/x`: MSYS's own translator is the authority for that mapping, and writing a
# second one here is what the canonicaliser note forbids.
# ⓘ A RELATIVE token is resolved against THIS shell's working directory, because
# a process's own working directory is not readable from outside on Windows.
# Every refusal below SAYS SO, so a reader can judge a match made that way.
run_gate_resolve_dir() {   # <path>
    _rg_rd="$(printf '%s' "$1" | tr '\\' '/')"
    _rg_ra="$(cd "$_rg_rd" 2>/dev/null && pwd -P)"
    if [ -z "$_rg_ra" ]; then
        case "$_rg_rd" in
            /*|[A-Za-z]:/*) _rg_ra="$_rg_rd" ;;
            *)              _rg_ra="$(pwd -P)/$_rg_rd" ;;
        esac
    fi
    if run_gate_is_windows && command -v cygpath >/dev/null 2>&1; then
        _rg_ra="$(cygpath -m "$_rg_ra" 2>/dev/null || printf '%s' "$_rg_ra")"
    fi
    run_gate_norm_dir "$_rg_ra"
}

# WHICH BUILD DIRECTORY DOES *THIS* GATE COMMAND NAME? Read from the real argv,
# so no tokenising and no guessing. ⚠ PROGRAM-KEYED, because the same letter
# means different things to different tools: `ctest -C Debug` is a CONFIGURATION
# and `ninja -C dir` is a DIRECTORY, and a rule that read `-C` for both would
# turn every `ctest -C Debug` into a build directory called `Debug`.
run_gate_dir_named_by_argv() {   # <argv...>
    _rg_prog="$(basename "${1:-}" 2>/dev/null || printf '%s' "${1:-}")"
    _rg_prog="$(printf '%s' "$_rg_prog" | tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz')"
    _rg_prog="${_rg_prog%.exe}"
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --test-dir)   [ "$#" -ge 2 ] && { printf '%s' "$2"; return 0; } ;;
            --test-dir=*) printf '%s' "${1#--test-dir=}"; return 0 ;;
            --build)      case "$_rg_prog" in cmake) [ "$#" -ge 2 ] && { printf '%s' "$2"; return 0; } ;; esac ;;
            -B)           case "$_rg_prog" in cmake) [ "$#" -ge 2 ] && { printf '%s' "$2"; return 0; } ;; esac ;;
            -B?*)         case "$_rg_prog" in cmake) printf '%s' "${1#-B}"; return 0 ;; esac ;;
            -C)           case "$_rg_prog" in ninja|make|gmake) [ "$#" -ge 2 ] && { printf '%s' "$2"; return 0; } ;; esac ;;
            -C?*)         case "$_rg_prog" in ninja|make|gmake) printf '%s' "${1#-C}"; return 0 ;; esac ;;
        esac
        shift
    done
    return 1
}

# pid <TAB> ppid <TAB> image <TAB> command-line, one row per live process.
# ✔MEASURED cost on this workstation: `powershell -NoProfile -NonInteractive`
# with the CIM query below returns 534 rows in 741–1086 ms (median 780 ms); the
# same query from an ALREADY-RUNNING PowerShell is 341 ms, so the spawn is most
# of it. `pwsh` was measured SLOWER to start (877–944 ms) and `Get-Process`
# slower still (1494–1575 ms), so `powershell` is preferred and `pwsh` is the
# fallback. Two checks per gate is ~1.6 s against gates that run for minutes.
run_gate_process_table() {
    if run_gate_is_windows; then
        _rg_pssh=""
        for _rg_c in powershell pwsh; do
            if command -v "$_rg_c" >/dev/null 2>&1; then _rg_pssh="$_rg_c"; break; fi
        done
        [ -n "$_rg_pssh" ] || return 1
        "$_rg_pssh" -NoProfile -NonInteractive -Command '$ErrorActionPreference="SilentlyContinue"; Get-CimInstance Win32_Process | ForEach-Object { $_.ProcessId.ToString() + "`t" + $_.ParentProcessId.ToString() + "`t" + $_.Name + "`t" + ($_.CommandLine -replace "`t", " ") }' 2>/dev/null | tr -d '\r'
    else
        ps -eo pid=,ppid=,comm=,args= 2>/dev/null |
            awk '{ p=$1; q=$2; c=$3; $1=""; $2=""; $3=""; sub(/^[ \t]+/, ""); printf "%s\t%s\t%s\t%s\n", p, q, c, $0 }'
    fi
}

# THIS shell's identity IN THE TABLE'S PID NAMESPACE. ⚠ On MSYS `$$` is the MSYS
# pid and the table above holds WINDOWS pids; `ps -e` prints both, and column 4
# is the WINPID. Without this the ancestor exclusion below would silently match
# nothing — which is the shape of a guard that runs and decides nothing.
run_gate_self_pid() {
    if run_gate_is_windows; then
        ps -e 2>/dev/null | awk -v p="$$" '$1 == p { print $4; exit }'
    else
        printf '%s' "$$"
    fi
}

# Candidates that name a build directory. Emitted as
#   DIR <TAB> pid <TAB> image <TAB> raw-token <TAB> command-line
#   UNREADABLE <TAB> pid <TAB> image
# ⚠ THE TOKENISER HONOURS DOUBLE QUOTES. ✔MEASURED: a Windows command line reads
# `"C:\Program Files\CMake\bin\cmake.exe" --build build/hg --parallel 6`, so a
# split on whitespace alone tears the program path in half.
run_gate_candidates_from_table() {
    awk -F'\t' '
        function tokenize(line,   i, c, n, inq, cur) {
            n = 0; inq = 0; cur = ""
            for (i = 1; i <= length(line); i++) {
                c = substr(line, i, 1)
                if (c == "\"") { inq = !inq; continue }
                if (!inq && (c == " " || c == "\t")) { if (cur != "") { T[++n] = cur; cur = "" }; continue }
                cur = cur c
            }
            if (cur != "") T[++n] = cur
            return n
        }
        {
            pid = $1; img = tolower($3); cl = $4
            sub(/\.exe$/, "", img)
            if (img != "ctest" && img != "ninja" && img != "cmake" && img != "make" && img != "gmake" && img != "msbuild") next
            if (cl == "") { print "UNREADABLE\t" pid "\t" img; next }
            n = tokenize(cl)
            for (i = 1; i <= n; i++) {
                a = T[i]; d = ""
                if      (a == "--test-dir" && i < n)                                        d = T[i+1]
                else if (a ~ /^--test-dir=/)                            { d = a; sub(/^--test-dir=/, "", d) }
                else if (img == "cmake" && a == "--build" && i < n)                         d = T[i+1]
                else if (img == "cmake" && a == "-B" && i < n)                              d = T[i+1]
                else if (img == "cmake" && a ~ /^-B./)                  { d = a; sub(/^-B/, "", d) }
                else if ((img == "ninja" || img == "make" || img == "gmake") && a == "-C" && i < n) d = T[i+1]
                else if ((img == "ninja" || img == "make" || img == "gmake") && a ~ /^-C./) { d = a; sub(/^-C/, "", d) }
                if (d != "") print "DIR\t" pid "\t" img "\t" d "\t" cl
            }
        }'
}

# The refusal text, shared by the pre-run and post-run arms so the two cannot
# drift into describing the same fact differently.
run_gate_contenders="" ; run_gate_unreadable=0 ; run_gate_table_ok=0 ; run_gate_relative_match=0
run_gate_scan_contention() {   # sets run_gate_contenders / run_gate_unreadable / run_gate_table_ok
    run_gate_contenders=""; run_gate_unreadable=0; run_gate_table_ok=0; run_gate_relative_match=0
    [ -n "$run_gate_build_dir" ] || return 0
    _rg_tbl="$(run_gate_process_table)" || return 0
    [ -n "$_rg_tbl" ] || return 0
    run_gate_table_ok=1

    # OUR OWN ANCESTORS ARE NOT CONTENDERS. A gate legitimately invoked from
    # inside a `ctest` (this repository registers guards that way) would
    # otherwise refuse itself the moment it named the same tree.
    _rg_self="$(run_gate_self_pid)"
    _rg_exclude=""
    if [ -n "$_rg_self" ]; then
        _rg_walk="$_rg_self"; _rg_depth=0
        while [ -n "$_rg_walk" ] && [ "$_rg_depth" -lt 24 ]; do
            _rg_exclude="$_rg_exclude $_rg_walk"
            _rg_walk="$(printf '%s\n' "$_rg_tbl" | awk -F'\t' -v p="$_rg_walk" '$1 == p { print $2; exit }')"
            _rg_depth=$((_rg_depth + 1))
        done
    else
        run_gate_contention_note="ancestry: UNRESOLVED (this shell's pid was not found in the process table; nothing was excluded)"
    fi

    _rg_cands="$(printf '%s\n' "$_rg_tbl" | run_gate_candidates_from_table)"
    run_gate_unreadable="$(printf '%s\n' "$_rg_cands" | grep -c '^UNREADABLE' || true)"
    while IFS='	' read -r _rg_kind _rg_pid _rg_img _rg_raw _rg_cl; do
        [ "${_rg_kind:-}" = "DIR" ] || continue
        case " $_rg_exclude " in *" $_rg_pid "*) continue ;; esac
        [ "$(run_gate_resolve_dir "$_rg_raw")" = "$run_gate_build_dir" ] || continue
        # Whether the RESOLUTION was an assumption is recorded per match, so the
        # refusal only makes that caveat when it actually applies — a message
        # that outruns its evidence is the shape this whole file is about.
        case "$(printf '%s' "$_rg_raw" | tr '\\' '/')" in
            /*|[A-Za-z]:/*) ;;
            *) run_gate_relative_match=1 ;;
        esac
        run_gate_contenders="${run_gate_contenders}      pid ${_rg_pid}  ${_rg_img}  named it as '${_rg_raw}'
        ${_rg_cl}
"
    done <<EOF
$_rg_cands
EOF
}

run_gate_refuse_contention() {   # <when>
    echo "run-gate.sh: FAIL — ANOTHER RUN IS LIVE IN THIS BUILD DIRECTORY, so this one has no verdict." >&2
    echo "  build dir: $run_gate_build_dir" >&2
    echo "  detected : $1" >&2
    echo "  These live processes name that same directory:" >&2
    printf '%s' "$run_gate_contenders" >&2
    echo "  ⚠ This is NOT 'the gate failed' and NOT 'the gate passed'. Two runs sharing one build" >&2
    echo "    directory rewrite each other's binaries and test artefacts mid-run: ✔MEASURED, a" >&2
    echo "    2100/2101 with ONE red that passed on a re-run in isolation." >&2
    if [ "$run_gate_relative_match" -eq 1 ]; then
        echo "  ⓘ At least one spelling above is RELATIVE, and this guard resolved it against THIS" >&2
        echo "    shell's working directory ($(pwd -P)) — a process's own working" >&2
        echo "    directory is not readable from outside on Windows. If that process is really" >&2
        echo "    standing in a DIFFERENT tree, this is a false refusal; read the command line" >&2
        echo "    printed above before assuming it is not." >&2
    fi
    echo "  Wait for the other run to finish, or point this gate at its own build directory." >&2
    echo "  (log: $log)" >&2
}

run_gate_input_roots="src/dss-config tests/corpus examples"
run_gate_marker="${log}.inputs-marker"
run_gate_moved_inputs() {
    [ -f "$run_gate_marker" ] || return 1
    # shellcheck disable=SC2086
    find $run_gate_input_roots -type f -newer "$run_gate_marker" 2>/dev/null | head -20
}
# ── PRE-RUN: refuse a contended build directory BEFORE anything starts ──────
# ⚠ Placed AHEAD of the input marker deliberately, so a refusal here leaves no
# marker file behind for the next run to trip over.
if run_gate_raw_build_dir="$(run_gate_dir_named_by_argv "$@")"; then
    run_gate_build_dir="$(run_gate_resolve_dir "$run_gate_raw_build_dir")"
fi
run_gate_scan_contention
if [ -n "$run_gate_contenders" ]; then
    {
        echo "--- run-gate.sh ---"
        echo "command : $*"
        echo "builddir: $run_gate_build_dir"
        echo "contended: YES, BEFORE THE RUN — nothing was executed"
        printf '%s' "$run_gate_contenders"
    } >> "$log"
    run_gate_refuse_contention "BEFORE the run started, so nothing was executed"
    exit "$run_gate_contention_exit"
fi

if ! { : > "$run_gate_marker"; } 2>/dev/null; then
    echo "run-gate.sh: FAIL — cannot create the input marker '$run_gate_marker', so the run" >&2
    echo "  could not be proved to have measured a still tree. Nothing was run." >&2
    echo "  This refusal is about the MARKER PATH, which sits beside the log path you gave." >&2
    echo "  shell   : $(run_gate_shell_identity)" >&2
    exit 2
fi

"$@" >>"$log" 2>&1
rc=$?

run_gate_moved="$(run_gate_moved_inputs)"
rm -f "$run_gate_marker"

# ── POST-RUN: a sibling can start MID-RUN, so the same question is asked again ──
run_gate_scan_contention

{
    echo "--- run-gate.sh ---"
    echo "command : $*"
    echo "rc      : $rc"
    if [ -n "$run_gate_moved" ]; then
        echo "inputs  : MOVED DURING THE RUN — this verdict is not evidence"
        echo "$run_gate_moved" | sed 's/^/          /'
    else
        echo "inputs  : held still ($run_gate_input_roots)"
    fi
    if [ -z "$run_gate_build_dir" ]; then
        echo "builddir: none named by this command — the contention check had no subject"
    else
        echo "builddir: $run_gate_build_dir"
        if [ -n "$run_gate_contenders" ]; then
            echo "contended: YES, ANOTHER RUN WAS LIVE IN IT — this verdict is not evidence"
            printf '%s' "$run_gate_contenders"
        elif [ "$run_gate_table_ok" -eq 1 ]; then
            echo "contended: no (this run was alone in it; $run_gate_unreadable candidate process(es) had no readable command line and could not be judged)"
        else
            echo "contended: UNKNOWN — no process table could be read on this host, so nothing was ruled out"
        fi
    fi
    [ -n "$run_gate_contention_note" ] && echo "$run_gate_contention_note"
} >> "$log"

# Checked BEFORE rc, and before the witness: a run whose inputs moved has no
# verdict to report, and saying "the gate failed" or "the gate passed" about it
# would be the misattribution this block exists to prevent.
if [ -n "$run_gate_moved" ]; then
    echo "run-gate.sh: FAIL — the tree CHANGED UNDER THE RUN, so its result is not evidence" >&2
    echo "  (command exited $rc; that number describes a tree that never existed as a whole)." >&2
    echo "  These read-at-test-time files were modified after the run started:" >&2
    echo "$run_gate_moved" | sed 's/^/      /' >&2
    echo "  ⚠ This is NOT 'the gate failed'. Any failure it reported may belong to the edit" >&2
    echo "    rather than to the code under test, and any PASS is equally unproven." >&2
    echo "  Let the tree settle and run it again. If you are the one who edited it: this" >&2
    echo "    project's runners read src/dss-config, tests/corpus and examples from the" >&2
    echo "    SOURCE TREE at test time, so an edit there is not inert while a suite runs." >&2
    echo "  (log: $log)" >&2
    exit 3
fi

# Checked next, and still BEFORE rc and the witness, for the same reason: a run
# that shared its build directory with another run has no verdict to report.
# ⓘ The ORDER between this and the inputs check is arbitrary in principle — both
# mean "no verdict" — and is fixed here so the two twins cannot disagree about
# which sentence a doubly-spoiled run prints. Inputs first, because that refusal
# is the older one and its exit code (3) is already cited in shipped fixtures.
if [ -n "$run_gate_contenders" ]; then
    run_gate_refuse_contention "AFTER the run finished (it exited $rc; that number describes a build directory two runs were writing)"
    exit "$run_gate_contention_exit"
fi

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
