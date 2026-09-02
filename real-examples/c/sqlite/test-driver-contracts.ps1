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
#   I  Push/Pop-LegLaunchEnv             a launched leg's environment ARRIVES:
#      + Get-LegLoaderSearchPath         the DECLARED launcher variables, the
#                                        loader search path in the LAUNCHER's
#                                        namespace with the TARGET's separator,
#                                        and the carrier that makes both visible
#      [D-HARNESS-PS1-CLI-SMOKE-IGNORES-THE-LEGS-DECLARED-LAUNCH-ENVIRONMENT]
#      [D-HARNESS-PS1-LOADER-SEARCH-PATH-NEVER-CROSSES-THE-LAUNCHER-BOUNDARY]
#   J  the Step-7c SMOKE CALL SITE       it applies that environment at all
#   K  the launcher-prerequisite gate    a launcher whose DECLARED needs are absent
#                                        skips the leg by name; one whose needs are
#                                        MET still REACHES the corpus; strict mode
#                                        makes the skip fatal; the Step-9 ledger
#                                        knows the token; and every smoke rc has
#                                        its own verdict (4 and 2 are not
#                                        accusations, 1 is)
#   L  the Step-7c SMOKE ARGV            --cli-target/--reference-target are
#                                        MEASURED, the reference launcher comes
#                                        from the CATALOGUE, and the host-identity
#                                        flag appears NOWHERE in the block
#   O  the dss:compiler-currency region  the LOCATED compiler is PROVED current
#                                        before the run is spent: a stale one
#                                        ABORTS naming the binary, its build time
#                                        and the rebuild command; SKIP_DSS_BUILD=1
#                                        does not exempt it; and a check that
#                                        COULD NOT RUN is a different answer from
#                                        a stale binary
#      [D-HARNESS-SQLITE-REUSES-A-RELEASE-BINARY-OLDER-THAN-THE-CONFIG-IT-IS-GIVEN]
#   F  RED-ON-DISABLE                    every guard above is BROKEN in a copy
#                                        and the pin MUST go red
# ⓘ The letters are not contiguous: H and G were already taken when K/L were added
#   (G twice), and reusing one would put two subjects behind one label.
#
# ★★★ WHY SECTION F ASSERTS ITS OWN MUTATIONS - see the long note in the .sh twin.
# On 2026-08-06 a mutator SILENTLY NO-OPPED and a red-on-disable demonstration
# reported green over a file it had never modified. Every mutation here is
# fail-closed on FOUR checks before the pin is re-run: the witness is UNIQUE in the
# driver; the mutant DIFFERS byte-wise; the witness is ABSENT from the mutant; and
# the mutant still PARSES, so a red can never be a syntax error in disguise.
#
# ⚠ CAVEAT ON CHECK (4), STATED RATHER THAN ASSUMED AWAY. The parse check is
# `[System.Management.Automation.Language.Parser]::ParseFile`, which parses with
# THE GRAMMAR OF THE POWERSHELL THAT IS RUNNING THIS FILE. It therefore certifies
# "parses under pwsh 7 on this box", not "parses under the shell that will run the
# driver" — a 5.1 host, or a future language mode, could reject a mutant this
# reports clean, and it can only see SYNTAX, never a runtime break. It is used
# anyway because it is the shape this file already uses everywhere else and a
# second convention for the same question would be worse; what it buys is the one
# thing it is here for — a red that is a missing guard rather than a typo. The .sh
# twin's `bash -n` carries the identical caveat for the identical reason.

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
function Pin-StageCapabilities($drivers) {
  # SOURCE-LEVEL, because the failure being pinned is a MISSING CALL SITE —
  # something no execution of the existing call sites can ever reveal. The
  # defect it exists for had already happened: the sqlite3 CLI recipe carried
  # -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE while the testfixture recipe,
  # derived from the same tree in the same run, carried neither.
  foreach ($drv in $drivers) {
    if (-not (Test-Path -LiteralPath $drv)) { Bad "stage capabilities: $drv exists"; continue }
    $name = Split-Path -Leaf $drv
    $text = [System.IO.File]::ReadAllLines($drv)
    # (1) no ./configure of the build dir is left bare.
    $bare = @($text | Where-Object { $_ -match 'configure"\s*>' }).Count
    Ck "$name : no ./configure invocation is left BARE" 0 $bare
    # (2) every make of a target in $BLD carries the declared OPTIONS.
    # ⚠ THE SITE COUNT IS ASSERTED FIRST, AND THAT IS NOT DECORATION: "no site
    #   without OPTIONS = 0" is satisfied by a pattern that matches NO SITES AT
    #   ALL, so a regex that silently stops matching converts this pin into a
    #   permanent green. Prove the pin can still SEE its subject before asking
    #   anything about it.
    $mkSites = @($text | Where-Object { $_ -match 'cd "\$BLD" && make ' })
    Ck "$name : the make-site matcher still finds sites (>0)" $true ($mkSites.Count -gt 0)
    $noOpts = @($mkSites | Where-Object { $_ -notmatch 'OPTIONS=' }).Count
    Ck "$name : no 'cd `$BLD && make' without OPTIONS=" 0 $noOpts
    # (3) CALL SITES, not mentions — a first cut of the .sh twin counted every
    #     occurrence of the name and reported 3-of-2 against a correct driver,
    #     because the extra hits were prose in the surrounding comments.
    $calls = @($text | Where-Object { $_ -match 'dss_bh_emit_recipe\s+\\$' }).Count
    $opts  = @($text | Where-Object { $_ -match '--make-var "OPTIONS=\$STAGE_MAKE_OPTIONS"' }).Count
    Ck "$name : the derivation matcher still finds call sites (>0)" $true ($calls -gt 0)
    Ck "$name : every dss_bh_emit_recipe call passes OPTIONS= ($calls derivation(s))" $calls $opts
  }
}

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
  # ★ Get-LegLoaderPathSpec IS THE TABLE; Get-LegLoaderPathVar is the one-line
  # reader in front of it. Extracting only the reader would fail with "the term
  # Get-LegLoaderPathSpec is not recognized" — a pin cannot assert about half a
  # mechanism.
  $fns = Get-Fns $driver @('Get-LegLoaderPathSpec','Get-LegLoaderPathVar','Get-AcquiredScriptLibrary')
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
  # ★ THE SEPARATOR IS THE SAME TABLE'S OTHER COLUMN, and it is asserted here for
  # the same reason the name is: the process that SPLITS the list is the target's
  # loader, so a driver that joined with its own host's separator handed an ELF
  # leg one directory named `C:\a;C:\b`.
  $wantSep = @{ 'elf64-x86_64' = ':'; 'elf64-arm64' = ':'; 'pe64-x86_64' = ';'
                'macho64-arm64' = ':'; 'macho64-x86_64' = ':' }
  foreach ($leg in $resolved.legs) {
    if (-not $want.ContainsKey($leg.label)) { continue }
    Ck "loader var: $($leg.label) ($($leg.format))" $want[$leg.label] (Get-LegLoaderPathVar $leg)
    Ck "loader separator: $($leg.label) ($($leg.format))" $wantSep[$leg.label] (Get-LegLoaderPathSpec $leg).Separator
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
$script:python3   = Get-Command python3 -ErrorAction SilentlyContinue
$script:XlateVerb = 'windows-to-wsl'   # the verb the measured defects were found under
$script:XlateOk   = $false
if ((Get-Command python3 -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $LegsPy) -and (Test-Path -LiteralPath $Cat)) {
  # `--environment-probes skip` DELIBERATELY: these pins are about the plan's
  # SHAPE, and measuring a clock for 20 s per invocation would put a wall-clock
  # sample inside a self-test the drivers run at STARTUP. It also exercises the
  # skip path, whose plan both drivers must REFUSE to run a corpus on.
  # [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
  & python3 $LegsPy '--catalogue' $Cat '--plan' '--environment-probes' 'skip' '--host-os' 'darwin' '--host-arch' 'arm64' 2>$null |
    Set-Content -LiteralPath (Join-Path $Work 'plan.json')
  if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath (Join-Path $Work 'plan.json') -ErrorAction SilentlyContinue }
  # ★ THE WINDOWS-HOST PLAN, and it is a SECOND file rather than a reuse: the two
  # defects pin I exists for are only REACHABLE from a host whose launcher lives in
  # another namespace, and on a darwin host every elf leg resolves to `skip` with no
  # launcher, no envTransfer and no declared launcher variable at all. Resolved by
  # the shipped resolver from the shipped catalogue — never a leg typed out here.
  & python3 $LegsPy '--catalogue' $Cat '--plan' '--environment-probes' 'skip' '--host-os' 'windows' '--host-arch' 'x86_64' 2>$null |
    Set-Content -LiteralPath (Join-Path $Work 'plan-windows.json')
  if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath (Join-Path $Work 'plan-windows.json') -ErrorAction SilentlyContinue }
  # CAN THIS HOST TRANSLATE AT ALL? The launcher-namespace half of pin I drives the
  # leg's DECLARED translator for real (no stub: a stubbed translation would assert
  # about this file's idea of /mnt/c and not about the driver's). Where the
  # translator is absent — any host that is not the Windows one these defects were
  # measured on — that half is SKIPPED WITH A REASON rather than quietly passing,
  # and its red-on-disable mutations are skipped with it, because a mutation that
  # cannot go red would be reported as VACUOUS against a correct driver.
  $probe = & python3 $LegsPy '--path-translation' $script:XlateVerb '--translate-path' 'C:\dss\pin\probe' 2>$null
  $script:XlateOk = ($LASTEXITCODE -eq 0) -and (@($probe) -join '').Trim().StartsWith('/')
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
  # ⚠ THE REGION IS A CONTRACT WITH harness_legs.py's DSS_REGIONS, WHICH NAMES THIS
  # FILE AS THE .ps1 HALF'S VERIFIER.
  # ANCHOR, ONE LINE, DO NOT WRAP (the registry guard matches the whole name):
  # D-HARNESS-CONFOUND-SUPPLY-PS1-HALF-IS-IN-NO-REGION
  # `Get-LegConfounds` used to live in NO dss: region at all while its .sh
  # twin sat inside `dss:confound-supply`, so the region machinery — whose whole job
  # is to red when a capability exists in one driver only - could not see this pair.
  # It reads the sentinels here so the claim in DSS_REGIONS is true rather than
  # credited: a comment naming an instrument that does not read the region is worse
  # than no comment, because it retires the reader's suspicion.
  $driverText = (Get-Content -LiteralPath $driver) -join "`n"
  $supplyRegion = if ($driverText -match '(?s)>>> dss:confound-supply >>>(.*?)<<< dss:confound-supply <<<') { $Matches[1] } else { '' }
  Ck 'the dss:confound-supply region is marked in this driver' $true ($supplyRegion.Length -gt 0)
  CkHas '...and the SUPPLY function is inside it' $supplyRegion 'function Get-LegConfounds'
  Invoke-Expression (Get-Fn $driver 'Get-LegConfounds')
  # + `confoundGating`, which the supply now REFUSES to proceed without: a
  # conditional row (`requires: [<environment probe>]`) is honoured only where
  # the named probe found its defect on THIS machine. 'probed' by default so
  # every assertion below keeps asking what it asked; the refusal gets its own
  # case. [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
  $mk = { param($label, $c, $g = 'probed') [pscustomobject]@{ label = $label;
            confounds = $c; confoundGating = $g } }
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
  # ★★ AND AN UNPROBED PLAN. The plan is already fail-SAFE without this (every
  # conditional row dropped, nothing excused on evidence nobody gathered) - which
  # is why the guard is easy to lose and why losing it is silent in the WRONG
  # place: a corpus run on an unprobed plan reports a broken host clock as a
  # compiler regression. [D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST]
  $ungated = ''
  try { [void](Get-LegConfounds (& $mk 'elf64-x86_64' @('^busy2-') 'unprobed')) }
  catch { $ungated = "$($_.Exception.Message)" }
  Ck 'an UNPROBED plan REFUSES rather than serving its ungated list' $true ($ungated -match "confoundGating='unprobed'")
  CkHas '...and says how to resolve a measured plan' "$ungated" '--environment-probes skip'
}

