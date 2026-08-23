// D-ASM-MEMORY-CONSTRAINT-REFUSED-DESPITE-BEING-DECLARED — THE FRONT-END HALF.
//
// A constraint letter binds one of THREE things (`TargetAsmConstraint::binds`):
// a register CLASS (`"r"`), a specific REGISTER (`"=a"`), or an operand FORM
// (`"m"` → `membase`, `"i"` → `imm32`). The CST→HIR resolution carried only the
// first two: the `AsmConstraintBinding::OperandKind` arm was a bare `break`, so
// a letter the target DOES declare arrived at every tier below looking exactly
// like a letter it does NOT — and the tier that refuses said so in those words,
// about a correctly-declared letter.
//
// ✔MEASURED before the fix, at the CLI, on a clean HEAD worktree, BOTH shipped
// targets, `--config=debug` AND `--config=release`:
//   `int f(int *p){ int r; __asm__("nop %1" : "=r"(r) : "m"(*p)); return r; }`
//   → error[H0009] … inline-asm operand 1 (constraint "m") has no resolved
//     register class — the constraint letter was never bound to a processor
// while gcc 13.3.0 compiles the same source on both (`nop (%rdi)` on x86_64,
// `nop [x0]` on aarch64). ⇒ the refusal's stated reason was FALSE, which is
// worse than no refusal: it sends the next reader to fix a config that is
// already correct.
//
// ★★ WHAT THIS FILE PINS, AND WHY IT IS THE FRONT END RATHER THAN THE EMIT.
// The emitted INSTRUCTION is pinned end-to-end and BY EXECUTION in
// `examples/c-subset/c_inline_asm_memory_operand` (four targets, a `release`
// arm, an exit code that is a function of the memory operand reaching the
// template). What that example CANNOT isolate is the fact whose absence caused
// the defect: that the FORM the target declared was carried out of the
// resolution at all. Deleting the two assignments this file asserts on makes
// every arm of that example red too — but with a diagnostic three tiers away
// from the line that caused it, which is the distance this file removes.
//
// ★ EVERY POSITIVE HAS A MATCHED NEGATIVE. A pin that only says "the form
// resolved" is satisfied by a resolution that says `membase` about everything;
// the class-bound control (`"r"` → a register class and NO form) and the
// undeclared-letter control (both arms unset, the state the honest refusal
// still catches) are what make each assertion a statement about ONE arm.

#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "hir/hir.hpp"
#include "hir/hir_inline_asm.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

// ⚠ THE TARGET MUST OUTLIVE THE MODEL AND THE HIR ALIKE — the model holds a
// NON-OWNING pointer and the lowering reads through it. A dangling target here
// would not fail loudly; it would read garbage letters, which is the shape this
// suite exists to catch.
struct Lowered {
    std::shared_ptr<TargetSchema const>    target;
    std::shared_ptr<CompilationUnit const> cu;
    std::optional<SemanticModel>           model;
    DiagnosticReporter                     hirReporter;
    std::unique_ptr<CstToHirResult>        hir;
};

// Analyze + lower ONE c-subset source with `arch` in scope. The target is what
// makes this a test of the resolution rather than of the parse: with no target
// the letters are deliberately left unresolved and every arm reads false.
[[nodiscard]] std::unique_ptr<Lowered> lowerFor(std::string_view arch,
                                                std::string      src) {
    auto out = std::make_unique<Lowered>();
    auto loaded = TargetSchema::loadShipped(arch);
    if (!loaded) {
        ADD_FAILURE() << "TargetSchema::loadShipped(\"" << arch << "\") failed";
        return out;
    }
    out->target = *loaded;
    out->cu     = buildShippedUnit("c-subset", {std::move(src)});
    out->model.emplace(analyze(out->cu, DiagnosticBudget::libraryDefault(),
                               DataModel::Lp64, std::nullopt, std::nullopt,
                               std::nullopt, arch, LongDoubleFormat::None,
                               out->target.get()));
    out->hir = lowerToHir(*out->model, out->hirReporter);
    return out;
}

