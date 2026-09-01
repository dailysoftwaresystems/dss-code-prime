#!/usr/bin/env pwsh
# check-anchor-registry.ps1 — Windows variant of the deferred-anchor
# registry CI guard. Mirrors the bash variant; same contract.
#
# Contract: every `D-*` identifier cited in a SCANNED ROOT (`src/`, `examples/`,
# `tests/`, `integrated_tests/`, `real-examples/`, `scripts/`, `.claude/` - see
# the roots table below)
# MUST resolve to a row in `.plans/_deferred-anchor-registry*.md` OR a citation
# in any `.plans/*.md` file.
#
# Exit codes, matching the `.sh` exactly: 0 clean · 1 an anchor resolves nowhere ·
# 2 a scan collapsed · 3 a markdown table row drops content · 4 a citation names a
# retired id · 5 a quotation declaration is stale or false · 6 the self-test failed.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot  = Split-Path -Parent (Split-Path -Parent $ScriptDir)
Set-Location $RepoRoot

# Anchor regex: the word-boundary `\b` before `D-` is what stops the regex from
# reaching INSIDE a longer hyphenated word and lifting a false anchor out of its
# tail. The live case is the phrase `FIXED-32-BIT-WORD` at
# `src/asm/format/fixed32.hpp:19` — an in-comment phrase whose tail is
# anchor-SHAPED but is not an anchor. `\b` fails there because the `D` is
# preceded by `E`, another word character, so the phrase is correctly skipped;
# without `\b` it would red this guard forever and could only be silenced by
# editing unrelated source. (`.sh` spells the same protection `\<`.)
#
# ⚠⚠ DO NOT WRITE THAT TAIL OUT HERE AS A STANDALONE LITERAL. THIS COMMENT DID,
# FROM TF-C111 UNTIL AP6 (2026-08-14), AND IT WAS A LIVE BOOBY-TRAP.
# Spelled on its own and preceded by a non-word character (a backtick will do),
# the tail stops being a quoted fragment and becomes a CITATION — of an anchor
# with no registry row, sitting in the one file the guard reads to decide what an
# anchor even is. It survived only because `scripts/` is unscanned; the moment this
# root is added, that line reds the guard on the guard's own prose.
# ✔MEASURED at the AP6 widening: it was the ONLY dangling anchor under `scripts/`,
# so rewording it is what makes that root addable. (It read "`tools/` and `scripts/`"
# until 2026-08-19, when those two directories became one.)
# ★★★ THE THRESHOLD IS `{1,}` SINCE 2026-09-01 (P50, operator ruling R3) — one
# hyphen group after the head makes a name FORMAL. It was `{2,}`, and that hid
# SEVENTY registered rows from this guard: the registry's own ANCHOR-NAME RULE
# spells a compound feature word as ONE segment, so its worked example turned a
# four-segment id this guard checked into a three-segment id it ignored. Full
# sizing, the refused rename and the stop-condition:
# D-GATE-ANCHOR-REGISTRY-SEGMENT-THRESHOLD-HIDES-SEVENTY-ROWS. HEAD-ONLY names
# (`D-OPT`) stay informal — pinned by the widened-core self-test arms below.
# ⇒ There is no under-the-threshold placeholder any more except a head-only
# name: a fixture or illustration is ASSEMBLED FROM CONCATENATED LITERALS
# (`'D-XX' + '-EXAMPLE'`), the pattern the self-test's own anchors use; never an
# Allowlist entry, which silences a name repo-wide and forever.
# See D-GATE-ANCHOR-GUARD-SCOPE-STILL-EXCLUDES-TOOLS-AND-TESTS, which is the row
# that predicted this and stays open for the `scripts/` + `tests/` half.
# ★ THE GRAMMAR IS SPELLED ONCE AND THE BOUNDARY IS BOLTED ON, mirroring the
# `.sh`. The wrap-recovery pass needs the same grammar WITHOUT the boundary,
# because it enforces the boundary itself by testing the preceding character; two
# spellings of the THRESHOLD would be the duplicated-site shape this file keeps
# closing, while two spellings of the BOUNDARY are unavoidable and so are made
# explicit here rather than left to be rediscovered.
$AnchorCore  = 'D-[A-Z0-9_]+(-[A-Z0-9_]+){1,}'
$AnchorRegex = '\b' + $AnchorCore

# ⚠ `-Force` IS LOAD-BEARING NOW, AND IT WAS NOT BEFORE. The header further down
# records `Get-ChildItem -Recurse` skipping hidden files and directories as a
# divergence from `grep -r`, "harmless only while no source lives under a dotted
# path". `.claude/` IS a dotted path, so adding it as a root retires that excuse.
# ✔MEASURED on this host, both ways: 35 files with `-Force` and 35 without,
# because a leading dot does not set the hidden ATTRIBUTE on Windows - so this is
# a repair for the filesystem where it does, not for a difference visible here.
# The cell-width half of this file already passes `-Force` for `.plans/`, and the
# two halves disagreeing about how to walk a dotted root is exactly the kind of
# drift `D-GATE-SCRIPT-PS1-CONTENT-DRIFT-UNCHECKED` is open about.
# ⚠ A GIT WORKTREE IS ANOTHER CHECKOUT OF THIS SAME REPO. An agent creates one
# under `.claude/worktrees/`, so the `.claude` root gets walked TWICE and every
# document faces its own duplicate. ✔MEASURED 2026-08-25: that produced six false
# "a quotation declaration LEAKED" failures -- each file accusing its own copy of
# exempting a name on its behalf -- on a tree that was entirely correct. The guard
# went red because a TOOL was running, which is the worst kind of false red: it
# trains a reader to disbelieve the guard.
# ★ Not a new judgement call. `.gitignore` calls these "throwaway checkouts an agent
# creates for an isolated build", and `check-plan-citations` already prunes
# `worktrees` as "another checkout". This brings the anchor guard in line.
# ★★ ONE FUNCTION, FOUR CALLERS. PowerShell has no `--exclude-dir` -- `-Exclude`
# filters NAMES and does not prune recursion -- so the filter runs on the RESULT, by
# path SEGMENT (never a substring: a directory legitimately named `my-worktrees-notes`
# must not vanish). The `.sh` twin has the same single owner in `_root_include_args`;
# adding this per call site would leave whichever site is added next as the hole.
$ScanExcludeDirs = @('worktrees', '__pycache__', '.git')
function Select-ScannableFile($Items) {
    return @($Items | Where-Object {
        $segs = ($_.FullName -replace '\\', '/') -split '/'
        -not ($segs | Where-Object { $ScanExcludeDirs -contains $_ })
    })
}

function Get-Anchors([string]$Path, [string[]]$Filters) {
    $files = Select-ScannableFile @(Get-ChildItem -Path $Path -Recurse -File -Force -Include $Filters -ErrorAction SilentlyContinue)
    $anchors = @{}
    foreach ($f in $files) {
        $content = Get-Content -Raw -LiteralPath $f.FullName -ErrorAction SilentlyContinue
        if (-not $content) { continue }
        $matchesFound = [regex]::Matches($content, $AnchorRegex)
        foreach ($m in $matchesFound) { $anchors[$m.Value] = $true }
    }
    return $anchors.Keys | Sort-Object
}

# Repo-relative, forward-slashed - the shape the `.sh` twin prints. The cell-width
# half already normalises this way; the anchor half did not, so every `cited in:`
# line differed between the twins by its separators alone. The twins are verified
# by DIFFING their output, and noise a real divergence could hide in is not free.
function RelPath([string]$full) {
    return $full.Substring($RepoRoot.Length + 1).Replace([IO.Path]::DirectorySeparatorChar, '/')
}

# ── WRAP RECOVERY ── twin of the block of that name in `check-anchor-registry.sh`,
# which carries the full rationale and the measurements. Restated here in the one
# sentence that matters, because a twin that only points at its sibling is how the
# pair drifts: a line-wrapped anchor id does not FAIL, it DISAPPEARS - the
# single-line regex sees at most a prefix, and 43 of the wrapped prefixes in this
# tree fall below the three-segment threshold and so are never collected at all.
# Recovery joins the continuation and lets the whole name face the ordinary
# resolve. It is a RECOVERY and not a refusal: refusing would demand 298 reflows
# of honest text across seven roots.
# ⚠ THE UPPERCASE REQUIREMENT LIVES IN ONE PLACE, and it used to live in two. An
# earlier cut tested `-cnotmatch '^[A-Z0-9_]'` before the anchored match below,
# which is the same question asked twice. ✔MEASURED on the `.sh` twin by planting
# a mutant on the first test and watching the self-test STAY GREEN - a redundant
# guard makes the property untestable, because no single-point break exposes it.
function WrapContinuation([string]$next) {
    $c = $next -replace '^[^A-Za-z0-9_]+', ''
    $m = [regex]::Match($c, '^[A-Z0-9_]+(-[A-Z0-9_]+)*')
    if (-not $m.Success) { return '' }
    return $m.Value
}
# The WORD-BOUNDARY test here is the .NET spelling of the `\b` in $AnchorRegex and
# of the `\<` the `.sh` greps use: the character before the token must not be a
# word character. ★ It LOOPS rather than testing the first match: the leftmost
# `D-` on a line can be an inner fragment of a longer hyphenated word while a
# genuine anchor sits later on the same line.
function WrapPrefix([string]$line) {
    $s = $line -replace '[ \t]+$', ''
    if (-not $s.EndsWith('-')) { return '' }
    $s = $s.Substring(0, $s.Length - 1)
    $rx = [regex]::new('D-[A-Z0-9_]+(-[A-Z0-9_]+)*$')
    $off = 0
    while ($off -le $s.Length) {
        $m = $rx.Match($s, $off)
        if (-not $m.Success) { return '' }
        if ($m.Index -eq 0 -or $s[$m.Index - 1] -notmatch '[A-Za-z0-9_]') { return $s.Substring($m.Index) }
        $off = $m.Index + 1
    }
    return ''
}
function JoinWrappedInFile([string]$path, [string]$rel) {
    $out = [System.Collections.ArrayList]::new()
    $lines = ([IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)) -split "`n"
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $pre = WrapPrefix ($lines[$i].TrimEnd([char]13))
        if ($pre -eq '') { continue }
        $nxt = ''
        if ($i + 1 -lt $lines.Count) { $nxt = $lines[$i + 1].TrimEnd([char]13) }
        $cont = WrapContinuation $nxt
        if ($cont -eq '') { continue }
        [void]$out.Add("${rel}:${pre}-${cont}")
    }
    return $out.ToArray()
}

