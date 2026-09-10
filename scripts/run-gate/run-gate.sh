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

# ══ PREFLIGHT: A PARALLEL ctest UNDER MSYS DIES PART-WAY. REFUSE IT UP FRONT. ══
#
# ⚠⚠ ✔MEASURED 2026-09-08 (P64), a discriminator, not a correlation. The SAME
# `ctest --test-dir build/dbg -j 4`, on the SAME tree, in the SAME minutes:
#
#     launched through THIS script from Git Bash   -> died at 8, then 83, then
#                                                     271, then 612 of 2131
#     launched from PowerShell, no bash in between -> 2131/2131, rc 0, 910 s
#
# Four aborts, four different points, NO test ever reporting a failure, 36 GB RAM
# free, and excluding every repo guard changed nothing. It is not ctest and it is
# not the tree: it is MSYS's `fork()` emulation. Under `-j N` each ctest worker
# spawns a compiler which spawns children; the emulation runs out and the SHELL
# dies, taking its whole child tree with it.
#
# ★ THE EXIT CODE WAS TELLING ME THIS THE WHOLE TIME AND I READ IT AS A QUIRK:
# one of those runs came back **127**, and a POSIX shell reserves 127 for "command
# not found" — the shell saying it could not exec. The rc-127 arm at the bottom of
# this file already explains that; nothing connected it to the aborts because the
# other runs returned 1. ⇒ Four wasted runs, and a truncated log whose rc reads
# exactly like an ordinary test failure.
#
# ⇒ **REFUSE BEFORE STARTING, never truncate half a gate.** A gate that examines
# 4% of the suite and returns a failure-shaped code is worse than one that will
# not start: the first invites you to debug the tree, the second names the fix.
# The `.ps1` twin is the supported path on this host and it is not affected.
#
# ⓘ SCOPED DELIBERATELY: only MSYS, only `ctest`, only genuine parallelism. A
# serial `ctest` under MSYS is fine (measured), so it is allowed through — an
# escape everything triggers would refuse nothing
# ([[feedback-an-escape-every-row-triggers-disarms-the-guard]]).
run_gate_preflight_exit=4
case "$(uname -s 2>/dev/null || echo unknown)" in
    MSYS*|MINGW*)
        run_gate_is_ctest=0
        case "${1##*/}" in ctest|ctest.exe) run_gate_is_ctest=1 ;; esac
        if [ "$run_gate_is_ctest" -eq 1 ]; then
            run_gate_par=""
            run_gate_prev=""
            for run_gate_a in "$@"; do
                case "$run_gate_a" in
                    -j[0-9]*)      run_gate_par="${run_gate_a#-j}" ;;
                    --parallel=*)  run_gate_par="${run_gate_a#--parallel=}" ;;
                esac
                case "$run_gate_prev" in
                    -j|--parallel) run_gate_par="$run_gate_a" ;;
                esac
                run_gate_prev="$run_gate_a"
            done
            case "$run_gate_par" in
                ''|1) : ;;   # serial is fine under MSYS — measured
                *)
                    echo "run-gate.sh: REFUSED — a PARALLEL ctest under MSYS dies part-way (rc=$run_gate_preflight_exit)." >&2
                    echo "  shell    : $(uname -s) (Git Bash / MSYS)" >&2
                    echo "  command  : $*" >&2
                    echo "  parallel : -j $run_gate_par" >&2
                    echo "  ✔MEASURED P64: this exact command died at 8, 83, 271 and 612 of 2131 under" >&2
                    echo "    MSYS — four runs, four points, ZERO tests reporting a failure — while the" >&2
                    echo "    same command from PowerShell completed 2131/2131 rc 0. MSYS's fork()" >&2
                    echo "    emulation cannot sustain the process fan-out of a parallel ctest." >&2
                    echo "  ⇒ USE THE .ps1 TWIN, which is unaffected:" >&2
                    echo "      pwsh -NoProfile -File scripts/run-gate/run-gate.ps1 <log> <regex> $*" >&2
                    echo "  ⇒ Or run it serially here (-j 1), which is measured-safe but ~4x slower." >&2
                    echo "  This refusal exists because the alternative is a gate that stops at 4% of" >&2
                    echo "  the suite and returns a code indistinguishable from a real test failure." >&2
                    exit "$run_gate_preflight_exit"
                    ;;
            esac
        fi
        ;;
