#!/usr/bin/env pwsh
# check-line-endings.ps1 — Windows variant of the tracked-blob line-ending guard.
# Mirrors `check-line-endings.sh`; same contract, same floor, same exit codes.
#
# Contract: NO tracked TEXT blob in this repository may contain a line-terminating
# CR — not in HEAD, and not staged in the index. A file that genuinely needs its
# 0x0D bytes preserved declares itself `binary` in `.gitattributes`, which this
# guard honours through `git grep -I`.
#
# ★ PAIRING NOTE (D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED). The anchor-registry pair
# drifted because each script REIMPLEMENTED the scan in its own shell's idioms —
# one `grep -r`, one `Get-ChildItem -Recurse` — and the two disagreed by 4
# anchors for months. This pair deliberately does NOT do that: every measurement
# below is made by the SAME `git` subcommands with the SAME arguments as the .sh
# sibling, so the shells only marshal arguments and compare strings. That does
# not make the pairing enforced — nothing checks it — but it removes the
# mechanism by which the other pair diverged.
#
# ┌─ CR-INSTRUMENT-QUOTED:BEGIN ─ this block QUOTES the blind idioms to explain
# │  them; it does not run one. Check F honours the same region marker from any
# │  file, and this twin uses it rather than being exempted by path.
# ⚠⚠ THE COMMON CR INSTRUMENTS ARE BLIND ON THIS HOST, IN BOTH DIRECTIONS, and
# an earlier version of the .sh sibling's note GOT THE REASON WRONG — it said
# `grep -c $'\r'` "matches the LETTER `r`", refuted by a control containing no
# `r` at all. ✔RE-MEASURED 2026-08-27 (cycle P42) on a `printf 'a\r\nb\n'`
# control verified by `od -c` to hold exactly one CR:
#   TRAP 1, FALSE POSITIVE — the literal `$'\r'` written INSIDE a command
#     substitution expands to the EMPTY STRING, so `n=$(grep -c $'\r' f)` runs
#     `grep -c ''` and returns the LINE COUNT: 2 for the CRLF control and 2 for
#     its pure-LF twin. CR-specific and parse-time (a `$'\t'` in the identical
#     position survives; a CR held in a VARIABLE survives).
#   TRAP 2, FALSE NEGATIVE, the dangerous one — GNU grep and sed open files in
#     TEXT MODE and strip the trailing CR BEFORE matching. On the CRLF control
#     `grep -c "$CR"` -> 0 while `grep -U -c "$CR"` -> 1; `awk '/\r$/'` -> 0;
#     `sed -n '/\r/p'` -> 0; `tr -dc '\r' | wc -c` -> 1 (the only correct one).
#     `-a` does NOT help. A MID-LINE CR is found by all of them — the blindness
#     is aimed exactly at the CR anyone hunts.
# ★★ WHY IT SURVIVED, and why THIS twin is held to its own control rather than
# assumed to inherit the sibling's: under WSL/Linux all three instruments are
# CORRECT. An idiom sanity-checked on the wrong leg is verified nowhere. This
# file runs on Windows, which is precisely where they lie.
# ⓘ .NET is NOT affected by trap 2 — `[IO.File]::ReadAllBytes` reads bytes — but
# `Get-Content` applies its own line splitting, so the byte reader is used below.
# └─ CR-INSTRUMENT-QUOTED:END ─────────────────────────────────────────────────
#
# ⚠ INSTRUMENT NOTE specific to THIS shell: `Set-StrictMode` + a native command
# exiting non-zero is a normal, expected outcome here (`git grep` exits 1 for
# "no match", which is this guard's PASS). PowerShell 7.4 turns native stderr
# into a terminating error when `$PSNativeCommandUseErrorActionPreference` is
# on, so it is explicitly disabled below — otherwise the clean case would throw.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot  = Split-Path -Parent (Split-Path -Parent $ScriptDir)
# ⚠ CAPTURED BEFORE Set-Location, for the same reason the .sh sibling captures
# INVOKED_FROM: `--files` takes paths from a caller standing somewhere else,
# very often outside this repo, and resolving them against the repo root
# silently answers about the wrong file.
$InvokedFrom = (Get-Location).Path

# ── THE ONE ROOT ──────────────────────────────────────────────────────────
# ⛔⛔ D-SCRIPT-GUARDS-ASK-GIT-FROM-THE-LANE-WORKTREE. Until 2026-09-01 this file
# said `Set-Location $RepoRoot` and then read repo-relative paths git had handed
# it with `[IO.File]::ReadAllBytes`. `Set-Location` moves the PowerShell provider
# location -- which native children (git) inherit -- and does NOT move
# `[Environment]::CurrentDirectory`, which is what every `[IO.File]` relative
# path follows. ✔MEASURED 2026-09-01 (cycle P51, lane `gw`): this guard,
# ENUMERATING from a lane worktree while READING at the main checkout, listed
# `scripts/check-line-endings/GW-CR-PROBE.md` (present only in the lane) and died
# on `[IO.File]::ReadAllBytes` at the OTHER root.
# ★★ IT DIED ONLY BECAUSE THE FILE WAS NEW. For every path both roots hold --
# which is all of them, for a lane that only EDITS files -- it read the wrong
# tree's bytes and reported a verdict about it. A CR introduced in a lane was
# invisible; a CR fixed in a lane still convicted. That is a wrong-root read that
# FAILS TOWARD GREEN.
# ⇒ the root, the git namespace and the read root are now settled ONCE by
# `Enter-RepoTree`, which moves BOTH working directories and then PROVES they and
# git agree. `$RepoTree` is that identity; nothing below asks git or resolves a
# path any other way.
. (Join-Path $RepoRoot 'scripts/repo-tree/repo-tree.ps1')
$RepoTree = $null

