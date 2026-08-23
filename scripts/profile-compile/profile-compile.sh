#!/usr/bin/env bash
# PURPOSE: compile one fixed subject with a RELEASE dss-code-prime on this host and report where the time went, so the HOST is the only variable across legs.
# profile-compile.sh — compile ONE fixed subject on THIS host with a RELEASE
# dss-code-prime and report where the time went. Run it on every leg with the
# same kit and the same target, and the HOST is the only thing that moves.
#
# ★★★ ONE SCRIPT, NO .ps1 TWIN, AND THAT IS A DECISION RATHER THAN AN OMISSION.
# This tool exists because a measurement was taken with an uncontrolled variable;
# a second implementation of it would be a second contract, and the defect that
# produced this tool's own reason for existing was exactly that:
# real-examples/c/sqlite/build-and-test.{sh,ps1} implemented "which compiler do we
# time" differently, one always Release and one always newest-wins, and the
# difference between -O0 and -O3 was published as a property of the Windows HOST
# (D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX, ~8x; ~2.1x once controlled).
# A profiler whose whole value is "the only variable is the host" must not have a
# per-host implementation. The Windows leg therefore runs this same file under Git
# Bash — which this repo already depends on for build-and-test.sh — and the paths
# it hands to native tools are normalised through `cygpath` where one exists.
#
# ── HOW TO RUN THE FULL FOUR-LEG PROFILE FROM A COLD START ───────────────────
#  0. Once, on a host that has a STAGED subject (the Windows box, after a
#     real-examples/c/sqlite run has staged sqlite and emitted a manifest):
#       python scripts/profile-compile/profile-compile-support.py kit \
#           --manifest build/real-examples/c/sqlite/windows/sqlite3.elf64-x86_64.dss-project.json \
#           --root stage=build/real-examples/c/sqlite/windows/stage \
#           --root libs="$USERPROFILE/.cache/dss-code-prime/harness-libs" \
#           --out build/perf/kit
#     The kit is COPIED, never re-staged: the sqlite harness pulls upstream on
#     every run, so a host that stages for itself is not compiling the same
#     program as its peers.
#  1. This host (Windows, from Git Bash):
#       bash scripts/profile-compile/profile-compile.sh --kit build/perf/kit --label win-x86_64 \
#            --target x86_64:elf64-x86_64-linux-exec
#  2. The other three legs, from WSL (it owns the ssh carriage):
#       bash scripts/profile-compile/profile-compile-dispatch.sh wsl
#       bash scripts/profile-compile/profile-compile-dispatch.sh vps
#       bash scripts/profile-compile/profile-compile-dispatch.sh mac
#  3. The yardstick, on any host with gcc (the same TUs, the same manifest):
#       python3 scripts/profile-compile/profile-compile-support.py gcc-reference \
#           --manifest build/perf/wsl-x86_64/subject.dss-project.json \
#           --out build/perf/wsl-x86_64 --jobs 32
#     ✔MEASURED 2026-08-18 on WSL, 103 TUs, gcc 13 -O2: -j1 21.5 s, -j32 4.8 s,
#     against DSS's 1m40.8 s on that same host.
#
# Usage:
#   profile-compile.sh --kit <dir> --target <spec> --label <name>
#                      [--repo <dir>] [--build-dir <dir>] [--no-build]
#                      [--out <dir>] [--jobs N] [--gcc-reference]
set -uo pipefail

KIT=""; TARGET=""; LABEL=""; REPO=""; BUILD_DIR=""; OUT=""; JOBS=""
NO_BUILD=0; GCC_REF=0
die()  { printf '\n[X] profile-compile: %s\n' "$*" >&2; exit 1; }
say()  { printf '\n=== [%s] %s ===\n' "${LABEL:-?}" "$*"; }
info() { printf '   %s\n' "$*"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --kit)        KIT="${2:?}"; shift 2 ;;
    --target)     TARGET="${2:?}"; shift 2 ;;
    --label)      LABEL="${2:?}"; shift 2 ;;
    --repo)       REPO="${2:?}"; shift 2 ;;
    --build-dir)  BUILD_DIR="${2:?}"; shift 2 ;;
    --out)        OUT="${2:?}"; shift 2 ;;
    --jobs)       JOBS="${2:?}"; shift 2 ;;
    --no-build)   NO_BUILD=1; shift ;;
    --gcc-reference) GCC_REF=1; shift ;;
    # ★★ THE HELP IS THE HEADER BLOCK ITSELF, EXTRACTED BY SHAPE. It used to be
    # a hard line range, and on 2026-08-19 a `# PURPOSE:` line inserted at the top
    # shifted every line under it -- the window then ran past the header into the
    # argument parser, and re-tuning the number would only have rearmed the same
    # trap for the next edit. A range coupled to line numbers is a claim about
    # the file that nothing rechecks.
    -h|--help)    awk 'NR==1 && /^#!/ {next} /^# PURPOSE:/ {next} /^#/ || /^[[:space:]]*$/ {print; next} {exit}' "$0"; exit 0 ;;
    # An unknown flag is a REFUSAL, never a shrug: silently ignoring one is how a
    # run ends up not measuring what its command line said it measured.
    *) die "unknown argument '$1'. See --help." ;;
  esac
