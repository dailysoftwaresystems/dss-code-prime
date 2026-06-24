#pragma once

#include "core/export.hpp"

#include <string>

namespace dss {

// Config-driven C-preprocessor declaration (schema v4 `preprocess` block).
//
// The single language-agnostic preprocessor pass (`src/analysis/preprocess/`)
// reads this struct instead of branching on the language name -- the whole
// preprocessor is a config-SELECTED pass. A `.lang.json` that carries a
// `preprocess` block with `enabled: true` OPTS IN and declares the directive
// vocabulary; a config that omits the block (toy / tsql-subset) gets
// `enabled == false` and the pass is a strict NO-OP (token stream in == out,
// proven by a strict test).
//
// The vocabulary is split into two kinds of strings:
//
//   TOKEN names (`directiveIntroToken`, `quoteIncludeToken`,
//   `angleIncludeToken`, `functionLikeOpenToken`) name SchemaTokenId kinds --
//   the loader validates each against `data.schemaTokens->contains(...)` (same
//   `C_UnknownToken` fail-loud as the `imports` block). They let the engine
//   recognise the `#` that opens a directive line, the `"`/`<` that opens an
//   include target, and the `(` that makes a `#define` function-like WITHOUT
//   hard-coding any lexeme.
//
//   DIRECTIVE-KEYWORD strings (`defineDirective`, `undefDirective`,
//   `includeDirective`) name the directive WORDS (`define` / `undef` /
//   `include`). These are matched by LEXEME TEXT against the token that
//   follows the intro `#`. `define`/`undef` lex as plain `Identifier` (they
//   are NOT grammar keywords), so matching them by text keeps the grammar
//   untouched. The loader validates each as a non-empty string.
//
// Every field is validated at load (`C_InvalidPreprocess` / `C_MissingField`
// / `C_UnknownToken`) so a loaded schema is guaranteed self-consistent; the
// engine's lookups are defensive only.
struct DSS_EXPORT PreprocessConfig {
    // False (the default) => the language declares no `preprocess` block and
    // the pass is a strict identity. Set true only when a well-formed block
    // with `enabled: true` is present.
    bool enabled = false;

    // The token kind that OPENS a directive line (C's `#` -> "HashOp").
    std::string directiveIntroToken;

    // Directive WORDS, matched by lexeme text against the token after `#`.
    std::string defineDirective;   // "define"
    std::string undefDirective;    // "undef"
    std::string includeDirective;  // "include"

    // FC14 (D-PP-CONDITIONAL-COMPILATION): the CONDITIONAL-compilation directive
    // WORDS (C 6.10.1), matched by lexeme TEXT against the token after `#` --
    // exactly like define/undef/include (`if`/`elif`/`else`/`endif` lex as plain
    // Identifier; `ifdef`/`ifndef` likewise). REQUIRED-when-the-block-is-present
    // + validated non-empty by the loader (same fail-loud as defineDirective): an
    // opt-in language declares the whole conditional vocabulary so the engine
    // never hard-codes a directive spelling. A language without a preprocess
    // block carries none of these (the pass is a strict no-op).
    std::string ifDirective;       // "if"
    std::string ifdefDirective;    // "ifdef"
    std::string ifndefDirective;   // "ifndef"
    std::string elifDirective;     // "elif"
    std::string elseDirective;     // "else"
    std::string endifDirective;    // "endif"

    // FC14: the `defined` OPERATOR keyword (C 6.10.1p1), valid only inside an
    // `#if`/`#elif` operand: `defined X` / `defined(X)` tests macro-definedness.
    // Matched by lexeme TEXT (an ordinary identifier in the operand, NOT a
    // distinct token kind -- like the directive WORDS), so a per-language
    // CONFIG spelling, never a hard-coded "defined". REQUIRED + validated
    // non-empty when the block is present.
    std::string definedOperator;   // "defined"

    // The token kind that opens a QUOTE include target (`#include "h"` ->
    // "StringStart"). Resolved relative to the including file's directory +
    // include dirs; the PP splices the (recursively preprocessed) header
    // TEXT into the synthesized buffer.
    std::string quoteIncludeToken;

    // The token kind that opens an ANGLE include target (`#include <h>` ->
    // "HeaderStart"). Angle includes are LEFT IN PLACE in the token stream --
    // the existing post-parse import resolver (FF11 language-neutral JSON
    // descriptors) owns them. Declared here so the PP can RECOGNISE an angle
    // include and pass it through untouched (never mis-read it as a quote
    // include).
    std::string angleIncludeToken;

    // The token kind whose ADJACENT presence after a macro name marks a
    // FUNCTION-like `#define` (C's `(` -> "ParenOpen"). The macro engine reads
    // this to distinguish `#define F(x) ...` (function-like) from
    // `#define F (x)` (object-like, space before the paren). `(` is NOT a
    // core/builtin token kind -- it is a per-language config lexeme -- so it
    // MUST come from config, not a hard-coded name: a language whose paren
    // token is named differently would otherwise silently accept a
    // function-like define as an object macro (a silent miscompile). REQUIRED
    // + validated at load (mirrors quoteIncludeToken).
    std::string functionLikeOpenToken;

