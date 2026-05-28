#include "hir/const_eval.hpp"

#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/hir_op.hpp"

namespace dss {

namespace {

[[nodiscard]] ConstEvalResult fail(ConstEvalFailure why, HirNodeId blamed) {
    return ConstEvalResult{.value{}, .failure = why, .blamedNode = blamed};
}

[[nodiscard]] ConstEvalResult ok(HirLiteralValue v) {
    return ConstEvalResult{.value{std::move(v)}, .failure = ConstEvalFailure::None, .blamedNode{}};
}

// Fold a HirKind::UnaryOp(Neg/BitNot) of an already-folded integer operand.
// `inner` is the operand's value (known to be an integer literal here).
[[nodiscard]] std::optional<HirLiteralValue>
applyUnaryInt(HirOpKind op, HirLiteralValue const& inner) {
    std::int64_t const* iv = std::get_if<std::int64_t>(&inner.value);
    if (iv == nullptr) return std::nullopt;
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
    std::int64_t const* av = std::get_if<std::int64_t>(&a.value);
    std::int64_t const* bv = std::get_if<std::int64_t>(&b.value);
    if (av == nullptr || bv == nullptr) return std::nullopt;
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

[[nodiscard]] bool isIntegerKind(TypeKind k) noexcept {
    return k == TypeKind::Bool || k == TypeKind::Char || k == TypeKind::Byte
        || k == TypeKind::I8   || k == TypeKind::I16  || k == TypeKind::I32
        || k == TypeKind::I64  || k == TypeKind::I128
        || k == TypeKind::U8   || k == TypeKind::U16  || k == TypeKind::U32
        || k == TypeKind::U64  || k == TypeKind::U128;
}

} // namespace

ConstEvalResult evaluateConstant(Hir const& hir,
                                 TypeInterner& interner,
                                 HirLiteralPool const& literals,
                                 HirNodeId expr,
                                 EvalOptions options) {
    if (!expr.valid()) return fail(ConstEvalFailure::NotAConstantExpression, expr);
    HirKind const k = hir.kind(expr);
    if (k == HirKind::Literal) {
        std::uint32_t const idx = hir.payload(expr);
        return ok(literals.at(idx));
    }
    if (k == HirKind::UnaryOp) {
        std::uint32_t const payload = hir.payload(expr);
        if (!isCoreOp(payload)) return fail(ConstEvalFailure::UnsupportedOperator, expr);
        HirOpKind const op = decodeCoreOp(payload);
        auto kids = hir.children(expr);
        if (kids.size() != 1) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        ConstEvalResult inner = evaluateConstant(hir, interner, literals, kids[0], options);
        if (!inner.value.has_value()) return inner;
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
        ConstEvalResult a = evaluateConstant(hir, interner, literals, kids[0], options);
        if (!a.value.has_value()) return a;
        ConstEvalResult b = evaluateConstant(hir, interner, literals, kids[1], options);
        if (!b.value.has_value()) return b;
        ConstEvalFailure why = ConstEvalFailure::None;
        if (auto folded = applyBinaryInt(op, *a.value, *b.value, options, why);
            folded.has_value()) {
            return ok(std::move(*folded));
        }
        if (why != ConstEvalFailure::None) return fail(why, expr);
        return fail(ConstEvalFailure::UnsupportedOperator, expr);
    }
    if (k == HirKind::Cast) {
        // Integer cast: fold the inner value, retag the core kind to the
        // target's. Width semantics (truncation / extension) are encoded
        // by the literal-bearing site's TypeId; codegen handles the
        // actual narrowing on load. Float and pointer casts fail until
        // CE3/CE5 land.
        auto kids = hir.children(expr);
        if (kids.size() != 1) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        ConstEvalResult inner = evaluateConstant(hir, interner, literals, kids[0], options);
        if (!inner.value.has_value()) return inner;
        TypeId const targetTy = hir.typeId(expr);
        if (!targetTy.valid()) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        TypeKind const toK = interner.kind(targetTy);
        if (!isIntegerKind(toK)) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        std::int64_t const* iv = std::get_if<std::int64_t>(&inner.value->value);
        if (iv == nullptr) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        HirLiteralValue folded;
        folded.value = *iv;
        folded.core  = toK;
        return ok(std::move(folded));
    }
    return fail(ConstEvalFailure::NotAConstantExpression, expr);
}

} // namespace dss
