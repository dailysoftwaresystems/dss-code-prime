// x86_64 SSE scalar-double byte-encoding tests — FC2 Part B.
//
// Pins (exact byte sequences cross-checked against the Intel SDM):
//   * `fadd` (ADDSD xmm, xmm)        — F2 0F 58 /r, 2-address form.
//   * ADDSD on xmm8/xmm9             — F2 45 0F 58 /r: the mandatory
//     prefix precedes the REX byte (load-bearing order — REX before
//     F2 would decode as a different instruction), and REX appears
//     with R+B set even though the template has rexW=false.
//   * `fp_to_si` (CVTTSD2SI r64,xmm) — F2 REX.W 0F 2C /r; ModR/M.reg
//     = GPR destination, ModR/M.rm = xmm source.
//   * `movaps` (MOVAPS xmm, xmm)     — 0F 28 /r, NO prefix.
//   * `movsd_load` mem form          — F2 0F 10 /r [base + disp32].
//   * `movsd_load` RIP-relative form — F2 0F 10 05 <rel32> + one
//     rel32 Relocation at the placeholder offset.
//   * `movsd_store` mem form         — F2 0F 11 /r [base + disp32]
//     ← xmm (the LOAD's direction mirror: 0F 10 = xmm←rm, 0F 11 =
//     rm←xmm; ModRM roles identical, reg = the xmm either way).
//     Pinned plain, with hwEncoding>7 (prefix-before-REX), and in
//     the exact [rsp + disp] SIB shape the ms_x64 prologue emits.

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
#include <span>
#include <vector>

using namespace dss;

namespace {

// On failure, surface the reporter's diagnostics in the gtest output
// so a red pin names the root cause instead of just a byte-count.
void dumpDiagnostics(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        ADD_FAILURE() << "diagnostic: " << d.actual;
    }
}

[[nodiscard]] std::vector<std::uint8_t>
assembleFirstFn(Lir const& lir, TargetSchema const& schema,
                DiagnosticReporter& rep) {
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto r = assemble(lir, schema, lirToMir, rep);
    EXPECT_EQ(r.functions.size(), 1u);
    if (r.functions.empty()) return {};
    return r.functions[0].bytes;
}

struct SseFixture {
    std::shared_ptr<TargetSchema> schema;
    std::uint16_t faddOp, fpToSiOp, movapsOp, movsdLoadOp, movsdStoreOp,
                  retOp;
    LirReg rax, xmm0, xmm1, xmm8, xmm9;
};

[[nodiscard]] LirReg physReg(TargetSchema const& s, char const* name,
                             LirRegClass cls) {
    auto const ord = s.registerByName(name);
    EXPECT_TRUE(ord.has_value()) << name;
    return makePhysicalReg(static_cast<std::uint32_t>(ord.value_or(0)), cls);
}

[[nodiscard]] SseFixture loadSse() {
    SseFixture f{};
    auto s = TargetSchema::loadShipped("x86_64");
    EXPECT_TRUE(s.has_value());
    if (!s) return f;
    f.schema      = *s;
    f.faddOp       = *f.schema->opcodeByMnemonic("fadd");
    f.fpToSiOp     = *f.schema->opcodeByMnemonic("fp_to_si");
    f.movapsOp     = *f.schema->opcodeByMnemonic("movaps");
    f.movsdLoadOp  = *f.schema->opcodeByMnemonic("movsd_load");
    f.movsdStoreOp = *f.schema->opcodeByMnemonic("movsd_store");
    f.retOp        = *f.schema->opcodeByMnemonic("ret");
    f.rax  = physReg(*f.schema, "rax",  LirRegClass::GPR);
    f.xmm0 = physReg(*f.schema, "xmm0", LirRegClass::FPR);
    f.xmm1 = physReg(*f.schema, "xmm1", LirRegClass::FPR);
    f.xmm8 = physReg(*f.schema, "xmm8", LirRegClass::FPR);
    f.xmm9 = physReg(*f.schema, "xmm9", LirRegClass::FPR);
    return f;
}

