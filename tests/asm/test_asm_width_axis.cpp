// FC3 c2 width-axis byte-encoding tests (D-CSUBSET-32BIT-ALU-FORMS).
//
// The encoding-variant guards gained an optional `width` key matched
// against `lirInstWidthBits(LirInst.flags)` — this file pins, against
// the ISA manuals:
//   * x86_64: every 32-bit (no-REX.W) variant's EXACT bytes — Intel
//     SDM Vol. 2: the 32-bit forms are the same opcodes WITHOUT REX.W
//     and auto-zero-extend their destination. Including the
//     REX-WITHOUT-W case (r8d-r15d operands set REX.R/B with W=0) and
//     CDQ (0x99 — the no-REX.W sibling of CQO).
//   * arm64: every W-form word — ARM ARM: the W-form is the X-form
//     word with sf (bit 31) CLEARED — plus the two NEW conversion
//     opcodes (SXTW; the W-form ORR `trunc` mov).
//   * back-compat contrast pins: the SAME instruction at the default
//     width (flags=0) emits the pre-existing 64-bit bytes.
//   * red-on-disable: stripping a 32-bit variant from the JSON makes
//     a width-32 instruction fail loud (A_NoMatchingEncodingVariant
//     naming the width), never silently fall back to the 64-bit form.

#include "asm/asm.hpp"
#include "asm_test_support.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

constexpr std::uint8_t kW32 = kLirInstFlagWidth32;
constexpr std::uint8_t kW8  = kLirInstFlagWidth8;

[[nodiscard]] std::vector<std::uint8_t>
assembleFirstFn(Lir const& lir, TargetSchema const& schema,
                DiagnosticReporter& rep) {
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto r = assemble(lir, schema, lirToMir, rep);
    EXPECT_EQ(r.functions.size(), 1u);
    if (r.functions.empty()) return {};
    return r.functions[0].bytes;
}

[[nodiscard]] LirReg gpr(TargetSchema const& s, std::string_view name) {
    auto const ord = s.registerByName(name);
    EXPECT_TRUE(ord.has_value()) << name;
    return LirReg{static_cast<std::uint32_t>(ord.value_or(0)), 1,
                  static_cast<std::uint8_t>(LirRegClass::GPR)};
}

// Build one function (body + ret), legalize (x86 2-address), assemble.
template <typename Emit>
[[nodiscard]] std::vector<std::uint8_t>
buildLegalizeAssemble(TargetSchema const& schema, DiagnosticReporter& rep,
                      Emit emit) {
    LirBuilder b{schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    emit(b);
    (void)b.addReturn(*schema.opcodeByMnemonic("ret"), {});
    Lir src = std::move(b).finish();
    auto legal = legalizeTwoAddress(src, schema, rep);
    if (!legal.ok()) {
        ADD_FAILURE() << "legalize pass failed";
        return {};
    }
    return assembleFirstFn(legal.lir, schema, rep);
}

// Expect `bytes` to START with `expected` (the trailing ret is not
// part of the pin).
void expectPrefix(std::vector<std::uint8_t> const& bytes,
                  std::vector<std::uint8_t> const& expected) {
    ASSERT_GE(bytes.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(bytes[i], expected[i]) << "byte " << i;
    }
}

[[nodiscard]] std::uint32_t
firstInstWord(std::vector<std::uint8_t> const& bytes) {
    if (bytes.size() < 4) return 0;
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) <<  8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

[[nodiscard]] std::shared_ptr<TargetSchema> loadX86() {
    auto s = TargetSchema::loadShipped("x86_64");
    EXPECT_TRUE(s.has_value());
    return s ? *s : nullptr;
}

[[nodiscard]] std::shared_ptr<TargetSchema> loadArm() {
    auto s = TargetSchema::loadShipped("arm64");
    EXPECT_TRUE(s.has_value());
    return s ? *s : nullptr;
}

} // namespace

// ── x86_64 32-bit ALU forms: same opcodes WITHOUT REX.W ────────────────

TEST(WidthAxisX86, Add32RaxRcxEmits_01_C8_NoRex) {
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    DiagnosticReporter rep;
    auto const rax = gpr(*schema, "rax");
    auto const rcx = gpr(*schema, "rcx");
    auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(rax),
                                  LirOperand::makeReg(rcx)};
        (void)b.addInst(*schema->opcodeByMnemonic("add"), rax, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    expectPrefix(bytes, {0x01, 0xC8});
    // Contrast: the default width still emits the REX.W form.
    DiagnosticReporter rep64;
    auto bytes64 = buildLegalizeAssemble(*schema, rep64, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(rax),
                                  LirOperand::makeReg(rcx)};
        (void)b.addInst(*schema->opcodeByMnemonic("add"), rax, ops);
    });
    EXPECT_EQ(rep64.errorCount(), 0u);
    expectPrefix(bytes64, {0x48, 0x01, 0xC8});
}

TEST(WidthAxisX86, Add32HighRegsEmitRexWithoutW_45_01_C8) {
    // add r8d, r9d — REX is still required for the high-register R/B
    // extension bits, but W stays 0: 0x40|R|B = 0x45, then 01 /r with
    // ModRM mod=3 reg=r9.lo3(1) rm=r8.lo3(0) = 0xC8.
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    DiagnosticReporter rep;
    auto const r8 = gpr(*schema, "r8");
    auto const r9 = gpr(*schema, "r9");
    auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(r8),
                                  LirOperand::makeReg(r9)};
        (void)b.addInst(*schema->opcodeByMnemonic("add"), r8, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    expectPrefix(bytes, {0x45, 0x01, 0xC8});
}

TEST(WidthAxisX86, Sub32AndNeg32EmitNoRexForms) {
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    auto const rcx = gpr(*schema, "rcx");
    {
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rax),
                                      LirOperand::makeReg(rcx)};
            (void)b.addInst(*schema->opcodeByMnemonic("sub"), rax, ops,
                            /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0x29, 0xC8});  // sub r/m32, r32
    }
    {
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rax)};
            (void)b.addInst(*schema->opcodeByMnemonic("neg"), rax, ops,
                            /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0xF7, 0xD8});  // neg r/m32 = F7 /3
    }
}

TEST(WidthAxisX86, Imul32Emits_0F_AF_C1_NoRex) {
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    DiagnosticReporter rep;
    auto const rax = gpr(*schema, "rax");
    auto const rcx = gpr(*schema, "rcx");
    auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(rax),
                                  LirOperand::makeReg(rcx)};
        (void)b.addInst(*schema->opcodeByMnemonic("mul"), rax, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    // imul r32, r/m32 — dest in ModRM.reg (the inverted mapping).
    expectPrefix(bytes, {0x0F, 0xAF, 0xC1});
}

TEST(WidthAxisX86, Cmp32RegRegAndRegImmEmitNoRexForms) {
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    auto const rcx = gpr(*schema, "rcx");
    {
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rax),
                                      LirOperand::makeReg(rcx)};
            (void)b.addInst(*schema->opcodeByMnemonic("cmp"),
                            InvalidLirReg, ops, /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0x39, 0xC8});  // cmp r/m32, r32
    }
    {
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rax),
                                      LirOperand::makeImmInt32(7)};
            (void)b.addInst(*schema->opcodeByMnemonic("cmp"),
                            InvalidLirReg, ops, /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        // cmp r/m32, imm32 = 81 /7; ModRM = 11_111_000 = 0xF8.
        expectPrefix(bytes, {0x81, 0xF8, 0x07, 0x00, 0x00, 0x00});
    }
}

