// ML5 cycle 2 — Lir substrate tests with JSON-loaded target schemas.
// Cycle 2 pivoted from C++ enum targets (`targets/x86_64.hpp`) to JSON
// config files (`src/dss-config/targets/*.target.json`). The test
// driver loads the x86_64 schema once at class fixture setup and uses
// mnemonic lookups instead of an enum.

#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <type_traits>

using namespace dss;

namespace {

LirOperand regOperand(LirReg r) {
    LirOperand o;
    o.kind = LirOperandKind::Reg;
    o.reg  = r;
    return o;
}

LirOperand immOperand(std::int32_t v) {
    LirOperand o;
    o.kind     = LirOperandKind::ImmInt;
    o.immInt32 = v;
    return o;
}

// Shared schema fixture: load once per test binary process.
std::shared_ptr<TargetSchema> const& x86Schema() {
    static std::shared_ptr<TargetSchema> const schema = [] {
        auto r = TargetSchema::loadShipped("x86_64");
        if (!r.has_value()) {
            std::abort();  // Misconfigured test environment
        }
        return *r;
    }();
    return schema;
}

// Mnemonic → opcode integer lookup; aborts on unknown to surface
// test typos loudly.
std::uint16_t op(std::string_view mnemonic) {
    auto const& s = *x86Schema();
    auto const i = s.opcodeByMnemonic(mnemonic);
    if (!i.has_value()) std::abort();
    return *i;
}

} // namespace

// Move-only, like Mir.
static_assert(!std::is_copy_constructible_v<Lir>);
static_assert(std::is_move_constructible_v<Lir>);
static_assert(!std::is_copy_constructible_v<LirBuilder>);

TEST(Lir, DefaultModuleIsEmpty) {
    Lir l;
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.instCount(), 0u);
    EXPECT_EQ(l.moduleFuncCount(), 0u);
    EXPECT_FALSE(l.id().valid());
}

TEST(Lir, MinimalFunctionBuildsAndReads) {
    LirBuilder b{*x86Schema()};
    LirFuncId const f = b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const r1 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const movOps{immOperand(42)};
    b.addInst(op("mov"), r1, movOps);
    std::array<LirOperand, 1> const retOps{regOperand(r1)};
    b.addReturn(op("ret"), retOps);
    Lir l = std::move(b).finish();

    ASSERT_EQ(l.moduleFuncCount(), 1u);
    LirFuncId const f0 = l.funcAt(0);
    EXPECT_EQ(f0, f);
    EXPECT_EQ(l.funcSymbol(f0).v, 1u);
    EXPECT_EQ(l.funcBlockCount(f0), 1u);
    EXPECT_EQ(l.funcNumVRegs(f0), 1u);

    LirBlockId const entryReturned = l.funcEntry(f0);
    EXPECT_EQ(entryReturned, entry);
    EXPECT_EQ(l.blockInstCount(entryReturned), 2u);  // mov + ret
    LirInstId const term = l.blockTerminator(entryReturned);
    EXPECT_EQ(l.instOpcode(term), op("ret"));
}

TEST(Lir, DiamondCfgWithBranchAndJmp) {
    LirBuilder b{*x86Schema()};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    LirBlockId const thenB = b.createBlock();
    LirBlockId const elseB = b.createBlock();
    LirBlockId const join  = b.createBlock();

    b.beginBlock(entry);
    LirReg const r1 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const mov1{immOperand(1)};
    b.addInst(op("mov"), r1, mov1);
    std::array<LirOperand, 2> const cmpOps{regOperand(r1), immOperand(0)};
    b.addInst(op("cmp"), InvalidLirReg, cmpOps);
    b.addCondBr(op("jcc"), std::span<LirOperand const>{}, thenB, elseB);

    b.beginBlock(thenB);
    b.addBr(op("jmp"), join);

    b.beginBlock(elseB);
    b.addBr(op("jmp"), join);

    b.beginBlock(join);
    b.addReturn(op("ret"), std::span<LirOperand const>{});

    Lir l = std::move(b).finish();

    EXPECT_EQ(l.funcBlockCount(l.funcAt(0)), 4u);
    auto succs = l.blockSuccessors(entry);
    ASSERT_EQ(succs.size(), 2u);
    EXPECT_EQ(succs[0], thenB);
    EXPECT_EQ(succs[1], elseB);
    auto thenSuccs = l.blockSuccessors(thenB);
    ASSERT_EQ(thenSuccs.size(), 1u);
    EXPECT_EQ(thenSuccs[0], join);
    EXPECT_EQ(l.blockSuccessors(join).size(), 0u);
}

