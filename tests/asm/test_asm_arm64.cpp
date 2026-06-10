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
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

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

// ── setcc (CSET) + zext byte-pins — D-AS3-COND-CODE-ARM64 ─────────
//
// These pin the two register-result control-flow-helper opcodes in
// ISOLATION from regalloc (hand-built LIR → assemble → exact bytes),
// hand-verified against the ARM ARM:
//   * `cset Xd, cc` lowers to CSINC Xd, XZR, XZR, INVERT(cc) — base
//     0x9A9F07E0 (Rd=0, Rn=Rm=XZR=31) OR'd with the INVERTED 4-bit
//     condition at bit 12. The invert is what makes "set 1 iff cc"
//     out of CSINC's "Rd = Rn+1 unless cond" semantics, so the pin
//     uses TWO conditions to prove the invert is applied PER-condition
//     (a single condition could pass with a constant nibble bug).
//   * `zext Xd, Wm` (32→64 zero-extension) lowers to ORR Wd, WZR, Wm
//     — the W-form ORR implicitly zeroes the upper 32 bits. Base
//     0x2A0003E0 (Rd=0, Rn=WZR=31) | Rm<<16.

TEST(Arm64Encoder, CsetEncodesInvertedCondAtBits12) {
    // CSINC base 0x9A9F07E0; the condition stored is INVERT(requested):
    //   cset x0, gt : GT(0xC) ^ 1 = 0xD  → 0x9A9F07E0 | (0xD<<12) = 0x9A9FD7E0  (LE: E0 D7 9F 9A)
    //   cset x0, eq : EQ(0x0) ^ 1 = 0x1  → 0x9A9F07E0 | (0x1<<12) = 0x9A9F17E0  (LE: E0 17 9F 9A)
    // Two conditions prove the invert is per-condition, not a constant.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const setccOp = (*s)->opcodeByMnemonic("setcc");
    auto const retOp   = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(setccOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x0{static_cast<std::uint32_t>(*(*s)->registerByName("x0")), 1, cls};

    auto const encodeCset = [&](TargetCondCode cc) {
        LirBuilder b{**s};
        (void)b.addFunction(SymbolId{1});
        auto blk = b.createBlock();
        b.beginBlock(blk);
        (void)b.addInst(*setccOp, x0, std::span<LirOperand const>{},
                        static_cast<std::uint32_t>(cc));
        (void)b.addReturn(*retOp, {});
        Lir lir = std::move(b).finish();
        DiagnosticReporter rep;
        auto bytes = assembleFirstFn(lir, **s, rep);
        EXPECT_EQ(rep.errorCount(), 0u);
        return bytes;
    };

    // cset x0, gt  → 0x9A9FD7E0  (LE: E0 D7 9F 9A)
    {
        auto bytes = encodeCset(TargetCondCode::Sgt);
        ASSERT_GE(bytes.size(), 4u);
        EXPECT_EQ(bytes[0], 0xE0);
        EXPECT_EQ(bytes[1], 0xD7);
        EXPECT_EQ(bytes[2], 0x9F);
        EXPECT_EQ(bytes[3], 0x9A);
    }
    // cset x0, eq  → 0x9A9F17E0  (LE: E0 17 9F 9A)
    {
        auto bytes = encodeCset(TargetCondCode::Eq);
        ASSERT_GE(bytes.size(), 4u);
        EXPECT_EQ(bytes[0], 0xE0);
        EXPECT_EQ(bytes[1], 0x17);
        EXPECT_EQ(bytes[2], 0x9F);
        EXPECT_EQ(bytes[3], 0x9A);
    }
}

TEST(Arm64Encoder, ZextEncodesOrrW) {
    // zext x0, w1 → ORR W0, WZR, W1 = 0x2A0103E0 (LE: E0 03 01 2A).
    //   base 0x2A0003E0 (Rd=0, Rn=WZR=31) | Rm=1<<16 = 0x10000.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const zextOp = (*s)->opcodeByMnemonic("zext");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(zextOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x0{static_cast<std::uint32_t>(*(*s)->registerByName("x0")), 1, cls};
    LirReg const x1{static_cast<std::uint32_t>(*(*s)->registerByName("x1")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const zops[] = { LirOperand::makeReg(x1) };
    (void)b.addInst(*zextOp, x0, zops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xE0);
    EXPECT_EQ(bytes[1], 0x03);
    EXPECT_EQ(bytes[2], 0x01);
    EXPECT_EQ(bytes[3], 0x2A);
}

// ── FC1 (V2-4.X): NEG — surfaced by the modulo corpus' negatives ───

TEST(Arm64Encoder, NegEncodesSubFromXzr) {
    // neg X0, X1 = SUB X0, XZR, X1:
    //   base 0xCB0003E0 (SUB shifted-reg 0xCB000000 | Rn=XZR=31<<5)
    //   | Rm = X1 (enc 1) << 16 → 0x00010000
    //   = 0xCB0103E0 — LE bytes: E0 03 01 CB.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const negOp = (*s)->opcodeByMnemonic("neg");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(negOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x0{static_cast<std::uint32_t>(*(*s)->registerByName("x0")), 1, cls};
    LirReg const x1{static_cast<std::uint32_t>(*(*s)->registerByName("x1")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const nops[] = { LirOperand::makeReg(x1) };
    (void)b.addInst(*negOp, x0, nops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xE0);
    EXPECT_EQ(bytes[1], 0x03);
    EXPECT_EQ(bytes[2], 0x01);
    EXPECT_EQ(bytes[3], 0xCB);
}

// ── FC1 (V2-4.X): SDIV/UDIV — the Rule-1 native divide opcodes ─────
// (D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC closure: arm64's divide is one
// result-bearing 3-address instruction, no implicit RDX:RAX dance.)

TEST(Arm64Encoder, SdivEncodesDataProc2Source) {
    // sdiv X0, X1, X2 — data-processing (2 source), sf=1:
    //   base 0x9AC00C00 (sf=1 | S=0 | 11010110 | opcode 000011)
    //   | Rm = X2 (enc 2)  << 16 → 0x00020000
    //   | Rn = X1 (enc 1)  << 5  → 0x00000020
    //   | Rd = X0 (enc 0)        → 0
    //   = 0x9AC20C20 — LE bytes: 20 0C C2 9A.
    // Hand-verified against the ARM ARM (C6.2.281 SDIV).
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const sdivOp = (*s)->opcodeByMnemonic("sdiv");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(sdivOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x0{static_cast<std::uint32_t>(*(*s)->registerByName("x0")), 1, cls};
    LirReg const x1{static_cast<std::uint32_t>(*(*s)->registerByName("x1")), 1, cls};
    LirReg const x2{static_cast<std::uint32_t>(*(*s)->registerByName("x2")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const dops[] = { LirOperand::makeReg(x1),
                                LirOperand::makeReg(x2) };
    (void)b.addInst(*sdivOp, x0, dops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x0C);
    EXPECT_EQ(bytes[2], 0xC2);
    EXPECT_EQ(bytes[3], 0x9A);
}

TEST(Arm64Encoder, UdivEncodesDataProc2SourceHighRegs) {
    // udiv X3, X4, X14 — UDIV's opcode field is 000010 (bit 11:10 =
    // 10 vs SDIV's 11; a flipped bit here silently turns every
    // unsigned divide into a signed one):
    //   base 0x9AC00800
    //   | Rm = X14 (enc 14) << 16 → 0x000E0000
    //   | Rn = X4  (enc 4)  << 5  → 0x00000080
    //   | Rd = X3  (enc 3)        → 0x00000003
    //   = 0x9ACE0883 — LE bytes: 83 08 CE 9A.
    // X14 covers a >7 register encoding (5-bit field, no x86-style
    // REX pitfalls, but pins the full-width Rm window).
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const udivOp = (*s)->opcodeByMnemonic("udiv");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(udivOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x3 {static_cast<std::uint32_t>(*(*s)->registerByName("x3")),  1, cls};
    LirReg const x4 {static_cast<std::uint32_t>(*(*s)->registerByName("x4")),  1, cls};
    LirReg const x14{static_cast<std::uint32_t>(*(*s)->registerByName("x14")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const dops[] = { LirOperand::makeReg(x4),
                                LirOperand::makeReg(x14) };
    (void)b.addInst(*udivOp, x3, dops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x83);
    EXPECT_EQ(bytes[1], 0x08);
    EXPECT_EQ(bytes[2], 0xCE);
    EXPECT_EQ(bytes[3], 0x9A);
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

TEST(Arm64Encoder, FrameRelativeLeaEncodesAddImm12) {
    // The ML7 callconv's `emitFrameAddr` materializes a body-local's
    // address (`alloca`) into a frame-relative `lea Xd, [sp + #disp]` =
    // AArch64 `ADD Xd, sp, #imm12` — the 3-op no-index lea variant
    // [base_reg, MemBase(1), MemOffset]. This is the HOST-INDEPENDENT
    // byte-pin for the new MemOffset→Imm12 (UNSIGNED, bits 10..21)
    // encoder branch + the frame-lea variant: at runtime that path is
    // exercised only by the qemu-gated arm64_control_flow corpus (a
    // single frame offset), so without this pin it is unguarded on every
    // non-arm64 CI leg (§A.5 cross-target closure — encoding pins guard
    // on every leg, the corpus is the one-leg end-to-end witness).
    //   lea X2, [SP, #24] → ADD X2, SP, #24. Base 0x91000000.
    //   Rd   = X2 (2)  at bits 0..4   → 0x002
    //   Rn   = SP (31) at bits 5..9   → 0x3E0
    //   Imm12 = 24     at bits 10..21 → 24<<10 = 0x6000
    // word = 0x910063E2; LE: E2 63 00 91. Distinct Rd≠Rn + non-zero disp
    // → a dropped offset, a wrong bit-window, or an Rd/Rn swap all fail.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const leaOp = (*s)->opcodeByMnemonic("lea");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(leaOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(24)
    };
    (void)b.addInst(*leaOp, gpr(**s, "x2"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xE2);
    EXPECT_EQ(bytes[1], 0x63);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x91);
}

TEST(Arm64Encoder, FrameLeaOffsetWiderThanImm12FailsLoud) {
    // RED-on-disable for the UNSIGNED Imm12 range guard on the frame-lea:
    // a frame offset wider than unsigned 12-bit (0..4095) must fail loud
    // rather than silently truncate to a WRONG stack slot. 5000 > 4095.
    // The shifted `ADD imm12<<12` form for larger frames is a future
    // generalization — D-LK10-ENTRY-ARM64-WIDE-IMMEDIATE.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const leaOp = (*s)->opcodeByMnemonic("lea");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(leaOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(5000)
    };
    (void)b.addInst(*leaOp, gpr(**s, "x2"), ops);
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
        << "a >12-bit frame-lea offset must emit A_ImmediateOperandOutOfRange";
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

// ── D-ARM64-FLOAT-SUBSTRATE: FPR (F64) byte-pins ──────────────────
//
// The arm64 float substrate is DECLARATION-ONLY (config rows in
// arm64.target.json — zero engine change); these pins lock the five
// new fixed32 words, each hand-derived from the ARM ARM and
// cross-checked against the base+slot composition:
//   * fadd     FADD Dd, Dn, Dm   — base 0x1E602800 (ftype=01 double)
//   * fp_to_si FCVTZS Xd, Dn     — base 0x9E780000 (sf=1, rmode=11)
//   * fmov     FMOV Dd, Dn       — base 0x1E604000 (1-source, Rn)
//   * fldur    LDUR Dt,[Xn,#s9]  — base 0xFC400000 (GPR LDUR | V=1)
//   * fstur    STUR Dt,[Xn,#s9]  — base 0xFC000000 (GPR STUR | V=1)
// The D-register hwEncodings ride the SAME 5-bit Rd/Rn/Rm/Rt windows
// the GPR pins already lock; the high-register pin (d31/d8/d15)
// proves the field placement across the full 5-bit width.

namespace {
[[nodiscard]] LirReg fpr(TargetSchema const& s, std::string_view name) {
    return makePhysicalReg(
        static_cast<std::uint32_t>(*s.registerByName(name)), LirRegClass::FPR);
}
} // namespace

TEST(Arm64Encoder, FaddEncodesScalarDouble) {
    // fadd d0, d1, d2 — FP data-processing (2 source), double:
    //   base 0x1E602800
    //   | Rm = d2 (enc 2) << 16 → 0x00020000
    //   | Rn = d1 (enc 1) << 5  → 0x00000020
    //   | Rd = d0 (enc 0)       → 0
    //   = 0x1E622820 — LE bytes: 20 28 62 1E.
    // Hand-verified against the ARM ARM (C7.2 FADD scalar, ftype=01).
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const faddOp = (*s)->opcodeByMnemonic("fadd");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(faddOp.has_value() && retOp.has_value());

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(fpr(**s, "d1")),
                               LirOperand::makeReg(fpr(**s, "d2")) };
    (void)b.addInst(*faddOp, fpr(**s, "d0"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x28);
    EXPECT_EQ(bytes[2], 0x62);
    EXPECT_EQ(bytes[3], 0x1E);
}

TEST(Arm64Encoder, FaddHighRegistersPinFiveBitFields) {
    // fadd d31, d8, d15 — every operand off the low-register fast
    // path, proving each 5-bit window's full width + placement
    // (d31 = 0b11111 fills Rd; d8/d15 sit in Rn/Rm):
    //   base 0x1E602800
    //   | Rm = d15 (enc 15) << 16 → 0x000F0000
    //   | Rn = d8  (enc 8)  << 5  → 0x00000100
    //   | Rd = d31 (enc 31)       → 0x0000001F
    //   = 0x1E6F291F — LE bytes: 1F 29 6F 1E.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const faddOp = (*s)->opcodeByMnemonic("fadd");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(faddOp.has_value() && retOp.has_value());

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(fpr(**s, "d8")),
                               LirOperand::makeReg(fpr(**s, "d15")) };
    (void)b.addInst(*faddOp, fpr(**s, "d31"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x1F);
    EXPECT_EQ(bytes[1], 0x29);
    EXPECT_EQ(bytes[2], 0x6F);
    EXPECT_EQ(bytes[3], 0x1E);
}

TEST(Arm64Encoder, FcvtzsEncodesDoubleToX0) {
    // fp_to_si x0, d1 → FCVTZS X0, D1 (sf=1, ftype=01, rmode=11
    // toward-zero — the C truncation semantics):
    //   base 0x9E780000
    //   | Rn = d1 (enc 1) << 5 → 0x00000020
    //   | Rd = x0 (enc 0)      → 0
    //   = 0x9E780020 — LE bytes: 20 00 78 9E.
    // The result register is GPR-class, the source FPR-class — the
    // 5-bit slots are class-blind hwEncodings (the cvttsd2si
    // reg/rm-role analog).
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const cvtOp = (*s)->opcodeByMnemonic("fp_to_si");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(cvtOp.has_value() && retOp.has_value());

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(fpr(**s, "d1")) };
    (void)b.addInst(*cvtOp, gpr(**s, "x0"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x78);
    EXPECT_EQ(bytes[3], 0x9E);
}

TEST(Arm64Encoder, FmovEncodesRegisterDouble) {
    // fmov d0, d1 — FP data-processing (1 source): the single source
    // rides Rn (bits 5..9), NOT Rm — the GPR mov's ORR alias puts its
    // source at Rm (bits 16..20), so a copy-paste of that wire would
    // move the wrong field:
    //   base 0x1E604000 | Rn = d1 (enc 1) << 5 → 0x1E604020.
    //   LE bytes: 20 40 60 1E.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const fmovOp = (*s)->opcodeByMnemonic("fmov");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(fmovOp.has_value() && retOp.has_value());

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(fpr(**s, "d1")) };
    (void)b.addInst(*fmovOp, fpr(**s, "d0"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x40);
    EXPECT_EQ(bytes[2], 0x60);
    EXPECT_EQ(bytes[3], 0x1E);
}

TEST(Arm64Encoder, FldurEncodes) {
    // fldur d2, [x3, #16] → LDUR D2, [X3, #16] (SIMD&FP 64-bit,
    // unscaled — the GPR LDUR word with V=1 at bit 26):
    //   base 0xFC400000
    //   | Imm9 = 16 << 12 → 0x00010000
    //   | Rn = x3 (3) << 5 → 0x00000060
    //   | Rt = d2 (2)      → 0x00000002
    //   = 0xFC410062 — LE bytes: 62 00 41 FC.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const ldOp  = (*s)->opcodeByMnemonic("fldur");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(ldOp.has_value() && retOp.has_value());

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "x3")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(16)
    };
    (void)b.addInst(*ldOp, fpr(**s, "d2"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x62);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x41);
    EXPECT_EQ(bytes[3], 0xFC);
}

TEST(Arm64Encoder, FsturEncodesNegativeImm9) {
    // fstur d4, [x5, #-8] → STUR D4, [X5, #-8] — the SIGNED 9-bit
    // window: -8 two's-complements to 0x1F8 in the 9-bit field
    // (bits 12..20), the negative half the positive-offset pins
    // never reach:
    //   base 0xFC000000
    //   | Imm9 = 0x1F8 << 12 → 0x001F8000
    //   | Rn = x5 (5) << 5   → 0x000000A0
    //   | Rt = d4 (4)        → 0x00000004
    //   = 0xFC1F80A4 — LE bytes: A4 80 1F FC.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const stOp  = (*s)->opcodeByMnemonic("fstur");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(stOp.has_value() && retOp.has_value());

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(fpr(**s, "d4")),
        LirOperand::makeReg(gpr(**s, "x5")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(-8)
    };
    (void)b.addInst(*stOp, InvalidLirReg, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xA4);
    EXPECT_EQ(bytes[1], 0x80);
    EXPECT_EQ(bytes[2], 0x1F);
    EXPECT_EQ(bytes[3], 0xFC);
}

// ── D-ARM64-FLOAT-SUBSTRATE: the FULL pipeline (the SSE-test mirror) ──

namespace {

// Decode the emitted byte stream as little-endian 32-bit words —
// sound on arm64 where every instruction is one 4-byte word (no
// variable-length scan needed).
[[nodiscard]] std::vector<std::uint32_t>
wordsOf(std::vector<std::uint8_t> const& bytes) {
    std::vector<std::uint32_t> ws;
    ws.reserve(bytes.size() / 4);
    for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
        ws.push_back(static_cast<std::uint32_t>(bytes[i])
                     | (static_cast<std::uint32_t>(bytes[i + 1]) << 8)
                     | (static_cast<std::uint32_t>(bytes[i + 2]) << 16)
                     | (static_cast<std::uint32_t>(bytes[i + 3]) << 24));
    }
    return ws;
}

// True iff any word matches `want` under `mask` (mask = the encoding's
// fixed bits; the cleared bits are the register/immediate fields the
// regalloc picks).
[[nodiscard]] bool containsWordMasked(std::vector<std::uint32_t> const& ws,
                                      std::uint32_t mask,
                                      std::uint32_t want) {
    for (auto const w : ws) {
        if ((w & mask) == want) return true;
    }
    return false;
}

// Fixed-bit masks for the FPR words (clear exactly the wired fields):
//   FADD   clears Rm[20:16] Rn[9:5] Rd[4:0]          → 0xFFE0FC00
//   FCVTZS clears Rn[9:5] Rd[4:0]                    → 0xFFFFFC00
//   FMOV   clears Rn[9:5] Rd[4:0]                    → 0xFFFFFC00
//   LDUR/STUR clear imm9[20:12] Rn[9:5] Rt[4:0]      → 0xFFE00C00
inline constexpr std::uint32_t kFaddMask   = 0xFFE0FC00u;
inline constexpr std::uint32_t kFaddBase   = 0x1E602800u;
inline constexpr std::uint32_t kFcvtzsMask = 0xFFFFFC00u;
inline constexpr std::uint32_t kFcvtzsBase = 0x9E780000u;
inline constexpr std::uint32_t kFmovMask   = 0xFFFFFC00u;
inline constexpr std::uint32_t kFmovBase   = 0x1E604000u;
inline constexpr std::uint32_t kFpMemMask  = 0xFFE00C00u;
inline constexpr std::uint32_t kFldurBase  = 0xFC400000u;
inline constexpr std::uint32_t kFsturBase  = 0xFC000000u;

void dumpDiagnostics(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        ADD_FAILURE() << "diagnostic: " << d.actual;
    }
}

struct PipelineOut {
    DiagnosticReporter        rep;
    std::vector<std::uint8_t> bytes;
    std::vector<Relocation>   relocs;
    bool                      ok = false;
};

// Drive hand-built MIR through the REAL pipeline: MIR→LIR → liveness
// → regalloc → rewrite → 2-addr legalize → callconv → assemble (the
// exact stage order of compile_pipeline.cpp's lowerMirModuleToAssembly
// — the same shape as test_asm_x86_sse.cpp's runFullPipeline).
// `ccIndex` selects the calling convention by the schema's declared
// order (arm64.target.json: 0 = aapcs64, 1 = apple_arm64).
void runFullPipeline(Mir& mir, TypeInterner const& interner,
                     TargetSchema const& target, PipelineOut& out,
                     std::uint16_t ccIndex = 0) {
    auto lir = lowerToLir(mir, target, interner, out.rep);
    ASSERT_TRUE(lir.ok) << "MIR->LIR failed";
    auto const liveness = analyzeLiveness(lir.lir);
    auto const alloc = allocateRegisters(lir.lir, target, liveness,
                                         ccIndex, out.rep);
    ASSERT_TRUE(alloc.ok()) << "regalloc failed";
    auto rewritten = rewriteWithAllocation(lir.lir, target, alloc, out.rep);
    ASSERT_TRUE(rewritten.ok) << "rewrite failed";
    auto legal = legalizeTwoAddress(rewritten.lir, target, out.rep);
    ASSERT_TRUE(legal.ok()) << "2-addr legalize failed";
    auto cc = materializeCallingConvention(legal.lir, target, alloc, out.rep);
    ASSERT_TRUE(cc.ok()) << "callconv failed";
    std::vector<MirInstId> lirToMir(cc.lir.instCount(), InvalidMirInst);
    auto assembled = assemble(cc.lir, target, lirToMir, out.rep);
    ASSERT_TRUE(assembled.ok()) << "assemble failed";
    ASSERT_EQ(assembled.functions.size(), 1u);
    out.bytes  = assembled.functions[0].bytes;
    out.relocs = assembled.functions[0].relocations;
    out.ok = true;
}

}  // namespace

TEST(Arm64Fpr, FullPipelineDoubleAddToIntEncodesFaddAndFcvtzs) {
    // MIR: i32 f(f64 a, f64 b) { return FPToSI(FAdd(a, b)); } —
    // through the FULL real pipeline under aapcs64, down to arm64
    // machine code with ZERO diagnostics. Beyond the FADD/FCVTZS
    // presence, this pins the three declaration-driven consumers:
    //   * the AAPCS64 d-register ARG PATH: the two `arg` ops
    //     materialize as FMOV copies READING d0 and d1 (cc.argFprs)
    //     — asserted via the FMOV words' Rn fields;
    //   * the fpr `move` row (those copies ARE fmov, not the GPR
    //     ORR-alias mov against a D hwEncoding);
    //   * the fpr `store`/`load` rows: AAPCS64 declares d8-d15
    //     callee-saved + the regalloc allocates callee-saved FIRST,
    //     so the FPR vregs land in d-regs the prologue must FSTUR-
    //     spill and the epilogue FLDUR-reload (the exact shape that
    //     surfaced movsd_store on ms_x64).
    // RED-on-disable lever: strip the arm64 registerClassOps fpr row
    // → classOpHandle fails loud (L_RequiredLirOpcodeMissing, "no
    // 'move' operation for register class 'fpr'") at the arg copy
    // and this test cannot even reach assemble.
    TypeInterner interner{CompilationUnitId{1}};
    auto const f64 = interner.primitive(TypeKind::F64);
    auto const i32 = interner.primitive(TypeKind::I32);
    TypeId const params[] = {f64, f64};
    auto const sig = interner.fnSig(params, i32, CallConv::CcAAPCS64);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    MirBlockId const bb = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    MirInstId const a = mb.addArg(0, f64);
    MirInstId const b = mb.addArg(1, f64);
    MirInstId const addOps[] = {a, b};
    MirInstId const s = mb.addInst(MirOpcode::FAdd, addOps, f64);
    MirInstId const cvtOps[] = {s};
    MirInstId const r = mb.addInst(MirOpcode::FPToSI, cvtOps, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    PipelineOut out;
    runFullPipeline(mir, interner, **target, out);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.rep.errorCount(), 0u);
    if (out.rep.errorCount() != 0u) dumpDiagnostics(out.rep);

    auto const ws = wordsOf(out.bytes);
    EXPECT_TRUE(containsWordMasked(ws, kFaddMask, kFaddBase))
        << "encoded function must contain FADD Dd, Dn, Dm";
    EXPECT_TRUE(containsWordMasked(ws, kFcvtzsMask, kFcvtzsBase))
        << "encoded function must contain FCVTZS Xd, Dn";

    // The AAPCS64 d-register arg path: FMOV copies reading d0 (arg 0)
    // and d1 (arg 1). Rn (the FMOV source) sits at bits 5..9.
    bool sawArgFromD0 = false;
    bool sawArgFromD1 = false;
    for (auto const w : ws) {
        if ((w & kFmovMask) != kFmovBase) continue;
        std::uint32_t const rn = (w >> 5) & 0x1Fu;
        if (rn == 0u) sawArgFromD0 = true;
        if (rn == 1u) sawArgFromD1 = true;
    }
    EXPECT_TRUE(sawArgFromD0)
        << "aapcs64 arg 0 must materialize as an FMOV reading d0";
    EXPECT_TRUE(sawArgFromD1)
        << "aapcs64 arg 1 must materialize as an FMOV reading d1";

    // Callee-saved d-reg discipline (d8-d15 + callee-saved-first
    // regalloc): the prologue spills via FSTUR, the epilogue reloads
    // via FLDUR — the fpr store/load rows' first in-pipeline consumer.
    EXPECT_TRUE(containsWordMasked(ws, kFpMemMask, kFsturBase))
        << "prologue must spill the callee-saved d-reg via STUR(D)";
    EXPECT_TRUE(containsWordMasked(ws, kFpMemMask, kFldurBase))
        << "epilogue must reload the callee-saved d-reg via LDUR(D)";
}

TEST(Arm64Fpr, FullPipelineDoublePlusRodataConstEncodesLeaFldurFadd) {
    // The float_cast corpus shape, host-independent: i32 f(f64 a)
    // { return FPToSI(a + 0.25); } with 0.25 in the HIR→MIR PROMOTED
    // form — an anonymous F64 rodata global + GlobalAddr + Load (what
    // the front-end emits for every float literal). Mirrors the x86
    // FullPipelineDoublePlusRodataConstEncodesLeaMovsdAddsd pin. On
    // arm64 the address materializes via the lea ADRP+ADD pair (TWO
    // relocations to the promoted symbol) and the F64 value loads via
    // the fpr class's `load` row → FLDUR. RED-on-disable: strip the
    // fpr registerClassOps row → MIR Load fails loud ("no 'load'
    // operation for register class 'fpr'").
    TypeInterner interner{CompilationUnitId{1}};
    auto const f64    = interner.primitive(TypeKind::F64);
    auto const ptrF64 = interner.pointer(f64);
    auto const i32    = interner.primitive(TypeKind::I32);
    TypeId const params[] = {f64};
    auto const sig = interner.fnSig(params, i32, CallConv::CcAAPCS64);
    MirBuilder mb;
    MirLiteralValue quarter; quarter.value = 0.25;
    quarter.core = TypeKind::F64;
    mb.addFunction(sig, SymbolId{1});
    (void)mb.addGlobal(f64, SymbolId{500}, mb.literalPoolAdd(quarter));
    MirBlockId const bb = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    MirInstId const a = mb.addArg(0, f64);
    MirInstId const addr = mb.addGlobalAddr(SymbolId{500}, ptrF64);
    MirInstId const loadOps[] = {addr};
    MirInstId const c = mb.addInst(MirOpcode::Load, loadOps, f64);
    MirInstId const addOps[] = {a, c};
    MirInstId const s = mb.addInst(MirOpcode::FAdd, addOps, f64);
    MirInstId const cvtOps[] = {s};
    MirInstId const r = mb.addInst(MirOpcode::FPToSI, cvtOps, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    PipelineOut out;
    runFullPipeline(mir, interner, **target, out);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.rep.errorCount(), 0u)
        << "the global+load shape must encode end-to-end (a missing "
           "fpr 'load' row fails loud here)";
    if (out.rep.errorCount() != 0u) dumpDiagnostics(out.rep);

    auto const ws = wordsOf(out.bytes);
    EXPECT_TRUE(containsWordMasked(ws, kFpMemMask, kFldurBase))
        << "must contain the FPR LDUR(D) load of the rodata constant";
    EXPECT_TRUE(containsWordMasked(ws, kFaddMask, kFaddBase))
        << "must contain FADD";
    EXPECT_TRUE(containsWordMasked(ws, kFcvtzsMask, kFcvtzsBase))
        << "must contain FCVTZS";

    // The lea ADRP+ADD materialization emits exactly TWO relocations,
    // both targeting the promoted global's symbol (the arm64 parallel
    // of the x86 single rel32 assert).
    ASSERT_EQ(out.relocs.size(), 2u);
    auto const* adrp = (*target)->relocationByName("adr_prel_pg_hi21");
    auto const* add  = (*target)->relocationByName("add_abs_lo12_nc");
    ASSERT_NE(adrp, nullptr);
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(out.relocs[0].target, SymbolId{500});
    EXPECT_EQ(out.relocs[0].kind, adrp->kind);
    EXPECT_EQ(out.relocs[1].target, SymbolId{500});
    EXPECT_EQ(out.relocs[1].kind, add->kind);
}
