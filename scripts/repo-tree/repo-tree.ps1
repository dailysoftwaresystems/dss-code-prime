#!/usr/bin/env pwsh
# PURPOSE: resolve the repository tree a PowerShell script is standing in, and keep that script's git enumeration and its file reads on the SAME root.
#
# repo-tree.ps1 -- the PowerShell owner of "which tree am I standing in?".
#
# D-SCRIPT-GUARDS-ASK-GIT-FROM-THE-LANE-WORKTREE -- the row this closes.
#
# ★★★ ONE OWNER PER LANGUAGE, AND THIS IS THE THIRD AND LAST OF THEM.
#   .sh  -> `leg_tree_driver_identity` / `leg_tree_driver_git` in scripts/leg-tree/leg-tree.sh
#   .py  -> `_git_prefix` / `_git`                              in scripts/carriage-excludes/carriage-excludes.py
#   .ps1 -> THIS FILE
# The first two were written for D-SCRIPT-CARRIAGES-CANNOT-IDENTIFY-A-CROSS-NAMESPACE-LANE-WORKTREE.
# PowerShell had no owner, so a fix applied per guard would have put five copies of
# the same resolver in five files. This is the one copy. Dot-source it; do not
# re-spell it.
#
# ── WHAT A CALLER GETS, AND WHY EACH PIECE EXISTS ───────────────────────────────
#   Get-RepoTreeIdentity <tree>     the root, the gitdir (empty when a plain `-C`
#                                   can see the tree), and HEAD. Three ordered
#                                   cases, identical to the .sh twin.
#   Invoke-RepoTreeGit  <id> <args> ONE git command in whichever form that identity
#                                   decided -- so no caller reads the gitdir itself.
#   Enter-RepoTree      <tree>      ★ THE POINT OF THE FILE. Moves BOTH working
#                                   directories PowerShell keeps, then PROVES they
#                                   agree with git's.
#   Resolve-RepoTreePath <id> <rel> a repo-relative path from git, made absolute
#                                   under the root git enumerated it from.
#   Assert-RepoTreeOneRoot <id>     the proof on its own, for a caller that moved
#                                   itself.
#
# ⛔⛔ THE DEFECT THIS EXISTS TO MAKE IMPOSSIBLE, AND IT IS NOT THE OBVIOUS ONE.
# `Set-Location` moves the PowerShell PROVIDER location. Native child processes
# (git) are launched with THAT directory. `[Environment]::CurrentDirectory` -- which
# is what every `[IO.File]` / `[IO.Path]` relative path resolves against -- IS NOT
# MOVED WITH IT. So a script that does `Set-Location $RepoRoot` and then reads a
# repo-relative path git handed it ENUMERATES FROM ONE TREE AND READS FROM ANOTHER.
# ✔MEASURED 2026-09-01 (cycle P51, lane `gw`), pwsh started in the MAIN checkout,
# running the LANE worktree's copy of `check-line-endings.ps1`:
#     AFTER Set-Location:  Get-Location=<...>\.worktrees\gw
#                          [Environment]::CurrentDirectory=<...>\dss-code-prime
#     native git rev-parse --show-toplevel = <...>/.worktrees/gw
#     rel=scratchpad/p51/gw/probe-ps-cwd.ps1
#        Test-Path (PS location)      = True
#        [IO.File]::Exists (.NET cwd) = False
#     rel=scratchpad/before_baseline.sh
#        Test-Path (PS location)      = False
#        [IO.File]::Exists (.NET cwd) = True
# -- two relative paths, each visible to exactly one of the two resolvers, in one
# process. `Test-Path` is a CMDLET and follows the provider; `[IO.File]` does not.
# A guard built from both says "the file is there" and then reads the other tree's
# bytes.
# ★★ IT FAILS TOWARD GREEN. When both roots hold the path -- which is every path,
# for a lane that only EDITS files -- nothing throws: the verdict is simply about
# the wrong tree. The measured instance only died loudly because the enumerated
# file was NEW in the lane and absent from the other root.
#
# ⓘ WHY NOT JUST `[Environment]::CurrentDirectory = ...` AT EACH CALL SITE. Because
# "somebody remembers the second line" is the same maintenance promise that produced
# the defect. `Enter-RepoTree` moves both and then REFUSES to continue if they
# disagree, so the failure mode is a refusal rather than a wrong answer.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

class RepoTreeCollapse : System.Exception {
    RepoTreeCollapse([string]$m) : base($m) {}
}

