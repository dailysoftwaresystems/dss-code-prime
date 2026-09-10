#!/usr/bin/env bash
# test-run-gate.sh -- prove BOTH run-gate twins refuse a run whose evidence is
# spoiled, and that they still pass a run whose evidence is intact.
#
# FOUR SUBJECTS, one fixture, because they are one contract:
#   * the SOURCE TREE moving under the run          -> exit 3
#   * ANOTHER RUN live in the same BUILD DIRECTORY  -> exit 4
#   * WHICH TREE those roots are read from at all   -> 3 or 0, and which one it
#     was must be readable from the log ALONE
#   * whether a file's TIMESTAMP can decide either answer -> it must not, in
#     EITHER direction, because one carriage's clock is not monotonic
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
#
# ! THE CONTROLS ARE THE POINT. Arms 1, 4, 7, 9, 14 and 16 must PASS: without
#   them, a green refusal arm is equally consistent with "this wrapper now
#   refuses everything".
# ! The mutations are REAL -- a real edit to a real read-at-test-time root while
#   the gate command runs, and a real second `ctest` alive in the same build
#   directory -- rather than simulations with a pre-dated marker or a fake
#   process table.
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
#     and left escapable it would silently drop all NINE .ps1 arms on the one host
#     category where twin parity is actually proved, while reporting green for
#     doing less work.
#   ★ THE .sh ARMS ARE NEVER ESCAPABLE. A host with no PowerShell still proves the
#     .sh twin: arms 1, 2, 3, 7, 8, 12, 13, 14, 18 and 19 run everywhere,
#     unconditionally.
#
# ⚠⚠ THE REACH OF THIS GUARD -- WHERE TWIN PARITY IS ACTUALLY PROVED, AND WHERE IT
#   IS NOT. ✔MEASURED BY EXECUTION 2026-09-08 on all four hosts this project gates
#   on, by running each candidate and requiring the token back:
#       Windows (Git Bash)   pwsh 7 AND powershell 5.1 present  -> .ps1 arms RUN
#       WSL x86_64           /usr/bin/pwsh 7.5.4                -> .ps1 arms RUN
#       macOS arm64          /usr/local/bin/pwsh                -> .ps1 arms RUN
#       arm64 VPS (ubuntu)   NEITHER spelling present           -> .ps1 arms N/A
#   ⇒ on the arm64 VPS leg, arms 4, 5, 9, 10, 15, 16, 20 and 21 -- and with them
#     ALL FOUR parity arms, 6, 11, 17 and 22 -- are not proved, and that is
#     PERMANENT rather than pending:
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

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
GATE_SH="${ROOT}/scripts/run-gate/run-gate.sh"
GATE_PS1="${ROOT}/scripts/run-gate/run-gate.ps1"
SCRATCH="${ROOT}/.temp/test-run-gate-scratch"
SANDBOX="${SCRATCH}/tree"
rm -rf "$SCRATCH"
mkdir -p "$SANDBOX/examples" "$SANDBOX/src/dss-config" "$SANDBOX/tests/corpus"
PROBE_REL="examples/.test-run-gate-input-probe.txt"
NOWHERE_BIN="${SCRATCH}/no-interpreter-here"
mkdir -p "$NOWHERE_BIN"
fails=0
arms_ran=0
arms_na=0

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

ran() { arms_ran=$((arms_ran + 1)); }

# An arm this host CANNOT run. Named, with its reason, and counted -- the three
# things a silent skip omits.
na() {  # <label> <reason>
    arms_na=$((arms_na + 1))
    echo "  [n/a ] $1   NOT APPLICABLE ON THIS HOST: $2"
}

# A synthetic ctest tree: a `CTestTestfile.cmake` alone is enough for ctest to
# run -- ✔MEASURED, no configure and no project needed. `slow` exists so a
# contender can still be alive while the arm under test starts.
# ⚠ THE INTERPRETER IS `$BASH` -- THE ONE ALREADY RUNNING THIS FIXTURE -- NOT A
#   HARD-CODED `/usr/bin/bash`. ✔MEASURED 2026-09-08 on the macOS carriage:
#   `/usr/bin/bash` DOES NOT EXIST there (only `/bin/bash`), so the synthetic
#   `add_test` below named a program that could not be launched, every arm from
#   `7-sh-alone` on lost its success witness, and the guard would have red on
#   that leg naming the SUBJECT for a defect in the fixture's own test tree.
#   ⓘ `$BASH` needs no probe and cannot lie: it is the absolute path of the shell
#     executing this line. `cygpath -m` converts it for CMake only on MSYS, where
#     a `/usr/bin/...` path is not something `ctest` can spawn.
BASH_W="$BASH"
if command -v cygpath >/dev/null 2>&1; then BASH_W="$(cygpath -m "$BASH")"; fi

