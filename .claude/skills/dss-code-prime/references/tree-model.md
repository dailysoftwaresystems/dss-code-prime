# The Tree / Node Model — core domain

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
- `NodeId` is a strong type wrapping `{uint32_t v; uint32_t arenaTag}` with `valid()` predicate
  (`v != 0` == invalid; `arenaTag == 0` == untagged literal). Equality and `std::hash` compare `.v` only.
  (`arenaTag` is the generalized arena-provenance field — see SP1's `DSS_ARENA_ID`; for `NodeId` it holds the source `TreeId`.)
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
- **Bounds-check + cross-tree-tag-check on every entry**. NodeIds carry an `arenaTag` (set by
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
