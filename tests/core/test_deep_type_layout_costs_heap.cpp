// ── DEPTH MUST COST HEAP, NOT HOST CALL FRAMES — THE TYPE/CONST-EVAL TIER ────
//
// Sibling of `tests/mir/test_deep_nesting_costs_heap.cpp`, one tier BELOW it.
// Row: D-CORE-TYPE-LAYOUT-AND-HIR-CONST-EVAL-RECURSE-PER-LEVEL
//
// ★★ EVERY TEST HERE RUNS ON THE ORDINARY gtest MAIN THREAD (~1 MiB). Nothing
// calls `callOnLargeStack`; a pin taken on the 64 MiB worker proves nothing.
//
// ★★ ITS OWN BINARY ON PURPOSE — a red here reds by EXHAUSTING THE STACK, which
// kills the process with no `[  FAILED  ]` line, and would take every sibling in
// a shared executable down unattributably with it.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "core/types/type_lattice/type_reintern.hpp"   // CompositeIdentityIndex (the spine_ pin)
#include "bounded_stack.hpp"                            // runOnBoundedStack — the BOUND
#include "hir/const_eval.hpp"
#include "hir/hir.hpp"
#include "hir/hir_literal_pool.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_text.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace dss;

namespace {

// PROBE KNOB. Every case below takes its depth from `DSS_TL_DEPTH` when it is
// set, so a ceiling can be bisected without a rebuild; unset, each case uses its
// own pinned constant. The pinned constant is what ships.
[[nodiscard]] int depthOr(int pinned) {
    char const* const e = std::getenv("DSS_TL_DEPTH");
    if (e == nullptr || *e == '\0') return pinned;
    int const v = std::atoi(e);
    return v > 0 ? v : pinned;
}

[[nodiscard]] AggregateLayoutParams params() {
    return AggregateLayoutParams{ScalarAlignmentRule::Natural, 16};
}

// A nested-ARRAY chain: `array(array(...(i32,1)...,1),1)`, one `computeLayout`
// TYPE level per link, built ITERATIVELY so the fixture cannot red for its own
// reason. Array interning takes no field span, so nothing about the fixture is
// itself a per-level recursion.
[[nodiscard]] TypeId nestedArrays(TypeInterner& ti, int depth) {
    TypeId cur = ti.primitive(TypeKind::I32);
    for (int i = 0; i < depth; ++i) cur = ti.array(cur, 1);
    return cur;
}

// A nested-STRUCT chain: `struct S_n { struct S_{n-1} m; }`, bottoming out at
// `struct S_0 { int x; }`. Built bottom-up and iteratively.
[[nodiscard]] TypeId nestedStructs(TypeInterner& ti, int depth) {
    TypeId const i32 = ti.primitive(TypeKind::I32);
    std::array<TypeId, 1> f{i32};
    TypeId cur = ti.structType("S0", f);
    for (int i = 1; i <= depth; ++i) {
        f[0] = cur;
        cur  = ti.structType("S" + std::to_string(i), f);
    }
    return cur;
}

// Everything one of the C-source pins needs, lowered on THIS thread. Mirrors
// `tests/mir/test_deep_nesting_costs_heap.cpp`'s `lowerC` — deliberately the
// SAME scaffolding, because the leverage claim is about that file's ceilings.
struct Lowered {
    std::shared_ptr<GrammarSchema const> schema;
    std::shared_ptr<TargetSchema const>  target;
    SemanticModel                        model;
    std::unique_ptr<CstToHirResult>      hir;
    HirToMirResult                       mir;
};

// ⚠ SMALL AND BOUNDED ON PURPOSE: `analyze`'s own worker defaults to 64 MiB and
// would absorb the very recursion under test.
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

[[nodiscard]] Lowered lowerC(std::string src, std::size_t exprDepthCap) {
    std::shared_ptr<GrammarSchema const> schema = shipped().schema;
    std::shared_ptr<TargetSchema const>  target = shipped().target;

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
    mirCfg.globalsAllowFloat     = schema->hirLowering().globalsConstEval.allowFloat;
    mirCfg.nonObjectTypeSizes    = schema->semantics().nonObjectTypeSizes;
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

// The `MirTextDeepNesting.DeeplyNestedAggregateLiteralRendersOnAnOrdinaryThread`
// shape, verbatim: N nested struct DEFINITIONS plus a global initializer braced
// N deep. Its ceiling is set by BOTH sites this row owns — the TYPE nest by
// `computeLayout`, the initializer by `const_eval` — and by neither of them in
// `src/mir/**`, which is what makes it the leverage measurement.
[[nodiscard]] std::string nestedStructGlobalInitSource(int depth) {
    std::string src;
    for (int i = 0; i < depth; ++i) src += "struct S" + std::to_string(i) + " { ";
    src += "int x;";
    for (int i = depth - 1; i >= 0; --i) src += " } m" + std::to_string(i) + ";";
    src += "\nstruct S0 g = ";
    for (int i = 0; i < depth; ++i) src += "{";
    src += "3";
    for (int i = 0; i < depth; ++i) src += "}";
    src += ";\nint main(void){ return 0; }";
    return src;
}

} // namespace

// ── ① computeLayout — the TYPE-level recursion ───────────────────────────────
//
// ✔MEASURED on the ordinary thread, through `ctest`, with the RECURSIVE engine
// in the tree: BOTH shapes below laid out at 1000 with rc 0 and died at 2000
// with rc 8 — no message, no location, no `[  FAILED  ]` line. Converted, the
// array shape reached 100_000 AND 1_000_000 with rc 0 and the struct shape
// 100_000. kDepth is 20_000: 10x the measured crash floor, so restoring the
// recursion does not merely fail these, it kills the process.

TEST(TypeLayoutDeepNesting, NestedArraysLayOutOnAnOrdinaryThread) {
    int const kDepth = depthOr(20000);
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const deep = nestedArrays(ti, kDepth);

    auto const lay = computeLayout(deep, ti, params(), DataModel::Lp64);
    ASSERT_TRUE(lay.has_value()) << "a merely-deep array type must lay out";
    EXPECT_EQ(lay->size, 4u) << "array(…,1) of i32 is 4 bytes at every depth";
    EXPECT_EQ(lay->align.bytes(), 4u);
}

TEST(TypeLayoutDeepNesting, NestedStructsLayOutOnAnOrdinaryThread) {
    int const kDepth = depthOr(20000);
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const deep = nestedStructs(ti, kDepth);

    auto const lay = computeLayout(deep, ti, params(), DataModel::Lp64);
    ASSERT_TRUE(lay.has_value()) << "a merely-deep struct nest must lay out";
    EXPECT_EQ(lay->size, 4u);
    EXPECT_EQ(lay->align.bytes(), 4u);
}

// ── ① the CYCLE — a different defect from depth ──────────────────────────────
//
// ⚠⚠ NOT A DEPTH PIN, AND THE DISTINCTION IS THE POINT. A by-value cycle has no
// depth to bound: the walk has no base case at all. ✔MEASURED with the
// RECURSIVE engine in the tree, through `ctest`: BOTH shapes below killed the
// process at their minimum size, while the self-POINTER control laid out fine —
// so a bigger stack fixes neither, and a work stack WITHOUT the visited set
// would have turned the crash into a hang, which is worse.

TEST(TypeLayoutCycle, SelfByValueCompositeIsRefusedNotACrash) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const a = ti.forwardComposite(TypeKind::Struct, "A", /*declSiteKey=*/1);
    std::array<TypeId, 1> f{a};
    ti.completeComposite(a, f, /*packed=*/false);

