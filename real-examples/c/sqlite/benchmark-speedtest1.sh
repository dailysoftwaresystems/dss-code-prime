#!/usr/bin/env bash
#
# real-examples/c/sqlite/benchmark-speedtest1.sh
# ─────────────────────────────────────────────────────────────────────────────
# Benchmark DSS Code Prime against gcc/clang (and, on Windows, MSVC) building
# and running SQLite's OWN performance program, `test/speedtest1.c`, FROM FULL
# SOURCE — the ~103 real translation units, not the amalgamation.
#
# Windows companion: benchmark-speedtest1.ps1. The two share EVERYTHING that
# produces a number (speedtest1_bench.py) and everything that derives the
# subject (this file, reachable as `--derive-only`), so they cannot report
# differently. See the pairing note at the bottom.
#
# ★★★ WHY THE SUBJECT IS DERIVED AND NOT WRITTEN DOWN.
# Upstream ships a `speedtest1` make target, and it is an AMALGAMATION build:
# `main.mk`'s recipe links `sqlite3.c`, and `Makefile.msc`'s links `$(SQLITE3C)`.
# ✔MEASURED 2026-08-21 by reading both files. So there is no upstream full-source
# recipe to invoke, and a hand-written list of 103 source files would go stale the
# first time upstream added one.
# What DOES exist is the full-source CLI target `sqlite3d` (`main.mk`:
# `sqlite3d$(T.exe): shell.c $(LIBOBJS0)`), whose recipe this repository already
# derives, ships and proves daily in build-and-test.{sh,ps1}. speedtest1 is that
# program with ONE translation unit swapped: `shell.c` out, `test/speedtest1.c`
# in — both are a `main()` over the same library. So the subject is derived from
# the proven recipe and then substituted, and the substitution is ASSERTED rather
# than assumed (see `--selftest`, and R3 in speedtest1_bench.py).
#
# ★★ ALL THREE ARMS COMPILE THE SAME DEFINE SET, AND THAT IS NOT A CONVENIENCE.
# The recipe is derived by a POSIX `configure`, so its `-D` set carries that
# configure's answers (`HAVE_USLEEP`, `_FILE_OFFSET_BITS` …). Those are wrong for
# a native Windows compiler, which is why the manifest generator implements the
# declared `windows-selfconfig` transform. The transform is applied ONCE, by
# gen-pe64-manifest.py, and this driver then reads the defines and includes back
# OUT of the generated manifest and hands THAT set to gcc and to cl. One set,
# three compilers — otherwise "DSS is faster" could just mean "DSS compiled less".
#
# ★ WHAT THIS DOES NOT EQUALIZE, and says so in the report: DSS compiles every
# CU inside ONE process on a worker-thread pool (`--jobs N`); gcc and cl are
# driven as N concurrent `-c` processes plus a link. That difference IS the
# architecture under measurement.
#
# ⚠ MEASURE WHERE THE CLOCK IS SOUND. This host family's WSL2 CLOCK_REALTIME
# oscillates ±34.47 s every ~5 s (project_wsl2_clock_realtime_broken_2026_08_01),
# and the gcc reference suffers it identically — so a wall-clock differential
# taken there measures the hypervisor. speedtest1_bench.py therefore reads
# `time.monotonic()` only, both reads inside one process, which is valid under
# WSL2 as well. What is NOT valid anywhere is compiling across a UNC/9P share:
# that penalty is real, unequal between toolchains, and refused (R4).
#
# Usage:
#   ./benchmark-speedtest1.sh                       # measure on this host
#   ./benchmark-speedtest1.sh --size 50 --testset main
#   ./benchmark-speedtest1.sh --derive-only --path-style windows --plan p.json
#   ./benchmark-speedtest1.sh --selftest
#
# Exit codes: 0 measured · 1 a refusal · 2 usage · 3 no arm produced a binary.
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

C_RST=$'\033[0m'; C_RED=$'\033[31m'; C_GRN=$'\033[32m'
C_YLW=$'\033[33m'; C_BLU=$'\033[34m'
step()  { printf '\n%s== %s ==%s\n' "$C_BLU" "$*" "$C_RST"; }
info()  { printf '   %s\n' "$*"; }
pass()  { printf '%s ✓ %s%s\n' "$C_GRN" "$*" "$C_RST"; }
warn()  { printf '%s ! %s%s\n' "$C_YLW" "$*" "$C_RST"; }
die()   { printf '%s ✗ ERROR: %s%s\n' "$C_RED" "$*" "$C_RST" >&2; exit 1; }
usage() { printf '%s ✗ USAGE: %s%s\n' "$C_RED" "$*" "$C_RST" >&2; exit 2; }

# ── THE SHARED CORE ──────────────────────────────────────────────────────────
# Sourced unguarded, exactly as build-and-test.sh sources it and for the same
# reason: a driver whose shared core is missing must not limp on with a private
# copy of half of it. `dss_bh_emit_recipe` is the ONE implementation of "what
# does the reference build actually compile", and re-typing it here is how the
# three-way drift that produced base-harness.sh started.
BASE_HARNESS="$SCRIPT_DIR/base-harness.sh"
[[ -r "$BASE_HARNESS" ]] || die "the shared harness core is missing: $BASE_HARNESS
      It carries the recipe derivation this benchmark's subject is built from.
      Restore it rather than reintroducing a private copy."
