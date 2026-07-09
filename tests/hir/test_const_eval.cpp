// Unit tests for the shared constants-evaluation engine (plan 12.5 CE1).
// Builds HIR programmatically (no parser dependency) and exercises each
// foldable + each refuse-to-fold case against `evaluateConstant`.

#include "core/substrate/large_stack_call.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "hir/const_eval.hpp"
#include "hir/hir.hpp"
#include "hir/hir_literal_pool.hpp"
#include "hir/hir_op.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
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
    HirNodeId litFloat(double v, TypeId ty) {
        HirLiteralValue lv;
        lv.value = v;
        lv.core  = interner.kind(ty);
        return builder.makeLiteral(ty, literals.add(lv));
    }
    TypeId f64T() { return interner.primitive(TypeKind::F64); }
    TypeId f32T() { return interner.primitive(TypeKind::F32); }
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

// D-CE-HOST-SIGNED-OVERFLOW-UB (the PR#34 UBSan CI catch, 2026-07-04): the
// engine's Neg/Add/Sub/Mul folds evaluate 2's-complement WRAP over uint64 —
// the direct signed forms were HOST UB at the INT64 boundaries (UBSan:
// "negation of -9223372036854775808 cannot be represented", hit by
// stdint_limit_macros through the preprocessor's #if const-eval). The wrapped
// value is the runtime outcome on every shipped target AND gcc/clang's #if
// fold, so these pins assert the exact wrap results. Red-on-disable: revert
// any helper to the direct signed form → that op's pin is the UBSan trap
// site again (the linux-clang UBSan CI leg aborts; the VALUE stays the same
// on wrap-happy hosts, which is exactly why only a sanitizer sees it — these
// pins lock the DEFINED semantics so the fold's answer can never drift).
TEST(ConstEval, IntegerFoldsWrapAtInt64BoundariesWithoutHostUb) {
    auto const i64min = std::numeric_limits<std::int64_t>::min();
    auto const i64max = std::numeric_limits<std::int64_t>::max();

    // -INT64_MIN wraps to itself (the reported UBSan site).
    {
        Rig r;
        HirNodeId const lit = r.litInt(i64min, r.i64T());
        HirNodeId const neg = r.unary(HirOpKind::Neg, lit, r.i64T());
        auto res = eval(r, neg);
        ASSERT_TRUE(res.value.has_value());
        EXPECT_EQ(std::get<std::int64_t>(res.value->value), i64min);
    }
    auto checkWrap = [](HirOpKind op, std::int64_t lhs, std::int64_t rhs,
                        std::int64_t expected) {
        Rig r;
        HirNodeId const a   = r.litInt(lhs, r.i64T());
        HirNodeId const b   = r.litInt(rhs, r.i64T());
        HirNodeId const exp = r.binary(op, a, b, r.i64T());
        auto res = eval(r, exp);
        ASSERT_TRUE(res.value.has_value())
            << "op ordinal " << static_cast<unsigned>(op) << " should fold (wrap)";
        EXPECT_EQ(std::get<std::int64_t>(res.value->value), expected)
            << "op ordinal " << static_cast<unsigned>(op);
    };
    checkWrap(HirOpKind::Add, i64max,  1, i64min);        // INT64_MAX+1 → wrap
    checkWrap(HirOpKind::Sub, i64min,  1, i64max);        // INT64_MIN-1 → wrap
    checkWrap(HirOpKind::Sub, 0, i64min, i64min);         // 0-INT64_MIN → wrap (== Neg)
    checkWrap(HirOpKind::Mul, i64max,  2, std::int64_t{-2}); // (2^63-1)*2 ≡ -2 mod 2^64
    checkWrap(HirOpKind::Mul, i64min, -1, i64min);        // INT64_MIN*-1 → wrap
}