    auto const lay = computeLayout(a, ti, params(), DataModel::Lp64);
    EXPECT_FALSE(lay.has_value())
        << "a struct that contains itself BY VALUE has no size — the engine must "
           "refuse (nullopt), never loop and never crash";
}

TEST(TypeLayoutCycle, MutualByValueCompositesAreRefusedNotACrash) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const a = ti.forwardComposite(TypeKind::Struct, "A", /*declSiteKey=*/1);
    TypeId const b = ti.forwardComposite(TypeKind::Struct, "B", /*declSiteKey=*/2);
    std::array<TypeId, 1> fa{b};
    std::array<TypeId, 1> fb{a};
    ti.completeComposite(a, fa, /*packed=*/false);
    ti.completeComposite(b, fb, /*packed=*/false);

    EXPECT_FALSE(computeLayout(a, ti, params(), DataModel::Lp64).has_value());
    EXPECT_FALSE(computeLayout(b, ti, params(), DataModel::Lp64).has_value());
}

// The complement: a POINTER back-reference is NOT a cycle for layout (a pointer
// is a scalar), so `struct S { int v; struct S *next; }` must lay out normally.
// Without this control the two cases above could be satisfied by refusing every
// composite that names itself at all.
TEST(TypeLayoutCycle, SelfPointerCompositeStillLaysOut) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "S", /*declSiteKey=*/1);
    std::array<TypeId, 2> f{ti.primitive(TypeKind::I32), ti.pointer(s)};
    ti.completeComposite(s, f, /*packed=*/false);

    auto const lay = computeLayout(s, ti, params(), DataModel::Lp64);
    ASSERT_TRUE(lay.has_value())
        << "a self-POINTER is the commonest struct shape in C and has a layout";
    EXPECT_EQ(lay->size, 16u);
    EXPECT_EQ(lay->align.bytes(), 8u);
}

