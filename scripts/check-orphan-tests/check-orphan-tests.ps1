#!/usr/bin/env pwsh
# check-orphan-tests.ps1 — Windows twin of the ORPHAN TEST SOURCE guard.
# TWIN of `check-orphan-tests.sh`; same contract, same exit codes, same output.
#
# Contract: every `*.cpp` under the tests root MUST either be registered by a
# STRUCTURAL `dss_add_test(NAME ... SOURCES ...)` call in some
# `tests/**/CMakeLists.txt`, or appear in the ALLOWLIST below with a reason whose
# CMakeLists reference THIS GUARD VERIFIES.
#
# ★★ THE RATIONALE IS RESTATED HERE IN FULL rather than delegated to the `.sh`
# with a "see the other file". A twin that only points at its sibling is how the
# pair drifts, and `D-GATE-SCRIPT-PS1-CONTENT-DRIFT-UNCHECKED` is open precisely
# because two other twins in this repo have drifted before.
#
# ── WHAT AN ORPHAN TEST IS, AND WHY IT IS WORSE THAN A MISSING TEST ──────────
# An orphan is a `tests/**/*.cpp` that no `CMakeLists.txt` names. It compiles
# NOWHERE. No target links it, no ctest entry runs it, not one of its assertions
# ever executes. And it is INVISIBLE from every direction anyone normally looks:
# the file is present, it is committed, it reads like a test, `grep` finds its
# assertions, a reviewer reading the diff sees a test being added, and the suite
# goes green — because the suite never had anything to say about it. A MISSING
# test at least looks missing. This is absent coverage wearing the appearance of
# coverage, which is the strictly more dangerous of the two.
#
# ★ THIS IS THE FOURTH VACUOUS-TEST INSTANCE FOUND IN A SINGLE CYCLE (2026-08-10):
# an op-count floor asserting >= 8 against a live 12; a macho example that passed
# for four cycles while reading a narrow `char**` through an `unsigned short**`; a
# red-on-disable arm asserting a rejection that does not happen. Each of those
# RAN and asserted nothing useful. This class is the same defect one step earlier.
# ✔MEASURED 2026-08-10, before this guard existed: NOTHING in this repository
# checked for it, and no CI job asserted an expected ctest entry count.
#
# ── WHY THE PARSE IS STRUCTURAL AND NOT A GREP ───────────────────────────────
# ★★ A LOOSE REGEX OVER THE CMakeLists TEXT GIVES THE WRONG ANSWER, in the
# direction that greens the guard. ✔MEASURED 2026-08-10: a loose scan reports
# **240** `dss_add_test` mentions and **234** unique `.cpp` strings, against
# **230** files on disk and **228** real registrations. The surplus is the
# function DEFINITION, its usage docblock, and prose comments — i.e. a guard built
# on those numbers would count a COMMENT as coverage. So comments are stripped
# QUOTE-AWARE, `dss_add_test` is matched only as a whole word followed by `(`, and
# the arguments are read to the MATCHING `)` so the 43 MULTI-LINE calls parse like
# the 185 single-line ones (185 + 43 = 228).
# ✔MEASURED token shapes over all 228 registrations: 217 plain same-directory, 11
# subdirectory-relative, ZERO absolute, ZERO with `..`, ZERO quoted, ZERO with an
# unexpanded `${...}`. The resolver implements `${CMAKE_CURRENT_SOURCE_DIR}`,
# `.`/`..` collapsing and absolute detection anyway, and FAILS LOUD on anything it
# cannot resolve: a token silently dropped would manufacture a phantom orphan, and
# one silently accepted could hide a real one.
#
# ── EXIT CODES (identical to the `.sh`) ──────────────────────────────────────
#   0  every source is registered or allowlisted
#   1  ORPHAN — a source is registered nowhere
#   2  SCAN COLLAPSED — missing root, a per-dimension floor, an unresolvable
#      token, a registration naming no file, or a FAILED SELF-TEST
#   3  STALE ALLOWLIST — an exemption that no longer describes reality
# Precedence 2 > 1 > 3; all classes report in ONE run.
#
# ── USAGE ────────────────────────────────────────────────────────────────────
#   check-orphan-tests.ps1              scan <repo>/tests AND run the self-test
#   check-orphan-tests.ps1 <root>       scan <root> only (no self-test)
#
# ★ Every line this script prints is ASCII-ONLY and carries NO absolute path,
# deliberately: the twins are verified by DIFFING their output byte-for-byte, and
# neither a console encoding nor a path separator may be able to fake a
# disagreement. Two divergences the sibling anchor guard had to document are
# CLOSED here rather than re-documented: `-Force` makes the walk descend hidden
# paths the way `find` does, and the case-SENSITIVE `-ceq` filters match
# `find -name '*.cpp'` instead of also taking `TEST.CPP`.

param([string]$Root = '')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ScriptAbs = Join-Path $ScriptDir 'check-orphan-tests.ps1'
$RepoRoot  = Split-Path -Parent (Split-Path -Parent $ScriptDir)

# ── FAIL-CLOSED PER-DIMENSION FLOORS ────────────────────────────────────────
# ★★ THREE floors, one per dimension, because a collapse in ANY ONE of them
# produces a clean-looking pass. Lose the source enumeration and there is nothing
# to accuse; lose the CMakeLists enumeration and nothing is registered so every
# source is an orphan (loud, at least); lose only the PARSE and the registered set
# shrinks silently while both enumerations still look healthy — that last one is
# the shape that greens a guard while it checks nothing.
# ⚠ THIS REPOSITORY HAS ALREADY SHIPPED EXACTLY THAT BUG:
# `D-GATE-ANCHOR-GUARD-FAILS-OPEN-ON-MISSING-ROOT`, where the anchor guard printed
# `OK (1 src anchors all resolve to plans)` while scanning nothing at all.
# ★ AND THE FLOORS ARE NOT THE ASSERTION. The assertion is the PROPERTY — every
# source is registered or allowlisted — which holds at any tree size. An exact
# pinned orphan count of 0 is deliberately NOT used: such a pin is satisfied by a
# scan that found nothing to count.
# ★ BOTH TWINS MUST CARRY THE SAME VALUES or the pairing is decorative.
$SourceFloor       = 150
$CMakeListsFloor   = 12
$RegistrationFloor = 150

# ── THE ALLOWLIST ───────────────────────────────────────────────────────────
# Row format, `|`-separated:  <source>|<CMakeLists that references it>|<reason>
# Both paths are relative to the TESTS ROOT. Reasons carry no `|` and no tab.
#
# ★★ AN EXEMPTION LIST THAT CAN ROT IS THE "GUARD WEAKENED EVERY TIME IT FIRES"
# PATTERN, anchored TWICE in this repo
# (`D-TEST-PE64-CONFOUND-PIN-WEAKENED-BY-ITS-OWN-SUBJECT`). So every entry is
# MACHINE-CHECKED on every run, three ways, and any failure is exit 3:
#   (a) the source still EXISTS under the tests root;
#   (b) the source is NOT registered by `dss_add_test` — the moment the real
#       mechanism covers it, the exemption is a false statement and must go;
#   (c) the CMakeLists named in column 2 still STRUCTURALLY references the source.
#       This is what turns the "reason" from PROSE into a CHECKED CLAIM.
# ⚠ (c) is verified structurally — a mention in a COMMENT does not satisfy it.
# ★ BOTH TWINS MUST CARRY BYTE-IDENTICAL ROWS.
$OrphanAllowlist = @(
    'examples/examples_runner.cpp|examples/CMakeLists.txt|NOT a gtest suite. It is the ONE example-corpus runner binary, built by add_executable(dss_examples_runner examples_runner.cpp) at examples/CMakeLists.txt:18 and driven per-example by the ctest entries that same file generates from the expected.json glob. Registering it via dss_add_test would make it a test in its own right, which it is not.',
    'test_support/pch_stub.cpp|CMakeLists.txt|NOT a test. A translation unit that exists only so add_library(dss_test_pch STATIC ...) at CMakeLists.txt:89 has something to compile, which is what lets target_precompile_headers PRODUCE the one shared test PCH every dss_add_test target consumes via REUSE_FROM. It contains no assertions and defines no symbol worth running.'
)

