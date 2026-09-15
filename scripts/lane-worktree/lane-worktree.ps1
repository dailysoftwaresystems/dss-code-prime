<#
PURPOSE: create and remove lane worktrees inside the ignored .worktrees/, refusing any root that would exceed Windows MAX_PATH.

★ THE POWERSHELL HALF OF scripts/lane-worktree/. It exists because the ROOT HOST IS
  WINDOWS and lanes are spawned from PowerShell: a bash-only owner means the first
  caller reaches for `git worktree add` directly, which is the exact erosion the
  ONE-OWNER rule exists to prevent. Behaviour, exit codes and refusal messages are
  the same as `lane-worktree.sh`; see that file's header for the full rationale and
  the operator ruling (2026-08-26) that placed worktrees inside the repository root.

⚠⚠ AND "THE SAME AS lane-worktree.sh" WAS A CLAIM THIS FILE MADE WITHOUT BEING TRUE.
  [[D-CYCLE-LANE-WORKTREE-REMOVE-DISCARDS-AN-UNPRESERVED-SCRATCHPAD]]
  ✔MEASURED 2026-09-07 (cycle P63): THREE separate fixes, each recorded as CLOSED in
  the registry, had landed in `lane-worktree.sh` ONLY, while the paragraph above went
  on promising parity:
    * the SCRATCHPAD GATE (P50) -- `remove` here deleted a lane's evidence tree with
      no check of any kind, which is verbatim the defect whose row reads ✅ CLOSED;
    * REMOVE-THEN-VERIFY-THEN-SPEAK (P46) -- `remove` here printed
      "removed <rel> and pruned stale registrations" without ever looking, which is
      the exact sentence P46 measured being printed over 4.4 GB still on disk;
    * the SEED-MANIFEST RESET (P57) -- `add` here left a stale
      `.worktrees/.manifests/seed-<name>.json` in place, and a stale entry that
      happens to equal a lane's file makes `lane-fold` SILENTLY DROP that lane's real
      work.
  ★ N implementations of one behaviour are N places to fix, and fixing one leaves the
  other wrong while the ROW READS DONE -- [[feedback-a-partial-fix-reads-as-a-complete-one]]
  at the twin level. ⇒ Anything changed in either file lands in BOTH, in the same
  commit, and `test-lane-worktree.sh` now drives BOTH so the claim is measured rather
  than asserted.
  ⓘ P66 changed both twins together: the evidence gate covers `scratchpad/` AND `.temp/`,
  the preserve refuses a destination inside the worktree or one already holding a
  same-named file and verifies every file, `-DiscardScratchpad` is retired in favour of
  `-DiscardEvidence`, and `add` records the lane's base commit in its manifest.

⚠ THE MAX_PATH PREFLIGHT MATTERS MORE ON THIS SIDE, NOT LESS. MAX_PATH IS A WINDOWS
  LIMIT, AND THIS IS THE WINDOWS ENTRY POINT -- the anchored defect
  `D-CYCLE-WORKTREE-UNDER-THE-SESSION-SCRATCH-PATH-CANNOT-BE-BUILT-ON-WINDOWS` was
  measured on exactly this host. Moving worktrees from a 10-char root into the
  repository root spends 46 characters of that budget, so the check is arithmetic
  performed BEFORE a path is handed back, not a red discovered mid-build in files
  the lane never touched.

Exit codes: 0 OK - 2 not a repository / git refused / a path that cannot be resolved
            - 3 MAX_PATH would be breached - 4 .worktrees/ is not ignored - 5 usage
            - 6 the worktree is STILL ON DISK after remove, prune and delete
            - 7 EVIDENCE WOULD BE LOST: an evidence root (scratchpad/ or .temp/) holds
              files and no decision was given, or a preserve could not be proved -- a
              destination inside the worktree, one already holding a same-named file
              with other bytes, a failed copy, or a file that does not re-read identical
              (see `Invoke-Remove`)
            - 8 THE LANE'S WORK WOULD BE LOST: the worktree's own git status lists a
              tracked modification or an untracked file that is not ignored, or its HEAD
              holds a commit that no ref of the repository reaches -- or either cannot be
              read -- and -DiscardWork was not given.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Verb,
    [Parameter(Position = 1)][string]$Name,
    [Parameter(Position = 2)][string]$Committish = 'HEAD',
    # The tree to act on. DEFAULTS to the tree this script lives in -- never the
    # caller's cwd. See `Get-RepoRoot` for the row and the measurement.
    [string]$Repo,
    # `remove` only, and they contradict each other by design. The `.sh` twin spells
    # them `--preserve-to <dir>` and `--discard-evidence`; the behaviour, the refusals
    # and the exit codes are the same on both sides.
    [string]$PreserveTo,
    [switch]$DiscardEvidence,
    # `remove` only: delete the worktree although its own `git status` lists uncommitted work, or
    # its HEAD holds commits no ref of the repository reaches; it names what it discards.
    # The `.sh` twin spells it `--discard-work`. See `Invoke-Remove`.
    [switch]$DiscardWork,
    # RETIRED. Declared only so that passing it reaches this script's own refusal (exit 5,
    # naming its replacement) instead of a parameter-binding error. See `Invoke-Remove`.
    [switch]$DiscardScratchpad
)

