// A REGISTER-PINNED ASM OUTPUT REACHES THE ALLOCATOR'S FORBIDDEN SET —
// D-LIR-PER-INSTRUCTION-OUTPUTS-NOT-ENFORCED-SUBSET-OF-CLOBBERED.
//
// ★★★ THE CLAIM, AND IT IS A CHAIN OF THREE LINKS RATHER THAN ONE FACT. The
// register allocator's forbidden set at an instruction is `inputs ∪ clobbered`
// and OUTPUTS ARE DELIBERATELY OMITTED. For the instruction's own operands the
// omission is sound on its own terms — the instruction reads its operands
// before it writes its outputs. For a value that merely LIVES ACROSS the
// instruction that argument says nothing whatever, and what stands in its place
// is the invariant `outputs ⊆ clobbered`. So the property this file pins is:
//
//   (1) the C lowering puts a register-pinned `"=a"` output into the
//       per-instruction constraint's OUTPUT set, and
//   (2) into its CLOBBER set as well — the invariant, discharged where the
//       entry is built, because GNU C forbids the SOURCE from writing an
//       output register in the clobber list, and
//   (3) `effectiveForbiddenOrdinals` — the one accessor every consumer asks —
//       therefore reports that register as forbidden at the asm instruction.
//
// Link (3) is the only one a crossing value can feel, and links (1) and (2) are
// the only reason link (3) holds. Asserting any one of them alone leaves the
// chain unpinned: (1) and (2) without (3) is a constraint that is carried and
// never consulted (the exact shape D-LIR-PER-INST-REG-CONSTRAINTS shipped),
// and (3) without (1) and (2) cannot distinguish "the invariant held" from
// "this target forbids that register for some unrelated reason".
//
// ★★ WHY THE UNPINNED CONTROL IS HALF THE TEST. "%rax is forbidden at this
// instruction" is not discriminating on its own — an allocator that forbade
// every register everywhere would satisfy it and be useless. The `"=r"` arm
// lowers the SAME statement with the SAME shape and one bit of difference (the
// output is class-bound, not register-bound), and there the per-instruction
// carrier must stay EMPTY. `EXPECT_TRUE(...empty())` on the control is what
// gives the positive arm its meaning.
//
// ★ WHY THE REAL LOWERING RATHER THAN A CONSTRAINT BUILT BY HAND. A hand-built
// entry would satisfy the invariant because THIS FILE wrote it that way, and
// would stay green with the lowering's normalization deleted — the ADD
// direction, which pins nothing. Driving c → HIR → MIR → LIR means the
// subject is the producer that actually fills the carrier.
//
// ⚠ THE RUNNABLE HALF LIVES IN `examples/c/c_inline_asm_pinned_output_live_across`,
// and neither tier replaces the other: this file asserts the STRUCTURE that no
// allocation coincidence can fake, the example asserts that a real program with
// fourteen values live across the block still computes the right answer.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`
// (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).
//
// ⚠ x86_64 ONLY, for a CONFIG reason: a pinned output needs a constraint letter
// that BINDS A NAMED REGISTER, and `arm64.target.json` declares only class- and
// form-bound letters. The `HasARegisterBoundConstraintLetter` arm below states
// that as a fact about the shipped document rather than leaving it implicit, so
// the day arm64 grows one, this file says where the missing arm belongs.

#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_reg_constraints.hpp"
#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace {

// The first instruction in the module carrying a per-INSTRUCTION register
// constraint. The asm expansion attaches ONE handle to every instruction it
// emitted, so the first is representative and the search does not depend on
// which mnemonic the template happened to spell.
[[nodiscard]] std::optional<LirInstId> firstConstrainedInst(Lir const& lir) {
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                LirInstId const id = lir.blockInstAt(b, ii);
                if (lir.instRegConstraints(id) != nullptr) return id;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool holds(std::vector<std::uint16_t> const& v,
                         std::uint16_t ord) {
    return std::find(v.begin(), v.end(), ord) != v.end();
}

// `void f(void){ int a; int b; __asm__(<template> : <outputs>); }` — the
// smallest statement that fills the per-instruction carrier.
[[nodiscard]] std::string asmSource(char const* outputs) {
    return std::string{"void f(void){ int a; int b; __asm__ __volatile__ ("}
         + "\"movl $11, %0\\n\\tmovl $22, %1\" : " + outputs + "); }";
}

} // namespace

// ── the shipped document's own precondition ──────────────────────────────────
//
// Stated as an arm rather than assumed, because every assertion below depends
// on it and because it is the reason this file has no aarch64 half.
TEST(LirAsmPinnedOutput, X86DeclaresARegisterBoundConstraintLetterAndArm64DoesNot) {
    auto x86 = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(x86.has_value());
    auto const* a = (*x86)->asmConstraint("a");
    ASSERT_NE(a, nullptr)
        << "x86_64 must declare the constraint letter `a` — `\"=a\"` is the "
           "shape this whole file is about";
    ASSERT_EQ(a->binds, AsmConstraintBinding::Register)
        << "letter 'a' must BIND A NAMED REGISTER, not a class";
    ASSERT_TRUE(a->registerOrdinal.has_value());
    EXPECT_EQ(*a->registerOrdinal,
              (*x86)->registerByName("rax").value_or(0xFFFFu));

    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(arm.has_value());
    for (auto const& c : (*arm)->asmConstraints()) {
        EXPECT_NE(c.binds, AsmConstraintBinding::Register)
            << "arm64 grew a register-BOUND asm constraint letter ('"
            << c.letter
            << "'). The per-instruction OUTPUT carrier is now reachable on "
               "that target, so this file owes an aarch64 arm and so does "
               "examples/c/c_inline_asm_pinned_output_live_across";
    }
}