# ── ROOT SELECTION ──────────────────────────────────────────────────────────
# ★ The label printed is `tests` for the default root and the ARGUMENT VERBATIM
# for a scoped run — never a resolved absolute path. That is what lets the two
# twins' whole output be compared byte-for-byte on the same subject: `pwd` in Git
# Bash answers `/c/...` where PowerShell answers `C:\...`, and that difference
# would read as a "disagreement" that is really two shells describing one
# directory.
$RunSelfTest = $false
if ($Root -ne '') {
    $TestsRootIn = $Root
    $RootLabel   = $Root
} else {
    $TestsRootIn = Join-Path $RepoRoot 'tests'
    $RootLabel   = 'tests'
    $RunSelfTest = $true
}

Write-Host "orphan-tests: root=$RootLabel"

if (-not (Test-Path -LiteralPath $TestsRootIn -PathType Container)) {
    Write-Host "orphan-tests: FAIL - the tests root does not exist. Refusing to report a pass on a scan of nothing."
    Write-Host "orphan-tests: FAIL - 0 sources / 0 CMakeLists / 0 registrations from 0 calls / 0 allowlisted: 0 orphan(s), 0 stale allowlist entries."
    exit 2
}
$TestsRoot = (Resolve-Path -LiteralPath $TestsRootIn).ProviderPath.TrimEnd([IO.Path]::DirectorySeparatorChar)

# ════════════════════════════════════════════════════════════════════════════
# THE PARSER — the twin of the single awk program in the `.sh`. Emits the same
# four record kinds into the same three collections:
#   R <resolved source> <NAME> <cmakelists>   a dss_add_test registration
#   T <cmakelists> <resolved source>          any STRUCTURAL .cpp token
#   E <message>                               fail-loud: the parse is incomplete
#   C <n>                                     dss_add_test calls parsed
# Every path is tests-root-relative and forward-slashed, so the records are host-
# and shell-independent.
# ════════════════════════════════════════════════════════════════════════════
$script:InQuote = $false

# Quote-aware "#" comment strip for ONE line, carrying quote state across lines in
# $script:InQuote. A "#" inside a double-quoted CMake argument is CONTENT, not a
# comment. ✔MEASURED 2026-08-10: zero such cases and zero multi-line quoted
# strings exist under tests/ today - written correctly anyway, because "correct
# only while an incidental property holds" is how the next reader inherits a
# silent mis-parse.
function Get-StrippedLine([string]$line) {
    $out = ''
    $rest = $line
    while ($rest -ne '') {
        if (-not $script:InQuote) {
            $q = $rest.IndexOf('"')
            $h = $rest.IndexOf('#')
            if ($h -ge 0 -and ($q -lt 0 -or $h -lt $q)) { return $out + $rest.Substring(0, $h) }
            if ($q -ge 0) {
                $out += $rest.Substring(0, $q + 1); $rest = $rest.Substring($q + 1)
                $script:InQuote = $true; continue
            }
            return $out + $rest
        }
        $q = $rest.IndexOf('"')
        if ($q -ge 0) {
            $out += $rest.Substring(0, $q + 1); $rest = $rest.Substring($q + 1)
            $script:InQuote = $false; continue
        }
        return $out + $rest
    }
    return $out
}

function Get-DirOf([string]$p) {
    $i = $p.LastIndexOf('/')
    if ($i -lt 0) { return '' }
    return $p.Substring(0, $i)
}

# Join dir + token and collapse "." / "..". Returns '' if the path escapes the
# tests root, which is a fail-loud condition and never a silent skip.
function Get-NormPath([string]$dir, [string]$tok) {
    if ($dir -eq '') { $full = $tok } else { $full = "$dir/$tok" }
    $stack = [System.Collections.ArrayList]::new()
    foreach ($part in ($full -split '/')) {
        if ($part -eq '' -or $part -eq '.') { continue }
        if ($part -eq '..') {
            if ($stack.Count -eq 0) { return '' }
            $stack.RemoveAt($stack.Count - 1); continue
        }
        [void]$stack.Add($part)
    }
    return ($stack -join '/')
}

# Resolve one CMake source token to a tests-root-relative path. $quiet suppresses
# the E record: the T pass sets it, because an unresolvable token there can only
# cost an allowlist entry its confirmation - i.e. it fails CLOSED (exit 3) rather
# than exempting anything, and reporting it twice would just be noise.
function Resolve-SourceToken([string]$fname, [string]$dir, [string]$tok, [bool]$quiet, [System.Collections.ArrayList]$errs) {
    $t = $tok.Replace('"', '')
    $t = $t.Replace('${CMAKE_CURRENT_SOURCE_DIR}', '.')
    if ($t.Contains('${')) {
        if (-not $quiet) { [void]$errs.Add("${fname}: SOURCES token $tok carries a CMake variable this guard cannot resolve, so the parse is INCOMPLETE. Teach BOTH twins to expand it, or use a literal path; do not let a token go unread.") }
        return ''
    }
    if ($t.StartsWith('/') -or $t -match '^[A-Za-z]:') {
        if (-not $quiet) { [void]$errs.Add("${fname}: SOURCES token $tok is an ABSOLUTE path, so it cannot be placed in the tests-root-relative scan set.") }
        return ''
    }
    $p = Get-NormPath $dir $t
    if ($p -eq '') {
        if (-not $quiet) { [void]$errs.Add("${fname}: SOURCES token $tok resolves OUTSIDE the tests root, so this guard cannot account for it.") }
        return ''
    }
    return $p
}

