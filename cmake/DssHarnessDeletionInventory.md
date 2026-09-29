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
`W-anchors` · ~~`W-build` (`local-build`)~~ **LANDED** · ~~`W-test` (`run-gate`)~~ **LANDED** ·
~~`W-sync` (`wsl-leg`, `remote-leg`, `macos-leg`, `ssh-macos`, `ssh-arm64-vps`, `carriage-excludes`,
`check-carriage-paths`)~~ **LANDED** · `W-worktrees` (`lane-worktree`) · `W-run`
(`sqlite-runtime-bench`, `sqlite-round-trip`, `macho-alias-ld64-matrix`) · `W-buildtime`
(`profile-compile`, `compile-bench`) · `W-lineendings` · ~~`W-cilegs`~~ **LANDED** · `W-buildchecks`
(`check-ninja-deps`, `check-root-litter`) · `W-secrets` · `W-lastconsumer` (`repo-tree`,
`owning-tree`, **`leg-tree`**).

⛔⛔ **`leg-tree` MOVED OUT OF `W-sync`, AND A WAVE THAT DELETES IT THERE REDS FOUR ENTRIES BELONGING
TO THREE OTHER WAVES.** ✔MEASURED 2026-09-17 (lane `mg`, while landing `W-sync`):
`scripts/leg-tree/leg-tree.sh` is TWO programs under one name. Its carriage half
(`leg_tree_prepare`, `leg_tree_restore`, `leg_tree_remote_command`) is what `dssharness sync`
replaces and now has no caller. Its other half is the **`.sh` owner of "which tree am I standing
in"** — `leg_tree_driver_identity`, `leg_tree_git_unsteered`, `leg_tree_owning_root` — one of the
three one-per-language resolvers beside `repo-tree.ps1` and `owning-tree.py`, and **three scripts
outside the leg drivers SOURCE it at runtime and die loudly if it is absent**:
`check-line-endings.sh` (`W-lineendings`), `check-root-litter.sh` (`W-buildchecks`) and
`lane-worktree.sh` (`W-worktrees`), plus `test-lane-worktree.sh`, which copies it into its fixture.
The entries that would have gone red: `line_endings_guard`, `line_endings_watchdog_sh_guard`,
`root_litter_guard`, `lane_worktree_guard`. ⇒ it retires with its LAST CONSUMER, which is item 10's
rule applied to the `.sh` resolver — item 10 recorded it only for the Python one.
Instrument: `grep -rnE "^\s*\.\s+.*leg-tree\.sh" --include=*.sh scripts/`.

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

**⟶ RE-MEASURED 2026-09-17, after `W-build` + `W-test` + `W-sync` + `W-cilegs` landed** (lane `mg`,
same two instruments): **34** (22 `.sh`, 12 `.ps1`) and **42** directories. Those four waves took
**15 files and 10 directories**. Remaining to the ⟶ 8 / ⟶ 25 target: **26 files, 17 directories**.
⚠ Not the 17 files and 11 directories the wave list implied, because `leg-tree` did not go — see the
block under *The deletion waves*. The `⟶` figures in the two rows above stay as the whole
migration's projection and are still arithmetic until the last wave runs them.

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

⟶ ✔RE-MEASURED 2026-09-17: **four**, after the leg-driver guards left with their subjects —
`lane_worktree_guard`, `root_litter_guard`, `leg_tree_guard`, `line_endings_watchdog_sh_guard`.
★ The probe itself did NOT change, which is the point of having defused it: its answer is about the
HOST, so it survived a wave that removed half its consumers without anyone touching it. **Re-derive
the set from the configure in front of you** rather than counting these names.
⚠ `run_gate_guard` left but its bash DERIVATION stayed, because `line_endings_watchdog_sh_guard` is
registered from `_dss_rg_bash` and would otherwise have vanished silently on every Windows host —
the eighth consumer this row exists to name.

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
| Rows total | **38** ✔MEASURED, and the configure line reads `named 38`, so all 38 match a registered entry today. ⟶ ✔RE-MEASURED 2026-09-17: **37**, `named 37`, after `W-test` deleted the `run_gate_guard` row with its entry. Total ctest entries 2210 ⟶ **2204**, and the −6 are exactly the six registrations those four waves removed |
| Instrument | `cmake -B build/<lane>` and read `-- ctest entry budgets: … (corpus N, named 38, unit N)`; the per-row detail is `build/<lane>/ctest-entry-budgets.txt`, `named\t<name>\t<ceiling>\t<matched>` |

⚠ **PARTLY DISAGREES with contract S4**, which says *"the budget check refuses a row naming a test
that no longer exists"*. ✔MEASURED: a stale row does **not** fail configure. `_dss_test_budgets_apply`
collects it into `_stale` and emits `message(WARNING … the pin 'ctest/entry-budgets' refuses this)`.
The **refusal** is a separate ctest entry, `ctest/entry-budgets`, registered by
`integrated_tests/CMakeLists.txt` and implemented in `cmake/DssTestBudgetsCheck.py`
(failure class `named-row-stale`). ⇒ the deletion commit must delete the row, and a commit that
forgets is caught by `ctest/entry-budgets`, not by its own configure.

⛔⛔ **AND THE ROW IS NOT THE ONLY BINDING. `cmake/DssTestBudgetsCheck.py`'s `PROBES` TABLE
NAMES A `_DSS_TB_NAMED` ENTRY TOO, AND THIS ITEM DID NOT RECORD IT.**
✔MEASURED 2026-09-17, in the full Debug gate of the commit that landed `W-test`: deleting the
`run_gate_guard|102|288|88` row — correct, because the entry left with the script it watched —
turned `ctest/entry-budgets` **RED**, and not through `named-row-stale`:

```
ctest-entry-budgets: FAIL class-probe (1)
    type='release' multi=0 flags='' entry='run_gate_guard': class release tier unit, expected release named
```

The probe re-configures a synthetic `release` build and asserts that entry is tiered `named`.
The budget FILE and the budget CHECKER are one decision written twice, so a deletion moves
both in the same commit. ⇒ **RETARGETED at `plan_citations_guard`, never removed**: the
lowercase-`release` arm is what proves a build type is compared case-insensitively and the
`named` tier is what proves a measured row is honoured over the unit ceiling — losing either
to a deletion is the guard disarmed by the thing it exists to catch. Any surviving
`_DSS_TB_NAMED` row whose entry is registered will do.
ⓘ ✔MEASURED that the blast radius is exactly one entry: `git grep DssTestBudgetsCheck` outside
that file returns only `integrated_tests/CMakeLists.txt`'s `ctest/entry-budgets` registration.

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

⟶ ✔RE-MEASURED 2026-09-17, after the four waves: **42 scripts**, both indexes agree, `SCRIPT_FLOOR`
UNCHANGED at 12 with 30 of headroom, `EXPECTED_ARMS` UNCHANGED at 31.

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

✅ **DISCHARGED 2026-09-17 IN THE `W-test` COMMIT, and the fix generalises past this subject.**
`SELFTEST_SUBJECT = "cmake-import"` is named once and the primary, the contradictable sibling,
the index row and the purpose text are all DERIVED from it; `_subject_paths` refuses before arm 0
when the subject has no primary or no sibling; and **every sabotage now goes through `_sabotage`,
which RAISES when its needle is absent**. ✔MEASURED: there were **eight** stale literals, not one
— six bare `run-gate` spellings, the index row ``| **`run-gate`**``, and, buried, `PURPOSE: run a
gate`, the opening words of that one script's own purpose text.
⚠ **AND THIS ROW'S PREDICTION OF THE FAILURE MODE WAS WRONG, ✔MEASURED.** It says the stale
`.replace` leaves the document pristine *"and the arm expects `EXIT_DISAGREE`"*. At the full
deletion `_read(prim)` raises FIRST, so the old shape produces a raw `FileNotFoundError` traceback
under a ctest entry whose name says nothing about a retired fixture — not a subtly-wrong arm. The
subtle-arm hazard is real but reachable only when the subject SURVIVES and a needle drifts, which
is the case `_sabotage` converts into a named refusal. Both mutants were driven THROUGH `ctest`
with the source md5 moved and returned and a named green control; the transcript is in the lane's
report. The row's CONCLUSION held; its prescribed diagnosis decayed, which is this project's own
rule about a remedy decaying fastest.

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

