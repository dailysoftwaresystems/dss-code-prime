// x86_64 binary ops byte-encoding tests — plan 13 AS3.
//
// Pins:
//   * `add reg-reg` (REX.W 0x01 /r) in 2-address form.
//   * `sub reg-reg` (REX.W 0x29 /r).
//   * `mul reg-reg` (REX.W 0x0F 0xAF /r) — multi-byte opcode with
//     INVERTED operand mapping (dest goes to ModR/M.reg, NOT rm).
//   * Each is verified end-to-end through `legalizeTwoAddress` +
//     `assemble` so the pipeline is exercised on real LIR.

#include "asm/asm.hpp"
#include "asm_test_support.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"

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

struct X86Fixture {
    std::shared_ptr<TargetSchema> schema;
    std::uint16_t addOp, subOp, mulOp, retOp, xorOp, notOp;
    LirReg rax, rcx, r8;
};

[[nodiscard]] X86Fixture loadX86() {
    X86Fixture f{};
    auto s = TargetSchema::loadShipped("x86_64");
    EXPECT_TRUE(s.has_value());
    if (!s) return f;
    f.schema = *s;
    f.addOp = *f.schema->opcodeByMnemonic("add");
    f.subOp = *f.schema->opcodeByMnemonic("sub");
    f.mulOp = *f.schema->opcodeByMnemonic("mul");
    f.xorOp = *f.schema->opcodeByMnemonic("xor");
    f.notOp = *f.schema->opcodeByMnemonic("not");
    f.retOp = *f.schema->opcodeByMnemonic("ret");
    auto const cls = static_cast<std::uint8_t>(LirRegClass::GPR);
    f.rax = LirReg{
        static_cast<std::uint32_t>(*f.schema->registerByName("rax")), 1, cls};
    f.rcx = LirReg{
        static_cast<std::uint32_t>(*f.schema->registerByName("rcx")), 1, cls};
    f.r8 = LirReg{
        static_cast<std::uint32_t>(*f.schema->registerByName("r8")), 1, cls};
    return f;
}

// Build a single-function LIR: one block with the supplied
// emit-body + a `ret` terminator. After build, run legalize and
// assemble.
template <typename Emit>
[[nodiscard]] std::vector<std::uint8_t>
buildLegalizeAssemble(X86Fixture const& f, DiagnosticReporter& rep,
                      Emit emit) {
    LirBuilder b{*f.schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    emit(b);
    (void)b.addReturn(f.retOp, {});
    Lir src = std::move(b).finish();

    auto legal = legalizeTwoAddress(src, *f.schema, rep);
    if (!legal.ok()) {
        ADD_FAILURE() << "legalize pass failed";
        return {};
    }
    return assembleFirstFn(legal.lir, *f.schema, rep);
}

} // namespace

// ── `add rax, rcx` (in-place: result==op[0]==rax) ─────────────────

TEST(X86BinaryOps, AddRaxRcxInPlaceEmits_48_01_C8) {
    // LIR: `add rax, rax, rcx` (result=rax, op0=rax, op1=rcx).
    // Post-legalize: unchanged (op0 == result). Emits:
    //   REX.W 0x48 + opcode 0x01 + ModR/M (mod=3 reg=rcx(1) rm=rax(0))
    //   = 0x48, 0x01, 0xC8.
    auto f = loadX86();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(f.rax),
            LirOperand::makeReg(f.rcx)
        };
        (void)b.addInst(f.addOp, f.rax, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 3u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x01);
    EXPECT_EQ(bytes[2], 0xC8);
}

TEST(X86BinaryOps, AddMismatchedGainsImplicitMov) {
    // LIR: `add rax, rcx, r8` (result=rax, op0=rcx, op1=r8).
    // Post-legalize: insert `mov rax, rcx` (REX.W 0x8B /r) THEN
    // `add rax, r8` (REX.W 0x01 /r).
    // Bytes (mov):  0x48, 0x8B, 0xC1  (ModR/M mod=3 reg=rax(0) rm=rcx(1))
    // Bytes (add):  0x4C, 0x01, 0xC0  (REX.W+B=0x4C, ModR/M mod=3 reg=r8(0) rm=rax(0))
    // Wait — op1=r8 has hwEncoding=8 → REX.R = 1, ModR/M.reg=0.
    //                        REX.W=1 + REX.R=1 → 0x4C. ModR/M = 0xC0.
    auto f = loadX86();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(f.rcx),
            LirOperand::makeReg(f.r8)
        };
        (void)b.addInst(f.addOp, f.rax, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 6u);
    // mov rax, rcx
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x8B);
    EXPECT_EQ(bytes[2], 0xC1);
    // add rax, r8 — REX.W=1, REX.R=1 (r8 high bit), REX.B=0
    EXPECT_EQ(bytes[3], 0x4C);
    EXPECT_EQ(bytes[4], 0x01);
    EXPECT_EQ(bytes[5], 0xC0);
}

TEST(X86BinaryOps, XorRaxRcxInPlaceEmits_48_31_C8) {
    // Cluster-F F2 (core_bitwise): `xor rax, rax, rcx` in-place.
    // REX.W 0x48 + opcode 0x31 (XOR r/m64, r64) + ModR/M (mod=3 reg=rcx(1)
    // rm=rax(0)) = 0xC8. Same reg-reg shape as add/and; opcode 0x31 vs AND 0x21.
    auto f = loadX86();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(f.rax),
            LirOperand::makeReg(f.rcx)
        };
        (void)b.addInst(f.xorOp, f.rax, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 3u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x31);
    EXPECT_EQ(bytes[2], 0xC8);
}

TEST(X86BinaryOps, NotRaxInPlaceEmits_48_F7_D0) {
    // Cluster-F F2 (core_bitwise): `not rax` (1-operand, in-place one's complement).
    // REX.W 0x48 + opcode 0xF7 + ModR/M (mod=3, /2 opcode-extension digit in reg,
    // rm=rax(0)) = 0xD0. NEG is the sibling /3 = 0xD8 — the modrmRegExt digit (2
    // vs 3) is the only difference, so a copied-from-neg row miswiring fails here.
    auto f = loadX86();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(f.rax) };
        (void)b.addInst(f.notOp, f.rax, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 3u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0xF7);
    EXPECT_EQ(bytes[2], 0xD0);
}

TEST(X86BinaryOps, SubRaxRcxInPlaceEmits_48_29_C8) {
    // `sub rax, rax, rcx` in-place. Opcode 0x29 (SUB r/m64, r64).
    auto f = loadX86();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(f.rax),
            LirOperand::makeReg(f.rcx)
        };
        (void)b.addInst(f.subOp, f.rax, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 3u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x29);
    EXPECT_EQ(bytes[2], 0xC8);
}

TEST(X86BinaryOps, MulRaxRcxInPlaceEmits_48_0F_AF_C1) {
    // `mul rax, rax, rcx`. IMUL r64, r/m64 is encoded REX.W 0x0F
    // 0xAF /r — destination goes to ModR/M.reg, source to .rm.
    // ModR/M: mod=3 reg=rax(0) rm=rcx(1) → 0xC1.
    auto f = loadX86();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(f.rax),
            LirOperand::makeReg(f.rcx)
        };
        (void)b.addInst(f.mulOp, f.rax, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x0F);
    EXPECT_EQ(bytes[2], 0xAF);
    EXPECT_EQ(bytes[3], 0xC1);
}