    // The token kind that CLOSES a function-like macro's parameter list AND its
    // call-site argument list (C's `)` -> "ParenClose"). The macro engine reads
    // this to (a) terminate the parameter-list parse in a function-like
    // `#define F(a,b) ...`, and (b) balance-track a call's argument list
    // (`F(x, g(y))` -- a nested `(` increments depth, this token decrements;
    // the matching depth-0 close ENDS the list). Like the opener, `)` is NOT a
    // core/builtin token kind (it lexes as core `Punctuation`, indistinguishable
    // from `,`/`;`), so it MUST come from config -- a language whose close-paren
    // is named differently would otherwise never find the list's end. REQUIRED
    // + validated at load (mirrors functionLikeOpenToken). Object-only-macro
    // languages still set it; it is unused unless a function-like define
    // appears.
    std::string functionLikeCloseToken;

    // The token kind that SEPARATES function-like macro parameters AND call-
    // site arguments (C's `,` -> "Comma"). The macro engine reads it to split
    // `#define F(a,b)` parameters and to split a call's top-level arguments
    // (`F(x, g(y))`). A `,` lexes as core `Punctuation` (indistinguishable from
    // `)`/`;` by core kind), so -- like the parens -- it MUST come from config:
    // a language whose argument separator is named differently would otherwise
    // mis-split (or never split) its argument lists. REQUIRED + validated at
    // load (mirrors functionLikeOpenToken). (FC13 cycle 2.)
    std::string functionLikeArgSeparatorToken;

    // The token kind that marks a VARIADIC function-like macro's
    // catch-all parameter (C's `...` -> "EllipsisOp"). The macro engine reads
    // it to RECOGNISE `#define V(...)` in parameter position (today: fail loud,
    // D-PP-VARIADIC-MACRO -- the `__VA_ARGS__` substitution is FC15-area). Like
    // every other PP-vocabulary token this is a per-language CONFIG lexeme, NOT
    // a hard-coded `...`: a second preprocess-opting language whose variadic
    // marker is spelled differently would otherwise have a word-like marker
    // silently accepted as a NAMED parameter (a silent mis-parse). OPTIONAL --
    // an empty string means the language declares NO variadic form (the engine's
    // `.valid()` guard then never treats any token as the marker). When present
    // it is `checkToken`-validated at load (C_UnknownToken) like the other
    // tokens. (FC13 cycle 2 review fold.)
    std::string variadicMarkerToken;

    // The IDENTIFIER that, inside a VARIADIC macro's replacement list, expands to
    // the trailing (un-named) arguments (C's `__VA_ARGS__` -> matched by TEXT,
    // like the directive WORDS define/undef/include, because it is an ordinary
    // identifier in the replacement, NOT a distinct token kind). The macro engine
    // substitutes a replacement `Word` whose text == this for the comma-joined
    // trailing-argument token sequence; the SAME identifier appearing in a
    // NON-variadic macro is a constraint violation (fail loud). Like the variadic
    // marker this is a per-language CONFIG spelling, NOT a hard-coded
    // `__VA_ARGS__`: a second preprocess-opting language whose catch-all
    // identifier differs is then substituted correctly (agnosticism). OPTIONAL
    // and only meaningful alongside `variadicMarkerToken`; empty means the
    // language declares no variadic catch-all identifier. When present it is
    // validated as a NON-EMPTY string at load (C_InvalidPreprocess). (FC13
    // cycle 3 -- D-PP-VARIADIC-MACRO.)
    std::string variadicArgsName;

    // FC15a (`#`/`##` operators): the token KIND of the STRINGIZE operator (C's
    // `#` -> "HashOp", C 6.10.3.2). In a function-like macro's REPLACEMENT list,
    // a `#` immediately followed by a parameter stringizes that parameter's RAW
    // (un-pre-expanded) argument into a single string literal. The macro engine
    // detects it by this token KIND -- which, in c-subset, is the SAME `HashOp`
    // as `directiveIntroToken`: directives are peeled at top level (firstOnLine)
    // BEFORE expansion, so every `#` a replacement list carries IS a stringize
    // operator (no ambiguity). Per-language CONFIG kind, never a hard-coded `#`:
    // a second preprocess-opting language whose stringize operator is spelled
    // differently is then detected correctly. OPTIONAL -- empty means the
    // language declares NO stringize operator (the engine's `.valid()` guard
    // never treats any token as `#`). `checkToken`-validated at load when
    // present (like `variadicMarkerToken`).
    std::string stringizeToken;

    // FC15a: the token KIND of the TOKEN-PASTE operator (C's `##` -> "HashHashOp",
    // C 6.10.3.3). In a replacement list, `a##b` concatenates the spelling of the
    // token to its left with the token to its right into a single new token
    // (re-tokenized + required to be exactly one token, C 6.10.3.3p3). A `##`
    // OPERAND that is a parameter uses the RAW argument. Detected by this token
    // KIND -- a DISTINCT lexeme from the single `#` (the loader/lexer's
    // longest-match wins `##` over two `#`), never hard-coded. Per-language
    // CONFIG kind: a second preprocess-opting language whose paste operator is
    // spelled differently is detected correctly. OPTIONAL -- empty means the
    // language declares NO paste operator. `checkToken`-validated at load when
    // present (like `variadicMarkerToken`).
    std::string pasteToken;
};

} // namespace dss
