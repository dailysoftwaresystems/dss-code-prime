#!/usr/bin/env bash
# Local incremental build + test harness for dss-code-prime.
#
# Usage:
#   scripts/build/local-build.sh              # build only
#   scripts/build/local-build.sh --test       # build then run ctest
#   scripts/build/local-build.sh --configure  # cmake configure + build
#   scripts/build/local-build.sh --clean      # wipe build dir + reconfigure + build
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
            sed -n '2,12p' "$0"; exit 0 ;;
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
    (cd build && ctest --output-on-failure)
fi
