#!/usr/bin/env bash
# The self-test for `lane-worktree.sh` AND `lane-worktree.ps1` — it proves that verb's
# `remove` cannot silently delete a lane's evidence, IN BOTH IMPLEMENTATIONS. No
# `PURPOSE:` line: `check-scripts-index` rules that a SIBLING may omit the declaration
# but may not contradict its primary's, and this file's subject is one behaviour of
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
# ⇒ EVERY GATE ARM BELOW NOW RUNS TWICE, once per twin, and the `.ps1` half carries its
# OWN control (an empty scratchpad still removes with no flag) so it cannot pass over a
# gate that simply refuses everything.
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
# ⇒ an arm this host CANNOT run is now reported NOT APPLICABLE, BY NAME, WITH THE REASON,
#   and COUNTED. Never a failure, and never a silent skip -- the three things that are
#   different findings [[feedback-a-vacuous-skip-and-a-misnamed-red]].
# ★ THE PROBE IS BY EXECUTION, NEVER BY LOOKUP (`lane_worktree_powershell`), AND WHAT IT
#   RUNS IS THE SUBJECT ITSELF. The old loop gated each candidate behind
#   `command -v`, which is documented IN THIS REPOSITORY to LIE over a non-interactive
#   ssh session on the macOS carriage (`scripts/remote-leg/remote-leg.sh` carries that
#   measurement, where tools sitting at /opt/homebrew/bin were reported NOT FOUND). A
#   lookup also fails the other way -- naming an interpreter that cannot start -- and that
#   is the direction that hurts here: every `.ps1` arm would then fail for a reason that
#   is not the subject's. The probe requires `lane-worktree.ps1`'s OWN usage line back, so
#   an interpreter that runs but cannot open or parse the subject is ABSENT, not present.
# ★ THE CANDIDATE ORDER `pwsh powershell` IS NOT A GUESS: it is exactly
#   `find_program(POWERSHELL_EXE NAMES pwsh powershell REQUIRED)` in `CMakeLists.txt`, so
#   this fixture drives the twin under the SAME interpreter the build system already picks
#   for every other `.ps1` ctest entry. ✔MEASURED 2026-09-08 on this Windows host: BOTH
#   spellings reach lane-worktree.ps1's usage refusal (`pwsh` 7.5.2 and `powershell` 5.1
#   both print `lane-worktree: usage:` and exit 5), so the fallback is real coverage and
#   not a spelling that would have been rejected anyway.
# ★★ THE ESCAPE IS DIRECTIONAL, AND THE FIXTURE RE-MEASURES THAT ON EVERY RUN. Arm (0a)
#   calls the SAME probe with PATH pointing at an empty directory and requires it to report
#   ABSENT. Without it, a probe that had quietly degenerated to *always absent* would
#   declare every `.ps1` arm not-applicable on every host and still print a green run.
#   ⚠ This repository has already paid for the opposite direction: a guard grew an escape
#   that EVERY subject triggered, so it refused nothing and three mutants came back green
#   [[feedback-an-escape-every-row-triggers-disarms-the-guard]].
# ★★ ON WINDOWS THE ESCAPE IS NOT AVAILABLE AT ALL (arm (0b), `lane_worktree_host_is_windows`).
#   PowerShell ships with that OS, `CMakeLists.txt` already refuses to configure without
#   one, and `lane-worktree.ps1` is the entry point THIS host calls -- so a Windows host
#   answering *no PowerShell* has a broken PATH, not a legitimate absence. Left escapable,
#   a `ctest` launched with a stripped PATH would drop all eight `.ps1` arms on the one
#   host category the twin actually runs on, and report green for doing less work.
# ★ NOTHING HERE IS SETTABLE BY A CALLER. The candidate list is a constant and there is no
#   environment override, so the escape cannot be SPELLED -- an escape that can be spelled
#   is one that will be.
# ★ THE `.sh` ARMS ARE NEVER ESCAPABLE. Arms (1)-(10), and the `(18)` control together
#   with its `.sh` half, run everywhere and unconditionally; a host with no PowerShell
#   still proves the `.sh` twin. ⓘ The `(18)` control and `.sh` half USED to sit inside
#   the pwsh branch, so a PowerShell-less host silently lost them too.
#
# ⚠ THE TWO SHELLS DISAGREE ABOUT HOW TO SPELL ONE DIRECTORY, AND A `-PreserveTo` PATH
# CROSSES THAT BOUNDARY. ✔MEASURED 2026-09-07 under Git Bash: handing pwsh the MSYS
# spelling `/tmp/tmp.XXXX/x` reached it as `C:\tmp\tmp.XXXX\x`, which does not exist --
# an arm built on that would have measured a copy into nowhere. `cygpath -w` yields the
# spelling BOTH sides resolve to the same directory; on WSL and macOS there is no
# cygpath and no conversion, so the identity is correct there.
#
# ★ THE ARMS ARE INTEGRATION ARMS ON PURPOSE. The defect was not in a predicate that
# could be unit-tested in isolation -- it was in what the VERB DOES to a real tree, so
# each arm drives the real script against a real worktree and asserts the MESSAGE of the
# refusal it names. An exit code alone cannot separate "refused for the scratchpad" from
# "refused for the name check".
#
# ⚠ ARM (5) IS THE CONTROL AND IS NOT DECORATION: without it, arms (1)-(4) all pass over
# a gate that simply refused everything, which is the vacuous-fixture class this project
# closes repeatedly. Arm (15) is its `.ps1` counterpart and carries the same weight.
#
# Lane names carry a pid-derived suffix so a parallel `ctest -j` cannot collide with itself,
# and every arm removes what it created on both the pass and the fail path.
#
# ⚠⚠ THE PROBE NAMES ARE SHORT ON PURPOSE, AND THE REASON IS MEASURED.
# [[D-TEST-LANE-WORKTREE-SELFTEST-PROBE-NAME-OVERSPENDS-MAX-PATH-INSIDE-A-LANE-WORKTREE]]
# ✔MEASURED 2026-09-04 (P60): this file named its probes `padtest$$a`, and under Git Bash
# `$$` is a SIX-digit MSYS pid, so the name was 14 characters. Run from the MAIN tree that
# fits; run from a LANE worktree (`<repo>/.worktrees/mo/`, where every lane's own gate runs
# it) the probe root is 14 characters deeper, `lane-worktree.sh`'s MAX_PATH preflight
# computed 19 spare against its required margin of 20 and REFUSED the `add` -- correctly,
# that preflight is a regression guard for an anchored build defect -- and this self-test
# reported `CANNOT RUN -- add failed` with the refusal's reason DISCARDED (`2>/dev/null`).
# Every P60 lane's full gate carried that red, attributed to nobody. Two repairs, both here:
# the probe names are now at most five characters (`t<pid mod 1000><letter>`), the length
# class of a sanctioned lane name, so the self-test spends the same budget wherever it runs;
# and a refused `add` now reports the refusal's own text, because an error that hides its
# diagnosis is a defect in its own right. The uniqueness the pid gave is kept by the suffix
# and by a sweep of same-named leftovers BEFORE the first add: isolation between concurrent
# gates is by TREE (each worktree has its own `.worktrees/`), never by the name's length.
#
# Exit codes: 0 every arm this host CAN run passed - 1 an arm failed, or the fixture's own
# driver (bash, git, or the two subject scripts) is missing. ⚠ An arm reported NOT
# APPLICABLE is neither: it is named, counted and printed, and it does not move the exit
# code. The run's own last lines say how many of each there were.
set -uo pipefail

