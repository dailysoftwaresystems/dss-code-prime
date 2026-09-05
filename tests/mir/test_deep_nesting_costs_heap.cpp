// ── DEPTH MUST COST HEAP, NOT HOST CALL FRAMES ──────────────────────────────
//
// The operator's standing ruling of 2026-09-02: *"it's well known to not use
// recursive structures in the compiler because big projects like sqlite will for
// sure explode the stack"*. These are the MIR tier's pins for it.
//
// ★★ EVERY TEST HERE RUNS ON THE ORDINARY gtest MAIN THREAD (~1 MiB), AND THAT
// IS THE WHOLE POINT. `src/program/program.cpp` builds every CU inside
// `substrate::callOnLargeStack(kDeepRecursionStackBytes, …)` — a 64 MiB worker —
// so the same input measured THROUGH THE CLI reports that everything is fine
// while a library embedder, an LSP, or a test binary crashes. A pin taken on the
// large-stack worker proves nothing about this class. Nothing in this file calls
// `callOnLargeStack`, and `analyze` is handed a small BOUNDED reserve so its own
// internal worker cannot hide the very thing being measured.
//
// ★★ ITS OWN BINARY ON PURPOSE. When one of these reds it reds by EXHAUSTING THE
// STACK — the process dies at 0xC00000FD with no `[  FAILED  ]` line and no case
// name, exactly the unattributable signature `no_abort_in_tests_guard` exists to
// keep out of shared binaries. Isolated here, `ctest` at least names this
// executable and no sibling test loses its verdict.
//
// Rows: D-MIR-TEXT-APPENDTYPE-RECURSES-PER-TYPE-LEVEL-AND-NEVER-TERMINATES-ON-A-SELF-REFERENTIAL-COMPOSITE
//       D-MIR-HIRTOMIR-WHOLE-BODY-PRE-PASSES-RECURSE-PER-HIR-LEVEL
//       D-MIR-NESTED-AGGREGATE-LITERAL-WALKS-RECURSE-PER-INITIALIZER-LEVEL
//       D-MIR-PROVABLE-LVALUE-ALIGN-TRUNCATES-SILENTLY-TOWARD-ALIGNED
//       D-MIR-AGGREGATE-INIT-INTO-SLOT-RECURSES-PER-INITIALIZER-LEVEL

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
#include "mir/mir_literal_pool.hpp"   // forEachLiteralNode — the shared heap walker
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/strong_ids.hpp"
#include "hir/hir_literal_pool.hpp"
#include "hir/hir_node.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_text.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include "../core/bounded_stack.hpp"   // runOnBoundedStack — the BOUND (see there)

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <variant>
#include <vector>
#include <optional>
#include <string>
#include <utility>

using namespace dss;

namespace {

// Everything one of these pins needs, lowered from C source on THIS thread.
struct Lowered {
    std::shared_ptr<GrammarSchema const> schema;
    std::shared_ptr<TargetSchema const>  target;
    SemanticModel                        model;
    std::unique_ptr<CstToHirResult>      hir;
    HirToMirResult                       mir;
};

// ⚠ THE ANALYSIS RESERVE IS SMALL AND BOUNDED ON PURPOSE. `analyze` runs its
// implementation on a dedicated worker whose default reserve is 64 MiB; left at
// the default it would absorb a per-level recursion in a stage under test and
// report a false green. 1 MiB is the ORDINARY thread's own size, so the analysis
// is held to the same budget as everything else in this file.
constexpr std::size_t kOrdinaryThreadReserveBytes = std::size_t{1} * 1024 * 1024;

// The shipped schemas, loaded ONCE and on the MAIN thread. Every bounded pin
// below calls this before spawning its small-stack worker: the loader's cost is
// large and input-INDEPENDENT (`bounded_stack.hpp` says why), so it must not
// be what a bounded stage pays for.
struct ShippedSchemas {
    std::shared_ptr<GrammarSchema const> schema;
    std::shared_ptr<TargetSchema const>  target;
};
[[nodiscard]] ShippedSchemas const& shipped() {
    static ShippedSchemas const loadedOnce = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded) throw std::runtime_error{"loadShipped(c) failed"};
        auto t = TargetSchema::loadShipped("x86_64");
        if (!t) throw std::runtime_error{"loadShipped(x86_64) failed"};
        return ShippedSchemas{.schema = *loaded, .target = *t};
    }();
    return loadedOnce;
}

