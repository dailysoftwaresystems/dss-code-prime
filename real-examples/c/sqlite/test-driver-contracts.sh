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
#   L  the launcher-prerequisite gate           a launcher whose DECLARED needs are
#                                               absent skips the leg by name; one
#                                               whose needs are MET still REACHES
#                                               the corpus; strict mode makes the
#                                               skip fatal; and every smoke rc has
#                                               its own verdict (4 and 2 are not
#                                               accusations, 1 is)
#   M  the Step-7c smoke argv                   --cli-target/--reference-target are
#                                               MEASURED off the binaries' own
#                                               headers, the reference launcher
#                                               comes from the CATALOGUE, and no
#                                               host-identity branch chooses it
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
  # `facts` (plural) as well as `fact`: the inert-file assertion below reads the
  # NAMES, and a pin that could only read the count would pass over a counter
  # that blamed the wrong file.
  load_fns "$drv" parse_segment fact facts || return 0

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

  # (4) COMPLETED IS NOT COVERED — the inert-file counter, driven through the
  #     SHIPPED parse_segment rather than a re-typing of what it is believed to
  #     do. ✔The log below is the MEASURED shape, copied off a real corpus.log:
  #     every file emits two harness teardown results, and an INERT file emits
  #     ONLY those. ⚠ Note the `...` has NO space before it — `fts5aa.test-
  #     closeallfiles...` — which is exactly the detail that made a first cut of
  #     this counter report ZERO inert files: an anchored `-closeallfiles$`
  #     match never fires, so teardown counted as coverage and every file looked
  #     busy. That is why this pin asserts the NAMES and not just the count.
  cat > "$WORK/inert.log" <<'LOG'
select1-1.1... Ok
select1-1.2... Ok
select1.test-closeallfiles... Ok
select1.test-sharedcachesetting... Ok
Time: select1.test 42 ms
fts5aa.test-closeallfiles... Ok
fts5aa.test-sharedcachesetting... Ok
Time: fts5aa.test 2 ms
! wherelimit-1.1 expected: [1]
! wherelimit-1.1 got: [0]
wherelimit.test-closeallfiles... Ok
wherelimit.test-sharedcachesetting... Ok
Time: wherelimit.test 7 ms
0 errors out of 8 tests on host Linux 64-bit
LOG
  parse_segment "$WORK/inert.log" "$WORK/inert.facts"
  ck "inert log: three files completed"          "3" "$(fact N "$WORK/inert.facts")"
  ck "inert log: exactly ONE asserted nothing"   "1" "$(fact M "$WORK/inert.facts")"
  # BY NAME, not by count: "one inert file" is satisfied by the WRONG one, and a
  # counter that blamed select1.test would be a worse instrument than none.
  ck "inert log: and it is the one that ran nothing" "fts5aa.test" \
     "$(facts I "$WORK/inert.facts" | tr '\n' ' ' | sed 's/ $//')"
  # A file whose only output is a FAILURE has asserted something. Counting it as
  # inert would let a capability gate go green over a file that ran and failed.
  ck "inert log: a file that only FAILED is not inert" "1" "$(fact Q "$WORK/inert.facts")"
  # And the healthy log from (2), which carries no teardown lines at all, must
  # report ZERO — the counter must not depend on teardown being present.
  ck "healthy log: nothing is inert" "0" "$(fact M "$WORK/healthy.facts")"
}

# ═══════════════════════════════════════════════════════════════════════════
# D2 — the declared capability set reaches EVERY build site, in BOTH drivers
# ═══════════════════════════════════════════════════════════════════════════
# ★★ THE PARITY PIN. "A capability in one driver and not the other" is this
# project's canonical silent harness bug, and it had already happened INSIDE one
# run: the sqlite3 CLI recipe carried -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE
# while the testfixture recipe, built from the same tree, carried neither.
# So this is asserted at the SOURCE level over BOTH drivers: every `make`
# invocation that builds or dry-runs a target in $BLD must carry OPTIONS=, and
# every `configure` invocation must carry the declared flags. A source-level pin
# because the failure is a MISSING call-site — something no run of the existing
# call sites can ever reveal.
pin_stage_capabilities() { # pin_stage_capabilities <driver...>
  local drv f n
  for drv in "$@"; do
    [ -f "$drv" ] || { ck "stage capabilities: $drv exists" "yes" "no"; continue; }
    f="$(basename "$drv")"
    # (1) every ./configure of the build dir carries the declared flags.
    n="$(LC_ALL=C grep -cE '(configure)" +("\$\{CONFIGURE_ARGS\[@\]\}"|\$STAGE_CONFIGURE_FLAGS)' "$drv" || true)"
    ck "$f: every ./configure carries the declared capability flags (count>0)" \
       "yes" "$([ "${n:-0}" -gt 0 ] && echo yes || echo no)"
    n="$(LC_ALL=C grep -cE '(configure)" *>' "$drv" || true)"
    ck "$f: no ./configure invocation is left BARE" "0" "${n:-0}"
    # (2) every make of a real target in the build dir carries OPTIONS=.
    #     `make -n` derivations go through dss_bh_emit_recipe, checked in (3).
    # ⚠ THE SITE COUNT IS ASSERTED FIRST. "no site without OPTIONS = 0" is
    #   satisfied by a pattern that matches NO SITES AT ALL, so a matcher that
    #   silently stops matching turns this pin into a permanent green. Prove it
    #   can still SEE its subject before asking anything about it — the same
    #   vacuity the sibling check in (3) actually hit while being written.
    local sites
    sites="$(LC_ALL=C grep -cE 'cd "\$BLD" && make ' "$drv" || true)"
    ck "$f: the make-site matcher still finds sites" "yes" \
       "$([ "${sites:-0}" -gt 0 ] && echo yes || echo no)"
    n="$(LC_ALL=C grep -E 'cd "\$BLD" && make ' "$drv" | LC_ALL=C grep -cv 'OPTIONS=' || true)"
    ck "$f: no 'cd \$BLD && make' without OPTIONS=" "0" "${n:-0}"
    # (3) every recipe derivation passes OPTIONS= as a make variable. Counted
    #     against the number of derivations so ADDING a third target without the
    #     flag reds, which a mere ">0" would not catch.
    # ⚠ CALL SITES, not mentions. A first cut counted every occurrence of the
    #   name and reported 3-of-2 and 4-of-2 against drivers that were correct —
    #   the extra hits were PROSE in the surrounding comments. A pin whose
    #   denominator counts documentation cannot be green on a correct driver.
    local derivations opts
    derivations="$(LC_ALL=C grep -cE '(^|[^a-z_])dss_bh_emit_recipe \\$' "$drv" || true)"
    opts="$(LC_ALL=C grep -c -- '--make-var "OPTIONS=\$STAGE_MAKE_OPTIONS"' "$drv" || true)"
    ck "$f: the derivation matcher still finds call sites" "yes" \
       "$([ "${derivations:-0}" -gt 0 ] && echo yes || echo no)"
    ck "$f: every dss_bh_emit_recipe call passes OPTIONS= ($derivations derivation(s))" \
       "$derivations" "$opts"
  done
}

