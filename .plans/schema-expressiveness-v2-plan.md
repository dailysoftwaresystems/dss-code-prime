# DSS Code Prime — Schema Expressiveness v2 Plan

> Sub-plan of [`compiler-implementation-plan.md`](./compiler-implementation-plan.md).
> Follow-up to [`tree-node-model-plan.md`](./tree-node-model-plan.md) (v1 sub-plan).
>
> **v1 establishes the foundation** — `Tree`, `TreeBuilder`, `GrammarSchema` with `sequence` / `alt` / `optional` / `repeat` shapes.
> **v2 extends that schema** with the expressiveness needed to represent real production languages — operator precedence, contextual keywords, speculative alternatives, string interpolation, custom string literals, and richer scope-stack rules.
>
> v2 is **additive, not breaking**. Every config written for the v1 schema (including `toy.lang.json`) must continue to load unchanged. v2 adds new optional fields and shape kinds; the loader infers defaults when they're absent.

---

## 0. Current Status

| | |
|---|---|
| Status        | ⏳ **Pending** — does not start until the v1 E2E milestone is complete. |
| Trigger       | First attempt to author a real-language `.lang.json` (starting with a C subset). |
| Predecessors  | v1 T1–T12 (`tree-node-model-plan.md`); a minimal end-to-end pipeline (tokenize → parse → IR → emit → run on a toy program). |
| Successors    | `languages-onboarding-plan.md` (authoring `csharp` / `dart` / `tsql` / `sqlite` configs). |
| Scope         | Schema-level expressiveness only. No new tree-node types. No IR/codegen changes. Tokenizer gains "lexer modes" for string interpolation; lexer-mode *stack* is the checkpoint primitive's responsibility. |

This plan is **deliberately empirical**: PR0 authors a C subset against the v1 schema *as-is* and catalogs the gaps that surface. §4.1 defines the workflow for what happens when a PR0 gap doesn't fit the §5 design as written.

---

## 1. Goal