# ═══════════════════════════════════════════════════════════════════════════
# G2 - THE REFUSAL MUST STOP *THE DRIVER*, NOT JUST THROW
# ═══════════════════════════════════════════════════════════════════════════
# D-HARNESS-CONFOUND-SUPPLY-REFUSAL-DIES-IN-A-SUBSHELL.
#
# ★★ THE TWIN OF test-driver-contracts.sh's I2, AND IT EXISTS BECAUSE THE .sh HALF
# FAILED THIS. There, `die` is `exit 1` inside a COMMAND SUBSTITUTION, so the
# refusal killed a subshell and the driver ran the whole corpus with an empty
# confound list (✔MEASURED, rc 0). Every pin above captured the refusal's own
# status — a shape the production call site did not have — so none of them could
# see it. This driver's `Die` is a plain `exit 1` on a DIRECT call and therefore
# stops, but "therefore" is an inference, and an inference about whether a corpus
# runs unexcused is exactly what the .sh's cycle-long silence was made of.
#
# ⚠ THE CALL SITE IS EXTRACTED FROM THE SHIPPED DRIVER, never re-typed, and run in
# a CHILD pwsh - the property under test is "the process exits", which cannot be
# asserted from inside the process that exits.
function Pin-ConfoundSupplyStopsTheDriver($driver) {
  $pwshCmd = Get-Command pwsh -ErrorAction SilentlyContinue
  if (-not $pwshCmd) {
    Skipped 'G2: pwsh is not on PATH, so a child-process refusal cannot be observed'
    return
  }
  $text = (Get-Content -LiteralPath $driver) -join "`n"
  $region = if ($text -match '(?s)>>> dss:confound-supply >>>(.*?)<<< dss:confound-supply <<<') { $Matches[1] } else { '' }
  $callsite = @(Get-Content -LiteralPath $driver | Where-Object { $_ -match '^\$Confounds\s*=\s*@\(Get-LegConfounds \$leg\)' })
  if (-not $region -or $callsite.Count -ne 1) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad "could not extract the region ($($region.Length) chars) and the single call site ($($callsite.Count) found) from $(Split-Path -Leaf $driver) - this pin would assert over nothing" }
    return
  }
  CkHas 'the extracted call site is the real supply call' $callsite[0] 'Get-LegConfounds $leg'
  $script = Join-Path ([IO.Path]::GetTempPath()) ("dss-confound-callsite-" + [Guid]::NewGuid().ToString('N') + ".ps1")
  $body = @(
    'param($Gating)',
    "`$ErrorActionPreference = 'Stop'",
    'function Info($m) { }',
    'function Step($m) { }',
    'function Die($m) { Write-Host "DIE: $m"; exit 1 }',
    '$ConfoundsOverride = $null',
    $region,
    '$leg = [pscustomobject]@{ label = "someleg"; confounds = @("^busy2-"); confoundGating = $Gating; confoundRows = @(1) }',
    '$LegTag = "someleg"',
    $callsite[0],
    'Write-Host "REACHED-NEXT-STATEMENT size=$($Confounds.Count)"'
  ) -join "`n"
  Set-Content -LiteralPath $script -Value $body -Encoding utf8
  # ── THE REFUSAL ARM ──────────────────────────────────────────────────────
  $out = & $pwshCmd.Source -NoProfile -NonInteractive -File $script 'unprobed' 2>&1 | Out-String
  $rc = $LASTEXITCODE
  Ck 'an UNPROBED plan STOPS THE DRIVER at the real call site (rc)' 1 $rc
  CkHas '...having said why' $out "confoundGating='unprobed'"
  Ck '...and the statement AFTER the call site never ran' 'no' `
     $(if ($out -match 'REACHED-NEXT-STATEMENT') { 'yes' } else { 'no' })
  # ── THE NEGATIVE CONTROL: without it the arm above could pass for any
  #    reason at all, including a script that never ran ──────────────────────
  $out = & $pwshCmd.Source -NoProfile -NonInteractive -File $script 'probed' 2>&1 | Out-String
  $rc = $LASTEXITCODE
  Ck 'a PROBED plan runs on through the call site (rc)' 0 $rc
  CkHas '...and reaches the next statement with the leg pattern' $out 'REACHED-NEXT-STATEMENT size=1'
  Remove-Item -LiteralPath $script -Force -ErrorAction SilentlyContinue
}

# ═══════════════════════════════════════════════════════════════════════════
# N - WHY A FAILURE WAS EXCUSED IS PRINTED, NOT MERELY DECIDED
# ═══════════════════════════════════════════════════════════════════════════
# D-HARNESS-CONFOUND-SCOPE-IS-A-RUN-MODE-NOT-A-HOST.
#
# `earnedOn` failed because it is prose nothing reads, so a probe verdict nobody
# SEES is the same failure with extra steps. The report TEXT is generated once by
# harness_legs.py and its two-driver agreement is proven by DIFFERENTIAL EXECUTION
# (--check-regions, case `confound-report`); what is pinned HERE is the shipped
# .ps1's transport of it, and the refusal that stops an empty report reading as
# "this leg had nothing to excuse".
function Pin-ConfoundReport($driver) {
  Invoke-Expression (Get-Fn $driver 'Write-ConfoundReport')
  # This runner's `Info` is a no-op, so the emission would be invisible and two
  # silences would compare equal - the vacuity shape this whole file refuses.
  # PowerShell scopes functions DYNAMICALLY, so this shadows it for the callee.
  function Info($m) { "REPORT> $m" }
  $out = @(Write-ConfoundReport 'elf64-x86_64' "probe clock-realtime-steps = ABSENT`n`nrow INACTIVE: ^walsetlk-")
  # `Info` is this runner's stub, so the ANSWER is the set of lines the function
  # decided to hand it - which is exactly the capability under test.
  Ck 'a report is printed line by line, blanks skipped' 2 $out.Count
  CkHas '...the probe verdict line' ($out -join '|') 'clock-realtime-steps = ABSENT'
  CkHas '...the INACTIVE row line' ($out -join '|') 'row INACTIVE: ^walsetlk-'
  # ★ THE REFUSAL: an unexplained exclusion is not an earned one.
  $empty = ''
  try { [void](Write-ConfoundReport 'elf64-x86_64' '   ') }
  catch { $empty = "$($_.Exception.Message)" }
  Ck 'an EMPTY report REFUSES rather than printing nothing' $true ($empty -match 'EMPTY confound report')
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

# ═══════════════════════════════════════════════════════════════════════════
# I - A LAUNCHED LEG'S RUN ENVIRONMENT ACTUALLY ARRIVES
# ═══════════════════════════════════════════════════════════════════════════
# TWO MEASURED DEFECTS, 2026-08-07, both on the leg a Windows host reaches only
# through a launcher in another namespace:
#   · D-HARNESS-PS1-CLI-SMOKE-IGNORES-THE-LEGS-DECLARED-LAUNCH-ENVIRONMENT
#     elf64-arm64 CLI smoke, 14/14 assertions rc=255, `qemu-aarch64: Could not
#     open '/lib/ld-linux-aarch64.so.1'` — legs.json declares QEMU_LD_PREFIX for
#     that launcher and the smoke gate applied none of it. Reported as
#     `FAIL — CHARGED TO DSS`, i.e. the harness accusing the compiler.
#   · D-HARNESS-PS1-LOADER-SEARCH-PATH-NEVER-CROSSES-THE-LAUNCHER-BOUNDARY
#     the testfixture died `libtcl8.6.so: cannot open shared object file` with
#     libtcl8.6.so and libz.so.1 staged in the directory the variable named —
#     because the value was joined with the HOST's separator, spelled in the
#     HOST's namespace, and then never named in the carrier that would have
#     carried it across at all.
# ★ DRIVEN THROUGH THE SHIPPED FUNCTIONS ON A REAL RESOLVED PLAN, and the
# translation goes through the leg's own DECLARED translator. Nothing about the
# launcher's namespace is re-typed here: the expected value is built by calling
# the same Convert-LaunchPath the driver calls, and the assertions that would
# still pass over a wrong answer (no `;`, no `\`, absolute) are stated separately.
function Pin-LaunchEnv($driver) {
  $planFile = Join-Path $Work 'plan-windows.json'
  if (-not $script:python3 -or -not (Test-Path -LiteralPath $planFile)) {
    Skipped "I: the launcher run environment - no python3/harness_legs.py, so no REAL resolved plan"
    return
  }
  $fns = Get-Fns $driver @('Get-LegLoaderPathSpec','Get-LegLoaderPathVar','Get-LegLoaderSearchPath',
                           'Get-LegDeclaredEnvNames','Convert-LaunchPath','Get-LaunchEnvCarrierName',
                           'Resolve-LaunchEnvCarrier','Push-LegLaunchEnv','Pop-LegLaunchEnv')
  if (-not $fns) { return }
  . ([scriptblock]::Create($fns))
  $resolved = Get-Content -Raw -LiteralPath $planFile | ConvertFrom-Json
  $arm = @($resolved.legs | Where-Object { $_.label -eq 'elf64-arm64' })[0]
  $pe  = @($resolved.legs | Where-Object { $_.label -eq 'pe64-x86_64' })[0]
  if (-not $arm -or -not $pe) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad "the windows-host plan carries no elf64-arm64/pe64-x86_64 leg - this pin would assert over nothing" }
    return
  }
  # THE SUBJECT'S OWN DECLARATION FIRST. Every assertion below is about what the
  # driver does WITH these; if the catalogue ever stops declaring them the pin must
  # say so rather than keep passing over a leg that no longer poses the question.
  Ck 'the arm64 leg is LAUNCHED into another path namespace' $script:XlateVerb "$($arm.run.pathTranslation)"
  Ck '...and does NOT inherit this driver environment'       'wslenv'          "$($arm.run.envTransfer)"
  Ck '...and the CATALOGUE declares its launcher variable'   '/usr/aarch64-linux-gnu' "$($arm.run.env.QEMU_LD_PREFIX)"
  Ck 'the pe64 leg runs NATIVELY, in this driver namespace'  'none'            "$($pe.run.pathTranslation)"

  # (1) THE NATIVE LEG - a `windows` target, so `;`, identity translation, and this
  #     driver's own PATH still merged behind the leg's own directory. This is the
  #     one case the old line got right, and it must stay byte-for-byte itself.
  Ck 'native leg: its directory, the TARGET separator, then this driver PATH' `
     "C:\dss\pin\z\lib;$([Environment]::GetEnvironmentVariable('PATH'))" `
     (Get-LegLoaderSearchPath $pe @('C:\dss\pin\z\lib'))

  # (2) + (3) THE LAUNCHED LEG. Needs the leg's declared translator, which exists
  #     only on the host these defects were measured on.
  if (-not $script:XlateOk) {
    Skipped "I: the launcher-namespace half - pathTranslation '$script:XlateVerb' cannot translate on this host"
    return
  }
  $hostSide = 'C:\dss\pin\host-side\lib'
  $oldLd = [Environment]::GetEnvironmentVariable('LD_LIBRARY_PATH')
  $oldW  = [Environment]::GetEnvironmentVariable('WSLENV')
  $oldQ  = [Environment]::GetEnvironmentVariable('QEMU_LD_PREFIX')
  try {
    [Environment]::SetEnvironmentVariable('LD_LIBRARY_PATH', $hostSide)
    $dirs = @('C:\dss\pin\tcl\lib', 'C:\dss\pin\z\lib')
    $val  = Get-LegLoaderSearchPath $arm $dirs
    $want = ((@($dirs | ForEach-Object { Convert-LaunchPath "$($arm.run.pathTranslation)" $_ })) -join ':')
    Ck 'launched leg: every directory in the LAUNCHER namespace, TARGET separated' $want $val
    Ck '...so the value carries no Windows list separator' $false ($val.Contains(';'))
    Ck '...and no Windows path spelling'                   $false ($val.Contains('\'))
    Ck '...it is absolute in the launcher namespace'       $true  ($val.StartsWith('/'))
    Ck '...and a HOST value already in the variable does NOT cross' $false ($val.Contains($hostSide))
    # (3) Push: the DECLARED launcher variables, the loader path, and the carrier
    #     that is the only reason either of them is visible on the other side.
    $snap = Push-LegLaunchEnv $arm $val @() @()
    try {
      Ck 'the CATALOGUE-declared launcher variable is applied' '/usr/aarch64-linux-gnu' `
         ([Environment]::GetEnvironmentVariable('QEMU_LD_PREFIX'))
      Ck 'the loader variable holds the launcher-namespace value' $val `
         ([Environment]::GetEnvironmentVariable('LD_LIBRARY_PATH'))
      $carrier = "$([Environment]::GetEnvironmentVariable('WSLENV'))"
      CkHas 'the carrier NAMES the declared launcher variable' $carrier 'QEMU_LD_PREFIX'
      CkHas '...and NAMES the loader search variable'          $carrier 'LD_LIBRARY_PATH'
    } finally { Pop-LegLaunchEnv $snap }
    # …and puts everything back, including "was unset". An empty-but-existing
    # variable is a real setting to everything downstream.
    Ck 'Pop restores the loader variable EXACTLY'   $hostSide ([Environment]::GetEnvironmentVariable('LD_LIBRARY_PATH'))
    Ck 'Pop restores the declared launcher variable' "$oldQ"  "$([Environment]::GetEnvironmentVariable('QEMU_LD_PREFIX'))"
    Ck 'Pop restores the carrier'                    "$oldW"  "$([Environment]::GetEnvironmentVariable('WSLENV'))"

    # (4) THE UNIT CORPUS STEP'S OWN CALL SHAPE — the one with a third of a million
    #     assertions riding on it, and the one this applier was extracted out of.
    #     Its forward lists are EXERCISED here rather than assumed: an `opaque`
    #     variable crosses by name, a `driver-path` one crosses TRANSLATED, and an
    #     UNSET one must not be named at all — naming an unset variable
    #     materialises it on the other side as EMPTY-BUT-EXISTING, which for
    #     SQLITE_TEST_PATTERN_LIST is an empty FILE LIST: the tier selects zero
    #     files, tester.tcl still prints `0 errors out of 1 tests`, and the run is
    #     reported GREEN.
    $tclHost = 'C:\dss\pin\tcl\lib\tcl8.6'
    $oldTcl  = [Environment]::GetEnvironmentVariable('TCL_LIBRARY')
    $oldPat  = [Environment]::GetEnvironmentVariable('SQLITE_TEST_PATTERN_LIST')
    $oldOmit = [Environment]::GetEnvironmentVariable('QUICKTEST_OMIT')
    try {
      [Environment]::SetEnvironmentVariable('TCL_LIBRARY', $tclHost)
      [Environment]::SetEnvironmentVariable('QUICKTEST_OMIT', 'a.test,b.test')
      [Environment]::SetEnvironmentVariable('SQLITE_TEST_PATTERN_LIST', $null)
      $snap2 = Push-LegLaunchEnv $arm $val @('SQLITE_TEST_PATTERN_LIST','QUICKTEST_OMIT') @('TCL_LIBRARY')
      try {
        Ck 'a DRIVER-PATH forward crosses TRANSLATED' `
           (Convert-LaunchPath "$($arm.run.pathTranslation)" $tclHost) `
           ([Environment]::GetEnvironmentVariable('TCL_LIBRARY'))
        $c2 = "$([Environment]::GetEnvironmentVariable('WSLENV'))"
        CkHas '...and is NAMED in the carrier'          $c2 'TCL_LIBRARY'
        CkHas 'an OPAQUE forward that is SET is named'  $c2 'QUICKTEST_OMIT'
        Ck    'an UNSET forward is NOT named' $false ($c2.Contains('SQLITE_TEST_PATTERN_LIST'))
      } finally { Pop-LegLaunchEnv $snap2 }
      # Pop must undo the carrier's REWRITE too: TCL_LIBRARY went in as a host path
      # and was replaced by its translated spelling, and only a snapshot taken of
      # every name the applier assigns can put that back.
      Ck 'Pop restores a forward the carrier REWROTE' $tclHost ([Environment]::GetEnvironmentVariable('TCL_LIBRARY'))
    } finally {
      [Environment]::SetEnvironmentVariable('TCL_LIBRARY', $oldTcl)
      [Environment]::SetEnvironmentVariable('SQLITE_TEST_PATTERN_LIST', $oldPat)
      [Environment]::SetEnvironmentVariable('QUICKTEST_OMIT', $oldOmit)
    }
  } finally {
    [Environment]::SetEnvironmentVariable('LD_LIBRARY_PATH', $oldLd)
    [Environment]::SetEnvironmentVariable('WSLENV', $oldW)
    [Environment]::SetEnvironmentVariable('QEMU_LD_PREFIX', $oldQ)
  }
}

# ═══════════════════════════════════════════════════════════════════════════
# J - THE STEP-7c SMOKE GATE APPLIES THAT ENVIRONMENT AT ALL
# ═══════════════════════════════════════════════════════════════════════════
# SOURCE-LEVEL, and deliberately so: the defect is a MISSING CALL SITE, which no
# execution of the existing call sites can ever reveal — the same reason
# Pin-StageCapabilities above is source-level. Pin I proves the MECHANISM is
# right; this proves the smoke gate REACHES it, and the two together are what the
# corpus step had and the smoke step did not.
# ⚠ ORDER IS ASSERTED, NOT PRESENCE ALONE: an environment applied AFTER the child
# was spawned is exactly as absent as one never applied.
function Pin-SmokeSite($driver) {
  $text = [System.IO.File]::ReadAllLines($driver)
  $start = -1; $end = -1
  for ($i = 0; $i -lt $text.Count; $i++) {
    if ($start -lt 0) { if ($text[$i].Contains('Step "7c/9')) { $start = $i } }
    elseif ($text[$i].Contains('# ── Step 8 — ')) { $end = $i; break }
  }
  if ($start -lt 0 -or $end -lt 0) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad "could not locate the Step-7c smoke gate block in $(Split-Path -Leaf $driver) - this pin would assert over nothing" }
    return
  }
  # ★★ CODE ONLY, NEVER PROSE - AND THIS IS NOT TIDINESS. ✔MEASURED while writing
  # this pin: the driver's own comment above the spawn quoted the spawn expression,
  # the matcher found the COMMENT first, and the pin reported the environment as
  # being applied AFTER the child was launched. The same shape in reverse is how a
  # red-on-disable goes vacuously green in this repository - a comment that mentions
  # the witness survives the mutation that removes the code. A source-level pin must
  # read the code it is about and nothing that merely talks about it.
  $block = @($text[$start..($end - 1)] | Where-Object { -not $_.TrimStart().StartsWith('#') })
  $iPush = -1; $iPop = -1; $iRun = -1; $iPath = -1
  for ($i = 0; $i -lt $block.Count; $i++) {
    if ($iPush -lt 0 -and $block[$i].Contains('Push-LegLaunchEnv'))       { $iPush = $i }
    if ($iPath -lt 0 -and $block[$i].Contains('Get-LegLoaderSearchPath')) { $iPath = $i }
    if ($iRun  -lt 0 -and $block[$i].Contains('& $python3.Source @smokeArgs')) { $iRun = $i }
    if ($block[$i].Contains('Pop-LegLaunchEnv')) { $iPop = $i }
  }
  # ★ THE SUBJECT FIRST: a block matcher that stopped finding the spawn would turn
  #   every assertion below into a permanent green.
  Ck 'the smoke gate still SPAWNS the gate process' $true ($iRun -ge 0)
  Ck "the leg's DECLARED run environment is applied BEFORE that spawn" $true ($iPush -ge 0 -and $iPush -lt $iRun)
  Ck 'its loader search path comes from the SHARED builder'            $true ($iPath -ge 0 -and $iPath -le $iPush)
  Ck 'and the environment is RESTORED after the spawn'                 $true ($iPop -gt $iRun)
  # THE SAME MECHANISM AS THE CORPUS STEP, which is the property that was missing:
  # two inline copies of one decision, only one of them ever fixed. Asserted as the
  # PRESENCE OF THE CALL, argument list and all — not as a count of anything.
  Ck 'the UNIT corpus step calls the very same applier' $true `
     (@($text | ForEach-Object { $_.Trim() }) -contains '$legEnv = Push-LegLaunchEnv $leg $runEnvPath $legForwardPlain $legForwardPaths')
}

# ═══════════════════════════════════════════════════════════════════════════
# K - THE LAUNCHER-PREREQUISITE GATE
# ═══════════════════════════════════════════════════════════════════════════
# ⓘ LETTER: K, not H. H is already taken by Pin-RunDirArgv in this file (and G is
# already used twice), so a second H would put two different subjects behind one
# label in the output a reader greps.
#
# The plan says `launched` because argv[0] RESOLVED. On this host the arm64 leg's
# argv[0] is `wsl.exe` while the program that actually runs the artefact is
# `qemu-aarch64` INSIDE the distro — so the leg passed every gate this harness had
# on a box with no qemu, every unit exited 255 with no diagnostic, and fourteen of
# them were charged to DSS.
#
# ★ THE DRIVER'S REAL GATE IS EXTRACTED AND RUN. What is stubbed is the RESOLVER
# (a fake harness_legs.py answering rc 0 / 3 / 2 on demand) — never the driver's
# classification of that answer, which is the thing under test.
#
# ★★ AND THE MET CASE IS ASSERTED AS LOUDLY AS THE UNMET ONE
# (D-HARNESS-UNITS-SKIP-A-LEG-WHOSE-LAUNCHER-IT-SAYS-IS-AVAILABLE). A new gate can
# be made to pass by skipping everything; the mirror assertion — a leg whose
# prerequisites are MET is still in $RunnableLegs, through the driver's OWN
# selection expression — is what makes over-skipping a failure rather than a green.
$FakeLegsPy = Join-Path $Work 'fake_harness_legs.py'
Set-Content -LiteralPath $FakeLegsPy -Encoding utf8 -Value @'
# A stand-in for harness_legs.py --check-launcher ONLY. The outcome is chosen by
# PIN_CHECK_LAUNCHER so one pin can drive every arm; everything else is refused
# loudly rather than answered.
import json, os, sys
if "--check-launcher" not in sys.argv:
    sys.stderr.write("fake resolver: asked something other than --check-launcher: %s\n"
                     % " ".join(sys.argv[1:]))
    raise SystemExit(64)
mode = os.environ.get("PIN_CHECK_LAUNCHER", "met")
if mode == "met":
    sys.stdout.write(json.dumps({"label": "lau", "ok": True, "verdict": "",
                                 "missing": [], "uncovered": []}) + "\n")
    raise SystemExit(0)
if mode == "unmet":
    sys.stdout.write(json.dumps({
        "label": "lau", "ok": False,
        "verdict": "skipped-launcher-prerequisite-missing",
        "missing": [{"kind": "command", "path": "qemu-aarch64",
                     "provides": "PIN-PROVIDES", "why": "PIN-WHY",
                     "install": "PIN-INSTALL",
                     "probe": ["wsl.exe", "-e", "sh", "-lc", "command -v qemu-aarch64"]}],
        "uncovered": []}) + "\n")
    raise SystemExit(3)
sys.stderr.write("harness_legs.py: FATAL: the pin asked for an unreadable outcome\n")
raise SystemExit(2)
'@
function Get-DriverRegion($driver, $startsWith, $endLine) {
  # Lines from the first one STARTING WITH $startsWith through the first later
  # line EQUAL to $endLine (inclusive). Anchored on the shipped text, so a region
  # that moves is still found and a region that vanishes yields '' — which every
  # caller treats as a FAILED assertion, never a skipped one.
  $text = [System.IO.File]::ReadAllLines($driver)
  $s = -1
  for ($i = 0; $i -lt $text.Count; $i++) { if ($text[$i].StartsWith($startsWith)) { $s = $i; break } }
  if ($s -lt 0) { return '' }
  if (-not $endLine) { return $text[$s] }
  for ($j = $s + 1; $j -lt $text.Count; $j++) { if ($text[$j] -eq $endLine) { return ($text[$s..$j] -join "`n") } }
  return ''
}
function Pin-LauncherPrereq($driver) {
  if (-not (Get-Command python3 -ErrorAction SilentlyContinue)) {
    if (-not $script:Quiet) { Skipped 'K: no python3 - the launcher-prerequisite gate cannot be exercised on this host' }
    return
  }
  $fns = Get-Fns $driver @('Get-LauncherPrereqRows','Test-LauncherPrereq','Set-LegVerdict','Test-LegRunSkipped')
  if (-not $fns) { return }
  . ([scriptblock]::Create($fns))
  # The driver's ambient state the gate reads. $LegsPy points at the FAKE: the
  # subject is the driver's handling of an answer, not the resolver's answer.
  $python3 = Get-Command python3
  $LegsPy  = $FakeLegsPy
  $HostOs  = 'windows'; $HostArch = 'x86_64'
  $script:LegLedger = @{}
  $script:UnclassifiedVerdicts = New-Object 'System.Collections.Generic.List[string]'
  $script:Warnings = New-Object 'System.Collections.Generic.List[string]'
  # The driver's own success-logger, which this file does not otherwise stub. Its
  # absence made the rc-0 arm of the smoke switch THROW, and a throw inside Red()
  # counts as a red — i.e. a missing stub would have certified a red-on-disable
  # that never exercised the guard.
  function Pass($m) {}
  function NewLeg() {
    [pscustomobject]@{ label = 'lau'; spec = 'arm64:elf64-aarch64-linux-exec'
                       run = [pscustomobject]@{ mode = 'launched'
                                                launcher = @('wsl.exe','-e','qemu-aarch64')
                                                verdict = ''; detail = '' } }
  }
  # ★ THE CORPUS-ENTRY DECISION IS THE DRIVER'S OWN LINE, lifted verbatim. A
  #   re-implementation here ("if mode -eq skip") would stay green while the
  #   shipped selection rotted, which is this file's whole premise.
  $runnableExpr = Get-DriverRegion $driver '$RunnableLegs = @($BuiltLegs' ''
  if (-not $runnableExpr) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad 'could not find the $RunnableLegs selection in the driver - this pin would assert over nothing' }
    return
  }
  function Reaches($leg) {
    $BuiltLegs = @($leg)
    $RunnableLegs = @()
    Invoke-Expression $runnableExpr
    if (@($RunnableLegs).Count -gt 0) { return 'REACHED' } else { return 'SKIPPED' }
  }

  # ── (3) THE MIRROR: a MET prerequisite REACHES the corpus ─────────────────
  # FIRST, deliberately. A gate that over-skips passes every assertion below it
  # and this is the only one that can see it.
  $env:PIN_CHECK_LAUNCHER = 'met'
  $leg = NewLeg
  Ck 'a MET prerequisite answers met' 'met' (Test-LauncherPrereq $leg)
  Ck "…and leaves the plan's run mode alone" 'launched' "$($leg.run.mode)"
  Ck '…and records NO verdict for the leg'   $false     ($script:LegLedger.ContainsKey('lau'))
  Ck '★ …and the leg REACHES the corpus'     'REACHED'  (Reaches $leg)

  # ── (1) an UNMET prerequisite skips the leg, with the remedy printed ──────
  $env:PIN_CHECK_LAUNCHER = 'unmet'
  $script:Warnings = New-Object 'System.Collections.Generic.List[string]'
  $leg = NewLeg
  Ck 'an UNMET prerequisite answers unmet' 'unmet' (Test-LauncherPrereq $leg)
  Ck '…and the leg verdict is the NEW closed-vocabulary token' `
     'skipped-launcher-prerequisite-missing' "$($script:LegLedger['lau'].Verdict)"
  Ck '…the RUN verdict too, so both artifacts read the same answer' `
     'skipped-launcher-prerequisite-missing' "$($leg.run.verdict)"
  Ck '…and the run mode is downgraded to skip' 'skip' "$($leg.run.mode)"
  Ck '★ …so the corpus is NOT entered'         'SKIPPED' (Reaches $leg)
  Ck '…and the token was NOT rejected by the driver own vocabulary guard' `
     0 $script:UnclassifiedVerdicts.Count
  # THE REMEDY, not just the diagnosis. All three fields, because a row printed
  # without `install` is a row nobody acts on.
  # ⚠ THE FORMATTED SHAPE, NOT THE BARE VALUE. ✔MEASURED while building F11: the
  # unreadable-answer path echoes the report VERBATIM, so `PIN-PROVIDES` is present
  # in a mutant that never formatted a row — three of these four passed against it.
  # `provides: PIN-PROVIDES` cannot be satisfied by the JSON (`"provides": "…"`).
  $w = (@($script:Warnings) -join ' ')
  CkHas '…the missing row is named'   $w 'MISSING [command] qemu-aarch64'
  CkHas '…with what it PROVIDES'      $w 'provides: PIN-PROVIDES'
  CkHas '…with WHY it is declared'    $w 'why     : PIN-WHY'
  CkHas '…and with HOW TO INSTALL it' $w 'install : PIN-INSTALL'

  # ── an UNREADABLE answer is never assumed benign ──────────────────────────
  $env:PIN_CHECK_LAUNCHER = 'boom'
  $leg = NewLeg
  Ck 'an rc the driver has no arm for answers unreadable' 'unreadable' (Test-LauncherPrereq $leg)
  Ck '…and is POISONED, not passed' 'poisoned' "$($script:LegLedger['lau'].Verdict)"
  Ck '…and the leg still does not reach the corpus' 'SKIPPED' (Reaches $leg)

  # ── a leg the plan runs NATIVELY is not this gate's business ──────────────
  $env:PIN_CHECK_LAUNCHER = 'unmet'
  $nat = NewLeg
  $nat.run.mode = 'native'; $nat.run.launcher = @()
  Ck 'a NATIVE leg is never probed (no launcher to have prerequisites)' 'not-launched' (Test-LauncherPrereq $nat)
  Ck '…and keeps its run mode' 'native' "$($nat.run.mode)"
  Remove-Item Env:\PIN_CHECK_LAUNCHER -ErrorAction SilentlyContinue

  # ── (2) DSS_STRICT_ARM_VERDICTS=1 turns the skip into a HARD FAILURE ──────
  $envGate = Get-DriverRegion $driver '$EnvironmentalVerdicts = @(' '}'
  if (-not $envGate) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad 'could not extract the Step-9 ENVIRONMENTAL-skip gate - this pin would assert over nothing' }
  } else {
    function RunEnvGate($verdict, $strict) {
      $LegOrder = @('lau')
      $LegLedger = @{ 'lau' = @{ Verdict = $verdict } }
      $StrictVerdicts = $strict
      $failReasons = @()
      Invoke-Expression $envGate
      return $failReasons
    }
    Ck '★ under DSS_STRICT_ARM_VERDICTS=1 the launcher skip REDS the run' 1 `
       @(RunEnvGate 'skipped-launcher-prerequisite-missing' $true).Count
    CkHas '…and says which variable made it one' `
       (@(RunEnvGate 'skipped-launcher-prerequisite-missing' $true) -join ' ') 'DSS_STRICT_ARM_VERDICTS=1'
    Ck 'by default it is a warning and the run survives' 0 `
       @(RunEnvGate 'skipped-launcher-prerequisite-missing' $false).Count
    # THE CONTROL: a STRUCTURAL skip must still survive strict mode, or the
    # assertion above would be satisfied by a gate that fails on everything.
    Ck 'a STRUCTURAL skip is NOT fatal even under strict' 0 `
       @(RunEnvGate 'skipped-by-runOn' $true).Count
  }

  # ── THE STEP-9 LEDGER KNOWS THE TOKEN, AND FILES IT AS ENVIRONMENTAL ──────
  # Its `switch` is a HARDCODED MIRROR of the resolver's vocabulary. Executed, not
  # read: ✔MEASURED before this cycle, the token had no arm and fell into
  # `default` -> $vUnclassified, i.e. "★ LEDGER ACCOUNTING HOLE" over a leg that
  # had been classified perfectly well.
  $ledger = Get-DriverRegion $driver '$vRan = 0; $vExpect = 0;' '}'
  $counts = Get-DriverRegion $driver '$environmental = $vEmuMissing' ''
  $line   = Get-DriverRegion $driver '$countsLine = "$verified verified' ''
  if (-not $ledger -or -not $counts -or -not $line) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad 'could not extract the Step-9 verdict ledger - this pin would assert over nothing' }
  } else {
    function RunLedger($verdict) {
      $LegOrder = @('lau')
      $LegLedger = @{ 'lau' = @{ Verdict = $verdict } }
      $AllLegs = @([pscustomobject]@{ label = 'lau' })
      $verified = 0; $structural = 0; $skipped = 0; $accounted = 0; $countsLine = ''
      Invoke-Expression $ledger
      Invoke-Expression $counts
      Invoke-Expression $line
      return [pscustomobject]@{ Environmental = $environmental; Unclassified = @($vUnclassified).Count; Line = $countsLine }
    }
    $r = RunLedger 'skipped-launcher-prerequisite-missing'
    Ck 'the ledger counts the new token as ENVIRONMENTAL' 1 $r.Environmental
    Ck '…and leaves nothing unclassified'                 0 $r.Unclassified
    CkHas '…and the counts line NAMES the class' "$($r.Line)" 'launcher-prerequisite-missing'
    # THE CONTROL: a token no list knows must still be caught.
    $r = RunLedger 'skipped-because-i-said-so'
    Ck 'an OFF-vocabulary token is still unclassified' 1 $r.Unclassified
  }

  # ── (4) THE SMOKE GATE'S rc TABLE — every code has its OWN verdict ────────
  $rcSwitch = Get-DriverRegion $driver '  switch ($srcc) {' '  }'
  if (-not $rcSwitch) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad 'could not extract the Step-7c smoke rc switch - this pin would assert over nothing' }
  } else {
    function RunRc($rc) {
      $CliSmokeVerdict = @{}; $CliSmokeFails = 0; $lbl = 'x'; $smokeDir = 'C:\pin'
      $srcc = $rc
      Invoke-Expression $rcSwitch
      return [pscustomobject]@{ Verdict = "$($CliSmokeVerdict['x'])"; Fails = $CliSmokeFails }
    }
    $r = RunRc 4
    CkHas 'rc 4 is its own verdict' $r.Verdict 'NOT A VERDICT'
    Ck   '…and is NOT charged to DSS' $false ($r.Verdict.Contains('CHARGED TO DSS'))
    Ck   '…and still REDS the run'    1 $r.Fails
    $r = RunRc 2
    CkHas 'rc 2 is named as OUR argv defect' $r.Verdict 'HARNESS ARGV DEFECT'
    Ck   '…and is NOT charged to DSS' $false ($r.Verdict.Contains('CHARGED TO DSS'))
    Ck   '…and still REDS the run'    1 $r.Fails
    # ★ THE OTHER DIRECTION, WHICH THE ENUMERATION HAD LOST: rc 1 IS the charge.
    # Without an arm it fell into `default` and printed "NOT charged to DSS" over
    # a matched, attributed compiler failure — a false ACQUITTAL.
    $r = RunRc 1
    CkHas 'rc 1 IS charged to DSS' $r.Verdict 'CHARGED TO DSS'
    Ck   '…and is not reported as an unknown rc' $false ($r.Verdict.Contains('UNKNOWN rc'))
    $r = RunRc 0
    CkHas 'rc 0 passes' $r.Verdict 'PASS (14/14)'
    Ck   '…and costs the run nothing' 0 $r.Fails
    $r = RunRc 9
    CkHas 'a genuinely unknown rc says so, and blames nobody' $r.Verdict 'UNKNOWN rc=9'
  }
}

# ═══════════════════════════════════════════════════════════════════════════
# L - THE SMOKE GATE'S ARGV: WHAT IS MEASURED, AND WHAT IS DECLARED
# ═══════════════════════════════════════════════════════════════════════════
# SOURCE-LEVEL, the same reason Pin-SmokeSite and Pin-StageCapabilities are: the
# failure is a MISSING ARGUMENT and a WRONG SOURCE for one, which no execution of
# the existing call site can reveal.
#
# cli-smoke.py compares a DECLARED target (`--leg-spec`) against a MEASURED one
# (`--cli-target`, read out of the binary's own header). That comparison is worth
# nothing if the driver feeds it the declaration twice.
#
# ★★ AND THE ABSENCE ASSERTION IS THE FIX ITSELF. The host-identity flag that used
# to choose the reference's launcher is what made the oracle unmatched — the
# reference ran host-native x86_64 while DSS ran arm64 under qemu. Its ABSENCE from
# this block is asserted over the block's FULL TEXT, comments included: the driver
# is written so the name appears nowhere in Step 7c, precisely so this guard cannot
# be weakened into a comment-stripped view.
function Pin-SmokeArgv($driver) {
  $text = [System.IO.File]::ReadAllLines($driver)
  $start = -1; $end = -1
  for ($i = 0; $i -lt $text.Count; $i++) {
    if ($start -lt 0) { if ($text[$i].Contains('Step "7c/9')) { $start = $i } }
    elseif ($text[$i].Contains('# ── Step 8 — ')) { $end = $i; break }
  }
  if ($start -lt 0 -or $end -lt 0) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad "could not locate the Step-7c block in $(Split-Path -Leaf $driver) - this pin would assert over nothing" }
    return
  }
  $full = ($text[$start..($end - 1)] -join "`n")
  # CODE ONLY for the presence assertions - a comment that quotes an expression
  # would keep them green over a line that had been deleted.
  $code = (@($text[$start..($end - 1)] | Where-Object { -not $_.TrimStart().StartsWith('#') }) -join "`n")
  CkHas 'the Step-7c block still spawns the gate' $code '& $python3.Source @smokeArgs'
  CkHas "the leg's DECLARED spec is passed"       $code "'--leg-spec',         `$leg.spec"
  CkHas "the subject's target is MEASURED from the binary" $code '$cliId = Get-BinaryTarget $CliBuilt[$lbl]'
  CkHas '…and THAT is what --cli-target carries'  $code "'--cli-target',       `$cliId.Target"
  CkHas "the reference's target is MEASURED too"  $code '$refId = Get-BinaryTarget $RefCliWin'
  CkHas '…and is passed as --reference-target'    $code "'--reference-target', `$RefCliTarget"
  CkHas 'the reference launcher comes from the CATALOGUE, keyed on that target' `
        $code '$lft = Get-LauncherForTarget $RefCliTarget'
  CkHas '…and every token is spelled with the `=` form' $code '"--reference-launcher=$t"'
  # ★ ABSENCE, OVER THE FULL TEXT INCLUDING COMMENTS. The absence IS the fix.
  Ck 'the host-identity flag appears NOWHERE in the smoke block' $false ($full.Contains('HostNeedsWsl'))
}

# ═══════════════════════════════════════════════════════════════════════════
# M - the precondition discriminator, INCLUDING THE SILENT CRASH
# ═══════════════════════════════════════════════════════════════════════════
# [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY,
#  D-HARNESS-PRECONDITION-DISCRIMINATOR-BLIND-TO-A-SILENT-CRASH]
#
# ★★ THIS PIN HAD NO PowerShell SIDE AT ALL until now, and that absence is itself
# an instance of the defect class this suite exists for: the .sh twin has pinned
# the discriminator (its section E) since the branch was written, so on a
# Windows-only host the decision that chooses between spending ONE resume and
# spending ten was covered by no test the driver in use ever runs.
#
# ★★ THE RESILIENCE RULE IS WHAT IT PROTECTS. A fixture abort stays RECOVERABLE -
# named, resumed past, reported in the union. Only ONE shape is diverted: zero
# files completed AND the same zero-progress signature as the previous zero-file
# segment. The RESUME assertions outnumber the PRECONDITION ones, so a
# discriminator that grew greedier goes red first.
function Pin-Precondition($driver) {
  $text = @(Get-Content -LiteralPath $driver)
  # BY PREFIX, and the FIRST match: the carry line below the condition opens with
  # the same test, and the condition is first in file order. A tighter prefix
  # naming the signature conjunct would make a mutation that DELETES that conjunct
  # read as "the discriminator is missing" instead of being judged behaviourally.
  # ⚠ THE PREFIX STOPS BEFORE `-and`. ✔MEASURED while writing this: with `-and` in
  # it, the F17 mutant (which deletes the conjuncts) matched NOTHING, the pin took
  # its "would assert over nothing" exit, and the red-on-disable demonstration
  # reported ONE red from the missing-line path instead of the NINE behavioural
  # reds the mutation actually causes. The .sh twin's header records the same rule;
  # this file had to learn it by getting it wrong.
  # `StartsWith` after TrimStart, so the CARRY line - which contains the same test
  # but as the right-hand side of an assignment - cannot be picked up instead.
  $condLine = @($text | Where-Object { $_.TrimStart().StartsWith('if ($res.Completed.Count -eq 0') })
  $carryLine = @($text | Where-Object { $_.TrimStart().StartsWith('$prevZeroSig = if (') })
  $budgetLine = @($text | Where-Object { $_.TrimStart().StartsWith('$MaxResumes') })
  if ($condLine.Count -lt 1 -or $carryLine.Count -lt 1 -or $budgetLine.Count -lt 1) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad "the precondition discriminator, its carry or the resume budget is not in $(Split-Path -Leaf $driver) (cond=$($condLine.Count) carry=$($carryLine.Count) budget=$($budgetLine.Count)) - this pin would assert over nothing" }
    return
  }
  # `if (…) {` -> the boolean expression alone. From the FIRST '(' to the LAST
  # ')', so the nested [string]::Equals(...) parens survive intact.
  $e = $condLine[0].Trim()
  $expr = $e.Substring($e.IndexOf('(') + 1)
  $expr = $expr.Substring(0, $expr.LastIndexOf(')'))
  $carry = $carryLine[0].Trim()
  # The DRIVER'S OWN budget, so "rather than ten" is its number, not this file's.
  $mb = [regex]::Match($budgetLine[0], 'else \{ (\d+) \}')
  if (-not $mb.Success) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad "the resume budget default could not be read from $(Split-Path -Leaf $driver) - this pin would assert over a default it invented" }
    return
  }
  $budget = [int]$mb.Groups[1].Value
  $fns = Get-Fns $driver @('Read-CorpusSegment','Get-ZeroProgressSignature')
  if (-not $fns) { return }
  . ([scriptblock]::Create($fns))
  # DOT-SOURCED, never `&`: `&` runs a scriptblock in a CHILD scope, where the
  # carry's assignment to $prevZeroSig would vanish on return and the comparison
  # would be against a stale value forever. Same scope-fidelity rule the extractor
  # header records. [scriptblock]::Create is the PowerShell twin of the .sh pin's
  # `eval "$cond"`, and both evaluate the DRIVER'S OWN line.
  $sbCond = [scriptblock]::Create($expr)
  $sbCarry = [scriptblock]::Create($carry)
  function Takes($filesDone, $diagnostic, $prevSig, $okLines, $failMarkers, $lastTest) {
    # A STUB SHAPED LIKE Read-CorpusSegment's answer: the condition reads
    # $res.Completed.Count, so what it is handed has to have that shape.
    $res = @{ Completed = New-Object 'System.Collections.Generic.List[string]' }
    for ($i = 0; $i -lt $filesDone; $i++) { [void]$res.Completed.Add("f$i.test") }
    # THE SHIPPED DERIVATION, not a hand-set signature: the sentinel is the whole
    # fix, so a pin that supplied it itself would pass over a driver that never
    # produced one.
    $zeroSig = Get-ZeroProgressSignature $diagnostic $okLines $failMarkers $lastTest
    $prevZeroSig = $prevSig
    if (. $sbCond) { return 'PRECONDITION' } else { return 'RESUME' }
  }
  $D1 = "Can't find a usable init.tcl in the following directories: /opt/local/lib/tcl8.6 ..."
  $D2 = 'child process exited abnormally'
  $SILENT = Get-ZeroProgressSignature '' 0 0 ''
  Ck 'zero progress twice, IDENTICAL diagnostic -> PRECONDITION' 'PRECONDITION' (Takes 0 $D1 $D1 0 0 '')
  Ck 'the FIRST such abort                      -> RESUME'       'RESUME' (Takes 0 $D1 '' 0 0 '')
  Ck 'zero progress, DIFFERENT diagnostic       -> RESUME'       'RESUME' (Takes 0 $D2 $D1 0 0 '')
  Ck 'a crash AFTER completing files            -> RESUME'       'RESUME' (Takes 7 $D1 $D1 0 0 '')
  Ck 'one file completed, same diagnostic       -> RESUME'       'RESUME' (Takes 1 $D1 $D1 0 0 '')
  Ck 'zero progress, NO OUTPUT, first time      -> RESUME'       'RESUME' (Takes 0 '' '' 0 0 '')
  Ck 'SILENCE TWICE                             -> PRECONDITION' 'PRECONDITION' (Takes 0 '' $SILENT 0 0 '')
  Ck "no diagnostic but ' Ok' lines             -> RESUME"       'RESUME' (Takes 0 '' $SILENT 5 0 '')
  Ck 'no diagnostic but a FAILURE marker        -> RESUME'       'RESUME' (Takes 0 '' $SILENT 0 2 '')
  Ck 'no diagnostic but a test NAME             -> RESUME'       'RESUME' (Takes 0 '' $SILENT 0 0 'select1-1.1')
  # ── CASE SENSITIVITY, WHICH IS A TWIN PROPERTY AND NOT A DETAIL ──────────
  # PowerShell's `-eq` on strings is CASE-INSENSITIVE while the .sh twin's
  # `[[ a == b ]]` is byte-wise, so a condition written with `-eq` would answer
  # PRECONDITION here and RESUME on Linux for the same two logs.
  Ck 'two diagnostics differing only in CASE    -> RESUME' 'RESUME' (Takes 0 'Cannot Open Libtcl' 'cannot open libtcl' 0 0 '')

  # ── THE STOPPING DECISION, DRIVEN OVER REAL LOGS ─────────────────────────
  # Every moving part is the driver's: Read-CorpusSegment and
  # Get-ZeroProgressSignature are LOADED from it, the condition and the carry are
  # EXTRACTED from it, the budget is READ from it. What this contributes is the
  # loop, and the loop is what makes "stops after ONE resume" sayable at all.
  $segDir = Join-Path $Work 'seg'
  New-Item -ItemType Directory -Force -Path $segDir | Out-Null
  $silent = @(); $same = @(); $diff = @()
  foreach ($i in 1..12) {
    $s = Join-Path $segDir "silent.$i.log"
    [System.IO.File]::WriteAllBytes($s, @())            # ZERO BYTES, the measured case
    $silent += $s
    $p = Join-Path $segDir "same.$i.log"
    Set-Content -LiteralPath $p -Value $D1; $same += $p
    $q = Join-Path $segDir "diff.$i.log"
    Set-Content -LiteralPath $q -Value "child process exited abnormally in file $i"; $diff += $q
  }
  # ✔THE INPUT IS ASSERTED, NOT ASSUMED: a write that failed would make the whole
  # demonstration a statement about a file that was not there.
  Ck "the silent fixture's log really is ZERO BYTES" 0 (Get-Item -LiteralPath $silent[0]).Length
  function Drive($logs) {
    $prevZeroSig = ''; $n = 0; $r = 0; $stop = 'BUDGET-EXHAUSTED'
    foreach ($lg in $logs) {
      $n++
      $res = Read-CorpusSegment $lg
      $zeroSig = Get-ZeroProgressSignature $res.Diagnostic $res.OkLines $res.FailMarkers $res.LastTest
      if (. $sbCond) { $stop = 'PRECONDITION'; break }
      . $sbCarry
      if ($r -ge $budget) { $stop = 'BUDGET-EXHAUSTED'; break }
      $r++
    }
    return "segments=$n resumes=$r stop=$stop"
  }
  Ck "TWO SILENT SEGMENTS: the engine stops after ONE resume, not $budget" `
     'segments=2 resumes=1 stop=PRECONDITION' (Drive $silent)
  Ck 'two segments with the SAME diagnostic: same answer' `
     'segments=2 resumes=1 stop=PRECONDITION' (Drive $same)
  # THE NEGATIVE CONTROL. A genuine crash that MOVES must still spend the whole
  # budget - if this ever reads PRECONDITION the discriminator has become greedy
  # and the resilience rule is gone.
  Ck 'DIFFERENT diagnostics every time: the whole budget IS spent' `
     "segments=$($budget + 1) resumes=$budget stop=BUDGET-EXHAUSTED" (Drive $diff)
}

