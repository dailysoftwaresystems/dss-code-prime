#pragma once

// Shared arithmetic + literal-bridge helpers used by BOTH const-eval
// walkers (HIR-side `const_eval.cpp` and CST-side `cst_const_eval.cpp`).
// These functions operate purely on `HirLiteralValue` / `TypeKind` /
// `HirOpKind` / `EvalOptions` — no Hir, no Tree, no GrammarSchema
// dependency. Extracting them lets the two walkers share one source of
// truth for IEEE 754 narrowing, cast overflow policy, C99 UAC
// common-typing, and div-by-zero / shift-out-of-range refusal — diverging
// here would silently produce two different "compile-time arithmetic"
// answers for the same expression depending on which walker fired.
//
// Header-only by design: every helper is small and hot. Lives in the
// `dss::detail` namespace because nothing outside this library should
// reach in directly — callers go through `evaluateConstant` (HIR) or
// `evaluateConstantCst` (CST), both of which return a `ConstEvalResult`
// with policy applied.

#include "core/types/bit_int_value.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "hir/const_eval.hpp"
#include "hir/hir_literal_pool.hpp"
#include "hir/hir_op.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <variant>

namespace dss::detail {

[[nodiscard]] inline ConstEvalResult ceFail(ConstEvalFailure why, HirNodeId blamed) {
    return ConstEvalResult{.value{}, .failure = why, .blamedNode = blamed};
}

[[nodiscard]] inline ConstEvalResult ceOk(HirLiteralValue v) {
    return ConstEvalResult{.value{std::move(v)}, .failure = ConstEvalFailure::None, .blamedNode{}};
}

// Build a Bool-cored literal carrying `n != 0` per C99 truthiness. The
// engine's normalization rule (documented in `hir_literal_pool.hpp`) is
// that `core == Bool` implies the int64 arm holding 0 or 1; this helper
// is the single source of truth across Cast-to-Bool, LogicalAnd/Or
// short-circuit, and combine paths.
[[nodiscard]] inline HirLiteralValue makeBoolLiteral(std::int64_t n) {
    HirLiteralValue v;
    v.core  = TypeKind::Bool;
    v.value = std::int64_t{(n != 0) ? 1 : 0};
    return v;
}

// Pull an integer-typed `HirLiteralValue` into a common `int64_t`
// arithmetic representation. Bridges the three numeric variant arms
// (`int64_t` / `uint64_t` / `bool`). Returns nullopt for non-integer
// variants, or when an unsigned value exceeds int64's positive range
// (the caller refuses with `Overflow`).
[[nodiscard]] inline std::optional<std::int64_t>
asInt64(HirLiteralValue const& v) noexcept {
    if (auto p = std::get_if<std::int64_t>(&v.value)) return *p;
    if (auto p = std::get_if<std::uint64_t>(&v.value)) {
        if (*p > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(*p);
    }
    if (auto p = std::get_if<bool>(&v.value)) return *p ? std::int64_t{1} : std::int64_t{0};
    // C4b (D-CSUBSET-BITINT-CONSTFOLD-LARGE): a NARROW (N≤64) `_BitInt` const value
    // bridges to int64 (a `_BitInt` is an integer type, admissible in an ICE — e.g.
    // `int a[(_BitInt(8))30]`). A WIDE (N>64) value cannot fit → nullopt (the caller
    // surfaces "not a constant integer"); a wide compare/asBool routes via isZero.
    if (auto p = std::get_if<BitIntValue>(&v.value)) {
        if (p->width() <= 64) return p->asI64();
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] inline bool isFloatValue(HirLiteralValue const& v) noexcept {
    return std::holds_alternative<double>(v.value);
}

// Pull a numeric `HirLiteralValue` into `double` for IEEE 754
// arithmetic. Large uint64/int64 values may lose precision — acceptable
// per IEEE 754 (the same loss happens at runtime). Returns nullopt for
// non-numeric arms.
[[nodiscard]] inline std::optional<double>
asDouble(HirLiteralValue const& v) noexcept {
    if (auto p = std::get_if<double>(&v.value)) return *p;
    if (auto p = std::get_if<std::int64_t>(&v.value)) return static_cast<double>(*p);
    if (auto p = std::get_if<std::uint64_t>(&v.value)) return static_cast<double>(*p);
    if (auto p = std::get_if<bool>(&v.value)) return *p ? 1.0 : 0.0;
    return std::nullopt;
}

// C99 truthiness. `0` (int) and `±0.0` (float) are false; everything
// else (including NaN and ±inf) is true. Returns nullopt for non-numeric
// arms or for float operands when `allowFloat` is off.
[[nodiscard]] inline std::optional<bool>
asBool(HirLiteralValue const& v, bool allowFloat) noexcept {
    if (isFloatValue(v)) {
        if (!allowFloat) return std::nullopt;
        return *asDouble(v) != 0.0;
    }
    // C4b: a `_BitInt` truthiness folds via `isZero` for EVERY N — never through
    // `asInt64`, which nullopt-fails for a WIDE value (a wide `_BitInt` condition in
    // `&&`/`||`/`?:` would then spuriously fail the whole fold).
    if (auto p = std::get_if<BitIntValue>(&v.value)) return !p->isZero();
    auto iv = asInt64(v);
    if (!iv.has_value()) return std::nullopt;
    return *iv != 0;
}

// ── 2's-complement WRAPPING i64 arithmetic (D-CE-HOST-SIGNED-OVERFLOW-UB) ──
// Neg/Add/Sub/Mul on int64 evaluate over uint64 (defined for EVERY input)
// and value-cast back (modular, defined since C++20 — the same pattern
// `wrapToIntTarget` below already relies on). The direct signed forms are
// HOST UB at the overflow points (`-INT64_MIN`, `INT64_MAX+1`, …): the
// COMPILER itself would UB folding a source program's overflow — the
// linux-clang UBSan CI leg trapped exactly this (stdint_limit_macros ×
// `applyUnaryInt` Neg of INT64_MIN, 2026-07-04). The WRAPPED value is the
// correct fold: it is what the runtime op produces on every shipped target
// (x86_64 + arm64 wrap) and what gcc/clang's preprocessor folds for `#if`
// arithmetic — folding it is behavior-preserving. CONTRAST Div/Rem's
// INT64_MIN/-1 below, which stay REFUSED (Overflow): there the runtime
// outcome is target-divergent (x86 idiv #DE TRAPS), so folding any value
// would hide the trap — refusal keeps the op live for the target to define.
[[nodiscard]] inline std::int64_t wrapNegI64(std::int64_t v) noexcept {
    return static_cast<std::int64_t>(0u - static_cast<std::uint64_t>(v));
}
[[nodiscard]] inline std::int64_t wrapAddI64(std::int64_t a, std::int64_t b) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a)
                                   + static_cast<std::uint64_t>(b));
}
[[nodiscard]] inline std::int64_t wrapSubI64(std::int64_t a, std::int64_t b) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a)
                                   - static_cast<std::uint64_t>(b));
}
[[nodiscard]] inline std::int64_t wrapMulI64(std::int64_t a, std::int64_t b) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a)
                                   * static_cast<std::uint64_t>(b));
}

