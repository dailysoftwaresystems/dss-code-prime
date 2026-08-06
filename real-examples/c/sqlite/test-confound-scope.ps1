# Verifies the .ps1 scoped-confound classifier by EXTRACTING the shipped block,
# not by re-implementing it. This driver has only a NATIVE leg, so the contract is:
# bare patterns excuse; `emulated:` patterns must NEVER excuse; `native:` patterns do.
$ErrorActionPreference = 'Stop'
$sh = Join-Path $PSScriptRoot 'build-and-test.ps1'
$lines = Get-Content $sh
$start = ($lines | Select-String -Pattern '^\$real = @\(\); \$confound = @\(\); \$scopedExcused = @\(\)' | Select-Object -First 1).LineNumber
# The end anchor's leg tag is a VARIABLE as of TF-C114 (`[$LegTag]`): the driver
# now runs the same classifier for every declared leg, so it cannot carry the
# literal `[pe64]` this used to match. `\[[^\]]*\]` keeps the anchor as specific as
# it needs to be — it is the `$($scopedExcused.Count) failure` half that identifies
# the line — while surviving whatever the driver names the leg.
$end   = ($lines | Select-String -Pattern '^\s*Warn "\[[^\]]*\] \$\(\$scopedExcused\.Count\) failure' | Select-Object -First 1).LineNumber
if (-not $start -or -not $end) { throw "could not locate the shipped classifier block (start=$start end=$end)" }
# LineNumber is 1-based, the array 0-based: line N is at index N-1. Take through the
# Warn line and supply the closing brace of `if ($scopedExcused.Count) {` ourselves.
$block = ($lines[($start-1)..($end-1)] -join "`n") + "`n}"
"extracted $($end - $start + 1) lines from the shipped script"

function Warn($m) { "      WARN: $m" }

# ── THE ACCOUNTING INVARIANT (mirrors test-confound-scope.sh) ────────────────
# Every assertion below must land in EXACTLY ONE of pass/fail/skip, and the three
# must SUM to this number. Checked at the bottom; a mismatch FAILS the self-test,
# which at Step 0 of build-and-test.ps1 refuses to start the run.
# The .sh twin grew this after its two hand-written SKIP counts drifted (11 and 10
# against blocks of 14 and 12), silently deleting five assertions from a git-less
# host's "OK (N assertions)". This file now has a skippable block of its own, so it
# inherits the same hazard and the same cure.
# ★ ADDING AN ASSERTION WITHOUT BUMPING THIS NUMBER FAILS ON THE VERY NEXT RUN.
$TotalAssertions = 63     # 11 classifier + 18 checkout-provenance + 9 loadext rc contract (python-gated)
                          # + 5 structural + 8 staged sqlite_cfg.h (5 this driver, 3 .sh pairing)
                          # + 6 launcher argv form (4 structural + 2 behavioural, python-gated)
                          # + 6 THE SUPPLY (Get-LegConfounds) — the half that was
                          #   never tested, and where the defect actually was
$pass = 0; $fail = 0; $skip = 0
function Check($label, $cond) {
  if ($cond) { "  ok   $label"; $script:pass++ } else { "  FAIL $label"; $script:fail++ }
}
function CheckEq($label, $want, $got) {
  # Substring/truthiness matching cannot express "the field is EMPTY" — and an empty
  # provenance field is precisely what these assertions exist to forbid.
  if ("$got" -ceq "$want") { "  ok   $label"; $script:pass++ }
  else { "  FAIL $label"; "       want exactly: [$want]"; "       got         : [$got]"; $script:fail++ }
}

# The driver stamps its log lines with the LEG it is processing; standing in for
# one here keeps this file's output readable. Nothing is asserted about it — the
# assertions are all about $real / $confound / $scopedExcused.
# ★ $LegRunMode is deliberately LEFT UNSET: the shipped block derives
# `$legMode = if ($LegRunMode -eq 'launched') { 'emulated' } else { 'native' }`,
# so an absent run mode must fall to 'native' — which is exactly the contract the
# `emulated:`/`native:` assertions below test. If a future edit makes the default
# anything else, these go red, which is the point.
$LegTag = 'pe64-x86_64'
$failNames = @('writecrash-1.1.1','walsetlk-2.1.3','zipfile-25.0','sometest-9.9')
# Real declared prefixes from permutations.test: the dotted default AND the `mmap`
# override "mm-" (a DASH, not derivable from the suite name). pe64's `all` run emits
# the mm- shape, which is why this driver needs the strip too.
$TierPrefixes = @('no_mutex_try.','memsubsys1.','memsubsys2.','mm-')

"--- native pe64 leg, scoped emulated pattern present ---"
$Confounds = @('^walsetlk-','^zipfile-25\.0$','emulated:^writecrash-')
Invoke-Expression $block
"      REAL=[$($real -join ' ')] CONFOUND=[$($confound -join ' ')] SCOPED=[$($scopedExcused -join ' ')]"
Check "emulated: pattern does NOT excuse on a native leg" ($real -contains 'writecrash-1.1.1')
Check "bare patterns still excuse"                        ($confound -contains 'walsetlk-2.1.3' -and $confound -contains 'zipfile-25.0')
Check "genuine failure stays real"                        ($real -contains 'sometest-9.9')
Check "no scope excusals recorded"                        ($scopedExcused.Count -eq 0)

