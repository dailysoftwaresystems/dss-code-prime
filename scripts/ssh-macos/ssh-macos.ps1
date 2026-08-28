# ssh-macos.ps1 — reach the operator's physical macOS host. Sibling of ssh-macos.sh;
# CAPABILITY-PAIRED (a change to one lands in the other, or the pair is broken).
#
# WHY. macOS is the ONE target with no off-Mac emulator: nothing on Windows or Linux runs
# a Mach-O. Every other leg is verifiable where it is built; a Darwin artefact can only be
# proven on real hardware. This is that carriage, made reproducible.
#
# ★★ PERSONAL MACHINE, NOT CI. Usually OFF. Never assume it is reachable, never wake it,
#    never run against it without the operator saying it is on. Failure to connect is the
#    EXPECTED state, not something to route around.
# ★ NO HOST DETAILS ARE TRACKED. Precedence: parameter > env > .secrets\macos.env > FAIL
#   LOUD. `.secrets/` is gitignored because this repo is slated to go public (PR #37).
# ★ KEY-BASED ONLY, DELIBERATELY. `BatchMode=yes` makes ssh FAIL rather than sit at a
#   password prompt. No password is accepted, stored or forwarded — a password in a repo
#   script is a committed credential, and password auth cannot be automated without
#   putting the secret in the environment. Put the public key in the Mac's
#   ~/.ssh/authorized_keys; the operator does that, so no secret passes through tooling.
# ★ HOST-KEY POLICY matches the operator's original (StrictHostKeyChecking=no) because the
#   Mac is a DHCP LAN host whose address moves. STATED, not hidden: that forgoes MITM
#   protection — fine on a home LAN, not on an untrusted one. Use -StrictHostKey there.
#
# ⚠⚠ SSH JOINS `-Command` WITH SPACES AND HANDS THE RESULT TO THE REMOTE SHELL, SO LOCAL
# QUOTING IS GONE BY THE TIME IT ARRIVES. ✔MEASURED 2026-08-20 through the `.sh` twin:
# `sh -c 'mkdir -p /tmp/x && …'` reached the Mac as `sh -c mkdir -p /tmp/x && …` and the
# remote `mkdir` printed its usage line — a failure that looks like a broken carriage and
# is not one. ⇒ ANYTHING CARRYING QUOTES, PIPES, `&&` OR REDIRECTION IS ONE ARGUMENT:
#        scripts\ssh-macos\ssh-macos.ps1 "cd /tmp/x && clang -c a.c && nm -n a.o"
# ssh's own contract, not a defect here — recorded because the failure names the wrong
# culprit. Kept identical to the `.sh` twin (CAPABILITY-PAIRED).

[CmdletBinding()]
param(
    [string]  $HostName,
    [string]  $UserName,
    [string]  $KeyPath,
    # Prepended to the remote PATH for every payload. See the resolve block below
    # and the long note in the .sh twin; '' disables the repair entirely.
    [string]  $PathPrefix,
    [switch]  $StrictHostKey,
    # `-PushSource <local-dir> -PushDest <remote-dir>` -- the tree transport. See the
    # block near the bottom of this file for why this, and not an `-Rsync` twin.
    #
    # ⚠ TWO SEPARATE PARAMETERS, NOT ONE TWO-ELEMENT ARRAY, AND THAT IS A FIX RATHER
    # THAN A STYLE CHOICE. ✔MEASURED 2026-08-25: as `[string[]]$Push`, the invocation
    # `pwsh -File ssh-macos.ps1 -Push <src> <dst>` bound only <src> to -Push and then
    # bound <dst> POSITIONALLY to -HostName -- so the script tried to resolve the
    # DESTINATION PATH as a hostname and failed with "cannot resolve
    # '~/dss-ps1test' on this network", a message pointing at the network when the
    # defect was in argument binding. Under `-File` there is no array literal syntax,
    # so an array parameter silently absorbs one value and leaks the rest. Two named
    # parameters cannot mis-bind, and a missing one is caught by the arity check below.
    [string]  $PushSource,
    # Makes the push a SYNC rather than an accumulation. See the push block.
    [switch]  $Prune,
    [string]  $PushDest,
    # Sibling of ssh-macos.sh's `--resolve`: print the resolved address and exit,
    # so a caller resolves ONCE for a whole leg instead of once per carriage call.
    # See that script's `--resolve` block for the measurement behind it.
    [switch]  $Resolve,
    [string[]]$Command
)
$ErrorActionPreference = 'Stop'

