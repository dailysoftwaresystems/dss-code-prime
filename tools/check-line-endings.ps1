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

# ── ONE `git ls-files --eol` read, shared by checks C/D/E, WITH A FLOOR ───────
# Checks D and E both answer from this table, and a table that comes back EMPTY
# would make both report nothing — the "guard that passes over what it never
# read" shape this file already fails closed against on the history side. So the
# row count carries the same floor as the positive control above.
# MEASURED 2026-08-10: 2,265 tracked paths.
# ⚠ NAMED `$AllTrackedEolRows`, NOT `$EolRows`. PowerShell variable names are
# CASE-INSENSITIVE, so a `$EolRows` here IS the same variable as check C's
# `$eolRows` — which holds only the PINNED-GLOB subset. ✔MEASURED 2026-08-10: with
# the colliding name, check C's assignment overwrote this table and checks D and
# E1 silently walked 1,418 rows instead of 2,265 while the summary still claimed
# to have judged the working tree. Found by RUNNING both twins and comparing
# their counts (2,265 vs 1,418), not by reading the code.
$AllTrackedEolRows = Get-GitLines @('ls-files','--eol')
if ($AllTrackedEolRows.Count -lt $ScanFloor) {
    Write-Host "line-endings: FAIL - ``git ls-files --eol`` returned only $($AllTrackedEolRows.Count) rows, below its floor of $ScanFloor."
    Write-Host "  Checks D and E answer from that table, so an empty one makes BOTH report a clean"
    Write-Host "  worktree over files they never looked at. Refusing to pass; fix the scan."
    exit 2
}

