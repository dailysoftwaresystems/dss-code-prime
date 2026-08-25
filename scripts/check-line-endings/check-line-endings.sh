#!/usr/bin/env bash
# PURPOSE: refuse a tracked text blob that carries a CR.
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
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
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

# ── STALE-CHECKOUT DETECTION — the git history here may not describe this tree ──
#
# ★ MEASURED 2026-08-06 (TF-C123), and it cost a false red on a green gate: the
#   WSL leg of the 3-leg gate rsyncs the Windows tree with `--exclude '.git/'`
#   (D-GATE-WSL-SYNC-LEAVES-GIT-HEAD-STALE). The CONTENT is current; the `.git`
#   directory is whatever that checkout last fetched. So checks A and C — which
#   read HEAD and the index — answer about a DIFFERENT COMMIT than the files on
#   disk, and reported the ten pre-normalisation CRLF blobs as live violations
#   while every one of them was LF on disk.
#
# ⇒ A guard that cannot tell whose history it is reading must SAY SO, not
#   convict. The detection is exact rather than heuristic: if a file that HEAD
#   or the index calls CRLF carries NO CR on disk, then the recorded history is
#   not the history of this working tree. A genuine violation has CR in BOTH.
#
# ⚠ Check D (worktree rewritten under a pin) is UNAFFECTED — it reads the disk —
#   so it keeps running. Only the history-scoped checks are suspended, and the
#   run is still a FAILURE if check D finds anything.
_stale_evidence=""
_all_history_offenders="$(printf '%s\n%s\n' "${_offenders_head}" "${_offenders_index}" | sort -u)"
while IFS= read -r _f; do
    [[ -z "${_f}" ]] && continue
    [[ -f "${_f}" ]] || continue
    if [[ "$(tr -dc '\r' < "${_f}" | wc -c | tr -d ' ')" -eq 0 ]]; then
        _stale_evidence="${_f}"
        break
    fi
done <<< "${_all_history_offenders}"

if [[ -n "${_stale_evidence}" ]]; then
    echo "line-endings: HISTORY SCAN SKIPPED — this work tree's .git does not describe its files." >&2
    echo "    evidence: '${_stale_evidence}' is recorded as CRLF in HEAD/index but carries ZERO CR on disk." >&2
    echo "    HEAD here is $(git rev-parse --short HEAD 2>/dev/null || echo '<unresolved>')." >&2
    echo "    This is the expected shape of a tree synced WITHOUT .git (see the WSL leg of the" >&2
    echo "    3-leg gate, D-GATE-WSL-SYNC-LEAVES-GIT-HEAD-STALE). Convicting on that history would" >&2
    echo "    report violations belonging to another commit — so checks A and C are suspended here." >&2
    echo "    Run this guard on the AUTHORITATIVE checkout for history hygiene." >&2
    _offenders_head=""
    _offenders_index=""
    _skip_binary_check=1
fi

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

# ── ONE `git ls-files --eol` read, shared by checks C/D/E, WITH A FLOOR ───────
# Checks D and E both answer from this table, and a table that comes back EMPTY
# would make both of them report nothing — the "guard that passes over what it
# never read" shape this file already fails closed against on the history side.
# So the row count carries the same floor as the positive control above.
# ✔MEASURED 2026-08-10: 2,265 tracked paths.
_eol_rows="$(git ls-files --eol 2>/dev/null)"
_eol_row_count="$(printf '%s' "${_eol_rows}" | grep -c . || true)"
if [[ "${_eol_row_count}" -lt "${SCAN_FLOOR}" ]]; then
    echo "line-endings: FAIL — \`git ls-files --eol\` returned only ${_eol_row_count} rows, below its floor of ${SCAN_FLOOR}." >&2
    echo "  Checks D and E answer from that table, so an empty one makes BOTH report a clean" >&2
    echo "  worktree over files they never looked at. Refusing to pass; fix the scan." >&2
    exit 2
fi

# ── Check C: the pin's own glob set must contain no binary-detected blob ──
# Without this, a source file git mis-detects as binary is skipped by `-I` above
# and this guard would report a clean tree over a file it never opened.
# ⚠ Suspended alongside checks A/C when the history does not describe this tree:
# `i/-text` is an INDEX fact, so a stale index answers about a different commit.
_binary_in_pinned=""
[[ -n "${_skip_binary_check:-}" ]] || \
_binary_in_pinned="$(git ls-files --eol -- '*.c' '*.h' '*.cpp' '*.hpp' '*.cmake' 'CMakeLists.txt' '*/CMakeLists.txt' '*.md' 'VERSION' 2>/dev/null \
    | awk '$1 == "i/-text" { sub(/^[^\t]*\t/, ""); print }')"
