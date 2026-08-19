#!/usr/bin/env bash
# PURPOSE: refuse a `D-*` anchor cited in a scanned root that resolves to no registry row, and refuse a markdown table row whose unescaped pipes would silently drop cells.
# check-anchor-registry.sh — CI guard for the deferred-anchor registry
# discipline. Per memory + the cross-plan staleness sweep, this leak
# recurred TWICE before being system-enforced.
#
# Contract: every `D-*` identifier cited in a SCANNED ROOT (`src/`, `examples/`,
# `tests/`, `integrated_tests/`, `real-examples/` — see the roots table below)
# MUST resolve to a row in
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

# Locate repo root (this script lives at scripts/check-anchor-registry/).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

# Anchor regex: D- followed by 3+ uppercase-or-digit segments separated by `-`.
# Same pattern the developer-side audit grep uses (see the cross-plan
# staleness sweep commit message). Two-segment names like `D-OPT` are
# treated as informal; the registry contract enforces ≥3 segments.
# ── THE LEADING `\<` IS LOAD-BEARING; the `.ps1` twin spells it `\b` ──────────
# It stops the regex reaching INSIDE a longer hyphenated word and lifting a false
# anchor out of its tail. The live case is the phrase `FIXED-32-BIT-WORD` at
# `src/asm/format/fixed32.hpp:19`: its tail is anchor-SHAPED, but the `D` is
# preceded by `E`, so there is no word boundary and the phrase is correctly
# skipped. Drop `\<` and this guard reds forever on an innocent comment.
# ⚠ Do NOT illustrate that tail by writing it out as a standalone literal — it
# becomes a citation of an anchor with no row, in the guard's own source. The
# `.ps1` twin carried exactly that from TF-C111 until AP6 (2026-08-14) and it was
# the sole thing blocking `scripts/` from being scanned. Any placeholder must stay
# UNDER the threshold: `D-XX-EXAMPLE` is inert here, three segments are not.
# See D-GATE-ANCHOR-GUARD-SCOPE-STILL-EXCLUDES-TOOLS-AND-TESTS.
ANCHOR_REGEX='\<D-[A-Z0-9_]+(-[A-Z0-9_]+){2,}'

# ── EVERY **GREP** IN THIS FILE RUNS IN THE BYTE LOCALE (`LC_ALL=C`) ─────────
# (The awk passes are deliberately NOT prefixed — see the end of this note.)
# The anchor grammar above is pure ASCII by construction, so byte semantics ARE
# the correct semantics for it — this is not a speed hack that trades away
# meaning. The plans and the registry are full of UTF-8 (★, ✔, —, arrows), but
# none of it can ever be part of an anchor, and the verdict was proved unchanged
# by DIFFING this script's whole stdout + exit code between the two locales
# rather than by asserting it (see the macOS measurements at PHASE 1 below:
# identical sha256 across `en_US.UTF-8` and `C`, on both the OK and the FAIL
# path).
# ★ THE ONE SEMANTIC RISK WAS PROBED DIRECTLY RATHER THAN ARGUED AWAY. `\<` is
# defined against the locale's word characters, so a MULTIBYTE LETTER sitting
# immediately before an anchor is the case where a byte locale and a UTF-8
# locale could legitimately disagree about whether a boundary exists — the byte
# locale seeing two non-word bytes where the UTF-8 locale sees one letter.
# ✔MEASURED 2026-08-12 by injecting a real `é` (bytes 0xC3 0xA9) directly
# against an anchor name and running both locales: the reports were BYTE-
# IDENTICAL (sha b150af36b8e4afd4) and BOTH found the anchor, so BSD grep does
# not in fact diverge here. Had it diverged, the byte locale is the FAIL-LOUD
# direction anyway (it finds MORE anchors, all of which must then resolve).
# ⚠ WHY THIS IS PER-COMMAND AND NOT ONE `export LC_ALL=C` AT THE TOP. A global
# export would also re-collate the two `sort -u` calls that order `SRC_ANCHORS`,
# and THAT is verdict-visible: the FAIL report lists missing anchors in
# SRC_ANCHORS order, so byte-vs-culture collation of `-` against `_` would
# reorder the report. Prefixing the SCANS leaves every `sort` exactly as it was,
# which makes the "output is unchanged" claim true by construction for ordering
# and measured for content. If a future scan is added here, prefix it too.
# ⓘ SIZE OF THE EFFECT, SO NOBODY OVER-CREDITS IT. On GNU grep the locale costs
# nothing either way (✔MEASURED: Linux 0.145 s vs 0.179 s, Git-Bash 1.0-1.5 s —
# indistinguishable). On the BSD grep macOS ships it is worth about **1.2x**
# (✔MEASURED on the fixed script: 4.288 s in `C` against 5.168 s in
# `en_US.UTF-8`), and the regex scan PHASE 1 now uses is locale-INSENSITIVE
# outright (0.245 s vs 0.219 s — the byte locale was marginally the SLOWER of
# the two there, i.e. inside the noise).
# ⚠ So `LC_ALL=C` is NOT what fixed this guard, and an earlier hypothesis that
# it would buy 10-100x was REFUTED by measurement before any of it was written.
# It is kept because it is free and semantically right, not because it is the
# cure; the cure is the algorithmic change PHASE 1 documents.
# ⓘ THE `awk` PASSES ARE LEFT UNPREFIXED, DELIBERATELY AND AS AN UNMEASURED
# NON-CHANGE. After the PHASE 1 fix the whole guard costs 4.288 s on the Mac, of
# which the greps account for 0.365 s (0.245 phase 1 + 0.120 collection) — so
# the cell-width awk is most of the remainder. It is very likely it would also
# be cheaper in the byte locale. It is NOT prefixed because the saving is a
# couple of seconds on a run that is already fast, and because prefixing it
# would change the locale under `gsub(/[^ -~]/, ...)` and `substr()` — the two
# operations `display()` above argues are byte/char-identical — without that
# argument having been re-verified end-to-end. A measured 2 s is not worth an
# unmeasured change to the one function whose output the two twins are diffed
# on. If someone wants it, MEASURE the twins' outputs against each other first.

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

