#!/usr/bin/env bash
# Verifies build-and-test.sh's LEG-CONTRACT logic by EXTRACTING the shipped
# functions and RUNNING them — never by re-implementing them here. A copy would
# stay green while the shipped logic was broken, which is the inert-test trap
# test-confound-scope.sh (this file's sibling and model) was written for.
#
# WHAT IT PINS, and the anchor each one belongs to:
#   A  unit_not_run + unit_verdict_token_known  an unclassified skip is impossible
#   B  leg_run_is_skipped                       ONE run decision, both artifacts
#   C  the Step-8 gate sequence, executed       a leg with NO control compiler
#                                               still REACHES the corpus
#      [D-HARNESS-UNITS-SKIP-A-LEG-WHOSE-LAUNCHER-IT-SAYS-IS-AVAILABLE]
#   D  parse_segment                            the FIRST DIAGNOSTIC line is kept
#   E  the precondition discriminator           zero progress + same error stops;
#                                               everything else still RESUMES
#      [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY]
#   F  acq_field                                the acquisition contract fields
#   G  leg_loader_path_var                      the loader variable is TARGET-keyed
#      [D-HARNESS-RUN-ENV-LD-LIBRARY-PATH-INERT-ON-DARWIN]
#   H  RED-ON-DISABLE                           every guard above is BROKEN in a
#                                               copy and the pin MUST go red
#
# ★★★ WHY SECTION H EXISTS AND WHY IT ASSERTS ITS OWN MUTATIONS.
# Red-on-disable is this project's primary defence against vacuous tests. On
# 2026-08-06 a mutator SILENTLY NO-OPPED (a cygwin fork failure killed it) and the
# pin reported GREEN over a file it had never modified — i.e. the demonstration
# said "I go red when the guard is removed" having never removed anything. That is
# the same family as `nm` on a file that no longer existed and a smoke gate scoring
# argparse's exit-2 as a compiler failure: AN INSTRUMENT REPORTING SUCCESS OVER
# SOMETHING IT COULD NOT OBSERVE — but aimed at the verification method itself,
# which makes it the worst of the set.
# So every mutation here is fail-closed on THREE checks before the pin is re-run:
#   (1) the mutant DIFFERS from the driver, byte-wise (`cmp`, not a line count:
#       a REPLACEMENT keeps the line count identical);
#   (2) a named WITNESS string present in the driver is ABSENT from the mutant, so
#       "something changed" can never stand in for "the guard was removed";
#   (3) the mutant still PARSES (`bash -n`), so a red can never be a syntax error
#       wearing the costume of a red-on-disable.
# Any of the three failing is a FAILED assertion, never a skipped one.

# ── bash 4+ required (`declare -A`) — macOS ships 3.2 ────────────────────────
# D-HARNESS-SELFTEST-BSD-SED-PORTABILITY, mirroring test-confound-scope.sh: this
# file is also run STANDALONE, and under bash 3.2 it would die at the first
# `declare -A` having run ZERO assertions while still exiting 0. A guard that
# reports success having proven nothing is worse than one that fails.
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
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SH="$HERE/build-and-test.sh"
LEGS_PY="$HERE/harness_legs.py"
CATALOGUE="$HERE/legs.json"
[ -f "$SH" ] || { echo "FATAL: $SH not found"; exit 1; }

WORK="$(mktemp -d 2>/dev/null || mktemp -d -t dsspin)"
# Trailing X's only + a trap: BSD/macOS mktemp substitutes X's only at the END of
# a template, the same portability note test-confound-scope.sh records.
trap 'rm -rf "$WORK"' EXIT

PASSED=0; FAILED=0; SKIPPED=0
PIN_FAILS=0          # per-pin, reset by green()/red()
QUIET=0              # 1 while a pin is being run against a MUTANT (its failures
                     # are the expected outcome and must not read as errors)

say()  { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"; }
ok()   { PASSED=$((PASSED + 1)); say "  ok   $1"; }
bad()  { FAILED=$((FAILED + 1)); printf '  FAIL %s\n' "$1"; }
skip() { SKIPPED=$((SKIPPED + 1)); printf '  skip %s\n' "$1"; }
# ck is what every pin asserts through. Inside a pin it bumps PIN_FAILS; that
# counter — not the global one — is what green()/red() judge.
ck()   { # ck <label> <expected> <actual>
  if [ "$2" = "$3" ]; then
    [ "$QUIET" -eq 1 ] && PASSED=$((PASSED + 1)) || ok "$1"
  else
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || { bad "$1"; printf '         expected: [%s]\n' "$2"; printf '         actual  : [%s]\n' "$3"; }
  fi
}
ck_has() { # ck_has <label> <haystack> <needle>
  case "$2" in
    *"$3"*) [ "$QUIET" -eq 1 ] && PASSED=$((PASSED + 1)) || ok "$1" ;;
    *) PIN_FAILS=$((PIN_FAILS + 1))
       [ "$QUIET" -eq 1 ] || { bad "$1"; printf '         [%s] does not contain [%s]\n' "$2" "$3"; } ;;
  esac
}

