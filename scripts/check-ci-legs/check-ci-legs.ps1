# PURPOSE: read the PR's CI verdict per leg from job METADATA, which outlives the logs, and separate a real test failure from a budget overrun.
#
# The .ps1 twin of check-ci-legs.sh. Same inputs, same properties, same flags, same
# exit codes — and it exists because the Windows leg is this project's PRIMARY gate
# host, so a bash-only capability is one the main gate cannot use.
#
# The WHY, the measurements behind it, and the reasoning about what the job
# metadata can and cannot answer live ONCE, in the `.sh` header. Read that file for
# the argument; this one carries only what differs in the implementation.
#
# ⚠ SAME DEPENDENCY SET, AND IT IS `gh` ALONE. `gh api --jq` uses gh's BUILT-IN jq;
# neither twin may pipe to a standalone `jq`. ✔MEASURED: there is no `jq` on this
# workstation's PATH, and a draft that piped to one printed `command not found` and
# still exited 0 — a missing dependency reading as "no leg failed".
#
# Usage:
#   pwsh -NoProfile -File scripts/check-ci-legs/check-ci-legs.ps1
#   pwsh -NoProfile -File scripts/check-ci-legs/check-ci-legs.ps1 -Limit 6
#   pwsh -NoProfile -File scripts/check-ci-legs/check-ci-legs.ps1 -Branch <name>
#   pwsh -NoProfile -File scripts/check-ci-legs/check-ci-legs.ps1 -Run 123,456
#
# [!] NO ctest ENTRY READS CI, for the reason stated in the .sh header: that needs the
# network and an authenticated `gh`, so it would red for a property of the machine rather
# than of the tree. The READ was proved by EXECUTION, both twins, against six real Pipeline
# runs. The LOCAL half -- which branch is asked about, which repository gh's own git sees,
# and the GH_REPO refusal -- is pinned hermetically for both twins by `test-check-ci-legs.py`.
#
# Exit: 0 every leg green - 1 at least one leg red - 2 the instrument could not run.
# [!] 2 is NOT "green": an instrument that could not look must never read as a pass.
[CmdletBinding()]
param(
    [string]$Workflow = 'Pipeline',
    [string]$Branch = '',
    [int]$Limit = 1,
    [string[]]$Run = @()
)

$ErrorActionPreference = 'Continue'

# Run from the repo root, resolved from this script's own location - twin of the same
# step in the .sh. `gh` resolves the repository from the working directory, and the
# workflow-file fallback below is a repo-relative path.
Set-Location (Join-Path $PSScriptRoot '..\..')

# Neither git NOR gh is asked in the caller's git environment -- twin of the block in the .sh,
# which carries the measurement: under another repository's GIT_DIR (or GIT_DIR + GIT_WORK_TREE)
# both twins read THAT repository's branch, and gh -- which finds `:owner/:repo` by running git --
# saw THAT repository's origin even with -Branch given. Every git and gh call below goes through
# `Invoke-RepoTreeUnsteered`, the PowerShell owner of that removal. Dot-sourced, never re-spelled.
# Pinned hermetically (a stub gh, no network) by `test-check-ci-legs.py` beside this file.
. (Join-Path $PSScriptRoot '..\repo-tree\repo-tree.ps1')

# A caller's GH_REPO is REFUSED -- not honoured, not stripped. The reason is the .sh twin's: it is
# gh's explicit override, it makes gh answer for the repository it names, and this tool reads only
# the CI of the tree it lives in. Exit 2: the instrument did not run.
if (-not [string]::IsNullOrEmpty($env:GH_REPO)) {
    [Console]::Error.WriteLine("check-ci-legs: FATAL -- GH_REPO is set ($($env:GH_REPO)), so gh would answer for THAT repository; this tool reads only the CI of the tree it lives in. Unset GH_REPO for this call. CI was NOT read (this is not a pass)")
    exit 2
}

# The matrix builder's own source, read ONLY when GitHub truncated a job name out of
# its budget fields - see the .sh header for why that fallback exists at all (the
# `linux-clang-asan` name is the truncated one, and it is the leg nearest its cap).
$WorkflowFile = '.github/workflows/pipeline-pr.yml'

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    [Console]::Error.WriteLine('check-ci-legs: FATAL -- `gh` is not on PATH; CI was NOT read (this is not a pass)')
    exit 2
}

$runs = @($Run)
if ($runs.Count -eq 0) {
    if (-not $Branch) { $Branch = (Invoke-RepoTreeUnsteeredGit @('rev-parse', '--abbrev-ref', 'HEAD')) }
    if (-not $Branch -or $Branch -eq 'HEAD') {
        [Console]::Error.WriteLine('check-ci-legs: FATAL -- no branch to ask about (detached HEAD?); pass -Branch or -Run')
        exit 2
    }
    $runs = @(Invoke-RepoTreeUnsteered -Command 'gh' -Arguments @('run', 'list', '--workflow', $Workflow, '--branch', $Branch, '--limit', "$Limit", '--json', 'databaseId', '--jq', '.[].databaseId') -DiscardStandardError)
    if ($runs.Count -eq 0) {
        [Console]::Error.WriteLine("check-ci-legs: FATAL -- no ``$Workflow`` run found for branch $Branch; CI was NOT read")
        exit 2
    }
}