"--- native: pattern DOES apply here ---"
$Confounds = @('native:^writecrash-')
Invoke-Expression $block
"      REAL=[$($real -join ' ')] CONFOUND=[$($confound -join ' ')] SCOPED=[$($scopedExcused -join ' ')]"
Check "native: pattern excuses on a native leg"  ($confound -contains 'writecrash-1.1.1')
Check "and is NAMED as scope-excused"            ($scopedExcused -contains 'writecrash-1.1.1')

"--- RED-ON-DISABLE: the same regex WITHOUT a scope must excuse (so the prefix is load-bearing) ---"
$Confounds = @('^writecrash-')
Invoke-Expression $block
Check "unscoped leaks in (prefix is what gated it)" ($confound -contains 'writecrash-1.1.1')

'--- prefix-qualified names (pe64 all-tier emits the mm- shape) ---'
$Confounds = @('^walsetlk-','^zipfile-25\.0$')
$failNames = @('mm-zipfile-25.0','mm-walsetlk-2.1.3','mm-backup4-3.3')
Invoke-Expression $block
"      REAL=[$($real -join ' ')] CONFOUND=[$($confound -join ' ')]"
Check "mm- zipfile excused"            ($confound -contains 'mm-zipfile-25.0')
Check "mm- walsetlk excused"           ($confound -contains 'mm-walsetlk-2.1.3')
Check "mm- backup4 (no pattern) REAL"  ($real -contains 'mm-backup4-3.3')

"--- RED-ON-DISABLE: with no prefixes known, qualified names must go unmatched ---"
$TierPrefixes = @()
Invoke-Expression $block
Check "without TierPrefixes they ARE misreported as genuine" ($real -contains 'mm-zipfile-25.0')
$TierPrefixes = @('no_mutex_try.','memsubsys1.','memsubsys2.','mm-')

# ─────────────────────────────────────────────────────────────────────────────
# CHECKOUT PROVENANCE + THE DECLARED-REF ASSERTIONS (dss:src-provenance)
# ─────────────────────────────────────────────────────────────────────────────
# WHY THIS EXISTS. build-and-test.{sh,ps1} are a PAIRED CONTRACT, and until this
# section none of the .ps1's provenance code was tested on ANY leg while its .sh
# twin carried 26 assertions over the same decisions. An untested half is exactly
# where the two silently diverge — which is how DSS_BRANCH/DSS_COMMIT came to be
# asserted on Linux and ignored on Windows.
# Same discipline as the classifier above: EXTRACT the shipped region and RUN it.
# A re-implementation here would stay green while the driver's copy rotted.
# ★ SCOPE FIDELITY: the region is top-level driver flow and is executed here at
# SCRIPT scope. PowerShell `if`/`else`/`try` blocks do NOT create a scope, so the
# region's plain assignments mean exactly what they mean in the driver.
$provStart = ($lines | Select-String -Pattern '^# >>> dss:src-provenance >>>$' | Select-Object -First 1).LineNumber
$provEnd   = ($lines | Select-String -Pattern '^# <<< dss:src-provenance <<<$' | Select-Object -First 1).LineNumber
if (-not $provStart -or -not $provEnd -or $provEnd -le $provStart) {
  throw "could not locate the shipped dss:src-provenance region (start=$provStart end=$provEnd) — those sentinels in build-and-test.ps1 are a CONTRACT with this file."
}
# LineNumber is 1-based: the sentinel at line N sits at index N-1, so the region's
# BODY is index $provStart .. $provEnd-2 (both sentinels excluded).
$prov = ($lines[$provStart..($provEnd - 2)]) -join "`n"
"extracted $($provEnd - $provStart - 1) provenance lines from the shipped script"

# The driver's logging helpers, stubbed so the region's output is INSPECTABLE and a
# Die is a CATCHABLE outcome rather than an exit that would take this test with it.
$provLog = @()
function Step($m) { $script:provLog += "STEP: $m" }
function Info($m) { $script:provLog += "INFO: $m" }
function Pass($m) { $script:provLog += "PASS: $m" }
function Die($m)  { $script:provLog += "DIE: $m"; throw "DIE: $m" }
# ⚠ Warn is ALSO used by the classifier block above, which expects it to emit. Keep
# both behaviours: record for assertions, and still return the line.
function Warn($m) { $script:provLog += "WARN: $m"; "      WARN: $m" }
# -c overrides rather than the caller's global config: a box with commit.gpgsign=true,
# a core.hooksPath, or an init template would otherwise fail (or run hooks) inside a
# self-test. A simple function (no param block) so `-q`/`-C` reach git untouched.
function GitQ { & git -c init.templateDir= -c user.email=selftest@dss.invalid -c user.name=dss-selftest -c commit.gpgsign=false @args }
function Set-Scenario($repoDir, $branchWant, $commitWant) {
  # Clear EVERY variable the region sets. Without this a scenario that Dies early
  # would be scored against the PREVIOUS scenario's values — a stale-value false pass,
  # the same defect class as an uncounted skip.
  $script:dssIsRepo = $null; $script:dssHead = $null; $script:dssHeadLong = $null
  $script:dssBranch = $null; $script:dssDivergeNote = $null
  $script:RepoRoot = $repoDir
  $script:provLog = @()
  $env:DSS_BRANCH = if ($branchWant) { $branchWant } else { $null }
  $env:DSS_COMMIT = if ($commitWant) { $commitWant } else { $null }
}