# ── the extractor ───────────────────────────────────────────────────────────
# ⚠ index($0,n)==1, NOT a dynamic regex. ✔MEASURED 2026-08-06: `$0 ~ "^" n
# "\\(\\) \\{"` makes this awk warn `escape sequence \( treated as plain (` and
# then MATCH NOTHING — the ERE reads `(` as a group opener — so the extractor
# yielded an EMPTY function and the pin asserted over code it had never loaded.
# A literal prefix test has no dialect dependency and no regex at all.
# The `first && /\}$/` arm handles a ONE-LINE function (`fact() { …; }`), which
# has no lone `}` line and would otherwise extract to end-of-file.
extract_fn() { # extract_fn <file> <name>
  LC_ALL=C awk -v n="$2() {" '
    index($0, n) == 1 { inb = 1; first = 1 }
    inb { print; if (first && /\}$/) exit; first = 0 }
    inb && /^\}$/ { exit }' "$1"
}
load_fns() { # load_fns <driver> <name...>  -> 0, or 1 having said which failed
  local drv="$1"; shift
  local fn txt
  for fn in "$@"; do
    txt="$(extract_fn "$drv" "$fn")"
    if [ -z "$txt" ]; then
      PIN_FAILS=$((PIN_FAILS + 1))
      [ "$QUIET" -eq 1 ] || bad "could not extract $fn from ${drv##*/} — this pin would assert over nothing"
      return 1
    fi
    eval "$txt" || { PIN_FAILS=$((PIN_FAILS + 1)); return 1; }
  done
  return 0
}

# ── the driver's ambient state, stubbed ─────────────────────────────────────
declare -A UNIT_VERDICT=() LEG_VERDICT=() LEG_VERDICT_DETAIL=() LEG_RUN_MODE=() \
           LEG_LAUNCH=() LEG_TCL_LIB=() COMPILE_OK=() FIXTURE=() LEG_CC=() \
           LEG_RUN_VERDICT=() LEG_RUN_DETAIL=() LEG_FORMAT=() LEG_CONFIG_STAGE_KEY=() \
           LEG_SPEC=() LEG_LIB_PROVIDER=()
# ── EVERY leg-plan array the resolver emits, DECLARED FROM THE DRIVER'S OWN LINE ─
# `emit_sh` writes `LEG_ARCH[elf64-x86_64]=x86_64` and its 25 siblings, and its
# docstring states the precondition: "The caller has already `declare -A`'d every
# array." An array NOT declared associative is INDEXED, so bash evaluates the
# subscript as ARITHMETIC — `elf64-x86_64` becomes the unset variable `elf64`,
# `set -u` makes that FATAL, and the whole eval takes the shell down with it.
# ✔MEASURED here: section G died mid-plan with no output at all, which read as a
# hung pin rather than a missing declaration.
# The list is EXTRACTED from the driver rather than retyped, so a 26th array added
# there cannot leave this pin silently short of one. TOP LEVEL, because `declare`
# inside a function is local.
DECLS="$(LC_ALL=C awk '/^declare -A LEG_/ { p = 1 } p { print; if ($0 !~ /\\$/) p = 0 }' "$SH")"
if [ -n "$DECLS" ]; then eval "$DECLS"; fi
declare -a UNIT_SKIP_VOCAB=()
UNIT_UNCLASSIFIED=0
declare -a UNIT_UNCLASSIFIED_LEGS=()
LEG_RESOLVER="$LEGS_PY"
WARNINGS=""
warn() { WARNINGS="$WARNINGS$*"$'\n'; }
info() { :; }
# `die` must not exit THIS shell: several pins assert that the shipped code
# refuses, and a refusal that killed the test runner could not be asserted about.
# 97 is arbitrary and only ever compared against itself.
die()  { printf 'DIE: %s\n' "$*"; exit 97; }

# ── THE CLOSED VOCABULARY: THE DRIVER'S OWN READ, EXTRACTED AND EXECUTED ────
# ★★ NOT A LITERAL LIST, AND NOT A RE-IMPLEMENTATION OF THE READ EITHER — this
# block is lifted from build-and-test.sh between its `dss:verdict-vocabulary`
# markers and executed here. THAT DISTINCTION IS THE WHOLE LESSON: the first
# version of this pin stubbed UNIT_SKIP_VOCAB with a hand-typed list of the eight
# tokens, which was clean by construction — and so it could not see that on
# Windows the shipped read stores every token with a trailing CR (Python writes
# stdout in text mode), which made the driver reject EVERY legitimate token. A pin
# that supplies its subject's input in a shape the subject never sees is testing
# the stub. Running the shipped read means the pin fails on the host where the
# read is wrong, which is the only place it matters.
# ⚠ EXTRACTED TO A VARIABLE AND EVAL'D AT TOP LEVEL — never inside a helper
# function. The shipped block contains `declare -a UNIT_SKIP_VOCAB=()`, and
# `declare` inside a function makes the array LOCAL: the block would run, populate
# it, and discard it on return, leaving the pin to report "the vocabulary is
# empty" about code that works. ✔MEASURED here while writing this file. It is the
# same SCOPE-FIDELITY trap test-confound-scope.sh records for the classifier
# (a top-level `local` that was legal inside the test's wrapper and fatal in the
# driver) — the shipped code must run in the scope it ships in.
VOCAB_BLOCK="$(LC_ALL=C awk '
  /^# >>> dss:verdict-vocabulary >>>$/ { inb = 1; next }
  /^# <<< dss:verdict-vocabulary <<<$/ { inb = 0 }
  inb { print }' "$SH")"
VOCAB_SOURCE="the driver's own dss:verdict-vocabulary block"
if ! command -v python3 >/dev/null 2>&1 || [ ! -f "$LEGS_PY" ] || [ ! -f "$CATALOGUE" ]; then
  VOCAB_SOURCE="unavailable (no python3 / harness_legs.py / legs.json)"
elif [ -z "$VOCAB_BLOCK" ]; then
  VOCAB_SOURCE="unavailable (the dss:verdict-vocabulary markers are not in the driver)"