function Import-DssSecrets([string]$File) {
    $h = @{}
    if (Test-Path $File) {
        foreach ($line in Get-Content $File) {
            if ($line -match '^\s*#' -or $line -notmatch '=') { continue }
            $k, $v = $line -split '=', 2
            # `$HOME` in the config is expanded HERE, not by a shell.
            $h[$k.Trim()] = $v.Trim() -replace '\$HOME', $HOME
        }
    }
    return $h
}
$conf = Import-DssSecrets (Join-Path $PSScriptRoot '..\..\.secrets\macos.env')
if (-not $HostName) { $HostName = if ($env:DSS_MACOS_HOST) { $env:DSS_MACOS_HOST } else { $conf['DSS_MACOS_HOST'] } }
if (-not $UserName) { $UserName = if ($env:DSS_MACOS_USER) { $env:DSS_MACOS_USER } else { $conf['DSS_MACOS_USER'] } }
if (-not $KeyPath)  { $KeyPath  = if ($env:DSS_MACOS_KEY)  { $env:DSS_MACOS_KEY }  else { $conf['DSS_MACOS_KEY'] } }

# THE REMOTE PATH IS REPAIRED BEFORE ANY PAYLOAD RUNS --
# D-TOOLS-SSH-MACOS-NONINTERACTIVE-PATH-IS-CLOBBERED-BY-EMSDK.
# Read the long note in the .sh twin: this host's emsdk shell hook REPLACES PATH for a
# NON-INTERACTIVE session and drops /opt/homebrew/bin and /usr/local/bin, so
# `command -v cmake` answers MISSING while the binary is sitting there. The wrong answer
# is a FALSE NEGATIVE, which silently shrinks what the project believes it can do.
# A login shell is the arm NOT taken: this host's profile is what eats a script piped to
# `bash -s`, so repairing PATH with it trades a false negative for a silent truncation.
# `$PATH` is single-quoted so the FAR SIDE expands it -- expanding here would ship this
# Windows box's PATH to macOS. `export ...;` is a STATEMENT, not an assignment prefix,
# because ssh hands the payload to the remote shell as ONE string: `PATH=x cd d && make`
# would run `make` with the broken PATH, and the export form is also inherited by
# `bash -s` without touching its stdin.
# `+set` semantics, NOT `-not $x` -- an EMPTY value is the caller's DISABLE switch and
# must survive. `-not ''` is TRUE in PowerShell exactly as `${x:-d}` substitutes on empty
# in sh, so the naive form silently re-assigns the default over a deliberate disable and
# turns a CONTROL arm into a second treatment arm. Measured on the .sh twin; see the
# set-ness note there. `$PSBoundParameters` distinguishes "passed empty" from "not passed".
if (-not $PSBoundParameters.ContainsKey('PathPrefix')) {
    $PathPrefix = if ($null -ne $env:DSS_MACOS_PATH_PREFIX) { $env:DSS_MACOS_PATH_PREFIX }
                  elseif ($conf.ContainsKey('DSS_MACOS_PATH_PREFIX')) { $conf['DSS_MACOS_PATH_PREFIX'] }
                  else { '/opt/homebrew/bin:/usr/local/bin' }
}
$remotePathStmt = if ($PathPrefix) { 'export PATH="' + $PathPrefix + ':$PATH"; ' } else { '' }