function Get-GitLines([string[]]$GitArgs) {
    # `git grep` exits 1 for "no match", which is a legitimate answer here, so
    # only the LINES are returned; callers that need to distinguish "empty"
    # from "broken" use the positive control below, never an exit code.
    # ⚠ DEFINED HERE, above the argument dispatch, because `--audit-instruments`
    # calls it: in PowerShell a function must exist when it is CALLED, and the
    # dispatch runs before the main body.
    # ⚠ `$RepoTree` is read at CALL time, so every caller is downstream of the
    # `Enter-RepoTree` that set it. A call before that is a bug and must not be
    # rescued by falling back to a bare `git` -- that fallback IS the defect.
    if ($null -eq $RepoTree) {
        Write-Error "line-endings: FAIL - a git query was made before the guard entered its tree. Refusing to answer from an unidentified root."
        exit 2
    }
    # ⚠ PLAIN `@(...)`. A caller that needs `.Count` writes `@(...)` at ITS call
    # site -- the idiom this file already uses everywhere else. See the measured
    # note on `Invoke-RepoTreeGit` for why the `,@(...)` shortcut is worse: it
    # reaches a pipeline as one object and silently disables the next filter.
    return @(Invoke-RepoTreeGit $RepoTree $GitArgs)
}

# ── THE ONE CORRECT INSTRUMENT ────────────────────────────────────────────
# Bytes, never lines: `Get-Content` would split on the line ending and hide the
# very byte in question, which is trap 2 wearing a PowerShell hat.
function Get-CrCount([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $n = 0
    foreach ($b in $bytes) { if ($b -eq 13) { $n++ } }
    return $n
}

# ── WHAT CHECK F REFUSES — the SAME ERE pair the .sh sibling uses ──────────
# Both twins hand these identical strings to the identical `git grep`, so the
# shells only marshal arguments; the pairing note at the top of this file
# explains why that is the one drift-resistant arrangement available here.
$CrVerb   = '(grep|egrep|fgrep|rg|awk|sed|findstr|Select-String)'
# ⚠ IN A SINGLE-QUOTED POWERSHELL STRING A LITERAL `'` IS WRITTEN `''`, so the
# character class `['"/]` must be typed `[''"/]`. An earlier draft here typed
# `['']"/]`, which yields `[']"/]` — a class of just `'`, followed by three
# LITERAL characters. ✔The self-test caught it (1 of 3 blind forms detected)
# after `--audit-instruments` had already reported a cheerful OK over the whole
# tree with it. That false green is precisely what the arms exist to refuse.
$CrPat    = '(\$''(\\r|\\015)''|[''"/]\\r\$|[''"/]\\r[''"/])'
$CrBlind  = "($CrVerb.*$CrPat|$CrPat.*$CrVerb)"
$CrExempt = '(git +(grep|ls-files|diff|cat-file)|-U |--binary|sub\(|gsub\(|s/\\r|-replace|tr +-d|%\$''|CR-INSTRUMENT-QUOTED)'
$CrAuditScope = ':(exclude).plans/'

function Test-InQuotedRegion([string]$Path, [int]$LineNo) {
    # A BEGIN/END region marks a whole documentation block at once. Inclusive of
    # both marker lines, matching the .sh sibling's awk exactly.
    # ⛔ `$Path` arrives repo-relative from `git grep -n`, so it is rooted at the
    # tree git enumerated it from before either resolver sees it -- otherwise
    # `Test-Path` answers about one tree and `[IO.File]::ReadAllLines` reads the
    # other, and the exemption would be granted or refused on the wrong file.
    $Path = Resolve-RepoTreePath $RepoTree $Path
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $inRegion = $false; $n = 0
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        $n++
        if ($line -match 'CR-INSTRUMENT-QUOTED:BEGIN') { $inRegion = $true }
        if ($n -eq $LineNo) { return $inRegion }
        if ($line -match 'CR-INSTRUMENT-QUOTED:END')   { $inRegion = $false }
    }
    return $false
}

function Invoke-FilesMode([string[]]$Paths) {
    $n = 0; $bad = 0; $unmeasured = 0
    foreach ($p in $Paths) {
        $n++
        $abs = if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $InvokedFrom $p }
        if (Test-Path -LiteralPath $abs -PathType Container) {
            Write-Host "  UNMEASURED  $p - is a directory, not a file"; $unmeasured++; continue
        }
        if (-not (Test-Path -LiteralPath $abs -PathType Leaf)) {
            Write-Host "  UNMEASURED  $p - no such file"; $unmeasured++; continue
        }
        try { $cr = Get-CrCount $abs }
        catch { Write-Host "  UNMEASURED  $p - not readable"; $unmeasured++; continue }
        if ($cr -eq 0) { Write-Host "  LF          $p" }
        else { Write-Host "  CR  $cr    $p"; $bad++ }
    }
    if ($n -eq 0) {
        Write-Host "line-endings: FAIL - --files was given no paths. Refusing to report a"
        Write-Host "  pass over an empty list; that is a vacuous green, not a clean tree."
        return 2
    }
    $rc = 0
    if ($unmeasured -gt 0) {
        Write-Host "line-endings: FAIL - $unmeasured of $n path(s) could NOT be measured (above)."
        Write-Host "  A guard that cannot read a file must say so, never imply it was clean."
        $rc = 2
    }
    if ($bad -gt 0) {
        Write-Host "line-endings: FAIL - $bad of $n file(s) carry a CR."
        if ($rc -eq 0) { $rc = 1 }
    }
    if ($rc -eq 0) { Write-Host "line-endings: OK ($n file(s), none carries a CR; measured as bytes)" }
    return $rc
}

