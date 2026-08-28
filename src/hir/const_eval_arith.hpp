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
#include "core/types/wide_float_value.hpp"   // LD-3: F80/F128 target-precision fold kernel
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
// arithmetic representation. Bridges the four numeric variant arms
// (`int64_t` / `uint64_t` / `bool` / `BitIntValue`). Returns nullopt for
// non-integer variants, and — uniformly across every arm — for any value
// outside int64's range, whatever container it arrived in (the caller
// refuses with `Overflow`, or with "not an integer constant expression").
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
    // C4b (D-CSUBSET-BITINT-CONSTFOLD-LARGE): a `_BitInt`/`__int128` const value
    // bridges to int64 (both are integer types, admissible in an ICE — e.g.
    // `int a[(_BitInt(8))30]`, `int a[(__int128)2 + 1]`).
    //
    // ── THE RULE IS THE VALUE'S MAGNITUDE, NOT ITS DECLARED WIDTH ─────────────
    // D-CE-ASINT64-REJECTS-BY-WIDTH-NOT-MAGNITUDE / D-CSUBSET-INT128-ICE-CONTEXT-REFUSED.
    // This arm used to nullopt for EVERY `width() > 64` value, however small — so
    // `int a[(__int128)2 + 1];` and `int a[(__uint128_t)3];` were refused while gcc
    // 13.3.0 (`-std=c2x`) and clang 18.1.3 (`-std=c23`), probed SEPARATELY, both
    // accept them. ★ AND IT CONTRADICTED THIS VERB'S OWN OTHER ARM: the `uint64_t`
    // arm six lines up already answers by MAGNITUDE (`> INT64_MAX → nullopt`). One
    // verb was carrying two incompatible rules for one question ("does this value
    // fit an int64?"); the width test was the wrong one.
    //
    // DEFINITION: the value fits iff `INT64_MIN ≤ value ≤ INT64_MAX`, compared as
    // the value it IS at its declared (width, signedness). Equivalently — and this
    // is the form implemented below, so it stays `noexcept` and allocation-free —
    // its two's-complement bit pattern must BE the sign-extension of its low 64
    // bits AT THE VALUE'S OWN SIGN. `ConstEvalAsInt64Magnitude.MatchesTheConvertToRoundTrip`
    // pins the fast form against the range definition (spelled with the bignum's own
    // `compare` against both int64 bounds) over a width × signedness × limb-pattern
    // matrix, so the two cannot drift apart.
    //
    // ⚠ BOTH HALVES OF THE SIGN CHECK ARE LOAD-BEARING, AND THE SECOND ONE WAS
    // MEASURED THE HARD WAY. Testing only "do the upper limbs repeat bit 63" admits
    // `(__uint128_t)-1` — an UNSIGNED 2^128−1, whose limbs are all ones — and hands
    // back −1, so `enum { EA = (__uint128_t)-1 };` would have silently become −1.
    // An unsigned value is NEVER negative however its bits read, so bit 63 must
    // already agree with the value's actual sign before the upper limbs are
    // consulted at all.
    //
    // ⚠ THE TRUNCATION GUARD IS THE WHOLE POINT, AND IT IS STRICTER THAN A WIDTH
    // TEST RATHER THAN WEAKER: a value that does NOT fit still nullopts, so a
    // 64-bit ICE slot fails loud instead of silently taking a low-64-bit slice.
    // `int a[((__int128)1 << 100) + 3];` — whose low 64 bits are 3 — must never
    // become `int a[3]`, and does not: limb 1 is non-zero, so this returns nullopt.
    // Pinned end-to-end by `examples/c/c_int128_ice_slot_overflow_error`.
    //
    // ⓘ ONE W ≤ 64 BEHAVIOUR MOVES, DELIBERATELY: an `unsigned _BitInt(N≤64)` whose
    // value exceeds INT64_MAX used to bridge to a NEGATIVE int64 (`asI64()` on the
    // raw bits) and now nullopts. That is this verb's `uint64_t` arm's rule applied
    // to the arm that was contradicting it — the old answer made
    // `enum { EA = (unsigned _BitInt(64))9223372036854775808uwb };` a negative
    // enumerator. Every value at or below INT64_MAX, and every signed `_BitInt`,
    // is bit-identical to what shipped.
    if (auto p = std::get_if<BitIntValue>(&v.value)) {
        std::uint64_t const lo = p->low64();
        // The value's REAL sign at its declared type (`isNegative` is false for
        // every unsigned width, whatever the bits say).
        bool const neg = p->isNegative();
        if ((static_cast<std::int64_t>(lo) < 0) != neg) return std::nullopt;
        // Every bit above 63 must repeat that sign. `wrapTo`/`maskTopLimb` keep the
        // stored limbs canonical — bits above the declared width already carry the
        // extension (the invariant `paddingByte` documents) — so the stored limbs
        // can be read directly, with no widening allocation.
        std::uint64_t const fill = neg ? ~std::uint64_t{0} : std::uint64_t{0};
        for (std::size_t i = 1; i < p->limbCount(); ++i) {
            if (p->limbs()[i] != fill) return std::nullopt;
        }
        return static_cast<std::int64_t>(lo);
    }
    return std::nullopt;
}