TEST(WidthAxisX86, Mov32ImmWritesExactFourBytesNoSignExtension) {
    // mov r/m32, imm32 (C7 /0 WITHOUT REX.W) writes the EXACT value
    // 0xFFFFFFFF and zero-extends — the 64-bit C7 form would
    // sign-extend to 0xFFFFFFFFFFFFFFFF. This byte-level difference
    // IS the `unsigned int` constant-correctness contract.
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    {
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeImmInt32(-1)};
            (void)b.addInst(*schema->opcodeByMnemonic("mov"), rax, ops,
                            /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF});
    }
    {
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeImmInt32(-1)};
            (void)b.addInst(*schema->opcodeByMnemonic("mov"), rax, ops);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF});
    }
}

TEST(WidthAxisX86, Mov32RegRegAndTrunc32EmitMovR32R32) {
    // Both realize as 8B /r without REX.W — `mov r32, r32`
    // zero-extends, which IS C's mod-2^32 conversion for trunc.
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    auto const rcx = gpr(*schema, "rcx");
    for (char const* mnemonic : {"mov", "trunc"}) {
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rcx)};
            (void)b.addInst(*schema->opcodeByMnemonic(mnemonic), rax, ops,
                            /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u) << mnemonic;
        expectPrefix(bytes, {0x8B, 0xC1});
    }
}

TEST(WidthAxisX86, TruncAtDefaultWidthFailsLoudNoVariant) {
    // The trunc encoding is deliberately width-keyed 32-only: a trunc
    // instruction WITHOUT the width-32 flag (e.g. a 16/8-bit Trunc
    // that slipped past the MIR gate) must match NOTHING — never the
    // 32-bit form.
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    auto const rcx = gpr(*schema, "rcx");
    DiagnosticReporter rep;
    (void)buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(rcx)};
        (void)b.addInst(*schema->opcodeByMnemonic("trunc"), rax, ops);
    });
    EXPECT_GT(rep.errorCount(), 0u);
    bool sawWidth = false;
    for (auto const& d : rep.all()) {
        if (d.actual.find("width 64") != std::string::npos) sawWidth = true;
    }
    EXPECT_TRUE(sawWidth)
        << "the no-variant diagnostic must name the instruction width";
}

TEST(WidthAxisX86, Cdq32AndDivFamily32EmitNoRexWForms) {
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rdi = gpr(*schema, "rdi");
    auto const rcx = gpr(*schema, "rcx");
    auto const r14 = gpr(*schema, "r14");
    {
        // CDQ — 0x99 without REX.W (the CQO sibling; Intel SDM
        // CWD/CDQ/CQO: operand size selects the mnemonic).
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            (void)b.addInst(*schema->opcodeByMnemonic("cqo"),
                            InvalidLirReg, {}, /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0x99});
        // 64-bit contrast: CQO = REX.W 99.
        DiagnosticReporter rep64;
        auto bytes64 = buildLegalizeAssemble(*schema, rep64,
                                             [&](LirBuilder& b) {
            (void)b.addInst(*schema->opcodeByMnemonic("cqo"),
                            InvalidLirReg, {});
        });
        EXPECT_EQ(rep64.errorCount(), 0u);
        expectPrefix(bytes64, {0x48, 0x99});
    }
    {
        // idiv r/m32 — F7 /7, rm=rdi(7) → ModRM 0xFF.
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rdi)};
            (void)b.addInst(*schema->opcodeByMnemonic("idiv_op"),
                            InvalidLirReg, ops, /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0xF7, 0xFF});
    }
    {
        // idiv r14d — REX.B WITHOUT W (0x41) proves the high-register
        // extension is independent of the W bit: 41 F7 FE (the 64-bit
        // sibling pin is 49 F7 FE).
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(r14)};
            (void)b.addInst(*schema->opcodeByMnemonic("idiv_op"),
                            InvalidLirReg, ops, /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0x41, 0xF7, 0xFE});
    }
    {
        // div r/m32 — F7 /6, rm=rcx(1) → ModRM 0xF1.
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rcx)};
            (void)b.addInst(*schema->opcodeByMnemonic("div_op"),
                            InvalidLirReg, ops, /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0xF7, 0xF1});
    }
}

// ── arm64 W-forms: the X-form word with sf (bit 31) cleared ───────────

namespace {

// Single-instruction arm64 assemble (3-address, no legalize needed).
template <typename Emit>
[[nodiscard]] std::vector<std::uint8_t>
buildAndAssembleArm(TargetSchema const& schema, DiagnosticReporter& rep,
                    Emit emit) {
    LirBuilder b{schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    emit(b);
    (void)b.addReturn(*schema.opcodeByMnemonic("ret"), {});
    Lir lir = std::move(b).finish();
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto r = assemble(lir, schema, lirToMir, rep);
    EXPECT_EQ(r.functions.size(), 1u);
    if (r.functions.empty()) return {};
    return r.functions[0].bytes;
}

} // namespace

TEST(WidthAxisArm64, WFormBinaryOpsClearBit31) {
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const x0 = gpr(*schema, "x0");
    auto const x1 = gpr(*schema, "x1");
    auto const x2 = gpr(*schema, "x2");
    struct Case {
        char const*   mnemonic;
        std::uint32_t expectedWord;  // Wd=0, Wn=1, Wm=2 forms
    };
    // Cross-derived from the ARM ARM (and cross-checked against GNU
    // as): add w0,w1,w2 / sub / mul (MADD wzr) / sdiv / udiv.
    for (Case const c : {Case{"add",  0x0B020020u},
                         Case{"sub",  0x4B020020u},
                         Case{"mul",  0x1B027C20u},
                         Case{"sdiv", 0x1AC20C20u},
                         Case{"udiv", 0x1AC20820u}}) {
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1),
                                      LirOperand::makeReg(x2)};
            (void)b.addInst(*schema->opcodeByMnemonic(c.mnemonic), x0, ops,
                            /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u) << c.mnemonic;
        EXPECT_EQ(firstInstWord(bytes), c.expectedWord) << c.mnemonic;
        // Width axis sanity: the X-form sibling is the SAME word with
        // bit 31 set.
        DiagnosticReporter rep64;
        auto bytes64 = buildAndAssembleArm(*schema, rep64,
                                           [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1),
                                      LirOperand::makeReg(x2)};
            (void)b.addInst(*schema->opcodeByMnemonic(c.mnemonic), x0, ops);
        });
        EXPECT_EQ(rep64.errorCount(), 0u) << c.mnemonic;
        EXPECT_EQ(firstInstWord(bytes64), c.expectedWord | 0x80000000u)
            << c.mnemonic;
    }
}

TEST(WidthAxisArm64, CmpWFormAndNegWForm) {
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const x0 = gpr(*schema, "x0");
    auto const x1 = gpr(*schema, "x1");
    auto const x2 = gpr(*schema, "x2");
    {
        // CMP W1, W2 = SUBS WZR, W1, W2 = 0x6B02003F.
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1),
                                      LirOperand::makeReg(x2)};
            (void)b.addInst(*schema->opcodeByMnemonic("cmp"),
                            InvalidLirReg, ops, /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(firstInstWord(bytes), 0x6B02003Fu);
    }
    {
        // NEG W0, W1 = SUB W0, WZR, W1 = 0x4B0103E0.
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1)};
            (void)b.addInst(*schema->opcodeByMnemonic("neg"), x0, ops,
                            /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(firstInstWord(bytes), 0x4B0103E0u);
    }
}

