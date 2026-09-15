#!/usr/bin/env bash
# test-run-gate.sh -- prove BOTH run-gate twins refuse a run whose evidence is
# spoiled, and that they still pass a run whose evidence is intact.
#
# ELEVEN SUBJECTS, one fixture, because they are one contract:
#   * the SOURCE TREE moving under the run          -> exit 3
#   * ANOTHER RUN live in the same BUILD DIRECTORY  -> exit 4
#   * WHICH TREE those roots are read from at all   -> 3 or 0, and which one it
#     was must be readable from the log ALONE
#   * whether a file's TIMESTAMP can decide either answer -> it must not, in
#     EITHER direction, because one carriage's clock is not monotonic
#   * a COMPILER running outside this gate's process tree -> NAMED, never refused,
#     and asserted BY PID, so unrelated compilers on the machine decide nothing
#   * a PARENT LINK that names a RECYCLED PID       -> never followed
#   * a WITNESS found only in the wrapper's OWN footer -> exit 1, not evidence
#   * a LOG PATH another LIVE run-gate holds         -> exit 5, its log untouched;
#     a dead holder's record reclaimed, an unjudgeable one refused
#   * a file CHANGED AND RESTORED during the run     -> exit 3, whatever the restore
#     did to its bytes and timestamps
#   * a RELATIVE build directory in ANOTHER process's tree -> not a contender where
#     that process's directory can be read; a stated assumption where it cannot
#   * a PROCESS TABLE big enough to fill a pipe      -> the pre-run scan completes
# ⓘ AND IT RUNS IN A SCRATCH DIRECTORY OF ITS OWN PER INVOCATION, so two gates of one
#   checkout can run it at once (see ONE SCRATCH DIRECTORY PER INVOCATION below).
#
# ⚠⚠ THE THIRD SUBJECT IS THE ONE WHOSE FAILURE IS SILENT, and that is why it is
#   proved in BOTH directions rather than only the refusing one. The input roots
#   are three RELATIVE names, and what they were relative to used to be the
#   PROCESS WORKING DIRECTORY. ✔MEASURED 2026-09-08 (P65) on both twins, with the
#   cwd in tree B and the gate command naming tree A's build directory:
#     · an edit in B (a tree the run never reads) -> exit 3, a LOUD FALSE
#       REFUSAL that throws away a quarter-hour gate;
#     · an edit in A (the tree whose config the run's tests actually read)
#       -> exit 0 and `inputs  : held still`, THE SILENT WRONG ANSWER.
#   The second is the reason this subject exists: a refusal that fires wrongly
#   costs a run, and a `held still` that is wrong ships a verdict that has none.
#
# A SIBLING FILE, not a `--selftest` flag: run-gate's interface is POSITIONAL and
# its first argument is a LOG PATH, which it refuses when it begins with '-' (see
# the long block about that in run-gate.sh). A flag could not be spelled without
# colliding with that refusal, so the proof lives beside the subject instead --
# the same shape as scripts/lane-worktree/test-lane-worktree.sh.
# ⓘ `__stand-in <verb>` IS NOT A FLAG OF THE SUBJECT. It is this FIXTURE calling
#   back into itself for the processes it plants (see THE STAND-IN LIFECYCLE), so
#   that every piece of choreography lives in one reviewed file rather than in a
#   helper generated at run time.
#
# ! THE CONTROLS ARE THE POINT. Arms 1, 4, 7, 9, 14 and 16 must PASS: without
#   them, a green refusal arm is equally consistent with "this wrapper now
#   refuses everything".
# ! The mutations are REAL -- a real edit to a real read-at-test-time root while
#   the gate command runs, a real second `ctest` alive in the same build
#   directory, a real compiler-named process alive beside the gate -- rather than
#   simulations with a pre-dated marker or a fake process table.
#   ⚠ WITH ONE NAMED EXCEPTION, AND ITS REASON: arms 31-33 hand the subject's OWN
#   classifier a CONSTRUCTED table, because the state they are about cannot be
#   produced on demand. ✔MEASURED 2026-09-14: Windows hands a freed pid back only
#   after ~108 allocations, and to a process nobody chooses; every POSIX carriage
#   REPARENTS an orphan, so the state is unreachable there at all. The REAL arms
#   23, 24 and 28 remain the proof that the real table carries keys that work.
#
# ⚠⚠ THIS FIXTURE RUNS INSIDE A SANDBOX TREE AND MUST KEEP DOING SO.
#   ✔MEASURED 2026-09-07 (P63): the first version hard-coded
#   `cd /c/Source/DailySoftware/dss-code-prime` -- an ABSOLUTE path to the main
#   checkout -- and then created and deleted `examples/.test-run-gate-input-probe.txt`
#   THERE. Two defects in one line, both of the wrong-root class this repository
#   has already anchored for its guards
#   (D-SCRIPT-GUARDS-ASK-GIT-FROM-THE-LANE-WORKTREE):
#     (a) run from a lane worktree it proved the MAIN tree's run-gate, i.e. the
#         one file the lane had not changed;
#     (b) `examples/` is one of run-gate's OWN input roots, so running this
#         fixture while anyone else gated the main checkout made THEIR gate
#         exit 3 naming a file they never touched -- and it is registered in
#         ctest, so "anyone else" includes the gate that is running it.
#   ⇒ the root comes from BASH_SOURCE, and every arm runs in a SYNTHETIC tree
#     under the scratch directory that carries its own `examples/`. Nothing here
#     touches the repository's own input roots or build directories.
#
# ⚠ TWO TRAPS THIS FIXTURE HIT ON ITS FIRST RUN, both in the FIXTURE and both
#   worth keeping written down, because either one makes an arm pass or fail for
#   a reason that has nothing to do with the subject:
#   (a) `cmd.exe /c ...` from Git Bash arrives as `cmd.exe C:/ ...` -- MSYS
#       rewrites a leading-slash argument into a DOS path, so the command never
#       ran as written and the arm "passed" for the wrong reason. Every
#       PowerShell-side arm therefore drives `powershell -Command`, which takes
#       no leading-slash arguments.
#   (b) the .ps1 twin's log is UTF-16, so a plain `grep` finds nothing in a file
#       that plainly contains the string. Read it through `logtext`, never
#       directly -- otherwise this fixture reports a defect in the subject that
#       belongs to the reader.
#
# ═══ NO ARM MAY DEPEND ON THE WALL CLOCK ══════════════════════════════════════
# [[D-TEST-RUN-GATE-FIXTURE-RACES-FIXED-LIFETIME-PROCESSES-AGAINST-THE-GATES-SAMPLING-LATENCY]]
# ⚠⚠⚠ CI WENT RED ON THIS FIXTURE THREE TIMES, AND IT WAS "FIXED" TWICE BY MOVING A
#   NUMBER. The primitive was `plant_foreign_compiler <seconds>`: a copy of `sleep`
#   with a FIXED lifetime, planted and forgotten, and every compiler arm then
#   ASSUMED the subject would sample inside that lifetime -- a claim about HOST
#   SPEED, never about run-gate. ✔MEASURED 2026-09-14 on the Windows workstation,
#   against COPIES of both twins slowed immediately ahead of their first
#   process-table read (the shipped scripts untouched):
#     · arm 28's 3 s stand-in was still named at +2.0 s of delay and missed at
#       +2.5 s, on BOTH twins. Unslowed, the .sh twin finishes its first sample
#       1.3-1.45 s after planting and the .ps1 twin 1.0-1.2 s, so a runner that is
#       2.3x slower puts the .sh twin at the edge and the .ps1 twin under it --
#       which is the CI signature;
#     · at +5 s this fixture printed that signature exactly: 28 red on
#       `compilers: none`, 29 green, `30-parity-union .sh reported=0 .ps1 reported=1`;
#     · at +3 s it went GREEN, and `a28.log` named ARM 24's 12 s stand-in, still
#       alive: an arm PASSING on a leftover of the arm before it.
#   And ctest holds a test's output until the test ends (✔MEASURED with
#   `--output-on-failure`, the CI flag: three lines written one second apart
#   arrived 30 ms apart, after the test), which is why the CI log could not say
#   which arm was slow however this file flushed.
# ⇒ THE CHOREOGRAPHY, and each clause removes one way the clock could decide:
#   · a stand-in has NO lifetime. It lives until this fixture stops it, by its
#     recorded PID and never by image name -- a real build on the same machine
#     runs real `dsscp` -- and one this fixture could not stop ends itself at a
#     LEAK bound that is reported as a failure, never counted as a stop;
#   · every plant BLOCKS until the stand-in is observed present and every stop
#     BLOCKS until it is observed gone, by an instrument that is NOT the subject's:
#     its own PID through `kill -0` on POSIX, MSYS's `ps -W` on Windows, never CIM;
#   · a compiler that must exit MID-RUN is stopped by the GATED COMMAND. Both twins
#     take their pre-run sample before they start the command and their post-run
#     sample after it returns, so "alive for the first, gone for the second" holds
#     at any host speed;
#   · every assertion about a stand-in names ITS pid, so no leftover can pass it;
#   · arm 23's ordering is ctest's own DEPENDS, and a contender is released by the
#     same PID stop -- no settle sleeps anywhere;
#   · a bounded wait is a HANG GUARD: exceeding it FAILS a named fixture
#     precondition, is never a skip, and is never reported as run-gate's verdict;
#   · every arm line carries its own clock (`at +S.SSs took S.SSs`, wall clock,
#     so WSL's stepping clock can distort it -- diagnostics only), and every
#     stand-in arm prints its independent reading before and after the gate.
#
# ⚠⚠ THE .ps1 ARMS ARE HOST-CONDITIONAL, AND THAT CONDITION IS THE ONLY ESCAPE IN
#   THIS FIXTURE. Not every carriage this project gates on carries a PowerShell,
#   and an arm that CANNOT run must say so -- never fail, never vanish.
#   ★ THE PROBE IS BY EXECUTION, NEVER BY LOOKUP (`run_gate_powershell`): each
#     candidate is actually RUN and has to hand a known token back with rc 0.
#     `command -v` is documented IN THIS REPOSITORY to LIE over a non-interactive
#     ssh session on the macOS carriage -- `scripts/remote-leg/remote-leg.sh`
#     carries the measurement, where tools that exist at `/opt/homebrew/bin` were
#     reported NOT FOUND. A lookup can also fail the other way, and that is the
#     direction that would hurt here: it would name an interpreter that cannot
#     start, and every .ps1 arm would then fail for a reason that is not the
#     subject's.
#   ★ THE CANDIDATE ORDER IS `pwsh` THEN `powershell`, AND IT IS NOT A GUESS: it
#     is exactly `find_program(POWERSHELL_EXE NAMES pwsh powershell REQUIRED)` in
#     `CMakeLists.txt`, so this fixture proves the twin under the SAME interpreter
#     the build system already picks for every other `.ps1` ctest entry. A second
#     answer spelled here is a second answer that drifts from the first.
#   ★★ THE ESCAPE IS DIRECTIONAL, AND THE FIXTURE RE-MEASURES THAT ON EVERY RUN.
#     Arm `0-probe-negative` calls the SAME probe with a PATH that reaches no
#     interpreter at all and requires it to report ABSENT. Without that arm, a
#     probe which had quietly degenerated to "always absent" would skip every
#     .ps1 arm on every host and still print a green run. ⚠ This repository has
#     already paid for the opposite direction: a guard grew an escape that EVERY
#     subject triggered, so it refused nothing and three mutants came back green.
#   ★ NOTHING HERE IS SETTABLE BY A CALLER. The candidate list is a constant and
#     there is no environment override, so the escape cannot be SPELLED -- it is
#     taken only when execution genuinely fails. Same ruling, and the same reason,
#     as both run-gate twins' own "no escape hatch, deliberately".
#   ★★ ON WINDOWS THE ESCAPE IS NOT AVAILABLE AT ALL (`run_gate_host_is_windows`,
#     the same predicate the subject uses). PowerShell ships with that OS and
#     `CMakeLists.txt` already refuses to configure without one, so a Windows host
#     answering "no PowerShell" has a broken PATH, not a legitimate absence --
#     and left escapable it would silently drop every .ps1 arm on the one host
#     category where twin parity is actually proved, while reporting green for
#     doing less work.
#   ★ THE .sh ARMS ARE NEVER ESCAPABLE. A host with no PowerShell still proves the
#     .sh twin: arms 1, 2, 3, 7, 8, 12, 13, 14, 18, 19, 31, 34 and 35 run
#     everywhere, unconditionally, and 23, 24 and 28 wherever a stand-in can run.
#
# ⚠⚠ THE REACH OF THIS GUARD -- WHERE TWIN PARITY IS ACTUALLY PROVED, AND WHERE IT
#   IS NOT. ✔MEASURED BY EXECUTION 2026-09-08 on all four hosts this project gates
#   on, by running each candidate and requiring the token back:
#       Windows (Git Bash)   pwsh 7 AND powershell 5.1 present  -> .ps1 arms RUN
#       WSL x86_64           /usr/bin/pwsh 7.5.4                -> .ps1 arms RUN
#       macOS arm64          /usr/local/bin/pwsh                -> .ps1 arms RUN
#       arm64 VPS (ubuntu)   NEITHER spelling present           -> .ps1 arms N/A
#   ⇒ on the arm64 VPS leg the .ps1 arms (4, 5, 9, 10, 15, 16, 20, 21, 25, 26, 29,
#     32, 36 and 37) -- and with them EVERY parity arm, 6, 11, 17, 22, 27, 30, 33
#     and 38 --
#     are not proved, and that is PERMANENT rather than pending:
#     nothing in this tree installs PowerShell there. A green `run_gate_guard` on
#     that carriage is evidence about `run-gate.sh` ALONE. The run says so in
#     words AND in a count of not-applicable arms, so the two cannot be confused.
#   ⚠ THE PRIOR ASSUMPTION WAS WRONG IN THE DANGEROUS DIRECTION, which is why the
#     table above is measured rather than reasoned: the defect this replaced
#     invoked `powershell` LITERALLY -- a spelling that exists on ONE of the four
#     hosts -- so every .ps1 arm returned 127 on the other three and arm 6 reported
#     `.sh=3 vs .ps1=127`. Two of those three hosts can in fact run the twin, and
#     `run-gate.ps1` was already cross-platform (`Test-RunGateIsWindows`, and the
#     `ps -eo` branch of `Get-RunGateProcessTable`); only the fixture was not.
set -u

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
SELF_SCRIPT="${SELF_DIR}/$(basename "${BASH_SOURCE[0]}")"
ROOT="$(cd "${SELF_DIR}/../.." && pwd -P)"
GATE_SH="${ROOT}/scripts/run-gate/run-gate.sh"
GATE_PS1="${ROOT}/scripts/run-gate/run-gate.ps1"

# ═══ ONE SCRATCH DIRECTORY PER INVOCATION, NOT PER SOURCE TREE ═════════════════
# [[D-TEST-RUN-GATE-GUARD-SCRATCH-IS-KEYED-BY-SOURCE-TREE-SO-CONCURRENT-GATES-DESTROY-EACH-OTHERS-FIXTURE]]
# ⚠⚠ THIS WAS `${ROOT}/.temp/test-run-gate-scratch`, ONE PATH PER CHECKOUT, and every
#   run began with `rm -rf` of it. The end-of-round gate runs TWO build trees of ONE
#   checkout (MinGW Debug and MSVC Release), each registering this very fixture.
#   ✔MEASURED 2026-09-15 (lane `ca`): the MSVC gate's guard failed 7 arms, 0 of them
#   fixture preconditions, first FAIL 7 s after the MinGW gate's guard re-created the
#   sandbox under it. ✔REPRODUCED the same day (lane `pg`) through ctest, `build/pg`
#   and `build/pg2` of one worktree 24 s apart: the EARLIER guard failed 9 arms, first
#   FAIL ~1 s after the later one's wipe, and the later guard passed.
# ⇒ `run-<pid>[w<winpid>]-XXXXXX` under that base, made by `mktemp -d`, so no two
#   live invocations can ever share one, however they were launched. The OWNER'S
#   IDENTITY IS IN THE NAME, so the sweep below can tell a live sibling from a dead
#   run without a window in which a just-created directory has no owner record.
# ★ STILL CLEANED, AND STILL INSIDE `.temp/`: each run removes every instance whose
#   owner is gone -- stopping that instance's leftover stand-ins by their recorded
#   identity first -- so the post-mortem of the latest run survives until the next
#   run starts, exactly as the single directory did.
# ⓘ A `__stand-in` callback is a SEPARATE process running this file, so it is told
#   its instance through `RG_FIXTURE_SCRATCH`. Only that callback mode reads it; the
#   fixture proper always makes its own, so the variable is not a knob a caller can
#   turn to share a directory.
SCRATCH_BASE="${ROOT}/.temp/test-run-gate-scratch"
SCRATCH=""
if [ "${1:-}" = "__stand-in" ]; then
    SCRATCH="${RG_FIXTURE_SCRATCH:?test-run-gate.sh: a __stand-in callback needs RG_FIXTURE_SCRATCH, which only the fixture itself sets}"
fi
STAND_IN_STATE="${SCRATCH}/stand-in"
RG_TAB=$'\t'
RG_NL=$'\n'

# ★ THE ONLY TWO NUMBERS IN THE STAND-IN LIFECYCLE, AND NEITHER IS SYNCHRONIZATION.
# · RG_HANG_GUARD_S bounds a wait for an event this fixture itself causes (a plant
#   appearing, a stop taking effect) -- ✔MEASURED at 103 ms and 26 ms. Exceeding it
#   is a FAILED PRECONDITION, by name; nothing waits for it to elapse.
# · RG_STAND_IN_LEAK_BOUND_S ends a stand-in this fixture could not stop (it was
#   SIGKILLed, say), so a leaked `dsscp` cannot sit in every later gate's
#   `compilers:` line. A stand-in that reaches it records why, and any arm still
#   relying on it fails its precondition rather than passing.
RG_HANG_GUARD_S=120
RG_STAND_IN_LEAK_BOUND_S=600

# ⚠ THE SAME PREDICATE AS `run_gate_is_windows` IN THE SUBJECT, deliberately
#   spelled the same way: a second answer to "is this Windows" is how two halves
#   of one pair start disagreeing. It cannot be reused by sourcing, because
#   run-gate.sh EXECUTES.
run_gate_host_is_windows() {
    case "$(uname -s 2>/dev/null || echo unknown)" in
        MINGW*|MSYS*|CYGWIN*) return 0 ;;
        *)                    return 1 ;;
    esac
}

# ═══ THE STAND-IN LIFECYCLE ════════════════════════════════════════════════════
# A stand-in is a copy of this host's bash NAMED like the compiler, running this
# very file as `__stand-in body <label>`. It records its own identity in
# `$STAND_IN_STATE/<label>/pid` and then lives until it is stopped.
# ⓘ WHY bash AND NOT `sleep`: a `sleep` can only be given a lifetime, and a
#   lifetime is the defect. A shell can wait for a signal instead.
# ⚠ Running the REAL `dsscp` here would be actively wrong: it writes to the
#   per-user cache this whole subject is about, so the fixture would perturb the
#   machine it is measuring.

# The recorded identity: RG_SI_PID in this host's own pid namespace (what `kill`
# takes), and RG_SI_WINPID on Windows (the pid CIM, and so run-gate, reports).
stand_in_load() {  # <label> -> 0 when an identity was recorded
    RG_SI_PID=""
    RG_SI_WINPID=""
    [ -f "$STAND_IN_STATE/$1/pid" ] || return 1
    read -r RG_SI_PID RG_SI_WINPID < "$STAND_IN_STATE/$1/pid"
    [ -n "$RG_SI_PID" ]
}
stand_in_describe() {  # <label> -> RG_SI_DESC
    if stand_in_load "$1"; then
        RG_SI_DESC="pid $RG_SI_PID${RG_SI_WINPID:+, WINPID $RG_SI_WINPID}"
    else
        RG_SI_DESC="no pid recorded"
    fi
}

# ALIVE AND PROVABLY THAT STAND-IN -- the pair (MSYS pid, WINPID) on Windows, the
# label in the command line on POSIX -- so a recycled pid can never read as present.
# ⓘ `ps -W` can prefix a row with a one-letter state column; `o` absorbs it.
stand_in_is_ours() {  # <label>
    stand_in_load "$1" || return 1
    if run_gate_host_is_windows; then
        ps -W 2>/dev/null | awk -v p="$RG_SI_PID" -v w="$RG_SI_WINPID" '
            { o = ($1 ~ /^[0-9]+$/) ? 0 : 1 }
            $(1 + o) == p && $(4 + o) == w { f = 1 }
            END { exit !f }'
    else
        case "$(ps -ww -o args= -p "$RG_SI_PID" 2>/dev/null)" in
            (*"__stand-in body $1") return 0 ;;
            (*)                     return 1 ;;
        esac
    fi
}

