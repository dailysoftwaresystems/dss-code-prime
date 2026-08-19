#!/usr/bin/env bash
# PURPOSE: build dss-code-prime incrementally on this host, and optionally run ctest.
# Local incremental build + test harness for dss-code-prime.
#
# Usage:
#   scripts/local-build/local-build.sh              # build only
#   scripts/local-build/local-build.sh --test       # build then run ctest
#   scripts/local-build/local-build.sh --configure  # cmake configure + build
#   scripts/local-build/local-build.sh --clean      # wipe build dir + reconfigure + build
#
# Designed to be safe to invoke without approval prompts in agentic
# workflows — read-only on src/, writes only inside build/.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

run_test=0
configure=0
clean=0
for arg in "$@"; do
    case "$arg" in
        --test)      run_test=1 ;;
        --configure) configure=1 ;;
        --clean)     clean=1; configure=1 ;;
        -h|--help)
            # ★★ THE HELP IS THE HEADER BLOCK ITSELF, EXTRACTED BY SHAPE. It used to be
            # a hard line range, and on 2026-08-19 a `# PURPOSE:` line inserted at the top
            # shifted every line under it -- the window then ran past the header into the
            # argument parser, and re-tuning the number would only have rearmed the same
            # trap for the next edit. A range coupled to line numbers is a claim about
            # the file that nothing rechecks.
            awk 'NR==1 && /^#!/ {next} /^# PURPOSE:/ {next} /^#/ || /^[[:space:]]*$/ {print; next} {exit}' "$0"; exit 0 ;;
        *) echo "unknown flag: $arg" >&2; exit 2 ;;
    esac
done

if [[ "$clean" == 1 ]]; then
    rm -rf build
fi

if [[ "$configure" == 1 || ! -f build/build.ninja ]]; then
    cmake -S . -B build -G Ninja
fi

cmake --build build

if [[ "$run_test" == 1 ]]; then
    # See scripts/run-gate/run-gate.sh for the measurement behind the 8.
    # Set CTEST_PARALLEL_LEVEL in the environment, or pass -j, to override.
    : "${CTEST_PARALLEL_LEVEL:=8}"
    export CTEST_PARALLEL_LEVEL
    (cd build && ctest --output-on-failure)
fi