Set-StrictMode -Version Latest

# ★★ THE `.ps1` OWNER OF "WHICH TREE CONTAINS THIS PATH?", REUSED RATHER THAN RESPELT.
# ⓘ `repo-tree.ps1` guards its own dispatch on `$MyInvocation.InvocationName -ne '.'`,
# so dot-sourcing defines and does nothing else. It also sets
# `$ErrorActionPreference = 'Stop'` in this scope; that is harmless for the `git`
# calls below because `$PSNativeCommandUseErrorActionPreference` is False (✔MEASURED
# 2026-09-02, pwsh 7.5.2), so a native command's stderr still does not throw and the
# `$LASTEXITCODE` checks in this file keep their meaning.
. (Join-Path $PSScriptRoot '..\repo-tree\repo-tree.ps1')

$MAX_PATH = 260
# The longest build-relative suffix a worktree is expected to generate. MEASURED
# 2026-08-26 inside a live lane worktree, not guessed. Raise it by MEASURING.
$WORST_SUFFIX = 163
# Refuse a root that only just fits: this repository's test names dominate that
# suffix and keep growing, and the margin is what protects the next long one.
$MARGIN = 20
# The top-level directories a lane keeps its EVIDENCE in -- the `.sh` twin's
# `EVIDENCE_ROOTS`, and the measurement behind the pair is in that file's `cmd_remove`.
$EVIDENCE_ROOTS = @('scratchpad', '.temp')

function Say  { param([string]$m) Write-Host "lane-worktree: $m" }
function Die  {
    param([int]$Code, [string[]]$Lines)
    foreach ($l in $Lines) { [Console]::Error.WriteLine("lane-worktree: $l") }
    exit $Code
}
# `git rev-list --oneline` lines -> indented lines: the first 10, then a count of the rest. Handed
# to `Say` or `Die`, they print exactly as the `.sh` twin's `_lw_list_commits` does.
function Format-CommitList {
    param([string[]]$Lines)
    $Lines | Select-Object -First 10 | ForEach-Object { "  $_" }
    if ($Lines.Count -gt 10) { "  ... and $($Lines.Count - 10) more" }
}

# ★★★ WHICH TREE THIS VERB IS ABOUT, AND THE ANSWER IS NOT "WHERE AM I STANDING".
# [[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]]
#
# This was a bare `git rev-parse --show-toplevel`, which answers "what repository is
# my CALLER'S SHELL in?" -- so every path built from it was rooted at whichever
# repository somebody happened to have cd'd into. ✔MEASURED 2026-09-02, driving this
# file out of `.worktrees/lw` from a throwaway repository outside the checkout:
# `list` reported the THROWAWAY repository's `.worktrees/`.
#
# ★ THE QUESTION IS "WHICH TREE DOES MY OWN FILE BELONG TO?" -- `$PSCommandPath`, not
# `$PWD`. The full reasoning, including the MAIN-CHECKOUT answer that was measured and
# REJECTED because from a lane it resolves `remove <sibling>` onto a live sibling
# lane's uncommitted work, is in `lane-worktree.sh`'s `_repo_root`; the two halves of
# this owner must keep the same answer, so it is stated once there and cited here.
# `-Repo <path>` is the explicit way to mean another tree.
function Get-RepoRoot {
    $anchor = if (-not [string]::IsNullOrWhiteSpace($Repo)) { $Repo } else { $PSCommandPath }
    try {
        return (Get-RepoTreeOwningRoot $anchor)
    } catch {
        if (-not [string]::IsNullOrWhiteSpace($Repo)) {
            Die 2 @("--repo '$Repo' is not inside a git working tree: $($_.Exception.Message)")
        }
        Die 2 @(
            'not inside a git repository -- cannot place a lane worktree.',
            "This script resolves the tree IT LIVES IN ($PSCommandPath), never the caller's",
            'cwd; pass -Repo <path> to name a different tree deliberately.',
            $_.Exception.Message
        )
    }
}