# shellcheck source=/dev/null
source "$BASE_HARNESS"

MANIFEST_GEN="$SCRIPT_DIR/gen-pe64-manifest.py"
LEG_RESOLVER="$SCRIPT_DIR/harness_legs.py"
LEG_CATALOGUE="$SCRIPT_DIR/legs.json"
BENCH_CORE="$SCRIPT_DIR/speedtest1_bench.py"

# ── defaults ─────────────────────────────────────────────────────────────────
SQLITE_DIR="${SQLITE_DIR:-$HOME/src/sqlite}"
SRC_DIR="${SRC_DIR:-$HOME/src/dss-code-prime}"
DSS_BIN="${DSS_BIN:-}"
OUT_DIR=""
WORK_SIZE=25
TESTSET=""
BUILD_REPEATS=3
RUN_REPEATS=5
JOBS_ARMS="1 4"
TARGET_SPEC=""
RECIPE_TRANSFORM=""
STACK_RESERVE=""
DERIVE_ONLY=0
PATH_STYLE="posix"
PLAN_OUT=""
SELFTEST=0
REF_CC="${CC:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sqlite-dir)     SQLITE_DIR="$2"; shift 2 ;;
    --dss-src)        SRC_DIR="$2"; shift 2 ;;
    --dss)            DSS_BIN="$2"; shift 2 ;;
    --out)            OUT_DIR="$2"; shift 2 ;;
    --size)           WORK_SIZE="$2"; shift 2 ;;
    --testset)        TESTSET="$2"; shift 2 ;;
    --build-repeats)  BUILD_REPEATS="$2"; shift 2 ;;
    --run-repeats)    RUN_REPEATS="$2"; shift 2 ;;
    --jobs-arms)      JOBS_ARMS="$2"; shift 2 ;;
    --target)         TARGET_SPEC="$2"; shift 2 ;;
    --recipe-transform) RECIPE_TRANSFORM="$2"; shift 2 ;;
    --stack-reserve)  STACK_RESERVE="$2"; shift 2 ;;
    --cc)             REF_CC="$2"; shift 2 ;;
    --derive-only)    DERIVE_ONLY=1; shift ;;
    --path-style)     PATH_STYLE="$2"; shift 2 ;;
    --plan)           PLAN_OUT="$2"; shift 2 ;;
    --selftest)       SELFTEST=1; shift ;;
    -h|--help)        sed -n '2,60p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) usage "unknown argument '$1'. --help lists what this accepts." ;;
  esac
done

[[ "$PATH_STYLE" == posix || "$PATH_STYLE" == windows ]] \
  || usage "--path-style takes 'posix' or 'windows', got '$PATH_STYLE'."

# ── path style ───────────────────────────────────────────────────────────────
# ★ THE TRANSLATION LIVES HERE, ON THE POSIX SIDE, BECAUSE THIS IS THE SIDE THAT
# HAS THE TOOL. The PowerShell twin calls this script through `wsl.exe` to derive
# the subject and then compiles NATIVELY, so every derived path has to cross from
# `/mnt/c/...` to `C:\...`. Hand-rolling that mapping in PowerShell would be a
# second implementation of a translation `wslpath` already does correctly
# (including for paths that are NOT under /mnt), and getting it subtly wrong
# produces a "file not found" three minutes into a build rather than here.
# REFUSED rather than approximated when no translator exists: a wrong path that
# happens to resolve is worse than a stop.
to_style() {                    # to_style <path> -> stdout
  local p="$1"
  [[ "$PATH_STYLE" == posix ]] && { printf '%s\n' "$p"; return 0; }
  if command -v wslpath >/dev/null 2>&1; then wslpath -w "$p"
  elif command -v cygpath >/dev/null 2>&1; then cygpath -w "$p"
  else
    die "--path-style windows needs a path translator and this shell has neither
      wslpath nor cygpath. Run the derivation from WSL or from Git Bash; do not
      let the caller guess the mapping."
  fi
}

# ── self-test ────────────────────────────────────────────────────────────────
# The substitution is the ONE thing this driver does to an otherwise proven
# recipe, so it is the one thing that gets its own arm. Exercised, never read:
# a substitution that silently did nothing would leave `shell.c` in place and
# benchmark the sqlite3 CLI under the name speedtest1.
substitute_main_tu() {          # substitute_main_tu <tus-file> <speedtest1.c>
  local f="$1" st1="$2" before after
  before="$(grep -cE '/shell\.c$' "$f" || true)"
  [[ "$before" == 1 ]] || return 3
  grep -vE '/shell\.c$' "$f" > "$f.tmp"
  printf '%s\n' "$st1" >> "$f.tmp"
  mv "$f.tmp" "$f"
  after="$(grep -cE '/shell\.c$' "$f" || true)"
  [[ "$after" == 0 ]] || return 4
  grep -qxF "$st1" "$f" || return 5
  return 0
}