esac

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
# ⚠⚠⚠ AND THE FIRST INSTRUMENT FOR IT ASSUMED A MONOTONIC WALL CLOCK, WHICH ONE
# OF THIS PROJECT'S FOUR CARRIAGES DOES NOT HAVE. The original scan was a marker
# file plus `find -newer`, and the sentence that justified it — *"`-newer` is
# strictly-greater, which is the RIGHT direction here, because the marker is
# written BEFORE the run and an offending edit lands after"* — is true in REAL
# TIME and false in STAMP ORDER. "Before" and "after" there are two readings of
# CLOCK_REALTIME taken seconds apart, and comparing them ORDERS them.
#
# ✔MEASURED 2026-09-09 (P66) on WSL x86_64, the leg this repository gates every
# commit on. CLOCK_REALTIME there steps FORWARD by +24.69 s for ~200 ms out of
# every ~5 s and then snaps back — 29 of 600 samples at a 100 ms cadence, a 4.8%
# duty cycle — and ✔THE EXCURSION REACHES INODE MTIMES, which is the half that
# matters and was measured separately: in 12 of 60 marker/probe pairs, a file
# created ONE SECOND AFTER the marker carried an mtime **23.70 s EARLIER** than
# it, and `find <root> -type f -newer <marker>` answered EMPTY.
# ⇒ ~5% of runs on that carriage stamped their marker inside an excursion, and
# every edit that landed afterwards was INVISIBLE: the wrapper printed
# `inputs  : held still` over a tree that had moved — the one sentence this
# block exists to be unable to say wrongly, arriving by a third door.
# ✔The same host answers 0 inversions in 40 pairs on Windows, which is why this
# was green there and red here, and is a property of the HOST'S CLOCK rather than
# of either twin. ✔BOTH twins were shown to fail under the identical mutation (a
# marker stamped +25 s): `find -newer` returned nothing and the `.ps1` twin's
# `LastWriteTimeUtc -gt` returned nothing, from the same two inodes.
#
# ★★★ SO THE FIX IS TO STOP ORDERING TWO CLOCK READINGS AND START COMPARING TWO
# READINGS OF THE SAME VALUE. Each snapshot records, per file, `C <crc> <size>
# <path>` from POSIX `cksum` plus `N <path>` for a file already newer than the
# marker; the verdict is the SYMMETRIC DIFFERENCE of the before and after
# snapshots. EQUALITY of a fingerprint with itself cannot be defeated by a clock
# that steps forward, backward or sideways, because both readings carry the same
# distortion. The two halves cover each other exactly:
#   · `C` catches an edit whose stamp the clock hid — the measured defect;
#   · `N` catches a write that left the bytes unchanged (a `touch`, an identical
#     rewrite), which content alone cannot see, and is DIFFERENCED against its
#     own pre-run reading, so a file that already carried a future stamp when the
#     run started appears in BOTH snapshots and cancels — removing the LOUD
#     false-refusal direction the same clock produces, which the old scan had.
# ⇒ strictly more sensitive than `-newer` alone, in both directions.
#
# ⓘ `cksum` AND NOT `md5`/`md5sum`: the older note rejected hashing because the
# tool is spelled differently per carriage (macOS `md5`, Linux `md5sum`) — true,
# and it overlooked that `cksum` is POSIX and present on all four. ⚠ It does not
# matter whether two carriages compute the SAME checksum, because the two
# snapshots being compared are always taken by the SAME binary on the SAME host
# within one run: only SELF-CONSISTENCY is load-bearing here, which is what makes
# this portable without a host fork.
# ★ AND IT IS PROBED BY EXECUTION WITH A KNOWN ANSWER before anything runs
# (`run_gate_cksum_works`), never by `command -v`: a missing tool would make every
# snapshot empty, every diff empty, and this guard would refuse nothing while
# reporting green — the shape of escape this repository has already paid for.
# ✔MEASURED cost of the whole scheme across 2035 files / 12.8 MB in the three
# roots: 0.03 s per snapshot under WSL, 0.39 s under Windows Git Bash — against
# gates that run for a quarter of an hour.
# ⚠ WHICH TREE those three roots are read from is a SEPARATE question with its own
# defect and its own measurement, and it is answered where the roots are built —
# see "AND THE ROOTS ARE THE GATE COMMAND'S TREE, NOT THIS SHELL'S" below. It is
# not repeated here, so the two cannot drift into describing it differently.
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

# ── THE TWO IMAGE SETS, EACH SPELLED ONCE ───────────────────────────────────
#
# ⚠ The build-tool list used to live inside the awk candidate filter and nowhere
# else. It now has a SECOND reader — the ancestor walk below asks whether one of
# OUR OWN ancestors is a build tool — so it moved out here rather than being
# typed twice. A second spelling of this set would not fail: it would quietly
# classify one process differently in the two places.
run_gate_build_tools="ctest ninja cmake make gmake msbuild"

