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

    THE EXIT CODES ARE PART OF THE CONTRACT AND THE TWO TWINS MUST AGREE:
      0   rc was 0 AND the success witness was present
      1   the command exited 0 but produced no witness -- no evidence it ran
      2   this wrapper refused before starting (bad usage, unwritable log/marker)
      3   THE SOURCE TREE MOVED under the run -- the verdict is not evidence
      4   ANOTHER RUN WAS LIVE IN THE SAME BUILD DIRECTORY -- likewise not evidence
      *   otherwise, the command's own exit code (127 gets its own sentence)
    ! 3 and 4 are DELIBERATELY DIFFERENT NUMBERS. Both mean "this run has no
    verdict", and a reader who cannot tell which of the two fired cannot tell
    whether to settle the tree or wait for a sibling -- two different remedies.

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
    # ★★★ OPERATOR RULING 2026-08-25: "never use all CPUS, the idea is to keep build + tests + run always at 4 cpus", AMENDED same-day to "make it 6 cores, not 4, everywhere".
    $env:CTEST_PARALLEL_LEVEL = '6'
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

# ---- THE RUN'S INPUTS MUST HOLD STILL, OR ITS VERDICT IS NOT EVIDENCE -------
#
# The twin of the block of the same name in run-gate.sh; that file carries the
# full argument and the measurement. In short: this project's runners read
# `src/dss-config/**`, `tests/corpus/**` and `examples/**` from the SOURCE TREE
# at TEST TIME, so an edit to one of them while a suite is in flight makes the
# run measure a tree that never existed as a whole.
#
# +MEASURED 2026-09-05 (P62): a whole-tree ctest reported 9 failures out of 2087;
# EIGHT were examples failing `C_UnbackedPredefinedMacro` about shipped-library
# descriptors, which reads as a defect in a sibling lane's FFI work -- and that
# is where the investigation went. The cause was the orchestrator rewriting
# `c.lang.json` mid-run. All eight passed on the stable tree seconds later.
#
# NO ESCAPE HATCH, deliberately: an escape every caller can set is one every
# caller sets. A command that rewrites these roots is a build step, not a gate.
# A root that does not exist contributes nothing, so a worktree carrying a subset
# of the tree is not penalised for it.
# ---- AND THE BUILD DIRECTORY MUST BE THIS RUN'S ALONE ----------------------
#
# The twin of the block of the same name in run-gate.sh, which carries the full
# argument, the measurement and the list of what the scan CANNOT see. In short:
# a run can be lied to through the BUILD DIRECTORY exactly as completely as
# through the source tree. +MEASURED 2026-09-07 (P63): `ctest --test-dir build/sh`
# launched while a lane's gate was already running against that same directory
# gave 2100/2101 with ONE red that passes in isolation, and this wrapper printed
# `inputs : held still`. Four concurrent ctest.exe were live.
# D-GATE-RUN-GATE-BLIND-TO-A-SECOND-RUN-IN-THE-SAME-BUILD-DIRECTORY
#
# ! A SCAN, NOT A LOCK: a lock sees only runs that went through this wrapper,
#   and the run that caused the measurement above did not.
# ! THE RULES BELOW ARE THE TWIN'S RULES, SPELLED IN THIS SHELL'S IDIOMS AND
#   NOTHING MORE -- same flag table, same program-keyed reading of `-C` (a
#   CONFIGURATION to ctest, a DIRECTORY to ninja), same quote-aware tokenising,
#   same exit code 4. That is the arrangement `check-line-endings` uses for the
#   same reason: the shells marshal, they do not each invent a scan.
$script:RunGateContentionExit = 4
$script:RunGateBuildDir       = ''
$script:RunGateAbsBuildDir    = ''
$script:RunGateContenders     = @()
$script:RunGateUnreadable     = 0
$script:RunGateTableOk        = $false
$script:RunGateRelativeMatch  = $false
$script:RunGateContentionNote = ''

function Test-RunGateIsWindows {
    if (Test-Path variable:IsWindows) { return [bool]$IsWindows }
    return $true   # Windows PowerShell 5.1 has no $IsWindows and runs nowhere else
}

# ONE SPELLING for a directory. Normalises exactly three things, like the twin:
# separator, trailing slash, and (on Windows only) case.
# ! SPLIT IN TWO, exactly as the twin is, because the two callers want DIFFERENT
#   halves: comparing two processes' directories needs the case fold, NAMING one
#   in the log does not -- `c:/source/dailysoftware/...` in a footer reads as a
#   different tree from the one the reader knows. `Tidy` is the shared half.
function Get-RunGateTidyDir([string]$p) {
    $n = $p -replace '\\', '/'
    while ($n.Length -gt 1 -and $n.EndsWith('/')) { $n = $n.Substring(0, $n.Length - 1) }
    return $n
}
function Get-RunGateNormDir([string]$p) {
    $n = Get-RunGateTidyDir $p
    if (Test-RunGateIsWindows) { return $n.ToLowerInvariant() }
    return $n
}

