// ARM64 binary-op + bl byte-encoding tests — plan 13 AS3b + AS4.
//
// Pins exact bytes for the 6 ARM64 binary ops (add/sub/mul/and/or/xor
// reg-reg, all 3-address — no requires2Address) plus the `bl sym`
// call-with-link form which emits a `call26` relocation.

#include "asm/asm.hpp"
#include "asm_test_support.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace dss;
using dss::test_support::asm_::countDiagnostics;

namespace {

struct ArmFx {
    std::shared_ptr<TargetSchema> schema;
    std::uint16_t addOp, subOp, mulOp, andOp, orOp, xorOp, blOp, retOp;
};

[[nodiscard]] ArmFx loadArm() {
    ArmFx f{};
    auto s = TargetSchema::loadShipped("arm64");
    EXPECT_TRUE(s.has_value());
    if (!s) return f;
    f.schema = *s;
    f.addOp = *f.schema->opcodeByMnemonic("add");
    f.subOp = *f.schema->opcodeByMnemonic("sub");
    f.mulOp = *f.schema->opcodeByMnemonic("mul");
    f.andOp = *f.schema->opcodeByMnemonic("and");
    f.orOp  = *f.schema->opcodeByMnemonic("or");
    f.xorOp = *f.schema->opcodeByMnemonic("xor");
    f.blOp  = *f.schema->opcodeByMnemonic("bl");
    f.retOp = *f.schema->opcodeByMnemonic("ret");
    return f;
}

[[nodiscard]] LirReg xreg(TargetSchema const& s, std::string_view name) {
    auto const ord = s.registerByName(name);
    EXPECT_TRUE(ord.has_value());
    return LirReg{static_cast<std::uint32_t>(ord.value_or(0)), 1,
                  static_cast<std::uint8_t>(LirRegClass::GPR)};
}

template <typename Emit>
[[nodiscard]] std::vector<std::uint8_t>
buildAndAssemble(ArmFx const& f, DiagnosticReporter& rep, Emit emit,
                 std::vector<Relocation>& outRelocs) {
    LirBuilder b{*f.schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    emit(b);
    (void)b.addReturn(f.retOp, {});
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    auto r = assemble(lir, *f.schema, lirToMir, rep);
    EXPECT_EQ(r.functions.size(), 1u);
    if (r.functions.empty()) return {};
    outRelocs = r.functions[0].relocations;
    return r.functions[0].bytes;
}

// Helper: extract the first instruction word (4 bytes LE) from `bytes`,
// returning as uint32 for direct comparison against expected encoding.
[[nodiscard]] std::uint32_t
firstInstWord(std::vector<std::uint8_t> const& bytes) {
    if (bytes.size() < 4) return 0;
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) <<  8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

} // namespace

// ── add Xd, Xn, Xm — 0x8B000000 + (Rm<<16) + (Rn<<5) + Rd ─────────────

TEST(Arm64BinaryOps, AddX0X1X2) {
    // ADD X0, X1, X2 = 0x8B020020
    //   Rd=X0(0), Rn=X1(1)<<5=0x20, Rm=X2(2)<<16=0x20000
    auto f = loadArm();
    DiagnosticReporter rep;
    std::vector<Relocation> relocs;
    LirReg const x0 = xreg(*f.schema, "x0");
    LirReg const x1 = xreg(*f.schema, "x1");
    LirReg const x2 = xreg(*f.schema, "x2");
    auto bytes = buildAndAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(x1),
                                    LirOperand::makeReg(x2) };
        (void)b.addInst(f.addOp, x0, ops);
    }, relocs);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes), 0x8B020020u);
    EXPECT_TRUE(relocs.empty());
}

TEST(Arm64BinaryOps, SubX0X1X2) {
    // SUB X0, X1, X2 = 0xCB020020
    auto f = loadArm();
    DiagnosticReporter rep;
    std::vector<Relocation> relocs;
    LirReg const x0 = xreg(*f.schema, "x0");
    LirReg const x1 = xreg(*f.schema, "x1");
    LirReg const x2 = xreg(*f.schema, "x2");
    auto bytes = buildAndAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(x1),
                                    LirOperand::makeReg(x2) };
        (void)b.addInst(f.subOp, x0, ops);
    }, relocs);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes), 0xCB020020u);
    EXPECT_TRUE(relocs.empty()) << "reg-reg binop must emit no reloc";
}

