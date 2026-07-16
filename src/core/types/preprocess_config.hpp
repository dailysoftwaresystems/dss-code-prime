#pragma once

#include "core/export.hpp"

#include <string>
#include <vector>

namespace dss {

// FC15b (`__FILE__`/`__LINE__`/`__STDC__`/...; C 6.10.8): how a PREDEFINED macro
// materializes its replacement. The engine dispatches ONLY on this kind, NEVER
// on the macro NAME (agnosticism: a language whose `__LINE__` is spelled
// differently still resolves correctly because the name is config and the
// behavior is keyed off the kind):
//   Line     -- the line number of the macro's INVOCATION (offset-derived via
//               the line-map), a decimal integer (C 6.10.8.1).
//   File     -- the presumed name of the current source FILE (offset-derived via
//               the line-map origin), a C string literal with `\`->`/` normalized.
//   Constant -- a STATIC integer-constant spelling carried verbatim in `value`
//               (`__STDC__`->"1", `__STDC_VERSION__`->"202311L", etc.).
//   Date     -- the translation DATE, a string literal `"Mmm dd yyyy"` computed
//               once at construction (C 6.10.8.1).
//   Time     -- the translation TIME, a string literal `"hh:mm:ss"` computed once.
enum class PredefinedMacroKind { Line, File, Constant, Date, Time };

// FC15b: one config-declared predefined macro (C 6.10.8). `name` is the macro
// identifier (matched by TEXT, like the directive words); `kind` selects the
// materialization behavior; `value` is the literal replacement spelling and is
// REQUIRED iff `kind == Constant` (ignored for the other kinds, whose value is
// derived at expansion time / construction).
struct DSS_EXPORT PredefinedMacroDef {
    std::string         name;
    PredefinedMacroKind kind = PredefinedMacroKind::Constant;
    std::string         value;
    // c105 (D-PP-FUNCTION-LIKE-PREDEFINE): OPTIONAL parameter list. A
    // params-bearing (`isFunctionLike`) predefine — e.g. the MSVC-profile
    // `__declspec(x)` → empty erase — is NOT seeded into `predefined_`;
    // it lowers to a `#define name(params) value` line in the synthetic
    // "<built-in>" PROLOGUE prepended to the synth stream, so the ordinary
    // directive handler owns param parsing, C 6.10.3p6 duplicate-param
    // rejection, 6.10.3p2 redefinition policy, and the function-like
    // arg-eating expansion (zero new expander machinery — the gcc built-in
    // model). Consequence: it is an ORDINARY macro (#undef-able); the
    // modeled construct is a compiler keyword by convention only.
    // `isFunctionLike` (not params.empty()) discriminates so a 0-ary
    // function-like (`F()`) stays expressible. Constant-kind only.
    std::vector<std::string> params;
    bool                isFunctionLike = false;
    // OPTIONAL per-object-format availability filter (mirrors the shipped-lib
    // descriptor `availableObjectFormats`). EMPTY ⇒ available on EVERY format
    // (the pre-existing behavior). A non-empty set of object-format NAMES
    // ("pe"/"elf"/"macho") restricts the macro to those formats — the
    // preprocessor seeds it only when the active format is in the set. This is
    // how an OS-selection macro like `_WIN32` is predefined for the pe target
    // ONLY, without leaking into elf/macho. Format names are validated at load
    // (an unknown name fails loud), never matched by an `if (name=="pe")` branch.
    std::vector<std::string> availableObjectFormats;
};

// FC15c (`__has_c_attribute` -- C23 6.10.1p4): one config-declared standard
// attribute the language KNOWS, with the C23 `__STDC_VERSION__`-style version
// integer it reports. `__has_c_attribute(name)` materializes `version` when the
// attribute is known, 0 otherwise. The set is config-driven (the engine never
// hard-codes an attribute name); a malformed entry fails LOUD at load
// (`C_InvalidPreprocess`). The lookup tries both `name` and the stripped form
// of a `__name__` dunder spelling (C 6.10.1: the operator ignores leading and
// trailing `__`), so a declared `deprecated` matches `__deprecated__` too.
struct DSS_EXPORT CAttributeDef {
    std::string name;       // the attribute identifier ("deprecated", ...)
    int         version = 0;  // the reported version int (> 0; e.g. 202311)
};

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

