// SPELLINGS A REFERENCE ASSEMBLER ACCEPTS, ONE PIN PER GAP — P68 round 8,
// D-ASM-DIALECT-GAPS-A-REFERENCE-ASSEMBLER-ACCEPTS and
// D-ASM-DOTTED-LABEL-AS-A-BRANCH-TARGET-REFUSED.
//
// ═══ WHAT EACH ARM PINS ════════════════════════════════════════════════════════
//
// Every spelling below was REFUSED by DSS at the part-3 base and is accepted by
// GNU as 2.42 and clang 18.1.3 (✔MEASURED 2026-09-21, each reference SEPARATELY,
// the words read back with objdump; the measured words are quoted beside each
// arm). Each arm assembles the spelling through the SHIPPED dialect and target
// and asserts the bytes DSS emits.
//
// ⚠ WHERE DSS'S BYTES DIFFER FROM GAS'S, THE DIFFERENCE IS AN EQUIVALENT
// ENCODING THIS TARGET ALREADY USES FOR EVERY INSTRUCTION OF THE SHAPE, and the
// arm says which: the x86 target writes a memory operand with a 32-bit
// displacement (mod=10) where gas picks the shortest; it writes a register move
// in the 8B (r, r/m) direction; its byte forms carry a REX prefix (`40`) that is
// a no-op on al/cl and REQUIRED on sil/dil (`forceRexPrefix`, the byte-register
// rule); and it jumps with rel32. objdump decodes both byte strings to the SAME
// instruction — ✔MEASURED for every arm here (`fo3/j5g/cb/x86_all.s`,
// `a64_all.s`, side by side).
//
// ★ AND EVERY ARM THAT WIDENS ACCEPTANCE HAS ITS REFUSAL CONTROL: the width a
// GPR name states still refuses a disagreement (`movl %eax, %rcx`), an
// unsuffixed spelling with disagreeing names is refused as both references
// refuse it, and a suffixed spelling never takes the unstated-width warning.
// The ELEMENT arms (L) carry eighteen refusals of their own, each measured
// refused by gas and clang, beside the one form only gas accepts.

#include "asm/asm.hpp"
#include "asm/asm_template_to_lir.hpp"
#include "asm_text_fixture.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_reg.hpp"

#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::test_support::asm_text::lowerAsmText;
using dss::test_support::asm_text::messages;
using dss::test_support::asm_text::parseMessages;
using dss::test_support::asm_text::parsedCleanly;
using dss::test_support::asm_text::shippedDialectDoc;