function Invoke-InstrumentAudit {
    $raw = Get-GitLines @('grep','-n','-I','-E',$CrBlind,'--','.',$CrAuditScope) |
           Where-Object { $_ -notmatch $CrExempt }
    $hits = @()
    foreach ($h in $raw) {
        $parts = $h -split ':', 3
        if ($parts.Count -lt 3) { continue }
        if (Test-InQuotedRegion $parts[0] ([int]$parts[1])) { continue }
        $hits += $h
    }
    $marked = (Get-GitLines @('grep','-c','-I','-e','CR-INSTRUMENT-QUOTED','--','.',$CrAuditScope)).Count
    if ($hits.Count -gt 0) {
        Write-Host "line-endings: FAIL - a CR instrument that cannot see a CR:"
        Write-Host ""
        foreach ($h in $hits) { Write-Host "  $h" }
        Write-Host ""
        # CR-INSTRUMENT-QUOTED:BEGIN - the refusal must SHOW what it refuses.
        Write-Host "These spellings do not measure what they appear to measure on this host:"
        Write-Host "  * ``grep -c `$'\r'``  returns the LINE COUNT - of a CLEAN file too;"
        Write-Host "  * ``awk '/\r`$/'``, ``sed -n '/\r/p'``, ``grep -P '\r`$'`` return 0 over a"
        Write-Host "    file that is entirely CRLF, because the reader strips the CR first."
        Write-Host "Use instead:"
        Write-Host "  (a) scripts/check-line-endings/check-line-endings.ps1 --files PATH..."
        Write-Host "  (b) ``tr -dc '\r' < f | wc -c``  (expect 0) if you must inline it; or"
        Write-Host "  (c) ``git grep``/``git ls-files --eol``, which read blobs and are unaffected."
        Write-Host "If the line is DOCUMENTATION that quotes the idiom on purpose, put the"
        Write-Host "marker CR-INSTRUMENT-QUOTED on it (or wrap the block in :BEGIN/:END)."
        # CR-INSTRUMENT-QUOTED:END
        return 1
    }
    Write-Host "line-endings: Check F OK (no blind CR instrument; $marked file(s) carry the quoted-idiom marker)"
    return 0
}

