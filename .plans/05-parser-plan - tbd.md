# Parser Phase — Sub-Plan

> Opens parent plan #7 (`analysis-syntactic`). The piece that consumes the live `TokenStream` from the tokenizer and drives `TreeBuilder::open` / `pushToken` / `Checkpoint` from the compiled shape graph. Closes `v2-gap-catalog - tbd.md` row 1 end-to-end (operator-table data ✅, tree shape ⏳ until this phase lands).

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🔵 **next.** All predecessors complete (core-types T0–T12, v2 PR0–PR8, SH1–SH4, tokenizer TZ1–TZ3). This is the first phase whose output drives a real semantic analyzer, so the contract with phase #8 has to be clean from PR1. |
| Predecessors  | ✅ T5 `TreeBuilder` with checkpoint/rollback; ✅ T6 `TreeCursor` for downstream consumption; ✅ v2 schema with compiled position graph + `expectedSet(cursor)` + `firstSetOf(rule)` + `isNullable(rule)`; ✅ tokenizer with live `TokenStream` + body-mode handling. |
| Successors    | Master plan phase #8 (`analysis-semantic`) consumes the resulting `Tree` + diagnostics. The `artifactProfile` plan AP3 plumbs the profile into the `CompilationContext` that wraps parser output. |
| Scope         | **Bounded.** PA1: recursive-descent driver from the shape graph. PA2: Pratt walker for operators via `OperatorTable`. PA3: error recovery + diagnostics. PA4: real-world stress with the 3 shipped languages. |

### Entry checklist (must be true before PA1 starts)

- [x] `TreeBuilder::Checkpoint` shipped + tested (PR4).
- [x] `GrammarSchema::advance` / `enterRule` / `leaveRule` / `routeToRuleLeaf` shipped + tested (PR2a + PR2b review).
- [x] `firstSetOf(rule)` / `isNullable(rule)` available for FIRST/FOLLOW disambiguation (PR2a).
- [x] `OperatorTable::lookup(kind, arity)` returns precedence + associativity (PR1).
- [x] `expectedSet(cursor)` returns the set of token kinds valid at the current cursor position (PR2a).
- [x] `TokenStream` consumed by `pushToken` end-to-end (TZ3).
- [x] `bodyDefaultTokenKinds_` cursor-skip in the builder so body-mode tokens don't desync the cursor (TZ3 + TZ3 review-fix r1).
- [ ] **Open**: Does the schema's compiled shape graph carry enough information for a single recursive-descent driver to handle every alt without per-language hand-tuning? PA1's first task is to answer this empirically on toy + c-subset; the c-subset call/array/postfix gaps are where this gets stressed.

---

## 1. Motivation

Master plan §4.6.2 says phase #7 implements `SyntaxAnalyzer` + `RecursiveDescentParser` + `PrattExpressionParser`. The v2 design pivot made that more specific:

1. **The parser is schema-driven, not hand-written per language.** A single driver walks the compiled position graph; per-language behavior comes from the schema's shape tables + operator table + alt FIRST sets.
2. **Speculation is the parser's escape hatch** for alts whose FIRST sets overlap (`speculative: true` in the schema). `TreeBuilder::Checkpoint` is the mechanism; the parser is the consumer.
3. **Error recovery is a first-class concern.** Production compilers must keep parsing past syntax errors and produce useful diagnostics. The current `pushError` machinery is the primitive; the parser layers a recovery strategy on top (panic-mode-to-FOLLOW-set, sync-token, partial subtree retention).
4. **Operator precedence is the missing piece.** `v2-gap-catalog - tbd.md` row 1 says the data shipped in PR1 but the tree shape is still wrong because nothing consumes it. The Pratt walker is that consumer.

---

## 2. Design

### 2.1 Files

```
src/analysis/syntactic/
├── CMakeLists.txt
├── parser.hpp / .cpp                # main schema-driven driver
├── pratt_walker.hpp / .cpp          # operator-table-driven expression parser
└── recovery.hpp / .cpp              # panic-mode + sync-token strategy

tests/analysis/syntactic/
├── CMakeLists.txt
├── test_parser_toy.cpp              # toy-language parser pinning
├── test_parser_c_subset.cpp         # c-subset full grammar
├── test_parser_tsql_subset.cpp      # tsql-subset full grammar
├── test_pratt_walker.cpp            # precedence + associativity unit pins
└── test_recovery.cpp                # broken-source recovery quality
```

End-to-end tests stay in `tests/core/` (the current `test_c_subset.cpp` / `test_tsql_subset.cpp`) and are extended to use the real parser instead of hand-driving `TreeBuilder::open` calls.

