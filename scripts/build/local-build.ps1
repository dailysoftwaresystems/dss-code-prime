# Local incremental build + test harness for dss-code-prime.
#
# Usage:
#   scripts\build\local-build.ps1               # build only
#   scripts\build\local-build.ps1 -Test         # build then run ctest
#   scripts\build\local-build.ps1 -Configure    # cmake configure + build
#   scripts\build\local-build.ps1 -Clean        # wipe build dir + reconfigure + build
#
# Designed to be safe to invoke without approval prompts in agentic
# workflows — read-only on src/, writes only inside build/.

[CmdletBinding()]
param(
    [switch]$Test,
    [switch]$Configure,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

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
