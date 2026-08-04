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
# ── bash 4+ required (`declare -A`) — macOS ships 3.2 ────────────────────────
# D-HARNESS-SELFTEST-BSD-SED-PORTABILITY. This guard is NOT redundant with the
# driver's: this file is also run STANDALONE (by hand, by CI, by a future ctest
# row), and under bash 3.2 it died at the first `declare -A` having run ZERO
# assertions — while still exiting 0. A guard that reports success having proven
# nothing is worse than one that fails, so refuse rather than under-report.
# Mirrors build-and-test.sh:67 verbatim; on Linux the version check passes and
# the loop is never entered.
if [ -z "${BASH_VERSINFO:-}" ] || [ "${BASH_VERSINFO[0]:-0}" -lt 4 ]; then
  for _newer_bash in /opt/homebrew/bin/bash /usr/local/bin/bash "$(command -v bash 2>/dev/null || true)"; do
    if [ -n "$_newer_bash" ] && [ -x "$_newer_bash" ] && "$_newer_bash" -c '[ "${BASH_VERSINFO[0]}" -ge 4 ]' 2>/dev/null; then
      exec "$_newer_bash" "$0" "$@"
    fi
  done
  echo "ERROR: this self-test needs bash 4+ (found ${BASH_VERSION:-unknown}); on macOS run: brew install bash" >&2
  exit 1
fi

set -uo pipefail
SH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build-and-test.sh"

# Match with OR without a `local` prefix ON PURPOSE: if someone reintroduces `local`,
# the block must still be EXTRACTED so it fails at RUNTIME for the real reason.
# Anchoring only on the correct form would fail with "could not extract", which proves
# nothing about scope.
#
# ★ POSIX BRE ONLY — `\(local \)*`, never `\(local \)\?`
# (D-HARNESS-SELFTEST-BSD-SED-PORTABILITY). `\?` is a GNU sed EXTENSION: BSD/macOS
# sed treats it literally, so the address never matched, the block extracted as
# EMPTY, and the driver's own self-test failed with "could not extract" — which
# refused to start the run on macOS, the one host the self-test exists to protect
# (the first run on a NEW HOST is the one you least want to lose). `\(...\)` and
# `*` are both core POSIX BRE and behave identically on BSD and GNU sed, so this
# form needs no `-E` and has no dialect dependency. MEASURED on both: BSD sed
# extracts 0 lines with `\?` vs 52 with `*`; GNU sed extracts 52 either way (so
# the Linux/Windows legs are byte-unchanged). `*` also still matches the
# `local`-prefixed form, preserving the deliberate red-on-disable above.
BLOCK=$(sed -n "/^  \(local \)*leg_mode='native'/,/^  fi$/p" "$SH")
if [ -z "$BLOCK" ]; then echo "FATAL: could not extract the classifier block"; exit 1; fi
echo "extracted $(printf '%s\n' "$BLOCK" | wc -l) lines from the shipped script"

# Trailing X's ONLY — BSD/macOS mktemp substitutes X's only at the END of the
# template, so `..._XXXXXX.sh` was returned VERBATIM (a fixed, shared path: two
# concurrent runs would clobber each other, and the failure messages named the
# literal template). The suffix bought nothing — the file is executed via
# "$BASH", not by extension. Portable to GNU coreutils mktemp unchanged.
TMPRUN=$(mktemp /tmp/confscope_XXXXXX)
# Scratch for the checkout-provenance section below (created unconditionally so
# ONE trap owns every temp — a second `trap … EXIT` would silently replace this one).
TMPPROV=$(mktemp /tmp/confprov_XXXXXX)
PROVTMP=$(mktemp -d /tmp/confprovd_XXXXXX)
trap 'rm -f "$TMPRUN" "$TMPPROV"; rm -rf "$PROVTMP"' EXIT

# The classifier's ONE input for confound scoping: the leg's RUN MODE, as the
# host-independent leg resolver (harness_legs.py) reports it — `native` when this
# host executes the artifact directly, `launched` when it goes through a declared
# launcher (qemu for a cross-arch host, Wine for a cross-OS one). This used to be
# `LEG_PREFIX` (the runner-prefix string the driver maintained by hand); the driver
# now reads the resolver's own answer, so the fixture must speak the same word or
# the extracted block would evaluate an unset array under `set -u`.
# The leg LABELS are the shipped catalogue's (legs.json), not invented here.
declare -A LEG_RUN_MODE=( [elf64-x86_64]="native" [elf64-arm64]="launched" )
# Real declared prefixes from permutations.test: the dotted default AND the `mmap`
# override "mm-" (a DASH, not derivable from the suite name).
declare -a TIER_PREFIXES=(no_mutex_try. memsubsys1. memsubsys2. mm-)

