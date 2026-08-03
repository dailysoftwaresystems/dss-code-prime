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
# ⚠ Measured before landing: 22 anchors cited across the drivers, all 22 already
# resolving. A future root that reds is closed by REGISTERING the rows, never by
# narrowing the guard.
# ★ The .ps1 sibling MUST scan the identical set (D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED
# records that the pairing is unenforced — pairing by EXISTENCE is not pairing by
# BEHAVIOUR), so this change is mirrored there in the same commit, in BOTH the
# collection above and the FAIL-path "cited in:" locator.
SRC_ANCHORS="$( { grep -rEoh "${ANCHOR_REGEX}" src/ examples/ \
        --include='*.cpp' --include='*.hpp' --include='*.json' --include='*.c' 2>/dev/null; \
      grep -rEoh "${ANCHOR_REGEX}" real-examples/ \
        --include='*.sh' --include='*.ps1' --include='*.py' 2>/dev/null; } \
    | sort -u || true)"

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
    echo "anchor-registry: OK ($(echo "${SRC_ANCHORS}" | wc -l) src anchors all resolve to plans)"
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
