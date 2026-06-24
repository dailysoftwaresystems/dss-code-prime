#pragma once

// FC14 (D-PP-CONDITIONAL-COMPILATION): the C-preprocessor `#if` / `#elif`
// integer-constant-expression (ICE) evaluator (C 6.10.1). Given the operand
// tokens of an `#if`/`#elif` line (everything after the directive word, up to
// the newline), it returns the operand's compile-time integer value -- the
// caller treats a NON-ZERO result as "branch taken".
//
// Pipeline (C 6.10.1p1/p4), in order:
//   1. `defined X` / `defined(X)` -> 1 or 0 (queried via the `isDefined`
//      callback). The operand of `defined` is NOT macro-expanded.
//   2. The REMAINING operand identifiers are macro-expanded (via the
//      `macroExpand` callback -- the SAME `MacroExpander::expand` engine the
//      rest of the preprocessor uses, so object/function-like macros in an
//      `#if` operand expand identically).
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

// FC15b: the MacroExpander's accumulated `#`/`##`/predefined PRODUCT text, as it
// stands AFTER `macroExpand` runs over an `#if` operand. A predefined macro
// (`__STDC_VERSION__` &c.) or a `#`/`##` product expanded inside a `#if`
// controlling expression materializes a token whose span points into the synth
// buffer's product TAIL (`[prefixLen + ..)`), which is NOT yet appended to the
// prefix-only `synth` buffer at #if-eval time. This provider returns that tail
// so the evaluator can assemble a COMBINED (prefix + product) buffer the ICE
// parser slices every real token against. Returns an empty view when the
// language produces no products (then the combined buffer == the prefix).
using PpProductText = std::function<std::string_view()>;

// Evaluate the `#if`/`#elif` operand tokens to a compile-time integer.
// `operandTokens` are sliced against `synth` (the prefix buffer). `productText`
// supplies any product-tail bytes materialized during expansion (FC15b) so a
// predefined/`#`/`##` product in the operand resolves; pass a provider returning
// "" for a language with no products. Returns the int64 value (caller: != 0 =>
// branch taken), or nullopt on any fail-loud condition (the diagnostic is
// already emitted into `rep`).
[[nodiscard]] std::optional<std::int64_t>
evaluateIfExpression(std::span<Token const> operandTokens,
                     GrammarSchema const&   schema,
                     PpMacroExpand const&   macroExpand,
                     PpIsDefined const&     isDefined,
                     SourceBuffer const&    synth,
                     PpProductText const&   productText,
                     DiagnosticReporter&    rep);

} // namespace dss
