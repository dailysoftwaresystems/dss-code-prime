// D-ASM-IMMEDIATE-CONSTRAINT-FORM-NOT-REALIZED — the `"i"` IMMEDIATE constraint
// at the tier that decides what an immediate operand IS, plus
// D-MIR-LVALUE-REFUSAL-RENDERS-A-RAW-ORDINAL-NOT-A-NAME, whose render site is
// the same function's terminal refusal.
//
// ★★★ WHAT THIS FILE PINS, AND WHY IT IS HIR→MIR RATHER THAN THE EMIT. The
// emitted INSTRUCTION is pinned end to end and BY EXECUTION in
// `examples/c-subset/c_inline_asm_immediate_operand` (four targets, a `release`
// arm, four shapes with four distinct exit codes). What that example cannot
// isolate are the two facts THIS tier owns and nothing below it can recover:
//
//   1. THE CONSTANT PROOF. An immediate operand binds no register, so a
//      non-constant has no lowering at all — not a worse one, none. ✔MEASURED,
//      gcc 13.3.0, BOTH shipped targets, at `-O0` AND `-O2` (so it is not an
//      optimizer artefact): `__asm__("nop %1" : "=r"(r) : "i"(x))` on a
//      parameter is `error: impossible constraint in 'asm'`. DSS must match the
//      STRICTNESS, and the only instrument that can see a refusal is a test
//      that asks for one.
//   2. THAT THE FORM COMES FROM THE **TARGET**, NOT FROM THE LETTER'S SPELLING.
//      Every arm below except the last is equally green under an implementation
//      that tests `constraint == "i"` — which is the workaround the bar vetoes,
//      and which no grep catches. The last arm re-spells the letter in the
//      shipped JSON and requires the identical lowering; that implementation is
//      RED there and nowhere else.
//
// ⚠ THE ACCEPTED SET IS gcc's, MEASURED RATHER THAN GUESSED. ✔gcc 13.3.0 on
// x86_64 and aarch64: `"i"(7)` → `nop $7` / `nop 7`; `"i"(1+2)` → `nop $3` /
// `nop 3`; `"i"(K)` for `enum { K = 9 }` → `nop $9` / `nop 9`; `"i"(sizeof(int))`
// → `nop $4` / `nop 4`. So the constraint takes C's INTEGER CONSTANT EXPRESSION,
// and an implementation that accepted only a literal TOKEN would be conformant
// on the first of those four and wrong on the rest.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/hir_lowering_config.hpp"
#include "core/types/target_schema.hpp"
#include "hir/hir.hpp"
#include "hir/hir_inline_asm.hpp"
#include "hir/hir_node.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_asm_descriptor.hpp"
#include "mir/mir_opcode.hpp"
#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace dss;

namespace {

constexpr char const* kX86 = "x86_64";
constexpr char const* kArm = "arm64";

struct Lowered {
    // ⚠ DECLARED FIRST SO IT IS DESTROYED LAST — `analyze` takes the target
    // NON-OWNING and `SemanticModel` republishes it, so the schema must outlive
    // the model. This is the ordering `test_mir_lowering_c_subset.cpp` states in
    // full; it is repeated here because members destroy in reverse declaration
    // order and getting it wrong dangles silently rather than loudly.
    std::shared_ptr<TargetSchema const> target;
    std::optional<SemanticModel>        model;
    std::unique_ptr<CstToHirResult>     hir;
    DiagnosticReporter                  hirReporter;
    std::optional<HirToMirResult>       mir;
    DiagnosticReporter                  mirReporter;

    [[nodiscard]] std::string firstMirDiagnostic() const {
        return mirReporter.all().empty() ? std::string{}
                                         : mirReporter.all()[0].actual;
    }
};

// c-subset source → CompilationUnit → SemanticModel → HIR → MIR, with `target`
// in scope at BOTH the semantic pass (where a constraint letter is resolved
// against `asmConstraints`) and the MIR config (where the layout engine that
// folds `sizeof` comes from). A driver that threaded neither would make every
// arm below vacuous in a different way: no target ⇒ the letter resolves to
// nothing and the refusal is about the letter, not the form; no layout ⇒ the
// type-query arm refuses for a reason that has nothing to do with the operand.
[[nodiscard]] Lowered lowerWith(std::shared_ptr<TargetSchema const> target,
                                std::string                        src) {
    Lowered out;
    out.target = std::move(target);

    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) {
        ADD_FAILURE() << "GrammarSchema::loadShipped(c-subset) failed";
        return out;
    }
    UnitBuilder builder{*loaded, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());

