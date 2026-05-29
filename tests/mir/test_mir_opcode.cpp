// ML1 — MirOpcode vocabulary + descriptor table.

#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace dss;

namespace {

// Every opcode ordinal in [0, Count_) — handy for table sweeps.
constexpr std::uint32_t kOpcodeCount = static_cast<std::uint32_t>(MirOpcode::Count_);

bool isOneOfTheFiveTerminators(MirOpcode op) {
    return op == MirOpcode::Br || op == MirOpcode::CondBr || op == MirOpcode::Switch
        || op == MirOpcode::Return || op == MirOpcode::Unreachable;
}

} // namespace

// The enum is a closed uint16_t vocabulary.
static_assert(static_cast<std::uint32_t>(MirOpcode::Count_) <= 0xFFFF);

TEST(MirOpcode, EveryOpcodeHasADescriptorWithAMnemonic) {
    for (std::uint32_t i = 0; i < kOpcodeCount; ++i) {
        auto const op = static_cast<MirOpcode>(i);
        MirOpcodeInfo const info = opcodeInfo(op);
        // The descriptor switch covers every enumerator: the fall-through
        // sentinel returns "?" and impossible {1,0} arity, which no real opcode
        // should hit.
        EXPECT_NE(info.mnemonic, "?") << "opcode ordinal " << i << " has no descriptor row";
        EXPECT_FALSE(info.mnemonic.empty());
    }
}

TEST(MirOpcode, ExactlyFiveTerminators) {
    int terminatorCount = 0;
    for (std::uint32_t i = 0; i < kOpcodeCount; ++i) {
        auto const op = static_cast<MirOpcode>(i);
        if (isTerminator(op)) {
            ++terminatorCount;
            EXPECT_TRUE(isOneOfTheFiveTerminators(op))
                << "unexpected terminator: " << opcodeInfo(op).mnemonic;
        } else {
            EXPECT_FALSE(isOneOfTheFiveTerminators(op));
        }
    }
    EXPECT_EQ(terminatorCount, 5);
}

TEST(MirOpcode, OnlyPhiUsesThePhiPool) {
    for (std::uint32_t i = 0; i < kOpcodeCount; ++i) {
        auto const op = static_cast<MirOpcode>(i);
        EXPECT_EQ(opcodeInfo(op).usesPhiPool, op == MirOpcode::Phi);
        EXPECT_EQ(isPhi(op), op == MirOpcode::Phi);
    }
}

TEST(MirOpcode, ResultRules) {
    // Value-producing computations require a result type.
    EXPECT_EQ(resultRule(MirOpcode::Add), MirResultRule::Value);
    EXPECT_EQ(resultRule(MirOpcode::Const), MirResultRule::Value);
    EXPECT_EQ(resultRule(MirOpcode::Arg), MirResultRule::Value);
    EXPECT_EQ(resultRule(MirOpcode::GlobalAddr), MirResultRule::Value);
    EXPECT_EQ(resultRule(MirOpcode::Load), MirResultRule::Value);
    EXPECT_EQ(resultRule(MirOpcode::ICmpEq), MirResultRule::Value);
    EXPECT_EQ(resultRule(MirOpcode::Phi), MirResultRule::Value);
    // Value-less effects / terminators carry no result.
    EXPECT_EQ(resultRule(MirOpcode::Store), MirResultRule::None);
    EXPECT_EQ(resultRule(MirOpcode::Br), MirResultRule::None);
    EXPECT_EQ(resultRule(MirOpcode::Return), MirResultRule::None);
    // Calls may or may not produce a value (void callee).
    EXPECT_EQ(resultRule(MirOpcode::Call), MirResultRule::Optional);
    EXPECT_EQ(resultRule(MirOpcode::IntrinsicCall), MirResultRule::Optional);
}

