// ── DEPTH MUST COST HEAP, NOT HOST CALL FRAMES — THE FRONT-END TIER ─────────
//
// The operator's standing ruling of 2026-09-02: *"it's well known to not use
// recursive structures in the compiler because big projects like sqlite will for
// sure explode the stack"*. The MIR tier's pins for it live in
// `tests/mir/test_deep_nesting_costs_heap.cpp`; this file is the front end's.
//
// ★★ EVERY TEST HERE RUNS ON THE ORDINARY gtest MAIN THREAD (~1 MiB), AND THAT
// IS THE WHOLE POINT. `src/program/program.cpp` builds every CU inside
// `substrate::callOnLargeStack(64 MiB)`, so the same input measured THROUGH THE
// CLI reports that everything is fine while a library embedder, an LSP, or a test
// binary crashes. A pin taken on that worker proves nothing about this class.
//
// ⚠ `analyze` IS GIVEN A LARGER RESERVE HERE, AND THAT IS DELIBERATE, NOT A
// SOFTENED INSTRUMENT. `analyze` runs its own implementation on its own worker;
// the stage under test in this file is `lowerToHir`, which runs on the CALLER's
// (ordinary) thread whatever that reserve is. ✔MEASURED on the ordinary thread
// through `ctest`: with `analyze` held to 1 MiB, this file's fixture ceiling is
// 7581 ok / 7892 crash and the death is inside `analyze` — the SEMANTIC tier's
// own uncapped recursion (`src/analysis/semantic/**`, not converted by this
// lane). Sizing that worker up moves someone else's ceiling out of the way so a
// red here can only mean what this file asserts. It cannot make a HIR-tier
// ceiling look better than it is.
//
// ★★ ITS OWN BINARY ON PURPOSE. When one of these reds it reds by EXHAUSTING THE
// STACK — the process dies with no `[  FAILED  ]` line and no case name, exactly
// the unattributable signature `no_abort_in_tests_guard` exists to keep out of
// shared binaries. Isolated here, `ctest` at least names this executable.
//
// ── ✔MEASURED CEILINGS, ORDINARY THREAD, THROUGH `ctest`, THIS LEG (MinGW gcc
//    13.2 Debug). The instrument is `test_frontend_deep_nesting_probe.cpp`;
//    every number below is reproducible with it, and NONE of them should be
//    re-quoted without re-measuring at the commit that carries it.
//
//   axis / fixture      site                                   before → after
//   ------------------  -------------------------------------  ---------------
//   structzero          synthZeroOrError + flattenInitSlot      2691 ok/2815
//   (aggregate TYPE       (cst_to_hir.cpp) — CONVERTED          → 7581 ok/7892,
//    nesting, zero-fill)                                          and the death
//                                                                 moved OUT of
//                                                                 this tier into
//                                                                 `analyze`.
//   initnest            lowerBraceInit <-> lowerExprOrBraceInit 1055 ok/1085
//   (brace NESTING)       (cst_to_hir.cpp) — CONVERTED to one   → 32768 ok; at
//                         `BraceLevel` heap stack; the row id     65536 the death
//                         is cited in full by the pin below.      is in PARSE, so
//                                                                 this tier's own
//                                                                 ceiling was
//                                                                 never reached.
//   initnestarray       TypeInterner::representationType        4096 ok/6144 —
//   (brace NESTING over   (src/core/types/type_lattice/         NOT this file, and
//    ARRAY levels)        type_lattice.cpp) — NOT CONVERTED     BELOW the 8192 the
//   arrayzero             and NOT in src/hir/**. The `arrayzero` references
//   (ARRAY TYPE depth,    control (ONE brace level, same array  require. Same walk
//    zero-fill)           TYPE depth) dies at the SAME          as `ptrtype`.
//                         4096/6144, which is what attributes
//                         the wall to the TYPE walk rather
//                         than to brace nesting.
//   complitnest         ParserConfig::maxSpeculationDepth      8 ok/9 REFUSED
//   (brace NESTING         (parser.hpp; C++ default 8 and NOT  with a diagnostic,
//    written as a          config-driven, unlike               not a crash — and
//    COMPOUND LITERAL      maxExpressionDepth) — NOT this      gcc compiles this
//    at every level)       file, and never reaches this tier.  shape to 8192,
//                         With the cap lifted through          clang to 256. A
//                         DSS_HS_PROBE_SPEC this tier lowers   conformance
//                         64 and 128 levels with ZERO errors;  divergence owned
//                         the next walls (~192, and a failure  by
//                         at 1024) are PARSE-side too.         src/analysis/
//                                                              syntactic/**.
//   paren / fncall      walkExpression <-> parseUntilFrameDepth 1257 ok/1318
//   (paren NESTING)       (parser.cpp) — NOT CONVERTED; the       → unchanged;
//                         config cap is 1024, so the margin is     the cap DOES
//                         1.23x, not the ~3x c.lang.json claims.   fire first.
//   ptrtype             TypeInterner::representationType        4297 ok/4453
//   (POINTER TYPE         (src/core/types/type_lattice/         → unchanged;
//    depth)               type_lattice.cpp) — NOT THIS LANE'S     NOT in
//                         file. `stop=analyze` reaches 19378      src/hir/**.
//                         rc 0, so the death is the HIR stage's
//                         call INTO the interner, not the
//                         semantic pass.
//
// ⚠ NOTHING HERE PINS `collectLeavesBelow_` (parser.cpp) BY DEPTH, and the
// reason is a measurement, not an omission: ✔MEASURED at 38812 terms of
// `(a)(x+1+…+1)` — the exact left-deep shape the residue row named as the
// highest-risk item — the walk returned rc 0. In every guarded shape the leftmost
// path reaches a token within a handful of levels, so no C source drives it deep.
// It is converted anyway, because that bound is a property of today's grammars
// rather than of that code.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/target_schema.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace dss;