// Single-function LIR: one block with the supplied body + `ret`,
// then 2-addr legalize + assemble (the binaryops-file pattern).
template <typename Emit>
[[nodiscard]] std::vector<std::uint8_t>
buildLegalizeAssemble(SseFixture const& f, DiagnosticReporter& rep,
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

// ── ADDSD xmm0, xmm1 (in-place: result==op[0]==xmm0) ──────────────

TEST(X86Sse, AddsdXmm0Xmm1Emits_F2_0F_58_C1) {
    // fadd xmm0, xmm0, xmm1 → ADDSD xmm0, xmm1:
    //   F2 (mandatory prefix) 0F 58 (opcode)
    //   ModR/M mod=3 reg=xmm0(0) rm=xmm1(1) → 0xC1. No REX (low regs,
    //   rexW=false).
    auto f = loadSse();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(f.xmm0),
            LirOperand::makeReg(f.xmm1)
        };
        (void)b.addInst(f.faddOp, f.xmm0, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    if (rep.errorCount() != 0u) dumpDiagnostics(rep);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0xF2);
    EXPECT_EQ(bytes[1], 0x0F);
    EXPECT_EQ(bytes[2], 0x58);
    EXPECT_EQ(bytes[3], 0xC1);
}

// ── ADDSD xmm8, xmm9 — prefix BEFORE REX, REX from R+B alone ──────

TEST(X86Sse, AddsdXmm8Xmm9EmitsPrefixBeforeRex_F2_45_0F_58_C1) {
    // reg=xmm8 (hwEncoding 8 → lo3=0, REX.R=1), rm=xmm9 (hwEncoding 9
    // → lo3=1, REX.B=1). Template rexW=false, so REX = 0x40|R|B =
    // 0x45 — proving the walker emits REX when ONLY R/B are set.
    // Byte order F2 THEN 45: a REX placed before the mandatory prefix
    // would not survive decode as part of the SSE opcode selection.
    auto f = loadSse();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(f.xmm8),
            LirOperand::makeReg(f.xmm9)
        };
        (void)b.addInst(f.faddOp, f.xmm8, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    if (rep.errorCount() != 0u) dumpDiagnostics(rep);
    ASSERT_GE(bytes.size(), 5u);
    EXPECT_EQ(bytes[0], 0xF2);
    EXPECT_EQ(bytes[1], 0x45);
    EXPECT_EQ(bytes[2], 0x0F);
    EXPECT_EQ(bytes[3], 0x58);
    EXPECT_EQ(bytes[4], 0xC1);
}

// ── CVTTSD2SI rax, xmm0 ───────────────────────────────────────────

TEST(X86Sse, CvttsdsiRaxXmm0Emits_F2_48_0F_2C_C0) {
    // fp_to_si rax ← xmm0: F2 (prefix) 48 (REX.W) 0F 2C (opcode)
    // ModR/M mod=3 reg=rax(0, the GPR DEST) rm=xmm0(0, the xmm SRC)
    // → 0xC0. Pins the reg/rm role direction (reversed roles would
    // still encode 0xC0 here, so the cross-register test below uses
    // distinct ordinals).
    auto f = loadSse();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(f.xmm0) };
        (void)b.addInst(f.fpToSiOp, f.rax, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    if (rep.errorCount() != 0u) dumpDiagnostics(rep);
    ASSERT_GE(bytes.size(), 5u);
    EXPECT_EQ(bytes[0], 0xF2);
    EXPECT_EQ(bytes[1], 0x48);
    EXPECT_EQ(bytes[2], 0x0F);
    EXPECT_EQ(bytes[3], 0x2C);
    EXPECT_EQ(bytes[4], 0xC0);
}

TEST(X86Sse, CvttsdsiRaxXmm1PinsRegRmRoleDirection) {
    // rax (reg field = 0) ← xmm1 (rm field = 1) → ModR/M 0xC1.
    // A role flip (xmm in reg, GPR in rm) would emit 0xC8 — the
    // silent-miscompile this pin guards.
    auto f = loadSse();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(f.xmm1) };
        (void)b.addInst(f.fpToSiOp, f.rax, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    if (rep.errorCount() != 0u) dumpDiagnostics(rep);
    ASSERT_GE(bytes.size(), 5u);
    EXPECT_EQ(bytes[0], 0xF2);
    EXPECT_EQ(bytes[1], 0x48);
    EXPECT_EQ(bytes[2], 0x0F);
    EXPECT_EQ(bytes[3], 0x2C);
    EXPECT_EQ(bytes[4], 0xC1);
}