// FC3.5 sweep-c1: the arm64 CMP-immediate alias — SUBS XZR, Xn, #imm12
// (ARM ARM C6.2.395 add/subtract-immediate; sf|1|1|100010|sh=0|imm12|
// Rn|11111). FIRST consumer is the MIR→LIR non-fused CondBr arm's
// `cmp cond, #0` over a PHI-produced Bool (`&&`/`||` results used as
// conditions can't fuse) — the gap was masked until the short-circuit
// marker fix let logical-op conditions reach arm64 codegen.
TEST(WidthAxisArm64, CmpImmediateXAndWForms) {
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const x1 = gpr(*schema, "x1");
    {
        // CMP X1, #0 = 0xF100003F (Rn=1 → bits 5..9; imm12=0).
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1),
                                      LirOperand::makeImmInt32(0)};
            (void)b.addInst(*schema->opcodeByMnemonic("cmp"),
                            InvalidLirReg, ops);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(firstInstWord(bytes), 0xF100003Fu);
    }
    {
        // CMP X1, #7 — imm12 lands at bits 10..21: 7<<10 = 0x1C00.
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1),
                                      LirOperand::makeImmInt32(7)};
            (void)b.addInst(*schema->opcodeByMnemonic("cmp"),
                            InvalidLirReg, ops);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(firstInstWord(bytes), 0xF1001C3Fu);
    }
    {
        // CMP W1, #0 = 0x7100003F (the X-form word with sf cleared).
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1),
                                      LirOperand::makeImmInt32(0)};
            (void)b.addInst(*schema->opcodeByMnemonic("cmp"),
                            InvalidLirReg, ops, /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(firstInstWord(bytes), 0x7100003Fu);
    }
}

TEST(WidthAxisArm64, SxtwAndTruncWords) {
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const x0 = gpr(*schema, "x0");
    auto const x1 = gpr(*schema, "x1");
    {
        // SXTW X0, W1 = SBFM X0, X1, #0, #31 = 0x93407C20 (the arm64
        // `sext` opcode; source on Rn). Width-keyed 32 since the byte
        // form (SXTB) was added (D-CSUBSET-CHAR-STRING-VALUE-CODEGEN) —
        // the I32-source SExt threads width-32 exactly as MIR→LIR does
        // (widthFlagsForType(I32)); the byte-source form is width-8.
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1)};
            (void)b.addInst(*schema->opcodeByMnemonic("sext"), x0, ops,
                            /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(firstInstWord(bytes), 0x93407C20u);
    }
    {
        // trunc = MOV W0, W1 = ORR W0, WZR, W1 = 0x2A0103E0 (the NEW
        // arm64 `trunc` opcode; width-keyed 32, source on Rm).
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1)};
            (void)b.addInst(*schema->opcodeByMnemonic("trunc"), x0, ops,
                            /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(firstInstWord(bytes), 0x2A0103E0u);
    }
}

// ── D-CSUBSET-CHAR-STRING-VALUE-CODEGEN: the `char` BYTE memory/widen
//    forms, byte-exact. These pin the EXACT encoding bytes (the flag →
//    bytes tier) for the width-8 load/store/sext variants — a regression
//    in a `.target.json` fixedWord/opcode surfaces here as a precise byte
//    mismatch, not only as a runtime exit-code drift in the corpus. ─────

TEST(WidthAxisX86, CharByteFormsEmitMovzxByteStoreAndMovsx) {
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    auto const rcx = gpr(*schema, "rcx");
    {
        // BYTE LOAD: movzx rcx, byte [rax+8] = REX.W 0F B6 /r; ModRM
        // mod=10 reg=rcx(1) rm=rax(0) = 0x88; disp32 = 08 00 00 00.
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rax),
                                      LirOperand::makeMemBase(1),
                                      LirOperand::makeMemOffset(8)};
            (void)b.addInst(*schema->opcodeByMnemonic("load"), rcx, ops,
                            /*payload=*/0, kW8);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0x48, 0x0F, 0xB6, 0x88, 0x08, 0x00, 0x00, 0x00});
    }
    {
        // BYTE STORE: mov byte [rax+8], cl = 0x88 /r with a FORCED REX
        // (0x40 — no W/R/B for rcx/rax) so the value reg is the proper
        // low byte (cl), never the ah/ch/dh/bh alias; ModRM mod=10
        // reg=rcx(1) rm=rax(0) = 0x88; disp32.
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rcx),   // value
                                      LirOperand::makeReg(rax),   // base
                                      LirOperand::makeMemBase(1),
                                      LirOperand::makeMemOffset(8)};
            (void)b.addInst(*schema->opcodeByMnemonic("store"),
                            InvalidLirReg, ops, /*payload=*/0, kW8);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0x40, 0x88, 0x88, 0x08, 0x00, 0x00, 0x00});
    }
    {
        // BYTE SEXT: movsx rax, al = REX.W 0F BE /r; ModRM mod=11
        // reg=rax(0) rm=rax(0) = 0xC0 (sign-extend the low byte → r64).
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rax)};
            (void)b.addInst(*schema->opcodeByMnemonic("sext"), rax, ops,
                            /*payload=*/0, kW8);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0x48, 0x0F, 0xBE, 0xC0});
    }
}

TEST(WidthAxisArm64, CharByteFormsEmitSxtbLdurbSturb) {
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const x0 = gpr(*schema, "x0");
    auto const x1 = gpr(*schema, "x1");
    {
        // SXTB X0, W1 = SBFM X0, X1, #0, #7 = 0x93401C00 | Rn(x1=1)<<5
        // = 0x93401C20 (the SXTW word with imms 31→7; source on Rn).
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1)};
            (void)b.addInst(*schema->opcodeByMnemonic("sext"), x0, ops,
                            /*payload=*/0, kW8);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(firstInstWord(bytes), 0x93401C20u);
    }
    {
        // LDURB W1, [X0, #8] = 0x38400000 | imm9(8)<<12 | Rn(x0=0)<<5
        // | Rt(x1=1) = 0x38408001 (size=00 byte, opc=01 load — zero-
        // extends into W1).
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x0),
                                      LirOperand::makeMemBase(1),
                                      LirOperand::makeMemOffset(8)};
            (void)b.addInst(*schema->opcodeByMnemonic("load"), x1, ops,
                            /*payload=*/0, kW8);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(firstInstWord(bytes), 0x38408001u);
    }
    {
        // STURB W1, [X0, #8] = 0x38000000 | imm9(8)<<12 | Rn(x0=0)<<5
        // | Rt(x1=1) = 0x38008001 (size=00 byte, opc=00 store — writes
        // exactly 1 byte).
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1),   // value
                                      LirOperand::makeReg(x0),   // base
                                      LirOperand::makeMemBase(1),
                                      LirOperand::makeMemOffset(8)};
            (void)b.addInst(*schema->opcodeByMnemonic("store"),
                            InvalidLirReg, ops, /*payload=*/0, kW8);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(firstInstWord(bytes), 0x38008001u);
    }
}

// ── FC3.5 sweep-c1: SHIFT encodings (the D-CSUBSET-32BIT-ALU-FORMS
//    shifts residue) — Intel SDM SAL/SAR/SHL/SHR + ARM ARM LSLV/LSRV/
//    ASRV, byte-pinned per variant. ──────────────────────────────────

