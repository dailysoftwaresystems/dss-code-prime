#!/usr/bin/env bash
# PURPOSE: run a DSS gate leg inside WSL -- sync the Windows checkout, build clean, and run ctest through run-gate.
#
# ★★★ WHY THIS IS A SCRIPT AND NOT A PIPELINE SOMEONE RETYPES. The three-leg gate
# runs a WSL x86_64 leg before every commit, and until 2026-08-19 the way to run
# it was to type an rsync, a cmake, and a ctest into a scratch file each time.
# ✔MEASURED that cycle: THREE near-identical scratch scripts were written in a
# single session (a full leg, a guards-only pass, an arm64-strict pass), each one
# re-deriving the same excludes and the same build layout. They are the same tool
# at three scopes, which is what the `--mode` flag below is.
#
# ⚠ AND EACH RETYPING RE-OPENS THE SAME EDGE CASES, all of them measured in this
# repository's own record:
#   * an UNANCHORED rsync exclude (`build*` rather than `/build*`) once silently
#     skipped `src/program/build_scripts.cpp`, and a gate leg was configured
#     against a tree missing a changed `.cpp`;
#   * `rsync -a` PRESERVES MTIMES, so a /mnt/c source whose mtime lands behind an
#     existing build output makes ninja skip the changed file
#     (D-SYNC-RSYNC-PRESERVED-MTIME-DEFEATS-THE-REBUILD). This script builds
#     CLEAN rather than hoping around that;
#   * a `wsl.exe bash -c` carrying a variable once expanded to
#     `rsync -a --delete / /`, ran for 70 minutes, and reported exit 0. Nothing
#     here interpolates into a remote `-c` string.
#
# ★ WHY THERE IS NO `.ps1` SIBLING, deliberately. This script's body runs INSIDE
# a WSL distro, where PowerShell is not the shell; a `.ps1` twin would have
# nothing to be a twin OF. The Windows-side half is a single invocation with no
# logic in it:
#
#     wsl.exe -e bash scripts/wsl-leg/wsl-leg.sh [--mode <m>] [-R <regex>]
#
# (`wsl.exe -e`, never `wsl.exe bash -c` — see the third edge case above.)
#
# MODES
#   full      (default) rsync -> CLEAN configure+build -> full ctest via run-gate
#   guards    run the repo guards over the WINDOWS checkout at /mnt/c, no sync,
#             no build. Cheap, and it is what proves a `.sh` guard's repo-root
#             derivation works under a real POSIX shell.
#   arm-strict  CLEAN configure+build with -DDSS_STRICT_ARM_VERDICTS=ON into
#             build/arm-strict, then ctest over `-R` (required in this mode:
#             the whole point is a SCOPED run, since strict verdicts emulate
#             every x86_64 example one at a time).
#
# Usage:
#   wsl.exe -e bash scripts/wsl-leg/wsl-leg.sh
#   wsl.exe -e bash scripts/wsl-leg/wsl-leg.sh --mode guards
#   wsl.exe -e bash scripts/wsl-leg/wsl-leg.sh --mode arm-strict -R 'harness/.*'
set -uo pipefail

MODE="full"
FILTER=""
SRC="${DSS_WIN_CHECKOUT:-/mnt/c/Source/DailySoftware/dss-code-prime}"
DST="${DSS_WSL_CHECKOUT:-$HOME/src/dss-code-prime}"

die() { printf '\n[X] wsl-leg: %s\n' "$*" >&2; exit 1; }
say() { printf '\n=== %s ===\n' "$*"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)   MODE="${2:?--mode needs a value}"; shift 2 ;;
        -R)       FILTER="${2:?-R needs a value}"; shift 2 ;;
        --src)    SRC="${2:?}"; shift 2 ;;
        --dst)    DST="${2:?}"; shift 2 ;;
        -h|--help)
            awk 'NR==1 && /^#!/ {next} /^# PURPOSE:/ {next} /^#/ || /^[[:space:]]*$/ {print; next} {exit}' "$0"
            exit 0 ;;
        # An unknown flag is a REFUSAL, never a shrug: silently ignoring one is
        # how a leg runs a different thing than the operator asked for and still
        # reports green.
        *) die "unknown argument '$1' (try --help)" ;;
    esac
done

[[ -d "$SRC" ]] || die "the Windows checkout is not visible at $SRC"

