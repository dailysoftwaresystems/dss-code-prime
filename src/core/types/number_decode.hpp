#pragma once

// Shared numeric-literal decoders. One implementation feeds every phase
// that must turn a numeric literal's source text into a value: HIR
// lowering (the literal pool) and the semantic phase (constant array
// lengths). Keeping them in one place means a radix/separator/suffix
// rule is interpreted identically wherever a literal is evaluated.

#include "core/types/number_style.hpp"
#include "core/types/wide_float_value.hpp"   // WideFloatValue (the F80/F128 target-precision arm)

#include <algorithm>
#include <bit>
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

// ★ THE ONE FLOAT-LITERAL BODY NORMALIZATION — strip ONE trailing declared float
// suffix, then strip digit separators. Shared by `decodeFloat` (host `double` via
// strtod) and `decodeFloatWide` (target-precision), so the two can never disagree
// about WHICH CHARACTERS are the number: the integer side's
// `normalizeIntegerLiteral` discipline, applied to the float side after
// D-CSUBSET-LONG-DOUBLE-LITERAL-DECODE-PRECISION gave it a second reader.
[[nodiscard]] inline std::string
floatLiteralBody(std::string_view text, NumberStyle const* ns) {
    std::string_view body = text;
    if (ns != nullptr) {
        body = stripTrailingSuffix(body, ns->floatSuffixes);
    }
    std::string s;
    s.reserve(body.size());
    char const sep = (ns && ns->digitSeparator) ? *ns->digitSeparator : '\0';
    for (char c : body) {
        if (sep != '\0' && c == sep) continue;
        s += c;
    }
    return s;
}

// ── D-CSUBSET-LONG-DOUBLE-LITERAL-DECODE-PRECISION: the arbitrary-precision
// substrate the TARGET-precision float decoder evaluates a literal in ──────────
//
// A float literal is an EXACT rational, and rounding it correctly at 64/113
// significand bits means evaluating it exactly first. These are the smallest
// magnitude primitives that permits: little-endian 64-bit limbs, always at least
// one limb, no sign (the literal's sign is carried separately).
//
// ⚠ NOT `BitIntValue` and not `WideFloatValue`'s 256-bit register: both are
// FIXED-WIDTH by design (a `_BitInt` has a declared width; the soft-float
// register is exactly 256 bits with ≥143 guard bits). A decimal literal's
// numerator and its power-of-ten denominator are unbounded — `1e-4000L` needs a
// 13288-bit denominator — so the intermediate has to grow. The RESULT lands back
// in `WideFloatValue`'s register the moment it is normalized, and every rounding
// decision is taken there.
using FloatMag = std::vector<std::uint64_t>;

