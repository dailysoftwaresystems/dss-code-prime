---
name: dss-code-prime
description: >
  DSS Code Prime repository guide — universal config-driven C++ compiler frontend. Covers
  the tree/node model, schema-driven grammar configs, TreeBuilder, TreeCursor + visitor walks,
  NodeAttribute<T> side-tables, typed views, diagnostic discipline, the .plans/ system,
  CMake build wireup, coding conventions, and (critically) the strict-assertion testing
  posture that makes every regression visible.
user-invocable: true
argument-hint: "[topic or question]"
---

# DSS Code Prime — Repository Guide Skill

You are assisting with the **DSS Code Prime** project. The authoritative onboarding docs live at
[`docs/tree-model.md`](../../../docs/tree-model.md) and [`docs/language-config-spec.md`](../../../docs/language-config-spec.md);
the internal design records live under [`.plans/`](../../../.plans/). This skill is a comprehensive
reference — when this skill and a doc disagree, **the doc and the plan win**.

---

## 1. Overview

- **DSS Code Prime** is a universal, configurable C++ compiler frontend. Both the source language
  AND the target platform are intended to be configurable — a single C++ engine compiles *any*
  defined language to *any* supported target.
- **Status today:** the tree/node model foundation (sub-plan T0–T12), schema-expressiveness v2
  (PR0–PR8), and substrate hardening (SH1–SH4) are all complete and shipping. The lexer,
  parser, semantic analyzer, IR, optimizer, and codegen layers do **not yet exist** — that's
  the next-up parent-plan phase #5. Everything in this repo is the substrate every later layer
  will build on.
- **Main technologies:** C++23, CMake 4.0+, FetchContent for `nlohmann/json` 3.12.0 and
  GoogleTest 1.17.0. Local dev on Windows uses MinGW GCC 13.2 (ucrt); CI exercises
  Linux/GCC-13, Linux/Clang-19+ASan, Windows/MSVC, and macOS/AppleClang on every PR.
- **Design discipline:** strongly-typed IDs, immutable post-build `Tree`, fail-loud invariant
  guards via local `*Fatal` helpers (no `<cassert>`), header-only templates where possible,
  config-driven languages (no compiled language definitions).

---

## 2. Repository Structure

| Directory | Purpose |
|-----------|---------|
| `src/core/` | The static `core` library — compiled into `libdss-code-prime.dll` |
| `src/core/types/` | The tree/node model and friends (22 headers, 11 `.cpp` files) |
| `src/source-config/languages/` | Shipped `.lang.json` grammar configs (`toy.lang.json` only so far) |
| `tests/core/` | GoogleTest unit + integration tests (one executable per file) |
| `docs/` | User-facing onboarding docs (`tree-model.md`, `language-config-spec.md`) |
| `.plans/` | Internal design records and roadmap (`compiler-implementation-plan.md`, `tree-node-model-plan.md`, `schema-expressiveness-v2-plan.md`) |
| `build/` | CMake build dir (gitignored) |
| `libs/`, `docker/`, `integrated_tests/` | Reserved for later phases |

---

## 3. Build System and Toolchain

- **CMake floor: 4.0** (`CMAKE_MINIMUM_REQUIRED`). Verified working with CMake 4.3.2 on Windows.
- **C++ standard: 23** project-wide.
- **Dependencies are vendored via `FetchContent`** — `nlohmann/json` v3.12.0 and GoogleTest v1.17.0.
  `nlohmann/json` is `PRIVATE` to the one TU that needs it (`grammar_schema_json.cpp`).
- **Tests use `dss_add_test`** — a project-local helper macro that registers a GoogleTest
  executable AND a ctest entry. One executable per test file (parallelism + per-file failure isolation).
- **Public include path** is set once via `target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)`
  — header-only additions (`tree_visitor.hpp`, `tree_attrs.hpp`, `tree_views.hpp`, `well_known_names.hpp`)
  don't need explicit registration.
- **Build commands** (Windows / PowerShell):
  ```powershell
  cmake --build C:\Source\DailySoftware\dss-code-prime\build
  cmake --build C:\Source\DailySoftware\dss-code-prime\build --target dss_core_test_tree_views
  ctest --test-dir C:\Source\DailySoftware\dss-code-prime\build --output-on-failure
  ```

