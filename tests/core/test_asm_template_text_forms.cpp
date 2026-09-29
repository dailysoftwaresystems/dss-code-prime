// ★★★ THE TEMPLATE TEXT FORMS A REFERENCE EXPANDS BEFORE ITS ASSEMBLER SEES THE
// TEXT — `expandAsmTemplateTextForms` (core/types/asm_template_text_forms.hpp),
// pinned against the MEASURED outputs (P68 round 8,
// D-ASM-TEMPLATE-FORMS-A-REFERENCE-EXPANDS-REFUSED).
//
// ✔MEASURED 2026-09-23, gcc 13.3.0 and clang 18.1.3, each form inside
// `.ascii "X<form>Y"` so the assembler accepts the line and only the form is
// under test. The x86 rows below are the union of gcc's and clang's x86 ports;
// the aarch64 rows the union of both aarch64 ports. The forms are declared as
// DATA here exactly as the dialect documents declare them — the function under
// test names none of them.

#include "core/types/asm_template_text_forms.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

using namespace dss;

namespace {

[[nodiscard]] AsmTemplateTextForms x86Forms() {
    AsmTemplateTextForms f;
    f.instanceNumber = "%=";
    f.fixed = {{"%{", "{"}, {"%}", "}"}, {"%|", "|"}, {"%*", "*"},
               {"%;", ""},  {"%+", ""},  {"%^", ""},  {"%!", ""}};
    f.featureSelected = {{"%~", "avx2", "i", "f"}};
    f.alternatives = {.declared = true, .open = "{", .separator = "|",
                      .close = "}", .select = 0};
    return f;
}

[[nodiscard]] AsmTemplateTextForms a64Forms() {
    AsmTemplateTextForms f;
    f.instanceNumber = "%=";
    f.fixed = {{"%{", "{"}, {"%}", "}"}};
    return f;
}

[[nodiscard]] AsmTemplateTextFormContext ctx(std::uint64_t instance,
                                             std::optional<bool> avx2 = false) {
    AsmTemplateTextFormContext c;
    c.placeholder      = "%";
    c.escape           = "%%";
    c.symbolicNameOpen = "[";
    c.instance         = instance;
    c.targetHasFeature = [avx2](std::string_view f) -> std::optional<bool> {
        if (f == "avx2") return avx2;
        return std::nullopt;
    };
    return c;
}

[[nodiscard]] std::string ok(std::string_view text, AsmTemplateTextForms const& f,
                             AsmTemplateTextFormContext const& c) {
    auto r = expandAsmTemplateTextForms(text, f, c);
    EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error());
    return r.has_value() ? *r : std::string{};
}

}  // namespace

TEST(AsmTemplateTextForms, TheInstanceNumberIsOneNumberPerInstanceEverywhereInIt) {
    EXPECT_EQ(ok(".Lu%=:", a64Forms(), ctx(14)), ".Lu14:");
    EXPECT_EQ(ok("X%=%=Y", x86Forms(), ctx(3)), "X33Y")
        << "measured: `%=%=` is the SAME number twice (gcc `1414`, clang `00`)";
}

TEST(AsmTemplateTextForms, TheX86FormsExpandAsGccsX86PortDoes) {
    EXPECT_EQ(ok("X%{Y", x86Forms(), ctx(0)), "X{Y");
    EXPECT_EQ(ok("X%}Y", x86Forms(), ctx(0)), "X}Y");
    EXPECT_EQ(ok("X%|Y", x86Forms(), ctx(0)), "X|Y");
    EXPECT_EQ(ok("X%*Y", x86Forms(), ctx(0)), "X*Y");
    for (std::string_view const form : {"%;", "%+", "%^", "%!"}) {
        SCOPED_TRACE(form);
        EXPECT_EQ(ok(std::string{"X"} + std::string{form} + "Y", x86Forms(), ctx(0)),
                  "XY")
            << "measured: gcc's x86 port prints NOTHING for these";
    }
    EXPECT_EQ(ok("X%~Y", x86Forms(), ctx(0, false)), "XfY") << "no AVX2: `f`";
    EXPECT_EQ(ok("X%~Y", x86Forms(), ctx(0, true)), "XiY") << "`-mavx2`: `i`";
}

TEST(AsmTemplateTextForms, X86AlternativesSelectTheAttArm) {
    EXPECT_EQ(ok("X{att|intel}Y", x86Forms(), ctx(0)), "XattY");
    EXPECT_EQ(ok("X{a|b|c}Y", x86Forms(), ctx(0)), "XaY");
    EXPECT_EQ(ok("X{x}Y", x86Forms(), ctx(0)), "XxY") << "a lone arm is the text";
    EXPECT_EQ(ok("X{}Y", x86Forms(), ctx(0)), "XY");
    EXPECT_EQ(ok("X{|b}Y", x86Forms(), ctx(0)), "XY") << "the AT&T arm is empty";
    EXPECT_EQ(ok("X{%=|z}Y", x86Forms(), ctx(7)), "X7Y")
        << "a form inside the selected arm is expanded";
    EXPECT_EQ(ok("X{a%|b|c}Y", x86Forms(), ctx(0)), "Xa|bY")
        << "measured gcc: `%|` is a literal bar INSIDE an arm";
    EXPECT_EQ(ok("Xa}bY", x86Forms(), ctx(0)), "Xa}bY") << "a stray close is text";
    EXPECT_EQ(ok("Xa|bY", x86Forms(), ctx(0)), "Xa|bY") << "a bare bar is text";
}

