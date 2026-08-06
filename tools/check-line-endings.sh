#!/usr/bin/env bash
# check-line-endings.sh — CI guard for D-REPO-GITATTRIBUTES-PINS-EOL-FOR-CONFIGS-BUT-NOT-FOR-SOURCES.
#
# Contract: NO tracked TEXT blob in this repository may contain a line-terminating
# CR — not in HEAD, and not staged in the index. A file that genuinely needs its
# 0x0D bytes preserved declares itself `binary` in `.gitattributes`, which this
# guard honours through `git grep -I` (that is how `examples/**/*.bin` keeps its
# `#embed` resource bytes).
#
# Why a gate and not just the pin. `.gitattributes` now pins `eol=lf` for the
# source tree, but a pin is only as good as its GLOB COVERAGE: the pin that
# existed before this cycle covered configs, goldens and harness drivers and
# still let six CRLF blobs land in `src/` and `tests/` because no `.cpp` was
# ever named. The next unpinned extension repeats that exactly. This check is
# the invariant the pin is only a mechanism for — it holds over every tracked
# text blob regardless of which globs anyone remembered to write.
#
# ⚠ INSTRUMENT NOTES, all MEASURED on this workstation 2026-08-06 while the
# defect was being diagnosed — read these before "improving" any command here:
#   · `git show HEAD:<path>` APPLIES eol conversion on the way out, so it can
#     NEVER tell you what was committed. `git cat-file blob` can. This script
#     uses `git grep <rev>`, which reads the BLOB, not a smudged checkout.
#   · `grep -c $'\r'` matches the LETTER `r` in some shells — it does not count
#     CRs at all. Counting is done here with `tr -dc '\r' | wc -c`.
#   · a `grep -P` that ABORTS (no PCRE support, a hostile locale) prints nothing
#     and is trivially misread as "measured zero offenders". That is why the
#     POSITIVE CONTROL below exists: the same `-P` engine, the same ref, a
#     pattern that MUST match thousands of files. If the instrument is dead the
#     control collapses and this guard FAILS — it never reports a clean tree it
#     did not actually read.
#
# Cross-platform: this is the bash variant for Linux/macOS CI; the companion
# `check-line-endings.ps1` runs on Windows CI and MUST stay behaviourally
# identical (D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED — pairing by EXISTENCE is not
# pairing by BEHAVIOUR, so any change here is mirrored there in the same commit).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

# ── fail-closed preconditions ─────────────────────────────────────────────
# A guard that cannot run must FAIL, never skip. `git rev-parse` proves both
# that git exists and that this really is a work tree with a resolvable HEAD.
if ! command -v git >/dev/null 2>&1; then
    echo "line-endings: FAIL — git is not on PATH. This guard reads BLOBS, so it" >&2
    echo "  cannot fall back to the working tree (a CRLF checkout would false-red" >&2
    echo "  and an LF checkout would false-green). Refusing to report a pass." >&2
    exit 2
fi
if ! git rev-parse --verify --quiet HEAD >/dev/null; then
    echo "line-endings: FAIL — HEAD does not resolve; this is not a git work tree" >&2
    echo "  with a commit. Refusing to report a pass over a tree it cannot read." >&2
    exit 2
fi

# ── POSITIVE CONTROL ──────────────────────────────────────────────────────
# The offender scan below PASSES by returning nothing, which is exactly what a
# BROKEN scan also returns. So first prove the instrument answers: same `git
# grep`, same `-I -l -P`, same ref, against a pattern every non-empty text blob
# matches. MEASURED at the commit that added this: 2214 blobs in HEAD, 2214 in
# the index, over 2238 tracked paths. The floor sits far below that so ordinary
# churn never trips it — it catches COLLAPSE (a dead PCRE engine, an unreadable
# ref, a moved tree), not drift. Fix the scan; never lower the floor.
SCAN_FLOOR=1500
_control_head="$(git grep -I -l -P '^.' HEAD 2>/dev/null | wc -l | tr -d ' ')"
_control_index="$(git grep --cached -I -l -P '^.' 2>/dev/null | wc -l | tr -d ' ')"
_control_failed=0
for _pair in "HEAD:${_control_head}" "index:${_control_index}"; do
    _what="${_pair%%:*}"; _n="${_pair#*:}"
    if [[ "${_n}" -lt "${SCAN_FLOOR}" ]]; then
        echo "line-endings: FAIL — the ${_what} scan saw only ${_n} text blobs, below its floor of ${SCAN_FLOOR}." >&2
        echo "  This does NOT mean the tree is clean — it means the SCAN collapsed" >&2
        echo "  (git built without PCRE for -P, an unresolvable ref, or a moved tree)." >&2
        echo "  A guard that reports success over what it could not read is the exact" >&2
        echo "  failure class this repository keeps anchoring. Refusing to pass." >&2
        _control_failed=1
    fi