# ═══════════════════════════════════════════════════════════════════════════
# E — the precondition discriminator
# ═══════════════════════════════════════════════════════════════════════════
# ★★ THE RESILIENCE RULE IS WHAT THIS PIN PROTECTS. A fixture abort stays a
# RECOVERABLE outcome — named, resumed past, reported in the union. Only ONE
# shape is diverted: zero files completed AND the same ZERO-PROGRESS SIGNATURE as
# the previous zero-progress segment. MOST of the 14 assertions below are the
# resilience cases — NINE demand RESUME-or-spend-the-budget (8 verdicts + the
# whole-budget drive) against FOUR that demand a PRECONDITION stop (2 verdicts + 2
# drives), plus one input control — so if a future change makes the discriminator
# greedier they go red first and they go red in numbers. (✔That ratio is what H4
# measures: dropping the sameness conjunct reds NINE checks.)
# ★★ AND THE SILENT CRASH IS THE OTHER HALF
# [D-HARNESS-PRECONDITION-DISCRIMINATOR-BLIND-TO-A-SILENT-CRASH]. The
# discriminator used to require a NON-EMPTY diagnostic, so a fixture that died
# writing ZERO BYTES could never satisfy it. ✔MEASURED 2026-08-10, one Windows
# run, two legs, same commit, same root cause: elf64-arm64 under qemu (whose
# crash PRINTS `qemu: uncaught target signal 11`) stopped after one resume, while
# elf64-x86_64 native (whose crash is SILENT) burned all ten and reported eleven
# unnameable aborts. So this pin now drives the WHOLE STOPPING DECISION over real
# segment logs, not only the condition in isolation — because "stops after ONE
# resume rather than ten" is the property, and a per-call verdict cannot state it.
pin_precondition() { # pin_precondition <driver>
  local drv="$1" cond carry line budget
  # BY PREFIX, so a WEAKENED discriminator is still LOADED and judged
  # BEHAVIOURALLY rather than the pin going red merely because it went missing.
  # ⚠ THE LOOSE PREFIX IS DELIBERATE AND `head -1` IS LOAD-BEARING: the carry line
  # two lines below the condition opens with the same test, and the CONDITION is
  # first in file order. A tighter prefix naming the `-n` conjunct would make a
  # mutation that DELETES that conjunct read as "the discriminator is missing"
  # instead of being judged behaviourally, which is the whole point of section H.
  line="$(LC_ALL=C grep -n 'if \[\[ "\$s_nf" -eq 0' "$drv" | head -1)"
  if [ -z "$line" ]; then
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || bad "the precondition discriminator is not in ${drv##*/} — this pin would assert over nothing"
    return 0
  fi
  cond="${line#*:}"; cond="${cond%; then}"; cond="${cond#*if }"
  # THE CARRY IS THE SECOND HALF OF THE DECISION and is extracted too: a condition
  # that is right about one segment decides nothing if what the next segment
  # compares against is wrong. ✔The old carry stored `$s_diag`, i.e. EMPTY for a
  # silent segment, which is indistinguishable from "the last segment made
  # progress" — the same defect a second time, one line down.
  carry="$(LC_ALL=C grep -h 'prev_zero_sig="\$s_zero_sig"' "$drv" | head -1)"
  # The DRIVER'S OWN budget, so "rather than ten" is its number and not this
  # file's opinion of it.
  budget="$(LC_ALL=C awk '/^DSS_MAX_RESUMES=/ { if (match($0, /:-[0-9]+/)) print substr($0, RSTART + 2, RLENGTH - 2); exit }' "$drv")"
  if [ -z "$carry" ] || [ -z "$budget" ]; then
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || bad "the carry line and/or DSS_MAX_RESUMES could not be read from ${drv##*/} (carry=[$carry] budget=[$budget]) — this pin would assert over a default it invented"
    return 0
  fi
  load_fns "$drv" parse_segment fact zero_progress_signature || return 0
  takes() { # takes <files-done> <this-diag> <prev-sig> [ok-lines] [fail-markers] [last-test]
    local s_nf="$1" prev_zero_sig="$3" s_zero_sig
    # THE SHIPPED DERIVATION, not a hand-set signature: the sentinel is the whole
    # fix, so a pin that supplied it itself would pass over a driver that never
    # produced one.
    s_zero_sig="$(zero_progress_signature "$2" "${4:-0}" "${5:-0}" "${6:-}")"
    if eval "$cond"; then printf 'PRECONDITION'; else printf 'RESUME'; fi
  }
  local D1="Can't find a usable init.tcl in the following directories: /opt/local/lib/tcl8.6 ..."
  local D2="child process exited abnormally"
  local SILENT
  SILENT="$(zero_progress_signature "" 0 0 "")"
  ck "zero progress twice, IDENTICAL diagnostic -> PRECONDITION" PRECONDITION "$(takes 0 "$D1" "$D1")"
  ck "the FIRST such abort                      -> RESUME"       RESUME "$(takes 0 "$D1" "")"
  ck "zero progress, DIFFERENT diagnostic       -> RESUME"       RESUME "$(takes 0 "$D2" "$D1")"
  ck "a crash AFTER completing files            -> RESUME"       RESUME "$(takes 7 "$D1" "$D1")"
  ck "one file completed, same diagnostic       -> RESUME"       RESUME "$(takes 1 "$D1" "$D1")"
  ck "zero progress, NO OUTPUT, first time      -> RESUME"       RESUME "$(takes 0 "" "")"
  # ── THE SILENT CASE, WHICH IS THE DEFECT ─────────────────────────────────
  ck "SILENCE TWICE                             -> PRECONDITION" PRECONDITION "$(takes 0 "" "$SILENT")"
  # ── AND THE RESILIENCE RULE UNDER SILENCE: no diagnostic is NOT the same as
  #    no output. A segment that ran TESTS without completing a FILE has an empty
  #    `A` fact too, and it must stay on the resume path.
  ck "no diagnostic but ' Ok' lines             -> RESUME"       RESUME "$(takes 0 "" "$SILENT" 5 0 "")"
  ck "no diagnostic but a FAILURE marker        -> RESUME"       RESUME "$(takes 0 "" "$SILENT" 0 2 "")"
  ck "no diagnostic but a test NAME             -> RESUME"       RESUME "$(takes 0 "" "$SILENT" 0 0 "select1-1.1")"

  # ── THE STOPPING DECISION, DRIVEN OVER REAL LOGS ─────────────────────────
  # Every moving part is the driver's: parse_segment and zero_progress_signature
  # are LOADED from it, the condition and the carry are EXTRACTED from it, and the
  # budget is READ from it. What this function contributes is the loop, and the
  # loop is what makes "stops after ONE resume" sayable at all.
  drive() { # drive <log>... -> "segments=N resumes=R stop=WHY"
    local prev_zero_sig="" n=0 r=0 stop="BUDGET-EXHAUSTED" lg
    local s_nf s_diag s_ok s_fx s_last s_zero_sig f="$WORK/drive.facts"
    for lg in "$@"; do
      n=$((n + 1))
      parse_segment "$lg" "$f"
      s_nf="$(fact N "$f")"; s_diag="$(fact A "$f")"
      s_ok="$(fact K "$f")"; s_fx="$(fact Q "$f")"; s_last="$(fact T "$f")"
      s_zero_sig="$(zero_progress_signature "$s_diag" "$s_ok" "$s_fx" "$s_last")"
      if eval "$cond"; then stop="PRECONDITION"; break; fi
      eval "$carry"
      if [ "$r" -ge "$budget" ]; then stop="BUDGET-EXHAUSTED"; break; fi
      r=$((r + 1))
    done
    printf 'segments=%s resumes=%s stop=%s' "$n" "$r" "$stop"
  }
  # 12 logs, i.e. more than the budget, so "it stopped early" is a real finding
  # and not the list running out.
  local i
  # `mkdir -p` and NO `rm -rf`: every one of these files is rewritten below on
  # every call, so there is nothing stale to clear — and `rm -rf "$WORK/seg"`
  # with an empty $WORK would spell `rm -rf /seg`. A destructive command whose
  # safety depends on a variable being non-empty is not worth the tidiness.
  mkdir -p "$WORK/seg"
  for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    : > "$WORK/seg/silent.$i.log"                       # ZERO BYTES, the measured case
    printf '%s\n' "$D1" > "$WORK/seg/same.$i.log"       # the talking case that already worked
    printf 'child process exited abnormally in file %s\n' "$i" > "$WORK/seg/diff.$i.log"
  done
  # ✔THE INPUT IS ASSERTED, NOT ASSUMED: a `: >` that failed would make the whole
  # demonstration a statement about a file that was not there.
  ck "the silent fixture's log really is ZERO BYTES" "0" \
     "$(LC_ALL=C wc -c < "$WORK/seg/silent.1.log" | tr -d '[:space:]')"
  ck "TWO SILENT SEGMENTS: the engine stops after ONE resume, not $budget" \
     "segments=2 resumes=1 stop=PRECONDITION" "$(drive "$WORK"/seg/silent.*.log)"
  ck "two segments with the SAME diagnostic: same answer" \
     "segments=2 resumes=1 stop=PRECONDITION" "$(drive "$WORK"/seg/same.*.log)"
  # THE NEGATIVE CONTROL. A genuine crash that moves must still spend the whole
  # budget — if this ever reads PRECONDITION the discriminator has become greedy
  # and the resilience rule is gone.
  ck "DIFFERENT diagnostics every time: the whole budget IS spent" \
     "segments=$((budget + 1)) resumes=$budget stop=BUDGET-EXHAUSTED" "$(drive "$WORK"/seg/diff.*.log)"
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
  # `--environment-probes skip` DELIBERATELY: these pins are about the plan's
  # SHAPE, and measuring a clock for 20 s per invocation would put a wall-clock
  # sample inside a self-test the drivers run at STARTUP. It also exercises the
  # skip path, whose plan both drivers must REFUSE to run a corpus on.
  # [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
  python3 "$LEGS_PY" --catalogue "$CATALOGUE" --plan --environment-probes skip \
    --host-os darwin --host-arch arm64 --format sh \
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
  # ★ THE GATING STAMP, WHICH THE SUPPLY NOW REFUSES TO PROCEED WITHOUT. A
  # conditional confound row (`requires: [<environment probe>]`) is honoured only
  # where the probe MEASURED its defect as PRESENT, and an `unprobed` plan is safe
  # but not usable — its withheld excusals would read as compiler regressions.
  # [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
  LEG_CONFOUND_GATING=()
  LEG_CONFOUND_GATING[elf64-x86_64]="probed"
  LEG_CONFOUND_GATING[pe64-x86_64]="probed"
  LEG_CONFOUND_GATING[elf64-arm64]="probed"
  LEG_CONFOUND_GATING[unprobedleg]="unprobed"
  LEG_CONFOUNDS[unprobedleg]="'^busy2-'"
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
  # ★★ AN UNPROBED PLAN IS REFUSED. This is the direction that matters: the plan is
  # already fail-SAFE (every conditional row dropped), so nothing is excused on
  # evidence nobody gathered — but running a corpus on it would surface those
  # withheld excusals as GENUINE reds, i.e. report a broken clock as a compiler
  # regression. Silence here would make an unmeasured run indistinguishable from a
  # measured one. [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
  out="$(leg_confound_patterns unprobedleg 2>&1)"; rc=$?
  ck "an UNPROBED plan REFUSES rather than serving its ungated list" "97" "$rc"
  ck_has "...naming the gating it got" "$out" "confoundGating='unprobed'"
  ck_has "...and how to resolve a measured plan" "$out" "--environment-probes skip"
}

