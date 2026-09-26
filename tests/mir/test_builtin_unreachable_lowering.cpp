// GNU `__builtin_unreachable()` ENDS ITS BLOCK — pinned at the MIR it lowers to
// (P68 round 12, D-C-STDDEF-H-LACKS-UNREACHABLE, fold S1b).
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// `__builtin_unreachable()` (gcc and clang) and C23's `unreachable()` (which
// <stddef.h> expands to it) tell the compiler control never gets there. Reaching
// it anyway is undefined behaviour, and the ONE lowering that is never silent is a
// block that ENDS there: MIR's own `Unreachable` terminator — the one a noreturn
// call's `Block{ExprStmt(call), Unreachable}` already produces — which mir_to_lir
// lowers to the target's declared `unreachable` opcode (`ud2` on x86_64, `brk #0`
// on aarch64). A lowering that emitted NOTHING would let the block fall through
// into whatever the layout puts next — the classic "unreachable" miscompile — and
// every program that never reaches it would still pass. Before this fold DSS had
// no such builtin at all (✔MEASURED 2026-09-24: `__builtin_unreachable();` was
// S_UndeclaredIdentifier on pe64 and ELF x86_64).
//
// ★ WHAT IS PINNED, read after the pipeline's own next step — the unreachable-block
// prune, which drops the dead blocks each seal opened and KEEPS the builtin's
// (control reaches it; it is the trap): in `k` — `return x > 0 ? x : (__builtin_
// unreachable(), 0);`, EXPRESSION position — the else-arm ends in `Unreachable`,
// which only the builtin's own lowering can put there; in `f` — `if (x == 7)
// __builtin_unreachable(); return x + 1;` — exactly ONE block ends in
// `Unreachable`, a successor of the entry block's conditional branch; the
// builtin-free twin `g` has NONE; and in `h` a statement-position builtin in the
// MIDDLE of a block seals that block. The MIR verifier then accepts the module.
//
// RED-ON-DISABLE: map the builtin to a no-op lowering (no terminator) and `k` —
// the EXPRESSION-position case — has ZERO `Unreachable` blocks: its else-arm falls
// through into the join, whatever any example's exit code says. (The statement
// shapes f and h also carry the HIR `Unreachable` leaf the noreturn wrap adds, so
// they cannot tell a no-op lowering apart; k is the pin's teeth.)

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/tree.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_verifier.hpp"
#include "opt/passes/prune_unreachable.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

struct Lowered {
    std::shared_ptr<GrammarSchema const> schema;
    std::shared_ptr<TargetSchema const>  target;   // outlives `model`, which republishes it
    SemanticModel                        model;
    std::unique_ptr<CstToHirResult>      hir;
    HirToMirResult                       mir;
    DiagnosticReporter                   mirReporter;
};

// The C front end through MIR, the way the shipped pipeline runs it (the
// test_deep_nesting_costs_heap.cpp fixture, without its depth knobs).
[[nodiscard]] Lowered lowerC(std::string src) {
    auto loaded = GrammarSchema::loadShipped("c");
    if (!loaded) throw std::runtime_error{"loadShipped(c) failed"};
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t) throw std::runtime_error{"loadShipped(x86_64) failed"};
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    std::shared_ptr<TargetSchema const>  target = *t;

    auto srcBuf = SourceBuffer::fromString(std::move(src), "<unreachable>");
    Tokenizer tk{srcBuf, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    ParserConfig pcfg;
    if (auto cap = schema->maxSpeculationDepth())     pcfg.maxSpeculationDepth = *cap;
    if (auto f   = schema->speculationBudgetFactor()) pcfg.speculationBudgetFactor = *f;
    Parser p{srcBuf, schema, std::move(stream), DiagnosticBudget::libraryDefault(),
             std::move(pcfg), std::move(lexDiags)};
    ParseResult result = std::move(p).parse();
    if (result.tree.diagnostics().hasErrors())
        throw std::runtime_error{"the fixture did not parse cleanly"};
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addTree(std::move(result.tree));
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, std::nullopt,
                std::nullopt, std::nullopt, std::nullopt, LongDoubleFormat::None, target.get(),
                std::size_t{1} * 1024 * 1024);
    if (model.hasErrors()) {
        std::string why;
        for (auto const& d : model.diagnostics().all())
            if (d.severity == DiagnosticSeverity::Error) {
                why = std::string{diagnosticCodeName(d.code)} + " got '" + d.actual + "'";
                break;
            }
        throw std::runtime_error{"the fixture did not analyze cleanly: " + why};
    }
    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    Lowered out{.schema = schema, .target = target, .model = std::move(model),
                .hir = std::move(hir), .mir = {}, .mirReporter = {}};
    MirLoweringConfig mirCfg;
    mirCfg.globalsAllowFloat     = schema->hirLowering().globalsConstEval.allowFloat;
    mirCfg.nonObjectTypeSizes    = schema->semantics().nonObjectTypeSizes;
    mirCfg.aggregateLayout       = target->aggregateLayout();
    mirCfg.aggregateLayoutLoaded = target->aggregateLayoutLoaded();
    out.mir = lowerToMir(out.hir->hir, out.hir->literalPool, out.model.lattice().interner(),
                         out.mirReporter, &out.hir->sourceMap, mirCfg, /*ffiMap=*/nullptr,
                         &out.hir->linkageMap, &out.hir->mutabilityMap, &out.hir->volatileMap,
                         /*alignmentMap=*/nullptr, &out.hir->threadLocalMap,
                         &out.hir->vlaSizeExprBySymbol, &out.hir->sizeofVlaSymbol,
                         &out.hir->typedefVlaOriginBySymbol, &out.hir->synthRecipeBySymbol,
                         &out.hir->returnsTwiceMap, &out.hir->noInlineMap,
                         &out.hir->alwaysInlineMap, &out.hir->noOptimizeMap,
                         &out.hir->noSanitizeThreadMap, &out.hir->inlineAsmPool,
                         /*inlineDefinitionMap=*/nullptr, &out.hir->enclosingFunctionMap);
    return out;
}