# ── Check C: the pin's own glob set must contain no binary-detected blob ──
# Suspended with checks A/C on a stale checkout: `i/-text` is an INDEX fact.
$PinnedGlobEolRows = if ($SkipHistoryScan) { @() } else {
    Get-GitLines @('ls-files','--eol','--','*.c','*.h','*.cpp','*.hpp',
                   '*.cmake','CMakeLists.txt','*/CMakeLists.txt','*.md','VERSION')
}
foreach ($row in $PinnedGlobEolRows) {
    if ($row -match '^i/-text') {
        $path = ($row -split "`t", 2)[-1]
        $report.Add("  staged (index): binary-detected inside the eol=lf pin, so the blob scan above never opened it: $path")
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
foreach ($row in $AllTrackedEolRows) {
    $parts = $row -split "`t", 2
    if ($parts.Count -lt 2) { continue }
    $attrs = $parts[0]
    if ($attrs -notmatch 'eol=lf') { continue }
    if ($attrs -match '(^|\s)w/(crlf|mixed)(\s|$)') {
        $report.Add("  working (tracked, eol=lf pinned): rewritten to CRLF on disk - git diff shows NOTHING to review: $($parts[1])")
    }
}

# ── Check E: THE UNSTAGED WORKING TREE — the tier the other checks cannot see ──
# ★★ THE BLIND SPOT, MEASURED 2026-08-10 on this tree. Checks A and B read BLOBS
# (HEAD and the index); check C is an index fact; check D reads the disk but ONLY
# for files that DECLARE `eol=lf`. So a CRLF introduced into a working file that
# is neither staged nor covered by the pin was invisible to every tier - and
# "before commit" is exactly when a line-ending mistake is cheap to fix and the
# only moment this guard can prevent rather than diagnose.
# Not theoretical arithmetic: of 2,265 tracked paths, 2,186 declare `eol=lf` and
# 55 are tracked TEXT with NO such declaration. `.gitattributes` carries no
# `* text=auto` and this workstation reports `core.autocrlf=false`, so for those
# 55 git performs NO normalisation on `git add` - a CRLF rewrite of any one of
# them LANDS CRLF IN THE COMMIT. Same for a NEW untracked file outside the pin.
# ⚠ `core.autocrlf=true` (the Git-for-Windows INSTALLER DEFAULT, so the likeliest
# config for a reader of THIS twin) makes a CRLF WORKING COPY of an unpinned text
# file the LEGITIMATE result of checkout. Convicting there would be a false red on
# a correctly-configured host, so E1 states the situation instead of convicting -
# and E2 (untracked files, never checked out) keeps convicting either way.
# Kept behaviourally identical to the .sh sibling, measured by the same `git`
# subcommands with the same arguments.
$AutoCrlf = (& git config core.autocrlf 2>$null)
if ([string]::IsNullOrEmpty($AutoCrlf)) { $AutoCrlf = '<unset>' }
# E1 - TRACKED, text, NOT covered by an `eol=lf` pin, CRLF/mixed on disk.
# `i/-text` is binary (its 0x0D is legitimate) and `i/none` is empty; both are
# excluded by NAME rather than by "anything else", for the same reason check D
# uses a closed `w/` set: an unmeasured state must not be guessed into a red.
foreach ($row in $AllTrackedEolRows) {
    $parts = $row -split "`t", 2
    if ($parts.Count -lt 2) { continue }
    $attrs = $parts[0]
    if ($attrs -match 'eol=lf') { continue }
    # Matched by REGEX, not by splitting and indexing: `Set-StrictMode -Version
    # Latest` turns an out-of-bounds array index into a THROW, so a single
    # malformed row would kill the guard with a PowerShell stack trace instead of
    # a diagnostic. Check D above already reads this field with a regex; E1 uses
    # the same shape so the two cannot diverge in robustness either.
    if ($attrs -match '(^|\s)i/(-text|none)(\s|$)') { continue }
    if ($attrs -notmatch '(^|\s)w/(crlf|mixed)(\s|$)') { continue }
    if ($AutoCrlf -eq 'true') {
        Write-Host "line-endings: NOTE - '$($parts[1])' is CRLF on disk and carries NO eol=lf pin, but core.autocrlf=true,"
        Write-Host "    so a CRLF checkout is the expected result here and this is NOT convicted. Add an eol=lf pin"
        Write-Host "    for its extension if this repo should own its bytes regardless of a host's git config."
    } else {
        $report.Add("  working (tracked, NOT covered by an eol=lf pin): CRLF on disk and core.autocrlf=$AutoCrlf, so ``git add`` will NOT normalise it - this WILL land CRLF in the commit: $($parts[1])")
    }
}
# E2 - UNTRACKED (not ignored), text, NOT covered by an `eol=lf` pin, has a CR.
# A pinned untracked file is deliberately NOT reported: its clean filter
# normalises it on `git add`, so it cannot land CRLF, and a guard that reds on a
# state git is about to fix teaches people to ignore it.
foreach ($f in (Get-GitLines @('ls-files','--others','--exclude-standard'))) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { continue }
    $bytes = [IO.File]::ReadAllBytes($f)
    if ($bytes.Length -eq 0) { continue }
    # BINARY skip via git's own heuristic (a NUL in the first 8000 bytes), so the
    # answer matches what `git grep -I` would have decided for a tracked blob.
    $probe = [Math]::Min($bytes.Length, 8000)
    $isBinary = $false
    for ($i = 0; $i -lt $probe; $i++) { if ($bytes[$i] -eq 0) { $isBinary = $true; break } }
    if ($isBinary) { continue }
    # An explicit `binary` / `-text` declaration is an exemption, exactly as `-I`
    # honours it for the blob tiers.
    if ((& git check-attr text -- $f 2>$null) -match ': text: unset$') { continue }
    if ((& git check-attr eol  -- $f 2>$null) -match ': eol: lf$')     { continue }
    $hasCr = $false
    foreach ($b in $bytes) { if ($b -eq 13) { $hasCr = $true; break } }
    if ($hasCr) {
        $report.Add("  working (untracked, not yet added): carries CR and NO eol=lf pin covers it, so ``git add`` will NOT normalise it - this WILL land CRLF in the commit: $f")
    }
}

if ($report.Count -eq 0) {
    if ($SkipHistoryScan) {
        # Never let the summary outrun the evidence: say what was NOT judged.
        Write-Host "line-endings: OK (WORKTREE ONLY - the history scan was SKIPPED, see above; the $ControlHead HEAD / $ControlIndex index blobs were NOT judged; $($AllTrackedEolRows.Count) working-tree paths were)"
    } else {
        # ★ The summary NAMES EVERY TIER it judged. A guard that says "OK" without
        # saying over what invites the reader to assume it covered the tier they
        # care about - and for four tiers of this guard's life, one of them (the
        # unstaged working tree) was the tier it did NOT cover.
        Write-Host "line-endings: OK (committed $ControlHead + staged $ControlIndex text blobs, working tree $($AllTrackedEolRows.Count) tracked paths + untracked, core.autocrlf=$AutoCrlf; none carries CR)"
    }
    exit 0
}

Write-Host "line-endings: FAIL - the LF contract is violated (tier named per line):"
Write-Host ""
foreach ($line in $report) { Write-Host $line }
Write-Host ""
Write-Host "Fix:"
Write-Host "  * A ``working (...)`` line is the CHEAP one: the bytes are only on your disk,"
Write-Host "    nothing is committed yet, and converting the file to LF right now costs a"
Write-Host "    single command. A ``committed (HEAD)`` line is the same defect after it"
Write-Host "    became history. Fix the working tier BEFORE you stage."
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
