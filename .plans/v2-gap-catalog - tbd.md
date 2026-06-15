# DSS Code Prime — v2 Gap Catalog

> Empirically discovered gaps from authoring `src/dss-config/sources/c-subset.lang.json`
> against the v1 schema. Format defined in [`02-schema-expressiveness-v2-plan - ok.md`](./02-schema-expressiveness-v2-plan - ok.md) §4.1.
>
> Each row = one C language construct the v1 schema can't express cleanly. **"Resolves via"**
> points at the §5 subsection / PR that closes the gap. Branches per §4.1:
>
> - **(a)** small extension fits naturally → amend §5; record a design-diff line in this row's notes.
> - **(b)** large extension out of PR scope → log in §9 of the v2 plan with trigger.
> - **(c)** out of v2 scope entirely → log in §9 with rationale.
>
> PR0's `c-subset.lang.json` was authored **conservatively**: where v1 couldn't express a feature,
> we omitted the construct rather than working around it. This catalog enumerates what we omitted
> so PR1+ has a target list.

---

## 0. Source for every row

`c-subset.lang.json` committed alongside this catalog. To reproduce: `GrammarSchema::loadShipped("c-subset")` — loads cleanly under the v2 loader. Every gap below is "loader accepted, but the resulting grammar is semantically incomplete or wrong-shaped for real C source."

---

## 1. Gaps from PR0 (c-subset authoring)