function Invoke-SelfTest {
    # ★★ SYNTHESIZES THE NEGATIVE. Every arm is built to fail if the instrument
    # is blind: the control is written with explicit bytes and verified to hold
    # exactly one 0x0D before it is used to judge anything.
    # ⚠ The synthetic blind lines are ASSEMBLED AT RUN TIME, never written
    # literally, or Check F would (correctly) refuse this very file.
    $t = Join-Path ([IO.Path]::GetTempPath()) ("dss-le-" + [Guid]::NewGuid().ToString('N'))
    $tr = (New-Item -ItemType Directory -Path $t).FullName
    if ($tr.StartsWith($RepoRoot, [StringComparison]::OrdinalIgnoreCase)) {
        Write-Host "line-endings: FAIL - selftest temp dir '$tr' is inside the repo"
        return 2
    }
    $fail = 0
    try {
        $crlf = Join-Path $tr 'ctl_crlf.txt'; $lf = Join-Path $tr 'ctl_lf.txt'
        [IO.File]::WriteAllBytes($crlf, [byte[]]@(97,13,10,98,10))   # a CR LF b LF
        [IO.File]::WriteAllBytes($lf,   [byte[]]@(97,10,98,10))      # a LF b LF
        # ARM 0 - prove the CONTROL before trusting a verdict taken with it.
        # ⚠ `@(...)` around every pipeline result: `Set-StrictMode -Version
        # Latest` makes `.Count` on a SCALAR a terminating error, so a one-CR
        # control would have killed the guard instead of measuring it.
        $n = @([IO.File]::ReadAllBytes($crlf) | Where-Object { $_ -eq 13 }).Count
        if ($n -ne 1) { Write-Host "line-endings: FAIL - selftest control is not one CR (got $n)"; $fail = 1 }
        # ARM 1 - THE NEGATIVE: the counter must SEE the CR.
        if ((Get-CrCount $crlf) -ne 1) { Write-Host "line-endings: FAIL - selftest: Get-CrCount reported no CR on the CRLF control. The instrument is blind."; $fail = 1 }
        # ARM 2 - and must not invent one.
        if ((Get-CrCount $lf) -ne 0) { Write-Host "line-endings: FAIL - selftest: Get-CrCount invented a CR on the pure-LF control."; $fail = 1 }
        # ARM 3/4 - --files must RED on the dirty control and pass on the clean one.
        if ((Invoke-FilesMode @($crlf) 6>$null) -eq 0) { Write-Host "line-endings: FAIL - selftest: --files reported success over a file holding a CR."; $fail = 1 }
        if ((Invoke-FilesMode @($lf)   6>$null) -ne 0) { Write-Host "line-endings: FAIL - selftest: --files refused a pure-LF file."; $fail = 1 }
        # ARM 5 - a missing path is UNMEASURED (2), never a quiet pass.
        if ((Invoke-FilesMode @((Join-Path $tr 'nope.txt')) 6>$null) -ne 2) { Write-Host "line-endings: FAIL - selftest: a missing path did not exit 2."; $fail = 1 }
        # ARM 6 - the detector must FIRE on blind lines, assembled from fragments.
        $q = [char]39; $d = '$'; $b = '\'
        $blind = @(
            "n=$d(grep -c $d$q${b}r$q f)",
            "awk $q/${b}r$d/ {bad++}$q f",
            "sed -n $q/${b}r/p$q f | wc -l"
        )
        $fired = @($blind | Where-Object { $_ -match $CrBlind }).Count
        if ($fired -ne 3) { Write-Host "line-endings: FAIL - selftest: Check F saw $fired/3 blind instruments. The detector is broken."; $fail = 1 }
        # ...and must NOT fire on the measured-safe forms.
        $safe = @(
            "tr -dc $q${b}r$q < f | wc -c",
            "git grep -I -l -P $q${b}r$d$q HEAD",
            "sub(/${b}r$d/, `"`", line)",
            "grep -U -c `"${d}CR`" f"
        )
        $bogus = @($safe | Where-Object { ($_ -match $CrBlind) -and ($_ -notmatch $CrExempt) }).Count
        if ($bogus -ne 0) { Write-Host "line-endings: FAIL - selftest: Check F fired on $bogus SAFE form(s)."; $fail = 1 }
        # ARM 7 - the marker must exempt, or nobody can document the idiom.
        $marked = "awk $q/${b}r$d/$q f   CR-INSTRUMENT-QUOTED"
        if (($marked -match $CrBlind) -and ($marked -notmatch $CrExempt)) { Write-Host "line-endings: FAIL - selftest: the CR-INSTRUMENT-QUOTED marker did not exempt a documented idiom."; $fail = 1 }
        # ARM 8 - ONE ROOT (D-SCRIPT-GUARDS-ASK-GIT-FROM-THE-LANE-WORKTREE).
        # Every arm above judges BYTES; this one judges WHICH TREE the bytes came
        # from, which is the question checks C/D/E answered wrongly for a lane.
        # The expensive proof -- a real `git worktree add` fixture, a child process
        # standing in the other root, and a control that reproduces the wrong-root
        # read -- lives with the owner (`repo-tree.ps1 --selftest`, ctest entry
        # `repo_tree_guard`). What belongs HERE is that THIS guard is actually
        # standing on it.
        if ($null -eq $RepoTree) {
            Write-Host "line-endings: FAIL - selftest: the guard reached its self-test without entering a tree; every verdict below would be about an unidentified root."
            $fail = 1
        } else {
            try { Assert-RepoTreeOneRoot $RepoTree }
            catch { Write-Host "line-endings: FAIL - selftest: $($_.Exception.Message)"; $fail = 1 }
            # ...and a repo-relative path from git must land under THAT root, not
            # under whatever directory this process happened to start in.
            $probeRel = 'scripts/check-line-endings/check-line-endings.ps1'
            $probeAbs = Resolve-RepoTreePath $RepoTree $probeRel
            if ((Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $probeAbs))) -ne $RepoTree.Root) {
                Write-Host "line-endings: FAIL - selftest: a repo-relative path resolved to '$probeAbs', which is not under the enumerated root '$($RepoTree.Root)'."
                $fail = 1
            }
            if (-not (Test-Path -LiteralPath $probeAbs -PathType Leaf)) {
                Write-Host "line-endings: FAIL - selftest: the resolved path '$probeAbs' does not exist; the guard is rooted somewhere it cannot read."
                $fail = 1
            }
        }
    }
    finally { Remove-Item -Recurse -Force -LiteralPath $tr -ErrorAction SilentlyContinue }
    if ($fail -ne 0) {
        Write-Host "line-endings: FAIL - the SELF-TEST failed (above). This guard cannot be trusted until it passes; do not silence it."
        return 2
    }
    return 0
}

# CR-INSTRUMENT-QUOTED:BEGIN - the help text NAMES the blind idiom.
function Show-Usage {
    Write-Host @'
check-line-endings.ps1 - the LF-contract guard, and the repo's CR instrument.

  (no arguments)      Verify the whole repository (checks A-E) plus Check F,
                      which refuses a CR instrument that cannot see a CR.
                      Self-tests first, so it cannot pass without proving it
                      can fail.
  --files PATH...     Ask about SPECIFIC files: "are these clean?" Works on
                      tracked, untracked and outside-the-repo paths alike.
                      Exit 0 all clean - 1 a CR was found - 2 unreadable.
  --files-from FILE   The same, one path per line ('-' reads stdin).
  --audit-instruments Run Check F alone.
  --selftest          Run the self-test alone.
  --help              This text.

WHY --files EXISTS: until 2026-08-27 this guard answered exactly one question,
"is the whole repo clean?", and took no arguments. A lane holding thirteen
specific files had NO entry point, so it hand-rolled an awk CR test that
reports a clean tree over a fully CRLF file, and certified all thirteen while
measuring nothing. The guard was not missing; its REACH was.
'@
}
# CR-INSTRUMENT-QUOTED:END

