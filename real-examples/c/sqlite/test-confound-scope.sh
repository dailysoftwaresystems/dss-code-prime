#!/usr/bin/env bash
# Verifies the confound classifier by EXTRACTING the block from the shipped
# build-and-test.sh and running it — not by re-implementing it here. A copy would
# stay green if the shipped logic broke, which is the inert-test trap.
#
# ★★ SCOPE + SHELL-OPTION FIDELITY — why this runs the block the awkward way.
# An earlier version ran the extracted block INSIDE a function (`classify() { ... }`)
# under default shell options. Both differed from production, and BOTH mattered:
#   * the shipped block runs at TOP LEVEL (the nearest function closes well above it),
#     so a `local` there is a fatal "can only be used in a function" — yet it is
#     perfectly legal inside the test's own wrapper;
#   * the driver runs `set -Eeuo pipefail` + an ERR trap (build-and-test.sh:64,369),
#     so that failure ABORTS the run — while under default options it is a mere line
#     on stderr and execution continues.
# With either difference present this test stayed GREEN while the real harness died at
# the classification step, AFTER a completed 13-hour arm64 corpus run. So the block is
# now executed in a temp script, at top level, under the driver's exact options.
set -uo pipefail
SH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build-and-test.sh"

# Match with OR without a `local` prefix ON PURPOSE: if someone reintroduces `local`,
# the block must still be EXTRACTED so it fails at RUNTIME for the real reason.
# Anchoring only on the correct form would fail with "could not extract", which proves
# nothing about scope.
BLOCK=$(sed -n "/^  \(local \)\?leg_mode='native'/,/^  fi$/p" "$SH")
if [ -z "$BLOCK" ]; then echo "FATAL: could not extract the classifier block"; exit 1; fi
echo "extracted $(printf '%s\n' "$BLOCK" | wc -l) lines from the shipped script"

TMPRUN=$(mktemp /tmp/confscope_XXXXXX.sh)
trap 'rm -f "$TMPRUN"' EXIT

declare -A LEG_PREFIX=( [host]="" [arm64]="qemu-aarch64" )
# Real declared prefixes from permutations.test: the dotted default AND the `mmap`
# override "mm-" (a DASH, not derivable from the suite name).
declare -a TIER_PREFIXES=(no_mutex_try. memsubsys1. memsubsys2. mm-)

classify() {
  {
    echo 'set -Eeuo pipefail'          # the driver's own options (build-and-test.sh:64)
    echo 'warn() { echo "      WARN: $*"; }'
    printf 'leg=%q\n' "$1"
    printf 'faillist=%q\n' "$2"
    declare -p LEG_PREFIX
    declare -p TIER_PREFIXES
    declare -p CONFOUND_PATTERNS
    printf '%s\n' "$BLOCK"
    cat <<'TAIL'
printf 'REAL=[%s] CONFOUND=[%s] SCOPED=[%s]\n' "${real[*]:-}" "${confound[*]:-}" "${scoped_excused[*]:-}"
TAIL
  } > "$TMPRUN"
  bash "$TMPRUN"                       # top level, driver options — same as production
}

CONFOUND_PATTERNS=('^walsetlk-' '^zipfile-25\.0$' '^recoverfault' 'emulated:^writecrash-')
fails='writecrash-1.1.1 walsetlk-2.1.3 zipfile-25.0 sometest-9.9'

pass=0; fail=0
check() { # <label> <expected-substring> <actual>
  if [[ "$3" == *"$2"* ]]; then echo "  ok   $1"; pass=$((pass+1))
  else echo "  FAIL $1"; echo "       want substring: $2"; echo "       got            : $3"; fail=$((fail+1)); fi
}

echo "--- emulated leg (arm64: runner prefix set) ---"
A=$(classify arm64 "$fails"); echo "$A" | sed 's/^/      /'
check "writecrash EXCUSED on emulated"        "CONFOUND=[writecrash-1.1.1" "$A"
check "writecrash NAMED as scope-excused"     "SCOPED=[writecrash-1.1.1]"  "$A"
check "genuine failure still REAL"            "REAL=[sometest-9.9]"        "$A"

echo "--- native leg (host: no runner prefix) ---"
B=$(classify host "$fails"); echo "$B" | sed 's/^/      /'
check "writecrash NOT excused on native"      "REAL=[writecrash-1.1.1 sometest-9.9]" "$B"
check "no scope excusals on native"           "SCOPED=[]"                            "$B"
check "bare patterns still excuse on native"  "CONFOUND=[walsetlk-2.1.3 zipfile-25.0]" "$B"