| # | Symptom                                                      | Source / JSON site                          | Diagnostic                                                            | Resolves via                          | Sample                                  |
|---|--------------------------------------------------------------|---------------------------------------------|-----------------------------------------------------------------------|---------------------------------------|-----------------------------------------|
| 1 | ~~`a + b * c` parses left-fold.~~ **Closed end-to-end in PA2 ✅**: c-subset's `expression` rule body migrated from `sequence:[operand, repeat[sequence:[binaryOp, operand]]]` to `{ "expr": { "atom": "operand" } }`; `binaryOp` rule deleted; `DefaultPrattWalker` produces right-recursive trees wrapping ops in `binaryExpr`/`unaryExpr`/`postfixExpr`. Pinned by `ParserCSubsetSmoke.FunctionBodyExpressionIsPrecedenceCorrect` (Parser::parse on `int main() { a + b * c; }` asserts the precedence-correct nested-binaryExpr shape) + `ParserCSubsetSmoke.ParenGroupingForcesOuterPrecedence` (paren re-entrancy: `(a + b) * c`) + `CSubsetEndToEnd.OperatorTableMatchesCPrecedence` (data correctness). Left-vs-right associativity is encoded in the operator table only — the tree is uniformly right-recursive. *(SUPERSEDED 2026-06-12 — `D-PARSE-INFIX-ASSOCIATIVITY-STRUCTURAL`, registry: that design was a silent miscompile [no downstream consumer ever read the table's assoc]; trees are now STRUCTURALLY associative via wrap-in-place, the walker itself consumes the declared associativity.)* | `shapes/expression` (migrated), `operators.groups` | none — closed | **§5.1 (PR1) — data ✅**; **PA1 driver ✅**; **PA2 walker ✅** | `1 + 2 * 3;`, `a = b = c;` |
| 2 | `typedef int MyInt; MyInt x;` can't distinguish `MyInt` as a type later — `Identifier` is unconditional in v1. **Schema-side mechanism landed in PR2b**: contextual keywords + `expectedSet(cursor)`-driven demotion. The typedef-symbol-table side (`MyInt` resolving to a user-defined type kind based on prior `typedef` declarations) is parser/semantic-pass work; the schema can't express it declaratively. | `keywords` / `Identifier` resolution        | none — loader accepts, parser produces wrong-typed leaves              | **§5.2 (PR2b) — mechanism ✅**; typedef-aware demotion awaits parser + semantic pass | `typedef int MyInt;`<br>`MyInt v;` |
| 3 | ~~`//` line comments and `/* */` block comments cannot be marked `EmptySpace` declaratively.~~ **End-to-end closed in tokenizer plan TZ2** (commit `90c7f75` + review-fix `8612486`): `c-subset.lang.json` declares `line-comment` / `block-comment` modes with `defaultToken: { kind: "CommentChar", flags: ["EmptySpace"] }`; the tokenizer stamps `Token.flags` from the mode's `defaultToken.flags` on every body emission; the builder OR-merges with `meaning.flagsApplied` at `pushToken` time so the resulting AST leaves carry `EmptySpace` and the AST cursor skips them wholesale. Pinned by `Tokenizer.CommentDefaultTokenFlagsAreEmptySpaceForAstCursorSkip` (schema-side) and `Tokenizer.TokenFlags_PropagateToBuilderLeafFlags` (builder-side, using an inline schema where the global meaning carries no flags so the OR-merge depends on `tok.flags` alone). | `c-subset.lang.json` `lexerModes.line-comment` / `block-comment` | n/a — feature shipped | **§5.5 (PR5) — mechanism ✅**; **TZ2 — end-to-end ✅** | `// hello`<br>`/* multi */`             |
| 4 | ~~String escape sequences treated as opaque chars; v1's `StringLiteral` had no per-language escape rules.~~ **Schema-side mechanism shipped in PR6**: `stringStyle` on string-opener token meanings with `escapeKind` (`char`/`doubled-delimiter`/`none`), `escapeChar`, `endsAt`, `endsAtLongestMatch`, `multiline`, `delimiterTag: "matched"` + `tagPattern`. Tokenizer integration is parent plan #5's job. | (relies on built-in `StringLiteral`)        | none — built-in token kind accepts, escapes round-trip as raw         | **§5.6 (PR6) — mechanism ✅**; tokenizer integration pending  | `"hello\nworld"`                        |
| 5 | ~~Char literal escapes (`'\n'`, `'\\''`) same problem.~~ **Schema-side mechanism shipped in PR6** (same `stringStyle` covers char literals — single-quoted delimiters with `escapeKind: "char"`). | (relies on built-in `CharLiteral`)         | none                                                                  | **§5.6 (PR6) — mechanism ✅**            | `'\n'`                                  |
| 6 | ~~Pointer declarator/dereference `int *p; *p = 5;` — `*` is declared as `StarOp` (binary).~~ **Closed by parser-plan PA4.** c-subset added prefix `*` to the operator-table groups; the Pratt walker emits `unaryExpr` for prefix `*p`. The typeRef-side declarator change (`int *p` vs `*p` as expression) still needs symbol-table awareness in phase #8 — but the parser-side dispatch now handles both positions. | `tokens/*`, `shapes/operand` | n/a                                                  | **parser-plan PA4 ✅**                  | `int *p;`<br>`*p = 5;`                  |
| 7 | ~~Function calls `f(x, y)` — no postfix-call shape.~~ **Closed by parser-plan PA4.** c-subset added postfix `(` with `endsAt: ")"` + `bodyRule: argList`; the walker emits a `postfixExpr` wrap containing opener, `argList` body, and closer. `argList` is `optional` so `f()` parses cleanly. | `shapes/argList`, `operators` postfix `(`   | n/a                                                                   | **parser-plan PA4 ✅**                  | `f(1, 2);`<br>`f();`                    |
| 8 | ~~Postfix `x++`, `x--` — no postfix arity in v1.~~ **Closed by parser-plan PA4.** c-subset added `++` / `--` tokens + simple-postfix (`endsAt` absent) operator-table entries; the walker emits `postfixExpr` containing the operand and the operator leaf. | `tokens/++` `tokens/--`, `operators` postfix | n/a                                                                  | **parser-plan PA4 ✅**                  | `i++;`                                   |
| 9 | ~~`for (init; cond; step) body` omitted from conservative cut.~~ **Added in PR0** as `shapes/forStmt`; init slot supports `varDeclHead` or `expression`. Inherits gap #1's precedence problem inside cond/step. | `shapes/forStmt`, `shapes/varDeclHead` (refactor) | n/a                                                                | **(a) — closed in PR0**                | `for (int i = 0; i < n; i = i + 1) ...` |
| 10 | ~~`do { ... } while (cond);` omitted from conservative cut.~~ **Added in PR0** as `shapes/doStmt`. | `shapes/doStmt`, `keywords/do`              | n/a                                                                   | **(a) — closed in PR0**                | `do { ... } while (x);`                 |
| 11 | ~~`switch / case / default / break` — `case` valid only inside `switch`.~~ **Shipped in SH4b** via shape-based positioning, NOT via `scopeRequire`. The original catalog guess at `scopeRequire: { topMustBe: "Switch" }` doesn't fit C's scope mechanics: scopes open via per-token `opensScope` (no token-sequence opens; `switch (` is two tokens), and inside the body `Block` is innermost, not `Switch`. The clean fix is a dedicated `switchStmt` shape with `switchBodyItem = alt[caseLabel, statement]`; `case`/`default` are valid only at positions where `caseLabel` is expected by the schema cursor. The `scopeRequire` mechanism (PR3) remains exercised by tsql-subset's loader-stress tests. | `c-subset.lang.json`: `switchStmt`, `switchBodyItem`, `caseLabel`, `breakStmt`; keywords `switch`/`case`/`default`/`break`; token `:`. | n/a | **SH4b ✅** — shape-based, not `scopeRequire` | `switch (x) { case 1: break; default: break; }` |
| 12 | ~~Arrays `int a[10]; a[0] = 1;` — no index-postfix shape.~~ **Fully closed.** Expression-side `a[0]` and `a[i+j*k]` closed in PA4 (postfix `[` with `endsAt: "]"` + `bodyRule: expression` routed through Pratt walker). Declarator-side `int a[10];` closed in PA5a-prep — c-subset added an `arrayDeclSuffix` shape (`[` optional-expression `]`) optionally attached to `varDeclTail` and `varDeclHead`. Pinned by `ParserCSubsetSmoke.{TopLevelArrayDeclParses, InnerArrayDeclParses, ArrayDeclWithInitializerExpressionParses}`. | `operators` postfix `[` (PA4) + `shapes/arrayDeclSuffix` (PA5a-prep) | n/a | **parser-plan PA4 ✅** (expression side); **PA5a-prep ✅** (declarator side) | `a[0]`, `a[i + j * k]`, `int a[10];`, `int buf[n * 2];` |
| 13 | ~~`const` qualifier omitted from `typeRef`.~~ **Added in PR0**: `typeRef` is now `(optional ConstKeyword) typeBase (optional ConstKeyword)`. Tolerates both `const int x` and `int const x` (and redundantly `const int const x`, matching real C compilers). | `shapes/typeRef`, `shapes/typeBase`         | n/a                                                                   | **(a) — closed in PR0**                | `const int x = 5;`                      |
| 14 | ✅ **Closed by 08.55:** `numberStyle` shipped (schema v4, additive). Per-language declarative numeric syntax (decimal/integerPrefixes/exponent/fractionPoint/digitSeparator/suffixes/emitKind); tokenizer `scanNumber()` reads it; zero hardcoded numeric paradigm in engine. Float literals — built-in `FloatLiteral` exists but C lexing (`3.14`, `1e10`, `1.0f`) needs longest-match + suffix rules. Not declarative in v1. | (omitted from `shapes/operand`)             | n/a                                                                   | §5.6 (PR6) — numeric-style descriptor (NOT in current v2 §5.6; v3 candidate) | `3.14`, `1e10f`                         |
| 15 | ~~Bitwise operators omitted for brevity.~~ **Added in PR0**: `&`, `|`, `^`, `~`, `<<`, `>>` as `BitAndOp`/`BitOrOp`/`BitXorOp`/`TildeOp`/`ShlOp`/`ShrOp`. `~` is a unary prefix in `operand`; the rest are binary. Inherits gap #1's precedence problem. | `tokens/&` `\|` `^` `~` `<<` `>>`; `shapes/binaryOp`, `shapes/operand` | n/a                                              | **(a) — closed in PR0**                | `x \| 0x80`, `~x`, `a << 2`            |
| 16 | ~~Compound assignment `+= -= *= /=`.~~ **Closed by PA5a-prep.** c-subset declared `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=` tokens + added them to the precedence-15 right-assoc operator group alongside `=`. Pinned by `ParserCSubsetSmoke.{CompoundAssignmentParsesAsBinaryExpr, ShlCompoundAssignmentRespectsLongestMatch, CompoundAssignmentIsRightAssociative}` (covers `+=`, `<<=` longest-match boundary, and the right-assoc shape `x += y += z`). | `tokens` keys + `operators.groups` | n/a | **PA5a-prep ✅** | `x += 1;`, `flags \|= mask;`, `x <<= 1;` |
| 17 | ~~Ternary `? :` — mixfix. v1 + v2 `Prefix`/`Infix`/`Postfix` enum doesn't model it.~~ **Closed 2026-05-28 (gap-closure G6).** Added a real `OperatorArity::Ternary` (4th arity, fits the 2-bit key-pack) + `OperatorTable::Entry.ternaryMiddle` (the `:` token) + an OPTIONAL `expr.wrapperRules.ternary`; a `DefaultPrattWalker` mixfix branch (rollback-replay like infix; condition at prec+1, middle at minPrec 0, else at prec → right-assoc) *(the rollback-replay mechanism was SUPERSEDED 2026-06-12 by wrap-in-place — `D-PARSE-INFIX-ASSOCIATIVITY-STRUCTURAL`, registry; middle/else minPrec behavior unchanged)*; `lowerTernary` → the existing HIR `Ternary` node. No parser special-casing of `?:`. | `operators` `arity:ternary` + `middle`; `wrapperRules.ternary`; `tokens/?` | n/a | **gap-closure G6 ✅** (commit 47c1a53) | `c ? a : b`, `a?b:c?d:e` (right-assoc) |
| 18 | ~~Multi-char operator longest-match (`==` vs `=`, `&&` vs `&`).~~ **Closed in tokenizer phase TZ1+.** `tokenizer.cpp` honors longest-match across the schema's declared `tokens` keys, bounded by `GrammarSchema::maxLexemeLength()`. | `tokens` keys + `tokenizer.cpp` longestMatch | n/a | **tokenizer phase ✅** | `if (a == b) ...`, `x \|\| y`, `<<=` |
| 19 | ~~**Body-lexed literal VALUES (char/string) cannot be captured as a CST subtree.**~~ **Closed 2026-05-28 (gap-closure G1 / tokenizer TZ4).** Was: a body mode emitted its `defaultToken.kind` per byte, off-grammar by design (`C_BodyDefaultKindInShape`), so the body spilled into the enclosing frame. **Fix:** a per-mode `DefaultTokenSpec.coalesce` flag — the body mode emits ONE in-grammar token spanning the body (reusing all its escape/endsAt/unterminated logic), excluded from `bodyDefaultTokenKinds` so `operand` can reference it (`charLiteralExpr`/`stringLiteralExpr`). Per-codepoint comment/EmptySpace bodies are unchanged (`coalesce` stays false). Lowering decodes via `core/types/char_decode.hpp`: char→Char, string→`Array<Char,N+1>`. | `lexerModes.*.defaultToken.coalesce`, `shapes/operand`, tokenizer coalesce branch | n/a — feature shipped | **gap-closure G1 / TZ4 ✅** (commit f08f72d) | `'a'`, `'\n'`, `"hello"` |