// ⚠ A THROW, NEVER `std::abort()` — an abort kills the process and every sibling
// here loses its verdict, which is the exact signature these tests exist to tell
// APART from a stack overflow. `no_abort_in_tests_guard` enforces the same rule.
[[nodiscard]] Lowered lowerC(std::string src, std::size_t exprDepthCap) {
    std::shared_ptr<GrammarSchema const> schema = shipped().schema;
    std::shared_ptr<TargetSchema const>  target = shipped().target;

    // Tokenize + parse on THIS thread. The parser's residual paren/postfix arm is
    // a separate, still-recursive site (plan-24 Stage 5b, capped by
    // `maxExpressionDepth`); none of the shapes below nest parentheses, and the
    // cap is raised past the fixture depth so a loud refusal cannot be mistaken
    // for the property under test.
    //
    // ⚠⚠ THE OTHER TWO PARSER CAPS COME FROM THE LANGUAGE SCHEMA, NOT FROM THE
    // `ParserConfig` C++ FALLBACKS, AND THAT WAS A LIVE INSTRUMENT DEFECT. A
    // bare `ParserConfig pcfg;` leaves `speculationBudgetFactor` at 16 while
    // `c.lang.json` declares 64 and `maxSpeculationDepth` at its fallback while
    // the config declares 320 — so this fixture parsed with a STRICTER parser
    // than the shipped compiler, and a construct the CLI compiles was refused
    // here as `P_SpeculationBudgetExhausted`. ✔MEASURED (P56, lane `rs`): a
    // 100-element comma chain compiled rc 0 through `dsscp` and was refused in
    // this fixture, i.e. the fixture reported a front-end refusal for a MIR-tier
    // probe — the "an instrument that answers an adjacent question" shape, and
    // it fails toward *the recursion never ran*. Seeded from the schema the way
    // `compilation_unit.cpp`'s `parserConfigFor` does it; `exprDepthCap` still
    // overrides afterwards because the depth is what each fixture is choosing.
    auto srcBuf = SourceBuffer::fromString(std::move(src), "<deepnest>");
    Tokenizer tk{srcBuf, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    ParserConfig pcfg;
    if (auto cap = schema->maxSpeculationDepth())     pcfg.maxSpeculationDepth = *cap;
    if (auto f   = schema->speculationBudgetFactor()) pcfg.speculationBudgetFactor = *f;
    pcfg.maxExpressionDepth = exprDepthCap;
    Parser p{srcBuf, schema, std::move(stream), DiagnosticBudget::libraryDefault(),
             std::move(pcfg), std::move(lexDiags)};
    ParseResult result = std::move(p).parse();
    // ⚠ THE REFUSAL MUST SAY WHAT REFUSED IT. A bare "did not parse cleanly"
    // reads as "the fixture is too deep for the site under test" when it can
    // equally mean a CAP fired somewhere else entirely and the recursion never
    // ran — the exact instrument defect the recursion census records twice
    // (a green, or a red, that means *the code never executed*). Naming the
    // first error is what tells those two apart without a second build.
    if (result.tree.diagnostics().hasErrors()) {
        std::string why;
        for (auto const& d : result.tree.diagnostics().all()) {
            if (d.severity != DiagnosticSeverity::Error) continue;
            why = std::string{diagnosticCodeName(d.code)} + " got '" + d.actual + "'";
            break;
        }
        throw std::runtime_error{"deep-nest fixture did not parse cleanly: " + why};
    }

    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addTree(std::move(result.tree));
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());

    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                LongDoubleFormat::None, target.get(),
                kOrdinaryThreadReserveBytes);
    if (model.hasErrors()) {
        std::string why;
        for (auto const& d : model.diagnostics().all()) {
            if (d.severity != DiagnosticSeverity::Error) continue;
            why = std::string{diagnosticCodeName(d.code)} + " got '" + d.actual + "'";
            break;
        }
        throw std::runtime_error{"deep-nest fixture did not analyze cleanly: " + why};
    }

    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);

    DiagnosticReporter mirReporter;
    MirLoweringConfig mirCfg;
    mirCfg.globalsAllowFloat  = schema->hirLowering().globalsConstEval.allowFloat;
    mirCfg.nonObjectTypeSizes = schema->semantics().nonObjectTypeSizes;
    mirCfg.aggregateLayout       = target->aggregateLayout();
    mirCfg.aggregateLayoutLoaded = target->aggregateLayoutLoaded();
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap, mirCfg, /*ffiMap=*/nullptr,
                                    &hir->linkageMap, &hir->mutabilityMap,
                                    &hir->volatileMap, /*alignmentMap=*/nullptr,
                                    &hir->threadLocalMap,
                                    &hir->vlaSizeExprBySymbol,
                                    &hir->sizeofVlaSymbol,
                                    &hir->typedefVlaOriginBySymbol,
                                    &hir->synthRecipeBySymbol,
                                    &hir->returnsTwiceMap,
                                    &hir->noInlineMap,
                                    &hir->alwaysInlineMap,
                                    &hir->noOptimizeMap,
                                    &hir->noSanitizeThreadMap,
                                    &hir->inlineAsmPool,
                                    /*inlineDefinitionMap=*/nullptr,
                                    &hir->enclosingFunctionMap);
    return Lowered{.schema = std::move(schema),
                   .target = std::move(target),
                   .model  = std::move(model),
                   .hir    = std::move(hir),
                   .mir    = std::move(mir)};
}

} // namespace

// ── THE SELF-REFERENTIAL COMPOSITE — the row is
// D-MIR-TEXT-APPENDTYPE-RECURSES-PER-TYPE-LEVEL-AND-NEVER-TERMINATES-ON-A-SELF-REFERENTIAL-COMPOSITE
//
// ★★★ THE DEFECT WAS NOT DEPTH, IT WAS NON-TERMINATION, AND THE SHAPE IS THE
// COMMONEST ONE IN C. `appendType` expands a composite's whole field list inline;
// `struct S { int v; struct S *next; }` interns a field that points back at the
// complete `S`, so `S → fields → ptr<S> → fields → …` had no base case.
//
// ✔MEASURED before the fix, through `ctest`, on the ordinary thread: this exact
// source lowered to MIR with rc 0 and then died with rc 8 / SEGFAULT the moment
// `emitMir` ran. A finite ACYCLIC type graph here is about three levels deep, so
// the crash is proof of a cycle rather than of a deep input — no stack size fixes
// it, and neither does a depth cap that merely truncates.
//
// The contract now: the emit COMPLETES, the self-reference is refused BY NAME
// with an `Error`, and the text carries the `?` mark that `parseType` rejects on
// the way back in — never a quietly truncated type that would read back as a
// DIFFERENT struct.
TEST(MirTextDeepNesting, SelfReferentialCompositeIsRefusedLoudNotACrash) {
    auto L = lowerC("struct S { int v; struct S *next; };\n"
                    "struct S g;\nint main(void){ return g.v; }",
                    /*exprDepthCap=*/1024);

    DiagnosticReporter reporter;
    MirTextContext ctx;
    ctx.interner = &L.model.lattice().interner();
    std::string const text = emitMir(L.mir.mir, ctx, reporter);

    // It came back at all. Before the fix control never returned from here.
    EXPECT_FALSE(text.empty());
    // The composite IS still named — the refusal must say WHAT it refused.
    EXPECT_NE(text.find("struct \"S\""), std::string::npos);
    // …and the self-reference carries the reader-refused mark, so the text
    // cannot silently round-trip as a struct whose `next` lost its pointee.
    EXPECT_NE(text.find("struct \"S\" ?"), std::string::npos)
        << "a self-referential composite must render with the '?' mark the "
           "reader refuses, never as a silently truncated type";
    // LOUD: an Error, not a warning and not silence.
    EXPECT_GT(reporter.errorCount(), 0u)
        << "refusing to render a self-referential composite must be an Error";
}

// The complement: a type that is merely DEEP must still render completely, so the
// test above cannot be satisfied by refusing composites in general — AND it is
// the actual DEPTH pin for `appendType`.
//
// ★★★ THE DEPTH AND THE SHAPE ARE BOTH MEASURED, AND THE FIRST VERSION OF THIS
// TEST WAS A DECORATION. It nested 600 composites, and a supplementary
// red-on-disable arm (`GTEST_FILTER` narrowed to this case, so the sibling
// crash could not mask it) MEASURED that 600 PASSES against the recursive
// `appendType` — nowhere near a ~1 MiB ceiling. A pin that stays green when the
// fix is removed is not a pin.
//
// ✔MEASURED on the ordinary thread with the RECURSIVE form in the tree, through
// `ctest`, walking the depth through the real entry point:
//   * a POINTER CHAIN (`int ***…*p;` → `ptr<ptr<…<i32>…>>`, one `appendType`
//     level per star) renders at 600 with rc 0 and SEGFAULTS at 2000 and 4000,
//     dying in the TEXT stage;
//   * at 8000 it still SEGFAULTS but in `lowerToHir` instead — a DIFFERENT and
//     still-unconverted site, which would make the red mean the wrong thing.
// ⇒ the usable window is 2000 < D < 8000, and kDepth is 4000: 2x past the
// measured crash floor for the recursion this file is about, and 2x below the
// unrelated ceiling that would confuse the verdict.
//
// ★ A POINTER CHAIN RATHER THAN NESTED STRUCTS ON PURPOSE — the front end walks
// a star chain iteratively (`declarator_walk.hpp` is a bounded `for`, not
// recursion), so the depth can go where `appendType` needs it without the parser
// or the semantic pass becoming the thing that dies.
TEST(MirTextDeepNesting, DeeplyNestedTypeRendersWholeOnAnOrdinaryThread) {
    constexpr int kDepth = 4000;   // 2x the MEASURED recursive crash floor (2000)

    std::string src = "int ";
    src.append(static_cast<std::size_t>(kDepth), '*');
    src += "p;\nint main(void){ return 0; }";

    auto L = lowerC(std::move(src), /*exprDepthCap=*/kDepth + 1024);

    DiagnosticReporter reporter;
    MirTextContext ctx;
    ctx.interner = &L.model.lattice().interner();
    std::string const text = emitMir(L.mir.mir, ctx, reporter);

    // Every level must be present. A truncated render is not a cosmetic defect:
    // `ptr<ptr<i32>>` and `ptr<i32>` are DIFFERENT types, and this text is what
    // `parseType` reads back.
    std::size_t levels = 0;
    for (std::size_t at = text.find("ptr<"); at != std::string::npos;
         at = text.find("ptr<", at + 1)) {
        ++levels;
    }
    EXPECT_GE(levels, static_cast<std::size_t>(kDepth))
        << "every pointer level must reach the text — a truncated render is a "
           "silently different type";
    // No `?` anywhere: nothing was refused, so this is a clean, complete render.
    EXPECT_EQ(text.find('?'), std::string::npos)
        << "a merely-deep type must render completely, not be refused";
    EXPECT_EQ(reporter.errorCount(), 0u);
}

