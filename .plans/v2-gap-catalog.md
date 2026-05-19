# DSS Code Prime — v2 Gap Catalog

> Empirically discovered gaps from authoring `src/source-config/languages/c-subset.lang.json`
> against the v1 schema. Format defined in [`schema-expressiveness-v2-plan.md`](./schema-expressiveness-v2-plan.md) §4.1.
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
| 1 | ~~`a + b * c` parses left-fold.~~ **Schema-side data landed in PR1**: `c-subset.lang.json` now declares the full C precedence table via `operators.groups`, queryable through `GrammarSchema::operatorTable().lookup(kind, arity)`. **Tree shape is still wrong** because no parser yet consumes the table — the flip happens when a Pratt walker lands in the parser phase. Test `CSubsetEndToEnd.ExpressionWithMixedOpsIsLeftFolded` stays pinned until then; `CSubsetEndToEnd.OperatorTableMatchesCPrecedence` proves the data is in place. | `shapes/expression`, `shapes/binaryOp`, `operators.groups` | none — loader accepts | **§5.1 (PR1) — data ✅**; tree-shape flip awaits parser phase | `1 + 2 * 3;`, `a = b = c;` |
| 2 | `typedef int MyInt; MyInt x;` can't distinguish `MyInt` as a type later — `Identifier` is unconditional in v1. | `keywords` / `Identifier` resolution        | none — loader accepts, parser produces wrong-typed leaves              | §5.2 (PR2b) — contextual keywords + symbol-table side-table | `typedef int MyInt;`<br>`MyInt v;` |
| 3 | `//` line comments and `/* */` block comments cannot be marked `EmptySpace` declaratively. | (omitted from `tokens`)                     | n/a — feature absent from c-subset                                    | §5.5 (PR5) — lexer modes, comment mode | `// hello`<br>`/* multi */`             |
| 4 | String escape sequences (`"\"hi\\n\""`) treated as opaque chars; v1's `StringLiteral` is a built-in primitive with no per-language escape rules. | (relies on built-in `StringLiteral`)        | none — built-in token kind accepts, escapes round-trip as raw         | §5.6 (PR6) — `stringStyle` descriptor  | `"hello\nworld"`                        |
| 5 | Char literal escapes (`'\n'`, `'\\''`) same problem.         | (relies on built-in `CharLiteral`)         | none                                                                  | §5.6 (PR6)                            | `'\n'`                                  |
| 6 | Pointer declarator/dereference `int *p; *p = 5;` — `*` is declared as `StarOp` (binary), no way to mark it postfix-after-typeRef or prefix-on-lvalue. | `tokens/*`, `shapes/binaryOp`, `shapes/operand` | builder eventually rejects: `*` consumed as binary leaves the LHS `int` dangling | §5.1 (PR1) — operator arity (`Prefix`/`Infix`/`Postfix` for `*`) + a typeRef-side change | `int *p;`<br>`*p = 5;`                  |
| 7 | Function calls `f(x, y)` — no postfix-call shape. `f` parses as `Identifier`, then `(` opens a new scope as `ParenOpen` mid-expression. | `shapes/operand`                            | none — loader accepts, but `f` and `(x,y)` are siblings, not a call    | §5.1 (PR1) — postfix arity OR an explicit call shape | `f(1, 2);`                              |
| 8 | Postfix `x++`, `x--` — same as #7, no postfix arity in v1.   | n/a — token omitted                         | n/a                                                                   | §5.1 (PR1) — postfix arity            | `i++;`                                   |
| 9 | ~~`for (init; cond; step) body` omitted from conservative cut.~~ **Added in PR0** as `shapes/forStmt`; init slot supports `varDeclHead` or `expression`. Inherits gap #1's precedence problem inside cond/step. | `shapes/forStmt`, `shapes/varDeclHead` (refactor) | n/a                                                                | **(a) — closed in PR0**                | `for (int i = 0; i < n; i = i + 1) ...` |
| 10 | ~~`do { ... } while (cond);` omitted from conservative cut.~~ **Added in PR0** as `shapes/doStmt`. | `shapes/doStmt`, `keywords/do`              | n/a                                                                   | **(a) — closed in PR0**                | `do { ... } while (x);`                 |
| 11 | `switch / case / default / break` — `case` valid only inside `switch`. Requires `scopeRequire.topMustBe: "Switch"` to reject `case` outside. | (omitted)                                   | n/a                                                                   | §5.3 (PR3) — `scopeRequire.topMustBe` | `switch (x) { case 1: ...; }`           |
| 12 | Arrays `int a[10]; a[0] = 1;` — `BracketOpen` declared but no array-decl-suffix or index-postfix shape. | `shapes/varDecl`, `shapes/operand`          | none                                                                  | §5.1 (PR1) — postfix arity (`[`)     | `int a[10];`<br>`a[0] = 1;`             |
| 13 | ~~`const` qualifier omitted from `typeRef`.~~ **Added in PR0**: `typeRef` is now `(optional ConstKeyword) typeBase (optional ConstKeyword)`. Tolerates both `const int x` and `int const x` (and redundantly `const int const x`, matching real C compilers). | `shapes/typeRef`, `shapes/typeBase`         | n/a                                                                   | **(a) — closed in PR0**                | `const int x = 5;`                      |
| 14 | Float literals — built-in `FloatLiteral` exists but C lexing (`3.14`, `1e10`, `1.0f`) needs longest-match + suffix rules. Not declarative in v1. | (omitted from `shapes/operand`)             | n/a                                                                   | §5.6 (PR6) — numeric-style descriptor (NOT in current v2 §5.6; v3 candidate) | `3.14`, `1e10f`                         |
| 15 | ~~Bitwise operators omitted for brevity.~~ **Added in PR0**: `&`, `|`, `^`, `~`, `<<`, `>>` as `BitAndOp`/`BitOrOp`/`BitXorOp`/`TildeOp`/`ShlOp`/`ShrOp`. `~` is a unary prefix in `operand`; the rest are binary. Inherits gap #1's precedence problem. | `tokens/&` `\|` `^` `~` `<<` `>>`; `shapes/binaryOp`, `shapes/operand` | n/a                                              | **(a) — closed in PR0**                | `x \| 0x80`, `~x`, `a << 2`            |
| 16 | Compound assignment `+= -= *= /=` — single-token multi-char ops; v1 token-key mechanism handles them, just omitted. Will inherit #1's precedence issue. | (omitted)                                   | n/a                                                                   | §5.1 (PR1) — once precedence lands, these slot in as right-assoc | `x += 1;`                               |
| 17 | Ternary `? :` — mixfix. v1 + v2 `Prefix`/`Infix`/`Postfix` enum doesn't model it. | (omitted)                                   | n/a                                                                   | **(c)** §9 item 1 of v2 plan — v3 candidate | `c ? a : b`                             |
| 18 | Multi-char operator longest-match (`==` vs `=`, `&&` vs `&`) — declared as separate `tokens` keys. v1 schema doesn't say how the tokenizer disambiguates; this is a **tokenizer-phase** concern, not a v2 schema concern. Flagged for the tokenizer phase to honor longest-match. | `tokens` keys: `==`, `!=`, `<=`, `>=`, `&&`, `||` | none — schema-side                                                    | tokenizer phase (parent-plan #5)      | `if (a == b) ...`                       |

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
| Tokenizer-phase (parent-plan #5)                             | 3 (comments lexer mode), 18 (longest-match) |

---

## 3. Branch-(a) follow-up — closed in PR0

Per user decision (Q1 below), rows 9, 10, 13, 15 were landed in the same PR0 commit. They're v1-expressible and free to add — see the strike-throughs in the table.

---

## 4. Branch-(b)/(c) candidates surfaced by PR0

| Row | Branch | Trigger to revisit                                                                    |
|-----|--------|---------------------------------------------------------------------------------------|
| 14  | (c)    | When a target language with strict float-lexing requirements lands (Dart, C#).        |
| 17  | (c)    | Already in §9 item 1 of v2 plan.                                                      |

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
- **PR2b..PR8:** ⏳ pending.
