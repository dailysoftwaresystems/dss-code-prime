// ★★★ GNU as's NUMERIC LOCAL LABELS, THE LABELS A TEMPLATE DEFINES, AND THE
// TEMPLATE TEXT FORMS — through the SHIPPED dialects and targets (P68 round 8
// part 4: D-ASM-LABELS-INSIDE-A-TEMPLATE-AND-NUMERIC-LOCAL-LABELS-REFUSED and
// D-ASM-TEMPLATE-FORMS-A-REFERENCE-EXPANDS-REFUSED).
//
// The RESOLUTION rule (nearest before / nearest after) is pinned once, on the
// resolver itself, in `test_asm_local_labels.cpp`; the runnable corpus examples
// (`examples/asm/asm_*_numeric_local_labels`, `examples/c/asm_template_labels_and_forms`)
// prove each resolution by its exit code. THIS file pins what those two cannot
// see: the refusals — each named, each with the parse proven clean first so a
// refusal cannot be green for the wrong reason — the shipped declarations the
// behaviour rests on, and the loader's refusal of every half-declaration.

#include "asm/asm.hpp"
#include "asm/asm_template_to_lir.hpp"
#include "asm_text_fixture.hpp"
#include "core/types/asm_template_text_forms.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_reg.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

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

struct Dialect {
    std::string_view language;
    std::string_view target;
    std::string_view functionMarker;
};
constexpr Dialect kX86{"asm-x86_64-att", "x86_64", "@function"};
constexpr Dialect kArm{"asm-arm64-gas", "arm64", "%function"};

struct Outcome {
    bool        parsed  = false;
    bool        lowered = false;
    std::string diagnostics;
};

// `main:` + the body + a return, through the shipped dialect and target.
[[nodiscard]] Outcome lowerBody(Dialect const& d, std::string const& body) {
    Outcome out;
    std::string const src = std::string{".globl main\n.type main, "}
                          + std::string{d.functionMarker} + "\nmain:\n" + body
                          + "  ret\n";
    auto const run = lowerAsmText(shippedDialectDoc(d.language), src, d.target);
    out.parsed      = parsedCleanly(*run);
    out.diagnostics = parseMessages(*run) + messages(*run);
    out.lowered     = run->module.has_value();
    return out;
}

// One template through the shipped dialect, every operand bound to a physical
// register, lowered into a function (no encoding: the refusals under test all
// happen at lowering).
[[nodiscard]] Outcome lowerTemplate(Dialect const& d, std::string_view text,
                                    std::vector<std::pair<std::string, std::string>> const& bound = {}) {
    Outcome out;
    auto grammar = GrammarSchema::loadFromText(shippedDialectDoc(d.language).dump(),
                                               std::string{d.language});
    auto target = TargetSchema::loadShipped(d.target);
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
    return out;
}

// "" = the dialect loaded clean; otherwise every load message, joined.
template <class Edit>
[[nodiscard]] std::string dialectLoadMessages(std::string_view language, Edit&& edit) {
    auto doc = shippedDialectDoc(language);
    edit(doc);
    auto const r = GrammarSchema::loadFromText(doc.dump(), std::string{language});
    std::string out;
    if (!r.has_value()) {
        for (auto const& e : r.error()) out += e.path + ": " + e.message + "\n";
        if (out.empty()) out = "(refused, no message)";
    }
    return out;
}

}  // namespace