# A directory token made absolute. A RELATIVE token is resolved against THIS
# shell's working directory, because a process's own working directory is not
# readable from outside on Windows; every refusal below says so when it applies.
function Get-RunGateAbsDir([string]$p) {
    $d = $p -replace '\\', '/'
    $full = $null
    try { $full = (Resolve-Path -LiteralPath $d -ErrorAction Stop).ProviderPath } catch { $full = $null }
    if (-not $full) {
        try { $full = [IO.Path]::GetFullPath([IO.Path]::Combine((Get-Location).Path, $d)) } catch { $full = $d }
    }
    return Get-RunGateTidyDir $full
}
function Resolve-RunGateDir([string]$p) {
    return Get-RunGateNormDir (Get-RunGateAbsDir $p)
}

# Quote-aware split. +MEASURED: a Windows command line reads
# `"C:\Program Files\CMake\bin\cmake.exe" --build build/hg --parallel 6`, so a
# split on whitespace alone tears the program path in half.
function Split-RunGateCommandLine([string]$line) {
    $out = New-Object System.Collections.Generic.List[string]
    $cur = ''; $inq = $false
    foreach ($c in $line.ToCharArray()) {
        if ($c -eq '"') { $inq = -not $inq; continue }
        if ((-not $inq) -and ($c -eq ' ' -or $c -eq "`t")) {
            if ($cur -ne '') { $out.Add($cur); $cur = '' }
            continue
        }
        $cur += $c
    }
    if ($cur -ne '') { $out.Add($cur) }
    return $out.ToArray()
}

# THE FLAG TABLE, program-keyed. `ctest -C Debug` is a CONFIGURATION and
# `ninja -C dir` is a DIRECTORY; a rule that read `-C` for both would turn every
# `ctest -C Debug` into a build directory called `Debug`.
function Get-RunGateDirsInTokens([string]$image, [string[]]$tok) {
    $p = ([IO.Path]::GetFileName($image)).ToLowerInvariant()
    if ($p.EndsWith('.exe')) { $p = $p.Substring(0, $p.Length - 4) }
    $found = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $tok.Count; $i++) {
        $a = [string]$tok[$i]
        $next = if ($i + 1 -lt $tok.Count) { [string]$tok[$i + 1] } else { $null }
        if     ($a -eq '--test-dir'  -and $next) { $found.Add($next) }
        elseif ($a.StartsWith('--test-dir='))    { $found.Add($a.Substring(11)) }
        elseif ($p -eq 'cmake' -and $a -eq '--build' -and $next) { $found.Add($next) }
        elseif ($p -eq 'cmake' -and $a -eq '-B' -and $next)      { $found.Add($next) }
        elseif ($p -eq 'cmake' -and $a.Length -gt 2 -and $a.StartsWith('-B')) { $found.Add($a.Substring(2)) }
        elseif (($p -eq 'ninja' -or $p -eq 'make' -or $p -eq 'gmake') -and $a -eq '-C' -and $next) { $found.Add($next) }
        elseif (($p -eq 'ninja' -or $p -eq 'make' -or $p -eq 'gmake') -and $a.Length -gt 2 -and $a.StartsWith('-C')) { $found.Add($a.Substring(2)) }
    }
    return $found.ToArray()
}

# pid / ppid / image / command line for every live process.
# +MEASURED on this workstation: `Get-CimInstance Win32_Process` returns 507-538
# rows in 341 ms from an already-running PowerShell, of which 249 report an
# EMPTY CommandLine (protected/system processes) while all four live build-tool
# processes reported theirs.
function Get-RunGateProcessTable {
    if (Test-RunGateIsWindows) {
        try {
            return @(Get-CimInstance Win32_Process -ErrorAction Stop | ForEach-Object {
                [PSCustomObject]@{
                    ProcId    = [int]$_.ProcessId
                    ParentId  = [int]$_.ParentProcessId
                    Image     = [string]$_.Name
                    CmdLine   = [string]$_.CommandLine
                } })
        } catch { return @() }
    }
    # The twin's POSIX instrument, so the two agree off Windows as well.
    try {
        return @(& ps -eo 'pid=,ppid=,comm=,args=' 2>$null | ForEach-Object {
            $f = ($_ -replace '^\s+', '') -split '\s+', 4
            if ($f.Count -lt 3) { return }
            [PSCustomObject]@{
                ProcId   = [int]$f[0]
                ParentId = [int]$f[1]
                Image    = [string]$f[2]
                CmdLine  = if ($f.Count -ge 4) { [string]$f[3] } else { '' }
            } })
    } catch { return @() }
}