# ═══════════════════════════════════════════════════════════════════════════
# O - THE LOCATED COMPILER IS PROVED CURRENT BEFORE THE RUN IS SPENT
# ═══════════════════════════════════════════════════════════════════════════
# D-HARNESS-SQLITE-REUSES-A-RELEASE-BINARY-OLDER-THAN-THE-CONFIG-IT-IS-GIVEN.
#
# ★★ THIS PIN DRIVES THE SHIPPED REGION, NOT A FUNCTION LIFTED OUT OF IT. The
# defect is not "the function returns the wrong answer" - it is "the run continues
# anyway", which is a property of the CALL SITE. Same reason
# Pin-ConfoundSupplyStopsTheDriver exists. So the whole `dss:compiler-currency`
# region runs in a CHILD pwsh with a statement AFTER it that must not be reached.
#
# ★★ THE COMPILER IS A FAKE AND IT IS THE ONLY FAKE HERE. Everything else in the
# path is shipped code: this driver's own region, and the real
# `speedtest1_bench.py --preflight-dss` it calls. A pin that stubbed the pre-flight
# would be asserting over its own idea of the contract.
# ⓘ A `.cmd`, because the pre-flight launches the compiler through Python's
# subprocess: ✔MEASURED 2026-08-31, `.cmd` and `.bat` launch, an extensionless
# `#!/bin/sh` file gives `[WinError 193] not a valid Win32 application`. The .sh
# twin makes the mirror-image choice for the same measured reason.
function New-FakeDsscp($dir, $name, $line) {
  $p = Join-Path $dir "$name.cmd"
  $body = @('@echo off')
  if ($line) { $body += "echo $line" }
  $body += 'exit /b 0'
  Set-Content -LiteralPath $p -Value $body -Encoding ascii
  return $p
}
function Pin-CompilerCurrency($driver) {
  $pwshCmd = Get-Command pwsh -ErrorAction SilentlyContinue
  if (-not $pwshCmd) {
    Skipped 'O: pwsh is not on PATH, so a child-process refusal cannot be observed'
    return
  }
  $py = Get-Command python3 -ErrorAction SilentlyContinue
  if (-not $py) { $py = Get-Command python -ErrorAction SilentlyContinue }
  if (-not $py) {
    Skipped 'O: python3 is not on PATH (the driver requires it at Step 0; this host has none)'
    return
  }
  $text = (Get-Content -LiteralPath $driver) -join "`n"
  $region = if ($text -match '(?s)>>> dss:compiler-currency >>>(.*?)<<< dss:compiler-currency <<<') { $Matches[1] } else { '' }
  if (-not $region) {
    $script:PinFails++
    if (-not $script:Quiet) { Bad "could not extract the dss:compiler-currency region from $(Split-Path -Leaf $driver) - this pin would assert over nothing" }
    return
  }
  # THE SUBJECT, ASSERTED PRESENT BEFORE IT IS RUN. A region that still DEFINES
  # the assertion but no longer CALLS it would execute cleanly and prove nothing.
  CkHas 'the region defines the currency assertion' $region 'function Assert-DssCompilerCurrent('
  CkHas '...and CALLS it, so the check is not merely available' $region '$DssCurrencyOk = Assert-DssCompilerCurrent '
  $bench    = Join-Path $Here 'speedtest1_bench.py'
  $repoRoot = (Resolve-Path (Join-Path $Here '../../..')).Path
  $fakeStale = New-FakeDsscp $Work 'stale-dsscp' "error[C_InvalidSemantics]: unknown key 'restrictMarker' in 'declarations[0]'"
  $fakeOk    = New-FakeDsscp $Work 'current-dsscp' ''
  $script = Join-Path ([IO.Path]::GetTempPath()) ("dss-currency-callsite-" + [Guid]::NewGuid().ToString('N') + ".ps1")
  # Everything between the stubs and the marker is the shipped driver's own text.
  # ⚠ `Die` EXITS here exactly as it does in the driver: what is under test is that
  # the process STOPS, which cannot be observed from inside one that does not.
  $body = @(
    'param($BenchCore, $DssBin, $DssConfigRoot)',
    "`$ErrorActionPreference = 'Stop'",
    'function Info($m) { }',
    'function Warn($m) { }',
    'function Pass($m) { }',
    'function Die($m) { Write-Host "DIE: $m"; exit 1 }',
    "`$python3 = Get-Command $($py.Name) -ErrorAction SilentlyContinue",
    "`$RepoRoot = '$repoRoot'",
    "`$dssAge = '2026-08-28 09:30:36'",
    "`$DssOrigin = 'LOCATED under an eligible build root - NOT built by this run'",
    # ★ SUPPLIED, so the rebuild instruction is exercised on its PRIMARY path -
    # the tree the located binary actually came from. Left undefined, the region
    # would silently take its `build\rel` fallback and the arm below would pass
    # over a branch nobody drove.
    '$DssInfo = [pscustomobject]@{ Tree = "D:\a-tree-that-produced-this-binary" }',
    '$Legs = @(',
    '  [pscustomobject]@{ label = "elf64-x86_64"; spec = "x86_64:elf64-x86_64-linux-exec" },',
    '  [pscustomobject]@{ label = "pe64-x86_64";  spec = "x86_64:pe64-x86_64-windows-exec" })',
    $region,
    'Write-Host "REACHED-NEXT-STATEMENT ok=[$DssCurrencyOk]"'
  ) -join "`n"
  Set-Content -LiteralPath $script -Value $body -Encoding utf8
  # ── THE REMOVE-DIRECTION ARM: a STALE compiler must STOP the run ─────────
  # ✔The signature is the one measured on 2026-08-31: an `error[C_Invalid...]`
  # naming config vocabulary the binary does not know.
  $out = & $pwshCmd.Source -NoProfile -NonInteractive -File $script $bench $fakeStale $repoRoot 2>&1 | Out-String
  $rc = $LASTEXITCODE
  Ck 'a STALE compiler STOPS the driver at Step 5 (rc)' 1 $rc
  CkHas '...naming the BINARY' $out $fakeStale
  CkHas '...naming WHEN it was built' $out 'built     : 2026-08-28 09:30:36'
  CkHas '...naming how it was obtained' $out 'NOT built by this run'
  CkHas '...naming the REBUILD command' $out 'REBUILD IT: cmake --build'
  CkHas '...pointed at the tree THIS binary came from' $out 'D:\a-tree-that-produced-this-binary'
  CkHas "...and quoting the compiler's own diagnostic" $out "unknown key 'restrictMarker'"
  Ck '...and the statement AFTER the region never ran' 'no' `
     $(if ($out -match 'REACHED-NEXT-STATEMENT') { 'yes' } else { 'no' })
  # ── THE GREEN CONTROL: a CURRENT compiler must be let through ────────────
  # Without it the arm above would pass for a driver that refuses everything.
  $out = & $pwshCmd.Source -NoProfile -NonInteractive -File $script $bench $fakeOk $repoRoot 2>&1 | Out-String
  $rc = $LASTEXITCODE
  Ck 'a CURRENT compiler is let through (rc)' 0 $rc
  CkHas '...reaching the statement after the region' $out 'REACHED-NEXT-STATEMENT'
  CkHas '...having recorded BOTH declared targets, deduped and in order' $out `
     'ok=[x86_64:elf64-x86_64-linux-exec, x86_64:pe64-x86_64-windows-exec]'
  # ── SKIP_DSS_BUILD=1 IS NOT AN INSTRUCTION TO TRUST ──────────────────────
  # ★ THE ARM MOST LIKELY TO BE GOT WRONG. `SKIP_DSS_BUILD` says do not BUILD; a
  # reading of it as "do not CHECK" restores the whole defect silently, because the
  # flag's own branch is the one that reuses a binary nobody looked at.
  $env:SKIP_DSS_BUILD = '1'
  try {
    $out = & $pwshCmd.Source -NoProfile -NonInteractive -File $script $bench $fakeStale $repoRoot 2>&1 | Out-String
    $rc = $LASTEXITCODE
  } finally { Remove-Item Env:\SKIP_DSS_BUILD -ErrorAction SilentlyContinue }
  Ck 'SKIP_DSS_BUILD=1 does NOT exempt a stale binary (rc)' 1 $rc
  CkHas '...and the refusal says so in as many words' $out 'SKIP_DSS_BUILD=1 DOES NOT EXEMPT A BINARY FROM THIS CHECK'
  # ── "I COULD NOT RUN THE CHECK" IS A DIFFERENT ANSWER ────────────────────
  # ⚠ A check that reports an environment problem as a stale binary sends the
  # operator to rebuild a compiler that was never the subject. $Work is a real
  # directory with no src\dss-config under it.
  $out = & $pwshCmd.Source -NoProfile -NonInteractive -File $script $bench $fakeOk $Work 2>&1 | Out-String
  $rc = $LASTEXITCODE
  Ck 'a check that CANNOT RUN still stops the run (rc)' 1 $rc
  CkHas '...saying the compiler was NOT judged' $out 'COULD NOT RUN'
  CkHas '...and saying so about staleness explicitly' $out 'NOTHING above says that binary is stale'
  Ck '...and it does NOT tell the operator to rebuild' 'no' `
     $(if ($out -match 'REBUILD IT:') { 'yes' } else { 'no' })
  # ── AN UNEXPECTED EXIT CODE IS NOT AN ACCUSATION EITHER ──────────────────
  # ★ THE ONE ARM THAT REPLACES THE CORE, and it says so rather than pretending
  # otherwise: the property under test belongs to the DRIVER's classification of an
  # exit code, and the real core cannot be made to return an arbitrary one through
  # the argv the driver builds. ✔MEASURED 2026-08-31 with the real core: a bad
  # --config-root reaches argparse as rc 2, and the first version of this gate
  # printed argparse's usage block underneath "THE LOCATED COMPILER CANNOT COMPILE
  # THREE LINES" plus a rebuild instruction. Only rc 1 may accuse.
  $stub = Join-Path $Work 'rc2-core.py'
  Set-Content -LiteralPath $stub -Value @('import sys', 'print("usage: ...", file=sys.stderr)', 'sys.exit(2)') -Encoding ascii
  $out = & $pwshCmd.Source -NoProfile -NonInteractive -File $script $stub $fakeOk $repoRoot 2>&1 | Out-String
  $rc = $LASTEXITCODE
  Ck 'an UNEXPECTED exit code stops the run (rc)' 1 $rc
  CkHas '...reported as COULD NOT RUN, naming the code' $out 'COULD NOT RUN for target x86_64:elf64-x86_64-linux-exec (exit 2)'
  Ck '...and NOT as an accusation against the compiler' 'no' `
     $(if ($out -match 'CANNOT COMPILE THREE LINES') { 'yes' } else { 'no' })
  Remove-Item -LiteralPath $script -Force -ErrorAction SilentlyContinue
}