    out.model.emplace(analyze(cu, DiagnosticBudget::libraryDefault(),
                              DataModel::Lp64, std::nullopt, std::nullopt,
                              std::nullopt, std::nullopt,
                              LongDoubleFormat::None, out.target.get()));
    out.hir = lowerToHir(*out.model, out.hirReporter);
    if (out.hir == nullptr) return out;

    MirLoweringConfig cfg;
    cfg.globalsAllowFloat = (*loaded)->hirLowering().globalsConstEval.allowFloat;
    if (out.target != nullptr) {
        cfg.aggregateLayout       = out.target->aggregateLayout();
        cfg.aggregateLayoutLoaded = out.target->aggregateLayoutLoaded();
    }
    out.mir.emplace(lowerToMir(out.hir->hir, out.hir->literalPool,
                               out.model->lattice().interner(), out.mirReporter,
                               &out.hir->sourceMap, cfg, /*ffiMap=*/nullptr,
                               &out.hir->linkageMap, &out.hir->mutabilityMap,
                               &out.hir->volatileMap, /*alignmentMap=*/nullptr,
                               &out.hir->threadLocalMap,
                               &out.hir->vlaSizeExprBySymbol,
                               &out.hir->sizeofVlaSymbol,
                               &out.hir->typedefVlaOriginBySymbol,
                               &out.hir->synthRecipeBySymbol,
                               &out.hir->returnsTwiceMap,
                               &out.hir->noInlineMap,
                               &out.hir->alwaysInlineMap,
                               &out.hir->noOptimizeMap,
                               &out.hir->noSanitizeThreadMap,
                               // Without the pool every descriptor-carrying
                               // `__asm__` is refused before it can be tested.
                               &out.hir->inlineAsmPool));
    return out;
}

[[nodiscard]] std::shared_ptr<TargetSchema const> shipped(std::string_view arch) {
    auto t = TargetSchema::loadShipped(arch);
    if (!t) {
        ADD_FAILURE() << "TargetSchema::loadShipped(\"" << arch << "\") failed";
        return nullptr;
    }
    return *t;
}

// The sole `InlineAsm` instruction of the lowered module. Searched by OPCODE
// over every instruction rather than by index: an index would be a second,
// silent premise about the lowering's instruction ordering.
[[nodiscard]] std::optional<MirInstId> soleAsmInst(Lowered const& l) {
    if (!l.mir.has_value()) return std::nullopt;
    Mir const& mir = l.mir->mir;
    std::optional<MirInstId> found;
    for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < mir.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
                MirInstId const id = mir.blockInstAt(b, ii);
                if (mir.instOpcode(id) != MirOpcode::InlineAsm
                    && mir.instOpcode(id) != MirOpcode::InlineAsmGoto) {
                    continue;
                }
                if (found.has_value()) {
                    ADD_FAILURE() << "more than one asm instruction — every "
                                     "probe here carries exactly one";
                    return std::nullopt;
                }
                found = id;
            }
        }
    }
    return found;
}

// The integer behind a MIR `Const`, or nullopt when the value is not one. This
// is the SAME question `mir_to_lir`'s `constIntegerValue` asks of the immediate
// operand, asked here directly so the assertion names the number rather than
// the fact that some lowering accepted it.
[[nodiscard]] std::optional<std::int64_t> constValue(Mir const& mir,
                                                     MirInstId id) {
    if (mir.instOpcode(id) != MirOpcode::Const) return std::nullopt;
    MirLiteralValue const& lit = mir.literalValue(mir.constLiteralIndex(id));
    if (auto const* i = std::get_if<std::int64_t>(&lit.value))  return *i;
    if (auto const* u = std::get_if<std::uint64_t>(&lit.value))
        return static_cast<std::int64_t>(*u);
    return std::nullopt;
}

