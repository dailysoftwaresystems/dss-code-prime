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
# Scratch for the loadext-staging section — created HERE, beside the others, and
# added to the ONE trap for the reason the comment above gives.
LOADTMP=$(mktemp -d /tmp/confload_XXXXXX)
trap 'rm -f "$TMPRUN" "$TMPPROV"; rm -rf "$PROVTMP" "$LOADTMP"' EXIT

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
TOTAL_ASSERTIONS=104     # 20 classifier + 14 provenance helpers + 12 Step-2 gate
                         # + 28 loadext staging + 6 staged sqlite_cfg.h (this driver)
                         # + 4 launcher argv form (this driver, 2 of them behavioural)
                         # + 12 driver-pairing (.ps1-gated)
                         # + 8 THE SUPPLY (leg_confound_patterns) — the half that
                         #   was never tested, and where the defect actually was
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
echo "=== THE LOADEXT HELPER: DSS builds it, the reference cc is a CONTROL, and no failure kills the run ==="
# TWO anchors, and this battery guards both halves.
#
# D-HARNESS-LOADEXT-HELPER-TARGET-BLINDNESS-NOW-ABORTS-THE-RUN. ✔MEASURED
# 2026-08-05 on a WSL/Ubuntu x86_64 host: two legs had already reported GREEN
# (331,351 and 331,355 units) when `stage_loadext_extension` failed to link the
# pe64 helper and `die`d. The run ENDED there — no Step 9, no ledger, no verdict
# for the two legs that had passed and none for the two never reached. The
# shipped function must RETURN on every failure path so the caller can record a
# NAMED verdict and CONTINUE.
#
# D-HARNESS-CROSS-HOST-ANY-TARGET. The helper is now emitted by DSS for the leg's
# declared `sharedLibFormat`, so no host needs a cross-compiler for any leg; the
# verified target compiler is an OPTIONAL CONTROL. The rc contract is what this
# driver translates, and it has TWO failure classes that must not be folded into
# one: 1 = poisoned (a REAL failure; must red the run) and 2 =
# skipped-build-input-missing (ENVIRONMENTAL; must NOT).
#
# ★ WHAT IS TESTED HERE vs IN harness_legs.py --self-test. The BUILD itself — the
# argv, the object format, whether the artefact is a loadable shared library,
# which arm is the control — lives in the shared resolver and is covered by its
# own self-test, which both drivers run as a refuse-to-start. (This line used to
# quote that battery's assertion COUNT; it was 1,187 when written and 1,606 when
# noticed. A number that only one of the two files knows how to update is a
# number that rots, so it is stated as a reference instead of a figure.)
# What is tested HERE is the DRIVER's half: that it calls the resolver with the
# paths only it knows, and that it turns one report into one verdict without
# dying. So the resolver is STUBBED below — that is the seam, deliberately, and
# it is why this file still runs on a machine with no compiler and no sqlite
# clone at all.
#
# Extracted and run the same way as the classifier above: at TOP LEVEL, under the
# driver's exact `set -Eeuo pipefail`, with `die` defined so that reintroducing one
# is VISIBLE (exit 9) instead of dying as "die: command not found".
STAGEBLK=$(sed -n -e '/^# >>> dss:loadext-stage >>>$/,/^# <<< dss:loadext-stage <<<$/p' "$SH")
VERDBLK=$(sed -n -e '/^  # >>> dss:loadext-verdict >>>$/,/^  # <<< dss:loadext-verdict <<<$/p' "$SH")
if [ -z "$STAGEBLK" ]; then echo "FATAL: could not extract the loadext-stage block"; exit 1; fi
if [ -z "$VERDBLK" ]; then echo "FATAL: could not extract the loadext-verdict block"; exit 1; fi
echo "extracted $(printf '%s\n' "$STAGEBLK" | wc -l) staging + $(printf '%s\n' "$VERDBLK" | wc -l) verdict lines from the shipped script"

# A stand-in tree. NOTHING here reads the real staged clone or the real resolver:
# this file is a refuse-to-start gate for the driver and must run on a machine
# that has never cloned sqlite and has no compiler.
LOAD_SQ="$LOADTMP/sqlite"; LOAD_BLD="$LOADTMP/bld"; LOAD_OUT="$LOADTMP/out"
mkdir -p "$LOAD_SQ/src" "$LOAD_BLD" "$LOAD_OUT"
printf 'int dss_selftest_helper(void) { return 42; }\n' > "$LOAD_SQ/src/test_loadext.c"