// ── ③ representationType — the OBJECT-REPRESENTATION projection ────────
//
// D-TYPEINTERNER-REPRESENTATIONTYPE-RECURSES-PER-TYPE-LEVEL-UNCAPPED. Its
// qualifier peel and its three composer arms (Ptr / Array / FnSig) were one host
// frame per TYPE LEVEL. ✔MEASURED by P55 lane `hs` from C source through the
// front end: 4297 rc 0 / 4453 SEGFAULT on the ordinary thread, gdb-attributed to
// this walk (and `stop=analyze` reaching 19378, so not the parse).
//
// These two drive the interner DIRECTLY, which isolates the walk from the front
// end that reaches it — and separates its TWO paths, which fail differently:
// the SCAN (nothing to project, the ~100% case) and the REBUILD (a `nullptr_t`
// really is in there). A pin on only the first would leave the whole work stack
// unexercised.
//
// ✔RE-DERIVED HERE on the ordinary thread, through `ctest`, with the RECURSIVE
// projection in the tree: BOTH cases below reached 4000 with rc 0 and died at
// 6000 with rc 8. That does NOT refute lane `hs`'s 4297/4453 — theirs was taken
// through the whole front end, which spends frames of its own before the walk
// starts, so a slightly lower ceiling on that route is what a shared cause
// predicts. Converted, both reach 100_000 with rc 0; kDepth is 20_000, over 3x
// the crash floor measured here.

// The scan path AND the identity contract in one: a type with no `nullptr_t`
// inside must come back as the SAME TypeId, not a fresh-but-equal one. This
// query runs once per identifier in every translation unit, so an answer that
// re-interned would be a per-identifier cost on every compile.
TEST(RepresentationProjectionDeepNesting, DeepPointerChainIsIdentityOnAnOrdinaryThread) {
    int const kDepth = depthOr(20000);
    TypeInterner ti{CompilationUnitId{1}};
    TypeId chain = ti.primitive(TypeKind::I32);
    for (int i = 0; i < kDepth; ++i) chain = ti.pointer(chain);

    EXPECT_EQ(ti.representationType(chain).v, chain.v)
        << "a type with no nullptr_t inside must come back as the SAME TypeId";
}

// The rebuild path: the same depth over a `nullptr_t` bottom, so every level
// really is reconstructed. The expected answer is built ITERATIVELY here for the
// same reason the fixtures above are — so the test cannot red for its own reason.
TEST(RepresentationProjectionDeepNesting, DeepPointerChainOverNullptrTRebuildsOnAnOrdinaryThread) {
    int const kDepth = depthOr(20000);
    TypeInterner ti{CompilationUnitId{1}};
    TypeId chain = ti.primitive(TypeKind::NullptrT);
    for (int i = 0; i < kDepth; ++i) chain = ti.pointer(chain);

    TypeId want = ti.pointer(ti.primitive(TypeKind::Void));   // nullptr_t -> void *
    for (int i = 0; i < kDepth; ++i) want = ti.pointer(want);

    TypeId const got = ti.representationType(chain);
    EXPECT_NE(got.v, chain.v) << "a nullptr_t bottom must actually be projected";
    EXPECT_EQ(got.v, want.v)
        << "every level must be rebuilt over the projected bottom — a short walk "
           "is a silently different type, not a cosmetic difference";
}

