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
#     5   ANOTHER LIVE RUN-GATE HOLDS THIS LOG PATH — nothing was run, and that
#         run's log was not touched
#     *   otherwise, the command's own exit code (127 gets its own sentence)
#   ⚠ 3, 4 and 5 are DELIBERATELY DIFFERENT NUMBERS. Each means "this run has no
#   verdict", and a reader who cannot tell which fired cannot pick the remedy:
#   settle the tree, wait for a sibling, or give this gate its own log path.
#
# Example — a log path that belongs to ONE tree and ONE build directory, because a
# log path is one live run's alone (a second live run on it is refused with 5):
#   scripts/run-gate/run-gate.sh build/dbg-ctest.log '100% tests passed' \
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
# ⚠ EXIT 2, NOT 4. This refusal used to return 4, the number the contract above
# reserves for "another run is live in the same build directory" — a usage refusal
# wearing the contention code, whose remedy (wait for a sibling) is the wrong one.
# A parallel ctest from this shell is an invocation this host cannot honour, which
# is exactly what 2 says. D-SCRIPT-WSL-LEG-AND-RUN-GATE-LET-CONCURRENT-LEGS-SHARE-ONE-LOG-PATH
run_gate_preflight_exit=2
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

# ⓘ THE TRUNCATE ITSELF NOW HAPPENS IN THE PRE-RUN SECTION, AFTER THIS RUN HAS TAKEN
#   THE LOG PATH'S OWNER RECORD — see "THE LOG PATH IS ONE LIVE RUN'S ALONE". Truncating
#   here, first, is what let a second run on the same path erase a live run's log
#   before anything could notice the path was taken. The refusal text is kept here,
#   as a function, so both of its callers say the same thing.
run_gate_refuse_log_path() {   # <what could not be created>
    echo "run-gate.sh: FAIL — cannot create $1, so nothing was run." >&2
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
}

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