_here="$(cd "$(dirname "$0")" && pwd -P)"
LW="$_here/lane-worktree.sh"
LWPS="$_here/lane-worktree.ps1"
REPO="$(cd "$_here/../.." && pwd -P)"
# ⚠⚠ `-r`, NOT `-x`, AND THE TWO REFUSALS ARE SEPARATE BECAUSE THEY ARE DIFFERENT FACTS.
# ✔MEASURED 2026-09-02 on CI: this arm read `[ -x "$LW" ]`, and `lane-worktree.sh` is committed
# mode 100644 — so on a fresh POSIX checkout (linux-arm64 and macOS) the guard refused with
# "is missing" while the file sat right there, and on Windows it passed because MSYS reports every
# readable file as executable. A Windows-only measurement published as a property of the tree,
# which is the same class as the gate figure this guard was written for.
# ★ `-x` was never the right question: every call below invokes it as `bash "$LW" …`, so the
# executable bit is not consulted by anything this test does. READABILITY is the real precondition.
# ⓘ Deliberately NOT "fixed" by chmod +x in git: that would make the exec bit load-bearing for a
# file nothing execs directly, and Windows checkouts cannot carry it faithfully anyway.
[ -e "$LW" ] || { echo "lane-worktree self-test: CANNOT RUN -- $LW does not exist" >&2; exit 1; }
[ -r "$LW" ] || { echo "lane-worktree self-test: CANNOT RUN -- $LW exists but is not readable" >&2; exit 1; }
[ -e "$LWPS" ] || { echo "lane-worktree self-test: CANNOT RUN -- $LWPS does not exist" >&2; exit 1; }
[ -r "$LWPS" ] || { echo "lane-worktree self-test: CANNOT RUN -- $LWPS exists but is not readable" >&2; exit 1; }

