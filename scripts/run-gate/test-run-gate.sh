#!/usr/bin/env bash
# test-run-gate.sh -- prove BOTH run-gate twins refuse a run whose evidence is
# spoiled, and that they still pass a run whose evidence is intact.
#
# TWO SUBJECTS, one fixture, because they are one contract:
#   * the SOURCE TREE moving under the run          -> exit 3
#   * ANOTHER RUN live in the same BUILD DIRECTORY  -> exit 4
#
# A SIBLING FILE, not a `--selftest` flag: run-gate's interface is POSITIONAL and
# its first argument is a LOG PATH, which it refuses when it begins with '-' (see
# the long block about that in run-gate.sh). A flag could not be spelled without
# colliding with that refusal, so the proof lives beside the subject instead --
# the same shape as scripts/lane-worktree/test-lane-worktree.sh.
#
# ! THE CONTROLS ARE THE POINT. Arms 1, 4, 7 and 9 must PASS: without them, a
#   green refusal arm is equally consistent with "this wrapper now refuses
#   everything".
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
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
GATE_SH="${ROOT}/scripts/run-gate/run-gate.sh"
GATE_PS1="${ROOT}/scripts/run-gate/run-gate.ps1"
SCRATCH="${ROOT}/.temp/test-run-gate-scratch"
SANDBOX="${SCRATCH}/tree"
rm -rf "$SCRATCH"
mkdir -p "$SANDBOX/examples" "$SANDBOX/src/dss-config" "$SANDBOX/tests/corpus"
PROBE_REL="examples/.test-run-gate-input-probe.txt"
fails=0

# A synthetic ctest tree: a `CTestTestfile.cmake` alone is enough for ctest to
# run -- ✔MEASURED, no configure and no project needed. `slow` exists so a
# contender can still be alive while the arm under test starts.
BASH_W="$(command -v cygpath >/dev/null 2>&1 && cygpath -m /usr/bin/bash || echo /usr/bin/bash)"
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

cd "$SANDBOX" || { echo "test-run-gate.sh: cannot enter the sandbox $SANDBOX"; exit 1; }

logtext() { tr -d '\000' < "$1" 2>/dev/null; }   # UTF-16LE or UTF-8, either way

arm() {  # <label> <want-rc> ...cmd
    local label=$1 want=$2; shift 2
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

echo "run-gate evidence-integrity proof (sandbox: $SANDBOX)"

# ---- ARM 1 (sh) CONTROL: inputs held still -> the wrapper still passes -------
rm -f "$PROBE_REL"
arm 1-sh-control 0 bash "$GATE_SH" "$SCRATCH/a1.log" 'HELLO' \
    bash -c 'echo HELLO'
says 1-sh-control 'run-gate.sh: OK'
says a1.log 'inputs  : held still' --log
# ...and a command that names no build directory must SAY it had no subject,
# rather than reporting a contention verdict it never took.
says a1.log 'builddir: none named by this command' --log

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
if logtext "$SCRATCH/2-sh-moved.out" | grep -q 'run-gate.sh: OK'; then
    echo "  [FAIL] 3-sh-order   the moved-input run reported OK"
    fails=$((fails + 1))
else
    echo "  [ok  ] 3-sh-order   a witness-matching rc=0 run was still refused"
fi

# ---- ARM 4 (ps1) CONTROL ----------------------------------------------------
rm -f "$PROBE_REL"
arm 4-ps1-control 0 powershell -NoProfile -ExecutionPolicy Bypass \
    -File "$GATE_PS1" "$SCRATCH/a4.log" 'HELLO' \
    powershell -NoProfile -Command "Write-Output HELLO"
says 4-ps1-control 'run-gate.ps1: OK'
says a4.log 'inputs  : held still' --log
says a4.log 'builddir: none named by this command' --log

# ---- ARM 5 (ps1) THE DEFECT -------------------------------------------------
rm -f "$PROBE_REL"
arm 5-ps1-moved 3 powershell -NoProfile -ExecutionPolicy Bypass \
    -File "$GATE_PS1" "$SCRATCH/a5.log" 'HELLO' \
    powershell -NoProfile -Command \
    "Start-Sleep -Seconds 2; Set-Content -Path '$PROBE_REL' -Value touched; Write-Output HELLO"
says 5-ps1-moved 'the tree CHANGED UNDER THE RUN'
says 5-ps1-moved 'test-run-gate-input-probe'
says 5-ps1-moved 'This is NOT'
rm -f "$PROBE_REL"

# ---- ARM 6 TWIN PARITY, ASSERTED FROM THE OBSERVED EXIT CODES ---------------
# ⚠ The first draft of this arm asserted parity from the EXISTENCE of the two
#   output files, which is true whenever the arms ran at all and says nothing
#   about what they decided. Compare the numbers the twins actually returned.
sh_rc=$(cat "$SCRATCH/2-sh-moved.rc"); ps_rc=$(cat "$SCRATCH/5-ps1-moved.rc")
if [ "$sh_rc" = "$ps_rc" ] && [ "$sh_rc" = "3" ]; then
    echo "  [ok  ] 6-parity   both twins refused the moved tree with exit $sh_rc"
else
    echo "  [FAIL] 6-parity   .sh returned $sh_rc, .ps1 returned $ps_rc (both must be 3)"
    fails=$((fails + 1))
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

# ---- ARM 9 (ps1) CONTROL ----------------------------------------------------
arm 9-ps1-alone 0 powershell -NoProfile -ExecutionPolicy Bypass \
    -File "$GATE_PS1" "$SCRATCH/a9.log" '100% tests passed' \
    ctest --test-dir "$SANDBOX/bd-alone"
says 9-ps1-alone 'run-gate.ps1: OK'
says a9.log 'contended: no' --log

# ---- ARM 10 (ps1) THE DEFECT ------------------------------------------------
ctest --test-dir "$SANDBOX/bd-shared" > "$SCRATCH/bg10.log" 2>&1 &
bg10=$!
sleep 3
arm 10-ps1-contended 4 powershell -NoProfile -ExecutionPolicy Bypass \
    -File "$GATE_PS1" "$SCRATCH/a10.log" '100% tests passed' \
    ctest --test-dir "$SANDBOX/bd-shared"
says 10-ps1-contended 'ANOTHER RUN IS LIVE IN THIS BUILD DIRECTORY'
says 10-ps1-contended 'bd-shared'
wait $bg10 2>/dev/null

# ---- ARM 11 TWIN PARITY ON THE SECOND SUBJECT ------------------------------
sh_rc=$(cat "$SCRATCH/8-sh-contended.rc"); ps_rc=$(cat "$SCRATCH/10-ps1-contended.rc")
if [ "$sh_rc" = "$ps_rc" ] && [ "$sh_rc" = "4" ]; then
    echo "  [ok  ] 11-parity  both twins refused the shared build directory with exit $sh_rc"
else
    echo "  [FAIL] 11-parity  .sh returned $sh_rc, .ps1 returned $ps_rc (both must be 4)"
    fails=$((fails + 1))
fi

# ---- ARM 12 THE TWO REFUSALS MUST STAY TELLABLE APART ----------------------
# ★ A reader who cannot tell 3 from 4 cannot tell whether to settle the tree or
#   wait for a sibling -- two different remedies. This asserts the codes differ
#   AND that neither refusal borrows the other's sentence.
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
echo "run-gate evidence-integrity proof: $fails failure(s)"
exit $fails