classify() {
  {
    echo 'set -Eeuo pipefail'          # the driver's own options (build-and-test.sh:64)
    echo 'warn() { echo "      WARN: $*"; }'
    printf 'leg=%q\n' "$1"
    printf 'faillist=%q\n' "$2"
    declare -p LEG_RUN_MODE
    declare -p TIER_PREFIXES
    declare -p CONFOUND_PATTERNS
    printf '%s\n' "$BLOCK"
    cat <<'TAIL'
printf 'REAL=[%s] CONFOUND=[%s] SCOPED=[%s]\n' "${real[*]:-}" "${confound[*]:-}" "${scoped_excused[*]:-}"
TAIL
  } > "$TMPRUN"
  # "$BASH", never a bare `bash` — see the header guard. The emitted script
  # replays `declare -p` output for an ASSOCIATIVE array, which bash 3.2 cannot
  # parse (it evaluates `[host]` arithmetically → "host: unbound variable" under
  # set -u). PATH `bash` is 3.2 on macOS, so a bare `bash` here failed all 20
  # assertions on a correct classifier. Same interpreter on Linux, no change.
  "$BASH" "$TMPRUN"                    # top level, driver options — same as production
}

CONFOUND_PATTERNS=('^walsetlk-' '^zipfile-25\.0$' '^recoverfault' 'emulated:^writecrash-')
fails='writecrash-1.1.1 walsetlk-2.1.3 zipfile-25.0 sometest-9.9'

# ── THE ACCOUNTING INVARIANT ─────────────────────────────────────────────────
# Every assertion in this file must land in EXACTLY ONE of pass/fail/skip, and the
# three must SUM to this number. Checked at the bottom; a mismatch FAILS the
# self-test, which at Step 0 of the driver refuses to start the run.
#
# WHY A DECLARED TOTAL AND NOT JUST THE SKIP COUNTERS. The two SKIP branches below
# carry HAND-WRITTEN counts, and a hand-written count drifts. It did: they said 11
# and 10 against blocks holding 14 and 12, so on a git-less host FIVE assertions
# vanished with no trace while the driver reported "driver self-test: OK (20
# assertions)" — a number that at HEAD meant the ENTIRE battery and by then meant
# 43% of it. Counting the skips was supposed to make "all proven" unsayable; only
# summing them against a declared total actually does. This is the same defect
# class the classifier assertions above exist to kill: an instrument that reports
# a pass over work it did not do.
# ★ ADDING AN ASSERTION WITHOUT BUMPING THIS NUMBER FAILS ON THE VERY NEXT RUN,
# by design. One line to update, against an instrument that would otherwise lie.
TOTAL_ASSERTIONS=46      # 20 classifier + 14 provenance helpers + 12 Step-2 gate
pass=0; fail=0; skip=0
check() { # <label> <expected-substring> <actual>
  if [[ "$3" == *"$2"* ]]; then echo "  ok   $1"; pass=$((pass+1))
  else echo "  FAIL $1"; echo "       want substring: $2"; echo "       got            : $3"; fail=$((fail+1)); fi
}
check_eq() { # <label> <expected-EXACT> <actual>
  # Substring matching cannot express "the field is EMPTY" or "the count is exactly
  # 2" — every string contains "". The provenance assertions below need both.
  if [[ "$3" == "$2" ]]; then echo "  ok   $1"; pass=$((pass+1))
  else echo "  FAIL $1"; echo "       want exactly: [$2]"; echo "       got         : [$3]"; fail=$((fail+1)); fi
}

echo "--- LAUNCHED leg (elf64-arm64: runs through a declared launcher) ---"
A=$(classify elf64-arm64 "$fails"); echo "$A" | sed 's/^/      /'
check "writecrash EXCUSED on emulated"        "CONFOUND=[writecrash-1.1.1" "$A"
check "writecrash NAMED as scope-excused"     "SCOPED=[writecrash-1.1.1]"  "$A"
check "genuine failure still REAL"            "REAL=[sometest-9.9]"        "$A"

