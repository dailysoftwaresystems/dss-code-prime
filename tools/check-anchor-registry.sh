#!/usr/bin/env bash
# check-anchor-registry.sh — CI guard for the deferred-anchor registry
# discipline. Per memory + the cross-plan staleness sweep, this leak
# recurred TWICE before being system-enforced.
#
# Contract: every `D-*` identifier cited in a SCANNED ROOT (`src/`, `examples/`,
# `real-examples/` — see the roots table below) MUST resolve to a row in
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

# ════════════════════════════════════════════════════════════════════════════
# CHECK 2 of 2 — MARKDOWN TABLE CELL-WIDTH PROPERTY
# `D-PLANS-REGISTRY-ROWS-WITH-EXTRA-CELLS-STILL-LIVE` (registry) — CLOSED BY
# THIS CHECK.
#
# THE PROPERTY, stated as a property: **in every markdown table under
# `.plans/`, no data row may carry MORE cells than its own table's header.**
# A row with surplus cells has the overflow SILENTLY DROPPED by the renderer:
# the text stays in the file and vanishes from the page. Nothing about the raw
# text shows it, so the loss is invisible from both sides — the reader of the
# rendered page never sees the words, and the reader of the diff never sees a
# problem.
#
# ★★ WHY THIS IS A PROPERTY CHECK AND NOT A LIST OF ROWS. The registry row
# named above existed for exactly this defect and FAILED TO STOP IT, because
# it *named three instances* instead of asserting the property, and scoped
# itself to one table of one file. It thereby repeated the mistake its own
# text diagnoses. While it was open, **19 more rows accumulated** (15 in the
# registry, 4 in plan-00, dropping up to 39,403 characters in a single row).
# An instance list cannot fail; only a property can. If this check ever fires
# on an honestly-earned change, fix the ROW — never narrow the property.
#
# ★★ TWO SUB-SHAPES were found in the census, and a matcher tuned to one
# MISSES the other. The fix must be SHAPE-BLIND — it counts separators, it
# does not try to recognise intent:
#   (a) ALTERNATION pipes — `{block|EndStatement}`, `RECIPE|MAKE-N|DERIV`,
#       `MINGW*|MSYS*|CYGWIN*`, a shell pipeline `ls … | grep …`, `||`;
#   (b) ABSOLUTE-VALUE / delimiter bars — `1,108 steps >|0.1 s|` — not an
#       alternation at all, and invisible to any "looks like a regex" heuristic.
# Both are content. The repair for both is the SAME: escape to `\|`, which the
# registry's own authoring note records as the only form that survives inside a
# code span. A row whose surplus is a genuinely EXTRA CELL (a pre-close row's
# tail appended to a closed row) is joined with the registry's documented
# ` ═══ ` seam marker instead — a joint in the prose, not a column boundary.
#
# ⚠ WHAT IS **NOT** A VIOLATION: a row with FEWER cells than its header. The
# renderer pads the trailing columns blank and loses nothing, and the registry
# says so in its own words above the Allowlist table. There are 333 such rows.
# Failing them would force ~333 padding edits that change not one rendered
# character, i.e. it would enforce a PROXY (uniform width) instead of the
# PROPERTY (nothing invisible). They are COUNTED and reported, never fatal.
#
# ⚠ UNESCAPED PIPES ONLY. `\|` is content, not a separator. The count is
# `count("|") - count("\|")`, which is the `(?<!\\)\|` rule — and it is
# deliberately NOT implemented with `grep`: `grep -o '\|'` counts BRE
# ALTERNATION (it has produced two false alarms in this project's history) and
# `grep -P` is absent from Git Bash, where it exits 2 — a silent-false-negative
# trap in a guard. awk does the scan in both this script and its `.ps1` twin's
# reimplementation, and the two are checked against each other by diffing their
# output, not by assuming.
#
# ★★ FAIL-CLOSED, like every other check in this file: the root must exist and
# the scan must clear per-dimension FLOORS. A guard that reports success while
# scanning nothing is the worst defect a guard can have, and this file has
# already shipped that bug once (D-GATE-ANCHOR-GUARD-FAILS-OPEN-ON-MISSING-ROOT).
CELL_WIDTH_ROOT='.plans'
# Floors, far below the live figures so ordinary churn never trips them. These
# catch a COLLAPSED scan, not drift.
# ★ NO LIVE COUNT IS QUOTED HERE, ON PURPOSE, AND THAT IS A CORRECTION: the row
# total grows every time anyone appends a table row, so a figure written into
# this comment is stale by the next cycle. ✔MEASURED 2026-08-10 — it had already
# drifted into THREE disagreeing values (two comments saying 3,097, a registry
# row saying 3,098) against a live 3,099, i.e. every copy was wrong and none of
# them could be trusted to say so. The single owner of the live figures is THIS
# SCRIPT'S OWN OUTPUT, which prints files / tables / rows on every run; read it
# there. Point-in-time readings belong in the registry's audit trail, where a
# date makes them history rather than a claim about now.
CELL_WIDTH_FILE_FLOOR=20
CELL_WIDTH_TABLE_FLOOR=100
CELL_WIDTH_ROW_FLOOR=1500
# ★★★ THERE IS NO EXCEPTION LIST, AND THERE MUST NEVER BE ONE AGAIN.
# This check briefly shipped a `CELL_WIDTH_QUARANTINE` ratchet — 17 known-bad
# rows across 9 plan files, scanned and counted but with their count PINNED so
# the run stayed green. It was defended as "a ratchet, not an exemption", and
# the distinction did not survive contact: a quarantine is the instance list
# this check exists to replace, wearing a config entry. All 17 rows were
# REPAIRED — 26 stray content pipes escaped to `\|` across 12 rows, and 6
# genuinely EXTRA cells joined with the ` ═══ ` seam across 5 more; every file
# verified by BYTE ARITHMETIC, (seams x 8) + (escapes x 1) + (seam-pad spaces x 1)
# = +75 bytes over the nine files, with line counts unchanged — and the mechanism
# was DELETED, so this check now asserts the property with ZERO exceptions.
# ⚠ If it ever fires on an honestly-earned change, fix the ROW. Do not
# reintroduce a pin, and do not narrow the property — a guard that is weakened
# every time it fires ends up asserting nothing.

