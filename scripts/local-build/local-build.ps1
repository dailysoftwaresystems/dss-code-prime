# PURPOSE: build dsscp incrementally on this host, and optionally run ctest.
# Local incremental build + test harness for dsscp.
#
# Usage:
#   scripts\local-build\local-build.ps1                     # build build\dbg
#   scripts\local-build\local-build.ps1 -Test               # build then run ctest
#   scripts\local-build\local-build.ps1 -Configure          # cmake configure + build
#   scripts\local-build\local-build.ps1 -Clean              # wipe THIS TREE + reconfigure + build
#   scripts\local-build\local-build.ps1 -Tree rel           # operate on build\rel instead
#   scripts\local-build\local-build.ps1 -Tree lane1 -BuildType Debug
#
# ⚠ A LANE TREE TAKES -BuildType EXACTLY ONCE, ON ITS FIRST CONFIGURE, AND
# REFUSES IT EVER AFTER. Only `dbg` -> Debug and `rel` -> Release are names this
# repository has an established meaning for; any other tree name must be TOLD its
# type the first time (rc 3 otherwise, naming the flag), and passing the flag to a
# tree whose cache already declares one is ALSO rc 3 -- so a caller cannot flip a
# configured tree underneath the artifacts already in it. Both refusals are
# deliberate; what was missing was saying so here.
#   first  run:  -Tree <lane> -Configure -BuildType Debug
#   after that:  -Tree <lane>            (and -Clean to change the type)
# ✔MEASURED by EXECUTION 2026-08-29, after a cycle brief stated this invocation
# from the usage text WITHOUT running it and cost a lane two failed rounds --
# D-CYCLE-BRIEF-STATED-AN-INVOCATION-ITS-AUTHOR-HAD-NEVER-RUN, one level out: the
# usage text is itself an interface claim, and it was making one it could not keep.
#
# ★★★ `build\` IS A CONTAINER, NOT A BUILD TREE (the one-root layout, operator
# 2026-08-17: one root `build/`, one SUBDIRECTORY per build). This script used to
# treat `build\` itself as the tree, and `-Clean` was `Remove-Item -Recurse -Force
# build`, which under that layout DELETES EVERY SIBLING TREE AT ONCE: `build\dbg`,
# `build\rel`, `build\perf` and the gate logs beside them. It would also have
# configured a FOURTH, unnamed tree at `build\` alongside the real ones, and then
# run ctest in the wrong directory.
# ⚠ Losing `build\rel` is not merely slow to recover: the Windows driver picks the
# NEWEST RELEASE build tree, so a deleted-then-stale `build\rel` silently answers
# discovery afterwards. Fixed 2026-08-20. An anchor name must never be wrapped --
# a split name is not greppable, so no tool can find it and the balance guard
# cannot see it. It goes on one line of its own:
#   D-SCRIPT-LOCAL-BUILD-TREATS-THE-BUILD-ROOT-AS-A-BUILD-TREE
#
# Safe to invoke without approval prompts in agentic workflows — read-only on src/,
# writes only inside build\<tree>. Requires `cmake` on PATH at version >= 4.0
# (project's CMakeLists.txt floor).

[CmdletBinding()]
param(
    [switch]$Test,
    [switch]$Configure,
    [switch]$Clean,
    [switch]$SelfTest,
    [string]$Tree = 'dbg',
    [string]$BuildType = ''
)

$ErrorActionPreference = 'Stop'