echo "--- NATIVE leg (elf64-x86_64: this host executes it directly) ---"
B=$(classify elf64-x86_64 "$fails"); echo "$B" | sed 's/^/      /'
check "writecrash NOT excused on native"      "REAL=[writecrash-1.1.1 sometest-9.9]" "$B"
check "no scope excusals on native"           "SCOPED=[]"                            "$B"
check "bare patterns still excuse on native"  "CONFOUND=[walsetlk-2.1.3 zipfile-25.0]" "$B"

echo "--- RED-ON-DISABLE: drop the scope prefix -> it must leak onto native ---"
CONFOUND_PATTERNS=('^walsetlk-' '^zipfile-25\.0$' '^recoverfault' '^writecrash-')
C=$(classify elf64-x86_64 "$fails")
if [[ "$C" == *"CONFOUND=[writecrash-1.1.1"* ]]; then
  echo "  ok   unscoped pattern DOES leak onto native (so the scope is what prevents it)"; pass=$((pass+1))
else
  echo "  FAIL the guard proves nothing — unscoped behaves the same as scoped"; fail=$((fail+1))
fi

echo "--- PERMUTATION-QUALIFIED names (the \`all\` tier) ---"
CONFOUND_PATTERNS=('^walsetlk-' '^busy2-' '^zipfile-25\.0$' '^recoverfault')
qual='memsubsys1.walsetlk-2.2.6 no_mutex_try.busy2-2.2.3 memsubsys2.recoverfault-1-oom-persistent.515 memsubsys1.zipfile-25.0 no_mutex_try.walsetlk_recover-1.2 memsubsys2.realbug-1.1'
D=$(classify elf64-x86_64 "$qual"); echo "$D" | sed 's/^/      /'
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
W=$(classify elf64-x86_64 'no_mutex_try.walsetlk_recover-1.2 walsetlk_recover-1.3.(36244809) memsubsys2.realbug-1.1')
wconf="$(echo "$W" | sed 's/.*CONFOUND=\[//;s/\].*//')"
wreal="$(echo "$W" | sed 's/.*REAL=\[//;s/\].*//')"
check "excused by its OWN row"        "walsetlk_recover-1.2"    "$wconf"
check "the (n)-suffixed form too"     "walsetlk_recover-1.3"    "$wconf"
check "an unrelated failure REAL"     "memsubsys2.realbug-1.1"  "$wreal"

echo "--- mm- prefixed names (what the pe64 all tier emits) ---"
CONFOUND_PATTERNS=('^walsetlk-' '^zipfile-25\.0$')
G=$(classify elf64-x86_64 'mm-zipfile-25.0 mm-walsetlk-2.1.3 mm-backup4-3.3')
gconf="$(echo "$G" | sed 's/.*CONFOUND=\[//;s/\].*//')"
greal="$(echo "$G" | sed 's/.*REAL=\[//;s/\].*//')"
check "mm- zipfile excused"            "mm-zipfile-25.0"   "$gconf"
check "mm- walsetlk excused"           "mm-walsetlk-2.1.3" "$gconf"
check "mm- backup4 (no pattern) REAL"  "mm-backup4-3.3"    "$greal"

echo "--- RED-ON-DISABLE: with NO prefixes known, qualified names go unmatched ---"
CONFOUND_PATTERNS=('^walsetlk-' '^busy2-' '^zipfile-25\.0$' '^recoverfault')
TIER_PREFIXES=()
E=$(classify elf64-x86_64 "$qual")
if [[ "$E" == *"REAL=[memsubsys1.walsetlk-2.2.6"* ]]; then
  echo "  ok   without TIER_PREFIXES the qualified confounds ARE misreported as genuine"
  pass=$((pass+1))
else
  echo "  FAIL the guard proves nothing — qualified names classify the same either way"
  echo "       got: $E"; fail=$((fail+1))
fi
TIER_PREFIXES=(no_mutex_try. memsubsys1. memsubsys2. mm-)

