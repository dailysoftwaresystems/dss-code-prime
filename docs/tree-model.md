# DSS Code Prime — Tree / Node Model

> **For new contributors.** Read this if you want to walk a parsed tree, add a typed view for a new rule, or attach semantic data to nodes. About 30 minutes end-to-end.
>
> Internal design rationale (decision records, deviations, history) lives in [`.plans/01-tree-node-model-plan - ok.md`](../.plans/01-tree-node-model-plan - ok.md) — you don't need it to use the model.

---

## 1. Mental model

A parsed source file is **one immutable `Tree`** built once by a `TreeBuilder`. Every node (`Internal` or `Token`) lives in a flat arena indexed by a strongly-typed `NodeId`. The tree is the hub — every later pass reads from it; nothing mutates it after `finish()`.

```
                      ┌──────────────────┐
                      │  Tree (arena)    │  ← immutable, schema-bound, diagnostic-aware
                      │  - root: NodeId  │
                      │  - nodes[]       │
                      │  - childIndex[]  │
                      └──────┬───────────┘
        ┌────────────────────┼─────────────────────────────┐
        ▼                    ▼                             ▼
   TreeCursor          tree_visitor                  NodeAttribute<T>
   (walk it)           (visit every node)            (attach data without
                                                     touching the Tree)
        │                    │                             │
        └─────────┬──────────┘                             │
                  ▼                                        │
            Typed views                                    │
            (`VarDeclView::from`, …)  ←────────────────────┘
            ergonomic accessors over (Tree, NodeId)
```

**Three things to internalize:**

1. **Lossless first, semantic later.** Every byte of source — including whitespace and comments — has a `Token` node. Whitespace tokens carry `NodeFlags::EmptySpace`. The CST is the lossless full picture; the AST is *the same tree*, traversed in a mode that skips `EmptySpace` leaves.
2. **`NodeId` is just a `uint32_t` with a strong type.** Slot 0 is the `InvalidNode` sentinel; real nodes start at 1. Lifetime-wise, a `NodeId` is meaningful only against the `Tree` it was minted from — passing one tree's `NodeId` to another tree's accessor isn't detectable today (`NodeId` doesn't carry its source `TreeId`); the bounds check on `NodeAttribute` catches the obvious cases.
3. **Three lifetimes, one rule.** `TreeCursor`, `NodeAttribute<T>`, and every view class hold a raw `Tree const*`. **The bound `Tree` must outlive them.** Don't move-construct a `Tree` while a cursor/attribute/view points at the old one.

---

## 2. The Tree API at a glance

```cpp
#include "core/types/tree.hpp"
```

Universal per-node accessors — these work on any node, no discriminant check needed:

| Accessor | Returns | Notes |
|---|---|---|
| `t.kind(id)` | `NodeKind` | `Internal` or `Token` |
| `t.flags(id)` | `NodeFlags` | Bitwise OR of `EmptySpace`, `HasError`, `Missing`, `Synthetic`, … |
| `t.span(id)` | `SourceSpan` | Byte range in `t.source()` |
| `t.parent(id)` | `NodeId` | `InvalidNode` for the root |
| `t.children(id)` | `std::span<NodeId const>` | CST order — includes EmptySpace |
| `t.text(id)` | `std::string_view` | Slice of `t.source()` covering `t.span(id)` |

Discriminant-asserting (debug-asserts on wrong kind):

| Accessor | Requires | Returns |
|---|---|---|
| `t.rule(id)` | `kind == Internal` | `RuleId` — name via `t.rules().name(rid)` |
| `t.tokenKind(id)` | `kind == Token` | `SchemaTokenId` — name via `t.schema().schemaTokens().name(tid)` |
| `t.diagnostic(id)` | any | `optional<DiagnosticIndex>` |

Identity:

| `t.id()` → `TreeId` | `t.root()` → `NodeId` | `t.nodeCount()` → `std::size_t` |
| `t.source()` → `SourceBuffer const&` | `t.rules()` → `RuleInterner const&` | `t.schema()` / `t.diagnostics()` (abort if unattached; probe with `t.hasSchema()` / `t.hasDiagnostics()`) |

The tree exposes two cursor factories — they're how you actually move through it:

```cpp
TreeCursor cst = t.cursor();      // CST mode — visits every node
TreeCursor ast = t.astCursor();   // AST mode — skips NodeFlags::EmptySpace
```

---

## 3. Walking the tree

The cursor is movable but most code goes through the visitor helpers in `tree_visitor.hpp`.

