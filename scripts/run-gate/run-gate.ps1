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
# ⛔ THIS PARAM BLOCK IS DELIBERATELY PLAIN. DO NOT ADD [CmdletBinding()], AND DO
# NOT ADD A [Parameter()] ATTRIBUTE TO ANY OF THESE — EITHER ONE RE-BREAKS IT.
# D-GATE-RUN-GATE-PS1-SILENTLY-DROPS-A-COMMON-PARAMETER-PREFIX-FROM-THE-GATE-COMMAND
#
# ✔MEASURED 2026-08-23 (cycle P29) with a child script echoing its own argv, three
# variants of this block:
#   [CmdletBinding()] + [Parameter()] attrs + ValueFromRemainingArguments
#       -> `-V`, `-v`, `-D` VANISH from the pass-through array
#   [Parameter()] attrs + ValueFromRemainingArguments, no [CmdletBinding()]
#       -> `-V`, `-v`, `-D` STILL VANISH
#   plain param(), rest collected from $args   (this one)
#       -> `--test-dir | build/dbg | -V | -R | foo | -D | x | -v` — all present
#
# ★ THE ROOT CAUSE IS NOT [CmdletBinding()]. A [Parameter()] attribute on ANY
# parameter makes a script ADVANCED on its own, and an advanced script's COMMON
# PARAMETER binder claims `-Verbose`/`-Debug` and their unambiguous prefixes BEFORE
# the remaining-arguments array is built. Deleting only [CmdletBinding()] looks like
# the fix, changes nothing, and leaves a green gate over the same silent loss — so
# the first write-up of this defect named half of it, and the half it named was the
# half that did not matter.
#
# ⚠ WHY IT IS WORTH THIS MUCH COMMENT: the flags dropped are not exotic. `ctest -V`
# is precisely the flag this repo uses to PROVE a registered entry actually executes
# its self-test arms — ✔a guard registered by an entry passing no flag was measured
# this same cycle running ZERO arms. So the one instrument for "did this guard run
# anything" was itself being silently discarded. rc stays 0, the success witness
# still matches, and the footer below prints the command with the flag ALREADY GONE,
# so the log is self-consistent and the loss is invisible in review.
# ⓘ `-E`/`-O`/`-W` die loudly as ambiguous; `-N`/`-j`/`-R`/`-Z` were never affected.
# ✔The `.sh` twin uses "$@" and never had this: a live .sh/.ps1 divergence inside the
# tool whose own header names divergence as its subject.
# ⛔ AND THE PARAM BLOCK IS **EMPTY** — DO NOT DECLARE $LogPath/$SuccessPattern/
# $Command AS PARAMETERS EITHER. NAMING THEM RE-BREAKS IT A THIRD WAY.
# ✔MEASURED 2026-08-24, after the [Parameter()] fix above: a plain
# `param($LogPath, $SuccessPattern, $Command)` still does NAME binding, and
# `-c` is an unambiguous PREFIX of `-Command`. So
#     run-gate.ps1 <log> <witness> python -c "import sys; sys.exit(7)"
# bound `$Command = "import sys; sys.exit(7)"` and left `python` in the rest,
# ARGUMENTS REORDERED. The wrapper then ran the wrong thing and reported **127**
# where the `.sh` twin reported the command's real **7** — and its footer printed
# `command : exit 7 bash`, i.e. the reordering was visible in the log and still
# read as a plausible failure. ⚠ `ctest -C Debug` is the everyday casualty, and it
# is TOTAL: ✔MEASURED against the binder, `<log> <witness> ctest --test-dir build/dbg
# -C Debug -j 8` bound **$Command = "Debug"** and left `ctest --test-dir build/dbg
# -j 8` in the rest — so the wrapper would try to EXECUTE a program named `Debug`,
# fail 127, and report "the gate command was NOT FOUND": a true sentence about a
# command the caller never wrote. ★ Every name declared here donates its unambiguous
# PREFIXES to the binder, matched against a namespace the caller cannot see; `-C`,
# `-c`, `-S` and `-L` were all live. That is why the block must stay EMPTY.
# ⚠⚠ `-L` IS THE WORST, and it is an ordinary ctest label filter: ✔MEASURED,
# `<log> <witness> ctest -L smoke -j 8` shifts ALL THREE positionals by one —
# $LogPath="smoke", $SuccessPattern=<the log path>, $Command=<the witness regex>.
# The wrapper would then WRITE ITS LOG TO A FILE NAMED `smoke` in the cwd, a path
# the caller never named and would never look for, and try to execute a program
# called `100% tests passed`. Declaring even ONE name puts the caller's whole
# argv at the binder's mercy.
# ✔The empty form preserves order and every flag, measured with a child echoing
# its own argv: `-c`, `-C Debug` and `-V` all arrive, in position.
param()