"--- checkout provenance: the shipped region against REAL throwaway repos ---"
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
  # The helpers shell out to git, so they are exercised against a REAL repository —
  # a stub would test the stub. Skip LOUDLY and COUNT it (18 = the assertions in the
  # else-arm below), so the summary can never read as "all proven". The count is kept
  # honest by $TotalAssertions, not by this comment.
  "  SKIP no git on PATH — the provenance region was NOT exercised (18 assertions skipped)"
  $skip += 18
} else {
  $tmpRoot = Join-Path ([IO.Path]::GetTempPath()) ("confprov_" + [guid]::NewGuid().ToString('N').Substring(0,12))
  $repo    = Join-Path $tmpRoot 'repo'
  New-Item -ItemType Directory -Force -Path $repo | Out-Null
  try {
    GitQ init -q $repo *>$null
    'tracked' | Set-Content -LiteralPath (Join-Path $repo 'tracked.c')
    GitQ -C $repo add tracked.c *>$null
    GitQ -C $repo commit -q --no-verify -m seed *>$null
    $wantShort = ("$(& git -C $repo rev-parse --short HEAD)").Trim()
    $wantLong  = ("$(& git -C $repo rev-parse HEAD)").Trim()
    $wantBr    = ("$(& git -C $repo rev-parse --abbrev-ref HEAD)").Trim()

    "  ·· state 1: pristine checkout, no declared refs"
    Set-Scenario $repo '' ''
    $die = ''; try { $null = Invoke-Expression $prov } catch { $die = "$_" }
    CheckEq "head is the real short sha"          $wantShort $dssHead
    CheckEq "branch is the real branch"           $wantBr    $dssBranch
    CheckEq "a clean tree gets NO divergence note" ''        $dssDivergeNote
    Check   "a clean tree does not die"           ($die -eq '')

    "  ·· state 2: DSS_COMMIT declared"
    Set-Scenario $repo '' $wantLong
    $die = ''; try { $null = Invoke-Expression $prov } catch { $die = "$_" }
    Check "DSS_COMMIT matching HEAD is VERIFIED"  ($die -eq '' -and ($provLog -join "`n") -match 'DSS_COMMIT verified')
    Set-Scenario $repo '' $wantShort
    $die = ''; try { $null = Invoke-Expression $prov } catch { $die = "$_" }
    Check "an ABBREVIATED DSS_COMMIT resolves and verifies" ($die -eq '' -and ($provLog -join "`n") -match [regex]::Escape($wantLong))
    # RED-ON-DISABLE: the tempting simpler form is a bare string compare of the
    # declared value against HEAD. Show it would REJECT a legitimate abbreviation,
    # so `rev-parse --verify --quiet <c>^{commit}` is load-bearing, not ceremony.
    Check "a bare string compare would REJECT that abbreviation (so ^{commit} resolution is what accepts it)" ($wantShort -cne $wantLong)
    Set-Scenario $repo '' 'deadbeefdeadbeefdeadbeefdeadbeefdeadbeef'
    $die = ''; try { $null = Invoke-Expression $prov } catch { $die = "$_" }
    Check "a DSS_COMMIT that is not here DIES"    ($die -match 'does not resolve to a commit')
    'second' | Set-Content -LiteralPath (Join-Path $repo 'tracked.c')
    GitQ -C $repo commit -q --no-verify -am second *>$null
    Set-Scenario $repo '' $wantLong                # now a REAL commit that is not HEAD
    $die = ''; try { $null = Invoke-Expression $prov } catch { $die = "$_" }
    Check "a resolvable DSS_COMMIT that is NOT HEAD DIES" ($die -match 'The run would have validated the checkout')

    "  ·· state 3: DSS_BRANCH declared"
    $wantBr2 = ("$(& git -C $repo rev-parse --abbrev-ref HEAD)").Trim()
    Set-Scenario $repo 'not-this-branch' ''
    $die = ''; try { $null = Invoke-Expression $prov } catch { $die = "$_" }
    Check "DSS_BRANCH mismatch DIES"              ($die -match "DSS_BRANCH='not-this-branch'")
    Set-Scenario $repo $wantBr2 ''
    $die = ''; try { $null = Invoke-Expression $prov } catch { $die = "$_" }
    Check "DSS_BRANCH matching the checkout does NOT die" ($die -eq '')

    "  ·· state 4: a DIRTY tree (modified + untracked — the pre-commit probe shape)"
    'edited'  | Add-Content -LiteralPath (Join-Path $repo 'tracked.c')
    'brandnew'| Set-Content -LiteralPath (Join-Path $repo 'added.c')
    Set-Scenario $repo '' ''
    $die = ''; try { $null = Invoke-Expression $prov } catch { $die = "$_" }
    Check "the note carries the count"            ($dssDivergeNote -match '\+2 file\(s\) differ from HEAD')
    # Per-ENTRY, not a `-match '^WARN:'` over the joined log: without RegexOptions
    # Multiline, `^` anchors to the start of the WHOLE string, so that form asserted
    # "the FIRST log line is a WARN" and failed on a driver that warns correctly.
    Check "the divergence is WARNED about, not merely noted" (@($provLog | Where-Object { $_ -like 'WARN:*' }).Count -gt 0)

    "  ·· state 5: a populated NON-checkout nested inside a real repo"
    # The sharpest shape for the .git guard: `git -C` WALKS UPWARD, so a bare
    # rev-parse here answers with the ENCLOSING repo's HEAD — a real, wrong citation.
    $nested = Join-Path $repo 'not-a-checkout'
    New-Item -ItemType Directory -Force -Path $nested | Out-Null
    'x' | Set-Content -LiteralPath (Join-Path $nested 'src.c')
    Set-Scenario $nested '' ''
    $die = ''; try { $null = Invoke-Expression $prov } catch { $die = "$_" }
    Check "a non-checkout head says UNKNOWN(no .git ...), never empty" ($dssHead -like 'UNKNOWN(no .git*')
    Check "its divergence is UNVERIFIED, never reported as clean"      ($dssDivergeNote -match 'UNVERIFIED')
    $bareNested = ("$(& git -C $nested rev-parse --short HEAD 2>$null)").Trim()
    $outerShort = ("$(& git -C $repo   rev-parse --short HEAD 2>$null)").Trim()
    Check "RED-ON-DISABLE: a bare rev-parse there reports the ENCLOSING repo's HEAD (so the .git guard is what prevents the wrong citation)" ($bareNested -ceq $outerShort -and $bareNested -ne '')

    "  ·· state 6: a DETACHED HEAD"
    GitQ -C $repo checkout -q --detach HEAD *>$null
    Set-Scenario $repo '' ''
    $die = ''; try { $null = Invoke-Expression $prov } catch { $die = "$_" }
    CheckEq "a detached HEAD maps to DETACHED-HEAD" 'DETACHED-HEAD' $dssBranch
    $rawDetached = ("$(& git -C $repo rev-parse --abbrev-ref HEAD 2>$null)").Trim()
    Check "RED-ON-DISABLE: raw git prints the literal 'HEAD' there (so the mapping is what stops it reading as a branch named HEAD)" ($rawDetached -ceq 'HEAD')
  } finally {
    $env:DSS_BRANCH = $null; $env:DSS_COMMIT = $null
    # Cleanup must never turn a green self-test red: git leaves read-only objects
    # under .git and Windows can hold a handle a moment longer than we do.
    Remove-Item -Recurse -Force -LiteralPath $tmpRoot -ErrorAction SilentlyContinue
  }
}