# ── A STAND-IN COMPILER, AND IT IS A COPY OF `sleep` RATHER THAN THE REAL ONE ─
#
# The subject matches on the process IMAGE NAME, so a copy of any long-lived
# program named `dsscp` is exactly as good a subject as the compiler and costs
# no build. ⚠ Running the REAL `dsscp` here would be actively wrong: it writes
# to the per-user cache this whole subject is about, so the fixture would
# perturb the machine it is measuring.
# ⓘ `.exe` on MSYS because that is the name Win32 reports and the matcher
# strips; a bare name elsewhere. `$STUB` is empty when no `sleep` binary can be
# copied, and the arms below then report NOT APPLICABLE rather than skipping.
STUB=""
STUB_W=""
if _rg_sleep="$(command -v sleep 2>/dev/null)" && [ -x "$_rg_sleep" ]; then
    mkdir -p "$SCRATCH/bin"
    if run_gate_host_is_windows; then STUB="$SCRATCH/bin/dsscp.exe"; else STUB="$SCRATCH/bin/dsscp"; fi
    if cp "$_rg_sleep" "$STUB" 2>/dev/null; then
        chmod +x "$STUB" 2>/dev/null || true
        STUB_W="$STUB"
        if command -v cygpath >/dev/null 2>&1; then STUB_W="$(cygpath -m "$STUB")"; fi
    else
        STUB=""
    fi
fi
STUB_ABSENT_WHY="no 'sleep' binary could be copied to a stand-in named after the compiler, so no live-compiler subject could be created on this host"

# A stand-in compiler with NO RESOLVABLE ANCESTRY — the shape a compiler started
# from another terminal has, as seen from here.
# ⚠ THE ORPHANING IS THE FIXTURE, NOT AN ACCIDENT. The inner shell exits
# immediately, so the stub's recorded parent is gone before the scan runs and
# the subject's upward walk stops at a pid that is not in the table. ✔MEASURED
# on MSYS: that is also what a background job started from ANY shell looks like
# here, because MSYS's fork/exec emulation leaves a transient parent behind.
plant_foreign_compiler() {  # <seconds>
    bash -c "nohup '$STUB' $1 >/dev/null 2>&1 &"
}
mk_ctest_dir() {  # <dir> <fast|slow>
    mkdir -p "$1"
    if [ "$2" = slow ]; then
        # 12 s: the contender only has to outlive the 3 s settle plus the gate's
        # ✔MEASURED ~1.6 s of scanning, and this fixture is a ctest entry whose
        # wall clock is paid on every leg.
        printf 'add_test(slow "%s" "-c" "sleep 12; echo ok")\n' "$BASH_W" > "$1/CTestTestfile.cmake"
    else
        printf 'add_test(fast "%s" "-c" "echo ok")\n' "$BASH_W" > "$1/CTestTestfile.cmake"
    fi
}

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
    "$@" > "$SCRATCH/$label.out" 2>&1
    local rc=$?
    echo "$rc" > "$SCRATCH/$label.rc"
    if [ "$rc" -eq "$want" ]; then
        echo "  [ok  ] $label   rc=$rc (want $want)"
    else
        echo "  [FAIL] $label   rc=$rc (want $want)"
        logtext "$SCRATCH/$label.out" | sed 's/^/         /' | head -12
        fails=$((fails + 1))
    fi
}

says() {  # <label-or-file> <needle> [--log]
    local f="$SCRATCH/$1.out"
    [ "${3:-}" = "--log" ] && f="$SCRATCH/$1"
    if logtext "$f" | grep -qF "$2"; then
        echo "  [ok  ] $1   says: $2"
    else
        echo "  [FAIL] $1   did NOT say: $2"
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
        fails=$((fails + 1))
    else
        echo "  [ok  ] $1   does not say: $2"
    fi
}

echo "run-gate evidence-integrity proof (sandbox: $SANDBOX)"