# The whole-file parse. Mirrors awk's proc(): registrations first, then every
# structural .cpp token for allowlist reference-site verification.
function Invoke-CMakeListsParse([string[]]$relFiles, [string]$rootAbs) {
    $regs = [System.Collections.ArrayList]::new()   # @{ Path; Name; File }
    $toks = [System.Collections.ArrayList]::new()   # "file`tpath"
    $errs = [System.Collections.ArrayList]::new()
    $calls = 0
    $callRx = [regex]::new('dss_add_test[ \t]*\(')
    foreach ($rel in $relFiles) {
        $raw = [IO.File]::ReadAllText((Join-Path $rootAbs ($rel -replace '/', [string][IO.Path]::DirectorySeparatorChar)))
        $script:InQuote = $false
        $sb = [Text.StringBuilder]::new()
        foreach ($ln in ($raw -split "`n")) {
            [void]$sb.Append((Get-StrippedLine ($ln -replace "`r$", ''))).Append("`n")
        }
        $text = $sb.ToString()
        $dir = Get-DirOf $rel

        $rest = $text
        while ($true) {
            $m = $callRx.Match($rest)
            if (-not $m.Success) { break }
            $s = $m.Index
            $l = $m.Length
            # Whole-word only. Without this, an identifier ENDING in dss_add_test
            # would be read as a call. (function(dss_add_test) is already excluded
            # by the required "(" after the name.)
            if ($s -gt 0 -and $rest[$s - 1] -match '[A-Za-z0-9_]') { $rest = $rest.Substring($s + $l); continue }
            $i = $s + $l
            $depth = 1
            while ($i -lt $rest.Length) {
                $c = $rest[$i]
                if ($c -eq '(') { $depth++ }
                elseif ($c -eq ')') { $depth--; if ($depth -eq 0) { break } }
                $i++
            }
            if ($depth -ne 0) {
                [void]$errs.Add("${rel}: an unterminated dss_add_test( call - the parse cannot be trusted.")
                break
            }
            $argtext = $rest.Substring($s + $l, $i - ($s + $l)).Replace("`t", ' ').Replace("`n", ' ')
            $mode = ''; $name = ''; $srcCount = 0
            foreach ($t in ($argtext -split ' +')) {
                if ($t -eq '') { continue }
                # NAME and SOURCES are the only keywords dss_add_test declares
                # (cmake_parse_arguments at tests/CMakeLists.txt:10), so anything
                # after SOURCES that is not NAME is a source. Mirroring the real
                # helper is the point: a parser that invents keywords would
                # silently drop sources.
                if ($t -eq 'NAME' -or $t -eq 'SOURCES') { $mode = $t; continue }
                if ($mode -eq 'NAME') { $name = $t; $mode = ''; continue }
                if ($mode -eq 'SOURCES') {
                    $p = Resolve-SourceToken $rel $dir $t $false $errs
                    if ($p -ne '') { [void]$regs.Add([pscustomobject]@{ Path = $p; Name = $name; File = $rel }); $srcCount++ }
                }
            }
            $calls++
            if ($name -eq '') { [void]$errs.Add("${rel}: a dss_add_test call declares no NAME.") }
            if ($srcCount -eq 0) { [void]$errs.Add("${rel}: dss_add_test(NAME $name) registers no resolvable source.") }
            $rest = $rest.Substring($i + 1)
        }

        # Every STRUCTURAL .cpp token in the file, for allowlist reference-site
        # verification. "(" and ")" are CMake token separators, so they are split
        # on here too - without that, add_executable(r examples_runner.cpp) yields
        # the token examples_runner.cpp) and the reference is missed. ✔MEASURED:
        # an earlier draft did exactly that and reported both allowlisted files as
        # referenced NOWHERE, which would have made both rows look stale.
        $flat = $text.Replace("`t", ' ').Replace("`n", ' ').Replace('(', ' ').Replace(')', ' ')
        foreach ($t0 in ($flat -split ' +')) {
            $t = $t0.Replace('"', '')
            if ($t.Length -lt 5) { continue }
            if ($t.Substring($t.Length - 4) -cne '.cpp') { continue }
            $p = Resolve-SourceToken $rel $dir $t $true $errs
            if ($p -ne '') { [void]$toks.Add("$rel`t$p") }
        }
    }
    return [pscustomobject]@{ Regs = $regs; Toks = $toks; Errs = $errs; Calls = $calls }
}

# ⚠ EVERY CALL SITE WRAPS THIS IN `@(...)`, AND THAT IS LOAD-BEARING — two
# consecutive PowerShell array-semantics traps were MEASURED here on 2026-08-10,
# both found by RUNNING the self-test rather than by reading the code:
#   (1) `$srcList = Sort-Ordinal @(...)` with an EMPTY result assigns `$null`,
#       because the function's output unrolls to nothing — and the very next line,
#       `$srcList.Count`, then THROWS under `Set-StrictMode -Version Latest`. The
#       empty-root arm died with a PowerShell exception (exit 1) instead of the
#       guard's own collapse verdict (exit 2): red for the wrong reason, on exactly
#       the input the floors exist to handle.
#   (2) The obvious fix for (1) — `return ,$c` to stop the unrolling — is WORSE.
#       Combined with `@(...)` at the call site it yields a ONE-element array whose
#       single element is the whole string array, so `.Count` read 1 and every
#       report printed all 230 paths space-joined onto one line. `1 sources`,
#       `PHANTOM: <blank>` and a `Copy-Item` against a 21-path filename were the
#       symptoms.
# ⇒ The correct combination is UNROLL here (`return $c`) and `@(...)` at the call
# site, which is right for both the empty and the populated case.
function Sort-Ordinal([string[]]$a) {
    $c = [string[]]@($a)
    [Array]::Sort($c, [StringComparer]::Ordinal)
    return $c
}

# ════════════════════════════════════════════════════════════════════════════
# THE SCAN
# ════════════════════════════════════════════════════════════════════════════
# ORDINAL sort, matching the `.sh`'s `LC_ALL=C sort`. A culture-aware collation
# would order `test_support/` differently and make the twins disagree for no real
# reason. `-Force` descends hidden paths the way `grep -r`/`find` do, and `-ceq`
# keeps the extension match case-SENSITIVE like `find -name '*.cpp'`.
$allFiles = @(Get-ChildItem -LiteralPath $TestsRoot -Recurse -File -Force)
$srcList = @(Sort-Ordinal @($allFiles |
    Where-Object { $_.Extension -ceq '.cpp' } |
    ForEach-Object { $_.FullName.Substring($TestsRoot.Length + 1).Replace([IO.Path]::DirectorySeparatorChar, '/') }))
$cmlList = @(Sort-Ordinal @($allFiles |
    Where-Object { $_.Name -ceq 'CMakeLists.txt' } |
    ForEach-Object { $_.FullName.Substring($TestsRoot.Length + 1).Replace([IO.Path]::DirectorySeparatorChar, '/') }))
$nSrc = $srcList.Count
$nCml = $cmlList.Count

$collapsed = $false
$collapseMsgs = [System.Collections.ArrayList]::new()

$parsed = $null
if ($nCml -eq 0) {
    [void]$collapseMsgs.Add("orphan-tests: FAIL - found 0 CMakeLists.txt under the tests root, below its floor of $CMakeListsFloor.")
    [void]$collapseMsgs.Add("  Nothing was parsed, so NOTHING is registered. Refusing to report a verdict at all; fix the scan, do not lower the floor.")
    $collapsed = $true
    $parsed = [pscustomobject]@{
        Regs = [System.Collections.ArrayList]::new(); Toks = [System.Collections.ArrayList]::new()
        Errs = [System.Collections.ArrayList]::new(); Calls = 0 }
} else {
    # A parser that DIES must be a collapse, never a quiet under-count: a partial
    # parse can only lose registrations, i.e. it can never be trusted to clear a
    # source. The `.sh` twin reads awk's rc for the same reason.
    try {
        $parsed = Invoke-CMakeListsParse $cmlList $TestsRoot
    } catch {
        [void]$collapseMsgs.Add("orphan-tests: FAIL - the CMakeLists parser threw. A partial parse can only UNDER-count registrations, so it can never be trusted to clear a source. Refusing to report a verdict.")
        $collapsed = $true
        $parsed = [pscustomobject]@{
            Regs = [System.Collections.ArrayList]::new(); Toks = [System.Collections.ArrayList]::new()
            Errs = [System.Collections.ArrayList]::new(); Calls = 0 }
    }
}