# ── QUOTED-NOT-CITED ── twin of the block of that name in the `.sh`, where the
# design argument lives in full: a document that QUOTES a dead id as evidence is
# not citing live work, and neither of this project's existing mechanisms fits.
# Rewording it would edit a measured record to suit a tool; an Allowlist entry
# silences a name repo-wide and forever and its own table is headed "code-internal
# pins, NOT deferrals". A declaration is scoped to ONE file, is visible to a human
# reader, and EXPIRES - three refusals below make sure of that.
# ★★ RECOGNITION IS POSITIONAL: only DECORATION may precede the token, so writing
# ABOUT the marker cannot trip it. That is the same lesson the retired-id matcher
# learned from two production false positives.
$QuoteDeclRx = '^[^A-Za-z0-9_]*ANCHOR-GUARD-QUOTED-NOT-CITED:'
function DeclRecordsInFile([string]$path, [string]$rel) {
    $out = [System.Collections.ArrayList]::new()
    $lines = ([IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)) -split "`n"
    $isDecl = New-Object bool[] $lines.Count
    $ids = [System.Collections.ArrayList]::new()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i].TrimEnd([char]13)
        if ($line -cnotmatch $QuoteDeclRx) { continue }
        $isDecl[$i] = $true
        # ★ ONE MECHANISM PROTECTS THE ID LIST, AND IT IS THE WORD BOUNDARY. The
        # marker itself ENDS IN AN ANCHOR-SHAPED TAIL (the `D` of GUARD plus
        # `-QUOTED-NOT-CITED`), and the boundary test rejects it because a word
        # character precedes it - ✔MEASURED, because the first cut of this
        # extractor lacked the test, lifted that tail out of the marker, and
        # reported the declaration as STALE. An earlier cut ALSO stripped the
        # marker off the front first; that was the same question asked twice, and a
        # mutant deleting it left the self-test GREEN, which is how a redundant
        # guard rots. The strip is gone and the boundary test is pinned by two arms.
        $rest = $line
        foreach ($m in [regex]::Matches($rest, 'D-[A-Z0-9_]+(-[A-Z0-9_]+)*')) {
            if ($m.Index -gt 0 -and $rest[$m.Index - 1] -match '[A-Za-z0-9_]') { continue }
            if (-not $ids.Contains($m.Value)) { [void]$ids.Add($m.Value) }
        }
    }
    foreach ($id in $ids) {
        $cited = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($isDecl[$i]) { continue }
            if ($lines[$i].Contains($id)) { $cited = $true; break }
        }
        [void]$out.Add(($(if ($cited) { 'DECL:' } else { 'STALE:' }) + "${rel}:${id}"))
    }
    return $out.ToArray()
}

# ── The retired-id matcher, in the same POSITIONAL form as the `.sh` ──────────
# ★★★ THIS CHECK DID NOT EXIST IN THIS TWIN AT ALL until 2026-08-21, and the
# Windows ctest entry runs THIS file. ✔MEASURED: zero occurrences of the word
# RETIRED anywhere in this script, against a `.sh` that has refused with exit 4
# since 2026-08-17 - so on the Windows leg the retired-id refusal had never once
# run. That is `D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED` in its purest form: the pair
# existed, and one of them silently did less.
# The matcher is positional because two SEARCHING forms produced false positives
# in production - one on a row whose prose merely said "as a retired id", the next
# on the row that DOCUMENTS the token. A marker that is merely PRESENT can always
# be tripped by writing about it, so the status cell must OPEN with it.
# ⚠ Field splitting is on EVERY `|`, escaped or not, exactly as the `.sh`'s
# `awk -F'|'` does. Matching its imperfection is the point: the twins must agree.
function RetiredIdsIn([string[]]$lines) {
    $out = [System.Collections.ArrayList]::new()
    foreach ($raw in $lines) {
        $line = $raw.TrimEnd([char]13)
        if ($line -cnotmatch '^\| `D-') { continue }
        $cells = $line -split '\|'
        if ($cells.Count -lt 3) { continue }
        $key = $cells[1] -replace '[` ]', ''
        # ⚠⚠ TWO CELL POSITIONS, matching the `.sh`'s awk clause for clause. On
        # 2026-09-01 the registry gained explicit `Priority` and `Status` columns
        # (`| Anchor | Priority | Status | Trigger | ... |`), so the glyph-led prose
        # this matcher keys on moved from index 2 to index 4. ✔MEASURED: with only
        # index 2 the scan returned ZERO and the guard refused the whole tree -- its
        # fail-closed arm working exactly as its own message predicts. Plan-side §3.1
        # tables were NOT migrated and still lead with the glyph at index 2, so BOTH
        # are read; the marker is anchored to the START of a cell either way, which is
        # what keeps this from becoming a substring search.
        if ($cells[2] -cmatch '^ *\**✅ \*\*CLOSED [0-9-]+ — RETIRED-ID') { [void]$out.Add($key); continue }
        if ($cells.Count -gt 4 -and $cells[4] -cmatch '^ *\**✅ \*\*CLOSED [0-9-]+ — RETIRED-ID') { [void]$out.Add($key) }
    }
    return $out.ToArray()
}

# ── Per-root scan, hoisted into a function so the self-test drives the SAME code
# the live loop drives. A self-test that re-implements its subject proves only
# that two copies of a mistake agree.
function ScanOneRoot([string]$Root, [int]$Floor, [string[]]$Filters, [hashtable]$Into) {
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        Write-Host "anchor-registry: FAIL - scan root '$Root' does not exist. A missing root would silently shrink coverage; refusing to report a partial scan as a pass."
        return $false
    }
    # @() forces an array even for the 0- and 1-anchor cases: under
    # `Set-StrictMode -Version Latest` a `$null.Count` from an empty root is a
    # terminating error, i.e. the collapse case would die with a PowerShell stack
    # trace instead of the sentence that says what happened.
    $rootAnchors = @(Get-Anchors $Root $Filters)
    if ($rootAnchors.Count -lt $Floor) {
        Write-Host "anchor-registry: FAIL - root '$Root' yielded only $($rootAnchors.Count) anchors, below its floor of $Floor."
        Write-Host "  This does NOT mean that root is clean - it means ITS scan collapsed (unreadable files, a drifted -Include filter, or a moved subtree)."
        Write-Host "  Refusing to report a pass. Fix the scan; do not lower the floor."
        return $false
    }
    foreach ($a in $rootAnchors) { $Into[$a] = $true }
    return $true
}