TEST(WidthAxisX86, ShiftClFormsBothWidthsAndHighReg) {
    // Variable-count forms: D3 /4 (shl), /5 (shr), /7 (sar). The
    // count is the IMPLICIT CL register — one explicit r/m operand.
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    auto const r8  = gpr(*schema, "r8");
    struct Case { char const* mnemonic; std::uint8_t modrm; };
    for (Case const c : {Case{"shl",  0xE0},     // /4 → reg=100
                         Case{"shr_l", 0xE8},    // /5 → reg=101
                         Case{"shr_a", 0xF8}}) { // /7 → reg=111
        {
            // 64-bit: REX.W D3 /digit.
            DiagnosticReporter rep;
            auto bytes = buildLegalizeAssemble(*schema, rep,
                                               [&](LirBuilder& b) {
                LirOperand const ops[] = {LirOperand::makeReg(rax)};
                (void)b.addInst(*schema->opcodeByMnemonic(c.mnemonic),
                                rax, ops);
            });
            EXPECT_EQ(rep.errorCount(), 0u) << c.mnemonic;
            expectPrefix(bytes, {0x48, 0xD3, c.modrm});
        }
        {
            // 32-bit: D3 /digit without REX.W.
            DiagnosticReporter rep;
            auto bytes = buildLegalizeAssemble(*schema, rep,
                                               [&](LirBuilder& b) {
                LirOperand const ops[] = {LirOperand::makeReg(rax)};
                (void)b.addInst(*schema->opcodeByMnemonic(c.mnemonic),
                                rax, ops, /*payload=*/0, kW32);
            });
            EXPECT_EQ(rep.errorCount(), 0u) << c.mnemonic;
            expectPrefix(bytes, {0xD3, c.modrm});
        }
    }
    {
        // REX-without-W: shl r8d, cl → 0x41 D3 E0 (B extension, W=0).
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(r8)};
            (void)b.addInst(*schema->opcodeByMnemonic("shl"), r8, ops,
                            /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        expectPrefix(bytes, {0x41, 0xD3, 0xE0});
    }
}

TEST(WidthAxisX86, ShiftImm8FormsBothWidths) {
    // Constant-count forms: C1 /digit ib — the count is ONE byte
    // after ModR/M (the NEW Imm8 slot's first consumer).
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    struct Case { char const* mnemonic; std::uint8_t modrm; };
    for (Case const c : {Case{"shl",  0xE0},
                         Case{"shr_l", 0xE8},
                         Case{"shr_a", 0xF8}}) {
        {
            // 64-bit: REX.W C1 /digit ib (count 5).
            DiagnosticReporter rep;
            auto bytes = buildLegalizeAssemble(*schema, rep,
                                               [&](LirBuilder& b) {
                LirOperand const ops[] = {LirOperand::makeReg(rax),
                                          LirOperand::makeImmInt32(5)};
                (void)b.addInst(*schema->opcodeByMnemonic(c.mnemonic),
                                rax, ops);
            });
            EXPECT_EQ(rep.errorCount(), 0u) << c.mnemonic;
            expectPrefix(bytes, {0x48, 0xC1, c.modrm, 0x05});
        }
        {
            // 32-bit: C1 /digit ib without REX.W (count 2).
            DiagnosticReporter rep;
            auto bytes = buildLegalizeAssemble(*schema, rep,
                                               [&](LirBuilder& b) {
                LirOperand const ops[] = {LirOperand::makeReg(rax),
                                          LirOperand::makeImmInt32(2)};
                (void)b.addInst(*schema->opcodeByMnemonic(c.mnemonic),
                                rax, ops, /*payload=*/0, kW32);
            });
            EXPECT_EQ(rep.errorCount(), 0u) << c.mnemonic;
            expectPrefix(bytes, {0xC1, c.modrm, 0x02});
        }
    }
}

TEST(WidthAxisX86, ShiftImm8OutOfRangeFailsLoudNeverTruncates) {
    // Defense-in-depth: a value that doesn't fit one byte must fail
    // loud at the walker (the lowering routes such counts through the
    // CL form; this pins the walker's own guard).
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    DiagnosticReporter rep;
    (void)buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(rax),
                                  LirOperand::makeImmInt32(300)};
        (void)b.addInst(*schema->opcodeByMnemonic("shl"), rax, ops);
    });
    EXPECT_GT(rep.errorCount(), 0u);
    bool sawRangeReject = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::A_NoMatchingEncodingVariant
            && d.actual.find("imm8") != std::string::npos) {
            sawRangeReject = true;
        }
    }
    EXPECT_TRUE(sawRangeReject);
}

TEST(WidthAxisArm64, ShiftVariableWordsXAndWForms) {
    // LSLV/LSRV/ASRV Xd, Xn, Xm — data-processing (2 source), opcode
    // field 001000/001001/001010. W-forms = the X word with sf (bit
    // 31) cleared. Derived from the ARM ARM (C6.2.215/217/21), with
    // d0=x0, n=x1, m=x2 → | 2<<16 | 1<<5 | 0.
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const x0 = gpr(*schema, "x0");
    auto const x1 = gpr(*schema, "x1");
    auto const x2 = gpr(*schema, "x2");
    struct Case { char const* mnemonic; std::uint32_t xWord; };
    for (Case const c : {Case{"shl",   0x9AC22020u},   // LSLV
                         Case{"shr_l", 0x9AC22420u},   // LSRV
                         Case{"shr_a", 0x9AC22820u}}) {// ASRV
        {
            DiagnosticReporter rep;
            auto bytes = buildAndAssembleArm(*schema, rep,
                                             [&](LirBuilder& b) {
                LirOperand const ops[] = {LirOperand::makeReg(x1),
                                          LirOperand::makeReg(x2)};
                (void)b.addInst(*schema->opcodeByMnemonic(c.mnemonic),
                                x0, ops);
            });
            EXPECT_EQ(rep.errorCount(), 0u) << c.mnemonic;
            EXPECT_EQ(firstInstWord(bytes), c.xWord) << c.mnemonic;
        }
        {
            DiagnosticReporter rep;
            auto bytes = buildAndAssembleArm(*schema, rep,
                                             [&](LirBuilder& b) {
                LirOperand const ops[] = {LirOperand::makeReg(x1),
                                          LirOperand::makeReg(x2)};
                (void)b.addInst(*schema->opcodeByMnemonic(c.mnemonic),
                                x0, ops, /*payload=*/0, kW32);
            });
            EXPECT_EQ(rep.errorCount(), 0u) << c.mnemonic;
            EXPECT_EQ(firstInstWord(bytes), c.xWord & ~0x80000000u)
                << c.mnemonic;
        }
    }
}

// RED-ON-DISABLE: strip the x86 shift encodings entirely → a shift
// inst must FAIL LOUD at the assembler (A_NoEncodingDeclared — the
// exact pre-FC3.5 wall this cycle removed), proving the encodings are
// the load-bearing piece, not some fallback.
TEST(WidthAxisX86, StrippedShiftEncodingFailsLoud) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto& op : doc["opcodes"]) {
                if (op.value("mnemonic", "") != "shl") continue;
                op.erase("encoding");
            }
        });
    ASSERT_TRUE(mutated.has_value());
    auto const& schema = **mutated;
    auto const rax = gpr(schema, "rax");
    DiagnosticReporter rep;
    (void)buildLegalizeAssemble(schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(rax)};
        (void)b.addInst(*schema.opcodeByMnemonic("shl"), rax, ops);
    });
    EXPECT_GT(rep.errorCount(), 0u)
        << "a shift with no declared encoding must NOT assemble";
    bool sawNoEncoding = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::A_NoEncodingDeclared) {
            sawNoEncoding = true;
        }
    }
    EXPECT_TRUE(sawNoEncoding);
}

// ── FC3.5 sweep-c1: the zext SOURCE-width forms (D-CSUBSET-ZEXT-32-TO-64) ──

TEST(WidthAxisX86, ZextWidth32EmitsMovR32AndDefaultKeepsMovzx) {
    // The zext mnemonic's width axis keys on the SOURCE type (threaded
    // by the MIR→LIR ZExt arm): width 32 (U32 source) = mov r32, r/m32
    // (0x8B /r, no REX.W — the 32-bit register write zero-extends);
    // width-default (Bool source / lowerICmp's setcc widener) = the
    // movzx byte form REX.W 0F B6 /r, byte-identical to pre-FC3.5.
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    auto const rcx = gpr(*schema, "rcx");
    {
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rcx)};
            (void)b.addInst(*schema->opcodeByMnemonic("zext"), rax, ops,
                            /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        // mov eax, ecx — 8B /r, ModRM = 11_000_001 = 0xC1, no REX.
        expectPrefix(bytes, {0x8B, 0xC1});
    }
    {
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rcx)};
            (void)b.addInst(*schema->opcodeByMnemonic("zext"), rax, ops);
        });
        EXPECT_EQ(rep.errorCount(), 0u);
        // movzx rax, cl — REX.W 0F B6 /r (unchanged byte shape).
        expectPrefix(bytes, {0x48, 0x0F, 0xB6, 0xC1});
    }
}