if [[ -n "${_binary_in_pinned}" ]]; then
    while IFS= read -r _f; do
        [[ -z "${_f}" ]] && continue
        _report+="  staged (index): binary-detected inside the eol=lf pin, so the blob scan above never opened it: ${_f}"$'\n'
    done <<< "${_binary_in_pinned}"
fi

# ── Check D: a PINNED file's WORKING TREE must not have been rewritten ────
# ★ THE PIN'S OWN BLIND SPOT, and the sharpest lesson in the anchor this guard
# closes. `.gitattributes` normalises on `git add`, so once a file is pinned
# `eol=lf`, a tool that rewrites it CRLF ON DISK changes every byte of the file
# while changing NOTHING git will ever show you as a change. That is not
# hypothetical: it is what happened to the `src/dss-config/**` files when an
# agent edited 159 fixtures through Python `pathlib.write_text`.
# ⚠ MEASURED EXACTLY, because the imprecise version of this claim is easy to
# repeat and wrong: rewrite a pinned `.cpp` to CRLF on disk and
#   · `git status` DOES report ` M <path>` (twice running — it does not settle);
#   · `git diff` and `git diff --stat` are EMPTY;
#   · `git add` + `git diff --cached` are EMPTY too, so the rewrite can never
#     be committed and simply LIVES in the working tree indefinitely.
# So the file is flagged as changed and then no view will tell you WHAT changed
# — an author who checks `git diff` on that `M` concludes it is noise. Checks
# A–C cannot see it BY CONSTRUCTION: the blob is fine; the working tree is not.
#
# `git ls-files --eol` reports BOTH sides — `i/` (index) and `w/` (worktree). A
# file DECLARED `eol=lf` whose worktree form is `crlf` or `mixed` was rewritten
# by something AFTER checkout, on any host and regardless of `core.autocrlf`,
# because with `eol=lf` declared a checkout always produces LF.
#
# ⚠ Deliberately a CLOSED set (`w/crlf`, `w/mixed`) rather than "anything that
# is not `w/lf`": an unmeasured `w/` state — a sparse or partial checkout, a
# file not materialised — must not be guessed at and turned into a red.
# MEASURED when this landed: 2166 tracked files declare `eol=lf`, and ZERO have
# a non-LF worktree form.
_worktree_rewritten="$(printf '%s\n' "${_eol_rows}" | awk -F'\t' '
    NF >= 2 && index($1, "eol=lf") > 0 {
        split($1, f, /[ \t]+/)
        if (f[2] == "w/crlf" || f[2] == "w/mixed") print $2
    }')"
if [[ -n "${_worktree_rewritten}" ]]; then
    while IFS= read -r _f; do
        [[ -z "${_f}" ]] && continue
        _report+="  working (tracked, eol=lf pinned): rewritten to CRLF on disk — git diff shows NOTHING to review: ${_f}"$'\n'
    done <<< "${_worktree_rewritten}"
fi

# ── Check E: THE UNSTAGED WORKING TREE — the tier the other checks cannot see ──
# ★★ THE BLIND SPOT, ✔MEASURED 2026-08-10 on this tree. Checks A and B read
# BLOBS (HEAD and the index); check C is an index fact; check D reads the disk but
# ONLY for files that DECLARE `eol=lf`. So a CRLF introduced into a working file
# that is neither staged nor covered by the pin was invisible to every tier — and
# "before commit" is exactly when a line-ending mistake is cheap to fix and the
# only moment this guard can prevent rather than diagnose.
# The gap is not theoretical arithmetic: of 2,265 tracked paths, 2,186 declare
# `eol=lf` and 55 are tracked TEXT with NO such declaration (the rest are empty or
# binary). `.gitattributes` carries no `* text=auto`, and this workstation reports
# `core.autocrlf=false`, so for those 55 git performs NO normalisation on `git add`
# — a CRLF rewrite of any one of them LANDS CRLF IN THE COMMIT. Same for a NEW
# untracked file outside the pin.
# ⚠ `core.autocrlf=true` (the Git-for-Windows INSTALLER DEFAULT) makes a CRLF
# WORKING COPY of an unpinned text file the LEGITIMATE result of checkout, not a
# rewrite. Convicting there would be a false red on a correctly-configured host,
# so E1 states the situation instead of convicting — and E2 (untracked files,
# which were never checked out) keeps convicting either way.
_autocrlf="$(git config core.autocrlf 2>/dev/null || true)"
[[ -n "${_autocrlf}" ]] || _autocrlf='<unset>'
# E1 — TRACKED, text, NOT covered by an `eol=lf` pin, CRLF/mixed on disk.
# `i/-text` is binary (its 0x0D is legitimate) and `i/none` is empty; both are
# excluded by NAME rather than by "anything else", for the same reason check D
# uses a closed `w/` set: an unmeasured state must not be guessed into a red.
_worktree_unpinned="$(printf '%s\n' "${_eol_rows}" | awk -F'\t' '
    NF >= 2 && index($1, "eol=lf") == 0 {
        split($1, f, /[ \t]+/)
        if (f[1] == "i/-text" || f[1] == "i/none") next
        if (f[2] == "w/crlf" || f[2] == "w/mixed") print $2
    }')"