# ─────────────────────────────────────────────────────────────────────────────
# CHECKOUT PROVENANCE (D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE)
# ─────────────────────────────────────────────────────────────────────────────
# Same discipline as the classifier above: EXTRACT the shipped block and run it —
# a re-implementation here would stay green while the driver's copy rotted.
# Extraction is by SENTINEL rather than by content address, because these helpers
# have no line as distinctive as `leg_mode='native'` to anchor on, and the .ps1
# already extracts the clone-lock region the same way.
# ★ POSIX BRE only — no `\?`/`\+`/`\|` (D-HARNESS-SELFTEST-BSD-SED-PORTABILITY);
# these two addresses are literal text plus `^`, which BSD and GNU sed agree on.
PROV=$(sed -n -e '/^# >>> dss:src-provenance >>>$/,/^# <<< dss:src-provenance <<<$/p' "$SH")
if [ -z "$PROV" ]; then echo "FATAL: could not extract the src-provenance block"; exit 1; fi
echo "extracted $(printf '%s\n' "$PROV" | wc -l) provenance lines from the shipped script"

# The helpers shell out to git, so they are exercised against a REAL repository —
# a stub would test the stub. `git` is not yet guaranteed at this point: the driver
# runs this self-test as Step 0, BEFORE Step 1 installs git, and refusing there
# would stop the harness from bootstrapping a bare machine. So: skip loudly,
# and count the skips so the summary can never read as "all proven".
prov_probe() {                 # prov_probe <repo> <non-repo> <empty-dir> <hidden-only-dir>
  {
    echo 'set -Eeuo pipefail'          # the driver's own options (build-and-test.sh:70)
    printf '%s\n' "$PROV"
    cat <<'TAIL'
printf 'HEAD=[%s]\n'          "$(git_head_short      "$R")"
printf 'BRANCH_OK=[%s]\n'     "$(git_head_branch     "$R")"
printf 'DIVERGE=[%s]\n'       "$(git_tree_divergence "$R")"
printf 'NOREPO_HEAD=[%s]\n'   "$(git_head_short      "$NR")"
printf 'NOREPO_DIV=[%s]\n'    "$(git_tree_divergence "$NR")"
if dir_has_entries "$R";  then printf 'ENT_REPO=[yes]\n';   else printf 'ENT_REPO=[no]\n';   fi
if dir_has_entries "$E";  then printf 'ENT_EMPTY=[yes]\n';  else printf 'ENT_EMPTY=[no]\n';  fi
if dir_has_entries "$H";  then printf 'ENT_HIDDEN=[yes]\n'; else printf 'ENT_HIDDEN=[no]\n'; fi
read_src_provenance "$R"
printf 'NOTE=[%s]\n' "$SRC_DIVERGE_NOTE"
printf 'SHORT=[%s]\n' "$SRC_HEAD"
TAIL
  } > "$TMPPROV"
  # "$BASH" for the same reason as classify(): PATH bash is 3.2 on macOS and the
  # block uses `mapfile`, which 3.2 does not have.
  R="$1" NR="$2" E="$3" H="$4" "$BASH" "$TMPPROV"
}
field() { printf '%s\n' "$2" | sed -n "s/^$1=\\[\\(.*\\)\\]$/\\1/p"; }
# -c overrides rather than the caller's global config: a box with commit.gpgsign=true,
# a core.hooksPath, or an init template would otherwise fail (or run hooks) inside a
# self-test. Defined at top level because BOTH sections below build repos with it.
gitq() { git -c init.templateDir= -c user.email=selftest@dss.invalid \
             -c user.name=dss-selftest -c commit.gpgsign=false "$@"; }

echo "--- checkout provenance: helpers run against a REAL git repo ---"
if ! command -v git >/dev/null 2>&1; then
  # 14 = the twelve check/check_eq calls in the else-arm plus its two inline
  # red-on-disable guards (the bare-rev-parse one and the `-uno` one). Kept honest
  # by the TOTAL_ASSERTIONS invariant at the bottom, not by this comment.
  echo "  SKIP no git on PATH — the provenance helpers were NOT exercised (14 assertions skipped)"
  skip=$((skip+14))