// ★★★ THE WHOLE ASSERTION FOR ONE ACCEPTED IMMEDIATE, IN ONE PLACE. An operand
// is realized correctly iff FOUR things hold together, and each of them is a
// place a regression could hide:
//   - the lowering raised NO diagnostic (it did not refuse);
//   - the descriptor's input entry carries the `ImmInt` FORM (not a class);
//   - the MIR operand at that index is a `Const` (the operand list stays
//     1:1 with `inputs`, which is the descriptor's own stated contract);
//   - and that `Const` holds the EXPECTED NUMBER. ⚠ The number is what a pin
//     asserting only "no diagnostic" cannot see, and it is exactly what a
//     regression that silently defaulted the payload would change.
void expectImmediateOperand(Lowered const& l, std::int64_t expected,
                            std::string_view what) {
    SCOPED_TRACE(what);
    ASSERT_TRUE(l.mir.has_value());
    EXPECT_TRUE(l.mirReporter.all().empty())
        << "the immediate operand was refused: " << l.firstMirDiagnostic();
    auto const asmId = soleAsmInst(l);
    ASSERT_TRUE(asmId.has_value());

    Mir const&              mir  = l.mir->mir;
    MirAsmDescriptor const& desc = mir.asmDescriptor(*asmId);
    auto const              ops  = mir.instOperands(*asmId);
    ASSERT_FALSE(desc.inputs.empty());
    ASSERT_EQ(ops.size(), desc.inputs.size())
        << "`MirAsmDescriptor::inputs` is aligned 1:1 with the instruction's "
           "MIR operands — an immediate that carried its value OUTSIDE the "
           "operand list would break exactly this";

    MirAsmOperand const& imm = desc.inputs[0];
    EXPECT_TRUE(imm.operandKindResolved)
        << "constraint \"" << imm.constraint << "\" resolved to no form";
    EXPECT_EQ(static_cast<OperandKindFilter>(imm.operandKind),
              OperandKindFilter::ImmInt);
    EXPECT_EQ(mir.instOpcode(ops[0]), MirOpcode::Const)
        << "an immediate operand travels as a `Const` — the pipeline's own verb "
           "for a value that is known now";
    auto const v = constValue(mir, ops[0]);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, expected);
}

// One statement, wrapped so every probe differs ONLY in its asm statement. The
// `+r` accumulator is the runnable shape the corpus example uses, so the unit
// pins and the execution pin are talking about the same construct.
[[nodiscard]] std::string wrap(std::string_view stmt) {
    return std::string{"int f(int x) { int r = x; "} + std::string{stmt}
         + " return r; }";
}

// ── the accepted set ────────────────────────────────────────────────────────

// THE DEFECT, PINNED: a `"i"` operand reaches MIR as a constant, on both
// targets. ✔MEASURED before the fix, at the CLI, BOTH targets, debug AND
// release: `error[H0009] … operand 1 (constraint "i") binds the operand form
// 'imm32', which this pipeline does not yet realize`.
TEST(MirInlineAsmImmediate, LiteralImmediateReachesMirAsAConstantOnBothTargets) {
    for (char const* arch : {kX86, kArm}) {
        SCOPED_TRACE(arch);
        auto const l = lowerWith(shipped(arch),
                                 wrap(R"(__asm__("nop %1" : "+r"(r) : "i"(5));)"));
        expectImmediateOperand(l, 5, "literal");
    }
}

// ★★ A CONSTANT **EXPRESSION**, NOT A LITERAL TOKEN. gcc folds `1+2` to `$3`;
// an implementation that only accepted a literal would be green on the arm
// above and red here — which is the difference between "an integer constant
// expression" and "a number the parser happened to see".
TEST(MirInlineAsmImmediate, AConstantExpressionIsFoldedNotOnlyALiteral) {
    for (char const* arch : {kX86, kArm}) {
        SCOPED_TRACE(arch);
        auto const l = lowerWith(shipped(arch),
                                 wrap(R"(__asm__("nop %1" : "+r"(r) : "i"(2 + 3));)"));
        expectImmediateOperand(l, 5, "2 + 3");
    }
}

// ★★ A TYPE QUERY, which folds ONLY because the constant-proof environment
// carries the layout engine (`resolveTypeSize`). ✔gcc: `nop $4` / `nop 4`. An
// environment built with the const-eval defaults refuses this and passes every
// other arm in the file.
TEST(MirInlineAsmImmediate, ATypeQueryFoldsThroughTheLayoutEngine) {
    for (char const* arch : {kX86, kArm}) {
        SCOPED_TRACE(arch);
        auto const l = lowerWith(
            shipped(arch),
            wrap(R"(__asm__("nop %1" : "+r"(r) : "i"(sizeof(int)));)"));
        expectImmediateOperand(l, 4, "sizeof(int)");
    }
}

// ── the refusals, each matched to a reference measurement ───────────────────