```cpp
#include "core/types/tree_visitor.hpp"
#include "core/types/tree_cursor.hpp"

// (a) Whole tree, void visitor, CST mode (every node, including EmptySpace).
walkPreOrder(t, [&](TreeCursor const& c) {
    std::printf("%-12s depth=%d\n",
                t.kind(c.current()) == NodeKind::Internal
                    ? t.rules().name(t.rule(c.current())).data()
                    : "(token)",
                c.depth());
});

// (b) Whole tree, AST mode (skip whitespace), control flow via WalkAction.
walkPreOrder(t.astCursor(), [&](TreeCursor const& c) {
    if (t.kind(c.current()) != NodeKind::Internal) return WalkAction::Continue;
    if (t.rules().name(t.rule(c.current())) == "comment") {
        return WalkAction::SkipChildren;   // don't descend
    }
    if (someStopCondition) return WalkAction::Stop;
    return WalkAction::Continue;
});

// (c) Subtree-bounded walk — never ascends past `start`.
walkPreOrder(t, /*start=*/ someInternalId, [&](TreeCursor const&) { ... });

// Post-order is the same shape (`walkPostOrder`) — leaves visited before parents.
// SkipChildren is meaningless in post-order (children already visited); it's silently treated as Continue.
```

**What you can rely on:**

- Walks are **zero-allocation** on the hot path — state is the cursor plus one int. (Verified by `test_tree_visitor.cpp:TenThousandNodeWalkAllocatesNothing`.)
- Walks are **subtree-bounded** — supplying `start` confines the walk to that subtree.
- Visitor signature is auto-detected: returning `void` is treated as `WalkAction::Continue`; returning `WalkAction` gives you control.

---

## 4. Cookbook: add a typed view

A *view* is a tiny POD-like wrapper around `(Tree const*, NodeId)` that gives named accessors for a specific rule's role-positions. Adding one is mostly boilerplate.

Suppose your grammar has a `whileStmt: { sequence: ["WhileKeyword", "ParenOpen", "expression", "ParenClose", "block"] }` and you want a view that exposes `condition()` and `body()`.

**Step 1.** Pick a rule-name constant. If `"whileStmt"` is going to be used in more than one place, add it to `well_known_names.hpp`:

```cpp
// src/core/types/well_known_names.hpp
namespace dss::rules {
inline constexpr std::string_view kWhileStmt = "whileStmt";
}
```

**Step 2.** Drop this header in `src/core/types/while_stmt_view.hpp`:

```cpp
#pragma once

#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_views.hpp"      // for detail::views::nthVisibleChild + ruleIdFor
#include "core/types/well_known_names.hpp"

#include <optional>

namespace dss {

class WhileStmtView {
public:
    // Unchecked constructor — caller promises this node IS a whileStmt.
    WhileStmtView(Tree const& t, NodeId id) noexcept : tree_(&t), id_(id) {}

    // Safe factory — returns nullopt if `id` is the wrong shape.
    [[nodiscard]] static std::optional<WhileStmtView> from(Tree const& t, NodeId id) {
        if (!id.valid() || t.kind(id) != NodeKind::Internal) return std::nullopt;
        const auto want = detail::views::ruleIdFor(t, rules::kWhileStmt);
        if (!want.valid() || t.rule(id) != want) return std::nullopt;
        return WhileStmtView{t, id};
    }

    [[nodiscard]] NodeId      node()      const noexcept { return id_; }
    [[nodiscard]] SourceSpan  span()      const noexcept { return tree_->span(id_); }

    // Visible (AST-mode) children, indexed by structural position.
    // Layout: [WhileKeyword, ParenOpen, expression, ParenClose, block]
    //          0             1           2           3            4
    [[nodiscard]] NodeId condition() const noexcept {
        return detail::views::nthVisibleChild(*tree_, id_, /*index=*/ 2);
    }
    [[nodiscard]] NodeId body() const noexcept {
        return detail::views::nthVisibleChild(*tree_, id_, /*index=*/ 4);
    }

private:
    Tree const* tree_;
    NodeId      id_;
};

} // namespace dss
```

**Step 3.** Add a test in `tests/core/test_while_stmt_view.cpp` that builds a `whileStmt` with `RawTreeBuilder` and verifies `from(...)` + `condition()` + `body()`. Use `tests/core/test_tree_views.cpp` as a template; register the test with `dss_add_test` in `tests/core/CMakeLists.txt`.

**That's the whole add-a-view path.** ~30 lines of header + one test. No CMake changes for the header itself — `tree_views.hpp`'s pattern is header-only and the public include path picks the new file up automatically.

**When to use a view:**

