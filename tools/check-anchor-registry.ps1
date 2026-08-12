#!/usr/bin/env pwsh
# check-anchor-registry.ps1 — Windows variant of the deferred-anchor
# registry CI guard. Mirrors the bash variant; same contract.
#
# Contract: every `D-*` identifier cited in a SCANNED ROOT (`src/`, `examples/`,
# `real-examples/` - see the roots table below) MUST resolve to a row
# in `.plans/_deferred-anchor-registry.md` OR a citation in any
# `.plans/*.md` file.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot  = Split-Path -Parent $ScriptDir
Set-Location $RepoRoot

# Anchor regex: word-boundary `\b` before `D-` prevents the regex from
# capturing the substring `D-32-BIT-WORD` out of `FIXED-32-BIT-WORD`
# (an in-comment phrase, not an anchor).
$AnchorRegex = '\bD-[A-Z0-9_]+(-[A-Z0-9_]+){2,}'

function Get-Anchors([string]$Path, [string[]]$Filters) {
    $files = Get-ChildItem -Path $Path -Recurse -File -Include $Filters -ErrorAction SilentlyContinue
    $anchors = @{}
    foreach ($f in $files) {
        $content = Get-Content -Raw -LiteralPath $f.FullName -ErrorAction SilentlyContinue
        if (-not $content) { continue }
        $matchesFound = [regex]::Matches($content, $AnchorRegex)
        foreach ($m in $matchesFound) { $anchors[$m.Value] = $true }
    }
    return $anchors.Keys | Sort-Object
}

