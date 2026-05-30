// ARM64 schema + fixed32 walker tests — plan 13 AS3 + plan 12 D-ML7-2.1.
//
// Pins:
//   * arm64.target.json loads cleanly.
//   * AAPCS64 calling convention shape (stackPointer, linkRegister,
//     callerSaved/calleeSaved partitioning).
//   * `ret` encodes to 0xD65F03C0 (AArch64 RET X30 — little-endian
//     bytes: C0 03 5F D6).
//   * `mov Xd, Xm` via ORR alias — fixed word 0xAA0003E0 + Rd in
//     bits 0..4 + Rm in bits 16..20.
//   * `unreachable` encodes to 0xD4200000 (BRK #0).

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

[[nodiscard]] std::vector<std::uint8_t>
assembleFirstFn(Lir const& lir, TargetSchema const& schema,
                DiagnosticReporter& rep) {
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto r = assemble(lir, schema, lirToMir, rep);
    EXPECT_EQ(r.functions.size(), 1u);
    if (r.functions.empty()) return {};
    return r.functions[0].bytes;
}

} // namespace

// ── Schema-load + AAPCS64 calling convention ──────────────────────

TEST(Arm64Schema, LoadsCleanly) {
    auto s = TargetSchema::loadShipped("arm64");
    if (!s.has_value()) {
        for (auto const& d : s.error()) {
            ADD_FAILURE() << "load: " << d.path << ": " << d.message;
        }
    }
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ((*s)->name(), "arm64");
    EXPECT_EQ((*s)->abiModel(), TargetAbiModel::RegisterMachine);
}

TEST(Arm64Schema, AAPCS64DeclaresStackPointerAndLinkRegister) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const* cc = (*s)->callingConventionByName("aapcs64");
    ASSERT_NE(cc, nullptr);
    EXPECT_TRUE(cc->stackPointer.has_value());
    EXPECT_EQ(cc->stackPointer->name, "sp");
    EXPECT_TRUE(cc->linkRegister.has_value());
    EXPECT_EQ(cc->linkRegister->name, "x30");
    EXPECT_EQ(cc->stackAlignment, 16u);
    EXPECT_EQ(cc->argGprs.size(), 8u);
    EXPECT_EQ(cc->argGprs[0], "x0");
    EXPECT_EQ(cc->argGprs[7], "x7");
}

// ── fixed32 walker — `ret` ────────────────────────────────────────

TEST(Arm64Encoder, RetEncodesD65F03C0) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(retOp.has_value());

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    // RET X30 = 0xD65F03C0; little-endian bytes: C0 03 5F D6.
    ASSERT_EQ(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xC0);
    EXPECT_EQ(bytes[1], 0x03);
    EXPECT_EQ(bytes[2], 0x5F);
    EXPECT_EQ(bytes[3], 0xD6);
}

TEST(Arm64Encoder, UnreachableEncodesD4200000) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const op = (*s)->opcodeByMnemonic("unreachable");
    ASSERT_TRUE(op.has_value());

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    (void)b.addUnreachable(*op);
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    // BRK #0 = 0xD4200000; LE bytes: 00 00 20 D4.
    ASSERT_EQ(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x20);
    EXPECT_EQ(bytes[3], 0xD4);
}

// ── fixed32 walker — `mov Xd, Xm` (ORR alias) ─────────────────────

TEST(Arm64Encoder, MovX0FromX1EncodesORR) {
    // mov X0, X1 via ORR X0, XZR, X1.
    // ORR (shifted register) base = 0xAA0003E0 (with Rn=XZR=31).
    // Rd  = X0 (encoding 0) at bits 0..4 → adds 0
    // Rm  = X1 (encoding 1) at bits 16..20 → adds 0x10000
    // Final word: 0xAA0003E0 | 0x00010000 = 0xAA0103E0
    // LE bytes: E0 03 01 AA.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const movOp = (*s)->opcodeByMnemonic("mov");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(movOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    auto const x0 = (*s)->registerByName("x0");
    auto const x1 = (*s)->registerByName("x1");
    ASSERT_TRUE(x0.has_value() && x1.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const r_x0{static_cast<std::uint32_t>(*x0), 1, cls};
    LirReg const r_x1{static_cast<std::uint32_t>(*x1), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(r_x1) };
    (void)b.addInst(*movOp, r_x0, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xE0);
    EXPECT_EQ(bytes[1], 0x03);
    EXPECT_EQ(bytes[2], 0x01);
    EXPECT_EQ(bytes[3], 0xAA);
}

TEST(Arm64Encoder, MovX19FromX2DerivesBitFields) {
    // mov X19, X2:
    //   Rd = X19 (enc 19 = 0b10011) at bits 0..4 → 0x13
    //   Rm = X2  (enc 2  = 0b00010) at bits 16..20 → 0x20000
    // word = 0xAA0003E0 | 0x13 | 0x20000 = 0xAA0203F3.
    // LE: F3 03 02 AA.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const movOp = (*s)->opcodeByMnemonic("mov");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const dst{
        static_cast<std::uint32_t>(*(*s)->registerByName("x19")), 1, cls};
    LirReg const src{
        static_cast<std::uint32_t>(*(*s)->registerByName("x2")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(src) };
    (void)b.addInst(*movOp, dst, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xF3);
    EXPECT_EQ(bytes[1], 0x03);
    EXPECT_EQ(bytes[2], 0x02);
    EXPECT_EQ(bytes[3], 0xAA);
}

// ── Shape-vs-slot validate rejection ──────────────────────────────

TEST(EncodingValidate, RejectsModRmSlotInFixed32Variant) {
    // Architect AS3 followup: cross-shape slot reference rejected.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth_arm", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": {
                "format": "fixed32",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "fixedWord": 1 },
                    "resultSlot": "modrm.reg",
                    "wires": [{ "index": 0, "slotKind": "rd" }]
                  }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}

TEST(EncodingValidate, RejectsRdSlotInX86VariableVariant) {
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth_x86", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": {
                "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "rexW": true, "opcode": [139] },
                    "resultSlot": "rd",
                    "wires": [{ "index": 0, "slotKind": "modrm.rm" }]
                  }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}
