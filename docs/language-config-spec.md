# DSS Code Prime — `.lang.json` Specification

> **For new contributors.** Read this if you want to author a `.lang.json` for a new source language. About 20 minutes; the cookbook at the end is a known-good starting point.
>
> Companion to [`tree-model.md`](./tree-model.md). Internal loader implementation lives in `src/core/types/grammar_schema_json.cpp`.

---

## 1. Mental model

A `.lang.json` config defines a single source language: its tokens, keywords, scope rules, and grammar shapes. The loader parses it into a `GrammarSchema` that the `TreeBuilder` uses to validate every token push and to drive lossless recovery.

Two design choices to internalize before writing one:

1. **Tokens are multi-typed.** One lexeme (e.g., `+`, `<`) may resolve to several token kinds depending on scope and priority. The loader doesn't pick — the schema captures all alternatives and the builder picks at parse time using the per-meaning `scopeRequire` (v2 §9.4; flat `validScopes: [...]` in v1) and `priority`.
2. **Shapes describe valid tree structure, not parsing strategy.** Today's `TreeBuilder` validates *within* a frame (lexeme resolution, scope filter, recovery). Sequence-level "is this token valid here?" enforcement lands when the parser does — but the schema declares the shape so the future parser doesn't have to invent it.

**v2 additions appear in [§9](#9-v2-additions).** Configs that touch `operators`, `expr` shape, `reservedWordPolicy`, `contextual` keywords, or `scopeRequire` are using v2 features.

A config that loads cleanly produces a `GrammarSchema` you can pass to `TreeBuilder`. A config with errors produces a `std::expected<...>` with a vector of `ConfigDiagnostic` — none of which are fatal at file-author time, but all of which will block parsing.

---

## 2. Top-level structure

```jsonc
{
  "dssSchemaVersion": 1,         // required — accepted range is 1..2; loader emits C_VersionMismatch on values outside that window

  "language": {                  // required — identifies the language
    "name":           "Calc",    // required
    "version":        "0.1.0",   // required
    "fileExtensions": [".calc"]  // required — at least one, leading "."
  },

  "tokens":   { /* see §3 */ },  // required
  "keywords": [ /* see §4 */ ],  // optional — empty array if no reserved words
  "scopes":   { /* see §5 */ },  // optional — only if you need forbid rules
  "shapes":   { /* see §6 */ }   // required — must define at least "root"
}
```

The loader's error catalogue (returned in `ConfigDiagnostic.code`):

| Code | When |
|---|---|
| `C_MalformedJson` | JSON parse failed — bad braces, quotes, etc. |
| `C_VersionMismatch` | `dssSchemaVersion` missing, non-integer, or outside the accepted range (currently `1..2`). |
| `C_InvalidLanguageName` | `language.name` missing, empty, or not a string. |
| `C_MissingField` | Required field absent. |
| `C_UnknownToken` | A `shapes` entry references a token kind that isn't declared in `tokens`, `keywords`, or built-ins. |
| `C_UnknownShape` | A `shapes` entry references a shape name that isn't declared. |
| `C_CircularShape` | A shape reaches itself with no progress (e.g., `a: {sequence: [a]}`). |
| `C_AmbiguousAlternatives` | An `alt` has two alternatives with overlapping FIRST sets. |
| `C_UnclosableScope` | A scope is opened by some token but no token closes it. |

---

## 3. `tokens` — lexeme → list of meanings

Every key is a lexeme (literal text the tokenizer matches). Every value is a list of one or more meanings. Pick a single meaning per lexeme unless a real ambiguity exists in your language.

```jsonc
"tokens": {
  " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
  "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
  "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],

  "//": [{ "kind": "LineComment",  "flags": ["EmptySpace"], "until": "newline" }],
  "/*": [{ "kind": "BlockComment", "flags": ["EmptySpace"], "until": "*/" }],

  "+":  [{ "kind": "PlusOp" }],
  "*":  [{ "kind": "MulOp" }],
  "=":  [{ "kind": "EqOp" }],

  "(":  [{ "kind": "ParenOpen",  "opensScope": "Paren" }],
  ")":  [{ "kind": "ParenClose", "closesScope": true   }],

  ";":  [{ "kind": "End" }],

  "<":  [
    { "kind": "LtOperator",              "priority": 10 },
    { "kind": "GenericDefinitionOpener", "priority": 5, "opensScope": "Generic" }
  ]
}
```

Each meaning object accepts:

| Field | Type | Required | Notes |
|---|---|---|---|
| `kind` | string | **yes** | Token-kind name. Interned into `schema.schemaTokens()`. |
| `flags` | array of strings | no | Currently: `["EmptySpace"]` — flagged tokens are skipped by AST cursors. |
| `priority` | integer | no | Lower wins on multi-meaning lexemes. Equal priorities → first-declared wins + `P_AmbiguousToken` diagnostic at parse time. |
| `validScopes` | array of strings | no | Whitelist of scope names where this meaning is valid. Empty/absent means "valid everywhere." **v2**: superseded by the richer `scopeRequire` object — see [§9.4](#94-scoperequire--per-meaning-scope-stack-constraints-pr3). Legacy syntax still loads. |
| `scopeRequire` | object | no | **v2.** Four-axis constraint with `anyOf` / `forbid` / `topMustBe` / `outermost`. See [§9.4](#94-scoperequire--per-meaning-scope-stack-constraints-pr3). Mutually exclusive with `validScopes`. |
| `contextual` | boolean | no | **v2, keywords only.** Marks a soft keyword that demotes to `Identifier` when not in the cursor's expected set. See [§9.3](#93-reservedwordpolicy--keywordscontextual-pr2b). |
| `opensScope` | string | no | Push this scope name onto the builder's scope stack when this token is consumed. |
| `closesScope` | boolean | no | `true` → pop the current scope on consumption. |
| `until` | string | no | Tokenizer hint for `LineComment` (`"newline"`) and `BlockComment` (closing delimiter). Currently informational; lexer doesn't ship yet. |

### Built-in token kinds

These names are pre-registered by every schema — you can reference them in `shapes` without declaring them in `tokens`:

| Kind | What the lexer produces |
|---|---|
| `Identifier` | A bare word that doesn't match any `keywords[].word`. |
| `IntLiteral`, `FloatLiteral` | Numeric literals. |
| `StringLiteral`, `CharLiteral` | Quoted literals. |
| `BoolLiteral`, `NullLiteral` | Boolean / null literals. |
| `Eof`, `Error` | End-of-input and recovery markers. |

You can shadow a built-in name in `tokens` if you need different flags or scope behavior — your declaration wins.

---

## 4. `keywords` — reserved words

```jsonc
"keywords": [
  { "word": "let",   "kind": "LetKeyword"   },
  { "word": "if",    "kind": "IfKeyword"    },
  { "word": "while", "kind": "WhileKeyword" }
]
```

Same `kind` value can appear once. Each keyword's lexeme (`word`) is matched as a complete word — `letme` is an `Identifier`, not `LetKeyword` + `me`. The loader interns each kind into `schema.schemaTokens()` alongside the `tokens` declarations.

---

## 5. `scopes` — validity rules

Only needed if some token meanings are forbidden inside certain scopes. Today there's one rule type:

```jsonc
"scopes": {
  "validity": [
    { "scope": "Generic", "forbid": ["LtOperator"] }
  ]
}
```

Meaning: when the builder's scope stack has `Generic` on top, the `LtOperator` meaning of `<` is filtered out — only `GenericDefinitionOpener` remains. This is how the toy grammar resolves `<` ambiguity between "less than" and "open generic."

The companion to `forbid` is per-meaning `validScopes` (§3) — they're inverses: `forbid` is blacklist, `validScopes` is whitelist. Use whichever is shorter to write.

---

## 6. `shapes` — rule grammar

Three shape forms compose:

```jsonc
"shapes": {
  "root":      { "sequence": [{ "repeat": "stmt" }] },
  "stmt":      { "alt": ["letDecl", "exprStmt"] },
  "letDecl":   { "sequence": ["LetKeyword", "Identifier", "EqOp", "IntLiteral", "End"] },
  "exprStmt":  { "sequence": ["IntLiteral", "End"] }
}
```

| Form | Shape | Meaning |
|---|---|---|
| `sequence` | `{ "sequence": [a, b, c] }` | Match `a`, then `b`, then `c` in order. |
| `alt` | `{ "alt": [a, b, c] }` | Match the first alternative whose FIRST set includes the upcoming token. No backtracking. |
| `repeat` | `{ "repeat": "name" }` | Match `name` zero-or-more times. Lives inside a `sequence`. |

Each element inside `sequence`/`alt`/`repeat` is a **name** (token kind, keyword kind, or shape name). Names are resolved against, in order: keyword kinds → token kinds → built-in token kinds → shape names. Unresolved → `C_UnknownToken` or `C_UnknownShape`.

**Required shape:** `root`. The loader treats `root` as the entry point. Other shapes are reachable via references from `root` (transitively).

**Constraints the loader checks:**

- `C_CircularShape` — a shape's `sequence` starts by referencing itself with no progress (e.g., `a: { sequence: [a, "End"] }`).
- `C_AmbiguousAlternatives` — two `alt` arms have overlapping FIRST sets. The loader catches this at config-load time so you fix it before deploying, not as a parse-time silent first-match.
- `C_UnclosableScope` — a token has `opensScope: "X"` but no token has `closesScope: true` reachable from inside X.

---

## 7. Cookbook: author a clean `.lang.json`

This is a complete, valid `.lang.json` for a tiny calculator language — copy it to `src/source-config/languages/calc.lang.json`, run any test that calls `GrammarSchema::loadShipped("calc")`, and watch it parse.

```jsonc
{
  "dssSchemaVersion": 1,

  "language": {
    "name":           "Calc",
    "version":        "0.1.0",
    "fileExtensions": [".calc"]
  },

  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],

    "+":  [{ "kind": "PlusOp" }],
    "-":  [{ "kind": "MinusOp" }],
    "=":  [{ "kind": "EqOp" }],
    ";":  [{ "kind": "End" }],

    "(":  [{ "kind": "ParenOpen",  "opensScope": "Paren" }],
    ")":  [{ "kind": "ParenClose", "closesScope": true   }]
  },

  "keywords": [
    { "word": "let", "kind": "LetKeyword" }
  ],

  "shapes": {
    "root":     { "sequence": [{ "repeat": "stmt" }] },
    "stmt":     { "alt":      ["letDecl", "exprStmt"] },
    "letDecl":  { "sequence": ["LetKeyword", "Identifier", "EqOp", "IntLiteral", "End"] },
    "exprStmt": { "sequence": ["IntLiteral", "End"] }
  }
}
```

Loads cleanly because:

- `dssSchemaVersion: 1` is inside the loader's accepted window (`1..2`).
- `language.name`, `version`, `fileExtensions` all present.
- Every shape reference (`stmt`, `letDecl`, `exprStmt`) resolves to a declared shape.
- Every token reference (`LetKeyword`, `Identifier`, `EqOp`, `IntLiteral`, `End`) resolves — keywords + tokens + built-in `Identifier`/`IntLiteral`.
- `letDecl` and `exprStmt` have disjoint FIRST sets (`LetKeyword` vs `IntLiteral`) — no `C_AmbiguousAlternatives`.
- `ParenOpen` opens a `Paren` scope; `ParenClose` closes — no `C_UnclosableScope`. (Even though `Paren` isn't used by any shape yet, the close-pair is consistent so the loader is happy.)

To verify, run any unit test that calls `GrammarSchema::loadShipped("calc")` and asserts `loaded.has_value()`. The shipped `toy.lang.json` is structurally identical — peek at `src/source-config/languages/toy.lang.json` if you want a second template.

---

## 8. Troubleshooting

| Symptom | Likely fix |
|---|---|
| `C_MalformedJson` | Run the file through a JSON validator. Trailing commas, unquoted keys, smart quotes. |
| `C_VersionMismatch` | `"dssSchemaVersion": 1` — must be an integer inside `1..2`, not a string. |
| `C_UnknownToken: "X"` referenced from `shapes.Y` | `X` isn't declared in `tokens` or `keywords` and isn't a built-in. Check spelling; built-ins are `Identifier`, `IntLiteral`, `FloatLiteral`, `StringLiteral`, `CharLiteral`, `BoolLiteral`, `NullLiteral`. |
| `C_UnknownShape: "X"` referenced from `shapes.Y` | `Y` references a shape name that isn't a key in `shapes`. |
| `C_CircularShape: "X"` | Your shape references itself as the first element of its sequence. Insert a literal token before the recursive reference, or split into two shapes. |
| `C_AmbiguousAlternatives` | Two `alt` arms can start with the same token. Pull the common prefix into a parent sequence or pick distinct sentinels. |
| `C_UnclosableScope: "X"` | Some token has `"opensScope": "X"` but nothing reachable inside X has `"closesScope": true`. |

For parse-time diagnostics (P_*), see [`tree-model.md` §6](./tree-model.md#6-diagnostics--when-something-goes-wrong).

---

## 9. v2 additions

These fields and shape kinds were added by the v2 schema-expressiveness work after the spec was first authored. They are **opt-in**: any config that doesn't use them keeps loading exactly as it did under v1, and the same `.lang.json` file may declare `"dssSchemaVersion": 1` until it references a v2-only field. Use `2` once any v2 field appears.

### 9.1 `operators` — precedence groups (PR1)

```jsonc
"operators": {
  "groups": [
    { "precedence": 90, "arity": "Prefix",  "associativity": "Right", "kinds": ["BangOp", "MinusOp"] },
    { "precedence": 70, "arity": "Infix",   "associativity": "Left",  "kinds": ["StarOp", "SlashOp"] },
    { "precedence": 60, "arity": "Infix",   "associativity": "Left",  "kinds": ["PlusOp", "MinusOp"] },
    { "precedence": 10, "arity": "Infix",   "associativity": "Right", "kinds": ["AssignOp"] }
  ]
}
```

Each group declares one or more `kinds` (token-kind names) sharing a precedence + arity + associativity. Multi-meaning lexemes require explicit `kinds` per group — `-` as prefix lives in one group with `arity: Prefix`, `-` as infix lives in another with `arity: Infix`. Lookup at runtime: `schema.operatorTable().lookup(kind, arity)`.

| Field | Required | Notes |
|---|---|---|
| `precedence` | yes | Integer; higher binds tighter. |
| `arity` | no | `"Prefix"`, `"Infix"`, or `"Postfix"`. Defaults to `Infix`. |
| `associativity` | no | `"Left"`, `"Right"`, or `"None"`. Defaults to `None`. |
| `kinds` | yes | Array of token-kind names declared in `tokens`. |

**New diagnostic code:** `C_InvalidPrecedenceTable` — malformed `operators` block, duplicate `(kind, arity)` keys, or non-integer precedence.

### 9.2 `expr` shape kind (PR1)

A new shape body kind for operator-bearing expressions:

```jsonc
"shapes": {
  "expression": { "expr": { "atom": "operand" } }
}
```

The cursor walks `expr` as a reference to the `atom` rule. Operator climbing is the parser's job — `operators.groups` provides the data; the schema layer doesn't fold the tree. The only recognized field inside `expr` is `atom`; any other key emits `C_UnknownShape`.

### 9.3 `reservedWordPolicy` + `keywords[].contextual` (PR2b)

```jsonc
{
  "reservedWordPolicy": "contextual",          // optional; default "strict"
  "keywords": [
    { "word": "if",     "kind": "IfKw" },                          // hard keyword
    { "word": "await",  "kind": "AwaitKw", "contextual": true }    // soft keyword
  ]
}
```

- **`reservedWordPolicy: "strict"`** (default) — every keyword always wins over `Identifier`. Matches v1 behavior; configs without this field get strict.
- **`reservedWordPolicy: "contextual"`** — every keyword is implicitly soft. Useful for SQL-family languages where any keyword may also appear as an identifier (`CREATE TABLE Select(Order int)` parses).
- **`contextual: true` per-entry** — marks one keyword as soft regardless of policy. The keyword's `kind` may NOT be `Identifier` (would be a no-op identity demotion — loader emits `C_MissingField`).

Soft keyword resolution: at `pushToken` time, the builder consults the schema cursor's `expectedSet`. If the soft keyword's kind is in the expected set, it wins; otherwise it demotes to `Identifier` and emits an info-level `P_ContextualKeywordResolution` diagnostic. When the schema cursor goes off-track (first `valid → invalid` transition during a parse), the builder emits a one-shot `P_SchemaCursorDesync` info diagnostic and falls back to strict resolution for the remainder.

### 9.4 `scopeRequire` — per-meaning scope-stack constraints (PR3)

Replaces v1's flat `validScopes: [...]` with a four-axis constraint object on every token-meaning entry:

```jsonc
"tokens": {
  "case": [
    { "kind": "CaseKw",
      "scopeRequire": {
        "anyOf":     ["Block"],          // empty/absent = no anyOf requirement
        "forbid":    ["Generic"],        // none of these may be on the stack
        "topMustBe": "Block",            // innermost scope must equal this
        "outermost": "Root"              // bottom-of-stack scope must equal this
      }
    }
  ]
}
```

The builder applies the four constraints in order `forbid → topMustBe → outermost → anyOf` inside the `pushToken` candidate filter. First failure rejects the meaning. Empty `anyOf` / `forbid` means "no constraint on this axis"; unset `topMustBe` / `outermost` means the same.

**Backward compat.** Legacy flat `"validScopes": ["Block"]` loads as `scopeRequire.anyOf = ["Block"]` with everything else default. A meaning may NOT declare both `validScopes` and `scopeRequire` (loader emits `C_ConflictingField`).

**Diagnostics added in PR3:**

| Code | When |
|---|---|
| `C_ConflictingField` | A meaning declares both `validScopes` and `scopeRequire`, or a `scopeRequire` sub-field has the wrong JSON type. |
| `C_UnknownScopeName` | A scope name (in `anyOf` / `forbid` / `topMustBe` / `outermost` / `opensScope` / `scopes.validity[].scope`) isn't a recognized built-in. Replaces the historical reuse of `C_UnclosableScope` for this case. |
| `C_RedundantScopeRequire` | Warning. Fires on: `topMustBe + anyOf` (anyOf redundant), `topMustBe == outermost` (rule matches only single-scope stacks), `forbid` containing `topMustBe`/`outermost` or intersecting `anyOf` (rule can never match), explicit empty array, or oversize list (>32 entries). |

### 9.5 Updated version window

`dssSchemaVersion` accepts the range `1..2`. v2 configs that use any of the fields above SHOULD set `"dssSchemaVersion": 2`. The loader still accepts `1` for any field combination — version bumping is documentation, not enforcement.

### 9.6 Troubleshooting (v2 codes)

| Symptom | Likely fix |
|---|---|
| `C_InvalidPrecedenceTable` | Duplicate `(kind, arity)` declared twice, or `precedence` not an integer. |
| `C_ConflictingField` on `scopeRequire` | Meaning declares both `validScopes` AND `scopeRequire`, or a sub-field is the wrong type (e.g., `anyOf` as a string). |
| `C_UnknownScopeName` | The scope name doesn't match any built-in (`None`, `Root`, `Block`, `Paren`, `Bracket`, `Generic`, `String`, `Comment`). |
| `C_RedundantScopeRequire` (warning) | Your `scopeRequire` has a contradiction or redundancy. Read the message — the rule still loads, but probably doesn't do what you intended. |
| `P_ContextualKeywordResolution` (info, at parse) | A soft keyword demoted to `Identifier`. Expected behavior when the cursor's expected set excludes the keyword. |
| `P_SchemaCursorDesync` (info, one-shot) | The schema cursor went off-track. Usually means a caller drove the builder against a sequence the schema doesn't expect. Contextual resolution stays strict from this point. |