# ═══════════════════════════════════════════════════════════════════════════
# I2 — THE REFUSAL MUST STOP *THE DRIVER*, NOT JUST RETURN NON-ZERO
# ═══════════════════════════════════════════════════════════════════════════
# D-HARNESS-CONFOUND-SUPPLY-REFUSAL-DIES-IN-A-SUBSHELL.
#
# ★★★ WHAT THE PINS ABOVE COULD NOT SEE, AND WHY. Every one of them captures the
# substitution's status DIRECTLY (`out="$(leg_confound_patterns …)"; rc=$?`) — a
# shape the PRODUCTION call site did not have. The driver ran
# `eval "CONFOUND_PATTERNS=($(leg_confound_patterns "$leg"))"`, and `die` is
# `exit 1`, which exits the SUBSHELL a command substitution runs in; under
# `set -Eeuo pipefail` bash does NOT propagate a failed substitution inside a
# NON-ASSIGNMENT command. ✔MEASURED end to end with the shipped region: the
# refusal printed, then `REACHED THE NEXT STATEMENT. CONFOUND_PATTERNS size=0`,
# then the script COMPLETED with rc 0. So the pins proved the FUNCTION refuses
# while nothing proved THE DRIVER STOPS — and the whole corpus would have run with
# an empty confound list, charging every clock failure to the compiler.
#
# ⚠ SO THIS PIN EXTRACTS THE PRODUCTION CALL SITE FROM THE SHIPPED DRIVER rather
# than re-typing a shape of its own. Re-fuse the two statements back into one and
# this pin runs THAT code and reds. It runs in a CHILD bash because the property
# under test is "the shell exits", which cannot be asserted from inside the shell
# that exits.
pin_confound_supply_stops_the_driver() { # pin_confound_supply_stops_the_driver <driver>
  local drv="$1" region callsite script out rc
  region="$(LC_ALL=C sed -n -e '/^# >>> dss:confound-supply >>>$/,/^# <<< dss:confound-supply <<</p' "$drv")"
  if [ -z "$region" ]; then
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || bad "could not extract the dss:confound-supply region from ${drv##*/} — this pin would assert over nothing"
    return 0
  fi
  # THE REAL CALL SITE: from the array declaration through the `eval`, verbatim —
  # STATEMENTS ONLY. ⚠ COMMENT LINES ARE DROPPED, and that is not tidiness: the
  # driver's own comment there QUOTES the fused form it warns against, so an
  # extraction that stopped at the first line MENTIONING the eval captured four
  # comment lines and no code — and the "contains the eval" assertion below passed
  # on the comment. ✔MEASURED while writing this pin: both arms answered
  # `REACHED-NEXT-STATEMENT size=0` because nothing executable had been extracted.
  callsite="$(LC_ALL=C awk '/declare -a CONFOUND_PATTERNS=\(\)/ { p = 1 }
                            p && /^[[:space:]]*#/ { next }
                            p { print; if ($0 ~ /eval "CONFOUND_PATTERNS=\(/) exit }' "$drv")"
  if [ -z "$callsite" ]; then
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || bad "could not extract the CONFOUND_PATTERNS call site from ${drv##*/} — this pin would assert over nothing"
    return 0
  fi
  # THE SUBJECT FIRST, and on its LAST line: the extraction must END at the eval
  # STATEMENT, so a mutation that removes it cannot leave this pin asserting over a
  # prefix of the call site.
  ck "the extracted call site ENDS at the eval statement" "yes" \
     "$(case "$(printf '%s\n' "$callsite" | tail -1)" in *'eval "CONFOUND_PATTERNS=('*) echo yes ;; *) echo no ;; esac)"
  ck_has "...and it carries the supply call itself" "$callsite" 'leg_confound_patterns "$leg"'
  script="$(mktemp "${TMPDIR:-/tmp}/dss-confound-callsite-XXXXXX.sh")"
  # `$LEG_STATE` / `$AFTER` are written by THIS file; everything between them is
  # the shipped driver's own text.
  {
    printf '%s\n' 'set -Eeuo pipefail'
    printf '%s\n' 'declare -A LEG_CONFOUNDS=() LEG_CONFOUND_GATING=() LEG_CONFOUND_DECLARED=()'
    printf '%s\n' 'DSS_CONFOUNDS=""'
    printf '%s\n' 'die() { printf "DIE: %s\n" "$*" >&2; exit 1; }'
    printf '%s\n' 'info() { :; }'
    printf '%s\n' "$region"
    printf '%s\n' 'leg="${1:?leg}"'
    printf '%s\n' 'LEG_CONFOUNDS[$leg]="'"'"'^busy2-'"'"'"'
    printf '%s\n' 'LEG_CONFOUND_GATING[$leg]="${2:?gating}"'
    printf '%s\n' 'LEG_CONFOUND_DECLARED[$leg]=1'
    printf '%s\n' "$callsite"
    printf '%s\n' 'printf "REACHED-NEXT-STATEMENT size=%d\n" "${#CONFOUND_PATTERNS[@]}"'
  } > "$script"
  # ── THE REFUSAL ARM: an unprobed plan must STOP the shell ────────────────
  out="$(bash "$script" someleg unprobed 2>&1)"; rc=$?
  ck "an UNPROBED plan STOPS THE DRIVER at the real call site (rc)" "1" "$rc"
  ck_has "...having said why" "$out" "confoundGating='unprobed'"
  # ⚠ THROUGH `ck`, not a bare `bad`: `bad` bumps the GLOBAL failure count, which
  # under red()'s QUIET=1 would turn an EXPECTED mutant failure into a run-level
  # red. Every pin assertion goes through ck/ck_has for exactly that reason.
  ck "...and the statement AFTER the call site never ran" "no" \
     "$(printf '%s' "$out" | grep -q 'REACHED-NEXT-STATEMENT' && echo yes || echo no)"
  # ── THE NEGATIVE CONTROL: the same script must reach the marker on a
  #    `probed` plan, or the arm above would pass for the wrong reason ──────
  out="$(bash "$script" someleg probed 2>&1)"; rc=$?
  ck "a PROBED plan runs on through the call site (rc)" "0" "$rc"
  ck_has "...and reaches the next statement with the leg's pattern" "$out" "REACHED-NEXT-STATEMENT size=1"
  rm -f "$script"
}

# ═══════════════════════════════════════════════════════════════════════════
# N — WHY A FAILURE WAS EXCUSED IS PRINTED, NOT MERELY DECIDED
# ═══════════════════════════════════════════════════════════════════════════
# D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST.
#
# `earnedOn` failed because it is prose nothing reads. A probe verdict nobody SEES
# is the same failure with extra steps, so the printing is a load-bearing capability
# and this pin treats it as one. The report TEXT is generated once by
# harness_legs.py (and its two-driver agreement is proven by DIFFERENTIAL EXECUTION
# in --check-regions, case `confound-report`); what is pinned HERE is the shipped
# .sh's transport of it, and the refusal that stops an empty report reading as
# "nothing to excuse".
pin_confound_report() { # pin_confound_report <driver>
  local drv="$1" out rc
  load_fns "$drv" print_confound_report || return 0
  # info() is stubbed by this runner, so drive the emission through a local echo:
  # what is under test is WHICH lines survive, not how they are decorated.
  info() { printf 'I %s\n' "$*"; }
  out="$(print_confound_report elf64-x86_64 'probe clock-realtime-steps = ABSENT

row INACTIVE: ^walsetlk-' 2>&1)"; rc=$?
  ck "a report is printed line by line" "0" "$rc"
  ck_has "...the probe verdict line" "$out" "I probe clock-realtime-steps = ABSENT"
  ck_has "...the INACTIVE row line" "$out" "I row INACTIVE: ^walsetlk-"
  ck "...and the BLANK line is skipped, never printed as an empty tag" "2" \
     "$(printf '%s\n' "$out" | grep -c '^I ')"
  # ★ THE REFUSAL: an empty report means the account of WHY a failure was excused
  # did not arrive. An unexplained exclusion is not an earned one, so this must die
  # rather than print nothing and continue.
  out="$(print_confound_report elf64-x86_64 '   ' 2>&1)"; rc=$?
  ck "an EMPTY report REFUSES rather than printing nothing" "97" "$rc"
  ck_has "...naming what is missing" "$out" "EMPTY confound report"
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

# ── K — WHAT A FORWARDED VARIABLE'S VALUE MEANS ON THE OTHER SIDE ───────────
# D-HARNESS-PS1-TCL-LIBRARY-NOT-FORWARDED-ACROSS-THE-WSL-BOUNDARY.
#
# The driver marshals its forward set into THREE declared groups — namespace-
# neutral, driver-path, catalogue-declared — and the counted-group argv that
# carries them is new code with no other coverage. What the RESOLVER then does
# with each group (translate a driver path, refuse an undeclared name) is pinned
# by harness_legs.py's own battery, which both drivers already run at Step 0.
#
# ★ `none` IS THE VERB HERE ON PURPOSE, not to dodge the hard case: it is the
# only verb whose translator exists on every host, so this pin runs identically
# on Linux, macOS and Windows. The TRANSLATION is asserted in the resolver's
# battery against an INJECTED translator, for the same reason.
pin_env_forward() { # pin_env_forward <driver>
  local drv="$1" out
  load_fns "$drv" launch_env_carrier || return 0
  LEG_RESOLVER="$LEGS_PY"; LEG_CATALOGUE="$CATALOGUE"
  export QUICKTEST_OMIT="a,b"
  # ★ A RELATIVE VALUE, AND THAT IS A PORTABILITY FIX, NOT A WEAKENING.
  # ✔MEASURED 2026-08-06: an ABSOLUTE `/opt/tcl/lib/tcl8.6` came back from the
  # resolver as `C:/Program Files/Git/opt/tcl/lib/tcl8.6` — MSYS rewrites a
  # lone POSIX absolute path on its way into native python.exe, so the pin was
  # asserting against Git Bash's path translation rather than against the
  # driver. Same artefact as D-TEST-CONFOUND-SCOPE-SH-CANNOT-RUN-UNDER-GIT-BASH.
  # A relative path is untouched by every host's translator and is still a path;
  # what this pin asserts — the three groups, the filter, the `--forward-path=`
  # spelling and the assignment ORDER — is unchanged by it.
  export TCL_LIBRARY="opt/tcl/lib/tcl8.6"
  export QEMU_LD_PREFIX="usr/aarch64-linux-gnu"
  unset SQLITE_TEST_PATTERN_LIST            # THE FILTER's subject: it is UNSET
  out="$(launch_env_carrier wslenv none "" \
           2 SQLITE_TEST_PATTERN_LIST QUICKTEST_OMIT 1 TCL_LIBRARY)"
  ck "a DRIVER-PATH variable is ASSIGNED, and before the carrier names it" \
     "TCL_LIBRARY=opt/tcl/lib/tcl8.6
WSLENV=QUICKTEST_OMIT:TCL_LIBRARY" "$out"
  case "$out" in
    *SQLITE_TEST_PATTERN_LIST*) ck "an UNSET variable is never carried" "absent" "present" ;;
    *) ck "an UNSET variable is never carried" "absent" "absent" ;;
  esac
  out="$(launch_env_carrier wslenv none "" \
           2 SQLITE_TEST_PATTERN_LIST QUICKTEST_OMIT 1 TCL_LIBRARY QEMU_LD_PREFIX)"
  ck_has "a CATALOGUE-declared launcher variable crosses too" "$out" "QEMU_LD_PREFIX"
  out="$(launch_env_carrier inherit none "" \
           2 SQLITE_TEST_PATTERN_LIST QUICKTEST_OMIT 1 TCL_LIBRARY)"
  ck "a launcher that INHERITS is left byte-for-byte alone" "" "$out"
}

# ═══════════════════════════════════════════════════════════════════════════
# L — THE LAUNCHER-PREREQUISITE GATE
# ═══════════════════════════════════════════════════════════════════════════
# The plan says `launched` because argv[0] RESOLVED. For the arm64 leg on a
# Windows host argv[0] is `wsl.exe` while the program that actually runs the
# artefact is `qemu-aarch64` INSIDE the distro — so the leg passed every gate this
# harness had on a box with no qemu, every unit exited 255 with no diagnostic, and
# fourteen of them were charged to DSS.
#
# ★ THE DRIVER'S REAL GATE IS EXTRACTED AND RUN. What is stubbed is the RESOLVER
# (a fake `harness_legs.py` that answers rc 0 / 3 / 2 on demand) — never the
# driver's classification of that answer, which is the thing under test. A pin
# that re-implemented the classification would stay green while the shipped one
# rotted, and a pin that stubbed the driver would be testing the stub.
#
# ★★ AND THE MET CASE IS ASSERTED AS LOUDLY AS THE UNMET ONE
# (D-HARNESS-UNITS-SKIP-A-LEG-WHOSE-LAUNCHER-IT-SAYS-IS-AVAILABLE). A new gate can
# be made to pass by skipping everything; the mirror assertion — a leg whose
# prerequisites are MET still REACHES the corpus, through the driver's OWN Step-8
# gate sequence — is what makes over-skipping a failure rather than a green.
FAKE_LEGS_PY="$WORK/fake_harness_legs.py"
cat > "$FAKE_LEGS_PY" <<'FAKEPY'
# A stand-in for harness_legs.py --check-launcher ONLY. The outcome is chosen by
# PIN_CHECK_LAUNCHER so one pin can drive every arm; everything else is refused
# loudly rather than answered, so a driver that calls this for some other purpose
# cannot be silently satisfied by it.
import json, os, sys
if "--check-launcher" not in sys.argv:
    sys.stderr.write("fake resolver: asked something other than --check-launcher: %s\n"
                     % " ".join(sys.argv[1:]))
    raise SystemExit(64)
mode = os.environ.get("PIN_CHECK_LAUNCHER", "met")
if mode == "met":
    sys.stdout.write(json.dumps({"label": "lau", "ok": True, "verdict": "",
                                 "missing": [], "uncovered": []}) + "\n")
    raise SystemExit(0)
if mode == "unmet":
    sys.stdout.write(json.dumps({
        "label": "lau", "ok": False,
        "verdict": "skipped-launcher-prerequisite-missing",
        "missing": [{"kind": "command", "path": "qemu-aarch64",
                     "provides": "PIN-PROVIDES", "why": "PIN-WHY",
                     "install": "PIN-INSTALL",
                     "probe": ["wsl.exe", "-e", "sh", "-lc", "command -v qemu-aarch64"]}],
        "uncovered": []}) + "\n")
    raise SystemExit(3)
sys.stderr.write("harness_legs.py: FATAL: the pin asked for an unreadable outcome\n")
raise SystemExit(2)
FAKEPY
extract_env_skip_gate() { # extract_env_skip_gate <driver>
  # The Step-9 ENVIRONMENTAL-skip classifier AND the exit line that makes it
  # fatal. Two regions, because they are two statements in the shipped file and
  # asserting only the first would prove the leg is LISTED, never that strict mode
  # REDS the run.
  LC_ALL=C awk '
    /^declare -a ENV_SKIPS=\(\)$/ { inb = 1 }
    inb { print }
    inb && /^fi$/ { inb = 0; next }
    /^if \[\[ \$\{#ENV_SKIPS\[@\]\} -gt 0 && "\$STRICT_VERDICTS" -eq 1 \]\]; then exit 1; fi$/ { print }' "$1"
}
run_env_skip_gate() { # run_env_skip_gate <gate-text> <strict 0|1> <verdict>  -> prints, rc
  (
    declare -a LEG_DECLARED=(lau)
    declare -A LEG_VERDICT=([lau]="$3")
    STRICT_VERDICTS="$2"
    C_RED=""; C_RST=""
    # ⚠ `warn` is RE-STUBBED here to PRINT. The file-level stub accumulates into
    # $WARNINGS, and $WARNINGS is written in a SUBSHELL and discarded — so the
    # non-strict arm's whole diagnostic would be invisible and the assertion about
    # it would be asserting over nothing. The strict arm printfs directly and would
    # have hidden the difference.
    warn() { printf 'WARN %s\n' "$*"; }
    eval "$1"
    exit 0
  ) 2>&1
}
extract_smoke_rc_case() { # extract_smoke_rc_case <driver>
  LC_ALL=C awk '
    /^  case "\$_srcc" in$/ { inb = 1 }
    inb { print }
    inb && /^  esac$/ { exit }' "$1"
}
extract_ledger_block() { # extract_ledger_block <driver>
  # The Step-9 verdict ledger: LEDGER_VOCAB, the per-leg tally, the counts line
  # and its own accounting-hole detector. The vocabulary there is a HARDCODED
  # MIRROR of harness_legs.py's, so a token the resolver added and this list did
  # not is filed under LEDGER_BOGUS — the ledger reporting a defect in the
  # resolver about a leg that was classified perfectly well.
  LC_ALL=C awk '
    /^declare -A VERDICT_COUNT=\(\)$/ { inb = 1 }
    inb { print }
    inb && /^fi$/ { exit }' "$1"
}
run_ledger_block() { # run_ledger_block <block-text> <verdict>  -> printed + a state line
  (
    declare -a LEG_DECLARED=(lau)
    declare -A LEG_VERDICT=([lau]="$2")
    C_RED=""; C_RST=""
    eval "$1"
    printf 'STATE env=%s hole=%s bogus=[%s] unnamed=[%s]\n' \
      "$LEDGER_ENVIRONMENTAL" "$LEDGER_HOLE" "${LEDGER_BOGUS[*]:-}" "${LEDGER_UNNAMED[*]:-}"
  ) 2>&1
}
run_smoke_rc_case() { # run_smoke_rc_case <case-text> <rc>  -> the recorded verdict
  (
    declare -A CLI_SMOKE_VERDICT=()
    CLI_SMOKE_FAILS=0
    leg=x; _srcc="$2"; _smoke_dir="/pin/out"
    pass() { :; }; warn() { :; }
    eval "$1"
    printf '%s|fails=%s' "${CLI_SMOKE_VERDICT[x]:-<none>}" "$CLI_SMOKE_FAILS"
  )
}
pin_launcher_prereq() { # pin_launcher_prereq <driver>
  local drv="$1" gates rc out envgate rccase ledger v
  if ! command -v python3 >/dev/null 2>&1; then
    [ "$QUIET" -eq 1 ] || skip "L: no python3 — the launcher-prerequisite gate cannot be exercised on this host"
    return 0
  fi
  load_fns "$drv" launcher_prereq_rows apply_launcher_prereq_gate \
                  leg_run_is_skipped unit_verdict_token_known unit_not_run || return 0
  gates="$(LC_ALL=C awk '
    /^  # ── the three ways a leg does not reach the corpus, each already NAMED ──────$/ { inb = 1 }
    /^  bin="\$\{FIXTURE\[\$leg\]\}"/ { inb = 0 }
    inb { print }' "$drv")"
  if [ -z "$gates" ]; then
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || bad "could not extract the Step-8 gate sequence for the launcher pin — it would assert over nothing"
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
  # ⚠ A SECOND ENTRY POINT, BECAUSE THE FIRST RUNS IN A SUBSHELL. `$(run_gates …)`
  # discards every array the gate wrote — including the UNIT ledger entry, which is
  # the whole point of asking. Reading UNIT_VERDICT after the substitution returns
  # the value from BEFORE it, which is exactly how a pin reports success over an
  # effect it could not observe. So the token is printed from INSIDE the subshell.
  run_gates_unit() { # run_gates_unit <leg> -> the recorded unit verdict
    local REACHED=SKIPPED leg
    for leg in "$1"; do
      eval "$gates"
      REACHED=REACHED
    done
    printf '%s' "${UNIT_VERDICT[$1]:-}"
  }
  LEG_RESOLVER="$FAKE_LEGS_PY"; LEG_CATALOGUE="$CATALOGUE"
  HOST_OS=linux; HOST_ARCH=x86_64
  UNIT_VERDICT=(); UNIT_UNCLASSIFIED=0; UNIT_UNCLASSIFIED_LEGS=(); WARNINGS=""

  # ── (3) THE MIRROR: a MET prerequisite REACHES the corpus ─────────────────
  # FIRST, deliberately. A gate that over-skips passes every assertion below it
  # and this is the only one that can see it.
  LEG_RUN_MODE[lau]=launched; LEG_LAUNCH[lau]="wsl.exe -e qemu-aarch64"
  LEG_SPEC[lau]="arm64:elf64-aarch64-linux-exec"
  LEG_TCL_LIB[lau]=/cache/libtcl8.6.so; COMPILE_OK[lau]=1; FIXTURE[lau]=/out/testfixture
  LEG_CC[lau]=""; LEG_VERDICT[lau]=""; LEG_VERDICT_DETAIL[lau]=""
  LEG_RUN_VERDICT[lau]=""; LEG_RUN_DETAIL[lau]=""
  PIN_CHECK_LAUNCHER=met apply_launcher_prereq_gate lau; rc=$?
  ck "a MET prerequisite returns 0"                     "0"        "$rc"
  ck "…and leaves the plan's run mode alone"            "launched" "${LEG_RUN_MODE[lau]}"
  ck "…and records NO skip verdict"                     ""         "${LEG_VERDICT[lau]}"
  ck "★ …and the leg REACHES the corpus"                "REACHED"  "$(run_gates lau)"

  # ── (1) an UNMET prerequisite skips the leg, with the remedy printed ──────
  WARNINGS=""
  LEG_RUN_MODE[lau]=launched
  LEG_VERDICT[lau]=""; LEG_VERDICT_DETAIL[lau]=""
  PIN_CHECK_LAUNCHER=unmet apply_launcher_prereq_gate lau; rc=$?
  ck "an UNMET prerequisite returns 1"  "1" "$rc"
  ck "…and the leg's verdict is the NEW closed-vocabulary token" \
     "skipped-launcher-prerequisite-missing" "${LEG_VERDICT[lau]}"
  ck "…the RUN verdict too, so both artifacts read the same answer" \
     "skipped-launcher-prerequisite-missing" "${LEG_RUN_VERDICT[lau]}"
  ck "…and the run mode is downgraded to skip"  "skip" "${LEG_RUN_MODE[lau]}"
  ck "★ …so the corpus is NOT entered"          "SKIPPED" "$(run_gates lau)"
  ck "…and the unit ledger says so under that same token" \
     "skipped-launcher-prerequisite-missing" \
     "$(v="$(run_gates_unit lau)"; v="${v#not run [}"; printf '%s' "${v%%]*}")"
  ck "…which the driver's OWN closed vocabulary accepts" "known" \
     "$(unit_verdict_token_known skipped-launcher-prerequisite-missing && printf known || printf unknown)"
  # THE REMEDY, not just the diagnosis. All three fields, because a row printed
  # without `install` is a row nobody acts on.
  # ⚠ THE FORMATTED SHAPE, NOT THE BARE VALUE. `PIN-PROVIDES` also occurs in the
  # RAW JSON, which the driver echoes on its unreadable-answer path — so a bare
  # substring assertion is satisfied by a driver that never formatted a row at
  # all, and one of these mutations proves it: F11's .ps1 twin dumps the report
  # verbatim and would have passed three of these four.
  ck_has "…the missing row is named"   "$WARNINGS" "MISSING [command] qemu-aarch64"
  ck_has "…with what it PROVIDES"      "$WARNINGS" "provides: PIN-PROVIDES"
  ck_has "…with WHY it is declared"    "$WARNINGS" "why     : PIN-WHY"
  ck_has "…and with HOW TO INSTALL it" "$WARNINGS" "install : PIN-INSTALL"

  # ── an UNREADABLE answer is never assumed benign ──────────────────────────
  WARNINGS=""; LEG_RUN_MODE[lau]=launched; LEG_VERDICT[lau]=""
  PIN_CHECK_LAUNCHER=boom apply_launcher_prereq_gate lau; rc=$?
  ck "an rc the driver has no arm for returns 2" "2" "$rc"
  ck "…and is POISONED, not passed"  "poisoned" "${LEG_VERDICT[lau]}"
  ck "…and the leg still does not reach the corpus" "SKIPPED" "$(run_gates lau)"

  # ── a leg the plan runs NATIVELY is not this gate's business ──────────────
  LEG_RUN_MODE[nat]=native; LEG_LAUNCH[nat]=""; LEG_VERDICT[nat]=""
  LEG_TCL_LIB[nat]=/cache/libtcl8.6.so; COMPILE_OK[nat]=1; FIXTURE[nat]=/out/testfixture
  LEG_CC[nat]=""; LEG_VERDICT_DETAIL[nat]=""
  PIN_CHECK_LAUNCHER=unmet apply_launcher_prereq_gate nat; rc=$?
  ck "a NATIVE leg is never probed (no launcher to have prerequisites)" "0" "$rc"
  ck "…and keeps its run mode"                                          "native" "${LEG_RUN_MODE[nat]}"

  # ── (2) DSS_STRICT_ARM_VERDICTS=1 turns the skip into a HARD FAILURE ──────
  envgate="$(extract_env_skip_gate "$drv")"
  if [ -z "$envgate" ]; then
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || bad "could not extract the Step-9 ENVIRONMENTAL-skip gate — this pin would assert over nothing"
  else
    out="$(run_env_skip_gate "$envgate" 0 skipped-launcher-prerequisite-missing)"; rc=$?
    ck "by default the new skip WARNS and the run survives" "0" "$rc"
    ck_has "…naming it as environmental" "$out" "ENVIRONMENTAL reason"
    out="$(run_env_skip_gate "$envgate" 1 skipped-launcher-prerequisite-missing)"; rc=$?
    ck "★ under DSS_STRICT_ARM_VERDICTS=1 it is a HARD FAILURE" "1" "$rc"
    ck_has "…and says which variable made it one" "$out" "DSS_STRICT_ARM_VERDICTS=1"
    # THE CONTROL: a STRUCTURAL skip must still survive strict mode, or the
    # assertion above would be satisfied by a gate that fails on everything.
    out="$(run_env_skip_gate "$envgate" 1 skipped-by-runOn)"; rc=$?
    ck "a STRUCTURAL skip is NOT fatal even under strict" "0" "$rc"
  fi

  # ── (4) THE SMOKE GATE'S rc TABLE — every code has its OWN verdict ────────
  # 4 and 2 must NEVER read as an accusation, and 1 must never read as anything
  # else. `*)` is the last resort, not the default verdict.
  rccase="$(extract_smoke_rc_case "$drv")"
  if [ -z "$rccase" ]; then
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || bad "could not extract the Step-7c smoke rc case — this pin would assert over nothing"
  else
    out="$(run_smoke_rc_case "$rccase" 4)"
    ck_has "rc 4 is its own verdict"                    "$out" "NOT A VERDICT"
    case "$out" in *"CHARGED TO DSS"*) ck "…and is NOT charged to DSS" "absent" "present" ;;
                   *) ck "…and is NOT charged to DSS" "absent" "absent" ;; esac
    ck_has "…and still REDS the run"                    "$out" "fails=1"
    out="$(run_smoke_rc_case "$rccase" 2)"
    ck_has "rc 2 is named as OUR argv defect"           "$out" "HARNESS ARGV DEFECT"
    case "$out" in *"CHARGED TO DSS"*) ck "…and is NOT charged to DSS" "absent" "present" ;;
                   *) ck "…and is NOT charged to DSS" "absent" "absent" ;; esac
    ck_has "…and still REDS the run"                    "$out" "fails=1"
    # ★ THE OTHER DIRECTION, WHICH THE ENUMERATION HAD LOST: rc 1 IS the charge.
    # Without an arm it fell into `*)` and printed "NOT charged to DSS" over a
    # matched, attributed compiler failure — a false ACQUITTAL.
    out="$(run_smoke_rc_case "$rccase" 1)"
    ck_has "rc 1 IS charged to DSS"                     "$out" "CHARGED TO DSS"
    case "$out" in *"UNKNOWN rc"*) ck "…and is not reported as an unknown rc" "absent" "present" ;;
                   *) ck "…and is not reported as an unknown rc" "absent" "absent" ;; esac
    out="$(run_smoke_rc_case "$rccase" 0)"
    ck_has "rc 0 passes"                                "$out" "PASS (14/14)"
    ck_has "…and costs the run nothing"                 "$out" "fails=0"
    out="$(run_smoke_rc_case "$rccase" 9)"
    ck_has "a genuinely unknown rc says so, and blames nobody" "$out" "UNKNOWN rc=9"
  fi

  # ── THE STEP-9 LEDGER KNOWS THE TOKEN, AND FILES IT AS ENVIRONMENTAL ──────
  # Its vocabulary is a HARDCODED MIRROR of the resolver's. Executed, not read:
  # "the token appears in the array" is satisfied by a token in the array and
  # nowhere in the counts line, which is how a leg lands in a class no reader
  # sees. ✔MEASURED before this cycle: the token was in NEITHER driver's list.
  ledger="$(extract_ledger_block "$drv")"
  if [ -z "$ledger" ]; then
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || bad "could not extract the Step-9 verdict ledger — this pin would assert over nothing"
  else
    out="$(run_ledger_block "$ledger" skipped-launcher-prerequisite-missing)"
    ck_has "the ledger counts the new token as ENVIRONMENTAL" "$out" "STATE env=1"
    ck_has "…with no accounting hole"                         "$out" "hole=0"
    ck_has "…and it is NOT filed outside the closed vocabulary" "$out" "bogus=[]"
    ck_has "…and the counts line NAMES the class"             "$out" "launcher-prerequisite-missing"
    # THE CONTROL: a token neither list knows must still be caught, or the
    # assertions above would be satisfied by a ledger that accepts anything.
    out="$(run_ledger_block "$ledger" skipped-because-i-said-so)"
    ck_has "an OFF-vocabulary token is still filed as bogus" "$out" "bogus=[lau=skipped-because-i-said-so]"
    ck_has "…and announces the accounting hole"              "$out" "LEDGER ACCOUNTING HOLE"
  fi
}