TEST(WidthAxisArm64, ZextIsWidthInvariantOneWordServesBothSources) {
    // arm64's zext = ORR Wd, WZR, Wm — the W-register write zeroes
    // bits 63:32 for ANY 32-bit source value, so the SAME word is the
    // correct realization for both the Bool 0/1 widener and the
    // U32→U64 zero-extend. The variant is deliberately width-ABSENT
    // (match-any; the xor_rdx_zero precedent) — pin both widths
    // emitting the identical word.
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const x0 = gpr(*schema, "x0");
    auto const x1 = gpr(*schema, "x1");
    for (std::uint8_t const flags : {std::uint8_t{0}, kW32}) {
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1)};
            (void)b.addInst(*schema->opcodeByMnemonic("zext"), x0, ops,
                            /*payload=*/0, flags);
        });
        EXPECT_EQ(rep.errorCount(), 0u) << unsigned(flags);
        EXPECT_EQ(firstInstWord(bytes), 0x2A0103E0u) << unsigned(flags);
    }
}

// ── red-on-disable: a stripped 32-bit variant fails loud ──────────────

TEST(WidthAxisX86, StrippedZextWidth32VariantFailsLoudNeverFallsBack) {
    // Strip the width-32 zext variant (the U32→U64 mov form): a
    // width-32 zext inst must FAIL LOUD at the matcher — silently
    // matching the byte-movzx form would read ONE byte of the U32
    // (the exact miscompile D-CSUBSET-ZEXT-32-TO-64's gate existed to
    // prevent). Proves both variants are width-KEYED, no match-any.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto& op : doc["opcodes"]) {
                if (op.value("mnemonic", "") != "zext") continue;
                auto& variants = op["encoding"]["variants"];
                variants.erase(
                    std::remove_if(
                        variants.begin(), variants.end(),
                        [](nlohmann::json const& v) {
                            return v.contains("guard")
                                && v["guard"].value("width", 0) == 32;
                        }),
                    variants.end());
            }
        });
    ASSERT_TRUE(mutated.has_value());
    auto const& schema = **mutated;
    auto const rax = gpr(schema, "rax");
    auto const rcx = gpr(schema, "rcx");
    DiagnosticReporter rep;
    (void)buildLegalizeAssemble(schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(rcx)};
        (void)b.addInst(*schema.opcodeByMnemonic("zext"), rax, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_GT(rep.errorCount(), 0u);
    bool sawNoVariantAtWidth32 = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::A_NoMatchingEncodingVariant
            && d.actual.find("width 32") != std::string::npos) {
            sawNoVariantAtWidth32 = true;
        }
    }
    EXPECT_TRUE(sawNoVariantAtWidth32);
}

TEST(WidthAxisX86, StrippedThirtyTwoBitVariantFailsLoudNeverFallsBack) {
    // Strip the width-32 reg-reg `add` variant from the shipped JSON
    // (the cycle-10k mutation substrate) and encode a width-32 add:
    // the matcher must FAIL LOUD naming width 32 — silently matching
    // the 64-bit REX.W variant would be the exact miscompile class the
    // width axis exists to kill. This is the test-tier half of the
    // red-on-disable demonstration (the corpus half: u32_wraparound's
    // exit flips 42 -> 7 when 32-bit compute is forced to 64).
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto& op : doc["opcodes"]) {
                if (op.value("mnemonic", "") != "add") continue;
                auto& variants = op["encoding"]["variants"];
                variants.erase(
                    std::remove_if(
                        variants.begin(), variants.end(),
                        [](nlohmann::json const& v) {
                            return v.contains("guard")
                                && v["guard"].value("width", 0) == 32
                                && v["guard"].contains("operandKinds")
                                && v["guard"]["operandKinds"].size() == 2;
                        }),
                    variants.end());
            }
        });
    ASSERT_TRUE(mutated.has_value());
    auto const& schema = **mutated;
    auto const rax = gpr(schema, "rax");
    auto const rcx = gpr(schema, "rcx");
    DiagnosticReporter rep;
    (void)buildLegalizeAssemble(schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(rax),
                                  LirOperand::makeReg(rcx)};
        (void)b.addInst(*schema.opcodeByMnemonic("add"), rax, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_GT(rep.errorCount(), 0u)
        << "a width-32 add with no 32-bit variant must NOT encode";
    bool sawNoVariantAtWidth32 = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::A_NoMatchingEncodingVariant
            && d.actual.find("width 32") != std::string::npos) {
            sawNoVariantAtWidth32 = true;
        }
    }
    EXPECT_TRUE(sawNoVariantAtWidth32);
}

// ═══════════════════════════════════════════════════════════════════════
// FC3.5 sweep-c2: the FLOAT arm of the width axis
// (D-CSUBSET-F32-CODEGEN + D-COND-FLOAT-NAN-TRUTHINESS-FCMP).
//
// Every new encoding byte-pinned against the manuals:
//   x86_64 (Intel SDM Vol. 2): ADDSS = F3 0F 58 /r; DIVSD = F2 0F 5E
//   /r; DIVSS = F3 0F 5E /r; CVTTSS2SI r64 = F3 REX.W 0F 2C /r;
//   CVTSD2SS = F2 0F 5A /r; CVTSS2SD = F3 0F 5A /r; UCOMISD = 66 0F
//   2E /r; UCOMISS = NP 0F 2E /r (no prefix!); MOVSS = F3 0F 10/11;
//   AND = REX.W 21 /r (+32-bit sibling); OR = REX.W 09 /r; the parity
//   setcc nibbles SETP=0F 9A / SETNP=0F 9B (SDM Vol. 1 App. B).
//   arm64 (ARM ARM C7.2): FADD-S 0x1E202800; FDIV-D 0x1E601800 /
//   FDIV-S 0x1E201800; FCMP-D 0x1E602000 / FCMP-S 0x1E202000 (quiet —
//   opcode2 bit4=0; FCMPE would be +0x10); FCVT D←S 0x1E22C000 / S←D
//   0x1E624000; FCVTZS-from-S 0x9E380000; LDUR-S 0xBC400000 / STUR-S
//   0xBC000000; CSET float conds (nibble INVERTED at bits 12..15 per
//   the CSINC alias).
// ═══════════════════════════════════════════════════════════════════════

namespace {
// NOTE: built via makePhysicalReg — the aggregate-init shape the
// file's `gpr()` helper uses ({ord, 1, cls}) sets LirReg's BITFIELD
// order {id, classKind, isPhysical}, which is only accidentally
// correct for GPR (classKind=1 == GPR, isPhysical=(GPR)1); for FPR it
// would silently build a class-GPR NON-physical reg.
[[nodiscard]] LirReg fprReg(TargetSchema const& s, std::string_view name) {
    auto const ord = s.registerByName(name);
    EXPECT_TRUE(ord.has_value()) << name;
    return makePhysicalReg(static_cast<std::uint32_t>(ord.value_or(0)),
                           LirRegClass::FPR);
}
} // namespace