# ---- ARM 0 THE ESCAPE MUST BE DIRECTIONAL, AND IT IS MEASURED HERE ----------
# The SAME probe, run where PATH reaches an EMPTY directory and nothing else,
# must report ABSENT. Without this arm, a probe that had degenerated to "always
# absent" would declare every .ps1 arm not-applicable on every host and still
# print a green run -- the exact shape of an escape that refuses nothing.
# ⓘ A subshell, not a `VAR= func` prefix: bash keeps such an assignment in the
#   caller's environment for a FUNCTION, which would break every later arm.
ran
neg_out=$( PATH="$NOWHERE_BIN"; run_gate_powershell ); neg_rc=$?
if [ "$neg_rc" -eq 0 ]; then
    echo "  [FAIL] 0-probe-negative   probe claimed PowerShell '$neg_out' with no interpreter on PATH"
    fails=$((fails + 1))
else
    echo "  [ok  ] 0-probe-negative   no interpreter reachable -> probe reports ABSENT (the escape is directional)"
fi

# ---- ARM 0b WHICH INTERPRETER THIS HOST ACTUALLY HAS ------------------------
# ★★ ON WINDOWS THE ESCAPE IS NOT AVAILABLE AT ALL, AND THAT IS THE OTHER HALF OF
#   MAKING IT DIRECTIONAL. Windows ships PowerShell 5.1 with the OS and
#   `CMakeLists.txt` already REFUSES TO CONFIGURE without one
#   (`find_program(POWERSHELL_EXE NAMES pwsh powershell REQUIRED)`), so a Windows
#   host that answers "no PowerShell" has a broken environment, not a legitimate
#   absence. Left escapable, a `ctest` launched with a stripped PATH would drop
#   all nine .ps1 arms on the ONE host category where every carriage proves twin
#   parity, and report a green run for doing less work. It fails loud instead.
ran
if PS_EXE=$(run_gate_powershell); then
    echo "  [ok  ] 0-probe-positive   .ps1 arms will run under '$PS_EXE' (probed BY EXECUTION, not by lookup)"
elif run_gate_host_is_windows; then
    PS_EXE=""
    echo "  [FAIL] 0-probe-positive   THIS IS WINDOWS and none of '$PS_CANDIDATES' ran: PowerShell ships with"
    echo "         the OS and CMake already requires one, so this is a broken PATH, not a host without"
    echo "         PowerShell. The .ps1 arms are NOT escapable here. Put pwsh or powershell on PATH."
    fails=$((fails + 1))
else
    PS_EXE=""
    echo "  [ok  ] 0-probe-positive   no working PowerShell here: none of '$PS_CANDIDATES' returned '$PS_PROBE_TOKEN' with rc 0"
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

# ---- ARM 2 (sh) THE DEFECT: an input root is edited DURING the run ----------
rm -f "$PROBE_REL"
arm 2-sh-moved 3 bash "$GATE_SH" "$SCRATCH/a2.log" 'HELLO' \
    bash -c "sleep 1; echo touched > $PROBE_REL; echo HELLO"
says 2-sh-moved 'the tree CHANGED UNDER THE RUN'
says 2-sh-moved 'test-run-gate-input-probe'
says 2-sh-moved 'This is NOT'
rm -f "$PROBE_REL"

# ---- ARM 3 (sh) ORDERING: rc=0 AND the witness present is NOT enough --------
# Arm 2's command exits 0 and prints the witness, so a wrapper that checked
# either one first would have PASSED it. This names that ordering.
ran
if logtext "$SCRATCH/2-sh-moved.out" | grep -q 'run-gate.sh: OK'; then
    echo "  [FAIL] 3-sh-order   the moved-input run reported OK"
    fails=$((fails + 1))
else
    echo "  [ok  ] 3-sh-order   a witness-matching rc=0 run was still refused"
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
        "Start-Sleep -Seconds 2; Set-Content -Path '$PROBE_REL' -Value touched; Write-Output HELLO"
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
    if [ "$sh_rc" = "$ps_rc" ] && [ "$sh_rc" = "3" ]; then
        echo "  [ok  ] 6-parity   both twins refused the moved tree with exit $sh_rc"
    else
        echo "  [FAIL] 6-parity   .sh returned $sh_rc, .ps1 returned $ps_rc (both must be 3)"
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
mk_ctest_dir "$SANDBOX/bd-alone" fast
mk_ctest_dir "$SANDBOX/bd-shared" slow