# ═══════════════════════════════════════════════════════════════════════════
# M — THE SMOKE GATE'S ARGV: WHAT IS MEASURED, AND WHAT IS DECLARED
# ═══════════════════════════════════════════════════════════════════════════
# SOURCE-LEVEL, and deliberately so — the same reason pin_stage_capabilities is:
# the failure being pinned is a MISSING ARGUMENT and a WRONG SOURCE for one, which
# no execution of the existing call site can reveal.
#
# cli-smoke.py compares a DECLARED target (`--leg-spec`, off the plan) against a
# MEASURED one (`--cli-target`, read out of the binary's own header) and reports a
# leg that built the wrong target as its own non-verdict. That comparison is worth
# exactly nothing if the driver feeds it the declaration twice — so the pin asserts
# that `--cli-target` comes from `identify_binary_triple` and not from the spec.
#
# ★ AND THE REFERENCE LAUNCHER MUST COME FROM THE CATALOGUE, keyed on the
# reference's own MEASURED target. The .ps1 twin used to pick it from a HOST
# identity flag, which is why the oracle was unmatched: it ran the reference
# host-native x86_64 while DSS ran arm64 under qemu and charged the difference to
# DSS. This driver's version of the same bug was passing NO reference launcher at
# all — latent only while every host that owns a reference can execute it directly.
extract_smoke_block() { # extract_smoke_block <driver>  -> CODE only, comments dropped
  # ⚠ CODE ONLY. A driver's comment quotes the very expressions these assertions
  # look for, so against the full text a removed line still "passes" — the exact
  # shape that made an earlier pin in this repo vacuously green.
  LC_ALL=C awk '
    /^step "7c\/9/ { inb = 1 }
    /^# ── Step 8 — run the .test UNIT CORPUS/ { inb = 0 }
    inb && $0 !~ /^[[:space:]]*#/ { print }' "$1"
}
pin_smoke_argv() { # pin_smoke_argv <driver>
  local drv="$1" blk
  blk="$(extract_smoke_block "$drv")"
  if [ -z "$blk" ]; then
    PIN_FAILS=$((PIN_FAILS + 1))
    [ "$QUIET" -eq 1 ] || bad "could not extract the Step-7c block — this pin would assert over nothing"
    return 0
  fi
  # THE SUBJECT FIRST: prove the matcher can still SEE the block, or every
  # assertion below becomes a permanent green.
  ck_has "the Step-7c block still spawns the gate" "$blk" 'python3 "${_smoke_argv[@]}"'
  ck_has "the leg's DECLARED spec is passed"       "$blk" '--leg-spec "${LEG_SPEC[$leg]}"'
  ck_has "the subject's target is MEASURED from the binary" "$blk" 'identify_binary_triple "${CLI_BIN[$leg]}"'
  ck_has "…and THAT is what --cli-target carries"  "$blk" '--cli-target "$_cli_target"'
  ck_has "the reference's target is MEASURED too"  "$blk" 'identify_binary_triple "$REF_CLI"'
  ck_has "…and is passed as --reference-target"    "$blk" '--reference-target "$REF_CLI_TARGET"'
  ck_has "the reference launcher comes from the CATALOGUE, keyed on that target" \
         "$blk" 'launcher_argv_for_target "$REF_CLI_TARGET"'
  ck_has "…and every token is spelled with the \`=\` form" "$blk" '--reference-launcher=$_t'
  # ABSENCE, asserted over the CODE: a host-identity branch is what this replaced.
  case "$blk" in
    *HostNeedsWsl*|*'if [[ "$HOST_OS"'*)
      ck "no host-identity branch decides the reference launcher" "absent" "present" ;;
    *) ck "no host-identity branch decides the reference launcher" "absent" "absent" ;;
  esac
}