$__argv = @($args)
# Mandatory/positional is enforced HERE rather than by a parameter declaration,
# because the declaration is what breaks the pass-through. The refusal text is what
# [Parameter(Mandatory)] would have produced, minus the interactive prompt — correct
# for a gate wrapper: a gate that stops to ask a question in CI has already failed.
if ($__argv.Count -lt 3) {
    Write-Host "run-gate.ps1: FAIL - expected at least 3 arguments, got $($__argv.Count)."
    Write-Host "  usage: run-gate.ps1 <log-path> <success-regex> <command> [args...]"
    exit 2
}
$LogPath        = [string]$__argv[0]
$SuccessPattern = [string]$__argv[1]
$Command        = [string]$__argv[2]
foreach ($required in @(
        @{ Name = 'LogPath';        Value = $LogPath },
        @{ Name = 'SuccessPattern'; Value = $SuccessPattern },
        @{ Name = 'Command';        Value = $Command })) {
    if ([string]::IsNullOrEmpty($required.Value)) {
        Write-Host "run-gate.ps1: FAIL - required argument '$($required.Name)' is empty."
        Write-Host "  usage: run-gate.ps1 <log-path> <success-regex> <command> [args...]"
        exit 2
    }
}
$CommandArgs = if ($__argv.Count -gt 3) { $__argv[3..($__argv.Count - 1)] } else { @() }

$ErrorActionPreference = 'Continue'

