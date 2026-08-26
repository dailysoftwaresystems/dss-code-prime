# PURPOSE: run a DSS gate leg on the operator's macOS host -- push the tree, build clean, run ctest.
#
# Sibling of macos-leg.sh; CAPABILITY-PAIRED (a change to one lands in the other, or the
# pair is broken). Same flags, same defaults, same exit codes, same witness discipline.
#
# ★★ WHY A TWIN EXISTS AT ALL, since the remote half is POSIX by nature. The remote SCRIPT
# runs on macOS and could only ever be shell -- but the DRIVER runs on the host that owns
# this repo's primary gate, which is Windows. The POSIX-only carve-out in the pairing rule
# is for scripts whose EXECUTION is POSIX-only (`wsl-leg` runs inside a WSL distro); this
# one merely TALKS to a POSIX box, so it does not qualify and the twin is owed.
#
# ★ IT NEVER TOUCHES THE OPERATOR'S CHECKOUT UNLESS ASKED. `-ResetTo` is opt-in and named
# for the same reason the .sh gives: a driver that resets someone's checkout as a silent
# default is a driver that eventually resets the wrong one.
#
# ⚠ THE WITNESS IS THE AUTHORITY, NEVER THE EXIT CODE OF A PIPELINE. Both siblings require
# a `REMOTE_CTEST_RC=` line to come back and REFUSE when it is absent. Measured at the P33
# fold: piping a gate through `tail` reported rc=0 while ctest had failed rc=8.
#
# ★★★ THE WITNESS IS MATCHED ON A PER-RUN TOKEN. ✔MEASURED 2026-08-25
# (D-SCRIPT-MACOS-LEG-WITNESS-CAN-BE-ANOTHER-RUN-S-EXIT-CODE): two legs ran against this
# Mac at once, both wrote one log, and `Select-Object -Last 1` over a shared file reads
# whichever run's line happens to sit latest -- a killed leg's `=143` outlived a live
# green one. The reverse, a stale `=0` outliving a live failure, is a FALSE GREEN by the
# same mechanism. A foreign run's line must be UNMATCHABLE, not merely unlikely to win.
#
# ⓘ PAIRING NOTE, since this twin was already half-right: it wrote `macos-leg-$PID.out`
# while the .sh wrote a FIXED path, so only the .sh could be contaminated -- and the .sh
# is what drives the Windows gate. A divergence that makes one twin safer is still a
# divergence, and this one hid the defect from the side that was measuring.
#
# ★★ THE LOCK IS WHAT PREVENTS THE COLLISION; THE TOKEN ONLY MAKES IT VISIBLE. Same
# measurement: the second leg's `rm -rf build/dbg` ran underneath the first leg's LIVE
# ctest, which then spent 2 h 34 m walking a tree being rebuilt under it. The remote half
# now refuses rather than destroying, and a lock whose owning pid is gone is stale.
#
# ★★ PARALLEL ctest, LOST BY BEING WRITTEN AFTER THE FIX THAT LANDED IT.
# `D-SCRIPT-REMOTE-LEG-CTEST-TAKES-THE-REMOTE-SERIAL-DEFAULT` closed this in
# `scripts/remote-leg/remote-leg.sh`: ssh forwards NO environment, so a driver-side
# `CTEST_PARALLEL_LEVEL` never arrives and the remote default is SERIAL. This leg was
# written afterwards and passed no `-j`. The level is 4 by operator ruling (see the jobs
# block) -- never serial, which is the failure that row exists to end, and never all cores,
# because a gate leg is a guest on a personal machine.
#
# ★★ ccache, BECAUSE THE CLEAN BUILD IS CORRECT AND ONLY ITS COST IS THE PROBLEM.
# `rm -rf build/dbg` stays: tar preserves mtimes, so a pushed source can land behind an
# object and ninja silently skips it. ✔MEASURED: 838 targets, ~22 min of a ~35 min leg.
# The answer is a CONTENT-ADDRESSED rebuild decision, not a newly-trusted mtime. ccache is
# ABSENT on this Mac as measured, so this stays inert and prints what to type rather than
# installing software on the operator's machine.
#
# Usage:
#   scripts/macos-leg/macos-leg.ps1                        # push CWD, clean build, full ctest
#   scripts/macos-leg/macos-leg.ps1 -Src <dir>             # push <dir> instead of CWD
#   scripts/macos-leg/macos-leg.ps1 -Filter '<regex>'      # scope the ctest
#   scripts/macos-leg/macos-leg.ps1 -Jobs <n>              # ctest parallelism (default 6)
#   scripts/macos-leg/macos-leg.ps1 -NoPush                # reuse what is already on the Mac
#   scripts/macos-leg/macos-leg.ps1 -ResetTo <commit>      # DESTRUCTIVE: reset the remote checkout first
param(
    [string] $Src,
    [string] $Filter = '',
    [string] $Jobs = '',
    [switch] $NoPush,
    [string] $Dst,
    [string] $ResetTo,
    [switch] $Guards
)
$ErrorActionPreference = 'Stop'