green "A+B  the not-run recorder + the shared run decision" pin_verdicts
green "C    the Step-8 gate sequence, executed"             pin_step8_gates
green "D    parse_segment keeps the first diagnostic"       pin_parse_segment
# ★ Runs over BOTH drivers, deliberately — the property being pinned is that the
#   two AGREE, so a pin that only ever saw the .sh could not fail on the exact
#   defect it exists for. It is invoked directly rather than through green(),
#   which passes $SH alone.
printf -- '-- %s\n' "D2   the declared capability set reaches every build site, BOTH drivers"
PIN_FAILS=0; QUIET=0
pin_stage_capabilities "$SH" "$HERE/build-and-test.ps1"
[ "$PIN_FAILS" -eq 0 ] || bad "D2 — $PIN_FAILS check(s) failed against the SHIPPED drivers"
green "E    the precondition discriminator"                 pin_precondition
green "F    acq_field over a REAL acquisition record"       pin_acq_field
green "G    the loader variable is TARGET-keyed"            pin_loader_var
green "I    the confound supply is PER LEG"                 pin_confound_supply
green "I2   the supply's REFUSAL stops the DRIVER"          pin_confound_supply_stops_the_driver
green "J    a failed run-dir operation is a VERDICT"        pin_run_dir_argv
green "K    a forwarded PATH crosses through its declared group" pin_env_forward
green "L    the launcher-prerequisite gate"                 pin_launcher_prereq
green "M    the smoke argv: MEASURED targets, DECLARED launcher" pin_smoke_argv
green "N    the confound report is PRINTED, per leg"        pin_confound_report

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

