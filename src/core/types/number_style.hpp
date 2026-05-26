#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace dss {

// Numeric-literal lexical grammar declared by a language's
// `numberStyle` schema block. Consumed by the tokenizer's
// `scanNumber()` so the engine has zero hardcoded numeric syntax.
//
// Every field is configurable; the engine reads the active language's
// NumberStyle and walks input accordingly. A language with no numeric
// literals omits the block entirely (the loader emits
// `C_MissingNumberStyle` only when the schema also declares
// `IntLiteral`/`FloatLiteral` as multi-char tokens).
//
// Invariants (enforced by the loader; do not construct outside it):
//   - `emitKind.integer` is required and valid when the block exists.
//   - `emitKind.float` is required and valid when any float-producing
//     facet is present (`exponent`, `fractionPoint`, or any
//     `floatSuffixes`).
//   - `integerPrefixes[*].prefix` and `.digits` are non-empty.
//   - `integerPrefixes[*].radix` ∈ [2, 36].
//   - `exponent.letters[*]` are single ASCII letters; the loader
//     keeps the user-declared order.
//   - `fractionPoint`, when present, is a single ASCII char (the C
//     `.`, the Pascal `.` etc.).
//   - `digitSeparator`, when present, is a single ASCII byte.
//   - Suffix vectors carry whatever language-specific tags the user
//     supplies; the scanner just longest-matches them after a number
//     body and DOES NOT validate the letters against a hardcoded set.
struct DSS_EXPORT NumberPrefix {
    std::string  prefix;      // e.g. "0x", "0b", "$"
    std::uint8_t radix = 10;  // 2..36
    std::string  digits;      // class string, e.g. "0-9a-fA-F" or "01"
};

struct DSS_EXPORT NumberExponent {
    std::vector<char>  letters;        // e.g. {'e','E'} or {'^'}
    // Semantics of `signOptional`:
    //   true  — a `+` or `-` MAY appear between the exponent letter
    //           and the exponent digits (`1e+3`, `1e-3`, `1e3` all
    //           accepted). This is the C-style default.
    //   false — NO sign is permitted between the letter and the
    //           digits. `1e+3` does NOT tokenize as a single float —
    //           it tokenizes as `1` IntLiteral + `e` Identifier (or
    //           whatever the schema makes of `e`) + `+` Plus + `3`.
    //           Use for languages whose exponent grammar bakes the
    //           sign into a separate operator.
    // Note: this field does NOT mean "sign is required" — that
    // would be `signRequired: true`, which no shipped config uses
    // and is intentionally not modeled.
    bool               signOptional = true;
};

struct DSS_EXPORT NumberEmitKind {
    SchemaTokenId integer{};
    SchemaTokenId floating{};   // `float` is a reserved word
};

struct DSS_EXPORT NumberStyle {
    bool                          decimal = false;
    std::vector<NumberPrefix>     integerPrefixes;
    std::optional<NumberExponent> exponent;
    std::optional<char>           fractionPoint;
    std::optional<char>           digitSeparator;
    std::vector<std::string>      integerSuffixes;
    std::vector<std::string>      floatSuffixes;
    NumberEmitKind                emitKind{};
};

// Trivially-copyable / size posture asserts. Mirror the
// `ExprWrapperRules` style in `grammar_schema.hpp:90` so a future
// refactor can't silently fatten the POD subset (NumberEmitKind is
// copied through the tokenizer's hot path).
static_assert(std::is_trivially_copyable_v<NumberEmitKind>,
              "NumberEmitKind must stay trivially copyable — read once "
              "per scanNumber call and propagated by value into Token "
              "emissions.");
static_assert(sizeof(NumberPrefix::radix) == 1,
              "NumberPrefix::radix should fit in uint8_t — the loader "
              "validates the input range as [2, 36].");
static_assert(!std::is_trivially_copyable_v<NumberStyle>,
              "NumberStyle owns strings/vectors — must NOT be trivially "
              "copyable; callers receive it by const reference / pointer.");

} // namespace dss