# ─────────────────────────────────────────────────────────────────────────────
# THE LOADEXT HELPER (dss:loadext-stage-ps1) — the half this driver never had
# ─────────────────────────────────────────────────────────────────────────────
# D-HARNESS-PS1-STAGES-NO-LOADEXT-HELPER-COVERAGE-IS-UNDECLARED is the gap this
# driver's new staging closes: build-and-test.sh pre-staged the shared object
# sqlite's test/loadext.test dlopen()s and this driver staged NONE, so ~16
# loadext-* units per leg were left to sqlite's own hardcoded `gcc`. It could not
# simply be copied over, because the helper has to be built FOR THE LEG'S TARGET
# and this host's `gcc` reports x86_64-w64-mingw32 - correct for pe64, correctly
# refused for the elf legs, whose 330,436 units would then have been skipped.
# DSS now emits the helper for the leg's declared sharedLibFormat, so the premise
# is gone [D-HARNESS-CROSS-HOST-ANY-TARGET].
#
# WHAT IS TESTED HERE. The BUILD lives in harness_legs.py and is covered by its
# own self-test, which this driver runs as a refuse-to-start. What is tested here
# is this driver's half: that Resolve-LoadextHelper translates the resolver's rc
# contract into the RIGHT verdict class, and that the staging block records a
# named verdict and CONTINUES rather than dying. Same discipline as every other
# section: EXTRACT the shipped code and RUN it - a re-implementation would stay
# green while the driver's copy rotted.
$fnStart = ($lines | Select-String -Pattern '^function Resolve-LoadextHelper \{' | Select-Object -First 1).LineNumber
if (-not $fnStart) { throw "could not locate `function Resolve-LoadextHelper` in build-and-test.ps1 - it is a CONTRACT with this file." }
$fnEnd = $null
for ($i = $fnStart; $i -lt $lines.Count; $i++) {
  if ($lines[$i] -eq '}') { $fnEnd = $i + 1; break }   # first closing brace at column 0
}
if (-not $fnEnd) { throw "could not find the end of Resolve-LoadextHelper (no closing brace at column 0)" }
Invoke-Expression (($lines[($fnStart-1)..($fnEnd-1)]) -join "`n")
"extracted $($fnEnd - $fnStart + 1) loadext-helper lines from the shipped script"