# ★★★ THE REPO GUARDS ARE SKIPPED ON THIS INDIRECT LEG, and until 2026-08-26 this
# sibling COULD NOT SKIP THEM AT ALL. The `.sh` twin has carried
# `[ "${LEG_GUARDS:-0}" = "1" ] || CTEST_ARGS="$CTEST_ARGS -LE repo-guard"` since the
# operator ruling that put guards on the root host only; this file had no `LEG_GUARDS`,
# no `-Guards`, and no `-LE repo-guard` anywhere in it, so every PowerShell-driven
# macOS leg ran the full suite regardless of what was asked for.
# ✔MEASURED at the P36 gate: macOS reported **1671** where WSL and the VPS reported
# **1653**, the difference being exactly the 18 `repo-guard` entries -- ~170 s per leg
# re-checking a source tree the root host had already checked.
# ★ THE COST IS NOT ONLY THE TIME. Two legs both reporting "1671/1671" were not running
# the same 1671, and a figure that silently means something different per host is worse
# than a slower one. D-SCRIPT-MACOS-LEG-PS1-CANNOT-SKIP-THE-REPO-GUARDS
# ⓘ `$env:DSS_LEG_GUARDS` is honoured for parity with the three `.sh` legs, so one
# standing switch still means the same thing everywhere.
$legGuards = if ($Guards) { '1' } elseif ($env:DSS_LEG_GUARDS) { $env:DSS_LEG_GUARDS } else { '0' }

if (-not $Src) { $Src = (Get-Location).Path }
if (-not $Dst) {
    $Dst = if ($env:DSS_MACOS_LEG_DIR) { $env:DSS_MACOS_LEG_DIR } else { '~/src/dss-code-prime' }
}
$carriage = 'scripts/ssh-macos/ssh-macos.ps1'

# ★ `Die` RESTORES THE LEG CLONE BEFORE IT EXITS, which is the PowerShell answer to the
# `.sh` twin's `trap … EXIT`. A leg that dies half way leaves the dirtiest tree of all,
# and that is exactly when the next leg most needs a clean one.
# ⚠ GUARDED THREE WAYS, because `Die` is reachable BEFORE any of them exists -- a
# missing carriage is refused above this point. A cleanup that throws inside a failure
# path replaces the real diagnosis with its own, which is the one thing a `Die` must
# never do; hence the `-ErrorAction SilentlyContinue` probe and the empty catch.
function Die([string]$m) {
    if ((Get-Command Invoke-LegTree -ErrorAction SilentlyContinue) -and $script:legSha -and $Dst) {
        try { Invoke-LegTree 'restore' @($Dst, $script:legSha) | Select-Object -Last 2 } catch { }
    }
    Write-Host ""; Write-Host "[X] macos-leg: $m"; exit 1
}
function Say([string]$m) { Write-Host ""; Write-Host "=== $m ===" }

if (-not (Test-Path $carriage)) { Die "carriage not found at $carriage (run from the repo root)" }

if ($ResetTo) {
    Say "remote fetch + reset --hard $ResetTo"
    # `$Dst` unquoted on the remote side so a leading `~` expands there.
    $cmd = "cd $Dst && git fetch --all --prune -q && git cat-file -e ${ResetTo}^{commit} && " +
           "git reset --hard -q $ResetTo && echo RESET_HEAD=`$(git rev-parse --short HEAD) && " +
           "echo RESET_UNTRACKED=`$(git status --porcelain --untracked-files=all | grep -c '^??')"
    $out = & pwsh -NoProfile -File $carriage -Command $cmd 2>&1
    $head = $out | Select-String -Pattern 'RESET_HEAD=' | Select-Object -Last 1
    if (-not $head) { $out | Write-Host; Die "reset did not report a HEAD - refusing to build on an unknown tree" }
    $head | Write-Host
    $untracked = ($out | Select-String -Pattern 'RESET_UNTRACKED=(\d+)' | Select-Object -Last 1)
    if ($untracked -and [int]$untracked.Matches[0].Groups[1].Value -gt 0) {
        Write-Host ("! {0} untracked file(s) survived the reset - reset --hard removes NONE of them." -f $untracked.Matches[0].Groups[1].Value)
        Write-Host "  A stale tree that git stopped tracking is still visible to every glob in the suite."
    }
}

