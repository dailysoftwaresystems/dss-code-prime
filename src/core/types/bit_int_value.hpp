#pragma once

// ── C23 _BitInt(N) — the host compile-time bit-precise value type (C4b) ──────
// D-CSUBSET-BITINT-CONSTFOLD-LARGE / D-CSUBSET-BITINT-WIDE-LITERAL.
//
// A pure HOST value type carrying an arbitrary-width bit-precise integer for the
// const-fold engines (`cst_const_eval.cpp` / `const_eval.cpp`) + the wb/uwb
// literal materialization (`hir_to_mir.cpp`). ZERO target / language / object-
// format identity — this is compile-time host arithmetic that mirrors the C2/C3
// RUNTIME limb model CONCEPTUALLY (a little-endian `std::vector<uint64_t>`
// magnitude, limb 0 least-significant) but shares NO code with it (the runtime
// model emits MIR limb-loops; this computes on the host).
//
// Every arithmetic/bitwise/shift op is mod-2^N + sign-aware to a GIVEN result
// (width, isSigned), funnelled through the ONE `wrapTo` chokepoint (the host
// analog of C1's by-construction wrap chokepoint + C2's `maskTopLimb`). An op
// first CONVERTS both operands to the result (width, isSigned) — C's "convert to
// the common type, then operate" — so two's-complement add/sub/mul/bitwise are
// bit-identical regardless of signedness; only wrap, right-shift, division, and
// comparison consult the signedness.
//
// Invariant (post-`wrapTo`): `limbs_.size() == ceil(width_/64)`, and the top
// limb's bits ABOVE `width_` are the sign-extension of bit `width_-1` (when
// `isSigned_` and that bit is set) or zero — a "clean" limb, exactly the runtime
// `maskTopLimb` invariant.

#include <cstdint>
#include <optional>
#include <vector>

namespace dss {

// C23 6.2.5: __BITINT_MAXWIDTH__ — the maximum admissible `_BitInt` width (the
// same 8388608 the predefined macro + the `_BitInt(N)` specifier resolver carry;
// the three encode ONE ABI constant). A wb/uwb literal whose magnitude-derived N
// exceeds this is a constraint violation the typing call sites raise fail-loud.
inline constexpr std::uint32_t kBitIntMaxWidth = 8388608u;

class BitIntValue {
public:
    BitIntValue() { limbs_.assign(1, 0); }

    // From raw little-endian limbs + a declared (width, isSigned). The limbs are
    // re-wrapped to the declared width (so a caller may pass more/fewer limbs).
    BitIntValue(std::vector<std::uint64_t> limbs, std::uint32_t width, bool isSigned)
        : limbs_(std::move(limbs)), width_(width == 0 ? 1 : width), isSigned_(isSigned) {
        wrapTo(width_, isSigned_);
    }

    // From a host 64-bit integer, represented at (width, isSigned).
    [[nodiscard]] static BitIntValue fromI64(std::int64_t v, std::uint32_t width, bool isSigned) {
        BitIntValue r;
        r.limbs_.assign(1, static_cast<std::uint64_t>(v));
        // Sign-extend the seed value across a full ceil(width/64) limb span BEFORE
        // masking, so a negative i64 widened to N>64 fills its upper limbs with 1s.
        r.isSigned_ = true;   // interpret the i64 seed as signed for the extension
        r.width_    = 64;
        r.convertTo(width, isSigned);
        return r;
    }
    [[nodiscard]] static BitIntValue fromU64(std::uint64_t v, std::uint32_t width, bool isSigned) {
        BitIntValue r;
        r.limbs_.assign(1, v);
        r.isSigned_ = false;   // unsigned seed → zero-extend
        r.width_    = 64;
        r.convertTo(width, isSigned);
        return r;
    }

    // C23 6.4.4.1: a `wb`/`uwb` LITERAL's type is `[unsigned] _BitInt(N)` with the
    // smallest N holding the (non-negative) decoded magnitude. `magnitude` is the
    // little-endian unsigned magnitude limbs (the `decodeBigInteger` feed). M3:
    // magnitude 0 → N=1 (⌈log2⌉ undefined at 0; C23's minimum width is 1). Signed
    // `wb`: N = bitLength(magnitude) + 1 (the sign bit — a positive value must not
    // read back negative). Unsigned `uwb`: N = max(1, bitLength(magnitude)).
    [[nodiscard]] static BitIntValue
    fromLiteralMagnitude(std::vector<std::uint64_t> const& magnitude, bool isSigned) {
        std::uint32_t const bits = bitLengthOf(magnitude);
        std::uint32_t width = isSigned ? (bits + 1) : (bits == 0 ? 1u : bits);
        if (width == 0) width = 1;   // M3 (redundant with the branches, pinned)
        return BitIntValue(magnitude, width, isSigned);
    }