// ── MOVAPS xmm0, xmm1 ─────────────────────────────────────────────

TEST(X86Sse, MovapsXmm0Xmm1Emits_0F_28_C1) {
    // movaps xmm0 ← xmm1: 0F 28, ModR/M mod=3 reg=xmm0(0) rm=xmm1(1)
    // → 0xC1. NO prefix byte — an accidental 66 prefix would make it
    // MOVAPD; an F2/F3 would make it MOVSD/MOVSS (upper-lane merge).
    auto f = loadSse();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(f.xmm1) };
        (void)b.addInst(f.movapsOp, f.xmm0, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    if (rep.errorCount() != 0u) dumpDiagnostics(rep);
    ASSERT_GE(bytes.size(), 3u);
    EXPECT_EQ(bytes[0], 0x0F);
    EXPECT_EQ(bytes[1], 0x28);
    EXPECT_EQ(bytes[2], 0xC1);
}

// ── MOVSD xmm1, [rax + 8] (mem-base form) ─────────────────────────

TEST(X86Sse, MovsdLoadBaseDispEmits_F2_0F_10_88_Disp32) {
    // movsd_load xmm1 ← [rax + 8]: F2 0F 10, ModR/M mod=10 (mem +
    // disp32) reg=xmm1(1) rm=rax(0) → 0x88, then disp32 = 08 00 00 00.
    auto f = loadSse();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(f.rax),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(8)
        };
        (void)b.addInst(f.movsdLoadOp, f.xmm1, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    if (rep.errorCount() != 0u) dumpDiagnostics(rep);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0xF2);
    EXPECT_EQ(bytes[1], 0x0F);
    EXPECT_EQ(bytes[2], 0x10);
    EXPECT_EQ(bytes[3], 0x88);
    EXPECT_EQ(bytes[4], 0x08);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00);
}

// ── MOVSD xmm0, [rip + sym] (RIP-relative form + relocation) ──────

TEST(X86Sse, MovsdLoadRipRelEmits_F2_0F_10_05_Rel32Reloc) {
    // movsd_load xmm0 ← [rip + sym]: F2 0F 10, ModR/M mod=00 rm=101
    // reg=xmm0(0) → 0x05, then 4 zero placeholder bytes patched by
    // the linker via a rel32 Relocation at offset 4 (the byte right
    // after the ModR/M — the prefix shifts the whole instruction, so
    // the offset derives from the emit cursor, not a hardcoded 3).
    auto f = loadSse();
    DiagnosticReporter rep;

    LirBuilder b{*f.schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeSymbolRef(77) };
    (void)b.addInst(f.movsdLoadOp, f.xmm0, ops);
    (void)b.addReturn(f.retOp, {});
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    auto result = assemble(lir, *f.schema, lirToMir, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    if (rep.errorCount() != 0u) dumpDiagnostics(rep);
    ASSERT_EQ(result.functions.size(), 1u);
    auto const& bytes  = result.functions[0].bytes;
    auto const& relocs = result.functions[0].relocations;

    // F2 0F 10 05 <00 00 00 00> ; C3 → 9 bytes total.
    ASSERT_EQ(bytes.size(), 9u);
    EXPECT_EQ(bytes[0], 0xF2);
    EXPECT_EQ(bytes[1], 0x0F);
    EXPECT_EQ(bytes[2], 0x10);
    EXPECT_EQ(bytes[3], 0x05);
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00);
    EXPECT_EQ(bytes[8], 0xC3);

    ASSERT_EQ(relocs.size(), 1u);
    EXPECT_EQ(relocs[0].offset, 4u);
    EXPECT_EQ(relocs[0].target, SymbolId{77});
    auto const rel32 = f.schema->relocationByName("rel32");
    ASSERT_NE(rel32, nullptr);
    EXPECT_EQ(relocs[0].kind, rel32->kind);
    EXPECT_EQ(relocs[0].addend, 0);
}