$regList = @(Sort-Ordinal @($parsed.Regs | ForEach-Object { $_.Path } | Select-Object -Unique))
$tokSet  = [System.Collections.Generic.HashSet[string]]::new([string[]]@($parsed.Toks), [StringComparer]::Ordinal)
$srcSet  = [System.Collections.Generic.HashSet[string]]::new([string[]]$srcList, [StringComparer]::Ordinal)
$regSet  = [System.Collections.Generic.HashSet[string]]::new([string[]]$regList, [StringComparer]::Ordinal)
$cmlSet  = [System.Collections.Generic.HashSet[string]]::new([string[]]$cmlList, [StringComparer]::Ordinal)
$nReg      = $regList.Count
$nRegCalls = $parsed.Calls
$nErr      = $parsed.Errs.Count

# ── FLOORS, one per dimension ───────────────────────────────────────────────
if ($nSrc -lt $SourceFloor) {
    [void]$collapseMsgs.Add("orphan-tests: FAIL - the source scan found only $nSrc *.cpp under the tests root, below its floor of $SourceFloor.")
    [void]$collapseMsgs.Add("  This does NOT mean every test is wired - it means the SCAN COLLAPSED. Refusing to report a pass; fix the scan, do not lower the floor.")
    $collapsed = $true
}
if ($nCml -lt $CMakeListsFloor) {
    [void]$collapseMsgs.Add("orphan-tests: FAIL - the CMakeLists scan found only $nCml files, below its floor of $CMakeListsFloor.")
    [void]$collapseMsgs.Add("  A collapsed CMakeLists scan registers nothing, so it cannot clear a single source. Refusing to report a pass; fix the scan, do not lower the floor.")
    $collapsed = $true
}
if ($nReg -lt $RegistrationFloor) {
    [void]$collapseMsgs.Add("orphan-tests: FAIL - the parser resolved only $nReg distinct registered sources, below its floor of $RegistrationFloor.")
    [void]$collapseMsgs.Add("  THIS IS THE DIMENSION THAT FAILS QUIETEST: both enumerations can look healthy while the PARSE has stopped matching, and every clear then evaporates. Refusing to report a pass; fix the parser, do not lower the floor.")
    $collapsed = $true
}
if ($nErr -gt 0) {
    [void]$collapseMsgs.Add("orphan-tests: FAIL - $nErr CMakeLists token(s) could not be resolved, so the parse is INCOMPLETE:")
    foreach ($e in $parsed.Errs) { [void]$collapseMsgs.Add("    $e") }
    $collapsed = $true
}

# ── PHANTOM REGISTRATIONS: registered, but no such file ─────────────────────
# ★★ THE OTHER DIRECTION OF THE SAME PROPERTY, and it was MEASURED MISSING.
# ✔MEASURED 2026-08-10 by sabotage: with quote-aware comment stripping REMOVED
# from the parser, this guard stayed fully GREEN — the usage docblock in
# tests/CMakeLists.txt contains a commented `dss_add_test(NAME core/test_strong_ids
# SOURCES test_strong_ids.cpp)`, which then parsed as a real registration for a
# path that does not exist. Nothing noticed, because the guard only ever asked "is
# every FILE registered?" and never "does every REGISTRATION name a file?". A
# registered path that resolves to nothing clears nobody, so a registered set full
# of fiction is a census that cannot be trusted - hence the collapse class. CMake
# itself errors on a missing source at configure time, so this should be
# unreachable in practice; the value is that it makes comment stripping GUARDED.
$phantoms = @($regList | Where-Object { -not $srcSet.Contains($_) })
$nPhantom = $phantoms.Count
if ($nPhantom -gt 0) {
    [void]$collapseMsgs.Add("orphan-tests: FAIL - $nPhantom registration(s) name a source that does NOT exist under the tests root:")
    $pi = 0
    foreach ($p in $phantoms) {
        $pi++
        if ($pi -le 20) { [void]$collapseMsgs.Add("    PHANTOM: $p") }
    }
    if ($nPhantom -gt 20) { [void]$collapseMsgs.Add("    ... and $($nPhantom - 20) more.") }
    [void]$collapseMsgs.Add("  A registration pointing at nothing clears nobody, so the registered set is partly fiction and this guard refuses to clear anything from it. Either the source was renamed without its CMakeLists, or the parser is reading text that is not a registration at all.")
    $collapsed = $true
}

# ── ALLOWLIST VALIDATION ────────────────────────────────────────────────────
$allowRows = @($OrphanAllowlist | Where-Object { @($_ -split '\|').Count -ge 3 })
$allowList = @(Sort-Ordinal @($allowRows | ForEach-Object { ($_ -split '\|')[0] } | Select-Object -Unique))
$nAllow = $allowList.Count

$nStale = 0
$staleMsgs = [System.Collections.ArrayList]::new()
foreach ($row in $allowRows) {
    $f = $row -split '\|'
    $ap = $f[0]; $asite = $f[1]
    if ($ap -eq '' -or $asite -eq '') { continue }
    if (-not $srcSet.Contains($ap)) {
        [void]$staleMsgs.Add("  STALE: the allowlist names '$ap', which does NOT exist under the tests root.")
        [void]$staleMsgs.Add("         An exemption for a deleted file reads like live coverage policy and protects nothing. Delete the row.")
        $nStale++; continue
    }
    if ($regSet.Contains($ap)) {
        [void]$staleMsgs.Add("  STALE: the allowlist exempts '$ap', but dss_add_test now REGISTERS it.")
        [void]$staleMsgs.Add("         The real mechanism covers it, so the exemption is a false statement about this tree. Delete the row.")
        $nStale++; continue
    }
    if (-not $cmlSet.Contains($asite)) {
        [void]$staleMsgs.Add("  STALE: the allowlist says '$ap' is referenced by '$asite', which does not exist.")
        $nStale++; continue
    }
    if (-not $tokSet.Contains("$asite`t$ap")) {
        [void]$staleMsgs.Add("  STALE: '$asite' no longer STRUCTURALLY references '$ap'.")
        [void]$staleMsgs.Add("         That reference IS the reason the row exists, so the file may now be genuinely orphaned. Re-check it, then either register it or correct the row - do not widen the exemption.")
        $nStale++
    }
}

# ── THE PROPERTY: every source is registered or allowlisted ─────────────────
$cleared = [System.Collections.Generic.HashSet[string]]::new([string[]]$regList, [StringComparer]::Ordinal)
foreach ($a in $allowList) { [void]$cleared.Add($a) }
$orphans = @($srcList | Where-Object { -not $cleared.Contains($_) })
$nOrphan = $orphans.Count

# ════════════════════════════════════════════════════════════════════════════
# REPORT — every failure class in ONE run. A guard that aborts after the first
# problem makes the reader re-run it N times to learn N things, and the
# remediation text never prints at all.
# ════════════════════════════════════════════════════════════════════════════
foreach ($m in $collapseMsgs) { Write-Host $m }