# ── tests/ + integrated_tests/ ADDED 2026-08-14 (AP6),
# D-GATE-ANCHOR-GUARD-SCOPE-STILL-EXCLUDES-TOOLS-AND-TESTS.
# The guard promised "every `D-*` resolves" and delivered it for the subtree it
# looked at, which is worse than no guard because it is TRUSTED. `tests/` is not
# a quiet corner: ✔MEASURED at the widening, **709 distinct anchors** are cited
# there and 18 more in `integrated_tests/` — comparable to `examples/` (406) and
# on the same order as `src/` (864). The two corpus HARNESSES live in these
# roots, i.e. the code that decides whether every other root's pins ran at all.
#
# ★★ THE MATCHER WAS **NOT** CHANGED, AND THAT WAS A MEASURED DECISION, NOT AN
# OMISSION — read this before "fixing" the wrapped-name case.
# The widening was proposed on the premise that a comment which WRAPS an anchor
# name across two lines defeats a line-based scan, leaving a bare prefix like
# `D-CSUBSET-STRUCT`, and that ~62 such fragments would strand. ✔MEASURED on the
# live tree before touching anything: over `tests/` + `integrated_tests/`, of
# 705 distinct names extracted with the source filter set, the number that fail
# the resolve is **4** — and not one of them is a wrapped fragment.
# The premise was wrong because THIS GUARD ALREADY HANDLES THAT CLASS BY
# CONSTRUCTION: the resolve is a SUBSTRING test (`grep -qrF`, phase 3 below),
# and a wrapped fragment is by definition a PREFIX of the real anchor, so it is
# a substring of any plan text carrying the whole name. That is case (1) of the
# two the resolve documents at PHASE 1's comment, and it has been the contract
# since the guard was written.
# ⇒ Teaching the matcher to join comment-continuation lines would have been a
# change to a `grep` with NO failing case behind it, in a file whose own history
# is a list of subtle `grep` regressions (`grep -P` absent from Git Bash and
# exiting 2 as a silent false negative; `-o` swallowing prefixes; `-F -f`
# quadratic on BSD grep). Reflowing the comments instead would have been a large
# no-behaviour diff enforcing a rule no future author can be held to. Neither
# was needed; the ROOTS were the whole defect. The substring contract is now
# pinned from the test side, because it is load-bearing rather than incidental:
# narrow it to a whole-word match and dozens of wrapped citations strand at once.
#
# ⚠ THE FILE-TYPE FILTERS WERE WIDENED IN THE SAME CHANGE, and it is not
# cosmetic: `CMakeLists.txt` is where the ctest ENTRY NAMES live, and entry names
# are cited as landed evidence by closed registry rows. ✔MEASURED: 10 CMake
# files across `src/` and `tests/` cite anchors and none of them was scanned,
# so `tests/examples/CMakeLists.txt` could rename an entry a registry row names
# and nothing would notice. `*.md`, `*.s`, `*.inc` and `*.probes` came along for
# the same reason — a citation is a citation whatever the extension — and
# ✔MEASURED at 89 additional distinct anchors across all roots, **0 unresolved**.
# `real-examples/` keeps its DRIVER-only filter set: it contains zero files of
# any of the added types, so adding them there would be the silent no-op version
# of a fix, which this file already names as a hazard above.
#
# ★ WHAT MUST LAND WITH THIS AND CANNOT BE VERIFIED BY THE GUARD ITSELF.
# ✔MEASURED 2026-08-14: 11 of the names cited under `tests/` resolve TODAY only
# because the registry row named at the top of this note QUOTES them verbatim
# while complaining that they are unregistered — remove that one row's lines and
# they strand. That row predicted this exact false green in its own text ("a
# detector whose contract is 'the string appears somewhere' can be satisfied by
# the very document that reports the violation"). The widening is therefore
# green on the honest set ONLY if the rows/Allowlist entries for those 11 land
# in the same commit. They are not the guard's to create and it cannot tell the
# difference — which is the residual, and it is recorded here rather than left
# to be rediscovered.
#
# ★ ONE TABLE, READ BY BOTH THE COLLECTION SCAN **AND** THE FAIL-PATH LOCATOR.
# It was already one table for the collection loop, and the note above says why;
# what that note did not cover is that the LOCATOR near the bottom of this file
# spelled the roots and their filters a THIRD time, hardcoded. ✔MEASURED at the
# AP6 widening: adding two roots to the table alone left the locator scanning the
# old three, so a missing anchor cited ONLY under `tests/` would have been
# reported with no `cited in:` line at all — the exact "guessing game" the
# locator's own comment says it exists to prevent, and silent, because a report
# missing one line still looks like a report. It is now derived from this array,
# so a root cannot be half-added.
# ⚠ bash 3.2 (macOS): index with `"${arr[@]}"` only where the array is known
# non-empty, which it is here by literal construction.
#
# root|floor|include-globs (space separated)
_ROOT_SPECS=(
    'src|400|*.cpp *.hpp *.json *.c *.s *.inc *.probes CMakeLists.txt *.md'
    'examples|150|*.cpp *.hpp *.json *.c *.s *.inc *.probes CMakeLists.txt *.md'
    'tests|300|*.cpp *.hpp *.json *.c *.s *.inc *.probes CMakeLists.txt *.md'
    'integrated_tests|8|*.cpp *.hpp *.json *.c *.s *.inc *.probes CMakeLists.txt *.md'
    'real-examples|10|*.sh *.ps1 *.py'
)
for _spec in "${_ROOT_SPECS[@]}"
#
# ★ ONE INSTANCE FOUND INDEPENDENTLY ON THE OTHER BRANCH, kept because it is the
# sharpest argument for this widening that either side produced:
# `D-EXAMPLES-RUNNER-TWO-RUNNERS-MUST-AGREE` was cited in `integrated_tests/runner.cpp`
# as the CLASS NAME of the rule "one runner enforcing while its sibling shrugs is a
# silent harness bug" — and no such row had ever been written. The project was citing
# a rule by an anchor that does not exist, in the very file whose violation of it was
# being closed. An unenforced citation is a citation that rots.
# ⚠ A future root that reds is closed by REGISTERING the rows, never by narrowing
# the guard.
do
    _root="${_spec%%|*}"; _rest="${_spec#*|}"
    _floor="${_rest%%|*}"; _globs="${_rest#*|}"
    if [[ ! -d "${_root}" ]]; then
        echo "anchor-registry: FAIL — scan root '${_root}' does not exist. A missing root would silently shrink coverage; refusing to report a partial scan as a pass." >&2
        _scan_failed=1; continue
    fi
    _inc=(); for _g in ${_globs}; do _inc+=(--include="${_g}"); done
    _hits="$(LC_ALL=C grep -rEoh "${ANCHOR_REGEX}" "${_root}/" "${_inc[@]}" 2>/dev/null | sort -u || true)"
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
# ⚠ THOSE 2026-08-10 FIGURES ARE LINUX-ONLY AND THAT WAS THE DEFECT. The phase-1
# form they justified (`grep -F -f` with the whole anchor list as patterns) is
# ~1500x slower on the BSD grep macOS ships; see the macOS numbers and the
# rewrite at PHASE 1 below. The three-phase STRUCTURE survived that rewrite
# unchanged — only phase 1's matcher changed — which is why these numbers are
# kept rather than deleted: they are still the reason the structure exists.
# ★ WHY PHASE 3 CANNOT BE DROPPED, measured rather than reasoned about: with
# `-o`, grep reports the LONGEST match at a position, so an anchor that is a
# PREFIX of a longer anchor in the same text is swallowed and never emitted under
# its own name. ✔MEASURED on the `-F -f` form: 905 patterns, 812 emitted, **93
# unemitted — every one of them a prefix**. Phase 1b recovers exactly those by
# substring-closing over the emitted set (if `a` is a substring of a token that
# DOES occur, then `a` occurs), which took all 93 to 0. The same swallowing
# applies to the pattern-scan form that replaced it — MORE so, since it emits
# only whole anchor tokens — so the closure is now load-bearing by design rather
# than as a rescue. Phase 3 then exists for the case neither phase covers, and it
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
# Blank lines are stripped. They are no longer the catastrophe they were while
# this list was a `grep -F -f` pattern file (a blank pattern matches EVERY line,
# which would resolve every anchor and green the guard silently), but PHASE 3
# still greps each entry, so an empty one would still match everything — hence
# both this filter and the `[[ -z ]]` guard in the phase-3 loop.
{ printf '%s\n' "${SRC_ANCHORS}" | grep . || true; } > "${_plan_pat}"
# PHASE 1 — ONE pass over `.plans/`, emitting the plan-side anchor VOCABULARY.
# `.plans/` (all files), not `.plans/**/*.md`, to keep this script's documented
# scope — the `.ps1` reads only `*.md`, and that divergence is recorded there.
#
# ★★ THIS SCANS FOR THE ANCHOR PATTERN, NOT FOR THE 950 SRC ANCHORS. That is a
# deliberate reversal, and it is what makes this guard usable on macOS.
# D-GATE-ANCHOR-GUARD-PHASE1-QUADRATIC-ON-BSD-GREP — CLOSED BY THIS BLOCK.
# ✔MEASURED 2026-08-12 on the operator's Mac (macOS 26.5.2 arm64, BSD grep
# 2.6.0-FreeBSD), matched content against Linux at the same commit:
#     BEFORE, whole guard, en_US.UTF-8, fresh clone   274.6 s
#     BEFORE, whole guard, en_US.UTF-8, warm          317.6 s
#     BEFORE, whole guard, LC_ALL=C,    warm    180.4 s / 153.4 s / 270.6 s
#     AFTER,  whole guard, LC_ALL=C                     4.288 s
#     AFTER,  whole guard, en_US.UTF-8                  5.168 s
#     this phase alone: 0.245 s, against >4 min for the form it replaced
#     the same script on Linux (GNU grep 3.11) 0.17 s, Git-Bash (GNU 3.0) ~1.1 s
# ⇒ ~63x on the macOS leg, with the verdict BYTE-IDENTICAL (same sha256 across
# before/after and across both locales, on the OK path AND on an injected-
# missing-anchor FAIL path).
# — and `ps` caught the run parked at 100% CPU inside `grep -rhoF -f <950
# patterns> .plans/` in BOTH locales. THREE hypotheses died on those numbers:
# it is not tree size (this Mac's scanned roots are SMALLER than the Linux
# tree's), it is not a cold page cache (the WARM run was the SLOWER of the two,
# and grep was CPU-bound at 100%, never blocked on I/O), and it is not the
# locale (worth ~1.2x; see the note under ANCHOR_REGEX).
# The cause is algorithmic: GNU grep compiles a `-F -f` pattern set into ONE
# Aho-Corasick/Commentz-Walter automaton and scans the text once, while BSD grep
# has no such matcher and degrades toward (patterns x text) — here 950 x 7.2 MB.
# ⚠ So PHASE 1 was a Linux OPTIMISATION that is a macOS PESSIMISATION, and it
# was landed measured on ONE platform. Scanning for the PATTERN instead makes the
# cost (1 x text) on every grep, which is the same shape as the three collection
# greps above that were always fast everywhere.
#
# WHAT CHANGES SEMANTICALLY, STATED PLAINLY: `_plan_hit` used to hold "src
# anchors seen verbatim in the plans" and now holds "anchor-shaped tokens the
# plans contain". PHASE 1b consumes it exactly the same way — `index(planToken,
# srcAnchor)` — so an anchor resolves when the plans cite it, or cite a MORE
# SPECIFIC name containing it, which is the documented contract above and
# unchanged. Anything this pass cannot settle still falls through to PHASE 3,
# which remains the ONLY phase allowed to conclude "missing". That is why the
# reversal is safe rather than merely faster: it can only move work between
# phase 1b and phase 3, never turn a MISSING into a RESOLVED.
{ LC_ALL=C grep -rhoE "${ANCHOR_REGEX}" .plans/ 2>/dev/null || true; } \
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
    if ! LC_ALL=C grep -qrF -- "${src_a}" .plans/ 2>/dev/null; then
        MISSING+=("${src_a}")
    fi
