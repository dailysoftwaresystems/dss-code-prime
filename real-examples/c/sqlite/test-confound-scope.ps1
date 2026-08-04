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
$TotalAssertions = 29     # 11 classifier + 18 checkout-provenance
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