// ── D-MIR-HIRTOMIR-WHOLE-BODY-PRE-PASSES-RECURSE-PER-HIR-LEVEL ───────────────
//
// Plan-24 flattened `lowerExpr` / `lowerLvalueAddress` / `lowerStmt` onto explicit
// work stacks and left the whole-body SCANS that run before them recursive —
// `collectAddressTakenSymbols`, `collectSehFilterReferencedSymbols`,
// `collectRefSymbols`, `collectAddressTakenLabels`, `collectLabelNodes`,
// `collectLocalDecls`, none of them capped. The flattened driver was therefore
// never what decided the ceiling.
//
// ✔MEASURED before the fix, on the ordinary thread, through `ctest`: nested `if`
// statements lowered with rc 0 at 2048 and SEGFAULTed at 2560, with the stage
// marker showing the death inside `lowerToMir`. After the conversion the same
// walk reached 16384 with rc 0.
//
// kDepth is set ABOVE the pre-fix crash ceiling on purpose: restore any one of
// those six walks to host recursion and this test does not merely fail, it dies.
// ★ ON THE BOUNDED STACK (`tests/core/bounded_stack.hpp`, 256 KiB) since P60:
// the six pre-passes' recursion cost ~400 bytes per level on mingw-w64 g++
// Debug (2560 levels ≈ 1 MiB), so 6000 levels need ≥ 2.3 MiB against 256 KiB
// — and `pathTerminates`, which also walks this nest inside `lowerToHir`,
// needed 780 KiB at its thinnest. Restore either and the process dies.
TEST(HirToMirDeepNesting, DeeplyNestedStatementsLowerOnABoundedStack) {
    constexpr int kDepth = 6000;   // ~2.3x the MEASURED pre-fix ceiling of ~2560
    (void)shipped();
    std::string src = "int main(void){ int x=0; ";
    for (int i = 0; i < kDepth; ++i) src += "if(x){ ";
    src += "return 1;";
    for (int i = 0; i < kDepth; ++i) src += " }";
    src += " return 0; }";

    std::size_t funcs = 0;
    test::runOnBoundedStack([&] {
        auto L = lowerC(std::move(src), /*exprDepthCap=*/kDepth + 1024);
        funcs = L.mir.mir.funcCount();
    });
    // It lowered at all — the whole-body pre-passes walked 6000 HIR levels on
    // a 256 KiB thread. Assert the module is real, not just that nothing
    // crashed.
    EXPECT_GT(funcs, 0u);
}

// ── D-MIR-NESTED-AGGREGATE-LITERAL-WALKS-RECURSE-PER-INITIALIZER-LEVEL ───────
//
// A `MirAggregateValue`'s fields are themselves literals, so a literal is a tree
// whose depth is the brace nesting of the initializer. Seven walks over that tree
// existed across `src/mir/**`, every one host recursion and every one uncapped;
// five now share the heap walker in `mir/mir_literal_pool.hpp` and the writer's
// own `appendLiteral` has its own stack. This drives the writer's half
// end-to-end.
//
// ⚠⚠ **THIS IS A CORRECTNESS PIN, NOT A DEPTH PIN, AND THE REASON IS MEASURED.**
// ✔MEASURED on the ordinary thread with the RECURSIVE writer in the tree: a
// nested array initializer renders at 400 with rc 0, and at 1000 the process
// SEGFAULTS inside `lowerToMir` — in the UNCONVERTED aggregate-init cluster
// (`lowerAggregateInitIntoSlot` / `lowerArrayInitIntoSlot` and their two
// siblings), not in the writer. That ceiling sits BELOW the recursive
// `appendLiteral`'s own, so `appendLiteral`'s depth is simply NOT REACHABLE from
// C source today: no choice of kDepth reds the writer without a shallower,
// different site dying first. What this test therefore pins is that the flattened
// writer still renders the WHOLE literal — a truncated initializer is a
// miscompile, not a dump bug — and the DEPTH half of the class is pinned by
// `MirLiteralWalk.HundredThousandLevelsCostHeapNotCallFrames` on the shared
// walker instead. Raising kDepth here becomes meaningful only once that
// aggregate-init cluster is converted.
TEST(MirTextDeepNesting, DeeplyNestedAggregateLiteralRendersOnAnOrdinaryThread) {
    constexpr int kDepth = 400;
    std::string src;
    for (int i = 0; i < kDepth; ++i) src += "struct S" + std::to_string(i) + " { ";
    src += "int x;";
    for (int i = kDepth - 1; i >= 0; --i) src += " } m" + std::to_string(i) + ";";
    src += "\nstruct S0 g = ";
    for (int i = 0; i < kDepth; ++i) src += "{";
    src += "3";
    for (int i = 0; i < kDepth; ++i) src += "}";
    src += ";\nint main(void){ return 0; }";

    auto L = lowerC(std::move(src), /*exprDepthCap=*/kDepth + 1024);

    DiagnosticReporter reporter;
    MirTextContext ctx;
    ctx.interner = &L.model.lattice().interner();
    std::string const text = emitMir(L.mir.mir, ctx, reporter);

    // The innermost value survived the walk — a truncated literal would be a
    // silently different initializer, which is a miscompile and not a dump bug.
    EXPECT_NE(text.find("lit int 3"), std::string::npos)
        << "the innermost initializer value must reach the text";
    EXPECT_EQ(reporter.errorCount(), 0u);
}