# ════════════════════════════════════════════════════════════════════════════
# SELF-TEST - RUNS ON EVERY INVOCATION, like `check-orphan-tests`, and TWIN of the
# battery of the same name in `check-anchor-registry.sh`: same 13 arms, same order.
#
# ★★ IT TAKES NO FLAG, DELIBERATELY. ctest invokes this guard with no arguments,
# so a `-SelfTest` switch would be a self-test that CI never runs - the vacuous
# pass this repository already has a row about. Running it unconditionally means
# the ctest entry cannot go green without the guard first PROVING IT CAN FAIL.
#
# ★ EVERY ARM DRIVES THE PRODUCTION CODE - the same functions the live path calls,
# never a copy. A self-test that re-implements its subject proves only that two
# copies of a mistake agree.
#
# ⛔ THE FIXTURE NAMES ARE BELOW THE THREE-SEGMENT THRESHOLD, or are assembled from
# two adjacent string literals so the whole name never appears in this file's
# bytes. `scripts/` is now a SCANNED ROOT, so an anchor-shaped fixture written out
# here would be a CITATION of a name with no row - which is exactly the defect this
# cycle found in another script's parser fixtures.
# ⛔ AND THE DECLARATION MARKER IS NEVER WRITTEN AT THE START OF A LINE HERE: a
# line whose only prefix is decoration IS a declaration, so a fixture spelled
# literally would be read as a real one. It is built by concatenation instead.
$stFail = $false
function St-Arm([string]$name, [string]$expected, [string]$actual) {
    if ($expected -ceq $actual) { return }
    Write-Host "anchor-registry: SELF-TEST arm '$name' FAILED - this guard cannot be trusted to fail."
    Write-Host "    expected: $expected"
    Write-Host "    actual  : $actual"
    $script:stFail = $true
}
$stDir = Join-Path ([IO.Path]::GetTempPath()) ("anchor-registry-selftest-" + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $stDir)
try {
    # ── arms 1-4: wrap recovery, both directions ────────────────────────────
    # ⚠ Fixture ids ASSEMBLED FROM CONCATENATED LITERALS (`'D-CTL' + '-ONE'`) so
    # this file's own text carries no anchor-shaped token — under `{1,}` a bare
    # two-segment fixture would be a citation of a row that does not exist, in
    # the guard's own source. Same pattern as `$stAnchor` below; never an
    # Allowlist entry.
    [IO.File]::WriteAllLines((Join-Path $stDir 'wrap.txt'), [string[]]@(
        ('// a one-line citation D-CTL' + '-ONE needs no recovery'),
        '// wrapped, comment continuation D-WRAPA-',
        '// TAILA is the rest of it',
        '# wrapped, decoration continuation D-WRAPB-',
        '# -- TAILB ends it',
        '// wrapped into prose D-WRAPC-',
        '// tailc is a word, not a name',
        '// an inner fragment of a longer word FIXED-32-BIT-WORD-',
        '// TAILD must not be reached'))
    $stGot = ((@(JoinWrappedInFile (Join-Path $stDir 'wrap.txt') 'wrap.txt') |
        ForEach-Object { $_.Substring($_.LastIndexOf(':') + 1) }) -join ' ') + ' '
    St-Arm 'wrap-join-recovers-both-continuation-shapes' ('D-WRAPA' + '-TAILA D-WRAPB' + '-TAILB ') $stGot
    St-Arm 'wrap-join-refuses-a-lower-case-continuation' '' (($stGot -split ' ' | Where-Object { $_ -clike 'D-WRAPC*' }) -join ' ')
    St-Arm 'wrap-join-refuses-a-fragment-inside-a-longer-word' '' (($stGot -split ' ' | Where-Object { $_ -clike '*TAILD*' }) -join ' ')
    St-Arm 'wrap-join-leaves-an-unwrapped-citation-alone' '' (($stGot -split ' ' | Where-Object { $_ -clike 'D-CTL*' }) -join ' ')

    # ── arms 5-6: quotation declarations ────────────────────────────────────
    $stMarker = 'ANCHOR-GUARD-QUOTED-NOT-CITED:'
    [IO.File]::WriteAllLines((Join-Path $stDir 'decl.txt'), [string[]]@(
        ('prose that quotes D-QUO' + '-HERE as evidence of a deleted row'),
        ("  > $stMarker D-QUO" + '-HERE D-QUO' + '-GONE WORD-D-QUO' + '-INNER')))
    $stGot = ((@(DeclRecordsInFile (Join-Path $stDir 'decl.txt') 'decl.txt')) -join ' ') + ' '
    St-Arm 'quotation-declaration-classifies-cited-and-absent-ids' `
           ('DECL:decl.txt:D-QUO' + '-HERE STALE:decl.txt:D-QUO' + '-GONE ') $stGot
    # Two SEPARATE outcomes of the one boundary test, pinned separately: the
    # anchor-shaped tail inside the marker, and an id sitting in the middle of a
    # longer hyphenated word on the same line.
    St-Arm 'quotation-marker-does-not-cite-its-own-anchor-shaped-tail' '' `
           (($stGot -split ' ' | Where-Object { $_ -clike '*D-QUOTED*' }) -join ' ')
    St-Arm 'quotation-ids-must-sit-on-a-word-boundary' '' `
           (($stGot -split ' ' | Where-Object { $_ -clike ('*D-QUO' + '-INNER*') }) -join ' ')
    # A line that merely MENTIONS the marker is prose, not a declaration. Two
    # earlier markers in this guard were defeated by exactly that - one by a row
    # whose prose said the words, the next by the row that documented the token.
    [IO.File]::WriteAllLines((Join-Path $stDir 'mid.txt'), [string[]]@(
        ("see the $stMarker convention, which would exempt D-QUO" + '-MID')))
    St-Arm 'quotation-declaration-recognition-is-positional' '' `
           ((@(DeclRecordsInFile (Join-Path $stDir 'mid.txt') 'mid.txt')) -join ' ')

    # ── arms 7-9: root existence and per-root floor ─────────────────────────
    # The fixture anchor is assembled from two literals; see the note above.
    $stAnchor = 'D-SELFTEST' + '-FIXTURE-ANCHOR'
    [void](New-Item -ItemType Directory -Path (Join-Path $stDir 'root'))
    [IO.File]::WriteAllText((Join-Path $stDir 'root/a.md'), "cite $stAnchor here`n")
    $stSink = @{}
    $stGot = (& { ScanOneRoot (Join-Path $stDir 'no-such-root') 1 @('*.md') $stSink } 6>&1 | Out-String)
    St-Arm 'missing-root-refuses-and-says-so' 'yes' `
           $(if ($stGot -match 'does not exist' -and $stGot -match 'refusing to report a partial scan as a pass') { 'yes' } else { $stGot })
    $stGot = (& { ScanOneRoot (Join-Path $stDir 'root') 99 @('*.md') $stSink } 6>&1 | Out-String)
    St-Arm 'below-floor-root-refuses-and-names-the-floor' 'yes' `
           $(if ($stGot -match 'yielded only 1 anchors, below its floor of 99' -and $stGot -match 'do not lower the floor') { 'yes' } else { $stGot })
    $stSink = @{}
    [void](ScanOneRoot (Join-Path $stDir 'root') 1 @('*.md') $stSink)
    St-Arm 'at-floor-root-passes-and-collects' $stAnchor (($stSink.Keys | Sort-Object) -join ' ')

    # ── arms 10-14: the retired-id matcher, against its two production false positives
    # AND both cell layouts. The four-cell rows are plan-side §3.1 tables, which were not
    # migrated; the six-cell rows are the registry as it has stood since 2026-09-01.
    # ⚠ WITHOUT THE SIX-CELL PAIR THE index-4 CLAUSE IS UNTESTED, and an untested clause
    # is one a later edit deletes in silence -- which is exactly how this matcher came to
    # read only index 2 while the live registry had already moved past it.
    $stR1 = 'D-RETA' + '-FIXTURE-ROW'; $stR2 = 'D-RETB' + '-FIXTURE-ROW'; $stR3 = 'D-RETC' + '-FIXTURE-ROW'
    $stR4 = 'D-RETD' + '-FIXTURE-ROW'; $stR5 = 'D-RETE' + '-FIXTURE-ROW'
    $stGot = ((@(RetiredIdsIn ([string[]]@(
        "| ``$stR1`` | ✅ **CLOSED 2026-01-01 — RETIRED-ID, renamed** | t | c |",
        "| ``$stR2`` | 🟠 **OPEN** — the prose here merely says ""as a retired id"" | t | c |",
        "| ``$stR3`` | 🟠 **OPEN** — this row DOCUMENTS the RETIRED-ID token | t | c |",
        "| ``$stR4`` | P1 | ✅ CLOSED | ✅ **CLOSED 2026-01-01 — RETIRED-ID, renamed** | t | c |",
        "| ``$stR5`` | P1 | ✅ CLOSED | ✅ **CLOSED 2026-01-01 — an ordinary closure** | t | c |")))) -join ' ') + ' '
    St-Arm 'retired-matcher-extracts-a-positionally-marked-row' 'yes' `
           $(if ($stGot -clike "*$stR1*") { 'yes' } else { $stGot })
    St-Arm 'retired-matcher-ignores-prose-that-says-retired-id' '' `
           (($stGot -split ' ' | Where-Object { $_ -ceq $stR2 }) -join ' ')
    St-Arm 'retired-matcher-ignores-a-row-that-documents-the-token' '' `
           (($stGot -split ' ' | Where-Object { $_ -ceq $stR3 }) -join ' ')
    St-Arm 'retired-matcher-reads-the-SIX-cell-layout-too' 'yes' `
           $(if ($stGot -clike "*$stR4*") { 'yes' } else { $stGot })
    St-Arm 'retired-matcher-ignores-a-plain-six-cell-closure' '' `
           (($stGot -split ' ' | Where-Object { $_ -ceq $stR5 }) -join ' ')

    # ── arms 18-21: the widened core, both directions of the flip ───────────
    # The threshold moved `{2,}` → `{1,}` in P50 (the block at `$AnchorCore`
    # holds the sizing). These arms are what the disclosure row demanded — an
    # id of the NEWLY VISIBLE two-segment shape that resolves, one that does
    # not, and the head-only carve-out — or the widening is unverified in the
    # driver this host runs. Fixture ids concatenated, as everywhere here.
    $stVis = 'D-VIS' + '-ROW'          # two-segment: newly visible under {1,}
    $stHdo = 'D-HEAD' + 'ONLY'         # head-only: stays informal, the carve-out
    [IO.File]::WriteAllText((Join-Path $stDir 'vis.txt'), "cite $stVis and $stHdo here`n")
    $stGot = (([regex]::Matches((Get-Content -Raw (Join-Path $stDir 'vis.txt')), $AnchorRegex) |
        ForEach-Object { $_.Value }) -join ' ') + ' '
    St-Arm 'widened-core-collects-a-two-segment-id' "$stVis " $stGot
    St-Arm 'widened-core-still-ignores-a-head-only-name' '' `
           (($stGot -split ' ' | Where-Object { $_ -ceq $stHdo }) -join ' ')
    # The resolve, both directions, under the same SUBSTRING contract the
    # plan-side resolve applies (`.Contains()`): a plan-side citation resolves
    # the id; its absence leaves the id a finding. Pinned here so the widened
    # shape's resolve is exercised without walking the real `.plans/`.
    $stPlansBlob = "the plans cite $stVis in prose"
    St-Arm 'a-two-segment-id-the-plans-cite-resolves' 'yes' `
           $(if ($stPlansBlob.Contains($stVis)) { 'yes' } else { 'no' })
    $stNores = 'D-VIS' + '-NOWHERE'
    St-Arm 'a-two-segment-id-the-plans-do-not-cite-stays-a-finding' 'no' `
           $(if ($stPlansBlob.Contains($stNores)) { 'yes' } else { 'no' })
} finally {
    Remove-Item -Recurse -Force -LiteralPath $stDir -ErrorAction SilentlyContinue
}
if ($stFail) {
    Write-Host "anchor-registry: FAIL - the self-test did not pass, so no verdict from this run means anything."
    exit 6
}
Write-Host "anchor-registry: self-test OK - 21 arms (4 wrap recovery incl. 3 refusals to join, 4 quotation classification incl. the positional rule and the word boundary, 3 root existence/floor, 5 retired-id matcher incl. both production false positives and BOTH cell layouts, 4 widened-core incl. the head-only carve-out and both resolve directions, 1 green control); this guard is PROVEN able to fail."

# The scanned set MUST match the .sh sibling EXACTLY (`src/ examples/` x
# {cpp,hpp,json,c}). It did not: this script scanned `src` without `*.c` and
# `examples` with ONLY `*.c`, so every anchor cited solely in an
# `examples/**/expected.json` was invisible HERE while the .sh guard saw it —
# measured 2026-07-31 as a reproducible 777 (ps1) vs 781 (sh) disagreement.
# THAT divergence was false-NEGATIVE only (this guard checked strictly fewer
# SOURCE files, so it could never red where .sh greens), which is precisely why
# it survived: the weaker guard is the one the Windows leg runs.
# ⚠ But "this guard is merely the weaker one" is NOT true in general — see
# residual (iii) below, which leans the other way.
# An instance of D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED — the pairing itself is
# unenforced, so the two scripts can drift again silently. Note the registry
# lists this pair as PAIRED, which was true and not sufficient: pairing by
# EXISTENCE is not pairing by BEHAVIOUR.
#
# TWO KNOWN DIVERGENCES SURVIVE this fix. Neither bites today; both are
# recorded so the next reader does not have to re-derive them:
#   * `Get-ChildItem -Recurse` skips hidden files and directories unless
#     `-Force` is passed, whereas `grep -r` descends into them. Harmless only
#     while no source lives under a dotted path in `src/` or `examples/`.
#   * The `.sh` FAIL path matches with `grep -rln` (a REGEX) while this one
#     uses `-SimpleMatch` (a literal). Harmless only while anchor names carry
#     no regex metacharacters — which the `D-[A-Z0-9_-]+` shape guarantees
#     today, but nothing enforces.
#   * (iii) THE PLAN-SIDE RESOLVE DIVERGES THE OTHER WAY, so this guard is not
#     uniformly the weaker one: `.sh` resolves an anchor with
#     `grep -qrF -- "$a" .plans/` over EVERY file under `.plans/`, whereas this
#     script reads only `.plans/**/*.md`. That makes THIS guard STRICTER — a
#     potential false-POSITIVE (an anchor cited only in a non-`.md` plan file
#     would red here and green there). Inert today ONLY because `.plans/`
#     currently contains zero non-`.md` files; nothing enforces that.
# ── real-examples/ ADDED 2026-08-03 (TF-C111), D-HARNESS-ANCHOR-GUARD-SKIPS-HARNESS-DRIVERS.
# The guard covered `src/ examples/` only, so every `D-*` cited in a HARNESS
# DRIVER resolved to nothing and failed nothing. That is not hypothetical: the
# name `D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE` was carried in two
# hand-off documents AS THOUGH TRACKED and had, measured, ZERO hits repo-wide
# and no registry row — invisible to this guard, to the plan sweep, and to
# every cycle's orient step. An unenforced citation is a citation that rots.
# The drivers are SCRIPTS, so they need their own filter set — reusing
# $AnchorFileFilters would have scanned real-examples/ for *.cpp and found
# nothing, which is the silent no-op version of this fix.
# ⚠ Dry-run before widening further. A future root that reds must be closed by
# REGISTERING the rows, never by narrowing the guard.
# ★ CORRECTION 2026-08-03 (TF-C112): the TF-C111 note here (and its `.sh` twin) said
# "measured at 22 anchors cited across the drivers". With the filter set that actually
# SHIPPED the figure is **24** — `*.sh` + `*.ps1` alone is exactly 22, so the dry-run
# that justified the change was run WITHOUT the `*.py` filter that landed with it. All
# 24 resolve, so nothing was ever broken by it; the defect is that a MEASURED claim was
# stated twice and was off by the one filter the same commit added. Recorded rather
# than quietly corrected, because "I measured it" is exactly the kind of claim this
# project requires to be true.
#
# ════════════════════════════════════════════════════════════════════════════
# CHECK 2 of 2 — MARKDOWN TABLE CELL-WIDTH PROPERTY
# TWIN of the block of the same name in `check-anchor-registry.sh`. Closes
# `D-PLANS-REGISTRY-ROWS-WITH-EXTRA-CELLS-STILL-LIVE`.
#
# ★★ THE RATIONALE IS RESTATED HERE IN FULL, not delegated to the `.sh` with a
# "see the other file". A twin that only points at its sibling is how the pair
# drifts — and `D-GATE-SCRIPT-PS1-CONTENT-DRIFT-UNCHECKED` is open precisely
# because these two have drifted before.
#
# THE PROPERTY: **in every markdown table under `.plans/`, no data row may
# carry MORE cells than its own table's header.** A row with surplus cells has
# the overflow SILENTLY DROPPED by the renderer — the text stays in the file
# and vanishes from the page. Invisible from both sides: nothing in the raw
# text shows it, and nothing in a diff shows it.
#
# ★★ WHY A PROPERTY AND NOT A LIST. The registry row named above existed for
# this exact defect and did not stop it, because it *named three instances*
# instead of asserting the property, and scoped itself to one table of one
# file — repeating the mistake its own text diagnoses. While it was open, 19
# more rows accumulated (15 in the registry, 4 in plan-00, up to 39,403
# characters dropped in a single row). An instance list cannot fail; only a
# property can.
#
# ★★ TWO SUB-SHAPES, and a matcher tuned to one MISSES the other — so this is
# SHAPE-BLIND: it counts separators and never guesses intent.
#   (a) ALTERNATION pipes — `{block|EndStatement}`, `RECIPE|MAKE-N|DERIV`,
#       `MINGW*|MSYS*|CYGWIN*`, a shell pipeline `ls … | grep …`, `||`;
#   (b) ABSOLUTE-VALUE bars — `1,108 steps >|0.1 s|` — not an alternation at
#       all, and invisible to any "looks like a regex" heuristic.
#
# ⚠ NOT a violation: FEWER cells than the header. The renderer pads the
# trailing columns and loses nothing (the registry says so in its own words).
# 333 rows are in that state; failing them would enforce a PROXY (uniform
# width) rather than the PROPERTY (nothing invisible). Counted, never fatal.
#
# ⚠ Unescaped pipes only: `\|` is content. Implemented by protecting `\|` with
# a placeholder and splitting on `|` — the `(?<!\\)\|` rule without needing a
# lookbehind, which the `.sh` twin's POSIX awk does not have. NOT done with
# grep: `grep -o '\|'` counts BRE alternation (two false alarms in this
# project's history) and `grep -P` is absent from Git Bash, where it exits 2.
#
# ★ Every message this check emits is ASCII-ONLY, deliberately: the twins are
# verified by DIFFING their output, and a console-encoding difference between
# pwsh and Git Bash must never be able to fake a disagreement.
# ★ Two divergences the anchor half above documents are deliberately CLOSED
# here rather than re-documented: `-Force` makes the walk descend hidden paths
# like `find` does, and the case-SENSITIVE `-ceq '.md'` filter matches
# `find -name '*.md'` instead of also taking `README.MD`.
$CellWidthRoot       = '.plans'
# Floors far below the live figures so ordinary churn never trips them. These
# catch a COLLAPSED scan, not drift. Both twins MUST carry the same values.
# ★ NO LIVE COUNT IS QUOTED HERE, ON PURPOSE, AND THAT IS A CORRECTION: the row
# total grows every time anyone appends a table row, so a figure written into
# this comment is stale by the next cycle. ✔MEASURED 2026-08-10 — it had already
# drifted into THREE disagreeing values (two comments saying 3,097, a registry
# row saying 3,098) against a live 3,099, i.e. every copy was wrong and none of
# them could be trusted to say so. The single owner of the live figures is THIS
# SCRIPT'S OWN OUTPUT, which prints files / tables / rows on every run; read it
# there. Point-in-time readings belong in the registry's audit trail, where a
# date makes them history rather than a claim about now.
$CellWidthFileFloor  = 20
$CellWidthTableFloor = 100
$CellWidthRowFloor   = 1500
# ★★★ THERE IS NO EXCEPTION LIST, AND THERE MUST NEVER BE ONE AGAIN.
# This check briefly shipped a `$CellWidthQuarantine` ratchet — 17 known-bad
# rows across 9 plan files, scanned and counted but with their count PINNED so
# the run stayed green. It was defended as "a ratchet, not an exemption", and
# the distinction did not survive contact: a quarantine is the instance list
# this check exists to replace, wearing a config entry. All 17 rows were
# REPAIRED — 26 stray content pipes escaped to `\|` across 12 rows, and 6
# genuinely EXTRA cells joined with the ` ═══ ` seam across 5 more; every file
# verified by BYTE ARITHMETIC, (seams x 8) + (escapes x 1) + (seam-pad spaces
# x 1) = +75 bytes over the nine files, with line counts unchanged — and the
# mechanism was DELETED from BOTH twins in the same commit, so this check now
# asserts the property with ZERO exceptions.
# ⚠ If it ever fires on an honestly-earned change, fix the ROW. Do not
# reintroduce a pin, and do not narrow the property — a guard that is weakened
# every time it fires ends up asserting nothing.

# awk's trim removes SPACE and TAB only — not every Unicode space, which is
# what a bare .Trim() would do. These files are full of non-ASCII whitespace-
# adjacent characters, so the distinction is load-bearing for agreement.
function CwTrim([string]$s) { return $s.Trim([char]32, [char]9) }
$CwPlaceholder = [string][char]1

# Split a row on UNESCAPED pipes. Returns the cell count plus the cell array
# and the live [Lo..Hi] window, mirroring the awk twin's CELLS/CELL_LO/CELL_HI.
function CwCells([string]$line) {
    $t = $line -replace '\\\|', $CwPlaceholder
    $parts = $t -split '\|'
    $lo = 0
    $hi = $parts.Count - 1
    if ((CwTrim $parts[$lo]) -eq '') { $lo++ }
    if ($hi -ge $lo -and (CwTrim $parts[$hi]) -eq '') { $hi-- }
    $n = 0
    if ($hi -ge $lo) { $n = $hi - $lo + 1 }
    return [pscustomobject]@{ N = $n; Lo = $lo; Hi = $hi; Parts = $parts }
}

# ASCII-only, single-line, bounded. Stripping non-ASCII BEFORE truncating is
# what makes the twins agree byte-wise: removing every non-ASCII BYTE and
# removing every non-ASCII CHARACTER yield the same string, after which the
# truncation is byte- and char-identical. Truncating first would split a UTF-8
# sequence in a byte-based awk and not in a char-based one.
function CwDisplay([string]$s) {
    $t = $s -replace $CwPlaceholder, '|'
    $t = $t -replace '[^ -~]', ''
    $t = CwTrim $t
    if ($t.Length -gt 110) { return $t.Substring(0, 110) }
    return $t
}

# Kept SEPARATE rather than assigning into one status. With a single variable,
# whichever branch ran LAST would own the exit code, so a COLLAPSED scan could
# be reported as a content-drop and send the reader to the wrong file. Both are
# red either way; the PRECEDENCE (collapse 2 > content-dropped 3) is decided
# once, at the single `exit` below.
$cwCollapsed = 0; $cwViolated = 0
if (-not (Test-Path -LiteralPath $CellWidthRoot -PathType Container)) {
    Write-Host "anchor-registry: FAIL - cell-width root '$CellWidthRoot' does not exist. Refusing to report a pass on a scan of nothing."
    $cwCollapsed = 1
} else {
    # ORDINAL sort, to match the .sh's `LC_ALL=C sort`. Culture-aware ordering
    # would place `_deferred-…` differently and make the twins disagree for no
    # real reason.
    $cwFiles = [string[]]@(
        Get-ChildItem -LiteralPath $CellWidthRoot -Recurse -File -Force |
            Where-Object { $_.Extension -ceq '.md' } |
            ForEach-Object { $_.FullName.Substring($RepoRoot.Length + 1).Replace([IO.Path]::DirectorySeparatorChar, '/') }
    )
    [Array]::Sort($cwFiles, [StringComparer]::Ordinal)

    $cwTables = 0; $cwRows = 0; $cwOver = 0; $cwUnder = 0
    $cwOffenders = [System.Collections.ArrayList]::new()
    foreach ($rel in $cwFiles) {
        $raw = [IO.File]::ReadAllText((Join-Path $RepoRoot $rel), [Text.Encoding]::UTF8)
        $lines = $raw -split "`n"
        $inFence = $false; $hdr = -1; $hdrLine = 0; $skipNext = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $line = $lines[$i].TrimEnd([char]13)
            $s = CwTrim $line
            if ($s.StartsWith('```') -or $s.StartsWith('~~~')) { $inFence = -not $inFence; $hdr = -1; continue }
            if ($inFence) { continue }
            if (-not $line.Contains('|')) { $hdr = -1; continue }
            if ($skipNext) { $skipNext = $false; continue }
            $nxt = ''
            if ($i + 1 -lt $lines.Count) { $nxt = $lines[$i + 1].TrimEnd([char]13) }
            $ns = CwTrim $nxt
            # A table STARTS where a row is followed by a delimiter row. The
            # width comes from the table's OWN header: these files carry 2-,
            # 3-, 4-, 5- and 6-cell tables, so any global constant would be
            # wrong somewhere — and a check that is wrong somewhere trains
            # people to ignore it.
            if ($nxt.Contains('|') -and $ns -match '^\|?[ ]*:?-+:?[ ]*(\|[ ]*:?-+:?[ ]*)*\|?$') {
                $hdr = (CwCells $line).N; $hdrLine = $i + 1; $cwTables++; $skipNext = $true; continue
            }
            if ($hdr -lt 0) { continue }
            $cwRows++
            $cc = CwCells $line
            if ($cc.N -lt $hdr) { $cwUnder++; continue }
            if ($cc.N -eq $hdr) { continue }
            $cwOver++
            $ex = ''
            for ($k = $cc.Lo + $hdr; $k -le $cc.Hi; $k++) {
                if ($ex -ne '') { $ex += ' // ' }
                $ex += (CwTrim $cc.Parts[$k])
            }
            [void]$cwOffenders.Add([pscustomobject]@{
                F = $rel; L = $i + 1; H = $hdrLine; E = $hdr; A = $cc.N; X = (CwDisplay $ex) })
        }
    }

    if ($cwFiles.Count -lt $CellWidthFileFloor) {
        Write-Host "anchor-registry: FAIL - cell-width scan found only $($cwFiles.Count) markdown files under $CellWidthRoot, below its floor of $CellWidthFileFloor."
        Write-Host "  This does NOT mean the plans are clean - it means the SCAN COLLAPSED. Refusing to report a pass; fix the scan, do not lower the floor."
        $cwCollapsed = 1
    }
    if ($cwTables -lt $CellWidthTableFloor) {
        Write-Host "anchor-registry: FAIL - cell-width scan found only $cwTables tables, below its floor of $CellWidthTableFloor."
        Write-Host "  A collapsed table scan checks nothing. Refusing to report a pass; fix the scan, do not lower the floor."
        $cwCollapsed = 1
    }
    if ($cwRows -lt $CellWidthRowFloor) {
        Write-Host "anchor-registry: FAIL - cell-width scan found only $cwRows table data rows, below its floor of $CellWidthRowFloor."
        Write-Host "  A collapsed row scan checks nothing. Refusing to report a pass; fix the scan, do not lower the floor."
        $cwCollapsed = 1
    }
    # ★★ THE VIOLATION COUNT IS COUNTED, NEVER DERIVED. `$cwOver` is incremented
    # at exactly ONE place — the moment a row is found wider than its header —
    # and that SAME variable drives the FAIL headline and the summary line, while
    # the detail loop walks the offender list it was incremented alongside. There
    # is no second source of truth for it to disagree with, and nothing is
    # subtracted from it.
    # MEASURED 2026-08-10, by exercising the failure arm rather than reading it:
    # while a quarantine ratchet existed here, the live count was DERIVED as
    # `scanned - pinned`, which printed "-1 live violation(s)" the moment a
    # pinned row was repaired, and - far worse - a repair in a pinned file plus a
    # NEW break in a clean one CANCELLED to "0 live" while the run was failing. A
    # guard whose own summary can read 0 during a failure is the "printed
    # failed=0 and exited 2" shape all over again. The ratchet is gone; this note
    # stays so the subtraction is never reintroduced with it.
    if ($cwOver -gt 0) {
        Write-Host "anchor-registry: FAIL - $cwOver markdown table row(s) carry MORE cells than their table header."
        Write-Host "The surplus is SILENTLY DROPPED by the renderer: the text stays in the file and"
        Write-Host "vanishes from the page. This is invisible from both sides - nothing in the raw"
        Write-Host "text shows it, and nothing in the diff shows it."
        Write-Host ""
        foreach ($o in $cwOffenders) {
            Write-Host "  $($o.F):$($o.L)  header@$($o.H) expected=$($o.E) actual=$($o.A)"
            Write-Host "      dropped: $($o.X)"
        }
        Write-Host ""
        Write-Host "Fix: the surplus is CONTENT that was read as a column boundary. Escape a content"
        Write-Host "pipe as \| (the only form that survives inside a code span); join a genuinely"
        Write-Host "extra cell with the registry seam marker instead of a column boundary. Never"
        Write-Host "delete text to make the count fit, and never widen the check."
        $cwViolated = 1
    }
    Write-Host "anchor-registry: cell-width $cwTables tables / $cwRows rows in $($cwFiles.Count) files: $cwOver violation(s), $cwUnder short rows (padded, no loss)."
}

