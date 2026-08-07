# Verifies build-and-test.ps1's LEG-CONTRACT logic by EXTRACTING the shipped
# functions and RUNNING them - never by re-implementing them here. A copy would
# stay green while the shipped logic was broken, which is the inert-test trap
# test-confound-scope.ps1 (this file's sibling and model) was written for.
#
# The .sh twin is test-driver-contracts.sh. Both emit the same
# `passed=N failed=N skipped=N` summary the drivers parse at Step 0.
#
# WHAT IT PINS, and the anchor each one belongs to:
#   A  Set-LegVerdict / Set-UnitNotRun   an unclassified skip is impossible
#   B  Test-LegRunSkipped                ONE run decision, both artifacts
#      [D-HARNESS-UNITS-SKIP-A-LEG-WHOSE-LAUNCHER-IT-SAYS-IS-AVAILABLE]
#   C  Read-CorpusSegment                the FIRST DIAGNOSTIC line is kept
#      [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY]
#   D  Get-LegLoaderPathVar              the loader variable is TARGET-keyed
#      [D-HARNESS-RUN-ENV-LD-LIBRARY-PATH-INERT-ON-DARWIN]
#   E  Get-AcquiredScriptLibrary         the acquisition contract field
#   F  RED-ON-DISABLE                    every guard above is BROKEN in a copy
#                                        and the pin MUST go red
#
# ★★★ WHY SECTION F ASSERTS ITS OWN MUTATIONS - see the long note in the .sh twin.
# On 2026-08-06 a mutator SILENTLY NO-OPPED and a red-on-disable demonstration
# reported green over a file it had never modified. Every mutation here is
# fail-closed on FOUR checks before the pin is re-run: the witness is UNIQUE in the
# driver; the mutant DIFFERS byte-wise; the witness is ABSENT from the mutant; and
# the mutant still PARSES, so a red can never be a syntax error in disguise.

$ErrorActionPreference = 'Stop'
$Here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$PS1    = Join-Path $Here 'build-and-test.ps1'
$LegsPy = Join-Path $Here 'harness_legs.py'
$Cat    = Join-Path $Here 'legs.json'
if (-not (Test-Path -LiteralPath $PS1)) { Write-Host "FATAL: $PS1 not found"; exit 1 }