done
[[ "${_control_failed}" -eq 0 ]] || exit 2

# ── the check ─────────────────────────────────────────────────────────────
# `-I` skips blobs git detects as BINARY, which is correct (a `.bin` #embed
# fixture legitimately carries 0x0D) but leaves one blind spot: a TEXT source
# that git happens to detect as binary would be skipped silently. Check C below
# closes exactly that hole for the extensions the pin claims to cover.
#
# ★ THERE IS DELIBERATELY NO `eol=crlf` ESCAPE HATCH, and the reason is a
# MEASURED property of git rather than a policy choice. Probed here 2026-08-06:
# a file declared `text eol=crlf`, written CRLF on disk and staged, lands in the
# index as `i/lf` with ZERO CR in the blob — `eol=` governs the SMUDGE
# (checkout) direction only; the clean filter always stores LF for a `text`
# file. So "declare eol=crlf" could never make a CR-carrying blob legitimate:
# it would only ever have exempted a LEGACY blob committed before its own
# declaration existed, while reading like a sanctioned way to store CRLF. An
# escape hatch that cannot do what it advertises is worse than none.
# The REAL exemption is `binary` / `-text`, and this guard already honours it
# through `-I` — that is how `examples/**/*.bin` keeps its 0x0D bytes.
_offenders_head="$(git grep -I -l -P '\r$' HEAD 2>/dev/null | sed 's|^HEAD:||')"
_offenders_index="$(git grep --cached -I -l -P '\r$' 2>/dev/null)"

_report=""
_check_set() {
    local _what="$1"; shift
    local _list="$1"
    [[ -z "${_list}" ]] && return 0
    while IFS= read -r _f; do
        [[ -z "${_f}" ]] && continue
        _report+="  ${_what}: ${_f}"$'\n'
    done <<< "${_list}"
}
_check_set "committed (HEAD)" "${_offenders_head}"
_check_set "staged (index)"   "${_offenders_index}"

# ── Check C: the pin's own glob set must contain no binary-detected blob ──
# Without this, a source file git mis-detects as binary is skipped by `-I` above
# and this guard would report a clean tree over a file it never opened.
_binary_in_pinned="$(git ls-files --eol -- '*.c' '*.h' '*.cpp' '*.hpp' '*.cmake' 'CMakeLists.txt' '*/CMakeLists.txt' '*.md' 'VERSION' 2>/dev/null \
    | awk '$1 == "i/-text" { sub(/^[^\t]*\t/, ""); print }')"
if [[ -n "${_binary_in_pinned}" ]]; then
    while IFS= read -r _f; do
        [[ -z "${_f}" ]] && continue
        _report+="  binary-detected inside the eol=lf pin (invisible to the scan above): ${_f}"$'\n'
    done <<< "${_binary_in_pinned}"
fi

if [[ -z "${_report}" ]]; then
    echo "line-endings: OK (${_control_head} committed + ${_control_index} staged text blobs, none carries CR)"
    exit 0
fi

echo "line-endings: FAIL — tracked blobs violate the LF contract:"
echo ""
printf '%s' "${_report}"
echo ""
echo "Fix:"
echo "  (a) convert the file to LF — \`sed 's/\\r\$//' f > f.tmp && mv f.tmp f\` —"
echo "      and commit that rewrite ON ITS OWN, never beside real changes; a"
echo "      whole-file EOL diff sitting next to logic is unreviewable, which is"
echo "      how the original six got in unnoticed; OR"
echo "  (b) add an \`eol=lf\` pin for its extension in \`.gitattributes\` so a"
echo "      tool's platform default can never decide this repo's bytes again"
echo "      (\`pathlib.write_text\` on Windows is the measured culprit); OR"
echo "  (c) if the file genuinely REQUIRES its 0x0D bytes preserved, declare it"
echo "      \`binary\` in \`.gitattributes\` (the \`examples/**/*.bin\` precedent)."
echo "      Do NOT reach for \`eol=crlf\`: MEASURED — a \`text eol=crlf\` file"
echo "      still stages as an LF blob, so it cannot make this red go away."
echo ""
echo "See D-REPO-GITATTRIBUTES-PINS-EOL-FOR-CONFIGS-BUT-NOT-FOR-SOURCES in"
echo ".plans/_deferred-anchor-registry.md for why this is machine-checked."
exit 1