# THE STUB RESOLVER. It records the argv it was handed (so the battery can assert
# WHAT the driver asked for), then emits a canned report and the rc the caller
# chose. python3 is already a hard requirement of the driver this stands in for.
STUB="$LOADTMP/stub_resolver.py"
cat > "$STUB" <<'STUBPY'
import json, os, sys
open(os.environ["STUB_ARGV_LOG"], "w").write("\n".join(sys.argv[1:]))
rc = int(os.environ.get("STUB_RC", "0"))
sys.stdout.write(json.dumps({
    "verdictClass": os.environ.get("STUB_CLASS", ""),
    "detail": os.environ.get("STUB_DETAIL", "a detail line"),
    "crossCheck": os.environ.get("STUB_CROSS", ""),
    "staged": os.environ.get("STUB_STAGED", "/staged/libtestloadext.so"),
}) + "\n")
sys.exit(rc)
STUBPY
# A stub that emits something that is NOT JSON at all — the resolver's own FATAL
# line takes this shape, and a driver that reads it as an empty field would print
# a confident blank instead of the refusal.
STUB_FATAL="$LOADTMP/stub_fatal.py"
cat > "$STUB_FATAL" <<'STUBPY'
import os, sys
open(os.environ["STUB_ARGV_LOG"], "w").write("\n".join(sys.argv[1:]))
sys.stderr.write("harness_legs.py: FATAL: no leg labelled 'nope'\n")
sys.exit(2)
STUBPY

declare -A LEG_CC=(          [pe-shaped]="mingw-gcc"          [posix-shaped]=""                  )
declare -A LEG_CC_MACHINE=(  [pe-shaped]="x86_64-w64-mingw32" [posix-shaped]=""                  )
declare -A LEG_LOADEXT_NAME=([pe-shaped]="testloadext.dll"    [posix-shaped]="libtestloadext.so" )
declare -A LEG_SPEC=(        [pe-shaped]="x86_64:pe64-x86_64-windows-exec" [posix-shaped]="x86_64:elf64-x86_64-linux-exec" )

STUB_ARGV_LOG="$LOADTMP/argv.txt"
stage_run() {   # stage_run <leg> <rundir> <resolver> ; env: STUB_RC/STUB_CLASS/…
  {
    echo 'set -Eeuo pipefail'          # the driver's own options (build-and-test.sh:64)
    echo 'info() { echo "      INFO: $*"; }'
    # ★ THE RED-ON-DISABLE THAT MATTERS. If a future edit puts a `die` back into
    # the staging block, this prints DIE: and exits 9 — a distinct, asserted
    # outcome — instead of the block failing for the unrelated reason that `die`
    # is not defined out here.
    echo 'die()  { echo "DIE: $*"; exit 9; }'
    declare -p LEG_CC LEG_CC_MACHINE LEG_LOADEXT_NAME LEG_SPEC
    printf 'LEG_RESOLVER=%q\n'  "$3"
    printf 'LEG_CATALOGUE=%q\n' "$LOADTMP/legs.json"
    printf 'LOADEXT_BUILDER=%q\n' "${STUB_BUILDER:-dss}"
    printf 'DSS_BIN=%q\n'       "/nonexistent/dss-code-prime"
    printf 'DSS_CONFIG=%q\n'    "release"
    printf 'SQLITE_DIR=%q\n'    "$LOAD_SQ"
    printf 'BLD=%q\n'           "$LOAD_BLD"
    printf 'OUT_DIR=%q\n'       "$LOAD_OUT"
    echo 'SQLITE_TESTDIR_SUBDIR="testdir"'
    printf '%s\n' "$STAGEBLK"
    printf 'if stage_loadext_extension %q %q; then echo "RC=0"; else echo "RC=$?"; fi\n' "$1" "$2"
    echo 'echo "WHY=[$STAGE_WHY]"'
    echo 'echo "CROSS=[$STAGE_CROSSCHECK]"'
  } > "$TMPRUN"
  STUB_ARGV_LOG="$STUB_ARGV_LOG" "$BASH" "$TMPRUN" 2>&1
}
# The driver invokes `python3 "$LEG_RESOLVER" …`, so the stub is handed to it as
# the SCRIPT — no shebang, no chmod, and portable to a host whose python3 is
# somewhere unusual.

