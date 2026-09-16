# The wiring a DssHarness deletion touches — measured, per item

**What this is.** The DssHarness migration deletes 41 of this repository's 49 `scripts/**` shell
programs, one commit per group of verbs. Each row below is a piece of gate wiring that **fails the
tree unless it changes in the same commit as the script it names**. For each one this file records
the file and the SYMBOL, the value MEASURED at the commit this document landed, the exact command
that measured it, what the value becomes after the deletion, and which deletion wave it rides in.

**Why it lives in `cmake/`.** This is a statement about how the GATE is wired, not about what the
gate tests. `cmake/` is where this repository already keeps that — `DssTestBudgets.cmake`, whose
rows are one of the items below, sits beside it. Two properties decided it over `tests/`:
`cmake` is **not** one of `check-plan-citations`' `CODE_ROOTS` (`tests` is), so this document
cannot silently become citation-ratchet debt; and `cmake/` is small and curated, where a deletion
commit's author will actually see it.

## The rules this file is written under
- ★★★ **Every entry is a RETARGET, never a lowered number.** Each guard's own refusal says *"fix the
  scan, never the floor"*. A floor lowered to accommodate a deletion is the guard being disarmed by
  the very thing it exists to catch.
- ★★★ **Re-derive, never re-quote.** Every figure here is reproducible from the command in its row.
  Re-run it at the commit that carries the claim; do not copy a number forward.
- ⛔ **Never cite a line number.** Every location below is a path plus a symbol.
- Claims are labelled ✔MEASURED / 📄DOCUMENTED / 🧠INFERRED. A projection marked ⟶ is arithmetic over
  a measured set, not a second measurement: it becomes MEASURED only when its deletion commit runs
  the command again.

## The deletion waves, named once
`W-anchors` · `W-build` (`local-build`) · `W-test` (`run-gate`) · `W-sync` (`wsl-leg`, `remote-leg`,
`macos-leg`, `leg-tree`, `ssh-macos`, `ssh-arm64-vps`, `carriage-excludes`, `check-carriage-paths`) ·
`W-worktrees` (`lane-worktree`) · `W-run` (`sqlite-runtime-bench`, `sqlite-round-trip`,
`macho-alias-ld64-matrix`) · `W-buildtime` (`profile-compile`, `compile-bench`) · `W-lineendings` ·
`W-cilegs` · `W-buildchecks` (`check-ninja-deps`, `check-root-litter`) · `W-secrets` ·
`W-lastconsumer` (`repo-tree`, `owning-tree`).

---

## 1. The population everything else is derived from

| Subject | Now | Instrument | After | Wave |
|---|---|---|---|---|
| `.sh` + `.ps1` under `scripts/` | **49** (31 `.sh`, 18 `.ps1`) | `git ls-files 'scripts/*' \| grep -E '\.(sh\|ps1)$' \| wc -l` | ⟶ **8** | all |
| `scripts/` directories | **52** | `ls -d scripts/*/ \| wc -l` | ⟶ **25** | all |
| the 8 survivors | — | — | `check-orphan-tests`, `cmake-import`, `corpus-census`, `pragma-profile-census` — both twins each | — |
| `.harness-config/runner/actions` | **0 files** (`.gitkeep` only) | `find .harness-config/runner/actions -type f` | ⟶ the YAML runners | `W-run` |

✔MEASURED 2026-09-16, lane s4. The filesystem walk and `git ls-files` agree exactly at 49, so no
untracked stray is inflating the figure. **AGREES with contract S4** (49 today, 41 go, 8 remain).

---

## 2. The CMake bash probe — **DEFUSED IN THIS COMMIT**

| Item | Value |
|---|---|
| Location | `CMakeLists.txt`, symbol `_dss_lane_probe`, under the comment *"WHAT \"A USABLE BASH\" MEANS, DEFINED ONCE FOR EVERY ENTRY THAT NEEDS ONE"* |
| Was | one string that also ran `test -f '${CMAKE_SOURCE_DIR}/scripts/lane-worktree/lane-worktree.sh'` |
| Is | `test -f '${CMAKE_SOURCE_DIR}/CMakeLists.txt' && command -v dirname && command -v mktemp && command -v git` |
| Blast radius when the file went missing | ✔MEASURED **8** ctest entries, not 7 |
| Wave | **already ridden — this commit**, so no later wave inherits it |