// ★★★ THE CONSTANT PROOF. A runtime value fed to `"i"` must refuse BY NAME.
// ✔MEASURED: gcc 13.3.0 says `error: impossible constraint in 'asm'` on both
// targets at `-O0` and `-O2`. The alternative to refusing is materializing a
// register the template will not read — a silent wrong answer.
TEST(MirInlineAsmImmediate, ANonConstantImmediateIsRefusedByName) {
    for (char const* arch : {kX86, kArm}) {
        SCOPED_TRACE(arch);
        auto const l = lowerWith(shipped(arch),
                                 wrap(R"(__asm__("nop %1" : "+r"(r) : "i"(x));)"));
        ASSERT_FALSE(l.mirReporter.all().empty())
            << "a runtime value satisfied a constraint that requires a "
               "compile-time constant";
        std::string const msg = l.firstMirDiagnostic();
        EXPECT_NE(msg.find("INTEGER CONSTANT EXPRESSION"), std::string::npos)
            << msg;
        // ⚠ THE FORM IS NAMED IN THE MESSAGE, and that is the half a reader
        // acts on: it says WHICH declared form was not satisfied, so the fix is
        // "make this a constant" rather than "the letter is unsupported".
        EXPECT_NE(msg.find("imm32"), std::string::npos) << msg;
    }
}

// ★★ AN IMMEDIATE IN THE **OUTPUT** SECTION. ✔MEASURED: gcc 13.3.0 says
// `error: output number 0 not directly addressable` on both targets. Refusing
// what the reference refuses is as much conformance as accepting what it
// accepts — and lowering it as if it were an input would accept a statement
// that promises a write and performs none.
TEST(MirInlineAsmImmediate, AnImmediateInTheOutputSectionIsRefusedByName) {
    for (char const* arch : {kX86, kArm}) {
        SCOPED_TRACE(arch);
        auto const l = lowerWith(shipped(arch),
                                 wrap(R"(__asm__("nop %0" : "=i"(r));)"));
        ASSERT_FALSE(l.mirReporter.all().empty())
            << "a constant was accepted as an output location";
        EXPECT_NE(l.firstMirDiagnostic().find("OUTPUT section"),
                  std::string::npos)
            << l.firstMirDiagnostic();
    }
}

// ★ THE FORM'S RANGE. `imm32` names a 32-bit immediate slot; a wider constant
// does not satisfy the constraint, and truncating it would encode a number the
// source never wrote.
TEST(MirInlineAsmImmediate, AnImmediateWiderThanTheFormIsRefusedByName) {
    for (char const* arch : {kX86, kArm}) {
        SCOPED_TRACE(arch);
        auto const l = lowerWith(
            shipped(arch),
            wrap(R"(__asm__("nop %1" : "+r"(r) : "i"(4294967296));)"));
        ASSERT_FALSE(l.mirReporter.all().empty())
            << "a constant wider than the declared form was accepted";
        EXPECT_NE(l.firstMirDiagnostic().find("does not fit"), std::string::npos)
            << l.firstMirDiagnostic();
    }
}

// ── the agnosticism arm ─────────────────────────────────────────────────────

// ★★★ THE ARM THAT FAILS A `constraint == "i"` IMPLEMENTATION, AND THE ONLY ONE
// THAT DOES. The letter is TARGET vocabulary: `.target.json` says which spelling
// binds which of `TargetAsmConstraint::binds`' three arms, and the pipeline is
// required to route on the FORM. This re-spells the shipped declaration —
// `"i"` → `"K"`, same `operandKind`, nothing else touched — and demands the
// identical lowering. A tier that named the letter refuses here while staying
// green everywhere above.
//
// ⚠ THE MUTATION IS VERIFIED, NOT ASSUMED: `mutateShippedTargetSchemaDoc`
// THROWS on a byte-identical document, so an arm whose navigator missed its
// container cannot pass by asserting nothing (D-TEST-SCHEMA-MUTATION-HELPER-FAILS-OPEN).
TEST(MirInlineAsmImmediate, TheFormIsReadFromTheTargetNotFromTheLetterSpelling) {
    for (char const* arch : {kX86, kArm}) {
        SCOPED_TRACE(arch);
        auto mutated = test_support::mutateShippedTargetSchemaDoc(
            arch, [](nlohmann::json& doc) {
                bool respelled = false;
                for (auto& row : doc.at("asmConstraints")) {
                    if (!row.contains("letter") || row.at("letter") != "i")
                        continue;
                    row["letter"] = "K";
                    respelled = true;
                }
                ASSERT_TRUE(respelled)
                    << "this target no longer declares the letter this arm "
                       "re-spells — re-aim the arm rather than deleting it";
            });
        ASSERT_TRUE(mutated.has_value());

        auto const l = lowerWith(*mutated,
                                 wrap(R"(__asm__("nop %1" : "+r"(r) : "K"(5));)"));
        expectImmediateOperand(l, 5, "re-spelled letter");
    }
}

