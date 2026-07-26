#!/usr/bin/env bash
# Verifies the SCOPED-CONFOUND classifier by EXTRACTING the block from the shipped
# build-and-test.sh and running it — not by re-implementing it here. A copy would
# stay green if the shipped logic broke, which is the inert-test trap.
set -uo pipefail
SH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build-and-test.sh"

BLOCK=$(sed -n '/^  local leg_mode=/,/^  fi$/p' "$SH")
if [ -z "$BLOCK" ]; then echo "FATAL: could not extract the classifier block"; exit 1; fi
echo "extracted $(printf '%s\n' "$BLOCK" | wc -l) lines from the shipped script"

warn() { echo "      WARN: $*"; }
declare -A LEG_PREFIX=( [host]="" [arm64]="qemu-aarch64" )

# The extracted text uses `local`, so it must run inside a function — same as in
# the real script.
eval "classify() { local leg=\"\$1\"; local faillist=\"\$2\"
$BLOCK
  printf 'REAL=[%s] CONFOUND=[%s] SCOPED=[%s]\n' \"\${real[*]:-}\" \"\${confound[*]:-}\" \"\${scoped_excused[*]:-}\"
}"

# The `all` tier qualifies names with each suite's DECLARED -prefix; veryquick/quick/
# full declare "" and are unqualified. These are real values from permutations.test:
# the dotted default AND the `mmap` override "mm-", which is why the classifier reads
# them as data instead of guessing `^<ident>\.`.
declare -a TIER_PREFIXES=(no_mutex_try. memsubsys1. memsubsys2. mm-)

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
# Real names taken verbatim from the first Linux `all` run's corpus.log.
CONFOUND_PATTERNS=('^walsetlk-' '^busy2-' '^zipfile-25\.0$' '^recoverfault')
qual='memsubsys1.walsetlk-2.2.6 no_mutex_try.busy2-2.2.3 memsubsys2.recoverfault-1-oom-persistent.515 memsubsys1.zipfile-25.0 no_mutex_try.walsetlk_recover-1.2 memsubsys2.realbug-1.1'
D=$(classify host "$qual"); echo "$D" | sed 's/^/      /'
check "qualified walsetlk excused"    "memsubsys1.walsetlk-2.2.6"                 "$(echo "$D" | sed 's/.*CONFOUND=\[//;s/\].*//')"
check "qualified busy2 excused"       "no_mutex_try.busy2-2.2.3"                  "$(echo "$D" | sed 's/.*CONFOUND=\[//;s/\].*//')"
check "qualified recoverfault excused" "memsubsys2.recoverfault-1-oom-persistent.515" "$(echo "$D" | sed 's/.*CONFOUND=\[//;s/\].*//')"
check "qualified zipfile (anchored \$) excused" "memsubsys1.zipfile-25.0"          "$(echo "$D" | sed 's/.*CONFOUND=\[//;s/\].*//')"
# walsetlk_recover must NOT be swept in by ^walsetlk- on family resemblance. It is a
# DIFFERENT test FILE and it earned its own confound row with its own control (a
# GCC-built reference fails it identically). Keeping this guard means the shipped
# suppression is the explicit ^walsetlk_recover- row, never an accident of ^walsetlk-.
check "walsetlk_recover NOT excused by ^walsetlk-" "no_mutex_try.walsetlk_recover-1.2" "$(echo "$D" | sed 's/.*REAL=\[//;s/\].*//')"

# ...and WITH its own row it IS excused, so the shipped default list classifies it.
CONFOUND_PATTERNS=('^walsetlk-' '^walsetlk_recover-' '^recoverfault')
W=$(classify host 'no_mutex_try.walsetlk_recover-1.2 walsetlk_recover-1.3.(36244809) memsubsys2.realbug-1.1')
check "walsetlk_recover excused by its OWN row" "walsetlk_recover-1.2" "$(echo "$W" | sed 's/.*CONFOUND=\[//;s/\].*//')"
check "the (n)-suffixed form too"               "walsetlk_recover-1.3" "$(echo "$W" | sed 's/.*CONFOUND=\[//;s/\].*//')"
check "an unrelated failure still REAL"         "memsubsys2.realbug-1.1" "$(echo "$W" | sed 's/.*REAL=\[//;s/\].*//')"
CONFOUND_PATTERNS=('^walsetlk-' '^busy2-' '^zipfile-25\.0$' '^recoverfault')
check "a genuine failure stays REAL"  "memsubsys2.realbug-1.1"                    "$(echo "$D" | sed 's/.*REAL=\[//;s/\].*//')"

# The `mmap` suite declares -prefix "mm-", NOT "mmap." — a dash, and not derivable
# from the suite name. This is the shape the pe64 `all` run actually emits.
G=$(classify host 'mm-zipfile-25.0 mm-walsetlk-2.1.3 mm-backup4-3.3')
echo "$G" | sed 's/^/      /'
check "mm- prefixed zipfile excused"  "mm-zipfile-25.0"  "$(echo "$G" | sed 's/.*CONFOUND=\[//;s/\].*//')"
check "mm- prefixed walsetlk excused" "mm-walsetlk-2.1.3" "$(echo "$G" | sed 's/.*CONFOUND=\[//;s/\].*//')"
check "mm- backup4 (no pattern) REAL" "mm-backup4-3.3"   "$(echo "$G" | sed 's/.*REAL=\[//;s/\].*//')"

echo "--- RED-ON-DISABLE: with NO permutations known, qualified names must go unmatched ---"
# This is the pre-fix behaviour: ^-anchored patterns cannot match a qualified name.
TIER_PREFIXES=()
E=$(classify host "$qual")
if [[ "$E" == *"REAL=[memsubsys1.walsetlk-2.2.6"* ]]; then
  echo "  ok   without TIER_PREFIXES the qualified confounds ARE misreported as genuine"
  echo "       (so the strip is what fixes it, not something else)"
  pass=$((pass+1))
else
  echo "  FAIL the guard proves nothing — qualified names classify the same either way"
  echo "       got: $E"; fail=$((fail+1))
fi
TIER_PREFIXES=(no_mutex_try. memsubsys1. memsubsys2. mm-)

echo
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
