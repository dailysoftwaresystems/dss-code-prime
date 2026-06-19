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
};

} // namespace dss