else
  PROVREPO="$PROVTMP/repo"; PROVNONE="$PROVTMP/plain"; PROVEMPTY="$PROVTMP/empty"; PROVHID="$PROVTMP/hidden"
  mkdir -p "$PROVREPO" "$PROVNONE" "$PROVEMPTY" "$PROVHID"
  echo one > "$PROVNONE/file.c"          # populated, no .git — shape (c) of the anchor
  echo hi  > "$PROVHID/.hidden"          # ONLY a dotfile: still not clonable-into
  gitq init -q "$PROVREPO" >/dev/null 2>&1
  echo tracked > "$PROVREPO/tracked.c"
  gitq -C "$PROVREPO" add tracked.c >/dev/null 2>&1
  gitq -C "$PROVREPO" commit -q --no-verify -m "seed" >/dev/null 2>&1
  want_sha="$(git -C "$PROVREPO" rev-parse --short HEAD 2>/dev/null || true)"
  want_br="$(git -C "$PROVREPO" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"

  echo "  ·· state 1: pristine checkout"
  P1="$(prov_probe "$PROVREPO" "$PROVNONE" "$PROVEMPTY" "$PROVHID")"
  check_eq "HEAD is the real short sha"        "$want_sha" "$(field HEAD "$P1")"
  check_eq "branch is the real branch"         "$want_br"  "$(field BRANCH_OK "$P1")"
  check_eq "clean tree diverges by 0"          "0"         "$(field DIVERGE "$P1")"
  check_eq "clean tree gets NO note"           ""          "$(field NOTE "$P1")"
  # Item (5) of the anchor: the old `printf ... "$(git rev-parse …)"` printed an
  # EMPTY field when git failed and `set -e` never saw it. A provenance field must
  # be non-empty and SAY it is unknown.
  check    "non-repo HEAD says UNKNOWN"        "UNKNOWN"   "$(field NOREPO_HEAD "$P1")"
  # "" (uncomputable) must stay DISTINCT from "0" (clean) — collapsing them would
  # report a tree nothing could measure as pristine.
  check_eq "non-repo divergence is UNCOMPUTABLE, not 0" "" "$(field NOREPO_DIV "$P1")"
  check_eq "dir_has_entries: populated dir"    "yes"       "$(field ENT_REPO "$P1")"
  check_eq "dir_has_entries: empty dir"        "no"        "$(field ENT_EMPTY "$P1")"
  check_eq "dir_has_entries: DOTFILE-only dir" "yes"       "$(field ENT_HIDDEN "$P1")"

  echo "  ·· state 2: + an UNTRACKED file (what a stale .git beside fresh sources looks like)"
  echo added > "$PROVREPO/added.c"
  P2="$(prov_probe "$PROVREPO" "$PROVNONE" "$PROVEMPTY" "$PROVHID")"
  check_eq "untracked file counts as divergence" "1" "$(field DIVERGE "$P2")"

  echo "  ·· state 3: + a MODIFIED tracked file"
  echo more >> "$PROVREPO/tracked.c"
  P3="$(prov_probe "$PROVREPO" "$PROVNONE" "$PROVEMPTY" "$PROVHID")"
  check_eq "modified + untracked = 2"          "2"    "$(field DIVERGE "$P3")"
  check    "the note carries the count"        "+2 file(s) differ from HEAD" "$(field NOTE "$P3")"

  echo "--- RED-ON-DISABLE: the PRE-FIX expression prints an EMPTY provenance field ---"
  # Exactly what build-and-test.sh:2418 used to interpolate. If this ever stops
  # being empty, the UNKNOWN(...) guard above is proving nothing.
  old_form="$(git -C "$PROVNONE" rev-parse --short HEAD 2>/dev/null || true)"
  if [ -z "$old_form" ]; then
    echo "  ok   a bare rev-parse on a non-checkout yields EMPTY (so UNKNOWN(...) is what prevents it)"
    pass=$((pass+1))
  else
    echo "  FAIL the guard proves nothing — the bare form already produced [$old_form]"; fail=$((fail+1))
  fi

  echo "--- RED-ON-DISABLE: counting WITHOUT untracked files hides the rsync shape ---"
  # `-uno` is the tempting "quieter" variant. On state 3 it sees only the modified
  # tracked file, so a stale-.git tree carrying NEW sources would report 0 and the
  # verdict would look clean. This asserts the two really differ.
  uno_n="$(git -C "$PROVREPO" status --porcelain -uno 2>/dev/null | wc -l | tr -d ' ')"
  if [ "$uno_n" = "1" ]; then
    echo "  ok   -uno undercounts (1 vs 2), so listing untracked files is load-bearing"
    pass=$((pass+1))
  else
    echo "  FAIL the guard proves nothing — -uno reported $uno_n, same information as the shipped form"
    fail=$((fail+1))
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# THE STEP-2 SOURCE GATE — all four shapes of the anchor, in under a second
# ─────────────────────────────────────────────────────────────────────────────
# The gate is TOP-LEVEL driver flow, not a function, so the only faithful way to
# test it is the one the classifier already uses: extract it and run it at top
# level under the driver's exact options, with the logging helpers stubbed.
# Nothing about the DECISION is re-implemented here — the clone helper is the real
# one and it performs a real clone, from a throwaway local repo instead of GitHub.
GATE=$(sed -n -e '/^# >>> dss:src-clone >>>/,/^# <<< dss:src-clone <<<$/p' "$SH")
GATE="$GATE
$(sed -n -e '/^# >>> dss:src-gate >>>$/,/^# <<< dss:src-gate <<<$/p' "$SH")"
if [ -z "$(printf '%s' "$GATE" | tr -d '\n ')" ]; then echo "FATAL: could not extract the src-gate block"; exit 1; fi
echo "extracted $(printf '%s\n' "$GATE" | wc -l) gate lines from the shipped script"