The eight: `lane_worktree_guard`, `root_litter_guard`, `macos_leg_source_tree_guard`,
`leg_tree_guard`, `check_ci_legs_git_environment_guard`, `remote_leg_helper_transport_guard`,
`run_gate_guard`, **`line_endings_watchdog_sh_guard`**.

⚠ **DISAGREES with contract S4, which says seven.** The eighth is `line_endings_watchdog_sh_guard`,
registered in `CMakeLists.txt` from `_dss_rg_bash`, which is derived from
`DSS_BASH_FOR_LANE_GUARD` — it rides the probe's answer without ever naming the probe.

Instrument, reproducible: point the probe's `test -f` at an absent path, then
`cmake -B build/<lane> -U DSS_BASH_FOR_LANE_GUARD` and
`ctest --test-dir build/<lane> -R '^(lane_worktree_guard|…)$' --output-on-failure`.

The two questions are now separated and each sits with its owner: the HOST question here, against a
file whose existence CMake guarantees by reading it; the TREE question on the `add_test` that NAMES
its own script. ✔MEASURED: a missing subject now reds that one entry by path —
`bash: …/test-lane-worktree.sh…: No such file or directory` (exit 127), and for a Python-driven
entry `python.exe: can't open file '…test-macos-leg.py…'` (exit 2).

---

## 3. `repo_tree_guard`'s PowerShell probe

| Item | Value |
|---|---|
| Location | `CMakeLists.txt`, symbols `_dss_rt_script` and `_dss_rt_verdict` |
| Subject | `scripts/repo-tree/repo-tree.ps1` |
| The literal matched | `repo-tree: this file is a library` — printed by `repo-tree.ps1`'s library-guard `Write-Host`, whose full line continues *"; dot-source it, or run --selftest / --help."* |
| Instrument | `grep -n "this file is a library" scripts/repo-tree/repo-tree.ps1` |
| After | the script, the probe, the three refusal arms and the `repo_tree_guard` entry all leave together |
| Wave | **`W-lastconsumer`** |

✔MEASURED, **AGREES with contract S4**. ⚠ The probe judges by a **string match on the script's
output**, not by an exit code (`string(FIND "${_dss_rt_out}${_dss_rt_err}" "${_dss_rt_verdict}" …)`),
so deletion misattributes exactly as item 2 did, one host down. ✔MEASURED 2026-09-16 against
`pwsh.exe` 7.x: a missing `-File` answers *"The argument '…' is not recognized as the name of a
script file"* at **exit 64**, the verdict line never appears, every candidate is recorded as tried,
and on Windows the entry takes the branch whose own text says there is *"deliberately NO
not-applicable escape on Windows"* — a REFUSAL blaming PowerShell for a deleted file. Control arm,
the real script: it prints the verdict line and exits **2** (its own library guard), and the probe is
green — which is why an exit-code test would have been the wrong instrument here and a text match is
right. ⇒ delete the probe, its three refusal arms and the entry in ONE commit with the script.

---

## 4. Test-budget rows

| Item | Value |
|---|---|
| Location | `cmake/DssTestBudgets.cmake`, symbol `_DSS_TB_NAMED` |
| Rows naming a migrating subject | `run_gate_guard\|102\|288\|88` → `W-test`; `lane_worktree_guard\|37\|104\|33` → `W-worktrees`; `harness/test_sqlite_harness_legs\|66\|73\|67` → **stays** (the test is retargeted, not deleted — item 9) |
| Rows total | **38** ✔MEASURED, and the configure line reads `named 38`, so all 38 match a registered entry today |
| Instrument | `cmake -B build/<lane>` and read `-- ctest entry budgets: … (corpus N, named 38, unit N)`; the per-row detail is `build/<lane>/ctest-entry-budgets.txt`, `named\t<name>\t<ceiling>\t<matched>` |

⚠ **PARTLY DISAGREES with contract S4**, which says *"the budget check refuses a row naming a test
that no longer exists"*. ✔MEASURED: a stale row does **not** fail configure. `_dss_test_budgets_apply`
collects it into `_stale` and emits `message(WARNING … the pin 'ctest/entry-budgets' refuses this)`.
The **refusal** is a separate ctest entry, `ctest/entry-budgets`, registered by
`integrated_tests/CMakeLists.txt` and implemented in `cmake/DssTestBudgetsCheck.py`
(failure class `named-row-stale`). ⇒ the deletion commit must delete the row, and a commit that
forgets is caught by `ctest/entry-budgets`, not by its own configure.

