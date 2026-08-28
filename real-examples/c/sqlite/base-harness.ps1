# real-examples/c/sqlite/base-harness.ps1
# ─────────────────────────────────────────────────────────────────────────────
# THE SHARED POWERSHELL CORE OF THE SQLITE HARNESSES — the capability-paired
# twin of base-harness.sh.
#
# ★ READ base-harness.sh's header FIRST. It states why a shared core exists at
# all (three measured drifts between two hand-kept copies of one decision) and
# it is the authority on the recipe-derivation half. This file does not repeat
# that reasoning; it states what is DIFFERENT on the PowerShell side.
#
# ★ WHY THERE IS NO RECIPE DERIVATION IN THIS FILE, AND WHY THAT IS NOT A
#   MISSING CAPABILITY. build-and-test.ps1 has never derived a recipe in
#   PowerShell. `make -n`, `ar`, `sed` and the sqlite build tree all live on the
#   POSIX side, so the .ps1 has always shipped a bash `$deriveScript` and run it
#   (through WSL on a Windows host, directly on a native POSIX one). That script
#   now SOURCES base-harness.sh, so the .ps1's derivation is not a reimplementation
#   of the .sh's — it is literally the same functions in the same file. Writing a
#   PowerShell `make -n` parser to make this file look symmetric would recreate
#   exactly the second implementation the shared core exists to delete.
#
#   The capability pairing that MATTERS is therefore: whatever ONE DRIVER can do,
#   the OTHER DRIVER can do. Both derive recipes (via base-harness.sh), both read
#   artifacts back (this file / base-harness.sh), both build any declared
#   artifact for any declared leg, both run the CLI smoke gate (cli-smoke.py).
#
# ★ WHAT IS HERE is what the PowerShell driver does IN PowerShell: reading the
#   compiler's artifact report, invoking the shared manifest generator, driving
#   the compiler process, and the per-(leg, artifact) verdict ledger.
#
# Every function is `*-Dss*`-named and depends on NOTHING build-and-test.ps1
# defines — it must not call Die/Warn/Info, because a shared core that killed the
# process would take the caller's per-leg verdict with it. Failures are returned
# as objects the caller renders in its own vocabulary.
# ─────────────────────────────────────────────────────────────────────────────

# ★ NO `Set-StrictMode` HERE, DELIBERATELY — AND IT IS NOT AN OVERSIGHT.
# This file is DOT-SOURCED, so `Set-StrictMode` would not scope to it: it would
# apply to build-and-test.ps1's ENTIRE 3,700-line scope, silently changing the
# semantics of code this file has nothing to do with. Under `-Version Latest` an
# absent hashtable key stops returning $null and property access on $null throws
# — and the driver depends on exactly that leniency in load-bearing places it
# documents (see its `$acquiredLibs = @($LegLibs[$lbl].Acquired | Where-Object …)`
# note: "a hashtable returns $null for an absent key"). A shared core that
# reaches out and re-specifies its caller's language is the same class of
# overreach as one that calls `exit` on the caller's behalf, which this file also
# refuses to do.

# Matches DSS_BASE_HARNESS_VERSION in base-harness.sh. Bump BOTH when a contract
# changes, so a driver paired with a stale copy fails loud instead of silently
# losing a capability — the exact failure mode this pair exists to end.
#
# 2 — the .sh half gained the drop ledger; this half gained a --self-test and
#     Invoke-DssBuild's failure statements became assertable. The two halves
#     carry ONE version number on purpose: a driver reaching a version-1 pair
#     would be missing capabilities on both sides.
$script:DssBaseHarnessVersion = 2

# ─────────────────────────────────────────────────────────────────────────────
# WHAT DID THE COMPILER ACTUALLY WRITE? ASK IT, DO NOT GUESS.
# ★ ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING
#
# This driver used to carry
#
#     $sfx = if ($fmt -like 'pe*') { '.exe' } else { '' }
#
# whose own comment claimed the suffix was "DERIVED FROM THE OBJECT FORMAT, never
# hardcoded". That was true of the intent and false of the code: it matched a
# format-NAME PREFIX rather than the closed format enum, and every non-`pe*`
# format fell through to '' — wrong for `.dll`, `.so`, `.dylib`, `.a` and `.lib`.
# Its .sh sibling carried NO suffix logic at all. DSS owns that table
# (`TargetSpec::outputExtension`, keyed on the closed object-format enum) and
# src/program/program.cpp:216 reports every artifact it commits:
#
#     dsscp: artifact <targetSpec> <absolute path>
#
# A target spec cannot contain whitespace (DSS refuses one that does), so the
# path is the whole REMAINDER of the line and an output dir with a space in it
# survives.
# ─────────────────────────────────────────────────────────────────────────────