gate_run() {                   # gate_run <SRC_DIR> <REPO_URL> <BRANCH> <COMMIT> <ALLOW_CLONE>
  {
    echo 'set -Eeuo pipefail'
    # The driver installs an ERR trap (build-and-test.sh:451) and `set -E` carries it
    # into functions — reproduce it, or a fault that ABORTS the real run would merely
    # print here and the test would call the gate green.
    echo 'die()  { printf "DIE: %s\n" "$*"; exit 1; }'
    echo 'trap '\''die "ERR trap at line $LINENO: $BASH_COMMAND"'\'' ERR'
    echo 'step() { printf "STEP: %s\n" "$*"; }'
    echo 'info() { printf "INFO: %s\n" "$*"; }'
    echo 'pass() { printf "PASS: %s\n" "$*"; }'
    echo 'warn() { printf "WARN: %s\n" "$*"; }'
    printf '%s\n' "$PROV"
    printf '%s\n' "$GATE"
    echo 'printf "REACHED-END branch=[%s] head=[%s] diverge=[%s]\n" "$SRC_BRANCH" "$SRC_HEAD" "$SRC_DIVERGE"'
  } > "$TMPPROV"
  SRC_DIR="$1" DSS_REPO_URL="$2" DSS_BRANCH="$3" DSS_COMMIT="$4" DSS_ALLOW_FRESH_CLONE="$5" \
    "$BASH" "$TMPPROV" 2>&1 || true      # a `die` is an EXPECTED outcome here
}

echo "--- Step-2 source gate: the four shapes ---"
if ! command -v git >/dev/null 2>&1; then
  # 12 = the eleven check/check_eq calls in the else-arm plus its one inline
  # red-on-disable guard (the pre-fix empty wanted-branch clone).
  echo "  SKIP no git on PATH — the Step-2 gate was NOT exercised (12 assertions skipped)"
  skip=$((skip+12))