    // C23 (D-PP-ELIFDEF-ELIFNDEF; C 6.10.1): the `#elifdef` / `#elifndef`
    // directive WORDS. `#elifdef X` is exactly `#elif defined(X)` and
    // `#elifndef X` is exactly `#elif !defined(X)` (C 6.10.1p5), so the engine
    // routes them through the SAME conditional-group state machine as
    // `#elif`, evaluating the operand with the DIRECT `#ifdef`-style definedness
    // lookup (never the `#if` expression evaluator -- the operand is a bare
    // macro name, NOT expanded). Matched by lexeme TEXT against the token after
    // `#`, exactly like the required conditional words (which lex as plain
    // Identifier). OPTIONAL -- empty means the language declares NO C23
    // elifdef/elifndef form, so such a directive falls through to the generic
    // unsupported-directive fail-loud (never a silent branch skip). A language
    // that predates C23 (or a stripped config) leaves both empty and every
    // consumer site is provably, uniformly inert (mirrors the `pragmaDirective`
    // opt-in). The engine matches THESE strings, never a hard-coded spelling.
    std::string elifdefDirective;  // "elifdef"
    std::string elifndefDirective; // "elifndef"

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

    // FC15b (predefined macros; C 6.10.8): the language's PREDEFINED macros
    // (`__FILE__`/`__LINE__`/`__STDC__`/`__STDC_VERSION__`/`__STDC_HOSTED__`/
    // `__DATE__`/`__TIME__`). Each entry names the macro IDENTIFIER + a
    // materialization `kind` (+ a literal `value`, REQUIRED iff kind==Constant).
    // Pre-seeded into the macro expander at construction: an identifier that is
    // NOT a `#define`d macro but IS a predefined-macro name materializes its
    // configured value. The engine keys EVERY behavior off `kind`, never the
    // name (agnosticism). OPTIONAL -- an empty list (toy / tsql-subset, which
    // declare none) means the language has NO predefined macros, so e.g.
    // `__LINE__` stays an ordinary identifier (the identity-pass property).
    // `#define`/`#undef` of a predefined name is a constraint violation
    // (C 6.10.8.1) -> fail loud `P_PreprocessorPredefinedMacro`.
    std::vector<PredefinedMacroDef> predefinedMacros;

    // FC15c (`#pragma`; C 6.10.6): the PRAGMA directive WORD, matched by lexeme
    // TEXT against the token after `#` (like define/undef/include -- `pragma`
    // lexes as a plain Identifier, NOT a grammar keyword). The preprocessor
    // consumes-and-DROPS the whole `#pragma` line with NO error (C 6.10.6p2
    // licenses ignoring an unrecognized pragma; DSS recognizes none, so every
    // pragma is dropped). OPTIONAL -- empty means the language has NO `#pragma`
    // directive, so a `#pragma` line then hits the generic unsupported-directive
    // fail-loud (`P_PreprocessorUnsupported`). The engine matches THIS string,
    // never a hard-coded "pragma".
    std::string pragmaDirective;

    // FC15c (`__has_include`; C23 6.10.1p4): the `__has_include` OPERATOR
    // keyword, valid only inside a `#if`/`#elif` operand. `__has_include(<h>)` /
    // `__has_include("h")` tests whether the named header would be found by a
    // `#include` of the same form, yielding 1 or 0. Matched by lexeme TEXT (an
    // ordinary identifier in the operand, like `defined`), so a per-language
    // CONFIG spelling, never a hard-coded `__has_include`. OPTIONAL -- empty
    // means the language declares NO such operator (`__has_include` then folds
    // as an ordinary identifier -> 0, the identity property).
    std::string hasIncludeOperator;

    // FC15c (make-or-break agnosticism): the token KINDS that DELIMIT the angle
    // form of a `__has_include` argument (C's `<` -> "LtOp", `>` -> "GtOp"). The
    // `__has_include(<h>)` extraction matches the angle delimiters by SCHEMA
    // KIND, NEVER by scanning for the literal `<`/`>` characters (input
    // classification by hard-coded byte is the exact agnosticism trap this
    // config forbids -- see `functionLikeOpenToken`). REQUIRED-together-with
    // `hasIncludeOperator`: a language declaring the operator WITHOUT both angle
    // tokens is a self-inconsistent contract -> LOAD-ERROR (`C_InvalidPreprocess`).
    // `checkToken`-validated when present (like `stringizeToken`).
    std::string hasIncludeAngleOpenToken;
    std::string hasIncludeAngleCloseToken;

    // FC15c (`__has_c_attribute`; C23 6.10.1p4): the `__has_c_attribute`
    // OPERATOR keyword, valid only inside a `#if`/`#elif` operand.
    // `__has_c_attribute(attr)` yields the version int of a KNOWN standard
    // attribute (from `knownCAttributes`) or 0. Matched by lexeme TEXT (like
    // `defined`/`__has_include`), a per-language CONFIG spelling. OPTIONAL --
    // empty means the language declares NO such operator (folds to 0).
    std::string hasCAttributeOperator;

