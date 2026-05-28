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
    HirNodeId logAnd(HirNodeId l, HirNodeId r) {
        return builder.makeLogicalAnd(l, r, boolT());
    }
    HirNodeId logOr(HirNodeId l, HirNodeId r) {
        return builder.makeLogicalOr(l, r, boolT());
    }
    HirNodeId ternary(HirNodeId c, HirNodeId t, HirNodeId e, TypeId resultTy) {
        return builder.makeTernary(c, t, e, resultTy);
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

// CE3: Cast to a SMALLER signed integer with a fitting value succeeds
// without invoking the wrap path.
TEST(ConstEval, CastFromI32ToI8WithFittingValueSucceeds) {
    Rig r;
    HirNodeId const lit = r.litInt(42, r.intT());
    HirNodeId const c   = r.cast(lit, r.interner.primitive(TypeKind::I8));
    auto res = eval(r, c);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 42);
    EXPECT_EQ(res.value->core, TypeKind::I8);
}

// CE3: Cast overflow with `refuseOnOverflow=true` (default) surfaces the
// Overflow failure code. D5.5 enum-bounds will pick this path.
TEST(ConstEval, CastFromI32ToI8WithOverflowRefusesByDefault) {
    Rig r;
    HirNodeId const lit = r.litInt(256, r.intT());   // outside [-128, 127]
    HirNodeId const c   = r.cast(lit, r.interner.primitive(TypeKind::I8));
    auto res = eval(r, c);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::Overflow);
    EXPECT_EQ(res.blamedNode, c);
}

// CE3: Cast overflow with `refuseOnOverflow=false` wraps modularly. The
// MIR-globals path uses this to keep folding when the runtime would
// produce a wrapped value anyway.
TEST(ConstEval, CastFromI32ToI8WithOverflowWrapsWhenAllowed) {
    Rig r;
    HirNodeId const lit = r.litInt(257, r.intT());   // 257 & 0xFF = 1
    HirNodeId const c   = r.cast(lit, r.interner.primitive(TypeKind::I8));
    EvalOptions opts;
    opts.refuseOnOverflow = false;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 1);
    EXPECT_EQ(res.value->core, TypeKind::I8);
}

// CE3: negative value wraps under signed-target narrowing (sign extension
// from the truncated bits).
TEST(ConstEval, CastFromI32ToI8WrapsNegativeViaSignExtension) {
    Rig r;
    // -1 fits in I8 directly — pick a value that needs wrap+sign-extend.
    // 257 wraps to 1 (above); -257 wraps to -1 via sign-extension.
    HirNodeId const lit = r.litInt(-257, r.intT());
    HirNodeId const c   = r.cast(lit, r.interner.primitive(TypeKind::I8));
    EvalOptions opts;
    opts.refuseOnOverflow = false;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), -1);
}

// CE3: Cast to Bool — `N != 0`.
TEST(ConstEval, CastNonZeroIntToBoolProducesOne) {
    Rig r;
    HirNodeId const lit = r.litInt(7, r.intT());
    HirNodeId const c   = r.cast(lit, r.boolT());
    auto res = eval(r, c);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 1);
    EXPECT_EQ(res.value->core, TypeKind::Bool);
}

TEST(ConstEval, CastZeroIntToBoolProducesZero) {
    Rig r;
    HirNodeId const lit = r.litInt(0, r.intT());
    HirNodeId const c   = r.cast(lit, r.boolT());
    auto res = eval(r, c);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 0);
}

// CE3: INT64_MIN → I32 boundary. The wrap path's `(masked & signBit) != 0`
// predicate would mis-trigger sign-extension if the mask computation were
// off by a bit. The low 32 bits of INT64_MIN are all zero, so the wrap
// should yield exactly 0 (not -1, not a partially-extended value).
TEST(ConstEval, CastFromI64MinToI32WrapsToZero) {
    Rig r;
    HirNodeId const lit = r.litInt(std::numeric_limits<std::int64_t>::min(), r.i64T());
    HirNodeId const c   = r.cast(lit, r.intT());
    EvalOptions opts;
    opts.refuseOnOverflow = false;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 0);
}

// CE3 silent-failure-hunter F1: Cast to U64 with a negative source refuses
// regardless of the `refuseOnOverflow` knob. The int64 storage arm can't
// reconcile signedness with downstream signed arithmetic; the engine
// refuses loudly rather than producing a wrong-typed fold.
TEST(ConstEval, CastNegativeToUnsigned64RefusesEvenWithOverflowAllowed) {
    Rig r;
    HirNodeId const lit = r.litInt(-1, r.i64T());
    HirNodeId const c   = r.cast(lit, r.interner.primitive(TypeKind::U64));
    EvalOptions opts;
    opts.refuseOnOverflow = false;   // even permissive policy refuses here
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, opts);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::Overflow);
}

