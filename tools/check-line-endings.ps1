#!/usr/bin/env pwsh
# check-line-endings.ps1 — Windows variant of the tracked-blob line-ending guard.
# Mirrors `check-line-endings.sh`; same contract, same floor, same exit codes.
#
# Contract: NO tracked TEXT blob in this repository may contain a line-terminating
# CR — not in HEAD, and not staged in the index. A file that genuinely needs its
# 0x0D bytes preserved declares itself `binary` in `.gitattributes`, which this
# guard honours through `git grep -I`.
#
# ★ PAIRING NOTE (D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED). The anchor-registry pair
# drifted because each script REIMPLEMENTED the scan in its own shell's idioms —
# one `grep -r`, one `Get-ChildItem -Recurse` — and the two disagreed by 4
# anchors for months. This pair deliberately does NOT do that: every measurement
# below is made by the SAME `git` subcommands with the SAME arguments as the .sh
# sibling, so the shells only marshal arguments and compare strings. That does
# not make the pairing enforced — nothing checks it — but it removes the
# mechanism by which the other pair diverged.
#
# ⚠ INSTRUMENT NOTE specific to THIS shell: `Set-StrictMode` + a native command
# exiting non-zero is a normal, expected outcome here (`git grep` exits 1 for
# "no match", which is this guard's PASS). PowerShell 7.4 turns native stderr
# into a terminating error when `$PSNativeCommandUseErrorActionPreference` is
# on, so it is explicitly disabled below — otherwise the clean case would throw.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot  = Split-Path -Parent $ScriptDir
Set-Location $RepoRoot

# ── fail-closed preconditions ─────────────────────────────────────────────
# A guard that cannot run must FAIL, never skip.
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Error "line-endings: FAIL - git is not on PATH. This guard reads BLOBS, so it cannot fall back to the working tree (a CRLF checkout would false-red and an LF checkout would false-green). Refusing to report a pass."
    exit 2
}
$null = git rev-parse --verify --quiet HEAD 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Error "line-endings: FAIL - HEAD does not resolve; this is not a git work tree with a commit. Refusing to report a pass over a tree it cannot read."
    exit 2
}

function Get-GitLines([string[]]$GitArgs) {
    # `git grep` exits 1 for "no match", which is a legitimate answer here, so
    # only the LINES are returned; callers that need to distinguish "empty"
    # from "broken" use the positive control below, never an exit code.
    $out = & git @GitArgs 2>$null
    if ($null -eq $out) { return @() }
    return @($out | Where-Object { $_ -ne '' })
}

# ── POSITIVE CONTROL ──────────────────────────────────────────────────────
# The offender scan PASSES by returning nothing, which is exactly what a BROKEN
# scan also returns. Prove the instrument answers first: same engine, same ref,
# a pattern every non-empty text blob matches. MEASURED at the commit that added
# this: 2214 blobs in HEAD, 2214 in the index, over 2238 tracked paths. The
# floor catches COLLAPSE, not drift. Fix the scan; never lower the floor.
$ScanFloor    = 1500
$ControlHead  = (Get-GitLines @('grep','-I','-l','-P','^.','HEAD')).Count
$ControlIndex = (Get-GitLines @('grep','--cached','-I','-l','-P','^.')).Count

$controlFailed = $false
foreach ($pair in @(@('HEAD', $ControlHead), @('index', $ControlIndex))) {
    if ($pair[1] -lt $ScanFloor) {
        Write-Host "line-endings: FAIL - the $($pair[0]) scan saw only $($pair[1]) text blobs, below its floor of $ScanFloor."
        Write-Host "  This does NOT mean the tree is clean - it means the SCAN collapsed"
        Write-Host "  (git built without PCRE for -P, an unresolvable ref, or a moved tree)."
        Write-Host "  A guard that reports success over what it could not read is the exact"
        Write-Host "  failure class this repository keeps anchoring. Refusing to pass."
        $controlFailed = $true
    }
}
if ($controlFailed) { exit 2 }

# ── the check ─────────────────────────────────────────────────────────────
# `-I` skips blobs git detects as BINARY, which is correct (a `.bin` #embed
# fixture legitimately carries 0x0D) but leaves one blind spot: a TEXT source
# git happens to detect as binary would be skipped silently. Check C closes
# exactly that hole for the extensions the pin claims to cover.
#
# ★ THERE IS DELIBERATELY NO `eol=crlf` ESCAPE HATCH, and the reason is a
# MEASURED property of git rather than a policy choice. Probed 2026-08-06: a
# file declared `text eol=crlf`, written CRLF on disk and staged, lands in the
# index as `i/lf` with ZERO CR in the blob — `eol=` governs the SMUDGE
# (checkout) direction only. The REAL exemption is `binary` / `-text`, already
# honoured through `-I`. Kept identical to the .sh sibling.
$OffendersHead  = Get-GitLines @('grep','-I','-l','-P','\r$','HEAD') |
                  ForEach-Object { $_ -replace '^HEAD:', '' }
$OffendersIndex = Get-GitLines @('grep','--cached','-I','-l','-P','\r$')

