# The schema / .lang.json system + TreeBuilder

## 5. The Schema and `.lang.json` System

See [`docs/language-config-spec.md`](../../../docs/language-config-spec.md) for the authoring guide.

### 5.1 Loading

```cpp
auto loaded = GrammarSchema::loadShipped("toy");    // from src/dss-config/sources/toy.lang.json
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