// ── THE SHARED LITERAL WALKER ITSELF ─────────────────────────────────────────
//
// `forEachLiteralNode` (mir/mir_literal_pool.hpp) is the ONE owner the five
// converted call sites route through — the merge's `remapLiteralSymbols` and
// `assignLiteralSymbols`, the summary's global-init `collect`, and the lazy
// import's `collectLiteralSymbolNames` and `noteLiteral`. Four of those five are
// file-static or lambdas with no reachable seam, so they cannot be driven from a
// test directly; pinning their shared traversal is what CAN be pinned, and the
// property that matters — that none of them carries a recursion of its own any
// more — is a grep, not a behaviour.
//
// ⚠ SO THIS PIN IS DELIBERATELY DEEPER THAN ANY SOURCE COULD PRODUCE. 100_000
// levels is far past the point where one host frame per level survives ANY
// thread, and it costs nothing on the heap. It also asserts FIELD ORDER, because
// the merge numbers symbols in visit order and the summary's target vector is
// order-observable — a walker that flattened correctly but reversed would be a
// silent renumbering, not a crash.
TEST(MirLiteralWalk, HundredThousandLevelsCostHeapNotCallFrames) {
    constexpr int kDepth = 100000;

    // Build `agg{ agg{ … agg{ int 1, int 2 } … } }` bottom-up, iteratively — the
    // FIXTURE must not recurse either, or it would red for its own reason.
    MirLiteralValue leafA;
    leafA.value = std::int64_t{1};
    leafA.core  = TypeKind::I64;
    MirLiteralValue leafB;
    leafB.value = std::int64_t{2};
    leafB.core  = TypeKind::I64;
    MirLiteralValue cur;
    {
        MirAggregateValue bottom;
        bottom.fields.push_back(leafA);
        bottom.fields.push_back(leafB);
        cur.value = std::move(bottom);
        cur.core  = TypeKind::I64;
    }
    for (int i = 0; i < kDepth; ++i) {
        MirAggregateValue up;
        up.fields.push_back(std::move(cur));
        MirLiteralValue next;
        next.value = std::move(up);
        next.core  = TypeKind::I64;
        cur = std::move(next);
    }

    std::size_t visited = 0;
    std::vector<std::int64_t> leaves;
    int copiedLevels = 0;
    // ★ THE WALK, THE COPY AND THE TEARDOWN ALL RUN ON THE BOUNDED STACK
    // (`tests/core/bounded_stack.hpp`, 256 KiB): each of the three cost one
    // host frame chain per level before its conversion, and at 100_000 levels
    // any of them needs megabytes. Restore any one and the process dies.
    test::runOnBoundedStack([&] {
        forEachLiteralNode(std::as_const(cur), [&](MirLiteralValue const& n) {
            ++visited;
            if (auto const* iv = std::get_if<std::int64_t>(&n.value)) {
                leaves.push_back(*iv);
            }
        });

        // ★★ THE COPY IS A WALK TOO (P60). `MirAggregateValue`'s copy
        // constructor was the compiler's own — `vector` copy → variant copy →
        // nested aggregate copy, one host frame chain per level. ✔MEASURED
        // 2026-09-04 (lane `rc`), gdb-attributed under MSVC 19.51 Debug:
        // `emitGlobals_` copying a 1000-level pool literal died 13 848 frames
        // deep in it, once the teardown had stopped being the first walk to
        // overflow. Copy the whole value and walk the copy ITERATIVELY to prove
        // it is complete — a copy that stopped short would be a silently
        // different initializer.
        MirLiteralValue const copy = cur;
        MirLiteralValue const* node = &copy;
        // kDepth single-field levels, then the two-leaf bottom aggregate.
        while (auto const* agg = std::get_if<MirAggregateValue>(&node->value)) {
            if (agg->fields.size() != 1) break;
            node = &agg->fields[0];
            ++copiedLevels;
        }
        if (auto const* bottom = std::get_if<MirAggregateValue>(&node->value);
            bottom != nullptr && bottom->fields.size() == 2) {
            ++copiedLevels;   // the bottom `agg{ int 1, int 2 }` arrived too
        }

        // ★★ `cur` AND `copy` DIE HERE, AT FULL DEPTH, ON PURPOSE — THE
        // TEARDOWN IS A THIRD PIN. A nested `MirLiteralValue`'s DESTRUCTOR is a
        // walk the standard library generates (`~variant → ~MirAggregateValue
        // → ~vector → ~MirLiteralValue`), and this case used to UNLINK the
        // spine by hand before letting it die because that walk cost one host
        // frame chain per level. Since P56
        // (D-MIR-LITERAL-VALUE-TEARDOWN-RECURSES-PER-AGGREGATE-LEVEL)
        // `mir_literal_pool.hpp` tears an aggregate down on an explicit heap
        // work list, so the unlink was a workaround for a defect that no longer
        // exists — and keeping it would have hidden a REGRESSION of that fix
        // behind a green (✔the row records 2000 ok / 3000 SEGFAULT under
        // mingw-w64 g++ Debug for the recursive form).
        MirLiteralValue drop = std::move(cur);
        (void)drop;
    });

    // kDepth+1 aggregates plus the two leaves.
    EXPECT_EQ(visited, static_cast<std::size_t>(kDepth) + 3u);
    ASSERT_EQ(leaves.size(), 2u);
    EXPECT_EQ(leaves[0], 1);
    EXPECT_EQ(leaves[1], 2)
        << "fields must be visited in declaration order — the merge assigns "
           "symbol numbers in this order and the summary records it";
    EXPECT_EQ(copiedLevels, kDepth + 1)
        << "the copy must reproduce every aggregate level — a short copy is a "
           "silently different initializer";
}

// ── D-MIR-PROVABLE-LVALUE-ALIGN-TRUNCATES-SILENTLY-TOWARD-ALIGNED ────────────
//
// ★★★ THIS ONE IS NOT A CRASH PIN — IT IS A WRONG-ANSWER PIN, AND THAT IS WHY
// IT MATTERS MORE THAN THE DEPTH PINS ABOVE.
//
// `provableLvalueAlign` walked the lvalue chain recursively and gave up at
// `depth > 64` by returning 0. In this pipeline 0 does NOT mean "be careful": it
// is the `payload2` sentinel `mir_to_lir`'s `atomicAccessIsUnderAligned` reads as
// `if (provable == 0) return false` — TREAT AS ALIGNED. So an `_Atomic` member
// that genuinely IS under-aligned kept the target's native inline atomic pair
// (`ldar`/`stlr` on arm64) instead of the format's `__atomic_*` runtime, purely
// because the member chain was longer than 64 links. The runtime witness for
// that mistake is `examples/c/packed_atomic_member/`: `rc 135, Bus error` on a
// NATIVE aarch64 host, and — the reason it stayed invisible — `rc 42` under
// qemu, on Windows, and in WSL, because three of the four gate legs cannot
// enforce the alignment check at all.
//
// ⇒ The walk is iterative and the cap is GONE, so depth cannot change the
// answer. The fixture is a 70-link chain (past the old 64) whose OUTERMOST
// struct is `packed` with a leading `char`, which drops the whole nest — and the
// `_Atomic int` at the bottom of it — onto an odd byte.
//
// ⚠ THE SHALLOW ARM IS THE CONTROL AND THE TEST IS WORTHLESS WITHOUT IT. A
// "fix" that simply answered "under-aligned" more often would pass the deep arm
// and be wrong; a fix that answered it LESS often would pass nothing. The
// shallow packed access must report the SAME under-alignment, and the shallow
// UNPACKED access must report a fully-aligned 4.
namespace {

// The alignment `payload2` of the first AtomicLoad in the module, or nullopt if
// the module has none (which is itself a fixture failure worth naming).
[[nodiscard]] std::optional<std::uint32_t> firstAtomicLoadAlign(Mir const& m) {
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
        MirFuncId const f = m.funcAt(fi);
        for (std::uint32_t bi = 0; bi < m.funcBlockCount(f); ++bi) {
            MirBlockId const bb = m.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < m.blockInstCount(bb); ++ii) {
                MirInstId const inst = m.blockInstAt(bb, ii);
                if (m.instOpcode(inst) == MirOpcode::AtomicLoad)
                    return m.instPayload2(inst);
            }
        }
    }
    return std::nullopt;
}

