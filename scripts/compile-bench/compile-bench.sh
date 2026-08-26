#!/usr/bin/env bash
# PURPOSE: time dsscp against gcc/clang/MSVC/tcc on ONE host over a subject size ladder, naming every reference it could not find.
# compile-bench.sh -- LAUNCHER ONLY. It resolves an interpreter and execs
# compile-bench.py. It holds no logic, parses no flag of its own, and makes no
# decision the Python does not.
#
# ★★★ WHY THERE IS NO .ps1 TWIN, AND WHY THIS FILE IS EMPTY OF BEHAVIOUR.
# This capability MUST reach the Windows leg -- MSVC lives there and so does the
# Windows gate -- and the repository's pairing rule says a capability that
# reaches Windows gets a `.ps1` sibling. That rule exists so a Windows caller is
# not left without the tool; it is not a demand for two programs. Two arbitrary
# programs cannot be checked for equivalence by any detector this repo could
# ship (D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED says exactly that: pairing by
# EXISTENCE is not pairing by BEHAVIOUR), so parity would fall entirely on
# review -- and this repository has already published a measurement voided by two
# siblings that disagreed about which compiler they were timing
# (D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX: ~8x, ~2.1x once the
# variable was controlled). A benchmark is the LAST place to accept that risk.
#
# So the whole tool is ONE program, in Python, which both hosts already require
# (every guard in `scripts/` is Python and ctest invokes it on every leg). A
# Windows caller runs the same file, either through Git Bash and this launcher or
# directly:
#
#     python scripts\compile-bench\compile-bench.py --dsscp build\rel\bin\dss\dsscp.exe
#
# and gets the same program, not a second implementation of it. What this
# launcher adds over that line is exactly one fact: WHICH interpreter. macOS and
# the arm64 VPS have `python3` and may not have `python`; this Windows host has
# `python` and may not have `python3`. That is the only host-dependent decision
# in the tool, and it lives here.
#
# Every argument is passed through untouched -- see `compile-bench.py --help`.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE="${HERE}/compile-bench.py"

if [ ! -f "${CORE}" ]; then
  printf '[X] compile-bench: %s is missing -- the launcher has nothing to launch.\n' \
         "${CORE}" >&2
  exit 2
fi

# `command -v` is used here and NOT for a toolchain directory. The distinction
# matters: this repo has measured `command -v` lying over ssh about /opt/homebrew
# (a login profile that REPLACES $PATH), which is why toolchain PATHS are probed
# on the filesystem. An INTERPRETER is the opposite case -- what must be found is
# whatever this shell would itself run -- so PATH is the right oracle, and the
# fallback below covers the Homebrew case for the one name that matters.
PY=""
for cand in python3 python; do
  if command -v "${cand}" >/dev/null 2>&1; then
    PY="${cand}"
    break
  fi
done
if [ -z "${PY}" ]; then
  for cand in /opt/homebrew/bin/python3 /usr/local/bin/python3 /usr/bin/python3; do
    if [ -x "${cand}" ]; then
      PY="${cand}"
      break
    fi
  done
fi
if [ -z "${PY}" ]; then
  printf '[X] compile-bench: no python3/python on this host. Install one, or run\n' >&2
  printf '    %s directly with an interpreter you have.\n' "${CORE}" >&2
  exit 2
fi

exec "${PY}" "${CORE}" "$@"
