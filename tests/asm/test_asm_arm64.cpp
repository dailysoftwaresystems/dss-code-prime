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

// ── fixed32 walker — `mov Xd, #imm16` (MOVZ) — D-LK10-ENTRY-ARM64 ──

TEST(Arm64Encoder, MovImm16EncodesMOVZ) {
    // mov X8, #94 via MOVZ X8, #94 (the entry trampoline's syscall-
    // number load). MOVZ base = 0xD2800000.
    //   Rd    = X8 (enc 8) at bits 0..4  → 0x8
    //   Imm16 = 94          at bits 5..20 → 94 << 5 = 0xBC0
    // word = 0xD2800000 | 0xBC0 | 0x8 = 0xD2800BC8; LE: C8 0B 80 D2.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const movOp = (*s)->opcodeByMnemonic("mov");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(movOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x8{
        static_cast<std::uint32_t>(*(*s)->registerByName("x8")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeImmInt32(94) };
    (void)b.addInst(*movOp, x8, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xC8);
    EXPECT_EQ(bytes[1], 0x0B);
    EXPECT_EQ(bytes[2], 0x80);
    EXPECT_EQ(bytes[3], 0xD2);
}

TEST(Arm64Encoder, MovImm16DerivesBitFields) {
    // mov X5, #0xABCD — pins the Imm16 window (bits 5..20) + Rd (0..4)
    // independently of the trampoline value.
    //   Rd    = X5 (enc 5)  at bits 0..4  → 0x5
    //   Imm16 = 0xABCD       at bits 5..20 → 0xABCD << 5 = 0x1579A0
    // word = 0xD2800000 | 0x1579A0 | 0x5 = 0xD29579A5; LE: A5 79 95 D2.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const movOp = (*s)->opcodeByMnemonic("mov");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x5{
        static_cast<std::uint32_t>(*(*s)->registerByName("x5")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeImmInt32(0xABCD) };
    (void)b.addInst(*movOp, x5, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xA5);
    EXPECT_EQ(bytes[1], 0x79);
    EXPECT_EQ(bytes[2], 0x95);
    EXPECT_EQ(bytes[3], 0xD2);
}

TEST(Arm64Encoder, ImmediateWiderThanImm16FailsLoud) {
    // RED-on-disable for the immediate range guard: a value wider than
    // the 16-bit Imm16 slot must fail loud (A_ImmediateOperandOutOfRange)
    // rather than silently truncate to a WRONG machine-code constant
    // (e.g. a wrong syscall number). 70000 > 0xFFFF.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const movOp = (*s)->opcodeByMnemonic("mov");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x0{
        static_cast<std::uint32_t>(*(*s)->registerByName("x0")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeImmInt32(70000) };
    (void)b.addInst(*movOp, x0, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    (void)assemble(lir, **s, lirToMir, rep);
    bool sawOutOfRange = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::A_ImmediateOperandOutOfRange) {
            sawOutOfRange = true;
        }
    }
    EXPECT_TRUE(sawOutOfRange)
        << "a >16-bit immediate must emit A_ImmediateOperandOutOfRange";
    EXPECT_GT(rep.errorCount(), 0u);
}

// ── fixed32 walker — load/store (LDUR/STUR) + ADD-imm — D-LK10-ENTRY-ARM64 ──

namespace {
[[nodiscard]] LirReg gpr(TargetSchema const& s, std::string_view name) {
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    return LirReg{static_cast<std::uint32_t>(*s.registerByName(name)), 1, cls};
}
} // namespace

TEST(Arm64Encoder, LoadLdurEncodes) {
    // load X1, [SP, #8] → LDUR X1, [SP, #8]. Base 0xF8400000.
    //   Rt   = X1 (1)  at bits 0..4  → 0x01
    //   Rn   = SP (31) at bits 5..9  → 0x3E0
    //   Imm9 = 8       at bits 12..20 → 0x8000
    // word = 0xF84083E1; LE: E1 83 40 F8.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const loadOp = (*s)->opcodeByMnemonic("load");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(loadOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(8)
    };
    (void)b.addInst(*loadOp, gpr(**s, "x1"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xE1);
    EXPECT_EQ(bytes[1], 0x83);
    EXPECT_EQ(bytes[2], 0x40);
    EXPECT_EQ(bytes[3], 0xF8);
}