⟶ ✔RE-MEASURED 2026-09-17 after the four waves: **28 `.py`, 13 `.sh`, 1 `.ps1`** = 42. `PY_FLOOR`
UNCHANGED at 8, 20 of headroom. The two Python primaries that left are `carriage-excludes` and
`check-carriage-paths`; the `unprotected` inventory is untouched, its one migrating entry
(`sqlite-runtime-bench`) still belonging to `W-run`.

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
| `check-shell-portability` | `SCAN_FLOOR` | **15** | **39** ⟶ ✔**30** (2026-09-17) | ⟶ **12** | ✅ **YES, but not yet** | the four landed waves removed 9 `.sh` and left **15 of headroom**; the floor is crossed by the 25th of the 27, so it is still the LATER commit that owes the retarget |
| `check-guard-output-encoding` | `PY_FLOOR` | **8** | **30** | ⟶ **21** | ❌ no | — |
| `check-scripts-index` | `SCRIPT_FLOOR` | **12** | **52** | ⟶ **25** | ❌ no | — |
| `check-carriage-paths` | `FLOOR_ROOTS` | **6** | **11** | ⟶ **2** | ✅ **YES** | `W-sync` |
| `check-carriage-paths` | `FLOOR_SKILLDIRS` | **1** | **3** | ⟶ **1** | ❌ no (zero headroom) | `W-sync` |
| `check-carriage-paths` | `FLOOR_CLONEPAIRS` | **1** | **1** | ⟶ **1** | ❌ no (zero headroom) | `W-sync` |
| `tests/harness/test_sqlite_harness_legs.cpp` | `kScriptFloor` | **16** | **49** | ⟶ **8** | ✅ **YES** | see item 9 |
| `tests/harness/test_sqlite_harness_legs.cpp` | `NoScriptInvokesWslWithoutExec`'s `invocationsSeen` | **4** | **5** | ⟶ **3** | ✅ **YES** | **`W-sync` — RETARGETED THERE 2026-09-17** |

⛔⛔ **THE SECOND `test_sqlite_harness_legs` FLOOR WAS MISSING FROM THIS TABLE, AND `W-sync` CROSSED
IT WHILE `kScriptFloor` HELD.** ✔MEASURED 2026-09-17 (lane `mg`) with a replica of the test's own
recogniser: the live total was **5** — THREE from `real-examples/c/sqlite/build-and-test.ps1`, one
from `ssh-arm64-vps.ps1`, one from `ssh-macos.sh`. `W-sync` deletes two of the three suppliers, so
the union total goes to **3** against a floor of 4. ⇒ RETARGETED, never lowered: the rule is
extracted into `scanForWslExec` so it can be pointed at a fixture; a new
`TEST_F(HarnessLegs, TheWslExecRuleJudgesEverySyntheticShape)` drives ten SYNTHESIZED shapes with
both verdicts each (the splat bound to `-e`, the splat unbound, a binding outside the twelve-line
window, `-e`, `--exec`, bare, `--`, and three that must NOT be refused); and the tree census is now
attached to the file that SUPPLIES it — `build-and-test.ps1` must supply ≥ 3, the same three it has
always supplied, no longer summed with suppliers that have left. A union floor of 4 was satisfiable
with one supplier at zero, which is the hazard the per-root floors elsewhere here exist to close.
★ This also discharges item 9's *"the commit that deletes the file must give the exemption a
SYNTHETIC fixture"*.

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

⇒ **STILL NOT RETARGETED AFTER `W-build`/`W-test`/`W-sync`/`W-cilegs`, for the same reason.**
✔MEASURED 2026-09-17 at the commit that landed those four: the `scripts/`-only walk counts **34**
against a floor of **16**, so those waves do not take it under and the per-root shape above would
still be asserting something no commit has made true. **The retarget belongs to the commit that
takes the count under 16**, exactly as this row says. What DID move in those waves is the SECOND
floor in the same file, `invocationsSeen` — see item 8.

**Three assertions re-point with it**, all naming `scripts/ssh-arm64-vps/ssh-arm64-vps.ps1`:
- `TEST_F(HarnessLegs, TheRecordedIdentityFlagIsNamedInExactlyOneFile)` — two references, both in
  the measured narrative of what the defect cost;
- `TEST_F(HarnessLegs, NoScriptInvokesWslWithoutExec)` — the `-e`-bound splat is described as *"how
  `scripts/ssh-arm64-vps/ssh-arm64-vps.ps1` passes"*, i.e. the rule's only live example of the
  accepted shape.

⚠ **THE FIRST BULLET MIS-ATTRIBUTES, AND THE CORRECTION MATTERS TO WHOEVER GREPS FOR IT.**
✔MEASURED 2026-09-17: `TheRecordedIdentityFlagIsNamedInExactlyOneFile` ENDS before those two
references begin — both sit in the narrative comment block that opens section 9 of that file
(*"NOTHING INVOKES `wsl.exe` WITHOUT `-e`"*), between the two tests, not inside either. The second
bullet is right: two more references sit inside `NoScriptInvokesWslWithoutExec`'s own failure
messages, and a fifth in its non-vacuity comment. All five re-pointed in the `W-sync` commit; the
narrative measurement is KEPT and marked as history, because it is why the rule exists.

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
| `.claude/skills/dss-cycle/references/round-gate-and-ci.md`, the macOS `run_gate_guard` paragraph of the CI-measurement block | `remote-leg.sh` passes `-LE repo-guard` while `wsl-leg.sh` defaults `DSS_LEG_GUARDS` to `1`; the 2207 / 2206 / 2167 split | `W-sync` | that a repo-guard is HOST-INDEPENDENT, so its leg set is a CONFIG decision, and a leg that skips it cannot see a guard-only defect — **re-derive the set from the run in front of you** |
| `.claude/skills/dss-cycle/references/triggers-and-hard-stops.md`, the *"local gate cannot substitute for CI"* hard-stop bullet | the same two script names | `W-sync` | the same principle |

