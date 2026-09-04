#pragma once

// FC14 (D-PP-CONDITIONAL-COMPILATION): the C-preprocessor `#if` / `#elif`
// integer-constant-expression (ICE) evaluator (C 6.10.1). Given the operand
// tokens of an `#if`/`#elif` line (everything after the directive word, up to
// the newline), it returns the operand's compile-time TRUTH VALUE -- true =
// "branch taken" (D-PP-IF-UNSIGNED-INTMAX; see `evaluateIfExpression` for why an
// int64 return was not merely redundant but unrepresentable).
//
// Pipeline (C 6.10.1p1/p4), in order:
//   1. MACRO-EXPAND the whole operand (via the `macroExpand` callback -- the
//      SAME `MacroExpander::expand` engine the rest of the preprocessor uses, so
//      object/function-like macros in an `#if` operand expand identically), with
//      a `PpIfOperandBarrier` (below) holding back exactly the operands the
//      reference toolchains hold back, PER OPERATOR.
//   2. Rewrite the conditional-inclusion OPERATORS to their values, ONCE, over
//      the expanded run: `defined X` / `defined(X)` -> 1/0 (via `isDefined`),
//      `__has_include` / `__has_embed` -> their resolutions, `__has_c_attribute`
//      -> its version.
//      ★ EXPANSION FIRST, OPERATORS SECOND, and that order is the whole design
//      (D-PP-DEFINED-VIA-MACRO-EXPANSION). It used to be the other way round,
//      which is precisely what made an operator PRODUCED BY expansion
//      unreachable. Both forms now take the SAME path and cannot answer
//      differently -- see the big note on `foldDefined` in pp_if_eval.cpp for
//      the measurement behind it.
//   3. Any identifier that SURVIVES expansion (not an integer literal, not an
//      operator) -> integer 0 (C 6.10.1p4).
//   4. The resulting token run is parsed + folded as an integer-constant-
//      expression via a precedence-climbing parser whose precedence +
//      associativity come from `schema.operatorTable()` and whose arithmetic
//      reuses the SHARED const-eval core (`const_eval_arith.hpp`) + operator
//      seams (`const_eval_operators.hpp`) -- never a private arithmetic copy.
//
// The whole thing reuses the existing const-eval substrate so the `#if`
// evaluator and the array-dimension / enum CST evaluator can never disagree on
// "what 1+2*3 folds to". It is config-driven end to end: the `defined` keyword,
// the parens, every operator's precedence + mapping are read from the schema.
//
// Fail-loud (NO silent miscompile):
//   * a malformed expression (missing operand, unbalanced paren, a non-integer
//     where a value is required) -> P_PreprocessorDirective, returns nullopt;
//   * the rejected `#if` subset (sizeof / a float or string literal /
//     assignment / comma / cast) -> P_PreprocessorUnsupported, returns nullopt;
//   * a div-by-zero / shift-out-of-range / overflow during folding ->
//     P_PreprocessorDirective, returns nullopt (MF-5).
// A nullopt return means "the branch condition could not be evaluated"; the
// caller has already emitted the positioned diagnostic, so it treats nullopt as
// false (the branch is not taken) and continues.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

// Macro-expand a token run with the preprocessor's existing expander (object +
// function-like macros, hide-set-precise). The callback is supplied by the
// MacroExpander so the evaluator reuses ONE expansion engine.
using PpMacroExpand =
    std::function<std::vector<Token>(std::vector<Token> const&)>;

// Test a macro name for definedness (C's `defined X`). Supplied by the
// MacroExpander (queries its macro table).
using PpIsDefined = std::function<bool(std::string_view)>;

// FC15c (`__has_include`; C23 6.10.1p4): test whether the header `filename`
// would be found by a `#include` of the same form. `isAngle` selects the form:
// true = `<filename>` (the angle / system search -- DSS maps `<stem>.json` on
// the system path); false = `"filename"` (the quote search -- self-dir +
// includeDirs). The raw filename spelling is passed verbatim (escapes are NOT
// decoded, mirroring the include resolver). Supplied by the MacroExpander, which
// holds the include search paths. Returns true iff the header exists.
using PpHasInclude = std::function<bool(std::string_view filename, bool isAngle)>;

