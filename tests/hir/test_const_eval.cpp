// Unit tests for the shared constants-evaluation engine (plan 12.5 CE1).
// Builds HIR programmatically (no parser dependency) and exercises each
// foldable + each refuse-to-fold case against `evaluateConstant`.

#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/const_eval.hpp"
#include "hir/hir.hpp"
#include "hir/hir_literal_pool.hpp"
#include "hir/hir_op.hpp"

#include <gtest/gtest.h>

#include <array>
#include <variant>

using namespace dss;

namespace {

// Mini test rig: build a tiny HIR module with an in-memory literal pool +
// type interner, then call `evaluateConstant` against the root expression
// to fold. The interner is bound to CompilationUnitId{1} as a stand-in
// CU; CE1 doesn't intern anything, so the binding is purely structural.
struct Rig {
    TypeInterner   interner{CompilationUnitId{1}};
    HirLiteralPool literals{};
    HirBuilder     builder;
    HirNodeId      root{};   // assembled by the caller

    TypeId intT() { return interner.primitive(TypeKind::I32); }
    TypeId boolT() { return interner.primitive(TypeKind::Bool); }
    TypeId i64T() { return interner.primitive(TypeKind::I64); }

    HirNodeId litInt(std::int64_t v, TypeId ty) {
        HirLiteralValue lv;
        lv.value = v;
        lv.core  = interner.kind(ty);
        return builder.makeLiteral(ty, literals.add(lv));
    }
    HirNodeId unary(HirOpKind op, HirNodeId operand, TypeId resultTy) {
        return builder.addParent(HirKind::UnaryOp, std::array{operand},
                                 resultTy, encodeOp(op));
    }
    HirNodeId binary(HirOpKind op, HirNodeId l, HirNodeId r, TypeId resultTy) {
        return builder.addParent(HirKind::BinaryOp, std::array{l, r},
                                 resultTy, encodeOp(op));
    }
    HirNodeId cast(HirNodeId operand, TypeId targetTy) {
        return builder.makeCast(operand, targetTy);
    }
    [[nodiscard]] Hir finishWith(HirNodeId expr) {
        // HirBuilder::finish() accepts any built HirNodeId as the root for
        // ad-hoc unit tests like these (its only invariant is that the root
        // be a node it minted). We pass the expression directly; CE1 walks
        // the tree from there without needing a synthetic Module wrapper.
        return std::move(builder).finish(expr);
    }
};

ConstEvalResult eval(Rig& r, HirNodeId expr) {
    Hir hir = r.finishWith(expr);
    return evaluateConstant(hir, r.interner, r.literals, expr);
}

} // namespace

TEST(ConstEval, IntegerLiteralFolds) {
    Rig r;
    HirNodeId const expr = r.litInt(42, r.intT());
    auto res = eval(r, expr);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 42);
}

TEST(ConstEval, UnaryNegOnIntegerLiteralFolds) {
    Rig r;
    HirNodeId const lit = r.litInt(7, r.intT());
    HirNodeId const neg = r.unary(HirOpKind::Neg, lit, r.intT());
    auto res = eval(r, neg);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), -7);
}

TEST(ConstEval, UnaryBitNotOnIntegerLiteralFolds) {
    Rig r;
    HirNodeId const lit = r.litInt(0, r.intT());
    HirNodeId const bn  = r.unary(HirOpKind::BitNot, lit, r.intT());
    auto res = eval(r, bn);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), -1);   // ~0 = -1 (signed)
}