function Get-RunGateContention {
    $script:RunGateContenders    = @()
    $script:RunGateUnreadable    = 0
    $script:RunGateTableOk       = $false
    $script:RunGateRelativeMatch = $false
    if (-not $script:RunGateBuildDir) { return }
    $table = Get-RunGateProcessTable
    if ($table.Count -eq 0) { return }
    $script:RunGateTableOk = $true

    # OUR OWN ANCESTORS ARE NOT CONTENDERS. A gate legitimately invoked from
    # inside a `ctest` (this repository registers guards that way) would
    # otherwise refuse itself the moment it named the same tree.
    $byId = @{}
    foreach ($r in $table) { if (-not $byId.ContainsKey($r.ProcId)) { $byId[$r.ProcId] = $r } }
    $exclude = @{}
    $walk = $PID; $depth = 0
    while ($walk -and $depth -lt 24) {
        $exclude[$walk] = $true
        if (-not $byId.ContainsKey($walk)) { break }
        $walk = $byId[$walk].ParentId
        $depth++
    }

    $tools = @('ctest', 'ninja', 'cmake', 'make', 'gmake', 'msbuild')
    foreach ($r in $table) {
        $img = $r.Image.ToLowerInvariant()
        if ($img.EndsWith('.exe')) { $img = $img.Substring(0, $img.Length - 4) }
        if ($tools -notcontains $img) { continue }
        if (-not $r.CmdLine) { $script:RunGateUnreadable++; continue }
        if ($exclude.ContainsKey($r.ProcId)) { continue }
        foreach ($raw in (Get-RunGateDirsInTokens $img (Split-RunGateCommandLine $r.CmdLine))) {
            if ((Resolve-RunGateDir $raw) -ne $script:RunGateBuildDir) { continue }
            $slashed = $raw -replace '\\', '/'
            if (-not ($slashed -match '^(/|[A-Za-z]:/)')) { $script:RunGateRelativeMatch = $true }
            $script:RunGateContenders += "      pid $($r.ProcId)  $img  named it as '$raw'"
            $script:RunGateContenders += "        $($r.CmdLine)"
        }
    }
}

function Show-RunGateContentionRefusal([string]$When) {
    Write-Host "run-gate.ps1: FAIL - ANOTHER RUN IS LIVE IN THIS BUILD DIRECTORY, so this one has no verdict."
    Write-Host "  build dir: $($script:RunGateBuildDir)"
    Write-Host "  detected : $When"
    Write-Host "  These live processes name that same directory:"
    foreach ($l in $script:RunGateContenders) { Write-Host $l }
    Write-Host "  [!] This is NOT 'the gate failed' and NOT 'the gate passed'. Two runs sharing one build"
    Write-Host "      directory rewrite each other's binaries and test artefacts mid-run: +MEASURED, a"
    Write-Host "      2100/2101 with ONE red that passed on a re-run in isolation."
    if ($script:RunGateRelativeMatch) {
        Write-Host "  (i) At least one spelling above is RELATIVE, and this guard resolved it against THIS"
        Write-Host "      shell's working directory ($((Get-Location).Path)) - a process's own working"
        Write-Host "      directory is not readable from outside on Windows. If that process is really"
        Write-Host "      standing in a DIFFERENT tree, this is a false refusal; read the command line"
        Write-Host "      printed above before assuming it is not."
    }
    Write-Host "  Wait for the other run to finish, or point this gate at its own build directory."
    Write-Host "  (log: $LogPath)"
}