[[nodiscard]] inline int magBitLength(FloatMag const& m) noexcept {
    for (std::size_t i = m.size(); i-- > 0;) {
        if (m[i] != 0)
            return static_cast<int>(i) * 64 + 64 - std::countl_zero(m[i]);
    }
    return 0;
}
[[nodiscard]] inline bool magIsZero(FloatMag const& m) noexcept {
    for (std::uint64_t w : m) if (w != 0) return false;
    return true;
}
[[nodiscard]] inline bool magGetBit(FloatMag const& m, int bit) noexcept {
    if (bit < 0) return false;
    std::size_t const w = static_cast<std::size_t>(bit) >> 6;
    if (w >= m.size()) return false;
    return ((m[w] >> (bit & 63)) & 1u) != 0;
}
// Any 1 bit strictly below `pos` — the sticky the round-to-nearest-even
// decision needs when a normalization shifts bits off the bottom.
[[nodiscard]] inline bool magAnyBitBelow(FloatMag const& m, int pos) noexcept {
    if (pos <= 0) return false;
    std::size_t const fullWords = static_cast<std::size_t>(pos) >> 6;
    for (std::size_t i = 0; i < fullWords && i < m.size(); ++i) {
        if (m[i] != 0) return true;
    }
    int const rem = pos & 63;
    if (rem != 0 && fullWords < m.size()) {
        std::uint64_t const mask = (std::uint64_t{1} << rem) - 1u;
        if ((m[fullWords] & mask) != 0) return true;
    }
    return false;
}
// m = m·mul + add. The 64×64 → 128 product goes through 32-bit halves — the
// `decodeBigInteger` / `BitIntValue::mul64` / `WideFloatValue::mul64` portability
// discipline (no `__int128`, no `_umul128`).
inline void magMulAddSmall(FloatMag& m, std::uint64_t mul, std::uint64_t add) {
    std::uint64_t carry = add;
    for (std::uint64_t& w : m) {
        std::uint64_t const a  = w;
        std::uint64_t const aL = a & 0xFFFFFFFFull,   aH = a >> 32;
        std::uint64_t const bL = mul & 0xFFFFFFFFull, bH = mul >> 32;
        std::uint64_t const ll = aL * bL, lh = aL * bH, hl = aH * bL, hh = aH * bH;
        std::uint64_t const cross = (ll >> 32) + (lh & 0xFFFFFFFFull) + (hl & 0xFFFFFFFFull);
        std::uint64_t const lo    = (ll & 0xFFFFFFFFull) | (cross << 32);
        std::uint64_t       hi    = hh + (lh >> 32) + (hl >> 32) + (cross >> 32);
        std::uint64_t const s     = lo + carry;
        hi += (s < lo) ? 1u : 0u;
        w     = s;
        carry = hi;
    }
    if (carry != 0) m.push_back(carry);
}
// m <<= k, EXACTLY (the magnitude grows; nothing is ever shifted out).
inline void magShiftLeft(FloatMag& m, int k) {
    if (k <= 0) return;
    std::size_t const ws  = static_cast<std::size_t>(k) >> 6;
    int const         bs  = k & 63;
    std::size_t const old = m.size();
    m.resize(old + ws + 1, 0);
    for (std::size_t i = m.size(); i-- > 0;) {
        std::uint64_t const lo  = (i >= ws       && (i - ws)     < old) ? m[i - ws]     : 0u;
        std::uint64_t const hiw = (i >= ws + 1u  && (i - ws - 1) < old) ? m[i - ws - 1] : 0u;
        m[i] = (bs == 0) ? lo : ((lo << bs) | (hiw >> (64 - bs)));
    }
}
[[nodiscard]] inline int magCompare(FloatMag const& a, FloatMag const& b) noexcept {
    std::size_t const n = std::max(a.size(), b.size());
    for (std::size_t i = n; i-- > 0;) {
        std::uint64_t const av = (i < a.size()) ? a[i] : 0u;
        std::uint64_t const bv = (i < b.size()) ? b[i] : 0u;
        if (av != bv) return (av > bv) ? 1 : -1;
    }
    return 0;
}
inline void magSubFrom(FloatMag& a, FloatMag const& b) noexcept {   // a -= b (requires a >= b)
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::uint64_t const bv = (i < b.size()) ? b[i] : 0u;
        std::uint64_t const d1 = a[i] - bv;    std::uint64_t const b1 = (a[i] < bv) ? 1u : 0u;
        std::uint64_t const d2 = d1 - borrow;  std::uint64_t const b2 = (d1 < borrow) ? 1u : 0u;
        a[i] = d2; borrow = b1 + b2;
    }
}
// Q = floor(N / D), R = N mod D. Bit-at-a-time from the top — `WideFloatValue::
// div`'s shape (no host divide of a multi-word value, nothing to get wrong about
// a normalization step). D must be nonzero; here it is always a power of ten.
inline void magDivMod(FloatMag const& N, FloatMag const& D, FloatMag& Q, FloatMag& R) {
    int const nb = magBitLength(N);
    Q.assign(N.size() + 1, 0);
    R.assign(D.size() + 1, 0);
    for (int i = nb - 1; i >= 0; --i) {
        std::uint64_t carry = magGetBit(N, i) ? 1u : 0u;     // R = (R << 1) | N[i]
        for (std::uint64_t& w : R) {
            std::uint64_t const nc = w >> 63;
            w = (w << 1) | carry;
            carry = nc;
        }
        if (magCompare(R, D) >= 0) {
            magSubFrom(R, D);
            Q[static_cast<std::size_t>(i) >> 6] |= (std::uint64_t{1} << (i & 63));
        }
    }
}
// The 256-bit window of `m` whose LOW bit is `lowBit`. A NEGATIVE `lowBit` reads
// `m` as if it had been shifted LEFT by -lowBit (which is exact, since the window
// then covers every bit m has). Written as 256 bit tests rather than a word-wise
// funnel shift: it is called ONCE per literal, and a funnel shift with a signed,
// possibly-negative distance is exactly where an off-by-one hides.
inline void magExtract256(FloatMag const& m, int lowBit, std::uint64_t (&W)[4]) noexcept {
    W[0] = W[1] = W[2] = W[3] = 0;
    for (int b = 0; b < 256; ++b) {
        if (magGetBit(m, lowBit + b)) W[b >> 6] |= (std::uint64_t{1} << (b & 63));
    }
}