TMP="$(mktemp -d)"
# The one directory, in the spelling each side resolves. See the header for the measured
# mangling this avoids; on a host without cygpath the two spellings are already one.
if command -v cygpath >/dev/null 2>&1; then TMP_NATIVE="$(cygpath -w "$TMP")"; else TMP_NATIVE="$TMP"; fi
# At most five characters each -- see the header for why the length is load-bearing.
_sfx="$(( $$ % 1000 ))"
L1="t${_sfx}a"; L2="t${_sfx}b"; L3="t${_sfx}c"; L4="t${_sfx}d"; L5="t${_sfx}e"
P1="t${_sfx}f"; P2="t${_sfx}g"; P3="t${_sfx}h"
MANIFESTS="$REPO/.worktrees/.manifests"
fail=0
# WHAT THIS RUN ACTUALLY PROVED, counted rather than claimed. The summary used to state
# a CONSTANT ("27 assertions over 18 arms") that stayed true only on a host carrying both
# interpreters; on any other it was a figure about a run that never happened.
_asserts=0    # assertions that really executed
_failures=0   # of those, how many failed -- `fail` stays the 0/1 EXIT contract
_na_arms=0    # arms this host cannot run, each named with its reason below
# A directory that exists and holds nothing, so arm (0a) can point PATH at a real place
# that reaches no interpreter. ⚠ Under $TMP, so the EXIT trap removes it.
NOWHERE_BIN="$TMP/no-interpreter-here"
mkdir -p "$NOWHERE_BIN"

