#!/usr/bin/env pwsh
# PURPOSE: resolve the repository tree a PowerShell script is standing in, and keep that script's git enumeration and its file reads on the SAME root.
#
# repo-tree.ps1 -- the PowerShell owner of "which tree am I standing in?".
#
# D-SCRIPT-GUARDS-ASK-GIT-FROM-THE-LANE-WORKTREE -- the row this closes.
#
# ★★★ ONE OWNER PER LANGUAGE, AND THIS IS THE THIRD AND LAST OF THEM.
#   .sh  -> `leg_tree_driver_identity` / `leg_tree_driver_git` in scripts/leg-tree/leg-tree.sh
#   .py  -> `owning_tree` / `run_git`                           in scripts/owning-tree/owning-tree.py
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
#   Invoke-RepoTreeUnsteered -Command <c> -Arguments <args>  ONE command of any kind with the
#                                   CALLER's repository-selecting git variables removed
#                                   (a tool like gh finds its repository by running git).
#   Invoke-RepoTreeUnsteeredGit <args>  ONE git command with the CALLER's repository-
#                                   selecting git variables removed. Every git call in
#                                   this file goes through it.
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

$script:RepoTreeLocalGitVars = $null

function script:Get-RepoTreeLocalGitVars {
    # The names git calls repository-local (`git rev-parse --local-env-vars`), asked ONCE.
    # ✔MEASURED 2026-09-15: that query answers (15 names) even with GIT_DIR pointing nowhere.
    # ⚠ A list that does not name GIT_DIR is a COLLAPSE: without git's own list no git call
    # here can be kept from the caller's environment, and a hand-typed list would be a second
    # definition of a fact git owns.
    if ($null -eq $script:RepoTreeLocalGitVars) {
        $names = @(& git rev-parse --local-env-vars 2>$null | Where-Object { $_ -ne '' })
        if ($names -notcontains 'GIT_DIR') {
            throw [RepoTreeCollapse]::new("git rev-parse --local-env-vars did not name GIT_DIR (exit $LASTEXITCODE), so no git call here can be kept from the caller's git environment")
        }
        $script:RepoTreeLocalGitVars = $names
    }
    return $script:RepoTreeLocalGitVars
}

function Invoke-RepoTreeUnsteered {
    <#
    .SYNOPSIS
    Run ONE command with every repository-selecting variable the CALLER exported REMOVED, then
    put each back exactly as it was. `Invoke-RepoTreeUnsteeredGit` below is this owner, for git.

    ★★ ANY COMMAND, NOT ONLY GIT: a program that runs git to find its own subject is steered by
    the same variables. ✔MEASURED 2026-09-15 (P66 lane ge; gh 2.89.0, pwsh 7.6.6): `gh api
    repos/:owner/:repo` run from this repository's root answered
    `dailysoftwaresystems/dss-code-prime`, and HTTP 404 under another repository's GIT_DIR, and
    again under GIT_DIR + GIT_WORK_TREE -- gh had resolved THAT repository's origin -- so
    a CI-reading twin asked about another repository's CI even when it was given the branch
    (that pair is retired -- `dssharness check-ci-legs` reads the verdict now).

    ⚠ STDERR IS THE CALLER'S, UNLESS `-DiscardStandardError` IS GIVEN, AND THE SWITCH IS NOT A
    STYLE CHOICE. ✔MEASURED 2026-09-15 (pwsh 7.6.6): a `2>$null` on the native call INSIDE a
    function discards its stderr even when the caller writes `2>&1`; without it, a caller's `2>&1`
    receives the lines as ErrorRecords and a bare call prints them. So the git wrapper's contract
    (stderr discarded) can only be kept by redirecting AT the call, which is what the switch does.
    ⚠ `$Arguments` is ONE explicit array, for the reason `Invoke-RepoTreeGit` records. Returns
    the command's output; `$LASTEXITCODE` is the command's. Self-test arms 17 and 18.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [string[]]$Arguments = @(),
        [switch]$DiscardStandardError
    )
    $held = @{}
    foreach ($name in (Get-RepoTreeLocalGitVars)) {
        $item = Get-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
        if ($null -ne $item) {
            $held[$name] = [string]$item.Value
            Remove-Item -LiteralPath "Env:$name"
        }
    }
    try {
        if ($DiscardStandardError) { & $Command @Arguments 2>$null } else { & $Command @Arguments }
    } finally {
        foreach ($name in $held.Keys) {
            Set-Item -LiteralPath "Env:$name" -Value $held[$name]
        }
    }
}