// Fold a UnaryOp(Neg / BitNot / Not) on an integer operand. `Not`
// (logical negation) re-tags the result core to Bool per C99 §6.5.3.3p5.
// Unary `+` is identity at the value level; no v1 frontend emits it
// as a UnaryOp.
[[nodiscard]] inline std::optional<HirLiteralValue>
applyUnaryInt(HirOpKind op, HirLiteralValue const& inner) {
    auto iv64 = asInt64(inner);
    if (!iv64.has_value()) return std::nullopt;
    std::int64_t const iv = *iv64;
    HirLiteralValue folded = inner;
    switch (op) {
        case HirOpKind::Neg:    folded.value = wrapNegI64(iv); return folded;
        case HirOpKind::BitNot: folded.value = ~iv;    return folded;
        case HirOpKind::Not:    folded.value = std::int64_t{iv == 0 ? 1 : 0}; folded.core = TypeKind::Bool; return folded;
        default: return std::nullopt;
    }
}

// Per-float-kind host-backing info: `bits` is the format's width; `hostBacked`
// says whether the host `double` can carry the EXACT value (F16/F32 narrow
// losslessly through `narrowToFloatWidth`; F64 is identity). F80/F128 are NOT
// host-backed — no soft-float engine exists for them, so const-eval refuses
// (fold + conversion) rather than bake a binary64-rounded value. Defined ABOVE
// the applyUnary/BinaryFloat folds, which gate on it (FC17.9(e),
// D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION).
struct FloatKindInfo {
    int  bits;
    bool hostBacked;
};
[[nodiscard]] inline std::optional<FloatKindInfo> floatKindInfo(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::F16:  return FloatKindInfo{16,  true};
        case TypeKind::F32:  return FloatKindInfo{32,  true};
        case TypeKind::F64:  return FloatKindInfo{64,  true};
        // F80 (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION): NOT host-backed —
        // the host `double` cannot represent an 80-bit-mantissa value, so
        // folding at binary64 would bake a silently-rounded constant.
        case TypeKind::F80:  return FloatKindInfo{80,  false};
        case TypeKind::F128: return FloatKindInfo{128, false};
        default: return std::nullopt;
    }
}