// ── ④ CompositeIdentityIndex::spine_ — the cross-CU reintern's field digest ──
//
// D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED. The
// index describes every composite's fields DOWN TO but not THROUGH the composites
// they reach, so a field's structural nesting — a pointer chain, an array of
// arrays — is one `spine_` level per constructor, and the walk used to call
// itself at the volatile peel and once per operand. It runs only when a
// cross-CU merge observes an interner, which is why no C-source probe reached
// it and the census carried it UNMEASURED. This drives `observe` directly: a
// struct whose one field is a kDepth-deep pointer chain, so the digest walks
// exactly kDepth levels.
//
// ✔MEASURED on the ordinary thread, through `ctest` (P60, lane `rc`), with the
// RECURSIVE walk restored: see the ceiling recorded at the site
// (`type_reintern.cpp`, `spine_`); converted, this reaches kDepth = 100_000.
// The digest is also asserted STABLE across the conversion in the only way that
// is observable: two interners holding the same shape must land on ONE key, and
// a shape one level shallower must land on ANOTHER — a walk that reordered the
// pre-order mix would still "pass" a survival-only pin.
// ★ ON THE BOUNDED STACK (`bounded_stack.hpp`, 256 KiB): the recursive walk
// cost at least one frame per level (two calls' worth of locals — well over
// 64 bytes on any toolchain), so 100_000 levels need ≥ 6 MiB against a 256 KiB
// reserve. Restore the recursion and the process dies, on every leg.
TEST(CompositeIdentityIndexDeepNesting, SpineOverADeepPointerFieldCostsHeap) {
    int const kDepth = depthOr(100000);
    auto build = [&](std::uint32_t owner, int depth) {
        auto ti = std::make_unique<TypeInterner>(CompilationUnitId{owner});
        TypeId chain = ti->primitive(TypeKind::I32);
        for (int i = 0; i < depth; ++i) chain = ti->pointer(chain);
        std::array<TypeId, 1> f{chain};
        TypeId const s = ti->structType("Deep", f);
        return std::pair{std::move(ti), s};
    };
    auto [a, sa] = build(1, kDepth);
    auto [b, sb] = build(2, kDepth);
    auto [c, sc] = build(3, kDepth - 1);   // one level shallower: a DIFFERENT type

    std::uint64_t ka = 0, kb = 0, kc = 0;
    test::runOnBoundedStack([&] {
        CompositeIdentityIndex index;
        index.observe(*a);
        index.observe(*b);
        index.observe(*c);
        ka = index.keyFor(*a, sa);
        kb = index.keyFor(*b, sb);
        kc = index.keyFor(*c, sc);
    });
    EXPECT_EQ(ka, kb)
        << "the same field shape in two CUs must digest to ONE identity key";
    EXPECT_NE(ka, kc)
        << "a chain one level shallower is a different type and must not merge "
           "— a digest that lost a level would silently fork or fuse layouts";
}

// ── ② const_eval — the evalNode ⇄ evalImpl recursion ─────────────────────────
//
// ✔MEASURED on the ordinary thread, through `ctest`, with the DELEGATED arms
// still recursive: all THREE shapes below folded at 400 with rc 0 and died at
// 1000 with rc 8. Converted, all three reach 100_000 with rc 0. kDepth is
// 20_000 — 20x the measured crash floor.

namespace {

// Nested `ConstructAggregate` all sharing ONE shallow `array(i32,1)` type, so
// HIR depth grows while TYPE depth does not — the isolation
// `HirToMirAggregateInit.TwentyThousandBraceLevelsCostHeapNotCallFrames` uses,
// for the same reason: otherwise `computeLayout` decides the ceiling instead.
struct ConstEvalRig {
    TypeInterner   interner{CompilationUnitId{1}};
    HirLiteralPool literals{};
    HirBuilder     builder{"c"};
};

} // namespace

