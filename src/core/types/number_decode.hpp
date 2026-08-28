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
#include <vector>

namespace dss {

namespace detail {

// The ONE longest-matching declared suffix at the END of the text (the
// matched spelling, or the empty view when none matches). Never matches
// mid-text: in a hex-float `0x1.fp3` the 'f' is a MANTISSA DIGIT, and in
// a base-36 integer a suffix letter can be a digit — only the trailing
// position is unambiguous. Shared by the strip below AND the FC3 c1
// integer-literal ladder (which classifies the literal by WHICH declared
// suffix spelling matched) so the two can never disagree on the match.
[[nodiscard]] inline std::string_view
matchTrailingSuffix(std::string_view text,
                    std::vector<std::string> const& suffixes) {
    std::size_t best = 0;
    for (auto const& sfx : suffixes) {
        if (sfx.size() > best && text.size() >= sfx.size()
            && text.substr(text.size() - sfx.size()) == sfx) {
            best = sfx.size();
        }
    }
    return text.substr(text.size() - best);
}

// Strip exactly ONE longest-matching declared suffix from the END of
// the text (via the shared matcher above).
[[nodiscard]] inline std::string_view
stripTrailingSuffix(std::string_view text,
                    std::vector<std::string> const& suffixes) {
    text.remove_suffix(matchTrailingSuffix(text, suffixes).size());
    return text;
}

// The digit VALUE of one character under the a..z → 10..35 map that covers
// every loader-admitted radix in [2,36], or nullopt for a character that is
// not a digit at all. ONE owner: `decodeInteger`, `decodeBigInteger` and the
// digit-valued-prefix test below all read it, so the map cannot drift between
// a decoder and the rule that decides whether a body has any digits.
[[nodiscard]] inline std::optional<std::uint64_t>
digitValue(char c) noexcept {
    if (c >= '0' && c <= '9') return static_cast<std::uint64_t>(c - '0');
    if (c >= 'a' && c <= 'z') return static_cast<std::uint64_t>(10 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') return static_cast<std::uint64_t>(10 + (c - 'A'));
    return std::nullopt;
}

// An integer literal's text after the ONE normalization every reader of one
// shares. `digits` is the body the digit loop consumes; `base` its resolved
// radix; `prefixed` whether a declared prefix matched at all (the RADIX CLASS
// C 6.4.4.1 keys its extra unsigned candidates on).
struct IntegerLiteralBody {
    std::string   digits;
    std::uint64_t base     = 10;
    bool          prefixed = false;
};

// ★★ THE ONE NORMALIZATION — strip ONE trailing declared suffix, strip digit
// separators, resolve the radix from the LONGEST declared prefix. This was
// written out THREE times (`integerLiteralIsPrefixed`, `decodeInteger`,
// `decodeBigInteger`), and the three had drifted: the two decoders disagreed
// about whether an empty digit body is the value ZERO or a malformed token.
// D-CSUBSET-BITINT-ZERO-LITERAL-EATEN-BY-THE-OCTAL-PREFIX is exactly that
// disagreement reaching a user — `0wb` decoded fine through `decodeInteger`
// and came back `nullopt` from `decodeBigInteger`, whose caller then reported
// the u64 fall-through's message and told the programmer their ZERO was
// "too large for any declared type".
//
// ★ A PREFIX IS A RADIX MARKER — EXCEPT WHEN IT IS ITSELF THE LITERAL'S
// DIGITS. C's octal prefix is spelled `0`, and C 6.4.4.1 gives
// `octal-constant: 0 | octal-constant octal-digit` — the leading `0` is the
// grammar's BASE CASE, a digit and not merely a marker. So consuming it out of
// `0` leaves an empty body that is not malformed at all; it is the number
// zero. The rule, stated once and applied by every reader: the prefix is
// removed UNLESS removing it would empty the body AND the prefix is itself a
// valid digit sequence in the resolved radix.
//
// ⚠ Deliberately NOT a config key. ✔MEASURED across every shipped
// `integerPrefixes` table: `0x`/`0X`/`0b`/`0B`/`0o`/`0O` all carry a letter
// that is NOT a valid digit in their own radix ('x'→33 ≥ 16, 'b'→11 ≥ 2,
// 'o'→24 ≥ 8), so they stay pure markers and `0xwb`/`0bwb`/`0owb` remain
// malformed here exactly as before — while `0` is all-digits and stands alone.
// The fact does not vary along any axis a key would carry; it is one decoder
// rule derived from the radix the config already declares.
[[nodiscard]] inline IntegerLiteralBody
normalizeIntegerLiteral(std::string_view text, NumberStyle const* ns) {
    IntegerLiteralBody out;
    if (ns != nullptr) {
        text = stripTrailingSuffix(text, ns->integerSuffixes);
    }
    out.digits.reserve(text.size());
    char const sep = (ns && ns->digitSeparator) ? *ns->digitSeparator : '\0';
    for (char c : text) {
        if (sep != '\0' && c == sep) continue;
        out.digits += c;
    }
    if (ns == nullptr) return out;

    std::size_t  bestLen   = 0;
    std::uint8_t bestRadix = 10;
    for (auto const& p : ns->integerPrefixes) {
        if (p.prefix.size() > bestLen && out.digits.size() >= p.prefix.size()
            && std::string_view{out.digits}.substr(0, p.prefix.size()) == p.prefix) {
            bestLen   = p.prefix.size();
            bestRadix = p.radix;
        }
    }
    if (bestLen == 0) return out;
    out.prefixed = true;
    out.base     = bestRadix;
    if (out.digits.size() == bestLen) {
        // The prefix is the WHOLE text. Keep it as the body when every one of
        // its characters is a digit in the radix it just selected — that is
        // the `0`-is-octal-zero case — and drop it otherwise, leaving an empty
        // body the caller may judge malformed.
        bool allDigits = true;
        for (char c : out.digits) {
            auto const d = digitValue(c);
            if (!d.has_value() || *d >= out.base) { allDigits = false; break; }
        }
        if (allDigits) return out;
    }
    out.digits.erase(0, bestLen);
    return out;
}

}  // namespace detail

// FC3 c1: the declared integer suffix spelling an integer literal's raw
// text carries (longest tail match against `ns->integerSuffixes`), or the
// empty view for an unsuffixed literal / null style. The integer-literal
// ladder keys its rule selection on the EXACT matched spelling.
[[nodiscard]] inline std::string_view
matchIntegerSuffix(std::string_view text, NumberStyle const* ns) {
    if (ns == nullptr) return {};
    return detail::matchTrailingSuffix(text, ns->integerSuffixes);
}

// FC3.5 sweep-c2: the float sibling — the declared float suffix
// spelling a float literal's raw text carries (longest tail match
// against `ns->floatSuffixes`), or the empty view for an unsuffixed
// literal / null style. `typeFloatLiteral` keys its rule selection on
// the EXACT matched spelling (the same match `decodeFloat`'s suffix
// strip performs, so typing and decode can never disagree).
[[nodiscard]] inline std::string_view
matchFloatSuffix(std::string_view text, NumberStyle const* ns) {
    if (ns == nullptr) return {};
    return detail::matchTrailingSuffix(text, ns->floatSuffixes);
}

// FC3 c1: true iff the literal's text (suffix + separators stripped) starts
// with a declared `integerPrefixes` prefix. This is the ladder's radix-CLASS
// test: C 6.4.4.1 gives octal/hex ("nondecimal") constants extra unsigned
// candidates. It reads the SHARED normalization rather than mirroring the
// decoders' prefix scan, so the class and the decoded value cannot disagree
// about the radix even in principle.
//
// ★ `0` IS PREFIXED, and that is C 6.4.4.1's own reading — `0` is an
// octal-constant, so it takes the nondecimal candidate list. Unchanged by the
// digit-valued-prefix rule, which decides what the DIGIT BODY is, never
// whether a prefix matched.
[[nodiscard]] inline bool
integerLiteralIsPrefixed(std::string_view text, NumberStyle const* ns) {
    return detail::normalizeIntegerLiteral(text, ns).prefixed;
}

// Decode an integer literal's text per the language's NumberStyle, over the
// SHARED `normalizeIntegerLiteral` (one suffix strip, separator strip and
// longest-declared-prefix radix resolution — FC1 cycle 2, 2026-06-10, which
// replaced a hardcoded 0x/0b/0o/0 set that silently returned 0 for any
// non-C-shaped prefix like `$ff`). Returns std::nullopt on overflow of the
// 64-bit accumulator (the value is reported by the caller, never silently
// wrapped). `ns` may be null (treated as plain decimal, no separator, no
// prefixes, no suffixes).
//
// The suffix strip happens FIRST (on the raw text) because at high
// radices a suffix letter is also a valid digit ('u' is the digit 30
// in base ≥31) — the digit loop's stop-at-non-digit can no longer be
// relied on to terminate at the suffix.
//
// ⚠ This decoder deliberately keeps NO "no digits at all" verdict: it returns
// 0 for an empty body, and its `nullopt` means OVERFLOW to every caller. Do
// not add an `anyDigit` guard here — it would report a malformed token under
// the overflow diagnostic, which is the mirror image of
// D-CSUBSET-BITINT-ZERO-LITERAL-EATEN-BY-THE-OCTAL-PREFIX (there, a
// well-formed ZERO was reported as an overflow). Since the shared
// normalization now keeps a digit-valued prefix as the body, an empty body
// reaching here means a token the tokenizer already refuses.
[[nodiscard]] inline std::optional<std::uint64_t>
decodeInteger(std::string_view text, NumberStyle const* ns) {
    auto const body = detail::normalizeIntegerLiteral(text, ns);
    // Parse as many base-valid digits as possible. The digit map (a..z →
    // 10..35, covering every loader-admitted radix in [2,36]) is
    // `detail::digitValue`'s, shared with `decodeBigInteger` — the pre-FC1c2
    // map stopped at 'f' and silently mis-valued radix-17+ configs, and two
    // hand-copies of the replacement is how that class of defect returns.
    std::uint64_t value = 0;
    for (char c : body.digits) {
        auto const d = detail::digitValue(c);
        if (!d.has_value()) break;  // stray char (e.g. a fraction point) — caller's domain
        if (*d >= body.base) break;
        if (value > (std::numeric_limits<std::uint64_t>::max() - *d) / body.base)
            return std::nullopt;  // overflow — caller reports
        value = value * body.base + *d;
    }
    return value;
}

// C23 6.4.4.1 (D-CSUBSET-BITINT-WIDE-LITERAL): decode an integer literal's text
// to its ARBITRARY-MAGNITUDE unsigned value as little-endian 64-bit limbs. The
// sibling of `decodeInteger` for a `wb`/`uwb` bit-precise literal ONLY — whose
// magnitude may exceed u64 (`633825300114114700748351602688uwb`), which
// `decodeInteger` rejects (nullopt) by design. Literally the SAME
// normalization as `decodeInteger` — `normalizeIntegerLiteral`, one function,
// not a second copy of the same four steps — then a bignum
// multiply-accumulate. Returns std::nullopt only when the body has NO
// base-valid digits (a malformed token the caller surfaces fail-loud) — never
// on "overflow" (there is none). `ns` may be null (plain decimal). The suffix
// strip happens FIRST (a suffix letter is a valid high-radix digit).
//
// ★ D-CSUBSET-BITINT-ZERO-LITERAL-EATEN-BY-THE-OCTAL-PREFIX: the `anyDigit`
// verdict below is CORRECT and stays. What was wrong lived one step earlier —
// the normalization consumed `0`'s only digit as a radix marker, so a
// well-formed `0wb` arrived here with an empty body and came back malformed.
// The fix is in the shared normalization, which is why the two callers this
// decoder shares with `cst_const_eval` and `cst_to_hir` needed no edit.
[[nodiscard]] inline std::optional<std::vector<std::uint64_t>>
decodeBigInteger(std::string_view text, NumberStyle const* ns) {
    auto const body = detail::normalizeIntegerLiteral(text, ns);
    std::uint64_t const base = body.base;
    std::vector<std::uint64_t> mag{0};   // little-endian magnitude accumulator
    bool anyDigit = false;
    for (char c : body.digits) {
        auto const dv = detail::digitValue(c);
        if (!dv.has_value()) break;       // stray char — caller's domain
        std::uint64_t const digit = *dv;
        if (digit >= base) break;
        anyDigit = true;
        // mag = mag * base + digit, as a little-endian bignum multiply-accumulate.
        std::uint64_t carry = digit;
        for (std::size_t i = 0; i < mag.size(); ++i) {
            // 64×64 → 128 via 32-bit halves (portable; no __uint128_t / _umul128).
            std::uint64_t const a = mag[i];
            std::uint64_t const aL = a & 0xFFFFFFFFull, aH = a >> 32;
            std::uint64_t const bL = base & 0xFFFFFFFFull, bH = base >> 32;
            std::uint64_t const ll = aL * bL, lh = aL * bH, hl = aH * bL, hh = aH * bH;
            std::uint64_t const cross = (ll >> 32) + (lh & 0xFFFFFFFFull) + (hl & 0xFFFFFFFFull);
            std::uint64_t lo = (ll & 0xFFFFFFFFull) | (cross << 32);
            std::uint64_t hi = hh + (lh >> 32) + (hl >> 32) + (cross >> 32);
            std::uint64_t const s0 = lo + carry;          // add the running carry
            hi += (s0 < lo) ? 1u : 0u;
            mag[i] = s0;
            carry  = hi;
        }
        if (carry != 0) mag.push_back(carry);
    }
    if (!anyDigit) return std::nullopt;
    return mag;
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