// ── MOVSD [rax + 8], xmm1 (store mem form — the load's mirror) ────

TEST(X86Sse, MovsdStoreBaseDispEmits_F2_0F_11_88_Disp32) {
    // movsd_store [rax + 8] ← xmm1: F2 0F 11 (Intel SDM: 0F 11 is the
    // rm←xmm STORE direction; 0F 10 the xmm←rm LOAD), ModR/M mod=10
    // reg=xmm1(1) rm=rax(0) → 0x88, disp32 = 08 00 00 00. Identical
    // bytes to the MovsdLoadBaseDisp pin EXCEPT the direction byte
    // (0x11 vs 0x10) — ModRM roles are the same (reg = the xmm), the
    // opcode's direction bit alone decides who is read vs written.
    // Operand order is the universal `store` shape ([value, base,
    // membase, memoffset]) — exactly what emitFrameStore emits.
    auto f = loadSse();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(f.xmm1),
            LirOperand::makeReg(f.rax),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(8)
        };
        (void)b.addInst(f.movsdStoreOp, InvalidLirReg, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    if (rep.errorCount() != 0u) dumpDiagnostics(rep);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0xF2);
    EXPECT_EQ(bytes[1], 0x0F);
    EXPECT_EQ(bytes[2], 0x11);
    EXPECT_EQ(bytes[3], 0x88);
    EXPECT_EQ(bytes[4], 0x08);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00);
}

// ── MOVSD [rax + 0x28], xmm8 — prefix BEFORE REX (hwEncoding > 7) ─

TEST(X86Sse, MovsdStoreXmm8EmitsPrefixBeforeRex_F2_44_0F_11_80) {
    // value=xmm8 (hwEncoding 8 → lo3=0, REX.R=1), base=rax (REX.B=0).
    // Template has no rexW, so REX = 0x40|R = 0x44 — and it must sit
    // BETWEEN the mandatory F2 and the 0F 11 escape (REX before F2
    // would not survive decode as part of the SSE opcode selection).
    // ModR/M mod=10 reg=000 rm=000 → 0x80; disp32 = 28 00 00 00.
    auto f = loadSse();
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(f.xmm8),
            LirOperand::makeReg(f.rax),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0x28)
        };
        (void)b.addInst(f.movsdStoreOp, InvalidLirReg, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    if (rep.errorCount() != 0u) dumpDiagnostics(rep);
    ASSERT_GE(bytes.size(), 9u);
    EXPECT_EQ(bytes[0], 0xF2);
    EXPECT_EQ(bytes[1], 0x44);
    EXPECT_EQ(bytes[2], 0x0F);
    EXPECT_EQ(bytes[3], 0x11);
    EXPECT_EQ(bytes[4], 0x80);
    EXPECT_EQ(bytes[5], 0x28);
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00);
    EXPECT_EQ(bytes[8], 0x00);
}

// ── MOVSD [rsp + 0x28], xmm6 — the exact ms_x64 prologue shape ────

TEST(X86Sse, MovsdStoreRspBaseXmm6EmitsSib_F2_0F_11_B4_24) {
    // The byte shape the ms_x64 prologue saved-reg spill actually
    // emits: base=rsp (hwEncoding 4 → ModR/M rm=100 mandates a SIB
    // byte; SIB = 0x24: scale=00 index=100(none) base=100(rsp)),
    // value=xmm6 (the first Win64 callee-saved xmm) → ModR/M mod=10
    // reg=110 rm=100 → 0xB4. F2 0F 11 B4 24 28 00 00 00.
    auto f = loadSse();
    DiagnosticReporter rep;
    LirReg const rsp  = physReg(*f.schema, "rsp",  LirRegClass::GPR);
    LirReg const xmm6 = physReg(*f.schema, "xmm6", LirRegClass::FPR);
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(xmm6),
            LirOperand::makeReg(rsp),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0x28)
        };
        (void)b.addInst(f.movsdStoreOp, InvalidLirReg, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    if (rep.errorCount() != 0u) dumpDiagnostics(rep);
    ASSERT_GE(bytes.size(), 9u);
    EXPECT_EQ(bytes[0], 0xF2);
    EXPECT_EQ(bytes[1], 0x0F);
    EXPECT_EQ(bytes[2], 0x11);
    EXPECT_EQ(bytes[3], 0xB4);
    EXPECT_EQ(bytes[4], 0x24);
    EXPECT_EQ(bytes[5], 0x28);
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00);
    EXPECT_EQ(bytes[8], 0x00);
}