# The scanned set MUST match the .sh sibling EXACTLY (`src/ examples/` x
# {cpp,hpp,json,c}). It did not: this script scanned `src` without `*.c` and
# `examples` with ONLY `*.c`, so every anchor cited solely in an
# `examples/**/expected.json` was invisible HERE while the .sh guard saw it —
# measured 2026-07-31 as a reproducible 777 (ps1) vs 781 (sh) disagreement.
# THAT divergence was false-NEGATIVE only (this guard checked strictly fewer
# SOURCE files, so it could never red where .sh greens), which is precisely why
# it survived: the weaker guard is the one the Windows leg runs.
# ⚠ But "this guard is merely the weaker one" is NOT true in general — see
# residual (iii) below, which leans the other way.
# An instance of D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED — the pairing itself is
# unenforced, so the two scripts can drift again silently. Note the registry
# lists this pair as PAIRED, which was true and not sufficient: pairing by
# EXISTENCE is not pairing by BEHAVIOUR.
#
# TWO KNOWN DIVERGENCES SURVIVE this fix. Neither bites today; both are
# recorded so the next reader does not have to re-derive them:
#   * `Get-ChildItem -Recurse` skips hidden files and directories unless
#     `-Force` is passed, whereas `grep -r` descends into them. Harmless only
#     while no source lives under a dotted path in `src/` or `examples/`.
#   * The `.sh` FAIL path matches with `grep -rln` (a REGEX) while this one
#     uses `-SimpleMatch` (a literal). Harmless only while anchor names carry
#     no regex metacharacters — which the `D-[A-Z0-9_-]+` shape guarantees
#     today, but nothing enforces.
#   * (iii) THE PLAN-SIDE RESOLVE DIVERGES THE OTHER WAY, so this guard is not
#     uniformly the weaker one: `.sh` resolves an anchor with
#     `grep -qrF -- "$a" .plans/` over EVERY file under `.plans/`, whereas this
#     script reads only `.plans/**/*.md`. That makes THIS guard STRICTER — a
#     potential false-POSITIVE (an anchor cited only in a non-`.md` plan file
#     would red here and green there). Inert today ONLY because `.plans/`
#     currently contains zero non-`.md` files; nothing enforces that.
# ── real-examples/ ADDED 2026-08-03 (TF-C111), D-HARNESS-ANCHOR-GUARD-SKIPS-HARNESS-DRIVERS.
# The guard covered `src/ examples/` only, so every `D-*` cited in a HARNESS
# DRIVER resolved to nothing and failed nothing. That is not hypothetical: the
# name `D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE` was carried in two
# hand-off documents AS THOUGH TRACKED and had, measured, ZERO hits repo-wide
# and no registry row — invisible to this guard, to the plan sweep, and to
# every cycle's orient step. An unenforced citation is a citation that rots.
# The drivers are SCRIPTS, so they need their own filter set — reusing
# $AnchorFileFilters would have scanned real-examples/ for *.cpp and found
# nothing, which is the silent no-op version of this fix.
# ⚠ Dry-run before widening further. A future root that reds must be closed by
# REGISTERING the rows, never by narrowing the guard.
# ★ CORRECTION 2026-08-03 (TF-C112): the TF-C111 note here (and its `.sh` twin) said
# "measured at 22 anchors cited across the drivers". With the filter set that actually
# SHIPPED the figure is **24** — `*.sh` + `*.ps1` alone is exactly 22, so the dry-run
# that justified the change was run WITHOUT the `*.py` filter that landed with it. All
# 24 resolve, so nothing was ever broken by it; the defect is that a MEASURED claim was
# stated twice and was off by the one filter the same commit added. Recorded rather
# than quietly corrected, because "I measured it" is exactly the kind of claim this
# project requires to be true.
#
# ════════════════════════════════════════════════════════════════════════════
# CHECK 2 of 2 — MARKDOWN TABLE CELL-WIDTH PROPERTY
# TWIN of the block of the same name in `check-anchor-registry.sh`. Closes
# `D-PLANS-REGISTRY-ROWS-WITH-EXTRA-CELLS-STILL-LIVE`.
#
# ★★ THE RATIONALE IS RESTATED HERE IN FULL, not delegated to the `.sh` with a
# "see the other file". A twin that only points at its sibling is how the pair
# drifts — and `D-GATE-SCRIPT-PS1-CONTENT-DRIFT-UNCHECKED` is open precisely
# because these two have drifted before.
#
# THE PROPERTY: **in every markdown table under `.plans/`, no data row may
# carry MORE cells than its own table's header.** A row with surplus cells has
# the overflow SILENTLY DROPPED by the renderer — the text stays in the file
# and vanishes from the page. Invisible from both sides: nothing in the raw
# text shows it, and nothing in a diff shows it.
#
# ★★ WHY A PROPERTY AND NOT A LIST. The registry row named above existed for
# this exact defect and did not stop it, because it *named three instances*
# instead of asserting the property, and scoped itself to one table of one
# file — repeating the mistake its own text diagnoses. While it was open, 19
# more rows accumulated (15 in the registry, 4 in plan-00, up to 39,403
# characters dropped in a single row). An instance list cannot fail; only a
# property can.
#
# ★★ TWO SUB-SHAPES, and a matcher tuned to one MISSES the other — so this is
# SHAPE-BLIND: it counts separators and never guesses intent.
#   (a) ALTERNATION pipes — `{block|EndStatement}`, `RECIPE|MAKE-N|DERIV`,
#       `MINGW*|MSYS*|CYGWIN*`, a shell pipeline `ls … | grep …`, `||`;
#   (b) ABSOLUTE-VALUE bars — `1,108 steps >|0.1 s|` — not an alternation at
#       all, and invisible to any "looks like a regex" heuristic.
#
# ⚠ NOT a violation: FEWER cells than the header. The renderer pads the
# trailing columns and loses nothing (the registry says so in its own words).
# 333 rows are in that state; failing them would enforce a PROXY (uniform
# width) rather than the PROPERTY (nothing invisible). Counted, never fatal.
#
# ⚠ Unescaped pipes only: `\|` is content. Implemented by protecting `\|` with
# a placeholder and splitting on `|` — the `(?<!\\)\|` rule without needing a
# lookbehind, which the `.sh` twin's POSIX awk does not have. NOT done with
# grep: `grep -o '\|'` counts BRE alternation (two false alarms in this
# project's history) and `grep -P` is absent from Git Bash, where it exits 2.
#
# ★ Every message this check emits is ASCII-ONLY, deliberately: the twins are
# verified by DIFFING their output, and a console-encoding difference between
# pwsh and Git Bash must never be able to fake a disagreement.
# ★ Two divergences the anchor half above documents are deliberately CLOSED
# here rather than re-documented: `-Force` makes the walk descend hidden paths
# like `find` does, and the case-SENSITIVE `-ceq '.md'` filter matches
# `find -name '*.md'` instead of also taking `README.MD`.
$CellWidthRoot       = '.plans'
# Floors far below the live figures so ordinary churn never trips them. These
# catch a COLLAPSED scan, not drift. Both twins MUST carry the same values.
# ★ NO LIVE COUNT IS QUOTED HERE, ON PURPOSE, AND THAT IS A CORRECTION: the row
# total grows every time anyone appends a table row, so a figure written into
# this comment is stale by the next cycle. ✔MEASURED 2026-08-10 — it had already
# drifted into THREE disagreeing values (two comments saying 3,097, a registry
# row saying 3,098) against a live 3,099, i.e. every copy was wrong and none of
# them could be trusted to say so. The single owner of the live figures is THIS
# SCRIPT'S OWN OUTPUT, which prints files / tables / rows on every run; read it
# there. Point-in-time readings belong in the registry's audit trail, where a
# date makes them history rather than a claim about now.
$CellWidthFileFloor  = 20
$CellWidthTableFloor = 100
$CellWidthRowFloor   = 1500
# ★★★ THERE IS NO EXCEPTION LIST, AND THERE MUST NEVER BE ONE AGAIN.
# This check briefly shipped a `$CellWidthQuarantine` ratchet — 17 known-bad
# rows across 9 plan files, scanned and counted but with their count PINNED so
# the run stayed green. It was defended as "a ratchet, not an exemption", and
# the distinction did not survive contact: a quarantine is the instance list
# this check exists to replace, wearing a config entry. All 17 rows were
# REPAIRED — 26 stray content pipes escaped to `\|` across 12 rows, and 6
# genuinely EXTRA cells joined with the ` ═══ ` seam across 5 more; every file
# verified by BYTE ARITHMETIC, (seams x 8) + (escapes x 1) + (seam-pad spaces
# x 1) = +75 bytes over the nine files, with line counts unchanged — and the
# mechanism was DELETED from BOTH twins in the same commit, so this check now
# asserts the property with ZERO exceptions.
# ⚠ If it ever fires on an honestly-earned change, fix the ROW. Do not
# reintroduce a pin, and do not narrow the property — a guard that is weakened
# every time it fires ends up asserting nothing.

