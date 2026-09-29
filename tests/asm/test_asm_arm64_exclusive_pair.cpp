// ★★★ arm64's EXCLUSIVE PAIR — `ldaxr` / `stlxr` — WRITTEN IN ASSEMBLY TEXT, AND
// THE STATUS-REGISTER OVERLAP THE ARCHITECTURE MAKES UNPREDICTABLE (P68 round 8
// part 4, the LL/SC pin of
// D-LIR-ASM-TEMPLATE-SPILL-CODE-LANDS-BETWEEN-TEMPLATE-INSTRUCTIONS).
//
// ✔MEASURED 2026-09-23, aarch64-linux-gnu-as 2.42 and clang 18.1.3
// (`--target=aarch64-linux-gnu`), one instruction per file, `objdump -d`:
//   ldaxr w0, [x2]      885ffc40        stlxr w1, w0, [x2]   8801fc40
//   ldaxr x0, [x2]      c85ffc40        stlxr w1, x0, [x2]   c801fc40
//   ldaxr w0, [sp]      885fffe0        stlxr wzr, w0, [sp]  clang 881fffe0
//   ldaxr w0, [x2, #8]  refused by both stlxr x1, w0, [x2]   refused by both
//   stlxr w0, w0, [x2] / stlxr w2, w0, [x2]: gas WARNS and assembles, clang
//   REFUSES. 📄 Arm ARM: Rs == Rt, or Rs == Rn with Rn not SP, is CONSTRAINED
//   UNPREDICTABLE — no program that WORKS, so DSS refuses with clang.
// The overlap is refused at EVERY tier that can see it, each pinned here:
//   * the text lowering, which can point at the line (a register written in
//     the text, or an operand bound to a physical register);
//   * the builder, which marks the result EARLY for every producer, so no
//     allocation can produce the overlap;
//   * the encoder backstop, for an instruction handed over with it anyway.

#include "asm/asm.hpp"
#include "asm/asm_template_to_lir.hpp"
#include "asm_text_fixture.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using dss::test_support::asm_text::lowerAsmText;
using dss::test_support::asm_text::messages;
using dss::test_support::asm_text::parseMessages;
using dss::test_support::asm_text::parsedCleanly;
using dss::test_support::asm_text::shippedDialectDoc;

namespace {

using Bytes = std::vector<std::uint8_t>;

struct Assembled {
    bool        parsed  = false;
    bool        lowered = false;
    bool        encoded = false;
    Bytes       bytes;
    std::string diagnostics;
};

[[nodiscard]] Bytes le32(std::uint32_t w) {
    return {static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8),
            static_cast<std::uint8_t>(w >> 16), static_cast<std::uint8_t>(w >> 24)};
}