else
  GATETMP="$PROVTMP/gate"; mkdir -p "$GATETMP"
  # An ORIGIN with two branches: HEAD on the repo default (what a bare `git clone`
  # lands on — this is shape (a)'s "main"), plus a feature branch nobody reaches by
  # accident. Local path, so no network and no ssh key.
  ORIGIN="$GATETMP/origin"; mkdir -p "$ORIGIN"
  gitq init -q "$ORIGIN" >/dev/null 2>&1
  echo v1 > "$ORIGIN/f.c"
  gitq -C "$ORIGIN" add f.c >/dev/null 2>&1
  gitq -C "$ORIGIN" commit -q --no-verify -m seed >/dev/null 2>&1
  ORIGIN_DEFAULT="$(git -C "$ORIGIN" rev-parse --abbrev-ref HEAD)"   # read, never assumed
  gitq -C "$ORIGIN" checkout -q -b feature/probe >/dev/null 2>&1
  echo v2 >> "$ORIGIN/f.c"
  gitq -C "$ORIGIN" commit -q --no-verify -am feature >/dev/null 2>&1
  gitq -C "$ORIGIN" checkout -q "$ORIGIN_DEFAULT" >/dev/null 2>&1

  # (a) absent SRC_DIR, no opt-in -> REFUSE. This is the shape that used to spend
  #     hours validating main and then print main's hash as the verdict.
  A="$(gate_run "$GATETMP/nope" "$ORIGIN" "" "" "0")"
  check "(a) absent dir REFUSES instead of cloning" "will NOT clone one silently" "$A"
  check_eq "(a) the gate does not fall through"     ""  "$(printf '%s' "$A" | sed -n 's/.*\(REACHED-END\).*/\1/p')"

  # (c) populated, not a checkout — exactly what `rsync --exclude=/.git` produces.
  C_DIR="$GATETMP/rsynced"; mkdir -p "$C_DIR"; echo x > "$C_DIR/src.c"
  C_OUT="$(gate_run "$C_DIR" "$ORIGIN" "" "" "1")"     # even WITH the opt-in
  check "(c) populated non-checkout names the rsync scenario" "is NOT a git checkout" "$C_OUT"

  # (1)+(2) opt-in clone HONOURS DSS_BRANCH — the empty third argument was the bug.
  CL="$GATETMP/cloned"
  CL_OUT="$(gate_run "$CL" "$ORIGIN" "feature/probe" "" "1")"
  check "opt-in clone reaches the end"          "REACHED-END"           "$CL_OUT"
  check "opt-in clone lands on DSS_BRANCH"      "branch=[feature/probe]" "$CL_OUT"

  # RED-ON-DISABLE for the above: the PRE-FIX call passed "" as the wanted branch.
  # Show that "" really does land somewhere else, so the assertion above is not
  # just restating git's default behaviour.
  OLDCL="$GATETMP/cloned-oldway"
  git clone --quiet "$ORIGIN" "$OLDCL" >/dev/null 2>&1
  old_branch="$(git -C "$OLDCL" rev-parse --abbrev-ref HEAD)"
  if [ "$old_branch" != "feature/probe" ]; then
    echo "  ok   the pre-fix empty wanted-branch lands on '$old_branch', NOT the feature branch"
    pass=$((pass+1))
  else
    echo "  FAIL the guard proves nothing — a bare clone already lands on feature/probe"; fail=$((fail+1))
  fi

  # (b) a real checkout whose working tree has moved on — the stale-.git shape and
  #     the uncommitted-work shape, which are the same observation from inside.
  WT="$GATETMP/worktree"
  git clone --quiet "$ORIGIN" "$WT" >/dev/null 2>&1
  echo dirty >> "$WT/f.c"; echo new > "$WT/extra.c"
  B_OUT="$(gate_run "$WT" "$ORIGIN" "" "" "0")"
  check "(b) a dirty tree still RUNS (never a hard failure)" "REACHED-END" "$B_OUT"
  check "(b) the divergence is WARNED about"    "differ from HEAD"  "$B_OUT"
  check "(b) the banner carries the count"      "diverge=[2]"       "$B_OUT"

  # DSS_BRANCH / DSS_COMMIT: declared intent, asserted.
  BR_OUT="$(gate_run "$WT" "$ORIGIN" "not-this-branch" "" "0")"
  check "DSS_BRANCH mismatch DIES"              "DIE: DSS_BRANCH="  "$BR_OUT"
  wt_head="$(git -C "$WT" rev-parse HEAD)"
  OK_OUT="$(gate_run "$WT" "$ORIGIN" "" "$wt_head" "0")"
  check "DSS_COMMIT matching HEAD is verified"  "DSS_COMMIT verified" "$OK_OUT"
  BAD_OUT="$(gate_run "$WT" "$ORIGIN" "" "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef" "0")"
  check "DSS_COMMIT that is not here DIES"      "does not resolve to a commit" "$BAD_OUT"
fi

echo
echo "passed=$pass failed=$fail skipped=$skip"
# ── the accounting invariant, ENFORCED (see TOTAL_ASSERTIONS at the top) ─────
# This is deliberately checked AFTER the summary line, so the numbers a reader
# (and the driver's `sed`) needs are already on stdout when it fires.
accounted=$((pass + fail + skip))
if [ "$accounted" -ne "$TOTAL_ASSERTIONS" ]; then
  echo "  FAIL ACCOUNTING: $accounted assertion(s) accounted for, but this file declares TOTAL_ASSERTIONS=$TOTAL_ASSERTIONS"
  echo "       (drift of $((accounted - TOTAL_ASSERTIONS)); negative = fewer ran/were counted than declared)."
  echo "       Either an assertion was added or removed without updating TOTAL_ASSERTIONS, or a SKIP"
  echo "       branch's hand-written count no longer matches the block it stands in for. Both make this"
  echo "       self-test report a pass over assertions it never ran. Fix the count — never delete this check."
  exit 1
fi
[ "$fail" -eq 0 ]
