# Verifies the .ps1 scoped-confound classifier by EXTRACTING the shipped block,
# not by re-implementing it. This driver has only a NATIVE leg, so the contract is:
# bare patterns excuse; `emulated:` patterns must NEVER excuse; `native:` patterns do.
$ErrorActionPreference = 'Stop'
$sh = Join-Path $PSScriptRoot 'build-and-test.ps1'
$lines = Get-Content $sh
$start = ($lines | Select-String -Pattern '^\$real = @\(\); \$confound = @\(\); \$scopedExcused = @\(\)' | Select-Object -First 1).LineNumber
$end   = ($lines | Select-String -Pattern '^\s*Warn "\[pe64\] \$\(\$scopedExcused\.Count\) failure' | Select-Object -First 1).LineNumber
if (-not $start -or -not $end) { throw "could not locate the shipped classifier block (start=$start end=$end)" }
# LineNumber is 1-based, the array 0-based: line N is at index N-1. Take through the
# Warn line and supply the closing brace of `if ($scopedExcused.Count) {` ourselves.
$block = ($lines[($start-1)..($end-1)] -join "`n") + "`n}"
"extracted $($end - $start + 1) lines from the shipped script"

function Warn($m) { "      WARN: $m" }

$pass = 0; $fail = 0
function Check($label, $cond) {
  if ($cond) { "  ok   $label"; $script:pass++ } else { "  FAIL $label"; $script:fail++ }
}

$failNames = @('writecrash-1.1.1','walsetlk-2.1.3','zipfile-25.0','sometest-9.9')

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

""
"passed=$pass failed=$fail"
if ($fail -gt 0) { exit 1 }