$Work = Join-Path ([System.IO.Path]::GetTempPath()) ("dsspin-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $Work | Out-Null

$script:Passed = 0; $script:Failed = 0; $script:Skipped = 0
$script:PinFails = 0     # per-pin, reset by Green/Red
$script:Quiet = $false   # true while a pin runs against a MUTANT

function Say($m)  { if (-not $script:Quiet) { Write-Host $m } }
function Ok($m)   { $script:Passed++;  Say "  ok   $m" }
function Bad($m)  { $script:Failed++;  Write-Host "  FAIL $m" }
function Skipped($m) { $script:Skipped++; Write-Host "  skip $m" }
function Ck($label, $expected, $actual) {
  if ("$expected" -eq "$actual") {
    if ($script:Quiet) { $script:Passed++ } else { Ok $label }
  } else {
    $script:PinFails++
    if (-not $script:Quiet) { Bad $label; Write-Host "         expected: [$expected]"; Write-Host "         actual  : [$actual]" }
  }
}
# ⚠ .StartsWith / .Contains, NEVER -like. `[poisoned]` and `[]` are CHARACTER
# CLASSES in a PowerShell wildcard: `-like 'not run [poisoned]*'` matches the wrong
# thing and `-like 'not run []*'` is a hard parse error. Measured 2026-08-06.
function CkStarts($label, $prefix, $actual) {
  if ("$actual".StartsWith("$prefix")) {
    if ($script:Quiet) { $script:Passed++ } else { Ok $label }
  } else {
    $script:PinFails++
    if (-not $script:Quiet) { Bad $label; Write-Host "         [$actual] does not start with [$prefix]" }
  }
}
function CkHas($label, $haystack, $needle) {
  if ("$haystack".Contains("$needle")) {
    if ($script:Quiet) { $script:Passed++ } else { Ok $label }
  } else {
    $script:PinFails++
    if (-not $script:Quiet) { Bad $label; Write-Host "         [$haystack] does not contain [$needle]" }
  }
}
$EmDash = [char]0x2014   # the drivers use a literal em dash in these strings

# ── the extractor ───────────────────────────────────────────────────────────
function Get-Fn($driver, $name) {
  $out = New-Object 'System.Collections.Generic.List[string]'
  $inb = $false
  foreach ($l in (Get-Content -LiteralPath $driver)) {
    if (-not $inb -and $l.StartsWith("function $name(")) { $inb = $true }
    if ($inb) { [void]$out.Add($l); if ($l -eq '}') { break } }
  }
  return ($out -join "`n")
}
# ⚠ RETURNS THE TEXT; THE CALLER DOT-SOURCES IT. Dot-sourcing HERE would define the
# extracted functions in THIS function's scope, where they vanish the moment it
# returns - the pin then fails with "Set-LegVerdict is not recognized" about a
# function the driver defines perfectly well. ✔MEASURED here, and it is the same
# SCOPE-FIDELITY trap the .sh twin hit with `declare -A` inside a helper and that
# test-confound-scope.* records for the classifier: the shipped code must run in
# the scope it ships in.
function Get-Fns($driver, [string[]]$names) {
  $all = New-Object 'System.Collections.Generic.List[string]'
  foreach ($n in $names) {
    $t = Get-Fn $driver $n
    if (-not $t) {
      $script:PinFails++
      if (-not $script:Quiet) { Bad "could not extract $n from $(Split-Path -Leaf $driver) - this pin would assert over nothing" }
      return ''
    }
    [void]$all.Add($t)
  }
  return ($all -join "`n")
}

# ── the driver's ambient state, stubbed ─────────────────────────────────────
$script:LegLedger = @{}
$script:UnclassifiedVerdicts = New-Object 'System.Collections.Generic.List[string]'
$script:Warnings = New-Object 'System.Collections.Generic.List[string]'
function Warn($m) { [void]$script:Warnings.Add("$m") }
function Info($m) {}
# Die must THROW, not exit: several pins assert that the shipped code refuses, and
# a refusal that killed the test runner could not be asserted about.
function Die($m)  { throw "DIE: $m" }

# ── THE CLOSED VOCABULARY: the driver's own read, not a literal list ────────
# ★★ The .sh twin's first version stubbed this with a hand-typed list of the eight
# tokens, which was clean by construction - and therefore could not see that the
# shipped .sh read stored every token with a trailing CR on Windows. A pin that
# supplies its subject's input in a shape the subject never sees is testing the
# stub. This driver's read goes through `.Trim()` and is safe, but it is exercised
# rather than assumed, and the tokens are ASSERTED CLEAN rather than counted.
$script:VerdictVocabulary = @()
$VocabSource = 'the resolver'
if ((Get-Command python3 -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $LegsPy)) {
  $raw = & python3 $LegsPy '--catalogue' $Cat '--verdict-vocabulary' 2>&1
  if ($LASTEXITCODE -eq 0) {
    $script:VerdictVocabulary = @(@($raw) |
      Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] } |
      ForEach-Object { "$_".Trim() } | Where-Object { $_ })
  }
}
if ($script:VerdictVocabulary.Count -eq 0) { $VocabSource = 'unavailable' }

# ── green / red drivers ─────────────────────────────────────────────────────
function Green($label, $fn) {
  $script:PinFails = 0; $script:Quiet = $false
  Write-Host "-- $label"
  & $fn $PS1
  if ($script:PinFails -gt 0) { Bad "$label - $($script:PinFails) check(s) failed against the SHIPPED driver" }
}
function Red($label, $fn, $mutant) {
  $script:PinFails = 0; $script:Quiet = $true
  try { & $fn $mutant } catch { $script:PinFails++ }
  $script:Quiet = $false
  if ($script:PinFails -gt 0) {
    Ok "$label - red-on-disable CONFIRMED ($($script:PinFails) check(s) went red)"
  } else {
    Bad "$label - VACUOUS: the pin stayed GREEN against a driver whose guard was REMOVED.`n         Either the pin does not exercise the guard, or the guard is not what makes it pass."
  }
}