---

## 4. The Tree / Node Model — Core Domain

This is the heart of the project. Read [`docs/tree-model.md`](../../../docs/tree-model.md)
before touching any of it.

### 4.1 Storage

| Type | File | Role |
|---|---|---|
| `Tree` | `tree.hpp/.cpp` | Immutable arena of `detail::Node` POD entries; one per parsed source |
| `detail::Node` (~40 bytes) | `tree_node.hpp` | Single struct stores every node — POD, no virtual dispatch |
| `detail::TreeData` | `tree.hpp` | Movable POD that `TreeBuilder::finish()` hands to `Tree`'s constructor |
| `NodeKind` enum | `tree_node.hpp` | `Internal` / `Token` (only two kinds — Error/Missing/Synthetic live in flags) |
| `NodeFlags` enum (bitmask) | `tree_node.hpp` | `EmptySpace`, `HasError`, `Missing`, `Synthetic`, … |

**Key invariants:**
- Slot 0 is the `InvalidNode` sentinel; real nodes start at index 1.
- Tree is immutable after `finish()`. All semantic annotation goes through `NodeAttribute<T>` side-tables.
- `NodeId` is a strong type wrapping `{uint32_t v; uint32_t treeTag}` with `valid()` predicate
  (`v != 0` == invalid; `treeTag == 0` == untagged literal). Equality and `std::hash` compare `.v` only.
- Cross-tree NodeId usage **is** detectable: `TreeBuilder::emit_` and `RawTreeBuilder` stamp every
  emitted `NodeId` with the source tree's id; `NodeAttribute<T>` and `Tree::node_` validate the tag
  on every access. Tagged-from-foreign-tree aborts with both ids in the fatal message; untagged
  literals (`NodeId{3}`) bypass the check (test ergonomics) but are still bounds-checked. See
  `docs/tree-model.md` §5 cross-tree guard section.

### 4.2 Strong IDs (`strong_ids.hpp`)

```cpp
DSS_STRONG_ID(NodeId);          // arena index
DSS_STRONG_ID(RuleId);          // interned rule name
DSS_STRONG_ID(SchemaTokenId);   // interned token-kind name
DSS_STRONG_ID(BufferId);        // source buffer
DSS_STRONG_ID(TreeId);          // tree identity (monotonic atomic counter)
DSS_STRONG_ID(DiagnosticIndex); // diagnostic table index
```

Every ID is a distinct struct — `tree.children(someRuleId)` won't compile.
Each has a `valid()` predicate; default-constructed is the invalid sentinel.

### 4.3 The Cursor (`tree_cursor.hpp/.cpp`)

`TreeCursor` is the stateful walker. **Two modes:**

| Mode | Visits |
|---|---|
| `CursorMode::Cst` | every node — including `NodeFlags::EmptySpace` leaves |
| `CursorMode::Ast` | skips `EmptySpace` ONLY. `Missing` and `Synthetic` ARE visible (load-bearing — downstream phases need to see them) |

Convenience entry points: `tree.cursor()` (CST) and `tree.astCursor()` (AST).

**Movement methods are `[[nodiscard]]`** — `gotoFirstChild`, `gotoLastChild`, `gotoNextSibling`,
`gotoPrevSibling`, `gotoParent`. The bool return says whether the move happened; discarding it
is almost always a bug.

**`Bookmark` is opaque** — private fields + `friend TreeCursor`. Carries the bound `TreeId`
for ABA protection. `cursor.restore(bookmark)` distinguishes three failure modes with distinct
fatal messages: invalid bookmark / cross-tree bookmark / stale bookmark.

### 4.4 Visitor Walks (`tree_visitor.hpp` — header-only)

```cpp
walkPreOrder(tree, [&](TreeCursor const& c) { … });          // whole tree
walkPreOrder(tree, startNode, [&](TreeCursor const& c) { …}); // subtree-bounded
walkPreOrder(tree.astCursor(), visitor);                      // AST mode

walkPostOrder(...)  // same shape; leaves before parents
```

- **Visitor returns `void`** (treated as `WalkAction::Continue`) **OR `WalkAction`**.
  Auto-detected via `if constexpr` — no signature ceremony.
