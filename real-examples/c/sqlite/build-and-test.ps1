#!/usr/bin/env pwsh
# real-examples/c/sqlite/build-and-test.ps1
# ─────────────────────────────────────────────────────────────────────────────
# SQLite UNIT-CORPUS harness for DSS Code Prime — Windows pe64, full-source
# (no amalgamation). The pe64 mirror of build-and-test.sh.
#
# Clone the repo and run ONE command to prove DSS Code Prime builds SQLite from
# its REAL sources (no amalgamation) into the Tcl `testfixture.exe` and runs
# SQLite's own `.test` unit corpus on a native Windows pe64 binary.
#
# The pipeline mirrors the .sh, adapted to a Windows host + the single pe64 leg:
#
#   1. verify the host is Windows + WSL is available + online
#   2. use the dss-code-prime checkout AS-IS on its CURRENT branch (never
#      switched/pulled — a probe tests the working tree exactly as it is)
#   3+4. VIA WSL: clone-or-update sqlite/sqlite, configure, and derive the
#      FULL-SOURCE testfixture recipe from the canonical
#      `make -n testfixture USE_AMALGAMATION=0` (the ~185-TU list, the -D
#      defines, the sqlite -I dirs) exactly as the .sh does — then STAGE the
#      sqlite sources + the generated derived sources + the real tcl8.6/zlib
#      HEADERS onto a Windows path, and emit the recipe in Windows paths.
#      (SQLite's build is autotools + tclsh — inherently Unix; the sources are
#      portable C, so the pe64 compile reuses the SAME TU set.)
#   5. locate (or build) a Windows dss-code-prime.exe (Release preferred)
#   6. resolve the pe64 tcl + zlib LIBRARIES the fixture links against: for pe64
#      `--resolve-library` reads a DLL's EXPORT table (`.edata`) — so this points
#      at real DLLs (tcl86.dll + zlib1.dll), NOT an import .lib. git-for-Windows
#      ships both with the full public API; override with $env:TCL_DLL/$env:ZLIB_DLL.
#   7. generate a `.dss-project.json` (language c-subset / profile cli / target
#      x86_64:pe64-x86_64-windows-exec / artifactName testfixture / the 185 TUs
#      as absolute `sources` / the sqlite+tcl+zlib include dirs / the recipe
#      defines / the two DLLs as resolveLibraries) and build it:
#        dss-code-prime --project <manifest> --config=release --output <out>
#      → <out>/pe64-x86_64-windows-exec/testfixture.exe
#   8. run SQLite's `.test` UNIT CORPUS through the dss-built fixture
#      (DSS_TIER: veryquick[default] | quick | full | all), parse
#      "N errors out of M tests", classify failures against the documented
#      non-DSS confounds. GREEN = every failure is a known confound.
#      The corpus runs the ORIGINAL, 100% sqlite suite: nothing is omitted by
#      default. A fixture ABORT does not end the run — it is detected, reported,
#      and RESUMED past through sqlite's own `--start=` / SQLITE_TEST_PATTERN_LIST
#      hooks so every remaining unit still reaches a verdict, while the abort
#      itself stays on the record as a failure (see "THE CORPUS RESUME ENGINE").
#      DSS_TIER_EXCLUDES remains only as an operator escape hatch (QUICKTEST_OMIT);
#      it defaults to EMPTY and any use is reported as a coverage reduction.
#   9. summarise + exit non-zero on any genuine failure / compile miss / crash.
#
# DESIGN: every step is idempotent and FAIL-LOUD. dss-code-prime exits 0 even on
# fatal compile errors, so step 7 reads success from the DIAGNOSTICS (no `error[`
# line) + the emitted binary, never $LASTEXITCODE.
#
# ─────────────────────────────────────────────────────────────────────────────
# ★★ pe64-testfixture — front-end + FFI closure RESOLVED (2026-07-25) ★★
#
# The pe64 full-source testfixture BUILDS + LINKS + RUNS the veryquick unit corpus
# via the pe64 target, DSS parsing the REAL tcl8.6 headers agnostically, the pe64
# `--resolve-library` path (tcl86.dll/zlib1.dll export tables), and the `--project`
# manifest. THREE Windows-only front-end gaps (each `_WIN32`/`SQLITE_OS_WIN`-gated,
# so the Linux/macOS arc never compiled them) were closed as real compiler cycles —
# documented below for the record:
#
#   B1  [config, trivial]  tcl.h / tclDecls.h — the legacy single-underscore
#       MSVC alias `_declspec` is not neutralized. Under DSS's pe profile
#       (_MSC_VER=1943, __GNUC__ undefined) tcl.h picks
#       `#define TCL_NORETURN _declspec(noreturn)` (tcl.h ~line 159); DSS
#       neutralizes `__declspec` (double underscore, c-subset.lang.json:67) but
#       NOT `_declspec`, so it leaks as raw tokens and derails the parser at the
#       first EXTERN declaration. FIX: add a `_declspec` pe predefine mirroring
#       `__declspec` in src/dss-config/sources/c-subset.lang.json:
#         { "name": "_declspec", "kind": "constant", "value": "",
#           "params": ["x"], "availableObjectFormats": ["pe"] }
#
#   B2  [parser, small]  src/test1.c:9346 — `extern LONG volatile sqlite3_os_type;`
#       (inside `#if SQLITE_OS_WIN`; LONG is windows.json's `i32 "long"`). DSS's
#       C parser accepts a type-qualifier AFTER a builtin type-specifier
#       (`int volatile x`) and BEFORE a typedef-name (`volatile LONG x`), but NOT
#       a trailing qualifier on a typedef-name (`LONG volatile x`). Minimal repro
#       (target-agnostic, reproduces on elf64): `typedef long LONG; extern LONG
#       volatile d;`  FIX (grammar): allow a type-qualifier to follow a
#       typedef-name type-specifier (C 6.7.1 — specifiers/qualifiers, any order).
#
#   B3  [FFI surface, multi-cycle]  With B1+B2 cleared the 185-TU compile advances
#       to a body of ~27 unresolved Windows/CRT/math symbols (fail-loud S0001,
#       + cascading S000D member-access errors) referenced by the Windows-gated
#       and Unix-oriented test sources — the pe64 analogue of the Linux arc's
#       per-cycle OS-symbol additions (isnan/pread64/…). The worklist:
#         · kernel32 (test1.c Win section): CreateEvent OpenEvent SetEvent
#           LockFile UnlockFile + EVENT_MODIFY_STATE
#         · msvcrt CRT (test1.c/test_quota.c): _set_abort_behavior _CALL_REPORTFAULT
#           _commit _chsize_s _stati64
#         · CRT low-level I/O by POSIX name (test_fs.c): open close read fstat —
#           test_fs.c:72 gates <unistd.h> on `!_WIN32 || __MSVCRT__`; DSS defines
#           _WIN32 but not __MSVCRT__, so they need <io.h>/<sys/stat.h> pe mappings
#         · math (func.c): log2 trunc acosh asinh atanh   · time (date.c): localtime_r
#       These need new/extended pe shipped-lib descriptors (windows.json / io.json
#       / math.json / time.json …). NOTE a recipe-provenance nuance: this recipe
#       is derived from a LINUX `make -n testfixture`, so some surface reflects
#       Unix-oriented test helpers; a native Windows `nmake -f Makefile.msc`
#       testfixture recipe would compile a somewhat different TU set. The core
#       Win32 (events/locking) + msvcrt surface is genuinely needed either way.
#
# ★ B3 was closed by an AGNOSTIC recipe auto-config (gen-pe64-manifest.py drops the
# Linux host-feature probes `HAVE_*`/`config.h` + adds `SQLITE_OS_WIN`, so SQLite
# self-configures its Windows build → the Unix symbols evaporate), the C99 math +
# `strftime` sourced from ucrtbase.dll (legacy msvcrt is C89), and the genuinely-
# Windows kernel32/msvcrt symbols added to the pe shipped-lib descriptors.
#
# ★ KNOWN remaining pe64 issue (anchored, NOT a DSS bug): `fpconv1-2.0` — legacy
#   msvcrt.dll's `sprintf` renders `%e` to ~17 significant digits while sqlite's own
#   dtoa is full-precision, and fpconv1-2.0 compares the two (a GCC build linking
#   msvcrt.dll fails IDENTICALLY; DSS's codegen was disasm-verified correct). It is
#   resolved by the pe->UCRT migration's Phase 3 (printf-family -> ucrtbase,
#   full-precision dtoa) — D-FFI-PE-CRT-UCRT-MIGRATION.
# ─────────────────────────────────────────────────────────────────────────────
#
# Overridable via env: SQLITE_WSL_DIR  DSS_JOBS  DSS_BIN  SKIP_DSS_BUILD
#                      DSS_TIER  DSS_CONFIG  DSS_TEST_FILE  DSS_CONFOUNDS
#                      DSS_TIER_EXCLUDES  DSS_MAX_RESUMES
#                      DSS_SEGMENT_STALL  DSS_SEGMENT_TIMEOUT
#                      TCL_DLL  ZLIB_DLL  TCL_LIBRARY

$ErrorActionPreference = 'Stop'
# WSL emits UTF-8. Without this, every non-ASCII character in a message coming back
# from the Linux side (the shared-clone lock's operator guidance, for one) is decoded
# with the OEM code page and reaches the console as `?`.
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