    [[nodiscard]] std::uint32_t width()    const noexcept { return width_; }
    [[nodiscard]] bool          isSigned() const noexcept { return isSigned_; }
    [[nodiscard]] std::vector<std::uint64_t> const& limbs() const noexcept { return limbs_; }
    [[nodiscard]] std::size_t   limbCount() const noexcept { return limbs_.size(); }

    // Low 64 bits of the value (sign-agnostic bit pattern). The narrow-literal
    // materialization + the "BitInt-outranked-by-standard" fold extract via this.
    [[nodiscard]] std::uint64_t low64() const noexcept {
        return limbs_.empty() ? 0ull : limbs_[0];
    }
    // The value as a host i64 (sign-extended when signed). Valid for width ≤ 64
    // consumers (a narrow container). For width > 64 this returns only the low
    // limb reinterpreted — callers gate on width ≤ 64.
    [[nodiscard]] std::int64_t asI64() const noexcept {
        return static_cast<std::int64_t>(low64());
    }

    [[nodiscard]] bool isZero() const noexcept {
        for (std::uint64_t l : limbs_) if (l != 0) return false;
        return true;
    }
    // Bit (width_-1) — the sign bit position of the value at its declared width.
    [[nodiscard]] bool signBitSet() const noexcept {
        if (width_ == 0) return false;
        std::uint32_t const b = width_ - 1;
        std::size_t const li = b / 64;
        if (li >= limbs_.size()) return false;
        return ((limbs_[li] >> (b % 64)) & 1ull) != 0;
    }
    [[nodiscard]] bool isNegative() const noexcept { return isSigned_ && signBitSet(); }

    // ── The ONE wrap chokepoint ──────────────────────────────────────────────
    // Re-establish the invariant at a (possibly new) (width, isSigned): resize to
    // ceil(width/64) limbs, then mask/sign-extend the top limb. Growing fills with
    // the CURRENT sign (so a signed-negative value keeps its 1-fill); every op ends
    // here. (Distinct from `convertTo`, which re-interprets THEN wraps.)
    void wrapTo(std::uint32_t width, bool isSigned) {
        std::uint64_t const grow = (isSigned_ && isNegative()) ? ~0ull : 0ull;
        std::size_t const n = static_cast<std::size_t>((width + 63u) / 64u);
        std::size_t const old = limbs_.size();
        limbs_.resize(n == 0 ? 1 : n, grow);
        if (old > limbs_.size()) { /* truncation handled by resize */ }
        width_    = width == 0 ? 1 : width;
        isSigned_ = isSigned;
        maskTopLimb(limbs_, width_, isSigned_);
    }

    // C 6.3.1.3 conversion of THIS value to (newWidth, newSigned): re-interpret the
    // current value (sign/zero-extended per the CURRENT signedness), then take the
    // low newWidth bits under the NEW signedness. Value-preserving where it fits.
    void convertTo(std::uint32_t newWidth, bool newSigned) {
        std::size_t const n = static_cast<std::size_t>((newWidth + 63u) / 64u);
        std::vector<std::uint64_t> ext = extendedLimbs(n == 0 ? 1 : n);
        maskTopLimb(ext, newWidth == 0 ? 1 : newWidth, newSigned);
        limbs_    = std::move(ext);
        width_    = newWidth == 0 ? 1 : newWidth;
        isSigned_ = newSigned;
    }
    [[nodiscard]] BitIntValue withType(std::uint32_t newWidth, bool newSigned) const {
        BitIntValue r = *this;
        r.convertTo(newWidth, newSigned);
        return r;
    }