else
  # `die` is stubbed above, so the block's own refusal cannot kill this runner.
  LEG_CATALOGUE="$CATALOGUE"
  eval "$VOCAB_BLOCK"
  [ "${#UNIT_SKIP_VOCAB[@]}" -gt 0 ] || VOCAB_SOURCE="unavailable (the block ran but produced no tokens)"
fi
# ★ AND THE TOKENS ARE ASSERTED CLEAN, not merely counted. A token carrying a
# stray CR (or any whitespace) is the defect above; counting eight of them would
# have reported success over it.
assert_vocab_clean() {
  local t bad_t=""
  for t in ${UNIT_SKIP_VOCAB[@]+"${UNIT_SKIP_VOCAB[@]}"}; do
    case "$t" in *[$'\r\n\t ']*) bad_t="$bad_t [$(printf '%q' "$t")]" ;; esac
  done
  if [ -n "$bad_t" ]; then
    bad "the shipped vocabulary read produced token(s) with stray whitespace:$bad_t
         Every classified not-run would be rejected as a HARNESS DEFECT. On Windows this is
         Python's text-mode stdout (\\n -> \\r\\n) surviving \`read -r\`."
  else
    ok "the shipped vocabulary read produces ${#UNIT_SKIP_VOCAB[@]} CLEAN token(s)"
  fi
}

# ── green / red drivers ─────────────────────────────────────────────────────
green() { # green <label> <pin-fn>   — the pin must be FULLY green against the driver
  PIN_FAILS=0; QUIET=0
  printf -- '-- %s\n' "$1"
  "$2" "$SH"
  if [ "$PIN_FAILS" -eq 0 ]; then :; else bad "$1 — $PIN_FAILS check(s) failed against the SHIPPED driver"; fi
}
red() {   # red <label> <pin-fn> <mutant>  — the pin MUST go red against the mutant
  PIN_FAILS=0; QUIET=1
  "$2" "$3"
  QUIET=0
  if [ "$PIN_FAILS" -gt 0 ]; then
    ok "$1 — red-on-disable CONFIRMED ($PIN_FAILS check(s) went red)"
  else
    bad "$1 — VACUOUS: the pin stayed GREEN against a driver whose guard was REMOVED.
         Either the pin does not actually exercise the guard, or the guard is not what makes it pass."
  fi
}

# ── the fail-closed mutator ─────────────────────────────────────────────────
# See the header. THREE checks, all fail-closed, before a mutant may be used.
mutate() { # mutate <label> <outfile> <witness> <awk-program>
  local label="$1" out="$2" witness="$3" prog="$4" n
  # (0) THE WITNESS MUST BE UNIQUE. A string that occurs twice survives its own
  #     removal — the mutation lands, check (2) sees the OTHER occurrence, and the
  #     harness reports "the guard is still present" about a guard that is gone.
  #     ✔MEASURED while writing this: `HARNESS DEFECT` also appears in a comment
  #     and in the Step-9 exit block, and H1 failed on exactly that.
  n="$(LC_ALL=C grep -cF -- "$witness" "$SH")"
  if [ "$n" != "1" ]; then
    bad "$label — the WITNESS occurs $n time(s) in the shipped driver, needs exactly 1: [$witness]
         A non-unique witness cannot tell 'the guard survived' from 'a copy of the text survived',
         and a missing one targets code that no longer exists. Either way this would prove nothing."
    return 1
  fi
  LC_ALL=C awk "$prog" "$SH" > "$out"
  # (1) it actually changed something. `cmp`, never a line count: a REPLACEMENT
  #     leaves the count identical, and that is exactly how a no-op mutation
  #     passes for a real one.
  if cmp -s "$SH" "$out"; then
    bad "$label — THE MUTATION DID NOT LAND: the mutant is byte-identical to the driver.
         Refusing to report a red-on-disable over an unmodified file."
    return 1
  fi
  # (2) it removed THE THING, not merely something.
  if LC_ALL=C grep -qF -- "$witness" "$out"; then
    bad "$label — the mutation changed the file but the WITNESS survives: [$witness]
         Something else was edited; the guard under test is still present."
    return 1
  fi
  # (3) the mutant is still a valid script, so a red cannot be a syntax error in
  #     disguise. ✔MEASURED: an earlier mutation left a dangling awk `} }` and the
  #     pin went red for a reason that had nothing to do with the guard.
  if ! bash -n "$out" 2>/dev/null; then
    bad "$label — the mutant does not PARSE, so any red it produces would be a syntax
         error rather than the missing guard. Narrow the mutation."
    return 1
  fi
  return 0
}