---

## 5. `scripts_index_guard`

| Item | Value |
|---|---|
| Location | `CMakeLists.txt`, `add_test(NAME scripts_index_guard …)`; guard `scripts/check-scripts-index/check-scripts-index.py`, symbol `SCRIPT_FLOOR` |
| Two indexes it holds to the tree | `scripts/README.md` and `.claude/skills/dss-cycle/references/scripts.md` — **52 named scripts each**, matching the 52 directories exactly (`grep -oE '^\| \*\*`[a-z0-9._-]+`\*\*' <index> \| wc -l`; raw `^\| ` line counts are 54 and 59, the difference being one and two header/separator rows plus prose rows) |
| Directories scanned | **52** ✔MEASURED |
| `SCRIPT_FLOOR` | **12** |
| After | ⟶ **25** directories |
| Wave | every wave edits both indexes; the guard itself survives |

Instrument: `python scripts/check-scripts-index/check-scripts-index.py` →
`check-scripts-index: OK (52 scripts, both indexes agree with the tree and with …)`.

⚠ **DISAGREES with contract S4**, which lists `check-scripts-index (≥ 12 directories)` among the
floors *"this migration crosses"*. It is **not crossed**: 25 ≥ 12, with 13 of headroom. The real
obligation is the two indexes, which must lose their rows in the same commit as the directory.
⛔⛔ **AND ITS SELF-TEST'S `run-gate` FIXTURE IS THE REAL ONE, WHICH MAKES `W-test` A HARD BLOCKER
FOR THIS GUARD.** ✔MEASURED by reading `selftest()` and `_mirror()`: `_mirror` `copytree`s every real
`scripts/<dir>` and `copyfile`s both real index documents into a `mktemp` tree, and the arms then
depend on the REAL `run-gate` in four distinct ways — `_read` of `scripts/run-gate/run-gate.sh`
before any arm runs (a missing file raises, collapsing the whole self-test); arms *2
INDEX-ENTRY-NOT-A-SCRIPT* / *2b* which `shutil.move` that directory out and back; arms *3
PURPOSE-DRIFTED* / *3b* and *6 NO-PURPOSE-DECLARED* / *6b*, which mutate that script's `PURPOSE:`
mark; and arms *4.N ONLY-&lt;doc&gt;-DRIFTS*, which sabotage each index by replacing the literal
``| **`run-gate`**``. After `W-test` removes `run-gate` from the two indexes, that `.replace` matches
nothing, the document is left pristine, and the arm expects `EXIT_DISAGREE` — *"an expectation that
follows the change it is meant to catch is not an expectation"*, which is the exact failure the
file's own `EXPECTED_ARMS = 31` comment was written about. ⇒ **`W-test` must re-point the fixture at a
surviving script in the same commit** (`cmake-import` is the natural pick: both twins, a `PURPOSE:`
mark, and it survives every wave). This is a retarget of a FIXTURE, not of a floor.

---

## 6. `check-wrapped-anchor-ids` inventory

| Item | Value |
|---|---|
| Location | `scripts/check-wrapped-anchor-ids/inventory.json`, key `ceilings` |
| Entries on a migrating file | `scripts/burndown-queue/burndown-queue.py` → **2** (`W-anchors`); `scripts/check-anchor-registry/check-anchor-registry.ps1` → **2** (`W-anchors`); `scripts/check-scripts-index/check-scripts-index.py` → **1** (**stays**) |
| Instrument | `python -c "import json,io;print(json.load(io.open('scripts/check-wrapped-anchor-ids/inventory.json',encoding='utf-8'))['ceilings'])"` |
| After | the two `W-anchors` keys are **deleted**, never lowered to 0 — the file is a ratchet where *"raising an entry, or adding a file, is a FAILURE"* |

✔MEASURED, **AGREES with contract S4** on all three names. ⓘ `check-scripts-index.py` stays, so its
ceiling of 1 stays with it — the contract's grouping of the three is right about the file, not about
the disposition.

---

## 7. `check-guard-output-encoding`