- You're reading a specific rule and care about role-positions (`condition`, `body`).
- You want compile-time safety vs typos (`while_stmt.condition()` won't compile if you rename it).

**When NOT to use a view:**

- One-shot lookups. `t.children(id)` + a tiny inline filter is fine.
- Cross-rule traversals. Use `walkPreOrder` / `walkPostOrder`.

---

## 5. Attaching data: `NodeAttribute<T>`

The Tree is immutable, so semantic passes annotate via **side-tables** keyed by `NodeId`. One attribute per kind of data.

```cpp
#include "core/types/tree_attrs.hpp"

struct TypeInfo { /* ... */ };
NodeAttribute<TypeInfo> nodeTypes{t};   // binds to t — caller must keep t alive

// Per-node writes.
nodeTypes.set(someExprId, TypeInfo{...});

// Read.
if (auto const* ty = nodeTypes.tryGet(someExprId)) { useType(*ty); }
auto const& ty = nodeTypes.get(someExprId);   // aborts if absent
bool present = nodeTypes.has(someExprId);

// In-place mutation (both const and non-const overloads exist).
nodeTypes.get(someExprId).addConstraint(...);

// Bulk.
nodeTypes.erase(someExprId);
nodeTypes.clear();              // resets to sparse mode

// Iteration — yields std::pair<NodeId, T&>, order unspecified.
for (auto const& [id, ty] : nodeTypes) { /* ... */ }
std::size_t covered = nodeTypes.size();
```

**Storage promotes automatically.** Starts sparse (`unordered_map`); once coverage ≥ 50% **and** `tree.nodeCount() ≥ 16`, an internal `set()` promotes to dense (`vector<optional<T>>` indexed by `NodeId.v`). The public API is identical in either mode; `attr.isDense()` is available if you need to know.

**Bounds-check fatals.** Every API entry validates `id.valid() && id.v < tree.nodeCount()`; out-of-bounds aborts with a clear message (same pattern as `treeFatal`).

**Cross-tree guard.** Every `NodeId` minted by `TreeBuilder` (or by the test `RawTreeBuilder`) carries a `treeTag` — a snapshot of its source tree's id. Pass a `NodeId` from tree `B` to a `NodeAttribute` bound to tree `A` (or to any `Tree::children` / `Tree::kind` / etc. on tree `A`) and the access aborts with both ids in the message:

```
dss::NodeAttribute fatal: NodeAttribute bound to TreeId=A got NodeId from TreeId=B
dss::Tree fatal: Tree::node_: NodeId from TreeId=B used on TreeId=A
```

Hand-fabricated literal `NodeId{N}` constants (treeTag == 0) bypass the cross-tree check so existing tests that mix literal and tree-emitted ids continue to assert structurally; the bounds check is still the catch for genuinely-bad untagged ids. `NodeId` equality and `std::hash<NodeId>` compare `.v` only — the tag is provenance metadata, not identity.

---

## 6. Diagnostics — when something goes wrong

Every `Tree` built via `TreeBuilder` carries a `DiagnosticReporter`. After `finish()`:

```cpp
auto const& diags = t.diagnostics().all();   // std::span<ParseDiagnostic const>
bool didFail = t.diagnostics().hasErrors();
```

The codes you'll see during parsing (full list in `parse_diagnostic.hpp`):

| Code | When |
|---|---|
| `P_UnexpectedToken` | The parser explicitly rejected a token via `pushError`. |
| `P_UnknownToken` | The schema can't resolve a lexeme to any token kind. |
| `P_PrematureEndOfInput` | `finish()` ran with frames still open — one per open frame. |
| `P_BuilderInvariant` | Sequence/scope-stack invariant violated mid-build (e.g., `}` with no `{`). |
| `P_TooManyDiagnostics` | Reporter cap reached; later diagnostics dropped. |
| `P_AmbiguousToken` | Warning. Two equal-priority meanings survived the scope filter; first-declared won. Disambiguate the config or accept the determinism. |
| `P_ContextualKeywordResolution` | Info, v2. A soft keyword (`contextual: true` or under `reservedWordPolicy: "contextual"`) demoted to `Identifier` because the schema cursor's expected set didn't contain it. |
| `P_SchemaCursorDesync` | Info, v2, one-shot. The schema cursor went from valid to invalid for the first time this build. Contextual-keyword resolution stays strict for the remainder. |
| `P_MaxSpeculationDepth` | Error, v2, one-shot. `TreeBuilder::Checkpoint` stack hit `BuilderConfig::maxSpeculationDepth` (default 64). Subsequent `checkpoint()` calls return no-op guards. |
| `P_UncommittedCheckpoint` | Warning, v2. A `Checkpoint` guard was destroyed without `commit()` or `rollback()` — the dtor rolled it back. Forgotten-commit bug in the caller. |
| `P_BacktrackFailed` | Reserved, v2. For the future parser's "all speculative alternatives failed" emission. |

Recovery is sound: even a broken parse produces a walkable tree with the `HasError` flag propagated up to the root. `prettyPrint` (see `test_tree_end_to_end.cpp`) does not surface flags by default — broken-tree assertions in tests scan `t.flags(id)` for `hasError`.

---

## 7. Cheat-sheet: which header for which job

| Job | Include |
|---|---|
| Read a tree's structure | `core/types/tree.hpp` |
| Move through it | `core/types/tree_cursor.hpp` |
| Walk every node | `core/types/tree_visitor.hpp` |
| Attach data to nodes | `core/types/tree_attrs.hpp` |
| Use a typed view | `core/types/tree_views.hpp` |
| Define / look up a rule name | `core/types/well_known_names.hpp` |
| Construct a tree (you're writing a parser) | `core/types/tree_builder.hpp` |
| Inspect diagnostics from a parse | `core/types/parse_diagnostic.hpp`, `core/types/diagnostic_reporter.hpp` |
| Author or load a grammar | `core/types/grammar_schema.hpp` (see [language-config-spec.md](./language-config-spec.md)) |