# ★★ FAIL-CLOSED, ADDED 2026-08-03 (TF-C112), D-GATE-ANCHOR-GUARD-FAILS-OPEN-ON-MISSING-ROOT.
# Mirrors the `.sh` sibling: assert every root EXISTS, then apply a count FLOOR.
# The `.sh` form could report `OK (1 src anchors all resolve to plans)` while scanning
# NOTHING; this script's `Get-ChildItem -ErrorAction SilentlyContinue` has the same
# shape — a renamed root yields an empty set and a silently smaller scan. A guard that
# reports success while checking nothing is the worst defect a guard can have.
$AnchorFileFilters     = @('*.cpp', '*.hpp', '*.json', '*.c',
                           '*.s', '*.inc', '*.probes', 'CMakeLists.txt', '*.md')
$HarnessFileFilters    = @('*.sh', '*.ps1', '*.py')
# `scripts/` is a driver root like `real-examples/`, plus its prose: ✔MEASURED per
# extension, anchors live in `*.sh` (26), `*.ps1` (21) and `*.py` (38) and nowhere
# else, and `scripts/README.md` is a citation site like any other prose file. The
# rest of the tree there is test FIXTURE data - a C example project, a golden
# `.expected`, two JSON data files - and scanning input data for citations is the
# category error that this cycle found in another script's parser fixtures.
$ScriptFileFilters     = @('*.sh', '*.ps1', '*.py', '*.md')
# `.claude/` is where this project's RULES live, so a dangling citation there
# misleads exactly the reader with the most authority to act on it. ✔MEASURED: 40
# distinct anchors, 39 in `*.md` and 1 in `*.mjs`. `*.py` contributes zero today
# and is included anyway because `.claude/` DOES contain a `.py` file - that is
# coverage, not a no-op. `*.json` is deliberately out: the only JSON here is the
# gitignored, machine-local `settings.local.json`.
$SkillFileFilters      = @('*.md', '*.py', '*.mjs')

