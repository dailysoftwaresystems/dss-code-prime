# Local incremental build + test harness for dss-code-prime.
#
# Usage:
#   scripts\build\local-build.ps1               # build only
#   scripts\build\local-build.ps1 -Test         # build then run ctest
#   scripts\build\local-build.ps1 -Configure    # cmake configure + build
#   scripts\build\local-build.ps1 -Clean        # wipe build dir + reconfigure + build
#
# Safe to invoke without approval prompts in agentic workflows —
# read-only on src/, writes only inside build/. Requires `cmake` on
# PATH at version >= 4.0 (project's CMakeLists.txt floor).

[CmdletBinding()]
param(
    [switch]$Test,
    [switch]$Configure,
    [switch]$Clean
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

if ($Clean) {
    if (Test-Path build) { Remove-Item -Recurse -Force build }
    $Configure = $true
}

if ($Configure -or -not (Test-Path 'build\build.ninja')) {
    cmake -S . -B build -G Ninja
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

cmake --build build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    Push-Location build
    try {
        ctest --output-on-failure
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Pop-Location
    }
}