# H4 — drop the "same signature" conjunct. The discriminator becomes greedy and
# the RESILIENCE cases must go red: a genuine crash would stop being resumed.
if mutate "H4 weaken the precondition discriminator" "$WORK/m4.sh" '"$s_zero_sig" == "${prev_zero_sig:-}"' '
    /^    if \[\[ "\$s_nf" -eq 0 && -n "\$s_zero_sig" && "\$s_zero_sig" == "\$\{prev_zero_sig:-\}" \]\]; then$/ {
      print "    if [[ \"$s_nf\" -eq 0 ]]; then"; next }
    { print }'; then
  red "H4 a genuine crash is still RESUMED" pin_precondition "$WORK/m4.sh"
fi

# H4b — THE SILENT-CRASH DEFECT ITSELF, RESTORED. The signature helper stops
# answering for a segment that produced NOTHING, which is exactly the state the
# discriminator was in when elf64-x86_64 burned all ten resumes on a fixture that
# had never started. The talking cases must stay green and the SILENT ones must go
# red — a mutation that reddened everything would prove far less.
# ⚠ THE WITNESS IS THE SENTINEL STRING ITSELF, which appears exactly once in the
#   driver: the surrounding `if`/`fi` and the comment above it survive, so
#   "something changed" cannot stand in for "the sentinel is gone".
if mutate "H4b make a SILENT crash unsignable again" "$WORK/m4b.sh" \
    '<SILENT: the fixture produced no diagnostic, no test result and no test name>' '
    /<SILENT: the fixture produced no diagnostic/ { print "    return 0"; next }
    { print }'; then
  red "H4b a fixture that dies SILENTLY is still diagnosed" pin_precondition "$WORK/m4b.sh"
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