TEST(FloatWidthAxisX86, AddssEmits_F3_0F_58_AndAddsdKeeps_F2) {
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const xmm0 = fprReg(*schema, "xmm0");
    auto const xmm1 = fprReg(*schema, "xmm1");
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(xmm0),
                                  LirOperand::makeReg(xmm1)};
        (void)b.addInst(*schema->opcodeByMnemonic("fadd"), xmm0, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    expectPrefix(bytes, {0xF3, 0x0F, 0x58, 0xC1});
    // Back-compat contrast: width-default keeps the F2 ADDSD bytes.
    DiagnosticReporter rep64;
    auto bytes64 = buildLegalizeAssemble(*schema, rep64, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(xmm0),
                                  LirOperand::makeReg(xmm1)};
        (void)b.addInst(*schema->opcodeByMnemonic("fadd"), xmm0, ops);
    });
    EXPECT_EQ(rep64.errorCount(), 0u);
    expectPrefix(bytes64, {0xF2, 0x0F, 0x58, 0xC1});
}

TEST(FloatWidthAxisX86, DivsdAndDivssEmit_0F_5E_WidthPrefixed) {
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const xmm0 = fprReg(*schema, "xmm0");
    auto const xmm1 = fprReg(*schema, "xmm1");
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(xmm0),
                                  LirOperand::makeReg(xmm1)};
        (void)b.addInst(*schema->opcodeByMnemonic("fdiv"), xmm0, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    expectPrefix(bytes, {0xF2, 0x0F, 0x5E, 0xC1});  // DIVSD xmm0, xmm1
    DiagnosticReporter rep32;
    auto bytes32 = buildLegalizeAssemble(*schema, rep32, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(xmm0),
                                  LirOperand::makeReg(xmm1)};
        (void)b.addInst(*schema->opcodeByMnemonic("fdiv"), xmm0, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(rep32.errorCount(), 0u);
    expectPrefix(bytes32, {0xF3, 0x0F, 0x5E, 0xC1});  // DIVSS xmm0, xmm1
}

TEST(FloatWidthAxisX86, Cvttss2siEmits_F3_RexW_0F_2C) {
    // fp_to_si width-32 = the F32-SOURCE form; REX.W stays (the
    // destination is the 64-bit GPR). Prefix precedes REX (the FC2
    // mandatory-prefix order pin).
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax  = gpr(*schema, "rax");
    auto const xmm0 = fprReg(*schema, "xmm0");
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(xmm0)};
        (void)b.addInst(*schema->opcodeByMnemonic("fp_to_si"), rax, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    expectPrefix(bytes, {0xF3, 0x48, 0x0F, 0x2C, 0xC0});
}

TEST(FloatWidthAxisX86, FpcvtKeysOnSourceWidth_5A_BothDirections) {
    // fpcvt width-64 (F64 source) = CVTSD2SS (narrow); width-32 (F32
    // source) = CVTSS2SD (widen). The SOURCE-width axis: picking the
    // result width instead would swap the two directions.
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const xmm0 = fprReg(*schema, "xmm0");
    auto const xmm1 = fprReg(*schema, "xmm1");
    DiagnosticReporter repN;
    auto narrow = buildLegalizeAssemble(*schema, repN, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(xmm1)};
        (void)b.addInst(*schema->opcodeByMnemonic("fpcvt"), xmm0, ops);
    });
    EXPECT_EQ(repN.errorCount(), 0u);
    expectPrefix(narrow, {0xF2, 0x0F, 0x5A, 0xC1});  // CVTSD2SS
    DiagnosticReporter repW;
    auto widen = buildLegalizeAssemble(*schema, repW, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(xmm1)};
        (void)b.addInst(*schema->opcodeByMnemonic("fpcvt"), xmm0, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(repW.errorCount(), 0u);
    expectPrefix(widen, {0xF3, 0x0F, 0x5A, 0xC1});   // CVTSS2SD
}

TEST(FloatWidthAxisX86, UcomisdAndUcomissEmit_0F_2E) {
    // UCOMISD = 66 0F 2E (the 66 mandatory prefix); UCOMISS = NP 0F 2E
    // — NO prefix at all (an F3 here would be an invalid encoding).
    // ModRM roles: op[0] (the predicate's LEFT side) = reg, op[1] = rm
    // — the RM-form direction (the integer cmp's 0x39 is MR-form; the
    // LIR-level convention op[0]=left is identical).
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const xmm0 = fprReg(*schema, "xmm0");
    auto const xmm1 = fprReg(*schema, "xmm1");
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(xmm0),
                                  LirOperand::makeReg(xmm1)};
        (void)b.addInst(*schema->opcodeByMnemonic("fcmp"), InvalidLirReg,
                        ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    expectPrefix(bytes, {0x66, 0x0F, 0x2E, 0xC1});
    DiagnosticReporter rep32;
    auto bytes32 = buildLegalizeAssemble(*schema, rep32, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(xmm0),
                                  LirOperand::makeReg(xmm1)};
        (void)b.addInst(*schema->opcodeByMnemonic("fcmp"), InvalidLirReg,
                        ops, /*payload=*/0, kW32);
    });
    EXPECT_EQ(rep32.errorCount(), 0u);
    expectPrefix(bytes32, {0x0F, 0x2E, 0xC1});
}

TEST(FloatWidthAxisX86, MovssLoadStoreAndRiprelEmit_F3_0F_10_11) {
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax  = gpr(*schema, "rax");
    auto const xmm1 = fprReg(*schema, "xmm1");
    // movss xmm1, [rax + 8] — F3 0F 10, ModR/M mod=10 reg=1 rm=0 →
    // 0x88, disp32 = 08 00 00 00.
    DiagnosticReporter repL;
    auto load = buildLegalizeAssemble(*schema, repL, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(rax),
                                  LirOperand::makeMemBase(1),
                                  LirOperand::makeMemOffset(8)};
        (void)b.addInst(*schema->opcodeByMnemonic("movsd_load"), xmm1, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(repL.errorCount(), 0u);
    expectPrefix(load, {0xF3, 0x0F, 0x10, 0x88, 0x08, 0x00, 0x00, 0x00});
    // movss [rax + 8], xmm1 — F3 0F 11, same ModRM roles.
    DiagnosticReporter repS;
    auto store = buildLegalizeAssemble(*schema, repS, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(xmm1),
                                  LirOperand::makeReg(rax),
                                  LirOperand::makeMemBase(1),
                                  LirOperand::makeMemOffset(8)};
        (void)b.addInst(*schema->opcodeByMnemonic("movsd_store"),
                        InvalidLirReg, ops, /*payload=*/0, kW32);
    });
    EXPECT_EQ(repS.errorCount(), 0u);
    expectPrefix(store, {0xF3, 0x0F, 0x11, 0x88, 0x08, 0x00, 0x00, 0x00});
    // The width-default contrast: the SAME loads keep F2 (movsd 8-byte).
    DiagnosticReporter repL64;
    auto load64 = buildLegalizeAssemble(*schema, repL64, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(rax),
                                  LirOperand::makeMemBase(1),
                                  LirOperand::makeMemOffset(8)};
        (void)b.addInst(*schema->opcodeByMnemonic("movsd_load"), xmm1, ops);
    });
    EXPECT_EQ(repL64.errorCount(), 0u);
    expectPrefix(load64, {0xF2, 0x0F, 0x10, 0x88, 0x08, 0x00, 0x00, 0x00});
    // The RIP-relative MOVSS form: F3 0F 10 05 <rel32> + one rel32
    // relocation at the placeholder (the F32 mirror of the F64 riprel
    // variant).
    LirBuilder b{*schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const symOps[] = {LirOperand::makeSymbolRef(7)};
    (void)b.addInst(*schema->opcodeByMnemonic("movsd_load"), xmm1, symOps,
                    /*payload=*/0, kW32);
    (void)b.addReturn(*schema->opcodeByMnemonic("ret"), {});
    Lir lir = std::move(b).finish();
    DiagnosticReporter repR;
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto r = assemble(lir, *schema, lirToMir, repR);
    EXPECT_EQ(repR.errorCount(), 0u);
    ASSERT_EQ(r.functions.size(), 1u);
    auto const& fn = r.functions[0];
    expectPrefix(fn.bytes, {0xF3, 0x0F, 0x10, 0x0D});
    ASSERT_EQ(fn.relocations.size(), 1u);
    EXPECT_EQ(fn.relocations[0].offset, 4u);
}