// ── Disassembler round-trip: the disasm consumes mandatoryPrefix ──

TEST(X86Sse, AddsdRoundTripsThroughDisassembler) {
    // The x86-variable DISASSEMBLER must consume the variant's
    // mandatoryPrefix bytes before the REX/opcode match (the encoder/
    // disasm multi-site contract) — without that arm, tryMatchPrefix
    // sees 0xF2 where it expects REX-or-opcode and every SSE
    // instruction reports A_RoundTripMismatch.
    auto f = loadSse();
    DiagnosticReporter rep;

    LirBuilder b{*f.schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeReg(f.xmm0),
        LirOperand::makeReg(f.xmm1)
    };
    (void)b.addInst(f.faddOp, f.xmm0, ops);
    (void)b.addReturn(f.retOp, {});
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    auto result = assemble(lir, *f.schema, lirToMir, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(result.functions.size(), 1u);
    auto const& bytes = result.functions[0].bytes;
    ASSERT_GE(bytes.size(), 5u);  // F2 0F 58 C1 + C3

    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    LirInstId const addsd = lir.blockInstAt(bb, 0);
    // The addsd instruction is the first 4 bytes of the function.
    std::span<std::uint8_t const> const window{bytes.data(), 4};
    EXPECT_TRUE(roundTripVerify(*f.schema, lir, addsd, window, rep))
        << (rep.all().empty() ? "" : rep.all().back().actual);
}

// ── FC2 Part B4: the 2-addr legalize copy is class-correct ────────

TEST(X86Sse, FaddMismatchedDestGainsMovapsThenAddsd) {
    // LIR: `fadd xmm0, xmm1, xmm2` (result != op0). The 2-addr
    // legalize must synthesize the implicit copy with the FPR class's
    // declared move (registerClassOps → movaps), NEVER the GPR mov
    // (REX.W 8B — valid-looking bytes that move the WRONG register
    // file). Pin the full sequence:
    //   movaps xmm0, xmm1 → 0F 28 C1
    //   addsd  xmm0, xmm2 → F2 0F 58 C2
    //   ret               → C3
    auto f = loadSse();
    DiagnosticReporter rep;
    LirReg const xmm2 = physReg(*f.schema, "xmm2", LirRegClass::FPR);
    auto bytes = buildLegalizeAssemble(f, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(f.xmm1),
            LirOperand::makeReg(xmm2)
        };
        (void)b.addInst(f.faddOp, f.xmm0, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    if (rep.errorCount() != 0u) dumpDiagnostics(rep);
    ASSERT_EQ(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0x0F);   // movaps
    EXPECT_EQ(bytes[1], 0x28);
    EXPECT_EQ(bytes[2], 0xC1);   // xmm0 <- xmm1
    EXPECT_EQ(bytes[3], 0xF2);   // addsd
    EXPECT_EQ(bytes[4], 0x0F);
    EXPECT_EQ(bytes[5], 0x58);
    EXPECT_EQ(bytes[6], 0xC2);   // xmm0 += xmm2
    EXPECT_EQ(bytes[7], 0xC3);   // ret
}

// ── FC2 Part B integration smoke: the FULL pipeline ──────────────

namespace {

// True iff `bytes` contains the SSE sequence `prefix [REX] 0F op2`
// at any offset — REX (0x40..0x4F) may legally sit between the
// mandatory prefix and the opcode escape when high registers (or
// REX.W) are in play, so the matcher accepts both shapes.
[[nodiscard]] bool containsSseOp(std::vector<std::uint8_t> const& bytes,
                                 std::uint8_t prefix, std::uint8_t op2) {
    for (std::size_t i = 0; i + 2 < bytes.size(); ++i) {
        if (bytes[i] != prefix) continue;
        std::size_t j = i + 1;
        if ((bytes[j] & 0xF0u) == 0x40u) ++j;  // optional REX
        if (j + 1 < bytes.size() && bytes[j] == 0x0F && bytes[j + 1] == op2) {
            return true;
        }
    }
    return false;
}

// D-LIR-GLOBALADDR-LOAD-RIPREL-FOLD: like containsSseOp but ALSO pins
// the ModR/M addressing MODE — `modrmMask`-selected bits of the byte
// after the opcode must equal `modrmBits`. The riprel form is mod=00
// rm=101 with any xmm reg → (modrm & 0xC7) == 0x05; the [base+disp32]
// form is mod=10 → (modrm & 0xC0) == 0x80.
[[nodiscard]] bool containsSseOpModRm(std::vector<std::uint8_t> const& bytes,
                                      std::uint8_t prefix, std::uint8_t op2,
                                      std::uint8_t modrmMask,
                                      std::uint8_t modrmBits) {
    for (std::size_t i = 0; i + 3 < bytes.size(); ++i) {
        if (bytes[i] != prefix) continue;
        std::size_t j = i + 1;
        if ((bytes[j] & 0xF0u) == 0x40u) ++j;  // optional REX
        if (j + 2 < bytes.size() && bytes[j] == 0x0F && bytes[j + 1] == op2
            && (bytes[j + 2] & modrmMask) == modrmBits) {
            return true;
        }
    }
    return false;
}

// True iff the byte stream contains an x86 LEA (REX.W 0x8D — the only
// LEA shape DSS emits today: `lea r64, [rip+sym]` / `[base+disp]`).
[[nodiscard]] bool containsLea(std::vector<std::uint8_t> const& bytes) {
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        if ((bytes[i] & 0xF8u) == 0x48u && bytes[i + 1] == 0x8D) return true;
    }
    return false;
}

struct PipelineOut {
    DiagnosticReporter        rep;
    std::vector<std::uint8_t> bytes;
    std::vector<Relocation>   relocs;
    bool                      ok = false;
};

// Drive hand-built MIR through the REAL pipeline: MIR→LIR → liveness
// → regalloc → rewrite → 2-addr legalize → callconv → assemble (the
// exact stage order of compile_pipeline.cpp's lowerMirModuleToAssembly).
// `ccIndex` selects the calling convention by the schema's declared
// order (x86_64.target.json: 0 = sysv_amd64, 1 = ms_x64).
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

TEST(X86Sse, FullPipelineDoubleAddToIntEncodesAddsdAndCvttsdsi) {
    // MIR: i32 f(f64 a, f64 b) { return FPToSI(FAdd(a, b)); } —
    // through the FULL real pipeline (regalloc picks the registers,
    // callconv materializes the xmm0/xmm1 arg moves via movaps, the
    // 2-addr legalize FPR copy fires) down to machine code. Proves
    // the whole Part-B chain composes BEFORE the source-level cast
    // syntax (Part A) exists.
    TypeInterner interner{CompilationUnitId{1}};
    auto const f64 = interner.primitive(TypeKind::F64);
    auto const i32 = interner.primitive(TypeKind::I32);
    TypeId const params[] = {f64, f64};
    auto const sig = interner.fnSig(params, i32, CallConv::CcSysV);
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

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    PipelineOut out;
    runFullPipeline(mir, interner, **target, out);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.rep.errorCount(), 0u);
    if (out.rep.errorCount() != 0u) dumpDiagnostics(out.rep);
    EXPECT_TRUE(containsSseOp(out.bytes, 0xF2, 0x58))
        << "encoded function must contain ADDSD (F2 [REX] 0F 58)";
    EXPECT_TRUE(containsSseOp(out.bytes, 0xF2, 0x2C))
        << "encoded function must contain CVTTSD2SI (F2 REX.W 0F 2C)";
}

TEST(X86Sse, FullPipelineDoublePlusRodataConstFoldsToRipRelMovsd) {
    // The FC2 Part-B3 shape — i32 f(f64 a) { return FPToSI(a + 0.25); }
    // with 0.25 in the HIR->MIR PROMOTED form (anonymous F64 rodata
    // global + GlobalAddr + Load) — now lands the
    // D-LIR-GLOBALADDR-LOAD-RIPREL-FOLD (FC3.5 sweep-c3): the
    // GlobalAddr is SINGLE-USE (this load), movsd_load declares the
    // [symbol] riprel variant, so the lea+load pair folds to ONE
    // `movsd xmm, [rip+sym]` (F2 [REX] 0F 10 05 rel32 — the FC2
    // riprel byte-pin's variant finally consumed by the pipeline).
    // RED-on-disable lever: disable the fold → the lea returns and
    // the riprel ModR/M (mod=00 rm=101) vanishes → both pins RED.
    TypeInterner interner{CompilationUnitId{1}};
    auto const f64    = interner.primitive(TypeKind::F64);
    auto const ptrF64 = interner.pointer(f64);
    auto const i32    = interner.primitive(TypeKind::I32);
    TypeId const params[] = {f64};
    auto const sig = interner.fnSig(params, i32, CallConv::CcSysV);
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

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    PipelineOut out;
    runFullPipeline(mir, interner, **target, out);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.rep.errorCount(), 0u)
        << "the global+load shape must encode end-to-end with NO "
           "A_NoMatchingEncodingVariant (the retired pool route's "
           "failure mode)";
    if (out.rep.errorCount() != 0u) dumpDiagnostics(out.rep);

    EXPECT_TRUE(containsSseOpModRm(out.bytes, 0xF2, 0x10, 0xC7, 0x05))
        << "the fold must emit the RIP-relative MOVSD (F2 [REX] 0F 10 "
           "<modrm mod=00 rm=101>) — the lea+load pair folded to ONE "
           "riprel load";
    EXPECT_FALSE(containsLea(out.bytes))
        << "the single-use GlobalAddr's lea must be ELIDED by the fold "
           "(no REX.W 8D anywhere in the function)";
    EXPECT_TRUE(containsSseOp(out.bytes, 0xF2, 0x58))
        << "must contain ADDSD";
    EXPECT_TRUE(containsSseOp(out.bytes, 0xF2, 0x2C))
        << "must contain CVTTSD2SI";
    // Exactly ONE rel32 relocation to the promoted global — now
    // emitted by the movsd_load riprel wire (pre-fold it came from
    // the lea; the fold preserves the reloc count and target).
    ASSERT_EQ(out.relocs.size(), 1u);
    EXPECT_EQ(out.relocs[0].target, SymbolId{500});
    auto const rel32 = (*target)->relocationByName("rel32");
    ASSERT_NE(rel32, nullptr);
    EXPECT_EQ(out.relocs[0].kind, rel32->kind);
}

