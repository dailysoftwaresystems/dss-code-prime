#!/usr/bin/env bash
# PURPOSE: build dss-code-prime incrementally on this host, and optionally run ctest.
# Local incremental build + test harness for dss-code-prime.
#
# Usage:
#   scripts/local-build/local-build.sh                    # build build/dbg
#   scripts/local-build/local-build.sh --test             # build then run ctest
#   scripts/local-build/local-build.sh --configure        # cmake configure + build
#   scripts/local-build/local-build.sh --clean            # wipe THIS TREE + reconfigure + build
#   scripts/local-build/local-build.sh --tree rel         # operate on build/rel instead
#   scripts/local-build/local-build.sh --tree lane1 --build-type Debug
#
# ★★★ `build/` IS A CONTAINER, NOT A BUILD TREE (the one-root layout, operator
# 2026-08-17: one root `build/`, one SUBDIRECTORY per build). This script used to
# treat `build/` itself as the tree -- `cmake -S . -B build`, `cmake --build build`,
# `cd build && ctest` -- and `--clean` was `rm -rf build`, which under that layout
# DELETES EVERY SIBLING TREE AT ONCE: `build/dbg`, `build/rel`, `build/perf` and the
# gate logs beside them. It would also have configured a FOURTH, unnamed tree at
# `build/` alongside the real ones, and then run ctest in the wrong directory.
# ⚠ Losing `build/rel` is not merely slow to recover: the Windows driver picks the
# NEWEST RELEASE build tree, so a deleted-then-stale `build/rel` silently answers
# discovery afterwards. Fixed 2026-08-20. An anchor name must never be wrapped --
# a split name is not greppable, so no tool can find it and the balance guard
# cannot see it. It goes on one line of its own:
#   D-SCRIPT-LOCAL-BUILD-TREATS-THE-BUILD-ROOT-AS-A-BUILD-TREE
#
# Designed to be safe to invoke without approval prompts in agentic workflows --
# read-only on src/, writes only inside build/<tree>.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

run_test=0
configure=0
clean=0
tree=dbg
build_type=""

# ★ The two tree names this repository has an established meaning for. A tree name
# NOT in this table is allowed (lane trees are ordinary), but configuring one for
# the FIRST time then requires an explicit --build-type: silently configuring a
# Debug tree that someone named `rel` is the kind of quiet wrong answer this file
# already shipped once.
default_build_type_for() {
    case "$1" in
        dbg) echo Debug ;;
        rel) echo Release ;;
        *)   echo "" ;;
    esac
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --test)      run_test=1 ;;
        --configure) configure=1 ;;
        --clean)     clean=1; configure=1 ;;
        --tree)
            [[ $# -ge 2 ]] || { echo "--tree needs a name" >&2; exit 2; }
            tree="$2"; shift ;;
        --build-type)
            [[ $# -ge 2 ]] || { echo "--build-type needs a value" >&2; exit 2; }
            build_type="$2"; shift ;;
        -h|--help)
            # ★★ THE HELP IS THE HEADER BLOCK ITSELF, EXTRACTED BY SHAPE. It used to be
            # a hard line range, and on 2026-08-19 a `# PURPOSE:` line inserted at the top
            # shifted every line under it -- the window then ran past the header into the
            # argument parser, and re-tuning the number would only have rearmed the same
            # trap for the next edit. A range coupled to line numbers is a claim about
            # the file that nothing rechecks.
            awk 'NR==1 && /^#!/ {next} /^# PURPOSE:/ {next} /^#/ || /^[[:space:]]*$/ {print; next} {exit}' "$0"; exit 0 ;;
        *) echo "unknown flag: $1" >&2; exit 2 ;;
    esac
    shift
done

# ── The tree name must resolve to a DIRECTLY-NESTED child of build/ ──────────
# Everything destructive below is keyed on this, so it is validated by SHAPE
# rather than by trusting the caller: no separators, no `..`, non-empty. A name
# like `../src` or `dbg/../..` would otherwise walk the delete out of the
# container, which is the precise failure this rewrite exists to prevent.
case "$tree" in
    ""|*/*|*\\*|.|..) echo "refusing tree name '$tree': it must be a single directory name directly under build/" >&2; exit 3 ;;
esac
BUILD_DIR="build/$tree"

if [[ "$clean" == 1 ]]; then
    # Belt and braces: the shape check above already guarantees this, and the
    # guarantee is cheap to restate at the one call site that cannot be undone.
    if [[ "$BUILD_DIR" != build/* || "$BUILD_DIR" == "build/" ]]; then
        echo "refusing to remove '$BUILD_DIR': --clean only ever removes ONE tree under build/" >&2
        exit 3
    fi
    rm -rf "$BUILD_DIR"
fi

# ── --build-type is meaningful ONLY when this run configures a tree from scratch ──
# ⚠ THIS CHECK USED TO LIVE INSIDE THE CONFIGURE BLOCK, AND THAT MADE IT A NO-OP on
# the commonest path: with `build.ninja` already present and no --configure, the
# whole block was skipped, so `--build-type Release` on the debug tree exited 0
# having silently ignored the flag. ✔MEASURED 2026-08-20 by EXERCISING the arm
# rather than reading it. It runs after --clean on purpose: a cleaned tree has no
# cache, so the flag IS meaningful there.
if [[ -n "$build_type" && -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "refusing --build-type on the EXISTING tree 'build/$tree': its cache already declares one." >&2
    echo "  Re-run with --clean to reconfigure it from scratch." >&2
    exit 3
fi

if [[ "$configure" == 1 || ! -f "$BUILD_DIR/build.ninja" ]]; then
    args=(-S . -B "$BUILD_DIR" -G Ninja)
    if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
        # First configure of this tree: the build type must be KNOWN, not guessed.
        # An existing tree keeps whatever its cache already says, so no type is
        # passed and a caller cannot silently flip a configured tree underneath
        # the artifacts already in it.
        [[ -n "$build_type" ]] || build_type="$(default_build_type_for "$tree")"
        if [[ -z "$build_type" ]]; then
            echo "refusing to configure a NEW tree 'build/$tree' with no build type: pass --build-type <Debug|Release|...>" >&2
            echo "  (only 'dbg' -> Debug and 'rel' -> Release are established names in this repository)" >&2
            exit 3
        fi
        args+=("-DCMAKE_BUILD_TYPE=$build_type")
    fi
    cmake "${args[@]}"
fi

cmake --build "$BUILD_DIR"

if [[ "$run_test" == 1 ]]; then
    # See scripts/run-gate/run-gate.sh for the measurement behind the 8.
    # Set CTEST_PARALLEL_LEVEL in the environment, or pass -j, to override.
    : "${CTEST_PARALLEL_LEVEL:=8}"
    export CTEST_PARALLEL_LEVEL
    (cd "$BUILD_DIR" && ctest --output-on-failure)
fi