### 2.2 Public API (proposed)

```cpp
namespace dss {

struct ParseResult {
    Tree tree;                                  // the CST
    // diagnostics live on the tree (`tree.diagnostics()`) — no separate channel
};

class DSS_EXPORT Parser {
public:
    Parser(std::shared_ptr<SourceBuffer>        src,
           std::shared_ptr<GrammarSchema const> schema,
           TokenStream                          tokens,
           ParserConfig                         config = {}) noexcept;

    // Single-use: consumes the parser. Drives TreeBuilder from the root rule.
    [[nodiscard]] ParseResult parse() &&;
};

struct ParserConfig {
    std::size_t maxRecoveryDistance     = 64;   // tokens to scan ahead before giving up
    std::size_t maxSpeculationDepth     = 8;    // distinct from BuilderConfig::maxSpeculationDepth — bounds the parser's own checkpoint nesting
};

} // namespace dss
```

The parser owns the `TokenStream` (move-in) and constructs an internal `TreeBuilder`. Callers don't see the builder directly — that's an implementation detail and would tempt mixing modes.

### 2.3 PR breakdown

| PR  | Title                                              | Scope                                                                                                                                                                                                                                                                                                                                                                       |
|-----|----------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| PA1 | Schema-driven recursive-descent driver             | Walk the compiled position graph from the root rule. Per slot: `TokenLeaf` → expect-and-advance; `RuleLeaf` → recurse; `AltChoice` → FIRST-set lookup against the next token; `End` → close frame. **Whitespace/comment handling**: drain at every advance, since the schema cursor already skips EmptySpace and body-default kinds. No operator handling — `expr` shape bodies are parked as a TODO that PA2 fills. Toy + c-subset (minus expression precedence) must round-trip.                                                                                                                                                                                                                          |
| PA2 | Pratt walker for expressions                       | `OperatorTable::lookup(kind, arity)` drives precedence climbing. Three arities: `Prefix` (consume operator, recurse with operator's precedence as min-bind), `Infix` (consume operator, recurse with `precedence+1` if left-assoc / `precedence` if right-assoc), `Postfix` (consume operator, no recursion). The walker is invoked when the recursive-descent driver enters an `expr`-kind shape (PR1's `expr` shape kind). **Closes v2-gap-catalog row 1's tree-shape gap** — flips `CSubsetEndToEnd.ExpressionWithMixedOpsIsLeftFolded` to the precedence-correct expected tree. Adds direct unit pins on `tests/analysis/syntactic/test_pratt_walker.cpp` for left/right assoc, prefix+infix interaction, postfix arity (call `f(x)`, index `a[i]`, postincrement `i++`). |
| PA3 | Error recovery + diagnostic quality                | Panic-mode strategy: on unexpected token, consume tokens until one is in the current rule's FOLLOW set OR a synchronizing token (`;`, `}`, `EndOfStatement`-kind). Emit `P_UnexpectedToken` with `expectedSet(cursor)` populated. Speculative alts gain `P_BacktrackFailed` when every branch fails. The recovery strategy is configurable via `ParserConfig`. **Diagnostic UX** — diagnostics carry source-rendered context (line + column + caret), not just byte offset; `tools/render_diagnostic.py` (or a C++ equivalent) renders the clang-like display the user actually sees.                                                                                                                                                                                                                            |
| PA4 | Real-world stress + corpus tests                   | Onboard real programs (not test-shaped snippets) into a `tests/corpus/` tree: a non-trivial c-subset program (~200 LOC), a multi-statement tsql-subset script with real DDL/DML, and the toy compiler self-host equivalent (TBD). Goal: every corpus file parses cleanly OR with a known + pinned diagnostic set. Surfaces the gaps c-subset/tsql-subset have for real input (pointer arithmetic, function calls, array indexing — `v2-gap-catalog - tbd.md` rows 6, 7, 8, 12).                                                                                                                                                                                                                                                                                                |

### 2.4 Speculation strategy

The schema marks `alt` shapes with `speculative: true` + `lookahead: N` (PR4 shipped the field; nothing consumes it yet). The parser:

1. At an `AltChoice` cursor, scan the next `lookahead` tokens.
2. For each branch in order:
   - Take a `TreeBuilder::Checkpoint`.
   - Attempt to drive the branch.
   - If we consume `lookahead` tokens AND no `P_*` error fired AND the cursor remains valid → commit, this branch wins.
   - Otherwise → rollback, try the next branch.
3. If every branch fails → emit `P_BacktrackFailed`, take the recovery path.

Non-speculative alts use direct FIRST-set lookup with no checkpoint — fastest path.