// Every integer BinaryOp in the engine's coverage table gets one positive
// test pin — a refactor that silently regresses one of the 16 ops would
// flip its individual EXPECT_EQ rather than slipping past a single golden.
TEST(ConstEval, BinaryArithmeticOpsFold) {
    auto check = [](HirOpKind op, std::int64_t lhs, std::int64_t rhs,
                    std::int64_t expected) {
        Rig r;
        HirNodeId const a   = r.litInt(lhs, r.intT());
        HirNodeId const b   = r.litInt(rhs, r.intT());
        HirNodeId const exp = r.binary(op, a, b, r.intT());
        auto res = eval(r, exp);
        ASSERT_TRUE(res.value.has_value())
            << "op ordinal " << static_cast<unsigned>(op) << " should fold";
        EXPECT_EQ(std::get<std::int64_t>(res.value->value), expected)
            << "op ordinal " << static_cast<unsigned>(op);
    };
    check(HirOpKind::Add,    7,  3,  10);
    check(HirOpKind::Sub,    7,  3,   4);
    check(HirOpKind::Mul,    7,  3,  21);
    check(HirOpKind::Div,    7,  3,   2);
    check(HirOpKind::Rem,    7,  3,   1);
    check(HirOpKind::BitAnd, 0b1100, 0b1010, 0b1000);
    check(HirOpKind::BitOr,  0b1100, 0b1010, 0b1110);
    check(HirOpKind::BitXor, 0b1100, 0b1010, 0b0110);
    check(HirOpKind::Shl,    1, 4, 16);
    check(HirOpKind::Shr,   32, 2,  8);
}

TEST(ConstEval, BinaryComparisonOpsFold) {
    auto check = [](HirOpKind op, std::int64_t lhs, std::int64_t rhs,
                    std::int64_t expected) {
        Rig r;
        HirNodeId const a   = r.litInt(lhs, r.intT());
        HirNodeId const b   = r.litInt(rhs, r.intT());
        HirNodeId const exp = r.binary(op, a, b, r.boolT());
        auto res = eval(r, exp);
        ASSERT_TRUE(res.value.has_value());
        EXPECT_EQ(std::get<std::int64_t>(res.value->value), expected)
            << "op ordinal " << static_cast<unsigned>(op);
    };
    check(HirOpKind::Eq, 5, 5, 1);  check(HirOpKind::Eq, 5, 6, 0);
    check(HirOpKind::Ne, 5, 5, 0);  check(HirOpKind::Ne, 5, 6, 1);
    check(HirOpKind::Lt, 3, 5, 1);  check(HirOpKind::Lt, 5, 3, 0);
    check(HirOpKind::Le, 5, 5, 1);  check(HirOpKind::Le, 6, 5, 0);
    check(HirOpKind::Gt, 5, 3, 1);  check(HirOpKind::Gt, 3, 5, 0);
    check(HirOpKind::Ge, 5, 5, 1);  check(HirOpKind::Ge, 3, 5, 0);
}

TEST(ConstEval, BinaryDivByZeroRefusesToFold) {
    Rig r;
    HirNodeId const a = r.litInt(7, r.intT());
    HirNodeId const z = r.litInt(0, r.intT());
    HirNodeId const div = r.binary(HirOpKind::Div, a, z, r.intT());
    auto res = eval(r, div);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::DivisionByZero);
}

TEST(ConstEval, BinaryRemByZeroRefusesToFold) {
    Rig r;
    HirNodeId const a = r.litInt(7, r.intT());
    HirNodeId const z = r.litInt(0, r.intT());
    HirNodeId const rem = r.binary(HirOpKind::Rem, a, z, r.intT());
    auto res = eval(r, rem);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::DivisionByZero);
}

TEST(ConstEval, BinaryShlWithOutOfRangeCountRefusesToFold) {
    Rig r;
    HirNodeId const a = r.litInt(1, r.intT());
    HirNodeId const sh = r.litInt(64, r.intT());   // 64 is out of range for i64
    HirNodeId const op = r.binary(HirOpKind::Shl, a, sh, r.intT());
    auto res = eval(r, op);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::ShiftCountOutOfRange);
}

TEST(ConstEval, BinaryShrWithNegativeCountRefusesToFold) {
    Rig r;
    HirNodeId const a = r.litInt(64, r.intT());
    HirNodeId const sh = r.litInt(-1, r.intT());
    HirNodeId const op = r.binary(HirOpKind::Shr, a, sh, r.intT());
    auto res = eval(r, op);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::ShiftCountOutOfRange);
}