if [[ -n "${_worktree_unpinned}" ]]; then
    while IFS= read -r _f; do
        [[ -z "${_f}" ]] && continue
        if [[ "${_autocrlf}" == "true" ]]; then
            echo "line-endings: NOTE — '${_f}' is CRLF on disk and carries NO eol=lf pin, but core.autocrlf=true," >&2
            echo "    so a CRLF checkout is the expected result here and this is NOT convicted. Add an eol=lf pin" >&2
            echo "    for its extension if this repo should own its bytes regardless of a host's git config." >&2
        else
            _report+="  working (tracked, NOT covered by an eol=lf pin): CRLF on disk and core.autocrlf=${_autocrlf}, so \`git add\` will NOT normalise it — this WILL land CRLF in the commit: ${_f}"$'\n'
        fi
    done <<< "${_worktree_unpinned}"
fi
# E2 — UNTRACKED (not ignored), text, NOT covered by an `eol=lf` pin, has a CR.
# A pinned untracked file is deliberately NOT reported: its clean filter
# normalises it on `git add`, so it cannot land CRLF, and a guard that reds on a
# state git is about to fix teaches people to ignore it.
# ⚠ `tr -dc '\r' | wc -c`, never `grep -c $'\r'` — the latter matches the LETTER
# `r` in some shells (instrument note at the top of this file).
_worktree_untracked=""
while IFS= read -r _f; do
    [[ -z "${_f}" ]] && continue
    [[ -f "${_f}" ]] || continue
    # BINARY skip, using grep's own detection (the same family as `git grep -I`)
    # rather than a hand-rolled NUL scan.
    grep -qI . -- "${_f}" 2>/dev/null || continue
    # An explicit `binary` / `-text` declaration is an exemption, exactly as `-I`
    # honours it for the blob tiers.
    case "$(git check-attr text -- "${_f}" 2>/dev/null)" in *": text: unset") continue;; esac
    case "$(git check-attr eol  -- "${_f}" 2>/dev/null)" in *": eol: lf")     continue;; esac
    if [[ "$(tr -dc '\r' < "${_f}" | wc -c | tr -d ' ')" -ne 0 ]]; then
        _worktree_untracked+="${_f}"$'\n'
    fi
done <<< "$(git ls-files --others --exclude-standard 2>/dev/null)"
if [[ -n "${_worktree_untracked}" ]]; then
    while IFS= read -r _f; do
        [[ -z "${_f}" ]] && continue
        _report+="  working (untracked, not yet added): carries CR and NO eol=lf pin covers it, so \`git add\` will NOT normalise it — this WILL land CRLF in the commit: ${_f}"$'\n'
    done <<< "${_worktree_untracked}"
fi

if [[ -z "${_report}" ]]; then
    if [[ -n "${_skip_binary_check:-}" ]]; then
        # Say plainly what was NOT checked. A guard reporting "OK" while having
        # silently skipped half its checks is the self-blind-instrument shape
        # this repo keeps finding — the message must not outrun the evidence.
        echo "line-endings: OK (WORKTREE ONLY — the history scan was SKIPPED, see above; the ${_control_head} HEAD / ${_control_index} index blobs were NOT judged; ${_eol_row_count} working-tree paths were)"
    else
        # ★ The summary NAMES EVERY TIER it judged. A guard that says "OK" without
        # saying over what invites the reader to assume it covered the tier they
        # care about — and for four tiers of this guard's life, one of them
        # (the unstaged working tree) was the tier it did NOT cover.
        echo "line-endings: OK (committed ${_control_head} + staged ${_control_index} text blobs, working tree ${_eol_row_count} tracked paths + untracked, core.autocrlf=${_autocrlf}; none carries CR)"
    fi
    exit 0
fi

echo "line-endings: FAIL — the LF contract is violated (tier named per line):"
echo ""
printf '%s' "${_report}"
echo ""
echo "Fix:"
echo "  ★ A \`working (...)\` line is the CHEAP one: the bytes are only on your disk,"
echo "    nothing is committed yet, and converting the file to LF right now costs a"
echo "    single command. A \`committed (HEAD)\` line is the same defect after it"
echo "    became history. Fix the working tier BEFORE you stage."
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
echo ".plans/_deferred-anchor-registry*.md for why this is machine-checked."
exit 1