# pid <TAB> ppid <TAB> image <TAB> command-line <TAB> argv0-image <TAB> created,
# one row per live process. TWO spellings of the image, and that is the whole
# point of the fifth column. The sixth is what makes the SECOND column safe to
# follow at all — see "A PARENT LINK IS A CLAIM ABOUT ORDER" below.
#
# ★★★ `comm` IS NOT AN IMAGE NAME ON macOS, AND THE CHECKS BUILT ON IT WERE
# STRUCTURALLY BLIND THERE. [D-SCRIPT-RUN-GATE-MATCHES-AN-IMAGE-AGAINST-A-COMM-COLUMN-THAT-IS-A-TRUNCATED-PATH-ON-MACOS]
# ✔MEASURED 2026-09-14 on the macOS carriage, `ps -eo pid=,ppid=,comm=,args=`:
#     77728     1 /tmp/dss-ci-stub /tmp/dss-ci-stubprobe/dsscp_signed 9
#     1         0 /sbin/launchd    /sbin/launchd
# `comm` there is an ABSOLUTE PATH, truncated to the column width (16 chars when
# it is not the last column) — **650 of 662 rows contained a `/`**. The Linux
# control, same command under WSL: `systemd`, `init`, **0 of 39 rows with a `/`**.
# So `tolower(comm) == "dsscp"` cannot be true on macOS for any process, ever,
# and both subjects built on it — the foreign-compiler line AND the build-tool
# contention scan — answered "none" on that host no matter what was running.
# That is the shape [[feedback-an-escape-every-row-triggers-disarms-the-guard]]
# names from the other side: a check that reaches its refusal on no input at all.
#
# ⇒ The image is now matched against BOTH the basename of `comm` AND the basename
# of `argv[0]` (the first token of the command line). ⚠ BOTH, never one: matching
# either can only ever ADD a detection, so no host can lose one, and the
# fail-toward-REPORTING direction is the one this subject already chose (the
# fixture's orphaned stub is deliberately read as foreign). argv[0] is the
# spelling that survives on macOS, because `args` is not truncated; `comm` stays
# because it is the spelling a process that rewrote its own argv still answers to.
# ⓘ Windows is unchanged in substance: CIM `Name` is already a bare image name, so
# it fills both columns and the matcher sees exactly what it saw before.
#
# ✔MEASURED cost on this workstation: `powershell -NoProfile -NonInteractive`
# with the CIM query below returns 534 rows in 741–1086 ms (median 780 ms); the
# same query from an ALREADY-RUNNING PowerShell is 341 ms, so the spawn is most
# of it. `pwsh` was measured SLOWER to start (877–944 ms) and `Get-Process`
# slower still (1494–1575 ms), so `powershell` is preferred and `pwsh` is the
# fallback. Two checks per gate is ~1.6 s against gates that run for minutes.
#
# ★★★ A PARENT LINK IS A CLAIM ABOUT ORDER, AND ON WINDOWS IT GOES STALE.
# D-TEST-RUN-GATE-FIXTURE-RACES-FIXED-LIFETIME-PROCESSES-AGAINST-THE-GATES-SAMPLING-LATENCY
# Windows does NOT reparent an orphan: `ParentProcessId` keeps naming the dead
# parent's pid, and pids are RECYCLED. ✔MEASURED 2026-09-14 on this workstation:
#   · under MSYS nearly every process is an orphan FROM BIRTH. A bash started
#     from another bash already reads `ppid=26144 NOT IN TABLE` (fork, exec in
#     the fork child, the fork child exits), and so does a background job;
#   · a freed pid is handed out again after ~108 allocations — 400 sequential
#     spawns gave 36 repeats, every gap 108 or 217 — and threads draw from the
#     same table, so under a parallel build that is well under a second.
# ⇒ both walks below could climb from a process into WHATEVER NOW HOLDS its dead
# parent's pid. ✔REPRODUCED DETERMINISTICALLY, by handing this file's own
# classifier the table such a recycle produces, before this rule existed:
#   · a foreign compiler whose dead parent's pid went to this gate's OWN
#     sampler read as ours — `compilers: none` with a foreign compiler alive;
#   · a gate whose own dead parent's pid went to an unrelated `ctest.exe`
#     adopted that ctest's whole tree, and the build-directory exclusion below
#     then waves through a real contender: exit 0 where 4 is owed, SILENTLY.
# ★ THE RULE (`parent_of` in run_gate_classify_table, and Get-RunGateTrustedParent
# in the twin): a process can only be the parent of a process created AFTER it.
# A link naming a parent created LATER is a recycled pid and the chain ENDS
# there; an unknown creation time on either side ends it too. A shorter chain
# fails LOUD — a compiler reported, a contender refused — and a longer unproven
# one is the silent direction this rule exists to remove.
# ⓘ POSIX REPARENTS an orphan to an OLDER live process, ✔MEASURED on all three
# carriages: WSL -> `Relay(599)`, macOS -> launchd (pid 1), arm64 VPS -> systemd
# (pid 1). The rule cannot fire there; it is applied anyway, so both twins decide
# by ONE rule on every host rather than by a host fork.
# ⓘ THE KEY is `yyyyMMddHHmmssffffff`, UTC, 20 digits, compared as a STRING — as
# a number it exceeds a double's exact range and awk would order it wrongly.
# Windows: CIM `CreationDate`, microseconds, InvariantCulture (a custom date
# format under a non-Gregorian culture prints a different year). POSIX: `lstart`
# under `LC_ALL=C TZ=UTC`, seconds, so equal keys are trusted — ✔`etimes` does
# not exist on macOS (`ps: etimes: keyword not found`) and `lstart` exists on all
# three; C for English month names, UTC so a DST fall-back cannot order a child
# before its parent. A `ps` without `lstart` yields NO table and reads as blind
# (`UNKNOWN`), which is the honest answer for a host that cannot say who is whose.
#
# ⚠⚠ ONE ROW PER PROCESS IS A PROPERTY OF THE COMMAND LINE, AND ON WINDOWS A
# COMMAND LINE CAN HOLD A NEWLINE. The CIM query used to replace only TABS in it.
# ✔MEASURED 2026-09-14 on this workstation: the VS Code PowerShell extension's
# `pwsh.exe` carries a multi-line `-Command`, and this table came back with that
# process's row cut short and FOUR invented rows after it —
# `Copyright (c) Microsoft Corporation.`, `https://aka.ms/vscode-powershell`, … —
# each one "a process" whose pid column is prose. The trailing columns (argv0,
# and now the creation key) landed on the wrong line. The `.ps1` twin holds
# OBJECTS and never saw it: a parity break inside the pair, invisible on any
# host without such a process. ⇒ CR, LF and TAB all become a space, in both twins.
run_gate_process_table() {
    if run_gate_is_windows; then
        _rg_pssh=""
        for _rg_c in powershell pwsh; do
            if command -v "$_rg_c" >/dev/null 2>&1; then _rg_pssh="$_rg_c"; break; fi
        done
        [ -n "$_rg_pssh" ] || return 1
        "$_rg_pssh" -NoProfile -NonInteractive -Command '$ErrorActionPreference="SilentlyContinue"; $inv = [Globalization.CultureInfo]::InvariantCulture; Get-CimInstance Win32_Process | ForEach-Object { $cr = ""; if ($_.CreationDate) { $cr = $_.CreationDate.ToUniversalTime().ToString("yyyyMMddHHmmssffffff", $inv) }; $_.ProcessId.ToString() + "`t" + $_.ParentProcessId.ToString() + "`t" + $_.Name + "`t" + ($_.CommandLine -replace "[`t`r`n]", " ") + "`t" + $_.Name + "`t" + $cr }' 2>/dev/null | tr -d '\r'
    else
        LC_ALL=C TZ=UTC ps -eo pid=,ppid=,lstart=,comm=,args= 2>/dev/null |
            awk '{
                p = $1; q = $2
                # lstart is FIVE fields: weekday, month, day, HH:MM:SS, year.
                mi = index("JanFebMarAprMayJunJulAugSepOctNovDec", $4)
                n = split($6, hms, ":")
                cr = ""
                if (length($4) == 3 && mi > 0 && (mi - 1) % 3 == 0 && n == 3 && ($5 $7 hms[1] hms[2] hms[3]) ~ /^[0-9]+$/)
                    cr = sprintf("%04d%02d%02d%02d%02d%02d000000", $7, (mi + 2) / 3, $5, hms[1], hms[2], hms[3])
                c = $8
                for (i = 1; i <= 8; i++) $i = ""
                sub(/^[ \t]+/, "")
                cl = $0
                a0 = cl; sub(/[ \t].*$/, "", a0)   # argv[0], whole-path
                sub(/^.*\//, "", a0)               # ... basenamed
                sub(/^.*\//, "", c)                # comm, basenamed
                printf "%s\t%s\t%s\t%s\t%s\t%s\n", p, q, c, cl, a0, cr
            }'
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

# THE WORKING DIRECTORY OF ANOTHER PROCESS, where this host lets one be read.
# [[D-SCRIPT-RUN-GATE-RESOLVES-ANOTHER-PROCESS-RELATIVE-BUILD-DIR-AGAINST-ITS-OWN-CWD]]
# A contender's RELATIVE build-directory token means nothing until it is joined to
# THAT process's directory. Linux publishes it as `/proc/<pid>/cwd`; macOS answers
# through `lsof -d cwd`, which ships with the OS; Windows does not expose another
# process's directory at all, so there — and wherever the read fails (another user's
# process, no lsof) — this returns 1 and the caller falls back to this shell's
# directory AND SAYS SO in the refusal.
# ⓘ BOTH readers are TRIED, never looked up with `command -v`, which this repository
#   has measured lying over a non-interactive ssh session on the macOS carriage.
# ⓘ Only an ABSOLUTE answer is accepted, so a reader that prints something else
#   cannot become a path this shell then `cd`s into.
run_gate_process_cwd() {   # <pid, in the table's namespace> -> prints the directory
    run_gate_is_windows && return 1
    _rg_pc="$(readlink "/proc/$1/cwd" 2>/dev/null)"
    [ -n "$_rg_pc" ] || _rg_pc="$(lsof -a -p "$1" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p' | head -1)"
    case "$_rg_pc" in
        /*) printf '%s' "$_rg_pc"; return 0 ;;
    esac
    return 1
}

# ⚠⚠ WHERE A PROCESS STANDS *NOW* IS NOT WHERE ITS RELATIVE ARGUMENTS WERE WRITTEN.
# ✔MEASURED 2026-09-15 (P66, lane `pg`) on WSL: `( cd Y && exec ctest --test-dir bd-rel )`
# reads `/proc/<pid>/cwd` = `Y/bd-rel` -- ctest changes INTO its test directory after it
# parses its arguments, and `ninja -C` does the same. Joining that cwd to the token built
# `Y/bd-rel/bd-rel`, so a contender in the SAME tree went unrefused and the gate ran its
# test. ⇒ a relative token denotes one of TWO directories: `cwd/token` while the tool has
# not entered it yet, or the cwd ITSELF once it has -- and the second reading is only
# offered when the cwd's own trailing components ARE the token. A token with a `..` or `.`
# component gets only the first reading, because a suffix comparison cannot interpret it.
run_gate_path_ends_with() {   # <absolute dir> <relative token> -> 0 when the dir's tail IS the token
    _rg_et="$(printf '%s' "$2" | tr '\\' '/')"
    while :; do
        case "$_rg_et" in
            ./*) _rg_et="${_rg_et#./}" ;;
            ?*/) _rg_et="${_rg_et%/}" ;;
            *)   break ;;
        esac
    done
    case "/$_rg_et/" in
        //|*/../*|*/./*) return 1 ;;
    esac
    _rg_ed="$(run_gate_tidy_dir "$1")"
    [ "${_rg_ed%"/$_rg_et"}" != "$_rg_ed" ]
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
        # The image key of one spelling: lowered, `.exe` dropped. Both the `comm`
        # basename and the argv[0] basename are run through it and EITHER may
        # match — see the two-spellings note above run_gate_process_table.
        function key(s) { s = tolower(s); sub(/\.exe$/, "", s); return s }
        {
            pid = $1; cl = $4
            img = key($3); alt = key($5)
            if (index(tools, " " img " ") == 0) {
                if (index(tools, " " alt " ") == 0) next
                img = alt
            }
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

# THIS GATE'S OWN LINEAGE, AND EVERY LIVE `dsscp` OUTSIDE IT — one awk pass over
# the table, one line per fact:
#   EXCLUDE  <TAB> every ancestor of this shell reached by a FOLLOWED link, this shell first
#   OWN      <TAB> the BOUNDED prefix of that chain (see below)
#   FOREIGN  <TAB> pid <TAB> image <TAB> command-line   one per compiler outside OWN
#   RECYCLED <TAB> child <TAB> parent                   one per link NOT followed
#                                                        because the parent is YOUNGER
#
# ★★ ONE PASS, AND ONE SPELLING OF `parent_of`. BOTH walks follow parent links —
# this shell's up towards the build machinery, every compiler's up towards this
# shell — so BOTH carry the recycled-pid rule described above
# run_gate_process_table, and two awk programs would be two spellings of it.
# ⓘ It also retires the shell loop that walked this shell's ancestry with three
# awk spawns per ancestor. And it is a PURE FUNCTION of (table, this shell's
# pid): nothing in it reads the machine, which is what lets test-run-gate.sh
# hand it the exact table a recycled pid produces.
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
# ⓘ awk builds the pid → ppid map itself. The shell version was O(processes x
# depth) subshells on a table this file already measured at 534 rows.
run_gate_classify_table() {   # <this shell's pid, in the table's namespace>
    awk -F'\t' -v self="$1" -v tools=" $run_gate_build_tools " -v want="$run_gate_compiler_image" '
        function key(s) { s = tolower(s); sub(/\.exe$/, "", s); return s }
        # BOTH image spellings, as everywhere else in this file.
        function is_tool(p,   k) {
            k = key(nm[p]); if (k != "" && index(tools, " " k " ") > 0) return 1
            k = key(a0[p]); return (k != "" && index(tools, " " k " ") > 0)
        }
        function valid(k) { return length(k) == 20 && k ~ /^[0-9]+$/ }
        # ★ THE ONE RULE — "A PARENT LINK IS A CLAIM ABOUT ORDER" above
        # run_gate_process_table. The parent of c, or "" when the link must NOT
        # be followed: absent, itself, a key that is not 20 digits, or created
        # AFTER c — a recycled pid, recorded once per link for the footer.
        # ⚠ The keys are compared as STRINGS on purpose (`"" ...`): 20 digits
        # exceed a double, so a numeric comparison would order them wrongly.
        function parent_of(c,   p) {
            p = par[c]
            if (p == "" || p == c || !(p in row)) return ""
            if (!valid(cr[c]) || !valid(cr[p])) return ""
            if ((cr[p] "") > (cr[c] "")) {
                if (!((c, p) in refused)) { refused[c, p] = 1; rec[++nrec] = c "\t" p }
                return ""
            }
            return p
        }
        $1 != "" { row[$1] = 1; par[$1] = $2; nm[$1] = $3; cl[$1] = $4; a0[$1] = $5; cr[$1] = $6; order[++k] = $1 }
        END {
            # 1. THIS SHELL, UP: every followed ancestor is excluded from the
            #    build-directory question; the prefix up to the OUTERMOST build
            #    tool is "own" for the compiler question.
            walk = self; d = 0; chain = ""; own = self; exclude = ""
            while (walk != "" && d < 24) {
                exclude = exclude " " walk
                chain = chain " " walk
                if (!(walk in row)) break
                if (is_tool(walk)) own = chain
                walk = parent_of(walk); d++
            }
            n = split(own, o, " ")
            for (i = 1; i <= n; i++) if (o[i] != "") OWN[o[i]] = 1
            # 2. EVERY COMPILER, UP, until it reaches OWN or a link ends.
            for (i = 1; i <= k; i++) {
                p = order[i]
                # EITHER spelling — the comm basename or the argv[0] basename.
                # On macOS only the second one can ever match; see the note
                # above run_gate_process_table.
                if (key(nm[p]) != want && key(a0[p]) != want) continue
                a = p; d = 0; ours = 0
                while (a != "" && d < 24) {
                    if (a in OWN) { ours = 1; break }
                    if (!(a in row)) break
                    a = parent_of(a); d++
                }
                if (ours) continue
                # ⚠ `want`, not `nm[p]`: on macOS the comm column is a truncated
                # PATH, so printing it here would name the process by a string
                # the reader cannot grep for. The command line beside it carries
                # the whole truth.
                print "FOREIGN\t" p "\t" want "\t" cl[p]
            }
            print "EXCLUDE\t" exclude
            print "OWN\t" own
            for (i = 1; i <= nrec; i++) print "RECYCLED\t" rec[i]
        }'
}

# The refusal text, shared by the pre-run and post-run arms so the two cannot
# drift into describing the same fact differently.
run_gate_contenders="" ; run_gate_unreadable=0 ; run_gate_table_ok=0 ; run_gate_relative_match=0
run_gate_foreign="" ; run_gate_recycled=""
# ONE tab and ONE newline, spelled once, for the fields this file reassembles.
run_gate_tab=$'\t'
run_gate_nl=$'\n'
run_gate_scan_contention() {   # sets run_gate_contenders / run_gate_unreadable / run_gate_table_ok / run_gate_foreign / run_gate_recycled
    run_gate_contenders=""; run_gate_unreadable=0; run_gate_table_ok=0; run_gate_relative_match=0
    run_gate_foreign=""; run_gate_recycled=""
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
    # of mine did. OWN is the BOUNDED prefix documented on
    # `run_gate_classify_table`, because the machine-wide question asks
    # *is this process part of my run*, which a login shell three levels up
    # does not make true.
    # ⓘ BOTH walks, BOTH image spellings for an ancestor build tool (an ancestor
    # `ctest` on macOS is `comm`ed as a truncated path), and the recycled-pid rule
    # they share now live in that one awk pass. This function reads the verdict.
    _rg_self="$(run_gate_self_pid)"
    [ -n "$_rg_self" ] || run_gate_contention_note="ancestry: UNRESOLVED (this shell's pid was not found in the process table; nothing was excluded, and every live compiler reads as external)"
    _rg_exclude=""
    # THIS RUN'S OWN CREATION KEY, read from the same table, for the log path's owner
    # record — see "THE LOG PATH IS ONE LIVE RUN'S ALONE" below.
    run_gate_self_created="$(printf '%s\n' "$_rg_tbl" | awk -F'\t' -v p="$_rg_self" '$1 == p { print $6; exit }')"
    _rg_class="$(printf '%s\n' "$_rg_tbl" | run_gate_classify_table "$_rg_self")"
    # ⚠⚠⚠ PROCESS SUBSTITUTION, NEVER A HERE-DOCUMENT, FOR TEXT THE MACHINE SIZES.
    # [[D-SCRIPT-RUN-GATE-PRE-RUN-SCAN-DEADLOCKS-ON-A-HERE-DOCUMENT-SIZED-BY-THE-PROCESS-TABLE]]
    # Both loops below used to read `done <<EOF $_rg_class EOF` and `$_rg_cands`, and
    # the size of that text is set by the MACHINE: one FOREIGN row per live `dsscp`
    # outside this gate, each carrying that process's whole command line.
    # ✔MEASURED 2026-09-15 (P66, lane pg), Git Bash / bash 5.3.15: a here-document
    # body of 65422 bytes and of 65858 bytes reads fine, and a 65656-byte body
    # DEADLOCKS the shell that expands it — bash decides the document fits in a pipe,
    # writes it into one whose MSYS capacity is smaller, and blocks on its own write
    # with nobody left to read. Idle, no CPU, forever. WSL's bash 5.2.21 reads every
    # size. ✔REPRODUCED END TO END with this file, its process table shimmed to 60
    # foreign rows: ~65340 and ~65820 bytes completed in ~1.4 s, ~65580 bytes WEDGED
    # with a 0-byte log and 0.05 CPU-s — the signature of the `run_gate_guard` arm
    # that sat idle for 876 s in a busy MSVC gate with its log truncated and empty.
    # ⇒ `< <(printf …)`: the writer is a separate process, so the pipe drains while it
    # fills, at ANY size (✔MEASURED at 65656, 66197 and 1010101 bytes on the same host).
    while IFS="$run_gate_tab" read -r _rg_kind _rg_f1 _rg_f2 _rg_f3; do
        case "$_rg_kind" in
            EXCLUDE)  _rg_exclude="$_rg_f1" ;;
            # THE MACHINE-WIDE SUBJECT. Asked whether or not a build directory
            # was named, and never fatal — see the footer for why.
            FOREIGN)  run_gate_foreign="${run_gate_foreign}${_rg_f1}${run_gate_tab}${_rg_f2}${run_gate_tab}${_rg_f3}${run_gate_nl}" ;;
            RECYCLED) run_gate_recycled="${run_gate_recycled}${_rg_f1}>${_rg_f2}${run_gate_nl}" ;;
        esac
    done < <(printf '%s\n' "$_rg_class")

    [ -n "$run_gate_build_dir" ] || return 0

    _rg_cands="$(printf '%s\n' "$_rg_tbl" | run_gate_candidates_from_table)"
    run_gate_unreadable="$(printf '%s\n' "$_rg_cands" | grep -c '^UNREADABLE' || true)"
    while IFS="$run_gate_tab" read -r _rg_kind _rg_pid _rg_img _rg_raw _rg_cl; do
        [ "${_rg_kind:-}" = "DIR" ] || continue
        case " $_rg_exclude " in *" $_rg_pid "*) continue ;; esac
        # ★★ A RELATIVE SPELLING IS RELATIVE TO *THAT* PROCESS'S DIRECTORY, AND THIS
        # HOST MAY BE ABLE TO READ IT. [[D-SCRIPT-RUN-GATE-RESOLVES-ANOTHER-PROCESS-RELATIVE-BUILD-DIR-AGAINST-ITS-OWN-CWD]]
        # ✔MEASURED 2026-09-15 (P66, lane pg) on WSL: a leg in one clone was refused
        # exit 4 because a leg in ANOTHER clone ran `ctest --test-dir build/dbg`, and
        # this loop resolved that token against THIS shell's directory — so two clones
        # contended over a directory neither shared. Where the other process's working
        # directory IS readable (`run_gate_process_cwd`) the token is joined to it;
        # only where it is not is this shell's directory assumed, and then the refusal
        # SAYS so. Whether the resolution was an assumption is recorded per match, so
        # the caveat is made only when it actually applies.
        _rg_assumed=0
        case "$(printf '%s' "$_rg_raw" | tr '\\' '/')" in
            /*|[A-Za-z]:/*) _rg_at="$(run_gate_resolve_dir "$_rg_raw")" ;;
            *)
                if _rg_pcwd="$(run_gate_process_cwd "$_rg_pid")"; then
                    # TWO READINGS -- see run_gate_path_ends_with: the tool has not entered
                    # the directory it named yet, or it already has and stands in it.
                    _rg_at="$(run_gate_resolve_dir "$_rg_pcwd/$_rg_raw")"
                    if [ "$_rg_at" != "$run_gate_build_dir" ] && run_gate_path_ends_with "$_rg_pcwd" "$_rg_raw"; then
                        _rg_at="$(run_gate_resolve_dir "$_rg_pcwd")"
                    fi
                else
                    _rg_at="$(run_gate_resolve_dir "$_rg_raw")"
                    _rg_assumed=1
                fi
                ;;
        esac
        [ "$_rg_at" = "$run_gate_build_dir" ] || continue
        [ "$_rg_assumed" -eq 1 ] && run_gate_relative_match=1
        run_gate_contenders="${run_gate_contenders}      pid ${_rg_pid}  ${_rg_img}  named it as '${_rg_raw}'
        ${_rg_cl}
"
    done < <(printf '%s\n' "$_rg_cands")
}

# ★★★ THE MACHINE-WIDE SUBJECT IS SAMPLED TWICE, AND THE LINE REPORTS BOTH.
# [[D-SCRIPT-RUN-GATE-COMPILERS-LINE-REPORTS-NONE-WHEN-IT-COULD-NOT-READ-THE-PROCESS-TABLE]]
#
# ⚠ THE FIRST SAMPLE USED TO BE THROWN AWAY. The scan runs before the command
# and again after it; the second call overwrote `run_gate_foreign`, so a
# compiler this gate HAD SEEN at the start and that exited before the end was
# reported as `compilers: none`. ⇒ *I saw one during this run* rendered as *the
# machine was clean* — the same sentence this whole subject exists to be unable
# to say wrongly, one door over from the blind case below.
#
# ★ AND THE DISCARDED CASE IS THE WORST ONE THIS LINE HAS.
# [[D-PROGRAM-RUNTIME-CACHE-PRUNE-DELETES-A-CONCURRENT-RUNS-LIVE-ARTIFACT]] is
# exactly a second `dsscp` that ran DURING a gate, deleted a cache entry the run
# had been handed the path to, and EXITED. A last-sample-wins rule cannot report
# the one shape the line was added for.
#
# ✔MEASURED 2026-09-14 on this Windows host, subject unmodified, a stub planted
# before the gate and outlived by it: gate wall clock 13.74 s against an 8 s
# stub -> alive at the pre-run scan, gone at the post-run scan ->
# `compilers: none outside this gate's own process tree`. The control with a
# 40 s stub named the process.
#
# ★ THAT IS ALSO WHAT THE CI `windows-msvc-release` LEG WAS PRINTING, ON A HOST
# WHOSE PROCESS TABLE READS FINE — ✔MEASURED from its own log, where the arm
# next door printed `compilers: none outside`, a sentence unreachable without a
# table. ⓘ INFERRED (that host cannot be logged into): the `.sh` twin SPAWNS
# `powershell` twice, ~1.0 s each here, while the `.ps1` twin calls
# `Get-CimInstance` IN-PROCESS and pays that zero times — 3.58 s against 2.29 s
# per gate here, on a leg that runs 2.3x slower. A wall-clock asymmetry between
# the twins, read for a cycle as a capability difference.
#
# ⓘ BOTH SAMPLES ARE KEPT WHOLE rather than accumulated in place, because the
# report needs to say WHICH sample saw each process: *alongside you the whole
# time* and *ran while you worked and exited* are different facts about your
# verdict, and collapsing them loses the one that explains a mid-run red.
run_gate_foreign_before="" ; run_gate_table_ok_before=0 ; run_gate_recycled_before=""
run_gate_foreign_after=""  ; run_gate_table_ok_after=0  ; run_gate_recycled_after=""
run_gate_sample_contention() {   # <before|after> — scan, then RECORD that sample
    run_gate_scan_contention
    # ⚠ The recording lives HERE and not at the end of the scan: that function
    # has three early returns (no table, empty table, no build directory), and a
    # capture written after them would silently skip exactly the samples whose
    # answer this line is about.
    if [ "$1" = before ]; then
        run_gate_foreign_before="$run_gate_foreign"
        run_gate_table_ok_before="$run_gate_table_ok"
        run_gate_recycled_before="$run_gate_recycled"
    else
        run_gate_foreign_after="$run_gate_foreign"
        run_gate_table_ok_after="$run_gate_table_ok"
        run_gate_recycled_after="$run_gate_recycled"
    fi
}

# The union of the two samples, one row per pid:
#   pid <TAB> image <TAB> when-it-was-seen <TAB> command-line
# ⓘ awk does the folding rather than a shell loop, and it also writes the sample
# tag as a LEADING column so nothing has to be escaped. `sed` is deliberately
# NOT used to write that column — BSD sed does not honour `\t` in a replacement,
# so on the darwin carriage the tag and the pid would arrive FUSED into one
# field and every row would fold under its own key.
run_gate_tag_foreign_rows() {   # <tag> — stdin: pid <TAB> image <TAB> cmdline
    awk -F'\t' -v tag="$1" '$1 != "" { print tag "\t" $0 }'
}
run_gate_foreign_union() {
    {
        [ -n "$run_gate_foreign_before" ] && printf '%s\n' "$run_gate_foreign_before" | run_gate_tag_foreign_rows B
        [ -n "$run_gate_foreign_after"  ] && printf '%s\n' "$run_gate_foreign_after"  | run_gate_tag_foreign_rows A
        :
    } | awk -F'\t' '
        {
            pid = $2
            if (pid == "") next
            if (!(pid in seen)) { order[++k] = pid; img[pid] = $3; cl[pid] = $4 }
            seen[pid] = seen[pid] $1
        }
        END {
            for (i = 1; i <= k; i++) {
                p = order[i]
                if (index(seen[p], "B") && index(seen[p], "A")) w = "throughout this run"
                else if (index(seen[p], "B"))                   w = "when this run STARTED"
                else                                            w = "when this run ENDED"
                printf "%s\t%s\t%s\t%s\n", p, img[p], w, cl[p]
            }
        }'
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
        echo "  ⓘ At least one spelling above is RELATIVE and THAT process's own working directory" >&2
        echo "    could not be read here (Windows does not expose it; on Linux and macOS it is read" >&2
        echo "    from /proc or lsof, so reaching this sentence there means that read failed), so" >&2
        echo "    this guard resolved it against THIS shell's working directory ($(pwd -P))." >&2
        echo "    If that process is really standing in a DIFFERENT tree, this is a false refusal;" >&2
        echo "    read the command line printed above before assuming it is not." >&2
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
#   U <witness> <path>      a CHANGE WITNESS the file system keeps and user space
#                           cannot put back — see the block below
# ⓘ The owner record of the log path (`<log>.run-gate-*`) is bookkeeping too, and is
#   excluded for the same reason as `<log>.inputs-*`.
run_gate_snapshot_inputs() {
    {
        run_gate_abs_input_roots | while IFS= read -r _rg_root; do
            [ -n "$_rg_root" ] || continue
            [ -d "$_rg_root" ] || continue
            find "$_rg_root" -type f ! -name "$run_gate_bookkeeping_glob" ! -name "$run_gate_owner_glob" \
                -exec cksum {} + 2>/dev/null | sed 's/^/C /'
            find "$_rg_root" -type f ! -name "$run_gate_bookkeeping_glob" ! -name "$run_gate_owner_glob" \
                -newer "$run_gate_marker" -print 2>/dev/null | sed 's/^/N /'
        done
        run_gate_change_witness_lines
    } | LC_ALL=C sort
}

# ══ A FILE CHANGED AND PUT BACK MID-RUN IS STILL A MOVED TREE ═══════════════════
# [[D-SCRIPT-RUN-GATE-INPUTS-HELD-STILL-OVER-A-FILE-CHANGED-AND-RESTORED-MID-RUN]]
#
# ★★★ `C` AND `N` COMPARE THE TREE AT THE TWO ENDS OF THE RUN, AND A TEST READS IT IN
# THE MIDDLE. A file changed after the first snapshot and restored before the second
# carries its old bytes (C sees nothing) and, when the restore also restores its
# timestamp, an mtime older than the marker (N sees nothing) — while every test that
# read it in between saw the changed bytes.
# ✔MEASURED 2026-09-15 (P66, lane `pg`), both twins, a gated command that read a
# watched file, let it be changed, READ THE CHANGED BYTES, and let it be restored:
#     restored by a plain rewrite ........... exit 3 (N saw the new mtime)
#     restored by `cp -p` ................... exit 0, `inputs  : held still`
#     restored by PowerShell `Copy-Item` .... exit 0, `inputs  : held still`
#     rewritten, then `touch -r` ............ exit 0, `inputs  : held still`
# `Copy-Item` from a scratch copy is exactly the undo this repository prescribes to a
# lane that corrupts its own file, and the red-on-disable workflow restores that way.
#
# ★★ SO A THIRD HALF, `U`, COMPARES A VALUE THE FILE SYSTEM ADVANCES ON EVERY CHANGE
# AND NO USER-SPACE WRITE CAN SET BACK — by EQUALITY, like `C`, never by order.
#   · POSIX: the status-change time (ctime). `utimes` cannot set it; ✔MEASURED on WSL
#     ext4, a `cp -p` restore and a rewrite + `touch -r` both moved it, and `cat`,
#     `cksum` and `find` did not.
#   · Windows: NOT ctime. ✔MEASURED on NTFS: `Copy-Item` puts ChangeTime BACK to the
#     source copy's value, which MSYS reports as ctime — so the twin that trusted ctime
#     here would still say `held still` over the prescribed restore. The file's USN
#     (FSCTL_READ_FILE_USN_DATA) is what NTFS advances on every change: ✔MEASURED
#     unelevated, unchanged by reads, moved by that very Copy-Item, 1753 files in
#     ~0.24 s. It is read by a PowerShell helper, because no MSYS tool reads it.
# ⚠ WHAT IT DELIBERATELY DOES NOT CHANGE: a tree nobody touched keeps every witness,
#   so it still reads `held still`; a file rewritten BEFORE the run carries the same
#   witness in both snapshots and cancels, exactly as `N` already cancels a stamp that
#   predates the run. A write DURING the run that leaves the bytes identical was
#   already `MOVED` through `N` (✔MEASURED, both twins) and stays so: an identical
#   rewrite truncates first, and a reader inside that window sees a torn file.
# ⚠ WHEN THE WITNESS CANNOT BE READ the run is NOT refused — `C` and `N` still stand —
#   and the footer's `changes :` line says NOT WATCHED and why, on every such run. A
#   refusal here would fail every gate on a host without perl or a USN journal.

# The perl program for POSIX: walks the roots itself (File::Find, core), skips this
# run's bookkeeping, prints `U <ctime> <path>`, and ends with a sentinel so a walk
# that died cannot pass for an empty tree. Arguments: the bookkeeping name prefixes,
# `--`, then the roots.
# ⓘ THE SAME TEXT IS THE .ps1 TWIN'S (`$script:RunGateCtimeWalker`), which hands it to
#   perl on a POSIX host. It carries NO quote characters (`q{}`/`qq{}` instead) because
#   PowerShell's native-argument quoting has differed across versions, and a program that
#   contains no quotes cannot be re-quoted wrongly.
run_gate_ctime_walker='use strict; use File::Find (); use Time::HiRes ();
my @pre; while (@ARGV && $ARGV[0] ne q{--}) { push @pre, shift @ARGV } shift @ARGV;
my $n = 0;
for my $root (@ARGV) {
    next unless -d $root;
    File::Find::find({ no_chdir => 1, wanted => sub {
        my $p = $File::Find::name;
        return unless -f $p && ! -l $p;
        (my $b = $p) =~ s{.*/}{};
        for my $x (@pre) { return if index($b, $x) == 0 }
        my @s = Time::HiRes::stat($p);
        return unless @s;
        print qq{U $s[10] $p\n}; $n++;
    } }, $root);
}
print qq{CTIME-OK $n\n};'