# -- TOOLCHAIN READ FAILURE - a distinct outcome from a build failure ---------
# The twin of local_build_toolchain_read_failure in local-build.sh: same
# condition, same exit code (9), same refusal to retry. Kept in step with it
# BY REVIEW, not by a detector -- equivalence of two arbitrary programs is not
# a property a script can decide, which is why the pairing rule is a review
# obligation in the first place.
#
# MEASURED 2026-08-20 (cycle P23,
# D-BUILD-CONCURRENT-LANES-TRIP-A-TOOLCHAIN-HEADER-READ-FAILURE):
# under several concurrent lane builds g++ failed to READ
# a standard-library header and printed a bare path plus an errno string,
# followed by ~6 CASCADED diagnostics that look exactly like source defects.
#
# * The discriminator is SHAPE: a compiler DIAGNOSTIC carries `file:line:col:`
#   and a severity; an I/O failure carries a bare path, a colon and an errno
#   string -- inside the TOOLCHAIN's own include tree, which this repo never
#   edits. Path alone is NOT the signal; a genuine diagnostic about a toolchain
#   header must stay classified as a diagnostic.
# ! Deliberately does NOT retry. A retry that hides the event stops it being
#   root-caused. The build still FAILS; it stops lying about whose fault it is.
$script:LocalBuildIoFailureRe =
    '(/(usr|opt)/[^ :]*|[A-Za-z]:[\\/][^ :]*)(include|lib[\\/]gcc)[^ :]*:\s+(Invalid argument|Input/output error|Permission denied|Resource temporarily unavailable|Bad file descriptor)\s*$|fatal error:\s+error\s+(writing\s+to|closing)\s+.+:\s+(Invalid argument|Input/output error|Permission denied|Resource temporarily unavailable|Bad file descriptor|No space left on device)\s*$'

function Test-LocalBuildToolchainIoFailure([string]$LogPath) {
    if (-not (Test-Path -LiteralPath $LogPath)) { return $false }
    foreach ($line in Get-Content -LiteralPath $LogPath) {
        if ($line -match $script:LocalBuildIoFailureRe) { return $true }
    }
    return $false
}

function Write-LocalBuildIoFailure([string]$LogPath) {
    Write-Host "local-build.ps1: FAIL - TOOLCHAIN I/O FAILURE (READ or WRITE), not a source defect."
    Write-Host "  The compiler could not READ a file inside its OWN include tree, or could"
    Write-Host "  not WRITE its own temporary. Every diagnostic after that line is a CASCADE"
    Write-Host "  and says nothing about this repository's source."
    Write-Host "  ! The WRITE shape names a TEMP path, not an include path - it is the half"
    Write-Host "    this detector was blind to until 2026-08-21."
    Write-Host "  the line that classifies it:"
    Get-Content -LiteralPath $LogPath |
        Where-Object { $_ -match $script:LocalBuildIoFailureRe } |
        Select-Object -First 3 |
        ForEach-Object { Write-Host "    $_" }
    Write-Host "  [!] DO NOT act on the errors above it and DO NOT 'fix' the standard library."
    Write-Host "  [!] If this happened during a RED-ON-DISABLE arm, that arm measured NOTHING -"
    Write-Host "      re-run it; a restore that fails this way is not evidence about your change."
    Write-Host "  Root cause is UNKNOWN and is not guessed at here (see the anchor). Re-run the"
    Write-Host "  build; if it recurs at the same file, say so in the row rather than retrying."
    Write-Host "  full log: $LogPath"
}

# ★★★ A NON-ZERO BUILD THAT PRINTED NO DIAGNOSTIC AT ALL IS ITS OWN CLASS, AND IT
# IS NOT A SOURCE DEFECT. Measured 2026-09-05 (P62 lane `dy`): editing a source
# file while ninja was already running in the same tree cost TWO WHOLE BUILDS, each
# ending rc=1 with a log that stopped mid-compile and named no error. At the exit
# code that is indistinguishable from a real compile failure. The I/O classifier
# above cannot see it: that one keys on a diagnostic of a particular SHAPE, and
# this class prints no diagnostic to key on.
# ★★ DEFINED BY THE COMPLEMENT, never by enumerating the shapes a silent failure
# takes -- there is nothing to enumerate. "A diagnostic was printed" is the
# positive vocabulary; its absence is the class, so a shape nobody has thought of
# yet counts as SILENT, which fails toward reading the log rather than trusting it.
# ! THE COLON (or the MSVC code) IS LOAD-BEARING. This repository ships
# `src/core/error/` and a `test_artifact_withheld_after_error` target whose names
# appear in an ordinary ninja PROGRESS line on every build; matching a bare `error`
# would blind the guard in exactly the tree it protects.
$script:LocalBuildDiagnosticRe =
    '(^|[^a-zA-Z0-9_])[Ee]rror:|fatal error|[Ee]rror [A-Z]+[0-9]{4}|undefined reference'

function Test-LocalBuildNoDiagnostic([string]$LogPath) {
    if (-not (Test-Path -LiteralPath $LogPath)) { return $true }
    foreach ($line in Get-Content -LiteralPath $LogPath) {
        if ($line -match $script:LocalBuildDiagnosticRe) { return $false }
    }
    return $true
}