// FC17.9(e) (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION): does the operand's
// core refuse host-double folding? A non-host-backed float kind (F80/F128)
// folded at binary64 precision would produce a silently-ROUNDED constant that
// then RUNS (never reaching the LIR encoded-width wall) — a precision
// mis-bind for any value needing more than a 53-bit mantissa. Refusing keeps
// the runtime op alive so the wall fires loud. On f64-axis formats `long
// double` IS F64 (host-backed) and folds normally.
[[nodiscard]] inline bool refusesHostFloatFold(TypeKind k) noexcept {
    auto const fi = floatKindInfo(k);
    return fi.has_value() && !fi->hostBacked;
}

// Fold a UnaryOp(Neg) on a float. BitNot/Not on float is C99-undefined
// (bitwise / logical-not bitwise interpretations only apply to integers);
// surfaced via `outFailure = UnsupportedTypeKind` so the caller can
// distinguish "op not modelled" from "op wrong on this type".
[[nodiscard]] inline std::optional<HirLiteralValue>
applyUnaryFloat(HirOpKind op, HirLiteralValue const& inner,
                ConstEvalFailure& outFailure) {
    if (refusesHostFloatFold(inner.core)) {
        outFailure = ConstEvalFailure::UnsupportedTypeKind;
        return std::nullopt;
    }
    auto dv = asDouble(inner);
    if (!dv.has_value()) return std::nullopt;
    HirLiteralValue folded = inner;
    switch (op) {
        case HirOpKind::Neg: folded.value = -(*dv); return folded;
        default:
            outFailure = ConstEvalFailure::UnsupportedTypeKind;
            return std::nullopt;
    }
}

// Fold a BinaryOp over two integer operands per the EvalOptions policy.
[[nodiscard]] inline std::optional<HirLiteralValue>
applyBinaryInt(HirOpKind op, HirLiteralValue const& a, HirLiteralValue const& b,
               EvalOptions const& opts, ConstEvalFailure& outFailure) {
    auto av64 = asInt64(a);
    auto bv64 = asInt64(b);
    if (!av64.has_value() || !bv64.has_value()) return std::nullopt;
    std::int64_t const av = *av64;
    std::int64_t const bv = *bv64;
    HirLiteralValue folded = a;
    switch (op) {
        // Wrapping forms (D-CE-HOST-SIGNED-OVERFLOW-UB, see the helpers
        // above): the direct `av + bv` was host UB at INT64 overflow.
        case HirOpKind::Add:    folded.value = wrapAddI64(av, bv); return folded;
        case HirOpKind::Sub:    folded.value = wrapSubI64(av, bv); return folded;
        case HirOpKind::Mul:    folded.value = wrapMulI64(av, bv); return folded;
        case HirOpKind::Div:
            if (bv == 0) {
                outFailure = opts.refuseOnDivByZero
                    ? ConstEvalFailure::DivisionByZero
                    : ConstEvalFailure::NotAConstantExpression;
                return std::nullopt;
            }
            // INT64_MIN / -1: the quotient (2^63) is unrepresentable —
            // C UB (6.5.5p6) AND host UB (the `av / bv` below would
            // overflow inside the COMPILER; the linux-clang UBSan CI
            // leg would trip). Refuse unconditionally: the op stays
            // live and the TARGET defines the outcome (x86 idiv #DE
            // trap; arm64 sdiv wraps) — folding would hide it.
            if (av == std::numeric_limits<std::int64_t>::min() && bv == -1) {
                outFailure = ConstEvalFailure::Overflow;
                return std::nullopt;
            }
            folded.value = av / bv; return folded;
        case HirOpKind::Rem:
            if (bv == 0) {
                outFailure = opts.refuseOnDivByZero
                    ? ConstEvalFailure::DivisionByZero
                    : ConstEvalFailure::NotAConstantExpression;
                return std::nullopt;
            }
            // INT64_MIN % -1: mathematically 0, but C UB (6.5.5p6 —
            // `a/b` must be representable for `a%b` to be defined)
            // and host UB (x86's own idiv traps computing it; the
            // C++ `%` below is UB the same way `/` is). Mirror the
            // Div guard — refuse, keep the runtime op.
            if (av == std::numeric_limits<std::int64_t>::min() && bv == -1) {
                outFailure = ConstEvalFailure::Overflow;
                return std::nullopt;
            }
            folded.value = av % bv; return folded;
        case HirOpKind::BitAnd: folded.value = av & bv;  return folded;
        case HirOpKind::BitOr:  folded.value = av | bv;  return folded;
        case HirOpKind::BitXor: folded.value = av ^ bv;  return folded;
        case HirOpKind::Shl: {
            if (bv < 0 || bv >= 64) {
                outFailure = opts.refuseOnShiftOutOfRange
                    ? ConstEvalFailure::ShiftCountOutOfRange
                    : ConstEvalFailure::NotAConstantExpression;
                return std::nullopt;
            }
            folded.value = static_cast<std::int64_t>(
                static_cast<std::uint64_t>(av) << bv);
            return folded;
        }
        case HirOpKind::Shr: {
            if (bv < 0 || bv >= 64) {
                outFailure = opts.refuseOnShiftOutOfRange
                    ? ConstEvalFailure::ShiftCountOutOfRange
                    : ConstEvalFailure::NotAConstantExpression;
                return std::nullopt;
            }
            folded.value = av >> bv;
            return folded;
        }
        case HirOpKind::Eq: folded.value = std::int64_t{av == bv}; return folded;
        case HirOpKind::Ne: folded.value = std::int64_t{av != bv}; return folded;
        case HirOpKind::Lt: folded.value = std::int64_t{av <  bv}; return folded;
        case HirOpKind::Le: folded.value = std::int64_t{av <= bv}; return folded;
        case HirOpKind::Gt: folded.value = std::int64_t{av >  bv}; return folded;
        case HirOpKind::Ge: folded.value = std::int64_t{av >= bv}; return folded;
        default: return std::nullopt;
    }
}