# ═══════════════════════════════════════════════════════════════════════════
# A + B — the not-run recorder and the shared run decision
# ═══════════════════════════════════════════════════════════════════════════
pin_verdicts() { # pin_verdicts <driver>
  local drv="$1"
  UNIT_VERDICT=(); LEG_VERDICT=(); LEG_VERDICT_DETAIL=()
  UNIT_UNCLASSIFIED=0; UNIT_UNCLASSIFIED_LEGS=(); WARNINGS=""
  load_fns "$drv" unit_verdict_token_known unit_not_run leg_run_is_skipped || return 0

  unit_not_run alpha "skipped-by-runOn" "runOn excludes this host"
  ck "a CLASSIFIED not-run is recorded verbatim" \
     "not run [skipped-by-runOn] — runOn excludes this host" "${UNIT_VERDICT[alpha]:-}"
  ck "…and costs no unclassified count" "0" "$UNIT_UNCLASSIFIED"

  # THE MEASURED DEFECT, reproduced: on the operator's Mac at 11e97e0e the units
  # line read `not run []` while the same sentence said the launcher was present.
  # The empty token is the seeded run verdict of a `launched` leg.
  LEG_RUN_MODE[macho64-x86_64]="launched"; LEG_LAUNCH[macho64-x86_64]="arch -x86_64"
  unit_not_run macho64-x86_64 "" \
    "host darwin/arm64 cannot run x86_64:macho64-x86_64-darwin-exec natively; declared launcher 'arch -x86_64' is available"
  case "${UNIT_VERDICT[macho64-x86_64]:-}" in
    "not run [] — "*)
      PIN_FAILS=$((PIN_FAILS + 1))
      [ "$QUIET" -eq 1 ] || bad "the EMPTY token was written through: ${UNIT_VERDICT[macho64-x86_64]}" ;;
    "not run [poisoned] — HARNESS DEFECT: "*)
      [ "$QUIET" -eq 1 ] && PASSED=$((PASSED + 1)) || ok "an EMPTY token becomes a classified, loud poisoned verdict" ;;
    *) PIN_FAILS=$((PIN_FAILS + 1))
       [ "$QUIET" -eq 1 ] || bad "unexpected: ${UNIT_VERDICT[macho64-x86_64]:-<unset>}" ;;
  esac
  ck "…is counted"                         "1" "$UNIT_UNCLASSIFIED"
  ck "…and named"                          "macho64-x86_64" "${UNIT_UNCLASSIFIED_LEGS[*]:-}"
  ck "…and the LEG verdict is in the closed vocabulary" "poisoned" "${LEG_VERDICT[macho64-x86_64]:-}"
  ck_has "…and it warns loudly"            "$WARNINGS" "HARNESS DEFECT"
  ck_has "…naming the resolved run plan"   "$WARNINGS" "declared launcher 'arch -x86_64'"

  unit_not_run beta "skipped-because-i-said-so" "made up"
  case "${UNIT_VERDICT[beta]:-}" in
    "not run [poisoned] — HARNESS DEFECT: "*)
      [ "$QUIET" -eq 1 ] && PASSED=$((PASSED + 1)) || ok "a token OUTSIDE the closed vocabulary is refused" ;;
    *) PIN_FAILS=$((PIN_FAILS + 1))
       [ "$QUIET" -eq 1 ] || bad "off-vocabulary token accepted: ${UNIT_VERDICT[beta]:-<unset>}" ;;
  esac

  # B — ONE run decision, shared by the CLI smoke gate and the unit corpus.
  local r
  LEG_RUN_MODE[nat]="native";   LEG_LAUNCH[nat]=""
  LEG_RUN_MODE[lau]="launched"; LEG_LAUNCH[lau]="arch -x86_64"
  LEG_RUN_MODE[skp]="skip";     LEG_LAUNCH[skp]=""
  leg_run_is_skipped nat && r=skipped || r=runnable; ck "run decision: native   -> runnable" "runnable" "$r"
  leg_run_is_skipped lau && r=skipped || r=runnable; ck "run decision: launched -> runnable" "runnable" "$r"
  leg_run_is_skipped skp && r=skipped || r=runnable; ck "run decision: skip     -> skipped"  "skipped"  "$r"

  # A `launched` leg with an EMPTY launcher argv is the anchor's contradiction
  # wearing its other face: runnable, and nothing to run it with.
  local out rc
  LEG_RUN_MODE[bad]="launched"; LEG_LAUNCH[bad]=""
  out="$(leg_run_is_skipped bad 2>&1)"; rc=$?
  ck "a 'launched' leg with an EMPTY launcher argv REFUSES" "97" "$rc"
  ck_has "…and names the contradiction" "$out" "EMPTY launcher argv"
}