# The PowerShell program for Windows, sent as -EncodedCommand so no quoting and no
# MSYS argument conversion can touch it. Variables $roots/$prefixes/$marker are
# prepended per run. Prints `U <usn> <path>`, then the marker's own USN, the count of
# files reporting USN 0 (a volume with no change journal), and a sentinel.
run_gate_usn_program='$ErrorActionPreference = "Stop"
try {
    if (-not ("RunGate.FileUsn" -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace RunGate {
    public static class FileUsn {
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr inBuf, uint inSize, byte[] outBuf, uint outSize, out uint returned, IntPtr overlapped);
        public static string Of(string path) {
            using (SafeFileHandle h = CreateFileW(path, 0x80, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero)) {
                if (h.IsInvalid) { return "unreadable"; }
                byte[] buf = new byte[1024]; uint got;
                if (!DeviceIoControl(h, 0x000900eb, IntPtr.Zero, 0, buf, (uint)buf.Length, out got, IntPtr.Zero)) { return "unreadable"; }
                ushort major = BitConverter.ToUInt16(buf, 4);
                return (major >= 3 ? BitConverter.ToInt64(buf, 40) : BitConverter.ToInt64(buf, 24)).ToString();
            }
        }
    }
}
"@
    }
    $zero = 0
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        foreach ($f in (Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue)) {
            $skip = $false
            foreach ($x in $prefixes) { if ($f.Name.StartsWith($x)) { $skip = $true } }
            if ($skip) { continue }
            $u = [RunGate.FileUsn]::Of($f.FullName)
            if ($u -eq "0") { $zero++ }
            "U " + $u + " " + ($f.FullName -replace "\\", "/")
        }
    }
    "USN-MARKER " + [RunGate.FileUsn]::Of($marker)
    "USN-ZERO " + $zero
    "USN-OK"
} catch {
    "USN-FAILED " + ($_.Exception.Message -replace "[\r\n]", " ")
}'

