#!/usr/bin/env bash
# check-anchor-registry.sh — CI guard for the deferred-anchor registry
# discipline. Per memory + the cross-plan staleness sweep, this leak
# recurred TWICE before being system-enforced.
#
# Contract: every `D-*` identifier cited in `src/` MUST resolve to a row in
# `.plans/_deferred-anchor-registry.md` OR a citation in any `.plans/*.md`
# file. The script greps source/, extracts each unique `D-*` anchor name,
# and fails-loud listing every anchor that has no plan-side counterpart.
#
# Allowlist: anchor-shaped strings that are NOT deferred-work markers
# (in-code constants, diagnostic-message identifiers) live in
# `.plans/_deferred-anchor-registry.md` under the "Allowlist" section. The
# script reads them from the table rows starting with `| `.
#
# Cross-platform: this is the bash variant for Linux/macOS CI; the
# companion `check-anchor-registry.ps1` is wired into Windows CI.
set -euo pipefail

# Locate repo root (this script lives at tools/).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

# Anchor regex: D- followed by 3+ uppercase-or-digit segments separated by `-`.
# Same pattern the developer-side audit grep uses (see the cross-plan
# staleness sweep commit message). Two-segment names like `D-OPT` are
# treated as informal; the registry contract enforces ≥3 segments.
ANCHOR_REGEX='\<D-[A-Z0-9_]+(-[A-Z0-9_]+){2,}'

# ── real-examples/ ADDED 2026-08-03 (TF-C111), D-HARNESS-ANCHOR-GUARD-SKIPS-HARNESS-DRIVERS.
# The guard covered `src/ examples/` only, so every `D-*` cited in a HARNESS
# DRIVER resolved to nothing and failed nothing. Measured instance: the name
# `D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE` was carried in two hand-off
# documents AS THOUGH TRACKED, with ZERO hits repo-wide and no registry row.
# An unenforced citation is a citation that rots.
# The drivers are SCRIPTS and need their own --include set; reusing the source
# filters would scan real-examples/ for *.cpp, find nothing, and be the silent
# no-op version of this fix. Kept as a SEPARATE grep rather than widening the
# first one, so the two roots cannot cross-contaminate filters.
# ⚠ Measured before landing: 24 anchors cited across the drivers (the TF-C111 note said
# 22 — that dry-run omitted the *.py filter the same commit shipped; corrected TF-C112), all
# resolving. A future root that reds is closed by REGISTERING the rows, never by
# narrowing the guard.
# ★ The .ps1 sibling MUST scan the identical set (D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED
# records that the pairing is unenforced — pairing by EXISTENCE is not pairing by
# BEHAVIOUR), so this change is mirrored there in the same commit, in BOTH the
# collection above and the FAIL-path "cited in:" locator.
# ★★ THE ROOTS ARE ASSERTED TO EXIST BEFORE THEY ARE SCANNED — this guard used to
# FAIL OPEN, which is the worst possible defect in a guard (2026-08-03, TF-C112,
# D-GATE-ANCHOR-GUARD-FAILS-OPEN-ON-MISSING-ROOT; found by an independent review of
# the very commit that widened the roots).
# The old form was `{ grep …; grep …; } | sort -u || true`, and under `set -euo
# pipefail` that `|| true` swallowed BOTH greps' exit status along with their stderr
# (`2>/dev/null`). Consequences, both measured by inspection:
#   · rename or move ONE root and it drops out of the scan SILENTLY — the reported
#     count falls and the guard still says OK, so the coverage this cycle just added
#     could be un-added by an unrelated refactor with nothing to notice;
#   · if EVERY root failed, `SRC_ANCHORS` is the empty string, the `while` loop's one
#     blank iteration is skipped by the `[[ -z ]]` guard at the top, `MISSING` stays
#     empty, and the script prints `OK (1 src anchors all resolve to plans)` — the
#     "1" being `echo "" | wc -l` — and exits 0. A guard reporting SUCCESS while
#     checking NOTHING is precisely the silent-failure class this whole registry
#     exists to prevent, and the file's own comment above already named "the silent
#     no-op version of this fix" as the hazard while shipping exactly that.
# ── PER-ROOT floors. ★★ A GLOBAL FLOOR WAS THE FIRST ATTEMPT AND IT WAS USELESS —
# corrected 2026-08-03 (TF-C112) after an independent audit MEASURED it against the
# live tree. The global form (`total < 100`) caught NO single-root collapse:
#     real-examples/ silently empty (-24)  -> 785 remaining -> PASSED
#     examples/      silently empty (-360) -> 434 remaining -> PASSED
#     src/           silently empty (-717) ->  85 remaining -> ... and even that only
#                                             fails by luck of the current ratio.
# ⇒ losing an ENTIRE root — including `src/`, the root this contract is ABOUT — sailed
# past the global floor. The red-on-disable "proof" that shipped with it used three
# roots ALL empty, i.e. it was matched to the one case a global floor does cover, and
# the claim was then generalised to the case it does not. **A control matched to the
# easy case is not evidence for the hard one.** The fix is a floor PER ROOT: each root
# independently asserts it still contributes, so any single collapse fails loud.
# Floors are set far below live counts (src 717 / examples 360 / real-examples 24 at
# time of writing) so ordinary churn never trips them — these catch collapse, not drift.
# ⚠ `real-examples` is legitimately small, so its floor is small; that is not slack, it
# is the honest number for that root, and it is exactly why a single global threshold
# could never serve all three.
#
# `grep` exits 1 on "no match", so its STATUS cannot distinguish "clean root" from
# "collapsed scan" — which is why the `|| true` exists and why deleting it is not the
# fix. A per-root COUNT is the only signal that separates them.
#
# ★ The root list, its filters and its floor live in ONE table, so adding a 4th root
# cannot silently skip its existence check or its floor — the previous form spelled the
# root list twice (once in the existence loop, once in the greps), which is the same
# duplicated-site shape this cycle has been closing everywhere else.
_scan_failed=0
_anchor_tmp="$(mktemp)"
trap 'rm -f "${_anchor_tmp}"' EXIT
# root|floor|include-globs (space separated)
for _spec in \
    'src|400|*.cpp *.hpp *.json *.c' \
    'examples|150|*.cpp *.hpp *.json *.c' \
    'real-examples|10|*.sh *.ps1 *.py'