| Item | Value |
|---|---|
| Location | `scripts/check-guard-output-encoding/check-guard-output-encoding.py`, symbol `PY_FLOOR`; inventory `scripts/check-guard-output-encoding/inventory.json`, key `unprotected` |
| `PY_FLOOR` | **8** |
| Python primaries now | **30** ✔MEASURED (the file's own comment still says *"14 ✔MEASURED 2026-08-23"* — **STALE**) |
| After | ⟶ **21** |
| `unprotected` entries | **3**: `check-shell-portability` (stays), `refresh_landing_log` (stays), **`sqlite-runtime-bench`** (leaves) |
| Wave | **`W-run`** deletes the `sqlite-runtime-bench` entry in the same commit as the file |

Instrument for the primaries: load `scripts/check-scripts-index/check-scripts-index.py`, call
`scan(Path('.'))` and count entries whose `primary` ends `.py` (this is the same enumeration
`check-guard-output-encoding` consumes). Breakdown today: **30 `.py`, 21 `.sh`, 1 `.ps1`**.

⚠ **DISAGREES with contract S4**, which lists `check-guard-output-encoding (≥ 8 Python primaries)`
among the crossed floors. It is **not crossed**: 21 ≥ 8, with 13 of headroom. The nine primaries
that leave are `anchors`, `apply-registry-row`, `burndown-queue`, `carriage-excludes`,
`check-carriage-paths`, `check-ninja-deps`, `owning-tree`, `sqlite-round-trip`,
`sqlite-runtime-bench`. The real obligation is the single `unprotected` entry.

---

## 8. The floors, re-derived

| Guard | Symbol | Floor | Now | After | Crossed? | Wave |
|---|---|---|---|---|---|---|
| `check-anchor-registry` | `_ROOT_SPECS` | `src` 400 · `examples` 150 · `docs` 8 | — | unchanged | **N/A — the roots contract S4 names no longer exist** | `W-anchors` deletes the guard |
| `check-shell-portability` | `SCAN_FLOOR` | **15** | **39** | ⟶ **12** | ✅ **YES** | 27 `.sh` are removed in total and the floor is crossed by the **25th** (39 − 25 = 14 < 15); retarget in that commit |
| `check-guard-output-encoding` | `PY_FLOOR` | **8** | **30** | ⟶ **21** | ❌ no | — |
| `check-scripts-index` | `SCRIPT_FLOOR` | **12** | **52** | ⟶ **25** | ❌ no | — |
| `check-carriage-paths` | `FLOOR_ROOTS` | **6** | **11** | ⟶ **2** | ✅ **YES** | `W-sync` |
| `check-carriage-paths` | `FLOOR_SKILLDIRS` | **1** | **3** | ⟶ **1** | ❌ no (zero headroom) | `W-sync` |
| `check-carriage-paths` | `FLOOR_CLONEPAIRS` | **1** | **1** | ⟶ **1** | ❌ no (zero headroom) | `W-sync` |
| `tests/harness/test_sqlite_harness_legs.cpp` | `kScriptFloor` | **16** | **49** | ⟶ **8** | ✅ **YES** | see item 9 |

⚠⚠ **The largest disagreement with contract S4.** It says *"six guards refuse below a floor, and this
migration crosses five of them"*, naming `check-anchor-registry` (`scripts` ≥ 25 distinct ids,
`.claude` ≥ 15), `check-shell-portability`, `check-guard-output-encoding`, `check-scripts-index` and
`carriage_paths_guard`. ✔MEASURED at this commit:
- **`check-anchor-registry` has no `scripts` root and no `.claude` root.** Its `_ROOT_SPECS` are
  `src|400`, `examples|150`, `docs|8`, under a comment reading *"NARROWED 2026-09-16 TO THE
  PRODUCTION ROOTS, BY OPERATOR RULING 5"*. Those two floors were already removed on this branch.
  The contract's bullet describes a pre-narrowing tree.
- **`check-guard-output-encoding` and `check-scripts-index` are not crossed** (13 of headroom each).
- ⇒ the crossed floors are **three**, not five: `check-shell-portability`, `check-carriage-paths`'s
  `FLOOR_ROOTS`, and `kScriptFloor`. `check-carriage-paths` needs no retarget only because the guard
  itself leaves in `W-sync`.
- ⚠ **AND THAT MAKES ORDERING load-bearing INSIDE `W-sync`.** `FLOOR_ROOTS` reads 11 sites today, and
  **nine of them are in `macos-leg`, `profile-compile`, `remote-leg` and `wsl-leg`** — three of which
  are in `W-sync` and `profile-compile` is in `W-buildtime`. If any of those lands before
  `scripts/check-carriage-paths/` and the `carriage_paths_guard` entry are removed, the guard
  collapses with its own *"fix the scan rather than lowering the floor"* refusal (exit 2) in between.
  The guard and its last subject must go in ONE commit, or the guard first.