TEST(FloatWidthAxisX86, AndOrEmit_21_09_BothWidths) {
    // The composed-FCmp combiner substrate: AND r/m64, r64 = REX.W 21
    // /r; OR = REX.W 09 /r; the no-REX.W 32-bit siblings per the
    // established width pairs (Intel SDM AND/OR).
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    auto const rcx = gpr(*schema, "rcx");
    auto const pin = [&](char const* mnemonic, std::uint8_t opByte) {
        DiagnosticReporter rep64;
        auto b64 = buildLegalizeAssemble(*schema, rep64, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rax),
                                      LirOperand::makeReg(rcx)};
            (void)b.addInst(*schema->opcodeByMnemonic(mnemonic), rax, ops);
        });
        EXPECT_EQ(rep64.errorCount(), 0u) << mnemonic;
        expectPrefix(b64, {0x48, opByte, 0xC8});
        DiagnosticReporter rep32;
        auto b32 = buildLegalizeAssemble(*schema, rep32, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(rax),
                                      LirOperand::makeReg(rcx)};
            (void)b.addInst(*schema->opcodeByMnemonic(mnemonic), rax, ops,
                            /*payload=*/0, kW32);
        });
        EXPECT_EQ(rep32.errorCount(), 0u) << mnemonic;
        expectPrefix(b32, {opByte, 0xC8});
    };
    pin("and", 0x21);
    pin("or", 0x09);
}

TEST(FloatWidthAxisX86, FloatCondCodeSetccNibblesMatchTheSdm) {
    // The float condCodeEncoding arms drive the SAME setcc low-nibble
    // OR mechanism: fogt→seta (0F 97), foge→setae (0F 93), fone→setne
    // (0F 95), fuo→setp (0F 9A), ford→setnp (0F 9B). forceRexPrefix
    // emits the plain 0x40 REX; result rax → ModRM 0xC0.
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    auto const pin = [&](TargetCondCode cc, std::uint8_t opByte) {
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            (void)b.addInst(*schema->opcodeByMnemonic("setcc"), rax, {},
                            static_cast<std::uint32_t>(cc));
        });
        EXPECT_EQ(rep.errorCount(), 0u)
            << "cc " << targetCondCodeName(cc);
        expectPrefix(bytes, {0x40, 0x0F, opByte, 0xC0});
    };
    pin(TargetCondCode::Fogt, 0x97);
    pin(TargetCondCode::Foge, 0x93);
    pin(TargetCondCode::Fone, 0x95);
    pin(TargetCondCode::Fuo,  0x9A);
    pin(TargetCondCode::Ford, 0x9B);
}

TEST(FloatWidthAxisX86, UndeclaredFoeqSetccFailsLoud) {
    // x86 deliberately declares NO foeq nibble (sete is TRUE on
    // unordered — the composed realization is the only correct one).
    // A single-cc setcc reaching the encoder with the undeclared cond
    // must FAIL LOUD naming it, never OR a stale nibble.
    auto schema = loadX86();
    ASSERT_NE(schema, nullptr);
    auto const rax = gpr(*schema, "rax");
    DiagnosticReporter rep;
    (void)buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        (void)b.addInst(*schema->opcodeByMnemonic("setcc"), rax, {},
                        static_cast<std::uint32_t>(TargetCondCode::Foeq));
    });
    EXPECT_GT(rep.errorCount(), 0u);
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::A_NoMatchingEncodingVariant
            && d.actual.find("foeq") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}

TEST(FloatWidthAxisX86, StrippedAddssVariantFailsLoudNeverFallsBackToF2) {
    // THE float strip-variant red-on-disable (the permanent in-suite
    // pin, D-CSUBSET-F32-CODEGEN): remove the width-32 ADDSS variant →
    // a width-32 fadd must FAIL LOUD at the matcher naming width 32 —
    // silently matching the F2 ADDSD sibling would compute F32 values
    // double-width (the exact miscompile the width axis kills).
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto& op : doc["opcodes"]) {
                if (op.value("mnemonic", "") != "fadd") continue;
                auto& variants = op["encoding"]["variants"];
                variants.erase(
                    std::remove_if(
                        variants.begin(), variants.end(),
                        [](nlohmann::json const& v) {
                            return v.contains("guard")
                                && v["guard"].value("width", 0) == 32;
                        }),
                    variants.end());
            }
        });
    ASSERT_TRUE(mutated.has_value());
    auto const& schema = **mutated;
    auto const xmm0 = fprReg(schema, "xmm0");
    auto const xmm1 = fprReg(schema, "xmm1");
    DiagnosticReporter rep;
    (void)buildLegalizeAssemble(schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(xmm0),
                                  LirOperand::makeReg(xmm1)};
        (void)b.addInst(*schema.opcodeByMnemonic("fadd"), xmm0, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_GT(rep.errorCount(), 0u)
        << "a width-32 fadd with no ADDSS variant must NOT encode";
    bool sawNoVariantAtWidth32 = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::A_NoMatchingEncodingVariant
            && d.actual.find("width 32") != std::string::npos) {
            sawNoVariantAtWidth32 = true;
        }
    }
    EXPECT_TRUE(sawNoVariantAtWidth32);
}

// ── arm64: the float S-forms + FCMP/FDIV/FCVT + CSET float conds ──────

TEST(FloatWidthAxisArm64, FaddSFormClearsFtypeTo00) {
    // fadd width-32 d0, d1, d2 → FADD S0, S1, S2: base 0x1E202800
    // | Rm=2<<16 | Rn=1<<5 | Rd=0 = 0x1E222820 (LE 20 28 22 1E).
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const d0 = fprReg(*schema, "d0");
    auto const d1 = fprReg(*schema, "d1");
    auto const d2 = fprReg(*schema, "d2");
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(d1),
                                  LirOperand::makeReg(d2)};
        (void)b.addInst(*schema->opcodeByMnemonic("fadd"), d0, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes), 0x1E222820u);
    // Width-default contrast: the D-form word is unchanged.
    DiagnosticReporter rep64;
    auto bytes64 = buildLegalizeAssemble(*schema, rep64, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(d1),
                                  LirOperand::makeReg(d2)};
        (void)b.addInst(*schema->opcodeByMnemonic("fadd"), d0, ops);
    });
    EXPECT_EQ(rep64.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes64), 0x1E622820u);
}

TEST(FloatWidthAxisArm64, FdivDAndSForms) {
    // FDIV D0, D1, D2 = 0x1E601800 | 2<<16 | 1<<5 = 0x1E621820;
    // FDIV S0, S1, S2 = 0x1E201800 | ... = 0x1E221820.
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const d0 = fprReg(*schema, "d0");
    auto const d1 = fprReg(*schema, "d1");
    auto const d2 = fprReg(*schema, "d2");
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(d1),
                                  LirOperand::makeReg(d2)};
        (void)b.addInst(*schema->opcodeByMnemonic("fdiv"), d0, ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes), 0x1E621820u);
    DiagnosticReporter rep32;
    auto bytes32 = buildLegalizeAssemble(*schema, rep32, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(d1),
                                  LirOperand::makeReg(d2)};
        (void)b.addInst(*schema->opcodeByMnemonic("fdiv"), d0, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(rep32.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes32), 0x1E221820u);
}