# H8 — forward the DRIVER-PATH group by NAME instead of with its value, which is
# the quieter half of D-HARNESS-PS1-TCL-LIBRARY-NOT-FORWARDED-ACROSS-THE-WSL-
# BOUNDARY: the variable crosses, nothing is translated, and Tcl blames the
# acquisition. The resolver refuses that spelling, so the mutant's helper `die`s
# and the assignment the pin asserts never appears.
if mutate "H8 forward a DRIVER PATH by name, untranslated" "$WORK/m8.sh" 'call+=("--forward-path=$n=${!n}"); carried=1' '
    /^    call\+=\("--forward-path=\$n=\$\{!n\}"\); carried=1$/ {
      print "    call+=(--forward \"$n\"); carried=1"; next }
    { print }'; then
  red "H8 a DRIVER PATH may not be forwarded raw" pin_env_forward "$WORK/m8.sh"
fi

# H9 — count the harness's own teardown results as coverage. This is not a
# hypothetical mutation: it is the ACTUAL defect a first cut of this counter had
# (the teardown lines end in `...` with no space, so an anchored suffix match
# never fired), and it reported ZERO inert files over a corpus with 362.
# ⚠ The WITNESS is the CODE (`$1 !~ /\.test-closeallfiles`), never the bare file
#   name — the name also appears in the comment above the rule, and a witness
#   that survives in a comment is how a removed guard reports itself present.
if mutate "H9 count harness teardown as coverage" "$WORK/m9.sh" '$1 !~ /\.test-closeallfiles' '
    /^    \/ Ok\$\/ +\{ ok\+\+$/ {
      print "    / Ok$/                     { ok++; pend++ }"; skip = 2; next }
    skip > 0 { skip--; next }
    { print }'; then
  red "H9 a file whose only output is teardown asserted NOTHING" pin_parse_segment "$WORK/m9.sh"
fi

# H10 — build a target in $BLD without the declared capability defines. The
# recipe would then be derived from a differently-configured build than the one
# the reference oracle is built from, and STAT4 (which reaches the compiler ONLY
# through `make OPTIONS=`, never through OPT_FEATURE_FLAGS) would vanish with no
# other check in a position to see it.
if mutate "H10 build the fixture without the declared OPTIONS" "$WORK/m10.sh" 'make -s testfixture USE_AMALGAMATION=0 "OPTIONS=$STAGE_MAKE_OPTIONS"' '
    /make -s testfixture USE_AMALGAMATION=0 "OPTIONS=\$STAGE_MAKE_OPTIONS"/ {
      sub(/ "OPTIONS=\$STAGE_MAKE_OPTIONS"/, ""); print; next }
    { print }'; then
  red "H10 every build of \$BLD carries the declared capability defines" pin_stage_capabilities "$WORK/m10.sh"
fi

# H11 — THE DEFECT THIS GATE EXISTS FOR, RESTORED: an UNMET prerequisite answered
# as if it were met. The leg then enters the corpus behind a launcher that cannot
# start the artefact, and every one of its ~330,000 units fails for one reason —
# which is precisely how fourteen assertions came to be charged to the compiler.
# ⚠ THE WITNESS IS THE RECORDING LINE, not the token: `skipped-launcher-
#   prerequisite-missing` appears in the vocabulary mirror, in the ENV_SKIPS case
#   and in prose, so a witness on the bare token would survive its own removal.
if mutate "H11 answer an UNMET launcher prerequisite as met" "$WORK/m11.sh" 'LEG_RUN_VERDICT["$leg"]="skipped-launcher-prerequisite-missing"' '
    /^    3\) rows="\$\(launcher_prereq_rows/ { print "    3) return 0 ;;"; skip = 1; next }
    skip && /^       return 1 ;;$/ { skip = 0; next }
    skip { next }
    { print }'; then
  red "H11 an unmet launcher prerequisite skips the leg" pin_launcher_prereq "$WORK/m11.sh"
