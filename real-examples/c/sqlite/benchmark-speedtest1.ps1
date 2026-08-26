#requires -Version 5.1
<#
.SYNOPSIS
  Benchmark DSS Code Prime against gcc and MSVC building and running SQLite's own
  `test/speedtest1.c` FROM FULL SOURCE (~103 real translation units, not the
  amalgamation), on a native Windows host.

.DESCRIPTION
  POSIX companion: benchmark-speedtest1.sh. This is the WINDOWS half of that pair,
  and the two share every line that produces a number.

  ★★★ THIS DRIVER DOES NOT DERIVE THE SUBJECT — IT CALLS THE .sh THAT DOES.
  SQLite's build is autosetup + make + tclsh, so the recipe derivation has always
  run in a POSIX shell; build-and-test.ps1 has done exactly this since it was
  written. So the subject (which 103 TUs, which -D set, which -I dirs) comes from
  ONE implementation — `benchmark-speedtest1.sh --derive-only --path-style
  windows` — invoked through WSL, which hands back a plan whose paths are already
  in Windows form. There is no second derivation to keep in step, only a caller.
  ⚠ The path translation is done on the POSIX side too, by `wslpath`, for the
  same reason: hand-rolling `/mnt/c/...` -> `C:\...` here would be a second
  implementation of a mapping that already exists and that fails LATE when it is
  subtly wrong.

  ★★ WHY WINDOWS IS WHERE THIS RUNS AT ALL. MSVC exists on no other host, and the
  operator asked for gcc AND MSVC. That decides the host, and the host decides the
  rest: the SQLite checkout must sit on a LOCAL disk, because compiling across
  \\wsl$ (9P) costs several times the local I/O and does not cost every toolchain
  the same — a build-time comparison taken there measures the filesystem. That is
  refused (R4 in speedtest1_bench.py), not warned about.

  ★ WHAT IS DELIBERATELY NOT EQUALIZED, and the report says so in its own column:
  DSS compiles every CU inside ONE process on a worker-thread pool (`--jobs N`),
  while gcc and cl are driven as N concurrent `-c` processes plus a link. That
  difference IS the architecture under measurement.

.PARAMETER SqliteDir
  The SQLite checkout, on a LOCAL disk. Used AS-IS: never switched, never pulled.

.PARAMETER JobsArms
  The worker counts to measure, e.g. '1 4'. Both are taken so the -j4 number is
  interpretable: the subject is ~103 TUs, so 4-way parallelism is a real
  measurement rather than a ceiling the program cannot reach.

.EXAMPLE
  .\benchmark-speedtest1.ps1 -SqliteDir C:\Source\sqlite

.EXAMPLE
  .\benchmark-speedtest1.ps1 -SelfTest

.NOTES
  Exit codes: 0 measured, 1 a refusal, 2 usage, 3 no arm produced a binary.
