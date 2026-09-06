#!/usr/bin/env bash
# test-run-gate.sh -- prove BOTH run-gate twins refuse a run whose inputs moved,
# and that they still pass a run whose inputs held still.
#
# A SIBLING FILE, not a `--selftest` flag: run-gate's interface is POSITIONAL and
# its first argument is a LOG PATH, which it refuses when it begins with '-' (see
# the long block about that in run-gate.sh). A flag could not be spelled without
# colliding with that refusal, so the proof lives beside the subject instead --
# the same shape as scripts/lane-worktree/test-lane-worktree.sh.
#
# ! THE CONTROL IS THE POINT. Arms 1 and 4 must PASS: without them, a green
#   refusal arm is equally consistent with "this wrapper now refuses everything".
# ! The mutation is a REAL edit to a REAL read-at-test-time root, applied WHILE
#   the gate command runs -- reproducing the P62 sequence rather than simulating
#   it with a pre-dated marker.
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
cd /c/Source/DailySoftware/dss-code-prime

SCRATCH=.temp/test-run-gate-scratch
rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"
PROBE_REL="examples/.test-run-gate-input-probe.txt"
fails=0

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

echo "run-gate input-stability proof"

# ---- ARM 1 (sh) CONTROL: inputs held still -> the wrapper still passes -------
rm -f "$PROBE_REL"
arm 1-sh-control 0 bash scripts/run-gate/run-gate.sh "$SCRATCH/a1.log" 'HELLO' \
    bash -c 'echo HELLO'
says 1-sh-control 'run-gate.sh: OK'
says a1.log 'inputs  : held still' --log

# ---- ARM 2 (sh) THE DEFECT: an input root is edited DURING the run ----------
rm -f "$PROBE_REL"
arm 2-sh-moved 3 bash scripts/run-gate/run-gate.sh "$SCRATCH/a2.log" 'HELLO' \
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
    -File scripts/run-gate/run-gate.ps1 "$SCRATCH/a4.log" 'HELLO' \
    powershell -NoProfile -Command "Write-Output HELLO"
says 4-ps1-control 'run-gate.ps1: OK'
says a4.log 'inputs  : held still' --log

# ---- ARM 5 (ps1) THE DEFECT -------------------------------------------------
rm -f "$PROBE_REL"
arm 5-ps1-moved 3 powershell -NoProfile -ExecutionPolicy Bypass \
    -File scripts/run-gate/run-gate.ps1 "$SCRATCH/a5.log" 'HELLO' \
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

rm -f "$PROBE_REL"
echo "run-gate input-stability proof: $fails failure(s)"
exit $fails