// The CONTRAST policy: Div/Rem at INT64_MIN/-1 REFUSE (Overflow) rather than
// wrap — the runtime outcome is target-divergent (x86 idiv #DE traps, arm64
// wraps), so folding any value would hide the trap; refusal keeps the op
// live for the target to define. Guards the asymmetry the wrap pins above
// must never erase.
TEST(ConstEval, DivRemAtInt64MinByMinusOneRefuseWithOverflow) {
    auto const i64min = std::numeric_limits<std::int64_t>::min();
    for (HirOpKind const op : {HirOpKind::Div, HirOpKind::Rem}) {
        Rig r;
        HirNodeId const a   = r.litInt(i64min, r.i64T());
        HirNodeId const b   = r.litInt(-1,     r.i64T());
        HirNodeId const exp = r.binary(op, a, b, r.i64T());
        auto res = eval(r, exp);
        EXPECT_FALSE(res.value.has_value())
            << "op ordinal " << static_cast<unsigned>(op) << " must refuse";
        EXPECT_EQ(res.failure, ConstEvalFailure::Overflow)
            << "op ordinal " << static_cast<unsigned>(op);
    }
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
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
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
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
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
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
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
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
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
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
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

// ─── CE5: allowFloat + IEEE 754 policy ────────────────────────────────────

// Without `allowFloat`, the engine refuses any float-touching expression.
TEST(ConstEval, FloatRefusedWhenAllowFloatOff) {
    Rig r;
    HirNodeId const a = r.litFloat(1.5, r.f64T());
    HirNodeId const b = r.litFloat(2.5, r.f64T());
    HirNodeId const add = r.binary(HirOpKind::Add, a, b, r.f64T());
    auto res = eval(r, add);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::UnsupportedTypeKind);
}

TEST(ConstEval, FloatAddFoldsWithAllowFloat) {
    Rig r;
    HirNodeId const a = r.litFloat(1.5, r.f64T());
    HirNodeId const b = r.litFloat(2.0, r.f64T());
    HirNodeId const add = r.binary(HirOpKind::Add, a, b, r.f64T());
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(add);
    auto res = evaluateConstant(hir, r.interner, r.literals, add, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(res.value->value), 3.5);
    EXPECT_EQ(res.value->core, TypeKind::F64);
}

// IEEE 754 div-by-zero is well-defined (±inf), unlike integer div-by-zero.
TEST(ConstEval, FloatDivByZeroProducesInfNotFailure) {
    Rig r;
    HirNodeId const a = r.litFloat(1.0, r.f64T());
    HirNodeId const b = r.litFloat(0.0, r.f64T());
    HirNodeId const div = r.binary(HirOpKind::Div, a, b, r.f64T());
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(div);
    auto res = evaluateConstant(hir, r.interner, r.literals, div, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_TRUE(std::isinf(std::get<double>(res.value->value)));
}

// NaN propagates through arithmetic.
TEST(ConstEval, FloatNanPropagates) {
    Rig r;
    double const nan = std::nan("");
    HirNodeId const a = r.litFloat(nan, r.f64T());
    HirNodeId const b = r.litFloat(1.0, r.f64T());
    HirNodeId const add = r.binary(HirOpKind::Add, a, b, r.f64T());
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(add);
    auto res = evaluateConstant(hir, r.interner, r.literals, add, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_TRUE(std::isnan(std::get<double>(res.value->value)));
}

// NaN comparison returns false (unordered) per IEEE 754 — even `nan == nan`.
TEST(ConstEval, FloatNanEqNanReturnsFalse) {
    Rig r;
    double const nan = std::nan("");
    HirNodeId const a = r.litFloat(nan, r.f64T());
    HirNodeId const b = r.litFloat(nan, r.f64T());
    HirNodeId const eq = r.binary(HirOpKind::Eq, a, b, r.boolT());
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(eq);
    auto res = evaluateConstant(hir, r.interner, r.literals, eq, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 0);
    EXPECT_EQ(res.value->core, TypeKind::Bool);
}

// Mixed int + float operand promotes to float per C99 UAC.
TEST(ConstEval, MixedIntFloatBinaryOpPromotesToFloat) {
    Rig r;
    HirNodeId const a = r.litInt(2, r.intT());
    HirNodeId const b = r.litFloat(1.5, r.f64T());
    HirNodeId const add = r.binary(HirOpKind::Add, a, b, r.f64T());
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(add);
    auto res = evaluateConstant(hir, r.interner, r.literals, add, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(res.value->value), 3.5);
}

// Int → Float cast.
TEST(ConstEval, CastIntToFloat) {
    Rig r;
    HirNodeId const i = r.litInt(7, r.intT());
    HirNodeId const c = r.cast(i, r.f64T());
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(res.value->value), 7.0);
    EXPECT_EQ(res.value->core, TypeKind::F64);
}

// Float → Int cast: truncate toward zero (C99 §6.3.1.4).
TEST(ConstEval, CastFloatToIntTruncatesTowardZero) {
    auto check = [](double in, std::int64_t expected) {
        Rig r;
        HirNodeId const f = r.litFloat(in, r.f64T());
        HirNodeId const c = r.cast(f, r.intT());
        EvalOptions opts;
        opts.allowFloat = true;
        Hir hir = r.finishWith(c);
        auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
        ASSERT_TRUE(res.value.has_value()) << "for input " << in;
        EXPECT_EQ(std::get<std::int64_t>(res.value->value), expected) << "for input " << in;
    };
    check(3.7,  3);
    check(-3.7, -3);   // truncate toward zero, not floor
    check(0.0,  0);
}

// Float → Int Cast with NaN refuses regardless of the overflow knob.
TEST(ConstEval, CastNanToIntRefuses) {
    Rig r;
    HirNodeId const f = r.litFloat(std::nan(""), r.f64T());
    HirNodeId const c = r.cast(f, r.intT());
    EvalOptions opts;
    opts.allowFloat       = true;
    opts.refuseOnOverflow = false;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::Overflow);
}