# `.worktrees/` must be IGNORED, and it is CHECKED rather than assumed: that one
# rule is what keeps N full checkouts off every gate host, because the carriages
# derive their exclude list from git (scripts/carriage-excludes/).
# ⚠ The trailing slash is required -- `git check-ignore .worktrees` answers
#   NOT-IGNORED for a directory that does not exist yet, while `.worktrees/`
#   answers correctly. ✔MEASURED 2026-08-26, both spellings, absent directory.
function Assert-Ignored {
    param([string]$Repo)
    git -C $Repo check-ignore -q -- '.worktrees/' 2>$null
    if ($LASTEXITCODE -ne 0) {
        Die 4 @(
            '.worktrees/ is NOT ignored by git.',
            'A lane worktree there would be committed, and -- worse -- would ride the',
            'carriage to every gate host, where the examples runner globs examples/<lang>/*',
            "and would run somebody's uncommitted corpus as if it were the cycle's.",
            "Restore the '/.worktrees/' rule in .gitignore before creating any worktree."
        )
    }
}

function Assert-PathBudget {
    param([string]$Root)
    $len   = $Root.Length
    $total = $len + $WORST_SUFFIX
    if (($total + $MARGIN) -gt $MAX_PATH) {
        Die 3 @(
            "REFUSING: '$Root' is $len chars; + $WORST_SUFFIX for the longest build path",
            "= $total, leaving $($MAX_PATH - $total) under MAX_PATH ($MAX_PATH), below the",
            "required margin of $MARGIN.",
            'This is D-CYCLE-WORKTREE-UNDER-THE-SESSION-SCRATCH-PATH-CANNOT-BE-BUILT-ON-WINDOWS.',
            'It would NOT fail as a link error -- it fails as a per-TU compile error in files',
            "you never touched, and reads as somebody else's breakage. Use a shorter lane name."
        )
    }
    Say "path budget OK: root=$len + suffix=$WORST_SUFFIX = $total ($($MAX_PATH - $total) spare)"
}

# The `.sh` twin's `find <dir> -type f | wc -l`, spelt once. `-File` is the `-type f`
# half and matters: an empty directory TREE must count zero, or the control case
# ("an empty scratchpad needs no flag") would refuse.
function Get-FileCount {
    param([string]$Dir)
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return 0 }
    return @(Get-ChildItem -LiteralPath $Dir -Recurse -File -Force -ErrorAction SilentlyContinue).Count
}

# A single path component, never `..`, never a dotted name. BOTH verbs check this and
# the `.sh` twin says why: harmless while the only verb was `git worktree remove`,
# which simply declines an unknown path, and NOT harmless the moment a recursive
# delete stands behind it. `remove ../..` must never resolve anywhere.
function Assert-LaneName {
    param([string]$LaneName)
    if ($LaneName -match '[\\/]' -or $LaneName.StartsWith('.')) {
        Die 5 @("lane name must be a single path component and must not start with '.': '$LaneName'")
    }
}