TEST(ConstEval, ComparisonProducesBoolValuedZeroOrOne) {
    Rig r;
    HirNodeId const a = r.litInt(5, r.intT());
    HirNodeId const b = r.litInt(3, r.intT());
    HirNodeId const gt = r.binary(HirOpKind::Gt, a, b, r.boolT());
    auto res = eval(r, gt);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 1);
}

TEST(ConstEval, CastFromIntegerLiteralRetagsCoreKind) {
    Rig r;
    HirNodeId const lit  = r.litInt(255, r.intT());
    HirNodeId const c    = r.cast(lit, r.i64T());
    auto res = eval(r, c);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 255);
    EXPECT_EQ(res.value->core, TypeKind::I64);
}

TEST(ConstEval, CastOfFoldedBinaryFolds) {
    // The end-to-end case from the substrate-PR: `int g = 1 > 0;` lowers to
    // Cast(BinaryOp(Gt, 1, 0), int). Engine folds the inner comparison to
    // bool=1, then the Cast retags to int.
    Rig r;
    HirNodeId const a   = r.litInt(1, r.intT());
    HirNodeId const b   = r.litInt(0, r.intT());
    HirNodeId const gt  = r.binary(HirOpKind::Gt, a, b, r.boolT());
    HirNodeId const c   = r.cast(gt, r.intT());
    auto res = eval(r, c);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 1);
    EXPECT_EQ(res.value->core, TypeKind::I32);
}

TEST(ConstEval, RefuseOnDivByZeroPolicyDisabledReportsNotAConstant) {
    // The `refuseOnDivByZero=false` knob is the "be permissive" path:
    // engine still refuses to fold (UB-by-spec; we don't choose a value),
    // but the failure code surfaces as `NotAConstantExpression` instead of
    // the policy-anchored `DivisionByZero`. Callers can use this to route
    // div-by-zero to a "non-constant" fallback (e.g. MIR's runtime-init
    // path) rather than a user-facing diagnostic.
    Rig r;
    HirNodeId const a = r.litInt(1, r.intT());
    HirNodeId const z = r.litInt(0, r.intT());
    HirNodeId const div = r.binary(HirOpKind::Div, a, z, r.intT());
    EvalOptions opts;
    opts.refuseOnDivByZero = false;
    Hir hir = r.finishWith(div);
    auto res = evaluateConstant(hir, r.interner, r.literals, div, opts);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::NotAConstantExpression);
}

TEST(ConstEval, NonFoldableKindReturnsNotAConstantExpression) {
    // A Ref WITHOUT a resolver callback is not foldable. (CE2's callback
    // is opt-in: callers that don't set EvalOptions::resolveConstSymbol
    // get CE1's pure-literal behaviour.)
    Rig r;
    HirNodeId const ref = r.builder.makeRef(r.intT(), /*sym=*/1);
    auto res = eval(r, ref);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::NotAConstantExpression);
}

// CE2: a Ref to a constant-bound symbol resolves through the resolver
// callback. The engine recurses into the defining expression and folds.
TEST(ConstEval, RefToConstSymbolFoldsViaResolver) {
    Rig r;
    // Setup: imagine `const int a = 42; int b = a;`. We build `b`'s init
    // as a Ref to symbol 7 (`a`), and the resolver maps symbol 7 to a
    // separate Literal-42 HIR node.
    HirNodeId const aInit = r.litInt(42, r.intT());
    HirNodeId const bInit = r.builder.makeRef(r.intT(), /*sym=*/7);
    EvalOptions opts;
    opts.resolveConstSymbol = [&aInit](SymbolId s) -> std::optional<HirNodeId> {
        if (s.v == 7) return aInit;
        return std::nullopt;
    };
    Hir hir = r.finishWith(bInit);
    auto res = evaluateConstant(hir, r.interner, r.literals, bInit, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 42);
}