if ($nOrphan -gt 0) {
    Write-Host "orphan-tests: FAIL - $nOrphan test source(s) under the tests root are named by NO CMakeLists.txt."
    Write-Host "They compile nowhere and no ctest entry runs them. Every assertion inside them is dead"
    Write-Host "text. This is ABSENT COVERAGE wearing the appearance of coverage: the file is there, it"
    Write-Host "reads like a test, the diff that added it looked like added coverage, and the suite is"
    Write-Host "green because it never had anything to say about them."
    Write-Host ""
    foreach ($o in $orphans) { Write-Host "  ORPHAN: $o" }
    Write-Host ""
    Write-Host "Fix: either"
    Write-Host "  (a) REGISTER it - add dss_add_test(NAME <dir>/<stem> SOURCES <file>) to the"
    Write-Host "      CMakeLists.txt of its own directory, then RUN it. A registration that compiles"
    Write-Host "      is not yet a test that asserts; check that it fails when it should."
    Write-Host "  (b) DELETE it, if it was superseded and nobody noticed because nothing ran it."
    Write-Host "  (c) if it is genuinely NOT a test source (a runner main, a PCH stub), add a row to"
    Write-Host "      the ALLOWLIST in BOTH scripts/check-orphan-tests/check-orphan-tests.sh and"
    Write-Host "      scripts/check-orphan-tests/check-orphan-tests.ps1, naming the reason AND the CMakeLists that"
    Write-Host "      references it. The guard verifies that reference on every run, so the exemption"
    Write-Host "      cannot quietly stop being true."
    Write-Host "Do NOT widen this guard to make a red go away. An orphan is the one defect here that"
    Write-Host "costs nothing to fix and everything to leave in place."
}

if ($nStale -gt 0) {
    Write-Host "orphan-tests: FAIL - the ALLOWLIST no longer describes this tree:"
    Write-Host ""
    foreach ($m in $staleMsgs) { Write-Host $m }
    Write-Host "An exemption list that can rot is the pattern this repository has anchored twice: a"
    Write-Host "guard weakened every time it fires ends up asserting nothing. Fix the ROW."
}

$verdict = 'OK'
if ($collapsed -or $nOrphan -gt 0 -or $nStale -gt 0) { $verdict = 'FAIL' }
Write-Host "orphan-tests: $verdict - $nSrc sources / $nCml CMakeLists / $nReg registrations from $nRegCalls calls / $nAllow allowlisted: $nOrphan orphan(s), $nStale stale allowlist entries."

$finalRc = 0
if ($nStale -gt 0)  { $finalRc = 3 }
if ($nOrphan -gt 0) { $finalRc = 1 }
if ($collapsed)     { $finalRc = 2 }

# ════════════════════════════════════════════════════════════════════════════
# RED-ON-DISABLE SELF-TEST — the guard PROVES it can fail, on every ctest run.
# TWIN of the block of the same name in `check-orphan-tests.sh`; same 12 arms,
# same order, same verdict lines.
#
# ★★ EXERCISE THE FAILURE ARM, DO NOT READ IT. `D-TEST-NONFATAL-GUARD-DEGRADES-
# TO-A-VACUOUS-PASS` and `D-CENSUS-INSTRUMENT-UNGUARDED-BY-CTEST` are both in this
# repository's registry because an instrument nobody executed was believed. The
# self-test therefore lives INSIDE the guard: it is impossible to run this check
# without also proving it reds, and there is exactly ONE thing to wire into ctest.
#
# THE SUBJECT is a MIRROR in a per-run temp directory: the CMakeLists copied
# byte-for-byte, every `*.cpp` created EMPTY. That is faithful precisely because
# this guard never opens a `.cpp`, and the mirror's own GREEN CONTROL arm PROVES
# the faithfulness (it compares the mirror's census against the live one) instead
# of asserting it. Per-run temp, never a fixed path
# (`D-TEST-FIXED-SCRATCH-PATH-POPULATION`).
#
# EACH ARM RE-INVOKES THE SCRIPT AS A SUBPROCESS and reads its EXIT CODE. An
# in-process helper would be cheaper and would NOT test the contract ctest
# consumes — and "printed failed=0 and exited 2" is a defect this repo carried for
# weeks, so the exit code is part of what has to be proven.
#
# FAIL-CLOSED RULES, all ENFORCED rather than described: green control first; the
# witness UNIQUE by SELECTION so selector and assertion cannot drift; the mutant
# differs by BYTES (a full comparison, stronger than the hash it approximates);
# absence proven BY THE GUARD'S OWN `  ORPHAN:` line; the mutant STILL PARSES,
# asserted numerically off the guard's own summary; every restore byte-verified;
# and nothing naming the mutation written inside the mutated tree.
# ════════════════════════════════════════════════════════════════════════════
if (-not $RunSelfTest) { exit $finalRc }

$stFail = 0
$stRc = 0
$stFile = [IO.Path]::Combine([IO.Path]::GetTempPath(), "orphan-st-$PID-$(Get-Random).txt")

# ★★ SNAPSHOT THE CENSUS INTO NAMES NOTHING BELOW WRITES TO. Every arm's expected
# values are derived from these three, never from the live `$nSrc`/`$nCml`/`$nReg`.
# ✔MEASURED WHY: a `$nsrc` loop counter in the witness selector below silently
# rewrote `$nSrc` (PowerShell is case-insensitive), and seven arms then compared
# against a corrupted expectation. That specific collision is fixed at its source,
# but an expectation that can be mutated between the measurement and the assertion
# is a structural hazard, not a one-off typo - so the values are captured once,
# here, under names used nowhere else.
$baseSrc = $nSrc
$baseCml = $nCml
$baseReg = $nReg

# ── RESOLVING THE POWERSHELL HOST TO RE-INVOKE ──────────────────────────────
# ★ NOT `(Get-Process -Id $PID).Path`, and that is a MEASURED correction rather
# than a preference. ✔MEASURED 2026-08-10 on this workstation: PowerShell 7.5.2 is
# installed as a DOTNET GLOBAL TOOL, so the current process's image is
# `C:\Program Files\dotnet\dotnet.exe` and re-invoking it produced
# `dotnet--NoProfile does not exist` — the self-test would have died on its FIRST
# arm for a reason having nothing to do with this guard. `$PSHOME` is the reliable
# anchor in both editions (it holds `pwsh.exe` for 7.x and `powershell.exe` for
# 5.1); PATH lookup is only the fallback.
# ★★ AND IF NO HOST CAN BE FOUND, THIS IS A FAILURE, NEVER A SKIP. A self-test
# that quietly does not run is precisely `D-TEST-NONFATAL-GUARD-DEGRADES-TO-A-
# VACUOUS-PASS`: the guard would print its census and look fine while having
# proven nothing at all.
$hostExe = ''
foreach ($cand in @((Join-Path $PSHOME 'pwsh.exe'), (Join-Path $PSHOME 'powershell.exe'))) {
    if (Test-Path -LiteralPath $cand -PathType Leaf) { $hostExe = $cand; break }
}
if ($hostExe -eq '') {
    foreach ($n in @('pwsh', 'powershell')) {
        $c = Get-Command $n -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($c) { $hostExe = $c.Source; break }
    }
}
if ($hostExe -eq '') {
    Write-Host "orphan-tests: SELF-TEST FAIL - no PowerShell host executable could be located (looked in"
    Write-Host "  `$PSHOME and then on PATH for pwsh/powershell), so the self-test cannot re-invoke this"
    Write-Host "  guard. Refusing to report a proven verdict it did not prove."
    exit 2
}