// CE5: fold a BinaryOp when at least one operand is float (after
// promotion). Result core is F64 for arithmetic (caller may re-tag via
// commonType); Bool for comparisons. NaN propagation / ±inf / IEEE 754
// rounding all delegate to the host platform's `<cmath>`.
[[nodiscard]] inline std::optional<HirLiteralValue>
applyBinaryFloat(HirOpKind op, HirLiteralValue const& a, HirLiteralValue const& b,
                 ConstEvalFailure& outFailure) {
    // FC17.9(e): refuse when EITHER operand's core is a non-host-backed float
    // (F80/F128) — see refusesHostFloatFold. Covers arithmetic AND comparisons
    // for both const-eval walkers by construction (this is their one chokepoint).
    if (refusesHostFloatFold(a.core) || refusesHostFloatFold(b.core)) {
        outFailure = ConstEvalFailure::UnsupportedTypeKind;
        return std::nullopt;
    }
    auto adv = asDouble(a);
    auto bdv = asDouble(b);
    if (!adv.has_value() || !bdv.has_value()) return std::nullopt;
    double const av = *adv;
    double const bv = *bdv;
    HirLiteralValue folded;
    folded.core = TypeKind::F64;
    switch (op) {
        case HirOpKind::Add: folded.value = av + bv; return folded;
        case HirOpKind::Sub: folded.value = av - bv; return folded;
        case HirOpKind::Mul: folded.value = av * bv; return folded;
        case HirOpKind::Div: folded.value = av / bv; return folded;
        case HirOpKind::Eq: folded.value = std::int64_t{av == bv}; folded.core = TypeKind::Bool; return folded;
        case HirOpKind::Ne: folded.value = std::int64_t{av != bv}; folded.core = TypeKind::Bool; return folded;
        case HirOpKind::Lt: folded.value = std::int64_t{av <  bv}; folded.core = TypeKind::Bool; return folded;
        case HirOpKind::Le: folded.value = std::int64_t{av <= bv}; folded.core = TypeKind::Bool; return folded;
        case HirOpKind::Gt: folded.value = std::int64_t{av >  bv}; folded.core = TypeKind::Bool; return folded;
        case HirOpKind::Ge: folded.value = std::int64_t{av >= bv}; folded.core = TypeKind::Bool; return folded;
        default:
            outFailure = ConstEvalFailure::UnsupportedTypeKind;
            return std::nullopt;
    }
}

[[nodiscard]] inline bool isFloatKind(TypeKind k) noexcept {
    return k == TypeKind::F16 || k == TypeKind::F32
        || k == TypeKind::F64 || k == TypeKind::F80 || k == TypeKind::F128;
}