# ═══════════════════════════════════════════════════════════════════════════
# C — the shipped Step-8 gate sequence, EXECUTED
# ═══════════════════════════════════════════════════════════════════════════
# Extracted verbatim and run inside a REAL `for` loop, so the `continue`s are the
# driver's own. This is the pin that fails if the CONTROL-COMPILER gate ever comes
# back: a leg with an EMPTY LEG_CC must REACH the corpus.
# ✔MEASURED root cause — on an arm64 Mac `clang -dumpmachine` reports
# `arm64-apple-darwin24.6.0`, which the resolver correctly REFUSES for
# macho64-x86_64, so that leg had no control compiler and this gate silently cost
# it its entire corpus while its CLI smoke passed 14/14 through the very launcher
# the same line said was available.
pin_step8_gates() { # pin_step8_gates <driver>
  local drv="$1" gates L
  gates="$(LC_ALL=C awk '
    /^  # ── the three ways a leg does not reach the corpus, each already NAMED ──────$/ { inb = 1 }
    /^  bin="\$\{FIXTURE\[\$leg\]\}"/ { inb = 0 }
    inb { print }' "$drv")"
  if [ -z "$gates" ]; then
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || bad "could not extract the Step-8 gate sequence — this pin would assert over nothing"
    return 0
  fi
  run_gates() { # run_gates <leg> -> REACHED | SKIPPED
    local REACHED=SKIPPED leg
    for leg in "$1"; do
      eval "$gates"
      REACHED=REACHED
    done
    printf '%s' "$REACHED"
  }
  UNIT_VERDICT=(); UNIT_UNCLASSIFIED=0; UNIT_UNCLASSIFIED_LEGS=()

  # the operator's Mac, macho64-x86_64: BUILT, launchable under `arch -x86_64`,
  # and with NO control compiler.
  L=macho64-x86_64
  LEG_TCL_LIB[$L]=/cache/libtcl8.6.dylib; COMPILE_OK[$L]=1; FIXTURE[$L]=/out/testfixture
  LEG_RUN_MODE[$L]=launched; LEG_LAUNCH[$L]="arch -x86_64"; LEG_CC[$L]=""
  LEG_VERDICT[$L]=""; LEG_VERDICT_DETAIL[$L]="host darwin/arm64 cannot run it natively; declared launcher 'arch -x86_64' is available"
  ck "no CONTROL compiler -> the corpus is REACHED" "REACHED" "$(run_gates $L)"

  # …and the three LEGITIMATE not-runs must still stop the leg.
  L=nolibs; LEG_TCL_LIB[$L]=""; COMPILE_OK[$L]=0; FIXTURE[$L]=""
  LEG_RUN_MODE[$L]=native; LEG_LAUNCH[$L]=""; LEG_CC[$L]=""
  LEG_VERDICT[$L]="skipped-build-input-missing"; LEG_VERDICT_DETAIL[$L]="no libtcl here"
  ck "no libraries       -> SKIPPED" "SKIPPED" "$(run_gates $L)"

  L=nofix; LEG_TCL_LIB[$L]=/cache/libtcl8.6.so; COMPILE_OK[$L]=0; FIXTURE[$L]=""
  LEG_RUN_MODE[$L]=native; LEG_LAUNCH[$L]=""; LEG_CC[$L]=""
  LEG_VERDICT[$L]="poisoned"; LEG_VERDICT_DETAIL[$L]="compile failed"
  ck "no fixture         -> SKIPPED" "SKIPPED" "$(run_gates $L)"

  L=noexec; LEG_TCL_LIB[$L]=/cache/libtcl8.6.so; COMPILE_OK[$L]=1; FIXTURE[$L]=/out/testfixture
  LEG_RUN_MODE[$L]=skip; LEG_LAUNCH[$L]=""; LEG_CC[$L]=""
  LEG_VERDICT[$L]="skipped-by-runOn"; LEG_VERDICT_DETAIL[$L]="runOn excludes this host"
  LEG_RUN_VERDICT[$L]="skipped-by-runOn"; LEG_RUN_DETAIL[$L]="runOn excludes this host"
  ck "host cannot EXECUTE -> SKIPPED" "SKIPPED" "$(run_gates $L)"
}

# ═══════════════════════════════════════════════════════════════════════════
# D — parse_segment keeps the FIRST DIAGNOSTIC line
# ═══════════════════════════════════════════════════════════════════════════
pin_parse_segment() { # pin_parse_segment <driver>
  local drv="$1"
  load_fns "$drv" parse_segment fact || return 0

  # (1) THE MEASURED PRECONDITION LOG, reproduced from the operator's Mac. The
  #     harness reported "the UNNAMED file that aborted … no resolvable corpus
  #     file" eleven times while THIS was the log's first line.
  cat > "$WORK/precond.log" <<'LOG'
Can't find a usable init.tcl in the following directories: /opt/local/lib/tcl8.6 /opt/local/lib/tcl8.6 ...
This probably means that Tcl wasn't installed properly.
    (procedure "tclInit" line 61)
    invoked from within
"interp create tinterp"
    (procedure "slave_test_script" line 4)
LOG
  parse_segment "$WORK/precond.log" "$WORK/precond.facts"
  ck "precondition log: the diagnostic is captured verbatim" \
     "Can't find a usable init.tcl in the following directories: /opt/local/lib/tcl8.6 /opt/local/lib/tcl8.6 ..." \
     "$(fact A "$WORK/precond.facts")"
  ck "precondition log: ZERO files completed" "0"  "$(fact N "$WORK/precond.facts")"
  ck "precondition log: no summary line"      ""   "$(fact S "$WORK/precond.facts")"

  # (2) a HEALTHY segment must parse EXACTLY as before — the new rule must not
  #     consume a line an older rule needed, and must not invent a diagnostic.
  cat > "$WORK/healthy.log" <<'LOG'
select1-1.1... Ok
select1-1.2... Ok
Time: select1.test 42 ms
misc7-7.0... Ok
Time: misc7.test 11 ms
0 errors out of 192 tests on host Darwin 64-bit
LOG
  parse_segment "$WORK/healthy.log" "$WORK/healthy.facts"
  ck "healthy log: NO diagnostic is invented" ""            "$(fact A "$WORK/healthy.facts")"
  ck "healthy log: files counted"             "2"           "$(fact N "$WORK/healthy.facts")"
  ck "healthy log: last file"                 "misc7.test"  "$(fact D "$WORK/healthy.facts")"
  ck "healthy log: summary intact"            "0 errors out of 192 tests on host Darwin 64-bit" \
                                                            "$(fact S "$WORK/healthy.facts")"
  ck "healthy log: ' Ok' tally"               "3"           "$(fact K "$WORK/healthy.facts")"

  # (3) a GENUINE mid-corpus crash: files completed AND a diagnostic exists, so
  #     the precondition branch cannot fire on it (see pin_precondition).
  cat > "$WORK/crash.log" <<'LOG'
select1-1.1... Ok
Time: select1.test 42 ms
swarmvtabfault-1.1-oom-persistent.143...
child process exited abnormally
    (procedure "do_test" line 12)
LOG
  parse_segment "$WORK/crash.log" "$WORK/crash.facts"
  ck "crash log: files completed > 0"     "1" "$(fact N "$WORK/crash.facts")"
  ck "crash log: the last test is named"  "swarmvtabfault-1.1-oom-persistent.143" "$(fact T "$WORK/crash.facts")"
  ck "crash log: a diagnostic is captured too" "child process exited abnormally"  "$(fact A "$WORK/crash.facts")"
}