function Invoke-Guard([string]$root) {
    # rc DIRECTLY from $LASTEXITCODE, and every later read is a FILE read, never a
    # pipe. ★ The `.sh` twin's first draft asserted through `printf | grep -q`
    # under `set -o pipefail`, which is a LATENT FLAKE (grep -q exits early,
    # printf takes SIGPIPE, pipefail hands the pipeline 141, and a SATISFIED
    # assertion reads as a FAILED one). Both twins now go through a file.
    & $hostExe -NoProfile -ExecutionPolicy Bypass -File $ScriptAbs $root *> $stFile
    $script:stRc = $LASTEXITCODE
}
# Read a census number off the guard's own summary line BY LABEL, never by
# position. ★ An index-counted field was the first draft and it was already wrong
# by one (it read the stale count where the orphan count belongs) - the
# COUNT-over-CONTENT mistake in miniature: a label cannot silently shift.
function Get-StNum([string]$want) {
    foreach ($line in [IO.File]::ReadAllLines($stFile)) {
        if ($line -notmatch '^orphan-tests: (OK|FAIL) - ') { continue }
        $w = $line -split ' +'
        for ($i = 1; $i -lt $w.Count; $i++) {
            if ($w[$i] -ceq $want -and $w[$i - 1] -match '^[0-9]+$') { return $w[$i - 1] }
        }
    }
    return ''
}
function Test-StExpect([string]$arm, [int]$want, [string]$note) {
    if ($script:stRc -ne $want) {
        Write-Host "orphan-tests: SELF-TEST FAIL - arm $arm exited $($script:stRc), expected $want. This guard CANNOT be"
        Write-Host "  trusted: an arm built to red did not red, so a green run of it says nothing."
        foreach ($l in [IO.File]::ReadAllLines($stFile)) { Write-Host "    | $l" }
        $script:stFail = 1
        return $false
    }
    Write-Host "orphan-tests: self-test arm $arm rc=$want as expected$note"
    return $true
}
function Test-StSays([string]$arm, [string]$needle) {
    $all = [IO.File]::ReadAllText($stFile)
    if (-not $all.Contains($needle)) {
        Write-Host "orphan-tests: SELF-TEST FAIL - arm $arm exited as expected but its message never said"
        Write-Host "  '$needle', so the red does not tell the reader what actually happened."
        $script:stFail = 1
    }
}
# ★★ ABSENCE, asserted. This is what makes the three FLOOR arms isolate one floor
# each instead of merely "being red". ✔MEASURED 2026-08-10 by sabotage: with all
# three floors forced to 0 an earlier arm set stayed fully GREEN, because arms 2
# and 3 red for floor-INDEPENDENT reasons and the substring "below its floor of 0"
# was then satisfied by a DIFFERENT floor's message than the arm claimed to test.
# A red arm that cannot say WHICH mechanism produced the red is not testing it.
function Test-StSilent([string]$arm, [string]$needle) {
    $all = [IO.File]::ReadAllText($stFile)
    if ($all.Contains($needle)) {
        Write-Host "orphan-tests: SELF-TEST FAIL - arm $arm also emitted '$needle', so this arm is NOT isolating the"
        Write-Host "  one mechanism it claims to test and could pass on the strength of another."
        $script:stFail = 1
    }
}
function Test-StCensus([string]$arm, [int]$wSrc, [int]$wCml, [int]$wReg, [int]$wOrp) {
    $cSrc = Get-StNum 'sources'; $cCml = Get-StNum 'CMakeLists'
    $cReg = Get-StNum 'registrations'; $cOrp = Get-StNum 'orphan(s),'
    if ("$cSrc" -ne "$wSrc" -or "$cCml" -ne "$wCml" -or "$cReg" -ne "$wReg" -or "$cOrp" -ne "$wOrp") {
        Write-Host "orphan-tests: SELF-TEST FAIL - arm $arm was not SURGICAL. Got $cSrc/$cCml/$cReg sources/CMakeLists/registrations"
        Write-Host "  and $cOrp orphan(s); wanted $wSrc/$wCml/$wReg and $wOrp. Either the mutant stopped parsing or it"
        Write-Host "  changed more than the one thing this arm claims, and then the red means something else."
        $script:stFail = 1
    }
}
function Test-BytesEqual([string]$a, [string]$b) {
    if (-not (Test-Path -LiteralPath $a -PathType Leaf)) { return $false }
    if (-not (Test-Path -LiteralPath $b -PathType Leaf)) { return $false }
    $x = [IO.File]::ReadAllBytes($a); $y = [IO.File]::ReadAllBytes($b)
    if ($x.Length -ne $y.Length) { return $false }
    for ($i = 0; $i -lt $x.Length; $i++) { if ($x[$i] -ne $y[$i]) { return $false } }
    return $true
}
function Get-Native([string]$root, [string]$rel) {
    return (Join-Path $root ($rel -replace '/', [string][IO.Path]::DirectorySeparatorChar))
}

$tmpBase  = [IO.Path]::GetTempPath()
$stamp    = "$PID-$(Get-Random)"
$mirror   = [IO.Path]::Combine($tmpBase, "orphan-mirror-$stamp")
$pristine = [IO.Path]::Combine($tmpBase, "orphan-pristine-$stamp")
$emptyDir = [IO.Path]::Combine($tmpBase, "orphan-empty-$stamp")
foreach ($d in @($mirror, $pristine, $emptyDir)) { [void](New-Item -ItemType Directory -Path $d -Force) }

# Build the mirror: directories, then every source as an EMPTY file, then the
# CMakeLists byte-for-byte (plus a pristine second copy to restore from and to
# byte-compare against).
# ⓘ $pristine deliberately holds the CMakeLists ONLY, no `*.cpp`. That makes it
# both the restore source AND, at zero extra cost, the perfectly isolated subject
# for the source-floor arm (21 CMakeLists, 228 registrations, 0 sources).
foreach ($rel in $srcList) {
    $p = Get-Native $mirror $rel
    [void](New-Item -ItemType Directory -Path (Split-Path -Parent $p) -Force)
    if (-not (Test-Path -LiteralPath $p)) { [void](New-Item -ItemType File -Path $p) }
}
foreach ($rel in $cmlList) {
    $src = Get-Native $TestsRoot $rel
    foreach ($dst in @((Get-Native $mirror $rel), (Get-Native $pristine $rel))) {
        [void](New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force)
        Copy-Item -LiteralPath $src -Destination $dst -Force
    }
}

# ── ARM 0 — GREEN CONTROL. Without this, every red below is worthless.
Invoke-Guard $mirror
if (Test-StExpect '0 GREEN-CONTROL' 0 ' (mirror is a faithful subject)') {
    Test-StCensus '0 GREEN-CONTROL' $baseSrc $baseCml $baseReg 0
}