function script:Resolve-RepoTreeRealPath([string]$Path) {
    # ⚠⚠ SYMLINKS IN *ANCESTOR* COMPONENTS, WHICH IS WHY `GetFullPath` ALONE IS NOT ENOUGH.
    # ✔MEASURED 2026-09-02 on macOS 26.6.2 and on CI's macos-latest, reproduced identically:
    # `/var` is a symlink to `/private/var`, so a temp tree has TWO correct spellings —
    # `/var/folders/…/lane` (what a caller passes) and `/private/var/folders/…/lane` (what
    # `[Environment]::CurrentDirectory` and `git rev-parse --show-toplevel` both return). A string
    # compare of two correct answers then REFUSED A CORRECTLY ENTERED TREE, reddening self-test
    # arms 3 and 6 and the whole `repo_tree_guard` on every POSIX CI host.
    # ★ `[IO.Path]::GetFullPath` normalises `.`/`..` and separators and does NOT follow links, and
    # `ResolveLinkTarget` follows only the LEAF — the link here is an ANCESTOR, so neither alone
    # sees it. Hence the walk: every component is resolved, from the root down.
    # ⓘ This does NOT weaken the assertion. Two genuinely different directories still resolve to
    # different paths; what it removes is a false alarm between two spellings of ONE directory.
    # ⓘ A non-existent path resolves to itself — the mangled-path arm below compares paths that
    # were never meant to exist, and must keep working.
    if ([string]::IsNullOrEmpty($Path)) { return $Path }
    $full = $Path
    try { $full = [IO.Path]::GetFullPath($Path) } catch { return $Path }
    $sep  = [IO.Path]::DirectorySeparatorChar
    $root = [IO.Path]::GetPathRoot($full)
    if ([string]::IsNullOrEmpty($root)) { return $full }
    $cur = $root
    foreach ($part in $full.Substring($root.Length).Split($sep, [StringSplitOptions]::RemoveEmptyEntries)) {
        $cur = [IO.Path]::Combine($cur, $part)
        # Bounded: a symlink cycle must not hang a guard.
        for ($hop = 0; $hop -lt 32; $hop++) {
            $t = $null
            try { $t = [IO.Directory]::ResolveLinkTarget($cur, $false) } catch { }
            if ($null -eq $t) { try { $t = [IO.File]::ResolveLinkTarget($cur, $false) } catch { } }
            if ($null -eq $t) { break }
            $tgt = $t.FullName
            if (-not [IO.Path]::IsPathRooted($tgt)) {
                $tgt = [IO.Path]::Combine([IO.Path]::GetDirectoryName($cur), $tgt)
            }
            try { $cur = [IO.Path]::GetFullPath($tgt) } catch { break }
        }
    }
    return $cur
}

function script:ConvertTo-RepoTreeComparable([string]$Path) {
    # One spelling for a path that three different producers hand back: git uses
    # forward slashes, .NET uses backslashes on Windows, and either may carry a
    # trailing separator. Comparison is case-insensitive because the two hosts this
    # runs on (Windows, macOS) are, and a case-sensitive compare would red on a
    # correct tree there rather than catching anything.
    if ([string]::IsNullOrEmpty($Path)) { return '' }
    $p = $Path.Replace('/', [IO.Path]::DirectorySeparatorChar)
    try { $p = [IO.Path]::GetFullPath($p) } catch { }
    # …and one spelling for a path whose ANCESTOR is a symlink. See the walk above.
    $p = Resolve-RepoTreeRealPath $p
    return $p.TrimEnd([IO.Path]::DirectorySeparatorChar).ToLowerInvariant()
}