// Float → Bool: nonzero (incl. NaN/inf) → true; ±0 → false.
TEST(ConstEval, CastFloatToBoolHandlesNaNAndInfAndZero) {
    auto check = [](double in, std::int64_t expected) {
        Rig r;
        HirNodeId const f = r.litFloat(in, r.f64T());
        HirNodeId const c = r.cast(f, r.boolT());
        EvalOptions opts;
        opts.allowFloat = true;
        Hir hir = r.finishWith(c);
        auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
        ASSERT_TRUE(res.value.has_value()) << "for input " << in;
        EXPECT_EQ(std::get<std::int64_t>(res.value->value), expected) << "for input " << in;
    };
    check(0.0,                            0);
    check(-0.0,                           0);
    check(0.1,                            1);
    check(-3.14,                          1);
    check(std::nan(""),                   1);    // NaN truthy
    check(std::numeric_limits<double>::infinity(),  1);
    check(-std::numeric_limits<double>::infinity(), 1);
}

// LogicalAnd with float operands (via asBool) — gated on allowFloat.
TEST(ConstEval, LogicalAndAcceptsFloatOperandsWhenAllowFloat) {
    Rig r;
    HirNodeId const a = r.litFloat(0.0, r.f64T());
    HirNodeId const b = r.litFloat(1.5, r.f64T());
    HirNodeId const and_ = r.logAnd(a, b);
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(and_);
    auto res = evaluateConstant(hir, r.interner, r.literals, and_, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 0);
    EXPECT_EQ(res.value->core, TypeKind::Bool);
}

// Float Cast overflow with refuseOnOverflow=true.
TEST(ConstEval, CastFloatToIntOverflowRefuses) {
    Rig r;
    HirNodeId const f = r.litFloat(1e20, r.f64T());   // way bigger than int64
    HirNodeId const c = r.cast(f, r.intT());
    EvalOptions opts;
    opts.allowFloat       = true;
    opts.refuseOnOverflow = true;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::Overflow);
}

// Unary negation on float.
TEST(ConstEval, UnaryNegOnFloat) {
    Rig r;
    HirNodeId const f = r.litFloat(3.14, r.f64T());
    HirNodeId const neg = r.unary(HirOpKind::Neg, f, r.f64T());
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(neg);
    auto res = evaluateConstant(hir, r.interner, r.literals, neg, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(res.value->value), -3.14);
}

// BitNot on float is C99-undefined — refuses with `UnsupportedTypeKind`
// (the operator IS modelled by the engine for int operands; the type is
// wrong, not the op). `UnsupportedOperator` would mis-signal "engine
// doesn't fold this op yet" and confuse verifier diagnostics.
TEST(ConstEval, BitNotOnFloatRefusesAsUnsupportedTypeKind) {
    Rig r;
    HirNodeId const f = r.litFloat(3.14, r.f64T());
    HirNodeId const bn = r.unary(HirOpKind::BitNot, f, r.f64T());
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(bn);
    auto res = evaluateConstant(hir, r.interner, r.literals, bn, {}, opts);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::UnsupportedTypeKind);
}

// Rem on float is C99-undefined — same UnsupportedTypeKind contract as
// BitNot above (covers the applyBinaryFloat side of the helper pair).
TEST(ConstEval, RemOnFloatRefusesAsUnsupportedTypeKind) {
    Rig r;
    HirNodeId const a   = r.litFloat(3.14, r.f64T());
    HirNodeId const b   = r.litFloat(2.0,  r.f64T());
    HirNodeId const rem = r.binary(HirOpKind::Rem, a, b, r.f64T());
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(rem);
    auto res = evaluateConstant(hir, r.interner, r.literals, rem, {}, opts);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::UnsupportedTypeKind);
}

