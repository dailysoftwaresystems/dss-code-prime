#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace dss {

// How the tokenizer interprets escape sequences inside a delimited
// string body.
//   None             — no escape mechanism; every char is literal until `endsAt`.
//                      Used by raw strings (R"(...)", here-strings, etc.).
//   Char             — a single lead character (typically `\`) introduces an
//                      escape pair. The tokenizer consumes the lead + the
//                      next char as a single escape sequence.
//   DoubledDelimiter — the terminator character doubled within the body
//                      means "literal terminator." Used by SQL (`'a''b'`),
//                      C# verbatim (`@"a""b"`), Pascal (`'a''b'`).
enum class EscapeKind : std::uint8_t {
    None,
    Char,
    DoubledDelimiter,
};

[[nodiscard]] DSS_EXPORT std::string_view escapeKindName(EscapeKind k) noexcept;

// Tokenizer-side metadata for a delimited string literal. Attached to
// the meaning of the OPENING delimiter token (e.g. `"`, `@"`, `R"`).
// The tokenizer (when authored) reads this off the meaning, switches
// into the string-body mode, and consumes characters according to the
// rules below until `endsAt` is matched.
//
// Strings are owned (not span-into-pool) because each StringStyle has
// at most two of them and the records live in a single
// `GrammarSchemaData::stringStyles` vector indexed by
// `LexemeMeaning::stringStyleIdx`. LexemeMeaning stays trivially
// copyable because it only carries the index.
struct DSS_EXPORT StringStyle {
    EscapeKind   escapeKind         = EscapeKind::None;
    char         escapeChar         = 0;        // valid when escapeKind == Char
    bool         endsAtLongestMatch = false;    // greedy run-of-terminator match
    bool         hasMatchedDelimiterTag = false;
    bool         multiline          = false;
    std::string  endsAt;                        // required terminator sequence
    std::string  tagPattern;                    // valid when hasMatchedDelimiterTag
};

} // namespace dss