# ── STALE-CHECKOUT DETECTION — kept identical in intent to the .sh sibling ──
#
# ★ MEASURED 2026-08-06 (TF-C123): the WSL leg of the 3-leg gate rsyncs the tree
#   with `--exclude '.git/'` (D-GATE-WSL-SYNC-LEAVES-GIT-HEAD-STALE), so HEAD and
#   the index answer about a DIFFERENT COMMIT than the files on disk, and this
#   guard convicted ten pre-normalisation blobs that were all LF on disk.
#   The detection is EXACT, not heuristic: a file the history calls CRLF while
#   carrying ZERO CR on disk proves the recorded history is not this tree's.
#   A genuine violation has CR in BOTH.
# ⚠ Present here even though this driver runs on the AUTHORITATIVE checkout in
#   practice: a capability in one sibling and not the other is the exact defect
#   class this repo keeps re-finding, and "it cannot happen on my host" is how
#   the last four instances survived.
$SkipHistoryScan = $false
foreach ($f in (@($OffendersHead) + @($OffendersIndex) | Sort-Object -Unique)) {
    if ([string]::IsNullOrEmpty($f) -or -not (Test-Path -LiteralPath $f)) { continue }
    $bytes = [IO.File]::ReadAllBytes($f)
    if (($bytes | Where-Object { $_ -eq 13 } | Select-Object -First 1) -eq $null) {
        Write-Host "line-endings: HISTORY SCAN SKIPPED — this work tree's .git does not describe its files."
        Write-Host "    evidence: '$f' is recorded as CRLF in HEAD/index but carries ZERO CR on disk."
        Write-Host "    Convicting on that history would report violations belonging to another commit,"
        Write-Host "    so checks A and C are suspended. Run this guard on the AUTHORITATIVE checkout."
        $SkipHistoryScan = $true
        break
    }
}
if ($SkipHistoryScan) { $OffendersHead = @(); $OffendersIndex = @() }

$report = New-Object System.Collections.Generic.List[string]
function Add-Offenders([string]$What, $Files) {
    foreach ($f in @($Files)) {
        if ([string]::IsNullOrEmpty($f)) { continue }
        $report.Add("  ${What}: $f")
    }
}
Add-Offenders 'committed (HEAD)' $OffendersHead
Add-Offenders 'staged (index)'   $OffendersIndex

# ── Check C: the pin's own glob set must contain no binary-detected blob ──
# Suspended with checks A/C on a stale checkout: `i/-text` is an INDEX fact.
$eolRows = if ($SkipHistoryScan) { @() } else {
    Get-GitLines @('ls-files','--eol','--','*.c','*.h','*.cpp','*.hpp',
                   '*.cmake','CMakeLists.txt','*/CMakeLists.txt','*.md','VERSION')
}
foreach ($row in $eolRows) {
    if ($row -match '^i/-text') {
        $path = ($row -split "`t", 2)[-1]
        $report.Add("  binary-detected inside the eol=lf pin (invisible to the scan above): $path")
    }
}

# ── Check D: a PINNED file's WORKING TREE must not have been rewritten ────
# ★ THE PIN'S OWN BLIND SPOT. `.gitattributes` normalises on `git add`, so once
# a file is pinned `eol=lf`, a tool that rewrites it CRLF ON DISK changes every
# byte while changing nothing git will show as a change (measured instance:
# Python `pathlib.write_text` over 159 fixtures). MEASURED exactly: `git status`
# DOES report ` M <path>`, but `git diff`, `git diff --stat` and (after
# `git add`) `git diff --cached` are ALL empty — so the file reads as modified
# with no reviewable content, and the rewrite can never even be committed.
# Checks A-C cannot see it BY CONSTRUCTION: the blob is fine; the tree is not.
# Closed set (`w/crlf`, `w/mixed`) on purpose — an unmeasured `w/` state must
# not be guessed at. Kept identical to the .sh sibling.
foreach ($row in (Get-GitLines @('ls-files','--eol'))) {
    $parts = $row -split "`t", 2
    if ($parts.Count -lt 2) { continue }
    $attrs = $parts[0]
    if ($attrs -notmatch 'eol=lf') { continue }
    if ($attrs -match '(^|\s)w/(crlf|mixed)(\s|$)') {
        $report.Add("  worktree rewritten to CRLF under an eol=lf pin (git diff shows NOTHING to review): $($parts[1])")
    }
}

if ($report.Count -eq 0) {
    if ($SkipHistoryScan) {
        # Never let the summary outrun the evidence: say what was NOT judged.
        Write-Host "line-endings: OK (WORKTREE ONLY - the history scan was SKIPPED, see above; the $ControlHead HEAD / $ControlIndex index blobs were NOT judged)"
    } else {
        Write-Host "line-endings: OK ($ControlHead committed + $ControlIndex staged text blobs, none carries CR)"
    }
    exit 0
}

Write-Host "line-endings: FAIL - tracked blobs violate the LF contract:"
Write-Host ""
foreach ($line in $report) { Write-Host $line }
Write-Host ""
Write-Host "Fix:"
Write-Host "  (a) convert the file to LF and commit that rewrite ON ITS OWN, never"
Write-Host "      beside real changes; a whole-file EOL diff sitting next to logic is"
Write-Host "      unreviewable, which is how the original six got in unnoticed; OR"
Write-Host "  (b) add an ``eol=lf`` pin for its extension in ``.gitattributes`` so a"
Write-Host "      tool's platform default can never decide this repo's bytes again"
Write-Host "      (``pathlib.write_text`` on Windows is the measured culprit); OR"
Write-Host "  (c) if the file genuinely REQUIRES its 0x0D bytes preserved, declare it"
Write-Host "      ``binary`` in ``.gitattributes`` (the ``examples/**/*.bin`` precedent)."
Write-Host "      Do NOT reach for ``eol=crlf``: MEASURED - a ``text eol=crlf`` file"
Write-Host "      still stages as an LF blob, so it cannot make this red go away."
Write-Host ""
Write-Host "See D-REPO-GITATTRIBUTES-PINS-EOL-FOR-CONFIGS-BUT-NOT-FOR-SOURCES in"
Write-Host ".plans/_deferred-anchor-registry.md for why this is machine-checked."
exit 1
