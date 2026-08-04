# ssh-arm64-vps.ps1 — reach the native aarch64 Linux VPS, from Windows. Sibling of
# ssh-arm64-vps.sh; CAPABILITY-PAIRED (a change to one lands in the other).
#
# WHY. Every arm64 result in this project used to come from qemu, which says nothing about
# real hardware. This box is native aarch64 Ubuntu and is where the `.sh` driver was first
# exercised end-to-end (2026-08-04: 331,330 tests, 1 known non-DSS confound, elf64-arm64
# running NATIVELY). It is also a third host for the de-host-locking property.
#
# ★ NO HOST DETAILS ARE TRACKED. Precedence: parameter > env > .secrets\arm64-vps.env >
#   FAIL LOUD. `.secrets/` is gitignored because this repo is slated to go public (PR #37).
# ★ KEY-BASED ONLY. `BatchMode=yes` makes ssh FAIL rather than prompt. No key material and
#   no password lives in the repo — `.secrets/` holds the key's PATH, never the key.
# ★ KEY LOCATION IS THE REAL CROSS-HOST WRINKLE: the key normally lives in the WSL home,
#   whose 0600 mode Windows `ssh.exe` cannot honour. So this delegates to WSL by default
#   rather than pretending otherwise. Set DSS_VPS_KEY to a Windows-side path to use
#   `ssh.exe` directly.

[CmdletBinding()]
param(
    [string]  $VpsHost,
    [string]  $VpsUser,
    [string]  $KeyPath,
    [string[]]$Command
)
$ErrorActionPreference = 'Stop'

function Import-DssSecrets([string]$File) {
    $h = @{}
    if (Test-Path $File) {
        foreach ($line in Get-Content $File) {
            if ($line -match '^\s*#' -or $line -notmatch '=') { continue }
            $k, $v = $line -split '=', 2
            $h[$k.Trim()] = $v.Trim()      # $HOME left intact: WSL's shell expands it
        }
    }
    return $h
}
$conf = Import-DssSecrets (Join-Path $PSScriptRoot '..\.secrets\arm64-vps.env')
if (-not $VpsHost) { $VpsHost = if ($env:DSS_VPS_HOST) { $env:DSS_VPS_HOST } else { $conf['DSS_VPS_HOST'] } }
if (-not $VpsUser) { $VpsUser = if ($env:DSS_VPS_USER) { $env:DSS_VPS_USER } else { $conf['DSS_VPS_USER'] } }
if (-not $KeyPath) { $KeyPath = $env:DSS_VPS_KEY }

if (-not $VpsHost -or -not $VpsUser) {
    Write-Error "ssh-arm64-vps: connection data missing. Create .secrets\arm64-vps.env with DSS_VPS_HOST, DSS_VPS_USER, DSS_VPS_KEY (a key PATH), or pass -VpsHost/-VpsUser."
    exit 3
}

$common = @('-o','StrictHostKeyChecking=accept-new','-o','ConnectTimeout=25',
            '-o','ServerAliveInterval=30','-o','BatchMode=yes')

if ($KeyPath) {
    if (-not (Test-Path $KeyPath)) { Write-Error "no private key at '$KeyPath' (DSS_VPS_KEY)."; exit 3 }
    $a = @('-i', $KeyPath) + $common + @("$VpsUser@$VpsHost")
    if ($Command) { $a += $Command }
    & ssh @a
    exit $LASTEXITCODE
}

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    Write-Error "no DSS_VPS_KEY set and wsl.exe is unavailable. Set DSS_VPS_KEY to a Windows-side key, or install WSL where the key lives."
    exit 3
}
$wslKey = if ($conf['DSS_VPS_KEY_WSL']) { $conf['DSS_VPS_KEY_WSL'] } else { '$HOME/.ssh/oracle-vps-private-key' }
$remote = if ($Command) { ' ' + ($Command -join ' ') } else { '' }
& wsl.exe bash -lc "ssh -i $wslKey $($common -join ' ') $VpsUser@$VpsHost$remote"
exit $LASTEXITCODE