Instruments: `python scripts/check-shell-portability/check-shell-portability.py` (prints its own
count), `python scripts/check-scripts-index/check-scripts-index.py`,
`python scripts/check-carriage-paths/check-carriage-paths.py` (prints roots / clone sites / skill
dirs), and reading `_ROOT_SPECS` in `scripts/check-anchor-registry/check-anchor-registry.sh`.

---

## 9. `tests/harness/test_sqlite_harness_legs.cpp` — the walk that crosses its floor

| Item | Value |
|---|---|
| Location | `tests/harness/test_sqlite_harness_legs.cpp`, function `shellScriptsUnderTest()`, constant `kScriptFloor` |
| What it walks today | `harnessDir()/build-and-test.{ps1,sh}` pushed by name, **plus a recursive walk of `repoRoot()/"scripts"` for `.sh` and `.ps1`** |
| `tools.size()` today | **49** ✔MEASURED |
| `kScriptFloor` | **16**; its refusal reads *"The enumeration COLLAPSED … Fix the walk; do not lower the floor."* |
| After the deletions, **unretargeted** | ⟶ **8** → the floor trips, and the trip would be RIGHT: the walk really would have stopped governing the repository's shell programs |
| Wave | the commit that takes the count under 16 |

**What the retargeted walk must count.** The test's own comment states the subject: *"COVERAGE IS BY
DIRECTORY, NOT BY LIST … so a NEW script is governed the day it lands."* That subject is **every
shell program this repository ships**, which after the migration lives in `real-examples/`,
`examples/`, `.harness-config/runner/actions` and what `scripts/` retains — not in `scripts/` alone.

**Predicted numbers, over the CURRENT tree** (both candidate shapes, measured, they agree):

| Walk | `tools.size()` now | after the 41 deletions | `kScriptFloor = 16` |
|---|---|---|---|
| today: `scripts/` only | 49 | 8 | **CROSSED** |
| declared roots `scripts` + `real-examples` + `examples` + `.harness-config` | **62** | ⟶ **21** | **HOLDS UNCHANGED** |
| whole tree minus `{.git, build, scratchpad, .temp, test-scratch}` | **62** | ⟶ **21** | **HOLDS UNCHANGED** |

Per-root today, drivers excluded: `scripts` 49, `real-examples` 11, `examples` 2,
`.harness-config` **0**.

⚠ **The whole-tree shape is REFUTED, and the refutation is measured.** Walking the tree with only
`{.git, build, scratchpad, .temp}` skipped picks up
`test-scratch/examples/58844-0/gen_answer_source.{sh,ps1}` — a **test-generated copy** of a corpus
example's script. The count would then depend on whether tests had run, and the guard would govern
generated files. This is the same hazard `check-shell-portability` documents and answers by asking
`.gitignore`; a C++ test cannot ask git cheaply. ⇒ **the retarget takes DECLARED ROOTS with a floor
PER ROOT**, the shape `check-anchor-registry`'s `_ROOT_SPECS` and `check-carriage-paths`'
`SCAN_TREES` already use, and for the reason `check-anchor-registry` records: a single global floor
is satisfied by one big root while another silently empties.

⚠ `.harness-config/runner/actions` holds **zero** shell programs today (`.gitkeep` only), so it
cannot carry a non-zero per-root floor until `W-run` places the YAML runners. Until then its
per-root rule is *the directory must EXIST*, not *it must be non-empty*.

⛔ **NOT RETARGETED IN THIS COMMIT, deliberately.** The count has not moved; a retarget landing
before the deletion would make the guard assert something no commit has yet made true.

**Three assertions re-point with it**, all naming `scripts/ssh-arm64-vps/ssh-arm64-vps.ps1`:
- `TEST_F(HarnessLegs, TheRecordedIdentityFlagIsNamedInExactlyOneFile)` — two references, both in
  the measured narrative of what the defect cost;
- `TEST_F(HarnessLegs, NoScriptInvokesWslWithoutExec)` — the `-e`-bound splat is described as *"how
  `scripts/ssh-arm64-vps/ssh-arm64-vps.ps1` passes"*, i.e. the rule's only live example of the
  accepted shape.

