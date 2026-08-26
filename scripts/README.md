# `scripts/` — every script this repository ships

**Look here before writing a script.** Most of what a cycle needs already exists,
and the cost of not knowing that is paid twice: once re-implementing a tool that
was already here, and again when the re-implementation carries the edge case the
original had already been taught (quoting, path mangling, CRLF, locale, a remote
that drops `PATH`).

## The rule that matters most

★★★ **If a script has a problem, FIX THE SCRIPT — never work around it.**
A workaround at the call site leaves the defect in place for the next caller, and
this repository's shared scripts exist precisely to hold the edge cases that keep
biting: `wsl.exe` quoting, heredocs eating backslashes, `/mnt/c` clock skew,
`rsync` excludes that must be anchored, non-interactive ssh dropping
`/opt/homebrew/bin`. A workaround re-opens every one of them.

## Layout

One directory per script, named for the script, with every sibling
implementation inside it:

```
scripts/<name>/<name>.sh      the POSIX implementation
scripts/<name>/<name>.ps1     the Windows sibling
scripts/<name>/<name>.py      a Python implementation or shared core
scripts/<name>/…              that script's own assets (configs, fixtures, examples)
```

The **primary** script — the one the guard reads, and the one to invoke — is
`<name>.sh`, else `<name>.py`, else `<name>.ps1`. Several scripts are
**capability-paired**: a change to one sibling lands in the other in the same
commit, or the pair is broken.

## The index

Each script declares its purpose once, in a `PURPOSE:` line in its own header.
The table below is **generated from those declarations** and verified against
them by the `scripts_index_guard` ctest entry — so a script added, renamed,
deleted, or repurposed without updating this file is a **red gate**, not a stale
document.

```bash
python scripts/check-scripts-index/check-scripts-index.py --write
```