// ── THE OPERAND'S BIT PATTERN, FOR USE WHERE THE DOMAIN IS KNOWN ───────────
// D-HIR-CONSTEVAL-UNSIGNED-WRAPAROUND-NOT-MODULAR.
//
// `asInt64` above answers "what is this value, as a signed int64?" and honestly
// nullopts for an unsigned payload above INT64_MAX, because that value is not
// representable. Array dimensions and enum bounds want exactly that question
// and keep asking it.
//
// An ARITHMETIC operand wants a different one. MEASURED: every expression over
// `18446744073709551615ull` -- including `_Static_assert(0xffffffffffffffffull
// == 0xffffffffffffffffull)` -- was rejected as "not an integer constant
// expression", because the operand could not cross the int64 bridge at all.
// The int64 storage IS the correct two's-complement bit pattern for that value;
// what was missing was any consumer entitled to read it as one. Now that
// `applyBinaryInt` establishes the operation's (width, signedness) domain
// before it touches an operand, it IS entitled -- the domain says how to read
// the bits, so nothing downstream can mistake them for a signed -1.
//
// ⚠ USE THIS ONLY BEHIND AN ESTABLISHED DOMAIN. Where the question really is
// "does this value fit", `asInt64` remains the right verb and this one would
// silently answer a question that was not asked.
[[nodiscard]] inline std::optional<std::int64_t>
asIntBits(HirLiteralValue const& v) noexcept {
    if (auto p = std::get_if<std::uint64_t>(&v.value))
        return static_cast<std::int64_t>(*p);
    return asInt64(v);
}

[[nodiscard]] inline bool isFloatValue(HirLiteralValue const& v) noexcept {
    // LD-3: an F80/F128 value lives in EITHER the `double` arm (unfolded leaf) or
    // the `WideFloatValue` arm (folded/widened) — both are "float" for the routing
    // in combineUnary/combineBinary/combineCast, so recognize both.
    return std::holds_alternative<double>(v.value)
        || std::holds_alternative<WideFloatValue>(v.value);
}