_sweep() {
  for l in "$L1" "$L2" "$L3" "$L4" "$L5" "$P1" "$P2" "$P3"; do
    [ -e "$REPO/.worktrees/$l" ] && bash "$LW" remove "$l" --discard-scratchpad >/dev/null 2>&1
    # `add` writes a seed manifest keyed by lane NAME alone; a probe's must not outlive it,
    # or the next run of this file inherits it exactly as a real lane would.
    rm -f "$MANIFESTS/seed-$l.json" 2>/dev/null
  done
  return 0
}
_cleanup() {
  _sweep
  rm -rf "$TMP"
}
trap _cleanup EXIT
# A same-named leftover from a run that died before its trap could fire would make the first
# `add` refuse for a reason that is not this test's subject; sweep it first.
_sweep

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
# Counted like an assertion, because the manual checks below are assertions; they simply
# cannot be expressed as a substring match.
_ok() {  # <label>
  _asserts=$(( _asserts + 1 )); printf '  ok   %s\n' "$1"
}
_fail() {  # <label> [detail]
  _asserts=$(( _asserts + 1 )); _failures=$(( _failures + 1 )); fail=1; printf '  FAIL %s\n' "$1"
  [ $# -gt 1 ] && printf '       %s\n' "$2"
  return 0
}
# An arm this host CANNOT run. Named, with its reason, and counted -- the three things a
# silent skip omits, and the three a red gets wrong in the other direction.
_na() {  # <label> <reason>
  _na_arms=$(( _na_arms + 1 ))
  printf '  n/a  %s\n       NOT APPLICABLE ON THIS HOST: %s\n' "$1" "$2"
}
# A stale manifest whose CONTENT is recognisable, so "reset to {}" cannot be confused with
# "the file was never written". ⚠ The fixture is the REMOVE direction: it plants what a real
# stale manifest looks like and requires the verb to destroy it.
_plant_stale_manifest() {  # <lane>
  mkdir -p "$MANIFESTS" 2>/dev/null
  printf '{"src/stale.cpp":"deadbeef"}' > "$MANIFESTS/seed-$1.json"
}
_manifest_of() {  # <lane>
  cat "$MANIFESTS/seed-$1.json" 2>/dev/null || printf '<<absent>>'
}

# ── (1) files present, no flag -> REFUSED, and the worktree SURVIVES ─────────
# The refusal's own text travels with the CANNOT RUN, never `2>/dev/null` (header).
_plant_stale_manifest "$L1"
if ! _add_out="$(bash "$LW" add "$L1" 2>&1)"; then
  printf 'lane-worktree self-test: CANNOT RUN -- add of %s failed; lane-worktree.sh said:\n%s\n' \
    "$L1" "$_add_out" >&2
  exit 1
fi
mkdir -p "$REPO/.worktrees/$L1/scratchpad/p/lane"
printf 'evidence\n' > "$REPO/.worktrees/$L1/scratchpad/p/lane/probe.log"
out="$(bash "$LW" remove "$L1" 2>&1)"; rc=$?
_arm "(1) a scratchpad with files and NO flag is REFUSED" "holds a scratchpad with 1 file(s)" "$out"
_arm "(1) ... with the scratchpad exit code, not a generic one" "7" "$rc"
if [ -d "$REPO/.worktrees/$L1" ]; then _ok "(1) ... and the worktree is STILL ON DISK"
else _fail "(1) the worktree was removed despite the refusal"; fi

# ── (2)(3) --preserve-to copies, VERIFIES, then removes ──────────────────────
out="$(bash "$LW" remove "$L1" --preserve-to "$TMP/kept" 2>&1)"
_arm "(2) --preserve-to reports a VERIFIED copy" "preserved 1 scratchpad file(s)" "$out"
_arm "(2) ... and only then removes the worktree" "VERIFIED absent" "$out"
if [ -f "$TMP/kept/p/lane/probe.log" ]; then _ok "(3) the evidence really is at the destination"
else _fail "(3) the preserved file is not at the destination"; fi

# ── (4) --discard-scratchpad is a DECISION, and says so ──────────────────────
bash "$LW" add "$L2" >/dev/null 2>&1
mkdir -p "$REPO/.worktrees/$L2/scratchpad"
printf 'x\n' > "$REPO/.worktrees/$L2/scratchpad/a.txt"
out="$(bash "$LW" remove "$L2" --discard-scratchpad 2>&1)"
_arm "(4) --discard-scratchpad names what it discards" "DISCARDING 1 scratchpad file(s)" "$out"

# ── (5) CONTROL: an EMPTY scratchpad needs no flag at all ────────────────────
# Without this arm, every arm above passes over a gate that refuses unconditionally.
bash "$LW" add "$L3" >/dev/null 2>&1
mkdir -p "$REPO/.worktrees/$L3/scratchpad/empty"
out="$(bash "$LW" remove "$L3" 2>&1)"
_arm "(5) CONTROL: an EMPTY scratchpad removes with NO flag" "VERIFIED absent" "$out"

# ── (6) the two flags contradict each other ──────────────────────────────────
bash "$LW" add "$L4" >/dev/null 2>&1
out="$(bash "$LW" remove "$L4" --preserve-to "$TMP/x" --discard-scratchpad 2>&1)"
_arm "(6) --preserve-to with --discard-scratchpad is a usage refusal" "contradict each other" "$out"
bash "$LW" remove "$L4" --discard-scratchpad >/dev/null 2>&1

# ── (7) THE ROOT IS THE SCRIPT'S OWN TREE, NOT THE CALLER'S CWD ──────────────
# [[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]]
#
# ⚠⚠ THIS ARM MUST SET A DIFFERENT CWD DELIBERATELY, AND THAT IS THE WHOLE REASON IT
# EXISTS. Under ctest `WORKING_DIRECTORY` pins the cwd to the repository, so a pin
# that merely runs cannot see this defect at all: the cwd and the script's tree agree
# by construction and the wrong resolver looks exactly like the right one. Every arm
# below therefore runs the verb from a THROWAWAY REPOSITORY that is not this one.
#
# ★ THE FIXTURE IS A REPOSITORY, NOT JUST A DIRECTORY. A bare `git rev-parse
# --show-toplevel` from a non-repository FAILS, and the verb would die -- which would
# make this arm pass over the broken resolver for the wrong reason. It has to be a
# tree git can answer about, holding a DECOY lane by the same name, so the old code
# succeeds while answering about the wrong tree.
#
# ⚠ NOTHING IS EVER REMOVED HERE. Every assertion reads `list`, which writes nothing.
# The verb under test is the one whose `remove` deletes trees, and an arm that
# exercised `remove` while probing root resolution would be exercising it against
# whichever root the code under test picked -- which is the defect. Reading is enough:
# if `list` names the wrong tree, so would `remove`.
FOREIGN="$TMP/foreign"
mkdir -p "$FOREIGN"
if git init -q "$FOREIGN" 2>/dev/null \
   && git -C "$FOREIGN" -c user.email=s@e.invalid -c user.name=s \
          commit -q --allow-empty -m base 2>/dev/null; then
  printf '/.worktrees/\n' > "$FOREIGN/.gitignore"
  mkdir -p "$FOREIGN/.worktrees/decoylane"
  printf 'decoy\n' > "$FOREIGN/.worktrees/decoylane/marker.txt"

  # ⚠ REALPATH PREFIX, NEVER A SUBSTRING TEST -- the fixture must be outside the
  # repository before anything runs in it.
  _f_real="$(cd "$FOREIGN" && pwd -P)"
  case "$_f_real/" in
    "$REPO"/*) _fail "(7) fixture $_f_real is INSIDE $REPO -- refusing to probe" ;;
    *)
      # ★ THE POSITIVE HALF IS A LANE THE VERB ITSELF CREATED IN ITS OWN TREE, not a
      # path string. ⓘ Deliberately NOT a comparison against `$REPO`: this test
      # derives that with `pwd -P` (POSIX spelling, `/c/...` under MSYS) while `git
      # worktree list` prints the Windows spelling (`C:/...`), so a string compare of
      # two CORRECT answers would red on a correct tree -- the same two-spellings
      # trap `repo-tree.ps1` already carries a symlink walk for. A lane NAME is
      # spelling-independent.
      bash "$LW" add "$L5" >/dev/null 2>&1
      out="$(cd "$FOREIGN" && bash "$LW" list 2>&1)"
      _arm "(7) driven from a FOREIGN repo's cwd, the verb still answers about the tree it LIVES in" \
           "$L5" "$out"
      # The NEGATIVE half, and it is the one with teeth: today's bare `rev-parse`
      # prints the decoy, and a positive-only pin would pass on a resolver that
      # reached BOTH trees.
      case "$out" in
        *decoylane*)
          _fail "(7) the verb reported the FOREIGN cwd's lane \"decoylane\" -- the root is cwd-keyed" \
                "got   : $out" ;;
        *) _ok "(7) ... and does NOT report the foreign cwd's lane" ;;
      esac
      bash "$LW" remove "$L5" --discard-scratchpad >/dev/null 2>&1

      # (8) `--repo <path>` IS THE EXPLICIT ESCAPE HATCH. Without this arm the fix
      # reads as "the tree is no longer selectable", and the capability the old
      # cwd-keying provided BY ACCIDENT would have been removed rather than named.
      out="$(cd "$REPO" && bash "$LW" --repo "$FOREIGN" list 2>&1)"
      _arm "(8) --repo names another tree deliberately, from a cwd that is NOT it" \
           "decoylane" "$out"
      ;;
  esac
else
  # ⚠ A FAILURE, NOT A NOT-APPLICABLE: `git` is this fixture's own driver (every arm
  # above already ran it), so a host that cannot build a two-commit throwaway repo is
  # broken rather than merely different.
  _fail "(7) could not build the foreign-repo fixture -- the cwd arm did not run"
fi

# ── (9) CONTROL FOR (7): the decoy the negative half looks for really EXISTS ──
# ⚠ WITHOUT THIS, (7)'s negative half passes VACUOUSLY on a fixture that was never
# built -- a failed `git init`, a bad path, a `list` that printed nothing at all.
# It is deliberately a plain filesystem question with NO dependence on the resolver
# under test: coupling it to (7)'s outcome would make it red for (7)'s reason
# instead of measuring its own. ⓘ (8) is the other half of this control -- it proves
# the decoy is not merely present but REACHABLE by this verb when the tree is named
# on purpose, so "decoylane did not appear in (7)" means "the resolver did not go
# there", not "there was nothing to find".
if [ -f "$FOREIGN/.worktrees/decoylane/marker.txt" ]; then
  _ok "(9) CONTROL: the decoy lane the negative half looks for really exists"
else
  _fail "(9) CONTROL: the decoy fixture was never built -- (7) measured nothing"
fi

# ── (10) `add` RESETS A STALE SEED MANIFEST -- the `.sh` half ────────────────
# ⚠ NOT TIDINESS. `lane-fold.py` adjudicates a fold as (the lane's git status set) MINUS
# (seeded paths whose md5 is UNCHANGED), reading `.worktrees/.manifests/seed-<name>.json`,
# which is keyed by lane NAME ALONE. ✔MEASURED 2026-09-03 (P57): four fresh worktrees
# silently inherited manifests written days earlier by lanes of the same two-letter name.
# ★ The expensive direction is not the false refusal: a stale entry that HAPPENS to equal
# the lane's own file marks real lane work as "untouched seed" and the fold SILENTLY DROPS
# IT. The fixture above planted `{"src/stale.cpp":"deadbeef"}` before (1)'s add, so this
# reads what that add did to it.
_arm "(10) .sh add RESETS a stale seed manifest to empty" "{}" "$(_manifest_of "$L1")"

# ══ THE `.ps1` TWIN ══════════════════════════════════════════════════════════
# Everything below drives `lane-worktree.ps1` through the arms above. See the header
# for why a green run of (1)-(10) said nothing whatever about this half for two cycles.
#
# ★ THE PROBE IS THE SUBJECT'S OWN VERDICT LINE, NEVER A LOOKUP AND NEVER AN EXIT CODE.
# The candidate is RUN against `lane-worktree.ps1` itself and must hand back that script's
# own usage refusal; an interpreter that starts but cannot open or parse the subject is
# therefore ABSENT here, exactly like one that is not installed. This script's history
# includes a driver that reported a bash-not-found `rc=127` as "the pin stayed green" --
# CANNOT-RUN, RED and VACUOUS are three different findings [[feedback-a-vacuous-skip-and-a-misnamed-red]].
LWPS_USAGE='usage: lane-worktree.ps1'
PWSH_CANDIDATES='pwsh powershell'
PWSH_PROBE_LOG="$TMP/pwsh-probe.log"
# ⓘ DECIDES WITH SHELL BUILTINS ONLY (`case`, `[`, `printf`, `local`) on purpose: arm (0a)
#   points PATH at an empty directory to disable the CANDIDATES, and a probe that also
#   needed `grep` or `tr` to reach its verdict would report ABSENT there for the wrong
#   reason -- a negative arm that passes vacuously proves nothing about directionality.
#   The diagnostic LOG line below is written after the verdict and cannot influence it.
lane_worktree_powershell() {   # echoes the winning candidate, rc 0; echoes nothing, rc 1
  local _c _out _rc
  : > "$PWSH_PROBE_LOG"
  for _c in $PWSH_CANDIDATES; do
    _out="$("$_c" -NoProfile -NoLogo -File "$LWPS" 2>&1)"; _rc=$?
    case "$_out" in
      (*"$LWPS_USAGE"*) printf '%s' "$_c"; return 0 ;;
    esac
    printf '       [%s] did not reach lane-worktree.ps1 own usage refusal (exit %s): %s\n' \
           "$_c" "$_rc" "${_out//$'\n'/ }" >> "$PWSH_PROBE_LOG"
  done
  return 1
}

# ⚠ THE SAME PREDICATE THE SIBLING FIXTURE USES, deliberately spelled the same way: a
#   second answer to "is this Windows" is how two halves of one rule start disagreeing.
lane_worktree_host_is_windows() {
  case "$(uname -s 2>/dev/null || echo unknown)" in
    (MINGW*|MSYS*|CYGWIN*) return 0 ;;
    (*)                    return 1 ;;
  esac
}

# ── (0a) THE ESCAPE MUST BE DIRECTIONAL, AND THAT IS RE-MEASURED ON EVERY RUN ──
# The SAME probe, run where PATH reaches an EMPTY directory and nothing else, must report
# ABSENT. Without this arm a probe that had quietly degenerated to "always absent" would
# declare every `.ps1` arm not-applicable on every host and still print a green run --
# the exact shape of an escape that refuses nothing.
# ⓘ A subshell, not a `VAR= func` prefix: bash keeps such an assignment in the CALLER'S
#   environment for a FUNCTION, which would strip PATH for every arm that follows.
_neg="$( PATH="$NOWHERE_BIN"; lane_worktree_powershell )"; _neg_rc=$?
if [ "$_neg_rc" -eq 0 ]; then
  _fail "(0a) DIRECTIONALITY: the probe claimed PowerShell '$_neg' with no interpreter reachable on PATH"
else
  _ok "(0a) DIRECTIONALITY: PATH reaching no interpreter -> the probe reports ABSENT"
fi

# ── (0b) WHICH INTERPRETER THIS HOST ACTUALLY HAS ────────────────────────────
# ★★ ON WINDOWS THE ESCAPE IS NOT AVAILABLE AT ALL, and that is the other half of making
# it directional -- see the header block for the measurement.
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

# ── (18) CONTROL AND ITS `.sh` HALF -- UNCONDITIONAL, AND THAT IS THE FIX ────
# ✔MEASURED 2026-09-07 (P63): the two `list` implementations did NOT name the same set.
# `.sh` globs `<repo>/.worktrees/*/`, which skips leading-dot entries; `.ps1` used
# `Get-ChildItem -Directory -Force`, which does not -- so it reported
# `.worktrees/.manifests`, `lane-fold`'s seed bookkeeping, as a removable lane holding 238
# files. ★ The CONTROL is first and is not optional: without it both negative halves pass
# on a tree where `.manifests` simply does not exist, which is the vacuous direction.
# ⚠ THESE TWO USED TO SIT INSIDE THE `pwsh` BRANCH, so a PowerShell-less host lost the
#   CONTROL and the `.sh` negative as well -- coverage that needs no PowerShell at all,
#   silently retired by an escape taken for the other twin.
if [ -d "$MANIFESTS" ]; then _ok "(18) CONTROL: .worktrees/.manifests really is on disk"
else _fail "(18) CONTROL: .worktrees/.manifests is absent -- the negatives below measure nothing"; fi
_arm_absent "(18) .sh list does not present .manifests as a lane" \
            ".worktrees/.manifests" "$(bash "$LW" list 2>&1)"

if [ -z "$PWSH" ]; then
  # ⚠⚠ NOT A FAILURE AND NOT A SILENT SKIP. Each arm the missing interpreter costs is
  # named, given its reason, and counted; the summary then states in words that the `.ps1`
  # twin was not proved here, so a green run cannot be read as proof of the pairing.
  _na "(11) .ps1 a scratchpad with files and NO flag is REFUSED" \
      "$PWSH_ABSENT_WHY -- the .ps1 twin's scratchpad refusal, its exit 7 and the survival of the worktree were NOT taken"
  _na "(12) .ps1 -PreserveTo reports a VERIFIED copy" \
      "$PWSH_ABSENT_WHY -- the .ps1 twin was never asked to preserve before removing"
  _na "(13) .ps1 the evidence really is at the destination" \
      "$PWSH_ABSENT_WHY -- no .ps1 preserve ran, so no destination could be read back"
  _na "(14) .ps1 -DiscardScratchpad names what it discards" \
      "$PWSH_ABSENT_WHY -- the .ps1 twin's explicit-discard sentence was not read"
  _na "(15) .ps1 CONTROL: an EMPTY scratchpad removes with NO flag" \
      "$PWSH_ABSENT_WHY -- the .ps1 CONTROL was not taken, so nothing here would have caught a .ps1 gate that refused everything"
  _na "(16) .ps1 -PreserveTo with -DiscardScratchpad is a usage refusal" \
      "$PWSH_ABSENT_WHY -- the .ps1 twin's contradiction refusal was not read"
  _na "(17) .ps1 add RESETS a stale seed manifest" \
      "$PWSH_ABSENT_WHY -- the .ps1 add never ran, so the manifest reset was not observed on that twin"
  _na "(18) .ps1 list does not present .manifests as a lane" \
      "$PWSH_ABSENT_WHY -- arm (18) still proves the .sh list above, but the two lists were NOT compared"
else
  # ── (11) .ps1: files present, no flag -> REFUSED, and the worktree SURVIVES ──
  _plant_stale_manifest "$P1"
  if ! _add_out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS" add "$P1" 2>&1)"; then
    # ⚠ A FAILURE, NOT A NOT-APPLICABLE, AND THE DISTINCTION IS THE WHOLE POINT OF (0b):
    # a WORKING interpreter was found and the subject's own `add` refused, so this is the
    # subject misbehaving on a host that can run it. The refusal's own text travels with
    # it; an error that hides its diagnosis is a defect in its own right.
    _fail "(11) .ps1 add of $P1 failed, so arms (11) (12) (13) and (17) could not be taken" \
          "lane-worktree.ps1 said: ${_add_out//$'\n'/ }"
  else
    mkdir -p "$REPO/.worktrees/$P1/scratchpad/p/lane"
    printf 'evidence\n' > "$REPO/.worktrees/$P1/scratchpad/p/lane/probe.log"
    out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS" remove "$P1" 2>&1)"; rc=$?
    _arm "(11) .ps1 a scratchpad with files and NO flag is REFUSED" "holds a scratchpad with 1 file(s)" "$out"
    _arm "(11) .ps1 ... with the scratchpad exit code, not a generic one" "7" "$rc"
    if [ -d "$REPO/.worktrees/$P1" ]; then _ok "(11) .ps1 ... and the worktree is STILL ON DISK"
    else _fail "(11) .ps1 the worktree was removed despite the refusal"; fi

    # ── (12)(13) .ps1: -PreserveTo copies, VERIFIES, then removes ─────────────
    # The destination is handed over in the spelling BOTH shells resolve (header).
    out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS" remove "$P1" -PreserveTo "$TMP_NATIVE/keptps" 2>&1)"
    _arm "(12) .ps1 -PreserveTo reports a VERIFIED copy" "preserved 1 scratchpad file(s)" "$out"
    _arm "(12) .ps1 ... and only then removes the worktree" "VERIFIED absent" "$out"
    if [ -f "$TMP/keptps/p/lane/probe.log" ]; then _ok "(13) .ps1 the evidence really is at the destination"
    else _fail "(13) .ps1 the preserved file is not at the destination"; fi

    # ── (17) .ps1 `add` RESETS A STALE SEED MANIFEST ──────────────────────────
    # Read after the removes above, because the manifest outlives the worktree by design.
    _arm "(17) .ps1 add RESETS a stale seed manifest to empty" "{}" "$(_manifest_of "$P1")"
  fi

  # ── (14) .ps1: -DiscardScratchpad is a DECISION, and says so ────────────────
  "$PWSH" -NoProfile -NoLogo -File "$LWPS" add "$P2" >/dev/null 2>&1
  mkdir -p "$REPO/.worktrees/$P2/scratchpad"
  printf 'x\n' > "$REPO/.worktrees/$P2/scratchpad/a.txt"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS" remove "$P2" -DiscardScratchpad 2>&1)"
  _arm "(14) .ps1 -DiscardScratchpad names what it discards" "DISCARDING 1 scratchpad file(s)" "$out"

  # ── (15) .ps1 CONTROL: an EMPTY scratchpad needs no flag at all ─────────────
  # ⚠ LOAD-BEARING, exactly as (5) is: without it, (11)-(14) all pass over a `.ps1`
  # gate that simply refused every removal, which is the vacuous-fixture class.
  "$PWSH" -NoProfile -NoLogo -File "$LWPS" add "$P3" >/dev/null 2>&1
  mkdir -p "$REPO/.worktrees/$P3/scratchpad/empty"
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS" remove "$P3" 2>&1)"
  _arm "(15) .ps1 CONTROL: an EMPTY scratchpad removes with NO flag" "VERIFIED absent" "$out"

  # ── (16) .ps1: the two flags contradict each other ──────────────────────────
  # ⓘ No worktree is created: the contradiction is a usage refusal decided before the
  # verb resolves a tree, on both twins. Spending a full checkout to prove it would
  # measure the same thing more slowly.
  out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS" remove "$P3" -PreserveTo "$TMP_NATIVE/xps" -DiscardScratchpad 2>&1)"
  _arm "(16) .ps1 -PreserveTo with -DiscardScratchpad is a usage refusal" "contradict each other" "$out"

  # ── (18) THE `.ps1` HALF OF "THE TWO `list`s MUST NAME THE SAME SET" ────────
  # ⓘ Its CONTROL and its `.sh` half ran unconditionally above, before this branch, so a
  # host without PowerShell keeps them; only the COMPARISON is lost there.
  _arm_absent "(18) .ps1 list does not present .manifests as a lane" \
              ".worktrees/.manifests" "$("$PWSH" -NoProfile -NoLogo -File "$LWPS" list 2>&1)"
fi

# ── WHAT THIS RUN ACTUALLY PROVED ───────────────────────────────────────────
# ★ THE COUNTS ARE COUNTED, NOT CLAIMED, AND THEY ARE PRINTED ON EVERY HOST, GREEN OR
# NOT. This line used to be the constant "27 assertions over 18 arms ... BOTH twins
# driven", which is a sentence about a run that only ever happened on a host carrying
# both interpreters. A reader who sees only "OK" cannot tell a run that proved both twins
# from one that proved half of them -- which is the entire reason the not-applicable arms
# are named and counted rather than skipped.
printf 'lane-worktree self-test: %s assertion(s) ran, %s arm(s) not applicable on this host, %s failure(s)\n' \
  "$_asserts" "$_na_arms" "$_failures"
if [ "$_na_arms" -gt 0 ]; then
  echo "  ! NOT PROVED HERE: the .ps1 twin of lane-worktree, and therefore the PARITY of the"
  echo "    two implementations. This host has no working PowerShell, so a green run above is"
  echo "    evidence about lane-worktree.sh ALONE -- and the .ps1 half is the entry point the"
  echo "    WINDOWS host calls, whose scratchpad gate was missing for two cycles."
  echo "    Not applicable is not a failure, and this fixture still exits on failures only."
fi
if [ "$fail" -eq 0 ]; then
  echo "lane-worktree self-test: OK - the scratchpad gate, the root resolver and the seed-manifest reset do what they say; this gate is PROVEN able to fail."
else
  echo "lane-worktree self-test: FAILED - the scratchpad gate, the root resolver, the seed-manifest reset or one of the two twins is not doing what it says." >&2
fi
exit "$fail"