if [[ $SELFTEST == 1 ]]; then
  step "benchmark-speedtest1.sh — self-test"
  fails=0
  t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
  ck() { if [[ "$2" == ok ]]; then printf '   ok    %s\n' "$1"; else printf '   FAIL  %s\n' "$1"; fails=$((fails+1)); fi; }

  printf '%s\n' /s/src/main.c /s/src/shell.c /s/src/util.c > "$t/tus"
  if substitute_main_tu "$t/tus" /s/test/speedtest1.c; then r=ok; else r=no; fi
  ck "substitution reports success on a well-formed TU list" "$r"
  ck "shell.c is gone"      "$(grep -qE '/shell\.c$' "$t/tus" && echo no || echo ok)"
  ck "speedtest1.c is in"   "$(grep -qxF /s/test/speedtest1.c "$t/tus" && echo ok || echo no)"
  ck "the other TUs survive" "$([[ $(wc -l < "$t/tus") == 3 ]] && echo ok || echo no)"

  # The complement: a list with NO shell.c must be REFUSED, not silently
  # appended to. That is the case where the recipe derivation changed shape and
  # the benchmark would otherwise have measured a library with two `main`s or
  # none.
  printf '%s\n' /s/src/main.c /s/src/util.c > "$t/tus2"
  if substitute_main_tu "$t/tus2" /s/test/speedtest1.c; then r=no; else r=ok; fi
  ck "a TU list with no shell.c is REFUSED" "$r"
  printf '%s\n' /s/src/shell.c /s/x/shell.c > "$t/tus3"
  if substitute_main_tu "$t/tus3" /s/test/speedtest1.c; then r=no; else r=ok; fi
  ck "a TU list with TWO shell.c is REFUSED" "$r"

  ck "the shared measurement core is present" "$([[ -r $BENCH_CORE ]] && echo ok || echo no)"
  ck "the manifest generator is present"      "$([[ -r $MANIFEST_GEN ]] && echo ok || echo no)"
  ck "the shared harness core exports dss_bh_emit_recipe" \
     "$(declare -F dss_bh_emit_recipe >/dev/null && echo ok || echo no)"

  step "self-test: the measurement core's own arms"
  python3 "$BENCH_CORE" --selftest || fails=$((fails+1))
  [[ $fails == 0 ]] || die "$fails self-test arm(s) FAILED"
  pass "all self-test arms green"; exit 0
fi

# ── Step 1 — resolve the subject and the toolchains ──────────────────────────
step "1/6  Resolve the subject, the compiler and the reference toolchain"
[[ -d "$SQLITE_DIR" ]] || die "no SQLite checkout at $SQLITE_DIR
      Point --sqlite-dir (or \$SQLITE_DIR) at one, or clone sqlite/sqlite there.
      This driver uses the checkout AS-IS and never switches or pulls it — a
      probe measures the tree exactly as it stands."
SPEEDTEST_C="$SQLITE_DIR/test/speedtest1.c"
[[ -f "$SPEEDTEST_C" ]] || die "the benchmark's subject is missing: $SPEEDTEST_C
      That file IS SQLite's own performance program; without it there is nothing
      to measure and no substitute worth inventing."
SQLITE_HEAD="$(cd "$SQLITE_DIR" && git rev-parse --short HEAD 2>/dev/null || echo UNKNOWN)"
info "sqlite    : $SQLITE_DIR  (upstream $SQLITE_HEAD)"

for tool in python3 make; do
  command -v "$tool" >/dev/null 2>&1 || die "'$tool' is not on PATH; the recipe derivation needs it."
done
# ⚠ tclsh is a BUILD-HOST tool here, not a target dependency: sqlite generates
# opcodes.c/h and parse.c through Tcl scripts. speedtest1 itself links no Tcl at
# all (it includes sqlite3.h and libc, nothing else — ✔MEASURED by reading its
# includes), which is exactly why this benchmark needs no library resolution
# while the testfixture harness needs two.
command -v tclsh >/dev/null 2>&1 || die "'tclsh' is not on PATH.
      SQLite's own build generates opcodes.c/parse.c through Tcl scripts, so the
      recipe cannot be derived without it. apt: tcl. brew: tcl-tk.
      (speedtest1 itself links no Tcl — this is a build-host tool.)"

# ★ THE BINARY IS `dss-code-prime`, NOT `dss`, AND SEARCHING FOR THE WRONG NAME
# LOOKS EXACTLY LIKE "NOT BUILT YET". The compiler is `dss-code-prime[.exe]`
# (a rename to `dsscp` is queued but has not landed); build-and-test.sh resolves
# it by `find … -name dss-code-prime -perm -u+x`, and this does the same rather
# than carrying a second list of guessed paths. Preference is rel over dbg,
# because a debug compiler's build time is not a number worth publishing.
if [[ -z "$DSS_BIN" ]]; then
  for _tree in rel dbg; do
    DSS_BIN="$(find "$SRC_DIR/build/$_tree" -type f -name 'dss-code-prime*' \
                 -perm -u+x -print -quit 2>/dev/null || true)"
    [[ -n "$DSS_BIN" ]] && break
  done
  [[ -n "$DSS_BIN" ]] || DSS_BIN="$(find "$SRC_DIR/build" -type f -name 'dss-code-prime*' \
                                      -perm -u+x -print -quit 2>/dev/null || true)"