Green 'A+B  the verdict recorders + the shared run decision' 'Pin-Verdicts'
Green 'C    Read-CorpusSegment keeps the first diagnostic'   'Pin-ReadSegment'
# ★★ THE PARITY PIN, RUN FROM THE PowerShell SIDE TOO. Its twin lives in
#    test-driver-contracts.sh and already checks BOTH drivers — but THAT SUITE
#    NEVER RUNS ON A WINDOWS-ONLY HOST: build-and-test.ps1 self-tests with this
#    file, build-and-test.sh with the .sh one. A parity check that only exists in
#    the suite one driver runs is itself the asymmetry it is meant to catch.
Say '-- C2   the declared capability set reaches every build site, BOTH drivers'
Pin-StageCapabilities @($PS1, (Join-Path $Here 'build-and-test.sh'))
Green 'D+E  the loader variable + the acquisition field'     'Pin-LoaderVar'
Green 'G    the confound supply follows the DECLARATION'     'Pin-ConfoundSupply'
Green "G2   the supply's REFUSAL stops the DRIVER"           'Pin-ConfoundSupplyStopsTheDriver'
Green 'H    a failed run-dir operation is a VERDICT'         'Pin-RunDirArgv'
Green "I    a launched leg's run environment ARRIVES"        'Pin-LaunchEnv'
Green 'J    the CLI smoke gate applies it too'               'Pin-SmokeSite'
Green 'K    the launcher-prerequisite gate'                  'Pin-LauncherPrereq'
Green 'L    the smoke argv: MEASURED targets, DECLARED launcher' 'Pin-SmokeArgv'
Green 'M    the precondition discriminator, silent crash included' 'Pin-Precondition'
Green 'N    the confound report is PRINTED, per leg'          'Pin-ConfoundReport'
Green 'O    the located compiler is PROVED current'           'Pin-CompilerCurrency'

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
if (Invoke-Mutation 'F3 hardcode the ELF loader variable' $m3 "'darwin'  { return [pscustomobject]@{ Name = 'DYLD_LIBRARY_PATH'" {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains("'darwin'  { return [pscustomobject]@{ Name = 'DYLD_LIBRARY_PATH'")) { $_.Replace("'DYLD_LIBRARY_PATH'", "'LD_LIBRARY_PATH'") } else { $_ }
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
  # ⚠ `ping -n`, NOT `timeout /t` — AND THE REASON IS MEASURED, NOT STYLISTIC.
  # [D-TEST-HYGIENE-PIN-DECOY-DIES-WHEN-STDIN-IS-REDIRECTED.]
  # `timeout` reads the console to honour a keypress, so with stdin redirected it
  # exits IMMEDIATELY ("Input redirection is not supported"). This runner is
  # spawned with redirected stdin by both drivers' start-up self-test and by CI,
  # so the decoy was already dead 800 ms later and the sweep correctly found
  # NOTHING — reporting a blind sweep when the only thing that had gone was the
  # pin's OWN fixture. ✔MEASURED: `HasExited = True` under a redirected stdin and
  # `False` from an interactive console, which is why it looked intermittent.
  # `ping -n 26 127.0.0.1` needs no console input and is on every Windows box.
  $decoy  = Start-Process -FilePath 'cmd.exe' `
              -ArgumentList @('/c', "ping -n 26 127.0.0.1 >nul & rem $marker") `
              -PassThru -WindowStyle Hidden
  try {
    Start-Sleep -Milliseconds 800
    # ★★ THE FIXTURE IS ASSERTED ALIVE BEFORE THE SUBJECT IS ASKED ANYTHING. This
    # is the structural half of the fix and it outlives the `ping`/`timeout`
    # detail: a decoy that dies makes the two assertions below fail for a reason
    # that has nothing to do with the sweep, i.e. the pin accuses its subject of
    # its own defect. Named separately so the next such death is one line of
    # triage instead of a hunt through the sweep.
    Ck "the pin's own decoy is still ALIVE when the sweep runs" $false $decoy.HasExited
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

# ── F7-F10  the launcher run environment ────────────────────────────────────
# ⚠ F7, F9 and F10 are SKIPPED WITH A REASON where the leg's declared translator
# is unavailable, because pin I skips its launcher-namespace half there — and a
# mutation whose pin cannot run reports VACUOUS against a perfectly correct
# driver, which trains a reader to ignore exactly the line that matters.

# F7 - THE MEASURED DEFECT ITSELF, RESTORED: join the loader search path with the
# HOST's separator. On a Windows host that is `;`, which ld.so reads as part of a
# single directory name.
$m7 = Join-Path $Work 'm7.ps1'
if (-not $script:XlateOk) {
  Skipped "F7: the loader search path separator - pathTranslation '$script:XlateVerb' cannot translate on this host"
} elseif (Invoke-Mutation 'F7 join the loader path with the HOST separator' $m7 'return ($parts -join $spec.Separator)' {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains('return ($parts -join $spec.Separator)')) {
          $_.Replace('-join $spec.Separator', '-join [System.IO.Path]::PathSeparator')
        } else { $_ }
      })
    }) {
  Red 'F7 the loader search path uses the TARGET separator' 'Pin-LaunchEnv' $m7
}