### 2.5 What's NOT in this phase

| Out of scope                                        | Why                                                                                            |
|-----------------------------------------------------|------------------------------------------------------------------------------------------------|
| Symbol tables / type inference                      | Phase #8 (semantic).                                                                           |
| IR generation                                       | Phase #9.                                                                                      |
| Multi-file compilation / imports / modules          | Production-readiness plan — needs a new shape mechanism for "translation unit boundary."        |
| Incremental parsing                                 | Production-readiness plan — IDE story, post-v1.                                                |
| Macro expansion / preprocessor                      | C-subset deliberately omits the preprocessor. If full C99 is ever a target, that's a new phase. |
| LSP integration                                     | Post-v1.                                                                                       |

---

## 3. Acceptance criteria

- [ ] Every test currently in `tests/core/test_*_subset.cpp` that hand-drives `TreeBuilder` flips to driving through `Parser::parse()`. The tree shape is identical (or, for `ExpressionWithMixedOpsIsLeftFolded`, **changes to the precedence-correct shape** — the visible signal that v2-gap-catalog row 1 is closed end-to-end).
- [ ] Toy / c-subset / tsql-subset parse the new `tests/corpus/` programs without unexpected diagnostics.
- [ ] Diagnostic UX renders source context (line + column + caret), not bare byte offsets.
- [ ] Speculation cap is enforced — pathological input doesn't run unbounded checkpoints (`P_MaxSpeculationDepth` from `BuilderConfig` is already wired; PA1 just has to plumb the parser's own per-alt depth budget).
- [ ] Death-test coverage: parser aborts cleanly on null tokens / null schema / bogus checkpoint state, matching the discipline of TreeBuilder's existing death tests.
- [ ] Per-PR review cadence (5-agent review + "Fix everything" round) applies to every PA[1-4] commit.

---

## 4. Open questions

| # | Question | Notes |
|---|----------|-------|
| 1 | When does the parser detect "ran off the end of the token stream mid-rule"? `TreeBuilder::finish()` already synthesizes premature-close diagnostics; does the parser layer add anything? | Default: parser stops at EOF and lets `finish()` synthesize the closes. The downside is that the parser can't emit a more precise "expected X here" diagnostic since it loses control of the cursor at EOF. Decide in PA3. |
| 2 | How does the parser interact with `artifactProfile`-specific shapes (e.g. a `cli` profile requires a `main` rule at the root)? | Per `06-artifact-profile-plan - tbd.md` §6 Q1: NOT a v1 concern. Profile-specific shape requirements are codegen-phase work. |
| 3 | What's the right strategy for c-subset's currently-omitted features (function calls, array indexing, pointer ops — rows 6/7/8/12)? Land them in c-subset during PA4, or defer to a `c-subset-v2.lang.json` follow-on? | Default: land in PA4 since real-world stress demands them. They're "postfix arity" work — operator-table extensions, not new shape kinds. v2-gap-catalog already flags them as PR1-shape resolutions. |
| 4 | Recovery quality benchmark — what's the bar? `clang -Weverything` quality? `tsc` quality? | TBD with the user. Default for v1: "every error produces an actionable message; recovery continues to EOF without cascading more than 3× the original error count." |

---

## 5. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| The schema doesn't carry enough info for a fully-generic driver — some grammar quirk needs per-language code | Medium | High (forces a re-think of the schema-driven bet) | Spike PA1 on toy first; only proceed to c-subset once toy round-trips cleanly. |
| Pratt walker integration is harder than expected because `expr` shape bodies don't compose cleanly with the position graph | Medium | Medium | The PR1 review hardened `expr` body validation. PA2's first task is a focused integration test against a synthetic 3-operator schema before touching c-subset. |
| Recovery quality is "good enough for tests, bad for users" | High | Medium | Treat PA3 as gated by real-world corpus stress (PA4 prereq). A test that says "no diagnostics" isn't proof — a test that takes a deliberately-broken real program and pins the diagnostic set is. |
| Speculation explodes on adversarial input (deeply nested ambiguous alts) | Low | Medium | `BuilderConfig::maxSpeculationDepth` already caps it; parser-side `ParserConfig::maxSpeculationDepth` is the per-alt budget. Death-test both. |

---

## 6. Next steps after this phase

Phase #8 (`analysis-semantic`) starts here. Symbol tables, scope resolution, type checking populate `NodeAttribute<TypeInfo>` / `NodeAttribute<SymbolId>` over the CST this phase produces. The contract is: parser produces a `Tree` + diagnostics; semantic consumes it read-only and writes only to attribute tables. No CST mutation.