done
[[ -n "$KIT"    ]] || die "--kit is required"
[[ -n "$TARGET" ]] || die "--target is required (e.g. x86_64:elf64-x86_64-linux-exec)"
[[ -n "$LABEL"  ]] || die "--label is required (it names this leg in the report)"

# ★ PROBE THE FILESYSTEM, NEVER `command -v`, FOR THE TOOLCHAIN PATH.
# ✔MEASURED on the Mac, twice: its login profile REPLACES $PATH outright (an emsdk
# block sets PATH=<emsdk dirs>:/usr/bin:/bin:/usr/sbin:/sbin), so a
# non-interactive ssh loses /opt/homebrew/bin entirely and `command -v cmake`
# answers "not installed" about a machine that has it. A directory either exists
# or it does not, and $PATH cannot corrupt that question.
for d in /opt/homebrew/bin /usr/local/bin; do
  [[ -d "$d" ]] && case ":$PATH:" in *":$d:"*) ;; *) PATH="$d:$PATH" ;; esac
done
export PATH

# The repo is the tree this script lives in, unless told otherwise — a profiler
# that has to be told where it is can be pointed at a different checkout than the
# compiler it just built.
if [[ -z "$REPO" ]]; then
  REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fi
[[ -d "$REPO/src/dss-config" ]] || die "$REPO/src/dss-config is missing — that is not a dss-code-prime checkout"

# ★ NATIVE-TOOL PATHS. Under MSYS/Git Bash a POSIX path handed to a native
# Windows binary (cmake, dss-code-prime) is either mangled by the argument
# translator or simply not understood. `cygpath -m` is the tree's own answer to
# "spell this path for a native tool", and it exists only where the question
# arises — everywhere else this is the identity function.
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi; }

# ★★★ NAME THE CONFIG TREE. `findShippedConfig` prefers $DSS_CONFIG_ROOT and
# otherwise WALKS UP FROM THE CWD, so a leg driven over ssh (cwd = $HOME) finds
# nothing and the compile dies, while a leg driven from Windows INTO WSL silently
# reads the WINDOWS tree across the 9p mount. That second one is not merely the
# wrong tree: it puts every config and shipped-header read on a filesystem ~10x
# slower than the one the sources are on, and the cost lands in phases nobody
# would think to suspect. ✔MEASURED, and it VOIDED the first cross-leg run of this
# cycle: WSL reported preprocess-splice 1m00.2s and [other] 51.5s against the
# Windows host's 9.8s and 5.8s — an artefact of the READER, not a property of the
# host. The variable is STATED here rather than inherited.
cd "$REPO" || die "cannot cd to $REPO"
DSS_CONFIG_ROOT="$(native "$REPO")"
export DSS_CONFIG_ROOT

OUT="${OUT:-$REPO/build/perf/$LABEL}"
BUILD_DIR="${BUILD_DIR:-$REPO/build/rel}"
JOBS="${JOBS:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )}"
mkdir -p "$OUT" || die "cannot create $OUT"
PY="$(command -v python3 || command -v python)"
[[ -n "$PY" ]] || die "no python3/python on this host"
SUPPORT="$REPO/scripts/profile-compile/profile-compile-support.py"
[[ -f "$SUPPORT" ]] || die "missing $SUPPORT"

say "host"
uname -a
printf 'cores : %s\n' "$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo '?') )"
printf 'mem   : %s\n' "$( (free -h 2>/dev/null | sed -n 2p) || \
                          (sysctl -n hw.memsize 2>/dev/null | awk '{printf "%.1f GiB\n", $1/1073741824}') || echo '?')"
printf 'cxx   : %s\n' "$( (c++ --version 2>/dev/null || g++ --version 2>/dev/null) | sed -n 1p )"
printf 'config: %s\n' "$DSS_CONFIG_ROOT"