# ── THE PATH NAMESPACE THE RESOLVER ACTUALLY RECEIVES ────────────────────────
# D-TEST-CONFOUND-SCOPE-SH-CANNOT-RUN-UNDER-GIT-BASH. The driver hands the
# resolver POSIX paths; what python3 RECEIVES is whatever the exec boundary
# delivers. Under Git Bash / MSYS on Windows that boundary REWRITES every
# path-shaped argument on its way into a NATIVE python.exe, so the argv this
# battery reads back says C:/Users/…/Temp/confload_X/sqlite/src where the shell
# said /tmp/confload_X/sqlite/src — and the three path assertions below failed on
# a perfectly correct driver, which is why this file had never once been runnable
# on the Windows workstation.
#
# ✔MEASURED 2026-08-06. The rewriter is the MSYS runtime at the MSYS-process →
# native-.exe boundary: `type -a python3` resolves to a Windows app-exec alias and
# sys.executable is …\pythoncore-3.14-64\python.exe. It is NOT a property of the
# nesting — the outer shell's own `python3 …` and the inner `"$BASH" "$TMPRUN"`
# run produce byte-identical argv — nor of the argv position (bare or after an
# option), nor of whether the path exists.
#
# ⚠ MSYS_NO_PATHCONV=1 IS NOT THE FIX, in either shell. It is the SAME rewrite
# that makes the stub's own path openable, so with it set python.exe never starts:
# "can't open file 'C:\tmp\confload_X\stub_resolver.py'". MEASURED at both layers.
#
# So the namespace is MEASURED, not assumed: the EXPECTATION is sent through the
# same transport the driver's argument takes, and compared where the driver
# actually spoke. The assertion is not weakened — the needle still originates in
# this fixture's own knowledge of which directory it created, so a driver that
# passed the build root for the source root (or omitted the option) still reds.
# On Linux/macOS/WSL the transport is the identity and every needle is unchanged.
NS_PROBE="$LOADTMP/ns_probe.py"
cat > "$NS_PROBE" <<'NSPY'
import sys
sys.stdout.write(sys.argv[1])
NSPY
ns_path() {   # <a path THIS fixture owns> → that same path AS THE RESOLVER SEES IT
  local _seen
  _seen="$(python3 "$NS_PROBE" "$1" 2>/dev/null)"
  # ⚠ NEVER return empty, and never `exit` from here: every call site is a `$( )`,
  # so an exit would end only the substitution and hand `check` an EMPTY needle —
  # which every string contains, i.e. a silent PASS over an unmeasured assertion.
  # A probe that could not answer POISONS its assertion instead, unmistakably.
  if [ -z "$_seen" ]; then
    echo "      NS-PROBE FAILED for [$1] — poisoning the assertion rather than passing it" >&2
    printf '%s' "<<NS-PROBE-FAILED:$1>>"
    return
  fi
  printf '%s' "$_seen"
}
# Probe self-check, on a sentinel path this battery never asserts on (so the
# mapping can neither be derived from nor launder any real expectation): a
# transport that dropped the tail would quietly shorten every needle below into a
# weaker one that still matched. Checked ONCE, and fatal — the stubs are python
# too, so a python3 that cannot echo its own argv makes this whole section inert.
_NS_SENTINEL="$(ns_path "$LOADTMP/ns-sentinel")"
case "$_NS_SENTINEL" in
  *ns-sentinel) : ;;
  *) echo "FATAL: the path-namespace probe mangled its sentinel ([$_NS_SENTINEL]); the loadext path assertions would be measuring the wrong string"; exit 1 ;;
esac

echo "--- the driver calls the resolver with the paths only IT knows ---"
A=$(STUB_RC=0 STUB_STAGED="$LOAD_OUT/x/testdir/testloadext.dll" \
    stage_run pe-shaped "$LOAD_OUT/pe/run" "$STUB")
