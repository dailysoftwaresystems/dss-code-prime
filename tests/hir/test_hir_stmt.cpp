// HR3: typed structured-CF statement helpers + the read accessors that hide
// each kind's child layout / payload encoding (optional positions, the ForStmt
// clause mask, the CaseArm default flag).

#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/hir_node.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <vector>

using dss::clauseCount;
using dss::CompilationUnitId;
using dss::ForClause;
using dss::has;
using dss::Hir;
using dss::HirBuilder;
using dss::HirKind;
using dss::HirNodeId;
using dss::SymbolId;
using dss::TypeId;
using dss::TypeInterner;
using dss::TypeKind;

namespace {

TypeInterner makeInterner() { return TypeInterner{CompilationUnitId{1}}; }

} // namespace

TEST(HirStmt, BlockHoldsStatementsInOrder) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirNodeId const s0 = b.makeExprStmt(b.makeLiteral(i32));
    HirNodeId const s1 = b.makeExprStmt(b.makeLiteral(i32));
    HirNodeId const blk = b.makeBlock(std::array{s0, s1});
    Hir h = std::move(b).finish(blk);

    EXPECT_EQ(h.kind(blk), HirKind::Block);
    ASSERT_EQ(h.children(blk).size(), 2u);
    EXPECT_EQ(h.children(blk)[0], s0);
    EXPECT_EQ(h.children(blk)[1], s1);
}

TEST(HirStmt, IfWithoutElse) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const boolT = ti.primitive(TypeKind::Bool);
    HirNodeId const cond = b.makeLiteral(boolT);
    HirNodeId const then = b.makeBlock({});
    HirNodeId const iff  = b.makeIfStmt(cond, then);
    Hir h = std::move(b).finish(iff);

    EXPECT_EQ(h.kind(iff), HirKind::IfStmt);
    EXPECT_EQ(h.ifCondition(iff), cond);
    EXPECT_EQ(h.ifThen(iff), then);
    EXPECT_FALSE(h.ifElse(iff).has_value());
    EXPECT_EQ(h.children(iff).size(), 2u);
}

TEST(HirStmt, IfWithElse) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const boolT = ti.primitive(TypeKind::Bool);
    HirNodeId const cond = b.makeLiteral(boolT);
    HirNodeId const then = b.makeBlock({});
    HirNodeId const els  = b.makeBlock({});
    HirNodeId const iff  = b.makeIfStmt(cond, then, els);
    Hir h = std::move(b).finish(iff);

    ASSERT_TRUE(h.ifElse(iff).has_value());
    EXPECT_EQ(*h.ifElse(iff), els);
    EXPECT_EQ(h.children(iff).size(), 3u);
}

TEST(HirStmt, WhileAndDoWhileBodyConditionAccessors) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const boolT = ti.primitive(TypeKind::Bool);
    HirNodeId const wc = b.makeLiteral(boolT);
    HirNodeId const wb = b.makeBlock({});
    HirNodeId const wh = b.makeWhileStmt(wc, wb);
    HirNodeId const dc = b.makeLiteral(boolT);
    HirNodeId const db = b.makeBlock({});
    HirNodeId const dw = b.makeDoWhileStmt(db, dc);
    HirNodeId const blk = b.makeBlock(std::array{wh, dw});
    Hir h = std::move(b).finish(blk);

    // Kind-agnostic loop accessors return the right child for both orderings.
    EXPECT_EQ(h.loopBody(wh), wb);
    ASSERT_TRUE(h.loopCondition(wh).has_value());
    EXPECT_EQ(*h.loopCondition(wh), wc);
    EXPECT_EQ(h.loopBody(dw), db);          // do-while stores body first
    ASSERT_TRUE(h.loopCondition(dw).has_value());
    EXPECT_EQ(*h.loopCondition(dw), dc);
    // Raw child order differs by kind (cond-first vs body-first).
    EXPECT_EQ(h.children(wh)[0], wc);
    EXPECT_EQ(h.children(dw)[0], db);
}

TEST(HirStmt, ForAllClausesPresent) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    TypeId const boolT = ti.primitive(TypeKind::Bool);
    HirNodeId const init   = b.makeVarDecl(i32, /*symbol=*/1, b.makeLiteral(i32));
    HirNodeId const cond   = b.makeLiteral(boolT);
    HirNodeId const update = b.makeExprStmt(b.makeLiteral(i32));
    HirNodeId const body   = b.makeBlock({});
    HirNodeId const f = b.makeForStmt(init, cond, update, body);
    Hir h = std::move(b).finish(f);

    EXPECT_EQ(h.kind(f), HirKind::ForStmt);
    EXPECT_EQ(h.children(f).size(), 4u);
    EXPECT_TRUE(has(h.forClauses(f), ForClause::Init));
    EXPECT_TRUE(has(h.forClauses(f), ForClause::Cond));
    EXPECT_TRUE(has(h.forClauses(f), ForClause::Update));
    ASSERT_TRUE(h.forInit(f).has_value());   EXPECT_EQ(*h.forInit(f), init);
    ASSERT_TRUE(h.loopCondition(f).has_value()); EXPECT_EQ(*h.loopCondition(f), cond);
    ASSERT_TRUE(h.forUpdate(f).has_value()); EXPECT_EQ(*h.forUpdate(f), update);
    EXPECT_EQ(h.loopBody(f), body);
}