fi
# ⚠ TWO DIFFERENT FACTS, TWO DIFFERENT MESSAGES. "You passed a path that is not
# executable" and "nothing was found under the tree I searched" have different
# remedies, and a single message for both sends the reader to build a compiler
# they already have. An instrument that misattributes is the failure this project
# cares most about.
if [[ -n "${DSS_BIN:-}" && ! -x "$DSS_BIN" ]]; then
  die "the dss-code-prime path given is not an executable file: $DSS_BIN
      (the compiler is named 'dss-code-prime[.exe]', not 'dss')"
fi
[[ -n "${DSS_BIN:-}" ]] || die "no dss-code-prime binary found.
      Pass --dss <path>, or build one: scripts/local-build/local-build.sh --tree rel
      Searched for an executable named dss-code-prime* under $SRC_DIR/build/."
info "dss       : $DSS_BIN"

# The reference C compiler. A benchmark that silently picked whichever `cc`
# happened to be first would compare against an unnamed toolchain; the version
# is captured and printed in the report next to its number.
[[ -n "$REF_CC" ]] || REF_CC="$(command -v gcc || command -v clang || true)"
[[ -n "$REF_CC" ]] || die "no reference C compiler found (looked for gcc, then clang).
      Pass --cc <path>. A benchmark with no reference is not a comparison."
REF_CC_VERSION="$("$REF_CC" --version 2>/dev/null | head -1 || echo unknown)"
info "reference : $REF_CC  ($REF_CC_VERSION)"

# ★★ THE CONFIG ROOT IS PINNED, AND THE COMPILER IS PROVEN TO WORK WITH IT
# BEFORE ANY MINUTES ARE SPENT. Two facts, one probe.
#   · PINNED: `findShippedConfig` walks up from the CWD unless DSS_CONFIG_ROOT
#     says otherwise, so an unpinned benchmark pairs the compiler with whichever
#     config tree sits above wherever it was launched. A measurement of "this
#     binary" has to say which config that binary read.
#   · PROVEN: ✔MEASURED 2026-08-21, a two-day-stale `build/rel` against the
#     CURRENT config refused with `unknown key 'templateLabelRule' in 'assembly'`
#     — a real, correctly-named refusal, but it arrived AFTER a configure, a
#     102-object reference build and a recipe derivation. The same refusal costs
#     one second here, and it is the difference between "the benchmark told me my
#     compiler is stale" and "the benchmark failed".
DSS_CONFIG_ROOT_PIN="${DSS_CONFIG_ROOT:-$SRC_DIR/src/dss-config}"
[[ -d "$DSS_CONFIG_ROOT_PIN" ]] || die "no dss config root at $DSS_CONFIG_ROOT_PIN
      Pass --dss-src pointing at the checkout the compiler was built from, or set
      DSS_CONFIG_ROOT. Leaving it unset would let the CWD decide which config the
      measured binary reads."
# ⚠ SKIPPED — AND SAID — WHEN THE COMPILER IS NOT NATIVE TO THIS SHELL. Under
# `--derive-only --path-style windows` this script is deriving on behalf of the
# PowerShell twin, so `$DSS_BIN` is a Windows `.exe` and only the twin's host can
# run it. The twin runs the SAME check, from the same implementation, before it
# calls here. Silence would be the harness bug; a named skip is not.
if [[ $DERIVE_ONLY == 1 && "$PATH_STYLE" == windows ]]; then
  info "dss pre-flight: skipped — the compiler is native to the CALLING host, which"
  info "                runs this same check (speedtest1_bench.py --preflight-dss)"
elif python3 "$BENCH_CORE" --preflight-dss "$DSS_BIN" --config-root "$DSS_CONFIG_ROOT_PIN"; then
  :
else
  die "the dss pre-flight refused (its diagnostic is above). Nothing is measured
      against a compiler that cannot compile three lines. Rebuild it:
        scripts/local-build/local-build.sh --tree rel"
fi

OUT_DIR="${OUT_DIR:-$SQLITE_DIR/bld-dss-bench}"
mkdir -p "$OUT_DIR"
BLD="$OUT_DIR/sqlite-build"
mkdir -p "$BLD"
info "output    : $OUT_DIR"

# ── Step 2 — the declared capability set ─────────────────────────────────────
step "2/6  Resolve the stage capabilities (harness_legs.py --stage-build)"
# ★ READ FROM THE CATALOGUE, NOT SPELLED HERE. Which extensions SQLite is built
# with is declared in exactly one place for this whole repository, and a
# benchmark configured with fts5/rtree OFF would be measuring a different, much
# smaller SQLite than the corpus tests — while reporting under the same name.
# stderr to its own file for the reason build-and-test.sh states: this output is
# eval'd, so a diagnostic merged into it would be executed as shell.
_sb_err="$(mktemp)"
if _sb_out="$(python3 "$LEG_RESOLVER" --catalogue "$LEG_CATALOGUE" \
                --stage-build --format sh 2>"$_sb_err")"; then _sb_rc=0; else _sb_rc=$?; fi