echo "$A" | sed 's/^/      /'
ARGV=$(cat "$STUB_ARGV_LOG")
check "it asks for THIS leg by label"          "--build-loadext-helper"  "$ARGV"
check "...passing the resolved builder"        "--helper-builder"        "$ARGV"
check "...the DSS binary"                      "--dss"                   "$ARGV"
check "...the sqlite src root (for sqlite3ext.h)" "$(ns_path "$LOAD_SQ/src")" "$ARGV"
check "...the BUILD root (for the generated sqlite3.h)" "$(ns_path "$LOAD_BLD")" "$ARGV"
check "...the run's testdir as the destination" "$(ns_path "$LOAD_OUT/pe/run/testdir")" "$ARGV"
check "...and the driver's own build config"   "release"                 "$ARGV"
# ★ THE CONTROL ARM IS PASSED, NOT ASSUMED — and it may be EMPTY.
check "the VERIFIED control compiler is passed through" "mingw-gcc"      "$ARGV"
check "...with the triple it reported"         "x86_64-w64-mingw32"      "$ARGV"
check "a staged helper RETURNS 0"              "RC=0"                    "$A"

echo "--- a leg with NO control compiler: the field is EMPTY, and the run proceeds ---"
B=$(STUB_RC=0 STUB_CROSS="NO CONTROL ON THIS HOST: nothing here targets it" \
    stage_run posix-shaped "$LOAD_OUT/posix/run" "$STUB")
echo "$B" | sed 's/^/      /'
check "an absent control does NOT stop the leg"      "RC=0"                    "$B"
check "...and the cross-check line says so out loud" "NO CONTROL ON THIS HOST" "$B"
# ★ RED-ON-DISABLE for the de-host-locking itself: `--reference-cc` must still be
# PASSED (empty), never omitted, or the resolver would fall back to its default.
check_eq "an empty control compiler is passed as an EMPTY ARGUMENT, not omitted" "1" \
  "$(grep -c -- '--reference-cc' "$STUB_ARGV_LOG")"

echo "--- a REAL failure (rc 3): poisoned, reported, and NOT fatal ---"
C=$(STUB_RC=3 STUB_CLASS=poisoned \
    STUB_DETAIL="the dss build FAILED: error[P0016] got quote include not found" \
    stage_run pe-shaped "$LOAD_OUT/pe/run2" "$STUB")
echo "$C" | sed 's/^/      /'
check "a failed helper build RETURNS 1 (it does NOT die)" "RC=1"      "$C"
check "...and carries the compiler's own diagnostic"      "P0016"     "$C"

echo "--- the operator asked for an arm this host lacks (rc 4): ENVIRONMENTAL ---"
D=$(STUB_RC=4 STUB_CLASS=skipped-build-input-missing STUB_BUILDER=reference \
    STUB_DETAIL="DSS_LOADEXT_HELPER=reference was requested, and no candidate" \
    stage_run posix-shaped "$LOAD_OUT/posix/run2" "$STUB")
echo "$D" | sed 's/^/      /'
# ★ THE VERDICT-CLASS CONTRACT. Folding this into rc 1 would red a run in which
# nothing is broken — the default builder would have staged the helper here.
check "a control arm this host cannot provide RETURNS 2, NOT 1" "RC=2" "$D"
check "...and says the DEFAULT would have worked"  "DSS_LOADEXT_HELPER=reference" "$D"

echo "--- output that is not a report at all: refused, and QUOTED ---"
E=$(STUB_RC=2 stage_run pe-shaped "$LOAD_OUT/pe/run3" "$STUB_FATAL")
echo "$E" | sed 's/^/      /'
check "an unreadable outcome is a FAILURE, never a quiet success" "RC=1" "$E"
check "...and the refusal quotes what it could not read"          "FATAL" "$E"

# ── STRUCTURAL: the shipped text itself, because "it returns" is only half ────
# The other half is that the CALLER turns that return into a named verdict and
# keeps going. Asserted against the shipped lines rather than by re-running a
# 4,800-line driver.
check_eq "the staging block contains NO die"  "" \
  "$(printf '%s\n' "$STAGEBLK" | grep -n '^\s*die \|[^_a-zA-Z]die "' | head -1)"
check_eq "the verdict block contains NO die"  "" \
  "$(printf '%s\n' "$VERDBLK"  | grep -n '^\s*die \|[^_a-zA-Z]die "' | head -1)"