// IEEE 754 ordered comparisons: < ≤ > ≥ produce defined results for
// non-NaN operands AND unordered (false) when either operand is NaN.
// Covers the float-comparison branches of applyBinaryFloat that the
// initial CE5 tests pinned only for Eq/Ne.
TEST(ConstEval, FloatOrderedComparisonsFold) {
    auto run = [](HirOpKind op, double l, double r) {
        Rig rig;
        HirNodeId const a   = rig.litFloat(l, rig.f64T());
        HirNodeId const b   = rig.litFloat(r, rig.f64T());
        HirNodeId const cmp = rig.binary(op, a, b, rig.boolT());
        EvalOptions opts;
        opts.allowFloat = true;
        Hir hir = rig.finishWith(cmp);
        return evaluateConstant(hir, rig.interner, rig.literals, cmp, {}, opts);
    };
    auto truthy = [&](HirOpKind op, double l, double r) {
        auto res = run(op, l, r);
        EXPECT_TRUE(res.value.has_value());
        EXPECT_EQ(std::get<std::int64_t>(res.value->value), 1);
        EXPECT_EQ(res.value->core, TypeKind::Bool);
    };
    auto falsy = [&](HirOpKind op, double l, double r) {
        auto res = run(op, l, r);
        EXPECT_TRUE(res.value.has_value());
        EXPECT_EQ(std::get<std::int64_t>(res.value->value), 0);
        EXPECT_EQ(res.value->core, TypeKind::Bool);
    };
    truthy(HirOpKind::Lt, 1.0, 2.0); falsy(HirOpKind::Lt, 2.0, 1.0);
    truthy(HirOpKind::Le, 2.0, 2.0); falsy(HirOpKind::Le, 3.0, 2.0);
    truthy(HirOpKind::Gt, 2.0, 1.0); falsy(HirOpKind::Gt, 1.0, 2.0);
    truthy(HirOpKind::Ge, 2.0, 2.0); falsy(HirOpKind::Ge, 1.0, 2.0);
    // NaN unordered: ALL four ordered comparisons return false.
    double const nan = std::nan("");
    falsy(HirOpKind::Lt, nan, 1.0); falsy(HirOpKind::Lt, 1.0, nan);
    falsy(HirOpKind::Le, nan, 1.0); falsy(HirOpKind::Le, 1.0, nan);
    falsy(HirOpKind::Gt, nan, 1.0); falsy(HirOpKind::Gt, 1.0, nan);
    falsy(HirOpKind::Ge, nan, 1.0); falsy(HirOpKind::Ge, 1.0, nan);
}

// F32 target: the host narrows via `static_cast<float>` round-trip so the
// stored double matches the runtime's IEEE single-precision value. Test
// pins a value (0.1) that is NOT exactly representable in either single
// or double and verifies the engine's result matches the host's single
// rounding (not the input double's bits).
TEST(ConstEval, CastF64ToF32ActuallyNarrows) {
    Rig r;
    HirNodeId const f = r.litFloat(0.1, r.f64T());
    HirNodeId const c = r.cast(f, r.f32T());
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(res.value->core, TypeKind::F32);
    double const stored   = std::get<double>(res.value->value);
    double const expected = static_cast<double>(static_cast<float>(0.1));
    EXPECT_DOUBLE_EQ(stored, expected);
    EXPECT_NE(stored, 0.1);   // narrowing actually happened
}

// F32 target from int source: same narrowing discipline.
TEST(ConstEval, CastIntToF32NarrowsViaFloat) {
    Rig r;
    // 16777217 = 2^24 + 1 — exact in double but NOT in float (loses LSB).
    HirNodeId const lit = r.litInt(16777217, r.i64T());
    HirNodeId const c   = r.cast(lit, r.f32T());
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(res.value->core, TypeKind::F32);
    EXPECT_DOUBLE_EQ(std::get<double>(res.value->value),
                     static_cast<double>(static_cast<float>(16777217.0)));
}

// F16 Cast (plan 12.5 §0.2 D1, closed): soft-float `narrowToHalf` rounds
// to IEEE 754 binary16. Exact halves round-trip; non-exact values round
// to nearest-even and re-widen to the closest double.
TEST(ConstEval, CastF64ToF16ExactHalfRoundsTrips) {
    Rig r;
    // 1.0 is exactly representable in binary16.
    HirNodeId const f = r.litFloat(1.0, r.f64T());
    HirNodeId const c = r.cast(f, r.interner.primitive(TypeKind::F16));
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(res.value->core, TypeKind::F16);
    EXPECT_DOUBLE_EQ(std::get<double>(res.value->value), 1.0);
}

TEST(ConstEval, CastF64ToF16NonExactRoundsAndDoesNotEqualSource) {
    Rig r;
    // 0.1 is not exactly representable in binary16; the round-trip
    // produces a different stored bit pattern (~0.0999755859375).
    HirNodeId const f = r.litFloat(0.1, r.f64T());
    HirNodeId const c = r.cast(f, r.interner.primitive(TypeKind::F16));
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(res.value->core, TypeKind::F16);
    double const stored = std::get<double>(res.value->value);
    EXPECT_NE(stored, 0.1);   // narrowing happened
    // Within one half-ULP of 0.1.
    EXPECT_NEAR(stored, 0.1, 1.0 / 1024.0);
}

