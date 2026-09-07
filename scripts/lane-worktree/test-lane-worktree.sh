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
# ⚠⚠ A MISSING `pwsh` IS A FAILURE, NOT A SKIP, AND THAT IS DELIBERATE. A guard that
# quietly does not run publishes a gate figure about the operator's PATH rather than
# about the tree -- the same ruling `CMakeLists.txt` already applies to the bash it hunts
# for this very test, where a host with no working bash gets a REGISTERED REFUSAL rather
# than a vanished entry. ✔MEASURED 2026-09-07: pwsh 7.5.2 on the Windows host and
# /usr/bin/pwsh inside WSL -- and those two are exactly the carriages that run repo-guards
# (`remote-leg.sh` appends `-LE repo-guard`, `wsl-leg.sh` defaults to running them), so
# this cannot red a leg that actually executes this guard.
# ★ THE PROBE LOOKS FOR THE SUBJECT'S OWN VERDICT LINE, NEVER THE EXIT CODE. This
# script's history includes a driver that reported a bash-not-found `rc=127` as "the pin
# stayed green" -- CANNOT-RUN, RED and VACUOUS are three different findings and naming
# one as another sends the next reader to repair something that was never broken
# [[feedback-a-vacuous-skip-and-a-misnamed-red]].
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
# Exit codes: 0 all arms passed - 1 an arm failed, or an implementation could not be run.
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
  case "$3" in
    (*"$2"*) printf '  ok   %s\n' "$1" ;;
    (*) fail=1; printf '  FAIL %s\n       wanted: %s\n       got   : %s\n' "$1" "$2" "$3" ;;
  esac
}
_arm_absent() {  # <label> <forbidden-substring> <actual>
  case "$3" in
    (*"$2"*) fail=1; printf '  FAIL %s\n       must NOT contain: %s\n       got   : %s\n' "$1" "$2" "$3" ;;
    (*) printf '  ok   %s\n' "$1" ;;
  esac
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
# ★ THE PROBE IS THE SUBJECT'S OWN VERDICT LINE. `pwsh` with no verb must reach
# lane-worktree.ps1's usage refusal; an interpreter that runs but cannot open or parse
# the script is CANNOT-RUN, and so is a `pwsh` that is not there at all. Neither is a
# pass, and neither is reported as a gate failure.
PWSH=""
PWSH_WHY=""
for _c in pwsh pwsh.exe; do
  if ! command -v "$_c" >/dev/null 2>&1; then
    PWSH_WHY="$PWSH_WHY
       [$_c] not found on PATH"
    continue
  fi
  _probe="$("$_c" -NoProfile -NoLogo -File "$LWPS" 2>&1)"; _prc=$?
  case "$_probe" in
    *"usage: lane-worktree.ps1"*) PWSH="$_c"; break ;;
    *) PWSH_WHY="$PWSH_WHY
       [$_c] ran but never reached lane-worktree.ps1's own usage refusal (exit $_prc): $(printf '%s' "$_probe" | tr '\n' ' ')" ;;
  esac
done

if [ -z "$PWSH" ]; then
  # ⚠⚠ A FAILURE, NOT A SKIP. A guard that quietly does not run publishes a figure about
  # the operator's PATH instead of about the tree, and the `.ps1` half of this owner is
  # the one the WINDOWS host actually calls. It is reported as CANNOT-RUN so nobody
  # repairs a gate that never executed [[feedback-a-vacuous-skip-and-a-misnamed-red]].
  fail=1
  printf 'lane-worktree self-test: CANNOT RUN -- no pwsh could execute %s, so the .ps1 twin was NOT exercised.\n' "$LWPS" >&2
  printf '  This is a FAILURE, not a skip: the .ps1 is the entry point the Windows host uses,\n' >&2
  printf '  and its scratchpad gate is the half that was missing for two cycles.\n' >&2
  printf '  Candidates tried:%s\n' "$PWSH_WHY" >&2
  printf '  Install PowerShell 7 (pwsh) or put it on PATH, then re-run.\n' >&2