// Soft-float narrow `double → IEEE 754 binary16 → double`. Produces the
// closest representable half-precision value of `dv`, then widens back
// to `double` losslessly. NaN / ±inf preserved; round-to-nearest-even.
[[nodiscard]] inline double narrowToHalf(double dv) noexcept {
    float const fv = static_cast<float>(dv);
    std::uint32_t bits;
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::memcpy(&bits, &fv, sizeof(bits));
    std::uint32_t const sign     = (bits >> 31) & 0x1u;
    std::uint32_t const exp32    = (bits >> 23) & 0xFFu;
    std::uint32_t const mant32   =  bits        & 0x7FFFFFu;
    std::uint16_t half;
    if (exp32 == 0xFFu) {
        half = static_cast<std::uint16_t>((sign << 15) | (0x1Fu << 10) |
            (mant32 != 0 ? (mant32 >> 13) | 0x200u : 0u));
    } else if (exp32 == 0) {
        half = static_cast<std::uint16_t>(sign << 15);
    } else {
        int const e = static_cast<int>(exp32) - 127 + 15;
        if (e >= 0x1F) {
            half = static_cast<std::uint16_t>((sign << 15) | (0x1Fu << 10));
        } else if (e <= 0) {
            std::uint32_t const mant = mant32 | 0x800000u;
            int const shift = 14 - e;
            if (shift >= 25) {
                half = static_cast<std::uint16_t>(sign << 15);
            } else {
                std::uint32_t const rounded = mant >> shift;
                std::uint32_t const rem     = mant & ((1u << shift) - 1u);
                std::uint32_t const half_lsb = 1u << (shift - 1);
                std::uint32_t out = rounded;
                if (rem > half_lsb || (rem == half_lsb && (rounded & 1u))) {
                    out += 1;
                }
                half = static_cast<std::uint16_t>((sign << 15) | (out & 0x3FFu));
            }
        } else {
            std::uint32_t const mant = mant32;
            std::uint32_t const rounded = mant >> 13;
            std::uint32_t const rem     = mant & 0x1FFFu;
            std::uint32_t const half_lsb = 0x1000u;
            std::uint32_t out = rounded;
            if (rem > half_lsb || (rem == half_lsb && (rounded & 1u))) {
                out += 1;
                if (out == 0x400u) {
                    out = 0;
                    if (e + 1 >= 0x1F) {
                        half = static_cast<std::uint16_t>((sign << 15) | (0x1Fu << 10));
                        goto done;
                    }
                    half = static_cast<std::uint16_t>(
                        (sign << 15) | (static_cast<std::uint32_t>(e + 1) << 10));
                    goto done;
                }
            }
            half = static_cast<std::uint16_t>(
                (sign << 15) | (static_cast<std::uint32_t>(e) << 10) | (out & 0x3FFu));
        }
    }
done:
    std::uint32_t const wsign = (static_cast<std::uint32_t>(half) >> 15) & 0x1u;
    std::uint32_t const wexp  = (static_cast<std::uint32_t>(half) >> 10) & 0x1Fu;
    std::uint32_t const wmant =  static_cast<std::uint32_t>(half)        & 0x3FFu;
    std::uint32_t wbits;
    if (wexp == 0x1Fu) {
        wbits = (wsign << 31) | (0xFFu << 23) | (wmant << 13);
    } else if (wexp == 0) {
        if (wmant == 0) {
            wbits = wsign << 31;
        } else {
            std::uint32_t m = wmant;
            int e = -1;
            while ((m & 0x400u) == 0) { m <<= 1; --e; }
            m &= 0x3FFu;
            wbits = (wsign << 31) |
                    (static_cast<std::uint32_t>(127 - 14 + e + 1) << 23) |
                    (m << 13);
        }
    } else {
        wbits = (wsign << 31) |
                (static_cast<std::uint32_t>(static_cast<int>(wexp) - 15 + 127) << 23) |
                (wmant << 13);
    }
    float fout;
    std::memcpy(&fout, &wbits, sizeof(fout));
    return static_cast<double>(fout);
}

// (FloatKindInfo + floatKindInfo moved ABOVE the applyUnary/BinaryFloat folds
// — FC17.9(e): the folds gate on hostBacked.)

[[nodiscard]] inline double narrowToFloatWidth(double dv, int bits) noexcept {
    switch (bits) {
        case 16: return narrowToHalf(dv);
        case 32: return static_cast<double>(static_cast<float>(dv));
        default: return dv;
    }
}

[[nodiscard]] inline bool intToFloatIsLossless(std::int64_t iv, int targetBits) noexcept {
    double const widened = static_cast<double>(iv);
    double const narrowed = narrowToFloatWidth(widened, targetBits);
    if (!std::isfinite(narrowed)) return false;
    if (narrowed < static_cast<double>(std::numeric_limits<std::int64_t>::min())
     || narrowed >= 9223372036854775808.0)
        return false;
    return static_cast<std::int64_t>(narrowed) == iv;
}

struct IntKindInfo {
    int  bits;
    bool isSigned;
};
[[nodiscard]] inline std::optional<IntKindInfo> intKindInfo(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::Bool: return IntKindInfo{1,   false};
        case TypeKind::I8:   return IntKindInfo{8,   true};
        case TypeKind::U8:   return IntKindInfo{8,   false};
        case TypeKind::Byte: return IntKindInfo{8,   false};
        case TypeKind::I16:  return IntKindInfo{16,  true};
        case TypeKind::U16:  return IntKindInfo{16,  false};
        case TypeKind::I32:  return IntKindInfo{32,  true};
        case TypeKind::U32:  return IntKindInfo{32,  false};
        case TypeKind::Char: return IntKindInfo{32,  false};
        case TypeKind::I64:  return IntKindInfo{64,  true};
        case TypeKind::U64:  return IntKindInfo{64,  false};
        case TypeKind::I128: return IntKindInfo{128, true};
        case TypeKind::U128: return IntKindInfo{128, false};
        default: return std::nullopt;
    }
}

