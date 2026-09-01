#!/usr/bin/env bash
# read-anchors.sh -- list every anchor as name + priority + status.
#
# A LAUNCHER, NOT AN IMPLEMENTATION -- and that is the whole design. The operator asked
# for a .sh and a .ps1 per verb; this repository's rule says a pair must not drift, while
# conceding that no gate can decide whether two arbitrary programs are equivalent. Both
# are satisfied by ONE implementation with eight entry points: the pair EXISTS on both
# hosts, takes the same flags and returns the same exit codes, and CANNOT diverge in
# behaviour because there is only one behaviour. A second hand-written markdown-table
# writer in PowerShell is exactly the class of defect anchors.py exists to prevent.
#
# Every argument is passed through untouched. See: anchors.py --help
set -eu
_dir="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
_py="${PYTHON:-}"
if [ -z "${_py}" ]; then
    if command -v python3 >/dev/null 2>&1; then _py=python3
    elif command -v python >/dev/null 2>&1; then _py=python
    else
        echo "read-anchors: no python3 or python on PATH. This is a launcher over" >&2
        echo "  ${_dir}/anchors.py -- set PYTHON=<interpreter> or install one." >&2
        exit 3
    fi
fi
exec "${_py}" "${_dir}/anchors.py" list "$@"