namespace {

// See the header note: this sizes `analyze`'s OWN worker, never the thread
// `lowerToHir` runs on.
constexpr std::size_t kAnalyzeReserveBytes = std::size_t{16} * 1024 * 1024;

struct Lowered {
    std::shared_ptr<GrammarSchema const> schema;
    std::shared_ptr<TargetSchema const>  target;
    SemanticModel                        model;
    std::unique_ptr<CstToHirResult>      hir;
    std::size_t                          hirErrors = 0;
};

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

    auto srcBuf = SourceBuffer::fromString(std::move(src), "<frontdeep>");
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
                LongDoubleFormat::None, target.get(), kAnalyzeReserveBytes);
    if (model.hasErrors())
        throw std::runtime_error{"deep-nest fixture did not analyze cleanly"};

    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    return Lowered{.schema    = std::move(schema),
                   .target    = std::move(target),
                   .model     = std::move(model),
                   .hir       = std::move(hir),
                   .hirErrors = hirReporter.errorCount()};
}

} // namespace

// ── `synthZeroOrError` + `flattenInitSlot` (src/hir/lowering/cst_to_hir.cpp) ──
//
// AXIS: the AGGREGATE TYPE's nesting, walked through the ZERO-FILL path. C23
// 6.7.10p11's `= {}` fills every omitted slot, so `struct S0 g = {};` over a
// D-deep nest of structs asks `synthZeroOrError` for a D-deep tree of zeros and
// then asks `flattenInitSlot` to assemble it. Both were host recursion, neither
// was capped; both are now explicit post-order heap stacks.
//
// ⚠ THIS IS NOT THE AXIS sqlite TRIPS, and saying so is part of the pin.
// ✔MEASURED on the staged amalgamation by the lane before this one: real
// struct-in-struct nesting is 4 deep. What a real program reaches is LIST LENGTH
// (1765 initializer elements in one brace), and a list is BREADTH here, not
// depth. This is a pin on a shape a GENERATED header can reach, not on sqlite.
//
// kDepth is 1.6x the MEASURED pre-conversion crash floor (2815) — restore either
// recursion and this test does not merely fail, the process dies — and 1.68x
// below the current measured ceiling (7581), which is now set by the SEMANTIC
// tier rather than by anything this file's subject owns.
TEST(FrontendDeepNesting, ZeroFillOfADeeplyNestedAggregateCostsHeap) {
    constexpr int kDepth = 4500;

    std::string src;
    for (int i = 0; i < kDepth; ++i) src += "struct S" + std::to_string(i) + " { ";
    src += "int x;";
    for (int i = kDepth - 1; i >= 0; --i) src += " } m" + std::to_string(i) + ";";
    src += "\nstruct S0 g = {};\nint main(void){ return 0; }";

    auto L = lowerC(std::move(src), /*exprDepthCap=*/kDepth + 1024);

    // It lowered at all — the two walks covered 4500 type levels on a ~1 MiB
    // thread. And the module is REAL: one `ConstructAggregate` per level plus the
    // innermost zero means the node count cannot be below the depth. A truncated
    // zero-fill is a MISCOMPILE (an object left partly uninitialized where C23
    // 6.7.10p11 promises zeros), not a cosmetic shortfall, so the completeness
    // half is asserted and not merely the survival half.
    EXPECT_EQ(L.hirErrors, 0u);
    EXPECT_GE(L.hir->hir.nodeCount(), static_cast<std::size_t>(kDepth))
        << "every aggregate level must reach the HIR — a truncated zero-fill "
           "leaves an object partly uninitialized";
}