namespace {

using Bytes = std::vector<std::uint8_t>;

struct Dialect {
    std::string_view language;
    std::string_view target;
    std::string_view functionMarker;
};
constexpr Dialect kX86{"asm-x86_64-att", "x86_64", "@function"};
constexpr Dialect kArm{"asm-arm64-gas", "arm64", "%function"};

struct Assembled {
    bool        parsed   = false;
    bool        lowered  = false;
    bool        encoded  = false;
    Bytes       bytes;
    std::string diagnostics;
    std::size_t warnings = 0;
    std::string warningText;
};

[[nodiscard]] std::string hex(Bytes const& b) {
    std::string out;
    for (auto const v : b) out += std::format("{:02x} ", v);
    return out;
}

// `main:` + the body + a return, through the shipped dialect and target, to
// the one function's bytes.
[[nodiscard]] Assembled assembleBody(Dialect const& d, std::string const& body) {
    Assembled out;
    std::string const src = std::string{".globl main\n.type main, "}
                          + std::string{d.functionMarker} + "\nmain:\n" + body
                          + "  ret\n";
    auto const run = lowerAsmText(shippedDialectDoc(d.language), src, d.target);
    out.parsed      = parsedCleanly(*run);
    out.diagnostics = parseMessages(*run) + messages(*run);
    for (auto const& diag : run->reporter.all()) {
        if (diag.severity == DiagnosticSeverity::Warning) {
            ++out.warnings;
            out.warningText += diag.actual;
        }
    }
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

[[nodiscard]] Bytes le32(std::uint32_t w) {
    return {static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8),
            static_cast<std::uint8_t>(w >> 16), static_cast<std::uint8_t>(w >> 24)};
}

constexpr std::uint32_t kArmRet = 0xD65F03C0u;

// One x86 instruction, then `ret` (c3).
void expectX86(std::string_view insn, Bytes want) {
    SCOPED_TRACE(std::string{insn});
    auto const a = assembleBody(kX86, "  " + std::string{insn} + "\n");
    ASSERT_TRUE(a.parsed) << a.diagnostics;
    ASSERT_TRUE(a.encoded) << "refused: " << a.diagnostics;
    want.push_back(0xC3);
    EXPECT_EQ(a.bytes, want) << "got " << hex(a.bytes) << " want " << hex(want);
}

// One aarch64 instruction (or several), then `ret`.
void expectArm(std::string_view insns, std::vector<std::uint32_t> words) {
    SCOPED_TRACE(std::string{insns});
    auto const a = assembleBody(kArm, std::string{insns});
    ASSERT_TRUE(a.parsed) << a.diagnostics;
    ASSERT_TRUE(a.encoded) << "refused: " << a.diagnostics;
    Bytes want;
    for (auto const w : words) {
        auto const b = le32(w);
        want.insert(want.end(), b.begin(), b.end());
    }
    auto const r = le32(kArmRet);
    want.insert(want.end(), r.begin(), r.end());
    EXPECT_EQ(a.bytes, want) << "got " << hex(a.bytes) << " want " << hex(want);
}

void expectRefused(Dialect const& d, std::string_view insn, std::string_view why) {
    SCOPED_TRACE(std::string{insn});
    auto const a = assembleBody(d, "  " + std::string{insn} + "\n");
    EXPECT_FALSE(a.encoded) << why << " — DSS assembled it to " << hex(a.bytes);
}

// ── the TEMPLATE surface: operands bound to physical registers ──────────────
struct Bound {
    std::string   spelling;   // `%0`
    std::string   physical;   // the target register the operand is bound to
    std::uint32_t widthBits;  // the C value's width
    LirRegClass   cls = LirRegClass::GPR;
};

// One `__asm__` template taken to BYTES through the shipped dialect of `d`,
// every operand bound to a physical register (an unallocated vreg has no
// encoding), then `ret`.
[[nodiscard]] Assembled assembleTemplate(Dialect const& d, std::string_view text,
                                         std::vector<Bound> const& bound) {
    Assembled out;
    auto doc     = shippedDialectDoc(d.language);
    auto grammar = GrammarSchema::loadFromText(doc.dump(), std::string{d.language});
    auto target  = TargetSchema::loadShipped(d.target);
    if (!grammar.has_value() || !target.has_value()) {
        throw std::runtime_error{"the shipped dialect or target did not load"};
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
    for (auto const& b : bound) {
        AsmOperandBinding ob;
        ob.spelling  = b.spelling;
        ob.regClass  = b.cls;
        ob.widthBits = b.widthBits;
        auto const ord = (*target)->registerByName(b.physical);
        if (!ord.has_value()) throw std::runtime_error{"no register " + b.physical};
        ob.reg = makePhysicalReg(*ord, b.cls);
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

// ══ (A) an xmm name states no width ═══════════════════════════════════════════
// gas and clang: `addsd %xmm1, %xmm0` = f2 0f 58 c1, `movsd %xmm1, %xmm0` =
// f2 0f 10 c1 — identical here.
TEST(AsmReferenceAcceptedSpellings, AnXmmNameStatesNoOperationWidth) {
    expectX86("addsd %xmm1, %xmm0", {0xF2, 0x0F, 0x58, 0xC1});
    expectX86("movsd %xmm1, %xmm0", {0xF2, 0x0F, 0x10, 0xC1});
    expectX86("addss %xmm1, %xmm0", {0xF3, 0x0F, 0x58, 0xC1});
}

// The control: a GPR name still states its width, and two that disagree are
// refused — by both references too (`operand type mismatch`).
TEST(AsmReferenceAcceptedSpellings, AGprNameStillStatesItsWidth) {
    expectRefused(kX86, "movl %eax, %rcx", "the names disagree");
    expectRefused(kX86, "mov %eax, %rcx", "the names disagree");
}

// The key's load-time contract: where width VIEWS exist, every name states a
// width, so the key is refused on a row that IS a view and on one that HAS one.
TEST(AsmReferenceAcceptedSpellings, NameStatesNoWidthIsRefusedWhereAWidthViewExists) {
    auto const setOn = [](std::string row) {
        return [row](nlohmann::json& doc) {
            for (auto& r : doc.at("registers")) {
                if (r.value("name", std::string{}) == row) {
                    r["nameStatesNoWidth"] = true;
                }
            }
        };
    };
    auto const isView  = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", setOn("eax"));
    auto const hasView = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", setOn("rax"));
    EXPECT_FALSE(isView.has_value()) << "`eax` IS a width view of `rax`";
    EXPECT_FALSE(hasView.has_value()) << "`rax` HAS width views";
    for (auto const* r : {&isView, &hasView}) {
        if (r->has_value()) continue;
        bool named = false;
        for (auto const& e : r->error()) {
            named = named || e.message.find("nameStatesNoWidth") != std::string::npos;
        }
        EXPECT_TRUE(named) << "the refusal must name the key";
    }
    // And a non-boolean is refused rather than coerced.
    auto const notBool = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto& r : doc.at("registers")) {
                if (r.value("name", std::string{}) == "xmm3") {
                    r["nameStatesNoWidth"] = 1;
                }
            }
        });
    EXPECT_FALSE(notBool.has_value());
}

// `unstatedWidth`'s load-time contract: a width in bits, and only on a row that
// DERIVES its width.
TEST(AsmReferenceAcceptedSpellings, UnstatedWidthIsLegalOnlyWhereTheWidthIsDerived) {
    auto const load = [](auto&& edit) {
        auto doc = shippedDialectDoc(kX86.language);
        edit(doc);
        return GrammarSchema::loadFromText(doc.dump(), "<mutated att>");
    };
    auto const rowNamed = [](nlohmann::json& doc, std::string const& sp)
        -> nlohmann::json& {
        for (auto& r : doc.at("assembly").at("instructions")) {
            if (r.value("spelling", std::string{}) == sp) return r;
        }
        throw std::runtime_error{"no row " + sp};
    };
    EXPECT_TRUE(load([](nlohmann::json&) {}).has_value()) << "the control";
    EXPECT_FALSE(load([&](nlohmann::json& doc) {
        rowNamed(doc, "movq")["unstatedWidth"] = 32;   // beside `width`
    }).has_value());
    EXPECT_FALSE(load([&](nlohmann::json& doc) {
        rowNamed(doc, "mov")["unstatedWidth"] = 24;    // not a width
    }).has_value());
    EXPECT_FALSE(load([&](nlohmann::json& doc) {
        rowNamed(doc, "movq")["unstatedWidthWarns"] = true;   // nothing to warn of
    }).has_value());
}

// A width the MNEMONIC states is not a default: `movaps` between two literal
// xmm registers is a 128-bit move with no warning (gas and clang: 0f 28 c1,
// silent), while the operands of a template may still narrow it.
TEST(AsmReferenceAcceptedSpellings, AMnemonicsOwnWidthDrawsNoWarning) {
    auto const a = assembleBody(kX86, "  movaps %xmm1, %xmm2\n");
    ASSERT_TRUE(a.encoded) << a.diagnostics;
    EXPECT_EQ(a.bytes, (Bytes{0x0F, 0x28, 0xD1, 0xC3})) << hex(a.bytes);
    EXPECT_EQ(a.warnings, 0u) << a.warningText;
}

// ══ (B) the cross-bank moves ═════════════════════════════════════════════════
// gas and clang, identical here.
TEST(AsmReferenceAcceptedSpellings, CrossBankMovqAndMovdElectByRegisterClass) {
    expectX86("movq %xmm0, %rax", {0x66, 0x48, 0x0F, 0x7E, 0xC0});
    expectX86("movq %rax, %xmm0", {0x66, 0x48, 0x0F, 0x6E, 0xC0});
    expectX86("movd %xmm0, %eax", {0x66, 0x0F, 0x7E, 0xC0});
    expectX86("movd %eax, %xmm0", {0x66, 0x0F, 0x6E, 0xC0});
}

// ══ (C) movapd ═══════════════════════════════════════════════════════════════
// gas and clang: 66 0f 28 c1 — identical. `movaps` stays 0f 28 c1.
TEST(AsmReferenceAcceptedSpellings, MovapdIsItsOwnEncoding) {
    expectX86("movapd %xmm1, %xmm0", {0x66, 0x0F, 0x28, 0xC1});
    expectX86("movaps %xmm1, %xmm0", {0x0F, 0x28, 0xC1});
}

// ══ (D) the test family ══════════════════════════════════════════════════════
// gas and clang: testq %rax,%rcx = 48 85 c1, testw %ax,%cx = 66 85 c1,
// testb %al,%cl = 84 c1 (here with the no-op REX), testq $1,%rcx =
// 48 f7 c1 01 00 00 00, testw $300,%cx = 66 f7 c1 2c 01, testb $1,%cl = f6 c1 01
// (+REX), testl $1,(%rdi) = f7 07 01.. (here disp32), and testl in BOTH operand
// orders against memory is ONE encoding, 85 /r.
TEST(AsmReferenceAcceptedSpellings, TheTestFamily) {
    expectX86("testq %rax, %rcx", {0x48, 0x85, 0xC1});
    expectX86("testl %eax, %ecx", {0x85, 0xC1});
    expectX86("testw %ax, %cx", {0x66, 0x85, 0xC1});
    expectX86("testb %al, %cl", {0x40, 0x84, 0xC1});
    expectX86("testq $1, %rcx", {0x48, 0xF7, 0xC1, 0x01, 0x00, 0x00, 0x00});
    expectX86("testw $300, %cx", {0x66, 0xF7, 0xC1, 0x2C, 0x01});
    expectX86("testb $1, %cl", {0x40, 0xF6, 0xC1, 0x01});
    expectX86("testl $1, (%rdi)",
              {0xF7, 0x87, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00});
    expectX86("testl %eax, (%rdi)", {0x85, 0x87, 0x00, 0x00, 0x00, 0x00});
    expectX86("testl (%rdi), %eax", {0x85, 0x87, 0x00, 0x00, 0x00, 0x00});
}

// ══ (E) the setcc spellings ══════════════════════════════════════════════════
// gas and clang: sete %al = 0f 94 c0 (+REX here), setg %sil = 40 0f 9f c6,
// setb %r8b = 41 0f 92 c0 — the condition nibble is the pin.
TEST(AsmReferenceAcceptedSpellings, TheSetccSpellingsCarryTheirConditions) {
    expectX86("sete %al", {0x40, 0x0F, 0x94, 0xC0});
    expectX86("setne %cl", {0x40, 0x0F, 0x95, 0xC1});
    expectX86("setl %dl", {0x40, 0x0F, 0x9C, 0xC2});
    expectX86("setg %sil", {0x40, 0x0F, 0x9F, 0xC6});
    expectX86("setb %r8b", {0x41, 0x0F, 0x92, 0xC0});
    expectX86("seta %al", {0x40, 0x0F, 0x97, 0xC0});
}

// ══ (F) the unsuffixed spellings — the width is the registers' ═══════════════
// gas and clang: mov %rax,%rcx = 48 89 c1 (here the 8B direction, 48 8b c8),
// mov %eax,%ecx = 89 c1 (8b c8), mov (%rdi),%cl = 8a 0f — the SUB-REGISTER
// load, never movzx (0f b6), which is why `load`'s movzx variants now state
// the width they write — add %eax,%ecx = 01 c1, xor = 31 c0, lea 2(%rdi),%eax
// = 8d 47 02, leaw = 66 8d 47 02.
TEST(AsmReferenceAcceptedSpellings, UnsuffixedSpellingsTakeTheirWidthFromTheNames) {
    expectX86("mov %rax, %rcx", {0x48, 0x8B, 0xC8});
    expectX86("mov %eax, %ecx", {0x8B, 0xC8});
    expectX86("mov (%rdi), %cl", {0x40, 0x8A, 0x8F, 0x00, 0x00, 0x00, 0x00});
    expectX86("mov (%rdi), %ax", {0x66, 0x8B, 0x87, 0x00, 0x00, 0x00, 0x00});
    expectX86("mov (%rdi,%rsi), %cl",
              {0x40, 0x8A, 0x8C, 0x37, 0x00, 0x00, 0x00, 0x00});
    expectX86("mov (%rdi), %rax", {0x48, 0x8B, 0x87, 0x00, 0x00, 0x00, 0x00});
    expectX86("mov %eax, (%rdi)", {0x89, 0x87, 0x00, 0x00, 0x00, 0x00});
    expectX86("add %eax, %ecx", {0x01, 0xC1});
    expectX86("xor %eax, %eax", {0x31, 0xC0});
    expectX86("cmp %eax, (%rdi)", {0x39, 0x87, 0x00, 0x00, 0x00, 0x00});
    expectX86("test %eax, %ecx", {0x85, 0xC1});
    expectX86("imul (%rdi), %eax", {0x0F, 0xAF, 0x87, 0x00, 0x00, 0x00, 0x00});
    expectX86("lea 2(%rdi), %eax", {0x8D, 0x87, 0x02, 0x00, 0x00, 0x00});
    expectX86("leal 2(%rdi), %eax", {0x8D, 0x87, 0x02, 0x00, 0x00, 0x00});
    expectX86("leaw 2(%rdi), %ax", {0x66, 0x8D, 0x87, 0x02, 0x00, 0x00, 0x00});
    expectX86("lea 2(%rdi), %ax", {0x66, 0x8D, 0x87, 0x02, 0x00, 0x00, 0x00});
}

// ══ (G) nothing states a width: gas's default, and gas's warning ═════════════
// gas: `mov $1, (%rax)` = c7 00 01 00 00 00 (32 bits) WITH a warning, and
// `not (%rax)` = f7 10; clang refuses both. The suffixed spelling states its
// width and draws no warning — the control.
TEST(AsmReferenceAcceptedSpellings, AnUnstatedWidthIsGasDefaultAndWarnsAsGasDoes) {
    auto const a = assembleBody(kX86, "  mov $1, (%rax)\n");
    ASSERT_TRUE(a.encoded) << a.diagnostics;
    Bytes const want{0xC7, 0x80, 0x00, 0x00, 0x00, 0x00,
                     0x01, 0x00, 0x00, 0x00, 0xC3};
    EXPECT_EQ(a.bytes, want) << hex(a.bytes);
    EXPECT_EQ(a.warnings, 1u) << a.diagnostics;
    EXPECT_NE(a.warningText.find("assembled at 32 bits"), std::string::npos)
        << a.warningText;

    auto const n = assembleBody(kX86, "  not (%rax)\n");
    ASSERT_TRUE(n.encoded) << n.diagnostics;
    EXPECT_EQ(n.bytes, (Bytes{0xF7, 0x90, 0x00, 0x00, 0x00, 0x00, 0xC3}))
        << hex(n.bytes);
    EXPECT_EQ(n.warnings, 1u) << n.diagnostics;

    auto const s = assembleBody(kX86, "  movl $1, (%rax)\n");
    ASSERT_TRUE(s.encoded) << s.diagnostics;
    EXPECT_EQ(s.bytes, want) << "the suffixed spelling is the same instruction";
    EXPECT_EQ(s.warnings, 0u) << "a stated width draws no warning: " << s.warningText;
}

// ══ (H) the dotted label as a branch target ══════════════════════════════════
// gas and clang: `jmp .L1` then `.L1:` = eb 00 (rel32 e9 here), `b .L1` =
// 0x14000001. And `.x0` is a SYMBOL — a name of several tokens is never a
// register, whatever its last token spells.
TEST(AsmReferenceAcceptedSpellings, ADottedLabelIsABranchTarget) {
    {
        auto const a = assembleBody(kX86, "  jmp .L1\n.L1:\n");
        ASSERT_TRUE(a.encoded) << a.diagnostics;
        EXPECT_EQ(a.bytes, (Bytes{0xE9, 0x00, 0x00, 0x00, 0x00, 0xC3}))
            << hex(a.bytes);
    }
    expectArm("  b .L1\n.L1:\n", {0x14000001u});
    expectArm("  b .x0\n.x0:\n", {0x14000001u});
}

// ══ (I) the aarch64 32-bit add/sub immediates ════════════════════════════════
// gas and clang, identical here word for word, the `wsp` readings included.
TEST(AsmReferenceAcceptedSpellings, TheWFormAddAndSubImmediates) {
    expectArm("  add w0, w1, #2\n", {0x11000820u});
    expectArm("  sub w0, w1, #4095\n", {0x513FFC20u});
    expectArm("  add wsp, w1, #16\n", {0x1100403Fu});
    expectArm("  add w0, wsp, #16\n", {0x110043E0u});
    expectArm("  sub wsp, wsp, #32\n", {0x510083FFu});
    expectArm("  add w0, w1, #0\n", {0x11000020u});
    // Past imm12: gas picks `add w0, w1, #1, lsl #12` (0x11400420); this target
    // declares the word pair its X form uses — same sum, each step wrapping at
    // 32 bits.
    expectArm("  add w0, w1, #4096\n", {0x11000020u, 0x11400400u});
    // The control: the X form is unchanged.
    expectArm("  add x0, x1, #2\n", {0x91000820u});
}

// ══ (K) an operand placeholder as a memory base or index ════════════════════
// The template surface only — a `.s` has no placeholders. gcc prints these
// operands as the registers they were given, so the bytes are the ones gas
// makes of `movq (%rdi,%rsi,8), %rax` (48 8b 04 f7 — here disp32, 48 8b 84 f7
// 00 00 00 00), `leal 2(%rdi), %eax` (8d 47 02 — here 8d 87 02 00 00 00) and
// `ldr x0, [x1, #8]` (0xF9400420, identical).
TEST(AsmReferenceAcceptedSpellings, AnOperandPlaceholderAddressesMemory) {
    {
        auto const a = assembleTemplate(
            kX86, "movq (%1,%2,8), %0",
            {{"%0", "rax", 64}, {"%1", "rdi", 64}, {"%2", "rsi", 64}});
        ASSERT_TRUE(a.encoded) << a.diagnostics;
        EXPECT_EQ(a.bytes, (Bytes{0x48, 0x8B, 0x84, 0xF7, 0x00, 0x00, 0x00, 0x00,
                                  0xC3}))
            << hex(a.bytes);
    }
    {
        auto const a = assembleTemplate(kX86, "leal 2(%1), %0",
                                        {{"%0", "rax", 32}, {"%1", "rdi", 64}});
        ASSERT_TRUE(a.encoded) << a.diagnostics;
        EXPECT_EQ(a.bytes, (Bytes{0x8D, 0x87, 0x02, 0x00, 0x00, 0x00, 0xC3}))
            << hex(a.bytes);
    }
    {
        auto const a = assembleTemplate(kArm, "ldr %0, [%1, #8]",
                                        {{"%0", "x0", 64}, {"%1", "x1", 64}});
        ASSERT_TRUE(a.encoded) << a.diagnostics;
        Bytes want = le32(0xF9400420u);
        auto const r = le32(kArmRet);
        want.insert(want.end(), r.begin(), r.end());
        EXPECT_EQ(a.bytes, want) << hex(a.bytes);
    }
}

// The refusal half: a placeholder that denotes no REGISTER — here an operand
// bound as a memory reference — cannot be an address register.
TEST(AsmReferenceAcceptedSpellings, APlaceholderThatIsNoRegisterIsRefusedAsAnAddress) {
    auto doc = shippedDialectDoc(kX86.language);
    auto grammar = GrammarSchema::loadFromText(doc.dump(), std::string{kX86.language});
    ASSERT_TRUE(grammar.has_value());
    auto target = TargetSchema::loadShipped(kX86.target);
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto tree = parseAsmTemplateText("movq (%1), %0", "<template>", *grammar,
                                     AsmTemplateSurface::Extended,
                                     DiagnosticBudget::libraryDefault(), rep);
    ASSERT_TRUE(tree.has_value());
    LirBuilder builder{**target};
    builder.addFunction(SymbolId{1});
    builder.beginBlock(builder.createBlock());
    std::vector<AsmOperandBinding> bindings(2);
    bindings[0].spelling  = "%0";
    bindings[0].regClass  = LirRegClass::GPR;
    bindings[0].widthBits = 64;
    bindings[0].reg = makePhysicalReg(*(*target)->registerByName("rax"), LirRegClass::GPR);
    bindings[1].spelling    = "%1";
    bindings[1].regClass    = LirRegClass::GPR;
    bindings[1].widthBits   = 64;
    bindings[1].operandKind = OperandKindFilter::MemBase;
    bindings[1].reg = makePhysicalReg(*(*target)->registerByName("rdi"), LirRegClass::GPR);
    bool const ok = lowerAsmTemplateToLirRun(*tree, **grammar, **target, bindings,
                                             builder, rep);
    EXPECT_FALSE(ok);
    bool named = false;
    for (auto const& d : rep.all()) {
        if (d.actual.find("must denote a REGISTER holding an address")
            != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named) << "the refusal must say why";
}

// ══ (J) cbz / cbnz ═══════════════════════════════════════════════════════════
// gas and clang: `cbz x5, +16` = 0xB4000085, `cbnz w7, +12` = 0x35000067. The
// pin reads the register field, the width bit and that the imm19 lands on the
// `ret` — the layout of the fallthrough blocks is this target's, and the
// arithmetic, not an offset constant, is what is pinned.
TEST(AsmReferenceAcceptedSpellings, CbzAndCbnzTestTheRegisterTheyName) {
    for (auto const& [insn, base, rt] :
         {std::tuple{"cbz x5, .L1", 0xB4000000u, 5u},
          std::tuple{"cbnz w7, .L1", 0x35000000u, 7u},
          std::tuple{"cbz w0, .L1", 0x34000000u, 0u},
          std::tuple{"cbnz x3, .L1", 0xB5000000u, 3u}}) {
        SCOPED_TRACE(insn);
        auto const a = assembleBody(kArm, std::string{"  "} + insn + "\n.L1:\n");
        ASSERT_TRUE(a.encoded) << a.diagnostics;
        ASSERT_GE(a.bytes.size(), 8u);
        std::uint32_t const w0 = a.bytes[0] | (a.bytes[1] << 8)
                               | (a.bytes[2] << 16)
                               | (static_cast<std::uint32_t>(a.bytes[3]) << 24);
        EXPECT_EQ(w0 & 0xFF00001Fu, base | rt) << std::format("{:08x}", w0);
        // imm19 (bits 5..23, x4) from word 0 must land on the `ret` word.
        std::int32_t imm19 = static_cast<std::int32_t>((w0 >> 5) & 0x7FFFF);
        if (imm19 & 0x40000) imm19 -= 0x80000;
        std::size_t retAt = a.bytes.size();
        for (std::size_t i = 0; i + 4 <= a.bytes.size(); i += 4) {
            std::uint32_t const w = a.bytes[i] | (a.bytes[i + 1] << 8)
                                  | (a.bytes[i + 2] << 16)
                                  | (static_cast<std::uint32_t>(a.bytes[i + 3]) << 24);
            if (w == kArmRet) { retAt = i; break; }
        }
        ASSERT_LT(retAt, a.bytes.size()) << "no ret";
        EXPECT_EQ(static_cast<std::int64_t>(imm19) * 4,
                  static_cast<std::int64_t>(retAt))
            << "the taken edge must reach `.L1:`";
    }
}

// ══ (L) ONE ELEMENT of a vector register ═════════════════════════════════════
// gas and clang, identical word for word (✔MEASURED 2026-09-21, `fo4/lane`):
// UMOV reads one element into a general register (W for b/h/s, X for d), INS
// writes one from a general register, and `mov` is the alias of each (UMOV only
// for s/d). The element's size and index share the `imm5` field.
TEST(AsmReferenceAcceptedSpellings, AnElementIsASizeAndAnIndex) {
    expectArm("  umov w0, v1.b[3]\n", {0x0E073C20u});
    expectArm("  umov w0, v1.h[2]\n", {0x0E0A3C20u});
    expectArm("  umov w0, v1.s[1]\n", {0x0E0C3C20u});
    expectArm("  umov x0, v1.d[1]\n", {0x4E183C20u});
    expectArm("  umov w0, v1.b[15]\n", {0x0E1F3C20u});
    expectArm("  umov x5, v30.d[1]\n", {0x4E183FC5u});
    expectArm("  mov w3, v17.s[2]\n", {0x0E143E23u});
    expectArm("  mov x0, v1.d[1]\n", {0x4E183C20u});
    expectArm("  ins v0.b[3], w1\n", {0x4E071C20u});
    expectArm("  ins v0.h[2], w1\n", {0x4E0A1C20u});
    expectArm("  ins v0.s[1], w1\n", {0x4E0C1C20u});
    expectArm("  ins v0.d[1], x1\n", {0x4E181C20u});
    expectArm("  mov v0.b[3], w1\n", {0x4E071C20u});
    expectArm("  mov v31.d[0], x30\n", {0x4E081FDFu});
    // Both references also take an upper-case spelling and a hex index.
    expectArm("  umov w1, V2.S[1]\n", {0x0E0C3C41u});
    expectArm("  umov w0, v1.b[0x3]\n", {0x0E073C20u});
}

// GNU as ALONE reads an arrangement before an index as the element's lane
// width, ignoring its count (clang 18.1.3 refuses every one of these). One
// working reference makes the form required, with gas's words.
TEST(AsmReferenceAcceptedSpellings, GasReadsAnArrangementBeforeAnIndexAsTheElementSize) {
    expectArm("  umov w0, v1.16b[3]\n", {0x0E073C20u});
    expectArm("  umov w0, v1.8b[15]\n", {0x0E1F3C20u});
    expectArm("  umov x0, v1.2d[1]\n", {0x4E183C20u});
    expectArm("  ins v0.2d[1], x1\n", {0x4E181C20u});
}

// The controls — every spelling here is refused by gas AND clang (✔MEASURED):
// a `mov` alias for a byte or halfword element, a destination width the size
// does not have, an index past the field, an element on a non-`v` name, a size
// that is not one, and an element where no instruction reads one.
TEST(AsmReferenceAcceptedSpellings, AnElementBothReferencesRefuseIsRefused) {
    for (auto const* insn :
         {"mov w0, v1.b[1]", "mov w0, v1.h[1]", "umov x0, v1.s[1]",
          "umov w0, v1.d[1]", "umov w0, v1.b[16]", "umov w0, v1.1d[0]",
          "ins v0.d[1], w1", "ins v0.s[1], x1", "ins v0.b[16], w1",
          "umov x0, q1.d[1]", "umov x0, d1.d[0]", "umov w0, s1.s[0]",
          "umov x0, x1.d[0]", "ins q0.d[1], x1", "umov x0, v1.x[1]",
          "umov x0, v1.d", "umov v0.d[1], x1", "add x0, x1, v1.d[1]"}) {
        expectRefused(kArm, insn, "gas and clang both refuse it");
    }
}

// The TEMPLATE surface: `%1.d[1]` is one element of the register the operand
// is bound to, and `%0.d[1]` one element of a destination whose other lanes
// `ins` keeps. gcc and clang print such an operand as its `v` register
// (`mov x9, v0.d[1]`, ✔MEASURED in clang's -S), so the words are those above.
TEST(AsmReferenceAcceptedSpellings, ATemplateElementIsALaneOfTheBoundRegister) {
    auto const expectTemplate = [](std::string_view text, std::vector<Bound> b,
                                   std::uint32_t word) {
        SCOPED_TRACE(std::string{text});
        auto const a = assembleTemplate(kArm, text, b);
        ASSERT_TRUE(a.encoded) << a.diagnostics;
        Bytes want = le32(word);
        auto const r = le32(kArmRet);
        want.insert(want.end(), r.begin(), r.end());
        EXPECT_EQ(a.bytes, want) << hex(a.bytes);
    };
    expectTemplate("umov %0, %1.d[1]",
                   {{"%0", "x0", 64}, {"%1", "v1", 128, LirRegClass::FPR}},
                   0x4E183C20u);
    expectTemplate("mov %w0, %1.s[3]",
                   {{"%0", "x0", 64}, {"%1", "v1", 128, LirRegClass::FPR}},
                   0x0E1C3C20u);
    expectTemplate("ins %0.d[1], %1",
                   {{"%0", "v0", 128, LirRegClass::FPR}, {"%1", "x1", 64}},
                   0x4E181C20u);
    // gas's arrangement-before-index form through a template (gcc prints
    // `v1.16b[15]`; a binary128 probe runs to 42 under gcc, clang refuses).
    expectTemplate("umov %w0, %1.16b[15]",
                   {{"%0", "x0", 64}, {"%1", "v1", 128, LirRegClass::FPR}},
                   0x0E1F3C20u);
    // Refused: an element of an operand bound to a GENERAL register (gcc
    // prints `x0.d[1]`, which both assemblers refuse), a width-view letter AND
    // an element, and the AT&T dialect, which declares no element sizes.
    for (auto const& [text, bound] :
         {std::pair{std::string_view{"umov %0, %1.d[1]"},
                    std::vector<Bound>{{"%0", "x0", 64}, {"%1", "x1", 64}}},
          std::pair{std::string_view{"umov %0, %q1.d[1]"},
                    std::vector<Bound>{{"%0", "x0", 64},
                                       {"%1", "v1", 128, LirRegClass::FPR}}}}) {
        SCOPED_TRACE(std::string{text});
        EXPECT_FALSE(assembleTemplate(kArm, text, bound).encoded);
    }
    EXPECT_FALSE(assembleTemplate(kX86, "movq %1.d[1], %0",
                                  {{"%0", "rax", 64},
                                   {"%1", "xmm1", 128, LirRegClass::FPR}})
                     .encoded);
}

// The load contracts of the element vocabulary: each half without the other is
// refused, on both sides of the dialect/target seam.
TEST(AsmReferenceAcceptedSpellings, TheElementVocabularyIsDeclaredWhole) {
    auto const onUmovB = [](auto&& edit) {
        return [edit](nlohmann::json& doc) {
            for (auto& op : doc.at("opcodes")) {
                if (op.value("mnemonic", std::string{}) == "umov_b") {
                    edit(op.at("encoding").at("variants").at(0).at("wires"));
                }
            }
        };
    };
    // An index field with no element field before it.
    auto const noElement = dss::test_support::mutateShippedTargetSchemaDoc(
        "arm64", onUmovB([](nlohmann::json& wires) {
            wires.at(0).erase("elementBits");
        }));
    // An element field with no index field after it.
    auto const noIndex = dss::test_support::mutateShippedTargetSchemaDoc(
        "arm64", onUmovB([](nlohmann::json& wires) {
            wires.at(1)["slotKind"] = "imm16";
        }));
    // An element width that is not one.
    auto const badWidth = dss::test_support::mutateShippedTargetSchemaDoc(
        "arm64", onUmovB([](nlohmann::json& wires) {
            wires.at(0)["elementBits"] = 12;
        }));
    auto const refusedNaming = [](auto const& r, std::string_view word) {
        if (r.has_value()) return false;
        for (auto const& e : r.error()) {
            if (e.message.find(word) != std::string::npos) return true;
        }
        return false;
    };
    EXPECT_TRUE(refusedNaming(noElement, "imm5.element"))
        << "an index field with no element field before it";
    EXPECT_TRUE(refusedNaming(noIndex, "imm5.element"))
        << "an element field with no index field after it";
    EXPECT_TRUE(refusedNaming(badWidth, "elementBits"))
        << "an element width that is not 8, 16, 32 or 64";

    // "" = loaded clean; otherwise every load message, joined.
    auto const loadMessages = [](auto&& edit) {
        auto doc = shippedDialectDoc("asm-arm64-gas");
        edit(doc.at("assembly"));
        auto const r = GrammarSchema::loadFromText(doc.dump(), "asm-arm64-gas");
        std::string out;
        if (!r.has_value()) {
            for (auto const& e : r.error()) out += e.message + "\n";
            if (out.empty()) out = "(refused, no message)";
        }
        return out;
    };
    EXPECT_EQ(loadMessages([](nlohmann::json&) {}), "") << "the control must load";
    EXPECT_NE(loadMessages([](nlohmann::json& a) { a.erase("registerElements"); })
                  .find("registerElements"),
              std::string::npos)
        << "an index rule with no element sizes";
    EXPECT_NE(loadMessages([](nlohmann::json& a) { a.erase("elementIndexRule"); })
                  .find("elementIndexRule"),
              std::string::npos)
        << "element sizes with no index rule";
    EXPECT_NE(loadMessages([](nlohmann::json& a) {
                  a.at("registerElements").at(0)["suffix"] = ".16b";
              }).find("also a lane arrangement"),
              std::string::npos)
        << "an element size that is also an arrangement";
    EXPECT_NE(loadMessages([](nlohmann::json& a) {
                  a.at("registerElements").at(0)["laneBits"] = 128;
              }).find("laneBits"),
              std::string::npos)
        << "an element width outside 8/16/32/64";
}