# ── the fail-closed mutator ─────────────────────────────────────────────────
function Invoke-Mutation($label, $out, $witness, $transform) {
  $src = Get-Content -LiteralPath $PS1
  # (0) the witness must be UNIQUE - a string that occurs twice survives its own
  #     removal, and check (2) would then report "the guard is still present"
  #     about a guard that is gone.
  $n = @($src | Where-Object { $_.Contains($witness) }).Count
  if ($n -ne 1) {
    Bad "$label - the WITNESS occurs $n time(s) in the shipped driver, needs exactly 1: [$witness]`n         A non-unique witness cannot tell 'the guard survived' from 'a copy of the text survived'."
    return $false
  }
  $mut = & $transform $src
  Set-Content -LiteralPath $out -Value $mut
  # (1) it actually changed something. Compare CONTENT, never a line count: a
  #     REPLACEMENT leaves the count identical, and that is exactly how a no-op
  #     mutation passes for a real one.
  if (($src -join "`n") -eq ($mut -join "`n")) {
    Bad "$label - THE MUTATION DID NOT LAND: the mutant is identical to the driver.`n         Refusing to report a red-on-disable over an unmodified file."
    return $false
  }
  # (2) it removed THE THING, not merely something.
  if (@($mut | Where-Object { $_.Contains($witness) }).Count -ne 0) {
    Bad "$label - the mutation changed the file but the WITNESS survives: [$witness]"
    return $false
  }
  # (3) the mutant still PARSES, so a red cannot be a syntax error in disguise.
  $errs = $null
  [void][System.Management.Automation.Language.Parser]::ParseFile($out, [ref]$null, [ref]$errs)
  if ($errs -and $errs.Count) {
    Bad "$label - the mutant does not PARSE ($($errs.Count) error(s)), so any red it produces would be a`n         syntax error rather than the missing guard. Narrow the mutation."
    return $false
  }
  return $true
}