check "the caller records the ledger's FAILURE class" 'LEG_VERDICT["$leg"]="poisoned"' "$VERDBLK"
check "the caller CONTINUES to the next leg"          "continue"                       "$VERDBLK"
check "the caller counts the degraded leg"            "STAGE_FAILS=\$((STAGE_FAILS + 1))" "$VERDBLK"
# ★ THE TWO CLASSES ARE KEPT APART BY THE CALLER TOO, and the environmental one
# must NOT feed the counter that reds the run.
check "the caller maps rc 2 to the ENVIRONMENTAL verdict" 'leg_marks_missing' "$VERDBLK"
check_eq "...and does NOT count it as a stage failure" "1" \
  "$(printf '%s\n' "$VERDBLK" | grep -c 'STAGE_FAILS=\$((STAGE_FAILS + 1))')"
# ★ THE errexit TRAP. `stage_loadext_extension …; rc=$?` would EXIT on rc 1/2
# before the class could be read — the shape that once shipped a whole classifier
# as dead code. The caller must use the `if`/`else rc=$?` form.
check "the caller captures the rc in a form errexit cannot swallow" \
  'if stage_loadext_extension "$leg" "$rundir"; then _stage_rc=0; else _stage_rc=$?; fi' "$VERDBLK"
check "Step 9 REDS the run on a degraded leg" \
  'if [[ "${STAGE_FAILS:-0}" -gt 0 ]]; then' "$(cat "$SH")"

# ── BOTH DRIVERS, OR THE CAPABILITY IS A SILENT HARNESS BUG ──────────────────
# D-HARNESS-PS1-STAGES-NO-LOADEXT-HELPER-COVERAGE-IS-UNDECLARED existed because
# build-and-test.sh staged a helper and build-and-test.ps1 staged none. The
# capability now lives in harness_legs.py precisely so it cannot be in one driver
# and not the other, and THIS is the assertion that keeps it that way.

# ── THE STAGED sqlite_cfg.h, IN THIS DRIVER ─────────────────────────────────
# D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES. The
# recipe's `_HAVE_SQLITE_CONFIG_H` makes sqliteInt.h include a `sqlite_cfg.h`, and
# the DERIVING host's build dir is on every include list — so unless this driver
# ASKS for a per-target one and puts it FIRST, every leg silently inherits this
# machine's ./configure answers. Both halves are asserted, because either one
# alone is inert: asking for the header and then listing it after $BLD stages a
# file nothing reads.
SHTXT="$(cat "$SH")"
# ★★ A CODE-SHAPE ASSERTION MUST NOT BE SATISFIABLE BY A COMMENT, and one of the
# ones below was. ✔MEASURED 2026-08-05 (TF-C121): the first cut of the `rm -f
# "$BLD/sqlite_cfg.h"` assertion matched against the WHOLE file text, and the
# explanatory comment written beside the fix QUOTES that exact line — so deleting
# the real `rm -f` and re-running still reported `ok`, 96/0. A guard whose own
# prose satisfies it proves nothing, which is the defect class this entire file
# exists to refuse, committed by this file.
# So: every assertion about the SHAPE OF THE CODE runs against a comment-stripped
# view. Assertions about DIAGNOSTIC TEXT keep using $SHTXT — a die message is code,
# and asserting it survives comment-stripping anyway.
SHCODE="$(grep -v '^[[:space:]]*#' "$SH")"
check "the .sh asks stage-zinc.py for a per-target sqlite_cfg.h" \
      "--sqlite-cfg-h" "$SHCODE"
check "...writing it into its own cfg/ root" "--cfg-dest" "$SHCODE"
check "...and REFUSES a run in which none was produced" \
      "produced NO per-target sqlite_cfg.h" "$SHTXT"
check "...with that dir FIRST on the fixture include list" \
      '"${CFG_STAGE_DIR[$_c]}" "${INC_DIRS_HEAD[@]}"' "$SHCODE"
check "...and FIRST on the CLI include list" \
      '"${CFG_STAGE_DIR[$_c]}" "${CLI_INCS[@]}"' "$SHCODE"
# ★ AND THE DERIVING HOST'S OWN COPY IS REMOVED, which is the OTHER half.
# A quote include searches the INCLUDING FILE'S OWN DIRECTORY before this list is
# consulted at all, so `$BLD/ctime.c` — TU #1 of BOTH artefacts — read the deriving
# host's `sqlite_cfg.h` out of its own directory no matter what the include list
# said, and then `#define SQLITECONFIG_H 1` shadowed sqliteInt.h's include for the
# rest of that TU. The list position alone therefore did NOT close the anchor.
# RED-ON-DISABLE: delete the `rm -f` line and this goes red — VERIFIED by doing
# exactly that. Matched against $SHCODE, never $SHTXT: the comment beside the fix
# quotes this line verbatim, and against the full text the assertion passed on a
# driver from which the real line had been deleted.
check "...and the DERIVING host's copy is REMOVED from the staged bld dir" \
      'rm -f "$BLD/sqlite_cfg.h"' "$SHCODE"