// CE3 silent-failure-hunter F3: uint64-arm literal flows through the
// asInt64 bridge — a `char c = 'A';`-shaped value (decoded into uint64
// arm) folds correctly rather than rejecting as "not an integer literal".
TEST(ConstEval, Uint64ArmLiteralFoldsThroughAsInt64) {
    Rig r;
    HirLiteralValue lv;
    lv.value = std::uint64_t{65};   // 'A'
    lv.core  = TypeKind::Char;
    HirNodeId const lit = r.builder.makeLiteral(
        r.interner.primitive(TypeKind::Char), r.literals.add(lv));
    HirNodeId const c   = r.cast(lit, r.intT());
    auto res = eval(r, c);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 65);
    EXPECT_EQ(res.value->core, TypeKind::I32);
}

// CE3 silent-failure-hunter F3 (Bool arm): a bool-arm literal (from
// `.dsshir` round-trip or future bool-keyword frontends) folds via
// `asInt64`'s bool branch — `true` → 1, `false` → 0.
TEST(ConstEval, BoolArmLiteralFoldsThroughAsInt64) {
    Rig r;
    HirLiteralValue lv;
    lv.value = true;
    lv.core  = TypeKind::Bool;
    HirNodeId const lit = r.builder.makeLiteral(r.boolT(), r.literals.add(lv));
    HirNodeId const c   = r.cast(lit, r.intT());
    auto res = eval(r, c);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 1);
    EXPECT_EQ(res.value->core, TypeKind::I32);
}

// CE3 correctness F1: shift result core is the PROMOTED LEFT operand
// only (C99 §6.5.7p3) — the shift count's type does not contribute.
TEST(ConstEval, ShiftResultCoreIsPromotedLeftOperand) {
    Rig r;
    // LHS is I8 literal; RHS is I32 shift count. C99: result core = I32
    // (promoted I8), not commonType(I8, I32) — both happen to be I32
    // here, but the SOURCE OF THE TYPE must be the LHS-only promotion.
    HirLiteralValue lvA; lvA.value = std::int64_t{1}; lvA.core = TypeKind::I8;
    HirLiteralValue lvB; lvB.value = std::int64_t{2}; lvB.core = TypeKind::I32;
    HirNodeId const litA = r.builder.makeLiteral(
        r.interner.primitive(TypeKind::I8), r.literals.add(lvA));
    HirNodeId const litB = r.builder.makeLiteral(r.intT(), r.literals.add(lvB));
    HirNodeId const shl = r.binary(HirOpKind::Shl, litA, litB, r.intT());
    auto res = eval(r, shl);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 4);
    EXPECT_EQ(res.value->core, TypeKind::I32);  // promoted I8, not commonType
}

// CE3: Cast to unsigned narrows correctly (wrap policy).
TEST(ConstEval, CastFromI32ToU8WrapsCorrectly) {
    Rig r;
    HirNodeId const lit = r.litInt(300, r.intT());   // 300 & 0xFF = 44
    HirNodeId const c   = r.cast(lit, r.interner.primitive(TypeKind::U8));
    EvalOptions opts;
    opts.refuseOnOverflow = false;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 44);
}

// CE3: comparison ops always result in Bool core regardless of operand
// types (re-pin against the previous CE1 behaviour where the result
// inherited LHS's core).
TEST(ConstEval, ComparisonResultCoreIsAlwaysBool) {
    Rig r;
    HirNodeId const a = r.litInt(5, r.intT());
    HirNodeId const b = r.litInt(3, r.intT());
    HirNodeId const gt = r.binary(HirOpKind::Gt, a, b, r.boolT());
    auto res = eval(r, gt);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(res.value->core, TypeKind::Bool);
}

// CE3: mixed-width BinaryOp folds with the COMMON type's core (C99 UAC),
// not LHS's core. `I8 + I32 → I32` after integer promotion.
// ─── CE4: short-circuit folding ───────────────────────────────────────────

TEST(ConstEval, LogicalAndOfTrueAndFalseFolds) {
    Rig r;
    HirNodeId const a = r.litInt(1, r.intT());
    HirNodeId const b = r.litInt(0, r.intT());
    HirNodeId const and_ = r.logAnd(a, b);
    auto res = eval(r, and_);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 0);
    EXPECT_EQ(res.value->core, TypeKind::Bool);
}

