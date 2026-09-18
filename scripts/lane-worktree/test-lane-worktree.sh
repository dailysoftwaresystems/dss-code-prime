#!/usr/bin/env bash
# The self-test for `lane-worktree.sh` AND `lane-worktree.ps1` — it proves that verb's
# `remove` cannot silently delete a lane's evidence or its uncommitted work, IN BOTH
# IMPLEMENTATIONS. No `PURPOSE:` line: `check-scripts-index` rules that a SIBLING may omit the
# declaration but may not contradict its primary's, and this file's subject is one behaviour of
# that primary, not a purpose of its own.
#
# [[D-CYCLE-LANE-WORKTREE-REMOVE-DISCARDS-AN-UNPRESERVED-SCRATCHPAD]]
#
# ★★ WHY THIS FILE EXISTS RATHER THAN A NOTE IN A ROW. ✔MEASURED 2026-09-01 (P50):
# the orchestrator ran `cp -r <worktree>/scratchpad/... scratchpad/... && echo preserved`
# followed by `lane-worktree.sh remove t2` ON ONE COMMAND LINE. The destination's parent
# did not exist, `cp` failed, `&&` swallowed the echo, and the ABSENCE of output read as
# success -- then the next command destroyed the only copy. Lane t2's 14 result JSONs and
# its md5 ledger are gone. The preserve step was a convention living in one head; this
# repository has already measured that a rule with no teeth at the moment of the decision
# erodes. The gate moved the rule into the tool, and this file is what keeps it there.
#
# ⚠⚠ AND FOR TWO CYCLES IT KEPT THE RULE IN EXACTLY ONE OF THE TWO TOOLS. ✔MEASURED
# 2026-09-07 (P63): the row above read ✅ CLOSED while `lane-worktree.ps1` -- the WINDOWS
# entry point, on the host every lane is spawned from -- still ran a bare
# `git worktree remove --force` with NO scratchpad check, no flags and no exit 7. This
# file could not have caught it: every arm drove `bash "$LW"`, so a green run said
# nothing whatever about the twin. ★ N implementations of one behaviour are N places to
# fix, and a pin that exercises ONE of them makes the other's defect invisible while the
# row reads done -- [[feedback-a-partial-fix-reads-as-a-complete-one]] at the twin level.
# ⇒ EVERY GATE ARM BELOW RUNS TWICE, once per twin, and the `.ps1` half carries its OWN
# controls so it cannot pass over a gate that simply refuses everything.
#
# ⚠⚠ AND THE GATE BOTH TWINS SHIPPED LOOKED IN THE WRONG DIRECTORY. ✔MEASURED 2026-09-15
# (P66), read-only, on the five live lanes: every one kept its evidence under
# `.temp/<lane>-scratch/` and NONE had a `scratchpad/`, so the gate counted zero for all of
# them. ✔REPRODUCED on BOTH twins in a throwaway repository, together with two ways the
# preserve reported a VERIFIED copy of evidence it then lost: a destination inside the
# worktree, and a destination already holding a same-named file with other bytes. Arms
# (19)-(24) and their `.ps1` halves (25)-(30) pin the repair.
#
# ⚠⚠ AND `remove` DELETED A LANE'S UNCOMMITTED WORK WITHOUT A WORD. ✔REPRODUCED 2026-09-15
# (P66) on both twins in a throwaway repository: a lane whose own `git status` read
# ` M tracked.txt` and `?? new-work.txt`, removed with no flag, rc=0 -- the edit and the new
# file gone, because `git worktree remove --force` deletes exactly that. Arms (31)-(35) and
# their `.ps1` halves (41)-(45) pin the WORK GATE: refused (exit 8) until `--discard-work` /
# `-DiscardWork`, IGNORED files are not work (the controls (33)/(43)), the refusal comes
# before any evidence is copied, and a directory git cannot open at its own root is refused
# rather than answered for its parent repository.
#
# ⚠⚠ AND A COMMIT MADE INSIDE A LANE WAS STILL DELETED WITHOUT A WORD. ✔REPRODUCED 2026-09-15
# (P66, round 3) on both twins in a throwaway repository: the lane's status read EMPTY, `remove`
# with no flag exited 0, and the worktree's HEAD -- the commit's only reference -- went with it;
# no ref reached the commit afterwards and `git prune` deleted it. Arms (36)-(40) and their `.ps1`
# halves (46)-(50) pin the gate's second question, asked at the repository root: a HEAD holding
# commits no ref reaches is refused and NAMED (36), a list git cannot produce is refused (36b),
# `--discard-work` names what it discards (37), a lane at its base needs no flag (the controls
# (38)/(48)), a commit a shared ref still holds is not refused (39), and one only the lane's own
# per-worktree ref holds is (40).
#
# ⚠⚠ AND UNTIL P66 THIS FILE CREATED ITS PROBE LANES IN THE REPOSITORY IT LIVES IN.
# ✔MEASURED 2026-09-15: the main tree's `.worktrees/.manifests/` held 110 probe manifests --
# `seed-padtest<pid><a-e>` (P44-P59 versions) and `seed-t<pid mod 1000><a-h>` (P60 on) -- with
# no worktree left, and a lane's tree held a registered probe "locked initializing". Every run
# made real `git worktree` registrations in the repository's shared `.git/` and real seed
# manifests beside the orchestrator's; its EXIT trap was the only cleanup, and a trap does not
# run when the process is killed. ✔REPRODUCED with a byte-identical copy of this file in a
# throwaway repository: SIGKILLed once its first probe existed, it left `.worktrees/t928a`
# registered and `seed-t928a.json` in the repository it lived in.
# ⇒ EVERY PROBE NOW LIVES IN A FIXTURE REPOSITORY this file builds under `/tmp`. Byte-identical
#   COPIES (cmp-checked) of the two subjects, the two files they source, and this repository's
#   `.gitignore` are committed there, and the arms drive the COPIES -- so the tree each verb
#   resolves when no `--repo` is given is the fixture, and a killed run leaves a temporary
#   directory, never a registration or a manifest in a real repository. Arms (I1) and (I2)
#   require each first probe to be IN the fixture and nothing of it -- directory, manifest,
#   registration -- in the repository this file lives in. The probe names start with `f` so
#   no historic `t*` or `padtest*` leftover can collide with those pins.
# ⓘ `/tmp`, NOT `$TMPDIR`: `add` refuses any root that leaves under 20 characters of the
#   MAX_PATH budget, and macOS's `$TMPDIR` (/var/folders/.../T/) spends about 49 on its own.
#
# ⚠⚠ THE `.ps1` ARMS ARE HOST-CONDITIONAL, AND THAT CONDITION IS THE ONLY ESCAPE IN THIS
# FIXTURE. The registry row for this repair is named in `CMakeLists.txt`, beside the
# `repo_tree_guard` registration that carried the SAME host-blindness the other way round;
# it is deliberately not cited by id here, because `scripts/` is a scanned root for
# `check-anchor-registry` and a lane that may not write `.plans/**` would be citing a row
# that does not exist yet — an unresolvable citation is a red about bookkeeping, not tree.
# ⛔ IT USED TO BE A FAILURE: a host with no PowerShell got `fail=1` and the sentence
# *CANNOT RUN -- no pwsh could execute lane-worktree.ps1*. ✔MEASURED 2026-09-08 (P65):
# that is a RED NAMING THIS GUARD FOR A PROPERTY OF THE HOST'S SOFTWARE INVENTORY -- the
# misattributing-instrument class -- and it was masked only because every leg driver
# appends `-LE repo-guard`; a plain `ctest` on a PowerShell-less carriage, or any leg run
# with guards forced on, met it. The sibling `run_gate_guard` met exactly that and it
# blocked a push [[D-GUARD-RUN-GATE-FIXTURE-DRIVES-A-WINDOWS-ONLY-INTERPRETER-ON-EVERY-HOST]].
# ⇒ an arm this host CANNOT run is reported NOT APPLICABLE, BY NAME, WITH THE REASON, and
#   COUNTED. Never a failure, and never a silent skip -- the three things that are different
#   findings [[feedback-a-vacuous-skip-and-a-misnamed-red]].
# ★ THE PROBE IS BY EXECUTION, NEVER BY LOOKUP (`lane_worktree_powershell`), AND WHAT IT
#   RUNS IS THE SUBJECT ITSELF. `command -v` is documented IN THIS REPOSITORY to LIE over a
#   non-interactive ssh session on the macOS carriage (the retired remote leg driver
#   carries that measurement), and a lookup also fails the other way -- naming an interpreter
#   that cannot start. The probe requires `lane-worktree.ps1`'s OWN usage line back, so an
#   interpreter that runs but cannot open or parse the subject is ABSENT, not present.
# ★ THE CANDIDATE ORDER `pwsh powershell` IS NOT A GUESS: it is exactly
#   `find_program(POWERSHELL_EXE NAMES pwsh powershell REQUIRED)` in `CMakeLists.txt`.
#   ✔MEASURED 2026-09-08 on this Windows host: BOTH spellings reach lane-worktree.ps1's usage
#   refusal (`pwsh` 7.5.2 and `powershell` 5.1 print `lane-worktree: usage:` and exit 5).
# ★★ THE ESCAPE IS DIRECTIONAL, AND THE FIXTURE RE-MEASURES THAT ON EVERY RUN. Arm (0a)
#   calls the SAME probe with PATH pointing at an empty directory and requires it to report
#   ABSENT, so a probe that had degenerated to *always absent* cannot declare every `.ps1`
#   arm not-applicable and still print a green run
#   [[feedback-an-escape-every-row-triggers-disarms-the-guard]].
# ★★ ON WINDOWS THE ESCAPE IS NOT AVAILABLE AT ALL (arm (0b), `lane_worktree_host_is_windows`):
#   PowerShell ships with that OS and `lane-worktree.ps1` is the entry point THIS host calls,
#   so a Windows host answering *no PowerShell* has a broken PATH, not a legitimate absence.
# ★ NOTHING HERE IS SETTABLE BY A CALLER. The candidate list is a constant and there is no
#   environment override, so the escape cannot be SPELLED.
# ★ THE `.sh` ARMS ARE NEVER ESCAPABLE: (1)-(10), (19)-(24), (31)-(40), (I1) and the `(18)`
#   control with its `.sh` half run everywhere and unconditionally.
#
# ⚠ THE TWO SHELLS DISAGREE ABOUT HOW TO SPELL ONE DIRECTORY, AND EVERY PATH HANDED TO pwsh
# CROSSES THAT BOUNDARY. ✔MEASURED 2026-09-07 under Git Bash: handing pwsh the MSYS spelling
# `/tmp/tmp.XXXX/x` reached it as `C:\tmp\tmp.XXXX\x`, which does not exist. `cygpath -w`
# yields the spelling BOTH sides resolve to the same directory, so the fixture's `.ps1` copy
# and every `-PreserveTo` go over in it; on WSL and macOS there is no cygpath and no
# conversion, so the identity is correct there.
#
# ★ THE ARMS ARE INTEGRATION ARMS ON PURPOSE. The defects were in what the VERB DOES to a real
# tree, so each arm drives a real script against a real `git worktree` and asserts the MESSAGE
# of the refusal it names. An exit code alone cannot separate "refused for the evidence" from
# "refused for the work" from "refused for the name check".
#
# ⚠ ARM (5) IS A CONTROL AND IS NOT DECORATION: without it, arms (1)-(4) all pass over a gate
# that simply refused everything. (15) is its `.ps1` counterpart, (23)/(29) control the clash
# refusals, and (33)/(43) control the work gate.
#
# ⚠⚠ THE PROBE NAMES ARE SHORT ON PURPOSE, AND THE REASON IS MEASURED.
# [[D-TEST-LANE-WORKTREE-SELFTEST-PROBE-NAME-OVERSPENDS-MAX-PATH-INSIDE-A-LANE-WORKTREE]]
# ✔MEASURED 2026-09-04 (P60): probes named `padtest$$a` were 14 characters under Git Bash, and
# run from a LANE worktree `lane-worktree.sh`'s MAX_PATH preflight REFUSED the `add`, while this
# self-test reported `CANNOT RUN -- add failed` with the refusal's reason discarded. The names
# are at most five characters (`f<pid mod 1000><letter>`), and a refused `add` reports the
# refusal's own text. ⓘ Inside the fixture the budget no longer depends on where the test runs:
# `/tmp/lw.XXXXXX/r/.worktrees/<name>` is the same length from the main tree and from a lane.
#
# Exit codes: 0 every arm this host CAN run passed - 1 an arm failed, or the fixture's own
# driver (bash, git, cp, cmp, mktemp, or a file the fixture copies) is missing. ⚠ An arm
# reported NOT APPLICABLE is neither: it is named, counted and printed, and it does not move
# the exit code. The run's own last lines say how many of each there were.
set -uo pipefail