# ---- AND THE ROOTS ARE THE GATE COMMAND'S TREE, NOT THIS SHELL'S ------------
#
# ★★★ THE THREE ROOT NAMES ARE RELATIVE, AND WHAT THEY ARE RELATIVE **TO** IS THE
# WHOLE QUESTION. They used to be resolved against the PROCESS WORKING DIRECTORY,
# which is right only when the caller happens to be standing in the tree the gate
# command reads -- and this project gates lane worktrees from sibling trees.
#
# +MEASURED 2026-09-08 (P65, lane `rc`), BOTH DIRECTIONS, BOTH TWINS, with two
# synthetic trees A and B: cwd = B, gate command = `ctest --test-dir A/build/x`.
#   . edit an input root in B (a tree the run never reads) -> exit 3 on BOTH
#     twins, naming a file the run could not have seen: a LOUD FALSE REFUSAL
#     that spends a quarter-hour gate. A sibling lane hit this shape in the field.
#   . edit an input root in A (the tree whose build directory the command names,
#     and whose config its tests read) -> exit 0 on BOTH twins, footer
#     `inputs  : held still`. => THE SILENT WRONG ANSWER, and the one sentence
#     this block exists to be unable to say wrongly.
#
# ★★★ WHAT DECIDES NOW, FROM THE MECHANISM RATHER THAN A PREFERENCE: the source
# tree CMake itself records as having configured the build directory this command
# names. `<build>/CMakeCache.txt` carries `CMAKE_HOME_DIRECTORY:INTERNAL=<dir>`,
# and the tests registered in that build tree read `src/dss-config`,
# `tests/corpus` and `examples` from THERE at test time.
#
# +AND THE BUILD SYSTEM SAYS IT IN SO MANY WORDS, which is why this is the
# MECHANISM and not an inference. `CMakeLists.txt` gives 35 registered tests
# `WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"`, and `tests/CMakeLists.txt` bakes
# `DSS_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}"` into every test binary -- what
# `tests/test_support/repo_root.hpp`'s `bakedRepoRoot()` returns and what every
# helper there resolves the three roots against. `CMAKE_SOURCE_DIR` is EXACTLY
# the value CMake writes to the cache as `CMAKE_HOME_DIRECTORY`.
#
# ! WHAT THIS STILL DOES NOT REACH, stated rather than left to be discovered:
# `$DSS_CONFIG_ROOT`. The compiler's own walk composes `<that>/src/dss-config`
# (`src/core/types/config_path_walk.cpp`, `repoShapedConfigRoot`), so a gate run
# with that variable pointing OUTSIDE the tree named here reads a config tree
# this scan never walks. NOT guessed at, deliberately -- the compiler and
# `repo_root.hpp` document precedences that do not obviously agree about whether
# the variable names a tree root or the config directory itself, and a rule
# built on the wrong one would watch a directory that does not exist, which
# contributes nothing and restores the very `held still` this block exists to
# prevent. +MEASURED: the one shipped caller that sets it,
# `scripts/profile-compile/profile-compile.sh`, sets it to the repository it is
# already standing in, so nothing is relocated today.
#
# ! THREE OTHER CANDIDATES WERE MEASURED AND ALL THREE ARE WRONG -- the twin
#   carries the full argument; the short form is: the repository root containing
#   the build tree is refuted by `build/rvff` in this very repository, which sits
#   in the main checkout and names a WORKTREE as its home directory; this
#   wrapper's own location is refuted by its fixture, which drives the
#   repository's copy over a synthetic sandbox; and the cwd is refuted by the
#   measurement above, in both directions.
#
# (i) THE CWD REMAINS THE FALLBACK and is now STATED rather than assumed: a gate
#   command need not name a build directory (`remote-leg` hands this wrapper a
#   `bash`), and a named directory need not be a CMake build tree. In both cases
#   there is no evidence about which tree the command reads, so the wrapper says
#   which rule decided, on every run, in the log.
# (i) NOT AN ESCAPE HATCH: nothing here is settable by a caller.
$script:RunGateInputRootNames = @('src/dss-config', 'tests/corpus', 'examples')
$script:RunGateSourceTree     = ''
$script:RunGateSourceTreeWhy  = ''
$script:RunGateSourceTreeMiss = ''

# CMake's own record of which tree configured this build tree, or $null -- and
# when $null, WHY, in $script:RunGateSourceTreeMiss.
# ★★ THE FOUR MISSES ARE NOT ONE MISS, and collapsing them into "not a CMake
# build tree" is the shape of message this pair keeps refusing: a sentence that
# outruns its evidence. +MEASURED while building this -- a CMakeCache whose
# recorded home directory THIS SHELL CANNOT SEE is a real, reachable state (an
# MSYS-spelled `/c/...` is invisible to PowerShell and a `C:/...` is invisible to
# a WSL bash), and it is emphatically NOT "there is no cache".
function Get-RunGateSourceTreeOfBuildDir([string]$buildDir) {
    $script:RunGateSourceTreeMiss = ''
    if (-not $buildDir) {
        $script:RunGateSourceTreeMiss = 'this command names no build directory, so there is nothing to ask'
        return $null
    }
    $cache = "$buildDir/CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) {
        $script:RunGateSourceTreeMiss = 'the build directory it names has no CMakeCache.txt, so nothing on disk records which tree configured it'
        return $null
    }
    $hit = Select-String -LiteralPath $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=' -List -ErrorAction SilentlyContinue
    $homeDir = ''
    if ($hit) { $homeDir = $hit.Line.Substring('CMAKE_HOME_DIRECTORY:INTERNAL='.Length).Trim() }
    if (-not $homeDir) {
        $script:RunGateSourceTreeMiss = 'its CMakeCache.txt carries no CMAKE_HOME_DIRECTORY entry'
        return $null
    }
    if (-not (Test-Path -LiteralPath $homeDir -PathType Container)) {
        $script:RunGateSourceTreeMiss = "its CMakeCache.txt names '$homeDir' as CMAKE_HOME_DIRECTORY and THIS SHELL ($(Get-RunGateShellIdentity)) CANNOT SEE THAT DIRECTORY - a DOS-drive path is invisible to a WSL bash and an MSYS '/c/...' path is invisible to PowerShell, so check which shell you handed this gate to"
        return $null
    }
    return (Get-RunGateTidyDir $homeDir)
}