TEST(ConstEval, CastF64ToF16OverflowsToInfinity) {
    Rig r;
    HirNodeId const f = r.litFloat(1e10, r.f64T());   // way past F16 max (~65504)
    HirNodeId const c = r.cast(f, r.interner.primitive(TypeKind::F16));
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_TRUE(std::isinf(std::get<double>(res.value->value)));
}

// F128 stays refused — no host backing AND no engine arithmetic on F128.
// Plan 12.5 §0.2 D1b → mapped to plan 19 HR-HW reserved (first consumer
// owns the soft-float / wider-than-int64 substrate).
TEST(ConstEval, CastF64ToF128RefusesAsUnsupportedTypeKind) {
    Rig r;
    HirNodeId const f = r.litFloat(1.0, r.f64T());
    HirNodeId const c = r.cast(f, r.interner.primitive(TypeKind::F128));
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::UnsupportedTypeKind);
}

// Plan 12.5 §0.2 D2 (closed): refuseOnLossyFloatConversion knob.
// Int→F32 of a 24-bit-precise-or-bigger value loses bits; verifier
// consumers opt in to refuse. Off-by-default codegen path keeps the
// runtime-equivalent silent precision loss.
TEST(ConstEval, CastIntToF32LossyRefusesWhenKnobOn) {
    Rig r;
    // 2^24 + 1 = 16777217 — exact in double + int64, NOT in float.
    HirNodeId const lit = r.litInt(16777217, r.i64T());
    HirNodeId const c   = r.cast(lit, r.f32T());
    EvalOptions opts;
    opts.allowFloat                  = true;
    opts.refuseOnLossyFloatConversion = true;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::LossyFloatConversion);
}

TEST(ConstEval, CastIntToF32LosslessAcceptsWhenKnobOn) {
    Rig r;
    // 16777216 = 2^24 — exact in single + double + int64.
    HirNodeId const lit = r.litInt(16777216, r.i64T());
    HirNodeId const c   = r.cast(lit, r.f32T());
    EvalOptions opts;
    opts.allowFloat                  = true;
    opts.refuseOnLossyFloatConversion = true;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(res.value->value), 16777216.0);
}

TEST(ConstEval, CastIntToF32LossyAcceptsByDefault) {
    Rig r;
    HirNodeId const lit = r.litInt(16777217, r.i64T());
    HirNodeId const c   = r.cast(lit, r.f32T());
    EvalOptions opts;
    opts.allowFloat = true;
    // refuseOnLossyFloatConversion = false (default) — codegen-equivalent.
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    ASSERT_TRUE(res.value.has_value());   // accepted with precision loss
}

// Float→int boundary at exactly 2^63. The naive
// `truncated > static_cast<double>(INT64_MAX)` would compare against
// 2^63 (INT64_MAX rounds UP on conversion to double) and miss
// `truncated == 2^63` — a value that subsequently undefined-behaves
// when cast to int64_t. Engine must refuse at this exact boundary.
TEST(ConstEval, CastFloatToInt64AtPositiveBoundaryRefuses) {
    Rig r;
    HirNodeId const f = r.litFloat(9223372036854775808.0, r.f64T());   // 2^63
    HirNodeId const c = r.cast(f, r.interner.primitive(TypeKind::I64));
    EvalOptions opts;
    opts.allowFloat = true;
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::Overflow);
}

// Float→int out-of-int64: refuses UNCONDITIONALLY of refuseOnOverflow.
// The C99 §6.3.1.4 bit-pattern is implementation-defined when the
// truncated value doesn't fit the destination integer type, and the
// engine has no portable wrap to emulate. Same policy as NaN/inf above.
TEST(ConstEval, CastFloatOutOfInt64RefusesEvenWithRefuseOnOverflowOff) {
    Rig r;
    HirNodeId const f = r.litFloat(1e30, r.f64T());   // way beyond int64
    HirNodeId const c = r.cast(f, r.intT());
    EvalOptions opts;
    opts.allowFloat       = true;
    opts.refuseOnOverflow = false;   // knob OFF — but engine still refuses
    Hir hir = r.finishWith(c);
    auto res = evaluateConstant(hir, r.interner, r.literals, c, {}, opts);
    EXPECT_FALSE(res.value.has_value());
    EXPECT_EQ(res.failure, ConstEvalFailure::Overflow);
}