function Get-RepoTreeIdentity {
    <#
    .SYNOPSIS
    The identity of the tree at <Tree>, in whichever namespace this process is in.

    Returns @{ Root; GitDir; Sha }. `GitDir` is EMPTY when a plain `git -C` can see
    the tree, and holds the RESOLVED gitdir when it cannot -- callers needing more
    than the identity go through `Invoke-RepoTreeGit` rather than reading it.
    Throws RepoTreeCollapse when git cannot describe the tree at all: a guard that
    cannot identify its subject must refuse, never report a pass over it.
    #>
    param([Parameter(Mandatory = $true)][string]$Tree)

    if ([string]::IsNullOrWhiteSpace($Tree)) { throw [RepoTreeCollapse]::new('no tree given') }
    $root = $null
    try { $root = [IO.Path]::GetFullPath($Tree).TrimEnd([IO.Path]::DirectorySeparatorChar) } catch {
        throw [RepoTreeCollapse]::new("cannot make '$Tree' absolute: $($_.Exception.Message)")
    }
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        throw [RepoTreeCollapse]::new("no such directory: $root")
    }

    # The ordinary case: a real repository, or a worktree whose gitdir resolves here.
    $sha = & git -C $root rev-parse HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($sha)) {
        return @{ Root = $root; GitDir = ''; Sha = $sha.Trim() }
    }

    # A worktree whose `.git` FILE names a gitdir THIS namespace cannot follow.
    $dotGit = Join-Path $root '.git'
    if (-not (Test-Path -LiteralPath $dotGit -PathType Leaf)) {
        throw [RepoTreeCollapse]::new("not a git work tree with a resolvable HEAD: $root")
    }
    $raw = ''
    foreach ($line in [IO.File]::ReadAllLines($dotGit)) {
        if ($line.StartsWith('gitdir:')) { $raw = $line.Substring('gitdir:'.Length).Trim(); break }
    }
    if ([string]::IsNullOrWhiteSpace($raw)) {
        throw [RepoTreeCollapse]::new("$dotGit is a file but names no gitdir: $root")
    }

    # ★★ THE THREE CASES ARE TRIED IN THIS ORDER AND THE ORDER IS THE WHOLE OF IT.
    # Identical to `leg_tree_driver_identity`, which learned it by getting it wrong:
    # testing "not absolute here" BEFORE "foreign-absolute" turns `C:/.../.git/worktrees/gw`
    # into `<worktree>/C:/.../.git/worktrees/gw`, which is byte for byte the mangling
    # a POSIX git performs on a Windows-created worktree -- i.e. the resolver would
    # reproduce the very defect it exists to undo.
    #   1. already a directory here            -> take it
    #   2. FOREIGN-ABSOLUTE (`X:/...` / `X:\...`) -> translate; only wslpath is claimed
    #   3. anything else                       -> relative to the worktree, as git allows
    $gd = $raw
    if (-not (Test-Path -LiteralPath $gd -PathType Container)) {
        if ($raw.Length -gt 1 -and $raw[1] -eq ':' -and [char]::IsLetter($raw[0])) {
            # `wslpath` is WSL's own and WSL is the only namespace crossing this
            # repository's carriages make. A second translator here would be
            # inventing a portability claim nothing has measured.
            if (-not (Get-Command wslpath -ErrorAction SilentlyContinue)) {
                throw [RepoTreeCollapse]::new(
                    "$root is a worktree whose gitdir '$raw' is absolute in another namespace, and no wslpath is available to translate it")
            }
            $conv = & wslpath -u $raw 2>$null
            if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($conv)) {
                throw [RepoTreeCollapse]::new("wslpath could not translate the gitdir '$raw' named by $dotGit")
            }
            $gd = $conv.Trim()
        } elseif ($raw.StartsWith('/')) {
            # POSIX-absolute and absent: there is nothing to try.
        } else {
            $gd = Join-Path $root $raw
        }
    }
    if (-not (Test-Path -LiteralPath $gd -PathType Container)) {
        throw [RepoTreeCollapse]::new("$dotGit names a gitdir this namespace cannot reach: $raw")
    }
    $gd = [IO.Path]::GetFullPath($gd).TrimEnd([IO.Path]::DirectorySeparatorChar)

    $sha = & git --git-dir=$gd --work-tree=$root rev-parse HEAD 2>$null
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($sha)) {
        throw [RepoTreeCollapse]::new("resolved the gitdir to $gd but git still cannot describe $root")
    }
    return @{ Root = $root; GitDir = $gd; Sha = $sha.Trim() }
}

