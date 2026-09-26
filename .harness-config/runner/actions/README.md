# `.harness-config/runner/actions/` — every program this repository ships

**Look here before writing a script.** Most of what a cycle needs already exists,
and the cost of not knowing that is paid twice: once re-implementing a tool that
was already here, and again when the re-implementation carries the edge case the
original had already been taught (quoting, path mangling, CRLF, locale, a remote
that drops `PATH`).

★★★ **This directory replaced `scripts/` and `real-examples/` on 2026-09-18.** Every
program is a DssHarness ACTION: one directory per program, holding its `<name>.yml`
and every file it runs, started by `dssharness run <name>` on the legs its runner
declares under `predefinedRunners` in `.harness-config/config.json`. The gate itself
still runs each guard through its ctest entry, on every leg.

## The rule that matters most

★★★ **If a program has a problem, FIX THE PROGRAM — never work around it.**
A workaround at the call site leaves the defect in place for the next caller, and
this repository's shared programs exist precisely to hold the edge cases that keep
biting: `wsl.exe` quoting, heredocs eating backslashes, `/mnt/c` clock skew, a
non-interactive ssh dropping `/opt/homebrew/bin`. A workaround re-opens every one.

## Layout

One directory per action, named for the action, with every file it runs inside it:

```
<name>/<name>.yml     the action: its steps, the witness each prints, a PURPOSE: comment
<name>/<name>.py      the program (the primary, when present) or a shared core
<name>/…              that program's own assets (inventories, fixtures, examples)
<group>/…/<name>/     a GROUPED action — real-examples/c/sqlite is the action `sqlite`
```