# ── logging / fail-loud (mirrors the .sh's step/info/pass/warn/die) ──────────
function Step($m) { Write-Host "`n== $m ==" -ForegroundColor Blue }
function Info($m) { Write-Host "   $m" }
function Pass($m) { Write-Host " [OK] $m" -ForegroundColor Green }
function Warn($m) { Write-Host " [!]  $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host " [X] ERROR: $m" -ForegroundColor Red; exit 1 }

# ── config (override via environment) ────────────────────────────────────────
# This script lives at real-examples/c/sqlite/, so repo root = ../../../.
$RepoRoot     = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$Jobs         = if ($env:DSS_JOBS) { $env:DSS_JOBS } else { [Environment]::ProcessorCount }
$SqliteWslDir = if ($env:SQLITE_WSL_DIR) { $env:SQLITE_WSL_DIR } else { '$HOME/src/sqlite' }
$Work         = Join-Path $RepoRoot 'build\real-examples\c\sqlite\windows'
$Spec         = 'x86_64:pe64-x86_64-windows-exec'
$Fmt          = ($Spec -split ':')[1]
# DSS_TIER: which unit-corpus tier — veryquick (default) | quick | full | all.
$Tier         = if ($env:DSS_TIER) { $env:DSS_TIER } else { 'veryquick' }
# DSS_CONFIG: RELEASE by default (load-bearing — the corpus must exercise the
# full optimizer to catch release-only miscompiles; a debug fixture would run
# green while masking a release bug, and far slower).
$Config       = if ($env:DSS_CONFIG) { $env:DSS_CONFIG } else { 'release' }
# DSS_CONFOUNDS: space-separated .NET-regex patterns for KNOWN non-DSS unit
# failures (a failing test matching any is not counted against green). Same set
# as the .sh: WAL set-lock wall-clock timing, an UPSTREAM zipfile test-isolation
# leak, the recover-fault OOM-oracle class.
# zipfile-25.0 — MECHANISM PROVEN 2026-07-26 (the old "error-text env diff" note
# was WRONG): symlink.test:163 `file mkdir x` is never cleaned up and symlink
# sorts before zipfile, so a DIRECTORY named `x` is present in the shared testdir
# when zipfile-25.0 asserts that `zipfile('x')` fails with "cannot open file: x".
# fopen() on a directory SUCCEEDS, so it fails in fread() instead. Proven by a
# 4-case probe in ONE process varying only the filesystem. Compiler-independent.
# See D-SQLITE-ZIPFILE25-SYMLINK-TESTDIR-LEAK.
# ★ ASYMMETRY, DELIBERATELY NOT "FIXED": the .sh also carries `^date-2\.4c$` and
# this list does not. Adding it here would suppress a failure that has never been
# observed on pe64 — a confound must be EARNED per platform, not copied across.
# Tracked by D-SQLITE-CONFOUND-LIST-DRIVER-ASYMMETRY.
$Confounds    = if ($env:DSS_CONFOUNDS) { $env:DSS_CONFOUNDS -split '\s+' } `
                else { @('^walsetlk-', '^walsetlk\.', '^busy2-', '^zipfile-25\.0$', '^recoverfault') }
# DSS_TIER_EXCLUDES: space-separated regexes naming .test FILES to drop from the
# tier. Delivered through SQLite's OWN upstream hook — the QUICKTEST_OMIT env var
# read by test/permutations.test (~line 152): a COMMA-separated list of Tcl regexes
# matched against each test file's tail name and subtracted from `$allquicktests`,
# the set every permutation in all.test is derived from EXCEPT `full` (which uses
# `$alltests`). So an excluded file still runs ONCE, under `full`, and is dropped
# from the derived permutations.
#
# ★★ DEFAULT EMPTY — ALWAYS, ON EVERY TIER. The requirement is that the ORIGINAL,
# 100% sqlite test suite runs, unmodified and with nothing omitted. The mechanism
# survives only as an explicit operator escape hatch; nothing is excluded unless a
# human sets DSS_TIER_EXCLUDES, and any use is loudly reported as the coverage
# reduction it is (echoed before the run, carried into $unitVerdict, printed in
# Step 9), so no result can ever be read as "the whole corpus ran".
#
# An excluded file is NOT how the harness survives an aborting unit — that is the
# RESUME ENGINE's job (Step 8): an abort is detected, reported, and resumed past
# via sqlite's own `--start=` / SQLITE_TEST_PATTERN_LIST hooks, so every remaining
# unit still reaches a verdict while the abort itself stays on the record as a
# FAILURE. Excluding a file would delete coverage; resuming preserves it.
$TierExcludes = if ($null -ne $env:DSS_TIER_EXCLUDES) { @($env:DSS_TIER_EXCLUDES -split '\s+' | Where-Object { $_ }) } `
                else { @() }
# DSS_MAX_RESUMES: hard bound on how many times Step 8 may re-invoke the fixture
# after an abort. Exceeded → the harness STOPS and says so; it never loops, and it
# never masks a fixture that aborts on everything.
$MaxResumes   = if ($env:DSS_MAX_RESUMES) { [int]$env:DSS_MAX_RESUMES } else { 10 }
# DSS_SEGMENT_STALL: seconds a segment may produce NO log output before the harness
# declares it HUNG, kills it, and resumes past it. A stall bound (not a wall-clock
# bound) is the right shape here: the tiers differ by orders of magnitude — `all`
# runs ~2.5 h — but output is continuous within any of them, so a silent fixture is
# the signal. MEASURED headroom: the slowest single test FILE in a real `all` run is
# sort4.test at 306 s, and the fixture prints a line per TEST, not per file, so real
# output gaps are far shorter still. 1800 s is ~6x the slowest file.
$SegStall     = if ($env:DSS_SEGMENT_STALL) { [int]$env:DSS_SEGMENT_STALL } else { 1800 }
# DSS_SEGMENT_TIMEOUT: optional ABSOLUTE per-segment wall-clock cap in seconds.
# 0 = disabled (the default) — the stall bound above is the one that generalises;
# an absolute cap has to be re-tuned for every tier.
$SegCap       = if ($env:DSS_SEGMENT_TIMEOUT) { [int]$env:DSS_SEGMENT_TIMEOUT } else { 0 }
# DSS_KILL_SETTLE: seconds to wait, after killing a hung segment, for the OS to
# actually release its file handles before the next segment starts. MEASURED: with
# no settle the next segment dies at tester.tcl's startup `reset_db` with
# `error deleting "test.db": permission denied` — the harness manufacturing its own
# next failure out of the previous kill.
$KillSettle   = if ($env:DSS_KILL_SETTLE) { [int]$env:DSS_KILL_SETTLE } else { 20 }
# pe64 resolve-library targets: DLLs with export tables (NOT import .libs).
# git-for-Windows ships a full Tcl 8.6 + zlib runtime; override if you have a
# dedicated Tcl dev kit (Magicsplat/ActiveTcl).
$GitMingwBin  = 'C:\Program Files\Git\mingw64\bin'
$TclDll       = if ($env:TCL_DLL)  { $env:TCL_DLL }  else { Join-Path $GitMingwBin 'tcl86.dll' }
$ZlibDll      = if ($env:ZLIB_DLL) { $env:ZLIB_DLL } else { Join-Path $GitMingwBin 'zlib1.dll' }
# Tcl runtime script library (init.tcl …) — testfixture needs it at RUN time.
$TclLibrary   = if ($env:TCL_LIBRARY) { $env:TCL_LIBRARY } else { 'C:\Program Files\Git\mingw64\lib\tcl8.6' }

$Stage        = Join-Path $Work 'stage'          # the staged sqlite tree + headers
$OutDir       = Join-Path $Work 'out'
$Manifest     = Join-Path $Work 'testfixture.pe64.dss-project.json'
$GenPy        = Join-Path $PSScriptRoot 'gen-pe64-manifest.py'

# Convert C:\path → /mnt/c/path (a backslash Windows path through `wsl wslpath`
# gets its backslashes stripped — do it manually, as the .sh companion does).
function ToWslPath($p) {
  $full = (Resolve-Path -LiteralPath $p).Path
  '/mnt/' + $full.Substring(0,1).ToLowerInvariant() + ($full.Substring(2) -replace '\\','/')
}

# ── Step 1 — host is Windows + WSL + online ──────────────────────────────────
Step '1/9  Host check (Windows + WSL, online)'
if (-not $IsWindows -and $PSVersionTable.PSVersion.Major -ge 6) {
  Die "this harness targets Windows (the pe64 leg); use build-and-test.sh on Linux/WSL/macOS."
}
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) { Die "wsl.exe not found — WSL is required to derive the sqlite recipe (autotools + tclsh). Install WSL + a Debian/Ubuntu distro." }
$probe = & wsl.exe bash -c 'echo wsl-ok' 2>&1
if ($LASTEXITCODE -ne 0 -or "$probe".Trim() -ne 'wsl-ok') { Die "WSL is present but a bash round-trip failed (got: '$probe')." }
$python3 = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $python3) { $python3 = Get-Command python -ErrorAction SilentlyContinue }
if (-not $python3) { Die "python3 not found on PATH — needed to generate the .dss-project.json manifest." }
try {
  $null = Invoke-WebRequest -Uri 'https://github.com' -Method Head -TimeoutSec 20 -UseBasicParsing
} catch { Die "offline — cannot reach https://github.com ($($_.Exception.Message))." }
Info "host: Windows ($([Environment]::OSVersion.Version))   leg: $Spec   tier: $Tier   config: $Config"
Pass "Windows host + WSL online"

# ── Step 2 — dss-code-prime (current checkout, untouched) ────────────────────
Step "2/9  Use dss-code-prime at $RepoRoot (current checkout, untouched)"
$dssHead = (& git -C $RepoRoot rev-parse --short HEAD 2>$null)
$dssBranch = (& git -C $RepoRoot rev-parse --abbrev-ref HEAD 2>$null)
Info "  at $dssHead on $dssBranch"
Pass "dss-code-prime checkout ready"

# ── Step 2b — take the RUN LOCK on the work tree ─────────────────────────────
# MEASURED FAILURE (2026-07-26) this exists to prevent: two harness invocations
# shared $Work. Invocation B's Step 3+4 deleted and re-staged $Stage — the test
# corpus invocation A's fixture was still sourcing .test files from, mid-run — and
# B's Step 7 then died with "Access to the path '…\testfixture.exe' is denied"
# because A's fixture was still EXECUTING that binary. The evidence is unambiguous:
# A's run/ dir and its process both dated 09:49:23, and A's corpus.log was still
# growing (79 MB) at 11:01. So the harness must be single-instance per work tree,
# and silently corrupting a live 2.5 h run is the worse half of that bug.
#
# The lock is LIVENESS-BASED, so a crashed run never wedges the next one: it records
# the owning PID + that process's start time, and a later invocation steals it (and
# SAYS so) when the owner is gone. Correctness never depends on a release.
# >>> dss:run-lock >>>
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$LockDir  = Join-Path $Work '.harness-lock'
$LockFile = Join-Path $LockDir 'owner.txt'
$LockStolen = ''
function Get-LockOwner {
  if (-not (Test-Path $LockFile)) { return $null }
  $t = (Get-Content -Raw -LiteralPath $LockFile -ErrorAction SilentlyContinue)
  if (-not $t) { return $null }
  $parts = $t.Trim() -split '\|'
  if ($parts.Count -lt 2) { return $null }
  return @{ Pid = [int]$parts[0]; Start = $parts[1]; Text = $t.Trim() }
}
function Test-LockOwnerAlive($owner) {
  if (-not $owner) { return $false }
  $p = Get-Process -Id $owner.Pid -ErrorAction SilentlyContinue
  if (-not $p) { return $false }
  # PID reuse guard: the live process must ALSO have the recorded start time.
  try { return ($p.StartTime.Ticks.ToString() -eq $owner.Start) } catch { return $false }
}
$selfStart = (Get-Process -Id $PID).StartTime.Ticks.ToString()
for ($try = 0; $try -lt 2; $try++) {
  try { [void][System.IO.Directory]::CreateDirectory($Work) } catch {}
  $created = $false
  try { $null = New-Item -ItemType Directory -Path $LockDir -ErrorAction Stop; $created = $true } catch { $created = $false }
  if (-not $created) {
    $owner = Get-LockOwner
    if (Test-LockOwnerAlive $owner) {
      Die @"
another dss sqlite harness run is ALREADY ACTIVE on this work tree.
      work tree : $Work
      owner PID : $($owner.Pid)  (still running)
      Two invocations here corrupt each other: Step 3+4 re-stages the .test corpus the
      other run's fixture is sourcing from, and Step 7 cannot delete a testfixture.exe
      that is still executing. Wait for it, or point this run elsewhere by overriding
      the work tree (this script derives it from the repo root).
      If you are certain that PID is dead, remove: $LockDir
"@
    }
    # stale (owner gone, or the PID was reused by something else) — steal + REPORT
    $LockStolen = if ($owner) { "PID $($owner.Pid)" } else { 'an unreadable lock' }
    Remove-Item -Recurse -Force $LockDir -ErrorAction SilentlyContinue
    continue
  }
  break
}
Set-Content -LiteralPath $LockFile -Value "$PID|$selfStart|$(Get-Date -Format o)" -Encoding ascii
if ($LockStolen) { Warn "took over a STALE run lock left by $LockStolen (that run died without releasing it) — reported in the verdict." }
Info "run lock: $LockDir (pid $PID)"
# <<< dss:run-lock <<<