if [[ $_sb_rc -ne 0 ]]; then
  _sb_msg="$(cat "$_sb_err" 2>/dev/null || true)"; rm -f "$_sb_err"
  die "could not resolve the sqlite stage build configuration (rc=$_sb_rc):
$(printf '%s\n' "$_sb_msg" | sed 's/^/      /')"
fi
rm -f "$_sb_err"
eval "$_sb_out"
STAGE_CONFIGURE_FLAGS="${DSS_STAGE_CONFIGURE_FLAGS:-}"
STAGE_MAKE_OPTIONS="${DSS_STAGE_MAKE_OPTIONS:-}"
[[ -n "$STAGE_CONFIGURE_FLAGS" ]] || die \
  "the resolver exited 0 but set no configure flags — a contract break between
      harness_legs.py and this driver, not a property of this host."
info "capabilities: $STAGE_CONFIGURE_FLAGS${STAGE_MAKE_OPTIONS:+   make OPTIONS=$STAGE_MAKE_OPTIONS}"

# ── Step 3 — configure + build the reference, which generates the sources ────
step "3/6  Configure SQLite and build the reference full-source CLI"
JOBS_HOST="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
# shellcheck disable=SC2206
_cfg_args=($STAGE_CONFIGURE_FLAGS)
( cd "$BLD" && "$SQLITE_DIR/configure" "${_cfg_args[@]}" ) > "$OUT_DIR/configure.log" 2>&1 \
  || die "sqlite configure FAILED — see $OUT_DIR/configure.log"
# `sqlite3d` is the FULL-SOURCE CLI (main.mk: `sqlite3d$(T.exe): shell.c
# $(LIBOBJS0)`). Building it for real is what generates parse.c / opcodes.c /
# ctime.c / fts5.c — the derived sources the recipe will name and that no
# checkout carries.
#
# ★★ `libsqlite3.a` IS BUILT TOO, AND `USE_AMALGAMATION=0` ON IT IS LOAD-BEARING.
# Two measurements, both taken 2026-08-21 while writing this, both of which the
# derivation caught by NAME rather than by producing a wrong answer:
#   · `make sqlite3d` alone produces 102 loose `.o` and NO archive. The recipe's
#     link line then names those objects, and the derivation has object NAMES
#     with no way back to sources — `dss_bh_archive_tus` (`ar t` + a search-root
#     lookup) IS that mapping, and it needs an archive to exist. It refused.
#   · `make libsqlite3.a` WITHOUT `USE_AMALGAMATION=0` produces an archive with
#     exactly ONE member, `sqlite3.o` — the AMALGAMATION. The derivation then
#     recovered 1 TU of 103 and the floor stopped it. ★ That is the sharper of
#     the two: a green archive of the right name holding the wrong thing is how
#     this benchmark would have silently measured the amalgamation it exists to
#     avoid, under a full-source label.
# build-and-test.sh never meets either because it builds `testfixture
# USE_AMALGAMATION=0` first, which links the correct archive into being as a side
# effect — an input this benchmark would otherwise have inherited by luck.
if ( cd "$BLD" && make -s sqlite3d libsqlite3.a USE_AMALGAMATION=0 \
        "OPTIONS=$STAGE_MAKE_OPTIONS" -j"$JOBS_HOST" ) \
     > "$OUT_DIR/reference-build.log" 2>&1; then
  pass "reference full-source CLI built (its derived sources are now on disk)"
else
  warn "the reference sqlite3d did not fully link — continuing, because what this"
  warn "step is FOR is the generated sources; the recipe floors below are the honest"
  warn "gate on whether enough of it succeeded. See $OUT_DIR/reference-build.log"
fi

# ── Step 4 — derive the full-source recipe, then substitute the main TU ──────
step "4/6  Derive the full-source recipe and substitute speedtest1.c for shell.c"
RECIPE="$OUT_DIR/sqlite3d-recipe.txt"
TUS_FILE="$OUT_DIR/tus.txt"
DEFS_FILE="$OUT_DIR/defines.txt"
INCS_FILE="$OUT_DIR/includes.txt"
AR="$BLD/libsqlite3.a"; [[ -f "$BLD/.libs/libsqlite3.a" ]] && AR="$BLD/.libs/libsqlite3.a"
# ★ THE ARCHIVE IS ASSERTED BY NAME, NOT BY COUNT, and this is the assertion the
# TU floor cannot make. An archive built without `USE_AMALGAMATION=0` holds one
# member — `sqlite3.o`, the amalgamation — under exactly the right file name. The
# floor happens to catch THAT case because one is far below a hundred, but a
# count can never say WHICH thing is in there, and "the amalgamation is present"
# is the one fact that would silently invert this benchmark's whole subject.
if [[ -f "$AR" ]] && ar t "$AR" 2>/dev/null | grep -qx 'sqlite3.o'; then
  die "the archive $AR contains 'sqlite3.o' — that is the AMALGAMATION object, so
      this tree was built without USE_AMALGAMATION=0. Recovering the subject from
      it would benchmark the amalgamation under a full-source label, which is the
      one thing this benchmark exists not to do. Delete the archive and re-run."
