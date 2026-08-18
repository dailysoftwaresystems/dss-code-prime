# Build-directory layout — ONE root, subdirectories for everything else

**Operator instruction, 2026-08-17:** *"EVERY build must be inside build directory (we can have
multiple subdirectories for distinct builds), but only 1 root build. Also, lane builds MUST be
cleared once everything is fine, so we keep the storage good and also organize build directories."*

Read this at step 5 (before creating any build tree) and at step 11 (before reporting a cycle
complete). It is part of the fail-loud gate, not housekeeping advice.

## The rule

1. **There is exactly ONE build root: `build/` at the repo root.** Nothing else at the repo root may
   be a build tree. `build-dbg/`, `build-rel/`, `build-lane-x/`, `build-wsl/`, `build-red/` are all
   violations of this rule by their existence, not by their contents.
2. **Every distinct build is a SUBDIRECTORY of that root**, named for what makes it distinct:
   `build/dbg`, `build/rel`, `build/wsl`, `build/lane-<id>`, `build/red-<mutant>`. Add a
   subdirectory; never add a sibling.
3. **A worktree gets the same rule applied at ITS root** — `<worktree>/build/<name>`. A worktree must
   never build into the main tree's `build/`, and the main tree must never build into a worktree's.
4. **Lane builds are TEMPORARY.** A `build/lane-*` tree is deleted as soon as that lane's work is
   folded AND the gate that covers it is green. **A cycle may not be reported complete while any
   `build/lane-*` survives** — same shape as the anchor-balance gate: the report is a receipt, so the
   check has to be mechanical.
5. **The same applies to agent worktrees.** A worktree whose work is folded is removed
   (`git worktree remove`), and its build tree goes with it. Verify the fold by **CONTAINMENT** of the
   lane's contribution, never by byte-identity with the main tree — later edits legitimately stack on
   top, so `diff -q` reporting DIFFERS proves nothing either way.

## Why one root, and this is measured rather than tidiness

A flat `build-*` namespace at the repo root means every consumer maintains its own private idea of
what a build directory is — `.gitignore`, each rsync invocation, each cleanup script, each doc, and
the `dss-state` driver's newest-CLI auto-pick. They drift independently, and **this repo has already
shipped a bug from exactly that drift**:

> ✔MEASURED, this cycle: an rsync exclude written as `build*` — unanchored — silently matched
> `src/program/build_scripts.cpp` and skipped it, so the WSL gate leg configured against a source
> tree missing a changed `.cpp`. The fix was to anchor it (`/build*`).

With a single root the exclude is `/build/` — one anchored path, no glob, nothing for a source file
named `build_scripts.cpp` to collide with. **The class of bug disappears rather than being fixed
again per consumer.**

The storage argument is real but secondary: ✔MEASURED 2026-08-17, eleven root-level build trees
totalling **54.3 GiB**, of which seven were single-cycle lane builds at ~5.6 GiB each — **39 GiB of
scratch that no gate would ever read again**. Lane isolation is cheap in disk only if the disk is
reclaimed.

## What this does NOT license

⛔ **Do not "fix" a violation by pointing two builds at one directory.** Concurrent Windows ctest and
the WSL leg are ALLOWED to run at once; serializing them is a workaround the operator rejected by
name. Distinct configurations get distinct SUBDIRECTORIES — the rule reorganizes the namespace, it
does not reduce the number of build trees or make them share one.

⛔ **Do not delete a build tree to make a gate figure reachable.** A gate that cannot be re-measured
because its build tree was cleaned is a gate that did not run. Clean AFTER the green, never before.

⛔ **A lane build is not a cache.** If you find yourself keeping `build/lane-x` "in case", the lane is
not folded — finish the fold.

## Migration status — NOT yet done, and it is not a `mv`

⚠ The live tree still uses the flat layout. **✔MEASURED 2026-08-17 (corrected): 19 files carry 33
reference lines** — `tools/run-gate.{sh,ps1}`, `tools/check-ninja-deps.py`, `tests/CMakeLists.txt`,
`tests/core/native_c_probe.hpp`, `tests/core/test_header_name_matching.cpp`,
`tests/harness/test_sqlite_harness_legs.cpp`, `real-examples/c/sqlite/build-and-test.ps1` (9 lines,
`build-rel`), plus narrative mentions in `src/core/types/parse_diagnostic.hpp`, a `.format.json`, a
`.lang.json` and an `expected.json`. Every hit is a genuine token — no `build-rel*` prefix collisions.

★★ **THE FIRST FIGURE WRITTEN HERE WAS WRONG AND IT INVERTED THE RECOMMENDATION.** This file
originally said *"47 files and 16 files"*, from a `grep -rl` whose scope **included the sibling build
trees themselves** — so generated output inside `build-lane-a/` counted as a reference to
`build-dbg`. ⇒ Scope a reference count to SOURCE, or it measures the artifacts of the thing you are
trying to count. At 47 files the migration reads as a cross-cutting change to defer; at **19 files,
mostly comments, with 7 operative scripts**, it is a single focused edit. **Measure before deciding
whether something is too big to do.**

⚠ The remaining hazard is unchanged and is the reason to sequence it carefully rather than to defer
it: a missed reference fails in the worst way available — a script that silently configures a NEW
empty build tree, then reports a pass over a scan of almost nothing. ⇒ **the migration's own gate is
that each moved tree be proven to have been READ** (subject binary mtime), never merely to exist.

### ★★ TWO RULES FOR DOING IT, both learned while classifying the 33 lines

**1. DO NOT REWRITE HISTORICAL MEASUREMENT PROVENANCE.** Most of the 33 lines are not configuration
at all — they are sentences like *"MEASURED 2026-08-10 (`build-dbg` at `3e86a187`, elf64-x86_64)"* in
`parse_diagnostic.hpp`, `entry_shape.hpp`, four `.format.json` files and three `expected.json`
`$comment`s. **That measurement was taken in a directory called `build-dbg`, and it still was.**
Renaming it inside the record would falsify the provenance to tidy a path — the opposite of what
those sentences are for. Change *configuration*; leave *history*. The same applies to the incident
narration in `tools/run-gate.{sh,ps1}`, which recounts a `cd build-dbg` that actually failed.

**2. EVERY EDIT MUST BE TRANSITION-SAFE — there is no flag day.** A reference rewritten to the new
path alone breaks every run made before the physical move; one left on the old path alone silently
follows a tree that is being deleted. So each site spans BOTH layouts with the new path FIRST, and
**existence decides, never a version flag** — the list follows the tree instead of having to be kept
in sync with it. ✔Applied: `tools/check-ninja-deps.py` picks `build/dbg` if it exists else
`build-dbg` (and returns the NEW path when neither does, so it fails loud on a missing tree rather
than sending the reader after a deliberately removed one — both arms measured); the sqlite harness
searches `build/rel`, `build/dbg`, then the three legacy roots, and a from-scratch build lands in the
new layout.

Tracked by `D-BUILD-LAYOUT-FLAT-ROOT-BUILD-DIRS-NOT-MIGRATED`. Sequence it for a quiet tree — no
lanes in flight, no gate mid-run — because the migration edits the very scripts the gate runs.
⚠ Until it lands, the rule above still governs **new** build trees: create them as
`build/<name>`, and do not add another root-level `build-*`.
