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

// ── FC4 c2: indirect call `BLR Xn` — 0xD63F0000 | Rn<<5 ────────────
// The `call` row's ["reg"] encoding variant (the direct ["symbol"]
// variant emits BL imm26 + a call26 relocation). Callee register in
// the Rn slot (bits 5..9), exactly like sdiv/udiv's Rn wiring.

TEST(Arm64Encoder, BlrX9EncodesD63F0120) {
    // BLR X9 = 0xD63F0000 | (9 << 5) = 0xD63F0120.
    // LE bytes: 20 01 3F D6.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const callOp = (*s)->opcodeByMnemonic("call");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    auto const x9 = (*s)->registerByName("x9");
    ASSERT_TRUE(x9.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const r_x9{static_cast<std::uint32_t>(*x9), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(r_x9) };
    (void)b.addInst(*callOp, InvalidLirReg, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);  // BLR + RET
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x01);
    EXPECT_EQ(bytes[2], 0x3F);
    EXPECT_EQ(bytes[3], 0xD6);
    // The trailing RET (C0 03 5F D6) confirms the BLR did not bleed
    // into the next word.
    EXPECT_EQ(bytes[4], 0xC0);
    EXPECT_EQ(bytes[5], 0x03);
    EXPECT_EQ(bytes[6], 0x5F);
    EXPECT_EQ(bytes[7], 0xD6);
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

// ── D-CSUBSET-VLA (C1b): `sub_sp_reg` + `sp_copy` byte pins ───────
//
// ★ THE XZR-VS-SP TRAP guard. `sub_sp_reg sp, Xm` MUST encode the
// EXTENDED-register SUB (0xCB2063FF base: option=011 UXTX, bit 21 set,
// Rd=Rn=sp=31), NOT the shifted-register SUB (0xCB000000) where reg 31
// = XZR would compute `sp - 0` and silently discard the size. A byte
// regression here is a silent stack miscompile the qemu run also
// catches (examples/c-subset/c99_vla) — this is the host-independent
// red-on-disable pin.

TEST(Arm64Encoder, SubSpRegEncodesExtendedRegisterWord) {
    // sub_sp_reg sp, x9  =>  SUB sp, sp, x9 (extended reg, UXTX):
    //   base 0xCB2063FF | (Rm=x9=9 << 16) = 0xCB2963FF.  LE: FF 63 29 CB.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const subSpReg = (*s)->opcodeByMnemonic("sub_sp_reg");
    auto const retOp    = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(subSpReg.has_value());
    ASSERT_TRUE(retOp.has_value());
    auto const sp = (*s)->registerByName("sp");
    auto const x9 = (*s)->registerByName("x9");
    ASSERT_TRUE(sp.has_value() && x9.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const r_sp{static_cast<std::uint32_t>(*sp), 1, cls};
    LirReg const r_x9{static_cast<std::uint32_t>(*x9), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(r_sp), LirOperand::makeReg(r_x9) };
    (void)b.addInst(*subSpReg, InvalidLirReg, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xFF);
    EXPECT_EQ(bytes[1], 0x63);
    EXPECT_EQ(bytes[2], 0x29);   // 0x20 base | (Rm=9 at bits 16..20) => 0x29
    EXPECT_EQ(bytes[3], 0xCB);
}

TEST(Arm64Encoder, SpCopyEncodesAddImmZero) {
    // sp_copy x29, sp  =>  ADD x29, sp, #0 (the SP-capable add-imm form,
    //   reg 31 = SP not XZR):  0x91000000 | Rd=29 | (Rn=sp=31 << 5)
    //   = 0x910003FD.  LE: FD 03 00 91.  (== the prologue `mov x29, sp`.)
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const spCopy = (*s)->opcodeByMnemonic("sp_copy");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(spCopy.has_value());
    ASSERT_TRUE(retOp.has_value());
    auto const sp  = (*s)->registerByName("sp");
    auto const x29 = (*s)->registerByName("x29");
    ASSERT_TRUE(sp.has_value() && x29.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const r_sp{static_cast<std::uint32_t>(*sp), 1, cls};
    LirReg const r_x29{static_cast<std::uint32_t>(*x29), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(r_sp) };
    (void)b.addInst(*spCopy, r_x29, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xFD);
    EXPECT_EQ(bytes[1], 0x03);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x91);
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

TEST(Arm64Encoder, NotEncodesMvnFromXzr) {
    // Cluster-F F2 (core_bitwise): not X0, X1 = MVN X0, X1 = ORN X0, XZR, X1:
    //   base 0xAA2003E0 (ORN shifted-reg: ORR family opc=01, N bit=1, Rn=XZR=31<<5)
    //   | Rm = X1 (enc 1) << 16 → 0x00010000
    //   = 0xAA2103E0 — LE bytes: E0 03 21 AA. (A wrong fixedWord, a missing N bit
    //   [→ ORR = a no-op move], or an Rn≠XZR all fail this pin.)
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const notOp = (*s)->opcodeByMnemonic("not");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(notOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x0{static_cast<std::uint32_t>(*(*s)->registerByName("x0")), 1, cls};
    LirReg const x1{static_cast<std::uint32_t>(*(*s)->registerByName("x1")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const nops[] = { LirOperand::makeReg(x1) };
    (void)b.addInst(*notOp, x0, nops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xE0);
    EXPECT_EQ(bytes[1], 0x03);
    EXPECT_EQ(bytes[2], 0x21);
    EXPECT_EQ(bytes[3], 0xAA);
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

TEST(Arm64Encoder, UmulhEncodesDataProc3SourceHighRegs) {
    // c103 (D-CSUBSET-INTRINSIC-UMULH): umulh X3, X4, X14 — the high 64 bits of the
    // unsigned 64x64 product. UMULH is a data-processing (3-source) op with Ra=XZR:
    //   base 0x9BC07C00 (sf=1 | 00 | 11011 | U=1 | 10 | Rm | o0=0 | Ra=XZR(11111))
    //   | Rm = X14 (enc 14) << 16 → 0x000E0000
    //   | Rn = X4  (enc 4)  << 5  → 0x00000080
    //   | Rd = X3  (enc 3)        → 0x00000003
    //   = 0x9BCE7C83 — LE bytes: 83 7C CE 9B.
    // The U bit (23) discriminates UMULH from SMULH (0x9B40...) — a flipped bit
    // silently turns unsigned mul-high into signed. The always-on structural guard
    // for the arm64 mul-high encoding (the qemu corpus run is the runtime witness).
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const umulhOp = (*s)->opcodeByMnemonic("umulh");
    auto const retOp   = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(umulhOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x3 {static_cast<std::uint32_t>(*(*s)->registerByName("x3")),  1, cls};
    LirReg const x4 {static_cast<std::uint32_t>(*(*s)->registerByName("x4")),  1, cls};
    LirReg const x14{static_cast<std::uint32_t>(*(*s)->registerByName("x14")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const mops[] = { LirOperand::makeReg(x4),
                                LirOperand::makeReg(x14) };
    (void)b.addInst(*umulhOp, x3, mops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x83);
    EXPECT_EQ(bytes[1], 0x7C);
    EXPECT_EQ(bytes[2], 0xCE);
    EXPECT_EQ(bytes[3], 0x9B);
}

// ── FC17.9(b) (D-CSUBSET-BITCOUNT-INTRINSICS): CLZ / RBIT ──
//
// The arm64 half of the native bit-count realization: CLZ (Clz's native op, the
// `clz` verb shared with x86 LZCNT) + RBIT (arm64's ctz composes CLZ(RBIT(x))).
// Both are data-processing 1-source (operand in Rn bits 9:5, result in Rd bits
// 4:0). The opcode field (bits 15:10) discriminates CLZ (000100) from RBIT
// (000000) — byte 1 (0x10 vs 0x00). The sf bit (byte 3, 0xDA vs 0x5A) picks the
// 64- vs 32-bit form.

namespace {
[[nodiscard]] std::vector<std::uint8_t>
assembleArm64Unary(char const* mnemonic, char const* dst, char const* src,
                   bool width32) {
    auto s = TargetSchema::loadShipped("arm64");
    EXPECT_TRUE(s.has_value());
    auto const op    = (*s)->opcodeByMnemonic(mnemonic);
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    EXPECT_TRUE(op.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const rd{static_cast<std::uint32_t>(*(*s)->registerByName(dst)), 1, cls};
    LirReg const rn{static_cast<std::uint32_t>(*(*s)->registerByName(src)), 1, cls};
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(rn) };
    (void)b.addInst(*op, rd, ops, /*payload=*/0,
                    width32 ? ::dss::kLirInstFlagWidth32 : std::uint8_t{0});
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    return bytes;
}
} // namespace

TEST(Arm64Encoder, ClzX3X4EncodesDataProc1Source) {
    // clz x3, x4 — base 0xDAC01000 | Rn=x4(4)<<5=0x80 | Rd=x3(3) = 0xDAC01083
    // → LE bytes: 83 10 C0 DA. The runtime witness is the qemu example arm.
    auto const bytes = assembleArm64Unary("clz", "x3", "x4", /*width32=*/false);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x83);
    EXPECT_EQ(bytes[1], 0x10);
    EXPECT_EQ(bytes[2], 0xC0);
    EXPECT_EQ(bytes[3], 0xDA);
}

TEST(Arm64Encoder, ClzW3W4EncodesWForm) {
    // clz w3, w4 — the 32-bit form (sf=0): 0x5AC01000 | 0x80 | 3 = 0x5AC01083
    // → LE bytes: 83 10 C0 5A. Byte 3 (0x5A vs 0xDA) pins the sf width bit.
    auto const bytes = assembleArm64Unary("clz", "x3", "x4", /*width32=*/true);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x83);
    EXPECT_EQ(bytes[1], 0x10);
    EXPECT_EQ(bytes[2], 0xC0);
    EXPECT_EQ(bytes[3], 0x5A);
}

TEST(Arm64Encoder, RbitX3X4EncodesDataProc1Source) {
    // rbit x3, x4 — opcode field 000000 (vs CLZ's 000100): 0xDAC00000 | 0x80 | 3
    // = 0xDAC00083 → LE bytes: 83 00 C0 DA. Byte 1 (0x00 vs 0x10) discriminates
    // it from CLZ — a swap would silently miscompute ctz. Used by arm64 ctz.
    auto const bytes = assembleArm64Unary("rbit", "x3", "x4", /*width32=*/false);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x83);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0xC0);
    EXPECT_EQ(bytes[3], 0xDA);
}

TEST(Arm64Encoder, LdaxrW0X1EncodesLoadAcquireExclusive) {
    // c104 (D-CSUBSET-INTRINSIC-ATOMIC-CAS): ldaxr w0, [x1] — load-acquire
    // exclusive, the LL half of the CAS retry loop. W-form base 0x885FFC00
    // (size=10 | 001000 | L=1 | Rs=11111 | o0=1 | Rt2=11111) | Rn=x1(1)<<5 |
    // Rt=w0(0) = 0x885FFC20 — LE bytes: 20 FC 5F 88. The result wires to the
    // Rt field (bits 4:0, the `rd` slot); a wrong L bit silently turns the
    // acquire-load into a store-form — byte 3/2 pin the exact class bits.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const ldaxrOp = (*s)->opcodeByMnemonic("ldaxr");
    auto const retOp   = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(ldaxrOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x0{static_cast<std::uint32_t>(*(*s)->registerByName("x0")), 1, cls};
    LirReg const x1{static_cast<std::uint32_t>(*(*s)->registerByName("x1")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(x1) };
    (void)b.addInst(*ldaxrOp, x0, ops, /*payload=*/0, kLirInstFlagWidth32);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0xFC);
    EXPECT_EQ(bytes[2], 0x5F);
    EXPECT_EQ(bytes[3], 0x88);
}

TEST(Arm64Encoder, StlxrW2W0X1EncodesStoreReleaseExclusiveStatusInRs) {
    // c104: stlxr w2, w0, [x1] — store-release exclusive, the SC half. W-form
    // base 0x8800FC00 | Rs=w2(2)<<16 | Rn=x1(1)<<5 | Rt=w0(0) = 0x8802FC20 —
    // LE bytes: 20 FC 02 88. The STATUS result lives in the Rs bit-field
    // (20:16 — the generic `rm` slot), the stored VALUE wires to the Rt field
    // (bits 4:0, the `rd` slot position): this pin locks the Rs-vs-Rt wiring —
    // swapping them silently stores the status and reports the value.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const stlxrOp = (*s)->opcodeByMnemonic("stlxr");
    auto const retOp   = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(stlxrOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x0{static_cast<std::uint32_t>(*(*s)->registerByName("x0")), 1, cls};
    LirReg const x1{static_cast<std::uint32_t>(*(*s)->registerByName("x1")), 1, cls};
    LirReg const x2{static_cast<std::uint32_t>(*(*s)->registerByName("x2")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(x0),    // stored value → Rt
                               LirOperand::makeReg(x1) };  // base → Rn
    (void)b.addInst(*stlxrOp, x2, ops, /*payload=*/0, kLirInstFlagWidth32);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0xFC);
    EXPECT_EQ(bytes[2], 0x02);
    EXPECT_EQ(bytes[3], 0x88);
}

// ── FC3.5 sweep-c3: MSUB — D-LIR-MOD-MSUB-FUSION (the fixed32 `ra`
// slot's first consumer; rule 3's fused remainder realization) ─────

TEST(Arm64Encoder, MsubEncodesDataProc3Source) {
    // msub X0, X1, X2, X3 = X3 − X1·X2 — data-processing (3 source),
    // ARM ARM C6.2.230:
    //   base 0x9B008000 (sf=1 | 00 | 11011 | 000 | o0=1 sub)
    //   | Rm = X2 (enc 2) << 16 → 0x00020000
    //   | Ra = X3 (enc 3) << 10 → 0x00000C00
    //   | Rn = X1 (enc 1) << 5  → 0x00000020
    //   | Rd = X0 (enc 0)       → 0
    //   = 0x9B028C20 — LE bytes: 20 8C 02 9B.
    // A wrong Ra window (e.g. the bits 10..14 field drifting) corrupts
    // the minuend register — every remainder silently wrong.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const msubOp = (*s)->opcodeByMnemonic("msub");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(msubOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x0{static_cast<std::uint32_t>(*(*s)->registerByName("x0")), 1, cls};
    LirReg const x1{static_cast<std::uint32_t>(*(*s)->registerByName("x1")), 1, cls};
    LirReg const x2{static_cast<std::uint32_t>(*(*s)->registerByName("x2")), 1, cls};
    LirReg const x3{static_cast<std::uint32_t>(*(*s)->registerByName("x3")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const mops[] = { LirOperand::makeReg(x1),
                                LirOperand::makeReg(x2),
                                LirOperand::makeReg(x3) };
    (void)b.addInst(*msubOp, x0, mops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x8C);
    EXPECT_EQ(bytes[2], 0x02);
    EXPECT_EQ(bytes[3], 0x9B);
}

TEST(Arm64Encoder, MsubWFormEncodesHighRegs) {
    // msub W3, W4, W14, W21 — the sf=0 W-form (0x1B008000) with HIGH
    // register encodings exercising the full 5-bit Rm (14) and Ra (21
    // = 0b10101, both window edges) fields:
    //   0x1B008000 | (14<<16) | (21<<10) | (4<<5) | 3 = 0x1B0ED483
    //   — LE bytes: 83 D4 0E 1B.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const msubOp = (*s)->opcodeByMnemonic("msub");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(msubOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x3 {static_cast<std::uint32_t>(*(*s)->registerByName("x3")),  1, cls};
    LirReg const x4 {static_cast<std::uint32_t>(*(*s)->registerByName("x4")),  1, cls};
    LirReg const x14{static_cast<std::uint32_t>(*(*s)->registerByName("x14")), 1, cls};
    LirReg const x21{static_cast<std::uint32_t>(*(*s)->registerByName("x21")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const mops[] = { LirOperand::makeReg(x4),
                                LirOperand::makeReg(x14),
                                LirOperand::makeReg(x21) };
    (void)b.addInst(*msubOp, x3, mops, /*payload=*/0, kLirInstFlagWidth32);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x83);
    EXPECT_EQ(bytes[1], 0xD4);
    EXPECT_EQ(bytes[2], 0x0E);
    EXPECT_EQ(bytes[3], 0x1B);
}

// ── FC3.5 sweep-c3: MOVK ladder — D-LK10-ENTRY-ARM64-WIDE-IMMEDIATE ─
// MOVK Xd, #imm16, LSL #n (ARM ARM C6.2.227): sf=1 | opc=11 | 100101 |
// hw | imm16 | Rd. X-base 0xF2800000; hw=01/10/11 → LSL 16/32/48.

TEST(Arm64Encoder, MovkLsl16EncodesHwField01) {
    // movk x5, #0xABCD, LSL #16 → 0xF2A00000 | (0xABCD<<5) | 5
    //   = 0xF2B579A5 — LE: A5 79 B5 F2.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const mkOp  = (*s)->opcodeByMnemonic("movk_lsl16");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(mkOp.has_value() && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x5{static_cast<std::uint32_t>(*(*s)->registerByName("x5")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(x5),
                               LirOperand::makeImmInt32(0xABCD) };
    (void)b.addInst(*mkOp, x5, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xA5);
    EXPECT_EQ(bytes[1], 0x79);
    EXPECT_EQ(bytes[2], 0xB5);
    EXPECT_EQ(bytes[3], 0xF2);
}

TEST(Arm64Encoder, MovkLsl32And48EncodeHwFields10And11) {
    // movk x9, #0xFFFF, LSL #32 → 0xF2C00000 | (0xFFFF<<5) | 9
    //   = 0xF2DFFFE9 — LE: E9 FF DF F2.
    // movk x21, #0x1234, LSL #48 → 0xF2E00000 | (0x1234<<5) | 21
    //   = 0xF2E24695 — LE: 95 46 E2 F2 (x21 = a >15 register encoding
    //   exercising the full Rd window under the hw=11 form).
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const mk32Op = (*s)->opcodeByMnemonic("movk_lsl32");
    auto const mk48Op = (*s)->opcodeByMnemonic("movk_lsl48");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(mk32Op.has_value() && mk48Op.has_value()
             && retOp.has_value());
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    LirReg const x9 {static_cast<std::uint32_t>(*(*s)->registerByName("x9")),  1, cls};
    LirReg const x21{static_cast<std::uint32_t>(*(*s)->registerByName("x21")), 1, cls};

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops32[] = { LirOperand::makeReg(x9),
                                 LirOperand::makeImmInt32(0xFFFF) };
    (void)b.addInst(*mk32Op, x9, ops32);
    LirOperand const ops48[] = { LirOperand::makeReg(x21),
                                 LirOperand::makeImmInt32(0x1234) };
    (void)b.addInst(*mk48Op, x21, ops48);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0xE9);
    EXPECT_EQ(bytes[1], 0xFF);
    EXPECT_EQ(bytes[2], 0xDF);
    EXPECT_EQ(bytes[3], 0xF2);
    EXPECT_EQ(bytes[4], 0x95);
    EXPECT_EQ(bytes[5], 0x46);
    EXPECT_EQ(bytes[6], 0xE2);
    EXPECT_EQ(bytes[7], 0xF2);
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

TEST(Arm64Encoder, BaseIndexLeaEncodesAddRegReg) {
    // D-AS4-ARM64-BASE-INDEX-LEA: the 4-op base+index lea
    // [base, index, MemBase(1), MemOffset(0)] = AArch64 `ADD Xd, Xn, Xm`
    // — the form `lowerGep` emits for an indexed pointer access `p[n]`,
    // closing the gap that made indexed access fail loud on arm64 (int
    // AND char alike). HOST-INDEPENDENT byte-pin: the runtime witness is
    // the qemu-gated char_ptr_indexed corpus (one leg), so this pin
    // guards the encoding on EVERY leg (§A.5 cross-target closure).
    //   lea X0, [X1 + X2*1 + 0] → ADD X0, X1, X2. Base 0x8B000000.
    //   Rd = X0 (0)  at bits 0..4   → 0x00
    //   Rn = X1 (1)  at bits 5..9   → 0x20
    //   Rm = X2 (2)  at bits 16..20 → 0x20000
    // word = 0x8B020020; LE: 20 00 02 8B. Distinct Rd/Rn/Rm → a wrong
    // fixedWord, an Rn↔Rm swap, or a dropped operand all fail.
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
        LirOperand::makeReg(gpr(**s, "x1")),   // base
        LirOperand::makeReg(gpr(**s, "x2")),   // index
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(0)
    };
    (void)b.addInst(*leaOp, gpr(**s, "x0"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x02);
    EXPECT_EQ(bytes[3], 0x8B);
}

TEST(Arm64Encoder, BaseIndexLeaNonZeroDispFailsLoud) {
    // RED-on-disable for the memoffset.zero guard: the base+index lea
    // (ADD Xd,Xn,Xm) has NO displacement field, so a nonzero disp MUST
    // fail loud — never silently drop the offset (a wrong address) and
    // never corrupt Rm (bits 10..21 overlap Rm at 16..20, so an Imm12
    // wiring would). A base+index+disp address needs a separate ADD,
    // which has no consumer yet.
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
        LirOperand::makeReg(gpr(**s, "x1")),
        LirOperand::makeReg(gpr(**s, "x2")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(8)   // nonzero — no field for it
    };
    (void)b.addInst(*leaOp, gpr(**s, "x0"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    (void)assembleFirstFn(lir, **s, rep);
    EXPECT_GT(rep.errorCount(), 0u)
        << "a base+index lea with a nonzero displacement must fail loud "
           "(memoffset.zero guard), never silently drop or mis-encode it";
}

TEST(Arm64Encoder, NegativeDispLeaEncodesNativeSubImm12) {
    // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB (c94): a NEGATIVE const-disp
    // GEP (`p[-N]` → the 3-op `lea Xd,[base + MemOffset(<0)]`) now encodes in
    // ONE NATIVE instruction `SUB Xd,Xn,#|disp|` — the negMemoffset sign
    // matcher routes it to the SUB variant; the encoder writes |disp| into the
    // unsigned imm12 field (the SUB base makes it a subtract). This REPLACES
    // c93's 5-7-instruction materialize-into-fresh-GPR + base+index fold (with
    // its DEAD const). HOST-INDEPENDENT byte-pin: the runtime witness is the
    // qemu-gated index_negative corpus (one leg), so this pin guards the native
    // SUB on EVERY leg (§A.5 cross-target closure).
    //   lea X2, [X1 + (-24)] → SUB X2, X1, #24. Base 0xD1000000.
    //   Rd    = X2 (2)  at bits 0..4   → 0x002
    //   Rn    = X1 (1)  at bits 5..9   → 0x020
    //   Imm12 = 24      at bits 10..21 → 24<<10 = 0x6000
    // word = 0xD1006022; LE: 22 60 00 D1. Distinct Rd≠Rn + the SUB op-bit
    // (0xD1 vs the ADD 0x91) → an ADD mis-encode, a dropped/negated disp, or
    // an Rd/Rn swap all fail (red-on-disable).
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
        LirOperand::makeReg(gpr(**s, "x1")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(-24)
    };
    (void)b.addInst(*leaOp, gpr(**s, "x2"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x22);
    EXPECT_EQ(bytes[1], 0x60);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0xD1);
}

TEST(Arm64Encoder, NegativeDispLeaAtImm12BoundaryEncodesSub) {
    // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB boundary pin: |disp| = 4095 is
    // the LARGEST magnitude the single-word SUB-imm12 form encodes (immMax:4095
    // on the negMemoffset variant). A magnitude 4096 falls through to the
    // shifted variant (next test). `lea X0, [X3 + (-4095)]` → SUB X0, X3, #4095:
    //   Rd=X0(0), Rn=X3(3)→0x60, imm12=4095(0xFFF)→0xFFF<<10 = 0x3FFC00
    // word = 0xD13FFC60; LE: 60 FC 3F D1.
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
        LirOperand::makeReg(gpr(**s, "x3")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(-4095)
    };
    (void)b.addInst(*leaOp, gpr(**s, "x0"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x60);
    EXPECT_EQ(bytes[1], 0xFC);
    EXPECT_EQ(bytes[2], 0x3F);
    EXPECT_EQ(bytes[3], 0xD1);
}

TEST(Arm64Encoder, NegativeDispLeaWiderThanImm12SplitsToShiftedSub) {
    // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB: a negative disp whose |disp| >
    // 4095 SPLITS into a 2-word `SUB Xd,Xn,#lo` + `SUB Xd,Xd,#hi,LSL#12` macro
    // (the imm12.hilo24 slot, SUB base) — the negative mirror of the positive
    // shifted ADD. `lea x2, [x1 + (-35996)]`, |disp| = 35996 = 0x8C9C →
    // lo = 0xC9C, hi = 0x8:
    //   word0 = SUB x2, x1, #0xC9C     : 0xD1000000|(0xC9C<<10)|(1<<5)|2 = 0xD1327022 → 22 70 32 D1
    //   word1 = SUB x2, x2, #0x8,LSL12 : 0xD1400000|(0x8<<10)|(2<<5)|2   = 0xD1402042 → 42 20 40 D1
    // word1 reads its OWN dest (x2) — SCRATCH-FREE. A missing sh=1 bit
    // (0x400000), a lo/hi swap, or an ADD-vs-SUB op-bit error all diverge.
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
        LirOperand::makeReg(gpr(**s, "x1")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(-35996)
    };
    (void)b.addInst(*leaOp, gpr(**s, "x2"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    // word0 (22 70 32 D1)
    EXPECT_EQ(bytes[0], 0x22);
    EXPECT_EQ(bytes[1], 0x70);
    EXPECT_EQ(bytes[2], 0x32);
    EXPECT_EQ(bytes[3], 0xD1);
    // word1 (42 20 40 D1)
    EXPECT_EQ(bytes[4], 0x42);
    EXPECT_EQ(bytes[5], 0x20);
    EXPECT_EQ(bytes[6], 0x40);
    EXPECT_EQ(bytes[7], 0xD1);
}

TEST(Arm64Encoder, NegativeDispLeaBeyond16MiBSplitsToMovzMovkSub) {
    // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB: a negative disp with |disp| >
    // 0xFFFFFF SPLITS into a 3-word MOVZ/MOVK + EXTENDED-register `SUB Xd,Xn,Xd`
    // macro — the negative mirror of the positive MOVZ/MOVK form. SCRATCH-FREE:
    // |disp| materializes into the DEST reg (x2), then `SUB x2, x1, x2`. UNLIKE
    // the positive form (Rn=sp baked), the NEGATIVE form WIRES Rn to the base
    // (x1). `lea x2, [x1 + (-0x1234000)]`, |disp| = 0x1234000, lo16 = 0x4000,
    // hi16 = 0x123:
    //   word0 = MOVZ x2,#0x4000       : 0xD2800000|(0x4000<<5)|2 = 0xD2880002 → 02 00 88 D2
    //   word1 = MOVK x2,#0x123,LSL#16 : 0xF2A00000|(0x123<<5)|2  = 0xF2A02462 → 62 24 A0 F2
    //   word2 = SUB x2,x1,x2 (EXTENDED): 0xCB206000|(1<<5)|(2<<16)|2 = 0xCB226022 → 22 60 22 CB
    // word2's FULL word is pinned (a shifted-register mis-encode 0xCB026022
    // would read x2 as the base instead of x1 — corrupt address; the ADD form
    // 0x8B... would ADD not SUB). Red-on-disable: revert the variant and
    // 0x1234000-magnitude fails A_NoMatchingEncodingVariant.
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
        LirOperand::makeReg(gpr(**s, "x1")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(-0x1234000)
    };
    (void)b.addInst(*leaOp, gpr(**s, "x2"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 12u);
    // word0 — MOVZ x2,#0x4000 (02 00 88 D2)
    EXPECT_EQ(bytes[0], 0x02);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x88);
    EXPECT_EQ(bytes[3], 0xD2);
    // word1 — MOVK x2,#0x123,LSL#16 (62 24 A0 F2)
    EXPECT_EQ(bytes[4], 0x62);
    EXPECT_EQ(bytes[5], 0x24);
    EXPECT_EQ(bytes[6], 0xA0);
    EXPECT_EQ(bytes[7], 0xF2);
    // word2 — SUB x2,x1,x2 EXTENDED (22 60 22 CB) — full word, F1 critical
    EXPECT_EQ(bytes[8],  0x22);
    EXPECT_EQ(bytes[9],  0x60);
    EXPECT_EQ(bytes[10], 0x22);
    EXPECT_EQ(bytes[11], 0xCB);
}

TEST(Arm64Encoder, FrameLeaOffsetWiderThanImm12SplitsToShifted) {
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12: a frame-lea offset wider
    // than the single-word imm12 reach (4095) is no longer fail-loud — it
    // SPLITS into a 2-word `ADD Xd,Xn,#lo` + `ADD Xd,Xd,#hi,LSL #12` macro
    // (the imm12.hilo24 slot). EXACT byte-pin for `lea x2, [sp + #35996]`:
    //   35996 = 0x8C9C → lo = 0xC9C, hi = 0x8.
    //   word0 = ADD x2, sp, #0xC9C  : 0x91000000 | (0xC9C<<10) | (31<<5) | 2
    //         = 0x913273E2 → LE bytes E2 73 32 91
    //   word1 = ADD x2, x2, #0x8,LSL#12 : 0x91400000 | (0x8<<10) | (2<<5) | 2
    //         = 0x91402042 → LE bytes 42 20 40 91
    // word1 reads its OWN dest (x2) — SCRATCH-FREE. A wrong split (lo/hi
    // swap), a missing sh=1 bit (0x400000) in word1, or a dropped Rd-thread
    // through word1.Rn all diverge these bytes (red-on-disable).
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
        LirOperand::makeMemOffset(35996)
    };
    (void)b.addInst(*leaOp, gpr(**s, "x2"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    // word0 (E2 73 32 91)
    EXPECT_EQ(bytes[0], 0xE2);
    EXPECT_EQ(bytes[1], 0x73);
    EXPECT_EQ(bytes[2], 0x32);
    EXPECT_EQ(bytes[3], 0x91);
    // word1 (42 20 40 91)
    EXPECT_EQ(bytes[4], 0x42);
    EXPECT_EQ(bytes[5], 0x20);
    EXPECT_EQ(bytes[6], 0x40);
    EXPECT_EQ(bytes[7], 0x91);
}

TEST(Arm64Encoder, FrameLeaOffsetBeyond16MiBSplitsToMovzMovkThreeWord) {
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB (the high-element lea tier): a
    // frame-lea offset > 0xFFFFFF SPLITS into a 3-word MOVZ/MOVK + EXTENDED-
    // register macro. SCRATCH-FREE — the displacement materializes into the
    // lea's DEST reg (here x2), then `add x2, sp, x2`. EXACT byte-pin for
    // `lea x2, [sp + #0x1234000]` (lo16 = 0x4000, hi16 = 0x0123):
    //   word0 = MOVZ x2,#0x4000       : 0xD2800000|(0x4000<<5)|2 = 0xD2880002 → 02 00 88 D2
    //   word1 = MOVK x2,#0x123,LSL#16 : 0xF2A00000|(0x123<<5)|2  = 0xF2A02462 → 62 24 A0 F2
    //   word2 = ADD x2,sp,x2 (EXTENDED): 0x8B2063E0|(2<<16)|2    = 0x8B2263E2 → E2 63 22 8B
    // word2 is the EXTENDED-register ADD (Rn=sp=31, Rd=Rm=x2) — the dest reg
    // threads through ALL THREE words (resultSlot + extraResultSlots). The
    // FULL word2 is pinned (a shifted-register mis-encode 0x8B0263E2 would
    // read x2 as the base instead of sp — corrupt address). Red-on-disable:
    // revert the variant / immMax and 0x1234000 fails A_NoMatchingEncodingVariant.
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
        LirOperand::makeMemOffset(0x1234000)
    };
    (void)b.addInst(*leaOp, gpr(**s, "x2"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 12u);
    // word0 — MOVZ x2,#0x4000 (02 00 88 D2)
    EXPECT_EQ(bytes[0], 0x02);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x88);
    EXPECT_EQ(bytes[3], 0xD2);
    // word1 — MOVK x2,#0x123,LSL#16 (62 24 A0 F2)
    EXPECT_EQ(bytes[4], 0x62);
    EXPECT_EQ(bytes[5], 0x24);
    EXPECT_EQ(bytes[6], 0xA0);
    EXPECT_EQ(bytes[7], 0xF2);
    // word2 — ADD x2,sp,x2 EXTENDED (E2 63 22 8B) — full word, F1 critical
    EXPECT_EQ(bytes[8],  0xE2);
    EXPECT_EQ(bytes[9],  0x63);
    EXPECT_EQ(bytes[10], 0x22);
    EXPECT_EQ(bytes[11], 0x8B);
}

TEST(Arm64Encoder, FrameLeaOffsetAtMaxInt32EncodesThreeWord) {
    // Boundary pin: 0x7FFFFFFF (the int32 frame ceiling) is the LARGEST
    // offset the MOVZ/MOVK form encodes. lo16 = 0xFFFF, hi16 = 0x7FFF;
    // `lea x2, [sp + #0x7FFFFFFF]`:
    //   word0 = MOVZ x2,#0xFFFF       : 0xD29FFFE2 → E2 FF 9F D2
    //   word1 = MOVK x2,#0x7FFF,LSL16 : 0xF2AFFFE2 → E2 FF AF F2
    //   word2 = ADD x2,sp,x2          : 0x8B2263E2 → E2 63 22 8B
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
        LirOperand::makeMemOffset(0x7FFFFFFF)
    };
    (void)b.addInst(*leaOp, gpr(**s, "x2"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 12u);
    EXPECT_EQ(bytes[0], 0xE2);
    EXPECT_EQ(bytes[1], 0xFF);
    EXPECT_EQ(bytes[2], 0x9F);
    EXPECT_EQ(bytes[3], 0xD2);
    EXPECT_EQ(bytes[4], 0xE2);
    EXPECT_EQ(bytes[5], 0xFF);
    EXPECT_EQ(bytes[6], 0xAF);
    EXPECT_EQ(bytes[7], 0xF2);
    EXPECT_EQ(bytes[8],  0xE2);
    EXPECT_EQ(bytes[9],  0x63);
    EXPECT_EQ(bytes[10], 0x22);
    EXPECT_EQ(bytes[11], 0x8B);
}

TEST(Arm64Encoder, SubSpFrameWiderThanImm12SplitsToShifted) {
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12 (prologue): the callconv's
    // `sub sp, sp, #frame` ([reg, ImmInt] form) splits when frame > 4095.
    // EXACT byte-pin for `sub sp, sp, #36032` (36032 = 0x8CC0; lo = 0xCC0,
    // hi = 0x8; Rd = Rn = sp = 31):
    //   word0 = SUB sp,sp,#0xCC0 : 0xD1000000|(0xCC0<<10)|(31<<5)|31 = 0xD13303FF → FF 03 33 D1
    //   word1 = SUB sp,sp,#0x8,LSL#12 : 0xD1400000|(0x8<<10)|(31<<5)|31 = 0xD14023FF → FF 23 40 D1
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const subOp = (*s)->opcodeByMnemonic("sub");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(subOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeImmInt32(36032)
    };
    (void)b.addInst(*subOp, gpr(**s, "sp"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0xFF);
    EXPECT_EQ(bytes[1], 0x03);
    EXPECT_EQ(bytes[2], 0x33);
    EXPECT_EQ(bytes[3], 0xD1);
    EXPECT_EQ(bytes[4], 0xFF);
    EXPECT_EQ(bytes[5], 0x23);
    EXPECT_EQ(bytes[6], 0x40);
    EXPECT_EQ(bytes[7], 0xD1);
}

TEST(Arm64Encoder, AddSpFrameWiderThanImm12SplitsToShifted) {
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12 (epilogue): `add sp, sp,
    // #frame` splits when frame > 4095. EXACT byte-pin for `add sp, sp,
    // #36032` (same 0x8CC0 split, ADD base 0x91000000 / 0x91400000):
    //   word0 = ADD sp,sp,#0xCC0 : 0x913303FF → FF 03 33 91
    //   word1 = ADD sp,sp,#0x8,LSL#12 : 0x914023FF → FF 23 40 91
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
        LirOperand::makeImmInt32(36032)
    };
    (void)b.addInst(*addOp, gpr(**s, "sp"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0xFF);
    EXPECT_EQ(bytes[1], 0x03);
    EXPECT_EQ(bytes[2], 0x33);
    EXPECT_EQ(bytes[3], 0x91);
    EXPECT_EQ(bytes[4], 0xFF);
    EXPECT_EQ(bytes[5], 0x23);
    EXPECT_EQ(bytes[6], 0x40);
    EXPECT_EQ(bytes[7], 0x91);
}

TEST(Arm64Encoder, FrameSubSpAtImm12BoundaryStaysSingleWord) {
    // Boundary pin: 4095 (the imm12 max) selects the SINGLE-word variant
    // (immMax:4095) — 4 bytes, no shift word. `sub sp, sp, #4095`:
    //   0xD1000000 | (4095<<10) | (31<<5) | 31 = 0xD13FFFFF → FF FF 3F D1
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const subOp = (*s)->opcodeByMnemonic("sub");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(subOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeImmInt32(4095)
    };
    (void)b.addInst(*subOp, gpr(**s, "sp"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    // sub (4 bytes) + ret (4 bytes) = 8; the single-word sub is the FIRST 4.
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xFF);
    EXPECT_EQ(bytes[1], 0xFF);
    EXPECT_EQ(bytes[2], 0x3F);
    EXPECT_EQ(bytes[3], 0xD1);
}

TEST(Arm64Encoder, FrameSubSpJustPastImm12IsTwoWord) {
    // Boundary pin: 4096 (one past imm12 max) selects the 2-word shifted
    // variant (immMin:4096). `sub sp, sp, #4096` (lo = 0, hi = 1):
    //   word0 = SUB sp,sp,#0 : 0xD1000000|(0<<10)|(31<<5)|31 = 0xD10003FF → FF 03 00 D1
    //   word1 = SUB sp,sp,#1,LSL#12 : 0xD1400000|(1<<10)|(31<<5)|31 = 0xD14007FF → FF 07 40 D1
    // The 2-word emission (8 bytes prologue, not 4) is itself the proof the
    // 4096 boundary routes to the shifted form.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const subOp = (*s)->opcodeByMnemonic("sub");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(subOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeImmInt32(4096)
    };
    (void)b.addInst(*subOp, gpr(**s, "sp"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0xFF);
    EXPECT_EQ(bytes[1], 0x03);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0xD1);
    EXPECT_EQ(bytes[4], 0xFF);
    EXPECT_EQ(bytes[5], 0x07);
    EXPECT_EQ(bytes[6], 0x40);
    EXPECT_EQ(bytes[7], 0xD1);
}

TEST(Arm64Encoder, FrameSubSpAt16MiBMinusOneIsTwoWord) {
    // Boundary pin: 16777215 (0xFFFFFF, the 24-bit max) is the LARGEST
    // value the shifted word-pair encodes. lo = 0xFFF, hi = 0xFFF:
    //   word0 = SUB sp,sp,#0xFFF : 0xD1000000|(0xFFF<<10)|(31<<5)|31 = 0xD13FFFFF → FF FF 3F D1
    //   word1 = SUB sp,sp,#0xFFF,LSL#12 : 0xD1400000|(0xFFF<<10)|(31<<5)|31 = 0xD17FFFFF → FF FF 7F D1
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const subOp = (*s)->opcodeByMnemonic("sub");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(subOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeImmInt32(16777215)
    };
    (void)b.addInst(*subOp, gpr(**s, "sp"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0xFF);
    EXPECT_EQ(bytes[1], 0xFF);
    EXPECT_EQ(bytes[2], 0x3F);
    EXPECT_EQ(bytes[3], 0xD1);
    EXPECT_EQ(bytes[4], 0xFF);
    EXPECT_EQ(bytes[5], 0xFF);
    EXPECT_EQ(bytes[6], 0x7F);
    EXPECT_EQ(bytes[7], 0xD1);
}

TEST(Arm64Encoder, SubSpFrameBeyond16MiBSplitsToMovzMovkThreeWord) {
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB (prologue): the callconv's
    // `sub sp, sp, #frame` SPLITS into a 3-word MOVZ/MOVK + EXTENDED-register
    // macro when frame > 0xFFFFFF (16 MiB). x16 (IP0) is the baked scratch.
    // EXACT byte-pin for `sub sp, sp, #0x1234000` (lo16 = 0x4000, hi16 =
    // 0x0123 — BOTH halves nonzero, so a lo/hi swap diverges these bytes):
    //   word0 = MOVZ x16,#0x4000        : 0xD2800010|(0x4000<<5) = 0xD2880010 → 10 00 88 D2
    //   word1 = MOVK x16,#0x123,LSL#16  : 0xF2A00010|(0x123<<5)  = 0xF2A02470 → 70 24 A0 F2
    //   word2 = SUB sp,sp,x16 (EXTENDED): 0xCB3063FF             → FF 63 30 CB
    // word2 is the EXTENDED-register form (Rd=Rn=sp=31, Rm=x16=16, opt=UXTX,
    // bit 21 set) — NOT the shifted-register 0xCB000000 (where #31=XZR would
    // make this `sp - 0` = corrupt frame). The FULL 32-bit word2 is pinned
    // (a low-byte-only pin would pass the XZR mis-encode: 0xCB0063FF and
    // 0xCB3063FF share byte[0]=0xFF). Red-on-disable: revert the variant /
    // immMax, and 0x1234000 fails A_NoMatchingEncodingVariant.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const subOp = (*s)->opcodeByMnemonic("sub");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(subOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeImmInt32(0x1234000)
    };
    (void)b.addInst(*subOp, gpr(**s, "sp"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 12u);
    // word0 — MOVZ x16,#0x4000 (10 00 88 D2)
    EXPECT_EQ(bytes[0], 0x10);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x88);
    EXPECT_EQ(bytes[3], 0xD2);
    // word1 — MOVK x16,#0x123,LSL#16 (70 24 A0 F2)
    EXPECT_EQ(bytes[4], 0x70);
    EXPECT_EQ(bytes[5], 0x24);
    EXPECT_EQ(bytes[6], 0xA0);
    EXPECT_EQ(bytes[7], 0xF2);
    // word2 — SUB sp,sp,x16 EXTENDED (FF 63 30 CB) — full word, F1 critical
    EXPECT_EQ(bytes[8],  0xFF);
    EXPECT_EQ(bytes[9],  0x63);
    EXPECT_EQ(bytes[10], 0x30);
    EXPECT_EQ(bytes[11], 0xCB);
}

TEST(Arm64Encoder, AddSpFrameBeyond16MiBSplitsToMovzMovkThreeWord) {
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB (EPILOGUE): the callconv's
    // `add sp, sp, #frame` SPLITS to the SAME 3-word MOVZ/MOVK + EXTENDED form
    // as the prologue sub, differing ONLY in word2's op bit (ADD=0 vs SUB=1).
    // EXACT byte-pin for `add sp, sp, #0x1234000` — word0/word1 identical to the
    // sub pin; word2 = ADD sp,sp,x16 EXTENDED = 0x8B3063FF → FF 63 30 8B (NOT
    // the shifted-register 0x8B000000 where #31=XZR). Closes the dss-audit
    // coverage gap (the epilogue add was runtime-only before).
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
        LirOperand::makeImmInt32(0x1234000)
    };
    (void)b.addInst(*addOp, gpr(**s, "sp"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 12u);
    EXPECT_EQ(bytes[0], 0x10); EXPECT_EQ(bytes[1], 0x00);  // MOVZ x16,#0x4000
    EXPECT_EQ(bytes[2], 0x88); EXPECT_EQ(bytes[3], 0xD2);
    EXPECT_EQ(bytes[4], 0x70); EXPECT_EQ(bytes[5], 0x24);  // MOVK x16,#0x123,LSL16
    EXPECT_EQ(bytes[6], 0xA0); EXPECT_EQ(bytes[7], 0xF2);
    // word2 — ADD sp,sp,x16 EXTENDED (FF 63 30 8B) — full word
    EXPECT_EQ(bytes[8],  0xFF);
    EXPECT_EQ(bytes[9],  0x63);
    EXPECT_EQ(bytes[10], 0x30);
    EXPECT_EQ(bytes[11], 0x8B);
}

TEST(Arm64Encoder, FrameSubSpAt16MiBSplitsToThreeWord) {
    // Boundary pin: 16777216 (0x1000000, the 24-bit ceiling + 1) NO LONGER
    // fails loud — it is the FIRST value routed to the 3-word MOVZ/MOVK form
    // (immMin:16777216). lo16 = 0, hi16 = 0x0100:
    //   word0 = MOVZ x16,#0          : 0xD2800010 → 10 00 80 D2
    //   word1 = MOVK x16,#0x100,LSL16: 0xF2A02010 → 10 20 A0 F2
    //   word2 = SUB sp,sp,x16        : 0xCB3063FF → FF 63 30 CB
    // The 12-byte emission (not 8, not a diagnostic) IS the proof the
    // 0x1000000 boundary now routes to the 3-word form.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const subOp = (*s)->opcodeByMnemonic("sub");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(subOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeImmInt32(16777216)
    };
    (void)b.addInst(*subOp, gpr(**s, "sp"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 12u);
    EXPECT_EQ(bytes[0], 0x10);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x80);
    EXPECT_EQ(bytes[3], 0xD2);
    EXPECT_EQ(bytes[4], 0x10);
    EXPECT_EQ(bytes[5], 0x20);
    EXPECT_EQ(bytes[6], 0xA0);
    EXPECT_EQ(bytes[7], 0xF2);
    EXPECT_EQ(bytes[8],  0xFF);
    EXPECT_EQ(bytes[9],  0x63);
    EXPECT_EQ(bytes[10], 0x30);
    EXPECT_EQ(bytes[11], 0xCB);
}

TEST(Arm64Encoder, FrameSubSpAt2GiBFailsLoud) {
    // Boundary pin (the NEW ceiling, D-ASM-AARCH64-FRAME-OFFSET-BEYOND-2GIB):
    // 0x80000000 (2 GiB) is the first value PAST the int32 frame ceiling. The
    // frame size flows through `emitSpAdjust`'s `static_cast<int32_t>(bytes)`
    // and `variantImmMagnitude`'s `immInt32` — 0x80000000 as int32 is
    // NEGATIVE, so `variantImmMagnitude` returns nullopt → NO variant matches
    // (the 3-word variant's immMin:16777216 is never reached) → fail-loud
    // A_NoMatchingEncodingVariant. A >2GiB frame is absurd; it must never
    // silently wrap to a small (or negative) frame.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const subOp = (*s)->opcodeByMnemonic("sub");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(subOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        // 0x80000000 stored into the int32 immInt32 field → INT32_MIN.
        LirOperand::makeImmInt32(static_cast<std::int32_t>(0x80000000u))
    };
    (void)b.addInst(*subOp, gpr(**s, "sp"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    (void)assemble(lir, **s, lirToMir, rep);
    EXPECT_GT(rep.errorCount(), 0u)
        << "a >2GiB sub-sp frame must fail loud (no matching variant), "
           "never silently wrap the frame size";
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

TEST(Arm64Encoder, FmulEncodesScalarDouble) {
    // Cluster-F F3: fmul d0, d1, d2 — FP data-proc (2 source), double:
    //   base 0x1E600800 (FMUL opcode field [15:10]=000010, vs FADD's 001010)
    //   | Rm=d2(2)<<16 | Rn=d1(1)<<5 = 0x1E620820 — LE bytes: 20 08 62 1E.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const fmulOp = (*s)->opcodeByMnemonic("fmul");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(fmulOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(fpr(**s, "d1")),
                               LirOperand::makeReg(fpr(**s, "d2")) };
    (void)b.addInst(*fmulOp, fpr(**s, "d0"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x08);
    EXPECT_EQ(bytes[2], 0x62);
    EXPECT_EQ(bytes[3], 0x1E);
}

TEST(Arm64Encoder, FsubEncodesScalarDoubleNonCommutative) {
    // Cluster-F F3: fsub d0, d1, d2 = d1 - d2 — FP data-proc (2 source), double:
    //   base 0x1E603800 (FSUB opcode field [15:10]=001110, = FADD | 0x1000)
    //   | Rm=d2(2)<<16 | Rn=d1(1)<<5 = 0x1E623820 — LE bytes: 20 38 62 1E.
    //   Rn(d1)=minuend, Rm(d2)=subtrahend: a SWAPPED Rn<->Rm wire flips the sign.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const fsubOp = (*s)->opcodeByMnemonic("fsub");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(fsubOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(fpr(**s, "d1")),
                               LirOperand::makeReg(fpr(**s, "d2")) };
    (void)b.addInst(*fsubOp, fpr(**s, "d0"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x38);
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

// FC12c (D-FC12C-AAPCS64-VARIADIC-CALLEE, BLOCKER-1): the 128-bit FPR store fstur_q
// (STUR Qt, [Xn, #simm9]) used to spill v0..v7 into the AAPCS64 variadic VR save
// block (16-byte slots). The fixedWord MUST be 0x3C800000 — 0x3C000000 is STURB
// (8-bit byte store), which would spill 1 byte per FP slot → va_arg(double) garbage.
// Byte-pin STUR Q0,[X1,#0]:
//   base 0x3C800000
//   | Rn = x1 (1) << 5 → 0x00000020
//   | Rt = q0 (0)      → 0x00000000
//   | Imm9 = 0         → 0x00000000
//   = 0x3C800020 — LE bytes: 20 00 80 3C. A regression to 0x3C000000 (STURB) flips
//   byte[3] to 0x38 (and would silently mis-encode the whole VR spill).
TEST(Arm64Encoder, FsturQEncodesByteExact) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const stOp  = (*s)->opcodeByMnemonic("fstur_q");
    auto const retOp = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(stOp.has_value()) << "arm64 must declare the fstur_q opcode (FC12c)";
    ASSERT_TRUE(retOp.has_value());

    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(fpr(**s, "d0")),   // q0 shares the d0 ordinal (V register)
        LirOperand::makeReg(gpr(**s, "x1")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(0)
    };
    (void)b.addInst(*stOp, InvalidLirReg, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    // 0x3C800020, little-endian.
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x80);
    EXPECT_EQ(bytes[3], 0x3C)
        << "fstur_q byte[3] must be 0x3C (STUR Q) — 0x38 would be STURB (1-byte)";
}

// FC12c (BLOCKER-1): the matching 128-bit Q disasm arm round-trips. Disassemble a
// hand-encoded STUR Q3,[X5,#16] and verify the Rd/Rn/Imm9 wires decode (proves the
// fixed32_disasm windowFor learned the Imm9 + MemBaseNoScale slots fstur_q uses).
TEST(Arm64Encoder, FsturQDisassemblesRoundTrip) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const stOp = (*s)->opcodeByMnemonic("fstur_q");
    ASSERT_TRUE(stOp.has_value());
    // STUR Q3, [X5, #16]: 0x3C800000 | (16<<12)=0x10000 | (5<<5)=0xA0 | 3 = 0x3C8100A3.
    // LE bytes: A3 00 81 3C.
    std::array<std::uint8_t, 4> bytes{0xA3, 0x00, 0x81, 0x3C};
    DiagnosticReporter rep;
    auto disasmed = disassembleInst(**s, *stOp, bytes, rep);
    ASSERT_TRUE(disasmed.has_value())
        << "fstur_q must disassemble — the fixed32_disasm Imm9/MemBaseNoScale window";
    EXPECT_EQ(rep.errorCount(), 0u);
    // result slot: none (a store). Wires: rd(Rt=3), rn(Rn=5), membase(no bits), imm9(16).
    ASSERT_FALSE(disasmed->result.has_value());
    ASSERT_EQ(disasmed->wires.size(), 4u);
    // wire[0] = rd (Rt) = q3.
    EXPECT_EQ(disasmed->wires[0].kind, EncodingSlotKind::Rd);
    ASSERT_TRUE(disasmed->wires[0].value.has_value());
    EXPECT_EQ(*disasmed->wires[0].value, 3);
    // wire[1] = rn (Rn) = x5.
    EXPECT_EQ(disasmed->wires[1].kind, EncodingSlotKind::Rn);
    ASSERT_TRUE(disasmed->wires[1].value.has_value());
    EXPECT_EQ(*disasmed->wires[1].value, 5);
    // wire[3] = imm9 = 16.
    EXPECT_EQ(disasmed->wires[3].kind, EncodingSlotKind::Imm9);
    ASSERT_TRUE(disasmed->wires[3].value.has_value());
    EXPECT_EQ(*disasmed->wires[3].value, 16);
}

// ── D-ASM-AARCH64-LARGE-FRAME-IMM12: the scaled unsigned-offset LDR/STR ──
//
// Byte-pins for the new `load_u`/`store_u` (scaled imm12) forms. The
// scaling is load-bearing: the encoded field is byteOffset/accessSize
// (e.g. 192/8 = 24 for a 64-bit LDR), NOT the raw byte offset. Each pin
// also catches a bit-24 LDUR-vs-LDR error (the unsigned-offset mode bit)
// and, for the 32-bit pin, the access-size scale (4, not 8).

TEST(Arm64Encoder, LoadUnsignedOffsetEncodes64Bit) {
    // load_u X1, [SP, #192] → LDR X1, [SP, #192]. Base 0xF9400000.
    //   Rt = X1 (1) at 0..4; Rn = SP (31) at 5..9; imm12 = 192/8 = 24 at 10..21.
    // word = 0xF94063E1; LE: E1 63 40 F9. (raw 192 would be a wild offset.)
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const loadOp = (*s)->opcodeByMnemonic("load_u");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(loadOp.has_value()) << "arm64 must declare load_u (D-ASM-AARCH64-LARGE-FRAME-IMM12)";
    ASSERT_TRUE(retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(192)
    };
    (void)b.addInst(*loadOp, gpr(**s, "x1"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xE1);
    EXPECT_EQ(bytes[1], 0x63);
    EXPECT_EQ(bytes[2], 0x40);
    EXPECT_EQ(bytes[3], 0xF9)
        << "load_u byte[3] must be 0xF9 (LDR unsigned-offset) — 0xF8 is LDUR (imm9)";
}

TEST(Arm64Encoder, StoreUnsignedOffsetEncodes64Bit) {
    // store_u X1, [SP, #192] → STR X1, [SP, #192]. Base 0xF9000000.
    //   Rt(value) = X1 (1) at 0..4; Rn = SP (31) at 5..9; imm12 = 24.
    // word = 0xF90063E1; LE: E1 63 00 F9.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const storeOp = (*s)->opcodeByMnemonic("store_u");
    auto const retOp   = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(storeOp.has_value()) << "arm64 must declare store_u";
    ASSERT_TRUE(retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "x1")),
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(192)
    };
    (void)b.addInst(*storeOp, InvalidLirReg, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xE1);
    EXPECT_EQ(bytes[1], 0x63);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0xF9);
}

TEST(Arm64Encoder, LoadUnsignedOffsetEncodes32Bit) {
    // load_u(width 32) W2, [X3, #64] → LDR W2, [X3, #64]. Base 0xB9400000.
    //   Rt = W2 (2) at 0..4; Rn = X3 (3) at 5..9; accessSize=4 ⇒ imm12 = 64/4 = 16.
    // word = 0xB9404062; LE: 62 40 40 B9. (a /8 scale would give imm12=8 → 0x39 wrong.)
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const loadOp = (*s)->opcodeByMnemonic("load_u");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(loadOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "x3")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(64)
    };
    // width 32 selects the LDR Wt variant (accessSize 4).
    (void)b.addInst(*loadOp, gpr(**s, "x2"), ops, /*payload=*/0, kLirInstFlagWidth32);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x62);
    EXPECT_EQ(bytes[1], 0x40);
    EXPECT_EQ(bytes[2], 0x40);
    EXPECT_EQ(bytes[3], 0xB9)
        << "load_u(32) byte[3] must be 0xB9 (LDR W unsigned-offset)";
}

TEST(Arm64Encoder, LoadUnsignedOffsetNonMultipleOfAccessSizeFailsLoud) {
    // RED-on-disable for the alignment guard: a 64-bit access (accessSize 8)
    // at byte offset 7 is NOT a multiple of 8 → the scaled field has no
    // encoding (an unaligned LDR is architecturally undefined). Fail loud.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const loadOp = (*s)->opcodeByMnemonic("load_u");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(loadOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(7)
    };
    (void)b.addInst(*loadOp, gpr(**s, "x1"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    (void)assemble(lir, **s, lirToMir, rep);
    bool sawOutOfRange = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::A_ImmediateOperandOutOfRange) sawOutOfRange = true;
    }
    EXPECT_TRUE(sawOutOfRange)
        << "a non-access-size-aligned scaled offset must emit A_ImmediateOperandOutOfRange";
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(Arm64Encoder, LoadUnsignedOffsetTooLargeFailsLoud) {
    // RED-on-disable for the >4095 guard: a 64-bit access (accessSize 8) at
    // byte offset 32768 scales to 4096, one past the 12-bit field max
    // (0..4095). Fail loud (a larger frame needs the shifted imm12<<12 form).
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const loadOp = (*s)->opcodeByMnemonic("load_u");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(loadOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(32768)   // 32768/8 = 4096 > 4095
    };
    (void)b.addInst(*loadOp, gpr(**s, "x1"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    (void)assemble(lir, **s, lirToMir, rep);
    bool sawOutOfRange = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::A_ImmediateOperandOutOfRange) sawOutOfRange = true;
    }
    EXPECT_TRUE(sawOutOfRange)
        << "a scaled offset > 4095 must emit A_ImmediateOperandOutOfRange";
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(Arm64Encoder, LoadUnsignedOffsetNegativeFailsLoud) {
    // RED-on-disable for the non-negative guard: the unsigned-offset form has
    // no sign bit, so a negative displacement (-8) must fail loud rather than
    // wrap into a huge positive offset. (The signed unscaled imm9 LDUR is the
    // form for small negative offsets; this scaled form rejects them.)
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const loadOp = (*s)->opcodeByMnemonic("load_u");
    auto const retOp  = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(loadOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(-8)
    };
    (void)b.addInst(*loadOp, gpr(**s, "x1"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    (void)assemble(lir, **s, lirToMir, rep);
    bool sawOutOfRange = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::A_ImmediateOperandOutOfRange) sawOutOfRange = true;
    }
    EXPECT_TRUE(sawOutOfRange)
        << "a negative scaled offset must emit A_ImmediateOperandOutOfRange";
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(Arm64Encoder, LoadUnsignedOffsetDisassemblesRoundTrip) {
    // The disasm oracle round-trips the scaled imm12. Disassemble a hand-
    // encoded LDR X1,[SP,#192] (0xF94063E1) and verify the decoder extracts
    // the RAW scaled field (imm12 == 24), NOT the byte offset (192), plus
    // Rt==1 and Rn==31 — proving fixed32_disasm.windowFor learned Imm12Scaled.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const loadOp = (*s)->opcodeByMnemonic("load_u");
    ASSERT_TRUE(loadOp.has_value());
    // 0xF94063E1, LE bytes: E1 63 40 F9.
    std::array<std::uint8_t, 4> bytes{0xE1, 0x63, 0x40, 0xF9};
    DiagnosticReporter rep;
    auto disasmed = disassembleInst(**s, *loadOp, bytes, rep);
    ASSERT_TRUE(disasmed.has_value())
        << "load_u must disassemble — the fixed32_disasm Imm12Scaled window";
    EXPECT_EQ(rep.errorCount(), 0u);
    // result slot: rd (Rt) = x1.
    ASSERT_TRUE(disasmed->result.has_value());
    EXPECT_EQ(disasmed->result->kind, EncodingSlotKind::Rd);
    ASSERT_TRUE(disasmed->result->value.has_value());
    EXPECT_EQ(*disasmed->result->value, 1);
    ASSERT_EQ(disasmed->wires.size(), 3u);
    // wire[0] = rn (Rn) = sp (31).
    EXPECT_EQ(disasmed->wires[0].kind, EncodingSlotKind::Rn);
    ASSERT_TRUE(disasmed->wires[0].value.has_value());
    EXPECT_EQ(*disasmed->wires[0].value, 31);
    // wire[2] = imm12.scaled = 24 (the RAW scaled field, NOT 192).
    EXPECT_EQ(disasmed->wires[2].kind, EncodingSlotKind::Imm12Scaled);
    ASSERT_TRUE(disasmed->wires[2].value.has_value());
    EXPECT_EQ(*disasmed->wires[2].value, 24)
        << "the disasm oracle pins the RAW imm12 (24), NOT the byte offset (192)";
}

// ── D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12 (load/store-displacement half):
//    imm9/scaled-imm12 BOUNDARY byte-pins ──
//
// The frame load/store chokepoint (selectFrameMemOp, lir_callconv.cpp) swaps
// the UNSCALED imm9 form (LDUR/STUR, ±256) to the SCALED imm12 form (LDR/STR,
// 0..4095×size) exactly when the offset leaves the imm9 reach. These pins lock
// the encoder boundary the chokepoint's threshold (offset > 255) is keyed to:
// at #255 the UNSCALED `store`/`load` (STUR/LDUR, byte[3]=0xF8) still encodes;
// at #256 the chokepoint must pick `store_u`/`load_u` (STR/LDR, byte[3]=0xF9).
// A regression that shifted either the encoder reach or the selection threshold
// diverges these bytes (red-on-disable, host-independent — every CI leg).

TEST(Arm64Encoder, StoreUnscaledImm9AtBoundary255Encodes) {
    // store X1, [SP, #255] → STUR X1, [SP, #255] (imm9 max-positive). Base
    // 0xF8000000 | (255<<12) | (31<<5) | 1 = 0xF80FF3E1; LE: E1 F3 0F F8.
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
        LirOperand::makeMemOffset(255)
    };
    (void)b.addInst(*storeOp, InvalidLirReg, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "offset 255 is the imm9 max-positive — STUR must still encode";
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xE1);
    EXPECT_EQ(bytes[1], 0xF3);
    EXPECT_EQ(bytes[2], 0x0F);
    EXPECT_EQ(bytes[3], 0xF8)
        << "byte[3] must be 0xF8 (STUR, unscaled imm9) at the #255 boundary";
}

TEST(Arm64Encoder, StoreUnscaledImm9JustPast255FailsLoud) {
    // store X1, [SP, #256] via the UNSCALED `store` (STUR imm9) fails loud —
    // 256 is one past the imm9 +255 max. (The chokepoint would instead pick
    // store_u; this pins that the unscaled form genuinely cannot reach #256, so
    // the swap is load-bearing, not cosmetic.)
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
        LirOperand::makeMemOffset(256)
    };
    (void)b.addInst(*storeOp, InvalidLirReg, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    (void)assemble(lir, **s, lirToMir, rep);
    bool sawOutOfRange = false;
    for (auto const& d : rep.all())
        if (d.code == DiagnosticCode::A_ImmediateOperandOutOfRange)
            sawOutOfRange = true;
    EXPECT_TRUE(sawOutOfRange)
        << "the UNSCALED store (STUR imm9) must fail loud at #256 — the swap to "
           "store_u is what makes #256 encodable";
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(Arm64Encoder, StoreScaledImm12AtBoundary256Encodes) {
    // store_u X1, [SP, #256] → STR X1, [SP, #256] (scaled imm12 = 256/8 = 32).
    // Base 0xF9000000 | (32<<10) | (31<<5) | 1 = 0xF90083E1; LE: E1 83 00 F9.
    // This is the form the chokepoint picks for the same #256 the unscaled
    // store rejects above (the boundary handoff).
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const storeUOp = (*s)->opcodeByMnemonic("store_u");
    auto const retOp    = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(storeUOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "x1")),
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(256)
    };
    (void)b.addInst(*storeUOp, InvalidLirReg, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xE1);
    EXPECT_EQ(bytes[1], 0x83);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0xF9)
        << "byte[3] must be 0xF9 (STR, scaled imm12) at the #256 boundary — the "
           "chokepoint's swap target";
}

TEST(Arm64Encoder, LoadScaledImm12AtImm12MaxEncodes) {
    // load_u X1, [SP, #32760] → LDR X1, [SP, #32760] (scaled imm12 = 32760/8 =
    // 4095, the field max). Base 0xF9400000 | (4095<<10) | (31<<5) | 1 =
    // 0xF97FFFE1; LE: E1 FF 7F F9. One scaled step past this (#32768 → 4096)
    // is the fail-loud LoadUnsignedOffsetTooLargeFailsLoud pin above.
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const loadUOp = (*s)->opcodeByMnemonic("load_u");
    auto const retOp   = (*s)->opcodeByMnemonic("ret");
    ASSERT_TRUE(loadUOp.has_value() && retOp.has_value());
    LirBuilder b{**s};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(gpr(**s, "sp")),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(32760)
    };
    (void)b.addInst(*loadUOp, gpr(**s, "x1"), ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter rep;
    auto bytes = assembleFirstFn(lir, **s, rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "offset 32760 scales to 4095 (the imm12 field max) — must encode";
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xE1);
    EXPECT_EQ(bytes[1], 0xFF);
    EXPECT_EQ(bytes[2], 0x7F);
    EXPECT_EQ(bytes[3], 0xF9);
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
    (void)mb.addGlobal(f64, SymbolId{500}, mb.literalPoolAdd(quarter),
                       MirFuncId{}, SymbolBinding::Global,
                       SymbolVisibility::Default, /*isConst=*/false,
                       MirThreadStorage::Shared);
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