function Invoke-Add {
    if ([string]::IsNullOrWhiteSpace($Name)) {
        Die 5 @('usage: lane-worktree.ps1 add <name> [committish]')
    }
    Assert-LaneName $Name
    $repo = Get-RepoRoot
    Assert-Ignored $repo
    $rel = ".worktrees/$Name"
    $abs = "$repo/$rel"
    Assert-PathBudget $abs
    if (Test-Path -LiteralPath $abs) {
        Die 5 @("'$rel' already exists -- remove it first, or pick another name.")
    }
    git -C $repo worktree add --detach $rel $Committish | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Die 2 @("git worktree add failed for '$rel' at '$Committish'.")
    }

    # ⚠⚠ RESET THIS LANE NAME'S SEED MANIFEST, AND IT IS A CORRECTNESS FIX RATHER THAN
    #    TIDINESS. `scripts/lane-fold/lane-fold.py` adjudicates a fold as
    #      (the lane's `git status` set) MINUS (seeded paths whose md5 is UNCHANGED),
    #    reading `.worktrees/.manifests/seed-<name>.json`. That file is keyed by LANE
    #    NAME ONLY -- it carries no cycle and no commit -- and lane names here are two
    #    letters, so they are reused constantly.
    #    ✔MEASURED 2026-09-03 (cycle P57): four worktrees were created with this verb on
    #    a clean tree, and ALL FOUR silently inherited manifests written days earlier by
    #    lanes of the same name; one held 82 entries of which 37 disagreed with the main
    #    tree, including files no lane that cycle touched.
    #    ★ THE FALSE REFUSAL IS THE CHEAP FAILURE. The expensive one is the other
    #    direction: a stale entry that HAPPENS to equal the lane's own file marks real
    #    lane work as "untouched seed" and the fold SILENTLY DROPS IT -- a lane's whole
    #    change vanishing while every report reads clean, which is the class this
    #    project treats as worst.
    #    ⇒ A worktree created here is a checkout of a COMMIT and carries no uncommitted
    #    work, so its honest manifest holds NO paths. An orchestrator that then seeds
    #    uncommitted work in re-writes it via `lane-fold.py seed <lane>`, the only other
    #    writer.
    #    ⚠ THIS LANDED IN THE `.sh` TWIN ONLY UNTIL P63 (see this file's header): the
    #    Windows entry point left the stale manifest in place, so the fold defect above
    #    was reachable through it the whole time.
    # ⚠⚠ AND THE MANIFEST RECORDS THE COMMIT THE LANE WAS CREATED AT (lane-fold's manifest
    #    format 2): a fold measures every unseeded path against the blob at the LANE'S OWN
    #    base, and refuses a lane whose HEAD has moved away from it. The `.sh` twin's
    #    `cmd_add` carries the reproduction that made it necessary.
    $base = git -C $abs rev-parse --verify HEAD 2>$null
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace([string]$base)) {
        Die 2 @("could not read the new worktree's HEAD, so '$Name' has no base commit to record.")
    }
    $base = ([string]$base).Trim()
    $manifestDir = Join-Path $repo '.worktrees/.manifests'
    try {
        if (-not (Test-Path -LiteralPath $manifestDir -PathType Container)) {
            New-Item -ItemType Directory -Path $manifestDir -Force | Out-Null
        }
        # ⓘ No newline and no BOM: `lane-fold.py` parses this as JSON, and the `.sh` twin
        # writes exactly this string with `printf`. The two writers must agree byte for byte.
        [IO.File]::WriteAllText((Join-Path $manifestDir "seed-$Name.json"),
                                ('{"base":"' + $base + '","format":2,"paths":{}}'),
                                (New-Object System.Text.UTF8Encoding($false)))
    } catch {
        Die 2 @("could not reset the seed manifest for '$Name': $($_.Exception.Message)")
    }

    $short = (git -C $abs rev-parse --short HEAD).Trim()
    Say "created $rel at $short"
    Say 'seed manifest reset: base recorded, no seeded paths (this lane starts from the commit, not from uncommitted work)'
    Say "build into $rel/build/$Name -- never into the main tree's build/."
    # ⚠ THE FIRST BUILD OF A FRESH TREE NEEDS --build-type, AND THIS LINE EXISTS
    #    BECAUSE THE ORCHESTRATOR KEPT WRITING BRIEFS THAT OMITTED IT.
    #    `local-build.sh` maps only `dbg` -> Debug and `rel` -> Release, and REFUSES
    #    (rc 3) to guess one for any other tree name. That refusal is CORRECT and is
    #    not to be softened: guessing Debug for an unknown lane would hand a
    #    Release-intending caller a Debug tree silently, which is the
    #    fails-to-WRONG-ANSWER direction. ✔MEASURED (P63): a lane hit the rc 3, and
    #    the brief that sent it there had never been run by its author --
    #    [[D-CYCLE-BRIEF-STATED-AN-INVOCATION-ITS-AUTHOR-HAD-NEVER-RUN]].
    #    ⇒ The command belongs where the lane name is KNOWN, which is here.
    Say "FIRST build of this tree:  DSS_JOBS=6 bash scripts/local-build/local-build.sh --tree $Name --build-type Debug"
    Say "        every build after:  DSS_JOBS=6 bash scripts/local-build/local-build.sh --tree $Name"
    Write-Output $abs
}