# F8 - THE OTHER MEASURED DEFECT, RESTORED: the smoke gate spawns its child with
# none of the leg's declared run environment, exactly as it did until this cycle.
$m8 = Join-Path $Work 'm8.ps1'
if (Invoke-Mutation 'F8 spawn the smoke gate with no leg environment' $m8 '$smokeEnv = Push-LegLaunchEnv $leg (Get-LegLoaderSearchPath $leg $smokeLibDirs) @() @()' {
      param($src)
      return @($src | Where-Object { -not $_.Contains('$smokeEnv = Push-LegLaunchEnv $leg (Get-LegLoaderSearchPath $leg $smokeLibDirs) @() @()') })
    }) {
  Red 'F8 the CLI smoke gate applies the leg run environment' 'Pin-SmokeSite' $m8
}

# F9 - forward the loader path in THIS DRIVER's spelling, which is what a
# `C:\…`-flavoured LD_LIBRARY_PATH did to a Linux loader.
$m9 = Join-Path $Work 'm9.ps1'
if (-not $script:XlateOk) {
  Skipped "F9: the loader search path namespace - pathTranslation '$script:XlateVerb' cannot translate on this host"
} elseif (Invoke-Mutation 'F9 leave the loader path in the HOST namespace' $m9 '$t = Convert-LaunchPath $verb "$d"' {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains('$t = Convert-LaunchPath $verb "$d"')) { '    $t = "$d"' } else { $_ }
      })
    }) {
  Red 'F9 the loader search path is spelled for the LAUNCHER' 'Pin-LaunchEnv' $m9
}

