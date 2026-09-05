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
# Exit codes: 0 all arms passed - 1 an arm failed.
set -uo pipefail

_here="$(cd "$(dirname "$0")" && pwd -P)"
LW="$_here/lane-worktree.sh"
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

TMP="$(mktemp -d)"
# At most five characters each -- see the header for why the length is load-bearing.
_sfx="$(( $$ % 1000 ))"
L1="t${_sfx}a"; L2="t${_sfx}b"; L3="t${_sfx}c"; L4="t${_sfx}d"; L5="t${_sfx}e"
fail=0

_sweep() {
  for l in "$L1" "$L2" "$L3" "$L4" "$L5"; do
    [ -e "$REPO/.worktrees/$l" ] && bash "$LW" remove "$l" --discard-scratchpad >/dev/null 2>&1
  done
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
  case "$3" in
    (*"$2"*) printf '  ok   %s\n' "$1" ;;
    (*) fail=1; printf '  FAIL %s\n       wanted: %s\n       got   : %s\n' "$1" "$2" "$3" ;;
  esac
}

# ── (1) files present, no flag -> REFUSED, and the worktree SURVIVES ─────────
# The refusal's own text travels with the CANNOT RUN, never `2>/dev/null` (header).
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
    "$REPO"/*) echo "  FAIL (7) fixture $_f_real is INSIDE $REPO -- refusing to probe"; fail=1 ;;
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
          fail=1
          printf '  FAIL (7) the verb reported the FOREIGN cwd'"'"'s lane "decoylane" -- the root is cwd-keyed\n       got   : %s\n' "$out" ;;
        *) printf '  ok   (7) ... and does NOT report the foreign cwd'"'"'s lane\n' ;;
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
  fail=1
  printf '  FAIL (7) could not build the foreign-repo fixture -- the cwd arm did not run\n'
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
  printf '  ok   (9) CONTROL: the decoy lane the negative half looks for really exists\n'
else
  fail=1; printf '  FAIL (9) CONTROL: the decoy fixture was never built -- (7) measured nothing\n'
fi

if [ "$fail" -eq 0 ]; then
  echo "lane-worktree self-test: OK - 13 assertions over 9 arms (7 gate, 2 control); this gate is PROVEN able to fail."
else
  echo "lane-worktree self-test: FAILED - the scratchpad gate or the root resolver is not doing what it says." >&2
fi
exit "$fail"