// Unary negation on float covers IEEE corners: NaN stays NaN with sign
// flipped (still NaN by value), ±inf flips sign, -0.0 → +0.0. None of
// these should refuse; the engine must just call host `-x`.
TEST(ConstEval, UnaryNegOnFloatHandlesNaNInfAndNegativeZero) {
    auto neg = [](double v) {
        Rig rig;
        HirNodeId const f = rig.litFloat(v, rig.f64T());
        HirNodeId const n = rig.unary(HirOpKind::Neg, f, rig.f64T());
        EvalOptions opts;
        opts.allowFloat = true;
        Hir hir = rig.finishWith(n);
        return evaluateConstant(hir, rig.interner, rig.literals, n, {}, opts);
    };
    {
        auto res = neg(std::nan(""));
        ASSERT_TRUE(res.value.has_value());
        EXPECT_TRUE(std::isnan(std::get<double>(res.value->value)));
    }
    {
        auto res = neg(std::numeric_limits<double>::infinity());
        ASSERT_TRUE(res.value.has_value());
        EXPECT_DOUBLE_EQ(std::get<double>(res.value->value),
                         -std::numeric_limits<double>::infinity());
    }
    {
        auto res = neg(-0.0);
        ASSERT_TRUE(res.value.has_value());
        double const out = std::get<double>(res.value->value);
        EXPECT_DOUBLE_EQ(out, 0.0);
        EXPECT_FALSE(std::signbit(out));   // -(-0) == +0 per IEEE
    }
}

// Cast Byte / Char source-and-target: these are integer kinds (8-bit
// unsigned and 32-bit Unicode codepoint respectively) so they go
// through the int→int path. Pin them so the Cast acceptance matrix
// explicitly covers them — closes the architecture-review gap noted
// in plan 12.5 closure.
TEST(ConstEval, CastIntToByteAndCharFolds) {
    Rig r;
    // 0x41 ('A') fits both Byte and Char.
    HirNodeId const lit = r.litInt(0x41, r.intT());
    HirNodeId const cb  = r.cast(lit, r.interner.primitive(TypeKind::Byte));
    auto resB = eval(r, cb);
    ASSERT_TRUE(resB.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(resB.value->value), 0x41);
    EXPECT_EQ(resB.value->core, TypeKind::Byte);
    // Char target via a fresh rig.
    Rig r2;
    HirNodeId const lit2 = r2.litInt(0x41, r2.intT());
    HirNodeId const cc   = r2.cast(lit2, r2.interner.primitive(TypeKind::Char));
    auto resC = eval(r2, cc);
    ASSERT_TRUE(resC.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(resC.value->value), 0x41);
    EXPECT_EQ(resC.value->core, TypeKind::Char);
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
    auto res = evaluateConstant(hir, r.interner, r.literals, div, {}, opts);
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
    EvalEnvironment env;
    env.resolveConstSymbol = [&aInit](SymbolId s) -> std::optional<HirNodeId> {
        if (s.v == 7) return aInit;
        return std::nullopt;
    };
    Hir hir = r.finishWith(bInit);
    auto res = evaluateConstant(hir, r.interner, r.literals, bInit, env);
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
    EvalEnvironment env;
    env.resolveConstSymbol = [&](SymbolId s) -> std::optional<HirNodeId> {
        if (s.v == 1) return aInit;
        if (s.v == 2) return bInit;
        return std::nullopt;
    };
    Hir hir = r.finishWith(cInit);
    auto res = evaluateConstant(hir, r.interner, r.literals, cInit, env);
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
    EvalEnvironment env;
    env.resolveConstSymbol = [&](SymbolId s) -> std::optional<HirNodeId> {
        if (s.v == 1) return aInit;
        if (s.v == 2) return bInit;
        return std::nullopt;
    };
    Hir hir = r.finishWith(aInit);
    auto res = evaluateConstant(hir, r.interner, r.literals, aInit, env);
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
    EvalEnvironment env;
    env.resolveConstSymbol = [](SymbolId) -> std::optional<HirNodeId> {
        return std::nullopt;   // every symbol is "not a constant"
    };
    Hir hir = r.finishWith(ref);
    auto res = evaluateConstant(hir, r.interner, r.literals, ref, env);
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
    EvalEnvironment env;
    env.resolveConstSymbol = [&](SymbolId s) -> std::optional<HirNodeId> {
        if (s.v == 5) return aInit;
        return std::nullopt;
    };
    Hir hir = r.finishWith(bInit);
    auto res = evaluateConstant(hir, r.interner, r.literals, bInit, env);
    ASSERT_TRUE(res.value.has_value());
    EXPECT_EQ(std::get<std::int64_t>(res.value->value), 7);
}