function Set-RunGateInputRoots {
    $fromBuild = Get-RunGateSourceTreeOfBuildDir $script:RunGateAbsBuildDir
    if ($fromBuild) {
        $script:RunGateSourceTree    = $fromBuild
        $script:RunGateSourceTreeWhy = "CMAKE_HOME_DIRECTORY recorded in $($script:RunGateAbsBuildDir)/CMakeCache.txt - the tree this command's build directory was configured from"
        return
    }
    $script:RunGateSourceTree    = Get-RunGateAbsDir '.'
    $script:RunGateSourceTreeWhy = "this shell's working directory - $($script:RunGateSourceTreeMiss)"
}

# The three roots as ABSOLUTE paths. (i) Used by BOTH the scan and the footer on
# purpose: a footer naming roots the scan did not walk is the class of lie this
# whole block is about.
function Get-RunGateAbsInputRoots {
    return @($script:RunGateInputRootNames | ForEach-Object { "$($script:RunGateSourceTree)/$_" })
}

# ---- PRE-RUN: refuse a contended build directory BEFORE anything starts -----
# ! Placed AHEAD of the input marker deliberately, so a refusal here leaves no
#   marker file behind for the next run to trip over.
$__rawBuildDir = @(Get-RunGateDirsInTokens $Command $CommandArgs) | Select-Object -First 1
if ($__rawBuildDir) {
    # TWO SPELLINGS OF ONE DIRECTORY, each with exactly one caller, like the twin:
    # the case-folded one is only ever COMPARED against another process's
    # spelling; the plain one is what gets NAMED in a message and what
    # CMakeCache.txt is read beside.
    $script:RunGateAbsBuildDir = Get-RunGateAbsDir $__rawBuildDir
    $script:RunGateBuildDir    = Get-RunGateNormDir $script:RunGateAbsBuildDir
}
Set-RunGateInputRoots
Get-RunGateContention
if ($script:RunGateContenders.Count -gt 0) {
    Add-Content -LiteralPath $LogPath -Value @"
--- run-gate.ps1 ---
command : $Command $($CommandArgs -join ' ')
builddir: $($script:RunGateBuildDir)
contended: YES, BEFORE THE RUN - nothing was executed
"@
    foreach ($l in $script:RunGateContenders) { Add-Content -LiteralPath $LogPath -Value $l }
    Show-RunGateContentionRefusal "BEFORE the run started, so nothing was executed"
    Restore-CplDefault
    exit $script:RunGateContentionExit
}

$script:RunGateMarker = "$LogPath.inputs-marker"
try {
    New-Item -ItemType File -Path $script:RunGateMarker -Force -ErrorAction Stop | Out-Null
} catch {
    Write-Host "run-gate.ps1: FAIL - cannot create the input marker '$($script:RunGateMarker)', so the run"
    Write-Host "  could not be proved to have measured a still tree. Nothing was run."
    Write-Host "  This refusal is about the MARKER PATH, which sits beside the log path you gave."
    Write-Host "  shell   : $(Get-RunGateShellIdentity)"
    Restore-CplDefault
    exit 2
}
$script:RunGateMarkerTime = (Get-Item -LiteralPath $script:RunGateMarker -Force).LastWriteTimeUtc
$script:RunGateInputsBefore = "$LogPath.inputs-before"
$script:RunGateInputsAfter  = "$LogPath.inputs-after"
# ⚠ THE WRAPPER MUST NOT MEASURE ITS OWN BOOKKEEPING — the twin's note applies
# verbatim: all three files sit beside the CALLER'S log path, nothing stops that
# path being inside a watched root, and `.inputs-before` exists only in the AFTER
# snapshot, so an unexcluded run would refuse ITSELF.
$script:RunGateBookkeepingPrefix = (Split-Path -Leaf $LogPath) + '.inputs-'