// `struct __attribute__((packed)) W { char pad; struct S_{N-1} m; };` over a
// chain of `struct S_i { struct S_{i-1} m; };` bottoming out at `_Atomic int a`.
// Every inner link sits at offset 0, so the ONLY term that makes the access
// under-aligned is the packed wrapper's leading `char` — i.e. the term at the
// DEEPEST end of the walk, the one a depth cap truncates away first.
[[nodiscard]] std::string packedAtomicChainSource(int links, bool packed) {
    std::string s = "struct S0 { _Atomic int a; };\n";
    for (int i = 1; i < links; ++i) {
        s += "struct S" + std::to_string(i) + " { struct S" +
             std::to_string(i - 1) + " m; };\n";
    }
    s += packed ? "struct __attribute__((packed)) W { char pad; struct S"
                : "struct W { char pad; struct S";
    s += std::to_string(links - 1) + " m; };\nstruct W w;\n";
    s += "int main(void){ return w.m";
    for (int i = links - 1; i >= 1; --i) s += ".m";
    s += ".a; }";
    return s;
}

} // namespace

// ── D-MIR-AGGREGATE-INIT-INTO-SLOT-RECURSES-PER-INITIALIZER-LEVEL ────────────
//
// `lowerAggregateInitIntoSlot` ⇄ `lowerArrayInitIntoSlot` ⇄
// `lowerBitfieldAggregateInitIntoSlot` were one mutual host recursion, one frame
// per BRACE LEVEL, uncapped. They now share one explicit frame stack.
//
// ★★★ THIS PIN IS BUILT FROM HAND-WRITTEN HIR, AND EVERY PART OF THAT IS
// FORCED — a C-source pin here would be a DECORATION and it was MEASURED to be
// one. ✔MEASURED on the ordinary thread through `ctest`, with the RECURSIVE
// cluster in the tree:
//   * a LOCAL `int a[1][1]…[1] = {{{…}}}` from C source survives 1000 and dies
//     at 2000 — in `cst_to_hir.cpp`'s `lowerBraceInit`, not here (gdb);
//   * a GLOBAL `struct S0 g = {{{…}}}` survives 400 and dies at 1000 — in
//     `src/hir/const_eval.cpp`'s `evalNode` ⇄ `evalImpl`, not here (gdb);
//   * hand-built HIR whose TYPES nest too dies at 2000 — in `computeLayout`
//     (`src/core/types/…/type_layout.cpp`), not here.
// Every route to this cluster is gated by a SHALLOWER recursion in somebody
// else's file, so no source and no naturally-typed fixture can red it.
//
// ⇒ The fixture gives every nested `ConstructAggregate` the SAME shallow
// `array(i32,1)` type, so HIR depth grows while TYPE depth does not, and the
// cluster's own ceiling becomes the only one in the way. That HIR is not a
// shape a front end emits — deliberately, exactly like
// `MirLiteralWalk.HundredThousandLevelsCostHeapNotCallFrames` above, which
// builds a 100_000-level literal no source could produce. What is under test is
// a TRAVERSAL, and this is what isolates it.
//
// ✔MEASURED, ordinary thread, through `ctest`, same binary, same fixture:
//   * RECURSIVE cluster: 2000 rc 0, **4000 SEGFAULT** inside `lowerToMir`;
//   * CONVERTED cluster: 4000, 100_000 and **1_000_000** all rc 0.
// kDepth is 20_000 — 5x the measured crash floor, so restoring the recursion
// does not merely fail this test, it kills the process.
TEST(HirToMirAggregateInit, TwentyThousandBraceLevelsCostHeapNotCallFrames) {
    constexpr int kDepth = 20000;   // 5x the MEASURED recursive crash floor (4000)

    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const arr1 = ti.array(i32, 1);

    HirBuilder      b{"c"};
    HirLiteralPool  pool;
    HirLiteralValue three;
    three.core  = TypeKind::I32;
    three.value = std::int64_t{3};
    // Bottom-up and ITERATIVE — the fixture must not recurse either.
    HirNodeId cur = b.makeLiteral(i32, pool.add(three));
    for (int i = 0; i < kDepth; ++i)
        cur = b.makeConstructAggregate(std::array{cur}, arr1, HirFlags::Synthetic);

    HirLiteralValue zero;
    zero.core  = TypeKind::I32;
    zero.value = std::int64_t{0};
    HirNodeId const decl = b.makeVarDecl(arr1, /*symbol=*/1, cur);
    HirNodeId const ret  = b.makeReturn(b.makeLiteral(i32, pool.add(zero)));
    HirNodeId const body = b.makeBlock(std::array{decl, ret});
    HirNodeId const fn   = b.makeFunction(ti.fnSig({}, i32, CallConv::CcSysV),
                                          /*symbol=*/2, {}, body);
    Hir hir = std::move(b).finish(b.makeModule(std::array{fn}));

    MirLoweringConfig cfg;
    cfg.aggregateLayout       = AggregateLayoutParams{ScalarAlignmentRule::Natural, 16};
    cfg.aggregateLayoutLoaded = true;
    cfg.dataModel             = DataModel::Lp64;
    DiagnosticReporter reporter;
    HirToMirResult res = lowerToMir(hir, pool, ti, reporter,
                                    /*sourceMap=*/nullptr, cfg);

    // Assert the MODULE IS REAL, not merely that nothing crashed: the walk must
    // have reached the bottom and stored the innermost value.
    EXPECT_EQ(reporter.errorCount(), 0u);
    ASSERT_GT(res.mir.funcCount(), 0u);
    std::size_t geps = 0, stores = 0;
    bool        storedThree = false;
    for (std::uint32_t fi = 0; fi < res.mir.moduleFuncCount(); ++fi) {
        MirFuncId const f = res.mir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < res.mir.funcBlockCount(f); ++bi) {
            MirBlockId const bb = res.mir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < res.mir.blockInstCount(bb); ++ii) {
                MirInstId const inst = res.mir.blockInstAt(bb, ii);
                if (res.mir.instOpcode(inst) == MirOpcode::Gep) ++geps;
                if (res.mir.instOpcode(inst) != MirOpcode::Store) continue;
                ++stores;
                auto const ops = res.mir.instOperands(inst);
                if (ops.size() != 2) continue;
                if (res.mir.instOpcode(ops[0]) != MirOpcode::Const) continue;
                auto const& lit =
                    res.mir.literalValue(res.mir.constLiteralIndex(ops[0]));
                if (auto const* iv = std::get_if<std::int64_t>(&lit.value))
                    storedThree = storedThree || (*iv == 3);
            }
        }
    }
    EXPECT_GE(geps, static_cast<std::size_t>(kDepth))
        << "one Gep per brace level must reach the module — a short walk is a "
           "silently truncated initializer, not a dump bug";
    EXPECT_EQ(stores, 1u) << "exactly one scalar element is initialized";
    EXPECT_TRUE(storedThree)
        << "the innermost initializer value must reach the MIR";
}