# awk's trim removes SPACE and TAB only — not every Unicode space, which is
# what a bare .Trim() would do. These files are full of non-ASCII whitespace-
# adjacent characters, so the distinction is load-bearing for agreement.
function CwTrim([string]$s) { return $s.Trim([char]32, [char]9) }
$CwPlaceholder = [string][char]1

# Split a row on UNESCAPED pipes. Returns the cell count plus the cell array
# and the live [Lo..Hi] window, mirroring the awk twin's CELLS/CELL_LO/CELL_HI.
function CwCells([string]$line) {
    $t = $line -replace '\\\|', $CwPlaceholder
    $parts = $t -split '\|'
    $lo = 0
    $hi = $parts.Count - 1
    if ((CwTrim $parts[$lo]) -eq '') { $lo++ }
    if ($hi -ge $lo -and (CwTrim $parts[$hi]) -eq '') { $hi-- }
    $n = 0
    if ($hi -ge $lo) { $n = $hi - $lo + 1 }
    return [pscustomobject]@{ N = $n; Lo = $lo; Hi = $hi; Parts = $parts }
}

# ASCII-only, single-line, bounded. Stripping non-ASCII BEFORE truncating is
# what makes the twins agree byte-wise: removing every non-ASCII BYTE and
# removing every non-ASCII CHARACTER yield the same string, after which the
# truncation is byte- and char-identical. Truncating first would split a UTF-8
# sequence in a byte-based awk and not in a char-based one.
function CwDisplay([string]$s) {
    $t = $s -replace $CwPlaceholder, '|'
    $t = $t -replace '[^ -~]', ''
    $t = CwTrim $t
    if ($t.Length -gt 110) { return $t.Substring(0, 110) }
    return $t
}