# The arm64 leg's examples need the sysroot prefix or ~450 of them false-red at
# exit 255. Exported for every mode: harmless where nothing reads it.
export QEMU_LD_PREFIX="${QEMU_LD_PREFIX:-/usr/aarch64-linux-gnu}"

# ── guards: no sync, no build, straight over the Windows checkout ────────────
if [[ "$MODE" == "guards" ]]; then
    cd "$SRC" || die "cannot enter $SRC"
    say "repo guards under a real POSIX shell ($(pwd))"
    rc=0
    for g in check-anchor-registry check-line-endings check-orphan-tests; do
        printf -- '--- %s\n' "$g"
        bash "scripts/$g/$g.sh" 2>&1 | tail -2
        s=${PIPESTATUS[0]}; [[ $s -eq 0 ]] || { rc=$s; echo "    RED rc=$s"; }
    done
    for g in check-scripts-index check-plan-citations check-anchor-balance; do
        printf -- '--- %s\n' "$g"
        python3 "scripts/$g/$g.py" 2>&1 | tail -2
        s=${PIPESTATUS[0]}; [[ $s -eq 0 ]] || { rc=$s; echo "    RED rc=$s"; }
    done
    [[ $rc -eq 0 ]] || die "a guard went red (rc=$rc)"
    say "guards OK"
    exit 0
fi

# ── sync ────────────────────────────────────────────────────────────────────
say "rsync $SRC -> $DST (excludes ANCHORED)"
mkdir -p "$DST" || die "cannot create $DST"
rsync -a --delete \
    --exclude='/build' --exclude='/build-*' --exclude='/target' \
    --exclude='/.dss-deps' --exclude='/scratchpad' --exclude='/Testing' \
    --exclude='/test-scratch' \
    "$SRC/" "$DST/" || die "rsync failed"
cd "$DST" || die "cannot enter $DST"
printf 'head : %s\n' "$(git log --oneline -1 2>/dev/null || echo '(no git)')"
printf 'dirty: %s path(s)\n' "$(git status --porcelain 2>/dev/null | wc -l)"

# ── build ───────────────────────────────────────────────────────────────────
# CLEAN, always. See the mtime note in the header: an incremental build over an
# rsynced tree can silently skip the very file the leg exists to exercise.
if [[ "$MODE" == "arm-strict" ]]; then
    BUILD="build/arm-strict"
    CONFIGURE_EXTRA="-DDSS_STRICT_ARM_VERDICTS=ON"
    [[ -n "$FILTER" ]] || die "--mode arm-strict requires -R <regex>: strict verdicts emulate every x86_64 example one at a time, so an unscoped run costs hours"
else
    BUILD="build/dbg"
    CONFIGURE_EXTRA=""
fi

say "clean configure + build ($BUILD${CONFIGURE_EXTRA:+, $CONFIGURE_EXTRA})"
rm -rf "$BUILD"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDSS_BUILD_TESTS=ON \
      $CONFIGURE_EXTRA > /tmp/wsl-leg-configure.log 2>&1 \
    || { tail -25 /tmp/wsl-leg-configure.log; die "configure failed"; }
cmake --build "$BUILD" > /tmp/wsl-leg-build.log 2>&1 \
    || { tail -30 /tmp/wsl-leg-build.log; die "build failed"; }
printf 'build: %s\n' "$(tail -1 /tmp/wsl-leg-build.log)"

# ── test, through run-gate so a silent no-run cannot report success ──────────
say "ctest${FILTER:+ (-R $FILTER)}"
if [[ -n "$FILTER" ]]; then
    bash scripts/run-gate/run-gate.sh /tmp/wsl-leg-ctest.log 'tests passed' \
        ctest --test-dir "$BUILD" --output-on-failure -R "$FILTER"
else
    bash scripts/run-gate/run-gate.sh /tmp/wsl-leg-ctest.log '100% tests passed' \
        ctest --test-dir "$BUILD" --output-on-failure
fi
rc=$?
grep -E "tests passed|tests failed|The following tests FAILED" -A20 /tmp/wsl-leg-ctest.log | tail -25
[[ $rc -eq 0 ]] || die "ctest leg failed (rc=$rc, log /tmp/wsl-leg-ctest.log)"
say "WSL leg OK"