if ($args.Count -gt 0) {
    switch ($args[0]) {
        { $_ -in '--help','-h' } { Show-Usage; exit 0 }
        '--files'   {
            # ⚠ NOT `$args[1..($args.Count-1)]`: with no paths that range is
            # `1..0`, which PowerShell walks BACKWARDS and returns element 0 —
            # so `--files` alone would have measured the string "--files".
            $paths = if ($args.Count -ge 2) { @($args[1..($args.Count-1)]) } else { @() }
            exit (Invoke-FilesMode $paths)
        }
        '--files-from' {
            if ($args.Count -lt 2) { Write-Host "line-endings: FAIL - --files-from needs a FILE (or -)"; exit 2 }
            $src = $args[1]
            $lines = if ($src -eq '-') { @($input) } elseif (Test-Path -LiteralPath $src -PathType Leaf) { [IO.File]::ReadAllLines($src) } else {
                Write-Host "line-endings: FAIL - cannot read path list '$src'"; exit 2 }
            exit (Invoke-FilesMode @($lines | Where-Object { $_ -ne '' }))
        }
        '--audit-instruments' { $RepoTree = Enter-RepoTree $RepoRoot; exit (Invoke-InstrumentAudit) }
        '--selftest'          { $RepoTree = Enter-RepoTree $RepoRoot; exit (Invoke-SelfTest) }
        default { Write-Host "line-endings: FAIL - unknown argument '$($args[0])' (see --help)"; exit 2 }
    }
}

# ── fail-closed preconditions ─────────────────────────────────────────────
# A guard that cannot run must FAIL, never skip.
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Error "line-endings: FAIL - git is not on PATH. This guard reads BLOBS, so it cannot fall back to the working tree (a CRLF checkout would false-red and an LF checkout would false-green). Refusing to report a pass."
    exit 2
}
# ★ `Enter-RepoTree` IS the precondition: it refuses when git cannot describe the
# tree AND when the enumeration root and the read root disagree. The old code
# tested only the first with a bare `git rev-parse HEAD`, which is exactly the
# check that passes while every read lands somewhere else.
try {
    $RepoTree = Enter-RepoTree $RepoRoot
} catch {
    Write-Error "line-endings: FAIL - $($_.Exception.Message) Refusing to report a pass over a tree it cannot read."
    exit 2
}
$null = Get-GitLines @('rev-parse','--verify','--quiet','HEAD')
if ([string]::IsNullOrWhiteSpace($RepoTree.Sha)) {
    Write-Error "line-endings: FAIL - HEAD does not resolve; this is not a git work tree with a commit. Refusing to report a pass over a tree it cannot read."
    exit 2
}

# ── SELF-TEST FIRST - this entry cannot pass without proving it can fail ──
# The same arrangement `orphan_tests_guard` uses: a guard wired into ctest is
# only evidence if it is still capable of redding
# (D-TEST-NONFATAL-GUARD-DEGRADES-TO-A-VACUOUS-PASS).
if ((Invoke-SelfTest) -ne 0) { exit 2 }

# ── POSITIVE CONTROL ──────────────────────────────────────────────────────
# The offender scan PASSES by returning nothing, which is exactly what a BROKEN
# scan also returns. Prove the instrument answers first: same engine, same ref,
# a pattern every non-empty text blob matches. MEASURED at the commit that added
# this: 2214 blobs in HEAD, 2214 in the index, over 2238 tracked paths. The
# floor catches COLLAPSE, not drift. Fix the scan; never lower the floor.
$ScanFloor    = 1500
$ControlHead  = (Get-GitLines @('grep','-I','-l','-P','^.','HEAD')).Count
$ControlIndex = (Get-GitLines @('grep','--cached','-I','-l','-P','^.')).Count

$controlFailed = $false
foreach ($pair in @(@('HEAD', $ControlHead), @('index', $ControlIndex))) {
    if ($pair[1] -lt $ScanFloor) {
        Write-Host "line-endings: FAIL - the $($pair[0]) scan saw only $($pair[1]) text blobs, below its floor of $ScanFloor."
        Write-Host "  This does NOT mean the tree is clean - it means the SCAN collapsed"
        Write-Host "  (git built without PCRE for -P, an unresolvable ref, or a moved tree)."
        Write-Host "  A guard that reports success over what it could not read is the exact"
        Write-Host "  failure class this repository keeps anchoring. Refusing to pass."
        $controlFailed = $true
    }
}
if ($controlFailed) { exit 2 }

# ── the check ─────────────────────────────────────────────────────────────
# `-I` skips blobs git detects as BINARY, which is correct (a `.bin` #embed
# fixture legitimately carries 0x0D) but leaves one blind spot: a TEXT source
# git happens to detect as binary would be skipped silently. Check C closes
# exactly that hole for the extensions the pin claims to cover.
#
# ★ THERE IS DELIBERATELY NO `eol=crlf` ESCAPE HATCH, and the reason is a
# MEASURED property of git rather than a policy choice. Probed 2026-08-06: a
# file declared `text eol=crlf`, written CRLF on disk and staged, lands in the
# index as `i/lf` with ZERO CR in the blob — `eol=` governs the SMUDGE
# (checkout) direction only. The REAL exemption is `binary` / `-text`, already
# honoured through `-I`. Kept identical to the .sh sibling.
# CR-INSTRUMENT-QUOTED:BEGIN - these two ARE `git grep` (the array's first
# element is the subcommand), which reads BLOBS and is unaffected by the text
# mode that blinds a plain grep. Check F cannot see the `git` through the array
# form, so the region says so explicitly rather than the guard exempting itself.
$OffendersHead  = Get-GitLines @('grep','-I','-l','-P','\r$','HEAD') |
                  ForEach-Object { $_ -replace '^HEAD:', '' }