// ★ ON THE BOUNDED STACK (`bounded_stack.hpp`, 256 KiB) — the fold, then the
// COPY, then the TEARDOWN of a 20_000-level value all run on it: the
// recursive `evalNode ⇄ evalImpl` cost ~1 KiB per level on mingw-w64 g++
// Debug (400 ok / 1000 crash on 1 MiB), the recursive teardown ~300 bytes,
// the recursive copy more than either; at 20_000 levels every one of them
// needs megabytes against 256 KiB, so restoring any of the three kills the
// process on every leg.
TEST(ConstEvalDeepNesting, NestedAggregatesFoldOnABoundedStack) {
    int const kDepth = depthOr(20000);
    ConstEvalRig r;
    TypeId const i32  = r.interner.primitive(TypeKind::I32);
    TypeId const arr1 = r.interner.array(i32, 1);

    HirLiteralValue three;
    three.core  = TypeKind::I32;
    three.value = std::int64_t{3};
    HirNodeId cur = r.builder.makeLiteral(i32, r.literals.add(three));
    for (int i = 0; i < kDepth; ++i)
        cur = r.builder.makeConstructAggregate(std::array{cur}, arr1,
                                               HirFlags::Synthetic);

    HirNodeId const root = cur;
    Hir hir = std::move(r.builder).finish(root);
    bool folded = false;
    int  copiedLevels = 0;
    test::runOnBoundedStack([&] {
        ConstEvalResult res = evaluateConstant(hir, r.interner, r.literals, root);
        folded = res.value.has_value();
        if (!folded) return;

        // ★★ THE COPY IS A WALK TOO. `HirAggregateValue`'s copy constructor
        // used to be the compiler's own — `vector` copy → variant copy →
        // nested `HirAggregateValue` copy, one host frame chain per level —
        // exactly the shape ✔MEASURED 2026-09-04 (P60, lane `rc`) on the MIR
        // twin, where `emitGlobals_` copying a 1000-level pool literal died
        // 13 848 frames deep under MSVC 19.51 Debug once the teardown stopped
        // being the first walk to overflow. Copy the whole value here and walk
        // the copy ITERATIVELY to prove it is complete — a copy that silently
        // stopped short would be a wrong initializer, not a crash.
        HirLiteralValue const copy = *res.value;
        HirLiteralValue const* node = &copy;
        while (auto const* agg = std::get_if<HirAggregateValue>(&node->value)) {
            if (agg->fields.size() != 1) break;
            node = &agg->fields[0];
            ++copiedLevels;
        }

        // ★★ AND THE TEARDOWN IS A WALK TOO. `res` and `copy` die here, at
        // full depth: `~HirLiteralValue → ~variant → ~HirAggregateValue →
        // ~vector → ~HirLiteralValue` is a walk the standard library generates,
        // and until P60 it cost one host frame chain per aggregate level — this
        // case used to UNLINK the spine by hand so that its depth measured the
        // fold and not the destructor. ✔MEASURED 2026-09-04 (P60, lane `rc`),
        // gdb-attributed under MSVC 19.51 Debug: that chain is 19 frames /
        // ~1160 bytes per level and a 1000-level global initializer died in it
        // at ~878 levels; under mingw-w64 g++ 13.2 the row records 1000–4000.
        // `hir_literal_pool.hpp` now tears an aggregate down — and copies one —
        // on an explicit heap work list.
    });
    ASSERT_TRUE(folded) << "a deep aggregate initializer must fold, not crash";
    EXPECT_EQ(copiedLevels, kDepth)
        << "the copy must reproduce every level — a short copy is a silently "
           "different initializer";
}

// ⚠ EVERY LEVEL MINTS ITS OWN LITERAL NODE. Reusing one `HirNodeId` as two
// children builds a DAG, and `HirBuilder` refuses that — MEASURED: the first
// draft of these two cases died with 0xC0000409 at depth FIVE, which would have
// read as the engine's ceiling if the shallow arm had not been probed.
TEST(ConstEvalDeepNesting, NestedTernariesFoldOnAnOrdinaryThread) {
    int const kDepth = depthOr(20000);
    ConstEvalRig r;
    TypeId const i32 = r.interner.primitive(TypeKind::I32);

    HirLiteralValue one;
    one.core  = TypeKind::I32;
    one.value = std::int64_t{1};
    std::uint32_t const oneIdx = r.literals.add(one);
    auto lit = [&] { return r.builder.makeLiteral(i32, oneIdx); };

    HirNodeId cur = lit();
    for (int i = 0; i < kDepth; ++i)
        cur = r.builder.makeTernary(lit(), cur, lit(), i32);

    HirNodeId const root = cur;
    Hir hir = std::move(r.builder).finish(root);
    ConstEvalResult res = evaluateConstant(hir, r.interner, r.literals, root);
    ASSERT_TRUE(res.value.has_value())
        << "a deep ternary spine must fold, not crash";
}

