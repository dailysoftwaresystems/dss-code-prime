#!/usr/bin/env pwsh
#Requires -Version 7.0
# ★ THE #Requires ABOVE IS FAIL-LOUD, NOT DECORATION — added TF-C121 after a
# MEASURED first-run failure. The shebang says `pwsh`, but on Windows the shell a
# reader reaches for is `powershell` (Windows PowerShell 5.1, which ships with the
# OS and is what `powershell -File …` resolves to). This script does not PARSE
# under 5.1: `powershell -NoProfile -File build-and-test.ps1` dies with
# `ParserError … MissingEndCurlyBrace` pointing at a line INSIDE a here-string,
# i.e. an error message that names neither the real cause nor the remedy. ✔MEASURED
# 2026-08-05: the identical file returns ERROR_COUNT=0 from
# `[System.Management.Automation.Language.Parser]::ParseFile` under pwsh 7.5.2.
# ⚠ THE INVERTED-INSTRUMENT LESSON: a .NET-parser check run from pwsh reports the
# file CLEAN and is therefore NOT evidence that `powershell -File` can run it — the
# checker inherits the checking shell's grammar. Verify with the shell a user would
# actually type, or declare the requirement so the wrong one refuses loudly. This
# line makes 5.1 print "requires Windows PowerShell 7.0" instead of a parse error
# 800 lines in. (D-HARNESS-PS1-UNPARSEABLE-UNDER-WINDOWS-POWERSHELL-5-1)
# real-examples/c/sqlite/build-and-test.ps1
# ─────────────────────────────────────────────────────────────────────────────
# SQLite UNIT-CORPUS harness for DSS Code Prime — full-source (no amalgamation),
# TARGET-KEYED, MULTI-LEG. The PowerShell twin of build-and-test.sh.
#
# Clone the repo and run ONE command to prove DSS Code Prime builds SQLite from
# its REAL sources (no amalgamation) into the Tcl `testfixture` and runs SQLite's
# own `.test` unit corpus on whichever legs this host can execute.
#
# ★★ THE LEG SET IS A PROPERTY OF THE HARNESS, NOT OF THIS MACHINE ★★
# (D-HARNESS-CROSS-HOST-ANY-TARGET item 2; user hard requirement 2026-07-25:
# "build ANY target inside ANY host — this MUST work".)
# Until TF-C114 this driver opened with `if (-not $IsWindows) { Die ... }` over a
# single hardcoded `$Spec = 'x86_64:pe64-x86_64-windows-exec'`, so it answered
# "which targets does this harness build?" with "whatever this machine is" — the
# exact inverse of the requirement. It now reads the HOST-FREE leg catalogue
# (legs.json) through the ONE shared resolver (harness_legs.py, also consumed by
# build-and-test.sh), gets the SAME five legs on every host, and ATTEMPTS THE
# BUILD OF EVERY ONE OF THEM. The only legitimate host question — "can this host
# EXECUTE this artifact?" — is answered by the resolver's `run.mode`/`run.verdict`
# from the leg's own `runOn` + `launchers` declaration, never by `$IsWindows`.
# Every declared leg reaches a NAMED verdict from the closed vocabulary in
# tests/test_support/arm_verdict_ledger.hpp; silence about a leg is a harness bug.
#
# The pipeline mirrors the .sh:
#
#   1. identify the host (os/arch), locate this HOST's POSIX toolchain + python3,
#      confirm online, then RESOLVE THE LEG PLAN from the catalogue
#   2. use the dss-code-prime checkout AS-IS on its CURRENT branch (never
#      switched/pulled — a probe tests the working tree exactly as it is)
#   3+4. VIA THIS HOST'S POSIX TOOLCHAIN (WSL on a Windows host, the native
#      shell elsewhere): clone-or-update sqlite/sqlite, configure, and derive the
#      FULL-SOURCE testfixture recipe from the canonical
#      `make -n testfixture USE_AMALGAMATION=0` (the ~185-TU list, the -D
#      defines, the sqlite -I dirs) exactly as the .sh does — then STAGE the
#      sqlite sources + the generated derived sources + the real tcl8.6/zlib
#      HEADERS where the compiler can see them, and emit the recipe in paths the
#      manifest can use. (SQLite's build is autotools + tclsh — inherently Unix;
#      the sources are portable C, so EVERY leg's compile reuses the SAME TU set.)
#      The reference gcc build that produces those derived sources is also
#      PRESERVED, as a ONE-SIDED attribution oracle: it is a Linux ELF, so it can
#      EXONERATE a corpus failure (it fails too => upstream) but never convict
#      (it passes => inconclusive on a Windows/CRT difference). Step 9 prints it.
#   5. locate (or build) a dss-code-prime binary (Release preferred)
#   6. PER LEG, resolve the tcl + zlib LIBRARIES that leg's fixture links against
#      from the leg's OWN declared `libraries.provider`. For the pe64 leg
#      (`search-paths`) `--resolve-library` reads a DLL's EXPORT table (`.edata`)
#      — so it points at real DLLs (tcl86.dll + zlib1.dll), NOT an import .lib;
#      git-for-Windows ships both with the full public API; override with
#      $env:TCL_DLL/$env:ZLIB_DLL. A leg declaring `pinned-archive` has its
#      libraries ACQUIRED — downloaded from a checksum-PINNED archive, sliced to
#      that leg's target arch and cached outside the repo — by the SHARED
#      resolver (`harness_legs.py --acquire`), which is how a Windows box obtains
#      Darwin dylibs for the macho legs. A leg whose DECLARED inputs are not on
#      this machine (or cannot be acquired) records `skipped-build-input-missing`
#      — LOUDLY, naming every name and path it searched, or quoting the
#      resolver's refusal — and the run continues with the other legs.
#   7. PER LEG, generate a `.dss-project.json` (language c-subset / profile cli /
#      the leg's target spec / artifactName testfixture / the 185 TUs as absolute
#      `sources` / the sqlite+tcl+zlib include dirs / the recipe defines / that
#      leg's two libraries as resolveLibraries — each carrying the leg's DECLARED
#      runtime identity when its library is an acquired STAND-IN, so the artefact
#      does not record the packager's own install name) and build it:
#        dss-code-prime --project <manifest> --config=release --output <out>/<leg>
#      → <out>/<leg>/<format>/, where the COMPILER names the file and SAYS SO
#      (`dss-code-prime: artifact <spec> <path>`). The suffix belongs to the
#      object format, and this driver no longer holds a copy of that table —
#      see Get-DssReportedArtifact (base-harness.ps1). An ACQUIRED library is then copied BESIDE the
#      artefact, because a `@loader_path/<name>` identity is a claim about that
#      directory and this is what makes it true.
#   8. PER LEG THAT THIS HOST CAN EXECUTE (run.mode native | launched), run
#      SQLite's `.test` UNIT CORPUS through the dss-built fixture
#      (DSS_TIER: veryquick[default] | quick | full | all), parse
#      "N errors out of M tests", classify failures against the documented
#      non-DSS confounds. GREEN = every failure is a known confound. A leg this
#      host cannot execute was still BUILT, and says so beside its skip verdict.
#      The corpus runs the ORIGINAL, 100% sqlite suite: nothing is omitted by
#      default. A fixture ABORT does not end the run — it is detected, reported,
#      and RESUMED past through sqlite's own `--start=` / SQLITE_TEST_PATTERN_LIST
#      hooks so every remaining unit still reaches a verdict, while the abort
#      itself stays on the record as a failure (see "THE CORPUS RESUME ENGINE").
#      DSS_TIER_EXCLUDES remains only as an operator escape hatch (QUICKTEST_OMIT);
#      it defaults to EMPTY and any use is reported as a coverage reduction.
#   9. summarise: a LEDGER LINE naming every verdict class and counting EVERY
#      declared leg (the idiom of ArmVerdictLedger::renderCountsLine()), one
#      detail line per non-verified leg, and exit non-zero on any `poisoned` leg
#      or genuine unit failure. STRUCTURAL skips are reported, never fatal;
#      ENVIRONMENTAL skips warn by default and are FATAL under
#      DSS_STRICT_ARM_VERDICTS=1.
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
#                      DSS_BRANCH  DSS_COMMIT
#                      DSS_SEGMENT_STALL  DSS_SEGMENT_TIMEOUT
#                      TCL_DLL  ZLIB_DLL  TCL_LIBRARY
#                      DSS_LEGS  DSS_STRICT_ARM_VERDICTS
#
# DSS_LEGS    — comma-separated leg LABELS to restrict this invocation to (fast
#               iteration; parity with build-and-test.sh). Unset = every declared
#               leg. A label that matches nothing DIES with the declared list, and
#               every leg it filters OUT is reported as REMOVED COVERAGE — in the
#               Step-9 summary as well as up front, so a filtered run can never be
#               read as the full leg set.
# DSS_STRICT_ARM_VERDICTS — 1|true|TRUE|yes turns an ENVIRONMENTAL skip (a
#               declared build input or a declared launcher absent from THIS
#               machine) into a hard failure. Mirrors readStrictArmVerdicts() in
#               tests/test_support/arm_verdict_ledger.hpp, INCLUDING its
#               malformed-value handling: an unrecognised value is neither "on"
#               nor "off", it is a LOUD refusal to start (read at Step 1, so a
#               typo costs a second rather than a multi-hour corpus run).
#
# DSS_BRANCH  — the branch you INTEND to test. Unset (default) = whatever the
#               checkout is on. Set, and Step 2 ASSERTS it and DIES on a mismatch.
# DSS_COMMIT  — the commit you INTEND to test (full or abbreviated). Unset = no
#               assertion. ⚠ An ASSERTION, never a checkout instruction: this
#               driver NEVER moves our repo (a bare-sha checkout would leave a
#               detached HEAD, contradicting "the checkout AS-IS").
# ★ BOTH ARE PARITY WITH build-and-test.sh:105-121 / :780-799, and the parity is
# load-bearing rather than cosmetic. A CI template or /loop driver that exports
# DSS_COMMIT=<sha> for BOTH legs used to get an assertion on Linux and SILENCE
# here: Windows ignored the variable, built whatever was in the tree, and printed
# a green verdict a reader would take as commit-pinned BECAUSE THE SIBLING LEG WAS.
# A capability in one driver and not the other is a silent harness bug.

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
# Forward slashes, and [IO.Path]::Combine for the work tree: both spell the SAME
# string on Windows (Combine uses the platform separator) and both are the only
# spellings that survive on a POSIX host, where a literal `\` is an ordinary
# filename character rather than a separator.
$RepoRoot     = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Jobs         = if ($env:DSS_JOBS) { $env:DSS_JOBS } else { [Environment]::ProcessorCount }
$SqliteWslDir = if ($env:SQLITE_WSL_DIR) { $env:SQLITE_WSL_DIR } else { '$HOME/src/sqlite' }
# ★ NOT a target directory and NOT a claim about the host: `windows` names THIS
# DRIVER's work tree (the .sh owns a sibling), kept as-is so an operator's paths
# and every log reference stay valid. The per-LEG outputs live under out/<label>/.
$Work         = [System.IO.Path]::Combine($RepoRoot, 'build', 'real-examples', 'c', 'sqlite', 'windows')
# ★★ THERE IS NO `$Spec` / `$Fmt` ANY MORE, AND THAT ABSENCE IS THE POINT.
# They were a single hardcoded pe64 target, which made "which target does this
# harness build?" a question this file answered instead of the catalogue. The
# leg set now comes from legs.json via harness_legs.py (Step 1b, `$Legs`), and
# every target-shaped value below is read off the LEG being processed.
$LegsPy       = Join-Path $PSScriptRoot 'harness_legs.py'
# The catalogue itself, named explicitly rather than left to the resolver's
# default: stage-zinc.py takes it too, and both must be reading the SAME file.
$LegsJson     = Join-Path $PSScriptRoot 'legs.json'
# Legs whose corpus ran but left a DECLARED capability's witness file inert.
# Run-wide rather than per-leg: the capability set is a property of the ONE
# staged tree every leg compiles, so a gap on any leg indicts the stage.
$script:CapabilityGaps = @()
# The deferred-anchor registry, consulted AT the point a leg fails
# (D-PROCESS-CHECK-THE-REGISTRY-FOR-A-MATCHED-CONTROL-BEFORE-COMMISSIONING-ONE).
# Not required to exist: a checkout without .plans still runs, it just gets one
# line saying the lookup found nothing to read.
$AnchorRegistry = Join-Path $RepoRoot '.plans/_deferred-anchor-registry.md'
# DSS_TIER: which unit-corpus tier — veryquick (default) | quick | full | all.
$Tier         = if ($env:DSS_TIER) { $env:DSS_TIER } else { 'veryquick' }
# DSS_CONFIG: RELEASE by default (load-bearing — the corpus must exercise the
# full optimizer to catch release-only miscompiles; a debug fixture would run
# green while masking a release bug, and far slower).
$Config       = if ($env:DSS_CONFIG) { $env:DSS_CONFIG } else { 'release' }
# DSS_CONFOUNDS: space-separated .NET-regex patterns for KNOWN non-DSS unit
# failures (a failing test matching any is not counted against green).
#
# ★★★ THE LIST IS NOT IN THIS FILE ANY MORE, AND THAT IS THE WHOLE FIX.
# [D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG,
#  D-HARNESS-SQLITE-CONFOUNDS-NOT-DECLARED-PER-LEG,
#  D-SQLITE-CONFOUND-LIST-DRIVER-ASYMMETRY.]
#
# What stood here was `$PeEarnedConfounds` plus a `Get-LegConfounds` whose whole
# body was `if ($legLabel -eq 'pe64-x86_64')`, and it was wrong in BOTH directions
# at once. Its six patterns were earned on LINUX x86_64 — the comment it carried
# conceded as much for two of them, and `zipfile-25.0`'s mechanism is a glibc
# `fopen()` on a DIRECTORY succeeding, which cannot happen on Windows at all — so
# this driver handed a Linux-earned list to the one leg that could not use it and
# withheld it from the legs that had earned it. Meanwhile build-and-test.sh
# applied ONE global list to every leg. ✔MEASURED consequence: the SAME
# elf64-x86_64 artefact's `zipfile-25.0` was a "known non-DSS confound" under one
# driver and a "genuine failure" under the other, in the same project on the same
# day — so no two legs' genuine-failure counts were comparable, and that count is
# what every verdict this harness renders rests on.
#
# ⇒ the earned set is DECLARED PER LEG in legs.json, each pattern carrying the
# leg + host + date + mechanism that earned it and the anchor holding the long
# form; harness_legs.py's lint REFUSES a pattern with no provenance, and both
# drivers read that one declaration. `[]` is a real answer, made out loud.
#
# ⓘ WHAT THIS VARIABLE STILL DOES: an operator OVERRIDE, which deliberately
# applies to EVERY leg — naming a pattern on the command line is stating intent
# for this run, not inheriting one — and which is announced as such per leg so a
# reader of the log can never mistake it for the earned set.
$ConfoundsOverride = if ($env:DSS_CONFOUNDS) { @($env:DSS_CONFOUNDS -split '\s+' | Where-Object { $_ }) } else { $null }
# THE SUPPLY, IN ONE PLACE, KEYED ON THE LEG'S OWN DECLARATION AND ON NOTHING
# ELSE — no label, no host, no format. `$leg` is the RESOLVED leg from
# harness_legs.py --plan, whose `confounds` field is the catalogue's rows already
# rendered into the `native:`/`emulated:` wire grammar both drivers speak.
# ⚠ A leg WITHOUT the field is a resolver/driver transport defect, never an empty
# list: the resolver refuses to plan a leg that does not declare `confounds`, so
# the field being absent HERE means the plan this driver read is not the plan that
# file produces. Failing loud is the only honest answer — silently substituting
# `@()` would report "every failure on this leg counts" on the strength of a bug.
function Get-LegConfounds($leg) {
  if ($null -ne $ConfoundsOverride) { return $ConfoundsOverride }
  if ($null -eq $leg.PSObject.Properties['confounds']) {
    Die "[$($leg.label)] the resolved leg plan carries NO ``confounds`` field. harness_legs.py refuses to plan a leg that does not declare one, so this is a transport defect between the resolver and this driver — not a leg with nothing earned. Treating it as an empty list would silently report every failure on this leg as a DSS defect. [D-HARNESS-CONFOUND-LEDGER-IS-PER-DRIVER-NOT-PER-LEG]"
  }
  return @($leg.confounds | Where-Object { $_ })
}
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
# ★ THE resolve-library BINARIES ARE PER LEG AND ARE RESOLVED IN STEP 6 from the
# leg's OWN declared `libraries.provider` / `tclNames` / `zNames` / `searchPaths`.
# There is deliberately no `$TclDll`/`$ZlibDll` global any more: one pair of
# globals silently WAS the pe64 leg's answer, so a second leg would have linked
# against Windows DLLs. $env:TCL_DLL / $env:ZLIB_DLL survive as explicit operator
# overrides for the `search-paths` provider (today: exactly the pe64 leg) — see
# Resolve-LegLibraries.
#
# Tcl runtime script library (init.tcl …) — the fixture needs it at RUN time, so
# this is a fact about the machine EXECUTING a leg, not about any target. It is
# applied only to a leg this host actually runs. The default is the
# git-for-Windows tree that ships the tcl86.dll the pe64 leg links; override with
# $env:TCL_LIBRARY on any other host.
$TclLibrary   = if ($env:TCL_LIBRARY) { $env:TCL_LIBRARY } else { 'C:\Program Files\Git\mingw64\lib\tcl8.6' }

# ── THE RUNTIME DATA an ACQUIRED library needs, which its CODE does not carry ──
# [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY]
#
# ✔MEASURED on the operator's Mac at 11e97e0e (in the .sh sibling, but the defect
# is the mechanism's, not that driver's): the macho64-arm64 fixture BUILT and ran
# `select1.test` correctly - "0 errors out of 192 tests" - and the TIER died
# instantly with `Can't find a usable init.tcl in the following directories:
# /opt/local/lib/tcl8.6`, because permutations.test runs every unit in a FRESH
# SLAVE interpreter and `interp create` re-enters `tclInit`, which needs Tcl's
# SCRIPT LIBRARY. The acquired MacPorts dylib bakes in MacPorts' own prefix, and
# only the dylib was ever downloaded.
#
# $TclLibrary above is the HOST's copy and stays exactly what it always was: the
# right answer for a `host-system`-provider leg and for nothing else. A leg whose
# Tcl was ACQUIRED gets the STAGED directory instead - target-keyed, leg-scoped.
#
# THE CONTRACT FIELD, NAMED ONCE - ✔READ FROM harness_legs.py, NOT GUESSED.
# `acquisition_record()` returns a TOP-LEVEL `scriptLibraryDir`, derived by
# `_script_library_dir()` from the ONE `dataDirs` entry whose `role` is
# `tclScriptLibrary` (`DATA_DIR_ROLES`, singled out there precisely because "a
# driver has to point TCL_LIBRARY at it"). It is on the SUCCESS and FAILURE
# returns alike - one record type, so "a field cannot be forgotten on the branch
# nobody exercises". This driver reads the resolved field and never re-derives it
# from `dataDirs`, so a rename is a one-line change here and in the .sh.
$AcqScriptLibraryKey = 'scriptLibraryDir'
# ⚠ THE KEY IS A PARAMETER, NOT AN AMBIENT `$script:` READ. Two reasons, the second
# measured: (a) it mirrors the .sh, which passes `optional:$ACQ_SCRIPT_LIBRARY_KEY`
# at the call site, so the two drivers read the contract the same way; and (b) an
# ambient variable that is not in scope makes `PSObject.Properties[$null]` throw
# "Index operation failed; the array index evaluated to null" - the exact trap this
# file already records for the launcher-environment read (Get-LegDeclaredEnvNames).
function Get-AcquiredScriptLibrary($acq, $key) {
  if (-not $key) { Die "Get-AcquiredScriptLibrary was called with no contract field name. The acquisition report field is `$AcqScriptLibraryKey; a lookup with an empty key cannot distinguish 'no script library' from 'this driver forgot which field to read'." }
  # ABSENT or EMPTY is not an error HERE - a leg whose acquired library needs no
  # runtime data legitimately has none - so this returns '' and the CALLER decides.
  $p = $acq.PSObject.Properties[$key]
  if (-not $p) { return '' }
  return "$($p.Value)"
}
# ── THE RUNTIME LOADER'S SEARCH VARIABLE, PER TARGET ────────────────────────
# [D-HARNESS-RUN-ENV-LD-LIBRARY-PATH-INERT-ON-DARWIN + item (c) of
#  D-HARNESS-ACQUIRE-ERGONOMIC-GAPS, which asked for the choice to be made in ONE
#  place, target-keyed.]
#
# There is no such thing as "the" loader search variable: ld.so reads
# LD_LIBRARY_PATH, dyld reads DYLD_LIBRARY_PATH and IGNORES the ELF one, and the
# Windows loader reads neither - it searches the EXECUTABLE'S OWN DIRECTORY, which
# is where Step 7 already stages this leg's acquired DLLs, and PATH is the
# next-best approximation this driver has always used for it.
#
# THE OS COMES FROM THE RESOLVED PLAN. `build.configStageKey` is
# `spec_target_os(spec)` - harness_legs.py's `configure_stage_key`: "the staged-
# configure-header directory name for this leg: its TARGET OS", and it RAISES
# rather than defaulting. The format token is CROSS-CHECKED against it rather than
# used as the source: the same declare-then-cross-check discipline zconfGuards
# uses. The .sh twin holds the identical table; the RIGHT long-term home is a
# declared loader-variable name emitted by the shared resolver, so neither driver
# holds it at all.
# ★★ AND THE SEPARATOR IS THE SAME QUESTION, WHICH THIS TABLE USED NOT TO ANSWER.
# The variable holds a LIST, and who splits it is the TARGET's loader: `;` for the
# Windows loader, `:` for ld.so and for dyld. ✔MEASURED 2026-08-07: this driver
# joined that list with `[System.IO.Path]::PathSeparator` — a property of the HOST
# it happens to be running on, `;` on Windows — and handed the result to an ELF
# leg, whose ld.so reads the whole thing as ONE directory named `C:\a;C:\b` and
# reports `libtcl8.6.so: cannot open shared object file` beside two libraries that
# are staged and present. Keyed on the target for the same reason the NAME is: the
# process that parses the value is the target's loader, never this driver.
function Get-LegLoaderPathSpec($leg) {
  $os = "$($leg.build.configStageKey)"
  # `<container><bits>-<arch>-<os>-<kind>`: the OS is the second-to-last token.
  $parts = @("$($leg.format)".Split('-'))
  $fmtOs = if ($parts.Count -ge 3) { $parts[$parts.Count - 2] } else { '' }
  if (-not $os) { Die "[$($leg.label)] the resolved plan carries no target OS (build.configStageKey is empty). The runtime loader's search variable is a property of the TARGET; choosing one without knowing the target is how LD_LIBRARY_PATH came to be exported for a Darwin leg that cannot read it." }
  if ($os -ne $fmtOs) { Die "[$($leg.label)] the resolved plan disagrees with itself about this leg's target OS: configStageKey says '$os', the object format '$($leg.format)' says '$fmtOs'. One of them decides which loader variable this leg's libraries are exported under." }
  switch ($os) {
    'windows' { return [pscustomobject]@{ Name = 'PATH';              Separator = ';' } }
    'linux'   { return [pscustomobject]@{ Name = 'LD_LIBRARY_PATH';   Separator = ':' } }
    'darwin'  { return [pscustomobject]@{ Name = 'DYLD_LIBRARY_PATH'; Separator = ':' } }
    default   { Die "[$($leg.label)] target OS '$os' has no declared runtime-loader search variable in this driver. Known: windows (PATH) | linux (LD_LIBRARY_PATH) | darwin (DYLD_LIBRARY_PATH). A new target OS must DECLARE its answer; defaulting to one spelling is the defect this function exists to end." }
  }
}
# The NAME alone — for the callers that only save, restore or forward the variable
# and never build its value. ONE table, two questions: a second switch keyed on the
# same target OS is how the two answers would come to disagree.
function Get-LegLoaderPathVar($leg) {
  return (Get-LegLoaderPathSpec $leg).Name
}

$Stage        = Join-Path $Work 'stage'          # the staged sqlite tree + headers
# PER-LEG output root: <Work>/out/<label>/… . Nothing is written to `out/` itself,
# so one leg's wipe can never reach a sibling's just-built artifact.
$OutRoot      = Join-Path $Work 'out'
$GenPy        = Join-Path $PSScriptRoot 'gen-pe64-manifest.py'
$CliSmokePy   = Join-Path $PSScriptRoot 'cli-smoke.py'
# ── THE SHARED CORE ──────────────────────────────────────────────────────────
# base-harness.ps1 holds the artifact read-back, the manifest/build wrappers and
# the per-(leg, artifact) verdict ledger — the decisions this driver and
# build-and-test.sh BOTH make, which used to be written out once per driver and
# had already drifted three measured ways (base-harness.sh's header names them).
#
# base-harness.SH is the other half, and this driver reaches it too: the recipe
# derivation has always run in a POSIX shell here, so the $deriveScript below
# SOURCES it rather than carrying a second copy of the same sed/ar/dedup logic.
# That is what makes the two drivers capability-paired instead of merely similar.
$BaseHarnessPs1 = Join-Path $PSScriptRoot 'base-harness.ps1'
$BaseHarnessSh  = Join-Path $PSScriptRoot 'base-harness.sh'
foreach ($f in @($BaseHarnessPs1, $BaseHarnessSh)) {
  if (-not (Test-Path -LiteralPath $f)) {
    Write-Host " [X] ERROR: the shared harness core is missing: $f" -ForegroundColor Red
    Write-Host "     It carries the recipe derivation, the artifact read-back and the verdict"
    Write-Host "     ledger that this driver and build-and-test.sh SHARE. Restore it rather than"
    Write-Host "     reintroducing a private copy — three hand-kept copies of one decision is what"
    Write-Host "     it was extracted to end."
    exit 1
  }
}
. $BaseHarnessPs1
# The contract version this driver was written against. A stale core would
# otherwise present itself as a MISSING CAPABILITY — silently skipping the CLI,
# say — which is the failure class the shared core exists to make impossible.
if ($script:DssBaseHarnessVersion -lt 1) {
  Write-Host " [X] ERROR: $BaseHarnessPs1 is version $($script:DssBaseHarnessVersion); this driver needs >= 1." -ForegroundColor Red
  exit 1
}

# Convert C:\path → /mnt/c/path for a WSL callee (a backslash Windows path through
# `wsl wslpath` gets its backslashes stripped — do it manually, as the .sh
# companion does). On a POSIX host there is nothing to convert: the path this
# driver holds IS the path the shell will see, so this is the identity. Which of
# the two applies is decided ONCE, in Step 1, from the host's identity — the one
# question a host is allowed to answer.
function ToShellPath($p) {
  $full = (Resolve-Path -LiteralPath $p).Path
  if (-not $script:HostNeedsWsl) { return $full }
  '/mnt/' + $full.Substring(0,1).ToLowerInvariant() + ($full.Substring(2) -replace '\\','/')
}

# Run ONE command line through THIS HOST's POSIX toolchain — the same channel
# Step 3+4 derives the recipe through, chosen by the same Step-1 decision.
# Returns @{ Rc; Out }; the rc is taken DIRECTLY off the native call.
#
# ★★ `-e` IS LOAD-BEARING — D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL.
# `wsl.exe <cmd>` WITHOUT it does NOT run <cmd>: WSL reconstructs a command LINE
# and hands it to the distro's DEFAULT SHELL, which strips quoting and expands
# ON THIS MACHINE before bash ever sees `$bashLine`. MEASURED 2026-08-04, one
# variable changed, same input string:
#   wsl.exe    bash -lc "printf '[%s]\n' 'echo A=$(uname -m)'" -> [echo A=x86_64]
#   wsl.exe -e bash -lc "printf '[%s]\n' 'echo A=$(uname -m)'" -> [echo A=$(uname -m)]
# $bashLine is a CALLER-SUPPLIED command line, so every `$`, backslash, glob and
# quote in it was being evaluated twice — the second evaluation invisible.
function Invoke-PosixCommand($bashLine) {
  # stderr merged INSIDE bash, never with PowerShell's `2>&1`: PowerShell wraps a
  # native command's stderr in ErrorRecords, an EMPTY stderr line then stringifies
  # to "System.Management.Automation.RemoteException", and non-ASCII bypasses the
  # console encoding and arrives as `?`. Same reasoning as the derive invocation.
  # (That is a DIFFERENT layer from `-e`: `-e` decides who parses the string,
  # `2>&1` inside bash decides who merges the streams. Both are needed.)
  if ($script:HostNeedsWsl) { $out = @(& wsl.exe -e bash -l -c "$bashLine 2>&1") }
  else                      { $out = @(& bash         -l -c "$bashLine 2>&1") }
  return @{ Rc = $LASTEXITCODE; Out = $out }
}

# ── THE LAUNCHER'S PATH NAMESPACE ────────────────────────────────────────────
# D-HARNESS-NO-WSL-LAUNCHER-FOR-ELF-ON-WINDOWS.
#
# A launcher does not always address files the way this driver does. Wine on
# Linux does (`wine /home/me/x.exe`); `wsl.exe` does NOT — this driver holds a
# drive-letter path and the callee needs `/mnt/c/...`. THE FAILURE MODE IS THE
# REASON THIS IS NOT A ONE-LINE CONFIG ADDITION: an untranslated path is not
# rejected as a bad path, it is opened as a RELATIVE file of that name, missed,
# and the run reads as a broken binary rather than a harness defect.
#
# ★ NEITHER FUNCTION KNOWS WHAT `wslpath` IS, AND THAT IS DELIBERATE. The verb
# comes off the resolved leg (`run.pathTranslation`, declared per LAUNCHER in
# legs.json) and harness_legs.py performs it. Two drivers each hand-rolling a
# `C:\ -> /mnt/c/` rewrite is the capability-pair defect this project keeps
# paying for; and drive letters are remappable (/etc/wsl.conf `root=`), UNC is a
# different mapping again, and case is not ours to guess.
#
# ★ `--assert-translated=` USES THE `=` FORM ON PURPOSE: a real fixture argv
# carries `--start=full:`, and the space-separated spelling would have the
# resolver's own argument parser read that as an option.
#
# ★ TWO POWERSHELL NATIVE-COMMAND TRAPS, both already paid for elsewhere in this
# file and both handled here rather than rediscovered:
#   · with `2>&1`, stderr arrives as ErrorRecord objects INTERLEAVED with stdout,
#     so the translated path is not reliably `$out[0]`. The stdout lines are
#     separated by TYPE, which is exact — an ErrorRecord is stderr, by
#     construction — instead of by position;
#   · PowerShell 7.3+ can make a nonzero-exiting native command THROW while
#     $ErrorActionPreference is 'Stop', which would abort with a stack trace
#     instead of the diagnostic below. Same try/catch shape as the leg plan's.
function Convert-LaunchPath($verb, $p) {
  if (-not $verb -or $verb -eq 'none') { return "$p" }
  try {
    $out = @(& $python3.Source $LegsPy '--path-translation' "$verb" '--translate-path' "$p" 2>&1)
    $rc  = $LASTEXITCODE
  } catch {
    $rc = if ($LASTEXITCODE) { $LASTEXITCODE } else { 1 }
    $out = @("$($_.Exception.Message)")
  }
  $stdout = @($out | Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] } |
                     ForEach-Object { "$_".Trim() } | Where-Object { $_ })
  if ($rc -ne 0 -or $stdout.Count -lt 1) {
    Die "could not translate '$p' into the launcher's path namespace (pathTranslation '$verb', rc=$rc):`n$(($out | ForEach-Object { "      $_" }) -join "`n")`n      The leg's DECLARED launcher cannot be handed a path at all, so this run stops here rather than spawn it with one its callee would silently read as a relative filename."
  }
  return $stdout[0]
}
# The net under "translate at construction": every argument, at the ONE choke
# point where the child is spawned. A future segment kind that adds a
# path-valued argument and forgets to translate it is refused BY NAME here,
# instead of failing three hours in looking like a fixture bug.
#
# ★ THE INVARIANT THE SEGMENT QUEUE RESTS ON, spelled in exactly one place: a
# segment's FIRST argument is always the .test SCRIPT the fixture sources (its
# Tcl `$argv0`) and is therefore always a PATH; every later argument is a bare
# Tcl word (a permutation name) or a tester.tcl flag (`--start=<perm>:`) and is
# never one. Assert-LaunchArgsTranslated is what catches a future segment kind
# that breaks it, rather than that segment silently reading a relative filename.
function Get-SegmentArgs($verb, $scriptPath, $rest) {
  return @((Convert-LaunchPath $verb $scriptPath)) + @($rest | Where-Object { $null -ne $_ })
}
#
# ── THE LAUNCHER'S FILESYSTEM ────────────────────────────────────────────────
# D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS.
#
# THE THIRD NAMESPACE, AND THE ONE THIS DRIVER GOT WRONG FOR LONGEST. The two
# above make a launched leg's argv and environment correct; neither says a word
# about the FILESYSTEM the launched fixture writes its databases onto, and this
# corpus is a database engine's.
#
# ✔MEASURED 2026-08-06 on this host, ONE process, TWO directories:
#     /mnt/c/…   fs=v9fs        chmod 644 -> 777    chmod 400 -> 555
#     /tmp       fs=ext2/ext3   chmod 644 -> 644    chmod 400 -> 400
# `/mnt/c` is mounted `9p … aname=drvfs;…` with NO `metadata` option, so DrvFs
# synthesises the whole POSIX mode from the single Windows read-only ATTRIBUTE.
# Every sqlite unit that asserts anything about file permissions therefore fails
# — and ✔MEASURED BY A 2x2 MATCHED CONTROL ({DSS fixture, gcc reference} x
# {DrvFs, ext4}) all 60 of them fail IDENTICALLY under GCC on DrvFs and VANISH on
# ext4. Zero were DSS's; all 60 were reported as if they might be.
#
# ⛔ AND THEY ARE NOT CONFOUNDS. The mechanism is ours and it is fixable; a
# confound row would launder a harness misconfiguration into "expected".
#
# ★ THIS FUNCTION KNOWS NOTHING ABOUT WSL, `--cd`, `/tmp` OR `cp`, exactly as
# Convert-LaunchPath knows nothing about `wslpath`. The verb comes off the leg's
# LAUNCHER declaration and harness_legs.py answers with the directory, the
# launcher argv (its working-directory option already spliced into the right
# position) and the argv PREFIXES that create, clear and populate it. A driver
# that spelled any of those would be the second copy of a mechanism, which is
# this project's canonical silent harness bug.
# tester.tcl's cmdlinearg(testdir) default: the fixture `file mkdir`s this subdir
# of its CWD and cd's into it before any .test body runs, so a test's relative
# `./libtestloadext.so` (`./testloadext.dll` on a Windows Tcl) resolves THERE.
# This driver passes no --testdir override. Named once here rather than spelled
# at each of its three sites — the .sh twin's SQLITE_TESTDIR_SUBDIR, same value,
# same reason.
$SqliteTestdirSubdir = 'testdir'
function Get-LegRunDirPlan($label, $driverRunDir) {
  try {
    $out = @(& $python3.Source $LegsPy '--catalogue' $LegsJson '--run-dir-plan' "$label" `
                '--host-os' $HostOs '--host-arch' $HostArch `
                '--driver-run-dir' "$driverRunDir" '--format' 'json' 2>&1)
    $rc  = $LASTEXITCODE
  } catch {
    $rc = if ($LASTEXITCODE) { $LASTEXITCODE } else { 1 }
    $out = @("$($_.Exception.Message)")
  }
  $stdout = @($out | Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] })
  if ($rc -ne 0) {
    Die "[$label] could not resolve this leg's RUN DIRECTORY (harness_legs.py --run-dir-plan, rc=$rc):`n$(($out | ForEach-Object { "      $_" }) -join "`n")`n      Which filesystem a launched leg runs on is DECLARED (legs.json ``launchers[].runFilesystem``), never assumed — and the assumption is what put a Linux corpus onto DrvFs."
  }
  try { return ($stdout -join "`n") | ConvertFrom-Json } catch {
    Die "[$label] harness_legs.py --run-dir-plan exited 0 but did not print the JSON this driver reads ($($_.Exception.Message)). Output was:`n$(($stdout | Select-Object -First 20 | ForEach-Object { "      $_" }) -join "`n")"
  }
}
# Run one of the resolver's argv PREFIXES. An EMPTY prefix means the launcher
# shares this driver's filesystem and the caller does the operation natively —
# that is what `runFilesystem: driver` MEANS, so empty is a real answer and not a
# missing one, and `Ok` is $true for it.
#
# ★ IT RETURNS A VERDICT, IT DOES NOT `Die` — and that is deliberate, not
# defensive. This driver attempts five legs; a run directory that could not be
# created costs THAT leg its corpus and must not delete four other legs' worth of
# evidence. It is the same rule the loadext staging block above learnt the
# expensive way on 2026-08-05, and test-confound-scope.ps1 pins "the staging block
# contains NO Die" precisely so it cannot be un-learnt.
# ⛔ AND THERE IS NO FALLBACK TO THIS DRIVER'S OWN DIRECTORY. Falling back would
# put the corpus straight back onto the filesystem the declaration exists to keep
# it off, and it would do so silently, which is worse than not running the leg.
function Invoke-RunDirArgv($label, $what, $prefix, $rest) {
  $argv = @($prefix | Where-Object { $_ }) + @($rest | Where-Object { $_ })
  if ($argv.Count -lt 2) { return @{ Ok = $true; Detail = '' } }
  $out = & $argv[0] @($argv | Select-Object -Skip 1) 2>&1
  if ($LASTEXITCODE -ne 0) {
    return @{ Ok = $false; Detail = "could not $what in the launcher's own filesystem — ``$($argv -join ' ')`` exited ${LASTEXITCODE}: $(($out | ForEach-Object { "$_" }) -join ' | ')" }
  }
  return @{ Ok = $true; Detail = '' }
}
#
# ── THE LAUNCHER'S ENVIRONMENT NAMESPACE ─────────────────────────────────────
# The second half of the same fact, and it was found by MEASURING the first: a
# launcher in another OS namespace does not inherit this driver's environment
# any more than it understands its paths. ✔MEASURED 2026-08-04: a wsl.exe-
# launched fixture saw SQLITE_TEST_PATTERN_LIST as EMPTY, so the corpus RESUME
# ENGINE — which selects its files through exactly that variable — silently
# re-ran the corpus FROM THE BEGINNING instead of from the abort point.
#
# ★★ ONLY A VARIABLE THAT IS ACTUALLY SET MAY BE CARRIED, AND THAT IS NOT
# TIDINESS — IT IS THE DIFFERENCE BETWEEN A RUN AND A FALSE GREEN. MEASURED
# 2026-08-04: naming an UNSET Windows variable in `WSLENV` materialises it INSIDE
# WSL as EMPTY-BUT-EXISTING. sqlite's permutations.test asks
# `info exists ::env(SQLITE_TEST_PATTERN_LIST)`, so an empty-but-existing value
# is an EMPTY FILE LIST, not "no filter": the tier selected ZERO files, tester.tcl
# still finalised and printed `0 errors out of 1 tests`, and the run was reported
# GREEN. The forward list is therefore recomputed PER SEGMENT, from the values
# actually in place at that moment.
#
# `Get-LaunchEnvCarrierName` is the per-leg half (the carrier's NAME does not
# change); `Resolve-LaunchEnvCarrier` is the per-segment half. Both the name and
# the list separator belong to the VERB, so both come from the resolver.
function Get-LaunchEnvCarrierName($verb) {
  if (-not $verb -or $verb -eq 'inherit') { return '' }
  try { $vocab = @(& $python3.Source $LegsPy '--env-transfers' 2>&1); $rc = $LASTEXITCODE }
  catch { $rc = 1; $vocab = @("$($_.Exception.Message)") }
  if ($rc -ne 0) { Die "could not read the environment-transfer vocabulary (rc=$rc):`n$(($vocab | ForEach-Object { "      $_" }) -join "`n")" }
  $carrier = ''
  foreach ($line in $vocab) {
    $parts = "$line".Split("`t")
    if ($parts.Count -ge 2 -and $parts[0].Trim() -eq $verb) { $carrier = $parts[1].Trim() }
  }
  if (-not $carrier) { Die "envTransfer '$verb' declares no carrier variable, yet it is not 'inherit'. The resolver and this driver disagree about the vocabulary." }
  return $carrier
}
#
# ★★ AND THE THIRD QUESTION, WHICH THE FIRST TWO DO NOT ASK: does the VALUE
# still mean the same thing once it has crossed? For a variable holding a path
# it does not, and TCL_LIBRARY is exactly that variable
# [D-HARNESS-PS1-TCL-LIBRARY-NOT-FORWARDED-ACROSS-THE-WSL-BOUNDARY]. It is
# therefore NOT forwarded by name: it goes through `--forward-path`, whose value
# the resolver puts through this launcher's DECLARED `pathTranslation` — the
# same door the argv already uses — and the resolver REFUSES any forwarded name
# it has no declared kind for, so the next path-valued variable someone adds
# here cannot be forwarded raw by accident.
function Resolve-LaunchEnvCarrier($verb, $pathVerb, $plain, $pathVars, $declared, $current) {
  if (-not $verb -or $verb -eq 'inherit') { return @() }
  $call = @('--env-transfer', "$verb", '--path-translation', "$pathVerb",
            '--carrier-current', "$current")
  # THE FILTER, and it is the load-bearing line of this function.
  $wanted = @()
  foreach ($n in @($plain | Where-Object { $_ })) {
    if ([Environment]::GetEnvironmentVariable($n)) { $call += @('--forward', "$n"); $wanted += $n }
  }
  foreach ($n in @($pathVars | Where-Object { $_ })) {
    # `--forward-path=NAME=VALUE` in ONE token, deliberately: a Windows path can
    # contain spaces and the resolver splits on the FIRST `=` only.
    $v = [Environment]::GetEnvironmentVariable($n)
    if ($v) { $call += "--forward-path=$n=$v"; $wanted += $n }
  }
  foreach ($n in @($declared | Where-Object { $_ })) {
    if ([Environment]::GetEnvironmentVariable($n)) { $call += @('--forward-declared', "$n"); $wanted += $n }
  }
  if (-not $wanted.Count) { return @() }
  try { $out = @(& $python3.Source $LegsPy @call 2>&1); $rc = $LASTEXITCODE }
  catch { $rc = if ($LASTEXITCODE) { $LASTEXITCODE } else { 1 }; $out = @("$($_.Exception.Message)") }
  if ($rc -ne 0) { Die "could not resolve the launcher's environment transfer (envTransfer '$verb', rc=$rc):`n$(($out | ForEach-Object { "      $_" }) -join "`n")`n      Without it the launched fixture runs with an EMPTY run environment, which does not fail — it silently changes what the corpus does." }
  return @($out | Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] } |
                  ForEach-Object { "$_".Trim() } | Where-Object { $_ })
}
#
# ── THE LOADER'S SEARCH PATH, BUILT IN THE LAUNCHER'S NAMESPACE ──────────────
# THE FOURTH QUESTION, AND IT IS THE ONE THE FIRST THREE MADE POSSIBLE TO MISS.
# `pathTranslation` spells an ARGUMENT the way the launcher reads it, `envTransfer`
# gets a NAME across the boundary, `runFilesystem` says where the child writes.
# None of them says a word about a value that is BOTH a list AND a set of paths,
# which is exactly what a runtime loader's search variable is — so this driver
# built one out of HOST spellings joined by a HOST separator and then handed it to
# a foreign loader.
#
# ✔MEASURED 2026-08-07, elf64-arm64 on a Windows host: `libtcl8.6.so: cannot open
# shared object file` with libtcl8.6.so (1,844,864 B) and libz.so.1 (133,520 B)
# staged in the very directory the variable named — because the variable named
# `C:\…\lib` and ld.so has never heard of a drive letter.
#
# ★ EVERY ELEMENT GOES THROUGH THE SAME DOOR THE ARGV ALREADY USES
# (Convert-LaunchPath -> the leg's DECLARED pathTranslation -> harness_legs.py), so
# this function knows nothing about `wslpath`, `/mnt/c`, or which host it is on.
# The separator comes off the TARGET (Get-LegLoaderPathSpec), because the process
# that splits the list is the target's loader.
#
# ★ AND THE VALUE ALREADY IN THE VARIABLE IS A NAMESPACE QUESTION TOO. It belongs
# to THIS driver's environment, so it may be merged only when the launcher shares
# this driver's namespace — which is precisely what `pathTranslation: none`
# declares, and it is why a native leg's PATH still arrives intact. When the
# launcher spells paths differently, a host value would cross as a list of
# directories the target's loader cannot open, so it is DROPPED and said out loud
# rather than passed along to fail quietly.
function Get-LegLoaderSearchPath($leg, $dirs) {
  $spec = Get-LegLoaderPathSpec $leg
  $verb = "$($leg.run.pathTranslation)"
  $parts = @()
  foreach ($d in @($dirs | Where-Object { $_ })) {
    $t = Convert-LaunchPath $verb "$d"
    if ($parts -notcontains $t) { $parts += $t }
  }
  $cur = [Environment]::GetEnvironmentVariable($spec.Name)
  if ($cur) {
    if (-not $verb -or $verb -eq 'none') { $parts += $cur }
    else { Info "[$($leg.label)] $($spec.Name) is SET in this driver's own environment and is NOT carried into the launcher's: its value is spelled in THIS namespace and the launcher declares pathTranslation '$verb', so it would reach the target's loader as directories it cannot open. Only this leg's staged library directories cross." }
  }
  return ($parts -join $spec.Separator)
}
#
# The names the CATALOGUE declared for this leg's launcher (legs.json
# `launchers[].env`, e.g. QEMU_LD_PREFIX), read in ONE place because the read has a
# trap in it.
# ★ `Where-Object { $_ }` IS LOAD-BEARING, MEASURED TF-C115 — NOT defensive noise.
# Every leg the resolver plans carries `run.env`, and for a NATIVE run (or a
# launcher declaring `"env": {}`) it is an EMPTY PSCustomObject. An empty object is
# TRUTHY in PowerShell, so the `if` fires; `.PSObject.Properties.Name` over zero
# properties yields a single $null, and `@($null)` is an array of ONE null. A
# caller then indexes a hashtable with $null and PowerShell throws "Index operation
# failed; the array index evaluated to null" — which killed Step 8 outright for the
# ONE leg a Windows host can execute, with two good testfixtures already on disk.
function Get-LegDeclaredEnvNames($leg) {
  if (-not $leg.run.env) { return @() }
  return @($leg.run.env.PSObject.Properties.Name | Where-Object { $_ })
}
#
# ── APPLYING A LEG'S RUN ENVIRONMENT TO A CHILD, IN ONE PLACE ────────────────
# D-HARNESS-PS1-CLI-SMOKE-IGNORES-THE-LEGS-DECLARED-LAUNCH-ENVIRONMENT.
#
# ✔MEASURED 2026-08-07: the elf64-arm64 CLI smoke gate failed EVERY assertion with
# `rc=255  qemu-aarch64: Could not open '/lib/ld-linux-aarch64.so.1'` — and
# legs.json had declared `QEMU_LD_PREFIX` for that launcher all along. The corpus
# step applied it; the smoke step, twenty lines of its own inline environment
# handling away, did not. TWO COPIES OF ONE DECISION, and only one of them was
# ever fixed — this project's canonical silent harness bug, which is why the
# decision now exists ONCE and both steps CALL it.
#
# There is no per-invocation environment parameter that also preserves the
# redirections these call sites need, so the child's environment is this process's:
# set here, restored by Pop-LegLaunchEnv in the caller's `finally`. EVERY name this
# function assigns is snapshotted before it is written — including the ones the
# resolver's own assignments name — so "was unset" is restored as unset rather than
# as empty, and nothing leaks into the next leg.
function Push-LegLaunchEnv($leg, $loaderPath, $plainNames, $pathNames) {
  $verb  = "$($leg.run.envTransfer)"
  $xlate = "$($leg.run.pathTranslation)"
  $var   = Get-LegLoaderPathVar $leg
  $declaredNames = @(Get-LegDeclaredEnvNames $leg)
  $carrier = Get-LaunchEnvCarrierName $verb
  $snap = @{ Names = @(); Old = @{} }
  foreach ($n in @(@($var) + $declaredNames + @($carrier) | Where-Object { $_ })) {
    if ($snap.Names -notcontains $n) { $snap.Names += $n; $snap.Old[$n] = [Environment]::GetEnvironmentVariable($n) }
  }
  [Environment]::SetEnvironmentVariable($var, $loaderPath)
  foreach ($n in $declaredNames) { [Environment]::SetEnvironmentVariable($n, "$($leg.run.env.$n)") }
  # LAST, because it reads the variables set above AND the ones the caller set:
  # the launcher's declared environment TRANSFER, resolved from what is actually
  # SET right now. For `inherit` this is empty and the spawn is byte-for-byte the
  # one it always was.
  # ★ THE LOADER VARIABLE CROSSES AS `declared`, AND THAT IS THE PRECISE CLAIM.
  # `--forward-declared` is the resolver's door for a value that is ALREADY IN THE
  # LAUNCHER'S NAMESPACE BY CONSTRUCTION and therefore crosses verbatim — which is
  # exactly what Get-LegLoaderSearchPath just made it, element by element, through
  # that launcher's own declared translator. It is emphatically NOT `--forward-path`
  # (that door translates the value, and translating an already-translated LIST
  # would hand ld.so a mangled string) and not `--forward` (that door is for a value
  # with no namespace at all). Without naming it here the variable is set on this
  # process and simply never crosses: WSLENV is what makes a Windows-side variable
  # visible inside WSL, and a value nobody carried is indistinguishable from a leg
  # whose libraries were never staged.
  $carrierOld = if ($carrier) { $snap.Old[$carrier] } else { '' }
  foreach ($a in (Resolve-LaunchEnvCarrier $verb $xlate $plainNames $pathNames `
                                           (@($declaredNames) + @($var)) $carrierOld)) {
    $kv = $a.Split('=', 2)
    if ($snap.Names -notcontains $kv[0]) { $snap.Names += $kv[0]; $snap.Old[$kv[0]] = [Environment]::GetEnvironmentVariable($kv[0]) }
    [Environment]::SetEnvironmentVariable($kv[0], $kv[1])
  }
  return $snap
}
# The other half, and it restores by SNAPSHOT rather than by recomputation: a
# variable that was unset before the spawn is set back to unset, because an
# empty-but-existing variable is a real setting to everything downstream — the same
# distinction that made the carrier's own filter load-bearing.
function Pop-LegLaunchEnv($snap) {
  if (-not $snap) { return }
  foreach ($n in @($snap.Names)) { [Environment]::SetEnvironmentVariable($n, $snap.Old[$n]) }
}
#
# ── THE REGISTRY, AT THE POINT OF FAILURE ────────────────────────────────────
# D-PROCESS-CHECK-THE-REGISTRY-FOR-A-MATCHED-CONTROL-BEFORE-COMMISSIONING-ONE.
#
# ✔MEASURED (TF-C123): a 2x2 attribution was commissioned from scratch for 57
# unit failures whose identical experiment and identical verdict were already in
# the registry from seven cycles earlier, and the un-cited row let three false
# statements reach a commit. The row was findable; looking is the part you have
# to remember. So the harness looks, HERE, and prints what it found beside the
# failure it just reported.
#
# ⚠ FAIL-SOFT BY CONSTRUCTION. This runs on a failure path and must never become
# one: every error is one line and a return. It is a POINTER, never a verdict —
# a matched row means someone has looked at something with this name before, not
# that this failure is explained, and the banner says so.
function Show-RegistryControls($legLabel, $failingTests) {
  try {
    $call = @('--registry-controls', $AnchorRegistry)
    if ($legLabel) { $call += @('--for-leg', "$legLabel") }
    # Bounded on purpose: a leg can fail with hundreds of names and this is a
    # pointer, not a search engine. The resolver reports how many rows it held
    # back, so a truncated lookup never reads as an exhaustive one.
    foreach ($t in @($failingTests | Where-Object { $_ } | Select-Object -First 12)) {
      $call += @('--for-test', "$t")
    }
    $out = @(& $python3.Source $LegsPy @call 2>&1)
  } catch {
    Info "      (registry lookup unavailable: $($_.Exception.Message))"
    return
  }
  $lines = @($out | Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] } |
                    ForEach-Object { "$_" } | Where-Object { $_ })
  if (-not $lines.Count) { return }
  Info "      ── registry rows naming this leg / these tests (a POINTER, not a verdict — read before commissioning an experiment):"
  foreach ($l in $lines) { Info "      $l" }
}
function Assert-LaunchArgsTranslated($verb, $argv) {
  if (-not $verb -or $verb -eq 'none') { return }
  $call = @('--path-translation', "$verb")
  foreach ($a in @($argv)) { $call += "--assert-translated=$a" }
  try {
    $out = @(& $python3.Source $LegsPy @call 2>&1)
    $rc  = $LASTEXITCODE
  } catch {
    $rc = if ($LASTEXITCODE) { $LASTEXITCODE } else { 1 }
    $out = @("$($_.Exception.Message)")
  }
  if ($rc -ne 0) {
    Die "REFUSING to spawn the leg's launcher — an argument is still in THIS driver's path namespace, not the launcher's:`n$(($out | ForEach-Object { "      $_" }) -join "`n")"
  }
}

# ── THE STAGED-SOURCE COHERENCE GATE ─────────────────────────────────────────
# D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE. MEASURED 2026-08-04.
#
# The harness reuses ONE sqlite build dir and `git pull --rebase`es the checkout
# under it every run. `make` refreshes only prerequisites of the requested target,
# and `testfixture USE_AMALGAMATION=0` makes sqlite3.c / shell.c / tclsqlite3.c /
# tsrc/ prerequisites of NOTHING — orphans that never refresh while every file
# around them marches forward. The staged tree accumulated FIVE upstream vintages
# (2026-06-25 → 2026-08-04); a cross-built sqlite3 out of it compiled and linked
# cleanly and then exited 1 at startup on shell.c's sqlite3_sourceid() guard.
# check-source-coherence.sh compares SQLITE_SOURCE_ID across every staged artifact
# that carries one and fails loud, naming the divergent files and both ids.
#
# ★ IT IS A PRECONDITION ON A SHARED INPUT, NOT A PER-LEG VERDICT — do not
# "improve" it into a `skipped-*` on one leg. ONE stage feeds EVERY leg, so an
# incoherent stage poisons the whole run: it is a run-wide `Die` BEFORE any leg is
# built. A per-leg skip would report four other legs as meaningful results
# obtained from an input that is known to be wrong.
#
# ★ IT IS UNSKIPPABLE, AND THERE IS NO OPT-OUT VARIABLE. It is deliberately NOT
# behind DSS_SKIP_SELFTEST: that flag governs the driver's self-tests, and this is
# a gate on the RUN's input. Its only prerequisite is a POSIX shell, which is a
# STRICT SUBSET of what this driver already requires to derive the recipe at all
# (make -n + tclsh + gcc, Step 3+4) — so a host that can reach this line can
# always run the checker, and there is no honest skip path to invent for it.
function Assert-StagedSourceCoherence($label) {
  $chk = Join-Path $PSScriptRoot 'check-source-coherence.sh'
  if (-not (Test-Path $chk)) {
    Die "staged-source coherence gate missing: $chk`n      It is what stops the harness compiling a tree that silently accumulated several upstream vintages (D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE). Running without it is how a binary links clean and then exits 1 on sqlite's own sourceid guard."
  }
  $chkP   = ToShellPath $chk
  $stageP = ToShellPath $Stage
  # The STAGED tree is what DSS compiles, so it is what is asserted — the sqlite
  # build dir is only its source. `--checkout` additionally pins the stage to the
  # checkout's own manifest.uuid: ONE coherent state is necessary but not
  # sufficient, and this says WHICH state it must be.
  #
  # ★ `--require-cli` IS NOW PASSED, and it was not before. The gate has always
  # had the flag (check-source-coherence.sh:78, :192-208) and NOTHING called it
  # with one, so the assertion it implements — that sqlite3.c, shell.c and
  # sqlite3.h are all present, and that shell.c's QUOTED `#include "sqlite3.h"`
  # can only resolve beside it — shipped inert. It matters now because this
  # harness BUILDS the CLI: shell.c's startup guard compares sqlite3_sourceid()
  # against the SQLITE_SOURCE_ID it was compiled with, and a mismatched pair
  # yields a binary that COMPILES CLEAN, LINKS CLEAN and then prints "SQLite
  # header and source version mismatch" and exit(1) — indistinguishable from a
  # miscompile from the outside.
  $dirs = @("$stageP/sqlite/bld", "$stageP/sqlite/src")
  $line = "bash '$chkP' --checkout `"$SqliteWslDir`" --require-cli --label '$label' " + (($dirs | ForEach-Object { "'$_'" }) -join ' ')
  $r = Invoke-PosixCommand $line
  foreach ($ln in $r.Out) { Info "      $ln" }
  if ($r.Rc -ne 0) {
    Die @"
STAGED SQLITE SOURCES ARE INCOHERENT (check-source-coherence.sh rc=$($r.Rc)) — refusing to compile.
      label : $label
      dirs  : $($dirs -join '  ')
      Every artifact carrying a SQLITE_SOURCE_ID must agree, and agree with the checkout.
      They do not, so this tree is several upstream vintages at once and anything built from
      it is unattributable. D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE — the report above
      names the divergent files and both ids.
      ★ RE-STAGING WILL NOT FIX IT. Step 3+4 wipes and re-stages on every run, but it copies
      from $SqliteWslDir/bld-dss, and THAT is where the orphans live: sqlite3.c / shell.c /
      tclsqlite3.c / tsrc/ are prerequisites of no target this harness ever asks make for, so
      they never refresh. Regenerate them from the current source state, in the build dir:
          make sqlite3.c shell.c tclsqlite3.c
      (hand-copying one file fixes the symptom you noticed and leaves the ones you did not.)
"@
  }
  Pass "staged sqlite sources coherent — $label"
}

# ── Step 0 — SELF-TEST the driver's own late-stage logic ─────────────────────
# python3 is discovered HERE rather than in Step 1 because Step 0's second
# self-test (the leg resolver) needs it. Same two-name ladder as before —
# `python3`, then `python` — and Step 1 still reports which one was found, so
# nothing about the discovery itself changed.
$python3 = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $python3) { $python3 = Get-Command python -ErrorAction SilentlyContinue }
if (-not $python3) { Die "python3 not found on PATH — needed to resolve the leg plan (harness_legs.py) and to generate each leg's .dss-project.json manifest." }
# >>> dss:selftest >>>
# Mirrors build-and-test.sh. The classifier runs at the very END of a run, so a
# defect there is invisible until the build and the whole corpus have already been
# paid for — a top-level `local` in the .sh classifier aborted a COMPLETED 13-hour
# arm64 run at exactly that point (D-HARNESS-TEST-SCOPE-FIDELITY). Syntax checks
# cannot catch that class; only executing the code can.
# So the driver REFUSES TO START if its own end-of-run logic is broken. This reuses
# test-confound-scope.ps1, which EXTRACTS the shipped classifier and runs it — no
# duplicated logic to drift. Set DSS_SKIP_SELFTEST=1 to bypass (not recommended).
# ★ A LIST, NOT A SINGLE FILE. Each entry EXTRACTS shipped logic and executes it,
# and each emits the same `passed=N failed=N skipped=N` summary parsed below — so
# adding a fourth costs one array entry instead of a second copy of this careful
# rc/skip/refuse block. A self-test that exists but is never RUN is documentation,
# which is how a guarded behaviour comes to look guarded without being tested.
#   test-confound-scope.ps1    the end-of-run confound classifier
#   test-driver-contracts.ps1  the LEG CONTRACTS — the verdict recorders, the
#                              shared run decision, Read-CorpusSegment's first
#                              diagnostic, the target-keyed loader variable and the
#                              acquisition contract field, and the LAUNCHER RUN
#                              ENVIRONMENT (the leg's declared variables, the loader
#                              search path in the launcher's namespace with the
#                              target's separator, the carrier that carries both,
#                              and the CLI smoke gate applying them at all), each
#                              with its red-on-disable mutation asserted to have
#                              LANDED.
#   test-mirror-regions.ps1    the `dss:` REGIONS — every region declared with who
#                              verifies it (a claimed verifier that does not read
#                              the region is a LOUD failure), and for a region
#                              declared MIRRORED the symbol pairing plus
#                              DIFFERENTIAL EXECUTION of both drivers' copies on
#                              byte-identical input. It found a live divergence on
#                              its first complete run: THIS driver recorded only
#                              the MATCHED SUBSTRING of sqlite's summary line
#                              where the .sh records the whole line.
#                              D-HARNESS-CORPUS-ENGINE-MIRROR-CLAIMS-A-VERIFIER-THAT-DOES-NOT-EXIST
foreach ($selfTest in @((Join-Path $PSScriptRoot 'test-confound-scope.ps1'),
                        (Join-Path $PSScriptRoot 'test-driver-contracts.ps1'),
                        (Join-Path $PSScriptRoot 'test-mirror-regions.ps1'))) {
$stName = Split-Path -Leaf $selfTest
if ($env:DSS_SKIP_SELFTEST -eq '1') {
  Warn "driver self-test $stName SKIPPED (DSS_SKIP_SELFTEST=1) — a late-stage defect will not surface until the end of the run."
} elseif (-not (Test-Path $selfTest)) {
  Die "driver self-test missing: $selfTest`n      This guard is what stops a defect in the END-OF-RUN classifier from costing you the entire run."
} else {
  $stOut = & pwsh -NoProfile -File $selfTest 2>&1
  if ($LASTEXITCODE -ne 0) {
    $stOut | ForEach-Object { "      $_" } | Write-Host
    Die "DRIVER SELF-TEST FAILED ($stName) — refusing to start.`n      Late-stage driver logic is broken, so this run would execute the whole corpus (hours) and then abort while classifying — or would classify a leg's outcome wrongly and report it."
  }
  # ★ PARITY with build-and-test.sh: the SKIP COUNT is part of the result, not a
  # footnote, and an UNPARSEABLE summary is a failure rather than a blank. The old
  # form indexed .Matches.Groups[1] unconditionally, so a self-test that stopped
  # printing `passed=` rendered "OK ( assertions)" -- a pass reported over a result
  # nobody could read, which is the very defect class this guard exists to catch.
  $sm = ($stOut | Select-String -Pattern '^passed=(\d+) failed=(\d+) skipped=(\d+)$' | Select-Object -Last 1)
  if (-not $sm) {
    $stOut | ForEach-Object { "      $_" } | Write-Host
    Die @"
driver self-test exited 0 but printed no readable summary line.
      Expected a final 'passed=N failed=N skipped=N'. Its assertions may well have passed,
      but a self-test whose RESULT cannot be read proves nothing and this guard will not
      report OK over it. Either test-confound-scope.ps1's summary format changed (update
      this parse with it) or it died before printing the line.
"@
  }
  $n = $sm.Matches[0].Groups[1].Value
  $nSkip = $sm.Matches[0].Groups[3].Value
  if ($nSkip -eq '0') {
    Info "driver self-test ${stName}: OK ($n assertions, 0 skipped)"
  } else {
    Warn "driver self-test ${stName}: OK ($n assertions) — but $nSkip assertion(s) SKIPPED on this host (unmet prerequisite, normally 'no git on PATH'). That part of the late-stage logic is UNPROVEN for this run: $selfTest"
  }
}
}
# ── the LEG RESOLVER's own self-test, same refuse-to-start discipline ────────
# The resolver decides WHICH TARGETS THIS RUN BUILDS. A defect there does not
# announce itself: a leg simply fails to appear, the run looks orderly, and the
# missing coverage is discovered — if ever — long after the hours are spent. That
# is the identical failure shape the classifier guard above exists for, so it gets
# the identical treatment, including the DSS_SKIP_SELFTEST=1 escape hatch.
# Its load-bearing assertion is HOST-INVARIANCE OF THE BUILD SET: nine synthetic
# hosts × three launcher-availability worlds must all yield the same legs in the
# same order. That property IS D-HARNESS-CROSS-HOST-ANY-TARGET item (2), executable.
if ($env:DSS_SKIP_SELFTEST -eq '1') {
  Warn 'leg-resolver self-test SKIPPED (DSS_SKIP_SELFTEST=1) — a defect in the leg plan will not surface until a leg is silently missing from the results.'
} elseif (-not (Test-Path $LegsPy)) {
  Die "leg resolver missing: $LegsPy`n      It is the ONE host-independent answer to 'which targets does this harness build?', shared with build-and-test.sh. Without it this driver has no leg set at all."
} else {
  $ltOut = & $python3.Source $LegsPy '--self-test' 2>&1
  if ($LASTEXITCODE -ne 0) {
    $ltOut | ForEach-Object { "      $_" } | Write-Host
    Die "LEG-RESOLVER SELF-TEST FAILED — refusing to start.`n      The leg plan is what decides which targets this run builds; a broken resolver silently deletes coverage."
  }
  # Same discipline as the classifier parse above: an UNREADABLE result is a
  # failure, never an "OK" printed over numbers nobody can see.
  $lm = ($ltOut | Select-String -Pattern '^passed=(\d+) failed=(\d+)$' | Select-Object -Last 1)
  if (-not $lm) {
    $ltOut | ForEach-Object { "      $_" } | Write-Host
    Die @"
leg-resolver self-test exited 0 but printed no readable summary line.
      Expected a final 'passed=N failed=0' from harness_legs.py --self-test. Either that
      output format changed (update this parse with it) or it died before printing the line.
"@
  }
  Info "leg-resolver self-test: OK ($($lm.Matches[0].Groups[1].Value) assertions)"
}

# ── WHICH ARM BUILDS THE loadext HELPER — resolved HERE, as a refuse-to-start ──
# The helper is the shared object sqlite's test/loadext.test dlopen()s. Since
# 2026-08-05 DSS emits it for the leg's declared sharedLibFormat, so this driver
# no longer needs a compiler for any leg's target; the leg's VERIFIED target
# compiler is an optional CONTROL, and DSS_LOADEXT_HELPER=reference stages that
# one instead so the corpus itself becomes the differential.
#
# RESOLVED BEFORE ANYTHING IS BUILT, on purpose: a typo'd DSS_LOADEXT_HELPER must
# stop the run in its first seconds, not after five fixture builds. The
# vocabulary lives in harness_legs.py so both drivers refuse the same values with
# the same words. ASCII ONLY in what is printed here (a non-ASCII character in
# gate output has already killed a run on a cp1252 console).
$LoadextBuilder = (& $python3.Source $LegsPy '--loadext-builder' 2>&1)
if ($LASTEXITCODE -ne 0) {
  Die "DSS_LOADEXT_HELPER='$($env:DSS_LOADEXT_HELPER)' is not a builder this harness implements:`n      $($LoadextBuilder -join "`n      ")"
}
$LoadextBuilder = "$LoadextBuilder".Trim()
if ($LoadextBuilder -eq 'reference') {
  Warn "DSS_LOADEXT_HELPER=reference - the loadext helper will be STAGED from each leg's VERIFIED"
  Info "      target C compiler instead of from DSS. That is the CONTROL arm: it makes the corpus"
  Info "      itself the differential for 'is a loadext-* red the fixture or the helper?', and it"
  Info "      re-introduces a host dependency ON PURPOSE - a leg with no such compiler on this box"
  Info "      records skipped-build-input-missing (environmental; the default would have run it)."
} else {
  Info "loadext helper builder: $LoadextBuilder (DSS emits it per leg; no host cross-compiler needed)"
}
# <<< dss:selftest <<<

# ── Step 1 — host identity, this HOST's POSIX toolchain, online ──────────────
# ★★ WHAT USED TO BE HERE, AND WHY IT IS GONE ★★
#     if (-not $IsWindows -and $PSVersionTable.PSVersion.Major -ge 6) {
#       Die "this harness targets Windows (the pe64 leg); ..."
#     }
# That gate CONFLATED HOST WITH TARGET. pe64 was the only target this driver
# could build, and no other target could be built FROM Windows — the precise
# inversion of "build ANY target inside ANY host". It is deleted, not relaxed.
#
# HOST IDENTITY IS NOW USED FOR EXACTLY TWO THINGS, AND NEVER FOR A THIRD:
#   (1) to answer "can this host EXECUTE this leg's artifact?" — and even that is
#       not answered here: it is answered by the resolver, from the leg's own
#       `runOn` + `launchers`, and arrives as run.mode / run.verdict.
#   (2) to locate this HOST's toolchains (the POSIX shell below, python3 above).
# It is NEVER used to decide which legs exist. If you find yourself adding an
# `if ($HostOs -eq …)` that changes the LEG SET, you have re-locked the harness.
Step '1/9  Host identity + this host''s POSIX toolchain (online)'
function Get-HostOsCanonical {
  # PowerShell 7 exposes $IsWindows/$IsLinux/$IsMacOS. Windows PowerShell 5.1
  # PREDATES them: they are simply UNDEFINED there, so `-not $IsWindows` is $true
  # on the one platform 5.1 can possibly be running on. (That is not a hypothetical
  # — it is why the deleted gate above needed its `-and PSVersion.Major -ge 6`
  # clause to avoid killing every 5.1 run.) 5.1 is Windows-only, so the version IS
  # the answer there and the automatic variables are never consulted.
  if ($PSVersionTable.PSVersion.Major -lt 6) { return 'windows' }
  if ($IsWindows) { return 'windows' }
  if ($IsLinux)   { return 'linux' }
  if ($IsMacOS)   { return 'darwin' }
  return 'unknown'
}
function Get-HostArchCanonical {
  # Spellings match the shipped *.target.json `name`s and currentHostArch() in
  # tests/test_support/arm_verdict_ledger.hpp, so a spec's target prefix compares
  # directly. OSArchitecture (not process architecture): an x86 pwsh on an x64 box
  # can still EXECUTE an x64 artifact, and execution is the question.
  $raw = ''
  try { $raw = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString() } catch { $raw = '' }
  if (-not $raw) { $raw = "$env:PROCESSOR_ARCHITECTURE" }   # 5.1 / older .NET fallback
  $low = "$raw".Trim().ToLowerInvariant()
  if ($low -match '^(x64|amd64|x86_64)$')  { return 'x86_64' }
  if ($low -match '^(arm64|aarch64)$')     { return 'arm64' }
  # UNKNOWN IS REPORTED, NOT GUESSED. The resolver still returns the full leg set
  # for an unrecognised host (its self-test pins that), so an unknown arch costs
  # execution, never coverage.
  return $(if ($low) { $low } else { 'unknown' })
}
$HostOs   = Get-HostOsCanonical
$HostArch = Get-HostArchCanonical
# The ONE host-keyed switch in this driver, and it is a BUILD-HOST TOOLCHAIN fact,
# not a target fact: deriving the sqlite recipe needs a POSIX shell + make + tclsh
# (sqlite's build is autotools + tclsh). On a Windows host that comes from WSL; on
# a Linux/macOS host it is native. Which target gets built is unaffected either way.
$script:HostNeedsWsl = ($HostOs -eq 'windows')
if ($script:HostNeedsWsl) {
  $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
  if (-not $wsl) { Die "no POSIX toolchain for this host: wsl.exe not found.`n      This is WHERE THIS HOST FINDS ITS POSIX TOOLCHAIN, not a statement about any target — deriving the sqlite recipe needs a POSIX shell + make + tclsh (sqlite's build is autotools + tclsh).`n      On a Windows host that toolchain is WSL: install WSL + a Debian/Ubuntu distro." }
  # `-e` for the same reason every other wsl.exe call site carries it (see
  # Invoke-PosixCommand): without it this would be a round-trip through the LOCAL
  # default shell, which is not the thing being probed.
  $probe = & wsl.exe -e bash -c 'echo posix-ok' 2>&1
  if ($LASTEXITCODE -ne 0 -or "$probe".Trim() -ne 'posix-ok') { Die "this host's POSIX toolchain (WSL) is present but a bash round-trip failed (got: '$probe')." }
  $PosixToolchain = "WSL ($($wsl.Source))"
} else {
  $bashCmd = Get-Command bash -ErrorAction SilentlyContinue
  if (-not $bashCmd) { Die "no POSIX toolchain for this host: 'bash' is not on PATH.`n      This is WHERE THIS HOST FINDS ITS POSIX TOOLCHAIN, not a statement about any target — deriving the sqlite recipe needs a POSIX shell + make + tclsh (sqlite's build is autotools + tclsh).`n      On a $HostOs host that toolchain is native: install bash (and git gcc make ar tclsh, which Step 3+4 checks for by name)." }
  $probe = & $bashCmd.Source -c 'echo posix-ok' 2>&1
  if ($LASTEXITCODE -ne 0 -or "$probe".Trim() -ne 'posix-ok') { Die "this host's POSIX toolchain (native bash at $($bashCmd.Source)) is present but a round-trip failed (got: '$probe')." }
  $PosixToolchain = "native bash ($($bashCmd.Source))"
  # ⚠ HONESTY, STATED AT THE TOP OF THE RUN RATHER THAN DISCOVERED AT THE BOTTOM:
  # the native-POSIX branch of Step 3+4 has never been EXECUTED. It is written and
  # it fails loud at every checkpoint (the derive script is `set -Eeuo pipefail`
  # and every stage is verified), but "written and loud" is not "measured", and
  # this driver does not print numbers it has not earned.
  Warn "this driver's native-POSIX recipe derivation is UNPROVEN — it has never been executed on a $HostOs host. It fails loud rather than silently, but treat a first run here as a bring-up, and prefer build-and-test.sh, which is the measured driver on a POSIX host."
}
# DSS_STRICT_ARM_VERDICTS is read HERE, not at Step 9, so a typo costs a second
# instead of a multi-hour corpus run. Mirrors readStrictArmVerdicts() in
# tests/test_support/arm_verdict_ledger.hpp EXACTLY, including the third state:
# an unrecognised value is neither on nor off. Folding it into `off` is how a
# stale `=0` or a typo'd `=ture` quietly disables a gate.
$StrictVerdicts = $false
if ($null -ne $env:DSS_STRICT_ARM_VERDICTS) {
  if     ($env:DSS_STRICT_ARM_VERDICTS -cin @('1','true','TRUE','yes'))       { $StrictVerdicts = $true }
  elseif ($env:DSS_STRICT_ARM_VERDICTS -cin @('','0','false','FALSE','no'))   { $StrictVerdicts = $false }
  else {
    Die @"
DSS_STRICT_ARM_VERDICTS='$($env:DSS_STRICT_ARM_VERDICTS)' is neither on nor off.
      Recognised (case-sensitive, same set as readStrictArmVerdicts() in
      tests/test_support/arm_verdict_ledger.hpp):  ON: 1 true TRUE yes   OFF: (empty) 0 false FALSE no
      An unrecognised value is refused rather than treated as OFF — a typo must never
      silently disable the gate it was typed to enable.
"@
  }
}
try {
  $null = Invoke-WebRequest -Uri 'https://github.com' -Method Head -TimeoutSec 20 -UseBasicParsing
} catch { Die "offline — cannot reach https://github.com ($($_.Exception.Message))." }
Info "host: $HostOs/$HostArch   ($([Environment]::OSVersion.VersionString))"
Info "posix toolchain: $PosixToolchain   python3: $($python3.Source)"
Info "tier: $Tier   config: $Config   strict-verdicts: $(if ($StrictVerdicts) { 'ON (environmental skips are FATAL)' } else { 'off (environmental skips WARN)' })"
Pass "host identified + POSIX toolchain reachable + online"

# ── Step 1b — the LEG PLAN, from the host-free catalogue ─────────────────────
# ★ THE LEG SET COMES FROM legs.json THROUGH harness_legs.py — the SAME resolver
# build-and-test.sh consumes, so the two drivers cannot disagree about which
# targets exist. The host identity is PASSED IN explicitly (rather than letting
# the resolver detect it) for one reason: it puts this driver's own canonical
# answer in the log, where a disagreement between the two detections is visible
# instead of silent.
Step '1b/9  Resolve the leg plan (harness_legs.py --plan)'
$planRc = 0
$planOut = @()
try {
  # rc taken DIRECTLY off the call — $LASTEXITCODE on the very next statement,
  # never after a pipe (a pipe reports the PIPE's status). The try/catch is not
  # decoration: PowerShell 7.3+ can make a nonzero-exiting native command THROW
  # while $ErrorActionPreference is 'Stop', which would abort with a stack trace
  # instead of the diagnostic below.
  $planOut = @(& $python3.Source $LegsPy '--plan' '--host-os' $HostOs '--host-arch' $HostArch '--format' 'json' 2>&1)
  $planRc  = $LASTEXITCODE
} catch {
  $planRc  = if ($LASTEXITCODE) { $LASTEXITCODE } else { 1 }
  $planOut = @("$_")
}
if ($planRc -ne 0) {
  Die "leg resolution FAILED (rc=$planRc) — this driver has no leg set and will not guess one:`n$(($planOut | ForEach-Object { "      $_" }) -join "`n")"
}
$planText = ($planOut | ForEach-Object { "$_" }) -join "`n"
$plan = $null
try { $plan = $planText | ConvertFrom-Json } catch {
  Die "leg resolver returned output that is not JSON:`n      $($_.Exception.Message)`n$(($planOut | Select-Object -First 20 | ForEach-Object { "      $_" }) -join "`n")"
}
$AllLegs = @($plan.legs)
if ($AllLegs.Count -eq 0) { Die "leg resolver returned a plan with NO legs — refusing to run a harness that builds nothing." }
if ($plan.host.os -ne $HostOs -or $plan.host.arch -ne $HostArch) {
  # Not fatal — the resolver canonicalises, and it is the authority on its own
  # spellings — but a silent disagreement between the two host detections is
  # exactly the kind of drift that makes a run's verdict unreadable.
  Warn "the resolver echoed host $($plan.host.os)/$($plan.host.arch) for the $HostOs/$HostArch this driver detected — canonicalisation, or a real disagreement. The resolver's answer is the one used."
}
# DSS_LEGS — a FILTER for fast iteration, never a redefinition of the leg set.
# Parity with build-and-test.sh. An unmatched label DIES (a typo must not quietly
# run a different set), and every leg it removes is reported as the coverage
# reduction it is — here AND in the Step-9 summary.
$LegFilterRaw = if ($env:DSS_LEGS) { @($env:DSS_LEGS -split '[,\s]+' | Where-Object { $_ }) } else { @() }
$FilteredOut = @()
$Legs = $AllLegs
if ($LegFilterRaw.Count) {
  $declared = @($AllLegs | ForEach-Object { $_.label })
  $unknown  = @($LegFilterRaw | Where-Object { $declared -notcontains $_ })
  if ($unknown.Count) {
    Die "DSS_LEGS names $($unknown.Count) leg(s) this harness does not declare: $($unknown -join ' ')`n      Declared legs: $($declared -join ' ')`n      A filter that matches nothing would silently run a different set than you asked for."
  }
  $Legs        = @($AllLegs | Where-Object { $LegFilterRaw -contains $_.label })
  $FilteredOut = @($AllLegs | Where-Object { $LegFilterRaw -notcontains $_.label } | ForEach-Object { $_.label })
}
Info "catalogue declares $($AllLegs.Count) leg(s) — the SAME set on every host:"
$SelectedLabels = @($Legs | ForEach-Object { $_.label })
foreach ($lg in $AllLegs) {
  # by LABEL, not by object identity: a PSCustomObject compares by reference, so
  # `-contains $lg` would quietly answer "no" the day this list is rebuilt.
  $sel = if ($SelectedLabels -contains $lg.label) { ' ' } else { '-' }
  $runTxt = switch ($lg.run.mode) {
    'native'   { 'run: NATIVE' }
    'launched' { "run: LAUNCHED via '$($lg.run.launcher -join ' ')'$(if ("$($lg.run.pathTranslation)" -ne 'none') { " [paths -> $($lg.run.pathTranslation)]" })" }
    default    { "run: skip [$($lg.run.verdict)]" }
  }
  Info "  [$sel] $($lg.label.PadRight(15)) $($lg.spec.PadRight(34)) build: ATTEMPTED   $runTxt"
}
if ($FilteredOut.Count) {
  Warn "DSS_LEGS REMOVED COVERAGE — $($FilteredOut.Count) declared leg(s) will NOT be built or reported on this run: $($FilteredOut -join ' ')"
  Warn "      this run therefore covers $($Legs.Count) of $($AllLegs.Count) declared legs; the Step-9 summary says so too."
}
Pass "leg plan resolved — $($Legs.Count) leg(s) selected of $($AllLegs.Count) declared"

# ── Step 2 — dss-code-prime (current checkout, untouched) ────────────────────
# >>> dss:src-provenance >>>
# ★ THIS REGION IS EXTRACTED AND EXECUTED by test-confound-scope.ps1 (the Step-0
# self-test) against throwaway git repos — the same discipline the .sh's self-test
# applies to its Step-2 gate: run the SHIPPED code, never a re-implementation that
# would stay green while this rotted. The sentinel comments are a CONTRACT with
# that file; rename or move one and the self-test fails with "could not locate",
# which this driver treats as refuse-to-start.
Step "2/9  Use dss-code-prime at $RepoRoot (current checkout, untouched)"
$DssBranchWant = if ($env:DSS_BRANCH) { $env:DSS_BRANCH } else { '' }
$DssCommitWant = if ($env:DSS_COMMIT) { $env:DSS_COMMIT } else { '' }
# One line of git output, or '' — never $null, never an array, never a throw.
# The try/catch is not decoration: under PowerShell 7.4+ the (now stable)
# $PSNativeCommandUseErrorActionPreference makes a nonzero-exiting NATIVE command
# throw while $ErrorActionPreference is 'Stop', so an unresolvable rev-parse would
# abort the driver instead of yielding the UNKNOWN this block exists to print.
# MEASURED here: pwsh 7.5.2 with that preference $false — precisely why it must not
# be relied on. `("$o").Trim()` also collapses git's single output line out of the
# ARRAY PowerShell may hand back, so downstream `-ne` compares strings rather than
# silently doing array filtering.
function Get-GitLine($dir, [string[]]$gitArgs) {
  try { $o = (& git -C $dir @gitArgs 2>$null) } catch { return '' }
  if ($null -eq $o) { return '' }
  return ("$o").Trim()
}
# ★ TEST FOR .git BEFORE CALLING git AT ALL. `git -C <dir>` WALKS UPWARD: aimed at a
# directory that is not a checkout it answers with an ENCLOSING repository's HEAD, so
# the verdict would cite a real but WRONG commit — strictly worse than UNKNOWN,
# because it reads as verified. `Test-Path` with no -PathType on purpose: a
# `git worktree` (and a submodule) keeps .git as a FILE, and a directory-only test
# would call such a checkout "not a repo" — the same choice the .sh makes with `-e`
# (build-and-test.sh:537).
$dssIsRepo = Test-Path -LiteralPath (Join-Path $RepoRoot '.git')
$dssHead = ''; $dssHeadLong = ''; $dssBranch = ''
if ($dssIsRepo) {
  $dssHead     = Get-GitLine $RepoRoot @('rev-parse','--short','HEAD')
  $dssHeadLong = Get-GitLine $RepoRoot @('rev-parse','HEAD')
  $dssBranch   = Get-GitLine $RepoRoot @('rev-parse','--abbrev-ref','HEAD')
}
# PARITY with build-and-test.sh, which grew this in the same cycle
# (D-HARNESS-SH-SRC-DIR-GIT-REQUIRED-VS-RSYNC-GATE item 3, and the preferred fix in
# its measured sibling D-HARNESS-WSL-TREE-PROVENANCE-UNVERIFIABLE). A verdict line
# naming a commit is a CITATION later cycles quote as "the compiler that ran this",
# and it lied in two ways:
#   * an EMPTY field -- `& git ... 2>$null` yields $null when git fails, and
#     "compiler : $DssBin @ " then reads as fine rather than as unknown;
#   * a hash the SOURCES do not match. Mid-cycle this checkout normally carries
#     uncommitted work, so the built compiler is NOT that commit and the verdict
#     should say so rather than assert it.
# Reported, NEVER fatal: gating uncommitted work is what these drivers are for.
# ── WHAT IS PORTED FROM THE .sh's STEP-2, AND WHAT IS DELIBERATELY NOT ───────
# NOT PORTED — the CLONE GATE. Its shapes (a) and (c) are about $SRC_DIR being
# absent, or populated-but-not-a-checkout, and about the harness then CLONING our
# repo onto the default branch. $RepoRoot is derived from $PSScriptRoot, so this
# driver always runs on the checkout it lives in and never clones dss-code-prime at
# all -- neither shape can occur, and inventing a gate for them would be ceremony.
# Nor does the .sh's stale-.git hybrid exist here: the measured instance of that was
# the rsync'd WSL tree, and this driver reads the real Windows checkout (verified in
# that same session: the .ps1 reported correctly while the .sh did not).
# PORTED, because nothing about them is host-shaped: the UNKNOWN(...)-never-empty
# fields, the .git guard, the DETACHED-HEAD mapping, the divergence count, and the
# DSS_BRANCH / DSS_COMMIT ASSERTIONS below. Those last two were the silent gap this
# comment previously said nothing about -- see the env-var block at the top of this
# file for the failure it produced.
if     (-not $dssIsRepo) { $dssHead   = "UNKNOWN(no .git under $RepoRoot)" }
elseif (-not $dssHead)   { $dssHead   = "UNKNOWN(rev-parse HEAD failed in $RepoRoot)" }
# A DETACHED HEAD makes `rev-parse --abbrev-ref HEAD` print the literal string
# "HEAD", which reads as a branch NAMED HEAD and would make the DSS_BRANCH mismatch
# message below nonsense ("but the tree is on 'HEAD'"). Mapped, as the .sh does at
# build-and-test.sh:549.
if     (-not $dssBranch)       { $dssBranch = 'UNKNOWN' }
elseif ($dssBranch -eq 'HEAD') { $dssBranch = 'DETACHED-HEAD' }
$dssDivergeNote = ''
if (-not $dssIsRepo) {
  # Same reason as above: `git status` in a non-checkout reports the ENCLOSING
  # repository's dirt. Not measuring is honest; measuring the wrong tree is not.
  $dssDivergeNote = " (divergence from HEAD UNVERIFIED -- no .git under $RepoRoot)"
} else {
  try {
    # --no-optional-locks: a plain `status` refreshes and REWRITES .git/index, and this
    # step's whole promise is "current checkout, untouched". Untracked files COUNT: a
    # source file HEAD never knew about is divergence exactly as much as an edit is.
    $porcelain = @(& git -C $RepoRoot --no-optional-locks status --porcelain 2>$null | Where-Object { $_ -ne '' })
    if ($LASTEXITCODE -ne 0) {
      $dssDivergeNote = ' (divergence from HEAD UNVERIFIED -- git status failed)'
    } elseif ($porcelain.Count -gt 0) {
      $dssDivergeNote = " (+$($porcelain.Count) file(s) differ from HEAD -- the sources built are NOT exactly this commit)"
    }
  } catch {
    # A throwing git (old git rejecting --no-optional-locks, or PowerShell's native
    # -command error preference) must never cost a run -- it costs the COUNT, and the
    # note says which.
    $dssDivergeNote = ' (divergence from HEAD UNVERIFIED -- git status could not run)'
  }
}
Info "  at $dssHead on $dssBranch$dssDivergeNote"
# ── the DECLARED ref, ASSERTED against the checkout ──────────────────────────
# PARITY with build-and-test.sh:777-799. Intent the operator STATES beats intent
# inferred from the filesystem. Both unset (the default) leaves behaviour exactly
# as it was: use the checkout as it is.
# ⚠ These are ASSERTIONS, never checkout instructions — the .sh made that choice
# deliberately and this driver holds to it: checking out a bare sha leaves a
# DETACHED HEAD, which contradicts the "current checkout, untouched" contract that
# is the whole reason a pre-commit probe can gate uncommitted work.
if ($DssBranchWant -and $dssBranch -ne $DssBranchWant) {
  Die @"
DSS_BRANCH='$DssBranchWant' but $RepoRoot is on '$dssBranch'.
      Refusing to spend a multi-hour corpus run on a branch you did not ask for.
      Check the branch out yourself — this harness NEVER switches our own repo, so
      that a probe tests the working tree exactly as it is — or drop DSS_BRANCH.
"@
}
if ($DssCommitWant) {
  if (-not $dssIsRepo) {
    Die "DSS_COMMIT='$DssCommitWant' cannot be verified: $RepoRoot is not a git checkout (no .git entry)."
  }
  # `--verify … ^{commit}` both resolves an abbreviation and proves the object is a
  # commit that EXISTS here; a typo or an unfetched sha fails loud instead of
  # silently comparing two strings that were never going to match.
  $dssCommitFull = Get-GitLine $RepoRoot @('rev-parse','--verify','--quiet',"$DssCommitWant^{commit}")
  if (-not $dssCommitFull) {
    Die "DSS_COMMIT='$DssCommitWant' does not resolve to a commit in $RepoRoot — fetch it, or fix the value."
  }
  if (-not $dssHeadLong) {
    Die "DSS_COMMIT='$DssCommitWant' cannot be verified: rev-parse HEAD failed in $RepoRoot."
  }
  if ($dssCommitFull -ne $dssHeadLong) {
    Die @"
DSS_COMMIT='$DssCommitWant' ($dssCommitFull) but $RepoRoot is at $dssHeadLong.
      The run would have validated the checkout, not the commit you named.
"@
  }
  Info "  DSS_COMMIT verified: HEAD is $dssCommitFull"
}
# The divergence is REPORTED, never fatal — a dirty tree is the NORMAL shape of a
# pre-commit probe, and refusing it would delete this driver's main use. It is
# WARNED about (not merely noted) so a commit-pinned run cannot read as byte-pinned:
# DSS_COMMIT proves which COMMIT is checked out, never that the sources match it.
if ($dssDivergeNote) { Warn "  $RepoRoot$dssDivergeNote" }
Pass "dss-code-prime checkout ready"
# <<< dss:src-provenance <<<

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

# ── Step 3+4 — derive the full-source recipe + stage (this host's POSIX shell) ─
# ★ TARGET-FREE BY CONSTRUCTION, AND THAT IS WHY IT RUNS ONCE FOR ALL FIVE LEGS.
# What this step produces — the TU list, the -I dirs, the -D defines, the staged
# sqlite sources + tcl/zlib headers — is sqlite's own portable C. Nothing here is
# keyed on a target; the per-target decisions (the recipe TRANSFORM, the stack
# reserve, the resolve-library binaries) are the LEG's, applied in Steps 6/7.
Step "3+4/9  Derive full-source testfixture recipe + stage sources/headers ($(if ($script:HostNeedsWsl) { 'WSL' } else { 'native POSIX' }))"
New-Item -ItemType Directory -Force -Path $Work | Out-Null
# NOTE the stage is NOT wiped here. The wipe happens inside the derive script BELOW,
# after the shared-clone lock is held — otherwise a run that is about to be refused
# has already destroyed its own staged corpus on the way to the refusal.
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
$StageShell = ToShellPath $Stage
# The derivation reproduces the .sh's Step-4 recipe logic (make -n
# testfixture USE_AMALGAMATION=0 → TUs + defines + -I dirs, with the
# libsqlite3.a core recovered via `ar t`), then STAGES the sqlite sources +
# generated derived sources + real tcl8.6/zlib headers into the driver-visible
# $Stage dir and writes the recipe as three files IN THE PATHS THIS DRIVER USES:
#   $Stage/tus.txt  $Stage/includes.base.txt  $Stage/defines.txt
# plus $Stage/zinc-src/{zlib.h,zconf.h} — the deriving host's copy, VERBATIM. The
# per-TARGET zlib header dirs ($Stage/zinc/<recipeTransform>/) and the per-stage
# include lists ($Stage/includes.<recipeTransform>.txt) are written just below,
# by stage-zinc.py, from the guards each leg declares in legs.json.
# The shell-side `win()` below performs that last translation: `wslpath -m` when
# the POSIX shell is WSL and the driver is on the Windows side of a boundary,
# and the identity when the shell and the driver share one filesystem.
$deriveScript = @'
set -Eeuo pipefail
__CLONE_LOCK_REGION__
for t in git gcc make ar tclsh; do
  command -v "$t" >/dev/null 2>&1 || {
    echo "MISSING tool: $t — install the recipe toolchain, e.g.:" >&2
    echo "    sudo apt-get install -y git build-essential tcl tcl-dev zlib1g-dev" >&2
    exit 1; }
done
# win <path> -> the same file spelled the way the DRIVER addresses it.
# `wslpath -m` (the forward-slash Windows form the manifest wants) when this shell
# is WSL and the driver is on the Windows side of that boundary; the IDENTITY when
# the shell and the driver share one filesystem. The body is substituted by the
# PowerShell side from the ONE host decision that also chose this shell — so there
# is no second place where a host could be mis-read, and `wslpath` (which does not
# exist outside WSL) is never named on a host that lacks it.
# ★ Defined HERE, before first use: bash resolves a function at CALL time, and the
# reference-oracle block below calls this long before the staging section does.
win() { __WIN_PATH_BODY__; }
# ── THE SHARED CORE, SOURCED ON THE POSIX SIDE ───────────────────────────────
# ★ THIS IS WHAT MAKES THE TWO DRIVERS ONE IMPLEMENTATION, not two that agree.
# The recipe derivation has always run in a POSIX shell here, so this script
# sources the SAME base-harness.sh build-and-test.sh does instead of carrying a
# second copy of the sed join, the ar recovery and the basename dedup. Three
# measured drifts came out of the old arrangement; the one this fixes on THIS
# side is D-HARNESS-SELFTEST-BSD-SED-PORTABILITY — the line below used to be
# `sed ':a;N;$!ba;s/\\\n/ /g'`, which BSD/macOS sed reads as one enormous LABEL
# and dies on, silently emitting only the recipe's FIRST line. This driver runs
# its derivation on a native POSIX host as well as through WSL, so that was not
# hypothetical.
BASE_HARNESS="__BASE_HARNESS_SH__"
[ -r "$BASE_HARNESS" ] || { echo "the shared harness core is missing: $BASE_HARNESS" >&2; exit 1; }
. "$BASE_HARNESS"
# ★ AND ITS VERSION IS ASSERTED, exactly as build-and-test.sh does at its own
# source site. This half of the driver carries the ENTIRE recipe derivation, so a
# stale core here does not look like an error — it looks like a smaller TU set, a
# missing define, a capability that quietly is not there. Checking only that the
# FILE EXISTS (which is all this did) tests the one failure mode that is already
# loud. The number must match $script:DssBaseHarnessVersion on the PowerShell
# side; both are bumped together when a contract changes.
[ "${DSS_BASE_HARNESS_VERSION:-0}" -ge 2 ] || {
  echo "the shared harness core $BASE_HARNESS is version ${DSS_BASE_HARNESS_VERSION:-<unset>}; this driver needs >= 2." >&2
  echo "A stale core does not fail loudly on its own: it silently loses capabilities (the TU drop" >&2
  echo "ledger, the archive-missing stop) that this derivation depends on." >&2
  exit 1; }
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
# clone-or-update sqlite (external dependency — DOES pull).
#
# ★ THIS BLOCK USED TO SWALLOW ITS OWN FAILURES. It read
#     git -C "$DIR" fetch --all --prune --quiet            || true
#     git -C "$DIR" pull  --rebase --quiet    2>/dev/null  || true
#   so a failed update (offline, detached HEAD, dirty tree, rebase conflict) was
#   discarded ALONG WITH the stderr explaining it, the run continued on a STALE
#   checkout, and Step 9 still printed an authoritative-looking `sqlite @ <sha>`.
#   That is a silent-WRONG-PROVENANCE defect, not merely a stale-corpus one: the
#   verdict names a version the run did not actually get. Under the
#   `set -Eeuo pipefail` at the top of this script a bare failure now ABORTS.
#
# ★ THE DEFAULT-BRANCH CHECKOUT IS LOAD-BEARING, and mirrors the .sh's
#   clone_or_update (incl. its `default_branch` helper — sqlite's default is
#   `master`/`trunk`, never `main`, so it must be RESOLVED, never assumed).
#   The two drivers SHARE this clone ($HOME/src/sqlite). Without the checkout the
#   .ps1 pulls whatever branch — or detached HEAD — the .sh last left behind, so
#   the WINDOWS leg's corpus version silently depended on the LINUX leg's
#   execution history. Resolving locally via symbolic-ref keeps the common path
#   OFFLINE-clean; `remote show origin` is only the fallback.
if [ -d "$DIR/.git" ]; then
  git -C "$DIR" fetch --all --prune --quiet
  SQLITE_BRANCH="$(git -C "$DIR" symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null || true)"
  SQLITE_BRANCH="${SQLITE_BRANCH#origin/}"
  if [ -z "$SQLITE_BRANCH" ]; then
    SQLITE_BRANCH="$(git -C "$DIR" remote show origin | sed -n 's/.*HEAD branch: //p')"
  fi
  [ -n "$SQLITE_BRANCH" ] || { echo "could not resolve sqlite's default branch in $DIR" >&2; exit 1; }
  git -C "$DIR" checkout --quiet "$SQLITE_BRANCH"
  git -C "$DIR" pull --rebase --quiet
elif [ ! -e "$DIR/configure" ]; then
  mkdir -p "$(dirname "$DIR")"
  git clone --quiet https://github.com/sqlite/sqlite.git "$DIR"
fi
[ -x "$DIR/configure" ] || { echo "no ./configure in $DIR — not a SQLite checkout" >&2; exit 1; }
BLD="$DIR/bld-dss"; mkdir -p "$BLD"
# ── WHICH SQLITE THIS RUN IS ACTUALLY TESTING ────────────────────────────────
# DECLARED in legs.json `stageBuild`, resolved by harness_legs.py ONCE on the
# PowerShell side and interpolated here. Byte-for-byte the same declaration
# build-and-test.sh reads, and that is the whole point: the two drivers SHARE
# $DIR, so a bare `configure` here would silently REVERT the .sh's capability
# flags on the next Windows run and leave the tree half-configured — the
# cross-driver spelling of the very defect this closes.
#
# ✔MEASURED 2026-08-06 on the elf64-x86_64 corpus: with these flags absent, 362
# of the 1,241 files the run called COMPLETED asserted nothing — they returned at
# their first `ifcapable` gate while DSS had already compiled the code they
# would have tested. D-HARNESS-CORPUS-FILES-COMPLETE-WITHOUT-ASSERTING-BECAUSE-
# CAPABILITIES-ARE-OFF.
STAGE_CONFIGURE_FLAGS="__STAGE_CONFIGURE_FLAGS__"
STAGE_MAKE_OPTIONS="__STAGE_MAKE_OPTIONS__"
STAGE_REQUIRED_DEFINES="__STAGE_REQUIRED_DEFINES__"
[ -n "$STAGE_CONFIGURE_FLAGS" ] || { echo "the stage build configuration did not reach the derive script -- legs.json stageBuild is the ONE declaration of which extensions the corpus tests, and configuring without it builds a sqlite with fts5/fts3/rtree/session OFF" >&2; exit 1; }
# ★ THE SAME BUILD-CONFIGURATION STAMP THE .sh KEEPS, and for a reason that bites
#   HARDER here: this driver did not have one at all, so the first run after a
#   capability change would re-run configure (rewriting OPT_FEATURE_FLAGS) while
#   every existing .o stayed NEWER than its .c — make skips them, and the fixture
#   links from a MIXTURE of two configurations. It is not only objects: main.mk
#   generates parse.c and keywordhash.h with $(OPT_FEATURE_FLAGS) too.
#   An ABSENT stamp FIRES, deliberately: a tree we cannot prove was built with
#   this configuration is exactly the tree that links a mixed fixture.
STAGE_STAMP="$BLD/.dss-stage-identity"
STAGE_STAMP_NOW="tclsh=$(echo 'puts $tcl_version' | tclsh 2>/dev/null || true) configure=$STAGE_CONFIGURE_FLAGS options=${STAGE_MAKE_OPTIONS:-<none>}"
STAGE_STAMP_WAS="$(cat "$STAGE_STAMP" 2>/dev/null || true)"
if [ "$STAGE_STAMP_WAS" != "$STAGE_STAMP_NOW" ]; then
  # Look at the target before destroying it: the path this driver builds, under
  # the checkout, carrying the marks of a configured sqlite build tree.
  if [ "$BLD" = "$DIR/bld-dss" ] && { [ -f "$BLD/Makefile" ] || [ -f "$STAGE_STAMP" ] || [ -f "$BLD/.dss-tcl-identity" ] || [ -z "$(ls -A "$BLD" 2>/dev/null)" ]; }; then
    echo "the build configuration behind $BLD changed -- REBUILDING IT FROM SCRATCH" >&2
    echo "      was: ${STAGE_STAMP_WAS:-<no stamp: this tree predates the configuration stamp>}" >&2
    echo "      now: $STAGE_STAMP_NOW" >&2
    rm -rf "$BLD"; mkdir -p "$BLD"
  else
    echo "the build configuration behind $BLD changed, and that directory could not be identified as this driver's own -- refusing to delete it. Remove it by hand once you have confirmed what it is: rm -rf '$BLD'" >&2
    exit 1
  fi
fi
# shellcheck disable=SC2086
( cd "$BLD" && "$DIR/configure" $STAGE_CONFIGURE_FLAGS >/dev/null 2>&1 )
printf '%s\n' "$STAGE_STAMP_NOW" > "$STAGE_STAMP"
rm -f "$BLD/.dss-tcl-identity"
# DID THE FLAGS ACTUALLY TAKE? A configure flag can be accepted and do nothing
# (✔MEASURED: `--memsys3` exits 0 and emits no define, MEMSYS5 wins), and upstream
# can retire one at any pull. The $(OPTIONS) defines are NOT checked here — they
# never reach OPT_FEATURE_FLAGS, they arrive on the make line — they are checked
# on the derived recipe below, which is where they become visible.
_optflags="$(sed -n 's/^OPT_FEATURE_FLAGS[[:space:]]*=[[:space:]]*//p' "$BLD/Makefile" | sed -n '1p')"
_missing_cfg=""
for _d in $STAGE_REQUIRED_DEFINES; do
  case " $STAGE_MAKE_OPTIONS " in *" -D$_d "*) continue ;; esac
  case " $_optflags " in *" -D$_d "*|*" -D$_d="*) ;; *) _missing_cfg="$_missing_cfg $_d" ;; esac
done
if [ -n "$_missing_cfg" ]; then
  echo "configure accepted its flags and did NOT produce the defines they exist for -- missing from OPT_FEATURE_FLAGS:$_missing_cfg" >&2
  echo "      flags passed: $STAGE_CONFIGURE_FLAGS" >&2
  echo "      OPT_FEATURE_FLAGS: ${_optflags:-<empty>}" >&2
  exit 1
fi

# ── the -L that TCL_LIBS needs but does not carry (Tcl 9 externalised tommath) ─
# WHY THIS EXISTS — do NOT "simplify" the -L away:
# Tcl 8.6 BUNDLED libtommath inside libtcl. Tcl 9 does NOT, so a Tcl-9
# tclConfig.sh declares it as an EXTERNAL dependency in TCL_LIBS (MEASURED on the
# macOS leg this cycle: TCL_LIBS=' -lz -lpthread -framework CoreFoundation
# -ltommath'), and main.mk's testfixture rule passes $TCL_LIBS through VERBATIM —
# with no `-L` to go with it. The reference build then dies with
# `ld: library 'tommath' not found` while every .o still compiles fine, i.e. the
# ATTRIBUTION ORACLE silently disappears (D-SQLITE-GCC-REFERENCE-FIXTURE-AS-ORACLE).
#
# ★ ON THIS DRIVER THE DEFECT IS LATENT, NOT LIVE — and that is precisely why it is
#   repaired here rather than "when it bites". The WSL distro this harness pins is
#   Debian/Ubuntu with Tcl 8.6, where libtommath is bundled and nothing is missing.
#   The day that distro's default Tcl becomes 9.x the reference build stops linking
#   WITH NO WARNING — a `do-release-upgrade` is the entire trigger. A defect whose
#   activation condition is someone else's release schedule is still a defect.
#
# Mirrors build-and-test.sh's DESIGN, not its syntax:
#   · PROBE-GATED. Every `-l<name>` in TCL_LIBS is LINK-PROBED first; only an
#     UNRESOLVABLE one earns a `-L`. On every WSL distro today every probe passes,
#     NOTHING is added, and both the configure invocation and the link line stay
#     byte-identical to their pre-change form. It is a strict NO-OP on Tcl 8.6.
#   · the probe uses the MAKEFILE'S OWN `CC`, read back out of the Makefile
#     configure just wrote — never a bare `gcc` off PATH. The probe must be the
#     compiler that will actually perform the link, or it lies about what links.
#   · WHICH tclConfig.sh matters, and only configure knows: it is the one configure
#     recorded as TCL_CONFIG_SH — literally the file the link's `.tclenv.sh`
#     sources. So DERIVE IT AFTER configure, off the Makefile, instead of
#     re-guessing the selection and risking a different answer.
#   · the DIRECTORY is DERIVED, never hardcoded. The .sh's chain is
#     pkg-config → `brew --prefix` → its LIB_ROOTS because it also runs on macOS.
#     This is the LINUX chain, the same design against the authorities that exist
#     inside a WSL rootfs: pkg-config's own libdir (the library's authoritative
#     self-description) → ldconfig's cache (the dynamic linker's OWN answer, which
#     is what makes a multiarch dir like /usr/lib/x86_64-linux-gnu findable at all)
#     → the compiler's own `-print-search-dirs` library list. `brew --prefix` is
#     deliberately NOT ported: it exists for the macOS keg layout, has no bearing
#     on a Debian/Ubuntu rootfs, and would be machinery that can never fire.
mk_var() {                      # mk_var <makefile> <NAME> -> its value (first defn, trimmed)
  sed -n "s/^$2[[:space:]]*=[[:space:]]*//p" "$1" 2>/dev/null \
    | sed 's/[[:space:]]*$//' | sed -n '1p' || true
}
PROBE_CC=(cc)                   # replaced with the Makefile's own CC a few lines down
probe_link_l() {                # probe_link_l <-L…/-l… args> -> 0 iff the LINK succeeds
  local tmp rc
  tmp="$(mktemp -d)" || return 1
  printf 'int main(void){return 0;}\n' > "$tmp/probe.c"
  # status taken DIRECTLY off the compiler — never through a pipe, which would
  # report the pipe's status instead. The if/else keeps `set -e` out of it.
  if "${PROBE_CC[@]}" "$tmp/probe.c" "$@" -o "$tmp/probe.bin" >/dev/null 2>&1
  then rc=0; else rc=$?; fi
  rm -rf "$tmp"
  return "$rc"
}
dir_holds_lib() {               # dir_holds_lib <dir> <name> -> 0 iff lib<name>.* is there
  local d="$1" n="$2" f
  [ -n "$d" ] && [ -d "$d" ] || return 1
  # an unmatched glob stays LITERAL and `-e` then rejects it — no nullglob needed.
  for f in "$d/lib$n".so "$d/lib$n".so.* "$d/lib$n".a; do
    [ -e "$f" ] && return 0
  done
  return 1
}
ldconfig_bin() {                # ldconfig lives in /sbin, which Debian leaves OFF a non-root PATH
  local c
  for c in ldconfig /sbin/ldconfig /usr/sbin/ldconfig; do
    command -v "$c" >/dev/null 2>&1 && { printf '%s' "$c"; return 0; }
  done
  return 0
}
libdir_for() {                  # libdir_for <name> -> a dir holding lib<name>.* (or "")
  local n="$1" d ldc
  if command -v pkg-config >/dev/null 2>&1; then
    for d in "$(pkg-config --variable=libdir "lib$n" 2>/dev/null || true)" \
             "$(pkg-config --variable=libdir "$n"    2>/dev/null || true)"; do
      dir_holds_lib "$d" "$n" && { printf '%s' "$d"; return 0; }
    done
  fi
  ldc="$(ldconfig_bin)"
  if [ -n "$ldc" ]; then
    while IFS= read -r d; do
      dir_holds_lib "$d" "$n" && { printf '%s' "$d"; return 0; }
    done < <("$ldc" -p 2>/dev/null | sed -n "s|.*=> \(/.*\)/lib$n\.so.*|\1|p" | sort -u)
  fi
  while IFS= read -r d; do
    dir_holds_lib "$d" "$n" && { printf '%s' "$d"; return 0; }
  done < <( { "${PROBE_CC[@]}" -print-search-dirs 2>/dev/null || true; } \
            | sed -n 's/^libraries: *=*//p' | tr ':' '\n' | sed 's|//*$||' | sort -u )
  return 0
}
# ★ every diagnostic below goes to STDERR under a MARKER prefix. Both halves are
#   load-bearing, for DIFFERENT reasons:
#     (a) stdout IS tcl_libs_ldflags's return value — an info line leaking into it
#         would be spliced onto the front of REF_LDFLAGS and handed to configure
#         as part of the flag;
#     (b) the PowerShell side only PRINTS $deriveOut when the derivation FAILS, so
#         on a SUCCESSFUL run an unmarked note is swallowed whole. The marker is
#         what lets the .ps1 re-emit these as Info/Warn — see the REF-LINK-* loop
#         there. Rename one end and the operator silently stops being told.
ref_note() { echo "REF-LINK-NOTE=$*" >&2; }
ref_warn() { echo "REF-LINK-WARN=$*" >&2; }
tcl_libs_ldflags() {            # tcl_libs_ldflags <tclConfig.sh> -> "-Ldir …" | ""
  local cfg="$1" libs tok name dir out=""
  local -a toks=()
  [ -n "$cfg" ] && [ -f "$cfg" ] || return 0
  libs="$( . "$cfg" >/dev/null 2>&1; printf '%s' "${TCL_LIBS:-}" )" || return 0
  # `read -ra` splits on IFS and does NOT glob — safer than `for tok in $libs`.
  read -r -a toks <<< "$libs" || true
  [ ${#toks[@]} -gt 0 ] || return 0
  for tok in "${toks[@]}"; do
    case "$tok" in -l?*) name="${tok#-l}" ;; *) continue ;; esac   # skips -framework X etc.
    probe_link_l -l"$name" && continue        # already resolvable → add NOTHING
    dir="$(libdir_for "$name")"
    if [ -z "$dir" ]; then
      ref_warn "tclConfig.sh declares -l$name in TCL_LIBS, which ${PROBE_CC[*]} cannot resolve and neither pkg-config, ldconfig, nor the compiler's own search dirs can locate. The reference testfixture -- the ATTRIBUTION ORACLE -- will NOT link. Install lib$name-dev inside the WSL distro."
      continue
    fi
    if probe_link_l -L"$dir" -l"$name"; then
      case " $out " in *" -L$dir "*) ;; *) out="${out:+$out }-L$dir" ;; esac
      ref_note "-l$name is not on a default search path -- adding -L$dir"
    else
      ref_warn "lib$name was found under $dir, yet ${PROBE_CC[*]} still cannot link -l$name against it (wrong architecture?). The reference testfixture will NOT link."
    fi
  done
  printf '%s' "$out"
}
_mk_cc="$(mk_var "$BLD/Makefile" CC)"
[ -n "$_mk_cc" ] && read -r -a PROBE_CC <<< "$_mk_cc"      # handles `ccache gcc`
REF_LDFLAGS="$(tcl_libs_ldflags "$(mk_var "$BLD/Makefile" TCL_CONFIG_SH)")"
if [ -n "$REF_LDFLAGS" ]; then
  # The mechanism is sqlite's OWN documented client knob: `LDFLAGS=…` handed to
  # CONFIGURE (not to make) lands in the Makefile as `LDFLAGS.configure`
  # (Makefile.in: `LDFLAGS.configure = @LDFLAGS@`), which main.mk folds into
  # $(LDFLAGS.libsqlite3) and therefore onto the testfixture link line. Chosen over
  # exporting LIBRARY_PATH for one `make` because it BAKES THE FIX INTO THE TREE:
  # the oracle exists to be USED, and a human re-running `make testfixture` by hand
  # inside the WSL clone to attribute a failure must get a link too, not just this
  # script. Also chosen over passing `LDFLAGS.configure=…` to make, which main.mk
  # explicitly says not to rely on.
  # The re-configure fires ONLY when a -L is genuinely missing, so a host that needs
  # none — every WSL distro today — still configures EXACTLY ONCE.
  # (no "reference link" prefix in the text — the PowerShell side already adds one)
  ref_note "re-running configure with LDFLAGS=$REF_LDFLAGS to supply the missing search path"
  # shellcheck disable=SC2086
  ( cd "$BLD" && "$DIR/configure" $STAGE_CONFIGURE_FLAGS "LDFLAGS=$REF_LDFLAGS" >/dev/null 2>&1 )
  # FAIL LOUD if it did not land. A silent miss — say sqlite reshapes the @LDFLAGS@
  # substitution upstream — would put us straight back to an oracle that quietly is
  # not there, which is the exact failure mode this block repairs.
  if grep -qF -- "$REF_LDFLAGS" "$BLD/Makefile"; then
    ref_note "LDFLAGS.configure now carries $REF_LDFLAGS"
  else
    ref_warn "configure did NOT carry LDFLAGS='$REF_LDFLAGS' into $BLD/Makefile -- the reference testfixture will almost certainly fail to link; sqlite's LDFLAGS.configure / @LDFLAGS@ substitution may have changed shape upstream."
  fi
fi

# ── the reference build, and the PRESERVED oracle ────────────────────────────
# The build generates every derived .c (parse.c/opcodes.c/ctime.c/tclsqlite-ex.c/
# fts5.c…) + libsqlite3.a, which the DSS TU set needs — AND, when it links, it
# produces the ATTRIBUTION ORACLE. A link miss stays TOLERATED exactly as before
# (the byproducts are still harvested and the run continues); what changes is that
# the binary is no longer thrown away and the log is no longer sent to /dev/null.
#
# ★ THE `rm -f "$BLD/testfixture"` BELOW IS LOAD-BEARING — `make -n` only PRINTS a
#   target's recipe when the target is MISSING. Harvesting that one cc/link line is
#   the ENTIRE reason this step exists (TU list + -D defines + -I dirs), so dropping
#   the `rm` empties $RECIPE and the <150-TU / <18-define floor below kills the run.
#   The copy and the `rm` are a PAIR: the binary is moved OUT of the make target's
#   path *precisely so* the delete can happen without destroying the oracle. The
#   copy is not a make target and not a prerequisite of one, so `make -n` still sees
#   `testfixture` missing and still prints the recipe.
#
# ★ WHAT THIS ORACLE IS, AND — just as important — WHAT IT IS NOT.
#   It is a LINUX/gcc ELF fixture built inside WSL. The leg under test is a native
#   Windows pe64 binary linking tcl86.dll + the MS CRT. So unlike the .sh's, this
#   oracle is ONE-SIDED and must be read that way:
#     · oracle FAILS the same .test  → strong exoneration. The failure is upstream
#       or test-suite, not DSS codegen. Every confound this driver already carries
#       (walsetlk wall-clock, the zipfile-25.0 testdir leak, recoverfault's OOM
#       oracle) is exactly this shape and would have been settled by it.
#     · oracle PASSES                → INCONCLUSIVE. It does NOT convict DSS: a
#       Windows/CRT platform difference passes on Linux by construction. The
#       fpconv1-2.0 anchor at the top of this file is the proof — it was settled by
#       a gcc build linking msvcrt.dll, i.e. a WINDOWS reference, which this is not.
#   That asymmetry is why the verdict line spells it out instead of just printing a
#   path. An oracle whose limits are not stated is a trap, not evidence.
#
# It is preserved into $STAGE — this harness's OWN output tree, NOT the sqlite
# checkout — so sqlite's `make clean`/`distclean` can never take it, and it is
# Windows-visible so a human triaging on the Windows side can see it exists. There
# is no counterpart to the .sh's "clear the previous run's copy BEFORE building":
# `rm -rf "$STAGE"` at the top of this script already guarantees the stronger
# property (past that line the preserved path holds THIS run's binary or nothing),
# and sqlite is pulled on every run, so a stale oracle would attribute against
# sources that are no longer the ones under test — strictly worse than none.
REF_KEEP="$STAGE/reference-testfixture"
REF_LOG="$STAGE/reference-build.log"
if ( cd "$BLD" && make -s testfixture USE_AMALGAMATION=0 "OPTIONS=$STAGE_MAKE_OPTIONS" -j"$(nproc 2>/dev/null || echo 4)" ) > "$REF_LOG" 2>&1; then
  # Plain `cp` + a best-effort `chmod +x`, NOT the .sh's `cp -p`. $STAGE is a
  # DrvFs (/mnt/c) path: `cp -p` there can fail on the ownership/timestamp
  # preservation it cannot honour and report non-zero for a copy that in fact
  # landed — which would make this announce a MISSING oracle that is sitting right
  # there. The copy is the load-bearing half; DrvFs already presents files as 0777
  # by default, so the chmod only matters when the mount carries real metadata.
  #
  # ★ BUT THE TOLERATED chmod IS THEN ASSERTED, and that is the difference
  # between "best effort" and "silently maybe". `chmod … || true` on its own
  # means an oracle can be announced as available while carrying no exec bit —
  # the one property every later use of it depends on. The `-x` test states the
  # outcome instead of hoping for it, on both mount kinds.
  if cp "$BLD/testfixture" "$REF_KEEP"; then
    chmod +x "$REF_KEEP" 2>/dev/null || true
    if [ -x "$REF_KEEP" ]; then
      echo "REF-ORACLE=$REF_KEEP"
      echo "REF-ORACLE-WIN=$(win "$REF_KEEP")"
    else
      echo "REF-ORACLE-MISS=reference testfixture was COPIED to $REF_KEEP but is not executable there (chmod +x did not take on this filesystem), so it cannot be run as an oracle."
    fi
  else
    echo "REF-ORACLE-MISS=reference testfixture LINKED but could NOT be preserved to $REF_KEEP -- it is about to be deleted to expose the recipe, so no oracle survives this run. Check permissions / free space."
  fi
else
  echo "REF-ORACLE-MISS=reference gcc testfixture did not fully link (tolerated -- byproducts + recipe still harvested). Log KEPT at $(win "$REF_LOG") -- READ IT.${REF_LDFLAGS:+ NOTE: link-path repair WAS in effect (LDFLAGS=$REF_LDFLAGS), so this is a DIFFERENT miss.}"
fi
rm -f "$BLD/testfixture"
RECIPE="$STAGE/testfixture-recipe.txt"
AR="$BLD/libsqlite3.a"; [ -f "$BLD/.libs/libsqlite3.a" ] && AR="$BLD/.libs/libsqlite3.a"
# ── THE FIXTURE RECIPE, DERIVED BY THE SHARED CORE ───────────────────────────
# ★ THE SAME dss_bh_emit_recipe THE CLI DERIVATION BELOW USES, and the same one
# build-and-test.sh's Step 4 uses. This block used to run `make -n` itself and
# then re-implement the span/archive TU recovery, the dedup and the floors — a
# hand-kept copy of the shared function sitting three lines above a call to it.
# The .sh twin carried the same copy. Two callers on the private path and two on
# the shared one is how the drift this core was extracted to end gets rebuilt.
# whole-blob + token-scope `all`: `make -n testfixture` runs with every
# prerequisite already built, so the recipe IS essentially the one link command
# and there is no bootstrap whose foreign -D must be kept out.
if ! dss_bh_emit_recipe \
      --build-dir "$BLD" --make-target testfixture --recipe-file "$RECIPE" \
      --make-var USE_AMALGAMATION=0 \
      --make-var "OPTIONS=$STAGE_MAKE_OPTIONS" \
      --prereq-mode link-line --always-make 1 --token-scope recipe \
      --archive "$AR" --archive-from-span 0 \
      --search-root "$DIR/src" --search-root "$DIR/ext" --search-root "$BLD" \
      --min-tus 150 --min-defines 18 \
      --out-tus "$STAGE/tus.shell.txt" \
      --out-defines "$STAGE/defines.txt" \
      --out-includes "$STAGE/recipe-includes.shell.txt"; then
  echo "the testfixture recipe derivation FAILED -- see $RECIPE" >&2; exit 1
fi
mapfile -t TUS         < "$STAGE/tus.shell.txt"
mapfile -t RECIPE_DEFS < "$STAGE/defines.txt"
mapfile -t SQLITE_INCS < "$STAGE/recipe-includes.shell.txt"

# ── the sqlite3 CLI: reference oracle + its OWN recipe ───────────────────────
# The .sh twin's Step 4 does exactly this, in the same order and for the same
# reasons; read the comments there. In short:
#   · `make sqlite3.c shell.c tclsqlite3.c` first — asking make for the CLI
#     regenerates fts5.c/sqlite3.h and leaves the amalgamation ORPHANS behind at
#     an older vintage, which reds the coherence gate
#     (D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE). Upstream's own rule, not a patch.
#   · build + PRESERVE a gcc `sqlite3d` — the CLI ATTRIBUTION ORACLE. The SAME
#     TUs from the SAME staged tree, built by upstream's own rule. NOT "the same
#     defines, only the compiler differs": upstream compiles the library with
#     SQLITE_CORE and shell.c without it (main.mk:2160-2166) and the reference
#     inherits that split, while DSS builds one program from the UNION. See the
#     matched-control note in build-and-test.sh's Step 4 and cli-smoke.py's
#     docstring — all three used to carry the same overclaim.
#   · then delete it, so `make -n sqlite3d` prints a recipe at all.
#   · derive in LINK-LINE mode: sqlite3d's prerequisites are .o files, and a
#     whole-blob scrape absorbs tool/lemon.c + lempar.c + mksourceid.c
#     (BUILD-HOST tools) whenever the objects are stale.
if ( cd "$BLD" && make sqlite3.c shell.c tclsqlite3.c "OPTIONS=$STAGE_MAKE_OPTIONS" ) > "$STAGE/amalgamation-regen.log" 2>&1; then
  echo "AMALGAMATION-REGEN=ok"
else
  echo "AMALGAMATION-REGEN=FAILED (tolerated here -- the coherence gate renders the verdict). Log: $(win "$STAGE/amalgamation-regen.log")"
fi
REF_CLI_KEEP="$STAGE/reference-sqlite3"
REF_CLI_LOG="$STAGE/reference-cli-build.log"
if ( cd "$BLD" && make -s sqlite3d "OPTIONS=$STAGE_MAKE_OPTIONS" -j"$(nproc 2>/dev/null || echo 4)" ) > "$REF_CLI_LOG" 2>&1 && [ -x "$BLD/sqlite3d" ]; then
  # Plain `cp` + a tolerated `chmod`, for the DrvFs reason spelled out at the
  # reference TESTFIXTURE above — and then ASSERTED, for the reason spelled out
  # there too: an oracle announced without an exec bit is an oracle that will
  # fail every one of cli-smoke.py's 14 assertions for a reason that has nothing
  # to do with either binary, which is precisely the "reference GIVEN BUT
  # UNUSABLE" case that gate now has to announce.
  if cp "$BLD/sqlite3d" "$REF_CLI_KEEP"; then
    chmod +x "$REF_CLI_KEEP" 2>/dev/null || true
    if [ ! -x "$REF_CLI_KEEP" ]; then
      echo "REF-CLI-MISS=the reference sqlite3 CLI was COPIED to $REF_CLI_KEEP but is not executable there (chmod +x did not take on this filesystem), so no smoke failure could be EXONERATED against it."
    else
    # BOTH spellings: the reference is a LINUX binary, so a Windows-side driver
    # reaches it through its launcher in the SHELL namespace, while every log
    # line and existence check wants the driver's own spelling.
    echo "REF-CLI=$REF_CLI_KEEP"
    echo "REF-CLI-WIN=$(win "$REF_CLI_KEEP")"
    fi
  else
    echo "REF-CLI-MISS=the reference sqlite3 CLI LINKED but could not be preserved to $REF_CLI_KEEP -- it is about to be deleted to expose its recipe, so no CLI oracle survives this run."
  fi
else
  echo "REF-CLI-MISS=the reference gcc sqlite3 CLI did not build (tolerated -- the CLI legs still build). Log KEPT at $(win "$REF_CLI_LOG") -- READ IT. Without it NO smoke failure can be EXONERATED."
fi
rm -f "$BLD/sqlite3d"
CLI_RECIPE="$STAGE/sqlite3-cli-recipe.txt"
#   · `--always-make` + `--token-scope recipe`: `make -n` prints only what it
#     WOULD do, so with the objects current it prints ONE line (the link) whose
#     -D set omits SQLITE_CORE — and without that, ext/icu/icu.c:31-33 demands
#     <unicode/*.h> and the CLI dies with four error[F001A]. `-B` prints
#     everything (still a DRY RUN); `recipe` scope keeps the jimsh/lemon
#     bootstrap's foreign -D out. The floor below is 18; the regression that
#     actually matters — losing SQLITE_CORE when the -D tokens are read off the
#     link line alone — is caught BY NAME a few lines down, because a count
#     cannot say WHICH define went missing and a count with no headroom reds on
#     an unrelated upstream edit. (This note used to claim a floor of 19 that the
#     call below sets to 18: two numbers for one fact, five lines apart.)
if ! dss_bh_emit_recipe \
      --build-dir "$BLD" --make-target sqlite3d --recipe-file "$CLI_RECIPE" \
      --make-var "OPTIONS=$STAGE_MAKE_OPTIONS" \
      --prereq-mode link-line --always-make 1 --token-scope recipe \
      --archive "$AR" --archive-from-span 1 \
      --search-root "$DIR/src" --search-root "$DIR/ext" --search-root "$BLD" \
      --min-tus 100 --min-defines 18 \
      --out-tus "$STAGE/cli-tus.shell.txt" \
      --out-defines "$STAGE/cli-defines.txt" \
      --out-includes "$STAGE/cli-includes.shell.txt"; then
  echo "the sqlite3 CLI recipe derivation FAILED -- see $CLI_RECIPE" >&2; exit 1
fi
mapfile -t CLI_TUS  < "$STAGE/cli-tus.shell.txt"
mapfile -t CLI_INCS < "$STAGE/cli-includes.shell.txt"
# The CLI's only entry point. Asserted BY NAME: losing it still clears the TU
# floor on the 102 library sources, producing a program with no `main` that fails
# far later at the entry trampoline (sqlite3.c has no main either).
printf '%s\n' "${CLI_TUS[@]}" | grep -qE '/shell\.c$' \
  || { echo "the CLI TU set has no shell.c -- derived from $CLI_RECIPE" >&2; exit 1; }
# ★ SQLITE_CORE, ASSERTED BY NAME — the define whose absence is silent in every
# count. Without it ext/icu/icu.c:31-33 stops compiling to nothing and demands
# <unicode/*.h>, failing three minutes later as `error[F001A] got unicode/*.h`.
# It comes from the library COMPILE lines, so its absence means the -D tokens
# were read off the link line alone (see --token-scope recipe above).
grep -qx 'SQLITE_CORE' "$STAGE/cli-defines.txt" \
  || { echo "the CLI define set has no SQLITE_CORE -- the -D tokens were read from the link line alone; ext/icu/icu.c will demand <unicode/*.h>. Derived from $CLI_RECIPE" >&2; exit 1; }

# ── THE DECLARED CAPABILITIES REACHED **BOTH** DERIVED RECIPES ───────────────
# Asserted on BOTH targets because the asymmetry WAS the defect: ✔MEASURED
# 2026-08-06, the CLI recipe carried -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE
# while the testfixture recipe, derived from the SAME configured tree in the SAME
# run, carried neither. And this is the ONLY check that can see the $(OPTIONS)
# defines at all: they never appear in OPT_FEATURE_FLAGS, so if `--make-var
# OPTIONS=…` is dropped from a call site, this is what fires.
for _f in "testfixture:$STAGE/defines.txt:$RECIPE" "sqlite3-CLI:$STAGE/cli-defines.txt:$CLI_RECIPE"; do
  _what="${_f%%:*}"; _rest="${_f#*:}"; _defs="${_rest%%:*}"; _from="${_rest#*:}"
  _missing=""
  for _d in $STAGE_REQUIRED_DEFINES; do
    # `NAME` OR `NAME=VALUE` — same rule as the .sh twin, and as the
    # OPT_FEATURE_FLAGS check above which already accepts `-DNAME=`.
    grep -qE "^${_d}(=|$)" "$_defs" || _missing="$_missing $_d"
  done
  if [ -n "$_missing" ]; then
    echo "the $_what recipe is MISSING declared capabilities:$_missing" >&2
    echo "      declared (legs.json stageBuild.requiredDefines): $STAGE_REQUIRED_DEFINES" >&2
    echo "      make OPTIONS passed:                            ${STAGE_MAKE_OPTIONS:-<none>}" >&2
    echo "      derived from:                                   $_from" >&2
    echo "      A missing one means the library about to be built does not have that capability," >&2
    echo "      while the run would still report every one of its test files as 'completed' --" >&2
    echo "      having asserted nothing in any of them." >&2
    exit 1
  fi
done

# ── stage: sqlite sources + generated derived sources + tcl8.6/zlib headers ──
mkdir -p "$STAGE/sqlite/bld" "$STAGE/tclinc" "$STAGE/zinc-src" "$STAGE/sqlite/test"
cp -r "$DIR/src" "$STAGE/sqlite/src"
cp -r "$DIR/ext" "$STAGE/sqlite/ext"
cp "$BLD"/*.c "$STAGE/sqlite/bld/" 2>/dev/null || true
cp "$BLD"/*.h "$STAGE/sqlite/bld/" 2>/dev/null || true
# the .test corpus + its tcl harness (tester.tcl …) — testfixture.exe runs these
#
# ★★ STAGED AT `$STAGE/sqlite/test`, NOT `$STAGE/test`, AND THE DIRECTORY IT SITS
#    IN IS THE WHOLE POINT. It used to be `$STAGE/test`, whose parent has no
#    `ext/` — and sqlite resolves a third of its corpus RELATIVE TO testdir:
#    `test/permutations.test:90-104` builds `$alltests` from `$testdir/*.test`
#    PLUS `glob -nocomplain $testdir/../ext/rtree/*.test`,
#    `$testdir/../ext/fts5/test/*.test`, `$testdir/../ext/session/*.test` and
#    five more. `-nocomplain` means a missing directory is not an error and not a
#    warning: the globs simply returned NOTHING and the run looked complete.
#    ✔MEASURED on the last Windows corpus.log before this change: 1,018 files
#    completed, of which **fts5 0, rtree 0, session 0** — while fts3 (58),
#    analyze3, wherelimit and mem5, which live directly in `test/`, were all
#    there. So the three LARGEST capability families this cycle turned on —
#    110 + 27 + 37 inert files — could never have run on the Windows driver,
#    and the leg would have paid the whole extra compile cost for nothing.
#    `$STAGE/sqlite/ext` is ALREADY staged two lines up, so putting the test dir
#    beside it costs no extra copy and makes the staged tree mirror upstream's
#    real layout instead of a flattened approximation of it.
#    Everything downstream reads this path out of `testdir.win.txt`, so the move
#    is confined to these two lines and the `win` call that records it.
cp -r "$DIR/test/." "$STAGE/sqlite/test/" 2>/dev/null || true
# real tcl8.6 headers (parsed agnostically — NO descriptor, D-FFI-SHIPPED-LIBS-OS-ONLY)
# ★ KNOWN, STILL LATENT — NOT repaired by this cycle's Tcl-9 work above, and left
#   here on purpose rather than silently half-fixed. Two 8.6-shaped assumptions:
#     · this picks its tclConfig.sh with `find /usr/lib | head -1` rather than the
#       TCL_CONFIG_SH the Makefile records — so on a distro carrying BOTH 8.6 and 9
#       the headers staged for the pe64 compile can come from a DIFFERENT Tcl than
#       the one configure selected for the reference link;
#     · the fallback below globs `-path '*tcl8*'`, which cannot match a Tcl 9 tree
#       at all, so the "tcl.h not found" hard-fail below fires on a 9-only distro.
#   Both are the SAME Tcl-9 story as the -L repair above and both fire on the same
#   trigger (the WSL distro's default Tcl moving to 9.x). They are reported, not
#   patched, because fixing them changes which headers the pe64 leg is COMPILED
#   against — a behaviour change that needs its own measured run, not a drive-by.
TCLH="$( . "$(find /usr/lib -name tclConfig.sh 2>/dev/null | head -1)" >/dev/null 2>&1; printf '%s' "${TCL_INCLUDE_SPEC#-I}" )"
[ -f "$TCLH/tcl.h" ] || TCLH="$(dirname "$(find /usr/include -name tcl.h -path '*tcl8*' 2>/dev/null | head -1)")"
[ -f "$TCLH/tcl.h" ] || { echo "tcl.h not found in WSL — apt-get install tcl-dev" >&2; exit 1; }
cp "$TCLH"/*.h "$STAGE/tclinc/"
# zlib headers (staged privately so they never shadow anything on -I)
#
# ★★ THE DERIVING HOST'S COPY, STAGED VERBATIM AND CONFIGURED FOR NOBODY.
# (D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED, closed TF-C115.)
# This zconf.h has been through the deriving host's ./configure, which rewrote
# `#if 1 /* was set to #if 1 by ./configure */ #define Z_HAVE_UNISTD_H` — a
# measurement of THIS MACHINE, exactly like the recipe's HAVE_*/Z_HAVE_* defines
# that `recipeTransform` exists to drop. So it is copied to `zinc-src/` UNTOUCHED
# and no leg ever includes it: the PowerShell side runs stage-zinc.py, which
# writes ONE zinc/<recipeTransform>/ per declared stage with that stage's guards
# applied, and each leg's include list points at its own.
#
# What used to be here was a `perl -0777 -pi` that flipped Z_HAVE_UNISTD_H to
# `#if 0` IN PLACE — correct for the pe64 leg, wrong for every other one, and
# with a single shared zinc/ there was nowhere for the other four legs' header to
# go. The .ps1 therefore refused them (`poisoned`) rather than compile them
# against a header configured for a different target, and the refusal was right:
# MEASURED TF-C115, that flip is NOT type-neutral off Linux. On darwin `off_t` is
# `long long`, so the guard decides whether zlib's z_off_t is `long long` (guard
# on, matching the real libz.dylib) or `long` (guard off) — same width, DIFFERENT
# TYPE, which is exactly what D-LANG-TYPE-IDENTITY-VOCABULARY forbids. On the pe
# target the un-flipped header fails loud instead (`error[F001D] got unistd.h`).
ZH="$(find /usr/include -maxdepth 3 -name zlib.h 2>/dev/null | head -1)"
[ -f "$ZH" ] || { echo "zlib.h not found in WSL — apt-get install zlib1g-dev" >&2; exit 1; }
[ -f "$(dirname "$ZH")/zconf.h" ] || { echo "zconf.h not found beside $ZH — the pair is what ./configure rewrites; a zlib.h without it cannot be staged for any target" >&2; exit 1; }
mkdir -p "$STAGE/zinc-src"
cp "$ZH" "$(dirname "$ZH")/zconf.h" "$STAGE/zinc-src/"
echo "ZLIB-SRC=$(win "$STAGE/zinc-src")"

# remap a shell-side path under $DIR or the header dirs → its STAGED location; the
# `win()` defined at the top of this script then spells it the way the DRIVER does.
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
# ★ THE SAME ROUND-TRIP ASSERTION THE CLI's TU LIST GETS, and for the same
# reason: a remap() case that is missing for one path does not error, it drops
# the TU — and a short manifest still compiles, still links, and produces a
# smaller program. The CLI half has had this check; the FIXTURE half did not,
# which is a capability in one path and not its twin one level down.
_n_fx_src=${#TUS[@]}; _n_fx_dst="$(grep -c . "$STAGE/tus.txt" || true)"
[ "$_n_fx_dst" -eq "$_n_fx_src" ] || {
  echo "staging lost fixture TUs: derived $_n_fx_src, staged $_n_fx_dst -- a remap() case is missing for one of them" >&2; exit 1; }
# includes.base.txt: the sqlite -I dirs (remapped) + $BLD + tcl. NOT zlib: that
# dir is PER TARGET, so the PowerShell side appends each stage's own zinc/ to
# this base and writes one includes.<stage>.txt per stage. The base is
# leg-independent — everything in it is sqlite's own portable C.
: > "$STAGE/includes.base.txt"
{ echo "$BLD"; for d in "${SQLITE_INCS[@]}"; do echo "$d"; done; } | while read -r d; do
  [ -n "$d" ] || continue; s="$(remap "$d")"; [ -d "$s" ] && win "$s" >> "$STAGE/includes.base.txt"
done
win "$STAGE/tclinc" >> "$STAGE/includes.base.txt"
# defines.txt is written DIRECTLY by dss_bh_emit_recipe above (--out-defines) —
# a define carries no path, so unlike the TU and include lists there is nothing
# to remap and no second spelling to produce. It is deliberately NOT rewritten
# here from $RECIPE_DEFS: two writers of one file is how the two of them get to
# disagree.
# ── the CLI's own tus / includes, remapped + spelled the way the DRIVER reads ─
# Same remap+win treatment as the fixture's above: the derivation ran against the
# LIVE sqlite tree, but the manifest must name the STAGED copies.
#
# ★ THE CLI'S INCLUDE LIST IS NOT THE FIXTURE'S, and the difference is real. The
# fixture's base list ends with $STAGE/tclinc; the CLI must not see it. shell.c
# has no Tcl in it (✔MEASURED 2026-08-05: zero `tcl.h` / `Tcl_` references in the
# generated shell.c), so handing it the staged Tcl headers would be an undeclared
# input that can only ever shadow something. The zlib dir is NOT here for the same
# reason it is not in the fixture's base: it is PER TARGET, and the PowerShell
# side appends each stage's own zinc/ (D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED).
: > "$STAGE/cli-tus.txt"
for f in "${CLI_TUS[@]}"; do s="$(remap "$f")"; [ -f "$s" ] && win "$s" >> "$STAGE/cli-tus.txt"; done
: > "$STAGE/cli-includes.base.txt"
{ echo "$BLD"; for d in "${CLI_INCS[@]}"; do echo "$d"; done; } | while read -r d; do
  [ -n "$d" ] || continue; s="$(remap "$d")"; [ -d "$s" ] && win "$s" >> "$STAGE/cli-includes.base.txt"
done
# A remap that silently dropped TUs would produce a short manifest that still
# compiles a smaller program. Assert the count SURVIVED the staging round-trip.
_n_cli_src=${#CLI_TUS[@]}; _n_cli_dst="$(grep -c . "$STAGE/cli-tus.txt" || true)"
[ "$_n_cli_dst" -eq "$_n_cli_src" ] || {
  echo "staging lost CLI TUs: derived $_n_cli_src, staged $_n_cli_dst -- a remap() case is missing for one of them" >&2; exit 1; }
echo "CLI-TUS=$_n_cli_dst"
echo "CLI-DEFS=$(grep -c . "$STAGE/cli-defines.txt" || true)"
echo "CLI-INCS=$(grep -c . "$STAGE/cli-includes.base.txt" || true)"
# the staged test dir (Windows path) for the corpus run. It MUST be the one whose
# PARENT holds `ext/`, or sqlite's own `$testdir/../ext/**/*.test` globs resolve
# to nothing and the fts5 / rtree / session families silently vanish from the run.
win "$STAGE/sqlite/test" > "$STAGE/testdir.win.txt"
# ★ FAIL LOUD RATHER THAN GLOB INTO SILENCE. `-nocomplain` is exactly why the
#   previous layout produced a confident, complete-looking 1,018-file run with
#   three whole capability families missing: sqlite does not consider an absent
#   ext/ an error, so nothing anywhere said so. Assert the relationship the tier
#   depends on, in the terms sqlite itself uses.
for d in rtree fts5/test session; do
  [ -d "$STAGE/sqlite/test/../ext/$d" ] || {
    echo "the staged test dir has no ../ext/$d, so sqlite's permutations.test would glob it to NOTHING and drop that whole family from the corpus without any error -- 'testdir' and 'ext' must be siblings" >&2
    exit 1
  }
done

echo "RECIPE-TUS=$(wc -l < "$STAGE/tus.txt")"
echo "RECIPE-DEFS=$(wc -l < "$STAGE/defines.txt")"
echo "RECIPE-INCS=$(wc -l < "$STAGE/includes.base.txt")"
# SQLITE-HEAD is a PROVENANCE FIELD: Step 9 prints it as `sqlite : <dir> @ <sha>`.
# The bare `$(git rev-parse …)` this used to be printed an EMPTY field whenever git
# failed or $DIR was not a checkout -- "sqlite : /home/…/src/sqlite @" reads as fine
# rather than as unknown, which is exactly the shape build-and-test.sh closed by
# routing every provenance read through git_head_short (never empty, always says WHY
# it is unknown). `set -e` never sees a substitution that fails inside an argument,
# so nothing else would have caught it. Same UNKNOWN(<why>) vocabulary here.
if [ -e "$DIR/.git" ]; then
  # `-e`, not `-d`: a worktree/submodule checkout keeps .git as a FILE.
  SQLITE_HEAD="$(git -C "$DIR" rev-parse --short HEAD 2>/dev/null)" || SQLITE_HEAD=""
  [ -n "$SQLITE_HEAD" ] || SQLITE_HEAD="UNKNOWN(rev-parse HEAD failed in $DIR)"
else
  # git -C WALKS UPWARD out of a non-checkout, so without this guard the field could
  # name an enclosing repository's commit -- a confident wrong citation.
  SQLITE_HEAD="UNKNOWN(no .git under $DIR)"
fi
echo "SQLITE-HEAD=$SQLITE_HEAD"
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
# `win()`'s body — the ONLY host-shaped line in the derive script, substituted from
# the SAME Step-1 decision that chose the shell. `wslpath` is not even NAMED on a
# host that has no WSL.
$winPathBody = if ($script:HostNeedsWsl) { 'wslpath -m "$1"' } else { 'printf ''%s\n'' "$1"' }
# ── WHICH SQLITE THIS RUN IS ACTUALLY TESTING ────────────────────────────────
# Resolved ONCE, HERE, from the SAME legs.json block build-and-test.sh reads, and
# interpolated into the derive script. Not spelled out in either driver: the two
# share $SqliteWslDir, so a capability set that lived in the .sh alone would be
# silently reverted by the next Windows run's `configure`.
# ★ FAIL LOUD IF IT CANNOT BE RESOLVED, never fall back to "no flags" — that
#   fallback IS the bug: it configures a sqlite with fts5/fts3/rtree/session OFF,
#   and the corpus then reports ~270 test files as completed having asserted
#   nothing. D-HARNESS-CORPUS-FILES-COMPLETE-WITHOUT-ASSERTING-BECAUSE-
#   CAPABILITIES-ARE-OFF.
try {
  # Same shape as the --plan call above: capture with 2>&1 and take $LASTEXITCODE
  # DIRECTLY, never through a pipe, and keep the native call out of a context
  # where $ErrorActionPreference='Stop' would replace the diagnostic with a stack.
  $sbOut = @(& $python3.Source $LegsPy '--catalogue' $LegsJson '--stage-build' '--format' 'json' 2>&1)
  $sbRc  = $LASTEXITCODE
} catch {
  $sbRc  = if ($LASTEXITCODE) { $LASTEXITCODE } else { 1 }
  $sbOut = @("$_")
}
if ($sbRc -ne 0) {
  Die "could not resolve the sqlite stage build configuration (harness_legs.py --stage-build, rc=$sbRc):`n$(($sbOut | ForEach-Object { "      $_" }) -join "`n")`n      This is the ONE declaration of which extensions the corpus tests, shared with build-and-test.sh."
}
$StageBuild = $null
try { $StageBuild = (($sbOut | ForEach-Object { "$_" }) -join "`n") | ConvertFrom-Json }
catch { Die "harness_legs.py --stage-build exited 0 but did not print the JSON this driver reads ($($_.Exception.Message)). Output was:`n$(($sbOut | Select-Object -First 20 | ForEach-Object { "      $_" }) -join "`n")" }
$StageConfigureFlags = ($StageBuild.configureFlags -join ' ')
$StageMakeOptions    = [string]$StageBuild.makeOptions
$StageRequiredDefines = ($StageBuild.requiredDefines -join ' ')
if (-not $StageConfigureFlags -or -not $StageRequiredDefines) {
  Die "harness_legs.py --stage-build returned a configuration with no configureFlags or no requiredDefines. That is a contract break between the resolver and this driver, not a property of this host."
}
Info "stage capabilities: $StageConfigureFlags$(if ($StageMakeOptions) { "   make OPTIONS=$StageMakeOptions" })"
$deriveScript = $deriveScript.Replace('__CLONE_LOCK_REGION__', $cloneLockRegion).Replace('__SQLITE_WSL_DIR__', $SqliteWslDir).Replace('__STAGE_WSL__', $StageShell).Replace('__WIN_PATH_BODY__', $winPathBody).Replace('__BASE_HARNESS_SH__', (ToShellPath $BaseHarnessSh)).Replace('__STAGE_CONFIGURE_FLAGS__', $StageConfigureFlags).Replace('__STAGE_MAKE_OPTIONS__', $StageMakeOptions).Replace('__STAGE_REQUIRED_DEFINES__', $StageRequiredDefines) -replace "`r`n", "`n"
# ★ ASSERT THE SUBSTITUTION LANDED. A `.Replace` whose token was renamed on the
#   other side silently leaves the literal `__STAGE_…__` in the script, which
#   would reach `configure` as an unknown flag — or, worse, reach the `-n` guard
#   in the derive script as a NON-EMPTY string and pass it.
if ($deriveScript -match '__STAGE_[A-Z_]+__') {
  Die "the derive script still carries an unsubstituted stage-build token ($($Matches[0])). A literal `__STAGE_…__` is non-empty, so the derive script's own emptiness guard would ACCEPT it and hand it to ./configure."
}
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
# ★ The `bash -l -c "bash '<path>' 2>&1"` shape is preserved EXACTLY on the WSL
# host — a login shell is what puts the recipe toolchain on PATH there. On a native
# POSIX host the driver already IS inside a shell session, so the script is run
# directly and only the `2>&1` merge is reproduced (with PowerShell's own redirect,
# which is safe here because the same merge is happening one process out).
# ★★ `-e` ADDED, `-l` KEPT — they answer different questions and both are needed.
# `-l` is the LOGIN shell that puts the recipe toolchain on PATH (above). `-e` says
# WHO PARSES THE STRING: without it, `wsl.exe` hands the whole line to the distro's
# default shell first, so the payload is parsed twice and the first pass happens
# where nobody is looking (D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL;
# MEASURED evidence at Invoke-PosixCommand).
if ($script:HostNeedsWsl) {
  $deriveOut = @(& wsl.exe -e bash -l -c "bash '$(ToShellPath $tmpSh)' 2>&1")
} else {
  $deriveOut = @(& bash -l "$(ToShellPath $tmpSh)" 2>&1)
}
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
# ── the reference link repair + the ATTRIBUTION ORACLE (both live on the WSL side) ─
# $deriveOut is only PRINTED when the derivation FAILS, so everything the Tcl-9
# link repair and the oracle preservation have to say would otherwise vanish on a
# SUCCESSFUL run — which is exactly how an oracle goes missing with nobody noticing
# (D-SQLITE-GCC-REFERENCE-FIXTURE-AS-ORACLE). Re-emit the markers as ordinary
# Info/Warn lines. The marker NAMES are a CONTRACT with the ref_note/ref_warn/
# REF-ORACLE* emitters in the WSL script above: rename one end only and the
# operator silently stops being told anything.
foreach ($ln in $deriveOut) {
  if     ($ln -match '^REF-LINK-NOTE=(.*)$') { Info "reference link: $($Matches[1])" }
  elseif ($ln -match '^REF-LINK-WARN=(.*)$') { Warn "reference link: $($Matches[1])" }
}
# Deliberately NOT read through `Marker`: that helper indexes .Matches[0]
# unconditionally and throws when its marker is absent, and an ABSENT oracle is a
# normal, TOLERATED outcome here (the byproducts + recipe are what the run needs).
# This is the same null-safe shape the clone-lock notes above use.
$RefOracle = ''; $RefOracleWin = ''; $RefOracleMiss = ''
$om = ($deriveOut | Select-String -Pattern '^REF-ORACLE=(.+)$'      | Select-Object -Last 1)
if ($om) { $RefOracle     = $om.Matches[0].Groups[1].Value.Trim() }
$om = ($deriveOut | Select-String -Pattern '^REF-ORACLE-WIN=(.+)$'  | Select-Object -Last 1)
if ($om) { $RefOracleWin  = $om.Matches[0].Groups[1].Value.Trim() }
$om = ($deriveOut | Select-String -Pattern '^REF-ORACLE-MISS=(.+)$' | Select-Object -Last 1)
if ($om) { $RefOracleMiss = $om.Matches[0].Groups[1].Value.Trim() }
if ($RefOracle) {
  Info "oracle: reference gcc testfixture preserved -> $RefOracleWin"
} else {
  if (-not $RefOracleMiss) { $RefOracleMiss = 'the WSL derivation emitted no oracle marker at all (REF-ORACLE/REF-ORACLE-MISS) — the reference-build block above may have been edited out of the derive script.' }
  Warn "oracle: ABSENT -- $RefOracleMiss"
  Warn "        a corpus failure on this leg therefore cannot be EXONERATED against a non-DSS build."
}
if (-not (Test-Path (Join-Path $Stage 'tus.txt'))) { Die "recipe derivation produced no tus.txt:`n$($deriveOut -join "`n")" }

# ── the sqlite3 CLI's derivation, read back ──────────────────────────────────
# Null-safe, like the oracle block above and NOT via `Marker` (which indexes
# .Matches[0] unconditionally and throws when its marker is absent).
$nCliTus = ''; $RefCli = ''; $RefCliWin = ''; $RefCliMiss = ''
$m = ($deriveOut | Select-String -Pattern '^CLI-TUS=(.+)$'      | Select-Object -Last 1)
if ($m) { $nCliTus    = $m.Matches[0].Groups[1].Value.Trim() }
$m = ($deriveOut | Select-String -Pattern '^REF-CLI=(.+)$'      | Select-Object -Last 1)
if ($m) { $RefCli     = $m.Matches[0].Groups[1].Value.Trim() }
$m = ($deriveOut | Select-String -Pattern '^REF-CLI-WIN=(.+)$'  | Select-Object -Last 1)
if ($m) { $RefCliWin  = $m.Matches[0].Groups[1].Value.Trim() }
$m = ($deriveOut | Select-String -Pattern '^REF-CLI-MISS=(.+)$' | Select-Object -Last 1)
if ($m) { $RefCliMiss = $m.Matches[0].Groups[1].Value.Trim() }
$m = ($deriveOut | Select-String -Pattern '^AMALGAMATION-REGEN=(.+)$' | Select-Object -Last 1)
if ($m -and $m.Matches[0].Groups[1].Value -ne 'ok') { Warn "amalgamation regeneration: $($m.Matches[0].Groups[1].Value)" }
# ★ A MISSING CLI TU FILE IS FATAL, exactly like tus.txt. The alternative is a
# run that silently builds no CLI on any leg and still reports success — which is
# the "a capability that quietly is not there" failure this work exists to end.
if (-not (Test-Path (Join-Path $Stage 'cli-tus.txt'))) {
  Die "recipe derivation produced no cli-tus.txt — the sqlite3 CLI could not be derived:`n$($deriveOut -join "`n")"
}
Info "cli recipe: $nCliTus TUs (shell.c + the library TUs), from the derivation's own 'make -n sqlite3d'"
if ($RefCli) {
  Info "cli oracle: reference gcc sqlite3 preserved -> $RefCliWin"
} else {
  if (-not $RefCliMiss) { $RefCliMiss = 'the derivation emitted no CLI oracle marker at all (REF-CLI/REF-CLI-MISS) — the reference-CLI block may have been edited out of the derive script.' }
  Warn "cli oracle: ABSENT -- $RefCliMiss"
  Warn "        every smoke failure this run is therefore UNATTRIBUTABLE, and cli-smoke.py charges an unattributable failure to DSS by design."
}

# ── PER-TARGET zlib headers + PER-LEG include lists ──────────────────────────
# ★ D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED, closed. The staged zconf.h carries
# the DERIVING host's ./configure probe results; each leg declares what ITS target's
# answers are (legs.json `build.zconfGuards`) and stage-zinc.py — the SAME tool
# build-and-test.sh calls — writes one zinc/<recipeTransform>/ per declared stage.
# Nothing here knows what a guard means or which target has <unistd.h>: this loop
# joins a KEY the resolver derived onto a directory. That is the whole difference
# between "target-keyed" and "a branch in a driver".
#
# A stage that cannot be produced is NAMED, and every leg that would have used it
# is poisoned in Step 7 — there is deliberately no fallback to a sibling stage's
# copy, because that fallback is precisely the defect this closes.
$ZincSrc  = Join-Path $Stage 'zinc-src'
$ZincRoot = Join-Path $Stage 'zinc'
foreach ($h in @('zlib.h', 'zconf.h')) {
  if (-not (Test-Path (Join-Path $ZincSrc $h))) { Die "the derivation did not stage $h into $ZincSrc — every leg parses this one header, so there is no leg to build on any host. See the derive output above." }
}
#
# ★★ AND THE SECOND STAGED HEADER, SQLITE'S OWN ./configure OUTPUT
# (D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES). The
# recipe carries `_HAVE_SQLITE_CONFIG_H`, which makes sqliteInt.h
# `#include "sqlite_cfg.h"`, and the staged bld dir on every include list holds the
# DERIVING host's copy — so every leg inherited a Linux box's probe answers.
# MEASURED: that is how the macho legs came to fail on `off64_t`/`pread64`/
# `pwrite64`. Each leg declares its own answers (legs.json `build.configureAnswers`)
# and the SAME stage-zinc.py writes one cfg/<targetOs>/sqlite_cfg.h per stage. The
# two families are keyed DIFFERENTLY on purpose (recipeTransform vs target OS), so
# the include lists below are written per (zinc stage, config stage) PAIR.
$CfgRoot = Join-Path $Stage 'cfg'
$DerivedCfgH = Join-Path $Stage 'sqlite/bld/sqlite_cfg.h'
if (-not (Test-Path $DerivedCfgH)) { Die "the derivation did not stage sqlite_cfg.h into $Stage/sqlite/bld -- sqlite's ./configure writes it and the recipe's _HAVE_SQLITE_CONFIG_H makes every TU include it, so each leg's own copy is rewritten from it. Without it there is no leg to build on any host." }
$StageZincPy = Join-Path $PSScriptRoot 'stage-zinc.py'
if (-not (Test-Path $StageZincPy)) { Die "stage-zinc.py not found next to this script ($StageZincPy) — it is the single implementation of per-target staged-header production, shared with build-and-test.sh." }
$zincOut = & $python3.Source $StageZincPy `
             '--zlib-h'  (Join-Path $ZincSrc 'zlib.h') `
             '--zconf-h' (Join-Path $ZincSrc 'zconf.h') `
             '--dest'    $ZincRoot `
             '--sqlite-cfg-h' $DerivedCfgH `
             '--cfg-dest' $CfgRoot `
             '--catalogue' $LegsJson 2>&1
$zincRc = $LASTEXITCODE
# label -> the include-list file that leg's manifest must use. A leg absent from
# this map lost one of its two header stages and is poisoned in Step 7.
$LegIncludes  = @{}
$StageDirs    = @{}
$CfgStageDirs = @{}
foreach ($ln in $zincOut) {
  if ($ln -match '^ZINC-STAGE-OK=([^|]*)\|([^|]*)\|([^|]*)\|(.*)$') {
    $k = $Matches[1]; $StageDirs[$k] = $Matches[2]
    Info "zinc stage '$k' -> $($Matches[2])   [$($Matches[3])]"
    if ($Matches[4]) { Info "      note: $($Matches[4])" }
  } elseif ($ln -match '^ZINC-STAGE-FAIL=([^|]*)\|(.*)$') {
    Warn "zinc stage '$($Matches[1])' COULD NOT BE PRODUCED — $($Matches[2])"
  } elseif ($ln -match '^ZINC-STAGES=(.*)$') {
    Info "zinc stages: $($Matches[1]) produced"
  } elseif ($ln -match '^CFG-STAGE-OK=([^|]*)\|([^|]*)\|([^|]*)\|(.*)$') {
    $c = $Matches[1]; $CfgStageDirs[$c] = $Matches[2]
    Info "sqlite config stage '$c' -> $($Matches[2])   [$($Matches[3])]"
    if ($Matches[4]) { Info "      note: $($Matches[4])" }
  } elseif ($ln -match '^CFG-STAGE-FAIL=([^|]*)\|(.*)$') {
    Warn "sqlite config stage '$($Matches[1])' COULD NOT BE PRODUCED — $($Matches[2])"
  } elseif ($ln -match '^CFG-STAGES=(.*)$') {
    Info "sqlite config stages: $($Matches[1]) produced"
  } else { Info "      $ln" }
}
if ($StageDirs.Count -eq 0) { Die "stage-zinc.py produced NO per-target zlib header dir (rc=$zincRc):`n$($zincOut -join "`n")" }
if ($CfgStageDirs.Count -eq 0) { Die "stage-zinc.py produced NO per-target sqlite_cfg.h (rc=$zincRc). Every leg would fall back to the DERIVING host's copy in the staged bld dir, which is exactly D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES:`n$($zincOut -join "`n")" }
# ── ★★ AND NOW REMOVE THE DERIVING HOST'S COPY, because "the staged dir is FIRST
# on the include list" DOES NOT COVER EVERY TU.
#
# THE EXCEPTION, MEASURED 2026-08-05 (TF-C121). A quote include searches the
# INCLUDING FILE'S OWN DIRECTORY *BEFORE* THE INCLUDE LIST IS CONSULTED AT ALL
# (src/core/types/include_path_resolve.hpp: `resolveIncludePath` -- "try the
# including file's own directory FIRST, then each of `includeDirs`", C 6.10.2p3).
# The position argument is therefore sound only for an includer with no
# `sqlite_cfg.h` beside it. `sqliteInt.h` lives in sqlite/src/ and has none -- but
# `$Stage/sqlite/bld/ctime.c` DOES, and it is TU #1 in BOTH tus.txt and
# cli-tus.txt, i.e. the fixture and the CLI, on every leg. It opens with
# `#if defined(_HAVE_SQLITE_CONFIG_H) && !defined(SQLITECONFIG_H)` /
# `#include "sqlite_cfg.h"` / `#define SQLITECONFIG_H 1`, so it took the DERIVING
# host's header out of its own directory (HAVE_MALLOC_H, HAVE_PREAD64,
# HAVE_PWRITE64 -- all three answers the staging exists to correct) and then
# SHADOWED sqliteInt.h's guarded include for the rest of that TU. Latent only
# because ctime.c consumes HAVE_ISNAN alone, a row every target agrees on.
#
# ★ THIS IS NOT "PATCHING THE STAGED TREE" -- worth stating because that is a HARD
# rule here (the corpus must be unmodified upstream sqlite). `sqlite_cfg.h` is not
# upstream source: it is a GENERATED ARTEFACT OF THE HARNESS'S OWN `./configure`
# run, produced by the derivation above into a build dir this harness created, and
# its content is not discarded -- stage-zinc.py has just read it and rewritten it
# into one per-target copy per stage. On THIS side the point is even plainer: the
# whole $Stage tree is `rm -rf`'d and re-copied at the start of every run, so this
# removal cannot outlive the run that made it.
Remove-Item -LiteralPath $DerivedCfgH -Force
Info "removed the deriving host's copy at $DerivedCfgH - $($CfgStageDirs.Count) per-target copy/copies replace it; a quote include searches the includer's OWN dir first, so leaving it there let bld/ctime.c (TU #1) read the deriving machine's answers ahead of the whole include list"
# The (zinc stage, config stage) pairs the DECLARED legs actually need — built from
# the legs, never as a cross product of the two families.
$StagePairs = @{}
foreach ($lg in $AllLegs) {
  $k = "$($lg.build.headerStageKey)"; $c = "$($lg.build.configStageKey)"
  if ($StageDirs.ContainsKey($k) -and $CfgStageDirs.ContainsKey($c)) { $StagePairs["$k|$c"] = $true }
}
$IncBase = Get-Content -LiteralPath (Join-Path $Stage 'includes.base.txt')
# THE CLI'S OWN PER-STAGE INCLUDE LISTS. Same treatment over a DIFFERENT base:
# cli-includes.base.txt carries no $Stage/tclinc, because shell.c has no Tcl in it.
$CliLegIncludes = @{}
$CliIncBase = Get-Content -LiteralPath (Join-Path $Stage 'cli-includes.base.txt')
foreach ($pair in $StagePairs.Keys) {
  $k = $pair.Split('|')[0]; $c = $pair.Split('|')[1]
  # ★ THE cfg/ DIR GOES FIRST -- but position is only HALF the mechanism, and on
  # its own it was NOT ENOUGH. A quote include searches the including file's OWN
  # DIRECTORY *BEFORE* this list is consulted at all (C 6.10.2p3), so no list
  # position can outrank a `sqlite_cfg.h` sitting beside the includer -- which is
  # exactly the case for bld/ctime.c, TU #1 of both artefacts. The OTHER half is
  # the `Remove-Item $DerivedCfgH` above, which deletes the deriving host's copy so
  # that self-directory lookup misses everywhere; the measurement is in the comment
  # there. First is still the only placement that stays correct however the rest is
  # ordered, and it shadows nothing: the staged dir holds that one file.
  # utf8NoBOM, NOT ascii: these lines are PATHS, and `-Encoding ascii` replaces
  # every non-ASCII character with `?` — a user profile with an accent in it
  # would silently become an include dir that does not exist. gen-pe64-manifest.py
  # reads this file as UTF-8, and a BOM would corrupt its first entry.
  Set-Content -LiteralPath (Join-Path $Stage "includes.$k.$c.txt") `
    -Value (@(($CfgStageDirs[$c] -replace '\\','/')) + @($IncBase) + @(($StageDirs[$k] -replace '\\','/'))) `
    -Encoding utf8NoBOM
  Set-Content -LiteralPath (Join-Path $Stage "cli-includes.$k.$c.txt") `
    -Value (@(($CfgStageDirs[$c] -replace '\\','/')) + @($CliIncBase) + @(($StageDirs[$k] -replace '\\','/'))) `
    -Encoding utf8NoBOM
}
foreach ($lg in $AllLegs) {
  $k = "$($lg.build.headerStageKey)"; $c = "$($lg.build.configStageKey)"
  # BOTH staged headers or NEITHER. A leg with one of the two would be compiled
  # against somebody else's answers for the other, so it stays out of the map and
  # Step 7 poisons it by name.
  if ($StageDirs.ContainsKey($k) -and $CfgStageDirs.ContainsKey($c)) {
    $LegIncludes[$lg.label]    = (Join-Path $Stage "includes.$k.$c.txt")
    $CliLegIncludes[$lg.label] = (Join-Path $Stage "cli-includes.$k.$c.txt")
  }
}
# FIRST GATE ON THE SHARED INPUT — the moment the stage exists, before anything is
# read out of it. See Assert-StagedSourceCoherence for why this is a run-wide Die
# and not a per-leg verdict, and why it has no opt-out.
Assert-StagedSourceCoherence 'staged sqlite (Step 4)'
Pass "recipe: $nTus TUs, $nDefs defines, $nIncs include dirs (sqlite @ $sqliteHead) staged under $Stage"

# ── Step 5 — locate (or build) the dss-code-prime compiler ───────────────────
# Picks the NEWEST existing binary (build-rel Release / build MSVC / build-dbg
# Ninja-Debug) — newest-wins deliberately avoids a STALE Release binary that
# predates a project-config field (the exact trap that fails loud below). With
# no binary present (a fresh clone) it configures + builds Release into build-rel.
# ★ THE COMPILER IS A HOST BINARY AND HAS NOTHING TO DO WITH THE LEG SET: ONE
# dss-code-prime emits every one of the five targets, selected from config. That
# is why there is one Step 5 and not one per leg.
Step '5/9  Locate / build dss-code-prime (newest existing, else build Release)'
function Find-Dss {
  # Both spellings on every host: the executable suffix is a fact about the
  # machine this compiler RUNS on, and probing for a name that cannot exist here
  # costs one Test-Path.
  $names = @('dss-code-prime.exe', 'dss-code-prime')
  $dirs  = @(
    [System.IO.Path]::Combine($RepoRoot, 'build-rel', 'bin', 'dss'),
    [System.IO.Path]::Combine($RepoRoot, 'build', 'bin', 'dss', 'Release'),
    [System.IO.Path]::Combine($RepoRoot, 'build', 'bin', 'dss'),
    [System.IO.Path]::Combine($RepoRoot, 'build-dbg', 'bin', 'dss')
  )
  $cands = @()
  foreach ($d in $dirs) { foreach ($n in $names) { $cands += (Join-Path $d $n) } }
  $cands = $cands | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
             Sort-Object { (Get-Item -LiteralPath $_).LastWriteTime } -Descending
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

# ── Step 6 — PER-LEG build inputs (each leg's OWN tcl + z resolve-libraries) ──
# ★ WHAT CHANGED AND WHY. This used to be two globals — $TclDll/$ZlibDll — that
# silently WERE the pe64 leg's answer: a second leg would have linked against
# Windows DLLs. Each leg now resolves its own (tcl, z) pair from ITS OWN declared
# `libraries.provider` / `tclNames` / `zNames` / `searchPaths`.
#
# ★ A MISSING INPUT IS A NAMED, LOUD, NON-FATAL VERDICT — NOT A DEAD RUN AND NOT
# A SILENT SKIP. `skipped-build-input-missing` exists in the closed vocabulary
# (tests/test_support/arm_verdict_ledger.hpp) for exactly this: "the manifest
# asked for something and the machine could not supply it". It names every name
# and every path searched, so the operator can fix it, and the OTHER legs still
# run. What it must never be is invisible: the run-level rule in Step 9 turns
# ZERO VERIFIED LEGS + at least one ENVIRONMENTAL skip into a hard failure, which
# is how the old "pe64 DLL missing ⇒ Die" loudness survives this generalisation.
Step '6/9  Resolve each leg''s DECLARED build inputs (tcl + z)'

# Candidate roots for the `host-system` provider. legs.json declares NO
# searchPaths for it (its own lint forbids them there), so the roots come from the
# driver — and they are CANDIDATES in exactly the sense legs.json's
# $noHostKeyingComment means: every root is tried on every host, a hit is used, a
# miss costs one Test-Path. No root is skipped because of the host we are on.
# $env:DSS_HOST_LIBDIR is the operator's explicit escape hatch, searched first.
function Get-HostSystemLibRoots {
  $roots = New-Object 'System.Collections.Generic.List[string]'
  foreach ($v in @($env:DSS_HOST_LIBDIR, $env:LD_LIBRARY_PATH, $env:DYLD_LIBRARY_PATH, $env:DYLD_FALLBACK_LIBRARY_PATH)) {
    if ($v) { foreach ($p in ("$v" -split '[:;]')) { if ($p) { [void]$roots.Add($p) } } }
  }
  foreach ($p in @('/usr/lib', '/usr/lib64', '/lib', '/lib64', '/usr/local/lib',
                   '/usr/lib/x86_64-linux-gnu', '/usr/lib/aarch64-linux-gnu',
                   '/usr/local/opt/tcl-tk/lib', '/opt/homebrew/lib', '/opt/local/lib')) {
    [void]$roots.Add($p)
  }
  return $roots
}
# First DECLARED name present under any candidate root. ROOT-major: the path order
# a catalogue declares is a preference order (an operator's ${env:DSS_PE_LIBDIR}
# is listed ahead of the git-for-Windows tree precisely so it wins), and the name
# order breaks ties within one directory.
function Find-DeclaredLib($names, $roots) {
  foreach ($r in $roots) {
    if (-not $r) { continue }
    foreach ($n in $names) {
      if (-not $n) { continue }
      $c = Join-Path $r $n
      if (Test-Path -LiteralPath $c -PathType Leaf) { return (Resolve-Path -LiteralPath $c).Path }
    }
  }
  return ''
}
# ── ONE call into the shared resolver ────────────────────────────────────────
# The PowerShell native-command traps this file has already paid for, handled in
# ONE place instead of being rediscovered at every new call site:
#   · the rc is taken DIRECTLY off the call ($LASTEXITCODE on the very next
#     statement, NEVER after a pipe — a pipe reports the PIPE's status);
#   · `2>&1` wraps the callee's stderr in ErrorRecords, so STDOUT (the JSON, the
#     argv tokens) must be separated from it — while the FULL text, stderr
#     included, is kept, because a refusal's entire value is its diagnostic and
#     that diagnostic has to survive into the leg's Detail;
#   · PowerShell 7.3+ can make a nonzero-exiting native command THROW while
#     $ErrorActionPreference is 'Stop', which would abort the whole run with a
#     stack trace instead of failing the ONE leg that asked for this.
# Every caller decides for itself what a non-zero rc means for ITS leg; this
# function never dies, because the whole point is per-leg isolation.
function Invoke-LegResolver([string[]]$callArgs) {
  try {
    $out = @(& $python3.Source $LegsPy @callArgs 2>&1)
    $rc  = $LASTEXITCODE
  } catch {
    $rc  = if ($LASTEXITCODE) { $LASTEXITCODE } else { 1 }
    $out = @("$($_.Exception.Message)")
  }
  return @{
    Rc     = $rc
    Stdout = @($out | Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] } |
                      ForEach-Object { "$_".TrimEnd() })
    Text   = (@($out | ForEach-Object { "$_".TrimEnd() }) -join "`n")
  }
}

# Resolve ONE leg's declared (tcl, z) pair. Returns @{ Ok; Tcl; Z; Detail } — plus,
# for a provider that ACQUIRED its libraries, `Acquired` (the resolver's own
# per-library records, which Step 7 stages beside the artefact).
# Ok=$false is ALWAYS `skipped-build-input-missing` with a Detail that names the
# provider, every candidate NAME and every candidate PATH — a reader must be able
# to act on it without reading this script.
function Resolve-LegLibraries($leg) {
  $libs     = $leg.build.libraries
  $provider = "$($libs.provider)"
  $tclNames = @($libs.tclNames)
  $zNames   = @($libs.zNames)
  switch ($provider) {
    'search-paths' {
      # $env:TCL_DLL / $env:ZLIB_DLL — the operator's EXPLICIT override, preserved
      # verbatim for the provider the pe64 leg declares. An override that points at
      # nothing is a HARD error, not a skip: stated intent that cannot be honoured
      # must never be quietly downgraded (this is the one pre-TF-C114 Die shape that
      # is kept exactly as it was).
      $tcl = ''; $z = ''
      if ($env:TCL_DLL) {
        if (-not (Test-Path -LiteralPath $env:TCL_DLL -PathType Leaf)) { Die "`$env:TCL_DLL='$($env:TCL_DLL)' does not name a file — fix it or unset it (leg '$($leg.label)')." }
        $tcl = (Resolve-Path -LiteralPath $env:TCL_DLL).Path
      }
      if ($env:ZLIB_DLL) {
        if (-not (Test-Path -LiteralPath $env:ZLIB_DLL -PathType Leaf)) { Die "`$env:ZLIB_DLL='$($env:ZLIB_DLL)' does not name a file — fix it or unset it (leg '$($leg.label)')." }
        $z = (Resolve-Path -LiteralPath $env:ZLIB_DLL).Path
      }
      # The resolver already expanded every ${env:…} and DROPPED the candidates
      # whose variable is unset, so what arrives here is real directories only.
      $roots = @($libs.searchPaths) | Select-Object -Unique
      if (-not $tcl) { $tcl = Find-DeclaredLib $tclNames $roots }
      if (-not $z)   { $z   = Find-DeclaredLib $zNames   $roots }
      if ($tcl -and $z) { return @{ Ok = $true; Tcl = $tcl; Z = $z; Detail = "provider 'search-paths'" } }
      $missing = @(); if (-not $tcl) { $missing += "tcl ($($tclNames -join ' | '))" }; if (-not $z) { $missing += "z ($($zNames -join ' | '))" }
      return @{ Ok = $false; Tcl = $tcl; Z = $z; Detail = "provider 'search-paths' found no $($missing -join ' and no ') under any declared search path [$($roots -join ' ; ')] — install them, add a path via `$env:DSS_PE_LIBDIR, or point `$env:TCL_DLL/`$env:ZLIB_DLL straight at them" }
    }
    'host-system' {
      $roots = @(Get-HostSystemLibRoots)
      $tcl = Find-DeclaredLib $tclNames $roots
      $z   = Find-DeclaredLib $zNames   $roots
      if ($tcl -and $z) { return @{ Ok = $true; Tcl = $tcl; Z = $z; Detail = "provider 'host-system'" } }
      $missing = @(); if (-not $tcl) { $missing += "tcl ($($tclNames -join ' | '))" }; if (-not $z) { $missing += "z ($($zNames -join ' | '))" }
      return @{ Ok = $false; Tcl = $tcl; Z = $z; Detail = "provider 'host-system' found no $($missing -join ' and no ') under any candidate root [$($roots -join ' ; ')] — this machine has no copy of that target's tcl/zlib runtime; put one anywhere and name the directory in `$env:DSS_HOST_LIBDIR" }
    }
    'pinned-archive' {
      # ★ THE LEG'S DECLARED ROUTE, PERFORMED BY THE RESOLVER — never by this
      # driver. `--acquire` downloads the archives legs.json PINS, verifies the
      # declared sha256, extracts the declared members (following an
      # intra-archive symlink), slices a Mach-O universal archive to THIS LEG's
      # target arch, and materialises the result in a cache OUTSIDE the repo.
      # This driver does not know what HTTP, bzip2 or a fat header are, and that
      # is the entire point: acquisition implemented once, in the file both
      # drivers already hard-require, cannot exist in one driver and not the
      # other (D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER).
      #
      # NOT host-keyed and NOT conditional: every host runs this for a leg that
      # declares it. A machine with no network and a cold cache gets the
      # resolver's REFUSAL — which is a `skipped-build-input-missing` for THIS
      # leg with the resolver's own diagnostic, never a fallback to "whatever
      # tcl is on this box" (that fallback would build the leg against a foreign
      # library and report it green).
      Info "[$($leg.label)] provider 'pinned-archive' — acquiring this leg's DECLARED libraries via harness_legs.py --acquire (cached after the first run; a cold cache downloads)"
      $acqRun = Invoke-LegResolver @('--acquire', "$($leg.label)")
      if ($acqRun.Rc -ne 0) {
        return @{ Ok = $false; Tcl = ''; Z = ''; Detail = "provider 'pinned-archive': harness_legs.py --acquire $($leg.label) FAILED (rc=$($acqRun.Rc)). The declared route could not be completed and nothing was improvised in its place:`n$(($acqRun.Text -split "`n" | ForEach-Object { "        $_" }) -join "`n")" }
      }
      $acq = $null
      try { $acq = ($acqRun.Stdout -join "`n") | ConvertFrom-Json } catch {
        return @{ Ok = $false; Tcl = ''; Z = ''; Detail = "provider 'pinned-archive': harness_legs.py --acquire $($leg.label) exited 0 but did not print the JSON report this driver reads ($($_.Exception.Message)). Output was:`n$(($acqRun.Text -split "`n" | Select-Object -First 20 | ForEach-Object { "        $_" }) -join "`n")" }
      }
      $cdir = "$($acq.cacheDir)"
      if (-not $cdir) { return @{ Ok = $false; Tcl = ''; Z = ''; Detail = "provider 'pinned-archive': the acquisition report for $($leg.label) carries no cacheDir, so there is no directory to resolve this leg's libraries out of." } }
      # ★ THE SUBSTITUTION IS STATED, NEVER HIDDEN. An acquired library is a
      # STAND-IN: we read its export surface here, and the target machine loads
      # its OWN copy. Its embedded identity is the PACKAGER's (MEASURED: the
      # MacPorts dylibs say `/opt/local/lib/…`), so the leg DECLARES the identity
      # to record instead. A build log that did not print the displacement could
      # not show it actually happened.
      #
      # `remediated` is printed FIRST and unconditionally: it is the resolver
      # saying a cached file was absent, or did not hash to what it hashed to when
      # it was extracted, and was restored from the digest-verified archive. Not
      # an error — but a cache that repaired itself silently is exactly the kind
      # of fact this harness refuses to keep to itself.
      foreach ($rem in @($acq.remediated | Where-Object { $_ })) {
        Warn "[$($leg.label)] cache REMEDIATED — $rem (restored from the pinned, digest-verified archive)"
      }
      foreach ($libRec in @($acq.libraries | Where-Object { $_ })) {
        $displaced = if ("$($libRec.embeddedIdentity)") { "displacing the packager's own '$($libRec.embeddedIdentity)'" } else { 'the file carries no embedded identity' }
        # BOTH digests, by the names the resolver reports them under: the ARCHIVE's
        # (what the catalogue pins) and the extracted FILE's (what the compiler is
        # actually handed). A missing one is SAID, never printed as a blank — a
        # digest field that quietly went empty is how a report stops meaning
        # anything while still looking like one.
        $aSha = if ("$($libRec.archiveSha256)") { "archive sha256 $($libRec.archiveSha256)" } else { 'archive sha256 <not reported by this resolver>' }
        $fSha = if ("$($libRec.fileSha256)")    { "file sha256 $($libRec.fileSha256)" }       else { 'file sha256 <not reported by this resolver>' }
        Info "[$($leg.label)] acquired $($libRec.as) -> $($libRec.path)"
        Info "[$($leg.label)]   recorded as '$($libRec.importName)' ($displaced)"
        Info "[$($leg.label)]   $aSha; $fSha; from $($libRec.sourceUrl)"
      }
      # Resolved out of the ACQUIRED cache by the leg's OWN declared names — the
      # same Find-DeclaredLib the other providers use, so "which file is the tcl
      # one" is answered identically everywhere.
      $tcl = Find-DeclaredLib $tclNames @($cdir)
      $z   = Find-DeclaredLib $zNames   @($cdir)
      # ── THE SCRIPT LIBRARY THE ACQUIRED Tcl CANNOT RUN WITHOUT ──────────────
      # [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY] - see the
      # $AcqScriptLibraryKey note at the top of this file for the measurement.
      # ⚠ ABSENT IS REPORTED, NEVER ASSUMED BENIGN. It does not fail the build and
      # does not fail a directly-named .test file - the MAIN interpreter is already
      # initialised by then, so `tclInit` is never re-entered. It kills the TIER,
      # and only the tier. That asymmetry is why this was invisible until a tier
      # ran, and why the absence is said out loud HERE rather than discovered as an
      # unnamed abort 11 resumes later.
      $tclScriptDir = Get-AcquiredScriptLibrary $acq $AcqScriptLibraryKey
      if ($tclScriptDir) {
        Info "[$($leg.label)] Tcl script library (acquired): $tclScriptDir - TCL_LIBRARY is pointed here for THIS leg only"
      } else {
        Warn "[$($leg.label)] the acquisition report stages NO Tcl script library (report field '$AcqScriptLibraryKey' is empty)."
        Warn "      An acquired libtcl bakes in ITS PACKAGER'S script-library path, which does not exist on this"
        Warn "      machine. Individual .test files will still run; the TIER will abort at the first ``interp create``"
        Warn "      with `"Can't find a usable init.tcl`" and NO unit will get a verdict."
        Warn "      D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY - the fix belongs in the leg's"
        Warn "      ``pinned-archive`` declaration + harness_legs.py, not here."
      }
      if ($tcl -and $z) {
        return @{ Ok = $true; Tcl = $tcl; Z = $z; Acquired = @($acq.libraries | Where-Object { $_ })
                  TclScriptDir = $tclScriptDir
                  Detail = "provider 'pinned-archive' — $(@($acq.libraries | Where-Object { $_ }).Count) declared library(ies) materialised for target arch $($acq.targetArch) in $cdir ($(if ($acq.fromCache) { 'from cache, no network' } else { 'freshly downloaded + checksum-verified' }))$(if ($tclScriptDir) { "; Tcl script library staged at $tclScriptDir" })" }
      }
      $missing = @(); if (-not $tcl) { $missing += "tcl ($($tclNames -join ' | '))" }; if (-not $z) { $missing += "z ($($zNames -join ' | '))" }
      # ★ `Acquired` RIDES ON THE FAILURE RETURN TOO, and it has to. Ok=$false
      # here means "the FIXTURE cannot be built for this leg" — but the CLI needs
      # only zlib, so Step 7b legitimately proceeds on a result whose .Z is set
      # and whose .Tcl is not. That build records `@loader_path/<name>` for the
      # acquired zlib, a claim made true ONLY by the copy Step 7b stages beside
      # the artefact, and that copy is driven off this very field. Dropping it on
      # the failure path would produce a binary that links clean here and fails in
      # the target's loader — the one failure this host cannot observe.
      return @{ Ok = $false; Tcl = $tcl; Z = $z; Acquired = @($acq.libraries | Where-Object { $_ })
                TclScriptDir = $tclScriptDir
                Detail = "provider 'pinned-archive' acquired $(@($acq.libraries | Where-Object { $_ }).Count) file(s) into $cdir but none of them is a declared $($missing -join ' and none is a declared ') — the leg's acquire members and its tclNames/zNames disagree in legs.json (the 'as' name a member materialises under MUST be one this leg's own name list can resolve)." }
    }
    default {
      # A provider this driver has NO dispatch arm for. Named out loud rather than
      # crashed on or silently passed: the leg is declared, it is real, and the gap
      # is THIS DRIVER's.
      #
      # ★ WHAT THIS ARM NO LONGER MEANS. It used to read "…is NOT IMPLEMENTED by
      # build-and-test.ps1 … build-and-test.sh is where acquisition lives", which
      # made a working provider Linux-driver-only. That is over: ACQUISITION IS NOT
      # DRIVER-LOCAL ANY MORE. `pinned-archive` is performed by the SHARED resolver
      # (`harness_legs.py --acquire`) and is dispatched right here, on every host —
      # which is how a Windows box acquires Darwin dylibs for the macho legs.
      #
      # ★ EXACTLY ONE DECLARED PROVIDER STILL REACHES THIS ARM, BY NAME:
      # ubuntu-ports-arm64, the bespoke ancestor of `pinned-archive`.
      # build-and-test.sh performs it inline with curl + dpkg-deb, and generalising
      # it into the shared resolver needs a .deb reader — MEASURED 2026-08-04, not
      # safely doable yet: a modern .deb's inner archive can be zstd, whose stdlib
      # module arrived only in Python 3.14 (this Windows host has 3.14, the WSL leg
      # has 3.12), so that route would be a capability that exists on one HOST and
      # not another — the same defect one level down. It is therefore named here,
      # with its anchor, instead of quietly passing, and
      # tests/harness/test_sqlite_harness_legs.cpp carries the self-retiring
      # exemption that reds the day it IS implemented in both drivers.
      # D-HARNESS-UBUNTU-PORTS-PROVIDER-NOT-GENERALISED-TO-PINNED-ARCHIVE
      return @{ Ok = $false; Tcl = ''; Z = ''; Detail = "library provider `"$provider`" has no dispatch arm in build-and-test.ps1, so this driver cannot obtain this leg's declared inputs (tcl: $($tclNames -join ' | ') / z: $($zNames -join ' | ')). ACQUISITION ITSELF IS NOT DRIVER-LOCAL — pinned-archive is performed HERE, by harness_legs.py --acquire, on every host. Exactly one DECLARED provider still lands in this arm: ubuntu-ports-arm64, whose .deb route build-and-test.sh performs inline with curl + dpkg-deb and which is not generalised into the shared resolver yet (a .deb's inner archive can be zstd, whose stdlib module arrived only in Python 3.14 — a route that works on one HOST and not another is the same defect one level down). D-HARNESS-UBUNTU-PORTS-PROVIDER-NOT-GENERALISED-TO-PINNED-ARCHIVE. If the provider named above is a DIFFERENT one, the catalogue (or LIBRARY_PROVIDERS in harness_legs.py) grew a provider and this driver was not extended in the same change — add the arm to BOTH drivers, because a capability in one driver and not the other is this project's canonical silent harness bug." }
    }
  }
}

# ── The argv that hands this leg's two resolved libraries to the build ───────
# ★ THE COMPILER'S FLAG IS NOT SPELLED IN THIS FILE, AND MUST NOT BE — the same
# argument `--translate-path` makes for `wslpath`. It is named ONCE, in
# harness_legs.py, and both drivers ask that file to build the argv. A driver
# that spelled it itself would be a capability that can exist in one driver and
# not the other, which is precisely
# D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER.
#
# ★ WHY THIS MATTERS BEYOND TIDINESS — THE `importName` OVERRIDE. An ACQUIRED
# library is a STAND-IN: this host reads its export surface, the target machine
# loads its OWN copy. Its embedded identity (Mach-O LC_ID_DYLIB / ELF DT_SONAME /
# PE export DllName) is therefore a fact about whoever PACKAGED it — MEASURED
# 2026-08-04: the MacPorts dylibs say `/opt/local/lib/…`, and a Mach-O cross-built
# against them records `LC_LOAD_DYLIB=/opt/local/lib/libtcl8.6.dylib`, demanding
# MacPorts on the target Mac. That is a dyld LOAD failure: not a build error, and
# NOT observable on this host. So a leg that needs a different runtime identity
# DECLARES it (plan `libraries.tclImportName` / `zImportName`, "" everywhere the
# library already carries the right one) and the resolver — which also PROBES the
# compiler for the capability — decides how to say it.
#
# ★ REFUSAL, NEVER A DROPPED OVERRIDE. If the resolver exits non-zero (this
# compiler cannot record the identity), the leg is POISONED. Building anyway
# would produce an artefact that links clean here and dies at load time on a
# machine this run cannot observe — the one failure mode nothing downstream
# would catch.
#
# ★ WHERE THESE TOKENS ACTUALLY GO, and why that is not a mismatch: this driver
# builds through the `--project` MANIFEST, so the argv is handed to
# gen-pe64-manifest.py, not straight to the compiler. That generator's
# `--resolve-library PATH[=IMPORT_NAME]` deliberately mirrors the DSS CLI's own
# flag and value grammar (gen-pe64-manifest.py `resolve_library_entry`), turning
# a bare PATH into the plain string entry it has always emitted and a
# `PATH=IMPORT_NAME` into the extended `{"path", "importName"}` object that
# src/program/project_config.cpp reads. So the SAME tokens carry the override all
# the way into `resolveLibraries` with no manifest-schema change of any kind.
#
# Returns @{ Ok; Tokens; Detail }. Never dies: a failure belongs to ONE leg.
#
# ★ $Only SELECTS WHICH LIBRARIES, and it exists because the two artifacts this
# harness builds have genuinely different link lines. The testfixture links BOTH
# (Tcl is what it IS); the sqlite3 CLI links ZLIB ONLY — shell.c has no Tcl in it
# (✔MEASURED 2026-08-05: zero `tcl.h` / `Tcl_` references in the generated
# shell.c), and declaring an unused library would record a runtime dependency
# (DT_NEEDED / LC_LOAD_DYLIB / PE import) that makes the binary refuse to load on
# a machine without Tcl, for nothing. Defaults to both, so every existing call
# site is unchanged.
function Get-ResolveLibraryArgv($leg, $legLibs, $Only = @('tcl','z')) {
  $libs   = $leg.build.libraries
  $tokens = @()
  $notes  = @()
  foreach ($w in @(
      @{ What = 'tcl'; Path = "$($legLibs.Tcl)"; Import = "$($libs.tclImportName)" },
      @{ What = 'z';   Path = "$($legLibs.Z)";   Import = "$($libs.zImportName)" } |
        Where-Object { @($Only) -contains $_.What })) {
    $call = @('--resolve-library-argv', "$($w.Path)")
    # --dss is passed exactly when there is an identity to record: that is the
    # only case the resolver probes the compiler for, and a leg needing no
    # override must keep paying nothing for one.
    if ($w.Import) { $call += @('--import-name', "$($w.Import)", '--dss', "$DssBin") }
    $res = Invoke-LegResolver $call
    if ($res.Rc -ne 0) {
      return @{ Ok = $false; Tokens = @(); Detail = "the resolver REFUSED to build the library argv for this leg's $($w.What) library (rc=$($res.Rc); path $($w.Path)$(if ($w.Import) { "; declared runtime identity '$($w.Import)'" })). Dropping the override and building anyway is not an option — the artefact would link clean here and fail at LOAD time on the target machine:`n$(($res.Text -split "`n" | ForEach-Object { "        $_" }) -join "`n")" }
    }
    $toks = @($res.Stdout)
    # ONE TOKEN PER LINE is the contract, and it is why the output is never
    # re-split on whitespace here: a token legitimately contains '=' and a path
    # legitimately contains spaces.
    if ($toks.Count -lt 2 -or @($toks | Where-Object { -not $_ }).Count -gt 0) {
      return @{ Ok = $false; Tokens = @(); Detail = "the resolver exited 0 for this leg's $($w.What) library but printed $($toks.Count) usable token(s) (expected at least the flag and its value, one per line). Refusing to build with an argv this driver cannot account for. Output was:`n$(($res.Text -split "`n" | Select-Object -First 10 | ForEach-Object { "        $_" }) -join "`n")" }
    }
    $tokens += $toks
    if ($w.Import) { $notes += "$($w.What) recorded as '$($w.Import)' (the leg's DECLARED runtime identity, displacing the acquired file's own)" }
  }
  # A filter that matched NOTHING must not read as "no libraries needed": that is
  # the inert-instrument shape, and it would silently produce a manifest with an
  # empty resolveLibraries that fails much later at link time.
  if (-not $tokens.Count) {
    return @{ Ok = $false; Tokens = @(); Detail = "no library matched the requested set '$(@($Only) -join ",")' — this leg would be built with NO resolved libraries at all, which cannot be what was meant." }
  }
  return @{ Ok = $true; Tokens = $tokens; Detail = $(if ($notes.Count) { $notes -join '; ' } else { 'no runtime-identity override declared — each library is recorded under its own embedded identity' }) }
}

# ── Can this driver express THIS leg's manifest correctly? ───────────────────
# ONE thing is keyed on the leg's DECLARED recipeTransform:
#
#   THE GENERATOR. gen-pe64-manifest.py once applied the windows self-config
#   transform (drop the deriving host's HAVE_/Z_HAVE_/_HAVE_SQLITE_CONFIG_H
#   probes, add SQLITE_OS_WIN=1) and an 8 MiB stackReserve UNCONDITIONALLY.
#   It has since grown --recipe-transform/--stack-reserve, so both are now the
#   LEG's to declare. This still PROBES for those arguments rather than
#   assuming them: the two files land in different cycles, and a driver that
#   assumed the newer generator would silently apply the WINDOWS transform to
#   a non-Windows target — a cross-compile category error — against the older
#   one. The probe makes the blocker appear and disappear on its own.
#
# ★ THE SECOND BLOCKER IS GONE, AND THAT IS THE POINT OF TF-C115. It read: "the
# SHARED staged zconf.h has Z_HAVE_UNISTD_H forced to '#if 0' for the pe target
# and there is ONE zinc/ for all legs", so every non-`windows-selfconfig` leg was
# refused. Step 3+4 now stages one zinc/ PER recipeTransform (stage-zinc.py) and
# each leg's manifest gets its OWN include list, so there is nothing left to
# refuse. The blocker was NOT relaxed to get a green — the need for it was
# removed. The ABI question it flagged as UNMEASURED is now MEASURED, and the
# refusal was right: on darwin the guard flips z_off_t between `long long` and
# `long` (same width, different type), and on pe the un-flipped header fails
# loud with `error[F001D] got unistd.h`. Only on Linux LP64 was it benign.
#
# A leg that trips the remaining blocker — or whose header stage could not be
# produced at all — records `poisoned` WITH THE REASON. It is not built wrong and
# reported green, and it is not quietly dropped.
function Get-GenCapabilities {
  $h = ''
  try { $h = (& $python3.Source $GenPy '--help' 2>&1) -join "`n" } catch { $h = '' }
  return @{
    RecipeTransform = ($h -match '--recipe-transform')
    StackReserve    = ($h -match '--stack-reserve')
  }
}
function Test-LegManifestBlockers($leg, $gen) {
  $blockers = @()
  $t = "$($leg.build.recipeTransform)"
  if ($t -ne 'windows-selfconfig') {
    if (-not ($gen.RecipeTransform -and $gen.StackReserve)) {
      $have = @(); if ($gen.RecipeTransform) { $have += '--recipe-transform' }; if ($gen.StackReserve) { $have += '--stack-reserve' }
      $blockers += "this leg declares recipeTransform='$t' + stackReserveBytes=$($leg.build.stackReserveBytes), but $([System.IO.Path]::GetFileName($GenPy)) applies 'windows-selfconfig' + an 8 MiB reserve UNCONDITIONALLY and exposes $(if ($have.Count) { "only $($have -join ' ')" } else { 'neither --recipe-transform nor --stack-reserve' }). Generating this leg's manifest with the generator as it stands would silently apply the WINDOWS transform to a non-Windows target — a cross-compile category error, so it is refused"
    }
  }
  # The leg's OWN staged headers, BOTH of them: zinc/<recipeTransform>/ (zlib) and
  # cfg/<targetOs>/ (sqlite's ./configure output). Absent = stage-zinc.py said so,
  # loudly, in Step 3+4. There is NO fallback to a sibling stage's copy or to the
  # deriving host's own: those fallbacks ARE the defect.
  if (-not $LegIncludes.ContainsKey($leg.label)) {
    $blockers += "this leg has no include list: its staged zlib header dir (zinc/$($leg.build.headerStageKey)/, from its declared zconfGuards) and/or its staged sqlite config dir (cfg/$($leg.build.configStageKey)/, from its declared configureAnswers) was NOT produced. See the ZINC-STAGE-FAIL / CFG-STAGE-FAIL line in Step 3+4. Compiling it against another stage's zinc/, or against the DERIVING host's sqlite_cfg.h, is exactly D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED / D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES and is refused"
  }
  return $blockers
}
$GenCaps = Get-GenCapabilities
Info "manifest generator: $([System.IO.Path]::GetFileName($GenPy))  --recipe-transform:$(if ($GenCaps.RecipeTransform) { 'YES' } else { 'no' })  --stack-reserve:$(if ($GenCaps.StackReserve) { 'YES' } else { 'no' })"

# ── the ledger: EVERY declared leg gets exactly one row, from here on ────────
# Same shape as ArmVerdictRecord (example/spec/arm/verdict/detail); Step 9 renders
# it in ArmVerdictLedger::renderCountsLine()'s idiom. A leg that never reaches a
# later step keeps the verdict written here, so no leg can end the run unnamed.
$LegLedger = @{}
$LegOrder  = @()

# ── THE CLOSED VERDICT VOCABULARY, READ FROM THE SHARED RESOLVER ─────────────
# ★ NOT SPELLED HERE. A driver-local copy of a closed vocabulary is how the two
# drivers drift, which is this file family's standing defect class
# (D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER). The .sh reads
# the same list from the same call.
$VerdictVocabulary = @()
$vocabOut = & $python3.Source $LegsPy '--verdict-vocabulary' 2>&1
$vocabRc  = $LASTEXITCODE
if ($vocabRc -eq 0) {
  # ⚠ `2>&1` merges STDERR into the pipeline, which is what makes the refusal below
  # able to QUOTE the diagnostic - but on SUCCESS a stderr line would otherwise be
  # accepted as a verdict token. ErrorRecords are dropped for that reason and that
  # reason only: the vocabulary is what the resolver printed on STDOUT.
  $VerdictVocabulary = @(@($vocabOut) |
      Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] } |
      ForEach-Object { "$_".Trim() } | Where-Object { $_ })
}
if ($VerdictVocabulary.Count -eq 0) {
  Die "the leg resolver could not state the CLOSED verdict vocabulary (rc=$vocabRc): [$vocabOut]. Without it this driver cannot tell a classified skip from an unclassified one, and an unclassified skip is precisely how a leg's entire corpus vanishes from the ledger while the summary still reads as full coverage. Refusing to run rather than guess the list."
}
# Legs whose non-verification could not be CLASSIFIED. Counted, named, and folded
# into the exit code at Step 9 — a harness defect that only warns is one that ships.
$UnclassifiedVerdicts = New-Object 'System.Collections.Generic.List[string]'

# ★ AN EMPTY OR UNKNOWN VERDICT TOKEN IS NOW IMPOSSIBLE BY CONSTRUCTION, NOT BY
# REVIEW (D-HARNESS-UNITS-SKIP-A-LEG-WHOSE-LAUNCHER-IT-SAYS-IS-AVAILABLE).
# ✔MEASURED on the operator's Mac at 11e97e0e, in this driver's .sh sibling:
# `macho64-x86_64 …: compiled   units: not run []` — a whole leg's corpus skipped
# with NO class, in the same sentence that said its declared launcher was present.
# A not-run carrying no class cannot be counted as structural / environmental /
# harness, so a leg can vanish from the accounting while the summary still LOOKS
# complete. Every verdict this driver writes goes through this ONE function, so
# there is no second place a fifth unguarded assignment can be added.
function Set-LegVerdict($label, $verdict, $detail) {
  if (-not $script:LegLedger.ContainsKey($label)) { $script:LegLedger[$label] = @{ Label = $label; Spec = ''; Verdict = ''; Detail = ''; Built = $false; UnitVerdict = ''; UnitFail = $false } }
  $tok = "$verdict".Trim()
  if (-not $tok -or ($script:VerdictVocabulary -notcontains $tok)) {
    $why = if (-not $tok) { "this driver recorded a verdict with an EMPTY token" }
           else { "this driver recorded the verdict token '$tok', which is OUTSIDE the closed vocabulary ($($script:VerdictVocabulary -join ' '))" }
    [void]$script:UnclassifiedVerdicts.Add($label)
    Warn "[$label] HARNESS DEFECT — $why."
    Warn "      what it did say: $(if ($detail) { $detail } else { '<no reason recorded>' })"
    # `poisoned` — the vocabulary's name for "no artifact was exercised and the
    # reason is OURS". It keeps the leg INSIDE the Step-9 accounting (so it can
    # never also become a ledger hole) and keeps the run from exiting 0, while
    # the run itself CONTINUES to every other leg: the harness must survive its
    # own defects, not hide them.
    $verdict = 'poisoned'
    $detail  = "HARNESS DEFECT: $why. $(if ($detail) { $detail } else { '<no reason recorded>' })"
  }
  $script:LegLedger[$label].Verdict = $verdict
  $script:LegLedger[$label].Detail  = $detail
}
# The UNIT-level twin: the `units: …` string a reader sees per leg. Same guard,
# because the leg verdict and the unit verdict are two different sentences and the
# measured defect was in the SECOND one.
function Set-UnitNotRun($label, $token, $detail) {
  if (-not $script:LegLedger.ContainsKey($label)) { $script:LegLedger[$label] = @{ Label = $label; Spec = ''; Verdict = ''; Detail = ''; Built = $false; UnitVerdict = ''; UnitFail = $false } }
  $tok = "$token".Trim()
  $txt = if ($detail) { "$detail" } else { '<no reason recorded>' }
  if (-not $tok -or ($script:VerdictVocabulary -notcontains $tok)) {
    [void]$script:UnclassifiedVerdicts.Add($label)
    Warn "[$label] HARNESS DEFECT — this leg's ENTIRE unit corpus did not run and the token recorded for it ('$tok') is not one this run can classify."
    Warn "      what it did say: $txt"
    $script:LegLedger[$label].UnitVerdict = "not run [poisoned] — HARNESS DEFECT: unclassified skip token '$tok'. $txt"
    return
  }
  $script:LegLedger[$label].UnitVerdict = "not run [$tok] — $txt"
}
# ── THE SINGLE RUN-DECISION, SHARED BY BOTH ARTIFACTS ────────────────────────
# The sqlite3 CLI smoke gate and the unit corpus ask the SAME question — "may this
# host EXECUTE this leg?" — and the answer is `run.mode` off the RESOLVED plan,
# never `$IsWindows`. A FUNCTION, so the two call sites cannot drift into two
# different answers.
# ⚠ NOTE, because the obvious reading of that anchor is WRONG: the two call sites
# had ALREADY agreed here, in BOTH drivers. What differed was a SECOND, unrelated
# gate in the .sh's units path (an absent CONTROL compiler) that this driver never
# had — see the note at the same place in build-and-test.sh.
function Test-LegRunSkipped($leg) {
  switch ("$($leg.run.mode)") {
    'skip'   { return $true }
    'native' { return $false }
    'launched' {
      # A `launched` leg with no launcher argv is the same contradiction wearing
      # its other face: the plan says "runnable" and hands the driver nothing to
      # run it with. The resolver already refuses an EMPTY declared command; this
      # asserts the invariant survived transport through the JSON plan.
      if (-not @($leg.run.launcher).Count) {
        Die "[$($leg.label)] the resolved plan says run mode 'launched' but carries an EMPTY launcher argv. A leg cannot be both runnable and unrunnable — that is a transport defect between harness_legs.py and this driver, not a property of this machine."
      }
      return $false
    }
    default { Die "[$($leg.label)] has an unknown run mode '$($leg.run.mode)' — the resolver and this driver disagree about the vocabulary." }
  }
}
foreach ($lg in $AllLegs) {
  $LegOrder += $lg.label
  $LegLedger[$lg.label] = @{ Label = $lg.label; Spec = $lg.spec; Verdict = ''; Detail = ''; Built = $false; UnitVerdict = ''; UnitFail = $false }
}
# A leg the operator filtered out is NOT verified and must not read as one. It is
# a HARNESS-class non-verification — the runner did not select it — which is the
# meaning `not-selected-by-runner` already carries in the closed vocabulary.
foreach ($lbl in $FilteredOut) {
  Set-LegVerdict $lbl 'not-selected-by-runner' "removed from this run by DSS_LEGS='$($env:DSS_LEGS)' — DECLARED coverage that was not exercised"
}

# ── WHAT EACH LAUNCHER NEEDS BEYOND ITS OWN argv[0] ─────────────────────────
# >>> dss:launcher-prereq >>>
# ★★ THE PLAN SAYS `launched` BECAUSE argv[0] RESOLVED, AND THAT IS A MUCH WEAKER
# FACT THAN IT READS AS. On THIS host the arm64 leg's argv[0] is `wsl.exe` —
# present on every machine with WSL — while the program that actually executes the
# artefact is `qemu-aarch64` INSIDE the distro, which `shutil.which('wsl.exe')` has
# never asked about. ✔MEASURED: that leg passed every gate this harness had on a
# box with no qemu, every unit exited 255 with NO diagnostic, and fourteen of them
# were charged to DSS — the harness accusing the compiler of a defect in the
# machine it was running on.
#
# `--check-launcher` EXECUTES the leg's DECLARED prerequisite rows (legs.json
# `launchers[].requires`) in the LAUNCHER's own namespace and answers rc 0 met /
# 3 unmet / 2 catalogue defect. This driver only classifies the answer; it never
# decides what a launcher needs and never probes anything itself.
#
# ★ THE OUTCOME IS `skipped-launcher-prerequisite-missing`, the closed
# vocabulary's ENVIRONMENTAL sibling of `skipped-emulator-missing` — announced by
# default, FATAL under DSS_STRICT_ARM_VERDICTS=1 through the SAME Step-9
# $envSkipLegs list, and STILL BUILT. It is only ever about EXECUTION here.
#
# ⓘ WHY HERE AND NOT AT STEP 1b. It runs at the EARLIEST point this driver can
# record a verdict at all — `Set-LegVerdict` and the closed vocabulary it validates
# against are defined immediately above — and that is still before Steps 7/7b/7c/8,
# which is what the gate has to precede. The .sh twin's structural equivalent is
# Step 1, because there the verdict arrays exist from the plan onwards.
# `--artifact` (the 4-D PT_INTERP/DT_NEEDED cross-check) is NOT passed: nothing is
# built yet, and asking for it here would mean lying about the artefact.
function Get-LauncherPrereqRows($report) {
  # THE ROWS THE CATALOGUE DECLARED, not a summary of them. `provides` says what
  # the missing thing is FOR, `why` says on what evidence it is declared, and
  # `install` is the one line the operator actually needs — a diagnostic without
  # the remedy is one nobody acts on.
  $out = New-Object 'System.Collections.Generic.List[string]'
  foreach ($row in @($report.missing)) {
    if (-not $row) { continue }
    [void]$out.Add("MISSING [$(if ($row.kind) { $row.kind } else { '?' })] $(if ($row.path) { $row.path } else { '?' })")
    [void]$out.Add("      provides: $(if ($row.provides) { $row.provides } else { '<not declared>' })")
    [void]$out.Add("      why     : $(if ($row.why)      { $row.why }      else { '<not declared>' })")
    [void]$out.Add("      install : $(if ($row.install)  { $row.install }  else { '<not declared>' })")
    if (@($row.probe).Count) { [void]$out.Add("      probed  : $(@($row.probe) -join ' ')") }
  }
  foreach ($u in @($report.uncovered)) { if ($u) { [void]$out.Add("UNCOVERED $u") } }
  return $out
}
function Test-LauncherPrereq($leg) {
  # -> 'not-launched' | 'met' | 'unmet' | 'unreadable'. The verdict is RECORDED
  # here, through the one guarded recorder, so there is no second place a fifth
  # unguarded assignment can appear.
  $lbl = $leg.label
  # A leg this host runs NATIVELY has no launcher, and a leg the plan already
  # skips has already been named. Neither is this gate's business.
  if ("$($leg.run.mode)" -ne 'launched') { return 'not-launched' }
  $declared = (@($leg.run.launcher) -join ' ')
  $out = @(); $rc = 0
  try {
    # rc taken DIRECTLY off the call — $LASTEXITCODE on the very next statement,
    # never after a pipe. The try/catch is not decoration: PowerShell 7.3+ can make
    # a nonzero-exiting native command THROW under $ErrorActionPreference='Stop'.
    $out = @(& $python3.Source $LegsPy '--check-launcher' $lbl '--host-os' $HostOs '--host-arch' $HostArch 2>&1)
    $rc  = $LASTEXITCODE
  } catch {
    $rc  = if ($LASTEXITCODE) { $LASTEXITCODE } else { 1 }
    $out = @("$_")
  }
  $text = (@($out) | ForEach-Object { "$_" }) -join "`n"
  if ($rc -eq 0) {
    Info "[$lbl] launcher '$declared': every DECLARED prerequisite is present on this machine"
    return 'met'
  }
  $report = $null
  if ($rc -eq 3) { try { $report = $text | ConvertFrom-Json } catch { $report = $null } }
  if ($rc -eq 3 -and $report) {
    $rows = Get-LauncherPrereqRows $report
    $n = @($report.missing).Count
    Warn "[$lbl] LAUNCHER PREREQUISITE MISSING — this host HAS '$declared', and does NOT have everything that launcher DECLARES it needs."
    foreach ($r in $rows) { Warn "      $r" }
    Warn "      This leg is STILL BUILT. Its sqlite3 CLI smoke gate and its ENTIRE unit corpus are NOT run on this machine —"
    Warn "      running them would exercise a launcher that cannot start the artefact, and every failure would be charged to the compiler."
    # DOWNGRADED TO `skip`, with the answer the machine gave. The plan resolved
    # `launched` from a fact that turned out to be too weak; this is the same
    # statement `skipped-emulator-missing` already makes, one probe deeper. Both
    # artifacts read it back through Test-LegRunSkipped.
    $detail = "the DECLARED launcher '$declared' is present on this host but $n of its DECLARED prerequisite(s) are not — see the rows above for what each one provides and how to install it"
    $leg.run.mode    = 'skip'
    $leg.run.verdict = 'skipped-launcher-prerequisite-missing'
    $leg.run.detail  = $detail
    Set-LegVerdict $lbl 'skipped-launcher-prerequisite-missing' $detail
    return 'unmet'
  }
  # rc 2 is the resolver's own FATAL (a catalogue/usage defect); anything else is
  # an outcome this driver has no arm for — and so is an rc 3 whose report will not
  # parse. NEVER assumed benign: an unreadable answer is not evidence that the
  # launcher works, and running the corpus on that assumption is how a launch
  # failure becomes a compiler accusation. `poisoned` is the closed vocabulary's
  # FAILURE class — it reds the run, and the run CONTINUES to every other leg.
  Warn "[$lbl] the launcher-prerequisite check exited $rc, which this driver does not recognise as a verdict class."
  Warn "      $(if ($text) { $text } else { '<no diagnostic>' })"
  Warn "      Treating it as a FAILURE rather than assuming the launcher is fine — an unreadable outcome is not evidence."
  $detail = "harness_legs.py --check-launcher exited $rc for this leg, so whether its launcher can start the artefact is UNKNOWN on this machine: $(if ($text) { $text } else { '<no diagnostic>' })"
  $leg.run.mode    = 'skip'
  $leg.run.verdict = 'poisoned'
  $leg.run.detail  = $detail
  Set-LegVerdict $lbl 'poisoned' $detail
  return 'unreadable'
}
$LauncherPrereqUnmet = 0
foreach ($lg in $Legs) {
  if ((Test-LauncherPrereq $lg) -notin @('met','not-launched')) { $LauncherPrereqUnmet++ }
}
if ($LauncherPrereqUnmet -gt 0) {
  Warn "$LauncherPrereqUnmet leg(s) will NOT be executed on this machine because a DECLARED launcher prerequisite is absent. They are still BUILT, and Step 9 names each one."
}
# <<< dss:launcher-prereq <<<

# Resolve every SELECTED leg's inputs up front, so the operator learns about a
# missing input in the first minute rather than after the first leg's build.
#
# ★ TWO MAPS, BECAUSE THE TWO ARTIFACTS NEED DIFFERENT LIBRARIES. $LegLibs holds
# only the legs where BOTH tcl and z resolved — that is the FIXTURE's
# precondition, because the fixture IS a Tcl interpreter. $LegLibsAll holds the
# resolution RESULT for every selected leg, Ok or not, because the sqlite3 CLI
# needs ZLIB and does not need Tcl at all: a leg whose Tcl could not be found on
# this host can still produce a perfectly good sqlite3, and Step 7b must be able
# to see its `.Z`. Keeping only the both-resolved map is what made the CLI loop
# TCL-gated while its own comment said it was not.
$LegLibs = @{}
$LegLibsAll = @{}
foreach ($lg in $Legs) {
  $r = Resolve-LegLibraries $lg
  $LegLibsAll[$lg.label] = $r
  if ($r.Ok) {
    $LegLibs[$lg.label] = $r
    Info "[$($lg.label)] tcl : $($r.Tcl)"
    Info "[$($lg.label)] z   : $($r.Z)"
  } else {
    Set-LegVerdict $lg.label 'skipped-build-input-missing' $r.Detail
    Warn "[$($lg.label)] BUILD INPUT MISSING — the TESTFIXTURE will NOT be built for this leg on this machine."
    Warn "      $($r.Detail)"
    # Said out loud, because it changes what the next steps will do: the CLI is a
    # separate artefact with a separate precondition and Step 7b still tries it.
    if ("$($r.Z)") {
      Warn "      …but its ZLIB DID resolve ($($r.Z)), so the sqlite3 CLI is still built for it in Step 7b."
    }
  }
}
$BuildableLegs = @($Legs | Where-Object { $LegLibs.ContainsKey($_.label) })

# ── THE FOURTH Tcl COHERENCE CHECK — AND THE ONLY PER-LEG ONE ────────────────
# [D-HARNESS-TCL-HEADER-IS-HOST-CHOSEN-WHILE-EVERY-LEG-LIBRARY-IS-PINNED]
#
# The Tcl HEADERS this run compiles every leg against were staged back in Step
# 3+4, by the POSIX derivation, from whatever tclConfig.sh that machine happened
# to have. Every leg's LIBRARY, resolved just above, comes from its OWN declared
# provider and is pinned. Nothing had ever compared the two.
#
# ✔MEASURED 2026-08-06, first native macOS run of the .sh: a 9.0 header over the
# pinned 8.6 libraries produced four K_SymbolUndefined (Tcl_GetBool,
# Tcl_GetBoolFromObj, Tcl_GetBytesFromObj, Tcl_GetChild) because sqlite's
# tclsqlite.c gates live code on TCL_MAJOR_VERSION>8. The same skew is reachable
# from here the day the WSL distro's default Tcl moves to 9.x — which the Step
# 3+4 staging comment already records as a KNOWN LATENT hazard.
#
# ★ FATAL, NEVER A WARN, AND NEVER A PER-LEG SKIP. The headers are staged ONCE
# for every leg, so a skew is a property of the RUN, not of one leg; and a warn
# would ship a binary that links clean and then misbehaves. A leg whose library
# version cannot be MEASURED is the one soft outcome and is named out loud.
#
# ★ THE COMPARISON LIVES IN harness_legs.py, called by BOTH drivers with the
# same verb — this capability cannot exist in one driver and not the other
# [D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER]. It reads each
# library's BYTES (export table + self-declared identity), never its file name.
#
# ★ THE HEADER IS MEASURED WHERE IT WAS STAGED, not where it came from: $Stage\
# tclinc\tcl.h is the copy every leg's include list actually points at, and it is
# on THIS filesystem even when the derivation that produced it ran inside WSL.
$StagedTclH = Join-Path (Join-Path $Stage 'tclinc') 'tcl.h'
$tclCohArgs = @('--tcl-coherence', '--staged-tcl-header', "$StagedTclH")
$tclCohLegs = 0
foreach ($lg in $Legs) {
  $t = "$($LegLibsAll[$lg.label].Tcl)"
  if ($t) { $tclCohArgs += @('--leg-tcl-library', "$($lg.label)=$t"); $tclCohLegs++ }
}
if ($tclCohLegs -gt 0) {
  # Invoke-LegResolver takes the rc DIRECTLY off the call and keeps stderr —
  # here the refusal IS the diagnostic.
  $tclCoh = Invoke-LegResolver $tclCohArgs
  if ($tclCoh.Rc -ne 0) {
    # ⚠ THE REMEDY THE RESOLVER NAMES IS NOT ACTIONABLE FROM THIS DRIVER, and
    # saying so is cheaper than letting an operator try it and get nowhere:
    # `DSS_TCL_VERSION` is implemented by build-and-test.sh ONLY (✔MEASURED
    # 2026-08-06: zero occurrences in this file). This driver's Step-3+4
    # derivation picks its tclConfig.sh with `find /usr/lib | head -1` and has no
    # pin at all — D-HARNESS-PS1-TCL-CONFIG-DISCOVERY-8X-ONLY, whose trigger this
    # refusal IS. So the remedy here is to make the POSIX side's Tcl match.
    Die "Tcl HEADER/LIBRARY COHERENCE FAILED (rc=$($tclCoh.Rc)) — refusing to build.`n$(($tclCoh.Text -split "`n" | ForEach-Object { "      $_" }) -join "`n")`n      The staged headers came from the POSIX derivation in Step 3+4 ($StagedTclH); every leg's library comes from its own declared provider. This driver will not compile a fixture against one Tcl and link another.`n      ⚠ `$env:DSS_TCL_VERSION is NOT implemented by this driver (build-and-test.sh only): Step 3+4 takes the first tclConfig.sh it finds, with no pin [D-HARNESS-PS1-TCL-CONFIG-DISCOVERY-8X-ONLY]. Until that is closed, the fix from here is to make the POSIX toolchain's default Tcl the one the legs' libraries are pinned at, then delete $Stage and re-run so the derivation re-stages."
  }
  foreach ($ln in ($tclCoh.Text -split "`n")) { if ($ln.Trim()) { Info "      $ln" } }
  Info "tcl coherence: the staged headers match the libtcl of all $tclCohLegs resolved leg(s) — measured from each library's OWN bytes, not its file name"
}

# ★ SCOPED TO THE LEGS THAT ACTUALLY NEED IT. This warned unconditionally, which
# is now misleading: a leg whose Tcl was ACQUIRED gets the STAGED script library
# instead ($LegTclLibrary at Step 8), so the host's copy is irrelevant to it. It is
# still required by a `host-system`-provider leg, whose libtcl IS this machine's.
# [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY]
$hostTclLegs = @($Legs | Where-Object { "$($_.build.libraries.provider)" -eq 'host-system' } | ForEach-Object { $_.label })
if ($hostTclLegs.Count -and -not (Test-Path -LiteralPath $TclLibrary)) {
  Warn "Tcl script library not at $TclLibrary — needed by the leg(s) whose Tcl is this MACHINE's ($($hostTclLegs -join ' ')); set `$env:TCL_LIBRARY. A leg whose Tcl was ACQUIRED is unaffected: it gets the staged script library."
}
Pass "TESTFIXTURE build inputs (tcl AND zlib) resolved for $($BuildableLegs.Count) of $($Legs.Count) selected leg(s); the sqlite3 CLI needs only zlib and is attempted for $(@($Legs | Where-Object { "$($LegLibsAll[$_.label].Z)" }).Count)"

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
# >>> dss:corpus-engine >>>  (region mirrored in build-and-test.sh)
# ⚠ CORRECTED 2026-08-06: this header used to say "the verifier extracts it from
# this file by these sentinels". THERE IS NO SUCH VERIFIER. ✔MEASURED — the only
# consumers of any `dss:` sentinel in this repository are test-confound-scope.sh
# (`src-provenance`, `src-clone`, `src-gate`, `loadext-stage`, `loadext-verdict`,
# `confound-supply`), test-confound-scope.ps1 (`src-provenance`,
# `loadext-stage-ps1`) and test-driver-contracts.sh (`verdict-vocabulary`);
# `corpus-engine` is read by NOTHING. The sentinels are still worth keeping — they
# mark the paired region for a reader and for the verifier that should exist — but
# a comment asserting a guard that is not there is the exact defect this project
# keeps paying for: an instrument credited with an observation it never made.
# ⇒ nothing about this region is enforced today; the two copies can diverge
# silently, and the confound classifier's SUPPLY (which lives outside it) did
# exactly that for months. Keep the sentinels on their own lines.

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
    # ── THE FIRST DIAGNOSTIC LINE ────────────────────────────────────────────
    # [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY, the harness half.]
    # MEASURED in the .sh sibling: the resume engine reported "the UNNAMED file
    # that aborted ... the log named no resolvable corpus file (last test: none)"
    # ELEVEN TIMES while the captured log's FIRST LINE said exactly what was wrong
    # ("Can't find a usable init.tcl in the following directories: ..."). It had
    # the diagnosis in hand and did not surface it. This is the first non-blank
    # line that is NOT the fixture doing its job - not a per-test line, not a
    # `Time:` line, not an ` Ok`, not a summary. CONSULTED ONLY on the
    # zero-progress abort path, so it can never mislabel a healthy segment.
    Diagnostic = ''
    # ── THE FILES THE TCL TRACEBACKS BLAME ───────────────────────────────────
    # [D-HARNESS-ABORT-FILE-NAMED-ONLY-BY-THE-TRACEBACK.] One entry per traceback
    # block, each the INNERMOST `(file "..." line N)` frame of its block — Tcl
    # prints errorInfo innermost-first, so that is the unit that died and the
    # later frames are permutations.test / veryquick.test driving it. The .sh
    # sibling emits these as `B` facts and its header carries the measurement
    # that motivated them; the LAST entry is the one the caller wants.
    Blamed = New-Object 'System.Collections.Generic.List[string]'
    # ── COMPLETED IS NOT THE SAME AS COVERED ─────────────────────────────────
    # Files that ran to their `Time:` line having emitted no result of their own.
    # The .sh sibling emits these as `I` facts; both drivers must count them, or
    # a run's coverage claim depends on which driver printed it.
    Inert = New-Object 'System.Collections.Generic.List[string]'
  }
  # Result lines this file has emitted that are NOT the two the harness emits for
  # EVERY file. ⚠ The teardown lines end in '...' with NO space before it, so the
  # name to match is `<f>.test-closeallfiles...` — an anchored `-closeallfiles$`
  # test silently never fires, which is exactly how a first cut of this counter
  # reported zero inert files and agreed with nothing.
  # ⚠ ANCHORED THE SAME WAY THE awk TWIN IS. awk tests `$1 !~ /…\.\.\.$/` — the
  #   FIRST FIELD must END with it — so this must require the token to end there
  #   too, not merely to start the line. Without the trailing `(\s|$)` the two
  #   engines diverge on a line with text after the `...`, and the differential
  #   is the only thing that would ever notice.
  $reTeardown = [regex]'^\S+\.test-(closeallfiles|sharedcachesetting)\.\.\.(\s|$)'
  $pend = 0
  $reSummary = [regex]'(\d+) errors? out of (\d+) tests'
  $reTime    = [regex]'^Time: (\S+) \d+ ms$'
  $reTest    = [regex]'^(\S+)\.\.\.'
  $reBang    = [regex]'^! (\S+) (expected|got):'
  # NOTE the leading '!': finalize_testing emits `!Failures on these tests: …`
  # (tester.tcl ~1304, `output2 -nonewline "!Failures on these tests:"`).
  $reFails   = [regex]'^!?Failures on these tests:\s*(.+)$'
  $rePerm    = [regex]'^"?(?:run_test_suite|run_tests)\s+([A-Za-z_][A-Za-z0-9_]*)'
  $reBlame   = [regex]'\(file "([^"]*)" line \d+\)'
  $tbSeen    = $false
  foreach ($line in [System.IO.File]::ReadLines($logPath)) {
    if ($line.Length -eq 0) { continue }
    # FIRST, before any `continue` below: a rule placed lower would never see a
    # line an earlier rule consumed. Only the FIRST qualifying line is kept.
    if (-not $r.Diagnostic -and $line.Trim() -and
        -not $line.StartsWith('Time: ', [System.StringComparison]::Ordinal) -and
        -not $line.EndsWith(' Ok', [System.StringComparison]::Ordinal) -and
        -not $reTest.IsMatch($line) -and -not $reSummary.IsMatch($line)) {
      $d = $line.Replace("`t", ' ')
      $r.Diagnostic = if ($d.Length -gt 400) { $d.Substring(0, 400) + ' ...[truncated]' } else { $d }
    }
    # Per-test tally. An ABORTED segment never prints a summary, so these counts are
    # the ONLY record of the work it did — see the derivation note at the union.
    if ($line.EndsWith(' Ok', [System.StringComparison]::Ordinal)) {
      $r.OkLines++
      if (-not $reTeardown.IsMatch($line)) { $pend++ }
    }
    # ONE BLAMED FILE PER TRACEBACK. The flag is cleared by any line of NORMAL
    # fixture output, so a block contributes exactly its first (innermost) frame
    # and a later traceback contributes its own. Placed here, above the $c
    # dispatch, for the same reason the .sh rule sits above its first `next`: a
    # traceback line starts with spaces and would otherwise fall through every
    # rule below, which is fine today and would stop being fine the moment one of
    # them grows a `continue`.
    if ($line.StartsWith('Time: ', [System.StringComparison]::Ordinal) -or
        $line.EndsWith(' Ok', [System.StringComparison]::Ordinal) -or
        $reTest.IsMatch($line)) { $tbSeen = $false }
    if (-not $tbSeen) {
      $mb = $reBlame.Match($line)
      if ($mb.Success) {
        $tbSeen = $true
        $b = $mb.Groups[1].Value.Replace("`t", ' ')
        if ($b) { $r.Blamed.Add($b) }
      }
    }
    $c = $line[0]
    if ($c -eq 'T') { $m = $reTime.Match($line)
      if ($m.Success) {
        $r.Completed.Add($m.Groups[1].Value)
        if ($pend -eq 0) { $r.Inert.Add($m.Groups[1].Value) }
        $pend = 0
        continue
      } }
    if ($c -eq '!') {
      $m = $reFails.Match($line)
      if ($m.Success) { foreach ($n in ($m.Groups[1].Value -split '\s+')) { if ($n) { $r.FailNames[$n] = $true } }; continue }
      $m = $reBang.Match($line)
      if ($m.Success) {
        $r.FailNames[$m.Groups[1].Value] = $true
        # one `expected:` per FAILED test (`got:` is its partner line) — the failure
        # tally that pairs with OkLines to reconstitute sqlite's own count.
        if ($m.Groups[2].Value -eq 'expected') { $r.FailMarkers++; $pend++ }
        continue
      }
    }
    if ($c -eq '*' -and $line.StartsWith('*** Giving up')) { $r.GaveUp = $true; continue }
    $m = $reSummary.Match($line)
    # ★ THE WHOLE LINE, not $m.Value — FOUND BY THE MIRROR VERIFIER ON ITS FIRST
    # COMPLETE RUN (TF-C124), and it had been wrong here since this function was
    # written. The .sh sibling's parse_segment records `summary=$0` and its
    # header says why: "S is the WHOLE line ('0 errors out of 9 tests on <host>
    # …'), which is what this harness has always printed". This copy recorded
    # only the MATCHED SUBSTRING, so the .ps1 dropped the host and OS suffix
    # while $summaryText's own comment three thousand lines below claimed the
    # text was "the fixture's own, byte for byte". Two drivers, one corpus, two
    # different verdict strings — the exact silent divergence
    # D-HARNESS-CORPUS-ENGINE-MIRROR-CLAIMS-A-VERIFIER-THAT-DOES-NOT-EXIST said
    # the unenforced mirror permitted. `$r.Errors`/`$r.Tests` still come from
    # the capture groups, so the counts are unchanged.
    if ($m.Success) { $r.Summary = $line; $r.Errors = [int]$m.Groups[1].Value; $r.Tests = [int]$m.Groups[2].Value; continue }
    $m = $rePerm.Match($line.TrimStart()); if ($m.Success) { $r.Permutation = $m.Groups[1].Value; continue }
    $m = $reTest.Match($line); if ($m.Success) { $r.LastTest = $m.Groups[1].Value }
  }
  return $r
}

# Which corpus FILE was the fixture inside when it died? Two things can name it,
# and this resolver takes EITHER: a qualified test NAME (LastTest) or the SOURCE
# PATH a Tcl traceback blames (Blamed). Pick the corpus stem that occurs RIGHTMOST
# in it on delimiter boundaries (rightmost, then longest).
# `inmemory_journal.swarmvtabfault-1.1-oom-persistent.143` -> swarmvtabfault.test
# (not swarmvtab.test: the 'f' after it is not a delimiter; not the leading
# permutation token: it is left of it).
#
# ★★ THE DIRECTORY PREFIX IS DISCARDED FIRST, ON EITHER SEPARATOR, AND THAT IS
# WHAT MAKES THIS NAMESPACE-AGNOSTIC. The corpus list is a list of BASENAMES
# (Get-CorpusFiles built it from .Name), so "which corpus file is this" is a
# question about the last path component and nothing else — whose namespace the
# prefix belongs to is then irrelevant. ✔MEASURED on the pe64 wine run: ONE log
# carried `(file "Z:/home/.../symlink2.test" line 48)` (the launcher's spelling,
# because wine resolved that path itself) beside `(file "/home/.../veryquick.test"
# line 16)` (the driver's own argv, handed back verbatim), and NEITHER resolved —
# not a wine quirk, since `/` was never one of the `.`/`-` delimiters this matcher
# accepts. No path TRANSLATION is done and none is declared; see the .sh sibling's
# header for why a `pathTranslation` verb would be the wrong instrument.
# [System.IO.Path]::GetFileName is deliberately NOT used: it honours the HOST's
# separators, and this input comes from a foreign namespace.
function Resolve-AbortFile($lastTest, $corpusFiles) {
  if (-not $lastTest) { return '' }
  $cut = [Math]::Max($lastTest.LastIndexOf('/'), $lastTest.LastIndexOf('\'))
  if ($cut -ge 0) { $lastTest = $lastTest.Substring($cut + 1) }
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
  # ── THE LAUNCHER ARM (D-HARNESS-SQLITE-PROCESS-HYGIENE-BLIND-UNDER-LAUNCHER) ──
  # Everything above matches the fixture's OWN image. Under a declared launcher the
  # OS process is the LAUNCHER's (`wsl.exe`, `qemu-x86_64`, `wine`) and the fixture
  # is its ARGUMENT, so the loop above enumerates nothing — and an empty result from
  # "there were none" is byte-identical to one from "I cannot see into that
  # namespace". That is why this leg's hygiene said UNVERIFIED rather than clean.
  #
  # ★ THE SHORTCUT STAYS REFUSED. Matching the launcher by IMAGE NAME would kill a
  # developer's unrelated `wine` or `qemu` — damaging the user's machine to make the
  # harness look tidy. We match the FULL COMMAND LINE against our exact fixture path
  # instead, which is as precise under a launcher as the path match is natively:
  # that path contains this run's own per-leg rundir (…/dss-sqlite-harness/<leg>-<hash>/…),
  # so it cannot collide with another checkout, another leg, or another developer.
  # ★ THIS MIRRORS THE .sh, WHICH ALREADY DID IT — `our_fixture_pids` matches the
  # fixture path anywhere in `ps -eo args=`. The two drivers disagreed about what
  # "our fixture" means, and only the PowerShell half was blind.
  # ⚠ SELF-EXCLUSION IS LOAD-BEARING, for the reason the .sh records from a MEASURED
  # self-match: this driver's own process tree carries the fixture path in its argv,
  # so without excluding ourselves the sweep finds the harness and tries to kill it.
  # We exclude this process AND its ancestors — never a blanket "skip pwsh", which
  # would let a genuinely leftover pwsh-hosted launcher survive.
  $selfChain = @()
  try {
    $cur = $PID
    for ($i = 0; $i -lt 16 -and $cur; $i++) {
      $selfChain += $cur
      $cur = (Get-CimInstance Win32_Process -Filter "ProcessId = $cur" -ErrorAction Stop).ParentProcessId
    }
  } catch { $selfChain = @($PID) }
  $seen = @{}; foreach ($h in $hits) { $seen[$h.Id] = $true }
  try {
    foreach ($cp in (Get-CimInstance Win32_Process -ErrorAction Stop)) {
      if (-not $cp.CommandLine) { continue }
      if ($selfChain -contains $cp.ProcessId) { continue }
      if ($seen[$cp.ProcessId]) { continue }
      # Both spellings: the launcher may carry the path translated into ITS OWN
      # namespace (legs.json `pathTranslation` turns C:\… into /mnt/c/…), so a
      # Windows-only comparison would miss exactly the WSL case that motivated this.
      if ($cp.CommandLine.Contains($want) -or $cp.CommandLine.Contains($fixturePath)) {
        $proc = Get-Process -Id $cp.ProcessId -ErrorAction SilentlyContinue
        if ($proc) { $hits.Add($proc); $seen[$cp.ProcessId] = $true }
      }
    }
  } catch {
    # Never silent: an enumeration we could not perform is NOT a clean bill.
    Warn "process-hygiene sweep could not enumerate command lines ($($_.Exception.Message)) — launcher-hosted leftovers are UNVERIFIED for this sweep."
  }
  return $hits
}
# ★ ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING
# The PRE-FLIGHT sibling of the matcher above, and it exists because at pre-flight
# time the fixture HAS NO NAME. The compiler decides what to call the artefact (and
# the suffix is the object format's business — see Get-DssReportedArtifact); this
# driver used to guess it from a `$sfx` table of its own, which is exactly the
# defect this cycle removes. The hazard is unchanged, so the sweep is re-scoped to
# the thing we DO know: the leg's artefact DIRECTORY, which the build is about to
# overwrite. Anything executing out of it is a leftover by construction — we hold
# the run lock. Deliberately wider than the exact-path matcher and deliberately
# only used here; the post-build sweeps know the real path and keep using it.
function Get-OurFixtureProcessesUnder($dir) {
  $hits = New-Object 'System.Collections.Generic.List[object]'
  if (-not $dir) { return $hits }
  # Lexical: GetFullPath does not require the directory to exist (on a first run
  # it does not), and the trailing separator keeps `…\pe64` from matching a
  # sibling `…\pe64-something`.
  $want = [System.IO.Path]::GetFullPath("$dir")
  if (-not $want.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
    $want = $want + [System.IO.Path]::DirectorySeparatorChar
  }
  foreach ($p in (Get-Process -ErrorAction SilentlyContinue)) {
    $path = $null
    try { $path = $p.Path } catch { $path = $null }   # access denied on foreign processes
    if (-not $path) { continue }
    if ([System.IO.Path]::GetFullPath($path).StartsWith(
          $want, [System.StringComparison]::OrdinalIgnoreCase)) { $hits.Add($p) }
  }
  return $hits
}
# Kill every process in $procs and return one report line per kill (never silent —
# a killed process is a fact the verdict has to carry). Shared by both sweeps so
# the kill/wait/report behaviour cannot drift between them.
function Stop-FixtureProcesses($procs, $why) {
  $killed = New-Object 'System.Collections.Generic.List[string]'
  foreach ($p in $procs) {
    $desc = "pid $($p.Id) (started $($p.StartTime.ToString('s')))"
    try { $p.Kill(); [void]$p.WaitForExit(15000); $killed.Add("$why — killed $desc") }
    catch { $killed.Add("$why — FAILED to kill ${desc}: $($_.Exception.Message)") }
  }
  return $killed
}
# Kill every leftover of OUR fixture, addressed by its EXACT path.
function Stop-OurFixtures($fixturePath, $why) {
  return (Stop-FixtureProcesses (Get-OurFixtureProcesses $fixturePath) $why)
}
# The pre-flight form: everything running out of this leg's artefact directory.
function Stop-OurFixturesUnder($dir, $why) {
  return (Stop-FixtureProcesses (Get-OurFixtureProcessesUnder $dir) $why)
}

# ── WHAT DID THE COMPILER ACTUALLY WRITE? ASK IT, DO NOT GUESS. ──────────────
# ★ ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING
#
# WHAT THIS REPLACED, and why porting it to the .sh would have been the wrong fix:
#
#     $sfx = if ($fmt -like 'pe*') { '.exe' } else { '' }
#
# Its own comment claimed the suffix was "DERIVED FROM THE OBJECT FORMAT, never
# hardcoded". That was true of the intent and false of the code: it matched a
# format-NAME PREFIX rather than the closed format enum, and every non-`pe*` format
# fell through to '' — wrong for `.dll`, `.so`, `.dylib`, `.a` and `.lib`, and
# harmless only because today's legs are all `-exec`. Meanwhile the .sh sibling
# carried NO suffix logic at all and built `…/testfixture`, so the two
# CAPABILITY-PAIRED drivers disagreed. ✔MEASURED 2026-08-04 on WSL x86_64: the .sh
# cross-built a real `testfixture.exe` for pe64 with ZERO `error[` and recorded the
# leg as a build FAILURE — a false negative on this project's headline capability,
# manufactured by three copies of one table.
#
# DSS owns that table (`TargetSpec::outputExtension`, keyed on the closed
# object-format enum) and now REPORTS the artefact it commits, one line per
# artefact, on stderr:
#
#     dss-code-prime: artifact <targetSpec> <absolute path>
#
# A target spec cannot contain whitespace (DSS refuses one that does), so the path
# is the whole REMAINDER of the line and an output directory with a space in it
# survives. Selecting by SPEC is what keeps a multi-target manifest unambiguous.
#
# ★ AND THE READER ITSELF NOW LIVES IN THE SHARED CORE. A private
# `Get-ReportedArtifact` used to sit here implementing "LAST match wins", with a
# `dss_reported_artifact` in build-and-test.sh doing the same — so extracting
# `Get-DssReportedArtifact` / `dss_bh_reported_artifact` took the copy count from
# two to FOUR, and the two survivors still carried the rule the shared pair
# refuses. "Last wins" silently assumes ONE artefact per (log, spec); that
# stopped being true when this harness grew a second artefact per leg, and the
# failure mode is being handed a SIBLING's binary with no diagnostic at all.
# Both private copies are deleted. Everything here goes through Invoke-DssBuild,
# which runs the compiler, counts `error[`, reads the artefact back through the
# shared reader (which REFUSES an ambiguous log) and checks the file is there.

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
#
# $launcher is the leg's DECLARED launcher argv (legs.json `launchers`, resolved
# per host by harness_legs.py — qemu for a cross-arch host, Wine for a cross-OS
# one), EMPTY for a native leg. When it is non-empty the OS process is the
# LAUNCHER's and the fixture is its first argument. Two consequences are stated
# rather than discovered:
#   · the stall/cap bounds still apply, because they watch the LOG, not the
#     process — a launcher that hangs looks exactly like a fixture that hangs;
#   · `Get-OurFixtureProcesses` matches on OUR EXACT fixture path, so under a
#     launcher it finds nothing and the settle loop below degrades to its 2 s
#     floor. That is a real limitation of the process sweep, not of the run, and
#     it is reported at the call sites rather than papered over: killing by image
#     name would be the workaround, and it would kill a developer's own qemu.
#
# $xlate is the launcher's DECLARED path namespace (legs.json `pathTranslation`)
# and $launchExe is $exe already spelled in it — the fixture path is translated
# ONCE per leg rather than once per segment. $exe itself stays in THIS driver's
# namespace throughout, because the process sweep and every log line address the
# file the way this driver does. The assertion below is the choke point: nothing
# reaches the launcher still spelled the way we hold it.
function Invoke-Fixture($exe, $argv, $workdir, $logPath, $errPath, $stall, $cap, $launcher, $xlate, $launchExe) {
  $emptyIn = Join-Path ([System.IO.Path]::GetDirectoryName($logPath)) '.stdin-eof'
  Set-Content -LiteralPath $emptyIn -Value '' -NoNewline -Encoding ascii
  $launcher = @($launcher | Where-Object { $_ })
  if (-not "$launchExe") { $launchExe = $exe }
  if ($launcher.Count) {
    $procExe  = $launcher[0]
    $procArgs = @($launcher | Select-Object -Skip 1) + @($launchExe) + @($argv)
    Assert-LaunchArgsTranslated $xlate (@($launchExe) + @($argv))
  } else {
    $procExe  = $exe
    $procArgs = @($argv)
  }
  $sp = @{
    FilePath = $procExe; ArgumentList = $procArgs; WorkingDirectory = $workdir
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

# ── Step 7 — PER LEG: generate the manifest + build the testfixture ──────────
# ★ EVERY BUILDABLE LEG IS BUILT, ON EVERY HOST. Whether this machine can EXECUTE
# the result is a separate question, asked in Step 8. A leg that this host can
# never run is still compiled and still linked, because that is the capability
# under test: TF-C113 proved the mach-o arm64 leg by BUILDING it on a Windows box
# and shipping the binary to a Mac.
# ★ A BUILD FAILURE IS `poisoned` FOR THAT LEG AND DOES NOT KILL THE RUN. The old
# single-leg `Die` here was correct when there was one leg; with five it would let
# one broken target delete four legs' worth of evidence — the same "one bad unit
# must never cost the other thousand" rule the resume engine exists for. The
# poisoned verdict is loud, it is in the Step-9 ledger, and it exits non-zero.
Step "7/9  Build each leg's full-source testfixture (dss-code-prime --project, $Config)"
# LAST GATE BEFORE HOURS OF COMPILING — re-assert the shared stage's coherence.
# See the call site at the end of Step 3+4 for why this exists; it is repeated
# here because everything between the two is the last chance to notice that the
# input changed vintage before the expensive part begins.
Assert-StagedSourceCoherence 'staged sqlite (pre-compile)'
$BuiltLegs = @()
foreach ($leg in $BuildableLegs) {
  $lbl  = $leg.label
  $fmt  = $leg.format
  # ★ THE ARTEFACT'S NAME IS NOT KNOWN YET, AND THAT IS CORRECT. It is decided by
  # the compiler and read back out of its report after the build (see
  # Get-DssReportedArtifact for the `$sfx` table that used to live here and what it
  # cost). All this step knows in advance is the DIRECTORY the build routes to.
  $legOut   = Join-Path $OutRoot $lbl
  # The directory the build ROUTES to (`--output <legOut>` + the project driver's
  # per-format subdir). Used ONLY by the pre-flight sweep below, which has to run
  # before the compiler exists to be asked. Everything AFTER the build derives its
  # directory from the reported artefact instead (`Split-Path -Parent $fixture`).
  $preflightDir = Join-Path $legOut $fmt
  $fixture  = $null
  $manifest = Join-Path $Work "testfixture.$lbl.dss-project.json"
  $LegLedger[$lbl].Fmt      = $fmt
  $LegLedger[$lbl].OutDir   = $legOut

  # ── can this driver express the leg's manifest at all? ──
  $blockers = Test-LegManifestBlockers $leg $GenCaps
  if ($blockers.Count) {
    $why = ($blockers -join '  ALSO: ')
    Set-LegVerdict $lbl 'poisoned' "this driver cannot express this leg's manifest correctly: $why"
    Warn "[$lbl] POISONED — refusing to build a manifest this driver cannot express correctly:"
    foreach ($b in $blockers) { Warn "      $b" }
    continue
  }

# >>> dss:preflight >>>
  # PRE-FLIGHT HYGIENE — FIRST thing for this leg, before anything is generated or
  # deleted. The out-dir wipe further down cannot remove a testfixture that is
  # still executing, and that is exactly how a leftover fixture from a dead run turns
  # into "Access to the path … is denied" — an error that looks nothing like its
  # cause. We hold the run lock, so anything still running out of OUR artefact
  # directory is a leftover by construction.
  #
  # PER LEG, against THIS leg's own artefact DIRECTORY — not a file name, because
  # the file has no name until the compiler gives it one (Get-DssReportedArtifact).
  # A sibling leg writes into a different directory and must never be swept here.
  #
  # NOTE the helper functions this calls are defined ABOVE, in the dss:corpus-engine
  # region hoisted before this step. PowerShell binds function names in EXECUTION
  # order, so a helper defined later in the file simply does not exist here — that is
  # a real, measured failure ("The term 'Stop-OurFixtures' is not recognized"), not a
  # style point. Keep the region above this line.
  $LegLedger[$lbl].PreflightKills = @(Stop-OurFixturesUnder $preflightDir 'pre-flight')
  foreach ($k in $LegLedger[$lbl].PreflightKills) { Warn "[$lbl] LEFTOVER FIXTURE: $k" }
# <<< dss:preflight <<<
  $extraDefineArgs = @()
  # The leg supplies target, libraries and — when the generator can take them —
  # its own recipe transform + stack reserve. When it cannot (Test-LegManifestBlockers
  # has already refused any leg that would need them to differ), the call is
  # BYTE-FOR-BYTE the pe64 invocation this driver has always made.
  # ★ THIS LEG'S OWN INCLUDE LIST — the base recipe dirs, the zinc/ staged for ITS
  # recipeTransform, and (FIRST on the list) the cfg/ staged for ITS target OS. Not
  # one shared includes.txt: that is what made every leg compile against the pe
  # leg's zlib header AND against the deriving host's sqlite_cfg.h.
  # ★ THE TWO LIBRARY ARGUMENTS COME FROM THE RESOLVER — see Get-ResolveLibraryArgv
  # for why this driver must not spell the flag, and why a leg whose declared
  # runtime identity cannot be recorded is POISONED rather than built. With no
  # override declared the tokens are byte-identical to the pair this call site has
  # always passed, so the pe64/elf legs' generator invocation is unchanged.
  $libArgvRes = Get-ResolveLibraryArgv $leg $LegLibs[$lbl]
  if (-not $libArgvRes.Ok) {
    Set-LegVerdict $lbl 'poisoned' "the library argv could not be built: $($libArgvRes.Detail)"
    Warn "[$lbl] POISONED — refusing to build an artefact whose runtime library identity would be wrong:"
    foreach ($l in ("$($libArgvRes.Detail)" -split "`n")) { Warn "      $l" }
    continue
  }
  Info "[$lbl] resolve-library: $($libArgvRes.Detail)"
  $genArgs = @(
    $GenPy,
    '--tus',      (Join-Path $Stage 'tus.txt'),
    '--includes', $LegIncludes[$lbl],
    '--defines',  (Join-Path $Stage 'defines.txt'),
    '--target',   $leg.spec
  ) + @($libArgvRes.Tokens) + @(
    '--artifact-name', 'testfixture'
  )
  if ($GenCaps.RecipeTransform) { $genArgs += @('--recipe-transform', "$($leg.build.recipeTransform)") }
  if ($GenCaps.StackReserve)    { $genArgs += @('--stack-reserve',    "$($leg.build.stackReserveBytes)") }
  $genArgs = $genArgs + $extraDefineArgs + @('--output', $manifest)
  $genOut = & $python3.Source @genArgs 2>&1
  if ($LASTEXITCODE -ne 0) {
    Set-LegVerdict $lbl 'poisoned' "manifest generation failed: $($genOut -join ' / ')"
    Warn "[$lbl] POISONED — manifest generation failed:`n$(($genOut | ForEach-Object { "      $_" }) -join "`n")"
    continue
  }
  Info "[$lbl] manifest -> $manifest ($genOut)"
  Info "[$lbl] zlib headers: $($StageDirs[""$($leg.build.headerStageKey)""])  [$(($leg.build.zconfGuards.PSObject.Properties | Sort-Object Name | ForEach-Object { "$($_.Name)=$(if ($_.Value) { 1 } else { 0 })" }) -join ' ')]"

  # SCOPED TO THIS LEG. The wipe used to take the whole of out/; with five legs
  # that would destroy a sibling's just-built artifact. The guard is not ceremony:
  # a $legOut that ever came out empty would make this `Remove-Item -Recurse
  # -Force` of the wrong thing, and the label comes from a JSON document.
  if ((-not $legOut) -or (-not $lbl) -or ($legOut -eq $OutRoot)) { Die "internal: refusing to wipe '$legOut' for leg '$lbl' — it is not a per-leg directory under $OutRoot." }
  if (Test-Path $legOut) { Remove-Item -Recurse -Force $legOut }
  New-Item -ItemType Directory -Force -Path $legOut | Out-Null
  $clog = Join-Path $legOut 'compile.log'
  # A project build routes each target to <output>/<formatName>/, and NAMES the file
  # there itself. dss-code-prime returns exit 0 even on FATAL errors → judge from
  # `error[` plus the artefact the build REPORTED. Invoke-DssBuild (the shared
  # core) is the ONE place that decision is made, for this artefact and the CLI
  # and both of build-and-test.sh's; the three failure statements it keeps
  # distinct are exactly the ones this block used to spell out inline.
  $build = Invoke-DssBuild -DssBin $DssBin -Manifest $manifest -Config $Config `
                           -OutputDir $legOut -Log $clog -Spec $leg.spec
  $ctimeSuffix = $build.TimeSuffix
  if (-not $build.Ok) {
    Get-Content $clog | Select-String -Pattern 'error\[' | Select-Object -First 5 | ForEach-Object { Info "      $($_.Line)" }
    Set-LegVerdict $lbl 'poisoned' "build FAILED$ctimeSuffix — $($build.Error). See $clog"
    Warn "[$lbl] BUILD FAILED$ctimeSuffix — $($build.Error). See $clog"
    continue
  }
  $fixture = $build.Path
  $LegLedger[$lbl].Fixture = $fixture

  # ── the acquired libraries go BESIDE the artefact ──────────────────────────
  # ★ `@loader_path/<name>` IS FALSE WITHOUT THIS STEP. A leg whose libraries were
  # ACQUIRED records a runtime identity that is true BY CONSTRUCTION — dyld
  # resolves `@loader_path` against the directory holding the executable — which
  # makes it a claim about THIS DIRECTORY, and the claim has to be made true here.
  # Skipping it produces a binary that links clean, passes every check this host
  # can perform, and dies at load time on the target machine.
  #
  # DRIVEN BY THE ACQUISITION REPORT (`libraries[].path` / `.as`), never by a name
  # list written here: the file that is copied is exactly the file the build
  # resolved against, under exactly the name the leg declared it would be recorded
  # as. A hardcoded list would be a second place to keep in step with legs.json.
  #
  # A COPY FAILURE IS `poisoned`, NOT A WARNING. The artefact exists but is
  # unloadable, and "built" would be a false statement about it.
  # `@(… | Where-Object { $_ })` is load-bearing, not defensive noise: a hashtable
  # returns $null for an absent key and `@($null)` is an array of ONE $null, so the
  # unfiltered form would report one acquired library for every non-acquiring leg.
  $acquiredLibs = @($LegLibs[$lbl].Acquired | Where-Object { $_ })
  if ($acquiredLibs.Count) {
    $artDir    = Split-Path -Parent $fixture
    $stageFail = ''
    $stagedAs  = @()
    foreach ($al in $acquiredLibs) {
      $src = "$($al.path)"; $as = "$($al.as)"
      if (-not $src -or -not $as) { $stageFail = "the acquisition report carries a library with no path and/or no 'as' name (path='$src', as='$as'), so it cannot be staged beside the artefact"; break }
      $dst = Join-Path $artDir $as
      try { Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction Stop } catch { $stageFail = "could not copy $src -> $dst : $($_.Exception.Message)"; break }
      if (-not (Test-Path -LiteralPath $dst -PathType Leaf)) { $stageFail = "copied $src -> $dst but the destination is not there afterwards"; break }
      $stagedAs += $as
    }
    if ($stageFail) {
      Set-LegVerdict $lbl 'poisoned' "built OK$ctimeSuffix, but its ACQUIRED libraries could not be staged beside the artefact in $artDir — $stageFail. The leg records '$($leg.build.libraries.tclImportName)'-style runtime identities that are only true when the library sits next to the executable, so shipping this binary would produce a LOAD failure on the target machine that nothing on this host can observe."
      Warn "[$lbl] POISONED — built, but its acquired libraries are not beside the artefact:"
      Warn "      $stageFail"
      continue
    }
    Info "[$lbl] staged beside the artefact ($artDir): $($stagedAs -join ' ') — this is what makes the recorded @loader_path identities true"
  }

  $LegLedger[$lbl].Built = $true
  $LegLedger[$lbl].CompileLog = $clog
  $BuiltLegs += $leg
  Pass "[$lbl] testfixture -> $fixture$ctimeSuffix"
}
Info "built $($BuiltLegs.Count) of $($BuildableLegs.Count) buildable leg(s); $($AllLegs.Count) declared"

# ── Step 7b — build the sqlite3 CLI for EVERY SELECTED leg ───────────────────
# ★ A SEPARATE LOOP, AND THAT IS THE POINT — the .sh twin says the same thing at
# the same place. The CLI's buildability is not the fixture's: it needs ZLIB and
# does not need TCL, so a leg that could not resolve Tcl on this host can still
# produce a perfectly good sqlite3. Nesting this in the fixture loop would have
# inherited its `continue`s and made the CLI unbuildable for a reason that has
# nothing to do with it.
#
# ★★ AND IT IS `$Legs`, NOT `$BuildableLegs` — THE COMMENT ABOVE USED TO BE A
# CLAIM THIS LOOP CONTRADICTED. `$BuildableLegs` is filtered on
# `$LegLibs.ContainsKey(...)`, and `$LegLibs` only takes a leg whose resolution
# returned Ok — which Resolve-LegLibraries does only `if ($tcl -and $z)`. So the
# loop that "does not need TCL" was gated on TCL, and a leg with zlib and no Tcl
# silently produced NO sqlite3, NO verdict, and NO effect on the exit code. The
# .sh twin iterates its full LEG_ORDER and gates on LEG_Z_LIB alone; this now
# matches it, which is the only reading under which the two drivers are
# capability-paired.
#
# ★ EVERY SELECTED LEG IS BUILT, ON EVERY HOST. No host test here, and there
# must never be one — whether this machine can EXECUTE the result is Step 7c's
# question, answered from the leg's resolved `run.mode`.
Step "7b/9  Build the sqlite3 CLI (dss-code-prime --project, $Config)"
$CliBuilt = @{}
$CliFails = 0
foreach ($leg in $Legs) {
  $lbl = $leg.label
  $fmt = $leg.format
  # THE ONE DECLARED BUILD INPUT THIS ARTEFACT NEEDS. DSS reads each
  # --resolve-library binary at COMPILE time, so with no zlib there is nothing to
  # compile against — an OBSERVED absence with a named verdict, never an
  # inference from what kind of box this is. It COUNTS as a CLI failure, exactly
  # as the .sh does: an artefact this run declared and did not produce must not
  # leave the run green.
  $legLibRes = $LegLibsAll[$lbl]
  if (-not "$($legLibRes.Z)") {
    $CliFails++
    Set-DssArtifactVerdict $lbl 'sqlite3' 'skipped-build-input-missing' "no zlib could be resolved for this leg on this host, and the CLI links zlib (SQLITE_HAVE_ZLIB=1 reaches a live '#include <zlib.h>' in shell.c). Resolver said: $($legLibRes.Detail)"
    Warn "[$lbl] CLI build NOT ATTEMPTED [skipped-build-input-missing] — $((Get-DssArtifactVerdict $lbl 'sqlite3').Detail)"
    continue
  }
  # ★ ITS OWN OUTPUT DIRECTORY AND ITS OWN COMPILE LOG. `<legOut>/cli/` so the
  # project driver's per-format subdir lands at `<legOut>/cli/<fmt>/sqlite3.exe`
  # and CANNOT collide with `<legOut>/<fmt>/testfixture.exe`. The separate log is
  # the STRUCTURAL half of the artifact-reader fix: two artifacts for the SAME
  # target spec in ONE log is exactly the ambiguity that made "take the LAST
  # match" unsafe. Get-DssReportedArtifact fails loud if it ever arises anyway.
  $cliOut  = Join-Path (Join-Path $OutRoot $lbl) 'cli'
  $cliLog  = Join-Path $cliOut 'compile.log'
  $cliMan  = Join-Path $Work "sqlite3.$lbl.dss-project.json"
  New-Item -ItemType Directory -Force -Path $cliOut | Out-Null
  if (-not $CliLegIncludes.ContainsKey($lbl)) {
    $CliFails++
    Set-DssArtifactVerdict $lbl 'sqlite3' 'poisoned' "this leg has no CLI include list — its zinc stage and/or its sqlite config stage was not produced (see the ZINC-STAGE-FAIL / CFG-STAGE-FAIL line above). Compiling it against another target's zlib header, or against the DERIVING host's sqlite_cfg.h, is refused (D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED / D-HARNESS-MACHO-LEG-INHERITS-THE-DERIVING-LINUX-HOSTS-CONFIGURE-PROBES)."
    Warn "[$lbl] CLI POISONED — $((Get-DssArtifactVerdict $lbl 'sqlite3').Detail)"
    continue
  }
  # ZLIB ONLY — see Get-ResolveLibraryArgv's $Only note for why declaring Tcl on
  # a program that never calls it is a load-time liability, not harmless noise.
  # It is fed from $LegLibsAll, not $LegLibs: this leg may be one whose Tcl did
  # not resolve, and `$Only = @('z')` never reads the .Tcl field.
  $cliLibRes = Get-ResolveLibraryArgv $leg $legLibRes @('z')
  if (-not $cliLibRes.Ok) {
    $CliFails++
    Set-DssArtifactVerdict $lbl 'sqlite3' 'poisoned' "the zlib argv could not be built: $($cliLibRes.Detail)"
    Warn "[$lbl] CLI POISONED — refusing to build an artefact whose runtime library identity would be wrong:"
    foreach ($l in ("$($cliLibRes.Detail)" -split "`n")) { Warn "      $l" }
    continue
  }
  $gen = New-DssManifest -Python $python3.Source -GenPy $GenPy -Output $cliMan `
           -ArtifactName 'sqlite3' -Spec $leg.spec `
           -TusFile (Join-Path $Stage 'cli-tus.txt') `
           -IncludesFile $CliLegIncludes[$lbl] `
           -DefinesFile (Join-Path $Stage 'cli-defines.txt') `
           -LibArgv $cliLibRes.Tokens `
           -RecipeTransform $(if ($GenCaps.RecipeTransform) { "$($leg.build.recipeTransform)" } else { $null }) `
           -StackReserve    $(if ($GenCaps.StackReserve)    { "$($leg.build.stackReserveBytes)" } else { $null })
  if (-not $gen.Ok) {
    $CliFails++
    Set-DssArtifactVerdict $lbl 'sqlite3' 'poisoned' $gen.Error
    Warn "[$lbl] CLI POISONED — $($gen.Error)"
    continue
  }
  Info "[$lbl] cli manifest -> $cliMan ($($gen.Output))"
  $build = Invoke-DssBuild -DssBin $DssBin -Manifest $cliMan -Config $Config `
                           -OutputDir $cliOut -Log $cliLog -Spec $leg.spec
  if (-not $build.Ok) {
    $CliFails++
    Get-Content $cliLog | Select-String -Pattern 'error\[' | Select-Object -First 5 | ForEach-Object { Info "      $($_.Line)" }
    Set-DssArtifactVerdict $lbl 'sqlite3' 'poisoned' "CLI build FAILED$($build.TimeSuffix) — $($build.Error). See $cliLog"
    Warn "[$lbl] CLI BUILD FAILED$($build.TimeSuffix) — $($build.Error). See $cliLog"
    continue
  }
  # The acquired libraries go beside THIS artefact too: `@loader_path/<name>` is a
  # claim about the directory holding the EXECUTABLE, and the CLI's directory is
  # not the fixture's, so staging beside the fixture does not make it true here.
  # $LegLibsAll, for the same reason the argv above uses it: this loop reaches
  # legs $LegLibs does not hold.
  $acq = @($legLibRes.Acquired | Where-Object { $_ })
  if ($acq.Count) {
    $artDir = Split-Path -Parent $build.Path
    $bad = ''
    foreach ($al in $acq) {
      try { Copy-Item -LiteralPath "$($al.path)" -Destination (Join-Path $artDir "$($al.as)") -Force -ErrorAction Stop }
      catch { $bad = "could not copy $($al.path) -> $artDir\$($al.as) : $($_.Exception.Message)"; break }
    }
    if ($bad) {
      $CliFails++
      Set-DssArtifactVerdict $lbl 'sqlite3' 'poisoned' "the CLI built, but its ACQUIRED libraries could not be staged beside it in $artDir — $bad. The artefact records '@loader_path/<name>', so it would fail in the target's loader."
      Warn "[$lbl] CLI POISONED — $bad"
      continue
    }
  }
  $CliBuilt[$lbl] = $build.Path
  Set-DssArtifactVerdict $lbl 'sqlite3' 'built' "sqlite3 -> $($build.Path)"
  Pass "[$lbl] sqlite3 -> $($build.Path)$($build.TimeSuffix)"
}
Info "sqlite3 CLI: built on $($CliBuilt.Count) of $($Legs.Count) selected leg(s); $($AllLegs.Count) declared"

# ── Step 7c — the sqlite3 CLI SMOKE GATE, per leg ────────────────────────────
# ★ WHY THIS EXISTS AT ALL. Step 8's unit corpus runs through `testfixture` — a
# Tcl interpreter linking the sqlite LIBRARY — and NEVER executes shell.c. argv
# handling, the dot-commands, the `.dump` writer and the startup version guard
# are covered by NOTHING without this [D-SQLITE-CLI-BUILT-ON-NO-LEG].
#
# ★ ONE IMPLEMENTATION, BOTH DRIVERS: the fourteen assertions live in
# cli-smoke.py, not in PowerShell and again in bash. Its exit codes are
# 0 pass / 1 charged-to-DSS / 3 red-but-not-DSS, and 3 is still RED — attribution
# says WHO is at fault, never that the gate passed.
Step "7c/9  sqlite3 CLI smoke gate (14 assertions, attributed against gcc)"
if (-not (Test-Path $CliSmokePy)) { Die "the CLI smoke gate is missing: $CliSmokePy" }
# The expectation is read from the STAGED header these binaries were compiled
# against — never a literal in this driver, which would silently stop testing
# anything the day upstream bumps the version.
$StagedHeader = Join-Path (Join-Path (Join-Path $Stage 'sqlite') 'bld') 'sqlite3.h'
if (-not (Test-Path -LiteralPath $StagedHeader)) { Die "the staged sqlite3.h is not there: $StagedHeader — the smoke gate has nothing to compare --version against, and a gate that asserts nothing must never pass quietly." }
$hdr = Get-Content -LiteralPath $StagedHeader
$CliExpectVersion  = ($hdr | Select-String -Pattern '^#define SQLITE_VERSION\s+"(.+)"'   | Select-Object -First 1)
$CliExpectSourceId = ($hdr | Select-String -Pattern '^#define SQLITE_SOURCE_ID\s+"(.+)"' | Select-Object -First 1)
if (-not $CliExpectVersion -or -not $CliExpectSourceId) { Die "could not read SQLITE_VERSION / SQLITE_SOURCE_ID out of $StagedHeader." }
$CliExpectVersion  = $CliExpectVersion.Matches[0].Groups[1].Value
$CliExpectSourceId = $CliExpectSourceId.Matches[0].Groups[1].Value
Info "expecting version '$CliExpectVersion' / source id '$CliExpectSourceId' (from $StagedHeader)"
if (-not $RefCli) { Warn "no gcc reference CLI — every smoke failure this run is UNATTRIBUTABLE and is charged to DSS by design." }
# >>> dss:smoke-targets >>>
# ── WHAT EACH BINARY ACTUALLY IS, READ OUT OF ITS OWN HEADER ────────────────
# ★ `--identify-binary` prints `<arch>\t<container>\t<targetOs>` read from the ELF
# e_machine/EI_OSABI, the PE Machine field or the Mach-O cputype — no external
# tool, and rc 3 with a NAMED diagnostic on bytes it cannot identify. A DEFAULT
# here would be the worst possible kind: the caller is deciding whether a binary
# that would not run is this compiler's fault.
function Get-BinaryTarget($path) {
  # -> @{ Ok; Target; Why }. The path is spelled the way THIS driver reads files
  # (the resolver runs here, not inside a launcher), so a WSL-namespace spelling
  # would be a file this process cannot open.
  $out = @(); $rc = 0
  try {
    $out = @(& $python3.Source $LegsPy '--identify-binary' $path 2>&1)
    $rc  = $LASTEXITCODE
  } catch {
    $rc  = if ($LASTEXITCODE) { $LASTEXITCODE } else { 1 }
    $out = @("$_")
  }
  $text = (@($out) | ForEach-Object { "$_" }) -join "`n"
  if ($rc -ne 0) {
    return @{ Ok = $false; Target = ''; Why = "could not identify $path (rc=$rc): $(if ($text) { $text } else { '<no diagnostic>' })" }
  }
  # tab-separated -> the colon triple cli-smoke.py parses. NEVER fabricated: an
  # unreadable header returns Ok=$false above and the caller says so out loud.
  $triple = ((@($out) | ForEach-Object { "$_" } | Where-Object { $_.Trim() } | Select-Object -First 1) -replace "`t", ':').Trim()
  if (-not $triple) {
    return @{ Ok = $false; Target = ''; Why = "harness_legs.py --identify-binary $path exited 0 and printed NOTHING — a contract break, not a property of the file." }
  }
  return @{ Ok = $true; Target = $triple; Why = '' }
}
# ── AND HOW THIS HOST RUNS A BINARY OF THAT TARGET ──────────────────────────
# ★★ THE HOST-IDENTITY BRANCH THAT USED TO PICK THE REFERENCE'S LAUNCHER IS GONE.
# It tested this driver's own "am I on the host that needs WSL" flag and, when
# true, appended a hardcoded `wsl.exe` + `-e` pair — and THAT is why the oracle was
# unmatched: the reference ran host-native x86_64 while DSS ran arm64 under qemu,
# and then every difference was charged to DSS. The launcher now comes from the leg
# catalogue, keyed on the reference's OWN MEASURED target, through the same resolver
# that answers for every other leg — so it can never again encode a fact about the
# HOST where a fact about the TARGET belongs.
# ⚠ AND THE FLAG'S NAME IS DELIBERATELY NOT WRITTEN ANYWHERE IN THIS BLOCK, not
# even in this sentence. Its ABSENCE from Step 7c is what test-confound-scope.ps1
# asserts, and it asserts it over the block's FULL TEXT — a prose mention would
# make a correct driver red, and the usual cure (match a comment-stripped view)
# would weaken the one guard whose whole content is "this string is not here".
function Get-LauncherForTarget($target) {
  # -> @{ Ok; Launcher = @(); Why }. rc 0 launched-or-native (a native target
  # prints NOTHING and the reason is on stderr), 3 no leg matches / this host
  # cannot run it, 2 a malformed triple — which would be OUR defect, since the
  # triple came from --identify-binary.
  $out = @(); $err = @(); $rc = 0
  $errFile = Join-Path ([System.IO.Path]::GetTempPath()) ("dss-lft-" + [guid]::NewGuid().ToString('N') + '.txt')
  try {
    # stderr to its OWN file: the reason is always written there, on every outcome,
    # and merging it into stdout would make it indistinguishable from the argv.
    $out = @(& $python3.Source $LegsPy '--launcher-for-target' $target '--host-os' $HostOs '--host-arch' $HostArch 2>$errFile)
    $rc  = $LASTEXITCODE
  } catch {
    $rc  = if ($LASTEXITCODE) { $LASTEXITCODE } else { 1 }
  }
  if (Test-Path -LiteralPath $errFile) { $err = @(Get-Content -LiteralPath $errFile); Remove-Item -LiteralPath $errFile -Force -ErrorAction SilentlyContinue }
  $why = (@($err) | ForEach-Object { "$_" }) -join ' '
  if ($rc -ne 0) { return @{ Ok = $false; Rc = $rc; Launcher = @(); Why = $why } }
  # The argv comes back shlex-quoted exactly as a plan's LEG_LAUNCH. Every token
  # this catalogue declares is a bare word, and a quoted one is REFUSED rather than
  # silently split on the space inside it — guessing would produce a launcher argv
  # that is subtly not the declared one.
  $line = (@($out) | ForEach-Object { "$_" } | Where-Object { $_.Trim() } | Select-Object -First 1)
  $toks = @()
  if ($line) {
    if ($line -match "['`"]") {
      return @{ Ok = $false; Rc = $rc; Launcher = @(); Why = "the launcher argv for $target contains a shlex-quoted token this driver will not split by hand: [$line]" }
    }
    $toks = @($line.Trim() -split '\s+' | Where-Object { $_ })
  }
  return @{ Ok = $true; Rc = 0; Launcher = $toks; Why = $why }
}
# The reference is ONE binary and it is the same for every leg, so it is measured
# ONCE. A reference this host cannot identify or cannot execute is DROPPED, loudly:
# cli-smoke.py's CONTROL_ABSENT is an honest state, and running a control that never
# starts is the false-ACQUITTAL half of the same defect family
# (D-HARNESS-ATTRIBUTION-ORACLE-EXONERATES-VIA-A-REFERENCE-THAT-NEVER-RAN).
$RefCliTarget = ''
$RefCliLaunch = @()
if ($RefCli) {
  # ⚠ $RefCliWin, NOT $RefCli. On a Windows host $RefCli is the WSL-namespace path
  # the launcher will be handed; the identification is done by THIS process, which
  # can only open the Windows spelling. On a native POSIX host `win()` is identity
  # and the two are the same string.
  $refId = Get-BinaryTarget $RefCliWin
  if (-not $refId.Ok) {
    Warn "the gcc reference CLI could not be IDENTIFIED — $($refId.Why)"
    Warn "      It is DROPPED for this run rather than passed with a guessed target: an unattributable"
    Warn "      smoke failure is an honest outcome, a fabricated control triple is not."
    $RefCli = ''
  } else {
    $RefCliTarget = $refId.Target
    $lft = Get-LauncherForTarget $RefCliTarget
    if ($lft.Ok) {
      $RefCliLaunch = @($lft.Launcher)
      Info "reference CLI target: $RefCliTarget (MEASURED from its own header) — $($lft.Why)"
      if ($RefCliLaunch.Count) { Info "reference CLI launcher: $($RefCliLaunch -join ' ')  (DECLARED by the catalogue for that target on this host, never inferred from the host's identity)" }
    } elseif ($lft.Rc -eq 3) {
      Warn "this host cannot EXECUTE the gcc reference CLI ($RefCliTarget) — $($lft.Why)"
      Warn "      The reference is DROPPED: a control that cannot start would fail all fourteen assertions for one"
      Warn "      reason and EXONERATE every DSS failure on every leg against a binary that never executed."
      $RefCli = ''
    } else {
      Warn "harness_legs.py --launcher-for-target '$RefCliTarget' exited $($lft.Rc) — $(if ($lft.Why) { $lft.Why } else { '<no diagnostic>' })"
      Warn "      That triple came from --identify-binary, so a malformed one is OUR defect, not this machine's."
      Warn "      The reference is DROPPED rather than run with an unknown launcher."
      $RefCli = ''
    }
  }
}
# <<< dss:smoke-targets <<<
$CliSmokeVerdict = @{}
$CliSmokeFails = 0
# EVERY SELECTED LEG, matching Step 7b — a leg the CLI was built for must reach a
# smoke verdict even when its FIXTURE could not be built, and $BuildableLegs is the
# fixture's list.
foreach ($leg in $Legs) {
  $lbl = $leg.label
  if (-not $CliBuilt.ContainsKey($lbl)) {
    $v = Get-DssArtifactVerdict $lbl 'sqlite3'
    $CliSmokeVerdict[$lbl] = "not run [$(if ($v) { $v.Verdict } else { '<no verdict>' })] — $(if ($v) { $v.Detail } else { 'the CLI build loop never reached this leg' })"
    continue
  }
  # THE ONE LEGITIMATE HOST QUESTION, and it is `run.mode` off the RESOLVED plan
  # — never `$IsWindows`. A `skip` leg WAS BUILT, and that is a completely
  # different fact from "not built"; it is recorded, printed, and never silent.
  # Asked through the SHARED predicate, which is the same call Step 8 makes.
  if (Test-LegRunSkipped $leg) {
    $CliSmokeVerdict[$lbl] = "built, NOT RUN here [$($leg.run.verdict)] — $($leg.run.detail)"
    Warn "[$lbl] CLI smoke SKIPPED — built at $($CliBuilt[$lbl]) but this host cannot execute it: $($leg.run.detail)"
    continue
  }
  $smokeDir = Join-Path (Join-Path $OutRoot $lbl) 'cli-smoke'
  if (Test-Path $smokeDir) { Remove-Item -Recurse -Force $smokeDir }
  New-Item -ItemType Directory -Force -Path $smokeDir | Out-Null
  # ★ THE BINARY IS SPELLED THE WAY ITS LAUNCHER SEES IT. Under `wsl.exe -e` the
  # child reads a WSL path, so the same Convert-LaunchPath the corpus runner uses
  # is applied here. cli-smoke.py passes only RELATIVE names for its databases
  # and sets the child's cwd, so nothing else needs translating.
  $legXlate = "$($leg.run.pathTranslation)"
  # ★ WHAT THIS LEG'S CLI ACTUALLY IS, MEASURED FROM ITS OWN HEADER — never assumed
  # from the leg's name and never from the spec, which is the DECLARED side.
  # cli-smoke.py compares the two and reports a leg that built the WRONG TARGET as
  # its own non-verdict; that comparison is worth nothing if this driver feeds it
  # the declaration twice. Identified through the DRIVER's spelling of the path,
  # not the launcher's — this process is the one opening the file.
  $cliId = Get-BinaryTarget $CliBuilt[$lbl]
  if (-not $cliId.Ok) {
    $CliSmokeFails++
    $CliSmokeVerdict[$lbl] = "FAIL — the built CLI could not be IDENTIFIED ($($CliBuilt[$lbl])); no smoke verdict was taken"
    Warn "[$lbl] CLI smoke NOT RUN — $($cliId.Why)"
    Warn "      This is RED and it is NOT charged to the compiler: the gate needs the subject's MEASURED target and this"
    Warn "      driver will not fabricate one. Counted as a failure so the run cannot exit 0 over a leg it never asserted about."
    continue
  }
  $smokeArgs = @($CliSmokePy,
    '--cli',              (Convert-LaunchPath $legXlate $CliBuilt[$lbl]),
    '--expect-version',   $CliExpectVersion,
    '--expect-source-id', $CliExpectSourceId,
    '--leg-spec',         $leg.spec,
    '--cli-target',       $cliId.Target,
    '--workdir',          $smokeDir,
    '--label',            $lbl,
    '--json',             (Join-Path $smokeDir 'result.json'))
  # ★ `--opt=value`, NOT `--opt value` — a launcher TOKEN may itself begin with a
  # dash and argparse then refuses it as "expected one argument" rather than taking
  # it as the value. ✔MEASURED 2026-08-05 (TF-C121): the sibling `--reference-launcher`
  # line below passed `-e` positionally and killed the pe64 CLI smoke gate outright;
  # the harness then reported that argv bug as `smoke: FAIL — CHARGED TO DSS`, i.e. it
  # accused the compiler of a defect in its own command line. THIS line had the same
  # shape and had simply never been reached by a dash-leading token — `wine`,
  # `qemu-aarch64` and `wsl.exe` all start with a letter. It is NOT hypothetical:
  # legs.json declares `arch -x86_64` for the macho64-x86_64 leg on a darwin/arm64
  # host, whose second token is `-x86_64`. Fixed here BEFORE that host ever runs it.
  # (D-HARNESS-DASH-LEADING-LAUNCHER-TOKEN-MISPARSED-AS-AN-OPTION)
  foreach ($t in @($leg.run.launcher)) { if ($t) { $smokeArgs += @("--launcher=$t") } }
  # ★★ THE REFERENCE'S LAUNCHER IS RESOLVED FROM ITS MEASURED TARGET, NOT FROM THIS
  # HOST'S IDENTITY. What stood here was a branch on this driver's own host flag
  # that appended a hardcoded `wsl.exe` + `-e` pair, and it is WHY THE ORACLE WAS
  # UNMATCHED: on a Windows host it ran the reference host-native x86_64 while DSS
  # ran arm64 under qemu, and then every difference was charged to DSS. (The flag's
  # name is not written here on purpose — see the note at Get-LauncherForTarget: its
  # ABSENCE from this whole block is what the confound-scope test pins, over the
  # block's FULL text.) The .sh twin had the same
  # bug in its latent form (it passed NO reference launcher at all, correct only for
  # as long as every host that owns a reference can execute it directly — which stops
  # being true on the arm64 VPS). Both are now ONE question asked of the catalogue,
  # `--launcher-for-target <the reference's MEASURED triple>`, resolved once above.
  # `--reference-target` is that same MEASURED triple, and it is what lets
  # cli-smoke.py refuse to exonerate anything against a control aimed elsewhere.
  # ★ `=` FORM for every launcher token, same rule and same anchor as `--launcher`
  # above (D-HARNESS-DASH-LEADING-LAUNCHER-TOKEN-MISPARSED-AS-AN-OPTION): this is the
  # very option whose SPACE form killed the pe64 gate before one assertion ran.
  # ⚠ `wsl.exe -e` is still REQUIRED where WSL is the launcher, and it still is —
  # legs.json DECLARES both tokens, which is why they no longer need to be spelled
  # here. Without `-e` WSL routes the command line through the distro's default
  # shell, which expands `$( )` on the WINDOWS side before bash ever sees it.
  if ($RefCli) {
    $smokeArgs += @('--reference', $RefCli, '--reference-target', $RefCliTarget)
    foreach ($t in @($RefCliLaunch)) { if ($t) { $smokeArgs += @("--reference-launcher=$t") } }
  }
  # ── THE LEG'S DECLARED RUN ENVIRONMENT, APPLIED TO THIS CHILD TOO ──────────
  # D-HARNESS-PS1-CLI-SMOKE-IGNORES-THE-LEGS-DECLARED-LAUNCH-ENVIRONMENT. The
  # spawn below used to stand bare, with nothing set around it, and the fourteen
  # assertions behind it ran with NONE of the environment the leg declares:
  #   ✔MEASURED 2026-08-07, elf64-arm64 on this host — every assertion rc=255,
  #   `qemu-aarch64: Could not open '/lib/ld-linux-aarch64.so.1'`, while legs.json
  #   had declared QEMU_LD_PREFIX for that very launcher and Step 8 was applying
  #   it twenty lines away. A harness defect reported as `CHARGED TO DSS` on a
  #   binary that is completely fine — the false-ACCUSATION direction, which sends
  #   someone hunting a compiler bug that does not exist.
  # ★ ONE MECHANISM, BOTH STEPS: Push-LegLaunchEnv is the same call Step 8 makes,
  # so a leg that declares a launcher variable, or whose libraries were staged,
  # cannot be honoured in one step and ignored in the other again.
  # ★ THE LIBRARY DIRECTORY IS zlib's, NOT Tcl's, and that is not an omission: the
  # CLI links zlib and does not embed Tcl, and this loop reaches legs whose Tcl
  # never resolved at all (`$LegLibsAll`, not `$LegLibs`) — asking for a Tcl
  # directory here would fail on exactly the leg the CLI was still built for. Same
  # choice, same reason, as the .sh twin's smoke subshell.
  $smokeLibDirs = @(@($LegLibsAll[$lbl].Z) | Where-Object { $_ } | ForEach-Object { Split-Path -Parent $_ })
  $smokeEnv = Push-LegLaunchEnv $leg (Get-LegLoaderSearchPath $leg $smokeLibDirs) @() @()
  try {
    # WHAT ACTUALLY CROSSED, read back out of the environment rather than restated
    # from what this driver meant to set — the whole class of defect above is a
    # value that was intended and never arrived. Bounded, because a native leg's
    # loader variable is the entire PATH.
    Info "[$lbl] launcher run environment: $((@($smokeEnv.Names) | ForEach-Object { $v = "$([Environment]::GetEnvironmentVariable($_))"; if ($v.Length -gt 160) { $v = $v.Substring(0, 160) + '…' }; "$_=$v" }) -join '  ')"
    $smokeOut = & $python3.Source @smokeArgs 2>&1
    $srcc = $LASTEXITCODE
  } finally { Pop-LegLaunchEnv $smokeEnv }
  Set-Content -LiteralPath (Join-Path $smokeDir 'smoke.log') -Value $smokeOut -Encoding utf8NoBOM
  foreach ($l in $smokeOut) { Info "      $l" }
  # ★★ EVERY rc THE GATE CAN RETURN HAS ITS OWN ARM — `default` IS THE LAST RESORT,
  # NOT THE DEFAULT VERDICT (D-HARNESS-CLI-SMOKE-CHARGES-A-LAUNCH-FAILURE-TO-THE-COMPILER).
  # Until TF-C136 this switch had arms for 0 and 3 only, so EVERY other rc — including
  # a gate that explicitly DECLINED to attribute, and an argv defect of our own — fell
  # into `default` and printed as an accusation against the compiler. ✔MEASURED: 14 rows
  # of "CHARGED TO DSS" over an elf64-arm64 binary that never launched, because qemu
  # could not find the guest loader. A default arm that names a culprit will eventually
  # name the wrong one; enumerate, and make the surviving `default` say "unknown rc".
  # ⚠ CAPABILITY-PAIRED: the `.sh` twin's `case` carries the identical arms. Change one,
  # change both, or the two harnesses disagree about who is at fault.
  switch ($srcc) {
    0 { $CliSmokeVerdict[$lbl] = 'PASS (14/14)'; Pass "[$lbl] CLI smoke: 14/14" }
    1 { # ★ THE ARM THE ENUMERATION LEFT OUT, AND IT IS THE ACCUSATION ITSELF.
        # rc 1 is the gate's "CHARGED TO DSS" — a MATCHED control passed where the
        # subject failed. Until this line it had no arm and fell into `default`,
        # which prints "an rc the driver does not understand is a driver defect.
        # NOT charged to DSS": a genuine, matched, attributed compiler failure
        # reported as a harness defect and quietly exonerated. That is the
        # FALSE-ACQUITTAL direction — the one that HIDES a real bug — and it was
        # introduced by the very change that removed the false-accusation default.
        # Enumerating the rc table means enumerating ALL of it. Paired with the
        # .sh twin's `1)` arm, in one change.
        $CliSmokeFails++
        $CliSmokeVerdict[$lbl] = "FAIL — CHARGED TO DSS (a MATCHED gcc control passes the assertions this leg fails); see $smokeDir\result.json"
        Warn "[$lbl] CLI smoke RED and CHARGED TO DSS — the reference targets this leg's own target, it launched, and it passes what this binary fails." }
    3 { $CliSmokeFails++
        $CliSmokeVerdict[$lbl] = "FAIL — NOT DSS (the gcc reference fails identically); see $smokeDir\result.json"
        Warn "[$lbl] CLI smoke RED, but DSS is NOT implicated — the gcc reference fails the same assertions." }
    4 { $CliSmokeFails++
        $CliSmokeVerdict[$lbl] = "FAIL — NOT A VERDICT (unattributable); see $smokeDir\result.json"
        # RED and counted, deliberately: an unattributable run is a FAILURE, never a
        # warning. What changed is WHO it names — `dssImplicated` is null, not true.
        Warn "[$lbl] CLI smoke RED, but this run is NOT A VERDICT about generated code — the subject never launched and/or there was no MATCHED control (the reference targets a different arch/format than this leg). See $smokeDir\result.json 'controlState' + 'subjectLaunched'." }
    2 { $CliSmokeFails++
        $CliSmokeVerdict[$lbl] = "FAIL — HARNESS ARGV DEFECT (the gate rejected its own arguments); see $smokeDir\smoke.log"
        Warn "[$lbl] CLI smoke could not run: the gate REJECTED THE ARGUMENTS THIS DRIVER PASSED IT. That is our defect, not the compiler's — see $smokeDir\smoke.log." }
    default { $CliSmokeFails++
        $CliSmokeVerdict[$lbl] = "FAIL — UNKNOWN rc=$srcc from the smoke gate; see $smokeDir\result.json"
        Warn "[$lbl] CLI smoke returned rc=$srcc, which this driver has no arm for. NOT charged to DSS — an rc the driver does not understand is a driver defect. Add an arm here and to the .sh twin." }
  }
}


# ── Step 8 — PER LEG THIS HOST CAN EXECUTE: run the .test UNIT CORPUS ────────
# ★ THE ONE LEGITIMATE HOST QUESTION, AND IT IS NOT ASKED HERE. Whether a leg
# runs is `run.mode` off the resolved plan — `native`, `launched` (through the
# leg's DECLARED launcher), or `skip` with a NAMED verdict from the closed
# vocabulary. There is no `$IsWindows` in this step and there must never be one.
# A `skip` leg WAS BUILT, and Step 9 says so beside its verdict: "built, not run
# here" is a completely different fact from "not built".
Step "8/9  Run SQLite unit corpus ($Tier.test) on every runnable leg + classify"
# Shared across legs — the staged corpus is one tree and one tier, whichever leg
# is executing it.
$StagedTestDir = (Get-Content -Raw (Join-Path $Stage 'testdir.win.txt')).Trim()
$TestFile = if ($env:DSS_TEST_FILE) { $env:DSS_TEST_FILE } else { Join-Path $StagedTestDir "$Tier.test" }
if (-not (Test-Path $TestFile)) { Die "test file not found: $TestFile (tier '$Tier')." }
# Tier exclusions — announced BEFORE the run so the reduction is on the record even
# if the fixture never reaches a summary (see $TierExcludes above).
if ($TierExcludes.Count) {
  Warn "tier EXCLUSIONS active — this run is NOT full-corpus coverage on ANY leg"
  Info "      QUICKTEST_OMIT=$($TierExcludes -join ',')  (sqlite's own hook, test/permutations.test)"
  Info "      drops these file(s) from every `$allquicktests-derived permutation (still run under 'full'):"
  Info "        $($TierExcludes -join ' ')  (operator-set DSS_TIER_EXCLUDES — the default is EMPTY: the 100% corpus)"
}
$CorpusFiles = Get-CorpusFiles $StagedTestDir
$TierPerms   = Get-TierPermutations $TestFile
$TierPrefixes = Get-TierPrefixes (Join-Path $StagedTestDir 'permutations.test')

# A leg that BUILT but that this host cannot execute keeps the resolver's verdict —
# and the fact that it was BUILT is recorded with it. This is the whole point of
# the cycle: the build happened on a host that can never run the artifact.
foreach ($leg in $BuiltLegs) {
  if (-not (Test-LegRunSkipped $leg)) { continue }
  Set-LegVerdict $leg.label "$($leg.run.verdict)" "BUILT OK ($($LegLedger[$leg.label].Fixture)) but NOT RUN on this host — $($leg.run.detail)"
  # …and the UNIT-level sentence too, guarded by the same closed vocabulary. Two
  # artifacts per leg means two ways to lose one silently.
  Set-UnitNotRun $leg.label "$($leg.run.verdict)" "$($leg.run.detail)  (the fixture DID build: $($LegLedger[$leg.label].Fixture))"
  Info "[$($leg.label)] built, NOT run here [$($leg.run.verdict)]: $($leg.run.detail)"
}
$RunnableLegs = @($BuiltLegs | Where-Object { -not (Test-LegRunSkipped $_) })
if ($RunnableLegs.Count -eq 0) { Info "no built leg can be EXECUTED on this host — every one of them has a named skip verdict above." }

# ── THE LOADEXT HELPER: THIS DRIVER NOW STAGES ONE, LIKE ITS SIBLING ─────────
# D-HARNESS-PS1-STAGES-NO-LOADEXT-HELPER-COVERAGE-IS-UNDECLARED (registered
# 2026-08-05, TF-C120) is the gap this closes. It was a DIFFERENT defect from its
# .sh sibling D-HARNESS-LOADEXT-HELPER-TARGET-BLINDNESS-NOW-ABORTS-THE-RUN: that
# one staged a WRONG-TARGET helper and died on it; this one staged NO helper at
# all, so ~16 loadext-* units per leg were left to sqlite's own hardcoded-`gcc`
# self-build (or, on a box with no gcc, to a "Skipping loadext tests" line and no
# verdict at all).
#
# ★ WHY IT COULD NOT SIMPLY BE COPIED OVER BEFORE, AND WHAT CHANGED. The blocker
# was real and it was written down here: the helper has to be built FOR THE LEG'S
# TARGET by a compiler on THIS host, and on Windows `gcc -dumpmachine` prints
# `x86_64-w64-mingw32` (MEASURED) — correct for pe64 and correctly REFUSED for
# elf64-x86_64. Staging would therefore have marked this driver's LARGEST leg
# (elf64-x86_64, 330,436 units under wsl.exe) `skipped-build-input-missing` for
# want of a Linux-targeting compiler on a Windows PATH. Closing one gap by
# opening a bigger one is not closing it.
#
# ⇒ THAT PREMISE IS GONE. DSS emits the helper for the leg's declared
# `sharedLibFormat`, so this Windows host builds the elf64 leg's
# `libtestloadext.so` itself. ✔MEASURED 2026-08-05 on this box: the shared
# resolver produced a 13,912-byte ELF64 ET_DYN for x86_64:elf64-x86_64-linux-dyn
# with no Linux compiler present at all, and the same call produced an
# 8,192-byte PE with IMAGE_FILE_DLL set for pe64 — which sqlite's own
# sqlite3_load_extension() loaded, answering `SELECT half(9.0)` = 4.500.
# [D-HARNESS-CROSS-HOST-ANY-TARGET]
#
# ★★ AND IT IS THE SAME CODE, NOT A SECOND IMPLEMENTATION. Everything that
# decides — object format, argv, whether the artefact is a loadable shared
# library, whether a control was possible, which verdict class a failure is —
# lives in harness_legs.py, which both drivers already hard-require. That is what
# keeps this from becoming the next capability that exists in one driver and not
# the other.
#
# ⚠ WHAT IS STILL DIFFERENT FROM THE .sh, STATED RATHER THAN LEFT TO BE NOTICED:
# a leg this driver launches through `wsl.exe` runs its fixture in ANOTHER
# filesystem namespace. The helper is staged into the run's testdir, which the
# fixture reaches through the same translated path as everything else, so it is
# found — but the CONTROL arm, when one exists, is a compiler on THIS host, not
# inside the launcher's namespace. Resolving a WSL-side cc for a wsl.exe-launched
# leg is still something neither driver can do; it costs nothing today because
# the primary arm needs no compiler at all.
#
# ⚠ ASCII ONLY in the emitted strings below: a non-ASCII character in gate output
# has already killed a run on a cp1252 console at its LAST line.

# Resolve-LoadextHelper -> @{ Ok; Class; Detail; CrossCheck; Staged }
#
# A THIN CALL, deliberately: it hands the shared resolver the paths only this
# driver knows and turns ONE report into ONE verdict. `$ReferenceCc` may be
# EMPTY, and that is the whole de-host-locking — empty means "no verified target
# compiler here", which costs the leg its CONTROL and nothing else.
#
# THE rc CONTRACT (harness_legs.py --build-loadext-helper): 0 staged, 3 poisoned
# (a REAL failure - the run must not exit 0), 4 skipped-build-input-missing
# (ENVIRONMENTAL - only reachable when the operator selected the control arm and
# this box cannot provide it). Anything else is treated as a failure, never as a
# quiet success: an unreadable outcome is not evidence the helper is there.
function Resolve-LoadextHelper {
  param(
    [Parameter(Mandatory)] $Python,
    [Parameter(Mandatory)] $LegsPy,
    [Parameter(Mandatory)] $Label,
    [Parameter(Mandatory)] $Builder,
    [Parameter(Mandatory)] $DssBin,
    [Parameter(Mandatory)] $SqliteSrc,
    [Parameter(Mandatory)] $SqliteBld,
    [Parameter(Mandatory)] $DestDir,
    [Parameter(Mandatory)] $WorkDir,
    [Parameter(Mandatory)] $Config,
    $ReferenceCc = '',
    $ReferenceMachine = ''
  )
  $call = @($LegsPy, '--build-loadext-helper', $Label,
            '--helper-builder', "$Builder",
            '--dss', "$DssBin",
            '--sqlite-src', "$SqliteSrc", '--sqlite-bld', "$SqliteBld",
            '--dest-dir', "$DestDir", '--work-dir', "$WorkDir",
            '--dss-config', "$Config",
            '--reference-cc', "$ReferenceCc",
            '--reference-machine', "$ReferenceMachine")
  $out = & $Python @call 2>&1
  $rc = $LASTEXITCODE
  $text = ($out | ForEach-Object { "$_" }) -join "`n"
  # The report is on stdout on EVERY outcome - a driver needs the detail most
  # when it failed - so it is parsed the same way either way. Output that is not
  # JSON at all is the resolver's own FATAL line; it is quoted rather than
  # swallowed.
  $rep = $null
  try { $rep = $text | ConvertFrom-Json } catch { $rep = $null }
  if ($null -eq $rep) {
    return @{ Ok = $false; Class = 'poisoned'; Staged = ''; CrossCheck = ''
              Detail = "the helper build exited $rc and printed something this driver could not read as a report: $($text -replace "`n", ' ')" }
  }
  switch ($rc) {
    0 { return @{ Ok = $true;  Class = '';         Staged = "$($rep.staged)"; Detail = "$($rep.detail)"; CrossCheck = "$($rep.crossCheck)" } }
    4 { return @{ Ok = $false; Class = 'skipped-build-input-missing'; Staged = ''; Detail = "$($rep.detail)"; CrossCheck = "$($rep.crossCheck)" } }
    3 { return @{ Ok = $false; Class = 'poisoned'; Staged = ''; Detail = "$($rep.detail)"; CrossCheck = "$($rep.crossCheck)" } }
    default {
      return @{ Ok = $false; Class = 'poisoned'; Staged = ''; CrossCheck = ''
                Detail = "the helper build exited $rc, which this driver does not recognise as a verdict class ('$($rep.verdictClass)'). Treating it as a failure rather than assuming the helper was staged. $($rep.detail)" }
    }
  }
}

# The leg's OPTIONAL control compiler. Empty when this host has none, which is
# the normal case for a cross leg and costs the leg NOTHING - the primary arm is
# DSS. Never a guess: it is whatever --resolve-target-cc accepted, or nothing.
# ★ ONLY ASKED FOR A LEG THIS DRIVER WILL ACTUALLY RUN, and only up front, so the
# per-leg staging call below never spawns a probe mid-corpus.
$LegControlCc = @{}
foreach ($leg in $RunnableLegs) {
  $lbl = $leg.label
  $LegControlCc[$lbl] = @{ Cc = ''; Machine = '' }
  $ccOut = & $python3.Source $LegsPy '--resolve-target-cc' $lbl 2>&1
  if ($LASTEXITCODE -eq 0) {
    $parts = ("$ccOut" -split "`t")
    if ($parts.Count -ge 2) {
      $LegControlCc[$lbl] = @{ Cc = $parts[0].Trim(); Machine = $parts[1].Trim() }
      Info "[$lbl] control cc: $($parts[0].Trim()) - it reports '$($parts[1].Trim())', which is $($leg.spec)'s arch+OS (asked, not assumed)"
    } else {
      # ★ THE THIRD OUTCOME, WHICH USED TO FIRE NEITHER BRANCH AND SAY NOTHING.
      # rc=0 is the resolver's "I FOUND one" answer, and its contract is a single
      # TAB-separated `<cc>\t<machine>` line. An rc=0 that does not parse is a
      # BROKEN CONTRACT, not an absent compiler - and the two must not look alike,
      # because the silent version left $LegControlCc[$lbl] empty and the run then
      # proceeded exactly as if the host had no cross-compiler, i.e. a resolver
      # defect wearing the disguise of a normal, expected, costless condition.
      # NOT a Die: the control arm is optional by construction (a leg with no
      # control still builds its helper with DSS), so refusing the whole run over
      # it would be the reverse defect. It is a NAMED, LOUD verdict with the bytes
      # quoted, consistent with the `default {}` arm of Resolve-LoadextHelper
      # above, which likewise refuses an unrecognised shape instead of assuming.
      Warn "[$lbl] --resolve-target-cc exited 0 but did not answer in the declared <cc><TAB><machine> shape - got $($parts.Count) field(s): [$ccOut]. Treating this leg as having NO control compiler, and saying so: an rc=0 that will not parse is a RESOLVER defect, not the ordinary 'this host has no cross-compiler' case reported below. The loadext helper is still built by DSS; only the cross-check is lost."
    }
  } else {
    Info "[$lbl] no CONTROL compiler on this host - the loadext helper will be built by DSS for $($leg.build.sharedLibFormat), which needs nothing from this machine."
  }
}
$SqliteStageSrc = Join-Path (Join-Path $Stage 'sqlite') 'src'
$SqliteStageBld = Join-Path (Join-Path $Stage 'sqlite') 'bld'
# Legs whose fixture BUILT but whose loadext helper could not be STAGED. Its own
# counter, deliberately: it is neither a fixture compile failure (the fixture
# built fine) nor a unit failure (no unit ran), and folding it into either would
# print a Step-9 line that names the wrong thing. The leg is ALSO recorded
# `poisoned`, so the exit code is already covered by $vPoisoned; this counter
# exists so the summary can say WHAT poisoned it. Same split as the .sh's
# $STAGE_FAILS.
$LoadextStageFails = 0

foreach ($leg in $RunnableLegs) {
# ★★ THE BODY OF THIS LOOP IS DELIBERATELY NOT INDENTED. Two reasons, both
# load-bearing: (1) test-confound-scope.ps1 EXTRACTS the shipped classifier below
# by matching `^\$real = @\(\)…` at COLUMN 0 and executes it at script scope —
# indenting it would break the Step-0 self-test, which is a refuse-to-start;
# (2) TF-C114 wrapped ~300 lines of already-proven corpus logic in this loop, and
# re-indenting every one of them would have buried the actual change in a
# whitespace diff on the one leg that must not regress. PowerShell attaches no
# meaning to indentation, so this costs nothing but a reader's first glance.
$LegTag      = $leg.label
$legRec      = $LegLedger[$LegTag]
$fixture     = $legRec.Fixture
$legOut      = $legRec.OutDir
$LegRunMode  = $leg.run.mode
$legLauncher = @($leg.run.launcher)
# The launcher's DECLARED path namespace + this leg's fixture spelled in it.
# Translated ONCE, here, so a per-segment path is the only other site.
$legXlate    = "$($leg.run.pathTranslation)"
$legLaunchFixture = Convert-LaunchPath $legXlate $fixture
# CONFOUNDS ARE PER LEG AND EVERY ONE OF THEM WAS EARNED SOMEWHERE — read from
# THIS LEG'S OWN DECLARATION (legs.json `confounds`, resolved by harness_legs.py),
# which is the same declaration build-and-test.sh reads. One ledger, both drivers.
$Confounds   = @(Get-LegConfounds $leg)
Step "8/9  [$LegTag] $($leg.spec) — $Tier.test ($LegRunMode$(if ($legLauncher.Count) { ": $($legLauncher -join ' ')" })$(if ($legXlate -and $legXlate -ne 'none') { "; paths -> '$legXlate' via '$($leg.run.pathTranslator -join ' ')'" }))"
if ($legXlate -and $legXlate -ne 'none') {
  Info "[$LegTag] the launcher addresses files in ANOTHER namespace — fixture $fixture -> $legLaunchFixture"
}
if ($Confounds.Count) {
  Info "[$LegTag] confound patterns in force ($($Confounds.Count)): $($Confounds -join ' ')$(if ($null -ne $ConfoundsOverride) { '   [operator DSS_CONFOUNDS — applied to EVERY leg]' } else { '   [EARNED on this leg — legs.json `confounds`, provenance per pattern]' })"
} else {
  Info "[$LegTag] NO confound patterns: this leg's catalogue entry declares ``confounds: []``, i.e. nothing has ever been measured as a non-DSS confound HERE, and a confound must be EARNED per platform, never copied from a sibling leg. Every failure here counts."
}
# The leg's OWN library directories go on its TARGET's loader search variable so
# its fixture can load them at run time; TCL_LIBRARY points the Tcl runtime at its
# script library.
#
# ★ THE VARIABLE NAME IS A PROPERTY OF THE TARGET, NOT A CONSTANT
# [D-HARNESS-RUN-ENV-LD-LIBRARY-PATH-INERT-ON-DARWIN]. For a `windows` target it
# resolves to PATH, which is byte-for-byte what this driver has always done and is
# the only case it can run natively; for an elf/macho leg it resolves to the
# variable that target's loader actually reads, instead of a name chosen once and
# then wrong for four of the five legs.
#
# ★★ AND SO IS THE SPELLING OF ITS VALUE. This line used to join the directories
# with `[System.IO.Path]::PathSeparator` — the HOST's separator, `;` on Windows —
# and pass them in the HOST's spelling, so an ELF leg was handed `C:\a;C:\b` and
# ld.so looked for one directory of that name. ✔MEASURED 2026-08-07: the
# elf64-arm64 fixture died `libtcl8.6.so: cannot open shared object file` with
# both libraries staged in the directory the variable named. Get-LegLoaderSearchPath
# builds it in the LAUNCHER's namespace, with the TARGET's separator, through the
# leg's own DECLARED pathTranslation — and for a native leg (translation `none`,
# target separator `;`) it yields byte-for-byte the string this line always made.
$LegLoaderVar = Get-LegLoaderPathVar $leg
$legLibDirs = @((Split-Path $LegLibs[$LegTag].Tcl), (Split-Path $LegLibs[$LegTag].Z)) | Select-Object -Unique
$runEnvPath = Get-LegLoaderSearchPath $leg $legLibDirs
# ★ THE ACQUIRED Tcl's SCRIPT LIBRARY WINS OVER THE HOST'S, and only for a leg
# that HAS one [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY]. $TclLibrary
# is this machine's own copy: correct for a `host-system` leg (whose libtcl is
# this machine's) and WRONG for an acquired one, whose libtcl bakes in its
# packager's prefix. Leg-scoped, never a global export, and never keyed on the
# host - a leg whose Tcl was acquired needs the staged copy on EVERY host that
# runs it.
# ⚠ THE FALLBACK IS A BRIDGE, NOT AN ENDORSEMENT, AND IT IS NOW LOAD-BEARING FOR
# EVERY LEG. As of this working tree ALL FIVE legs declare `pinned-archive`, so
# there is no `host-system` leg left for which $TclLibrary is the right answer -
# yet until legs.json declares a `dataDirs` entry with role `tclScriptLibrary`,
# `scriptLibraryDir` is empty and every leg lands here, on the HOST's script
# library. That is exactly the pairing D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-
# LIBRARY warns about (a packager's library against a host's scripts, matched on
# nothing but a version-number coincidence). It is kept because removing it would
# red a currently-working tier before the declaration lands, and the per-leg WARN
# at acquisition time says so out loud every run rather than letting it pass.
$LegTclLibrary = if ($LegLibs[$LegTag].TclScriptDir) { "$($LegLibs[$LegTag].TclScriptDir)" } else { $TclLibrary }
# THE DRIVER-SIDE run directory. It exists on EVERY leg regardless of where the
# corpus actually runs, because this driver has to be able to WRITE into it: the
# loadext helper is produced by a process on this machine and can only land where
# this machine can put a file. For a `driver` filesystem it is also where the
# fixture runs; for a foreign one it is the staging area the resolver's copy argv
# reads FROM. [D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS]
$rundir = Join-Path $legOut 'run'; if (Test-Path $rundir) { Remove-Item -Recurse -Force $rundir }
New-Item -ItemType Directory -Force -Path $rundir | Out-Null
# WHERE THE CORPUS RUNS, DECLARED — never "wherever this driver happens to put
# its build tree". `$legRunDir` is the launcher's own path when the two
# filesystems differ, and $rundir when they do not; `$legLaunchRun` is empty in
# the second case, which is how every site below tells the two apart without
# knowing a single verb name.
$runDirPlan   = Get-LegRunDirPlan $LegTag $rundir
$legRunFs     = "$($runDirPlan.runFilesystem)"
$legLaunchRun = "$($runDirPlan.launcherPath)"
$legRunDir    = if ($legLaunchRun) { $legLaunchRun } else { $rundir }
# The launcher argv the fixture is spawned through — the DECLARED command with
# the working-directory option already spliced in by the resolver. Equal to the
# declared command whenever the verb needs none, so a `driver` leg's spawn is
# byte-for-byte the one it has always been.
$legLauncher  = @($runDirPlan.launcher | Where-Object { $_ })
if ($legLaunchRun) {
  Info "[$LegTag] the launcher writes onto ITS OWN filesystem (runFilesystem '$legRunFs') — the corpus runs in $legLaunchRun"
  Info "      NOT in $rundir, which that launcher reaches only through a compatibility mount whose POSIX file modes are synthesised from one Windows attribute (chmod 644 reads back as 777). [D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS]"
  $mk = Invoke-RunDirArgv $LegTag "clear the run directory $legLaunchRun" $runDirPlan.rmTreeArgv @($legLaunchRun)
  if ($mk.Ok) { $mk = Invoke-RunDirArgv $LegTag "create the run directory $legLaunchRun" $runDirPlan.mkdirArgv @("$legLaunchRun/$SqliteTestdirSubdir") }
  if (-not $mk.Ok) {
    # PER-LEG, NEVER THE RUN. Same rule as the loadext staging block below.
    # ⛔ NOT a fallback to $rundir: that is the DrvFs directory this declaration
    # exists to keep the corpus off, and taking it silently would manufacture ~60
    # failures that look like compiler defects.
    Set-LegVerdict $LegTag 'poisoned' "the fixture BUILT ($fixture), but this leg's DECLARED run directory could not be prepared, so its corpus was NOT run. $($mk.Detail)"
    Set-UnitNotRun $LegTag 'poisoned' "run directory preparation FAILED: $($mk.Detail)"
    Warn "[$LegTag] POISONED - could not prepare the declared run directory; this leg's corpus is NOT run, the rest of the run CONTINUES:"
    Warn "      $($mk.Detail)"
    continue
  }
} else {
  Info "[$LegTag] the corpus runs in this driver's own filesystem (runFilesystem '$legRunFs') — $rundir"
}
$runlog = Join-Path $legOut 'corpus.log'
$Ledger = Join-Path $legOut 'corpus-units.txt'

# >>> dss:loadext-stage-ps1 >>>
# THE LOADEXT HELPER, STAGED FOR THIS LEG'S TARGET. See the block above Step 8's
# leg loop for why this driver can do it at all now.
#
# tester.tcl's cmdlinearg(testdir) default: the fixture `file mkdir`s this subdir
# of its CWD and cd's into it before any .test body runs, so a test's relative
# './libtestloadext.so' ('./testloadext.dll' on a Windows Tcl) resolves THERE.
# This driver passes no --testdir override, so the destination is $rundir/testdir.
#
# ★ A FAILURE HERE IS A PER-LEG VERDICT, NEVER THE END OF THE RUN. The .sh learnt
# this the expensive way on 2026-08-05: a `die` in its staging function ended a
# run in which two legs had already reported green over 331,351 and 331,355
# units. Every branch below records a NAMED verdict from the closed vocabulary in
# tests/test_support/arm_verdict_ledger.hpp and CONTINUES to the next leg.
$helper = Resolve-LoadextHelper -Python $python3.Source -LegsPy $LegsPy `
            -Label $LegTag -Builder $LoadextBuilder -DssBin $DssBin `
            -SqliteSrc $SqliteStageSrc -SqliteBld $SqliteStageBld `
            -DestDir (Join-Path $rundir $SqliteTestdirSubdir) `
            -WorkDir (Join-Path $legOut 'loadext-helper') -Config $Config `
            -ReferenceCc $LegControlCc[$LegTag].Cc `
            -ReferenceMachine $LegControlCc[$LegTag].Machine
if (-not $helper.Ok) {
  if ($helper.Class -eq 'skipped-build-input-missing') {
    # ENVIRONMENTAL, and only reachable when the operator selected the CONTROL
    # arm - the default builder needs nothing from this machine. It must NOT red
    # a run in which nothing is broken.
    Set-LegVerdict $LegTag 'skipped-build-input-missing' "the fixture BUILT ($fixture), but DSS_LOADEXT_HELPER=$LoadextBuilder was requested and this host cannot provide that arm, so this leg's corpus was NOT run. $($helper.Detail)"
    Set-UnitNotRun $LegTag 'skipped-build-input-missing' "$($helper.Detail)"
    Warn "[$LegTag] corpus NOT run - the requested loadext helper arm is unavailable here; the rest of the run CONTINUES:"
    Warn "      $($helper.Detail)"
  } else {
    # A REAL failure: the compiler under test did not produce a loadable shared
    # library. `poisoned` because this leg's loadext-* units cannot be trusted
    # and the alternative - run the corpus anyway - hands the fixture back to
    # loadext.test's own hardcoded `gcc`, which is the wrong-target helper
    # D-HARNESS-ARM64-LEG-HOST-ARCH-HELPER-SO is named after.
    $LoadextStageFails++
    Set-LegVerdict $LegTag 'poisoned' "the fixture BUILT ($fixture), but this leg's loadext helper extension could not be staged, so its corpus was NOT run and this run covers NONE of its units. $($helper.Detail)"
    Set-UnitNotRun $LegTag 'poisoned' "loadext helper staging FAILED: $($helper.Detail)"
    Warn "[$LegTag] POISONED - loadext helper staging FAILED; this leg's corpus is NOT run, the rest of the run CONTINUES:"
    Warn "      $($helper.Detail)"
  }
  continue
}
Info "[$LegTag] loadext helper -> $($helper.Staged) - $($helper.Detail)"
if ($helper.CrossCheck) { Info "      $($helper.CrossCheck)" }
# ★ AND INTO THE FILESYSTEM THE FIXTURE WILL ACTUALLY LOOK IN. The helper is
# BUILT by a process on this machine, so it can only be written where this
# machine can write; when the corpus runs in the launcher's own filesystem the
# staged file has to be carried across, through the resolver's DECLARED copy
# argv. Without this the move to an ext4 run directory would have FIXED the
# permission families and BROKEN loadext.test — trading one manufactured failure
# class for another. [D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS]
if ($legLaunchRun) {
  $helperSrc = Convert-LaunchPath $legXlate "$($helper.Staged)"
  $helperDst = "$legLaunchRun/$SqliteTestdirSubdir/$([System.IO.Path]::GetFileName("$($helper.Staged)"))"
  $cp = Invoke-RunDirArgv $LegTag "copy the loadext helper to $helperDst" $runDirPlan.copyArgv @($helperSrc, $helperDst)
  if (-not $cp.Ok) {
    $LoadextStageFails++
    Set-LegVerdict $LegTag 'poisoned' "the fixture BUILT ($fixture) and its loadext helper was produced, but the helper could not be carried into this leg's DECLARED run directory, so its corpus was NOT run. $($cp.Detail)"
    Set-UnitNotRun $LegTag 'poisoned' "loadext helper transfer FAILED: $($cp.Detail)"
    Warn "[$LegTag] POISONED - the loadext helper could not reach the run directory; this leg's corpus is NOT run, the rest of the run CONTINUES:"
    Warn "      $($cp.Detail)"
    continue
  }
  Info "[$LegTag] loadext helper carried into the launcher's filesystem -> $helperDst"
}
# <<< dss:loadext-stage-ps1 <<<

# >>> dss:corpus-loop >>>
# Segment queue. Segment 0 is EXACTLY today's invocation (`fixture <tier>.test`)
# so a run with no abort is bit-for-bit the run it always was; resume segments are
# only ever appended by an abort.
$segments   = @(@{ Kind = 'tier'; Args = (Get-SegmentArgs $legXlate $TestFile @()); Patterns = @(); Label = "$Tier.test"; Perm = '' })
$results    = @()          # one Read-CorpusSegment record per segment actually run
$aborts     = @()          # one record per abort — these NEVER disappear from the verdict
$notReached = @()          # units we can prove were never given a chance
$hygiene    = @()          # leftover/killed fixture processes — never silent
foreach ($k in $legRec.PreflightKills) { $hygiene += $k }
if ($LockStolen) { $hygiene += "took over a STALE run lock left by $LockStolen" }
foreach ($n in $CloneLockNotes) { $hygiene += $n }
# ★ THE BLANKET `UNVERIFIED` THAT USED TO LIVE HERE IS GONE, and removing it was as
# much a correctness fix as adding the sweep arm: once Get-OurFixtureProcesses
# matches the launcher by FULL COMMAND LINE, declaring every launcher leg
# unverified is no longer a cautious statement — it is a FALSE one, and a
# permanent "UNVERIFIED" on every emulated leg trains a reader to ignore the field
# on the day it means something. ✔MEASURED 2026-08-06, both halves separately:
#   P1 a `wsl.exe -e <fixture> …` launcher IS enumerable and matchable by its
#      CommandLine (2 processes matched on the exact path).
#   P2 killing that Windows-side launcher REAPS the Linux-side process — the exact
#      pid captured beforehand (438336) read GONE afterwards and `pgrep -x
#      testfixture` came back empty.
# ⚠ P2's FIRST measurement said the opposite, and it was an INSTRUMENT ARTEFACT:
#   the prober asked `pgrep -f <marker>` inside `bash -c`, whose own command line
#   carries the marker, so pgrep matched itself and reported a "survivor" whose pid
#   (438287) was not either of the pids that had been running (438267/438277). That
#   is the SAME self-match `our_fixture_pids` in the .sh already records from a
#   measured incident. Re-run by pid identity — which cannot self-match — it
#   reversed. The lesson is why this note exists: a survivor whose PID CHANGED was
#   never a survivor.
# An enumeration that FAILS is still reported, by Get-OurFixtureProcesses itself —
# a sweep that could not run is not a clean bill, and that path warns.
$resumes    = 0
$lastBoundary = ''
# The previous segment's first diagnostic, but ONLY when that segment completed
# zero files. Empty means "the last segment made progress", which is what keeps a
# genuine mid-corpus crash on the ordinary resume path. $preconditionFail is the
# diagnostic itself once detected — the classifier below reads it, so the verdict
# is decided in one place. [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY]
$prevZeroDiag = ''
$preconditionFail = ''
$oldTclLib = $env:TCL_LIBRARY
$oldOmit = $env:QUICKTEST_OMIT; $oldPatterns = $env:SQLITE_TEST_PATTERN_LIST
# The leg's DECLARED launcher environment (legs.json `launchers[].env`, e.g.
# QEMU_LD_PREFIX) — read HERE only to name the candidates in the line below.
# APPLYING it, snapshotting it and restoring it belong to Push/Pop-LegLaunchEnv,
# which the CLI smoke gate calls too: this block used to do it inline, and the
# smoke gate's own inline copy simply omitted it
# [D-HARNESS-PS1-CLI-SMOKE-IGNORES-THE-LEGS-DECLARED-LAUNCH-ENVIRONMENT].
$legEnvNames = @(Get-LegDeclaredEnvNames $leg)
# ── the launcher's ENVIRONMENT namespace ─────────────────────────────────────
# WHICH VARIABLES MAY CROSS IS THIS DRIVER'S KNOWLEDGE — it is the one that sets
# them. `$env:PATH` stays ABSENT on purpose: a foreign fixture handed this host's
# PATH would be worse off than with its own, and there is no translation that
# makes a Windows PATH mean anything to a Linux process.
# ★ TCL_LIBRARY IS PRESENT AND IS NOT IN THE PLAIN LIST
# [D-HARNESS-PS1-TCL-LIBRARY-NOT-FORWARDED-ACROSS-THE-WSL-BOUNDARY]. Its value is
# a HOST path, so it crosses through $legForwardPaths, which the resolver puts
# through this launcher's DECLARED pathTranslation. Leaving it out entirely was
# the defect; adding it to the plain list would have been a QUIETER one — Tcl
# would fail to find init.tcl again and the driver would blame the acquisition.
# WHICH kind each name is, and the refusal of any name with no declared kind, is
# the resolver's (LAUNCH_FORWARD_KINDS); HOW they cross belongs to the verb.
# ★★ AND THE LOADER SEARCH VARIABLE IS ONE OF THEM, WHICH IT WAS NOT.
# [D-HARNESS-PS1-LOADER-SEARCH-PATH-NEVER-CROSSES-THE-LAUNCHER-BOUNDARY.] The
# value was built and set on THIS process and then named nowhere, so for a
# launcher that does not inherit — every leg a Windows host launches — it never
# crossed at all: WSLENV is what makes a Windows-side variable visible inside WSL.
# The fixture then failed to load a library that was staged, present and correctly
# spelled, which reads as a broken artefact rather than as a variable that never
# arrived. It crosses as DECLARED (already in the launcher's namespace by
# construction), never as a forwarded HOST path — see Push-LegLaunchEnv.
# ⚠ AND IT DOES NOT CONTRADICT `$env:PATH stays ABSENT` ABOVE. For a `windows`
# target the loader variable IS PATH — but a windows target is the one this driver
# runs NATIVELY, whose envTransfer is `inherit`, so no carrier is built and PATH is
# never named in one. A future windows-target-through-a-launcher cell would have to
# answer that question deliberately rather than inherit this line's answer.
$legEnvVerb      = "$($leg.run.envTransfer)"
$legForwardPlain = @('SQLITE_TEST_PATTERN_LIST', 'QUICKTEST_OMIT')
$legForwardPaths = @('TCL_LIBRARY')
$legForward      = $legForwardPlain + $legForwardPaths + $legEnvNames + @($LegLoaderVar)
$legCarrierName = Get-LaunchEnvCarrierName $legEnvVerb
if ($legCarrierName) {
  Info "[$LegTag] the launcher does NOT inherit this driver's environment (envTransfer '$legEnvVerb') — variables that are SET at spawn time cross via $legCarrierName; candidates: $($legForward -join ', ')"
}
$si = 0
while ($si -lt $segments.Count) {
  $seg = $segments[$si]
  $log = if ($si -eq 0) { $runlog } else { Join-Path $legOut "corpus.resume$si.log" }
  if ($si -eq 0) { Info "[$LegTag] running $($seg.Label) via $([System.IO.Path]::GetFileName($fixture)) …" }
  else {
    Info "[$LegTag] segment $($si + 1): $($seg.Label)$(if ($seg.Patterns.Count) { "  (SQLITE_TEST_PATTERN_LIST: $($seg.Patterns.Count) candidate file(s))" })"
  }
  $legEnv = $null
  try {
    if (Test-Path -LiteralPath $LegTclLibrary) { $env:TCL_LIBRARY = $LegTclLibrary }
    if ($TierExcludes.Count) { $env:QUICKTEST_OMIT = ($TierExcludes -join ',') }
    # SQLITE_TEST_PATTERN_LIST is a Tcl LIST of globs; corpus basenames are
    # bare words, so a space join is a valid list.
    if ($seg.Patterns.Count) { $env:SQLITE_TEST_PATTERN_LIST = ($seg.Patterns -join ' ') } else { $env:SQLITE_TEST_PATTERN_LIST = $null }
    # LAST, because it reads the three variables set above as well as its own: the
    # leg's DECLARED launcher environment, its loader search path, and the
    # launcher's environment TRANSFER, resolved PER SEGMENT from what is actually
    # SET right now. For `inherit` the transfer is empty and the spawn below is
    # byte-for-byte the run it always was.
    # ★ THE SAME CALL THE CLI SMOKE GATE MAKES (Step 7c). It was two inline copies
    # of one decision until 2026-08-07, and the copy that mattered — the smoke
    # gate's — had never applied the leg's declared environment at all.
    $legEnv = Push-LegLaunchEnv $leg $runEnvPath $legForwardPlain $legForwardPaths
    $run = Invoke-Fixture $fixture @($seg.Args) $rundir $log "$log.stderr" $SegStall $SegCap $legLauncher $legXlate $legLaunchFixture
    $segRc = $run.Rc
  } finally {
    # Pop FIRST: it restores every name it assigned, TCL_LIBRARY included (the
    # carrier rewrites that one to its TRANSLATED spelling), and the three lines
    # after it put back what this block set on its own.
    Pop-LegLaunchEnv $legEnv
    $env:TCL_LIBRARY = $oldTclLib
    $env:QUICKTEST_OMIT = $oldOmit; $env:SQLITE_TEST_PATTERN_LIST = $oldPatterns
  }
  if ($run.KillReason) { Warn "[$LegTag] segment $($si + 1) HUNG — killed: $($run.KillReason)"; $hygiene += "segment $($si + 1) TIMED OUT and was killed — $($run.KillReason)" }
  # POST-SEGMENT HYGIENE — a segment that spawned or left a fixture behind must not
  # carry its file handles into the next one (the abort class this engine exists for
  # is a leaked handle), nor outlive the run.
  foreach ($k in (Stop-OurFixtures $fixture "after segment $($si + 1)")) {
    Warn "[$LegTag] LEFTOVER FIXTURE: $k"; $hygiene += $k
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
      Warn "[$LegTag] segment $si stopped EARLY at the --maxerror cap (`*** Giving up...`) — this is NOT full coverage"
      $notReached += "every file after $lastF in '$($seg.Label)' — the fixture hit its --maxerror cap and finalised early (raise it with --maxerror=N)"
    }
    continue
  }

  # ── ABORT ──────────────────────────────────────────────────────────────────
  # ★★ FIRST: IS THIS AN ABORT AT ALL, OR A PRECONDITION FAILURE?
  # [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY, the harness half.]
  #
  # MEASURED on the operator's Mac (in the .sh sibling; the defect is the
  # mechanism's): the fixture could not initialise a SLAVE interpreter, so EVERY
  # segment died before running a single file. The engine reported "the UNNAMED
  # file that aborted ... the log named no resolvable corpus file" ELEVEN TIMES and
  # then "resume budget (10) exhausted" - burning its whole budget on a failure
  # that could never make progress, and never naming a cause, while the captured
  # log's FIRST LINE said exactly what was wrong.
  #
  # ★★ THIS DOES NOT WEAKEN THE RESILIENCE RULE, AND THE SEPARATION IS EXACT.
  # A fixture abort/crash stays a RECOVERABLE outcome: named, resumed past,
  # reported in the union - one bad unit must never cost us the other thousand.
  # What is added is a DISTINCT case with two conjuncts a genuine mid-corpus crash
  # cannot satisfy together: (1) the segment completed ZERO files, and (2) its
  # first diagnostic is IDENTICAL to the previous segment's, which also completed
  # zero files. A real crash on the corpus's next file also completes zero files -
  # but the resume boundary STRICTLY ADVANCES every time, so it dies in a different
  # file with a different diagnostic and (2) fails. The FIRST such abort is still
  # resumed exactly as today: one attempt is what distinguishes "could not start"
  # from "crashed at the start".
  if ($res.Completed.Count -eq 0 -and $res.Diagnostic -and $res.Diagnostic -eq $prevZeroDiag) {
    $preconditionFail = $res.Diagnostic
    Warn "[$LegTag] PRECONDITION FAILURE — the fixture completed ZERO test files in TWO consecutive segments with the IDENTICAL first diagnostic."
    Warn "      This is NOT a resumable fixture crash: nothing the resume engine can do changes it,"
    Warn "      so the remaining $($MaxResumes - $resumes) resume(s) are NOT spent on it."
    Warn "      the diagnostic, verbatim, from $log :"
    Warn "        $($res.Diagnostic)"
    Info "      first lines of that log:"
    Get-Content $log -TotalCount 6 | ForEach-Object { Info "        $_" }
    $notReached += "EVERY unit of the '$(if ($seg.Perm) { $seg.Perm } else { $Tier })' corpus — the fixture never completed a single file. PRECONDITION FAILURE: $($res.Diagnostic)"
    break
  }
  # Carried to the NEXT segment so the comparison above has something to compare
  # against. Cleared by any segment that made progress, which is what keeps a
  # genuine crash on the resilience path.
  $prevZeroDiag = if ($res.Completed.Count -eq 0) { $res.Diagnostic } else { '' }
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
  # THE TRACEBACK FIRST, THE TEST NAME SECOND. The blamed frame is the fixture
  # SAYING which file it was in; the last test name is a PROXY for it, and a
  # measured-wrong one — the pe64 wine run resolved `symlink.test-sharedcache
  # setting` to symlink.test, a file whose own `Time:` line was already in the
  # same log, while the traceback named symlink2.test. LastTest stays because a
  # KILLED segment prints no traceback at all, which is when resume matters most.
  $blamed = if ($res.Blamed.Count) { $res.Blamed[$res.Blamed.Count - 1] } else { '' }
  $abortSource = ''
  $abortFile = Resolve-AbortFile $blamed $CorpusFiles
  if ($abortFile) {
    $abortSource = "named by the Tcl traceback ($blamed)"
  } else {
    $abortFile = Resolve-AbortFile $res.LastTest $CorpusFiles
    if ($abortFile) { $abortSource = "INFERRED from the last test name ($($res.LastTest))" }
  }
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
  Warn "[$LegTag] ABORT #$($aborts.Count) — segment $si ('$($seg.Label)') $how"
  Info  "        permutation        : $(if ($perm) { $perm } else { '(UNDETERMINED)' })$(if ($permInferred) { "   [$permInferred]" })"
  Info  "        last file completed: $(if ($lastDone) { $lastDone } else { '(none)' })"
  Info  "        died inside file   : $(if ($abortFile) { $abortFile } else { '(unresolved)' })   last test: $(if ($res.LastTest) { $res.LastTest } else { '(none)' })"
  Info  "        how it was named   : $(if ($abortSource) { $abortSource } else { '(could not be named — no traceback frame and no resolvable test name)' })"
  # The unit that died NEVER goes unreported — named when we can name it, described
  # by what we do know when we cannot. Silence about a unit is the defect.
  if ($abortFile) {
    # "last test emitted", not "aborted at": the two are the same thing only when
    # the file got as far as a do_test. symlink2.test died before its first one,
    # so the last name in that log belonged to the PREVIOUS file — which is
    # exactly the confusion the old wording invited.
    $notReached += "the REMAINDER of $abortFile under permutation '$(if ($perm) { $perm } else { '?' })' ($(if ($abortSource) { $abortSource } else { 'source unrecorded' }); last test emitted: $(if ($res.LastTest) { $res.LastTest } else { 'none' }))"
  } else {
    $what = if ($forced) { "the resume boundary was FORCED to $(if ($boundary) { $boundary } else { 'the end of the corpus' }), so that one file may have been skipped without a verdict" }
            else { "the next segment resumes from $(if ($boundary) { $boundary } else { 'the end of the corpus' }) and will RE-ATTEMPT it" }
    # The traceback frame, when there was one, goes IN the report even though it
    # did not resolve: "the log named nothing" and "the log named something that
    # is not in this corpus" are different facts and the reader needs the second.
    $notReached += "the UNNAMED file that aborted under permutation '$(if ($perm) { $perm } else { '?' })' after $(if ($lastDone) { $lastDone } else { 'the start of the permutation' }) — the log named no resolvable corpus file (last test: $(if ($res.LastTest) { $res.LastTest } else { 'none' }); traceback frame: $(if ($blamed) { $blamed } else { 'none' })); $what"
  }
  Get-Content $log -Tail 6 | ForEach-Object { Info "      $_" }

  if (-not $boundary) { Warn "[$LegTag] the abort is at the END of the corpus file list — nothing left to resume."; continue }
  if (-not $perm) {
    Warn "[$LegTag] CANNOT RESUME — the aborting permutation could not be determined from the log."
    $notReached += "every unit after $boundary — no resume was possible (permutation undetermined; see $log)"
    continue
  }
  $permIdx = $TierPerms.IndexOf($perm)
  if ($resumes -ge $MaxResumes) {
    Warn "[$LegTag] RESUME BUDGET EXHAUSTED ($MaxResumes) — stopping. Raise DSS_MAX_RESUMES to go further."
    $notReached += "every unit after $boundary in '$perm'" + $(if ($permIdx -ge 0 -and $permIdx -lt $TierPerms.Count - 1) { " and every permutation after '$perm' ($($TierPerms[($permIdx + 1)..($TierPerms.Count - 1)] -join ' '))" } else { '' }) + " — resume budget ($MaxResumes) exhausted"
    continue
  }
  # (a) the rest of the aborting permutation, via sqlite's own file-selection hook.
  $after = Get-FilesAfter $CorpusFiles $boundary
  $resumes++; $lastBoundary = $boundary
  Info  "        -> resume $resumes/$($MaxResumes): permutations.test $perm, corpus files after $boundary"
  $tail = @(@{ Kind = 'perm'; Args = (Get-SegmentArgs $legXlate (Join-Path $StagedTestDir 'permutations.test') @($perm))
               Patterns = $after; Perm = $perm; Label = "permutations.test $perm (after $boundary)" })
  # (b) the tier continued from the NEXT permutation — the ORIGINAL tier script, so
  # every ifcapable/platform guard is evaluated by sqlite exactly as in a full run.
  if ($seg.Kind -ne 'perm') {
    if ($permIdx -lt 0) {
      Warn "[$LegTag] permutation '$perm' is not named by $([System.IO.Path]::GetFileName($TestFile)) — cannot continue the tier past it."
      $notReached += "every permutation after '$perm' in $([System.IO.Path]::GetFileName($TestFile)) — '$perm' is not one of its run_test_suite entries"
    } elseif ($permIdx -lt $TierPerms.Count - 1) {
      $next = $TierPerms[$permIdx + 1]
      $tail += @{ Kind = 'tier'; Args = (Get-SegmentArgs $legXlate $TestFile @("--start=${next}:")); Patterns = @(); Perm = $next
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
$filesDone = 0; $filesInert = 0; $failNames = @(); $calibration = @()
foreach ($r in $results) {
  $filesDone += $r.Completed.Count
  $filesInert += $r.Inert.Count
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
  # (bare = every leg). Parsed rather than ignored: without this, a scoped pattern
  # would be treated as the literal regex "emulated:^writecrash-", match nothing,
  # and SILENTLY excuse nothing while appearing configured
  # (D-SQLITE-CONFOUND-LIST-DRIVER-ASYMMETRY).
  # ★ THE SCOPE IS NOW A FACT ABOUT THE LEG, NOT ABOUT THE DRIVER. Until TF-C114
  # this line was the constant 'native', because the driver had exactly one leg
  # and it ran its binary directly. A leg whose resolved run.mode is `launched`
  # executes through a DECLARED launcher (qemu / Wine / `arch -x86_64`), which is
  # precisely what `emulated:` means in the .sh — so it is read off the leg. The
  # DEFAULT is still 'native', which is also what test-confound-scope.ps1 gets
  # when it executes this block with no leg in scope.
  $legMode = if ($LegRunMode -eq 'launched') { 'emulated' } else { 'native' }
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
  Warn "[$LegTag] $($scopedExcused.Count) failure(s) excused ONLY because this leg runs '$legMode': $($scopedExcused -join ' ')"
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
  # ★ BESIDE "files completed", NEVER INSTEAD OF IT, and printed even at zero. A
  #   count of files that ASSERTED NOTHING is what makes the line above honest:
  #   without it a run in which a third of the corpus returned at its first
  #   `ifcapable` gate reads exactly like a run in which all of it executed.
  $led.Add("   of those, files that ASSERTED NOTHING ($($r.Inert.Count)): $($r.Inert -join ' ')")
  if ($r.FailNames.Count) { $led.Add("   failing test(s) seen here ($($r.FailNames.Count)): $(($r.FailNames.Keys | Sort-Object) -join ' ')") }
}
if ($calibration.Count) { $led.Add(""); $led.Add("== derivation calibration MISMATCH =="); foreach ($c in $calibration) { $led.Add("   $c") } }
if ($aborts.Count)     { $led.Add(""); $led.Add("== aborts =="); foreach ($a in $aborts) { $led.Add("   segment $($a.Segment): permutation '$(if ($a.Perm) { $a.Perm } else { '?' })' file '$(if ($a.File) { $a.File } else { '?' })' after test '$($a.LastTest)' ($(if ($a.KillReason) { "KILLED: $($a.KillReason)" } else { "rc=$($a.Rc)" })) -> $($a.Log)") } }
if ($notReached.Count) { $led.Add(""); $led.Add("== NOT REACHED (no verdict) =="); foreach ($n in $notReached) { $led.Add("   $n") } }
if ($hygiene.Count)    { $led.Add(""); $led.Add("== process hygiene =="); foreach ($h in $hygiene) { $led.Add("   $h") } }
if ($TierExcludes.Count) { $led.Add(""); $led.Add("== EXCLUDED by operator (DSS_TIER_EXCLUDES -> QUICKTEST_OMIT) =="); $led.Add("   $($TierExcludes -join ' ')") }
Set-Content -LiteralPath $Ledger -Value $led

# ── DID THE DECLARED CAPABILITIES REACH THE TESTS? ───────────────────────────
# The recipe assertions in the derive script proved each define reached the
# COMPILER. That is not the property being claimed. A capability can be compiled
# in and never exercised — which is the exact state this work was written
# against, where DSS compiled fts5.c on every run and every fts5 test file
# returned at its gate having asserted nothing. Each declared witness must have
# emitted at least one real result. The witnesses were chosen from the MEASURED
# inert set, so this gate was red before the capability set existed and goes
# green only by it.
if ($StageBuild -and $StageBuild.capabilityWitnesses -and $results.Count) {
  $inertUnion = @{}; foreach ($r in $results) { foreach ($f in $r.Inert)     { $inertUnion[$f] = $true } }
  $ranUnion   = @{}; foreach ($r in $results) { foreach ($f in $r.Completed) { $ranUnion[$f]   = $true } }
  $gaps = @(); $absent = @(); $checked = 0
  $declaredWitnesses = @($StageBuild.capabilityWitnesses.PSObject.Properties.Name)
  foreach ($cap in ($declaredWitnesses | Sort-Object)) {
    $file = "$($StageBuild.capabilityWitnesses.$cap.file).test"
    # A witness this tier never reached is NOT a capability gap — it is a file
    # outside this run's corpus, and calling it a gap would make the instrument
    # lie in the very direction it exists to prevent.
    # ★ IT IS NOT SILENT EITHER. ✔MEASURED on THIS driver 2026-08-07: fts5aa,
    #   rtree1 and session1 were absent from the Windows corpus entirely (the
    #   staged test dir had no sibling ext/, so sqlite's own
    #   `glob -nocomplain $testdir/../ext/…` matched nothing) and this gate
    #   announced "every declared capability reached the tests (7 witness
    #   file(s))" over three families that could not have run. Counting what was
    #   actually CHECKED is what stops that sentence being writable again.
    if (-not $ranUnion.ContainsKey($file)) { $absent += "$cap($file)"; continue }
    $checked++
    if ($inertUnion.ContainsKey($file)) { $gaps += "$cap($file)" }
  }
  if ($absent.Count) {
    Warn "[$LegTag] $($absent.Count) of $($declaredWitnesses.Count) capability witness(es) were NOT IN THIS RUN'S CORPUS, so nothing was proved about them: $($absent -join ' ')"
    Warn "      A witness file that never appears is not a passing witness. Either the tier does not"
    Warn "      include it, or the corpus this leg was handed is missing the directory it lives in."
  }
  if ($gaps.Count) {
    $script:CapabilityGaps += "$LegTag : $($gaps -join ' ')"
    Warn "[$LegTag] DECLARED CAPABILITIES DID NOT REACH THE TESTS — $($gaps -join ' ')"
    Warn "      Each of those files ran to completion and asserted NOTHING: it returned at its"
    Warn "      ``ifcapable`` gate. The define reached the compiler, so either the library was built"
    Warn "      without the capability the flag was supposed to enable, or the fixture linked"
    Warn "      objects from an older configuration. Reported at the end of the run."
  } else {
    # CHECKED-of-DECLARED, never just DECLARED.
    Pass "[$LegTag] every capability witness that was IN THIS CORPUS reached the tests — $checked of $($declaredWitnesses.Count) declared"
  }
}

$unitVerdict = ''; $unitFail = $false
if ($preconditionFail) {
  # ★ FIRST ARM, AND IT CARRIES THE DIAGNOSIS. The old engine would have landed in
  # the ABORT arm below and produced "N fixture ABORT(s) [veryquick/?]" — identical
  # rows naming an unnamed file — while sitting on the actual error. A harness that
  # says "the log named no resolvable corpus file" while holding "Can't find a
  # usable init.tcl" is withholding the diagnosis, and THAT, not the retrying, was
  # the expensive half. [D-HARNESS-ACQUIRED-TCL-DYLIB-HAS-NO-SCRIPT-LIBRARY]
  $unitVerdict = "FAIL: PRECONDITION FAILURE — the fixture completed ZERO test files in $($results.Count) consecutive segment(s), each dying with the same first diagnostic: $preconditionFail  (this is not a resumable crash; the remaining resume budget was NOT spent on it — see $runlog)"
  $unitFail = $true
  Warn "[$LegTag] corpus FAIL — PRECONDITION FAILURE, no unit of this leg's corpus ever ran."
  Warn "      $preconditionFail"
  Info "      $($results.Count) segment(s), $filesDone test file(s) completed ($filesInert of them asserted NOTHING), $resumes of $MaxResumes resume(s) used."
  Info "      per-unit ledger: $Ledger"
} elseif ($aborts.Count) {
  # An abort is itself a FAILURE. Resuming recovers the units behind it; it never
  # makes the abort disappear, and a run with aborts is NEVER green.
  $where = @(); foreach ($a in $aborts) { $where += "$(if ($a.Perm) { $a.Perm } else { '?' })/$(if ($a.File) { $a.File } else { '?' })" }
  $unitVerdict = "FAIL: $($aborts.Count) fixture ABORT(s) [$($where -join ' ')]; recovered by $resumes resume(s); union: $summaryText"
  if ($derivationText) { $unitVerdict += " [$derivationText]" }
  if ($real.Count)      { $unitVerdict += "; $($real.Count) genuine unit failure(s): $($real -join ' ')" }
  if ($notReached.Count) { $unitVerdict += "; $($notReached.Count) unit group(s) NOT REACHED — see $Ledger" }
  $unitFail = $true
  Warn "[$LegTag] corpus FAIL — $($aborts.Count) abort(s): $($where -join ' ')"
  Info "      union across $($results.Count) segment(s): $summaryText; $filesDone test file(s) completed ($filesInert of them asserted NOTHING)"
  if ($derivationText) { Info "        derived from: $derivationText" }
  if ($real.Count) { Info "      $($real.Count) UNCLASSIFIED failure(s) — not matched by any earned confound, NOT yet attributed to DSS: $($real -join ' ')" }
  foreach ($n in $notReached) { Warn "      NOT REACHED: $n" }
  Info "      per-unit ledger: $Ledger"
} elseif (-not $results[0].Summary) {
  $unitVerdict = "FAIL: fixture did not complete the suite (crash?) — see $runlog"; $unitFail = $true
  Warn "[$LegTag] corpus FAIL — no summary line (fixture crashed mid-suite); tail:"
  Get-Content $runlog -Tail 6 | ForEach-Object { Info "      $_" }
} elseif ($filesDone -eq 0) {
  # ★★ A RUN THAT COMPLETED ZERO TEST FILES IS NOT GREEN, WHATEVER ITS SUMMARY
  # LINE SAYS. MEASURED 2026-08-04 (TF-C116): with the corpus tier reduced to no
  # files, tester.tcl still finalises and prints `0 errors out of 1 tests`, and
  # this driver reported "corpus GREEN" beside "0 test file(s) completed" — a
  # FALSE PASS from a run that executed nothing. The cause that day was one
  # environment variable arriving EMPTY-BUT-SET through a cross-OS launcher, but
  # the defect is this branch's absence: a summary line was treated as proof that
  # a suite ran. The floor is structural and belongs here, not next to the cause.
  $unitVerdict = "FAIL: the fixture completed ZERO test files yet printed a summary ($summaryText) — a suite that ran nothing is not a pass; see $runlog"
  $unitFail = $true
  Warn "[$LegTag] corpus FAIL — 0 test file(s) completed, though the fixture printed '$summaryText'."
  Info "      A tier that selects no files still finalises and reports a summary. That is not a run."
  Get-Content $runlog -Tail 6 | ForEach-Object { Info "      $_" }
} elseif ($totalErrors -gt 0 -and $failNames.Count -eq 0) {
  $unitVerdict = "FAIL: $totalErrors error(s) but no failure markers ('Failures on these tests:' / '! <name>') to classify — see $runlog"; $unitFail = $true
  Warn "[$LegTag] corpus FAIL — $summaryText (unclassifiable — no failure markers)"
} elseif ($real.Count -eq 0) {
  $unitVerdict = "PASS ($summaryText$(if ($confound.Count) { "; $($confound.Count) known confound(s): $($confound -join ' ')" }))"
  Pass "[$LegTag] corpus GREEN — $summaryText$(if ($confound.Count) { "; all $($confound.Count) failure(s) are known non-DSS confounds: $($confound -join ' ')" })"
} else {
  $unitVerdict = "FAIL: $($real.Count) genuine unit failure(s): $($real -join ' ')"; $unitFail = $true
  Warn "[$LegTag] corpus FAIL — $summaryText; $($real.Count) UNCLASSIFIED failure(s) — run each against the gcc reference fixture before charging it to DSS: $($real -join ' ')"
  if ($confound.Count) { Info "      (+$($confound.Count) known confound(s) ignored: $($confound -join ' '))" }
}
# ★ ONE call for EVERY failing branch above, deliberately placed after the chain
# rather than inside the branch that happened to motivate it: an ABORT and a
# ZERO-FILES run need the prior control every bit as much as a genuine failure,
# and a lookup wired into one branch is a lookup that silently does not run for
# the other four. It costs nothing on the green path.
if ($unitFail) { Show-RegistryControls $LegTag $real }
# A killed zombie / stolen stale lock is a fact about THIS run, not a footnote — it
# rides on the verdict even when the corpus itself came back clean.
if ($hygiene.Count) {
  $unitVerdict += "  [PROCESS HYGIENE: $($hygiene.Count) event(s) — $($hygiene -join '; ')]"
  foreach ($h in $hygiene) { Warn "[$LegTag] HYGIENE: $h" }
}
# A NOT-REACHED unit is a coverage hole even when nothing failed — never silent.
if ($notReached.Count -and -not $aborts.Count) {
  $unitVerdict += "  [NOT FULL COVERAGE: $($notReached.Count) unit group(s) NOT REACHED — see $Ledger]"
  $unitFail = $true
  foreach ($n in $notReached) { Warn "[$LegTag] NOT REACHED: $n" }
}
# The exclusion rides along on EVERY verdict — pass and fail alike — so a GREEN
# line can never be read as "the whole corpus ran".
if ($TierExcludes.Count) {
  $unitVerdict += "  [NOT FULL COVERAGE: $($TierExcludes.Count) file pattern(s) EXCLUDED from the $Tier tier via QUICKTEST_OMIT -- $($TierExcludes -join ' ')]"
  Warn "[$LegTag] the verdict above covers a REDUCED corpus: $($TierExcludes -join ' ') excluded from every `$allquicktests-derived permutation."
}
# <<< dss:corpus-loop <<<

# ── this leg's row in the ledger ─────────────────────────────────────────────
# The VERDICT is `ran`: the leg produced an assertion, which is exactly what the
# closed vocabulary means by verified. Whether that assertion PASSED is a
# separate fact ($unitFail), carried separately and folded into the exit code by
# Step 9 — the same separation ArmVerdictLedger draws between a verdict and a
# test result. A leg that ran and failed is `1 verified … 0 poisoned` plus a
# non-zero exit, not a vanished leg.
$legRec.Verdict     = 'ran'
$legRec.Detail      = "$LegRunMode$(if ($legLauncher.Count) { " via '$($legLauncher -join ' ')'" })$(if ($legXlate -and $legXlate -ne 'none') { " [paths -> $legXlate]" }) — $unitVerdict"
$legRec.UnitVerdict = $unitVerdict
$legRec.UnitFail    = $unitFail
$legRec.Ledger      = $Ledger
# The per-leg detail Step 9 reprints. Only populated when there is something to
# say, so a clean single-segment run leaves the summary byte-identical to what it
# always was.
$rep = New-Object 'System.Collections.Generic.List[string]'
if ($results.Count -gt 1 -or $aborts.Count -or $notReached.Count -or $hygiene.Count) {
  [void]$rep.Add("segments : $($results.Count) ($resumes resume(s) of max $MaxResumes)   $filesDone test file(s) completed ($filesInert asserted NOTHING)   ledger: $Ledger")
  if ($derivationText) { [void]$rep.Add("counts   : $derivationText") }
  foreach ($a in $aborts) { [void]$rep.Add("aborted  : permutation '$(if ($a.Perm) { $a.Perm } else { '?' })' file '$(if ($a.File) { $a.File } else { '?' })'$(if ($a.KillReason) { " [KILLED: $($a.KillReason)]" }) — its remaining cases did NOT run  ($($a.Log))") }
  foreach ($n in $notReached) { [void]$rep.Add("NOT RUN  : $n") }
  foreach ($h in $hygiene)    { [void]$rep.Add("hygiene  : $h") }
}
$legRec.Report = $rep
}
# ── end of the per-leg run loop ──────────────────────────────────────────────

# ── Step 9 — results ─────────────────────────────────────────────────────────
Step '9/9  Results'
# $dssDivergeNote is the Step-2 measurement, reprinted verbatim so the opening
# banner and the closing verdict cannot disagree about the same run.
Info "compiler : $DssBin @ $dssHead$dssDivergeNote"
Info "sqlite   : $SqliteWslDir @ $sqliteHead   (staged: $Stage)"
Info "recipe   : $nTus TUs, $nDefs defines"
# The ATTRIBUTION ORACLE, surfaced where a human triaging a failure will actually
# see it. Its ABSENCE is printed too, and loudly: a missing oracle is the difference
# between attributing a corpus failure and arguing about it, so it must never be
# silent. Step 3+4 preserves this copy OUT of the make target's path — see "the
# reference build, and the PRESERVED oracle" there.
# ★ THE ONE-SIDEDNESS IS PART OF THE VERDICT, not a footnote. This fixture is a
#   LINUX/gcc ELF built by the deriving host's own toolchain. Against a leg of a
#   DIFFERENT platform (pe64, mach-o) it can EXONERATE (it fails too → upstream,
#   not DSS) but it can NEVER convict (it passes → could still be a platform/CRT
#   difference; fpconv1-2.0 is the standing example). Do not "tidy" that sentence
#   away — a reader who takes an oracle pass as proof of a DSS bug will chase a
#   defect that is not there.
#   ⚠ AND NOTE THE ASYMMETRY BETWEEN LEGS, which is new as of TF-C114: for an
#   elf64 Linux leg this same binary is very nearly a MATCHED control (same OS,
#   same object format, differing only in the compiler), i.e. a much stronger
#   instrument than it is for pe64 or mach-o. Neither claim has been MEASURED on
#   this driver, so the line below states what the oracle IS and leaves the
#   strength of the inference to the leg being triaged.
if ($RefOracle) {
  Info "oracle   : $RefOracleWin"
  Info "             LINUX/gcc reference testfixture (ELF, built by the deriving host). Run it on"
  # `-e`, not `--`: this line is ADVICE AN OPERATOR PASTES, so it must be the
  # shape that survives. MEASURED 2026-08-04 — `wsl.exe -- <argv>` still routes
  # through the distro's default shell (`wsl.exe -- /nope` answers `/bin/bash:
  # line 1: /nope: No such file`, `wsl.exe -e /nope` answers `execvpe(/nope)
  # failed`), so a staged path with a glob character or a `$` would be rewritten
  # under the operator mid-triage.
  Info "             the same .test to EXONERATE dss:   $(if ($script:HostNeedsWsl) { "wsl.exe -e $RefOracle" } else { $RefOracle }) <staged .test>"
  Info "             It fails too => upstream/test-suite. It passes => INCONCLUSIVE for a leg of a"
  Info "             different platform (pe64 / mach-o); never proof of a dss bug on its own."
} else {
  Warn "oracle   : ABSENT — no reference fixture survived this run, so a corpus failure"
  Warn "             cannot be attributed to DSS vs upstream. $RefOracleMiss"
}
Info "tier     : $Tier.test   outputs: $Work"
Info "excluded : $(if ($TierExcludes.Count) { "$($TierExcludes -join ' ')   (operator DSS_TIER_EXCLUDES -> QUICKTEST_OMIT; dropped from every `$allquicktests-derived permutation, still run under 'full')" } else { '(none — the full tier ran)' })"

# ── the LEG LEDGER — every DECLARED leg, one row, a NAMED verdict ────────────
# ★ THE ACCOUNTING IS THE DELIVERABLE. This is
# ArmVerdictLedger::renderCountsLine()'s idiom, in its vocabulary, for the same
# reason it exists there: "N skipped" collapses four different facts into one and
# turns a gate back into a log. Every class is named, every declared leg is
# counted, and if the classes do not SUM to the declared total the line says so
# ABOUT ITSELF rather than quietly printing a breakdown that does not add up.
# ★ `skipped-launcher-prerequisite-missing` IS ENVIRONMENTAL, beside
# `skipped-emulator-missing`, and it has an arm here because it did NOT:
# harness_legs.py added the token to the closed vocabulary while this `switch` is a
# HARDCODED MIRROR, so a leg carrying it fell into `default` -> $vUnclassified and
# printed as "★ LEDGER ACCOUNTING HOLE" — the ledger reporting a harness defect
# about a leg that had been correctly classified. Its .sh twin's LEDGER_VOCAB did
# the same thing via LEDGER_BOGUS. Both fixed together; a fix in one driver and not
# the other is this project's canonical silent harness bug.
$vRan = 0; $vExpect = 0; $vByRunOn = 0; $vNoEmu = 0; $vEmuMissing = 0
$vLauncherPrereq = 0
$vInputMissing = 0; $vNotSelected = 0; $vPoisoned = 0; $vUnclassified = @()
foreach ($lbl in $LegOrder) {
  switch ("$($LegLedger[$lbl].Verdict)") {
    'ran'                          { $vRan++ }
    'expect-error-asserted'        { $vExpect++ }
    'skipped-by-runOn'             { $vByRunOn++ }
    'skipped-no-emulator-declared' { $vNoEmu++ }
    'skipped-emulator-missing'     { $vEmuMissing++ }
    'skipped-launcher-prerequisite-missing' { $vLauncherPrereq++ }
    'skipped-build-input-missing'  { $vInputMissing++ }
    'not-selected-by-runner'       { $vNotSelected++ }
    'poisoned'                     { $vPoisoned++ }
    default                        { $vUnclassified += $lbl }
  }
}
$verified   = $vRan + $vExpect
$structural = $vByRunOn + $vNoEmu
$environmental = $vEmuMissing + $vLauncherPrereq + $vInputMissing
$skipped    = $structural + $environmental + $vNotSelected
$accounted  = $verified + $skipped + $vPoisoned
$countsLine = "$verified verified ($vRan ran, $vExpect expect-error), $skipped skipped [structural: $vByRunOn by-runOn, $vNoEmu no-emulator-declared; environmental: $vEmuMissing emulator-missing, $vLauncherPrereq launcher-prerequisite-missing, $vInputMissing build-input-missing; harness: $vNotSelected not-selected], $vPoisoned poisoned  (of $($AllLegs.Count) declared legs)"
if ($accounted -ne $AllLegs.Count) {
  # The runtime twin of renderCountsLine()'s accounting-hole branch: a breakdown
  # that does not sum to its own denominator reads like full accounting and is
  # worse than none. It announces itself here rather than waiting to be noticed.
  $countsLine += "  ★ LEDGER ACCOUNTING HOLE: $accounted of $($AllLegs.Count) legs fall in a reported class — the rest belong to NO class and have VANISHED from this line$(if ($vUnclassified.Count) { " (unclassified: $($vUnclassified -join ' '))" }) (D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT)"
}
Info "legs     : $countsLine"
# One detail line per NON-VERIFIED leg, with its reason. Verified legs are omitted
# from THIS list — they are already accounted for above and reported in full below.
foreach ($lbl in $LegOrder) {
  $r = $LegLedger[$lbl]
  if ($r.Verdict -eq 'ran' -or $r.Verdict -eq 'expect-error-asserted') { continue }
  Info "  [$($r.Verdict)] $lbl spec=$($r.Spec)$(if ($r.Built) { ' (BUILT)' }) — $($r.Detail)"
}
# …and the full report for every leg that DID run.
foreach ($lbl in $LegOrder) {
  $r = $LegLedger[$lbl]
  if ($r.Verdict -ne 'ran') { continue }
  foreach ($ln in @($r.Report)) { Info "  [$lbl] $ln" }
  Info "$($lbl) ($($r.Spec)): compiled   units: $($r.UnitVerdict)"
}
if ($FilteredOut.Count) {
  Warn "coverage : DSS_LEGS restricted this run to $($Legs.Count) of $($AllLegs.Count) declared legs — NOT EXERCISED: $($FilteredOut -join ' ')"
}
# Release the run lock. Correctness does NOT depend on this — the lock is
# liveness-based, so a run that dies here just leaves one the next invocation
# steals and reports. Releasing simply keeps that report quiet when it should be.
Remove-Item -Recurse -Force $LockDir -ErrorAction SilentlyContinue

# ── THE sqlite3 CLI, PER LEG — a SECOND artifact needs a SECOND ledger line ──
# ★ ONE LINE PER DECLARED LEG, ALWAYS. The fixture ledger above is keyed on the
# fixture's outcome and cannot express "the fixture built and the CLI did not"
# (or the reverse, which is reachable: the CLI needs zlib and NOT Tcl). A reader
# must see both artifacts' fate for every leg without inferring either from the
# other, and a leg that produced no line at all is the silent shortfall the whole
# ledger exists to prevent.
Write-Host "   --- sqlite3 CLI (full TU: shell.c + the library TUs recovered from libsqlite3.a) ---"
$SelectedLabels = @($Legs | ForEach-Object { $_.label })
foreach ($lg in $AllLegs) {
  $lbl = $lg.label
  $v = Get-DssArtifactVerdict $lbl 'sqlite3'
  if ($CliBuilt.ContainsKey($lbl)) {
    Write-Host "   $($lbl.PadRight(14)) ($($lg.spec)): built   smoke: $(if ($CliSmokeVerdict.ContainsKey($lbl)) { $CliSmokeVerdict[$lbl] } else { '<NO SMOKE VERDICT>' })"
  } elseif (-not $v) {
    # ★ THE HARNESS KNOWS WHICH, SO IT SAYS WHICH. This line used to print
    # "not processed [not-selected-by-runner or not buildable]" — an OR between
    # two facts the driver can distinguish perfectly well, one of which is a
    # normal operator choice and the other of which is a harness bug. Printing
    # the OR is how the second one hides inside the first.
    if ($SelectedLabels -notcontains $lbl) {
      Write-Host "   $($lbl.PadRight(14)) ($($lg.spec)): not processed [not-selected-by-runner] — DSS_LEGS='$($env:DSS_LEGS)' did not select this leg" -ForegroundColor Yellow
    } else {
      Write-Host "   $($lbl.PadRight(14)) ($($lg.spec)): >> NO CLI VERDICT — this leg WAS selected and the CLI loop still recorded nothing for it. That is a harness bug; the ledger check below fails the run on it." -ForegroundColor Red
    }
  } else {
    Write-Host "   $($lbl.PadRight(14)) ($($lg.spec)): NOT BUILT [$($v.Verdict)] — $($v.Detail)" -ForegroundColor Yellow
  }
}
# ── THE CLI VERDICT-COMPLETENESS GUARD — the INSTRUMENT, actually wired in ───
# ★ Assert-DssArtifactVerdicts SHIPPED WITH ZERO CALL SITES, in both cores, with
# a docstring calling itself "the inert-instrument guard: a ledger nobody filled
# in must never read as clean". It is the check that would have caught both of
# the defects found beside it — a CLI loop silently gated on Tcl, and a run that
# printed "BUILT on 0 of 5 declared leg(s)" and exited 0.
#
# OVER THE SELECTED LEGS, not the declared ones: a leg DSS_LEGS filtered out was
# never asked for and correctly has no verdict, while a SELECTED leg with none is
# a harness bug. base-harness.sh's dss_bh_assert_verdicts is the .sh twin and is
# wired at the same point in that driver.
$CliLedgerHoles = @(Assert-DssArtifactVerdicts 'sqlite3' $SelectedLabels)

# ── the exit code ────────────────────────────────────────────────────────────
$failReasons = @()
# ★ THE CLI IS PART OF THE RUN'S VERDICT, NOT AN EXTRA. A CLI that failed to
# build, or whose smoke gate went red, fails the run exactly as a fixture failure
# does — otherwise "we can run all sqlite3 CLI in any host" would be a claim
# nothing enforces. The two counters stay apart so the message says WHICH half.
if ($CliFails -gt 0)      { $failReasons += "$CliFails leg(s) did not produce a sqlite3 CLI — each one's reason is on its CLI ledger line above; where a compile was actually attempted the diagnostics are in $OutRoot\<leg>\cli\compile.log" }
if ($CliSmokeFails -gt 0) { $failReasons += "$CliSmokeFails leg(s) failed the sqlite3 CLI SMOKE GATE — inspect $OutRoot\<leg>\cli-smoke\smoke.log. Each leg's line above says whether it was CHARGED TO DSS or exonerated against the gcc reference; exonerated is still red." }
# A SELECTED leg with no CLI verdict at all — the ledger cannot say what happened
# to an artefact this run declared, which is a HARNESS defect and never a result.
if ($CliLedgerHoles.Count) {
  $failReasons += "the sqlite3 CLI ledger does not account for every SELECTED leg — no verdict was ever recorded for: $($CliLedgerHoles -join ' '). Every selected leg must reach a named CLI verdict (built / poisoned / skipped-build-input-missing); silence about one is a harness bug."
}
# ★ AND A RUN THAT PRODUCED NO CLI AT ALL IS NEVER GREEN. With the CLI loop
# previously gated on Tcl, an empty list meant Step 7b never ran, $CliFails and
# $CliSmokeFails both stayed 0, no fail reason was raised — and the closing line
# announced "BUILT on 0 of 5 declared leg(s); the smoke gate passed on 0" on the
# way to exit 0. The loop is fixed above; this is the assertion that says so out
# loud, so the shape cannot come back by a different route.
if ($Legs.Count -gt 0 -and $CliBuilt.Count -eq 0) {
  $failReasons += "NOT ONE sqlite3 CLI was built, on any of the $($Legs.Count) selected leg(s). A zero-artefact run proves nothing and must not exit 0 — each leg's reason is on its CLI ledger line above."
}
if ($vPoisoned -gt 0) {
  $failReasons += "$vPoisoned leg(s) POISONED: $(@($LegOrder | Where-Object { $LegLedger[$_].Verdict -eq 'poisoned' }) -join ' ')"
}
# NAMED SEPARATELY from the poisoned count above, which it is a subset of: a leg
# that built its fixture and then could not stage the shared object the corpus
# dlopen()s ran NONE of its units, and the summary has to say that rather than
# leaving a reader to infer it from a bare "POISONED".
if ($LoadextStageFails -gt 0) {
  $failReasons += "$LoadextStageFails leg(s) BUILT their testfixture but could not stage the loadext helper the corpus dlopen()s - their units did NOT run. Each one is named above with the exact reason and its $OutRoot\<leg>\loadext-helper\ logs."
}
$unitFailLegs = @($LegOrder | Where-Object { $LegLedger[$_].UnitFail })
if ($unitFailLegs.Count) { $failReasons += "$($unitFailLegs.Count) leg(s) with genuine unit failures: $($unitFailLegs -join ' ')" }
if ($accounted -ne $AllLegs.Count) { $failReasons += "the leg ledger does not account for every declared leg ($accounted of $($AllLegs.Count))" }
# ★ AN UNCLASSIFIED NON-VERIFICATION IS ITS OWN FAIL REASON, and it is NOT the
# accounting-hole line above. That line (correctly) also fires, but it names the
# wrong thing: "the ledger does not account for a leg" sends a reader to the leg
# plan, when what happened is that a leg was recorded not-run under a token this
# run could not classify. Reporting it separately is the difference between a
# diagnosis and a symptom (D-HARNESS-UNITS-SKIP-A-LEG-WHOSE-LAUNCHER-IT-SAYS-IS-AVAILABLE).
if ($UnclassifiedVerdicts.Count) {
  $failReasons += "$($UnclassifiedVerdicts.Count) non-verification(s) carried a verdict token this driver could not classify: $(@($UnclassifiedVerdicts | Select-Object -Unique) -join ' '). Each is warned above with what the driver DID say. This is a HARNESS defect, not a compiler result: a not-run that names no class cannot be counted as structural, environmental or harness, so the leg would vanish from the accounting while the summary still read as coverage. The closed vocabulary is: $($VerdictVocabulary -join ' ')"
}
# STRUCTURAL skips are reported, NEVER fatal — nothing about this machine could
# change them. ENVIRONMENTAL skips warn by default and are FATAL under
# DSS_STRICT_ARM_VERDICTS=1, exactly as arm_verdict_ledger.hpp specifies: a
# developer without the target's tcl runtime must still get a usable run, while
# the gate opts in to demanding every declared input be present.
$EnvironmentalVerdicts = @('skipped-emulator-missing','skipped-launcher-prerequisite-missing','skipped-build-input-missing')
$envSkipLegs = @($LegOrder | Where-Object { $EnvironmentalVerdicts -contains $LegLedger[$_].Verdict })
if ($envSkipLegs.Count) {
  if ($StrictVerdicts) {
    $failReasons += "DSS_STRICT_ARM_VERDICTS=1 and $($envSkipLegs.Count) leg(s) were skipped for an ENVIRONMENTAL reason: $($envSkipLegs -join ' ')"
  } else {
    Warn "$($envSkipLegs.Count) leg(s) skipped for an ENVIRONMENTAL reason (this machine could supply what the catalogue declared, and did not): $($envSkipLegs -join ' ')"
    Warn "      set DSS_STRICT_ARM_VERDICTS=1 to make that a hard failure. It is NOT fatal by default so a developer missing one target's runtime still gets a usable run."
  }
}
# ★ ZERO VERIFIED + AT LEAST ONE ENVIRONMENTAL SKIP IS A FAILURE, and this rule is
# what carries the pre-TF-C114 loudness across the generalisation. Before this
# cycle a missing tcl86.dll was a hard `Die`; with five legs and a per-leg
# `skipped-build-input-missing` it would otherwise have become "0 verified, 5
# skipped, exit 0" — a broken machine reporting success, which is precisely the
# "449 of 557 silently skipped and counted as passes" defect arm_verdict_ledger.hpp
# was written for. The environmental clause is what keeps it honest in the other
# direction: on a host where every non-verification is STRUCTURAL (a riscv64 Linux
# box, say) nothing about the machine could have done better, the legs whose
# inputs WERE present are still built, and that is not a failure.
if ($verified -eq 0) {
  if ($environmental -gt 0) {
    $failReasons += "NO declared leg reached a VERIFIED verdict, and $environmental non-verification(s) are ENVIRONMENTAL — this machine could have produced evidence and did not. A run that proves nothing must not exit 0"
  } else {
    Warn "no declared leg reached a VERIFIED verdict on this host — every non-verification is STRUCTURAL (nothing about this machine could change it). Builds were still attempted; this run proves nothing about EXECUTION on any target."
  }
}
if ($failReasons.Count) {
  Write-Host ""
  foreach ($f in $failReasons) { Write-Host " [X] $f" -ForegroundColor Red }
  Write-Host " [X] sqlite harness FAILED — $countsLine" -ForegroundColor Red
  exit 1
}
Pass "sqlite harness GREEN — $countsLine"
Pass "$verified leg(s) compiled the full-source testfixture + ran the $Tier unit corpus GREEN"
# The CLI's own closing claim, bounded the same way: "built" and "ran the 14
# assertions" are different facts, and a cross leg with no launcher on this host
# legitimately reaches only the first.
$cliSmoked = @($CliSmokeVerdict.Keys | Where-Object { "$($CliSmokeVerdict[$_])" -like 'PASS*' }).Count
# ★ "THE REST" IS COUNTED OFF THE BUILT LEGS, NOT OFF THE DECLARED ONES. It used
# to be $AllLegs.Count − $cliSmoked described as "built but not executable on
# this host", which silently absorbed every leg that was never built at all —
# contradicted by this driver's own CLI ledger a few lines earlier. A leg that
# did not build is not a leg this host could not run.
Pass "sqlite3 CLI: BUILT on $($CliBuilt.Count) of $($AllLegs.Count) declared leg(s); the 14-assertion smoke gate passed on $cliSmoked (of the $($CliBuilt.Count) built, $($CliBuilt.Count - $cliSmoked) were NOT executed here — each named above; the $($AllLegs.Count - $CliBuilt.Count) that did not build are named there too)"
# ★ A CAPABILITY GAP FAILS THE RUN, and it fails it HERE rather than mid-leg so
#   the other legs still produce their verdicts — one stage defect must not cost
#   four legs' results. It is a HARNESS failure, never a DSS one: the compiler
#   built what it was handed, and what it was handed was wrong.
if ($script:CapabilityGaps.Count) {
  foreach ($g in $script:CapabilityGaps) { Warn "capability gap — $g" }
  Die "the run built a sqlite that does NOT have capabilities this harness declares.`n      Every gap above is a test file that completed and asserted nothing, for a capability`n      legs.json stageBuild names explicitly. The corpus totals above are therefore an`n      OVERSTATEMENT of coverage: those files are counted as completed.`n      [D-HARNESS-CORPUS-FILES-COMPLETE-WITHOUT-ASSERTING-BECAUSE-CAPABILITIES-ARE-OFF]"
}
exit 0