# ⚠⚠⚠ WHY THIS IS A BEFORE/AFTER FINGERPRINT AND NOT `LastWriteTimeUtc -gt`.
# ✔MEASURED 2026-09-09 (P66) on WSL x86_64: CLOCK_REALTIME there steps FORWARD by
# +24.69 s for ~200 ms out of every ~5 s (4.8% duty cycle) and the excursion
# REACHES INODE MTIMES — 12 of 60 marker/probe pairs had the probe, created one
# second AFTER the marker, carrying an mtime 23.70 s EARLIER. Ordering two clock
# readings taken seconds apart is therefore not sound on a carriage this project
# gates on, and BOTH twins were shown to fail under one identical mutation (a
# marker stamped +25 s): `find -newer` returned nothing, and this twin's
# `LastWriteTimeUtc -gt $markerTime` returned nothing, from the same two inodes.
# ⇒ this is a SHARED defect in one algorithm, not a divergence between the twins,
# and it is fixed on both sides in one commit. The full measurement, the duty
# cycle, and why EQUALITY of a fingerprint with itself is immune to it are in the
# `.sh` twin's "THE RUN'S INPUTS MUST HOLD STILL" block; not repeated here so the
# two cannot drift into describing it differently.
# ⓘ SHA256 rather than MD5: `Get-FileHash -Algorithm MD5` throws under a FIPS
#   policy, and only self-consistency between two snapshots in one run is
#   load-bearing, so the stronger algorithm costs nothing worth having.
#   ✔MEASURED 317 ms over the 1749 files of `examples/`.
function Get-RunGateInputFingerprint {
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($root in (Get-RunGateAbsInputRoots)) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        foreach ($f in (Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue)) {
            if ($f.Name.StartsWith($script:RunGateBookkeepingPrefix)) { continue }
            $h = Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256 -ErrorAction SilentlyContinue
            if ($null -ne $h) { $lines.Add("C $($h.Hash) $($f.Length) $($f.FullName)") }
            # The stamp-order half, differenced against its own pre-run reading:
            # a file that ALREADY carried a future stamp when the run started is
            # in both snapshots and cancels, instead of refusing a run it never
            # touched. See the twin for the measurement that made that real.
            if ($f.LastWriteTimeUtc -gt $script:RunGateMarkerTime) { $lines.Add("N $($f.FullName)") }
        }
    }
    return @($lines | Sort-Object)
}

# ★ PROBED BY EXECUTION WITH A KNOWN ANSWER, like the twin's `cksum` probe: the
# empty marker's SHA256 is a constant, so a hashing path that silently produced
# nothing would refuse this run rather than emptying every snapshot and passing
# everything while appearing to run.
function Test-RunGateHashWorks {
    $h = Get-FileHash -LiteralPath $script:RunGateMarker -Algorithm SHA256 -ErrorAction SilentlyContinue
    return ($null -ne $h -and $h.Hash -eq 'E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855')
}

# ⚠⚠ `-Force` IS LOAD-BEARING, AND WITHOUT IT THIS TWIN IS BLIND OFF WINDOWS.
# `Get-ChildItem -Recurse` omits HIDDEN entries, and on Linux and macOS "hidden"
# means A LEADING DOT -- so every dot-file under the three input roots was
# invisible here while `find -type f -newer` in the .sh twin saw it. That is the
# worst possible direction for this particular check: the wrapper printed
# `inputs  : held still` over a tree that HAD moved, which is the one sentence it
# exists to be unable to say wrongly.
# ✔MEASURED 2026-09-08 on WSL x86_64 (pwsh 7.5.4): `Get-ChildItem -Recurse -File`
# under a directory holding `.dotfile` and `plain.txt` returned only `plain.txt`;
# with `-Force` it returned both. The fixture's own probe file is a dot-file, so
# arm `5-ps1-moved` of `scripts/run-gate/test-run-gate.sh` returned 0 instead of 3
# and arm `6-parity` reported `.sh=3 vs .ps1=0`.
# ⚠ IT WAS INVISIBLE ON WINDOWS FOR TWO COMPOUNDING REASONS: NTFS does not treat a
# leading dot as hidden, so the same file was returned there without `-Force`; and
# the fixture drove the .ps1 arms with a literal `powershell`, so they had never
# run on a host where the difference exists. `-Force` also picks up genuinely
# hidden-attributed files on Windows, which is what the .sh twin already did.
# (i) A root that does not exist contributes nothing, so a lane worktree carrying
#   a subset of the tree, or a synthetic self-test root, is not penalised for it.
#   The roots are ABSOLUTE (see "AND THE ROOTS ARE THE GATE COMMAND'S TREE"
#   above), so this walks the tree the gate command reads and not this shell's.
# ⚠⚠ "I COULD NOT MEASURE" MUST NOT BE SPELLED `held still`, so taking the after
# snapshot is the CALLER's job and its failure is a REFUSAL, not an empty list.
# An empty return from here is indistinguishable from "nothing moved", which is
# the fails-toward-clean answer this whole block exists to be unable to give —
# and the scan this replaced had exactly that shape. The twin says the same in
# its `run_gate_diff_inputs` note; both decide it in the outer flow instead.
function Get-RunGateMovedInputs {
    $after  = @(Get-Content -LiteralPath $script:RunGateInputsAfter -ErrorAction SilentlyContinue |
                Where-Object { $_ -ne '' })
    $before = @(Get-Content -LiteralPath $script:RunGateInputsBefore -ErrorAction SilentlyContinue |
                Where-Object { $_ -ne '' })
    $moved = @()
    foreach ($line in (Compare-Object -ReferenceObject $before -DifferenceObject $after)) {
        $l = $line.InputObject
        if     ($l -match '^C \S+ \d+ (.+)$') { $moved += $Matches[1] }
        elseif ($l -match '^N (.+)$')         { $moved += $Matches[1] }
    }
    # Same cap as the .sh twin: the refusal names the class, it is not a manifest.
    return @($moved | Sort-Object -Unique | Select-Object -First 20)
}