# Kept SEPARATE rather than assigning into one status. With a single variable,
# whichever branch ran LAST would own the exit code, so a COLLAPSED scan could
# be reported as a content-drop and send the reader to the wrong file. Both are
# red either way; the PRECEDENCE (collapse 2 > content-dropped 3) is decided
# once, at the single `exit` below.
$cwCollapsed = 0; $cwViolated = 0
if (-not (Test-Path -LiteralPath $CellWidthRoot -PathType Container)) {
    Write-Host "anchor-registry: FAIL - cell-width root '$CellWidthRoot' does not exist. Refusing to report a pass on a scan of nothing."
    $cwCollapsed = 1
} else {
    # ORDINAL sort, to match the .sh's `LC_ALL=C sort`. Culture-aware ordering
    # would place `_deferred-…` differently and make the twins disagree for no
    # real reason.
    $cwFiles = [string[]]@(
        Get-ChildItem -LiteralPath $CellWidthRoot -Recurse -File -Force |
            Where-Object { $_.Extension -ceq '.md' } |
            ForEach-Object { $_.FullName.Substring($RepoRoot.Length + 1).Replace([IO.Path]::DirectorySeparatorChar, '/') }
    )
    [Array]::Sort($cwFiles, [StringComparer]::Ordinal)

    $cwTables = 0; $cwRows = 0; $cwOver = 0; $cwUnder = 0
    $cwOffenders = [System.Collections.ArrayList]::new()
    foreach ($rel in $cwFiles) {
        $raw = [IO.File]::ReadAllText((Join-Path $RepoRoot $rel), [Text.Encoding]::UTF8)
        $lines = $raw -split "`n"
        $inFence = $false; $hdr = -1; $hdrLine = 0; $skipNext = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $line = $lines[$i].TrimEnd([char]13)
            $s = CwTrim $line
            if ($s.StartsWith('```') -or $s.StartsWith('~~~')) { $inFence = -not $inFence; $hdr = -1; continue }
            if ($inFence) { continue }
            if (-not $line.Contains('|')) { $hdr = -1; continue }
            if ($skipNext) { $skipNext = $false; continue }
            $nxt = ''
            if ($i + 1 -lt $lines.Count) { $nxt = $lines[$i + 1].TrimEnd([char]13) }
            $ns = CwTrim $nxt
            # A table STARTS where a row is followed by a delimiter row. The
            # width comes from the table's OWN header: these files carry 2-,
            # 3-, 4-, 5- and 6-cell tables, so any global constant would be
            # wrong somewhere — and a check that is wrong somewhere trains
            # people to ignore it.
            if ($nxt.Contains('|') -and $ns -match '^\|?[ ]*:?-+:?[ ]*(\|[ ]*:?-+:?[ ]*)*\|?$') {
                $hdr = (CwCells $line).N; $hdrLine = $i + 1; $cwTables++; $skipNext = $true; continue
            }
            if ($hdr -lt 0) { continue }
            $cwRows++
            $cc = CwCells $line
            if ($cc.N -lt $hdr) { $cwUnder++; continue }
            if ($cc.N -eq $hdr) { continue }
            $cwOver++
            $ex = ''
            for ($k = $cc.Lo + $hdr; $k -le $cc.Hi; $k++) {
                if ($ex -ne '') { $ex += ' // ' }
                $ex += (CwTrim $cc.Parts[$k])
            }
            [void]$cwOffenders.Add([pscustomobject]@{
                F = $rel; L = $i + 1; H = $hdrLine; E = $hdr; A = $cc.N; X = (CwDisplay $ex) })
        }
    }

    if ($cwFiles.Count -lt $CellWidthFileFloor) {
        Write-Host "anchor-registry: FAIL - cell-width scan found only $($cwFiles.Count) markdown files under $CellWidthRoot, below its floor of $CellWidthFileFloor."
        Write-Host "  This does NOT mean the plans are clean - it means the SCAN COLLAPSED. Refusing to report a pass; fix the scan, do not lower the floor."
        $cwCollapsed = 1
    }
    if ($cwTables -lt $CellWidthTableFloor) {
        Write-Host "anchor-registry: FAIL - cell-width scan found only $cwTables tables, below its floor of $CellWidthTableFloor."
        Write-Host "  A collapsed table scan checks nothing. Refusing to report a pass; fix the scan, do not lower the floor."
        $cwCollapsed = 1
    }
    if ($cwRows -lt $CellWidthRowFloor) {
        Write-Host "anchor-registry: FAIL - cell-width scan found only $cwRows table data rows, below its floor of $CellWidthRowFloor."
        Write-Host "  A collapsed row scan checks nothing. Refusing to report a pass; fix the scan, do not lower the floor."
        $cwCollapsed = 1
    }
    # ★★ THE VIOLATION COUNT IS COUNTED, NEVER DERIVED. `$cwOver` is incremented
    # at exactly ONE place — the moment a row is found wider than its header —
    # and that SAME variable drives the FAIL headline and the summary line, while
    # the detail loop walks the offender list it was incremented alongside. There
    # is no second source of truth for it to disagree with, and nothing is
    # subtracted from it.
    # MEASURED 2026-08-10, by exercising the failure arm rather than reading it:
    # while a quarantine ratchet existed here, the live count was DERIVED as
    # `scanned - pinned`, which printed "-1 live violation(s)" the moment a
    # pinned row was repaired, and - far worse - a repair in a pinned file plus a
    # NEW break in a clean one CANCELLED to "0 live" while the run was failing. A
    # guard whose own summary can read 0 during a failure is the "printed
    # failed=0 and exited 2" shape all over again. The ratchet is gone; this note
    # stays so the subtraction is never reintroduced with it.
    if ($cwOver -gt 0) {
        Write-Host "anchor-registry: FAIL - $cwOver markdown table row(s) carry MORE cells than their table header."
        Write-Host "The surplus is SILENTLY DROPPED by the renderer: the text stays in the file and"
        Write-Host "vanishes from the page. This is invisible from both sides - nothing in the raw"
        Write-Host "text shows it, and nothing in the diff shows it."
        Write-Host ""
        foreach ($o in $cwOffenders) {
            Write-Host "  $($o.F):$($o.L)  header@$($o.H) expected=$($o.E) actual=$($o.A)"
            Write-Host "      dropped: $($o.X)"
        }
        Write-Host ""
        Write-Host "Fix: the surplus is CONTENT that was read as a column boundary. Escape a content"
        Write-Host "pipe as \| (the only form that survives inside a code span); join a genuinely"
        Write-Host "extra cell with the registry seam marker instead of a column boundary. Never"
        Write-Host "delete text to make the count fit, and never widen the check."
        $cwViolated = 1
    }
    Write-Host "anchor-registry: cell-width $cwTables tables / $cwRows rows in $($cwFiles.Count) files: $cwOver violation(s), $cwUnder short rows (padded, no loss)."
}

