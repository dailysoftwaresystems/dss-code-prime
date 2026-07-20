#pragma once

// ── LD-3 (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION): the host compile-time
// wide-float value type for TRUE 80/128-bit constant folding ────────────────
//
// A pure HOST value type carrying an F80 (x87 80-bit extended) or F128 (IEEE
// binary128) floating value at its TARGET precision, for the const-fold engines
// (`const_eval.cpp` / `cst_const_eval.cpp`). ZERO target / language / object-
// format identity — this is compile-time host arithmetic keyed ONLY on the
// closed `TypeKind::F80` / `TypeKind::F128` enums (never an arch/format/
// target.name branch), the exact agnosticism discipline `BitIntValue` follows.
//
// WHY: the const-evaluator carries floats as a host `double` (53-bit mantissa),
// so folding `20.0L + 22.0L` at binary64 would bake a SILENTLY-ROUNDED constant
// for any value needing >53 mantissa bits — the reason `refusesHostFloatFold`
// walls F80/F128 today. This kernel folds at the real 64-bit (F80) / 113-bit
// (F128) significand precision with correct IEEE 754 round-to-nearest-even, so
// the gate can be relaxed for the realized kinds.
//
// DESIGN (mirrors `BitIntValue` — pure-host, header-only, a NEW `std::variant`
// arm on the literal pools, portable 64×64→128 multiply via 32-bit halves that
// deliberately avoids `__int128`/`_umul128`, "new arm fails loud on un-updated
// consumers"):
//   * ONE unified engine parameterized by `significandBits()` (64 for F80, 113
//     for F128) — add/sub/mul/div/round written ONCE (F80 is the degenerate
//     high-guard case of the same round-to-nearest-even), only pack/unpack is
//     kind-specific. No two engines (that would duplicate the highest-risk
//     surface: guard/round/sticky, tie-to-even, carry-out renormalization).
//   * Significand carried in a UNIFORM left-justified 128-bit register
//     (sigHi_:sigLo_) with the explicit integer (leading) bit at bit 63 of
//     sigHi_ (= bit 127 of the pair). F80 uses the top 64 bits (sigLo_ == 0);
//     F128 uses the top 113 bits (low 15 bits of sigLo_ == 0). Arithmetic runs
//     in a 256-bit working register (integer bit at bit 255) giving ≥143 guard
//     bits, so alignment/normalization never lose a round-determining bit; the
//     far tail collapses into ONE sticky flag.
//
// FAIL-LOUD, NO SILENT MIS-FOLD: add/sub/mul/div return `std::optional` —
// nullopt ⇐ a subnormal RESULT (true exponent underflows the normal range;
// astronomically unlikely, `D-CSUBSET-LONG-DOUBLE-CONSTFOLD-SUBNORMAL-RESULT`)
// OR a defensive cross-kind mismatch (F80 vs F128, unreachable via source). The
// caller maps nullopt to `ConstEvalFailure::UnsupportedTypeKind` (no new enum).
// Overflow → infinity (IN scope). Float div-by-zero → signed inf / NaN (IEEE),
// NOT a DivisionByZero (that is the int path). A subnormal double INPUT widen IS
// handled (renormalized into a normal F80/F128 — the wider exponent absorbs it,
// exactly as `appendF80Extended`/`appendF128` do).