// FC6: `sizeof(T)` folds to T's byte size ONLY when the `resolveTypeSize` hook
// is supplied — the hook is load-bearing. Without it the engine surfaces
// NotAConstantExpression (the pre-FC6 behaviour verifier consumers keep), so a
// regression that drops the hook from a consumer's EvalEnvironment goes RED.
TEST(ConstEval, SizeOfFoldsOnlyWithTypeSizeResolver) {
    Rig r;
    // sizeof of an 8-byte primitive (I64). The TypeRef child carries the type.
    TypeId const sized   = r.i64T();
    HirNodeId const tref  = r.builder.makeTypeRef(sized);
    HirNodeId const sizeOf =
        r.builder.makeSizeOf(tref, r.interner.primitive(TypeKind::U64));
    Hir hir = r.finishWith(sizeOf);

    // No resolver → NotAConstantExpression (load-bearing red-on-disable).
    {
        auto res = evaluateConstant(hir, r.interner, r.literals, sizeOf);
        EXPECT_FALSE(res.value.has_value());
    }
    // With the REAL computeLayout resolver → folds to size_t(8), core U64.
    {
        EvalEnvironment env;
        env.resolveTypeSize = [&](TypeId t) -> std::optional<std::uint64_t> {
            auto const l = computeLayout(
                t, r.interner,
                AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                DataModel::Lp64);
            if (!l) return std::nullopt;
            return l->size;
        };
        auto res = evaluateConstant(hir, r.interner, r.literals, sizeOf, env);
        ASSERT_TRUE(res.value.has_value());
        EXPECT_EQ(std::get<std::uint64_t>(res.value->value), 8u);
        EXPECT_EQ(res.value->core, TypeKind::U64);
    }
}

// C11/C23 6.5.3.4: `_Alignof(T)` folds to T's alignment ONLY when the
// `resolveTypeAlign` hook is supplied — the hook is load-bearing (mirrors the
// SizeOf/resolveTypeSize pin). Without it the engine surfaces
// NotAConstantExpression, so a regression that drops the hook goes RED.
TEST(ConstEval, AlignOfFoldsOnlyWithTypeAlignResolver) {
    Rig r;
    // _Alignof of an 8-byte primitive (I64) → alignment 8. The TypeRef child
    // carries the queried type.
    TypeId const queried  = r.i64T();
    HirNodeId const tref   = r.builder.makeTypeRef(queried);
    HirNodeId const alignOf =
        r.builder.makeAlignOf(tref, r.interner.primitive(TypeKind::U64));
    Hir hir = r.finishWith(alignOf);

    // No resolver → NotAConstantExpression (load-bearing red-on-disable).
    {
        auto res = evaluateConstant(hir, r.interner, r.literals, alignOf);
        EXPECT_FALSE(res.value.has_value());
    }
    // With the REAL computeLayout resolver → folds to size_t(8), core U64.
    {
        EvalEnvironment env;
        env.resolveTypeAlign = [&](TypeId t) -> std::optional<std::uint64_t> {
            auto const l = computeLayout(
                t, r.interner,
                AggregateLayoutParams{ScalarAlignmentRule::Natural, 16},
                DataModel::Lp64);
            if (!l) return std::nullopt;
            return l->align.bytes();
        };
        auto res = evaluateConstant(hir, r.interner, r.literals, alignOf, env);
        ASSERT_TRUE(res.value.has_value());
        EXPECT_EQ(std::get<std::uint64_t>(res.value->value), 8u);
        EXPECT_EQ(res.value->core, TypeKind::U64);
    }
}

