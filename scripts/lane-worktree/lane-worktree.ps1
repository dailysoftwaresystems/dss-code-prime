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
            - 7 a scratchpad would be lost (see `Invoke-Remove`).
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
    # them `--preserve-to <dir>` and `--discard-scratchpad`; the behaviour, the
    # refusals and the exit codes are the same on both sides.
    [string]$PreserveTo,
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

function Say  { param([string]$m) Write-Host "lane-worktree: $m" }
function Die  {
    param([int]$Code, [string[]]$Lines)
    foreach ($l in $Lines) { [Console]::Error.WriteLine("lane-worktree: $l") }
    exit $Code
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
    #    work, so its honest manifest is EMPTY. An orchestrator that then seeds
    #    uncommitted work in re-writes it via `lane-fold.py seed <lane>`, the only other
    #    writer.
    #    ⚠ THIS LANDED IN THE `.sh` TWIN ONLY UNTIL P63 (see this file's header): the
    #    Windows entry point left the stale manifest in place, so the fold defect above
    #    was reachable through it the whole time.
    $manifestDir = Join-Path $repo '.worktrees/.manifests'
    try {
        if (-not (Test-Path -LiteralPath $manifestDir -PathType Container)) {
            New-Item -ItemType Directory -Path $manifestDir -Force | Out-Null
        }
        # ⓘ `-NoNewline` and no BOM: `lane-fold.py` parses this as JSON, and the `.sh`
        # twin writes exactly two bytes with `printf '{}'`. The two writers must agree.
        [IO.File]::WriteAllText((Join-Path $manifestDir "seed-$Name.json"), '{}',
                                (New-Object System.Text.UTF8Encoding($false)))
    } catch {
        Die 2 @("could not reset the seed manifest for '$Name': $($_.Exception.Message)")
    }

    $short = (git -C $abs rev-parse --short HEAD).Trim()
    Say "created $rel at $short"
    Say 'seed manifest reset to empty (this lane starts from the commit, not from uncommitted work)'
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

# ⚠⚠ A LANE'S SCRATCHPAD IS ITS EVIDENCE, AND THIS VERB USED TO DELETE IT WITHOUT
#    ASKING. [[D-CYCLE-LANE-WORKTREE-REMOVE-DISCARDS-AN-UNPRESERVED-SCRATCHPAD]]
#    ✔MEASURED 2026-09-01 (cycle P50): the orchestrator ran a `cp ... && echo preserved`
#    followed by `lane-worktree.sh remove t2` on ONE command line. The destination's
#    parent did not exist, `cp` failed, `&& echo` printed nothing, and the ABSENCE of
#    output read as "fine" -- then the very next command destroyed the only copy. Lane
#    `t2`'s 14 result JSONs and its md5 ledger are gone, and the row that cites them had
#    to be amended to admit it. ★ THE PRESERVE STEP WAS A CONVENTION LIVING IN THE
#    ORCHESTRATOR'S HEAD, and a rule with no teeth at the moment of the decision erodes.
#    ⇒ the tool owns it: a lane whose scratchpad holds files cannot be removed silently.
#    `-PreserveTo <dir>` makes the COPY this script's job so it cannot fail quietly (it
#    copies, counts both sides, and REFUSES on any mismatch); `-DiscardScratchpad` is
#    the explicit "I do not want it", which is a decision rather than an accident.
#    ⚠⚠ AND UNTIL P63 THAT SENTENCE WAS TRUE OF THE `.sh` TWIN ONLY. This function had
#    NO gate at all while the row read ✅ CLOSED. See this file's header.
function Invoke-Remove {
    if ([string]::IsNullOrWhiteSpace($Name)) {
        Die 5 @('usage: lane-worktree.ps1 remove <name> [-PreserveTo <dir> | -DiscardScratchpad]')
    }
    if ($DiscardScratchpad.IsPresent -and -not [string]::IsNullOrWhiteSpace($PreserveTo)) {
        Die 5 @('-PreserveTo and -DiscardScratchpad contradict each other; pick one.')
    }
    Assert-LaneName $Name
    $repo = Get-RepoRoot
    $rel  = ".worktrees/$Name"
    $abs  = "$repo/$rel"

    # ── THE SCRATCHPAD GATE, BEFORE ANY DELETION ────────────────────────────────
    # Counted with files only, so an empty directory tree is correctly "nothing to
    # preserve" and a single file is enough to stop the removal.
    $pad = "$abs/scratchpad"
    $padFiles = Get-FileCount $pad
    if ($padFiles -gt 0 -and -not $DiscardScratchpad.IsPresent -and
        [string]::IsNullOrWhiteSpace($PreserveTo)) {
        Die 7 @(
            "'$rel' holds a scratchpad with $padFiles file(s) and would be DELETED with it.",
            "A lane's scratchpad is its EVIDENCE -- probes, transcripts, mutant logs, the",
            'artefacts its registry row cites. Choose explicitly:',
            '  -PreserveTo <dir>        copy it there FIRST; this script verifies the copy',
            '  -DiscardScratchpad       delete it deliberately',
            "This gate exists because a hand-rolled 'cp && remove' one-liner lost lane t2's",
            "entire evidence tree in P50 when the destination's parent did not exist."
        )
    }
    if ($padFiles -gt 0 -and -not [string]::IsNullOrWhiteSpace($PreserveTo)) {
        try {
            if (-not (Test-Path -LiteralPath $PreserveTo -PathType Container)) {
                New-Item -ItemType Directory -Path $PreserveTo -Force | Out-Null
            }
        } catch {
            Die 7 @("could not create '$PreserveTo': $($_.Exception.Message)")
        }
        try {
            # The CONTENTS, so an existing destination is filled rather than nested one
            # level deeper -- the shape a re-run needs. `-Force` is what reaches hidden
            # entries, which the `.sh` twin's `cp -R <src>/.` reaches by construction.
            Copy-Item -Path (Join-Path $pad '*') -Destination $PreserveTo -Recurse -Force -ErrorAction Stop
        } catch {
            Die 7 @("copy of '$pad' -> '$PreserveTo' FAILED; nothing was removed: $($_.Exception.Message)")
        }
        $got = Get-FileCount $PreserveTo
        # ⚠ VERIFY, DO NOT ASSUME. The whole defect this gate closes was a copy that
        # failed while the caller read silence as success.
        if ($got -lt $padFiles) {
            Die 7 @(
                "preserve VERIFY FAILED: $padFiles file(s) under '$pad' but only $got under",
                "'$PreserveTo'. REFUSING to remove '$rel' -- the evidence would be lost."
            )
        }
        Say "preserved $padFiles scratchpad file(s) -> $PreserveTo (verified $got present)"
    }
    elseif ($padFiles -gt 0) {
        Say "DISCARDING $padFiles scratchpad file(s) under $rel, as instructed"
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
        '                          remove <name> [-PreserveTo <dir> | -DiscardScratchpad] |',
        '                          list}',
        '',
        'The tree acted on defaults to the one THIS SCRIPT LIVES IN, never the',
        "caller's cwd. -Repo <path> names another tree deliberately.") }
}