TEST(ConstEvalDeepNesting, NestedLogicalsFoldOnAnOrdinaryThread) {
    int const kDepth = depthOr(20000);
    ConstEvalRig r;
    TypeId const i32   = r.interner.primitive(TypeKind::I32);
    TypeId const boolT = r.interner.primitive(TypeKind::Bool);

    HirLiteralValue one;
    one.core  = TypeKind::I32;
    one.value = std::int64_t{1};
    std::uint32_t const oneIdx = r.literals.add(one);
    auto lit = [&] { return r.builder.makeLiteral(i32, oneIdx); };

    HirNodeId cur = lit();
    for (int i = 0; i < kDepth; ++i)
        cur = r.builder.makeLogicalAnd(lit(), cur, boolT);

    HirNodeId const root = cur;
    Hir hir = std::move(r.builder).finish(root);
    ConstEvalResult res = evaluateConstant(hir, r.interner, r.literals, root);
    ASSERT_TRUE(res.value.has_value())
        << "a deep && spine must fold, not crash";
}

// ── ★ THE LEVERAGE MEASUREMENT ───────────────────────────────────────────────
//
// Both cases below live entirely in code this row does not touch. Their ceilings
// are set by ① / ② and by nothing in `src/mir/**` — that is the claim, and these
// are how it is measured.

// Route 1: hand-built HIR whose TYPES nest, through `lowerToMir`. The MIR
// aggregate-init cluster is already flattened (P55 lane `rc`, 1_000_000 rc 0);
// what stopped this one was `computeLayout`.
//
// ✔MEASURED, ordinary thread, through `ctest`, same fixture, same binary:
//   * RECURSIVE `computeLayout`: 1000 rc 0, **2000 rc 8**;
//   * CONVERTED: 2000 AND 20_000 both rc 0.
// ⚠ kDepth is 4000 rather than 20_000 for RUNTIME, not for safety: this route is
// superlinear in the nesting (✔MEASURED 3000 -> 5.3 s, 4000 -> 10.2 s,
// 6000 -> 25.0 s), and the cost is in `lowerToMir`, not in the layout engine.
// 4000 is still 2x the measured crash floor.
TEST(LayoutLeverage, HirToMirWithDeeplyNestedTypesLowersOnAnOrdinaryThread) {
    int const kDepth = depthOr(4000);
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const deep = nestedArrays(ti, kDepth);

    HirBuilder      b{"c"};
    HirLiteralPool  pool;
    HirLiteralValue three;
    three.core  = TypeKind::I32;
    three.value = std::int64_t{3};
    HirLiteralValue zero;
    zero.core  = TypeKind::I32;
    zero.value = std::int64_t{0};

    // One ConstructAggregate per TYPE level, each tagged with the array type of
    // that level — so HIR depth and TYPE depth advance together.
    std::vector<TypeId> levels;
    levels.reserve(static_cast<std::size_t>(kDepth));
    {
        TypeId cur = i32;
        for (int i = 0; i < kDepth; ++i) {
            cur = ti.array(cur, 1);
            levels.push_back(cur);
        }
    }
    HirNodeId cur = b.makeLiteral(i32, pool.add(three));
    for (int i = 0; i < kDepth; ++i)
        cur = b.makeConstructAggregate(std::array{cur}, levels[static_cast<std::size_t>(i)],
                                       HirFlags::Synthetic);

    HirNodeId const decl = b.makeVarDecl(deep, /*symbol=*/1, cur);
    HirNodeId const ret  = b.makeReturn(b.makeLiteral(i32, pool.add(zero)));
    HirNodeId const body = b.makeBlock(std::array{decl, ret});
    HirNodeId const fn   = b.makeFunction(ti.fnSig({}, i32, CallConv::CcSysV),
                                          /*symbol=*/2, {}, body);
    Hir hir = std::move(b).finish(b.makeModule(std::array{fn}));

    MirLoweringConfig cfg;
    cfg.aggregateLayout       = params();
    cfg.aggregateLayoutLoaded = true;
    cfg.dataModel             = DataModel::Lp64;
    DiagnosticReporter reporter;
    HirToMirResult res = lowerToMir(hir, pool, ti, reporter,
                                    /*sourceMap=*/nullptr, cfg);
    EXPECT_EQ(reporter.errorCount(), 0u);
    ASSERT_GT(res.mir.funcCount(), 0u);
}

