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

**v2 additions appear in [§9](#9-v2-additions).** Configs that touch `operators`, `expr` shape, `reservedWordPolicy`, `contextual` keywords, `scopeRequire`, `speculative` alts, `lexerModes` / `modeOp`, or `stringStyle` are using v2 features.

A config that loads cleanly produces a `GrammarSchema` you can pass to `TreeBuilder`. A config with errors produces a `std::expected<...>` with a vector of `ConfigDiagnostic` — none of which are fatal at file-author time, but all of which will block parsing.

---

## 2. Top-level structure

```jsonc
{
  "dssSchemaVersion": 1,         // required — accepted range is 1..4; loader emits C_VersionMismatch on values outside that window

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
| `C_VersionMismatch` | `dssSchemaVersion` missing, non-integer, or outside the accepted range (currently `1..4`). |
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
| `IntLiteral`, `FloatLiteral` | Numeric literals. Driven by [`numberStyle`](#114-numberstyle--numeric-literal-grammar). |
| `StringLiteral` | Quoted literals from a delimited-string opener (see `stringStyle`). |
| `Whitespace`, `Newline` | Trivia. |
| `Eof`, `Error` | End-of-input and recovery markers. |

You can shadow a built-in name in `tokens` if you need different flags or scope behavior — your declaration wins.

**Paradigm-specific kinds are NOT pre-interned.** Languages that have `CharLiteral` / `BoolLiteral` / `NullLiteral` as syntactic categories declare them explicitly in `tokens` (or `keywords` for `true`/`false`/`null`-as-keyword). Not every plausible language has those categories — leaving them out of the built-ins forces every config to spell out what it actually uses.

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
  "dssSchemaVersion": 4,

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

  "numberStyle": {
    "decimal":  true,
    "emitKind": { "integer": "IntLiteral" }
  },

  "shapes": {
    "root":     { "sequence": [{ "repeat": "stmt" }] },
    "stmt":     { "alt":      ["letDecl", "exprStmt"] },
    "letDecl":  { "sequence": ["LetKeyword", "Identifier", "EqOp", "IntLiteral", "End"] },
    "exprStmt": { "sequence": ["IntLiteral", "End"] }
  }
}
```

Loads cleanly because:

- `dssSchemaVersion: 4` is inside the loader's accepted window (`1..4`).
- `language.name`, `version`, `fileExtensions` all present.
- Every shape reference (`stmt`, `letDecl`, `exprStmt`) resolves to a declared shape.
- Every token reference (`LetKeyword`, `Identifier`, `EqOp`, `IntLiteral`, `End`) resolves — keywords + tokens + built-in `Identifier`/`IntLiteral`.
- `numberStyle` is required because the grammar references `IntLiteral`. The minimal block declares `decimal: true` (bare digits) and the emit kind.
- `letDecl` and `exprStmt` have disjoint FIRST sets (`LetKeyword` vs `IntLiteral`) — no `C_AmbiguousAlternatives`.
- `ParenOpen` opens a `Paren` scope; `ParenClose` closes — no `C_UnclosableScope`. (Even though `Paren` isn't used by any shape yet, the close-pair is consistent so the loader is happy.)

To verify, run any unit test that calls `GrammarSchema::loadShipped("calc")` and asserts `loaded.has_value()`. The shipped `toy.lang.json` is structurally identical — peek at `src/source-config/languages/toy.lang.json` if you want a second template.

---

## 8. Troubleshooting

| Symptom | Likely fix |
|---|---|
| `C_MalformedJson` | Run the file through a JSON validator. Trailing commas, unquoted keys, smart quotes. |
| `C_VersionMismatch` | `"dssSchemaVersion": 1` — must be an integer inside `1..4`, not a string. |
| `C_UnknownToken: "X"` referenced from `shapes.Y` | `X` isn't declared in `tokens` or `keywords` and isn't a built-in. Check spelling; built-ins are `Identifier`, `IntLiteral`, `FloatLiteral`, `StringLiteral`, `Eof`, `Error`, `Whitespace`, `Newline`. Paradigm-specific kinds (`CharLiteral`, `BoolLiteral`, `NullLiteral`, …) are no longer pre-interned — a language that uses them must declare them in `tokens`/`keywords` or via a `defaultToken` body mode. |
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

### 9.5 Speculative `alt` (PR4)

Standard `alt` is first-match-on-FIRST. When two alternatives share a prefix and the disambiguator lives N tokens deep, mark the alt speculative:

```jsonc
"shapes": {
  "expression": {
    "alt": ["patternMatchExpr", "regularExpr"],
    "speculative": true,
    "lookahead": 6       // optional; default = 8
  }
}
```

The parser (when authored) takes a `TreeBuilder::Checkpoint`, tries the first branch up to `lookahead` tokens, and commits or rolls back. Speculative alts are exempt from the load-time ambiguity check (`C_AmbiguousAlternatives`) — overlapping FIRST sets are the whole point. Malformed `speculative`/`lookahead` emits `C_ConflictingField`; `lookahead` without `speculative: true` emits `C_RedundantField` warning.

### 9.6 Lexer modes (PR5)

Tokenizers for interpolated strings, here-strings, and multi-language embeddings need a mode stack — the lexeme `"` means different things inside a string body vs. in normal grammar. Declare modes at top level:

```jsonc
{
  "lexerModes": {
    "main":        { "tokens": "default" },                    // inherits top-level `tokens`
    "string-body": { "defaultToken": { "kind": "StringChar" } }
  },
  "tokens": {
    "$\"":  [{ "kind": "InterpStart", "modeOp": "pushMode", "modeArg": "string-body" }],
    "\"":   [{ "kind": "StringEnd",   "modeOp": "popMode" }]
  }
}
```

**Per-meaning fields** (any token entry):

| Field | Required | Notes |
|---|---|---|
| `modeOp` | no | `"pushMode"`, `"popMode"`, or `"replaceMode"`. Default = no mode-stack effect. |
| `modeArg` | required for `pushMode`/`replaceMode` | Target mode name. Must be declared in `lexerModes`. |

**Per-mode fields** (entries under `lexerModes`):

| Field | Required | Notes |
|---|---|---|
| `tokens` | no | `"default"` inherits the top-level `tokens` map. An inline `{ ... }` object is a **per-mode override table** parsed with the same meaning semantics as the top-level `tokens` map: while the tokenizer is in this mode the scanner consults this table FIRST and falls back to the top-level `tokens` map for any lexeme the mode doesn't override. So the same source character can lex to a different token kind inside the mode than outside it (context-sensitive lexing). A non-array value for a lexeme key, or a meaning entry missing its `kind`, is a load error (same fail-loud discipline as the top-level map). |
| `defaultToken` | no | `{ "kind": "X" }`. Emitted by the tokenizer when nothing else matches the input. Common for `string-body` modes whose body is mostly free text. |

**`defaultToken.kind` is off-grammar.** A kind declared here is implicitly off-grammar — it represents per-codepoint body content that the schema cursor never expects. The builder skips the cursor-advance for those tokens (string/comment bodies stay visible in the AST but don't drive shape matching). Referencing the same kind from any `shapes/*` rule emits `C_BodyDefaultKindInShape` at load time, because the shape's slot would silently never match. Either pick a distinct kind for the shape slot, or stop using the kind as a `defaultToken`.

**Override-first longest-match (a context-sensitivity footgun).** A per-mode `tokens` override wins at the LONGEST length *it* matches, short-circuiting before the top-level map is consulted. So if a mode overrides a 1-char lexeme (`<`) that is also the prefix of a 2-char *global* lexeme (`<<`), input `<<` inside that mode lexes as TWO override tokens — the global `<<` is never reached. This is the intended semantic: a mode *redefines* what a character means (e.g. an `#include` mode makes `<` open a header path, deliberately suppressing `<<`-as-shift). To keep a longer global lexeme available INSIDE a mode, **re-list it in the mode's override table**. Longest-match still applies *within* the override table.

**Backward compat.** If `lexerModes` is absent the loader synthesizes a `"main"` mode pointing at the top-level `tokens` map. v1 configs continue to load unchanged.

**Keywords cannot carry `modeOp`** — soft-keyword demotion (contextual resolution) and lexer-mode switching are distinct mechanisms. Mixing them on a `keywords[]` entry emits `C_ConflictingField`. Move the entry to `tokens` to switch modes.

**Cyclic mode references are normal**, not an error. `main` pushes `string-body`; `string-body` pushes `main` (for `{...}` interpolation expressions); they recurse arbitrarily.

### 9.7 `stringStyle` descriptor (PR6)

Delimited string literals — `"hello"`, `@"verbatim ""quotes"""`, `R"DELIM(raw)DELIM"`, `"""triple"""` — declare their escape rules and termination as `stringStyle` on the opening-delimiter token's meaning:

```jsonc
"tokens": {
  "\"":     [{ "kind": "StringStart",
                "modeOp": "pushMode", "modeArg": "string-body",
                "stringStyle": { "escapeKind": "char",
                                  "escapeChar": "\\",
                                  "endsAt":     "\"" } }],
  "@\"":    [{ "kind": "VerbatimStringStart",
                "modeOp": "pushMode", "modeArg": "verbatim-body",
                "stringStyle": { "escapeKind": "doubled-delimiter",
                                  "endsAt":     "\"" } }],
  "R\"":    [{ "kind": "RawStringStart",
                "modeOp": "pushMode", "modeArg": "raw-body",
                "stringStyle": { "escapeKind":   "none",
                                  "endsAt":       ")\"",
                                  "delimiterTag": "matched",
                                  "tagPattern":   "[A-Za-z_]{0,16}" } }],
  "\"\"\"":  [{ "kind": "TripleStringStart",
                "modeOp": "pushMode", "modeArg": "triple-body",
                "stringStyle": { "escapeKind":         "char",
                                  "escapeChar":         "\\",
                                  "endsAt":             "\"\"\"",
                                  "endsAtLongestMatch": true,
                                  "multiline":          true } }]
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `escapeKind` | enum string | **yes** | `"char"`, `"doubled-delimiter"`, or `"none"`. |
| `escapeChar` | single ASCII char | only when `escapeKind: "char"` | The lead character for escape pairs (typically `\`). Must be exactly one byte. |
| `endsAt` | non-empty string | **yes** | The literal sequence that terminates the body. |
| `endsAtLongestMatch` | bool | no (default `false`) | For triple-quotes — consume the longest run matching `endsAt` from the end. |
| `delimiterTag` | string `"matched"` | no | Enables dynamically-captured delimiter tags (C++ `R"DELIM(...)DELIM"`). Only `"matched"` is a valid value. |
| `tagPattern` | regex string | no (default `[A-Za-z0-9_]{0,16}` when `delimiterTag` is set) | Constraint on what characters are valid in the captured tag. Compiled at load time — invalid regexes are rejected with `C_InvalidStringStyle`. Must be paired with `delimiterTag: "matched"`. |
| `multiline` | bool | no (default `false`) | Whether newlines are allowed in the body. |

**Keywords cannot carry `stringStyle`** — word-shaped tokens can't open delimited strings. Use a `tokens` entry instead.

**Loader fail-fasts** on these config errors: `escapeChar` set when `escapeKind != "char"`; `tagPattern` set without `delimiterTag: "matched"`; missing/empty `endsAt`; multi-byte `escapeChar`; unknown `escapeKind`; non-`"matched"` `delimiterTag`; wrong-type fields. Cross-field warning: `endsAtLongestMatch: true` with a 1-character `endsAt` (the flag is a no-op for single-char terminators).

### 9.8 Updated version window

`dssSchemaVersion` accepts the range `1..4`. v2 configs that use any of the fields above SHOULD set `"dssSchemaVersion": 2`; v3 configs that use `typeExtensions[]` (§10) SHOULD set `3`; v4 configs that use the `imports` block (§11) SHOULD set `4`. The loader still accepts `1` for any field combination — version bumping is documentation, not enforcement.

### 9.9 Troubleshooting (v2 codes)

| Symptom | Likely fix |
|---|---|
| `C_InvalidPrecedenceTable` | Duplicate `(kind, arity)` declared twice, or `precedence` not an integer. |
| `C_ConflictingField` on `scopeRequire` | Meaning declares both `validScopes` AND `scopeRequire`, or a sub-field is the wrong type (e.g., `anyOf` as a string). |
| `C_ConflictingField` on `modeOp`/`modeArg` | Wrong-type field, unknown `modeOp` string, missing `modeArg` on `pushMode`/`replaceMode`, `modeArg` without `modeOp`, or `modeOp`/`modeArg` placed on a keyword entry instead of a token. |
| `C_ConflictingField` on `speculative`/`lookahead` | Non-boolean `speculative`, non-integer or out-of-range `lookahead`. |
| `C_UnknownScopeName` | The scope name doesn't match any built-in (`None`, `Root`, `Block`, `Paren`, `Bracket`, `Generic`, `String`, `Comment`). |
| `C_UnknownLexerMode` | A `modeArg` references a mode that wasn't declared in `lexerModes`. |
| `C_InvalidStringStyle` | A `stringStyle` block is malformed — see §9.7 (missing/empty `endsAt`; `escapeChar` without `escapeKind: "char"`; `tagPattern` without `delimiterTag: "matched"`; invalid `tagPattern` regex; unknown `escapeKind`; non-`"matched"` `delimiterTag` value; wrong-typed sub-field). |
| `C_BodyDefaultKindInShape` | A `lexerModes.<m>.defaultToken.kind` is also referenced from a `shapes/*` rule. Body-default kinds are off-grammar (the cursor never expects them); a shape reference would silently never match. Pick a distinct kind for the shape slot or stop using the kind as a `defaultToken`. |
| `C_RedundantScopeRequire` (warning) | Your `scopeRequire` has a contradiction or redundancy. The rule still loads. |
| `C_RedundantField` (warning) | `lookahead` without `speculative: true`; `modeArg` with `popMode`; case-folded duplicate mode name; mode with only `defaultToken` and no `tokens` field. The rule still loads. |
| `P_ContextualKeywordResolution` (info, at parse) | A soft keyword demoted to `Identifier`. Expected behavior when the cursor's expected set excludes the keyword. |
| `P_SchemaCursorDesync` (info, one-shot) | The schema cursor went off-track. Usually means a caller drove the builder against a sequence the schema doesn't expect. Contextual resolution stays strict from this point. |
| `P_MaxSpeculationDepth` (error, one-shot) | `TreeBuilder::Checkpoint` stack exceeded `BuilderConfig::maxSpeculationDepth` (default 64). Subsequent `checkpoint()` calls return no-op guards. |
| `P_UncommittedCheckpoint` (warning) | A `Checkpoint` guard was destroyed without `commit()` or `rollback()` — the dtor rolled it back. Indicates a forgotten-commit bug in the caller. |

---

## 10. v3 additions

Added by the SP2 type-lattice work. Opt-in and additive: configs that don't use them keep loading as before. Set `"dssSchemaVersion": 3` once any v3 field appears.

### 10.1 `typeExtensions` — per-language extension type-kinds

The compiler's **core type lattice** (primitives, aggregates, SIMD, pointers/references, function signatures, …) is universal and hardcoded. A language that needs **nominal, language-specific** types it can't express in the core lattice — `TSQL::Varchar<N>`, `C++::MemberPtr`, `C#::Boxed<T>` — declares them here. Each becomes a registered extension type-kind (per-CU, nominal, language-qualified) the semantic phase can reference.

```jsonc
{
  "dssSchemaVersion": 3,
  "language": { "name": "TsqlSubset", "version": "0.1.0" },

  "typeExtensions": [
    {
      "name": "TSQL::Varchar",                          // language-qualified
      "parameters": [ { "name": "N", "kind": "Integer" } ]
    },
    { "name": "TSQL::RowType" }                          // parameters optional
  ]
}
```

- `name` (required, string) — the extension's name. Convention: language-qualified (`<Lang>::<Type>`). Extensions are **nominal** — `C++::Boxed` and `C#::Boxed` are distinct kinds even if structurally identical.
- `parameters` (optional, array) — formal parameters. Each is `{ "name": <string>, "kind": "Integer" | "Type" }` (`Integer` = a compile-time integer like a length; `Type` = a type parameter).

The lattice is **CU-scoped**: each `CompilationUnit` gets its own interner + registry, and extension kinds registered for one language don't leak into another. A `TypeId` from one CU used against another's lattice aborts loudly.

### 10.2 Troubleshooting (v3 codes)

| Symptom | Likely fix |
|---|---|
| `C_UnknownTypeExtension` | `typeExtensions` isn't an array, or an entry isn't an object — each entry must be `{ "name": ..., "parameters": [...] }`. (The same code is emitted by later phases when a type references an extension name no registry resolved.) |
| `C_TypeExtensionParamMismatch` | A `parameters` entry is malformed: not an object, missing string `name`/`kind`, an unknown `kind` (only `Integer`/`Type`), or `parameters` itself isn't an array. |
| `C_ConflictingField` on `typeExtensions` | The same extension `name` is declared more than once in the file. |
| `C_MissingField` on `typeExtensions/*/name` | An entry has no string `name`. |

---

## 11. v4 additions

Added by the config-driven import refactor. Opt-in and additive: configs that don't declare an `imports` block keep loading as before (and resolve no cross-file imports). Set `"dssSchemaVersion": 4` once the block appears.

### 11.1 `imports` — config-driven import resolution

Cross-file import resolution (populating a `CompilationUnit`'s `crossRefs`) is handled by **one language-agnostic engine** — no engine code branches on the source language name. Per-language behavior comes entirely from this block: the engine reads `strategy` and the parameter fields and runs the matching generic algorithm. This replaces the old per-language import resolver C++ (which dispatched on the language name).

Two generic strategies ship (plus `none`):

```jsonc
// include-following — e.g. C-style `#include "x.h"`
{
  "dssSchemaVersion": 4,
  "language": { "name": "CSubset", "version": "0.1.0" },

  "imports": {
    "strategy":      "include-following",
    "directiveRule": "includeDirective",   // shape wrapping the directive
    "pathToken":     "StringStart"          // token whose quoted literal is the path
  }
}
```

```jsonc
// name-matching — e.g. T-SQL table references resolved to CREATE TABLE
{
  "dssSchemaVersion": 4,
  "language": { "name": "TsqlSubset", "version": "0.1.0" },

  "imports": {
    "strategy":         "name-matching",
    "nameRule":         "qualifiedName",                                  // the name shape
    "definitionRule":   "createTableStmt",                               // shape that DEFINES a name
    "referenceParents": ["tableRef", "insertStmt", "updateStmt", "deleteStmt"], // reference positions
    "nameToken":        "Identifier",                                    // token keyed for matching
    "caseSensitive":    false                                            // SQL folds identifier case
  }
}
```

- `strategy` (required, string) — `"none"`, `"include-following"`, or `"name-matching"`.
- **include-following** resolves each `directiveRule` node's `pathToken` quoted literal against the including file's directory + declared include dirs, **loads** the target into the CU (recursively, deduplicating by canonical path), and records a `CrossTreeRef` from the directive to the included tree's root. Required: `directiveRule`, `pathToken`.
- **name-matching** matches a `nameRule` appearing under one of `referenceParents` to a `nameRule` under a `definitionRule` of the same name in **another** tree (keyed on the last `nameToken` text). Required: `nameRule`, `definitionRule`, non-empty `referenceParents[]`, `nameToken`.
- `caseSensitive` (optional, bool, default `true`) — when `false`, names are case-folded before matching (set by case-insensitive languages like T-SQL).

Unresolved references surface as driver diagnostics (`D_UnresolvedImport` / `D_UnresolvedReference`), never silently dropped. Referenced rule/token names are validated against the schema's interners at load time.

### 11.2 Troubleshooting (v4 codes)

| Symptom | Likely fix |
|---|---|
| `C_InvalidImports` | `imports` isn't an object; `strategy` is missing/not a string/not one of `none`/`include-following`/`name-matching`; a parameter field has the wrong JSON type; or `referenceParents` isn't a non-empty array. |
| `C_MissingField` on `imports/*` | A field required by the chosen `strategy` is absent or empty (e.g. include-following without `directiveRule`/`pathToken`; name-matching without `nameRule`/`definitionRule`/`nameToken`/`referenceParents`). |
| `C_UnknownShape` on `imports/*` | A `directiveRule`/`nameRule`/`definitionRule`/`referenceParents[]` entry names a shape not declared under `shapes`. |
| `C_UnknownToken` on `imports/*` | A `pathToken`/`nameToken` names a token kind no `tokens`/`keywords`/built-in declared. |

### 11.3 `expr.wrapperRules` — per-language Pratt wrapper names

Every `expr` shape now declares the three Pratt-walker wrapper rule names the engine will synthesize around operator-precedence results. The engine no longer hardcodes any wrapper-rule name; each language picks its own. The walker reads the three RuleIds out of the schema once per `walkExpression` entry.

```jsonc
"expression": {
  "expr": {
    "atom":         "operand",
    "minPrecedence": 0,
    "wrapperRules": {
      "binary":  "binaryExpr",    // each language picks its own names
      "unary":   "unaryExpr",
      "postfix": "postfixExpr"
    }
  }
}
```

- All three (`binary` / `unary` / `postfix`) are required when `wrapperRules` is present; an `expr` shape without a complete `wrapperRules` block fails to load (`C_MissingWrapperRules`).
- Names declared here cannot also appear under top-level `shapes` — wrapper rules are walker-synthesized and have no compiled body. A redeclaration fails to load (`C_UnknownShape`).
- A non-`expr` language uses none of this — the block is required only on `expr`-shape rules.

### 11.4 `numberStyle` — numeric-literal grammar

`numberStyle` is the universal config-driven numeric scanner descriptor. The tokenizer's `scanNumber()` walks the active language's `NumberStyle` — no hardcoded letter classes, no C-style assumptions baked into the engine.

```jsonc
"numberStyle": {
  "decimal":         true,             // permit bare decimal digits
  "integerPrefixes": [                 // optional; empty = decimal only
    { "prefix": "0x", "radix": 16, "digits": "0-9a-fA-F" },
    { "prefix": "0b", "radix": 2,  "digits": "01" },
    { "prefix": "0o", "radix": 8,  "digits": "0-7" }
  ],
  "exponent": {                        // optional; absent = integer only
    "letters":      ["e", "E"],
    "signOptional": true
  },
  "fractionPoint":   ".",              // optional; absent = no float literal
  "digitSeparator":  "_",              // optional; absent = no separators
  "integerSuffixes": ["u","U","l","L","ll","LL","ul","UL","ull","ULL"],
  "floatSuffixes":   ["f","F","d","D"],
  "emitKind": {                        // which token kinds the scanner emits
    "integer": "IntLiteral",
    "float":   "FloatLiteral"
  }
}
```

- `decimal` (bool, default `false`) — permit bare digits at start of a number.
- `integerPrefixes[*]` — `{ prefix, radix, digits }`. The scanner walks prefixes in declaration order; the first whose `digits` class accepts the byte after the prefix wins. `radix` ∈ [2, 36]. `digits` is a character class string supporting `a-z` ranges (e.g. `"0-9a-fA-F"`).
- `exponent` — `{ letters, signOptional }`. `letters[*]` are single ASCII characters; the scanner accepts any one of them as the exponent introducer. `signOptional` defaults to `true`. **Semantics**: `signOptional: true` means a `+` or `-` MAY appear between the exponent letter and digits (`1e+3`, `1e-3`, `1e3` all accepted — the C-style default). `signOptional: false` means NO sign is permitted there — `1e+3` tokenizes as `1` + identifier-or-token `e` + `+` + `3`, NOT as one float. The name does NOT mean "sign required"; that mode is intentionally not modeled.
- `fractionPoint` — single ASCII char (typically `.`). When set AND followed by a digit, the scanner promotes the literal to float.
- `digitSeparator` — single ASCII char. Accepted (and consumed silently) between digits.
- `integerSuffixes` / `floatSuffixes` — string arrays; the scanner longest-matches against them after the number body. A float-suffix match promotes the kind.
- `emitKind.integer` — required; names the token kind the scanner emits for integer literals.
- `emitKind.float` — required IFF any float-producing facet is declared (`exponent` / `fractionPoint` / non-empty `floatSuffixes`).

Languages with no numeric literals omit the block entirely. Configs that reference `IntLiteral`/`FloatLiteral` in any shape but declare no `numberStyle` fail to load (`C_MissingNumberStyle`).

### 11.5 Troubleshooting (08.55 codes)

| Symptom | Likely fix |
|---|---|
| `C_MissingWrapperRules` | An `expr` shape was declared without a complete `wrapperRules` block, or one of `binary`/`unary`/`postfix` is missing/empty/non-string. Names declared here cannot collide with top-level `shapes`. |
| `C_MissingNumberStyle` | The language declared `IntLiteral`/`FloatLiteral` in any shape but omitted the `numberStyle` block entirely. |
| `C_MissingField` on `numberStyle/...` | A required sub-field is absent or empty (e.g. `emitKind`, `emitKind.integer`, a prefix's `prefix`/`radix`/`digits`). |
| `C_InvalidNumberStyle` | The block is present but malformed: wrong JSON type, `radix` outside `[2, 36]`, `fractionPoint`/`digitSeparator` not single-char, non-bool `signOptional`, etc. |
| `C_UnknownToken` on `numberStyle/emitKind/*` | The named token kind isn't declared anywhere (built-in or `tokens`). |

### 11.6 `artifactProfiles` — supported output shapes (plan 06 AP1)

An **optional** top-level array naming the **artifact profiles** a language can be compiled into — the shape its output takes (a console binary, a shared library, a SQL script, …). It is per-language data: each `.lang.json` declares its own set. AP1 is the schema-field + loader-validation slice only; no codegen or driver consumes it yet (the driver-enforcement layer, AP2+, reads this set to reject a project asking for a profile the language can't produce).

```jsonc
{
  "dssSchemaVersion": 4,
  "language": { "name": "CSubset", "version": "0.1.0", "fileExtensions": [".c", ".h"] },

  "artifactProfiles": ["cli", "lib", "staticlib"],   // ← this language's supported outputs

  "tokens":  { /* ... */ },
  "shapes":  { /* ... */ }
}
```

- **Optional.** Absent ⇒ the language declares no profiles (`artifactProfiles()` returns an empty span); the config still loads cleanly.
- Each entry must be a name in the **registered profile set**: `cli`, `gui`, `lib`, `staticlib`, `script`, `sproc`, `transpile`, `shader`, `hdl` (plan 06 §3). The set is loader-owned vocabulary, not a config-authored name — a new profile arrives with the backend plan that introduces it.
- An unknown name → `C_UnknownArtifactProfile`. A malformed block (not an array, or a non-string entry) → the same `C_UnknownArtifactProfile`. A duplicate entry → `C_RedundantField` (the duplicate is dropped).
- The shipped languages declare: `toy` → `["cli"]`; `c-subset` → `["cli", "lib", "staticlib"]`; `tsql-subset` → `["script", "sproc"]`.

| Symptom | Likely fix |
|---|---|
| `C_UnknownArtifactProfile` | An entry isn't in the registered set, OR `artifactProfiles` isn't an array, OR an entry isn't a string. Use one of `cli`/`gui`/`lib`/`staticlib`/`script`/`sproc`/`transpile`/`shader`/`hdl`. |
| `C_RedundantField` on `artifactProfiles` | The same profile is listed twice; remove the duplicate. |