run_gate_change_witness=none
run_gate_change_why=""
run_gate_change_state=unavailable
run_gate_usn_shell=""
run_gate_usn_encoded=""

# Decides, once, which witness this host can read. Run after the marker exists.
run_gate_decide_change_witness() {
    run_gate_change_witness=none
    if run_gate_is_windows; then
        for _rg_c in powershell pwsh; do
            if command -v "$_rg_c" >/dev/null 2>&1; then run_gate_usn_shell="$_rg_c"; break; fi
        done
        if [ -z "$run_gate_usn_shell" ]; then
            run_gate_change_why="no PowerShell was found to read NTFS USNs with"
            return
        fi
        _rg_q() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/''/g")"; }
        _rg_pr=""
        while IFS= read -r _rg_r; do
            [ -n "$_rg_r" ] && _rg_pr="${_rg_pr}$(_rg_q "$_rg_r"),"
        done < <(run_gate_abs_input_roots)
        _rg_pm="$(cygpath -m "$run_gate_marker" 2>/dev/null || printf '%s' "$run_gate_marker")"
        _rg_script="\$roots = @(${_rg_pr%,})
\$prefixes = @($(_rg_q "${log##*/}.inputs-"),$(_rg_q "${log##*/}.run-gate-"))
\$marker = $(_rg_q "$_rg_pm")
$run_gate_usn_program"
        if ! run_gate_usn_encoded="$(printf '%s' "$_rg_script" | iconv -f UTF-8 -t UTF-16LE 2>/dev/null | base64 2>/dev/null | tr -d '\n')" \
           || [ -z "$run_gate_usn_encoded" ]; then
            run_gate_change_why="the USN reader could not be encoded for PowerShell (iconv/base64 failed)"
            return
        fi
        run_gate_change_witness=usn
    else
        if [ "$(perl -MTime::HiRes -e 'my @s = Time::HiRes::stat($ARGV[0]); print((@s && $s[10] > 0) ? "ok" : "no")' "$run_gate_marker" 2>/dev/null)" != ok ]; then
            run_gate_change_why="perl with Time::HiRes did not return a status-change time for a file this run had just created"
            return
        fi
        run_gate_change_witness=ctime
    fi
}