TEST(ConstEval, LogicalAndOfTwoNonZeroFoldsToTrue) {
    Rig r;
    HirNodeId const a = r.litInt(5, r.intT());
    HirNodeId const b = r.litInt(7, r.intT());
    HirNodeId const and_ = r.logAnd(a, b);
    auto res = eval(r, and_);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 1);
    EXPECT_EQ(res.value->core, TypeKind::Bool);
}

// CE4 load-bearing: short-circuit means `0 && unfoldable` is foldable
// (the engine MUST NOT recurse into the right operand once the left
// determines the result). Build a definitely-unfoldable right operand
// (a Ref with no resolver) and verify the whole expression still folds.
TEST(ConstEval, LogicalAndShortCircuitsOnFalseLeft) {
    Rig r;
    HirNodeId const zero    = r.litInt(0, r.intT());
    HirNodeId const dangling = r.builder.makeRef(r.intT(), /*sym=*/9999);
    HirNodeId const and_    = r.logAnd(zero, dangling);
    auto res = eval(r, and_);
    ASSERT_TRUE(res.value.has_value()) << "0 && unfoldable must short-circuit";
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 0);
}

TEST(ConstEval, LogicalAndDoesNotShortCircuitOnTrueLeftAndFailsIfRightUnfoldable) {
    Rig r;
    HirNodeId const one      = r.litInt(1, r.intT());
    HirNodeId const dangling = r.builder.makeRef(r.intT(), /*sym=*/9999);
    HirNodeId const and_     = r.logAnd(one, dangling);
    auto res = eval(r, and_);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::NotAConstantExpression);
}

TEST(ConstEval, LogicalOrShortCircuitsOnTrueLeft) {
    Rig r;
    HirNodeId const one      = r.litInt(1, r.intT());
    HirNodeId const dangling = r.builder.makeRef(r.intT(), /*sym=*/9999);
    HirNodeId const or_      = r.logOr(one, dangling);
    auto res = eval(r, or_);
    ASSERT_TRUE(res.value.has_value()) << "1 || unfoldable must short-circuit";
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 1);
}

TEST(ConstEval, LogicalOrOfTwoZerosFoldsToFalse) {
    Rig r;
    HirNodeId const a   = r.litInt(0, r.intT());
    HirNodeId const b   = r.litInt(0, r.intT());
    HirNodeId const or_ = r.logOr(a, b);
    auto res = eval(r, or_);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 0);
}

// CE4 ternary: cond folds to true → recurse only into THEN. Else can be
// non-constant without poisoning the fold.
TEST(ConstEval, TernaryWithTrueCondFoldsToThenArmEvenIfElseUnfoldable) {
    Rig r;
    HirNodeId const cond     = r.litInt(1, r.intT());
    HirNodeId const then_    = r.litInt(42, r.intT());
    HirNodeId const dangling = r.builder.makeRef(r.intT(), /*sym=*/9999);
    HirNodeId const tern = r.ternary(cond, then_, dangling, r.intT());
    auto res = eval(r, tern);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 42);
}

TEST(ConstEval, TernaryWithFalseCondFoldsToElseArmEvenIfThenUnfoldable) {
    Rig r;
    HirNodeId const cond     = r.litInt(0, r.intT());
    HirNodeId const dangling = r.builder.makeRef(r.intT(), /*sym=*/9999);
    HirNodeId const else_    = r.litInt(7, r.intT());
    HirNodeId const tern = r.ternary(cond, dangling, else_, r.intT());
    auto res = eval(r, tern);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 7);
}

// Selected-arm failure propagates verbatim — diagnostic anchors at the
// arm's blamed node, NOT at the Ternary use site (per the user's
// "propagate" choice on the clarifying question).
TEST(ConstEval, TernaryFailurePropagatesFromSelectedArm) {
    Rig r;
    HirNodeId const cond     = r.litInt(1, r.intT());
    HirNodeId const dangling = r.builder.makeRef(r.intT(), /*sym=*/9999);
    HirNodeId const else_    = r.litInt(7, r.intT());
    HirNodeId const tern = r.ternary(cond, dangling, else_, r.intT());
    auto res = eval(r, tern);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::NotAConstantExpression);
    // Failure is blamed at the arm (dangling Ref), not at the Ternary.
    EXPECT_EQ(res.blamedNode, dangling);
}

