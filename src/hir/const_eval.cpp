#include "hir/const_eval.hpp"

#include "core/types/type_lattice/type_interner.hpp"
#include "hir/const_eval_arith.hpp"
#include "hir/hir.hpp"
#include "hir/hir_op.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

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
using detail::valueFitsInIntTarget;
using detail::wrapToIntTarget;

[[nodiscard]] inline ConstEvalResult fail(ConstEvalFailure why, HirNodeId blamed) {
    return ceFail(why, blamed);
}
[[nodiscard]] inline ConstEvalResult ok(HirLiteralValue v) {
    return ceOk(std::move(v));
}


// Internal recursive impl. `visitedSyms` carries the per-call cycle-
// detection set (CE2): any `Ref(sym)` resolution checks containment
// before descending into the symbol's defining expression. The public
// `evaluateConstant` seeds an empty set.
[[nodiscard]] ConstEvalResult
evalImpl(Hir const& hir, TypeInterner& interner, HirLiteralPool const& literals,
         HirNodeId expr, EvalEnvironment const& env, EvalOptions const& options,
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
        if (!env.resolveConstSymbol) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        std::uint32_t const sym = hir.payload(expr);
        if (visitedSyms.contains(sym)) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        auto definingExpr = env.resolveConstSymbol(SymbolId{sym});
        if (!definingExpr.has_value() || !definingExpr->valid()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        visitedSyms.insert(sym);
        ConstEvalResult inner = evalImpl(hir, interner, literals,
                                         *definingExpr, env, options, visitedSyms);
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
        ConstEvalResult inner = evalImpl(hir, interner, literals, kids[0], env, options, visitedSyms);
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
        ConstEvalResult a = evalImpl(hir, interner, literals, kids[0], env, options, visitedSyms);
        if (!a.value.has_value()) return a;
        ConstEvalResult b = evalImpl(hir, interner, literals, kids[1], env, options, visitedSyms);
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
        ConstEvalResult inner = evalImpl(hir, interner, literals, kids[0], env, options, visitedSyms);
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
            folded.value = narrowToFloatWidth(dv, info->bits);
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
            if (options.refuseOnLossyFloatConversion
             && !intToFloatIsLossless(*iv64, info->bits)) {
                return fail(ConstEvalFailure::LossyFloatConversion, expr);
            }
            double const widened = static_cast<double>(*iv64);
            HirLiteralValue folded;
            folded.core  = toK;
            folded.value = narrowToFloatWidth(widened, info->bits);
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
        ConstEvalResult a = evalImpl(hir, interner, literals, kids[0], env, options, visitedSyms);
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
        ConstEvalResult b = evalImpl(hir, interner, literals, kids[1], env, options, visitedSyms);
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
        ConstEvalResult cond = evalImpl(hir, interner, literals, kids[0], env, options, visitedSyms);
        if (!cond.value.has_value()) return cond;
        // Cond truthiness via the shared `asBool` (CE5): accepts float
        // operands when `allowFloat` is on, applying the same NaN/inf →
        // true semantics as LogicalAnd/Or.
        auto condIsTrueOpt = asBool(*cond.value, options.allowFloat);
        if (!condIsTrueOpt.has_value()) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        HirNodeId const selected = *condIsTrueOpt ? kids[1] : kids[2];
        ConstEvalResult inner = evalImpl(hir, interner, literals,
                                         selected, env, options, visitedSyms);
        if (inner.value.has_value()) {
            TypeId const ternTy = hir.typeId(expr);
            if (ternTy.valid()) inner.value->core = interner.kind(ternTy);
        }
        return inner;
    }
    if (k == HirKind::ConstructAggregate) {
        // D5.3: fold a struct / union / array aggregate construction.
        // The node's children are the POSITIONAL element expressions
        // (designators and zero-fills already normalized at HIR-
        // lowering time per HIR's positional discipline). Each child
        // must fold independently; the engine assembles their values
        // into a recursive `HirAggregateValue` arm of `HirLiteralValue`.
        // The first failing child propagates verbatim (failure code +
        // blame anchor stays at the child that didn't fold), so
        // MIR-globals' classify path can route a partially-non-constant
        // aggregate to runtime-init while surfacing the precise refusal
        // reason to consumers. `core` is read from the aggregate's
        // TypeId (Struct / Union / Array — the result-type tag the
        // engine's discipline requires).
        TypeId const aggTy = hir.typeId(expr);
        if (!aggTy.valid()) return fail(ConstEvalFailure::NotAConstantExpression, expr);
        auto kids = hir.children(expr);
        HirAggregateValue agg;
        agg.fields.reserve(kids.size());
        for (HirNodeId child : kids) {
            ConstEvalResult fr = evalImpl(hir, interner, literals, child,
                                          env, options, visitedSyms);
            if (!fr.value.has_value()) return fr;   // propagate failure verbatim
            agg.fields.push_back(std::move(*fr.value));
        }
        HirLiteralValue folded;
        folded.core  = interner.kind(aggTy);
        folded.value = std::move(agg);
        return ok(std::move(folded));
    }
    if (k == HirKind::SizeOf) {
        // FC6: fold `sizeof(T)` to T's byte size (result `size_t` = U64),
        // mirroring the MIR SizeOf fold (`hir_to_mir.cpp`). The TypeRef child
        // carries the sized type. Absent resolver (verifier consumers) or an
        // incomplete / un-sizeable type ⇒ `NotAConstantExpression` — never a
        // guessed size. The type unevaluated (C 6.5.3.4) — only its size matters.
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
    return fail(ConstEvalFailure::NotAConstantExpression, expr);
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