# ═══════════════════════════════════════════════════════════════════════════
# A + B - the verdict recorders and the shared run decision
# ═══════════════════════════════════════════════════════════════════════════
function Pin-Verdicts($driver) {
  $script:LegLedger = @{}
  $script:UnclassifiedVerdicts = New-Object 'System.Collections.Generic.List[string]'
  $script:Warnings = New-Object 'System.Collections.Generic.List[string]'
  $fns = Get-Fns $driver @('Set-LegVerdict','Set-UnitNotRun','Test-LegRunSkipped')
  if (-not $fns) { return }
  . ([scriptblock]::Create($fns))

  Set-LegVerdict 'alpha' 'skipped-by-runOn' 'runOn excludes this host'
  Ck "a CLASSIFIED verdict is recorded verbatim" 'skipped-by-runOn' $script:LegLedger['alpha'].Verdict
  Ck "…and costs no unclassified count" 0 $script:UnclassifiedVerdicts.Count

  # THE MEASURED DEFECT, reproduced: on the operator's Mac at 11e97e0e the units
  # line read `not run []` while the same sentence said the launcher was present.
  Set-LegVerdict 'macho64-x86_64' '' "host darwin/arm64 cannot run x86_64:macho64-x86_64-darwin-exec natively; declared launcher 'arch -x86_64' is available"
  Ck "an EMPTY token becomes poisoned" 'poisoned' $script:LegLedger['macho64-x86_64'].Verdict
  Ck "…is counted"                     1          $script:UnclassifiedVerdicts.Count
  CkStarts "…and the detail names the harness defect" 'HARNESS DEFECT:' $script:LegLedger['macho64-x86_64'].Detail
  CkHas    "…and it warns loudly" (@($script:Warnings) -join ' ') 'HARNESS DEFECT'

  Set-LegVerdict 'beta' 'skipped-because-i-said-so' 'made up'
  Ck "a token OUTSIDE the closed vocabulary is refused" 'poisoned' $script:LegLedger['beta'].Verdict

  # the UNIT-level twin: two artifacts per leg means two ways to lose one silently.
  Set-UnitNotRun 'gamma' 'skipped-by-runOn' 'runOn excludes this host'
  Ck "a CLASSIFIED unit not-run is recorded verbatim" `
     "not run [skipped-by-runOn] $EmDash runOn excludes this host" $script:LegLedger['gamma'].UnitVerdict
  Set-UnitNotRun 'delta' '' 'launcher is available'
  CkStarts "an EMPTY unit token is refused" "not run [poisoned] $EmDash HARNESS DEFECT:" $script:LegLedger['delta'].UnitVerdict
  if ("$($script:LegLedger['delta'].UnitVerdict)".StartsWith('not run []')) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad "the EMPTY token was written through" }
  }

  # B - ONE run decision, shared by the CLI smoke gate and the unit corpus.
  function L($mode, $launcher) { [pscustomobject]@{ label = 'x'; run = [pscustomobject]@{ mode = $mode; launcher = $launcher } } }
  Ck "run decision: native   -> runnable" $false (Test-LegRunSkipped (L 'native' @()))
  Ck "run decision: launched -> runnable" $false (Test-LegRunSkipped (L 'launched' @('arch','-x86_64')))
  Ck "run decision: skip     -> skipped"  $true  (Test-LegRunSkipped (L 'skip' @()))
  # A `launched` leg with an EMPTY launcher argv is the anchor's contradiction
  # wearing its other face: runnable, and nothing to run it with.
  try {
    [void](Test-LegRunSkipped (L 'launched' @()))
    $script:PinFails++
    if (-not $script:Quiet) { Bad "a 'launched' leg with an EMPTY launcher argv did NOT refuse" }
  } catch {
    if ("$_".Contains('EMPTY launcher argv')) {
      if ($script:Quiet) { $script:Passed++ } else { Ok "a 'launched' leg with an EMPTY launcher argv REFUSES, naming the contradiction" }
    } else {
      $script:PinFails++
      if (-not $script:Quiet) { Bad "wrong refusal: $_" }
    }
  }
}

# ═══════════════════════════════════════════════════════════════════════════
# C - Read-CorpusSegment keeps the FIRST DIAGNOSTIC line
# ═══════════════════════════════════════════════════════════════════════════
function Pin-ReadSegment($driver) {
  $fns = Get-Fns $driver @('Read-CorpusSegment')
  if (-not $fns) { return }
  . ([scriptblock]::Create($fns))

  # (1) THE MEASURED PRECONDITION LOG, reproduced from the operator's Mac.
  $p = Join-Path $Work 'precond.log'
  @("Can't find a usable init.tcl in the following directories: /opt/local/lib/tcl8.6 /opt/local/lib/tcl8.6 ...",
    "This probably means that Tcl wasn't installed properly.",
    '    (procedure "tclInit" line 61)',
    '    invoked from within',
    '"interp create tinterp"') | Set-Content -LiteralPath $p
  $r = Read-CorpusSegment $p
  Ck "precondition log: the diagnostic is captured verbatim" `
     "Can't find a usable init.tcl in the following directories: /opt/local/lib/tcl8.6 /opt/local/lib/tcl8.6 ..." $r.Diagnostic
  Ck "precondition log: ZERO files completed" 0  $r.Completed.Count
  Ck "precondition log: no summary line"      '' $r.Summary

  # (2) a HEALTHY segment must parse EXACTLY as before - the new rule must not
  #     consume a line an older rule needed, and must not invent a diagnostic.
  $h = Join-Path $Work 'healthy.log'
  @('select1-1.1... Ok','select1-1.2... Ok','Time: select1.test 42 ms','misc7-7.0... Ok',
    'Time: misc7.test 11 ms','0 errors out of 192 tests on host Darwin 64-bit') | Set-Content -LiteralPath $h
  $r = Read-CorpusSegment $h
  Ck "healthy log: NO diagnostic is invented" '' $r.Diagnostic
  Ck "healthy log: files counted"             2  $r.Completed.Count
  Ck "healthy log: last file"        'misc7.test' $r.Completed[$r.Completed.Count - 1]
  # ★ THE WHOLE LINE, HOST SUFFIX AND ALL — CORRECTED TF-C124, and this pin used
  # to encode the defect. Read-CorpusSegment recorded only the MATCHED SUBSTRING
  # ('0 errors out of 192 tests'), so this driver dropped the trailing
  # ' on host Darwin 64-bit' that build-and-test.sh's parse_segment keeps
  # (`summary=$0`) and that $summaryText's own comment claims is carried "byte
  # for byte". Found by the new mirror verifier's DIFFERENTIAL battery
  # (harness_legs.py --check-regions) on its first complete run — not by review,
  # and not by this file, which was asserting the wrong side of the divergence.
  # D-HARNESS-CORPUS-ENGINE-MIRROR-CLAIMS-A-VERIFIER-THAT-DOES-NOT-EXIST.
  Ck "healthy log: summary is the WHOLE line, not just the counts" `
     '0 errors out of 192 tests on host Darwin 64-bit' $r.Summary
  Ck "healthy log: ' Ok' tally"      3  $r.OkLines

  # (3) a GENUINE mid-corpus crash: files completed AND a diagnostic exists, so
  #     the precondition branch cannot fire on it.
  $c = Join-Path $Work 'crash.log'
  @('select1-1.1... Ok','Time: select1.test 42 ms','swarmvtabfault-1.1-oom-persistent.143...',
    'child process exited abnormally','    (procedure "do_test" line 12)') | Set-Content -LiteralPath $c
  $r = Read-CorpusSegment $c
  Ck "crash log: files completed > 0" 1 $r.Completed.Count
  Ck "crash log: the last test is named" 'swarmvtabfault-1.1-oom-persistent.143' $r.LastTest
  Ck "crash log: a diagnostic is captured too" 'child process exited abnormally' $r.Diagnostic
}

# ═══════════════════════════════════════════════════════════════════════════
# D + E - the loader variable, and the acquisition contract field
# ═══════════════════════════════════════════════════════════════════════════
function Pin-LoaderVar($driver) {
  $planFile = Join-Path $Work 'plan.json'
  if (-not (Test-Path -LiteralPath $planFile)) {
    Skipped "D: Get-LegLoaderPathVar - no python3/harness_legs.py, so no REAL resolved plan"
    return
  }
  $fns = Get-Fns $driver @('Get-LegLoaderPathVar','Get-AcquiredScriptLibrary')
  if (-not $fns) { return }
  . ([scriptblock]::Create($fns))
  # ⚠ NOT `$plan` when the parameter is `[string]$Plan`: PowerShell variable names
  # are CASE-INSENSITIVE, so assigning a parsed object to a `[string]`-typed
  # parameter COERCES IT TO A STRING, `.legs` becomes $null, and `@($null).Count`
  # is 1 - a pin asserting over one phantom leg instead of five. Measured twice.
  $resolved = Get-Content -Raw -LiteralPath $planFile | ConvertFrom-Json
  # ★ A PIN THAT ASSERTS OVER ZERO SUBJECTS PASSES WHILE PROVING NOTHING.
  if (@($resolved.legs).Count -lt 5) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad "the plan has $(@($resolved.legs).Count) leg(s), expected at least 5 - this pin would assert over nothing" }
    return
  }
  $want = @{ 'elf64-x86_64' = 'LD_LIBRARY_PATH'; 'elf64-arm64' = 'LD_LIBRARY_PATH'
             'pe64-x86_64' = 'PATH'; 'macho64-arm64' = 'DYLD_LIBRARY_PATH'
             'macho64-x86_64' = 'DYLD_LIBRARY_PATH' }
  foreach ($leg in $resolved.legs) {
    if (-not $want.ContainsKey($leg.label)) { continue }
    Ck "loader var: $($leg.label) ($($leg.format))" $want[$leg.label] (Get-LegLoaderPathVar $leg)
  }
  # A plan that contradicts ITSELF about the target OS must refuse, never pick one.
  $bad = $resolved.legs[0].PSObject.Copy()
  $bad.format = 'macho64-arm64-darwin-exec'
  try {
    [void](Get-LegLoaderPathVar $bad)
    $script:PinFails++
    if (-not $script:Quiet) { Bad "a self-contradicting plan did NOT refuse" }
  } catch {
    if ("$_".Contains('disagrees with itself')) {
      if ($script:Quiet) { $script:Passed++ } else { Ok "a self-contradicting plan REFUSES, naming the contradiction" }
    } else {
      $script:PinFails++
      if (-not $script:Quiet) { Bad "wrong refusal: $_" }
    }
  }
  # E - the acquisition contract field, over a REAL acquisition_record.
  $acqFile = Join-Path $Work 'acq.json'
  if (-not (Test-Path -LiteralPath $acqFile)) {
    Skipped "E: Get-AcquiredScriptLibrary - no REAL acquisition record could be produced"
    return
  }
  $acqRec = Get-Content -Raw -LiteralPath $acqFile | ConvertFrom-Json
  Ck "scriptLibraryDir is read" '/pin/cache/tcl8.6' (Get-AcquiredScriptLibrary $acqRec 'scriptLibraryDir')
  Ck "an ABSENT field yields empty" '' (Get-AcquiredScriptLibrary ([pscustomobject]@{ cacheDir = '/x' }) 'scriptLibraryDir')
  # An EMPTY key must REFUSE, not silently look nothing up: `PSObject.Properties[$null]`
  # throws "Index operation failed; the array index evaluated to null" - the trap
  # build-and-test.ps1 already records for `$oldLegEnv[$null]`.
  try {
    [void](Get-AcquiredScriptLibrary $acqRec '')
    $script:PinFails++
    if (-not $script:Quiet) { Bad "an EMPTY contract-field name did NOT refuse" }
  } catch {
    if ($script:Quiet) { $script:Passed++ } else { Ok "an EMPTY contract-field name REFUSES" }
  }
}

# ── prerequisites for D and E ───────────────────────────────────────────────
# Produced ONCE, before any pin runs. Their absence is a SKIP with a reason, never
# a silent pass.
if ((Get-Command python3 -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $LegsPy) -and (Test-Path -LiteralPath $Cat)) {
  & python3 $LegsPy '--catalogue' $Cat '--plan' '--host-os' 'darwin' '--host-arch' 'arm64' 2>$null |
    Set-Content -LiteralPath (Join-Path $Work 'plan.json')
  if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath (Join-Path $Work 'plan.json') -ErrorAction SilentlyContinue }
  $mk = Join-Path $Work 'mkacq.py'
  @'
import importlib.util, json, sys
spec = importlib.util.spec_from_file_location("hl", sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
plan = {"leg": "macho64-arm64", "targetArch": "arm64",
        "cacheDir": "/pin/cache", "scriptLibraryDir": "/pin/cache/tcl8.6"}
print(json.dumps(m.acquisition_record(
    plan,
    libraries=[{"as": "libtcl8.6.dylib", "path": "/pin/cache/libtcl8.6.dylib"}],
    from_cache=True, remediated=[])))
'@ | Set-Content -LiteralPath $mk
  & python3 $mk $LegsPy 2>$null | Set-Content -LiteralPath (Join-Path $Work 'acq.json')
  if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath (Join-Path $Work 'acq.json') -ErrorAction SilentlyContinue }
}

Write-Host "pinning $(Split-Path -Leaf $PS1) - verdict vocabulary: $VocabSource ($($script:VerdictVocabulary.Count) token(s))"
if ($VocabSource -eq 'unavailable') {
  Skipped "A: the closed vocabulary could not be read from the resolver, so the token guard cannot be exercised"
} else {
  # ★ ASSERTED CLEAN, not merely counted: a token carrying a stray CR would make
  # every classified verdict read as a HARNESS DEFECT, and counting eight of them
  # would have reported success over it (which is what happened in the .sh twin).
  $dirty = @($script:VerdictVocabulary | Where-Object { $_ -match '\s' })
  if ($dirty.Count) { Bad "the vocabulary read produced token(s) with stray whitespace: $($dirty -join '|')" }
  else { Ok "the vocabulary read produces $($script:VerdictVocabulary.Count) CLEAN token(s)" }
}

# ═══════════════════════════════════════════════════════════════════════════
# G - THE CONFOUND SUPPLY IS KEYED ON THE LEG'S DECLARATION, NEVER ON ITS LABEL
# ═══════════════════════════════════════════════════════════════════════════
# D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG. The shipped function used
# to BE `if ($legLabel -eq 'pe64-x86_64') { return $PeEarnedConfounds }`, and no
# test in this repository ever called it. F4 below restores exactly that body and
# this pin must go red - which is the demonstration that the pin is about the
# supply and not about the matcher its sibling file already covers.
function Pin-ConfoundSupply($driver) {
  $script:ConfoundsOverride = $null
  Invoke-Expression (Get-Fn $driver 'Get-LegConfounds')
  $mk = { param($label, $c) [pscustomobject]@{ label = $label; confounds = $c } }
  Ck 'a leg gets ITS OWN declared patterns' '^walsetlk- ^busy2-' `
     ((@(Get-LegConfounds (& $mk 'elf64-x86_64' @('^walsetlk-','^busy2-')))) -join ' ')
  # THE ONE THAT WOULD HAVE CAUGHT IT: same declaration, DIFFERENT label. A
  # label-keyed supply cannot give the same answer to both.
  Ck '...and the LABEL does not change the answer' '^walsetlk- ^busy2-' `
     ((@(Get-LegConfounds (& $mk 'pe64-x86_64' @('^walsetlk-','^busy2-')))) -join ' ')
  Ck 'a leg declaring [] inherits NOTHING' '' `
     ((@(Get-LegConfounds (& $mk 'pe64-x86_64' @()))) -join ' ')
  $script:ConfoundsOverride = @('^op-1')
  Ck 'the operator override reaches EVERY leg' '^op-1' `
     ((@(Get-LegConfounds (& $mk 'elf64-x86_64' @('^walsetlk-')))) -join ' ')
  $script:ConfoundsOverride = $null
  $refused = ''
  try { [void](Get-LegConfounds ([pscustomobject]@{ label = 'nodecl' })) }
  catch { $refused = "$($_.Exception.Message)" }
  Ck 'an UNDECLARED leg REFUSES, never answers @()' $true ($refused -match 'transport defect')
}

# ═══════════════════════════════════════════════════════════════════════════
# H - A FAILED RUN-DIRECTORY OPERATION IS A VERDICT, NOT A SILENT FALLBACK
# ═══════════════════════════════════════════════════════════════════════════
# D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS. Two properties, and BOTH matter:
# an EMPTY argv prefix is a real answer (`runFilesystem: driver` - the driver does
# it natively), and a FAILING prefix must be reported rather than fallen back
# from. Falling back would put the corpus straight onto the filesystem the whole
# declaration exists to keep it off, silently.
function Pin-RunDirArgv($driver) {
  Invoke-Expression (Get-Fn $driver 'Invoke-RunDirArgv')
  $ok = Invoke-RunDirArgv 'leg' 'do nothing' @() @('/x')
  Ck 'an EMPTY prefix is a real answer (driver filesystem)' $true $ok.Ok
  Ck '...and carries no complaint' '' "$($ok.Detail)"
  # A prefix that FAILS. `cmd /c exit 3` is on every Windows box this driver
  # runs on and needs no WSL - the point under test is the rc handling, not the
  # tool.
  $bad = Invoke-RunDirArgv 'leg' 'prepare the run directory' @('cmd','/c') @('exit 3')
  Ck 'a FAILING prefix is reported, never swallowed' $false $bad.Ok
  CkHas '...naming what could not be done' "$($bad.Detail)" 'prepare the run directory'
  CkHas '...and the exit code' "$($bad.Detail)" '3'
}

Green 'A+B  the verdict recorders + the shared run decision' 'Pin-Verdicts'
Green 'C    Read-CorpusSegment keeps the first diagnostic'   'Pin-ReadSegment'
Green 'D+E  the loader variable + the acquisition field'     'Pin-LoaderVar'
Green 'G    the confound supply follows the DECLARATION'     'Pin-ConfoundSupply'
Green 'H    a failed run-dir operation is a VERDICT'         'Pin-RunDirArgv'

# ═══════════════════════════════════════════════════════════════════════════
# F - RED-ON-DISABLE. Every guard above is REMOVED in a copy; the pin must fail.
# ═══════════════════════════════════════════════════════════════════════════
Write-Host '-- F    red-on-disable (each mutation is asserted to have LANDED first)'

# F1 - the empty/unknown-token guard inside Set-LegVerdict.
$m1 = Join-Path $Work 'm1.ps1'
# ⚠ THE WITNESS IS UNIQUE TO Set-LegVerdict. `[void]$script:UnclassifiedVerdicts.Add($label)`
# looks like the obvious choice and is NOT usable: Set-UnitNotRun contains the same
# line, so it survives this mutation and the harness (correctly) refused to certify
# the removal. That refusal is the uniqueness check earning its place.
if (Invoke-Mutation 'F1 remove the Set-LegVerdict guard' $m1 'this driver recorded a verdict with an EMPTY token' {
      param($src)
      $out = New-Object 'System.Collections.Generic.List[string]'
      $skip = $false; $done = $false
      foreach ($l in $src) {
        if (-not $done -and $l -eq '  if (-not $tok -or ($script:VerdictVocabulary -notcontains $tok)) {') { $skip = $true }
        if ($skip -and $l -eq '  $script:LegLedger[$label].Verdict = $verdict') { $skip = $false; $done = $true }
        if (-not $skip) { [void]$out.Add($l) }
      }
      return $out
    }) {
  Red 'F1 an unclassified verdict token is refused' 'Pin-Verdicts' $m1
}

# F2 - the first-diagnostic capture in Read-CorpusSegment.
$m2 = Join-Path $Work 'm2.ps1'
if (Invoke-Mutation 'F2 remove the first-diagnostic capture' $m2 '$r.Diagnostic = if ($d.Length -gt 400)' {
      param($src)
      $out = New-Object 'System.Collections.Generic.List[string]'
      $skip = $false
      foreach ($l in $src) {
        if ($l -eq '    if (-not $r.Diagnostic -and $line.Trim() -and') { $skip = $true }
        if ($skip -and $l -eq '    }') { $skip = $false; continue }
        if (-not $skip) { [void]$out.Add($l) }
      }
      return $out
    }) {
  Red 'F2 the captured log first error line is surfaced' 'Pin-ReadSegment' $m2
}

# F3 - the target-OS answer for darwin: make it the ELF spelling, which is the
# original defect (dyld IGNORES LD_LIBRARY_PATH).
$m3 = Join-Path $Work 'm3.ps1'
if (Invoke-Mutation 'F3 hardcode the ELF loader variable' $m3 "'darwin'  { return 'DYLD_LIBRARY_PATH' }" {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains("'darwin'  { return 'DYLD_LIBRARY_PATH' }")) { $_.Replace("'DYLD_LIBRARY_PATH'", "'LD_LIBRARY_PATH'") } else { $_ }
      })
    }) {
  Red 'F3 the loader variable follows the TARGET' 'Pin-LoaderVar' $m3
}

# F4 - THE DEFECT ITSELF, RESTORED: key the confound supply on the LEG LABEL
# again, exactly as the shipped `Get-LegConfounds` did until this cycle. The pin
# must go red on "the LABEL does not change the answer".
$m4 = Join-Path $Work 'm4.ps1'
if (Invoke-Mutation 'F4 restore the label-keyed confound supply' $m4 '  return @($leg.confounds | Where-Object { $_ })' {
      param($src)
      return @($src | ForEach-Object {
        if ($_ -eq '  return @($leg.confounds | Where-Object { $_ })') {
          "  if (`$leg.label -eq 'pe64-x86_64') { return @('^walsetlk-','^busy2-') }"
          '  return @()'
        } else { $_ }
      })
    }) {
  Red 'F4 the confound supply follows the DECLARATION, not the label' 'Pin-ConfoundSupply' $m4
}

