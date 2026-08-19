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

## 11. What's Done vs. What's NOT (Honest Status)

### Done (T0–T12, 12/12 of the tree/node sub-plan)

- The tree/node model: `Tree`, `Node`, `NodeKind`, `NodeFlags`, strong IDs, interners.
- Schema loader + `GrammarSchema` (loads `.lang.json` from disk or text).
- `TreeBuilder` with RAII scopes, recovery, EOF synthesis, diagnostic emission.
- `TreeCursor` (CST + AST modes, opaque Bookmark, cycle-capped depth/parent walks).
- Visitor walks (`walkPreOrder`, `walkPostOrder`, `WalkAction`).
- `NodeAttribute<T>` with sparse↔dense auto-promotion.
- Seven typed views + `well_known_names.hpp`.
- End-to-end integration test exercising the full stack against `toy.lang.json`.
- Onboarding docs (`docs/tree-model.md`, `docs/language-config-spec.md`).
- **531 ctest cases across 26 suites, 100% pass.** (v1 T0–T12 baseline + v2 PR0–PR8 + SH1–SH4.) (historical v1/v2 baseline; the current suite is 604 — see plan-00 §0.)
- **Schema-expressiveness v2 (PR0–PR8): done.** Operator precedence + arity (`OperatorTable`),
  contextual keywords + `reservedWordPolicy`, `scopeRequire` (anyOf/forbid/topMustBe/outermost),
  `TreeBuilder::Checkpoint` + speculative-alt loader plumbing, `lexerModes` + `LexerModeStack` +
  `modeOp`, `stringStyle` descriptor with `EscapeKind` + dynamic tag patterns. Two real grammars
  ship: `toy.lang.json` and `tsql-subset.lang.json` (empirical stress proves v2 is sufficient
  for non-trivial languages). See `.plans/02-schema-expressiveness-v2-plan - ok.md`.
- **Substrate hardening (SH1–SH4): done.** SH2 confirmed the multi-OS CI matrix (Linux/GCC,
  Linux/Clang+ASan, Windows/MSVC, macOS/AppleClang). SH3 closed the cross-tree `NodeId` caveat
  (`NodeId.arenaTag` + tag validation in `NodeAttribute<T>` and `Tree::node_`). SH1 ships
  `scripts/refresh_landing_log/refresh_landing_log.py` for plan-doc hygiene; SH4a wires its `--check` into CI. SH4b
  adopted `switch`/`case`/`default`/`break` in c-subset via shape-based positioning. SH4c
  pinned multi-level AltChoice routing.
- **Three shipped `.lang.json` configs**: `toy.lang.json`, `c-subset.lang.json`,
  `tsql-subset.lang.json`.

### NOT done yet

- **The lexer.** The current code drives `TreeBuilder` from hand-constructed tokens — no real
  tokenizer exists. Substrate hardening is done; tokenizer phase is the next-up parent-plan
  phase.
- **The parser.** `TreeBuilder` validates *within* a frame but the sequence/alt/repeat shape
  walker isn't fully consumer-driven. The "is `open(varDecl)` valid here?" check is the
  parser's job. The schema cursor walks correctly through arbitrary AltChoice nesting (SH4c
  pin) — that mechanism is ready for a real parser.
- **Most real-language grammars.** Only `toy`, `c-subset`, and `tsql-subset` ship. C#, Dart,
  SQLite are promised but not authored. Float-literal styling and ternary operators are not
  yet schema-expressible (v3 candidates).
- **Semantic analyzer, IR, optimizer, codegen, linker.** None exist. `src/core/compiler.cpp`
  is a placeholder.
- **Cross-platform — partial.** CI matrix exercises Linux/GCC-13, Linux/Clang-19+ASan,
  Windows/MSVC, and macOS/AppleClang on every PR (SH2 + SH4a). iOS / Android / WASM are stated
  goals; untested. Local dev convention on Windows is MinGW GCC 13.2; production code paths
  are toolchain-portable (proven by green CI on all four legs).

### The biggest near-term risk

The tokenizer phase (parent plan #5) opens next. v2 has been validated against tsql-subset
end-to-end at the schema level (PR7) and c-subset adopted shape-based switch/case (SH4b), so
the substrate is empirically sufficient for non-trivial languages. The first real test will
be when an actual lexer drives `TreeBuilder::pushToken` from real source bytes — that's where
PR5 (lexer modes) and PR6 (string styles) get exercised under non-stub-driver pressure for
the first time. Expect a v2-fixup pass once the tokenizer surfaces gaps.

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
