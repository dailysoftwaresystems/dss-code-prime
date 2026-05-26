// HR2: the typed expression builder helpers on HirBuilder — each fixes its
// kind's child arity + payload convention, and the BinaryOp/UnaryOp helpers
// assert operator arity at construction.

#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/hir_op.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

using dss::CompilationUnitId;
using dss::encodeOp;
using dss::Hir;
using dss::HirBuilder;
using dss::HirKind;
using dss::HirNodeId;
using dss::HirOpArity;
using dss::HirOpId;
using dss::HirOpKind;
using dss::isCoreOp;
using dss::TypeId;
using dss::TypeInterner;
using dss::TypeKind;

namespace {

// A self-contained interner — any nonzero CompilationUnitId tags its TypeIds.
TypeInterner makeInterner() { return TypeInterner{CompilationUnitId{1}}; }

} // namespace

TEST(HirExpr, BinaryOpHasTwoChildrenAndOperatorPayload) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirBuilder b{"toy"};
    HirNodeId const lhs = b.makeLiteral(i32, /*literalIndex=*/1);
    HirNodeId const rhs = b.makeLiteral(i32, /*literalIndex=*/2);
    HirNodeId const add = b.makeBinaryOp(HirOpKind::Add, lhs, rhs, i32);
    Hir h = std::move(b).finish(add);

    EXPECT_EQ(h.kind(add), HirKind::BinaryOp);
    EXPECT_EQ(h.typeId(add), i32);
    EXPECT_EQ(h.payload(add), encodeOp(HirOpKind::Add));
    EXPECT_TRUE(isCoreOp(h.payload(add)));
    auto kids = h.children(add);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(kids[0], lhs);
    EXPECT_EQ(kids[1], rhs);
}

TEST(HirExpr, UnaryOpHasOneChild) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirBuilder b{"toy"};
    HirNodeId const operand = b.makeLiteral(i32);
    HirNodeId const neg     = b.makeUnaryOp(HirOpKind::Neg, operand, i32);
    Hir h = std::move(b).finish(neg);

    EXPECT_EQ(h.kind(neg), HirKind::UnaryOp);
    EXPECT_EQ(h.payload(neg), encodeOp(HirOpKind::Neg));
    EXPECT_EQ(h.children(neg).size(), 1u);
}

TEST(HirExpr, CallChildrenAreCalleeThenArgs) {
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, dss::CallConv::CcSysV);

    HirBuilder b{"toy"};
    HirNodeId const callee = b.makeRef(fnTy, /*symbol=*/3);
    HirNodeId const arg0   = b.makeLiteral(i32);
    HirNodeId const call   = b.makeCall(callee, std::array{arg0}, i32);
    Hir h = std::move(b).finish(call);

    EXPECT_EQ(h.kind(call), HirKind::Call);
    EXPECT_EQ(h.typeId(call), i32);
    auto kids = h.children(call);
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(kids[0], callee);   // callee first
    EXPECT_EQ(kids[1], arg0);     // then args
}

TEST(HirExpr, TypeRefIsTypedLeaf) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirBuilder b{"toy"};
    HirNodeId const tr = b.makeTypeRef(i32);
    Hir h = std::move(b).finish(tr);

    EXPECT_EQ(h.kind(tr), HirKind::TypeRef);
    EXPECT_EQ(h.typeId(tr), i32);
    EXPECT_TRUE(h.children(tr).empty());
}

TEST(HirExpr, ExtensionOperatorEncodesRegistryId) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirBuilder b{"L"};
    HirOpId const spaceship = b.opRegistry().registerExtension("L::Spaceship",
                                                               HirOpArity::Binary, "L");
    HirNodeId const lhs = b.makeLiteral(i32);
    HirNodeId const rhs = b.makeLiteral(i32);
    HirNodeId const cmp = b.makeBinaryOp(spaceship, lhs, rhs, i32);
    Hir h = std::move(b).finish(cmp);

    EXPECT_EQ(h.kind(cmp), HirKind::BinaryOp);
    EXPECT_EQ(h.payload(cmp), spaceship.v);
    EXPECT_FALSE(isCoreOp(h.payload(cmp)));
    EXPECT_EQ(h.opRegistry().descriptor(spaceship).name(), "L::Spaceship");
}

