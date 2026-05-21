# Tokenizer Phase — Sub-Plan

> Opens parent plan #5 (`tokenizer`). The first real consumer of v2's `lexerModes` + `stringStyle` machinery. Three PRs (TZ1–TZ3); bounded scope; the per-PR cadence from v2 / SH applies.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🔵 **in progress.** TZ1 + TZ2 shipped (TZ2 had a comprehensive review-fix round 2 closing 24 items). TZ3 (c-subset + tsql-subset E2E) next. 623 cases / 29 suites, 100% pass. |
| Predecessors  | ✅ v1 T0–T12 (`tree-node-model-plan.md`); ✅ v2 PR0–PR8 (`schema-expressiveness-v2-plan.md`); ✅ SH1–SH4 (`substrate-hardening-plan.md`). |
| Successors    | Master plan phase #6 (`analysis-lexical`) **subsumed** into this plan + builder — the v2 schema-aware builder already does the validation phase 6 was envisioned for. Master plan phase #7 (`analysis-syntactic`) becomes the next-up phase once TZ3 lands. |
| Scope         | **Bounded.** TZ1: bare tokenizer + toy.lang.json. TZ2: lexer modes + strings + comments. TZ3: c-subset + tsql-subset end-to-end. Anything new (interpolated strings, `schema-expressiveness-v3-plan.md`-class gaps surfaced by TZ3) becomes a separate sub-plan. |

### PR landing log