// ══ `.s`: a reference with no definition on its side is refused by name ══════
// ✔MEASURED (gas 2.42 and clang 18.1.3, x86_64 and aarch64): the SAME shapes
// resolve and run when the definition is on the named side — the examples.
TEST(AsmNumericLabels, AReferenceWithNothingOnItsSideIsRefusedByName) {
    for (Dialect const* d : {&kX86, &kArm}) {
        SCOPED_TRACE(d->language);
        std::string const back = d == &kX86 ? "  jmp 1b\n1:\n" : "  b 1b\n1:\n";
        auto const b = lowerBody(*d, back);
        ASSERT_TRUE(b.parsed) << b.diagnostics;
        EXPECT_FALSE(b.lowered) << "`1b` with the only `1:` AFTER it";
        EXPECT_NE(b.diagnostics.find("defines no `1:` before it"), std::string::npos)
            << b.diagnostics;

        std::string const fwd = d == &kX86 ? "1:\n  jmp 1f\n" : "1:\n  b 1f\n";
        auto const f = lowerBody(*d, fwd);
        ASSERT_TRUE(f.parsed) << f.diagnostics;
        EXPECT_FALSE(f.lowered) << "`1f` with the only `1:` BEFORE it";
        EXPECT_NE(f.diagnostics.find("defines no `1:` after it"), std::string::npos)
            << f.diagnostics;

        // CONTROL: the same two lines, each reference pointing the right way.
        std::string const ok = d == &kX86 ? "1:\n  jmp 2f\n2:\n  jmp 3f\n3:\n"
                                          : "1:\n  b 2f\n2:\n  b 3f\n3:\n";
        auto const c = lowerBody(*d, ok);
        EXPECT_TRUE(c.lowered) << c.diagnostics;
    }
}

// A CALL to an unresolved local reference is a typo in THIS file — it is
// refused, never minted as an import named `1f` that no object could define.
TEST(AsmNumericLabels, AnUnresolvedLocalCallIsRefusedNotImported) {
    auto const r = lowerBody(kX86, "  call 1f\n");
    ASSERT_TRUE(r.parsed) << r.diagnostics;
    EXPECT_FALSE(r.lowered);
    EXPECT_NE(r.diagnostics.find("local label `1f`"), std::string::npos) << r.diagnostics;
}

// `0b` and `0f` are label 0; `0b101` stays the binary 5. The number grammar
// tries its prefixes first, so the suffix only applies where no prefix digit
// follows — which is how gas reads them.
TEST(AsmNumericLabels, ZeroBAndZeroFAreLabelZeroBesideABinaryLiteral) {
    auto const r = lowerBody(kX86,
                             "  jmp 0f\n"
                             "0:\n"
                             "  addl $0b101, %eax\n"
                             "  cmpl $5, %eax\n"
                             "  jl 0b\n");
    ASSERT_TRUE(r.parsed) << r.diagnostics;
    EXPECT_TRUE(r.lowered) << r.diagnostics;
}

// A numeric label is an ordinary label to every later pass, and it may head a
// chain after a NAMED label on the same line — the scan used to drop it there.
TEST(AsmNumericLabels, ANumericLabelAfterANamedOneOnOneLineIsCollected) {
    auto const r = lowerBody(kArm,
                             "a: 1:\n"
                             "  sub w0, w0, #1\n"
                             "  cbnz w0, 1b\n");
    ASSERT_TRUE(r.parsed) << r.diagnostics;
    EXPECT_TRUE(r.lowered) << r.diagnostics;
}

// ══ TEMPLATE labels ════════════════════════════════════════════════════════
// A jump OUT of the statement is refused, citing the GCC manual's rule.
TEST(AsmTemplateLabels, AJumpOutOfTheStatementIsRefusedCitingGcc) {
    auto const out = lowerTemplate(kArm, "b elsewhere");
    ASSERT_TRUE(out.parsed) << out.diagnostics;
    EXPECT_FALSE(out.lowered);
    EXPECT_NE(out.diagnostics.find("may not perform jumps into other asm statements"),
              std::string::npos)
        << out.diagnostics;
    // CONTROL: the same branch to a label the template itself defines.
    auto const own = lowerTemplate(kArm, "b elsewhere\nelsewhere:\nnop");
    EXPECT_TRUE(own.lowered) << own.diagnostics;
}

