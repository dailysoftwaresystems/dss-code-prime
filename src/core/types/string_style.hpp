#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace dss {

// How the tokenizer interprets escape sequences inside a delimited
// string body.
//   None             — every char literal until `endsAt` (raw strings).
//   Char             — single lead character (typically `\`) starts an
//                      escape pair. Lead + next char consumed together.
//   DoubledDelimiter — terminator doubled within the body is a literal
//                      terminator. SQL `'a''b'`, C# `@"a""b"`, Pascal.
enum class EscapeKind : std::uint8_t {
    None,
    Char,
    DoubledDelimiter,
};

[[nodiscard]] DSS_EXPORT std::string_view escapeKindName(EscapeKind k) noexcept;

// Tokenizer metadata for a delimited string literal. Attached to the
// opening-delimiter token's meaning (e.g. `"`, `@"`, `R"`).
//
// Invariants (enforced by the loader; do not construct outside it):
//   - `escapeChar` is meaningful only when `escapeKind == Char`;
//     interpreted as a single ASCII byte.
//   - `tagPattern` non-empty IS the signal that the delimiter carries
//     a dynamically-captured tag (C++ R"DELIM(...)DELIM", Rust r#"..."#).
//     Default pattern when the JSON sets `delimiterTag: "matched"` but
//     omits `tagPattern` is `[A-Za-z0-9_]{0,16}`.
//   - `endsAt` is required, non-empty.
struct DSS_EXPORT StringStyle {
    EscapeKind   escapeKind         = EscapeKind::None;
    char         escapeChar         = 0;        // ASCII byte; valid iff escapeKind == Char
    bool         endsAtLongestMatch = false;
    bool         multiline          = false;
    std::string  endsAt;
    std::string  tagPattern;                    // non-empty ⇒ dynamic delimiter tag
};

// Decode the inner text of a bracket-quoted identifier whose opener `[`
// begins at byte `open` in `src` (e.g. tsql's `[Orders]` → "Orders").
// The opener token spans only the `[`; the body bytes are off-grammar
// default-mode tokens, so the content is read directly from the source
// slice rather than from the parse tree.
//
// This MATCHES the tokenizer's `EscapeKind::DoubledDelimiter` rule for the
// `[` opener (tsql-subset declares `escapeKind: doubled-delimiter,
// endsAt: "]"`): a doubled `]]` inside the body is an escaped literal `]`,
// NOT the close — so `[a]]b]` decodes to the single identifier `a]b`. A
// lone `]` (next byte is not `]`) is the close.
//
// Both the semantic engine (identifier extraction) and the import resolver
// (cross-file name matching) decode bracket-ids; sharing one implementation
// keeps them byte-identical with the tokenizer. Returns empty when the
// slice is not a well-formed `[...]` (no opener at `open`, or no close).
[[nodiscard]] DSS_EXPORT std::string
bracketInnerText(std::string_view src, std::size_t open);

} // namespace dss