# ── WHICH SHELL IS ACTUALLY RUNNING THIS, NAMED IN EVERY REFUSAL ────────────
#
# ★★ A GATE THAT REFUSES MUST SAY WHICH REFUSAL IT IS -- the mirror of the same
# block in run-gate.sh, added in the same commit for the same reason.
#
# ✔MEASURED 2026-08-20, and the .ps1 half was the WORSE of the two: given a
# command it cannot resolve, `& $Command` raises a TERMINATING error, so this
# script died before its own footer, left a ZERO-BYTE log, printed a raw
# PowerShell "is not recognized as a name of a cmdlet" on the caller's console
# instead of into the log, and exited **1** -- the exit code this wrapper
# reserves for "ran, but produced no witness". The .sh twin reports the same
# condition as **127**. Same input, two different exit codes and one of them
# actively misleading: that is precisely the divergence class this project keeps
# paying for, inside the tool written to make gates trustworthy.
#
# ⇒ The command is RESOLVED FIRST, the refusal is written into the LOG as well
# as to the console, and it exits 127 -- the .sh's number for "not found".
#
# ⓘ THIS NAMES, IT DOES NOT TRANSLATE. Rewriting a path into the other shell's
# spelling would make this file a second path canonicaliser; see
# scripts/check-path-identity. The refusal stands, it just stops being anonymous.
# ⚠ THE POWERSHELL IDENTITY LEADS, THE HOST PROCESS TRAILS, and that order is a
# measurement rather than a preference: ✔MEASURED 2026-08-20, the running
# process path for pwsh 7.5.2 on this box is `C:\Program Files\dotnet\dotnet.exe`
# (pwsh is a dotnet-hosted app), so a refusal that led with the process path
# named something that is not a shell at all. `$PSHOME` is the one value that
# always points at the PowerShell that is actually executing this file.
function Get-RunGateShellIdentity {
    $exe = try { [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName }
           catch { '<unknown host process>' }
    $plat = if ($PSVersionTable.Platform) { $PSVersionTable.Platform } else { 'Win32NT (Windows PowerShell)' }
    return "PowerShell $($PSVersionTable.PSVersion) $($PSVersionTable.PSEdition) on $plat (PSHOME: $PSHOME; host process: $exe)"
}
# A path handed to this shell that begins with a POSIX root or a WSL mount --
# reported, not repaired. Windows PowerShell cannot open '/mnt/c/...' any more
# than a WSL bash can open 'C:\...'; it is the same defect seen from the other
# side, which is why the twins carry the same check with the shapes swapped.
function Test-RunGateForeignPath([string]$p) {
    return ($p -match '^/')
}

# ── A LOG PATH THAT BEGINS WITH '-' IS REFUSED, BY NAME, BEFORE ANYTHING OPENS ──
# Mirror of run-gate.sh's block; the measurement, the reason the rule is the LEADING
# DASH rather than a PowerShell parameter SHAPE, and the './-name' escape all live
# there. This twin is where the defect was OBSERVED: `-LogPath <path> …` bound the
# NAME as $__argv[0] (the param block is empty on purpose, see above), and the script
# then created a file literally called '-LogPath' in the repo root. Placed here, next
# to Get-RunGateShellIdentity and ahead of the truncate, for the same reason and in
# the same order as the twin.
if ($LogPath.StartsWith('-')) {
    Write-Host "run-gate.ps1: FAIL - the log path '$LogPath' begins with '-', so nothing was run."
    Write-Host "  This refusal is about the LOG PATH, not about the gate command."
    Write-Host "  shell   : $(Get-RunGateShellIdentity)"
    Write-Host "  This wrapper's interface is POSITIONAL and it accepts NO named parameters:"
    Write-Host "      run-gate.ps1 <log-path> <success-regex> <command> [args...]"
    Write-Host "  If you meant '-LogPath'/'-SuccessPattern'/'-Command' as PowerShell named"
    Write-Host "  parameters, drop the names and pass the three values in that order - this"
    Write-Host "  script's param() block is EMPTY ON PURPOSE (declaring them breaks the"
    Write-Host "  argument pass-through it exists to preserve), so a name binds as a VALUE."
    Write-Host "  Refused rather than honoured because creating it would leave a stray file"
    Write-Host "  named '$LogPath' behind, and every later tool that receives that path reads"
    Write-Host "  a leading '-' as an OPTION - including the twin's own grep and tail."
    Write-Host "  If you really do want that filename, spell it './$LogPath'."
    exit 2
}

# Truncate up front (see LogPath above).
try {
    Set-Content -LiteralPath $LogPath -Value $null -NoNewline -ErrorAction Stop
} catch {
    Write-Host "run-gate.ps1: FAIL - cannot create the log '$LogPath', so nothing was run."
    Write-Host "  This refusal is about the LOG PATH, not about the gate command."
    Write-Host "  shell   : $(Get-RunGateShellIdentity)"
    Write-Host "  script  : $PSCommandPath"
    Write-Host "  cwd     : $((Get-Location).Path)"
    if (Test-RunGateForeignPath $LogPath) {
        Write-Host "  [!] that log path is POSIX-ROOTED ('/...'), and the shell named above is the one"
        Write-Host "      that has to open it. A Windows PowerShell has no '/mnt/c' and no '/tmp'; that"
        Write-Host "      spelling belongs to a WSL/Linux shell. Hand this script a path THIS shell can"
        Write-Host "      see; a repo-relative path works from either side."
        Write-Host "      This script deliberately does NOT rewrite the path for you: one canonicaliser,"
        Write-Host "      see scripts/check-path-identity."
    } else {
        Write-Host "  Check that the parent directory exists and is writable by this shell."
    }
    Write-Host "  reason  : $($_.Exception.Message)"
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

# ── DEFAULT: A FAILING TEST'S OWN OUTPUT GOES IN THE LOG ─────────────────────
# Mirror of run-gate.sh's block; the reasoning, and the measurement that ctest
# honours this variable, live there. Same env-channel argument as the level
# above (never argv injection), same in-process scoping obligation as the level
# above (a .ps1 assignment would otherwise outlive this script), and it prints
# only for tests that FAIL, so a green run's log is unchanged.
$__coofPrev = $env:CTEST_OUTPUT_ON_FAILURE
$__coofSet  = $false
if (-not $env:CTEST_OUTPUT_ON_FAILURE) {
    $env:CTEST_OUTPUT_ON_FAILURE = '1'
    $__coofSet = $true
}
# ── AND ACROSS THE WSL BOUNDARY, WHICH SETTING THEM DOES NOT CROSS ──────────
#
# ★★ THE TWIN HAD THIS AND THIS FILE DID NOT, so a `.ps1` gate that shells into
# WSL (`run-gate.ps1 <log> <witness> wsl.exe -e ctest …`) ran with BOTH defaults
# lost on the far side while this script believed it had set them. Windows->WSL
# forwards only the variables `WSLENV` names.
#
# ✔MEASURED 2026-08-24 from THIS host's PowerShell, `wsl.exe -e printenv <NAME>`:
#     no WSLENV ......................................... [] (empty)
#     WSLENV='CTEST_PARALLEL_LEVEL' ..................... [8]
#     WSLENV='CTEST_PARALLEL_LEVEL:CTEST_OUTPUT_ON_FAILURE' -> [8] and [1]
# i.e. a plain colon-separated NAME (no `/p`, `/u`, `/w`, `/l` flag) is what these
# two need, because neither is a path and neither needs translating.
#
# APPENDING, never overwriting — the twin's reason exactly: a caller may already
# be forwarding something of their own, and clobbering it would silently drop it.
# The membership test matches the twin's BARE-NAME test character for character so
# the two cannot diverge about what counts as already-present. ⓘ Neither twin
# recognises a flagged spelling (`CTEST_PARALLEL_LEVEL/p`) as the same entry; that
# is a shared narrowing, stated so it is not rediscovered as a bug in one of them.
#
# ★ AND IT IS RESTORED, for the reason the level above is: a .ps1 runs IN-PROCESS,
# so a bare assignment would outlive this script and silently re-route every later
# `wsl.exe` the caller runs. The .sh twin's `export` dies with its own process and
# owes nothing here.
$__wslenvPrev = $env:WSLENV
$__wslenvSet  = $false
foreach ($__n in @('CTEST_PARALLEL_LEVEL', 'CTEST_OUTPUT_ON_FAILURE')) {
    if ((':' + $env:WSLENV + ':') -notlike ('*:' + $__n + ':*')) {
        $env:WSLENV = if ($env:WSLENV) { $env:WSLENV + ':' + $__n } else { $__n }
        $__wslenvSet = $true
    }
}

function Restore-CplDefault {
    if ($script:__wslenvSet) {
        if ($null -eq $script:__wslenvPrev) {
            Remove-Item Env:\WSLENV -ErrorAction SilentlyContinue
        } else {
            $env:WSLENV = $script:__wslenvPrev
        }
        $script:__wslenvSet = $false
    }
    if ($script:__cplSet) {
        if ($null -eq $script:__cplPrev) {
            Remove-Item Env:\CTEST_PARALLEL_LEVEL -ErrorAction SilentlyContinue
        } else {
            $env:CTEST_PARALLEL_LEVEL = $script:__cplPrev
        }
        $script:__cplSet = $false
    }
    if ($script:__coofSet) {
        if ($null -eq $script:__coofPrev) {
            Remove-Item Env:\CTEST_OUTPUT_ON_FAILURE -ErrorAction SilentlyContinue
        } else {
            $env:CTEST_OUTPUT_ON_FAILURE = $script:__coofPrev
        }
        $script:__coofSet = $false
    }
}
trap { Restore-CplDefault; break }

# ★ RESOLVE argv[0] BEFORE RUNNING IT, so "not found" is its own named refusal
# with the .sh's exit code and not a raw PowerShell error over an empty log.
# See Get-RunGateShellIdentity above for the measurement. `Get-Command` resolves
# an application, a cmdlet, a function and an explicit path alike, so this
# rejects nothing the `&` below would have accepted.
$resolved = Get-Command -Name $Command -ErrorAction SilentlyContinue |
            Select-Object -First 1
if (-not $resolved) {
    $notFound = @(
        "run-gate.ps1: FAIL - the gate command was NOT FOUND, so it never ran (rc=127).",
        "  command : $Command $($CommandArgs -join ' ')",
        "  shell   : $(Get-RunGateShellIdentity)",
        "  This is NOT 'the gate failed' and NOT 'the witness was missing' - the shell named",
        "  above could not resolve argv[0] on ITS OWN PATH. 127 is the number run-gate.sh",
        "  reports for the same condition; the twins must not disagree about an exit code.",
        "  (log: $LogPath)"
    )
    # INTO THE LOG TOO. An empty log is the one artefact a reader cannot learn
    # anything from, and it is what this arm used to leave behind.
    Add-Content -LiteralPath $LogPath -Value ($notFound -join [Environment]::NewLine)
    Restore-CplDefault
    foreach ($l in $notFound) { Write-Host $l }
    exit 127
}

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

if ($rc -eq 127) {
    # The command WAS resolved above, so this 127 is the child's own -- which is
    # the one thing the .sh twin cannot tell you, because a POSIX shell reserves
    # 127 for "not found". Saying which it is here is not a divergence from the
    # twin; it is the extra fact this side actually has.
    Write-Host "run-gate.ps1: FAIL - the gate command exited 127 (log: $LogPath)."
    Write-Host "  command : $Command $($CommandArgs -join ' ')"
    Write-Host "  resolved: $(if ($resolved.Source) { $resolved.Source } else { $resolved.Name })"
    Write-Host "  It was FOUND and it RAN - 127 is its own exit code here, not 'command not found'"
    Write-Host "  (which this wrapper reports before starting anything, with the same 127)."
    Show-Tail
    exit $rc
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