$OffendersIndex = Get-GitLines @('grep','--cached','-I','-l','-P','\r$')
# CR-INSTRUMENT-QUOTED:END

# ── STALE-CHECKOUT DETECTION — kept identical in intent to the .sh sibling ──
#
# ★ MEASURED 2026-08-06 (TF-C123): the WSL leg of the 3-leg gate rsyncs the tree
#   with `--exclude '.git/'` (D-GATE-WSL-SYNC-LEAVES-GIT-HEAD-STALE), so HEAD and
#   the index answer about a DIFFERENT COMMIT than the files on disk, and this
#   guard convicted ten pre-normalisation blobs that were all LF on disk.
#   The detection is EXACT, not heuristic: a file the history calls CRLF while
#   carrying ZERO CR on disk proves the recorded history is not this tree's.
#   A genuine violation has CR in BOTH.
# ⚠ Present here even though this driver runs on the AUTHORITATIVE checkout in
#   practice: a capability in one sibling and not the other is the exact defect
#   class this repo keeps re-finding, and "it cannot happen on my host" is how
#   the last four instances survived.
$SkipHistoryScan = $false
foreach ($f in (@($OffendersHead) + @($OffendersIndex) | Sort-Object -Unique)) {
    # ⛔ `$f` IS REPO-RELATIVE, STRAIGHT OUT OF `git grep`. It must be resolved
    # against the root git ENUMERATED it from. `Test-Path` follows the provider
    # location and `[IO.File]` follows [Environment]::CurrentDirectory, so the
    # unresolved pair could answer about two different trees in one `if`.
    $abs = Resolve-RepoTreePath $RepoTree $f
    if ([string]::IsNullOrEmpty($f) -or -not (Test-Path -LiteralPath $abs)) { continue }
    $bytes = [IO.File]::ReadAllBytes($abs)
    if (($bytes | Where-Object { $_ -eq 13 } | Select-Object -First 1) -eq $null) {
        Write-Host "line-endings: HISTORY SCAN SKIPPED — this work tree's .git does not describe its files."
        Write-Host "    evidence: '$f' is recorded as CRLF in HEAD/index but carries ZERO CR on disk."
        Write-Host "    Convicting on that history would report violations belonging to another commit,"
        Write-Host "    so checks A and C are suspended. Run this guard on the AUTHORITATIVE checkout."
        $SkipHistoryScan = $true
        break
    }
}
if ($SkipHistoryScan) { $OffendersHead = @(); $OffendersIndex = @() }

$report = New-Object System.Collections.Generic.List[string]
function Add-Offenders([string]$What, $Files) {
    foreach ($f in @($Files)) {
        if ([string]::IsNullOrEmpty($f)) { continue }
        $report.Add("  ${What}: $f")
    }
}
Add-Offenders 'committed (HEAD)' $OffendersHead
Add-Offenders 'staged (index)'   $OffendersIndex

# ── ONE `git ls-files --eol` read, shared by checks C/D/E, WITH A FLOOR ───────
# Checks D and E both answer from this table, and a table that comes back EMPTY
# would make both report nothing — the "guard that passes over what it never
# read" shape this file already fails closed against on the history side. So the
# row count carries the same floor as the positive control above.
# MEASURED 2026-08-10: 2,265 tracked paths.
# ⚠ NAMED `$AllTrackedEolRows`, NOT `$EolRows`. PowerShell variable names are
# CASE-INSENSITIVE, so a `$EolRows` here IS the same variable as check C's
# `$eolRows` — which holds only the PINNED-GLOB subset. ✔MEASURED 2026-08-10: with
# the colliding name, check C's assignment overwrote this table and checks D and
# E1 silently walked 1,418 rows instead of 2,265 while the summary still claimed
# to have judged the working tree. Found by RUNNING both twins and comparing
# their counts (2,265 vs 1,418), not by reading the code.
$AllTrackedEolRows = Get-GitLines @('ls-files','--eol')
if ($AllTrackedEolRows.Count -lt $ScanFloor) {
    Write-Host "line-endings: FAIL - ``git ls-files --eol`` returned only $($AllTrackedEolRows.Count) rows, below its floor of $ScanFloor."
    Write-Host "  Checks D and E answer from that table, so an empty one makes BOTH report a clean"
    Write-Host "  worktree over files they never looked at. Refusing to pass; fix the scan."
    exit 2
}

# ── Check C: the pin's own glob set must contain no binary-detected blob ──
# Suspended with checks A/C on a stale checkout: `i/-text` is an INDEX fact.
$PinnedGlobEolRows = if ($SkipHistoryScan) { @() } else {
    Get-GitLines @('ls-files','--eol','--','*.c','*.h','*.cpp','*.hpp',
                   '*.cmake','CMakeLists.txt','*/CMakeLists.txt','*.md','VERSION')
}
foreach ($row in $PinnedGlobEolRows) {
    if ($row -match '^i/-text') {
        $path = ($row -split "`t", 2)[-1]
        $report.Add("  staged (index): binary-detected inside the eol=lf pin, so the blob scan above never opened it: $path")
    }
}