// ── D-MIR-HIRTOMIR-SEH-TRY-EXCEPT-RECURSES-PER-REGION-LEVEL ──────────────────
//
// Plan-24 Stage 4b flattened Block / If / While / DoWhile / For / Label /
// Switch onto the statement driver's work stack and left `SehTryExcept` in the
// per-node handler, where each nested `__try` cost ONE `lowerStmtNode` frame
// (a 971-line switch) plus one `lowerStmt` driver frame with its own vector.
//
// ✔MEASURED before the conversion, in-process on the ordinary ~1 MiB gtest main
// thread, through `ctest`, ramping the depth in one process and printing a mark
// per completed level: **500 rc 0, 600 SEGFAULT** — the LOWEST ceiling in the
// MIR tier at the time. ✔ATTRIBUTED with gdb rather than inferred from the
// bisection: the stack was `lowerStmt` → `lowerStmtNode` (the `SehTryExcept`
// arm's `lowerStmt(tryN)`) → `lowerStmt` → …, repeating, with nothing else in
// the cycle.
//
// ✔MEASURED after, same instrument, same thread: **4000 rc 0**, and the crash
// at 8000 is gdb-attributed to `pathTerminates` in `src/hir/hir_verifier.hpp` —
// a DIFFERENT tier's still-recursive site, so the MIR-tier ceiling is out of
// the way entirely and a deeper kDepth here would red for somebody else's
// reason.
//
// ★★★ P60 (D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED):
// THE HIR-VERIFIER CEILING THIS PIN USED TO STAY BELOW IS GONE, AND THE PIN
// MOVED ONTO THE BOUNDED STACK. ✔MEASURED 2026-09-04 (lane `rc`): at the OLD
// kDepth of 2000 this case was green on the Ninja + mingw-w64 g++ gate and
// SEGFAULTED under MSVC 19.51 Debug — gdb-attributed, frame by frame, to a
// two-frame `pathTerminates` self-cycle (`src/hir/hir_verifier.hpp`) at 368
// bytes a frame, 736 bytes per `__try` level, dead at ~1380 of the 2000. The
// same site was the mingw wall at ~8000 (≈130 bytes per level). A green that
// depends on which toolchain built it is a margin, not a proof; `pathTerminates`
// is now an explicit heap frame stack and this case runs on 256 KiB
// (`tests/core/bounded_stack.hpp`) at 4000 — where the recursive predicate at
// its THINNEST measured cost (130 B/level → 520 KiB) already needs twice the
// reserve, and the MIR arm's old recursion (500 levels ≈ 1 MiB → ~2 KiB/level)
// sixteen times it. Restore either and the process dies, on every leg. The
// schemas are loaded on the main thread first (`shipped()`).
TEST(HirToMirSehDeepNesting, NestedTryExceptRegionsCostHeapNotCallFrames) {
    constexpr int kDepth = 4000;   // 8x the MEASURED pre-fix MIR ceiling (500)
    (void)shipped();               // load on the MAIN thread, never on the bounded one

    std::string src = "int main(void){ int rc = 0;\n";
    for (int i = 0; i < kDepth; ++i) src += "__try { ";
    src += "rc = rc + 1;";
    for (int i = 0; i < kDepth; ++i) src += " } __except(1) { rc = rc - 1; }";
    src += "\nreturn rc; }";

    std::size_t funcs = 0;
    std::size_t regions = 0;
    test::runOnBoundedStack([&] {
        auto L = lowerC(std::move(src), /*exprDepthCap=*/kDepth + 4096);
        funcs = L.mir.mir.funcCount();
        // COUNT THE MODULE'S SEH REGIONS: one `SehTryBegin` per source
        // `__try`. A short walk would be a silently dropped guarded region — a
        // miscompile, not a missing diagnostic.
        for (std::uint32_t fi = 0; fi < L.mir.mir.moduleFuncCount(); ++fi) {
            MirFuncId const f = L.mir.mir.funcAt(fi);
            for (std::uint32_t bi = 0; bi < L.mir.mir.funcBlockCount(f); ++bi) {
                MirBlockId const bb = L.mir.mir.funcBlockAt(f, bi);
                for (std::uint32_t ii = 0; ii < L.mir.mir.blockInstCount(bb); ++ii) {
                    if (L.mir.mir.instOpcode(L.mir.mir.blockInstAt(bb, ii))
                        == MirOpcode::SehTryBegin) {
                        ++regions;
                    }
                }
            }
        }
    });
    // ASSERT THE MODULE IS REAL, not merely that nothing crashed.
    ASSERT_GT(funcs, 0u);
    EXPECT_EQ(regions, static_cast<std::size_t>(kDepth))
        << "every source `__try` must mint exactly one SEH region — a short "
           "count is a guarded body that lost its handler";
}