# OBSERVED GONE, STRICTLY. On Windows no row may carry the recorded WINPID beside
# the stand-in's image directory at all, whatever MSYS pid sits next to it: that
# WINPID is exactly what the subject's CIM query would report.
stand_in_gone() {  # <label>
    stand_in_load "$1" || return 1
    if run_gate_host_is_windows; then
        ! ps -W 2>/dev/null | awk -v w="$RG_SI_WINPID" '
            { o = ($1 ~ /^[0-9]+$/) ? 0 : 1 }
            $(4 + o) == w && /stand-in-bin/ { f = 1 }
            END { exit !f }'
    else
        ! kill -0 "$RG_SI_PID" 2>/dev/null
    fi
}

stand_in_wait() {  # <present|gone> <label> -> 0 when observed, 1 when the hang guard ran out
    local start=$SECONDS
    while :; do
        if [ "$1" = present ]; then
            stand_in_is_ours "$2" && return 0
        else
            stand_in_gone "$2" && return 0
        fi
        [ $((SECONDS - start)) -lt "$RG_HANG_GUARD_S" ] || return 1
        sleep 0.2   # the poll cadence of a hang-guarded wait: no outcome depends on its length
    done
}

# BY ITS RECORDED PID, AND ONLY WHEN IT IS PROVABLY OURS. Never by image name:
# `pkill dsscp` or `taskkill /IM dsscp.exe` would kill real compilers.
stand_in_stop() {  # <label> -> 0 when stopped AND observed gone
    stand_in_load "$1" || return 1
    if stand_in_is_ours "$1"; then kill -TERM "$RG_SI_PID" 2>/dev/null; fi
    stand_in_wait gone "$1"
}

# WHAT A STAND-IN RUNS. `sleep 1 & wait $!` rather than a foreground `sleep`,
# because bash runs a trap the moment `wait` is interrupted but only after a
# foreground child exits -- ✔MEASURED: stopped and gone 26 ms after the signal.
stand_in_body() {  # <label>
    local dir="$STAND_IN_STATE/$1" wp=""
    [ -d "$dir" ] || exit 71
    if run_gate_host_is_windows; then read -r wp < "/proc/$$/winpid"; fi
    { printf '%s %s\n' "$$" "$wp" > "$dir/pid.tmp" && mv "$dir/pid.tmp" "$dir/pid"; } || exit 72
    trap 'echo stopped > "$dir/exit-reason"; exit 0' TERM
    while [ -d "$dir" ]; do
        if [ "$SECONDS" -ge "$RG_STAND_IN_LEAK_BOUND_S" ]; then
            echo leak-bound > "$dir/exit-reason"
            exit 0
        fi
        sleep 1 </dev/null >/dev/null 2>&1 &   # the leak check's cadence, not synchronization
        wait $!
    done
    exit 0
}

# THE WITNESS A GATED STOP PRINTS. ⚠ It must not appear in the gate command's own
# argv: both twins append that argv to the log in their footer BEFORE they grep
# for the witness, so a witness spelled in the command line matches the footer.
RG_STOP_WITNESS='stand-in stopped and observed gone'

stand_in_main() {  # <verb> <label>
    case "${1:-}" in
        (body)
            stand_in_body "$2" ;;
        (await)
            if stand_in_wait present "$2"; then echo present > "$STAND_IN_STATE/$2/await-result"; exit 0; fi
            echo hang-guard > "$STAND_IN_STATE/$2/await-result"
            exit 70 ;;
        (stop)
            if stand_in_stop "$2"; then
                echo stopped > "$STAND_IN_STATE/$2/stop-result"
                echo "$RG_STOP_WITNESS"
                exit 0
            fi
            echo hang-guard > "$STAND_IN_STATE/$2/stop-result"
            exit 70 ;;
        (logholder)
            # A GATED COMMAND THAT HOLDS ITS GATE LIVE: it prints a line into the gate's
            # log, runs a stand-in body (image $3) until this fixture stops that body by
            # its recorded pid, and only then prints its witness. The gate is therefore
            # alive for exactly as long as the fixture says, at any host speed.
            echo "LOG-HOLDER-STARTED $2"
            "$3" "$SELF_SCRIPT" __stand-in body "$2"
            printf '%s-%s-%s\n' LOG HOLDER DONE
            exit 0 ;;
        (*)
            echo "test-run-gate.sh: unknown __stand-in verb '${1:-}'" >&2
            exit 64 ;;
    esac
}

if [ "${1:-}" = "__stand-in" ]; then
    shift
    stand_in_main "$@"
    exit $?
fi

# ═══ THE FIXTURE ═══════════════════════════════════════════════════════════════