# ── Check D: a PINNED file's WORKING TREE must not have been rewritten ────
# ★ THE PIN'S OWN BLIND SPOT. `.gitattributes` normalises on `git add`, so once
# a file is pinned `eol=lf`, a tool that rewrites it CRLF ON DISK changes every
# byte while changing nothing git will show as a change (measured instance:
# Python `pathlib.write_text` over 159 fixtures). MEASURED exactly: `git status`
# DOES report ` M <path>`, but `git diff`, `git diff --stat` and (after
# `git add`) `git diff --cached` are ALL empty — so the file reads as modified
# with no reviewable content, and the rewrite can never even be committed.
# Checks A-C cannot see it BY CONSTRUCTION: the blob is fine; the tree is not.
# Closed set (`w/crlf`, `w/mixed`) on purpose — an unmeasured `w/` state must
# not be guessed at. Kept identical to the .sh sibling.
foreach ($row in $AllTrackedEolRows) {
    $parts = $row -split "`t", 2
    if ($parts.Count -lt 2) { continue }
    $attrs = $parts[0]
    if ($attrs -notmatch 'eol=lf') { continue }
    if ($attrs -match '(^|\s)w/(crlf|mixed)(\s|$)') {
        $report.Add("  working (tracked, eol=lf pinned): rewritten to CRLF on disk - git diff shows NOTHING to review: $($parts[1])")
    }
}

# ── Check E: THE UNSTAGED WORKING TREE — the tier the other checks cannot see ──
# ★★ THE BLIND SPOT, MEASURED 2026-08-10 on this tree. Checks A and B read BLOBS
# (HEAD and the index); check C is an index fact; check D reads the disk but ONLY
# for files that DECLARE `eol=lf`. So a CRLF introduced into a working file that
# is neither staged nor covered by the pin was invisible to every tier - and
# "before commit" is exactly when a line-ending mistake is cheap to fix and the
# only moment this guard can prevent rather than diagnose.
# Not theoretical arithmetic: of 2,265 tracked paths, 2,186 declare `eol=lf` and
# 55 are tracked TEXT with NO such declaration. `.gitattributes` carries no
# `* text=auto` and this workstation reports `core.autocrlf=false`, so for those
# 55 git performs NO normalisation on `git add` - a CRLF rewrite of any one of
# them LANDS CRLF IN THE COMMIT. Same for a NEW untracked file outside the pin.
# ⚠ `core.autocrlf=true` (the Git-for-Windows INSTALLER DEFAULT, so the likeliest
# config for a reader of THIS twin) makes a CRLF WORKING COPY of an unpinned text
# file the LEGITIMATE result of checkout. Convicting there would be a false red on
# a correctly-configured host, so E1 states the situation instead of convicting -
# and E2 (untracked files, never checked out) keeps convicting either way.
# Kept behaviourally identical to the .sh sibling, measured by the same `git`
# subcommands with the same arguments.
# ⚠ `@(...)` around the call: this is the one query here whose answer is a SINGLE
# line, and an unwrapped one-element result unrolls to a String, on which
# `Set-StrictMode -Version Latest` makes `.Count` a terminating error.
$AutoCrlfRows = @(Get-GitLines @('config','core.autocrlf'))
$AutoCrlf = if ($AutoCrlfRows.Count -ge 1) { [string]$AutoCrlfRows[0] } else { '' }
if ([string]::IsNullOrEmpty($AutoCrlf)) { $AutoCrlf = '<unset>' }
# E1 - TRACKED, text, NOT covered by an `eol=lf` pin, CRLF/mixed on disk.
# `i/-text` is binary (its 0x0D is legitimate) and `i/none` is empty; both are
# excluded by NAME rather than by "anything else", for the same reason check D
# uses a closed `w/` set: an unmeasured state must not be guessed into a red.
foreach ($row in $AllTrackedEolRows) {
    $parts = $row -split "`t", 2
    if ($parts.Count -lt 2) { continue }
    $attrs = $parts[0]
    if ($attrs -match 'eol=lf') { continue }
    # Matched by REGEX, not by splitting and indexing: `Set-StrictMode -Version
    # Latest` turns an out-of-bounds array index into a THROW, so a single
    # malformed row would kill the guard with a PowerShell stack trace instead of
    # a diagnostic. Check D above already reads this field with a regex; E1 uses
    # the same shape so the two cannot diverge in robustness either.
    if ($attrs -match '(^|\s)i/(-text|none)(\s|$)') { continue }
    if ($attrs -notmatch '(^|\s)w/(crlf|mixed)(\s|$)') { continue }
    if ($AutoCrlf -eq 'true') {
        Write-Host "line-endings: NOTE - '$($parts[1])' is CRLF on disk and carries NO eol=lf pin, but core.autocrlf=true,"
        Write-Host "    so a CRLF checkout is the expected result here and this is NOT convicted. Add an eol=lf pin"
        Write-Host "    for its extension if this repo should own its bytes regardless of a host's git config."
    } else {
        $report.Add("  working (tracked, NOT covered by an eol=lf pin): CRLF on disk and core.autocrlf=$AutoCrlf, so ``git add`` will NOT normalise it - this WILL land CRLF in the commit: $($parts[1])")
    }
}
# E2 - UNTRACKED (not ignored), text, NOT covered by an `eol=lf` pin, has a CR.
# A pinned untracked file is deliberately NOT reported: its clean filter
# normalises it on `git add`, so it cannot land CRLF, and a guard that reds on a
# state git is about to fix teaches people to ignore it.
foreach ($f in (Get-GitLines @('ls-files','--others','--exclude-standard'))) {
    # ⛔ Same wrong-root read as check C above, and THIS is the site that died in
    # the measurement: `ls-files --others` lists a file that exists ONLY in the
    # lane, and the unresolved `[IO.File]::ReadAllBytes` looked for it at the
    # other root.
    $abs = Resolve-RepoTreePath $RepoTree $f
    if (-not (Test-Path -LiteralPath $abs -PathType Leaf)) { continue }
    $bytes = [IO.File]::ReadAllBytes($abs)
    if ($bytes.Length -eq 0) { continue }
    # BINARY skip via git's own heuristic (a NUL in the first 8000 bytes), so the
    # answer matches what `git grep -I` would have decided for a tracked blob.
    $probe = [Math]::Min($bytes.Length, 8000)
    $isBinary = $false
    for ($i = 0; $i -lt $probe; $i++) { if ($bytes[$i] -eq 0) { $isBinary = $true; break } }
    if ($isBinary) { continue }
    # An explicit `binary` / `-text` declaration is an exemption, exactly as `-I`
    # honours it for the blob tiers.
    if ((Get-GitLines @('check-attr','text','--',$f)) -match ': text: unset$') { continue }
    if ((Get-GitLines @('check-attr','eol','--',$f))  -match ': eol: lf$')     { continue }
    $hasCr = $false
    foreach ($b in $bytes) { if ($b -eq 13) { $hasCr = $true; break } }
    if ($hasCr) {
        $report.Add("  working (untracked, not yet added): carries CR and NO eol=lf pin covers it, so ``git add`` will NOT normalise it - this WILL land CRLF in the commit: $f")
    }
}