[[nodiscard]] Bytes words(std::vector<std::uint32_t> const& ws) {
    Bytes out;
    for (auto const w : ws) {
        auto const b = le32(w);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

[[nodiscard]] std::string hex(Bytes const& b) {
    std::string out;
    for (auto const v : b) out += std::format("{:02x} ", v);
    return out;
}

constexpr std::uint32_t kRet = 0xD65F03C0u;

// `main:` + the body + `ret`, through the shipped arm64 dialect and target, to
// the function's bytes.
[[nodiscard]] Assembled assembleBody(std::string const& body) {
    Assembled out;
    std::string const src =
        ".globl main\n.type main, %function\nmain:\n" + body + "  ret\n";
    auto const run = lowerAsmText(shippedDialectDoc("asm-arm64-gas"), src, "arm64");
    out.parsed      = parsedCleanly(*run);
    out.diagnostics = parseMessages(*run) + messages(*run);
    if (!run->module.has_value()) return out;
    out.lowered = true;
    std::vector<MirInstId> lirToMir(run->module->lir.instCount());
    DiagnosticReporter     rep;
    auto const mod = assemble(run->module->lir, *run->target, lirToMir, rep);
    for (auto const& diag : rep.all()) out.diagnostics += diag.actual + "\n";
    if (rep.errorCount() != 0 || mod.functions.size() != 1) return out;
    out.encoded = true;
    out.bytes   = mod.functions[0].bytes;
    return out;
}

void expectWords(std::string_view insn, std::uint32_t want) {
    SCOPED_TRACE(std::string{insn});
    auto const a = assembleBody("  " + std::string{insn} + "\n");
    ASSERT_TRUE(a.parsed) << a.diagnostics;
    ASSERT_TRUE(a.encoded) << "refused: " << a.diagnostics;
    EXPECT_EQ(a.bytes, words({want, kRet}))
        << "got " << hex(a.bytes) << " want " << hex(words({want, kRet}));
}

// Refused at lowering, with the parse proven clean first and `word` named.
void expectRefused(std::string_view insn, std::string_view word, std::string_view why) {
    SCOPED_TRACE(std::string{insn});
    auto const a = assembleBody("  " + std::string{insn} + "\n");
    ASSERT_TRUE(a.parsed) << a.diagnostics;
    EXPECT_FALSE(a.encoded) << why << " — DSS assembled it to " << hex(a.bytes);
    EXPECT_NE(a.diagnostics.find(word), std::string::npos) << a.diagnostics;
}

// One template through the shipped arm64 dialect with `%0`/`%1`/`%2` bound to
// the named physical registers, lowered and encoded, then `ret`.
[[nodiscard]] Assembled assembleTemplate(
    std::string_view text,
    std::vector<std::pair<std::string, std::string>> const& bound) {
    Assembled out;
    auto grammar = GrammarSchema::loadFromText(shippedDialectDoc("asm-arm64-gas").dump(),
                                               "asm-arm64-gas");
    auto target  = TargetSchema::loadShipped("arm64");
    if (!grammar.has_value() || !target.has_value()) {
        throw std::runtime_error{"the shipped arm64 dialect or target did not load"};
    }
    DiagnosticReporter rep;
    auto tree = parseAsmTemplateText(std::string{text}, "<template>", *grammar,
                                     AsmTemplateSurface::Extended,
                                     DiagnosticBudget::libraryDefault(), rep);
    out.parsed = tree.has_value();
    if (!out.parsed) {
        for (auto const& diag : rep.all()) out.diagnostics += diag.actual + "\n";
        return out;
    }
    LirBuilder builder{**target};
    builder.addFunction(SymbolId{1});
    LirBlockId const entry = builder.createBlock();
    builder.beginBlock(entry);
    std::vector<AsmOperandBinding> bindings;
    for (auto const& [spelling, physical] : bound) {
        AsmOperandBinding ob;
        ob.spelling  = spelling;
        ob.regClass  = LirRegClass::GPR;
        ob.widthBits = 64;
        auto const ord = (*target)->registerByName(physical);
        if (!ord.has_value()) throw std::runtime_error{"no register " + physical};
        ob.reg = makePhysicalReg(*ord, LirRegClass::GPR);
        bindings.push_back(std::move(ob));
    }
    out.lowered = lowerAsmTemplateToLirRun(*tree, **grammar, **target, bindings,
                                           builder, rep);
    for (auto const& diag : rep.all()) out.diagnostics += diag.actual + "\n";
    if (!out.lowered) return out;
    auto const retOp = (*target)->opcodeByMnemonic("ret");
    if (!retOp.has_value()) throw std::runtime_error{"target has no `ret`"};
    builder.addReturn(*retOp, {});
    Lir lir = std::move(builder).finish();
    DiagnosticReporter     asmRep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto const mod = assemble(lir, **target, lirToMir, asmRep);
    for (auto const& diag : asmRep.all()) out.diagnostics += diag.actual + "\n";
    if (asmRep.errorCount() != 0 || mod.functions.size() != 1) return out;
    out.encoded = true;
    out.bytes   = mod.functions[0].bytes;
    return out;
}

}  // namespace

// ══ the measured encodings ═══════════════════════════════════════════════════
TEST(AsmArm64ExclusivePair, TheMeasuredEncodings) {
    expectWords("ldaxr w0, [x2]", 0x885FFC40u);
    expectWords("ldaxr x0, [x2]", 0xC85FFC40u);
    expectWords("ldaxr w0, [sp]", 0x885FFFE0u);
    expectWords("stlxr w1, w0, [x2]", 0x8801FC40u);
    expectWords("stlxr w1, x0, [x2]", 0xC801FC40u);
    // Two DIFFERENT registers that both encode 31 — clang accepts it.
    expectWords("stlxr wzr, w0, [sp]", 0x881FFFE0u);
}

TEST(AsmArm64ExclusivePair, WhatBothReferencesRefuseIsRefused) {
    auto const offset = assembleBody("  ldaxr w0, [x2, #8]\n");
    ASSERT_TRUE(offset.parsed) << offset.diagnostics;
    EXPECT_FALSE(offset.encoded) << "an exclusive access takes no offset";
    auto const xStatus = assembleBody("  stlxr x1, w0, [x2]\n");
    ASSERT_TRUE(xStatus.parsed) << xStatus.diagnostics;
    EXPECT_FALSE(xStatus.encoded) << "the status register is a W register";
}

// ══ the UNPREDICTABLE overlap — the text lowering names the line ═════════════
// RED-ON-DISABLE: drop `resultEarlyClobber` from the shipped `stlxr` row and
// both refusals turn into encodings (gas's bytes, which are not a program).
TEST(AsmArm64ExclusivePair, AStatusThatIsTheDataOrBaseRegisterIsRefused) {
    expectRefused("stlxr w0, w0, [x2]", "also reads",
                  "the status register is the data register");
    expectRefused("stlxr w2, w0, [x2]", "also reads",
                  "the status register is the base register");
    // `w1` and `x1` are ONE register: the width view does not make it another.
    expectRefused("stlxr w1, x1, [x2]", "also reads",
                  "the status register is the data register at another width");
}

TEST(AsmArm64ExclusivePair, TheLlScTemplateEncodesAndItsOverlapIsRefused) {
    auto const ok = assembleTemplate(
        "ldaxr %w0, [%2]\n"
        "add %w0, %w0, #1\n"
        "stlxr %w1, %w0, [%2]",
        {{"%0", "x0"}, {"%1", "x1"}, {"%2", "x2"}});
    ASSERT_TRUE(ok.parsed) << ok.diagnostics;
    ASSERT_TRUE(ok.encoded) << ok.diagnostics;
    EXPECT_EQ(ok.bytes, words({0x885FFC40u, 0x11000400u, 0x8801FC40u, kRet}))
        << "got " << hex(ok.bytes);

    // The same template with the status bound to the DATA register.
    auto const clash = assembleTemplate(
        "ldaxr %w0, [%2]\n"
        "stlxr %w1, %w0, [%2]",
        {{"%0", "x0"}, {"%1", "x0"}, {"%2", "x2"}});
    ASSERT_TRUE(clash.parsed) << clash.diagnostics;
    EXPECT_FALSE(clash.encoded);
    EXPECT_NE(clash.diagnostics.find("also reads"), std::string::npos)
        << clash.diagnostics;
}

// ══ the builder marks the result EARLY, for every producer ═══════════════════
TEST(AsmArm64ExclusivePair, TheBuilderMarksADeclaredEarlyClobberResult) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto const stlxr = (*target)->opcodeByMnemonic("stlxr");
    auto const ldaxr = (*target)->opcodeByMnemonic("ldaxr");
    ASSERT_TRUE(stlxr.has_value() && ldaxr.has_value());
    ASSERT_TRUE((*target)->opcodeInfo(*stlxr)->resultEarlyClobber)
        << "the shipped arm64 target declares it on `stlxr`";
    EXPECT_FALSE((*target)->opcodeInfo(*ldaxr)->resultEarlyClobber) << "control";

    LirBuilder builder{**target};
    builder.addFunction(SymbolId{1});
    LirBlockId const entry = builder.createBlock();
    builder.beginBlock(entry);
    LirReg const ptr    = builder.newVReg(LirRegClass::GPR);
    LirReg const val    = builder.newVReg(LirRegClass::GPR);
    LirReg const status = builder.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const ldOps{LirOperand::makeReg(ptr)};
    LirInstId const ld = builder.addInst(*ldaxr, val, ldOps, 0, kLirInstFlagWidth32);
    std::array<LirOperand, 2> const stOps{LirOperand::makeReg(val),
                                          LirOperand::makeReg(ptr)};
    LirInstId const st = builder.addInst(*stlxr, status, stOps, 0, kLirInstFlagWidth32);
    auto const retOp = (*target)->opcodeByMnemonic("ret");
    ASSERT_TRUE(retOp.has_value());
    builder.addReturn(*retOp, {});
    Lir const lir = std::move(builder).finish();
    EXPECT_TRUE(lirInstResultIsEarlyClobber(lir.instFlags(st)))
        << "a producer that never heard of the rule gets it all the same";
    EXPECT_EQ(lirInstWidthBits(lir.instFlags(st)), 32) << "the width flag survives";
    EXPECT_FALSE(lirInstResultIsEarlyClobber(lir.instFlags(ld))) << "control";
}

