#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
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

// TF-C83 (D-CSUBSET-TOOLCHAIN-IDENTITY-PREDEFINES). Pack a dot-separated version ("0.0.2")
// into ONE integer using `weights` (most-significant first, last entry 1):
// sum(component[i] * weights[i]). With [1000000, 1000, 1] this is the
// GCC_VERSION encoding, so the result ORDERS correctly — 0.0.2 (2) < 0.1.0
// (1000) < 1.0.0 (1000000) — so a `#if`-time `>=` against the resulting macro
// behaves as read. Which macro that is stays CONFIG's business; the engine
// knows only the `version` kind and the `componentWeights` key.
//
// Returns the packed value, or an ERROR MESSAGE. Every rejection is a case
// where the encoding would otherwise be silently WRONG rather than merely
// unusual:
//   * malformed version text (empty field / non-digit / absurd component);
//   * component count != weight count (the config describes a different
//     version shape than the build actually has);
//   * a component that REACHES its derived bound weights[i-1]/weights[i] —
//     e.g. 0.0.1000 under [1000000,1000,1] packs to 1000, indistinguishable
//     from 0.1.0. The bound is read off the weights the CONFIG declared; no
//     magic 1000 exists in the engine, so a different declared encoding is
//     bounded correctly for free.
//
// Split out of the `version`-kind loader specifically so these paths are
// reachable from unit tests with an ARBITRARY version string — baking the
// build's own version in would make them testable only by editing the repo's
// VERSION file and reconfiguring, which in practice means untested.
[[nodiscard]] DSS_EXPORT std::expected<long long, std::string>
packVersionComponents(std::string_view           versionText,
                      std::span<const long long> weights);

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
// ── TF-C82: the `#pragma` / `_Pragma` effect registry (C 6.10.6 / 6.10.9) ──
//
// The CLOSED verb set a `pragmaEffects` row may declare. Mirrors
// `AttributeEffect` exactly — one table, `static_assert`ed complete at the
// switch, and an unknown verb is a LOAD error (`C_InvalidPreprocess`) so a typo
// can never silently disarm a row.
//
// ★ WHY THREE FLAVOURS OF "DO NOTHING" RATHER THAN ONE. They make DIFFERENT
// claims, and the whole reason this registry exists is that "the compiler was
// silent" is not a claim at all:
//
//  • `DiagnosticsOnly` asserts the pragma's ENTIRE effect is to configure
//    diagnostics of a compiler DSS is not, AND that DSS emits none of them. That
//    is checkable — `clang diagnostic push/pop/ignored` tunes clang warnings;
//    `clang assume_nonnull begin/end` scopes NULLABILITY qualifiers DSS neither
//    parses nor diagnoses. Both are inert HERE for a stated reason, not by
//    accident. (MEASURED reached in the sqlite corpus: 48 `clang diagnostic`
//    + 24 `clang assume_nonnull` occurrences.)
//  • `AnnotationOnly` asserts the pragma is pure authoring metadata with no
//    translation semantics in ANY compiler (`#pragma mark` is an IDE bookmark).
//  • `RealizationRequestOnly` (TF-C85) asserts something NEITHER of the two
//    above can say, and is why it needed its own verb rather than being folded
//    into one of them. MSVC `#pragma intrinsic(f, g)` DOES concern translation —
//    it is not a diagnostic knob and not metadata — but its entire content is a
//    request about HOW a listed name is realized (inline expansion instead of a
//    CRT call), never a claim that the name EXISTS or a change to what calling
//    it means. DSS chooses realization itself: a name it lowers as a builtin
//    never becomes a CRT call, and a name it does not provide fails loud at the
//    CALL SITE (`S_UnknownIdentifier`) whether or not the pragma was honored. So
//    ignoring the pragma cannot mask a missing symbol and cannot change program
//    behavior. That is a checkable claim about the ENGINE, not about the
//    pragma's arguments — which matters because the match is by PREFIX, so ONE
//    row makes ONE claim covering EVERY name the pragma can list, including
//    names this implementation has never heard of.
//  • `Unsupported` asserts the pragma DOES have translation semantics that DSS
//    has not implemented — so it is LOUD, and (like `#error`) unsuppressable:
//    silencing a real semantic effect is how a wrong-layout/wrong-code artifact
//    ships green.
//  • `IncludeOnce` (TF-C87) asserts the pragma declares its FILE include-once
//    (C's `#pragma once`). It is a REFINEMENT of `Unsupported`, not an escape
//    from it: DSS does not implement include-once dedup, so `applyPragma` is
//    just as LOUD on this verb as on `Unsupported`. What the separate verb buys
//    is that ONE OTHER reader — the include-guard detector
//    (D-PP-INCLUDE-REENTRY-GUARD-AWARE) — can tell "this header's include-once
//    mechanism is a pragma I don't implement" apart from "this header carries no
//    include-once mechanism at all". Those are different facts about the user's
//    code and they need different messages; without the verb the detector would
//    have to recognise the WORD `once`, which is exactly the identity branch the
//    registry exists to forbid.
//
// `StructPacking` and `OptimizerControl` are the verbs with a real sink:
// `#pragma pack` drives the member-alignment CAP into the composite layout
// channel; `#pragma optimize` drives a per-function optimizer opt-out into
// `MirFunc.noOptimize`.
enum class PragmaEffect : std::uint8_t {
    // Configures another compiler's diagnostics; DSS emits none of them.
    DiagnosticsOnly,
    // Authoring/IDE metadata with no translation semantics anywhere.
    AnnotationOnly,
    // TF-C85: the pragma only requests HOW a name it LISTS is realized — never
    // WHETHER that name exists. See the long argument on the enumerator below.
    RealizationRequestOnly,
    // C `#pragma pack` — a LEXICALLY SCOPED maximum member alignment. The one
    // verb with a layout sink (`TypeInterner`'s `maxFieldAlign` channel).
    StructPacking,
    // TF-C85: MSVC `#pragma optimize("", on|off)` — a LEXICALLY SCOPED
    // per-function optimizer opt-out. The second verb with a real sink.
    OptimizerControl,
    // Real translation semantics DSS has NOT implemented → loud + unsuppressable.
    Unsupported,
    // TF-C87: the pragma declares its FILE include-once (C's `#pragma once`).
    // Still loud at `applyPragma` (DSS implements no include-once dedup); the
    // verb exists so the include-guard detector can NAME the mechanism instead
    // of reporting "no include guard detected". See the argument above.
    IncludeOnce,
};

