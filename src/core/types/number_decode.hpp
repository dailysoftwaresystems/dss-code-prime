#pragma once

// Shared numeric-literal decoders. One implementation feeds every phase
// that must turn a numeric literal's source text into a value: HIR
// lowering (the literal pool) and the semantic phase (constant array
// lengths). Keeping them in one place means a radix/separator/suffix
// rule is interpreted identically wherever a literal is evaluated.

#include "core/types/number_style.hpp"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace dss {

namespace detail {

// Strip exactly ONE longest-matching declared suffix from the END of
// the text. Never strips mid-text: in a hex-float `0x1.fp3` the 'f'
// is a MANTISSA DIGIT, and in a base-36 integer a suffix letter can
// be a digit — only the trailing position is unambiguous.
[[nodiscard]] inline std::string_view
stripTrailingSuffix(std::string_view text,
                    std::vector<std::string> const& suffixes) {
    std::size_t best = 0;
    for (auto const& sfx : suffixes) {
        if (sfx.size() > best && text.size() >= sfx.size()
            && text.substr(text.size() - sfx.size()) == sfx) {
            best = sfx.size();
        }
    }
    text.remove_suffix(best);
    return text;
}

}  // namespace detail

// Decode an integer literal's text per the language's NumberStyle:
// strip ONE trailing declared integer suffix, strip digit separators,
// resolve the radix from the longest matching DECLARED prefix
// (FC1 cycle 2, 2026-06-10 — previously a hardcoded 0x/0b/0o/0 set
// that silently returned 0 for any non-C-shaped prefix like `$ff`),
// then parse base-valid digits. Returns std::nullopt on overflow of
// the 64-bit accumulator (the value is reported by the caller, never
// silently wrapped). `ns` may be null (treated as plain decimal, no
// separator, no prefixes, no suffixes).
//
// The suffix strip happens FIRST (on the raw text) because at high
// radices a suffix letter is also a valid digit ('u' is the digit 30
// in base ≥31) — the digit loop's stop-at-non-digit can no longer be
// relied on to terminate at the suffix.
[[nodiscard]] inline std::optional<std::uint64_t>
decodeInteger(std::string_view text, NumberStyle const* ns) {
    if (ns != nullptr) {
        text = detail::stripTrailingSuffix(text, ns->integerSuffixes);
    }
    std::string s;
    s.reserve(text.size());
    char const sep = (ns && ns->digitSeparator) ? *ns->digitSeparator : '\0';
    for (char c : text) {
        if (sep != '\0' && c == sep) continue;
        s += c;
    }
    std::string_view v{s};
    std::uint64_t base = 10;
    if (ns != nullptr) {
        std::size_t  bestLen   = 0;
        std::uint8_t bestRadix = 10;
        for (auto const& p : ns->integerPrefixes) {
            if (p.prefix.size() > bestLen && v.size() >= p.prefix.size()
                && v.substr(0, p.prefix.size()) == p.prefix) {
                bestLen   = p.prefix.size();
                bestRadix = p.radix;
            }
        }
        if (bestLen > 0) {
            base = bestRadix;
            v.remove_prefix(bestLen);
        }
    }
    // Parse as many base-valid digits as possible. Letters map a..z →
    // 10..35 (covering every loader-admitted radix in [2,36]; the
    // pre-FC1c2 map stopped at 'f', silently mis-valuing radix-17+
    // configs).
    std::uint64_t value = 0;
    for (char c : v) {
        std::uint64_t digit;
        if (c >= '0' && c <= '9') digit = static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'z') digit = static_cast<std::uint64_t>(10 + (c - 'a'));
        else if (c >= 'A' && c <= 'Z') digit = static_cast<std::uint64_t>(10 + (c - 'A'));
        else break;  // stray char (e.g. a fraction point) — caller's domain
        if (digit >= base) break;
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / base)
            return std::nullopt;  // overflow — caller reports
        value = value * base + digit;
    }
    return value;
}

// Decode a float literal's text per the language's NumberStyle: strip
// ONE trailing declared float suffix, strip digit separators, then
// hand the body to std::strtod — which parses both decimal floats and
// C99 hex-floats (`0x1.8p3`) on every supported toolchain.
//
// FC1 cycle 2 (2026-06-10): hoisted from cst_to_hir.cpp, where it
// stripped EVERY 'f'/'F' character anywhere in the text (a hardcoded
// C-ism in shared substrate). That was value-corrupting for
// hex-floats — `0x1.fp3` (= 15.5) lost its mantissa digit and decoded
// as `0x1.p3` (= 8.0). The strip is now schema-declared and
// trailing-only. (Red-on-disable demonstrated: restoring the
// strip-anywhere behavior turns `FloatHexMantissaFDigitIsNotStripped`
// red at 8.0 ≠ 15.5.)
//
// `ok` reports whether strtod consumed the WHOLE body in-range (audit
// fold, FC1c2: prefix-consumption is not enough — a non-strtod-shaped
// exotic config like `1.5^3` would otherwise return 1.5 with the
// `^3` silently dropped, a truncated value masquerading as success).
// The caller owns the diagnostic: any config whose token text strtod
// cannot FULLY parse degrades LOUDLY through ok=false — never a
// silent zero, never a silently truncated value.
[[nodiscard]] inline double
decodeFloat(std::string_view text, NumberStyle const* ns, bool& ok) {
    std::string_view body = text;
    if (ns != nullptr) {
        body = detail::stripTrailingSuffix(body, ns->floatSuffixes);
    }
    std::string s;
    s.reserve(body.size());
    char const sep = (ns && ns->digitSeparator) ? *ns->digitSeparator : '\0';
    for (char c : body) {
        if (sep != '\0' && c == sep) continue;
        s += c;
    }
    errno = 0;
    char* end = nullptr;
    double const d = std::strtod(s.c_str(), &end);
    ok = (end == s.c_str() + s.size()) && !s.empty() && errno != ERANGE;
    return d;
}

} // namespace dss
