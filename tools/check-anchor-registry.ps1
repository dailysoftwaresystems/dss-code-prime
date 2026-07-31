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
$AnchorFileFilters = @('*.cpp', '*.hpp', '*.json', '*.c')
$srcAnchors = (Get-Anchors 'src'      $AnchorFileFilters) +
              (Get-Anchors 'examples' $AnchorFileFilters)
$srcAnchors = $srcAnchors | Sort-Object -Unique

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
    $files = Get-ChildItem -Path 'src', 'examples' -Recurse -File `
                           -Include $AnchorFileFilters -ErrorAction SilentlyContinue
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