# ★★★ THE SCRIPT FINDS THE KEY. THE CALLER NEVER DOES ANYTHING MANUALLY.
# Operator instruction 2026-08-17: "you must be able to get everything from the tool
# scripts, which goes inside .secrets and find the key. never manually."
# ⚠ `$HOME` names a DIFFERENT directory in WSL / Git Bash / PowerShell, so a
# `$HOME`-relative key makes a host reachable or not depending on which shell you
# started in — ✔MEASURED on the arm64 VPS twin, whose key existed only under WSL's
# $HOME. `.secrets/` is the SAME directory in all three and is gitignored (as are
# *.key and *.env repo-wide), so the key lives there and this script RESOLVES it.
# ⚠ CAPABILITY-PAIRED with `ssh-macos.sh` and BOTH arm64-vps carriages — a change to
# one lands in all four, which is a claim these headers make and nothing checks
# (that is exactly how the `.ps1` twin ended up not expanding `$HOME` at all).
$_repoKey = Join-Path $PSScriptRoot '..\..\.secrets\macos.key'
if ($KeyPath) { $KeyPath = $KeyPath -replace '^\$HOME', $HOME -replace '^~', $HOME }
if ($KeyPath -and -not (Test-Path $KeyPath) -and (Test-Path $_repoKey)) {
    Write-Warning "ssh-macos: DSS_MACOS_KEY='$KeyPath' does not exist; using the repo-local key $_repoKey"
    $KeyPath = $_repoKey
} elseif (-not $KeyPath -and (Test-Path $_repoKey)) {
    $KeyPath = $_repoKey
}

if (-not $HostName -or -not $UserName) {
    Write-Error "ssh-macos: connection data missing. Create .secrets\macos.env with DSS_MACOS_HOST, DSS_MACOS_USER, DSS_MACOS_KEY (a key PATH), or pass -HostName/-UserName."
    exit 3
}

# `.local` resolution. A literal IP skips this entirely.
#
# ★ RESOLVE-DNSNAME FIRST, NOT TEST-CONNECTION — this is a MEASURED defect in the
#   original script, not a preference. `Test-Connection <host>.local` resolves the IPv6
#   LINK-LOCAL address first (fe80::…), so `.IPv4Address` comes back $null and the script
#   reports "cannot resolve" for a Mac that is powered on and answering pings. Measured
#   2026-08-04: ping reported fe80::c7e:…:7fb8 while the host was live at 192.168.0.71.
#   Resolve-DnsName returns BOTH records, so filtering to IPv4 is what actually works.
#   Test-Connection is kept only as a fallback for hosts Resolve-DnsName cannot see.
$target = $HostName
if ($HostName -notmatch '^\d{1,3}(\.\d{1,3}){3}$') {
    $ip = $null
    try {
        $ip = (Resolve-DnsName $HostName -ErrorAction Stop |
               Where-Object { $_.IPAddress -and $_.IPAddress -match '^\d{1,3}(\.\d{1,3}){3}$' } |
               Select-Object -First 1).IPAddress
    } catch { }
    if (-not $ip) {
        $ip = (Test-Connection $HostName -Count 1 -ErrorAction SilentlyContinue |
               Select-Object -First 1).IPv4Address.IPAddressToString
    }
    # ★★ PAIRING NOTE, WRITTEN DOWN BECAUSE IT IS A DELIBERATE ASYMMETRY AND NOT
    # DRIFT (2026-08-21, cycle P23). The `.sh` twin prints a THIRD cause here that
    # this file deliberately does not:
    # D-SCRIPT-MACOS-HOST-OVERRIDE-DOES-NOT-CROSS-THE-WSLENV-BOUNDARY — a
    # `DSS_MACOS_HOST` set in a Windows parent does not reach a `wsl.exe` child
    # unless `WSLENV` names it. That cause CANNOT ARISE in PowerShell, where
    # `$env:DSS_MACOS_HOST` is simply read by this process, so printing it here
    # would be advice about a boundary this file never crosses. The twins still
    # exit **3** for the same condition with the same first two causes; only the
    # WSL-only third line differs, and it is runtime-gated on `WSL_DISTRO_NAME`
    # over there rather than always printed.
    if (-not $ip) {
        Write-Error @"
cannot resolve '$HostName' on this network.
  Most likely: the Mac is OFF or asleep — a personal machine, not CI. Ask before waking it.
  Otherwise:   mDNS unavailable from here. Put a literal IP in DSS_MACOS_HOST.
"@
        exit 3
    }
    $target = $ip
}