#>
[CmdletBinding()]
param(
  [string] $SqliteDir      = $env:SQLITE_DIR,
  [string] $DssSrc         = '',
  [string] $Dss            = $env:DSS_BIN,
  [string] $Out            = '',
  [int]    $Size           = 25,
  [string] $Testset        = '',
  [int]    $BuildRepeats   = 3,
  [int]    $RunRepeats     = 5,
  [string] $JobsArms       = '1 4',
  [string] $Target         = 'x86_64:pe64-x86_64-windows-exec',
  [string] $RecipeTransform = 'windows-selfconfig',
  [int]    $StackReserve   = 8388608,
  [string] $Cc             = '',
  [string] $Plan           = '',
  [switch] $DeriveOnly,
  [switch] $SelfTest
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Step($m) { Write-Host "`n== $m ==" -ForegroundColor Blue }
function Info($m) { Write-Host "   $m" }
function Pass($m) { Write-Host " OK $m" -ForegroundColor Green }
function Warn($m) { Write-Host " !  $m" -ForegroundColor Yellow }
function Die  ($m) { Write-Host " X  ERROR: $m" -ForegroundColor Red; exit 1 }
function Usage($m) { Write-Host " X  USAGE: $m" -ForegroundColor Red; exit 2 }

$DriverSh   = Join-Path $ScriptDir 'benchmark-speedtest1.sh'
$BenchCore  = Join-Path $ScriptDir 'speedtest1_bench.py'
foreach ($f in @($DriverSh, $BenchCore)) {
  if (-not (Test-Path $f)) {
    Die "the shared half of this pair is missing: $f`n      This driver does not carry a private copy of it, on purpose."
  }
}

# ── the POSIX carriage ───────────────────────────────────────────────────────
# ★ `wsl.exe -e`, NEVER a bare `wsl.exe <cmd>`. Without `-e`, WSL RECONSTRUCTS a
# command line and the distro's shell expands `$( )` and `$VAR` a second time —
# the same trap build-and-test.ps1 documents at its own call sites, and the same
# family as the `wsl.exe bash -c` with a variable that once became
# `rsync -a --delete / /` and reported exit 0.
function Invoke-Wsl {
  param([Parameter(Mandatory)][string] $BashLine)
  $out = @(& wsl.exe -e bash -l -c $BashLine 2>&1)
  return @{ Output = $out; ExitCode = $LASTEXITCODE }
}

function ConvertTo-WslPath {
  param([Parameter(Mandatory)][string] $WindowsPath)
  # `wslpath -a -u` is the translator; asking WSL is the only way to be right
  # about a path that is not under a drive mount.
  $r = Invoke-Wsl "wslpath -a -u '$($WindowsPath -replace "'", "'\''")'"
  if ($r.ExitCode -ne 0 -or -not $r.Output) {
    Die "could not translate '$WindowsPath' into a WSL path (rc=$($r.ExitCode)): $($r.Output -join ' ')"
  }
  return ($r.Output | Select-Object -Last 1).Trim()
}

# ── self-test ────────────────────────────────────────────────────────────────
if ($SelfTest) {
  Step 'benchmark-speedtest1.ps1 — self-test'
  $fails = 0
  function Ck($name, $ok) {
    if ($ok) { Write-Host "   ok    $name" } else { Write-Host "   FAIL  $name"; $script:fails++ }
  }
  Ck 'the POSIX derivation driver is present'   (Test-Path $DriverSh)
  Ck 'the shared measurement core is present'   (Test-Path $BenchCore)
  $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
  Ck 'wsl.exe is reachable (this host can derive)' ($null -ne $wsl)
  if ($wsl) {
    $probe = Invoke-Wsl 'echo posix-ok'
    Ck 'the WSL carriage answers' ($probe.Output -contains 'posix-ok')
    # ★ THE ARM THAT MATTERS: `-e` must actually be suppressing the second
    # expansion. Without it the distro shell expands `$(uname)` locally and this
    # comes back as the literal it must NOT be.
    $q = Invoke-Wsl "printf '[%s]\n' 'echo A=`$(uname -m)'"
    Ck 'wsl.exe -e suppresses the second expansion' (($q.Output -join '') -match '\$\(uname -m\)')
  }
  $py = Get-Command python -ErrorAction SilentlyContinue
  if (-not $py) { $py = Get-Command python3 -ErrorAction SilentlyContinue }
  Ck 'a native python is reachable (the measurement runs here)' ($null -ne $py)
  if ($py) {
    Step 'self-test: the measurement core''s own arms'
    & $py.Source $BenchCore --selftest
    if ($LASTEXITCODE -ne 0) { $fails++ }
  }
  if ($fails -gt 0) { Die "$fails self-test arm(s) FAILED" }
  Pass 'all self-test arms green'
  exit 0
}

# ── Step 1 — resolve, and refuse a source tree that would poison the numbers ──
Step '1/4  Resolve the subject and the toolchains (native Windows)'
if (-not $SqliteDir) { $SqliteDir = 'C:\Source\sqlite' }
if (-not (Test-Path $SqliteDir)) {
  Die @"
no SQLite checkout at $SqliteDir
      Clone one there (git clone https://github.com/sqlite/sqlite) or pass
      -SqliteDir. It must be on a LOCAL disk: see the next refusal for why a
      \\wsl$ path is not an alternative.
"@
}
$SqliteDir = (Resolve-Path $SqliteDir).Path
# R4, asserted HERE as well as in the core, because here it can still be acted on
# cheaply — before a configure and a reference build have been spent.
if ($SqliteDir.StartsWith('\\')) {
  Die @"
the SQLite checkout is on a UNC share: $SqliteDir
      Compiling across \\wsl$ (9P) costs several times local-disk I/O and does NOT
      cost every toolchain the same, so a build-TIME comparison taken there
      measures the filesystem rather than the compilers. Put the checkout on a
      local disk. (Deriving the recipe over the share would be fine; measuring
      against it is not, so this refuses rather than warns.)
"@
}
$Speedtest = Join-Path $SqliteDir 'test\speedtest1.c'
if (-not (Test-Path $Speedtest)) {
  Die "the benchmark's subject is missing: $Speedtest`n      That file IS SQLite's own performance program."
}
Info "sqlite    : $SqliteDir"

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
  Die @"
wsl.exe not found.
      This is WHERE THIS HOST FINDS ITS POSIX TOOLCHAIN, not a statement about any
      target: deriving the SQLite recipe needs a POSIX shell + make + tclsh
      (SQLite's build is autosetup + tclsh). On a Windows host that toolchain is
      WSL. The MEASUREMENT still runs natively, here.
"@
}

$Python = Get-Command python -ErrorAction SilentlyContinue
if (-not $Python) { $Python = Get-Command python3 -ErrorAction SilentlyContinue }
if (-not $Python) { Die 'no native python on PATH; the measurement core runs here, not in WSL.' }

if (-not $DssSrc) { $DssSrc = (Resolve-Path (Join-Path $ScriptDir '..\..\..')).Path }
# ★ THE BINARY IS `dsscp.exe`, NOT `dss.exe`, AND SEARCHING FOR THE
# WRONG NAME LOOKS EXACTLY LIKE "NOT BUILT YET" (a rename to `dsscp` is queued
# but has not landed). Same resolution the .sh does, same rel-before-dbg
# preference: a debug compiler's build time is not a number worth publishing.
if (-not $Dss) {
  foreach ($tree in @('rel', 'dbg')) {
    $root = Join-Path $DssSrc "build\$tree"
    if (-not (Test-Path $root)) { continue }
    $hit = Get-ChildItem -Path $root -Filter 'dsscp.exe' -Recurse -File `
             -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hit) { $Dss = $hit.FullName; break }
  }
}
# ⚠ TWO DIFFERENT FACTS, TWO DIFFERENT MESSAGES — the same split the .sh makes.
# "the path you gave is not there" and "nothing was found under the tree I
# searched" have different remedies, and one message for both sends the reader to
# build a compiler they already have.
if ($Dss -and -not (Test-Path $Dss)) {
  Die "the dsscp path given does not exist: $Dss`n      (the compiler is named 'dsscp.exe', not 'dss.exe')"
}
if (-not $Dss) {
  Die @"
no dsscp binary found.
      Pass -Dss <path>, or build one: scripts\local-build\local-build.ps1 -Tree rel
      Searched for dsscp.exe under $DssSrc\build\{rel,dbg}\.
"@
}
$Dss = (Resolve-Path $Dss).Path
Info "dss       : $Dss"

# ★★ PIN THE CONFIG ROOT, THEN PROVE THE COMPILER WORKS WITH IT — BEFORE PAYING
# FOR A CONFIGURE AND A 102-OBJECT REFERENCE BUILD. `findShippedConfig` walks up
# from the CWD unless DSS_CONFIG_ROOT says otherwise, so an unpinned run pairs
# the measured binary with whichever config tree sits above wherever it was
# launched — and a measurement of "this binary" has to say which config it read.
# ⚠ THIS CHECK RUNS HERE, NOT IN THE .sh, and that is not duplication: the .sh
# derives inside WSL where a Windows `dsscp.exe` is not native, so it
# skips the probe BY NAME and this host runs it. Same implementation
# (speedtest1_bench.py --preflight-dss), different host.
# ✔MEASURED 2026-08-21: a two-day-stale build against the current config refuses
# with `unknown key 'templateLabelRule' in 'assembly'` — correct, well-named, and
# three minutes too late without this.
$CfgRoot = Join-Path $DssSrc 'src\dss-config'
if (-not (Test-Path $CfgRoot)) {
  Die "no dss config root at $CfgRoot`n      Pass -DssSrc pointing at the checkout the compiler was built from."
}
# ★★ `-Target` IS PASSED, and its absence is what let this check pass while the
# measurement failed. ✔MEASURED 2026-08-26: the probe compiled with no --target,
# validating whatever dsscp defaults to, so a `build/rel` that predated the
# `encoding.registerClass` vocabulary printed `preflight: OK` and then refused
# `x86_64:pe64-x86_64-windows-exec` as an unknown key minutes later. macOS hit the
# mirror image in the same run. A control must match the TARGET — the twin passes
# the same value through `--preflight-target`, one implementation, one rule.
& $Python.Source $BenchCore --preflight-dss $Dss --config-root $CfgRoot --preflight-target $Target
if ($LASTEXITCODE -ne 0) {
  Die @"
the dss pre-flight refused (its diagnostic is above). Nothing is measured against
      a compiler that cannot compile three lines. Rebuild it:
        scripts\local-build\local-build.ps1 -Tree rel
"@
}

# The reference C compiler, NATIVE. A MinGW gcc that targets Windows is the
# comparable one; a WSL gcc would be a different OS, a different CRT and a
# different filesystem, which is three variables where the benchmark wants none.
if (-not $Cc) {
  $g = Get-Command gcc.exe -ErrorAction SilentlyContinue
  if (-not $g) { $g = Get-Command clang.exe -ErrorAction SilentlyContinue }
  if ($g) { $Cc = $g.Source }
}
if (-not $Cc) {
  Die @"
no native reference C compiler found (looked for gcc.exe, then clang.exe).
      Pass -Cc <path>. A benchmark with no reference is not a comparison, and a
      WSL gcc is NOT the reference for a Windows measurement: it is a different
      OS, CRT and filesystem.
"@
}
Info "reference : $Cc"
# MSVC is resolved by the shared core (vswhere + vcvarsall harvested through a
# batch file), so BOTH drivers get the same arm or the same named skip. An arm
# whose toolchain is absent is reported as SKIPPED with its reason, never dropped.
$msvcProbe = & $Python.Source $BenchCore --resolve-msvc
if ($LASTEXITCODE -eq 0) { Info 'msvc      : resolved (vswhere + vcvarsall)' }
else { Warn "msvc      : ABSENT — $(($msvcProbe | ConvertFrom-Json).reason)" }

if (-not $Out) { $Out = Join-Path $SqliteDir 'bld-dss-bench' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
if (-not $Plan) { $Plan = Join-Path $Out 'benchmark-plan.json' }
Info "output    : $Out"

# ── Step 2 — derive the subject through the POSIX half of this pair ──────────
Step '2/4  Derive the full-source subject (benchmark-speedtest1.sh --derive-only)'
Info 'SQLite configures with autosetup + make + tclsh, so the derivation runs in WSL.'
Info 'The MEASUREMENT does not: it runs natively, below.'

$shPosix     = ConvertTo-WslPath $DriverSh
$sqlitePosix = ConvertTo-WslPath $SqliteDir
$dssPosix    = ConvertTo-WslPath $Dss
$dssSrcPosix = ConvertTo-WslPath $DssSrc
$outPosix    = ConvertTo-WslPath $Out
$planPosix   = ConvertTo-WslPath $Plan
# ★★ THE NATIVE gcc IS HANDED TO THE DERIVATION EXPLICITLY, and this line is the
# whole reason the reference arm is the reference. Left to itself, the .sh
# resolves the FIRST gcc it can see — and it is running inside WSL, so that is
# `/usr/bin/gcc`: a Linux compiler, written into a Windows plan, unlaunchable by
# the native measurement. ✔MEASURED 2026-08-21, exactly that.
# ★ And even if a WSL gcc COULD be launched, it would be the wrong control: a
# different OS, a different CRT and a different filesystem is three variables
# where this benchmark wants none. A reference control must match the target.
$ccPosix     = ConvertTo-WslPath $Cc

# ⚠ THE COMMAND GOES INTO A FILE, AND THE FILE IS WHAT RUNS. A quoted heredoc
# eats backslashes and an inline `bash -c` with interpolated Windows-ish paths is
# the quoting trap this repository has been bitten by more than once. Writing the
# script and running the script has no such failure mode.
$derive = @"
set -Eeuo pipefail
bash '$shPosix' --derive-only --path-style windows \
  --sqlite-dir '$sqlitePosix' \
  --dss '$dssPosix' \
  --dss-src '$dssSrcPosix' \
  --cc '$ccPosix' \
  --out '$outPosix' \
  --plan '$planPosix' \
  --target '$Target' \
  --recipe-transform '$RecipeTransform' \
  --stack-reserve '$StackReserve' \
  --size '$Size' \
  --build-repeats '$BuildRepeats' \
  --run-repeats '$RunRepeats' \
  --jobs-arms '$JobsArms'$(if ($Testset) { " \`n  --testset '$Testset'" })
"@
$deriveSh = Join-Path $Out 'derive.sh'
[System.IO.File]::WriteAllText($deriveSh, ($derive -replace "`r`n", "`n"), (New-Object System.Text.UTF8Encoding($false)))
$deriveShPosix = ConvertTo-WslPath $deriveSh

$r = Invoke-Wsl "bash '$deriveShPosix' 2>&1"
$r.Output | ForEach-Object { Write-Host $_ }
if ($r.ExitCode -ne 0) {
  Die "the subject derivation FAILED (rc=$($r.ExitCode)). The measurement is not attempted on an unproven subject."
}
if (-not (Test-Path $Plan)) {
  Die "the derivation reported success but wrote no plan at $Plan.`n      A driver that claims a file it did not produce is the one failure this pair refuses to shrug at."
}
Pass 'subject derived (the plan carries Windows paths, translated by wslpath)'

if ($DeriveOnly) {
  Info "(-DeriveOnly: stopping before the measurement; the plan is at $Plan)"
  exit 0
}

# ── Step 3/4 — measure, natively ─────────────────────────────────────────────
Step '3/4  Measure (native Windows: dss, gcc, cl)'
& $Python.Source $BenchCore --plan $Plan `
    --json-out (Join-Path $Out 'benchmark-speedtest1.json') `
    --md-out   (Join-Path $Out 'benchmark-speedtest1.md')
$rc = $LASTEXITCODE
if ($rc -ne 0) { exit $rc }

Step '4/4  Done'
Info "raw JSON : $(Join-Path $Out 'benchmark-speedtest1.json')"
Info "markdown : $(Join-Path $Out 'benchmark-speedtest1.md')"
exit 0

# ─────────────────────────────────────────────────────────────────────────────
# TWIN PARITY (benchmark-speedtest1.sh) — a review obligation, not a gate.
# Same inputs: every flag of the .sh has a parameter here with the same meaning
# and the same default (-Size/--size, -JobsArms/--jobs-arms, ...). Same
# properties: the subject comes from the .sh itself and the numbers from
# speedtest1_bench.py, so neither is re-implemented here. Same exit codes:
# 0 measured / 1 a refusal / 2 usage / 3 no arm produced a binary.
# The ONE asymmetry is deliberate and belongs to the platform rather than to the
# pair: this driver adds the MSVC arm, because cl.exe exists on no other host —
# and even that arm is resolved by the SHARED core, so the .sh picks it up too
# when it happens to be run on Windows.
# ─────────────────────────────────────────────────────────────────────────────
