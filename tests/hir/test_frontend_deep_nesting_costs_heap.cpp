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
//   (brace NESTING         — was a C++ default of 8; config-   with a diagnostic,
//    written as a          driven since P55 (`c.lang.json`     not a crash — and
//    COMPOUND LITERAL      `parser.maxSpeculationDepth`, 2048  gcc compiles this
//    at every level)       since P60). NOT this file, and      shape to 8192,
//                         never reaches this tier. With the    clang to 256.
//                         cap lifted through DSS_HS_PROBE_SPEC
//                         this tier lowers 64 and 128 levels
//                         with ZERO errors.
//   paren / fncall      walkExpression <-> parseUntilFrameDepth 1257 ok/1318
//   (paren NESTING)       (parser.cpp) — CONVERTED IN P60         → 65536 parens
//                         (one heap driver, `driveParse_`); the   and 16384 calls
//                         config cap is 16384 since P60, a        parse on the
//                         SEMANTIC limit, no longer a stack       ordinary thread;
//                         backstop (D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED).
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
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/hir_literal_pool.hpp"
#include "hir/hir_text.hpp"       // emitHir / parseHir — the `.dsshir` codec's deep pins
#include "hir/hir_verifier.hpp"   // pathTerminates — the structural-termination predicate
#include "hir/lowering/cst_to_hir.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include "../core/bounded_stack.hpp"   // runOnBoundedStack — the BOUND (see there)

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

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
// ★ ON THE BOUNDED STACK (`tests/core/bounded_stack.hpp`, 256 KiB) since P60:
// the two recursions cost ~370 bytes per level on mingw-w64 g++ Debug (2815
// levels ≈ 1 MiB), so 4500 levels need ≥ 1.6 MiB against 256 KiB. Restore
// either and the process dies. The schemas load on the main thread first.
TEST(FrontendDeepNesting, ZeroFillOfADeeplyNestedAggregateCostsHeap) {
    constexpr int kDepth = 4500;
    (void)shipped();

    std::string src;
    for (int i = 0; i < kDepth; ++i) src += "struct S" + std::to_string(i) + " { ";
    src += "int x;";
    for (int i = kDepth - 1; i >= 0; --i) src += " } m" + std::to_string(i) + ";";
    src += "\nstruct S0 g = {};\nint main(void){ return 0; }";

    std::size_t hirErrors = 1;
    std::size_t nodes     = 0;
    test::runOnBoundedStack([&] {
        auto L = lowerC(std::move(src), /*exprDepthCap=*/kDepth + 1024);
        hirErrors = L.hirErrors;
        nodes     = L.hir->hir.nodeCount();
    });

    // It lowered at all — the two walks covered 4500 type levels on a 256 KiB
    // thread. And the module is REAL: one `ConstructAggregate` per level plus the
    // innermost zero means the node count cannot be below the depth. A truncated
    // zero-fill is a MISCOMPILE (an object left partly uninitialized where C23
    // 6.7.10p11 promises zeros), not a cosmetic shortfall, so the completeness
    // half is asserted and not merely the survival half.
    EXPECT_EQ(hirErrors, 0u);
    EXPECT_GE(nodes, static_cast<std::size_t>(kDepth))
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
// ★ ON THE BOUNDED STACK (`tests/core/bounded_stack.hpp`, 256 KiB) since P60:
// the two-function recursion cost ~1 KiB per level on mingw-w64 g++ Debug
// (1085 levels ≈ 1 MiB), so 8192 levels need ≥ 8 MiB against 256 KiB — and
// so does any of the per-level walks the initializer meets after this tier
// (the HIR pool's copy and teardown, both converted in P60). Restore any and
// the process dies. The schemas load on the main thread first.
TEST(FrontendDeepNesting, NestedBraceInitializerCostsHeap) {
    constexpr int kDepth = 8192;
    (void)shipped();

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

    std::size_t hirErrors = 1;
    std::size_t nodes     = 0;
    test::runOnBoundedStack([&] {
        auto L = lowerC(std::move(src), /*exprDepthCap=*/kDepth + 1024);
        hirErrors = L.hirErrors;
        nodes     = L.hir->hir.nodeCount();
    });

    // It lowered at all — 8192 brace levels on a 256 KiB thread. And the module
    // is REAL: one `ConstructAggregate` per level means the node count cannot be
    // below the depth, so a lowering that silently stopped descending (dropping
    // the initializer for every level below the cut — a MISCOMPILE, not a
    // cosmetic shortfall) fails here instead of passing on survival alone.
    EXPECT_EQ(hirErrors, 0u);
    EXPECT_GE(nodes, static_cast<std::size_t>(kDepth))
        << "every brace level must reach the HIR — a truncated brace-init "
           "leaves an object initialized with the wrong value";
}

// ── `pathTerminates` (src/hir/hir_verifier.hpp) — the structural-termination
//    predicate, shared by the verifier and by cst_to_hir's implicit-return-0 ──
//
// AXIS: STATEMENT nesting. The predicate used to call itself once per nested
// statement level — every child of a block, both arms of an if, a `__try`'s body
// and handler, a switch's body — with no cap, and it runs INSIDE `lowerToHir`
// (cst_to_hir asks it whether `main`'s body already terminates) and inside the
// HIR verifier, so its depth followed the user's nesting.
//
// ✔MEASURED 2026-09-04 (P60, lane `rc`), gdb-attributed frame by frame on the
// MSVC 19.51 Debug build: the MIR tier's
// `HirToMirSehDeepNesting.NestedTryExceptRegionsCostHeapNotCallFrames` (2000
// nested `__try`) died of stack overflow in a TWO-frame `pathTerminates`
// self-cycle at 368 bytes per frame — 736 bytes per region level, ~1380 levels
// of a 1 MB stack — and under mingw-w64 g++ 13.2 the same site was the wall the
// MIR row records at ~8000. Converted, each level is one heap `Frame`.
//
// These three cases drive the predicate DIRECTLY over a hand-built HIR, one
// per compound kind that used to recurse (Block, If, SehTryExcept — Switch
// rides the Block arm), so a regression in any one arm reds under its own name.
// kDepth is 100_000: far past the point where ANY host frame per level survives
// any thread, and it costs nothing on the heap — restore either recursion and
// the process dies rather than merely failing.
//
// ⚠ THE NEGATIVE CONTROL IS WHAT MAKES THE POSITIVE ARMS MEAN SOMETHING. A
// predicate that answered `true` without walking would pass every deep pin; the
// same nest over a NON-terminating leaf must answer `false`, which it can only
// do by reaching the bottom.
namespace {

struct TerminationRig {
    TypeInterner interner{CompilationUnitId{1}};
    HirBuilder   builder{"c"};
    TypeId       i32 = interner.primitive(TypeKind::I32);
    // A literal `1` for every condition / filter — each use mints its OWN node
    // (a shared node would build a DAG, which `HirBuilder` refuses).
    [[nodiscard]] HirNodeId one() { return builder.makeLiteral(i32, 0); }
    [[nodiscard]] HirNodeId terminating() { return builder.makeReturn(); }
    // `x;` — a statement control falls through.
    [[nodiscard]] HirNodeId fallingThrough() { return builder.makeExprStmt(one()); }
};

} // namespace

// The predicate runs on the BOUNDED STACK (`tests/core/bounded_stack.hpp`,
// 256 KiB): at its THINNEST measured cost (~130 bytes per level, mingw-w64 g++
// Debug) 100_000 levels of the old recursion need ~13 MiB, fifty times the
// reserve; at MSVC's 736 they need 70 MiB. Only the verdict crosses back.
TEST(FrontendDeepNesting, PathTerminatesOverNestedBlocksCostsHeap) {
    constexpr int kDepth = 100000;
    for (bool const terminates : {true, false}) {
        TerminationRig r;
        HirNodeId cur = terminates ? r.terminating() : r.fallingThrough();
        for (int i = 0; i < kDepth; ++i) cur = r.builder.makeBlock(std::array{cur});
        bool verdict = !terminates;
        test::runOnBoundedStack([&] { verdict = pathTerminates(r.builder, cur); });
        EXPECT_EQ(verdict, terminates)
            << "a " << kDepth << "-deep block nest over a "
            << (terminates ? "return" : "fall-through")
            << " must answer by reaching the bottom, on a 256 KiB thread";
    }
}

TEST(FrontendDeepNesting, PathTerminatesOverNestedIfElseCostsHeap) {
    constexpr int kDepth = 100000;
    for (bool const terminates : {true, false}) {
        TerminationRig r;
        // `if (1) return; else <inner>` — the verdict descends the ELSE arm at
        // every level, and only the innermost leaf decides it.
        HirNodeId cur = terminates ? r.terminating() : r.fallingThrough();
        for (int i = 0; i < kDepth; ++i)
            cur = r.builder.makeIfStmt(r.one(), r.terminating(), cur);
        bool verdict = !terminates;
        test::runOnBoundedStack([&] { verdict = pathTerminates(r.builder, cur); });
        EXPECT_EQ(verdict, terminates)
            << "a " << kDepth << "-deep if/else chain must be decided by its "
               "innermost leaf";
    }
}

TEST(FrontendDeepNesting, PathTerminatesOverNestedSehRegionsCostsHeap) {
    constexpr int kDepth = 100000;
    for (bool const terminates : {true, false}) {
        TerminationRig r;
        // `__try { <inner> } __except (1) { return; }` — terminates iff BOTH the
        // guarded body and the handler do, so the innermost body decides.
        HirNodeId cur = terminates ? r.terminating() : r.fallingThrough();
        for (int i = 0; i < kDepth; ++i)
            cur = r.builder.makeSehTryExcept(cur, r.one(), r.terminating());
        bool verdict = !terminates;
        test::runOnBoundedStack([&] { verdict = pathTerminates(r.builder, cur); });
        EXPECT_EQ(verdict, terminates)
            << "a " << kDepth << "-deep __try nest must be decided by its "
               "innermost guarded body";
    }
}

// ── `hir_text.cpp` — the `.dsshir` codec: `appendType` / `parseType` and
//    `appendLiteralValue` / `parseLiteralValue` ────────────────────────────────
//
// D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED. The
// MIR tier's text codec went onto explicit stacks in P55/P56
// (`mir_text.cpp`); the HIR tier's twin still called itself once per type
// level (every `ptr<`, `arr<`, `fn(` parameter, `struct {` field) and once per
// aggregate-literal level, in BOTH directions. The writer's depth follows the
// module's types; the reader's follows an UNTRUSTED `.dsshir` text — and
// `parseTypeFromText`, which decodes every shipped FFI descriptor's signature,
// drives the same production. Both are `TypeEmitTask` / `TypeParseFrame` /
// `LiteralEmitTask` / `LiteralParseFrame` machines now, transferred from the
// MIR tier with the P44 qualification channel carried on the frames.
//
// ★ ON THE BOUNDED STACK (`tests/core/bounded_stack.hpp`, 256 KiB). The MIR
// twins measured 1000 ok / 2000 crash (reader) and 600 ok / 2000 crash
// (writer) on a 1 MiB thread, i.e. ≥ 500 bytes per level at the THINNEST; at
// these depths any of the four old recursions needs megabytes. Restore any and
// the process dies. And the round trip is asserted BYTE-IDENTICAL: a codec that
// survived by truncating would be a silently different type or initializer.
namespace {

// A module whose one statement is a literal of TYPE `t` and VALUE `v`, so the
// text carries `lit <v> : <t>` — the writer spells both, the reader rebuilds
// both. Symbol 1 is `main`.
struct TextRig {
    TypeInterner   interner{CompilationUnitId{1}};
    HirLiteralPool pool;
    std::vector<std::string> names{"", "main"};
    [[nodiscard]] Hir module(TypeId literalType, HirLiteralValue value) {
        HirBuilder b{"c"};
        TypeId const i32 = interner.primitive(TypeKind::I32);
        HirNodeId const stmt =
            b.makeExprStmt(b.makeLiteral(literalType, pool.add(std::move(value))));
        HirNodeId const body = b.makeBlock(std::array{stmt, b.makeReturn()});
        HirNodeId const fn   = b.makeFunction(
            interner.fnSig({}, i32, CallConv::CcSysV), /*symbol=*/1, {}, body);
        return std::move(b).finish(b.makeModule(std::array{fn}));
    }
    [[nodiscard]] HirTextContext context() {
        HirTextContext ctx;
        ctx.interner    = &interner;
        ctx.symbolNames = &names;
        ctx.literalPool = &pool;
        return ctx;
    }
};

} // namespace

TEST(FrontendDeepNesting, HirTextDeepPointerTypeRoundTripsOnABoundedStack) {
    constexpr int kDepth = 16000;
    TextRig r;
    TypeId deep = r.interner.primitive(TypeKind::I64);
    for (int i = 0; i < kDepth; ++i) deep = r.interner.pointer(deep);
    Hir const hir = r.module(deep, HirLiteralValue{std::int64_t{0}, TypeKind::I64});

    std::string text, again;
    std::size_t writerErrors = 1, readerErrors = 1;
    bool        parsedOk     = false;
    test::runOnBoundedStack([&] {
        DiagnosticReporter w;
        text         = emitHir(hir, r.context(), w);
        writerErrors = w.errorCount();
        DiagnosticReporter p;
        auto res = parseHir(text, CompilationUnitId{9}, p);
        parsedOk     = res != nullptr && res->ok;
        readerErrors = p.errorCount();
        if (!parsedOk) return;
        // Re-emit from the REBUILT module through its own interner and pool.
        HirTextContext ctx2;
        ctx2.interner    = &res->interner;
        ctx2.symbolNames = &r.names;
        ctx2.literalPool = &res->literalPool;
        DiagnosticReporter w2;
        again = emitHir(res->hir, ctx2, w2);
    });
    EXPECT_EQ(writerErrors, 0u);
    ASSERT_TRUE(parsedOk) << "the reader must accept the writer's own output";
    EXPECT_EQ(readerErrors, 0u);
    std::size_t levels = 0;
    for (std::size_t at = text.find("ptr<"); at != std::string::npos;
         at = text.find("ptr<", at + 1)) {
        ++levels;
    }
    EXPECT_GE(levels, static_cast<std::size_t>(kDepth))
        << "every pointer level must reach the text — a truncated render is a "
           "silently different type";
    EXPECT_EQ(again, text)
        << "the round trip must be byte-identical — a reader that lost a level "
           "would re-emit a different type";
}

TEST(FrontendDeepNesting, HirTextDeepAggregateLiteralRoundTripsOnABoundedStack) {
    constexpr int kDepth = 20000;
    TextRig r;
    // `agg{ agg{ … agg{ int 7 } … } }`, built bottom-up and iteratively — the
    // fixture must not recurse either. Every nested field carries the `Array`
    // core the writer spells after it.
    HirLiteralValue cur;
    cur.value = std::int64_t{7};
    cur.core  = TypeKind::I64;
    for (int i = 0; i < kDepth; ++i) {
        HirAggregateValue up;
        up.fields.push_back(std::move(cur));
        HirLiteralValue next;
        next.value = std::move(up);
        next.core  = TypeKind::Array;
        cur = std::move(next);
    }
    Hir const hir = r.module(r.interner.primitive(TypeKind::I64), std::move(cur));

    std::string text, again;
    bool        parsedOk    = false;
    int         readLevels  = 0;
    std::int64_t readLeaf   = 0;
    test::runOnBoundedStack([&] {
        DiagnosticReporter w;
        text = emitHir(hir, r.context(), w);
        DiagnosticReporter p;
        auto res = parseHir(text, CompilationUnitId{9}, p);
        parsedOk = res != nullptr && res->ok && res->literalPool.size() == 1;
        if (!parsedOk) return;
        // Walk the rebuilt value ITERATIVELY and read the leaf.
        HirLiteralValue const* node = &res->literalPool.at(0);
        while (auto const* agg = std::get_if<HirAggregateValue>(&node->value)) {
            if (agg->fields.size() != 1) break;
            node = &agg->fields[0];
            ++readLevels;
        }
        if (auto const* iv = std::get_if<std::int64_t>(&node->value)) readLeaf = *iv;
        HirTextContext ctx2;
        ctx2.interner    = &res->interner;
        ctx2.symbolNames = &r.names;
        ctx2.literalPool = &res->literalPool;
        DiagnosticReporter w2;
        again = emitHir(res->hir, ctx2, w2);
        // `res` — the rebuilt pool with its 20_000-level value — dies here, on
        // the bounded stack: the teardown is a walk too (`hir_literal_pool.hpp`).
    });
    ASSERT_TRUE(parsedOk) << "the reader must accept the writer's own output";
    EXPECT_EQ(readLevels, kDepth)
        << "every brace level must survive the READ — a truncated literal is a "
           "silently different initializer";
    EXPECT_EQ(readLeaf, 7);
    EXPECT_EQ(again, text) << "the round trip must be byte-identical";
}