_here="$(cd "$(dirname "$0")" && pwd -P)"
SUBJECT_LW="$_here/lane-worktree.sh"
SUBJECT_LWPS="$_here/lane-worktree.ps1"
REAL_REPO="$(cd "$_here/../.." && pwd -P)"
# ⚠⚠ `-r`, NOT `-x`, AND THE TWO REFUSALS ARE SEPARATE BECAUSE THEY ARE DIFFERENT FACTS.
# ✔MEASURED 2026-09-02 on CI: this arm read `[ -x "$LW" ]`, and `lane-worktree.sh` is committed
# mode 100644 — so on a fresh POSIX checkout the guard refused with "is missing" while the file
# sat right there. Every call below invokes the scripts through an interpreter, so the executable
# bit is consulted by nothing: READABILITY is the real precondition.
for _src in "$SUBJECT_LW" "$SUBJECT_LWPS" "$_here/../leg-tree/leg-tree.sh" \
            "$_here/../repo-tree/repo-tree.ps1" "$REAL_REPO/.gitignore"; do
  [ -e "$_src" ] || { echo "lane-worktree self-test: CANNOT RUN -- $_src does not exist" >&2; exit 1; }
  [ -r "$_src" ] || { echo "lane-worktree self-test: CANNOT RUN -- $_src exists but is not readable" >&2; exit 1; }
done
for _tool in git cp cmp mktemp; do
  command -v "$_tool" >/dev/null 2>&1 \
    || { echo "lane-worktree self-test: CANNOT RUN -- '$_tool' is not on PATH" >&2; exit 1; }
done

TMP="$(mktemp -d /tmp/lw.XXXXXX 2>/dev/null)" \
  || { echo "lane-worktree self-test: CANNOT RUN -- mktemp -d /tmp/lw.XXXXXX failed" >&2; exit 1; }
_cleanup() { rm -rf "$TMP"; }
trap _cleanup EXIT

# ── THE FIXTURE REPOSITORY ────────────────────────────────────────────────────
# See the header: nothing below this block may create anything in "$REAL_REPO".
_fixture_fail() {
  echo "lane-worktree self-test: CANNOT RUN -- the fixture repository could not be built: $*" >&2
  exit 1
}
FIXTURE="$TMP/r"
git init -q "$FIXTURE" >/dev/null 2>&1 || _fixture_fail "git init $FIXTURE"
{ git -C "$FIXTURE" config user.email selftest@example.invalid \
  && git -C "$FIXTURE" config user.name "lane-worktree self-test" \
  && git -C "$FIXTURE" config core.autocrlf false; } || _fixture_fail "git config"
for _rel in scripts/lane-worktree/lane-worktree.sh scripts/lane-worktree/lane-worktree.ps1 \
            scripts/leg-tree/leg-tree.sh scripts/repo-tree/repo-tree.ps1 .gitignore; do
  { mkdir -p "$FIXTURE/$(dirname "$_rel")" && cp "$REAL_REPO/$_rel" "$FIXTURE/$_rel" \
    && cmp -s "$REAL_REPO/$_rel" "$FIXTURE/$_rel"; } || _fixture_fail "byte-identical copy of $_rel"
done
printf 'base\n' > "$FIXTURE/tracked.txt"
{ git -C "$FIXTURE" add -A && git -C "$FIXTURE" commit -q -m "lane-worktree self-test fixture"; } \
  >/dev/null 2>&1 || _fixture_fail "commit"
FIXTURE_HEAD="$(git -C "$FIXTURE" rev-parse --verify HEAD 2>/dev/null)" || _fixture_fail "rev-parse HEAD"
FIXTURE_REAL="$(cd "$FIXTURE" && pwd -P)"
LW="$FIXTURE/scripts/lane-worktree/lane-worktree.sh"
LWPS="$FIXTURE/scripts/lane-worktree/lane-worktree.ps1"
if command -v cygpath >/dev/null 2>&1; then
  TMP_NATIVE="$(cygpath -w "$TMP")"; FIXTURE_NATIVE="$(cygpath -w "$FIXTURE")"; LWPS_NATIVE="$(cygpath -w "$LWPS")"
else
  TMP_NATIVE="$TMP"; FIXTURE_NATIVE="$FIXTURE"; LWPS_NATIVE="$LWPS"
fi
printf 'lane-worktree self-test: fixture repository %s\n' "$FIXTURE_NATIVE"

# At most five characters each -- see the header for why the length is load-bearing.
_sfx="$(( $$ % 1000 ))"
L1="f${_sfx}a"; L2="f${_sfx}b"; L3="f${_sfx}c"; L4="f${_sfx}d"; L5="f${_sfx}e"
L6="f${_sfx}i"; L7="f${_sfx}j"; L8="f${_sfx}k"
W1="f${_sfx}p"; W2="f${_sfx}q"; W3="f${_sfx}r"; W4="f${_sfx}s"
P1="f${_sfx}f"; P2="f${_sfx}g"; P3="f${_sfx}h"
P4="f${_sfx}m"; P5="f${_sfx}n"; P6="f${_sfx}o"
V1="f${_sfx}t"; V2="f${_sfx}u"; V3="f${_sfx}v"; V4="f${_sfx}w"
# The commit-gate lanes reuse their letters: every arm removes its lane before the next one, or
# the `.ps1` half, adds that name again.
C1="f${_sfx}l"; C2="f${_sfx}x"; C3="f${_sfx}y"; C4="f${_sfx}z"
MANIFESTS="$FIXTURE/.worktrees/.manifests"
fail=0
# WHAT THIS RUN ACTUALLY PROVED, counted rather than claimed.
_asserts=0    # assertions that really executed
_failures=0   # of those, how many failed -- `fail` stays the 0/1 EXIT contract
_na_arms=0    # arms this host cannot run, each named with its reason below
# A directory that exists and holds nothing, so arm (0a) can point PATH at a real place
# that reaches no interpreter. ⚠ Under $TMP, so the EXIT trap removes it.
NOWHERE_BIN="$TMP/no-interpreter-here"
mkdir -p "$NOWHERE_BIN"