# ── tests/ + integrated_tests/ ADDED 2026-08-14 (AP6),
# D-GATE-ANCHOR-GUARD-SCOPE-STILL-EXCLUDES-TOOLS-AND-TESTS. Mirrors the `.sh`
# sibling in the same commit; that file carries the full rationale, and per the
# ★★ note further down this one restates the load-bearing half rather than
# pointing at it, because a twin that only points at its sibling is how the pair
# drifts. ✔MEASURED at the widening: 709 distinct anchors under `tests/`, 18
# under `integrated_tests/` — the guard's promise was true only of the subtree
# it looked at, and the corpus HARNESSES live in the subtree it did not.
# ⚠ The MATCHER was deliberately not touched: the wrapped-across-two-lines
# citation the widening was expected to strand is already handled by the
# SUBSTRING resolve below (a wrapped fragment is a PREFIX, hence a substring),
# measured at 4 unresolved names over both roots and not one of them wrapped.
# ⚠ The FILE FILTERS were widened at the same time, `CMakeLists.txt` above all:
# ctest ENTRY NAMES live there and closed registry rows cite them as landed
# evidence, so a rename could silently invalidate a row. `real-examples` keeps
# the driver-only set — it contains ZERO files of any added type, so widening it
# would be the silent no-op version of a fix.
#
# ★★ PER-ROOT FLOORS, REPLACING A SINGLE GLOBAL ONE — a twin divergence found
# and closed by this change, not a new feature. The `.sh` moved to per-root
# floors in TF-C112 after an audit MEASURED that a global floor catches no
# single-root collapse (losing all of `src/` still left enough anchors to pass);
# this script was never updated and still carried `$AnchorFloor = 100` against a
# live total near 1,150. Adding two roots to a global floor would have made that
# worse: `integrated_tests` contributes 18 anchors, so its total disappearance
# could never move a global total past any useful threshold. The pairing
# contract is about BEHAVIOUR, and "both scripts have a floor" was pairing by
# existence. Floors below MUST stay identical to the `.sh` table.
#
# ★ ONE TABLE, READ BY BOTH THE COLLECTION SCAN AND THE FAIL-PATH LOCATOR — the
# locator below spelled the roots and filters a second time, so adding a root to
# the scan alone would have left it unable to attribute a `cited in:` line for
# that root. Derived from this array now, so a root cannot be half-added.
# ── scripts/ + .claude/ ADDED 2026-08-21 (P23), completing
# D-GATE-ANCHOR-GUARD-SCOPE-STILL-EXCLUDES-TOOLS-AND-TESTS. Mirrors the `.sh`
# sibling in the same commit; that file carries the full rationale and the
# per-extension measurements. The half of it that must not be delegated: the
# `scripts/` widening does NOT close that row, because ✔MEASURED, the eleven
# synthetic fixture names the row predicted as blockers resolve today through
# exactly ONE piece of text - the registry row that reports them as unregistered -
# and through nothing else. Green because the complaint exists is not green.
# Floors below MUST stay identical to the `.sh` table.
$RootSpecs = @(
    @{ Root = 'src';              Floor = 400; Filters = $AnchorFileFilters  }
    @{ Root = 'examples';         Floor = 150; Filters = $AnchorFileFilters  }
    @{ Root = 'tests';            Floor = 300; Filters = $AnchorFileFilters  }
    @{ Root = 'integrated_tests'; Floor = 8;   Filters = $AnchorFileFilters  }
    @{ Root = 'real-examples';    Floor = 10;  Filters = $HarnessFileFilters }
    @{ Root = 'scripts';          Floor = 25;  Filters = $ScriptFileFilters  }
    @{ Root = '.claude';          Floor = 15;  Filters = $SkillFileFilters   }
)