# The SAME jq program the twin runs, kept in step BY REVIEW: both read the Build and
# Test steps of every `run-tests` job, both emit the leg name, the two conclusions,
# the two durations and the ctest budget parsed out of the job name, and both emit a
# single NO-MATRIX row when `run-tests` never ran.
$jqProgram = @'
[.jobs[] | select(.name|test("run-tests \\("))] as $m |
if ($m|length) == 0 then
    "NO-MATRIX\t" + ([.jobs[] | .name + "=" + (.conclusion // "?")] | join(" "))
else
    ($m[] | . as $j |
    (($j.steps // []) | map(select(.name=="Build")) | first) as $b |
    (($j.steps // []) | map(select(.name=="Test"))  | first) as $t |
    [ ($j.name | capture("run-tests \\((?<leg>[^,]+)").leg),
      $j.conclusion,
      "build=" + (($b.conclusion) // "-"),
      "test="  + (($t.conclusion)  // "-"),
      "build_s=" + (if $b and $b.started_at and $b.completed_at
                    then ((($b.completed_at|fromdate) - ($b.started_at|fromdate))|tostring) else "-" end),
      "test_s="  + (if $t and $t.started_at and $t.completed_at
                    then ((($t.completed_at|fromdate) - ($t.started_at|fromdate))|tostring) else "-" end),
      "budget_s=" + ((($j.name | capture(", (?<b>[0-9]+), [0-9]+\\)$") | .b) // "?")
                     | if . == "?" then "?" else ((tonumber * 60)|tostring) end)
    ] | @tsv)
end
'@

$rc = 0
foreach ($r in $runs) {
    $meta = Invoke-RepoTreeUnsteered -Command 'gh' -Arguments @('api', "repos/:owner/:repo/actions/runs/$r", '--jq', '[.head_sha[0:8], .head_branch, .created_at, .conclusion] | @tsv') -DiscardStandardError
    if (-not $meta) {
        [Console]::Error.WriteLine("check-ci-legs: FATAL -- run $r could not be read")
        exit 2
    }
    Write-Output ''
    Write-Output "=== run $r  $meta"

    $legs = @(Invoke-RepoTreeUnsteered -Command 'gh' -Arguments @('api', "repos/:owner/:repo/actions/runs/$r/jobs?per_page=100", '--jq', $jqProgram) -DiscardStandardError)
    # An EMPTY answer is FATAL, not a green run - twin of the same refusal in the .sh.
    if ($legs.Count -eq 0) {
        [Console]::Error.WriteLine("check-ci-legs: FATAL -- run $r returned NO job rows; nothing was verified")
        exit 2
    }

    if ($legs[0] -like 'NO-MATRIX*') {
        Write-Output '  [!] THE MATRIX DID NOT RUN in this run - `run-tests` is absent/skipped, so it says'
        Write-Output ("    NOTHING about the tree. Jobs present: " + ($legs[0] -replace '^NO-MATRIX\t', ''))
        Write-Output '    (on this repo that is normally the `Run Pipes` label being absent.)'
        continue
    }

    foreach ($line in $legs) {
        $f = $line -split "`t"
        if ($f.Count -lt 7) { continue }
        $leg = $f[0]; $concl = $f[1]; $b = $f[2]; $t = $f[3]; $ts = $f[5]; $budget = $f[6]
        $used = $ts -replace '^test_s=', ''
        $cap  = $budget -replace '^budget_s=', ''
        $capSrc = ''
        if ($cap -eq '?' -and (Test-Path -LiteralPath $WorkflowFile)) {
            $wf = Select-String -LiteralPath $WorkflowFile -Pattern ("`"name`":`"" + [regex]::Escape($leg) + "`".*`"ctest_budget_min`":([0-9]+)") |
                  Select-Object -First 1
            if ($wf) { $cap = [string]([int]$wf.Matches[0].Groups[1].Value * 60); $capSrc = ' (workflow)' }
        }
        $note = ''
        if ($t -eq 'test=failure' -and $used -ne '-' -and $cap -ne '?') {
            if ([int]$used -ge [int]$cap) {
                $note = '  <- AT OR OVER THE ctest BUDGET: read this as a possible overrun, not a test failure'
            } else {
                $note = "  <- REAL TEST FAILURE: ${used}s of a ${cap}s budget, nowhere near the cap. FIX IT; do not raise the cap"
            }
        } elseif ($t -eq 'test=success' -and $used -ne '-' -and $cap -ne '?') {
            if ([int]$used -gt ([int]$cap * 8 / 10)) {
                $note = '  <- GREEN BUT ABOVE 80% OF ITS BUDGET; re-derive it before it reds (D-CI-ASAN-LEG-WALL-CLOCK-GROWS-WITH-THE-CORPUS)'
            }
        }
        Write-Output ('  {0,-26} {1,-8} {2,-16} {3,-14} {4,-12} {5}{6}{7}' -f $leg, $concl, $b, $t, $ts, "$cap s cap", $capSrc, $note)
        if ($concl -eq 'failure') { $rc = 1 }
    }
}

if ($rc -ne 0) {
    Write-Output ''
    Write-Output 'check-ci-legs: RED. At least one CI leg failed.'
    Write-Output '  [X] This is a HARD STOP for the cycle, not a retry. Reproduce the failing leg'
    Write-Output '      LOCALLY in its own configuration - the logs behind these verdicts expire in'
    Write-Output '      three days and a red leg nobody reproduces becomes a red leg nobody can.'
}
exit $rc
