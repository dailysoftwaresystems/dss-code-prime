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
    [switch]  $StrictHostKey,
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

$a = @('-o','ConnectTimeout=10','-o','BatchMode=yes')
if ($KeyPath -and (Test-Path $KeyPath)) { $a += @('-i', $KeyPath) }
if (-not $StrictHostKey) { $a += @('-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=/dev/null') }
$a += "$UserName@$target"
if ($Command) { $a += $Command }

& ssh @a
exit $LASTEXITCODE
