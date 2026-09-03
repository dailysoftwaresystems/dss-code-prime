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

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
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

// ⚠ A THROW, NEVER `std::abort()` — an abort kills the process and every sibling
// here loses its verdict, which is the exact signature these tests exist to tell
// APART from a stack overflow. `no_abort_in_tests_guard` enforces the same rule.
[[nodiscard]] Lowered lowerC(std::string src, std::size_t exprDepthCap) {
    auto loaded = GrammarSchema::loadShipped("c");
    if (!loaded) throw std::runtime_error{"loadShipped(c) failed"};
    std::shared_ptr<GrammarSchema const> schema = *loaded;

    auto t = TargetSchema::loadShipped("x86_64");
    if (!t) throw std::runtime_error{"loadShipped(x86_64) failed"};
    std::shared_ptr<TargetSchema const> target = *t;

    // Tokenize + parse on THIS thread. The parser's residual paren/postfix arm is
    // a separate, still-recursive site (plan-24 Stage 5b, capped by
    // `maxExpressionDepth`); none of the shapes below nest parentheses, and the
    // cap is raised past the fixture depth so a loud refusal cannot be mistaken
    // for the property under test.
    auto srcBuf = SourceBuffer::fromString(std::move(src), "<deepnest>");
    Tokenizer tk{srcBuf, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    ParserConfig pcfg;
    pcfg.maxExpressionDepth = exprDepthCap;
    Parser p{srcBuf, schema, std::move(stream), DiagnosticBudget::libraryDefault(),
             std::move(pcfg), std::move(lexDiags)};
    ParseResult result = std::move(p).parse();
    if (result.tree.diagnostics().hasErrors())
        throw std::runtime_error{"deep-nest fixture did not parse cleanly"};

    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addTree(std::move(result.tree));
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());

    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                LongDoubleFormat::None, target.get(),
                kOrdinaryThreadReserveBytes);
    if (model.hasErrors())
        throw std::runtime_error{"deep-nest fixture did not analyze cleanly"};

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
TEST(HirToMirDeepNesting, DeeplyNestedStatementsLowerOnAnOrdinaryThread) {
    constexpr int kDepth = 6000;   // ~2.3x the MEASURED pre-fix ceiling of ~2560
    std::string src = "int main(void){ int x=0; ";
    for (int i = 0; i < kDepth; ++i) src += "if(x){ ";
    src += "return 1;";
    for (int i = 0; i < kDepth; ++i) src += " }";
    src += " return 0; }";

    auto L = lowerC(std::move(src), /*exprDepthCap=*/kDepth + 1024);
    // It lowered at all — the whole-body pre-passes walked 6000 HIR levels on a
    // ~1 MiB thread. Assert the module is real, not just that nothing crashed.
    EXPECT_GT(L.mir.mir.funcCount(), 0u);
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
    forEachLiteralNode(std::as_const(cur), [&](MirLiteralValue const& n) {
        ++visited;
        if (auto const* iv = std::get_if<std::int64_t>(&n.value)) {
            leaves.push_back(*iv);
        }
    });

    // kDepth+1 aggregates plus the two leaves.
    EXPECT_EQ(visited, static_cast<std::size_t>(kDepth) + 3u);
    ASSERT_EQ(leaves.size(), 2u);
    EXPECT_EQ(leaves[0], 1);
    EXPECT_EQ(leaves[1], 2)
        << "fields must be visited in declaration order — the merge assigns "
           "symbol numbers in this order and the summary records it";

    // ⚠ TEAR IT DOWN ITERATIVELY, AND THE REASON IS THE SUBJECT OF THIS FILE.
    // A nested `MirLiteralValue`'s DESTRUCTOR is itself one host frame per level
    // — `~variant` → `~MirAggregateValue` → `~vector` → `~MirLiteralValue` — and
    // it is GENERATED BY THE STANDARD LIBRARY, so nothing this project flattens
    // can reach it. ✔MEASURED that it survives kDepth on THIS leg (the clean
    // gate passes), but that is a one-leg measurement of somebody else's frame
    // size; a pin that died in TEARDOWN would red for a reason with nothing to
    // do with what it asserts. Unlink the spine so every node destroyed is
    // shallow. INFERRED, not measured: that a deeper chain or a fatter Debug
    // frame on another leg would overflow here.
    {
        std::vector<MirLiteralValue> shallow;
        shallow.reserve(static_cast<std::size_t>(kDepth) + 2u);
        while (true) {
            auto* agg = std::get_if<MirAggregateValue>(&cur.value);
            if (agg == nullptr || agg->fields.empty()) break;
            MirLiteralValue child = std::move(agg->fields.front());
            agg->fields.clear();
            shallow.push_back(std::move(cur));
            cur = std::move(child);
        }
    }
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