done < "${_plan_cand}"

# ══ RETIRED IDS — a citation must not name an id that has been withdrawn ══════
#
# ★★★ WHY THIS EXISTS, AND IT IS A DEFECT IN *THIS FILE* THAT WAS PROVEN BY THIS
# FILE'S OWN BUG REPORT. Resolution above is deliberately SUBSTRING-anywhere in
# `.plans/` (see the three-phase block: it is what lets a line-wrapped citation
# and a parent-name citation resolve). The cost of that tolerance is that it
# CANNOT DISTINGUISH A LIVE NAME FROM A DEAD ONE — any name mentioned anywhere in
# the plans resolves forever, including in the prose that reports it as dead.
# ✔MEASURED 2026-08-17: `D-ASM-INDIRECT-BRANCH-SUCCESSOR-SET-UNDERIVABLE` was
# cited in `tests/asm/test_asm_text_to_lir.cpp` after a one-word rename to
# `…-UNSTATED`. It resolved — and the ONLY text in `.plans/` supplying that
# resolution was the row that had just been written to REPORT it as stale. The
# act of documenting the dangling citation is what made the guard green about it.
# ⇒ A guard whose pass is manufactured by its own bug report asserts nothing.
#
# ★★ THE MATCHER IS POSITIONAL, NOT A SEARCH, AND IT TOOK TWO FALSE POSITIVES TO
# GET THERE — both worth keeping, because each one is the previous rule's own
# failure mode:
#   (1) Searching the status cell for the words `RETIRED ID` red on
#       `D-GATE-ANCHOR-GUARD-DOES-NOT-SCAN-INTEGRATED-TESTS`, whose prose merely
#       says "as a retired id". Prose is not a marker.
#   (2) Searching it for the unspaced token `RETIRED-ID` then red on
#       `D-GATE-ANCHOR-CITATION-RESOLVES-VIA-ITS-OWN-BUG-REPORT` — the row that
#       DOCUMENTS the token, and therefore contains it. ★★★ The row explaining the
#       rule was classified BY the rule: the same self-reference this whole check
#       exists to kill, one level up, and it appeared within minutes of the fix.
# ⇒ A marker that is merely PRESENT can always be tripped by writing about it. The
# marker must be POSITIONAL: the status cell must OPEN with the retirement, in the
# CLOSED headline itself, where prose never sits. Same rule the anchor-balance
# instrument already uses — a row is CLOSED iff its status cell BEGINS with `✅`,
# defining the complement rather than enumerating glyphs anywhere in the cell.
# ⚠ The regex is byte-anchored under LC_ALL=C, so the `✅` and the em-dash are
# compared as literal bytes; that is intended, not incidental.
#
# ⚠ EQUALITY, NOT SUBSTRING, and deliberately so: a retired id may be a PREFIX of
# a live one, and substring here would red the live citation too. The failure this
# catches is a citation that still spells the dead name exactly.
# ⛔ A retired id must not appear in scanned source AT ALL, narration included. A
# comment that spells a dead id is how the dead id survives — the LK7 case is
# exactly that: a test comment quoted the withdrawn spelling while explaining the
# gap, and the spelling outlived the explanation. Narration cites the LIVE row.
_retired_tmp="$(mktemp)"; _tmps+=("${_retired_tmp}")
LC_ALL=C awk -F'|' '
    /^\| `D-/ {
        key = $2; gsub(/[` ]/, "", key)
        if ($3 ~ /^ *\**✅ \*\*CLOSED [0-9-]+ — RETIRED-ID/) print key
    }
