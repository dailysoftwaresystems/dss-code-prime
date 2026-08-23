// D-HIR-TEXT-INLINE-ASM-OPERAND-KIND-DROPPED-IN-TRANSIT — the CONSEQUENCE pin.
//
// ★★★ WHY THIS IS A SEPARATE FILE AND NOT ANOTHER ARM IN `test_hir_text.cpp`.
// The round-trip arms there assert that the operand-form binding survives the
// text tier. That is the mechanism. THIS file asserts the thing that actually
// goes wrong when it does not: the very next tier reads the dropped pair as
// "the letter resolved to nothing" and refuses the statement with a reason that
// is FALSE — the letter is declared, by both shipped targets, and the binding
// was lost in transit. A reader sent to fix `asmConstraints` finds a config that
// is already correct, which is the whole reason
// D-ASM-MEMORY-CONSTRAINT-REFUSED-DESPITE-BEING-DECLARED exists; this pin is
// there so the same defect cannot be re-created by a serialization gap.
//
// ⚠ A PIN THAT ONLY ASSERTED "NO DIAGNOSTIC" WOULD BE GREEN ON THIS DEFECT IN
// THE OTHER DIRECTION. The drop is silent at the text tier — the parse is clean,
// the module verifies, the bytes re-emit identically — so the arms below assert
// the ABSENCE OF ONE NAMED REFUSAL and the PRESENCE of the rebuilt binding, not
// merely that something parsed.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/hir_inline_asm.hpp"
#include "hir/hir_text.hpp"
#include "mir/lowering/hir_to_mir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

using namespace dss;

namespace {

// The exact sentence `lowerInlineAsm` produces for an operand whose letter
// resolved to NEITHER a register class NOR an operand form. Quoted as a
// substring of the real message rather than restated, so a reworded diagnostic
// makes this pin fail loudly instead of passing vacuously.
constexpr std::string_view kResolvedToNothing =
    "resolved to nothing - the constraint letter is not declared by this target";

// The same sentence with the em dash the message actually carries. The file is
// compiled without assuming a code page, so both spellings are tried and a hit
// on either is a hit.
constexpr std::string_view kResolvedToNothingPrefix =
    "resolved to nothing";

// ⚠⚠ THREADING THE POOL IS NOT BOILERPLATE — WITHOUT IT THIS WHOLE FILE ASSERTS
// NOTHING, AND THAT IS MEASURED RATHER THAN ARGUED. The first draft called
// `lowerToMir` with its defaults; the two positive arms went GREEN and the
// matched negative went RED, because `lowerInlineAsm` refuses BEFORE reading a
// single operand when the pool is absent ("the HirInlineAsmPool was not threaded
// into lowerToMir"). Every operand-form assertion was therefore about a
// statement the lowering never looked at. The negative arm is the only reason
// that was visible at all.
[[nodiscard]] HirToMirResult lowerAsmToMir(HirParseResult&     parsed,
                                           DiagnosticReporter& reporter) {
    return lowerToMir(parsed.hir, parsed.literalPool,
                      parsed.interner, reporter,
                      &parsed.sourceMap, MirLoweringConfig{},
                      /*ffiMap=*/nullptr, /*linkageMap=*/nullptr,
                      /*mutabilityMap=*/nullptr, /*volatileMap=*/nullptr,
                      /*alignmentMap=*/nullptr, /*threadLocalMap=*/nullptr,
                      /*vlaSizeMap=*/nullptr, /*sizeofVlaSymMap=*/nullptr,
                      /*typedefVlaOriginMap=*/nullptr,
                      /*synthRecipeMap=*/nullptr, /*returnsTwiceMap=*/nullptr,
                      /*noInlineMap=*/nullptr, /*alwaysInlineMap=*/nullptr,
                      /*noOptimizeMap=*/nullptr,
                      /*noSanitizeThreadMap=*/nullptr,
                      &parsed.inlineAsmPool);
}

[[nodiscard]] std::string allDiagText(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) { out += d.actual; out += '\n'; }
    return out;
}

// A module whose single statement is an extended `__asm__` carrying ONE
// form-bound operand written with `<letter>` / `<form>`.
[[nodiscard]] std::string moduleWithFormBoundOperand(std::string_view letter,
                                                     std::string_view form) {
    return std::string(
               "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
               "  function %1 : fn() -> void {\n    block {\n"
               "      inline_asm \"nop %0\" { extended outputs 0 operands ( \"")
         + std::string(letter) + "\" spells ( \"%0\" ) operand_kind "
         + std::string(form) + " -> lit int 7 : i32 ) }\n"
           "      return\n    }\n  }\n}\n";
}

} // namespace