# ── the leg repository is a CLONE, put on the tree under test ────────────────
# Operator ruling 2026-08-26: every leg host keeps its own clone (`~/src/dss-code-prime`
# here), the leg CHECKS ITS BRANCH before working in it, and the leg cleans up after
# itself. One owner for all three hosts: `scripts/leg-tree/`.
# ★ The script text is INLINED rather than assumed present on the far side -- the Mac's
# checkout can predate this file, and a bootstrap that needs the thing it bootstraps is
# not a bootstrap. `-Raw` keeps it one string; the verb is appended on its own line.
# ⚠ This runs even under `-NoPush`, deliberately: `-NoPush` means "do not replace the
# files", not "do not know which commit they belong to".
function Invoke-LegTree([string] $Verb, [string[]] $Args) {
    $body = Get-Content -Raw (Join-Path $PSScriptRoot '..\leg-tree\leg-tree.sh')
    $quoted = ($Args | ForEach-Object { "'$_'" }) -join ' '
    & pwsh -NoProfile -File $carriage -Command "$body`nleg_tree_$Verb $quoted"
}
$legBranch = (& git -C $Src rev-parse --abbrev-ref HEAD 2>$null)
$legSha    = (& git -C $Src rev-parse HEAD 2>$null)
if (-not $legBranch) { Die "cannot read this checkout's branch - the host's clone is put on the DRIVER's branch, so a driver that cannot name its own has nothing to ask for" }
Say "leg-tree prepare $Dst -> $legBranch @ $legSha"
Invoke-LegTree 'prepare' @($Dst, $legBranch, $legSha)
if ($LASTEXITCODE -ne 0) { Die "leg-tree could not prepare $Dst" }

if (-not $NoPush) {
    Say "push $Src -> $Dst"
    # -Prune, because a LEG's contract is "test THIS tree". Without it the Mac keeps every
    # file this repo has ever deleted and the gate measures a tree that exists nowhere.
    & pwsh -NoProfile -File $carriage -PushSource $Src -PushDest $Dst -Prune
    if ($LASTEXITCODE -ne 0) { Die "push failed" }
}

Say "remote clean configure + build + ctest"
# The remote half goes over STDIN. Measured 2026-08-25 that stdin survives byte-exact to
# this host, so a remote script never has to be quoted into a `-c` string.
$remoteBody = @'
set -uo pipefail
cd "$LEG" || { echo "[X] remote: $LEG missing"; exit 1; }
# Artifacts under build/ -- gitignored AND excluded from the push, so a leg's own logs
# never look like a dirty tree to `--reset-to`'s untracked-file count.
LOGDIR="build/macos-leg/$LEG_RUN"
mkdir -p "$LOGDIR" || { echo "[X] remote: cannot create $LOGDIR"; exit 1; }
echo "logs  : $LEG/$LOGDIR"
# MUTUAL EXCLUSION: a second leg's `rm -rf` deletes the tree the first is testing, and
# then NEITHER verdict is attributable. Refuse; never destroy.
LOCK="build/.macos-leg.lock"
if [ -e "$LOCK" ]; then
    _owner=$(sed -n 's/^pid=//p' "$LOCK" | head -1)
    if [ -n "$_owner" ] && kill -0 "$_owner" 2>/dev/null; then
        echo "[X] remote: another macOS leg owns $LEG/build/dbg"
        echo "    owner pid=$_owner run=$(sed -n 's/^run=//p' "$LOCK" | head -1)"
        echo "    Refusing -- starting here would rm -rf build/dbg underneath a live ctest."
        exit 4
    fi
    echo "! stale lock (pid ${_owner:-?} is gone) -- taking it"
    rm -f "$LOCK"
fi
printf 'pid=%s\nrun=%s\n' "$$" "$LEG_RUN" > "$LOCK"
trap 'rm -f "$LOCK"' EXIT INT TERM
CMAKE=""
for c in /opt/homebrew/bin/cmake /usr/local/bin/cmake; do [ -x "$c" ] && CMAKE="$c" && break; done
NINJA=""
for n in /opt/homebrew/bin/ninja /usr/local/bin/ninja; do [ -x "$n" ] && NINJA="$n" && break; done
[ -n "$CMAKE" ] || { echo "[X] remote: no cmake found on the filesystem"; exit 1; }
[ -n "$NINJA" ] || { echo "[X] remote: no ninja found on the filesystem"; exit 1; }
echo "cmake : $CMAKE ($("$CMAKE" --version | head -1))"
echo "ninja : $NINJA ($("$NINJA" --version))"
echo "cc    : $(/usr/bin/cc --version | head -1)"
# THE DEFAULT IS 4, BY OPERATOR RULING 2026-08-25: "default for testing is -j4 to not use
# 100% of the machine on our tests", amended same-day to 6. A gate leg is a GUEST. -Jobs
# still overrides.
JOBS="${LEG_JOBS:-}"
case "${JOBS:-}" in
    ''|0|*[!0-9]*) JOBS="${DSS_JOBS:-6}" ;;