TEST(AsmTemplateLabels, ANumericReferenceResolvesOnlyWithinTheTemplate) {
    auto const out = lowerTemplate(kX86, "jmp 1f");
    ASSERT_TRUE(out.parsed) << out.diagnostics;
    EXPECT_FALSE(out.lowered);
    EXPECT_NE(out.diagnostics.find("within the template"), std::string::npos)
        << out.diagnostics;
    auto const loop = lowerTemplate(kX86, "1:\nsubq $1, %0\njnz 1b", {{"%0", "rcx"}});
    EXPECT_TRUE(loop.lowered) << loop.diagnostics;
}

// A NAMED label defined twice is refused (gas refuses a symbol defined twice);
// a NUMERIC one may repeat — that is what it is for.
TEST(AsmTemplateLabels, ANamedLabelTwiceIsRefusedANumericOneRepeats) {
    auto const twice = lowerTemplate(kX86, "a:\nnop\na:\nnop");
    ASSERT_TRUE(twice.parsed) << twice.diagnostics;
    EXPECT_FALSE(twice.lowered);
    EXPECT_NE(twice.diagnostics.find("more than once"), std::string::npos)
        << twice.diagnostics;
    auto const numeric = lowerTemplate(kX86, "1:\nnop\n1:\njmp 1b");
    EXPECT_TRUE(numeric.lowered) << numeric.diagnostics;
}

// The ADDRESS of a template's own label is still refused, and the refusal says
// WHY — it used to claim a template "has no labels of its own".
TEST(AsmTemplateLabels, TheAddressOfATemplateLabelIsRefusedTruthfully) {
    auto const out = lowerTemplate(kArm, "adr %0, 1f\n1:\nnop", {{"%0", "x9"}});
    ASSERT_TRUE(out.parsed) << out.diagnostics;
    EXPECT_FALSE(out.lowered);
    EXPECT_NE(out.diagnostics.find("block of this statement's own body"),
              std::string::npos)
        << out.diagnostics;
    EXPECT_EQ(out.diagnostics.find("has no labels of its own"), std::string::npos)
        << out.diagnostics;
}

// ══ the TEMPLATE TEXT FORMS, as SHIPPED ═════════════════════════════════════
// The expansion is pinned against the measured table in
// `tests/core/test_asm_template_text_forms.cpp` with the forms written as test
// data. THIS pin is the other half: the SHIPPED dialects declare exactly the
// measured rows, and the SHIPPED x86_64 target answers `%~`'s feature question.
// RED-ON-DISABLE: drop a row from a dialect's `templateTextForms`, or
// `isaFeatures` from the target, and a row below reddens.
TEST(AsmTemplateTextFormsShipped, EachDialectDeclaresItsMeasuredForms) {
    auto x86 = GrammarSchema::loadShipped("asm-x86_64-att");
    auto arm = GrammarSchema::loadShipped("asm-arm64-gas");
    auto x86t = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(x86.has_value() && arm.has_value() && x86t.has_value());
    auto const& xf = (*x86)->assembly().templateTextForms;
    auto const& af = (*arm)->assembly().templateTextForms;

    AsmTemplateTextFormContext ctx;
    ctx.placeholder      = "%";
    ctx.escape           = "%%";
    ctx.symbolicNameOpen = "[";
    ctx.instance         = 7;
    ctx.targetHasFeature = [&](std::string_view f) { return (*x86t)->isaFeature(f); };

    auto const x = expandAsmTemplateTextForms(
        "%=|%{%}%|%*|%;%+%^%!|%~|{att|intel}", xf, ctx);
    ASSERT_TRUE(x.has_value()) << x.error();
    EXPECT_EQ(*x, "7|{}|*||f|att")
        << "x86: %= %{ %} %| %* expand; %; %+ %^ %! print nothing; %~ is `f` on a "
           "target without avx2; the AT&T arm is the first";

    auto const a = expandAsmTemplateTextForms("%=|%{%}|{att|intel}", af, ctx);
    ASSERT_TRUE(a.has_value()) << a.error();
    EXPECT_EQ(*a, "7|{}|{att|intel}") << "aarch64: braces are literal text";
    for (std::string_view form : {"%*", "%|", "%~", "%;"}) {
        EXPECT_FALSE(expandAsmTemplateTextForms(form, af, ctx).has_value())
            << "aarch64 refuses " << form << ", as both aarch64 ports do";
    }

    ASSERT_EQ((*x86t)->isaFeature("avx2"), std::optional<bool>{false})
        << "the shipped x86_64 target assumes no AVX2";
    EXPECT_EQ((*x86t)->isaFeature("no-such-feature"), std::nullopt)
        << "an undeclared feature is UNKNOWN, never off";
}