# ── THE LAUNCHER ARGV FORM: `--launcher=<tok>`, NEVER `--launcher <tok>` ─────
# D-HARNESS-DASH-LEADING-LAUNCHER-TOKEN-MISPARSED-AS-AN-OPTION.
#
# A launcher TOKEN may itself begin with a dash, and argparse then refuses the
# SPACE form with "expected one argument" instead of taking the next word as the
# value. ✔MEASURED 2026-08-05 (TF-C121): that killed the pe64 CLI smoke gate before
# a single assertion ran, and the caller classified the harness's own argv defect
# as `smoke: FAIL — CHARGED TO DSS` — the harness accusing the compiler.
#
# ★ PINNED WITH A DASH-LEADING TOKEN ON PURPOSE. Every launcher token the harness
# has ever actually run starts with a LETTER (`wine`, `qemu-aarch64`, `qemu-x86_64`,
# `wsl.exe`), and against those the two forms behave identically — so a test written
# with one of them would pass on the broken code and prove nothing. `arch -x86_64`
# is what legs.json DECLARES for macho64-x86_64 on a darwin/arm64 host, i.e. the
# real token that would fire this the first time that leg runs on the operator's
# Mac. Using the declared string means this test cannot rot into a letter-initial
# token that passes either way.
check "the .sh emits the \`=\` form to cli-smoke.py" '--launcher=$_t' "$SHCODE"
# $SHCODE, not $SHTXT: the comment beside the fix spells the broken form out
# (`NOT --launcher <tok>`) as the thing being warned against, so against the full
# text this assertion would red on a CORRECT driver.
check_eq "...and never the space form" "" \
  "$(printf '%s\n' "$SHCODE" | grep -n -- '--launcher "\|--launcher \$\|--launcher [a-z]\|--reference-launcher "\|--reference-launcher \$\|--reference-launcher [a-z-]' | head -1)"
# ★ BEHAVIOURAL RED-ON-DISABLE, against the REAL shipped cli-smoke.py parser — not
# a restatement of the source text above. Both invocations carry the SAME two
# tokens (`arch`, `-x86_64`); only the argv FORM differs. Nothing is built, no
# binary is run: argparse rejects or accepts the tokens long before any work.
_LAUNCH_SPACE="$(python3 "$(dirname "$SH")/cli-smoke.py" --launcher arch --launcher -x86_64 2>&1 || true)"
_LAUNCH_EQ="$(python3 "$(dirname "$SH")/cli-smoke.py" --launcher=arch --launcher=-x86_64 2>&1 || true)"
check "RED-ON-DISABLE: the SPACE form with \`arch -x86_64\` is REFUSED by argparse" \
      "argument --launcher: expected one argument" "$_LAUNCH_SPACE"
# The `=` form must get PAST token consumption — asserted POSITIVELY, by the error
# it reaches instead (the missing required arguments), not merely by the absence of
# the other one. "It did not say X" is satisfied by a tool that says nothing at all.
check "...while the \`=\` form consumes BOTH tokens and reaches the required-arg check" \
      "the following arguments are required: --cli" "$_LAUNCH_EQ"

# ── BOTH DRIVERS, OR THE CAPABILITY IS A SILENT HARNESS BUG ──────────────────
# D-HARNESS-PS1-STAGES-NO-LOADEXT-HELPER-COVERAGE-IS-UNDECLARED existed because
# build-and-test.sh staged a helper and build-and-test.ps1 staged none. The
# capability now lives in harness_legs.py precisely so it cannot be in one driver
# and not the other, and THIS is the assertion that keeps it that way.
PS1="$(dirname "$SH")/build-and-test.ps1"
if [ ! -f "$PS1" ]; then
  echo "  SKIP build-and-test.ps1 not found beside the .sh — 12 pairing assertions not run"
  skip=$((skip+12))