Make the `GrammarSchema` config format expressive enough to represent **the four target production languages** (C#, Dart, Transact-SQL, SQLite) as JSON configs, with no language-specific C++ in the engine.

Concretely: by the end of v2, all six of the following must be expressible in a `.lang.json` file and exercisable by `TreeBuilder`:

1. **Operator precedence + associativity** (left/right/none associative, infix/prefix/postfix arity).
2. **Contextual / soft keywords** (`await`, `yield`, `async` in C#; `late`, `required`, `sealed` in Dart).
3. **Reserved-word policy** (T-SQL's "any keyword can be an identifier" mode).
4. **Bounded speculative `alt`** with cheap, complete checkpoint/rollback.
5. **String interpolation** (`$"text {expr} more"` re-entering the grammar; nested interpolation via a *lexer-mode stack*).
6. **Custom string-literal variants** (verbatim `@"..."`, raw `r"..."`, triple-quoted `"""..."""`, dynamic-tag here-strings).

**Out of scope for v2:**
- Indentation-sensitive grammar (Python). None of the four target languages need it.
- Macro systems or preprocessor (Rust, C). Targets don't need them.
- Inline grammars / parser combinators (the schema stays declarative).
- Ternary `?:` (the `Prefix|Infix|Postfix` enum doesn't model mixfix; deferred to v3).
- User-defined operator overloading inside config (no target language needs runtime-extensible operator tables).
- Performance optimization of the schema interpreter — correctness first.

---

## 2. Design Decisions (locked in)

| # | Decision | Rationale |
|---|---|---|
| D1 | **Additive only.** v2 only adds optional fields and shape kinds. Every v1 `.lang.json` loads unchanged. | Backwards compatibility is non-negotiable for `toy.lang.json` and for any work that lands between v1 and v2. |
| D2 | **Empirical first.** PR0 authors a C-subset config against the v1 schema and *fails to load* it; the gap catalog drives PR1..PRN. The gap-catalog format and "what if a gap contradicts §5" workflow are pinned in §4.1. | Designing expressiveness in a vacuum produces abstractions that don't fit reality. |
| D3 | **Validation order: C-subset → T-SQL subset → C# / Dart.** | C-subset hits precedence + typedef + simple strings in ~200 lines of JSON. T-SQL is the worst-case reserved-word leak. Save C#/Dart for `languages-onboarding-plan.md` once v2 is locked. |
| D4 | **Schema interpreter, not codegen.** v2 stays interpreted at runtime — no offline pre-compilation. | Hot-reload of `.lang.json` is a goal (plan §9). Pre-compilation can come later if profiling demands it. |
| D5 | **One comprehensive plan, sized PR landings.** Each PR closes one concern, ships independently, but all live in this one document. | The concerns interlock: interpolation re-enters the grammar via speculative parse; precedence interacts with `alt` resolution; soft keywords lean on the schema cursor. Splitting risks designing them in isolation. |
| D6 | **TreeBuilder gains `Checkpoint` / `commit` / `rollback`.** Speculative parsing needs to undo arena pushes, frame-stack pushes, scope-stack changes, lexer-mode-stack changes, parser-side Pratt state, and diagnostic emissions (including `perCode_` counters, dedup window, and `hitCap_` latch). | First feature that mutates the builder's "single-pass append-only" character. Done well, this becomes a primitive every future advanced shape can lean on. |
| D7 | **Checkpoint default is rollback-on-destruct; commit is explicit.** Mirrors SQLite, std::filesystem transactions, every well-designed checkpoint API. Implicit-commit-on-destruct is a known anti-pattern: any early return, exception, or branch oversight silently commits corrupt state. | The cases where you "want" implicit commit are the cases where `commit()` documents intent; the cases where you forget are the cases that silently corrupt the tree. |
| D8 | **Tokenizer gains a lexer-mode *stack*.** Nested string interpolation requires a mode-stack, not a mode-switch — `$"a {$"{b}"} c"` returns to the *outer* `string-body`, not `main`. Mode operations are `pushMode("X")` / `popMode()` / `replaceMode("X")`. | Without a stack, nested same-style interpolation produces wrong tokens. The lexer-mode stack is part of `Checkpoint`'s snapshot. |
| D9 | **`dssSchemaVersion` field has semantic teeth.** A v1 loader (this build is v2, but eventually we'll ship a maintained branch) rejects `dssSchemaVersion: 2` with `C_VersionMismatch`. The v2 loader accepts `1` and `2`, rejects `99`. Under `dssSchemaVersion: 1`, v2-only fields produce `C_UnknownField` rather than silent acceptance. Under `dssSchemaVersion: 2`, all v1 and v2 fields are valid. | Forward-compat for v3 requires the loader to detect "config uses fields newer than this loader knows about." Silent acceptance makes typos undetectable. |
| D10 | **Acceptance criteria are assertion-shaped, not prose.** Each PR's acceptance bullet specifies the concrete `EXPECT_*` or `LoadResult` check that proves it. | Prose criteria require interpretation at PR-review time; concrete assertions are pasteable into ctest. |
| D11 | **`LexemeMeaning` extensions are reserved-name fields, not free-form metadata.** | Keeps the struct typed (no `std::map<string, json>` blob); preserves the v1 strong-typing pattern. |

---

## 3. Relationship to v1 (`tree-node-model-plan.md`)

v2 modifies the following v1 artifacts. All changes are additive (v1 configs load unchanged).

### Files to extend

| v1 file | v2 changes | First-touched-by |
|---|---|---|
| `core/types/grammar_schema.hpp`         | New fields on `LexemeMeaning` (precedence, associativity, arity). New top-level methods (`operatorTable()`, `lexerModes()`, `reservedWordPolicy()`). New `ScopeMatch` struct on `LexemeMeaning`. | PR1, PR2, PR3, PR5 |
| `core/types/grammar_schema_json.cpp`    | New JSON keys: `operators`, `contextual: true` on keywords, `reservedWordPolicy`, `scopeRequire` object, `mode: "string-body"` on tokens, `speculative: true` on alts, `interpolation: {...}`, `stringStyle`, `lexerModes`. | PR1, PR2, PR3, PR4, PR5, PR6 |
| `core/types/parse_diagnostic.hpp`       | New codes: `P_BacktrackFailed`, `P_ContextualKeywordResolution`, `P_MaxSpeculationDepth`, `C_InvalidPrecedenceTable`, `C_UnknownLexerMode`. | PR1, PR2, PR4, PR5 |
| `core/types/parse_diagnostic.cpp`       | The exhaustive switch in `diagnosticCodeName()` MUST be updated for every new code. **Easy-to-miss step**; explicitly listed in each PR's file set. | PR1, PR2, PR4, PR5 |
| `core/types/scope_kind.hpp`             | No source change unless onboarding introduces a built-in scope (e.g. `Interpolation`). Hold off until forced. | — |
| `core/types/tree_builder.hpp/.cpp`      | New `Checkpoint` type (rollback-on-destruct default, explicit `commit()`, `Disposition` enum). `checkpoint()`, `commit(Checkpoint&&)`, `rollback(Checkpoint&&)`. `pushToken` consults precedence + contextual rules. Lexer-mode-stack snapshot/restore. | PR2b, PR4 |
| `core/types/diagnostic_reporter.hpp/.cpp` | New `truncateTo(size_t newSize, RolllbackToken&)` API. Returns a `RolllbackToken` that restores `perCode_`, `recent_`, `hitCap_` deltas. Used exclusively by `TreeBuilder::Checkpoint`. | PR4 |
| `core/types/schema_cursor.hpp`          | `advance()` / `enterRule()` / `leaveRule()` become real (v1 §0 deviation #7 closed). New `expectedSet()` returning `std::span<SchemaTokenId const>`. | PR2a |
| `core/types/interner.hpp`               | Unchanged. | — |
| `src/source-config/languages/toy.lang.json` | Unchanged. Re-loaded as a smoke test in every v2 PR. | — |

### Files to add

| New file | Purpose | PR |
|---|---|---|
| `core/types/operator_table.hpp/.cpp`     | Compact representation of the schema's operator-precedence table. | PR1 |
| `core/types/lexer_mode.hpp/.cpp`         | Enum + helpers for tokenizer mode-switch tags declared by the schema. Includes the *lexer-mode stack* API used by checkpoint. | PR5 |
| `core/types/string_style.hpp`            | `StringStyleDescriptor` for custom string variants. | PR6 |
| `src/source-config/languages/c-subset.lang.json` | PR0 deliverable. | PR0 |
| `src/source-config/languages/tsql-subset.lang.json` | PR7 deliverable. | PR7 |
| `.plans/v2-gap-catalog.md`               | Empirically-discovered gaps from PR0, updated through PR7. Format defined in §4.1. | PR0 |
| `tests/core/test_operator_table.cpp`     | Tests for operator-table queries. | PR1 |
| `tests/core/test_schema_cursor.cpp`      | Walker tests (advance, enterRule, leaveRule, expectedSet). | PR2a |
| `tests/core/test_contextual_keywords.cpp`| Soft-keyword + reservedWordPolicy resolution tests. | PR2b |
| `tests/core/test_scope_require.cpp`      | `scopeRequire` (anyOf/forbid/topMustBe/outermost) tests. | PR3 |
| `tests/core/test_speculative_alt.cpp`    | `TreeBuilder::Checkpoint` rollback + nested + watchdog tests. | PR4 |
| `tests/core/test_string_interpolation.cpp` | Lexer-mode-stack interplay tests (stub-driver only; full integration with the tokenizer phase later). | PR5 |
| `tests/core/test_string_styles.cpp`      | `stringStyle` descriptor tests (stub-driver). | PR6 |

### What v2 deliberately does NOT touch

- `Tree`, `TreeCursor`, `NodeAttribute<T>`, typed views — unchanged.
- `BufferRegistry`, `ParseDiagnostic` shape (new *codes* only) — unchanged structurally.
- IR generator, optimizer, linker — out of scope.

---

## 4. Empirical validation strategy

Each PR is gated by a real config-authoring exercise. Work order:

```
PR0  →  Author c-subset.lang.json against v1 schema → catalog every gap (5–8 days).
PR1  →  Operator precedence + arity (LexemeMeaning + operator_table).
PR2a →  Real SchemaCursor::advance() walking the compiled shape graph.
PR2b →  Contextual keywords + reservedWordPolicy on top of the walker.
PR3  →  scopeRequire object (anyOf / forbid / topMustBe / outermost).
PR4  →  TreeBuilder::Checkpoint + speculative alt.
PR5  →  Lexer-mode stack + interpolation shape (schema-side; tokenizer integration deferred).
PR6  →  stringStyle descriptor for custom string variants (schema-side).
PR7  →  Stress test: tsql-subset.lang.json (5–10 days).
PR8  →  Plan-doc maintenance — amend v1 §9 (already done), parent plan, language-config-spec docs.
```

The C-subset target deliberately includes:
- `typedef` (forces contextual identifier resolution)
- Operator precedence (`a + b * c`, `a = b = c`, `!x`, `x++`)
- String literals (`"hello"` only — no escapes for v0.1)
- Block structure + scope rules
- Function declarations + `if`/`while`/`return` statements

It deliberately excludes:
- Preprocessor (out of scope)
- `goto`/labels
- Pointer-to-function syntax — pathological, defer to v3

### 4.1 Gap-to-PR workflow

When PR0 authors `c-subset.lang.json` against the v1 schema, gaps surface in three forms:

1. **Loader rejection** — the v1 loader emits a `C_*` diagnostic. Capture the exact code + JSON pointer + message.
2. **Loader silently accepts but builder rejects** — config loads but `TreeBuilder` cannot parse a sample C source. Capture the builder's `P_*` diagnostic + the input.
3. **Author judgment** — the config author knows the schema can't express something even if the loader accepts a workaround. Capture the rationale + the language construct.

Each gap becomes a row in `.plans/v2-gap-catalog.md`:

```markdown
| # | Symptom               | Source/JSON           | Diagnostic        | Resolves via   | Sample             |
|---|-----------------------|-----------------------|-------------------|----------------|--------------------|
| 1 | `a + b * c` parses    | shapes/expression     | none — builder    | §5.1 (PR1)     | `1 + 2 * 3;`       |
|   | as left-assoc rather  |                       | accepts but tree  |                |                    |
|   | than `(1 + (2 * 3))`  |                       | is wrong shape    |                |                    |
```

**When a gap fits the existing §5 design** (rows 1–N), close in the relevant PR.

**When a gap doesn't fit §5** (PR0 surfaces something §5 doesn't cover):
- (a) Small extension fits naturally → amend §5 in this plan; record the amendment in a per-PR "design diff" appended to the catalog row.
- (b) Large extension would derail the PR's scope → log in §9 as a v3 candidate with the explicit trigger condition.
- (c) Out of v2 scope entirely → log in §9 with the rationale.

Branch (a) is the expected path for most gaps. Branches (b)/(c) require explicit triage by the author before continuing the PR.

---

## 5. Per-concern design

### 5.1 Operator precedence + associativity

**Problem.** v1's `shapes` produce a tree by sequence/alt. They have no way to say "an `expression` is a sequence of atoms separated by operators where `*` binds tighter than `+` and `=` is right-associative." Without precedence, every expression-bearing grammar needs hand-coded climbing per operator group.

**v2 design.**

Extend `LexemeMeaning`:

```cpp
struct LexemeMeaning {
    SchemaTokenId  id;
    std::int32_t   priority      = 0;            // v1 — for multi-meaning tiebreak (unrelated to operator precedence)
    NodeFlags      flagsApplied  = NodeFlags::None;
    ScopeKind      opensScope    = ScopeKind::None;
    bool           closesScope   = false;
    ScopeMatch     scopeRequire;                  // PR3 — replaces v1's flat validScopes

    // ── v2/PR1 additions ──
    std::optional<std::int32_t>   precedence;    // higher binds tighter; absent = "not an operator"
    OperatorAssoc                 associativity = OperatorAssoc::None;   // Left | Right | None
    OperatorArity                 arity         = OperatorArity::Infix;  // Prefix | Infix | Postfix
};
```

A lexeme that is both prefix and infix (`-` in `-a + b`) lands as **two `LexemeMeaning` entries** in the `lexemeTable[*lexeme*]` vector — distinguishable by `arity`. The v1 vector-of-meanings shape already supports this.

Top-level config section:

```jsonc
"operators": {
  "groups": [
    { "precedence": 10, "associativity": "right", "operators": ["="]   },
    { "precedence": 50, "associativity": "left",  "operators": ["+", "-"] },
    { "precedence": 60, "associativity": "left",  "operators": ["*", "/"] },
    { "precedence": 70, "associativity": "right", "arity": "prefix", "operators": ["!", "-"] },
    { "precedence": 80, "associativity": "left",  "arity": "postfix", "operators": ["++"] }
  ]
}
```

The loader transfers each group's `precedence`/`associativity`/`arity` onto matching `LexemeMeaning` records. A lexeme appearing in two groups (`-` in prefix + infix groups) yields two cloned meanings.

New shape kind:

```jsonc
"shapes": {
  "expression": {
    "expr": {
      "atom":          "primary",   // rule that parses one operand
      "minPrecedence": 0            // optional; defaults to 0
      // "allowAssignment" was proposed in an earlier draft — replaced by
      // a more general `excludeOperators: ["="]` knob in v3 if real
      // languages need fine-grained control. v2 keeps it simple.
    }
  }
}
```

**Pratt is parser-side, not builder-side.** The `expr` shape body declares the expression-parsing contract; the *parser* (parent-plan phase #7) is what climbs operators using `OperatorTable::lookup`. `TreeBuilder` itself does **not** gain a `parseExpression` method — the parser drives `open` / `pushToken` / `close` as it climbs.

This split is deliberate:
- The builder remains state-mutation-only (single responsibility).
- The parser owns the algorithm and consults the schema.
- Speculative `alt` (§5.4) still works because the parser's own Pratt stack is snapshotted into `Checkpoint` (see §5.4 cross-cutting).

**API additions.**
- `GrammarSchema::operatorTable()` → `OperatorTable const&`.
- `OperatorTable::lookup(SchemaTokenId, OperatorArity)` → `std::optional<{precedence, associativity}>`.

**Acceptance (assertion-shaped).**
- `EXPECT_EQ(schema->operatorTable().lookup(plus, OperatorArity::Infix)->precedence, 50);`
- `EXPECT_EQ(schema->operatorTable().lookup(plus, OperatorArity::Infix)->associativity, OperatorAssoc::Left);`
- `EXPECT_EQ(schema->operatorTable().lookup(minus, OperatorArity::Prefix)->precedence, 70);`
- `EXPECT_FALSE(schema->operatorTable().lookup(identifier, OperatorArity::Infix).has_value());`
- Parser-side parse-tree assertions are **deferred** until the parser exists (PR1 acceptance does not include them; the test harness uses a hand-written climbing test driver in `test_operator_table.cpp` that consumes only `OperatorTable`).

**Deferred to v3.**
- Ternary `?:` (mixfix; the `Prefix|Infix|Postfix` enum can't express it).
- Fine-grained per-context operator exclusion (`allowAssignment` evolved away).

---

### 5.2 Contextual / soft keywords + reserved-word policy

**Problem.** v1's keywords are unconditionally resolved. This breaks:

1. **Soft keywords** — `await` is a keyword inside `async` methods (C#); outside, it's a legal identifier. Dart's `late`, `required`, `sealed` are similar.
2. **Reserved-leakage** — T-SQL has *no* truly reserved words; `SELECT [Select] FROM T` is valid SQL.

**v2 design — soft keywords.**

```jsonc
"keywords": [
  { "word": "await",  "kind": "AwaitKeyword",  "contextual": true },
  { "word": "yield",  "kind": "YieldKeyword",  "contextual": true },
  { "word": "if",     "kind": "IfKeyword" },               // hard
  { "word": "return", "kind": "ReturnKeyword" }            // hard
]
```

Resolution rule (`pushToken`, single-token lookahead — no `Checkpoint` needed):
- Hard keyword: always wins over Identifier.
- Soft keyword (`contextual: true`): consult `SchemaCursor::expectedSet()` at the current cursor position. If the keyword's `SchemaTokenId` is in the expected set, it wins; otherwise the lexeme degrades to `Identifier`.

**Inside-async case.** `await` being a keyword inside `async Task M() { ... }` and an identifier elsewhere is expressed by the schema's shape graph: the `methodBody` rule reached inside an `async`-modified declaration has `AwaitExpression` in its `expectedSet()`. The shape graph itself encodes which scopes/rules expose `await` as a possible token; the builder's resolution rule just queries that. **No special-case "async mode" needed in the schema**; it's a consequence of the cursor's position.

`where` (C#) opening a constraint scope while ALSO being contextual: resolution order is **classify-then-side-effect**. Resolve the meaning (contextual → either WhereKeyword or Identifier), THEN apply `opensScope` if the meaning won. The plan §5.4 covers Pratt × speculative; §5.5 covers lexer-modes × speculative; here the simpler interaction is "classify before scope-effect" which is the existing v1 order.

**v2 design — reserved-word policy.**

```jsonc
"reservedWordPolicy": "strict"     // default — every keyword is reserved
// or
"reservedWordPolicy": "contextual" // T-SQL — every keyword degrades when not expected
```

`reservedWordPolicy: contextual` implicitly sets `contextual: true` on every keyword, regardless of per-entry flag. The toy.lang.json has no `reservedWordPolicy` field and gets `strict` by default — backwards-compat.

**Dependency.** PR2b consumes the real `SchemaCursor::advance()` / `expectedSet()` API, which lands in PR2a. This is the **shape-graph walker** — closing v1 §0 deviation #7 (the deferred work in v1 §9 item 3). PR2a is a multi-day deliverable in its own right.

**API additions.**
- `GrammarSchema::reservedWordPolicy()` → `enum { Strict, Contextual }`.
- `SchemaCursor::expectedSet()` → `std::span<SchemaTokenId const>`. Built into the compiled shape graph at load time.

**Acceptance (assertion-shaped).**
- PR2a: `EXPECT_TRUE(cursor.expectedSet().contains(varKeyword));` at the position before a `varDecl`.
- PR2a: `cursor = schema->advance(cursor, varKeyword);` returns a valid cursor; `expectedSet()` returns `{Identifier}` next.
- PR2b: in a config with `await` declared contextual, the leaf produced for `int await = 0;` has `tokenKind == Identifier` (not `AwaitKeyword`); `t.diagnostics().errorCount() == 0`.
- PR2b: with `reservedWordPolicy: contextual` set, `CREATE TABLE Select(Order int)` parses with `Select` and `Order` as Identifier leaves.

---

### 5.3 Scope-stack patterns

**Problem.** v1 has two scope mechanisms that the v2 design must not conflate:

1. **`LexemeMeaning::validScopes`** — a flat per-token list that the v1 loader populates but `pushToken` does **not** currently consult. This is the API v2 §5.3 extends.
2. **`scopes.validity[].forbid`** — a top-level per-scope `forbid` list that `pushToken` *does* consult via `isTokenValidInScope`. This is the v1 mechanism `toy.lang.json` uses and it stays alive untouched.

v2 §5.3 extends path (1) only.

**v2 design.**

Replace the v1 single `validScopes: [...]` field on per-token meanings with a richer object:

```jsonc
"+":  [
  { "kind": "SumOperator",
    "scopeRequire": {
      "anyOf":      ["Block", "Paren"],     // v1 list, now nested — at least one on stack
      "forbid":     ["Generic"],            // NEW — none of these may be on stack
      "topMustBe":  null,                   // NEW — innermost scope must equal this id
      "outermost":  null                    // NEW — outermost scope must equal this id
    }
  }
]
```

Loader compiles into `LexemeMeaning::scopeRequire`:

```cpp
struct ScopeMatch {
    std::vector<ScopeKind>     anyOf;        // empty = match any
    std::vector<ScopeKind>     forbid;       // empty = no forbidden scopes
    std::optional<ScopeKind>   topMustBe;
    std::optional<ScopeKind>   outermost;
};
```

Builder consults this in `pushToken`'s scope filter. Check order: `forbid` → `topMustBe` → `outermost` → `anyOf`; first failure rejects the meaning.

**Interaction with §5.2 contextual degradation.** When a contextual keyword degrades to `Identifier`, the `Identifier` meaning's `scopeRequire` is the one that gets checked — *not* the keyword's. Two-meaning lookup: first resolve which meaning applies (contextual rule), then apply its scope filter.

**Normalization.** When both `topMustBe` and `anyOf` are set, `topMustBe` is the stronger constraint and `anyOf` becomes redundant; the loader emits a warning (`C_RedundantScopeRequire`) but accepts.

**Backwards compat.** v1 flat `validScopes: [...]` syntax loads as `scopeRequire.anyOf = [...]` with all other fields default. `toy.lang.json`'s `scopes.validity[].forbid` stays unchanged — it's a different mechanism.

**Acceptance (assertion-shaped).**
- `EXPECT_FALSE(schema->isTokenValidInScope(ltOperator, scopeStackWithGeneric));`
- A config using `"scopeRequire": { "forbid": ["Generic"] }` rejects `LtOperator` when `Generic` is on the stack.
- A config using `"scopeRequire": { "topMustBe": "Switch" }` rejects `CaseKeyword` when `Switch` is not the innermost scope.
- `toy.lang.json` reloads without error after v2.

---

### 5.4 Speculative `alt` (bounded backtracking)

**Problem.** v1 `alt` is first-match-wins on FIRST-set. Breaks when two alternatives share a prefix:

```
expression = patternMatchExpr | regularExpr
patternMatchExpr = "case" pattern "=>" expression
regularExpr     = "case" pattern "when" expression "=>" expression
```

The disambiguating token may be 3 tokens deep.

**v2 design.**

New shape attribute on `alt`:

```jsonc
"shapes": {
  "expression": {
    "alt": ["patternMatchExpr", "regularExpr"],
    "speculative": true,
    "lookahead": 6        // optional; default = 8
  }
}
```

When `speculative: true`, the builder:
1. Takes a `Checkpoint` of its complete state.
2. Tries the first alternative. If it consumes ≤ `lookahead` tokens and:
   - Succeeds without producing an Error/Missing node → `commit(checkpoint)`. Branch wins.
   - Hits an Error/Missing within the window → `rollback(checkpoint)`. Try the next alternative.
3. If all alternatives fail, emit `P_NoAlternativeMatched`; the final state is the last rollback. Both-succeed is **first-wins** (deterministic, no longest-match in v2).

#### `TreeBuilder::Checkpoint` API

```cpp
class DSS_EXPORT TreeBuilder {
public:
    // ... existing ...

    // Move-only RAII guard. By default — destructor rolls back. To keep the
    // speculative work, call `commit(std::move(cp))`. Mirrors std::filesystem
    // and SQLite transaction conventions: silent commit is exactly the wrong
    // default for a backtracking primitive.
    class DSS_EXPORT Checkpoint {
    public:
        enum class Disposition : std::uint8_t { Pending, Committed, RolledBack };

        Checkpoint(Checkpoint&&) noexcept;
        Checkpoint& operator=(Checkpoint&&) noexcept;
        Checkpoint(Checkpoint const&)            = delete;
        Checkpoint& operator=(Checkpoint const&) = delete;
        ~Checkpoint() noexcept;     // rolls back if still Pending; logs P9 diagnostic if so (helps catch forgotten-commit bugs)

        [[nodiscard]] Disposition disposition() const noexcept;

    private:
        friend class TreeBuilder;
        // opaque snapshot (see field list below)
    };

    [[nodiscard]] Checkpoint checkpoint();
    void                     commit(Checkpoint&& cp);    // mark committed; release without restoring
    void                     rollback(Checkpoint&& cp);  // restore all snapshotted state
};
```

#### What `Checkpoint` snapshots — the complete list

| Subsystem | Field(s) | Rollback strategy |
|---|---|---|
| Builder arena | `nodes_.size()`, `childIndex_.size()` | Truncate vectors. O(diff). |
| Open-frame stack | `open_.size()` AND each frame's `children.size()` | **Frame children stored as offset-into-`childIndex_`** (NOT a per-frame `vector<NodeId>`). Rollback truncates one vector. v2 PR4 refactors `Frame::children` from `std::vector<NodeId>` to `{ childIndexStart, childIndexCount }`. |
| Scope stack | `scopes_.size()` | Truncate. |
| Cookie counter | `nextCookie_` | Restore the integer. |
| `closedCookies_` | size + entry set | Set-of-cookies-closed-since-checkpoint, undone on rollback (so future RAII-close-after-rollback doesn't double-no-op). |
| `DiagnosticReporter::all_` | `.size()` | Truncate. |
| `DiagnosticReporter::perCode_` | per-code counters | Snapshot delta; restore. |
| `DiagnosticReporter::recent_` | dedup window deque | Snapshot delta; restore. |
| `DiagnosticReporter::hitCap_` | the one-way latch | **Critical** — if a speculative branch hits the cap, rollback must clear the latch. Otherwise the reporter permanently silences after a failed speculation. |
| Tokenizer lexer-mode stack | mode stack snapshot | The tokenizer exposes `pushMode`/`popMode` and `modeStackDepth()`. Rollback restores depth + the top frame's mode tag. **Tokenizer ownership** — TreeBuilder calls the tokenizer's snapshot/restore primitives; the tokenizer phase implements them when authored. PR4 declares the contract; tokenizer phase honors it. |
| Parser-side Pratt state | per-parser precedence stack | The parser is responsible for its own checkpoint integration: it observes `TreeBuilder::checkpoint()` / `rollback()` and snapshots/restores its operator-stack accordingly. PR4 documents the contract; the parser phase implements it. |

`Frame::children` refactor is part of PR4's deliverable. The motivation is twofold: (a) cheap rollback via index truncation, (b) avoids O(frames) heap allocations per checkpoint.

#### `DiagnosticReporter` API additions

```cpp
class DiagnosticReporter {
public:
    // ... existing ...

    // Snapshot just enough state for a speculative rollback. Opaque token —
    // the caller passes it back to truncateTo() to restore.
    struct Snapshot {
        std::size_t allSize;
        std::unordered_map<DiagnosticCode, std::size_t> perCodeDelta;  // baseline counters
        std::size_t recentSize;
        bool        hitCap;
    };
    [[nodiscard]] Snapshot snapshotForRollback() const noexcept;
    void                    truncateTo(Snapshot const& snap);   // restores all fields above
};
```

These are used exclusively by `TreeBuilder::Checkpoint`. They're not public-API-quality for general consumers (no thread-safety, no semantic guarantees beyond "use it with the matching checkpoint").

#### Watchdog

A counter caps total speculation depth at `maxSpeculationDepth` (default 64). Exceeded → `P_MaxSpeculationDepth`. Prevents pathological grammars from blowing memory.

#### Cost budget

`Checkpoint` should be cheap. Acceptance includes a benchmark target: **parsing a 1000-token source with one outer speculative alt completes in under 50 ms on the reference machine (GCC 13.2, Release build).** Each individual checkpoint allocates O(1) — just the diagnostic snapshot's hashmap and a few size_t's; the arena/scope/cookie/children-index rollbacks are integer-truncations.

#### Acceptance (assertion-shaped)

- `auto cp = b.checkpoint(); b.pushToken(t); b.rollback(std::move(cp)); EXPECT_EQ(b.openFrameCount(), preFrameCount);`
- Nested speculation: `cp1 = b.checkpoint(); cp2 = b.checkpoint(); b.commit(std::move(cp2)); b.rollback(std::move(cp1));` — restored state matches pre-cp1.
- Diagnostics emitted in a rolled-back branch are absent from `b.finish().diagnostics().all()`.
- Reporter `hitCap_` reset after rollback: a speculative branch that produces 1001 diagnostics rolls back; subsequent real diagnostics are reported normally.
- `maxSpeculationDepth = 3` with 4 nested speculative alts emits exactly one `P_MaxSpeculationDepth`.
- `Checkpoint` destructor with `Disposition::Pending` emits `P_UncommittedCheckpoint` (warning) and rolls back.
- Benchmark: 1000-token speculative source under threshold.

---

### 5.5 String interpolation

**Problem.** `$"hello {name}, you are {age + 1}"` is a string literal whose body contains arbitrary expressions in `{...}`. The tokenizer can't represent this with a single token — it needs to switch modes between "scanning string body" and "scanning normal grammar." Nested interpolation (`$"a {$"{b}"} c"`) needs a *mode stack*, not a single switch.

**v2 design.**

Tokenizer carries a *mode stack*. Operations:
- `pushMode("X")` — push a new mode atop the stack.
- `popMode()` — return to the previous mode.
- `replaceMode("X")` — swap the top of the stack (rare; useful when entering a sibling mode like switching escape-rules without nesting).

Schema declares modes:

```jsonc
"lexerModes": {
  "main": {
    "tokens": "default"            // the v1 tokens map
  },
  "string-body": {
    "tokens": {
      "{":     [{ "kind": "InterpolationOpen",   "modeOp": "pushMode", "modeArg": "main" }],
      "\"":    [{ "kind": "StringEnd",           "modeOp": "popMode" }],
      "\\":    [{ "kind": "EscapeChar",          "consume": "next-char" }]
    },
    "defaultToken": { "kind": "StringChar" }   // anything not matched goes here
  }
}
```

Token meanings gain optional `modeOp` (one of `pushMode` | `popMode` | `replaceMode`) and `modeArg` (the target mode name, when applicable).

**Nested-interpolation semantics.** When the inner `"` in `$"a {$"{b}"} c"` triggers `popMode`, the tokenizer pops back to the outer `string-body` — NOT to `main`. Because mode operations are stack ops, this works automatically.

**Lexer-mode stack is part of `Checkpoint`.** See §5.4 — speculative branches that cross `pushMode`/`popMode` must restore the stack on rollback. The tokenizer exposes `modeStackDepth()` and `snapshotMode()`/`restoreMode()` to the builder's checkpoint mechanism.

#### Shape side

```jsonc
"shapes": {
  "interpolatedString": {
    "sequence": [
      "StringStart",
      { "repeat": { "alt": ["StringChar", "EscapeChar",
                            { "sequence": ["InterpolationOpen", "expression", "InterpolationClose"] }] } },
      "StringEnd"
    ]
  }
}
```

Shape graph doesn't change — `sequence`/`alt`/`repeat`. The novelty is that tokens come from *different lexer modes* depending on the tokenizer's mode-stack top.

#### API additions

- `GrammarSchema::lexerModes()` → const-ref to the compiled mode table.
- `LexerMode::tokens()` / `LexerMode::defaultToken()`.
- Tokenizer (when authored) reads `lexerModes` to decide what to scan next. PR5 deliverable includes a **stub-driver test** that fakes a tokenizer by manually calling mode operations and verifying the schema's compiled table responds correctly.

**Backwards compat.** If `lexerModes` is absent, the tokenizer behaves exactly as v1 (one implicit "main" mode using the `tokens` map directly).

**Acceptance (assertion-shaped).**
- `EXPECT_TRUE(schema->lexerModes().contains("string-body"));`
- `EXPECT_EQ(schema->lexerModes().get("string-body").tokenForLexeme("\"").modeOp, ModeOp::PopMode);`
- Stub-driver: simulated `pushMode/popMode/popMode` returns the tokenizer to the same mode it started in.
- Stub-driver: nested interpolation (`pushMode("string-body") → pushMode("main") → popMode → push("string-body") → popMode → popMode`) leaves stack empty.
- Schema with circular mode reference (`main` switches to `string-body` which switches to `main` which switches to `string-body` ...) loads cleanly; cyclic mode references are *normal*, not an error.

**Limitations explicitly accepted in v2.**
- Format specifiers (`{x:0.00}`) → separate `StringFormatSpec` token; rich DSL inside `{...}` is v3.
- Multiline interpolated strings: `\n` is a `StringChar`.

---

### 5.6 Here-strings / custom string literals

**Problem.** Languages use varied string literal forms:
- C#: `@"text"` (verbatim — `""` escapes `"`)
- C++: `R"DELIM(...)DELIM"` (raw with custom delimiter tag)
- Dart: `r"..."` (raw)
- C# 11 / Python: `"""..."""` (triple-quoted, multiline)
- Bash: `<<EOF ... EOF` (here-doc)

**v2 design.**

```jsonc
"tokens": {
  "\"":  [{ "kind": "StringStart", "modeOp": "pushMode", "modeArg": "string-body",
            "stringStyle": { "escapeKind": "char", "escapeChar": "\\", "endsAt": "\"" } }],
  "@\"": [{ "kind": "VerbatimStringStart", "modeOp": "pushMode", "modeArg": "verbatim-body",
            "stringStyle": { "escapeKind": "doubled-delimiter", "endsAt": "\"" } }],
  "R\"": [{ "kind": "RawStringStart", "modeOp": "pushMode", "modeArg": "raw-body",
            "stringStyle": { "escapeKind": "none", "delimiterTag": "matched", "tagPattern": "[A-Za-z_]{0,16}", "endsAt": ")\"" } }],
  "\"\"\"": [{ "kind": "TripleStringStart", "modeOp": "pushMode", "modeArg": "triple-body",
            "stringStyle": { "escapeKind": "char", "escapeChar": "\\", "endsAt": "\"\"\"",
                             "endsAtLongestMatch": true, "multiline": true } }]
}
```

`stringStyle` fields:

| Field | Type | Semantics |
|---|---|---|
| `escapeKind` | enum: `"char"` \| `"doubled-delimiter"` \| `"none"` | How escape sequences work. |
| `escapeChar` | string (1 char) | When `escapeKind: "char"`, the lead char (`\`). |
| `endsAt` | string | The literal sequence that terminates the body. |
| `endsAtLongestMatch` | bool (default false) | When true (`"""`), the tokenizer consumes the longest run matching `endsAt` from the end; useful for `""""abc"""""` parsing as `"""abc"""""` → `""""abc"""` body, end at `"""`. |
| `delimiterTag` | string: `"matched"` or omitted | When `"matched"`, the tokenizer captures the tag dynamically at the open delimiter and matches it at close. See below. |
| `tagPattern` | string regex (default `[A-Za-z0-9_]{0,16}`) | Constraint on what characters are valid in the captured tag. Bounded by length for safety. |
| `multiline` | bool (default false for `"`, true for `"""` / verbatim) | Whether newlines are allowed in body. |

#### `delimiterTag: "matched"` — dynamic tag handling

C++ raw strings `R"DELIM(body with )other_tag" not closing here)DELIM"` require:

1. At the open delimiter (`R"`), the tokenizer reads ahead until `(`, capturing chars matching `tagPattern` into a *dynamic tag variable*. Max 16 chars (configurable).
2. The tokenizer enters `raw-body` mode WITH the captured tag stored on the mode-stack frame.
3. While in the body, the close pattern is `)<captured-tag>"` — checked at every character.
4. The captured tag is part of the mode-stack frame, so `Checkpoint`/`rollback` correctly preserves it.

**API impact.** `LexerMode` frames are no longer just `{modeName}` — they're `{modeName, optional<string> dynamicTag}`. The tokenizer phase implements this; PR6 just declares the schema fields and stub-driver tests them.

**Triple-quoted handling.** `"""hello """ world"""` — three quotes end the string, but only at a run *longest* in the input. `endsAtLongestMatch: true` enables this; the tokenizer reads forward from any `"""` candidate and includes any preceding extra `"`s in the body.

**Backwards compat.** v1 has no `stringStyle` field; the default behaves like v1 (no recognition of `@"` / `R"` / `"""` as openers).

**Acceptance (assertion-shaped).**
- Stub-driver: `tokenStart("@\"").stringStyle.escapeKind == EscapeKind::DoubledDelimiter`.
- Stub-driver: `tokenStart("R\"").stringStyle.delimiterTag.has_value()`.
- Loader: a config declaring `delimiterTag: "matched"` with no `tagPattern` defaults to `[A-Za-z0-9_]{0,16}`.
- Loader: a config declaring `tagPattern` with invalid regex emits `C_InvalidStringStyle`.
- Tokenizer integration deferred to its phase; PR6 ships schema-side only.

**v2 ceiling.** Python f-strings (triple-quotes + interpolation + format specs stacking) → v3.

---

## 6. Combined config-file shape (all v2 extensions)

```jsonc
{
  "dssSchemaVersion": 2,                                                 // PR0 — bump

  "language": { "name": "...", "version": "...", "fileExtensions": [".x"] },

  "reservedWordPolicy": "strict",                                        // PR2b — strict | contextual

  "lexerModes": {                                                        // PR5/PR6
    "main":           { "tokens": "default" },
    "string-body":    { "tokens": { ... },  "defaultToken": { "kind": "StringChar" } },
    "verbatim-body":  { "tokens": { ... },  "defaultToken": { "kind": "StringChar" } }
  },

  "tokens": {
    "+":  [
      { "kind": "SumOperator",
        "scopeRequire": { "anyOf": ["Block","Paren"], "forbid": ["Generic"] }     // PR3
      }
    ],
    "\"": [
      { "kind": "StringStart", "modeOp": "pushMode", "modeArg": "string-body",   // PR5
        "stringStyle": { "escapeKind": "char", "escapeChar": "\\", "endsAt": "\"" }  // PR6
      }
    ]
  },

  "keywords": [
    { "word": "if",    "kind": "IfKeyword" },                                     // hard
    { "word": "await", "kind": "AwaitKeyword", "contextual": true }               // PR2b — soft
  ],

  "operators": {                                                                  // PR1
    "groups": [
      { "precedence": 10, "associativity": "right", "operators": ["="]   },
      { "precedence": 50, "associativity": "left",  "operators": ["+","-"] },
      { "precedence": 70, "associativity": "right", "arity": "prefix", "operators": ["!","-"] }
    ]
  },

  "scopes": {
    "validity": [
      { "scope": "Generic", "forbid": ["LtOperator"] }                            // v1 — unchanged
    ]
  },

  "shapes": {
    "root":       { "sequence": [{ "repeat": "statement" }] },
    "statement":  {
      "alt": ["patternStmt", "exprStmt"],
      "speculative": true, "lookahead": 6                                         // PR4
    },
    "patternStmt": { "sequence": ["CaseKeyword", "pattern", "ArrowOperator", "expression", "EndCommand"] },
    "exprStmt":    { "sequence": ["expression", "EndCommand"] },
    "pattern":     { "alt": ["literal", "identifier"] },
    "expression": { "expr": { "atom": "primary" } },                              // PR1
    "primary":    { "alt": ["literal", "identifier",
                            { "sequence": ["(", "expression", ")"] }] },
    "interpolatedString": {                                                       // PR5
      "sequence": [
        "StringStart",
        { "repeat": { "alt": ["StringChar", "EscapeChar",
                              { "sequence": ["InterpolationOpen", "expression", "InterpolationClose"] }] } },
        "StringEnd"
      ]
    }
  }
}
```

---

## 7. Implementation Phases

Each PR is sized in **person-days** assuming someone with v1-codebase context. Acceptance bullets are assertion-shaped.

| # | Status | ID | Title | Days | Files | Acceptance |
|---|---|---|---|---|---|---|
| **PR0** | ⏳ pending | `v2-empirical-c-subset` | Author `c-subset.lang.json` against v1 schema; build gap catalog | **5–8** | `src/source-config/languages/c-subset.lang.json` + `.plans/v2-gap-catalog.md` | (a) Config loads or fails-with-named-`C_*`-codes (every error in v2-gap-catalog has the `C_*` code + JSON pointer + sample input). (b) For each gap row, the table cell "Resolves via" points to a §5 subsection. (c) `dssSchemaVersion: 2` accepted by v2 loader; same config with `dssSchemaVersion: 1` emits `C_UnknownField` on every v2-only field. (d) `toy.lang.json` still loads. (e) PR0 also touches v1 §9 to strike items now in flight. |
| **PR1** | ⏳ pending | `v2-precedence` | Operator precedence + associativity + arity | **3–5** | `grammar_schema.{hpp,cpp}`, `grammar_schema_json.cpp`, `parse_diagnostic.{hpp,cpp}` (new code `C_InvalidPrecedenceTable`), new `operator_table.{hpp,cpp}`, `tests/core/test_operator_table.cpp` | `EXPECT_EQ(schema->operatorTable().lookup(plus, Infix)->precedence, 50);` and similar for assoc, arity, prefix-vs-infix distinct entries. `toy.lang.json` reloads. Pratt parse-tree assertions deferred until parser exists. |
| **PR2a** | ⏳ pending | `v2-schema-walker` | Real `SchemaCursor::advance()` / `enterRule()` / `leaveRule()` / `expectedSet()` | **4–6** | `grammar_schema.{hpp,cpp}`, `grammar_schema_json.cpp`, `schema_cursor.hpp`, `tests/core/test_schema_cursor.cpp` | Cursor advances through `sequence` / `alt` / `optional` / `repeat`. `expectedSet()` returns the FIRST set at any cursor position. Cursor descends into nested rules and pops correctly. Walker handles cycles (rule A references rule B references rule A indirectly through `repeat`). Closes v1 §0 deviation #7. |
| **PR2b** | ⏳ pending | `v2-contextual-keywords` | Soft keywords + reserved-word policy on top of the walker | **2–3** | `grammar_schema.{hpp,cpp}`, `grammar_schema_json.cpp`, `tree_builder.{hpp,cpp}`, `parse_diagnostic.{hpp,cpp}` (new code `P_ContextualKeywordResolution`), `tests/core/test_contextual_keywords.cpp` | `int await = 0;` with `await` contextual: `t.kind(leaf2) == NodeKind::Token && t.tokenKind(leaf2) == identifierId`. `CREATE TABLE Select(x int)` under `reservedWordPolicy: contextual`: same expectation for `Select`. `toy.lang.json` reloads. |
| **PR3** | ⏳ pending | `v2-scope-patterns` | `scopeRequire` object (anyOf/forbid/topMustBe/outermost) | **2** | `grammar_schema.{hpp,cpp}`, `grammar_schema_json.cpp`, `tree_builder.cpp`, `tests/core/test_scope_require.cpp` | v1 flat `validScopes: [...]` migrates to `scopeRequire.anyOf` (verified by test). New constraints reject tokens correctly: assertions on `pushToken` rejecting `LtOperator` inside `Generic` via `forbid`, etc. `toy.lang.json` reloads (uses `scopes.validity`, untouched). |
| **PR4** | ⏳ pending | `v2-speculative-alt` | `TreeBuilder::Checkpoint` + speculative alt | **6–9** | `tree_builder.{hpp,cpp}`, `diagnostic_reporter.{hpp,cpp}` (new `Snapshot`+`truncateTo`), `grammar_schema.cpp`, `parse_diagnostic.{hpp,cpp}` (`P_BacktrackFailed`, `P_MaxSpeculationDepth`, `P_UncommittedCheckpoint`), `tests/core/test_speculative_alt.cpp`. **Includes `Frame::children` refactor to `{childIndexStart, childIndexCount}` for O(1) rollback.** | (a) `auto cp = b.checkpoint(); b.pushToken(t); b.rollback(std::move(cp));` — every field listed in §5.4's snapshot table is verified restored via `EXPECT_EQ` to its pre-checkpoint value. (b) Reporter `hitCap_` clears after rollback. (c) Dropped Pending checkpoint emits `P_UncommittedCheckpoint`. (d) Nested checkpoints (3-deep) commit/rollback in stack order correctly. (e) `maxSpeculationDepth = 3` + 4 nested speculatives → exactly 1 `P_MaxSpeculationDepth`. (f) Benchmark: 1000-token speculative source under 50 ms. |
| **PR5** | ⏳ pending | `v2-lexer-modes` | Lexer-mode stack + `modeOp` token field | **3–4** | `grammar_schema.{hpp,cpp}`, `grammar_schema_json.cpp`, new `lexer_mode.{hpp,cpp}`, `parse_diagnostic.{hpp,cpp}` (`C_UnknownLexerMode`), `tests/core/test_string_interpolation.cpp` (stub-driver) | `EXPECT_TRUE(schema->lexerModes().contains("string-body"));`. Stub-driver: nested `pushMode/popMode` sequences leave stack at expected depths. Schema-level cyclic mode references load cleanly (not an error). Tokenizer integration deferred to its phase; PR5 ships schema-side + stub-driver only. |
| **PR6** | ⏳ pending | `v2-custom-strings` | `stringStyle` descriptor for string variants | **2–3** | `grammar_schema.{hpp,cpp}`, `grammar_schema_json.cpp`, new `string_style.hpp`, `parse_diagnostic.{hpp,cpp}` (`C_InvalidStringStyle`), `tests/core/test_string_styles.cpp` (stub-driver) | `EXPECT_EQ(tokenMeaning("@\"").stringStyle.escapeKind, EscapeKind::DoubledDelimiter);` and similar for each of `char`/`doubled-delimiter`/`none`. `tagPattern` defaults to `[A-Za-z0-9_]{0,16}` when omitted. Invalid regex in `tagPattern` emits `C_InvalidStringStyle`. Stub-driver: triple-quote longest-match-from-end resolved correctly. |
| **PR7** | ⏳ pending | `v2-stress-tsql` | Author `tsql-subset.lang.json`; verify v2 features sufficient | **5–10** | `src/source-config/languages/tsql-subset.lang.json`, `.plans/v2-gap-catalog.md` (update) | **Feature-coverage matrix (this is the cap on PR7 scope):** must exercise precedence, contextual keywords (`reservedWordPolicy: contextual`), bracket-quoted identifiers `[name]`, `N'unicode'` strings via stringStyle, three-part naming `db.schema.table`. **Explicitly out of PR7 scope:** batches (`GO`), full system-function namespace (`sys.*`), CLR types. Residual gaps logged in §9 with explicit trigger conditions. |
| **PR8** | ⏳ pending | `v2-plan-doc-maintenance` | Cross-plan updates as v2 lands | **1** | `tree-node-model-plan.md` (§9 items 4/5/11 — already done; verify), `compiler-implementation-plan.md` (top-of-file list — already done; verify), `docs/language-config-spec.md` (will be created in v1 T12 — write the v2 diff for each v2-introduced field) | Each v1 §9 item v2 closes is struck-through with a v2 pointer. Parent plan's "Status & sub-plans" lists v2 as ✅ done once PR7 lands. `docs/language-config-spec.md` has a "v2 additions" section. |

**Critical path:** PR0 → PR1 → PR2a → PR2b → PR4 → PR7 → PR8. (PR3, PR5, PR6 are independent landings.)

**Parallelizable design, serialized landing.** PR1/PR2/PR3/PR5/PR6 all edit `grammar_schema.{hpp,cpp}` and `grammar_schema_json.cpp`. Designs can be developed in parallel; merges must be serialized via the critical-path order. The `Files` column shows file overlap explicitly so reviewers can sequence reasonably.

**Total v2 effort:** **~33–51 person-days**. Larger than the rough sketch a year of plan-doc work might have suggested — but realistic given (a) PR2a's deferred-walker scope, (b) PR4's complete Checkpoint surface, (c) PR0/PR7's authoring depth.

---

## 8. Integration with v1 sub-plan and parent plan

### v1 sub-plan amendments (already done — verify in PR8)

The following v1 §9 items have been struck-through and redirected to v2:
- §9 item 4 → v2 §5.1 / §5.2 / §5.4
- §9 item 5 → v2 §5.3
- §9 item 11 → v2 §5.4

The v1 §0 deviation #7 ("CP1 declared but did not implement `Tree::cursor()`...") stays until v1 T6 closes; PR2a closes the related §9 item 3 (deferred `SchemaCursor::advance()`).

### Parent plan amendments (already done — verify in PR8)

The parent plan's top "Status & sub-plans" list has been updated to include this v2 plan with a one-line description.

### Tokenizer phase dependency (parent plan #5)

The tokenizer phase, when authored, must implement:
- Read `GrammarSchema::lexerModes()` and honor mode operations.
- Maintain a *mode stack*, not a single mode.
- Honor `stringStyle` for body scanning, including dynamic `delimiterTag` capture.
- Expose `snapshotMode()` / `restoreMode()` for `TreeBuilder::Checkpoint`.

PR5 and PR6 are *schema-side* deliveries; tokenizer integration is the tokenizer phase's responsibility, with PR5/PR6 as its dependency.

### `docs/language-config-spec.md` (v1 T12)

v1 T12 produces this doc. Every v2 PR adds a "v2 additions" subsection covering the new fields/shapes it introduced. PR8 finalizes.

---

## 9. Open Questions / candidates for v3

Each is tagged with the trigger that would force the work.

1. **Ternary `?:` / mixfix operators.** The `Prefix|Infix|Postfix` enum can't model mixfix. Real languages with rich expression syntax (Dart, C#) need this — trigger is hitting it in language onboarding.
2. **Format specifiers inside interpolation** (`{x:0.00}`, `{x,10:F2}`). v2 treats them as a separate `StringFormatSpec` token; richer DSL inside `{...}` is v3.
3. **Indentation-sensitive grammar** (Python, F#, YAML). None of v2's targets need it.
4. **Macro systems** (Rust `macro_rules!`, C preprocessor, Lisp). Out of v2 scope. Major undertaking when needed.
5. **Inline grammar / parser combinators**. Schema stays declarative in v2; revisit only when forced.
6. **User-defined operator overloading** in the config schema. Out of v2 scope — no target language needs runtime-extensible operator tables.
7. **Per-meaning costs / longest-match speculative.** v2 is first-wins. Real grammars may benefit from longest-match or weighted-cost alts. Defer until profiling speculative parses on real configs.
8. **Schema versioning compatibility window beyond v1↔v2.** When v3 adds breaking changes, decide whether to bump major + reject older, or auto-migrate. Punt until forced.
9. **Tokenizer perf with many lexer modes.** O(modes × tokens) lookup on huge sources may matter; premature concern; defer.
10. **Hot-reload of `.lang.json` for IDE scenarios.** v1 already supports it. v2's modes don't change that — but a file watcher is still not in scope.
11. **Design-pivot workflow refinement.** §4.1 covers the "gap doesn't fit §5" case at a high level. If PR0/PR7 frequently invoke branch (a) "amend §5 in this plan" (more than 2–3 amendments), spawn a v2.5 plan to consolidate.

---

## 10. Language onboarding sequence (post-v2)

Once PR7 is green, the path to `languages-onboarding-plan.md`:

1. **SQLite subset** — smallest target. Subset of standard SQL with minor SQLite-isms (`AUTOINCREMENT`, `WITHOUT ROWID`). Mostly contextual-keyword + string-literal exercises.
2. **T-SQL full** — extends PR7's subset. Stored procedures, batches (`GO`), system functions, full reserved-word table.
3. **Dart** — well-defined operator precedence; heavy string interpolation; soft keywords; raw strings.
4. **C#** — biggest. ~500-page spec. Pattern matching, async/await, generics, nullable reference types, expression-bodied members, partial classes. Likely surfaces v3 gaps.

Onboarding plan is the next document to author — but only after v2 PR7 closes and the schema design is locked.