// Both shipped targets declare `"i"` → `imm32` and `"m"` → `membase`; those are
// the two forms the pipeline realizes, so they are the two an author can
// actually write. Each is driven through the text tier and then through the
// tier that reads the binding.
TEST(HirTextOperandForm, ImmediateFormReachesTheMirLoweringWithItsBindingIntact) {
    std::string const text = moduleWithFormBoundOperand("i", "imm32");
    DiagnosticReporter pr;
    auto parsed = parseHir(text, CompilationUnitId{1}, pr);
    ASSERT_TRUE(parsed->ok) << allDiagText(pr);
    ASSERT_EQ(parsed->inlineAsmPool.size(), 1u);
    ASSERT_EQ(parsed->inlineAsmPool.at(1).operands.size(), 1u);
    ASSERT_TRUE(parsed->inlineAsmPool.at(1).operands[0].operandKindResolved)
        << "the binding did not survive the text tier — every assertion below "
           "would then be about a DIFFERENT descriptor";
    EXPECT_EQ(parsed->inlineAsmPool.at(1).operands[0].operandKind,
              static_cast<std::uint8_t>(OperandKindFilter::ImmInt));

    DiagnosticReporter mr;
    auto const mir = lowerAsmToMir(*parsed, mr);
    (void)mir;
    std::string const diags = allDiagText(mr);
    EXPECT_EQ(diags.find(kResolvedToNothingPrefix), std::string::npos)
        << "the round trip re-created D-ASM-MEMORY-CONSTRAINT-REFUSED-DESPITE-"
           "BEING-DECLARED one tier over: the letter IS declared and the "
           "refusal says it is not, because the binding was dropped in "
           "transit.\n" << diags;
    EXPECT_EQ(diags.find(kResolvedToNothing), std::string::npos) << diags;
}

TEST(HirTextOperandForm, MemoryFormReachesTheMirLoweringWithItsBindingIntact) {
    std::string const text = moduleWithFormBoundOperand("m", "membase");
    DiagnosticReporter pr;
    auto parsed = parseHir(text, CompilationUnitId{2}, pr);
    ASSERT_TRUE(parsed->ok) << allDiagText(pr);
    ASSERT_EQ(parsed->inlineAsmPool.size(), 1u);
    ASSERT_EQ(parsed->inlineAsmPool.at(1).operands.size(), 1u);
    ASSERT_TRUE(parsed->inlineAsmPool.at(1).operands[0].operandKindResolved);
    EXPECT_EQ(parsed->inlineAsmPool.at(1).operands[0].operandKind,
              static_cast<std::uint8_t>(OperandKindFilter::MemBase));

    DiagnosticReporter mr;
    auto const mir = lowerAsmToMir(*parsed, mr);
    (void)mir;
    std::string const diags = allDiagText(mr);
    EXPECT_EQ(diags.find(kResolvedToNothingPrefix), std::string::npos) << diags;
    EXPECT_EQ(diags.find(kResolvedToNothing), std::string::npos) << diags;
}

// ★ THE MATCHED NEGATIVE, so the two arms above cannot pass by the diagnostic
// having become unreachable for some unrelated reason. An operand that genuinely
// resolved to nothing — no `class`, no `pin`, no `operand_kind` — must STILL be
// refused with exactly that sentence. Without this arm, deleting the refusal
// outright would leave the whole file green.
TEST(HirTextOperandForm, AnOperandThatResolvedToNothingIsStillRefusedByThatName) {
    std::string const text =
        "dsshir 1\nsymbols {\n  %1 \"f\"\n}\nmodule \"toy\" {\n"
        "  function %1 : fn() -> void {\n    block {\n"
        "      inline_asm \"nop %0\" { extended outputs 0 operands ( \"i\" "
        "spells ( \"%0\" ) -> lit int 7 : i32 ) }\n"
        "      return\n    }\n  }\n}\n";
    DiagnosticReporter pr;
    auto parsed = parseHir(text, CompilationUnitId{3}, pr);
    ASSERT_TRUE(parsed->ok) << allDiagText(pr);
    ASSERT_EQ(parsed->inlineAsmPool.size(), 1u);
    ASSERT_EQ(parsed->inlineAsmPool.at(1).operands.size(), 1u);
    EXPECT_FALSE(parsed->inlineAsmPool.at(1).operands[0].operandKindResolved);
    EXPECT_FALSE(parsed->inlineAsmPool.at(1).operands[0].regClassResolved);

    DiagnosticReporter mr;
    auto const mir = lowerAsmToMir(*parsed, mr);
    (void)mir;
    std::string const diags = allDiagText(mr);
    EXPECT_NE(diags.find(kResolvedToNothingPrefix), std::string::npos)
        << "an operand with no binding at all must still be refused — this arm "
           "is what proves the two positive arms are not green because the "
           "refusal stopped firing.\n" << diags;
}