# F10 - set the loader variable and never name it in the carrier: the value is
# perfect and the launched process never sees it. This is the silent half of the
# defect, and the one no amount of looking at the driver's own environment finds.
$m10 = Join-Path $Work 'm10.ps1'
if (-not $script:XlateOk) {
  Skipped "F10: the carrier - pathTranslation '$script:XlateVerb' cannot translate on this host"
} elseif (Invoke-Mutation 'F10 keep the loader variable out of the carrier' $m10 '(@($declaredNames) + @($var)) $carrierOld)) {' {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains('(@($declaredNames) + @($var)) $carrierOld)) {')) {
          $_.Replace('(@($declaredNames) + @($var))', '(@($declaredNames))')
        } else { $_ }
      })
    }) {
  Red 'F10 the loader search variable CROSSES the boundary' 'Pin-LaunchEnv' $m10
}

# ── F11-F14  the launcher-prerequisite gate + the token's classification ────

# F11 - THE DEFECT THIS GATE EXISTS FOR, RESTORED: an UNMET prerequisite answered
# as if it were something else. The whole rc-3 arm is removed, so the leg never
# gets the ENVIRONMENTAL verdict that keeps it out of the corpus.
# ⚠ THE WITNESS IS THE RECORDING LINE, not the token: the token also appears in
#   the ledger switch, in $EnvironmentalVerdicts and in prose, so a witness on the
#   bare token would survive its own removal.
$m11 = Join-Path $Work 'm11.ps1'
if (Invoke-Mutation 'F11 answer an UNMET launcher prerequisite as something else' $m11 "    `$leg.run.verdict = 'skipped-launcher-prerequisite-missing'" {
      param($src)
      $out = New-Object 'System.Collections.Generic.List[string]'
      $skip = $false; $done = $false
      foreach ($l in $src) {
        if (-not $done -and $l -eq '  if ($rc -eq 3 -and $report) {') { $skip = $true }
        if ($skip) { if ($l -eq '  }') { $skip = $false; $done = $true }; continue }
        [void]$out.Add($l)
      }
      return $out
    }) {
  Red 'F11 an unmet launcher prerequisite skips the leg' 'Pin-LauncherPrereq' $m11
}