// ══ the LOADER refuses every half-declaration ═══════════════════════════════
TEST(AsmNumericLabelsLoader, ThePairAndItsSuffixesAreDeclaredWholeOrRefused) {
    EXPECT_EQ(dialectLoadMessages("asm-x86_64-att", [](nlohmann::json&) {}), "")
        << "the control must load";
    EXPECT_NE(dialectLoadMessages("asm-x86_64-att", [](nlohmann::json& d) {
                  d.at("assembly").erase("localLabelSuffixes");
              }).find("declared together or not at all"),
              std::string::npos)
        << "a definition rule with no reference suffixes";
    EXPECT_NE(dialectLoadMessages("asm-x86_64-att", [](nlohmann::json& d) {
                  d.at("assembly").erase("numericLabelRule");
              }).find("declared together or not at all"),
              std::string::npos)
        << "reference suffixes with no definition rule";
    EXPECT_NE(dialectLoadMessages("asm-x86_64-att", [](nlohmann::json& d) {
                  d.at("numberStyle").at("integerSuffixes") = nlohmann::json::array({"f"});
              }).find("does not declare"),
              std::string::npos)
        << "a suffix the number grammar does not lex as part of the number";
    EXPECT_NE(dialectLoadMessages("asm-x86_64-att", [](nlohmann::json& d) {
                  d.at("assembly").at("localLabelSuffixes")["forward"] = "b";
              }).find("never say which way"),
              std::string::npos)
        << "one suffix for both directions";
}

TEST(AsmTemplateTextFormsLoader, AFormThatCouldNeverBeReachedIsRefused) {
    auto const withForms = [](auto&& edit) {
        return dialectLoadMessages("asm-x86_64-att", [&](nlohmann::json& d) {
            edit(d.at("assembly").at("templateTextForms"));
        });
    };
    EXPECT_NE(withForms([](nlohmann::json& f) {
                  f.at("fixed").push_back({{"code", "l"}, {"text", "x"}});
              }).find("must open with ASCII punctuation"),
              std::string::npos)
        << "a letter after the sigil is an operand reference";
    EXPECT_NE(withForms([](nlohmann::json& f) {
                  f.at("fixed").push_back({{"code", "%x"}, {"text", "x"}});
              }).find("must open with ASCII punctuation"),
              std::string::npos)
        << "a form beginning with the escape is unreachable";
    EXPECT_NE(withForms([](nlohmann::json& f) {
                  f.at("fixed").push_back({{"code", "{"}, {"text", "x"}});
              }).find("must open with ASCII punctuation"),
              std::string::npos)
        << "a form declared twice";
    EXPECT_NE(withForms([](nlohmann::json& f) {
                  f.at("fixed").push_back({{"form", "%?"}, {"text", "x"}});
              }).find("unknown key 'form'"),
              std::string::npos)
        << "the retired whole-form key is refused, never read";
    EXPECT_NE(withForms([](nlohmann::json& f) {
                  f.at("alternatives")["open"] = "%(";
              }).find("begins with the placeholder"),
              std::string::npos)
        << "an alternative delimiter the expansion reads as a form";
    EXPECT_NE(dialectLoadMessages("asm-x86_64-att", [](nlohmann::json& d) {
                  auto& a = d.at("assembly");
                  for (char const* k : {"templateLexerMode", "templateOperandRule",
                                        "templateLabelRule", "templateModifierRule",
                                        "templateModifiers"}) {
                      a.erase(k);
                  }
              }).find("declares no template surface"),
              std::string::npos)
        << "text forms on a dialect that lowers no template";
}
