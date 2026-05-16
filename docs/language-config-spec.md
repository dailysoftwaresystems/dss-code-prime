# DSS Code Prime — `.lang.json` Specification

> **For new contributors.** Read this if you want to author a `.lang.json` for a new source language. About 20 minutes; the cookbook at the end is a known-good starting point.
>
> Companion to [`tree-model.md`](./tree-model.md). Internal loader implementation lives in `src/core/types/grammar_schema_json.cpp`.

---

## 1. Mental model

A `.lang.json` config defines a single source language: its tokens, keywords, scope rules, and grammar shapes. The loader parses it into a `GrammarSchema` that the `TreeBuilder` uses to validate every token push and to drive lossless recovery.

Two design choices to internalize before writing one:

1. **Tokens are multi-typed.** One lexeme (e.g., `+`, `<`) may resolve to several token kinds depending on scope and priority. The loader doesn't pick — the schema captures all alternatives and the builder picks at parse time using `validScopes` and `priority`.
2. **Shapes describe valid tree structure, not parsing strategy.** Today's `TreeBuilder` validates *within* a frame (lexeme resolution, scope filter, recovery). Sequence-level "is this token valid here?" enforcement lands when the parser does — but the schema declares the shape so the future parser doesn't have to invent it.

A config that loads cleanly produces a `GrammarSchema` you can pass to `TreeBuilder`. A config with errors produces a `std::expected<...>` with a vector of `ConfigDiagnostic` — none of which are fatal at file-author time, but all of which will block parsing.

---

## 2. Top-level structure

```jsonc
{
  "dssSchemaVersion": 1,         // required — loader emits C_VersionMismatch on mismatch

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
| `C_VersionMismatch` | `dssSchemaVersion` missing or not `1`. |
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
| `validScopes` | array of strings | no | Whitelist of scope names where this meaning is valid. Empty/absent means "valid everywhere." |
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

- `dssSchemaVersion: 1` is the current version.
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
| `C_VersionMismatch` | `"dssSchemaVersion": 1` — must be an integer, not a string. |
| `C_UnknownToken: "X"` referenced from `shapes.Y` | `X` isn't declared in `tokens` or `keywords` and isn't a built-in. Check spelling; built-ins are `Identifier`, `IntLiteral`, `FloatLiteral`, `StringLiteral`, `CharLiteral`, `BoolLiteral`, `NullLiteral`. |
| `C_UnknownShape: "X"` referenced from `shapes.Y` | `Y` references a shape name that isn't a key in `shapes`. |
| `C_CircularShape: "X"` | Your shape references itself as the first element of its sequence. Insert a literal token before the recursive reference, or split into two shapes. |
| `C_AmbiguousAlternatives` | Two `alt` arms can start with the same token. Pull the common prefix into a parent sequence or pick distinct sentinels. |
| `C_UnclosableScope: "X"` | Some token has `"opensScope": "X"` but nothing reachable inside X has `"closesScope": true`. |

For parse-time diagnostics (P_*), see [`tree-model.md` §6](./tree-model.md#6-diagnostics--when-something-goes-wrong).