# ── Step 3+4 — derive the full-source recipe + stage to Windows (VIA WSL) ────
Step '3+4/9  Derive full-source testfixture recipe + stage sources/headers (WSL)'
New-Item -ItemType Directory -Force -Path $Work | Out-Null
# NOTE the stage is NOT wiped here. The wipe happens inside the WSL script BELOW,
# after the shared-clone lock is held — otherwise a run that is about to be refused
# has already destroyed its own staged corpus on the way to the refusal.
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
$StageWsl = ToWslPath $Stage
# The WSL derivation reproduces the .sh's Step-4 recipe logic (make -n
# testfixture USE_AMALGAMATION=0 → TUs + defines + -I dirs, with the
# libsqlite3.a core recovered via `ar t`), then STAGES the sqlite sources +
# generated derived sources + real tcl8.6/zlib headers onto the Windows-visible
# $Stage dir and writes the recipe as three files IN WINDOWS PATHS:
#   $Stage/tus.txt  $Stage/includes.txt  $Stage/defines.txt
# (via `wslpath -m`, the forward-slash Windows form the manifest wants).
$deriveScript = @'
set -Eeuo pipefail
__CLONE_LOCK_REGION__
for t in git gcc make ar tclsh; do
  command -v "$t" >/dev/null 2>&1 || {
    echo "MISSING tool: $t — install the recipe toolchain, e.g.:" >&2
    echo "    sudo apt-get install -y git build-essential tcl tcl-dev zlib1g-dev" >&2
    exit 1; }
done
DIR="__SQLITE_WSL_DIR__"
STAGE="__STAGE_WSL__"
# ── SHARED-CLONE WRITE LOCK ─────────────────────────────────────────────────
# THIS is the mutating window: the fetch/checkout/pull below rewrites the very
# .test files a build-and-test.sh corpus run sources LIVE out of this same clone
# for hours. Held only for staging — once the tree is copied to $STAGE the .ps1
# touches the clone no more, so the lock is released when this script ends.
trap dss_clone_lock_release EXIT
dss_clone_lock_write "$DIR" "build-and-test.ps1 pe64 staging (fetch/pull + stage copy)"
echo "CLONE-LOCK=WRITE $DSS_CLONE_LOCK_DIR"
# Only now is anything destroyed: a run that gets refused above still has its
# previous stage intact.
rm -rf "$STAGE"; mkdir -p "$STAGE"
# clone-or-update sqlite (external dependency — DOES pull)
if [ -d "$DIR/.git" ]; then
  git -C "$DIR" fetch --all --prune --quiet || true
  git -C "$DIR" pull --rebase --quiet 2>/dev/null || true
elif [ ! -e "$DIR/configure" ]; then
  mkdir -p "$(dirname "$DIR")"
  git clone --quiet https://github.com/sqlite/sqlite.git "$DIR"
fi
[ -x "$DIR/configure" ] || { echo "no ./configure in $DIR — not a SQLite checkout" >&2; exit 1; }
BLD="$DIR/bld-dss"; mkdir -p "$BLD"
( cd "$BLD" && "$DIR/configure" >/dev/null 2>&1 )
# best-effort reference build → generates every derived .c (parse.c/opcodes.c/
# ctime.c/tclsqlite-ex.c/fts5.c…) + libsqlite3.a (a Tcl-link miss is tolerated —
# we only harvest byproducts + the recipe, not gcc's binary).
( cd "$BLD" && make -s testfixture USE_AMALGAMATION=0 -j"$(nproc 2>/dev/null || echo 4)" >/dev/null 2>&1 ) || true
rm -f "$BLD/testfixture"
RECIPE="$STAGE/testfixture-recipe.txt"
( cd "$BLD" && make -n testfixture USE_AMALGAMATION=0 ) > "$RECIPE" 2>&1 || true
BLOB="$(sed ':a;N;$!ba;s/\\\n/ /g' "$RECIPE" | tr '\t' ' ')"
# defines (-DNAME[=VALUE]) — strip make's literal "" and -D
mapfile -t RECIPE_DEFS < <(printf '%s\n' "$BLOB" | grep -oE '\-D[A-Za-z0-9_]+(=[^ ]*)?' | sed 's/^-D//; s/"//g' | sort -u)
# sqlite -I dirs (drop the bare `.`; it is $BLD, added explicitly below)
mapfile -t SQLITE_INCS < <(printf '%s\n' "$BLOB" | grep -oE '\-I ?[^ ]+' | sed 's/^-I *//' | grep -v '^\.$' | sort -u)
# TU set (1): every .c the recipe names (absolute, or relative → $BLD)
declare -A TU=()
while IFS= read -r c; do
  [ -n "$c" ] || continue
  if   [ -f "$c" ];      then TU["$c"]=1
  elif [ -f "$BLD/$c" ]; then TU["$BLD/$c"]=1
  fi
done < <(printf '%s\n' "$BLOB" | tr ' ' '\n' | grep -E '\.c$' | sort -u)
# TU set (2): the core sources inside libsqlite3.a (DSS can't consume the .a)
AR="$BLD/libsqlite3.a"; [ -f "$BLD/.libs/libsqlite3.a" ] && AR="$BLD/.libs/libsqlite3.a"
if [ -f "$AR" ]; then
  while read -r obj; do
    base="${obj%.o}"
    hit="$(find "$DIR/src" "$DIR/ext" "$BLD" -name "$base.c" 2>/dev/null | grep -v '/tsrc/' | head -1)"
    [ -z "$hit" ] && hit="$(find "$DIR/src" "$DIR/ext" "$BLD" -name "$base.c" 2>/dev/null | head -1)"
    [ -n "$hit" ] && TU["$hit"]=1
  done < <(ar t "$AR" 2>/dev/null | grep '\.o$')
fi
# de-alias generated .c that exist under both bld/ and bld/tsrc/ (dedup by basename)
declare -A SEEN=(); declare -a TUS=()
for f in $(printf '%s\n' "${!TU[@]}" | sort); do
  b="$(basename "$f")"; [ -n "${SEEN[$b]:-}" ] && continue; SEEN[$b]="$f"; TUS+=("$f")
