#!/usr/bin/env bash
# The self-test for `check-root-litter.sh` -- it proves that guard can actually
# FAIL: it refuses a planted root file and names it, while staying green on a
# clean root and on a legitimately nested one.
#
# ⓘ No `PURPOSE:` line, deliberately. `scripts_index_guard` reads one declaration
# per script directory from the PRIMARY, and refuses a sibling whose own
# declaration contradicts it -- a sibling may omit it, never disagree.
#
# ★ ARM 3 IS THE CONTROL AND IT IS LOAD-BEARING. Without it, arms 1-2 passing is
# equally consistent with "this guard refuses everything" -- the vacuous-fixture
# class this project closes repeatedly. It plants a file NESTED one level down,
# where litter is legitimate, and requires the guard to stay GREEN.
set -uo pipefail
_here="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd -P)" || exit 2
_root="$(cd "$_here/../.." && pwd -P)" || exit 2
_guard="$_here/check-root-litter.sh"

fail=0
_planted=()
cleanup() { for p in "${_planted[@]:-}"; do rm -f "$p" 2>/dev/null; done; }
trap cleanup EXIT INT TERM

_arm() {  # name expected_rc must_say
    local name="$1" want="$2" says="${3:-}" out rc
    out="$(bash "$_guard" 2>&1)"; rc=$?
    if [ "$rc" -ne "$want" ]; then
        echo "  FAIL ($name) rc=$rc want=$want"; echo "$out" | sed 's/^/        /'; fail=1; return
    fi
    if [ -n "$says" ] && ! printf '%s' "$out" | grep -qF "$says"; then
        echo "  FAIL ($name) rc correct but the message never said: $says"; fail=1; return
    fi
    echo "  ok   ($name) rc=$rc${says:+ , said: $says}"
}

# ⚠ REFUSE TO RUN ON A DIRTY ROOT rather than reporting a misleading pass: arm 1
# asserts cleanliness, so pre-existing litter would make this suite lie in the
# flattering direction.
if [ -n "$(git -C "$_root" status --porcelain=v1 2>/dev/null | sed -n 's/^?? //p' | grep -vE '/')" ]; then
    echo "test-check-root-litter: CANNOT RUN -- the root already holds untracked file(s);" >&2
    echo "  arm 1 asserts a clean root, so this suite would report a false verdict." >&2
    exit 2
fi

echo "test-check-root-litter:"
_arm "1 CLEAN-ROOT is green" 0 "OK"

_planted+=("$_root/dss-litter-probe-$$.tmp")
printf 'planted by the self-test\n' > "$_root/dss-litter-probe-$$.tmp"
_arm "2 PLANTED-FILE is refused, BY NAME" 1 "dss-litter-probe-$$.tmp"
rm -f "$_root/dss-litter-probe-$$.tmp"

mkdir -p "$_root/.temp" 2>/dev/null || :
_planted+=("$_root/.temp/dss-litter-nested-$$.tmp")
printf 'nested, and legitimate\n' > "$_root/.temp/dss-litter-nested-$$.tmp"
_arm "3 CONTROL: a NESTED file is NOT litter" 0 "OK"
rm -f "$_root/.temp/dss-litter-nested-$$.tmp"

if [ "$fail" -eq 0 ]; then
    echo "test-check-root-litter: OK - 3 arms (2 gate, 1 control); this guard is PROVEN able to fail."
    exit 0
fi
echo "test-check-root-litter: FAILED" >&2
exit 1