# ⚠ THREE REFUSALS, NOT ONE, like the twin: a wrapper that cannot fingerprint the
# tree cannot vouch for its stillness, and the honest answer is to run NOTHING.
if (-not (Test-RunGateHashWorks)) {
    Write-Host "run-gate.ps1: FAIL - Get-FileHash did not return the known SHA256 of an empty file on"
    Write-Host "  this host, so the input fingerprint this wrapper compares before and after the run"
    Write-Host "  cannot be taken. Nothing was run."
    Write-Host "  This refusal is deliberate rather than a skip: an empty fingerprint would make every"
    Write-Host "    diff empty, and this check would pass everything while appearing to run."
    Write-Host "  shell   : $(Get-RunGateShellIdentity)"
    Remove-Item -LiteralPath $script:RunGateMarker -Force -ErrorAction SilentlyContinue
    Restore-CplDefault
    exit 2
}
try {
    Set-Content -LiteralPath $script:RunGateInputsBefore -ErrorAction Stop `
        -Value ((Get-RunGateInputFingerprint) -join [Environment]::NewLine)
} catch {
    Write-Host "run-gate.ps1: FAIL - cannot record the pre-run input fingerprint at"
    Write-Host "  '$($script:RunGateInputsBefore)', so the run could not be proved to have measured a"
    Write-Host "  still tree. Nothing was run."
    Write-Host "  This refusal is about that PATH, which sits beside the log path you gave."
    Write-Host "  shell   : $(Get-RunGateShellIdentity)"
    Remove-Item -LiteralPath $script:RunGateMarker -Force -ErrorAction SilentlyContinue
    Restore-CplDefault
    exit 2
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

$snapshotOk = $true
if (-not (Test-Path -LiteralPath $script:RunGateInputsBefore)) {
    $snapshotOk = $false
} else {
    try {
        Set-Content -LiteralPath $script:RunGateInputsAfter -ErrorAction Stop `
            -Value ((Get-RunGateInputFingerprint) -join [Environment]::NewLine)
    } catch {
        $snapshotOk = $false
    }
    if (-not (Test-Path -LiteralPath $script:RunGateInputsAfter)) { $snapshotOk = $false }
}
$movedInputs = @()
if ($snapshotOk) { $movedInputs = Get-RunGateMovedInputs }
Remove-Item -LiteralPath $script:RunGateMarker -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $script:RunGateInputsBefore -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $script:RunGateInputsAfter -Force -ErrorAction SilentlyContinue

# ---- POST-RUN: a sibling can start MID-RUN, so the same question is asked again
Get-RunGateContention