// ── Plan 24 Stage 6 — deep const-fold flatten pins (SF-4) ──────────────────
// `evaluateConstant` (src/hir/const_eval.cpp) is now an explicit WORK-STACK
// DRIVER for the deep STRAIGHT-LINE arms (BinaryOp / UnaryOp / Cast — the arms
// whose only recursion is `evalImpl(child)`), so a deeply-nested const-expr
// folds with FLAT O(1) host-stack cost per nesting level instead of one host
// frame per level. These pins fold a ~3000-deep chain and assert the EXACT
// value — the byte-identity correctness witness at depth (any phase/combine-
// order bug yields the wrong value → red).
//
// WITNESS SPLIT (mirrors the MIR IterativeDeep pins): the build + fold +
// teardown run on the 64 MiB worker (runOnLargeStack) so the ORTHOGONAL
// per-node HIR-teardown recursion (which overflows a stock main stack at this
// depth, independent of the fold) is removed — isolating the property under
// test (the FOLD is flat) from an unrelated destructor-recursion artifact. The
// flat/host-stack-overflow RUN witness for the fold itself lives in the CORPUS
// (`deep_global_const_init` folds a deep global initializer through the real
// pipeline on the DEFAULT stack and would crash rc-127 if the driver recursed).
//
// RED-ON-DISABLE: reverting the driver to recursion makes the deep fold recurse
// ~3000 frames; on the worker the value still computes (so these pins stay green
// — they pin VALUE byte-identity, not the overflow), but the corpus crashes. A
// combine-order regression (e.g. swapping LHS/RHS fold order, or a wrong-phase
// pop) corrupts the folded value here → red. Depth 3000 is ~12x the parser's
// 256 cap and far past any shallow output-identity test above.
TEST(ConstEval, IterativeDeepBinaryChainFoldsFlatAndByteIdentical) {
    constexpr std::int64_t kDepth = 3000;   // # of Sub nodes (chain levels)
    dss::substrate::runOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&] {
        Rig r;
        TypeId const i32 = r.intT();
        // Left-assoc SUBTRACTION spine `((((5000 - 1) - 1) - 1) … )` (kDepth
        // levels) → 5000 - kDepth == 2000. Subtraction is NON-commutative AND
        // non-associative, so the value is a strict witness of BOTH the
        // left-to-right LHS-then-RHS fold ORDER (a swapped combine `1 - prev`
        // diverges immediately) AND every level being folded exactly once (a
        // dropped/duplicated level drifts the result). RED on a phase/combine-
        // order regression.
        HirNodeId cur = r.litInt(5000, i32);
        for (std::int64_t i = 0; i < kDepth; ++i)
            cur = r.binary(HirOpKind::Sub, cur, r.litInt(1, i32), i32);
        Hir hir = r.finishWith(cur);
        auto res = evaluateConstant(hir, r.interner, r.literals, cur);
        ASSERT_TRUE(res.value.has_value()) << "deep Sub chain must fold";
        EXPECT_EQ(std::get<std::int64_t>(res.value->value), 5000 - kDepth);  // 2000
    });
}

TEST(ConstEval, IterativeDeepUnaryChainFoldsFlatAndByteIdentical) {
    constexpr std::int64_t kDepth = 3001;   // ODD # of Neg nodes → sign flips
    dss::substrate::runOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&] {
        Rig r;
        TypeId const i32 = r.intT();
        // `-(-(-(…-(7))))` with an ODD count of negations → -7 (parity-sensitive:
        // a single dropped/duplicated Neg level flips the sign → red).
        HirNodeId cur = r.litInt(7, i32);
        for (std::int64_t i = 0; i < kDepth; ++i) cur = r.unary(HirOpKind::Neg, cur, i32);
        Hir hir = r.finishWith(cur);
        auto res = evaluateConstant(hir, r.interner, r.literals, cur);
        ASSERT_TRUE(res.value.has_value()) << "deep Neg chain must fold";
        EXPECT_EQ(std::get<std::int64_t>(res.value->value), -7);
    });
}

TEST(ConstEval, IterativeDeepCastChainFoldsFlatAndByteIdentical) {
    constexpr std::int64_t kDepth = 3000;   // # of nested casts
    dss::substrate::runOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes, [&] {
        Rig r;
        TypeId const i32 = r.intT();
        TypeId const i64 = r.i64T();
        // Alternate (I64)(I32)(I64)…(99) so each level is a real width conversion
        // routed through the single-child Cast frame; the int 99 round-trips
        // unchanged → folds to 99. NOTE (honest witness scope): a cast chain
        // converges to a fixed point, so this pin canNOT detect a dropped Cast
        // level by value (the Cast frame is single-child — there is no operand
        // ORDER to corrupt either). Its job is narrower: drive the Cast frame
        // 3000 levels deep and confirm it folds WITHOUT corruption/crash on the
        // value path (a mis-popped frame would crash or yield garbage, not 99).
        // The exact byte-identity of the Cast arm is covered by the shallow Cast
        // goldens above; the BinaryOp pin is the strong combine-order witness.
        HirNodeId cur = r.litInt(99, i32);
        for (std::int64_t i = 0; i < kDepth; ++i)
            cur = r.cast(cur, (i % 2 == 0) ? i64 : i32);
        Hir hir = r.finishWith(cur);
        auto res = evaluateConstant(hir, r.interner, r.literals, cur);
        ASSERT_TRUE(res.value.has_value()) << "deep Cast chain must fold";
        EXPECT_EQ(std::get<std::int64_t>(res.value->value), 99);
    });
}