// The MIR function a C name was lowered to (the semantic record names it).
[[nodiscard]] MirFuncId findFunc(Lowered const& L, std::string_view name) {
    Mir const& mir = L.mir.mir;
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (auto const* rec = L.model.recordFor(mir.funcSymbol(f)); rec != nullptr && rec->name == name)
            return f;
    }
    return MirFuncId{};
}

// The blocks of `f` whose LAST instruction is MIR's `Unreachable` terminator.
[[nodiscard]] std::vector<MirBlockId> unreachableBlocks(Mir const& mir, MirFuncId f) {
    std::vector<MirBlockId> out;
    for (std::uint32_t i = 0; i < mir.funcBlockCount(f); ++i) {
        MirBlockId const b = mir.funcBlockAt(f, i);
        std::uint32_t const n = mir.blockInstCount(b);
        if (n != 0 && mir.instOpcode(mir.blockInstAt(b, n - 1)) == MirOpcode::Unreachable) out.push_back(b);
    }
    return out;
}

}  // namespace

TEST(BuiltinUnreachableLowering, TheBuiltinEndsItsBlockWithTheUnreachableTerminator) {
    Lowered L = lowerC(
        // the builtin in a branch, statement position: its OWN block must end in Unreachable
        "int f(int x) { if (x == 7) __builtin_unreachable(); return x + 1; }\n"
        // the control: the same shape without it ends no block in Unreachable
        "int g(int x) { if (x == 7) { } return x + 1; }\n"
        // mid-block, statement position: it seals the block it is in
        "int h(int x) { int y = x * 2; __builtin_unreachable(); y = y + 1; return y; }\n"
        // EXPRESSION position: no statement wrap reaches it, so the builtin's OWN
        // lowering is the only terminator this arm can have
        "int k(int x) { return x > 0 ? x : (__builtin_unreachable(), 0); }\n");
    ASSERT_TRUE(L.mir.ok);
    ASSERT_FALSE(L.mirReporter.hasErrors());

    // The pipeline's own next step (compile_pipeline.cpp): the unreachable-block prune drops
    // the DEAD blocks each seal opened, and must KEEP the builtin's block — control reaches
    // it, and it IS the trap. It REBUILDS the module (fresh ids), so functions are found by
    // name afterwards.
    DiagnosticReporter prep;
    ASSERT_TRUE(opt::passes::runPruneUnreachableBlocks(L.mir.mir, L.model.lattice().interner(), prep).ok);
    Mir const& mir = L.mir.mir;
    MirFuncId const f = findFunc(L, "f");
    MirFuncId const g = findFunc(L, "g");
    MirFuncId const h = findFunc(L, "h");
    MirFuncId const k = findFunc(L, "k");
    ASSERT_TRUE(f.valid() && g.valid() && h.valid() && k.valid());

    // EXPRESSION position first — the no-fall-through property itself: nothing but the
    // builtin's own lowering can end this arm, so a no-op lowering leaves `k` with none.
    EXPECT_EQ(unreachableBlocks(mir, k).size(), 1u)
        << "k: in expression position only the builtin's OWN lowering ends the else-arm; without it "
           "the arm falls through into the join";

    auto const fu = unreachableBlocks(mir, f);
    ASSERT_EQ(fu.size(), 1u) << "f: the builtin's branch must end in MIR's Unreachable terminator";
    auto const succ = mir.blockSuccessors(mir.funcEntry(f));
    EXPECT_NE(std::find(succ.begin(), succ.end(), fu[0]), succ.end())
        << "f: the Unreachable block must be the builtin's OWN block — a successor of the entry "
           "block's conditional branch";
    EXPECT_TRUE(unreachableBlocks(mir, g).empty())
        << "g: the control without the builtin must end no block in Unreachable";
    auto const hu = unreachableBlocks(mir, h);
    ASSERT_EQ(hu.size(), 1u);
    EXPECT_EQ(hu[0], mir.funcEntry(h)) << "h: a statement-position builtin mid-block seals the block it is in";

    DiagnosticReporter vrep;
    MirVerifier verifier{mir, &L.model.lattice().interner()};
    bool const verified = verifier.verify(vrep);
    std::string why;
    for (auto const& d : vrep.all()) why += "\n    " + d.actual;
    EXPECT_TRUE(verified) << "after the prune the module must verify:" << why;
}