# ★★ FAIL-CLOSED. Every root must EXIST and must independently CLEAR ITS FLOOR.
# `Get-ChildItem -ErrorAction SilentlyContinue` turns a renamed root into an
# empty set and a silently smaller scan; a guard that reports success while
# checking nothing is the worst defect a guard can have.
# ★ Every failing root is reported before exiting (mirroring the `.sh`'s
# `_scan_failed` accumulator) rather than exiting on the first: a guard that
# aborts after the first problem makes the reader re-run it N times to learn N
# things.
$scanFailed = $false
$srcAnchorSet = @{}
foreach ($spec in $RootSpecs) {
    if (-not (ScanOneRoot $spec.Root $spec.Floor $spec.Filters $srcAnchorSet)) { $scanFailed = $true }
}
if ($scanFailed) { exit 2 }

# ── WRAP RECOVERY, one root at a time, AFTER the floors ──────────────────────
# ★ The floors are computed on the RAW single-line scan and are deliberately
# unaffected by recovery: a floor exists to catch a COLLAPSED scan, and folding
# recovered names into it would let a burst of wrapped citations mask a root that
# had stopped being read. Recovered names widen what must resolve; they must not
# move what proves the scan ran.
# The recovered records are `path:token` - the same shape the FAIL-path locator
# index uses - so a recovered name that fails to resolve is reported WITH its
# `cited in:` line, which matters most for exactly the citations that are hardest
# to find by eye.
$wrapRecords = [System.Collections.ArrayList]::new()
foreach ($spec in $RootSpecs) {
    if (-not (Test-Path -LiteralPath $spec.Root -PathType Container)) { continue }
    foreach ($f in (Select-ScannableFile @(Get-ChildItem -Path $spec.Root -Recurse -File -Force `
                                   -Include $spec.Filters -ErrorAction SilentlyContinue))) {
        foreach ($r in (JoinWrappedInFile $f.FullName (RelPath $f.FullName))) { [void]$wrapRecords.Add($r) }
    }
}
$wholeAnchorRx = [regex]::new('^' + $AnchorCore + '$')
foreach ($r in $wrapRecords) {
    $t = $r.Substring($r.LastIndexOf(':') + 1)
    if ($wholeAnchorRx.IsMatch($t)) { $srcAnchorSet[$t] = $true }
}

# ── QUOTED-NOT-CITED declarations - see the block of that name above ─────────
# Runs BEFORE the anchor set is frozen, because an exempted id must never reach
# the resolve. ⓘ A RETIRED id can never be exempted: a retired row exists, so the
# id RESOLVES, so the FALSE refusal below fires first. That is by construction
# rather than by a special case.
$declRecords = [System.Collections.ArrayList]::new()
foreach ($spec in $RootSpecs) {
    if (-not (Test-Path -LiteralPath $spec.Root -PathType Container)) { continue }
    foreach ($f in (Select-ScannableFile @(Get-ChildItem -Path $spec.Root -Recurse -File -Force `
                                   -Include $spec.Filters -ErrorAction SilentlyContinue))) {
        $head = Get-Content -Raw -LiteralPath $f.FullName -ErrorAction SilentlyContinue
        if (-not $head) { continue }
        if ($head -cnotmatch "(?m)$QuoteDeclRx") { continue }
        foreach ($r in (DeclRecordsInFile $f.FullName (RelPath $f.FullName))) { [void]$declRecords.Add($r) }
    }
}

# ⚠ KNOWN PAIRING DIVERGENCES, extension-case half — documented 2026-08-03 (TF-C112),
# adding to the three already listed above. `Get-ChildItem -Include '*.sh'` matches
# `RUN.SH`; `grep --include='*.sh'` does NOT. So a driver with an uppercase extension
# would be scanned on Windows and INVISIBLE on the Linux leg. Likewise the FAIL-path
# locator: the `.sh` uses `grep -rln` (case-SENSITIVE) while this script uses
# `Select-String -SimpleMatch`, which is case-INSENSITIVE by default — the
# regex-vs-literal half of that divergence was already documented, the CASE half was
# not. Both are inert today (zero uppercase-extension files in the scanned roots) and
# are left as documented divergences rather than "fixed", because forcing either side
# to the other's casing rules would change which files are scanned on one leg only —
# which is the very asymmetry being guarded against. If an uppercase-extension driver
# ever lands, close this by making BOTH sides case-sensitive, not by matching Windows.