✔RE-VERIFIED 2026-09-25 (P68 round 12's independent audit): neither block names the two scripts any
more and neither carries a `⏳ SCRIPT-ERA` marker — the skills rewrite kept each block's principle
(the fourth column; both files state that a repo-guard is HOST-INDEPENDENT) and dropped the
script-era text. So these two rows need no deletion-wave edit; they stay as the record of what had
to survive. (Until that date this paragraph claimed both blocks carried the marker, which no block
did, so a deletion commit grepping `SCRIPT-ERA` would have found nothing and read it as done.)

★ **The reason worth carrying into the commit message:** that leg-set divergence existed BECAUSE three
hand-written scripts each decided it independently, with different defaults — which is why the skill
asserted for a year that guards ran on *"exactly one local host"* when they ran on two. **One tool
reading one config cannot have that class of bug.**

⚠ **`W-guardrunners` inherits this row**, because whichever wave moves the surviving guards to
`.harness-config/runner/actions/<name>/<name>.yml` is the wave that decides their leg set in config —
and that is the moment the durable sentence above becomes checkable rather than advisory.

## 13. `run_gate_guard` is a COIN FLIP on the WSL leg — measured 2026-09-16 (P68 round 2, orchestrator)

**Wave: `W-test` (`run-gate`).** This is the first measured cost of keeping the script alive, as opposed
to the standing 252 s it charges every gate run.

✔MEASURED at this commit's tree, WSL x86_64: `run_gate_guard` went **RED twice and GREEN on the third
attempt** on the **Release** leg, and green first time on the **Debug** leg, with nothing in the tree
changing between runs. The failing arms are `40-sh-challenger-vs-sh-holder` and
`41-ps1-challenger-vs-sh-holder`: a challenger that must be REFUSED (exit 5) while a holder is live
exited 0, RECLAIMED the holder's log path, and wrote `CHALLENGER-RAN` into a log that had to carry
`LOG-HOLDER-STARTED`. **A live gate's evidence log was overwritten under it** — so the guard is right
and the red is real.

**Why it happens, and why it is a WAVE item rather than a fix:** run-gate keys process IDENTITY on
`ps -eo lstart`, an absolute wall-clock value, and `lstart` is `btime + starttime/HZ` with `btime`
recomputed from the realtime clock — so a CLOCK_REALTIME step rewrites the creation time of every LIVE
process, and `run_gate_owner_stale` then reads a live holder as a recycled pid. The red run carried
**4** `clock stepped` reports from the proof's own instrumentation (`took ?(the clock stepped back)s`)
against **0** in each green run. The host is this WSL2 box's known ±34.47 s CLOCK_REALTIME oscillation.

| | Debug | Release |
|---|---|---|
| `clock stepped` reports in the run | 0 | 4 (red) / 0 (green, 3rd attempt) |
| `run_gate_guard` | passed | failed, failed, passed |

★ **THE POINT FOR THIS FILE:** the sound fix — key identity on `/proc/<pid>/stat` field 22, ticks since
boot, which no realtime step can move — has to land in BOTH twins, needs a new guard arm and a
red-on-disable transcript, and then a **fresh eight-run gate**, because run-gate is what RUNS the gate.
That is a full round spent hardening a script this wave deletes. The row
`D-SCRIPT-RUN-GATE-LOG-HOLDER-IDENTITY-KEYS-ON-A-WALL-CLOCK-A-STEPPING-HOST-REWRITES` carries the
decision and names the fix, **so that if `W-test` slips out of this PR the fix happens instead of the
knowledge evaporating.**

✅ **THE QUESTION THIS SECTION RAISED IS ANSWERED, AND THE ANSWER IS WHY `W-test` STAYS ON SCHEDULE.**
It asked whether DssHarness's own run verification keys on a wall clock, because inheriting the same
defect would make this deletion a lateral move. Reported to the operator; repo-harness measured it and
**had the identical bug** — its `RunLock` recorded a wall-clock instant and compared it against
`Process.StartTime`, which .NET derives on Linux the same way `ps lstart` does, under a one-second
tolerance that absorbs nothing. On a clock-stepping host every live holder read as dead, so a second
sync or build could start on a tree a live one was still writing, with no warning. Fixed upstream by
removing the clock from the identity: `{boot_id}:{starttime ticks}` from `/proc` on Linux, the kernel's
recorded creation time elsewhere, compared exactly, and the contention sampler had the same fault
independently and took the same fix. ⇒ **`W-test` routes through a lock that does not share our bug.**
⚠ It is not released yet — it is on repo-harness's `consumer-findings-0-5-4`, so the wave that lands
`W-test` must confirm the deployed version carries it rather than assuming this paragraph still holds.

✅ **CONFIRMED AGAINST THE DEPLOYED 0.5.5 BEFORE `W-test` LANDED, 2026-09-17**, not against this
paragraph: `repo-harness/docs/architecture.md`, *Leg integrity → One run per build directory*, states
it as shipped behaviour — *"That stamp holds no clock. On Linux it is the boot this machine is on and
the tick within it the process started, read from `/proc`; on Windows and macOS it is the start time
the kernel records once at creation and never works out again. A start time recomputed from the
current clock — which is what `ps lstart` reports, and what adding `/proc/stat`'s `btime` to
ticks-since-boot produces — moves for every live process the moment the clock steps."* That is this
row's defect, named and fixed. ⇒ `W-test` landed, and with it the standing 252 s charge and the
`40-sh-challenger-vs-sh-holder` / `41-ps1-challenger-vs-sh-holder` coin flip.

## 14. Exit **21** is not a pass — a requirement with no consumer YET, which is why it is written here

**Wave: every wave that replaces a script with a DssHarness verb.**

DssHarness **SHIPPED** a shared exit code **21 (Incomplete)** in 0.5.5: the work ran, nothing failed, but
not every leg reached a verdict — distinct from 0 (all passed) and 20 (something failed). Its JSON ledger
gains a `complete` boolean beside `passed`. This closes the second of the two blockers this branch
reported, where `LegRunService` built its OK line from legs *reported on* while `Verdicts` marked
`SkippedUnavailable` as `IsFailure=false`, so a run that skipped every leg reported passed.

✔MEASURED 2026-09-16, and it is the reason this belongs in the deletion inventory rather than in a
fix: **there is no DssHarness invocation anywhere in `scripts/`, `.github/`, `cmake/` or `tests/`.**
Every reference is prose. So there is nothing here to correct today, and a requirement with no
consumer is exactly the kind that gets lost — the wave commit is its first reader.

**What every wave commit owes when it introduces one:**
- treat **21 as NOT a pass**. Any test of the shape "not 20" or "non-zero means failure" is wrong;
  the pass condition is `rc == 0`.
- when parsing the ledger, read **`complete` as well as `passed`**. `passed` alone repeats the defect
  the exit code was added to fix.
- ⚠ **If the gate ENUMERATES acceptable codes, 21 goes in the FAILING set by name.** Letting it fall
  through as "unknown" is how a new code becomes a silent pass the first time it is emitted — the
  same shape as the defect it was added to fix, one level out.
- ⚠ **An INTERRUPTED run (exit 130) also reports `complete: false`.** Before 0.5.5 it described
  itself as a clean pass of however many legs had finished, so a Ctrl-C mid-gate produced a green
  ledger. Anything reading the ledger rather than the exit code must therefore read `complete`, not
  `passed`, or Ctrl-C reads as success.

✔MEASURED 2026-09-17 at 0.5.5, with the upgraded tool reading this repository's own config:
`dssharness legs` answers `OK - 8 of 8 leg(s) can run`, exit 0, so no leg names a toolchain absent
from its own `os` — a contradiction that 0.5.5 turned into a hard config-read refusal and that had
been silently ignored until now. ⚠ **AND `legs` IS NO LONGER EXEMPT — this file said it was, and that is now false.**
It answered `OK - N of M` with **exit 0** when a host was unreachable, and this branch reported that
as the same shape exit 21 exists to fix. It was accepted: from **0.5.6** `legs` exits **21** when a
declared host did not answer, naming the hosts. A leg that cannot run because no host MATCHES it is
still exit 0 — that is a complete survey, honestly reported. ⇒ **gate on 21 from `legs` too.**

ⓘ `scripts/run-gate` already behaves correctly by construction — it requires `rc == 0` **and** a
tool-emitted success witness in the command's own output, so a 21 fails it twice over. That is a
property of the script `W-test` deletes, not of whatever replaces it.

## 15. Superuser: root in CI, a declared key for declared hosts, the prompt only at a terminal

**Wave: every wave that replaces a script with a DssHarness verb.**

0.5.5 makes an install that needs a superuser ask at the terminal, once per host, held in memory for
that one command and written nowhere. That is fine for a person and must never be what CI depends on.

✔MEASURED 2026-09-17, which is why this is a requirement and not a change: **no workflow under
`.github/` invokes DssHarness at all.** Every `sudo` there is a direct `apt-get` or `gem install` in
its own dedicated step on a GitHub-hosted runner, where the runner user already has passwordless
sudo. So nothing in CI can depend on being asked today, and nothing needs correcting today.

**What a wave commit owes when it introduces the first CI invocation:**
- **Run the harness as root** for any step needing a privileged install — that needs no password at
  all. Normally it does not arise, because dependencies are installed in an earlier step.
- **Do not feed a password in.** A run with no terminal, a run answering with `--json`, and a run
  given `--no-prompt` all refuse exactly as a run with no credential always has, so what a script
  parses keeps parsing. Make the STEP root rather than supplying a secret.
- For a **declared** host, the credential is `SUDO_PASSWORD` in that host's own `.env` under
  `.harness-config/`. ⛔ A session does not read `.secrets/` and never prints, copies or commits such
  a value; placing one is the operator's act, not a session's.

## 16. The FINAL DssHarness API — agreed 2026-09-17, **NOT YET DEPLOYED**

**Wave: every wave that replaces a script with a DssHarness verb, and deploy day itself.**

📄DOCUMENTED, not ✔MEASURED — **every line of this section is a promise until a version carrying it
is installed.** That distinction is not pedantry here: §16.3 below exists because this repository
already wrote one such promise in the present tense and retired a guard on the strength of it.

### 16.1 `{buildDir}` — the blocker, and the one-line change deploy day owes

    "test": { "all": { "runner": "ctest",
                       "args": ["--test-dir", "{buildDir}", "--output-on-failure", "--no-tests=error"] } }

Vocabulary: `{buildDir}`, `{treeDir}`, `{harnessDir}`, usable in `args` and in a new
`workingDirectory` key. A name outside it is refused when `config.json` is READ, listing what is
available. Expansion happens **after** `--filter` and `--exclude` are spliced in, so every argument
the runner sees goes through one rule — `filterArg` / `excludeArg` / `countPattern` are untouched.
⇒ this is what makes `dssharness test` reach its build directory instead of starting `ctest` at the
tree root and finding nothing (exit 8, ✔MEASURED 2026-09-17), and it is why the eight-run gate
cannot yet be driven through the tool.

### 16.2 `rebuildableFormats` — and **nothing is owed here**

The rebuild-input set is derived from the project's type (cmake / dotnet / dart). Override with
`projects[].rebuildableFormats: string[]`, entries being extensions or whole file names — `".cpp"`,
`"cpp"` and `"CMakeLists.txt"` all match. Non-empty REPLACES the language set; empty or absent means
the language set applies. **Empty is *say nothing*, never *match nothing*.** ★ A file with **no
extension is always a build input**, whatever any list says.

✔MEASURED at this commit (`git ls-files`, basename without a dot; churn against the merge-base
`adf254c7`): this repository has **four** extensionless tracked files — `DCO`, `LICENSE`, `NOTICE`,
`VERSION` — and **zero** of them changed across the whole PR. So the always-a-build-input rule costs
this consumer nothing, `VERSION` is covered on its own merits rather than resting on
`CMAKE_CONFIGURE_DEPENDS`, and **no override belongs in our config**. If a wave reveals a build that
reads something else, that is a finding to report, not an override to write.

⇒ a documentation edit stops forcing a clean rebuild, and the ledger now names **which** of the three
rebuild conditions fired, not only which file differed.

### 16.3 ★★★ `build` gets the moving-tree guard IT NEVER HAD — and this tree already claimed it did

repo-harness verified: `BuildService` fingerprinted the tree exactly **once, AFTER the build**.
Nothing watched a build while it ran, so a source edited mid-build produced a binary from a tree that
never existed and the leg reported `passed`. All three verbs now open the same guard scope:
`test` contention + inputs (unchanged) · `build` contention + inputs (**new**) · `run` whatever the
step declares (§16.4).

⚠⚠ **AND THAT FALSIFIES A CLAIM THIS BRANCH HAS ALREADY WRITTEN.** The `run_gate_guard` obituary in
`CMakeLists.txt` — by symbol, in the `line_endings_watchdog_sh_guard` block — states that *"DssHarness
`build`, `test` and `run` carry those refusals themselves now"*, of the moving-tree (exit 3) and
shared-build-directory (exit 4) refusals. **False for `build` at every version installed to date, and
false for `run` except where a step declares the keys.** ⇒ a guard was retired against a capability
the tool did not have. Nothing was harmed — ✔MEASURED zero tracked files moved inside any build
window — but the deletion's stated justification was a promise, not a property.

★ **THE RULE THIS PUTS ON EVERY REMAINING WAVE:** a deletion is justified by what the INSTALLED tool
does, or it states plainly that the replacement ships later. *"The verb carries it now"* is a claim
with a version attached. Write the version.

**Two consequences for whatever consumes a verdict:**
- **`build` can now answer `contended` (exit 4)** where it previously answered `passed`. ⇒ extending
  §14: if a gate ENUMERATES codes, **4 now belongs to `build` as well as `test`**, and the set of
  build verdicts is no longer closed at *passed / failed*.
- **A build whose tree moved records itself untrustworthy**, so the NEXT build starts from clean and
  says why.

✔RE-DERIVED at this commit, and it is why this is a requirement rather than a correction: there is
still **no DssHarness invocation anywhere** in `scripts/`, `.github/`, `cmake/` or `tests/` — every
hit is prose. The wave commit that introduces the first one is this rule's first reader.

### 16.4 Actions carry the guard properties — the caveat is WITHDRAWN

    steps:
      - name: <step>
        run: |
          <program> <args>
        successPattern: "<regex>"
        stallSeconds: 900
        watchContention: true
        requireInputsUnmoved: true

Not a special case: the same guard scope all three verbs use. ⇒ the standing constraint that *a unit
of work needing inputs held still belongs in `test`/`build`, never an action* is repealed.

⚠ **ONE LIMIT, and it fires at the worst possible moment.** `watchContention` needs a directory to
watch, which is **the leg's build directory**. An action run with **no leg** has none, and that
combination is refused **WHEN THE ACTION RUNS**, naming the key — deliberately not at config-read,
because reading an action file cannot know how it will later be invoked. ⇒ an action meant to be
invoked standalone (a manual runner is exactly this) must not declare `watchContention`: it would be
green in every check available beforehand and refuse at the one moment it exists for. No limit was
stated for `requireInputsUnmoved` — that is UNSTATED, not proven absent.

### 16.5 `sync` removes emptied directories — and it DOES close our case

The husk is invisible from the deleting side because `git rm` takes the directory with the last file,
so only the HOST keeps it; a manifest holds files, so a plan can delete every file a directory had and
never mention the directory. After deleting, `sync` removes directories a deletion emptied, walking
upward while parents empty too, and **reports each surviving one by name with what kept it**.

⚠⚠ **AN EARLIER DRAFT OF THIS SECTION SAID THE FIX DID NOT CLOSE OUR CASE. THAT WAS WRONG, AND THE
WAY IT WAS WRONG IS THE PART WORTH KEEPING.** The claim was that `sync.neverTransfer` protected the
`__pycache__` inside our two husks, so they were not empty and would survive the prune. ✔MEASURED on
the WSL host 2026-09-17:

    0 files  ./scripts/carriage-excludes/__pycache__
    0 files  ./scripts/check-carriage-paths/__pycache__

against **1–2 files in each of the other 29** `__pycache__` directories on that host. **The `.pyc`
were deleted.** They were never protected: repo-harness disclosed that the matcher compared the whole
relative path or an `entry + '/'` prefix — **ROOTED ONLY** — and ✔MEASURED, **all 17 of our entries
are bare names**, with no `./__pycache__` at our root. That entry protected **nothing at all**, and
two more (`node_modules`, `.claude/worktrees`) protected nothing either. What survives in our two
husks is an empty `__pycache__` and its empty parent: an ordinary husk, exactly what the prune takes.

⇒ **the eight go, and so do the two, and `scripts_index_guard` goes green on WSL** — on one condition.

### ⛔ 16.5a WE DO **NOT** TAKE THE `**/__pycache__` ENTRY WE WERE TOLD TO ADD

0.5.6 makes `**/name` mean that name at any depth (whole segments, so `**/cache` does not cover
`src/mycache`), and a bare name **stays rooted, deliberately**. The deploy-day list we were sent says
to add `**/__pycache__`. **For this repository that is backwards**, and the arithmetic is one-sided:

| | protect `__pycache__` at depth | leave it unprotected |
|---|---|---|
| cost | every husk persists on every host, for every `.py` we ever delete, until a human removes it by hand — and `scripts_index_guard` reds on each | one `.pyc` recompile per script per host, once |
| benefit | avoids that recompile | the prune closes the husk automatically |

We are deleting most of `scripts/`. ⇒ **leave it unprotected.** A trivial recompile is the price of a
guard that stays green without anybody SSHing anywhere.

### ★★ 16.5b THE ENTRY THAT *DOES* NEED `**/`, AND IT IS NOT `__pycache__`

✔MEASURED at this commit, redacted to a count and a depth because a session does not read `.secrets/`:
the bare `.secrets` entry protects the root instance, and there is a **second `.secrets` directory at
depth 3 under `.harness-config/` that it did not protect** — the very directory this inventory's §15
and DssHarness's own superuser design name as where a host credential goes. It holds only `.gitkeep`
today, so **nothing has leaked and nothing is owed as an incident**; the exposure is latent and becomes
real the first time anyone does what §15 says to do.

⇒ **the `**/` entries this repository actually needs are `**/.secrets` and `**/.env`** — not the one on
the list. ★ And the general lesson, which is why this sits in the binding map: **a protective rule that
silently matches nothing is worse than no rule**, because the name in the config file is read as
evidence that the thing is protected. Our config named `.secrets` and a reader would stop there.

⇒ **Reported back to repo-harness** as the suggestion that a bare `neverTransfer` entry with **no root
instance but N nested instances** should be named at config-read. That is our `__pycache__` case
exactly (root absent, 133 nested) and would have surfaced this the first time anyone ran it.

⚠ **ONE RESIDUAL TO MEASURE ON DEPLOY DAY, not to assume.** The prune is described as removing
directories **a deletion emptied**. Our two were emptied by an EARLIER sync, so a run that deletes
nothing inside them may never walk them. If the wave reports the eight and not these two, that is the
reason, and a one-off manual removal closes it. ⛔ Either way it is never a reason to lower a floor or
soften a structural guard: a guard disarmed to accommodate a husk on a remote host is the guard being
defeated by its own subject.

### 16.6 The doubled `DETAIL` column — and why it was not cosmetic

The ledger's `Detail` carried the timing mark **already composed** and carried the notes separately,
so a remote leg answering with `--json` handed back a string that the asking machine composed the mark
onto a second time — doubling once per machine the answer passed through. Fixed by separating data
from presentation: `Detail` is now what the leg said, notes travel beside it, and the mark is composed
only where the table is drawn. **The `--json` shape is unchanged.** ✔RE-DERIVED: nothing in this
repository parses it, per the scan in §16.3.

### 16.7 Deploy day, in order

1. `"--test-dir", "{buildDir}"` into `projects[].test.all.args` — **the orchestrator's one line**, and
   the one that unblocks the eight-run gate.
2. Nothing for `rebuildableFormats` (§16.2).
3. `sync.neverTransfer` gains **`**/.secrets` and `**/.env`** (§16.5b). ⛔ **NOT `**/__pycache__`**,
   which the list we were sent asks for and which §16.5a declines by measurement.
4. Open any enumeration of build verdicts to `contended` (§16.3); gate on **21** from `legs` (§14) and
   on **13** from `sync --dry-run`.
5. Re-run a deletion wave against a host and read what `sync` reports. **Expect all ten directories to
   go**, the two `__pycache__` husks included, since they are empty. ⚠ If the two survive, the reason
   is that the prune is scoped to what THIS deletion emptied and theirs was emptied earlier (§16.5) —
   a one-off manual removal closes it. ⛔ Not a reason to touch a floor either way.
6. Land the version-gated changes the lanes prepared: the retiring tools declared as actions, with
   `watchContention` / `requireInputsUnmoved` wherever a step needs what `run-gate` gave us.
7. Correct the obituary in `CMakeLists.txt` (§16.3) to the version-stamped form: **`test` carries
   these; `build` carries them from 0.5.6; `run` carries them where a step asks.**
8. Correct `scripts/check-diagnostic-codes/`'s docstring: the success-witness rule is **`test` and
   `run`**; **`build` witnesses by `buildOutputs`** and sets no pattern anywhere (§16.3).

## 17. What lane `m2` measured, and the four waves that CANNOT complete as written

**Added 2026-09-17, lane `m2`, at `9cac826b`** — a base that does NOT contain lane `mg`'s four landed
waves, so every figure here is over a **49-file / 52-directory** `scripts/`, not over the 34 / 42 in
§1's re-measurement. ⚠ Do not read the two sets of numbers as one series.

### 17.1 ⛔⛔ A ctest ENTRY MAY NEVER SPELL `dssharness` AS A BARE NAME
| Item | Value |
|---|---|
| Location | `CMakeLists.txt`, symbols `DSS_HARNESS_EXE` and `dss_add_harness_guard` |
| ✔MEASURED, WSL x86_64, `wsl.exe -e bash <file>` | the installed file is `<home>/.dotnet/tools/DssHarness` — **capitalised**, no lowercase sibling — and that directory is on the **login** `PATH` only |
| ⇒ | `command -v dssharness` fails under both shell classes; `command -v DssHarness` fails under a non-login one. Two independent failures, either hiding the other, and **invisible from Windows**, where PATH carries the directory and the filesystem folds case |
| The shape that survives it | `find_program(... NAMES DssHarness dssharness HINTS "$ENV{HOME}/.dotnet/tools" "$ENV{USERPROFILE}/.dotnet/tools")`, plus a REFUSAL entry when absent |
| Control arm | the same `find_program` with `HINTS` removed answers **NOT FOUND** on the same WSL shell — so the hint is load-bearing, ✔MEASURED through a throwaway project under `mktemp -d` |
| Wave | **every wave that registers a harness verb as a ctest entry** |

⚠ `dssharness legs` answered `OK - 8 of 8 leg(s) can run` while this was true, so `legs` does not see
it. Reported to the operator.

### 17.2 ⛔ `test-leg-tree.sh`'s `G` ARMS WERE A SECOND `run-gate`-CLASS FIXTURE, AND §5 DID NOT NAME IT
| Item | Value |
|---|---|
| Location | `scripts/leg-tree/test-leg-tree.sh`, the block headed *"a CONSUMER asks git only through leg_tree_git_unsteered"*, constant `EXPECTED` |
| Was | it `cp`'d the REAL `scripts/check-root-litter/check-root-litter.sh` into its fixture and drove three arms against it |
| ✔MEASURED in this lane's own gate | deleting that script turned `leg_tree_guard` RED at `cp: cannot stat …` with three arms at **rc 127** — an entry whose name says nothing about a retired fixture. Exactly §5's shape, one guard over, and this file did not record it |
| RETARGETED, never lowered | both consumers are now **SYNTHESIZED** by `_lt_write_consumer` — the correct shape and the historical DEFECT (git asked bare, its exit status never read) — so no real script can be deleted out from under the arms. `EXPECTED` **raised 27 ⟶ 30**: three paired negatives `N1a`, `N1b`, `N2`, each driving the bare consumer through the identical conditions |
| ✔MEASURED after | `test-leg-tree: OK -- 30 arm(s)`, exit 0, all six `G`/`N` arms green |
| Wave | `W-buildchecks`, in the same commit as `check-root-litter` |

### 17.3 THE FOUR SUBJECTS WHOSE HARNESS VERB DOES NOT COVER THEM — ⛔ STOP, per this file's own rule
✔MEASURED by diffing each script's checks against `dssharness <verb> --help` and running both sides.

| Subject | Verb | What the verb does NOT cover | Disposition |
|---|---|---|---|
| `check-anchor-registry` | `check-anchor-citations` | the markdown CELL-WIDTH / unescaped-pipe property over ALL of `.plans/` (✔MEASURED **340 tables, 4264 rows, 42 files**; `read-anchors --lint` sees the two registry files only); the RETIRED-ID matcher; the QUOTED-NOT-CITED expiry mechanism; every per-root collapse floor; the 21-arm self-test | guard KEPT; `anchor_citations_guard` added BESIDE it for the coverage the tool adds |
| `check-line-endings` | `fix-line-endings --check --all` | Check A (HEAD **blobs**) and Check B (index blobs) — the tool judges the working tree; Check C (a pinned source git calls binary); Checks E1/E2 (✔MEASURED **72 files with no declared ending**, considered and not judged; and untracked files); **Check F entirely** — *"a CR instrument that cannot see one"*, which is half the stated PURPOSE; `--files`; `SCAN_FLOOR`; the watchdog and its 5-arm proof | ⛔ NOT DELETED |
| `check-ninja-deps` | **none** | ✔MEASURED: all ten `dssharness help` topics grepped for `ninja\|depfile\|header dep` — **zero matches**. `build` checks PRODUCTS, not dependency RECORDS | ⛔ NOT DELETED |
| `lane-worktree` | `create-worktree` / `delete-worktree` / `list-worktree` | `--preserve-to`, the VERIFIED evidence copy — the harness can only refuse or delete; and the **seed-manifest write `scripts/lane-fold/lane-fold.py` reads to adjudicate a fold** | ⛔ NOT DELETED — deleting it breaks fold adjudication |

★ The path-budget and evidence gates DID port cleanly and are visible in `.harness-config/config.json`
as `worktrees.pathBudgetReserve` / `pathLimit` / `pathBudgetMargin` / `evidenceRoots` — ✔MEASURED,
`create-worktree` with a 20-character name refuses with the same 260-character reasoning.

### 17.4 `owning-tree` CANNOT RETIRE — §10's 🧠INFERRED consequence is now ✔MEASURED
Twelve directories that SURVIVE every wave import it: `check-anchor-balance`,
`check-diagnostic-codes`, `check-guard-output-encoding`, `check-pkg-pipeline`,
`check-plan-citations`, `check-scripts-index`, `check-shell-portability`,
`check-stale-refusal-citations`, `check-wrapped-anchor-ids`, `corpus-census`, `lane-fold`,
`refresh_landing_log`. It is not a script a verb replaces — it is a LIBRARY supplying `resolve()`,
`run_git()` and `root_arms`, the shared arm set each consumer's self-test runs against its own
resolver. No DssHarness verb offers any of the three. ⇒ **the ⟶ 25 directory target is ⟶ 26**, and
the ⟶ 8 FILE target is unaffected because `owning-tree` ships no `.sh` and no `.ps1`.
ⓘ `scripts/anchors/` retires the same way and for the same reason: its eight SHELL twins are the
door and are replaced, while `anchors.py` stays as the library `check-anchor-balance`,
`check-stale-blockers` and `lane-fold` import. A directory is not the unit of this migration; a
PROGRAM is.

### 17.5 `.harness-config/runner/actions` NOW HOLDS A SHELL PROGRAM, WHICH §9 SAID IT DID NOT
✔MEASURED: `macho-alias-ld64-matrix.remote.sh` moved there with mode **100755** (six tracked files
already carry that mode, so it is an existing convention, not a new one), and its local half —
which was pure base64-into-one-ssh-argument transport — is DELETED rather than ported. ⇒ §9's
*"`.harness-config/runner/actions` holds zero shell programs today … its per-root rule is
that the directory must EXIST, not that it must be non-empty"* is now false in the direction that
HELPS: a per-root floor of ≥ 1 is spellable for the first time.
⚠ `kScriptFloor` is still NOT crossed and still NOT retargeted: ✔MEASURED **37** against a floor of
**16** at this commit. §9's rule holds — the retarget belongs to the commit that takes it under 16,
which is the FOLD, not this lane.

## 18. Lane `mig` (P68 round 7): `scripts/` and `real-examples/` are GONE

**Added 2026-09-18, lane `mig`, on base `7df54cc1`.** Operator ruling: *"there is no `scripts/` directory
and no `real-examples/`"* — every program is either DELETED because a verb provably does its job, or MOVED
into `.harness-config/runner/actions/` as an action: one directory per program, holding `<name>.yml` and
every file it runs.

### 18.1 Disposition
| What | Where it went | How |
|---|---|---|
| 39 program directories under `scripts/` | `.harness-config/runner/actions/<name>/`, FLAT — siblings of the three actions already there | `git mv`, blob and mode preserved; each gained `<name>.yml` (its `# PURPOSE:` line read from the program) and a `predefinedRunners` entry |
| `scripts/README.md` | `.harness-config/runner/actions/README.md` | `git mv`; still a GENERATED index, now of actions (`check-scripts-index` owns it) |
| `real-examples/c/sqlite/` | `.harness-config/runner/actions/real-examples/c/sqlite/` | `git mv`; `real-examples/` and `c/` are GROUP directories, `sqlite` is the action (`sqlite.yml`) |
| `scripts/repo-secrets/repo-secrets.sh` | — DELETED | its own header: *"THIS FILE HAS NO CONSUMER"*; connection data lives in the main checkout's `.harness-config/sshItems/<host>/`, resolved by the tool for every worktree |
| `scripts/profile-compile/profile-compile-dispatch.sh` | — DELETED | it drove the retired ssh carriages and `run-gate`; a remote leg is `dssharness run profile-compile --legs <leg>` |

✔MEASURED: `git status` shows **89 renames and 2 deletions**; exactly **6** tracked files carry mode
`100755`, the same six as before the move. FLAT, not grouped, because every program loads its libraries as
SIBLINGS (`dirname(dirname(__file__))/<lib>/`) and that idiom survives a flat move unchanged.

### 18.2 The tree-identity rule was keyed on the directory being deleted
`owning-tree.MARKERS`, `leg_tree_dss_tree` and the walk inside `repo-tree.ps1` all recognised a DSS tree by
`.plans/` + `scripts/`. With `scripts/` gone NO ancestor matches, so every consumer's root refuses. The markers
are now `.plans/` + `.harness-config/` in all three owners (one per language; `repo-tree.ps1` gained the public
`Get-RepoTreeDssTree`). About 25 programs had computed their tree by counting `..` and named
`.harness-config/runner` after the move; each was routed through its language's owner instead, and THREE of
them failed SILENTLY rather than loudly (`check-shell-portability` scanned only `.harness-config/runner` and
still cleared its floor; the sqlite `.sh` driver took its "copied out of the repository" path; the `.ps1`
drivers' `Resolve-Path ../../..` succeeded on a directory that exists).

### 18.3 Floors — retargeted, never lowered
| Guard | Before | After | ✔MEASURED live |
|---|---|---|---|
| `check-scripts-index` | `SCRIPT_FLOOR = 12` over `scripts/` | `ACTION_FLOOR = 12` over the actions tree (groups recursed, reachability from `predefinedRunners` enforced) | 43 actions, 43 self-test arms |
| `kScriptFloor` (`tests/harness/test_sqlite_harness_legs.cpp`) | 16 over `scripts/` | 16 over the actions tree (skips `build/`, `artifacts/`, `__pycache__/`) | 32 besides the two drivers |
| `check-shell-portability` | floor 15 over the tree, but the move had narrowed the ROOT to `.harness-config/runner` | floor 15 over the whole tree | 21 = every tracked `.sh` |
| `check-guard-output-encoding` | floor 8 | floor 8 | 30 primaries (26 self, 2 transitive, 2 unprotected) |
| `check-plan-citations` `CODE_ROOTS` | named `scripts`, `real-examples` | names `.harness-config/runner/actions`, and skips `artifacts/` | 2140 citations over 210 documents; arm 13 plants at the harness's NEW path |
| `check-wrapped-anchor-ids` | 68 wraps / 30 files | 48 / 28 (burned down by the guard's own lowering verb) | — |

### 18.4 Corrections to §17
- §17.5 says `macho-alias-ld64-matrix.remote.sh` moved with mode **100755**. ✔MEASURED `git ls-files -s`:
  **100644**. The action therefore runs it as `bash ./macho-alias-ld64-matrix.remote.sh`, and its comment says so.
- §17.3's `check-ninja-deps` row says the tool has **none**. At 0.5.7 `help legs` says every cmake `build`
  reads `ninja -t deps` and reports a build whose records cannot be read as `unmeasured`. The action STAYS
  anyway: CI runs `ctest` with no DssHarness installed, so `build/ninja-deps-freshness` is the only place the
  property is checked on a CI leg.
- §17.1's CAPITALISED file is RELEASE-DEPENDENT. The shim is named after the package's `ToolCommandName`:
  `DssHarness` through 0.5.5, `dssharness` from 0.5.6 (📄 `src/RepoHarness.Cli/RepoHarness.Cli.csproj` at
  each repo-harness tag). ✔MEASURED 2026-09-19 on WSL x86_64 under 0.5.7:
  `<home>/.dotnet/tools/dssharness`, no capitalised sibling; on Windows `dssharness.exe`. The `NAMES` list
  already covers both spellings. The LOGIN-PATH half of §17.1 still holds: a non-login `which dssharness`
  exits 1, and only the login PATH carries `<home>/.dotnet/tools`.
- §17.5's `kScriptFloor` note ("NOT retargeted … belongs to the FOLD"): retargeted by this lane, because the
  `scripts/` walk found 0 after the move and the floor refused — correctly.
- §17.4's directory target: the end state is 39 moved program directories plus one grouped action, all under
  `.harness-config/runner/actions/`.

### 18.5 Rows, budgets and roots this record depends on
- `cmake/DssTestBudgets.cmake`: named rows **37 ⟶ 40** (`lane_worktree_guard` re-derived from the current
  suite; `leg_tree_guard` and `lane_fold_selftest_guard` added under the module's own tier rule; on
  2026-09-19 the two `link/` MSVC-witness rows re-derived and `core/test_include_path_rooted_resolution`
  named, all three from runs measured alone AND under gate-like load, because their rows had come from a
  cost-data average and each had timed out at 315 s in one full run).
- `anchors.citationRoots` gained `.harness-config` after its dangling citations were settled (1104 → 0 in this
  lane's files, measured with `check-anchor-citations` itself); then `packaging`, `integrated_tests` and
  `tests/examples` the same day, and on 2026-09-19 `tests` in place of `tests/examples` — checked under the
  EXACT rule of DssHarness round four (a round-four build over the main tree with this fold laid over it: zero
  unresolved or cut citations in any file this fold carries, and none in `tests`).
- ⚠ A remote leg cannot run an action until DssHarness round four ships: 0.5.7's `sync` withholds
  `.harness-config/runner/actions`. Every remote-leg claim about an action is BLOCKED on that release.

## 19. Lane `mig` (P68 round 8, part 4): no `.sh` and no `.ps1` under the actions directory

**Added 2026-09-22, lane `mig`.** Operator ruling of 2026-09-21, verbatim: *"I don't want .sh/.ps1 files inside
.harness-config\runner\actions. entrypoint is .yml, you can call .py files, BUT NOT .sh/.ps1 please. They are specific
per OS. I don't want this anymore"*. Every action's entry point is its `.yml`, and every step starts
`python3 <file>.py`. `check-scripts-index` refuses a `.sh` or `.ps1` anywhere under the actions root BY NAME (the
tool's run directories exempt), so the property is a gate and not a convention. Outside the ruling's subject and left
alone: `examples/c/project_prebuild_script_codegen/gen_answer_source.{sh,ps1}`, product test data its example runs.

★ **THE METHOD, for every pair:** a `.sh`/`.ps1` twin became ONE `.py` carrying the UNION of both twins' checks —
where they disagreed, the stricter rule — with every self-test arm ported under its old label, each negative
synthesized, and a red-on-disable proved THROUGH ctest (the mutant written into the file ctest runs, its md5 moved and
returned, the failing arm's name read, a named control green). The transcription tables (old check → new function,
old arm → new arm) are in the lane's findings, part 4 (P4-4 … P4-11), beside the eleven read-only analyses of the old
files they were written from.

### 19.1 Disposition
| Retired (34 files in 13 actions) | Now | Its own proof |
|---|---|---|
| `check-anchor-registry.{sh,ps1}` | `check-anchor-registry.py` | self-test, 60 arms (the 20 inherited + 40 new), run first on every invocation |
| `check-orphan-tests.{sh,ps1}` | `check-orphan-tests.py` + `allowlist.json` (the allowlist once, schema-checked) | self-test, 40 arms (12 + 28) |
| `check-line-endings.{sh,ps1}` | `check-line-endings.py` | `--selftest` 35 arms; `--selftest-watchdog` 6 arms |
| `lane-worktree.{sh,ps1}`, `test-lane-worktree.sh` | `lane-worktree.py` | `--self-test`, 154 assertions (81 ported + 73 new), 0 shell starts |
| `leg-tree/leg-tree.sh`, `leg-tree/test-leg-tree.sh`, `repo-tree/repo-tree.ps1` (and their two actions) | `owning-tree.py`, extended | `--self-test`, 54 arms |
| `cmake-import.{sh,ps1}` (the wrappers) | folded into `cmake-import.py` | `--self-test` 20 arms; `--prove-any-cwd <dsscp>` (19.2) |
| `profile-compile.sh`, `macho-alias-ld64-matrix.remote.sh` | `profile-compile.py`, `macho-alias-ld64-matrix.py` | differential runs against the old programs: identical output |
| `compile-bench.sh`, `corpus-census.{sh,ps1}`, `pragma-profile-census.{sh,ps1}` (launchers of the `.py` their `.yml` already started) | — deleted | the `.py` each `.yml` starts |
| `check-shell-portability/` (the whole action) | — RETIRED WITH ITS SUBJECT (19.3) | — |
| sqlite: `build-and-test.{sh,ps1}` | `build_and_test.py` + flat `sqlite_common`, `_compiler`, `_launch`, `_libs`, `_build`, `_smoke`, `_units`, `_verdicts`, `_report`, `_stage`, `_corpus`, `_procs` | `--self-test` = the driver's Step 0 (19.2) |
| sqlite: `base-harness.{sh,ps1}`, `check-source-coherence.sh` | `sqlite_base.py`, `sqlite_coherence.py` | self-tests, 134 and 32 arms |
| sqlite: `benchmark-speedtest1.{sh,ps1}` | `benchmark_speedtest1.py` | `--self-test`, 105 arms |
| sqlite: `test-confound-scope.{sh,ps1}`, `test-driver-contracts.{sh,ps1}` | `test_confound_scope.py`, `test_driver_contracts.py` | 158 arms (220 of the twins' 322 carried, 102 retired — 19.3); 81 arms (pins DC-01…DC-22, red arms RD-01…RD-42 each a mutation of a scratch copy of a driver module, 10 mutator arms) |
| sqlite: `test-mirror-regions.{sh,ps1}` (+ the resolver's `--check-regions`) | — RETIRED WITH THE SECOND DRIVER (19.3) | — |

### 19.2 ctest entries
| Entry | Change |
|---|---|
| `anchor_registry_guard`, `orphan_tests_guard`, `line_endings_guard` | ONE `${Python3_EXECUTABLE}` entry each, the same on every host; the `CMAKE_HOST_WIN32` dispatch and `find_program(POWERSHELL_EXE … REQUIRED)` gone with the twins |
| `line_endings_watchdog_guard` | the ONE program's `--selftest-watchdog`; labelled `repo-guard` by name |
| `lane_worktree_guard` | `lane-worktree.py --self-test`; the bash probe block and its refusal gone |
| `line_endings_watchdog_sh_guard` (Windows only), `leg_tree_guard`, `repo_tree_guard`, `shell_portability_guard` | RETIRED — 19.3 names what covers each |
| ★ `harness/sqlite_driver_selftest` (NEW) | `build_and_test.py --self-test`: the driver's Step 0 alone — both suites, every module's `--self-test`, the leg plan's self-test and lint — the same list and judgement a real run applies before it starts |
| ★ `harness/sqlite_benchmark_selftest` (NEW) | `benchmark_speedtest1.py --self-test` |
| ★ `harness/cmake_import_selftest`, `harness/cmake_import_any_cwd` (NEW) | the program's arms; and the path-base pin: a manifest written OUTSIDE the project root builds, with this build's dsscp, the same artefact byte for byte from the root, from a directory of `#error` look-alikes and from an empty one, while the root-relative manifest the tool wrote until 2026-09-21 fails from the last two, and a trap control proves the look-alikes are read when named |
⓪ Before this part NO gate ran any sqlite-harness self-test, the benchmark's, or `cmake-import` at all. The new
entries are not `repo-guard`: what they exercise depends on the host (process enumeration, the POSIX half in process
or inside WSL, a real compile), so every leg runs them.

### 19.3 What retired with its subject, and what covers the property now
- `leg_tree_guard`, `repo_tree_guard` (+ `DSS_PWSH_FOR_REPO_TREE_GUARD`, the probe, refusal and not-applicable
  branches) → `owning_tree_selftest_guard`: `owning-tree.py` carries every arm both helpers proved (the identity of a
  worktree git cannot follow, git asked through that identity, the one-root rule, the owning root of a nested copy) on
  every host. The helpers' carriage half (`leg_tree_prepare`/`restore`, the remote loader) had NO caller (✔ grep) —
  `dssharness sync` replaced it; nothing is needed.
- `line_endings_watchdog_sh_guard` existed only so the bash twin's bound was proved on SOME leg → the one program's
  watchdog, proved by `line_endings_watchdog_guard` on every host (arm W6: a probe blocked on a registered grandchild
  dies inside its budget and nothing survives holding its pipe).
- `shell_portability_guard` refused bash-3.2-fatal shapes in this repository's `.sh` programs → no `.sh` remains under
  the actions root, and `scripts_index_guard` refuses one by name; the one `.sh` left in the tree is an example's
  product test data, run by that example on the POSIX legs. Its `SCAN_FLOOR = 15` could no longer be met.
- The sqlite twins' parity machinery — `test-mirror-regions`, the resolver's `--check-regions`, `DSS_REGIONS`,
  `MIRROR_PAIRS`, `MIRROR_CASES`, the emitted shell forms (`--plan --format sh`, `--stage-build --format sh`, the
  monitor's `sh` format) — proved that TWO drivers agree; one driver has no counterpart to disagree with. Every mirror
  CASE with an answer is carried as a unit test of the ONE Python function (`sqlite_corpus.py`'s self-test: the 8
  corpus-engine cases, the two with stated answers and the six whose answers the old copies were measured to agree
  on); `confound-report` and its `crClean` by property (`test_driver_contracts.py` DC-17); and
  `unknown-library-provider` by `tests/harness/test_sqlite_harness_legs.cpp`, part (c) of
  `BothDriversRefuseAnUnimplementedProviderWithTheSameVerdict`, which runs the ONE decision on the case's six
  inputs and requires its answer byte for byte — restated for one driver (the retired answer told a reader to
  add the arm to both).
- The 102 cross-driver parity arms of the confound-scope suite and the region markers → one implementation the suite
  IMPORTS by name (a missing function fails the import — stronger than a substring match).
- The 27 `test-lane-worktree.sh` lines that mirrored a ported arm 1:1 for the `.ps1`, and the interpreter probes of
  every twin → the ported arms against the one implementation.

### 19.4 Floors — moved with their population, never lowered
| Floor | Before | After | ✔MEASURED at this record |
|---|---|---|---|
| `check-scripts-index` `ACTION_FLOOR` | 12 | 12, unchanged | 40 actions (the two tree-helper actions retired) |
| `check-guard-output-encoding` | 8 | 8, unchanged | 39 Python primaries probed (36 self, 2 transitive, 1 inventoried debt) |
| `kScriptFloor` (`tests/harness/test_sqlite_harness_legs.cpp`) | 16, over the `.sh`/`.ps1` population the ruling takes to zero | `kPythonFloor = 33`, over the `.py` population — the same half-of-the-population ratio as 16 of 32 | 66 tracked `.py` under the actions tree |
| the per-file WSL-invocation floor (≥ 3) | on `build-and-test.ps1` | on `sqlite_common.py`: `PosixSide`, the one route into WSL for the driver and the benchmark alike | exactly 3 |
| `check-shell-portability` `SCAN_FLOOR = 15` | — | RETIRED with its guard (19.3) | — |
| `check-line-endings` 1500, `check-orphan-tests` 150/12/150, `check-anchor-registry` 20/100/1500 and its root floors | — | unchanged, carried by the one program | green on this tree |

### 19.5 Defects the port found and fixed (each pinned by an arm that reds on its return)
- `make -n -B` is NOT write-free: GNU make runs a makefile's self-remake rule even under `-n`, and sqlite's
  `bld-dss/Makefile` re-runs configure — so the old drivers' recipe derivation RE-CONFIGURED the tree it was reading.
  The dry run now passes `-o Makefile` (arm n39b).
- `-D`/`-I` harvested from INSIDE a token: a path holding `-DailySoftware-` yielded the define `ailySoftware`. A flag
  counts only at a token start (arm n06b).
- The shared clone lock judged liveness by `ps -o lstart`, which WSL2 moves when the clock steps (25 s between two
  samples 0.5 s apart): a live reader looked dead and a writer was let in. Its owner file keeps the old four lines and
  gains `proc-start=<ticks>`, which the new code judges by. A FOUR-line owner (the retired bash driver's) is judged by
  its pid ALONE: ✔MEASURED 2026-09-23, a live WSL2 process's lstart read wrong in 84 of 1184 samples (±26 s, in step
  with `/proc/stat btime`, its start ticks unmoved), and the proof run's Step 0 caught the twin's lstart rule, kept
  until then for such owners, calling a live one dead (arm CL09). Without the fifth line a moved lstart cannot tell a
  clock step from a reused pid; a false hold is loud and names the lock entry to remove, a false steal is silent
  (arms CL09, CL10, CL10b; CL20 keeps a Windows host's by-name skips equal to the arms a POSIX run runs).
- `lane-worktree.sh` under an exported `GIT_DIR`/`GIT_WORK_TREE`/`GIT_INDEX_FILE` deleted an uncommitted edit and
  reported "VERIFIED absent"; removed a SIBLING lane through a link; deleted a locked lane's directory while its
  registration survived. Each is refused or completed and verified now.
- `check-source-coherence.sh` printed `checkout: matches` on an incoherent stage, hung on a trailing `--label`, and
  passed a stale `sqlite3.c` that was a symlink or unreadable (two false greens).
- `check-guard-output-encoding`'s probe imported each subject without `-B`, writing bytecode beside every probed
  program (arms 20, 20b).
- The PowerShell process sweep could not see a fixture running inside WSL (the launched sweep now enters the
  launcher's own kernel).
- `pragma-profile-census.py` could not decode `c.lang.json` under cp1252 and did not find `dsscp.exe`;
  `corpus-census.py` died with `NameError` at import in a tree without `parse_diagnostic.cpp`.
- `sqlite_compiler`'s candidate search missed every DssHarness build directory (`build/<processor>-<toolchain>-
  <config>`), while the benchmark searched them on its own: one owner then (`search_roots`). Since 2026-09-26 the
  driver takes only a named compiler, and since P68 round 13 the benchmark's `select_dss` too (`require_named`, the
  driver's own words), so the search -- `search_roots`, `find_candidates`, `select_compiler`, `format_candidates`,
  and the constants `SEARCH_ROOTS` and `BINARY_NAMES` -- was deleted with its last caller.
- `cmake-import` wrote paths relative to the PROJECT ROOT into a manifest that may live elsewhere (its own runner
  writes into the step's build directory): relative now to the MANIFEST's directory, absolute outside it.
- The driver's provenance count excluded CR-only changes by `diff --ignore-cr-at-eol --name-only`, which still lists
  them on git 2.43 (the WSL and arm64 hosts' git), and it compared git's QUOTED names with unquoted porcelain paths,
  so a real change to a quoted name read as clean. Both answers are read NUL-separated now (`--numstat -z`).
- The corpus loop's process sweep kept only its kills: a leftover it could not kill, and a sweep that could not look
  inside the fixture's kernel, reached the log and never the leg's verdict. Both are verdict hygiene now.
- `pid_alive` answered TRUE for an unreaped zombie on POSIX, against its own contract, so a lock whose holder
  crashed into a zombie read as held; Windows already answered "exited".
- One marker, three reading rules: the resolver stripped lines, the benchmark core found the marker anywhere, the
  driver anchored it. One rule now (`sqlite_base`), every reader through it.
- The `CloneLock` refused on an `os.name` test of its own, and the benchmark core spelled `.exe` keyed on the host.
  The lock is keyed on the driver's one host switch (`PosixSide`); the extension comes from the target format's
  own config (`outputExtension`) through the plan.
- `stage-zinc.py` imported the resolver with bytecode on, writing `__pycache__` into its own action directory on
  every run.

### 19.6 Rows, budgets and configuration this record depends on
- `cmake/DssTestBudgets.cmake`: the `leg_tree_guard` row left with its entry. Two rows, each granted by the
  orchestrator on 2026-09-22 under the module's own rule (a ceiling past 60 s takes a NAMED row), each with its
  measurement in a comment beside it: ★ `harness/sqlite_driver_selftest|230|230|230` — ✔MEASURED 38 s direct, 41–89 s
  through ctest on MinGW Debug and MSVC Release, 229 s once under heavy load; one ceiling on every class because the
  entry is Python, whose speed follows the machine's load and never the C++ build class. And
  `harness/test_sqlite_harness_legs` re-derived from its stale `|66|73|67` to `|66|108|215` (debug 215, the slowest
  healthy run; release 108; sanitized kept, as only CI builds it) — ✔MEASURED over four direct runs: the pins this part
  re-pointed or added take ~30 % of the binary's time and the 22 it never touched ~70 %, in every run, and one binary
  ran 83 s and 148 s minutes apart, so the rise is mostly the shared machine's load; the part's own cost is the new pins'
  Python inspector (~2–3.4 s per batch). The other two new `harness/*` entries stay in the unit tier's budget. The
  kept-name rows `lane_worktree_guard`, `orphan_tests_guard` and `lane_fold_selftest_guard` were re-measured on the new
  programs and reported, not edited.
- `.harness-config/config.json`: the runners `leg-tree`, `repo-tree` and `check-shell-portability` left with their
  actions; `check-anchor-registry`, `check-line-endings`, `check-orphan-tests`, `lane-worktree` and `cmake-import`
  gained `windows-x86_64-debug` beside `linux-x86_64-debug`, each PROVED first with `dssharness run <runner> --legs
  windows-x86_64-debug` (a `.sh` entry point could not run there; a Python one can). In `sync.neverTransfer` (the
  orchestrator's item, 2026-09-23): the bare `__pycache__` entry, which matched nothing, left, and `node_modules`
  became `**/node_modules`; the file's own comment gives each reason. ✔MEASURED `sync --dry-run`: neither warns
  after, and the transfer plan is unchanged.
- `.gitattributes`: the `.sh`/`.ps1` pins under `.harness-config/**` retired — the layout forbids the files.
- `sqlite.yml`: its one step runs `python3 ./build_and_test.py` on every leg, with no `runOn`; the success pattern
  is unchanged.

### 19.7 The hang question this port was the test of — answered, as measured
Before the port, `line_endings_guard` and `line_endings_watchdog_guard` — then `.sh`/`.ps1` twins, the PowerShell
pair on a Windows host — TIMED OUT under ctest 3 runs of 3 on main when the host was loaded, at "Enter-RepoTree (git
rev-parse…)" or at start-up, while passing by hand in about 4 s. Their replacement is ONE Python program that bounds
every wait (90 s, naming what it waited on) and starts every child with no inherited handle.
✔MEASURED 2026-09-22/23 on the lane's tree, `ctest -L repo-guard -j8` on this shared, loaded workstation, TEN runs
across BOTH build classes — MinGW Debug ×6 (an early read, three consecutive, two on the final tree) and MSVC Release
×4 (three consecutive, one on the final tree): 31/31 every time, 0 timeouts; `line_endings_guard` 3.3–17.7 s (the
slowest 17.7 s against its 90 s bound), `line_endings_watchdog_guard` 10.7–19.8 s. The hang class did not appear. Scope: Windows only, where the hang was;
the Linux legs' runs belong to the round's gate. 🧠INFERRED, not isolated: the PowerShell pair's start-up and child
handles under load as the cause — the question is closed by removing PowerShell from the path, not by a mechanism
proven alone.

### 19.8 The one sqlite driver, proven on both host kinds
The retired drivers were one per host kind: `build-and-test.sh` on a POSIX host, `build-and-test.ps1` on Windows.
✔MEASURED 2026-09-23: the ONE Python driver ran end to end on the final code on both, its own Step 0 first (both
suites, every module's self-test, the leg plan's self-test and lint), against sqlite `b943fa1288` (the fixture recipe
189 TUs / 36 defines, the CLI's 103 / 30), with the full `veryquick` tier:
| Host | Compiler | Verdict | Unit corpus per runnable leg (errors, every one a known non-DSS confound) |
|---|---|---|---|
| Windows (the POSIX half derived inside WSL) | the MSVC Release dsscp, named by `DSS_BIN` | 3 of 5 VERIFIED, 0 poisoned, exit 0, 1 h 11 min | elf64-x86_64 through `wsl.exe -e`: 9 / 394 845; elf64-arm64 through `wsl.exe -e qemu-aarch64`: 7 / 394 849; pe64-x86_64 native: 3 / 394 498 |
| WSL as a POSIX host | its own Release dsscp, rebuilt by Step 5 | 3 of 5 VERIFIED, 0 poisoned, exit 0, 1 h 47 min | elf64-x86_64 native: 8 / 394 845; elf64-arm64 under `qemu-aarch64`: 8 / 394 849; pe64-x86_64 under `wine`: 3 / 392 964 over 4 segments, three aborts proven not DSS by earned rows and their remainders named |
On both, the two macho legs BUILT and were skipped by `runOn=[darwin]`, and the sqlite3 CLI BUILT on 5 of 5 with its
14-assertion smoke gate green on the 3 runnable legs. The first WSL attempt refused to start: Step 0 caught the clone
lock still judging a four-line holder by its lstart line (19.5), and it was fixed and proven before the run was
repeated.

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