// Pull a numeric `HirLiteralValue` into `double` for IEEE 754
// arithmetic. Large uint64/int64 values may lose precision — acceptable
// per IEEE 754 (the same loss happens at runtime). Returns nullopt for
// non-numeric arms.
[[nodiscard]] inline std::optional<double>
asDouble(HirLiteralValue const& v) noexcept {
    // ★ LD-3 FAIL-LOUD (no silent mis-fold): a `WideFloatValue` (F80/F128) arm
    // returns nullopt HERE — it must NEVER be silently narrowed to a rounded
    // `double` (that would defeat the whole point of the target-precision fold).
    // A forgetful future caller that reaches an F80/F128 value through asDouble
    // then crashes loud on the empty optional rather than baking a binary64 value.
    // Wide-float consumers go through `toWideFloatOperand` (arm-checked) instead.
    if (std::holds_alternative<WideFloatValue>(v.value)) return std::nullopt;
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
    // LD-3: an F80/F128 value's truthiness folds via `isZero` — NEVER via
    // `asDouble` (which nullopts for this arm; a `*asDouble(v)` deref would be UB).
    // ±0.0 is false; every other value (incl. NaN / ±inf) is true, per C99.
    if (auto p = std::get_if<WideFloatValue>(&v.value)) {
        if (!allowFloat) return std::nullopt;
        return !p->isZero();
    }
    if (isFloatValue(v)) {   // the `double` arm (F16/F32/F64 and unfolded F80/F128 leaves)
        if (!allowFloat) return std::nullopt;
        return *asDouble(v) != 0.0;
    }
    // C4b: a `_BitInt` truthiness folds via `isZero` for EVERY N — never through
    // `asInt64`, which nullopt-fails for a value that does not FIT an int64 (a
    // `_BitInt` condition holding such a value in `&&`/`||`/`?:` would then
    // spuriously fail the whole fold, though its truth value is perfectly defined).
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

// FC17.9(e) → LD-3 (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION): does the
// operand's core refuse host-double folding? A non-host-backed float kind folded
// at binary64 precision would produce a silently-ROUNDED constant that then RUNS
// (never reaching the LIR encoded-width wall) — a precision mis-bind for any
// value needing more than a 53-bit mantissa. That was the original F80/F128 gate.
//
// LD-3 RELAXES it for the kinds the `WideFloatValue` soft-float now folds at TRUE
// target precision: refuse ONLY a non-host-backed float that is ALSO not a
// supported wide kind (`WideFloatValue::isSupportedKind`). Today F80 and F128 are
// both supported, so this is dead for them — the wide-float dispatch in
// `applyUnaryFloat`/`applyBinaryFloat` (checked BEFORE the asDouble path) folds
// them. `floatKindInfo`/`hostBacked` stay UNTOUCHED (hostBacked keeps its true
// meaning — the `double` arm cannot carry an exact F80/F128 value; the new arm
// does); F16 is UNAFFECTED. The gate remains as defense for any FUTURE
// non-host-backed kind the kernel does not yet realize.
[[nodiscard]] inline bool refusesHostFloatFold(TypeKind k) noexcept {
    auto const fi = floatKindInfo(k);
    return fi.has_value() && !fi->hostBacked && !WideFloatValue::isSupportedKind(k);
}

// LD-3: pull a float-typed operand into a `WideFloatValue` at `kernelKind` (F80/
// F128) for the soft-float engine. A `WideFloatValue` arm passes through when its
// kind already matches `kernelKind` (a cross-kind operand — F80 vs F128, never
// produced by valid C — is a defensive nullopt refuse). Every other numeric arm
// (an unfolded F80/F128 `double` leaf, an F64/F32 `double`, or a promoted integer)
// widens EXACTLY-or-IEEE via `fromDouble(asDouble(...))` — 53-bit ⊆ 64/113-bit for
// a genuine long-double leaf; a huge-int operand shares the documented binary64
// precision loss the f64 path already accepts (and the typed tier normally inserts
// an explicit widening Cast first, so an int operand rarely reaches here).
[[nodiscard]] inline std::optional<WideFloatValue>
toWideFloatOperand(HirLiteralValue const& v, TypeKind kernelKind) noexcept {
    if (auto p = std::get_if<WideFloatValue>(&v.value)) {
        if (p->kind() != kernelKind) return std::nullopt;   // cross-kind defensive refuse
        return *p;
    }
    auto dv = asDouble(v);
    if (!dv.has_value()) return std::nullopt;
    return WideFloatValue::fromDouble(*dv, kernelKind);
}

// LD-3: the F80/F128 kind for a wide-float fold of operand cores `ac`/`bc` — the
// supported (F80/F128) kind present. nullopt = neither is wide (use the double
// path). A supported-but-DIFFERENT pair (F80 vs F128) returns nullopt AND sets
// `crossKind` (the caller refuses loud) — valid C never mixes the two axes.
[[nodiscard]] inline std::optional<TypeKind>
wideFloatFoldKind(TypeKind ac, TypeKind bc, bool& crossKind) noexcept {
    crossKind = false;
    bool const aWide = WideFloatValue::isSupportedKind(ac);
    bool const bWide = WideFloatValue::isSupportedKind(bc);
    if (!aWide && !bWide) return std::nullopt;
    if (aWide && bWide && ac != bc) { crossKind = true; return std::nullopt; }
    return aWide ? ac : bc;
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
    // LD-3: an F80/F128 operand folds through the soft-float kernel at TRUE target
    // precision — BEFORE the asDouble path (which nullopts for the new arm). The
    // result core is the OPERAND's core (F80/F128), NOT a hardcoded F64.
    if (WideFloatValue::isSupportedKind(inner.core)) {
        auto w = toWideFloatOperand(inner, inner.core);
        if (!w.has_value()) { outFailure = ConstEvalFailure::UnsupportedTypeKind; return std::nullopt; }
        switch (op) {
            case HirOpKind::Neg: {
                HirLiteralValue folded;
                folded.core  = inner.core;
                folded.value = w->negate();   // exact sign flip
                return folded;
            }
            default:
                outFailure = ConstEvalFailure::UnsupportedTypeKind;   // BitNot/Not on float
                return std::nullopt;
        }
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

// ⚠ INT64-DOMAIN HELPER (D-CSUBSET-INT128-CONSTFOLD, TF-C94). `v` is an
// `std::int64_t`, so this can only ever express a 64-bit result: for
// `target.bits >= 64` it returns `v` UNCHANGED, which is right for a 64-bit
// target and for a signed 128-bit one (every int64 is already its own 128-bit
// value), but is NOT a general 128-bit wrap — `wrapToIntTarget(-1, {128,false})`
// would have to yield 2^128-1, which no `std::int64_t` can hold. That case is
// unreachable rather than handled: `valueFitsInIntTarget(v, {128,false})` is
// `v >= 0`, so a negative value is REFUSED before any caller wraps it, and every
// 128-bit fold now routes through the bignum (`foldBitIntBinary` /
// the 128-bit cast arms) long before reaching here. Keep it that way — a caller
// that wraps a 128-bit target through this function silently truncates to 64
// bits, and there is no failure channel here to say so.
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


// True for the two 128-bit standard integer kinds — the widths that are STANDARD
// (not bit-precise) yet still too wide for the int64/uint64 literal arms, so they
// must stay in the `BitIntValue` bignum arm (D-CSUBSET-INT128-CONSTFOLD, TF-C94).
[[nodiscard]] inline bool isInt128Kind(TypeKind k) noexcept {
    return k == TypeKind::I128 || k == TypeKind::U128;
}

// A `_BitInt`-UAC operand descriptor. A standard integer arm's (width, signed)
// come from its `core` via `intKindInfo` (the CST leaf tags integer literals I32;
// sizeof → U64; a cast → its target). nullopt for a non-integer arm.
struct BitIntOperandType { std::uint32_t width; bool isSigned; bool isBitPrecise; };

// ── C 6.2.5p9 / 6.3.1.3p2: THE OPERATION'S TYPE, NOT THE HOST'S ────────────
// D-HIR-CONSTEVAL-UNSIGNED-WRAPAROUND-NOT-MODULAR (+ its shared-root sibling
// D-CE-NEGATIVE-WIDENED-TO-U64-NOT-CONSTFOLDABLE).
//
// `applyBinaryInt` used to evaluate every integer fold in the HOST's signed
// int64 domain and inherit the LHS's core, with no notion of the C type the
// operation actually has. Three consequences, all MEASURED against clang AND
// gcc (both accept all 103 matrix cells; DSS failed 46):
//   1. an unsigned result was never reduced mod 2^N -- `0u - 1u` stayed -1
//      instead of 0xffffffff, so `0u - 1u == 0xffffffffu` was FALSE;
//   2. every comparison was SIGNED whatever the operands were -- `(0u-1u) > 0u`
//      and `18446744073709551615ull > 0ull` both folded to false;
//   3. `/`, `%` and `>>` used signed division and an ARITHMETIC right shift on
//      unsigned operands.
// ★ THE HARM DIRECTION IS THE REASON THIS IS P0: a `_Static_assert` written in
// the positive fails LOUDLY, but the same fact written in the negative
// (`!=`, `>`) SILENTLY PASSES, and every const-folded relational context --
// array size, `case` label, enum bound -- takes the wrong value with no
// diagnostic at all.
//
// ★★ THE VERB IS NOT NEW. `bitIntUac` below already computes the C23 usual
// arithmetic conversions, and its "both standard" arm carried the comment
// "dead in practice -- the bignum path needs >=1 BitInt -- but kept for a total
// function". That arm is exactly this case; the fix makes it LIVE rather than
// adding a second, parallel standard-integer UAC. Same for `wrapToIntTarget`
// (the modular reduction), `promoteBitIntOperand` (C 6.3.1.8) and
// `intKindFromWidth`. Nothing here is a private `Int*` verb set.
[[nodiscard]] inline std::optional<BitIntOperandType>
bitIntOperandType(HirLiteralValue const& v) noexcept;
[[nodiscard]] inline BitIntOperandType
promoteBitIntOperand(BitIntOperandType t) noexcept;
[[nodiscard]] inline BitIntOperandType
bitIntUac(BitIntOperandType a, BitIntOperandType b) noexcept;
[[nodiscard]] inline TypeKind
intKindFromWidth(std::uint32_t width, bool isSigned) noexcept;

// True for the shift operators, whose result type is C 6.5.7p3's PROMOTED LEFT
// operand -- NOT the usual arithmetic conversions. Getting this wrong would
// make `1u << 1` unsigned-wrap at the RHS's width.
[[nodiscard]] inline bool isShiftOp(HirOpKind op) noexcept {
    return op == HirOpKind::Shl || op == HirOpKind::Shr;
}

// The (width, signedness) the C abstract machine performs `op` in. nullopt when
// either operand is not a standard integer -- a `_BitInt` pair is the bignum
// path's business (`foldBitIntBinary`), and a non-integer never reaches here.
// A width above 64 also yields nullopt: this function's callers hold `int64_t`,
// so a 128-bit domain cannot be expressed and MUST NOT be silently truncated
// (D-CSUBSET-INT128-CONSTFOLD-WIDE routes those through the bignum instead).
[[nodiscard]] inline std::optional<IntKindInfo>
intOpDomain(HirOpKind op, HirLiteralValue const& a, HirLiteralValue const& b) noexcept {
    auto at = bitIntOperandType(a);
    if (!at.has_value() || at->isBitPrecise) return std::nullopt;
    BitIntOperandType rt{};
    if (isShiftOp(op)) {
        rt = promoteBitIntOperand(*at);          // C 6.5.7p3
    } else {
        auto bt = bitIntOperandType(b);
        if (!bt.has_value() || bt->isBitPrecise) return std::nullopt;
        rt = bitIntUac(*at, *bt);                // C 6.3.1.8
    }
    if (rt.isBitPrecise || rt.width > 64) return std::nullopt;
    return IntKindInfo{static_cast<int>(rt.width), rt.isSigned};
}

// Fold a BinaryOp over two integer operands per the EvalOptions policy.
[[nodiscard]] inline std::optional<HirLiteralValue>
applyBinaryInt(HirOpKind op, HirLiteralValue const& a, HirLiteralValue const& b,
               EvalOptions const& opts, ConstEvalFailure& outFailure) {
    auto av64 = asIntBits(a);
    auto bv64 = asIntBits(b);
    if (!av64.has_value() || !bv64.has_value()) return std::nullopt;
    // ── CONVERT TO THE OPERATION'S TYPE (C 6.3.1.3p2 -- modular, ALWAYS
    // defined, never an overflow). `wrapToIntTarget` is identity at width 64,
    // where the int64 payload is already the right bit pattern; below 64 it
    // masks and re-extends. `dom` absent = a `_BitInt`/128-bit/non-integer pair
    // that this int64 path does not own, so the historical behaviour stands.
    auto const dom = intOpDomain(op, a, b);
    std::int64_t const av = dom.has_value() ? wrapToIntTarget(*av64, *dom) : *av64;
    // The RHS of a shift keeps its own value: it is a COUNT, not an operand of
    // the result type (C 6.5.7p3), and the range check below reads it directly.
    std::int64_t const bv = (dom.has_value() && !isShiftOp(op))
                              ? wrapToIntTarget(*bv64, *dom)
                              : *bv64;
    bool const uns = dom.has_value() && !dom->isSigned;
    auto const U   = [](std::int64_t v) noexcept { return static_cast<std::uint64_t>(v); };
    // Reduce an arithmetic RESULT to the operation's type -- C 6.2.5p9 for the
    // unsigned case. Signed overflow is C UB and keeps its existing wrap-or-
    // refuse policy, so this is identity for a signed 64-bit domain.
    auto const R = [&](std::int64_t v) noexcept {
        return dom.has_value() ? wrapToIntTarget(v, *dom) : v;
    };
    HirLiteralValue folded = a;
    // The result's core is the operation's type, not the LHS's. Comparison
    // results are re-tagged Bool by every caller; shift and arithmetic cores
    // are additionally re-tagged from the authoritative TypeId by the HIR
    // engine, so this is the CST engine's only source of a correct core.
    if (dom.has_value() && !isComparison(op)) {
        folded.core = intKindFromWidth(static_cast<std::uint32_t>(dom->bits),
                                       dom->isSigned);
    }
    switch (op) {
        // Wrapping forms (D-CE-HOST-SIGNED-OVERFLOW-UB, see the helpers
        // above): the direct `av + bv` was host UB at INT64 overflow.
        case HirOpKind::Add:    folded.value = R(wrapAddI64(av, bv)); return folded;
        case HirOpKind::Sub:    folded.value = R(wrapSubI64(av, bv)); return folded;
        case HirOpKind::Mul:    folded.value = R(wrapMulI64(av, bv)); return folded;
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
            // UNSIGNED division is a different operation, not the same one on
            // a differently-labelled value: `0xffffffffffffffffull / 2` is
            // 9223372036854775807, while the signed `-1 / 2` is 0. It also has
            // NO undefined corner -- the INT64_MIN/-1 refusal above is a SIGNED
            // fact -- so it is taken before that guard can misfire on the bit
            // pattern (`av == INT64_MIN` is a perfectly ordinary 2^63 here).
            if (uns) { folded.value = R(static_cast<std::int64_t>(U(av) / U(bv)));
                       return folded; }
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
            // Unsigned `%` -- see the Div arm; same reasoning, same placement.
            if (uns) { folded.value = R(static_cast<std::int64_t>(U(av) % U(bv)));
                       return folded; }
            if (av == std::numeric_limits<std::int64_t>::min() && bv == -1) {
                outFailure = ConstEvalFailure::Overflow;
                return std::nullopt;
            }
            folded.value = av % bv; return folded;
        // Bitwise ops are representation-level and identical in both
        // signednesses; `R` still runs so the result cannot carry stale bits
        // above the operation's width (e.g. `~0u` narrowed to 32 bits).
        case HirOpKind::BitAnd: folded.value = R(av & bv);  return folded;
        case HirOpKind::BitOr:  folded.value = R(av | bv);  return folded;
        case HirOpKind::BitXor: folded.value = R(av ^ bv);  return folded;
        case HirOpKind::Shl: {
            if (bv < 0 || bv >= 64) {
                outFailure = opts.refuseOnShiftOutOfRange
                    ? ConstEvalFailure::ShiftCountOutOfRange
                    : ConstEvalFailure::NotAConstantExpression;
                return std::nullopt;
            }
            folded.value = R(static_cast<std::int64_t>(U(av) << bv));
            return folded;
        }
        case HirOpKind::Shr: {
            if (bv < 0 || bv >= 64) {
                outFailure = opts.refuseOnShiftOutOfRange
                    ? ConstEvalFailure::ShiftCountOutOfRange
                    : ConstEvalFailure::NotAConstantExpression;
                return std::nullopt;
            }
            // C 6.5.7p5: `>>` on an UNSIGNED left operand is a LOGICAL shift
            // (the vacated bits are zero). The signed `av >> bv` below is an
            // arithmetic shift, which fills them with the sign bit -- so
            // `0xffffffffffffffffull >> 1` yielded -1 instead of 2^63-1.
            folded.value = uns ? R(static_cast<std::int64_t>(U(av) >> bv))
                               : R(av >> bv);
            return folded;
        }
        // ── COMPARISONS RUN IN THE OPERANDS' OWN DOMAIN ────────────────────
        // Both sides were converted to the common type above, so the only
        // question left is which ORDER to read them in. An unsigned common
        // type orders by magnitude: `0xffffffffu > 0u` is true, while the same
        // bit pattern read as int64 (-1) says false. This is the arm that made
        // a negatively-written assertion pass silently.
        case HirOpKind::Eq: folded.value = std::int64_t{av == bv}; return folded;
        case HirOpKind::Ne: folded.value = std::int64_t{av != bv}; return folded;
        case HirOpKind::Lt: folded.value = std::int64_t{uns ? U(av) <  U(bv) : av <  bv}; return folded;
        case HirOpKind::Le: folded.value = std::int64_t{uns ? U(av) <= U(bv) : av <= bv}; return folded;
        case HirOpKind::Gt: folded.value = std::int64_t{uns ? U(av) >  U(bv) : av >  bv}; return folded;
        case HirOpKind::Ge: folded.value = std::int64_t{uns ? U(av) >= U(bv) : av >= bv}; return folded;
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
    // FC17.9(e): refuse when EITHER operand's core is a non-host-backed float that
    // the kernel does NOT realize — see refusesHostFloatFold (dead for F80/F128
    // now that LD-3 folds them; live as defense for a future unrealized kind).
    if (refusesHostFloatFold(a.core) || refusesHostFloatFold(b.core)) {
        outFailure = ConstEvalFailure::UnsupportedTypeKind;
        return std::nullopt;
    }
    // ★ LD-3: an F80/F128 operand folds through the soft-float kernel at TRUE
    // target precision — BEFORE the asDouble path. Homogenize both operands to the
    // kernel kind (F80 or F128), then arithmetic → kernel; comparisons → compare().
    // The arithmetic RESULT core is the KERNEL kind (NOT the hardcoded F64 below);
    // the caller's commonType retag agrees (F80/F128 outranks F64). A subnormal
    // result / cross-kind mismatch is a fail-loud nullopt (UnsupportedTypeKind).
    {
        bool crossKind = false;
        auto kk = wideFloatFoldKind(a.core, b.core, crossKind);
        if (crossKind) { outFailure = ConstEvalFailure::UnsupportedTypeKind; return std::nullopt; }
        if (kk.has_value()) {
            auto wa = toWideFloatOperand(a, *kk);
            auto wb = toWideFloatOperand(b, *kk);
            if (!wa.has_value() || !wb.has_value()) {
                outFailure = ConstEvalFailure::UnsupportedTypeKind;
                return std::nullopt;
            }
            if (isComparison(op)) {
                WideFloatValue::Ordering const ord = WideFloatValue::compare(*wa, *wb);
                using O = WideFloatValue::Ordering;
                bool res = false;
                switch (op) {
                    case HirOpKind::Eq: res = (ord == O::Equal); break;
                    case HirOpKind::Ne: res = (ord != O::Equal); break;   // NaN → true
                    case HirOpKind::Lt: res = (ord == O::Less); break;
                    case HirOpKind::Le: res = (ord == O::Less || ord == O::Equal); break;
                    case HirOpKind::Gt: res = (ord == O::Greater); break;
                    case HirOpKind::Ge: res = (ord == O::Greater || ord == O::Equal); break;
                    default: outFailure = ConstEvalFailure::UnsupportedTypeKind; return std::nullopt;
                }
                return makeBoolLiteral(res ? 1 : 0);
            }
            std::optional<WideFloatValue> r;
            switch (op) {
                case HirOpKind::Add: r = WideFloatValue::add(*wa, *wb); break;
                case HirOpKind::Sub: r = WideFloatValue::sub(*wa, *wb); break;
                case HirOpKind::Mul: r = WideFloatValue::mul(*wa, *wb); break;
                case HirOpKind::Div: r = WideFloatValue::div(*wa, *wb); break;   // x/0 → signed inf/NaN
                default:
                    outFailure = ConstEvalFailure::UnsupportedTypeKind;   // Rem etc. on float
                    return std::nullopt;
            }
            if (!r.has_value()) {
                // Subnormal RESULT (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-SUBNORMAL-RESULT)
                // or a defensive cross-kind — fail loud, NEVER silently flushed.
                outFailure = ConstEvalFailure::UnsupportedTypeKind;
                return std::nullopt;
            }
            HirLiteralValue folded;
            folded.core  = *kk;        // the kernel kind — NOT F64
            folded.value = *r;
            return folded;
        }
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

// ── C23 _BitInt(N) wrap-aware const-fold (C4b, D-CSUBSET-BITINT-CONSTFOLD-LARGE) ──
//
// The shared bignum fold both const-eval walkers (`const_eval.cpp` /
// `cst_const_eval.cpp`) route through when at least ONE operand is a `_BitInt`
// value (the `BitIntValue` pool arm). It mirrors the TYPED side's usual-
// arithmetic-conversion (type_rules.hpp usualArithmeticCommonType, lines
// 626-633/880-895) to compute the TRUE C23 result (width, signed) and wraps the
// bignum fold AT THAT width — NOT naively at an operand width (which miscompiles
// `15wb + 1`, whose int-outranked-BitInt result is a plain `int`, no wrap → 16).


[[nodiscard]] inline std::optional<BitIntOperandType>
bitIntOperandType(HirLiteralValue const& v) noexcept {
    // D-CSUBSET-INT128-CONSTFOLD (TF-C94): `core` is consulted BEFORE the variant
    // arm, because after this cycle the two no longer determine each other. A
    // folded 128-bit STANDARD value rides the `BitIntValue` payload (nothing
    // narrower holds 128 bits) while carrying an I128/U128 core, so a
    // variant-first test would report `isBitPrecise = true` for it and
    // `packBitIntResult` would then retag the result `_BitInt` — a `__int128`
    // expression silently changing type mid-fold. Ordering the checks this way
    // makes `core` the authority on bit-preciseness and the variant merely the
    // storage. `bitIntIsSigned` for these kinds is the kind's own signedness.
    if (isInt128Kind(v.core)) {
        return BitIntOperandType{128, v.core == TypeKind::I128, false};
    }
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

// The core TypeKind for a standard (non-bit-precise) result
// (width ∈ {8,16,32,64,128}).
[[nodiscard]] inline TypeKind intKindFromWidth(std::uint32_t width, bool isSigned) noexcept {
    switch (width) {
        case 8:  return isSigned ? TypeKind::I8  : TypeKind::U8;
        case 16: return isSigned ? TypeKind::I16 : TypeKind::U16;
        case 32: return isSigned ? TypeKind::I32 : TypeKind::U32;
        // D-CSUBSET-INT128-CONSTFOLD (TF-C94): 128 is a REAL standard width now
        // (`__int128`/`__uint128_t` bind to I128/U128 and `intKindInfo` already
        // reports {128, signed}). Before this arm it fell to the `default` and a
        // 128-bit result was mislabelled I64/U64 — the root of the silent 64-bit
        // wrap, since `packBitIntResult` then extracted only the low 64 bits.
        case 128: return isSigned ? TypeKind::I128 : TypeKind::U128;
        default: return isSigned ? TypeKind::I64 : TypeKind::U64;   // 64 (and any residue)
    }
}

// Package a folded `BitIntValue` into a HirLiteralValue: a bit-precise result
// keeps the `BitIntValue` arm (`core == BitInt`); a standard result of width ≤ 64
// extracts to the int64/uint64 arm with its `core` kind (so downstream int64
// bridges + the narrow-cast paths see a plain integer, exactly as the typed side
// produces a standard type for an int-outranked BitInt).
//
// D-CSUBSET-INT128-CONSTFOLD (TF-C94): a 128-bit STANDARD result is the third
// case. It is not bit-precise (its core is I128/U128, never BitInt), but it does
// NOT fit the int64/uint64 arms either — `asI64()`/`low64()` would silently drop
// the high 64 bits, which is exactly the mod-2^64 wrap this cycle exists to
// prevent. It therefore keeps the `BitIntValue` payload while carrying an
// I128/U128 core. That deliberately breaks the old
// "BitIntValue arm ⟺ core == BitInt" invariant, so every consumer that keys on
// the VARIANT to say something `_BitInt`-specific must key on `core` instead —
// see the audit note at the `BitIntValue` arm in asm.cpp.
[[nodiscard]] inline HirLiteralValue
packBitIntResult(BitIntValue const& r, BitIntOperandType rt) {
    HirLiteralValue out;
    if (rt.isBitPrecise) {
        out.core  = TypeKind::BitInt;
        out.value = r;
        return out;
    }
    out.core = intKindFromWidth(rt.width, rt.isSigned);
    if (isInt128Kind(out.core)) {
        out.value = r;              // 128 bits — the bignum arm is the only one wide enough
        return out;
    }
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
    // D-CSUBSET-INT128-CONSTFOLD (TF-C94): entry is by "does this fold need MORE
    // than 64 bits?", not by "is a `BitIntValue` variant present?". The old
    // variant-only predicate was the reason a PURE 128-bit fold silently wrapped
    // at 64: `__uint128_t` values whose magnitude fits in 64 bits carry a plain
    // u64/i64 arm and NO `BitIntValue`, so the bignum was never entered and the
    // caller's int64 path folded them mod 2^64. Keying on `core` as well catches
    // them. The downstream machinery already handles these operands with no
    // change — `bitIntOperandType` reads {128, signed} straight out of
    // `intKindInfo(core)`, `asBitIntValue` widens the int64 arm to a 128-bit
    // bignum, and `bitIntUac`'s two-standard branch ranks 128 above every
    // narrower kind — so this predicate is the whole of the entry fix.
    bool const aBit = std::holds_alternative<BitIntValue>(a.value)
                   || isInt128Kind(a.core);
    bool const bBit = std::holds_alternative<BitIntValue>(b.value)
                   || isInt128Kind(b.core);
    BitIntBinaryFold r;
    if (!aBit && !bBit) return r;    // not a wide fold — caller's normal path
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
    // D-CSUBSET-INT128-CONSTFOLD (TF-C94): the binary entry's twin — enter for a
    // 128-bit STANDARD operand too, not only for a `BitIntValue` variant, or
    // `-(__int128)x` / `~(__uint128_t)x` fall to the caller's int64 path and wrap
    // at 64 bits. `bitIntOperandType` supplies the operand's true (width, signed,
    // isBitPrecise) triple, and threading that triple into `packBitIntResult` —
    // instead of the hard-coded `true` this used to pass — is what keeps a 128-bit
    // result labelled I128/U128 rather than mislabelled `_BitInt`.
    bool const isBitVariant = std::holds_alternative<BitIntValue>(inner.value);
    if (!isBitVariant && !isInt128Kind(inner.core)) {
        return r;                   // not a wide operand — caller's normal path
    }
    r.applies = true;
    auto ot = bitIntOperandType(inner);
    auto bv = asBitIntValue(inner);
    if (!ot.has_value() || !bv.has_value()) {
        r.failure = ConstEvalFailure::UnsupportedTypeKind;   // a non-integer operand
        return r;
    }
    std::uint32_t const w = ot->width;
    bool const s = ot->isSigned;
    switch (op) {
        case HirOpKind::Neg:
            r.value = packBitIntResult(BitIntValue::neg(*bv, w, s), *ot);
            r.ok = true; return r;
        case HirOpKind::BitNot:
            r.value = packBitIntResult(BitIntValue::bitNot(*bv, w, s), *ot);
            r.ok = true; return r;
        case HirOpKind::Not:
            r.value = makeBoolLiteral(bv->isZero() ? 1 : 0);
            r.ok = true; return r;
        default:
            r.failure = ConstEvalFailure::UnsupportedOperator; return r;
    }
}

} // namespace dss::detail
