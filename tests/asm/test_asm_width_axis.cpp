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

TEST(WidthAxisArm64, SxtwAndTruncWords) {
    auto schema = loadArm();
    ASSERT_NE(schema, nullptr);
    auto const x0 = gpr(*schema, "x0");
    auto const x1 = gpr(*schema, "x1");
    {
        // SXTW X0, W1 = SBFM X0, X1, #0, #31 = 0x93407C20 (the NEW
        // arm64 `sext` opcode; source on Rn).
        DiagnosticReporter rep;
        auto bytes = buildAndAssembleArm(*schema, rep, [&](LirBuilder& b) {
            LirOperand const ops[] = {LirOperand::makeReg(x1)};
            (void)b.addInst(*schema->opcodeByMnemonic("sext"), x0, ops);
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

// ── red-on-disable: a stripped 32-bit variant fails loud ──────────────

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