The **primary** program — the one to invoke by hand — is `<name>.py`; an action may
ship none of that name (the sqlite harness's driver is `build_and_test.py`).
★★★ **No `.sh` and no `.ps1` anywhere under this directory** — operator ruling,
2026-09-21: *"entrypoint is .yml, you can call .py files, BUT NOT .sh/.ps1 please. They
are specific per OS."* Every step starts `python3 <file>.py`, so one program runs
unchanged on every leg its runner declares, with no twin to keep in step;
`check-scripts-index` refuses a `.sh` or `.ps1` here by name.
DssHarness reserves `build/` (a run's working space) and `artifacts/` (what a run
keeps) inside every action; both are gitignored by the tool's managed block.

## The index

Each action declares its purpose once, in a `PURPOSE:` comment line in its own
`<name>.yml`. The table below is **generated from those declarations** and verified
against them by the `scripts_index_guard` ctest entry — so an action added, renamed,
deleted, or repurposed without updating this file, or one no runner can start, is a
**red gate**, not a stale document.

```bash
python .harness-config/runner/actions/check-scripts-index/check-scripts-index.py --write
```

<!-- BEGIN GENERATED ACTION INDEX -->
| Action | Runs | Purpose |
| --- | --- | --- |
| **`anchors`** | `anchors.py` | read and lint deferred-anchor registry rows, and launch the one door that writes them, dssharness write-anchor and set-anchor. |
| **`apply-registry-row`** | `apply-registry-row.py` | replace one deferred-anchor registry row with a lane's verbatim row text from a file. |
| **`burndown-queue`** | `burndown-queue.py` | re-derive the prioritized burndown queue from the registry, production errors first. |
| **`check-anchor-balance`** | `check-anchor-balance.py` | refuse a cycle that ends with more OPEN deferral-registry rows than it began. |
| **`check-anchor-registry`** | `check-anchor-registry.py` | refuse a `D-*` anchor cited in a scanned root that resolves to no registry row, and refuse a markdown table row whose unescaped pipes would silently drop cells. |
| **`check-diagnostic-codes`** | `check-diagnostic-codes.py` | refuse a duplicate, implicitly-numbered, or newly-uncovered `DiagnosticCode` ordinal. |
| **`check-doc-census`** | `check-doc-census.py`, `source-census.py` | refuse a documented figure a census refutes, or an unpinned quantified claim about the corpus in config prose, and repair figures in place. |
| **`check-emitted-anchor-ids`** | `check-emitted-anchor-ids.py` | refuse a new anchor id inside a C++ string literal under src/: operator output states the condition and the action, never the bookkeeping. |
| **`check-enum-name-table-guards`** | `check-enum-name-table-guards.py` | refuse an `EnumNameTable` vocabulary declared in `src/` without a `DSS_CHECK_ENUM_NAME_TABLE` well-formedness assert. |
| **`check-export-macro-placement`** | `check-export-macro-placement.py` | refuse DSS_EXPORT on a member of an already-exported class, which is MSVC error C2487. |
| **`check-guard-output-encoding`** | `check-guard-output-encoding.py` | refuse a Python script whose report cannot carry a non-cp1252 character through a pipe. |
| **`check-line-endings`** | `check-line-endings.py` | refuse a tracked text blob that carries a CR, and a CR instrument that cannot see one. |
| **`check-lsp-coordinates`** | `check-lsp-coordinates.py` | refuse a raw coordinate conversion in src/lsp/ outside lsp_coordinates.cpp — the anti-regression device for LSP positions resolved in synthesized preprocessor coordinates. |
| **`check-ninja-deps`** | `check-ninja-deps.py` | refuse a gate over a build directory whose objects recorded no header dependencies. |
| **`check-no-abort-in-tests`** | `check-no-abort-in-tests.py` | refuse a new live `abort()` call site in test or test-support code. |
| **`check-orphan-tests`** | `check-orphan-tests.py` | refuse a test source that no CMake target compiles and no ctest entry runs. |
| **`check-path-identity`** | `check-path-identity.py` | refuse a second path canonicalizer -- path resolution lives in exactly one place. |
| **`check-pkg-pipeline`** | `check-pkg-pipeline.py` | pin the package pipeline's release-path step sequence and refuse an artifacts-only run that can reach a release. |
| **`check-plan-citations`** | `check-plan-citations.py` | refuse a new `path:line` citation in the plans -- a citation names a stable reference, never a line number. |
| **`check-retyped-closed-sets`** | `check-retyped-closed-sets.py` | census the diagnostics that RETYPE a closed vocabulary instead of projecting it. |
| **`check-scripts-index`** | `check-scripts-index.py` | refuse an action that no index documents, an index entry that no action backs, and an action no runner can start. |
| **`check-stale-blockers`** | `check-stale-blockers.py` | list OPEN registry rows whose Closing-work cell waits on a blocker that has since CLOSED. |
| **`check-stale-refusal-citations`** | `check-stale-refusal-citations.py` | refuse a new present-tense refusal sentence that cites an anchor row already marked CLOSED. |
| **`check-wall-clock-in-tests`** | `check-wall-clock-in-tests.py` | refuse a new wall-clock duration literal in test code outside the shared measured budget. |
| **`check-wrapped-anchor-ids`** | `check-wrapped-anchor-ids.py` | refuse a NEW anchor id split across a line break, which no grep can ever return. |
| **`clock-step-probe`** | `clock-step-probe.py` | count the intervals in which this host's CLOCK_REALTIME diverged from CLOCK_MONOTONIC beyond the tolerance, so a failure blamed on a clock step has something that can refuse to confirm it. |
| **`cmake-import`** | `cmake-import.py` | convert a CMake project into a DSS `.dss-project.json` manifest. |
| **`compile-bench`** | `compile-bench.py` | time dsscp against gcc/clang/MSVC/tcc on ONE host over a subject size ladder, naming every reference it could not find. |
| **`corpus-census`** | `corpus-census.py`, `test-corpus-census.py` | census the real-example corpus into a run-identified report instead of one overwritten log. |
| **`examples-census`** | `examples-census.py` | re-derive every corpus-manifest figure examples/README.md states, by parsing the manifests. |
| **`lane-fold`** | `lane-fold.py` | seed a lane worktree from the main tree, fold only that lane's real changes back, and land it with its rows applied and its evidence preserved. |
| **`lane-worktree`** | `lane-worktree.py` | create and remove lane worktrees inside the ignored .worktrees/, refusing any root that would exceed Windows MAX_PATH. |
| **`macho-alias-ld64-matrix`** | `macho-alias-ld64-matrix.py` | measure what Apple's ld64 does with a second defined symbol at the address of a canonical one, with and without -dead_strip. |
| **`manual-end-to-end`** | `manual-end-to-end.py` | run the manual end-to-end corpus, the entries whose cost makes them wrong to put in a gate, taken deliberately instead. |
| **`owning-tree`** | `owning-tree.py` | name the DSS tree a script's own file lives in -- walked up from that file, never taken from the caller's working directory or git environment. |
| **`pragma-profile-census`** | `pragma-profile-census.py` | census `#pragma` usage across the corpus and hold the profile to its expected shape. |
| **`probe-reference-cc`** | `probe-reference-cc.py` | compile, and optionally run or dump, one probe with the reference compiler of the host a leg runs on, and keep the verdict, the diagnostics and the result redacted, so a reference is MEASURED on a host no session reaches directly. |
| **`profile-compile`** | `profile-compile-support.py`, `profile-compile.py` | compile one fixed subject with a RELEASE dsscp on this host and report where the time went, so the HOST is the only variable across legs. |
| **`prototype-census`** | `prototype-census.py` | judge every prototype DSS ships against the TYPE the leg's own platform header gives it — twice, by DSS's own `_Generic` and by the leg's reference compiler — and keep a per-symbol report, so a shipped prototype's identity is MEASURED, never assumed. |
| **`read-leg-path`** | `read-leg-path.py` | print the tail of a file, or the newest entries of a directory, inside a leg's tree on the host that leg runs on, redacted and kept, so a remote step's log can be read without a raw ssh session. |
| **`real-examples/c/sqlite`** | `benchmark_speedtest1.py`, `build_and_test.py`, `cli-smoke.py`, `gen-pe64-manifest.py`, `harness_legs.py`, `speedtest1_bench.py`, `sqlite_base.py`, `sqlite_build.py`, `sqlite_coherence.py`, `sqlite_common.py`, `sqlite_compiler.py`, `sqlite_corpus.py`, `sqlite_launch.py`, `sqlite_libs.py`, `sqlite_procs.py`, `sqlite_recompile.py`, `sqlite_report.py`, `sqlite_smoke.py`, `sqlite_stage.py`, `sqlite_units.py`, `sqlite_verdicts.py`, `stage-zinc.py`, `test_confound_scope.py`, `test_driver_contracts.py` | prove dsscp builds SQLite from its real sources into the Tcl testfixture and the sqlite3 CLI, and runs SQLite's own unit corpus green, for every declared target. |
| **`refresh_landing_log`** | `refresh_landing_log.py`, `test_refresh_landing_log.py` | regenerate the PR landing-log hash anchors in the plans from git log. |
| **`sqlite-round-trip`** | `sqlite-round-trip.py` | carry a sqlite leg's DSS-built artefacts to a machine that runs their target and prove they EXECUTE there (the round trip). |
| **`sqlite-runtime-bench`** | `sqlite-runtime-bench.py` | measure the RUNTIME of an emitted sqlite3 binary, the standing runtime-differential instrument. |
<!-- END GENERATED ACTION INDEX -->

## Which of these run in the gate

Every repository guard here is a ctest entry carrying the `repo-guard` label, and runs
on every gate, on every host that configures the tree; each action's own
`description` names the entry that gates it (`ctest -N -L repo-guard` lists them).
Each is ONE Python program run the same way on every host.
Two programs have their SELF-TESTS in the gate though their real work is not:
`cmake-import` (`harness/cmake_import_selftest`, and `harness/cmake_import_any_cwd`,
which builds a generated manifest with the tree's own dsscp from three working
directories) and the corpus harness `sqlite` (`harness/sqlite_driver_selftest` runs its
driver's Step 0 alone; `harness/sqlite_benchmark_selftest` its benchmark's arms) — a
corpus run takes hours and runs only when someone runs the action. The instruments
with no ctest entry at all (`burndown-queue`, `check-stale-blockers`, `compile-bench`,
`examples-census`, `pragma-profile-census`, `profile-compile`, `sqlite-round-trip`,
`sqlite-runtime-bench`) run only when someone runs their action.

## Test parallelism

The gate runner supplies a parallel level explicitly rather than leaving ctest to
its own default. ✔MEASURED 2026-08-19 on a 16C/32T host: six example tests took
9741 ms with no level given and 2648 ms at 8 — the whole suite had been running
one test at a time because nothing ever supplied a level.
Today that level is configuration, not a script default: `defaults.buildCores`
and `defaults.testCores` in `.harness-config/config.json`, which a host may
replace with its own, because a remote host rarely has the core count of the
machine that wrote the configuration.