# ★★★ THE COMPILER'S OWN IMAGE, AND WHY IT IS A SECOND SUBJECT RATHER THAN A
# SEVENTH BUILD TOOL. [[D-PROGRAM-RUNTIME-CACHE-PRUNE-DELETES-A-CONCURRENT-RUNS-LIVE-ARTIFACT]]
#
# ✔MEASURED 2026-09-10 (cycle P66): two corpus examples went red inside an
# otherwise 2172/2174 run because a SECOND `dsscp` on the machine deleted a
# cache entry this run had been handed the path to — and this wrapper printed
# `contended: no`, which was TRUE (nothing else named the build directory) and
# useless (the resource actually shared lives in `%LOCALAPPDATA%`, outside both
# `srctree` and `builddir`).
#
# ⚠ IT IS NOT ADDED TO `run_gate_build_tools`, AND THE DIFFERENCE IS NOT
# COSMETIC. A build tool is matched only when its ARGV NAMES THIS BUILD
# DIRECTORY, and `dsscp` has no such argument to name — the cache it contends
# for is per-USER, so the subject is the MACHINE and the verdict is a different
# sentence. Folding it into the same list would have made every `dsscp` on the
# host either invisible (it names no build dir) or a build-directory contender
# (a claim that is simply false).
run_gate_compiler_image="dsscp"

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
#
# ⓘ SPLIT IN TWO because the two callers want DIFFERENT halves, and folding case
# for both would print a path this project never spells that way. Comparing two
# processes' directories needs the case fold; NAMING a directory in the log does
# not, and `c:/source/dailysoftware/...` in a footer reads as a different tree
# from the one the reader knows. `tidy` is the shared half.
run_gate_tidy_dir() {   # <path> — separator and trailing slash only
    _rg_np="$(printf '%s' "$1" | tr '\\' '/')"
    while :; do
        case "$_rg_np" in
            ?*/) _rg_np="${_rg_np%/}" ;;
            *)   break ;;
        esac
    done
    printf '%s' "$_rg_np"
}
run_gate_norm_dir() {   # <path> — tidy, plus case on Windows
    _rg_np="$(run_gate_tidy_dir "$1")"
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
run_gate_abs_dir() {   # <path> — absolute and tidy, NOT case-folded
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
    run_gate_tidy_dir "$_rg_ra"
}
run_gate_resolve_dir() {   # <path> — absolute AND comparable
    run_gate_norm_dir "$(run_gate_abs_dir "$1")"
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
    awk -F'\t' -v tools=" $run_gate_build_tools " '
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
            if (index(tools, " " img " ") == 0) next
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

# Every live `dsscp` that is NOT part of THIS gate's own process tree, one row:
#   pid <TAB> image <TAB> command-line
#
# ★★★ THE DISCRIMINATOR IS DESCENT, AND THE NAIVE VERSION IS USELESS. This
# gate's own `ctest` spawns hundreds of `dsscp` children, so *any live dsscp*
# would report on every run — the mirror image of
# [[feedback-an-escape-every-row-triggers-disarms-the-guard]], a signal that
# fires unconditionally and therefore carries nothing.
#
# ⚠ AND "OURS" IS BOUNDED, DELIBERATELY, BECAUSE THE OBVIOUS DEFINITION
# DISARMS THE CHECK IN THE OTHER DIRECTION. *Descends from any ancestor of
# mine* sounds right and is not: an ancestor chain that reaches a login shell,
# a session manager or `explorer.exe` makes EVERY process on the host a
# descendant of one of my ancestors, and the check then reports nothing, ever.
# ⇒ `own` is THIS SHELL, plus the ancestor chain UP TO AND INCLUDING THE
# OUTERMOST ANCESTOR THAT IS A BUILD TOOL. With no build-tool ancestor it is
# this shell alone. That covers the one case that needs covering — a gate
# invoked from INSIDE a `ctest`, which this repository does register, whose
# sibling `dsscp` processes belong to the same logical run — and it cannot
# widen past the build machinery no matter how the caller was launched.
#
# ⓘ ONE awk PASS, not a shell loop calling awk per ancestor: awk builds the
# pid → ppid map itself. The shell version was O(processes x depth) subshells
# on a table this file already measured at 534 rows.
run_gate_foreign_from_table() {   # <own-pid-list>
    awk -F'\t' -v own="$1" -v want="$run_gate_compiler_image" '
        BEGIN { n = split(own, o, " "); for (i = 1; i <= n; i++) if (o[i] != "") OWN[o[i]] = 1 }
        { par[$1] = $2; nm[$1] = $3; cl[$1] = $4; order[++k] = $1 }
        END {
            for (i = 1; i <= k; i++) {
                p = order[i]
                m = tolower(nm[p]); sub(/\.exe$/, "", m)
                if (m != want) continue
                a = p; d = 0; ours = 0
                while (a != "" && d < 24) {
                    if (a in OWN) { ours = 1; break }
                    if (!(a in par)) break
                    a = par[a]; d++
                }
                if (ours) continue
                print p "\t" nm[p] "\t" cl[p]
            }
        }'
}

# The refusal text, shared by the pre-run and post-run arms so the two cannot
# drift into describing the same fact differently.
run_gate_contenders="" ; run_gate_unreadable=0 ; run_gate_table_ok=0 ; run_gate_relative_match=0
run_gate_foreign="" ; run_gate_foreign_count=0
run_gate_scan_contention() {   # sets run_gate_contenders / run_gate_unreadable / run_gate_table_ok / run_gate_foreign
    run_gate_contenders=""; run_gate_unreadable=0; run_gate_table_ok=0; run_gate_relative_match=0
    run_gate_foreign=""; run_gate_foreign_count=0
    # ⚠ NO EARLY RETURN ON AN UNNAMED BUILD DIRECTORY ANY MORE. This function
    # now answers TWO questions, and only the first one has the build directory
    # as its subject; `run-gate.sh <log> <witness> bash -c …` names no build
    # directory and can still be sharing a machine with another compiler.
    _rg_tbl="$(run_gate_process_table)" || return 0
    [ -n "$_rg_tbl" ] || return 0
    run_gate_table_ok=1

    # OUR OWN ANCESTORS ARE NOT CONTENDERS. A gate legitimately invoked from
    # inside a `ctest` (this repository registers guards that way) would
    # otherwise refuse itself the moment it named the same tree.
    # ⓘ TWO SETS COME OUT OF ONE WALK. `_rg_exclude` is every ancestor — the
    # build-directory question asks *did I name this myself*, and any ancestor
    # of mine did. `_rg_own` is the BOUNDED prefix documented on
    # `run_gate_foreign_from_table`, because the machine-wide question asks
    # *is this process part of my run*, which a login shell three levels up
    # does not make true.
    _rg_self="$(run_gate_self_pid)"
    _rg_exclude=""
    _rg_own=""
    if [ -n "$_rg_self" ]; then
        _rg_walk="$_rg_self"; _rg_depth=0; _rg_chain=""; _rg_own=" $_rg_self"
        while [ -n "$_rg_walk" ] && [ "$_rg_depth" -lt 24 ]; do
            _rg_exclude="$_rg_exclude $_rg_walk"
            _rg_chain="$_rg_chain $_rg_walk"
            _rg_aimg="$(printf '%s\n' "$_rg_tbl" | awk -F'\t' -v p="$_rg_walk" '$1 == p { print tolower($3); exit }')"
            _rg_aimg="${_rg_aimg%.exe}"
            case " $run_gate_build_tools " in
                *" $_rg_aimg "*) _rg_own="$_rg_chain" ;;
            esac
            _rg_walk="$(printf '%s\n' "$_rg_tbl" | awk -F'\t' -v p="$_rg_walk" '$1 == p { print $2; exit }')"
            _rg_depth=$((_rg_depth + 1))
        done
    else
        run_gate_contention_note="ancestry: UNRESOLVED (this shell's pid was not found in the process table; nothing was excluded, and every live compiler reads as external)"
    fi

    # THE MACHINE-WIDE SUBJECT. Asked whether or not a build directory was
    # named, and never fatal — see the footer for why.
    run_gate_foreign="$(printf '%s\n' "$_rg_tbl" | run_gate_foreign_from_table "$_rg_own")"
    if [ -n "$run_gate_foreign" ]; then
        run_gate_foreign_count="$(printf '%s\n' "$run_gate_foreign" | grep -c . || true)"
    fi

    [ -n "$run_gate_build_dir" ] || return 0

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

# ── AND THE ROOTS ARE THE GATE COMMAND'S TREE, NOT THIS SHELL'S ─────────────
#
# ★★★ THE THREE ROOT NAMES ARE RELATIVE, AND WHAT THEY ARE RELATIVE **TO** IS
# THE WHOLE QUESTION. They used to be resolved against the PROCESS WORKING
# DIRECTORY, which is right only when the caller happens to be standing in the
# tree the gate command reads — and this project gates lane worktrees from
# sibling trees all day.
#
# ✔MEASURED 2026-09-08 (P65, lane `rc`), BOTH DIRECTIONS, BOTH TWINS, with two
# synthetic trees A and B: cwd = B, gate command = `ctest --test-dir A/build/x`.
#   · edit an input root in **B** (a tree the run never reads) -> exit 3, both
#     twins, naming `examples/.probe-…` — a LOUD FALSE REFUSAL that spends a
#     15-minute gate on a file the run could not have seen. A sibling lane hit
#     exactly this shape in the field.
#   · edit an input root in **A** (the tree whose build directory the command
#     names, and whose config its tests read) -> exit **0**, both twins, footer
#     `inputs  : held still`. ⇒ THE SILENT WRONG ANSWER, and it is the one
#     sentence this whole block exists to be unable to say wrongly. The false
#     refusal wastes a run; this one SHIPS a verdict that has none.
#
# ★★★ WHAT THEY ARE RESOLVED AGAINST NOW, AND THE MECHANISM RATHER THAN A
# PREFERENCE: **the source tree that CMake itself records as having configured
# the build directory this command names.** `<build>/CMakeCache.txt` carries
# `CMAKE_HOME_DIRECTORY:INTERNAL=<dir>`; the tests registered in that build tree
# are the ones that will read `src/dss-config`, `tests/corpus` and `examples` at
# test time, and they will read them THERE. Nothing else in reach is evidence
# about which tree a gate command reads — this is CMake's own record of it.
#
# ✔AND THE BUILD SYSTEM SAYS IT IN SO MANY WORDS, which is why this is the
# MECHANISM and not an inference. `CMakeLists.txt` gives 35 registered tests
# `WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"`, and `tests/CMakeLists.txt` bakes
# `DSS_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}"` into every test binary — which is
# what `tests/test_support/repo_root.hpp`'s `bakedRepoRoot()` returns and what
# every helper there resolves the three roots against. `CMAKE_SOURCE_DIR` is
# EXACTLY the value CMake writes to the cache as `CMAKE_HOME_DIRECTORY`. So the
# tree named here is the same tree the test binaries were compiled to read; the
# cwd agreed with it only when the caller happened to be standing in it.
#
# ⚠ WHAT THIS STILL DOES NOT REACH, stated rather than left to be discovered:
# `$DSS_CONFIG_ROOT`. The compiler's own walk composes `<that>/src/dss-config`
# (`src/core/types/config_path_walk.cpp`, `repoShapedConfigRoot`), so a gate run
# with that variable pointing OUTSIDE the tree named here reads a config tree
# this scan never walks. It is NOT guessed at here, deliberately: the compiler
# and `repo_root.hpp` document precedences that do not obviously agree about
# whether the variable names a tree root or the config directory itself, and a
# rule built on the wrong one would watch a directory that does not exist —
# which contributes nothing and restores the very `held still` this block
# exists to prevent, by a new door. ✔MEASURED: the one shipped caller that sets
# it, `scripts/profile-compile/profile-compile.sh`, sets it to the repository it
# is already standing in, so nothing is relocated today; the footer's absolute
# roots below are what make a future divergence visible.
#
# ⚠ THREE OTHER CANDIDATES WERE MEASURED AND ALL THREE ARE WRONG:
#   · **the repository root containing the build tree** — ✔MEASURED IN THIS
#     REPOSITORY: `build/rvff` sits in the main checkout and carries
#     `CMAKE_HOME_DIRECTORY:INTERNAL=…/.worktrees/ff`. A build tree in one tree,
#     configured from another. Walking up from the build directory answers "the
#     main checkout"; its tests read the worktree. The obvious rule is refuted
#     by a build tree this project already has on disk.
#   · **this wrapper's own location** — the fixture refutes it: test-run-gate.sh
#     drives the repository's `scripts/run-gate/run-gate.sh` from a SYNTHETIC
#     sandbox, so a script-relative rule would point every arm at the real
#     `examples/`, which is precisely the hazard
#     D-GATE-RUN-GATE-BLIND-TO-A-SECOND-RUN-IN-THE-SAME-BUILD-DIRECTORY closed
#     (a fixture that makes everyone else's gate exit 3).
#   · **the cwd** — the measurement above, in both directions.
#
# ⓘ THE CWD REMAINS THE FALLBACK, AND IT IS NOW STATED RATHER THAN ASSUMED. A
# gate command need not name a build directory at all (`remote-leg` hands this
# wrapper a `bash`, and its tree is on another host), and a named directory need
# not be a CMake build tree (this file's own fixture builds bare `ctest` trees).
# In both cases the wrapper has no evidence about which tree the command reads,
# and the cwd is the only thing it knows — so it says which rule decided, on
# every run, in the log.
#
# ⓘ NOT AN ESCAPE HATCH: nothing here is settable by a caller. Which rule fires
# is decided by what the argv NAMES and what is on disk beside it, exactly like
# the contention check above.
run_gate_input_root_names="src/dss-config tests/corpus examples"
run_gate_source_tree=""
run_gate_source_tree_why=""
run_gate_source_tree_miss=""
run_gate_source_tree_found=""
run_gate_marker="${log}.inputs-marker"
run_gate_inputs_before="${log}.inputs-before"
run_gate_inputs_after="${log}.inputs-after"
# ⚠ THE WRAPPER MUST NOT MEASURE ITS OWN BOOKKEEPING. All three files above sit
# beside the LOG PATH THE CALLER CHOSE, and nothing stops that path being inside
# a watched root. The marker was self-excluding under the old scan (a file is
# never `-newer` than itself); a snapshot is not, and `.inputs-before` — created
# after the BEFORE walk and therefore present only in the AFTER one — would have
# made every such run refuse ITSELF, a new false refusal invented by the fix.
# ⓘ `-name` matches the basename only, so this cannot reach outside the three.
run_gate_bookkeeping_glob="${log##*/}.inputs-*"

# CMake's own record of which tree configured this build tree, or nothing — and
# when nothing, WHY nothing, in `run_gate_source_tree_miss`.
# ★★ THE FOUR MISSES ARE NOT ONE MISS, and collapsing them into "not a CMake
# build tree" is the shape of message this file keeps refusing: a sentence that
# outruns its evidence. ✔MEASURED while building this — a CMakeCache whose
# recorded home directory THIS SHELL CANNOT SEE is a real, reachable state (an
# MSYS-spelled `/c/…` is invisible to PowerShell and a `C:/…` is invisible to a
# WSL bash, which is the same wrong-bash family this file already documents at
# `run_gate_shell_identity`), and it is emphatically NOT "there is no cache".
# ⚠ IT SETS VARIABLES AND DOES NOT ECHO, deliberately: a `$( … )` runs in a
# SUBSHELL, so the reason for a miss would be set in a process that exits before
# anyone could read it — and the caller would then print the WRONG reason with
# every appearance of having asked. Same class as this file's other "a message
# that outruns its evidence" notes.
run_gate_source_tree_of_build_dir() {   # <absolute build dir>
    run_gate_source_tree_miss=""; run_gate_source_tree_found=""
    if [ -z "${1:-}" ]; then
        run_gate_source_tree_miss="this command names no build directory, so there is nothing to ask"
        return 1
    fi
    _rg_cache="$1/CMakeCache.txt"
    if [ ! -f "$_rg_cache" ]; then
        run_gate_source_tree_miss="the build directory it names has no CMakeCache.txt, so nothing on disk records which tree configured it"
        return 1
    fi
    _rg_home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$_rg_cache" 2>/dev/null | head -1 | tr -d '\r')"
    if [ -z "$_rg_home" ]; then
        run_gate_source_tree_miss="its CMakeCache.txt carries no CMAKE_HOME_DIRECTORY entry"
        return 1
    fi
    if [ ! -d "$_rg_home" ]; then
        run_gate_source_tree_miss="its CMakeCache.txt names '$_rg_home' as CMAKE_HOME_DIRECTORY and THIS SHELL ($(run_gate_shell_identity)) CANNOT SEE THAT DIRECTORY — a DOS-drive path is invisible to a WSL bash and an MSYS '/c/…' path is invisible to PowerShell, so check which shell you handed this gate to"
        return 1
    fi
    run_gate_source_tree_found="$(run_gate_tidy_dir "$_rg_home")"
    return 0
}

run_gate_decide_input_roots() {
    if run_gate_source_tree_of_build_dir "$run_gate_abs_build_dir"; then
        run_gate_source_tree="$run_gate_source_tree_found"
        run_gate_source_tree_why="CMAKE_HOME_DIRECTORY recorded in ${run_gate_abs_build_dir}/CMakeCache.txt — the tree this command's build directory was configured from"
        return 0
    fi
    run_gate_source_tree="$(run_gate_abs_dir .)"
    run_gate_source_tree_why="this shell's working directory — $run_gate_source_tree_miss"
}

# The three roots as ABSOLUTE paths, one per line. ⓘ Used by BOTH the scan and
# the footer on purpose: a footer that names roots the scan did not walk is the
# class of lie this row is about.
run_gate_abs_input_roots() {
    for _rg_n in $run_gate_input_root_names; do
        printf '%s/%s\n' "$run_gate_source_tree" "$_rg_n"
    done
}

# ⚠ ONE `find` PER ROOT, EACH ARGUMENT QUOTED, AND EACH ROOT EXISTENCE-CHECKED.
# The former single `find $roots …` relied on word splitting, so an absolute root
# holding a space (`C:/Program Files/…` is a real spelling on this host) tore in
# half — and if the list had ever been empty, `find` with no path operand walks
# the CURRENT DIRECTORY under GNU find, which is the same wrong-tree answer this
# block exists to remove, arriving by a different door.
# ⓘ A root that does not exist contributes nothing, so a lane worktree carrying a
# subset of the tree, or a synthetic self-test root, is not penalised for it.
#
# ★ PROBED BY EXECUTION WITH A KNOWN ANSWER, never by `command -v` — the same
# ruling, and the same reason, as the fixture's PowerShell probe: a lookup can
# name a tool that cannot run, and here that would empty every snapshot and make
# this guard refuse nothing while printing green. The known answer is the BYTE
# COUNT and not the checksum, deliberately: only self-consistency between two
# snapshots on one host is load-bearing, so pinning a particular CRC would be a
# portability claim this file does not need and cannot check on four carriages.
run_gate_cksum_works() {
    [ "$(printf dss | cksum 2>/dev/null | awk '{print $2}')" = "3" ]
}

# ONE fingerprint line per file, plus one per file that ALREADY carries a stamp
# later than the marker. Sorted, so `comm` can difference two of them.
#   C <crc> <size> <path>   the bytes — an equality that no clock can distort
#   N <path>                the stamp order — differenced against its own pre-run
#                           reading, so a future stamp that predates the run
#                           cancels instead of refusing it
run_gate_snapshot_inputs() {
    run_gate_abs_input_roots | while IFS= read -r _rg_root; do
        [ -n "$_rg_root" ] || continue
        [ -d "$_rg_root" ] || continue
        find "$_rg_root" -type f ! -name "$run_gate_bookkeeping_glob" \
            -exec cksum {} + 2>/dev/null | sed 's/^/C /'
        find "$_rg_root" -type f ! -name "$run_gate_bookkeeping_glob" \
            -newer "$run_gate_marker" -print 2>/dev/null | sed 's/^/N /'
    done | LC_ALL=C sort
}

# THE DIFF ALONE, over two snapshot files the caller has already proved exist.
# ⚠⚠ THE AFTER SNAPSHOT IS TAKEN IN THE MAIN SHELL AND NOT HERE, DELIBERATELY.
# This function's result is read through `$( … )`, which runs in a SUBSHELL, so a
# failure noticed inside it could not reach the caller through any variable — it
# could only return EMPTY, and empty means `inputs  : held still`. ⇒ "I could not
# measure" would print as "nothing moved", which is the fails-toward-clean answer
# this whole block exists to be unable to give. ⓘ The scan this replaced had the
# same shape (`[ -f "$run_gate_marker" ] || return 1`): a marker that vanished
# mid-run read as a still tree. Both doors are closed by deciding it out here.
run_gate_diff_inputs() {
    {
        LC_ALL=C comm -23 "$run_gate_inputs_before" "$run_gate_inputs_after"
        LC_ALL=C comm -13 "$run_gate_inputs_before" "$run_gate_inputs_after"
    } 2>/dev/null | sed -e 's/^C [0-9][0-9]* [0-9][0-9]* //' -e 's/^N //' \
      | LC_ALL=C sort -u | head -20
}
# ── PRE-RUN: refuse a contended build directory BEFORE anything starts ──────
# ⚠ Placed AHEAD of the input marker deliberately, so a refusal here leaves no
# marker file behind for the next run to trip over.
run_gate_abs_build_dir=""
if run_gate_raw_build_dir="$(run_gate_dir_named_by_argv "$@")"; then
    # TWO SPELLINGS OF ONE DIRECTORY, and each has exactly one caller: the
    # case-folded one is only ever COMPARED against another process's spelling;
    # the plain one is what gets NAMED in a message and what CMakeCache.txt is
    # read beside.
    run_gate_abs_build_dir="$(run_gate_abs_dir "$run_gate_raw_build_dir")"
    run_gate_build_dir="$(run_gate_norm_dir "$run_gate_abs_build_dir")"
fi
run_gate_decide_input_roots
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

# ⚠ THREE REFUSALS, NOT ONE, and each names what it could not do. A wrapper that
# cannot fingerprint the tree cannot vouch for its stillness, and the honest
# answer to that is to run NOTHING — never to run the gate and report a verdict
# the wrapper knows it cannot stand behind.
if ! run_gate_cksum_works; then
    echo "run-gate.sh: FAIL — 'cksum' did not return the byte count of a 3-byte input on this" >&2
    echo "  host, so the input fingerprint this wrapper compares before and after the run" >&2
    echo "  cannot be taken. Nothing was run." >&2
    echo "  cksum is POSIX and is expected on every carriage this project gates on; a host" >&2
    echo "    without one has a broken environment, and this refusal is deliberate rather" >&2
    echo "    than a skip: an empty fingerprint would make every diff empty and this check" >&2
    echo "    would pass everything while appearing to run." >&2
    echo "  shell   : $(run_gate_shell_identity)" >&2
    rm -f "$run_gate_marker"
    exit 2
fi

if ! run_gate_snapshot_inputs > "$run_gate_inputs_before" 2>/dev/null \
   || [ ! -f "$run_gate_inputs_before" ]; then
    echo "run-gate.sh: FAIL — cannot record the pre-run input fingerprint at" >&2
    echo "  '$run_gate_inputs_before', so the run could not be proved to have measured a" >&2
    echo "  still tree. Nothing was run." >&2
    echo "  This refusal is about that PATH, which sits beside the log path you gave." >&2
    echo "  shell   : $(run_gate_shell_identity)" >&2
    rm -f "$run_gate_marker"
    exit 2
fi

"$@" >>"$log" 2>&1
rc=$?

run_gate_snapshot_ok=1
if [ ! -f "$run_gate_inputs_before" ]; then
    run_gate_snapshot_ok=0
elif ! run_gate_snapshot_inputs > "$run_gate_inputs_after" 2>/dev/null; then
    run_gate_snapshot_ok=0
elif [ ! -f "$run_gate_inputs_after" ]; then
    run_gate_snapshot_ok=0
fi
run_gate_moved=""
[ "$run_gate_snapshot_ok" -eq 1 ] && run_gate_moved="$(run_gate_diff_inputs)"
rm -f "$run_gate_marker" "$run_gate_inputs_before" "$run_gate_inputs_after"

# ── POST-RUN: a sibling can start MID-RUN, so the same question is asked again ──
run_gate_scan_contention

{
    echo "--- run-gate.sh ---"
    echo "command : $*"
    echo "rc      : $rc"
    if [ "$run_gate_snapshot_ok" -eq 0 ]; then
        echo "inputs  : NOT MEASURED — the post-run fingerprint could not be taken, so this verdict is not evidence"
    elif [ -n "$run_gate_moved" ]; then
        echo "inputs  : MOVED DURING THE RUN — this verdict is not evidence"
        echo "$run_gate_moved" | sed 's/^/          /'
    else
        echo "inputs  : held still"
    fi
    # ★★ THE FOOTER NAMES THE TREE IT WATCHED, ABSOLUTELY, ON EVERY RUN — green,
    # refused, or failed. `held still` is a claim about a DIRECTORY, and a reader
    # who has to reconstruct the caller's working directory to learn which
    # directory cannot check the claim at all. This is the half of the fix that
    # costs three lines and would have made the measured defect self-evident in
    # the log the first time it happened.
    echo "srctree : $run_gate_source_tree"
    echo "          decided by: $run_gate_source_tree_why"
    run_gate_abs_input_roots | sed 's/^/watched : /'
    if [ -z "$run_gate_build_dir" ]; then
        echo "builddir: none named by this command — the contention check had no subject"
    else
        echo "builddir: $run_gate_build_dir"
        if [ -n "$run_gate_contenders" ]; then
            echo "contended: YES, ANOTHER RUN WAS LIVE IN IT — this verdict is not evidence"
            printf '%s' "$run_gate_contenders"
        elif [ "$run_gate_table_ok" -eq 1 ]; then
            # ⚠ THE WORDING IS NARROWED, AND THE OLD ONE WAS NOT WRONG — IT WAS
            # OVER-READ, WHICH IS WORSE. It said "this run was alone in it", and
            # "it" is the BUILD DIRECTORY, which was true of the run that took
            # two false reds from a second compiler. A line that is true and
            # invites the wrong conclusion costs more than one that is false,
            # because nobody re-checks it. The subject is now named in the
            # sentence, and the machine-wide subject has a line of its own.
            echo "contended: no — no other build-tool run named THIS BUILD DIRECTORY ($run_gate_unreadable candidate process(es) had no readable command line and could not be judged). ⚠ This says NOTHING about the rest of the machine; see 'compilers:' below."
        else
            echo "contended: UNKNOWN — no process table could be read on this host, so nothing was ruled out"
        fi
    fi
    # ★★★ THE MACHINE-WIDE SUBJECT, ON EVERY RUN, GREEN OR NOT, AND WITH OR
    # WITHOUT A BUILD DIRECTORY. [[D-PROGRAM-RUNTIME-CACHE-PRUNE-DELETES-A-CONCURRENT-RUNS-LIVE-ARTIFACT]]
    #
    # ⚠ IT REPORTS AND DOES NOT REFUSE, and that is a decision rather than
    # timidity. `exit 4` means THIS RUN HAS NO VERDICT, and a second compiler
    # no longer takes one away: the mechanism that made it do so — a store
    # deleting a cache entry it could not prove was dead — is gone in the same
    # change that added this line. What is left is real but weaker (CPU, a
    # shared cache being warmed under us), and refusing on it would refuse
    # EVERY gate of a project whose own working rule is up to four lanes
    # building in parallel — a refusal that fires on every honest run, which is
    # exactly as useless as an escape that does.
    # ⇒ The line exists so that the NEXT unexplained red has this fact in its
    # log instead of needing an operator to remember they had a shell open.
    if [ "$run_gate_table_ok" -ne 1 ]; then
        echo "compilers: UNKNOWN — no process table could be read on this host"
    elif [ "$run_gate_foreign_count" -eq 0 ]; then
        echo "compilers: none outside this gate's own process tree"
    else
        echo "compilers: $run_gate_foreign_count live '$run_gate_compiler_image' process(es) OUTSIDE this gate's process tree — they share this user's compiler caches with this run, which are NOT under srctree or builddir"
        printf '%s\n' "$run_gate_foreign" | sed 's/^/          /'
    fi
    [ -n "$run_gate_contention_note" ] && echo "$run_gate_contention_note"
} >> "$log"

# Checked BEFORE rc, and before the witness: a run whose inputs moved has no
# verdict to report, and saying "the gate failed" or "the gate passed" about it
# would be the misattribution this block exists to prevent.
# ⚠ AND "I COULD NOT MEASURE" IS CHECKED FIRST OF ALL, because it is the one
# answer that must never be spelled `held still`. The pre-run fingerprint already
# succeeded — this wrapper refused to start otherwise — so reaching here means
# something removed or blocked the snapshot path WHILE the run was in flight.
if [ "$run_gate_snapshot_ok" -eq 0 ]; then
    echo "run-gate.sh: FAIL — THE POST-RUN INPUT FINGERPRINT COULD NOT BE TAKEN, so this run" >&2
    echo "  cannot be shown to have measured a still tree." >&2
    echo "  (command exited $rc; that number is NOT being reported as a verdict)." >&2
    echo "  The PRE-run fingerprint was taken successfully or this run would not have started," >&2
    echo "    so something removed or blocked '$run_gate_inputs_before' or" >&2
    echo "    '$run_gate_inputs_after' while the command was running." >&2
    echo "  ⚠ Refusing is deliberate. Reading an unmeasurable tree as 'held still' is exactly" >&2
    echo "    the fails-toward-clean answer this check exists to be unable to give." >&2
    echo "  (log: $log)" >&2
    exit 3
fi

if [ -n "$run_gate_moved" ]; then
    echo "run-gate.sh: FAIL — the tree CHANGED UNDER THE RUN, so its result is not evidence" >&2
    echo "  (command exited $rc; that number describes a tree that never existed as a whole)." >&2
    echo "  These read-at-test-time files were modified after the run started:" >&2
    echo "$run_gate_moved" | sed 's/^/      /' >&2
    echo "  source tree watched: $run_gate_source_tree" >&2
    echo "    decided by: $run_gate_source_tree_why" >&2
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