esac
echo "jobs  : $JOBS"
CCACHE=""
for c in /opt/homebrew/bin/ccache /usr/local/bin/ccache; do [ -x "$c" ] && CCACHE="$c" && break; done
CACHE_ARGS=""
if [ -n "$CCACHE" ]; then
    echo "ccache: $CCACHE ($("$CCACHE" --version | head -1))"
    CACHE_ARGS="-DCMAKE_C_COMPILER_LAUNCHER=$CCACHE -DCMAKE_CXX_COMPILER_LAUNCHER=$CCACHE"
else
    echo "ccache: ABSENT -- every leg recompiles all ~838 targets from scratch (~22 min measured)."
    echo "        The clean build is DELIBERATE and stays: tar preserves mtimes, so an"
    echo "        incremental ninja here can silently skip a pushed source. ccache removes"
    echo "        the COST without trusting an mtime, because it keys on CONTENT."
    echo "        One line, on the Mac:  brew install ccache"
fi
_t=$(date +%s)
_phase() { echo "PHASE $1 $(( $(date +%s) - _t ))s"; _t=$(date +%s); }
rm -rf build/dbg
# shellcheck disable=SC2086
"$CMAKE" -S . -B build/dbg -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_MAKE_PROGRAM="$NINJA" $CACHE_ARGS > "$LOGDIR/configure.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then echo "[X] remote configure rc=$rc"; tail -25 "$LOGDIR/configure.log"; exit 1; fi
echo "configure OK"; _phase configure
"$CMAKE" --build build/dbg --parallel "$JOBS" > "$LOGDIR/build.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then echo "[X] remote build rc=$rc"; grep -iE 'error' "$LOGDIR/build.log" | head -25; exit 1; fi
echo "build OK: $(tail -1 "$LOGDIR/build.log")"; _phase build
CTEST_ARGS="--test-dir build/dbg --output-on-failure -j $JOBS"
[ -n "${LEG_FILTER:-}" ] && CTEST_ARGS="$CTEST_ARGS -R ${LEG_FILTER}"
# THE REPO GUARDS ARE SKIPPED HERE, by operator ruling: this is an INDIRECT leg, the
# guards check the SOURCE TREE, and that tree is the root host's own. `-Guards` (or
# DSS_LEG_GUARDS=1) restores them. This line is the one the `.sh` twin has always had
# and this file never did -- see the header block beside `$legGuards`.
[ "${LEG_GUARDS:-0}" = "1" ] || CTEST_ARGS="$CTEST_ARGS -LE repo-guard"
# shellcheck disable=SC2086
"${CMAKE%cmake}ctest" $CTEST_ARGS > "$LOGDIR/ctest.log" 2>&1
rc=$?
tail -25 "$LOGDIR/ctest.log"
_phase ctest
echo "REMOTE_CTEST_RC[$LEG_RUN]=$rc"
exit $rc
'@

# The token identifies THIS invocation to the witness. $PID alone is not enough - pids
# are reused, and two legs minutes apart on one host can collide.
$legRun = "$PID-" + [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()

# LEG is emitted UNQUOTED so a leading `~` still expands on the remote side.
$payload = "LEG=$Dst`nLEG_FILTER='$Filter'`nLEG_RUN='$legRun'`nLEG_JOBS='$Jobs'`nLEG_GUARDS='$legGuards'`n$remoteBody`n"
$tmp = Join-Path ([IO.Path]::GetTempPath()) "macos-leg-$legRun.out"
$payload | & pwsh -NoProfile -File $carriage -Command 'bash -s' 2>&1 | Tee-Object -FilePath $tmp | Write-Host

# [regex]::Escape, because the token is interpolated into a pattern and `[` is a
# metacharacter - an unescaped one would make this match nothing and read as "no witness".
$pat = 'REMOTE_CTEST_RC' + [regex]::Escape("[$legRun]") + '=(\d+)'
$witness = (Select-String -Path $tmp -Pattern $pat | Select-Object -Last 1)
(Select-String -Path $tmp -Pattern '^PHASE ').Line | Write-Host
Remove-Item $tmp -ErrorAction SilentlyContinue
if (-not $witness) { Die "no REMOTE_CTEST_RC[$legRun] witness came back - this run's real status is UNKNOWN, which is not a pass" }
$rc = [int]$witness.Matches[0].Groups[1].Value
if ($rc -ne 0) { Die "macOS ctest leg FAILED (rc=$rc)" }
# The success path restores too -- `Die` covers every other exit, and between them the
# clone is left pristine however this run ended.
Invoke-LegTree 'restore' @($Dst, $legSha) | Select-Object -Last 2
Say "macOS leg OK (run $legRun)"
