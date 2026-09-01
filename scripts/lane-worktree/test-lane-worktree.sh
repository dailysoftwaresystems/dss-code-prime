#!/usr/bin/env bash
# The self-test for `lane-worktree.sh` — it proves that verb's `remove` cannot
# silently delete a lane's evidence. No `PURPOSE:` line: `check-scripts-index`
# rules that a SIBLING may omit the declaration but may not contradict its
# primary's, and this file's subject is one behaviour of that primary, not a
# purpose of its own.
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
# ★ THE ARMS ARE INTEGRATION ARMS ON PURPOSE. The defect was not in a predicate that
# could be unit-tested in isolation -- it was in what the VERB DOES to a real tree, so
# each arm drives the real script against a real worktree and asserts the MESSAGE of the
# refusal it names. An exit code alone cannot separate "refused for the scratchpad" from
# "refused for the name check", and this script's own history includes a driver that
# reported a bash-not-found rc=127 as "the pin stayed green".
#
# ⚠ ARM (5) IS THE CONTROL AND IS NOT DECORATION: without it, arms (1)-(4) all pass over
# a gate that simply refused everything, which is the vacuous-fixture class this project
# closes repeatedly.
#
# Lane names carry $$ so a parallel `ctest -j` cannot collide with itself, and every arm
# removes what it created on both the pass and the fail path.
#
# Exit codes: 0 all arms passed - 1 an arm failed.
set -uo pipefail

_here="$(cd "$(dirname "$0")" && pwd -P)"
LW="$_here/lane-worktree.sh"
REPO="$(cd "$_here/../.." && pwd -P)"
[ -x "$LW" ] || { echo "lane-worktree self-test: CANNOT RUN -- $LW is missing" >&2; exit 1; }

TMP="$(mktemp -d)"
L1="padtest$$a"; L2="padtest$$b"; L3="padtest$$c"; L4="padtest$$d"
fail=0

_cleanup() {
  for l in "$L1" "$L2" "$L3" "$L4"; do
    [ -e "$REPO/.worktrees/$l" ] && bash "$LW" remove "$l" --discard-scratchpad >/dev/null 2>&1
  done
  rm -rf "$TMP"
}
trap _cleanup EXIT

_arm() {  # <label> <expected-substring> <actual>
  case "$3" in
    (*"$2"*) printf '  ok   %s\n' "$1" ;;
    (*) fail=1; printf '  FAIL %s\n       wanted: %s\n       got   : %s\n' "$1" "$2" "$3" ;;
  esac
}

# ── (1) files present, no flag -> REFUSED, and the worktree SURVIVES ─────────
bash "$LW" add "$L1" >/dev/null 2>&1 || { echo "lane-worktree self-test: CANNOT RUN -- add failed" >&2; exit 1; }
mkdir -p "$REPO/.worktrees/$L1/scratchpad/p/lane"
printf 'evidence\n' > "$REPO/.worktrees/$L1/scratchpad/p/lane/probe.log"
out="$(bash "$LW" remove "$L1" 2>&1)"; rc=$?
_arm "(1) a scratchpad with files and NO flag is REFUSED" "holds a scratchpad with 1 file(s)" "$out"
_arm "(1) ... with the scratchpad exit code, not a generic one" "7" "$rc"
if [ -d "$REPO/.worktrees/$L1" ]; then printf '  ok   (1) ... and the worktree is STILL ON DISK\n'
else fail=1; printf '  FAIL (1) the worktree was removed despite the refusal\n'; fi

# ── (2)(3) --preserve-to copies, VERIFIES, then removes ──────────────────────
out="$(bash "$LW" remove "$L1" --preserve-to "$TMP/kept" 2>&1)"
_arm "(2) --preserve-to reports a VERIFIED copy" "preserved 1 scratchpad file(s)" "$out"
_arm "(2) ... and only then removes the worktree" "VERIFIED absent" "$out"
if [ -f "$TMP/kept/p/lane/probe.log" ]; then printf '  ok   (3) the evidence really is at the destination\n'
else fail=1; printf '  FAIL (3) the preserved file is not at the destination\n'; fi

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

if [ "$fail" -eq 0 ]; then
  echo "lane-worktree self-test: OK - 9 assertions over 6 arms (5 gate, 1 control); this gate is PROVEN able to fail."
else
  echo "lane-worktree self-test: FAILED - the scratchpad gate is not doing what it says." >&2
fi
exit "$fail"
