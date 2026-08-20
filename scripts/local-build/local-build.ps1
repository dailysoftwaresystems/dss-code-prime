# PURPOSE: build dss-code-prime incrementally on this host, and optionally run ctest.
# Local incremental build + test harness for dss-code-prime.
#
# Usage:
#   scripts\local-build\local-build.ps1                     # build build\dbg
#   scripts\local-build\local-build.ps1 -Test               # build then run ctest
#   scripts\local-build\local-build.ps1 -Configure          # cmake configure + build
#   scripts\local-build\local-build.ps1 -Clean              # wipe THIS TREE + reconfigure + build
#   scripts\local-build\local-build.ps1 -Tree rel           # operate on build\rel instead
#   scripts\local-build\local-build.ps1 -Tree lane1 -BuildType Debug
#
# ★★★ `build\` IS A CONTAINER, NOT A BUILD TREE (the one-root layout, operator
# 2026-08-17: one root `build/`, one SUBDIRECTORY per build). This script used to
# treat `build\` itself as the tree, and `-Clean` was `Remove-Item -Recurse -Force
# build`, which under that layout DELETES EVERY SIBLING TREE AT ONCE: `build\dbg`,
# `build\rel`, `build\perf` and the gate logs beside them. It would also have
# configured a FOURTH, unnamed tree at `build\` alongside the real ones, and then
# run ctest in the wrong directory.
# ⚠ Losing `build\rel` is not merely slow to recover: the Windows driver picks the
# NEWEST RELEASE build tree, so a deleted-then-stale `build\rel` silently answers
# discovery afterwards. Fixed 2026-08-20. An anchor name must never be wrapped --
# a split name is not greppable, so no tool can find it and the balance guard
# cannot see it. It goes on one line of its own:
#   D-SCRIPT-LOCAL-BUILD-TREATS-THE-BUILD-ROOT-AS-A-BUILD-TREE
#
# Safe to invoke without approval prompts in agentic workflows — read-only on src/,
# writes only inside build\<tree>. Requires `cmake` on PATH at version >= 4.0
# (project's CMakeLists.txt floor).

[CmdletBinding()]
param(
    [switch]$Test,
    [switch]$Configure,
    [switch]$Clean,
    [string]$Tree = 'dbg',
    [string]$BuildType = ''
)

$ErrorActionPreference = 'Stop'

# Refresh PATH from the system + user env vars. PowerShell sessions
# inherit their parent's PATH and don't pick up post-launch system
# updates (Windows broadcasts WM_SETTINGCHANGE but PowerShell doesn't
# listen). This refresh costs nothing and makes the script robust
# against the "PATH was updated but my shell is stale" case.
$env:PATH = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ';' + [System.Environment]::GetEnvironmentVariable("Path", "User")

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root

# ★ The two tree names this repository has an established meaning for. A tree name
# NOT in this table is allowed (lane trees are ordinary), but configuring one for
# the FIRST time then requires an explicit -BuildType: silently configuring a Debug
# tree that someone named `rel` is the kind of quiet wrong answer this file already
# shipped once.
$establishedBuildType = @{ 'dbg' = 'Debug'; 'rel' = 'Release' }

# ── The tree name must resolve to a DIRECTLY-NESTED child of build\ ──────────
# Everything destructive below is keyed on this, so it is validated by SHAPE
# rather than by trusting the caller: no separators, no `..`, non-empty. A name
# like `..\src` or `dbg\..\..` would otherwise walk the delete out of the
# container, which is the precise failure this rewrite exists to prevent.
if ([string]::IsNullOrEmpty($Tree) -or $Tree -match '[\\/]' -or $Tree -eq '.' -or $Tree -eq '..') {
    [Console]::Error.WriteLine("refusing tree name '$Tree': it must be a single directory name directly under build\")
    exit 3
}
$buildDir = Join-Path 'build' $Tree

if ($Clean) {
    # Belt and braces: the shape check above already guarantees this, and the
    # guarantee is cheap to restate at the one call site that cannot be undone.
    if ($buildDir -eq 'build' -or -not $buildDir.StartsWith('build' + [System.IO.Path]::DirectorySeparatorChar)) {
        [Console]::Error.WriteLine("refusing to remove '$buildDir': -Clean only ever removes ONE tree under build\")
        exit 3
    }
    if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir -Confirm:$false }
    $Configure = $true
}

# -- -BuildType is meaningful ONLY when this run configures a tree from scratch --
# WARNING: THIS CHECK USED TO LIVE INSIDE THE CONFIGURE BLOCK, AND THAT MADE IT A
# NO-OP on the commonest path: with `build.ninja` already present and no -Configure,
# the whole block was skipped, so `-BuildType Release` on the debug tree exited 0
# having silently ignored the flag. MEASURED 2026-08-20 by EXERCISING the arm rather
# than reading it. It runs after -Clean on purpose: a cleaned tree has no cache, so
# the flag IS meaningful there.
if ($BuildType -and (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    [Console]::Error.WriteLine("refusing -BuildType on the EXISTING tree 'build\$Tree': its cache already declares one. " +
                 "Re-run with -Clean to reconfigure it from scratch.")
    exit 3
}

if ($Configure -or -not (Test-Path (Join-Path $buildDir 'build.ninja'))) {
    $cmakeArgs = @('-S', '.', '-B', $buildDir, '-G', 'Ninja')
    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        # First configure of this tree: the build type must be KNOWN, not guessed.
        # An existing tree keeps whatever its cache already says, so no type is
        # passed and a caller cannot silently flip a configured tree underneath
        # the artifacts already in it.
        if (-not $BuildType) { $BuildType = $establishedBuildType[$Tree] }
        if (-not $BuildType) {
            [Console]::Error.WriteLine("refusing to configure a NEW tree 'build\$Tree' with no build type: pass -BuildType <Debug|Release|...> " +
                         "(only 'dbg' -> Debug and 'rel' -> Release are established names in this repository)")
            exit 3
        }
        $cmakeArgs += "-DCMAKE_BUILD_TYPE=$BuildType"
    }
    cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    Push-Location $buildDir
    try {
        # See scripts/run-gate/run-gate.sh for the measurement behind the 8.
        # Set CTEST_PARALLEL_LEVEL, or pass -j, to override.
        # ★ Restored afterwards: a .ps1 runs IN-PROCESS, so a bare assignment
        # would leave the caller's shell silently parallel for everything it
        # ran next. The .sh twin needs no such care -- `export` dies with it.
        $__cplPrev = $env:CTEST_PARALLEL_LEVEL
        $__cplSet  = $false
        if (-not $env:CTEST_PARALLEL_LEVEL) {
            $env:CTEST_PARALLEL_LEVEL = '8'
            $__cplSet = $true
        }
        try {
            ctest --output-on-failure
        } finally {
            if ($__cplSet) {
                if ($null -eq $__cplPrev) {
                    Remove-Item Env:\CTEST_PARALLEL_LEVEL -ErrorAction SilentlyContinue
                } else {
                    $env:CTEST_PARALLEL_LEVEL = $__cplPrev
                }
            }
        }
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Pop-Location
    }
}