    // FC15c: the standard attributes the language KNOWS + their reported version
    // ints (C23 6.10.1p4). Only meaningful alongside `hasCAttributeOperator`.
    // Each entry's `name` must be non-empty and `version` > 0 (a malformed entry
    // -> `C_InvalidPreprocess` at load). OPTIONAL -- empty means NO attribute is
    // known (every `__has_c_attribute(x)` then yields 0).
    std::vector<CAttributeDef> knownCAttributes;

    // FC17.9(h) (`#embed`; C23 6.10.4 / N3096 6.10.3): the `#embed` directive
    // WORD, matched by lexeme TEXT against the token after `#` (like
    // define/undef/include -- `embed` lexes as a plain Identifier, NOT a grammar
    // keyword). A `#embed "resource"` directive resolves the QUOTED binary
    // resource EXACTLY as a quote-`#include` would (self-dir first, then the
    // include dirs) and expands to the resource's bytes as a comma-separated list
    // of decimal integer constants (C23: constants of type `int` in
    // [0, 2^CHAR_BIT)). OPTIONAL -- empty means the language declares NO `#embed`
    // directive, so an `#embed` line falls through to the generic
    // unsupported-directive fail-loud (`P_PreprocessorUnsupported`; the
    // `pragmaDirective`/`elifdefDirective` opt-in model). The engine matches
    // THIS string, never a hard-coded "embed".
    std::string embedDirective;    // "embed"

    // FC17.9(h) (`__has_embed`; C23 6.10.1): the `__has_embed` OPERATOR keyword,
    // valid only inside a `#if`/`#elif` operand. `__has_embed("resource")` tests
    // whether the resource a `#embed` of the same form would read exists, yielding
    // the C23 trichotomy `__STDC_EMBED_NOT_FOUND__`(0) / `__STDC_EMBED_FOUND__`(1)
    // / `__STDC_EMBED_EMPTY__`(2). Matched by lexeme TEXT (like `defined` /
    // `__has_include`), a per-language CONFIG spelling. OPTIONAL -- empty means
    // the language declares NO such operator (`__has_embed` then folds as an
    // ordinary identifier -> 0, the identity property).
    //
    // ANGLE-form self-consistency (D-PP-EMBED, FIX-3, deliberate): unlike
    // `hasIncludeOperator` (which REQUIRES both angle-delimiter tokens at load,
    // since its angle form is supported), `hasEmbedOperator` imposes NO such
    // load-time requirement. The `#embed <resource>` / `__has_embed(<resource>)`
    // ANGLE form is a cycle-1 loud DEFERRAL (D-PP-EMBED-ANGLE): DSS ships JSON
    // descriptors, not binary resources, on the system path, so an angle embed
    // resolves nothing. `__has_embed` reuses the language's existing
    // `hasIncludeAngleOpenToken`/`hasIncludeAngleCloseToken` KINDS to RECOGNISE an
    // angle argument only so it can answer 0 truthfully; a language that declares
    // `hasEmbedOperator` without them cannot lex an angle argument at all and its
    // `__has_embed(<...>)` fails loud at RUNTIME as a malformed argument -- which
    // suffices precisely because the angle form is deferred (no self-inconsistent
    // contract to guard against at load).
    std::string hasEmbedOperator;  // "__has_embed"

    // FC15 paste residuals (D-PP-VARIADIC-GNU-COMMA-ELISION): opt into the GNU
    // `,##__VA_ARGS__` extension. When TRUE, a `separator ## __VA_ARGS__` whose
    // variadic part expands to EMPTY drops the preceding separator entirely (so
    // with `#define LOG(fmt, ...) f(fmt, ## __VA_ARGS__)`, `LOG("x")` -> `f("x")`);
    // a NON-empty `__VA_ARGS__` keeps the separator and does NOT paste. When FALSE
    // (the default), standard C placemarker behavior applies and the separator
    // survives (`sep ## <placemarker>` = `sep`). The separator is matched by the
    // config-declared `functionLikeArgSeparatorToken` KIND and `__VA_ARGS__` by
    // `variadicArgsName` -- never a hardcoded `,` byte or name. A language that does
    // not opt in (every non-C grammar) leaves this FALSE -> no behavior change.
    bool variadicCommaElision = false;
};

} // namespace dss