# ── ARM 1 — THE ORPHAN. Delete ONE whole single-line registration; the source it
# named is then wired nowhere.
# ★ Witness selection and the uniqueness requirement are the SAME query, so they
# cannot drift apart: if no such registration exists the arm is REFUSED rather
# than run against something ambiguous.
function Get-LiteralCount([string]$s, [string]$needle) {
    $n = 0; $i = 0
    while ($true) {
        $p = $s.IndexOf($needle, $i)
        if ($p -lt 0) { break }
        $n++; $i = $p + $needle.Length
    }
    return $n
}
$wFile = ''; $wLine = 0; $wTok = ''; $wPath = ''
foreach ($rel in $cmlList) {
    if ($wFile -ne '') { break }
    $raw = [IO.File]::ReadAllText((Get-Native $mirror $rel))
    $lines = $raw -split "`n"
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $s = $lines[$i].Trim([char]32, [char]9, [char]13)
        if (-not $s.StartsWith('dss_add_test(')) { continue }
        if (-not $s.EndsWith(')')) { continue }
        if ((Get-LiteralCount $s '(') -ne 1 -or (Get-LiteralCount $s ')') -ne 1) { continue }
        $inner = $s.Substring('dss_add_test('.Length, $s.Length - 'dss_add_test('.Length - 1)
        # ⚠ `$wNumSrc`, NOT `$nsrc`. ★★ POWERSHELL VARIABLE NAMES ARE
        # CASE-INSENSITIVE, so a loop counter called `$nsrc` at SCRIPT scope IS the
        # census variable `$nSrc`. ✔MEASURED 2026-08-10 by running the arms: this
        # loop silently set the source count to 1, and SEVEN arms then failed their
        # census assertion with "wanted 1/21/227". The guard's verdict on the live
        # tree was still correct, so nothing but the self-test could have seen it -
        # which is the whole argument for having one. The `.sh` twin cannot hit this
        # class: awk locals are function-scoped and its names are case-distinct.
        $mode = ''; $src = ''; $wNumSrc = 0
        foreach ($t in ($inner -split ' +')) {
            if ($t -eq '') { continue }
            if ($t -eq 'NAME' -or $t -eq 'SOURCES') { $mode = $t; continue }
            if ($mode -eq 'NAME') { $mode = ''; continue }
            if ($mode -eq 'SOURCES') { $src = $t; $wNumSrc++ }
        }
        if ($wNumSrc -ne 1) { continue }
        if ($src.Contains('/') -or $src.Contains('$')) { continue }
        if ((Get-LiteralCount $raw $src) -ne 1) { continue }
        $d = Get-DirOf $rel
        $wFile = $rel; $wLine = $i + 1; $wTok = $src
        $wPath = if ($d -eq '') { $src } else { "$d/$src" }
        break
    }
}
if ($wFile -eq '') {
    Write-Host "orphan-tests: SELF-TEST FAIL - no witness registration could be selected (it must be a"
    Write-Host "  single-line dss_add_test whose sole source token occurs EXACTLY ONCE in its file)."
    Write-Host "  Without a unique witness the mutation would be ambiguous, so the arm is REFUSED"
    Write-Host "  rather than run weakly."
    $stFail = 1
} else {
    $mFile = Get-Native $mirror $wFile
    $pFile = Get-Native $pristine $wFile
    # Rejoin with "`n": these files are LF and a Set-Content round trip would
    # rewrite EVERY line to CRLF, which is not a surgical mutation and would also
    # make the byte-restore check meaningless.
    $keep = @([IO.File]::ReadAllText($pFile) -split "`n" | Select-Object -Skip 0)
    $out = [System.Collections.ArrayList]::new()
    for ($i = 0; $i -lt $keep.Count; $i++) { if ($i -ne ($wLine - 1)) { [void]$out.Add($keep[$i]) } }
    [IO.File]::WriteAllText($mFile, ($out -join "`n"))
    if (Test-BytesEqual $pFile $mFile) {
        Write-Host "orphan-tests: SELF-TEST FAIL - the mutation changed NO bytes of the witness file. An arm"
        Write-Host "  that did not mutate anything cannot prove a red."
        $stFail = 1
    } else {
        Invoke-Guard $mirror
        if (Test-StExpect '1 ORPHAN' 1 " (witness $wPath)") {
            Test-StSays   '1 ORPHAN' "  ORPHAN: $wPath"
            Test-StCensus '1 ORPHAN' $baseSrc $baseCml ($baseReg - 1) 1
        }
        # ── ARM 1b — A COMMENT IS NOT COVERAGE. ★★ ADDED BECAUSE IT WAS MEASURED
        # MISSING: sabotaging the parser's quote-aware comment stripping left the
        # whole arm set GREEN, i.e. the one mechanism separating 228 real
        # registrations from 240 textual mentions was completely unexercised. This
        # arm re-adds the deleted registration AS A COMMENT and requires the guard
        # to still call the witness an orphan. (The phantom-registration check
        # catches the same sabotage by an independent route; two mechanisms,
        # deliberately.)
        $wStem = $wTok.Substring(0, $wTok.Length - 4)
        $wDir  = Get-DirOf $wPath
        [IO.File]::AppendAllText($mFile, "# dss_add_test(NAME $wDir/$wStem SOURCES $wTok)`n")
        Invoke-Guard $mirror
        if (Test-StExpect '1b COMMENT-IS-NOT-COVERAGE' 1 ' (witness still orphaned)') {
            Test-StSays   '1b COMMENT-IS-NOT-COVERAGE' "  ORPHAN: $wPath"
            Test-StCensus '1b COMMENT-IS-NOT-COVERAGE' $baseSrc $baseCml ($baseReg - 1) 1
        }
    }
    Copy-Item -LiteralPath $pFile -Destination $mFile -Force
    if (-not (Test-BytesEqual $pFile $mFile)) {
        Write-Host "orphan-tests: SELF-TEST FAIL - restoring the witness file did not reproduce the pristine bytes; later arms would run against arm 1's mutant."
        $stFail = 1
    }
}

# ── ARM 2 / 3 — COLLAPSE, the two whole-root cases. Both red for reasons
# INDEPENDENT of the three floors, which is why they are not sufficient on their
# own and arms 4-6 exist.
Invoke-Guard $emptyDir
if (Test-StExpect '2 COLLAPSE-EMPTY-ROOT' 2 '') {
    Test-StSays '2 COLLAPSE-EMPTY-ROOT' 'found 0 CMakeLists.txt under the tests root'
}
Invoke-Guard ([IO.Path]::Combine($emptyDir, 'definitely-not-here'))
if (Test-StExpect '3 COLLAPSE-MISSING-ROOT' 2 '') {
    Test-StSays '3 COLLAPSE-MISSING-ROOT' 'the tests root does not exist'
}

# ── ARM 4 — SOURCE floor. Subject: the CMakeLists-only pristine tree.
Invoke-Guard $pristine
if (Test-StExpect '4 COLLAPSE-SOURCE-FLOOR' 2 '') {
    Test-StSays   '4 COLLAPSE-SOURCE-FLOOR' 'the source scan found only 0 *.cpp'
    Test-StSilent '4 COLLAPSE-SOURCE-FLOOR' 'the CMakeLists scan found only'
    Test-StSilent '4 COLLAPSE-SOURCE-FLOOR' 'the parser resolved only'
}

# ── ARM 5 — CMAKELISTS floor. Rename all but two CMakeLists out of the way so the
# enumeration falls below its floor while all sources stay in place. A rename
# rather than a move: nothing has to be re-created and the restore is the same
# rename backwards.
$keepN = 2
$stashed = [System.Collections.ArrayList]::new()
for ($i = $keepN; $i -lt $cmlList.Count; $i++) {
    $p = Get-Native $mirror $cmlList[$i]
    Move-Item -LiteralPath $p -Destination "$p.stashed" -Force
    [void]$stashed.Add($cmlList[$i])
}
Invoke-Guard $mirror
if (Test-StExpect '5 COLLAPSE-CMAKELISTS-FLOOR' 2 '') {
    Test-StSays   '5 COLLAPSE-CMAKELISTS-FLOOR' "the CMakeLists scan found only $keepN files"
    Test-StSilent '5 COLLAPSE-CMAKELISTS-FLOOR' 'the source scan found only'
}
foreach ($rel in $stashed) { $p = Get-Native $mirror $rel; Move-Item -LiteralPath "$p.stashed" -Destination $p -Force }

