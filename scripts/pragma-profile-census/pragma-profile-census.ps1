#!/usr/bin/env pwsh
# TF-C85 pragma census / TF-C86 (D-CSUBSET-STDARG-F001A) pairing retrofit — LAUNCHER ONLY (Windows sibling of
# scripts/pragma-profile-census/pragma-profile-census.sh).
#
# The census itself lives in `scripts/pragma-profile-census/pragma-profile-census.py`, once. This file finds a
# Python 3 and runs it. Its ONLY other job is to propagate the exit code
# EXACTLY: `$LASTEXITCODE` is read into a local IMMEDIATELY after the call and
# nothing is allowed to run in between, because any later statement (even a
# `Write-Host` that internally invokes a native command) can clobber it — the
# PowerShell analogue of a tail-swallowed status in the bash launcher.
#
# This script NEVER shells out to bash: a .ps1 that needs bash is useless on the
# machines it exists for. Both launchers call the same .py, so the two platforms
# cannot drift into disagreeing about the corpus.
#
# Usage / options / exit codes: see scripts/pragma-profile-census/pragma-profile-census.py (`--help`).

[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $CensusArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$censusPy = Join-Path $here 'pragma-profile-census.py'

if (-not (Test-Path -LiteralPath $censusPy -PathType Leaf)) {
    Write-Error "pragma-census: FATAL: $censusPy is missing — this launcher has no census of its own."
    exit 2
}

# Interpreter discovery, Windows order: `python` (the usual install), then the
# `py -3` launcher that ships with python.org builds, then `python3`.
# Each candidate is PROVEN to be 3.x before it is accepted; a bare name that
# resolves to the Microsoft Store stub would otherwise fail much later and much
# less clearly.
$versionProbe = 'import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)'
$candidates = @(
    @{ Exe = 'python';  Pre = @() },
    @{ Exe = 'py';      Pre = @('-3') },
    @{ Exe = 'python3'; Pre = @() }
)

$chosen = $null
foreach ($candidate in $candidates) {
    $cmd = Get-Command $candidate.Exe -ErrorAction SilentlyContinue
    if (-not $cmd) { continue }
    & $candidate.Exe @($candidate.Pre + @('-c', $versionProbe)) 2>$null
    $probeCode = $LASTEXITCODE          # read IMMEDIATELY
    if ($probeCode -eq 0) { $chosen = $candidate; break }
}

if ($null -eq $chosen) {
    Write-Host 'pragma-census: FATAL: no Python 3 interpreter found.'
    Write-Host '  looked for: python, then "py -3", then python3'
    Write-Host '  (each accepted only if it reports 3.x)'
    Write-Host '  Install Python 3 or put it on PATH. Refusing to skip the census'
    Write-Host '  silently — a census that does not run must not look like a census'
    Write-Host '  with nothing to report.'
    exit 2
}

$forward = @()
if ($null -ne $CensusArgs) { $forward = $CensusArgs }

& $chosen.Exe @($chosen.Pre + @($censusPy) + $forward)
$censusExit = $LASTEXITCODE             # read IMMEDIATELY — nothing between
exit $censusExit