TEST(Arm64Encoder, StoreSturEncodes) {
    // store X1, [SP, #8] → STUR X1, [SP, #8]. Base 0xF8000000.
    //   Rt(value) = X1 (1) at 0..4, Rn = SP (31) at 5..9, Imm9 = 8.
    // word = 0xF80083E1; LE: E1 83 00 F8.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const storeOp = (*s)->opcodeByMnemonic("store");
    auto const retOp   = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(storeOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "x1")),
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(8)
    };
    (void)b.addInst(*storeOp, InvalidLirReg, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xE1);
    EXPECT_EQ(bytes[1], 0x83);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0xF8);
}

TEST(Arm64Encoder, AddImm12Encodes) {
    // add SP, SP, #16 → ADD SP, SP, #16. Base 0x91000000.
    //   Rd = SP (31) at 0..4 → 0x1F, Rn = SP (31) at 5..9 → 0x3E0,
    //   Imm12 = 16 at bits 10..21 → 0x4000.
    // word = 0x910043FF; LE: FF 43 00 91.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const addOp = (*s)->opcodeByMnemonic("add");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(addOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeImmInt32(16)
    };
    (void)b.addInst(*addOp, gpr(**s, "sp"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xFF);
    EXPECT_EQ(bytes[1], 0x43);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x91);
}

TEST(Arm64Encoder, MemOffsetWiderThanImm9FailsLoud) {
    // RED-on-disable for the Imm9 range guard: a memory offset wider
    // than signed 9-bit (-256..255) must fail loud rather than silently
    // truncate to a WRONG stack slot. 300 > 255.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const loadOp = (*s)->opcodeByMnemonic("load");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(300)
    };
    (void)b.addInst(*loadOp, gpr(**s, "x1"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    (void)assemble(lir, **s, lirToMir, rep);
    bool sawOutOfRange = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::A_ImmediateOperandOutOfRange) {
            sawOutOfRange = true;
        }
    }
    EXPECT_TRUE(sawOutOfRange)
        << "a >9-bit memory offset must emit A_ImmediateOperandOutOfRange";
    EXPECT_GT(rep.errorCount(), 0u);
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

// ── D-AS4-3: multi-instruction-macro encoder + ARM64 lea (ADRP+ADD) ──
//
// `lea Xd, [sym]` lowers to the AArch64 ADRP+ADD pair — the FIRST
// multi-word fixed32 opcode. These pins lock the 8-byte output, the
// two per-word relocations, the single source-map entry, and (via
// the synthetic-schema validate tests below) the word-aware slot
// uniqueness that the generic encoder substrate relies on.

TEST(Arm64Encoder, LeaEmitsAdrpAddWordPair) {
    // word0 = ADRP base 0x90000000 | Rd(bits 0..4)
    // word1 = ADD-imm base 0x91000000 | Rd(0..4) | Rn(5..9)  [Rn == Rd]
    // The page immediate (immlo[30:29]+immhi[23:5]) and lo12 (imm12
    // bits 21..10) are left ZERO — the linker patches them via the two
    // relocations (rejectIfBitfieldDirty requires the fields be zero).
    //
    // RED-on-disable for the per-word `wroteSlot` (plan-lock must-fix 6):
    // a flat (word-blind) slot tracker would treat word1's Rd write as a
    // collision with word0's Rd → the lea fails to encode → this test
    // (8 bytes, no errors) goes red.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const leaOp = (*s)->opcodeByMnemonic("lea");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(leaOp.has_value()) << "arm64 must declare a `lea` opcode";
    ASSERT_TRUE(retOp.has_value());

    auto const word = [](std::vector<std::uint8_t> const& by, std::size_t off) {
        return static_cast<std::uint32_t>(by[off])
             | (static_cast<std::uint32_t>(by[off + 1]) << 8)
             | (static_cast<std::uint32_t>(by[off + 2]) << 16)
             | (static_cast<std::uint32_t>(by[off + 3]) << 24);
    };
    // The lea is the FIRST instruction (byte offset 0); a `ret`
    // terminator follows (functions require a terminator). The lea
    // occupies bytes [0,8); its exact 8-byte span is pinned separately
    // by LeaProducesOneSrcMapEntrySpanning8Bytes.
    auto const encodeLea = [&](std::string_view reg) {
        LirBuilder b{**s};
        (void)b.addFunction(SymbolId{1});
        auto blk = b.createBlock();
        b.beginBlock(blk);
        LirOperand const ops[] = { LirOperand::makeSymbolRef(55) };
        (void)b.addInst(*leaOp, gpr(**s, reg), ops);
        (void)b.addReturn(*retOp, {});
        Lir lir = std::move(b).finish();
        DiagnosticReporter rep;
        auto bytes = assembleFirstFn(lir, **s, rep);
        EXPECT_EQ(rep.errorCount(), 0u);
        return bytes;
    };

    // Rd = x0 (hwEncoding 0): pure base words.
    {
        auto bytes = encodeLea("x0");
        ASSERT_GE(bytes.size(), 8u);
        EXPECT_EQ(word(bytes, 0), 0x90000000u);
        EXPECT_EQ(word(bytes, 4), 0x91000000u);
    }
    // Rd = x3 (hwEncoding 3): Rd in word0 bits 0..4; Rd AND Rn in word1.
    {
        auto bytes = encodeLea("x3");
        ASSERT_GE(bytes.size(), 8u);
        std::uint32_t const w0 = word(bytes, 0);
        std::uint32_t const w1 = word(bytes, 4);
        EXPECT_EQ(w0, 0x90000003u);                 // ADRP Rd=3
        EXPECT_EQ(w1, 0x91000063u);                 // ADD Rd=3 (0x03) | Rn=3 (0x60)
        EXPECT_EQ((w0 >> 29) & 0x3u,     0u) << "ADRP immlo must be zero";
        EXPECT_EQ((w0 >> 5)  & 0x7FFFFu, 0u) << "ADRP immhi must be zero";
        EXPECT_EQ((w1 >> 10) & 0xFFFu,   0u) << "ADD lo12 imm12 must be zero";
    }
}

