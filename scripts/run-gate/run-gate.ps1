<#
.SYNOPSIS
    Run a gate command and REFUSE to report success without evidence it ran.

.DESCRIPTION
    PowerShell sibling of scripts/run-gate/run-gate.sh. Same contract, same exit codes.

    ★★★ WHY THIS EXISTS: a gate reporting exit 0 that never executed has now
    happened THREE times in this project's record, each by a different
    mechanism and each caught only by a human reading the log:
      1. a test suite printing `failed=0` while exiting 2 (weeks undetected);
      2. a probe whose rc was read AFTER a pipe, reporting the pipe's status;
      3. a `cd build-dbg && ctest ...` chain piped through tee/tail and
         followed by an echo of PIPESTATUS -- the `cd` failed (the shell was
         already there), no test ran, and the TRAILING echo succeeded, so the
         whole chain exited 0.
    Vigilance is the wrong mechanism for a recurring failure: occurrences (1)
    and (2) each produced a resolution to be careful, and (3) happened anyway.

    ★★ THE CONTRACT, BOTH HALVES LOAD-BEARING:
      * rc is captured DIRECTLY from the command -- never after a pipe, never
        from a following statement;
      * rc == 0 is NOT sufficient. The output must ALSO match a caller-supplied
        success witness. A command exiting 0 without producing evidence of work
        is indistinguishable from one that never ran, so it is a FAILURE.

    ⚠ WHY THIS SCRIPT REDIRECTS WITH `*>` RATHER THAN PIPING TO Out-File:
    piping a native command's output would put a pipeline between the command
    and the `$LASTEXITCODE` read -- which is occurrence (2) exactly, rebuilt
    inside the tool written to prevent it. The redirection operator keeps the
    native command last, so the exit code that is read is the one that matters.

    ⚠ AND WHY IT IS A SIBLING AT ALL: this project ships every tool as a
    .sh/.ps1 PAIR (check-anchor-registry, check-line-endings, check-orphan-tests,
    ssh-arm64-vps, ssh-macos). A pair where only one side exists is a silent
    harness bug of the same family the pair discipline exists to prevent -- the
    Windows host is where this project's primary ctest leg runs, so a bash-only
    gate wrapper is a gate wrapper that the main gate cannot use.

.PARAMETER LogPath
    File to receive the command's combined output. TRUNCATED, never appended --
    a stale log is itself a way to "find" a witness this run never produced.

.PARAMETER SuccessPattern
    Regex that MUST appear in the output, e.g. '100% tests passed'.

.PARAMETER Command
    The executable to run.

.PARAMETER CommandArgs
    Remaining arguments, passed through verbatim.

.EXAMPLE
    scripts/run-gate/run-gate.ps1 build/ctest.log '100% tests passed' ctest --test-dir build/dbg --output-on-failure
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$LogPath,
    [Parameter(Mandatory = $true, Position = 1)][string]$SuccessPattern,
    [Parameter(Mandatory = $true, Position = 2)][string]$Command,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$CommandArgs
)

$ErrorActionPreference = 'Continue'

# Truncate up front (see LogPath above).
try {
    Set-Content -LiteralPath $LogPath -Value $null -NoNewline -ErrorAction Stop
} catch {
    Write-Error "run-gate.ps1: cannot write log '$LogPath'"
    exit 2
}

if ($null -eq $CommandArgs) { $CommandArgs = @() }

# ── DEFAULT TEST PARALLELISM (mirror of run-gate.sh; see the reasoning there) ──
# This wrapper runs an ARBITRARY command, so it sets ctest's own env channel
# rather than splicing `-j 8` into a caller's argv. An explicit `-j` still wins.
# ✔MEASURED 2026-08-19 (ctest 4.3.2, 16C/32T), six example tests: no level
# 9741 ms; CTEST_PARALLEL_LEVEL=8 2648 ms; explicit -j 8 2446 ms; env 8 with
# -j 1 9669 ms. 8 rather than all cores is an operator instruction: the Windows
# and WSL gate legs run concurrently here by design.
# ★★ SCOPED TO THE CHILD, AND THE try/finally IS THE WHOLE POINT. A .ps1 runs
# IN-PROCESS, so a bare assignment here outlives this script and every later
# hand-run ctest in the caller's shell would be silently 8-way parallel with
# nothing on screen saying so -- while the .sh twin's `export` dies with its own
# process. ✔MEASURED by audit: the caller's CTEST_PARALLEL_LEVEL read empty
# before the call and 8 after it. The twins' headers claim "same contract"; for
# the environment that was false until this block.
$__cplPrev = $env:CTEST_PARALLEL_LEVEL
$__cplSet  = $false
if (-not $env:CTEST_PARALLEL_LEVEL) {
    $env:CTEST_PARALLEL_LEVEL = '8'
    $__cplSet = $true
}
function Restore-CplDefault {
    if ($script:__cplSet) {
        if ($null -eq $script:__cplPrev) {
            Remove-Item Env:\CTEST_PARALLEL_LEVEL -ErrorAction SilentlyContinue
        } else {
            $env:CTEST_PARALLEL_LEVEL = $script:__cplPrev
        }
        $script:__cplSet = $false
    }
}
trap { Restore-CplDefault; break }

# Redirect ALL streams to the log with `*>` so the native command stays last
# and $LASTEXITCODE is its own, not a pipeline's.
try {
    & $Command @CommandArgs *> $LogPath
    $rc = $LASTEXITCODE
} finally {
    Restore-CplDefault
}
if ($null -eq $rc) {
    # A non-native command (cmdlet/function) leaves $LASTEXITCODE unset. Treat
    # "no exit code at all" as absence of evidence, not as success.
    $rc = 0
}

Add-Content -LiteralPath $LogPath -Value @"
--- run-gate.ps1 ---
command : $Command $($CommandArgs -join ' ')
rc      : $rc
"@

function Show-Tail {
    if (Test-Path -LiteralPath $LogPath) {
        Get-Content -LiteralPath $LogPath -Tail 20 | ForEach-Object { Write-Host $_ }
    }
}

if ($rc -ne 0) {
    Write-Host "run-gate.ps1: FAIL - command exited $rc (log: $LogPath)"
    Show-Tail
    exit $rc
}

if (-not (Select-String -LiteralPath $LogPath -Pattern $SuccessPattern -Quiet)) {
    Write-Host "run-gate.ps1: FAIL - command exited 0 but its output never matched the"
    Write-Host "  success witness /$SuccessPattern/, so there is NO EVIDENCE it did any work."
    Write-Host "  An exit code alone cannot distinguish 'passed' from 'never ran'."
    Write-Host "  (log: $LogPath)"
    Show-Tail
    exit 1
}

Write-Host "run-gate.ps1: OK - rc=0 and the success witness /$SuccessPattern/ was present."
exit 0