do
    _root="${_spec%%|*}"; _rest="${_spec#*|}"
    _floor="${_rest%%|*}"; _globs="${_rest#*|}"
    if [[ ! -d "${_root}" ]]; then
        echo "anchor-registry: FAIL — scan root '${_root}' does not exist. A missing root would silently shrink coverage; refusing to report a partial scan as a pass." >&2
        _scan_failed=1; continue
    fi
    _inc=(); for _g in ${_globs}; do _inc+=(--include="${_g}"); done
    _hits="$(grep -rEoh "${ANCHOR_REGEX}" "${_root}/" "${_inc[@]}" 2>/dev/null | sort -u || true)"
    _n="$(printf '%s' "${_hits}" | grep -c . || true)"
    if [[ "${_n}" -lt "${_floor}" ]]; then
        echo "anchor-registry: FAIL — root '${_root}' yielded only ${_n} anchors, below its floor of ${_floor}." >&2
        echo "  This does NOT mean that root is clean — it means ITS scan collapsed (unreadable files, a drifted --include filter, or a moved subtree)." >&2
        echo "  Refusing to report a pass. Fix the scan; do not lower the floor." >&2
        _scan_failed=1; continue
    fi
    printf '%s\n' "${_hits}" >> "${_anchor_tmp}"
done
[[ "${_scan_failed}" -eq 0 ]] || exit 2

SRC_ANCHORS="$(sort -u "${_anchor_tmp}" | grep -c . >/dev/null && sort -u "${_anchor_tmp}" || true)"
_anchor_count="$(printf '%s' "${SRC_ANCHORS}" | grep -c . || true)"

# For each src anchor, check substring presence in any .plans/*.md.
# Substring (vs equality) handles two false-positive modes:
#   (1) Multi-line citation in src: a comment wraps the anchor name
#       across a newline — the regex captures only the prefix.
#   (2) Plans use a more specific anchor name (e.g.
#       `D-LK6-14-INTEGRATION-GOT-SLOTS`) but src cites the parent
#       (`D-LK6-14-INTEGRATION`) — both are "known" via the same row.
MISSING=()
while IFS= read -r src_a; do
    [[ -z "${src_a}" ]] && continue
    if ! grep -qrF -- "${src_a}" .plans/ 2>/dev/null; then
        MISSING+=("${src_a}")
    fi
done <<< "${SRC_ANCHORS}"

if [[ ${#MISSING[@]} -eq 0 ]]; then
    # ${_anchor_count}, not `echo "${SRC_ANCHORS}" | wc -l` — the latter reports 1 for
    # an EMPTY set (echo emits a lone newline), which is exactly how the fail-open bug
    # above dressed a scan of nothing as "OK (1 src anchors all resolve)".
    echo "anchor-registry: OK (${_anchor_count} src anchors all resolve to plans)"
    exit 0
fi

echo "anchor-registry: FAIL — the following anchors are cited in src/ but"
echo "have no matching row/citation in any .plans/*.md file:"
echo ""
for anchor in "${MISSING[@]}"; do
    echo "  ${anchor}"
    # Same scanned set as the collection above — an anchor this guard FOUND must
    # also be LOCATABLE here, or the FAIL output names a name with no "cited in:"
    # line and the fix becomes a guessing game.
    { grep -rln "${anchor}" src/ examples/ \
        --include='*.cpp' --include='*.hpp' --include='*.json' --include='*.c' 2>/dev/null; \
      grep -rln "${anchor}" real-examples/ \
        --include='*.sh' --include='*.ps1' --include='*.py' 2>/dev/null; } \
        | sed 's/^/    cited in: /'
done
echo ""
echo "Fix: either"
echo "  (a) add a row in .plans/_deferred-anchor-registry.md naming the"
echo "      trigger + closing work, OR"
echo "  (b) cite the anchor in a per-plan §3.1 row (preferred when the"
echo "      anchor maps to a specific plan's feature area), OR"
echo "  (c) if the string is a code-internal pin not deferred work, add it"
echo "      to the Allowlist section of the registry."
echo ""
echo "Discipline: this leak recurred TWICE before this guard landed."
echo "See .plans/_deferred-anchor-registry.md for the discipline rationale."
exit 1