# ═══════════════════════════════════════════════════════════════════════════
# E — the precondition discriminator
# ═══════════════════════════════════════════════════════════════════════════
# ★★ THE RESILIENCE RULE IS WHAT THIS PIN PROTECTS. A fixture abort stays a
# RECOVERABLE outcome — named, resumed past, reported in the union. Only ONE
# shape is diverted: zero files completed AND the same first diagnostic as the
# previous zero-progress segment. FIVE of these six assertions are the resilience
# cases; if a future change makes the discriminator greedier they go red first.
pin_precondition() { # pin_precondition <driver>
  local drv="$1" cond line
  # BY PREFIX, so a WEAKENED discriminator is still LOADED and judged
  # BEHAVIOURALLY rather than the pin going red merely because it went missing.
  line="$(LC_ALL=C grep -n 'if \[\[ "\$s_nf" -eq 0' "$drv" | head -1)"
  if [ -z "$line" ]; then
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || bad "the precondition discriminator is not in ${drv##*/} — this pin would assert over nothing"
    return 0
  fi
  cond="${line#*:}"; cond="${cond%; then}"; cond="${cond#*if }"
  takes() { # takes <files-done> <this-diag> <prev-zero-diag>
    local s_nf="$1" s_diag="$2" prev_zero_diag="$3"
    if eval "$cond"; then printf 'PRECONDITION'; else printf 'RESUME'; fi
  }
  local D1="Can't find a usable init.tcl in the following directories: /opt/local/lib/tcl8.6 ..."
  local D2="child process exited abnormally"
  ck "zero progress twice, IDENTICAL diagnostic -> PRECONDITION" PRECONDITION "$(takes 0 "$D1" "$D1")"
  ck "the FIRST such abort                      -> RESUME"       RESUME "$(takes 0 "$D1" "")"
  ck "zero progress, DIFFERENT diagnostic       -> RESUME"       RESUME "$(takes 0 "$D2" "$D1")"
  ck "a crash AFTER completing files            -> RESUME"       RESUME "$(takes 7 "$D1" "$D1")"
  ck "one file completed, same diagnostic       -> RESUME"       RESUME "$(takes 1 "$D1" "$D1")"
  ck "zero progress, NO diagnostic at all       -> RESUME"       RESUME "$(takes 0 "" "")"
}

# ═══════════════════════════════════════════════════════════════════════════
# F — acq_field, against a REAL acquisition_record
# ═══════════════════════════════════════════════════════════════════════════
# Built by harness_legs.py's OWN `acquisition_record()`, never by a hand-written
# JSON blob: the point is that the driver reads the shape the resolver actually
# emits, and a fixture typed here would keep agreeing with itself after a rename.
pin_acq_field() { # pin_acq_field <driver>
  local drv="$1" j
  if [ ! -f "$WORK/acq.json" ]; then
    skip "F: acq_field — no python3/harness_legs.py, so no REAL acquisition record to read"
    return 0
  fi
  load_fns "$drv" acq_field || return 0
  j="$(cat "$WORK/acq.json")"
  ck "acq_field cacheDir (required read)"        "/pin/cache"        "$(acq_field "$j" cacheDir)"
  ck "acq_field scriptLibraryDir (optional)"     "/pin/cache/tcl8.6" "$(acq_field "$j" optional:scriptLibraryDir)"
  ck "acq_field libraries"                       "libtcl8.6.dylib	/pin/cache/libtcl8.6.dylib" \
                                                                     "$(acq_field "$j" libraries)"
  ck "an ABSENT optional key yields empty"       ""                  "$(acq_field "$j" optional:noSuchField)"
}

# ═══════════════════════════════════════════════════════════════════════════
# G — the loader search variable is TARGET-keyed
# ═══════════════════════════════════════════════════════════════════════════
# Over a REAL `--plan`, so the mapping is exercised against every leg the
# catalogue declares rather than against five hand-typed strings.
pin_loader_var() { # pin_loader_var <driver>
  local drv="$1" l
  if [ ! -f "$WORK/plan.sh" ]; then
    skip "G: leg_loader_path_var — no python3/harness_legs.py, so no REAL resolved plan"
    return 0
  fi
  declare -a LEG_ORDER=()
  # THE REAL PLAN, through the driver's own `eval` transport.
  eval "$(cat "$WORK/plan.sh")" 2>/dev/null || true
  if [ "${#LEG_ORDER[@]}" -lt 5 ]; then
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || bad "the plan yielded ${#LEG_ORDER[@]} leg(s), expected at least 5 — this pin would assert over nothing"
    return 0
  fi
  load_fns "$drv" leg_loader_path_var || return 0
  declare -A WANT=([elf64-x86_64]=LD_LIBRARY_PATH [elf64-arm64]=LD_LIBRARY_PATH \
                   [pe64-x86_64]="" [macho64-arm64]=DYLD_LIBRARY_PATH \
                   [macho64-x86_64]=DYLD_LIBRARY_PATH)
  for l in "${LEG_ORDER[@]}"; do
    [ -n "${WANT[$l]+set}" ] || continue
    ck "loader var: $l (${LEG_FORMAT[$l]})" "${WANT[$l]}" "$(leg_loader_path_var "$l")"
  done
  # A plan that contradicts ITSELF about the target OS must refuse, never pick one.
  local out rc
  LEG_CONFIG_STAGE_KEY[elf64-x86_64]="darwin"
  out="$(leg_loader_path_var elf64-x86_64 2>&1)"; rc=$?
  ck "a self-contradicting plan REFUSES" "97" "$rc"
  ck_has "…and names the contradiction" "$out" "disagrees with itself"
}