// ── ARM 1: the chain, through the real lowering ──────────────────────────────
TEST(LirAsmPinnedOutput, PinnedOutputIsAlsoClobberedAndSoIsForbidden) {
    auto L = lowerCToLir(asmSource("\"=a\"(a), \"=d\"(b)"), "x86_64");
    ASSERT_FALSE(L.model.hasErrors())
        << (L.model.diagnostics().all().empty()
                ? std::string{} : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << (L.mirReporter.all().empty() ? std::string{}
                                        : L.mirReporter.all()[0].actual);
    ASSERT_TRUE(L.lir.ok)
        << "a register-pinned asm output must LOWER: "
        << (L.lirReporter.all().empty() ? std::string{}
                                        : L.lirReporter.all()[0].actual);

    Lir const& lir = L.lir.lir;
    auto const rax = L.target->registerByName("rax");
    auto const rdx = L.target->registerByName("rdx");
    ASSERT_TRUE(rax.has_value());
    ASSERT_TRUE(rdx.has_value());

    auto const inst = firstConstrainedInst(lir);
    ASSERT_TRUE(inst.has_value())
        << "the asm expansion attached no per-instruction constraint at all — "
           "with none attached the allocator forbids nothing here and every "
           "assertion below would be vacuous";
    auto const* c = lir.instRegConstraints(*inst);
    ASSERT_NE(c, nullptr);

    // (1) both pinned outputs are recorded AS OUTPUTS.
    EXPECT_TRUE(holds(c->outputOrdinals, *rax));
    EXPECT_TRUE(holds(c->outputOrdinals, *rdx));

    // (2) THE INVARIANT. GNU C forbids the SOURCE from naming an output
    // register in the clobber list, so this is the lowering's own obligation
    // and nothing upstream can discharge it.
    EXPECT_FALSE(c->firstOutputNotClobbered().has_value())
        << "a pinned output is missing from the clobber set — the allocator "
           "omits outputs from its forbidden set on exactly this basis, so a "
           "value live across the block would keep a register the block "
           "overwrites, with no diagnostic";
    EXPECT_TRUE(holds(c->clobberedOrdinals, *rax));
    EXPECT_TRUE(holds(c->clobberedOrdinals, *rdx));

    // (3) …AND THE ONE ACCESSOR EVERY CONSUMER ASKS AGREES. This is the only
    // link a value living across the block can feel.
    auto const forbidden = effectiveForbiddenOrdinals(lir, *L.target, *inst);
    EXPECT_TRUE(holds(forbidden, *rax))
        << "%rax is a pinned OUTPUT of this instruction and is not in its "
           "effective forbidden set";
    EXPECT_TRUE(holds(forbidden, *rdx))
        << "%rdx is a pinned OUTPUT of this instruction and is not in its "
           "effective forbidden set";
}

// ── ARM 2: the control — one bit of difference, and it must not forbid ───────
//
// The same statement with CLASS-bound outputs. Nothing is pinned, so the
// per-instruction carrier records no output and no clobber, and the forbidden
// set stays empty. Without this arm, "the register is forbidden" cannot be told
// apart from "everything is always forbidden".
TEST(LirAsmPinnedOutput, ClassBoundOutputsPinNothingAndForbidNothing) {
    auto L = lowerCToLir(asmSource("\"=r\"(a), \"=r\"(b)"), "x86_64");
    ASSERT_FALSE(L.model.hasErrors());
    ASSERT_TRUE(L.mir.ok);
    ASSERT_TRUE(L.lir.ok)
        << (L.lirReporter.all().empty() ? std::string{}
                                        : L.lirReporter.all()[0].actual);

    Lir const& lir = L.lir.lir;
    auto const rax = L.target->registerByName("rax");
    ASSERT_TRUE(rax.has_value());

    // ⚠ ASSERTED OVER EVERY INSTRUCTION, NOT OVER THE FIRST CONSTRAINED ONE.
    // The expected outcome here is that NO instruction carries a constraint at
    // all, and a control that returns early on finding none asserts nothing —
    // the vacuous shape this repo keeps rediscovering. The loop states the
    // property directly, so it has work to do in both outcomes.
    std::size_t constrained = 0;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                LirInstId const id = lir.blockInstAt(b, ii);
                if (auto const* c = lir.instRegConstraints(id); c != nullptr) {
                    ++constrained;
                    EXPECT_TRUE(c->outputOrdinals.empty())
                        << "a CLASS-bound `\"=r\"` output is allocator-chosen "
                           "and must not enter the per-instruction OUTPUT set "
                           "— recording one would pin a register the source "
                           "never named";
                }
                EXPECT_FALSE(holds(effectiveForbiddenOrdinals(lir, *L.target,
                                                              id),
                                   *rax))
                    << "nothing in this statement names %rax, so no "
                       "instruction it lowered to may forbid it — otherwise "
                       "the positive arm cannot tell 'the pin reached the "
                       "allocator' from 'this register is always forbidden'";
            }
        }
    }
    EXPECT_EQ(constrained, 0u)
        << "class-bound outputs pin no register, so the per-instruction "
           "carrier must stay empty for this statement";
}