# `-Resolve` -- print the address and exit. Twin of ssh-macos.sh's `--resolve`;
# the reasoning, and the measurement that produced it, live there.
# D-SCRIPT-MACOS-LEG-RERESOLVES-THE-HOST-AT-EVERY-CARRIAGE-CALL
if ($Resolve) {
    Write-Output $target
    exit 0
}

$a = @('-o','ConnectTimeout=10','-o','BatchMode=yes')
if ($KeyPath -and (Test-Path $KeyPath)) { $a += @('-i', $KeyPath) }
if (-not $StrictHostKey) { $a += @('-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=/dev/null') }

# ============================================================================
# `-PushSource <local-dir> -PushDest <remote-dir>` -- sibling of ssh-macos.sh's `--push`.
#
# ★★ WHY THIS MODE AND NOT AN `-Rsync` TWIN, stated because the omission looks like
# the pairing break it is not. The `.sh` sibling carries BOTH `--rsync` and `--push`;
# this file carries only `--push`, DELIBERATELY. `--rsync` `exec`s the LOCAL rsync,
# and ✔MEASURED 2026-08-25 neither Git Bash nor Windows PowerShell ships one -- so an
# `-Rsync` here would be a mode that can never run on the platform this file exists
# to serve. `--push` uses `tar` + `ssh`, both of which Windows 10+ ships in-box, so it
# is the FIRST mode of this carriage that can honestly be paired at all.
# ⚠ The header's "CAPABILITY-PAIRED" claim was therefore already untrue before this
# change, and it is now true for every mode that CAN be paired. Pairing by EXISTENCE
# is not pairing by BEHAVIOUR, and pairing a mode onto a platform that cannot execute
# it is worse than not pairing it -- it ships a green-looking flag that always fails.
#
# ⚠ EXCLUDES ARE ANCHORED (`./build`, never `build`), for the reason `scripts/wsl-leg`
# records: an unanchored exclude once silently skipped a changed `.cpp` and a gate ran
# against an incomplete tree. An over-matching exclude does not fail -- it produces a
# green run over the wrong source.
#
# ★ THE WITNESS CARRIES THE RESOLVED REMOTE PATH, not merely a success token.
# ✔MEASURED 2026-08-25 on the `.sh` side: with the destination QUOTED into the remote
# command, the push reported OK, the witness fired, rc was 0 -- and the files landed in
# a directory literally NAMED `~`, because a tilde only expands unquoted. tar genuinely
# succeeded; it just succeeded somewhere else. A witness that only proves success is
# satisfied by success elsewhere, so this one proves the PLACE.
# ============================================================================
if ($PushSource -or $PushDest) {
    if (-not $PushSource -or -not $PushDest) {
        Write-Error 'ssh-macos: push needs BOTH -PushSource <local-dir> and -PushDest <remote-dir>'
        exit 2
    }
    $src = $PushSource
    $dst = $PushDest
    if (-not (Test-Path -PathType Container $src)) {
        Write-Error "ssh-macos: -PushSource '$src' is not a directory"
        exit 2
    }
    if ($dst -in @('', '/', '~', '~/', '$HOME')) {
        Write-Error "ssh-macos: -PushDest refuses destination '$dst'"
        exit 2
    }
    # Unquoted on the remote side so `~` expands there => refuse anything ambiguous.
    if ($dst -match '[\s;&|`><]' -or $dst -match '\$\(') {
        Write-Error "ssh-macos: -PushDest refuses a destination with whitespace or shell metacharacters: '$dst'"
        exit 2
    }
    $witness = "DSS_PUSH_OK_$PID"
    # `-m` on extract (do not extract modification time) + a stamp taken BEFORE the
    # archive lands. Together they turn "was this file in the archive" into "is it newer
    # than the stamp", which is what makes -Prune answerable without trusting a manifest
    # that could itself have arrived truncated. It also retires the mtime hazard that
    # forced the macOS leg to build clean: a pushed source can no longer land behind an
    # existing object and be silently skipped by ninja.
    $sshArgs = $a + @("$UserName@$target", $remotePathStmt, "mkdir -p $dst && cd $dst && mkdir -p build && touch build/.dss-push-stamp && sleep 1 && tar -x -m -f - && echo ${witness}:`$(pwd -P)")
    # ⚠⚠ `2>&1` MUST NOT APPEAR ON THE PRODUCING SIDE OF A BINARY PIPE. ✔MEASURED
    # 2026-08-25: with `tar ... 2>&1 | ssh ...`, tar's DIAGNOSTICS are merged into the
    # archive byte stream and the remote tar receives a corrupted file -- the push
    # failed with an empty capture and rc=1, and the error pointed nowhere near the
    # cause. The `.sh` sibling never had this defect only because its redirect sits on
    # the ssh side of the pipe. tar's stderr is left on the terminal deliberately: it
    # is diagnostics for a human, not payload for a pipe.
    # ★ PowerShell 7's native-to-native pipe is itself binary-clean -- ✔MEASURED with a
    # local `tar -c -f - . | tar -t -f -` round trip -- so the pipe was never the
    # problem, and assuming it was would have replaced a correct mechanism.
    # ⚠ `./.claude/worktrees` IS THE LARGEST EXCLUDE HERE. ✔MEASURED 2026-08-25: the Mac
    # held 16,312 files against a local 6,660 and 9,638 of the difference was worktrees --
    # a full copy of the repo per live agent. The examples runner GLOBS `examples/<lang>/*`
    # and a worktree carries its own, so a gate host holding one can run somebody's
    # uncommitted corpus.
    # ★★★ AND THE EXCLUDE LIST IS DERIVED, NOT TYPED. This enumeration was one of
    # four, and every one spelled `node_modules` ANCHORED at the top level, which
    # `.kilo/node_modules` is not. ✔MEASURED 2026-08-26: the arm64 VPS gate host
    # held that tree -- 3,671 files, 61 MB -- and the `.sh` sibling's identical list
    # ships 9,284 members against this derivation's 3,583, `.secrets/` included.
    # ⚠ THIS path did NOT leak `node_modules`, and that is the uncomfortable half:
    # bsdtar and GNU tar disagreed about what the SAME pattern meant, so two
    # carriages written to be siblings were shipping different trees. A derivation
    # both read the same way is what removes that, not a better-typed list.
    # D-SCRIPT-CARRIAGE-EXCLUDES-ARE-A-HAND-LIST-AND-MISS-NESTED-IGNORED-TREES
    # ⓘ `.git` stays a POLICY withhold (`--also`): this carriage syncs no history.
    # ★ `tar` here is System32 bsdtar 3.8.4, NOT the Git Bash GNU tar 1.35 the `.sh`
    # sibling gets -- ✔MEASURED 2026-08-26. `-X` is the exclude-from spelling BOTH
    # accept, which is why `carriage-excludes` emits `./`-anchored patterns and no
    # `--anchored` flag: bsdtar has no such flag, and the `./` prefix already
    # anchors against a `./`-rooted member name.
    $excl = Join-Path ([System.IO.Path]::GetTempPath()) "ssh-macos-excludes.$PID"
    $xs = Join-Path $PSScriptRoot '..\carriage-excludes\carriage-excludes.py'
    & python $xs --format tar --repo $src --also .git --out $excl
    if ($LASTEXITCODE -ne 0) {
        Remove-Item -LiteralPath $excl -ErrorAction SilentlyContinue
        Write-Error 'ssh-macos: carriage-excludes refused - refusing to push a list it would not vouch for'
        exit 1
    }
    $out = & tar -c -C $src -X $excl `
        -f - . | & ssh @sshArgs 2>&1
    # ⚠ `$LASTEXITCODE` IS CAPTURED FIRST AND THE CLEANUP RUNS AFTER. A cmdlet does
    # not set `$LASTEXITCODE`, so the other order happens to work -- but "happens to"
    # is how a status gets silently replaced by an unrelated one, and the push's exit
    # status is the only thing standing between a truncated archive and a green leg.
    $tarRc = $LASTEXITCODE
    Remove-Item -LiteralPath $excl -ErrorAction SilentlyContinue
    $landed = ($out | Select-String -Pattern "$witness`:(.*)" | Select-Object -Last 1)
    if ($tarRc -ne 0 -and -not $landed) {
        Write-Error "ssh-macos: push FAILED (rc=$tarRc)"
        $out | Write-Host
        exit 1
    }
    if (-not $landed) {
        Write-Error 'ssh-macos: push produced no witness - the archive may have arrived TRUNCATED'
        $out | Write-Host
        exit 1
    }
    $path = $landed.Matches[0].Groups[1].Value.Trim()
    if (-not $path.StartsWith('/')) {
        Write-Error "ssh-macos: push witness carried a non-absolute path '$path'"
        exit 1
    }
    Write-Host "ssh-macos: push OK -> $path"

    if ($Prune) {
        # The push is otherwise an ACCUMULATION: tar extraction never deletes, so a file
        # removed locally lives forever on the remote and the gate tests a tree that
        # exists nowhere. Opt-in and named, exactly as -ResetTo is, because this deletes
        # files on somebody's machine.
        $pruneBody = @'
set -uo pipefail
cd "$D" || { echo "[X] prune: $D missing"; exit 1; }
S=build/.dss-push-stamp
[ -f "$S" ] || { echo "[X] prune: no push stamp -- REFUSING (cannot tell fresh from stale)"; exit 1; }
_fresh=$(find . -type f -newer "$S" \
    -not -path './build/*' -not -path './.git/*' -not -path './scratchpad/*' \
    -not -path './target/*' -not -path './.venv/*' -not -path './node_modules/*' \
    | wc -l | tr -d ' ')
if [ "${_fresh:-0}" -lt 100 ]; then
    echo "[X] prune: only ${_fresh:-0} file(s) look freshly extracted -- REFUSING."
    echo "    A truncated archive here would delete the tree this push was updating."
    exit 1
fi
find . -type f ! -newer "$S" \
    -not -path './build/*' -not -path './.git/*' -not -path './scratchpad/*' \
    -not -path './target/*' -not -path './.venv/*' -not -path './node_modules/*' \
    -print -delete > "$S.pruned" 2>/dev/null
echo "PRUNED=$(wc -l < "$S.pruned" | tr -d ' ') FRESH=$_fresh"
head -25 "$S.pruned"
rm -f "$S" "$S.pruned"

# The files are gone; the directories that held them are not. A hollow
# `.claude/worktrees/agent-*` still reads as "this host holds worktrees" to the next
# person who looks. `-delete` implies `-depth` on BSD find, so nested empties collapse
# in one pass. Every empty directory here is a leftover: git tracks no empty directory,
# so anything the repo genuinely wants arrives holding a file.
_dirs=$(find . -type d -empty \
    -not -path './build' -not -path './build/*' -not -path './.git' -not -path './.git/*' \
    -not -path './scratchpad' -not -path './scratchpad/*' -not -path './target/*' \
    -not -path './.venv/*' -not -path './node_modules/*' -not -path '.' \
    -print -delete | wc -l | tr -d ' ')
echo "PRUNED_DIRS=$_dirs"
'@
        $pruneArgs = $a + @("$UserName@$target", $remotePathStmt, 'bash -s')
        $pOut = ("D=$dst`n" + $pruneBody + "`n") | & ssh @pruneArgs 2>&1
        # A witness, not an exit code -- a pipeline's status is its last stage's.
        if ($pOut -match 'PRUNED=') { $pOut | Select-Object -First 26 | Write-Host }
        else {
            Write-Error 'ssh-macos: -Prune produced no PRUNED= witness - the remote tree may still hold stale files'
            $pOut | Write-Host
            exit 1
        }
    }
    exit 0
}

$a += "$UserName@$target"
# An EMPTY payload is an interactive session; prefixing it would turn a login into
# a one-shot `export` and hand back a shell that exits immediately.
if ($Command) {
    if ($remotePathStmt) { $a += $remotePathStmt }
    $a += $Command
}

& ssh @a
exit $LASTEXITCODE