// FC17.9(h) (`__has_embed`; C23 6.10.1): test whether the resource `filename`
// that a `#embed` of the same form would read exists, returning the C23
// trichotomy: 0 = `__STDC_EMBED_NOT_FOUND__`, 1 = `__STDC_EMBED_FOUND__`
// (non-empty), 2 = `__STDC_EMBED_EMPTY__` (found but zero bytes). `isAngle`
// selects the form (true = `<r>`, the deferred angle form -> the callback
// answers 0 truthfully; false = `"r"`, the quote search). `opSpan` is the
// operator token's span, so the authoritative callback can derive the
// per-origin resolution directory (the resource search is relative to the file
// containing the `__has_embed`, exactly as `#embed` resolves) -- so
// `__has_embed` answers precisely what `#embed` would do at that spot. Supplied
// by the MacroExpander, which holds the resource search paths. An UNSET
// callback (`{}`) is null-tolerant and mints 0 (NOT_FOUND) -- the same
// null-callback tolerance the `hasInclude && hasInclude(...)` site uses.
using PpHasEmbed =
    std::function<int(std::string_view filename, bool isAngle, SourceSpan opSpan)>;

// FC15b: the MacroExpander's accumulated `#`/`##`/predefined PRODUCT text, as it
// stands AFTER `macroExpand` runs over an `#if` operand. A predefined macro
// (`__STDC_VERSION__` &c.) or a `#`/`##` product expanded inside a `#if`
// controlling expression materializes a token whose span points into the synth
// buffer's product TAIL (`[prefixLen + ..)`), which is NOT yet appended to the
// prefix-only `synth` buffer at #if-eval time. This provider returns that tail,
// which the ICE parser slices a product-span token out of DIRECTLY — the tail is
// BORROWED for the duration of the call, never copied, so the bytes it names
// must outlive `evaluateIfExpression` and must not be appended to during it
// (both callers own the storage and stop appending before the tail is taken).
// Returns an empty view when the language produces no products.
//
// ★ IT USED TO SAY "so the evaluator can assemble a COMBINED (prefix + product)
// buffer", and it did — copying the WHOLE translation unit on every `#if` after
// the TU's first product. See the
// D-PERF-PP-IF-REMATERIALIZES-THE-WHOLE-SYNTH-BUFFER-PER-EVALUATION note in
// `evaluateIfExpression` for the measurement that removed it.
using PpProductText = std::function<std::string_view()>;

// D-PP-HAS-EXTENSION-BUILTIN-ABSENT: "has the program `#undef`'d this
// implementation-provided operator away?" A conditional-inclusion or
// feature-query operator lives in the CONFIG, not in the macro table, so the
// evaluator cannot learn from `isDefined` alone that the name has been revoked —
// and a fold that still fired after an `#undef` would be a diagnostic with no
// effect, which is worse than the refusal the ruling replaced.
//
// ★ IT ANSWERS FOR THE OPERATOR WORD, NEVER FOR A MACRO NAME. `isDefined`
// already covers the macro table; this asks only whether the CONFIG-declared
// operator is still live. Null-callback tolerant: an unset `{}` means nothing
// was ever revoked, which is the correct answer for every test caller and for
// any language whose operators cannot be `#undef`'d (`reservedIdentifiers`
// declaring `refuse`).
using PpOperatorRevoked = std::function<bool(std::string_view)>;