_arm() {  # <label> <expected-substring> <actual>
  _asserts=$(( _asserts + 1 ))
  case "$3" in
    (*"$2"*) printf '  ok   %s\n' "$1" ;;
    (*) fail=1; _failures=$(( _failures + 1 ))
        printf '  FAIL %s\n       wanted: %s\n       got   : %s\n' "$1" "$2" "$3" ;;
  esac
}
_arm_absent() {  # <label> <forbidden-substring> <actual>
  _asserts=$(( _asserts + 1 ))
  case "$3" in
    (*"$2"*) fail=1; _failures=$(( _failures + 1 ))
             printf '  FAIL %s\n       must NOT contain: %s\n       got   : %s\n' "$1" "$2" "$3" ;;
    (*) printf '  ok   %s\n' "$1" ;;
  esac
}
_ok() {  # <label>
  _asserts=$(( _asserts + 1 )); printf '  ok   %s\n' "$1"
}
_fail() {  # <label> [detail]
  _asserts=$(( _asserts + 1 )); _failures=$(( _failures + 1 )); fail=1; printf '  FAIL %s\n' "$1"
  [ $# -gt 1 ] && printf '       %s\n' "$2"
  return 0
}
# An arm this host CANNOT run. Named, with its reason, and counted.
_na() {  # <label> <reason>
  _na_arms=$(( _na_arms + 1 ))
  printf '  n/a  %s\n       NOT APPLICABLE ON THIS HOST: %s\n' "$1" "$2"
}
# A stale manifest whose CONTENT is recognisable, so "reset" cannot be confused with
# "the file was never written".
_plant_stale_manifest() {  # <lane>
  mkdir -p "$MANIFESTS" 2>/dev/null
  printf '{"src/stale.cpp":"deadbeef"}' > "$MANIFESTS/seed-$1.json"
}
_manifest_of() {  # <lane>
  cat "$MANIFESTS/seed-$1.json" 2>/dev/null || printf '<<absent>>'
}
# Plants evidence exactly where every P66 lane keeps it: `.temp/<lane>-scratch/`.
_plant_temp_evidence() {  # <lane>
  mkdir -p "$FIXTURE/.worktrees/$1/.temp/$1-scratch/mutants"
  printf 'findings\n' > "$FIXTURE/.worktrees/$1/.temp/$1-scratch/findings.log"
  printf 'mutant transcript\n' > "$FIXTURE/.worktrees/$1/.temp/$1-scratch/mutants/m1.log"
}
# Commits a file INSIDE a lane, as a lane never should; the arm then reads what `remove` does.
_commit_in() {  # <lane> <file> <subject>
  printf '%s\n' "$3" > "$FIXTURE/.worktrees/$1/$2" \
    && git -C "$FIXTURE/.worktrees/$1" add -- "$2" >/dev/null 2>&1 \
    && git -C "$FIXTURE/.worktrees/$1" commit -q -m "$3" >/dev/null 2>&1
}
# Deletes the LOOSE object of one of a lane's commits in the FIXTURE's object store, so that
# `git rev-list` over it fails while the lane's `status` still reads (✔MEASURED 2026-09-15:
# status rc=0, rev-list rc=128). Prints `constructed` only when that object was loose and is gone.
_drop_loose_object() {  # <lane> <revision>
  local _sha _obj
  _sha="$(git -C "$FIXTURE/.worktrees/$1" rev-parse --verify -q "$2" 2>/dev/null)" || return 0
  case "$_sha" in (""|*[!0-9a-f]*) return 0 ;; esac
  [ "${#_sha}" -eq 40 ] || return 0
  _obj="$FIXTURE/.git/objects/${_sha:0:2}/${_sha:2}"
  [ -f "$_obj" ] || return 0
  chmod u+w "$_obj" 2>/dev/null
  rm -f "$_obj" && [ ! -e "$_obj" ] && printf 'constructed'
}
# (I1)/(I2): the probe is IN the fixture, and NOTHING of it is in the repository this file lives
# in -- no directory, no seed manifest, no worktree registration. The positive half comes first:
# without it the negative half passes over a probe that was never created anywhere.
_isolated() {  # <label> <probe>
  local _p="$2" _leak="" _line
  if [ ! -d "$FIXTURE/.worktrees/$_p" ]; then
    _fail "$1" "the probe '$_p' is not in the fixture repository $FIXTURE_NATIVE"
    return 0
  fi
  [ -e "$REAL_REPO/.worktrees/$_p" ] && _leak="$_leak .worktrees/$_p"
  [ -e "$REAL_REPO/.worktrees/.manifests/seed-$_p.json" ] && _leak="$_leak .worktrees/.manifests/seed-$_p.json"
  while IFS= read -r _line; do
    case "$_line" in (worktree*"/.worktrees/$_p") _leak="$_leak (a registered worktree)" ;; esac
  done < <(git -C "$REAL_REPO" worktree list --porcelain 2>/dev/null)
  if [ -n "$_leak" ]; then
    _fail "$1" "probe '$_p' reached the repository this test lives in ($REAL_REPO):$_leak"
  else
    _ok "$1"
  fi
}

# ── (1) evidence present, no flag -> REFUSED, and the worktree SURVIVES ──────
# The refusal's own text travels with the CANNOT RUN, never `2>/dev/null` (header).
_plant_stale_manifest "$L1"
if ! _add_out="$(bash "$LW" add "$L1" 2>&1)"; then
  printf 'lane-worktree self-test: CANNOT RUN -- add of %s failed; lane-worktree.sh said:\n%s\n' \
    "$L1" "$_add_out" >&2
  exit 1
fi
_isolated "(I1) the .sh probe lane lives in the FIXTURE, and nothing of it is in the repository this test lives in" "$L1"
mkdir -p "$FIXTURE/.worktrees/$L1/scratchpad/p/lane"
printf 'evidence\n' > "$FIXTURE/.worktrees/$L1/scratchpad/p/lane/probe.log"
out="$(bash "$LW" remove "$L1" 2>&1)"; rc=$?
_arm "(1) a scratchpad file and NO flag is REFUSED" "holds 1 evidence file(s)" "$out"
_arm "(1) ... with the evidence exit code, not a generic one" "7" "$rc"
if [ -d "$FIXTURE/.worktrees/$L1" ]; then _ok "(1) ... and the worktree is STILL ON DISK"
else _fail "(1) the worktree was removed despite the refusal"; fi

# ── (2)(3) --preserve-to copies, VERIFIES, then removes ──────────────────────
out="$(bash "$LW" remove "$L1" --preserve-to "$TMP/kept" 2>&1)"
_arm "(2) --preserve-to reports a VERIFIED copy" "preserved 1 evidence file(s)" "$out"
_arm "(2) ... and only then removes the worktree" "VERIFIED absent" "$out"
# ⓘ Under `scratchpad/`: a preserve lays every evidence root out under its own name.
if [ -f "$TMP/kept/scratchpad/p/lane/probe.log" ]; then _ok "(3) the evidence really is at the destination"
else _fail "(3) the preserved file is not at the destination" "$(find "$TMP/kept" -type f 2>/dev/null | head -3)"; fi

# ── (4) --discard-evidence is a DECISION, and says so ────────────────────────
bash "$LW" add "$L2" >/dev/null 2>&1
mkdir -p "$FIXTURE/.worktrees/$L2/scratchpad"
printf 'x\n' > "$FIXTURE/.worktrees/$L2/scratchpad/a.txt"
out="$(bash "$LW" remove "$L2" --discard-evidence 2>&1)"
_arm "(4) --discard-evidence names what it discards" "DISCARDING 1 evidence file(s)" "$out"

# ── (5) CONTROL: an EMPTY scratchpad needs no flag at all ────────────────────
# Without this arm, every arm above passes over a gate that refuses unconditionally.
bash "$LW" add "$L3" >/dev/null 2>&1
mkdir -p "$FIXTURE/.worktrees/$L3/scratchpad/empty"
out="$(bash "$LW" remove "$L3" 2>&1)"
_arm "(5) CONTROL: an EMPTY scratchpad removes with NO flag" "VERIFIED absent" "$out"

# ── (6) the two evidence flags contradict each other ─────────────────────────
bash "$LW" add "$L4" >/dev/null 2>&1
out="$(bash "$LW" remove "$L4" --preserve-to "$TMP/x" --discard-evidence 2>&1)"
_arm "(6) --preserve-to with --discard-evidence is a usage refusal" "contradict each other" "$out"
bash "$LW" remove "$L4" --discard-evidence >/dev/null 2>&1

# ── (7) THE ROOT IS THE SCRIPT'S OWN TREE, NOT THE CALLER'S CWD ──────────────
# [[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]]
#
# ⚠⚠ THIS ARM MUST SET A DIFFERENT CWD DELIBERATELY, AND THAT IS THE WHOLE REASON IT
# EXISTS. Under ctest `WORKING_DIRECTORY` pins the cwd to the repository, so a pin that merely
# runs cannot see this defect: the cwd and the script's tree agree by construction. So the verb
# runs from a THROWAWAY REPOSITORY that is not the tree the script lives in -- here, the
# fixture copy's tree -- and holds a DECOY lane, so a cwd-keyed resolver succeeds while
# answering about the wrong tree.
# ⚠ NOTHING IS EVER REMOVED BY (7)-(9). Every assertion reads `list`, which writes nothing: an
# arm that exercised `remove` while probing root resolution would exercise it against whichever
# root the code under test picked -- which is the defect.
FOREIGN="$TMP/foreign"
mkdir -p "$FOREIGN"
if git init -q "$FOREIGN" 2>/dev/null \
   && git -C "$FOREIGN" -c user.email=s@e.invalid -c user.name=s \
          commit -q --allow-empty -m base 2>/dev/null; then
  printf '/.worktrees/\n' > "$FOREIGN/.gitignore"
  mkdir -p "$FOREIGN/.worktrees/decoylane"
  printf 'decoy\n' > "$FOREIGN/.worktrees/decoylane/marker.txt"

  # ⚠ REALPATH PREFIX, NEVER A SUBSTRING TEST -- the decoy must be outside the tree the
  # script lives in before anything runs in it.
  _f_real="$(cd "$FOREIGN" && pwd -P)"
  case "$_f_real/" in
    "$FIXTURE_REAL"/*) _fail "(7) fixture $_f_real is INSIDE $FIXTURE_REAL -- refusing to probe" ;;
    *)
      # ★ THE POSITIVE HALF IS A LANE THE VERB ITSELF CREATED IN ITS OWN TREE, not a path
      # string: two CORRECT spellings of one directory (`/c/...` and `C:/...`) would red a
      # string compare, and a lane NAME is spelling-independent.
      bash "$LW" add "$L5" >/dev/null 2>&1
      out="$(cd "$FOREIGN" && bash "$LW" list 2>&1)"
      _arm "(7) driven from a FOREIGN repo's cwd, the verb still answers about the tree it LIVES in" \
           "$L5" "$out"
      case "$out" in
        *decoylane*)
          _fail "(7) the verb reported the FOREIGN cwd's lane \"decoylane\" -- the root is cwd-keyed" \
                "got   : $out" ;;
        *) _ok "(7) ... and does NOT report the foreign cwd's lane" ;;
      esac
      bash "$LW" remove "$L5" --discard-evidence >/dev/null 2>&1

      # (8) `--repo <path>` IS THE EXPLICIT ESCAPE HATCH, from a cwd that is not that tree.
      out="$(cd "$FIXTURE" && bash "$LW" --repo "$FOREIGN" list 2>&1)"
      _arm "(8) --repo names another tree deliberately, from a cwd that is NOT it" \
           "decoylane" "$out"
      ;;
  esac
else
  # ⚠ A FAILURE, NOT A NOT-APPLICABLE: `git` is this fixture's own driver.
  _fail "(7) could not build the foreign-repo fixture -- the cwd arm did not run"
fi

# ── (9) CONTROL FOR (7): the decoy the negative half looks for really EXISTS ──
if [ -f "$FOREIGN/.worktrees/decoylane/marker.txt" ]; then
  _ok "(9) CONTROL: the decoy lane the negative half looks for really exists"
else
  _fail "(9) CONTROL: the decoy fixture was never built -- (7) measured nothing"
fi

# ── (10) `add` RESETS A STALE SEED MANIFEST AND RECORDS THE LANE'S BASE -- the `.sh` half ──
# ⚠ NOT TIDINESS. `lane-fold.py` adjudicates a fold as (the lane's git status set) MINUS
# (seeded paths whose md5 is UNCHANGED), reading `.worktrees/.manifests/seed-<name>.json`,
# which is keyed by lane NAME ALONE. ✔MEASURED 2026-09-03 (P57): four fresh worktrees
# silently inherited manifests written days earlier by lanes of the same two-letter name.
# ★ (10b) SINCE P66 THE MANIFEST ALSO RECORDS THE COMMIT THE LANE WAS CREATED AT, because
# `lane-fold.py` measures every unseeded path against the blob at THAT commit and refuses a
# lane whose HEAD has moved away from it. A bare `{}` passes the older half of this arm.
_m10="$(_manifest_of "$L1")"
_arm_absent "(10) .sh add RESETS a stale seed manifest -- the planted entry is gone" "src/stale.cpp" "$_m10"
_arm "(10) ... to manifest format 2 with NO seeded paths" '"format":2,"paths":{}' "$_m10"
_arm "(10b) ... and RECORDS the commit the lane was created at" "\"base\":\"$FIXTURE_HEAD\"" "$_m10"

# ══ THE EVIDENCE ROOTS AND A PRESERVE THAT CANNOT LIE -- the `.sh` half ═══════
# ── (19) evidence ONLY under .temp/, no flag -> REFUSED, and the worktree SURVIVES ──
bash "$LW" add "$L6" >/dev/null 2>&1
_plant_temp_evidence "$L6"
out="$(bash "$LW" remove "$L6" 2>&1)"; rc=$?
_arm "(19) evidence ONLY under .temp/ and NO flag is REFUSED" "holds 2 evidence file(s)" "$out"
_arm "(19) ... naming .temp/ as where it is" ".temp/=2" "$out"
_arm "(19) ... with the evidence exit code" "7" "$rc"
if [ -d "$FIXTURE/.worktrees/$L6" ]; then _ok "(19) ... and the worktree is STILL ON DISK"
else _fail "(19) the worktree was removed despite its .temp/ evidence"; fi

# ── (20) --preserve-to carries .temp/ too, under its worktree-relative path, byte-identical ──
out="$(bash "$LW" remove "$L6" --preserve-to "$TMP/kept20" 2>&1)"
_arm "(20) --preserve-to preserves every .temp/ file" "preserved 2 evidence file(s)" "$out"
_arm "(20) ... and only then removes the worktree" "VERIFIED absent" "$out"
if [ "$(cat "$TMP/kept20/.temp/$L6-scratch/findings.log" 2>/dev/null)" = "findings" ] \
   && [ "$(cat "$TMP/kept20/.temp/$L6-scratch/mutants/m1.log" 2>/dev/null)" = "mutant transcript" ]; then
  _ok "(20) the .temp/ evidence really is at the destination, byte-identical, under .temp/"
else
  _fail "(20) the .temp/ evidence is not at the destination" "$(find "$TMP/kept20" -type f 2>/dev/null | head -3)"
fi

# ── (21) a destination INSIDE the worktree being removed -> REFUSED ─────────
bash "$LW" add "$L7" >/dev/null 2>&1
mkdir -p "$FIXTURE/.worktrees/$L7/scratchpad"
printf 'evidence\n' > "$FIXTURE/.worktrees/$L7/scratchpad/e.log"
out="$(bash "$LW" remove "$L7" --preserve-to "$FIXTURE/.worktrees/$L7/kept" 2>&1)"; rc=$?
_arm "(21) a --preserve-to INSIDE the worktree being removed is REFUSED" "INSIDE '.worktrees/$L7'" "$out"
_arm "(21) ... with the evidence exit code" "7" "$rc"
if [ -f "$FIXTURE/.worktrees/$L7/scratchpad/e.log" ]; then _ok "(21) ... and the worktree and its evidence SURVIVE"
else _fail "(21) the worktree or its evidence is gone despite the refusal"; fi
bash "$LW" remove "$L7" --discard-evidence >/dev/null 2>&1

# ── (22) a destination already holding the SAME PATH with DIFFERENT bytes -> REFUSED ──
mkdir -p "$TMP/clash/scratchpad"
printf 'somebody else\n' > "$TMP/clash/scratchpad/e.log"
bash "$LW" add "$L8" >/dev/null 2>&1
mkdir -p "$FIXTURE/.worktrees/$L8/scratchpad"
printf 'evidence of the lane\n' > "$FIXTURE/.worktrees/$L8/scratchpad/e.log"
out="$(bash "$LW" remove "$L8" --preserve-to "$TMP/clash" 2>&1)"; rc=$?
_arm "(22) a destination file at the same path with DIFFERENT bytes is REFUSED" "DIFFERENT bytes" "$out"
_arm "(22) ... with the evidence exit code" "7" "$rc"
if [ "$(cat "$TMP/clash/scratchpad/e.log" 2>/dev/null)" = "somebody else" ] && [ -d "$FIXTURE/.worktrees/$L8" ]; then
  _ok "(22) ... the evidence already at the destination is UNTOUCHED, and the worktree survives"
else _fail "(22) the destination's file was overwritten, or the worktree was removed"; fi

# ── (23) CONTROL FOR (22): the SAME bytes already there -- a re-run -- is not a clash ──
printf 'evidence of the lane\n' > "$TMP/clash/scratchpad/e.log"
out="$(bash "$LW" remove "$L8" --preserve-to "$TMP/clash" 2>&1)"
_arm "(23) CONTROL: identical bytes already at the destination preserve and remove" "VERIFIED absent" "$out"

# ── (24) the retired --discard-scratchpad is REFUSED, naming what replaced it ──
# ⓘ No worktree is needed: flags are refused before the verb resolves a tree.
out="$(bash "$LW" remove "$L8" --discard-scratchpad 2>&1)"; rc=$?
_arm "(24) the retired --discard-scratchpad is a usage refusal naming --discard-evidence" "--discard-evidence" "$out"
_arm "(24) ... with the usage exit code" "5" "$rc"

# ══ THE WORK GATE -- the `.sh` half ══════════════════════════════════════════
# ── (31) a TRACKED modification, no flag -> REFUSED (exit 8); the worktree and the edit SURVIVE ──
bash "$LW" add "$W1" >/dev/null 2>&1
printf 'the lane edited this\n' > "$FIXTURE/.worktrees/$W1/tracked.txt"
out="$(bash "$LW" remove "$W1" 2>&1)"; rc=$?
_arm "(31) a tracked modification and NO flag is REFUSED as uncommitted work" "carries 1 path(s) of UNCOMMITTED WORK" "$out"
_arm "(31) ... with the work exit code" "8" "$rc"
if [ "$(cat "$FIXTURE/.worktrees/$W1/tracked.txt" 2>/dev/null)" = "the lane edited this" ]; then
  _ok "(31) ... and the worktree and its edit SURVIVE"
else _fail "(31) the worktree or its edit is gone despite the refusal"; fi

# ── (32) + an untracked file and .temp/ evidence, asked to preserve -> REFUSED BEFORE any copy ──
printf 'the lane wrote this\n' > "$FIXTURE/.worktrees/$W1/new-work.txt"
_plant_temp_evidence "$W1"
out="$(bash "$LW" remove "$W1" --preserve-to "$TMP/k32" 2>&1)"; rc=$?
_arm "(32) tracked + untracked work with a --preserve-to is REFUSED" "carries 2 path(s) of UNCOMMITTED WORK" "$out"
_arm "(32) ... with the work exit code" "8" "$rc"
if [ -z "$(find "$TMP/k32" -type f 2>/dev/null)" ] && [ -f "$FIXTURE/.worktrees/$W1/new-work.txt" ]; then
  _ok "(32) ... and the work gate refused BEFORE the preserve copied anything"
else _fail "(32) evidence was copied, or the work removed, before the work refusal" \
           "$(find "$TMP/k32" -type f 2>/dev/null | head -3)"; fi

# ── (32b) an UNTRACKED, not-ignored file ALONE is uncommitted work ──────────
bash "$LW" add "$W3" >/dev/null 2>&1
printf 'only new\n' > "$FIXTURE/.worktrees/$W3/only-new.txt"
out="$(bash "$LW" remove "$W3" 2>&1)"; rc=$?
_arm "(32b) an untracked, not-ignored file ALONE is REFUSED as uncommitted work" "carries 1 path(s) of UNCOMMITTED WORK" "$out"
_arm "(32b) ... with the work exit code" "8" "$rc"
bash "$LW" remove "$W3" --discard-work >/dev/null 2>&1

# ── (33) CONTROL: IGNORED files are not work ─────────────────────────────────
# ⚠ LOAD-BEARING: without it, (31)-(32b) all pass over a work gate that refuses every removal.
bash "$LW" add "$W2" >/dev/null 2>&1
mkdir -p "$FIXTURE/.worktrees/$W2/build"
printf 'o\n' > "$FIXTURE/.worktrees/$W2/build/x.o"
out="$(bash "$LW" remove "$W2" 2>&1)"
_arm "(33) CONTROL: a worktree holding only IGNORED files removes with NO flag" "VERIFIED absent" "$out"

# ── (34) --discard-work is a DECISION, says so, and composes with --preserve-to ──
# ⓘ This is exactly the call `lane-fold.py land` makes once it has measured nothing left to fold.
out="$(bash "$LW" remove "$W1" --discard-work --preserve-to "$TMP/k34" 2>&1)"
_arm "(34) --discard-work names the work it discards" "DISCARDING 2 path(s) of uncommitted work" "$out"
_arm "(34) ... the evidence is still preserved beside it" "preserved 2 evidence file(s)" "$out"
_arm "(34) ... and only then is the worktree removed" "VERIFIED absent" "$out"

# ── (35) a directory git cannot open AT ITS OWN ROOT is refused, not answered for its parent ──
mkdir -p "$FIXTURE/.worktrees/$W4"
printf 'not a checkout\n' > "$FIXTURE/.worktrees/$W4/stray.txt"
out="$(bash "$LW" remove "$W4" 2>&1)"; rc=$?
_arm "(35) a directory whose git status cannot be read at its own root is REFUSED" "cannot read the git status" "$out"
_arm "(35) ... with the work exit code" "8" "$rc"
if [ -f "$FIXTURE/.worktrees/$W4/stray.txt" ]; then _ok "(35) ... and it SURVIVES"
else _fail "(35) it was deleted despite the refusal"; fi
out="$(bash "$LW" remove "$W4" --discard-work 2>&1)"
_arm "(35b) ... and --discard-work removes it deliberately" "VERIFIED absent" "$out"

# ══ THE WORK GATE, COMMITS -- the `.sh` half ═════════════════════════════════
# ✔REPRODUCED 2026-09-15 (P66, round 3): a file committed inside a lane leaves its status EMPTY,
# and `remove` with no flag deleted the worktree whose HEAD was the commit's only reference.
# ── (36) a HEAD holding commits no ref reaches, no flag -> REFUSED (exit 8), NAMING them ──
bash "$LW" add "$C1" >/dev/null 2>&1
_commit_in "$C1" first.txt "lane commit one"
_commit_in "$C1" second.txt "lane commit two"
c1_head="$(git -C "$FIXTURE/.worktrees/$C1" rev-parse --verify -q HEAD 2>/dev/null)"
out="$(bash "$LW" remove "$C1" 2>&1)"; rc=$?
_arm "(36) a lane whose HEAD holds commits no ref reaches, and NO flag, is REFUSED" "holds 2 COMMIT(S) that no branch, tag or other ref" "$out"
_arm "(36) ... with the work exit code" "8" "$rc"
_arm "(36) ... naming the newer commit" "lane commit two" "$out"
_arm "(36) ... and the older one" "lane commit one" "$out"
if [ -n "$c1_head" ] && [ -d "$FIXTURE/.worktrees/$C1" ] \
   && [ "$(git -C "$FIXTURE/.worktrees/$C1" rev-parse --verify -q HEAD 2>/dev/null)" = "$c1_head" ]; then
  _ok "(36) ... and the worktree SURVIVES, its HEAD still holding them"
else _fail "(36) the worktree, or the HEAD holding the commits, is gone despite the refusal"; fi

# ── (37) --discard-work is a DECISION, and NAMES the commits it discards ─────
out="$(bash "$LW" remove "$C1" --discard-work 2>&1)"
_arm "(37) --discard-work names the commits it discards" "DISCARDING 2 commit(s)" "$out"
_arm "(37) ... each by its subject" "lane commit one" "$out"
_arm "(37) ... and only then is the worktree removed" "VERIFIED absent" "$out"

# ── (36b) a commit list git CANNOT produce is REFUSED, never read as "no commits" ──
bash "$LW" add "$C2" >/dev/null 2>&1
_commit_in "$C2" older.txt "lane commit whose object goes missing"
_commit_in "$C2" newer.txt "lane commit on top of it"
if [ "$(_drop_loose_object "$C2" HEAD~1)" = "constructed" ]; then
  out="$(bash "$LW" remove "$C2" 2>&1)"; rc=$?
  _arm "(36b) a lane whose commits cannot be listed (a missing object) is REFUSED" "cannot list the commits" "$out"
  _arm "(36b) ... with the work exit code" "8" "$rc"
  out="$(bash "$LW" remove "$C2" --discard-work 2>&1)"
  _arm "(36b) ... and --discard-work removes it deliberately" "VERIFIED absent" "$out"
else
  _fail "(36b) CANNOT CONSTRUCT: the lane's older commit was not a loose object in the fixture, so no object could go missing"
  bash "$LW" remove "$C2" --discard-work >/dev/null 2>&1
fi

# ── (38) CONTROL: a lane still AT ITS BASE, with no commits, needs no flag ────
# ⚠ LOAD-BEARING: without it, (36)-(37) pass over a gate that refuses every lane that has a HEAD.
bash "$LW" add "$C3" >/dev/null 2>&1
out="$(bash "$LW" remove "$C3" 2>&1)"
_arm "(38) CONTROL: a lane still at its base, with no commits, removes with NO flag" "VERIFIED absent" "$out"
_arm_absent "(38) ... and names no commit" "COMMIT(S)" "$out"

# ── (39) a commit a SHARED ref outside branches/tags/remotes still holds loses nothing: NOT refused ──
bash "$LW" add "$C4" >/dev/null 2>&1
_commit_in "$C4" kept.txt "lane commit a shared ref keeps"
c4_head="$(git -C "$FIXTURE/.worktrees/$C4" rev-parse --verify -q HEAD 2>/dev/null)"
git -C "$FIXTURE/.worktrees/$C4" update-ref "refs/keep/$C4" HEAD >/dev/null 2>&1
out="$(bash "$LW" remove "$C4" 2>&1)"
_arm "(39) a commit refs/keep/<lane> still holds is NOT refused -- removing the lane loses nothing" "VERIFIED absent" "$out"
if [ -n "$c4_head" ] \
   && [ "$(git -C "$FIXTURE" for-each-ref --contains "$c4_head" --format='%(refname)' 2>/dev/null)" = "refs/keep/$C4" ]; then
  _ok "(39) ... and after the removal that ref still reaches the commit"
else _fail "(39) after the removal refs/keep/$C4 does not reach the lane's commit"; fi

# ── (40) a commit held only by the lane's OWN per-worktree ref dies with it: REFUSED ──
bash "$LW" add "$C1" >/dev/null 2>&1
_commit_in "$C1" bisected.txt "lane commit only its own bisect ref holds"
git -C "$FIXTURE/.worktrees/$C1" update-ref refs/bisect/bad HEAD >/dev/null 2>&1
out="$(bash "$LW" remove "$C1" 2>&1)"; rc=$?
_arm "(40) a commit only the lane's OWN refs/bisect/bad holds is REFUSED -- that ref dies with the worktree" "holds 1 COMMIT(S)" "$out"
_arm "(40) ... with the work exit code" "8" "$rc"
bash "$LW" remove "$C1" --discard-work >/dev/null 2>&1

# ══ THE `.ps1` TWIN ══════════════════════════════════════════════════════════
LWPS_USAGE='usage: lane-worktree.ps1'
PWSH_CANDIDATES='pwsh powershell'
PWSH_PROBE_LOG="$TMP/pwsh-probe.log"
# ⓘ DECIDES WITH SHELL BUILTINS ONLY (`case`, `[`, `printf`, `local`) on purpose: arm (0a)
#   points PATH at an empty directory, and a probe that also needed `grep` or `tr` would report
#   ABSENT there for the wrong reason. The LOG line is written after the verdict.
lane_worktree_powershell() {   # echoes the winning candidate, rc 0; echoes nothing, rc 1
  local _c _out _rc
  : > "$PWSH_PROBE_LOG"
  for _c in $PWSH_CANDIDATES; do
    _out="$("$_c" -NoProfile -NoLogo -File "$LWPS_NATIVE" 2>&1)"; _rc=$?
    case "$_out" in
      (*"$LWPS_USAGE"*) printf '%s' "$_c"; return 0 ;;
    esac
    printf '       [%s] did not reach lane-worktree.ps1 own usage refusal (exit %s): %s\n' \
           "$_c" "$_rc" "${_out//$'\n'/ }" >> "$PWSH_PROBE_LOG"
  done
  return 1
}

# ⚠ THE SAME PREDICATE THE SIBLING FIXTURE USES, deliberately spelled the same way.
lane_worktree_host_is_windows() {
  case "$(uname -s 2>/dev/null || echo unknown)" in
    (MINGW*|MSYS*|CYGWIN*) return 0 ;;
    (*)                    return 1 ;;
  esac
}

# ── (0a) THE ESCAPE MUST BE DIRECTIONAL, AND THAT IS RE-MEASURED ON EVERY RUN ──
# ⓘ A subshell, not a `VAR= func` prefix: bash keeps such an assignment in the CALLER'S
#   environment for a FUNCTION, which would strip PATH for every arm that follows.
_neg="$( PATH="$NOWHERE_BIN"; lane_worktree_powershell )"; _neg_rc=$?
if [ "$_neg_rc" -eq 0 ]; then
  _fail "(0a) DIRECTIONALITY: the probe claimed PowerShell '$_neg' with no interpreter reachable on PATH"
else
  _ok "(0a) DIRECTIONALITY: PATH reaching no interpreter -> the probe reports ABSENT"
fi

# ── (0b) WHICH INTERPRETER THIS HOST ACTUALLY HAS ────────────────────────────
PWSH="$(lane_worktree_powershell)"
if [ -n "$PWSH" ]; then
  _ok "(0b) the .ps1 arms will run under '$PWSH' (probed BY EXECUTION against lane-worktree.ps1's own usage line)"
elif lane_worktree_host_is_windows; then
  _fail "(0b) THIS IS WINDOWS and none of '$PWSH_CANDIDATES' reached lane-worktree.ps1's usage refusal." \
        "PowerShell ships with this OS, CMake already requires one, and lane-worktree.ps1 is the entry point THIS host calls: that is a broken PATH, not a host without PowerShell. The .ps1 arms are NOT escapable here."
  printf '%s' "$(cat "$PWSH_PROBE_LOG" 2>/dev/null)" >&2; printf '\n' >&2
else
  _ok "(0b) no working PowerShell on this host, so the .ps1 arms are NOT APPLICABLE (named below)"
fi
PWSH_ABSENT_WHY="no working PowerShell on this host (each of '$PWSH_CANDIDATES' was RUN against lane-worktree.ps1 and none returned its own '$LWPS_USAGE' line)"

# ── (18) CONTROL AND ITS `.sh` HALF -- UNCONDITIONAL ─────────────────────────
# ✔MEASURED 2026-09-07 (P63): the two `list` implementations did NOT name the same set -- the
# `.ps1` twin reported `.worktrees/.manifests` as a removable lane. The CONTROL is first:
# without it both negative halves pass on a tree where `.manifests` simply does not exist.
if [ -d "$MANIFESTS" ]; then _ok "(18) CONTROL: .worktrees/.manifests really is on disk"
else _fail "(18) CONTROL: .worktrees/.manifests is absent -- the negatives below measure nothing"; fi
_arm_absent "(18) .sh list does not present .manifests as a lane" \
            ".worktrees/.manifests" "$(bash "$LW" list 2>&1)"

if [ -z "$PWSH" ]; then
  # ⚠⚠ NOT A FAILURE AND NOT A SILENT SKIP: each arm the missing interpreter costs is named,
  # given its reason, and counted.
  _na "(11) .ps1 evidence and NO flag is REFUSED" \
      "$PWSH_ABSENT_WHY -- the .ps1 twin's evidence refusal, its exit 7 and the survival of the worktree were NOT taken"
  _na "(I2) .ps1 the probe lane lives in the fixture only" \
      "$PWSH_ABSENT_WHY -- the .ps1 add never ran, so its isolation was not observed"
  _na "(12) .ps1 -PreserveTo reports a VERIFIED copy" \
      "$PWSH_ABSENT_WHY -- the .ps1 twin was never asked to preserve before removing"
  _na "(13) .ps1 the evidence really is at the destination" \
      "$PWSH_ABSENT_WHY -- no .ps1 preserve ran, so no destination could be read back"
  _na "(14) .ps1 -DiscardEvidence names what it discards" \
      "$PWSH_ABSENT_WHY -- the .ps1 twin's explicit-discard sentence was not read"
  _na "(15) .ps1 CONTROL: an EMPTY scratchpad removes with NO flag" \
      "$PWSH_ABSENT_WHY -- the .ps1 CONTROL was not taken"
  _na "(16) .ps1 -PreserveTo with -DiscardEvidence is a usage refusal" \
      "$PWSH_ABSENT_WHY -- the .ps1 twin's contradiction refusal was not read"
  _na "(17) .ps1 add RESETS a stale seed manifest and records the base" \
      "$PWSH_ABSENT_WHY -- the .ps1 add never ran, so the manifest reset was not observed on that twin"
  _na "(18) .ps1 list does not present .manifests as a lane" \
      "$PWSH_ABSENT_WHY -- arm (18) still proves the .sh list above, but the two lists were NOT compared"
  _na "(25) .ps1 evidence ONLY under .temp/ is REFUSED" "$PWSH_ABSENT_WHY -- the .ps1 twin's .temp/ gate was not taken"
  _na "(26) .ps1 -PreserveTo carries .temp/ byte-identical" "$PWSH_ABSENT_WHY -- no .ps1 preserve of .temp/ ran"
  _na "(27) .ps1 a -PreserveTo INSIDE the worktree is REFUSED" "$PWSH_ABSENT_WHY -- the .ps1 containment refusal was not read"
  _na "(28) .ps1 a destination clash is REFUSED" "$PWSH_ABSENT_WHY -- the .ps1 clash refusal was not read"
  _na "(29) .ps1 CONTROL: identical bytes are not a clash" "$PWSH_ABSENT_WHY -- the .ps1 clash CONTROL was not taken"
  _na "(30) .ps1 the retired -DiscardScratchpad is REFUSED" "$PWSH_ABSENT_WHY -- the .ps1 retired-flag refusal was not read"
  _na "(41) .ps1 a tracked modification is REFUSED as uncommitted work" "$PWSH_ABSENT_WHY -- the .ps1 work gate was not taken"
  _na "(42) .ps1 tracked + untracked work is REFUSED before any copy" "$PWSH_ABSENT_WHY -- the .ps1 work gate's ordering was not observed"
  _na "(42b) .ps1 an untracked file alone is REFUSED" "$PWSH_ABSENT_WHY -- the .ps1 untracked-work detection was not taken"
  _na "(43) .ps1 CONTROL: ignored files are not work" "$PWSH_ABSENT_WHY -- the .ps1 work-gate CONTROL was not taken"
  _na "(44) .ps1 -DiscardWork is a decision and composes with -PreserveTo" "$PWSH_ABSENT_WHY -- the .ps1 discard-work sentence was not read"
  _na "(45) .ps1 a directory git cannot open at its root is REFUSED" "$PWSH_ABSENT_WHY -- the .ps1 unreadable-status refusal was not read"
  _na "(46) .ps1 a HEAD holding commits no ref reaches is REFUSED, naming them" "$PWSH_ABSENT_WHY -- the .ps1 commit gate was not taken"
  _na "(46b) .ps1 a commit list git cannot produce is REFUSED" "$PWSH_ABSENT_WHY -- the .ps1 unreadable-commit-list refusal was not read"
  _na "(47) .ps1 -DiscardWork names the commits it discards" "$PWSH_ABSENT_WHY -- the .ps1 commit-discard sentence was not read"
  _na "(48) .ps1 CONTROL: a lane at its base removes with no flag" "$PWSH_ABSENT_WHY -- the .ps1 commit-gate CONTROL was not taken"
  _na "(49) .ps1 a commit a shared ref still holds is NOT refused" "$PWSH_ABSENT_WHY -- the .ps1 shared-ref case was not taken"
  _na "(50) .ps1 a commit only the lane's own per-worktree ref holds is REFUSED" "$PWSH_ABSENT_WHY -- the .ps1 per-worktree-ref case was not taken"
else
  # ── (11) .ps1: evidence present, no flag -> REFUSED, and the worktree SURVIVES ──
  _plant_stale_manifest "$P1"
  if ! _add_out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$P1" 2>&1)"; then
    # ⚠ A FAILURE, NOT A NOT-APPLICABLE: a WORKING interpreter was found and the subject's own
    # `add` refused, so this is the subject misbehaving on a host that can run it.
    _fail "(11) .ps1 add of $P1 failed, so arms (11) (I2) (12) (13) and (17) could not be taken" \
          "lane-worktree.ps1 said: ${_add_out//$'\n'/ }"
  else
    _isolated "(I2) the .ps1 probe lane lives in the FIXTURE, and nothing of it is in the repository this test lives in" "$P1"
    mkdir -p "$FIXTURE/.worktrees/$P1/scratchpad/p/lane"
    printf 'evidence\n' > "$FIXTURE/.worktrees/$P1/scratchpad/p/lane/probe.log"
    out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$P1" 2>&1)"; rc=$?
    _arm "(11) .ps1 a scratchpad file and NO flag is REFUSED" "holds 1 evidence file(s)" "$out"
    _arm "(11) .ps1 ... with the evidence exit code, not a generic one" "7" "$rc"
    if [ -d "$FIXTURE/.worktrees/$P1" ]; then _ok "(11) .ps1 ... and the worktree is STILL ON DISK"
    else _fail "(11) .ps1 the worktree was removed despite the refusal"; fi

    # ── (12)(13) .ps1: -PreserveTo copies, VERIFIES, then removes ─────────────
    out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$P1" -PreserveTo "$TMP_NATIVE/keptps" 2>&1)"
    _arm "(12) .ps1 -PreserveTo reports a VERIFIED copy" "preserved 1 evidence file(s)" "$out"
    _arm "(12) .ps1 ... and only then removes the worktree" "VERIFIED absent" "$out"
    if [ -f "$TMP/keptps/scratchpad/p/lane/probe.log" ]; then _ok "(13) .ps1 the evidence really is at the destination"
    else _fail "(13) .ps1 the preserved file is not at the destination" "$(find "$TMP/keptps" -type f 2>/dev/null | head -3)"; fi

    # ── (17) .ps1 `add` RESETS A STALE SEED MANIFEST AND RECORDS THE BASE ─────
    _m17="$(_manifest_of "$P1")"
    _arm_absent "(17) .ps1 add RESETS a stale seed manifest -- the planted entry is gone" "src/stale.cpp" "$_m17"
    _arm "(17) .ps1 ... to manifest format 2 with NO seeded paths" '"format":2,"paths":{}' "$_m17"
    _arm "(17b) .ps1 ... and RECORDS the commit the lane was created at" "\"base\":\"$FIXTURE_HEAD\"" "$_m17"
  fi

  # ── (14) .ps1: -DiscardEvidence is a DECISION, and says so ──────────────────
  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$P2" >/dev/null 2>&1
  mkdir -p "$FIXTURE/.worktrees/$P2/scratchpad"
  printf 'x\n' > "$FIXTURE/.worktrees/$P2/scratchpad/a.txt"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$P2" -DiscardEvidence 2>&1)"
  _arm "(14) .ps1 -DiscardEvidence names what it discards" "DISCARDING 1 evidence file(s)" "$out"

  # ── (15) .ps1 CONTROL: an EMPTY scratchpad needs no flag at all ─────────────
  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$P3" >/dev/null 2>&1
  mkdir -p "$FIXTURE/.worktrees/$P3/scratchpad/empty"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$P3" 2>&1)"
  _arm "(15) .ps1 CONTROL: an EMPTY scratchpad removes with NO flag" "VERIFIED absent" "$out"

  # ── (16) .ps1: the two evidence flags contradict each other ─────────────────
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$P3" -PreserveTo "$TMP_NATIVE/xps" -DiscardEvidence 2>&1)"
  _arm "(16) .ps1 -PreserveTo with -DiscardEvidence is a usage refusal" "contradict each other" "$out"

  # ── (18) THE `.ps1` HALF OF "THE TWO `list`s MUST NAME THE SAME SET" ────────
  _arm_absent "(18) .ps1 list does not present .manifests as a lane" \
              ".worktrees/.manifests" "$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" list 2>&1)"

  # ══ (25)-(30): THE EVIDENCE ROOTS AND A PRESERVE THAT CANNOT LIE -- the `.ps1` half ══
  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$P4" >/dev/null 2>&1
  _plant_temp_evidence "$P4"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$P4" 2>&1)"; rc=$?
  _arm "(25) .ps1 evidence ONLY under .temp/ and NO flag is REFUSED" "holds 2 evidence file(s)" "$out"
  _arm "(25) .ps1 ... naming .temp/ as where it is" ".temp/=2" "$out"
  _arm "(25) .ps1 ... with the evidence exit code" "7" "$rc"
  if [ -d "$FIXTURE/.worktrees/$P4" ]; then _ok "(25) .ps1 ... and the worktree is STILL ON DISK"
  else _fail "(25) .ps1 the worktree was removed despite its .temp/ evidence"; fi

  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$P4" -PreserveTo "$TMP_NATIVE/kept26" 2>&1)"
  _arm "(26) .ps1 -PreserveTo preserves every .temp/ file" "preserved 2 evidence file(s)" "$out"
  _arm "(26) .ps1 ... and only then removes the worktree" "VERIFIED absent" "$out"
  if [ "$(cat "$TMP/kept26/.temp/$P4-scratch/findings.log" 2>/dev/null)" = "findings" ] \
     && [ "$(cat "$TMP/kept26/.temp/$P4-scratch/mutants/m1.log" 2>/dev/null)" = "mutant transcript" ]; then
    _ok "(26) .ps1 the .temp/ evidence really is at the destination, byte-identical, under .temp/"
  else
    _fail "(26) .ps1 the .temp/ evidence is not at the destination" "$(find "$TMP/kept26" -type f 2>/dev/null | head -3)"
  fi

  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$P5" >/dev/null 2>&1
  mkdir -p "$FIXTURE/.worktrees/$P5/scratchpad"
  printf 'evidence\n' > "$FIXTURE/.worktrees/$P5/scratchpad/e.log"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$P5" -PreserveTo "$FIXTURE_NATIVE/.worktrees/$P5/kept" 2>&1)"; rc=$?
  _arm "(27) .ps1 a -PreserveTo INSIDE the worktree being removed is REFUSED" "INSIDE '.worktrees/$P5'" "$out"
  _arm "(27) .ps1 ... with the evidence exit code" "7" "$rc"
  if [ -f "$FIXTURE/.worktrees/$P5/scratchpad/e.log" ]; then _ok "(27) .ps1 ... and the worktree and its evidence SURVIVE"
  else _fail "(27) .ps1 the worktree or its evidence is gone despite the refusal"; fi
  bash "$LW" remove "$P5" --discard-evidence >/dev/null 2>&1

  mkdir -p "$TMP/clashps/scratchpad"
  printf 'somebody else\n' > "$TMP/clashps/scratchpad/e.log"
  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$P6" >/dev/null 2>&1
  mkdir -p "$FIXTURE/.worktrees/$P6/scratchpad"
  printf 'evidence of the lane\n' > "$FIXTURE/.worktrees/$P6/scratchpad/e.log"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$P6" -PreserveTo "$TMP_NATIVE/clashps" 2>&1)"; rc=$?
  _arm "(28) .ps1 a destination file at the same path with DIFFERENT bytes is REFUSED" "DIFFERENT bytes" "$out"
  _arm "(28) .ps1 ... with the evidence exit code" "7" "$rc"
  if [ "$(cat "$TMP/clashps/scratchpad/e.log" 2>/dev/null)" = "somebody else" ] && [ -d "$FIXTURE/.worktrees/$P6" ]; then
    _ok "(28) .ps1 ... the evidence already at the destination is UNTOUCHED, and the worktree survives"
  else _fail "(28) .ps1 the destination's file was overwritten, or the worktree was removed"; fi

  printf 'evidence of the lane\n' > "$TMP/clashps/scratchpad/e.log"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$P6" -PreserveTo "$TMP_NATIVE/clashps" 2>&1)"
  _arm "(29) .ps1 CONTROL: identical bytes already at the destination preserve and remove" "VERIFIED absent" "$out"

  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$P6" -DiscardScratchpad 2>&1)"; rc=$?
  _arm "(30) .ps1 the retired -DiscardScratchpad is a usage refusal naming -DiscardEvidence" "-DiscardEvidence" "$out"
  _arm "(30) .ps1 ... with the usage exit code" "5" "$rc"

  # ══ (41)-(45): THE WORK GATE -- the `.ps1` half ════════════════════════════
  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$V1" >/dev/null 2>&1
  printf 'the lane edited this\n' > "$FIXTURE/.worktrees/$V1/tracked.txt"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$V1" 2>&1)"; rc=$?
  _arm "(41) .ps1 a tracked modification and NO flag is REFUSED as uncommitted work" "carries 1 path(s) of UNCOMMITTED WORK" "$out"
  _arm "(41) .ps1 ... with the work exit code" "8" "$rc"
  if [ "$(cat "$FIXTURE/.worktrees/$V1/tracked.txt" 2>/dev/null)" = "the lane edited this" ]; then
    _ok "(41) .ps1 ... and the worktree and its edit SURVIVE"
  else _fail "(41) .ps1 the worktree or its edit is gone despite the refusal"; fi

  printf 'the lane wrote this\n' > "$FIXTURE/.worktrees/$V1/new-work.txt"
  _plant_temp_evidence "$V1"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$V1" -PreserveTo "$TMP_NATIVE/k42" 2>&1)"; rc=$?
  _arm "(42) .ps1 tracked + untracked work with a -PreserveTo is REFUSED" "carries 2 path(s) of UNCOMMITTED WORK" "$out"
  _arm "(42) .ps1 ... with the work exit code" "8" "$rc"
  if [ -z "$(find "$TMP/k42" -type f 2>/dev/null)" ] && [ -f "$FIXTURE/.worktrees/$V1/new-work.txt" ]; then
    _ok "(42) .ps1 ... and the work gate refused BEFORE the preserve copied anything"
  else _fail "(42) .ps1 evidence was copied, or the work removed, before the work refusal" \
             "$(find "$TMP/k42" -type f 2>/dev/null | head -3)"; fi

  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$V3" >/dev/null 2>&1
  printf 'only new\n' > "$FIXTURE/.worktrees/$V3/only-new.txt"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$V3" 2>&1)"; rc=$?
  _arm "(42b) .ps1 an untracked, not-ignored file ALONE is REFUSED as uncommitted work" "carries 1 path(s) of UNCOMMITTED WORK" "$out"
  _arm "(42b) .ps1 ... with the work exit code" "8" "$rc"
  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$V3" -DiscardWork >/dev/null 2>&1

  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$V2" >/dev/null 2>&1
  mkdir -p "$FIXTURE/.worktrees/$V2/build"
  printf 'o\n' > "$FIXTURE/.worktrees/$V2/build/x.o"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$V2" 2>&1)"
  _arm "(43) .ps1 CONTROL: a worktree holding only IGNORED files removes with NO flag" "VERIFIED absent" "$out"

  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$V1" -DiscardWork -PreserveTo "$TMP_NATIVE/k44" 2>&1)"
  _arm "(44) .ps1 -DiscardWork names the work it discards" "DISCARDING 2 path(s) of uncommitted work" "$out"
  _arm "(44) .ps1 ... the evidence is still preserved beside it" "preserved 2 evidence file(s)" "$out"
  _arm "(44) .ps1 ... and only then is the worktree removed" "VERIFIED absent" "$out"

  mkdir -p "$FIXTURE/.worktrees/$V4"
  printf 'not a checkout\n' > "$FIXTURE/.worktrees/$V4/stray.txt"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$V4" 2>&1)"; rc=$?
  _arm "(45) .ps1 a directory whose git status cannot be read at its own root is REFUSED" "cannot read the git status" "$out"
  _arm "(45) .ps1 ... with the work exit code" "8" "$rc"
  if [ -f "$FIXTURE/.worktrees/$V4/stray.txt" ]; then _ok "(45) .ps1 ... and it SURVIVES"
  else _fail "(45) .ps1 it was deleted despite the refusal"; fi
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$V4" -DiscardWork 2>&1)"
  _arm "(45b) .ps1 ... and -DiscardWork removes it deliberately" "VERIFIED absent" "$out"

  # ══ (46)-(50): THE WORK GATE, COMMITS -- the `.ps1` half ═══════════════════
  # ⓘ The same four lane names: the `.sh` half removed each one before this block adds it again.
  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$C1" >/dev/null 2>&1
  _commit_in "$C1" first.txt "lane commit one"
  _commit_in "$C1" second.txt "lane commit two"
  c1_head="$(git -C "$FIXTURE/.worktrees/$C1" rev-parse --verify -q HEAD 2>/dev/null)"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$C1" 2>&1)"; rc=$?
  _arm "(46) .ps1 a lane whose HEAD holds commits no ref reaches, and NO flag, is REFUSED" "holds 2 COMMIT(S) that no branch, tag or other ref" "$out"
  _arm "(46) .ps1 ... with the work exit code" "8" "$rc"
  _arm "(46) .ps1 ... naming the newer commit" "lane commit two" "$out"
  _arm "(46) .ps1 ... and the older one" "lane commit one" "$out"
  if [ -n "$c1_head" ] && [ -d "$FIXTURE/.worktrees/$C1" ] \
     && [ "$(git -C "$FIXTURE/.worktrees/$C1" rev-parse --verify -q HEAD 2>/dev/null)" = "$c1_head" ]; then
    _ok "(46) .ps1 ... and the worktree SURVIVES, its HEAD still holding them"
  else _fail "(46) .ps1 the worktree, or the HEAD holding the commits, is gone despite the refusal"; fi

  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$C1" -DiscardWork 2>&1)"
  _arm "(47) .ps1 -DiscardWork names the commits it discards" "DISCARDING 2 commit(s)" "$out"
  _arm "(47) .ps1 ... each by its subject" "lane commit one" "$out"
  _arm "(47) .ps1 ... and only then is the worktree removed" "VERIFIED absent" "$out"

  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$C2" >/dev/null 2>&1
  _commit_in "$C2" older.txt "lane commit whose object goes missing"
  _commit_in "$C2" newer.txt "lane commit on top of it"
  if [ "$(_drop_loose_object "$C2" HEAD~1)" = "constructed" ]; then
    out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$C2" 2>&1)"; rc=$?
    _arm "(46b) .ps1 a lane whose commits cannot be listed (a missing object) is REFUSED" "cannot list the commits" "$out"
    _arm "(46b) .ps1 ... with the work exit code" "8" "$rc"
    out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$C2" -DiscardWork 2>&1)"
    _arm "(46b) .ps1 ... and -DiscardWork removes it deliberately" "VERIFIED absent" "$out"
  else
    _fail "(46b) .ps1 CANNOT CONSTRUCT: the lane's older commit was not a loose object in the fixture, so no object could go missing"
    "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$C2" -DiscardWork >/dev/null 2>&1
  fi

  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$C3" >/dev/null 2>&1
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$C3" 2>&1)"
  _arm "(48) .ps1 CONTROL: a lane still at its base, with no commits, removes with NO flag" "VERIFIED absent" "$out"
  _arm_absent "(48) .ps1 ... and names no commit" "COMMIT(S)" "$out"

  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$C4" >/dev/null 2>&1
  _commit_in "$C4" kept.txt "lane commit a shared ref keeps"
  c4_head="$(git -C "$FIXTURE/.worktrees/$C4" rev-parse --verify -q HEAD 2>/dev/null)"
  git -C "$FIXTURE/.worktrees/$C4" update-ref "refs/keep/$C4" HEAD >/dev/null 2>&1
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$C4" 2>&1)"
  _arm "(49) .ps1 a commit refs/keep/<lane> still holds is NOT refused -- removing the lane loses nothing" "VERIFIED absent" "$out"
  if [ -n "$c4_head" ] \
     && [ "$(git -C "$FIXTURE" for-each-ref --contains "$c4_head" --format='%(refname)' 2>/dev/null)" = "refs/keep/$C4" ]; then
    _ok "(49) .ps1 ... and after the removal that ref still reaches the commit"
  else _fail "(49) .ps1 after the removal refs/keep/$C4 does not reach the lane's commit"; fi

  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" add "$C1" >/dev/null 2>&1
  _commit_in "$C1" bisected.txt "lane commit only its own bisect ref holds"
  git -C "$FIXTURE/.worktrees/$C1" update-ref refs/bisect/bad HEAD >/dev/null 2>&1
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$C1" 2>&1)"; rc=$?
  _arm "(50) .ps1 a commit only the lane's OWN refs/bisect/bad holds is REFUSED -- that ref dies with the worktree" "holds 1 COMMIT(S)" "$out"
  _arm "(50) .ps1 ... with the work exit code" "8" "$rc"
  "$PWSH" -NoProfile -NoLogo -File "$LWPS_NATIVE" remove "$C1" -DiscardWork >/dev/null 2>&1
fi

# ── WHAT THIS RUN ACTUALLY PROVED ───────────────────────────────────────────
# ★ THE COUNTS ARE COUNTED, NOT CLAIMED, AND THEY ARE PRINTED ON EVERY HOST, GREEN OR NOT.
printf 'lane-worktree self-test: %s assertion(s) ran, %s arm(s) not applicable on this host, %s failure(s)\n' \
  "$_asserts" "$_na_arms" "$_failures"
if [ "$_na_arms" -gt 0 ]; then
  echo "  ! NOT PROVED HERE: the .ps1 twin of lane-worktree, and therefore the PARITY of the"
  echo "    two implementations. This host has no working PowerShell, so a green run above is"
  echo "    evidence about lane-worktree.sh ALONE -- and the .ps1 half is the entry point the"
  echo "    WINDOWS host calls, whose evidence gate was missing for two cycles."
  echo "    Not applicable is not a failure, and this fixture still exits on failures only."
fi
if [ "$fail" -eq 0 ]; then
  echo "lane-worktree self-test: OK - the work gate, the evidence gate (scratchpad/ and .temp/), the verified preserve, the root resolver, the seed-manifest reset and the fixture isolation do what they say; this gate is PROVEN able to fail."
else
  echo "lane-worktree self-test: FAILED - the work gate, the evidence gate, the verified preserve, the root resolver, the seed-manifest reset, the fixture isolation or one of the two twins is not doing what it says." >&2
fi
exit "$fail"