inline constexpr std::uint64_t kPow10[20] = {
    1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull,
    100000000ull, 1000000000ull, 10000000000ull, 100000000000ull,
    1000000000000ull, 10000000000000ull, 100000000000000ull, 1000000000000000ull,
    10000000000000000ull, 100000000000000000ull, 1000000000000000000ull,
    10000000000000000000ull,
};
inline constexpr std::uint64_t kPow16[16] = {
    1ull, 0x10ull, 0x100ull, 0x1000ull, 0x10000ull, 0x100000ull, 0x1000000ull,
    0x10000000ull, 0x100000000ull, 0x1000000000ull, 0x10000000000ull,
    0x100000000000ull, 0x1000000000000ull, 0x10000000000000ull,
    0x100000000000000ull, 0x1000000000000000ull,
};

inline void magMulPow10(FloatMag& m, long long k) {
    while (k >= 19) { magMulAddSmall(m, kPow10[19], 0); k -= 19; }
    if (k > 0) magMulAddSmall(m, kPow10[static_cast<std::size_t>(k)], 0);
}

// The digit string as an exact magnitude. Chunked (19 decimal / 15 hex digits per
// multiply-accumulate, the widest that stays inside a `std::uint64_t`) so a long
// literal costs a bignum step per chunk rather than per digit. The digit MAP is
// `digitValue`, shared with every other decoder in this header.
[[nodiscard]] inline FloatMag digitsToMag(std::string_view digits, unsigned base) {
    FloatMag m{0};
    std::size_t const chunk = (base == 16) ? 15u : 19u;
    for (std::size_t i = 0; i < digits.size();) {
        std::size_t const take = std::min(chunk, digits.size() - i);
        std::uint64_t acc = 0;
        for (std::size_t j = 0; j < take; ++j) {
            acc = acc * base + digitValue(digits[i + j]).value_or(0);
        }
        magMulAddSmall(m, (base == 16) ? kPow16[take] : kPow10[take], acc);
        i += take;
    }
    return m;
}

// A floating constant's exact shape, as this header's own grammar reads it:
//   decimal: digits(base 10) · 10^(exponent − pointShift)
//   hex:     digits(base 16) · 2^(exponent − 4·pointShift)
struct FloatLiteralParts {
    bool        sign       = false;
    bool        hex        = false;
    std::string digits;                 // integer digits ∥ fraction digits
    long long   pointShift = 0;         // how many of them followed the radix point
    long long   exponent   = 0;         // the explicit e/E (decimal) or p/P (binary) exponent
};