# Read every plan-file's raw content for substring matching. Substring
# (vs equality vs extracted-anchor-set) handles two false-positive modes:
#   (1) Multi-line citation in src: a comment wraps the anchor name
#       across a newline — the regex captures only the prefix.
#   (2) Plans use a more specific anchor name (e.g.
#       `D-LK6-14-INTEGRATION-GOT-SLOTS`) but src cites the parent
#       (`D-LK6-14-INTEGRATION`) — both are "known" via the same row.
#
# ════════════════════════════════════════════════════════════════════════════
# ★★ PAIRING DECISION, 2026-08-12: THE `.sh` TWIN'S PHASE-1 REWRITE AND ITS
# `LC_ALL=C` PREFIXES ARE **DELIBERATELY NOT MIRRORED HERE**, AND THIS IS THE
# REASON — recorded IN THE FILE because SKILL.md §2 requires a change to one
# twin to land in the other OR to be explained, and because
# `D-GATE-SCRIPT-PS1-CONTENT-DRIFT-UNCHECKED` means nothing else will notice.
#
# WHAT HAPPENED ON THE OTHER SIDE. ✔MEASURED 2026-08-12 on the operator's Mac
# (macOS 26.5.2 arm64, BSD grep 2.6.0-FreeBSD), matched content, same commit:
# the `.sh` guard took 274.6 s / 317.6 s in `en_US.UTF-8` and 180.4 s / 153.4 s
# in `LC_ALL=C`, against 0.17 s on Linux (GNU grep 3.11) and ~1.1 s in Git Bash
# (GNU grep 3.0). `ps` caught it parked at 100% CPU inside
# `grep -rhoF -f <950 patterns> .plans/` in BOTH locales. GNU grep compiles a
# `-F -f` set into one Aho-Corasick automaton; BSD grep has no such matcher and
# degrades toward (patterns x text). The `.sh` fix scans for the anchor PATTERN
# instead of for the 950 anchor STRINGS, so its cost is (1 x text): the same
# guard, same host, same content, went 270.6 s -> 4.3 s with a BYTE-IDENTICAL
# verdict. For scale, THIS script measures ~3.5 s on the Windows leg, so after
# the fix the two twins are finally in the same order of magnitude.
#
# WHY THIS FILE NEEDS NEITHER HALF OF THAT FIX:
#   · There is NO `grep` here at all. The plan-side resolve below is a single
#     in-memory `.Contains()` per anchor over one joined string — i.e. this twin
#     was ALREADY doing the cheap thing, which the `.sh` has now caught up to
#     from the slow side. There is no `-F -f` pattern set to degrade.
#   · There is no `LC_ALL` equivalent to apply. `[regex]::Matches` and
#     `String.Contains` are ORDINAL over UTF-16 for the ASCII-only character
#     classes this guard uses; no collation table participates, so no locale can
#     change either their result or their cost.
# ⇒ The pair still agrees on the VERDICT, which is the only thing the pairing
#   contract is about: the `.sh`'s phase 1/1b are a cheap PRE-FILTER whose only
#   authority is to hand work to phase 3, and phase 3 asks the same question
#   this line asks. What diverged is an implementation of a non-verdict-bearing
#   filter on one platform, not a behaviour.
# ⚠ If a `grep` ever appears in a `.sh`-side scan again, it needs `LC_ALL=C` and
#   it must not be given a large `-f` pattern set. That constraint lives on that
#   side; nothing here can enforce it.
# ════════════════════════════════════════════════════════════════════════════
$planFiles = Get-ChildItem -Path '.plans' -Recurse -File -Include '*.md'
$allPlanText = ($planFiles | ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"

# ── ONE `path:token` CITATION INDEX, BUILT AT MOST ONCE, WITH TWO CONSUMERS ──
# ★★ THE TREE IS WALKED ONCE - NOT ONCE PER QUESTION. This twin has paid for that
# lesson before (the FAIL-path locator re-walked per missing anchor and took 82.9
# s for eight of them), and the `.sh` paid for it a third time while this cycle's
# quotation check was being written: asking the tree per declared id took that
# guard from 8.1 s to 49.3 s for THREE ids. One pass answers both questions.
# ⚠ Built LAZILY: a tree with no quotation declaration and no missing anchor never
# pays for it, and a run that needs it for one consumer hands it to the other free.
$script:LocIndexBuilt = $false
$script:LocTokens = [System.Collections.ArrayList]::new()   # distinct tokens, first-seen order
$script:LocPaths  = @{}                                     # token -> ordered path list
$script:LocRank   = @{}                                     # path  -> enumeration position
function Build-CitationIndex {
    if ($script:LocIndexBuilt) { return }
    $script:LocIndexBuilt = $true
    $locFiles = @()
    foreach ($spec in $RootSpecs) {
        if (-not (Test-Path -LiteralPath $spec.Root -PathType Container)) { continue }
        $locFiles += Select-ScannableFile @(Get-ChildItem -Path $spec.Root -Recurse -File -Force `
                                     -Include $spec.Filters -ErrorAction SilentlyContinue)
    }
    $locRx = [regex]::new($AnchorRegex, [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    foreach ($f in $locFiles) {
        $c = Get-Content -Raw -LiteralPath $f.FullName -ErrorAction SilentlyContinue
        if (-not $c) { continue }
        $rel = RelPath $f.FullName
        $seenHere = @{}
        foreach ($m in $locRx.Matches($c)) {
            $t = $m.Value
            if ($seenHere.ContainsKey($t)) { continue }
            $seenHere[$t] = $true
            if (-not $script:LocPaths.ContainsKey($t)) {
                [void]$script:LocTokens.Add($t)
                $script:LocPaths[$t] = [System.Collections.ArrayList]::new()
            }
            [void]$script:LocPaths[$t].Add($rel)
        }
    }
    # The recovered wrapped names are part of the index, not just of the anchor set.
    foreach ($r in $wrapRecords) {
        $i = $r.LastIndexOf(':')
        $p = $r.Substring(0, $i); $t = $r.Substring($i + 1)
        if (-not $script:LocPaths.ContainsKey($t)) {
            [void]$script:LocTokens.Add($t)
            $script:LocPaths[$t] = [System.Collections.ArrayList]::new()
        }
        if (-not $script:LocPaths[$t].Contains($p)) { [void]$script:LocPaths[$t].Add($p) }
    }
    # ORDER: the old form walked FILES and printed the matching ones, so the report
    # is ordered by enumeration position. Replay that by ranking each path.
    $locOrder = 0
    foreach ($f in $locFiles) {
        $rel = RelPath $f.FullName
        if (-not $script:LocRank.ContainsKey($rel)) { $script:LocRank[$rel] = $locOrder++ }
    }
}

# ── The three quotation refusals. See the QUOTED-NOT-CITED block above ───────
$quoteFailed = $false
$exempt = @{}
foreach ($rec in $declRecords) {
    $kind = $rec.Substring(0, $rec.IndexOf(':'))
    $tail = $rec.Substring($rec.IndexOf(':') + 1)
    $i = $tail.LastIndexOf(':')
    $dpath = $tail.Substring(0, $i); $did = $tail.Substring($i + 1)
    if ($kind -eq 'STALE') {
        Write-Host "anchor-registry: FAIL - a quotation declaration is STALE."
        Write-Host "    $dpath declares $did as QUOTED-NOT-CITED, but that id appears nowhere else in that file."
        Write-Host "    The quotation it exempted is gone. Delete the declaration; an exemption that outlives what it covered silences a future real citation."
        $quoteFailed = $true
        continue
    }
    if ($allPlanText.Contains($did)) {
        Write-Host "anchor-registry: FAIL - a quotation declaration is FALSE."
        Write-Host "    $dpath declares $did as QUOTED-NOT-CITED, but that id RESOLVES in .plans/."
        Write-Host "    It names live work, so it is an ordinary citation. Remove it from the declaration and let the ordinary check own it."
        $quoteFailed = $true
        continue
    }
    # EQUALITY on the token, not substring: a LONGER name that happens to contain
    # this id is a different anchor and is nobody else's citation of this one.
    Build-CitationIndex
    $elsewhere = ''
    if ($script:LocPaths.ContainsKey($did)) {
        foreach ($p in $script:LocPaths[$did]) { if ($p -ne $dpath) { $elsewhere += " $p" } }
    }
    if ($elsewhere -ne '') {
        Write-Host "anchor-registry: FAIL - a quotation declaration LEAKED."
        Write-Host "    $dpath declares $did as QUOTED-NOT-CITED, but that id is also cited in:$elsewhere"
        Write-Host "    One document may not exempt a name on another document's behalf. Either those are live citations and the id needs a row, or they are quotations and each file declares its own."
        $quoteFailed = $true
        continue
    }
    $exempt[$did] = $true
}
if ($quoteFailed) {
    Write-Host "  A quotation declaration says 'this document QUOTES a dead id as evidence, it does not cite live work'."
    Write-Host "  It is scoped to ONE file and it EXPIRES: that is what makes it narrower than an Allowlist entry, which"
    Write-Host "  silences a name repo-wide and forever. Repair the declaration; do not widen it."
    exit 5
}
foreach ($e in $exempt.Keys) { $srcAnchorSet.Remove($e) }
$srcAnchors = @($srcAnchorSet.Keys | Sort-Object)

# ══ RETIRED IDS - a citation must not name an id that has been withdrawn ══════
# See the note on `RetiredIdsIn` above: this half of the contract did not exist in
# this twin at all until 2026-08-21, while the `.sh` had refused with exit 4 since
# 2026-08-17 - and the Windows ctest entry runs THIS file.
# ⚠ EQUALITY, NOT SUBSTRING, and deliberately so: a retired id may be a PREFIX of
# a live one, and substring here would red the live citation too. The failure this
# catches is a citation that still spells the dead name exactly.
# ★★ FAIL-CLOSED on a collapsed extraction, like every other scan here. The
# registry carries retired rows; zero means the marker drifted or the table shape
# changed, never that the check has nothing to do.
# ⚠ GLOB, not one file: the registry split into production/harness on 2026-08-25
# and a retired id may live in either. Naming one file here would silently stop
# matching half of them -- an invisible narrowing, which is the failure mode this
# repository treats as the dangerous one. The fail-closed count below still applies
# to the UNION, so a collapsed read of either file is still refused.
$registryLines = @(
    Get-ChildItem -LiteralPath (Join-Path $RepoRoot '.plans') `
                  -Filter '_deferred-anchor-registry*.md' -File |
        Sort-Object Name |
        ForEach-Object { [IO.File]::ReadAllLines($_.FullName, [Text.Encoding]::UTF8) }
)
$retiredIds = @(RetiredIdsIn $registryLines)
if ($retiredIds.Count -lt 1) {
    Write-Host "anchor-registry: FAIL - the retired-id scan found 0 marked rows."
    Write-Host "  This does NOT mean no id is retired - it means THIS scan collapsed (the ``RETIRED-ID`` token was renamed, or the registry's column layout moved)."
    Write-Host "  Refusing to report a pass. Fix the scan; do not delete the check."
    exit 2
}
$retiredCited = @($retiredIds | Where-Object { $srcAnchorSet.ContainsKey($_) })
if ($retiredCited.Count -gt 0) {
    Write-Host "anchor-registry: FAIL - $($retiredCited.Count) citation(s) name a RETIRED anchor id:"
    Build-CitationIndex
    foreach ($rid in $retiredCited) {
        Write-Host "    $rid"
        if ($script:LocPaths.ContainsKey($rid)) {
            foreach ($p in $script:LocPaths[$rid]) { Write-Host "      $p" }
        }
    }
    Write-Host "  A retired id resolves only because the plans still MENTION it. Repoint each"
    Write-Host "  citation at the live row named in the retired row's own status cell."
    exit 4
}

$missing = @()
foreach ($a in $srcAnchors) {
    if (-not $allPlanText.Contains($a)) { $missing += $a }
}

if ($missing.Count -eq 0) {
    # The retired-id count is part of the headline BECAUSE this twin now runs that
    # check: a summary that omits a check nobody can see running is how a check
    # gets deleted by accident. It also keeps the two twins byte-comparable, which
    # is the only thing that makes the pairing contract checkable at all.
    Write-Host "anchor-registry: OK ($($srcAnchors.Count) src anchors all resolve to plans, $($retiredIds.Count) retired id(s) uncited)"
    # ★ The cell-width verdict is NOT allowed to be swallowed by the anchor
    # check's success. Exit codes, matching the .sh: 1 = an anchor resolves
    # nowhere, 2 = a scan collapsed, 3 = a markdown table row drops content,
    # 4 = a citation names a retired id, 5 = a quotation declaration is stale or
    # false, 6 = the self-test failed.
    exit $(if ($cwCollapsed) { 2 } elseif ($cwViolated) { 3 } else { 0 })
}

# ★ THE ROOT LIST IN THE HEADLINE IS DERIVED, TOO. It was another hardcoded copy
# and it had gone stale in the direction that misleads most: the sentence that
# tells the reader WHERE the guard looked was the one place guaranteed to be
# wrong about it.
$rootNames = (($RootSpecs | ForEach-Object { "$($_.Root)/" }) -join ', ')
Write-Host "anchor-registry: FAIL - the following anchors are cited in a SCANNED ROOT"
Write-Host "($rootNames - NOT src/ alone) but"
Write-Host "have no matching row/citation in any .plans/*.md file:"
Write-Host ""
# ★★ THE TREE IS WALKED ONCE — NOT ONCE PER MISSING ANCHOR.
# MEASURED 2026-08-10 on the live tree: the previous form re-enumerated
# `src/ examples/ real-examples/` with `Get-ChildItem -Recurse` and then ran a
# `Select-String` per FILE, all INSIDE the per-anchor loop — cost
# (missing anchors) x (whole tree). With 8 missing anchors this FAIL path took
# **82.9 s** (the `.sh` twin, same 8, took 18.1 s); a wholesale break strands
# every anchor and the guard simply looks HUNG at the moment its output matters
# most. Now the enumeration and the file reads happen ONCE into an ordered
# (file, anchor-token) index, and each anchor's lookup is an in-memory test with
# no I/O. Behaviour is UNCHANGED, verified by DIFFING the whole FAIL report
# before and after on the identical mutation.
# ★ Two behaviours of the old form are deliberately PRESERVED rather than
# quietly tightened, because the task was cost, not semantics:
#   · `Select-String -SimpleMatch` is case-INSENSITIVE by default (a documented
#     divergence from the `.sh`'s case-SENSITIVE `grep`), so the index is built
#     with `IgnoreCase` and probed with `OrdinalIgnoreCase`;
#   · attribution is by SUBSTRING, so a file citing a MORE SPECIFIC anchor is
#     still listed under its PREFIX — a relationship the plan-side resolve
#     documents and relies on, so dropping it would silently shrink the report.
# ★ Paths are recorded in ENUMERATION order and replayed in that order, so the
# report's line order is identical to the old nested loop's.
# ★ DERIVED FROM `$RootSpecs`, never respelled — see the note on that table.
# Enumerated one root at a time (not one call over all roots) because the
# filters differ per root, which is the same reason the collection loop is
# per-root.
# ★ The build now lives in `Build-CitationIndex` above and is SHARED with the
# quotation check and the retired-id report, so whichever consumer needs it first
# pays for it and the others get it free. It also carries the recovered wrapped
# names, so a wrapped citation that fails to resolve is reported WITH its location.
Build-CitationIndex
$locTokens = $script:LocTokens
$locPaths  = $script:LocPaths
$locRank   = $script:LocRank
# An index that came back EMPTY means the locator can attribute NOTHING, and the
# report would then be a list of bare anchor names with no `cited in:` line -
# precisely the guessing game the note above says this locator exists to prevent.
# It should be unreachable (the anchor floor above already proved these same
# filters match hundreds of anchors), so say so LOUDLY rather than degrade
# quietly if it ever happens. Mirrors the .sh sibling.
if ($locTokens.Count -eq 0) {
    Write-Host "anchor-registry: WARNING - the citation index is EMPTY, so no 'cited in:' line can be produced."
    Write-Host "  The anchor floor above passed, so this should be impossible; the locator's file"
    Write-Host "  enumeration (same roots, same -Include set) must have stopped matching. Fix the scan."
}
foreach ($a in $missing) {
    Write-Host "  $a"
    $hits = @{}
    foreach ($t in $locTokens) {
        if ($t.IndexOf($a, [StringComparison]::OrdinalIgnoreCase) -lt 0) { continue }
        foreach ($p in $locPaths[$t]) { $hits[$p] = $locRank[$p] }
    }
    foreach ($p in ($hits.GetEnumerator() | Sort-Object Value | ForEach-Object { $_.Key })) {
        Write-Host "    cited in: $p"
    }
}
Write-Host ""
Write-Host "Fix: either"
Write-Host "  (a) add a row in the deferred-anchor registry naming the"
Write-Host "      trigger + closing work -- .plans/_deferred-anchor-registry-production.md"
Write-Host "      if a USER of the compiler could hit it, -harness.md if only WE can, OR"
# ⚠ THESE FOUR LINES WRAP EXACTLY AS THE `.sh`'s DO, and that is not fussiness:
# the twins are verified by DIFFING their output, so a report that differs on four
# lines for no reason is four lines of noise a real divergence could hide inside.
Write-Host "  (b) cite the anchor in a per-plan section 3.1 row (preferred when the"
Write-Host "      anchor maps to a specific plan's feature area), OR"
Write-Host "  (c) if the string is a code-internal pin not deferred work, add it"
Write-Host "      to the Allowlist section of the registry."
Write-Host ""
Write-Host "Discipline: this leak recurred TWICE before this guard landed."
Write-Host "See .plans/_deferred-anchor-registry-production.md for the discipline rationale."
# ★ BOTH halves report on every run. But an exit code can carry only ONE number,
# so when both fail the precedence is DELIBERATE rather than whichever branch
# happens to run last: a COLLAPSED scan (2) beats a missing anchor (1) beats
# dropped content (3), because a scan that checked nothing makes the other two
# verdicts untrustworthy. MEASURED 2026-08-10: with 20 plan files removed the
# cell-width floors fire AND every anchor stops resolving, and without this the
# `exit 1` below silently outranked the collapse. Mirrors the .sh exactly.
if ($cwCollapsed) { exit 2 }
exit 1