# ── prerequisites for F and G ───────────────────────────────────────────────
# Produced ONCE, before any pin runs. Their absence is a SKIP with a reason, never
# a silent pass: a pin that quietly asserts nothing is the defect this file exists
# to prevent.
if command -v python3 >/dev/null 2>&1 && [ -f "$LEGS_PY" ] && [ -f "$CATALOGUE" ]; then
  python3 "$LEGS_PY" --catalogue "$CATALOGUE" --plan --host-os darwin --host-arch arm64 --format sh \
    > "$WORK/plan.sh" 2>/dev/null || rm -f "$WORK/plan.sh"
  python3 - "$LEGS_PY" > "$WORK/acq.json" 2>/dev/null <<'PY' || rm -f "$WORK/acq.json"
import importlib.util, json, sys
spec = importlib.util.spec_from_file_location("hl", sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
plan = {"leg": "macho64-arm64", "targetArch": "arm64",
        "cacheDir": "/pin/cache", "scriptLibraryDir": "/pin/cache/tcl8.6"}
print(json.dumps(m.acquisition_record(
    plan,
    libraries=[{"as": "libtcl8.6.dylib", "path": "/pin/cache/libtcl8.6.dylib"}],
    from_cache=True, remediated=[])))
PY
fi

echo "pinning $(basename "$SH") — verdict vocabulary: $VOCAB_SOURCE (${#UNIT_SKIP_VOCAB[@]} token(s))"
case "$VOCAB_SOURCE" in
  unavailable*) skip "A/B: $VOCAB_SOURCE — the token guard cannot be exercised on this host" ;;
  *)            assert_vocab_clean ;;
esac

# ═══════════════════════════════════════════════════════════════════════════
# I — THE CONFOUND SUPPLY IS PER LEG, FROM THE LEG'S OWN DECLARATION
# ═══════════════════════════════════════════════════════════════════════════
# D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG. This driver used to build
# ONE `CONFOUND_PATTERNS` array before the leg loop, from one global
# `DSS_CONFOUNDS`, and apply it to every leg — including legs where nothing had
# ever been measured. H6 below restores exactly that and this pin must go red.
# ★ The MATCHER has been pinned in detail by test-confound-scope.sh for months;
# the SUPPLY was pinned by nothing, in either driver, which is why the defect
# outlived so many green runs.
pin_confound_supply() { # pin_confound_supply <driver>
  local drv="$1" out rc
  DSS_CONFOUNDS=""
  LEG_CONFOUNDS=()
  LEG_CONFOUNDS[elf64-x86_64]="'^walsetlk-' '^busy2-'"
  LEG_CONFOUNDS[pe64-x86_64]=""
  LEG_CONFOUNDS[elf64-arm64]="'^busy2-' 'emulated:^writecrash-'"
  load_fns "$drv" leg_confound_patterns || return 0
  local -a got=()
  eval "got=($(leg_confound_patterns elf64-x86_64))"
  ck "a leg gets ITS OWN declared patterns" "^walsetlk- ^busy2-" "${got[*]}"
  eval "got=($(leg_confound_patterns elf64-arm64))"
  ck "...a DIFFERENT leg gets a DIFFERENT set" "^busy2- emulated:^writecrash-" "${got[*]}"
  # THE HEADLINE: a leg declaring nothing must inherit nothing. Under the old
  # global list this came back with every pattern in the shipped default.
  eval "got=($(leg_confound_patterns pe64-x86_64))"
  ck "a leg declaring [] inherits NOTHING" "" "${got[*]}"
  # The scope prefix must survive the supply, or the qemu-only writecrash
  # excusal silently becomes a bare one on a native run.
  eval "got=($(leg_confound_patterns elf64-arm64))"
  ck_has "the 'emulated:' scope survives the supply" "${got[*]}" "emulated:^writecrash-"
  # The operator override still applies to EVERY leg — intent, not inheritance.
  DSS_CONFOUNDS='^op-1 ^op-2'
  eval "got=($(leg_confound_patterns pe64-x86_64))"
  ck "the operator override reaches EVERY leg" "^op-1 ^op-2" "${got[*]}"
  DSS_CONFOUNDS=""
  # An UNDECLARED leg is a transport defect, never an empty list.
  out="$(leg_confound_patterns nodecl 2>&1)"; rc=$?
  ck "an UNDECLARED leg REFUSES rather than answering []" "97" "$rc"
  ck_has "...naming the reason" "$out" "transport"
}

# ═══════════════════════════════════════════════════════════════════════════
# J — A FAILED RUN-DIRECTORY OPERATION IS A VERDICT, NOT A SILENT FALLBACK
# ═══════════════════════════════════════════════════════════════════════════
# D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS. Two properties, both load-bearing:
# an EMPTY argv prefix is a real answer (`runFilesystem: driver` — this driver
# does it natively), and a FAILING prefix must be REPORTED rather than fallen
# back from, because the fallback is the DrvFs directory the declaration exists
# to keep the corpus off.
pin_run_dir_argv() { # pin_run_dir_argv <driver>
  local drv="$1" rc
  load_fns "$drv" run_dir_argv || return 0
  RUN_DIR_WHY="stale"
  run_dir_argv leg "do nothing" "" /x; rc=$?
  ck "an EMPTY prefix is a real answer (driver filesystem)" "0" "$rc"
  ck "...and clears any previous reason" "" "$RUN_DIR_WHY"
  run_dir_argv leg "prepare the run directory" "sh -c" 'exit 3'; rc=$?
  ck "a FAILING prefix is reported, never swallowed" "1" "$rc"
  ck_has "...naming what could not be done" "$RUN_DIR_WHY" "prepare the run directory"
  ck_has "...and the exit code" "$RUN_DIR_WHY" "exited 3"
}

