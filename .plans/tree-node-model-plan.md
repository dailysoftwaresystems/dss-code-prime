# DSS Code Prime — Tree / Node Model Implementation Plan

> Sub-plan of [`compiler-implementation-plan.md`](./compiler-implementation-plan.md).
> Defines the **core data structure** that every later module (tokenizer, parser, semantic analyzer, IR generator) reads from or writes to.

---

## 0. Current Status (snapshot)

Updated as work progresses. Detailed phase status lives in §7.

| Phase | Title | Status |
|---|---|---|
| **T0**  | nlohmann/json + GoogleTest + tests/ tree (CMake)            | ✅ done |
| **T1**  | source primitives + strong IDs + RuleInterner               | ✅ done |
| **T2**  | Tree arena + Node + NodeFlags (read-only API)               | ✅ done |
| **T3**  | ParseDiagnostic + DiagnosticReporter + DiagnosticPolicy     | ✅ done |
| **T4**  | GrammarSchema + SchemaCursor + ScopeKind + JSON loader + `toy.lang.json` | ✅ done |
| **T5**  | schema-aware `TreeBuilder` (RAII `OpenScope`, recovery)     | ✅ done (review-fix sweep applied) |
| **T6**  | `TreeCursor` (CST + AST modes)                              | ⏳ next |
| **T7**  | `tree_visitor.hpp` walk helpers                             | ⏳ pending |
| **T8**  | `NodeAttribute<T>` side-tables                              | ⏳ pending |
| **T9**  | initial typed views                                         | ⏳ pending |
| **T10** | toy-parser end-to-end (happy + broken)                      | ⏳ pending |
| **T11** | CMake wireup for any remaining core sources                 | (rolling — done per chunk) |
| **T12** | `docs/tree-model.md` + `docs/language-config-spec.md`       | ⏳ pending |

**Build state today**

| | |
|---|---|
| CMake floor | **4.0** (latest stable on inception; tested with 4.3.2) |
| C++ standard | **23** |
| Compilers verified | GCC 13.2 (MinGW-W64 ucrt) on Windows |
| Deps via FetchContent | **nlohmann/json 3.12.0**, **GoogleTest 1.17.0** |
| Test suite | **136 cases across 12 ctest suites — 100% pass** |

**Files now in `src/core/types/` (all on `core` static lib):**

```
strong_ids.hpp
source_span.hpp/.cpp
source_buffer.hpp/.cpp
token.hpp
interner.hpp                 ← header-only template with transparent heterogeneous lookup
rule_id.hpp                  ← using RuleInterner = Interner<RuleId>
schema_token_interner.hpp    ← using SchemaTokenInterner = Interner<SchemaTokenId>
scope_kind.hpp/.cpp
schema_cursor.hpp
parse_diagnostic.hpp/.cpp
diagnostic_reporter.hpp/.cpp ← FNV-1a64 dedup hash includes ruleContext
grammar_schema.hpp/.cpp
grammar_schema_json.hpp/.cpp ← only TU that includes <nlohmann/json.hpp>
tree_node.hpp                ← NodeKind, NodeFlags, detail::Node (40 bytes)
tree.hpp/.cpp                ← detail::TreeData, Tree (immutable, schema- & diagnostics-aware accessors)
tree_builder.hpp/.cpp        ← schema-aware assembler, RAII OpenScope, cascade-cookie tracking
```

**Shipped configs:**
- `src/source-config/languages/toy.lang.json` — minimal Toy language (multi-typed `+`/`<`, scope forbid, sequence/alt/repeat shapes). Loads via `GrammarSchema::loadShipped("toy")`.

**Deviations from spec, captured intentionally:**
1. `detail::Node` is **40 bytes** (not 32) — `DiagnosticIndex` added during the rigor-review pass pushed past the original cacheline-doubled budget. Still 1.6 nodes per 64 B line; performance impact negligible.
2. `RuleInterner` is now `using RuleInterner = Interner<RuleId>;` — header-only template `Interner<Id>` is the shared source. Uses C++20 transparent heterogeneous lookup (`std::hash<string_view>` + `equal_to<>`) so `find()`/`contains()`/`intern()` accept `string_view` without allocating temporaries. Identifier interner (plan §9 item 2) becomes a one-line addition.
3. `GrammarSchemaData` POD mirrors the Tree/TreeData split — instead of friending the JSON loader, the loader builds a movable POD that the schema's public ctor consumes.
4. `GrammarSchema::loadShipped` walks parent dirs to find `src/source-config/languages/<name>.lang.json`, so the lookup is independent of cwd (ctest, repo root, build dir all work).
5. CP1 declared but did not implement `Tree::cursor()` / `Tree::astCursor()` / `Tree::childrenOfRule()` — those declarations were removed and will return when T6 lands. `Tree::firstChildOfRule()` is documented for T5/T6 but the declaration also waits.
6. `T11` (CMake wireup) is folded into each checkpoint as files land — there's no separate "wire all files" phase at the end. The standalone T11 row in §7 is retained for the docs/discoverability angle.
7. **T5 scope limit:** the builder validates *within* an open frame (lexeme resolution, scope filter, priority tiebreak, scope-stack mutation, recovery, EOF synthesis, HasError propagation, internal invariants). It does **not** drive the schema's compiled shape graph for sequence/alt validation — that requires extending `GrammarSchema` with a navigable shape instruction stream. The parser (parent-plan phase #7) is the source of truth for "this `open(rule)` is valid here" until the shape walker lands.
8. **T5 reliability features added during review-fix sweep:**
   - `open() &` qualifier — disqualifies rvalue builders so `OpenScope` can't outlive a temporary.
   - `closedCookies_` set in builder — cascade-closed frames register their cookies; subsequent OpenScope `close()`s no-op cleanly instead of emitting spurious `P_BuilderInvariant`.
   - Release-mode `P_BuilderInvariant` guards on every public mutator (no debug-only `assert` corruption paths post-finish).
   - Leftover-scope detection at `finish()` emits a `P_BuilderInvariant` noting the imbalance.
   - `currentRule()` introspector for the parser layer.
   - Empty-tree `finish()` (no `open()` ever called) returns a well-formed Tree with `InvalidNode` root instead of dereferencing past the arena.

---

## 1. Goal

Design and implement a **single, language-agnostic tree data structure** that:

1. Is built once from a token stream + a language config file.
2. Faithfully represents *any* source language (C#, Dart, T-SQL, SQLite, future additions) without C++ changes.
3. Is navigable from root to leaves in both a *concrete* (lossless) and *abstract* (semantic) view.
4. Carries enough information to drive IR generation, which in turn produces a native binary for any supported OS / arch.

**Out of scope for this sub-plan:** the language config schema itself, the parser, IR, optimization passes, and target emitters. Those are tracked in the parent plan; this plan only defines the tree contract those modules will use.

---

## 2. Design Decisions (locked in)

| # | Decision | Rationale |
|---|---|---|
| D1 | **CST with an AST view** | Lossless parse tree preserves whitespace, comments, exact token positions. AST view is a *cursor mode* that hides trivia. Enables formatters, refactoring tools, error messages with original spacing, and incremental re-parsing later. |
| D2 | **Universal `Node` + typed views** | One `Node` struct stores every tree element. The "kind" of node is a `RuleId` — an interned grammar rule name from the language config (`"functionDecl"`, `"ifStmt"`, etc.). Hand-written *views* (`FunctionDeclView`, `BinaryExprView`) wrap `(Tree&, NodeId)` and provide named accessors with zero overhead. Adding a language = writing a config, not new C++ classes. |
| D3 | **Arena + `NodeId` indices** | All nodes live in a flat `std::vector<Node>` owned by `Tree`. Children are `uint32_t` indices. Cache-friendly, cheap to serialize, no per-node heap allocations, parent pointers free. Same approach as tree-sitter, Roslyn, rustc. |
| D4 | **Tree → IR → Target** | Tree describes *shape of source*; IR describes *shape of computation*. Optimizations live on IR (target-independent). Tree is immutable after build; IR is mutable. This keeps the parent plan's pipeline intact. |
| D5 | **Tree is immutable after build** | TreeBuilder mutates during parsing; once `finalize()` is called, only read APIs are exposed. Semantic info attaches via *attribute side-tables* keyed by `NodeId`, not by mutating nodes. Eliminates cursor-invalidation bugs. |
| D6 | **Attribute side-tables** | Type info, resolved symbols, evaluated constants, etc. live in typed maps (`NodeAttribute<TypeInfo>`, `NodeAttribute<SymbolId>`) on the side. Keeps `Node` small (~32 bytes), keeps semantic passes additive, makes the analysis phase trivially toggleable. |
| D7 | **Source text owned alongside the tree** | `Tree` holds the source buffer + a precomputed line-offset table. Nodes store byte spans (`{start, end}`); line/column derive lazily. Removes per-node duplication of `(file, line, col)`. |
| D8 | **Empty-space is a flag, not a node kind** | Whitespace, comments, and any other ignorable content carry a `NodeFlag::EmptySpace` bit on the node. A *single* bit-test (`flags & EmptySpace`) skips them — no string compares, no kind switches. The flag is distinct from `NodeKind`: an Internal grouping can also be flagged empty-space (e.g. a blank-line block) if a config models it that way. AST cursor mode skips by flag, not by kind. |
| D9 | **Schema-driven parsing prevents invalid syntax** | The language config compiles into a `GrammarSchema` — a tree of *expected* node shapes plus token definitions, scope rules, and validation predicates. The `TreeBuilder` walks this schema in lock-step with the source. Anything that doesn't match an allowed transition becomes a marked `Error` node carrying a structured `ParseDiagnostic` ("expected `;` or `,`, got `}`"). The tree can never contain silently-invalid syntax — every deviation is named and located. |
| D10 | **Language definitions are config files, not C++ classes** | The compiler **ships defaults** as JSON files under `src/source-config/languages/` (`csharp.lang.json`, `dart.lang.json`, `tsql.lang.json`, `sqlite.lang.json` — to be authored later). At runtime, callers either pick a shipped default by name (`--language csharp`) or supply a custom file (`--language-config ./my-dsl.lang.json`). Adding a language or tweaking an existing one **never** requires recompiling the engine. `GrammarSchema` only knows how to *load* a config; it does not embed any language's rules in code. |
| D11 | **C++23 + std-library-first, cross-platform from day one** | Engine targets **C++23** (MSVC 17.5+, GCC 13+, Clang 16+ — all current LTS-grade releases). Standard-library types are the default: `std::expected<T, E>` for fallible loaders, `std::format` for diagnostic rendering, `std::filesystem::path` for all paths, `std::span` for non-owning ranges, `std::optional` for nullable values, `std::ranges` for traversal. External dependencies are limited to **nlohmann/json** (config parsing) and **GoogleTest** (testing), both header-mostly, MIT/BSD-3, pulled via CMake `FetchContent` — no system packages required on any platform. Build system requires **CMake 4.0+** (current latest-stable on project inception; tested with 4.3.2). No POSIX-only APIs, no Win32 calls outside dedicated target backends. The minimum-version floor is updated to whatever the latest mainline GCC/Clang/MSVC support; we do not freeze to old compilers. |

---

## 3. Where this lives in the existing scaffold

The current repo already has placeholders for this work:

| Scaffold path | What's there today | What this plan adds |
|---|---|---|
| `src/core/types/.gitkeep` | Empty placeholder | All tree / node / span / token types |
| `src/core/compiler.hpp` | Empty `Compiler` class | Will own a `Tree` during compilation (later sub-plan) |
| `src/core/export.hpp` | `DSS_EXPORT` macro | Re-used on every public type |
| `src/core/CMakeLists.txt` | Builds `core` static lib | Add the new `.cpp` files here |
| `tests/core/.gitkeep` | Empty placeholder | All tree-related unit tests |

Conventions to follow (already established by the scaffold):
- `namespace dss`
- `DSS_EXPORT` on every public class
- One `CMakeLists.txt` per module; this plan only touches `src/core/CMakeLists.txt`
- Headers use `core/export.hpp` (project-relative include style)

This plan does **not** modify `program/`, `gen/`, or `tokenizer/` yet — those will consume the tree once it exists.

---

## 4. Module Layout

All new files land in `src/core/types/`:

```
src/core/
├── CMakeLists.txt                  # updated: add new .cpp files
├── compiler.hpp                    # unchanged (this plan)
├── compiler.cpp                    # unchanged (this plan)
├── export.hpp                      # unchanged
│
└── types/
    ├── source_buffer.hpp           # Source text + line-offset table
    ├── source_buffer.cpp
    ├── source_span.hpp             # Byte range {start, end}; line/col resolution
    ├── source_span.cpp
    │
    ├── token.hpp                   # Input to the builder: {kind, span}
    │
    ├── rule_id.hpp                 # RuleId (uint32_t) + RuleInterner
    ├── rule_id.cpp
    │
    ├── strong_ids.hpp              # NodeId, RuleId, SchemaTokenId, BufferId, TreeId, DiagnosticIndex
    ├── tree_node.hpp               # Node (Tree-private POD), NodeKind (Internal/Token/Error), NodeFlags
    │
    ├── scope_kind.hpp              # ScopeKind enum (Block, Paren, Generic, ...) used by builder + schema
    ├── schema_cursor.hpp           # SchemaCursor — position within a compiled GrammarSchema shape graph
    ├── grammar_schema.hpp          # Compiled language config: expected shapes + token defs + scope rules
    ├── grammar_schema.cpp          # Loader (JSON → schema), validator, scope-stack helpers
    ├── grammar_schema_json.hpp     # Internal: nlohmann/json-aware loader entry (NOT included by consumers)
    ├── grammar_schema_json.cpp
    │
    ├── parse_diagnostic.hpp        # Structured "expected X, got Y" diagnostic type
    ├── parse_diagnostic.cpp
    │
    ├── diagnostic_reporter.hpp     # Accumulates ParseDiagnostics; formats them with caret-pointed source snippets
    ├── diagnostic_reporter.cpp
    │
    ├── tree.hpp                    # Tree class — read-only API + arena storage + diagnostics handle
    ├── tree.cpp
    │
    ├── tree_builder.hpp            # Schema-aware stateful builder: validates as it builds, emits diagnostics
    ├── tree_builder.cpp
    │
    ├── tree_cursor.hpp             # Navigation cursor (CST mode + AST mode = skips NodeFlag::EmptySpace)
    ├── tree_cursor.cpp
    │
    ├── tree_visitor.hpp            # Visitor base + DFS helpers
    │
    ├── tree_attrs.hpp              # NodeAttribute<T> side-table template
    │
    └── tree_views.hpp              # Built-in typed views (Identifier, Literal, etc.)
```

The scaffold does **not** currently have a `tests/` sibling to `src/`. T11 (CMake wireup) creates it.

`tests/core/` mirrors this:

```
tests/core/
├── test_source_buffer.cpp
├── test_source_span.cpp
├── test_rule_id.cpp
├── test_grammar_schema.cpp         # loads a JSON config, asserts shape; rejects malformed config
├── test_parse_diagnostic.cpp
├── test_diagnostic_reporter.cpp    # formatted output (caret pointers, expected/actual)
├── test_tree_builder.cpp           # both happy path + invalid-syntax → Error nodes + diagnostics
├── test_tree_cursor.cpp            # AST mode skips by NodeFlag::EmptySpace
├── test_tree_attrs.cpp
├── test_tree_views.cpp
└── test_tree_end_to_end.cpp        # config-file → tokens → schema-validated tree → walk
```

And one canonical sample config used by the end-to-end test:

```
src/source-config/languages/
├── toy.lang.json                  # minimal expression-language config used by tests
├── csharp.lang.json                # shipped defaults (authored in a later plan)
├── dart.lang.json
├── tsql.lang.json
└── sqlite.lang.json
```

---

## 5. Type-by-type Specification

### 5.1 `SourceBuffer` — `source_buffer.hpp/.cpp`

Owns one source file's text. Computes a line-offset table once at construction.

```cpp
namespace dss {

// 1-based line, 1-based column. Free-standing so it can be reused without dragging SourceBuffer in.
struct DSS_EXPORT LineCol {
    uint32_t line;
    uint32_t column;
    constexpr bool operator==(const LineCol&) const = default;
};

class DSS_EXPORT SourceBuffer {
public:
    static std::shared_ptr<SourceBuffer> fromFile(const std::string& path);
    static std::shared_ptr<SourceBuffer> fromString(std::string text, std::string name = "<string>");

    BufferId          id()   const noexcept;        // unique per buffer; lets diagnostics span multiple files
    std::string_view  text() const noexcept;        // view, not std::string& — permits mmap-backed buffers later
    std::string_view  name() const noexcept;        // file path or synthetic name
    std::string_view  slice(uint32_t startByte, uint32_t endByte) const;
    LineCol           lineCol(uint32_t byteOffset) const;
    uint32_t          size() const noexcept;        // byte count

private:
    BufferId              id_;
    std::string           text_;                    // could become std::variant<string, mmap_view> later
    std::string           name_;
    std::vector<uint32_t> lineStarts_;              // byte offset of each line start; lineStarts_[0] == 0
};

} // namespace dss
```

**Why shared_ptr:** the same buffer is referenced by `Tree`, every `Token`, every diagnostic. Cheap to share, freed when the last holder is gone.

**BufferId** (defined in `strong_ids.hpp`) is a monotonic counter assigned at construction. When diagnostics later need to span multiple files (includes/imports), the `(BufferId, SourceSpan)` pair is unambiguous; a single `SourceBuffer&` parameter is not.

### 5.2 `SourceSpan` — `source_span.hpp/.cpp`

```cpp
// 4-GiB cap per buffer (uint32_t offsets); document this explicitly.
using ByteOffset = uint32_t;

class DSS_EXPORT SourceSpan {
public:
    // Factory enforces start <= end. Asserts in debug, clamps in release (end := max(start,end)).
    static SourceSpan of(ByteOffset start, ByteOffset end);
    static constexpr SourceSpan empty(ByteOffset at) noexcept { return SourceSpan{at, at}; }

    ByteOffset start()  const noexcept { return start_; }
    ByteOffset end()    const noexcept { return end_; }
    ByteOffset length() const noexcept { return end_ - start_; }
    bool       isEmpty() const noexcept { return end_ == start_; }

    bool contains(ByteOffset off)        const noexcept;
    bool containsSpan(SourceSpan other)  const noexcept;
    bool overlaps(SourceSpan other)      const noexcept;

    // smallest span covering both; empty operands are ignored
    // (join(empty(p), s) == s) — load-bearing for synthetic Missing-node parents.
    static SourceSpan join(SourceSpan a, SourceSpan b);

    // intersection; may be empty.
    static SourceSpan intersect(SourceSpan a, SourceSpan b);

    constexpr bool operator==(const SourceSpan&) const = default;
    // Ordering: by start, then end. Used to sort diagnostics in source order.
    constexpr auto operator<=>(const SourceSpan&) const = default;

private:
    constexpr SourceSpan(ByteOffset s, ByteOffset e) noexcept : start_(s), end_(e) {}
    ByteOffset start_;
    ByteOffset end_;
};
```

8 bytes, value type. Line/column are *not* stored — derived on demand via `SourceBuffer::lineCol()`. The factory-only construction makes "start ≤ end" a *type-level* invariant — no caller can fabricate an inverted span.

### 5.3 `Token`, `CoreTokenKind`, `SchemaTokenId` — `token.hpp`

Two distinct namespaces deliberately:

- **`CoreTokenKind`** — a small, fixed enum used by the *tokenizer* before any schema is consulted. Universal lexical categories only.
- **`SchemaTokenId`** — an interned id owned by the `GrammarSchema`, naming a schema-defined *meaning* (`"SumOperator"`, `"StringAppendOperator"`, `"GenericDefinitionOpener"`). The set is open-ended and language-specific.

This split prevents the silent confusion the reviewer flagged: a node's *schema meaning* cannot be stored in a `uint16_t` cast of `CoreTokenKind` because the two namespaces don't overlap.

```cpp
// Lexer-level. The tokenizer assigns this category before the schema sees the token.
enum class CoreTokenKind : uint16_t {
    Unknown = 0,
    Identifier,
    IntLiteral, FloatLiteral, StringLiteral, CharLiteral, BoolLiteral, NullLiteral,
    Punctuation,        // {} () [] , ;  — exact lexeme via span
    Operator,           // + - * / == != etc. — exact lexeme via span
    Word,               // alphanumeric run; keyword-vs-identifier resolved later via schema
    Whitespace,
    LineComment, BlockComment,
    Newline,
    Eof,
    Error,
};

// Schema-resolved meaning (interned by GrammarSchema). See §5.4 / §5.12.
// Bare struct here; full definition in strong_ids.hpp.
struct SchemaTokenId { uint32_t v; constexpr bool valid() const { return v != 0; } };

struct DSS_EXPORT Token {
    CoreTokenKind  coreKind;        // assigned by the tokenizer
    SchemaTokenId  schemaKind;      // assigned by the schema-aware resolver; invalid() before resolution
    SourceSpan     span;
};
```

The actual lexeme text is recovered via `buffer->slice(span.start(), span.end())`. Stored once, in the buffer.

**`isEmptySpace` lives on the schema, not on the token.** The language config decides what counts as ignorable; the tokenizer must not. The schema exposes `GrammarSchema::isEmptySpace(SchemaTokenId)` (see §5.12) — fast, table-driven.

A typical pipeline:
1. Tokenizer reads the buffer, emits tokens with `coreKind` set and `schemaKind = invalid`.
2. The schema-aware resolver (inside `TreeBuilder::pushToken`) consults `GrammarSchema::lookupLexeme` + scope stack + expected position, assigns `schemaKind`, and decides whether the token is `EmptySpace`.
3. The token becomes a leaf `Node` with `tokenKind` populated from `SchemaTokenId` (and `NodeFlags::EmptySpace` set if applicable).

### 5.4 Strong IDs + `RuleInterner` — `strong_ids.hpp`, `rule_id.cpp`

Every domain id is a *distinct* type. Bare `uint32_t` everywhere was flagged as the single biggest invariant gap: `tree.children(someRuleId)` compiled silently; attributes keyed by Tree-A's `NodeId` queried with Tree-B's value silently returned wrong data.

```cpp
// strong_ids.hpp

// Macro that mints a strong wrapper around uint32_t with explicit-only construction,
// equality/ordering, and a valid() predicate. Zero overhead.
#define DSS_STRONG_ID(Name)                                                        \
    struct Name {                                                                  \
        constexpr explicit Name(uint32_t v = 0) noexcept : v(v) {}                 \
        uint32_t v;                                                                \
        constexpr bool valid() const noexcept { return v != 0; }                   \
        constexpr bool operator==(const Name&) const = default;                    \
        constexpr auto operator<=>(const Name&) const = default;                   \
    };                                                                             \
    template <> struct std::hash<Name> {                                           \
        size_t operator()(Name id) const noexcept { return std::hash<uint32_t>{}(id.v); } \
    }

DSS_STRONG_ID(NodeId);            // node index within a Tree
DSS_STRONG_ID(RuleId);            // grammar rule (from GrammarSchema's interner)
DSS_STRONG_ID(SchemaTokenId);     // schema-resolved token meaning (interned)
DSS_STRONG_ID(BufferId);          // source buffer
DSS_STRONG_ID(TreeId);            // tree (for cross-tree NodeId guards in attributes)
DSS_STRONG_ID(DiagnosticIndex);   // index into a DiagnosticReporter

// Sentinels: any default-constructed strong id is the invalid value.
inline constexpr NodeId           InvalidNode{};
inline constexpr RuleId           InvalidRule{};
inline constexpr SchemaTokenId    InvalidSchemaToken{};
inline constexpr DiagnosticIndex  InvalidDiagnostic{};
```

`RuleInterner` (interns grammar rule names from the language config — `"functionDecl"`, `"ifStmt"`, …):

```cpp
class DSS_EXPORT RuleInterner {
public:
    RuleInterner();                                  // reserves slot 0 as InvalidRule
    RuleId            intern(std::string_view name); // returns existing id if seen; fails if frozen
    std::string_view  name(RuleId id) const;         // returns "" for InvalidRule
    bool              contains(std::string_view name) const;
    size_t            size() const noexcept;         // includes the reserved slot 0

    // Iteration support for tooling (e.g. dumping the grammar).
    using const_iterator = std::vector<std::string>::const_iterator;
    const_iterator begin() const noexcept;
    const_iterator end()   const noexcept;

    // After freeze(), intern() refuses new entries (returns InvalidRule + asserts in debug).
    // Called by GrammarSchema::loadFromFile / loadShipped once schema build is complete.
    void freeze() noexcept;
    bool isFrozen() const noexcept;

private:
    std::vector<std::string>                          names_;   // names_[0] == "" (InvalidRule sentinel)
    std::unordered_map<std::string, RuleId>           lookup_;
    bool                                              frozen_ = false;
};
```

**Why the freeze:** the plan's concurrency guarantee (§9.7 — concurrent `Tree`/`GrammarSchema` readers safe without locks) breaks if anyone calls `intern()` after schema load. Reserving slot 0 + an explicit `frozen_` flag makes that violation impossible to do silently.

A second interner (`SchemaTokenInterner`) follows the exact same shape and is used by `GrammarSchema` for `SchemaTokenId`.

### 5.5 `Node`, `NodeKind`, `NodeFlags` — `tree_node.hpp`

The `Node` POD is **private to `Tree`** — consumers never touch fields directly; they go through `Tree`'s discriminant-asserting accessors. This prevents the "tokenKind only valid when `kind == Token`" footgun.

```cpp
enum class NodeKind : uint8_t {
    Internal,   // grammar rule with children
    Token,      // leaf produced from a Token (resolved via SchemaTokenId)
    Error       // parser error-recovery node (carries a ParseDiagnostic)
};

// Bitmask. Multiple flags can apply to one node.
enum class NodeFlags : uint8_t {
    None       = 0,
    EmptySpace = 1u << 0,   // whitespace / comments / any ignorable run
    Missing    = 1u << 1,   // schema required this node but source didn't have it
                            //   (synthetic node, empty span at insertion point)
    HasError   = 1u << 2,   // this node or some descendant is/contains an Error
    Synthetic  = 1u << 3,   // inserted by builder, not derived from source tokens
    // bits 4-7 reserved
};

// Inline constexpr so they're header-only and zero-cost. Not DSS_EXPORT —
// MSVC dllimport on a free constexpr function produces warnings, and existing
// scaffold (target_base.hpp) only exports classes.
inline constexpr NodeFlags  operator|(NodeFlags a, NodeFlags b) noexcept;
inline constexpr NodeFlags  operator&(NodeFlags a, NodeFlags b) noexcept;
inline constexpr NodeFlags& operator|=(NodeFlags& a, NodeFlags b) noexcept;
inline constexpr NodeFlags  operator~(NodeFlags v) noexcept;
inline constexpr bool       any(NodeFlags v) noexcept;                  // v != None
inline constexpr bool       has(NodeFlags v, NodeFlags bit) noexcept;   // (v & bit) != None
inline constexpr bool       isEmptySpace(NodeFlags v) noexcept;         // has(v, EmptySpace)

namespace detail {  // implementation detail — accessed only via Tree
    struct Node {
        NodeKind         kind;
        NodeFlags        flags;
        uint16_t         _pad;
        SchemaTokenId    tokenKind;    // meaningful when kind == Token
        RuleId           rule;         // meaningful when kind == Internal
        SourceSpan       span;         // bytes covered (possibly empty for Missing)
        NodeId           parent;       // InvalidNode for root
        uint32_t         firstChild;   // offset into Tree::childIndex_
        uint32_t         childCount;   // consecutive children (0 for leaves)
        DiagnosticIndex  diagnostic;   // valid() iff Error or HasError set
    };
    static_assert(sizeof(Node) <= 40, "Node grew unexpectedly — review layout");
    // Plan originally targeted 32 bytes, but the DiagnosticIndex added during
    // the rigor-review pass pushed the layout to 40 bytes. Still 1.6 nodes per
    // 64 B cacheline; shrinking further would force packing firstChild/
    // childCount into 16 bits each (capping children/node at 65 535).
}
```

**Discriminant-safe access via `Tree`.** Consumers don't see `detail::Node`; they call:
- `tree.kind(id) / tree.flags(id) / tree.span(id) / tree.parent(id) / tree.children(id)` — always safe.
- `tree.tokenKind(id)` — debug-asserts `kind == Token`.
- `tree.rule(id)` — debug-asserts `kind == Internal`.
- `tree.diagnostic(id)` — returns `std::optional<DiagnosticIndex>`; `nullopt` if none.

**Why `EmptySpace` is a flag, not a kind:**
- One bit-test (`isEmptySpace(tree.flags(id))`) suffices in every consumer — cursor, visitor, formatter, IR generator.
- An *Internal* grouping can also be flagged empty-space (rare, but useful for things like a "blank-line group" that some configs model).
- `NodeKind` stays at three essential structural states; `NodeFlags` carries orthogonal markers.

**Children layout:** all children of node N occupy `childCount` consecutive slots in a single `std::vector<NodeId> childIndex_` starting at `firstChild`. `Tree::children()` bounds-checks `firstChild + childCount` against the vector size in *release* mode (the cost is one branch, the invariant is critical).

### 5.6 `Tree` — `tree.hpp/.cpp`

The read-only public API after the parser hands ownership over. No `friend class TreeBuilder` — the builder constructs `Tree` from a movable POD, which makes T2 (fabricate-tree-by-hand test) clean and avoids leaking builder internals.

```cpp
// Movable POD shipped from TreeBuilder::finish() to Tree's constructor.
// Defined inside detail/, exposed only to TreeBuilder + Tree.
namespace detail {
    struct TreeData {
        std::shared_ptr<SourceBuffer>          source;
        std::shared_ptr<const GrammarSchema>   schema;        // co-owned; tree may outlive builder
        std::vector<detail::Node>              nodes;
        std::vector<NodeId>                    childIndex;
        std::unique_ptr<DiagnosticReporter>    diagnostics;
        NodeId                                 root;
    };
}

class DSS_EXPORT Tree {
public:
    // Constructor — only TreeBuilder::finish() calls this (with a move).
    explicit Tree(detail::TreeData&& data);

    // Tree is immutable: non-copyable, movable.
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    Tree(Tree&&) noexcept = default;
    Tree& operator=(Tree&&) noexcept = default;

    // ---- identity ----
    TreeId                       id()     const noexcept;
    const SourceBuffer&          source() const noexcept;
    const GrammarSchema&         schema() const noexcept;
    const RuleInterner&          rules()  const noexcept;
    const DiagnosticReporter&    diagnostics() const noexcept;
    NodeId                       root()      const noexcept;
    size_t                       nodeCount() const noexcept;

    // ---- per-node accessors (discriminant-safe) ----
    NodeKind                kind(NodeId id)    const;
    NodeFlags               flags(NodeId id)   const;
    SourceSpan              span(NodeId id)    const;
    NodeId                  parent(NodeId id)  const;
    std::span<const NodeId> children(NodeId id) const;     // release-mode bounds-checked
    std::string_view        text(NodeId id)    const;      // source slice

    // Discriminant-asserting accessors (debug-asserts on wrong kind):
    RuleId                       rule(NodeId id)      const;   // requires Internal
    SchemaTokenId                tokenKind(NodeId id) const;   // requires Token
    std::optional<DiagnosticIndex> diagnostic(NodeId id) const; // nullopt if absent

    // ---- queries ----
    NodeId firstChildOfRule(NodeId parent, RuleId rule) const;

    // Zero-allocation range; callers iterate. Replaces the previous vector-returning version.
    auto childrenOfRule(NodeId parent, RuleId rule) const
        -> std::ranges::filter_view<std::span<const NodeId>, /*pred*/ /*…*/>;

    // ---- cursors (see 5.8) ----
    TreeCursor cursor() const;          // CST cursor
    TreeCursor astCursor() const;       // AST cursor (skips NodeFlags::EmptySpace only — NOT Missing/Synthetic)

private:
    // Private storage; constructor-only access.
    TreeId                                 id_;
    std::shared_ptr<SourceBuffer>          source_;
    std::shared_ptr<const GrammarSchema>   schema_;
    std::vector<detail::Node>              nodes_;
    std::vector<NodeId>                    childIndex_;
    std::unique_ptr<DiagnosticReporter>    diagnostics_;
    NodeId                                 root_;
};
```

**Notes on the cursor's AST mode** (clarifying §5.8): it skips *only* `NodeFlags::EmptySpace`. `Missing` and `Synthetic` nodes are visible in AST mode — IR generation and semantic passes specifically want to see them so they can report "you didn't write the `)`".

**`childrenOfRule` returns a range view**, not `std::vector<NodeId>`, to avoid a heap allocation on every call. Semantic passes that query "all `param` children of a `paramList`" run this in tight loops.

### 5.7 `TreeBuilder` — `tree_builder.hpp/.cpp`

The builder is **schema-aware**. Constructed with a `GrammarSchema` (see §5.12), it walks the schema in lock-step with the source. Every `pushToken` is checked against what the schema permits at the current position; any mismatch is recorded as a structured `ParseDiagnostic` (see §5.13) and an `Error` node is inserted. The builder is *single-use* — `finish()` consumes it.

```cpp
class DSS_EXPORT TreeBuilder {
public:
    TreeBuilder(std::shared_ptr<SourceBuffer> src,
                std::shared_ptr<const GrammarSchema> schema,
                DiagnosticReporter::Config diagConfig = {});

    // ---- shape construction (RAII — open returns a scope guard) ----

    // Opaque scope guard. Destructor closes the node automatically.
    // Move-only — cannot be copied or fabricated by callers (private body).
    class OpenScope;

    [[nodiscard]] OpenScope open(RuleId rule);

    // Add a token. The builder resolves SchemaTokenId via schema lookup +
    // scope stack + expected position. EmptySpace tokens (per schema) are
    // attached as leaves with NodeFlags::EmptySpace set; they DO NOT
    // advance the schema cursor.
    void pushToken(const Token& tok);

    // Explicit error production (when the parser already knows it's wrong).
    // Both "expected" arguments are optional; either, both, or neither may be set.
    void pushError(SourceSpan span,
                   std::optional<RuleId>        expectedRule  = std::nullopt,
                   std::optional<SchemaTokenId> expectedToken = std::nullopt,
                   std::string_view             note          = {});

    // ---- scope stack (defined in scope_kind.hpp; see §5.12) ----
    void               pushScope(ScopeKind kind);
    void               popScope();              // P9000 diagnostic on underflow (never silent)
    ScopeKind          currentScope() const;
    std::span<const ScopeKind> scopeStack() const;

    // ---- finalize ----
    //
    // Consumes the builder, producing the final Tree. No NodeId argument —
    // the builder tracks the root automatically (asserts exactly one closed
    // top-level shape). After finish(), the builder is in a moved-from state.
    Tree finish() &&;

private:
    // OpenScope captures the builder's state needed to close cleanly.
    // Body is opaque — callers can move it but cannot read its fields.
    class OpenScope {
    public:
        OpenScope(OpenScope&&) noexcept;
        OpenScope& operator=(OpenScope&&) noexcept;
        OpenScope(const OpenScope&) = delete;
        ~OpenScope();                            // auto-closes if not already closed
        void close();                            // explicit early close (idempotent)
    private:
        friend class TreeBuilder;
        OpenScope(TreeBuilder& b, uint32_t cookie);
        TreeBuilder* builder_;                   // null after close / move-from
        uint32_t     cookie_;                    // private handle into builder state
    };
};
```

**How validation works (per `pushToken`):**

1. **Multi-typed resolution.** The schema lists every possible meaning of the raw lexeme (e.g. `+` may be `SumOperator | StringAppendOperator | ArrayAppendOperator`). The builder asks `schema.lookupLexeme(lexeme)` and filters by scope-stack validity + current expected position.
2. **Tiebreak on multiple matches.** Configs may attach a `"priority"` integer per token meaning; lowest priority wins. If two configs tie, a `P0008_AmbiguousToken` diagnostic is emitted and the first declared meaning wins (deterministic). See §5.12 for the config field.
3. **No match.** Emit a `ParseDiagnostic` ("expected `;` or expression continuation, got `+`"), insert an `Error` leaf carrying that diagnostic, then **always make forward progress** — either consume the token (mark as best-fit Error leaf, advance source position) or skip to the nearest sync token (`;`, `}`, `)` per schema); never *both* "don't consume and don't advance schema cursor" (which would loop forever).
4. **EmptySpace passthrough.** Tokens whose schema-resolved kind is *EmptySpace* are attached with `NodeFlags::EmptySpace` set but **do not advance the schema cursor**. Significant for round-tripping in formatters/IDE tooling.

**Internal invariant violations** — never silent:

| Violation | Debug | Release |
|---|---|---|
| `pushToken` with no open node | `DSS_ASSERT(false)` | Emit `P9000_BuilderInvariant` against the token's span; ignore the token |
| `popScope` underflow | `DSS_ASSERT(false)` | Emit `P9000` against the current cursor position; ignore |
| `OpenScope::close()` out-of-order (LIFO violation) | `DSS_ASSERT(false)` | Emit `P9000`; close-down to the offending scope |
| `pushToken` with unknown `CoreTokenKind` | `DSS_ASSERT(false)` | Emit `P0003_UnknownToken`; insert Error leaf; advance one byte |
| `finish()` called with >1 unclosed top-level shape | `DSS_ASSERT(false)` | Emit `P9002_UnfinishedTree`; close everything synthetically |

**`HasError` propagation timing** — `HasError` is set on every ancestor *the moment a diagnostic is attached* (immediate parent-chain walk inside `pushError` / Error-producing `pushToken` / synthetic `Missing` insertion / EOF cleanup). It is **not** deferred to `close()`. This guarantees that errors inserted during recovery — when a parent may already be auto-closed — still mark every still-open ancestor, and the closed-ancestor case is handled by walking via stored `parent` links rather than the open-scope stack.

**Scope-stack snapshot on diagnostic.** When a `ParseDiagnostic` is constructed, the builder snapshots `scopeStack()` into the diagnostic so the formatter can render "got `>` while inside `Generic` scope" without needing to reconstruct state.

**Invariants enforced (summary):**
- Every `open()` produces an `OpenScope`; the destructor closes it. Forgetting to close becomes impossible (compile-time enforcement via RAII).
- Recovery branches **must** consume ≥1 source token OR advance the schema cursor on each iteration; a per-builder watchdog tracks `(sourceIndex, schemaCursor)` and emits `P9003_RecoveryStalled` + force-advances if the pair repeats N times (N=3 by default).
- Spans roll up: a parent's span is the union of its children's spans. `SourceSpan::join` ignores empty operands, so synthetic Missing children don't collapse the parent span.
- The schema's `canEndSource` predicate must be true at `finish()`; otherwise one `P0004_PrematureEnd` per unclosed open shape, chained via the diagnostic's `related` vector (deepest first).
- `Error` and `Missing` nodes set `HasError` on every ancestor up to the root, so a single `has(tree.flags(tree.root()), NodeFlags::HasError)` answers "did this parse cleanly?".

### 5.8 `TreeCursor` — `tree_cursor.hpp/.cpp`

Stateful walker — the primary navigation API. Two modes; AST mode skips **only** `NodeFlags::EmptySpace` (not `Missing` or `Synthetic` — IR generation needs to see those).

```cpp
enum class CursorMode { Cst, Ast };

class DSS_EXPORT TreeCursor {
public:
    TreeCursor(const Tree& tree, NodeId start, CursorMode mode);

    // ---- position ----
    NodeId    current() const noexcept;
    NodeKind  kind()    const noexcept;
    NodeFlags flags()   const noexcept;

    // ---- movement (return false if no such node exists) ----
    bool gotoFirstChild();
    bool gotoLastChild();
    bool gotoNextSibling();
    bool gotoPrevSibling();
    bool gotoParent();

    // ---- bookmark / restore ----
    // Opaque snapshot — captures the tree pointer too, so cross-tree restoration
    // is a compile-time error and cross-cursor restoration is debug-asserted.
    struct Bookmark {
        const Tree* tree;
        NodeId      id;
        // (intentionally trivially copyable; non-aggregate to discourage hand-construction)
    };
    [[nodiscard]] Bookmark mark() const noexcept;
    void                   restore(Bookmark saved);   // asserts saved.tree == &tree_

    // ---- convenience ----
    bool isAtLeaf() const noexcept;
    int  depth()    const noexcept;

private:
    const Tree& tree_;
    NodeId      current_;
    CursorMode  mode_;
};
```

AST mode predicate (in `tree_cursor.cpp`): a single bit test, `isEmptySpace(tree.flags(id))`. No kind switches, no string compares.

**Thread safety.** Cursor state is purely local — multiple cursors over the same `Tree` from multiple threads are safe with no synchronization (the tree is immutable after `finish()`). The class is `thread_compatible` per the [Google style](https://google.github.io/styleguide/cppguide.html#Thread_Safety_Annotations) — concurrent calls on the *same* cursor instance need external synchronization.

**Possible future optimization (not in scope here):** template `TreeCursor<CursorMode::Ast>` vs `TreeCursor<CursorMode::Cst>` to eliminate the per-move branch on the mode field. Worth doing only if profiling shows it matters — visitor walks dominate cost anyway.

### 5.9 `tree_visitor.hpp` — Recursive walks

```cpp
template <typename F>
void walkPreOrder(const Tree& t, NodeId start, F&& visit);   // visit before children

template <typename F>
void walkPostOrder(const Tree& t, NodeId start, F&& visit);  // visit after children

// Visitor with skip control:
//   return WalkAction::Continue / SkipChildren / Stop
enum class WalkAction { Continue, SkipChildren, Stop };
template <typename F> void walk(const Tree& t, NodeId start, F&& visit);
```

All implemented over `TreeCursor`. No dynamic dispatch.

### 5.10 `tree_attrs.hpp` — Attribute side-tables

Bound to a specific `Tree` via `TreeId` at construction; cross-tree lookups debug-assert. Two storage strategies pick themselves at runtime (sparse vs dense) based on actual coverage.

```cpp
template <typename T>
class NodeAttribute {
public:
    // Required: the tree this attribute table belongs to.
    // Cross-tree lookups (set/get with a NodeId from a different tree) DSS_ASSERT.
    explicit NodeAttribute(TreeId tree) noexcept;

    void     set(NodeId id, T value);
    bool     has(NodeId id) const;
    const T& get(NodeId id) const;            // asserts has(id)
    const T* tryGet(NodeId id) const;         // nullptr if absent

    // Bulk operations / introspection
    void     clear() noexcept;
    size_t   size() const noexcept;           // count of attached values
    void     reserve(size_t n);

    // Iteration over (NodeId, T&) pairs. Useful for semantic passes that
    // want "every node with a resolved type". Order is unspecified.
    auto begin() const;
    auto end() const;

    TreeId tree() const noexcept;

private:
    // Implementation: flat_hash_map<NodeId, T> for sparse coverage,
    // or dense vector<optional<T>> indexed by NodeId for full coverage.
    // The choice is internal; the public API is the same.
    TreeId tree_;
    /* storage_ */
};
```

Semantic analysis populates these:

```cpp
NodeAttribute<TypeInfo>   nodeTypes(tree.id());        // type of an expression
NodeAttribute<SymbolId>   resolvedSymbols(tree.id());  // identifier → declaration
NodeAttribute<ConstValue> evaluatedConsts(tree.id());  // compile-time constant value
```

**Thread safety.** `thread_compatible`: concurrent reads on the *same* `NodeAttribute<T>` are safe; concurrent writes require external synchronization. Concurrent writes to *different* attribute tables on the same `Tree` are safe with no synchronization. This is the *only* way later passes annotate the tree — the `Tree` itself stays immutable.

### 5.11 `tree_views.hpp` — Typed views

Hand-written ergonomic accessors over a `(Tree, NodeId)` pair. Zero overhead — just `inline` member functions.

```cpp
class IdentifierView {
public:
    IdentifierView(const Tree& t, NodeId id) : tree_(t), id_(id) {}
    std::string_view name() const { return tree_.text(id_); }
    SourceSpan       span() const { return tree_.span(id_); }
private:
    const Tree& tree_;
    NodeId      id_;
};

class BinaryExprView {
public:
    BinaryExprView(const Tree& t, NodeId id);
    NodeId lhs() const;
    NodeId rhs() const;
    std::string_view op() const;     // operator lexeme
};

class FunctionDeclView {
public:
    FunctionDeclView(const Tree& t, NodeId id);
    IdentifierView name() const;
    NodeId         paramList() const;
    NodeId         body() const;
};
```

**Discovery model:** `views` are *optional*. A consumer who knows the rule name can use the typed view; everyone else uses the generic `Tree::children()` API. The set of views grows organically as we implement IR generation and semantic passes — there is no obligation to write a view for every possible rule.

**Future enhancement (not in this sub-plan):** a small code generator that emits view classes from a language config. Punted because the hand-written set will be small (likely <30 shapes) and shared across languages.

---

### 5.12 `GrammarSchema`, `SchemaCursor`, `ScopeKind` — `grammar_schema.hpp/.cpp`

> **Supersedes parent plan §4.4 (`source-factory/`).** `GrammarSchema` is the *replacement* for the parent plan's `LanguageConfig` + model classes + `ConfigValidator`. `src/source-factory/models/` is dropped; the `source-factory` module's remaining responsibility is path resolution + invoking `GrammarSchema::loadFromFile`.

The compiled, in-memory form of a language config file. **Always loaded from a config file** — the engine ships no language baked in.

#### `ScopeKind` — `scope_kind.hpp`

```cpp
// Stable, language-agnostic scope categories. Multiple may be active simultaneously.
// The schema decides which tokens open/close which scopes.
enum class ScopeKind : uint16_t {
    None = 0,
    Root,            // top-level file scope
    Block,           // { ... }
    Paren,           // ( ... )
    Bracket,         // [ ... ]
    Generic,         // < ... > in a type-parameter context
    String,          // " ... " (relevant for interpolation)
    Comment,         // /* ... */ — affects tokenization
    // Reserved range for future shared scopes (256..511)
    // Language-specific scopes use ids >= 1024; schema names them.
};
```

#### `SchemaCursor` — `schema_cursor.hpp`

Tiny value type that knows "I am inside shape X, at position N within its sequence (so the next thing expected is Y)". Trivially copyable.

```cpp
class DSS_EXPORT SchemaCursor {
public:
    // Construct only via GrammarSchema::rootCursor() and movement methods.
    // No public constructors; opaque cookie.
    constexpr SchemaCursor() noexcept = default;

    constexpr bool valid() const noexcept { return shapeId_ != 0; }
    constexpr bool operator==(const SchemaCursor&) const = default;

private:
    friend class GrammarSchema;
    uint32_t shapeId_   = 0;   // index into schema's compiled shape table
    uint32_t position_  = 0;   // step within the shape's production
    uint32_t parentRet_ = 0;   // saved return-position for nested rule descent (call stack as integer)
    uint16_t altIndex_  = 0;   // which alternative was taken (for diagnostics)
    uint16_t _pad       = 0;
};
// 16 bytes; passed by value through the builder.
```

#### `GrammarSchema`

```cpp
// A loader error is a structured diagnostic, not an exception.
struct DSS_EXPORT ConfigDiagnostic {
    DiagnosticSeverity severity;        // typically Error
    std::string        path;            // JSON pointer or file path
    std::string        message;
    std::string        code;            // "C0001_MissingField", "C0007_CircularShape", ...
};

// Standard C++23 fallible-result type. Error channel carries every
// diagnostic the loader collected before bailing — not just the first.
template <typename T>
using LoadResult = std::expected<T, std::vector<ConfigDiagnostic>>;

class DSS_EXPORT GrammarSchema {
public:
    // ---- loading ----
    // All loaders return a Result so loader failures are *data*, not exceptions.
    // Multiple diagnostics may be collected before bailing (missing field + bad ref + …).
    static Result<std::shared_ptr<GrammarSchema>> loadFromFile(const std::string& path);
    static Result<std::shared_ptr<GrammarSchema>> loadShipped(const std::string& name);
    static Result<std::shared_ptr<GrammarSchema>> loadFromText(std::string_view jsonText,
                                                                std::string_view sourceLabel = "<inline>");
    // (`loadFromJson(nlohmann::json)` is in `grammar_schema_json.hpp` — internal only,
    //  not pulled in by consumers of `grammar_schema.hpp`. This keeps the JSON library
    //  out of the public include surface.)

    // ---- introspection ----
    std::string_view             name() const;
    std::string_view             version() const;
    uint32_t                     schemaVersion() const;   // for dssSchemaVersion compat checks
    const RuleInterner&          rules()           const; // frozen after load
    const SchemaTokenInterner&   schemaTokens()    const; // frozen after load
    std::span<const std::string> fileExtensions()  const;

    // ---- token recognition ----
    // Given a raw lexeme, return all possible schema-resolved meanings.
    // Multi-typed: '+' might be SumOperator | StringAppendOperator | ...
    // The result is in *declaration order* and includes per-meaning priority for tiebreak.
    struct LexemeMeaning {
        SchemaTokenId    id;
        int32_t          priority;          // lower wins; default 0
        NodeFlags        flagsApplied;      // includes EmptySpace if applicable
        ScopeKind        opensScope;        // None if not a scope opener
        bool             closesScope;       // closes whichever scope is currently on top
        std::span<const ScopeKind> validScopes;  // empty = valid everywhere
    };
    std::span<const LexemeMeaning> lookupLexeme(std::string_view lexeme) const;

    // Convenience: does any meaning of this id carry the EmptySpace flag?
    bool isEmptySpace(SchemaTokenId id) const;

    // ---- shape navigation (used by TreeBuilder) ----
    SchemaCursor rootCursor() const;

    // Advance the cursor by consuming a schema token meaning. Returns the
    // updated cursor + whether the advance was a successful match. If the
    // cursor was already at end-of-shape and no match is possible, returns
    // an invalid cursor; the builder then triggers recovery.
    struct AdvanceResult { SchemaCursor next; bool matched; };
    AdvanceResult advance(SchemaCursor cur, SchemaTokenId tok) const;
    AdvanceResult enterRule(SchemaCursor cur, RuleId rule) const;
    AdvanceResult leaveRule(SchemaCursor cur) const;

    // ---- scope rules ----
    bool isTokenValidInScope(SchemaTokenId tok, std::span<const ScopeKind> stack) const;

    // ---- termination ----
    bool canEndSource(SchemaCursor cur) const;

    // ---- diagnostics support ----
    // Returns a stable, schema-owned view — no allocation per call.
    // Names are pre-formatted by the loader.
    std::span<const std::string_view> expectedAt(SchemaCursor cur) const;

private:
    GrammarSchema() = default;
    // ... internal compiled tables, all frozen post-load
};
```

#### Behavior contracts

- **No backtracking on `alt`.** When the schema offers `{"alt": [A, B, C]}`, the builder takes the *first* alternative whose FIRST set matches the incoming token. If none matches, `P0009_NoAlternativeMatched` is emitted. This makes parse complexity linear and recovery predictable. PEG-style speculative lookahead is deferred to a later plan (see §9).
- **`alt` ambiguity at config-load time.** The loader checks FIRST-set overlap across alternatives within a shape. Overlapping FIRST sets produce `C0010_AmbiguousAlternatives` at load time — pushed to the user fix early, not silently first-match-wins.
- **Multi-typed lexeme tiebreak.** When `lookupLexeme` returns >1 valid meanings (post scope/position filtering), `LexemeMeaning::priority` resolves. Equal priorities → first-declared wins + `P0008_AmbiguousToken` diagnostic.
- **`canEndSource` precision.** Defined per-shape in the config (`"canEndSource": true|false` — defaults to "yes if the cursor is at a complete production"). The loader infers a default if unspecified.
- **Loader diagnostic codes** use the `C####_*` namespace (config) distinct from parse-time `P####`. Examples: `C0001_MissingField`, `C0002_UnknownShape`, `C0003_UnknownToken`, `C0005_VersionMismatch`, `C0007_CircularShape`, `C0010_AmbiguousAlternatives`, `C0011_UnclosableScope`.

#### Config file shape (sketch — full spec authored in T12's `docs/language-config-spec.md`)

```jsonc
{
  "dssSchemaVersion": 1,    // required; loader emits C0005 on mismatch

  "language":  { "name": "ExampleLang", "version": "1.0.0", "fileExtensions": [".exl"] },

  // Token definitions — multi-typed by design. Lexeme → list of possible meanings.
  // "priority" (lower wins) breaks ties when scope/position filtering leaves >1 match.
  "tokens": {
    " ":  [{ "kind": "Whitespace",   "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace",   "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",      "flags": ["EmptySpace"] }],
    "//": [{ "kind": "LineComment",  "flags": ["EmptySpace"], "until": "newline" }],
    "/*": [{ "kind": "BlockComment", "flags": ["EmptySpace"], "until": "*/" }],
    "+":  [
      { "kind": "SumOperator",          "validScopes": ["Block", "Paren"], "priority": 10 },
      { "kind": "StringAppendOperator", "validScopes": ["Block", "Paren"], "priority": 20 }
    ],
    "<":  [
      { "kind": "LtOperator",              "validScopes": ["Block", "Paren"], "priority": 10 },
      { "kind": "GenericDefinitionOpener", "opensScope": "Generic",          "priority": 5  }
    ],
    "{":  [{ "kind": "BlockOpen",  "opensScope": "Block"  }],
    "}":  [{ "kind": "BlockClose", "closesScope": true    }],
    ";":  [{ "kind": "EndCommand" }]
  },

  // Reserved words.
  "keywords": [
    { "word": "if",    "kind": "IfKeyword"    },
    { "word": "else",  "kind": "ElseKeyword"  },
    { "word": "while", "kind": "WhileKeyword" }
  ],

  // Scope rules (which scopes a token may exist in; which it opens/closes).
  "scopes": {
    "validity": [
      { "scope": "Generic", "forbid": ["LeftShiftOperator", "RightShiftOperator"] }
    ]
  },

  // Expected node shapes — the tree of valid productions.
  "shapes": {
    "root":       { "sequence": [{ "repeat": "statement" }], "canEndSource": true },
    "statement":  { "alt": ["varDecl", "ifStmt", "block", "exprStmt"] },
    "varDecl":    { "sequence": ["VarKeyword", "Identifier", "EndCommand"] },
    "ifStmt":     {
      "sequence": [
        "IfKeyword", "(", "expression", ")", "block",
        { "optional": { "sequence": ["ElseKeyword", "block"] } }
      ]
    },
    "block":      { "sequence": ["{", { "repeat": "statement" }, "}"], "canEndSource": false },
    "exprStmt":   { "sequence": ["expression", "EndCommand"] },
    "expression": { "alt": ["literal", "identifier", { "binary": "...precedence-driven..." }] }
  }
}
```

#### Loading order

```
GrammarSchema::loadShipped("csharp")
  ↓  reads src/source-config/languages/csharp.lang.json (relative to binary)
  ↓  parses JSON (in detail/grammar_schema_json.cpp — nlohmann/json stays here)
  ↓  validates structure → collects ConfigDiagnostic vector, bails on errors
  ↓  intern rule + token-kind names → RuleInterner, SchemaTokenInterner
  ↓  compile "shapes" into a SchemaCursor-navigable graph
  ↓  FIRST/FOLLOW analysis → checks alt-overlap → C0010 if ambiguous
  ↓  freeze() both interners
  ↓  return Result{value, diagnostics}
```

A user supplies their own with `loadFromFile("./my-dsl.lang.json")` — same code path, no compilation, no engine change. **Hot reload** (e.g. for an IDE editing a config live) is just calling `loadFromFile` again — the loader is idempotent.

---

### 5.13 `ParseDiagnostic` — `parse_diagnostic.hpp/.cpp`

Every parse error is a structured value with enough metadata to produce a *useful* message: what was expected, what was seen, the scope it happened in, related locations, and exactly where.

```cpp
enum class DiagnosticSeverity : uint8_t { Hint, Info, Warning, Error };

// Stable enum, ~16 bits, exhaustively switchable. Strings derive from this in the formatter.
// "P" = parse-time, "C" = config-load, "S" = semantic (later), "I" = IR (later).
enum class DiagnosticCode : uint16_t {
    None = 0,

    // ---- P0xxx — parser / tree-builder ----
    P_UnexpectedToken            = 0x0001,
    P_MissingRequiredChild       = 0x0002,
    P_UnknownToken               = 0x0003,
    P_PrematureEndOfInput        = 0x0004,
    P_InvalidEscapeSequence      = 0x0005,
    P_NumericLiteralOutOfRange   = 0x0006,
    P_DeprecatedSyntax           = 0x0007,
    P_AmbiguousToken             = 0x0008,
    P_NoAlternativeMatched       = 0x0009,
    P_UnclosedScope              = 0x000A,
    P_UnmatchedClose             = 0x000B,

    // ---- P9xxx — builder internal-invariant violations (release-mode rescues) ----
    P_BuilderInvariant           = 0x9000,
    P_TooManyDiagnostics         = 0x9001,
    P_UnfinishedTree             = 0x9002,
    P_RecoveryStalled            = 0x9003,

    // ---- C0xxx — config loader (see §5.12) ----
    C_MissingField               = 0xC001,
    C_UnknownShape               = 0xC002,
    C_UnknownToken               = 0xC003,
    C_VersionMismatch            = 0xC005,
    C_CircularShape              = 0xC007,
    C_AmbiguousAlternatives      = 0xC010,
    C_UnclosableScope            = 0xC011,
};

struct DSS_EXPORT RelatedLocation {
    BufferId   buffer;        // may differ from the primary diagnostic's buffer
    SourceSpan span;
    std::string note;         // "previously declared here", "matching opener at line 12"
};

struct DSS_EXPORT ParseDiagnostic {
    DiagnosticCode        code;
    DiagnosticSeverity    severity;
    BufferId              buffer;             // primary location's buffer
    SourceSpan            span;               // primary location's span

    std::optional<RuleId> ruleContext;        // which expected shape was active

    // What the schema would have accepted at this position.
    // Free-form strings sourced from GrammarSchema::expectedAt():
    //   { "';'", "','", "expression" }.
    std::vector<std::string> expected;

    // What was actually seen (lexeme or token-kind name).
    std::string actual;

    // Builder-captured scope stack at the moment of the error.
    // Powers "got '>' while inside Generic scope" without state reconstruction.
    std::vector<ScopeKind> scopeStack;

    // Secondary locations: "matching opener", "previously declared", etc.
    std::vector<RelatedLocation> related;

    // Optional human-friendly hint: "did you forget a semicolon?"
    std::string suggestion;
};
```

Formatted output looks like:

```
error[P0001]: expected ';' or ',' — got '}'
  ╭─ src/foo.exl:14:23
  │
14│    var x = 1 + 2 }
  │                  ^ unexpected token
  │
note: matching opener at src/foo.exl:12:9
12│    var x = (
  │            ^ scope opened here
  │
scope: Root > Block > Paren
hint:  insert ';' before this token
```

The reporter (§5.14) owns the rendering; the diagnostic itself is data only.

---

### 5.14 `DiagnosticReporter` — `diagnostic_reporter.hpp/.cpp`

```cpp
// Per-code severity override + suppression. Configured per compilation unit.
struct DSS_EXPORT DiagnosticPolicy {
    std::unordered_map<DiagnosticCode, DiagnosticSeverity> overrides;  // demote/promote
    std::unordered_set<DiagnosticCode>                     suppress;   // drop these silently
    bool warningsAsErrors = false;                                     // strict mode
};

// Multi-buffer resolver — lets diagnostics span imports/includes once we have them.
class DSS_EXPORT BufferRegistry {
public:
    BufferId                            add(std::shared_ptr<SourceBuffer> buf);
    const SourceBuffer&                 get(BufferId id) const;        // asserts present
    std::shared_ptr<const SourceBuffer> tryGet(BufferId id) const;     // nullptr if absent
};

class DSS_EXPORT DiagnosticReporter {
public:
    struct Config {
        size_t maxDiagnostics = 1000;       // hard cap; emits P9001_TooManyDiagnostics on overflow
        size_t maxPerCode     = 50;         // per-code cap; coalesces beyond this
        DiagnosticPolicy policy;
    };

    explicit DiagnosticReporter(Config cfg = {});

    // Append a diagnostic. May be silently dropped (suppress), demoted/promoted (overrides),
    // coalesced (per-code cap), or replaced with a single P9001 once maxDiagnostics is hit.
    // Identical (code, buffer, span) within a 4-diag window is also deduped.
    void report(ParseDiagnostic d);

    std::span<const ParseDiagnostic> all() const;
    size_t errorCount()   const;
    size_t warningCount() const;
    bool   hasErrors()    const { return errorCount() > 0; }
    bool   hitCap()       const;                       // true if anything was dropped

    // Pretty-printers — take a registry so multi-file diagnostics format correctly.
    std::string formatAll(const BufferRegistry& bufs) const;       // all, sorted by (buffer, span)
    std::string format(const ParseDiagnostic& d,
                       const BufferRegistry& bufs) const;          // single
};
```

The `Tree` owns a `DiagnosticReporter` populated by `TreeBuilder`. Consumers retrieve it via `tree.diagnostics()`. Semantic phases later in the pipeline write *additional* diagnostics into the *same* reporter using the same data model — one reporter per compilation unit, regardless of phase.

**Policy use cases:**
- `--strict`: set `warningsAsErrors = true`.
- "Don't warn me about deprecated syntax in this codebase": `suppress.insert(P_DeprecatedSyntax)`.
- "Treat P_AmbiguousToken as an error here": `overrides[P_AmbiguousToken] = Error`.

---

### 5.15 Error Recovery Strategy

The builder chooses recovery per failure mode. **Every branch is required to make forward progress** (consume ≥1 token OR advance the schema cursor); a `(sourceIndex, schemaCursor)` watchdog catches violations and force-advances after 3 stalled iterations with `P_RecoveryStalled`.

| Situation | Recovery |
|---|---|
| Unexpected token, schema has a sync point (e.g. `;`, `}`, `)`) within N tokens | Skip tokens up to the sync point, attach skipped run as a single `Error` node spanning the run, continue from the sync point. Diagnostic: `P_UnexpectedToken` + the skipped lexemes in `related`. |
| Required child of an internal node absent (e.g. `if (` followed by `{` with no `)`) | Insert a synthetic node with `NodeFlags::Missing \| NodeFlags::Synthetic`, empty span at insertion point. Parent's span join ignores the empty operand. Diagnostic: `P_MissingRequiredChild` with `expected = ["')'"]`. Continue at the next expected position. |
| Unrecognized lexeme entirely | Emit `P_UnknownToken`, attach a single `Error` leaf covering the lexeme's span, advance one byte (UTF-8 safe — advance by one codepoint, actually). |
| EOF mid-shape | Close every open shape from deepest to root with synthetic `Missing` nodes for absent required children. Emit **one `P_PrematureEndOfInput` per unclosed open shape**, chained via the diagnostic's `related` list (deepest is the primary; each ancestor is a related location pointing at its opener). |
| Unmatched close (e.g. stray `}`) | Emit `P_UnmatchedClose`. If the schema knows what opener it expected, attach `related = [openerSpan]`. Consume the token as an `Error` leaf to make progress. |
| `alt` exhausted (no first-set match) | Emit `P_NoAlternativeMatched` with `expected` populated from FIRST(alt₁), FIRST(alt₂), … Consume one token as Error leaf. |
| Multi-typed token unresolved after scope + position + priority | Emit `P_AmbiguousToken`. First-declared meaning wins (deterministic). |

**`HasError` propagation timing.** Set on every ancestor *the moment the diagnostic is attached* by walking the stored `parent` chain — not deferred to `close()`. So errors inserted during EOF cleanup (when scopes are auto-closing) still mark every ancestor up to the root.

**No fail-fast.** The builder *always* produces a complete tree — error recovery is non-optional. `has(tree.flags(tree.root()), NodeFlags::HasError)` answers "was there any error"; `tree.diagnostics().hasErrors()` answers "should compilation halt before IR gen". Downstream phases can be configured to bail on any error or to continue (e.g. for an IDE that wants warnings about as much code as possible even when broken).

---

## 6. End-to-End Flow This Enables

```
                                       ┌───────────────────────┐
                       config file ───▶│ GrammarSchema::load*  │
                       (shipped or     └──────────┬────────────┘
                        user-supplied)            │
                                                  ▼
                                          GrammarSchema  ────────┐
                                                                  │ guides
SourceBuffer  ──▶  Tokenizer  ──▶  [Token]  ──▶  Parser ─────────▶│
   ▲                  ▲                            │              │
   │ source text      │ schema-aware               │ drives       │
   │                  │ recognition                ▼              │
   │                                         TreeBuilder ◀────────┘
   │                                               │
   │                                               │ schema-validated;
   │                                               │ invalid ⇒ Error nodes
   │                                               │            + diagnostics
   │                                               ▼
   │                                          Tree (CST)
   │                                          + DiagnosticReporter
   │                                               │
   │                       ┌───────────────────────┤
   │                       │                       │
   │              AST cursor (skips      CST cursor (sees every
   │              NodeFlag::EmptySpace)  token incl. EmptySpace)
   │                       │                       │
   │                       ▼                       ▼
   │             Semantic analysis      Formatter / IDE tools
   │           (writes NodeAttribute<T>;  (round-trips source)
   │            adds diagnostics)
   │                       │
   │                       ▼
   │                IR Generator                (later sub-plan)
   │                       │
   │                       ▼
   └─◀─── Optimizer  ──▶  Linker  ──▶  Target binary
```

The tree is the **hub**: schema validates it as it's built, parser writes it once, every downstream pass either reads from it or attaches attributes via side-tables. Diagnostics flow alongside; they never leave the tree's reporter.

---

## 7. Implementation Phases (Todos)

Each phase produces a self-contained, testable deliverable. Land them in order; each phase's tests gate the next.

| # | Status | ID | Title | Files | Notes |
|---|---|---|---|---|---|
| T0  | ✅ done | `tree-deps-cmake`     | nlohmann/json + GoogleTest FetchContent | root `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/core/CMakeLists.txt`, `templates/.gitignore.cpp` | CMake floor 4.0, C++23, nlohmann_json 3.12.0, GoogleTest 1.17.0 wired via FetchContent. `enable_testing()` + `option(DSS_BUILD_TESTS ON)`. |
| T1  | ✅ done | `tree-source-types`   | Source primitives + strong IDs | `strong_ids.hpp`, `source_buffer.*`, `source_span.*`, `token.hpp`, `interner.hpp`, `rule_id.hpp` (+ stub `rule_id.cpp`), `schema_token_interner.hpp` | `DSS_STRONG_ID` macro; `SourceSpan::of()` factory; `Interner<Id>` template (RuleInterner and SchemaTokenInterner are using-aliases); `freeze()` enforced. **8 + 9 + 10 + 4 + 8 = 39 test cases.** |
| T2  | ✅ done | `tree-storage`        | `Tree` arena + `Node` struct + `NodeFlags` | `tree_node.hpp`, `tree.hpp/.cpp`, stub `grammar_schema.hpp`, stub `diagnostic_reporter.hpp` | `detail::Node` is 40 bytes (relaxed from 32 once `DiagnosticIndex` was added); `NodeFlags` ops `inline constexpr`; discriminant-asserting `rule()`/`tokenKind()`/`diagnostic()`; release-fatal `node_(id)` bounds check. **9 + 10 = 19 test cases including a death test.** |
| T3  | ✅ done | `tree-diagnostics`    | Diagnostic types + reporter + policy | `scope_kind.hpp/.cpp`, `parse_diagnostic.hpp/.cpp`, `diagnostic_reporter.hpp/.cpp` | `DiagnosticCode` as `enum class : uint16_t` (P/C/S/I prefix ranges); `RelatedLocation`, scopeStack on every diag; `DiagnosticPolicy` (suppress/overrides/warningsAsErrors); `Config{maxDiagnostics,maxPerCode,dedupWindow}`; FNV-1a64 hash dedup including `ruleContext`; `BufferRegistry`. `format()` produces caret-pointed line + scope + related; `formatAll()` sorts by (buffer, span). **5 + 18 = 23 test cases** (includes ruleContext-in-hash regression). |
| T4  | ✅ done | `tree-schema-loader`  | `GrammarSchema` + `SchemaCursor` + `ScopeKind` + JSON loader + toy config | `grammar_schema.hpp/.cpp`, `grammar_schema_json.hpp/.cpp`, `schema_cursor.hpp`, `src/source-config/languages/toy.lang.json` | `GrammarSchemaData` POD mirrors Tree/TreeData split (no friend gymnastics). `LoadResult<T>` = `std::expected<T, vector<ConfigDiagnostic>>`. Pre-interns `Identifier`/`IntLiteral`/etc. as built-in token kinds. Walks parent dirs in `loadShipped` to be cwd-agnostic. nlohmann/json linked PRIVATE — no leak. **19 test cases** covering happy path + every C_* code. |
| T5  | ✅ done | `tree-builder`        | Schema-aware `TreeBuilder` with RAII `OpenScope` | `tree_builder.hpp/.cpp` | (a) Happy path verified. (b) `P_UnknownToken`/`P_UnexpectedToken` both produce Error nodes with scope-stack snapshot and HasError ancestor walk. (c) `P_PrematureEndOfInput` per unclosed shape, ruleContext-distinct via dedup-hash fix in reporter. (d) EmptySpace flag landed from `meaning.flagsApplied`. (e) `P_AmbiguousToken` warning + first-declared wins. (f) Forward progress guaranteed structurally (one token per `pushToken` call). (g) `OpenScope` RAII + move-only + idempotent close + cascade-cookie tracking. (h) Builder invariants (no-open-frame, popScope underflow, LIFO violation, double-finish) all emit `P_BuilderInvariant` in release. **+9 review-fix improvements** (rvalue-disqualified `open()`, leftover-scope diagnostic, `currentRule()` peek, empty-tree finish, etc.). **22 test cases** including OpenScope move semantics, LIFO cascade, death-test for double `finish()`. |
| T6  | ⏳ next | `tree-cursor`      | CST + AST cursors | `tree_cursor.hpp/.cpp` | AST mode skips by `isEmptySpace()` bit ONLY — `Missing`/`Synthetic` ARE visible. `Bookmark { tree*, id }` cross-tree restore debug-asserts. |
| T7  | ⏳ pending | `tree-visitor`     | Walk helpers | `tree_visitor.hpp` | Pre-order/post-order/skip-control over `TreeCursor`. Zero-alloc 10K-node walk. |
| T8  | ⏳ pending | `tree-attrs`       | `NodeAttribute<T>` side-tables | `tree_attrs.hpp` | Required `TreeId` association; cross-tree access debug-asserts. Sparse+dense storage; `set`/`get`/`tryGet`/`has`/`clear`/`size`/iteration. |
| T9  | ⏳ pending | `tree-views`       | Initial typed views | `tree_views.hpp` | `IdentifierView`, `LiteralView`, `BinaryExprView`, `BlockView`, `FunctionDeclView`. Each has `::from(tree, id)` returning `std::optional` based on rule check. |
| T10 | ⏳ pending | `tree-end-to-end`  | Toy parser + walker demo + broken-input demo | `tests/core/test_tree_end_to_end.cpp` | Drive `TreeBuilder` from a mocked tokenizer against `toy.lang.json`; walk the tree with a visitor; print it. Broken sample produces non-empty `DiagnosticReporter` with expected codes. |
| T11 | (rolling) | `tree-cmake-wireup` | Per-checkpoint `src/core/CMakeLists.txt` updates | `src/core/CMakeLists.txt` | Folded into each chunk as files land. No separate "wire all files" pass. |
| T12 | ⏳ pending | `tree-docs`        | Header docs + design docs | inline doxygen + `docs/tree-model.md` + `docs/language-config-spec.md` | New-contributor onboarding: read both docs and (a) add a typed view in <30 min, (b) author a `.lang.json` that loads cleanly. |

**Critical path:** T0 → T1 → T2 → T3 → T4 → T5 → T10.
**Parallelizable once deps met:** T3 + T4 after T1/T2. T6, T7, T8, T9 after T5. T11, T12 anytime.

---

## 8. Integration with the Parent Plan

This sub-plan **supersedes** the placeholder `ast.hpp` line in the parent plan's section 4.2.2. Update parent plan section #2 (`core-types` phase) so it depends on this sub-plan's T1–T9, then proceeds to add the remaining `core/error/` and `core/utils/` pieces.

Downstream phases now have well-defined inputs/outputs against the tree:

| Parent-plan phase | Consumes | Produces |
|---|---|---|
| `source-config-schema` (#3) | the JSON schema spec | shipped `*.lang.json` files under `src/source-config/languages/` |
| `source-factory` (#4) | a `.lang.json` path or shipped name | a `GrammarSchema` (this sub-plan's §5.12) |
| `tokenizer` (#5) | `SourceBuffer` + `GrammarSchema` | `std::vector<Token>` with multi-typed lexeme metadata |
| `analysis-lexical` (#6) | `[Token]` + `GrammarSchema` | validated `[Token]`; emits diagnostics into the same `DiagnosticReporter` |
| `analysis-syntactic` (#7) | `[Token]` + `GrammarSchema` | drives schema-aware `TreeBuilder` → `Tree` (this sub-plan) |
| `analysis-semantic` (#8) | `Tree` | populates `NodeAttribute<TypeInfo>`, `NodeAttribute<SymbolId>`; adds diagnostics |
| `gen-intermediate` (#9) | `Tree` + attribute tables | `IRProgram` (skipped if `tree.diagnostics().hasErrors()` in strict mode) |

`source-factory` (parent plan #4) becomes the **producer** of `GrammarSchema` defined here. It's the only place that knows about JSON; everyone else consumes the typed `GrammarSchema` object. No other phase needs to know how the tree is *built* — only how to *read* it.

#### Amendments to the parent plan

These supersede the corresponding sections of `compiler-implementation-plan.md`:

- **§2 / §4.2.2 (core-types).** The placeholder `ast.hpp` is *replaced* by the tree/node types defined in this sub-plan (§5.1–§5.11). Parent-plan phase #2 (`core-types`) now consists of this sub-plan's T1–T12 plus, separately, `core/error/` (subsumed by `DiagnosticReporter` here — drop the duplicate) and `core/utils/`.
- **§4.4 (source-factory).** `LanguageConfig` + model classes + `ConfigValidator` are *replaced* by `GrammarSchema` (§5.12 here). The `source-factory/models/` directory is removed; `source-factory/validators/` is removed. What remains of `source-factory` is a thin facade that resolves a language name to a config-file path and calls `GrammarSchema::loadFromFile`.
- **§4.5 (tokenizer).** The tokenizer **does not** skip whitespace or comments. It emits *every* token (`CoreTokenKind` set, `schemaKind` invalid). The schema-aware resolver inside `TreeBuilder::pushToken` assigns `schemaKind` and applies `NodeFlags::EmptySpace` per the config. This preserves the source for formatters/IDE tooling.
- **§4.6.2 (analysis-syntactic).** The parser drives the schema-aware `TreeBuilder` defined here (§5.7); the parent plan's "recursive descent / table-driven parser" line stands but is now framed as *how* the parser feeds the builder.
- **§4.7.1 (gen-intermediate).** The IR generator reads from `Tree` (via cursor/visitor) and `NodeAttribute<T>` side-tables populated by semantic analysis. It is **not** allowed to mutate the tree.
- **§7 (third-party dependencies).** nlohmann/json is *only* included by the schema loader (`grammar_schema_json.cpp`). Other modules never include it.
- **§9 (open questions).** Drop "Grammar format BNF vs PEG" — answered by §5.12 (sequence/alt/optional/repeat + no backtracking on alt). Drop "IR level" timing — unchanged. Add "schema expressiveness for context-sensitive languages" (carried over to §9 here).

---

## 9. Open Questions (deferred, not blocking T1–T12)

1. **Attribute storage strategy per attribute.** Sparse hash map vs dense vector — measure once a real attribute exists (TypeInfo, after semantic phase lands).
2. **Identifier/string interning.** A second interner alongside `RuleInterner` / `SchemaTokenInterner`, for identifier lexemes. Defer until tokenizer lands; the cost-benefit depends on actual program sizes.
3. **`SchemaCursor` implementation strategy.** Three viable approaches: (a) interpreter that walks the compiled shape tree node-by-node (simplest, slowest), (b) state-machine table generated from the shape graph at load time (medium, faster), (c) JIT-compiled cursor advance (overkill). Default to (a) for T4; revisit if profiling shows it dominates.
4. **Schema expressiveness for context-sensitive languages.** Today's sketch supports `sequence`, `alt`, `optional`, `repeat`, and `binary` (precedence-driven). No PEG-style negative lookahead, no semantic predicates, no dynamic context-sensitive rules. **T-SQL** is known to leak reserved words into identifier contexts — config authors may need an escape hatch (e.g., a `"contextualKeyword": true` token flag). Validate against the four target languages before freezing the schema grammar.
5. **Scope-dependent token meaning expressiveness.** `LexemeMeaning::validScopes` is a flat list. Some languages need *negated* scopes ("valid everywhere except in `String`") or scope-stack patterns ("valid only if `Generic` is the *outermost* scope, not a nested one"). Decide once we hit a real case.
6. **Schema versioning compatibility window.** `dssSchemaVersion` rejects mismatches today. As the schema format evolves, decide: (a) only the current major version, (b) auto-migrate older versions, (c) parallel loaders per major version. Defer until v2.
7. **Incremental re-parsing.** The arena + immutable-tree design *permits* it (build a new tree referencing unchanged subtrees), but the API for it is out of scope here.
8. **Persistence / on-disk caching.** Indices serialize trivially. Schema versioning is needed before this is useful. Defer.
9. **Concurrency model.** `Tree` and `GrammarSchema` are immutable after `finish()` / `load*()` (interners frozen). Concurrent readers safe without sync; concurrent attribute writes need per-attribute locking — left to the consumer. Document `thread_compatible` everywhere it applies.
10. **Hot-reload of user configs.** An IDE may want to live-edit a `.lang.json`. The loader is idempotent and cheap; full reload is simpler than a diff/patch API. Document but don't build a watcher.
11. **Backtracking-on-`alt` escape hatch.** First-match-wins is deterministic but rules out some natural grammars (e.g., where `expression` overlaps `pattern` until the next token disambiguates). If onboarding hits this wall, add an explicit `"speculative": true` shape flag with a bounded lookahead — but only when forced.

---

## 10. After This Sub-Plan: Language Onboarding (preview, not implemented here)

Once the tree model is solid, adding **C#**, **Dart**, **Transact-SQL**, and **SQLite** is **purely a config-authoring exercise** — no engine code changes, no recompiles. The user can substitute or override any of these by passing `--language-config <their-file.lang.json>`.

For each shipped language:

1. Author `src/source-config/languages/<lang>.lang.json` (tokens + scope rules + expected shapes).
2. Run the schema-aware `TreeBuilder` against a corpus of sample source files; assert clean parse (`!hasErrors()`).
3. Walk the tree; assert key shapes (every C# `MethodDeclaration` has an `Identifier` child, etc.).
4. (Optional) Hand-write typed views unique to that language (e.g., T-SQL `CteView`) — engine code, but optional ergonomics, not required for compilation.
5. IR generation — uses semantic attributes; the generator stays one config-agnostic implementation.

These steps will be tracked in a follow-up `languages-onboarding-plan.md` once T1–T12 are complete.