// Route 2: REAL C SOURCE, the exact shape
// `MirTextDeepNesting.DeeplyNestedAggregateLiteralRendersOnAnOrdinaryThread`
// pins at 400 — nested struct definitions plus a global initializer, and the
// only route here a user can actually write.
//
// ✔MEASURED, ordinary thread, through `ctest`, same fixture, same binary:
//   * BEFORE (both sites recursive): 400 rc 0, **1000 rc 8**;
//   * AFTER  (both sites converted): **1000 rc 0**, 2000 rc 8.
// ⇒ a 2.5x move in a route whose every file this row leaves untouched. The 2000
// crash is a DIFFERENT, still-unconverted site: the census records
// `lowerBraceInit` (`cst_to_hir.cpp`) at exactly 1000/2000, and this file has
// separately MEASURED the compiler-generated `~HirLiteralValue` destructor
// (`hir_literal_pool.hpp`) dying between 1000 and 4000 on the same thread.
//
// ⓘ kDepth 1000 is a DIRECT measurement on the unmodified pre-conversion tree,
// not an extrapolation from a mutant: the red-on-disable mutant crashes this
// shape between 500 and 600, i.e. EARLIER than the code it replaces, because it
// spends a frame the original did not. That makes the mutant conservative — it
// cannot make a pin look redder than the real regression would — but it also
// means the mutant's floor is NOT the original's, so this depth is set from the
// unmodified tree's 400-rc-0 / 1000-rc-8 pair and from nothing else.
//
// ⚠⚠ THE ONE-OCTAVE WINDOW THIS CASE USED TO LIVE IN (1000 <= D < 2000) WAS A
// PORTABILITY CLAIM, AND THE FATTER LEG PROVED IT: ✔MEASURED 2026-09-04 (P60,
// lane `rc`) on MSVC 19.51 Debug, kDepth 1000 died of stack overflow on the
// ordinary thread — gdb-attributed, in order, to THREE compiler-generated walks
// nobody had written: `~HirLiteralValue`'s teardown (19 frames, ~1160 bytes
// per level), then, with that converted, the `MirAggregateValue` COPY
// constructor 13 848 frames deep (`emitGlobals_` copying the pool literal),
// then `HirAggregateValue`'s. All three are iterative now
// (`hir_literal_pool.hpp`, `mir_literal_pool.hpp`), and `lowerBraceInit` was
// converted in P55, so the walls that bounded this window are gone.
//
// ★★ SO IT RUNS ON THE BOUNDED STACK (`bounded_stack.hpp`, 256 KiB), AT
// 2000 — twice the old window's top. The arithmetic that makes 2000
// discriminating on the THINNEST supported frames: the recursive teardown
// cost ~300 bytes per level under mingw-w64 g++ Debug (the row's 1000 ok /
// 4000 crash pair on 1 MiB), so a regression needs ≥ 600 KiB here against a
// 256 KiB reserve; the copy chain (~14 frames per level) needs more still.
// Restore either and the process dies — on the Ninja gate too. The schemas are
// loaded on the main thread first (`shipped()`).
TEST(LayoutLeverage, NestedStructGlobalInitLowersOnABoundedStack) {
    int const kDepth = depthOr(2000);
    (void)shipped();   // load on the MAIN thread, never on the bounded one

    std::string text;
    std::size_t errors = 0;
    test::runOnBoundedStack([&] {
        auto L = lowerC(nestedStructGlobalInitSource(kDepth),
                        /*exprDepthCap=*/static_cast<std::size_t>(kDepth) + 1024);
        DiagnosticReporter reporter;
        MirTextContext ctx;
        ctx.interner = &L.model.lattice().interner();
        text   = emitMir(L.mir.mir, ctx, reporter);
        errors = reporter.errorCount();
        // `L` dies here, at full depth, on the bounded stack: the HIR pool's
        // and the MIR pool's teardowns are part of what this pin bounds.
    });
    EXPECT_NE(text.find("lit int 3"), std::string::npos)
        << "the innermost initializer value must reach the text";
    EXPECT_EQ(errors, 0u);
}