TEST(HirStmt, ForCondOnlyMapsClausesCorrectly) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const boolT = ti.primitive(TypeKind::Bool);
    HirNodeId const cond = b.makeLiteral(boolT);
    HirNodeId const body = b.makeBlock({});
    HirNodeId const f = b.makeForStmt(std::nullopt, cond, std::nullopt, body);
    Hir h = std::move(b).finish(f);

    EXPECT_EQ(h.children(f).size(), 2u);      // cond + body only
    EXPECT_EQ(clauseCount(h.forClauses(f)), 1u);
    EXPECT_FALSE(h.forInit(f).has_value());
    EXPECT_FALSE(h.forUpdate(f).has_value());
    ASSERT_TRUE(h.loopCondition(f).has_value());
    EXPECT_EQ(*h.loopCondition(f), cond);     // not mis-mapped to the body
    EXPECT_EQ(h.loopBody(f), body);
}

TEST(HirStmt, ForInitAndUpdateButNoCond) {
    // The index-arithmetic edge: a MIDDLE clause (cond) absent. forUpdate must
    // resolve to children[1] (after init at 0), NOT children[2].
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirNodeId const init   = b.makeVarDecl(i32, /*symbol=*/1, b.makeLiteral(i32));
    HirNodeId const update = b.makeExprStmt(b.makeLiteral(i32));
    HirNodeId const body   = b.makeBlock({});
    HirNodeId const f = b.makeForStmt(init, std::nullopt, update, body);
    Hir h = std::move(b).finish(f);

    ASSERT_EQ(h.children(f).size(), 3u);          // init, update, body
    EXPECT_EQ(clauseCount(h.forClauses(f)), 2u);
    ASSERT_TRUE(h.forInit(f).has_value());    EXPECT_EQ(*h.forInit(f), init);
    EXPECT_FALSE(h.loopCondition(f).has_value());
    ASSERT_TRUE(h.forUpdate(f).has_value());  EXPECT_EQ(*h.forUpdate(f), update);  // children[1]
    EXPECT_EQ(h.loopBody(f), body);
    EXPECT_EQ(h.children(f)[1], update);          // not mis-shifted by the absent cond
}

TEST(HirStmt, ForNoClausesIsBodyOnly) {
    HirBuilder b{"toy"};
    HirNodeId const body = b.makeBlock({});
    HirNodeId const f = b.makeForStmt(std::nullopt, std::nullopt, std::nullopt, body);
    Hir h = std::move(b).finish(f);

    EXPECT_EQ(h.children(f).size(), 1u);
    EXPECT_EQ(clauseCount(h.forClauses(f)), 0u);
    EXPECT_FALSE(h.loopCondition(f).has_value());
    EXPECT_EQ(h.loopBody(f), body);
}

TEST(HirStmt, SwitchWithValuedAndDefaultArms) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirNodeId const disc = b.makeRef(i32, 1);
    HirNodeId const caseVal = b.makeLiteral(i32);
    HirNodeId const caseBody = b.makeExprStmt(b.makeLiteral(i32));
    HirNodeId const arm0 = b.makeCaseArm(caseVal, std::array{caseBody});
    HirNodeId const defBody = b.makeExprStmt(b.makeLiteral(i32));
    HirNodeId const arm1 = b.makeCaseArm(std::nullopt, std::array{defBody});
    HirNodeId const sw = b.makeSwitchStmt(disc, std::array{arm0, arm1});
    Hir h = std::move(b).finish(sw);

    EXPECT_EQ(h.switchDiscriminant(sw), disc);
    ASSERT_EQ(h.switchArms(sw).size(), 2u);
    EXPECT_EQ(h.switchArms(sw)[0], arm0);

    // Valued arm: not default, value present, body is the rest.
    EXPECT_FALSE(h.caseArmIsDefault(arm0));
    ASSERT_TRUE(h.caseArmValue(arm0).has_value());
    EXPECT_EQ(*h.caseArmValue(arm0), caseVal);
    ASSERT_EQ(h.caseArmBody(arm0).size(), 1u);
    EXPECT_EQ(h.caseArmBody(arm0)[0], caseBody);

    // Default arm: flagged default, no value, all children are body.
    EXPECT_TRUE(h.caseArmIsDefault(arm1));
    EXPECT_FALSE(h.caseArmValue(arm1).has_value());
    ASSERT_EQ(h.caseArmBody(arm1).size(), 1u);
    EXPECT_EQ(h.caseArmBody(arm1)[0], defBody);
}

