#!/usr/bin/env pwsh
# cmake-import — convert a CMake project into a DSS `.dss-project.json` manifest.
#
# Thin wrapper: it runs `cmake` with CMAKE_EXPORT_COMPILE_COMMANDS=ON, then
# hands the resulting compile_commands.json to the shared transform
# `cmake-import.py` (the single source of truth), which aggregates the per-TU
# sources / includes / defines and writes the `.dss-project.json` the compiler
# consumes via `dss-code-prime --project`.
#
# Usage:
#   scripts/cmake-import/cmake-import.ps1 <root-cmake-dir> <output-project-file> [options]
#
# Positional (both required):
#   <root-cmake-dir>       CMake project root (must contain CMakeLists.txt)
#   <output-project-file>  path of the .dss-project.json to write
#
# Options:
#   --target <spec>        DSS "<targetName>:<formatName>" (repeatable).
#                          Default = the host-native spec.
#   --language <name>      DSS language name.        Default: c-subset
#   --profile <name>       DSS artifactProfile.      Default: cli
#   --artifact-name <name> binary base name (no path separators).
#                          Default: the root dir's basename (sanitized).
#   -h, --help             show this help and exit.
#
# Requirements: cmake (with a Ninja or Unix Makefiles generator — Visual Studio
# and Xcode do not emit compile_commands.json) AND python3 (runs the shared
# cmake-import.py transform).
#
# NOTE: compile_commands.json is COMPILE-only — it carries no link libraries,
# so `resolveLibraries` is never emitted. If your project links external
# libraries, add a `resolveLibraries` array to the manifest by hand.
#
# Companion of cmake-import.sh; both call the same cmake-import.py, so they
# emit BYTE-IDENTICAL JSON by construction.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$prog = 'cmake-import.ps1'
$scriptDir = $PSScriptRoot
$pyFile = Join-Path $scriptDir 'cmake-import.py'

function Die([string]$msg) {
    [Console]::Error.WriteLine("${prog}: error: $msg")
    exit 1
}

function Show-Usage {
    # Print the contiguous leading comment block (after the shebang) as help.
    $lines = @(Get-Content -LiteralPath $PSCommandPath)
    for ($k = 1; $k -lt $lines.Count; $k++) {
        if ($lines[$k] -notmatch '^#') { break }
        Write-Output ($lines[$k] -replace '^# ?', '')
    }
}

# ─────────────────────────────────────────────────────────────────────────────
# host-native target spec detection
#
# Format names are the shipped src/dss-config/object-formats/*.format.json
# stems; target names are the shipped targets/*.target.json `target.name`
# values (x86_64 / arm64). Confirmed against the config tree.
# ─────────────────────────────────────────────────────────────────────────────
function Get-HostSpec {
    $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
    $isX64 = ($arch -eq [System.Runtime.InteropServices.Architecture]::X64)
    $isArm = ($arch -eq [System.Runtime.InteropServices.Architecture]::Arm64)
    if ($IsWindows) {
        if ($isX64) { return 'x86_64:pe64-x86_64-windows-exec' }
        Die "unsupported Windows architecture '$arch' — pass --target explicitly"
    } elseif ($IsMacOS) {
        if ($isArm) { return 'arm64:macho64-arm64-darwin-exec' }
        if ($isX64) { return 'x86_64:macho64-x86_64-darwin-exec' }
        Die "unsupported macOS architecture '$arch' — pass --target explicitly"
    } elseif ($IsLinux) {
        if ($isX64) { return 'x86_64:elf64-x86_64-linux-exec' }
        if ($isArm) { return 'arm64:elf64-aarch64-linux-exec' }
        Die "unsupported Linux architecture '$arch' — pass --target explicitly"
    } else {
        Die "unrecognized host OS — pass --target explicitly"
    }
}

# ─────────────────────────────────────────────────────────────────────────────
# argument parsing
# ─────────────────────────────────────────────────────────────────────────────
$language = 'c-subset'
$profile  = 'cli'
$artifact = ''            # explicit --artifact-name; empty => derive
$targets  = [System.Collections.Generic.List[string]]::new()
$positionals = [System.Collections.Generic.List[string]]::new()

$argv = @($args)
$idx = 0
while ($idx -lt $argv.Count) {
    $a = [string]$argv[$idx]
    # NOTE: `switch -Regex` runs EVERY matching branch unless `break` — each
    # specific flag pattern also matches the catch-all `^-.+`, so every branch
    # MUST break or a real flag would be parsed AND then rejected as unknown.
    switch -Regex ($a) {
        '^(-h|--help)$'    { Show-Usage; exit 0 }
        '^--target=(.*)$'  { $targets.Add($Matches[1]); break }
        '^--target$'       { $idx++; if ($idx -ge $argv.Count) { Die '--target requires a value' }; $targets.Add([string]$argv[$idx]); break }
        '^--language=(.*)$' { $language = $Matches[1]; break }
        '^--language$'     { $idx++; if ($idx -ge $argv.Count) { Die '--language requires a value' }; $language = [string]$argv[$idx]; break }
        '^--profile=(.*)$' { $profile = $Matches[1]; break }
        '^--profile$'      { $idx++; if ($idx -ge $argv.Count) { Die '--profile requires a value' }; $profile = [string]$argv[$idx]; break }
        '^--artifact-name=(.*)$' { $artifact = $Matches[1]; break }
        '^--artifact-name$' { $idx++; if ($idx -ge $argv.Count) { Die '--artifact-name requires a value' }; $artifact = [string]$argv[$idx]; break }
        '^-.+'             { Die "unknown flag '$a' (see --help)"; break }
        default            { $positionals.Add($a); break }
    }
    $idx++
}