// ══ the encoder backstop — every format passes it ════════════════════════════
TEST(AsmArm64ExclusivePair, TheAssemblerRefusesAnOverlapItIsHandedAnyway) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto const stlxr = (*target)->opcodeByMnemonic("stlxr");
    auto const retOp = (*target)->opcodeByMnemonic("ret");
    auto const x1    = (*target)->registerByName("x1");
    auto const x2    = (*target)->registerByName("x2");
    auto const x3    = (*target)->registerByName("x3");
    ASSERT_TRUE(stlxr && retOp && x1 && x2 && x3);
    auto const encodeWithStatus = [&](std::uint16_t statusOrdinal) {
        LirBuilder builder{**target};
        builder.addFunction(SymbolId{1});
        LirBlockId const entry = builder.createBlock();
        builder.beginBlock(entry);
        std::array<LirOperand, 2> const ops{
            LirOperand::makeReg(makePhysicalReg(*x1, LirRegClass::GPR)),
            LirOperand::makeReg(makePhysicalReg(*x2, LirRegClass::GPR))};
        builder.addInst(*stlxr, makePhysicalReg(statusOrdinal, LirRegClass::GPR),
                        ops, 0, kLirInstFlagWidth32);
        builder.addReturn(*retOp, {});
        Lir lir = std::move(builder).finish();
        DiagnosticReporter     rep;
        std::vector<MirInstId> lirToMir(lir.instCount());
        (void)assemble(lir, **target, lirToMir, rep);
        std::string text;
        for (auto const& d : rep.all()) {
            if (d.code == DiagnosticCode::A_NoMatchingEncodingVariant) text += d.actual;
        }
        return std::pair{rep.errorCount(), text};
    };
    auto const [clashErrors, clashText] = encodeWithStatus(*x1);
    EXPECT_NE(clashErrors, 0u) << "status x1 over data x1 must not encode";
    EXPECT_NE(clashText.find("UNPREDICTABLE"), std::string::npos) << clashText;
    auto const [baseErrors, baseText] = encodeWithStatus(*x2);
    EXPECT_NE(baseErrors, 0u) << "status x2 over base x2 must not encode";
    auto const [okErrors, okText] = encodeWithStatus(*x3);
    EXPECT_EQ(okErrors, 0u) << "control: a status register of its own — " << okText;
}