// ★★ THE MATCHED NEGATIVE, WITHOUT WHICH THE ARM ABOVE PROVES NOTHING. If `"K"`
// also worked against the SHIPPED target, the mutation was not what made the
// previous test pass and the pair would be measuring the letter's absence
// rather than the form's presence.
TEST(MirInlineAsmImmediate, AnUndeclaredLetterIsStillRefusedOnTheShippedTarget) {
    for (char const* arch : {kX86, kArm}) {
        SCOPED_TRACE(arch);
        auto const l = lowerWith(shipped(arch),
                                 wrap(R"(__asm__("nop %1" : "+r"(r) : "K"(5));)"));
        ASSERT_FALSE(l.mirReporter.all().empty())
            << "an undeclared constraint letter was accepted";
        EXPECT_NE(l.firstMirDiagnostic().find("resolved to nothing"),
                  std::string::npos)
            << l.firstMirDiagnostic();
    }
}

// ── D-MIR-LVALUE-REFUSAL-RENDERS-A-RAW-ORDINAL-NOT-A-NAME ───────────────────

// ★★★ THE REFUSAL NAMES THE KIND. ✔MEASURED at the CLI before the fix:
// `error[H0009] … lvalue kind ordinal 30 not supported by this lowering`. The
// refusal is LOUD and CORRECT — `30` is the defect, because it makes the reader
// count enumerators in a header, and any inserted kind silently retargets every
// ordinal a bug report ever quoted.
//
// ⚠ THE PROBE IS ORDINARY C, not a hand-built node: `"=r"(1+2)` asks to write a
// register back into an expression that is not a location, which is the door
// this refusal has always guarded.
TEST(MirInlineAsmImmediate, TheLvalueRefusalNamesTheKindItRefused) {
    auto const l = lowerWith(shipped(kX86),
                             wrap(R"(__asm__("nop %0" : "=r"(1 + 2));)"));
    ASSERT_FALSE(l.mirReporter.all().empty());
    std::string const msg = l.firstMirDiagnostic();
    EXPECT_NE(msg.find("'BinaryOp'"), std::string::npos) << msg;
    // The ordinal is KEPT beside the name — it is the only thing a bug report
    // can compare against a build whose enum has since moved — so a message
    // that dropped it would lose a fact rather than gain clarity.
    EXPECT_NE(msg.find("ordinal"), std::string::npos) << msg;
}

// ★ THE TABLE ITSELF, ASKED DIRECTLY. `hirKindName` is the projection the
// message above goes through; pinning it here is what makes a failure say
// "the table is wrong" instead of "the message changed".
// ⚠ THE UNLISTED SENTINEL IS PART OF THE CONTRACT. `Count_` is a counter, not a
// kind, and `EnumNameTable::name()`'s row-0 fallback would hand it `Module`'s
// spelling — a plausible wrong answer. `nameOrEmpty` is the only legal
// projection here, and this arm is what says so.
TEST(MirInlineAsmImmediate, TheHirKindNameTableSpellsEveryCoreKindAndNoSentinel) {
    EXPECT_EQ(hirKindName(HirKind::BinaryOp), "BinaryOp");
    EXPECT_EQ(hirKindName(HirKind::InlineAsm), "InlineAsm");
    EXPECT_EQ(hirKindName(HirKind::Module), "Module");
    EXPECT_TRUE(hirKindName(HirKind::Count_).empty())
        << "`Count_` is a counter, not a kind — a spelling for it would render "
           "in a diagnostic as though it were one";
    // Every core kind has one, which is what the `rows == Count_` static_assert
    // beside the table guarantees at compile time. Asked again at run time so a
    // future table edit that satisfied the COUNT while duplicating a row is
    // still caught here (`DSS_CHECK_ENUM_NAME_TABLE` covers duplicates; this
    // covers the join of the two).
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(HirKind::Count_);
         ++i) {
        EXPECT_FALSE(hirKindName(static_cast<HirKind>(i)).empty())
            << "core HirKind ordinal " << i << " has no spelling";
    }
}

} // namespace