TEST(Arm64Encoder, LeaStampsTwoRelocsAtWordOffsets) {
    // D-AS4-3 multi-relocation proof: lea emits TWO relocations — ADRP
    // page-reloc at word0 (offset 0) + ADD lo12-reloc at word1 (offset 4),
    // both targeting the SAME symbol.
    //
    // RED-on-disable for the deleted single-reloc cap: re-introducing the
    // "only one symbol-relative wire per fixed32 instruction" cap would
    // reject the second reloc → lea fails to encode → this test goes red.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const leaOp = (*s)->opcodeByMnemonic("lea");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(leaOp.has_value() && retOp.has_value());

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeSymbolRef(55) };
    (void)b.addInst(*leaOp, gpr(**s, "x0"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto result = assemble(lir, **s, lirToMir, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(result.functions.size(), 1u);
    // `ret` carries no symbol → no extra relocation; the two relocs are
    // the lea's ADRP + ADD patches.
    auto const& relocs = result.functions[0].relocations;
    ASSERT_EQ(relocs.size(), 2u);

    auto const* adrp = (*s)->relocationByName("adr_prel_pg_hi21");
    auto const* add  = (*s)->relocationByName("add_abs_lo12_nc");
    ASSERT_NE(adrp, nullptr);
    ASSERT_NE(add, nullptr);

    // reloc[0]: ADRP page-reloc at the START of word0 (offset 0).
    EXPECT_EQ(relocs[0].offset, 0u);
    EXPECT_EQ(relocs[0].kind, adrp->kind);
    EXPECT_EQ(relocs[0].target.v, 55u);
    // reloc[1]: ADD lo12-reloc at the START of word1 (offset 4).
    EXPECT_EQ(relocs[1].offset, 4u);
    EXPECT_EQ(relocs[1].kind, add->kind);
    EXPECT_EQ(relocs[1].target.v, 55u);
}

TEST(Arm64Encoder, LeaProducesOneSrcMapEntrySpanning8Bytes) {
    // D-AS4-3: a multi-word macro is ONE LIR instruction → ONE srcMap
    // entry spanning all 8 bytes (NOT one per word). The next inst (ret)
    // starts at offset 8 — the stride is byte-offset-derived, never a
    // hardcoded 4.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const leaOp = (*s)->opcodeByMnemonic("lea");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(leaOp.has_value() && retOp.has_value());

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeSymbolRef(55) };
    (void)b.addInst(*leaOp, gpr(**s, "x0"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto result = assemble(lir, **s, lirToMir, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(result.functions.size(), 1u);
    auto const& fn = result.functions[0];
    ASSERT_EQ(fn.sourceMap.size(), 2u);              // lea + ret
    EXPECT_EQ(fn.sourceMap[0].byteOffset, 0u);       // lea at 0
    EXPECT_EQ(fn.sourceMap[1].byteOffset, 8u);       // ret at 8 — lea spans 8
    EXPECT_EQ(fn.bytes.size(), 12u);                 // 8 (lea) + 4 (ret)
}

// ── D-AS4-3 word-aware validate (plan-lock must-fix 6, validate half) ──

TEST(EncodingValidate, MultiWordPerWordSlotReuseLoads) {
    // The SAME slot kind in DIFFERENT words is NOT a double-write — a
    // 2-word macro placing Rd in word0 (resultSlot) AND word1
    // (extraResultSlots) must LOAD. (The shipped arm64 `lea` exercises
    // this; this synthetic pin isolates the accept pole.)
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth_mw", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "macro", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": {
                "format": "fixed32",
                "variants": [
                  { "guard": { "operandKinds": ["symbol"] },
                    "template": { "fixedWords": [2415919104, 2432696320] },
                    "resultSlot": "rd",
                    "extraResultSlots": [ { "slotKind": "rd", "wordIndex": 1 } ],
                    "wires": [
                      { "index": 0, "slotKind": "sym.patch", "wordIndex": 0, "relocationKind": "r" }
                    ]
                  }
                ]
              } }
        ],
        "relocations": [
          { "name": "r", "kind": 1, "formula": "linear", "pcRelative": false, "addendBias": 0, "widthBytes": 4 }
        ]
    })";
    EXPECT_TRUE(TargetSchema::loadFromText(kJson, "x.json").has_value())
        << "the same slot kind in distinct words is not a double-write";
}