# ---- ARM 7 (sh) CONTROL: alone in the build directory -> still passes -------
arm 7-sh-alone 0 bash "$GATE_SH" "$SCRATCH/a7.log" '100% tests passed' \
    ctest --test-dir "$SANDBOX/bd-alone"
says 7-sh-alone 'run-gate.sh: OK'
says a7.log 'contended: no' --log

# ---- ARM 8 (sh) THE DEFECT: a second ctest is ALREADY live in it -----------
ctest --test-dir "$SANDBOX/bd-shared" > "$SCRATCH/bg8.log" 2>&1 &
bg8=$!
sleep 3
arm 8-sh-contended 4 bash "$GATE_SH" "$SCRATCH/a8.log" '100% tests passed' \
    ctest --test-dir "$SANDBOX/bd-shared"
says 8-sh-contended 'ANOTHER RUN IS LIVE IN THIS BUILD DIRECTORY'
says 8-sh-contended 'bd-shared'
says 8-sh-contended 'This is NOT'
wait $bg8 2>/dev/null

if [ -n "$PS_EXE" ]; then
    # ---- ARM 9 (ps1) CONTROL ------------------------------------------------
    arm 9-ps1-alone 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a9.log" '100% tests passed' \
        ctest --test-dir "$SANDBOX/bd-alone"
    says 9-ps1-alone 'run-gate.ps1: OK'
    says a9.log 'contended: no' --log

    # ---- ARM 10 (ps1) THE DEFECT --------------------------------------------
    ctest --test-dir "$SANDBOX/bd-shared" > "$SCRATCH/bg10.log" 2>&1 &
    bg10=$!
    sleep 3
    arm 10-ps1-contended 4 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a10.log" '100% tests passed' \
        ctest --test-dir "$SANDBOX/bd-shared"
    says 10-ps1-contended 'ANOTHER RUN IS LIVE IN THIS BUILD DIRECTORY'
    says 10-ps1-contended 'bd-shared'
    wait $bg10 2>/dev/null

    # ---- ARM 11 TWIN PARITY ON THE SECOND SUBJECT ---------------------------
    ran
    sh_rc=$(cat "$SCRATCH/8-sh-contended.rc"); ps_rc=$(cat "$SCRATCH/10-ps1-contended.rc")
    if [ "$sh_rc" = "$ps_rc" ] && [ "$sh_rc" = "4" ]; then
        echo "  [ok  ] 11-parity  both twins refused the shared build directory with exit $sh_rc"
    else
        echo "  [FAIL] 11-parity  .sh returned $sh_rc, .ps1 returned $ps_rc (both must be 4)"
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
moved_rc=$(cat "$SCRATCH/2-sh-moved.rc"); cont_rc=$(cat "$SCRATCH/8-sh-contended.rc")
if [ "$moved_rc" != "$cont_rc" ] \
   && ! logtext "$SCRATCH/8-sh-contended.out" | grep -q 'CHANGED UNDER THE RUN' \
   && ! logtext "$SCRATCH/2-sh-moved.out"     | grep -q 'ANOTHER RUN IS LIVE'; then
    echo "  [ok  ] 12-distinct  moved-tree ($moved_rc) and contended-build-dir ($cont_rc) are distinct refusals"
else
    echo "  [FAIL] 12-distinct  the two refusals are not tellable apart (rc $moved_rc vs $cont_rc)"
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
mk_cmake_build_dir "$TREE_A/build/own" "$TREE_A" \
    "sleep 1; echo touched > '$TREE_A/examples/.probe-own-tree.txt'; echo ok"
