#!/usr/bin/env pwsh
# check-anchor-registry.ps1 — Windows variant of the deferred-anchor
# registry CI guard. Mirrors the bash variant; same contract.
#
# Contract: every `D-*` identifier cited in `src/` MUST resolve to a row
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
    exit 0
}

Write-Host "anchor-registry: FAIL - the following anchors are cited in src/ but"
Write-Host "have no matching row/citation in any .plans/*.md file:"
Write-Host ""
foreach ($a in $missing) {
    Write-Host "  $a"
    # Same scanned set as the collection above — a citation this guard FOUND
    # must also be LOCATABLE here, or the FAIL output names an anchor with no
    # "cited in:" line and the fix is a guessing game.
    $files = (Get-ChildItem -Path 'src', 'examples' -Recurse -File `
                            -Include $AnchorFileFilters  -ErrorAction SilentlyContinue) +
             (Get-ChildItem -Path 'real-examples'  -Recurse -File `
                            -Include $HarnessFileFilters -ErrorAction SilentlyContinue)
    foreach ($f in $files) {
        if (Select-String -LiteralPath $f.FullName -Pattern $a -SimpleMatch -Quiet) {
            Write-Host "    cited in: $($f.FullName.Replace($RepoRoot + [IO.Path]::DirectorySeparatorChar, ''))"
        }
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
exit 1