TEST(X86Sse, FullPipelineRodataConstTwoLoadsKeepsLeaPlusBaseForm) {
    // NOT-single-use guard (D-LIR-GLOBALADDR-LOAD-RIPREL-FOLD's
    // conservative arm): the SAME promoted-global shape but the
    // GlobalAddr address feeds TWO loads — the fold must NOT fire
    // (folding would orphan the second consumer's address). The lea
    // stays, both loads keep the [base+disp32] form (mod=10), and the
    // riprel ModR/M never appears.
    TypeInterner interner{CompilationUnitId{1}};
    auto const f64    = interner.primitive(TypeKind::F64);
    auto const ptrF64 = interner.pointer(f64);
    auto const i32    = interner.primitive(TypeKind::I32);
    TypeId const params[] = {f64};
    auto const sig = interner.fnSig(params, i32, CallConv::CcSysV);
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
    MirInstId const c1 = mb.addInst(MirOpcode::Load, loadOps, f64);
    MirInstId const c2 = mb.addInst(MirOpcode::Load, loadOps, f64);
    MirInstId const add1Ops[] = {a, c1};
    MirInstId const s1 = mb.addInst(MirOpcode::FAdd, add1Ops, f64);
    MirInstId const add2Ops[] = {s1, c2};
    MirInstId const s2 = mb.addInst(MirOpcode::FAdd, add2Ops, f64);
    MirInstId const cvtOps[] = {s2};
    MirInstId const r = mb.addInst(MirOpcode::FPToSI, cvtOps, i32);
    mb.addReturn(r);
    Mir mir = std::move(mb).finish();

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    PipelineOut out;
    runFullPipeline(mir, interner, **target, out);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.rep.errorCount(), 0u);
    if (out.rep.errorCount() != 0u) dumpDiagnostics(out.rep);

    EXPECT_TRUE(containsLea(out.bytes))
        << "a TWO-consumer GlobalAddr must keep its lea (the fold is "
           "single-use only)";
    EXPECT_TRUE(containsSseOpModRm(out.bytes, 0xF2, 0x10, 0xC0, 0x80))
        << "the loads must keep the [base+disp32] form (ModR/M mod=10)";
    EXPECT_FALSE(containsSseOpModRm(out.bytes, 0xF2, 0x10, 0xC7, 0x05))
        << "no riprel MOVSD may appear when the address has two "
           "consumers";
    // Exactly ONE rel32 relocation — the lea's; the base-form loads
    // emit none.
    ASSERT_EQ(out.relocs.size(), 1u);
    EXPECT_EQ(out.relocs[0].target, SymbolId{500});
}