else
  # ── (11) .ps1: files present, no flag -> REFUSED, and the worktree SURVIVES ──
  _plant_stale_manifest "$P1"
  if ! _add_out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS" add "$P1" 2>&1)"; then
    fail=1
    printf 'lane-worktree self-test: CANNOT RUN -- .ps1 add of %s failed; lane-worktree.ps1 said:\n%s\n' \
      "$P1" "$_add_out" >&2
  else
    mkdir -p "$REPO/.worktrees/$P1/scratchpad/p/lane"
    printf 'evidence\n' > "$REPO/.worktrees/$P1/scratchpad/p/lane/probe.log"
    out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS" remove "$P1" 2>&1)"; rc=$?
    _arm "(11) .ps1 a scratchpad with files and NO flag is REFUSED" "holds a scratchpad with 1 file(s)" "$out"
    _arm "(11) .ps1 ... with the scratchpad exit code, not a generic one" "7" "$rc"
    if [ -d "$REPO/.worktrees/$P1" ]; then printf '  ok   (11) .ps1 ... and the worktree is STILL ON DISK\n'
    else fail=1; printf '  FAIL (11) .ps1 the worktree was removed despite the refusal\n'; fi

    # ── (12)(13) .ps1: -PreserveTo copies, VERIFIES, then removes ─────────────
    # The destination is handed over in the spelling BOTH shells resolve (header).
    out="$("$PWSH" -NoProfile -NoLogo -File "$LWPS" remove "$P1" -PreserveTo "$TMP_NATIVE/keptps" 2>&1)"
    _arm "(12) .ps1 -PreserveTo reports a VERIFIED copy" "preserved 1 scratchpad file(s)" "$out"
    _arm "(12) .ps1 ... and only then removes the worktree" "VERIFIED absent" "$out"
    if [ -f "$TMP/keptps/p/lane/probe.log" ]; then printf '  ok   (13) .ps1 the evidence really is at the destination\n'
    else fail=1; printf '  FAIL (13) .ps1 the preserved file is not at the destination\n'; fi

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

  # ── (18) THE TWO `list`s MUST NAME THE SAME SET ─────────────────────────────
  # ✔MEASURED 2026-09-07 (P63): they did not. `.sh` globs `<repo>/.worktrees/*/`, which
  # skips leading-dot entries; `.ps1` used `Get-ChildItem -Directory -Force`, which does
  # not -- so it reported `.worktrees/.manifests`, `lane-fold`'s seed bookkeeping, as a
  # removable lane holding 238 files. ★ The CONTROL is first and is not optional: without
  # it both negative halves pass on a tree where `.manifests` simply does not exist, which
  # is the vacuous direction.
  if [ -d "$MANIFESTS" ]; then printf '  ok   (18) CONTROL: .worktrees/.manifests really is on disk\n'
  else fail=1; printf '  FAIL (18) CONTROL: .worktrees/.manifests is absent -- the two negatives below measure nothing\n'; fi
  _arm_absent "(18) .sh list does not present .manifests as a lane" \
              ".worktrees/.manifests" "$(bash "$LW" list 2>&1)"
  _arm_absent "(18) .ps1 list does not present .manifests as a lane" \
              ".worktrees/.manifests" "$("$PWSH" -NoProfile -NoLogo -File "$LWPS" list 2>&1)"
fi

if [ "$fail" -eq 0 ]; then
  echo "lane-worktree self-test: OK - 27 assertions over 18 arms (14 gate, 3 control, 1 parity-with-control), BOTH twins driven; this gate is PROVEN able to fail."
else
  echo "lane-worktree self-test: FAILED - the scratchpad gate, the root resolver, the seed-manifest reset or one of the two twins is not doing what it says." >&2
fi
exit "$fail"