fi

# H12 — drop the new token from the ENVIRONMENTAL class at Step 9. The leg is
# still skipped and still ledgered; what disappears is DSS_STRICT_ARM_VERDICTS=1's
# power to red the run over it — a gate that a one-word edit silently disables.
if mutate "H12 declassify the launcher skip as environmental" "$WORK/m12.sh" '    skipped-emulator-missing|skipped-launcher-prerequisite-missing|skipped-build-input-missing) ENV_SKIPS+=("$leg") ;;' '
    /^    skipped-emulator-missing\|skipped-launcher-prerequisite-missing\|skipped-build-input-missing\) ENV_SKIPS\+=\("\$leg"\) ;;$/ {
      print "    skipped-emulator-missing|skipped-build-input-missing) ENV_SKIPS+=(\"$leg\") ;;"; next }
    { print }'; then
  red "H12 strict mode REDS the run over a launcher-prerequisite skip" pin_launcher_prereq "$WORK/m12.sh"
fi

# H13 — remove the smoke gate's rc-1 arm, which is the state the enumeration left
# it in until this cycle: a MATCHED, attributed compiler failure falling into `*)`
# and printing "NOT charged to DSS". The FALSE-ACQUITTAL direction — the one that
# hides a real bug rather than inventing one.
if mutate "H13 take the smoke gate's CHARGED-TO-DSS arm away" "$WORK/m13.sh" '    1) # ★ THE ARM THE ENUMERATION LEFT OUT' '
    /^    1\) # ★ THE ARM THE ENUMERATION LEFT OUT/ { sub(/^    1\)/, "    11)"); print; next }
    { print }'; then
  red "H13 rc 1 from the smoke gate IS the accusation" pin_launcher_prereq "$WORK/m13.sh"
fi

# H14 — remove the new token from the Step-9 LEDGER_VOCAB mirror. This is the
# MEASURED pre-cycle state: harness_legs.py had the token, this hardcoded list did
# not, and a correctly-classified leg was filed under LEDGER_BOGUS as "a verdict
# OUTSIDE the closed vocabulary".
# ⚠ THE WITNESS IS THE VOCABULARY LINE WITH ITS OWN INDENTATION — the bare token
#   appears on six other lines and would survive this removal untouched.
if mutate "H14 drop the token from the ledger vocabulary" "$WORK/m14.sh" '                         skipped-launcher-prerequisite-missing' '
    /^ +skipped-launcher-prerequisite-missing$/ { next }
    { print }'; then
  red "H14 the Step-9 ledger knows the token" pin_launcher_prereq "$WORK/m14.sh"
fi

# H15 — feed the smoke gate the DECLARED spec where the MEASURED target belongs.
# The gate's wrong-target check then compares the declaration with itself and can
# never fire, which is the "a pin that supplies its subject's input by hand is
# testing the stub" shape moved into the shipped harness.
if mutate "H15 pass the declared spec as the measured target" "$WORK/m15.sh" '--cli-target "$_cli_target"' '
    /--cli-target "\$_cli_target"/ { sub(/\$_cli_target/, "${LEG_SPEC[$leg]}"); print; next }
    { print }'; then
  red "H15 --cli-target is MEASURED, never the declaration again" pin_smoke_argv "$WORK/m15.sh"
fi

# H16 — stop asking the catalogue how this host runs the reference. This is the
# .sh's own latent form of the unmatched-oracle defect: no launcher at all, which
# is right until the reference is not host-native.
if mutate "H16 resolve no launcher for the reference" "$WORK/m16.sh" 'launcher_argv_for_target "$REF_CLI_TARGET"' '
    /_lrc=0; launcher_argv_for_target "\$REF_CLI_TARGET" \|\| _lrc=\$\?/ {
      print "    _lrc=0; LAUNCHER_FOR_ARGV=\"\"; LAUNCHER_FOR_WHY=\"\""; next }
    { print }'; then
  red "H16 the reference launcher comes from the catalogue" pin_smoke_argv "$WORK/m16.sh"
fi

# H17 — remove the CONFOUND GATING REFUSAL. The plan is already fail-SAFE without
# it (every conditional row was dropped), so nothing is excused on evidence nobody
# gathered — which is exactly why this guard is easy to lose and why losing it is
# not silent in the right direction: a corpus run on an unprobed plan reports a
# broken host clock as a compiler regression. The pin must notice.
if mutate "H17 remove the confound gating refusal" "$WORK/m17.sh" "LEG_CONFOUND_GATING[\$leg]:-}\" == 'probed'" '
    /== .probed. \]\] \|\| die/ { skip = 1 }
    skip && /A-RUN-MODE-NOT-A-HOST\]"$/ { skip = 0; next }
    !skip { print }'; then
  red "H17 an UNPROBED plan is REFUSED, never served ungated" pin_confound_supply "$WORK/m17.sh"
fi

# H18 — remove the EMPTY-REPORT REFUSAL. Without it a driver whose report never
# arrived prints nothing and carries on, which on a log reads exactly like a leg
# with nothing to excuse. That equivalence is the whole defect this gate exists to
# end one level up (`earnedOn` is prose nothing reads), so an unexplained exclusion
# must not be reachable.
if mutate "H18 remove the empty-report refusal" "$WORK/m18.sh" 'EMPTY confound report' '
    /^  if \[\[ -z "\$\{_report\/\/\[\[:space:\]\]\/\}" \]\]; then$/ { skip = 1 }
    skip && /^  fi$/ { skip = 0; next }
    !skip { print }'; then
  red "H18 an EMPTY confound report is REFUSED, never printed as silence" pin_confound_report "$WORK/m18.sh"
fi

# H19 — RE-FUSE THE CALL SITE. [D-HARNESS-CONFOUND-SUPPLY-REFUSAL-DIES-IN-A-SUBSHELL.]
# This is the defect itself, restored: the supply called from inside a COMMAND
# SUBSTITUTION in a non-assignment command, where `die`'s `exit 1` kills only the
# subshell and `set -Eeuo pipefail` does not propagate the failure. The refusal
# still PRINTS, and every earlier pin in this file still passes, because they
# capture the substitution's rc directly — a shape production never had. Only the
# I2 pin, which drives the real call site in a child shell, can see it.
if mutate "H19 re-fuse the supply call into the eval" "$WORK/m19.sh" \
    'CONFOUND_SUPPLY="$(leg_confound_patterns "$leg")"' '
    /^  CONFOUND_SUPPLY="\$\(leg_confound_patterns "\$leg"\)"$/ { next }
    /^  eval "CONFOUND_PATTERNS=\(\$CONFOUND_SUPPLY\)"$/ {
        print "  eval \"CONFOUND_PATTERNS=($(leg_confound_patterns \"$leg\"))\""; next }
    { print }'; then
  red "H19 the supply's refusal STOPS THE DRIVER" pin_confound_supply_stops_the_driver "$WORK/m19.sh"
fi

printf '\n'
printf 'passed=%d failed=%d skipped=%d\n' "$PASSED" "$FAILED" "$SKIPPED"
[ "$FAILED" -eq 0 ] || exit 1
exit 0