# ── THE SWEEP: AN INSTANCE WHOSE OWNER IS GONE IS STOPPED, THEN REMOVED ───────
# See ONE SCRATCH DIRECTORY PER INVOCATION at the top of this file. An instance is
# `run-<pid>[w<winpid>]-XXXXXX`, and its owner is LIVE only while the identity the
# stand-ins already use still holds: on Windows the (MSYS pid, WINPID) pair, so a
# recycled pid cannot keep a dead run's directory alive; elsewhere that pid running
# THIS file. A live sibling is never touched, whatever launched it.
fixture_instance_alive() {  # <instance dir>
    local name id pid wp=""
    name="$(basename "$1")"
    id="${name#run-}"; id="${id%-*}"
    pid="${id%%w*}"
    case "$id" in *w*) wp="${id#*w}" ;; esac
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    if run_gate_host_is_windows; then
        case "$wp" in ''|*[!0-9]*) return 1 ;; esac
        ps -W 2>/dev/null | awk -v p="$pid" -v w="$wp" '
            { o = ($1 ~ /^[0-9]+$/) ? 0 : 1 }
            $(1 + o) == p && $(4 + o) == w { f = 1 }
            END { exit !f }'
    else
        case "$(ps -ww -o args= -p "$pid" 2>/dev/null)" in
            (*test-run-gate.sh*) return 0 ;;
            (*)                  return 1 ;;
        esac
    fi
}
# A dead instance's stand-ins are stopped by THEIR recorded identity, and only when that
# identity still holds, before any of its state is deleted.
stop_leftover_stand_ins() {  # <a stand-in state directory>
    local saved="$STAND_IN_STATE" left
    [ -d "$1" ] || return 0
    STAND_IN_STATE="$1"
    for left in "$1"/*/; do
        [ -d "$left" ] || continue
        left="$(basename "$left")"
        stand_in_is_ours "$left" || continue
        kill -TERM "$RG_SI_PID" 2>/dev/null
        stand_in_wait gone "$left" \
            || echo "  [warn] a stand-in left by an earlier run ($1/$left, pid $RG_SI_PID) did not stop"
    done
    STAND_IN_STATE="$saved"
}
mkdir -p "$SCRATCH_BASE" || { echo "test-run-gate.sh: cannot create $SCRATCH_BASE"; exit 1; }
# ⓘ THE PRE-INSTANCE LAYOUT kept its stand-ins directly under `stand-in/`; they are
#   stopped FIRST, because on Windows an executable still running cannot be deleted.
stop_leftover_stand_ins "$SCRATCH_BASE/stand-in"
for _rg_inst in "$SCRATCH_BASE"/*; do
    [ -e "$_rg_inst" ] || continue
    case "$(basename "$_rg_inst")" in
        (run-*)
            [ -d "$_rg_inst" ] || continue
            fixture_instance_alive "$_rg_inst" && continue
            stop_leftover_stand_ins "$_rg_inst/stand-in"
            rm -rf "$_rg_inst"
            ;;
        (*)
            rm -rf "$_rg_inst"   # the pre-instance layout's entries
            ;;
    esac
done
_rg_self_winpid=""
if run_gate_host_is_windows; then read -r _rg_self_winpid < "/proc/$$/winpid"; fi
SCRATCH="$(mktemp -d "$SCRATCH_BASE/run-$$${_rg_self_winpid:+w$_rg_self_winpid}-XXXXXX")" \
    || { echo "test-run-gate.sh: cannot create a scratch instance under $SCRATCH_BASE"; exit 1; }
export RG_FIXTURE_SCRATCH="$SCRATCH"
STAND_IN_STATE="${SCRATCH}/stand-in"
SANDBOX="${SCRATCH}/tree"
mkdir -p "$SANDBOX/examples" "$SANDBOX/src/dss-config" "$SANDBOX/tests/corpus" "$STAND_IN_STATE"
PROBE_REL="examples/.test-run-gate-input-probe.txt"
NOWHERE_BIN="${SCRATCH}/no-interpreter-here"
mkdir -p "$NOWHERE_BIN"
fails=0
preconditions_failed=0
arms_ran=0
arms_na=0
STAND_IN_PLANTED=""

# EVERY stand-in this run planted is stopped on the way out, whatever the way out.
cleanup_stand_ins() {
    for _rg_l in $STAND_IN_PLANTED; do
        if stand_in_is_ours "$_rg_l"; then kill -TERM "$RG_SI_PID" 2>/dev/null; fi
    done
}
trap cleanup_stand_ins EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# ── A CLOCK FOR THE LOG, NOT FOR ANY DECISION ──────────────────────────────────
# Microseconds from bash's EPOCHREALTIME where it exists (bash 5), else whole
# seconds from `date`. Its locale may spell the decimal point as a comma.
rg_now() {
    local t="${EPOCHREALTIME:-}"
    if [ -n "$t" ]; then
        t="${t/,/.}"
        RG_NOW_US="${t%%.*}${t#*.}"
    else
        RG_NOW_US="$(date +%s)000000"
    fi
}
rg_span() {  # <from-us> <to-us> -> RG_SPAN
    local d=$(( ($2 - $1) / 10000 ))
    if [ "$d" -lt 0 ]; then RG_SPAN="?(the clock stepped back)"; return 0; fi
    local cs=$(( d % 100 ))
    [ "$cs" -lt 10 ] && cs="0$cs"
    RG_SPAN="$(( d / 100 )).$cs"
}
rg_at() { rg_now; rg_span "$FIXTURE_T0_US" "$RG_NOW_US"; RG_AT="at +${RG_SPAN}s"; }
rg_now; FIXTURE_T0_US="$RG_NOW_US"

# A FAILURE OF THIS FIXTURE, NAMED AS ONE. It FAILS the run -- never a skip -- and
# its sentence says, every time, that it is not a verdict on the subject.
precondition_failed() {  # <label> <what>
    preconditions_failed=$((preconditions_failed + 1))
    fails=$((fails + 1))
    rg_at
    echo "  [FAIL] $1   FIXTURE PRECONDITION NOT MET, which is NOT a verdict on run-gate: $2   $RG_AT"
}

# ── WHICH POWERSHELL, IF ANY, CAN RUN THE .ps1 TWIN ON THIS HOST ────────────
# BY EXECUTION, never by lookup -- see the block above for why `command -v` is
# not trusted here. Echoes the winning candidate and returns 0; echoes nothing
# and returns 1 when no candidate actually ran.
# ⓘ USES ONLY SHELL BUILTINS on purpose: `case` and `[` rather than `grep`/`tr`,
#   so that arm `0-probe-negative` can point PATH at an empty directory and
#   disable the CANDIDATES without also disabling the probe -- which would make
#   the negative arm pass for the wrong reason.
PS_PROBE_TOKEN='DSS_RUN_GATE_PS_ALIVE'
PS_CANDIDATES='pwsh powershell'
run_gate_powershell() {
    local cand out rc
    for cand in $PS_CANDIDATES; do
        out=$("$cand" -NoProfile -Command "Write-Output $PS_PROBE_TOKEN" 2>/dev/null)
        rc=$?
        [ "$rc" -eq 0 ] || continue
        case "$out" in
            *"$PS_PROBE_TOKEN"*) echo "$cand"; return 0 ;;
        esac
    done
    return 1
}

ran() { arms_ran=$((arms_ran + 1)); }

# An arm this host CANNOT run. Named, with its reason, and counted -- the three
# things a silent skip omits.
na() {  # <label> <reason>
    arms_na=$((arms_na + 1))
    echo "  [n/a ] $1   NOT APPLICABLE ON THIS HOST: $2"
}

# ⚠ THE INTERPRETER IS `$BASH` -- THE ONE ALREADY RUNNING THIS FIXTURE -- NOT A
#   HARD-CODED `/usr/bin/bash`. ✔MEASURED 2026-09-08 on the macOS carriage:
#   `/usr/bin/bash` DOES NOT EXIST there (only `/bin/bash`), so a synthetic
#   `add_test` named a program that could not be launched, every arm from
#   `7-sh-alone` on lost its success witness, and the guard would have red on
#   that leg naming the SUBJECT for a defect in the fixture's own test tree.
#   ⓘ `$BASH` needs no probe and cannot lie: it is the absolute path of the shell
#     executing this line. `cygpath -m` converts a path for a NATIVE consumer
#     (CMake, ctest, PowerShell) only on MSYS, where a `/c/...` path is not
#     something they can open.
native_path() {  # <path> -> RG_NATIVE
    RG_NATIVE="$1"
    if command -v cygpath >/dev/null 2>&1; then RG_NATIVE="$(cygpath -m "$1")"; fi
}
native_path "$BASH";        BASH_W="$RG_NATIVE"
native_path "$SELF_SCRIPT"; SELF_SCRIPT_W="$RG_NATIVE"
native_path "$GATE_SH";     GATE_SH_W="$RG_NATIVE"
native_path "$GATE_PS1";    GATE_PS1_W="$RG_NATIVE"
native_path "$SCRATCH";     SCRATCH_W="$RG_NATIVE"

# ── THE STAND-IN'S IMAGE: this bash, copied under the compiler's name ───────────
# ⓘ `.exe` on MSYS because that is the name Win32 reports and the matcher strips;
#   a bare name elsewhere. `$STAND_IN` is empty when no runnable copy can be made,
#   and the compiler arms then report NOT APPLICABLE rather than skipping.
STAND_IN=""
STAND_IN_W=""
mkdir -p "$SCRATCH/stand-in-bin"
if run_gate_host_is_windows; then _rg_si="$SCRATCH/stand-in-bin/dsscp.exe"; else _rg_si="$SCRATCH/stand-in-bin/dsscp"; fi
if cp "$BASH" "$_rg_si" 2>/dev/null; then
    chmod +x "$_rg_si" 2>/dev/null || true
    # ★★★ macOS SIGKILLS A COPY OF A PLATFORM BINARY, AND THAT KILLED THIS WHOLE
    # SUBJECT ON ONE HOST WHILE READING AS A DEFECT IN THE DETECTOR.
    # [D-TEST-RUN-GATE-STUB-IS-A-COPY-OF-A-PLATFORM-BINARY-AND-MACOS-KILLS-IT]
    # ✔MEASURED 2026-09-14 on the macOS carriage, three constructions of the
    # then-`sleep` stand-in launched the way the fixture launched one:
    #   · plain `cp /bin/sleep …/dsscp`            -> alive_after_2s = 0  (killed)
    #   · the same copy + `codesign --force -s -`  -> alive_after_2s = 1
    #   · `ln -s /bin/sleep …/dsscp`               -> alive_after_2s = 1
    # A platform binary's code identity lives in the system trust cache, so a copy
    # off the system volume satisfies nothing and AMFI kills it. ⇒ ad-hoc sign the
    # COPY, so the stand-in keeps its own path as argv[0]; a symlink would exec the
    # real binary and is the weaker fixture for a subject about image names. The
    # same holds for `/bin/bash`, and a Homebrew bash merely gets re-signed.
    if [ "$(uname -s 2>/dev/null)" = "Darwin" ] && command -v codesign >/dev/null 2>&1; then
        codesign --force --sign - "$_rg_si" >/dev/null 2>&1 || true
    fi
    # ★★ AND IT IS PROVED TO RUN, not assumed to. A stand-in that cannot start
    # makes every arm below assert against an empty machine.
    if "$_rg_si" -c 'exit 0' >/dev/null 2>&1; then
        STAND_IN="$_rg_si"
        native_path "$STAND_IN"; STAND_IN_W="$RG_NATIVE"
    else
        _rg_stand_in_unrunnable=1
    fi
fi
STAND_IN_ABSENT_WHY="no copy of this shell could be made under the compiler's name, so no live-compiler subject could be created on this host"
if [ "${_rg_stand_in_unrunnable:-0}" = "1" ]; then
    STAND_IN_ABSENT_WHY="a copy of this shell named after the compiler was made but WOULD NOT RUN on this host (on macOS a copy of a platform binary is SIGKILLed by AMFI unless ad-hoc signed, and \`codesign\` was absent or refused), so no live-compiler subject could be created"
fi

# THE SPELLING run-gate ITSELF PRINTS for a directory. ⚠ NOT a second
# canonicaliser: it is the SAME two steps the subject takes -- `cd && pwd -P`,
# then MSYS's own `cygpath -m` on Windows -- so the assertions below compare like
# with like. A fixture that spells a path its own way reports a defect that
# belongs to its own reader, and this file already carries two of those.
gate_spelling() {  # <dir>
    local abs
    abs=$(cd "$1" && pwd -P) || return 1
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$abs"; else printf '%s\n' "$abs"; fi
}

# A synthetic ctest tree: a `CTestTestfile.cmake` alone is enough for ctest to
# run -- ✔MEASURED, no configure and no project needed.
mk_ctest_dir() {  # <dir>
    mkdir -p "$1"
    printf 'add_test(fast "%s" "-c" "echo ok")\n' "$BASH_W" > "$1/CTestTestfile.cmake"
}

# A synthetic SOURCE tree: its own three read-at-test-time roots, nothing else.
mk_source_tree() {  # <dir>
    mkdir -p "$1/examples" "$1/src/dss-config" "$1/tests/corpus"
}

# A build directory that RECORDS WHICH TREE CONFIGURED IT, which is the only
# evidence run-gate has about where a gate command's tests read their config
# from. ⚠ The recorded path is written in the spelling BOTH twins can open: CMake
# writes a NATIVE path, and an MSYS `/c/...` home directory is INVISIBLE to
# PowerShell (`Test-Path` answers False). ✔MEASURED while these arms were built --
# a `/c/...` cache sent the .ps1 twin back to the cwd, so the arm went green on
# one twin and red on the other for a reason that was in the FIXTURE.
mk_cmake_build_dir() {  # <build dir> <source tree> <the sh -c command for its one test>
    mkdir -p "$1"
    printf 'CMAKE_HOME_DIRECTORY:INTERNAL=%s\n' "$(gate_spelling "$2")" > "$1/CMakeCache.txt"
    printf 'add_test(probe "%s" "-c" "%s")\n' "$BASH_W" "$3" > "$1/CTestTestfile.cmake"
}

# ⚠ TREE_A IS A SIBLING OF THE SANDBOX AND SHARES NO PATH PREFIX WITH IT. Put it
#   under one of the sandbox's own roots and an edit in A would ALSO be an edit
#   in B, both candidate answers would agree, and the arms below could not fail
#   -- the vacuous-arm shape this repository has already paid for twice.
TREE_A="${SCRATCH}/gate-tree-a"
mk_source_tree "$TREE_A"
TREE_A_SPELT="$(gate_spelling "$TREE_A")"
SANDBOX_SPELT="$(gate_spelling "$SANDBOX")"

# ⚠⚠ AND THE TWO SPELLINGS MUST BE NON-EMPTY AND DISTINCT, OR EVERY PATH
#   ASSERTION BELOW PASSES WITHOUT PROVING ANYTHING. `gate_spelling` returns 1
#   when it cannot enter the directory, and an EMPTY spelling degrades
#   `says … "srctree : $TREE_A_SPELT"` into `says … "srctree : "` -- a needle
#   present in every log this wrapper has ever written -- while `says_not` on an
#   empty needle refuses nothing at all. Two coinciding spellings would collapse
#   the two candidate answers into one and make the arms unfalsifiable the other
#   way. Both are the vacuous-arm shape this repository has already paid for, so
#   this refuses LOUDLY here rather than reporting a green run that measured
#   nothing.
if [ -z "$TREE_A_SPELT" ] || [ -z "$SANDBOX_SPELT" ] || [ "$TREE_A_SPELT" = "$SANDBOX_SPELT" ]; then
    echo "test-run-gate.sh: REFUSED -- the two synthetic trees did not resolve to two"
    echo "  distinct, non-empty absolute spellings, so every path assertion in the"
    echo "  third subject's arms would pass vacuously and this run would prove nothing."
    echo "    tree A  : '$TREE_A' -> '$TREE_A_SPELT'"
    echo "    sandbox : '$SANDBOX' -> '$SANDBOX_SPELT'"
    exit 1
fi

cd "$SANDBOX" || { echo "test-run-gate.sh: cannot enter the sandbox $SANDBOX"; exit 1; }

logtext() { tr -d '\000' < "$1" 2>/dev/null; }   # UTF-16LE or UTF-8, either way

arm() {  # <label> <want-rc> ...cmd
    local label=$1 want=$2; shift 2
    ran
    rg_now
    local t0="$RG_NOW_US"
    "$@" > "$SCRATCH/$label.out" 2>&1
    local rc=$?
    rg_now
    rg_span "$t0" "$RG_NOW_US"
    local took="$RG_SPAN"
    rg_span "$FIXTURE_T0_US" "$RG_NOW_US"
    echo "$rc" > "$SCRATCH/$label.rc"
    if [ "$rc" -eq "$want" ]; then
        echo "  [ok  ] $label   rc=$rc (want $want)   at +${RG_SPAN}s took ${took}s"
    else
        echo "  [FAIL] $label   rc=$rc (want $want)   at +${RG_SPAN}s took ${took}s"
        logtext "$SCRATCH/$label.out" | sed 's/^/         /' | head -12
        fails=$((fails + 1))
    fi
}

# ★ A FAILED ASSERTION SHOWS WHAT IT READ. ctest holds this fixture's output
#   until it ends, and a CI runner's scratch directory is gone by the time anyone
#   looks, so a needle that is missing must bring the end of its haystack with it
#   -- otherwise the red names the arm and hides the evidence.
show_tail() {  # <file>
    echo "         ... the last lines of $(basename "$1"):"
    logtext "$1" | tail -8 | sed 's/^/         | /'
}

says() {  # <label-or-file> <needle> [--log]
    local f="$SCRATCH/$1.out"
    [ "${3:-}" = "--log" ] && f="$SCRATCH/$1"
    if logtext "$f" | grep -qF "$2"; then
        echo "  [ok  ] $1   says: $2"
    else
        echo "  [FAIL] $1   did NOT say: $2"
        show_tail "$f"
        fails=$((fails + 1))
    fi
}

# ★ THE NEGATIVE HALF, AND IT IS NOT SYMMETRY FOR ITS OWN SAKE. "The footer names
#   the right tree" is only half a claim: a wrapper that watched BOTH candidate
#   trees would satisfy every `says` above and still be watching a tree the gate
#   command never reads. This is what makes that impossible to pass by accident.
says_not() {  # <label-or-file> <needle> [--log]
    local f="$SCRATCH/$1.out"
    [ "${3:-}" = "--log" ] && f="$SCRATCH/$1"
    if logtext "$f" | grep -qF "$2"; then
        echo "  [FAIL] $1   SAID what it must not: $2"
        show_tail "$f"
        fails=$((fails + 1))
    else
        echo "  [ok  ] $1   does not say: $2"
    fi
}

# The rc an arm recorded, or `not-run` when its own precondition kept it from running.
rc_of() {  # <label> -> RG_RC
    RG_RC=not-run
    [ -f "$SCRATCH/$1.rc" ] && read -r RG_RC < "$SCRATCH/$1.rc"
}

rg_now
echo "run-gate evidence-integrity proof (sandbox: $SANDBOX), started $(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)"

# ---- ARM 0 THE ESCAPE MUST BE DIRECTIONAL, AND IT IS MEASURED HERE ----------
# The SAME probe, run where PATH reaches an EMPTY directory and nothing else,
# must report ABSENT. Without this arm, a probe that had degenerated to "always
# absent" would declare every .ps1 arm not-applicable on every host and still
# print a green run -- the exact shape of an escape that refuses nothing.
# ⓘ A subshell, not a `VAR= func` prefix: bash keeps such an assignment in the
#   caller's environment for a FUNCTION, which would break every later arm.
ran
neg_out=$( PATH="$NOWHERE_BIN"; run_gate_powershell ); neg_rc=$?
rg_at
if [ "$neg_rc" -eq 0 ]; then
    echo "  [FAIL] 0-probe-negative   probe claimed PowerShell '$neg_out' with no interpreter on PATH   $RG_AT"
    fails=$((fails + 1))
else
    echo "  [ok  ] 0-probe-negative   no interpreter reachable -> probe reports ABSENT (the escape is directional)   $RG_AT"
fi

# ---- ARM 0b WHICH INTERPRETER THIS HOST ACTUALLY HAS ------------------------
# ★★ ON WINDOWS THE ESCAPE IS NOT AVAILABLE AT ALL, AND THAT IS THE OTHER HALF OF
#   MAKING IT DIRECTIONAL. Windows ships PowerShell 5.1 with the OS and
#   `CMakeLists.txt` already REFUSES TO CONFIGURE without one
#   (`find_program(POWERSHELL_EXE NAMES pwsh powershell REQUIRED)`), so a Windows
#   host that answers "no PowerShell" has a broken environment, not a legitimate
#   absence. Left escapable, a `ctest` launched with a stripped PATH would drop
#   every .ps1 arm on the ONE host category where every carriage proves twin
#   parity, and report a green run for doing less work. It fails loud instead.
ran
rg_at
if PS_EXE=$(run_gate_powershell); then
    echo "  [ok  ] 0-probe-positive   .ps1 arms will run under '$PS_EXE' (probed BY EXECUTION, not by lookup)   $RG_AT"
elif run_gate_host_is_windows; then
    PS_EXE=""
    echo "  [FAIL] 0-probe-positive   THIS IS WINDOWS and none of '$PS_CANDIDATES' ran: PowerShell ships with"
    echo "         the OS and CMake already requires one, so this is a broken PATH, not a host without"
    echo "         PowerShell. The .ps1 arms are NOT escapable here. Put pwsh or powershell on PATH."
    fails=$((fails + 1))
else
    PS_EXE=""
    echo "  [ok  ] 0-probe-positive   no working PowerShell here: none of '$PS_CANDIDATES' returned '$PS_PROBE_TOKEN' with rc 0   $RG_AT"
fi
PS_ABSENT_WHY="no working PowerShell on this host (each of '$PS_CANDIDATES' was RUN and did not return '$PS_PROBE_TOKEN' with rc 0)"

# ---- ARM 1 (sh) CONTROL: inputs held still -> the wrapper still passes -------
rm -f "$PROBE_REL"
arm 1-sh-control 0 bash "$GATE_SH" "$SCRATCH/a1.log" 'HELLO' \
    bash -c 'echo HELLO'
says 1-sh-control 'run-gate.sh: OK'
says a1.log 'inputs  : held still' --log
# ...and a command that names no build directory must SAY it had no subject,
# rather than reporting a contention verdict it never took.
says a1.log 'builddir: none named by this command' --log
# ...and it must NAME, absolutely, the tree whose stillness it just vouched for.
# A `held still` whose subject the reader has to reconstruct from the caller's
# working directory is a claim that cannot be checked at all.
says a1.log "srctree : $SANDBOX_SPELT" --log
says a1.log "watched : $SANDBOX_SPELT/examples" --log

# ---- ARM 0c THE HOST'S THIRD STATE, MEASURED RATHER THAN LOOKED UP ----------
#
# ★★★ WHICH `compilers:` ANSWER IS CORRECT IS A PROPERTY OF THE HOST.
# [[D-SCRIPT-RUN-GATE-COMPILERS-LINE-REPORTS-NONE-WHEN-IT-COULD-NOT-READ-THE-PROCESS-TABLE]]
# On a host that can enumerate processes, the compiler arms below demand the
# stand-in be NAMED. On one that cannot, NO correct implementation can name it
# and the only honest answer is `UNKNOWN`; arm 24 as first written demanded the
# name unconditionally and was therefore UNSATISFIABLE on such a host.
#
# ⚠ THE BRANCH IS NOT AN ESCAPE, AND THE TEST OF THAT IS WHETHER THE DEFECT
# COULD PASS EITHER SIDE. `compilers: none` with a live foreign compiler is
# refused on BOTH sides, so neither branch is satisfiable by the thing this
# subject is about. [[feedback-an-escape-every-row-triggers-disarms-the-guard]]
#
# ⓘ READ OUT OF `a1.log` — the control arm's own log, already written — rather
# than from a `command -v powershell` here. A second implementation of the
# subject's own lookup could disagree with the subject's, and a fixture that
# branches on its own opinion of the host instead of the host's answer is this
# very row's defect, re-introduced in the instrument that is supposed to catch it.
TABLE_STATE=""
if   logtext "$SCRATCH/a1.log" | grep -qF 'compilers: UNKNOWN'; then TABLE_STATE=blind
elif logtext "$SCRATCH/a1.log" | grep -qF 'compilers:';         then TABLE_STATE=readable
fi
ran
rg_at
if [ -n "$TABLE_STATE" ]; then
    echo "  [ok  ] 0-probe-table-state   this host's run-gate reports its process table as $TABLE_STATE (MEASURED from a1.log, not looked up)   $RG_AT"
else
    echo "  [FAIL] 0-probe-table-state   a1.log carries NO 'compilers:' line at all, so every compiler arm below has no host state to branch on   $RG_AT"
    fails=$((fails + 1))
fi

# The compiler arms' shared assertion set, so the two twins and the two
# subjects cannot drift into demanding different things of the same line.
# ⚠ `compilers: none` is refused FIRST and on every host — that is the
# invariant; everything after it is what the host makes checkable.
# ★ THE IDENTITY LINE NAMES THE STAND-IN'S OWN PID. ✔MEASURED: a bare "dsscp …
# seen when this run STARTED" was satisfied by the PREVIOUS arm's stand-in.
says_named_or_unknown() {  # <log-basename> [the exact identity line]
    says_not "$1" 'compilers: none' --log
    if [ "$TABLE_STATE" = readable ]; then
        says "$1" "OUTSIDE this gate's process tree" --log
        says "$1" 'dsscp' --log
        if [ -n "${2:-}" ]; then says "$1" "$2" --log; fi
    else
        says "$1" 'compilers: UNKNOWN' --log
    fi
}

# ★★★ THE `compilers:` LINE MAY NAME ONLY COMPILERS OUTSIDE THIS RUN, AND MUST COUNT
# WHAT IT NAMES. [[D-TEST-RUN-GATE-GUARD-ASSERTS-THAT-NO-COMPILER-RUNS-ANYWHERE-ON-THE-MACHINE]]
# The decidable form of "the twin does not report unconditionally" (arm 26). With any
# number of unrelated compilers alive, a correct line is `none`, or a count followed by
# exactly that many rows, each of whose command line names the compiler image and none
# of which is a stand-in this run planted. A line that fires regardless of the table
# names non-compilers, or states a count it does not print.
compilers_line_is_sound() {  # <log basename>
    local f="$SCRATCH/$1" head count verdict label pids=""
    head="$(logtext "$f" | grep -m1 '^compilers:')"
    case "$head" in
        ("compilers: none outside"*)
            echo "  [ok  ] $1   is sound: compilers: none outside (nothing to name)"
            return 0 ;;
        ("compilers: UNKNOWN"*)
            if [ "$TABLE_STATE" = blind ]; then
                echo "  [ok  ] $1   is sound: compilers: UNKNOWN on a host whose process table is unreadable"
                return 0
            fi
            echo "  [FAIL] $1   said compilers: UNKNOWN on a host whose process table reads"
            show_tail "$f"; fails=$((fails + 1)); return 1 ;;
        ("compilers: "[0-9]*) ;;
        (*)
            echo "  [FAIL] $1   carries no recognisable 'compilers:' line"
            show_tail "$f"; fails=$((fails + 1)); return 1 ;;
    esac
    count="${head#compilers: }"; count="${count%% *}"
    for label in $STAND_IN_PLANTED; do
        stand_in_subject_pid "$label"
        [ -n "${RG_SI_SUBJECT:-}" ] && pids="$pids $RG_SI_SUBJECT"
    done
    verdict="$(logtext "$f" | awk -v want="$count" -v planted=" $pids " '
        /^compilers: [0-9]/ { on = 1; next }
        on && /^          pid [0-9]+  / {
            rows++
            if (index(planted, " " $2 " ") > 0) own++
            # ⓘ An EMPTY command line is a compiler whose line could not be read (it had
            #   exited, or is protected) -- ✔MEASURED in a live footer beside four readable
            #   ones. It was matched by its IMAGE, so it is not evidence of a line that
            #   fires regardless; only a READABLE line that names no compiler is.
            if ((getline cl) > 0) { sub(/^ +/, "", cl); if (cl != "" && tolower(cl) !~ /dsscp/) bad++ }
            next
        }
        on { on = 0 }
        END {
            if (rows != want) { printf "states %s process(es) and prints %d row(s)", want, rows; exit }
            if (bad > 0)      { printf "names %d process(es) whose command line is not the compiler", bad; exit }
            if (own > 0)      { printf "names %d stand-in(s) this run planted and observed gone", own; exit }
            printf "OK %d", rows
        }')"
    case "$verdict" in
        (OK*) echo "  [ok  ] $1   is sound: ${verdict#OK } process(es) named, each a compiler outside this run" ;;
        (*)   echo "  [FAIL] $1   'compilers:' line is NOT sound: it $verdict"; show_tail "$f"; fails=$((fails + 1)) ;;
    esac
}

# ---- ARM 2 (sh) THE DEFECT: an input root is edited DURING the run ----------
# ⓘ NO SETTLE BEFORE THE EDIT. Both twins take the pre-run fingerprint before they
#   start the command and the post-run one after it returns, so the edit lands
#   between them by construction -- and landing in the same SECOND as the marker is
#   the stricter case, the one a timestamp scan could miss.
rm -f "$PROBE_REL"
arm 2-sh-moved 3 bash "$GATE_SH" "$SCRATCH/a2.log" 'HELLO' \
    bash -c "echo touched > $PROBE_REL; echo HELLO"
says 2-sh-moved 'the tree CHANGED UNDER THE RUN'
says 2-sh-moved 'test-run-gate-input-probe'
says 2-sh-moved 'This is NOT'
rm -f "$PROBE_REL"

# ---- ARM 3 (sh) ORDERING: rc=0 AND the witness present is NOT enough --------
# Arm 2's command exits 0 and prints the witness, so a wrapper that checked
# either one first would have PASSED it. This names that ordering.
ran
rg_at
if logtext "$SCRATCH/2-sh-moved.out" | grep -q 'run-gate.sh: OK'; then
    echo "  [FAIL] 3-sh-order   the moved-input run reported OK   $RG_AT"
    fails=$((fails + 1))
else
    echo "  [ok  ] 3-sh-order   a witness-matching rc=0 run was still refused   $RG_AT"
fi

if [ -n "$PS_EXE" ]; then
    # ---- ARM 4 (ps1) CONTROL ------------------------------------------------
    rm -f "$PROBE_REL"
    arm 4-ps1-control 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a4.log" 'HELLO' \
        "$PS_EXE" -NoProfile -Command "Write-Output HELLO"
    says 4-ps1-control 'run-gate.ps1: OK'
    says a4.log 'inputs  : held still' --log
    says a4.log 'builddir: none named by this command' --log
    says a4.log "srctree : $SANDBOX_SPELT" --log
    says a4.log "watched : $SANDBOX_SPELT/examples" --log

    # ---- ARM 5 (ps1) THE DEFECT ---------------------------------------------
    rm -f "$PROBE_REL"
    arm 5-ps1-moved 3 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a5.log" 'HELLO' \
        "$PS_EXE" -NoProfile -Command \
        "Set-Content -Path '$PROBE_REL' -Value touched; Write-Output HELLO"
    says 5-ps1-moved 'the tree CHANGED UNDER THE RUN'
    says 5-ps1-moved 'test-run-gate-input-probe'
    says 5-ps1-moved 'This is NOT'
    rm -f "$PROBE_REL"

    # ---- ARM 6 TWIN PARITY, ASSERTED FROM THE OBSERVED EXIT CODES -----------
    # ⚠ The first draft of this arm asserted parity from the EXISTENCE of the two
    #   output files, which is true whenever the arms ran at all and says nothing
    #   about what they decided. Compare the numbers the twins actually returned.
    ran
    sh_rc=$(cat "$SCRATCH/2-sh-moved.rc"); ps_rc=$(cat "$SCRATCH/5-ps1-moved.rc")
    rg_at
    if [ "$sh_rc" = "$ps_rc" ] && [ "$sh_rc" = "3" ]; then
        echo "  [ok  ] 6-parity   both twins refused the moved tree with exit $sh_rc   $RG_AT"
    else
        echo "  [FAIL] 6-parity   .sh returned $sh_rc, .ps1 returned $ps_rc (both must be 3)   $RG_AT"
        fails=$((fails + 1))
    fi
else
    na 4-ps1-control "$PS_ABSENT_WHY -- the .ps1 twin's still-tree CONTROL was not taken"
    na 5-ps1-moved   "$PS_ABSENT_WHY -- the .ps1 twin was never shown a moving tree"
    na 6-parity      "$PS_ABSENT_WHY -- arm 2 still proves the .sh twin refuses a moving tree, but the twins were NOT compared"
fi

# ═══ THE SECOND SUBJECT: A BUILD DIRECTORY SHARED WITH ANOTHER LIVE RUN ═════
# ✔MEASURED 2026-09-07 (P63): `ctest --test-dir build/sh` launched while a lane's
# gate was already running against that same directory gave 2100/2101 -- ONE red
# that PASSES in isolation -- while run-gate printed `inputs : held still`.
# ⚠ Every arm below uses a SYNTHETIC build directory under the sandbox. Pointing
#   one at the repository's real build tree would make this fixture refuse the
#   very gate that is running it.
# ★ THE CONTENDER IS HELD, NOT TIMED. It is a real ctest whose one test is a
#   stand-in body: it is alive from the moment it is observed present until this
#   fixture stops it by pid AFTER the gate returns, so the gate's pre-run sample
#   cannot miss it on any host. The `sleep 12` contender and the `sleep 3` settle
#   it replaces were a bet that the gate would start sampling inside 12 s.
mk_ctest_dir "$SANDBOX/bd-alone"
mkdir -p "$SANDBOX/bd-shared"

start_contender() {  # <label> <build dir> -> CONTENDER_BG
    rm -rf "${STAND_IN_STATE:?}/$1"
    mkdir -p "$STAND_IN_STATE/$1"
    STAND_IN_PLANTED="$STAND_IN_PLANTED $1"
    printf 'add_test(held "%s" "%s" "__stand-in" "body" "%s")\n' "$BASH_W" "$SELF_SCRIPT_W" "$1" > "$2/CTestTestfile.cmake"
    ctest --test-dir "$2" > "$SCRATCH/$1.contender.log" 2>&1 &
    CONTENDER_BG=$!
    stand_in_wait present "$1" && return 0
    precondition_failed "$1" "the contending ctest's held test never appeared within the ${RG_HANG_GUARD_S}s hang guard"
    return 1
}
release_contender() {  # <label>
    stand_in_stop "$1"
    local start=$SECONDS
    while kill -0 "$CONTENDER_BG" 2>/dev/null; do
        if [ $((SECONDS - start)) -ge "$RG_HANG_GUARD_S" ]; then
            precondition_failed "$1" "the contending ctest was still running ${RG_HANG_GUARD_S}s after its held test was stopped"
            return 1
        fi
        sleep 0.2   # the poll cadence of a hang-guarded wait: no outcome depends on its length
    done
    wait "$CONTENDER_BG" 2>/dev/null
    return 0
}
# The contender reading: its held test by pid, and the ctest that holds it.
contender_reading() {  # <label> -> RG_READING
    local t=gone c=exited
    stand_in_is_ours "$1" && t=present
    kill -0 "$CONTENDER_BG" 2>/dev/null && c=running
    RG_READING="held-test=$t,ctest=$c"
}
contender_arm() {  # <label> <gate twin: sh|ps1> <log> -> 1 when a precondition failed
    local label=$1 before after ok=0
    if ! start_contender "$label" "$SANDBOX/bd-shared"; then
        release_contender "$label"
        return 1
    fi
    contender_reading "$label"; before="$RG_READING"
    if [ "$2" = sh ]; then
        arm "$label" 4 bash "$GATE_SH" "$SCRATCH/$3" '100% tests passed' \
            ctest --test-dir "$SANDBOX/bd-shared"
    else
        arm "$label" 4 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
            -File "$GATE_PS1" "$SCRATCH/$3" '100% tests passed' \
            ctest --test-dir "$SANDBOX/bd-shared"
    fi
    contender_reading "$label"; after="$RG_READING"
    stand_in_describe "$label"; rg_at
    echo "  [info] $label   independent reading of the contender ($RG_SI_DESC): before the gate=$before, after the gate=$after   $RG_AT"
    case "$before" in
        (held-test=present,ctest=running) : ;;
        (*) precondition_failed "$label" "the contender was not live when the gate started ($before), so the gate was never shown a shared build directory"
            ok=1 ;;
    esac
    release_contender "$label" || ok=1
    return "$ok"
}

# ---- ARM 7 (sh) CONTROL: alone in the build directory -> still passes -------
arm 7-sh-alone 0 bash "$GATE_SH" "$SCRATCH/a7.log" '100% tests passed' \
    ctest --test-dir "$SANDBOX/bd-alone"
says 7-sh-alone 'run-gate.sh: OK'
says a7.log 'contended: no' --log

# ---- ARM 8 (sh) THE DEFECT: a second ctest is ALREADY live in it -----------
if contender_arm 8-sh-contended sh a8.log; then
    says 8-sh-contended 'ANOTHER RUN IS LIVE IN THIS BUILD DIRECTORY'
    says 8-sh-contended 'bd-shared'
    says 8-sh-contended 'This is NOT'
fi

if [ -n "$PS_EXE" ]; then
    # ---- ARM 9 (ps1) CONTROL ------------------------------------------------
    arm 9-ps1-alone 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a9.log" '100% tests passed' \
        ctest --test-dir "$SANDBOX/bd-alone"
    says 9-ps1-alone 'run-gate.ps1: OK'
    says a9.log 'contended: no' --log

    # ---- ARM 10 (ps1) THE DEFECT --------------------------------------------
    if contender_arm 10-ps1-contended ps1 a10.log; then
        says 10-ps1-contended 'ANOTHER RUN IS LIVE IN THIS BUILD DIRECTORY'
        says 10-ps1-contended 'bd-shared'
    fi

    # ---- ARM 11 TWIN PARITY ON THE SECOND SUBJECT ---------------------------
    ran
    rc_of 8-sh-contended; sh_rc="$RG_RC"
    rc_of 10-ps1-contended; ps_rc="$RG_RC"
    rg_at
    if [ "$sh_rc" = "$ps_rc" ] && [ "$sh_rc" = "4" ]; then
        echo "  [ok  ] 11-parity  both twins refused the shared build directory with exit $sh_rc   $RG_AT"
    else
        echo "  [FAIL] 11-parity  .sh returned $sh_rc, .ps1 returned $ps_rc (both must be 4)   $RG_AT"
        fails=$((fails + 1))
    fi
else
    na 9-ps1-alone      "$PS_ABSENT_WHY -- the .ps1 twin's uncontended CONTROL was not taken"
    na 10-ps1-contended "$PS_ABSENT_WHY -- the .ps1 twin never saw a shared build directory"
    na 11-parity        "$PS_ABSENT_WHY -- arm 8 still proves the .sh twin refuses a shared build directory, but the twins were NOT compared"
fi

# ---- ARM 12 THE TWO REFUSALS MUST STAY TELLABLE APART ----------------------
# ★ A reader who cannot tell 3 from 4 cannot tell whether to settle the tree or
#   wait for a sibling -- two different remedies. This asserts the codes differ
#   AND that neither refusal borrows the other's sentence.
ran
rc_of 2-sh-moved; moved_rc="$RG_RC"
rc_of 8-sh-contended; cont_rc="$RG_RC"
rg_at
if [ "$moved_rc" != "$cont_rc" ] \
   && ! logtext "$SCRATCH/8-sh-contended.out" | grep -q 'CHANGED UNDER THE RUN' \
   && ! logtext "$SCRATCH/2-sh-moved.out"     | grep -q 'ANOTHER RUN IS LIVE'; then
    echo "  [ok  ] 12-distinct  moved-tree ($moved_rc) and contended-build-dir ($cont_rc) are distinct refusals   $RG_AT"
else
    echo "  [FAIL] 12-distinct  the two refusals are not tellable apart (rc $moved_rc vs $cont_rc)   $RG_AT"
    fails=$((fails + 1))
fi

rm -f "$PROBE_REL"

# ═══ THE THIRD SUBJECT: WHICH TREE THE INPUT ROOTS ARE READ FROM ═══════════
# ✔MEASURED 2026-09-08 (P65), both twins, both directions -- the block at the top
# of this file carries the numbers. Every arm here runs with the cwd in the
# SANDBOX and the gate command naming a build directory inside TREE_A, so the two
# candidate answers are DIFFERENT DIRECTORIES and no arm can pass by accident.
# ★ THE PAIR IS THE POINT, not either half. An arm that only proves the refusal
#   fires is equally consistent with "this wrapper refuses whenever anything on
#   the disk moves"; an arm that only proves it stays quiet is consistent with
#   "this wrapper stopped watching". 13/15 are the refusal, 14/16 are the CONTROL
#   -- and 13/15 are the direction whose failure was SILENT, which is why they
#   are here at all rather than being left to the older arms 2/5.
# ⓘ No settle before either edit, for the reason arm 2 gives.
mk_cmake_build_dir "$TREE_A/build/own" "$TREE_A" \
    "echo touched > '$TREE_A/examples/.probe-own-tree.txt'; echo ok"
mk_cmake_build_dir "$TREE_A/build/foreign" "$TREE_A" \
    "echo touched > '$SANDBOX/examples/.probe-foreign-tree.txt'; echo ok"

# ---- ARM 13 (sh) THE DEFECT: a foreign cwd must not HIDE this run's own tree --
rm -f "$TREE_A/examples/.probe-own-tree.txt"
arm 13-sh-own-tree-moves 3 bash "$GATE_SH" "$SCRATCH/a13.log" '100% tests passed' \
    ctest --test-dir "$TREE_A/build/own"
says 13-sh-own-tree-moves 'the tree CHANGED UNDER THE RUN'
says 13-sh-own-tree-moves '.probe-own-tree.txt'
says a13.log "srctree : $TREE_A_SPELT" --log
rm -f "$TREE_A/examples/.probe-own-tree.txt"

# ---- ARM 14 (sh) CONTROL: a FOREIGN tree moving must NOT refuse this run ------
# ⚠ This is the direction the old behaviour failed LOUDLY -- exit 3 naming a file
#   the run could not have read, throwing away a quarter-hour gate. Arm 13 is the
#   direction it failed SILENTLY.
rm -f "$SANDBOX/examples/.probe-foreign-tree.txt"
arm 14-sh-foreign-tree-moves 0 bash "$GATE_SH" "$SCRATCH/a14.log" '100% tests passed' \
    ctest --test-dir "$TREE_A/build/foreign"
says 14-sh-foreign-tree-moves 'run-gate.sh: OK'
says a14.log 'inputs  : held still' --log
says a14.log "watched : $TREE_A_SPELT/examples" --log
says_not a14.log "watched : $SANDBOX_SPELT/examples" --log
rm -f "$SANDBOX/examples/.probe-foreign-tree.txt"

if [ -n "$PS_EXE" ]; then
    # ---- ARM 15 (ps1) THE DEFECT --------------------------------------------
    rm -f "$TREE_A/examples/.probe-own-tree.txt"
    arm 15-ps1-own-tree-moves 3 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a15.log" '100% tests passed' \
        ctest --test-dir "$TREE_A/build/own"
    says 15-ps1-own-tree-moves 'the tree CHANGED UNDER THE RUN'
    says 15-ps1-own-tree-moves '.probe-own-tree.txt'
    says a15.log "srctree : $TREE_A_SPELT" --log
    rm -f "$TREE_A/examples/.probe-own-tree.txt"

    # ---- ARM 16 (ps1) CONTROL -----------------------------------------------
    rm -f "$SANDBOX/examples/.probe-foreign-tree.txt"
    arm 16-ps1-foreign-tree-moves 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a16.log" '100% tests passed' \
        ctest --test-dir "$TREE_A/build/foreign"
    says 16-ps1-foreign-tree-moves 'run-gate.ps1: OK'
    says a16.log 'inputs  : held still' --log
    says a16.log "watched : $TREE_A_SPELT/examples" --log
    says_not a16.log "watched : $SANDBOX_SPELT/examples" --log
    rm -f "$SANDBOX/examples/.probe-foreign-tree.txt"

    # ---- ARM 17 TWIN PARITY ON THE THIRD SUBJECT, IN BOTH DIRECTIONS --------
    # ⚠ Asserted from the four exit codes the twins actually returned, never from
    #   the existence of their logs -- the correction arm 6 already carries.
    ran
    own_sh=$(cat "$SCRATCH/13-sh-own-tree-moves.rc")
    own_ps=$(cat "$SCRATCH/15-ps1-own-tree-moves.rc")
    fgn_sh=$(cat "$SCRATCH/14-sh-foreign-tree-moves.rc")
    fgn_ps=$(cat "$SCRATCH/16-ps1-foreign-tree-moves.rc")
    rg_at
    if [ "$own_sh" = "3" ] && [ "$own_ps" = "3" ] && [ "$fgn_sh" = "0" ] && [ "$fgn_ps" = "0" ]; then
        echo "  [ok  ] 17-parity  both twins read their roots from the gate command's tree (own moved 3/3, foreign moved 0/0)   $RG_AT"
    else
        echo "  [FAIL] 17-parity  own tree moved: .sh=$own_sh .ps1=$own_ps (both must be 3); foreign tree moved: .sh=$fgn_sh .ps1=$fgn_ps (both must be 0)   $RG_AT"
        fails=$((fails + 1))
    fi
else
    na 15-ps1-own-tree-moves     "$PS_ABSENT_WHY -- the .ps1 twin was never shown its OWN tree moving from a foreign cwd"
    na 16-ps1-foreign-tree-moves "$PS_ABSENT_WHY -- the .ps1 twin's foreign-tree CONTROL was not taken"
    na 17-parity                 "$PS_ABSENT_WHY -- arms 13 and 14 still prove the .sh twin reads the gate command's tree, but the twins were NOT compared"
fi

# ═══ THE FOURTH SUBJECT: A TIMESTAMP THE HOST'S CLOCK GOT WRONG ════════════
# ⚠⚠⚠ THE THREE SUBJECTS ABOVE CANNOT CATCH THIS CLASS RELIABLY, AND THAT IS WHY
#   THIS ONE EXISTS. ✔MEASURED 2026-09-09 (P66) on WSL x86_64: CLOCK_REALTIME
#   there steps FORWARD +24.69 s for ~200 ms out of every ~5 s (4.8% duty cycle)
#   and the excursion REACHES INODE MTIMES -- 12 of 60 marker/probe pairs had the
#   probe, created ONE SECOND AFTER the marker, carrying an mtime 23.70 s
#   EARLIER, and `find -newer` answered EMPTY. Both twins compared stamps for
#   ORDER, so both went blind, and arm `13-sh-own-tree-moves` came back
#   `rc=0 (want 3)` on a real gate leg -- while `15-ps1-own-tree-moves` passed in
#   the SAME run, purely because the other twin's marker missed the excursion.
# ★★★ SO THE OLDER ARMS DETECT THIS ONLY BY LUCK, AT ROUGHLY 5% PER MARKER, AND A
#   GUARD THAT REDS 5% OF THE TIME REPORTS GREEN OVER A BROKEN INSTRUMENT THE
#   OTHER 95%. These arms remove the luck: rather than waiting for the host clock
#   to misbehave, they reproduce its OBSERVABLE EFFECT with `touch -t`, which is
#   POSIX and behaves the same on BSD and GNU. Nothing here manipulates a clock,
#   nothing is host-conditional, and both directions are deterministic on EVERY
#   carriage -- including the ones whose clocks are perfectly well behaved.
# ★ THE PAIR IS THE POINT, again. Arms 18/20 are the SILENT direction (a real
#   change wearing an old timestamp must still be seen); arms 19/21 are the LOUD
#   one (a file that ALREADY carried a future timestamp before the run started
#   must NOT refuse a run that never touched it) -- the false refusal the same
#   clock produces, which the stamp-ordering scan had and no arm covered.
# ⚠ ITS OWN TREE, because arm 19/21's bystander carries a year-2090 stamp and a
#   leftover of that shape would silently arm every later arm in another tree.
TREE_C="${SCRATCH}/gate-tree-c"
mk_source_tree "$TREE_C"
BACKDATED="$TREE_C/examples/.probe-backdated.txt"
BYSTANDER="$TREE_C/examples/.probe-bystander.txt"
mk_cmake_build_dir "$TREE_C/build/backdated" "$TREE_C" \
    "echo touched > '$BACKDATED'; touch -t 200001010000 '$BACKDATED'; echo ok"
mk_cmake_build_dir "$TREE_C/build/bystander" "$TREE_C" "echo ok"

# A file created DURING the run, then stamped into the year 2000 -- exactly what
# a backward clock step does to a file the run really did write.
plant_bystander() { echo bystander > "$BYSTANDER"; touch -t 209001010000 "$BYSTANDER"; }

# ---- ARM 18 (sh) THE SILENT DIRECTION: an old stamp must not hide a change ---
rm -f "$BACKDATED"
arm 18-sh-backdated-change-refuses 3 bash "$GATE_SH" "$SCRATCH/a18.log" '100% tests passed' \
    ctest --test-dir "$TREE_C/build/backdated"
says 18-sh-backdated-change-refuses 'the tree CHANGED UNDER THE RUN'
says 18-sh-backdated-change-refuses '.probe-backdated.txt'
rm -f "$BACKDATED"

# ---- ARM 19 (sh) THE LOUD DIRECTION: a future stamp that PREDATES the run -----
plant_bystander
arm 19-sh-future-stamped-bystander 0 bash "$GATE_SH" "$SCRATCH/a19.log" '100% tests passed' \
    ctest --test-dir "$TREE_C/build/bystander"
says 19-sh-future-stamped-bystander 'run-gate.sh: OK'
says a19.log 'inputs  : held still' --log
says_not a19.log '.probe-bystander.txt' --log
rm -f "$BYSTANDER"

if [ -n "$PS_EXE" ]; then
    # ---- ARM 20 (ps1) THE SILENT DIRECTION ----------------------------------
    rm -f "$BACKDATED"
    arm 20-ps1-backdated-change-refuses 3 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a20.log" '100% tests passed' \
        ctest --test-dir "$TREE_C/build/backdated"
    says 20-ps1-backdated-change-refuses 'the tree CHANGED UNDER THE RUN'
    says 20-ps1-backdated-change-refuses '.probe-backdated.txt'
    rm -f "$BACKDATED"

    # ---- ARM 21 (ps1) THE LOUD DIRECTION ------------------------------------
    plant_bystander
    arm 21-ps1-future-stamped-bystander 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a21.log" '100% tests passed' \
        ctest --test-dir "$TREE_C/build/bystander"
    says 21-ps1-future-stamped-bystander 'run-gate.ps1: OK'
    says a21.log 'inputs  : held still' --log
    says_not a21.log '.probe-bystander.txt' --log
    rm -f "$BYSTANDER"

    # ---- ARM 22 TWIN PARITY ON THE FOURTH SUBJECT, IN BOTH DIRECTIONS -------
    ran
    bd_sh=$(cat "$SCRATCH/18-sh-backdated-change-refuses.rc")
    bd_ps=$(cat "$SCRATCH/20-ps1-backdated-change-refuses.rc")
    by_sh=$(cat "$SCRATCH/19-sh-future-stamped-bystander.rc")
    by_ps=$(cat "$SCRATCH/21-ps1-future-stamped-bystander.rc")
    rg_at
    if [ "$bd_sh" = "3" ] && [ "$bd_ps" = "3" ] && [ "$by_sh" = "0" ] && [ "$by_ps" = "0" ]; then
        echo "  [ok  ] 22-parity  neither twin decides by TIMESTAMP ORDER (backdated change 3/3, future-stamped bystander 0/0)   $RG_AT"
    else
        echo "  [FAIL] 22-parity  backdated change: .sh=$bd_sh .ps1=$bd_ps (both must be 3); future-stamped bystander: .sh=$by_sh .ps1=$by_ps (both must be 0)   $RG_AT"
        fails=$((fails + 1))
    fi
else
    na 20-ps1-backdated-change-refuses  "$PS_ABSENT_WHY -- the .ps1 twin was never shown a change wearing an old timestamp"
    na 21-ps1-future-stamped-bystander  "$PS_ABSENT_WHY -- the .ps1 twin's future-stamped-bystander CONTROL was not taken"
    na 22-parity                        "$PS_ABSENT_WHY -- arms 18 and 19 still prove the .sh twin ignores timestamp order, but the twins were NOT compared"
fi

# ═══ THE FIFTH SUBJECT: A COMPILER RUNNING OUTSIDE THIS GATE'S PROCESS TREE ══
#
# [[D-PROGRAM-RUNTIME-CACHE-PRUNE-DELETES-A-CONCURRENT-RUNS-LIVE-ARTIFACT]]
# ✔MEASURED 2026-09-10 (P66): two corpus examples went red inside an otherwise
# 2172/2174 run because a second `dsscp` deleted a cache entry the run had been
# handed the path to — and run-gate printed `contended: no`, a sentence that was
# TRUE of the build directory and silent about the machine. The resource shared
# is a PER-USER cache under `%LOCALAPPDATA%`, outside both `srctree` and
# `builddir`, so no subject this wrapper already had could see it.
#
# ★★★ THE ARMS ARE A SET AND NONE OF THEM MEANS ANYTHING ALONE.
#   · the DESCENDANT arm proves the check is not "any live dsscp", which would
#     fire on every gate this project runs (its own ctest spawns hundreds);
#   · the FOREIGN arm proves it is not permanently silent — and it is also the
#     descendant arm's control, because a matcher that recognised nothing would
#     satisfy the descendant arm perfectly;
#   · the EXITS-MID-RUN arm proves the line reports the UNION of both samples;
#   · the CONTROL arm (26) proves it says "none" when there is nothing to say.
# ★★ EVERY STAND-IN BELOW IS PLANTED, OBSERVED PRESENT, READ BEFORE AND AFTER THE
#   GATE, STOPPED BY PID AND OBSERVED GONE -- see NO ARM MAY DEPEND ON THE WALL
#   CLOCK at the top of this file. No stand-in outlives its own arm.

# The pid run-gate's footer names for a stand-in: the WINPID on Windows, where
# the table is CIM's, and the pid itself elsewhere.
stand_in_subject_pid() {  # <label> -> RG_SI_SUBJECT
    stand_in_load "$1"
    if run_gate_host_is_windows; then RG_SI_SUBJECT="$RG_SI_WINPID"; else RG_SI_SUBJECT="$RG_SI_PID"; fi
}
stand_in_reading() {  # <label> -> RG_READING: present | gone | never-started
    if ! stand_in_load "$1"; then RG_READING=never-started
    elif stand_in_is_ours "$1"; then RG_READING=present
    else RG_READING=gone
    fi
}

# ORPHANED from this shell on purpose: the inner shell exits at once, so the
# stand-in has no resolvable ancestry — the shape a compiler started from another
# terminal has, as seen from here. ✔MEASURED on MSYS: its Windows parent is dead
# before the gate ever samples, and every POSIX carriage reparents it to pid 1 or
# a session relay, none of which is part of the gate's tree.
plant_foreign_compiler() {  # <label>   -- NO lifetime: it lives until stop_foreign_compiler
    rm -rf "${STAND_IN_STATE:?}/$1"
    mkdir -p "$STAND_IN_STATE/$1"
    STAND_IN_PLANTED="$STAND_IN_PLANTED $1"
    "$BASH" -c '"$0" "$@" </dev/null >/dev/null 2>&1 &' "$STAND_IN" "$SELF_SCRIPT" __stand-in body "$1"
    stand_in_wait present "$1" && return 0
    precondition_failed "$1" "its stand-in never appeared as a live process with a recorded pid within the ${RG_HANG_GUARD_S}s hang guard"
    return 1
}
stop_foreign_compiler() {  # <label>
    stand_in_stop "$1" && return 0
    stand_in_describe "$1"
    precondition_failed "$1" "its stand-in ($RG_SI_DESC) was still alive ${RG_HANG_GUARD_S}s after it was stopped"
    return 1
}

# A stand-in must have ended because it was STOPPED, never on its own.
exit_reason_check() {  # <label>
    local r=""
    [ -f "$STAND_IN_STATE/$1/exit-reason" ] && read -r r < "$STAND_IN_STATE/$1/exit-reason"
    [ "$r" = stopped ] && return 0
    precondition_failed "$1" "its stand-in ended with reason '${r:-none recorded}', not 'stopped'"
    return 1
}

# THE READING BEFORE AND AFTER THE GATE, PRINTED EVERY TIME, ENFORCED WHERE THE
# ARM'S CLAIM DEPENDS ON IT.
check_readings() {  # <label> <before> <after> <want-before> <want-after>
    stand_in_describe "$1"
    rg_at
    echo "  [info] $1   independent reading of its stand-in ($RG_SI_DESC): before the gate=$2, after the gate=$3   $RG_AT"
    if [ "$2" = "$4" ] && [ "$3" = "$5" ]; then return 0; fi
    precondition_failed "$1" "this arm needs its stand-in $4 before the gate and $5 after it, and the independent reading was $2 then $3 -- the subject's two samples were not shown the process this arm is about"
    return 1
}

# A foreign-compiler arm whose stand-in is alive for BOTH of the gate's samples.
alive_throughout_arm() {  # <label> <twin: sh|ps1> <log basename>
    local label=$1 before after
    plant_foreign_compiler "$label" || return 1
    stand_in_reading "$label"; before="$RG_READING"
    if [ "$2" = sh ]; then
        arm "$label" 0 bash "$GATE_SH" "$SCRATCH/$3" 'HELLO' \
            bash -c 'echo HELLO'
    else
        arm "$label" 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
            -File "$GATE_PS1" "$SCRATCH/$3" 'HELLO' \
            "$PS_EXE" -NoProfile -Command "Write-Output HELLO"
    fi
    stand_in_reading "$label"; after="$RG_READING"
    stop_foreign_compiler "$label" || return 1
    check_readings "$label" "$before" "$after" present present || return 1
    exit_reason_check "$label"
}

# A foreign-compiler arm whose stand-in the GATED COMMAND stops: alive for the
# pre-run sample, gone for the post-run one, by the twins' own sequencing.
exits_midrun_arm() {  # <label> <twin: sh|ps1> <log basename>
    local label=$1 before after r=""
    plant_foreign_compiler "$label" || return 1
    stand_in_reading "$label"; before="$RG_READING"
    if [ "$2" = sh ]; then
        arm "$label" 0 bash "$GATE_SH" "$SCRATCH/$3" "$RG_STOP_WITNESS" \
            "$BASH" "$SELF_SCRIPT" __stand-in stop "$label"
    else
        arm "$label" 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
            -File "$GATE_PS1" "$SCRATCH/$3" "$RG_STOP_WITNESS" \
            "$BASH_W" "$SELF_SCRIPT_W" __stand-in stop "$label"
    fi
    stand_in_reading "$label"; after="$RG_READING"
    [ -f "$STAND_IN_STATE/$label/stop-result" ] && read -r r < "$STAND_IN_STATE/$label/stop-result"
    if [ "$r" != stopped ]; then
        precondition_failed "$label" "the gated command did not stop its stand-in (stop-result '${r:-none}'), so the post-run sample may still have seen it"
        stop_foreign_compiler "$label"
        return 1
    fi
    check_readings "$label" "$before" "$after" present gone || return 1
    exit_reason_check "$label"
}

if [ -n "$STAND_IN" ]; then
    # ---- ARM 23 (sh) A DESCENDANT COMPILER MUST NOT BE REPORTED -------------
    # The fixture is a REAL ctest running four tests: the stand-in `busy`, a
    # nested run-gate `gate`, and two stand-in verbs that ORDER them. `busy` and
    # `gate` are both children of that ctest, so the stand-in is inside the gate's
    # own process tree through a BUILD-TOOL ancestor — the one case the bounded
    # "own" set exists to cover, and the shape a guard registered as a ctest test
    # really has in this repository.
    # ★ THE ORDER IS ctest's DEPENDS, NOT A LIFETIME: `started` does not finish
    #   until `busy` is observed present, `gate` does not start until `started`
    #   finishes, and `stopper` does not stop `busy` until `gate` has finished --
    #   so the stand-in is alive across BOTH of the gate's samples on any host.
    #   ✔MEASURED that ctest honours DEPENDS as run-after under -j. The `busy 8`
    #   this replaces was a bet that the nested gate would be done inside 8 s, and
    #   a stand-in that exited first would have made this arm agree with a subject
    #   that reports nothing.
    # ⚠ IT CANNOT BE BUILT AS "the gate command backgrounds a child": the scans
    # run BEFORE and AFTER the command, so such a child is either not yet born
    # or already orphaned. ✔MEASURED — an orphaned child reads as FOREIGN, which
    # is the fail-toward-reporting direction and is what arm 24 relies on.
    L23=23-sh-descendant-compiler
    rm -rf "${STAND_IN_STATE:?}/$L23"
    mkdir -p "$STAND_IN_STATE/$L23" "$SANDBOX/bd-nested"
    STAND_IN_PLANTED="$STAND_IN_PLANTED $L23"
    {
        printf 'add_test(busy "%s" "%s" "__stand-in" "body" "%s")\n' "$STAND_IN_W" "$SELF_SCRIPT_W" "$L23"
        printf 'add_test(started "%s" "%s" "__stand-in" "await" "%s")\n' "$BASH_W" "$SELF_SCRIPT_W" "$L23"
        printf 'add_test(gate "%s" "%s" "%s" "HELLO" "%s" "-c" "echo HELLO")\n' \
            "$BASH_W" "$GATE_SH_W" "$SCRATCH_W/a23.log" "$BASH_W"
        printf 'set_tests_properties(gate PROPERTIES DEPENDS started)\n'
        printf 'add_test(stopper "%s" "%s" "__stand-in" "stop" "%s")\n' "$BASH_W" "$SELF_SCRIPT_W" "$L23"
        printf 'set_tests_properties(stopper PROPERTIES DEPENDS gate)\n'
    } > "$SANDBOX/bd-nested/CTestTestfile.cmake"
    arm "$L23" 0 ctest --test-dir "$SANDBOX/bd-nested" -j 4
    # The ctest verdict IS part of this arm's precondition: all four tests,
    # including the two that prove the ordering, must have passed -- rc 0 above,
    # and ctest's summary, read in the words EVERY ctest version prints.
    # ⚠ NOT one sentence: ✔MEASURED 2026-09-14, ctest 4.3.2 (Windows, WSL) prints
    #   `100% tests passed, 0 tests failed out of 4` and ctest 4.4.1 (the arm64 VPS)
    #   prints `100% tests passed out of 4`. The needle this arm used to carry,
    #   `tests failed out of 2`, exists only in the older form, so it would have
    #   gone red on any carriage or runner that upgraded CMake.
    says "$L23" '100% tests passed'
    says "$L23" 'out of 4'
    r23a=""; r23s=""
    [ -f "$STAND_IN_STATE/$L23/await-result" ] && read -r r23a < "$STAND_IN_STATE/$L23/await-result"
    [ -f "$STAND_IN_STATE/$L23/stop-result" ] && read -r r23s < "$STAND_IN_STATE/$L23/stop-result"
    stand_in_describe "$L23"; rg_at
    echo "  [info] $L23   independent reading of its stand-in ($RG_SI_DESC): before the gate=${r23a:-never-read}, after the gate=${r23s:-never-read}   $RG_AT"
    if [ "$r23a" = present ] && [ "$r23s" = stopped ]; then
        # ★★★ THE SUBJECT IS *THIS* DESCENDANT, NEVER THE WHOLE MACHINE.
        # [[D-TEST-RUN-GATE-GUARD-ASSERTS-THAT-NO-COMPILER-RUNS-ANYWHERE-ON-THE-MACHINE]]
        # This arm used to require `compilers: none outside` -- a claim about every
        # process on the host, which a project running four lanes cannot keep true.
        # ✔MEASURED 2026-09-15: lane `ca`'s MSVC gate failed here while lane `ih` was
        # compiling; and (lane `pg`) ONE planted foreign `dsscp` alive through an
        # unmodified guard failed exactly this arm and arm 26, both naming that pid,
        # and nothing else. The arm's subject is that a DESCENDANT is classified INSIDE
        # the gate's tree, so it asserts the gate did not name THAT pid -- decidable
        # with any number of unrelated compilers alive, and still red the moment the
        # descendant is misread as outside. Arm 24 remains its control: it must name
        # its own foreign stand-in BY PID, so a matcher that recognised nothing cannot
        # pass both.
        if exit_reason_check "$L23"; then
            stand_in_subject_pid "$L23"
            if [ "$TABLE_STATE" = readable ]; then
                says a23.log 'compilers:' --log
                says_not a23.log "pid $RG_SI_SUBJECT  dsscp" --log
            else
                says a23.log 'compilers: UNKNOWN' --log
            fi
        fi
    else
        precondition_failed "$L23" "its stand-in was not observed present before the nested gate and stopped after it (await '${r23a:-none}', stop '${r23s:-none}'), so the gate's samples were not shown a descendant"
        stop_foreign_compiler "$L23"
    fi

    # ---- ARM 24 (sh) THE DEFECT: A COMPILER OUTSIDE THIS GATE'S TREE --------
    if alive_throughout_arm 24-sh-foreign-compiler sh a24.log; then
        stand_in_subject_pid 24-sh-foreign-compiler
        says 24-sh-foreign-compiler 'run-gate.sh: OK'
        says_named_or_unknown a24.log "pid $RG_SI_SUBJECT  dsscp  seen throughout this run"
        # ⚠ AND IT MUST NOT HAVE BECOME A REFUSAL. The line is an OBSERVATION: the
        # mechanism that made a second compiler destroy a verdict is gone, and
        # refusing here would refuse every gate of a project that runs four lanes in
        # parallel by design. rc 0 above is that claim; this is it said out loud.
        says_not 24-sh-foreign-compiler 'FAIL'
    fi

    # ---- ARM 28 (sh) THE COMPILER THAT EXITS *DURING* THE RUN ---------------
    #
    # ★★★ THE RED-ON-DISABLE HALF OF THE UNION:
    # [[D-SCRIPT-RUN-GATE-COMPILERS-LINE-REPORTS-NONE-WHEN-IT-COULD-NOT-READ-THE-PROCESS-TABLE]]
    # ⚠ ARM 24 ALONE IS SATISFIED BY A SUBJECT THAT ONLY EVER READS ITS LAST SAMPLE.
    # Here the GATED COMMAND stops the stand-in and waits until it is observed
    # gone, so it is alive for the pre-run sample and gone for the post-run one BY
    # CONSTRUCTION, and only a subject that reports the UNION of both samples can
    # name it `seen when this run STARTED` — the exact shape of
    # [[D-PROGRAM-RUNTIME-CACHE-PRUNE-DELETES-A-CONCURRENT-RUNS-LIVE-ARTIFACT]], a
    # compiler that ran during the gate and exited.
    # ⚠ This arm used to plant a 3 s stand-in against a 5 s command on the argument
    # that it was "certainly alive for the pre-run sample". On a runner 2.3x slower
    # it was not, and CI went red on a healthy subject; see the top of this file.
    if exits_midrun_arm 28-sh-compiler-exits-midrun sh a28.log; then
        stand_in_subject_pid 28-sh-compiler-exits-midrun
        says 28-sh-compiler-exits-midrun 'run-gate.sh: OK'
        says_named_or_unknown a28.log "pid $RG_SI_SUBJECT  dsscp  seen when this run STARTED"
        says_not 28-sh-compiler-exits-midrun 'FAIL'
    fi
else
    na 23-sh-descendant-compiler     "$STAND_IN_ABSENT_WHY"
    na 24-sh-foreign-compiler        "$STAND_IN_ABSENT_WHY"
    na 28-sh-compiler-exits-midrun   "$STAND_IN_ABSENT_WHY"
fi

if [ -n "$PS_EXE" ] && [ -n "$STAND_IN" ]; then
    # ---- ARM 25 (ps1) THE DEFECT -------------------------------------------
    if alive_throughout_arm 25-ps1-foreign-compiler ps1 a25.log; then
        stand_in_subject_pid 25-ps1-foreign-compiler
        says 25-ps1-foreign-compiler 'run-gate.ps1: OK'
        says_named_or_unknown a25.log "pid $RG_SI_SUBJECT  dsscp  seen throughout this run"
    fi

    # ---- ARM 26 (ps1) THE CONTROL, i.e. THE ESCAPE IS DIRECTIONAL ----------
    # ⚠ WITHOUT THIS THE TWIN COULD SIMPLY ALWAYS REPORT. Arm 25 alone is
    # satisfied by a line that fires unconditionally, which is the failure this
    # whole subject was written to avoid in the other direction.
    # ⓘ NO WAIT BEFORE IT: every stand-in planted so far was stopped and OBSERVED
    #   gone by its own arm. That is re-checked here, by pid, rather than assumed
    #   -- the `sleep 13` it replaces was a bet that arm 25's 12 s stand-in had died.
    all_gone=1
    for _rg_l in $STAND_IN_PLANTED; do
        stand_in_gone "$_rg_l" || { stand_in_load "$_rg_l" && all_gone=0; }
    done
    if [ "$all_gone" = 1 ]; then
        arm 26-ps1-no-compiler 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
            -File "$GATE_PS1" "$SCRATCH/a26.log" 'HELLO' \
            "$PS_EXE" -NoProfile -Command "Write-Output HELLO"
        rg_at
        echo "  [info] 26-ps1-no-compiler   independent reading: every stand-in this run planted ($STAND_IN_PLANTED ) was gone before the gate   $RG_AT"
        # ★★★ SOUNDNESS, NOT SILENCE.
        # [[D-TEST-RUN-GATE-GUARD-ASSERTS-THAT-NO-COMPILER-RUNS-ANYWHERE-ON-THE-MACHINE]]
        # This arm used to require `compilers: none outside` -- a claim about the whole
        # machine, which one unrelated compiler falsifies. ✔MEASURED 2026-09-15 (lane
        # `pg`): one planted foreign `dsscp` failed this arm and arm 23 and nothing else.
        # Its real subject is that the twin does not report UNCONDITIONALLY, and that is
        # decidable at any load: every process the line names is a compiler (its command
        # line names the compiler image), none is a stand-in this run planted (all were
        # observed gone above), and the count the line states is the number of rows it
        # prints. A line that fires regardless fails the first clause or the last; a
        # correct twin passes with or without unrelated compilers alive.
        compilers_line_is_sound a26.log
    else
        precondition_failed 26-ps1-no-compiler "a stand-in from an earlier arm was still alive, so a 'none' answer could not be asked for"
    fi

    # ---- ARM 27 TWIN PARITY ON THE FIFTH SUBJECT ---------------------------
    ran
    rc_of 24-sh-foreign-compiler; sh_rc="$RG_RC"
    rc_of 25-ps1-foreign-compiler; ps_rc="$RG_RC"
    # ⚠ PID-SPECIFIC. [[D-TEST-RUN-GATE-GUARD-ASSERTS-THAT-NO-COMPILER-RUNS-ANYWHERE-ON-THE-MACHINE]]
    # Counting the bare "OUTSIDE this gate's process tree" header was satisfied by ANY
    # unrelated compiler alive on the machine, so a twin that failed to name its own
    # stand-in still counted as having reported one. Each count is now of the arm's OWN
    # stand-in's row.
    stand_in_subject_pid 24-sh-foreign-compiler
    sh_saw=$(logtext "$SCRATCH/a24.log" | grep -c "pid $RG_SI_SUBJECT  dsscp  seen throughout this run" || true)
    stand_in_subject_pid 25-ps1-foreign-compiler
    ps_saw=$(logtext "$SCRATCH/a25.log" | grep -c "pid $RG_SI_SUBJECT  dsscp  seen throughout this run" || true)
    rg_at
    if [ "$sh_rc" = not-run ] || [ "$ps_rc" = not-run ]; then
        precondition_failed 27-parity "arm 24 or 25 never ran (.sh rc=$sh_rc, .ps1 rc=$ps_rc) -- its own precondition failure is reported above"
    elif [ "$sh_saw" -ge 1 ] && [ "$ps_saw" -ge 1 ] && [ "$sh_rc" = "0" ] && [ "$ps_rc" = "0" ]; then
        echo "  [ok  ] 27-parity  both twins REPORTED a foreign compiler and both still exited 0   $RG_AT"
    else
        echo "  [FAIL] 27-parity  .sh reported=$sh_saw rc=$sh_rc, .ps1 reported=$ps_saw rc=$ps_rc (both must report, both must exit 0)   $RG_AT"
        fails=$((fails + 1))
    fi

    # ---- ARM 29 (ps1) THE COMPILER THAT EXITS *DURING* THE RUN -------------
    # The twin of arm 28, with the SAME gated command. ⚠ A capability in one twin
    # and not the other is this project's canonical silent harness bug, and the
    # union is a capability.
    if exits_midrun_arm 29-ps1-compiler-exits-midrun ps1 a29.log; then
        stand_in_subject_pid 29-ps1-compiler-exits-midrun
        says 29-ps1-compiler-exits-midrun 'run-gate.ps1: OK'
        says_named_or_unknown a29.log "pid $RG_SI_SUBJECT  dsscp  seen when this run STARTED"
    fi

    # ---- ARM 30 TWIN PARITY ON THE UNION -----------------------------------
    # ⚠ ARM 27 CANNOT COVER THIS. It compares the twins on a compiler that was
    # alive for BOTH samples, which a last-sample-wins subject also reports. The
    # parity that broke on CI is the one over a compiler alive for only ONE.
    ran
    rc_of 28-sh-compiler-exits-midrun; sh_urc="$RG_RC"
    rc_of 29-ps1-compiler-exits-midrun; ps_urc="$RG_RC"
    # ⚠ PID-SPECIFIC, for the reason arm 27 gives: an unrelated compiler that happened to
    # exit during the run would otherwise satisfy this count for a twin that failed to
    # name its own stand-in.
    stand_in_subject_pid 28-sh-compiler-exits-midrun
    sh_u=$(logtext "$SCRATCH/a28.log" | grep -c "pid $RG_SI_SUBJECT  dsscp  seen when this run STARTED" || true)
    stand_in_subject_pid 29-ps1-compiler-exits-midrun
    ps_u=$(logtext "$SCRATCH/a29.log" | grep -c "pid $RG_SI_SUBJECT  dsscp  seen when this run STARTED" || true)
    if [ "$TABLE_STATE" = blind ]; then
        sh_u=$(logtext "$SCRATCH/a28.log" | grep -c 'compilers: UNKNOWN' || true)
        ps_u=$(logtext "$SCRATCH/a29.log" | grep -c 'compilers: UNKNOWN' || true)
    fi
    rg_at
    if [ "$sh_urc" = not-run ] || [ "$ps_urc" = not-run ]; then
        precondition_failed 30-parity-union "arm 28 or 29 never ran (.sh rc=$sh_urc, .ps1 rc=$ps_urc) -- its own precondition failure is reported above"
    elif [ "$sh_u" -ge 1 ] && [ "$ps_u" -ge 1 ] && [ "$sh_urc" = "0" ] && [ "$ps_urc" = "0" ]; then
        echo "  [ok  ] 30-parity-union  both twins reported the SAME state ($TABLE_STATE) for a compiler that exited mid-run, and both still exited 0   $RG_AT"
    else
        echo "  [FAIL] 30-parity-union  host=$TABLE_STATE .sh reported=$sh_u rc=$sh_urc, .ps1 reported=$ps_u rc=$ps_urc (both must report the same state, both must exit 0)   $RG_AT"
        fails=$((fails + 1))
    fi
elif [ -z "$STAND_IN" ]; then
    na 25-ps1-foreign-compiler       "$STAND_IN_ABSENT_WHY"
    na 26-ps1-no-compiler            "$STAND_IN_ABSENT_WHY"
    na 27-parity                     "$STAND_IN_ABSENT_WHY -- the twins were NOT compared on the fifth subject"
    na 29-ps1-compiler-exits-midrun  "$STAND_IN_ABSENT_WHY"
    na 30-parity-union               "$STAND_IN_ABSENT_WHY -- the twins were NOT compared on the two-sample union"
else
    na 25-ps1-foreign-compiler       "$PS_ABSENT_WHY -- the .ps1 twin was never shown a foreign compiler"
    na 26-ps1-no-compiler            "$PS_ABSENT_WHY -- the .ps1 twin's no-compiler CONTROL was not taken"
    na 27-parity                     "$PS_ABSENT_WHY -- arms 23 and 24 still prove the .sh twin, but the twins were NOT compared"
    na 29-ps1-compiler-exits-midrun  "$PS_ABSENT_WHY -- the .ps1 twin was never shown a compiler that exits mid-run"
    na 30-parity-union               "$PS_ABSENT_WHY -- arm 28 still proves the .sh twin's union, but the twins were NOT compared on it"
fi

# ═══ THE SIXTH SUBJECT: A PARENT LINK THAT NAMES A RECYCLED PID ════════════════
# [[D-TEST-RUN-GATE-FIXTURE-RACES-FIXED-LIFETIME-PROCESSES-AGAINST-THE-GATES-SAMPLING-LATENCY]]
# ★★★ BOTH TWINS DECIDE "ours" BY WALKING PARENT LINKS, AND ON WINDOWS A PARENT
# LINK GOES STALE. Windows does not reparent an orphan and recycles pids;
# ✔MEASURED on the workstation: under MSYS a bash's own Windows parent is ALREADY
# dead when it starts, and a freed pid comes back after ~108 allocations. Before
# the rule, BOTH twins were driven through the tables below and got FOUR of them
# wrong: a foreign compiler read as ours, an unrelated ctest's tree adopted as the
# gate's own, and a real contender waved through — silently, exit 0 where 4 is owed.
# ★ THE RULE UNDER TEST: a process is another's parent only if it was CREATED
# FIRST; a link naming a younger process is a recycled pid, and the walk ends.
#
# ⓘ SHARED DATA, TWO DRIVERS, ONE COMPARISON. The scenarios are written ONCE, as
#   data, and each twin's driver feeds them to that twin's OWN functions -- the
#   .sh loaded from its source text, the .ps1 through the PowerShell PARSER -- so
#   neither script is executed, nothing in either is settable by a caller, and
#   arm 33 can require the two twins' verdicts to be identical line for line.
# ⓘ EVERY PID IS ODD AND ABOVE 900000000, so none can collide with a real one:
#   Windows pids are multiples of four, Linux caps at 4194304, macOS at 99999.
# ⓘ And each driver first asks its twin's REAL table for this process's own row:
#   the rule is only as good as the key it compares, so a real key must exist.
write_recycled_pid_scenarios() {  # <file>
    cat > "$1" <<'SCENARIOS'
# row is pid|ppid|image|created|command-line; SELF is the classifying process.
scenario|A-a-compilers-dead-parent-pid-went-to-this-gates-own-sampler|-
row|SELF|900000005|bash.exe|20260914222907360291|bash.exe run-gate.sh
row|900000021|900000013|dsscp.exe|20260914222908245336|C:/stand-in/dsscp.exe
row|900000009|SELF|bash.exe|20260914222909050000|bash.exe
row|900000013|900000009|powershell.exe|20260914222909100000|powershell -NoProfile -Command Get-CimInstance Win32_Process
expect|foreign|900000021
expect|recycled|900000021>900000013
end
scenario|B-control-the-same-orphan-whose-parent-pid-was-not-recycled|-
row|SELF|900000005|bash.exe|20260914222907360291|bash.exe run-gate.sh
row|900000021|900000013|dsscp.exe|20260914222908245336|C:/stand-in/dsscp.exe
row|900000009|SELF|bash.exe|20260914222909050000|bash.exe
row|900000017|900000009|powershell.exe|20260914222909100000|powershell -NoProfile -Command Get-CimInstance Win32_Process
expect|foreign|900000021
end
scenario|C-control-a-genuine-descendant-through-a-build-tool|-
row|900000041|900000033|ctest.exe|20260914222900000000|ctest --test-dir C:/x/bd-nested -j 4
row|SELF|900000041|bash.exe|20260914222901000000|bash.exe run-gate.sh
row|900000045|900000041|dsscp.exe|20260914222901000500|C:/stand-in/dsscp.exe
expect|ours|900000045
end
scenario|D-this-gates-dead-parent-pid-went-to-an-unrelated-build-tool|-
row|SELF|900000061|bash.exe|20260914222907360291|bash.exe run-gate.sh
row|900000061|900000057|ctest.exe|20260914222930000000|ctest --test-dir C:/other/build -j 6
row|900000065|900000061|dsscp.exe|20260914222931000000|C:/other/dsscp.exe main.c
expect|foreign|900000065
expect|recycled|SELF>900000061
end
scenario|X-a-contender-holding-this-gates-dead-parent-pid-is-still-a-contender|/rg-recycled-pid/build
row|SELF|900000081|bash.exe|20260914222907360291|bash.exe run-gate.sh
row|900000081|900000077|ctest.exe|20260914222930000000|ctest --test-dir /rg-recycled-pid/build -j 6
expect|contender|900000081
end
scenario|Y-control-a-gate-genuinely-run-inside-that-ctest-is-not-refused|/rg-recycled-pid/build
row|900000101|900000097|ctest.exe|20260914222900000000|ctest --test-dir /rg-recycled-pid/build -j 6
row|SELF|900000101|bash.exe|20260914222901000000|bash.exe run-gate.sh
expect|excluded|900000101
end
SCENARIOS
}

# The .sh twin's FUNCTIONS and the settings they read, never its top level: every
# `name() {` block through its closing `}` at column 0, plus four one-line
# assignments the functions depend on.
sh_subject_definitions() {
    awk '
        /^[A-Za-z_][A-Za-z0-9_]*\(\) *\{/ { cap = 1 }
        !cap && /^run_gate_(build_tools|compiler_image)="[^"]*"$/ { print; next }
        !cap && /^run_gate_(tab|nl)=\$'"'"'[^'"'"']*'"'"'$/ { print; next }
        cap { print }
        cap && /^}/ { cap = 0; print "" }
    ' "$GATE_SH"
}

# One verdict line per expectation: `<scenario>|<kind>|<subject>|<what the twin did>`.
recycled_pid_verdicts_sh() {  # <scenario file>
    (
        eval "$(sh_subject_definitions)"
        for _f in run_gate_scan_contention run_gate_resolve_dir run_gate_process_table; do
            declare -F "$_f" >/dev/null || { echo "EXTRACT-FAILED|$_f"; exit 91; }
        done
        if [ -z "${run_gate_build_tools:-}" ] || [ -z "${run_gate_compiler_image:-}" ]; then
            echo "EXTRACT-FAILED|settings"
            exit 91
        fi
        _me="$$"
        if run_gate_host_is_windows; then read -r _me < "/proc/$$/winpid"; fi
        echo "REALKEY|$(run_gate_process_table 2>/dev/null | awk -F'\t' -v p="$_me" '$1 == p { print $6; exit }')"
        _self=900000001
        run_gate_self_pid() { printf '%s' "$_self"; }
        run_gate_process_table() { printf '%s' "$_tbl"; }
        _tbl=""; _name=""; _bd="-"; _expects=""
        while IFS='|' read -r _k _a _b _c _d _e; do
            case "$_k" in
                (scenario)
                    _name="$_a"; _bd="$_b"; _tbl=""; _expects="" ;;
                (row)
                    [ "$_a" = SELF ] && _a="$_self"
                    [ "$_b" = SELF ] && _b="$_self"
                    _tbl="$_tbl$_a$RG_TAB$_b$RG_TAB$_c$RG_TAB$_e$RG_TAB$_c$RG_TAB$_d$RG_NL" ;;
                (expect)
                    _expects="$_expects$_a|$_b$RG_NL" ;;
                (end)
                    run_gate_contention_note=""
                    run_gate_build_dir=""
                    [ "$_bd" = "-" ] || run_gate_build_dir="$(run_gate_resolve_dir "$_bd")"
                    run_gate_scan_contention
                    while IFS='|' read -r _kind _subj; do
                        [ -n "$_kind" ] || continue
                        case "$_subj" in
                            (SELF*) _real="$_self${_subj#SELF}" ;;
                            (*)     _real="$_subj" ;;
                        esac
                        case "$_kind" in
                            (foreign|ours)
                                if printf '%s\n' "${run_gate_foreign:-}" | awk -F'\t' -v p="$_real" '$1 == p { f = 1 } END { exit !f }'
                                then _got=foreign; else _got=ours; fi ;;
                            (contender|excluded)
                                if printf '%s\n' "${run_gate_contenders:-}" | grep -q "pid $_real "
                                then _got=contender; else _got=excluded; fi ;;
                            (*)
                                if printf '%s\n' "${run_gate_recycled:-}" | grep -qxF "$_real"
                                then _got=recycled; else _got=followed; fi ;;
                        esac
                        echo "$_name|$_kind|$_subj|$_got"
                    done <<EOF
$_expects
EOF
                    ;;
            esac
        done < "$1"
        exit 0
    )
}

# The .ps1 twin's driver: the same contract, through the PowerShell PARSER.
write_recycled_pid_driver_ps1() {  # <file>
    cat > "$1" <<'PS1'
# Generated by test-run-gate.sh: the .ps1 twin's half of the recycled-pid arms.
param()
$gate = [string]$args[0]
$scen = [string]$args[1]
$errs = $null; $toks = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($gate, [ref]$toks, [ref]$errs)
if ($errs.Count -gt 0) { Write-Output 'EXTRACT-FAILED|parse'; exit 91 }
foreach ($fn in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([ScriptBlock]::Create($fn.Extent.Text))
}
foreach ($as in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and $n.Parent -is [System.Management.Automation.Language.NamedBlockAst] }, $false)) {
    if ($as.Left.Extent.Text -eq '$script:RunGateBuildTools' -or $as.Left.Extent.Text -eq '$script:RunGateCompilerImage') {
        . ([ScriptBlock]::Create($as.Extent.Text))
    }
}
foreach ($need in 'Get-RunGateContention', 'Resolve-RunGateDir', 'Get-RunGateProcessTable') {
    if (-not (Get-Command $need -ErrorAction SilentlyContinue)) { Write-Output "EXTRACT-FAILED|$need"; exit 91 }
}
if (-not $script:RunGateBuildTools -or -not $script:RunGateCompilerImage) { Write-Output 'EXTRACT-FAILED|settings'; exit 91 }
$mine = @(Get-RunGateProcessTable | Where-Object { $_.ProcId -eq $PID })
$realKey = if ($mine.Count -gt 0) { [string]$mine[0].Created } else { '' }
Write-Output "REALKEY|$realKey"
$script:ScenarioRows = @()
function Get-RunGateProcessTable { return @($script:ScenarioRows) }
$name = ''; $bd = '-'; $rows = @(); $expects = @()
foreach ($line in (Get-Content -LiteralPath $scen)) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    $f = $line -split '\|'
    if ($f[0] -eq 'scenario') { $name = $f[1]; $bd = $f[2]; $rows = @(); $expects = @(); continue }
    if ($f[0] -eq 'row') {
        $id = if ($f[1] -eq 'SELF') { $PID } else { [int]$f[1] }
        $pp = if ($f[2] -eq 'SELF') { $PID } else { [int]$f[2] }
        $rows += [PSCustomObject]@{ ProcId = [int]$id; ParentId = [int]$pp; Image = $f[3]; CmdLine = $f[5]; Argv0 = $f[3]; Created = $f[4] }
        continue
    }
    if ($f[0] -eq 'expect') { $expects += ,@($f[1], $f[2]); continue }
    if ($f[0] -ne 'end') { continue }
    $script:ScenarioRows = $rows
    $script:RunGateBuildDir = if ($bd -eq '-') { '' } else { Resolve-RunGateDir $bd }
    $script:RunGateRecycled = @()
    Get-RunGateContention
    foreach ($e in $expects) {
        $kind = $e[0]; $subj = $e[1]
        $real = if ($subj.StartsWith('SELF')) { "$PID" + $subj.Substring(4) } else { $subj }
        if ($kind -eq 'foreign' -or $kind -eq 'ours') {
            $got = if (@($script:RunGateForeign | Where-Object { "$($_.ProcId)" -eq $real }).Count -gt 0) { 'foreign' } else { 'ours' }
        } elseif ($kind -eq 'contender' -or $kind -eq 'excluded') {
            $got = if (@($script:RunGateContenders | Where-Object { $_ -match ('^\s*pid ' + [regex]::Escape($real) + '\s') }).Count -gt 0) { 'contender' } else { 'excluded' }
        } else {
            $got = if (@($script:RunGateRecycled) -contains $real) { 'recycled' } else { 'followed' }
        }
        Write-Output "$name|$kind|$subj|$got"
    }
}
exit 0
PS1
}

# THE JUDGEMENT, shared by both twins' arms: the expectation's kind IS the
# correct verdict, every expectation in the data must have produced a line, and
# the twin's REAL table must have given this process a 20-digit creation key.
judge_recycled_pid_verdicts() {  # <label> <verdict file> <driver rc> <scenario file>
    local label=$1 file=$2 drc=$3 scen=$4 key n_want n_got name kind subj got
    rg_at
    if [ "$drc" -ne 0 ] || grep -q '^EXTRACT-FAILED' "$file"; then
        echo "  [FAIL] $label   the driver could not load the subject's own definitions (rc=$drc): $(tr -d '\r' < "$file" | head -3 | tr '\n' ' ')   $RG_AT"
        fails=$((fails + 1))
        return 1
    fi
    key="$(tr -d '\r' < "$file" | sed -n 's/^REALKEY|//p')"
    case "$key" in
        (*[!0-9]*|"") key_ok=0 ;;
        (*) key_ok=1; [ "${#key}" -eq 20 ] || key_ok=0 ;;
    esac
    if [ "$key_ok" = 1 ]; then
        echo "  [ok  ] $label   the subject's REAL process table gives this process a 20-digit creation key ($key)"
    else
        echo "  [FAIL] $label   the subject's REAL process table gives this process no 20-digit creation key ('$key'), so the recycled-pid rule has nothing to compare on this host"
        fails=$((fails + 1))
    fi
    n_want=$(grep -c '^expect|' "$scen")
    n_got=0
    while IFS='|' read -r name kind subj got; do
        [ "$name" = REALKEY ] && continue
        [ -n "$name" ] || continue
        n_got=$((n_got + 1))
        if [ "$got" = "$kind" ]; then
            echo "  [ok  ] $label   $name: $subj is $got"
        else
            echo "  [FAIL] $label   $name: $subj was classified '$got', and the correct verdict is '$kind'"
            fails=$((fails + 1))
        fi
    done <<EOF
$(tr -d '\r' < "$file")
EOF
    if [ "$n_got" -ne "$n_want" ]; then
        echo "  [FAIL] $label   $n_got verdict(s) for the $n_want expectation(s) in the scenario data"
        fails=$((fails + 1))
    fi
    return 0
}

SCEN="$SCRATCH/recycled-pid-scenarios.txt"
write_recycled_pid_scenarios "$SCEN"

# ---- ARM 31 (sh) A RECYCLED PID IS NEVER A PARENT -----------------------------
ran
rg_now; t31="$RG_NOW_US"
recycled_pid_verdicts_sh "$SCEN" > "$SCRATCH/31-sh-recycled-pid.verdicts" 2> "$SCRATCH/31-sh-recycled-pid.err"
rc31=$?
rg_now; rg_span "$t31" "$RG_NOW_US"
echo "  [info] 31-sh-recycled-pid   the .sh twin's own classifier, driven through $(grep -c '^scenario|' "$SCEN") constructed tables, took ${RG_SPAN}s"
judge_recycled_pid_verdicts 31-sh-recycled-pid "$SCRATCH/31-sh-recycled-pid.verdicts" "$rc31" "$SCEN"

if [ -n "$PS_EXE" ]; then
    # ---- ARM 32 (ps1) A RECYCLED PID IS NEVER A PARENT ------------------------
    write_recycled_pid_driver_ps1 "$SCRATCH/recycled-pid-verdicts.ps1"
    native_path "$SCRATCH/recycled-pid-verdicts.ps1"; drv_w="$RG_NATIVE"
    native_path "$SCEN"; scen_w="$RG_NATIVE"
    ran
    rg_now; t32="$RG_NOW_US"
    "$PS_EXE" -NoProfile -ExecutionPolicy Bypass -File "$drv_w" "$GATE_PS1_W" "$scen_w" \
        > "$SCRATCH/32-ps1-recycled-pid.verdicts" 2> "$SCRATCH/32-ps1-recycled-pid.err"
    rc32=$?
    rg_now; rg_span "$t32" "$RG_NOW_US"
    echo "  [info] 32-ps1-recycled-pid   the .ps1 twin's own classifier, driven through the same tables, took ${RG_SPAN}s"
    judge_recycled_pid_verdicts 32-ps1-recycled-pid "$SCRATCH/32-ps1-recycled-pid.verdicts" "$rc32" "$SCEN"

    # ---- ARM 33 TWIN PARITY ON THE RECYCLED-PID RULE --------------------------
    ran
    tr -d '\r' < "$SCRATCH/31-sh-recycled-pid.verdicts"  | grep -v '^REALKEY|' > "$SCRATCH/33.sh.cmp"
    tr -d '\r' < "$SCRATCH/32-ps1-recycled-pid.verdicts" | grep -v '^REALKEY|' > "$SCRATCH/33.ps1.cmp"
    rg_at
    if [ -s "$SCRATCH/33.sh.cmp" ] && cmp -s "$SCRATCH/33.sh.cmp" "$SCRATCH/33.ps1.cmp"; then
        echo "  [ok  ] 33-parity-recycled-pid  both twins reached the SAME $(grep -c . "$SCRATCH/33.sh.cmp") verdicts on the same tables   $RG_AT"
    else
        echo "  [FAIL] 33-parity-recycled-pid  the twins' verdicts differ on the same tables:   $RG_AT"
        diff "$SCRATCH/33.sh.cmp" "$SCRATCH/33.ps1.cmp" | sed 's/^/         /' | head -12
        fails=$((fails + 1))
    fi
else
    na 32-ps1-recycled-pid       "$PS_ABSENT_WHY -- the .ps1 twin's classifier was never shown a recycled pid"
    na 33-parity-recycled-pid    "$PS_ABSENT_WHY -- arm 31 still proves the .sh twin, but the twins were NOT compared"
fi

# ═══ THE SEVENTH SUBJECT: A WITNESS THE WRAPPER WROTE ITSELF ═══════════════════
# [[D-TEST-RUN-GATE-FIXTURE-RACES-FIXED-LIFETIME-PROCESSES-AGAINST-THE-GATES-SAMPLING-LATENCY]]
# ★★★ THE WITNESS MUST COME FROM THE COMMAND'S OUTPUT, AND IT USED NOT TO HAVE TO.
# ✔MEASURED 2026-09-14 while this fixture was being rebuilt: both twins append the
# gate command's argv to the log (`command : …`) BEFORE they search it for the
# witness, so a command that printed NOTHING but spelled the witness in its own
# argv came back OK from both — `bash -c ': ZQX-WITNESS'` and
# `$null = "ZQX-WITNESS"`. Every arm above that pairs a witness with `echo HELLO`
# carries HELLO in its argv too, so none of them could ever have seen it.
# ★ THE PAIR IS THE POINT: 34/36 put the witness ONLY in the argv and must be
#   refused (exit 1); 35/37 put it ONLY in the output — the argv spells it split
#   in two — and must pass, or the refusal arms are equally consistent with a
#   wrapper that now refuses every witness.
arm 34-sh-witness-only-in-argv 1 bash "$GATE_SH" "$SCRATCH/a34.log" 'RG-WITNESS-ARGV-ONLY' \
    bash -c ': RG-WITNESS-ARGV-ONLY'
says 34-sh-witness-only-in-argv 'NO EVIDENCE it did any work'
arm 35-sh-witness-only-in-output 0 bash "$GATE_SH" "$SCRATCH/a35.log" 'RG-WITNESS-OUTPUT-ONLY' \
    bash -c 'printf "%s-%s\n" RG-WITNESS OUTPUT-ONLY'
says 35-sh-witness-only-in-output 'run-gate.sh: OK'

if [ -n "$PS_EXE" ]; then
    arm 36-ps1-witness-only-in-argv 1 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a36.log" 'RG-WITNESS-ARGV-ONLY' \
        "$PS_EXE" -NoProfile -Command '$null = "RG-WITNESS-ARGV-ONLY"'
    says 36-ps1-witness-only-in-argv 'NO EVIDENCE it did any work'
    arm 37-ps1-witness-only-in-output 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a37.log" 'RG-WITNESS-OUTPUT-ONLY' \
        "$PS_EXE" -NoProfile -Command "Write-Output ('RG-WITNESS-' + 'OUTPUT-ONLY')"
    says 37-ps1-witness-only-in-output 'run-gate.ps1: OK'

    # ---- ARM 38 TWIN PARITY ON THE WITNESS'S SOURCE ---------------------------
    ran
    rc_of 34-sh-witness-only-in-argv;   w_sh_argv="$RG_RC"
    rc_of 35-sh-witness-only-in-output; w_sh_out="$RG_RC"
    rc_of 36-ps1-witness-only-in-argv;  w_ps_argv="$RG_RC"
    rc_of 37-ps1-witness-only-in-output; w_ps_out="$RG_RC"
    rg_at
    if [ "$w_sh_argv" = 1 ] && [ "$w_ps_argv" = 1 ] && [ "$w_sh_out" = 0 ] && [ "$w_ps_out" = 0 ]; then
        echo "  [ok  ] 38-parity-witness  both twins take the witness from the command's OUTPUT only (argv-only 1/1, output-only 0/0)   $RG_AT"
    else
        echo "  [FAIL] 38-parity-witness  argv-only: .sh=$w_sh_argv .ps1=$w_ps_argv (both must be 1); output-only: .sh=$w_sh_out .ps1=$w_ps_out (both must be 0)   $RG_AT"
        fails=$((fails + 1))
    fi
else
    na 36-ps1-witness-only-in-argv   "$PS_ABSENT_WHY -- the .ps1 twin was never shown a witness spelled only in its argv"
    na 37-ps1-witness-only-in-output "$PS_ABSENT_WHY -- the .ps1 twin's output-only CONTROL was not taken"
    na 38-parity-witness             "$PS_ABSENT_WHY -- arms 34 and 35 still prove the .sh twin, but the twins were NOT compared"
fi

# ═══ THE EIGHTH SUBJECT: A LOG PATH ANOTHER LIVE RUN-GATE HOLDS ════════════════
# [[D-SCRIPT-WSL-LEG-AND-RUN-GATE-LET-CONCURRENT-LEGS-SHARE-ONE-LOG-PATH]]
# ✔MEASURED 2026-09-15 (P66, lane `pg`), a second run started on a log path while the
# first was live: .sh/.sh left the first run exit 3 (its fingerprint deleted by the
# second run's cleanup) and its output ERASED while the second reported OK; .sh/.ps1
# left NEITHER run's output in the log; a .ps1 first run refused a second one only by
# the accident of its open handle. On two real wsl-leg runs sharing
# /tmp/wsl-leg-ctest.log, a leg reported OK over a log naming another clone.
# ★ THE ARMS ARE A SET: a live HOLDER on a path (held by this fixture, never by a
#   clock); a CHALLENGER of each twin on that path must refuse 5 WITHOUT touching the
#   holder's log; the holder must still finish 0 with its own evidence and release the
#   record; the released path must take a new run (CONTROL); a record left by a
#   HARD-KILLED holder must be RECLAIMED, across the twins (CONTROL: liveness decides,
#   not the file's existence); and a record from another pid namespace, or an empty
#   one, must still refuse -- the silent direction stays shut.

# The pid a run-gate's owner record names, read from the record itself.
owner_record_pid() {  # <log basename> -> RG_OWNER_PID
    RG_OWNER_PID="$(sed -n 's/^pid=//p' "$SCRATCH/$1.run-gate-owner" 2>/dev/null | tr -d '\r' | head -1)"
}

# Kills a process by the pid its OWN owner record names, with no chance to clean up --
# the shape a TerminateProcess or a SIGKILL leaves. Windows pids go through PowerShell,
# which takes a Windows pid; elsewhere a plain SIGKILL.
hard_kill_recorded_pid() {  # <pid in the process table's namespace>
    case "$1" in ''|*[!0-9]*) return 1 ;; esac
    if run_gate_host_is_windows; then
        "$PS_EXE" -NoProfile -Command "Stop-Process -Id $1 -Force -ErrorAction Stop" >/dev/null 2>&1
    else
        kill -9 "$1" 2>/dev/null
    fi
}

# Starts a HOLDER gate in the background and blocks until its gated command is live.
start_log_holder() {  # <label> <twin: sh|ps1> <log basename> -> HOLDER_BG
    local label=$1
    rm -rf "${STAND_IN_STATE:?}/$label"
    mkdir -p "$STAND_IN_STATE/$label"
    STAND_IN_PLANTED="$STAND_IN_PLANTED $label"
    rm -f "$SCRATCH/$3" "$SCRATCH/$3".run-gate-*
    if [ "$2" = sh ]; then
        bash "$GATE_SH" "$SCRATCH/$3" 'LOG-HOLDER-DONE' \
            "$BASH" "$SELF_SCRIPT" __stand-in logholder "$label" "$STAND_IN" \
            > "$SCRATCH/$label.holder.out" 2>&1 &
    else
        "$PS_EXE" -NoProfile -ExecutionPolicy Bypass -File "$GATE_PS1" "$SCRATCH/$3" 'LOG-HOLDER-DONE' \
            "$BASH_W" "$SELF_SCRIPT_W" __stand-in logholder "$label" "$STAND_IN_W" \
            > "$SCRATCH/$label.holder.out" 2>&1 &
    fi
    HOLDER_BG=$!
    stand_in_wait present "$label" && return 0
    precondition_failed "$label" "the holder gate's command never became live within the ${RG_HANG_GUARD_S}s hang guard"
    return 1
}

# Releases a holder's command and waits for the holder gate to finish -> HOLDER_RC.
finish_log_holder() {  # <label>
    local start=$SECONDS
    HOLDER_RC=not-run
    if ! stand_in_stop "$1"; then
        precondition_failed "$1" "the holder's stand-in could not be stopped"
        return 1
    fi
    while kill -0 "$HOLDER_BG" 2>/dev/null; do
        if [ $((SECONDS - start)) -ge "$RG_HANG_GUARD_S" ]; then
            precondition_failed "$1" "the holder gate was still running ${RG_HANG_GUARD_S}s after its command was released"
            return 1
        fi
        sleep 0.2   # the poll cadence of a hang-guarded wait: no outcome depends on its length
    done
    wait "$HOLDER_BG"; HOLDER_RC=$?
    return 0
}

record_is_gone() {  # <log basename>
    ran; rg_at
    if [ -e "$SCRATCH/$1.run-gate-owner" ]; then
        echo "  [FAIL] $1   its owner record is still on disk after the holder finished   $RG_AT"
        fails=$((fails + 1))
    else
        echo "  [ok  ] $1   its owner record was released when the holder finished   $RG_AT"
    fi
}

# One holder, a challenger of each twin, the holder's completion, and the CONTROL.
log_holder_arms() {  # <holder twin> <holder label> <log> <sh challenger label> <ps1 challenger label> <completes label> <again label>
    local ht=$1 lh=$2 lg=$3
    if ! start_log_holder "$lh" "$ht" "$lg"; then
        stand_in_stop "$lh" >/dev/null 2>&1
        return 1
    fi
    ran; rg_at
    echo "  [ok  ] $lh   the $ht holder's gated command is live (its stand-in observed present)   $RG_AT"
    says "$lg" "LOG-HOLDER-STARTED $lh" --log
    arm "$4" 5 bash "$GATE_SH" "$SCRATCH/$lg" 'CHALLENGER-WITNESS' \
        bash -c 'printf "%s-%s\n" CHALLENGER RAN; printf "%s-%s\n" CHALLENGER WITNESS'
    says "$4" 'ANOTHER LIVE RUN-GATE HOLDS THIS LOG PATH'
    says "$4" 'holder  : pid'
    if [ -n "$PS_EXE" ]; then
        arm "$5" 5 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
            -File "$GATE_PS1" "$SCRATCH/$lg" 'CHALLENGER-WITNESS' \
            "$PS_EXE" -NoProfile -Command "Write-Output ('CHALLENGER-' + 'RAN'); Write-Output ('CHALLENGER-' + 'WITNESS')"
        says "$5" 'ANOTHER LIVE RUN-GATE HOLDS THIS LOG PATH'
        says "$5" 'holder  : pid'
    else
        na "$5" "$PS_ABSENT_WHY -- the .ps1 twin was never shown a held log path"
    fi
    # The holder's evidence is intact WHILE it is still live -- the moment that matters.
    says "$lg" "LOG-HOLDER-STARTED $lh" --log
    says_not "$lg" 'CHALLENGER-RAN' --log
    if finish_log_holder "$lh"; then
        ran; rg_at
        if [ "$HOLDER_RC" = 0 ]; then
            echo "  [ok  ] $6   the holder finished rc=0 after its challengers were refused   $RG_AT"
        else
            echo "  [FAIL] $6   the holder finished rc=$HOLDER_RC (want 0)   $RG_AT"
            logtext "$SCRATCH/$lh.holder.out" | sed 's/^/         /' | head -12
            fails=$((fails + 1))
        fi
        says "$lg" 'LOG-HOLDER-DONE' --log
        says "$lg" 'logpath : held by this run alone' --log
        says_not "$lg" 'CHALLENGER-RAN' --log
        record_is_gone "$lg"
    fi
    # CONTROL: a path whose holder has finished takes the next run.
    if [ "$ht" = sh ]; then
        arm "$7" 0 bash "$GATE_SH" "$SCRATCH/$lg" 'AGAIN-OK' bash -c 'printf "%s-%s\n" AGAIN OK'
        says "$7" 'run-gate.sh: OK'
    else
        arm "$7" 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
            -File "$GATE_PS1" "$SCRATCH/$lg" 'AGAIN-OK' \
            "$PS_EXE" -NoProfile -Command "Write-Output ('AGAIN-' + 'OK')"
        says "$7" 'run-gate.ps1: OK'
    fi
    record_is_gone "$lg"
}

if [ -n "$STAND_IN" ]; then
    # ---- ARMS 39-43: a .sh HOLDER --------------------------------------------
    log_holder_arms sh 39-sh-holds-log a39.log \
        40-sh-challenger-vs-sh-holder 41-ps1-challenger-vs-sh-holder \
        42-sh-holder-completes 43-sh-same-path-again
    if [ -n "$PS_EXE" ]; then
        # ---- ARMS 44-48: a .ps1 HOLDER ---------------------------------------
        log_holder_arms ps1 44-ps1-holds-log a44.log \
            45-sh-challenger-vs-ps1-holder 46-ps1-challenger-vs-ps1-holder \
            47-ps1-holder-completes 48-ps1-same-path-again
    else
        na 44-ps1-holds-log "$PS_ABSENT_WHY -- no .ps1 holder could be started"
    fi

    # ---- ARM 49 TWIN PARITY ON A HELD LOG PATH -------------------------------
    ran
    rc_of 40-sh-challenger-vs-sh-holder;   p40="$RG_RC"
    rc_of 41-ps1-challenger-vs-sh-holder;  p41="$RG_RC"
    rc_of 45-sh-challenger-vs-ps1-holder;  p45="$RG_RC"
    rc_of 46-ps1-challenger-vs-ps1-holder; p46="$RG_RC"
    rg_at
    if [ -z "$PS_EXE" ]; then
        na 49-parity-log-held "$PS_ABSENT_WHY -- arm 40 still proves the .sh twin, but the twins were NOT compared"
    elif [ "$p40$p41$p45$p46" = 5555 ]; then
        echo "  [ok  ] 49-parity-log-held  every challenger of either twin refused a held path with exit 5 (sh-held 5/5, ps1-held 5/5)   $RG_AT"
    else
        echo "  [FAIL] 49-parity-log-held  sh holder: .sh=$p40 .ps1=$p41; ps1 holder: .sh=$p45 .ps1=$p46 (all must be 5)   $RG_AT"
        fails=$((fails + 1))
    fi

    # ---- ARMS 50-51: A RECORD LEFT BY A HARD-KILLED HOLDER IS RECLAIMED --------
    # ⚠ The .sh holder's command writes the log FILE directly, so after its gate is
    #   killed it may still print; this waits for that last line before the reclaim
    #   runs, so nothing but the reclaiming run writes the log afterwards. A .ps1
    #   holder's command writes through a pipe its dead PowerShell owned, so nothing
    #   of it can reach the file after the kill.
    if [ -n "$PS_EXE" ]; then
        LS=50-stale-sh-record
        if start_log_holder "$LS" sh a50.log; then
            owner_record_pid a50.log; stale50="$RG_OWNER_PID"
            hard_kill_recorded_pid "$stale50"
            stand_in_stop "$LS" >/dev/null 2>&1
            wait "$HOLDER_BG" 2>/dev/null
            _rg_n=0
            while ! logtext "$SCRATCH/a50.log" | grep -qF 'LOG-HOLDER-DONE' && [ "$_rg_n" -lt $((RG_HANG_GUARD_S * 5)) ]; do
                _rg_n=$((_rg_n + 1)); sleep 0.2
            done
            if [ -e "$SCRATCH/a50.log.run-gate-owner" ]; then
                arm 50-ps1-reclaims-stale-sh-record 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
                    -File "$GATE_PS1" "$SCRATCH/a50.log" 'RECLAIM-OK' \
                    "$PS_EXE" -NoProfile -Command "Write-Output ('RECLAIM-' + 'OK')"
                says a50.log 'RECLAIMED a stale record' --log
                says a50.log "pid $stale50" --log
                record_is_gone a50.log
            else
                precondition_failed 50-ps1-reclaims-stale-sh-record "the killed .sh holder (pid $stale50) left no owner record, so there was nothing stale to reclaim"
            fi
        fi
        LS=51-stale-ps1-record
        if start_log_holder "$LS" ps1 a51.log; then
            owner_record_pid a51.log; stale51="$RG_OWNER_PID"
            hard_kill_recorded_pid "$stale51"
            stand_in_stop "$LS" >/dev/null 2>&1
            wait "$HOLDER_BG" 2>/dev/null
            if [ -e "$SCRATCH/a51.log.run-gate-owner" ]; then
                arm 51-sh-reclaims-stale-ps1-record 0 bash "$GATE_SH" "$SCRATCH/a51.log" 'RECLAIM-OK' \
                    bash -c 'printf "%s-%s\n" RECLAIM OK'
                says a51.log 'RECLAIMED a stale record' --log
                says a51.log "pid $stale51" --log
                record_is_gone a51.log
            else
                precondition_failed 51-sh-reclaims-stale-ps1-record "the killed .ps1 holder (pid $stale51) left no owner record, so there was nothing stale to reclaim"
            fi
        fi
    else
        na 50-ps1-reclaims-stale-sh-record "$PS_ABSENT_WHY -- a cross-twin reclaim needs both twins"
        na 51-sh-reclaims-stale-ps1-record "$PS_ABSENT_WHY -- a cross-twin reclaim needs both twins"
    fi
else
    na 39-sh-holds-log "$STAND_IN_ABSENT_WHY -- no holder could be kept live without a stand-in"
fi

# ---- ARMS 52-56: RECORDS NO LIVENESS RULE MAY RECLAIM ------------------------
# A record from another pid namespace cannot be judged from here, and an empty one may
# be a sibling's half-written create; both must REFUSE, from either twin.
write_constructed_record() {  # <log basename> <namespace>
    printf 'run-gate-owner-record: constructed by test-run-gate.sh\ntoken=constructed-0-1\npid=1\ncreated=\nnamespace=%s\nshell=test-run-gate.sh\ncommand=constructed\n' "$2" \
        > "$SCRATCH/$1.run-gate-owner"
}
write_constructed_record a52.log 'elsewhere:no-such-host:'
arm 52-sh-foreign-namespace-record 5 bash "$GATE_SH" "$SCRATCH/a52.log" 'NS-OK' bash -c 'printf "%s-%s\n" NS OK'
says 52-sh-foreign-namespace-record 'pid namespace'
: > "$SCRATCH/a54.log.run-gate-owner"
arm 54-sh-empty-record 5 bash "$GATE_SH" "$SCRATCH/a54.log" 'EMPTY-OK' bash -c 'printf "%s-%s\n" EMPTY OK'
says 54-sh-empty-record 'empty, half-written'
if [ -n "$PS_EXE" ]; then
    arm 53-ps1-foreign-namespace-record 5 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a52.log" 'NS-OK' \
        "$PS_EXE" -NoProfile -Command "Write-Output ('NS-' + 'OK')"
    says 53-ps1-foreign-namespace-record 'pid namespace'
    arm 55-ps1-empty-record 5 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a54.log" 'EMPTY-OK' \
        "$PS_EXE" -NoProfile -Command "Write-Output ('EMPTY-' + 'OK')"
    says 55-ps1-empty-record 'empty, half-written'
    ran
    rc_of 52-sh-foreign-namespace-record; p52="$RG_RC"
    rc_of 53-ps1-foreign-namespace-record; p53="$RG_RC"
    rc_of 54-sh-empty-record; p54="$RG_RC"
    rc_of 55-ps1-empty-record; p55="$RG_RC"
    rg_at
    if [ "$p52$p53$p54$p55" = 5555 ]; then
        echo "  [ok  ] 56-parity-unjudgeable-record  both twins refused a foreign-namespace record (5/5) and an empty one (5/5)   $RG_AT"
    else
        echo "  [FAIL] 56-parity-unjudgeable-record  foreign namespace: .sh=$p52 .ps1=$p53; empty: .sh=$p54 .ps1=$p55 (all must be 5)   $RG_AT"
        fails=$((fails + 1))
    fi
else
    na 53-ps1-foreign-namespace-record "$PS_ABSENT_WHY"
    na 55-ps1-empty-record             "$PS_ABSENT_WHY"
    na 56-parity-unjudgeable-record    "$PS_ABSENT_WHY -- arms 52 and 54 still prove the .sh twin, but the twins were NOT compared"
fi
rm -f "$SCRATCH/a52.log.run-gate-owner" "$SCRATCH/a54.log.run-gate-owner"

# ═══ THE NINTH SUBJECT: A FILE CHANGED AND RESTORED DURING THE RUN ═════════════
# [[D-SCRIPT-RUN-GATE-INPUTS-HELD-STILL-OVER-A-FILE-CHANGED-AND-RESTORED-MID-RUN]]
# ✔MEASURED 2026-09-15 (P66, lane `pg`), both twins, a gated command that let a watched
# file be changed, READ the changed bytes, and let it be restored: restored by a plain
# rewrite -> exit 3; restored by `cp -p`, by PowerShell `Copy-Item`, or rewritten and
# `touch -r`'d -> exit 0, `inputs  : held still`.
# ★ THE GATED COMMAND DOES ALL THREE STEPS ITSELF (a script ctest runs), so the change
#   and the restore land between the twins' two snapshots by construction.
# ★ HOST-CONDITIONAL, AND THE CONDITION IS READ FROM THE SUBJECT: the witness is ctime
#   (perl) on POSIX and the NTFS USN (PowerShell) on Windows. Where a twin reports
#   `changes : NOT WATCHED`, it cannot be asked to refuse, so its arms are NOT
#   APPLICABLE -- named and counted. The untouched-tree CONTROL below, and every
#   `held still` arm above, must pass on every host: a witness that moved on its own
#   would break them all.
change_state_of() {  # <log basename> -> RG_CHANGE_STATE
    RG_CHANGE_STATE=""
    if   logtext "$SCRATCH/$1" | grep -qF 'changes : watched';     then RG_CHANGE_STATE=watched
    elif logtext "$SCRATCH/$1" | grep -qF 'changes : NOT WATCHED'; then RG_CHANGE_STATE=unwatched
    fi
}
change_state_of a1.log; SH_CHANGE="$RG_CHANGE_STATE"
ran; rg_at
if [ -n "$SH_CHANGE" ]; then
    echo "  [ok  ] 57-probe-change-witness   run-gate.sh reports its change witness as $SH_CHANGE on this host (MEASURED from a1.log)   $RG_AT"
else
    echo "  [FAIL] 57-probe-change-witness   a1.log carries no 'changes :' line   $RG_AT"
    fails=$((fails + 1))
fi
PS_CHANGE=""
if [ -n "$PS_EXE" ]; then change_state_of a4.log; PS_CHANGE="$RG_CHANGE_STATE"; fi

TREE_D="${SCRATCH}/gate-tree-d"
mk_source_tree "$TREE_D"
RESTORED="$TREE_D/examples/.probe-restored.txt"
native_path "$RESTORED"; RESTORED_W="$RG_NATIVE"
native_path "$SCRATCH/probe-restored.orig"; RESTORED_ORIG_W="$RG_NATIVE"
# Before EVERY arm: a fresh file, back-dated, and its scratch copy -- a write BEFORE the
# run, which both snapshots then see alike.
reset_restored_probe() {
    echo ORIGINAL > "$RESTORED"
    touch -t 202001010000 "$RESTORED"
    cp -p "$RESTORED" "$SCRATCH/probe-restored.orig"
}
# The two restores, each a script ctest runs as the gate command's only test.
printf '%s\n' '#!/usr/bin/env bash' \
    "echo MUTATED > '$RESTORED'" \
    "cat '$RESTORED'" \
    "cp -p '$SCRATCH/probe-restored.orig' '$RESTORED'" \
    'echo ok' > "$SCRATCH/restore-cp-p.sh"
printf '%s\n' '#!/usr/bin/env bash' \
    "echo MUTATED > '$RESTORED'" \
    "cat '$RESTORED'" \
    "\"\$1\" -NoProfile -Command \"Copy-Item -LiteralPath '$RESTORED_ORIG_W' -Destination '$RESTORED_W' -Force\"" \
    'echo ok' > "$SCRATCH/restore-copy-item.sh"
native_path "$SCRATCH/restore-cp-p.sh"; RESTORE_CPP_W="$RG_NATIVE"
native_path "$SCRATCH/restore-copy-item.sh"; RESTORE_CI_W="$RG_NATIVE"
mkdir -p "$TREE_D/build/cp-p" "$TREE_D/build/copy-item" "$TREE_D/build/untouched"
for _rg_bd in cp-p copy-item untouched; do
    printf 'CMAKE_HOME_DIRECTORY:INTERNAL=%s\n' "$(gate_spelling "$TREE_D")" > "$TREE_D/build/$_rg_bd/CMakeCache.txt"
done
printf 'add_test(probe "%s" "%s")\n' "$BASH_W" "$RESTORE_CPP_W" > "$TREE_D/build/cp-p/CTestTestfile.cmake"
printf 'add_test(probe "%s" "%s" "%s")\n' "$BASH_W" "$RESTORE_CI_W" "${PS_EXE:-pwsh}" > "$TREE_D/build/copy-item/CTestTestfile.cmake"
printf 'add_test(probe "%s" "-c" "echo ok")\n' "$BASH_W" > "$TREE_D/build/untouched/CTestTestfile.cmake"

restored_arm() {  # <label> <twin: sh|ps1> <build dir> <want> <log basename>
    reset_restored_probe
    if [ "$2" = sh ]; then
        arm "$1" "$4" bash "$GATE_SH" "$SCRATCH/$5" '100% tests passed' ctest --test-dir "$TREE_D/build/$3"
    else
        arm "$1" "$4" "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
            -File "$GATE_PS1" "$SCRATCH/$5" '100% tests passed' ctest --test-dir "$TREE_D/build/$3"
    fi
}

# ---- ARM 58 (sh) CONTROL: the witness does not move on an untouched tree -----------
restored_arm 58-sh-untouched-control sh untouched 0 a58.log
says 58-sh-untouched-control 'run-gate.sh: OK'
says a58.log 'inputs  : held still' --log
if [ "$SH_CHANGE" = watched ]; then
    says a58.log 'changes : watched' --log
    # ---- ARM 59 (sh) THE DEFECT: changed, read, restored with `cp -p` ----------
    restored_arm 59-sh-cp-p-restore-refuses sh cp-p 3 a59.log
    says 59-sh-cp-p-restore-refuses 'the tree CHANGED UNDER THE RUN'
    says 59-sh-cp-p-restore-refuses '.probe-restored.txt'
    if [ -n "$PS_EXE" ]; then
        # ---- ARM 60 (sh) THE DEFECT: restored by PowerShell Copy-Item ------------
        restored_arm 60-sh-copy-item-restore-refuses sh copy-item 3 a60.log
        says 60-sh-copy-item-restore-refuses 'the tree CHANGED UNDER THE RUN'
        says 60-sh-copy-item-restore-refuses '.probe-restored.txt'
    else
        na 60-sh-copy-item-restore-refuses "$PS_ABSENT_WHY -- no Copy-Item to restore with"
    fi
else
    na 59-sh-cp-p-restore-refuses       "run-gate.sh does not watch changes mid-run on this host ($(logtext "$SCRATCH/a58.log" | sed -n 's/^changes : //p' | head -1))"
    na 60-sh-copy-item-restore-refuses  "run-gate.sh does not watch changes mid-run on this host"
fi
if [ -n "$PS_EXE" ]; then
    # ---- ARM 61 (ps1) CONTROL ----------------------------------------------------
    restored_arm 61-ps1-untouched-control ps1 untouched 0 a61.log
    says 61-ps1-untouched-control 'run-gate.ps1: OK'
    says a61.log 'inputs  : held still' --log
    if [ "$PS_CHANGE" = watched ]; then
        says a61.log 'changes : watched' --log
        restored_arm 62-ps1-cp-p-restore-refuses ps1 cp-p 3 a62.log
        says 62-ps1-cp-p-restore-refuses 'the tree CHANGED UNDER THE RUN'
        says 62-ps1-cp-p-restore-refuses '.probe-restored.txt'
        restored_arm 63-ps1-copy-item-restore-refuses ps1 copy-item 3 a63.log
        says 63-ps1-copy-item-restore-refuses 'the tree CHANGED UNDER THE RUN'
        says 63-ps1-copy-item-restore-refuses '.probe-restored.txt'
    else
        na 62-ps1-cp-p-restore-refuses       "run-gate.ps1 does not watch changes mid-run on this host ($(logtext "$SCRATCH/a61.log" | sed -n 's/^changes : //p' | head -1))"
        na 63-ps1-copy-item-restore-refuses  "run-gate.ps1 does not watch changes mid-run on this host"
    fi
    # ---- ARM 64 TWIN PARITY ON THE RESTORED CHANGE -------------------------------
    ran
    rc_of 58-sh-untouched-control;          r58="$RG_RC"
    rc_of 61-ps1-untouched-control;         r61="$RG_RC"
    rc_of 59-sh-cp-p-restore-refuses;       r59="$RG_RC"
    rc_of 62-ps1-cp-p-restore-refuses;      r62="$RG_RC"
    rc_of 60-sh-copy-item-restore-refuses;  r60="$RG_RC"
    rc_of 63-ps1-copy-item-restore-refuses; r63="$RG_RC"
    rg_at
    if [ "$SH_CHANGE" != "$PS_CHANGE" ]; then
        echo "  [FAIL] 64-parity-restored-change  the twins disagree about watching changes on ONE host: .sh=$SH_CHANGE .ps1=$PS_CHANGE   $RG_AT"
        fails=$((fails + 1))
    elif [ "$SH_CHANGE" != watched ]; then
        na 64-parity-restored-change "neither twin watches changes mid-run on this host; their untouched controls were .sh=$r58 .ps1=$r61"
    elif [ "$r58$r61" = 00 ] && [ "$r59$r62$r60$r63" = 3333 ]; then
        echo "  [ok  ] 64-parity-restored-change  untouched 0/0; a cp -p restore 3/3 and a Copy-Item restore 3/3   $RG_AT"
    else
        echo "  [FAIL] 64-parity-restored-change  untouched .sh=$r58 .ps1=$r61 (0); cp -p .sh=$r59 .ps1=$r62 (3); Copy-Item .sh=$r60 .ps1=$r63 (3)   $RG_AT"
        fails=$((fails + 1))
    fi
else
    na 61-ps1-untouched-control          "$PS_ABSENT_WHY"
    na 62-ps1-cp-p-restore-refuses       "$PS_ABSENT_WHY"
    na 63-ps1-copy-item-restore-refuses  "$PS_ABSENT_WHY"
    na 64-parity-restored-change         "$PS_ABSENT_WHY -- arms 58 and 59 still prove the .sh twin, but the twins were NOT compared"
fi

# ═══ THE TENTH SUBJECT: A RELATIVE BUILD DIRECTORY IN ANOTHER PROCESS'S TREE ═══
# [[D-SCRIPT-RUN-GATE-RESOLVES-ANOTHER-PROCESS-RELATIVE-BUILD-DIR-AGAINST-ITS-OWN-CWD]]
# ✔MEASURED 2026-09-15 (P66, lane `pg`) on WSL: a leg in one clone was refused exit 4
# because a leg in another clone ran `ctest --test-dir build/dbg`, a relative token the
# scan resolved against its OWN directory.
# ★ A contender ctest standing in tree X names `bd-rel`; a gate standing in tree Y names
#   `bd-rel` too. Where this host can read another process's working directory the gate
#   must NOT refuse (different trees); where it cannot (Windows), the refusal is the
#   stated limitation and must SAY that the resolution was assumed. The CONTROL puts the
#   contender in tree Y itself, which every host must refuse -- and, where the directory
#   is readable, without the caveat.
REL_X="$SCRATCH/rel-tree-x"
REL_Y="$SCRATCH/rel-tree-y"
mkdir -p "$REL_X/bd-rel" "$REL_Y/bd-rel"
start_relative_contender() {  # <label> <tree> -> CONTENDER_BG
    rm -rf "${STAND_IN_STATE:?}/$1"
    mkdir -p "$STAND_IN_STATE/$1"
    STAND_IN_PLANTED="$STAND_IN_PLANTED $1"
    # ★ TWO TESTS, SELECTED BY NAME: the contender runs `held`, every gate arm runs `fast`.
    #   A gate that fails to refuse then finishes in about a second, rc 0 -- a fast red
    #   naming the missed contender -- instead of running the contender's held test until
    #   RG_STAND_IN_LEAK_BOUND_S. ✔MEASURED 2026-09-15 on WSL with one shared `held` test:
    #   the unrefused arms 69/70 took 600.30 s and 601.81 s, and a second `__stand-in body`
    #   of one label re-records that label's pid file.
    {
        printf 'add_test(held "%s" "%s" "__stand-in" "body" "%s")\n' "$BASH_W" "$SELF_SCRIPT_W" "$1"
        printf 'add_test(fast "%s" "-c" "echo ok")\n' "$BASH_W"
    } > "$2/bd-rel/CTestTestfile.cmake"
    ( cd "$2" && exec ctest --test-dir bd-rel -R held ) > "$SCRATCH/$1.contender.log" 2>&1 &
    CONTENDER_BG=$!
    stand_in_wait present "$1" && return 0
    precondition_failed "$1" "the relative contender's held test never appeared within the ${RG_HANG_GUARD_S}s hang guard"
    return 1
}
# Can THIS host read another process's working directory? Asked of the live contender,
# by the same two readers the subject uses -- measured, not looked up.
# ⚠ EITHER DIRECTORY IS THE CONTENDER'S. ✔MEASURED 2026-09-15 on WSL: ctest enters its
#   --test-dir after parsing, so its cwd reads `<tree>/bd-rel`, not `<tree>`; this probe
#   first compared against `<tree>` alone and reported CANNOT on a host that can.
contender_cwd_readable() {  # <expected tree>
    local want got=""
    want="$(cd "$1" && pwd -P)"
    run_gate_host_is_windows && return 1
    got="$(readlink "/proc/$CONTENDER_BG/cwd" 2>/dev/null)"
    [ -n "$got" ] || got="$(lsof -a -p "$CONTENDER_BG" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p' | head -1)"
    [ "$got" = "$want" ] || [ "$got" = "$want/bd-rel" ]
}
relative_gate_arm() {  # <label> <twin> <want> <log basename>
    if [ "$2" = sh ]; then
        arm "$1" "$3" bash -c 'cd "$1" && exec bash "$2" "$3" "100% tests passed" ctest --test-dir bd-rel -R fast' \
            _ "$REL_Y" "$GATE_SH" "$SCRATCH/$4"
    else
        arm "$1" "$3" bash -c 'cd "$1" && exec "$2" -NoProfile -ExecutionPolicy Bypass -File "$3" "$4" "100% tests passed" ctest --test-dir bd-rel -R fast' \
            _ "$REL_Y" "$PS_EXE" "$GATE_PS1_W" "$SCRATCH_W/$4"
    fi
}
printf 'add_test(fast "%s" "-c" "echo ok")\n' "$BASH_W" > "$REL_Y/bd-rel/CTestTestfile.cmake"
# ⓘ A BRACE GROUP FOR LAYOUT ONLY: the contender's held test is plain bash, so unlike the
#   compiler arms these need no stand-in image and run on every host.
{
    LX=65-relative-contender-in-tree-x
    if start_relative_contender "$LX" "$REL_X"; then
        if contender_cwd_readable "$REL_X"; then REL_READABLE=1; else REL_READABLE=0; fi
        ran; rg_at
        echo "  [ok  ] 65-probe-cwd-readable   this host $( [ "$REL_READABLE" = 1 ] && echo CAN || echo CANNOT ) read another process's working directory (asked of the live contender)   $RG_AT"
        if [ "$REL_READABLE" = 1 ]; then want_other=0; else want_other=4; fi
        relative_gate_arm 66-sh-relative-contender-other-tree sh "$want_other" a66.log
        if [ -n "$PS_EXE" ]; then
            relative_gate_arm 67-ps1-relative-contender-other-tree ps1 "$want_other" a67.log
        else
            na 67-ps1-relative-contender-other-tree "$PS_ABSENT_WHY"
        fi
        if [ "$REL_READABLE" = 0 ]; then
            says 66-sh-relative-contender-other-tree 'could not be read here'
            [ -n "$PS_EXE" ] && says 67-ps1-relative-contender-other-tree 'could not be read here'
        fi
        stand_in_stop "$LX" >/dev/null 2>&1
        wait "$CONTENDER_BG" 2>/dev/null
    fi
    printf 'add_test(fast "%s" "-c" "echo ok")\n' "$BASH_W" > "$REL_Y/bd-rel/CTestTestfile.cmake"
    LY=68-relative-contender-in-tree-y
    if start_relative_contender "$LY" "$REL_Y"; then
        relative_gate_arm 69-sh-relative-contender-same-tree sh 4 a69.log
        says 69-sh-relative-contender-same-tree 'ANOTHER RUN IS LIVE IN THIS BUILD DIRECTORY'
        if [ -n "$PS_EXE" ]; then
            relative_gate_arm 70-ps1-relative-contender-same-tree ps1 4 a70.log
            says 70-ps1-relative-contender-same-tree 'ANOTHER RUN IS LIVE IN THIS BUILD DIRECTORY'
        else
            na 70-ps1-relative-contender-same-tree "$PS_ABSENT_WHY"
        fi
        if [ "${REL_READABLE:-0}" = 1 ]; then
            says_not 69-sh-relative-contender-same-tree 'could not be read here'
            [ -n "$PS_EXE" ] && says_not 70-ps1-relative-contender-same-tree 'could not be read here'
        fi
        stand_in_stop "$LY" >/dev/null 2>&1
        wait "$CONTENDER_BG" 2>/dev/null
    fi
    if [ -n "$PS_EXE" ]; then
        ran
        rc_of 66-sh-relative-contender-other-tree;  r66="$RG_RC"
        rc_of 67-ps1-relative-contender-other-tree; r67="$RG_RC"
        rc_of 69-sh-relative-contender-same-tree;   r69="$RG_RC"
        rc_of 70-ps1-relative-contender-same-tree;  r70="$RG_RC"
        rg_at
        if [ "$r66" = "$r67" ] && [ "$r69$r70" = 44 ] && [ "$r66" = "${want_other:-x}" ]; then
            echo "  [ok  ] 71-parity-relative-contender  other tree $r66/$r67 (want ${want_other:-?}), same tree 4/4   $RG_AT"
        else
            echo "  [FAIL] 71-parity-relative-contender  other tree .sh=$r66 .ps1=$r67 (want ${want_other:-?}); same tree .sh=$r69 .ps1=$r70 (want 4)   $RG_AT"
            fails=$((fails + 1))
        fi
    else
        na 71-parity-relative-contender "$PS_ABSENT_WHY -- the twins were NOT compared"
    fi
}

# ═══ THE ELEVENTH SUBJECT: A PROCESS TABLE THAT FILLS A PIPE ════════════════════
# [[D-SCRIPT-RUN-GATE-PRE-RUN-SCAN-DEADLOCKS-ON-A-HERE-DOCUMENT-SIZED-BY-THE-PROCESS-TABLE]]
# ✔MEASURED 2026-09-15 (P66, lane `pg`), Git Bash / bash 5.3.15: a here-document body of
# 65422 or 65858 bytes reads, and 65656 bytes DEADLOCKS the shell that expands it. The
# pre-run scan fed its classification of the live process table through exactly such a
# document, and ✔REPRODUCED end to end: 60 foreign compiler rows sized to ~65580 bytes
# WEDGED run-gate.sh with a 0-byte log and 0.05 CPU-s -- the signature of the arm that sat
# idle for 876 s in a busy MSVC gate.
# ★ The subject's own table reader (`powershell`) is SHIMMED, first on PATH, to print that
#   table, and the gate gets the hang guard every waiting arm here has: still running past
#   it is a WEDGE, named, and stopped by its pid.
# ⓘ Red-capable only where bash deadlocks in that band (MSYS); on a POSIX host the
#   subject reads its table with `ps`, the band was not reproduced (WSL bash 5.2.21 read
#   every size), and the arm is NOT APPLICABLE.
if run_gate_host_is_windows; then
    SHIM="$SCRATCH/shim-table"
    mkdir -p "$SHIM"
    : > "$SHIM/table.txt"
    _rg_pad="$(head -c 1050 /dev/zero | tr '\0' 'x')"
    _rg_i=0
    while [ "$_rg_i" -lt 60 ]; do
        _rg_i=$((_rg_i + 1))
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$((900000001 + 2 * _rg_i))" 900000000 dsscp.exe "C:/fake/dsscp.exe $_rg_pad" dsscp.exe 20260915000000000000 >> "$SHIM/table.txt"
    done
    printf '#!/usr/bin/env bash\ncat "%s"\n' "$SHIM/table.txt" > "$SHIM/powershell"
    chmod +x "$SHIM/powershell"
    _rg_band="$(awk -F'\t' '{ n += length("FOREIGN\t" $1 "\tdsscp\t" $4) + 1 } END { print n }' "$SHIM/table.txt")"
    ran
    _rg_t0=$SECONDS
    ( PATH="$SHIM:$PATH" exec bash "$GATE_SH" "$SCRATCH/a72.log" 'BAND-OK' bash -c 'printf "%s-%s\n" BAND OK' ) > "$SCRATCH/72.out" 2>&1 &
    _rg_gbg=$!
    _rg_wedged=0
    while kill -0 "$_rg_gbg" 2>/dev/null; do
        if [ $((SECONDS - _rg_t0)) -ge "$RG_HANG_GUARD_S" ]; then _rg_wedged=1; break; fi
        sleep 0.2   # the poll cadence of a hang-guarded wait: no outcome depends on its length
    done
    rg_at
    if [ "$_rg_wedged" = 1 ]; then
        kill -9 "$_rg_gbg" 2>/dev/null
        wait "$_rg_gbg" 2>/dev/null
        echo "  [FAIL] 72-sh-process-table-in-the-pipe-band   WEDGED: still running ${RG_HANG_GUARD_S}s after it started over a ~${_rg_band}-byte classification (log $(wc -c < "$SCRATCH/a72.log" 2>/dev/null || echo '?') bytes); stopped by its pid   $RG_AT"
        fails=$((fails + 1))
    else
        wait "$_rg_gbg"; _rg_rc=$?
        if [ "$_rg_rc" = 0 ]; then
            echo "  [ok  ] 72-sh-process-table-in-the-pipe-band   completed rc=0 in $((SECONDS - _rg_t0)) s over a ~${_rg_band}-byte classification   $RG_AT"
        else
            echo "  [FAIL] 72-sh-process-table-in-the-pipe-band   rc=$_rg_rc (want 0) over a ~${_rg_band}-byte classification   $RG_AT"
            logtext "$SCRATCH/72.out" | sed 's/^/         /' | head -12
            fails=$((fails + 1))
        fi
        says a72.log 'compilers: 60' --log
    fi
else
    na 72-sh-process-table-in-the-pipe-band "the deadlock band is a property of MSYS pipes, and on this host run-gate.sh reads its process table with ps"
fi

# ---- WHAT THIS RUN ACTUALLY PROVED -----------------------------------------
# The count is printed on EVERY host, green or not. A reader who sees only
# "0 failure(s)" cannot tell a run that proved both twins from one that proved
# half of them, and that is the whole reason the not-applicable arms are counted
# rather than skipped.
rg_at
echo "run-gate evidence-integrity proof: $arms_ran arm(s) ran, $arms_na not applicable on this host, $fails failure(s), $preconditions_failed of them FIXTURE PRECONDITIONS (not verdicts on run-gate); finished $RG_AT"
if [ "$arms_na" -gt 0 ]; then
    echo "  ! NOT PROVED HERE: every arm marked n/a above. Where that is the .ps1 twin, TWIN"
    echo "    PARITY is not proved here, and a green run is evidence about run-gate.sh ALONE."
    echo "    Not-applicable is not a failure and this fixture still exits on failures only."
else
    echo "  both twins were driven on this host; twin parity is proved here."
fi
exit $fails
