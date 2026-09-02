# Workflow recipes, the .plans system, and honest status

## 10. Common Workflow Patterns

### 10.1 Adding a new public type / class

1. Header in `src/core/types/foo.hpp`.
2. If non-template + has out-of-line members → `.cpp` next to it, registered in
   `src/core/CMakeLists.txt`. If header-only → no `.cpp`, no CMake change.
3. `DSS_EXPORT` on the class (if non-template) or omit (if template/inline-only).
4. Test file `tests/core/test_foo.cpp` registered via `dss_add_test`.
5. Update `.plans/01-tree-node-model-plan - ok.md`'s §0 status + §7 row table (if it's a sub-plan
   deliverable).

### 10.2 Adding a typed view

See the cookbook in `docs/tree-model.md` §4. The full WhileStmtView template is there as
a drop-in starting point. Key steps:

1. Add the rule-name constant to `well_known_names.hpp` if it'll be referenced elsewhere.
2. New header in `src/core/types/`, public unchecked ctor + `static optional<View> from(...)`.
3. Test against a hand-built tree via `RawTreeBuilder` or `SchemaTreeBuilder`.

### 10.3 Adding a new `.lang.json` grammar

See the cookbook in `docs/language-config-spec.md` §7. The full Calc-language template is
copy-pasteable and verified by `GrammarSchema.DocsCookbookCalcExampleLoadsCleanly`. Drop a
new file in `src/dss-config/sources/`, load via `GrammarSchema::loadShipped("yourname")`.

### 10.4 Adding a diagnostic code

1. Add the enum value to `DiagnosticCode` in `parse_diagnostic.hpp`.
2. Add the name string in `parse_diagnostic.cpp`'s `diagnosticCodeName` switch.
3. Emit it from wherever the condition arises.
4. Add a test that triggers the emission and asserts the count exactly.

### 10.5 Driving `TreeBuilder` from tests

Use `ToyHarness` for the schema + source buffer (or `loadShippedHarness` for the on-disk
pipeline). Use `TokenSeq` for sequential token emission. Open frames via the RAII `OpenScope`
guards. For broken-path tests where you want frames open at `finish()`, hold the guards in a
heap-allocated `std::vector` and reset it AFTER `finish()` (see
`BrokenPath_UnclosedScopesAtEof` in `test_tree_end_to_end.cpp` for the canonical pattern).

---

## 8. The `.plans/` System

Internal design records live under `.plans/`. **They are NOT user docs** — the user-facing docs
are in `docs/`. The plans capture:

- The roadmap (`.plans/00-compiler-implementation-plan - tbd.md`).
- Sub-plans per major area (`.plans/01-tree-node-model-plan - ok.md`, `.plans/02-schema-expressiveness-v2-plan - ok.md`).
- Status snapshots at the top of each sub-plan (the §0 status tables).
- **Honest deviation lists** — the §0 deviations document every "the plan said X but we did Y"
  call with a reason. This is load-bearing for future contributors.

**Plans rot.** When status changes, update the plan in the same commit. Never let the plan
disagree with what's in `src/` — agents and contributors read it as canonical.

---

## 11. Status — WHERE TO GET IT, NEVER WHAT IT IS

★★★ **THIS SECTION DELIBERATELY CARRIES NO STATUS FIGURES, AND THAT ABSENCE IS THE POINT.**
A skill reference is served to a cycle as CURRENT truth. Nothing dates it, nothing re-measures it,
and no gate reds when it diverges from the tree — so a status figure written here is a claim with
no owner and no expiry. ✔MEASURED 2026-08-24: the T0–T12-era snapshot this section used to hold had
gone wholesale false while still reading as present tense. It asserted *"Semantic analyzer, IR,
optimizer, codegen, linker. **None exist.**"* and *"no real tokenizer exists"* — at a commit where
`src/analysis/semantic`, `src/analysis/lexical`, `src/tokenizer`, `src/hir`, `src/mir`, `src/lir`,
`src/opt`, `src/link` and `src/asm` all exist and DSS compiles and links SQLite on five target legs.
It also called *"the tokenizer phase opens next"* the biggest near-term risk, roughly thirty cycles
after that phase closed.

