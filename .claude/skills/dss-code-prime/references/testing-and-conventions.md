# Testing discipline + coding conventions

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