echo "--- RED-ON-DISABLE: drop the scope prefix -> it must leak onto native ---"
CONFOUND_PATTERNS=('^walsetlk-' '^zipfile-25\.0$' '^recoverfault' '^writecrash-')
C=$(classify host "$fails")
if [[ "$C" == *"CONFOUND=[writecrash-1.1.1"* ]]; then
  echo "  ok   unscoped pattern DOES leak onto native (so the scope is what prevents it)"; pass=$((pass+1))
else
  echo "  FAIL the guard proves nothing — unscoped behaves the same as scoped"; fail=$((fail+1))
fi

echo "--- PERMUTATION-QUALIFIED names (the \`all\` tier) ---"
CONFOUND_PATTERNS=('^walsetlk-' '^busy2-' '^zipfile-25\.0$' '^recoverfault')
qual='memsubsys1.walsetlk-2.2.6 no_mutex_try.busy2-2.2.3 memsubsys2.recoverfault-1-oom-persistent.515 memsubsys1.zipfile-25.0 no_mutex_try.walsetlk_recover-1.2 memsubsys2.realbug-1.1'
D=$(classify host "$qual"); echo "$D" | sed 's/^/      /'
dconf="$(echo "$D" | sed 's/.*CONFOUND=\[//;s/\].*//')"
dreal="$(echo "$D" | sed 's/.*REAL=\[//;s/\].*//')"
check "qualified walsetlk excused"             "memsubsys1.walsetlk-2.2.6"                    "$dconf"
check "qualified busy2 excused"                "no_mutex_try.busy2-2.2.3"                     "$dconf"
check "qualified recoverfault excused"         "memsubsys2.recoverfault-1-oom-persistent.515" "$dconf"
check "qualified zipfile (anchored \$) excused" "memsubsys1.zipfile-25.0"                     "$dconf"
# walsetlk_recover must NOT be swept in by ^walsetlk- on family resemblance: it is a
# DIFFERENT test FILE that earned its own confound row with its own control (a
# GCC-built reference fails it identically). Keeping this guard means the shipped
# suppression is the explicit ^walsetlk_recover- row, never an accident of ^walsetlk-.
check "walsetlk_recover NOT excused by ^walsetlk-" "no_mutex_try.walsetlk_recover-1.2"        "$dreal"
check "a genuine failure stays REAL"           "memsubsys2.realbug-1.1"                       "$dreal"

echo "--- walsetlk_recover WITH its own row ---"
CONFOUND_PATTERNS=('^walsetlk-' '^walsetlk_recover-' '^recoverfault')
W=$(classify host 'no_mutex_try.walsetlk_recover-1.2 walsetlk_recover-1.3.(36244809) memsubsys2.realbug-1.1')
wconf="$(echo "$W" | sed 's/.*CONFOUND=\[//;s/\].*//')"
wreal="$(echo "$W" | sed 's/.*REAL=\[//;s/\].*//')"
check "excused by its OWN row"        "walsetlk_recover-1.2"    "$wconf"
check "the (n)-suffixed form too"     "walsetlk_recover-1.3"    "$wconf"
check "an unrelated failure REAL"     "memsubsys2.realbug-1.1"  "$wreal"

echo "--- mm- prefixed names (what the pe64 all tier emits) ---"
CONFOUND_PATTERNS=('^walsetlk-' '^zipfile-25\.0$')
G=$(classify host 'mm-zipfile-25.0 mm-walsetlk-2.1.3 mm-backup4-3.3')
gconf="$(echo "$G" | sed 's/.*CONFOUND=\[//;s/\].*//')"
greal="$(echo "$G" | sed 's/.*REAL=\[//;s/\].*//')"
check "mm- zipfile excused"            "mm-zipfile-25.0"   "$gconf"
check "mm- walsetlk excused"           "mm-walsetlk-2.1.3" "$gconf"
check "mm- backup4 (no pattern) REAL"  "mm-backup4-3.3"    "$greal"

echo "--- RED-ON-DISABLE: with NO prefixes known, qualified names go unmatched ---"
CONFOUND_PATTERNS=('^walsetlk-' '^busy2-' '^zipfile-25\.0$' '^recoverfault')
TIER_PREFIXES=()
E=$(classify host "$qual")
if [[ "$E" == *"REAL=[memsubsys1.walsetlk-2.2.6"* ]]; then
  echo "  ok   without TIER_PREFIXES the qualified confounds ARE misreported as genuine"
  pass=$((pass+1))
else
  echo "  FAIL the guard proves nothing — qualified names classify the same either way"
  echo "       got: $E"; fail=$((fail+1))
fi
TIER_PREFIXES=(no_mutex_try. memsubsys1. memsubsys2. mm-)

echo
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
