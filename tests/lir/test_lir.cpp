// ML5 cycle 1 — Lir substrate skeleton tests. Builds tiny modules
// via LirBuilder, freezes them, walks the frozen Lir, and asserts
// the cross-arena invariants hold.

#include "core/types/strong_ids.hpp"
#include "lir/lir.hpp"
#include "lir/targets/x86_64.hpp"

#include <gtest/gtest.h>

#include <array>
#include <type_traits>

using namespace dss;
using X86 = dss::x86_64::Opcode;

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

constexpr std::uint16_t op(X86 o) noexcept {
    return static_cast<std::uint16_t>(o);
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
    EXPECT_EQ(l.targetId(), TargetId::Invalid);
    EXPECT_FALSE(l.id().valid());
}

TEST(Lir, MinimalFunctionBuildsAndReads) {
    LirBuilder b{TargetId::X86_64};
    LirFuncId const f = b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const r1 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const movOps{immOperand(42)};
    b.addInst(op(X86::Mov), r1, movOps);
    std::array<LirOperand, 1> const retOps{regOperand(r1)};
    b.addReturn(op(X86::Ret), retOps);
    Lir l = std::move(b).finish();

    EXPECT_EQ(l.targetId(), TargetId::X86_64);
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
    EXPECT_EQ(l.instOpcode(term), op(X86::Ret));
}

TEST(Lir, DiamondCfgWithBranchAndJmp) {
    LirBuilder b{TargetId::X86_64};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    LirBlockId const thenB = b.createBlock();
    LirBlockId const elseB = b.createBlock();
    LirBlockId const join  = b.createBlock();

    b.beginBlock(entry);
    LirReg const r1 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const mov1{immOperand(1)};
    b.addInst(op(X86::Mov), r1, mov1);
    std::array<LirOperand, 2> const cmpOps{regOperand(r1), immOperand(0)};
    b.addInst(op(X86::Cmp), InvalidLirReg, cmpOps);
    b.addCondBr(op(X86::Jcc), std::span<LirOperand const>{}, thenB, elseB);

    b.beginBlock(thenB);
    b.addBr(op(X86::Jmp), join);

    b.beginBlock(elseB);
    b.addBr(op(X86::Jmp), join);

    b.beginBlock(join);
    b.addReturn(op(X86::Ret), std::span<LirOperand const>{});

    Lir l = std::move(b).finish();

    EXPECT_EQ(l.funcBlockCount(l.funcAt(0)), 4u);
    // entry block has 2 successors (then + else)
    auto succs = l.blockSuccessors(entry);
    ASSERT_EQ(succs.size(), 2u);
    EXPECT_EQ(succs[0], thenB);
    EXPECT_EQ(succs[1], elseB);
    // then/else each have 1 successor (join)
    auto thenSuccs = l.blockSuccessors(thenB);
    ASSERT_EQ(thenSuccs.size(), 1u);
    EXPECT_EQ(thenSuccs[0], join);
    // join has 0 successors (return)
    EXPECT_EQ(l.blockSuccessors(join).size(), 0u);
}

TEST(Lir, OpcodeInfoTableMatchesEnumerators) {
    using namespace dss::x86_64;
    EXPECT_EQ(opcodeInfo(Opcode::Mov).mnemonic, "mov");
    EXPECT_EQ(opcodeInfo(Opcode::Add).mnemonic, "add");
    EXPECT_EQ(opcodeInfo(Opcode::Ret).mnemonic, "ret");
    EXPECT_TRUE(opcodeInfo(Opcode::Ret).isTerminator);
    EXPECT_TRUE(opcodeInfo(Opcode::Jmp).isTerminator);
    EXPECT_FALSE(opcodeInfo(Opcode::Mov).isTerminator);
    EXPECT_EQ(opcodeInfo(Opcode::Add).result, LirResultRule::Value);
    EXPECT_EQ(opcodeInfo(Opcode::Cmp).result, LirResultRule::None);
}

TEST(Lir, VirtualRegisterMintingIsMonotonic) {
    LirBuilder b{TargetId::X86_64};
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
    b.addReturn(op(X86::Ret), std::span<LirOperand const>{});
    (void)std::move(b).finish();
}

TEST(Lir, TargetIdRoundTrips) {
    EXPECT_EQ(targetName(TargetId::X86_64), "x86_64");
    EXPECT_EQ(targetName(TargetId::ARM64),  "arm64");
    EXPECT_EQ(targetName(TargetId::Invalid), "invalid");
}

// `LirReg::operator==` ignores the `_pad` bit so future helpers
// that leave it uninitialised don't break equality.
TEST(Lir, LirRegEqualityIgnoresPad) {
    LirReg const a = makeVirtualReg(5, LirRegClass::GPR);
    LirReg b = makeVirtualReg(5, LirRegClass::GPR);
    // Manually flip `_pad` (semantically a no-op).
    b._pad = 1;
    EXPECT_TRUE(a == b)
        << "LirReg equality must not depend on the unused _pad bit";
}

// Death test: calling addBr with a non-terminator opcode (e.g. Mov)
// must abort, not silently corrupt the module.
TEST(LirDeathTest, AddBrRejectsNonTerminatorOpcode) {
    LirBuilder b{TargetId::X86_64};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const e = b.createBlock();
    LirBlockId const t = b.createBlock();
    b.beginBlock(e);
    EXPECT_DEATH(b.addBr(op(X86::Mov), t), "not a terminator");
}

// Death test: calling beginBlock twice on the same block must abort.
TEST(LirDeathTest, BeginBlockTwiceAborts) {
    LirBuilder b{TargetId::X86_64};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const e = b.createBlock();
    LirBlockId const t = b.createBlock();
    b.beginBlock(e);
    b.addBr(op(X86::Jmp), t);  // close e
    b.beginBlock(t);
    b.addReturn(op(X86::Ret), std::span<LirOperand const>{});
    EXPECT_DEATH(b.beginBlock(e), "already been opened");
}

// Death test: a block that never gets a terminator must abort at
// closeFunction (called by addFunction or finish).
TEST(LirDeathTest, BlockWithoutTerminatorAborts) {
    LirBuilder b{TargetId::X86_64};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const e = b.createBlock();
    b.beginBlock(e);
    LirReg const r = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const ops{immOperand(1)};
    b.addInst(op(X86::Mov), r, ops);
    // No terminator emitted.
    EXPECT_DEATH(std::move(b).finish(), "not a terminator");
}

TEST(Lir, LirAttributeBindsToInstructionTier) {
    LirBuilder b{TargetId::X86_64};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const r1 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const movOps{immOperand(7)};
    LirInstId const movInst = b.addInst(op(X86::Mov), r1, movOps);
    b.addReturn(op(X86::Ret), std::span<LirOperand const>{});
    Lir l = std::move(b).finish();

    // Side-table proves the substrate `Arena` concept binding works.
    LirAttribute<int> attrs{l};
    attrs.set(movInst, 42);
    EXPECT_EQ(*attrs.tryGet(movInst), 42);
}