# ── the compiler: ALWAYS Release, and the build type is READ, not assumed ────
say "the compiler (build dir: $BUILD_DIR)"
if [[ "$NO_BUILD" -eq 0 ]]; then
  cmake -S "$(native "$REPO")" -B "$(native "$BUILD_DIR")" -DCMAKE_BUILD_TYPE=Release \
      > "$OUT/cmake-configure.log" 2>&1
  rc=$?; [[ $rc -eq 0 ]] || { tail -20 "$OUT/cmake-configure.log"; die "cmake configure failed (rc=$rc)"; }
  cmake --build "$(native "$BUILD_DIR")" --config Release --target dss-code-prime -j "$JOBS" \
      > "$OUT/cmake-build.log" 2>&1
  rc=$?; [[ $rc -eq 0 ]] || { tail -30 "$OUT/cmake-build.log"; die "dss-code-prime build failed (rc=$rc)"; }
else
  info "--no-build: reusing whatever is already in $BUILD_DIR"
fi
# BOTH spellings on EVERY host: the executable suffix is a fact about the machine
# the compiler RUNS on, and probing for a name that cannot exist here costs
# nothing — whereas assuming one name is how the predecessor of this script once
# reported "no dss-code-prime" over a build that had just succeeded.
DSS="$(find "$BUILD_DIR" -type f \( -name dss-code-prime -o -name dss-code-prime.exe \) -print -quit 2>/dev/null)"
[[ -n "$DSS" ]] || die "no dss-code-prime under $BUILD_DIR (looked for both dss-code-prime and dss-code-prime.exe)"
# ★★ THE ASSERTION THAT MAKES THE NUMBER MEAN ANYTHING. --require Release exits
# non-zero and says what it read and where it read it from; there is no flag to
# proceed anyway, because a non-Release timing published beside Release ones is
# the exact defect this tool was promoted out of.
"$PY" "$SUPPORT" build-type "$DSS" --require Release || die "the compiler is not a Release build"
info "compiler : $DSS"

# ── the subject: the kit, rewritten onto this host's paths ──────────────────
say "materialize the subject"
MAN="$OUT/subject.dss-project.json"
"$PY" "$SUPPORT" manifest --kit "$(native "$KIT")" --target "$TARGET" --out "$(native "$MAN")" \
  || die "could not materialize the kit manifest on this host"

# ── the measurement ─────────────────────────────────────────────────────────
# ONE run carries both payloads: DSS_OPT_TRACE costs ~1.5% (✔MEASURED on the
# Windows host: 3m29.7s traced against 3m32.9s clean, i.e. inside the noise), so a
# second untraced run would buy nothing and cost another full compile.
# rc comes back from timed-gate, which captured it DIRECTLY from run-gate.sh,
# which captured it DIRECTLY from the compiler and refuses an exit-0 that produced
# no `compile time` report of its own.
say "compile  (target=$TARGET, --config=release)"
LOG="$OUT/compile.log"
DSS_OPT_TRACE=1 "$PY" "$SUPPORT" timed-gate --repo "$REPO" --log "$LOG" \
    --witness 'compile time' -- \
    "$DSS" --project "$(native "$MAN")" --config=release --time --output "$(native "$OUT/image")"
rc=$?

if [[ $rc -ne 0 ]]; then
  # ⚠ NO SUCCESS TOKEN ON THIS PATH, EVER. The first version of this script
  # emitted `PROFILE-LEG-OK vps-arm64 rc=1` over a compile that had died before
  # parsing a single file — a success string the script wrote about itself, which
  # is what scripts/run-gate/run-gate.sh exists to refuse. The failure token deliberately
  # does not contain the success token as a substring, so a grep for one cannot
  # match the other.
  echo "PROFILE-LEG-FAILED $LABEL rc=$rc  (log: $LOG)"
  tail -30 "$LOG"
  exit "$rc"
fi

say "phase report"
grep -E 'compile time|^dss-code-prime:   phase' "$LOG" || true
say "optimizer passes"
"$PY" "$SUPPORT" agg-trace "$LOG" | sed -n '1,40p' || true
say "artifact"
find "$OUT/image" -type f -exec ls -la {} \; 2>/dev/null | head -5

if [[ "$GCC_REF" -eq 1 ]]; then
  say "gcc yardstick (same TUs, same manifest)"
  "$PY" "$SUPPORT" gcc-reference --manifest "$MAN" --out "$OUT" --jobs "$JOBS" \
    || die "the gcc reference failed"
fi

echo
echo "PROFILE-LEG-OK $LABEL   (out: $OUT)"