function Invoke-RepoTreeUnsteeredGit {
    <#
    .SYNOPSIS
    Run ONE git command with every repository-selecting variable the CALLER exported REMOVED,
    then put each back exactly as it was. Every git call in this file goes through it.
    ⓘ It is `Invoke-RepoTreeUnsteered` above with git's stderr discarded; its contract is the one
    it had before the owner was generalised, and self-test arms 10 and 11 -- which predate that --
    are the control.

    ★★★ WHY. `git -C <dir>` moves git's working directory and nothing else: an exported
    GIT_DIR, GIT_WORK_TREE or GIT_INDEX_FILE still decides which repository answers.
    ✔MEASURED 2026-09-15 (P66 lane rr, round 2; pwsh 7.6.6, fixture repositories):
      * `Invoke-RepoTreeGit <id> @('ls-files','-co','--exclude-standard')` listed a decoy
        repository's file under its GIT_INDEX_FILE and under its GIT_DIR, and listed nothing
        at all under an EMPTY-but-set GIT_INDEX_FILE -- an empty value steers too;
      * `Get-RepoTreeOwningRoot` answered `<tree>\scripts\probe` under GIT_DIR alone, and the
        OTHER repository under GIT_DIR + GIT_WORK_TREE.
    A git hook exports GIT_INDEX_FILE to everything it runs, so a guard started from one is
    exactly such a caller. The names are git's own list -- the set git clears itself when it
    enters a submodule.
    ⚠ `Remove-Item Env:`, NEVER `[Environment]::SetEnvironmentVariable($n, $null)` nor
    `$env:X = ''`: ✔MEASURED on the same pwsh, both leave the variable SET TO EMPTY, and an
    empty value is a steering value.
    ⚠ Same argument contract as `Invoke-RepoTreeGit`: ONE explicit array. Returns git's output;
    `$LASTEXITCODE` is git's.
    #>
    param([Parameter(Mandatory = $true)][string[]]$GitArgs)
    Invoke-RepoTreeUnsteered -Command 'git' -Arguments $GitArgs -DiscardStandardError
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
    $sha = Invoke-RepoTreeUnsteeredGit @('-C', $root, 'rev-parse', 'HEAD')
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

    $sha = Invoke-RepoTreeUnsteeredGit @("--git-dir=$gd", "--work-tree=$root", 'rev-parse', 'HEAD')
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
    $out = Invoke-RepoTreeUnsteeredGit (@($pre) + @($GitArgs))
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

function Get-RepoTreeOwningRoot {
    <#
    .SYNOPSIS
    The root of the WORKING TREE THAT CONTAINS <Path>. Throws RepoTreeCollapse when
    no working tree does. <Path> may be a file or a directory; a file is taken by
    its directory.

    ★★★ THIS ANSWERS A DIFFERENT QUESTION FROM `Get-RepoTreeIdentity`, AND THAT IS WHY
    IT EXISTS RATHER THAN BEING FOLDED INTO IT.
      Get-RepoTreeIdentity  <Tree> -- "WHAT is the tree at <Tree>?" (root, gitdir, sha)
      Get-RepoTreeOwningRoot <Path> -- "WHICH tree contains <Path>?" (a root)
    `Get-RepoTreeIdentity` TAKES the tree as its argument and hands `.Root` straight
    back from it: every caller must already know which tree it means. This one
    DERIVES that argument. Making the identity function return a root would have been
    a true answer to the wrong question -- the adjacent-instrument class -- so the
    reuse runs the other way: this is a thin layer ON the identity function, in the
    owner's own file, and the hard case below is one call into it. No second resolver.
    ⓘ The `.sh` twin is `leg_tree_owning_root` in scripts/leg-tree/leg-tree.sh; the
    three ordered cases it relies on are the ones this file already documents.

    ⚠⚠ THE CALLER THIS WAS ADDED FOR PASSES `$PSCommandPath`, NOT `$PWD`.
    [[D-SCRIPT-LANE-WORKTREE-REPO-ROOT-IS-CWD-KEYED]] `lane-worktree.ps1` derived its
    root from a bare `git rev-parse --show-toplevel`, which answers "what repository
    am I STANDING in?" -- so every path it computed was rooted at whichever repository
    the caller's shell happened to be in. ✔MEASURED 2026-09-02 from a throwaway
    repository outside this one: it reported that repository's `.worktrees/` while
    running out of this one's checkout. A verb that manages `.worktrees/` must ask
    which tree ITS OWN FILE belongs to -- `$PWD` is a property of the caller's shell,
    the script's path is a property of the script, and only the second survives a `cd`.

    ✔MEASURED 2026-09-02, why `--show-toplevel` and not `--git-common-dir`: for a
    SUBMODULE checkout `--git-common-dir` names `<super>/.git/modules/<name>` and
    `git worktree list` names it too -- neither is a working tree, and rooting a
    removal there would aim it inside `.git`. `--show-toplevel` answers correctly.

    ★★★ AND GIT'S ANSWER IS CHECKED AGAINST THE TREE THE PATH LIVES IN.
    ✔MEASURED 2026-09-15 (P66 lane rr, fixtures; pwsh 7.6.6), each wrong answer returned with
    no throw:
        the caller exports GIT_DIR                    -> `<tree>\scripts\probe`, not a root
        the caller exports GIT_DIR and GIT_WORK_TREE  -> the OTHER repository
        an untracked DSS tree copied inside another checkout -> the OUTER checkout
    ⇒ git is asked through `Invoke-RepoTreeUnsteeredGit`, and the rule the `.sh` twin and
    `scripts/owning-tree/owning-tree.py` state is applied here too:
        A = the nearest DSS tree holding the path (`.plans\` AND `scripts\`), if any
        G = git's working tree for the path (case 1, else case 2)
        no G -> throw · no A -> G · G is A, or inside A -> G · A inside G -> throw
    #>
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) { throw [RepoTreeCollapse]::new('no path given') }
    $dir = $null
    try { $dir = [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar) } catch {
        throw [RepoTreeCollapse]::new("cannot make '$Path' absolute: $($_.Exception.Message)")
    }
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
        $dir = [IO.Path]::GetDirectoryName($dir)
    }
    if ([string]::IsNullOrEmpty($dir) -or -not (Test-Path -LiteralPath $dir -PathType Container)) {
        throw [RepoTreeCollapse]::new("no such directory for '$Path'")
    }

    # G, case 1: ask git AT THE PATH'S OWN DIRECTORY rather than at the cwd, and without the
    #    caller's git environment.
    $git = $null
    $top = Invoke-RepoTreeUnsteeredGit @('-C', $dir, 'rev-parse', '--show-toplevel')
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace([string]$top)) {
        $git = [IO.Path]::GetFullPath(([string]$top).Trim()).TrimEnd([IO.Path]::DirectorySeparatorChar)
    } else {
        # G, case 2: a worktree whose `.git` FILE names a gitdir THIS namespace cannot follow.
        #    `git -C` fails outright there, so walk up to the directory that HOLDS the `.git`
        #    and let `Get-RepoTreeIdentity` prove it can describe it -- the directory holding a
        #    resolvable `.git` IS the root.
        $cur = $dir
        while ($true) {
            if (Test-Path -LiteralPath (Join-Path $cur '.git')) {
                $ok = $true
                try { $null = Get-RepoTreeIdentity $cur } catch { $ok = $false }
                if ($ok) { $git = $cur; break }
            }
            $up = [IO.Path]::GetDirectoryName($cur)
            if ([string]::IsNullOrEmpty($up) -or $up -eq $cur) { break }
            $cur = $up
        }
    }
    if ($null -eq $git) { throw [RepoTreeCollapse]::new("no git working tree contains '$Path'") }

    # A, and the rule in the synopsis.
    $dss = $null
    $cur = $dir
    while ($true) {
        if ((Test-Path -LiteralPath (Join-Path $cur '.plans') -PathType Container) -and
            (Test-Path -LiteralPath (Join-Path $cur 'scripts') -PathType Container)) { $dss = $cur; break }
        $up = [IO.Path]::GetDirectoryName($cur)
        if ([string]::IsNullOrEmpty($up) -or $up -eq $cur) { break }
        $cur = $up
    }
    if ($null -eq $dss) { return $git }
    $cg = ConvertTo-RepoTreeComparable $git
    $ca = ConvertTo-RepoTreeComparable $dss
    if ($cg -eq $ca -or $cg.StartsWith($ca + [IO.Path]::DirectorySeparatorChar, [StringComparison]::Ordinal)) { return $git }
    throw [RepoTreeCollapse]::new("'$dss' is a DSS tree nested inside the checkout '$git', which it is not the root of. git answers for the enclosing checkout there, so no owning root is named.")
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

function script:Test-RepoTreeSamePath($A, $B) {
    if ([string]::IsNullOrWhiteSpace([string]$A) -or [string]::IsNullOrWhiteSpace([string]$B)) { return $false }
    return ((ConvertTo-RepoTreeComparable ([string]$A)) -eq (ConvertTo-RepoTreeComparable ([string]$B)))
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

        # ══ THE CALLER'S GIT ENVIRONMENT, AND WHICH TREE A PATH BELONGS TO (P66 lane rr) ══
        # ★ Every steering arm proves its negative first: the same query through a bare `& git`
        # under the same environment must reach the other repository, or the arm fails saying so.
        $decoy = Join-Path $box 'decoy'
        & git init -q $decoy 2>&1 | Out-Null
        [IO.File]::WriteAllText((Join-Path $decoy 'zz-steer-decoy.txt'), "d`n")
        & git -C $decoy add zz-steer-decoy.txt 2>&1 | Out-Null
        & git -C $decoy -c user.email=selftest@example.invalid -c user.name=repo-tree commit -q --no-verify -m decoy 2>&1 | Out-Null

        # ARM 10 - Invoke-RepoTreeGit IGNORES a caller's GIT_INDEX_FILE, and a caller's GIT_DIR.
        foreach ($steer in @(@{ N = 'GIT_INDEX_FILE'; V = (Join-Path $decoy '.git/index') },
                             @{ N = 'GIT_DIR'; V = (Join-Path $decoy '.git') })) {
            Set-Item -LiteralPath "Env:$($steer.N)" -Value $steer.V
            try {
                $raw = @(& git -C $main ls-files -co --exclude-standard 2>$null | Where-Object { $_ -match 'zz-steer-decoy' })
                $got = @(Invoke-RepoTreeGit $idMain @('ls-files', '-co', '--exclude-standard'))
            } finally { Remove-Item -LiteralPath "Env:$($steer.N)" -ErrorAction SilentlyContinue }
            $listed = @($got | Where-Object { $_ -match 'zz-steer-decoy' }).Count
            if ($raw.Count -lt 1 -or $listed -ne 0 -or $got -notcontains 'witness.txt') {
                Write-Host "repo-tree: FAIL - arm 10 ($($steer.N)): Invoke-RepoTreeGit listed the other repository's file $listed time(s) and witness.txt $(@($got | Where-Object { $_ -eq 'witness.txt' }).Count) time(s); a bare git listed the other repository's file $($raw.Count) time(s), and 0 there means the negative never materialised."
                $fail = 1
            }
        }

        # ARM 11 - and the caller's environment is PUT BACK exactly: a value kept, an absence kept.
        $env:GIT_WORK_TREE = 'repo-tree-held-value'
        $before = @{}; $after = @{}; $inside = $null
        try {
            foreach ($n in (Get-RepoTreeLocalGitVars)) {
                $i = Get-Item -LiteralPath "Env:$n" -ErrorAction SilentlyContinue
                $before[$n] = if ($null -eq $i) { '<absent>' } else { "=$($i.Value)" }
            }
            $inside = Invoke-RepoTreeUnsteeredGit @('-C', $main, 'rev-parse', '--show-toplevel')
            foreach ($n in (Get-RepoTreeLocalGitVars)) {
                $i = Get-Item -LiteralPath "Env:$n" -ErrorAction SilentlyContinue
                $after[$n] = if ($null -eq $i) { '<absent>' } else { "=$($i.Value)" }
            }
        } finally { Remove-Item -LiteralPath Env:GIT_WORK_TREE -ErrorAction SilentlyContinue }
        $drift = @($before.Keys | Where-Object { $before[$_] -ne $after[$_] })
        if ($drift.Count -gt 0 -or $before['GIT_WORK_TREE'] -ne '=repo-tree-held-value' -or -not (Test-RepoTreeSamePath $inside $main)) {
            Write-Host "repo-tree: FAIL - arm 11: after Invoke-RepoTreeUnsteeredGit the caller's git environment drifted on [$($drift -join ', ')], or git inside answered '$inside' rather than '$main' while GIT_WORK_TREE was held."
            $fail = 1
        }

        # A DSS-shaped tree that is its own repository, and another repository.
        $dss = Join-Path $box 'dss'
        $null = New-Item -ItemType Directory -Path (Join-Path $dss '.plans'), (Join-Path $dss 'scripts/probe')
        [IO.File]::WriteAllText((Join-Path $dss 'scripts/probe/probe.ps1'), "# probe`n")
        & git init -q $dss 2>&1 | Out-Null
        $foreign = Join-Path $box 'foreign'
        & git init -q $foreign 2>&1 | Out-Null
        $probeDir = Join-Path $dss 'scripts/probe'
        $probe = Join-Path $probeDir 'probe.ps1'

        # ARM 12 - CONTROL: the owning root of a path in that tree is the tree.
        $got = $null; $why = ''
        try { $got = Get-RepoTreeOwningRoot $probe } catch { $why = $_.Exception.Message }
        if (-not (Test-RepoTreeSamePath $got $dss)) {
            Write-Host "repo-tree: FAIL - arm 12 (control): Get-RepoTreeOwningRoot answered '$got' $why for a path in '$dss'."
            $fail = 1
        }

        # ARM 13 - under a caller's GIT_DIR, and GIT_DIR + GIT_WORK_TREE, it is STILL the tree.
        foreach ($steer in @(@{ L = 'GIT_DIR'; E = @{ GIT_DIR = (Join-Path $foreign '.git') } },
                             @{ L = 'GIT_DIR + GIT_WORK_TREE'; E = @{ GIT_DIR = (Join-Path $foreign '.git'); GIT_WORK_TREE = $foreign } })) {
            foreach ($k in @($steer.E.Keys)) { Set-Item -LiteralPath "Env:$k" -Value $steer.E[$k] }
            $got = $null; $why = ''; $raw = $null
            try {
                $raw = & git -C $probeDir rev-parse --show-toplevel 2>$null
                try { $got = Get-RepoTreeOwningRoot $probe } catch { $why = $_.Exception.Message }
            } finally { foreach ($k in @($steer.E.Keys)) { Remove-Item -LiteralPath "Env:$k" -ErrorAction SilentlyContinue } }
            if ([string]::IsNullOrWhiteSpace([string]$raw) -or (Test-RepoTreeSamePath $raw $dss) -or -not (Test-RepoTreeSamePath $got $dss)) {
                Write-Host "repo-tree: FAIL - arm 13 ($($steer.L)): Get-RepoTreeOwningRoot answered '$got' $why for a path in '$dss'; a bare git answered '$raw', which must name some OTHER directory for this arm to mean anything."
                $fail = 1
            }
        }

        # ARM 14 - an untracked DSS tree copied INSIDE another checkout is REFUSED.
        $outer = Join-Path $box 'outer'
        & git init -q $outer 2>&1 | Out-Null
        $copy = Join-Path $outer '.temp/copy'
        $null = New-Item -ItemType Directory -Path (Join-Path $copy '.plans'), (Join-Path $copy 'scripts/probe')
        [IO.File]::WriteAllText((Join-Path $copy 'scripts/probe/probe.ps1'), "# probe`n")
        $raw = & git -C (Join-Path $copy 'scripts/probe') rev-parse --show-toplevel 2>$null
        $got = $null; $why = ''
        try { $got = Get-RepoTreeOwningRoot (Join-Path $copy 'scripts/probe/probe.ps1') } catch { $why = $_.Exception.Message }
        if (-not (Test-RepoTreeSamePath $raw $outer) -or $null -ne $got -or $why -notmatch 'nested inside the checkout') {
            Write-Host "repo-tree: FAIL - arm 14: for an untracked DSS tree inside another checkout Get-RepoTreeOwningRoot answered '$got' ($why); a bare git answered '$raw' and must name the OUTER checkout '$outer' for this arm to mean anything."
            $fail = 1
        }

        # ARM 15 - a plain repository with no DSS tree around it is still answered (lane-worktree's -Repo).
        $got = $null; $why = ''
        try { $got = Get-RepoTreeOwningRoot $foreign } catch { $why = $_.Exception.Message }
        if (-not (Test-RepoTreeSamePath $got $foreign)) {
            Write-Host "repo-tree: FAIL - arm 15: a plain repository resolved to '$got' $why, not '$foreign'."
            $fail = 1
        }

        # ARM 16 - a repository nested INSIDE a DSS tree is its own working tree.
        $inner = Join-Path $dss '.temp/inner'
        $null = New-Item -ItemType Directory -Path $inner
        & git init -q $inner 2>&1 | Out-Null
        $got = $null; $why = ''
        try { $got = Get-RepoTreeOwningRoot $inner } catch { $why = $_.Exception.Message }
        if (-not (Test-RepoTreeSamePath $got $inner)) {
            Write-Host "repo-tree: FAIL - arm 16: a repository nested inside a DSS tree resolved to '$got' $why, not '$inner'."
            $fail = 1
        }

        # ══ THE OWNER RUNS ANY COMMAND UNSTEERED, NOT ONLY GIT (P66 lane ge) ══
        # ARM 17 - Invoke-RepoTreeUnsteered runs a NON-git command with the caller's GIT_DIR,
        # GIT_WORK_TREE and GIT_INDEX_FILE removed, hands back its exit status, and puts the
        # caller's values back. A child pwsh stands in for gh, which resolves its repository by
        # running git. The negative is the same child launched bare under the same environment.
        $pw17 = script:Get-RepoTreePwsh
        $probe17 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes(
            '$s = foreach ($n in @(''GIT_DIR'', ''GIT_WORK_TREE'', ''GIT_INDEX_FILE'')) { if (Test-Path -LiteralPath ("Env:" + $n)) { "$n=set" } else { "$n=unset" } }; Write-Output ($s -join '' ''); exit 7'))
        $steer17 = @{ GIT_DIR = (Join-Path $decoy '.git'); GIT_WORK_TREE = $decoy; GIT_INDEX_FILE = (Join-Path $decoy '.git/index') }
        $raw17 = $null; $got17 = $null; $rc17 = $null; $lost17 = @('<not measured>')
        foreach ($k in @($steer17.Keys)) { Set-Item -LiteralPath "Env:$k" -Value $steer17[$k] }
        try {
            $raw17 = @(& $pw17 -NoProfile -EncodedCommand $probe17) | Select-Object -Last 1
            $got17 = @(Invoke-RepoTreeUnsteered -Command $pw17 -Arguments @('-NoProfile', '-EncodedCommand', $probe17)) | Select-Object -Last 1
            $rc17 = $LASTEXITCODE
            $lost17 = @($steer17.Keys | Where-Object { [string](Get-Item -LiteralPath "Env:$_" -ErrorAction SilentlyContinue).Value -ne $steer17[$_] })
        } finally { foreach ($k in @($steer17.Keys)) { Remove-Item -LiteralPath "Env:$k" -ErrorAction SilentlyContinue } }
        if ($raw17 -ne 'GIT_DIR=set GIT_WORK_TREE=set GIT_INDEX_FILE=set' -or
            $got17 -ne 'GIT_DIR=unset GIT_WORK_TREE=unset GIT_INDEX_FILE=unset' -or $rc17 -ne 7 -or $lost17.Count -ne 0) {
            Write-Host "repo-tree: FAIL - arm 17: through Invoke-RepoTreeUnsteered a non-git child saw '$got17' and exited $rc17 (expected all three unset, exit 7); launched bare it saw '$raw17' (all three set is the negative); caller values not put back: [$($lost17 -join ', ')]."
            $fail = 1
        }

        # ARM 18 - THE STDERR CONTRACT. Invoke-RepoTreeUnsteeredGit still DISCARDS git's stderr,
        # even under a caller's 2>&1; Invoke-RepoTreeUnsteered leaves it to its caller unless
        # -DiscardStandardError is given. The negative is the owner without the switch: git's
        # stderr must reach that caller, or the empty captures prove nothing.
        $missing18 = Join-Path $box 'no-such-directory'
        $wrap18 = @(Invoke-RepoTreeUnsteeredGit @('-C', $missing18, 'rev-parse', 'HEAD') 2>&1)
        $rcWrap18 = $LASTEXITCODE
        $open18 = @(Invoke-RepoTreeUnsteered -Command 'git' -Arguments @('-C', $missing18, 'rev-parse', 'HEAD') 2>&1 |
                    Where-Object { $_ -is [System.Management.Automation.ErrorRecord] })
        $shut18 = @(Invoke-RepoTreeUnsteered -Command 'git' -Arguments @('-C', $missing18, 'rev-parse', 'HEAD') -DiscardStandardError 2>&1)
        if ($open18.Count -lt 1 -or $wrap18.Count -ne 0 -or $rcWrap18 -eq 0 -or $shut18.Count -ne 0) {
            Write-Host "repo-tree: FAIL - arm 18: git's stderr reached a 2>&1 caller $($wrap18.Count) time(s) through Invoke-RepoTreeUnsteeredGit (exit $rcWrap18, must be non-zero) and $($shut18.Count) time(s) through the owner with -DiscardStandardError, both expected 0; without the switch it reached that caller $($open18.Count) time(s), and 0 there means the negative never materialised."
            $fail = 1
        }

        # ══ THE CHILD PROBE'S OWN VERDICT PATH (P66 exit gate) ══
        # Each arm catches its own throw and names itself: a probe that throws is a finding about
        # THAT arm, and must not collapse every arm after it into one unattributed line.
        # ARM 19 - CONTROL: a child that exits 0 and answers comes back as exactly its answer.
        $got19 = $null; $why19 = ''
        try { $got19 = script:Invoke-RepoTreeProbe -Cwd $main -Tree $lane -Mode 'echo' } catch { $why19 = $_.Exception.Message }
        if ($why19 -ne '' -or $got19 -ne 'PROBE-ECHO') {
            Write-Host "repo-tree: FAIL - arm 19 (control): a child that exits 0 printing PROBE-ECHO came back as '$got19'$(if ($why19) { " and the probe THREW: $why19" }). Arm 20 cannot be read without this."
            $fail = 1
        }

        # ARM 20 - a child that exits NON-ZERO and writes NOTHING comes back as a CHILD-EXIT verdict
        # naming its exit code and both (empty) streams -- never as a throw. That child is the shape
        # that collapsed this whole self-test in P66's eight-run gate.
        $got20 = $null; $why20 = ''
        try { $got20 = script:Invoke-RepoTreeProbe -Cwd $main -Tree $lane -Mode 'exit-silent' } catch { $why20 = $_.Exception.Message }
        if ($why20 -ne '' -or $got20 -ne "CHILD-EXIT-3: stdout='' stderr=''") {
            Write-Host "repo-tree: FAIL - arm 20: a child that exited 3 with both streams empty came back as '$got20'$(if ($why20) { " and the probe THREW: $why20" }); expected exactly CHILD-EXIT-3 naming both streams empty."
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
        Write-Host "repo-tree: self-test OK - 20 arms (ordinary checkout leaves the common path alone, a worktree-shaped tree resolves without the mangled join, the wrong-root read is PINNED with a control that reproduces it, the one-root proof refuses a split and passes a sound one, two refusal arms, path rooting, a caller's GIT_INDEX_FILE and GIT_DIR reach no git call and the caller's environment is put back exactly, the owning root is the tree a path lives in under GIT_DIR and GIT_DIR + GIT_WORK_TREE, refused for a nested untracked copy, answered for a plain repository and for a repository nested in a tree, and the unsteered owner runs a NON-git command with the caller's git variables removed and its exit status kept, while the git wrapper still discards git's stderr, and a child that exits non-zero with both streams empty comes back as a CHILD-EXIT verdict naming its exit code, beside a control that reads an ordinary child); this owner is PROVEN able to fail."
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
        'echo' {
            # Arm 19, the control: a child that exits 0 and answers.
            Write-Output 'PROBE-ECHO'
        }
        'exit-silent' {
            # Arm 20: a child that exits NON-ZERO and writes nothing to either stream.
            exit 3
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
    # ★★ THE VERDICT PATH IS TOTAL: whatever the child did, ONE string comes back and nothing
    # throws. A child that did not exit 0 is `CHILD-EXIT-<code>` carrying BOTH its streams, so
    # the arm that asked reports the evidence, instead of the whole self-test collapsing on it.
    # ✔MEASURED 2026-09-15 (P66's eight-run gate, Windows Debug, ctest -j 8): repo_tree_guard
    # failed in 2.76 s (5.7 to 18.3 s whenever it passed, locally and on CI) with *selftest
    # collapsed: Method invocation failed because [System.Object[]] does not contain a method
    # named 'Trim'*. The failure path was `((Get-Content $err -Raw) -replace '\s+', ' ').Trim()`:
    # over an EMPTY or a MISSING file `Get-Content -Raw` yields nothing, `-replace` over nothing
    # yields an EMPTY ARRAY, and `.Trim()` on an empty array throws exactly that (pwsh 7.6.6, with
    # and without StrictMode). So a child had not exited 0 and had written nothing to stderr, and
    # the exit code and the stdout that could have said why were discarded with it.
    # ⚠ `$p.ExitCode` reads as $null, WITHOUT an exception, while the child has not exited
    # (✔MEASURED, same pwsh), and `$null -ne 0` is true. So "has not exited" and "exited
    # non-zero" both land here, and the verdict says which of the two it was.
    # ⚠ Both reads are cast to [string] first: `[string]` of nothing is '', and only a string can
    # be trimmed without first asking what kind of nothing came back.
    $stdout = [string](Get-Content -LiteralPath $out -Raw -ErrorAction SilentlyContinue)
    $stderr = [string](Get-Content -LiteralPath $err -Raw -ErrorAction SilentlyContinue)
    $code = $p.ExitCode
    if ($code -ne 0) {
        $exitText = if ($null -eq $code) { "UNREAD (HasExited=$($p.HasExited))" } else { [string]$code }
        return "CHILD-EXIT-${exitText}: stdout='$(($stdout -replace '\s+', ' ').Trim())' stderr='$(($stderr -replace '\s+', ' ').Trim())'"
    }
    return $stdout.Trim()
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
