# Parser Phase — Sub-Plan

> Opens parent plan #7 (`analysis-syntactic`). The piece that consumes the live `TokenStream` from the tokenizer and drives `TreeBuilder::open` / `pushToken` / `Checkpoint` from the compiled shape graph. Closes `v2-gap-catalog - tbd.md` row 1 end-to-end (operator-table data ✅, tree shape ⏳ until this phase lands).

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🟢 **PA0 ✅ + PA1 ✅ done; PA2 next.** PA0 (`SchemaWalker` extraction) shipped as `c764c2a`. PA1 (schema-driven iterative RD driver with try-each-branch speculation) shipped + 5-agent review + fix-everything round. Speculation re-architected from "single probe + fall-through" to proper try-each-branch with snapshot/restore per candidate; speculation depth cap + abandon-token guard added. <!-- LANDING-LOG-HASHES: PA1 --><!-- /LANDING-LOG-HASHES --> |
| Predecessors  | ✅ T5 `TreeBuilder` with checkpoint/rollback; ✅ T6 `TreeCursor` for downstream consumption; ✅ v2 schema with compiled position graph + `expectedSet(cursor)` + `firstSetOf(rule)` + `isNullable(rule)`; ✅ tokenizer with live `TokenStream` + body-mode handling; ✅ **PA0 `SchemaWalker` substrate** (parser embeds its own instance, lock-step with builder's). |
| Successors    | **Phase #7.5** ([`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan%20-%20tbd.md)) consumes per-file `Tree` output and bundles into `CompilationUnit` for semantic. The `artifactProfile` plan AP3 plumbs the profile into the `CompilationContext` that wraps parser output. |
| Scope         | **Bounded.** PA0 substrate refactor ✅. PA1: iterative RD driver from the shape graph. PA2: Pratt walker for operators. PA3: error recovery + diagnostic UX. PA4: corpus stress. PA5a/PA5b: LSP scaffolding + semantic-stub method handlers. |

### Entry checklist (must be true before PA1 starts)

- [x] `TreeBuilder::Checkpoint` shipped + tested (PR4).
- [x] `GrammarSchema::advance` / `enterRule` / `leaveRule` / `routeToRuleLeaf` shipped + tested (PR2a + PR2b review).
- [x] `firstSetOf(rule)` / `isNullable(rule)` available for FIRST/FOLLOW disambiguation (PR2a).
- [x] `OperatorTable::lookup(kind, arity)` returns precedence + associativity (PR1).
- [x] `expectedSet(cursor)` returns the set of token kinds valid at the current cursor position (PR2a).
- [x] `TokenStream` consumed by `pushToken` end-to-end (TZ3).
- [x] `bodyDefaultTokenKinds_` cursor-skip in the builder so body-mode tokens don't desync the cursor (TZ3 + TZ3 review-fix r1).
- [x] **`SchemaWalker` substrate shipped** (PA0). Parser embeds its own instance + drives lock-step with builder's; opaque move-only `Snapshot` for speculation; fatal-abort on contract violations (`enterRule(InvalidRule)`, underflow, cross-walker `restore`, throwing desync callback).
- [x] **Closed by PA1**: Does the schema's compiled shape graph carry enough information for a single recursive-descent driver to handle every alt without per-language hand-tuning? **Yes** — empirically verified on toy + c-subset smoke (22 tests green). The driver handles TokenLeaf + RuleLeaf + AltChoice (speculative and non-speculative) + End-slot with no per-language code; the only schema-side extensions PA1 needed were `nullableTail` / `nullableBranch` accessors (added under PA1's substrate diff). The c-subset call/array/postfix gaps land in PA4 corpus stress and are operator-table / shape work, not driver work.

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
| PA0 | ✅ **done** — Substrate refactor: extract `SchemaWalker` | **Pure refactor; no behavior change.** New `src/core/types/schema_walker.{hpp,cpp}` owns the schema cursor + parent-cursor stack + opaque move-only speculation snapshot + one-shot desync latch — the state machine previously embedded in `TreeBuilder`. `TreeBuilder` keeps its existing public surface but delegates cursor work to an embedded `SchemaWalker walker_` member; the parser (PA1) embeds its own. Two consumers, one canonical state machine — agreement enforced by both being driven lock-step. **Contract violations are fatal-abort** (matches project discipline): `enterRule(InvalidRule)`, `leaveRule` stack underflow, `restore` from a snapshot taken by a different walker (schema pointer-identity guard), throwing desync callback. New `tests/core/test_schema_walker.cpp` (21 tests, 3 death). `Snapshot` non-default-constructible + move-only + opaque — `friend SchemaWalker::snapshot()` is the only construction path; `TreeBuilder::CheckpointSnapshot` holds `std::optional<SchemaWalker::Snapshot>` so it can be built field-by-field. `static_assert(!std::is_move_constructible_v<TreeBuilder>)` pins the `[this]`-capture lifetime of the desync emission lambda. 646 cases / 30 suites, 100% pass; pre-PA0 625 pass byte-identical. Substrate-tier 5-agent review + fix-everything round + 5th-reviewer follow-up round (3 confidence-8/10 items addressed: drop unused include, tighter contract on throwing-callback, non-default-constructible Snapshot). <!-- LANDING-LOG-HASHES: PA0 --><!-- /LANDING-LOG-HASHES --> Shipped as `c764c2a` on `feature/parser-phase` (PR #7); will squash-merge into main per the project's existing convention. |
| PA1 | ✅ **done** — Schema-driven recursive-descent driver | Iterative dispatch loop over the compiled position graph: `TokenLeaf` → expect-and-advance; `RuleLeaf` → enter / skip-nullable (gated by `skipWouldAbandonToken`); `AltChoice` (non-speculative) → FIRST-set + `routeToRuleLeaf` scan; `AltChoice` (speculative) → **try-each-branch** with full snapshot/restore (token bookmark + builder checkpoint + walker snapshot + parser frame stack + diag counter, with delta-basis desync detection); `End` → close frame. Speculation depth bounded by `ParserConfig::maxSpeculationDepth` (default 8, ≥1 enforced in ctor); per-probe budget = 16 × declared lookahead. **`SpeculationProbe` RAII refactor deferred to a follow-up** — current `goto fail` works correctly but the load-bearing four-machine restore-on-exit invariant is convention-only; tracked as PA1 tech-debt. Pratt walker is NOT plumbed in PA1 — PA2 adds the public ctor parameter + dispatch invocation. Forward-progress watchdog fatal-aborts on `(cursor, tokPos, depth)` tuple equality across iterations; speculation commit re-baselines the tuple. **Counter-mirror caveat**: `diagsEmitted` mirrors only parser-emitted recovery diagnostics, NOT builder-side `pushToken` paths (P_UnknownToken / P_SchemaCursorDesync / P_AmbiguousToken) — known + accepted gap, those codes signal informational drift, not commit-blocking errors. New files: `src/analysis/syntactic/parser.{hpp,cpp}`, `src/analysis/syntactic/pratt_walker.hpp`. Tests: 12 toy (5 happy + 3 broken + 2 dispatch-pin + 3 trivia + death) + 6 speculation (commit caseA + commit caseB + bogus + cap + no-committable + empty) + 1 c-subset smoke = 19 + 3 death = **22 parser tests**, all pre-existing 29 suites unchanged. Shipped on `feature/tokenizer-plan` after 5-agent review + fix-everything round addressing ~25 items (top: speculation `isDesynced` delta-basis (C1 from code-reviewer), PrattWalker removed from public ctor (C2), watchdog post-commit re-baseline (I5), abandon-token guard for both AltChoice + RuleLeaf nullable-skip paths (I2), null-check in ctor not parse() (type-design 1), `makeDiag` helper collapsing 8 sites (type-design 10), `expectExactCodes` whitelist expanded to catch `P_BuilderInvariant` / `P_AmbiguousToken` / `P_SchemaCursorDesync` / `P_MaxSpeculationDepth` / `P_RecoveryStalled` / `P_UncommittedCheckpoint`). Existing `test_*_subset.cpp` tests stay manual-driven; PA4 flips them. |
| PA2 | Pratt walker for expressions                       | `OperatorTable::lookup(kind, arity)` drives precedence climbing. Three arities: `Prefix` (consume operator, recurse with operator's precedence as min-bind), `Infix` (consume operator, recurse with `precedence+1` if left-assoc / `precedence` if right-assoc), `Postfix` (consume operator, no recursion). The walker is invoked when the recursive-descent driver enters an `expr`-kind shape (PR1's `expr` shape kind). **Closes v2-gap-catalog row 1's tree-shape gap** — flips `CSubsetEndToEnd.ExpressionWithMixedOpsIsLeftFolded` to the precedence-correct expected tree. Adds direct unit pins on `tests/analysis/syntactic/test_pratt_walker.cpp` for left/right assoc, prefix+infix interaction, postfix arity (call `f(x)`, index `a[i]`, postincrement `i++`). |
| PA3 | Error recovery + diagnostic quality                | Panic-mode strategy: on unexpected token, consume tokens until one is in the current rule's FOLLOW set OR a synchronizing token (`;`, `}`, `EndOfStatement`-kind). Emit `P_UnexpectedToken` with `expectedSet(cursor)` populated. Speculative alts gain `P_BacktrackFailed` when every branch fails. The recovery strategy is configurable via `ParserConfig`. **Diagnostic UX** — diagnostics carry source-rendered context (line + column + caret), not just byte offset; `tools/render_diagnostic.py` (or a C++ equivalent) renders the clang-like display the user actually sees.                                                                                                                                                                                                                            |
| PA4 | Real-world stress + corpus tests                   | Onboard real programs (not test-shaped snippets) into a `tests/corpus/` tree: a non-trivial c-subset program (~200 LOC), a multi-statement tsql-subset script with real DDL/DML, and the toy compiler self-host equivalent (TBD). Goal: every corpus file parses cleanly OR with a known + pinned diagnostic set. Surfaces the gaps c-subset/tsql-subset have for real input (pointer arithmetic, function calls, array indexing — `v2-gap-catalog - tbd.md` rows 6, 7, 8, 12).                                                                                                                                                                                                                                                                                                |
| PA5a | LSP server skeleton + diagnostics                 | Standalone language-server binary delivering editor diagnostics from the parser pipeline. Scope: stdio JSON-RPC loop; `initialize` / `initialized` / `shutdown` / `exit` handshake; `textDocument/{didOpen,didChange,didClose,didSave}` lifecycle; `textDocument/publishDiagnostics` translating `P_*` / `C_*` / `D_*` codes to LSP `Diagnostic` with severity + range + code-name + source. Language selection via `GrammarSchema.fileExtensions`. New files: `src/lsp/server.{hpp,cpp}`, `src/lsp/json_rpc.{hpp,cpp}`, `src/lsp/protocol.{hpp,cpp}` (LSP message types), `src/lsp/diagnostic_to_lsp.{hpp,cpp}`. `src/main.cpp` gains a `--lsp` flag wiring stdin/stdout to the server. Tests: focused unit pins on the JSON-RPC framing, diagnostic translator, and a minimal end-to-end test that drives `didOpen` + asserts diagnostics fire. **Acceptance:** real editor (VS Code) shows `P_IllegalChar` squiggles on a toy file. |
| PA5b | LSP semantic-stub method handlers + golden replay | Add empty-result handlers for `textDocument/{hover,completion,definition,references,rename,signatureHelp}` — they return `null` / empty arrays until phase #8 lights them up. Also adds the golden-file session-replay harness at `tests/lsp/sessions/*.jsonl` + the `tests/lsp/test_lsp_replay.cpp` that drives captured editor sessions through the server and byte-compares responses. This is the test pattern future semantic-LSP work will reuse. **Acceptance:** every method registered in `initialize.capabilities` has at least one wire-protocol test even if the response is empty; golden harness green. |

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

### 2.5 Forward-progress watchdog (PA1)

Every iteration of PA1's main dispatch loop captures `(walker.cursor(), tokens.position())`. If neither changes across one iteration, the parser fatal-aborts with a message naming the slot kind, the head token, and the current rule context. Per the project's fail-loud discipline (matching PA0's `enterRule(InvalidRule)` / underflow / cross-walker `restore` and the throwing-callback contract), this is a contract violation — better to halt the test process loudly than hang CI. PA2 and PA3 inherit the invariant; an infinite-loop regression introduced by Pratt-walker or recovery code surfaces as a death-test failure immediately, not as a CI timeout.

### 2.6 What's NOT in this phase

| Out of scope                                        | Why                                                                                            |
|-----------------------------------------------------|------------------------------------------------------------------------------------------------|
| Symbol tables / type inference                      | Phase #8 (semantic).                                                                           |
| IR generation                                       | Phase #9.                                                                                      |
| Multi-file compilation / imports / modules          | **Phase 7.5** ([`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan%20-%20tbd.md)) — sits between parser and semantic. Parser still produces one Tree per file; CU bundles them. |
| Incremental parsing                                 | Post-v1. The current PA5 LSP impl re-parses on every `didChange`. Build caching + incremental landed when memory budget G-721 forces it. |
| Macro expansion / preprocessor                      | C-subset deliberately omits the preprocessor. If full C99 is ever a target, that's a new phase. |
| Semantic-powered LSP capabilities (hover, completion, goto-def, references, rename) | PA5 stubs them as empty-result responders. They light up post-phase #8 in a dedicated LSP follow-up plan. |

---

## 3. Acceptance criteria

- [ ] Every test currently in `tests/core/test_*_subset.cpp` that hand-drives `TreeBuilder` flips to driving through `Parser::parse()`. The tree shape is identical (or, for `ExpressionWithMixedOpsIsLeftFolded`, **changes to the precedence-correct shape** — the visible signal that v2-gap-catalog row 1 is closed end-to-end).
- [ ] Toy / c-subset / tsql-subset parse the new `tests/corpus/` programs without unexpected diagnostics.
- [ ] Diagnostic UX renders source context (line + column + caret), not bare byte offsets.
- [ ] Speculation cap is enforced — pathological input doesn't run unbounded checkpoints (`P_MaxSpeculationDepth` from `BuilderConfig` is already wired; PA1 just has to plumb the parser's own per-alt depth budget).
- [ ] Death-test coverage: parser aborts cleanly on null tokens / null schema / bogus checkpoint state, matching the discipline of TreeBuilder's existing death tests.
- [x] **Forward-progress watchdog** in code (PA1) + termination test for the canonical "every speculative branch matches FIRST but none commits" scenario (`ParserSpeculation.NoCommittableBranchTerminatesCleanly`). The watchdog itself is a safety net for PA2/PA3 regressions: under PA1's invariants, every dispatch path either advances cursor, advances token, or changes frame depth (P_BacktrackFailed + consume on speculation failure, EOF guard mid-rule, etc.), so a natural stall isn't constructible. A true `EXPECT_DEATH` for the watchdog would require fault injection — deferred to PA2/PA3 where their changes might actually risk regression.
- [ ] LSP server (PA5) starts via `dss-code-prime --lsp`, completes the initialize handshake, and publishes diagnostics on `didOpen`/`didChange` for every shipped language. Golden-file session replay green.
- [ ] Per-PR review cadence (5-agent review + "Fix everything" round) applies to every PA[0,1,2,3,4,5a,5b] commit. PA0 + future substrate touch gets full substrate-tier review; PA1–PA4 + PA5a/PA5b are feature-tier.

---

## 4. Open questions

| # | Question | Notes |
|---|----------|-------|
| 1 | When does the parser detect "ran off the end of the token stream mid-rule"? `TreeBuilder::finish()` already synthesizes premature-close diagnostics; does the parser layer add anything? | Default: parser stops at EOF and lets `finish()` synthesize the closes. The downside is that the parser can't emit a more precise "expected X here" diagnostic since it loses control of the cursor at EOF. Decide in PA3. |
| 2 | How does the parser interact with `artifactProfile`-specific shapes (e.g. a `cli` profile requires a `main` rule at the root)? | Per `06-artifact-profile-plan - tbd.md` §6 Q1: NOT a v1 concern. Profile-specific shape requirements are codegen-phase work. |
| 3 | What's the right strategy for c-subset's currently-omitted features (function calls, array indexing, pointer ops — rows 6/7/8/12)? Land them in c-subset during PA4, or defer to a `c-subset-v2.lang.json` follow-on? | Default: land in PA4 since real-world stress demands them. They're "postfix arity" work — operator-table extensions, not new shape kinds. v2-gap-catalog already flags them as PR1-shape resolutions. |
| 4 | Recovery quality benchmark — what's the bar? `clang -Weverything` quality? `tsc` quality? | TBD with the user. Default for v1: "every error produces an actionable message; recovery continues to EOF without cascading more than 3× the original error count." |
| 5 | PA1 carry-over: refactor `Impl::trySpeculativeBranch`'s `goto fail` + manual 4-machine restore into a `SpeculationProbe` RAII guard (mirrors `TreeBuilder::Checkpoint`). Type-design review flagged this as 9/10 confidence "would make the invariant local + structurally enforced". Currently the four-machine snapshot/restore is convention-only across ~15 lines. | Decide before PA2 lands more speculative paths. If PA2 ends up adding a 5th state machine (e.g. Pratt walker stack), the cost of getting the RAII refactor right grows; doing it now is cheap, doing it after PA2 is risky. |

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

**Phase 7.5** (`compilation-unit-model`, [`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan%20-%20tbd.md)) starts here. Parser produces one `Tree` per file; phase 7.5 bundles them into a `CompilationUnit` + resolves cross-file imports declaratively per language. **Phase #8** (`analysis-semantic`) consumes the resulting `CompilationUnit` (not raw trees) so symbol tables / type checking / scope resolution work across files from day one. Contract chain: parser → Tree-per-file → CU bundle → semantic read-only over CU → IR.

**LSP spin-off — planned post-PA5b.** PA5a/PA5b live in this plan because the diagnostic-rendering pipeline is shared substrate with PA3, and shipping that pipeline through a real editor is the strongest forcing function for diagnostic quality (G-115). The scope will grow beyond what `05-parser-plan` can hold the moment semantic-powered methods light up: incremental rebuild, cross-file goto-def, workspace symbols, code actions, semantic-token highlighting, formatting. **At that point, spin off `09-lsp-plan - tbd.md`** owning everything in `src/lsp/`. The split point is "ship PA5a + PA5b, then create the LSP sub-plan and migrate the existing implementation under it." Don't pre-create the sub-plan — let the parser-side work establish the patterns first.