function Get-LocalBuildSourcesTouchedSince([string]$MarkerPath) {
    if (-not (Test-Path -LiteralPath $MarkerPath)) { return @() }
    $since = (Get-Item -LiteralPath $MarkerPath).LastWriteTimeUtc
    @('src', 'tests') |
        Where-Object { Test-Path -LiteralPath $_ } |
        ForEach-Object { Get-ChildItem -LiteralPath $_ -Recurse -File -ErrorAction SilentlyContinue } |
        Where-Object { $_.LastWriteTimeUtc -gt $since } |
        Select-Object -First 20 |
        ForEach-Object { $_.FullName }
}

# ! THREE STATES, NOT TWO, AND THE THIRD IS WHY THIS HELPER EXISTS. "no file is
# newer than the marker" and "there is no marker, so nothing was compared" are
# different facts, and collapsing them makes the report assert a RULING-OUT it
# never performed -- the instrument answering an adjacent question, failing toward
# clean. That defect was live in the .sh twin until its own message arm printed the
# text; it is written correctly here from the start because of that.
function Get-LocalBuildTouchedVerdict([string]$MarkerPath) {
    if (-not (Test-Path -LiteralPath $MarkerPath)) { return 'unchecked' }
    if (@(Get-LocalBuildSourcesTouchedSince $MarkerPath).Count -gt 0) { return 'moved' }
    return 'clean'
}

function Write-LocalBuildNoDiagnostic([string]$LogPath, [string]$MarkerPath) {
    Write-Host "local-build.ps1: FAIL - THE BUILD EXITED NON-ZERO AND PRINTED NO DIAGNOSTIC."
    Write-Host "  No line in the log matches any shape our toolchains emit for an error."
    Write-Host "  [!] THIS IS NOT EVIDENCE OF A SOURCE DEFECT. Do NOT start 'fixing' the last"
    Write-Host "      file the log happened to name - that file is where the log STOPPED, which"
    Write-Host "      is not where anything went wrong."
    $failed = @(Get-Content -LiteralPath $LogPath -ErrorAction SilentlyContinue |
                Where-Object { $_ -like 'FAILED: *' } | Select-Object -First 3)
    if ($failed.Count -gt 0) {
        Write-Host "  ninja named a failed target, so a command died without saying why:"
        $failed | ForEach-Object { Write-Host "    $_" }
    } else {
        Write-Host "  ninja named NO failed target - the log simply ends. The build process"
        Write-Host "    itself went away (killed, or its input changed underneath it)."
    }
    switch (Get-LocalBuildTouchedVerdict $MarkerPath) {
        'moved' {
            Write-Host "  * MEASURED: these source files CHANGED WHILE THIS BUILD WAS RUNNING -"
            Write-Host "    this is the known cause, and re-running the build is the whole remedy:"
            Get-LocalBuildSourcesTouchedSince $MarkerPath | ForEach-Object { Write-Host "    $_" }
        }
        'clean' {
            Write-Host "  * MEASURED: no file under src/ or tests/ changed while the build ran, so"
            Write-Host "    the edit-under-ninja cause is RULED OUT here. Re-run once; if it recurs"
            Write-Host "    at the same point, say so in the row rather than retrying a third time."
        }
        default {
            Write-Host "  [!] NOT MEASURED: no build-start marker, so nothing was compared and the"
            Write-Host "      edit-under-ninja cause is neither shown nor ruled out. This is what an"
            Write-Host "      absent marker means - it is NOT a clean bill of health."
        }
    }
    Write-Host "  [!] If this happened during a RED-ON-DISABLE arm, that arm measured NOTHING -"
    Write-Host "      a build that never completed is not evidence about your change."
    Write-Host "  full log: $LogPath"
}