# The `.sh` twin's `_lw_preserve`: copies every file under every evidence root to
# <Destination>\<root>\<same path> and REFUSES (exit 7) on anything that would lose
# evidence. It deletes nothing, ever. The refusals, their order and their exit code are
# the twin's; the per-file proof here is size plus SHA-256 where the `.sh` uses size plus
# CRC -- one property (every file re-reads identical), each shell's own instrument.
function Invoke-PreserveEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$WorktreeAbs,
        [Parameter(Mandatory = $true)][string]$Rel,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][int]$Counted
    )
    # ★ The resolver is `repo-tree.ps1`'s, dot-sourced above -- ONE OWNER of "what does this
    # path really point at". A missing owner is a refusal, never a silent fallback.
    if (-not (Get-Command Resolve-RepoTreeRealPath -ErrorAction SilentlyContinue)) {
        Die 7 @("cannot prove a preserve: repo-tree.ps1's path resolver is not available. Nothing was copied or removed.")
    }
    # A relative -PreserveTo means relative to THIS SESSION'S location, which .NET's own
    # path functions do not track -- so it is made absolute through PowerShell first.
    $destFull = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
    try {
        [void][IO.Directory]::CreateDirectory($destFull)
    } catch {
        Die 7 @("could not create '$Destination': $($_.Exception.Message); nothing was removed.")
    }
    $sep      = [IO.Path]::DirectorySeparatorChar
    $destReal = ([string](Resolve-RepoTreeRealPath $destFull)).TrimEnd($sep)
    $wtReal   = ([string](Resolve-RepoTreeRealPath $WorktreeAbs)).TrimEnd($sep)
    # ⚠ A DESTINATION INSIDE THE TREE ABOUT TO BE DELETED IS A COPY INTO NOWHERE.
    if (($destReal + $sep).StartsWith($wtReal + $sep, [StringComparison]::OrdinalIgnoreCase)) {
        Die 7 @(
            "REFUSING: -PreserveTo '$Destination' resolves to '$destReal', which is INSIDE '$Rel'.",
            'This verb is about to delete that tree, so the copy would be destroyed with the',
            'evidence it was taken from -- after being reported as verified. Nothing was removed.')
    }

    # ONE list, taken ONCE: the count, the clash check, the copy and the verify all walk it.
    $files = New-Object 'System.Collections.Generic.List[object]'
    try {
        foreach ($r in $EVIDENCE_ROOTS) {
            $rootDir = [IO.Path]::GetFullPath((Join-Path $WorktreeAbs $r))
            if (-not [IO.Directory]::Exists($rootDir)) { continue }
            $prefix = $rootDir.TrimEnd($sep) + $sep
            foreach ($f in Get-ChildItem -LiteralPath $rootDir -Recurse -File -Force -ErrorAction Stop) {
                if (-not $f.FullName.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                    throw "'$($f.FullName)' does not sit under '$prefix'"
                }
                $sub = $f.FullName.Substring($prefix.Length)
                $files.Add([pscustomobject]@{
                    Rel    = ($r + '/' + ($sub -replace '\\', '/'))
                    Src    = $f.FullName
                    Dst    = [IO.Path]::Combine($destReal, $r, $sub)
                    Length = $f.Length
                    Hash   = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256 -ErrorAction Stop).Hash
                })
            }
        }
    } catch {
        Die 7 @("could not re-read the evidence under '$Rel': $($_.Exception.Message)",
                'Nothing was copied or removed.')
    }
    if ($files.Count -ne $Counted) {
        Die 7 @("counted $Counted evidence file(s) under '$Rel' but re-read $($files.Count): the tree changed",
                'underneath this verb. Nothing was copied or removed.')
    }

    # ⚠ A FILE ALREADY AT THE DESTINATION UNDER THE SAME PATH, WITH OTHER BYTES, IS
    #   SOMEBODY ELSE'S EVIDENCE -- refused before anything is copied.
    $clash = @()
    foreach ($e in $files) {
        if ([IO.File]::Exists($e.Dst)) {
            $len = ([IO.FileInfo]::new($e.Dst)).Length
            if ($len -ne $e.Length -or (Get-FileHash -LiteralPath $e.Dst -Algorithm SHA256).Hash -ne $e.Hash) {
                $clash += $e.Rel
            }
        }
    }
    if ($clash.Count -gt 0) {
        Die 7 (@(
            "REFUSING: '$destReal' already holds a file at the same path with DIFFERENT bytes, so",
            'copying would silently OVERWRITE evidence that is already there. Nothing was copied and',
            'nothing was removed; preserve into a fresh directory. First clash(es):') +
            @($clash | Select-Object -First 5))
    }

    # File by file, to the exact path the list names: no wildcard, no Copy-Item nesting rule,
    # and each root lands under its own name, as the `.sh` twin's `cp -R <root>/.` does.
    try {
        foreach ($e in $files) {
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($e.Dst))
            [IO.File]::Copy($e.Src, $e.Dst, $true)
        }
    } catch {
        Die 7 @("copy of the evidence under '$Rel' -> '$destReal' FAILED; nothing was removed: $($_.Exception.Message)")
    }

    # ⚠ VERIFY EVERY FILE, NEVER A COUNT.
    $bad = @()
    foreach ($e in $files) {
        if (-not [IO.File]::Exists($e.Dst)) { $bad += "$($e.Rel) (absent)"; continue }
        $len = ([IO.FileInfo]::new($e.Dst)).Length
        if ($len -ne $e.Length) { $bad += "$($e.Rel) (size $len, expected $($e.Length))"; continue }
        if ((Get-FileHash -LiteralPath $e.Dst -Algorithm SHA256).Hash -ne $e.Hash) { $bad += "$($e.Rel) (content differs)" }
    }
    if ($bad.Count -gt 0) {
        Die 7 (@(
            "preserve VERIFY FAILED: the $($files.Count) evidence file(s) under '$Rel' do not all re-read",
            "identical (size and SHA-256, file by file) under '$destReal':") +
            @($bad | Select-Object -First 5) +
            @("REFUSING to remove '$Rel' -- the evidence would be lost."))
    }
    Say "preserved $($files.Count) evidence file(s) -> $destReal (every file re-read at the destination: size and SHA-256 match)"
}