# Get-DssReportedArtifacts — every DISTINCT path reported for <spec>, in order.
function Get-DssReportedArtifacts($compileLog, $spec) {
  if (-not (Test-Path -LiteralPath $compileLog)) { return @() }
  $marker = "dsscp: artifact $spec "
  $seen = [System.Collections.Generic.List[string]]::new()
  foreach ($l in (Get-Content -LiteralPath $compileLog)) {
    if ("$l".StartsWith($marker, [System.StringComparison]::Ordinal)) {
      $p = "$l".Substring($marker.Length)
      if (-not $seen.Contains($p)) { [void]$seen.Add($p) }
    }
  }
  return @($seen)
}

# Get-DssReportedArtifact -> @{ Ok; Path; Error }
#
# ★ THE SEAM THIS CLOSES, AND WHY "LAST WINS" WAS NOT SAFE ANY MORE.
# The old rule was "select by (log, spec), take the LAST match", written when a
# re-run could append to a log and an EARLIER build's artifact must not be
# resurrected. It silently assumes ONE artifact per (log, spec) — true while the
# harness built exactly one thing per leg, and FALSE the moment a second
# `--project` invocation for the same target lands in the same log, which is
# what adding the CLI does. "Last wins" would then hand the caller its SIBLING's
# binary with no diagnostic at all.
#
# THE FIX HAS TWO HALVES, and it needs both:
#   1. STRUCTURAL — the caller gives each artifact its OWN compile log and its
#      OWN `--output` directory, so the ambiguity cannot arise. That is the real
#      answer; this function only has to notice if it ever does.
#   2. FAIL-LOUD HERE — two DIFFERENT paths for one spec in one log is an ERROR,
#      not an arbitrary pick. The legitimate re-run case (the same path reported
#      again) still passes, because the comparison is over DISTINCT paths.
#
# A path that will not resolve is returned AS REPORTED — the caller's existence
# check renders that verdict and has to be able to quote what the compiler
# claimed. The compiler prints forward slashes on every host (this codebase's
# path-for-display convention), so a resolvable path is canonicalised to the host
# spelling that the process sweep, Split-Path and every log line use.
function Get-DssReportedArtifact($compileLog, $spec) {
  $all = @(Get-DssReportedArtifacts $compileLog $spec)
  if ($all.Count -eq 0) {
    return @{ Ok = $false; Path = $null; Error = "the build reported NO artefact for $spec (expected a 'dsscp: artifact $spec <path>' line in $compileLog)" }
  }
  if ($all.Count -gt 1) {
    return @{ Ok = $false; Path = $null; Error = @"
the build log '$compileLog' reports $($all.Count) DIFFERENT artefacts for the target spec '$spec':
      $($all -join "`n      ")
      Refusing to guess which one was meant. Each artefact must be built into its OWN
      --output directory with its OWN compile log — see
      D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING.
"@ }
  }
  $hit = $all[0]
  try   { return @{ Ok = $true; Path = (Resolve-Path -LiteralPath $hit -ErrorAction Stop).Path; Error = $null } }
  catch { return @{ Ok = $true; Path = $hit; Error = $null } }
}

# ─────────────────────────────────────────────────────────────────────────────
# BUILD ONE ARTIFACT
# ─────────────────────────────────────────────────────────────────────────────

# New-DssManifest -> @{ Ok; Output; Error }
#
# A thin, ORDERED wrapper over the ONE manifest generator both drivers already
# share (gen-pe64-manifest.py). It is here so the ARGUMENT SET a caller must
# remember is itself shared — the generator having one implementation does not
# help if two call sites disagree about which flags matter.
#
# $LibArgv is passed through as TOKENS, never re-spelled: a resolved library may
# carry a declared runtime identity (`<path>=<import-name>`) and this file must
# not know that vocabulary (D-FFI-DECLARED-IMPORT-NAME).
#
# $RecipeTransform / $StackReserve are OPTIONAL — pass $null to omit the flag, so
# a generator too old to accept them still gets a byte-for-byte legacy
# invocation (build-and-test.ps1's Test-LegManifestBlockers is what decides
# whether omitting them is honest for a given leg).
function New-DssManifest {
  param(
    [Parameter(Mandatory)] $Python,
    [Parameter(Mandatory)] $GenPy,
    [Parameter(Mandatory)] $Output,
    [Parameter(Mandatory)] $ArtifactName,
    [Parameter(Mandatory)] $Spec,
    [Parameter(Mandatory)] $TusFile,
    [Parameter(Mandatory)] $IncludesFile,
    [Parameter(Mandatory)] $DefinesFile,
    $LibArgv = @(),
    $RecipeTransform = $null,
    $StackReserve = $null
  )
  $genArgs = @(
    $GenPy,
    '--tus',      $TusFile,
    '--includes', $IncludesFile,
    '--defines',  $DefinesFile,
    '--target',   $Spec
  ) + @($LibArgv) + @('--artifact-name', $ArtifactName)
  if ($null -ne $RecipeTransform) { $genArgs += @('--recipe-transform', "$RecipeTransform") }
  if ($null -ne $StackReserve)    { $genArgs += @('--stack-reserve',    "$StackReserve") }
  $genArgs += @('--output', $Output)
  $out = & $Python @genArgs 2>&1
  if ($LASTEXITCODE -ne 0) {
    return @{ Ok = $false; Output = $out; Error = "manifest generation failed: $(($out | ForEach-Object { "$_" }) -join ' / ')" }
  }
  return @{ Ok = $true; Output = $out; Error = $null }
}

# Invoke-DssBuild -> @{ Ok; Path; ErrCount; TimeSuffix; Error; Log }
#
# dsscp RETURNS EXIT 0 EVEN ON FATAL ERRORS, so the verdict is taken
# from `error[` in the log PLUS the artefact the build itself reported — never
# from the process exit status. The three failure statements are kept DISTINCT
# because they have three different remedies: diagnostics were emitted / the
# build was silent AND claimed nothing / the build claimed a file that is not
# there. The middle one is the genuinely interesting case — a compiler that
# returned quietly having written nothing — and it is no longer reachable by
# merely mis-spelling a file name.
function Invoke-DssBuild {
  param(
    [Parameter(Mandatory)] $DssBin,
    [Parameter(Mandatory)] $Manifest,
    [Parameter(Mandatory)] $Config,
    [Parameter(Mandatory)] $OutputDir,
    [Parameter(Mandatory)] $Log,
    [Parameter(Mandatory)] $Spec
  )
  & $DssBin --project $Manifest --config="$Config" --output $OutputDir --time *>&1 |
    Tee-Object -FilePath $Log | Out-Null
  $errCount = (Select-String -Path $Log -Pattern 'error\[' -AllMatches).Count
  $ctime = (Get-Content $Log | Select-String -Pattern 'compile time (\S+)' | Select-Object -Last 1)
  $sfx = if ($ctime) { "  ($($ctime.Matches[0].Value))" } else { '' }
  if ($errCount -gt 0) {
    return @{ Ok = $false; Path = $null; ErrCount = $errCount; TimeSuffix = $sfx; Log = $Log
              Error = "$errCount error[...] diagnostic(s)" }
  }
  $rep = Get-DssReportedArtifact $Log $Spec
  if (-not $rep.Ok) {
    return @{ Ok = $false; Path = $null; ErrCount = 0; TimeSuffix = $sfx; Log = $Log
              Error = "0 error[...] and $($rep.Error)" }
  }
  if (-not (Test-Path -LiteralPath $rep.Path)) {
    return @{ Ok = $false; Path = $rep.Path; ErrCount = 0; TimeSuffix = $sfx; Log = $Log
              Error = "0 error[...] but the artefact the build REPORTED is not there: $($rep.Path)" }
  }
  return @{ Ok = $true; Path = $rep.Path; ErrCount = 0; TimeSuffix = $sfx; Log = $Log; Error = $null }
}

# ─────────────────────────────────────────────────────────────────────────────
# ARTIFACT VERDICT LEDGER
#
# One artifact on one leg gets ONE verdict, and Step 9 must be able to prove that
# every (leg, artifact) pair it declared reached one. "Silence about a unit is a
# harness bug" applies per ARTIFACT now that there is more than one, and a ledger
# keyed only by leg cannot express "the fixture built and the CLI did not".
# ─────────────────────────────────────────────────────────────────────────────
$script:DssArtifactVerdict = @{}

function Set-DssArtifactVerdict($leg, $artifact, $verdict, $detail) {
  $script:DssArtifactVerdict["$leg/$artifact"] = @{ Verdict = $verdict; Detail = $detail }
}
function Get-DssArtifactVerdict($leg, $artifact) {
  if ($script:DssArtifactVerdict.ContainsKey("$leg/$artifact")) { return $script:DssArtifactVerdict["$leg/$artifact"] }
  return $null
}
# Assert-DssArtifactVerdicts -> the legs with NO verdict for <artifact>.
# The inert-instrument guard: a ledger nobody filled in must never read as clean.
#
# ★ IT SHIPPED WITH ZERO CALL SITES, which is the joke writing itself: the guard
# against a silent ledger was the silent thing. build-and-test.ps1's Step 9 now
# calls it over the SELECTED legs and fails the run on any hole — see the note
# there, and dss_bh_assert_verdicts in base-harness.sh for the .sh twin.
function Assert-DssArtifactVerdicts($artifact, $legs) {
  $missing = @()
  foreach ($l in @($legs)) {
    if (-not $script:DssArtifactVerdict.ContainsKey("$l/$artifact")) { $missing += $l }
  }
  return $missing
}

# ─────────────────────────────────────────────────────────────────────────────
# SELF-TEST — red-on-disable, by construction.
#
# ★ WHY THIS HAD TO EXIST. base-harness.sh carries ~40 assertions over exactly
# the seams whose wrong answers are SILENT, and this file had NONE — while being
# a genuine SECOND implementation, in another language, of the artifact
# read-back, the build verdict and the verdict ledger. "Both drivers reach one
# decision" is only true of the parts that literally share base-harness.sh; the
# parts written twice are only kept honest by being tested twice.
#
# Runs ONLY when this file is EXECUTED, never when it is dot-sourced, so the
# driver pays nothing for it and the ledger it mutates is never a live one:
#   pwsh base-harness.ps1 --self-test
# ─────────────────────────────────────────────────────────────────────────────
$script:DssStPass = 0
$script:DssStFail = 0
function Assert-DssStEq($label, $want, $got) {
  if ("$want" -eq "$got") { Write-Host ("  [PASS] {0}" -f $label); $script:DssStPass++ }
  else { Write-Host ("  [FAIL] {0}`n         want: {1}`n         got : {2}" -f $label, $want, $got); $script:DssStFail++ }
}

function Invoke-DssBaseHarnessSelfTest {
  $T = Join-Path ([System.IO.Path]::GetTempPath()) ("dss-bh-st-" + [System.Guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path $T | Out-Null
  try {
    Write-Host ("== base-harness.ps1 --self-test (version {0}) ==" -f $script:DssBaseHarnessVersion)
    $spec = 'x86_64:elf64-x86_64-linux-exec'

    # ── 1. the artifact reader ────────────────────────────────────────────────
    $log1 = Join-Path $T 'log1'
    Set-Content -LiteralPath $log1 -Value @("noise", "dsscp: artifact $spec /out/a", "more noise")
    Assert-DssStEq "reported_artifact reads the path" "/out/a" (Get-DssReportedArtifact $log1 $spec).Path
    Assert-DssStEq "  … and reports Ok" "True" (Get-DssReportedArtifact $log1 $spec).Ok

    # A path with a SPACE: the spec is one token by construction, so the path is
    # the whole remainder of the line and must survive intact.
    $logS = Join-Path $T 'logS'
    Set-Content -LiteralPath $logS -Value @("dsscp: artifact $spec /out dir/a")
    Assert-DssStEq "reported_artifact keeps a path containing a space" "/out dir/a" (Get-DssReportedArtifact $logS $spec).Path

    # THE LEGITIMATE RE-RUN CASE: the same path reported twice COLLAPSES to one
    # answer. This is what makes the refusal below safe to be strict.
    $log2 = Join-Path $T 'log2'
    Set-Content -LiteralPath $log2 -Value @("dsscp: artifact $spec /out/a", "dsscp: artifact $spec /out/a")
    Assert-DssStEq "a REPEATED identical report collapses to ONE artefact" "1" (@(Get-DssReportedArtifacts $log2 $spec)).Count
    Assert-DssStEq "  … and is still Ok" "True" (Get-DssReportedArtifact $log2 $spec).Ok

    # ★ THE SEAM. Two DIFFERENT artefacts for one spec used to silently return
    # the LAST one — the rule this driver's private Get-ReportedArtifact still
    # implemented after the shared core had refused it. It must REFUSE.
    $log3 = Join-Path $T 'log3'
    Set-Content -LiteralPath $log3 -Value @("dsscp: artifact $spec /out/a", "dsscp: artifact $spec /out/b")
    $amb = Get-DssReportedArtifact $log3 $spec
    Assert-DssStEq "TWO DIFFERENT artefacts for one spec is REFUSED" "False" $amb.Ok
    Assert-DssStEq "  … and the refusal NAMES both paths" "True" (("$($amb.Error)" -like '*/out/a*') -and ("$($amb.Error)" -like '*/out/b*'))

    # A sibling spec's line must never be handed back for ours.
    $log4 = Join-Path $T 'log4'
    Set-Content -LiteralPath $log4 -Value @("dsscp: artifact arm64:elf64-aarch64-linux-exec /out/other")
    Assert-DssStEq "a DIFFERENT spec's artefact is not returned" "False" (Get-DssReportedArtifact $log4 $spec).Ok
    Assert-DssStEq "an absent log is a verdict, not a crash" "False" (Get-DssReportedArtifact (Join-Path $T 'nosuch') $spec).Ok

    # ── 2. the verdict ledger ─────────────────────────────────────────────────
    $script:DssArtifactVerdict = @{}
    Set-DssArtifactVerdict 'legA' 'sqlite3' 'built' 'ok'
    Assert-DssStEq "verdict round-trips" "built" (Get-DssArtifactVerdict 'legA' 'sqlite3').Verdict
    # PER ARTIFACT, NOT PER LEG — a ledger keyed only by leg cannot express "the
    # fixture built and the CLI did not", which is the reason this exists.
    Assert-DssStEq "verdicts are keyed per ARTIFACT, not per leg" "" (Get-DssArtifactVerdict 'legA' 'testfixture')
    Assert-DssStEq "assert_verdicts passes when every leg has one" "0" (@(Assert-DssArtifactVerdicts 'sqlite3' @('legA'))).Count
    Assert-DssStEq "assert_verdicts NAMES a leg with no verdict" "legB" (@(Assert-DssArtifactVerdicts 'sqlite3' @('legA','legB')) -join ' ')

    # ── 3. Invoke-DssBuild's three failure statements ─────────────────────────
    # A FAKE compiler, because the decisions under test are all about reading its
    # OUTPUT: dsscp returns 0 even on fatal errors, so the verdict comes
    # from `error[` plus the artefact the build itself reported.
    $fake = Join-Path $T 'fake-dss.ps1'
    Set-Content -LiteralPath $fake -Value @'
param([string]$project, [string]$config, [string]$output, [switch]$time)
foreach ($l in (Get-Content -LiteralPath $env:DSS_BH_ST_SAY)) { Write-Output $l }
exit 0
'@
    $say = Join-Path $T 'say.txt'; $env:DSS_BH_ST_SAY = $say
    $bLog = Join-Path $T 'build.log'
    $real = Join-Path $T 'real-artifact.bin'; Set-Content -LiteralPath $real -Value 'x'

    Set-Content -LiteralPath $say -Value @("error[F001A] something went wrong", "error[F001A] and again")
    $r = Invoke-DssBuild -DssBin $fake -Manifest 'm' -Config 'debug' -OutputDir $T -Log $bLog -Spec $spec
    Assert-DssStEq "build with diagnostics is NOT Ok" "False" $r.Ok
    Assert-DssStEq "  … and counts them" "2" $r.ErrCount

    Set-Content -LiteralPath $say -Value @("nothing interesting happened")
    $r = Invoke-DssBuild -DssBin $fake -Manifest 'm' -Config 'debug' -OutputDir $T -Log $bLog -Spec $spec
    Assert-DssStEq "0 error[ and NO artefact report is NOT Ok" "False" $r.Ok
    Assert-DssStEq "  … and says the build reported nothing" "True" ("$($r.Error)" -like '*NO artefact*')

    Set-Content -LiteralPath $say -Value @("dsscp: artifact $spec $T/not-written.bin")
    $r = Invoke-DssBuild -DssBin $fake -Manifest 'm' -Config 'debug' -OutputDir $T -Log $bLog -Spec $spec
    Assert-DssStEq "an artefact REPORTED but not on disk is NOT Ok" "False" $r.Ok
    Assert-DssStEq "  … and quotes what the compiler claimed" "True" ("$($r.Error)" -like '*not-written.bin*')

    Set-Content -LiteralPath $say -Value @("compile time 1.5s", "dsscp: artifact $spec $real")
    $r = Invoke-DssBuild -DssBin $fake -Manifest 'm' -Config 'debug' -OutputDir $T -Log $bLog -Spec $spec
    Assert-DssStEq "a real artefact with 0 error[ IS Ok (the control)" "True" $r.Ok
    Assert-DssStEq "  … and carries the compile-time suffix" "  (compile time 1.5s)" $r.TimeSuffix

    # ── 4. New-DssManifest omits the OPTIONAL flags when they are `$null` ─────
    # The contract a generator too old to accept them depends on: a byte-for-byte
    # legacy invocation, not a flag with an empty value.
    $echoPy = Join-Path $T 'echo-argv.ps1'
    Set-Content -LiteralPath $echoPy -Value 'Write-Output ($args -join " "); exit 0'
    $g = New-DssManifest -Python (Get-Command pwsh).Source -GenPy $echoPy -Output 'o' `
           -ArtifactName 'sqlite3' -Spec $spec -TusFile 't' -IncludesFile 'i' -DefinesFile 'd'
    Assert-DssStEq "manifest argv omits --recipe-transform when `$null" "False" ("$($g.Output)" -like '*--recipe-transform*')
    $g = New-DssManifest -Python (Get-Command pwsh).Source -GenPy $echoPy -Output 'o' `
           -ArtifactName 'sqlite3' -Spec $spec -TusFile 't' -IncludesFile 'i' -DefinesFile 'd' `
           -RecipeTransform 'windows-selfconfig' -StackReserve '8388608'
    Assert-DssStEq "  … and carries them when they are given" "True" (("$($g.Output)" -like '*--recipe-transform windows-selfconfig*') -and ("$($g.Output)" -like '*--stack-reserve 8388608*'))

    Write-Host ""
    Write-Host ("passed={0} failed={1}" -f $script:DssStPass, $script:DssStFail)
    return ($script:DssStFail -eq 0)
  } finally {
    Remove-Item -LiteralPath 'Env:\DSS_BH_ST_SAY' -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $T -ErrorAction SilentlyContinue
  }
}

# EXECUTED DIRECTLY (not dot-sourced) -> run the self-test. `$MyInvocation.
# InvocationName` is literally '.' for a dot-sourced script, which is how this
# file tells the two apart — the PowerShell counterpart of base-harness.sh's
# `[ "${BASH_SOURCE[0]}" = "$0" ]`. `$args` is only read on this branch: a
# dot-sourced script SHARES the caller's scope, so reading $args there would read
# the DRIVER's arguments.
if ($MyInvocation.InvocationName -ne '.') {
  if ($args -contains '--self-test') {
    if (Invoke-DssBaseHarnessSelfTest) { exit 0 } else { exit 1 }
  }
  Write-Host "base-harness.ps1 is a DOT-SOURCED library."
  Write-Host ("  usage: pwsh {0} --self-test" -f $PSCommandPath)
  exit 2
}