# F12 - drop the new token from the ENVIRONMENTAL class at Step 9. The leg is still
# skipped and still ledgered; what disappears is DSS_STRICT_ARM_VERDICTS=1's power
# to red the run over it - a gate a one-word edit silently disables.
$m12 = Join-Path $Work 'm12.ps1'
if (Invoke-Mutation 'F12 declassify the launcher skip as environmental' $m12 "`$EnvironmentalVerdicts = @('skipped-emulator-missing','skipped-launcher-prerequisite-missing','skipped-build-input-missing')" {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains("`$EnvironmentalVerdicts = @('skipped-emulator-missing','skipped-launcher-prerequisite-missing','skipped-build-input-missing')")) {
          "`$EnvironmentalVerdicts = @('skipped-emulator-missing','skipped-build-input-missing')"
        } else { $_ }
      })
    }) {
  Red 'F12 strict mode REDS the run over a launcher-prerequisite skip' 'Pin-LauncherPrereq' $m12
}

# F13 - remove the smoke gate's rc-1 arm, which is the state the enumeration left it
# in until this cycle: a MATCHED, attributed compiler failure falling into `default`
# and printing "NOT charged to DSS". The FALSE-ACQUITTAL direction - the one that
# hides a real bug rather than inventing one.
$m13 = Join-Path $Work 'm13.ps1'
if (Invoke-Mutation 'F13 take the smoke gate CHARGED-TO-DSS arm away' $m13 'CLI smoke RED and CHARGED TO DSS' {
      param($src)
      $out = New-Object 'System.Collections.Generic.List[string]'
      $skip = $false
      foreach ($l in $src) {
        if (-not $skip -and $l.TrimStart().StartsWith('1 { # ')) { $skip = $true }
        if ($skip) { if ($l.Contains('CLI smoke RED and CHARGED TO DSS')) { $skip = $false }; continue }
        [void]$out.Add($l)
      }
      return $out
    }) {
  Red 'F13 rc 1 from the smoke gate IS the accusation' 'Pin-LauncherPrereq' $m13
}

# F14 - remove the new token's arm from the Step-9 ledger switch. This is the
# MEASURED pre-cycle state: harness_legs.py had the token, this hardcoded mirror did
# not, and a correctly-classified leg fell into `default` and printed as a
# "★ LEDGER ACCOUNTING HOLE".
$m14 = Join-Path $Work 'm14.ps1'
if (Invoke-Mutation 'F14 drop the ledger arm for the new token' $m14 "    'skipped-launcher-prerequisite-missing' { `$vLauncherPrereq++ }" {
      param($src)
      return @($src | Where-Object { -not $_.Contains("    'skipped-launcher-prerequisite-missing' { `$vLauncherPrereq++ }") })
    }) {
  Red 'F14 the Step-9 ledger knows the token' 'Pin-LauncherPrereq' $m14
}

# F15 - feed the smoke gate the DECLARED spec where the MEASURED target belongs.
# The gate's wrong-target check then compares the declaration with itself and can
# never fire.
$m15 = Join-Path $Work 'm15.ps1'
if (Invoke-Mutation 'F15 pass the declared spec as the measured target' $m15 "'--cli-target',       `$cliId.Target" {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains("'--cli-target',       `$cliId.Target")) { $_.Replace("`$cliId.Target", "`$leg.spec") } else { $_ }
      })
    }) {
  Red 'F15 --cli-target is MEASURED, never the declaration again' 'Pin-SmokeArgv' $m15
}