# ── Check F: the INSTRUMENT tier - a CR detector that cannot detect a CR ──
# ★★ THE TIER CHECKS A-E CANNOT REACH. A-E judge BYTES; Check F judges the
# MEASUREMENT - text that will be RUN or COPIED to answer "is there a CR here?"
# and that answers wrongly on this host. ✔MEASURED 2026-08-27: a lane certified
# thirteen files "pure LF" with a blind awk test. Every byte tier was green
# throughout, correctly so - the tree WAS clean. The claim about it was worthless.
$instrumentRc = Invoke-InstrumentAudit

if ($report.Count -eq 0 -and $instrumentRc -eq 0) {
    if ($SkipHistoryScan) {
        # Never let the summary outrun the evidence: say what was NOT judged.
        Write-Host "line-endings: OK (WORKTREE ONLY - the history scan was SKIPPED, see above; the $ControlHead HEAD / $ControlIndex index blobs were NOT judged; $($AllTrackedEolRows.Count) working-tree paths were)"
    } else {
        # ★ The summary NAMES EVERY TIER it judged. A guard that says "OK" without
        # saying over what invites the reader to assume it covered the tier they
        # care about - and for four tiers of this guard's life, one of them (the
        # unstaged working tree) was the tier it did NOT cover.
        Write-Host "line-endings: OK (committed $ControlHead + staged $ControlIndex text blobs, working tree $($AllTrackedEolRows.Count) tracked paths + untracked, core.autocrlf=$AutoCrlf; none carries CR)"
    }
    exit 0
}

# Check F failed on its own: it has already printed its finding and its fix, and
# the byte tiers found nothing. Do not head that with an "LF contract violated"
# banner over an empty list - a report whose heading outruns its evidence is the
# same self-blindness this guard is about.
if ($report.Count -eq 0) { exit 1 }

Write-Host "line-endings: FAIL - the LF contract is violated (tier named per line):"
Write-Host ""
foreach ($line in $report) { Write-Host $line }
Write-Host ""
Write-Host "Fix:"
Write-Host "  * A ``working (...)`` line is the CHEAP one: the bytes are only on your disk,"
Write-Host "    nothing is committed yet, and converting the file to LF right now costs a"
Write-Host "    single command. A ``committed (HEAD)`` line is the same defect after it"
Write-Host "    became history. Fix the working tier BEFORE you stage."
Write-Host "  (a) convert the file to LF and commit that rewrite ON ITS OWN, never"
Write-Host "      beside real changes; a whole-file EOL diff sitting next to logic is"
Write-Host "      unreviewable, which is how the original six got in unnoticed; OR"
Write-Host "  (b) add an ``eol=lf`` pin for its extension in ``.gitattributes`` so a"
Write-Host "      tool's platform default can never decide this repo's bytes again"
Write-Host "      (``pathlib.write_text`` on Windows is the measured culprit); OR"
Write-Host "  (c) if the file genuinely REQUIRES its 0x0D bytes preserved, declare it"
Write-Host "      ``binary`` in ``.gitattributes`` (the ``examples/**/*.bin`` precedent)."
Write-Host "      Do NOT reach for ``eol=crlf``: MEASURED - a ``text eol=crlf`` file"
Write-Host "      still stages as an LF blob, so it cannot make this red go away."
Write-Host ""
Write-Host "See D-REPO-GITATTRIBUTES-PINS-EOL-FOR-CONFIGS-BUT-NOT-FOR-SOURCES in"
Write-Host ".plans/_deferred-anchor-registry*.md for why this is machine-checked."
exit 1