function Invoke-RepoTreeGit {
    <#
    .SYNOPSIS
    Run ONE git command against the tree an identity describes.

    ★ THIS EXISTS SO NO CALLER READS `.GitDir` ITSELF. Which form is needed was
    decided once, by `Get-RepoTreeIdentity`; deciding it again at a call site is a
    second answer to a settled question, and the two answers drift.
    Returns the OUTPUT LINES only -- git's exit code is a legitimate answer for
    several of these subcommands ("no match" is 1), so callers that must tell
    "empty" from "broken" take a positive control instead of reading `$LASTEXITCODE`.

    ⚠ `$GitArgs` IS ONE EXPLICIT ARRAY, NOT `ValueFromRemainingArguments`, AND THAT
    IS DELIBERATE. ✔MEASURED 2026-09-01: with the remaining-arguments form, git's
    own short flags are matched against POWERSHELL parameter names first --
    `Invoke-RepoTreeGit $id grep -I -l -P '^.' HEAD` died with "the parameter name
    'P' is ambiguous. Possible matches include: -ProgressAction -PipelineVariable",
    and the surviving forms silently handed git a truncated argument list, which
    the caller's floor then reported as a collapsed scan. An accessor whose
    argument list can be eaten by its own shell is not an accessor.
    #>
    param(
        [Parameter(Mandatory = $true)][hashtable]$Identity,
        [Parameter(Mandatory = $true)][string[]]$GitArgs
    )
    $pre = if ([string]::IsNullOrEmpty($Identity.GitDir)) {
        @('-C', $Identity.Root)
    } else {
        @("--git-dir=$($Identity.GitDir)", "--work-tree=$($Identity.Root)")
    }
    $out = & git @pre @GitArgs 2>$null
    if ($null -eq $out) { return @() }
    # ⚠ ORDINARY PIPELINE SEMANTICS -- `@(...)`, NEVER `,@(...)`, AND BOTH WRONG
    # ANSWERS WERE MEASURED HERE ON 2026-09-01 BEFORE THIS SETTLED.
    #   * A one-line answer UNROLLS to a String, so a caller doing `.Count` under
    #     `Set-StrictMode -Version Latest` gets a TERMINATING error. The caller
    #     writes `@(...)` around the call, which is the idiom the rest of this
    #     repository's PowerShell already uses.
    #   * `,@(...)` "fixes" that and breaks something worse: the result reaches a
    #     PIPELINE as ONE object, so `... | Where-Object { $_ -notmatch $x }`
    #     applies the operator to the whole ARRAY -- which returns a FILTERED
    #     ARRAY, which is truthy -- and every line passes the filter unfiltered.
    #     Check F went from OK to eleven false convictions, all of them lines its
    #     exemption list plainly covers. A filter that silently stops filtering is
    #     the worse of the two failures, so the enumerable form wins.
    return @($out | Where-Object { $_ -ne '' })
}

function Assert-RepoTreeOneRoot {
    <#
    .SYNOPSIS
    Prove the enumeration root and the read root are the SAME root.

    ⓷ of this row's closing work: the two roots must be PROVEN equal, not assumed.
    That single assertion is what turns the wrong-root read from silent into loud,
    and it is worth more than fixing any number of individual call sites -- a call
    site added tomorrow is covered by it and would not have been covered by them.
    #>
    param([Parameter(Mandatory = $true)][hashtable]$Identity)

    $want    = ConvertTo-RepoTreeComparable $Identity.Root
    $psHere  = ConvertTo-RepoTreeComparable (Get-Location).ProviderPath
    $netHere = ConvertTo-RepoTreeComparable ([Environment]::CurrentDirectory)
    $bad = @()
    if ($psHere  -ne $want) { $bad += "the PowerShell provider location is '$((Get-Location).ProviderPath)'" }
    if ($netHere -ne $want) { $bad += "[Environment]::CurrentDirectory (what every [IO.File] relative read follows) is '$([Environment]::CurrentDirectory)'" }

    # And git's own answer, taken THROUGH the identity, so the check covers the
    # `--git-dir` form as well as the plain one.
    # ⚠ `@(...)` around the call: PowerShell UNROLLS a one-element array on return,
    # and `Set-StrictMode -Version Latest` makes `.Count` on the resulting scalar a
    # TERMINATING error -- which this function would then report as "git could not
    # name a top level", a true-sounding message about the wrong thing.
    $top = @(Invoke-RepoTreeGit $Identity @('rev-parse', '--show-toplevel'))
    if ($top.Count -ne 1) {
        $bad += "git could not name a top level for this tree"
    } elseif ((ConvertTo-RepoTreeComparable $top[0]) -ne $want) {
        $bad += "git enumerates from '$($top[0])'"
    }

    if ($bad.Count -gt 0) {
        throw [RepoTreeCollapse]::new(
            "repo-tree: the enumeration root and the read root are NOT the same root. Expected '$($Identity.Root)', but " +
            ($bad -join '; ') + ". Refusing to report a verdict about a tree this process cannot agree on.")
    }
}