# F16 - THE MEASURED DEFECT ITSELF, RESTORED: pick the reference's launcher from
# the HOST identity flag instead of from the catalogue. This is the line that made
# the oracle unmatched, and the pin must go red on BOTH halves - the catalogue call
# is gone AND the flag is back in the block.
$m16 = Join-Path $Work 'm16.ps1'
if (Invoke-Mutation 'F16 pick the reference launcher from the host identity' $m16 '$lft = Get-LauncherForTarget $RefCliTarget' {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains('$lft = Get-LauncherForTarget $RefCliTarget')) {
          '    $lft = @{ Ok = $true; Rc = 0; Why = ''''; Launcher = @(if ($script:HostNeedsWsl) { ''wsl.exe''; ''-e'' }) }'
        } else { $_ }
      })
    }) {
  Red 'F16 the reference launcher comes from the catalogue, not the host' 'Pin-SmokeArgv' $m16
}

# F17 - drop the "same signature" conjunct. The discriminator becomes greedy and
# the RESILIENCE cases must go red: a genuine crash would stop being resumed.
$m17 = Join-Path $Work 'm17.ps1'
if (Invoke-Mutation 'F17 weaken the precondition discriminator' $m17 '[string]::Equals($zeroSig, $prevZeroSig, [System.StringComparison]::Ordinal)' {
      param($src)
      return @($src | ForEach-Object {
        if ($_.TrimStart().StartsWith('if ($res.Completed.Count -eq 0 -and')) {
          '  if ($res.Completed.Count -eq 0) {'
        } else { $_ }
      })
    }) {
  Red 'F17 a genuine crash is still RESUMED' 'Pin-Precondition' $m17
}

# F18 - THE SILENT-CRASH DEFECT ITSELF, RESTORED. Get-ZeroProgressSignature stops
# answering for a segment that produced NOTHING, which is exactly the state the
# discriminator was in when elf64-x86_64 burned all ten resumes on a fixture that
# had never started. The TALKING cases must stay green and only the SILENT ones go
# red - a mutation that reddened everything would prove far less.
# ⚠ THE WITNESS IS THE SENTINEL STRING ITSELF, unique in the driver: the
#   surrounding if/return and the comment above it survive, so "something changed"
#   cannot stand in for "the sentinel is gone".
$m18 = Join-Path $Work 'm18.ps1'
if (Invoke-Mutation 'F18 make a SILENT crash unsignable again' $m18 "return '<SILENT: the fixture produced no diagnostic, no test result and no test name>'" {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains("return '<SILENT: the fixture produced no diagnostic")) { "    return ''" } else { $_ }
      })
    }) {
  Red 'F18 a fixture that dies SILENTLY is still diagnosed' 'Pin-Precondition' $m18
}

# F19 - DOWNGRADE THE GATING REFUSAL TO A NOTE, so the supply hands back its
# UNGATED list and the driver carries on.
# ANCHOR, ONE LINE, DO NOT WRAP (the registry guard matches the whole name):
# D-HARNESS-CONFOUND-SUPPLY-REFUSAL-DIES-IN-A-SUBSHELL
# This is what "the refusal does not stop the driver" looks like
# from the outside, and it is the .ps1's own version of the .sh defect that shipped:
# there the refusal printed and the corpus ran anyway. G2 must notice, in the child
# process, which is the only place "the driver stopped" is observable at all.
$m19 = Join-Path $Work 'm19.ps1'
if (Invoke-Mutation 'F19 downgrade the gating refusal to a note' $m19 "not 'probed'. A conditional confound row" {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains("not 'probed'. A conditional confound row")) {
          '    Info "[$($leg.label)] gating $gating"'
        } else { $_ }
      })
    }) {
  Red "F19 the supply's refusal STOPS THE DRIVER" 'Pin-ConfoundSupplyStopsTheDriver' $m19
}

# F20 - RESTORE THE WARNING WHERE THE CHECK NOW STANDS. This is the MEASURED
# pre-cycle state of this very driver: Step 5 ended with `Warn "if the build below
# fails with a stale-manifest-field error ... rebuild it"` and let the run proceed.
# The region still DEFINES the assertion, so nothing is missing to read - only to
# run. [D-HARNESS-SQLITE-REUSES-A-RELEASE-BINARY-OLDER-THAN-THE-CONFIG-IT-IS-GIVEN]
$m20 = Join-Path $Work 'm20.ps1'
if (Invoke-Mutation 'F20 warn about a stale compiler instead of refusing' $m20 '$DssCurrencyOk = Assert-DssCompilerCurrent ' {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains('$DssCurrencyOk = Assert-DssCompilerCurrent ')) {
          '$DssCurrencyOk = "(not checked)"; Warn "if the build below fails with a stale-manifest-field error, this binary predates the project-config extensions"'
        } else { $_ }
      })
    }) {
  Red 'F20 a stale compiler is REFUSED, not warned about' 'Pin-CompilerCurrency' $m20
}

# F21 - GUARD THE CALL ON SKIP_DSS_BUILD, i.e. "only check the binary we built".
# The single most plausible wrong reading of that flag, and the one that restores
# the defect in silence: the branch it governs is precisely the one that reuses a
# binary nobody looked at.
# ⚠ THE GUARDED CALL IS SPELLED THROUGH THE CALL OPERATOR, and that is what makes
# the mutation legal rather than cosmetic. ✔MEASURED while writing this: a first
# version wrapped the call in an `if` and re-emitted it verbatim, so the witness
# reappeared INSIDE its own replacement and Invoke-Mutation correctly refused —
# a mutation that KEEPS the witness text can never satisfy check (2). `& 'Name'`
# is an ordinary PowerShell invocation of the same function, so the mutant is the
# wrong READING and not a different program.
$m21 = Join-Path $Work 'm21.ps1'
if (Invoke-Mutation 'F21 let SKIP_DSS_BUILD bypass the check' $m21 '$DssCurrencyOk = Assert-DssCompilerCurrent ' {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains('$DssCurrencyOk = Assert-DssCompilerCurrent ')) {
          '$DssCurrencyOk = "(not checked under SKIP_DSS_BUILD=1)"; if ($env:SKIP_DSS_BUILD -ne ''1'') { $DssCurrencyOk = & ''Assert-DssCompilerCurrent'' $python3.Source $BenchCore $DssBin $dssAge $DssOrigin $DssConfigRoot $DssCurrencySpecs $DssRebuildCmd }'
        } else { $_ }
      })
    }) {
  Red 'F21 SKIP_DSS_BUILD does not exempt a binary' 'Pin-CompilerCurrency' $m21
}

# F22 - COLLAPSE THE TWO FAILURE KINDS INTO ONE. Without the rc-3 arm, "I could
# not run the check" is reported with the stale-binary message and the rebuild
# instruction - a true-sounding answer to the adjacent question, which sends the
# operator to rebuild a compiler that was never the subject.
$m22 = Join-Path $Work 'm22.ps1'
if (Invoke-Mutation 'F22 report an unrunnable check as a stale binary' $m22 '    if ($rc -ne 1) {' {
      param($src)
      $out = New-Object 'System.Collections.Generic.List[string]'
      $skip = $false
      foreach ($l in $src) {
        if ($l -eq '    if ($rc -ne 1) {') { $skip = $true; continue }
        if ($skip -and $l -eq '    }') { $skip = $false; continue }
        if (-not $skip) { [void]$out.Add($l) }
      }
      return $out.ToArray()
    }) {
  Red 'F22 an unrunnable check is NOT a stale-binary verdict' 'Pin-CompilerCurrency' $m22
}

# ═══════════════════════════════════════════════════════════════════════════
# P - A LOCATED COMPILER IS REFRESHED, NOT TRUSTED
#     [D-HARNESS-PS1-REUSES-A-RELEASE-BINARY-OLDER-THAN-THE-SOURCES-IT-COMPILES]
#
# The sibling pin O proves a located binary can READ today's config. That is a
# DIFFERENT question from whether it was BUILT from today's sources, and the two
# were conflated until a run compiled the sqlite corpus with a compiler four
# cycles old and reported it as the current tree's.
# ⓘ THE BUILD IS INJECTED, so this pin never shells out to cmake: a self-test that
# ran a real build would measure the machine rather than this logic, and would be
# skipped on any host without a configured tree - which is every CI host.
# ═══════════════════════════════════════════════════════════════════════════
function Pin-CompilerRefresh($driver) {
  $fns = Get-Fns $driver @('Update-LocatedDssCompiler')
  if (-not $fns) { return }
  . ([scriptblock]::Create($fns))

  # (1) IT BUILDS, AND IT BUILDS THE LOCATED TREE - not a default, not the repo
  #     root. A refresh aimed at the wrong tree leaves the located binary exactly
  #     as stale as before while printing that it refreshed something.
  $seen = New-Object 'System.Collections.Generic.List[string]'
  $okBuild = { param($t, $j) [void]$seen.Add("$t|$j"); return 0 }
  $ret = Update-LocatedDssCompiler 'T:\rel' 'T:\rel\bin\dss\dsscp.exe' '2026-08-31 23:25:16' 7 $okBuild
  Ck 'P1   the LOCATED tree is what gets rebuilt, with this run''s job count' 'T:\rel|7' ($seen -join ',')
  Ck 'P2   ...and the refresh reports back the tree it built'                 'T:\rel'   "$ret"

  # (3) A FAILED REBUILD REFUSES. This is the whole point: falling back to the
  #     binary we found is the defect, dressed as resilience.
  $badBuild = { param($t, $j) return 1 }
  $msg = ''
  $threw = $false
  try { [void](Update-LocatedDssCompiler 'T:\rel' 'T:\rel\bin\dss\dsscp.exe' '2026-08-31 23:25:16' 7 $badBuild) }
  catch { $threw = $true; $msg = "$_" }
  if ($threw) {
    Ok 'P3   a FAILED rebuild REFUSES, never falls back to the located binary'
  } else {
    $script:PinFails++
    if (-not $script:Quiet) { Bad 'P3   a failed rebuild did NOT refuse - the run would proceed on an unverifiable compiler' }
  }
  # The message must name what a fallback would reinstate, or the next reader
  # "fixes" the refusal by removing it. Asserted as a FRAGMENT: a whole anchor id
  # here would be a citation this file does not own.
  CkHas 'P4   ...and says which defect a fallback would reinstate' $msg 'OLDER-THAN-THE-SOURCES-IT-COMPILES'

  # (5) NO TREE IS ITS OWN REFUSAL, distinct from a failed build - a binary whose
  #     build root cannot be named cannot be refreshed, and that is not the same
  #     fact as a build that ran and failed.
  $msg2 = ''
  $threw2 = $false
  try { [void](Update-LocatedDssCompiler '' 'T:\somewhere\dsscp.exe' '2026-08-31 23:25:16' 7 $okBuild) }
  catch { $threw2 = $true; $msg2 = "$_" }
  if ($threw2) {
    Ok 'P5   a binary with NO build tree REFUSES, and is not silently reused'
  } else {
    $script:PinFails++
    if (-not $script:Quiet) { Bad 'P5   an untreed binary did NOT refuse' }
  }
  CkHas 'P6   ...and the two refusals are told apart' $msg2 'build TREE could not be determined'

  # (6b) A LEAKY BUILDER IS ITS OWN REFUSAL, NOT A FAILED BUILD. ✔MEASURED on the
  #      first real run of this fix: `& cmake ...; return $LASTEXITCODE` in a
  #      scriptblock returns [...every output line..., code], because a native
  #      command's stdout IS pipeline output - and the refusal above then fired on
  #      a compiler that had just been rebuilt successfully. Coercing with $rc[-1]
  #      would read the right number and HIDE the leak; charging it to the build
  #      sends the reader to repair a tree that is fine.
  $leaky = { param($t, $j) return @('[1/5] dss: computing DSS_BUILD_STAMP', '[5/5] Linking', 0) }
  $msg3 = ''
  $threw3 = $false
  try { [void](Update-LocatedDssCompiler 'T:\rel' 'T:\rel\bin\dss\dsscp.exe' '2026-08-31 23:25:16' 7 $leaky) }
  catch { $threw3 = $true; $msg3 = "$_" }
  if ($threw3) {
    Ok 'P6b  a builder that LEAKS its child''s stdout is refused in its own words'
  } else {
    $script:PinFails++
    if (-not $script:Quiet) { Bad 'P6b  a leaky builder was accepted - a successful build would be read as a failed one' }
  }
  CkHas 'P6c  ...and is NOT reported as a failed build' $msg3 'did not return an EXIT CODE'
  # The CONTROL for P6b: a plain integer 0 must still pass, or P6b is passing
  # because the function refuses everything.
  $seen2 = New-Object 'System.Collections.Generic.List[string]'
  $ret2 = Update-LocatedDssCompiler 'T:\rel2' 'T:\rel2\dsscp.exe' 'w' 3 { param($t, $j) [void]$seen2.Add($t); return 0 }
  Ck 'P6d  CONTROL: a scalar 0 still refreshes' 'T:\rel2' "$ret2"

  # (7) THE CALL SITE. A function that is defined and never called is precisely
  #     the vacuity this file exists to refuse - pin O learned the same lesson.
  $text = (Get-Content -LiteralPath $driver) -join "`n"
  CkHas 'P7   the located-binary branch CALLS the refresh' $text '[void](Update-LocatedDssCompiler $DssInfo.Tree'
  CkHas 'P8   ...and the origin line says the run rebuilt it' $text 'then REBUILT by this run (incremental)'
}
Green 'P    a located compiler is REFRESHED, not trusted' 'Pin-CompilerRefresh'

# F23 - TAKE THE FATAL ARM AWAY, i.e. "a failed rebuild is not worth stopping
# for". That is the exact reading the pre-fix driver embodied by never building at
# all, and it fails toward *clean*: the run continues on whatever the root held.
$m23 = Join-Path $Work 'm23.ps1'
if (Invoke-Mutation 'F23 let a failed rebuild fall back to the located binary' $m23 '  if ($rc -ne 0) {   # FATAL: falling back to the binary we located IS the defect' {
      param($src)
      return @($src | ForEach-Object {
        if ($_.Contains('# FATAL: falling back to the binary we located IS the defect')) { '  if ($false) {' } else { $_ }
      })
    }) {
  Red 'F23 a failed rebuild is FATAL, not a fallback' 'Pin-CompilerRefresh' $m23
}

Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
Write-Host ''
Write-Host "passed=$($script:Passed) failed=$($script:Failed) skipped=$($script:Skipped)"
if ($script:Failed -ne 0) { exit 1 }
exit 0
