#pragma once

// Shared integer-literal decoder. One implementation feeds every phase that
// must turn a numeric literal's source text into a value: HIR lowering (the
// literal pool) and the semantic phase (constant array lengths). Keeping it in
// one place means a radix/separator/suffix rule is interpreted identically
// wherever a literal is evaluated.

#include "core/types/number_style.hpp"

#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace dss {

// Decode an integer literal's text per the language's NumberStyle: strip digit
// separators, detect a radix prefix (0x/0b/0o/0), parse base-valid digits and
// stop at an integer suffix (u/l/…). Returns std::nullopt on overflow of the
// 64-bit accumulator (the value is reported by the caller, never silently
// wrapped). `ns` may be null (treated as plain decimal, no separator).
[[nodiscard]] inline std::optional<std::uint64_t>
decodeInteger(std::string_view text, NumberStyle const* ns) {
    std::string s;
    s.reserve(text.size());
    char const sep = (ns && ns->digitSeparator) ? *ns->digitSeparator : '\0';
    for (char c : text) {
        if (sep != '\0' && c == sep) continue;
        s += c;
    }
    std::string_view v{s};
    std::uint64_t base = 10;
    if (v.size() >= 2 && v[0] == '0') {
        char const p = static_cast<char>(std::tolower(static_cast<unsigned char>(v[1])));
        if (p == 'x') { base = 16; v.remove_prefix(2); }
        else if (p == 'b') { base = 2; v.remove_prefix(2); }
        else if (p == 'o') { base = 8; v.remove_prefix(2); }
        else { base = 8; v.remove_prefix(1); }  // C octal: leading 0
    }
    // Parse as many base-valid digits as possible (stops at a suffix like u/l).
    std::uint64_t value = 0;
    for (char c : v) {
        std::uint64_t digit;
        if (c >= '0' && c <= '9') digit = static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint64_t>(10 + (c - 'a'));
        else if (c >= 'A' && c <= 'F') digit = static_cast<std::uint64_t>(10 + (c - 'A'));
        else break;  // suffix or stray char
        if (digit >= base) break;
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / base)
            return std::nullopt;  // overflow — caller reports
        value = value * base + digit;
    }
    return value;
}

} // namespace dss