// ── D-MIR-TEXT-READER-PARSETYPE-AND-PARSELITERAL-RECURSE-PER-LEVEL-UNCAPPED ──
//
// The WRITER half of this grammar (`appendType` / `appendLiteral`) went onto
// explicit work stacks in P55. The READER half did not: `parseType` had SIX
// self-calls and `parseLiteral` one, neither capped, and their depth follows
// the `.dssmir` TEXT — an input this reader does NOT produce and must not
// trust.
//
// ✔MEASURED before the conversion, in-process through `ctest` on the ordinary
// ~1 MiB main thread, with NO compiler stage in front of the reader (the text
// is built here), so nothing else can be the thing that dies: a `ptr<…<i32>…>`
// global type parsed with `ok=1 errors=0` at **1000** and SEGFAULTed at
// **2000**. ✔ATTRIBUTED with gdb: the stack is `parseType` calling itself at
// its wrapper arm, one frame per `ptr<`, with no other function in the cycle.
// ✔MEASURED after: **16000 rc 0, errors=0** — at least 16x, with no cap
// introduced and none needed (the work stack is bounded by the text already in
// memory).
//
// kDepth is 4000: 4x the MEASURED pre-fix crash floor of 1000.
TEST(MirTextDeepNesting, DeeplyNestedTypeParsesBackOnAnOrdinaryThread) {
    constexpr int kDepth = 4000;   // 4x the MEASURED pre-fix crash floor (1000)

    std::string text = "dssir 1\nmodule {\n  global %1 : ";
    for (int i = 0; i < kDepth; ++i) text += "ptr<";
    text += "i32";
    for (int i = 0; i < kDepth; ++i) text += ">";
    text += " = zero\n}\n";

    DiagnosticReporter reporter;
    auto parsed = parseMir(text, CompilationUnitId{1}, reporter);

    // It came back at all, and CLEANLY — a reader that truncated the type would
    // read back a DIFFERENT type, which is the silent half of this defect.
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(reporter.errorCount(), 0u);
    // Every level survived: re-render and count. `ptr<ptr<i32>>` and
    // `ptr<i32>` are different types, so a short count is a miscompile.
    DiagnosticReporter back;
    MirTextContext ctx;
    ctx.interner = &parsed->interner;
    std::string const again = emitMir(parsed->mir, ctx, back);
    std::size_t levels = 0;
    for (std::size_t at = again.find("ptr<"); at != std::string::npos;
         at = again.find("ptr<", at + 1)) {
        ++levels;
    }
    EXPECT_GE(levels, static_cast<std::size_t>(kDepth))
        << "every pointer level must survive the READ — a truncated parse is a "
           "silently different type";
}

// The literal half of the same row. `parseLiteral`'s ONE self-call is its `agg`
// arm, so the depth is the brace nesting of an untrusted initializer — the
// exact twin of the writer's `appendLiteral`, and the same shape the MIR
// literal walker is pinned at 100_000 for.
//
// ⚠⚠ IT PINS TWO CONVERSIONS AT ONCE, AND THE SECOND ONE WAS FOUND BY THIS
// TEST REFUSING TO GO DEEP. ✔MEASURED with the reader converted but the
// literal TYPE untouched: 400…2000 parsed and read back, 3000 SEGFAULTed —
// and ✔gdb attributed that crash to `~MirLiteralValue`, the COMPILER-GENERATED
// chain `~variant → ~MirAggregateValue → ~vector → ~MirLiteralValue`, one host
// frame per aggregate level. DESTRUCTION is a walk too; it was the one walk
// nobody had written, so no grep for recursion could see it. With the iterative
// `~MirAggregateValue` in `mir/mir_literal_pool.hpp` the same fixture reaches
// **100_000 rc 0**.
// ✔MEASURED for the reader itself, with the recursive `parseLiteral` restored:
// **1000 rc 0 / 1500 SEGFAULT**. kDepth 4000 is therefore 2.6x the reader's
// crash floor AND 2x the destructor's — either conversion removed reds it.
TEST(MirTextDeepNesting, DeeplyNestedLiteralParsesBackOnAnOrdinaryThread) {
    constexpr int kDepth = 4000;   // 2.6x the recursive reader's crash floor
                                   // (1500) AND 2x the pre-fix destructor wall
    std::string text = "dssir 1\nmodule {\n  global %1 : i64 = ";
    for (int i = 0; i < kDepth; ++i) text += "lit agg { ";
    text += "lit int 7 : i64";
    for (int i = 0; i < kDepth; ++i) text += " } : i64";
    text += "\n}\n";

    DiagnosticReporter reporter;
    auto parsed = parseMir(text, CompilationUnitId{1}, reporter);

    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(reporter.errorCount(), 0u);
    // The innermost value survived the whole descent — walk down ITERATIVELY
    // (the fixture must not recurse either) and read it.
    ASSERT_GT(parsed->mir.moduleGlobalCount(), 0u);
    std::uint32_t const litIdx =
        parsed->mir.globalInitLiteralIndex(parsed->mir.globalAt(0));
    ASSERT_NE(litIdx, UINT32_MAX);
    MirLiteralValue const* cur = &parsed->mir.literalValue(litIdx);
    int levels = 0;
    while (auto const* agg = std::get_if<MirAggregateValue>(&cur->value)) {
        ASSERT_EQ(agg->fields.size(), 1u);
        cur = &agg->fields[0];
        ++levels;
    }
    EXPECT_EQ(levels, kDepth)
        << "every brace level must survive the READ — a truncated literal is a "
           "silently different initializer";
    auto const* iv = std::get_if<std::int64_t>(&cur->value);
    ASSERT_NE(iv, nullptr);
    EXPECT_EQ(*iv, 7);
}

// ── THE MEASUREMENT INSTRUMENT FOR THE ONE MIR SITE STILL RECURSIVE ─────────
//
// `runExprDriver`'s `SeqExpr` arm lowers a comma chain's side-effect statements
// through `lowerStmt` — a separate machine on its own host frames — and each
// of those statements' expressions re-enters the driver, so a LEFT-DEEP comma
// chain (the shape `a=1, a=2, …` lowers to) costs a six-frame
// `SeqExpr ⇄ ExprStmt ⇄ AssignStmt` cycle per element. Its axis is LIST
// LENGTH, the one shape a real corpus reaches (fts5.c: 1765 elements in one
// initializer). The row records 300 rc 0 / 400 SEGFAULT on the ordinary
// thread; this case re-measures it without a rebuild:
//   DSS_MIR_PROBE_COMMA=<N> ctest --test-dir build/<x> \
//       -R mir/test_deep_nesting_costs_heap -V
// Idle (skipped) unless the variable is set, so the gate is unaffected.
TEST(HirToMirSeqExprProbe, WalksTheConfiguredCommaChainLength) {
    char const* const e = std::getenv("DSS_MIR_PROBE_COMMA");
    if (e == nullptr || *e == '\0') {
        GTEST_SKIP() << "DSS_MIR_PROBE_COMMA unset — probe idle";
    }
    int const n = std::atoi(e);
    (void)shipped();
    // `x = 1, x = 2, …, x = n;` — every element is an assignment whose value is
    // discarded, and `x` is a runtime variable so nothing folds.
    std::string src = "int main(void){ int x = 0; ";
    for (int i = 1; i <= n; ++i) {
        if (i > 1) src += ", ";
        src += "x = " + std::to_string(i);
    }
    src += "; return x; }";
    std::fprintf(stdout, "PROBE-MARK BEGIN comma=%d\n", n);
    std::fflush(stdout);
    auto L = lowerC(std::move(src), /*exprDepthCap=*/static_cast<std::size_t>(n) + 4096);
    std::fprintf(stdout, "PROBE-MARK MIR funcs=%u\n",
                 static_cast<unsigned>(L.mir.mir.funcCount()));
    std::fflush(stdout);
}