// ── D-PP-DEFINED-VIA-MACRO-EXPANSION ─────────────────────────────────────────
// THE `#if`-OPERAND BARRIER: the ONE definition of "which tokens a
// conditional-inclusion operator protects from macro expansion", shared by the
// `#if` evaluator (which folds the operators to values) and by BOTH
// `#if`-operand expansion engines (which must copy a protected run VERBATIM).
//
// ★ WHY A BARRIER AND NOT A REWRITE PASS ALONE. Every operator is folded AFTER
// macro expansion (see `foldDefined` and the operator loop beside it in
// pp_if_eval.cpp for why that order is the references' own), which means the
// operands are exposed to the expander
// before the operators ever run, and an unprotected operand is simply GONE by
// then: `#define BAR 0` + `#define HAS_BAR defined(BAR)` rescans to
// `defined ( 0 )`, which is not a syntax error to be reported but a DIFFERENT
// QUESTION, silently answered. Operands have to be protected AT THE MOMENT the
// expander reaches them — inside the expansion, not after it.
//
// ★ DRIVE IT OVER THE STREAM THE EXPANDER PROCESSES, NOT OVER ITS INPUT. The
// state machine observes each token at the moment the expander reaches it —
// including tokens a splice has just introduced — which is what makes the
// barrier compose ACROSS the replacement-list boundary: `#define D defined` then
// `#if D(FOO)`, and `#define HI __has_include` then `#if HI(<stdio.h>)`, each
// produce the keyword from one construct and the operand from another, and only
// an output-driven barrier joins them (✔MEASURED: all three references answer
// the operator, 1 and 1).
//
// ★★ THE PROTECTION RULE IS PER-OPERATOR, AND EACH ARM IS MEASURED, NOT
// REASONED. ✔2026-08-27, gcc 13.3.0 `-std=c2x -pedantic`, clang 18.1.3
// `-std=c23 -pedantic`, MSVC 19.51 `/std:c17 /W4` (traditional AND
// `/Zc:preprocessor`) — unanimous on every `__has_include` shape:
//
//   `defined`                     ALWAYS protects its operand.
//     `#define BAR 0` + `#define HAS_BAR defined(BAR)` -> 1 on all three, so the
//     operand is not expanded even when it is a macro with a value.
//
//   `__has_include` / `__has_embed`   protect the operand ONLY when it ALREADY
//     opens with the angle or the quote delimiter; otherwise the operand IS
//     macro-expanded and re-examined — C's own `#include MACRO` rule (C23
//     6.10.1p4 defers to 6.10.2). Both halves measured:
//       `#define HDR stdio` + `#if __has_include(<HDR.h>)` -> 0 on all three
//         (the `<...>` interior is NOT expanded; it looks for a header literally
//         named `HDR.h`), and
//       `#define H <stdio.h>` + `#if __has_include(H)`     -> 1 on all three
//         (an operand matching NEITHER form IS expanded, then re-examined).
//     Protecting the second shape would answer 0 where every reference answers
//     1; expanding the first would answer 1 where every reference answers 0.
//
//   `__has_c_attribute`           NEVER protects — its operand IS expanded.
//     `#define A deprecated` + `#if __has_c_attribute(A)` -> the attribute's
//     version on gcc AND clang (MSVC `/std:c17` does not implement the operator
//     at all, so it folds the name to 0; that is the operator's ABSENCE, not a
//     third opinion about operand expansion). So this operator is simply not a
//     state in the machine below.
//
// ⚠ THE BARRIER IS DEPTH-0 ONLY — it must NOT reach argument pre-expansion.
// ✔MEASURED: `#define ID(a) a` + `#if ID(defined(FOO))` is an ERROR in gcc
// ("operator \"defined\" requires an identifier"), clang ("macro name must be an
// identifier") AND MSVC ("C2004: expected 'defined(id)'"), i.e. the argument IS
// pre-expanded and `defined(1)` then fails loud. Protecting it would ACCEPT what
// no reference accepts. (The same shape with `__has_include` is ACCEPTED by all
// three — `#if ID(__has_include(<stdio.h>))` -> 1 — and falls out correctly
// without a special case, because nothing in `__has_include(<stdio.h>)` is a
// macro for the pre-expansion to damage.)
class PpIfOperandBarrier {
public:
    // Every spelling and every token KIND comes from the schema, so there is ONE
    // construction site for this vocabulary and no caller can spell `(` or `<`
    // itself. A language that declares no `defined` / `__has_include` /
    // `__has_embed` operator leaves those spellings EMPTY and gets a provably
    // inert barrier — the opt-out identity property the rest of this file's
    // operator arms carry.
    explicit PpIfOperandBarrier(GrammarSchema const& schema);

    // Advance over `t` and answer whether it is INSIDE a protected operand run.
    // `word` is `t`'s spelling when it is a Word and empty otherwise — passed in
    // because the caller already holds it and the barrier must never slice a
    // buffer (a token's bytes have three possible homes; see `ppTokenText`).
    // Trivia neither advances the state nor is reported protected.
    [[nodiscard]] bool protects(Token const& t, std::string_view word);

    // TRUE when the token just handed to `protects()` WAS the `defined` operator
    // keyword, whichever construct produced it. The expander uses it to decide
    // whether to diagnose P_PreprocessorDefinedFromExpansion: it already knows
    // which tokens arrived from a replacement list (a non-empty hide set), and
    // this supplies the other half of that AND without a second copy of the
    // "is this the operator?" test.
    [[nodiscard]] bool lastWasDefinedKeyword() const noexcept {
        return lastWasDefinedKeyword_;
    }

private:
    // defined:        Idle -(kw)-> DefKeyword -( `(` )-> DefOpen -(Word)->
    //                 DefOperand -( `)` )-> Idle
    //                 DefKeyword -(Word)-> Idle                [no-paren form]
    // __has_include:  Idle -(kw)-> HdrKeyword -( `(` )-> HdrOpen
    //                 HdrOpen -( `<` )-> InAngle -(…)-> InAngle -( `>` )->
    //                     HdrOperand -( `)` )-> Idle
    //                 HdrOpen -( `"` )-> InQuote -(…)-> InQuote -( `)` )-> Idle
    //                 HdrOpen -(anything else)-> Idle, NOT protected  [the
    //                     "macro-expand and re-examine" arm]
    // Any other token in a slot returns to Idle WITHOUT protecting it, so a
    // malformed shape reaches the evaluator intact and fails loud there.
    enum class State {
        Idle, DefKeyword, DefOpen, DefOperand,
        HdrKeyword, HdrOpen, InAngle, HdrOperand, InQuote
    };