| PR | Status | Commit / notes |
|---|---|---|
| TZ1 | ✅ done | Bare tokenizer skeleton (`src/tokenizer/`) — `SourceReader` + `Tokenizer` + `TokenStream` returning a named `TokenizeResult { stream, diagnostics }`. Whitespace, single-char punctuation, identifiers/keywords, integer + float literals (hand-coded with `0x`/`0b`/`0o` base-prefix recognition; restricted suffix charset; `P_MalformedNumber` on empty-digit-prefix bodies like `0x_`), Eof. No lexer modes, no string styles, no comments. `Token.schemaKind` ownership moved from builder to tokenizer; `Builder.pushToken`'s fast-path trusts `tok.schemaKind` when scope-allowed, falls back to full lookup otherwise (preserves both `P_AmbiguousToken` warning AND slow-path-equivalent `matchCount`). New diagnostic codes `P_IllegalChar`, `P_MalformedNumber`. Longest-match probe length now derived from `GrammarSchema::maxLexemeLength()` with a debug-only consistency check in the ctor. UTF-8 BOM skipped at byte 0; mid-source BOM accepted as legitimate U+FEFF via byte-pass-through identifier model (v3 work for full Unicode XID semantics); lone continuation bytes (0x80-0xBF / reserved leads) flagged as `P_IllegalChar`. `SourceBuffer::fromFile/fromString` enforce the 4-GiB `ByteOffset` cap; `fromFile` also checks `ifstream::bad()` so mid-stream IO errors don't silently truncate. `Token` gains a trivially-copyable `static_assert`; `TokenStream` gets explicit move ops (defaulted-from-vector "valid but unspecified" was leaking past the empty-stream guard). Existing `test_tree_end_to_end.cpp` (9 tests) flipped from hand-tokenization to real tokenization against `toy.lang.json` — all pass identically. 68 new tokenizer unit tests across 3 suites (initial + two review-fix rounds). 599 ctest cases / 29 suites, 100% pass. <!-- LANDING-LOG-HASHES: TZ1 -->`5d1d95d`<!-- /LANDING-LOG-HASHES --> + `8f08599` (review-fix round 1) + `33b3ec4` (review-fix round 2). |
| TZ2 | ✅ done | Lexer modes + strings + comments. Tokenizer owns a single `frames` stack of `Frame { LexerModeId, StringStyle const*, std::string dynamicSuffix }` (review-fix replaced three parallel vectors — defensive empty-check guards retired as fatal-asserts). Body-mode branch resolves "doubled-delim → endsAt → char-escape → per-codepoint defaultToken" with UTF-8 codepoint length detection. `escapeKind`: `Char` consumes lead + next codepoint; `DoubledDelimiter` consumes doubled endsAt as a literal; `None` plain pass-through. Main-mode `modeOp`/`modeArg` applied AFTER token emit; `tagPattern` regex captured at opener for dynamic-suffix close matching (compiled regex cached per `StringStyleId`; plumbing in place; no shipped schema exercises it yet). EOF with open modes emits `P_UnterminatedString` or `P_UnterminatedComment` per schema-declared `unterminatedAs` enum (no more substring-match on mode name). New `LexerMode::defaultToken` holds a `DefaultTokenSpec { SchemaTokenId kind; NodeFlags flags }`; `Token` gains a `flags` field that the builder OR-merges with `meaning.flagsApplied` at pushToken time — c-subset's comment modes use `defaultToken.flags: ["EmptySpace"]` so the AST cursor skips comment bodies wholesale (closes `v2-gap-catalog.md` row 3 end-to-end). New diagnostic codes: `P_UnterminatedString`, `P_UnterminatedComment`, `P_InvalidEscape`. Loader-side cross-field validation rejects `endsAtLongestMatch + tagPattern` (silently disagrees at runtime); runtime fatal-asserts guard against `PushMode` into a defaultToken-bearing mode without a stringStyle and against `escapeKind=Char` with `escapeChar=0`. Initial commit added 10 test cases (line + block comments, untermination diagnostics, T-SQL doubled-delimiter + bracket-id, non-nested block comments, mode-stack reset, schema-side EmptySpace pin); review-fix round 2 added 5 inline-schema branch tests (char-escape, endsAtLongestMatch, replaceMode, explicit popMode, doubled-delim single-occurrence close), tightened EXPECT_GE → EXPECT_EQ on 3 tests with full-token breakdowns, pinned position invariants on NestedBlockComments and ModeStackResets, and added a builder-side `TokenFlags_PropagateToBuilderLeafFlags` test that pins CR1's effectiveFlags wiring (the test uses an inline schema where the global meaning carries no flags so the OR-merge must depend on `tok.flags` alone). 623 cases / 29 suites, 100% pass. <!-- LANDING-LOG-HASHES: TZ2 -->`90c7f75`<!-- /LANDING-LOG-HASHES --> + `8612486` (review-fix round 2, 24 items addressed). |
| TZ3 | ⏳ pending | All-language end-to-end. Every existing E2E test flipped from hand-tokenization to real tokenization: `toy`, `c-subset` (incl. SH4b's switch shapes), `tsql-subset` (three lexer modes, doubled-delimiter strings, bracket-quoted identifiers, three-part qualified names). Surfaces any v3-candidate schema gaps (interpolation, non-C float syntax, `endsAt: longestMatch` corner cases) as `v2-gap-catalog.md` § entries. <!-- LANDING-LOG-HASHES: TZ3 --><!-- /LANDING-LOG-HASHES --> |

---

## 1. Motivation

Master plan §4.5 ("`tokenizer/` — Character Stream to Token Stream") and §4.6.1 ("`analysis/lexical/` — Phase 1: Lexical Analysis") describe a two-phase split: tokenizer assigns `coreKind`, lexer validates/classifies. That split predates v2. The v2 schema-aware builder absorbed what was originally phase 6's job — `scopeRequire` filtering, `expectedSet`-driven contextual-keyword demotion, `P_SchemaCursorDesync`. What remains for a tokenizer-phase implementation is:

1. **Character → token splitting** with longest-match lexeme lookup against the schema's lexeme table.
2. **Lexer-mode-stack tracking** (`LexerModeStack` from PR5) so per-mode token tables (`lookupLexemeInMode`) drive resolution inside string bodies, comments, and any other mode the language declares.
3. **String body lexing** honoring `stringStyle.escapeKind` / `endsAt` / `endsAtLongestMatch` / dynamic `tagPattern` (PR6 mechanism).
4. **Hand-coded numeric lexing** (gap-catalog row 14 is v3-candidate for full schema-driven numeric styling; until then, the tokenizer hard-codes C-style float syntax as the universal default).
5. **`Token.schemaKind` resolution** — historically the `Token` comment said "assigned by the schema-aware resolver inside `TreeBuilder::pushToken`," but that only does single-mode `lookupLexeme`. The tokenizer is the right owner because it tracks the mode stack; builder.pushToken keeps doing the parts that need scope-stack + schema cursor (scope-filter, contextual demotion, cursor advance).

The tokenizer is also the **first real test of v2's expressiveness under live input** — PR7's tsql-subset stress drove hand-built tokens through `TreeBuilder`. TZ3 drives the full E2E chain with the tokenizer in the middle. Gaps that surface there become the candidate set for a future `schema-expressiveness-v3-plan.md`.

---

## 2. Design

### 2.1 Files

```
src/tokenizer/
├── CMakeLists.txt
├── source_reader.hpp / .cpp        # buffered char reader with peek/advance + UTF-8 awareness
├── tokenizer.hpp / .cpp            # main engine — owns LexerModeStack
└── token_stream.hpp / .cpp         # iterable container w/ peek / advance / match / expect

tests/tokenizer/
├── CMakeLists.txt
├── test_source_reader.cpp
├── test_tokenizer.cpp              # unit tests against synthetic schemas
└── test_token_stream.cpp
```

End-to-end tests stay where they are — `tests/core/test_tree_end_to_end.cpp`, `tests/core/test_c_subset.cpp`, `tests/core/test_tsql_subset.cpp` — and flip from hand-tokenization to real tokenization in TZ1 / TZ3 respectively.

### 2.2 Public API

```cpp
class Tokenizer {
public:
    Tokenizer(std::shared_ptr<SourceBuffer>        src,
              std::shared_ptr<GrammarSchema const> schema,
              DiagnosticReporter::Config           diagConfig = {});

    // Single-use, like TreeBuilder.
    Tokenizer(Tokenizer const&)            = delete;
    Tokenizer& operator=(Tokenizer const&) = delete;

    // Consume the entire source; returns a TokenStream + the
    // DiagnosticReporter ownership. Stream is positioned at index 0.
    [[nodiscard]] std::pair<TokenStream, std::unique_ptr<DiagnosticReporter>>
        tokenize() &&;
};

class TokenStream {
public:
    [[nodiscard]] Token const& peek(std::size_t lookahead = 0) const noexcept;
    Token                       advance() noexcept;
    [[nodiscard]] bool          isAtEnd() const noexcept;
    [[nodiscard]] std::size_t   position() const noexcept;
    void                        rewind(std::size_t pos) noexcept;
    // Bookmark/restore mirrors TreeCursor's pattern; ABA-protected
    // via the owner-stamp idiom from LexerModeStack::Snapshot.
};
```

The tokenizer is **single-use, batch-mode**: it consumes the whole source up front and hands back a complete `TokenStream`. Lazy-iterator was considered and rejected for the same reason `TreeBuilder` is single-use — the whole-stream view simplifies the parser-phase (#7) code paths that will run lookahead / Pratt parsing on it.

### 2.3 Token resolution flow

```
chars                       lookup
  │       ┌── LexerModeStack ──┐
  ▼       │                    ▼
SourceReader ──▶ scan-lexeme ──▶ schema.lookupLexemeInMode(mode, lexeme)
                  (longest match)    │
                                     ▼
                              winning LexemeMeaning
                                     │
                          ┌──────────┼──────────┐
                          ▼          ▼          ▼
                    schemaKind   modeOp/Arg  emit Token
                    on Token     applied to    │
                                  stack        ▼
                                          TokenStream
```

Steps in order:

1. **Skip nothing.** Whitespace is emitted as a token (`CoreTokenKind::Whitespace`); the schema's `EmptySpace` flag is applied via `flagsApplied` on the meaning, not by the tokenizer dropping the token. Builder's `pushToken` propagates `NodeFlags::EmptySpace` per the meaning.
2. **Probe one char**, then ask `SourceReader` for the longest run of identifier-like / digit / operator-like chars consistent with the start.
3. **Look up the lexeme in the current mode.** `schema.lookupLexemeInMode(modeStack.top(), lexeme)` returns the candidate `LexemeMeaning`s. Tokenizer picks the highest-priority candidate (lowest `priority` value wins; declaration order on ties). Scope filtering is **not** done here — the builder's `pushToken` does it.
4. **Word fallback.** If no meaning matches and the lexeme is an alphanumeric run, emit `Token { coreKind = Word, schemaKind = invalid }`. Builder's `pushToken` does the keyword-vs-identifier fallback (existing logic, unchanged).
5. **Apply mode side-effects.** The winning meaning's `modeOp` / `modeArg` are applied to the `LexerModeStack` AFTER the token is emitted. So `pushMode: "single-string"` on `'` means the `'` token still tokenized in main mode, but the next token is in `single-string`.
6. **`defaultToken` fallback.** If no meaning matches AND the current mode declares a `defaultToken`, emit `Token { coreKind = (from default), schemaKind = (default's id) }` for one character. This is the per-char body emission for strings/comments.
7. **EOF.** When `SourceReader` exhausts, emit one `Token { coreKind = Eof, span = {size, size} }`. If the mode stack is non-empty at EOF, emit `P_UnterminatedString` / `P_UnterminatedComment` (mode-specific code chosen by the mode's name; details in TZ2).

### 2.4 Numeric lexing (hand-coded)

Tokenizer hard-codes C-style numeric grammar:

```
int    := [0-9]+ ([uU][lL]{0,2} | [lL]{1,2}[uU]?)?
float  := ([0-9]+ \. [0-9]*  |  \. [0-9]+  |  [0-9]+)
         ([eE] [+-]? [0-9]+)?
         [fFlL]?           // float-only when '.' OR exponent present
```

The grammar lives in the tokenizer, not in the schema. Languages that want different numeric syntax (e.g., Python's `1_000.5` underscores, Rust's `0x_dead_beef`, Ada's based literals) will need a **`numberStyle` descriptor** in a future `schema-expressiveness-v3-plan.md`. Gap-catalog row 14 already records this; TZ adds a reciprocal note that the tokenizer's hand-coded path is the **interim default**, not the long-term plan.

### 2.5 Token.schemaKind ownership change

**Token.hpp comment update**: drop the line "assigned by the schema-aware resolver inside `TreeBuilder::pushToken`"; replace with "assigned by `Tokenizer` via mode-aware lookup against `GrammarSchema::lookupLexemeInMode`."

**Builder.pushToken simplification**: `resolveMeaning` no longer does `schema.lookupLexeme(lexeme)` from scratch — it operates on `tok.schemaKind` already being resolved, and only filters the candidate set by scope-stack + contextual-keyword demotion. The "fall back to Identifier when the lexeme isn't a keyword" path becomes "fall back to Identifier when `tok.coreKind == Word && tok.schemaKind == InvalidSchemaToken`."

This is a deviation from the original Token comment but matches the v2 mode-aware design.

### 2.6 Diagnostics

Tokenizer owns its own `DiagnosticReporter`. New codes (placed in `parse_diagnostic.hpp`):

| Code | When |
|---|---|
| `P_UnterminatedString` | Source ends before string-body mode's `endsAt` is matched. |
| `P_UnterminatedComment` | Source ends before block-comment mode's closer is matched. |
| `P_InvalidEscape` | `escapeKind: Char` + escape-lead char followed by EOF or a char not on the accepted list (TZ2 design decision: do we allow arbitrary chars after `\`, or restrict to a known set?). |
| `P_IllegalChar` | Char doesn't start any lexeme and isn't whitespace and no `defaultToken` is declared. Tokenizer emits a 1-char Error token and continues. |

`P_UnknownToken` (existing) is reused when the tokenizer produces a lexeme but the builder's resolution fails — orthogonal to the tokenizer's own diagnostics. The two reporters are merged in TZ3's end-to-end glue or kept separate per the existing builder pattern (`Tree` carries the builder's reporter; the tokenizer's reporter is consumed by whatever owns the pipeline — likely `Compiler` in phase #7+ work).

### 2.7 Phase #6 deletion

Master plan §8 row 6 (`analysis-lexical`) is subsumed. The plan-doc updates in TZ1 reflect this — row 6 becomes "✅ subsumed into #5 (TZ1–TZ3)."

---

## 3. PRs

### TZ1 — Bare tokenizer + toy.lang.json flip

**Goal.** Stand up `src/tokenizer/` with the minimum API needed to drive `TreeBuilder` from real source bytes against `toy.lang.json`. No lexer modes (always main). No strings (toy has none). No comments (toy has none). Numeric: int + float per §2.4.

**Surface.**
- `src/tokenizer/source_reader.{hpp,cpp}` — buffered byte reader with `peek(n)` / `advance(n)` / position tracking. UTF-8: bytes-pass-through; the schema's lexeme keys are byte strings, so UTF-8 multi-byte sequences in identifiers fall out automatically.
- `src/tokenizer/tokenizer.{hpp,cpp}` — `Tokenizer` class per §2.2. Internal trie or sorted-key vector of lexeme strings for longest-match (built lazily on first `tokenize()`).
- `src/tokenizer/token_stream.{hpp,cpp}` — `TokenStream` class per §2.2.
- `src/core/types/token.hpp` — comment update per §2.5.
- `src/core/types/tree_builder.cpp` — `resolveMeaning` simplification per §2.5 (tokenizer-resolved kinds bypass the fresh lookup; scope-filter + contextual demotion remain).
- `tests/tokenizer/{test_source_reader,test_tokenizer,test_token_stream}.cpp` — unit tests.
- `tests/core/test_tree_end_to_end.cpp` — flip from hand-tokenization (the existing `TokenSeq` pattern) to real tokenization against `toy.lang.json`. All 9 existing tests should pass identically.

**Tests.** ~40 new cases.

**Out of scope.** Lexer modes (TZ2). String / comment body handling (TZ2). c-subset and tsql-subset E2E flips (TZ3).

---

### TZ2 — Lexer modes + strings + comments

**Goal.** Tokenizer becomes a full mode-stack-aware engine. Resolves `defaultToken` per-char inside non-main modes. Honors `stringStyle.endsAt` / `escapeKind` / `endsAtLongestMatch` / dynamic `tagPattern`. c-subset gains comment modes (closes `v2-gap-catalog.md` row 3 authoring task).

**Surface.**
- `Tokenizer::tokenize`: track `LexerModeStack`; on every resolved meaning apply `modeOp` / `modeArg` AFTER token emission; when current mode has a non-empty `defaultToken` and no lexeme matches, consume one char and emit the default token; same for whitespace inside modes (mode authors can `EmptySpace`-flag the default if they want it ignorable).
- StringStyle handling — see §2.3 step 6 plus body-end detection: scan to the next occurrence of `endsAt`, honoring `escapeKind` (skip the escape pair), `endsAtLongestMatch` (consume the **longest** match of `endsAt`'s pattern), and dynamic-tag `tagPattern` (the body ends at the same captured tag as the opener).
- New diagnostics: `P_UnterminatedString`, `P_UnterminatedComment`, `P_InvalidEscape` per §2.6.
- `src/source-config/languages/c-subset.lang.json`: add a `line-comment` mode (`defaultToken: { kind: "CommentChar", flags: ["EmptySpace"] }`; opener `//`; newline pops); a `block-comment` mode (opener `/*`; closer `*/`); existing config rows for `//` and `/*` adjusted accordingly. Authoring should mirror the `tsql-subset.lang.json` mode patterns.
- `tests/tokenizer/test_tokenizer.cpp` — string-body, comment-body, multi-mode push/pop tests.
- `tests/core/test_c_subset.cpp` — at least one test with an inline comment proves the new mode works end-to-end.

**Tests.** ~30 new cases.

**Out of scope.** tsql-subset E2E (TZ3 — it stresses every mode at once). Interpolated strings (`${...}` inside `"..."`) — future v3 work. macOS comment auto-detection or other heuristics — c-subset declares its modes explicitly.

---

### TZ3 — All-language end-to-end

**Goal.** Every shipped `.lang.json` config tokenizes + parses end-to-end via the real tokenizer. Tests that previously hand-tokenized via `TokenSeq` flip to `Tokenizer`. Surfaces any v3-candidate gaps as `v2-gap-catalog.md` entries.

**Surface.**
- `tests/core/test_c_subset.cpp` — all 5 existing tests flip. Source bytes go through `Tokenizer`; token stream drives `TreeBuilder` directly (parser phase #7 is still pending, so tests still call `b.open(rule)` manually; the difference is `b.pushToken(tok)` now consumes a real `Tokenizer`-produced `Token` instead of a hand-built one).
- `tests/core/test_tsql_subset.cpp` — 19 existing tests flip. Stress every v2 mechanism under live tokenization: contextual `reservedWordPolicy`, three lexer modes (`bracket-id`, `single-string`, `unicode-string`), doubled-delimiter `'a''b'` and `N'…''…'`, three-part qualified names, every statement shape.
- `v2-gap-catalog.md` updates — record any new schema gaps the live tokenizer exposes (e.g., a mode-transition footgun, a `defaultToken`-interaction with EmptySpace flagging, a longest-match corner case in `endsAt`).
- Master plan §8 row 6 — flip from `pending` to `subsumed into #5`.

**Tests.** No new tests added — every flipped existing test counts as a new validation of the tokenizer. ~25 tests collectively flip.

**Out of scope.** Real parser (phase #7). Real semantic analyzer (phase #8). Interpolated-string tokenization (deferred).

---

## 4. Sequencing

TZ1 → TZ2 → TZ3. Each builds on the previous.

Per-PR cadence reuses the v2 / SH discipline: implement → 5-agent review (code-reviewer, silent-failure-hunter, comment-analyzer, pr-test-analyzer, type-design-analyzer) → comprehensive fix-all → cross-plan refresh → commit.

## 5. What comes after

Once TZ1–TZ3 all land:
- `compiler-implementation-plan.md` §8 row 5 flips to ✅ done; row 6 flips to ✅ subsumed.
- `tokenizer-plan.md` §0 flips to ✅ done.
- v2-gap-catalog entries that the live tokenizer surfaces become the seed for `schema-expressiveness-v3-plan.md` — the candidates known today are: row 14 (float literals — formal `numberStyle` descriptor), row 17 (ternary mixfix), interpolated strings, plus whatever else TZ3 turns up.
- Parent plan phase #7 (`analysis-syntactic` / parser) opens. The parser drives `TreeBuilder` from a `TokenStream`; the tokenizer phase delivered that stream end-to-end.