// The single `InlineAsm` descriptor this source produced. Searched by KIND over
// the whole module rather than by a node index: an index would be a second,
// silent premise about the lowering's node ordering.
[[nodiscard]] HirInlineAsmDescriptor const*
soleInlineAsmDescriptor(Lowered const& l) {
    if (l.hir == nullptr) return nullptr;
    Hir const& hir = l.hir->hir;
    HirInlineAsmDescriptor const* found = nullptr;
    // ⚠ NODE IDS ARE 1-BASED — ordinal 0 is the arena's invalid sentinel and
    // `Hir::at` aborts on it. The `i = 1` lower bound is the same idiom every
    // tree walk in this suite uses, and it is a fact about the arena rather
    // than a defensive off-by-one.
    for (std::uint32_t i = 1; i < hir.nodeCount(); ++i) {
        HirNodeId const n{i};
        if (hir.kind(n) != HirKind::InlineAsm) continue;
        std::uint32_t const handle = hir.payload(n);
        if (!l.hir->inlineAsmPool.contains(handle)) continue;
        if (found != nullptr) {
            ADD_FAILURE() << "more than one descriptor-carrying InlineAsm node "
                             "— every probe here carries exactly one, so a "
                             "second means the source under test changed";
            return nullptr;
        }
        found = &l.hir->inlineAsmPool.at(handle);
    }
    return found;
}

// One statement, wrapped so every probe differs ONLY in its asm statement.
// `*p` gives the memory operand a genuinely addressable lvalue whose address is
// not a compile-time constant — the shape gcc lowers to `(%rdi)` / `[x0]`.
[[nodiscard]] std::string wrap(std::string_view stmt) {
    return "int f(int *p){ int r = 0; " + std::string{stmt}
           + " return r; }\n";
}

constexpr char const* kX86 = "x86_64";
constexpr char const* kArm = "arm64";

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// THE DEFECT, PINNED: a form-bound letter resolves to a FORM, on both targets.
// ─────────────────────────────────────────────────────────────────────────────

// ★★★ THE ASSERTION THE BARE `break` FAILED. Both shipped targets declare
// `{ "letter": "m", "binds": "operandKind", "operandKind": "membase" }`, so the
// lowering must record `membase` — and must NOT record a register class, which
// is the half that keeps a memory operand from being filed in a register bank
// the source never asked for.
TEST(InlineAsmOperandKindBinding, MemoryLetterResolvesToTheMembaseFormNotAClass) {
    for (char const* arch : {kX86, kArm}) {
        SCOPED_TRACE(arch);
        auto const l = lowerFor(arch, wrap(R"(__asm__("nop %1" : "=r"(r) : "m"(*p));)"));
        ASSERT_TRUE(l->model.has_value());
        ASSERT_FALSE(l->model->hasErrors())
            << "the SEMANTIC tier accepts `\"m\"` on both shipped targets — a "
               "failure here means the probe stopped reaching the resolution";
        auto const* desc = soleInlineAsmDescriptor(*l);
        ASSERT_NE(desc, nullptr);
        ASSERT_EQ(desc->operands.size(), 2u);

        // operand 1 — the subject.
        HirInlineAsmOperand const& mem = desc->operands[1];
        EXPECT_EQ(mem.constraint.raw, "m");
        EXPECT_TRUE(mem.operandKindResolved)
            << "the letter IS declared by " << arch
            << "; leaving the form unrecorded is what made a declared letter "
               "indistinguishable from an undeclared one three tiers down";
        EXPECT_EQ(mem.operandKind,
                  static_cast<std::uint8_t>(OperandKindFilter::MemBase));
        // ⚠ THE OTHER HALF, AND IT IS NOT REDUNDANT: a resolution that set the
        // form AND a class would hand the consumer two answers, and the
        // consumer's `regClass` arm is the one that wins by position.
        EXPECT_FALSE(mem.regClassResolved)
            << "a form-bound letter selects NO register class — the ADDRESS is "
               "what gets a register";
        EXPECT_TRUE(mem.fixedRegister.empty());
    }
}