if ($positionals.Count -lt 2) {
    Die "expected 2 positional arguments <root-cmake-dir> <output-project-file>; got $($positionals.Count) (see --help)"
}
if ($positionals.Count -gt 2) {
    Die "too many positional arguments ($($positionals.Count)); expected exactly <root-cmake-dir> <output-project-file> (see --help)"
}
$rootDir = $positionals[0]
$outFile = $positionals[1]

if ([string]::IsNullOrEmpty($language)) { Die '--language must be non-empty' }
if ([string]::IsNullOrEmpty($profile))  { Die '--profile must be non-empty' }
if ($artifact -match '[/\\]') { Die "--artifact-name must be a bare file name (no '/' or '\'): '$artifact'" }

# ─────────────────────────────────────────────────────────────────────────────
# validate inputs / tooling
# ─────────────────────────────────────────────────────────────────────────────
if (-not (Test-Path -LiteralPath $rootDir -PathType Container)) {
    Die "root directory not found: '$rootDir'"
}
if (-not (Test-Path -LiteralPath (Join-Path $rootDir 'CMakeLists.txt') -PathType Leaf)) {
    Die "no CMakeLists.txt in root directory: '$rootDir'"
}
if (-not (Test-Path -LiteralPath $pyFile -PathType Leaf)) {
    Die "shared transform not found next to this script: '$pyFile'"
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Die 'cmake not found on PATH — install CMake and retry'
}

# Both wrappers run the shared cmake-import.py, so python3 is required.
$py = $null
foreach ($cand in @('python3', 'python')) {
    if (Get-Command $cand -ErrorAction SilentlyContinue) { $py = $cand; break }
}
if ($null -eq $py) {
    Die 'python3 (or python) not found on PATH — required to run cmake-import.py'
}

$rootAbs = (Resolve-Path -LiteralPath $rootDir).Path
$rootBasename = Split-Path -Leaf $rootAbs

if ($targets.Count -eq 0) {
    $targets.Add((Get-HostSpec))
}

# ─────────────────────────────────────────────────────────────────────────────
# CMake configure — export compile_commands.json (default -> Ninja -> Makefiles)
# ─────────────────────────────────────────────────────────────────────────────
$buildDir = Join-Path $rootAbs '.dss-cmake-import-build'
$log = Join-Path $buildDir '.cmake-import.log'
$ccjson = $null

function Invoke-CMakeGen([string]$generator) {
    if (Test-Path -LiteralPath $buildDir) { Remove-Item -Recurse -Force -LiteralPath $buildDir }
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    $cmakeArgs = @('-S', $rootAbs, '-B', $buildDir, '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON')
    if ($generator -ne '') { $cmakeArgs += @('-G', $generator) }
    & cmake @cmakeArgs *> $log
    return ($LASTEXITCODE -eq 0)
}

try {
    foreach ($gen in @('', 'Ninja', 'Unix Makefiles')) {
        $ok = Invoke-CMakeGen $gen
        $cc = Join-Path $buildDir 'compile_commands.json'
        if ($ok -and (Test-Path -LiteralPath $cc -PathType Leaf)) {
            $ccjson = $cc
            break
        }
    }

    if ($null -eq $ccjson) {
        [Console]::Error.WriteLine("${prog}: error: CMake did not produce compile_commands.json.")
        [Console]::Error.WriteLine('Tried the default generator, Ninja, and Unix Makefiles.')
        [Console]::Error.WriteLine('Use a generator that supports CMAKE_EXPORT_COMPILE_COMMANDS')
        [Console]::Error.WriteLine('(Ninja or Unix Makefiles). Last CMake output:')
        [Console]::Error.WriteLine('----------------------------------------------------------------')
        if (Test-Path -LiteralPath $log) { [Console]::Error.WriteLine((Get-Content -Raw -LiteralPath $log)) }
        [Console]::Error.WriteLine('----------------------------------------------------------------')
        exit 1
    }

    # ── invoke the shared transform (single source of truth) ─────────────────
    # $rootAbs is the native Windows path (C:\...); cmake-import.py normalizes
    # its backslashes to '/', matching the compile_commands.json paths, so the
    # sources/includes under the root come out relative.
    $pyArgs = @(
        $pyFile,
        '--compile-commands', $ccjson,
        '--output', $outFile,
        '--language', $language,
        '--profile', $profile,
        '--default-artifact-name', $rootBasename,
        '--relative-to', $rootAbs
    )
    if ($artifact -ne '') { $pyArgs += @('--artifact-name', $artifact) }
    foreach ($t in $targets) { $pyArgs += @('--target', $t) }

    & $py @pyArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
    if (Test-Path -LiteralPath $buildDir) {
        Remove-Item -Recurse -Force -LiteralPath $buildDir -ErrorAction SilentlyContinue
    }
}