# ★★ FAIL-CLOSED, ADDED 2026-08-03 (TF-C112), D-GATE-ANCHOR-GUARD-FAILS-OPEN-ON-MISSING-ROOT.
# Mirrors the `.sh` sibling: assert every root EXISTS, then apply a count FLOOR.
# The `.sh` form could report `OK (1 src anchors all resolve to plans)` while scanning
# NOTHING; this script's `Get-ChildItem -ErrorAction SilentlyContinue` has the same
# shape — a renamed root yields an empty set and a silently smaller scan. A guard that
# reports success while checking nothing is the worst defect a guard can have.
foreach ($_root in @('src', 'examples', 'real-examples')) {
    if (-not (Test-Path -LiteralPath $_root -PathType Container)) {
        Write-Host "anchor-registry: FAIL - scan root '$_root' does not exist. A missing root would silently shrink coverage; refusing to report a partial scan as a pass."
        exit 2
    }
}
$AnchorFileFilters     = @('*.cpp', '*.hpp', '*.json', '*.c')
$HarnessFileFilters    = @('*.sh', '*.ps1', '*.py')
$srcAnchors = (Get-Anchors 'src'           $AnchorFileFilters) +
              (Get-Anchors 'examples'      $AnchorFileFilters) +
              (Get-Anchors 'real-examples' $HarnessFileFilters)
$srcAnchors = $srcAnchors | Sort-Object -Unique

# The floor. Same reasoning as the `.sh`: "no match" is not distinguishable from "scan
# collapsed" by status alone, so a COUNT is the only signal that separates them. Kept
# far below the live count (800 at time of writing) so ordinary churn never trips it —
# this catches collapse, not drift. Both scripts MUST use the same value or the pairing
# is decorative.
$AnchorFloor = 100
if ($srcAnchors.Count -lt $AnchorFloor) {
    Write-Host "anchor-registry: FAIL - only $($srcAnchors.Count) anchors found across src/ examples/ real-examples/, below the floor of $AnchorFloor."
    Write-Host "  This does NOT mean the tree is clean - it means the SCAN COLLAPSED (an unreadable root, a broken regex, or a filter that matches nothing)."
    Write-Host "  Refusing to report a pass. Fix the scan; do not lower the floor."
    exit 2
}