done
[ ${#TUS[@]} -ge 150 ]        || { echo "recipe yielded only ${#TUS[@]} TUs (<150) — see $RECIPE" >&2; exit 1; }
[ ${#RECIPE_DEFS[@]} -ge 18 ] || { echo "recipe yielded only ${#RECIPE_DEFS[@]} defines (<18)" >&2; exit 1; }

# ── stage: sqlite sources + generated derived sources + tcl8.6/zlib headers ──
mkdir -p "$STAGE/sqlite/bld" "$STAGE/tclinc" "$STAGE/zinc" "$STAGE/test"
cp -r "$DIR/src" "$STAGE/sqlite/src"
cp -r "$DIR/ext" "$STAGE/sqlite/ext"
cp "$BLD"/*.c "$STAGE/sqlite/bld/" 2>/dev/null || true
cp "$BLD"/*.h "$STAGE/sqlite/bld/" 2>/dev/null || true
# the .test corpus + its tcl harness (tester.tcl …) — testfixture.exe runs these
cp -r "$DIR/test/." "$STAGE/test/" 2>/dev/null || true
# real tcl8.6 headers (parsed agnostically — NO descriptor, D-FFI-SHIPPED-LIBS-OS-ONLY)
TCLH="$( . "$(find /usr/lib -name tclConfig.sh 2>/dev/null | head -1)" >/dev/null 2>&1; printf '%s' "${TCL_INCLUDE_SPEC#-I}" )"
[ -f "$TCLH/tcl.h" ] || TCLH="$(dirname "$(find /usr/include -name tcl.h -path '*tcl8*' 2>/dev/null | head -1)")"
[ -f "$TCLH/tcl.h" ] || { echo "tcl.h not found in WSL — apt-get install tcl-dev" >&2; exit 1; }
cp "$TCLH"/*.h "$STAGE/tclinc/"
# zlib headers (staged privately so they never shadow anything on -I)
ZH="$(find /usr/include -maxdepth 3 -name zlib.h 2>/dev/null | head -1)"
[ -f "$ZH" ] || { echo "zlib.h not found in WSL — apt-get install zlib1g-dev" >&2; exit 1; }
cp "$ZH" "$STAGE/zinc/"; cp "$(dirname "$ZH")/zconf.h" "$STAGE/zinc/" 2>/dev/null || true
# un-configure the Linux ./configure edit for the pe TARGET: the staged zconf.h has
# `#if 1 → #define Z_HAVE_UNISTD_H` (a ./configure host-probe result ~line 444) which
# forces `#include <unistd.h>` — absent on windows (F001D). Flip ONLY that guard to
# `#if 0` in the STAGED copy (target-appropriate header staging — the zconf.h analog of
# gen-pe64-manifest.py's HAVE_/Z_HAVE_ host-probe drop). The sibling Z_HAVE_STDARG_H
# `#if 1` block is deliberately left intact (<stdarg.h> exists on pe).
perl -0777 -pi -e 's{#if 1(\s*/\* was set to #if 1 by \./configure \*/\s*\n#  define Z_HAVE_UNISTD_H)}{#if 0$1}' "$STAGE/zinc/zconf.h"

# remap a WSL path under $DIR or the header dirs → its STAGED location, then → a
# forward-slash Windows path (wslpath -m).
win() { wslpath -m "$1"; }
remap() {
  local p="$1"
  case "$p" in
    "$DIR"/src/*)  echo "$STAGE/sqlite/src/${p#"$DIR"/src/}" ;;
    "$DIR"/ext/*)  echo "$STAGE/sqlite/ext/${p#"$DIR"/ext/}" ;;
    "$BLD")        echo "$STAGE/sqlite/bld" ;;                    # the -I build dir itself → staged bld
    "$BLD"/*)      echo "$STAGE/sqlite/bld/$(basename "$p")" ;;   # a generated TU under it
    "$DIR"/*)      echo "$STAGE/sqlite/${p#"$DIR"/}" ;;
    "$TCLH")       echo "$STAGE/tclinc" ;;
    "$TCLH"/*)     echo "$STAGE/tclinc/${p#"$TCLH"/}" ;;
    *)             echo "$p" ;;   # already staged / third-party
  esac
}
# tus.txt (Windows paths)
: > "$STAGE/tus.txt"
for f in "${TUS[@]}"; do s="$(remap "$f")"; [ -f "$s" ] && win "$s" >> "$STAGE/tus.txt"; done
# includes.txt: the sqlite -I dirs (remapped) + $BLD + tcl + zlib
: > "$STAGE/includes.txt"
{ echo "$BLD"; for d in "${SQLITE_INCS[@]}"; do echo "$d"; done; } | while read -r d; do
  [ -n "$d" ] || continue; s="$(remap "$d")"; [ -d "$s" ] && win "$s" >> "$STAGE/includes.txt"
done
win "$STAGE/tclinc" >> "$STAGE/includes.txt"
win "$STAGE/zinc"   >> "$STAGE/includes.txt"
# defines.txt
printf '%s\n' "${RECIPE_DEFS[@]}" > "$STAGE/defines.txt"
# the staged test dir (Windows path) for the corpus run
win "$STAGE/test" > "$STAGE/testdir.win.txt"

echo "RECIPE-TUS=$(wc -l < "$STAGE/tus.txt")"
echo "RECIPE-DEFS=$(wc -l < "$STAGE/defines.txt")"
echo "RECIPE-INCS=$(wc -l < "$STAGE/includes.txt")"
echo "SQLITE-HEAD=$(git -C "$DIR" rev-parse --short HEAD 2>/dev/null)"
echo "CLONE-LOCK-NOTES=$DSS_CLONE_NOTES"
'@
# The shared-clone lock is EXTRACTED from build-and-test.sh rather than copied, so
# the two drivers run literally the same lock code against the same key and cannot
# drift apart. Both see this clone as the same WSL path, so the keys agree.
$shHarness = Join-Path $PSScriptRoot 'build-and-test.sh'
if (-not (Test-Path $shHarness)) { Die "build-and-test.sh not found next to this script ($shHarness) — it is the single source of the shared-clone lock." }
$shLines = [System.IO.File]::ReadAllLines($shHarness)
$lockStart = -1; $lockEnd = -1
for ($i = 0; $i -lt $shLines.Count; $i++) {
  if ($lockStart -lt 0 -and $shLines[$i] -match '>>> dss:clone-lock >>>') { $lockStart = $i }
  elseif ($lockStart -ge 0 -and $shLines[$i] -match '<<< dss:clone-lock <<<') { $lockEnd = $i; break }
}
if ($lockStart -lt 0 -or $lockEnd -lt 0) { Die "could not find the 'dss:clone-lock' region in $shHarness (start=$lockStart end=$lockEnd) — the shared-clone lock cannot be injected, and running without it lets this staging step rewrite .test files under a live corpus run." }
$cloneLockRegion = ($shLines[($lockStart + 1)..($lockEnd - 1)]) -join "`n"
$deriveScript = $deriveScript.Replace('__CLONE_LOCK_REGION__', $cloneLockRegion).Replace('__SQLITE_WSL_DIR__', $SqliteWslDir).Replace('__STAGE_WSL__', $StageWsl) -replace "`r`n", "`n"
$tmpSh = Join-Path $Work 'derive.sh'
# UTF-8 WITHOUT a BOM. `-Encoding ascii` (what this used to be) replaces every
# non-ASCII character with `?` — which silently mangled the injected shared-clone
# lock's operator-facing message. A BOM, meanwhile, would break bash on line 1.
[System.IO.File]::WriteAllText($tmpSh, $deriveScript, (New-Object System.Text.UTF8Encoding($false)))
# Merge stderr into stdout INSIDE bash, not with PowerShell's `2>&1`. PowerShell wraps
# a native command's stderr in ErrorRecords, and an EMPTY stderr line then stringifies
# to "System.Management.Automation.RemoteException" while non-ASCII bypasses the
# console encoding and arrives as `?` — mangling exactly the operator-facing text the
# shared-clone lock prints. On stdout it is plain UTF-8 and survives intact.
$deriveOut = @(& wsl.exe bash -l -c "bash '$(ToWslPath $tmpSh)' 2>&1")
if ($LASTEXITCODE -ne 0) {
  if (($deriveOut -join "`n") -match 'DSS-CLONE-LOCK-BLOCKED') {
    Die "the shared sqlite clone is LOCKED by another dss harness run:`n$(($deriveOut | Where-Object { $_ -notmatch 'DSS-CLONE-LOCK-BLOCKED' -and $_.Trim() }) -join "`n")"
  }
  Die "WSL recipe derivation failed:`n$($deriveOut -join "`n")"
}
function Marker($k) { ($deriveOut | Select-String -Pattern "^$k=(.+)$" | Select-Object -Last 1).Matches[0].Groups[1].Value }
$nTus = Marker 'RECIPE-TUS'; $nDefs = Marker 'RECIPE-DEFS'; $nIncs = Marker 'RECIPE-INCS'
$sqliteHead = Marker 'SQLITE-HEAD'
# stale clone locks stolen during staging ride into the verdict like any other
# hygiene event — a theft is never silent.
$CloneLockNotes = @()
$m = ($deriveOut | Select-String -Pattern '^CLONE-LOCK-NOTES=(.+)$' | Select-Object -Last 1)
if ($m) { $CloneLockNotes += "shared-clone lock: $($m.Matches[0].Groups[1].Value.TrimEnd('; ',' '))" }
foreach ($n in $CloneLockNotes) { Warn $n }
Info "clone lock: WRITE taken + released for the staging window (the .ps1 touches the clone only here)"
if (-not (Test-Path "$Stage\tus.txt")) { Die "recipe derivation produced no tus.txt:`n$($deriveOut -join "`n")" }
Pass "recipe: $nTus TUs, $nDefs defines, $nIncs include dirs (sqlite @ $sqliteHead) staged under $Stage"

# ── Step 5 — locate (or build) a Windows dss-code-prime.exe ──────────────────
# Picks the NEWEST existing binary (build-rel Release / build MSVC / build-dbg
# Ninja-Debug) — newest-wins deliberately avoids a STALE Release binary that
# predates a project-config field (the exact trap that fails loud below). With
# no binary present (a fresh clone) it configures + builds Release into build-rel.
Step '5/9  Locate / build dss-code-prime (newest existing, else build Release)'
function Find-Dss {
  $cands = @(
    (Join-Path $RepoRoot 'build-rel\bin\dss\dss-code-prime.exe'),
    (Join-Path $RepoRoot 'build\bin\dss\Release\dss-code-prime.exe'),
    (Join-Path $RepoRoot 'build\bin\dss\dss-code-prime.exe'),
    (Join-Path $RepoRoot 'build-dbg\bin\dss\dss-code-prime.exe')
  ) | Where-Object { Test-Path $_ } | Sort-Object { (Get-Item $_).LastWriteTime } -Descending
  return ($cands | Select-Object -First 1)
}
if ($env:DSS_BIN -and (Test-Path $env:DSS_BIN)) {
  $DssBin = (Resolve-Path $env:DSS_BIN).Path
  Info "using `$env:DSS_BIN — $DssBin"
} elseif ($env:SKIP_DSS_BUILD -eq '1') {
  $DssBin = Find-Dss
  if (-not $DssBin) { Die "SKIP_DSS_BUILD=1 but no dss-code-prime.exe found under build-rel/build/build-dbg." }
  Info "SKIP_DSS_BUILD=1 — reusing $DssBin"
} else {
  $DssBin = Find-Dss
  if (-not $DssBin) {
    Info "no existing binary — configuring + building Release (build-rel)"
    $bdir = Join-Path $RepoRoot 'build-rel'
    if (-not (Test-Path $bdir)) { & cmake -S $RepoRoot -B $bdir -DCMAKE_BUILD_TYPE=Release; if ($LASTEXITCODE -ne 0) { Die "cmake configure failed" } }
    & cmake --build $bdir --config Release --target dss-code-prime -j $Jobs
    if ($LASTEXITCODE -ne 0) { Die "dss-code-prime build failed" }
    $DssBin = Find-Dss
  }
}
if (-not $DssBin -or -not (Test-Path $DssBin)) { Die "dss-code-prime.exe not found." }
$dssAge = (Get-Item $DssBin).LastWriteTime
Info "compiler: $DssBin  (built $dssAge)"
Warn "if the build below fails with a stale-manifest-field error (e.g. unknown 'artifactName'), this binary predates the project-config extensions — rebuild it (delete build-rel or set SKIP_DSS_BUILD off)."
Pass "dss-code-prime located"

# ── Step 6 — pe64 tcl + zlib libraries (resolve-library = DLL export tables) ──
Step '6/9  Resolve-library DLLs (tcl + zlib export surfaces) for pe64'
if (-not (Test-Path $TclDll))  { Die "Tcl DLL not found: $TclDll — install git-for-Windows (ships tcl86.dll) or a Tcl dev kit, or set \$env:TCL_DLL." }
if (-not (Test-Path $ZlibDll)) { Die "zlib DLL not found: $ZlibDll — set \$env:ZLIB_DLL to a zlib1.dll." }
Info "tcl : $TclDll"
Info "zlib: $ZlibDll"
if (-not (Test-Path $TclLibrary)) { Warn "Tcl script library not at $TclLibrary — the corpus RUN needs it (set \$env:TCL_LIBRARY)." }
Pass "pe64 resolve-library DLLs ready"

# ─────────────────────────────────────────────────────────────────────────────
# THE CORPUS RESUME ENGINE — an abort is a RECOVERABLE, REPORTED outcome
# ─────────────────────────────────────────────────────────────────────────────
# The harness exists so that EVERY sqlite unit reaches a verdict. Before this
# engine, a fixture that ABORTED mid-suite (a Tcl `error` out of a test body, a
# hard crash) produced one line — "fixture did not complete the suite (crash?)" —
# and every test FILE behind the abort point was silently never run. One bad unit
# cost the other thousand: two ~2.5h `all` runs died at the same file and taught
# us nothing about the ~7 permutations queued behind it.
#
# DETECT  a segment aborted iff its log has no "N errors out of M tests" summary
#         line. That is the STRUCTURAL fact; the engine is never keyed on a test
#         name or iteration index (the OOM-injection abort point is a function of
#         process-global allocation history — the `all` tier dies at
#         ...oom-persistent.143, the 6s bounded repro at .134).
# LOCATE  from the log: the last COMPLETED file ("Time: <file> N ms"), the
#         permutation ("run_tests <name>" / "run_test_suite <name>" in the Tcl
#         traceback, or the tier's sole suite), and the ABORTING file (resolved
#         from the last emitted test name against the real corpus file list —
#         the traceback's own `(file "…")` frame is NOT usable: Tcl truncates it
#         to ~200 chars, so a long staged path degrades it to `…/test/sw...`).
# RESUME  in a NEW PROCESS (required, not merely convenient: the leaked handle
#         behind this abort class is held for the life of the fixture process),
#         using SQLITE'S OWN upstream hooks — no hand-rolled Tcl runner, nothing
#         written into the sqlite clone or the staged tree:
#           · SQLITE_TEST_PATTERN_LIST (permutations.test ~1175) — a glob list
#             intersected with the permutation's own -files, so passing "every
#             corpus basename after the abort point" selects exactly the
#             permutation's remaining files without the harness ever needing to
#             know that file set.
#           · --start=<permutation>: (tester.tcl ~444 / slave_test_file ~2395) —
#             re-runs THE ORIGINAL tier script, skipping every permutation before
#             the named one, so every `ifcapable`/platform guard in all.test is
#             evaluated by sqlite exactly as in a normal run.
#         So one abort yields at most two segments: the rest of the aborting
#         permutation, then the tier continued from the next permutation.
# BOUND   $MaxResumes (DSS_MAX_RESUMES, default 10). The resume boundary is forced
#         to advance every time, so an aborting file can never be re-entered.
# REPORT  the UNION across segments — total tests, total errors, EVERY abort with
#         its permutation + file, the resume count, and every unit NOT reached.
#         An abort is itself a FAILURE line: resuming never makes it disappear,
#         and a run with aborts is NEVER green.
#
# GRANULARITY (stated because it is a real, reported loss): resume restarts at the
# next FILE. The remainder of the aborting file — the fault-injection iterations
# after the one that died — is NOT run, and is reported as such per abort. sqlite
# exposes no finer restart point than (permutation, file), so a finer resume would
# mean hand-rolling a runner.
#
# >>> dss:corpus-engine >>>  (region mirrored in build-and-test.sh; the verifier
# extracts it from this file by these sentinels, so keep them on their own lines)

# sqlite's own $alltests: every `.test` basename in the corpus dir MINUS the driver
# scripts it excludes by name (all.test / permutations.test / …), ORDINAL-sorted —
# the same order run_tests uses (`lsort $options(-files)`, default -ascii). The
# exclusion list is read as DATA out of permutations.test's own
# `set alltests [test_set $alltests -exclude { … }]` block, never hard-coded; a
# parse miss only widens the list, and the list is used as a SUPERSET filter, so
# the worst case is a wasted resume stepping over a non-unit.
function Get-CorpusFiles($testdir) {
  $skip = @{}
  $perms = Join-Path $testdir 'permutations.test'
  if (Test-Path $perms) {
    $inBlock = $false; $found = $false
    foreach ($line in [System.IO.File]::ReadLines($perms)) {
      if (-not $inBlock) {
        if ($line -match '^\s*set\s+alltests\s+\[test_set\s+\$alltests\s+-exclude\s+\{') { $inBlock = $true; $found = $true }
        continue
      }
      foreach ($w in ($line -split '[\s{}\]]+')) { if ($w -match '\.test$') { $skip[$w] = $true } }
      if ($line -match '\}\s*\]') { break }
    }
    if (-not $found) { Warn "could not read sqlite's \$alltests exclude list from $perms — using the raw *.test glob (a superset; still correct)." }
  }
  $lst = New-Object 'System.Collections.Generic.List[string]'
  Get-ChildItem -LiteralPath $testdir -Filter '*.test' -File |
    ForEach-Object { if (-not $skip.ContainsKey($_.Name)) { $lst.Add($_.Name) } }
  $lst.Sort([System.StringComparer]::Ordinal)
  return $lst
}

# The tier script's permutation sequence, in order, read as DATA from sqlite's own
# `run_test_suite <name>` lines (all.test names 27; veryquick/quick/full name one).
function Get-TierPermutations($tierFile) {
  $names = New-Object 'System.Collections.Generic.List[string]'
  foreach ($line in [System.IO.File]::ReadLines($tierFile)) {
    $m = [regex]::Match($line, '(?<![\w-])run_test_suite\s+([A-Za-z_][A-Za-z0-9_]*)')
    if ($m.Success) { $names.Add($m.Groups[1].Value) }
  }
  return $names
}

# The TEST-NAME PREFIX each suite stamps onto its test names, read as DATA out of
# permutations.test — mirroring tier_prefixes() in build-and-test.sh. Necessary
# because the prefix is NOT derivable from the suite name: permutations.test:39
# defaults it to "<name>." but :220 declares `mmap -prefix "mm-"`, and
# quick/full/threads declare "" (none). The pe64 `all` run emits exactly the
# "mm-…" shape, so without this every ^-anchored confound pattern misses those
# names and a documented confound is reported as a GENUINE failure
# (D-SQLITE-CONFOUND-PERMUTATION-PREFIX). Longest-first so a longer prefix wins
# over one that is merely its head.
function Get-TierPrefixes($permutationsFile) {
  $out = New-Object 'System.Collections.Generic.List[string]'
  if (-not (Test-Path $permutationsFile)) { return $out }
  foreach ($line in [System.IO.File]::ReadLines($permutationsFile)) {
    $mn = [regex]::Match($line, 'test_suite\s+"([^"]*)"')
    if (-not $mn.Success) { continue }
    $mp = [regex]::Match($line, '-prefix\s+"([^"]*)"')
    $p  = if ($mp.Success) { $mp.Groups[1].Value } else { $mn.Groups[1].Value + '.' }
    if ($p -ne '' -and -not $out.Contains($p)) { $out.Add($p) }
  }
  return ($out | Sort-Object -Property Length -Descending)
}

# Single streaming pass over one segment log. (These logs reach 150 MB / 3.6M
# lines — a per-pattern Get-Content sweep would cost minutes per pattern.)
function Read-CorpusSegment($logPath) {
  $r = @{
    Summary = ''; Tests = 0; Errors = 0; FailNames = @{}; Completed = New-Object 'System.Collections.Generic.List[string]'
    LastTest = ''; Permutation = ''; GaveUp = $false; OkLines = 0; FailMarkers = 0
  }
  $reSummary = [regex]'(\d+) errors? out of (\d+) tests'
  $reTime    = [regex]'^Time: (\S+) \d+ ms$'
  $reTest    = [regex]'^(\S+)\.\.\.'
  $reBang    = [regex]'^! (\S+) (expected|got):'
  # NOTE the leading '!': finalize_testing emits `!Failures on these tests: …`
  # (tester.tcl ~1304, `output2 -nonewline "!Failures on these tests:"`).
  $reFails   = [regex]'^!?Failures on these tests:\s*(.+)$'
  $rePerm    = [regex]'^"?(?:run_test_suite|run_tests)\s+([A-Za-z_][A-Za-z0-9_]*)'
  foreach ($line in [System.IO.File]::ReadLines($logPath)) {
    if ($line.Length -eq 0) { continue }
    # Per-test tally. An ABORTED segment never prints a summary, so these counts are
    # the ONLY record of the work it did — see the derivation note at the union.
    if ($line.EndsWith(' Ok', [System.StringComparison]::Ordinal)) { $r.OkLines++ }
    $c = $line[0]
    if ($c -eq 'T') { $m = $reTime.Match($line);  if ($m.Success) { $r.Completed.Add($m.Groups[1].Value); continue } }
    if ($c -eq '!') {
      $m = $reFails.Match($line)
      if ($m.Success) { foreach ($n in ($m.Groups[1].Value -split '\s+')) { if ($n) { $r.FailNames[$n] = $true } }; continue }
      $m = $reBang.Match($line)
      if ($m.Success) {
        $r.FailNames[$m.Groups[1].Value] = $true
        # one `expected:` per FAILED test (`got:` is its partner line) — the failure
        # tally that pairs with OkLines to reconstitute sqlite's own count.
        if ($m.Groups[2].Value -eq 'expected') { $r.FailMarkers++ }
        continue
      }
    }
    if ($c -eq '*' -and $line.StartsWith('*** Giving up')) { $r.GaveUp = $true; continue }
    $m = $reSummary.Match($line)
    if ($m.Success) { $r.Summary = $m.Value; $r.Errors = [int]$m.Groups[1].Value; $r.Tests = [int]$m.Groups[2].Value; continue }
    $m = $rePerm.Match($line.TrimStart()); if ($m.Success) { $r.Permutation = $m.Groups[1].Value; continue }
    $m = $reTest.Match($line); if ($m.Success) { $r.LastTest = $m.Groups[1].Value }
  }
  return $r
}

# Which corpus FILE was the fixture inside when it died? The last test it emitted
# names it: pick the corpus stem that occurs RIGHTMOST in that test name on
# delimiter boundaries (rightmost, then longest). `inmemory_journal.swarmvtabfault
# -1.1-oom-persistent.143` -> swarmvtabfault.test (not swarmvtab.test: the 'f'
# after it is not a delimiter; not the leading permutation token: it is left of it).
function Resolve-AbortFile($lastTest, $corpusFiles) {
  if (-not $lastTest) { return '' }
  $bestFile = ''; $bestIdx = -1; $bestLen = -1
  foreach ($f in $corpusFiles) {
    $stem = $f.Substring(0, $f.Length - 5)   # strip '.test'
    $i = $lastTest.Length
    while ($true) {
      $i = $lastTest.LastIndexOf($stem, [Math]::Min($i, $lastTest.Length - 1), [System.StringComparison]::Ordinal)
      if ($i -lt 0) { break }
      $before = if ($i -eq 0) { '.' } else { [string]$lastTest[$i - 1] }
      $afterI = $i + $stem.Length
      $after  = if ($afterI -ge $lastTest.Length) { '.' } else { [string]$lastTest[$afterI] }
      if (($before -eq '.' -or $before -eq '-') -and ($after -eq '.' -or $after -eq '-')) {
        if ($i -gt $bestIdx -or ($i -eq $bestIdx -and $stem.Length -gt $bestLen)) { $bestIdx = $i; $bestLen = $stem.Length; $bestFile = $f }
        break
      }
      if ($i -eq 0) { break }
      $i = $i - 1
    }
  }
  return $bestFile
}

# Every corpus basename ordinally AFTER $boundary — the SQLITE_TEST_PATTERN_LIST
# superset that sqlite intersects with the permutation's own -files.
function Get-FilesAfter($corpusFiles, $boundary) {
  $out = New-Object 'System.Collections.Generic.List[string]'
  foreach ($f in $corpusFiles) { if ([string]::CompareOrdinal($f, $boundary) -gt 0) { $out.Add($f) } }
  return $out
}

# ── process hygiene ──────────────────────────────────────────────────────────
# Scoped to OUR EXACT fixture binary path — never to the image name. A developer's
# own testfixture.exe, or one belonging to a different checkout, is never touched.
# This is only safe because the run lock guarantees we are the sole invocation on
# this work tree: any process still running THIS path is, by construction, a
# leftover of a run that is already over.
function Get-OurFixtureProcesses($fixturePath) {
  $hits = New-Object 'System.Collections.Generic.List[object]'
  if (-not $fixturePath) { return $hits }
  $want = [System.IO.Path]::GetFullPath($fixturePath)
  foreach ($p in (Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($fixturePath)) -ErrorAction SilentlyContinue)) {
    $path = $null
    try { $path = $p.Path } catch { $path = $null }   # access denied on foreign processes
    if ($path -and ([System.IO.Path]::GetFullPath($path) -eq $want)) { $hits.Add($p) }
  }
  return $hits
}
# Kill every leftover of OUR fixture and return one report line per kill (never
# silent — a killed process is a fact the verdict has to carry).
function Stop-OurFixtures($fixturePath, $why) {
  $killed = New-Object 'System.Collections.Generic.List[string]'
  foreach ($p in (Get-OurFixtureProcesses $fixturePath)) {
    $desc = "pid $($p.Id) (started $($p.StartTime.ToString('s')))"
    try { $p.Kill(); [void]$p.WaitForExit(15000); $killed.Add("$why — killed $desc") }
    catch { $killed.Add("$why — FAILED to kill ${desc}: $($_.Exception.Message)") }
  }
  return $killed
}

# Run ONE fixture segment: stdin at EOF, stdout+stderr to $logPath, killed if it
# stalls (no log growth for $stall s) or exceeds an absolute cap.
#
# stdin is closed deliberately. It is NOT the fix for anything observed — the
# "aborted fixture drops into the Tcl REPL and blocks on stdin" theory was TESTED
# and REFUTED: tclsqlite.c's TCLSH_MAIN evaluates its mainloop script, takes the
# `source $argv0` branch whenever $argv is non-empty, and on error prints errorInfo
# and `return 1`; the `while {![eof stdin]}` REPL is reachable only with NO script
# argument. Measured: with stdin an open pipe that is never written and never
# closed, an aborted fixture still exits rc=1 in 8.8 s. Closing stdin is kept as
# cheap hardening (tester.tcl's own `--pause` does read stdin) — not as a cure.
function Invoke-Fixture($exe, $argv, $workdir, $logPath, $errPath, $stall, $cap) {
  $emptyIn = Join-Path ([System.IO.Path]::GetDirectoryName($logPath)) '.stdin-eof'
  Set-Content -LiteralPath $emptyIn -Value '' -NoNewline -Encoding ascii
  $sp = @{
    FilePath = $exe; ArgumentList = $argv; WorkingDirectory = $workdir
    RedirectStandardOutput = $logPath; RedirectStandardError = $errPath
    RedirectStandardInput = $emptyIn; NoNewWindow = $true; PassThru = $true
  }
  $p = Start-Process @sp
  $t0 = Get-Date; $lastLen = -1L; $lastGrow = Get-Date; $killReason = ''
  while (-not $p.HasExited) {
    Start-Sleep -Seconds 5
    if ($p.HasExited) { break }
    $len = 0L
    try { $len = (Get-Item -LiteralPath $logPath -ErrorAction SilentlyContinue).Length } catch { $len = $lastLen }
    if ($len -ne $lastLen) { $lastLen = $len; $lastGrow = Get-Date }
    elseif ($stall -gt 0 -and ((Get-Date) - $lastGrow).TotalSeconds -ge $stall) {
      $killReason = "produced no output for $stall s (DSS_SEGMENT_STALL)"; break
    }
    if ($cap -gt 0 -and ((Get-Date) - $t0).TotalSeconds -ge $cap) {
      $killReason = "exceeded the absolute cap of $cap s (DSS_SEGMENT_TIMEOUT)"; break
    }
  }
  if ($killReason) {
    try { $p.Kill($true) } catch { try { $p.Kill() } catch {} }   # $true = whole tree
    [void]$p.WaitForExit(30000)
    # SETTLE. MEASURED (on the .sh twin): without this the very next segment dies at
    # tester.tcl's startup `reset_db` with `error deleting "test.db": permission
    # denied` — the killed fixture's handle is still open, so the harness
    # manufactures its own next failure and burns the resume budget on it. Bounded,
    # and only on this path.
    $s = 0
    while ($s -lt $KillSettle -and (Get-OurFixtureProcesses $exe).Count -gt 0) { Start-Sleep -Seconds 1; $s++ }
    Start-Sleep -Seconds 2
  }
  $p.WaitForExit()
  # stderr is appended to the segment log: the Tcl traceback the parser reads is
  # written there, and it is emitted once, at the very end — the same place it sits
  # in a merged stream. A clean segment writes nothing to stderr, so its log is
  # unchanged from what this harness has always produced.
  if ((Test-Path $errPath) -and (Get-Item $errPath).Length -gt 0) {
    Add-Content -LiteralPath $logPath -Value (Get-Content -Raw -LiteralPath $errPath)
  }
  Remove-Item -LiteralPath $emptyIn -ErrorAction SilentlyContinue
  return @{ Rc = $p.ExitCode; KillReason = $killReason; Seconds = ((Get-Date) - $t0).TotalSeconds }
}
# <<< dss:corpus-engine <<<

# ── Step 7 — generate the manifest + build the testfixture (dss --project) ───
Step "7/9  Build the full-source testfixture (dss-code-prime --project, $Config)"
# >>> dss:preflight >>>
# PRE-FLIGHT HYGIENE — FIRST thing in Step 7, before anything is generated or
# deleted. The $OutDir wipe further down cannot remove a testfixture.exe that is
# still executing, and that is exactly how a leftover fixture from a dead run turns
# into "Access to the path … is denied" — an error that looks nothing like its
# cause. We hold the run lock, so anything still running OUR fixture path is a
# leftover by construction.
#
# NOTE the helper functions this calls are defined ABOVE, in the dss:corpus-engine
# region hoisted before this step. PowerShell binds function names in EXECUTION
# order, so a helper defined later in the file simply does not exist here — that is
# a real, measured failure ("The term 'Stop-OurFixtures' is not recognized"), not a
# style point. Keep the region above this line.
$PreflightKills = Stop-OurFixtures (Join-Path $OutDir "$Fmt\testfixture.exe") 'pre-flight'
foreach ($k in $PreflightKills) { Warn "[pe64] LEFTOVER FIXTURE: $k" }
# <<< dss:preflight <<<
$extraDefineArgs = @()
$genArgs = @(
  $GenPy,
  '--tus',      (Join-Path $Stage 'tus.txt'),
  '--includes', (Join-Path $Stage 'includes.txt'),
  '--defines',  (Join-Path $Stage 'defines.txt'),
  '--target',   $Spec,
  '--resolve-library', $TclDll,
  '--resolve-library', $ZlibDll,
  '--artifact-name', 'testfixture'
) + $extraDefineArgs + @('--output', $Manifest)
$genOut = & $python3.Source @genArgs 2>&1
if ($LASTEXITCODE -ne 0) { Die "manifest generation failed:`n$($genOut -join "`n")" }
Info "manifest -> $Manifest ($genOut)"

if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$log = Join-Path $Work 'compile.log'
# dss-code-prime returns exit 0 even on FATAL errors → judge from `error[` + the binary.
& $DssBin --project $Manifest --config="$Config" --output $OutDir --time *>&1 |
  Tee-Object -FilePath $log | Out-Null
$errCount = (Select-String -Path $log -Pattern 'error\[' -AllMatches).Count
$fixture  = Join-Path $OutDir "$Fmt\testfixture.exe"
$ctime = (Get-Content $log | Select-String -Pattern 'compile time (\S+)' | Select-Object -Last 1)
$ctimeSuffix = if ($ctime) { "  ($($ctime.Matches[0].Value))" } else { '' }
if ($errCount -gt 0 -or -not (Test-Path $fixture)) {
  Get-Content $log | Select-String -Pattern 'error\[' | Select-Object -First 5 | ForEach-Object { Info "      $($_.Line)" }
  Die "pe64 build FAILED$ctimeSuffix — $errCount error[...] diagnostic(s); no testfixture.exe. See $log"
}
Pass "testfixture -> $fixture$ctimeSuffix"


# ── Step 8 — run the .test UNIT CORPUS through the fixture ────────────────────
Step "8/9  Run SQLite unit corpus ($Tier.test) + classify failures"
$StagedTestDir = (Get-Content -Raw (Join-Path $Stage 'testdir.win.txt')).Trim()
$TestFile = if ($env:DSS_TEST_FILE) { $env:DSS_TEST_FILE } else { Join-Path $StagedTestDir "$Tier.test" }
if (-not (Test-Path $TestFile)) { Die "test file not found: $TestFile (tier '$Tier')." }
# tcl86.dll + zlib1.dll must be loadable at runtime (put them on PATH); TCL_LIBRARY
# points the Tcl runtime at its script library.
$runEnvPath = (Split-Path $TclDll) + ';' + (Split-Path $ZlibDll) + ';' + $env:PATH
$rundir = Join-Path $Work 'run'; if (Test-Path $rundir) { Remove-Item -Recurse -Force $rundir }
New-Item -ItemType Directory -Force -Path $rundir | Out-Null
$runlog = Join-Path $Work 'corpus.log'
# Tier exclusions — announced BEFORE the run so the reduction is on the record even
# if the fixture never reaches a summary (see $TierExcludes above).
if ($TierExcludes.Count) {
  Warn "[pe64] tier EXCLUSIONS active — this run is NOT full-corpus coverage"
  Info "      QUICKTEST_OMIT=$($TierExcludes -join ',')  (sqlite's own hook, test/permutations.test)"
  Info "      drops these file(s) from every `$allquicktests-derived permutation (still run under 'full'):"
  Info "        $($TierExcludes -join ' ')  (operator-set DSS_TIER_EXCLUDES — the default is EMPTY: the 100% corpus)"
}
$CorpusFiles = Get-CorpusFiles $StagedTestDir
$TierPerms   = Get-TierPermutations $TestFile
$TierPrefixes = Get-TierPrefixes (Join-Path $StagedTestDir 'permutations.test')
$Ledger      = Join-Path $Work 'corpus-units.txt'

# >>> dss:corpus-loop >>>
# Segment queue. Segment 0 is EXACTLY today's invocation (`fixture <tier>.test`)
# so a run with no abort is bit-for-bit the run it always was; resume segments are
# only ever appended by an abort.
$segments   = @(@{ Kind = 'tier'; Args = @($TestFile); Patterns = @(); Label = "$Tier.test"; Perm = '' })
$results    = @()          # one Read-CorpusSegment record per segment actually run
$aborts     = @()          # one record per abort — these NEVER disappear from the verdict
$notReached = @()          # units we can prove were never given a chance
$hygiene    = @()          # leftover/killed fixture processes — never silent
foreach ($k in $PreflightKills) { $hygiene += $k }
if ($LockStolen) { $hygiene += "took over a STALE run lock left by $LockStolen" }
foreach ($n in $CloneLockNotes) { $hygiene += $n }
$resumes    = 0
$lastBoundary = ''
$oldPath = $env:PATH; $oldTclLib = $env:TCL_LIBRARY
$oldOmit = $env:QUICKTEST_OMIT; $oldPatterns = $env:SQLITE_TEST_PATTERN_LIST
$si = 0
while ($si -lt $segments.Count) {
  $seg = $segments[$si]
  $log = if ($si -eq 0) { $runlog } else { Join-Path $Work "corpus.resume$si.log" }
  if ($si -eq 0) { Info "[pe64] running $($seg.Label) via $([System.IO.Path]::GetFileName($fixture)) …" }
  else {
    Info "[pe64] segment $($si + 1): $($seg.Label)$(if ($seg.Patterns.Count) { "  (SQLITE_TEST_PATTERN_LIST: $($seg.Patterns.Count) candidate file(s))" })"
  }
  try {
    $env:PATH = $runEnvPath; if (Test-Path $TclLibrary) { $env:TCL_LIBRARY = $TclLibrary }
    if ($TierExcludes.Count) { $env:QUICKTEST_OMIT = ($TierExcludes -join ',') }
    # SQLITE_TEST_PATTERN_LIST is a Tcl LIST of globs; corpus basenames are
    # bare words, so a space join is a valid list.
    if ($seg.Patterns.Count) { $env:SQLITE_TEST_PATTERN_LIST = ($seg.Patterns -join ' ') } else { $env:SQLITE_TEST_PATTERN_LIST = $null }
    $run = Invoke-Fixture $fixture @($seg.Args) $rundir $log "$log.stderr" $SegStall $SegCap
    $segRc = $run.Rc
  } finally {
    $env:PATH = $oldPath; $env:TCL_LIBRARY = $oldTclLib
    $env:QUICKTEST_OMIT = $oldOmit; $env:SQLITE_TEST_PATTERN_LIST = $oldPatterns
  }
  if ($run.KillReason) { Warn "[pe64] segment $($si + 1) HUNG — killed: $($run.KillReason)"; $hygiene += "segment $($si + 1) TIMED OUT and was killed — $($run.KillReason)" }
  # POST-SEGMENT HYGIENE — a segment that spawned or left a fixture behind must not
  # carry its file handles into the next one (the abort class this engine exists for
  # is a leaked handle), nor outlive the run.
  foreach ($k in (Stop-OurFixtures $fixture "after segment $($si + 1)")) {
    Warn "[pe64] LEFTOVER FIXTURE: $k"; $hygiene += $k
  }
  $res = Read-CorpusSegment $log
  $res.Log = $log; $res.Rc = $segRc; $res.Label = $seg.Label; $res.Kind = $seg.Kind
  $res.KillReason = $run.KillReason
  $results += $res
  $si++
  if ($res.Summary) {
    # A completed segment can still have stopped early: `*** Giving up...` is
    # tester.tcl hitting --maxerror (default 1000) and finalising. It DOES print a
    # summary, so it would otherwise read as a full run. Say so instead.
    if ($res.GaveUp) {
      $lastF = if ($res.Completed.Count) { $res.Completed[$res.Completed.Count - 1] } else { '(none)' }
      Warn "[pe64] segment $si stopped EARLY at the --maxerror cap (`*** Giving up...`) — this is NOT full coverage"
      $notReached += "every file after $lastF in '$($seg.Label)' — the fixture hit its --maxerror cap and finalised early (raise it with --maxerror=N)"
    }
    continue
  }

  # ── ABORT ──────────────────────────────────────────────────────────────────
  $lastDone = if ($res.Completed.Count) { $res.Completed[$res.Completed.Count - 1] } else { '' }
  $perm     = $res.Permutation
  if (-not $perm -and $seg.Perm) { $perm = $seg.Perm }
  if (-not $perm -and $TierPerms.Count -eq 1) { $perm = $TierPerms[0] }
  # A KILLED segment (stall/cap) has NO Tcl traceback at all — and that is precisely
  # when resume matters most, so the traceback cannot be the only source. Fall back
  # to the permutation PREFIX sqlite stamps on every test name (`<perm>.<test>`,
  # run_tests -prefix), then — for an initial tier segment that never showed ANY
  # later permutation's prefix — to the tier's first permutation (all.test's first
  # suite, `full`, is the one declared with -prefix "").
  $permInferred = ''
  if (-not $perm -and $res.LastTest) {
    foreach ($p in $TierPerms) {
      if ($res.LastTest.StartsWith("$p.", [System.StringComparison]::Ordinal)) { $perm = $p; $permInferred = 'from the test-name prefix'; break }
    }
  }
  if (-not $perm -and $res.LastTest -and $seg.Kind -eq 'tier' -and -not $seg.Perm -and $TierPerms.Count -gt 0) {
    $perm = $TierPerms[0]
    $permInferred = "INFERRED — no permutation prefix ever appeared, so the run never left '$($TierPerms[0])', the tier's first suite"
  }
  $abortFile = Resolve-AbortFile $res.LastTest $CorpusFiles
  # The boundary must STRICTLY advance every resume, or an aborting file could be
  # re-entered forever. If the aborting file could not be named (or is not past the
  # last completed one), fall back to the last completed file and then force the
  # boundary one corpus entry forward.
  $boundary = $abortFile
  if (-not $boundary -or [string]::CompareOrdinal($boundary, $lastDone) -le 0) { $boundary = $lastDone }
  $forced = $false
  if ([string]::CompareOrdinal($boundary, $lastBoundary) -le 0) {
    $forced = $true
    $fwd = Get-FilesAfter $CorpusFiles $lastBoundary
    if ($fwd.Count -eq 0) { $boundary = '' } else { $boundary = $fwd[0] }
  }
  $aborts += @{
    Segment = $si; Perm = $perm; File = $abortFile; LastDone = $lastDone
    LastTest = $res.LastTest; Rc = $segRc; Log = $log; Boundary = $boundary
    KillReason = $run.KillReason
  }
  $how = if ($run.KillReason) { "was KILLED after it $($run.KillReason)" } else { "exited rc=$segRc with NO summary line" }
  Warn "[pe64] ABORT #$($aborts.Count) — segment $si ('$($seg.Label)') $how"
  Info  "        permutation        : $(if ($perm) { $perm } else { '(UNDETERMINED)' })$(if ($permInferred) { "   [$permInferred]" })"
  Info  "        last file completed: $(if ($lastDone) { $lastDone } else { '(none)' })"
  Info  "        died inside file   : $(if ($abortFile) { $abortFile } else { '(unresolved)' })   last test: $(if ($res.LastTest) { $res.LastTest } else { '(none)' })"
  # The unit that died NEVER goes unreported — named when we can name it, described
  # by what we do know when we cannot. Silence about a unit is the defect.
  if ($abortFile) {
    $notReached += "the REMAINDER of $abortFile under permutation '$(if ($perm) { $perm } else { '?' })' (aborted at $($res.LastTest))"
  } else {
    $what = if ($forced) { "the resume boundary was FORCED to $(if ($boundary) { $boundary } else { 'the end of the corpus' }), so that one file may have been skipped without a verdict" }
            else { "the next segment resumes from $(if ($boundary) { $boundary } else { 'the end of the corpus' }) and will RE-ATTEMPT it" }
    $notReached += "the UNNAMED file that aborted under permutation '$(if ($perm) { $perm } else { '?' })' after $(if ($lastDone) { $lastDone } else { 'the start of the permutation' }) — the log named no resolvable corpus file (last test: $(if ($res.LastTest) { $res.LastTest } else { 'none' })); $what"
  }
  Get-Content $log -Tail 6 | ForEach-Object { Info "      $_" }

  if (-not $boundary) { Warn "[pe64] the abort is at the END of the corpus file list — nothing left to resume."; continue }
  if (-not $perm) {
    Warn "[pe64] CANNOT RESUME — the aborting permutation could not be determined from the log."
    $notReached += "every unit after $boundary — no resume was possible (permutation undetermined; see $log)"
    continue
  }
  $permIdx = $TierPerms.IndexOf($perm)
  if ($resumes -ge $MaxResumes) {
    Warn "[pe64] RESUME BUDGET EXHAUSTED ($MaxResumes) — stopping. Raise DSS_MAX_RESUMES to go further."
    $notReached += "every unit after $boundary in '$perm'" + $(if ($permIdx -ge 0 -and $permIdx -lt $TierPerms.Count - 1) { " and every permutation after '$perm' ($($TierPerms[($permIdx + 1)..($TierPerms.Count - 1)] -join ' '))" } else { '' }) + " — resume budget ($MaxResumes) exhausted"
    continue
  }
  # (a) the rest of the aborting permutation, via sqlite's own file-selection hook.
  $after = Get-FilesAfter $CorpusFiles $boundary
  $resumes++; $lastBoundary = $boundary
  Info  "        -> resume $resumes/$($MaxResumes): permutations.test $perm, corpus files after $boundary"
  $tail = @(@{ Kind = 'perm'; Args = @((Join-Path $StagedTestDir 'permutations.test'), $perm)
               Patterns = $after; Perm = $perm; Label = "permutations.test $perm (after $boundary)" })
  # (b) the tier continued from the NEXT permutation — the ORIGINAL tier script, so
  # every ifcapable/platform guard is evaluated by sqlite exactly as in a full run.
  if ($seg.Kind -ne 'perm') {
    if ($permIdx -lt 0) {
      Warn "[pe64] permutation '$perm' is not named by $([System.IO.Path]::GetFileName($TestFile)) — cannot continue the tier past it."
      $notReached += "every permutation after '$perm' in $([System.IO.Path]::GetFileName($TestFile)) — '$perm' is not one of its run_test_suite entries"
    } elseif ($permIdx -lt $TierPerms.Count - 1) {
      $next = $TierPerms[$permIdx + 1]
      $tail += @{ Kind = 'tier'; Args = @($TestFile, "--start=${next}:"); Patterns = @(); Perm = $next
                  Label = "$([System.IO.Path]::GetFileName($TestFile)) --start=${next}:" }
      Info  "        -> then: $([System.IO.Path]::GetFileName($TestFile)) --start=${next}:  (permutations $next..$($TierPerms[$TierPerms.Count - 1]))"
    }
  }
  $segments = @($segments[0..($si - 1)]) + $tail + @(if ($si -lt $segments.Count) { $segments[$si..($segments.Count - 1)] })
}

# ── union the segments + classify ────────────────────────────────────────────
# TWO SOURCES, COUNTED SEPARATELY AND NAMED. A segment that ABORTED prints no
# summary line — but it is not an empty segment: the real `all` run's two aborted
# segments between them executed 4.1M passing tests and one genuine failure
# (mm-backup4-3.3). Summing only the summary lines under-reported that run by ~98%
# and would have hidden any regression inside those segments from the totals.
#
# DERIVATION for an aborted segment, from its per-test lines:
#     tests  = (lines ending " Ok") + (`! <name> expected:` lines) + 1
#     errors = (`! <name> expected:` lines)
# The +1 is STRUCTURAL, not a fudge: finalize_testing reports `[incr_ntest]`
# (tester.tcl ~1273), and incr_ntest INCREMENTS THEN RETURNS, so sqlite's own figure
# is always one more than the tests actually run. Every do_test increments that
# counter and prints exactly one of the two line kinds above.
# CALIBRATED: exact (delta 0) against all four real logs that DO carry a summary —
# 46860, 25452, 1 from the `all` run's resumed segments, and 59055 from an unrelated
# 1000-failure run that also hit `*** Giving up`. It is re-checked EVERY run below,
# so it can never quietly drift out of agreement with sqlite's arithmetic.
$sumTests = 0; $sumErrors = 0; $nSummarised = 0
$derTests = 0; $derErrors = 0; $nDerived = 0
$filesDone = 0; $failNames = @(); $calibration = @()
foreach ($r in $results) {
  $filesDone += $r.Completed.Count
  $failNames += $r.FailNames.Keys        # names ALWAYS flow, summary or not
  $r.DerivedTests  = $r.OkLines + $r.FailMarkers + 1
  $r.DerivedErrors = $r.FailMarkers
  if ($r.Summary) {
    $sumTests += $r.Tests; $sumErrors += $r.Errors; $nSummarised++
    # self-calibration: on a segment where sqlite DID report, the derivation must
    # agree. A mismatch means the derived figures for aborted segments are off by a
    # comparable margin, and that gets said out loud rather than assumed away.
    if ($r.DerivedTests -ne $r.Tests) { $calibration += "$($r.Label): sqlite says $($r.Tests) tests, the per-test derivation says $($r.DerivedTests) (delta $($r.DerivedTests - $r.Tests))" }
  } else {
    $derTests += $r.DerivedTests; $derErrors += $r.DerivedErrors; $nDerived++
  }
}
$failNames = @($failNames | Select-Object -Unique)
$totalTests = $sumTests + $derTests
$totalErrors = $sumErrors + $derErrors
function Format-Count($n) { return ([long]$n).ToString('#,##0', [System.Globalization.CultureInfo]::InvariantCulture) }
# For a single clean segment the summary text is the fixture's own, byte for byte.
$summaryText = if ($results.Count -eq 1 -and $results[0].Summary) { $results[0].Summary }
               else { "$totalErrors errors out of $(Format-Count $totalTests) tests (union of $($results.Count) segment(s))" }
# Where the union came from — stated so nobody has to reverse-engineer it.
$derivationText = ''
if ($nDerived -gt 0) {
  $derivationText = "$nSummarised segment summary/summaries: $(Format-Count $sumTests) test(s), $sumErrors error(s) · $nDerived ABORTED segment(s): $(Format-Count $derTests) test(s), $derErrors error(s) counted from per-test lines (' Ok' + '! <name> expected:' + 1 — an aborted segment prints no summary)"
  if ($calibration.Count) {
    $derivationText += "  [!! the derivation DISAGREES with sqlite on $($calibration.Count) segment(s) that did report — treat the aborted-segment figures as APPROXIMATE: $($calibration -join '; ')]"
  }
}
$real = @(); $confound = @(); $scopedExcused = @()
foreach ($t in $failNames) {
  $isc = $false
  # Scoped confounds, mirroring build-and-test.sh: `native:<re>` / `emulated:<re>`
  # (bare = every leg). This driver has only the NATIVE pe64 leg — it runs its
  # binaries directly, with no runner prefix — so an `emulated:` pattern must never
  # excuse anything here. Parsed rather than ignored: without this, a scoped pattern
  # would be treated as the literal regex "emulated:^writecrash-", match nothing,
  # and SILENTLY excuse nothing while appearing configured
  # (D-SQLITE-CONFOUND-LIST-DRIVER-ASYMMETRY).
  $legMode = 'native'
  # Strip the suite's declared test-name prefix before matching, so an ^-anchored
  # pattern still recognises `mm-zipfile-25.0` in the `all` tier as the same
  # confound it recognises as `zipfile-25.0` in `full`.
  $tBare = $t
  foreach ($pfx in $TierPrefixes) {
    if ($t.StartsWith($pfx, [System.StringComparison]::Ordinal)) { $tBare = $t.Substring($pfx.Length); break }
  }
  foreach ($p in $Confounds) {
    $pScope = ''; $pRx = $p
    if ($p -like 'native:*')        { $pScope = 'native';   $pRx = $p.Substring(7) }
    elseif ($p -like 'emulated:*')  { $pScope = 'emulated'; $pRx = $p.Substring(9) }
    if ($pScope -and $pScope -ne $legMode) { continue }
    if ($t -match $pRx -or $tBare -match $pRx) { $isc = $true; if ($pScope) { $scopedExcused += $t }; break }
  }
  if ($isc) { $confound += $t } else { $real += $t }
}
if ($scopedExcused.Count) {
  Warn "[pe64] $($scopedExcused.Count) failure(s) excused ONLY because this leg runs '$legMode': $($scopedExcused -join ' ')"
}
# Per-unit ledger — every file that reached a verdict, every abort, every gap.
$led = New-Object 'System.Collections.Generic.List[string]'
$led.Add("sqlite unit ledger — tier '$Tier', $($results.Count) segment(s), $resumes resume(s)")
$led.Add("union: $summaryText")
if ($derivationText) { $led.Add("   derived from: $derivationText") }
foreach ($r in $results) {
  $led.Add("")
  $led.Add("== segment: $($r.Label)   rc=$($r.Rc)   $(if ($r.Summary) { $r.Summary } else { 'ABORTED (no summary line)' })")
  $led.Add("   log: $($r.Log)")
  # counts + WHERE THEY CAME FROM, per segment
  if ($r.Summary) {
    $led.Add("   tests: $(Format-Count $r.Tests) / errors: $($r.Errors)   [source: sqlite's own summary line; per-test derivation independently gives $(Format-Count $r.DerivedTests)]")
  } else {
    $led.Add("   tests: $(Format-Count $r.DerivedTests) / errors: $($r.DerivedErrors)   [source: DERIVED from per-test lines — $(Format-Count $r.OkLines) ' Ok' + $($r.FailMarkers) '! expected:' + 1; this segment aborted and printed no summary]")
  }
  $led.Add("   files completed ($($r.Completed.Count)): $($r.Completed -join ' ')")
  if ($r.FailNames.Count) { $led.Add("   failing test(s) seen here ($($r.FailNames.Count)): $(($r.FailNames.Keys | Sort-Object) -join ' ')") }
}
if ($calibration.Count) { $led.Add(""); $led.Add("== derivation calibration MISMATCH =="); foreach ($c in $calibration) { $led.Add("   $c") } }
if ($aborts.Count)     { $led.Add(""); $led.Add("== aborts =="); foreach ($a in $aborts) { $led.Add("   segment $($a.Segment): permutation '$(if ($a.Perm) { $a.Perm } else { '?' })' file '$(if ($a.File) { $a.File } else { '?' })' after test '$($a.LastTest)' ($(if ($a.KillReason) { "KILLED: $($a.KillReason)" } else { "rc=$($a.Rc)" })) -> $($a.Log)") } }
if ($notReached.Count) { $led.Add(""); $led.Add("== NOT REACHED (no verdict) =="); foreach ($n in $notReached) { $led.Add("   $n") } }
if ($hygiene.Count)    { $led.Add(""); $led.Add("== process hygiene =="); foreach ($h in $hygiene) { $led.Add("   $h") } }
if ($TierExcludes.Count) { $led.Add(""); $led.Add("== EXCLUDED by operator (DSS_TIER_EXCLUDES -> QUICKTEST_OMIT) =="); $led.Add("   $($TierExcludes -join ' ')") }
Set-Content -LiteralPath $Ledger -Value $led

$unitVerdict = ''; $unitFail = $false
if ($aborts.Count) {
  # An abort is itself a FAILURE. Resuming recovers the units behind it; it never
  # makes the abort disappear, and a run with aborts is NEVER green.
  $where = @(); foreach ($a in $aborts) { $where += "$(if ($a.Perm) { $a.Perm } else { '?' })/$(if ($a.File) { $a.File } else { '?' })" }
  $unitVerdict = "FAIL: $($aborts.Count) fixture ABORT(s) [$($where -join ' ')]; recovered by $resumes resume(s); union: $summaryText"
  if ($derivationText) { $unitVerdict += " [$derivationText]" }
  if ($real.Count)      { $unitVerdict += "; $($real.Count) genuine unit failure(s): $($real -join ' ')" }
  if ($notReached.Count) { $unitVerdict += "; $($notReached.Count) unit group(s) NOT REACHED — see $Ledger" }
  $unitFail = $true
  Warn "[pe64] corpus FAIL — $($aborts.Count) abort(s): $($where -join ' ')"
  Info "      union across $($results.Count) segment(s): $summaryText; $filesDone test file(s) completed"
  if ($derivationText) { Info "        derived from: $derivationText" }
  if ($real.Count) { Info "      $($real.Count) GENUINE DSS failure(s): $($real -join ' ')" }
  foreach ($n in $notReached) { Warn "      NOT REACHED: $n" }
  Info "      per-unit ledger: $Ledger"
} elseif (-not $results[0].Summary) {
  $unitVerdict = "FAIL: fixture did not complete the suite (crash?) — see $runlog"; $unitFail = $true
  Warn "[pe64] corpus FAIL — no summary line (fixture crashed mid-suite); tail:"
  Get-Content $runlog -Tail 6 | ForEach-Object { Info "      $_" }
} elseif ($totalErrors -gt 0 -and $failNames.Count -eq 0) {
  $unitVerdict = "FAIL: $totalErrors error(s) but no failure markers ('Failures on these tests:' / '! <name>') to classify — see $runlog"; $unitFail = $true
  Warn "[pe64] corpus FAIL — $summaryText (unclassifiable — no failure markers)"
} elseif ($real.Count -eq 0) {
  $unitVerdict = "PASS ($summaryText$(if ($confound.Count) { "; $($confound.Count) known confound(s): $($confound -join ' ')" }))"
  Pass "[pe64] corpus GREEN — $summaryText$(if ($confound.Count) { "; all $($confound.Count) failure(s) are known non-DSS confounds: $($confound -join ' ')" })"
} else {
  $unitVerdict = "FAIL: $($real.Count) genuine unit failure(s): $($real -join ' ')"; $unitFail = $true
  Warn "[pe64] corpus FAIL — $summaryText; $($real.Count) GENUINE DSS failure(s): $($real -join ' ')"
  if ($confound.Count) { Info "      (+$($confound.Count) known confound(s) ignored: $($confound -join ' '))" }
}
# A killed zombie / stolen stale lock is a fact about THIS run, not a footnote — it
# rides on the verdict even when the corpus itself came back clean.
if ($hygiene.Count) {
  $unitVerdict += "  [PROCESS HYGIENE: $($hygiene.Count) event(s) — $($hygiene -join '; ')]"
  foreach ($h in $hygiene) { Warn "[pe64] HYGIENE: $h" }
}
# A NOT-REACHED unit is a coverage hole even when nothing failed — never silent.
if ($notReached.Count -and -not $aborts.Count) {
  $unitVerdict += "  [NOT FULL COVERAGE: $($notReached.Count) unit group(s) NOT REACHED — see $Ledger]"
  $unitFail = $true
  foreach ($n in $notReached) { Warn "[pe64] NOT REACHED: $n" }
}
# The exclusion rides along on EVERY verdict — pass and fail alike — so a GREEN
# line can never be read as "the whole corpus ran".
if ($TierExcludes.Count) {
  $unitVerdict += "  [NOT FULL COVERAGE: $($TierExcludes.Count) file pattern(s) EXCLUDED from the $Tier tier via QUICKTEST_OMIT -- $($TierExcludes -join ' ')]"
  Warn "[pe64] the verdict above covers a REDUCED corpus: $($TierExcludes -join ' ') excluded from every `$allquicktests-derived permutation."
}
# <<< dss:corpus-loop <<<

# ── Step 9 — results ─────────────────────────────────────────────────────────
Step '9/9  Results'
Info "compiler : $DssBin @ $dssHead"
Info "sqlite   : $SqliteWslDir @ $sqliteHead   (staged: $Stage)"
Info "recipe   : $nTus TUs, $nDefs defines"
Info "tier     : $Tier.test   outputs: $Work"
Info "excluded : $(if ($TierExcludes.Count) { "$($TierExcludes -join ' ')   (operator DSS_TIER_EXCLUDES -> QUICKTEST_OMIT; dropped from every `$allquicktests-derived permutation, still run under 'full')" } else { '(none — the full tier ran)' })"
# Only printed when there is something to say: a clean single-segment run leaves
# this block byte-identical to what it always was.
if ($results.Count -gt 1 -or $aborts.Count -or $notReached.Count -or $hygiene.Count) {
  Info "segments : $($results.Count) ($resumes resume(s) of max $MaxResumes)   $filesDone test file(s) completed   ledger: $Ledger"
  if ($derivationText) { Info "counts   : $derivationText" }
  foreach ($a in $aborts) { Info "aborted  : permutation '$(if ($a.Perm) { $a.Perm } else { '?' })' file '$(if ($a.File) { $a.File } else { '?' })'$(if ($a.KillReason) { " [KILLED: $($a.KillReason)]" }) — its remaining cases did NOT run  ($($a.Log))" }
  foreach ($n in $notReached) { Info "NOT RUN  : $n" }
  foreach ($h in $hygiene)    { Info "hygiene  : $h" }
}
Info "pe64 ($Spec): compiled   units: $unitVerdict"
# Release the run lock. Correctness does NOT depend on this — the lock is
# liveness-based, so a run that dies here just leaves one the next invocation
# steals and reports. Releasing simply keeps that report quiet when it should be.
Remove-Item -Recurse -Force $LockDir -ErrorAction SilentlyContinue
if ($unitFail) { Write-Host "`n [X] pe64 leg had genuine unit failures (non-confound) — the corpus is not green." -ForegroundColor Red; exit 1 }
Pass "pe64 leg compiled the full-source testfixture + ran the $Tier unit corpus GREEN"
exit 0