run_gate_change_witness_lines() {
    case "$run_gate_change_witness" in
        ctime)
            _rg_wr=()
            while IFS= read -r _rg_r; do
                [ -n "$_rg_r" ] && _rg_wr+=("$_rg_r")
            done < <(run_gate_abs_input_roots)
            perl -e "$run_gate_ctime_walker" "${log##*/}.inputs-" "${log##*/}.run-gate-" -- "${_rg_wr[@]}" 2>/dev/null
            ;;
        usn)
            "$run_gate_usn_shell" -NoProfile -NonInteractive -EncodedCommand "$run_gate_usn_encoded" 2>/dev/null | tr -d '\r'
            ;;
    esac
    return 0
}

# Reads the two finished snapshots IN THE MAIN SHELL (see the note on
# run_gate_diff_inputs) and decides whether the U half may be believed.
run_gate_judge_change_witness() {
    run_gate_change_state=unavailable
    case "$run_gate_change_witness" in
        ctime)
            if grep -q '^CTIME-OK ' "$run_gate_inputs_before" && grep -q '^CTIME-OK ' "$run_gate_inputs_after"; then
                run_gate_change_state=watched
            else
                run_gate_change_why="the perl ctime walk did not complete in both snapshots"
            fi
            ;;
        usn)
            _rg_m1="$(sed -n 's/^USN-MARKER //p' "$run_gate_inputs_before" | head -1)"
            _rg_z1="$(sed -n 's/^USN-ZERO //p' "$run_gate_inputs_before" | head -1)"
            _rg_z2="$(sed -n 's/^USN-ZERO //p' "$run_gate_inputs_after" | head -1)"
            if ! grep -q '^USN-OK$' "$run_gate_inputs_before" || ! grep -q '^USN-OK$' "$run_gate_inputs_after"; then
                run_gate_change_why="the USN reader did not complete in both snapshots ($(sed -n 's/^USN-FAILED //p' "$run_gate_inputs_before" "$run_gate_inputs_after" | head -1))"
            elif [ -z "$_rg_m1" ] || [ "$_rg_m1" = 0 ] || [ "$_rg_m1" = unreadable ]; then
                run_gate_change_why="the log's own volume returned no USN for this run's marker (no change journal?)"
            elif [ "${_rg_z1:-0}" != 0 ] || [ "${_rg_z2:-0}" != 0 ]; then
                run_gate_change_why="${_rg_z1:-?} file(s) under the watched roots report USN 0, i.e. their volume keeps no change journal"
            else
                run_gate_change_state=watched
            fi
            ;;
        *) : ;;
    esac
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
# ⓘ ONLY THE THREE HALVES ARE DIFFERENCED. The witness readers' sentinel lines
#   (`CTIME-OK`, `USN-*`) are judged by run_gate_judge_change_witness, never reported
#   as moved files; and the `U` half is believed only when that judge said `watched`.
run_gate_diff_inputs() {
    {
        LC_ALL=C comm -23 "$run_gate_inputs_before" "$run_gate_inputs_after"
        LC_ALL=C comm -13 "$run_gate_inputs_before" "$run_gate_inputs_after"
    } 2>/dev/null | grep -E '^[CNU] ' \
      | if [ "$run_gate_change_state" = watched ]; then cat; else grep -v '^U '; fi \
      | sed -e 's/^C [0-9][0-9]* [0-9][0-9]* //' -e 's/^N //' -e 's/^U [^ ][^ ]* //' \
      | LC_ALL=C sort -u | head -20
}
# ══ THE LOG PATH IS ONE LIVE RUN'S ALONE ══════════════════════════════════════
# [[D-SCRIPT-WSL-LEG-AND-RUN-GATE-LET-CONCURRENT-LEGS-SHARE-ONE-LOG-PATH]]
#
# ★★★ THE SIXTH WAY A GATE'S EXIT CODE CAN MEAN NOTHING, AND IT NEEDS NO DEFECT IN
# THE GATE COMMAND: two runs handed ONE LOG PATH. This file truncates its log,
# appends the command's output to it, greps it for the witness and keeps its state
# files beside it (`<log>.inputs-*`), so a second run on the same path erases the
# first's evidence, interleaves both commands' output, and deletes the first's
# fingerprints when it cleans up after itself.
# ✔MEASURED 2026-09-15 (P66, lane `pg`), a live first run whose command printed NO
#   witness and a second run on the same path whose command printed one:
#     .sh then .sh    first run exit 3 (its `.inputs-before` deleted by the second
#                     run's cleanup); second run exit 0 OK over a log a live run was
#                     writing; the first run's own output ERASED from the log
#     .sh then .ps1   both exit 1, and NEITHER run's output survived in the log
#     .ps1 then either  second run exit 2: the .ps1 twin's open log handle refused it
#                     by accident, and in one direction only
#   and on two REAL `wsl-leg.sh` runs in two clones sharing `/tmp/wsl-leg-ctest.log`,
#   a leg reported `WSL leg OK` over a log naming the OTHER clone's build directory.
# ⇒ exit 5, with nothing touched: this run takes the path's OWNER RECORD
#   (`<log>.run-gate-owner`) by EXCLUSIVE CREATE before it truncates anything, and
#   refuses to start while another LIVE run-gate holds it. Both twins write and honour
#   the same record, so a .sh run and a .ps1 run on one path exclude each other too.
#
# ★★ "LIVE" IS DECIDED BY THE PROCESS TABLE, BY THE RULE THIS FILE ALREADY TRUSTS.
#   The record names the holder's pid IN THE TABLE'S NAMESPACE and its creation key —
#   the key "A PARENT LINK IS A CLAIM ABOUT ORDER" compares. The holder is gone only
#   when a FRESH table lacks that pid, or shows it with a DIFFERENT creation key (a
#   recycled pid); only then is the record reclaimed. Every other state REFUSES: an
#   unreadable table, a record written in another pid namespace (a WSL run and a
#   Windows run share `/mnt/c`, and ✔MEASURED even `uname -n`), a record that is
#   empty or half-written. A refusal that should not have fired costs a retry; a
#   reclaim that should not have happened is the silent trade this block ends.
# ★★ A RECLAIM IS ITSELF A RACE, MADE ATOMIC BY A HARD LINK. Two runs that both judge
#   one dead record stale must not both delete-and-create. A reclaimer first LINKS the
#   record to a tombstone named for the dead holder's token — `ln` fails when that name
#   exists, so exactly one reclaimer wins — and re-reads the tombstone's token before
#   deleting anything, so a record replaced in between is never taken for the one judged.
# ⓘ NO ESCAPE HATCH: the path is the caller's own argument, so "use another log path"
#   is always available and nothing here is settable.
# ⓘ A run killed by SIGKILL or TerminateProcess leaves its record behind (no EXIT trap
#   runs then); the liveness rule reclaims it on the next run, and the footer says so.
run_gate_log_held_exit=5
run_gate_owner="${log}.run-gate-owner"
run_gate_owner_glob="${log##*/}.run-gate-*"
run_gate_owner_token=""
run_gate_owner_held=0
run_gate_owner_reclaimed=""
run_gate_owner_verdict=""
run_gate_owner_why=""
run_gate_owner_holder=""
run_gate_owner_stale_token=""
run_gate_self_table_pid=""
run_gate_ns=""

