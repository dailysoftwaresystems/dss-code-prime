<#
PURPOSE: create and remove lane worktrees inside the ignored .worktrees/, refusing any root that would exceed Windows MAX_PATH.

★ THE POWERSHELL HALF OF scripts/lane-worktree/. It exists because the ROOT HOST IS
  WINDOWS and lanes are spawned from PowerShell: a bash-only owner means the first
  caller reaches for `git worktree add` directly, which is the exact erosion the
  ONE-OWNER rule exists to prevent. Behaviour, exit codes and refusal messages are
  the same as `lane-worktree.sh`; see that file's header for the full rationale and
  the operator ruling (2026-08-26) that placed worktrees inside the repository root.

⚠ THE MAX_PATH PREFLIGHT MATTERS MORE ON THIS SIDE, NOT LESS. MAX_PATH IS A WINDOWS
  LIMIT, AND THIS IS THE WINDOWS ENTRY POINT -- the anchored defect
  `D-CYCLE-WORKTREE-UNDER-THE-SESSION-SCRATCH-PATH-CANNOT-BE-BUILT-ON-WINDOWS` was
  measured on exactly this host. Moving worktrees from a 10-char root into the
  repository root spends 46 characters of that budget, so the check is arithmetic
  performed BEFORE a path is handed back, not a red discovered mid-build in files
  the lane never touched.

Exit codes: 0 OK - 2 not a repository / git refused - 3 MAX_PATH would be breached
            - 4 .worktrees/ is not ignored - 5 usage.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Verb,
    [Parameter(Position = 1)][string]$Name,
    [Parameter(Position = 2)][string]$Committish = 'HEAD',
    # The tree to act on. DEFAULTS to the tree this script lives in -- never the
    # caller's cwd. See `Get-RepoRoot` for the row and the measurement.
    [string]$Repo
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

function Invoke-Add {
    if ([string]::IsNullOrWhiteSpace($Name)) {
        Die 5 @('usage: lane-worktree.ps1 add <name> [committish]')
    }
    if ($Name -match '[\\/]' -or $Name.StartsWith('.')) {
        Die 5 @("lane name must be a single path component and must not start with '.': '$Name'")
    }
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
    $short = (git -C $abs rev-parse --short HEAD).Trim()
    Say "created $rel at $short"
    Say "build into $rel/build/<lane> -- never into the main tree's build/."
    Write-Output $abs
}

function Invoke-Remove {
    if ([string]::IsNullOrWhiteSpace($Name)) {
        Die 5 @('usage: lane-worktree.ps1 remove <name>')
    }
    $repo = Get-RepoRoot
    $rel  = ".worktrees/$Name"
    # --force because a lane worktree always carries an ignored build/ tree; without
    # it git refuses and the caller is tempted to Remove-Item -Recurse, which leaves
    # the registration behind in .git/worktrees/ where `git status` NEVER shows it.
    git -C $repo worktree remove --force $rel 2>$null
    if ($LASTEXITCODE -ne 0) {
        Say "worktree remove declined for '$rel' (already gone?) -- pruning anyway"
    }
    git -C $repo worktree prune
    # Drop the container only when WE emptied it; never disturb a sibling lane's.
    $container = Join-Path $repo '.worktrees'
    if ((Test-Path -LiteralPath $container) -and
        -not (Get-ChildItem -LiteralPath $container -Force)) {
        Remove-Item -LiteralPath $container -Force
        Say 'removed the now-empty .worktrees/'
    }
    Say "removed $rel and pruned stale registrations"
}

function Invoke-List {
    $repo = Get-RepoRoot
    Say 'registered worktrees:'
    git -C $repo worktree list | ForEach-Object { "  $_" }
    $container = Join-Path $repo '.worktrees'
    if (Test-Path -LiteralPath $container) {
        Say 'under .worktrees/:'
        foreach ($d in Get-ChildItem -LiteralPath $container -Directory -Force) {
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
        'usage: lane-worktree.ps1 [-Repo <path>] {add <name> [committish] | remove <name> | list}',
        '',
        'The tree acted on defaults to the one THIS SCRIPT LIVES IN, never the',
        "caller's cwd. -Repo <path> names another tree deliberately.") }
}
