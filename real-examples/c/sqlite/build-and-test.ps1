#!/usr/bin/env pwsh
# build-and-test.ps1 — the WINDOWS (pe64) RUNNABLE harness for SQLite-on-DSS.
#
# The native-Windows companion to build-and-test.sh. The .sh runs on a Linux/WSL
# host and treats `windows` as COMPILE-ONLY (a Linux host cannot execute a .exe).
# This script makes the Windows pe64 leg a FULLY RUNNABLE target: it builds
# dss-code-prime with MSVC, compiles SQLite to a real pe64 `sqlite3.exe`, RUNS it,
# and smoke-tests a live SQL round-trip — the same "SELECT sum → 42, --version →
# 3.54.0" witness the .sh runs for the Linux legs, now on Windows.
#
# ── AMALGAMATION VIA WSL (user decision 2026-07-06) ──────────────────────────
# SQLite's amalgamation (`./configure && make sqlite3.c shell.c`) is autotools +
# tclsh — inherently a Unix build. Rather than reproduce it on Windows, this
# script INVOKES WSL to amalgamate (clone/update sqlite + make sqlite3.c shell.c
# in the WSL tree), then copies the two amalgamated TUs onto Windows for the
# native DSS compile + run. WSL is a hard prerequisite.
#
# ── NO --define KNOBS (since c116) ───────────────────────────────────────────
# The pe64 compile needs NO extra defines. Both former knobs are closed by real
# compiler support: SQLITE_DISABLE_INTRINSIC dropped at c113 (DSS resolves
# <intrin.h> via the pe-gated shippedLibs/intrin.json descriptor + the
# _ReadWriteBarrier/_byteswap builtins), SQLITE_OMIT_SEH dropped at c116 (DSS
# emits x64 SEH filter funclets + __C_specific_handler scope tables, so
# sqlite's wal.c __try/__except compiles and catches for real).
#
# ── PREREQUISITES ────────────────────────────────────────────────────────────
#   Windows + WSL (Debian/Ubuntu) with git/gcc/make/tcl; MSVC + CMake 4+ for the
#   DSS Windows build. Run from anywhere — paths resolve relative to this script.
#
# ── OVERRIDES (env vars) ─────────────────────────────────────────────────────
#   $env:DSS_JOBS     parallel build jobs (default: CPU count)
#   $env:SQLITE_WSL_DIR   WSL path for the sqlite clone (default: ~/src/sqlite)
#   $env:SKIP_DSS_BUILD   "1" to reuse an existing Release binary (fast re-runs)

$ErrorActionPreference = 'Stop'

# ── logging / fail-loud (mirrors the .sh's step/info/pass/warn/die) ──────────
function Step($m) { Write-Host "`n== $m ==" -ForegroundColor Blue }
function Info($m) { Write-Host "   $m" }
function Pass($m) { Write-Host " [OK] $m" -ForegroundColor Green }
function Warn($m) { Write-Host " [!]  $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host " [X] ERROR: $m" -ForegroundColor Red; exit 1 }

# The repo root: this script lives at real-examples/c/sqlite/, so root = ../../../.
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$Jobs     = if ($env:DSS_JOBS) { $env:DSS_JOBS } else { [Environment]::ProcessorCount }
$SqliteWslDir = if ($env:SQLITE_WSL_DIR) { $env:SQLITE_WSL_DIR } else { '$HOME/src/sqlite' }
$Work     = Join-Path $RepoRoot 'build\real-examples\c\sqlite\windows'
$Spec     = 'x86_64:pe64-x86_64-windows-exec'

# The smoke witness — identical to the .sh's.
$SqlSmoke = "CREATE TABLE t(x);INSERT INTO t VALUES(1);INSERT INTO t VALUES(41);SELECT sum(x) FROM t;"
$ExpectSum = '42'
$ExpectVersionPrefix = '3.54.0'

# ── Step 1 — host is Windows + WSL is available ──────────────────────────────
Step '1/6  Host check (Windows + WSL)'
if (-not $IsWindows -and $PSVersionTable.PSVersion.Major -ge 6) { Die "this harness targets Windows (the pe64 RUN leg); use build-and-test.sh on Linux/WSL." }
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) { Die "wsl.exe not found — WSL is required to amalgamate SQLite (autotools + tclsh). Install WSL + a Debian/Ubuntu distro." }
# A trivial WSL round-trip confirms a distro is installed and responsive.
$probe = & wsl.exe bash -c 'echo wsl-ok' 2>&1
if ($LASTEXITCODE -ne 0 -or "$probe".Trim() -ne 'wsl-ok') { Die "WSL is present but a bash round-trip failed (got: '$probe'). Ensure a distro is installed + running." }
Pass "Windows host + WSL online"