$stageStart = ($lines | Select-String -Pattern '^# >>> dss:loadext-stage-ps1 >>>$' | Select-Object -First 1).LineNumber
$stageEnd   = ($lines | Select-String -Pattern '^# <<< dss:loadext-stage-ps1 <<<$' | Select-Object -First 1).LineNumber
if (-not $stageStart -or -not $stageEnd -or $stageEnd -le $stageStart) {
  throw "could not locate the shipped dss:loadext-stage-ps1 region (start=$stageStart end=$stageEnd) - those sentinels are a CONTRACT with this file."
}
$stageBlk = ($lines[$stageStart..($stageEnd - 2)]) -join "`n"

"--- the rc contract: 0 staged / 3 poisoned / 4 environmental / anything else refused ---"
$py = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $py) { $py = Get-Command python -ErrorAction SilentlyContinue }
if (-not $py) {
  # The stub resolver is a python script for the same reason the real one is: both
  # drivers already hard-require python3. Skip LOUDLY and COUNT it (9 = the
  # assertions in the else-arm), so the summary can never read as "all proven".
  "  SKIP no python3 on PATH - the loadext rc contract was NOT exercised (9 assertions skipped)"
  $skip += 9
} else {
  $lxTmp = Join-Path ([IO.Path]::GetTempPath()) ("confload_" + [guid]::NewGuid().ToString('N').Substring(0,12))
  New-Item -ItemType Directory -Force -Path $lxTmp | Out-Null
  try {
    # A stub that records the argv it was handed, then emits a canned report and
    # the rc the caller chose. NOTHING here touches DSS or a sqlite clone: this
    # file is a refuse-to-start gate and must run on a bare machine.
    $stub = Join-Path $lxTmp 'stub_resolver.py'
    @'
import json, os, sys
open(os.environ["STUB_ARGV_LOG"], "w").write("\n".join(sys.argv[1:]))
sys.stdout.write(json.dumps({
    "verdictClass": os.environ.get("STUB_CLASS", ""),
    "detail": os.environ.get("STUB_DETAIL", "a detail line"),
    "crossCheck": os.environ.get("STUB_CROSS", ""),
    "staged": os.environ.get("STUB_STAGED", "/staged/testloadext.dll"),
}) + "\n")
sys.exit(int(os.environ.get("STUB_RC", "0")))
'@ | Set-Content -LiteralPath $stub -Encoding UTF8
    $env:STUB_ARGV_LOG = Join-Path $lxTmp 'argv.txt'
    function Invoke-Stub($rc, $klass, $detail) {
      $env:STUB_RC = "$rc"; $env:STUB_CLASS = "$klass"; $env:STUB_DETAIL = "$detail"
      Resolve-LoadextHelper -Python $py.Source -LegsPy $stub -Label 'pe64-x86_64' `
        -Builder 'dss' -DssBin 'C:\nope\dss.exe' `
        -SqliteSrc (Join-Path $lxTmp 'sq\src') -SqliteBld (Join-Path $lxTmp 'sq\bld') `
        -DestDir (Join-Path $lxTmp 'run\testdir') -WorkDir (Join-Path $lxTmp 'work') `
        -Config 'release' -ReferenceCc 'gcc' -ReferenceMachine 'x86_64-w64-mingw32'
    }
    $r0 = Invoke-Stub 0 '' 'testloadext.dll, built for x86_64:pe64-x86_64-windows-dll by dss'
    Check "rc 0 is a staged helper"                    ($r0.Ok -and $r0.Class -eq '')
    Check "...and it carries the staged path"          ("$($r0.Staged)" -eq '/staged/testloadext.dll')
    $argv = (Get-Content -Raw $env:STUB_ARGV_LOG)
    # ★ THE ARGUMENT THAT MUST NOT BE OMITTED. An absent --reference-cc would let
    # the resolver take its own default; passing it EMPTY is what says "this host
    # has no control compiler" without changing which arm is primary.
    Check "the control compiler is PASSED, not assumed"  ($argv -match '--reference-cc')
    Check "...and so is the destination testdir"         ($argv -match 'testdir')
    $r3 = Invoke-Stub 3 'poisoned' 'the dss build FAILED: error[P0016] got quote include not found'
    Check "rc 3 is POISONED - a real failure"           ((-not $r3.Ok) -and $r3.Class -eq 'poisoned')
    Check "...carrying the compiler's own diagnostic"   ("$($r3.Detail)" -match 'P0016')
    # ★ THE VERDICT-CLASS CONTRACT. Folding this into 'poisoned' would red a run
    # in which nothing is broken - the DEFAULT builder needs nothing from this box.
    $r4 = Invoke-Stub 4 'skipped-build-input-missing' 'DSS_LOADEXT_HELPER=reference was requested'
    Check "rc 4 is ENVIRONMENTAL, NOT a failure"        ((-not $r4.Ok) -and $r4.Class -eq 'skipped-build-input-missing')
    $r9 = Invoke-Stub 9 'something-new' 'an unknown class'
    Check "an unrecognised rc is refused, never a quiet success" ((-not $r9.Ok) -and $r9.Class -eq 'poisoned')
    # Output that is not a report at all (the resolver's own FATAL line).
    $badStub = Join-Path $lxTmp 'stub_fatal.py'
    @'
import os, sys
open(os.environ["STUB_ARGV_LOG"], "w").write("\n".join(sys.argv[1:]))
sys.stderr.write("harness_legs.py: FATAL: no leg labelled 'nope'\n")
sys.exit(2)
'@ | Set-Content -LiteralPath $badStub -Encoding UTF8
    $rf = Resolve-LoadextHelper -Python $py.Source -LegsPy $badStub -Label 'nope' `
            -Builder 'dss' -DssBin 'C:\nope\dss.exe' -SqliteSrc $lxTmp -SqliteBld $lxTmp `
            -DestDir $lxTmp -WorkDir $lxTmp -Config 'release'
    Check "unreadable output is POISONED and QUOTED" ((-not $rf.Ok) -and "$($rf.Detail)" -match 'FATAL')
  } finally {
    Remove-Item Env:STUB_RC, Env:STUB_CLASS, Env:STUB_DETAIL, Env:STUB_ARGV_LOG -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force -LiteralPath $lxTmp -ErrorAction SilentlyContinue
  }
}