# ⚠ KNOWN PAIRING DIVERGENCES, extension-case half — documented 2026-08-03 (TF-C112),
# adding to the three already listed above. `Get-ChildItem -Include '*.sh'` matches
# `RUN.SH`; `grep --include='*.sh'` does NOT. So a driver with an uppercase extension
# would be scanned on Windows and INVISIBLE on the Linux leg. Likewise the FAIL-path
# locator: the `.sh` uses `grep -rln` (case-SENSITIVE) while this script uses
# `Select-String -SimpleMatch`, which is case-INSENSITIVE by default — the
# regex-vs-literal half of that divergence was already documented, the CASE half was
# not. Both are inert today (zero uppercase-extension files in the scanned roots) and
# are left as documented divergences rather than "fixed", because forcing either side
# to the other's casing rules would change which files are scanned on one leg only —
# which is the very asymmetry being guarded against. If an uppercase-extension driver
# ever lands, close this by making BOTH sides case-sensitive, not by matching Windows.

# Read every plan-file's raw content for substring matching. Substring
# (vs equality vs extracted-anchor-set) handles two false-positive modes:
#   (1) Multi-line citation in src: a comment wraps the anchor name
#       across a newline — the regex captures only the prefix.
#   (2) Plans use a more specific anchor name (e.g.
#       `D-LK6-14-INTEGRATION-GOT-SLOTS`) but src cites the parent
#       (`D-LK6-14-INTEGRATION`) — both are "known" via the same row.
$planFiles = Get-ChildItem -Path '.plans' -Recurse -File -Include '*.md'
$allPlanText = ($planFiles | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"

$missing = @()
foreach ($a in $srcAnchors) {
    if (-not $allPlanText.Contains($a)) { $missing += $a }
}

if ($missing.Count -eq 0) {
    Write-Host "anchor-registry: OK ($($srcAnchors.Count) src anchors all resolve to plans)"
    # ★ The cell-width verdict is NOT allowed to be swallowed by the anchor
    # check's success. Exit codes, matching the .sh: 1 = an anchor resolves
    # nowhere, 2 = a scan collapsed, 3 = a markdown table row drops content.
    exit $(if ($cwCollapsed) { 2 } elseif ($cwViolated) { 3 } else { 0 })
}

Write-Host "anchor-registry: FAIL - the following anchors are cited in a SCANNED ROOT"
Write-Host "(src/, examples/, real-examples/ - NOT src/ alone) but"
Write-Host "have no matching row/citation in any .plans/*.md file:"
Write-Host ""
# ★★ THE TREE IS WALKED ONCE — NOT ONCE PER MISSING ANCHOR.
# MEASURED 2026-08-10 on the live tree: the previous form re-enumerated
# `src/ examples/ real-examples/` with `Get-ChildItem -Recurse` and then ran a
# `Select-String` per FILE, all INSIDE the per-anchor loop — cost
# (missing anchors) x (whole tree). With 8 missing anchors this FAIL path took
# **82.9 s** (the `.sh` twin, same 8, took 18.1 s); a wholesale break strands
# every anchor and the guard simply looks HUNG at the moment its output matters
# most. Now the enumeration and the file reads happen ONCE into an ordered
# (file, anchor-token) index, and each anchor's lookup is an in-memory test with
# no I/O. Behaviour is UNCHANGED, verified by DIFFING the whole FAIL report
# before and after on the identical mutation.
# ★ Two behaviours of the old form are deliberately PRESERVED rather than
# quietly tightened, because the task was cost, not semantics:
#   · `Select-String -SimpleMatch` is case-INSENSITIVE by default (a documented
#     divergence from the `.sh`'s case-SENSITIVE `grep`), so the index is built
#     with `IgnoreCase` and probed with `OrdinalIgnoreCase`;
#   · attribution is by SUBSTRING, so a file citing a MORE SPECIFIC anchor is
#     still listed under its PREFIX — a relationship the plan-side resolve
#     documents and relies on, so dropping it would silently shrink the report.
# ★ Paths are recorded in ENUMERATION order and replayed in that order, so the
# report's line order is identical to the old nested loop's.
$locFiles = @(Get-ChildItem -Path 'src', 'examples' -Recurse -File `
                            -Include $AnchorFileFilters  -ErrorAction SilentlyContinue) +
            @(Get-ChildItem -Path 'real-examples'  -Recurse -File `
                            -Include $HarnessFileFilters -ErrorAction SilentlyContinue)
$locRx     = [regex]::new($AnchorRegex, [Text.RegularExpressions.RegexOptions]::IgnoreCase)
$locTokens = [System.Collections.ArrayList]::new()   # distinct tokens, first-seen order
$locPaths  = @{}                                     # token -> ordered path list
foreach ($f in $locFiles) {
    $c = Get-Content -Raw -LiteralPath $f.FullName -ErrorAction SilentlyContinue
    if (-not $c) { continue }
    $rel = $f.FullName.Replace($RepoRoot + [IO.Path]::DirectorySeparatorChar, '')
    $seenHere = @{}
    foreach ($m in $locRx.Matches($c)) {
        $t = $m.Value
        if ($seenHere.ContainsKey($t)) { continue }
        $seenHere[$t] = $true
        if (-not $locPaths.ContainsKey($t)) {
            [void]$locTokens.Add($t)
            $locPaths[$t] = [System.Collections.ArrayList]::new()
        }
        [void]$locPaths[$t].Add($rel)
    }
}
# ORDER: the old form walked FILES and printed the matching ones, so the report
# is ordered by enumeration position. Replay that by ranking each path.
$locRank = @{}
$locOrder = 0
foreach ($f in $locFiles) {
    $rel = $f.FullName.Replace($RepoRoot + [IO.Path]::DirectorySeparatorChar, '')
    if (-not $locRank.ContainsKey($rel)) { $locRank[$rel] = $locOrder++ }
}
# An index that came back EMPTY means the locator can attribute NOTHING, and the
# report would then be a list of bare anchor names with no `cited in:` line -
# precisely the guessing game the note above says this locator exists to prevent.
# It should be unreachable (the anchor floor above already proved these same
# filters match hundreds of anchors), so say so LOUDLY rather than degrade
# quietly if it ever happens. Mirrors the .sh sibling.
if ($locTokens.Count -eq 0) {
    Write-Host "anchor-registry: WARNING - the citation index is EMPTY, so no 'cited in:' line can be produced."
    Write-Host "  The anchor floor above passed, so this should be impossible; the locator's file"
    Write-Host "  enumeration (same roots, same -Include set) must have stopped matching. Fix the scan."
}
foreach ($a in $missing) {
    Write-Host "  $a"
    $hits = @{}
    foreach ($t in $locTokens) {
        if ($t.IndexOf($a, [StringComparison]::OrdinalIgnoreCase) -lt 0) { continue }
        foreach ($p in $locPaths[$t]) { $hits[$p] = $locRank[$p] }
    }
    foreach ($p in ($hits.GetEnumerator() | Sort-Object Value | ForEach-Object { $_.Key })) {
        Write-Host "    cited in: $p"
    }
}
Write-Host ""
Write-Host "Fix: either"
Write-Host "  (a) add a row in .plans/_deferred-anchor-registry.md naming the"
Write-Host "      trigger + closing work, OR"
Write-Host "  (b) cite the anchor in a per-plan section 3.1 row (preferred when"
Write-Host "      the anchor maps to a specific plan's feature area), OR"
Write-Host "  (c) if the string is a code-internal pin not deferred work, add"
Write-Host "      it to the Allowlist section of the registry."
Write-Host ""
Write-Host "Discipline: this leak recurred TWICE before this guard landed."
Write-Host "See .plans/_deferred-anchor-registry.md for the discipline rationale."
# ★ BOTH halves report on every run. But an exit code can carry only ONE number,
# so when both fail the precedence is DELIBERATE rather than whichever branch
# happens to run last: a COLLAPSED scan (2) beats a missing anchor (1) beats
# dropped content (3), because a scan that checked nothing makes the other two
# verdicts untrustworthy. MEASURED 2026-08-10: with 20 plan files removed the
# cell-width floors fire AND every anchor stops resolving, and without this the
# `exit 1` below silently outranked the collapse. Mirrors the .sh exactly.
if ($cwCollapsed) { exit 2 }
exit 1