<!-- BEGIN GENERATED SCRIPT INDEX -->
| Script | Runs as | Purpose |
| --- | --- | --- |
| **`burndown-queue`** | `burndown-queue.py` | re-derive the prioritized burndown queue from the registry, production errors first. |
| **`carriage-excludes`** | `carriage-excludes.py` | emit the transport exclude list for a gate carriage, derived from what git ignores rather than re-typed once per carriage. |
| **`check-anchor-balance`** | `check-anchor-balance.py` | refuse a cycle that ends with more OPEN deferral-registry rows than it began. |
| **`check-anchor-registry`** | `check-anchor-registry.ps1`, `check-anchor-registry.sh` | refuse a `D-*` anchor cited in a scanned root that resolves to no registry row, and refuse a markdown table row whose unescaped pipes would silently drop cells. |
| **`check-carriage-paths`** | `check-carriage-paths.py` | refuse a carriage script whose repository path disagrees with the project's own declared name. |
| **`check-diagnostic-codes`** | `check-diagnostic-codes.py` | refuse a duplicate, implicitly-numbered, or newly-uncovered `DiagnosticCode` ordinal. |
| **`check-enum-name-table-guards`** | `check-enum-name-table-guards.py` | refuse an `EnumNameTable` vocabulary declared in `src/` without a `DSS_CHECK_ENUM_NAME_TABLE` well-formedness assert. |
| **`check-guard-output-encoding`** | `check-guard-output-encoding.py` | refuse a Python script whose report cannot carry a non-cp1252 character through a pipe. |
| **`check-line-endings`** | `check-line-endings.ps1`, `check-line-endings.sh` | refuse a tracked text blob that carries a CR. |
| **`check-ninja-deps`** | `check-ninja-deps.py` | refuse a gate over a build directory whose objects recorded no header dependencies. |
| **`check-no-abort-in-tests`** | `check-no-abort-in-tests.py` | refuse a new live `abort()` call site in test or test-support code. |
| **`check-orphan-tests`** | `check-orphan-tests.ps1`, `check-orphan-tests.sh` | refuse a test source that no CMake target compiles and no ctest entry runs. |
| **`check-path-identity`** | `check-path-identity.py` | refuse a second path canonicalizer -- path resolution lives in exactly one place. |
| **`check-plan-citations`** | `check-plan-citations.py` | refuse a new `path:line` citation in the plans -- a citation names a stable reference, never a line number. |
| **`check-retyped-closed-sets`** | `check-retyped-closed-sets.py` | census the diagnostics that RETYPE a closed vocabulary instead of projecting it. |
| **`check-scripts-index`** | `check-scripts-index.py` | refuse a script that no index documents, and an index entry that no script backs. |
| **`check-shell-portability`** | `check-shell-portability.py` | refuse a tracked shell script that cannot run on bash 3.2 without declaring it. |
| **`check-stale-refusal-citations`** | `check-stale-refusal-citations.py` | refuse a new present-tense refusal sentence that cites an anchor row already marked CLOSED. |
| **`check-wall-clock-in-tests`** | `check-wall-clock-in-tests.py` | refuse a new wall-clock duration literal in test code outside the shared measured budget. |
| **`check-wrapped-anchor-ids`** | `check-wrapped-anchor-ids.py` | refuse a NEW anchor id split across a line break, which no grep can ever return. |
| **`cmake-import`** | `cmake-import.ps1`, `cmake-import.py`, `cmake-import.sh` | convert a CMake project into a DSS `.dss-project.json` manifest. |
| **`compile-bench`** | `compile-bench.py`, `compile-bench.sh` | time dsscp against gcc/clang/MSVC/tcc on ONE host over a subject size ladder, naming every reference it could not find. |
| **`corpus-census`** | `corpus-census.ps1`, `corpus-census.py`, `corpus-census.sh` | census the real-example corpus into a run-identified report instead of one overwritten log. |
| **`examples-census`** | `examples-census.py` | re-derive every corpus-manifest figure examples/README.md states, by parsing the manifests. |
| **`leg-tree`** | `leg-tree.sh` | put a gate host's own clone on the tree under test before a leg, and restore it to pristine afterwards. |
| **`local-build`** | `local-build.ps1`, `local-build.sh` | build dsscp incrementally on this host, and optionally run ctest. |
| **`macho-alias-ld64-matrix`** | `macho-alias-ld64-matrix.remote.sh`, `macho-alias-ld64-matrix.sh` | measure what Apple's ld64 does with a SECOND defined symbol at the same address as a canonical one, with and without -dead_strip. |
| **`macos-leg`** | `macos-leg.ps1`, `macos-leg.sh` | run a DSS gate leg on the operator's macOS host -- push the tree, build clean, run ctest. |
| **`pragma-profile-census`** | `pragma-profile-census.ps1`, `pragma-profile-census.py`, `pragma-profile-census.sh` | census `#pragma` usage across the corpus and hold the profile to its expected shape. |
| **`profile-compile`** | `profile-compile-dispatch.sh`, `profile-compile-support.py`, `profile-compile.sh` | compile one fixed subject with a RELEASE dsscp on this host and report where the time went, so the HOST is the only variable across legs. |
| **`refresh_landing_log`** | `refresh_landing_log.py`, `test_refresh_landing_log.py` | regenerate the PR landing-log hash anchors in the plans from git log. |
| **`remote-leg`** | `remote-leg.sh` | run a DSS gate leg on a physical remote host -- push the working tree over a carriage, build clean, and run ctest through run-gate. |
| **`run-gate`** | `run-gate.ps1`, `run-gate.sh` | run a gate command and REFUSE to report success without evidence that it ran. |
| **`sqlite-runtime-bench`** | `sqlite-runtime-bench.py` | measure the RUNTIME of an emitted sqlite3 binary, the standing runtime-differential instrument. |
| **`ssh-arm64-vps`** | `ssh-arm64-vps.ps1`, `ssh-arm64-vps.sh` | reach the native aarch64 Linux VPS (the carriage; WSL only). |
| **`ssh-macos`** | `ssh-macos.ps1`, `ssh-macos.sh` | reach the operator's physical macOS host (the carriage). |
| **`wsl-leg`** | `wsl-leg.sh` | run a DSS gate leg inside WSL -- sync the Windows checkout, build clean, and run ctest through run-gate. |
<!-- END GENERATED SCRIPT INDEX -->

## Which of these run in CI

`check-anchor-registry`, `check-line-endings` and `check-orphan-tests` are wired
into `ctest` (as `anchor_registry_guard`, `line_endings_guard` and
`orphan_tests_guard`) and run on every gate, on every host. ⚠ **One spelling per
host, not both**: `CMakeLists.txt` dispatches the `.ps1` on Windows and the `.sh`
everywhere else, so a change to one sibling is only ever exercised by the hosts
that run it — which is why the pair is required to stay behaviourally identical
rather than merely to exist. `check-scripts-index` joins them as
`scripts_index_guard`, as a single Python implementation with no twin.
The remaining `check-*` scripts are run by the development cycle's gate battery
rather than by ctest.

## Test parallelism

`run-gate` and `local-build` default `CTEST_PARALLEL_LEVEL` to **8** when the
environment does not already set it. An explicit `-j` on a ctest command line
still wins, so this is a default and not a policy. ✔MEASURED 2026-08-19 on a
16C/32T host: six example tests took 9741 ms with no level given and 2648 ms at
8 — the whole suite had been running one test at a time because nothing ever
supplied a level.