TEST(EncodingValidate, SameWordSlotDoubleWriteRejected) {
    // Per-word uniqueness (must-fix 6): the SAME slot twice in the SAME
    // word IS a double-write and must fail loud — here resultSlot=rd
    // (word0) collides with an extraResultSlots rd ALSO in word0.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth_dup", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": {
                "format": "fixed32",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "fixedWord": 1 },
                    "resultSlot": "rd",
                    "extraResultSlots": [ { "slotKind": "rd", "wordIndex": 0 } ],
                    "wires": [ { "index": 0, "slotKind": "rn" } ]
                  }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value())
        << "the same slot kind twice in one word must be rejected";
}

TEST(EncodingValidate, MultiWordRejectsFixedWordPlusFixedWords) {
    // A template must not declare BOTH `fixedWord` and `fixedWords`
    // (the single-word default would be silently shadowed).
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth_both", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "none",
              "minOperands": 0, "maxOperands": 0,
              "encoding": {
                "format": "fixed32",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "fixedWord": 1, "fixedWords": [1, 2] } }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value())
        << "fixedWord + fixedWords together must be rejected";
}

TEST(EncodingValidate, MultiWordRejectsWordIndexBeyondTemplate) {
    // A wire wordIndex addressing a word the template does not emit must
    // fail loud (would write into a non-existent word at encode time).
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth_oob", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": {
                "format": "fixed32",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "fixedWords": [1, 2] },
                    "resultSlot": "rd",
                    "wires": [ { "index": 0, "slotKind": "rn", "wordIndex": 5 } ]
                  }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value())
        << "a wire wordIndex beyond the template's word count must be rejected";
}

TEST(EncodingValidate, MultiWordRejectsFixedWordsOnNonFixed32) {
    // `fixedWords` (multi-word macro) is a fixed32-only construct — a
    // multi-word x86 template has no defined emission model. Reject it
    // on an x86-variable opcode. (Isolated negative pole for validate
    // rule (a); the shipped arm64 lea exercises the positive case.)
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth_mw_x86", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "none",
              "minOperands": 0, "maxOperands": 0,
              "encoding": {
                "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "fixedWords": [1, 2] } }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value())
        << "'fixedWords' on a non-fixed32 shape must be rejected";
}

TEST(EncodingValidate, MultiWordRejectsExtraResultSlotsWithoutResultSlot) {
    // `extraResultSlots` is an ADDITIONAL placement of the result reg —
    // meaningless without a primary `resultSlot`. result=none + no
    // resultSlot isolates validate rule (b) (no Convergence-fix G
    // result-needs-a-slot conflict, since result is none).
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth_extra_noresult", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "none",
              "minOperands": 0, "maxOperands": 0,
              "encoding": {
                "format": "fixed32",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "fixedWord": 1 },
                    "extraResultSlots": [ { "slotKind": "rd", "wordIndex": 0 } ] }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value())
        << "'extraResultSlots' without a 'resultSlot' must be rejected";
}