TEST(HirExpr, PayloadCarryingHelpersStashTheirScalar) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirBuilder b{"toy"};
    // IntrinsicCall: children are [args...]; payload is the intrinsic id.
    HirNodeId const ia = b.makeLiteral(i32);
    HirNodeId const intr = b.makeIntrinsicCall(/*intrinsicId=*/7, std::array{ia}, i32);
    // MemberAccess: one child [base]; payload is the field index.
    HirNodeId const base = b.makeRef(i32, 1);
    HirNodeId const mem  = b.makeMemberAccess(base, /*fieldIndex=*/3, i32);
    // Swizzle: one child [base]; payload is the component mask.
    HirNodeId const vbase = b.makeRef(i32, 2);
    HirNodeId const swz   = b.makeSwizzle(vbase, /*componentMask=*/0b1100, i32);
    HirNodeId const blk = b.addParent(HirKind::Block, std::array{intr, mem, swz});
    Hir h = std::move(b).finish(blk);

    EXPECT_EQ(h.kind(intr), HirKind::IntrinsicCall);
    EXPECT_EQ(h.payload(intr), 7u);
    EXPECT_EQ(h.children(intr).size(), 1u);
    EXPECT_EQ(h.kind(mem), HirKind::MemberAccess);
    EXPECT_EQ(h.payload(mem), 3u);
    EXPECT_EQ(h.children(mem).size(), 1u);
    EXPECT_EQ(h.kind(swz), HirKind::Swizzle);
    EXPECT_EQ(h.payload(swz), 0b1100u);
}

TEST(HirExpr, SingleChildHelpersWrapTheirOperand) {
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);

    HirBuilder b{"toy"};
    HirNodeId const c1 = b.makeLiteral(i32);
    HirNodeId const cast = b.makeCast(c1, i32);
    HirNodeId const c2 = b.makeRef(i32, 1);
    HirNodeId const addr = b.makeAddressOf(c2, i32);
    HirNodeId const c3 = b.makeRef(i32, 2);
    HirNodeId const der = b.makeDeref(c3, i32);
    HirNodeId const tr  = b.makeTypeRef(i32);
    HirNodeId const so  = b.makeSizeOf(tr, i32);
    HirNodeId const blk = b.addParent(HirKind::Block, std::array{cast, addr, der, so});
    Hir h = std::move(b).finish(blk);

    EXPECT_EQ(h.kind(cast), HirKind::Cast);
    EXPECT_EQ(h.children(cast).size(), 1u);
    EXPECT_EQ(h.kind(addr), HirKind::AddressOf);
    EXPECT_EQ(h.children(addr).size(), 1u);
    EXPECT_EQ(h.kind(der), HirKind::Deref);
    EXPECT_EQ(h.children(der).size(), 1u);
    EXPECT_EQ(h.kind(so), HirKind::SizeOf);
    ASSERT_EQ(h.children(so).size(), 1u);
    EXPECT_EQ(h.children(so)[0], tr);   // SizeOf wraps a TypeRef
}

