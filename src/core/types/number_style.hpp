#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
struct NumberExponent;

// Per-prefix FLOAT continuation (FC1 cycle 2, 2026-06-10 — C23
// hex-floats). A prefix that declares this block can continue past
// its integer digit run into a float: an optional fraction
// (the language's top-level `fractionPoint` + the PREFIX's digit
// class, with digits required on at least ONE side of the point —
// C23 6.4.4.2 admits `0x1.p3` and `0x.8p3` but not `0x.p3`) followed
// by a REQUIRED exponent (the letters below + an optional sign per
// `signOptional` + ≥1 digit from `exponentDigits`). The exponent is
// ALWAYS required once the float continuation commits (a fraction
// was consumed, or an exponent letter was consumed): C23 hex-floats
// mandate the binary-exponent-part, and a prefix-float without it is
// ONE malformed token + P_MalformedNumber — never a silent split. An
// `exponentRequired: false` knob is intentionally NOT modeled until
// a language consumes it (the `signRequired` precedent below).
//
// `exponentDigits` is a digit-class string for the EXPONENT digits —
// independent of the prefix's mantissa class because C hex-float
// exponents are DECIMAL ("0-9") while the mantissa is hex. The
// loader rejects an exponent letter that lands inside the prefix's
// mantissa digit class (the digit run would always consume it first,
// making the float branch unreachable — a silently-dead config).
//
// signOptional semantics here DIVERGE from the top-level decimal
// exponent: decimal's letter-without-digits leaves the letter
// unconsumed (documented split, `1e` → `1` + `e`); a prefix-float
// letter-without-digits is COMMITTED and yields the loud malformed
// token (the exponent is required, so there is no valid split to
// fall back to).
struct DSS_EXPORT NumberPrefixFloat {
    std::vector<char> exponentLetters;        // e.g. {'p','P'}
    bool              exponentSignOptional = true;
    std::string       exponentDigits = "0-9"; // class string
};

struct DSS_EXPORT NumberPrefix {
    std::string  prefix;      // e.g. "0x", "0b", "$"
    std::uint8_t radix = 10;  // 2..36
    std::string  digits;      // class string, e.g. "0-9a-fA-F" or "01"
    // Absent = the prefix lexes integers only (the pre-FC1c2 shape).
    std::optional<NumberPrefixFloat> floating;
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
    // C23 decimal fractional-constant edge forms (FC1 cycle 2,
    // 2026-06-10) — both default FALSE so every pre-existing config
    // keeps its exact lexing:
    //   trailingFraction — `1.` (digits then point, no fraction
    //     digits) lexes as ONE float. Off, `1.` splits into the
    //     integer `1` + the point's own token — the right default
    //     for range-operator languages (`1..5`).
    //   leadingFraction — `.5` (point then digits, no integer part)
    //     lexes as ONE float; the tokenizer's number-entry dispatch
    //     admits the fraction point when the NEXT byte is a decimal
    //     digit (so a bare `.` / `.foo` stays the language's dot
    //     token). Requires `fractionPoint` to be declared.
    bool                          trailingFraction = false;
    bool                          leadingFraction  = false;
};

// Test whether `c` lands in a digit character-class string. The class
// syntax supports literal chars and `a-z` ranges (the shape every
// shipped config uses); unknown forms are interpreted literally.
// Single source shared by the tokenizer's scanNumber, the schema
// loader's prefix-float validation, and decode helpers.
[[nodiscard]] inline bool digitClassMatches(std::string_view digits,
                                            char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    for (std::size_t i = 0; i < digits.size(); ++i) {
        // `a-z` range form.
        if (i + 2 < digits.size() && digits[i + 1] == '-') {
            const auto lo = static_cast<unsigned char>(digits[i]);
            const auto hi = static_cast<unsigned char>(digits[i + 2]);
            if (u >= lo && u <= hi) return true;
            i += 2;
            continue;
        }
        if (static_cast<unsigned char>(digits[i]) == u) return true;
    }
    return false;
}

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