⛔ **`ssh-arm64-vps.ps1` leaves in `W-sync`, and it is the ONLY splat site in the repository.**
✔MEASURED: `grep -rnE "(&\s*)?wsl(\.exe)?\s+@[A-Za-z_]" --include=*.ps1 scripts real-examples
examples .harness-config` returns exactly one hit, `scripts/ssh-arm64-vps/ssh-arm64-vps.ps1`'s
`& wsl.exe @a`. The nearest survivor, `real-examples/c/sqlite/benchmark-speedtest1.ps1`, uses the
**direct** form `& wsl.exe -e bash -l -c $BashLine`, which the rule accepts by a different clause. ⇒
after `W-sync` the splat exemption has **no positive example anywhere in the tree** and becomes
untested logic — an escape nothing exercises. The commit that deletes the file must give the
exemption a **synthetic** fixture (the repository's own rule: a fixture must synthesize the case,
positive and negative), not simply re-point the prose.

---

## 10. `owning-tree`

| Item | Value |
|---|---|
| Location | `scripts/owning-tree/owning-tree.py` |
| Importers outside its own directory | **21**, all tracked, all real imports (not mentions) ✔MEASURED |
| ctest entries invoking one of those importers | **20** ✔MEASURED |
| Wave | **`W-lastconsumer`** — *"retire with their last consumer"*, and 15 of the 21 importers survive every wave |

**AGREES with contract S4** on both figures (21 and 20). Instrument: intersect the importer list with
`add_test` bodies across every tracked `CMakeLists.txt` and `*.cmake`; the reproduction script is in
the lane's scratch as `measure.py` / `measure2.py`.

The 20: `anchor_balance_selftest_guard`, `anchors_selftest_guard`,
`apply_registry_row_selftest_guard`, `build/ninja-deps-freshness`, `carriage_excludes_selftest_guard`,
`check_ci_legs_git_environment_guard`, `corpus_census_git_environment_guard`,
`diagnostic_codes_guard`, `guard_output_encoding_guard`, `landing_log_git_environment_guard`,
`lane_fold_selftest_guard`, `macos_leg_source_tree_guard`, `ninja_deps_selftest_guard`,
`pkg_pipeline_modes_guard`, `plan_citations_guard`, `remote_leg_helper_transport_guard`,
`scripts_index_guard`, `shell_portability_guard`, `stale_refusal_citations_guard`,
`wrapped_anchor_ids_guard`.

⚠ `owning-tree` therefore cannot leave until `scripts/check-scripts-index`,
`check-shell-portability`, `check-guard-output-encoding`, `check-plan-citations`,
`check-stale-refusal-citations`, `check-wrapped-anchor-ids`, `check-diagnostic-codes`,
`check-pkg-pipeline`, `corpus-census`, `lane-fold`, `check-anchor-balance` and `refresh_landing_log`
have stopped importing it — and S6 keeps every one of those. 🧠INFERRED consequence: **`owning-tree`
outlives the migration** unless its `repoRoot()` resolution is replaced for the survivors.

---

## 11. Items S4 names that this lane does not own — recorded so the coverage claim is honest

These three are wiring the deletion touches, but they live outside `CMakeLists.txt`, `cmake/**` and
`tests/**`. They are measured here and owned elsewhere.

| Item | ✔MEASURED | Owner / wave |
|---|---|---|
| `.plans/_handoff.md`'s *"HOW TO ORIENT"* authority chain names `_deferred-anchor-registry-harness.md` as authority 3 and `scripts/anchors/{write,set,read}-anchor` as *"the only door"* | both become false in `W-anchors` | the handoff document's owner, in `W-anchors` |
| the plan-citation ratchet, `scripts/check-plan-citations/inventory.json` — **210 ceiling entries**, `SCAN_ROOTS = ('.plans', '.claude')` plus `CODE_ROOTS` (`src`, `tests`, `scripts`, `examples`, `docs`, `packaging`, `.github`, `integrated_tests`, `real-examples`) | it reds on a **stale** ceiling exactly as on a raised one, so every commit that moves rows recomputes the inventory in that same commit | the registry lane, every wave that moves rows |
| CI's landing-log job in `.github/workflows/pipeline-pr.yml`, job `landing-log-check` | ✔**ungated by `Run Pipes`** — its own comment reads *"Ungated by the Run Pipes label — plan-doc hygiene matters on every PR"* — and it runs `scripts/refresh_landing_log/test_refresh_landing_log.py` then `refresh_landing_log.py --check`. ✔MEASURED: **no ctest entry invokes `test_refresh_landing_log.py`**; the local entry `landing_log_git_environment_guard` runs `refresh_landing_log.py --self-test` instead ⇒ the first file is invisible to every local gate | `refresh_landing_log` **stays** per S6, so nothing here is deleted — but a change to it is provable only in CI |