# THIS SHELL'S PID NAMESPACE, spelled exactly as the .ps1 twin spells it: the OS
# family, the host name lowercased, and on Linux the pid namespace's own inode, because
# two WSL distros share a host name and a `/mnt/c` but not a process table.
run_gate_namespace() {
    _rg_nh=""; _rg_nn=""
    case "$(uname -s 2>/dev/null || echo unknown)" in
        MINGW*|MSYS*|CYGWIN*) _rg_nk=windows; _rg_nh="${COMPUTERNAME:-$(uname -n 2>/dev/null)}" ;;
        Linux)                _rg_nk=linux;   _rg_nh="$(uname -n 2>/dev/null)"; _rg_nn="$(readlink /proc/self/ns/pid 2>/dev/null)" ;;
        Darwin)               _rg_nk=darwin;  _rg_nh="$(uname -n 2>/dev/null)" ;;
        *)                    _rg_nk="$(uname -s 2>/dev/null | tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz')"; _rg_nh="$(uname -n 2>/dev/null)" ;;
    esac
    printf '%s:%s:%s' "$_rg_nk" "$(printf '%s' "$_rg_nh" | tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz')" "$_rg_nn"
}

run_gate_owner_field() {   # <record file> <key>
    sed -n "s/^$2=//p" "$1" 2>/dev/null | head -1 | tr -d '\r'
}