⚠⚠ **AND THE OBVIOUS REMEDY IS MEASURED NOT TO WORK — THAT IS WHY THIS IS A POINTER AND NOT A
DATE-STAMP.** A previous pass did try annotating the rotting number instead of removing it, leaving
*"(historical v1/v2 baseline; the current suite is 604 — see plan-00 §0.)"* beside a stale 531.
**The annotation then rotted too**: the suite is four figures now, so the reader met two wrong
numbers where before there was one, and the caveat lent the second one credibility. A caveat beside
a stale figure is just a second stale figure. ⇒ the repo's own standing rule is the only stable
form: *"never re-quote a gate figure — RE-MEASURE at the commit that carries it."*
Species: [[D-PLANS-SKILL-REFERENCE-ASSERTS-UNRECHECKED-STATUS]].

### Where status actually comes from

| You want | Ask this | Never ask |
|---|---|---|
| % complete, empirical C coverage, plan-23 arc %, SQLite-readiness, the cross-target emit/run matrix, an ETA | the **`dss-state`** skill — it runs a 104-probe C-feature battery through the REAL CLI and re-derives every axis at the commit you are standing on | any figure written in a document |
| What the last cycle did, what it owes the next one, the live operator queue | `.plans/_handoff.md` — **READ FIRST**, rewritten every cycle | a plan's §0 summary, which lags |
| Whether one specific defect is open | that row's own `Status` cell — `bash scripts/anchors/read-anchor.sh <ANCHOR>` prints it, and finds the row in whichever of the three registries holds it | any list, queue or summary that names the row |
| Whether it is OPEN vs GATED, and its priority | the same row's `Status` and `Priority` columns, explicit since 2026-09-01 | the glyph leading its `Trigger` prose, which is a second copy the gate cross-checks but does not read |
| Anchor open/closed counts | `python scripts/check-anchor-balance/check-anchor-balance.py` | a count quoted in prose |
| Suite size, pass count, timings | run the gate | this file |

### What this file may state, because a reader re-derives it in one command

**Structure, not status.** Which subsystems exist is `ls src/`; which languages ship is
`ls src/dss-config/sources/*.lang.json`; which targets and formats exist is `ls src/dss-config/targets/`
and `ls src/dss-config/object-formats/`. Those are stable facts about shape, they answer *where does
this live*, and §1–§10 above document them. A sentence here earns its place only if a reader can
check it against the tree faster than they can doubt it.

⇒ **If you are about to write "N tests", "X% done", "not yet implemented", or "the next phase is Y"
into this file — don't. Name the instrument instead.** Any prose that survives here despite being a
status carries an explicit re-measure obligation naming the instrument that settles it.

---

## 13. Contribution Checklist

Before declaring a phase done:

1. **Run the full ctest suite.** `ctest --test-dir build --output-on-failure` — must be 100%.
2. **Every new test must use STRICT asserts** per §7. No `EXPECT_GE` on known counts. No
   substring `find` where full equality would work.
3. **Update `.plans/`** — flip the row status, update test counts in §0, add the new file to
   the file list.
4. **Update `docs/`** if you've added a new public type that a contributor would discover.
5. **Update this skill** if you've changed a convention (the fatal pattern, the testing rules,
   the strong-IDs list, the well-known names).
6. **Run a `/pr-review-toolkit:review-pr`** if the change is substantive. The review-and-fix
   cycle is part of the project's quality discipline.
7. **No new dependencies** without explicit approval. `nlohmann/json` and GoogleTest are the
   only third-party libraries.
8. **No `<cassert>`** — use the `*Fatal` pattern.
9. **No backwards-compatibility shims** for unreleased internal types. If you rename
   `nthVisibleChild`, update every caller in the same commit.

---