TEST(Lir, TargetSchemaLoadsX86_64FromJson) {
    auto r = TargetSchema::loadShipped("x86_64");
    if (!r.has_value()) {
        for (auto const& d : r.error()) {
            ADD_FAILURE() << "diag: " << d.path << ": " << d.message;
        }
    }
    ASSERT_TRUE(r.has_value());
    auto const& s = **r;
    EXPECT_EQ(s.name(), "x86_64");
    EXPECT_GT(s.opcodeCount(), 1u);
    EXPECT_TRUE(s.id().valid());
    // Slot 0 is the Invalid sentinel.
    auto const* slot0 = s.opcodeInfo(0);
    ASSERT_NE(slot0, nullptr);
    EXPECT_EQ(slot0->mnemonic, "invalid");
    EXPECT_FALSE(slot0->isTerminator);
    // ret is a terminator.
    auto retIdx = s.opcodeByMnemonic("ret");
    ASSERT_TRUE(retIdx.has_value());
    EXPECT_TRUE(s.isTerminator(*retIdx));
    // mov is value-producing, not a terminator.
    auto movIdx = s.opcodeByMnemonic("mov");
    ASSERT_TRUE(movIdx.has_value());
    EXPECT_EQ(s.opcodeInfo(*movIdx)->result, TargetResultRule::Value);
    EXPECT_FALSE(s.isTerminator(*movIdx));
}

TEST(Lir, TargetSchemaUnknownMnemonicReturnsNullopt) {
    auto const& s = *x86Schema();
    EXPECT_FALSE(s.opcodeByMnemonic("nosuchop").has_value());
}

TEST(Lir, VirtualRegisterMintingIsMonotonic) {
    LirBuilder b{*x86Schema()};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const a = b.newVReg(LirRegClass::GPR);
    LirReg const b2 = b.newVReg(LirRegClass::GPR);
    LirReg const c = b.newVReg(LirRegClass::FPR);
    EXPECT_EQ(a.id, 1u);
    EXPECT_EQ(b2.id, 2u);
    EXPECT_EQ(c.id, 3u);
    EXPECT_FALSE(a.isPhysical);
    EXPECT_EQ(a.regClass(), LirRegClass::GPR);
    EXPECT_EQ(c.regClass(), LirRegClass::FPR);
    b.addReturn(op("ret"), std::span<LirOperand const>{});
    (void)std::move(b).finish();
}

TEST(Lir, LirRegEqualityIgnoresPad) {
    LirReg const a = makeVirtualReg(5, LirRegClass::GPR);
    LirReg b = makeVirtualReg(5, LirRegClass::GPR);
    b._pad = 1;
    EXPECT_TRUE(a == b);
}

TEST(LirDeathTest, AddBrRejectsNonTerminatorOpcode) {
    LirBuilder b{*x86Schema()};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const e = b.createBlock();
    LirBlockId const t = b.createBlock();
    b.beginBlock(e);
    EXPECT_DEATH(b.addBr(op("mov"), t), "not a terminator");
}

TEST(LirDeathTest, BeginBlockTwiceAborts) {
    LirBuilder b{*x86Schema()};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const e = b.createBlock();
    LirBlockId const t = b.createBlock();
    b.beginBlock(e);
    b.addBr(op("jmp"), t);
    b.beginBlock(t);
    b.addReturn(op("ret"), std::span<LirOperand const>{});
    EXPECT_DEATH(b.beginBlock(e), "already been opened");
}

TEST(LirDeathTest, BlockWithoutTerminatorAborts) {
    LirBuilder b{*x86Schema()};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const e = b.createBlock();
    b.beginBlock(e);
    LirReg const r = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const ops{immOperand(1)};
    b.addInst(op("mov"), r, ops);
    EXPECT_DEATH(std::move(b).finish(), "not a terminator");
}

TEST(Lir, LirAttributeBindsToInstructionTier) {
    LirBuilder b{*x86Schema()};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const r1 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const movOps{immOperand(7)};
    LirInstId const movInst = b.addInst(op("mov"), r1, movOps);
    b.addReturn(op("ret"), std::span<LirOperand const>{});
    Lir l = std::move(b).finish();

    LirAttribute<int> attrs{l};
    attrs.set(movInst, 42);
    EXPECT_EQ(*attrs.tryGet(movInst), 42);
}