---

## 2. Gap-to-PR roll-up

| Resolves via                                                 | Rows                          |
|--------------------------------------------------------------|-------------------------------|
| §5.1 (PR1) — operator precedence + associativity + arity     | 1, 6, 7, 8, 12, 16            |
| §5.2 (PR2b) — contextual keywords + reservedWordPolicy       | 2                             |
| §5.3 (PR3) — `scopeRequire` (anyOf/forbid/topMustBe/outermost) | 11                          |
| §5.5 (PR5) — lexer-mode stack                                | 3                             |
| §5.6 (PR6) — `stringStyle` descriptor                        | 4, 5                          |
| Branch (a) — closed in PR0 (v1-expressible, added inline)    | 9, 10, 13, 15                 |
| Branch (c) — v3 candidate (§9 of v2 plan)                    | 14, 17                        |
| Tokenizer-phase (parent-plan #5)                             | ~~3 (comments lexer mode) — closed in TZ2~~, 18 (longest-match) |

---

## 3. Branch-(a) follow-up — closed in PR0

Per user decision (Q1 below), rows 9, 10, 13, 15 were landed in the same PR0 commit. They're v1-expressible and free to add — see the strike-throughs in the table.

---

## 4. Branch-(b)/(c) candidates surfaced by PR0

| Row | Branch | Trigger to revisit                                                                    |
|-----|--------|---------------------------------------------------------------------------------------|
| 14  | (c)    | When a target language with strict float-lexing requirements lands (Dart, C#).        |
| ~~17~~ | (c) | ✅ **closed 2026-05-28 (gap-closure G6)** — ternary shipped as a real mixfix arity.    |

---

## 5. Resolved open questions

- **Q1. Branch-(a) rows (9, 10, 13, 15) — landed in PR0** as a small extension on top of the conservative cut.
- **Q2. Hand-tokenized end-to-end test — landed in PR0** as `tests/core/test_c_subset.cpp`. Three tests:
  - `TopLevelVarDeclWithIntInitializer` — happy path through `varDecl` → `varDeclHead` → `typeRef` → `typeBase`.
  - `IfWithReturnInsideBlock` — happy path through `ifStmt` → `block` → `returnStmt`.
  - `ExpressionWithMixedOpsIsCurrentlyLeftFolded` — pins the current (wrong) left-fold shape for `a + b * c;`. Flipping the assertion when PR1 lands precedence is the visible signal that gap #1 is closed.
- **Q3. `dssSchemaVersion` — stays at `1`** in `c-subset.lang.json` (no v2-only fields used yet). PR1+ bumps to `2` when v2 fields appear.

---

## 6. Status

- **PR0:** ✅ shipped (commits `4aef654` + `3aca464`).
- **PR1:** ✅ shipped (commits `068b633` + `048953f`). OperatorTable + loader `operators` section + `expr` shape kind + c-subset operators (bumped to `dssSchemaVersion: 2`). Row 1 marked data-resolved; tree-shape flip awaits parser phase. Review followup hardened the table (key-packing fatal guard, duplicate-detect, shape-kind mutual exclusion, `expr` body allowlist) — see plan §0 "PR1 contracts established for downstream PRs".
- **PR2a:** ✅ shipped (`b571cf3` + review followup). Real `SchemaCursor` walker — compiled position tables, fixed-point FIRST/NULLABLE, per-position `nullableTail`, `expectedAt` dropped. Review followup added: Position-as-class with factories, ambiguity detection via `C_AmbiguousAlternatives`, empty-array / zero-shape-kind body errors, iteration caps on fixed points, c-subset refactored to disambiguate `topLevel`. Closes v1 §0 deviation #7. See plan §0 "PR2a deviations" + "PR2a contracts established for downstream PRs".
- **PR2b:** ✅ shipped + review-fixed (`03c6f22` + `9765791`). `LexemeMeaning.contextual` + `ReservedWordPolicy`. `TreeBuilder` walks `SchemaCursor` on open/close/pushToken; contextual keywords consult expectedSet(cursor) and demote when not expected. Row 2 (typedef contextual identifier) closer to resolved — the mechanism exists; the typedef-symbol-table side is parser work. Review followup added `P_SchemaCursorDesync` (one-shot info diagnostic on the first valid→invalid cursor transition), `GrammarSchema::routeToRuleLeaf` (AltChoice → RuleLeaf routing on `open()` so `leaveRule` stays valid for `repeat`/`optional`/`alt` parents), EmptySpace tokens skip cursor advance, and loader rejection of `kind:"Identifier" + contextual:true`. See plan §0 "PR2b deviations".
- **PR3:** ✅ shipped + review-fixed (`901ad62` + `55db3df`). `ScopeMatch { anyOf, forbid, topMustBe, outermost }` replacing v1's flat `validScopes`. Legacy syntax still loads. New `C_ConflictingField` (dual-field, wrong-type) and `C_UnknownScopeName` (replaces overloaded `C_UnclosableScope`). `C_RedundantScopeRequire` warnings on contradictions / oversized lists. Row 11's c-subset adoption was ultimately implemented in **SH4b** via shape-based positioning rather than `scopeRequire` — the PR3 mechanism remains the right tool for context-sensitive token kinds (used by tsql-subset PR7) but isn't a fit for C's `switch`/`case` shape (Block-not-Switch is innermost inside a switch body). See row 11 + SH4b in `03-substrate-hardening-plan - ok.md`.
- **PR4:** ✅ shipped + review-fixed (`9b1ca2c` + `40d8576`). `TreeBuilder::Checkpoint` (move-only RAII, rolls-back-by-default) snapshots arena + child-index + pendingChildren + open frames + scopes + cursor + cookies + reporter. `Frame::children` refactored to shared `pendingChildren_` staging vector. Loader plumbs `speculative: true` + `lookahead: N` on alt shapes; speculative alts exempt from ambiguity-detect. New codes: `P_MaxSpeculationDepth`, `P_UncommittedCheckpoint`, `P_BacktrackFailed`, `C_RedundantField`. Three real bugs fixed in review (recent_ deque restore, inner-commit-outer-rollback firstId math, no-op-guard dtor warning) + 16 test gaps closed.
- **PR5:** ✅ shipped + 2 review rounds (`10641af` + `0507a7c` + `d53d627`). `lexer_mode.{hpp,cpp}` ships `ModeOp` enum, `LexerMode` POD, `LexerModeStack` runtime with fatal-on-empty discipline + lenient opt-ins (`tryPop()`/`topOrInvalid()`) + `clear()`. Snapshot owner-stamp uses per-instance monotonic id (defeats address-recycling). `LexemeMeaning` gains `modeOp` + `modeArg`. Loader registers modes in Pass 1 (forward-ref resolution), populates per-mode tokens in Pass 2 ("main" inherits top-level; `tokens: "default"` inherits; inline objects warn). Auto-synthesizes "main" mode for v1 configs. `lexerModes()` returns `subspan(1)` to hide internal sentinel. Keywords reject `modeOp`/`modeArg`. Case-folded duplicate-mode warning. Orphan `defaultToken`-only mode warning. `lookupLexemeInMode` aborts on invalid id with id value in message. Row 3 (line/block comments as `EmptySpace`) schema-side resolved. New code: `C_UnknownLexerMode`.
- **PR6:** ✅ shipped + review-fixed (`283fbb2` + `3285eed`). New `string_style.{hpp,cpp}` with `EscapeKind` enum (`None`/`Char`/`DoubledDelimiter`) and `StringStyle` POD (escapeKind/escapeChar/endsAt/endsAtLongestMatch/multiline/tagPattern — `tagPattern.empty()` IS the dynamic-tag signal). `LexemeMeaning::stringStyleId` is a proper `DSS_STRONG_ID(StringStyleId)` (zero=invalid, dense 1..N with slot-0 sentinel — matches `LexerModeId` convention). New `DSS_STRONG_ID(SchemaId)` stamped onto every owned `LexemeMeaning` so cross-schema lookups (`stringStyle(m)` etc.) abort loudly. Loader fail-fasts on `escapeChar` with non-Char kind, `tagPattern` without `delimiterTag: "matched"`, and now warns on longest-match + 1-char endsAt. Regex compile catches `std::regex_error` THEN any `std::exception` (catches `bad_alloc` etc. instead of letting it propagate). Keywords reject `stringStyle` (`C_ConflictingField`). Rows 4 + 5 (string + char escape sequences) schema-side resolved. New code: `C_InvalidStringStyle`.
- **PR7:** ✅ shipped + review-fixed (`f2ba763` + `0c06d05`). Authored `src/dss-config/sources/tsql-subset.lang.json` exercising the full v2 feature surface: `reservedWordPolicy: "contextual"` (every keyword soft); operator precedence table with infix + prefix `-`; bracket-quoted identifiers via stringStyle (`escapeKind: "doubled-delimiter"`, `endsAt: "]"`); single-quoted strings + N'unicode' strings sharing the doubled-quote escape rule; three lexer modes (`bracket-id`, `single-string`, `unicode-string`) each with a `defaultToken`; three-part naming `db.schema.table` via `qualifiedName` shape; SELECT / INSERT / UPDATE / DELETE / CREATE TABLE statements. 19 tests in `test_tsql_subset.cpp` covering loader pins for every v2 mechanism + end-to-end TreeBuilder parses for every statement shape + a contextual-keyword demotion test (`CREATE TABLE SELECT ...` demotes SELECT to Identifier at qualifiedName position). v2's feature set is **empirically sufficient** for a non-trivial real grammar.
- **PR8:** ✅ shipped (cross-plan close-out, doc-only). Parent + v1 sub-plan status flipped to ✅ done; v2 plan §0 marked complete; landing log finalized; `docs/language-config-spec.md` §9 and `docs/tree-model.md` §6 reflect the final v2 surface. **v2 is complete.**
- **Substrate hardening SH1–SH4:** ✅ shipped (`ab0800e`, `ac76408`, `05d5c80`, `e22a728`, `ded11d3`, `130d5b1`, `97db647`). SH2 confirmed the DSS.DevOps@v2 multi-OS matrix + opted into the macOS leg. SH3 closed the cross-tree `NodeId` caveat documented in `tree-model.md` §5.10 — `NodeId` now carries an 8-byte `treeTag` and `NodeAttribute<T>` / `Tree::node_` validate cross-tree access. SH1 shipped `tools/refresh_landing_log.py` for plan-doc hygiene; SH4a wired its `--check` into CI as the `landing-log-check` job. SH4b closed gap-catalog row 11 (shape-based, not `scopeRequire`). SH4c closed §7 row 21 (multi-level AltChoice routing). Tokenizer phase now unblocked.

### Residual T-SQL gaps explicitly out of PR7 scope

These are real T-SQL features the subset deliberately omits per the plan §7 PR7 caps. None of them require new v2 schema mechanisms — they're authoring extensions that can be added to `tsql-subset.lang.json` post-v2 without further schema work.

| Feature | Trigger condition |
|---|---|
| `GO` batch separator | When the language onboarding plan targets full T-SQL (currently subset is enough for v2 stress). Implementable as a top-level token that closes the current statement-list and starts a new one. |
| Full `sys.*` system-function namespace | When the onboarding plan needs runtime stored-proc / view introspection. The schema mechanism (three-part naming + Identifier) already supports parsing the references; only the symbol table is missing. |
| CLR types (`Geography`, `HierarchyId`, etc.) | When a target deployment needs them. Implementable as additional `keywords[]` entries with `typeRef` alt arms. |
| Stored-procedure declaration / control flow (`BEGIN ... END`, `IF`, cursors, `TRY/CATCH`) | When onboarding targets server-side T-SQL execution. Mechanism-wise just more shapes; no new schema features needed. |
| `OVER (PARTITION BY ... ORDER BY ...)` window clauses | Same. Shape work, not schema work. |

---

## 7. Gaps surfaced by PR2b review

These are NOT C-language gaps from c-subset authoring — they're internal walker/cursor gaps the PR2b review exposed. Tracking here so the parser phase (which will exercise the same machinery harder) doesn't rediscover them.

| # | Symptom | Site | Resolution |
|---|---------|------|------------|
| 19 | Schema cursor silently went off-track on every parse rooted at `repeat(X)` / `optional(X)` / `alt(...)` — `leaveRule` on a non-RuleLeaf saved cursor returns invalid. Contextual keyword resolution fell back to strict silently, hiding the desync. | `TreeBuilder::open` / `GrammarSchema::leaveRule` | **Closed in PR2b review followup.** New `routeToRuleLeaf(parentCur, rule)` walks AltChoice positions to find a `RuleLeaf(rule)` branch; `open()` saves that RuleLeaf so `leaveRule` resumes correctly. One-shot `P_SchemaCursorDesync` info diagnostic surfaces residual desyncs (e.g. when the caller drives an off-schema sequence). |
| 20 | `EmptySpace` tokens (whitespace) invalidated the cursor — whitespace isn't in any schema expected set, so `advance` returned invalid on every space. | `TreeBuilder::pushToken` | **Closed in PR2b review followup.** `pushToken` skips `schema_->advance` when the resolved meaning carries `NodeFlags::EmptySpace`. Whitespace is off-grammar by design. |
| 21 | Multi-level AltChoice nesting (an `alt` whose chosen branch is itself an `optional`/`alt` containing the target `RuleLeaf`) — `routeToRuleLeaf` recurses through nested AltChoice but is untested past two levels. | `GrammarSchema::routeToRuleLeaf` | **Closed in SH4c** (`03-substrate-hardening-plan - ok.md`). 7 new tests in `tests/core/test_schema_cursor.cpp` (suite `SchemaCursorRouteRuleLeaf`) pin routing through 3-level nesting (`alt → optional → alt → RuleLeaf`) and 4-level nesting (`alt → optional → alt → optional → RuleLeaf`), including the identity base case, the "unreachable rule returns invalid" case, and a sibling-of-outer-alt case to ensure the recursion finds matches without unnecessarily descending. No production-code fix was needed — the recursion handles arbitrary nesting correctly; this is a pin against future regressions. |
| 22 | Demoted-contextual-keyword cursor advance: when a soft keyword demotes to Identifier, the cursor advances by `Identifier`'s schema-token-id (the demoted kind), not the original keyword. PR2b shipped this correctly but the test (`DemotedKeywordAdvancesCursorAsIdentifier`) only landed in review followup — easy to regress without the pin. | `tests/core/test_contextual_keywords.cpp` | **Pinned in PR2b review followup.** Keep the test alive as the parser phase grows. |

---

## 8. Anticipated gaps from v1 production scope

> Added 2026-05-21 when the project committed to v1 production: end-to-end binaries for `toy`/`c-subset`/`tsql-subset` × {Windows, Linux, macOS} × {x86_64, ARM64}. These rows are gaps the parser + semantic + IR + codegen work will surface; logging them in advance so they're not "discovered" mid-PR.

Cross-references the gap-numbering convention in [`07-production-readiness-plan - tbd.md`](./07-production-readiness-plan - tbd.md) (G-001..G-762). Where a gap has a G-number AND a row here, the G-number is authoritative; this catalog adds the "what specifically does it break in the current configs" view.

| #  | Symptom                                                            | Resolves via                                       | G-ref          |
|----|--------------------------------------------------------------------|----------------------------------------------------|----------------|
| 23 | The 3 shipped configs declare no `artifactProfiles`. Loading any of them under `dssSchemaVersion: 3` (the bump that makes the field required) emits `C_MissingField`. | [`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan - tbd.md) AP4 — add the field to every shipped `.lang.json`. Per-language defaults: toy=`["cli"]`, c-subset=`["cli", "lib", "staticlib"]`, tsql-subset=`["script", "sproc"]`. | G-602 |
| 24 | c-subset has no `main` function shape. A `cli` artifactProfile demands an entry point. The schema doesn't currently express "the root rule must contain a function called `main` for this profile." | Codegen-phase validation (per [`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan - tbd.md) §6 Q1, not a schema-phase concern for v1). Emits `D_MissingEntryPoint` at codegen if a `cli` project's tree has no function declared `main`. | G-308, G-501 |
| 25 | c-subset has no struct / union / enum. Real C-subset programs use them heavily. | c-subset shape extension. Mechanism exists (`shapes` + `tokens`); pure authoring work. Likely lands during parser-plan PA4 corpus stress. | G-730 |
| 26 | ~~c-subset has no function calls (row 7) and no array indexing (row 12).~~ **Closed by parser-plan PA4.** Grouped-postfix entries with `endsAt`/`bodyRule` driving call/index parsing; `argList` shape added; pinned by `ParserCSubsetSmoke.{FunctionCallParsesAsPostfix, EmptyArgumentCallParsesAsPostfix, ArrayIndexParsesAsPostfix, ArrayIndexBodyClimbsPrecedence}`. | parser-plan PA4 ✅ | G-103, G-104 |
| 27 | tsql-subset's "what does compile mean?" — T-SQL doesn't produce native binaries. The `script` and `sproc` profiles answer this: `script` emits a `.sql` file; `sproc` emits a deployment package. Neither path goes through the IR/codegen lowering used for `cli`/`lib`. | [`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan - tbd.md) AP3 — `CompilationContext` picks the backend variant; per-language lowering for SQL-shaped profiles is a separate code path that walks the CST and emits text. | G-304, G-501 (alt path) |
| 28 | ✅ **Closed by CU4 imports refactor (commit `aef7490`):** schema v4 `imports` block shipped — per-language import syntax declared in config (strategy `include-following`/`name-matching`/`none`); ONE `ConfigDrivenImportResolver` reads it; per-language `chooseResolver(schema.name())` dispatch retired. No translation-unit / multi-file model. Every test today is one source string → one tree. Real c-subset programs span `.c` + `.h` files; T-SQL scripts span multiple files via `:r` (sqlcmd) or just concatenation. | **New phase concept needed.** "Compilation unit" = list of trees + cross-tree symbol references. Decide before semantic phase PR1. v3 schema bump may be required to declare per-language import syntax. | G-110, G-111 |
| 29 | ~~No `extern` declaration for external functions in c-subset.~~ **Closed by PA5a-prep.** c-subset added `extern` keyword + `externDecl` shape with `externTail = alt[funcParamsOnly, varDeclTail]`. Handles both `extern int printf(int x);` and `extern int errno;`. Pinned by `ParserCSubsetSmoke.{ExternFunctionPrototypeParses, ExternVariableDeclParses}`. Multi-file `#include` / module system is still separate — tracked by row 28 (compilation-unit-plan). | `keywords/extern`, `shapes/externDecl`, `shapes/externTail` | n/a | **PA5a-prep ✅** | `extern int printf(int x);`, `extern int errno;` |
| 30 | Numeric literal lexing rules differ per language. C-subset wants `3.14f`/`1e10`/`0x10`/`0b1`. T-SQL wants `1.5`/`1.5e2`/`0x10`/`$10.50` (money literal). Today the tokenizer hand-codes C-style float lexing as universal. | **v3 schema candidate** — `numericStyle` descriptor per language (analog of `stringStyle`). Tracking as row 14 above + G-109. Not v1-blocking — c-subset's current lexing is close enough. | G-109 |
| 31 | Identifier byte semantics. Today the tokenizer treats id-start/id-continue as ASCII + UTF-8-byte-passthrough. Production C-subset needs `_`/`a-zA-Z` and (per C99) Unicode identifier characters via `\u` escapes. T-SQL wants `@`-prefix for variables, `#`/`##` for temp tables, `[bracket-quoted]` (already handled). | **v3 schema candidate** — `identifierClass` descriptor per language, or escape hatch in `lexerModes` for custom id-byte handlers. Logging now so it's not a surprise at parser-plan PA4. | G-109-adj |
| 32 | Diagnostic UX gap. Today diagnostics carry `SourceSpan` + a code + an `actual` string. They DON'T carry rendered source context (line + col + caret) — the rendering is on the consumer (`prettyPrint` in tests, nothing for the driver). Production usability requires clang-style output. | Driver-level diagnostic-rendering layer. Substrate is already there in `tree.diagnostics()`; needs a new `program/diagnostic_renderer.cpp` that walks diagnostics + `SourceBuffer` and produces formatted output. | G-115, G-608 |
| 33 | Schema-driven parser's escape hatches. The schema-driven bet is that grammars are mostly expressible declaratively. **Some won't be.** Already-known examples: Python-style indentation (off-side rule), C preprocessor (token-stream-level macro expansion), JS automatic semicolon insertion. None apply to the 3 v1 shipped languages, but the architectural decision of *where* to add the escape hatch (custom lexer hook? custom parser plugin? new schema field?) should be made before a fourth language exposes the limit. | **Strategic v3+ decision** — defer until a real language requires it, but track here so it's not surprising. Likely answer: per-language tokenizer subclass with a `TokenizerExtension` hook. | (no G-ref yet; post-v1) |

---

## 8b. Gaps now owned by post-v1 sub-plans (added 2026-05-23)

Per the universal-compiler decisions in [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1 (rev 2), several formerly-deferred gaps now have explicit homes in new sub-plans. Listing here so future PA-style empirical surveys don't re-discover them as fresh gaps.

| Capability | Plan owner |
|---|---|
| Source-to-source transpilation (language-pair `.map.json`) | [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md) |
| GPU / shader codegen (SPIR-V) + same-source CPU+GPU functions | [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md) |
| WASM backend | [`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md) |
| VHDL / Verilog hardware backend (HIR-HW sibling IR) | [`19-hir-hw-reserved-plan`](./19-hir-hw-reserved-plan%20-%20tbd.md) |
| FFI binary readers + C-header importer | [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md) |
| Apple codesign + notarization + Authenticode + APK v3 | [`16-codesign-publish-plan`](./16-codesign-publish-plan%20-%20tbd.md) |
| DWARF / PDB debug-info emission | [`15-debug-info-plan`](./15-debug-info-plan%20-%20tbd.md) |
| In-tree assembler (x86_64 + ARM64) | [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) |
| In-tree linker (ELF / PE / Mach-O / WASM / SPIR-V) | [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) |
| HIR / MIR / LIR layered IR | [`09-hir-plan`](./09-hir-plan%20-%20ok.md) + [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md) |
| Core type lattice + per-language extension registry | [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20ok.md) |
| Multi-language compilation units | [`08-compilation-unit-plan`](./08-compilation-unit-plan%20-%20tbd.md) CU5 |
| Custom user-authored language (eventual) | [`20-custom-language-reserved-plan`](./20-custom-language-reserved-plan%20-%20tbd.md) |
| Language runtime (GC / exceptions / coroutines) | [`21-runtime-reserved-plan`](./21-runtime-reserved-plan%20-%20tbd.md) |

These no longer count as v2 "gap" candidates — they're full architectural commitments tracked in their own plans.

---

## 9. Sequencing notes for parser phase

These are the gaps in §1/§8 that **parser-plan PA4** (real-world corpus stress) will hit first. Listing here so PA4 doesn't get blocked on each one as a surprise.

| Order | Gap to address                              | Resolution                                       |
|-------|---------------------------------------------|--------------------------------------------------|
| 1     | ~~Row 1 (precedence tree shape)~~           | **Closed by parser-plan PA2.**                   |
| 2     | ~~Row 6 (`*` prefix-on-lvalue)~~            | **Closed by parser-plan PA4** (expression side; declarator side awaits semantic).            |
| 3     | ~~Row 7 (function calls `f(x,y)`)~~         | **Closed by parser-plan PA4** (postfix `(` with `argList` bodyRule).                         |
| 4     | ~~Row 12 (array indexing `a[0]`)~~          | **Closed by parser-plan PA4** (postfix `[` with `expression` bodyRule + Pratt routing).      |
| 5     | ~~Row 8 (postfix `x++` `x--`)~~             | **Closed by parser-plan PA4** (simple-postfix `++`/`--`).                                    |
| 6     | ~~Row 16 (compound assignment `+= -= *= /=`)~~ | **Closed by PA5a-prep.**                       |
| 7     | ~~Row 29 (extern decl in c-subset)~~        | **Closed by PA5a-prep.**                         |
| 8     | Row 25 (struct/union/enum in c-subset)      | Deferred to a focused c-subset shape PR (post parser-phase). Bigger scope than PA5a-prep wants. |
| 9     | Row 28 (multi-file translation units)       | **Decide before semantic phase #8 starts** — design new "compilation unit" concept |
| 10    | Row 17 (ternary `? :`)                      | **Defer to v3** — mixfix not modeled in v1+v2 operator-arity enum |