TEST(HirExpr, MultiChildHelpersOrderChildren) {
    TypeInterner ti = makeInterner();
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const boolT = ti.primitive(TypeKind::Bool);

    HirBuilder b{"toy"};
    // Index: [base, index].
    HirNodeId const ibase = b.makeRef(i32, 1);
    HirNodeId const idx   = b.makeLiteral(i32);
    HirNodeId const index = b.makeIndex(ibase, idx, i32);
    // Ternary: [cond, then, else].
    HirNodeId const cond = b.makeLiteral(boolT);
    HirNodeId const thn  = b.makeLiteral(i32);
    HirNodeId const els  = b.makeLiteral(i32);
    HirNodeId const tern = b.makeTernary(cond, thn, els, i32);
    // LogicalAnd / LogicalOr: [lhs, rhs].
    HirNodeId const la = b.makeLiteral(boolT);
    HirNodeId const lb = b.makeLiteral(boolT);
    HirNodeId const land = b.makeLogicalAnd(la, lb, boolT);
    HirNodeId const lc = b.makeLiteral(boolT);
    HirNodeId const ld = b.makeLiteral(boolT);
    HirNodeId const lor = b.makeLogicalOr(lc, ld, boolT);
    // ConstructAggregate: [fields...].
    HirNodeId const f0 = b.makeLiteral(i32);
    HirNodeId const f1 = b.makeLiteral(i32);
    HirNodeId const agg = b.makeConstructAggregate(std::array{f0, f1}, i32);
    HirNodeId const blk = b.addParent(HirKind::Block, std::array{index, tern, land, lor, agg});
    Hir h = std::move(b).finish(blk);

    EXPECT_EQ(h.kind(index), HirKind::Index);
    EXPECT_EQ(h.children(index)[0], ibase);
    EXPECT_EQ(h.children(index)[1], idx);
    EXPECT_EQ(h.kind(tern), HirKind::Ternary);
    ASSERT_EQ(h.children(tern).size(), 3u);
    EXPECT_EQ(h.children(tern)[0], cond);
    EXPECT_EQ(h.children(tern)[1], thn);
    EXPECT_EQ(h.children(tern)[2], els);
    EXPECT_EQ(h.kind(land), HirKind::LogicalAnd);
    EXPECT_EQ(h.children(land).size(), 2u);
    EXPECT_EQ(h.kind(lor), HirKind::LogicalOr);
    EXPECT_EQ(h.children(lor).size(), 2u);
    EXPECT_EQ(h.kind(agg), HirKind::ConstructAggregate);
    EXPECT_EQ(h.children(agg).size(), 2u);
}

// ── construction-time arity guards (death tests) ──

TEST(HirExprDeathTest, BinaryHelperRejectsUnaryOperator) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirBuilder b{"toy"};
    HirNodeId const a = b.makeLiteral(i32);
    HirNodeId const c = b.makeLiteral(i32);
    EXPECT_DEATH({ (void)b.makeBinaryOp(HirOpKind::Neg, a, c, i32); },
                 "operator 'Neg' is unary but was used to build a binary expression node");
}

TEST(HirExprDeathTest, UnaryHelperRejectsBinaryOperator) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirBuilder b{"toy"};
    HirNodeId const a = b.makeLiteral(i32);
    EXPECT_DEATH({ (void)b.makeUnaryOp(HirOpKind::Add, a, i32); },
                 "operator 'Add' is binary but was used to build a unary expression node");
}

TEST(HirExprDeathTest, ExtensionBinaryHelperRejectsUnaryExtensionOp) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirBuilder b{"L"};
    HirOpId const rot = b.opRegistry().registerExtension("L::Rotate", HirOpArity::Unary, "L");
    HirNodeId const a = b.makeLiteral(i32);
    HirNodeId const c = b.makeLiteral(i32);
    EXPECT_DEATH({ (void)b.makeBinaryOp(rot, a, c, i32); },
                 "operator 'L::Rotate' is unary but was used to build a binary expression node");
}

TEST(HirExprDeathTest, ExtensionUnaryHelperRejectsBinaryExtensionOp) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirBuilder b{"L"};
    HirOpId const cmp = b.opRegistry().registerExtension("L::Spaceship", HirOpArity::Binary, "L");
    HirNodeId const a = b.makeLiteral(i32);
    EXPECT_DEATH({ (void)b.makeUnaryOp(cmp, a, i32); },
                 "operator 'L::Spaceship' is binary but was used to build a unary expression node");
}