# ── Step 2 — amalgamate SQLite VIA WSL (autotools: make sqlite3.c shell.c) ────
Step '2/6  Amalgamate SQLite in WSL (autotools: make sqlite3.c shell.c)'
New-Item -ItemType Directory -Force -Path $Work | Out-Null
# The amalgamation runs in WSL (autotools + tclsh are inherently Unix). The bash
# below is written to a temp .sh and run via `wsl bash <file>` — the most robust
# way to hand a multi-line script to WSL (no `bash -c` arg-quoting hazards). It:
#   * FAILS LOUD (not apt-install) if a tool is missing — this is a user-local
#     Windows+WSL harness, not CI with passwordless sudo; a clear "install X" beats
#     a cryptic sudo prompt. All four (git/gcc/make/tclsh) are the .sh's Step-4 deps.
#   * REUSES an existing checkout at $DIR whether it's a git clone, a fossil
#     checkout, or a plain copy (sqlite's canonical repo is fossil; the github
#     mirror is git) — it only `git clone`s when $DIR is absent. Keyed on the
#     presence of `configure`, so it never clone-clobbers a non-empty dir.
$amalgamateScript = @'
set -euo pipefail
for t in git gcc make tclsh; do
  command -v "$t" >/dev/null 2>&1 || {
    echo "MISSING tool: $t — install the amalgamation toolchain, e.g.:" >&2
    echo "    sudo apt-get install -y git build-essential tcl tcl-dev" >&2
    exit 1; }
done
DIR="__SQLITE_WSL_DIR__"
if [ -x "$DIR/configure" ]; then
  [ -d "$DIR/.git" ] && git -C "$DIR" pull --rebase --quiet 2>/dev/null || true
elif [ -d "$DIR/.git" ]; then
  git -C "$DIR" pull --rebase --quiet 2>/dev/null || true
else
  mkdir -p "$(dirname "$DIR")"
  git clone --quiet https://github.com/sqlite/sqlite.git "$DIR"