if ($SelfTest) {
    # The arm is EXERCISED, not read - the same four arms the .sh twin runs.
    $stDir = Join-Path ([System.IO.Path]::GetTempPath()) ("lb-selftest-" + [System.Guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $stDir
    $stFail = 0
    try {
        # The fixture paths are ASSEMBLED, not written literally - same reason as
        # the .sh twin: a literal `path:line` here is indistinguishable from a
        # CITATION to scripts/check-plan-citations, and these are compiler OUTPUT
        # SAMPLES, not claims about any file in this repository.
        $at = ':'
        $read = Join-Path $stDir 'read.log'
        Set-Content -LiteralPath $read -Value @(
            'C:/Strawberry/c/include/c++/13.2.0/bits/locale_facets.tcc: Invalid argument',
            "In file included from x.cpp${at}1:",
            "error: '__use_cache' is not a class template")
        $real = Join-Path $stDir 'real.log'
        Set-Content -LiteralPath $real -Value @(
            "src/link/linker.cpp${at}120:5: error: no member named q",
            'ninja: build stopped: subcommand failed.')
        $diag = Join-Path $stDir 'diag-in-toolchain.log'
        Set-Content -LiteralPath $diag -Value @(
            "/usr/include/c++/13/bits/basic_string.h${at}1:1: error: expected unqualified-id")

        if (Test-LocalBuildToolchainIoFailure $read) {
            Write-Host 'self-test arm 1 READ-FAILURE            classified as expected'
        } else {
            Write-Host 'self-test arm 1 READ-FAILURE            NOT classified - the guard is blind'; $stFail = 1
        }
        if (Test-LocalBuildToolchainIoFailure $real) {
            Write-Host 'self-test arm 2 REAL-COMPILE-ERROR      misclassified - it would HIDE a real defect'; $stFail = 1
        } else {
            Write-Host 'self-test arm 2 REAL-COMPILE-ERROR      left alone as expected'
        }
        if (Test-LocalBuildToolchainIoFailure $diag) {
            Write-Host 'self-test arm 3 DIAGNOSTIC-IN-TOOLCHAIN misclassified - path alone is not the signal'; $stFail = 1
        } else {
            Write-Host 'self-test arm 3 DIAGNOSTIC-IN-TOOLCHAIN left alone as expected'
        }
        # Arms 5 and 6 are the WRITE half - the shape every pattern here was
        # blind to until 2026-08-21, because they all required an INCLUDE path.
        $write = Join-Path $stDir 'write.log'
        Set-Content -LiteralPath $write -Value @(
            "cc1plus: fatal error: error writing to C:\Users\x\AppData\Local\Temp\ccigZdWt.s: Invalid argument",
            'compilation terminated.')
        $writeReal = Join-Path $stDir 'write-real.log'
        Set-Content -LiteralPath $writeReal -Value @(
            "src/hir/hir_verifier.cpp${at}42:7: error: no member named 'writing'",
            'ninja: build stopped: subcommand failed.')
        if (Test-LocalBuildToolchainIoFailure $write) {
            Write-Host 'self-test arm 5 WRITE-FAILURE           classified as expected'
        } else {
            Write-Host 'self-test arm 5 WRITE-FAILURE           NOT classified - blind to the write half'; $stFail = 1
        }
        if (Test-LocalBuildToolchainIoFailure $writeReal) {
            Write-Host 'self-test arm 6 REAL-ERROR-SAYS-WRITING misclassified - it would HIDE a real defect'; $stFail = 1
        } else {
            Write-Host 'self-test arm 6 REAL-ERROR-SAYS-WRITING left alone as expected'
        }
        # 6>&1 because Write-Host emits on the INFORMATION stream, not the output
        # stream: a bare `| Out-String` captures NOTHING and the text goes to the
        # console instead. The .sh twin captures with 2>&1 for the same reason, and
        # this arm caught the asymmetry on its first run.
        $msg = (Write-LocalBuildIoFailure $read 6>&1 | Out-String)
        if ($msg -match 'TOOLCHAIN I/O FAILURE' -and $msg -match 'locale_facets\.tcc' -and $msg -match 'RED-ON-DISABLE') {
            Write-Host 'self-test arm 4 MESSAGE                 names the shape, the file and the red-on-disable hazard'
        } else {
            Write-Host "self-test arm 4 MESSAGE                 incomplete: $msg"; $stFail = 1
        }
        # Arms 7..14 are the SILENT half - the same arms, in the same order, as the
        # .sh twin. Twin parity here is about the ARM-BY-ARM VERDICTS, which is how
        # this pair has been proven twice before.
        $silent = Join-Path $stDir 'silent.log'
        Set-Content -LiteralPath $silent -Value @(
            '[412/1180] Building CXX object src/hir/CMakeFiles/dss_hir.dir/hir_builder.cpp.obj',
            '[413/1180] Building CXX object src/mir/CMakeFiles/dss_mir.dir/hir_to_mir.cpp.obj')
        if (Test-LocalBuildNoDiagnostic $silent) {
            Write-Host 'self-test arm 7 SILENT-STOP             classified as expected'
        } else {
            Write-Host 'self-test arm 7 SILENT-STOP             NOT classified - blind to the silent half'; $stFail = 1
        }
        # * THE ARM THIS CLASSIFIER EXISTS TO SURVIVE. Both paths ship in this
        # repository and appear in an ordinary progress line on EVERY build.
        $silentErr = Join-Path $stDir 'silent-errorpath.log'
        Set-Content -LiteralPath $silentErr -Value @(
            '[7/9] Building CXX object src/core/CMakeFiles/dss_core.dir/error/reporter.cpp.obj',
            '[8/9] Building CXX object tests/link/CMakeFiles/x.dir/test_artifact_withheld_after_error.cpp.obj')
        if (Test-LocalBuildNoDiagnostic $silentErr) {
            Write-Host 'self-test arm 8 SILENT-ON-ERROR-PATH    classified as expected'
        } else {
            Write-Host 'self-test arm 8 SILENT-ON-ERROR-PATH    NOT classified - a FILE NAME fooled it'; $stFail = 1
        }
        if (Test-LocalBuildNoDiagnostic $real) {
            Write-Host 'self-test arm 9 REAL-COMPILE-ERROR      misclassified - it would HIDE a real defect'; $stFail = 1
        } else {
            Write-Host 'self-test arm 9 REAL-COMPILE-ERROR      left alone as expected'
        }
        $msvc = Join-Path $stDir 'msvc.log'
        Set-Content -LiteralPath $msvc -Value @(
            "src${at}mir${at}x.cpp(42): error C2065: 'q': undeclared identifier")
        $ldundef = Join-Path $stDir 'ldundef.log'
        Set-Content -LiteralPath $ldundef -Value @(
            "/usr/bin/ld: x.o: in function ``main':",
            "x.c:(.text+0x9): undefined reference to ``helper'")
        if ((Test-LocalBuildNoDiagnostic $msvc) -or (Test-LocalBuildNoDiagnostic $ldundef)) {
            Write-Host 'self-test arm 10 MSVC-AND-LD-DIAGNOSTIC misclassified - a vendor spelling is missing'; $stFail = 1
        } else {
            Write-Host 'self-test arm 10 MSVC-AND-LD-DIAGNOSTIC left alone as expected'
        }
        $msg = (Write-LocalBuildNoDiagnostic $silent (Join-Path $stDir 'no-such-marker') 6>&1 | Out-String)
        if ($msg -match 'PRINTED NO DIAGNOSTIC' -and $msg -match 'NOT EVIDENCE OF A SOURCE DEFECT' -and $msg -match 'RED-ON-DISABLE') {
            Write-Host 'self-test arm 11 MESSAGE                names the class, the mis-fix hazard and the red-on-disable hazard'
        } else {
            Write-Host "self-test arm 11 MESSAGE                incomplete: $msg"; $stFail = 1
        }
        # Arms 12..14 drive all THREE verdicts. An absent marker means nothing was
        # compared, which is a different sentence from "nothing changed".
        if ((Get-LocalBuildTouchedVerdict (Join-Path $stDir 'absent-marker')) -eq 'unchecked') {
            Write-Host 'self-test arm 12 VERDICT-UNCHECKED     no marker reported as NOT MEASURED'
        } else {
            Write-Host 'self-test arm 12 VERDICT-UNCHECKED     a missing marker did not report as unchecked'; $stFail = 1
        }
        $stMarker = Join-Path $stDir 'marker'
        Set-Content -LiteralPath $stMarker -Value '' -NoNewline
        if ((Get-LocalBuildTouchedVerdict $stMarker) -eq 'clean') {
            Write-Host 'self-test arm 13 VERDICT-CLEAN         nothing newer than the marker reported as clean'
        } else {
            Write-Host 'self-test arm 13 VERDICT-CLEAN         a quiet tree did not report as clean'; $stFail = 1
        }
        # ! THE POSITIVE VERDICT NEEDS A DETERMINISTIC GAP, AND WRITE-ORDER DOES NOT
        # PROVIDE ONE: a marker and a probe written back-to-back get mtimes equal to
        # the nanosecond on this host, and the comparison is strictly-greater. The
        # marker is back-dated HERE, in the fixture, for that reason alone.
        # ★★ PRODUCTION DELIBERATELY DOES NOT BACK-DATE - a marker aged even one
        # second would flag the file the caller had just edited before starting the
        # build, which is the ordinary workflow, so the report would fire on every
        # failed build and mean nothing.
        $stProbe = Join-Path 'tests' '.local-build-selftest-probe'
        Set-Content -LiteralPath $stMarker -Value '' -NoNewline
        (Get-Item -LiteralPath $stMarker).LastWriteTimeUtc = [datetime]::new(2000, 1, 1, 0, 0, 0, [System.DateTimeKind]::Utc)
        Set-Content -LiteralPath $stProbe -Value 'probe'
        try {
            if ((Get-LocalBuildTouchedVerdict $stMarker) -eq 'moved') {
                Write-Host 'self-test arm 14 VERDICT-MOVED         a file changed after the marker was seen'
            } else {
                Write-Host 'self-test arm 14 VERDICT-MOVED         a CHANGED FILE WAS MISSED - the measurement is blind'; $stFail = 1
            }
        } finally {
            Remove-Item -LiteralPath $stProbe -Force -ErrorAction SilentlyContinue
        }
    } finally {
        Remove-Item -LiteralPath $stDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($stFail -eq 0) { Write-Host 'local-build: self-test OK - 14 arms, both directions exercised.' }
    exit $stFail
}

# Refresh PATH from the system + user env vars. PowerShell sessions
# inherit their parent's PATH and don't pick up post-launch system
# updates (Windows broadcasts WM_SETTINGCHANGE but PowerShell doesn't
# listen). This refresh costs nothing and makes the script robust
# against the "PATH was updated but my shell is stale" case.
$env:PATH = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ';' + [System.Environment]::GetEnvironmentVariable("Path", "User")

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root

# ★ The two tree names this repository has an established meaning for. A tree name
# NOT in this table is allowed (lane trees are ordinary), but configuring one for
# the FIRST time then requires an explicit -BuildType: silently configuring a Debug
# tree that someone named `rel` is the kind of quiet wrong answer this file already
# shipped once.
$establishedBuildType = @{ 'dbg' = 'Debug'; 'rel' = 'Release' }

# ── The tree name must resolve to a DIRECTLY-NESTED child of build\ ──────────
# Everything destructive below is keyed on this, so it is validated by SHAPE
# rather than by trusting the caller: no separators, no `..`, non-empty. A name
# like `..\src` or `dbg\..\..` would otherwise walk the delete out of the
# container, which is the precise failure this rewrite exists to prevent.
if ([string]::IsNullOrEmpty($Tree) -or $Tree -match '[\\/]' -or $Tree -eq '.' -or $Tree -eq '..') {
    [Console]::Error.WriteLine("refusing tree name '$Tree': it must be a single directory name directly under build\")
    exit 3
}
$buildDir = Join-Path 'build' $Tree

if ($Clean) {
    # Belt and braces: the shape check above already guarantees this, and the
    # guarantee is cheap to restate at the one call site that cannot be undone.
    if ($buildDir -eq 'build' -or -not $buildDir.StartsWith('build' + [System.IO.Path]::DirectorySeparatorChar)) {
        [Console]::Error.WriteLine("refusing to remove '$buildDir': -Clean only ever removes ONE tree under build\")
        exit 3
    }
    if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir -Confirm:$false }
    $Configure = $true
}

# -- -BuildType is meaningful ONLY when this run configures a tree from scratch --
# WARNING: THIS CHECK USED TO LIVE INSIDE THE CONFIGURE BLOCK, AND THAT MADE IT A
# NO-OP on the commonest path: with `build.ninja` already present and no -Configure,
# the whole block was skipped, so `-BuildType Release` on the debug tree exited 0
# having silently ignored the flag. MEASURED 2026-08-20 by EXERCISING the arm rather
# than reading it. It runs after -Clean on purpose: a cleaned tree has no cache, so
# the flag IS meaningful there.
if ($BuildType -and (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    [Console]::Error.WriteLine("refusing -BuildType on the EXISTING tree 'build\$Tree': its cache already declares one. " +
                 "Re-run with -Clean to reconfigure it from scratch.")
    exit 3
}

if ($Configure -or -not (Test-Path (Join-Path $buildDir 'build.ninja'))) {
    $cmakeArgs = @('-S', '.', '-B', $buildDir, '-G', 'Ninja')
    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        # First configure of this tree: the build type must be KNOWN, not guessed.
        # An existing tree keeps whatever its cache already says, so no type is
        # passed and a caller cannot silently flip a configured tree underneath
        # the artifacts already in it.
        if (-not $BuildType) { $BuildType = $establishedBuildType[$Tree] }
        if (-not $BuildType) {
            [Console]::Error.WriteLine("refusing to configure a NEW tree 'build\$Tree' with no build type: pass -BuildType <Debug|Release|...> " +
                         "(only 'dbg' -> Debug and 'rel' -> Release are established names in this repository)")
            exit 3
        }
        $cmakeArgs += "-DCMAKE_BUILD_TYPE=$BuildType"
    }
    cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# Tee so the console still streams while a copy stays scannable. $LASTEXITCODE
# after a native command piped to Tee-Object is the NATIVE command's.
$buildLog = Join-Path $buildDir '.local-build-last.log'
# Stamped the instant BEFORE the build starts, so "newer than this" is exactly
# "changed while ninja was running". Written unconditionally: a marker left over
# from a previous run would date the wrong build and turn the one measurement the
# no-diagnostic reporter makes into a fabrication.
$buildMarker = Join-Path $buildDir '.local-build-started'
Set-Content -LiteralPath $buildMarker -Value '' -NoNewline
# ★★★ OPERATOR RULING 2026-08-25: "never use all CPUS, the idea is to keep build + tests + run always at 4 cpus", AMENDED same-day to "make it 6 cores, not 4, everywhere".
# A bare `cmake --build` hands off to ninja, whose default is ALL CORES.
$dssJobs = if ($env:DSS_JOBS) { $env:DSS_JOBS } else { '6' }
cmake --build $buildDir --parallel $dssJobs 2>&1 | Tee-Object -FilePath $buildLog
$buildRc = $LASTEXITCODE
if ($buildRc -ne 0 -and (Test-LocalBuildToolchainIoFailure $buildLog)) {
    Write-LocalBuildIoFailure $buildLog
    exit 9
}
# ! ORDER IS DELIBERATE: the I/O classifier runs FIRST because an I/O failure DOES
# print a diagnostic (`fatal error: error writing to ...`), so it is a strictly more
# specific class. Reaching here means the build failed having printed nothing any
# toolchain would call an error. Its OWN exit code, distinct from both 9 and a real
# compiler's rc, so a caller can tell "nothing was measured" from "your code is wrong".
if ($buildRc -ne 0 -and (Test-LocalBuildNoDiagnostic $buildLog)) {
    Write-LocalBuildNoDiagnostic $buildLog $buildMarker
    exit 10
}
if ($buildRc -ne 0) { exit $buildRc }

if ($Test) {
    Push-Location $buildDir
    try {
        # See scripts/run-gate/run-gate.sh for the measurement behind the 8.
        # Set CTEST_PARALLEL_LEVEL, or pass -j, to override.
        # ★ Restored afterwards: a .ps1 runs IN-PROCESS, so a bare assignment
        # would leave the caller's shell silently parallel for everything it
        # ran next. The .sh twin needs no such care -- `export` dies with it.
        $__cplPrev = $env:CTEST_PARALLEL_LEVEL
        $__cplSet  = $false
        if (-not $env:CTEST_PARALLEL_LEVEL) {
            # ★★★ OPERATOR RULING 2026-08-25: "never use all CPUS, the idea is to keep build + tests + run always at 4 cpus", AMENDED same-day to "make it 6 cores, not 4, everywhere".
            $env:CTEST_PARALLEL_LEVEL = '6'
            $__cplSet = $true
        }
        try {
            ctest --output-on-failure
        } finally {
            if ($__cplSet) {
                if ($null -eq $__cplPrev) {
                    Remove-Item Env:\CTEST_PARALLEL_LEVEL -ErrorAction SilentlyContinue
                } else {
                    $env:CTEST_PARALLEL_LEVEL = $__cplPrev
                }
            }
        }
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Pop-Location
    }
}