Add-Content -LiteralPath $LogPath -Value @"
--- run-gate.ps1 ---
command : $Command $($CommandArgs -join ' ')
rc      : $rc
"@
if (-not $snapshotOk) {
    Add-Content -LiteralPath $LogPath -Value "inputs  : NOT MEASURED - the post-run fingerprint could not be taken, so this verdict is not evidence"
} elseif ($movedInputs.Count -gt 0) {
    Add-Content -LiteralPath $LogPath -Value "inputs  : MOVED DURING THE RUN - this verdict is not evidence"
    foreach ($m in $movedInputs) { Add-Content -LiteralPath $LogPath -Value "          $m" }
} else {
    Add-Content -LiteralPath $LogPath -Value "inputs  : held still"
}
# ★★ THE FOOTER NAMES THE TREE IT WATCHED, ABSOLUTELY, ON EVERY RUN -- green,
# refused, or failed. `held still` is a claim about a DIRECTORY, and a reader who
# has to reconstruct the caller's working directory to learn which directory
# cannot check the claim at all.
Add-Content -LiteralPath $LogPath -Value "srctree : $($script:RunGateSourceTree)"
Add-Content -LiteralPath $LogPath -Value "          decided by: $($script:RunGateSourceTreeWhy)"
foreach ($r in (Get-RunGateAbsInputRoots)) { Add-Content -LiteralPath $LogPath -Value "watched : $r" }
if (-not $script:RunGateBuildDir) {
    Add-Content -LiteralPath $LogPath -Value "builddir: none named by this command - the contention check had no subject"
} else {
    Add-Content -LiteralPath $LogPath -Value "builddir: $($script:RunGateBuildDir)"
    if ($script:RunGateContenders.Count -gt 0) {
        Add-Content -LiteralPath $LogPath -Value "contended: YES, ANOTHER RUN WAS LIVE IN IT - this verdict is not evidence"
        foreach ($l in $script:RunGateContenders) { Add-Content -LiteralPath $LogPath -Value $l }
    } elseif ($script:RunGateTableOk) {
        Add-Content -LiteralPath $LogPath -Value "contended: no (this run was alone in it; $($script:RunGateUnreadable) candidate process(es) had no readable command line and could not be judged)"
    } else {
        Add-Content -LiteralPath $LogPath -Value "contended: UNKNOWN - no process table could be read on this host, so nothing was ruled out"
    }
}

# Checked BEFORE rc, and before the witness: a run whose inputs moved has no
# verdict to report, and calling it a pass or a failure is the misattribution
# this block exists to prevent. Exit 3 matches the .sh twin.
# ⚠ "I could not measure" is checked FIRST OF ALL, for the reason the twin gives:
# it is the one answer that must never be spelled `held still`.
if (-not $snapshotOk) {
    Write-Host "run-gate.ps1: FAIL - THE POST-RUN INPUT FINGERPRINT COULD NOT BE TAKEN, so this run"
    Write-Host "  cannot be shown to have measured a still tree."
    Write-Host "  (command exited $rc; that number is NOT being reported as a verdict)."
    Write-Host "  The PRE-run fingerprint was taken successfully or this run would not have started,"
    Write-Host "    so something removed or blocked '$($script:RunGateInputsBefore)' or"
    Write-Host "    '$($script:RunGateInputsAfter)' while the command was running."
    Write-Host "  Refusing is deliberate. Reading an unmeasurable tree as 'held still' is exactly"
    Write-Host "    the fails-toward-clean answer this check exists to be unable to give."
    Write-Host "  (log: $LogPath)"
    exit 3
}

if ($movedInputs.Count -gt 0) {
    Write-Host "run-gate.ps1: FAIL - the tree CHANGED UNDER THE RUN, so its result is not evidence"
    Write-Host "  (command exited $rc; that number describes a tree that never existed as a whole)."
    Write-Host "  These read-at-test-time files were modified after the run started:"
    foreach ($m in $movedInputs) { Write-Host "      $m" }
    Write-Host "  source tree watched: $($script:RunGateSourceTree)"
    Write-Host "    decided by: $($script:RunGateSourceTreeWhy)"
    Write-Host "  This is NOT 'the gate failed'. Any failure it reported may belong to the edit"
    Write-Host "    rather than to the code under test, and any PASS is equally unproven."
    Write-Host "  Let the tree settle and run it again. If you are the one who edited it: this"
    Write-Host "    project's runners read src/dss-config, tests/corpus and examples from the"
    Write-Host "    SOURCE TREE at test time, so an edit there is not inert while a suite runs."
    Write-Host "  (log: $LogPath)"
    exit 3
}

# Checked next, and still BEFORE rc and the witness. The ORDER between this and
# the inputs check is fixed to match the twin so the two cannot disagree about
# which sentence a doubly-spoiled run prints: inputs first, because that refusal
# is the older one and its exit code (3) is already cited in shipped fixtures.
if ($script:RunGateContenders.Count -gt 0) {
    Show-RunGateContentionRefusal "AFTER the run finished (it exited $rc; that number describes a build directory two runs were writing)"
    exit $script:RunGateContentionExit
}

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