fi
[ -x "$DIR/configure" ] || { echo "no ./configure in $DIR — not a SQLite checkout" >&2; exit 1; }
BLD="$DIR/bld-dss"; mkdir -p "$BLD"
( cd "$BLD" && "$DIR/configure" >/dev/null 2>&1 && make -s sqlite3.c shell.c )
[ -f "$BLD/sqlite3.c" ] || { echo "amalgamation not produced at $BLD/sqlite3.c" >&2; exit 1; }
[ -f "$BLD/shell.c" ]   || { echo "shell.c not produced at $BLD/shell.c" >&2; exit 1; }
# Emit the Windows-visible paths of the three TUs (sqlite3.h is beside sqlite3.c),
# each on its own marker line so PowerShell parses them unambiguously.
echo "AMALGAMATED-C=$(wslpath -w "$BLD/sqlite3.c")"
echo "AMALGAMATED-SHELL=$(wslpath -w "$BLD/shell.c")"
echo "AMALGAMATED-H=$(wslpath -w "$BLD/sqlite3.h")"
'@
$amalgamateScript = $amalgamateScript.Replace('__SQLITE_WSL_DIR__', $SqliteWslDir) -replace "`r`n", "`n"
$tmpSh = Join-Path $Work 'amalgamate.sh'
Set-Content -LiteralPath $tmpSh -Value $amalgamateScript -NoNewline -Encoding ascii
# Convert C:\path\file → /mnt/c/path/file MANUALLY — passing a backslash Windows
# path through `wsl.exe wslpath` strips the backslashes (arg mangling).
function ToWslPath($p) {
  $full = (Resolve-Path -LiteralPath $p).Path
  '/mnt/' + $full.Substring(0,1).ToLowerInvariant() + ($full.Substring(2) -replace '\\','/')
}
$tmpShWsl = ToWslPath $tmpSh
$wslOut = & wsl.exe bash -l $tmpShWsl 2>&1
if ($LASTEXITCODE -ne 0) { Die "WSL amalgamation failed:`n$($wslOut -join "`n")" }
function Marker($k) { ($wslOut | Select-String -Pattern "^$k=(.+)$" | Select-Object -Last 1).Matches[0].Groups[1].Value }
$srcSqlite3C = Marker 'AMALGAMATED-C'
$srcShellC   = Marker 'AMALGAMATED-SHELL'
$srcSqlite3H = Marker 'AMALGAMATED-H'
if (-not ($srcSqlite3C -and $srcShellC -and $srcSqlite3H)) { Die "amalgamation did not emit the 3 TU paths:`n$($wslOut -join "`n")" }

# Copy the amalgamation onto Windows (shell.c #includes "sqlite3.h", so all three
# must sit adjacent — the same 3-file rule the pe64 probe uses).
Copy-Item -Force -LiteralPath $srcSqlite3C -Destination (Join-Path $Work 'sqlite3.c')
Copy-Item -Force -LiteralPath $srcShellC   -Destination (Join-Path $Work 'shell.c')
Copy-Item -Force -LiteralPath $srcSqlite3H -Destination (Join-Path $Work 'sqlite3.h')
$amLines = (Get-Content (Join-Path $Work 'sqlite3.c') | Measure-Object -Line).Lines
Pass "amalgamation copied to $Work (sqlite3.c: $amLines lines)"

# ── Step 3 — build dss-code-prime (MSVC Release) ─────────────────────────────
Step '3/6  Build dss-code-prime (MSVC, Release)'
$DssBin = Join-Path $RepoRoot 'build\bin\dss\Release\dss-code-prime.exe'
if ($env:SKIP_DSS_BUILD -eq '1' -and (Test-Path $DssBin)) {
  Info "SKIP_DSS_BUILD=1 — reusing $DssBin"
} else {
  if (-not (Test-Path (Join-Path $RepoRoot 'build'))) {
    & cmake -S $RepoRoot -B (Join-Path $RepoRoot 'build') -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { Die "cmake configure failed" }
  }
  & cmake --build (Join-Path $RepoRoot 'build') --config Release --target dss-code-prime -j $Jobs
  if ($LASTEXITCODE -ne 0) { Die "dss-code-prime build failed" }
}
if (-not (Test-Path $DssBin)) { Die "dss-code-prime.exe not found at $DssBin after the build." }
Pass "dss-code-prime built: $DssBin"

# ── Step 4 — compile SQLite to pe64 (2-TU, NO defines — SEH on) ──────────────
Step '4/6  Compile SQLite -> sqlite3.exe (pe64, 2-TU, no defines)'
$OutDir = Join-Path $Work 'out'
if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
# sqlite3.c FIRST (the multi-TU merge needs the library CU ahead of the driver),
# then shell.c (the CLI driver). NO --define: c113/c116 closed both knobs.
$log = Join-Path $Work 'compile.log'
& $DssBin --language c-subset --target $Spec --time `
    --compile (Join-Path $Work 'sqlite3.c') (Join-Path $Work 'shell.c') `
    --output $OutDir *>&1 | Tee-Object -FilePath $log | Out-Null
# GOTCHA: dss-code-prime returns exit 0 even on FATAL compile errors — success is
# read from the DIAGNOSTICS, not $LASTEXITCODE. Any `error[CODE]:` line = a miss.
$errCount = (Select-String -Path $log -Pattern '^error\[' -AllMatches).Count
if ($errCount -gt 0) {
  Get-Content $log | Select-String -Pattern '^error\[' | Select-Object -First 5 | ForEach-Object { Info $_.Line }
  Die "pe64 compile emitted $errCount error[...] diagnostic(s) — see $log"
}
# `--output X` writes the artifact INTO X as a directory (X/<name>.exe) — flatten.
$exe = Get-ChildItem -Path $OutDir -Recurse -Filter '*.exe' -File | Select-Object -First 1
if (-not $exe) { Die "no sqlite3.exe emitted under $OutDir" }
$Sqlite3Exe = Join-Path $Work 'sqlite3.exe'
Copy-Item -Force -LiteralPath $exe.FullName -Destination $Sqlite3Exe
$ctime = (Get-Content $log | Select-String -Pattern 'compile time (\S+)' | Select-Object -Last 1)
$ctimeSuffix = if ($ctime) { "  ($($ctime.Matches[0].Value))" } else { '' }
Pass "compiled: $Sqlite3Exe$ctimeSuffix"

# ── Step 5 — smoke: a live SQL round-trip + --version ────────────────────────
Step '5/6  Smoke test (SELECT sum -> 42, --version -> 3.54.0)'
$sqlOut = ($SqlSmoke | & $Sqlite3Exe 2>&1 | Out-String).Trim()
$verOut = (& $Sqlite3Exe --version 2>&1 | Out-String).Trim()
$reasons = @()
if (($sqlOut -replace '\s','') -ne $ExpectSum) {
  $reasons += "SELECT sum(x) != $ExpectSum (got: '$($sqlOut.Substring(0, [Math]::Min(120, $sqlOut.Length)))')"
}
if (-not $verOut.StartsWith($ExpectVersionPrefix)) {
  $reasons += "--version != $ExpectVersionPrefix* (got: '$($verOut.Substring(0, [Math]::Min(120, $verOut.Length)))')"
}
if ($reasons.Count -gt 0) { Die ("[windows] smoke FAIL — " + ($reasons -join '; ')) }
Pass "[windows] smoke PASS — SELECT sum -> 42, --version -> $ExpectVersionPrefix"

# ── Step 6 — summary ─────────────────────────────────────────────────────────
Step '6/6  Summary'
Pass "Windows pe64 leg is RUNNABLE: dss-built sqlite3.exe runs a live SQL round-trip."
Info "  binary : $Sqlite3Exe"
Info "  version: $verOut"
Write-Host "`n [OK] SQLite-on-DSS Windows (pe64) harness: PASS" -ForegroundColor Green
exit 0