    std::string   definedKw_;
    std::string   hasIncludeKw_;
    std::string   hasEmbedKw_;
    SchemaTokenId openParen_{};
    SchemaTokenId closeParen_{};
    SchemaTokenId angleOpen_{};
    SchemaTokenId angleClose_{};
    SchemaTokenId stringOpen_{};
    State         state_ = State::Idle;
    bool          lastWasDefinedKeyword_ = false;
};

// Evaluate the `#if`/`#elif` operand tokens to a compile-time integer.
// `operandTokens` are sliced against `synth` (the prefix buffer). `productText`
// supplies any product-tail bytes materialized during expansion (FC15b) so a
// predefined/`#`/`##` product in the operand resolves; pass a provider returning
// "" for a language with no products. Returns the controlling expression's
// TRUTH VALUE (true => branch taken), or nullopt on any fail-loud condition (the
// diagnostic is already emitted into `rep`).
//
// ★ IT USED TO RETURN THE int64 VALUE, and that return type was a lie
// (D-PP-IF-UNSIGNED-INTMAX). C 6.10.1p1 evaluates a `#if` controlling expression
// in `intmax_t`/`uintmax_t`, so its result is not always representable as an
// int64 -- `#if UINT64_MAX` has a perfectly well-defined truth value and no
// int64 value at all. C 6.10.1p2 only ever asks whether the result "compares
// unequal to 0", and both callers immediately did `*v != 0`, so the truth value
// is the whole contract. Returning it directly removes the one representation
// the standard does not require this expression to have.
// FC15c: `hasInclude` resolves a `__has_include(<h>)` / `__has_include("h")`
// operand against the include search paths; pass a provider returning false for
// a language with no `__has_include` operator (then a stray `__has_include`
// folds as an ordinary identifier -> 0). The `__has_c_attribute` operator needs
// no callback -- its known-attribute set is read directly from the schema's
// `preprocess().knownCAttributes`.
// FC17.9(h): `hasEmbed` resolves a `__has_embed(...)` operand to the C23
// trichotomy (0/1/2). It is a DEFAULTED trailing parameter (null-callback
// tolerance: an unset `{}` mints 0 = NOT_FOUND); both production callers thread
// their per-origin resolver. A language with no `__has_embed` operator leaves it
// unset and a stray `__has_embed` folds to an ordinary identifier -> 0.
[[nodiscard]] std::optional<bool>
evaluateIfExpression(std::span<Token const> operandTokens,
                     GrammarSchema const&   schema,
                     PpMacroExpand const&   macroExpand,
                     PpIsDefined const&     isDefined,
                     PpHasInclude const&    hasInclude,
                     SourceBuffer const&    synth,
                     PpProductText const&   productText,
                     DiagnosticReporter&    rep,
                     PpHasEmbed const&      hasEmbed = {},
                     PpOperatorRevoked const& operatorRevoked = {},
                     // [[D-CSUBSET-CONST-EVAL-CHAR-SIGNEDNESS]]: the ACTIVE
                     // (target × object format)'s plain-`char` signedness,
                     // `TargetSchema::charIsUnsigned(ObjectFormatKind)`. C
                     // 6.4.4.4p10 makes `#if '\xff' < 0` answer differently on a
                     // signed- and an unsigned-`char` target, and this evaluator
                     // has no other way to know. DEFAULTED to `nullopt` because
                     // a `#if` fold is reachable from callers with no target in
                     // scope at all (the LSP, the direct-API tests) — but the
                     // default is NOT "signed": with it absent, a character
                     // constant above 0x7F REFUSES, loud, and every 0–127 body
                     // (which is every real-world one) folds unchanged.
                     std::optional<bool>    charIsUnsigned = std::nullopt);

} // namespace dss