TEST(Arm64BinaryOps, MulX0X1X2) {
    // MUL X0, X1, X2 = MADD X0, X1, X2, XZR
    //   Base = 0x9B007C00 (sf=1, Ra=XZR=31 << 10 = 0x7C00)
    //   + Rm=2<<16=0x20000 + Rn=1<<5=0x20 + Rd=0
    //   = 0x9B027C20
    auto f = loadArm();
    DiagnosticReporter rep;
    std::vector<Relocation> relocs;
    LirReg const x0 = xreg(*f.schema, "x0");
    LirReg const x1 = xreg(*f.schema, "x1");
    LirReg const x2 = xreg(*f.schema, "x2");
    auto bytes = buildAndAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(x1),
                                    LirOperand::makeReg(x2) };
        (void)b.addInst(f.mulOp, x0, ops);
    }, relocs);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes), 0x9B027C20u);
    EXPECT_TRUE(relocs.empty()) << "reg-reg binop must emit no reloc";
}

TEST(Arm64BinaryOps, AndX0X1X2) {
    // AND X0, X1, X2 = 0x8A020020
    auto f = loadArm();
    DiagnosticReporter rep;
    std::vector<Relocation> relocs;
    LirReg const x0 = xreg(*f.schema, "x0");
    LirReg const x1 = xreg(*f.schema, "x1");
    LirReg const x2 = xreg(*f.schema, "x2");
    auto bytes = buildAndAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(x1),
                                    LirOperand::makeReg(x2) };
        (void)b.addInst(f.andOp, x0, ops);
    }, relocs);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes), 0x8A020020u);
    EXPECT_TRUE(relocs.empty()) << "reg-reg binop must emit no reloc";
}

TEST(Arm64BinaryOps, OrX0X1X2) {
    // ORR X0, X1, X2 = 0xAA020020
    auto f = loadArm();
    DiagnosticReporter rep;
    std::vector<Relocation> relocs;
    LirReg const x0 = xreg(*f.schema, "x0");
    LirReg const x1 = xreg(*f.schema, "x1");
    LirReg const x2 = xreg(*f.schema, "x2");
    auto bytes = buildAndAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(x1),
                                    LirOperand::makeReg(x2) };
        (void)b.addInst(f.orOp, x0, ops);
    }, relocs);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes), 0xAA020020u);
    EXPECT_TRUE(relocs.empty()) << "reg-reg binop must emit no reloc";
}

TEST(Arm64BinaryOps, XorX0X1X2) {
    // EOR X0, X1, X2 = 0xCA020020
    auto f = loadArm();
    DiagnosticReporter rep;
    std::vector<Relocation> relocs;
    LirReg const x0 = xreg(*f.schema, "x0");
    LirReg const x1 = xreg(*f.schema, "x1");
    LirReg const x2 = xreg(*f.schema, "x2");
    auto bytes = buildAndAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(x1),
                                    LirOperand::makeReg(x2) };
        (void)b.addInst(f.xorOp, x0, ops);
    }, relocs);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes), 0xCA020020u);
    EXPECT_TRUE(relocs.empty()) << "reg-reg binop must emit no reloc";
}

// ── bl symbol — emits call26 relocation ────────────────────────────

TEST(Arm64Relocations, BlSymEmitsCall26Reloc) {
    // bl sym = 0x94000000 base + imm26 (bits 0..25 = 0 since linker
    // patches). One Relocation entry pointing at the target symbol
    // with kind=call26 and offset = 0 (the bl instruction is the
    // function's first inst).
    auto f = loadArm();
    DiagnosticReporter rep;
    std::vector<Relocation> relocs;
    auto bytes = buildAndAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeSymbolRef(42) };
        (void)b.addInst(f.blOp, LirReg{}, ops);
    }, relocs);
    EXPECT_EQ(rep.errorCount(), 0u);
    // First 4 bytes are the bl instruction.
    EXPECT_EQ(firstInstWord(bytes), 0x94000000u);
    // One relocation, kind=call26, target=symbol 42, offset=0.
    ASSERT_EQ(relocs.size(), 1u);
    EXPECT_EQ(relocs[0].offset, 0u);
    EXPECT_EQ(relocs[0].target, SymbolId{42});
    auto const callKind = (*f.schema).relocationByName("call26");
    ASSERT_NE(callKind, nullptr);
    EXPECT_EQ(relocs[0].kind, callKind->kind);
    EXPECT_EQ(relocs[0].addend, 0);
}