#include "core/types/type_lattice/core_type.hpp"   // TypeKind

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace dss {

class WideFloatValue {
public:
    enum class Class : std::uint8_t { Zero, Normal, Infinity, NaN };
    enum class Ordering : std::uint8_t { Less, Equal, Greater, Unordered };

    WideFloatValue() = default;

    // The SINGLE gate source — every branch keying on "is this a soft-float
    // kind" asks HERE (never an arch/format string). F80 || F128, closed enums.
    [[nodiscard]] static constexpr bool isSupportedKind(TypeKind k) noexcept {
        return k == TypeKind::F80 || k == TypeKind::F128;
    }
    [[nodiscard]] TypeKind kind() const noexcept { return kind_; }

    // Significand precision (with the explicit leading bit): F80 = 64, F128 = 113.
    [[nodiscard]] static constexpr int significandBits(TypeKind k) noexcept {
        return k == TypeKind::F128 ? 113 : 64;   // F80 (and the defensive default) → 64
    }
    [[nodiscard]] int significandBits() const noexcept { return significandBits(kind_); }

    // ── Constructors (all set kind_; the default-ctor F64 value is inert — every
    // real value flows through one of these or the pool's widen-from-double) ──
    [[nodiscard]] static WideFloatValue zero(TypeKind k, bool sign) noexcept {
        WideFloatValue v; v.kind_ = k; v.class_ = Class::Zero; v.sign_ = sign; return v;
    }
    [[nodiscard]] static WideFloatValue infinity(TypeKind k, bool sign) noexcept {
        WideFloatValue v; v.kind_ = k; v.class_ = Class::Infinity; v.sign_ = sign; return v;
    }
    [[nodiscard]] static WideFloatValue nan(TypeKind k) noexcept {
        WideFloatValue v; v.kind_ = k; v.class_ = Class::NaN; return v;
    }

    // Widen a host `double` (binary64) EXACTLY into F80/F128 (53-bit mantissa ⊆
    // 64/113-bit — no rounding). A subnormal double INPUT renormalizes into a
    // normal wide value (the wider exponent absorbs it). This is the ALWAYS-EXACT
    // path the pre-existing `double` pool arm relies on for an unfolded leaf.
    [[nodiscard]] static WideFloatValue fromDouble(double d, TypeKind k) noexcept {
        WideFloatValue v; v.kind_ = k;
        std::uint64_t bits = 0; std::memcpy(&bits, &d, sizeof(double));
        v.sign_ = (bits >> 63) & 1u;
        std::uint32_t const exp11 = static_cast<std::uint32_t>((bits >> 52) & 0x7FFull);
        std::uint64_t const frac52 = bits & 0x000F'FFFF'FFFF'FFFFull;
        if (exp11 == 0x7FFu) {
            v.class_ = (frac52 == 0) ? Class::Infinity : Class::NaN;
            return v;
        }
        if (exp11 == 0) {
            if (frac52 == 0) { v.class_ = Class::Zero; return v; }
            // Subnormal double (value = frac52 · 2^-1074): normalize the highest
            // set bit up to bit 63. frac52 < 2^52 ⇒ countl_zero ≥ 12.
            int const shift = std::countl_zero(frac52);
            int const highBit = 63 - shift;            // 0-indexed MSB of frac52
            v.class_    = Class::Normal;
            v.sigHi_    = frac52 << shift;             // MSB now at bit 63
            v.sigLo_    = 0;
            v.exponent_ = highBit - 1074;
            return v;
        }
        v.class_    = Class::Normal;
        v.exponent_ = static_cast<std::int32_t>(exp11) - 1023;
        v.sigHi_    = (std::uint64_t{1} << 63) | (frac52 << 11);   // explicit integer bit
        v.sigLo_    = 0;
        return v;
    }

    // Convert a host integer EXACTLY into F80/F128 (the Int→Float cast quadrant).
    // ALWAYS exact — a 64-bit magnitude fits the 64-bit (F80) / 113-bit (F128)
    // significand with room to spare (UNLIKE int→binary64, which loses >53 bits;
    // that is why the fold must not route int→long-double through a host `double`).
    [[nodiscard]] static WideFloatValue fromUint64(std::uint64_t mag, bool sign, TypeKind k) noexcept {
        WideFloatValue v; v.kind_ = k;
        if (mag == 0) { v.class_ = Class::Zero; v.sign_ = sign; return v; }
        v.class_    = Class::Normal;
        v.sign_     = sign;
        int const highBit = 63 - std::countl_zero(mag);   // mag != 0
        v.sigHi_    = mag << (63 - highBit);               // MSB → bit 63 (explicit integer bit)
        v.sigLo_    = 0;
        v.exponent_ = highBit;
        return v;
    }
    [[nodiscard]] static WideFloatValue fromInt64(std::int64_t v, TypeKind k) noexcept {
        bool const sign = v < 0;
        std::uint64_t const mag = sign ? (~static_cast<std::uint64_t>(v) + 1u)   // -v (INT64_MIN-safe)
                                       : static_cast<std::uint64_t>(v);
        return fromUint64(mag, sign, k);
    }

    [[nodiscard]] bool isZero()     const noexcept { return class_ == Class::Zero; }
    [[nodiscard]] bool isNaN()      const noexcept { return class_ == Class::NaN; }
    [[nodiscard]] bool isInfinity() const noexcept { return class_ == Class::Infinity; }

    // Exact sign flip (the unary Neg fold). NaN keeps its (unspecified) sign.
    [[nodiscard]] WideFloatValue negate() const noexcept {
        WideFloatValue v = *this; v.sign_ = !v.sign_; return v;
    }

    // ── Arithmetic (require both operands the SAME kind — the caller homogenizes
    // and does the F80-vs-F128 defensive refuse). nullopt ⇐ subnormal result /
    // cross-kind. Overflow → inf. Div-by-zero → signed inf / NaN. ──────────────
    [[nodiscard]] static std::optional<WideFloatValue>
    add(WideFloatValue const& a, WideFloatValue const& b) noexcept { return addSub(a, b, false); }
    [[nodiscard]] static std::optional<WideFloatValue>
    sub(WideFloatValue const& a, WideFloatValue const& b) noexcept { return addSub(a, b, true); }

    [[nodiscard]] static std::optional<WideFloatValue>
    mul(WideFloatValue const& a, WideFloatValue const& b) noexcept {
        if (a.kind_ != b.kind_ || !isSupportedKind(a.kind_)) return std::nullopt;
        TypeKind const k = a.kind_;
        bool const sign = a.sign_ ^ b.sign_;
        if (a.class_ == Class::NaN || b.class_ == Class::NaN) return nan(k);
        if (a.class_ == Class::Infinity || b.class_ == Class::Infinity) {
            if (a.class_ == Class::Zero || b.class_ == Class::Zero) return nan(k);   // inf·0
            return infinity(k, sign);
        }
        if (a.class_ == Class::Zero || b.class_ == Class::Zero) return zero(k, sign);
        // Both normal: 128×128 → 256 product of the significands.
        std::uint64_t P[4];
        mul128(a.sigHi_, a.sigLo_, b.sigHi_, b.sigLo_, P);
        std::int32_t exp = a.exponent_ + b.exponent_;
        // Product of two [2^127,2^128) significands ∈ [2^254, 2^256): leading bit
        // at 254 or 255. Bring it to bit 255 (the normalized integer-bit slot).
        if (getBit(P, 255)) { exp += 1; }      // leading already at 255
        else { shl(P, 1); }                    // leading at 254 → shift up 1
        return roundNormal(k, sign, exp, P, /*extSticky=*/false);
    }

    [[nodiscard]] static std::optional<WideFloatValue>
    div(WideFloatValue const& a, WideFloatValue const& b) noexcept {
        if (a.kind_ != b.kind_ || !isSupportedKind(a.kind_)) return std::nullopt;
        TypeKind const k = a.kind_;
        bool const sign = a.sign_ ^ b.sign_;
        if (a.class_ == Class::NaN || b.class_ == Class::NaN) return nan(k);
        if (a.class_ == Class::Infinity) {
            if (b.class_ == Class::Infinity) return nan(k);   // inf/inf
            return infinity(k, sign);                          // inf/finite
        }
        if (b.class_ == Class::Infinity) return zero(k, sign); // finite/inf
        if (b.class_ == Class::Zero) {
            if (a.class_ == Class::Zero) return nan(k);        // 0/0
            return infinity(k, sign);                          // x/0 → signed inf (IEEE)
        }
        if (a.class_ == Class::Zero) return zero(k, sign);     // 0/x
        // Both normal: Q = (Sa << 128) / Sb, bit-at-a-time; remainder → sticky.
        std::uint64_t N[4]  = {0, 0, a.sigLo_, a.sigHi_};   // Sa << 128 (top words)
        std::uint64_t Dv[4] = {b.sigLo_, b.sigHi_, 0, 0};   // Sb (low 128 bits)
        std::uint64_t Q[4]  = {0, 0, 0, 0};
        std::uint64_t R[4]  = {0, 0, 0, 0};
        for (int i = 255; i >= 0; --i) {
            shl1(R);
            if (getBit(N, i)) R[0] |= 1u;
            if (geq(R, Dv)) { subFrom(R, Dv); setBit(Q, i); }
        }
        bool const sticky = !isZeroReg(R);
        std::int32_t const rawExp = a.exponent_ - b.exponent_;
        // Sa/Sb ∈ (0.5, 2) ⇒ Q = (Sa<<128)/Sb ∈ (2^127, 2^129): normalize the
        // leading bit to 255. value = Q·2^(rawExp-128) = Qnorm·2^(E-255) with
        // E = rawExp + 127 - shiftLeft.
        int const h = highestSetBit(Q);
        int const shiftLeft = 255 - h;
        shl(Q, shiftLeft);
        std::int32_t const E = rawExp + 127 - shiftLeft;
        return roundNormal(k, sign, E, Q, sticky);
    }

    // IEEE 754 total-ish comparison (the 6 relational folds route through this).
    // NaN → Unordered; +0 == -0.
    [[nodiscard]] static Ordering compare(WideFloatValue const& a, WideFloatValue const& b) noexcept {
        if (a.class_ == Class::NaN || b.class_ == Class::NaN) return Ordering::Unordered;
        int const as = signum(a), bs = signum(b);
        if (as != bs) return as < bs ? Ordering::Less : Ordering::Greater;
        if (as == 0) return Ordering::Equal;   // both zero (any sign)
        int const cm = compareMagnitudeAny(a, b);
        if (cm == 0) return Ordering::Equal;
        bool greater = (cm > 0);
        if (as < 0) greater = !greater;         // negative: larger magnitude = smaller
        return greater ? Ordering::Greater : Ordering::Less;
    }

    // Truncate toward zero to an int64 (C99 §6.3.1.4). nullopt for NaN / Inf /
    // out-of-range. The range check is done AT THE OPERAND'S OWN PRECISION (never
    // narrow-to-double first — the 2^63 boundary would flip sign).
    [[nodiscard]] std::optional<std::int64_t> toInt64() const noexcept {
        if (class_ == Class::NaN || class_ == Class::Infinity) return std::nullopt;
        if (class_ == Class::Zero) return std::int64_t{0};
        if (exponent_ < 0)  return std::int64_t{0};    // |v| < 1 truncates to 0
        if (exponent_ > 63) return std::nullopt;        // |v| ≥ 2^64 — out of range
        int const sh = 127 - exponent_;                 // in [64, 127]
        std::uint64_t const mag = sigHi_ >> (sh - 64);  // sh-64 ∈ [0,63]; discards fraction (trunc)
        if (!sign_) {
            if (mag > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                return std::nullopt;
            return static_cast<std::int64_t>(mag);
        }
        std::uint64_t const int64MinMag =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u;  // 2^63
        if (mag > int64MinMag) return std::nullopt;
        if (mag == int64MinMag) return std::numeric_limits<std::int64_t>::min();
        return -static_cast<std::int64_t>(mag);
    }

    // Narrow to a host `double` (binary64) with round-to-nearest-even. Used by
    // the F80/F128 → F64 cast quadrant (the F32/F16 narrow then routes the result
    // through the existing `narrowToFloatWidth`).
    [[nodiscard]] double toDouble() const noexcept {
        if (class_ == Class::Zero) return sign_ ? -0.0 : 0.0;
        if (class_ == Class::Infinity) {
            double const inf = std::numeric_limits<double>::infinity();
            return sign_ ? -inf : inf;
        }
        if (class_ == Class::NaN) return std::numeric_limits<double>::quiet_NaN();

        // value = ±T·2^(exponent_-127) with T = sigHi_:sigLo_ (128-bit, integer
        // bit at 127). Decide F64 normal vs subnormal from the UNROUNDED exponent
        // FIRST — rounding at the 53-bit (normal) position before this test then
        // re-shifting into a subnormal DOUBLE-rounds (and truncates the second
        // step). The two regimes round ONCE, each at its own grid.
        std::int32_t const expField0 = exponent_ + 1023;

        if (expField0 <= 0) {
            // ── F64 SUBNORMAL / underflow ─────────────────────────────────────
            // Round the FULL significand to the subnormal grid (LSB = 2^-1074)
            // with ONE round-to-nearest-even. The subnormal LSB sits `rshift` bits
            // up from T bit 0; expField0 ≤ 0 ⇒ rshift ≥ 76, so the kept integer
            // part lies entirely in sigHi_ and the guard/round/sticky tail spans
            // the low sigHi_ bits + ALL of sigLo_ (F128's sigLo_ fraction MUST feed
            // the sticky — a naïve `mant`-only path would drop it).
            int const rshift = 76 - expField0;             // ≥ 76
            if (rshift > 128) return sign_ ? -0.0 : 0.0;   // < ½·min-subnormal → 0
            std::uint64_t m;
            bool roundBit;
            bool stickyBit;
            if (rshift <= 127) {
                int const s = rshift - 64;                 // kept-LSB bit index in sigHi_ (∈[12,63])
                m         = sigHi_ >> s;                    // ≤ 52 bits
                roundBit  = ((sigHi_ >> (s - 1)) & 1u) != 0;
                std::uint64_t const belowMask = (std::uint64_t{1} << (s - 1)) - 1u;
                stickyBit = ((sigHi_ & belowMask) != 0) || (sigLo_ != 0);
            } else {                                       // rshift == 128: round bit IS the integer bit
                m         = 0;
                roundBit  = true;                          // T bit 127 (a normal value's integer bit)
                stickyBit = ((sigHi_ & 0x7FFF'FFFF'FFFF'FFFFull) != 0) || (sigLo_ != 0);
            }
            if (roundBit && (stickyBit || (m & 1u) != 0)) {
                m += 1;   // RNE; m may reach 2^52 → smallest NORMAL (bit 52 spills into the exp field)
            }
            std::uint64_t const bits = (static_cast<std::uint64_t>(sign_) << 63) | m;
            double out; std::memcpy(&out, &bits, sizeof(double)); return out;
        }

        // ── F64 NORMAL: keep 53 bits (52 fraction), round at sigHi_[10]; sticky =
        // sigHi_[9..0] | sigLo_. A carry-out renormalizes (exp++), possibly to inf.
        std::uint64_t mant = (sigHi_ >> 11) & 0x000F'FFFF'FFFF'FFFFull;   // 52 bits
        bool const roundBit = ((sigHi_ >> 10) & 1u) != 0;
        bool const sticky   = ((sigHi_ & 0x3FFull) != 0) || (sigLo_ != 0);
        bool const lsb      = (mant & 1u) != 0;
        std::int32_t expField = expField0;
        if (roundBit && (sticky || lsb)) {
            mant += 1;
            if ((mant >> 52) != 0) { mant = 0; expField += 1; }   // mantissa carry-out
        }
        if (expField >= 0x7FF) {                            // overflow → inf
            double const inf = std::numeric_limits<double>::infinity();
            return sign_ ? -inf : inf;
        }
        std::uint64_t const bits = (static_cast<std::uint64_t>(sign_) << 63)
                                 | (static_cast<std::uint64_t>(expField) << 52) | mant;
        double out; std::memcpy(&out, &bits, sizeof(double)); return out;
    }

    // The on-disk bit pattern as {lo, hi} — byte-layout EXACTLY matching
    // `appendF80Extended` / `appendF128` (the asm producers). `appendWideFloatBits`
    // writes lo (8 LE bytes) then hi (8 LE bytes) = the 16-byte slot.
    //   F80:  lo = 64-bit significand (explicit integer bit); hi = sign|exp in the
    //         low 16 bits, top 48 bits zero (the 6-byte pad).
    //   F128: lo = fraction bits 0..63; hi = sign|exp|fraction bits 64..111.
    struct Packed { std::uint64_t lo; std::uint64_t hi; };
    [[nodiscard]] Packed pack() const noexcept {
        return kind_ == TypeKind::F128 ? packF128() : packF80();
    }

    // The exact inverse of pack() — reconstruct a value from its {lo, hi} on-disk
    // bit pattern + kind (the `.dsshir`/`.dssir` text round-trip reader). pack ∘
    // fromPacked == identity for every representable value (pinned in the test).
    [[nodiscard]] static WideFloatValue fromPacked(std::uint64_t lo, std::uint64_t hi,
                                                   TypeKind k) noexcept {
        return k == TypeKind::F128 ? unpackF128(lo, hi) : unpackF80(lo, hi);
    }

    // Bit-exact equality (the pool round-trip identity check — NOT IEEE compare,
    // which is `compare`). Defaulted: two values are == iff every field matches.
    friend bool operator==(WideFloatValue const&, WideFloatValue const&) = default;

private:
    bool          sign_     = false;
    Class         class_    = Class::Zero;
    std::int32_t  exponent_ = 0;              // unbiased (value = ±sig·2^exponent_)
    std::uint64_t sigHi_    = 0;              // significand, integer bit at bit 63
    std::uint64_t sigLo_    = 0;              // low half (F128 uses bits 63..15; F80 = 0)
    TypeKind      kind_     = TypeKind::F64;  // inert default; isSupportedKind gates

    static constexpr std::int32_t kExpBias =  16383;
    static constexpr std::int32_t kMaxExp  =  16383;   // max normal unbiased exponent
    static constexpr std::int32_t kMinExp  = -16382;   // min normal unbiased exponent

    // ── Portable 64×64 → 128 (lo, hi) via 32-bit halves — no __uint128/_umul128
    // (MSVC + clang + gcc compile this identically; the `BitIntValue::mul64`
    // precedent, duplicated per the codebase convention). ──────────────────────
    static void mul64(std::uint64_t a, std::uint64_t b,
                      std::uint64_t& lo, std::uint64_t& hi) noexcept {
        std::uint64_t const aL = a & 0xFFFFFFFFull, aH = a >> 32;
        std::uint64_t const bL = b & 0xFFFFFFFFull, bH = b >> 32;
        std::uint64_t const ll = aL * bL, lh = aL * bH, hl = aH * bL, hh = aH * bH;
        std::uint64_t const cross = (ll >> 32) + (lh & 0xFFFFFFFFull) + (hl & 0xFFFFFFFFull);
        lo = (ll & 0xFFFFFFFFull) | (cross << 32);
        hi = hh + (lh >> 32) + (hl >> 32) + (cross >> 32);
    }

    // 128×128 → 256 (P[0..3], P[3] most significant).
    static void mul128(std::uint64_t aHi, std::uint64_t aLo,
                       std::uint64_t bHi, std::uint64_t bLo, std::uint64_t* P) noexcept {
        std::uint64_t ll_lo, ll_hi; mul64(aLo, bLo, ll_lo, ll_hi);
        std::uint64_t lh_lo, lh_hi; mul64(aLo, bHi, lh_lo, lh_hi);
        std::uint64_t hl_lo, hl_hi; mul64(aHi, bLo, hl_lo, hl_hi);
        std::uint64_t hh_lo, hh_hi; mul64(aHi, bHi, hh_lo, hh_hi);
        std::uint64_t const p0 = ll_lo;
        std::uint64_t p1 = ll_hi, t, c = 0;
        t = p1 + lh_lo; c += (t < p1) ? 1u : 0u; p1 = t;
        t = p1 + hl_lo; c += (t < p1) ? 1u : 0u; p1 = t;
        std::uint64_t const carry1 = c;
        std::uint64_t p2 = lh_hi; c = 0;
        t = p2 + hl_hi;  c += (t < p2) ? 1u : 0u; p2 = t;
        t = p2 + hh_lo;  c += (t < p2) ? 1u : 0u; p2 = t;
        t = p2 + carry1; c += (t < p2) ? 1u : 0u; p2 = t;
        std::uint64_t const p3 = hh_hi + c;
        P[0] = p0; P[1] = p1; P[2] = p2; P[3] = p3;
    }

    // ── 256-bit working-register primitives (W[3] most significant) ───────────
    static bool getBit(std::uint64_t const* W, int bit) noexcept {
        return ((W[bit >> 6] >> (bit & 63)) & 1u) != 0;
    }
    static void setBit(std::uint64_t* W, int bit) noexcept {
        W[bit >> 6] |= (std::uint64_t{1} << (bit & 63));
    }
    static bool isZeroReg(std::uint64_t const* W) noexcept {
        return (W[0] | W[1] | W[2] | W[3]) == 0;
    }
    static int highestSetBit(std::uint64_t const* W) noexcept {   // -1 if all zero
        for (int i = 3; i >= 0; --i) if (W[i] != 0) return i * 64 + 63 - std::countl_zero(W[i]);
        return -1;
    }
    static bool geq(std::uint64_t const* a, std::uint64_t const* b) noexcept {   // a >= b
        for (int i = 3; i >= 0; --i) if (a[i] != b[i]) return a[i] > b[i];
        return true;
    }
    static void subFrom(std::uint64_t* a, std::uint64_t const* b) noexcept {     // a -= b (a >= b)
        std::uint64_t borrow = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint64_t const d1 = a[i] - b[i]; std::uint64_t const b1 = (a[i] < b[i]) ? 1u : 0u;
            std::uint64_t const d2 = d1 - borrow; std::uint64_t const b2 = (d1 < borrow) ? 1u : 0u;
            a[i] = d2; borrow = b1 + b2;
        }
    }
    static std::uint64_t add(std::uint64_t* a, std::uint64_t const* b) noexcept {  // a += b; ret carry-out
        std::uint64_t carry = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint64_t const s1 = a[i] + b[i]; std::uint64_t const c1 = (s1 < a[i]) ? 1u : 0u;
            std::uint64_t const s2 = s1 + carry; std::uint64_t const c2 = (s2 < s1) ? 1u : 0u;
            a[i] = s2; carry = c1 + c2;
        }
        return carry;
    }
    static void subOne(std::uint64_t* W) noexcept {   // W -= 1
        for (int i = 0; i < 4; ++i) { std::uint64_t const before = W[i]; W[i] = before - 1u; if (before != 0) break; }
    }
    static std::uint64_t shl1(std::uint64_t* W) noexcept {   // W <<= 1; ret bit shifted out of top
        std::uint64_t carry = 0;
        for (int i = 0; i < 4; ++i) { std::uint64_t const nc = W[i] >> 63; W[i] = (W[i] << 1) | carry; carry = nc; }
        return carry;
    }
    static bool shr(std::uint64_t* W, int d) noexcept {   // W >>= d; ret sticky (any 1 shifted out)
        if (d <= 0) return false;
        if (d >= 256) { bool const s = !isZeroReg(W); W[0] = W[1] = W[2] = W[3] = 0; return s; }
        bool sticky = false;
        for (int bit = 0; bit < d; ++bit) if (getBit(W, bit)) { sticky = true; break; }
        int const ws = d >> 6, bs = d & 63;
        std::uint64_t out[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            std::uint64_t const lo = (i + ws < 4)     ? W[i + ws]     : 0u;
            std::uint64_t const hi = (i + ws + 1 < 4) ? W[i + ws + 1] : 0u;
            out[i] = (bs == 0) ? lo : ((lo >> bs) | (hi << (64 - bs)));
        }
        for (int i = 0; i < 4; ++i) W[i] = out[i];
        return sticky;
    }
    static void shl(std::uint64_t* W, int k) noexcept {   // W <<= k (k ≤ leading zeros; no loss)
        if (k <= 0) return;
        if (k >= 256) { W[0] = W[1] = W[2] = W[3] = 0; return; }
        int const ws = k >> 6, bs = k & 63;
        std::uint64_t out[4] = {0, 0, 0, 0};
        for (int i = 3; i >= 0; --i) {
            std::uint64_t const lo  = (i - ws >= 0)     ? W[i - ws]     : 0u;
            std::uint64_t const hiw = (i - ws - 1 >= 0) ? W[i - ws - 1] : 0u;
            out[i] = (bs == 0) ? lo : ((lo << bs) | (hiw >> (64 - bs)));
        }
        for (int i = 0; i < 4; ++i) W[i] = out[i];
    }
    static bool anyBitsBelow(std::uint64_t const* W, int pos) noexcept {   // any 1 in [0, pos)
        for (int bit = 0; bit < pos; ++bit) if (getBit(W, bit)) return true;
        return false;
    }
    static void clearBitsBelow(std::uint64_t* W, int pos) noexcept {       // clear [0, pos)
        for (int bit = 0; bit < pos; ++bit) W[bit >> 6] &= ~(std::uint64_t{1} << (bit & 63));
    }
    static bool addAt(std::uint64_t* W, int pos) noexcept {   // W += (1<<pos); ret carry-out of bit 255
        int const word = pos >> 6, off = pos & 63;
        std::uint64_t carry = std::uint64_t{1} << off;
        for (int i = word; i < 4 && carry; ++i) {
            std::uint64_t const before = W[i]; W[i] = before + carry; carry = (W[i] < before) ? 1u : 0u;
        }
        return carry != 0;
    }

    static void load(WideFloatValue const& v, std::uint64_t* W) noexcept {   // significand → 256-bit reg (bit 255 = integer bit)
        W[3] = v.sigHi_; W[2] = v.sigLo_; W[1] = 0; W[0] = 0;
    }

    // ★ THE ROUND-TO-NEAREST-EVEN CHOKEPOINT — the single shared rounding surface
    // for add/sub/mul/div, written ONCE. `W` must be NORMALIZED (integer bit at
    // bit 255). Keeps the top `p` bits; guard/round/sticky come from below plus
    // `extSticky`. Tie-to-even; carry-out of rounding renormalizes (exp++).
    // Post-op exponent guard: overflow → inf; subnormal result → nullopt (loud).
    [[nodiscard]] static std::optional<WideFloatValue>
    roundNormal(TypeKind k, bool sign, std::int32_t exp, std::uint64_t* W, bool extSticky) noexcept {
        int const p        = significandBits(k);
        int const keptLSB  = 256 - p;         // 192 (F80) / 143 (F128)
        int const roundPos = keptLSB - 1;
        bool const lsb      = getBit(W, keptLSB);
        bool const roundBit = getBit(W, roundPos);
        bool const sticky   = extSticky || anyBitsBelow(W, roundPos);
        clearBitsBelow(W, keptLSB);           // truncate to the kept significand
        if (roundBit && (sticky || lsb)) {
            if (addAt(W, keptLSB)) {          // carry out of bit 255 → significand became 2.0
                W[0] = W[1] = W[2] = 0; W[3] = std::uint64_t{1} << 63; exp += 1;
            }
        }
        if (exp > kMaxExp) return infinity(k, sign);   // overflow → inf (in scope)
        if (exp < kMinExp) return std::nullopt;        // subnormal result → fail loud
        WideFloatValue v; v.kind_ = k; v.class_ = Class::Normal; v.sign_ = sign;
        v.exponent_ = exp; v.sigHi_ = W[3]; v.sigLo_ = W[2];
        return v;
    }

    // add/sub dispatch — a - b ≡ a + (-b); route same-sign to magnitude-add,
    // opposite-sign to magnitude-subtract. Specials (NaN/Inf/Zero) handled first.
    [[nodiscard]] static std::optional<WideFloatValue>
    addSub(WideFloatValue const& a, WideFloatValue const& bIn, bool subtract) noexcept {
        if (a.kind_ != bIn.kind_ || !isSupportedKind(a.kind_)) return std::nullopt;
        TypeKind const k = a.kind_;
        WideFloatValue b = bIn;
        if (subtract) b.sign_ = !b.sign_;
        if (a.class_ == Class::NaN || b.class_ == Class::NaN) return nan(k);
        if (a.class_ == Class::Infinity || b.class_ == Class::Infinity) {
            if (a.class_ == Class::Infinity && b.class_ == Class::Infinity)
                return (a.sign_ != b.sign_) ? nan(k) : infinity(k, a.sign_);   // inf + -inf = NaN
            return a.class_ == Class::Infinity ? infinity(k, a.sign_) : infinity(k, b.sign_);
        }
        if (a.class_ == Class::Zero && b.class_ == Class::Zero)
            return zero(k, a.sign_ && b.sign_);     // -0 only when both -0 (RNE default)
        if (a.class_ == Class::Zero) return b;
        if (b.class_ == Class::Zero) return a;
        return (a.sign_ == b.sign_) ? addMagnitudes(k, a.sign_, a, b) : subMagnitudes(k, a, b);
    }

    // Same-sign: align the smaller exponent down (sticky), add, handle the
    // carry-out (exp++), round.
    [[nodiscard]] static std::optional<WideFloatValue>
    addMagnitudes(TypeKind k, bool sign, WideFloatValue const& a, WideFloatValue const& b) noexcept {
        std::uint64_t Wa[4], Wb[4]; load(a, Wa); load(b, Wb);
        std::int32_t exp;
        bool sticky;
        if (a.exponent_ >= b.exponent_) { sticky = shr(Wb, a.exponent_ - b.exponent_); exp = a.exponent_; }
        else                            { sticky = shr(Wa, b.exponent_ - a.exponent_); exp = b.exponent_; }
        if (add(Wa, Wb) != 0) {                 // carry out of bit 255 → significand ∈ [2,4)
            bool const s2 = shr(Wa, 1); setBit(Wa, 255); exp += 1; sticky = sticky || s2;
        }
        return roundNormal(k, sign, exp, Wa, sticky);
    }

    // Opposite-sign: subtract the smaller magnitude from the larger; the aligned
    // sticky forces a borrow (residual → sticky); normalize the cancellation; round.
    [[nodiscard]] static std::optional<WideFloatValue>
    subMagnitudes(TypeKind k, WideFloatValue const& a, WideFloatValue const& b) noexcept {
        int const cmp = compareMagnitude(a, b);
        if (cmp == 0) return zero(k, false);     // equal magnitude, opposite sign → +0
        WideFloatValue const& big   = (cmp > 0) ? a : b;
        WideFloatValue const& small = (cmp > 0) ? b : a;
        std::uint64_t Wbig[4], Wsmall[4]; load(big, Wbig); load(small, Wsmall);
        std::int32_t exp = big.exponent_;
        bool const alignSticky = shr(Wsmall, big.exponent_ - small.exponent_);
        subFrom(Wbig, Wsmall);
        bool sticky = false;
        if (alignSticky) { subOne(Wbig); sticky = true; }   // borrow-from-sticky
        int const h = highestSetBit(Wbig);
        if (h < 0) return zero(k, false);        // total cancellation (defensive)
        int const shiftLeft = 255 - h;
        if (shiftLeft > 0) { shl(Wbig, shiftLeft); exp -= shiftLeft; }
        return roundNormal(k, big.sign_, exp, Wbig, sticky);
    }

    // Magnitude order of two NORMAL values (exp, then significand).
    static int compareMagnitude(WideFloatValue const& a, WideFloatValue const& b) noexcept {
        if (a.exponent_ != b.exponent_) return a.exponent_ < b.exponent_ ? -1 : 1;
        if (a.sigHi_ != b.sigHi_) return a.sigHi_ < b.sigHi_ ? -1 : 1;
        if (a.sigLo_ != b.sigLo_) return a.sigLo_ < b.sigLo_ ? -1 : 1;
        return 0;
    }
    // Magnitude order treating Infinity as the largest (both nonzero, non-NaN).
    static int compareMagnitudeAny(WideFloatValue const& a, WideFloatValue const& b) noexcept {
        bool const ai = a.class_ == Class::Infinity, bi = b.class_ == Class::Infinity;
        if (ai || bi) { if (ai && bi) return 0; return ai ? 1 : -1; }
        return compareMagnitude(a, b);
    }
    static int signum(WideFloatValue const& v) noexcept {
        if (v.class_ == Class::Zero) return 0;
        return v.sign_ ? -1 : 1;
    }

    // ── pack (kind-specific byte layout) ──────────────────────────────────────
    [[nodiscard]] Packed packF80() const noexcept {
        std::uint64_t const s = sign_ ? 1u : 0u;
        switch (class_) {
            case Class::Zero:     return {0, s << 15};
            case Class::Infinity: return {std::uint64_t{1} << 63, (s << 15) | 0x7FFFu};
            case Class::NaN:      return {(std::uint64_t{1} << 63) | (std::uint64_t{1} << 62),
                                          (s << 15) | 0x7FFFu};   // integer + quiet bit
            case Class::Normal: {
                std::uint32_t const exp15 =
                    static_cast<std::uint32_t>(exponent_ + kExpBias) & 0x7FFFu;
                return {sigHi_, (s << 15) | exp15};   // full 64-bit significand in lo
            }
        }
        return {0, 0};
    }
    [[nodiscard]] Packed packF128() const noexcept {
        std::uint64_t const s = sign_ ? 1u : 0u;
        switch (class_) {
            case Class::Zero:     return {0, s << 63};
            case Class::Infinity: return {0, (s << 63) | (std::uint64_t{0x7FFF} << 48)};
            case Class::NaN:      return {0, (s << 63) | (std::uint64_t{0x7FFF} << 48)
                                             | (std::uint64_t{1} << 47)};   // quiet
            case Class::Normal: {
                std::uint32_t const exp15 =
                    static_cast<std::uint32_t>(exponent_ + kExpBias) & 0x7FFFu;
                // Fraction (112 bits) = register bits [126..15]; integer bit dropped.
                std::uint64_t const fracLo64 = (sigLo_ >> 15) | (sigHi_ << 49);   // bits 0..63
                std::uint64_t const fracHi48 = (sigHi_ >> 15) & 0xFFFF'FFFF'FFFFull;  // bits 64..111
                return {fracLo64,
                        (s << 63) | (static_cast<std::uint64_t>(exp15) << 48) | fracHi48};
            }
        }
        return {0, 0};
    }

    // ── unpack (inverse of pack) ──────────────────────────────────────────────
    [[nodiscard]] static WideFloatValue unpackF80(std::uint64_t lo, std::uint64_t hi) noexcept {
        WideFloatValue v; v.kind_ = TypeKind::F80;
        v.sign_ = ((hi >> 15) & 1u) != 0;
        std::uint32_t const exp15 = static_cast<std::uint32_t>(hi & 0x7FFFu);
        if (exp15 == 0x7FFFu) {
            // integer bit set + fraction 0 → inf; fraction != 0 → NaN.
            v.class_ = ((lo & 0x7FFF'FFFF'FFFF'FFFFull) == 0) ? Class::Infinity : Class::NaN;
            return v;
        }
        if (exp15 == 0 || lo == 0) { v.class_ = Class::Zero; return v; }
        v.class_    = Class::Normal;
        v.sigHi_    = lo;   // 64-bit significand (explicit integer bit)
        v.sigLo_    = 0;
        v.exponent_ = static_cast<std::int32_t>(exp15) - kExpBias;
        return v;
    }
    [[nodiscard]] static WideFloatValue unpackF128(std::uint64_t lo, std::uint64_t hi) noexcept {
        WideFloatValue v; v.kind_ = TypeKind::F128;
        v.sign_ = ((hi >> 63) & 1u) != 0;
        std::uint32_t const exp15 = static_cast<std::uint32_t>((hi >> 48) & 0x7FFFu);
        std::uint64_t const fracHi48 = hi & 0xFFFF'FFFF'FFFFull;   // fraction bits 64..111
        std::uint64_t const fracLo64 = lo;                         // fraction bits 0..63
        if (exp15 == 0x7FFFu) {
            v.class_ = (fracHi48 == 0 && fracLo64 == 0) ? Class::Infinity : Class::NaN;
            return v;
        }
        if (exp15 == 0 && fracHi48 == 0 && fracLo64 == 0) { v.class_ = Class::Zero; return v; }
        v.class_ = Class::Normal;
        // register R = (1<<127) | (fraction << 15); sigHi_ = R>>64, sigLo_ = low 64.
        v.sigHi_ = (std::uint64_t{1} << 63) | (fracHi48 << 15) | (fracLo64 >> 49);
        v.sigLo_ = fracLo64 << 15;
        v.exponent_ = static_cast<std::int32_t>(exp15) - kExpBias;
        return v;
    }
};

}  // namespace dss