green "A+B  the not-run recorder + the shared run decision" pin_verdicts
green "C    the Step-8 gate sequence, executed"             pin_step8_gates
green "D    parse_segment keeps the first diagnostic"       pin_parse_segment
green "E    the precondition discriminator"                 pin_precondition
green "F    acq_field over a REAL acquisition record"       pin_acq_field
green "G    the loader variable is TARGET-keyed"            pin_loader_var
green "I    the confound supply is PER LEG"                 pin_confound_supply
green "J    a failed run-dir operation is a VERDICT"        pin_run_dir_argv

# ═══════════════════════════════════════════════════════════════════════════
# H — RED-ON-DISABLE. Every guard above is REMOVED in a copy; the pin must fail.
# ═══════════════════════════════════════════════════════════════════════════
printf -- '-- H    red-on-disable (each mutation is asserted to have LANDED first)\n'

# H1 — the empty-token guard inside unit_not_run.
if mutate "H1 remove the empty-token guard" "$WORK/m1.sh" 'UNIT_UNCLASSIFIED_LEGS+=("$leg")' '
    /^  if \[\[ -n "\$why" \]\]; then$/ { skip = 1 }
    /^  UNIT_VERDICT\["\$leg"\]="not run \[\$token\] — \$detail"$/ { skip = 0 }
    !skip { print }'; then
  red "H1 an unclassified skip token is refused" pin_verdicts "$WORK/m1.sh"
fi

# H2 — put the CONTROL-COMPILER gate back. This is the defect itself, restored.
if mutate "H2 restore the control-compiler gate" "$WORK/m2.sh" 'no CONTROL compiler on this host' '
    /^    info "\[\$leg\] no CONTROL compiler on this host/ { print "    continue"; next }
    { print }'; then
  red "H2 a leg with no control compiler still runs" pin_step8_gates "$WORK/m2.sh"
fi

# H3 — the first-diagnostic capture in parse_segment.
if mutate "H3 remove the first-diagnostic capture" "$WORK/m3.sh" 'diag = $0' '
    /\{ if \(diag == "" && \$0 ~ \/\[\^ \\t\]\/ \\$/ { skip = 1 }
    skip && /^      \} \}$/ { skip = 0; next }
    !skip { print }'; then
  red "H3 the captured log's first error line is surfaced" pin_parse_segment "$WORK/m3.sh"
fi

# H4 — drop the "same diagnostic" conjunct. The discriminator becomes greedy and
# the RESILIENCE cases must go red: a genuine crash would stop being resumed.
if mutate "H4 weaken the precondition discriminator" "$WORK/m4.sh" '"$s_diag" == "${prev_zero_diag:-}"' '
    /^    if \[\[ "\$s_nf" -eq 0 && -n "\$s_diag" && "\$s_diag" == "\$\{prev_zero_diag:-\}" \]\]; then$/ {
      print "    if [[ \"$s_nf\" -eq 0 ]]; then"; next }
    { print }'; then
  red "H4 a genuine crash is still RESUMED" pin_precondition "$WORK/m4.sh"
fi

# H5 — the target-OS cross-check in leg_loader_path_var: make it always answer the
# ELF spelling, which is the original defect (dyld ignores LD_LIBRARY_PATH).
if mutate "H5 hardcode the ELF loader variable" "$WORK/m5.sh" "darwin)  printf '%s' 'DYLD_LIBRARY_PATH'" '
    /^    darwin\)  printf .%s. .DYLD_LIBRARY_PATH. ;;$/ {
      print "    darwin)  printf '\''%s'\'' '\''LD_LIBRARY_PATH'\'' ;;"; next }
    { print }'; then
  red "H5 the loader variable follows the TARGET" pin_loader_var "$WORK/m5.sh"
fi

# H6 — THE DEFECT ITSELF, RESTORED: one global list for every leg. The pin must
# go red on "a leg declaring [] inherits NOTHING" and on the per-leg answers.
if mutate "H6 restore the ONE-GLOBAL-LIST confound supply" "$WORK/m6.sh" 'printf '"'"'%s'"'"' "${LEG_CONFOUNDS[$leg]}"' '
    /^  printf .%s. "\$\{LEG_CONFOUNDS\[\$leg\]\}"$/ {
      print "  printf '\''%s'\'' \"'\''^walsetlk-'\'' '\''^busy2-'\'' '\''^zipfile-25.0$'\''\""; next }
    { print }'; then
  red "H6 the confound supply is PER LEG, not one global list" pin_confound_supply "$WORK/m6.sh"
fi

# H7 — make a failed run-directory operation report success, which is the silent
# fallback onto DrvFs that the whole declaration exists to prevent.
if mutate "H7 make a failed run-dir operation look fine" "$WORK/m7.sh" 'RUN_DIR_WHY="could not $what in the launcher'"'"'s own filesystem' '
    /^  RUN_DIR_WHY="could not \$what in the launcher.s own filesystem/ {
      print "  RUN_DIR_WHY=\"\""; print "  return 0"; skip = 1; next }
    skip && /^  return 1$/ { skip = 0; next }
    { print }'; then
  red "H7 a failed run-dir operation is REPORTED, never swallowed" pin_run_dir_argv "$WORK/m7.sh"
fi

printf '\n'
printf 'passed=%d failed=%d skipped=%d\n' "$PASSED" "$FAILED" "$SKIPPED"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