' .plans/_deferred-anchor-registry.md > "${_retired_tmp}"
_retired_count="$(grep -c . "${_retired_tmp}" || true)"
# FAIL-CLOSED on a collapsed extraction, like every other scan here. The registry
# carries retired rows; zero means the marker drifted or the table shape changed,
# never that the check has nothing to do.
if [[ "${_retired_count}" -lt 1 ]]; then
    echo "anchor-registry: FAIL — the retired-id scan found 0 marked rows." >&2
    echo "  This does NOT mean no id is retired — it means THIS scan collapsed (the \`RETIRED-ID\` token was renamed, or the registry's column layout moved)." >&2
    echo "  Refusing to report a pass. Fix the scan; do not delete the check." >&2
    exit 2
fi
RETIRED_CITED=()
while IFS= read -r _rid; do
    [[ -z "${_rid}" ]] && continue
    while IFS= read -r _src_a; do
        [[ "${_src_a}" == "${_rid}" ]] && RETIRED_CITED+=("${_rid}")
    done <<< "${SRC_ANCHORS}"
done < "${_retired_tmp}"

if [[ ${#RETIRED_CITED[@]} -gt 0 ]]; then
    echo "anchor-registry: FAIL — ${#RETIRED_CITED[@]} citation(s) name a RETIRED anchor id:" >&2
    for _rid in "${RETIRED_CITED[@]}"; do
        echo "    ${_rid}" >&2
        LC_ALL=C grep -rn --include='*.cpp' --include='*.hpp' --include='*.json' --include='*.c' \
            --include='*.sh' --include='*.ps1' --include='*.py' \
            -F -- "${_rid}" src/ tests/ integrated_tests/ examples/ real-examples/ 2>/dev/null \
            | sed 's/^/      /' >&2 || true
    done
    echo "  A retired id resolves only because the plans still MENTION it. Repoint each" >&2
    echo "  citation at the live row named in the retired row's own status cell." >&2
    exit 4
fi

if [[ ${#MISSING[@]} -eq 0 ]]; then
    # ${_anchor_count}, not `echo "${SRC_ANCHORS}" | wc -l` — the latter reports 1 for
    # an EMPTY set (echo emits a lone newline), which is exactly how the fail-open bug
    # above dressed a scan of nothing as "OK (1 src anchors all resolve)".
    echo "anchor-registry: OK (${_anchor_count} src anchors all resolve to plans, ${_retired_count} retired id(s) uncited)"
    # ★ The cell-width verdict is NOT allowed to be swallowed by the anchor
    # check's success. Exit codes: 1 = an anchor resolves nowhere, 2 = a scan
    # collapsed, 3 = a markdown table row drops content, 4 = a citation names a
    # retired id.
    exit "${_cw_status}"
fi

echo "anchor-registry: FAIL — the following anchors are cited in a SCANNED ROOT"
echo "(src/, examples/, tests/, integrated_tests/, real-examples/ — NOT src/ alone) but"
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
# ★ DERIVED FROM `_ROOT_SPECS`, never respelled — see the note on that table.
# One grep PER ROOT (not one grep over all roots) because the filters differ per
# root, which is the same reason the collection loop above is per-root.
: > "${_locator_tmp}"
for _spec in "${_ROOT_SPECS[@]}"; do
    _root="${_spec%%|*}"; _rest="${_spec#*|}"; _globs="${_rest#*|}"
    _inc=(); for _g in ${_globs}; do _inc+=(--include="${_g}"); done
    { LC_ALL=C grep -rEo "${ANCHOR_REGEX}" "${_root}/" "${_inc[@]}" 2>/dev/null || true; } \
        >> "${_locator_tmp}"
done
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