[[nodiscard]] inline bool valueFitsInIntTarget(std::int64_t v, IntKindInfo target) noexcept {
    if (target.bits >= 64) {
        if (!target.isSigned) return v >= 0;
        return true;
    }
    if (target.isSigned) {
        std::int64_t const lo = -(std::int64_t{1} << (target.bits - 1));
        std::int64_t const hi =  (std::int64_t{1} << (target.bits - 1)) - 1;
        return v >= lo && v <= hi;
    }
    std::int64_t const hi = (target.bits < 63)
        ? (std::int64_t{1} << target.bits) - 1
        : std::numeric_limits<std::int64_t>::max();
    return v >= 0 && v <= hi;
}

[[nodiscard]] inline std::int64_t wrapToIntTarget(std::int64_t v, IntKindInfo target) noexcept {
    if (target.bits >= 64) return v;
    std::uint64_t const mask = (std::uint64_t{1} << target.bits) - 1;
    std::uint64_t       masked = static_cast<std::uint64_t>(v) & mask;
    if (target.isSigned) {
        std::uint64_t const signBit = std::uint64_t{1} << (target.bits - 1);
        if ((masked & signBit) != 0) masked |= ~mask;
    }
    return static_cast<std::int64_t>(masked);
}

// ── C23 _BitInt(N) wrap-aware const-fold (C4b, D-CSUBSET-BITINT-CONSTFOLD-LARGE) ──
//
// The shared bignum fold both const-eval walkers (`const_eval.cpp` /
// `cst_const_eval.cpp`) route through when at least ONE operand is a `_BitInt`
// value (the `BitIntValue` pool arm). It mirrors the TYPED side's usual-
// arithmetic-conversion (type_rules.hpp usualArithmeticCommonType, lines
// 626-633/880-895) to compute the TRUE C23 result (width, signed) and wraps the
// bignum fold AT THAT width — NOT naively at an operand width (which miscompiles
// `15wb + 1`, whose int-outranked-BitInt result is a plain `int`, no wrap → 16).

// A `_BitInt`-UAC operand descriptor. A standard integer arm's (width, signed)
// come from its `core` via `intKindInfo` (the CST leaf tags integer literals I32;
// sizeof → U64; a cast → its target). nullopt for a non-integer arm.
struct BitIntOperandType { std::uint32_t width; bool isSigned; bool isBitPrecise; };

[[nodiscard]] inline std::optional<BitIntOperandType>
bitIntOperandType(HirLiteralValue const& v) noexcept {
    if (auto const* bv = std::get_if<BitIntValue>(&v.value)) {
        return BitIntOperandType{bv->width(), bv->isSigned(), true};
    }
    if (std::holds_alternative<std::int64_t>(v.value)
        || std::holds_alternative<std::uint64_t>(v.value)
        || std::holds_alternative<bool>(v.value)) {
        if (auto ik = intKindInfo(v.core); ik.has_value()) {
            return BitIntOperandType{static_cast<std::uint32_t>(ik->bits),
                                     ik->isSigned, false};
        }
    }
    return std::nullopt;
}

// The operand as a `BitIntValue` at its OWN (width, signed) — a bignum arm passes
// through; a standard arm converts at its `core` width. nullopt for a non-integer.
[[nodiscard]] inline std::optional<BitIntValue>
asBitIntValue(HirLiteralValue const& v) noexcept {
    if (auto const* bv = std::get_if<BitIntValue>(&v.value)) return *bv;
    auto ot = bitIntOperandType(v);
    if (!ot.has_value()) return std::nullopt;
    if (auto const* i = std::get_if<std::int64_t>(&v.value))
        return BitIntValue::fromI64(*i, ot->width, ot->isSigned);
    if (auto const* u = std::get_if<std::uint64_t>(&v.value))
        return BitIntValue::fromU64(*u, ot->width, ot->isSigned);
    if (auto const* b = std::get_if<bool>(&v.value))
        return BitIntValue::fromU64(*b ? 1u : 0u, ot->width, ot->isSigned);
    return std::nullopt;
}

// C 6.3.1.8 integer promotion of a STANDARD operand (a `_BitInt` does NOT promote):
// a standard width below int's rank (32) promotes to `int` (32, signed).
[[nodiscard]] inline BitIntOperandType promoteBitIntOperand(BitIntOperandType t) noexcept {
    if (!t.isBitPrecise && t.width < 32) return {32, true, false};
    return t;
}