// One registry row: the leading WORD(S) that identify a pragma, and what DSS
// does with it. The LONGEST matching prefix wins.
struct DSS_EXPORT PragmaEffectRow {
    std::vector<std::string> prefix;   // e.g. {"clang","diagnostic"} or {"pack"}
    PragmaEffect             effect = PragmaEffect::Unsupported;
};

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
    // lexes as a plain Identifier, NOT a grammar keyword). OPTIONAL -- empty
    // means the language has NO `#pragma` directive, so a `#pragma` line then
    // hits the generic unsupported-directive fail-loud
    // (`P_PreprocessorUnsupported`). The engine matches THIS string, never a
    // hard-coded "pragma".
    //
    // ★★ TF-C82 — WHAT THIS FIELD USED TO MEAN, AND WHY THAT WAS A SILENT DROP.
    // From FC15c until TF-C82 a recognized `#pragma` line was consumed and
    // DROPPED with no diagnostic and no tokens, justified by C 6.10.6p2 ("an
    // unrecognized pragma may be ignored"). The justification was sound for a
    // pragma that DOES nothing and false for one that does: MEASURED on this
    // Mac, the sqlite corpus REACHES 40 `#pragma pack` lines across 5 TUs, and
    // `sys/fcntl.h`'s `#pragma pack(4)` region makes `struct log2phys` 20 bytes
    // / align 4 where the unpacked layout is 24 / 8 — a wrong-ABI struct handed
    // to a live `fcntl(F_LOG2PHYS)` syscall. 6.10.6p2 licenses IGNORING a
    // pragma; it does not license claiming to have ignored one that changed the
    // layout. So recognition is now a REGISTRY (`pragmaEffects`) and the
    // unknown case is LOUD (`unknownPragmaIsError`).
    std::string pragmaDirective;

    // TF-C82 (`_Pragma`; C 6.10.9): the PRAGMA OPERATOR word, matched by lexeme
    // TEXT (an ordinary identifier, like `defined`/`__has_include`). `_Pragma(
    // "string-literal")` is EXACTLY equivalent to a `#pragma` line whose
    // pp-tokens are the DE-STRINGIZED literal (6.10.9p1: delete the `L` prefix
    // if present, delete the outer quotes, replace `\"` with `"` and `\\` with
    // `\`), so it routes through the SAME `pragmaEffects` registry — one
    // registry, two spellings. OPTIONAL: empty ⇒ the language has no pragma
    // operator and `_Pragma` is an ORDINARY identifier (toy / tsql — pinned).
    //
    // ★ IT RESOLVES AT EXPANSION TIME, NOT AT THE DIRECTIVE SCAN. `_Pragma` is
    // an operator in the token stream, not a directive, so it can (and in the
    // Apple SDK routinely does) arrive from inside a macro REPLACEMENT LIST:
    // `sys/queue.h`'s `__NULLABILITY_COMPLETENESS_PUSH/POP` are exactly that
    // shape, expanded at 40 use sites. Handling it only where directives are
    // scanned would leave a file-scope `_Pragma` green while every macro-borne
    // one silently vanished — the halfway state this field's test battery
    // discriminates on.
    std::string pragmaOperator;

    // TF-C82: what a `#pragma` / `_Pragma` whose leading word(s) match NO
    // `pragmaEffects` row does. TRUE (the C-subset posture) ⇒ fail loud
    // (`P_PreprocessorPragma`). FALSE ⇒ silently ignored, the pre-TF-C82
    // behavior, kept as a deliberate OPT-OUT rather than deleted: C 6.10.6p2
    // genuinely permits it, and a language whose pragma surface is provably
    // inert may say so. Default FALSE so a language that declares no registry
    // at all is unchanged (a config must OPT IN to loudness).
    //
    // ★ THE POINT OF THE PAIR IS THAT SILENCE BECOMES A CLAIM. Before TF-C82
    // every pragma was silently dropped and nothing distinguished "we checked
    // and it is inert" from "we never looked". With the registry, an ignored
    // pragma is ignored because a ROW SAYS SO, and anything else is loud.
    bool unknownPragmaIsError = false;

    // TF-C82: the pragma REGISTRY — the `attributeEffects` house pattern, keyed
    // by the pragma's leading WORD(S) rather than by a single name (a real
    // pragma's identity is a prefix: `clang diagnostic push` is a `clang
    // diagnostic` pragma). The LONGEST matching prefix wins, so a future
    // `["clang","diagnostic","push"]` row could refine `["clang","diagnostic"]`
    // without either row being shadowed by declaration order.
    //
    // OPTIONAL: an empty registry with `unknownPragmaIsError == false` is
    // byte-for-byte the pre-TF-C82 drop-everything behavior.
    std::vector<PragmaEffectRow> pragmaEffects;

    // TF-C82: the `structPacking` operand SUB-VOCABULARY — the two words that
    // select the STACK forms of C's `#pragma pack` (`pack(push, N)` /
    // `pack(pop)`), matched by lexeme TEXT. The depth-less SET/RESET forms
    // (`pack(N)` / `pack()`) need no vocabulary at all: they are recognized
    // structurally (one integer operand, or none).
    //
    // ★ WHY THESE ARE CONFIG AND `N` IS NOT. The engine owns the SEMANTIC of the
    // `structPacking` verb — a maximum member alignment, its push/pop discipline,
    // its power-of-two rule — exactly as `AttributeEffect::Align` owns
    // `aligned(N)`'s. What it must never own is a WORD: a literal `"push"` in
    // engine code is an identity branch on source vocabulary, the same class of
    // hard-coding `pragmaDirective` exists to prevent. OPTIONAL and independent:
    // a language declaring `structPacking` but NOT these words gets the set/reset
    // forms and a LOUD refusal of the stack forms (never a silent no-op) — which
    // is also the red-on-disable pin for this pair.
    //
    // MEASURED necessity: the reached sqlite corpus uses BOTH idioms — 13
    // `pack(4)` + 1 `pack(1)` closed by 14 `pack()`, and 5 `pack(push, 4)` + 1
    // `pack(push, 1)` closed by 6 `pack(pop)`. Building only one is insufficient.
    std::string pragmaPackPushWord;
    std::string pragmaPackPopWord;

    // TF-C85: the `optimizerControl` operand SUB-VOCABULARY — the two words that
    // OPEN and CLOSE an MSVC `#pragma optimize("", off) … #pragma optimize("",
    // on)` region, matched by lexeme TEXT. Same discipline as the `pack`
    // push/pop pair above and for the same reason: the engine owns the SEMANTIC
    // (a lexically scoped per-function optimizer opt-out), config owns the WORD.
    //
    // OPTIONAL and independent. A language declaring `optimizerControl` but NOT
    // these words gets a LOUD refusal of every `#pragma optimize` form (never a
    // silent no-op) — which is this pair's red-on-disable pin.
    std::string pragmaOptimizeOnWord;
    std::string pragmaOptimizeOffWord;

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

    // TF-C59 (`#line`; C23 6.10.4 / D-CPP-LINE-DIRECTIVE): the LINE-CONTROL
    // directive keyword. `#line digits ["file"]` sets the PRESUMED line (and
    // optionally the presumed file name) reported by `__LINE__`/`__FILE__` and
    // by diagnostic positions from the FOLLOWING line onward. The operands may
    // themselves be macro-invocations (6.10.4p4), in which case they are
    // macro-expanded first and the RESULT must match one of the two forms.
    //
    // Why this is not optional in practice: every C-generating tool emits it
    // (lemon, bison, flex, re2c, protoc) so the compiler's diagnostics point at
    // the GENERATOR's input rather than the generated file. SQLite's own
    // `parse.c` (lemon) carries 50 of them — the amalgamation only works today
    // because `mksqlite3c.tcl` STRIPS them while generating `sqlite3.c`.
    //
    // OPTIONAL -- empty means the language declares NO `#line`, so a `#line`
    // falls through to the generic unsupported-directive fail-loud
    // (`P_PreprocessorUnsupported`; the `pragmaDirective`/`embedDirective`
    // opt-in model). The engine matches THIS string, never a hard-coded "line".
    std::string lineDirective;     // "line"

    // D-CPP-ERROR-WARNING (`#error`; C23 6.10.5 / `#warning`; C23 6.10.6): the
    // DIAGNOSTIC directive words, matched by lexeme TEXT against the token after
    // `#` (like define/undef/include/line -- `error`/`warning` lex as plain
    // Identifiers, NOT grammar keywords). A REACHED `#error` emits
    // `P_PreprocessorErrorDirective` at Error severity (C23 6.10.5p1: a
    // constraint -- the implementation shall diagnose and the translation unit
    // is invalid); a reached `#warning` emits `P_PreprocessorWarningDirective` at
    // Warning severity and translation CONTINUES. Both messages carry the
    // directive's `pp-tokens` VERBATIM -- the operand is never macro-expanded
    // (gcc/clang agree; the text is prose, not a macro invocation site) and is
    // OPTIONAL (`pp-tokens_opt`), so a bare `#error` is well-formed and still
    // fires.
    //
    // OPTIONAL -- empty means the language declares NO such directive, so the
    // line falls through to the generic unsupported-directive fail-loud
    // (`P_PreprocessorUnsupported`; the `pragmaDirective`/`embedDirective`/
    // `lineDirective` opt-in model). The engine matches THESE strings, never a
    // hard-coded "error"/"warning" -- rebinding the word in the config rebinds
    // the feature, and stripping the field restores the fail-loud fallback.
    //
    // ★ Both are dispatched BELOW `handleDirective`'s `if (!stackActive())`
    // dead-branch gate -- the `#pragma`/`#embed`/`#line` parity -- so an `#error`
    // inside a NOT-TAKEN `#if` branch is entirely SILENT (C 6.10p1: a skipped
    // group is parsed only far enough to track nesting). That is load-bearing,
    // not cosmetic: the Apple SDK headers guard unsupported configurations with
    // an `#error` inside branches a supported target skips, so a recognise-on-lex
    // implementation could not compile a single macOS translation unit.
    std::string errorDirective;    // "error"
    std::string warningDirective;  // "warning"

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