    // ── Binary ops (result at (W, S) — both operands converted first) ─────────
    [[nodiscard]] static BitIntValue add(BitIntValue const& a, BitIntValue const& b,
                                         std::uint32_t W, bool S) {
        BitIntValue ca = a.withType(W, S), cb = b.withType(W, S);
        std::size_t const n = ca.limbs_.size();
        std::vector<std::uint64_t> out(n, 0);
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < n; ++i) {
            std::uint64_t const s1 = ca.limbs_[i] + cb.limbs_[i];
            std::uint64_t const c1 = (s1 < ca.limbs_[i]) ? 1u : 0u;
            std::uint64_t const s2 = s1 + carry;
            std::uint64_t const c2 = (s2 < s1) ? 1u : 0u;
            out[i] = s2; carry = c1 + c2;
        }
        return BitIntValue(std::move(out), W, S);
    }
    [[nodiscard]] static BitIntValue sub(BitIntValue const& a, BitIntValue const& b,
                                         std::uint32_t W, bool S) {
        BitIntValue ca = a.withType(W, S), cb = b.withType(W, S);
        std::size_t const n = ca.limbs_.size();
        std::vector<std::uint64_t> out(n, 0);
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < n; ++i) {
            std::uint64_t const d1 = ca.limbs_[i] - cb.limbs_[i];
            std::uint64_t const b1 = (ca.limbs_[i] < cb.limbs_[i]) ? 1u : 0u;
            std::uint64_t const d2 = d1 - borrow;
            std::uint64_t const b2 = (d1 < borrow) ? 1u : 0u;
            out[i] = d2; borrow = b1 + b2;
        }
        return BitIntValue(std::move(out), W, S);
    }
    [[nodiscard]] static BitIntValue mul(BitIntValue const& a, BitIntValue const& b,
                                         std::uint32_t W, bool S) {
        BitIntValue ca = a.withType(W, S), cb = b.withType(W, S);
        std::size_t const n = ca.limbs_.size();
        std::vector<std::uint64_t> prod(2 * n, 0);   // full product, then truncate
        for (std::size_t i = 0; i < n; ++i) {
            std::uint64_t carry = 0;
            for (std::size_t j = 0; j < n; ++j) {
                std::uint64_t lo, hi;
                mul64(ca.limbs_[i], cb.limbs_[j], lo, hi);
                // prod[i+j] += lo + carry; propagate into hi.
                std::uint64_t s = prod[i + j] + lo;
                std::uint64_t c = (s < prod[i + j]) ? 1u : 0u;
                std::uint64_t s2 = s + carry;
                c += (s2 < s) ? 1u : 0u;
                prod[i + j] = s2;
                carry = hi + c;
            }
            prod[i + n] += carry;   // fits: the running column never overflows here
        }
        prod.resize(n);   // low n limbs (mod 2^(n*64)); wrapTo tightens to W bits
        return BitIntValue(std::move(prod), W, S);
    }
    // C99 6.5.5: `/` and `%`. Unsigned binary long-division on the magnitudes, then
    // the signed fixup q=(sa^sb)?-uq:uq; r=sa?-ur:ur. Division by zero → nullopt
    // (the const-eval caller raises a real diagnostic — a constant-expression
    // div-by-zero is a constraint violation, fail loud, NOT a runtime trap).
    [[nodiscard]] static std::optional<BitIntValue>
    divide(BitIntValue const& a, BitIntValue const& b, std::uint32_t W, bool S) {
        return divRem(a, b, W, S, /*wantRem=*/false);
    }
    [[nodiscard]] static std::optional<BitIntValue>
    remainder(BitIntValue const& a, BitIntValue const& b, std::uint32_t W, bool S) {
        return divRem(a, b, W, S, /*wantRem=*/true);
    }
    [[nodiscard]] static BitIntValue bitAnd(BitIntValue const& a, BitIntValue const& b,
                                            std::uint32_t W, bool S) {
        return bitwise(a, b, W, S, 0);
    }
    [[nodiscard]] static BitIntValue bitOr(BitIntValue const& a, BitIntValue const& b,
                                           std::uint32_t W, bool S) {
        return bitwise(a, b, W, S, 1);
    }
    [[nodiscard]] static BitIntValue bitXor(BitIntValue const& a, BitIntValue const& b,
                                            std::uint32_t W, bool S) {
        return bitwise(a, b, W, S, 2);
    }
    // C 6.5.7: shift — result at the (promoted) LEFT operand's type (W, S). The
    // count is a plain integer. `>>` is arithmetic iff S. A count ≥ W or < 0 is C
    // UB; we saturate the count to [0, W) here (const-fold never traps), matching
    // the "well-defined fold, target defines the runtime" discipline elsewhere.
    [[nodiscard]] static BitIntValue shiftLeft(BitIntValue const& a, std::uint64_t count,
                                               std::uint32_t W, bool S) {
        BitIntValue r = a.withType(W, S);
        if (count >= W) { r.limbs_.assign(r.limbs_.size(), 0); r.wrapTo(W, S); return r; }
        r.shlBits(static_cast<std::uint32_t>(count));
        r.wrapTo(W, S);
        return r;
    }
    [[nodiscard]] static BitIntValue shiftRight(BitIntValue const& a, std::uint64_t count,
                                                std::uint32_t W, bool S) {
        BitIntValue r = a.withType(W, S);
        std::uint32_t const c = (count >= W) ? (W == 0 ? 0 : W - 1) : static_cast<std::uint32_t>(count);
        r.shrBits(c, S);
        r.wrapTo(W, S);
        return r;
    }

    // ── Unary ops (result at (W, S)) ─────────────────────────────────────────
    [[nodiscard]] static BitIntValue neg(BitIntValue const& a, std::uint32_t W, bool S) {
        // 0 - a, at (W, S).
        return sub(BitIntValue::fromU64(0, W, S), a, W, S);
    }
    [[nodiscard]] static BitIntValue bitNot(BitIntValue const& a, std::uint32_t W, bool S) {
        BitIntValue r = a.withType(W, S);
        for (auto& l : r.limbs_) l = ~l;
        r.wrapTo(W, S);
        return r;
    }

    // ── Comparison (three-way, over the common type (W, S)) ──────────────────
    // Returns -1 / 0 / +1. Both operands converted to (W, S) first; when S the
    // comparison is signed, else unsigned magnitude.
    [[nodiscard]] static int compare(BitIntValue const& a, BitIntValue const& b,
                                     std::uint32_t W, bool S) {
        BitIntValue ca = a.withType(W, S), cb = b.withType(W, S);
        if (S) {
            bool const an = ca.isNegative(), bn = cb.isNegative();
            if (an != bn) return an ? -1 : 1;    // negative < non-negative
            // Same sign: an unsigned limb compare of the two's-complement bits gives
            // the correct signed order (e.g. -1=0xFF.. > -2=0xFE.. AND 0xFF>0xFE).
        }
        return ucompare(ca.limbs_, cb.limbs_);
    }

    // ── Serialization ────────────────────────────────────────────────────────
    // ceil(N/64)*8 little-endian bytes (the wide-literal `materializeWideLiteral`
    // limb fill + the `.dsshir`/`.dssir` text round-trip).
    [[nodiscard]] std::vector<std::uint8_t> toLimbBytes() const {
        std::vector<std::uint8_t> out;
        out.reserve(limbs_.size() * 8);
        for (std::uint64_t l : limbs_)
            for (int b = 0; b < 8; ++b)
                out.push_back(static_cast<std::uint8_t>((l >> (8 * b)) & 0xFFu));
        return out;
    }
    [[nodiscard]] static BitIntValue
    fromLimbBytes(std::vector<std::uint8_t> const& bytes, std::uint32_t width, bool isSigned) {
        std::vector<std::uint64_t> limbs((bytes.size() + 7) / 8, 0);
        for (std::size_t i = 0; i < bytes.size(); ++i)
            limbs[i / 8] |= static_cast<std::uint64_t>(bytes[i]) << (8 * (i % 8));
        return BitIntValue(std::move(limbs), width, isSigned);
    }

    friend bool operator==(BitIntValue const&, BitIntValue const&) = default;