mk_cmake_build_dir "$TREE_A/build/foreign" "$TREE_A" \
    "sleep 1; echo touched > '$SANDBOX/examples/.probe-foreign-tree.txt'; echo ok"

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
    if [ "$own_sh" = "3" ] && [ "$own_ps" = "3" ] && [ "$fgn_sh" = "0" ] && [ "$fgn_ps" = "0" ]; then
        echo "  [ok  ] 17-parity  both twins read their roots from the gate command's tree (own moved 3/3, foreign moved 0/0)"
    else
        echo "  [FAIL] 17-parity  own tree moved: .sh=$own_sh .ps1=$own_ps (both must be 3); foreign tree moved: .sh=$fgn_sh .ps1=$fgn_ps (both must be 0)"
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
    "sleep 1; echo touched > '$BACKDATED'; touch -t 200001010000 '$BACKDATED'; echo ok"
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
    if [ "$bd_sh" = "3" ] && [ "$bd_ps" = "3" ] && [ "$by_sh" = "0" ] && [ "$by_ps" = "0" ]; then
        echo "  [ok  ] 22-parity  neither twin decides by TIMESTAMP ORDER (backdated change 3/3, future-stamped bystander 0/0)"
    else
        echo "  [FAIL] 22-parity  backdated change: .sh=$bd_sh .ps1=$bd_ps (both must be 3); future-stamped bystander: .sh=$by_sh .ps1=$by_ps (both must be 0)"
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
# ★★★ THE THREE ARMS ARE A SET AND NONE OF THEM MEANS ANYTHING ALONE.
#   · the DESCENDANT arm proves the check is not "any live dsscp", which would
#     fire on every gate this project runs (its own ctest spawns hundreds);
#   · the FOREIGN arm proves it is not permanently silent — and it is also the
#     descendant arm's control, because a matcher that recognised nothing would
#     satisfy the descendant arm perfectly;
#   · the CONTROL arm (arms 1 and 7 already carry the line) proves it says
#     "none" when there is nothing to say.
# Run in that order so the foreign arm's orphan cannot be alive during the
# descendant arm.

if [ -n "$STUB" ]; then
    # ---- ARM 23 (sh) A DESCENDANT COMPILER MUST NOT BE REPORTED -------------
    # The fixture is a REAL ctest running two tests concurrently: the stub, and
    # a nested run-gate. Both are children of that ctest, so the stub is inside
    # the gate's own process tree through a BUILD-TOOL ancestor — the one case
    # the bounded "own" set exists to cover, and the shape a guard registered as
    # a ctest test really has in this repository.
    # ⚠ IT CANNOT BE BUILT AS "the gate command backgrounds a child": the scans
    # run BEFORE and AFTER the command, so such a child is either not yet born
    # or already orphaned. ✔MEASURED — an orphaned child reads as FOREIGN, which
    # is the fail-toward-reporting direction and is what arm 24 relies on.
    mkdir -p "$SANDBOX/bd-nested"
    {
        printf 'add_test(busy "%s" "8")\n' "$STUB_W"
        printf 'add_test(gate "%s" "%s" "%s" "HELLO" "%s" "-c" "echo HELLO")\n' \
            "$BASH_W" "$(gate_spelling "$ROOT")/scripts/run-gate/run-gate.sh" \
            "$SCRATCH/a23.log" "$BASH_W"
    } > "$SANDBOX/bd-nested/CTestTestfile.cmake"
    arm 23-sh-descendant-compiler 0 ctest --test-dir "$SANDBOX/bd-nested" -j 2
    # The ctest verdict IS this arm's precondition: `busy` must actually have
    # run, or the nested gate looked at a machine with no stub on it and the
    # assertion below holds vacuously.
    # ⓘ AND IT MUST HAVE OUTLIVED THE GATE, which is why the stub sleeps 8 s:
    # ✔MEASURED on this host the nested gate finished in 5.10 s (two process
    # table reads dominate it) while `busy` ran the full 8.04 s, so the stub was
    # live across BOTH of the gate's scans. A stub that exited first would make
    # this arm agree with a broken subject.
    says 23-sh-descendant-compiler 'tests failed out of 2'
    says a23.log "compilers: none outside" --log

    # ---- ARM 24 (sh) THE DEFECT: A COMPILER OUTSIDE THIS GATE'S TREE --------
    plant_foreign_compiler 12
    arm 24-sh-foreign-compiler 0 bash "$GATE_SH" "$SCRATCH/a24.log" 'HELLO' \
        bash -c 'echo HELLO'
    says 24-sh-foreign-compiler 'run-gate.sh: OK'
    says_not a24.log 'compilers: none' --log
    says a24.log "OUTSIDE this gate's process tree" --log
    says a24.log 'dsscp' --log
    # ⚠ AND IT MUST NOT HAVE BECOME A REFUSAL. The line is an OBSERVATION: the
    # mechanism that made a second compiler destroy a verdict is gone, and
    # refusing here would refuse every gate of a project that runs four lanes in
    # parallel by design. rc 0 above is that claim; this is it said out loud.
    says_not 24-sh-foreign-compiler 'FAIL'