function Enter-RepoTree {
    <#
    .SYNOPSIS
    Stand in <Tree>: move BOTH working directories, then prove they agree.

    Use this instead of `Set-Location $RepoRoot`. Returns the identity.
    #>
    param([Parameter(Mandatory = $true)][string]$Tree)
    $id = Get-RepoTreeIdentity $Tree
    Set-Location -LiteralPath $id.Root
    # ⛔ THE LINE THE WHOLE ROW IS ABOUT. Without it `Set-Location` moves git and
    # leaves every `[IO.File]` read behind in the directory pwsh was started in.
    [Environment]::CurrentDirectory = $id.Root
    Assert-RepoTreeOneRoot $id
    return $id
}

function Resolve-RepoTreePath {
    <#
    .SYNOPSIS
    A repo-relative path (as git emits it, forward slashes) made absolute under the
    root git enumerated it FROM.

    ⓘ An already-absolute path is returned untouched: `--files`-style modes take
    paths from a caller standing somewhere else entirely, and re-rooting those would
    be the same wrong-root answer in the other direction.
    #>
    param(
        [Parameter(Mandatory = $true)][hashtable]$Identity,
        [Parameter(Mandatory = $true)][string]$RelPath
    )
    if ([string]::IsNullOrEmpty($RelPath)) { return $RelPath }
    if ([IO.Path]::IsPathRooted($RelPath)) { return $RelPath }
    return (Join-Path $Identity.Root ($RelPath -replace '/', [string][IO.Path]::DirectorySeparatorChar))
}

# ══ SELF-TEST ═══════════════════════════════════════════════════════════════════
# ★★ THE ARMS THAT MATTER ARE 3 AND 4: they build a REAL worktree-shaped tree --
# `git worktree add`, so the lane's `.git` is a FILE -- put DIFFERENT bytes at the
# SAME relative path in the two roots, and run a child process whose cwd is the
# WRONG one. Arm 3 asserts the fixed path reads the tree it was pointed at. Arm 4
# is the CONTROL: the same child doing what the defect did (bare `Set-Location`,
# relative read) must read the OTHER tree -- proving arm 3 is measuring something
# and not passing vacuously.
# ⚠ A self-test run only from the main checkout would be vacuous here BY
# CONSTRUCTION -- the defect exists only where two roots hold the same path. So the
# fixture MANUFACTURES both roots rather than hoping to be run inside one.

function script:New-RepoTreeSandbox {
    $t = Join-Path ([IO.Path]::GetTempPath()) ("repo-tree-st-" + [Guid]::NewGuid().ToString('N'))
    $dir = (New-Item -ItemType Directory -Path $t).FullName
    # ⚠ REALPATH PREFIX COMPARISON before anything here is ever deleted. GetTempPath
    # can be redirected, and a sandbox that turned out to be inside the checkout
    # would make the cleanup below a `rm -rf` over the repository.
    $real = [IO.Path]::GetFullPath($dir).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $repo = [IO.Path]::GetFullPath((Split-Path -Parent (Split-Path -Parent $PSCommandPath))).TrimEnd([IO.Path]::DirectorySeparatorChar)
    if ($real.Equals($repo, [StringComparison]::OrdinalIgnoreCase) -or
        $real.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw [RepoTreeCollapse]::new("selftest sandbox '$real' is inside the repository at '$repo'")
    }
    return $real
}