else
  PS1TXT="$(cat "$PS1")"
  # Same comment-stripped view as $SHCODE above, and for the same measured reason.
  PS1CODE="$(grep -v '^[[:space:]]*#' "$PS1")"
  check "the .ps1 stages a helper too"                "--build-loadext-helper" "$PS1TXT"
  check "...through the SAME shared resolver"         "Resolve-LoadextHelper"  "$PS1TXT"
  check "...and honours the same operator switch"     "--loadext-builder"      "$PS1TXT"
  check "...mapping the ENVIRONMENTAL class apart from the failure one" \
        "skipped-build-input-missing" "$PS1TXT"
  # The same four-way pairing for the staged sqlite_cfg.h. A capability in one
  # driver and not the other is the silent harness bug this block is named for,
  # and this one would be INVISIBLE on the .ps1 side: the leg would build, and
  # build wrong.
  check "the .ps1 asks for the per-target sqlite_cfg.h too" "--sqlite-cfg-h" "$PS1TXT"
  check "...writing it into its own cfg/ root"              "--cfg-dest"     "$PS1TXT"
  check "...and REFUSES a run in which none was produced" \
        "produced NO per-target sqlite_cfg.h" "$PS1TXT"
  check "...with that dir FIRST on its include lists" \
        "@((\$CfgStageDirs[\$c] -replace '\\\\','/')) + @(\$IncBase)" "$PS1CODE"
  check "...and REMOVES the deriving host's copy, exactly as the .sh does" \
        'Remove-Item -LiteralPath $DerivedCfgH -Force' "$PS1CODE"
  # ── AND THE LAUNCHER ARGV FORM, IN BOTH DRIVERS ────────────────────────────
  # This is the pairing that was NOT there: the fix for
  # D-HARNESS-DASH-LEADING-LAUNCHER-TOKEN-MISPARSED-AS-AN-OPTION landed in both
  # drivers with prose only and no guard at all, so either one could silently
  # regress to the space form. The .ps1 is where it was MEASURED to bite (its
  # `--reference-launcher -e` killed the pe64 CLI smoke gate), which is why the
  # sibling option is asserted here by name and not folded into the one above.
  check "the .ps1 emits the \`=\` form too"                 '--launcher=$t'            "$PS1CODE"
  check "...including the sibling that actually broke"      '--reference-launcher=-e'  "$PS1CODE"
  # ★ THE .ps1's SPLIT SHAPE IS NOT A SPACE. ✔MEASURED while demonstrating this
  # guard: reverting the .ps1 to `@("--launcher", "$t")` produces NO literal
  # `--launcher ` anywhere, because PowerShell passes the option and its value as
  # two ARRAY ELEMENTS. A space-only pattern therefore stayed green on a driver
  # that had been reverted. The regression shape to forbid here is the option
  # string CLOSED and followed by a comma.
  check_eq "...and never the space form (nor the .ps1's array-split shape)" "" \
    "$(printf '%s\n' "$PS1CODE" | grep -n -E -- "--(reference-)?launcher( ['\"\$a-z]|['\"][[:space:]]*,)" | head -1)"
fi

# ═══════════════════════════════════════════════════════════════════════════
# THE SUPPLY — WHERE THAT PATTERN ARRAY CAME FROM
# ═══════════════════════════════════════════════════════════════════════════
# ★★★ THIS SECTION EXISTS BECAUSE EVERYTHING ABOVE IT IS ABOUT THE MATCHER, AND
# THE DEFECT WAS IN THE SUPPLY.
#
# Every assertion above sets `CONFOUND_PATTERNS=(...)` BY HAND and then exercises
# the shipped classifier over it. That pinned the matching in real detail — scope
# prefixes, permutation prefixes, the `$` anchor, family resemblance — while the
# question "where does that array COME FROM" was asked by NOTHING, in EITHER
# driver. So for months the .sh applied one global list to every leg and the .ps1
# returned a Linux-earned list for pe64 and NOTHING for the legs that had earned
# it, and the whole battery stayed green over both.
# ⇒ D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG. The same shape as
# "the shipped vocabulary read" in test-driver-contracts.sh: a pin that supplies
# its subject's input by hand is testing the stub.
#
# EXTRACTED AND RUN, never re-implemented — same discipline as the classifier.
echo "--- THE SUPPLY: leg_confound_patterns, extracted from the shipped driver ---"
SUPPLY=$(sed -n -e '/^# >>> dss:confound-supply >>>$/,/^# <<< dss:confound-supply <<<$/p' "$SH")
if [ -z "$SUPPLY" ]; then
  echo "  FAIL could not extract the dss:confound-supply region — those sentinels are a CONTRACT with this file"
  fail=$((fail+1))
  # The 8 assertions below cannot run; count them so the accounting invariant
  # cannot be satisfied by silently losing them.
  skip=$((skip+8))
else
  declare -A LEG_CONFOUNDS=(
    [elf64-x86_64]="'^walsetlk-' '^busy2-' '^zipfile-25\\.0\$'"
    [pe64-x86_64]=""
    [elf64-arm64]="'^busy2-' 'emulated:^writecrash-'"
  )
  DSS_CONFOUNDS=""
  # `die` stubbed so the shipped refusal is a CATCHABLE outcome: the last two
  # assertions are about the driver REFUSING, and a refusal that killed this
  # runner could not be asserted about. ⚠ `exit`, NOT `return` — mirroring
  # test-driver-contracts.sh's stub, and it is load-bearing: with `return` the
  # shipped function CONTINUES past its refusal to the `printf` below it, so what
  # the assertion measured was the NEXT statement's status (an unbound-key error
  # under `set -u`, rc 1) rather than the refusal. Every call below is inside a
  # `$( )` subshell, so the exit ends that subshell and nothing else. 97 is
  # arbitrary and only ever compared against itself.
  die() { printf 'DIE: %s\n' "$*"; exit 97; }
  eval "$SUPPLY"
  declare -a SUP=()
  eval "SUP=($(leg_confound_patterns elf64-x86_64))"
  check_eq "a leg gets ITS OWN declared patterns"     "^walsetlk- ^busy2- ^zipfile-25\\.0\$" "${SUP[*]}"
  eval "SUP=($(leg_confound_patterns elf64-arm64))"
  check_eq "...a DIFFERENT leg gets a DIFFERENT set"  "^busy2- emulated:^writecrash-"        "${SUP[*]}"
  # ★ THE HEADLINE ASSERTION. A leg whose catalogue entry declares `[]` must come
  # back EMPTY — never inheriting a sibling's list. This is exactly the direction
  # the old global `DSS_CONFOUNDS` got wrong.
  eval "SUP=($(leg_confound_patterns pe64-x86_64))"
  check_eq "a leg declaring [] inherits NOTHING"      ""                                     "${SUP[*]}"
  # The scope prefix must survive the supply: if it were stripped here, the
  # qemu-only writecrash excusal would silently become a bare one and suppress a
  # future genuine regression on a native run — which every assertion in the
  # classifier section above would still call correct.
  eval "SUP=($(leg_confound_patterns elf64-arm64))"
  # ⚠ NO BACKTICKS IN THIS LABEL: it is a DOUBLE-quoted bash string, so `emulated:`
  # would be command substitution, and bash duly reported `emulated:: command not
  # found` and ran the assertion against a mangled label. Measured while writing.
  check "the 'emulated:' scope survives the supply"   "emulated:^writecrash-"                "${SUP[*]}"
  # THE OPERATOR OVERRIDE still applies to EVERY leg — stating intent, not
  # inheriting one — including to a leg that declares nothing.
  DSS_CONFOUNDS='^operator-1 ^operator-2'
  eval "SUP=($(leg_confound_patterns pe64-x86_64))"
  check_eq "the operator override reaches EVERY leg"  "^operator-1 ^operator-2"              "${SUP[*]}"
  eval "SUP=($(leg_confound_patterns elf64-x86_64))"
  check_eq "...and REPLACES the earned set, never merges" "^operator-1 ^operator-2"          "${SUP[*]}"
  DSS_CONFOUNDS=""
  # ⚠ AN UNDECLARED LEG IS A TRANSPORT DEFECT, NOT AN EMPTY LIST. harness_legs.py
  # refuses to plan a leg with no `confounds`, so an absent entry here means the
  # plan this driver eval'd is not the plan that file produces — and answering
  # `[]` would report every failure on that leg as a DSS defect on the strength
  # of a bug.
  SUPOUT="$(leg_confound_patterns macho64-arm64 2>&1)"; SUPRC=$?
  check_eq "an UNDECLARED leg REFUSES rather than answering []" "97" "$SUPRC"
  check "...naming the reason"  "transport" "$SUPOUT"
  unset -f die
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