// The C23 usual-arithmetic-conversion RESULT type for a BitInt-involved pair —
// MIRRORS type_rules.hpp usualArithmeticCommonType (880-895): two BitInts → the
// wider N (equal N → unsigned); a BitInt vs a promoted standard of width W →
// N>W ? the BitInt : the standard (a standard integer out-ranks a bit-precise one
// of equal-or-lesser width); two standards → wider rank, mixed sign → unsigned
// (rank-prefer-unsigned). `isBitPrecise` marks whether the result is a `_BitInt`.
[[nodiscard]] inline BitIntOperandType
bitIntUac(BitIntOperandType a, BitIntOperandType b) noexcept {
    a = promoteBitIntOperand(a);
    b = promoteBitIntOperand(b);
    if (a.isBitPrecise && b.isBitPrecise) {
        if (a.width != b.width) return a.width > b.width ? a : b;
        return {a.width, a.isSigned && b.isSigned, true};   // equal N → unsigned wins
    }
    if (a.isBitPrecise != b.isBitPrecise) {
        BitIntOperandType const bit = a.isBitPrecise ? a : b;
        BitIntOperandType const std = a.isBitPrecise ? b : a;
        if (bit.width > std.width) return bit;
        return {std.width, std.isSigned, false};
    }
    // Both standard (dead in practice — the bignum path needs ≥1 BitInt — but kept
    // for a total function): wider rank; equal rank + mixed sign → unsigned.
    if (a.width != b.width) return a.width > b.width ? a : b;
    return {a.width, a.isSigned && b.isSigned, false};
}

// The core TypeKind for a standard (non-bit-precise) result (width ∈ {8,16,32,64}).
[[nodiscard]] inline TypeKind intKindFromWidth(std::uint32_t width, bool isSigned) noexcept {
    switch (width) {
        case 8:  return isSigned ? TypeKind::I8  : TypeKind::U8;
        case 16: return isSigned ? TypeKind::I16 : TypeKind::U16;
        case 32: return isSigned ? TypeKind::I32 : TypeKind::U32;
        default: return isSigned ? TypeKind::I64 : TypeKind::U64;   // 64 (and any residue)
    }
}

// Package a folded `BitIntValue` into a HirLiteralValue: a bit-precise result
// keeps the `BitIntValue` arm (`core == BitInt`); a standard result (width ≤ 64)
// extracts to the int64/uint64 arm with its `core` kind (so downstream int64
// bridges + the narrow-cast paths see a plain integer, exactly as the typed side
// produces a standard type for an int-outranked BitInt).
[[nodiscard]] inline HirLiteralValue
packBitIntResult(BitIntValue const& r, BitIntOperandType rt) {
    HirLiteralValue out;
    if (rt.isBitPrecise) {
        out.core  = TypeKind::BitInt;
        out.value = r;
        return out;
    }
    out.core = intKindFromWidth(rt.width, rt.isSigned);
    if (rt.isSigned) out.value = r.asI64();
    else             out.value = r.low64();
    return out;
}