function script:Remove-RepoTreeSandbox([string]$Dir) {
    if ([string]::IsNullOrWhiteSpace($Dir)) { return }
    $real = [IO.Path]::GetFullPath($Dir).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $tmp  = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
    if (-not $real.StartsWith($tmp + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        Write-Host "repo-tree: REFUSING to remove '$real' -- it is not under '$tmp'."
        return
    }
    # A git worktree leaves read-only pack files; -Force is required and is scoped
    # by the prefix assertion above.
    Remove-Item -LiteralPath $real -Recurse -Force -ErrorAction SilentlyContinue
}

function Invoke-RepoTreeSelfTest {
    $fail = 0
    $box = $null
    try {
        $box = New-RepoTreeSandbox
        $main = Join-Path $box 'main'
        $lane = Join-Path $box 'lane'
        $null = New-Item -ItemType Directory -Path $main
        $env:GIT_CONFIG_GLOBAL = Join-Path $box 'gitconfig-none'
        $env:GIT_CONFIG_SYSTEM = Join-Path $box 'gitconfig-none'
        & git -C $main init -q --initial-branch=trunk 2>&1 | Out-Null
        & git -C $main config user.email 'selftest@example.invalid' | Out-Null
        & git -C $main config user.name  'repo-tree selftest' | Out-Null
        [IO.File]::WriteAllText((Join-Path $main 'witness.txt'), "MAIN-ROOT`n")
        & git -C $main add witness.txt 2>&1 | Out-Null
        & git -C $main commit -q -m 'seed' 2>&1 | Out-Null
        & git -C $main worktree add -q --detach $lane 2>&1 | Out-Null

        if (-not (Test-Path -LiteralPath (Join-Path $lane '.git') -PathType Leaf)) {
            Write-Host "repo-tree: FAIL - selftest fixture is not worktree-shaped (lane/.git is not a FILE); every arm below would be vacuous."
            return 2
        }
        # THE FIXTURE'S WHOLE POINT: one relative path, two roots, different bytes.
        [IO.File]::WriteAllText((Join-Path $lane 'witness.txt'), "LANE-ROOT`n")

        # ARM 1 - an ORDINARY checkout resolves with NO gitdir engaged, so the
        # common path is untouched by any of this.
        $idMain = Get-RepoTreeIdentity $main
        if ($idMain.GitDir -ne '') {
            Write-Host "repo-tree: FAIL - arm 1: an ordinary checkout engaged the --git-dir form ('$($idMain.GitDir)'); the common path is not supposed to change."
            $fail = 1
        }

        # ARM 2 - the WORKTREE resolves, and its gitdir is NOT the mangled join.
        $idLane = Get-RepoTreeIdentity $lane
        if ([string]::IsNullOrWhiteSpace($idLane.Sha)) {
            Write-Host "repo-tree: FAIL - arm 2: could not describe the lane worktree at all."
            $fail = 1
        }
        if (-not [string]::IsNullOrEmpty($idLane.GitDir)) {
            $mangled = ConvertTo-RepoTreeComparable (Join-Path $lane 'C:')
            if ((ConvertTo-RepoTreeComparable $idLane.GitDir).StartsWith($mangled)) {
                Write-Host "repo-tree: FAIL - arm 2: the gitdir was JOINED to the worktree path ('$($idLane.GitDir)') -- this is the mangling the ordered cases exist to undo."
                $fail = 1
            }
        }

        # ARM 3 - THE PIN. A child standing in MAIN, told to stand in LANE, must
        # read LANE's bytes through a relative path.
        $got3 = script:Invoke-RepoTreeProbe -Cwd $main -Tree $lane -Mode 'fixed'
        if ($got3 -ne 'LANE-ROOT') {
            Write-Host "repo-tree: FAIL - arm 3: Enter-RepoTree read '$got3' from a process standing in the OTHER root; expected 'LANE-ROOT'. The enumeration root and the read root are not the same root."
            $fail = 1
        }

        # ARM 4 - THE CONTROL, and it must FAIL in the defect's own direction.
        # Same child, doing exactly what the guard did before this file existed.
        # If this arm ever reports LANE-ROOT the fixture has stopped reproducing the
        # defect and arm 3 proves nothing.
        $got4 = script:Invoke-RepoTreeProbe -Cwd $main -Tree $lane -Mode 'bare'
        if ($got4 -ne 'MAIN-ROOT') {
            Write-Host "repo-tree: FAIL - arm 4 (control): a bare Set-Location + relative read returned '$got4', not 'MAIN-ROOT'. The fixture no longer reproduces the defect, so arm 3 is vacuous and must not be trusted."
            $fail = 1
        }

        # ARM 5 - the ASSERTION itself must REFUSE a split root. This is the
        # REMOVE-direction mutant: delete the [Environment]::CurrentDirectory line
        # from Enter-RepoTree and this arm reds.
        $split = script:Invoke-RepoTreeProbe -Cwd $main -Tree $lane -Mode 'assert-split'
        if ($split -ne 'REFUSED') {
            Write-Host "repo-tree: FAIL - arm 5: Assert-RepoTreeOneRoot returned '$split' over a deliberately split root instead of refusing. The proof is not proving anything."
            $fail = 1
        }

        # ARM 6 - and it must NOT refuse a sound one (a green control for arm 5).
        $sound = script:Invoke-RepoTreeProbe -Cwd $main -Tree $lane -Mode 'assert-sound'
        if ($sound -ne 'OK') {
            Write-Host "repo-tree: FAIL - arm 6: Assert-RepoTreeOneRoot refused a correctly entered tree ('$sound'). A proof that reds on the good case is not usable."
            $fail = 1
        }

        # ARM 7 - a NON-git directory is a refusal, never an identity.
        $plain = Join-Path $box 'plain'
        $null = New-Item -ItemType Directory -Path $plain
        $threw = $false
        try { $null = Get-RepoTreeIdentity $plain } catch [RepoTreeCollapse] { $threw = $true } catch { $threw = $true }
        if (-not $threw) {
            Write-Host "repo-tree: FAIL - arm 7: a directory that is not a git tree produced an identity."
            $fail = 1
        }

        # ARM 8 - a `.git` FILE naming a gitdir that does not exist is a refusal,
        # not a silently-empty answer.
        $ghost = Join-Path $box 'ghost'
        $null = New-Item -ItemType Directory -Path $ghost
        [IO.File]::WriteAllText((Join-Path $ghost '.git'), "gitdir: $box/definitely-not-here`n")
        $threw = $false
        try { $null = Get-RepoTreeIdentity $ghost } catch { $threw = $true }
        if (-not $threw) {
            Write-Host "repo-tree: FAIL - arm 8: a .git file naming a nonexistent gitdir produced an identity."
            $fail = 1
        }

        # ARM 9 - Resolve-RepoTreePath roots a relative path at the IDENTITY, and
        # leaves an absolute one alone.
        $r = Resolve-RepoTreePath $idLane 'a/b.txt'
        if ((ConvertTo-RepoTreeComparable $r) -ne (ConvertTo-RepoTreeComparable (Join-Path $lane 'a/b.txt'))) {
            Write-Host "repo-tree: FAIL - arm 9: a relative path resolved to '$r', not under the identity's root."
            $fail = 1
        }
        $abs = Join-Path $main 'witness.txt'
        if ((Resolve-RepoTreePath $idLane $abs) -ne $abs) {
            Write-Host "repo-tree: FAIL - arm 9: an already-absolute path was re-rooted."
            $fail = 1
        }
    } catch {
        Write-Host "repo-tree: FAIL - selftest collapsed: $($_.Exception.Message)"
        $fail = 2
    } finally {
        if ($null -ne $box) {
            # Detach the worktree registration first so nothing points into a
            # directory that is about to go.
            try { & git -C (Join-Path $box 'main') worktree prune 2>&1 | Out-Null } catch { }
            Remove-RepoTreeSandbox $box
        }
        $env:GIT_CONFIG_GLOBAL = $null
        $env:GIT_CONFIG_SYSTEM = $null
    }
    if ($fail -eq 0) {
        Write-Host "repo-tree: self-test OK - 9 arms (ordinary checkout leaves the common path alone, a worktree-shaped tree resolves without the mangled join, the wrong-root read is PINNED with a control that reproduces it, the one-root proof refuses a split and passes a sound one, two refusal arms, path rooting); this owner is PROVEN able to fail."
    }
    return $fail
}

function script:Get-RepoTreePwsh {
    # The pwsh EXECUTABLE, for spawning a child.
    # ⚠ NOT `[Environment]::ProcessPath` and NOT `(Get-Process -Id $PID).Path`.
    # ✔MEASURED 2026-09-01 on this workstation, where PowerShell is installed as a
    # dotnet global tool: BOTH report `C:\Program Files\dotnet\dotnet.exe`, and
    # `Start-Process dotnet.exe -File x.ps1` dies with "the specified command or
    # file was not found". The arms below then reported the SPAWN failure in the
    # words of the property they were testing -- an instrument answering an
    # adjacent question. So the launcher is resolved explicitly and its absence is
    # a COLLAPSE, never a skip.
    foreach ($cand in @(
        (Join-Path $PSHOME 'pwsh.exe'),
        (Join-Path $PSHOME 'pwsh'),
        ((Get-Command pwsh -ErrorAction SilentlyContinue) | Select-Object -First 1 -ExpandProperty Source -ErrorAction SilentlyContinue),
        ((Get-Command powershell -ErrorAction SilentlyContinue) | Select-Object -First 1 -ExpandProperty Source -ErrorAction SilentlyContinue)
    )) {
        if (-not [string]::IsNullOrWhiteSpace($cand) -and (Test-Path -LiteralPath $cand -PathType Leaf)) { return $cand }
    }
    throw [RepoTreeCollapse]::new(
        "cannot locate a pwsh executable to spawn a child with (PSHOME=$PSHOME). The arms that pin the wrong-root read REQUIRE a child process, so this is a collapse, not a skip.")
}

function script:Invoke-RepoTreeProbe {
    # Runs ONE property in a CHILD pwsh whose PROCESS working directory is `Cwd`.
    # ★ It must be a child: the defect is a property of a process's two working
    # directories, and nothing in-process can start with them already split the way
    # a real invocation does.
    param([string]$Cwd, [string]$Tree, [string]$Mode)
    $owner = $PSCommandPath
    $body = @"
`$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. '$owner'
try {
    switch ('$Mode') {
        'fixed' {
            `$id = Enter-RepoTree '$Tree'
            Write-Output ([IO.File]::ReadAllText((Resolve-RepoTreePath `$id 'witness.txt')).Trim())
        }
        'bare' {
            # EXACTLY what the defect did: move the provider location only, then
            # read a repo-relative path with a .NET API.
            Set-Location -LiteralPath '$Tree'
            Write-Output ([IO.File]::ReadAllText('witness.txt').Trim())
        }
        'assert-split' {
            `$id = Get-RepoTreeIdentity '$Tree'
            Set-Location -LiteralPath `$id.Root
            # [Environment]::CurrentDirectory deliberately left behind.
            try { Assert-RepoTreeOneRoot `$id; Write-Output 'PASSED' }
            catch { Write-Output 'REFUSED' }
        }
        'assert-sound' {
            `$id = Enter-RepoTree '$Tree'
            try { Assert-RepoTreeOneRoot `$id; Write-Output 'OK' }
            catch { Write-Output "REFUSED: `$(`$_.Exception.Message)" }
        }
    }
} catch { Write-Output "THREW: `$(`$_.Exception.Message)" }
"@
    $script = Join-Path ([IO.Path]::GetDirectoryName($Cwd)) ("probe-$Mode.ps1")
    [IO.File]::WriteAllText($script, $body)
    $out = Join-Path ([IO.Path]::GetDirectoryName($Cwd)) ("probe-$Mode.out")
    $err = Join-Path ([IO.Path]::GetDirectoryName($Cwd)) ("probe-$Mode.err")
    $p = Start-Process -FilePath (script:Get-RepoTreePwsh) `
                       -ArgumentList @('-NoProfile', '-File', $script) `
                       -WorkingDirectory $Cwd -Wait -PassThru `
                       -RedirectStandardOutput $out -RedirectStandardError $err
    if ($p.ExitCode -ne 0) {
        return "CHILD-EXIT-$($p.ExitCode): " + ((Get-Content -LiteralPath $err -Raw -ErrorAction SilentlyContinue) -replace '\s+', ' ').Trim()
    }
    return ((Get-Content -LiteralPath $out -Raw -ErrorAction SilentlyContinue) + '').Trim()
}

# ── entry point ─────────────────────────────────────────────────────────────────
# Dot-sourcing must define and do nothing else; `$MyInvocation.InvocationName` is
# '.' exactly then. Anything else is a direct run.
if ($MyInvocation.InvocationName -ne '.') {
    $arg = if ($args.Count -gt 0) { [string]$args[0] } else { '' }
    switch ($arg) {
        '--selftest' { exit (Invoke-RepoTreeSelfTest) }
        '--help' {
            Write-Host "repo-tree.ps1 -- the PowerShell owner of 'which tree am I standing in?'."
            Write-Host ""
            Write-Host "  Dot-source it, then use Enter-RepoTree instead of Set-Location:"
            Write-Host "      . <repo>/scripts/repo-tree/repo-tree.ps1"
            Write-Host "      `$id = Enter-RepoTree `$RepoRoot"
            Write-Host "      `$lines = Invoke-RepoTreeGit `$id @('ls-files','--eol')   # ONE array, never loose flags"
            Write-Host "      `$bytes = [IO.File]::ReadAllBytes((Resolve-RepoTreePath `$id `$rel))"
            Write-Host ""
            Write-Host "  --selftest   run the arms (also wired into ctest as repo_tree_guard)"
            Write-Host "  --help       this text"
            exit 0
        }
        '' {
            Write-Host "repo-tree: this file is a library; dot-source it, or run --selftest / --help."
            exit 2
        }
        default {
            Write-Host "repo-tree: FAIL - unknown argument '$arg' (see --help)."
            exit 2
        }
    }
}