fi
# The arguments are build-and-test.sh's proven CLI call site, verbatim in shape.
# `--always-make 1` + `--token-scope recipe` are load-bearing and their whole
# rationale is at that call site: without them the -D set is read off the link
# line alone and loses SQLITE_CORE, after which ext/icu/icu.c stops compiling to
# nothing and demands <unicode/*.h>.
if _summary="$(dss_bh_emit_recipe \
      --build-dir "$BLD" --make-target sqlite3d --recipe-file "$RECIPE" \
      --make-var "OPTIONS=$STAGE_MAKE_OPTIONS" \
      --prereq-mode link-line --always-make 1 --token-scope recipe \
      --archive "$AR" --archive-from-span 1 \
      --search-root "$SQLITE_DIR/src" --search-root "$SQLITE_DIR/ext" --search-root "$BLD" \
      --min-tus 100 --min-defines 18 \
      --out-tus "$TUS_FILE" --out-defines "$DEFS_FILE" --out-includes "$INCS_FILE")"; then
  pass "recipe: $_summary"
else
  die "the full-source recipe derivation FAILED — see $RECIPE and the diagnostic above."
fi

# SQLITE_CORE by NAME, for the reason the CLI call site records: its absence is
# silent in every count, and turns into four `error[F001A] got unicode/*.h`
# minutes later that read as a missing system dependency.
grep -qx 'SQLITE_CORE' "$DEFS_FILE" || die \
  "the derived define set has no SQLITE_CORE. Without it ext/icu/icu.c demands
      <unicode/*.h>. That means the -D tokens came off the link line alone: check
      that --always-make and --token-scope recipe survived. Recipe: $RECIPE"

case "$(substitute_main_tu "$TUS_FILE" "$SPEEDTEST_C" && echo 0 || echo $?)" in
  0) : ;;
  3) die "the derived TU set does not carry exactly one shell.c, so there is nothing
      to substitute. That is a change in the reference recipe's shape, not a
      benchmark option — a TU list without it would build a library with no main.
      Recipe: $RECIPE" ;;
  4|5) die "the main-TU substitution did not take effect on $TUS_FILE — refusing to
      benchmark a subject whose identity is unproven." ;;
  *) die "the main-TU substitution failed for an unclassified reason." ;;
esac
TU_COUNT="$(wc -l < "$TUS_FILE" | tr -d ' ')"
pass "subject: $TU_COUNT full-source TUs, main = test/speedtest1.c"

# ★★ THE BUILD DIRECTORY IS APPENDED TO THE INCLUDE LIST, AND IT IS NOT OPTIONAL.
# `make -n` runs its compiles FROM $BLD, so the recipe spells that directory as
# `-I.` — a relative path that means nothing once the compiler is invoked from
# anywhere else, and which the derivation therefore drops. Everything GENERATED
# lives there: sqlite3.h, opcodes.h, parse.h, keywordhash.h, sqlite_cfg.h.
# ✔MEASURED 2026-08-21 by leaving it out: DSS said
# `error[P0016] quote include not found: sqlite3.h` and gcc failed on parse.c —
# both arms, immediately, which is the good failure. build-and-test.sh supplies
# the same directory through its own $INC_DIRS_TAIL.
# LAST, matching that driver: this list's earlier entries are SOURCE dirs, and a
# generated header must never be in a position to shadow one.
grep -qxF "$BLD" "$INCS_FILE" || printf '%s\n' "$BLD" >> "$INCS_FILE"
INC_COUNT="$(wc -l < "$INCS_FILE" | tr -d ' ')"
# ⚠ speedtest1 needs NO staged third-party headers at all — no Tcl (it is not the
# testfixture) and no zlib (that is shell.c's, and shell.c is the TU we just
# removed). ✔MEASURED by reading its includes: sqlite3.h plus libc. That is why
# this driver has no header-staging apparatus while build-and-test.sh needs one.
grep -qE 'sqlite3\.h' "$BLD/sqlite3.h" >/dev/null 2>&1 || [[ -f "$BLD/sqlite3.h" ]] || die \
  "the generated $BLD/sqlite3.h is not there, so the reference build did not get far
      enough to produce it. Every arm would fail on the first TU. See $OUT_DIR/reference-build.log"
info "includes  : $INC_COUNT dirs (the six sqlite src/ext dirs + the generated-header dir)"

# ── Step 5 — the manifest, and the ONE define set all three arms compile ─────
step "5/6  Generate the DSS project manifest (it also fixes the shared define set)"
if [[ -z "$TARGET_SPEC" ]]; then
  case "$(uname -s)" in
    Linux)  TARGET_SPEC="$(uname -m | sed 's/aarch64/arm64:elf64-aarch64-linux-exec/;s/x86_64/x86_64:elf64-x86_64-linux-exec/')" ;;
    Darwin) TARGET_SPEC="$(uname -m | sed 's/arm64/arm64:macho64-arm64-darwin-exec/;s/x86_64/x86_64:macho64-x86_64-darwin-exec/')" ;;
    MINGW*|MSYS*|CYGWIN*) TARGET_SPEC="x86_64:pe64-x86_64-windows-exec" ;;
    *) die "this host's uname ($(uname -s)) has no default target spec here; pass --target." ;;
  esac
fi
# The transform follows the TARGET, matching the catalogue's own declaration:
# every pe64 leg declares `windows-selfconfig`, every POSIX leg `none`.
[[ -n "$RECIPE_TRANSFORM" ]] || \
  { case "$TARGET_SPEC" in *pe64*) RECIPE_TRANSFORM=windows-selfconfig ;; *) RECIPE_TRANSFORM=none ;; esac; }
# stackReserve: 0 OMITS the key, which is what the POSIX formats need (they
# declare no stack-reserve capability, so a request they cannot carry is refused
# outright). The pe64 figure is the catalogue's, and its evidence is in
# gen-pe64-manifest.py.
[[ -n "$STACK_RESERVE" ]] || \
  { case "$TARGET_SPEC" in *pe64*) STACK_RESERVE=8388608 ;; *) STACK_RESERVE=0 ;; esac; }
# ★★ THE REFERENCE LINK FLAGS FOLLOW THE **TARGET**, NEVER THE HOST — the same
# rule `D-HARNESS-CROSS-HOST-ANY-TARGET` states for leg selection, and it matters
# here for a concrete reason: the PowerShell twin runs this derivation inside WSL,
# so `uname -s` says Linux while the artifact being measured is a pe64 Windows
# binary. Keying on the host would hand MinGW `-lpthread -ldl`, which it does not
# have, and the gcc arm would "fail to build" for a reason that is purely the
# deriving shell's identity.
# SQLITE_THREADSAFE=1 is in the derived define set, which is what makes pthread
# necessary on the ELF legs; Darwin carries pthread and dl in libSystem; the
# Windows CRT supplies both. speedtest1 links no zlib and no Tcl.
case "$TARGET_SPEC" in
  *elf64*)  REF_LINK_FLAGS='["-lm", "-lpthread", "-ldl"]' ;;
  *macho*)  REF_LINK_FLAGS='["-lm"]' ;;
  *pe64*)   REF_LINK_FLAGS='[]' ;;
  *) die "no reference link flags are declared for target '$TARGET_SPEC'.
      Add the case rather than defaulting: a silently empty link line fails at
      the linker with an undefined symbol that reads like a codegen bug." ;;
esac
MANIFEST="$OUT_DIR/speedtest1.dss-project.json"
dss_bh_generate_manifest "$MANIFEST_GEN" "$MANIFEST" speedtest1 "$TARGET_SPEC" \
  "$TUS_FILE" "$INCS_FILE" "$DEFS_FILE" "$RECIPE_TRANSFORM" "$STACK_RESERVE" \
  || die "manifest generation FAILED for $TARGET_SPEC"
info "target    : $TARGET_SPEC   (recipe transform: $RECIPE_TRANSFORM)"

# ── Step 6 — the plan, and the measurement ───────────────────────────────────
step "6/6  Write the benchmark plan"
# ★ THE PLAN'S sources/includes/defines ARE READ BACK OUT OF THE MANIFEST, not
# out of the recipe files. That is the whole mechanism by which the three arms
# provably compile the same thing: the manifest is where the declared recipe
# transform has already been applied, so gcc, cl and dss are handed one set that
# one program produced, rather than three sets three call sites believe are equal.
PLAN_OUT="${PLAN_OUT:-$OUT_DIR/benchmark-plan.json}"
STYLE="$PATH_STYLE" REF_LINK_FLAGS="$REF_LINK_FLAGS" CFG_ROOT="$DSS_CONFIG_ROOT_PIN" \
python3 - "$MANIFEST" "$PLAN_OUT" "$OUT_DIR" "$DSS_BIN" "$REF_CC" \
    "$REF_CC_VERSION" "$SQLITE_DIR" "$SQLITE_HEAD" "$TARGET_SPEC" \
    "$WORK_SIZE" "$TESTSET" "$BUILD_REPEATS" "$RUN_REPEATS" "$JOBS_ARMS" <<'PY'
import json, os, re, subprocess, sys
(manifest_p, plan_p, outdir, dss, cc, cc_version, sqlite_dir, sqlite_head,
 target, size, testset, brep, rrep, jobs) = sys.argv[1:]

# The reference's version, for the report. ⚠ NOT `split()[2]`: the three shipped
# spellings put the number in three different places —
#   gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
#   gcc.exe (MinGW-W64 x86_64-ucrt-posix-seh, built by …, r3) 13.2.0
#   clang version 18.1.3 (…)
# and a positional pick reported "x86_64-ucrt-posix-seh," as the compiler version.
# The first dotted number is right for all three.
_m = re.search(r"\d+\.\d+(?:\.\d+)?", cc_version)
cc_version_short = _m.group(0) if _m else ""
style = os.environ.get("STYLE", "posix")

def conv(p):
    if style != "windows":
        return p
    for tool in ("wslpath", "cygpath"):
        try:
            return subprocess.run([tool, "-w", p], capture_output=True, text=True,
                                  check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            continue
    raise SystemExit("benchmark-speedtest1: --path-style windows needs wslpath or "
                     "cygpath; neither ran. Refusing to guess the mapping.")

with open(manifest_p, encoding="utf-8") as fh:
    m = json.load(fh)

# ★★ THE MANIFEST IS TRANSLATED TOO, AND IT HAS TO BE — IT IS WHAT DSS READS.
# `--project` hands the compiler the MANIFEST, not the plan, so converting only
# the plan's copy of the paths leaves the DSS arm reading POSIX paths on a
# Windows host. ✔MEASURED 2026-08-21: gcc and cl both built and ran while DSS
# refused with `error[D_FileNotFound] cannot open /mnt/c/.../ctime.c` — the two
# reference arms passing is exactly what makes this kind of miss survivable long
# enough to be confusing.
# ⚠ TRANSLATED **AFTER** GENERATION, NEVER BEFORE. gen-pe64-manifest.py asserts
# that every source exists on disk, and that assertion is worth keeping: it is
# run in the shell that can actually see the files. Converting first would turn a
# real existence check into one that cannot resolve anything it is handed.
#
# ⚠⚠ CONVERTED **ONCE**, INTO NAMED LISTS THAT BOTH CONSUMERS THEN SHARE.
# `conv()` is NOT idempotent and fails SILENTLY when applied twice: `wslpath -w`
# reads its argument as a POSIX path, so handing it `C:\Source\x\y.c` returns
# `CSourcexy.c` — every separator gone, no error, no non-zero exit.
# ✔MEASURED 2026-08-21: converting for the manifest and then converting the
# manifest's own values again for the plan produced exactly that, and it was
# caught only because R1 checks the TUs exist. A path mangler that succeeds is
# the shape this whole file's refusals exist for.
sources = [conv(s) for s in m["sources"]]
includes = [conv(i) for i in m["includes"]]
if style == "windows":
    m["sources"], m["includes"] = sources, includes
    with open(manifest_p, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(m, fh, indent=2)
        fh.write("\n")

plan = {
  "subject": {
    "tus":      sources,
    "includes": includes,
    "defines":  list(m["defines"]),
    "sqliteSrc": conv(sqlite_dir),
    "upstreamCommit": sqlite_head,
  },
  "compilers": [
    {"id": "dss", "kind": "dss", "label": "DSS Code Prime",
     "bin": conv(dss), "manifest": conv(manifest_p), "config": "release",
     "configRoot": conv(os.environ["CFG_ROOT"]),
     # The TARGET SPEC is on the arm because the artifact path is read back out
     # of the build's own `dss-code-prime: artifact <spec> <path>` line, and that
     # line is keyed on the spec.
     "target": target,
     "artifactName": "speedtest1", "optimizationLabel": "--config=release"},
    {"id": "cc", "kind": "unix-cc",
     "label": os.path.splitext(os.path.basename(cc))[0],
     "version": cc_version_short,
     # ⚠ conv(), LIKE EVERY OTHER PATH. ✔MEASURED 2026-08-21: without it a
     # Windows plan derived inside WSL carried `bin: "/usr/bin/gcc"`, and the
     # native measurement could not launch it. The reference compiler is a PATH
     # like the sources and the manifest, and it crosses the same boundary.
     "bin": conv(cc), "optFlags": ["-O2"],
     "linkFlags": json.loads(os.environ["REF_LINK_FLAGS"]),
     "optimizationLabel": "-O2"},
    {"id": "msvc", "kind": "msvc", "label": "MSVC cl.exe",
     "bin": "cl.exe", "optFlags": ["/O2"], "optimizationLabel": "/O2"},
  ],
  "workload": {"size": int(size), "testset": (testset or None), "verify": True},
  "repeats":  {"build": int(brep), "run": int(rrep)},
  "jobsArms": [int(j) for j in jobs.split()],
  "outDir":   conv(outdir),
  "target":   target,
}
with open(plan_p, "w", encoding="utf-8", newline="\n") as fh:
    json.dump(plan, fh, indent=2); fh.write("\n")
print("   plan      : %s  (%d TUs, %d defines, %d include dirs)"
      % (plan_p, len(plan["subject"]["tus"]), len(plan["subject"]["defines"]),
         len(plan["subject"]["includes"])))
PY

if [[ $DERIVE_ONLY == 1 ]]; then
  pass "derivation complete — the plan is at $PLAN_OUT"
  info "(--derive-only: the measurement is the caller's, which is how the .ps1 twin"
  info " reaches this same derivation without a second implementation of it)"
  exit 0
fi

step "Measure"
python3 "$BENCH_CORE" --plan "$PLAN_OUT" \
  --json-out "$OUT_DIR/benchmark-speedtest1.json" \
  --md-out   "$OUT_DIR/benchmark-speedtest1.md"

# ─────────────────────────────────────────────────────────────────────────────
# TWIN PARITY (benchmark-speedtest1.ps1) — a review obligation, not a gate.
# Same inputs (every flag above, same spellings), same properties (the same
# derivation through this very file, the same measurement through
# speedtest1_bench.py), same exit codes (0/1/2/3 as documented at the top).
# The ONE asymmetry is deliberate and is the same one build-and-test.ps1 has
# carried since it was written: the recipe derivation runs in a POSIX shell,
# because SQLite's build is autosetup + make + tclsh. The .ps1 therefore calls
# THIS script with `--derive-only --path-style windows` and then measures
# natively, so there is no second derivation to keep in step — only a caller.
# ─────────────────────────────────────────────────────────────────────────────
