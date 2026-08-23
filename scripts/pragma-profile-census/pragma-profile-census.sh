#!/usr/bin/env bash
# PURPOSE: census `#pragma` usage across the corpus and hold the profile to its expected shape.
# TF-C85 pragma census / TF-C86 (D-CSUBSET-STDARG-F001A) pairing retrofit — LAUNCHER ONLY.
#
# The census itself lives in `scripts/pragma-profile-census/pragma-profile-census.py`, once. This file finds a
# Python 3 and execs it. Its ONLY other job is to propagate the exit code
# EXACTLY — no pipe, no `tail`, nothing between the call and the `exit`, because
# a tail-swallowed status is precisely how a red run shipped as green in this
# project before.
#
# `scripts/pragma-profile-census/pragma-profile-census.ps1` is the Windows sibling and calls the SAME .py.
# Neither launcher ever invokes the other: a .ps1 that shells out to bash is
# useless on the machines it exists for, and two hand-written ports of one
# census would drift — a drifted census is worse than a missing one, because
# then the platforms disagree about the corpus and nobody knows which to trust.
#
# Usage / options / exit codes: see scripts/pragma-profile-census/pragma-profile-census.py (`--help`).

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CENSUS_PY="$HERE/pragma-profile-census.py"

if [[ ! -f "$CENSUS_PY" ]]; then
  printf 'pragma-census: FATAL: %s is missing — this launcher has no census of its own.\n' \
         "$CENSUS_PY" >&2
  exit 2
fi

# Interpreter discovery. `python3` first; plain `python` ONLY if it reports 3.x
# (on some systems `python` is still 2.7, and running this under 2.7 would fail
# in a confusing place rather than here).
PY=""
if command -v python3 > /dev/null 2>&1; then
  PY="python3"
elif command -v python > /dev/null 2>&1 \
     && python -c 'import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)' \
        > /dev/null 2>&1; then
  PY="python"
fi

if [[ -z "$PY" ]]; then
  printf 'pragma-census: FATAL: no Python 3 interpreter found.\n' >&2
  printf '  looked for: python3, then python (accepted only if it reports 3.x)\n' >&2
  printf '  Install Python 3 or put it on PATH. Refusing to skip the census\n' >&2
  printf '  silently — a census that does not run must not look like a census\n' >&2
  printf '  with nothing to report.\n' >&2
  exit 2
fi

"$PY" "$CENSUS_PY" "$@"
exit $?