# F5 - make a failed run-directory operation report success, which is the silent
# fallback the declaration exists to prevent.
$m5 = Join-Path $Work 'm5.ps1'
if (Invoke-Mutation 'F5 make a failed run-dir operation look fine' $m5 "return @{ Ok = `$false; Detail = `"could not `$what in the launcher's own filesystem" {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains("return @{ Ok = `$false; Detail = `"could not `$what in the launcher's own filesystem")) {
          '    return @{ Ok = $true; Detail = '''' }'
        } else { $_ }
      })
    }) {
  Red 'F5 a failed run-dir operation is REPORTED, never swallowed' 'Pin-RunDirArgv' $m5
}

# ── G  the leftover-fixture sweep must see a LAUNCHER-HOSTED fixture ─────────
# D-HARNESS-SQLITE-PROCESS-HYGIENE-BLIND-UNDER-LAUNCHER. Under a launcher the OS
# process is the LAUNCHER's and the fixture is its ARGUMENT, so an image-name +
# path sweep enumerates nothing — and "found none" is indistinguishable from
# "cannot see". This pin reproduces the SHAPE with `cmd /c`, which is on every
# Windows box and needs no WSL: the point under test is whether the sweep reads
# the COMMAND LINE, not which emulator is installed.
function Pin-HygieneLauncher($driver) {
  Invoke-Expression (Get-Fn $driver 'Get-OurFixtureProcesses')
  # A path that does not exist on disk: the sweep must key on the ARGUMENT TEXT,
  # never on resolving the file. It is under $Work so it can collide with nothing.
  $marker = Join-Path $Work 'legdir\testfixture'
  $decoy  = Start-Process -FilePath 'cmd.exe' `
              -ArgumentList @('/c', "timeout /t 25 /nobreak >nul & rem $marker") `
              -PassThru -WindowStyle Hidden
  try {
    Start-Sleep -Milliseconds 800
    $hits = @(Get-OurFixtureProcesses $marker)
    # CONTENT, not just count: a sweep that returned some unrelated process would
    # satisfy "found >= 1" and then kill the wrong thing.
    Ck 'a launcher-hosted fixture is FOUND by its command line' $true ($hits.Count -ge 1)
    Ck '...and it is the launcher process itself' $true ($hits.Id -contains $decoy.Id)
    # The negative control: an unrelated path must match NOTHING, or the sweep is
    # not precise enough to be allowed to kill.
    $none = @(Get-OurFixtureProcesses (Join-Path $Work 'other\testfixture'))
    Ck 'an unrelated fixture path matches nothing' 0 $none.Count
  } finally {
    Stop-Process -Id $decoy.Id -Force -ErrorAction SilentlyContinue
  }
}
Green 'G    the leftover sweep sees a launcher-hosted fixture' 'Pin-HygieneLauncher'

# F6 - remove the launcher arm, leaving the original image-name+path sweep. The
# pin must go red: that is exactly the blind state the anchor describes.
$m6 = Join-Path $Work 'm6.ps1'
if (Invoke-Mutation 'F6 blind the sweep to launcher-hosted processes' $m6 'if ($cp.CommandLine.Contains($want) -or $cp.CommandLine.Contains($fixturePath)) {' {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains('if ($cp.CommandLine.Contains($want) -or $cp.CommandLine.Contains($fixturePath)) {')) {
          '      if ($false) {'
        } else { $_ }
      })
    }) {
  Red 'F6 the sweep sees a launcher-hosted fixture' 'Pin-HygieneLauncher' $m6
}

Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
Write-Host ''
Write-Host "passed=$($script:Passed) failed=$($script:Failed) skipped=$($script:Skipped)"
if ($script:Failed -ne 0) { exit 1 }
exit 0