// Fold a binary op with ≥1 `_BitInt` operand. `applies=false` ⇒ neither operand is
// bit-precise (caller uses the standard int64/float path). On `applies`, `ok`
// carries the folded value or `failure` the reason (div-by-zero, non-integer
// operand). Comparisons yield a Bool value; every other op yields the UAC result.
struct BitIntBinaryFold {
    bool             applies = false;
    bool             ok      = false;
    HirLiteralValue  value;
    ConstEvalFailure failure = ConstEvalFailure::None;
};
[[nodiscard]] inline BitIntBinaryFold
foldBitIntBinary(HirOpKind op, HirLiteralValue const& a, HirLiteralValue const& b) {
    bool const aBit = std::holds_alternative<BitIntValue>(a.value);
    bool const bBit = std::holds_alternative<BitIntValue>(b.value);
    BitIntBinaryFold r;
    if (!aBit && !bBit) return r;    // not a BitInt fold — caller's normal path
    r.applies = true;
    auto at = bitIntOperandType(a);
    auto bt = bitIntOperandType(b);
    auto av = asBitIntValue(a);
    auto bv = asBitIntValue(b);
    if (!at || !bt || !av || !bv) {
        r.failure = ConstEvalFailure::UnsupportedTypeKind;   // a non-integer operand
        return r;
    }
    // Comparison: fold over the common type, yield Bool.
    if (isComparison(op)) {
        BitIntOperandType const ct = bitIntUac(*at, *bt);
        int const c = BitIntValue::compare(*av, *bv, ct.width, ct.isSigned);
        bool res = false;
        switch (op) {
            case HirOpKind::Eq: res = (c == 0); break;
            case HirOpKind::Ne: res = (c != 0); break;
            case HirOpKind::Lt: res = (c <  0); break;
            case HirOpKind::Le: res = (c <= 0); break;
            case HirOpKind::Gt: res = (c >  0); break;
            case HirOpKind::Ge: res = (c >= 0); break;
            default: r.failure = ConstEvalFailure::UnsupportedOperator; return r;
        }
        r.value = makeBoolLiteral(res ? 1 : 0);
        r.ok = true;
        return r;
    }
    // Shifts: C 6.5.7 — the result type is the (promoted) LEFT operand's type; a
    // `_BitInt` does NOT promote. The count is the right operand's integer value.
    if (op == HirOpKind::Shl || op == HirOpKind::Shr) {
        BitIntOperandType const rt = promoteBitIntOperand(*at);
        std::uint64_t count = 0;
        if (auto ic = asInt64(b); ic.has_value() && *ic >= 0)
            count = static_cast<std::uint64_t>(*ic);
        else if (bBit) count = bv->low64();
        BitIntValue const res = (op == HirOpKind::Shl)
            ? BitIntValue::shiftLeft(*av, count, rt.width, rt.isSigned)
            : BitIntValue::shiftRight(*av, count, rt.width, rt.isSigned);
        r.value = packBitIntResult(res, rt);
        r.ok = true;
        return r;
    }
    BitIntOperandType const rt = bitIntUac(*at, *bt);
    switch (op) {
        case HirOpKind::Add: r.value = packBitIntResult(BitIntValue::add(*av,*bv,rt.width,rt.isSigned), rt); r.ok = true; return r;
        case HirOpKind::Sub: r.value = packBitIntResult(BitIntValue::sub(*av,*bv,rt.width,rt.isSigned), rt); r.ok = true; return r;
        case HirOpKind::Mul: r.value = packBitIntResult(BitIntValue::mul(*av,*bv,rt.width,rt.isSigned), rt); r.ok = true; return r;
        case HirOpKind::BitAnd: r.value = packBitIntResult(BitIntValue::bitAnd(*av,*bv,rt.width,rt.isSigned), rt); r.ok = true; return r;
        case HirOpKind::BitOr:  r.value = packBitIntResult(BitIntValue::bitOr(*av,*bv,rt.width,rt.isSigned), rt); r.ok = true; return r;
        case HirOpKind::BitXor: r.value = packBitIntResult(BitIntValue::bitXor(*av,*bv,rt.width,rt.isSigned), rt); r.ok = true; return r;
        case HirOpKind::Div: {
            auto q = BitIntValue::divide(*av, *bv, rt.width, rt.isSigned);
            if (!q.has_value()) { r.failure = ConstEvalFailure::DivisionByZero; return r; }
            r.value = packBitIntResult(*q, rt); r.ok = true; return r;
        }
        case HirOpKind::Rem: {
            auto rem = BitIntValue::remainder(*av, *bv, rt.width, rt.isSigned);
            if (!rem.has_value()) { r.failure = ConstEvalFailure::DivisionByZero; return r; }
            r.value = packBitIntResult(*rem, rt); r.ok = true; return r;
        }
        default: r.failure = ConstEvalFailure::UnsupportedOperator; return r;
    }
}

// Fold a unary op on a `_BitInt` operand (the caller gates on the operand holding
// a `BitIntValue` arm). Neg/BitNot yield the operand's own `_BitInt` type; `Not`
// (logical negation) yields Bool (isZero).
struct BitIntUnaryFold {
    bool             applies = false;
    bool             ok      = false;
    HirLiteralValue  value;
    ConstEvalFailure failure = ConstEvalFailure::None;
};
[[nodiscard]] inline BitIntUnaryFold
foldBitIntUnary(HirOpKind op, HirLiteralValue const& inner) {
    BitIntUnaryFold r;
    auto const* bv = std::get_if<BitIntValue>(&inner.value);
    if (bv == nullptr) return r;    // not a BitInt operand — caller's normal path
    r.applies = true;
    std::uint32_t const w = bv->width();
    bool const s = bv->isSigned();
    switch (op) {
        case HirOpKind::Neg:
            r.value = packBitIntResult(BitIntValue::neg(*bv, w, s), {w, s, true});
            r.ok = true; return r;
        case HirOpKind::BitNot:
            r.value = packBitIntResult(BitIntValue::bitNot(*bv, w, s), {w, s, true});
            r.ok = true; return r;
        case HirOpKind::Not:
            r.value = makeBoolLiteral(bv->isZero() ? 1 : 0);
            r.ok = true; return r;
        default:
            r.failure = ConstEvalFailure::UnsupportedOperator; return r;
    }
}

} // namespace dss::detail