TEST(FloatWidthAxisArm64, FcmpQuietDAndSForms) {
    // FCMP D1, D2 = 0x1E602000 | Rm=2<<16 | Rn=1<<5 = 0x1E622020
    // (opcode2 bits [4:0] = 00000 — the QUIET form; bit 4 set would
    // be FCMPE). S-form: 0x1E222020.
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const d1 = fprReg(*schema, "d1");
    auto const d2 = fprReg(*schema, "d2");
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(d1),
                                  LirOperand::makeReg(d2)};
        (void)b.addInst(*schema->opcodeByMnemonic("fcmp"), InvalidLirReg,
                        ops);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes), 0x1E622020u);
    DiagnosticReporter rep32;
    auto bytes32 = buildLegalizeAssemble(*schema, rep32, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(d1),
                                  LirOperand::makeReg(d2)};
        (void)b.addInst(*schema->opcodeByMnemonic("fcmp"), InvalidLirReg,
                        ops, /*payload=*/0, kW32);
    });
    EXPECT_EQ(rep32.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes32), 0x1E222020u);
}

TEST(FloatWidthAxisArm64, FcvtKeysOnSourceWidthBothDirections) {
    // fpcvt width-64 (F64 source) = FCVT S0, D1 (narrow): 0x1E624000 |
    // 1<<5 = 0x1E624020; width-32 (F32 source) = FCVT D0, S1 (widen):
    // 0x1E22C000 | 1<<5 = 0x1E22C020.
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const d0 = fprReg(*schema, "d0");
    auto const d1 = fprReg(*schema, "d1");
    DiagnosticReporter repN;
    auto narrow = buildLegalizeAssemble(*schema, repN, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(d1)};
        (void)b.addInst(*schema->opcodeByMnemonic("fpcvt"), d0, ops);
    });
    EXPECT_EQ(repN.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(narrow), 0x1E624020u);
    DiagnosticReporter repW;
    auto widen = buildLegalizeAssemble(*schema, repW, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(d1)};
        (void)b.addInst(*schema->opcodeByMnemonic("fpcvt"), d0, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(repW.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(widen), 0x1E22C020u);
}

TEST(FloatWidthAxisArm64, FcvtzsFromSForm) {
    // fp_to_si width-32 x0, d1 → FCVTZS X0, S1 (ftype=00):
    // 0x9E380000 | 1<<5 = 0x9E380020. The width-default D-source form
    // stays 0x9E780020.
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const x0 = gpr(*schema, "x0");
    auto const d1 = fprReg(*schema, "d1");
    DiagnosticReporter rep;
    auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(d1)};
        (void)b.addInst(*schema->opcodeByMnemonic("fp_to_si"), x0, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes), 0x9E380020u);
    DiagnosticReporter rep64;
    auto bytes64 = buildLegalizeAssemble(*schema, rep64, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(d1)};
        (void)b.addInst(*schema->opcodeByMnemonic("fp_to_si"), x0, ops);
    });
    EXPECT_EQ(rep64.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(bytes64), 0x9E780020u);
}

TEST(FloatWidthAxisArm64, LdurSturSFormsAreFourByteAccesses) {
    // LDUR S1, [X0, #8] = 0xBC400000 | 8<<12 | 0<<5 | 1 = 0xBC408001;
    // STUR S1, [X0, #8] = 0xBC000000 | ... = 0xBC008001. The size=10
    // S-forms read/write exactly 4 bytes — the type-exact access width
    // for F32 rodata items.
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const x0 = gpr(*schema, "x0");
    auto const d1 = fprReg(*schema, "d1");
    DiagnosticReporter repL;
    auto load = buildLegalizeAssemble(*schema, repL, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(x0),
                                  LirOperand::makeMemBase(1),
                                  LirOperand::makeMemOffset(8)};
        (void)b.addInst(*schema->opcodeByMnemonic("fldur"), d1, ops,
                        /*payload=*/0, kW32);
    });
    EXPECT_EQ(repL.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(load), 0xBC408001u);
    DiagnosticReporter repS;
    auto store = buildLegalizeAssemble(*schema, repS, [&](LirBuilder& b) {
        LirOperand const ops[] = {LirOperand::makeReg(d1),
                                  LirOperand::makeReg(x0),
                                  LirOperand::makeMemBase(1),
                                  LirOperand::makeMemOffset(8)};
        (void)b.addInst(*schema->opcodeByMnemonic("fstur"), InvalidLirReg,
                        ops, /*payload=*/0, kW32);
    });
    EXPECT_EQ(repS.errorCount(), 0u);
    EXPECT_EQ(firstInstWord(store), 0xBC008001u);
}

TEST(FloatWidthAxisArm64, CsetFloatCondsInvertNibbleAtBit12) {
    // CSET Xd, cond = CSINC Xd, XZR, XZR, invert(cond): base
    // 0x9A9F07E0 | (invert(nibble) << 12) | Rd. Float conds: fogt =
    // GT(0xC)→0xD; foge = GE(0xA)→0xB; foeq = EQ(0)→1; fune =
    // NE(1)→0; fuo = VS(6)→7; ford = VC(7)→6 (the ^1 inversion pairs
    // each condition with its complement).
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const x0 = gpr(*schema, "x0");
    auto const pin = [&](TargetCondCode cc, std::uint32_t word) {
        DiagnosticReporter rep;
        auto bytes = buildLegalizeAssemble(*schema, rep, [&](LirBuilder& b) {
            (void)b.addInst(*schema->opcodeByMnemonic("setcc"), x0, {},
                            static_cast<std::uint32_t>(cc));
        });
        EXPECT_EQ(rep.errorCount(), 0u) << targetCondCodeName(cc);
        EXPECT_EQ(firstInstWord(bytes), word) << targetCondCodeName(cc);
    };
    pin(TargetCondCode::Fogt, 0x9A9FD7E0u);  // cset x0, gt
    pin(TargetCondCode::Foge, 0x9A9FB7E0u);  // cset x0, ge
    pin(TargetCondCode::Foeq, 0x9A9F17E0u);  // cset x0, eq
    pin(TargetCondCode::Fune, 0x9A9F07E0u);  // cset x0, ne
    pin(TargetCondCode::Fuo,  0x9A9F77E0u);  // cset x0, vs
    pin(TargetCondCode::Ford, 0x9A9F67E0u);  // cset x0, vc
}

// ── loader validation: the width vocabulary is closed ─────────────────

TEST(WidthAxisLoader, GuardWidthSixteenIsRejected) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto& op : doc["opcodes"]) {
                if (op.value("mnemonic", "") != "neg") continue;
                op["encoding"]["variants"][0]["guard"]["width"] = 16;
            }
        });
    EXPECT_FALSE(mutated.has_value())
        << "guard width 16 must be a load-time reject (only 32/64 are "
           "encodable this cycle)";
}

TEST(WidthAxisLoader, AmbiguousWidthMixIsRejected) {
    // A width-keyed variant + a width-ABSENT same-operand-kind sibling:
    // first-match would shadow or silently absorb — validate() rejects.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto& op : doc["opcodes"]) {
                if (op.value("mnemonic", "") != "neg") continue;
                // neg ships width-64 + width-32 single-reg variants;
                // strip the width key from the FIRST → mix.
                op["encoding"]["variants"][0]["guard"].erase("width");
            }
        });
    EXPECT_FALSE(mutated.has_value())
        << "mixing width-keyed and width-absent same-kind variants must "
           "be rejected at load";
}

TEST(WidthAxisLoader, DuplicateKindsAndWidthIsRejected) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto& op : doc["opcodes"]) {
                if (op.value("mnemonic", "") != "neg") continue;
                // Make the 32-bit variant a duplicate of the 64-bit one.
                op["encoding"]["variants"][1]["guard"]["width"] = 64;
            }
        });
    EXPECT_FALSE(mutated.has_value())
        << "two variants with identical (operandKinds, width) must be "
           "rejected at load (first-match shadow)";
}