# The scanner. Kept in ONE place and fed a file list, so the `.ps1` twin has a
# single algorithm to mirror. POSIX awk only — no gawk extensions (`length(arr)`,
# `asort`, `PROCINFO`), because the macOS leg may run an old awk.
# ★ Every message this check emits is ASCII-ONLY, deliberately: the twins are
# verified by DIFFING their output, and a console-encoding difference between
# Git Bash and pwsh must never be able to fake a disagreement.
CELL_WIDTH_AWK='
function trim(s) { gsub(/^[ \t]+/, "", s); gsub(/[ \t]+$/, "", s); return s }
# Split a row on UNESCAPED pipes and return the cell count, leaving the cell
# text in CELLS[CELL_LO..CELL_HI]. `\|` is protected by a placeholder first, so
# no regex lookbehind is needed (and none is available in POSIX awk). The
# separator is written "[|]" and not "|": a one-character separator string is
# taken literally by gawk/mawk but a bare "|" compiled as a regex would be an
# EMPTY ALTERNATION matching between every character - a silent catastrophe. A
# bracket expression is unambiguous in every awk.
function cellsOf(s,   t, n, first, last) {
    t = s
    gsub(/\\\|/, "\001", t)
    n = split(t, CELLS, "[|]")
    first = 1; last = n
    if (trim(CELLS[first]) == "") first++
    if (last >= first && trim(CELLS[last]) == "") last--
    CELL_LO = first; CELL_HI = last
    return (last >= first) ? (last - first + 1) : 0
}
# ASCII-only, single-line, bounded - so the two twins can be diffed byte-wise.
# Stripping non-ASCII BEFORE truncating is what makes the two agree: removing
# every non-ASCII BYTE and removing every non-ASCII CHARACTER give the same
# string, after which substr() is byte- and char-identical. Truncating first
# would split a UTF-8 sequence in a byte-based awk and not in a char-based one.
function display(s,   t) {
    t = s
    gsub(/\001/, "|", t)
    gsub(/[^ -~]/, "", t)
    t = trim(t)
    return substr(t, 1, 110)
}
# ONE-LINE LOOKAHEAD. A table header is only recognisable by the line AFTER it,
# so each line is held until the next arrives. The pending line is flushed at
# every file boundary AND in END - without the first flush the LAST LINE OF
# EVERY FILE BUT THE LAST would go unchecked, and the filename is carried with
# it so a violation on that line is not misattributed to the next file.
FNR == 1 {
    if (have) emit(prevFile, prev, prevNo, "")
    inFence = 0; hdr = -1; hdrLine = 0; skipNext = 0; have = 0
}
{
    line = $0
    sub(/\r$/, "", line)
    if (have) emit(prevFile, prev, prevNo, line)
    prevFile = FILENAME; prev = line; prevNo = FNR; have = 1
}
function emit(fname, line, no, nxt,   s, ns, n, i, ex) {
    s = trim(line)
    if (s ~ /^```/ || s ~ /^~~~/) { inFence = !inFence; hdr = -1; return }
    if (inFence) return
    if (index(line, "|") == 0) { hdr = -1; return }
    if (skipNext) { skipNext = 0; return }
    ns = trim(nxt)
    # A table STARTS where a row is followed by a delimiter row. Deriving the
    # width from the table OWN header is the whole point: these files carry
    # 2-, 3-, 4-, 5- and 6-cell tables, so any global constant would be wrong
    # somewhere - and a check that is wrong somewhere trains people to ignore it.
    if (index(nxt, "|") > 0 && ns ~ /^\|?[ ]*:?-+:?[ ]*(\|[ ]*:?-+:?[ ]*)*\|?$/) {
        hdr = cellsOf(line); hdrLine = no; tables++; skipNext = 1; return
    }
    if (hdr < 0) return
    rows++
    n = cellsOf(line)
    if (n < hdr) { under++; return }
    if (n == hdr) return
    over++
    ex = ""
    for (i = CELL_LO + hdr; i <= CELL_HI; i++) ex = ex (ex == "" ? "" : " // ") trim(CELLS[i])
    OVER_F[over] = fname; OVER_L[over] = no; OVER_H[over] = hdrLine
    OVER_E[over] = hdr; OVER_A[over] = n; OVER_X[over] = display(ex)
}
END {
    if (have) emit(prevFile, prev, prevNo, "")
    # ★ `collapsed` and `violated` are kept SEPARATE rather than assigning into
    # one `status`. With a single variable, whichever branch ran LAST would own
    # the exit code, so a COLLAPSED scan could be reported as a content-drop and
    # send the reader to the wrong file. Both are red either way; the PRECEDENCE
    # (collapse 2 > content-dropped 3) is decided once, in the `exit` below.
    collapsed = 0; violated = 0
    # ★ FORCE the four counters NUMERIC before anything prints them. An awk
    # variable that was never assigned is the EMPTY STRING in a concatenation, so
    # on the all-clean path `over` had never been touched and the summary read
    # "files:  violation(s)" — a blank where the number belongs. ✔MEASURED
    # 2026-08-10 by RUNNING the green arm after the quarantine was deleted (the
    # deleted code happened to initialise its own counter, so removing it exposed
    # this). Same hazard for `tables`/`rows`/`under` inside the collapse
    # messages, which is the one place they are guaranteed to be small.
    over = over + 0; under = under + 0; tables = tables + 0; rows = rows + 0
    if (files < fileFloor) {
        print "anchor-registry: FAIL - cell-width scan found only " files " markdown files under " root ", below its floor of " fileFloor "."
        print "  This does NOT mean the plans are clean - it means the SCAN COLLAPSED. Refusing to report a pass; fix the scan, do not lower the floor."
        collapsed = 1
    }
    if (tables < tableFloor) {
        print "anchor-registry: FAIL - cell-width scan found only " tables " tables, below its floor of " tableFloor "."
        print "  A collapsed table scan checks nothing. Refusing to report a pass; fix the scan, do not lower the floor."
        collapsed = 1
    }
    if (rows < rowFloor) {
        print "anchor-registry: FAIL - cell-width scan found only " rows " table data rows, below its floor of " rowFloor "."
        print "  A collapsed row scan checks nothing. Refusing to report a pass; fix the scan, do not lower the floor."
        collapsed = 1
    }
    # ★★ THE VIOLATION COUNT IS COUNTED, NEVER DERIVED. `over` is incremented at
    # exactly ONE place — the moment a row is found wider than its header — and
    # that SAME variable drives the FAIL headline, the detail loop bound and the
    # summary line. There is no second source of truth for it to disagree with,
    # and nothing is subtracted from it.
    # ✔MEASURED 2026-08-10, by exercising the failure arm rather than reading it:
    # while a quarantine ratchet existed here, the live count was DERIVED as
    # `scanned - pinned`, which printed "-1 live violation(s)" the moment a
    # pinned row was repaired, and — far worse — a repair in a pinned file plus
    # a NEW break in a clean one CANCELLED to "0 live" while the run was
    # failing. A guard whose own summary can read 0 during a failure is the
    # `printed failed=0 and exited 2` shape all over again. The ratchet is gone;
    # this note stays so the subtraction is never reintroduced with it.
    if (over > 0) {
        print "anchor-registry: FAIL - " over " markdown table row(s) carry MORE cells than their table header."
        print "The surplus is SILENTLY DROPPED by the renderer: the text stays in the file and"
        print "vanishes from the page. This is invisible from both sides - nothing in the raw"
        print "text shows it, and nothing in the diff shows it."
        print ""
        for (i = 1; i <= over; i++) {
            print "  " OVER_F[i] ":" OVER_L[i] "  header@" OVER_H[i] " expected=" OVER_E[i] " actual=" OVER_A[i]
            print "      dropped: " OVER_X[i]
        }
        print ""
        print "Fix: the surplus is CONTENT that was read as a column boundary. Escape a content"
        print "pipe as \\| (the only form that survives inside a code span); join a genuinely"
        print "extra cell with the registry seam marker instead of a column boundary. Never"
        print "delete text to make the count fit, and never widen the check."
        violated = 1
    }
    print "anchor-registry: cell-width " tables " tables / " rows " rows in " files " files: " over " violation(s), " under " short rows (padded, no loss)."
    exit (collapsed ? 2 : (violated ? 3 : 0))
}
'

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
# ── ONE cumulative temp-file registry and ONE trap ───────────────────────────
# ★ Set ONCE, and every temp file registers itself. Reassigning `trap … EXIT` per
# temp file REPLACES the handler rather than adding to it, so the last assignment
# silently stops cleaning up everything the earlier ones covered — which is
# exactly what an intermediate version of this script did, leaking three files on
# every FAILING run (the runs a CI box makes most of). Found by grepping this
# file's own `trap` sites after adding the third one.
# `${_tmps[@]+"${_tmps[@]}"}` is the bash-3.2-safe expansion (macOS ships 3.2,
# where `set -u` makes a plain empty `"${arr[@]}"` a fatal unbound-variable).
# ⚠ Each site registers itself on the SAME LINE, deliberately, instead of through
# a `_t="$(_mktmp)"` helper. ✔MEASURED: the helper form cleaned up NOTHING —
# command substitution runs in a SUBSHELL, so the helper appended to the
# subshell's copy of `_tmps` and the parent array stayed empty. The leak was
# exactly one file per `mktemp` call (4 on a green run, 6 on a failing one),
# counted by watching TMPDIR across a run rather than by reading the code.
_tmps=()
trap 'rm -f ${_tmps[@]+"${_tmps[@]}"}' EXIT

_anchor_tmp="$(mktemp)"; _tmps+=("${_anchor_tmp}")

# ── RUN CHECK 2 (cell-width) FIRST, but do NOT exit on it yet.
# Both checks report in ONE run. The `.sh` learned this the hard way on the
# anchor path (see the `|| true` note below): a guard that aborts after the
# first problem makes the reader re-run it N times to learn N things, and the
# remediation text never prints at all.
_cw_status=0
if [[ ! -d "${CELL_WIDTH_ROOT}" ]]; then
    echo "anchor-registry: FAIL - cell-width root '${CELL_WIDTH_ROOT}' does not exist. Refusing to report a pass on a scan of nothing."
    _cw_status=2
else
    # Filenames under .plans/ contain SPACES, so the list goes through an array
    # (no `mapfile`: macOS still ships bash 3.2, where it does not exist).
    # LC_ALL=C makes the order BYTE-ordinal, which is what the .ps1 twin sorts
    # by — culture-aware ordering would place `_deferred-…` differently and the
    # two twins' output would differ for no real reason.
    _cw_files=()
    while IFS= read -r _cw_f; do _cw_files+=("${_cw_f}"); done < <(
        find "${CELL_WIDTH_ROOT}" -type f -name '*.md' -print | LC_ALL=C sort)
    # ★ The empty case is handled HERE, before awk, and not left to the floor
    # inside awk. Under `set -u` an empty `"${arr[@]}"` is an unbound-variable
    # FATAL on bash 3.2 — which is the bash macOS ships, i.e. exactly the
    # platform this `.sh` exists to serve. It would still be red, but red with a
    # bash error instead of the sentence that tells the reader what happened, and
    # "the guard died" reads to most people like "the guard is broken", not
    # "the scan collapsed".
    if [[ "${#_cw_files[@]}" -eq 0 ]]; then
        echo "anchor-registry: FAIL - cell-width scan found only 0 markdown files under ${CELL_WIDTH_ROOT}, below its floor of ${CELL_WIDTH_FILE_FLOOR}."
        echo "  This does NOT mean the plans are clean - it means the SCAN COLLAPSED. Refusing to report a pass; fix the scan, do not lower the floor."
        _cw_status=2
        _cw_files=()
    else
    # rc DIRECTLY, never after a pipe: awk's status IS the verdict here.
    set +e
    awk -v root="${CELL_WIDTH_ROOT}" \
        -v files="${#_cw_files[@]}" \
        -v fileFloor="${CELL_WIDTH_FILE_FLOOR}" \
        -v tableFloor="${CELL_WIDTH_TABLE_FLOOR}" \
        -v rowFloor="${CELL_WIDTH_ROW_FLOOR}" \
        "${CELL_WIDTH_AWK}" "${_cw_files[@]}"
    _cw_status=$?
    set -e
    fi
fi

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
# ★★ THE SAME TEST, IN THREE PHASES — AND PHASE 3 IS THE AUTHORITY. The first
# two only make it cheap; anything they cannot settle falls through to the
# original per-anchor `grep -qrF`, so the VERDICT is unchanged by construction.
# ✔MEASURED 2026-08-10, decomposing this guard's own runtime on the live tree:
# the three collection greps cost **242 ms** and this resolve cost **18,328 ms**
# — 99% of the base runtime was 905 sequential `grep -qrF` subprocesses, each
# re-walking all of `.plans/` (7.2 MB), once per anchor, on every single gate
# run. Phase 1 does it in **63 ms** and phase 1b in **58 ms**, leaving **ZERO**
# work for phase 3 on a clean tree ⇒ ~121 ms for the same answer.
# ⓘ The `.ps1` twin was ALREADY doing the cheap thing (it joins every plan file
# into one string and calls `.Contains()` per anchor in memory), which is why its
# clean run measured 9.6 s against this script's 14.7 s. This closes that gap
# from the slow side rather than making the fast side sloppier.
# ★ WHY PHASE 3 CANNOT BE DROPPED, measured rather than reasoned about: with
# `-o -F -f`, grep reports the LONGEST match at a position, so an anchor that is
# a PREFIX of a longer anchor in the same text is swallowed and never emitted.
# ✔MEASURED: 905 patterns, 812 emitted, **93 unemitted — every one of them a
# prefix**. Phase 1b recovers exactly those by substring-closing over the emitted
# set (if `a` is a substring of a token that DOES occur, then `a` occurs), which
# took all 93 to 0. Phase 3 then exists for the case neither phase covers, and it
# is the ONLY phase allowed to conclude "missing".
# ⚠ Phase 1 failing wholesale is FAIL-SLOW, never fail-wrong: every anchor
# becomes a phase-3 candidate and the guard is merely as slow as it used to be.
#
# Substring (vs equality) is the contract, unchanged: it handles
#   (1) a multi-line citation in src whose comment wraps the anchor name, so the
#       regex captured only the prefix;
#   (2) plans using a MORE SPECIFIC name (`D-LK6-14-INTEGRATION-GOT-SLOTS`)
#       while src cites the parent (`D-LK6-14-INTEGRATION`) — both are "known"
#       via the same row.
_plan_pat="$(mktemp)";  _tmps+=("${_plan_pat}")
_plan_hit="$(mktemp)";  _tmps+=("${_plan_hit}")
_plan_cand="$(mktemp)"; _tmps+=("${_plan_cand}")
# Blank lines are stripped: a blank line in a `grep -F -f` pattern file matches
# EVERY line, which would resolve every anchor and green the guard silently.
{ printf '%s\n' "${SRC_ANCHORS}" | grep . || true; } > "${_plan_pat}"
# PHASE 1 — ONE pass over `.plans/`, emitting each anchor string that occurs.
# `.plans/` (all files), not `.plans/**/*.md`, to keep this script's documented
# scope — the `.ps1` reads only `*.md`, and that divergence is recorded there.
{ grep -rhoF -f "${_plan_pat}" .plans/ 2>/dev/null || true; } \
    | LC_ALL=C sort -u > "${_plan_hit}"
# PHASE 1b — substring closure, then everything left over is a phase-3 candidate.
# ⚠ The first-file test is `FILENAME == hitf`, NOT `FNR == NR`: with an EMPTY
# phase-1 result `NR` never advances, so `FNR == NR` would be TRUE for the FIRST
# pattern and silently treat it as RESOLVED — a false GREEN on one anchor, which
# is the one direction a guard must never fail in.
awk -v hitf="${_plan_hit}" '
FILENAME == hitf { n++; R[n] = $0; next }
{
    a = $0
    if (a == "") next
    for (i = 1; i <= n; i++) if (index(R[i], a)) next
    print a
}' "${_plan_hit}" "${_plan_pat}" > "${_plan_cand}"
# PHASE 3 — THE AUTHORITY. Unchanged from the original loop, just fed far fewer
# anchors. Order is preserved: SRC_ANCHORS is already `sort -u` and both phases
# above emit in input order, so MISSING lists in the same sequence as before.
MISSING=()
while IFS= read -r src_a; do
    [[ -z "${src_a}" ]] && continue
    if ! grep -qrF -- "${src_a}" .plans/ 2>/dev/null; then
        MISSING+=("${src_a}")
    fi
done < "${_plan_cand}"

if [[ ${#MISSING[@]} -eq 0 ]]; then
    # ${_anchor_count}, not `echo "${SRC_ANCHORS}" | wc -l` — the latter reports 1 for
    # an EMPTY set (echo emits a lone newline), which is exactly how the fail-open bug
    # above dressed a scan of nothing as "OK (1 src anchors all resolve)".
    echo "anchor-registry: OK (${_anchor_count} src anchors all resolve to plans)"
    # ★ The cell-width verdict is NOT allowed to be swallowed by the anchor
    # check's success. Exit codes: 1 = an anchor resolves nowhere, 2 = a scan
    # collapsed, 3 = a markdown table row drops content.
    exit "${_cw_status}"
fi

echo "anchor-registry: FAIL — the following anchors are cited in a SCANNED ROOT"
echo "(src/, examples/, real-examples/ — NOT src/ alone) but"
echo "have no matching row/citation in any .plans/*.md file:"
echo ""
# ★★ THE TREE IS WALKED ONCE — NOT ONCE PER MISSING ANCHOR.
# ✔MEASURED 2026-08-10 on the live tree (~2k source files, 6,496 anchor
# occurrences): the previous form ran the two `grep -rln` calls INSIDE the
# per-anchor loop, so the cost was (missing anchors) x (whole tree). With 8
# missing anchors the FAIL path took **18.1 s**; extrapolated to a wholesale
# break (every anchor stranded, which is exactly what a deleted plan file
# causes) it is ~30 MINUTES — and the observed symptom is a guard that looks
# HUNG, at the precise moment its output is most needed. The .ps1 twin had the
# same shape and measured **82.9 s** for the same 8.
# The fix walks the roots ONCE into an ordered `path:anchor` index, then each
# anchor's lookup is an in-memory table scan plus a `seen[]` hash — no file I/O
# at all. Behaviour is UNCHANGED and that was verified by DIFFING the whole FAIL
# report before and after on the identical mutation, not by reasoning about it.
# ★ The index is built with the SAME `ANCHOR_REGEX` that produced the anchors
# above, and attribution is by SUBSTRING (`index()`), which preserves the one
# non-obvious behaviour of the old `grep -rln "${anchor}"`: a file citing a MORE
# SPECIFIC anchor is also listed under its PREFIX. That prefix relationship is
# real in this repo — the plan-side resolve documents and relies on it — so
# dropping it would have silently shrunk the report.
# ★ `|| true` ON EACH grep IS LOAD-BEARING — NOT DEFENSIVE NOISE.
# ✔MEASURED 2026-08-08: without it this guard reported only the FIRST missing
# anchor, printed NO `cited in:` lines, and swallowed the whole `Fix:` trailer
# below. Cause: `set -euo pipefail` (top of file) applies inside the brace group,
# so a grep that matches nothing exits 1, `set -e` aborts the group before the
# second grep runs, and pipefail then kills the script mid-report. A grep that
# finds nothing is a NORMAL outcome here, never an error. Hoisting the greps out
# of the loop does not retire that hazard — it still applies to this ONE run.
_locator_tmp="$(mktemp)"; _tmps+=("${_locator_tmp}")
{ grep -rEo "${ANCHOR_REGEX}" src/ examples/ \
    --include='*.cpp' --include='*.hpp' --include='*.json' --include='*.c' 2>/dev/null || true; \
  grep -rEo "${ANCHOR_REGEX}" real-examples/ \
    --include='*.sh' --include='*.ps1' --include='*.py' 2>/dev/null || true; } \
    > "${_locator_tmp}"
# The missing list goes through a file too, so an anchor containing a shell
# metacharacter could never be re-interpreted on its way into awk.
_missing_tmp="$(mktemp)"; _tmps+=("${_missing_tmp}")
printf '%s\n' "${MISSING[@]}" > "${_missing_tmp}"
# An index that came back EMPTY means the locator can attribute NOTHING, and the
# report would then be a list of bare anchor names with no `cited in:` line —
# precisely the guessing game the note above says this locator exists to prevent.
# It should be unreachable (the per-root floors above already proved these same
# filters match hundreds of anchors), so say so LOUDLY rather than degrade
# quietly if it ever happens.
if [[ ! -s "${_locator_tmp}" ]]; then
    echo "anchor-registry: WARNING — the citation index is EMPTY, so no 'cited in:' line can be produced."
    echo "  The per-root floors above passed, so this should be impossible; the locator greps"
    echo "  (same roots, same --include set) must have stopped matching. Fix the scan."
fi
# POSIX awk only (macOS ships an old one): no `delete arr`, no `asort`. The
# de-dup key is a compound `anchor SUBSEP path`, so nothing needs clearing
# between anchors.
# ⚠ The first-file test is `FILENAME == idxf`, NOT the usual `FNR == NR`. With an
# EMPTY index file `NR` never advances, so `FNR == NR` is TRUE for the FIRST
# record of the SECOND file — the missing-anchor list would be silently parsed as
# index rows and the report would print nothing at all. Comparing FILENAME cannot
# misfire on an empty input.
awk -v idxf="${_locator_tmp}" '
FILENAME == idxf {
    # path = everything before the LAST colon, token = everything after it.
    # Splitting on the last colon (not the first) keeps a path containing a
    # colon intact; the token itself can never contain one.
    line = $0
    if (line == "") next
    p = line; sub(/:[^:]*$/, "", p)
    t = line; sub(/^.*:/, "", t)
    if (p == "" || t == "") next
    if (PAIR[p SUBSEP t]) next     # collapse repeat occurrences, keep order
    PAIR[p SUBSEP t] = 1
    n++; IP[n] = p; IT[n] = t
    next
}
{
    a = $0
    if (a == "") next
    print "  " a
    for (i = 1; i <= n; i++) {
        if (index(IT[i], a) == 0) continue
        if (SEEN[a SUBSEP IP[i]]) continue
        SEEN[a SUBSEP IP[i]] = 1
        print "    cited in: " IP[i]
    }
}
' "${_locator_tmp}" "${_missing_tmp}"
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
# ★ BOTH halves report on every run — that is settled above. But an exit code
# can carry only ONE number, so when both fail the precedence is DELIBERATE
# rather than whichever branch happens to run last: a COLLAPSED scan (2) beats a
# missing anchor (1) beats dropped content (3), because a scan that checked
# nothing makes the other two verdicts untrustworthy. ✔MEASURED 2026-08-10: with
# 20 plan files removed, the cell-width floors fire AND every anchor stops
# resolving, and without this the `exit 1` below silently outranked the collapse.
if [[ "${_cw_status}" -eq 2 ]]; then exit 2; fi
exit 1