"--- STRUCTURAL: a staging failure is a per-leg verdict, never the end of the run ---"
# The .sh learnt this the expensive way: a `die` in its staging function ended a
# run in which two legs had already reported green over 331,351 and 331,355 units.
Check "the staging block records a NAMED verdict"        ($stageBlk -match "Set-LegVerdict")
Check "...keeps the ENVIRONMENTAL class apart from the failure one" ($stageBlk -match 'skipped-build-input-missing')
Check "...CONTINUES to the next leg"                     ($stageBlk -match '(?m)^\s*continue\s*$')
Check "...counts only the REAL failure toward the run's red" ($stageBlk -match '\$LoadextStageFails\+\+')
# RED-ON-DISABLE for the whole point: a `Die` here would take the run with it.
Check "the staging block contains NO Die"                (-not ($stageBlk -match '(?m)^\s*Die '))

"--- STRUCTURAL: the staged sqlite_cfg.h is asked for, and it is asked for FIRST ---"
# D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES. The
# recipe's _HAVE_SQLITE_CONFIG_H makes sqliteInt.h include a sqlite_cfg.h, and the
# staged bld dir - which holds the DERIVING host's copy - is the FIRST entry on
# this driver's base include list. So the per-target header has to be both
# REQUESTED and placed AHEAD of it; either half alone is inert, and the failure is
# silent (the leg builds, and builds wrong). The .sh twin of these assertions lives
# in test-confound-scope.sh, which also cross-checks THIS file.
$ps1Txt = Get-Content -LiteralPath $sh -Raw
# ★★ A CODE-SHAPE ASSERTION MUST NOT BE SATISFIABLE BY A COMMENT, and one below
# was. MEASURED 2026-08-05 (TF-C121): the first cut of the staged-cfg removal
# assertion matched the WHOLE file text, and the explanatory comment written beside
# the fix quotes the removed line verbatim - so deleting the real code and
# re-running still reported ok. A guard its own prose satisfies proves nothing.
# So every SHAPE-OF-CODE assertion runs against a comment-stripped view; assertions
# about DIAGNOSTIC TEXT keep the raw text (a Die message is code either way).
$ps1Code = (($ps1Txt -split "`r?`n") | Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
Check "the .ps1 asks stage-zinc.py for a per-target sqlite_cfg.h" ($ps1Code -match '--sqlite-cfg-h')
Check "...writing it into its own cfg/ root"                      ($ps1Code -match '--cfg-dest')
Check "...and REFUSES a run in which none was produced"           ($ps1Txt  -match 'produced NO per-target sqlite_cfg\.h')
Check "...with that dir FIRST on its include lists"               ($ps1Code -match '\@\(\(\$CfgStageDirs\[\$c\] -replace ''\\\\'',''/''\)\) \+ \@\(\$IncBase\)')
# ★ AND THE OTHER HALF: the DERIVING host's own copy is REMOVED from the staged
# bld dir. Position alone was NOT enough. A quote include searches the INCLUDING
# FILE'S OWN DIRECTORY before this list is consulted at all, so bld/ctime.c - TU #1
# of BOTH the fixture and the CLI, which does `#include "sqlite_cfg.h"` and then
# `#define SQLITECONFIG_H 1` - read the deriving host's answers out of its own
# directory whatever the include list said, and then shadowed sqliteInt.h's include
# for the rest of that TU. RED-ON-DISABLE: put the file back and this goes red.
Check "...and REMOVES the deriving host's copy from the staged bld dir" ($ps1Code -match 'Remove-Item -LiteralPath \$DerivedCfgH -Force')
# And the .sh must carry the same capability, or it is a capability in one driver
# and not the other - the exact shape this file exists to refuse.
$shTxt  = Get-Content -LiteralPath ($sh -replace '\.ps1$', '.sh') -Raw
$shCode = (($shTxt -split "`r?`n") | Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
Check "the .sh asks for it too"                                   ($shCode -match '--sqlite-cfg-h')
Check "...and refuses a run without it"                           ($shTxt  -match 'produced NO per-target sqlite_cfg\.h')
Check "...and removes the deriving host's copy too"               ($shCode -match 'rm -f "\$BLD/sqlite_cfg\.h"')

"--- STRUCTURAL: the launcher argv form is ``--launcher=<tok>``, never a space ---"
# D-HARNESS-DASH-LEADING-LAUNCHER-TOKEN-MISPARSED-AS-AN-OPTION.
#
# A launcher TOKEN may itself begin with a dash, and argparse then refuses the
# SPACE form with "expected one argument" instead of taking the next word as the
# value. MEASURED 2026-08-05 (TF-C121) in THIS driver: `--reference-launcher -e`
# killed the pe64 CLI smoke gate before a single assertion ran, and the caller then
# classified that argv defect as `smoke: FAIL - CHARGED TO DSS`, i.e. the harness
# accusing the compiler of a bug in the harness's own command line.
#
# The fix landed in both drivers with PROSE ONLY and no guard, which is what this
# block repairs. Both options are named separately because the sibling is the one
# that actually bit.
# $ps1Code / $shCode, never the raw text: the comments beside these fixes spell the
# BROKEN form out as the thing being warned against, so a full-text "never the
# space form" would red on a CORRECT driver, and a full-text "emits the = form"
# would stay green on a broken one.
Check "the .ps1 emits the ``=`` form for --launcher"          ($ps1Code -match '--launcher=\$t')
Check "...and for the sibling that actually broke the gate"   ($ps1Code -match '--reference-launcher=-e')
# ★ THE SPLIT SHAPE IS NOT A SPACE. MEASURED while demonstrating this guard:
# reverting to `@("--launcher", "$t")` produces NO literal `--launcher ` anywhere,
# because PowerShell passes the option and its value as two ARRAY ELEMENTS - so a
# space-only pattern stayed green on a reverted driver. The shape to forbid here is
# the option string CLOSED and followed by a comma.
Check "...and never the space form (nor the array-split shape)" (-not ($ps1Code -match "--(reference-)?launcher( ['`"`$a-z]|['`"]\s*,)"))
Check "the .sh emits the ``=`` form too"                      ($shCode -match '--launcher=\$_t')

# ★ BEHAVIOURAL RED-ON-DISABLE, against the REAL shipped cli-smoke.py parser -
# not a restatement of the source text above. Both invocations carry the SAME two
# tokens; only the argv FORM differs.
# ★ PINNED WITH A DASH-LEADING TOKEN ON PURPOSE. Every launcher token this harness
# has ever actually run starts with a LETTER (wine, qemu-aarch64, qemu-x86_64,
# wsl.exe) and against those the two forms behave identically - so a test written
# with one of them would pass on the broken code and prove nothing. `arch -x86_64`
# is what legs.json DECLARES for macho64-x86_64 on a darwin/arm64 host, i.e. the
# real token that fires this the first time that leg runs on the operator's Mac.
if (-not $py) {
  "  SKIP no python3 on PATH - the launcher argv form was NOT exercised against cli-smoke.py (2 assertions skipped)"
  $skip += 2
} else {
  $smokePy = Join-Path $PSScriptRoot 'cli-smoke.py'
  # Nothing is built and no binary runs: argparse decides long before any work.
  $spaceOut = (& $py.Source $smokePy '--launcher' 'arch' '--launcher' '-x86_64' 2>&1) -join "`n"
  $eqOut    = (& $py.Source $smokePy '--launcher=arch' '--launcher=-x86_64'      2>&1) -join "`n"
  Check "RED-ON-DISABLE: the SPACE form with ``arch -x86_64`` is REFUSED by argparse" `
        ($spaceOut -match 'argument --launcher: expected one argument')
  # Asserted POSITIVELY, by the error the `=` form reaches INSTEAD (missing
  # required args) - "it did not say X" is satisfied by a tool that says nothing.
  Check "...while the ``=`` form consumes BOTH tokens and reaches the required-arg check" `
        ($eqOut -match 'the following arguments are required: --cli')
}

# ═══════════════════════════════════════════════════════════════════════════
# THE SUPPLY - WHERE THAT PATTERN ARRAY CAME FROM
# ═══════════════════════════════════════════════════════════════════════════
# ★★★ THIS SECTION EXISTS BECAUSE EVERYTHING ABOVE IT IS ABOUT THE MATCHER, AND
# THE DEFECT WAS IN THE SUPPLY.
#
# Every classifier assertion at the top of this file sets `$Confounds = @(...)`
# BY HAND and then runs the shipped block over it. That pinned the matching in
# real detail while the question "where does that array COME FROM" was asked by
# NOTHING - so `Get-LegConfounds`, whose entire body was
# `if ($legLabel -eq 'pe64-x86_64') { return $PeEarnedConfounds }`, was never
# executed by a test. It handed a LINUX-earned list to the one leg that could not
# use it (zipfile-25.0 turns on POSIX fopen() succeeding on a directory) and an
# EMPTY list to the legs that had earned it, and this file stayed green.
# ⇒ D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG. Same lesson as the .sh
# twin's: a pin that supplies its subject's input by hand is testing the stub.
#
# EXTRACTED AND RUN, never re-implemented - same discipline as the classifier.
"--- THE SUPPLY: Get-LegConfounds, extracted from the shipped driver ---"
$supStart = ($lines | Select-String -Pattern '^function Get-LegConfounds\(' | Select-Object -First 1).LineNumber
$supEnd = $null
if ($supStart) {
  for ($i = $supStart; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -eq '}') { $supEnd = $i + 1; break }   # first closing brace at column 0
  }
}
if (-not $supStart -or -not $supEnd) {
  "  FAIL could not locate Get-LegConfounds in the shipped driver - this pin would assert over nothing"
  $fail++
  # The 6 assertions below cannot run; count them so the accounting invariant
  # cannot be satisfied by silently losing them.
  $skip += 6
} else {
  Invoke-Expression (($lines[($supStart-1)..($supEnd-1)]) -join "`n")
  "extracted $($supEnd - $supStart + 1) supply lines from the shipped script"
  # The resolved-leg shape harness_legs.py --plan produces, as ConvertFrom-Json
  # hands it to the driver: a PSCustomObject with a `confounds` array.
  function New-PinLeg($label, $confounds) {
    return [pscustomobject]@{ label = $label; confounds = $confounds }
  }
  $ConfoundsOverride = $null
  $earned = @(Get-LegConfounds (New-PinLeg 'elf64-x86_64' @('^walsetlk-','^busy2-','^zipfile-25\.0$')))
  CheckEq "a leg gets ITS OWN declared patterns" '^walsetlk- ^busy2- ^zipfile-25\.0$' ($earned -join ' ')
  # ★ THE HEADLINE ASSERTION, AND THE ONE THAT WOULD HAVE CAUGHT THE DEFECT: the
  # answer follows the leg's DECLARATION and nothing else - not its label.
  $empty = @(Get-LegConfounds (New-PinLeg 'pe64-x86_64' @()))
  CheckEq "a leg declaring [] inherits NOTHING" '' ($empty -join ' ')
  $other = @(Get-LegConfounds (New-PinLeg 'elf64-arm64' @('^busy2-','emulated:^writecrash-')))
  CheckEq "...a DIFFERENT leg gets a DIFFERENT set" '^busy2- emulated:^writecrash-' ($other -join ' ')
  # The scope prefix must survive the supply: stripped here, the qemu-only
  # writecrash excusal silently becomes a bare one and suppresses a future genuine
  # regression on a native run - which every classifier assertion above would
  # still call correct.
  Check "the 'emulated:' scope survives the supply" ($other -contains 'emulated:^writecrash-')
  # THE OPERATOR OVERRIDE still applies to EVERY leg - stating intent, not
  # inheriting one - including to a leg that declares nothing.
  $ConfoundsOverride = @('^operator-1','^operator-2')
  $ov = @(Get-LegConfounds (New-PinLeg 'pe64-x86_64' @()))
  CheckEq "the operator override reaches EVERY leg" '^operator-1 ^operator-2' ($ov -join ' ')
  $ConfoundsOverride = $null
  # ⚠ AN UNDECLARED LEG IS A TRANSPORT DEFECT, NOT AN EMPTY LIST. harness_legs.py
  # refuses to plan a leg with no `confounds`, so an absent field means the plan
  # this driver read is not the plan that file produces - and answering @() would
  # report every failure on that leg as a DSS defect on the strength of a bug.
  # `Die` is the stub installed for the provenance section above, which THROWS.
  $refused = ''
  try { [void](Get-LegConfounds ([pscustomobject]@{ label = 'macho64-arm64' })) }
  catch { $refused = "$($_.Exception.Message)" }
  Check "an UNDECLARED leg REFUSES rather than answering @()" ($refused -match 'transport defect')
}

""
"passed=$pass failed=$fail skipped=$skip"
# ── the accounting invariant, ENFORCED (see $TotalAssertions at the top) ─────
# Checked AFTER the summary line so the numbers a reader (and the driver's parse)
# needs are already on stdout when it fires.
$accounted = $pass + $fail + $skip
if ($accounted -ne $TotalAssertions) {
  "  FAIL ACCOUNTING: $accounted assertion(s) accounted for, but this file declares TotalAssertions=$TotalAssertions"
  "       (drift of $($accounted - $TotalAssertions); negative = fewer ran/were counted than declared)."
  "       Either an assertion was added or removed without updating `$TotalAssertions, or the SKIP"
  "       branch's hand-written count no longer matches the block it stands in for. Both make this"
  "       self-test report a pass over assertions it never ran. Fix the count — never delete this check."
  exit 1
}
if ($fail -gt 0) { exit 1 }