TEST(HirToMirAtomicAlign, DeepPackedChainStaysUnderAlignedPastTheOldDepthCap) {
    // 70 links: comfortably past the removed `depth > 64`, nowhere near the
    // MEASURED front-end ceilings for nested struct definitions.
    constexpr int kLinks = 70;
    auto L = lowerC(packedAtomicChainSource(kLinks, /*packed=*/true),
                    /*exprDepthCap=*/kLinks + 1024);
    auto const align = firstAtomicLoadAlign(L.mir.mir);
    ASSERT_TRUE(align.has_value())
        << "fixture precondition: the module must contain an AtomicLoad";
    EXPECT_EQ(*align, 1u)
        << "a packed _Atomic member reached through a 70-link chain is "
           "under-aligned; 0 here is the 'unknown' sentinel mir_to_lir reads as "
           "ALIGNED, which emits the native atomic and Bus-errors on arm64";
}

TEST(HirToMirAtomicAlign, ShallowPackedChainIsUnderAlignedAndUnpackedIsNot) {
    // CONTROL 1 — the same shape, 2 links, well inside the old cap: the answer
    // must be the same, so the deep arm above cannot be satisfied by a change
    // that merely reports under-alignment more often.
    {
        auto L = lowerC(packedAtomicChainSource(2, /*packed=*/true),
                        /*exprDepthCap=*/1024);
        auto const align = firstAtomicLoadAlign(L.mir.mir);
        ASSERT_TRUE(align.has_value());
        EXPECT_EQ(*align, 1u) << "shallow packed access must still be under-aligned";
    }
    // CONTROL 2 — NOT packed: `char pad` then a struct whose alignment is 4
    // pushes the member to offset 4, so the access is fully aligned and must
    // keep the native inline form. A fix that routed everything through the
    // runtime would pass both arms above and fail here.
    {
        auto L = lowerC(packedAtomicChainSource(2, /*packed=*/false),
                        /*exprDepthCap=*/1024);
        auto const align = firstAtomicLoadAlign(L.mir.mir);
        ASSERT_TRUE(align.has_value());
        EXPECT_EQ(*align, 4u)
            << "an unpacked _Atomic int member is naturally aligned — reporting "
               "anything less would route a fine access through the libcall";
    }
}


// ── D-MIR-HIRTOMIR-ADDRESSOF-DEREF-REENTRY-RECURSES-PER-LINK ─────────────────
//
// `request` (the shared {value,address} driver's classifier) flattens the
// straight-line arms, the CFG value arms and Call; every OTHER kind DELEGATES to
// `lowerExprNode` / `lowerLvalueAddressNode`, which lower that one node and
// RE-ENTER the driver for its children — a fresh `runExprDriver` with its own
// two vectors per link. Two of those delegating arms do nothing but hand the
// request back with the address/value flag flipped: `AddressOf`'s body is
// `lowerLvalueAddress(child)` and the by-address `Deref`'s is
// `lowerExpr(child)`. So `*&*&…*&p` — a chain whose depth is its LENGTH, not
// any nesting — cost FOUR host frames per `*&` pair.
//
// ✔MEASURED before the fix, in-process on the ordinary ~1 MiB gtest main thread
// through `ctest`, ramping in one process: **100 rc 0 / 200 SEGFAULT** — the
// LOWEST ceiling anywhere in the MIR tier, below the `SehTryExcept` arm's 500
// and below the `SeqExpr` statement re-entry's 300. ✔ATTRIBUTED with gdb: the
// stack is `runExprDriver` → `lowerExprNode`(AddressOf) → `lowerLvalueAddress`
// → `runExprDriver` → `lowerLvalueAddressNode`(Deref) → `lowerExpr` → …,
// repeating, entirely inside `hir_to_mir.cpp`.
//
// ⚠⚠ THE CENSUS RECORDED THIS SITE AS `4000 rc 0` AND THAT NUMBER MEASURED
// NOTHING: it was taken on a left-deep `argc+argc+…` spine, and `BinaryOp` IS
// in `request`'s flatten set, so that input never reached the re-entry at all.
// Confirming that a construct REACHES the recursion before recording a ceiling
// for it is the discipline this whole class keeps paying for.
//
// ✔MEASURED after: **8000 rc 0** (at least 80x), and 16000 is a ctest TIMEOUT
// rather than a stack death — the remaining limit on this shape is TIME, not
// host frames. kDepth is 2000: 20x the measured crash floor of 100, and chosen
// low enough that the pin stays fast.
// ★ ON THE BOUNDED STACK (`tests/core/bounded_stack.hpp`, 256 KiB) since P60:
// the re-entry cost four host frames per `*&` pair — 100 pairs ≈ 1 MiB on
// mingw-w64 g++ Debug, ~10 KiB per pair — so 2000 pairs need ~20 MiB against
// 256 KiB. Restore it and the process dies.
TEST(HirToMirAddressChain, LongAddressOfDerefChainCostsHeapNotCallFrames) {
    constexpr int kDepth = 2000;   // 20x the MEASURED pre-fix ceiling (100)
    (void)shipped();

    // `*&` applied to an `int *` lvalue yields an `int *` lvalue again, so the
    // chain is well-typed at every length and its VALUE is just `p`.
    std::string src = "int x; int *p = &x;\nint main(void){ int *r = ";
    for (int i = 0; i < kDepth; ++i) src += "*&";
    src += "p;\nreturn *r; }";

    std::size_t funcs = 0;
    std::size_t loads = 0, stores = 0;
    test::runOnBoundedStack([&] {
        auto L = lowerC(std::move(src), /*exprDepthCap=*/kDepth * 4 + 4096);
        funcs = L.mir.mir.funcCount();
        for (std::uint32_t fi = 0; fi < L.mir.mir.moduleFuncCount(); ++fi) {
            MirFuncId const f = L.mir.mir.funcAt(fi);
            for (std::uint32_t bi = 0; bi < L.mir.mir.funcBlockCount(f); ++bi) {
                MirBlockId const bb = L.mir.mir.funcBlockAt(f, bi);
                for (std::uint32_t ii = 0; ii < L.mir.mir.blockInstCount(bb); ++ii) {
                    MirOpcode const op =
                        L.mir.mir.instOpcode(L.mir.mir.blockInstAt(bb, ii));
                    if (op == MirOpcode::Load)  ++loads;
                    if (op == MirOpcode::Store) ++stores;
                }
            }
        }
    });

    // Assert the module is REAL, and assert the SHAPE rather than just the
    // absence of a crash: `*&` is an identity on an lvalue, so the whole chain
    // must collapse to the same load-and-store `p` alone would produce. A
    // rewrite that dropped or duplicated a link would show up as extra memory
    // traffic here, not as a crash.
    ASSERT_GT(funcs, 0u);
    // `int *r = <chain>; return *r;` — the chain reads `p` once, `r` is stored
    // once and read once, and `*r` loads the int. A per-link Load/Store would
    // scale with kDepth; this asserts it does not.
    EXPECT_LT(loads,  static_cast<std::size_t>(16))
        << "the `*&` chain must collapse — a per-link Load means the rewrite "
           "materialized every intermediate address";
    EXPECT_LT(stores, static_cast<std::size_t>(16))
        << "the `*&` chain must collapse — a per-link Store means the rewrite "
           "materialized every intermediate address";
}