# Sets run_gate_owner_verdict to held | unknown | stale, with run_gate_owner_why and
# run_gate_owner_holder for the message. Only `stale` may lead to a reclaim.
run_gate_judge_owner() {
    run_gate_owner_verdict=held
    run_gate_owner_stale_token=""
    _rg_ot="$(run_gate_owner_field "$run_gate_owner" token)"
    _rg_op="$(run_gate_owner_field "$run_gate_owner" pid)"
    _rg_oc="$(run_gate_owner_field "$run_gate_owner" created)"
    _rg_on="$(run_gate_owner_field "$run_gate_owner" namespace)"
    run_gate_owner_holder="pid ${_rg_op:-?} ($(run_gate_owner_field "$run_gate_owner" shell)), running: $(run_gate_owner_field "$run_gate_owner" command)"
    case "$_rg_ot" in
        ''|*[!A-Za-z0-9-]*)
            run_gate_owner_why="its owner record is empty, half-written or unreadable -- another run-gate may be writing it this instant, or one died while writing it"
            return ;;
    esac
    case "$_rg_op" in
        ''|*[!0-9]*)
            run_gate_owner_why="its owner record names no usable pid"
            return ;;
    esac
    if [ "$_rg_on" != "$run_gate_ns" ]; then
        run_gate_owner_verdict=unknown
        run_gate_owner_why="the record was written in pid namespace '$_rg_on' and this shell runs in '$run_gate_ns', whose process table cannot see that holder"
        return
    fi
    _rg_otbl="$(run_gate_process_table)"
    if [ -z "$_rg_otbl" ]; then
        run_gate_owner_verdict=unknown
        run_gate_owner_why="no process table could be read on this host, so the holder could not be shown to be gone"
        return
    fi
    _rg_ocur="$(printf '%s\n' "$_rg_otbl" | awk -F'\t' -v p="$_rg_op" '$1 == p { print "L" $6; f = 1; exit } END { if (!f) print "A" }')"
    if [ "$_rg_ocur" = A ]; then
        run_gate_owner_verdict=stale
        run_gate_owner_why="pid $_rg_op is not alive"
    elif [ "$_rg_op" = "$run_gate_self_table_pid" ]; then
        run_gate_owner_verdict=stale
        run_gate_owner_why="pid $_rg_op is THIS run, so the holder it named is gone and its pid was reused"
    else
        _rg_ocur="${_rg_ocur#L}"
        if printf '%s' "$_rg_oc" | grep -qE '^[0-9]{20}$' \
           && printf '%s' "$_rg_ocur" | grep -qE '^[0-9]{20}$' \
           && [ "$_rg_oc" != "$_rg_ocur" ]; then
            run_gate_owner_verdict=stale
            run_gate_owner_why="pid $_rg_op is alive but was created at $_rg_ocur, not at $_rg_oc -- a RECYCLED pid, not the holder"
        else
            run_gate_owner_why="pid $_rg_op is alive${_rg_ocur:+ (created $_rg_ocur)}"
            return
        fi
    fi
    run_gate_owner_stale_token="$_rg_ot"
}

# 0 when the record judged stale is gone; 1 when a sibling moved first, the record was
# replaced in between, or no hard link could be made beside the log.
run_gate_reclaim_owner() {
    _rg_tomb="${log}.run-gate-stale-$run_gate_owner_stale_token"
    ln "$run_gate_owner" "$_rg_tomb" 2>/dev/null || return 1
    if [ "$(run_gate_owner_field "$_rg_tomb" token)" != "$run_gate_owner_stale_token" ]; then
        rm -f "$_rg_tomb"
        return 1
    fi
    rm -f "$run_gate_owner"
    rm -f "$_rg_tomb"
    return 0
}

# 0 the record is this run's | 5 another live run-gate holds the path | 2 not creatable
run_gate_acquire_log() {   # <the gate command's argv>
    run_gate_self_table_pid="$(run_gate_self_pid)"
    run_gate_owner_token="$$-$(date +%s 2>/dev/null || echo 0)-$RANDOM$RANDOM"
    _rg_body="run-gate-owner-record: while this file exists a LIVE run-gate holds the log path beside it
token=$run_gate_owner_token
pid=$run_gate_self_table_pid
created=$run_gate_self_created
namespace=$run_gate_ns
shell=run-gate.sh
command=$(printf '%s' "$*" | tr '\r\n' '  ')
"
    _rg_try=0
    _rg_reclaim_failed=0
    while [ "$_rg_try" -lt 5 ]; do
        _rg_try=$((_rg_try + 1))
        # `set -C` is an EXCLUSIVE create (O_EXCL; ✔MEASURED to refuse an existing file
        # under Git Bash too), so of two runs racing for a free path exactly one wins.
        if ( set -C; printf '%s' "$_rg_body" > "$run_gate_owner" ) 2>/dev/null; then
            run_gate_owner_held=1
            return 0
        fi
        [ -e "$run_gate_owner" ] || continue
        run_gate_judge_owner
        [ "$run_gate_owner_verdict" = stale ] || return 5
        if run_gate_reclaim_owner; then
            run_gate_owner_reclaimed="$run_gate_owner_holder -- $run_gate_owner_why"
        else
            _rg_reclaim_failed=1
        fi
    done
    [ -e "$run_gate_owner" ] || return 2
    if [ "$_rg_reclaim_failed" -eq 1 ]; then
        run_gate_owner_why="$run_gate_owner_why; and the stale record could not be reclaimed (no hard link could be made beside the log, or a sibling kept replacing it)"
    fi
    return 5
}

# Runs from the EXIT trap. Removes the record only while it is still this run's.
run_gate_release_log() {
    [ "$run_gate_owner_held" -eq 1 ] || return 0
    if [ "$(run_gate_owner_field "$run_gate_owner" token)" = "$run_gate_owner_token" ]; then
        rm -f "$run_gate_owner"
    fi
    run_gate_owner_held=0
}

run_gate_refuse_log_held() {
    echo "run-gate.sh: FAIL — ANOTHER LIVE RUN-GATE HOLDS THIS LOG PATH, so nothing was run and its log was not touched (rc=$run_gate_log_held_exit)." >&2
    echo "  log     : $log" >&2
    echo "  record  : $run_gate_owner" >&2
    echo "  holder  : $run_gate_owner_holder" >&2
    echo "  judged  : $run_gate_owner_why" >&2
    echo "  ⚠ This is NOT 'the gate failed'. Two runs on one log path truncate and interleave each" >&2
    echo "    other's output and delete each other's fingerprints, and a witness grep can then find" >&2
    echo "    the OTHER run's success line. ✔MEASURED: a leg reported OK over a log that named" >&2
    echo "    another clone's build directory." >&2
    echo "  Give this gate its own log path, or wait for that run to finish. If you are CERTAIN" >&2
    echo "    nothing uses this path (for instance the holder ran in another OS namespace and is" >&2
    echo "    gone), delete the record by hand: $run_gate_owner" >&2
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
run_gate_sample_contention before

# ── THE LOG PATH IS TAKEN BEFORE ONE BYTE OF IT IS TOUCHED ──────────────────
# After the pre-run sample, so the record carries this run's creation key from the
# table that sample just read; before the truncate, so a refused run erases nothing.
run_gate_ns="$(run_gate_namespace)"
run_gate_acquire_log "$@"
case "$?" in
    0) trap run_gate_release_log EXIT ;;
    5) run_gate_refuse_log_held; exit "$run_gate_log_held_exit" ;;
    *) run_gate_refuse_log_path "the log's owner record '$run_gate_owner'"; exit 2 ;;
esac

# Truncate rather than append: a stale log from a previous run is itself a way
# to "find" a success witness that this invocation never produced.
# ⚠ `{ …; } 2>/dev/null` and NOT `: > "$log" 2>/dev/null`. Redirections are set up
# LEFT TO RIGHT, so in the second form the failing `>` reports to the ORIGINAL
# stderr before `2>` is ever established — ✔MEASURED: bash's raw
# `line NNN: C:/…: No such file or directory` printed AHEAD of the named refusal,
# which is the anonymous noise the refusal exists to replace. The group form
# establishes the group's stderr first, so only our sentence survives.
if ! { : > "$log"; } 2>/dev/null; then
    run_gate_refuse_log_path "the log '$log'"
    exit 2
fi

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

# Which change witness this host can read — see "A FILE CHANGED AND PUT BACK MID-RUN
# IS STILL A MOVED TREE". Decided once, before the first snapshot, so both snapshots
# carry the same halves.
run_gate_decide_change_witness

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

# ★★★ THE WITNESS IS READ HERE, FROM THE COMMAND'S OWN OUTPUT, BEFORE THIS WRAPPER
# WRITES ONE WORD INTO THE LOG. Found and fixed in the lane that closed
# D-TEST-RUN-GATE-FIXTURE-RACES-FIXED-LIFETIME-PROCESSES-AGAINST-THE-GATES-SAMPLING-LATENCY
# ✔MEASURED 2026-09-14, both twins: the footer below records `command : <argv>`,
# and the witness used to be grepped over the WHOLE log after it — so
#   run-gate.sh <log> 'ZQX-WITNESS' bash -c ': ZQX-WITNESS'
# a command that printed NOTHING, came back `run-gate.sh: OK`, and the .ps1 twin
# said the same of `$null = "ZQX-WITNESS"`. The evidence the wrapper found was the
# sentence it had just written — exactly the case the witness exists to refuse: an
# exit code with no work behind it, whenever a witness is also spelled in the argv.
# ⓘ Only the RESULT is taken here. The refusal order below is unchanged, so a moved
#   tree, a contended build directory, 127 and a non-zero rc still outrank it.
run_gate_witness_seen=0
grep -qE "$witness" "$log" && run_gate_witness_seen=1