else
    na 23-sh-descendant-compiler "$STUB_ABSENT_WHY"
    na 24-sh-foreign-compiler    "$STUB_ABSENT_WHY"
fi

if [ -n "$PS_EXE" ] && [ -n "$STUB" ]; then
    # ---- ARM 25 (ps1) THE DEFECT -------------------------------------------
    plant_foreign_compiler 12
    arm 25-ps1-foreign-compiler 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a25.log" 'HELLO' \
        "$PS_EXE" -NoProfile -Command "Write-Output HELLO"
    says 25-ps1-foreign-compiler 'run-gate.ps1: OK'
    says_not a25.log 'compilers: none' --log
    says a25.log "OUTSIDE this gate's process tree" --log

    # ---- ARM 26 (ps1) THE CONTROL, i.e. THE ESCAPE IS DIRECTIONAL ----------
    # ⚠ WITHOUT THIS THE TWIN COULD SIMPLY ALWAYS REPORT. Arm 25 alone is
    # satisfied by a line that fires unconditionally, which is the failure this
    # whole subject was written to avoid in the other direction.
    # ⓘ It waits for arm 25's orphan rather than asserting over it: 12 s was
    # chosen to outlive one gate and not two.
    sleep 13
    arm 26-ps1-no-compiler 0 "$PS_EXE" -NoProfile -ExecutionPolicy Bypass \
        -File "$GATE_PS1" "$SCRATCH/a26.log" 'HELLO' \
        "$PS_EXE" -NoProfile -Command "Write-Output HELLO"
    says a26.log "compilers: none outside" --log

    # ---- ARM 27 TWIN PARITY ON THE FIFTH SUBJECT ---------------------------
    ran
    sh_saw=$(logtext "$SCRATCH/a24.log" | grep -c "OUTSIDE this gate's process tree" || true)
    ps_saw=$(logtext "$SCRATCH/a25.log" | grep -c "OUTSIDE this gate's process tree" || true)
    sh_rc=$(cat "$SCRATCH/24-sh-foreign-compiler.rc")
    ps_rc=$(cat "$SCRATCH/25-ps1-foreign-compiler.rc")
    if [ "$sh_saw" -ge 1 ] && [ "$ps_saw" -ge 1 ] && [ "$sh_rc" = "0" ] && [ "$ps_rc" = "0" ]; then
        echo "  [ok  ] 27-parity  both twins REPORTED a foreign compiler and both still exited 0"
    else
        echo "  [FAIL] 27-parity  .sh reported=$sh_saw rc=$sh_rc, .ps1 reported=$ps_saw rc=$ps_rc (both must report, both must exit 0)"
        fails=$((fails + 1))
    fi
elif [ -z "$STUB" ]; then
    na 25-ps1-foreign-compiler "$STUB_ABSENT_WHY"
    na 26-ps1-no-compiler      "$STUB_ABSENT_WHY"
    na 27-parity               "$STUB_ABSENT_WHY -- the twins were NOT compared on the fifth subject"
else
    na 25-ps1-foreign-compiler "$PS_ABSENT_WHY -- the .ps1 twin was never shown a foreign compiler"
    na 26-ps1-no-compiler      "$PS_ABSENT_WHY -- the .ps1 twin's no-compiler CONTROL was not taken"
    na 27-parity               "$PS_ABSENT_WHY -- arms 23 and 24 still prove the .sh twin, but the twins were NOT compared"
fi

# ---- WHAT THIS RUN ACTUALLY PROVED -----------------------------------------
# The count is printed on EVERY host, green or not. A reader who sees only
# "0 failure(s)" cannot tell a run that proved both twins from one that proved
# half of them, and that is the whole reason the not-applicable arms are counted
# rather than skipped.
echo "run-gate evidence-integrity proof: $arms_ran arm(s) ran, $arms_na not applicable on this host, $fails failure(s)"
if [ "$arms_na" -gt 0 ]; then
    echo "  ! NOT PROVED HERE: the .ps1 twin, and therefore TWIN PARITY. This host has no"
    echo "    working PowerShell, so a green run above is evidence about run-gate.sh ALONE."
    echo "    Not-applicable is not a failure and this fixture still exits on failures only."
else
    echo "  both twins were driven on this host; twin parity is proved here."
fi
exit $fails