- `WalkAction::{Continue, SkipChildren, Stop}`. `SkipChildren` is meaningless in post-order
  (children already visited) and silently becomes `Continue`.
- **Subtree-bounded:** the walk never ascends past `start`. Critical invariant — there's a
  depth-0 guard before sibling probes in both walks.
- **Zero allocations on the hot path** — verified by an `operator new` counter test
  (`test_tree_visitor.cpp:TenThousandNodeWalkAllocatesNothing`).

### 4.5 Side-tables (`tree_attrs.hpp` — header-only template)

`NodeAttribute<T>` is the **only** mechanism by which later passes annotate the tree. The Tree
itself stays immutable.

```cpp
NodeAttribute<TypeInfo> nodeTypes{tree};   // binds via Tree const&
nodeTypes.set(id, TypeInfo{…});
auto const& ty = nodeTypes.get(id);         // aborts on absent
auto const* t = nodeTypes.tryGet(id);       // nullptr on absent
```

- **Dual storage with auto-promotion**: starts as `unordered_map`, promotes to dense
  `vector<optional<T>>` when coverage ≥ 50% AND `tree.nodeCount() ≥ 16`. `clear()` resets to sparse.
- **Move-only** with custom move ops that reset the source's `denseCount_` + variant so
  moved-from state is observably consistent (rather than std-lib's "valid but unspecified").
- **Bounds-check + cross-tree-tag-check on every entry**. NodeIds carry a `treeTag` (set by
  `TreeBuilder::emit_` / `RawTreeBuilder::addNode`); foreign-tree usage aborts with both
  TreeIds in the fatal message. Untagged literals (`NodeId{N}` from test code) bypass the
  tag check but are still bounds-checked.

### 4.6 Typed Views (`tree_views.hpp` + `well_known_names.hpp` — header-only)

Seven views ship today. Each wraps `(Tree const*, NodeId)`:

| Layer | View | Rule / token kind |
|---|---|---|
| Token | `IdentifierView` | tokenKind == `Identifier` |
| Token | `LiteralView` | one of `{Int, Float, String, Char, Bool, Null}` Literal — caches `Kind` enum at construction |
| Rule | `BinaryExprView` | rule `"binaryExpr"` — `[lhs, op, rhs]` |
| Rule | `BlockView` | rule `"block"` — variable visible children |
| Rule | `FunctionDeclView` | rule `"functionDecl"` — `[name, paramList, body]` |
| Rule | `VarDeclView` | rule `"varDecl"` — toy-aligned |
| Rule | `ExprStmtView` | rule `"exprStmt"` — toy-aligned |

**Pattern (every view):**
- Public **unchecked** constructor `View(Tree const&, NodeId)`.
- Static factory `static std::optional<View> from(Tree const&, NodeId)` — checks rule /
  tokenKind only. Returns `nullopt` on mismatch.
- Structural accessors use `detail::views::nthVisibleChild` which **skips `EmptySpace`** —
  whitespace between role positions doesn't break role indexing.
- **Trivially copyable** POD layout (`Tree const*` + `NodeId` + maybe 1 byte for `LiteralView::Kind`).
- Lifetime contract: same as `TreeCursor` / `NodeAttribute` — caller keeps Tree alive.

### 4.7 Diagnostics

Every `Tree` built via `TreeBuilder` carries a `DiagnosticReporter`. Reporter dedups by FNV-1a64
hash of `(code, buffer, span, ruleContext)`.

Common codes (full list in `parse_diagnostic.hpp`):

| Code | When emitted |
|---|---|
| `P_UnexpectedToken` | Parser called `pushError` explicitly |
| `P_UnknownToken` | Schema couldn't resolve a lexeme to any token kind |
| `P_PrematureEndOfInput` | `finish()` ran with open frames — one per still-open frame |
| `P_BuilderInvariant` | Sequence/scope-stack invariant violated mid-build |
| `P_NoAlternativeMatched` | (reserved — parser-level; not emitted by T5 yet) |
| `P_UnmatchedClose`, `P_UnclosedScope` | (reserved — not emitted by T5 yet; future parser work) |

Tree-level: `t.diagnostics().all()` returns `std::span<ParseDiagnostic const>`,
`t.diagnostics().hasErrors()` is the quick yes/no. `t.flags(t.root())` carries `HasError`
when any descendant has errors (propagation done by `TreeBuilder`).

---

## 5. The Schema and `.lang.json` System

See [`docs/language-config-spec.md`](../../../docs/language-config-spec.md) for the authoring guide.

### 5.1 Loading

```cpp
auto loaded = GrammarSchema::loadShipped("toy");    // from src/source-config/languages/toy.lang.json
auto loaded = GrammarSchema::loadFromText(jsonText); // from an inline JSON literal
```

Both return `std::expected<std::shared_ptr<GrammarSchema const>, std::vector<ConfigDiagnostic>>`.
`loadShipped` walks parent dirs to find the config independent of cwd.

### 5.2 What the schema captures

- `tokens` — lexeme → list of meanings (multi-typed). Each meaning has `kind`, optional `flags`
  (`["EmptySpace"]`), `priority` (lower wins on tiebreak), `validScopes` (whitelist),
  `opensScope` / `closesScope`.
- `keywords` — `{word, kind}` reserved words.
- `scopes.validity[]` — `{scope, forbid}` blacklist rules (e.g. forbid `LtOperator` inside `Generic`).
- `shapes` — `{ sequence | alt | repeat }` grammar rules. `root` is the entry point.

### 5.3 Built-in token kinds (always pre-registered)

`Identifier`, `IntLiteral`, `FloatLiteral`, `StringLiteral`, `CharLiteral`, `BoolLiteral`,
`NullLiteral`, `Eof`, `Error`. Configs can reference these without declaring them in `tokens`.

### 5.4 Loader diagnostic codes

`C_MalformedJson`, `C_VersionMismatch`, `C_InvalidLanguageName`, `C_MissingField`,
`C_UnknownToken`, `C_UnknownShape`, `C_CircularShape`, `C_AmbiguousAlternatives`,
`C_UnclosableScope`. The full troubleshooting table is in `language-config-spec.md` §8.

### 5.5 Well-known names

`well_known_names.hpp` provides `inline constexpr std::string_view` constants:
- `dss::rules::{kIdentifier, kLiteral, kBinaryExpr, kBlock, kFunctionDecl, kVarDecl, kExprStmt}`
- `dss::tokens::{kIdentifier, kIntLiteral, kFloatLiteral, kStringLiteral, kCharLiteral, kBoolLiteral, kNullLiteral}`

Use these instead of bare string literals when referencing standard rule / token names —
single source of truth, no typo risk.

---

## 6. `TreeBuilder` — How Trees Get Built

```cpp
TreeBuilder b{src, schema};
{
    auto root = b.open(schema->rules().find("root"));
    auto stmt = b.open(schema->rules().find("statement"));
    auto vd   = b.open(schema->rules().find("varDecl"));
    b.pushToken(Token{...});
    // ... more pushToken calls
    // RAII: stmt and vd close as their guards go out of scope (LIFO).
}
Tree t = std::move(b).finish();
```

- **Single-use, non-copy, non-move.** `finish()` is `&&`-qualified.
- **`open(rule) &`** returns an RAII `OpenScope` guard. The `&` qualifier disqualifies rvalue
  builders so `OpenScope` can't outlive a temporary.
- **`pushToken(tok)`** validates within the current frame: lexeme resolution, scope filter,
  priority tiebreak, scope-stack mutation, `EmptySpace` flag propagation. With no open frame,
  emits `P_BuilderInvariant`.
- **`pushError(span, expectedRule?, expectedToken?, note)`** is the parser's explicit
  "this is wrong" signal. Inserts an Error leaf, emits `P_UnexpectedToken`, propagates
  `HasError` up to root.
- **`finish()`** synthesizes missing closes for any still-open frames (emitting one
  `P_PrematureEndOfInput` per frame), checks for leftover scope stack (`P_BuilderInvariant`
  with details), and produces the immutable Tree.

**What `TreeBuilder` does NOT do today:** sequence-level validation (i.e., whether
`open(varDecl)` is valid given the parent is `statement` and just saw `VarKeyword`).
That's parser work — coming in parent plan phase #7. The current builder trusts the caller
on shape validity.

---

## 7. Testing — STRICT ASSERTS REQUIRED

**This is the most important rule in this skill.** This project's tests catch regressions that
weaker assertions silently allow. **Every test you add must assert the strongest provable
property, not the weakest convenient one.**

### 7.1 Rules — non-negotiable

1. **Prefer `EXPECT_EQ` over `EXPECT_GE` / `EXPECT_TRUE` when the value is known exactly.**
   - WRONG: `EXPECT_GE(countCode(diags, P_PrematureEndOfInput), 1u)` when the setup makes the
     count exactly 3.
   - RIGHT: `EXPECT_EQ(countCode(diags, P_PrematureEndOfInput), 3u)`.
   - WRONG: `EXPECT_TRUE(diags.size() > 0)`.
   - RIGHT: `EXPECT_EQ(diags.size(), N)` where `N` is the expected count, OR a
     full-sequence comparison.

2. **Full-sequence equality on ordered output, not just size.**
   - WRONG: `EXPECT_EQ(visited.size(), 6u)`.
   - RIGHT: `EXPECT_EQ(visited, expectedVector)` — every NodeId checked in order.
   - Rationale: a regression that visits 6 wrong nodes silently passes the size-only check.

3. **Full pretty-printed string equality on happy paths.**
   - WRONG: substring `find` checks on a parse output you fully control.
   - RIGHT: `EXPECT_EQ(prettyPrint(t), expectedLiteral)` against an inline string literal.
   - Rationale: any structural drift (extra child, missing child, reordering) is caught by
     character-exact comparison. Use substring `find` ONLY when the output format is
     implementation-defined (e.g., error-leaf representation in broken-path tests).

4. **Hash-keyed comparison for unordered iteration.**
   - When iteration order is unspecified (e.g., `NodeAttribute<T>` sparse-mode iteration), collect
     into a `std::map<std::uint32_t, T>` keyed by `NodeId.v` and compare to an expected map.
   - Don't sort-and-compare vectors when a map captures the (id → value) relationship more
     precisely.

5. **`static_assert` for compile-time invariants.**
   - Type triviality: `static_assert(std::is_trivially_copyable_v<View>);`
   - Size budgets: `static_assert(sizeof(View) <= 2 * sizeof(void*));`
   - Const-overload return types: `static_assert(std::is_same_v<decltype(cref.get(id)), int const&>);`
   - Catches regressions at build time, not test time.

6. **Death tests with regex matching the actual fatal message.**
   - `EXPECT_DEATH({ … }, "invalid NodeId");` — the regex must match the substring the
     `*Fatal` helper actually emits. The fatal strings ARE part of the API contract because
     death tests depend on them.
   - Pair death tests with the message they assert against — when you change a fatal string,
     change the death test in the same commit.

7. **Verify recovery shape on broken-path tests, not just diagnostic codes.**
   - When testing recovery, assert ALL of:
     - the expected diagnostic codes are emitted (exact count where deterministic),
     - `hasError(t.flags(t.root()))` is `true`,
     - at least one descendant has `HasError` set (the actual Error leaf — not just the
       propagation up to root), and
     - the tree still walks (e.g., `prettyPrint` produces non-empty output containing the
       surrounding structural names).
   - Rationale: a regression that silently drops the bad token (no Error leaf, only the
     diagnostic record) would pass a weaker test.

8. **Pin documentation examples with CI tests.**
   - Every code example in `docs/` that claims to compile, load, or parse cleanly **must have
     a corresponding test** that exercises that exact snippet. See
     `GrammarSchema.DocsCookbookCalcExampleLoadsCleanly` as the canonical pattern.
   - Rationale: docs rot; tests don't.

9. **Test the "stays in mode" invariants, not just the "transitions to mode" ones.**
   - `NodeAttribute<T>` promotes sparse→dense at 50%/16. Tests must cover:
     - the exact-boundary cases (nc=16 promotes at size 8; nc=15 stays sparse at 100%),
     - the no-demotion-on-erase case (drop below 50% in dense; assert still dense),
     - the promotion-only-via-set case (hammer reads / erases; assert mode unchanged).

10. **Verify what isn't there, not just what is.**
    - Happy-path tests must assert `t.diagnostics().all().empty()` — catches spurious-warning
      regressions.
    - Move-construct / move-assign tests must assert the moved-from object is observably empty
      (`src.size() == 0`) — catches incomplete custom move ops.

### 7.2 Test infrastructure — known good patterns

| Helper | Where | When to use |
|---|---|---|
| `RawTreeBuilder` | `tests/core/raw_tree_builder.hpp` | Hand-fabricate trees with shapes `TreeBuilder` can't produce (pre-flagged Missing/Synthetic/EmptySpace internal wrappers). No schema attached — token-level views won't work. |
| `SchemaTreeBuilder` | `tests/core/test_tree_views.cpp` (inline) | Same shape as `RawTreeBuilder` but attaches a real `GrammarSchema` so token-level views resolve. Local rule interner — can intern arbitrary rule names (e.g., `"binaryExpr"`) not in the bound schema. |
| `ToyHarness` | `tests/core/toy_harness.hpp` | `make(sourceText, configText)` loads an inline JSON config + source buffer; `tok(text, kind)` synthesizes tokens by substring lookup. For loadShipped paths, prefer `E2EHarness`; reach for `ToyHarness` only when you need to hand-fabricate tokens against an inline schema. |
| `E2EHarness` | `tests/core/e2e_harness.hpp` | `tokenizeShipped(configName, text)` loads a shipped `.lang.json`, builds the SourceBuffer, runs the live `Tokenizer`, and returns the harness. The dtor asserts `lexerDiags.empty()` by default — call `h.dismissLexerDiags()` when the test deliberately trips a tokenizer diagnostic (`P_IllegalChar`, etc.). Load failure aborts via `std::abort` rather than returning a half-built harness. Drain whitespace between structural opens with `drainWhitespace(b, h.stream)`. |
| `prettyPrint` | `tests/core/test_tree_end_to_end.cpp` (inline) | Walks AST-mode and emits `rule:<name>` / `tok:"<text>"`. Doesn't surface flags — pair with a separate flag-walk for broken-path verification. |
| `countCode` | `tests/core/test_tree_end_to_end.cpp` (inline) | Counts diagnostics matching a `DiagnosticCode`. |
| `countErrorDescendants` | `tests/core/test_tree_end_to_end.cpp` (inline) | Walks a subtree (excluding start) counting nodes with `HasError`. Use to prove an Error leaf was actually inserted, not just that a diagnostic was logged. |
| Allocation counter | `tests/core/test_tree_visitor.cpp` (inline) | Global `operator new` override + atomic counter; snapshot delta around the code under test to assert zero allocations. |

### 7.3 Death tests on Windows / MinGW

The project's death tests use `EXPECT_DEATH` and work on Windows + MinGW. Several existing
tests (`test_tree.cpp`, `test_tree_builder.cpp`, `test_tree_cursor.cpp`, `test_tree_attrs.cpp`,
`test_tree_views.cpp`, `test_tree_end_to_end.cpp`) prove the gtest fork-emulation works
locally. Death tests should:

- Always match a regex against the fatal message string (the second argument).
- Never `EXPECT_DEATH({})` empty bodies.
- Live in a separate `*Death` test fixture when they dominate a test file's runtime
  (e.g., `NodeAttributeDeath` in `test_tree_attrs.cpp`).

### 7.4 Don't

- **No `<cassert>`.** Use the project's `*Fatal` pattern (see §9).
- **No `EXPECT_TRUE` on a count that's known.** Use `EXPECT_EQ`.
- **No `EXPECT_NE(find, npos)` when a full-string equality would work.**
- **No tests that pass when the implementation is silently broken.** If a test would still
  pass after `return std::nullopt` is replaced with `return Kind::Int`, it isn't strict enough.
- **No skipping the moved-from-state check** in move ctor/assign tests — see §7.1 rule 10.

---

## 8. The `.plans/` System

Internal design records live under `.plans/`. **They are NOT user docs** — the user-facing docs
are in `docs/`. The plans capture:

- The roadmap (`.plans/compiler-implementation-plan.md`).
- Sub-plans per major area (`.plans/tree-node-model-plan.md`, `.plans/schema-expressiveness-v2-plan.md`).
- Status snapshots at the top of each sub-plan (the §0 status tables).
- **Honest deviation lists** — the §0 deviations document every "the plan said X but we did Y"
  call with a reason. This is load-bearing for future contributors.

**Plans rot.** When status changes, update the plan in the same commit. Never let the plan
disagree with what's in `src/` — agents and contributors read it as canonical.

---

## 9. Coding Conventions — Mandatory

### 9.1 The fatal pattern (NOT `<cassert>`)

Every layer has a local `*Fatal` helper:

```cpp
[[noreturn]] inline void attrFatal(char const* what) {
    std::fputs("dss::NodeAttribute fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}
```

Precedents: `treeFatal` (in `tree.cpp`'s anonymous namespace), `cursorFatal` style
(inline `fputs+abort` in `tree_cursor.cpp`), `attrFatal` (`tree_attrs.hpp`), `viewFatal`
(`tree_views.hpp`). Always-on, release-mode aborts. No debug-only `assert` paths.

### 9.2 Strong-typed IDs everywhere

Bare `uint32_t` for tree indices is banned. Every ID is a `DSS_STRONG_ID` struct.

### 9.3 `DSS_EXPORT` discipline

- Apply to every class/struct with out-of-line members compiled into `libdss-code-prime.dll`
  AND consumed across translation units.
- DO NOT apply to: templates (each TU instantiates), header-only inline-only types (views,
  `well_known_names` constants), enums (header-only types).
- The grep `grep -n "^class\b\|^struct\b" src/core/types/*.hpp` is your sanity check.

### 9.4 `[[nodiscard]]` on accessors and consequential returns

All `Tree` accessors, view factory `from(...)`, cursor `current()` / `kind()` / `flags()`,
attribute `has` / `get` / `tryGet`, container `size` / `empty` / `isDense`, cursor movement
returns (`gotoFirstChild` etc.) — all `[[nodiscard]]`. Discarding them is almost always a bug.

### 9.5 Templates and headers

- Templates and inline helpers live in headers (header-only).
- `tree_visitor.hpp`, `tree_attrs.hpp`, `tree_views.hpp`, `well_known_names.hpp`,
  `raw_tree_builder.hpp` (test) are all header-only.
- Don't introduce a non-template `.cpp` boundary for accessor-heavy code — the project bets on
  inlining for the call-hot wrappers.

### 9.6 Comments — strict policy

From `CLAUDE.md` / repo convention: **default to no comments**. Only add one when the WHY is
non-obvious — a hidden constraint, a subtle invariant, a workaround, behavior that would
surprise a reader.

- DON'T explain WHAT well-named code does.
- DON'T reference the current task ("T7", "this fix", "this PR", "used by X") — those belong in
  PR descriptions.
- DON'T narrate test bodies.
- DO document non-obvious invariants (e.g., `Iterator_::value_type` being a by-value pair with
  a reference inside), API contracts that callers can't infer (e.g., the lifetime rule on
  `Tree const*`), and the rationale for surprising design choices (e.g., why `clear()` resets
  to sparse instead of staying dense).

### 9.7 Move semantics

When move semantics matter (e.g., `NodeAttribute<T>`'s `denseCount_`), **don't trust defaulted
move ops** — the `denseCount_` is a primitive that gets copied by `=default`, leaving the
moved-from object reporting `size() == 11` while iteration yields nothing. Write a custom
move ctor / move assign that resets the source to its empty state.

### 9.8 No abbreviations / no narration

- Don't use `e` / `i` / `n` as variable names — use `entry` / `index` / `count`.
- Don't write "Function that does X" comments on functions named `doX`.
- Section banners (`// ── Section ──`) belong in the file header preamble only, not as
  test/code separators.

---

## 10. Common Workflow Patterns

### 10.1 Adding a new public type / class

1. Header in `src/core/types/foo.hpp`.
2. If non-template + has out-of-line members → `.cpp` next to it, registered in
   `src/core/CMakeLists.txt`. If header-only → no `.cpp`, no CMake change.
3. `DSS_EXPORT` on the class (if non-template) or omit (if template/inline-only).
4. Test file `tests/core/test_foo.cpp` registered via `dss_add_test`.
5. Update `.plans/tree-node-model-plan.md`'s §0 status + §7 row table (if it's a sub-plan
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
new file in `src/source-config/languages/`, load via `GrammarSchema::loadShipped("yourname")`.

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
- **531 ctest cases across 26 suites, 100% pass.** (v1 T0–T12 baseline + v2 PR0–PR8 + SH1–SH4.)
- **Schema-expressiveness v2 (PR0–PR8): done.** Operator precedence + arity (`OperatorTable`),
  contextual keywords + `reservedWordPolicy`, `scopeRequire` (anyOf/forbid/topMustBe/outermost),
  `TreeBuilder::Checkpoint` + speculative-alt loader plumbing, `lexerModes` + `LexerModeStack` +
  `modeOp`, `stringStyle` descriptor with `EscapeKind` + dynamic tag patterns. Two real grammars
  ship: `toy.lang.json` and `tsql-subset.lang.json` (empirical stress proves v2 is sufficient
  for non-trivial languages). See `.plans/schema-expressiveness-v2-plan.md`.
- **Substrate hardening (SH1–SH4): done.** SH2 confirmed the multi-OS CI matrix (Linux/GCC,
  Linux/Clang+ASan, Windows/MSVC, macOS/AppleClang). SH3 closed the cross-tree `NodeId` caveat
  (`NodeId.treeTag` + tag validation in `NodeAttribute<T>` and `Tree::node_`). SH1 ships
  `tools/refresh_landing_log.py` for plan-doc hygiene; SH4a wires its `--check` into CI. SH4b
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

## 12. Where to Look for Canonical Examples

| Want to see | Look at |
|---|---|
| A clean test file that asserts STRICTLY | `tests/core/test_tree_visitor.cpp` (23 tests, full-sequence comparisons, allocation counter, static_asserts) |
| The strictest broken-path pattern | `tests/core/test_tree_end_to_end.cpp` (9 tests, full pretty-print equality, exact diagnostic counts, error-leaf walks) |
| A header-only template done right | `src/core/types/tree_attrs.hpp` (`NodeAttribute<T>`, custom move ops, dual storage) |
| A typed view | `src/core/types/tree_views.hpp` (all 7 views, ~250 lines) |
| The fatal helper pattern | `src/core/types/tree.cpp:20-25` (`treeFatal`) or `src/core/types/tree_attrs.hpp:35-40` (`attrFatal`) |
| Driving `TreeBuilder` from a test | `tests/core/test_tree_builder.cpp` (40+ tests covering every recovery flavor) |
| The shipped grammar config | `src/source-config/languages/toy.lang.json` |
| Onboarding docs writing style | `docs/tree-model.md` (the WhileStmtView cookbook is the template) |

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

## 14. Quick-Reference Header Map

| Need | Include |
|---|---|
| Read a tree | `core/types/tree.hpp` |
| Build a tree | `core/types/tree_builder.hpp` |
| Move through a tree | `core/types/tree_cursor.hpp` |
| Walk every node | `core/types/tree_visitor.hpp` |
| Attach data to nodes | `core/types/tree_attrs.hpp` |
| Typed views | `core/types/tree_views.hpp` |
| Standard rule / token names | `core/types/well_known_names.hpp` |
| Source location | `core/types/source_span.hpp`, `core/types/source_buffer.hpp` |
| Tokens (input to builder) | `core/types/token.hpp` |
| Strong IDs | `core/types/strong_ids.hpp` |
| Diagnostics | `core/types/parse_diagnostic.hpp`, `core/types/diagnostic_reporter.hpp` |
| Grammar schema | `core/types/grammar_schema.hpp` |
| Schema JSON loader | `core/types/grammar_schema_json.hpp` (PRIVATE — only `grammar_schema.cpp` should include) |
| Interner template | `core/types/interner.hpp` |
| Schema cursor | `core/types/schema_cursor.hpp` (internal — accessed via `GrammarSchema`) |
| Scope kinds | `core/types/scope_kind.hpp` |
| Rule IDs | `core/types/rule_id.hpp` |
| `DSS_EXPORT` macro | `core/export.hpp` |