// CE4: non-foldable cond → not foldable (the engine can't pick an arm
// without knowing the cond's value).
TEST(ConstEval, TernaryWithUnfoldableCondFails) {
    Rig r;
    HirNodeId const dangling = r.builder.makeRef(r.intT(), /*sym=*/9999);
    HirNodeId const a    = r.litInt(1, r.intT());
    HirNodeId const b    = r.litInt(2, r.intT());
    HirNodeId const tern = r.ternary(dangling, a, b, r.intT());
    auto res = eval(r, tern);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::NotAConstantExpression);
    // Blame anchors at the cond (the dangling Ref), not at the Ternary.
    EXPECT_EQ(res.blamedNode, dangling);
}

// CE4 LHS-first evaluation: a non-foldable LHS surfaces failure blamed
// at the LHS (NOT at the AND/OR node). A future refactor that tried to
// "peek at RHS first for short-circuit" would silently violate this.
TEST(ConstEval, LogicalAndWithUnfoldableLeftFails) {
    Rig r;
    HirNodeId const dangling = r.builder.makeRef(r.intT(), /*sym=*/9999);
    HirNodeId const one      = r.litInt(1, r.intT());
    HirNodeId const and_     = r.logAnd(dangling, one);
    auto res = eval(r, and_);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::NotAConstantExpression);
    EXPECT_EQ(res.blamedNode, dangling);
}

TEST(ConstEval, LogicalOrWithUnfoldableLeftFails) {
    Rig r;
    HirNodeId const dangling = r.builder.makeRef(r.intT(), /*sym=*/9999);
    HirNodeId const zero     = r.litInt(0, r.intT());
    HirNodeId const or_      = r.logOr(dangling, zero);
    auto res = eval(r, or_);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::NotAConstantExpression);
    EXPECT_EQ(res.blamedNode, dangling);
}

// CE4 OR truth-table gap: false-LHS + true-RHS exercises the
// no-short-circuit fallthrough that the other OR tests miss.
TEST(ConstEval, LogicalOrOfFalseAndTrueFoldsToTrue) {
    Rig r;
    HirNodeId const a   = r.litInt(0, r.intT());
    HirNodeId const b   = r.litInt(1, r.intT());
    HirNodeId const or_ = r.logOr(a, b);
    auto res = eval(r, or_);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 1);
    EXPECT_EQ(res.value->core, TypeKind::Bool);
}

// CE4 Ternary core re-tag: a narrower-than-Ternary arm core must NOT
// leak into the folded value's core. The Ternary node's declared
// typeId is authoritative.
TEST(ConstEval, TernaryArmCoreIsRetaggedToTernaryDeclaredType) {
    Rig r;
    HirNodeId const cond = r.litInt(1, r.intT());
    // Build a then-arm whose literal core is I8 (narrower than the
    // Ternary's declared I32 result type).
    HirLiteralValue thenLv;
    thenLv.value = std::int64_t{5};
    thenLv.core  = TypeKind::I8;
    HirNodeId const thenLit = r.builder.makeLiteral(
        r.interner.primitive(TypeKind::I8), r.literals.add(thenLv));
    HirNodeId const elseLit = r.litInt(9, r.intT());
    HirNodeId const tern = r.ternary(cond, thenLit, elseLit, r.intT());
    auto res = eval(r, tern);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 5);
    // Result core MUST be the Ternary's declared I32, not the arm's I8.
    EXPECT_EQ(res.value->core, TypeKind::I32);
}

TEST(ConstEval, BinaryOpMixedWidthResultCoreIsCommonType) {
    Rig r;
    // Build literals with different cores. Note the literal's `core`
    // field is what the engine reads (not the typeId — we test the
    // engine in isolation).
    HirLiteralValue lvA; lvA.value = std::int64_t{10}; lvA.core = TypeKind::I8;
    HirLiteralValue lvB; lvB.value = std::int64_t{20}; lvB.core = TypeKind::I32;
    HirNodeId const litA = r.builder.makeLiteral(
        r.interner.primitive(TypeKind::I8),  r.literals.add(lvA));
    HirNodeId const litB = r.builder.makeLiteral(r.intT(), r.literals.add(lvB));
    HirNodeId const sum = r.binary(HirOpKind::Add, litA, litB, r.intT());
    auto res = eval(r, sum);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 30);
    // C99 UAC: rank-<int promotes to int. Common(I8, I32) = I32.
    EXPECT_EQ(res.value->core, TypeKind::I32);
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
