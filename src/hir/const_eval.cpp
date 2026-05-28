#include "hir/const_eval.hpp"

#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/hir_op.hpp"

#include <cmath>
#include <limits>
#include <unordered_set>

namespace dss {

namespace {

[[nodiscard]] ConstEvalResult fail(ConstEvalFailure why, HirNodeId blamed) {
    return ConstEvalResult{.value{}, .failure = why, .blamedNode = blamed};
}

[[nodiscard]] ConstEvalResult ok(HirLiteralValue v) {
    return ConstEvalResult{.value{std::move(v)}, .failure = ConstEvalFailure::None, .blamedNode{}};
}

// Build a Bool-cored literal carrying `n != 0` per C99 truthiness. The
// engine's normalization rule (documented in `hir_literal_pool.hpp`) is
// that `core == Bool` implies the int64 arm holding 0 or 1; this helper
// is the single source of truth for that rule across Cast-to-Bool,
// LogicalAnd/Or short-circuit, and LogicalAnd/Or combine paths.
[[nodiscard]] HirLiteralValue makeBoolLiteral(std::int64_t n) {
    HirLiteralValue v;
    v.core  = TypeKind::Bool;
    v.value = std::int64_t{(n != 0) ? 1 : 0};
    return v;
}

// Pull an integer-typed `HirLiteralValue` into a common `int64_t` arithmetic
// representation, regardless of which of the three numeric variant arms it
// arrived on (`int64_t` / `uint64_t` / `bool`). The unsigned arm is the
// natural arrival for `unsigned int`, `char`, and any source-decoded
// unsigned literal (per `cst_to_hir.cpp`); the bool arm arrives from
// round-tripped `.dsshir` text. Without this bridge, the engine silently
// rejects every unsigned/bool literal as "not an integer" — a real
// production gap (a `char c = 'A';` global would never fold).
//
// Returns nullopt only when:
//   - the value is genuinely non-numeric (`monostate` / `string` / `double`),
//   - or an unsigned value exceeds `int64_t`'s positive range (would
//     misrepresent as negative on signed-arithmetic paths).
// Float values are explicitly NOT pulled here — float folding is CE5.
[[nodiscard]] std::optional<std::int64_t>
asInt64(HirLiteralValue const& v) noexcept {
    if (auto p = std::get_if<std::int64_t>(&v.value)) return *p;
    if (auto p = std::get_if<std::uint64_t>(&v.value)) {
        if (*p > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;   // out-of-int64-range; caller refuses with Overflow
        }
        return static_cast<std::int64_t>(*p);
    }
    if (auto p = std::get_if<bool>(&v.value)) return *p ? std::int64_t{1} : std::int64_t{0};
    return std::nullopt;
}

// True iff `v` is held in the `double` variant arm — used as the engine's
// "is this operand a float?" predicate. Distinct from the operand's `core`
// (which is metadata; the variant arm is the authority per
// `hir_literal_pool.hpp`'s contract).
[[nodiscard]] bool isFloatValue(HirLiteralValue const& v) noexcept {
    return std::holds_alternative<double>(v.value);
}

// Pull a numeric `HirLiteralValue` into `double` for IEEE 754 arithmetic.
// Mirror of `asInt64`: handles all four numeric variant arms (int64,
// uint64, bool, double). The double promotion uses the host platform's
// `<cmath>` rounding (which is C99's "round to nearest, ties to even"
// default — the only sensible default; runtime-matched).
//
// Returns nullopt only for the genuinely non-numeric arms (`monostate` /
// `string`). Bool is always 0.0 / 1.0. Large uint64 / int64 values may
// lose precision when widening to double — acceptable per IEEE 754 (the
// same loss happens at runtime); a future cycle may add a strict-mode
// `refuseOnLossyFloatConversion` knob for verifier consumers, deferred
// in plan 12.5 closure.
[[nodiscard]] std::optional<double>
asDouble(HirLiteralValue const& v) noexcept {
    if (auto p = std::get_if<double>(&v.value)) return *p;
    if (auto p = std::get_if<std::int64_t>(&v.value)) {
        return static_cast<double>(*p);
    }
    if (auto p = std::get_if<std::uint64_t>(&v.value)) {
        return static_cast<double>(*p);
    }
    if (auto p = std::get_if<bool>(&v.value)) return *p ? 1.0 : 0.0;
    return std::nullopt;
}

// C99 truthiness: `0` (int) and `±0.0` (float) are false; everything else
// (including NaN and ±inf) is true. Returns nullopt for non-numeric arms.
// Floats are accepted only when the caller has the `allowFloat` knob on
// (signalled by passing `true`); otherwise float-arm operands refuse to
// preserve the engine's "integer-only without allowFloat" contract.
[[nodiscard]] std::optional<bool>
asBool(HirLiteralValue const& v, bool allowFloat) noexcept {
    if (isFloatValue(v)) {
        if (!allowFloat) return std::nullopt;
        return *asDouble(v) != 0.0;
    }
    auto iv = asInt64(v);
    if (!iv.has_value()) return std::nullopt;
    return *iv != 0;
}

// CE5: fold a HirKind::UnaryOp(Neg) on a float operand. BitNot/Not on
// float is C99-undefined (bitwise / logical-not bitwise interpretation
// only apply to integers); the helper signals this via `outFailure =
// UnsupportedTypeKind` so the caller distinguishes "engine doesn't model
// op" from "this op is wrong on this type" — matches `applyBinaryInt`'s
// out-param pattern. IEEE 754 negation flips the sign bit — equivalent
// to `-x` in host C++ and well-defined for every double including
// NaN/±inf/-0 (negated NaN is still NaN; -0.0 yields +0.0 when negated
// again, matching IEEE).
[[nodiscard]] std::optional<HirLiteralValue>
applyUnaryFloat(HirOpKind op, HirLiteralValue const& inner,
                ConstEvalFailure& outFailure) {
    auto dv = asDouble(inner);
    if (!dv.has_value()) return std::nullopt;
    HirLiteralValue folded = inner;
    switch (op) {
        case HirOpKind::Neg: folded.value = -(*dv); return folded;
        default:
            // BitNot / Not on float: operator IS modelled (applyUnaryInt
            // handles it for ints) — the TYPE is wrong per C99. Surface
            // `UnsupportedTypeKind` so verifier diagnostics keyed on
            // "op not yet folded" don't fire on user-program type errors.
            outFailure = ConstEvalFailure::UnsupportedTypeKind;
            return std::nullopt;
    }
}

// Fold a HirKind::UnaryOp(Neg/BitNot) of an already-folded integer operand.
// `inner` is the operand's value (known to be an integer literal here).
[[nodiscard]] std::optional<HirLiteralValue>
applyUnaryInt(HirOpKind op, HirLiteralValue const& inner) {
    auto iv64 = asInt64(inner);
    if (!iv64.has_value()) return std::nullopt;
    std::int64_t const* iv = &*iv64;
    HirLiteralValue folded = inner;
    switch (op) {
        case HirOpKind::Neg:    folded.value = -(*iv);    return folded;
        case HirOpKind::BitNot: folded.value = ~(*iv);    return folded;
        // Unary `+` is identity at the value level (none of v1's frontends
        // emit it as a HirKind::UnaryOp today, but it would fold trivially).
        default: return std::nullopt;
    }
}

// Fold a HirKind::BinaryOp over two integer operands per the EvalOptions
// policy. Returns nullopt for non-foldable cases (caller maps to the
// appropriate `ConstEvalFailure`).
[[nodiscard]] std::optional<HirLiteralValue>
applyBinaryInt(HirOpKind op, HirLiteralValue const& a, HirLiteralValue const& b,
               EvalOptions const& opts, ConstEvalFailure& outFailure) {
    auto av64 = asInt64(a);
    auto bv64 = asInt64(b);
    if (!av64.has_value() || !bv64.has_value()) return std::nullopt;
    std::int64_t const* av = &*av64;
    std::int64_t const* bv = &*bv64;
    HirLiteralValue folded = a;   // inherit core/type from lhs side
    switch (op) {
        case HirOpKind::Add:    folded.value = *av + *bv;  return folded;
        case HirOpKind::Sub:    folded.value = *av - *bv;  return folded;
        case HirOpKind::Mul:    folded.value = *av * *bv;  return folded;
        case HirOpKind::Div:
            if (*bv == 0) {
                // Always refuse to fold a div-by-zero (UB-by-spec; the engine
                // doesn't invent a value). The `refuseOnDivByZero` knob only
                // controls WHICH failure code surfaces: ON → the dedicated
                // `DivisionByZero` (callers route to a user-facing diagnostic);
                // OFF → `NotAConstantExpression` (caller treats it as just
                // another reason the expression isn't a compile-time constant
                // — e.g. MIR-globals falls back to the runtime-init path).
                outFailure = opts.refuseOnDivByZero
                    ? ConstEvalFailure::DivisionByZero
                    : ConstEvalFailure::NotAConstantExpression;
                return std::nullopt;
            }
            folded.value = *av / *bv; return folded;
        case HirOpKind::Rem:
            if (*bv == 0) {
                outFailure = opts.refuseOnDivByZero
                    ? ConstEvalFailure::DivisionByZero
                    : ConstEvalFailure::NotAConstantExpression;
                return std::nullopt;
            }
            folded.value = *av % *bv; return folded;
        case HirOpKind::BitAnd: folded.value = *av & *bv;  return folded;
        case HirOpKind::BitOr:  folded.value = *av | *bv;  return folded;
        case HirOpKind::BitXor: folded.value = *av ^ *bv;  return folded;
        case HirOpKind::Shl: {
            if (*bv < 0 || *bv >= 64) {
                outFailure = opts.refuseOnShiftOutOfRange
                    ? ConstEvalFailure::ShiftCountOutOfRange
                    : ConstEvalFailure::NotAConstantExpression;
                return std::nullopt;
            }
            folded.value = static_cast<std::int64_t>(
                static_cast<std::uint64_t>(*av) << *bv);
            return folded;
        }
        case HirOpKind::Shr: {
            if (*bv < 0 || *bv >= 64) {
                outFailure = opts.refuseOnShiftOutOfRange
                    ? ConstEvalFailure::ShiftCountOutOfRange
                    : ConstEvalFailure::NotAConstantExpression;
                return std::nullopt;
            }
            folded.value = *av >> *bv;  // arith shift on signed
            return folded;
        }
        case HirOpKind::Eq: folded.value = std::int64_t{*av == *bv}; return folded;
        case HirOpKind::Ne: folded.value = std::int64_t{*av != *bv}; return folded;
        case HirOpKind::Lt: folded.value = std::int64_t{*av <  *bv}; return folded;
        case HirOpKind::Le: folded.value = std::int64_t{*av <= *bv}; return folded;
        case HirOpKind::Gt: folded.value = std::int64_t{*av >  *bv}; return folded;
        case HirOpKind::Ge: folded.value = std::int64_t{*av >= *bv}; return folded;
        default: return std::nullopt;
    }
}

// CE5: fold a HirKind::BinaryOp over two operands when at least one is a
// float (after promotion). Result is always `double` for arithmetic, or
// `int64{0|1}` for comparisons (matches `applyBinaryInt`'s convention).
// IEEE 754 semantics come from the host platform's `<cmath>`:
//   - NaN propagates: any operation involving NaN produces NaN.
//   - Comparisons against NaN return false (unordered), including `==`.
//   - Division by ±0 produces ±infinity (no DivisionByZero failure).
//   - Overflow produces ±infinity (saturation, not wrap).
// `refuseOnDivByZero` is a no-op here because IEEE division by zero is
// well-defined; integer-style refusal would diverge from runtime.
[[nodiscard]] std::optional<HirLiteralValue>
applyBinaryFloat(HirOpKind op, HirLiteralValue const& a, HirLiteralValue const& b,
                 ConstEvalFailure& outFailure) {
    auto adv = asDouble(a);
    auto bdv = asDouble(b);
    if (!adv.has_value() || !bdv.has_value()) return std::nullopt;
    double const av = *adv;
    double const bv = *bdv;
    HirLiteralValue folded;
    folded.core = TypeKind::F64;   // result core; caller's BinaryOp tag may override
    switch (op) {
        case HirOpKind::Add: folded.value = av + bv; return folded;
        case HirOpKind::Sub: folded.value = av - bv; return folded;
        case HirOpKind::Mul: folded.value = av * bv; return folded;
        case HirOpKind::Div: folded.value = av / bv; return folded;   // ±inf / NaN per IEEE
        case HirOpKind::Eq: folded.value = std::int64_t{av == bv}; folded.core = TypeKind::Bool; return folded;
        case HirOpKind::Ne: folded.value = std::int64_t{av != bv}; folded.core = TypeKind::Bool; return folded;
        case HirOpKind::Lt: folded.value = std::int64_t{av <  bv}; folded.core = TypeKind::Bool; return folded;
        case HirOpKind::Le: folded.value = std::int64_t{av <= bv}; folded.core = TypeKind::Bool; return folded;
        case HirOpKind::Gt: folded.value = std::int64_t{av >  bv}; folded.core = TypeKind::Bool; return folded;
        case HirOpKind::Ge: folded.value = std::int64_t{av >= bv}; folded.core = TypeKind::Bool; return folded;
        default:
            // Rem / BitAnd / BitOr / BitXor / Shl / Shr are C99-undefined
            // on float — the operator IS modelled (applyBinaryInt handles
            // it) but the operand type is wrong. Surface
            // `UnsupportedTypeKind`, same pattern as applyUnaryFloat.
            outFailure = ConstEvalFailure::UnsupportedTypeKind;
            return std::nullopt;
    }
}

// Float target descriptor: width in bits. Used by Cast to choose the
// right host-precision narrowing AND to refuse precise-loss targets the
// engine cannot represent (F16 / F128: no host type; values would
// silently stay as `double`, breaking `HirLiteralValue::core` ↔ variant-
// arm contract). F32 narrows via `static_cast<float>` round-trip;
// F64 is identity. F16 / F128 refuse with `UnsupportedTypeKind` until
// a host soft-float helper lands.
[[nodiscard]] bool isFloatKind(TypeKind k) noexcept {
    return k == TypeKind::F16 || k == TypeKind::F32
        || k == TypeKind::F64 || k == TypeKind::F128;
}

struct FloatKindInfo {
    int bits;        // 16 / 32 / 64 / 128
    bool hostBacked; // true iff `double` storage carries an exact value
                     // for this width — F32 + F64 are host-backed (with
                     // F32 needing a narrowing round-trip); F16 + F128
                     // are NOT (no host type, soft-float deferred).
};
[[nodiscard]] std::optional<FloatKindInfo> floatKindInfo(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::F16:  return FloatKindInfo{16,  false};
        case TypeKind::F32:  return FloatKindInfo{32,  true};
        case TypeKind::F64:  return FloatKindInfo{64,  true};
        case TypeKind::F128: return FloatKindInfo{128, false};
        default: return std::nullopt;
    }
}

// Integer target descriptor: width in bits + signedness. `Bool` is a
// special case (1-bit unsigned; truthiness rules apply). `Char` is a
// 32-bit unsigned Unicode codepoint. `Byte` is 8-bit unsigned.
// `I128`/`U128` are listed but values that overflow 64-bit storage are
// not representable in the engine's `int64_t` arm — those folds refuse.
struct IntKindInfo {
    int  bits;
    bool isSigned;
};
[[nodiscard]] std::optional<IntKindInfo> intKindInfo(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::Bool: return IntKindInfo{1,   false};  // truthiness
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

// Range check: does `v` (held as int64) fit in the target's representable
// range? `bits` is the target width; `isSigned` is the target's signedness.
// 64-bit signed targets always accept; 64-bit unsigned accepts non-negative.
// Wider-than-64 targets always accept (any int64 fits). For width < 64,
// computes the inclusive [lo, hi] range and tests.
[[nodiscard]] bool valueFitsInIntTarget(std::int64_t v, IntKindInfo target) noexcept {
    if (target.bits >= 64) {
        if (!target.isSigned) return v >= 0;   // u64+: must be non-negative
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

// Modular truncation: mask `v` to the target's `bits` width, then sign-
// extend if the target is signed. C99 unsigned-integer conversion is
// well-defined modular wrap; the signed-overflow case is UB but every
// real compiler emits the bit-equivalent wrap — match that here so the
// fold result is what runtime would observe.
[[nodiscard]] std::int64_t wrapToIntTarget(std::int64_t v, IntKindInfo target) noexcept {
    if (target.bits >= 64) return v;   // no narrowing
    std::uint64_t const mask = (std::uint64_t{1} << target.bits) - 1;
    std::uint64_t       masked = static_cast<std::uint64_t>(v) & mask;
    if (target.isSigned) {
        std::uint64_t const signBit = std::uint64_t{1} << (target.bits - 1);
        if ((masked & signBit) != 0) masked |= ~mask;   // sign-extend
    }
    return static_cast<std::int64_t>(masked);
}

// Internal recursive impl. `visitedSyms` carries the per-call cycle-
// detection set (CE2): any `Ref(sym)` resolution checks containment
// before descending into the symbol's defining expression. The public
// `evaluateConstant` seeds an empty set.
[[nodiscard]] ConstEvalResult
evalImpl(Hir const& hir, TypeInterner& interner, HirLiteralPool const& literals,
         HirNodeId expr, EvalOptions const& options,
         std::unordered_set<std::uint32_t>& visitedSyms) {
    if (!expr.valid()) return fail(ConstEvalFailure::NotAConstantExpression, expr);
    HirKind const k = hir.kind(expr);
    if (k == HirKind::Literal) {
        std::uint32_t const idx = hir.payload(expr);
        return ok(literals.at(idx));
    }
    if (k == HirKind::Ref) {
        // CE2: resolve a Ref to a constant-bound symbol via the caller's
        // resolver callback. Absent callback (CE1's behaviour) or absent
        // mapping → NotAConstantExpression. Cycle detection prevents
        // infinite recursion on `int a = b; int b = a;`-shape inputs.
        if (!options.resolveConstSymbol) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        std::uint32_t const sym = hir.payload(expr);
        if (visitedSyms.contains(sym)) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        auto definingExpr = options.resolveConstSymbol(SymbolId{sym});
        if (!definingExpr.has_value() || !definingExpr->valid()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        visitedSyms.insert(sym);
        ConstEvalResult inner = evalImpl(hir, interner, literals,
                                         *definingExpr, options, visitedSyms);
        visitedSyms.erase(sym);
        // On failure, re-blame at the Ref USE site rather than wherever
        // the definition tree's failure surfaced. The caller (a
        // diagnostic emitter) has the use-site span available; the
        // definition-tree's node may live in an entirely different
        // module decl and carry no helpful context. On success, blame
        // stays default (no anchor needed).
        if (!inner.value.has_value()) inner.blamedNode = expr;
        return inner;
    }
    if (k == HirKind::UnaryOp) {
        std::uint32_t const payload = hir.payload(expr);
        if (!isCoreOp(payload)) return fail(ConstEvalFailure::UnsupportedOperator, expr);
        HirOpKind const op = decodeCoreOp(payload);
        auto kids = hir.children(expr);
        if (kids.size() != 1) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        ConstEvalResult inner = evalImpl(hir, interner, literals, kids[0], options, visitedSyms);
        if (!inner.value.has_value()) return inner;
        // Float operand routes through the float path (CE5) only when
        // `allowFloat` is opted in by the caller. Without the knob, a
        // float-typed UnaryOp refuses with `UnsupportedTypeKind` —
        // consistent with the engine's "integer-only until CE5" gate.
        if (isFloatValue(*inner.value)) {
            if (!options.allowFloat) {
                return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            }
            ConstEvalFailure why = ConstEvalFailure::None;
            if (auto folded = applyUnaryFloat(op, *inner.value, why);
                folded.has_value()) {
                return ok(std::move(*folded));
            }
            // `why` is set by applyUnaryFloat to `UnsupportedTypeKind`
            // for C99-undefined op+float combinations (BitNot/Not); we
            // reserve `UnsupportedOperator` for genuine "engine doesn't
            // model this op yet".
            return fail(why != ConstEvalFailure::None
                            ? why
                            : ConstEvalFailure::UnsupportedOperator,
                        expr);
        }
        // Distinguish "value isn't an integer" from "operator isn't modelled"
        // at the caller so failure codes match the other int-only fold sites
        // (LogicalAnd/Or/Ternary cond) which surface `UnsupportedTypeKind`.
        if (!asInt64(*inner.value).has_value()) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        if (auto folded = applyUnaryInt(op, *inner.value); folded.has_value()) {
            return ok(std::move(*folded));
        }
        return fail(ConstEvalFailure::UnsupportedOperator, expr);
    }
    if (k == HirKind::BinaryOp) {
        std::uint32_t const payload = hir.payload(expr);
        if (!isCoreOp(payload)) return fail(ConstEvalFailure::UnsupportedOperator, expr);
        HirOpKind const op = decodeCoreOp(payload);
        auto kids = hir.children(expr);
        if (kids.size() != 2) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        ConstEvalResult a = evalImpl(hir, interner, literals, kids[0], options, visitedSyms);
        if (!a.value.has_value()) return a;
        ConstEvalResult b = evalImpl(hir, interner, literals, kids[1], options, visitedSyms);
        if (!b.value.has_value()) return b;
        // CE5: float promotion. Per C99 UAC, if either operand is float
        // the other promotes to float and the op runs in IEEE 754. Without
        // the `allowFloat` knob, refuse with `UnsupportedTypeKind` — the
        // engine's "integer-only by default" contract holds.
        bool const eitherFloat = isFloatValue(*a.value) || isFloatValue(*b.value);
        if (eitherFloat) {
            if (!options.allowFloat) {
                return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            }
            ConstEvalFailure why = ConstEvalFailure::None;
            if (auto folded = applyBinaryFloat(op, *a.value, *b.value, why);
                folded.has_value()) {
                // Result core: comparisons → Bool (set by applyBinaryFloat);
                // arithmetic → the C99-UAC common type from the HIR node's
                // typeId so wider-of-two-floats wins (commonType on float
                // pairs promotes to the wider). Re-tag with the same
                // discipline as the int path.
                if (!isComparison(op)) {
                    TypeId const aTy = hir.typeId(kids[0]);
                    TypeId const bTy = hir.typeId(kids[1]);
                    if (TypeId const common = interner.commonType(aTy, bTy);
                        common.valid()) {
                        folded->core = interner.kind(common);
                    }
                }
                return ok(std::move(*folded));
            }
            // `why` distinguishes "op valid, type wrong" (e.g. `1.5 % 2.0` —
            // applyBinaryFloat sets UnsupportedTypeKind) from "op not yet
            // modelled by the engine" (UnsupportedOperator).
            return fail(why != ConstEvalFailure::None
                            ? why
                            : ConstEvalFailure::UnsupportedOperator,
                        expr);
        }
        // Same distinction as UnaryOp: a non-integer operand surfaces
        // `UnsupportedTypeKind` consistent with LogicalAnd/Or/Ternary;
        // applyBinaryInt's nullopt is reserved for "operator not yet
        // modelled" (UnsupportedOperator) and policy refusals.
        if (!asInt64(*a.value).has_value() || !asInt64(*b.value).has_value()) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        ConstEvalFailure why = ConstEvalFailure::None;
        if (auto folded = applyBinaryInt(op, *a.value, *b.value, options, why);
            folded.has_value()) {
            // CE3: tag the folded value's core per C99 binary-op semantics.
            // Three cases:
            //   - Comparison ops (Eq/Ne/Lt/Le/Gt/Ge): result is Bool
            //     regardless of operand types (force-override; applyBinaryInt
            //     inherited LHS's core which is wrong for the cmp case).
            //   - Shift ops (Shl/Shr) (C99 §6.5.7p3): result type is the
            //     PROMOTED LEFT operand only; the shift count's type does
            //     not contribute. `commonType(lhs, lhs)` realizes the
            //     left-operand integer promotion uniformly.
            //   - All other arithmetic / bitwise: result is the C99-UAC
            //     common type of both operands.
            // Type source is the HIR node's typeId (the authoritative
            // record), not the literal's `core` mirror — folded recursion
            // results may have re-tagged cores that diverge from the
            // declared type tree.
            TypeId const aTy = hir.typeId(kids[0]);
            TypeId const bTy = hir.typeId(kids[1]);
            bool const isShift = (op == HirOpKind::Shl || op == HirOpKind::Shr);
            if (isComparison(op)) {
                folded->core = TypeKind::Bool;
            } else if (isShift) {
                TypeId const promoted = interner.commonType(aTy, aTy);
                if (promoted.valid()) folded->core = interner.kind(promoted);
            } else if (TypeId const common = interner.commonType(aTy, bTy);
                       common.valid()) {
                folded->core = interner.kind(common);
            }
            // Else (commonType InvalidType on a non-arithmetic operand
            // pair that nonetheless folded via int64 arithmetic — an
            // unlikely-but-possible substrate inconsistency): leave the
            // LHS-inherited core in place. applyBinaryInt's success
            // requires both arms to pull through `asInt64`, so this
            // path indicates the types disagree with the values; a
            // downstream verifier will catch the cross-tier mismatch.
            return ok(std::move(*folded));
        }
        if (why != ConstEvalFailure::None) return fail(why, expr);
        return fail(ConstEvalFailure::UnsupportedOperator, expr);
    }
    if (k == HirKind::Cast) {
        // Target-type-aware cast. Four quadrants by (source kind, target
        // kind):
        //   int → int : truncate/extend per width + sign; refuseOnOverflow
        //               policy applies (CE3).
        //   int → bool: `N != 0` truthiness (CE3 special case).
        //   int → float (CE5, allowFloat=true): host conversion; precision
        //               loss for very large ints is documented IEEE 754
        //               behavior matching runtime.
        //   float → int (CE5, allowFloat=true): C99 §6.3.1.4 truncation
        //               toward zero. Refused (Overflow) when the truncated
        //               value doesn't fit the integer target AND
        //               refuseOnOverflow=true. NaN/inf always refuse with
        //               Overflow (truncation is undefined; runtime wrap
        //               is not portable).
        //   float → bool: nonzero (incl. NaN) → true per C semantics.
        //   float → float (CE5): host conversion; rounding per IEEE 754
        //               round-to-nearest-even (host default).
        // Pointer / aggregate targets remain non-foldable.
        auto kids = hir.children(expr);
        if (kids.size() != 1) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        ConstEvalResult inner = evalImpl(hir, interner, literals, kids[0], options, visitedSyms);
        if (!inner.value.has_value()) return inner;
        TypeId const targetTy = hir.typeId(expr);
        if (!targetTy.valid()) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        TypeKind const toK = interner.kind(targetTy);
        bool const targetFloat = isFloatKind(toK);
        bool const sourceFloat = isFloatValue(*inner.value);

        // Float-involving casts require the allowFloat knob.
        if ((sourceFloat || targetFloat) && !options.allowFloat) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }

        // Float → Bool: nonzero (incl. NaN/inf) → true; ±0 → false.
        if (sourceFloat && toK == TypeKind::Bool) {
            double const dv = *asDouble(*inner.value);
            return ok(makeBoolLiteral(dv != 0.0 ? 1 : 0));
        }

        // Float → Float: convert via host. F32 needs an actual
        // narrowing round-trip (otherwise the stored `double` would
        // diverge from the IEEE-754 single-precision value the runtime
        // produces); F64 is identity; F16 / F128 have no host backing
        // and refuse — storing a `double` under `core = F16` would
        // violate the `HirLiteralValue::core` ↔ variant-arm contract
        // (the value-bits wouldn't be the actual half/quad). Lifting
        // F16 / F128 requires a soft-float helper, deferred in plan
        // 12.5 closure (no consumer today emits these literals).
        if (sourceFloat && targetFloat) {
            auto info = floatKindInfo(toK);
            if (!info.has_value() || !info->hostBacked) {
                return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            }
            double const dv = *asDouble(*inner.value);
            HirLiteralValue folded;
            folded.core  = toK;
            // F32 narrows via host `float` round-trip — this performs
            // the IEEE 754 round-to-nearest-even rounding the runtime
            // does. F64 is identity (already `double`).
            folded.value = (info->bits == 32)
                ? static_cast<double>(static_cast<float>(dv))
                : dv;
            return ok(std::move(folded));
        }

        // Int → Float: host conversion (precision-loss for huge ints is
        // IEEE 754-defined behaviour; runtime path produces the same
        // bits). F16 / F128 refuse for the same reason as float→float.
        if (!sourceFloat && targetFloat) {
            auto info = floatKindInfo(toK);
            if (!info.has_value() || !info->hostBacked) {
                return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            }
            auto iv64 = asInt64(*inner.value);
            if (!iv64.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            double const widened = static_cast<double>(*iv64);
            HirLiteralValue folded;
            folded.core  = toK;
            folded.value = (info->bits == 32)
                ? static_cast<double>(static_cast<float>(widened))
                : widened;
            return ok(std::move(folded));
        }

        // Float → Int: truncate toward zero (C99 §6.3.1.4); refuse when
        // the truncated value doesn't fit the integer target. NaN/inf
        // always refuse with Overflow — truncating them is undefined per
        // the standard and the bit pattern isn't portable.
        if (sourceFloat) {
            auto target = intKindInfo(toK);
            if (!target.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            double const dv = *asDouble(*inner.value);
            // NaN / inf: refuse with Overflow unless we can constructively
            // claim a value. We cannot; refuse regardless of the knob.
            if (std::isnan(dv) || std::isinf(dv)) {
                return fail(ConstEvalFailure::Overflow, expr);
            }
            // Truncate toward zero (host `floor`/`ceil` are exact on
            // double — no precision loss to range-check).
            double const truncated = (dv >= 0.0) ? std::floor(dv) : std::ceil(dv);
            // Range check against int64 storage. Naive bounds with
            // `static_cast<double>(INT64_MAX)` are unsafe: 2^63 - 1 is
            // not representable as a double, so the conversion ROUNDS UP
            // to exactly 2^63, and `truncated > 2^63` then misses the
            // `truncated == 2^63` case — UB on the subsequent
            // `static_cast<int64_t>(truncated)`. Use 2^63 as the exclusive
            // upper bound directly (representable exactly). The lower
            // bound -2^63 IS representable exactly, so `< -2^63` is safe.
            constexpr double kInt64MaxExclusive = 9223372036854775808.0;   // 2^63 (exact)
            constexpr double kInt64Min          = -9223372036854775808.0;  // -2^63 (exact)
            if (truncated >= kInt64MaxExclusive || truncated < kInt64Min) {
                // Out-of-int64 floats: C99 §6.3.1.4 leaves the bit-pattern
                // implementation-defined, and the engine has no portable
                // wrap to emulate. Refuse UNIFORMLY regardless of the
                // `refuseOnOverflow` knob — same policy as NaN/inf above.
                // The wrap-knob only governs the inner-range "fits int64
                // but not target" case below.
                return fail(ConstEvalFailure::Overflow, expr);
            }
            std::int64_t const iv = static_cast<std::int64_t>(truncated);
            if (toK == TypeKind::Bool) {
                return ok(makeBoolLiteral(iv));
            }
            HirLiteralValue folded;
            folded.core = toK;
            if (target->bits >= 64 && !target->isSigned && iv < 0) {
                return fail(ConstEvalFailure::Overflow, expr);
            }
            if (valueFitsInIntTarget(iv, *target)) {
                folded.value = iv;
                return ok(std::move(folded));
            }
            if (options.refuseOnOverflow) {
                return fail(ConstEvalFailure::Overflow, expr);
            }
            folded.value = wrapToIntTarget(iv, *target);
            return ok(std::move(folded));
        }

        // Int → Int (and int → bool) — CE3's existing path.
        auto target = intKindInfo(toK);
        if (!target.has_value()) {
            // Non-integer, non-float target (pointer / aggregate).
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        auto iv64 = asInt64(*inner.value);
        if (!iv64.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        std::int64_t const iv = *iv64;
        if (toK == TypeKind::Bool) {
            return ok(makeBoolLiteral(iv));
        }
        HirLiteralValue folded;
        folded.core = toK;
        // Cast to unsigned target ≥64 bits with a negative source value:
        // the int64 storage arm cannot reconcile signedness with downstream
        // signed-arithmetic paths (`applyBinaryInt` reads via int64), so the
        // engine refuses regardless of the `refuseOnOverflow` knob. Callers
        // route through the runtime path which handles the bit-pattern wrap
        // correctly. (When CE5 opens the uint64 arm for arithmetic, this
        // restriction can lift.)
        if (target->bits >= 64 && !target->isSigned && iv < 0) {
            return fail(ConstEvalFailure::Overflow, expr);
        }
        if (valueFitsInIntTarget(iv, *target)) {
            folded.value = iv;
            return ok(std::move(folded));
        }
        // Value overflows the target. Knob-controlled policy: refuse with
        // `Overflow` (D5.5 enum-bounds verifier path) OR wrap modularly
        // (MIR-globals / runtime-matched path).
        if (options.refuseOnOverflow) {
            return fail(ConstEvalFailure::Overflow, expr);
        }
        folded.value = wrapToIntTarget(iv, *target);
        return ok(std::move(folded));
    }
    if (k == HirKind::LogicalAnd || k == HirKind::LogicalOr) {
        // C99 short-circuit semantics: evaluate `a` first. If `a` already
        // determines the result (`0 && unfoldable` is unambiguously
        // false; `1 || unfoldable` is unambiguously true), the engine
        // MUST NOT recurse into `b` — otherwise a non-foldable `b`
        // would spuriously fail the whole fold. Once `a` doesn't short-
        // circuit, the result depends on `b`; recurse into `b` and
        // combine. Result core is always Bool.
        auto kids = hir.children(expr);
        if (kids.size() != 2) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        ConstEvalResult a = evalImpl(hir, interner, literals, kids[0], options, visitedSyms);
        if (!a.value.has_value()) return a;
        // `asBool` handles both integer and float operands (the latter
        // only when `allowFloat` is on); NaN / ±inf evaluate to true per
        // C semantics; ±0.0 evaluates to false.
        auto aIsTrueOpt = asBool(*a.value, options.allowFloat);
        if (!aIsTrueOpt.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        bool const aIsTrue = *aIsTrueOpt;
        bool const isAnd   = (k == HirKind::LogicalAnd);
        // && short-circuits when `a` is false; || short-circuits when `a`
        // is true. Either way the determined value IS `aIsTrue`.
        bool const shortCircuits = isAnd ? !aIsTrue : aIsTrue;
        if (shortCircuits) {
            return ok(makeBoolLiteral(aIsTrue ? 1 : 0));
        }
        // No short-circuit; need `b` to determine the result.
        ConstEvalResult b = evalImpl(hir, interner, literals, kids[1], options, visitedSyms);
        if (!b.value.has_value()) return b;
        auto bIsTrueOpt = asBool(*b.value, options.allowFloat);
        if (!bIsTrueOpt.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        return ok(makeBoolLiteral(*bIsTrueOpt ? 1 : 0));
    }
    if (k == HirKind::Ternary) {
        // children: [cond, then, else]. Fold cond first; recurse into ONLY
        // the selected arm. The unselected arm may be non-constant — a
        // legitimate compile-time-known choice between a constant and a
        // computation (`cond ? known : maybe_runtime`) should still fold
        // when cond and the chosen arm are both constants. The selected
        // arm's failure propagates verbatim (we return the inner result;
        // `blamedNode` retains the arm's anchor).
        //
        // Result core: the SELECTED arm's `core` may be narrower than
        // the Ternary's declared `typeId` (e.g. `cond ? (int8)5 : 1000`
        // where the Ternary type is I32). Re-tag the folded core from
        // the Ternary node's typeId so `core` mirrors the authoritative
        // type record (per the `hir_literal_pool.hpp` contract). Same
        // discipline as BinaryOp's `commonType` retag above.
        auto kids = hir.children(expr);
        if (kids.size() != 3) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        ConstEvalResult cond = evalImpl(hir, interner, literals, kids[0], options, visitedSyms);
        if (!cond.value.has_value()) return cond;
        // Cond truthiness via the shared `asBool` (CE5): accepts float
        // operands when `allowFloat` is on, applying the same NaN/inf →
        // true semantics as LogicalAnd/Or.
        auto condIsTrueOpt = asBool(*cond.value, options.allowFloat);
        if (!condIsTrueOpt.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        HirNodeId const selected = *condIsTrueOpt ? kids[1] : kids[2];
        ConstEvalResult inner = evalImpl(hir, interner, literals,
                                         selected, options, visitedSyms);
        if (inner.value.has_value()) {
            TypeId const ternTy = hir.typeId(expr);
            if (ternTy.valid()) inner.value->core = interner.kind(ternTy);
        }
        return inner;
    }
    return fail(ConstEvalFailure::NotAConstantExpression, expr);
}

} // namespace

ConstEvalResult evaluateConstant(Hir const& hir,
                                 TypeInterner& interner,
                                 HirLiteralPool const& literals,
                                 HirNodeId expr,
                                 EvalOptions options) {
    std::unordered_set<std::uint32_t> visitedSyms;
    return evalImpl(hir, interner, literals, expr, options, visitedSyms);
}

} // namespace dss