// Parse EXACTLY the floating-constant grammar `std::strtod` accepts, and consume
// the WHOLE body or fail. Matching strtod's shape is deliberate: `decodeFloat`'s
// standing contract is that a body strtod cannot fully parse degrades LOUDLY
// through ok=false, and the target-precision decoder must draw the accept/refuse
// line in the same place — otherwise the `long double` axis would accept or
// refuse literals the `double` axis does not, purely by which decoder ran.
[[nodiscard]] inline bool
parseFloatLiteralBody(std::string_view s, FloatLiteralParts& out) {
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { out.sign = (s[i] == '-'); ++i; }
    auto isDec = [](char c) { return c >= '0' && c <= '9'; };
    auto isHex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    out.hex = (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X'));
    if (out.hex) i += 2;

    std::size_t nDigits = 0;
    bool seenPoint = false;
    for (; i < s.size(); ++i) {
        char const c = s[i];
        if (c == '.') {
            if (seenPoint) return false;
            seenPoint = true;
            continue;
        }
        if (out.hex ? isHex(c) : isDec(c)) {
            out.digits += c;
            ++nDigits;
            if (seenPoint) ++out.pointShift;
            continue;
        }
        break;
    }
    if (nDigits == 0) return false;      // "", ".", "0x", "0x." — no digits at all

    // The exponent. Mandatory in ISO C's hex-float grammar, but strtod accepts a
    // hex float without one (as 2^0), so this decoder does too — the accept/refuse
    // line is strtod's, per the note above.
    if (i < s.size()) {
        char const e = s[i];
        bool const marker = out.hex ? (e == 'p' || e == 'P') : (e == 'e' || e == 'E');
        if (!marker) return false;       // a trailing character the grammar has no place for
        ++i;
        bool negExp = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) { negExp = (s[i] == '-'); ++i; }
        if (i >= s.size() || !isDec(s[i])) return false;   // marker with no digits
        long long acc = 0;
        for (; i < s.size() && isDec(s[i]); ++i) {
            if (acc < 1'000'000'000LL) acc = acc * 10 + (s[i] - '0');   // saturating: the
        }                                                              // magnitude band
        out.exponent = negExp ? -acc : acc;                            // refuses either way
    }
    return i == s.size();
}

// Round the EXACT value `N · 2^scale2` (N a nonzero magnitude) to `kind`, ONCE,
// through `WideFloatValue`'s round-to-nearest-even chokepoint. `ok` is set only
// on a value that is NORMAL and finite in the target format — an overflow to
// infinity and an underflow to subnormal both leave it false, which is the same
// accept/refuse edge `strtod`'s ERANGE draws for `decodeFloat` today.
[[nodiscard]] inline std::optional<WideFloatValue>
roundExactInteger(FloatMag const& N, long long scale2, TypeKind kind, bool sign, bool& ok) {
    int const h = magBitLength(N);
    if (h == 0) { ok = true; return WideFloatValue::zero(kind, sign); }
    // Normalize the top 256 bits into the working register: bit 255 becomes N's
    // leading bit, so value = (W / 2^255) · 2^(h − 1 + scale2).
    int const  lowBit = h - 256;                              // negative ⇒ a left shift
    bool const sticky = magAnyBitBelow(N, lowBit);
    std::uint64_t W[4];
    magExtract256(N, lowBit, W);
    long long const expLL = static_cast<long long>(h) - 1 + scale2;
    if (expLL > 100000 || expLL < -100000) return std::nullopt;   // out of any float range
    auto v = WideFloatValue::fromExactBinary(kind, sign,
                                             static_cast<std::int32_t>(expLL), W, sticky);
    if (!v.has_value() || v->isInfinity()) return std::nullopt;
    ok = true;
    return v;
}

// Round the EXACT value `M / 10^den10` (M nonzero, den10 > 0) to `kind`.
//
// ★ THE SCALE IS CHOSEN SO THE QUOTIENT IS ALWAYS 258 OR 259 BITS, which is the
// whole trick: 256 bits fill the working register and the two or three below it
// fall into the sticky, so no rounding-deciding bit can be lost before
// `fromExactBinary` sees it — and the division REMAINDER is folded in as well, so
// a decimal that merely LOOKS like a tie at 256 bits is correctly known not to be
// one. Everything is exact up to that single rounding.
[[nodiscard]] inline std::optional<WideFloatValue>
roundExactQuotient(FloatMag const& M, long long den10, TypeKind kind, bool sign, bool& ok) {
    FloatMag den{1};
    magMulPow10(den, den10);
    int const bm = magBitLength(M), bd = magBitLength(den);
    long long const s = 258LL + bd - bm;
    FloatMag N = M, D = den;
    if (s >= 0) magShiftLeft(N, static_cast<int>(s));
    else        magShiftLeft(D, static_cast<int>(-s));
    FloatMag Q, R;
    magDivMod(N, D, Q, R);
    int const h = magBitLength(Q);
    if (h < 256) return std::nullopt;      // unreachable by construction — fail loud, not round
    int const  lowBit = h - 256;
    bool const sticky = magAnyBitBelow(Q, lowBit) || !magIsZero(R);
    std::uint64_t W[4];
    magExtract256(Q, lowBit, W);
    long long const expLL = static_cast<long long>(h) - 1 - s;
    if (expLL > 100000 || expLL < -100000) return std::nullopt;
    auto v = WideFloatValue::fromExactBinary(kind, sign,
                                             static_cast<std::int32_t>(expLL), W, sticky);
    if (!v.has_value() || v->isInfinity()) return std::nullopt;
    ok = true;
    return v;
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
    std::string const s = detail::floatLiteralBody(text, ns);
    errno = 0;
    char* end = nullptr;
    double const d = std::strtod(s.c_str(), &end);
    ok = (end == s.c_str() + s.size()) && !s.empty() && errno != ERANGE;
    return d;
}

// ★★ D-CSUBSET-LONG-DOUBLE-LITERAL-DECODE-PRECISION: decode a float literal's
// text at the TARGET's mantissa width, not the host's.
//
// THE DEFECT THIS CLOSES. `decodeFloat` above routes every float literal —
// `long double` included — through `std::strtod`, whose result is a host
// `double`. An l-suffixed literal needing more than 53 mantissa bits therefore
// arrives at the literal LEAF already rounded to binary64, BEFORE any fold:
// LD-3 then folds arithmetic at true 64/113-bit precision over inputs that are
// only binary64-precise, and an unfolded leaf is widened EXACTLY from a value
// that was already wrong. ✔MEASURED at the emitted bytes, `const long double
// g = 0.1L;` on elf64-x86_64: DSS `00 d0 cc cc cc cc cc cc fb 3f` against gcc
// 13.3.0 AND clang 18.1.3 `cd cc cc cc cc cc cc cc fb 3f` — the low ELEVEN bits
// of the significand zeroed, the signature of a binary64 value widened. On
// elf64-aarch64 (binary128) the low SIXTY are zero. A whole number or a
// power-of-two denominator (`20.0L`, `0.5L`) is exact in binary64 and so was
// never affected, which is why the corpus did not catch it.
//
// ⚠ WHY NOT `strtold`. It yields the HOST's long double — 80-bit on Linux/x86,
// 64-bit under MSVC, 128-bit on aarch64 hosts — so the compiler's OUTPUT would
// depend on where the compiler was BUILT, and a cross-compile to the x87-80 axis
// from an MSVC host would silently emit binary64 values. The width here comes
// from `kind`, which the caller resolved through the config-driven float-literal
// ladder (`typeFloatLiteral` → `coreByLongDoubleFormat` → the FORMAT's declared
// `longDoubleFormat` axis) — a closed `TypeKind`, never a host `#ifdef` and
// never an arch/format name. ✔MEASURED that the axis really does diverge: MSVC
// 14.51.36231 puts EIGHT bytes in `long double` and decodes `0.1L` to
// `9a 99 99 99 99 99 99 3f`, identical to `double`.
//
// THE METHOD. The literal is an EXACT rational — decimal digits `M` times a
// power of ten, or hex digits times a power of two — so it is evaluated exactly
// in arbitrary precision and rounded ONCE, at the target's significand width,
// through `WideFloatValue::fromExactBinary` (the SAME round-to-nearest-even
// chokepoint add/sub/mul/div use; no second rounding implementation lives here).
// A power of ten in the DENOMINATOR is handled by an exact long division whose
// remainder becomes the sticky bit, so a decimal that is not a true tie can
// never be mistaken for one.
//
// `ok` keeps `decodeFloat`'s contract exactly: false ⇐ a body this grammar
// cannot FULLY consume, a value outside the target's NORMAL range (overflow to
// infinity / underflow to subnormal — the same edges strtod's ERANGE refuses
// today), or a kind the wide-float kernel does not realize. Never a silent zero,
// never a silently truncated value. The caller owns the diagnostic.
[[nodiscard]] inline std::optional<WideFloatValue>
decodeFloatWide(std::string_view text, NumberStyle const* ns, TypeKind kind, bool& ok) {
    ok = false;
    if (!WideFloatValue::isSupportedKind(kind)) return std::nullopt;

    std::string const           body = detail::floatLiteralBody(text, ns);
    detail::FloatLiteralParts   p;
    if (!detail::parseFloatLiteralBody(body, p)) return std::nullopt;

    // Leading zeros do not change the value and must go before the digit count
    // becomes a magnitude estimate. `pointShift` counts digits AFTER the point
    // and is unaffected by the strip (`0.001` → digits "1", pointShift 3).
    std::size_t const firstSig = p.digits.find_first_not_of('0');
    if (firstSig == std::string::npos) {          // every digit a zero
        ok = true;
        return WideFloatValue::zero(kind, p.sign);
    }
    p.digits.erase(0, firstSig);

    if (p.hex) {
        // Exact by construction: H · 2^(pexp − 4·fracHexDigits). No power of ten,
        // so no division and no magnitude short-circuit is needed — the binary
        // exponent only ADDS to the result exponent.
        detail::FloatMag const H = detail::digitsToMag(p.digits, 16);
        return detail::roundExactInteger(H, p.exponent - 4 * p.pointShift,
                                         kind, p.sign, ok);
    }

    long long const scale  = p.exponent - p.pointShift;     // value = M · 10^scale
    long long const decade = scale + static_cast<long long>(p.digits.size());
    // 10^(decade−1) ≤ |value| < 10^decade. The F80/F128 NORMAL range is
    // [2^-16382, 2^16384) ≈ [3.4e-4932, 1.2e4932), so a decade outside ±5000 is
    // decidably out of range — refused here rather than built as a multi-million-
    // bit integer first. Inside the band the exact path runs and `roundNormal`
    // renders the real verdict.
    constexpr long long kDecadeBand = 5000;
    if (decade > kDecadeBand || decade < -kDecadeBand) return std::nullopt;
    // ⚠ A DIGIT COUNT IS ALSO A WORK BOUND. Truncating significant digits is NOT
    // safe near an exact tie (the kept prefix can BE the tie while the true value
    // is below it), so the digits are all kept and the absurd case is refused
    // LOUDLY instead — never silently rounded from a prefix. The band above then
    // bounds the denominator too (10^(digits+5000)).
    constexpr std::size_t kMaxSignificantDigits = 20000;
    if (p.digits.size() > kMaxSignificantDigits) return std::nullopt;

    detail::FloatMag M = detail::digitsToMag(p.digits, 10);
    if (scale >= 0) {
        detail::magMulPow10(M, scale);                       // exact integer
        return detail::roundExactInteger(M, 0, kind, p.sign, ok);
    }
    return detail::roundExactQuotient(M, -scale, kind, p.sign, ok);
}

} // namespace dss