// ── `lowerBraceInit` <-> `lowerExprOrBraceInit` (src/hir/lowering/cst_to_hir.cpp)
//
// AXIS: initializer BRACE nesting. The row is
// [[D-HIR-BRACE-INIT-LOWERING-RECURSES-PER-BRACE-LEVEL-AND-A-CAP-IS-UNAVAILABLE]].
// `struct S0 g = {{{…3…}}}` over a D-deep nest of structs asks the brace-init
// lowerer for D levels, and the two functions used
// to call each other once per level — two host frames each. Both halves now run
// on ONE explicit heap level stack (`BraceLevel`), and the union arm was folded
// into it as a one-slot level rather than left as a second recursion edge.
//
// ★★★ kDepth IS 8192 BECAUSE A FAIL-LOUD CAP WAS NOT AN AVAILABLE EXIT, AND THAT
// IS THE WHOLE POINT OF THIS PIN — it is not a round number chosen for margin.
// Under `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` the union is over what WORKS, so any
// cap must sit ABOVE what a working reference compiles. ✔MEASURED separately per
// reference (WSL, `-std=c2x -O0`, this exact shape, D ∈ {64,128,256,257,512,1024,
// 2048,4096,8192}): gcc 13.3.0 compiles EVERY depth rc 0 and its 8192 binary RUNS
// rc 0; clang 18.1.3 stops at 256 with a loud positioned `bracket nesting level
// exceeded maximum of 256`. ONE working reference at 8192 makes 8192 REQUIRED, so
// a cap anywhere below it would refuse a program gcc compiles — which is why this
// site was CONVERTED and no counter was reintroduced.
//
// ✔MEASURED on the ordinary thread through `ctest` at the commit that carries
// this: 1055 ok / 1085 SEGFAULT before, and 32768 ok after — at 65536 the death
// is in PARSE (the last mark is TOKENIZE), so this tier's own ceiling was not
// reached at all. 8192 therefore sits 7.5x above the pre-conversion crash floor —
// restore either recursion and this test does not merely fail, the process dies —
// and 4x below the highest depth this tier is measured to survive.
//
// ⚠ THE END-TO-END CLI CEILING IS SET SOMEWHERE ELSE AND IS FAR LOWER, and saying
// so is part of the pin. ✔MEASURED through `dsscp --compile` on the 64 MiB CU
// worker, `struct SD g = {{{…42…}}}` builds and runs rc 42 at 1000 and dies
// 0xC00000FD at 1100 — and that pair is IDENTICAL on both arms of this file's
// red-on-disable transcript, so the remaining wall belongs to a tier AFTER
// `lowerToHir`, not to this one. Two consequences: no corpus example can witness
// this conversion's ceiling (there is no depth that is green after and red
// before), and this unit pin is the only place the 8192 requirement can live.
TEST(FrontendDeepNesting, NestedBraceInitializerCostsHeap) {
    constexpr int kDepth = 8192;

    // ⚠ THE STRUCT SPELLING, DELIBERATELY, AND THE CHEAPER ARRAY ONE IS A TRAP.
    // `int g[1][1]…[1] = {{{…3…}}}` asks the same question of the same code in
    // ONE declaration instead of kDepth of them, and gcc/clang answer it exactly
    // the same way (✔MEASURED: gcc 13.3.0 compiles AND runs 8192; clang 18.1.3
    // refuses at 257). But it CANNOT carry this pin: ✔MEASURED, that shape dies
    // at 6144 (4096 ok) inside the HIR stage — and the `arrayzero` control in the
    // probe (`int g[1][1]…[1] = {};`, ONE brace level, the same array TYPE depth)
    // dies at the SAME 4096/6144, so the wall is the per-level ARRAY TYPE walk in
    // `TypeInterner::representationType`, not brace nesting. Pinning the array
    // spelling here would report the type lattice's ceiling under this test's
    // name. A nominal struct is projected to itself, so the struct spelling
    // isolates the brace axis — at the price of kDepth declarations to parse
    // (✔MEASURED at 8192: ~36 s parse, ~29 s lower).
    std::string src;
    for (int i = 0; i < kDepth; ++i) src += "struct S" + std::to_string(i) + " { ";
    src += "int x;";
    for (int i = kDepth - 1; i >= 0; --i) src += " } m" + std::to_string(i) + ";";
    src += "\nstruct S0 g = ";
    src.append(static_cast<std::size_t>(kDepth), '{');
    src += "3";
    src.append(static_cast<std::size_t>(kDepth), '}');
    src += ";\nint main(void){ return 0; }";

    auto L = lowerC(std::move(src), /*exprDepthCap=*/kDepth + 1024);

    // It lowered at all — 8192 brace levels on a ~1 MiB thread. And the module is
    // REAL: one `ConstructAggregate` per level means the node count cannot be
    // below the depth, so a lowering that silently stopped descending (dropping
    // the initializer for every level below the cut — a MISCOMPILE, not a
    // cosmetic shortfall) fails here instead of passing on survival alone.
    EXPECT_EQ(L.hirErrors, 0u);
    EXPECT_GE(L.hir->hir.nodeCount(), static_cast<std::size_t>(kDepth))
        << "every brace level must reach the HIR — a truncated brace-init "
           "leaves an object initialized with the wrong value";
}
