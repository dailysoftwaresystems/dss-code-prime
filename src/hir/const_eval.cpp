#include "hir/const_eval.hpp"

#include "core/types/type_lattice/type_interner.hpp"
#include "hir/const_eval_arith.hpp"
#include "hir/hir.hpp"
#include "hir/hir_op.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss {

namespace {

using detail::asBool;
using detail::asDouble;
using detail::asInt64;
using detail::applyBinaryFloat;
using detail::applyBinaryInt;
using detail::applyUnaryFloat;
using detail::applyUnaryInt;
using detail::ceFail;
using detail::ceOk;
using detail::FloatKindInfo;
using detail::floatKindInfo;
using detail::IntKindInfo;
using detail::intKindInfo;
using detail::intToFloatIsLossless;
using detail::isFloatKind;
using detail::isFloatValue;
using detail::makeBoolLiteral;
using detail::narrowToFloatWidth;
using detail::toWideFloatOperand;
using detail::asIntBits;
using detail::valueFitsInIntTarget;
using detail::wrapToIntTarget;
using detail::floatToWideIntTarget;

[[nodiscard]] inline ConstEvalResult fail(ConstEvalFailure why, HirNodeId blamed) {
    return ceFail(why, blamed);
}
[[nodiscard]] inline ConstEvalResult ok(HirLiteralValue v) {
    return ceOk(std::move(v));
}


// ── THE FOLD COSTS HEAP, NOT HOST CALL FRAMES ──────────────────────────────
//
// D-CORE-TYPE-LAYOUT-AND-HIR-CONST-EVAL-RECURSE-PER-LEVEL. Plan 24 Stage 6
// flattened the three STRAIGHT-LINE arms (UnaryOp / BinaryOp / Cast) onto the
// `evalImpl` work stack and left every other arm DELEGATED — `evalNode` folded
// the node and re-entered `evalImpl` for its children. That left an
// `evalImpl` ⇄ `evalNode` MUTUAL recursion of two host frames per level for
// Ref, LogicalAnd/Or, Ternary and ConstructAggregate, i.e. for exactly the
// shapes a C initializer nests.
//
// ✔MEASURED before this cycle's conversion, on the ordinary ~1 MiB thread and
// through `ctest`: a 400-level nested aggregate, a 400-level ternary spine and
// a 400-level `&&` spine each folded with rc 0, and all three died at 1000 with
// rc 8 — no message, no location, and no `[  FAILED  ]` line. The same 400/1000
// pair bounded a REAL C source route (nested struct definitions plus a global
// initializer) through `lowerToMir`.
//
// ⇒ There is now ONE driver and no per-node recursive body at all: every arm
// with children is a phased frame on the heap stack, and what is left
// (`evalTerminal`) is childless by construction. The recursion is gone from the
// file rather than merely unreachable, which is what keeps a later edit from
// quietly restoring it.

// ── Plan 24 Stage 6 — straight-line const-fold epilogues ───────────────────
// Each `combine*` is the BYTE-IDENTICAL slice of a flattened arm AFTER its
// child sub-expression(s) have been folded (their `ConstEvalResult`s passed in).
// ONE source of truth for the driver's frames, so the iterative path produces
// the exact same value + failure code + blame anchor + result `core` the
// recursive path did. A child-failure short-circuit (`!inner.value`) returns the
// child's result VERBATIM (its own blame), matching
// `if (!inner.value.has_value()) return inner;` in the recursive form.

// UnaryOp epilogue (operand already folded to `inner`).
[[nodiscard]] ConstEvalResult
combineUnary(Hir const& hir, HirNodeId expr, EvalOptions const& options,
             ConstEvalResult inner) {
    if (!inner.value.has_value()) return inner;
    HirOpKind const op = decodeCoreOp(hir.payload(expr));
    // C4b (I2 go-live gate): a unary op on a `_BitInt` operand (`-5wb`, `~x`) folds
    // via the bignum — BEFORE the `asInt64`+`applyUnaryInt` path below, which would
    // negate a NARROW `_BitInt` via un-wrapped int64 arithmetic (a silent miscompile).
    if (auto uf = detail::foldBitIntUnary(op, *inner.value, options.charIsUnsigned); uf.applies) {
        if (uf.ok) return ok(std::move(uf.value));
        return fail(uf.failure, expr);
    }
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

// BinaryOp epilogue (lhs+rhs already folded, IN THAT ORDER — the recursive form
// folds `a = evalImpl(kids[0])` then `b = evalImpl(kids[1])` as two sequential
// statements, left-to-right and platform-independent). Either child's failure
// short-circuits with that child's verbatim result (a first, then b).
[[nodiscard]] ConstEvalResult
combineBinary(Hir const& hir, TypeInterner& interner, HirNodeId expr,
              EvalOptions const& options, ConstEvalResult a, ConstEvalResult b) {
    if (!a.value.has_value()) return a;
    if (!b.value.has_value()) return b;
    HirOpKind const op = decodeCoreOp(hir.payload(expr));
    auto kids = hir.children(expr);
    // C4b (D-CSUBSET-BITINT-CONSTFOLD-LARGE): a `_BitInt`-involving binary op folds
    // via the shared wrap-aware bignum at the TRUE C23 UAC result width (mod-2^N).
    // The fold sets its OWN result core (BitInt for a bit-precise result, or the
    // standard kind for an int-outranked BitInt) and returns HERE — bypassing the
    // `interner.commonType` retag below, which returns InvalidType for a BitInt (I2).
    if (auto bf = detail::foldBitIntBinary(op, *a.value, *b.value, options.charIsUnsigned); bf.applies) {
        if (bf.ok) return ok(std::move(bf.value));
        return fail(bf.failure, expr);
    }
    // CRIT-3 belt-and-suspenders: a BitInt-typed RESULT whose operand values did NOT
    // fold to bit-precise (a shape C's typing rules never produce) must NEVER take the
    // un-wrapped int64 path — fail loud rather than silently mis-fold.
    // D-CSUBSET-INT128-CONSTFOLD (TF-C94): I128/U128 join the belt for the SAME
    // reason and it is the load-bearing half of this cycle's const-fold closure.
    // The int64 path below wraps at 64 bits; a 128-bit-typed result reaching it
    // would be silently mod-2^64 — green in every existing test, wrong in the
    // emitted value. Complete routing (the widened `foldBitIntBinary` entry above
    // and the cast arm) should make this unreachable, so this belt is what turns
    // any FUTURE routing miss into a loud failure instead of a quiet wrap.
    TypeKind const resultKind = interner.kind(hir.typeId(expr));
    if (resultKind == TypeKind::BitInt || detail::isInt128Kind(resultKind)) {
        return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
    }
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
    // `asIntBits`, not `asInt64` — see
    // D-HIR-CONSTEVAL-UNSIGNED-WRAPAROUND-NOT-MODULAR.
    // This guard asks "is this an INTEGER operand", and an unsigned
    // value above INT64_MAX is one. Asking `asInt64` made it answer "is this
    // value representable as signed", which rejected every `u64` expression
    // whose high bit is set before `applyBinaryInt` -- which now establishes
    // the operation's domain -- ever saw it.
    if (!asIntBits(*a.value).has_value() || !asIntBits(*b.value).has_value()) {
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
        //   - Shift ops (Shl/Shr): the result type is the config-driven
        //     shift-result rule (D-UAC-SHIFT-RESULT-RULE-CONFIG) — already
        //     resolved and stamped on THIS node's authoritative typeId by
        //     cst_to_hir's `shiftResultType` funnel. Read it directly so
        //     the folded mirror agrees with the verb for EVERY language
        //     (never re-derive C's promoted-left discipline here — that
        //     would re-hardcode the rule a third time).
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
            if (TypeId const t = hir.typeId(expr); t.valid())
                folded->core = interner.kind(t);
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

// Cast epilogue (operand already folded to `inner`). Target-type-aware cast,
// byte-identical to the recursive arm (all four (source,target) quadrants +
// the bool special-cases). A non-foldable operand short-circuits verbatim.
[[nodiscard]] ConstEvalResult
combineCast(Hir const& hir, TypeInterner& interner, HirNodeId expr,
            EvalOptions const& options, ConstEvalResult inner) {
    if (!inner.value.has_value()) return inner;
    TypeId const targetTy = hir.typeId(expr);
    if (!targetTy.valid()) return fail(ConstEvalFailure::NotAConstantExpression, expr);
    TypeKind const toK = interner.kind(targetTy);
    // C4b (D-CSUBSET-BITINT-CONSTFOLD-LARGE): a cast TO `_BitInt(N)` folds via the
    // wrap-aware bignum `convertTo(N, signed)` (mod-2^N) — narrow AND wide — so
    // `_Static_assert((_BitInt(4))15 + 1 == 0)` and `(_BitInt(40))2000000 * …` fold
    // correctly. A cast FROM a `_BitInt` to a standard type flows through the ordinary
    // Int→Int / Int→Float / Int→Bool paths below (a NARROW `_BitInt` source bridges to
    // int64 via `asInt64`; a WIDE source nullopt-fails there — a documented boundary).
    if (toK == TypeKind::BitInt) {
        std::uint32_t const bw = static_cast<std::uint32_t>(interner.bitIntWidth(targetTy));
        bool const          bs = interner.bitIntIsSigned(targetTy);
        // [[D-C-FLOAT-CAST-DOES-NOT-FOLD-IN-A-CONSTANT-EXPRESSION]]: a FLOAT source
        // reaching a bit-precise target truncates per C 6.3.1.4p1 rather than being
        // handed to `asBitIntValue`, which answers only for integer arms and so
        // refused `(_BitInt(16))300.5` — a conversion clang 18.1.3 folds
        // (✔MEASURED; gcc 13.3.0 and MSVC 19.51 have no `_BitInt` at all).
        if (auto const f = detail::floatToWideIntTarget(*inner.value, bw, bs, options)) {
            if (!f->value.has_value()) return fail(ConstEvalFailure::Overflow, expr);
            HirLiteralValue v;
            v.core  = TypeKind::BitInt;
            v.value = *f->value;
            return ok(std::move(v));
        }
        auto bv = detail::asBitIntValue(*inner.value, options.charIsUnsigned);
        if (!bv.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        bv->convertTo(bw, bs);
        HirLiteralValue v;
        v.core  = TypeKind::BitInt;
        v.value = std::move(*bv);
        return ok(std::move(v));
    }
    // D-CSUBSET-INT128-CONSTFOLD (TF-C94): a cast TO a 128-bit integer routes
    // through the SAME wrap-aware bignum. Without this arm `(__uint128_t)X`
    // produced a plain u64/i64 literal merely TAGGED `core = U128`: the value had
    // already been truncated to 64 bits, so every later fold read a wrapped
    // operand while the type said 128. Mirrors the `_BitInt` arm above exactly —
    // `convertTo(128, signed)` is mod-2^128 — and differs only in the resulting
    // `core`, which stays I128/U128 (a `__int128` is a STANDARD type, not a
    // bit-precise one; conflating them would misname it in every later
    // diagnostic and mis-rank it in the usual arithmetic conversions).
    if (detail::isInt128Kind(toK)) {
        bool const i128Signed = (toK == TypeKind::I128);
        // The float twin of the `_BitInt` arm above, and the ONLY exact route for
        // it: `(__int128)1e30` has no int64 rendering, so an `asInt64` bridge
        // refused a conversion gcc 13.3.0 and clang 18.1.3 both fold (✔MEASURED).
        if (auto const f = detail::floatToWideIntTarget(*inner.value, 128u, i128Signed, options)) {
            if (!f->value.has_value()) return fail(ConstEvalFailure::Overflow, expr);
            HirLiteralValue v;
            v.core  = toK;
            v.value = *f->value;
            return ok(std::move(v));
        }
        auto bv = detail::asBitIntValue(*inner.value, options.charIsUnsigned);
        if (!bv.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        bv->convertTo(128u, i128Signed);
        HirLiteralValue v;
        v.core  = toK;
        v.value = std::move(*bv);
        return ok(std::move(v));
    }
    bool const targetFloat = isFloatKind(toK);
    bool const sourceFloat = isFloatValue(*inner.value);

    // Float-involving casts require the allowFloat knob.
    if ((sourceFloat || targetFloat) && !options.allowFloat) {
        return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
    }

    // Float → Bool: nonzero (incl. NaN/inf) → true; ±0 → false.
    if (sourceFloat && toK == TypeKind::Bool) {
        // LD-3: an F80/F128 source folds truthiness via isZero (asDouble nullopts
        // for the WideFloatValue arm — a `*asDouble` deref would be UB).
        if (auto const* wf = std::get_if<WideFloatValue>(&inner.value->value)) {
            return ok(makeBoolLiteral(wf->isZero() ? 0 : 1));
        }
        double const dv = *asDouble(*inner.value);
        return ok(makeBoolLiteral(dv != 0.0 ? 1 : 0));
    }

    // Float → Float: convert via host. F32 needs an actual narrowing round-trip
    // (otherwise the stored `double` would diverge from the IEEE-754 single-
    // precision value the runtime produces); F64 is identity; F16 has no host
    // backing on the `double` arm and refuses.
    if (sourceFloat && targetFloat) {
        // LD-3: an F80/F128 TARGET carries a WideFloatValue at true precision.
        // Widen a `double` leaf (exact) or pass a same-kind WideFloatValue through
        // (a cross-kind F80↔F128 cast — unreachable in valid C — refuses loud).
        if (WideFloatValue::isSupportedKind(toK)) {
            auto w = toWideFloatOperand(*inner.value, toK);
            if (!w.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            HirLiteralValue folded;
            folded.core  = toK;
            folded.value = *w;
            return ok(std::move(folded));
        }
        // Target is F16/F32/F64. An F80/F128 SOURCE narrows via the kernel's
        // round-to-nearest-even toDouble (then narrowToFloatWidth for F16/F32).
        auto info = floatKindInfo(toK);
        if (!info.has_value() || !info->hostBacked) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        double const dv = (std::get_if<WideFloatValue>(&inner.value->value) != nullptr)
            ? std::get<WideFloatValue>(inner.value->value).toDouble()
            : *asDouble(*inner.value);
        HirLiteralValue folded;
        folded.core  = toK;
        folded.value = narrowToFloatWidth(dv, info->bits);
        return ok(std::move(folded));
    }

    // Int → Float: host conversion (precision-loss for huge ints is
    // IEEE 754-defined behaviour; runtime path produces the same
    // bits). F16 refuses (no host backing on the `double` arm).
    if (!sourceFloat && targetFloat) {
        // LD-3: int → F80/F128 is ALWAYS exact (any int64 fits the 64/113-bit
        // significand — unlike int → binary64), so it folds via the kernel's
        // exact `fromInt64`, NEVER through a lossy host `double`. (No lossy-knob
        // check: the conversion cannot lose precision.)
        if (WideFloatValue::isSupportedKind(toK)) {
            auto iv = asInt64(*inner.value);
            if (!iv.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            HirLiteralValue folded;
            folded.core  = toK;
            folded.value = WideFloatValue::fromInt64(*iv, toK);
            return ok(std::move(folded));
        }
        auto info = floatKindInfo(toK);
        if (!info.has_value() || !info->hostBacked) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        // ⚠ THE WIDENING READS THE SOURCE'S SIGNEDNESS FROM ITS CORE
        // ([[D-C-FLOAT-CAST-DOES-NOT-FOLD-IN-A-CONSTANT-EXPRESSION]]). This used
        // to be `asInt64`, which nullopts for an unsigned value above INT64_MAX and
        // so refused `(double)18446744073709551615ULL` — a conversion all four
        // references fold, to 2^64 (✔MEASURED separately). The shared verb is the
        // one the CST walker's float-target arm calls, so the two cannot disagree.
        auto const widenedOpt = detail::integerConstantAsDouble(*inner.value, options.charIsUnsigned);
        if (!widenedOpt.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        double const widened = *widenedOpt;
        if (options.refuseOnLossyFloatConversion) {
            // The precision question is asked as a ROUND TRIP through the target's
            // own width, which is what `intToFloatIsLossless` does for a value with
            // an int64 rendering and is the only form available for one without.
            double const narrowed = narrowToFloatWidth(widened, info->bits);
            auto const iv64 = asInt64(*inner.value);
            bool const lossless = iv64.has_value()
                ? intToFloatIsLossless(*iv64, info->bits)
                : (std::isfinite(narrowed) && narrowed == widened);
            if (!lossless) return fail(ConstEvalFailure::LossyFloatConversion, expr);
        }
        HirLiteralValue folded;
        folded.core  = toK;
        folded.value = narrowToFloatWidth(widened, info->bits);
        return ok(std::move(folded));
    }

    // Float → Int: truncate toward zero (C99 §6.3.1.4); refuse when
    // the truncated value doesn't fit the integer target. NaN/inf
    // always refuse with Overflow — truncating them is undefined per
    // the standard and the bit pattern isn't portable. The arithmetic is
    // `const_eval_arith.hpp`'s, shared with the CST walker's cast arm.
    if (sourceFloat) {
        auto target = intKindInfo(toK, options.charIsUnsigned);
        if (!target.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        // LD-3: an F80/F128 source truncates via the kernel's toInt64 — the range
        // check is done AT THE OPERAND'S OWN PRECISION (never narrow-to-double
        // first; the 2^63 boundary would flip sign). nullopt = NaN / inf / out-of-
        // int64-range → refuse with Overflow, exactly as the double path below.
        if (auto const* wf = std::get_if<WideFloatValue>(&inner.value->value)) {
            auto iv = wf->toInt64();
            if (!iv.has_value()) return fail(ConstEvalFailure::Overflow, expr);
            if (toK == TypeKind::Bool) return ok(makeBoolLiteral(*iv));
            HirLiteralValue folded;
            folded.core = toK;
            if (target->bits >= 64 && !target->isSigned && *iv < 0) {
                return fail(ConstEvalFailure::Overflow, expr);
            }
            if (valueFitsInIntTarget(*iv, *target)) {
                folded.value = *iv;
                return ok(std::move(folded));
            }
            if (options.refuseOnOverflow) return fail(ConstEvalFailure::Overflow, expr);
            folded.value = wrapToIntTarget(*iv, *target);
            return ok(std::move(folded));
        }
        // ── THE RANGE TEST IS AT THE TARGET'S WIDTH, NOT AT INT64'S ────────
        // [[D-C-FLOAT-CAST-DOES-NOT-FOLD-IN-A-CONSTANT-EXPRESSION]]. This used to
        // truncate into an `int64_t` FIRST and range-check the target SECOND, so
        // every target inherited int64's ceiling. ✔MEASURED, that refused
        // `(unsigned long long)1.8446744e19` — a value comfortably inside
        // `unsigned long long`, and accepted by gcc 13.3.0, clang 18.1.3,
        // mingw-w64 gcc 13.2.0 and MSVC 19.51, probed separately. The predicates
        // now live in `const_eval_arith.hpp` and are the SAME ones the CST walker
        // calls, which is the point: the two walkers used to answer this question
        // in two places, and only one of them answered it at all.
        //
        // NaN / ±inf have no integral part (6.3.1.4p1) — `truncateFloatTowardZero`
        // nullopts, and the refusal is unconditional, not knob-governed.
        auto const truncOpt = detail::truncateFloatTowardZero(*inner.value);
        if (!truncOpt.has_value()) return fail(ConstEvalFailure::Overflow, expr);
        double const truncated = *truncOpt;
        if (toK == TypeKind::Bool) {
            return ok(makeBoolLiteral(truncated != 0.0 ? 1 : 0));
        }
        if (detail::truncatedFitsIntWidth(truncated,
                                          static_cast<std::uint32_t>(target->bits),
                                          target->isSigned)) {
            HirLiteralValue fits;
            fits.core = toK;
            // A 64-bit UNSIGNED target is the one width whose whole range does not
            // fit the int64 arm; it takes the uint64 arm, which `asIntBits` and
            // `asBool` already read. Every other admitted value is ≤ 63 bits of
            // magnitude and lands in the int64 arm exactly as before.
            if (!target->isSigned && target->bits >= 64) {
                fits.value = static_cast<std::uint64_t>(truncated);
            } else {
                fits.value = static_cast<std::int64_t>(truncated);
            }
            return ok(std::move(fits));
        }
        // Out of the target's range: UNDEFINED per 6.3.1.4p1, and the references do
        // not agree on what they produce — ✔MEASURED `(int)1e30` is INT_MAX on
        // gcc/clang/mingw and 0 on MSVC 19.51. There is no value to bake, so the
        // strict knob refuses. The permissive knob (`refuseOnOverflow=false`, used
        // by the optimizer's const-fold and one hir_to_mir site) keeps its wrap —
        // but ONLY through the int64 domain `wrapToIntTarget` can actually express;
        // outside it there is no portable wrap to emulate and the refusal stands
        // whatever the knob says, exactly as before.
        // ⚠ `wrapToIntTarget` is an INT64-DOMAIN helper and is IDENTITY at width
        // ≥ 64 (its own comment says so), so the wrap is offered only where it can
        // actually express the answer: a target NARROWER than 64 bits, holding a
        // truncated value that itself fits an int64. Outside that window — a
        // 64-bit-or-wider target the value missed, or a value outside int64 at all
        // — there is no expressible wrap and the refusal stands. That is the same
        // window the previous spelling admitted, stated once instead of as three
        // guards that had to be read together.
        constexpr double kInt64MaxExclusive = 9223372036854775808.0;   // 2^63 (exact)
        constexpr double kInt64Min          = -9223372036854775808.0;  // -2^63 (exact)
        if (options.refuseOnOverflow || target->bits >= 64
            || truncated >= kInt64MaxExclusive || truncated < kInt64Min) {
            return fail(ConstEvalFailure::Overflow, expr);
        }
        std::int64_t const iv = static_cast<std::int64_t>(truncated);
        HirLiteralValue folded;
        folded.core  = toK;
        folded.value = wrapToIntTarget(iv, *target);
        return ok(std::move(folded));
    }

    // Int → Int (and int → bool) — CE3's existing path.
    auto target = intKindInfo(toK, options.charIsUnsigned);
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
    // ── D-CE-NEGATIVE-WIDENED-TO-U64-NOT-CONSTFOLDABLE: THE GUARD THAT USED
    // TO STAND HERE IS GONE, AND ITS OWN COMMENT NAMED THE CONDITION ────────
    // It read: "Cast to unsigned target >=64 bits with a negative source value:
    // the int64 storage arm cannot reconcile signedness with downstream
    // signed-arithmetic paths (`applyBinaryInt` reads via int64) ... (When CE5
    // opens the uint64 arm for arithmetic, this restriction can lift.)" That
    // stated lift-condition is now met: `applyBinaryInt` evaluates in the
    // operation's own (width, signedness) domain and reads unsigned operands
    // through `uint64_t`, so the int64 payload is an unambiguous BIT PATTERN
    // and nothing downstream re-reads it as signed by accident.
    //
    // ★ IT WAS ALSO WRONG ON THE STANDARD'S OWN TERMS, WHICH IS WHY THIS IS A
    // DELETION AND NOT A KNOB. C 6.3.1.3p2 makes integer -> unsigned conversion
    // ALWAYS DEFINED (reduce modulo 2^N); there is no overflow to report, so
    // `(unsigned long)(-1)` was a legal constant expression being refused. It
    // now falls through to the shared `valueFitsInIntTarget` / `wrapToIntTarget`
    // policy every other width already used -- which is why `(unsigned int)(-1)`
    // MEASURED as working the whole time while `(unsigned long)(-1)` did not.
    //
    // ⚠ THE TWO FLOAT -> INT GUARDS OF THE SAME SHAPE ABOVE DELIBERATELY STAY.
    // C 6.3.1.4p1 makes a float -> integer conversion UNDEFINED when the
    // integral part is unrepresentable, so a negative double to an unsigned
    // type has no defined value to fold. Refusing there is fail-loud; deleting
    // it would invent an answer the standard does not give.
    //
    // ★★ AND `refuseOnOverflow` DOES NOT APPLY TO AN UNSIGNED TARGET AT ALL --
    // THE KNOB WAS CONFLATING TWO DIFFERENT PARAGRAPHS OF 6.3.1.3.
    //   p2 (target UNSIGNED): the value is reduced modulo 2^N. Always defined.
    //                         There is no overflow, so there is nothing for a
    //                         strict policy to refuse -- refusing would reject
    //                         a legal constant expression.
    //   p3 (target SIGNED):   an unrepresentable value is IMPLEMENTATION-DEFINED
    //                         (or raises a signal). A strict verifier is right
    //                         to refuse that, and it still does.
    // Deleting the >=64-bit guard alone was not enough: `valueFitsInIntTarget`
    // reports "no value change", which is FALSE for -1 -> 2^64-1, so the strict
    // knob still turned a defined conversion into an Overflow. Keying the knob
    // on the target's SIGNEDNESS is what makes the two paragraphs distinct.
    if (!target->isSigned) {
        folded.value = wrapToIntTarget(iv, *target);
        return ok(std::move(folded));
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

// ── The CHILDLESS arms, and only those ─────────────────────────────────────
//
// What is left after every arm with children became a driver frame: the leaves
// (`Literal`, `SizeOf`, `AlignOf`) plus the loud refusal every malformed or
// unmodelled node lands on. It takes no `visitedSyms` and calls nothing that
// walks HIR, so `const_eval` has no per-node recursive body any more.
//
// ⚠ THE ARITY / `isCoreOp` GUARDS ARE NOT DEAD, THEY ARE THE FAILURE CODES. A
// well-formed UnaryOp/BinaryOp/Cast/Logical/Ternary/ConstructAggregate/Ref is
// pushed as a frame by `enter` and never arrives here; a MALFORMED one is
// delegated here precisely so the code it always carried is still the code it
// gets. `UnsupportedOperator` for a non-core operator, `NotAConstantExpression`
// for everything else — the same two answers as before the flattening, which is
// why every arm whose only failure was `NotAConstantExpression` (Cast, Logical,
// Ternary, ConstructAggregate, Ref) now simply falls through to the shared
// refusal at the bottom instead of restating it.
[[nodiscard]] ConstEvalResult
evalTerminal(Hir const& hir, HirLiteralPool const& literals, HirNodeId expr,
             EvalEnvironment const& env) {
    if (!expr.valid()) return fail(ConstEvalFailure::NotAConstantExpression, expr);
    HirKind const k = hir.kind(expr);
    if (k == HirKind::Literal) {
        std::uint32_t const idx = hir.payload(expr);
        return ok(literals.at(idx));
    }
    if (k == HirKind::UnaryOp || k == HirKind::BinaryOp) {
        // Distinguish "the engine does not model this operator" from "this node
        // is malformed" — the one place that distinction is made, and the reason
        // these two kinds still appear in a function that folds no operators.
        if (!isCoreOp(hir.payload(expr)))
            return fail(ConstEvalFailure::UnsupportedOperator, expr);
        return fail(ConstEvalFailure::NotAConstantExpression, expr);
    }
    if (k == HirKind::SizeOf) {
        // FC6: fold `sizeof(T)` to T's byte size (result `size_t` = U64),
        // mirroring the MIR SizeOf fold (`hir_to_mir.cpp`). The TypeRef child
        // carries the sized type. Absent resolver (verifier consumers) or an
        // incomplete / un-sizeable type ⇒ `NotAConstantExpression` — never a
        // guessed size. The type unevaluated (C 6.5.3.4) — only its size matters.
        // (A CHILDLESS arm despite having a child: the TypeRef is read for its
        // TypeId, never folded, so nothing here re-enters the driver.)
        if (!env.resolveTypeSize) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        auto kids = hir.children(expr);
        if (kids.empty()) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        TypeId const sized = hir.typeId(kids.front());
        auto const sz = env.resolveTypeSize(sized);
        if (!sz) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        HirLiteralValue v;
        v.core  = TypeKind::U64;
        v.value = static_cast<std::uint64_t>(*sz);
        return ok(std::move(v));
    }
    if (k == HirKind::AlignOf) {
        // C11/C23 6.5.3.4: fold `_Alignof(T)` to T's alignment (result `size_t` =
        // U64), an ADDITIVE mirror of the SizeOf fold above reading the align
        // resolver instead of the size resolver. The TypeRef child carries the
        // queried type. Absent resolver (verifier consumers) or an incomplete /
        // un-alignable type ⇒ `NotAConstantExpression` — never a guessed align.
        if (!env.resolveTypeAlign) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        auto kids = hir.children(expr);
        if (kids.empty()) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        TypeId const queried = hir.typeId(kids.front());
        auto const al = env.resolveTypeAlign(queried);
        if (!al) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        HirLiteralValue v;
        v.core  = TypeKind::U64;
        v.value = static_cast<std::uint64_t>(*al);
        return ok(std::move(v));
    }
    return fail(ConstEvalFailure::NotAConstantExpression, expr);
}

// ── THE ITERATIVE CONST-FOLD DRIVER — every arm with children lives here ────
//
// A POD work-stack frame for ONE arm. `phase` counts children already REQUESTED;
// the final phase pops and delivers into `result`. Mirrors the Stage-4
// hir_to_mir `ValueFrame` idiom, and its realloc-safe rule is a hard
// requirement, not a style: copy the frame's fields to locals and advance
// `phase` BEFORE any `enter`/`push_back`, and copy `result` out before
// `pop_back`, because `work.back()` may dangle the moment the vector grows.
//
// The three STRAIGHT-LINE arms (Unary / Binary / Cast) reach a shared
// `combine*` epilogue. The four arms this cycle added (Ref / Logical / Ternary
// / Aggregate) carry their own prologue and epilogue in the switch below —
// moved out of the old `evalNode` VERBATIM, because each has semantics a
// generic frame cannot express: the Ref's visited-symbol scope, the logical
// arms' short-circuit, the Ternary's single-arm selection, and the aggregate's
// positional accumulation.
struct FoldFrame {
    enum class Kind : std::uint8_t {
        Unary, Binary, Cast, Ref, Logical, Ternary, Aggregate
    } kind;
    HirNodeId       node;
    std::uint32_t   phase;
    HirNodeId       child;   // Ref: the resolved DEFINING expression (see `enter`)
    ConstEvalResult c0;      // Binary: the folded LHS (stashed between phase 1 and 2)
    std::vector<HirLiteralValue> parts;   // Aggregate: elements folded so far
};

// Internal driver. `visitedSyms` carries the per-call Ref cycle-detection set;
// its insert/erase discipline is now split across the Ref frame — `enter`
// inserts when it decides to descend, the frame's final phase erases — which is
// the same bracket the recursive form had around its `evalImpl` call.
//
// For each node `enter` either PUSHES a frame (every kind that has children to
// fold) or DELEGATES to `evalTerminal` (a leaf, or a MALFORMED node whose own
// failure code that function owns). The arity / `isCoreOp` / typeId guards here
// mirror `evalTerminal`'s EXACTLY, so a malformed arm fails loud there with the
// code it always carried.
//
// Output-identity: every flattened arm reproduces the recursive child-fold
// ORDER and epilogue exactly, so value + failure code + blame + result core are
// byte-identical to the pre-conversion engine.
[[nodiscard]] ConstEvalResult
evalImpl(Hir const& hir, TypeInterner& interner, HirLiteralPool const& literals,
         HirNodeId expr, EvalEnvironment const& env, EvalOptions const& options,
         std::unordered_set<std::uint32_t>& visitedSyms) {
    std::vector<FoldFrame> work;
    // `enter` ALWAYS assigns `result` for a delegated node, and every pushed
    // frame delivers into `result` before it is read (then popped), so this
    // sentinel never leaks.
    ConstEvalResult result = fail(ConstEvalFailure::NotAConstantExpression, expr);

    auto const enter = [&](HirNodeId n) {
        if (n.valid()) {
            HirKind const nk = hir.kind(n);
            if (nk == HirKind::UnaryOp) {
                if (isCoreOp(hir.payload(n)) && hir.children(n).size() == 1) {
                    work.push_back({.kind = FoldFrame::Kind::Unary, .node = n, .phase = 0});
                    return;
                }
            } else if (nk == HirKind::BinaryOp) {
                if (isCoreOp(hir.payload(n)) && hir.children(n).size() == 2) {
                    work.push_back({.kind = FoldFrame::Kind::Binary, .node = n, .phase = 0});
                    return;
                }
            } else if (nk == HirKind::Cast) {
                if (hir.children(n).size() == 1) {
                    work.push_back({.kind = FoldFrame::Kind::Cast, .node = n, .phase = 0});
                    return;
                }
            } else if (nk == HirKind::Ref) {
                // CE2: resolve a Ref to a constant-bound symbol via the caller's
                // resolver callback. Absent callback (CE1's behaviour) or absent
                // mapping → NotAConstantExpression. Cycle detection prevents
                // infinite recursion on `int a = b; int b = a;`-shape inputs.
                //
                // ⚠ THE WHOLE PROLOGUE IS HERE, not split with `evalTerminal`,
                // and the reason is that `resolveConstSymbol` is a CALLER
                // callback: mirroring the guard would invoke it a second time
                // for every Ref that resolves. The resolved node is stashed in
                // the frame so phase 0 does not have to ask again.
                if (env.resolveConstSymbol) {
                    std::uint32_t const sym = hir.payload(n);
                    if (!visitedSyms.contains(sym)) {
                        auto definingExpr = env.resolveConstSymbol(SymbolId{sym});
                        if (definingExpr.has_value() && definingExpr->valid()) {
                            visitedSyms.insert(sym);
                            work.push_back({.kind  = FoldFrame::Kind::Ref,
                                            .node  = n,
                                            .phase = 0,
                                            .child = *definingExpr});
                            return;
                        }
                    }
                }
                result = fail(ConstEvalFailure::NotAConstantExpression, n);
                return;
            } else if (nk == HirKind::LogicalAnd || nk == HirKind::LogicalOr) {
                if (hir.children(n).size() == 2) {
                    work.push_back({.kind = FoldFrame::Kind::Logical, .node = n, .phase = 0});
                    return;
                }
            } else if (nk == HirKind::Ternary) {
                if (hir.children(n).size() == 3) {
                    work.push_back({.kind = FoldFrame::Kind::Ternary, .node = n, .phase = 0});
                    return;
                }
            } else if (nk == HirKind::ConstructAggregate) {
                // `core` is read from the aggregate's TypeId (Struct / Union /
                // Array — the result-type tag the engine's discipline requires),
                // so an invalid one is refused BEFORE any element is folded.
                if (hir.typeId(n).valid()) {
                    work.push_back({.kind = FoldFrame::Kind::Aggregate, .node = n, .phase = 0});
                    return;
                }
            }
        }
        // Delegate: a leaf (Literal / SizeOf / AlignOf), or a malformed arm →
        // fail loud there with its own code.
        result = evalTerminal(hir, literals, n, env);
    };

    enter(expr);
    while (!work.empty()) {
        FoldFrame& f = work.back();
        switch (f.kind) {
        case FoldFrame::Kind::Unary:
            if (f.phase == 0) {
                f.phase = 1;
                HirNodeId const operandN = hir.children(f.node)[0];
                enter(operandN);            // build operand — may invalidate `f`
            } else {
                HirNodeId const node2 = f.node;
                ConstEvalResult operand = std::move(result);
                work.pop_back();
                result = combineUnary(hir, node2, options, std::move(operand));
            }
            break;
        case FoldFrame::Kind::Binary:
            // LHS first (phase 0→1), then RHS (phase 1→2) — matching the
            // recursive `a = evalImpl(kids[0]); b = evalImpl(kids[1]);` (two
            // SEQUENTIAL statements → left-to-right, platform-independent).
            if (f.phase == 0) {
                f.phase = 1;
                HirNodeId const lhsN = hir.children(f.node)[0];
                enter(lhsN);                // build LHS — may invalidate `f`
            } else if (f.phase == 1) {
                f.c0 = std::move(result);   // LHS result
                f.phase = 2;
                HirNodeId const rhsN = hir.children(f.node)[1];
                enter(rhsN);                // build RHS — may invalidate `f`
            } else {
                HirNodeId const node2 = f.node;
                ConstEvalResult lhs = std::move(f.c0);
                ConstEvalResult rhs = std::move(result);
                work.pop_back();
                result = combineBinary(hir, interner, node2, options,
                                       std::move(lhs), std::move(rhs));
            }
            break;
        case FoldFrame::Kind::Cast:
            if (f.phase == 0) {
                f.phase = 1;
                HirNodeId const operandN = hir.children(f.node)[0];
                enter(operandN);            // build operand — may invalidate `f`
            } else {
                HirNodeId const node2 = f.node;
                ConstEvalResult operand = std::move(result);
                work.pop_back();
                result = combineCast(hir, interner, node2, options, std::move(operand));
            }
            break;
        case FoldFrame::Kind::Ref:
            // Phase 0 folds the symbol's DEFINING expression (resolved once, in
            // `enter`); phase 1 leaves the symbol's scope and re-blames.
            if (f.phase == 0) {
                f.phase = 1;
                HirNodeId const definingExpr = f.child;
                enter(definingExpr);        // may invalidate `f`
            } else {
                HirNodeId const node2 = f.node;
                std::uint32_t const sym = hir.payload(node2);
                work.pop_back();
                visitedSyms.erase(sym);
                // On failure, re-blame at the Ref USE site rather than wherever
                // the definition tree's failure surfaced. The caller (a
                // diagnostic emitter) has the use-site span available; the
                // definition-tree's node may live in an entirely different
                // module decl and carry no helpful context. On success, blame
                // stays default (no anchor needed).
                if (!result.value.has_value()) result.blamedNode = node2;
            }
            break;
        case FoldFrame::Kind::Logical: {
            // C99 short-circuit semantics: evaluate `a` first. If `a` already
            // determines the result (`0 && unfoldable` is unambiguously false;
            // `1 || unfoldable` is unambiguously true), the engine MUST NOT
            // descend into `b` — otherwise a non-foldable `b` would spuriously
            // fail the whole fold. Result core is always Bool.
            HirNodeId const node2 = f.node;
            if (f.phase == 0) {
                f.phase = 1;
                HirNodeId const lhsN = hir.children(node2)[0];
                enter(lhsN);                // may invalidate `f`
                break;
            }
            if (f.phase == 1) {
                if (!result.value.has_value()) {   // propagate `a`'s failure verbatim
                    work.pop_back();
                    break;
                }
                // `asBool` handles both integer and float operands (the latter
                // only when `allowFloat` is on); NaN / ±inf evaluate to true per
                // C semantics; ±0.0 evaluates to false.
                auto aIsTrueOpt = asBool(*result.value, options.allowFloat);
                if (!aIsTrueOpt.has_value()) {
                    work.pop_back();
                    result = fail(ConstEvalFailure::UnsupportedTypeKind, node2);
                    break;
                }
                bool const aIsTrue = *aIsTrueOpt;
                bool const isAnd   = (hir.kind(node2) == HirKind::LogicalAnd);
                // && short-circuits when `a` is false; || short-circuits when
                // `a` is true. Either way the determined value IS `aIsTrue`.
                if (isAnd ? !aIsTrue : aIsTrue) {
                    work.pop_back();
                    result = ok(makeBoolLiteral(aIsTrue ? 1 : 0));
                    break;
                }
                f.phase = 2;
                HirNodeId const rhsN = hir.children(node2)[1];
                enter(rhsN);                // may invalidate `f`
                break;
            }
            work.pop_back();
            if (!result.value.has_value()) break;   // propagate `b`'s failure verbatim
            auto bIsTrueOpt = asBool(*result.value, options.allowFloat);
            if (!bIsTrueOpt.has_value()) {
                result = fail(ConstEvalFailure::UnsupportedTypeKind, node2);
                break;
            }
            result = ok(makeBoolLiteral(*bIsTrueOpt ? 1 : 0));
            break;
        }
        case FoldFrame::Kind::Ternary: {
            // children: [cond, then, else]. Fold cond first; descend into ONLY
            // the selected arm. The unselected arm may be non-constant — a
            // legitimate compile-time-known choice between a constant and a
            // computation (`cond ? known : maybe_runtime`) should still fold
            // when cond and the chosen arm are both constants. The selected
            // arm's failure propagates verbatim (`blamedNode` retains the arm's
            // anchor).
            HirNodeId const node2 = f.node;
            if (f.phase == 0) {
                f.phase = 1;
                HirNodeId const condN = hir.children(node2)[0];
                enter(condN);               // may invalidate `f`
                break;
            }
            if (f.phase == 1) {
                if (!result.value.has_value()) {   // propagate cond's failure
                    work.pop_back();
                    break;
                }
                // Cond truthiness via the shared `asBool` (CE5): accepts float
                // operands when `allowFloat` is on, applying the same NaN/inf →
                // true semantics as LogicalAnd/Or.
                auto condIsTrueOpt = asBool(*result.value, options.allowFloat);
                if (!condIsTrueOpt.has_value()) {
                    work.pop_back();
                    result = fail(ConstEvalFailure::UnsupportedTypeKind, node2);
                    break;
                }
                auto kids = hir.children(node2);
                HirNodeId const selected = *condIsTrueOpt ? kids[1] : kids[2];
                f.phase = 2;
                enter(selected);            // may invalidate `f`
                break;
            }
            work.pop_back();
            // Result core: the SELECTED arm's `core` may be narrower than the
            // Ternary's declared `typeId` (e.g. `cond ? (int8)5 : 1000` where
            // the Ternary type is I32). Re-tag the folded core from the Ternary
            // node's typeId so `core` mirrors the authoritative type record (per
            // the `hir_literal_pool.hpp` contract). Same discipline as
            // BinaryOp's `commonType` retag.
            if (result.value.has_value()) {
                TypeId const ternTy = hir.typeId(node2);
                if (ternTy.valid()) result.value->core = interner.kind(ternTy);
            }
            break;
        }
        case FoldFrame::Kind::Aggregate: {
            // D5.3: fold a struct / union / array aggregate construction. The
            // node's children are the POSITIONAL element expressions
            // (designators and zero-fills already normalized at HIR-lowering
            // time per HIR's positional discipline). Each element must fold
            // independently; the first failing one propagates VERBATIM (failure
            // code + blame anchor stay at the element that didn't fold), so
            // MIR-globals' classify path can route a partially-non-constant
            // aggregate to runtime-init while surfacing the precise refusal.
            //
            // `phase` is the number of elements already REQUESTED, so it is both
            // the index of the next child and the count of results collected.
            HirNodeId const node2 = f.node;
            auto kids = hir.children(node2);
            if (f.phase == 0) {
                f.parts.reserve(kids.size());
            } else {
                if (!result.value.has_value()) {   // propagate the element's failure
                    work.pop_back();
                    break;
                }
                f.parts.push_back(std::move(*result.value));
            }
            if (static_cast<std::size_t>(f.phase) == kids.size()) {
                HirAggregateValue agg;
                agg.fields = std::move(f.parts);
                work.pop_back();
                HirLiteralValue folded;
                folded.core  = interner.kind(hir.typeId(node2));
                folded.value = std::move(agg);
                result = ok(std::move(folded));
                break;
            }
            HirNodeId const child = kids[static_cast<std::size_t>(f.phase)];
            f.phase += 1;
            enter(child);                   // may invalidate `f`
            break;
        }
        }
    }
    return result;
}

} // namespace

ConstEvalResult evaluateConstant(Hir const& hir,
                                 TypeInterner& interner,
                                 HirLiteralPool const& literals,
                                 HirNodeId expr,
                                 EvalEnvironment env,
                                 EvalOptions options) {
    std::unordered_set<std::uint32_t> visitedSyms;
    return evalImpl(hir, interner, literals, expr, env, options, visitedSyms);
}

} // namespace dss