TEST(HirStmt, BreakContinueCarryDepthAsLeaves) {
    HirBuilder b{"toy"};
    HirNodeId const br = b.makeBreak(2);
    HirNodeId const co = b.makeContinue();   // default depth 0
    HirNodeId const blk = b.makeBlock(std::array{br, co});
    Hir h = std::move(b).finish(blk);

    EXPECT_EQ(h.kind(br), HirKind::BreakStmt);
    EXPECT_TRUE(h.children(br).empty());
    EXPECT_EQ(h.branchDepth(br), 2u);
    EXPECT_EQ(h.kind(co), HirKind::ContinueStmt);
    EXPECT_EQ(h.branchDepth(co), 0u);
}

TEST(HirStmt, ReturnWithAndWithoutValue) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirNodeId const v = b.makeLiteral(i32);
    HirNodeId const r0 = b.makeReturn(v);
    HirNodeId const r1 = b.makeReturn();           // bare return;
    HirNodeId const blk = b.makeBlock(std::array{r0, r1});
    Hir h = std::move(b).finish(blk);

    ASSERT_TRUE(h.returnValue(r0).has_value());
    EXPECT_EQ(*h.returnValue(r0), v);
    EXPECT_FALSE(h.returnValue(r1).has_value());
    EXPECT_TRUE(h.children(r1).empty());
}

TEST(HirStmt, VarDeclCarriesTypeSymbolAndOptionalInit) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirNodeId const init = b.makeLiteral(i32);
    HirNodeId const vd   = b.makeVarDecl(i32, /*symbol=*/42, init);
    HirNodeId const vd2  = b.makeVarDecl(i32, /*symbol=*/43);   // no initializer
    HirNodeId const blk  = b.makeBlock(std::array{vd, vd2});
    Hir h = std::move(b).finish(blk);

    EXPECT_EQ(h.varDeclType(vd), i32);                 // declared type rides in typeId
    EXPECT_EQ(h.varDeclSymbol(vd), SymbolId{42});
    ASSERT_TRUE(h.varDeclInit(vd).has_value());
    EXPECT_EQ(*h.varDeclInit(vd), init);
    EXPECT_FALSE(h.varDeclInit(vd2).has_value());
}

TEST(HirStmt, ExprStmtWrapsItsExpression) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirNodeId const inner = b.makeLiteral(i32);
    HirNodeId const es = b.makeExprStmt(inner);
    Hir h = std::move(b).finish(es);

    EXPECT_EQ(h.kind(es), HirKind::ExprStmt);
    EXPECT_EQ(h.exprStmtExpr(es), inner);
    EXPECT_EQ(h.children(es).size(), 1u);
}

TEST(HirStmt, AssignTargetAndValue) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    HirNodeId const target = b.makeRef(i32, 1);
    HirNodeId const value  = b.makeLiteral(i32);
    HirNodeId const asg = b.makeAssignStmt(target, value);
    Hir h = std::move(b).finish(asg);

    EXPECT_EQ(h.kind(asg), HirKind::AssignStmt);
    EXPECT_EQ(h.assignTarget(asg), target);
    EXPECT_EQ(h.assignValue(asg), value);
}

// Cross-check: every statement helper produces a node whose child count obeys
// the childArity single source of truth (so the verifier's arity rule and the
// helpers can never silently disagree).
TEST(HirStmt, HelpersSatisfyChildAritySpec) {
    HirBuilder b{"toy"};
    TypeInterner ti = makeInterner();
    TypeId const i32 = ti.primitive(TypeKind::I32);
    auto lit = [&] { return b.makeLiteral(i32); };

    std::vector<HirNodeId> nodes;
    nodes.push_back(b.makeBlock({}));
    nodes.push_back(b.makeIfStmt(lit(), b.makeBlock({})));
    nodes.push_back(b.makeWhileStmt(lit(), b.makeBlock({})));
    nodes.push_back(b.makeDoWhileStmt(b.makeBlock({}), lit()));
    nodes.push_back(b.makeForStmt(std::nullopt, lit(), std::nullopt, b.makeBlock({})));
    nodes.push_back(b.makeSwitchStmt(lit(), {}));
    nodes.push_back(b.makeBreak(0));
    nodes.push_back(b.makeContinue(0));
    nodes.push_back(b.makeReturn());
    nodes.push_back(b.makeExprStmt(lit()));
    nodes.push_back(b.makeVarDecl(i32, 1));
    nodes.push_back(b.makeAssignStmt(b.makeRef(i32, 2), lit()));
    HirNodeId const root = b.makeBlock(nodes);
    Hir h = std::move(b).finish(root);

    for (HirNodeId n : nodes) {
        auto const a = dss::childArity(h.kind(n));
        auto const c = static_cast<std::uint32_t>(h.children(n).size());
        EXPECT_GE(c, a.min) << "kind ordinal " << static_cast<unsigned>(h.kind(n));
        if (a.max != dss::kUnboundedArity) {
            EXPECT_LE(c, a.max) << "kind ordinal " << static_cast<unsigned>(h.kind(n));
        }
    }
}