run_gate_snapshot_ok=1
if [ ! -f "$run_gate_inputs_before" ]; then
    run_gate_snapshot_ok=0
elif ! run_gate_snapshot_inputs > "$run_gate_inputs_after" 2>/dev/null; then
    run_gate_snapshot_ok=0
elif [ ! -f "$run_gate_inputs_after" ]; then
    run_gate_snapshot_ok=0
fi
run_gate_moved=""
if [ "$run_gate_snapshot_ok" -eq 1 ]; then
    # IN THE MAIN SHELL, before the diff reads it — see run_gate_diff_inputs.
    run_gate_judge_change_witness
    run_gate_moved="$(run_gate_diff_inputs)"
fi
rm -f "$run_gate_marker" "$run_gate_inputs_before" "$run_gate_inputs_after"

# ── POST-RUN: a sibling can start MID-RUN, so the same question is asked again ──
run_gate_sample_contention after

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
    # ★★ WHETHER `held still` COVERS THE MIDDLE OF THE RUN, ON EVERY RUN — see "A FILE
    # CHANGED AND PUT BACK MID-RUN IS STILL A MOVED TREE". Two states, and the second is
    # not spelled as a pass: a reader who skims for `held still` must still land on a
    # line that says what the two ends could not see.
    if [ "$run_gate_change_state" = watched ]; then
        case "$run_gate_change_witness" in
            usn) echo "changes : watched through every file's NTFS USN — a change undone before the run ended is still seen" ;;
            *)   echo "changes : watched through every file's status-change time (ctime) — a change undone before the run ended is still seen" ;;
        esac
    else
        echo "changes : NOT WATCHED MID-RUN — ${run_gate_change_why:-no change witness was read}; a file changed and restored during this run is not ruled out"
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
    # THE LOG PATH'S OWNERSHIP, ON EVERY RUN THAT GOT THIS FAR — see "THE LOG PATH IS
    # ONE LIVE RUN'S ALONE". A reclaim changed what this run was allowed to do, so it
    # is named rather than left for a reader to infer from a vanished file.
    if [ -n "$run_gate_owner_reclaimed" ]; then
        echo "logpath : held by this run alone for its whole duration ($run_gate_owner) — it first RECLAIMED a stale record: $run_gate_owner_reclaimed"
    else
        echo "logpath : held by this run alone for its whole duration ($run_gate_owner)"
    fi
    # HOW MANY OF THE RUN'S TWO SAMPLES ACTUALLY READ A PROCESS TABLE. Both
    # lines below are claims about a scan, and neither may assert more than the
    # scans it got — computed once here so the two cannot answer differently.
    run_gate_samples_read=$((run_gate_table_ok_before + run_gate_table_ok_after))
    if [ -z "$run_gate_build_dir" ]; then
        echo "builddir: none named by this command — the contention check had no subject"
    else
        echo "builddir: $run_gate_build_dir"
        if [ -n "$run_gate_contenders" ]; then
            echo "contended: YES, ANOTHER RUN WAS LIVE IN IT — this verdict is not evidence"
            printf '%s' "$run_gate_contenders"
        elif [ "$run_gate_samples_read" -eq 0 ]; then
            echo "contended: UNKNOWN — no process table could be read on this host (0 of this run's 2 samples), so nothing was ruled out"
        else
            # ⚠ THE WORDING IS NARROWED, AND THE OLD ONE WAS NOT WRONG — IT WAS
            # OVER-READ, WHICH IS WORSE. It said "this run was alone in it", and
            # "it" is the BUILD DIRECTORY, which was true of the run that took
            # two false reds from a second compiler. A line that is true and
            # invites the wrong conclusion costs more than one that is false,
            # because nobody re-checks it. The subject is now named in the
            # sentence, and the machine-wide subject has a line of its own.
            #
            # ⚠ AND IT COUNTS ITS SAMPLES FOR THE SAME REASON THE `compilers:`
            # LINE BELOW DOES. This used to read the LAST scan's `table_ok`
            # alone, so a run whose pre-run scan could not look — the scan whose
            # whole job is to refuse BEFORE anything executes — still printed a
            # flat `contended: no`. That is this row's defect one line up, and
            # leaving it there while fixing the sibling would repeat the exact
            # mistake the row was opened for.
            # [[D-SCRIPT-RUN-GATE-COMPILERS-LINE-REPORTS-NONE-WHEN-IT-COULD-NOT-READ-THE-PROCESS-TABLE]]
            if [ "$run_gate_samples_read" -lt 2 ]; then
                echo "contended: no — no other build-tool run named THIS BUILD DIRECTORY ($run_gate_unreadable candidate process(es) had no readable command line and could not be judged), but only $run_gate_samples_read of this run's 2 samples could read a process table at all, so the other one ruled nothing out. ⚠ This says NOTHING about the rest of the machine; see 'compilers:' below."
            else
                echo "contended: no — no other build-tool run named THIS BUILD DIRECTORY ($run_gate_unreadable candidate process(es) had no readable command line and could not be judged). ⚠ This says NOTHING about the rest of the machine; see 'compilers:' below."
            fi
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
    #
    # ★★★ THREE STATES, AND THE THIRD IS NOT SPELLED AS A PASS. `none` and
    # `UNKNOWN` are different answers and the second one is not an answer at
    # all: a reader skimming for `compilers: none` must miss the blind case, and
    # a reader skimming for `compilers:` must land on something that says so.
    # The sample count is printed with every one of them, because "I looked
    # twice" and "I looked once and could not look the second time" are not the
    # same evidence for the same sentence.
    run_gate_union="$(run_gate_foreign_union)"
    run_gate_union_count=0
    [ -n "$run_gate_union" ] && run_gate_union_count="$(printf '%s\n' "$run_gate_union" | grep -c . || true)"
    if [ "$run_gate_samples_read" -eq 0 ]; then
        echo "compilers: UNKNOWN — NO PROCESS TABLE COULD BE READ on this host (0 of this run's 2 samples), so no other compiler was ruled out"
    elif [ "$run_gate_union_count" -eq 0 ]; then
        if [ "$run_gate_samples_read" -lt 2 ]; then
            echo "compilers: none outside this gate's own process tree — but only $run_gate_samples_read of this run's 2 samples could read a process table, so the other one ruled nothing out"
        else
            echo "compilers: none outside this gate's own process tree, in either of this run's 2 samples"
        fi
    else
        echo "compilers: $run_gate_union_count '$run_gate_compiler_image' process(es) ran OUTSIDE this gate's process tree DURING this run — they share this user's compiler caches with this run, which are NOT under srctree or builddir"
        printf '%s\n' "$run_gate_union" |
            awk -F'\t' '$1 != "" { printf "          pid %s  %s  seen %s\n            %s\n", $1, $2, $3, $4 }'
    fi
    # ★ A LINK THE RULE REFUSED IS NAMED, because refusing it CHANGED a verdict: a
    # process whose parent pid points into this gate's tree was judged NOT to
    # descend from it. Without this line the judgement is invisible, and a reader
    # lining up the pid columns by hand would call it a defect. Printed only when
    # a link was refused; an observation, never a refusal.
    # D-TEST-RUN-GATE-FIXTURE-RACES-FIXED-LIFETIME-PROCESSES-AGAINST-THE-GATES-SAMPLING-LATENCY
    run_gate_recycled_union="$(printf '%s%s' "$run_gate_recycled_before" "$run_gate_recycled_after" | awk 'NF && !seen[$0]++')"
    if [ -n "$run_gate_recycled_union" ]; then
        echo "ancestry: $(printf '%s\n' "$run_gate_recycled_union" | grep -c .) parent link(s) NOT followed — each named a parent created AFTER its child, i.e. a RECYCLED pid, not an ancestor (child>parent): $(printf '%s\n' "$run_gate_recycled_union" | head -8 | tr '\n' ' ')"
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

if [ "$run_gate_witness_seen" -ne 1 ]; then
    echo "run-gate.sh: FAIL — command exited 0 but its output never matched the" >&2
    echo "  success witness /$witness/, so there is NO EVIDENCE it did any work." >&2
    echo "  An exit code alone cannot distinguish 'passed' from 'never ran'." >&2
    echo "  (log: $log)" >&2
    tail -20 "$log" >&2
    exit 1
fi

echo "run-gate.sh: OK — rc=0 and the success witness /$witness/ was present."
exit 0