# ⚠⚠ A LANE'S EVIDENCE IS WHAT ITS ROW CITES, AND THIS VERB USED TO DELETE IT WITHOUT
#    ASKING. [[D-CYCLE-LANE-WORKTREE-REMOVE-DISCARDS-AN-UNPRESERVED-SCRATCHPAD]]
#    ✔MEASURED 2026-09-01 (cycle P50): the orchestrator ran a `cp ... && echo preserved`
#    followed by `lane-worktree.sh remove t2` on ONE command line. The destination's
#    parent did not exist, `cp` failed, `&& echo` printed nothing, and the ABSENCE of
#    output read as "fine" -- then the very next command destroyed the only copy. Lane
#    `t2`'s 14 result JSONs and its md5 ledger are gone, and the row that cites them had
#    to be amended to admit it. ★ THE PRESERVE STEP WAS A CONVENTION LIVING IN THE
#    ORCHESTRATOR'S HEAD, and a rule with no teeth at the moment of the decision erodes.
#    ⇒ the tool owns it: a lane whose evidence roots hold files cannot be removed silently.
#    ⚠⚠ AND UNTIL P63 THAT SENTENCE WAS TRUE OF THE `.sh` TWIN ONLY. This function had
#    NO gate at all while the row read ✅ CLOSED. See this file's header.
#    ⚠⚠ AND UNTIL P66 BOTH TWINS GATED ONLY `scratchpad/`, while every live lane kept its
#    evidence under `.temp/<lane>-scratch/` -- the `.sh` twin's `cmd_remove` carries the
#    measurement (26,466 evidence files across five lanes, all ungated) and the two
#    reproduced ways the preserve itself could lose what it had just "verified".
function Invoke-Remove {
    if ([string]::IsNullOrWhiteSpace($Name)) {
        Die 5 @('usage: lane-worktree.ps1 remove <name> [-DiscardWork] [-PreserveTo <dir> | -DiscardEvidence]')
    }
    if ($DiscardScratchpad.IsPresent) {
        # ⚠ RETIRED, AND REFUSED RATHER THAN KEPT AS AN ALIAS: honouring it now would
        #   silently WIDEN a destructive flag to `.temp/` as well.
        Die 5 @(
            "-DiscardScratchpad is retired: the gate now covers every evidence root ($($EVIDENCE_ROOTS -join ' ')),",
            'so a flag named for one of them would silently discard the others.',
            'Say which you mean: -DiscardEvidence deletes ALL of it; -PreserveTo <dir> keeps it.')
    }
    if ($DiscardEvidence.IsPresent -and -not [string]::IsNullOrWhiteSpace($PreserveTo)) {
        Die 5 @('-PreserveTo and -DiscardEvidence contradict each other; pick one.')
    }
    Assert-LaneName $Name
    $repo = Get-RepoRoot
    $rel  = ".worktrees/$Name"
    $abs  = "$repo/$rel"

    # ── THE WORK GATE, FIRST ────────────────────────────────────────────────────
    # The `.sh` twin's `cmd_remove` carries the reproductions and the reasoning. This verb asks
    # only (1) whether the worktree's OWN `git status` lists a tracked modification or an
    # untracked file that is not ignored, and (2) which commits its HEAD holds that no ref of
    # the repository reaches -- `rev-list <HEAD> --not --glob=refs/*`, asked AT THE REPOSITORY
    # ROOT -- never whether that work is folded, which is `lane-fold.py`'s measurement. A status
    # that cannot be read at the worktree's own root (`--show-prefix` not empty: git walked up to
    # a parent repository), and a commit list git cannot produce, are refused too.
    if (Test-Path -LiteralPath $abs) {
        $workReadable = $true
        $workLines = @()
        $commitsReadable = $true
        $commitLines = @()
        $prefix = git --no-optional-locks -C $abs rev-parse --show-prefix 2>$null
        if ($LASTEXITCODE -ne 0 -or -not [string]::IsNullOrEmpty((@($prefix) -join ''))) {
            $workReadable = $false
        }
        if ($workReadable) {
            $workLines = @(git --no-optional-locks -C $abs status --porcelain --untracked-files=all 2>$null |
                           Where-Object { $_ -ne '' })
            if ($LASTEXITCODE -ne 0) { $workReadable = $false }
        }
        if ($workReadable) {
            $workHead = (@(git --no-optional-locks -C $abs rev-parse --verify -q HEAD 2>$null) -join '').Trim()
            if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrEmpty($workHead)) {
                $commitLines = @(git --no-optional-locks -C $repo rev-list --oneline $workHead --not '--glob=refs/*' 2>$null |
                                 Where-Object { $_ -ne '' })
                if ($LASTEXITCODE -ne 0) { $commitsReadable = $false }
            }
        }
        if (-not $workReadable) {
            if (-not $DiscardWork.IsPresent) {
                Die 8 @(
                    "cannot read the git status of '$rel' at its own root, so it cannot be shown to carry no",
                    'uncommitted work (git cannot open it as a working tree there, and asked from inside it',
                    'git answers about a parent repository instead). REFUSING to delete it.',
                    '  pass -DiscardWork to remove it anyway, deliberately.')
            }
            Say "DISCARDING '$rel', whose git status cannot be read at its own root, as instructed"
        }
        elseif (-not $commitsReadable) {
            if (-not $DiscardWork.IsPresent) {
                Die 8 @(
                    "cannot list the commits of '$rel' that no ref of the repository reaches (git rev-list",
                    'failed at the repository root), so it cannot be shown to hold none. REFUSING to delete it.',
                    '  pass -DiscardWork to remove it anyway, deliberately.')
            }
            Say "DISCARDING '$rel', whose commits cannot be listed, as instructed"
        }
        elseif ($workLines.Count -gt 0 -or $commitLines.Count -gt 0) {
            if (-not $DiscardWork.IsPresent) {
                $refusal = @()
                if ($workLines.Count -gt 0) {
                    $refusal += @(
                        "'$rel' carries $($workLines.Count) path(s) of UNCOMMITTED WORK -- tracked modifications, or untracked",
                        'files that are not ignored, by its own git status -- and would be DELETED with them.',
                        'first path(s):') + @($workLines | Select-Object -First 5)
                }
                if ($commitLines.Count -gt 0) {
                    $refusal += @(
                        "'$rel' holds $($commitLines.Count) COMMIT(S) that no branch, tag or other ref of the repository reaches --",
                        "only this worktree's HEAD holds them, and removing it would ORPHAN them:") + @(Format-CommitList $commitLines)
                }
                Die 8 ($refusal + @(
                    "This verb cannot tell whether that work is already folded; that is lane-fold's measurement:",
                    "  python scripts/lane-fold/lane-fold.py land $Name <production|harness> --apply",
                    '    (folds uncommitted work, passes -DiscardWork only after measuring nothing left to fold,',
                    '     and refuses a lane that committed)',
                    '  git branch <branch> <commit>   keeps a commit, so the removal no longer orphans it',
                    '  -DiscardWork                   delete it deliberately'))
            }
            if ($workLines.Count -gt 0) {
                Say "DISCARDING $($workLines.Count) path(s) of uncommitted work under $rel, as instructed"
            }
            if ($commitLines.Count -gt 0) {
                Say "DISCARDING $($commitLines.Count) commit(s) under $rel that no ref of the repository reaches, as instructed:"
                Format-CommitList $commitLines | ForEach-Object { Say $_ }
            }
        }
    }

    # ── THE EVIDENCE GATE, BEFORE ANY DELETION ──────────────────────────────────
    # Counted with files only, so an empty directory tree is correctly "nothing to
    # preserve" and a single file under either root is enough to stop the removal.
    $evTotal = 0
    $evWhere = @()
    foreach ($r in $EVIDENCE_ROOTS) {
        $n = Get-FileCount (Join-Path $abs $r)
        $evTotal += $n
        $evWhere += "$r/=$n"
    }
    $evWhereText = $evWhere -join ' '
    if ($evTotal -gt 0 -and -not $DiscardEvidence.IsPresent -and
        [string]::IsNullOrWhiteSpace($PreserveTo)) {
        Die 7 @(
            "'$rel' holds $evTotal evidence file(s) ($evWhereText) and would be DELETED with them.",
            "A lane's evidence is what its registry row cites -- findings logs, mutant",
            'transcripts, gate logs, row cells. Choose explicitly:',
            '  -PreserveTo <dir>        copy ALL of it there FIRST; every file is re-read and matched',
            '  -DiscardEvidence         delete it deliberately',
            "This gate exists because a hand-rolled 'cp && remove' lost lane t2's evidence in P50,",
            'and because the gate that closed that looked only at scratchpad/ while every P66',
            'lane kept its evidence under .temp/.'
        )
    }
    if ($evTotal -gt 0 -and -not [string]::IsNullOrWhiteSpace($PreserveTo)) {
        Invoke-PreserveEvidence -WorktreeAbs $abs -Rel $rel -Destination $PreserveTo -Counted $evTotal
    }
    elseif ($evTotal -gt 0) {
        Say "DISCARDING $evTotal evidence file(s) under $rel ($evWhereText), as instructed"
    }

    # --force because a lane worktree always carries an ignored build/ tree; without
    # it git refuses and the caller is tempted to Remove-Item -Recurse, which leaves
    # the registration behind in .git/worktrees/ where `git status` NEVER shows it.
    git -C $repo worktree remove --force $rel 2>$null
    if ($LASTEXITCODE -ne 0) {
        Say "worktree remove declined for '$rel' (already gone?) -- pruning anyway"
    }
    git -C $repo worktree prune

    # ⚠⚠ THE GIT VERB CAN DECLINE AND LEAVE THE ENTIRE TREE ON DISK, AND THIS FUNCTION
    # USED TO REPORT SUCCESS ANYWAY. ✔MEASURED 2026-08-31 (cycle P46) on the `.sh` side:
    # lane `cm` was folded mid-flight and its `.git` emptied, so `git worktree remove`
    # could not see it and exited non-zero; control fell through to `prune` and printed
    # "removed .worktrees/cm and pruned stale registrations" over **4.4 GB that was still
    # there**. ★ An instrument reporting a pass over work it did not do is this project's
    # worst class -- and the failure is invisible, because the caller's next `git worktree
    # list` agrees the worktree is gone. ⇒ REMOVE, THEN VERIFY, THEN SPEAK.
    # ⚠ P63: this half was in the `.sh` twin ONLY. Until now THIS file printed the exact
    # sentence P46 measured as a lie, with nothing between it and the disk.
    if (Test-Path -LiteralPath $abs) {
        # Belt and braces over the name check: resolve both sides and require the target
        # to sit STRICTLY inside the repository's .worktrees/. A link is the one way a
        # single-component name could still land elsewhere.
        # ★ The resolver is `repo-tree.ps1`'s, dot-sourced above -- ONE OWNER of "what
        # does this path really point at", including the ANCESTOR-symlink case a bare
        # GetFullPath cannot see. A missing owner is a refusal, never a silent fallback.
        if (-not (Get-Command Resolve-RepoTreeRealPath -ErrorAction SilentlyContinue)) {
            Die 2 @(
                "refusing to delete '$abs': repo-tree.ps1's path resolver is not available,",
                'so containment inside .worktrees/ cannot be proved.')
        }
        $real      = Resolve-RepoTreeRealPath $abs
        $container = Resolve-RepoTreeRealPath (Join-Path $repo '.worktrees')
        if ([string]::IsNullOrWhiteSpace($real) -or [string]::IsNullOrWhiteSpace($container)) {
            Die 2 @("refusing to delete '$abs': could not resolve it or its container.")
        }
        $sep = [IO.Path]::DirectorySeparatorChar
        $prefix = $container.TrimEnd($sep) + $sep
        if ($real.Length -le $prefix.Length -or -not $real.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            Die 2 @(
                "refusing to delete '$abs': it resolves to '$real', which is not",
                "strictly inside '$container'.")
        }
        Remove-Item -LiteralPath $abs -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $abs) {
        Die 6 @(
            "'$rel' is STILL ON DISK after worktree-remove, prune and a recursive delete.",
            'REFUSING to report success over work that did not happen.',
            'A locked file is the likely cause -- a stalled ctest holding libdsscp.dll',
            'is this repository''s known instance. Close it and re-run.'
        )
    }
    git -C $repo worktree prune
    # Drop the container only when WE emptied it; never disturb a sibling lane's.
    $container = Join-Path $repo '.worktrees'
    if ((Test-Path -LiteralPath $container) -and
        -not (Get-ChildItem -LiteralPath $container -Force)) {
        Remove-Item -LiteralPath $container -Force
        Say 'removed the now-empty .worktrees/'
    }
    Say "removed $rel (VERIFIED absent) and pruned stale registrations"
}