// ── TF-C86 (D-CSUBSET-STDARG-F001A): the conditional-inclusion OPERATOR names ──
//
// A language's `#if`-only operators (`__has_include`, `__has_embed`,
// `__has_c_attribute`) are IMPLEMENTATION-OWNED IDENTIFIERS, not ordinary
// undefined names. Two consequences a preprocessor MUST honor, and which DSS
// did not until TF-C86:
//
//   (1) `#ifdef`/`#ifndef`/`#elifdef`/`#elifndef`/`defined()` see them as
//       DEFINED. MEASURED on the host clang
//       (`clang -std=c2x -E`): `#ifdef __has_include` -> taken;
//       likewise `__has_embed`, `__has_c_attribute`. This is the entire point
//       of the ubiquitous portability shim
//           #ifndef __has_include
//           #define __has_include(x) 0
//           #endif
//       (Apple SDK `sys/cdefs.h:91-93`, and the same three lines in glibc,
//       musl, Boost, zlib, ...): the guard is DEAD on a compiler that has the
//       operator. Reading the name as undefined takes the arm and SHADOWS the
//       real operator with a function-like macro that answers 0 forever.
//       MEASURED consequence before this predicate existed: every
//       `#if __has_include(<mach/…>)`-guarded include in `malloc/_platform.h`
//       became `F001A` — not because the header was missing (it is right there
//       in the SDK) but because the pre-scan saw a function-like macro in the
//       guard, went conservative-uncertain, skipped the angle SOURCE splice,
//       and the post-parse import resolver then hard-failed the surviving
//       directive as a missing system header.
//
//   (2) They are NOT names a program may `#define` or `#undef` (C23 6.10.1).
//       Honoring such a redefinition would make `#include <h>` and
//       `__has_include(<h>)` disagree — a silent miscompile — so DSS refuses
//       it LOUDLY (`P_PreprocessorOperatorNameNotDefinable`).
//
// `definedOperator` is deliberately NOT a member of this set: `defined` is an
// operator spelling, not a macro name. MEASURED on the same clang:
// `#ifdef defined` is NOT taken.
//
// Config-driven throughout — the set is whatever spellings THIS language
// declares, so a grammar that names its operator something else is covered and
// one that declares none has an empty set. No hard-coded `__has_include`.
[[nodiscard]] inline bool
isConditionalInclusionOperator(std::string_view        name,
                               PreprocessConfig const& cfg) noexcept {
    return (!cfg.hasIncludeOperator.empty()    && name == cfg.hasIncludeOperator)
        || (!cfg.hasEmbedOperator.empty()      && name == cfg.hasEmbedOperator)
        || (!cfg.hasCAttributeOperator.empty() && name == cfg.hasCAttributeOperator);
}

} // namespace dss