TEST(X86Sse, FullPipelineMsX64FprPrologueSpillsViaMovsdStore) {
    // The PE-float gap, end-to-end (FC2 runtime-corpus unblock,
    // 2026-06-10): the SAME MIR as the SysV pipeline test above —
    // i32 f(f64 a, f64 b) { return FPToSI(FAdd(a, b)); } — but
    // allocated under ms_x64 (cc index 1). Win64 declares xmm6-15
    // CALLEE-SAVED and the regalloc allocates callee-saved FIRST
    // (lir_regalloc tryAllocate), so the very first FPR vreg lands
    // in a callee-saved xmm → the prologue MUST spill it with the
    // fpr class's `store` (registerClassOps → movsd_store, F2 [REX]
    // 0F 11) and the epilogue restore it with `load` (F2 [REX]
    // 0F 10). SysV has no callee-saved XMMs, which is why the gap
    // only fired on PE.
    //
    // RED-on-disable lever: remove `"store": "movsd_store"` from the
    // fpr registerClassOps row → classOpHandle reports
    // L_RequiredLirOpcodeMissing ("callconv: prologue saved-reg
    // store") and the callconv stage fails — this test cannot even
    // assemble, the exact pre-fix failure compiling ANY float-using
    // c-subset program for x86_64 Windows PE.
    TypeInterner interner{CompilationUnitId{1}};
    auto const f64 = interner.primitive(TypeKind::F64);
    auto const i32 = interner.primitive(TypeKind::I32);
    TypeId const params[] = {f64, f64};
    auto const sig = interner.fnSig(params, i32, CallConv::CcMS64);
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

    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    PipelineOut out;
    runFullPipeline(mir, interner, **target, out, /*ccIndex=*/1);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.rep.errorCount(), 0u)
        << "ms_x64 float lowering must not fail-loud on the FPR "
           "saved-reg store (L_RequiredLirOpcodeMissing = the pre-fix "
           "PE float gap)";
    if (out.rep.errorCount() != 0u) dumpDiagnostics(out.rep);
    EXPECT_TRUE(containsSseOp(out.bytes, 0xF2, 0x11))
        << "ms_x64 prologue must spill the callee-saved xmm via the "
           "MOVSD store (F2 [REX] 0F 11)";
    EXPECT_TRUE(containsSseOp(out.bytes, 0xF2, 0x10))
        << "ms_x64 epilogue must restore the callee-saved xmm via the "
           "MOVSD load (F2 [REX] 0F 10)";
    EXPECT_TRUE(containsSseOp(out.bytes, 0xF2, 0x58))
        << "the function body must still contain ADDSD";
}