// CE2: transitive resolution — `a = 1; b = a; c = b;`. Each level
// resolves through the callback; the chain folds end-to-end.
TEST(ConstEval, RefChainFoldsTransitivelyViaResolver) {
    Rig r;
    HirNodeId const aInit = r.litInt(99, r.intT());
    HirNodeId const bInit = r.builder.makeRef(r.intT(), /*sym=*/1);   // a
    HirNodeId const cInit = r.builder.makeRef(r.intT(), /*sym=*/2);   // b
    EvalOptions opts;
    opts.resolveConstSymbol = [&](SymbolId s) -> std::optional<HirNodeId> {
        if (s.v == 1) return aInit;
        if (s.v == 2) return bInit;
        return std::nullopt;
    };
    Hir hir = r.finishWith(cInit);
    auto res = evaluateConstant(hir, r.interner, r.literals, cInit, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 99);
}

// CE2: cycle detection — `a = b; b = a;` doesn't infinite-recurse.
// Engine tracks visited symbols and surfaces NotAConstantExpression at
// the cycle-closing Ref.
TEST(ConstEval, RefCycleDoesNotInfiniteRecurseAndSurfacesNonConstant) {
    Rig r;
    HirNodeId const aInit = r.builder.makeRef(r.intT(), /*sym=*/2);   // a = b
    HirNodeId const bInit = r.builder.makeRef(r.intT(), /*sym=*/1);   // b = a
    EvalOptions opts;
    opts.resolveConstSymbol = [&](SymbolId s) -> std::optional<HirNodeId> {
        if (s.v == 1) return aInit;
        if (s.v == 2) return bInit;
        return std::nullopt;
    };
    Hir hir = r.finishWith(aInit);
    auto res = evaluateConstant(hir, r.interner, r.literals, aInit, opts);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::NotAConstantExpression);
    // Blame anchors at the USE site (the top-level Ref the caller passed),
    // not at some node inside the cycle's interior tree. The use site is
    // what the caller has source-span context for.
    EXPECT_EQ(res.blamedNode, aInit);
}

// CE2: resolver returns nullopt → engine reports NotAConstantExpression
// (not, e.g., UnsupportedOperator). Caller's "symbol isn't constant"
// fallback routes to runtime-init / per-language diagnostic.
TEST(ConstEval, RefToNonConstantSymbolViaResolverFails) {
    Rig r;
    HirNodeId const ref = r.builder.makeRef(r.intT(), /*sym=*/42);
    EvalOptions opts;
    opts.resolveConstSymbol = [](SymbolId) -> std::optional<HirNodeId> {
        return std::nullopt;   // every symbol is "not a constant"
    };
    Hir hir = r.finishWith(ref);
    auto res = evaluateConstant(hir, r.interner, r.literals, ref, opts);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::NotAConstantExpression);
}

// CE2 + CE1 compose: `int a = 1 + 2; int b = a + 1;` folds end-to-end.
TEST(ConstEval, RefAndArithmeticCompose) {
    Rig r;
    HirNodeId const one  = r.litInt(1, r.intT());
    HirNodeId const two  = r.litInt(2, r.intT());
    HirNodeId const aInit = r.binary(HirOpKind::Add, one, two, r.intT());   // 3
    HirNodeId const refA  = r.builder.makeRef(r.intT(), /*sym=*/5);
    HirNodeId const fourLit = r.litInt(4, r.intT());
    HirNodeId const bInit = r.binary(HirOpKind::Add, refA, fourLit, r.intT()); // a+4 = 7
    EvalOptions opts;
    opts.resolveConstSymbol = [&](SymbolId s) -> std::optional<HirNodeId> {
        if (s.v == 5) return aInit;
        return std::nullopt;
    };
    Hir hir = r.finishWith(bInit);
    auto res = evaluateConstant(hir, r.interner, r.literals, bInit, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 7);
}