TEST(MirOpcode, OperandArities) {
    EXPECT_EQ(opcodeInfo(MirOpcode::Add).minOperands, 2);
    EXPECT_EQ(opcodeInfo(MirOpcode::Add).maxOperands, 2);
    EXPECT_EQ(opcodeInfo(MirOpcode::Neg).minOperands, 1);
    EXPECT_EQ(opcodeInfo(MirOpcode::Neg).maxOperands, 1);
    EXPECT_EQ(opcodeInfo(MirOpcode::Const).minOperands, 0);
    EXPECT_EQ(opcodeInfo(MirOpcode::Const).maxOperands, 0);
    // Calls / gep are variadic.
    EXPECT_EQ(opcodeInfo(MirOpcode::Call).maxOperands, kMirUnboundedOperands);
    EXPECT_EQ(opcodeInfo(MirOpcode::Gep).maxOperands, kMirUnboundedOperands);
    EXPECT_GE(opcodeInfo(MirOpcode::Call).minOperands, 1);  // at least the callee
}

TEST(MirOpcode, SideEffects) {
    EXPECT_TRUE(opcodeInfo(MirOpcode::Store).hasSideEffects);
    EXPECT_TRUE(opcodeInfo(MirOpcode::Call).hasSideEffects);
    EXPECT_TRUE(opcodeInfo(MirOpcode::IntrinsicCall).hasSideEffects);
    EXPECT_FALSE(opcodeInfo(MirOpcode::Add).hasSideEffects);
    EXPECT_FALSE(opcodeInfo(MirOpcode::Load).hasSideEffects);
}

TEST(MirOpcode, InvalidSentinelHasImpossibleArity) {
    // The slot-0 sentinel's impossible {min=1, max=0} arity surfaces any
    // accidental use loudly (no real opcode can satisfy min > max).
    MirOpcodeInfo const info = opcodeInfo(MirOpcode::Invalid);
    EXPECT_GT(info.minOperands, info.maxOperands);
    EXPECT_FALSE(info.isTerminator);
    EXPECT_EQ(info.result, MirResultRule::None);
}

TEST(MirOpcode, SuccessorArities) {
    // Every terminator carries a CFG successor arity in the SAME descriptor as
    // operands — the builder's recordSuccessors_ asserts against it, so the
    // single-source-of-truth runs both ways: a future terminator method that
    // miscounts fails loud, and ML3's frozen-module verifier reads this same
    // table for arbitrary construction paths.
    EXPECT_EQ(opcodeInfo(MirOpcode::Br).minSuccessors, 1);
    EXPECT_EQ(opcodeInfo(MirOpcode::Br).maxSuccessors, 1);
    EXPECT_EQ(opcodeInfo(MirOpcode::CondBr).minSuccessors, 2);
    EXPECT_EQ(opcodeInfo(MirOpcode::CondBr).maxSuccessors, 2);
    EXPECT_EQ(opcodeInfo(MirOpcode::Switch).minSuccessors, 1);   // ≥ default
    EXPECT_EQ(opcodeInfo(MirOpcode::Switch).maxSuccessors, kMirUnboundedSuccessors);
    EXPECT_EQ(opcodeInfo(MirOpcode::Return).minSuccessors, 0);
    EXPECT_EQ(opcodeInfo(MirOpcode::Return).maxSuccessors, 0);
    EXPECT_EQ(opcodeInfo(MirOpcode::Unreachable).minSuccessors, 0);
    EXPECT_EQ(opcodeInfo(MirOpcode::Unreachable).maxSuccessors, 0);

    // Non-terminators carry {0, 0} — a CFG edge is meaningful only on a terminator.
    for (std::uint32_t i = 0; i < kOpcodeCount; ++i) {
        auto const op = static_cast<MirOpcode>(i);
        if (!isTerminator(op)) {
            EXPECT_EQ(opcodeInfo(op).minSuccessors, 0)
                << "non-terminator " << opcodeInfo(op).mnemonic << " has non-zero minSuccessors";
            EXPECT_EQ(opcodeInfo(op).maxSuccessors, 0)
                << "non-terminator " << opcodeInfo(op).mnemonic << " has non-zero maxSuccessors";
        }
    }
}

TEST(MirOpcode, MnemonicsAreStable) {
    EXPECT_EQ(mnemonic(MirOpcode::Add), "add");
    EXPECT_EQ(mnemonic(MirOpcode::ICmpSlt), "icmp.slt");
    EXPECT_EQ(mnemonic(MirOpcode::FCmpOeq), "fcmp.oeq");
    EXPECT_EQ(mnemonic(MirOpcode::Phi), "phi");
    EXPECT_EQ(mnemonic(MirOpcode::CondBr), "condbr");
}