private:
    std::vector<std::uint64_t> limbs_;   // little-endian, ceil(width_/64), wrapped
    std::uint32_t width_    = 1;
    bool          isSigned_ = false;

    // Bit length (position of the highest set bit + 1) of an unsigned magnitude.
    [[nodiscard]] static std::uint32_t bitLengthOf(std::vector<std::uint64_t> const& m) {
        for (std::size_t i = m.size(); i-- > 0;) {
            if (m[i] != 0) {
                std::uint32_t top = 0;
                std::uint64_t v = m[i];
                while (v != 0) { ++top; v >>= 1; }
                return static_cast<std::uint32_t>(i * 64) + top;
            }
        }
        return 0;
    }

    // This value's limbs sign/zero-extended (per the CURRENT signedness) to `nOut`
    // limbs; truncates (drops high limbs) when `nOut` is smaller.
    [[nodiscard]] std::vector<std::uint64_t> extendedLimbs(std::size_t nOut) const {
        std::vector<std::uint64_t> out(nOut, 0);
        std::uint64_t const fill = (isSigned_ && isNegative()) ? ~0ull : 0ull;
        for (std::size_t i = 0; i < nOut; ++i)
            out[i] = (i < limbs_.size()) ? limbs_[i] : fill;
        return out;
    }

    // Mask / sign-extend the TOP limb of `limbs` (already ceil(width/64) entries)
    // to `width` bits under `isSigned`.
    static void maskTopLimb(std::vector<std::uint64_t>& limbs, std::uint32_t width, bool isSigned) {
        if (limbs.empty()) return;
        std::uint32_t const hb = ((width - 1u) % 64u) + 1u;   // significant bits, top limb
        if (hb == 64) return;                                 // full limb — nothing to clear
        std::uint64_t const mask = (std::uint64_t{1} << hb) - 1u;
        std::uint64_t& top = limbs.back();
        if (isSigned && ((top >> (hb - 1)) & 1ull))
            top |= ~mask;     // sign-extend bit hb-1 across [hb, 64)
        else
            top &= mask;      // zero the bits above width
    }

    // Portable 64×64 → 128 (lo, hi) via 32-bit halves (no __uint128_t / _umul128 —
    // MSVC + clang + gcc all compile this identically).
    static void mul64(std::uint64_t a, std::uint64_t b,
                      std::uint64_t& lo, std::uint64_t& hi) {
        std::uint64_t const aL = a & 0xFFFFFFFFull, aH = a >> 32;
        std::uint64_t const bL = b & 0xFFFFFFFFull, bH = b >> 32;
        std::uint64_t const ll = aL * bL;
        std::uint64_t const lh = aL * bH;
        std::uint64_t const hl = aH * bL;
        std::uint64_t const hh = aH * bH;
        std::uint64_t const cross = (ll >> 32) + (lh & 0xFFFFFFFFull) + (hl & 0xFFFFFFFFull);
        lo = (ll & 0xFFFFFFFFull) | (cross << 32);
        hi = hh + (lh >> 32) + (hl >> 32) + (cross >> 32);
    }

    // Unsigned three-way limb compare (little-endian): high limbs first.
    [[nodiscard]] static int ucompare(std::vector<std::uint64_t> const& a,
                                      std::vector<std::uint64_t> const& b) {
        std::size_t const n = a.size() > b.size() ? a.size() : b.size();
        for (std::size_t i = n; i-- > 0;) {
            std::uint64_t const av = i < a.size() ? a[i] : 0ull;
            std::uint64_t const bv = i < b.size() ? b[i] : 0ull;
            if (av != bv) return av < bv ? -1 : 1;
        }
        return 0;
    }

    [[nodiscard]] static BitIntValue bitwise(BitIntValue const& a, BitIntValue const& b,
                                             std::uint32_t W, bool S, int which) {
        BitIntValue ca = a.withType(W, S), cb = b.withType(W, S);
        std::size_t const n = ca.limbs_.size();
        std::vector<std::uint64_t> out(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            switch (which) {
                case 0: out[i] = ca.limbs_[i] & cb.limbs_[i]; break;
                case 1: out[i] = ca.limbs_[i] | cb.limbs_[i]; break;
                default: out[i] = ca.limbs_[i] ^ cb.limbs_[i]; break;
            }
        }
        return BitIntValue(std::move(out), W, S);
    }

    [[nodiscard]] static std::optional<BitIntValue>
    divRem(BitIntValue const& a, BitIntValue const& b, std::uint32_t W, bool S, bool wantRem) {
        BitIntValue ca = a.withType(W, S), cb = b.withType(W, S);
        if (cb.isZero()) return std::nullopt;   // fail-loud sentinel
        bool const sa = S && ca.isNegative();
        bool const sb = S && cb.isNegative();
        std::vector<std::uint64_t> ua = sa ? negatedMagnitude(ca) : ca.limbs_;
        std::vector<std::uint64_t> ub = sb ? negatedMagnitude(cb) : cb.limbs_;
        std::vector<std::uint64_t> q, r;
        udivmod(ua, ub, q, r);
        std::vector<std::uint64_t>& mag = wantRem ? r : q;
        bool const negResult = wantRem ? sa : (sa != sb);
        if (negResult) {
            BitIntValue const mg(std::move(mag), W, /*isSigned=*/false);
            return BitIntValue::neg(mg, W, S);
        }
        return BitIntValue(std::move(mag), W, S);
    }

    // Two's-complement magnitude of a negative value: ~v + 1 over its limbs.
    [[nodiscard]] static std::vector<std::uint64_t> negatedMagnitude(BitIntValue const& v) {
        std::vector<std::uint64_t> out(v.limbs_.size(), 0);
        std::uint64_t carry = 1;
        for (std::size_t i = 0; i < out.size(); ++i) {
            std::uint64_t const t = ~v.limbs_[i] + carry;
            carry = (t < carry) ? 1u : 0u;
            out[i] = t;
        }
        return out;
    }

    // Unsigned binary long division: q = a / b, r = a % b (magnitudes; b != 0).
    static void udivmod(std::vector<std::uint64_t> const& a,
                        std::vector<std::uint64_t> const& b,
                        std::vector<std::uint64_t>& q, std::vector<std::uint64_t>& r) {
        std::size_t const n = a.size() > b.size() ? a.size() : b.size();
        q.assign(n, 0);
        r.assign(n, 0);
        for (std::size_t bit = n * 64; bit-- > 0;) {
            shl1(r);
            if (getBit(a, bit)) r[0] |= 1ull;
            if (ucompare(r, b) >= 0) { usubInPlace(r, b); setBit(q, bit); }
        }
    }
    static void shl1(std::vector<std::uint64_t>& v) {
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < v.size(); ++i) {
            std::uint64_t const nc = v[i] >> 63;
            v[i] = (v[i] << 1) | carry;
            carry = nc;
        }
    }
    static void usubInPlace(std::vector<std::uint64_t>& a, std::vector<std::uint64_t> const& b) {
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            std::uint64_t const bv = i < b.size() ? b[i] : 0ull;
            std::uint64_t const d1 = a[i] - bv;
            std::uint64_t const b1 = (a[i] < bv) ? 1u : 0u;
            std::uint64_t const d2 = d1 - borrow;
            std::uint64_t const b2 = (d1 < borrow) ? 1u : 0u;
            a[i] = d2; borrow = b1 + b2;
        }
    }
    [[nodiscard]] static bool getBit(std::vector<std::uint64_t> const& v, std::size_t bit) {
        std::size_t const li = bit / 64;
        if (li >= v.size()) return false;
        return ((v[li] >> (bit % 64)) & 1ull) != 0;
    }
    static void setBit(std::vector<std::uint64_t>& v, std::size_t bit) {
        std::size_t const li = bit / 64;
        if (li < v.size()) v[li] |= (std::uint64_t{1} << (bit % 64));
    }

    // In-place left shift by `c` (< width_) bits.
    void shlBits(std::uint32_t c) {
        if (c == 0) return;
        std::uint32_t const limbShift = c / 64;
        std::uint32_t const bitShift  = c % 64;
        std::size_t const n = limbs_.size();
        std::vector<std::uint64_t> out(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t const src = i;
            std::size_t const dst = i + limbShift;
            if (dst < n) {
                out[dst] |= (bitShift == 0) ? limbs_[src] : (limbs_[src] << bitShift);
                if (bitShift != 0 && dst + 1 < n)
                    out[dst + 1] |= (limbs_[src] >> (64 - bitShift));
            }
        }
        limbs_ = std::move(out);
    }
    // In-place right shift by `c` (< width_) bits; arithmetic iff `arith`.
    void shrBits(std::uint32_t c, bool arith) {
        if (c == 0) return;
        bool const neg = arith && isNegative();
        std::uint32_t const limbShift = c / 64;
        std::uint32_t const bitShift  = c % 64;
        std::size_t const n = limbs_.size();
        std::uint64_t const fill = neg ? ~0ull : 0ull;
        std::vector<std::uint64_t> out(n, fill);
        for (std::size_t i = 0; i < n; ++i) {
            if (i < limbShift) continue;
            std::size_t const src = i;
            std::size_t const dst = i - limbShift;
            std::uint64_t v = limbs_[src];
            if (bitShift != 0) {
                v >>= bitShift;
                std::uint64_t const hi = (src + 1 < n) ? limbs_[src + 1] : fill;
                v |= hi << (64 - bitShift);
            }
            out[dst] = v;
        }
        // The top `c` bits of the result come from the sign fill (arith) or 0.
        limbs_ = std::move(out);
    }
};

}  // namespace dss