function Invoke-List {
    $repo = Get-RepoRoot
    Say 'registered worktrees:'
    git -C $repo worktree list | ForEach-Object { "  $_" }
    $container = Join-Path $repo '.worktrees'
    if (Test-Path -LiteralPath $container) {
        Say 'under .worktrees/:'
        foreach ($d in Get-ChildItem -LiteralPath $container -Directory -Force) {
            # ⚠ LEADING-DOT ENTRIES ARE NOT LANES, AND `-Force` REACHES THEM WHILE THE
            # `.sh` TWIN'S `"$repo"/.worktrees/*/` GLOB DOES NOT. ✔MEASURED 2026-09-07
            # (P63) against the live tree: the two halves of this owner disagreed, and
            # this one listed `.worktrees/.manifests` -- `lane-fold`'s seed bookkeeping --
            # as a removable lane with 238 files in it. Filtering on the leading dot is
            # exactly the glob's rule, so the two now answer identically.
            if ($d.Name.StartsWith('.')) { continue }
            $count = (Get-ChildItem -LiteralPath $d.FullName -Recurse -File -Force -ErrorAction SilentlyContinue |
                      Measure-Object).Count
            $spare = $MAX_PATH - $d.FullName.Length - $WORST_SUFFIX
            '  {0,-50} {1} files, {2} spare under MAX_PATH' -f ".worktrees/$($d.Name)", $count, $spare
        }
    }
    else {
        Say 'under .worktrees/: (absent -- no lane worktrees)'
    }
}

switch ($Verb) {
    'add'    { Invoke-Add }
    'remove' { Invoke-Remove }
    'list'   { Invoke-List }
    default  { Die 5 @(
        'usage: lane-worktree.ps1 [-Repo <path>]',
        '                         {add <name> [committish] |',
        '                          remove <name> [-DiscardWork] [-PreserveTo <dir> | -DiscardEvidence] |',
        '                          list}',
        '',
        'The tree acted on defaults to the one THIS SCRIPT LIVES IN, never the',
        "caller's cwd. -Repo <path> names another tree deliberately.") }
}