// ★ THE MATCHED CONTROL — a CLASS-bound letter in the SAME statement resolves
// the other way. Without it, every assertion above is satisfied by a resolution
// that answers `membase` to everything.
TEST(InlineAsmOperandKindBinding, ClassBoundLetterResolvesToAClassAndNoForm) {
    for (char const* arch : {kX86, kArm}) {
        SCOPED_TRACE(arch);
        auto const l = lowerFor(arch, wrap(R"(__asm__("nop %1" : "=r"(r) : "m"(*p));)"));
        auto const* desc = soleInlineAsmDescriptor(*l);
        ASSERT_NE(desc, nullptr);
        ASSERT_EQ(desc->operands.size(), 2u);

        HirInlineAsmOperand const& cls = desc->operands[0];
        EXPECT_EQ(cls.constraint.raw, "=r");
        EXPECT_TRUE(cls.regClassResolved);
        EXPECT_FALSE(cls.operandKindResolved)
            << "`\"r\"` binds a register CLASS — recording a form for it would "
               "make the consumer's form arm fire on every ordinary operand";
    }
}

// ★★ THE SECOND FORM-BOUND LETTER, WHICH IS WHY THE ARM IS DRIVEN BY CONFIG AND
// NOT BY THE SPELLING `"m"`. Both targets also declare `"i"` → `imm32`, and it
// must resolve to ITS OWN form. An implementation that special-cased the letter
// `m` passes every assertion above and fails this one — which is exactly the
// workaround the bar forbids.
TEST(InlineAsmOperandKindBinding, ImmediateLetterResolvesToItsOwnFormNotMembase) {
    for (char const* arch : {kX86, kArm}) {
        SCOPED_TRACE(arch);
        auto const l = lowerFor(arch, wrap(R"(__asm__("nop %1" : "=r"(r) : "i"(7));)"));
        auto const* desc = soleInlineAsmDescriptor(*l);
        ASSERT_NE(desc, nullptr);
        ASSERT_EQ(desc->operands.size(), 2u);

        HirInlineAsmOperand const& imm = desc->operands[1];
        EXPECT_EQ(imm.constraint.raw, "i");
        EXPECT_TRUE(imm.operandKindResolved);
        EXPECT_EQ(imm.operandKind,
                  static_cast<std::uint8_t>(OperandKindFilter::ImmInt));
        EXPECT_FALSE(imm.regClassResolved);
    }
}

// ★★★ THE NEGATIVE THAT KEEPS THE HONEST REFUSAL HONEST. With NO target in
// scope nothing can resolve — the state the consumer's
// `!regClassResolved && !operandKindResolved` refusal exists for. If this ever
// reads true, the resolution started guessing, and the guess would be
// `OperandKindFilter::Reg` (value 0), i.e. the plausible wrong answer rather
// than the loud one.
TEST(InlineAsmOperandKindBinding, NoTargetInScopeLeavesBothArmsUnresolved) {
    auto cu = buildShippedUnit(
        "c-subset", {wrap(R"(__asm__("nop %1" : "=r"(r) : "m"(*p));)")});
    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault());
    DiagnosticReporter rep;
    auto hir = lowerToHir(model, rep);
    ASSERT_NE(hir, nullptr);

    HirInlineAsmDescriptor const* desc = nullptr;
    for (std::uint32_t i = 1; i < hir->hir.nodeCount(); ++i) {
        HirNodeId const n{i};
        if (hir->hir.kind(n) != HirKind::InlineAsm) continue;
        std::uint32_t const handle = hir->hir.payload(n);
        if (hir->inlineAsmPool.contains(handle)) {
            desc = &hir->inlineAsmPool.at(handle);
        }
    }
    ASSERT_NE(desc, nullptr);
    ASSERT_EQ(desc->operands.size(), 2u);
    for (auto const& o : desc->operands) {
        EXPECT_FALSE(o.regClassResolved) << "constraint " << o.constraint.raw;
        EXPECT_FALSE(o.operandKindResolved) << "constraint " << o.constraint.raw;
    }
}