# ── ARM 6 — REGISTRATION floor: THE DIMENSION THAT FAILS QUIETEST. Both
# enumerations stay healthy while the PARSE stops resolving enough registrations.
# The densest CMakeLists are selected from the guard's OWN parse output rather than
# by guessing which files are big.
$dense = @($parsed.Regs | Group-Object -Property File |
           Sort-Object -Property Count -Descending)
$stashed = [System.Collections.ArrayList]::new()
$left = $nReg
$cmlLeft = $nCml
foreach ($g in $dense) {
    if ($left -lt $RegistrationFloor) { break }
    if (($cmlLeft - 1) -lt $CMakeListsFloor) { break }
    $p = Get-Native $mirror $g.Name
    Move-Item -LiteralPath $p -Destination "$p.stashed" -Force
    [void]$stashed.Add($g.Name); $left -= $g.Count; $cmlLeft--
}
if ($left -ge $RegistrationFloor) {
    Write-Host "orphan-tests: SELF-TEST FAIL - arm 6 could not drive the registration count below its floor"
    Write-Host "  ($left left, floor $RegistrationFloor) while keeping >= $CMakeListsFloor CMakeLists. The arm is REFUSED"
    Write-Host "  rather than run in a shape that proves something else."
    $stFail = 1
} else {
    Invoke-Guard $mirror
    if (Test-StExpect '6 COLLAPSE-REGISTRATION-FLOOR' 2 '') {
        Test-StSays   '6 COLLAPSE-REGISTRATION-FLOOR' 'the parser resolved only'
        Test-StSilent '6 COLLAPSE-REGISTRATION-FLOOR' 'the source scan found only'
        Test-StSilent '6 COLLAPSE-REGISTRATION-FLOOR' 'the CMakeLists scan found only'
    }
}
foreach ($rel in $stashed) { $p = Get-Native $mirror $rel; Move-Item -LiteralPath "$p.stashed" -Destination $p -Force }
# Prove the stash/unstash round trip left the subject byte-identical. Without this
# every arm after 6 would be running against a tree that merely LOOKS restored.
foreach ($rel in $cmlList) {
    if (-not (Test-BytesEqual (Get-Native $pristine $rel) (Get-Native $mirror $rel))) {
        Write-Host "orphan-tests: SELF-TEST FAIL - after the floor arms, a mirror CMakeLists no longer matches its pristine copy; later arms would run against a mutated subject."
        $stFail = 1
        break
    }
}

# The first allowlist row drives arms 7-9. Everything about them is derived from
# the row, so reordering or replacing the allowlist cannot leave a stale literal
# behind in the self-test.
$a1 = $allowRows[0] -split '\|'
$a1Path = $a1[0]; $a1Site = $a1[1]
$a1Base = $a1Path.Substring($a1Path.LastIndexOf('/') + 1)
$a1Dir  = Get-DirOf $a1Path
$a1Stem = $a1Base.Substring(0, $a1Base.Length - 4)

# ── ARM 7 — STALE ALLOWLIST: the named file is gone.
Remove-Item -LiteralPath (Get-Native $mirror $a1Path) -Force
Invoke-Guard $mirror
if (Test-StExpect '7 STALE-ALLOW-FILE-GONE' 3 '') {
    Test-StSays   '7 STALE-ALLOW-FILE-GONE' 'does NOT exist under the tests root'
    Test-StCensus '7 STALE-ALLOW-FILE-GONE' ($baseSrc - 1) $baseCml $baseReg 0
}
[void](New-Item -ItemType File -Path (Get-Native $mirror $a1Path))

# ── ARM 8 — STALE ALLOWLIST: the file is NOW registered, so the exemption is a
# lie. The line added is an ordinary-looking registration; nothing in it names the
# mutation, so the guard cannot key off a marker instead of the real property.
$a1SiteNative = Get-Native $mirror $a1Site
[IO.File]::AppendAllText($a1SiteNative, "dss_add_test(NAME $a1Dir/$a1Stem SOURCES $a1Base)`n")
Invoke-Guard $mirror
if (Test-StExpect '8 STALE-ALLOW-NOW-REGISTERED' 3 '') {
    Test-StSays   '8 STALE-ALLOW-NOW-REGISTERED' 'dss_add_test now REGISTERS it'
    Test-StCensus '8 STALE-ALLOW-NOW-REGISTERED' $baseSrc $baseCml ($baseReg + 1) 0
}
Copy-Item -LiteralPath (Get-Native $pristine $a1Site) -Destination $a1SiteNative -Force
if (-not (Test-BytesEqual (Get-Native $pristine $a1Site) $a1SiteNative)) {
    Write-Host "orphan-tests: SELF-TEST FAIL - restoring the allowlist reference site after arm 8 did not reproduce the pristine bytes."
    $stFail = 1
}

# ── ARM 9 — STALE ALLOWLIST: the reference site stops referencing the file. THIS
# is the arm that makes the allowlist REASON a checked claim rather than prose.
$siteLines = @([IO.File]::ReadAllText((Get-Native $pristine $a1Site)) -split "`n" |
               Where-Object { -not $_.Contains($a1Base) })
[IO.File]::WriteAllText($a1SiteNative, ($siteLines -join "`n"))
if (Test-BytesEqual (Get-Native $pristine $a1Site) $a1SiteNative) {
    Write-Host "orphan-tests: SELF-TEST FAIL - arm 9 removed no line, so the reference site still references"
    Write-Host "  the allowlisted source and the arm would pass for the wrong reason."
    $stFail = 1
} else {
    Invoke-Guard $mirror
    if (Test-StExpect '9 STALE-ALLOW-SITE-DROPPED-REFERENCE' 3 '') {
        Test-StSays   '9 STALE-ALLOW-SITE-DROPPED-REFERENCE' 'no longer STRUCTURALLY references'
        Test-StCensus '9 STALE-ALLOW-SITE-DROPPED-REFERENCE' $baseSrc $baseCml $baseReg 0
    }
}
Copy-Item -LiteralPath (Get-Native $pristine $a1Site) -Destination $a1SiteNative -Force

# ── ARM 10 — GREEN AFTER RESTORE. Every mutation undone, the subject is back.
Invoke-Guard $mirror
if (Test-StExpect '10 GREEN-AFTER-RESTORE' 0 ' (all mutations undone)') {
    Test-StCensus '10 GREEN-AFTER-RESTORE' $baseSrc $baseCml $baseReg 0
}

foreach ($d in @($mirror, $pristine, $emptyDir)) {
    if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
}
if (Test-Path -LiteralPath $stFile) { Remove-Item -LiteralPath $stFile -Force }

if ($stFail -ne 0) {
    Write-Host "orphan-tests: SELF-TEST FAILED. Treat the verdict above as UNPROVEN: this guard has not"
    Write-Host "  demonstrated that it can fail, which is the only thing that makes a green mean"
    Write-Host "  anything. Fix the guard before trusting its census."
    exit 2
}
Write-Host "orphan-tests: self-test OK - 12 arms exercised (2 green controls, orphan, comment-is-not-coverage, 2 whole-root collapse, 3 per-floor collapse, 3 stale-allowlist); this guard is PROVEN able to fail."
exit $finalRc