// ✔MEASURED 2026-09-23 (gcc 13.3.0, clang 18.1.3, x86_64 -S): gcc never READS
// a dropped alternative — a placeholder and the byte after it pass as one unit
// — so `{a|%&}`, `{a|%@}` and `{a|%~}` give `a` under gcc while clang refuses
// all three ("invalid % escape"); the union accepts. And `{a|b%}c}` gives `a`
// under BOTH: the escaped close does not end the group.
TEST(AsmTemplateTextForms, ADroppedAlternativeIsSkippedNotRead) {
    EXPECT_EQ(ok("X{a|%&}Y", x86Forms(), ctx(0)), "XaY");
    EXPECT_EQ(ok("X{a|%@}Y", x86Forms(), ctx(0)), "XaY");
    EXPECT_EQ(ok("X{a|%~}Y", x86Forms(), ctx(0, std::nullopt)), "XaY")
        << "a feature form in a dropped arm asks the target nothing";
    EXPECT_EQ(ok("X{a|b%}c}Y", x86Forms(), ctx(0)), "XaY");
    // CONTROL: the same `%&` in the SELECTED arm is read, and refused.
    auto const selected = expandAsmTemplateTextForms("X{%&|a}Y", x86Forms(), ctx(0));
    ASSERT_FALSE(selected.has_value()) << "`%&` in the selected arm is read";
}

TEST(AsmTemplateTextForms, X86RefusesWhatBothX86ReferencesRefuse) {
    auto const nested = expandAsmTemplateTextForms("X{a{b}c|d}Y", x86Forms(), ctx(0));
    ASSERT_FALSE(nested.has_value());
    EXPECT_NE(nested.error().find("nested"), std::string::npos) << nested.error();
    auto const unterminated = expandAsmTemplateTextForms("X{a|bY", x86Forms(), ctx(0));
    ASSERT_FALSE(unterminated.has_value());
    EXPECT_NE(unterminated.error().find("never closes"), std::string::npos);
}

TEST(AsmTemplateTextForms, OnAarch64BracesAreTextAndOnlyTheBraceEscapesExist) {
    EXPECT_EQ(ok("X{att|intel}Y", a64Forms(), ctx(0)), "X{att|intel}Y");
    EXPECT_EQ(ok("X%{Y", a64Forms(), ctx(0)), "X{Y") << "clang aarch64 expands it";
    auto const star = expandAsmTemplateTextForms("X%*Y", a64Forms(), ctx(0));
    ASSERT_FALSE(star.has_value()) << "both aarch64 ports refuse `%*`";
    EXPECT_NE(star.error().find("`%*`"), std::string::npos) << star.error();
}

TEST(AsmTemplateTextForms, ReferencesAndTheEscapeAreLeftForTheLexer) {
    EXPECT_EQ(ok("add %0, %1, %[out], %l2, %w3, %%", a64Forms(), ctx(0)),
              "add %0, %1, %[out], %l2, %w3, %%");
    EXPECT_EQ(ok("X%%=Y", x86Forms(), ctx(9)), "X%%=Y")
        << "`%%` is the escape: the `=` after it is text, not an instance number";
    EXPECT_EQ(ok("trailing %", x86Forms(), ctx(0)), "trailing %")
        << "a trailing bare placeholder is the scan's and the lexer's to judge";
}

TEST(AsmTemplateTextForms, AFormNoOneDeclaresIsRefusedByNameWithTheInventory) {
    auto const amp = expandAsmTemplateTextForms("X%&Y", x86Forms(), ctx(0));
    ASSERT_FALSE(amp.has_value()) << "`%&` is expanded by no reference";
    EXPECT_NE(amp.error().find("`%&`"), std::string::npos) << amp.error();
    EXPECT_NE(amp.error().find("`%=`"), std::string::npos)
        << "the refusal lists what IS declared: " << amp.error();
}

TEST(AsmTemplateTextForms, AFeatureTheTargetDoesNotKnowIsRefusedNotReadAsAbsent) {
    AsmTemplateTextForms f = x86Forms();
    f.featureSelected = {{"%~", "avx512", "i", "f"}};
    auto const r = expandAsmTemplateTextForms("X%~Y", f, ctx(0));
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("avx512"), std::string::npos) << r.error();
}