---

## 12. Prose that must die with its code — added 2026-09-16 (P68, orchestrator)

**Operator ruling, 2026-09-16:** *"be carefull on the skill changes... after migration, the scripts
limitations and problems must not be considered anymore"*. A script's limitation is not a project rule.
When the script goes, a sentence documenting its quirk does not merely go stale — it MISLEADS, because
it reads as guidance and describes something that no longer exists, in the document a contextless
session trusts first.

| Where | What it says | Wave | What must survive |
|---|---|---|---|
| `.claude/skills/dss-cycle/SKILL.md`, the macOS `run_gate_guard` paragraph of the CI-measurement block | `remote-leg.sh` passes `-LE repo-guard` while `wsl-leg.sh` defaults `DSS_LEG_GUARDS` to `1`; the 2207 / 2206 / 2167 split | `W-sync` | that a repo-guard is HOST-INDEPENDENT, so its leg set is a CONFIG decision, and a leg that skips it cannot see a guard-only defect — **re-derive the set from the run in front of you** |
| `.claude/skills/dss-cycle/SKILL.md`, the *"local gate cannot substitute for CI"* hard-stop bullet | the same two script names | `W-sync` | the same principle |

Both blocks already carry a `⏳ SCRIPT-ERA` marker naming this wave, so the deletion commit can find
them by grepping `SCRIPT-ERA`.

★ **The reason worth carrying into the commit message:** that leg-set divergence existed BECAUSE three
hand-written scripts each decided it independently, with different defaults — which is why the skill
asserted for a year that guards ran on *"exactly one local host"* when they ran on two. **One tool
reading one config cannot have that class of bug.**

⚠ **`W-guardrunners` inherits this row**, because whichever wave moves the surviving guards to
`.harness-config/runner/actions/<name>/<name>.yml` is the wave that decides their leg set in config —
and that is the moment the durable sentence above becomes checkable rather than advisory.

## Summary of disagreements with contract S4

| # | Contract S4 says | ✔MEASURED at this commit |
|---|---|---|
| 1 | deleting `lane-worktree.sh` takes down **seven** ctest entries | **eight** — `line_endings_watchdog_sh_guard` is the eighth |
| 2 | the budget check **refuses** a row naming a missing test | configure **warns**; the refusal is the separate `ctest/entry-budgets` entry |
| 3 | `check-anchor-registry` floors are `scripts ≥ 25`, `.claude ≥ 15` | **neither root exists**; `_ROOT_SPECS` is `src\|400`, `examples\|150`, `docs\|8` |
| 4 | the migration crosses **five** of six floors | it crosses **three**: `check-shell-portability`, `check-carriage-paths`'s `FLOOR_ROOTS`, `kScriptFloor` |
| 5 | `check-guard-output-encoding` floor is crossed | 30 ⟶ 21 against a floor of 8 — 13 of headroom |
| 6 | `check-scripts-index` floor is crossed | 52 ⟶ 25 against a floor of 12 — 13 of headroom |
| 7 | (not stated) | `check-guard-output-encoding.py`'s own comment says 14 primaries; there are **30** |
| 8 | (not stated) | `check-shell-portability`'s own comment says 24 `.sh`; it scans **39** |
| 9 | `scripts_index_guard`'s self-test *"uses `scripts/run-gate` as a fixture"* | **confirmed and worse** — `_mirror` copies the REAL tree, so `W-test` collapses the self-test outright, not one arm (item 5) |
| 10 | (not stated) | `repo_tree_guard`'s probe judges by TEXT, not exit code; a missing `-File` is pwsh **exit 64** with no verdict line, and the control exits **2** while passing (item 3) |
| 11 | (not stated) | a whole-tree retarget of `kScriptFloor` is refuted by `test-scratch/`, a test-GENERATED copy of a corpus script (item 9) |
| 12 | 49 scripts, 41 go, 8 remain · `owning-tree` 21 importers, 20 ctest entries · the three wrapped-id inventory entries | **all confirmed** |
