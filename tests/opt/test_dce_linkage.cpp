// D-OPT2-DCE-LINKAGE-SYMTAB-ASSERTION (plan 22 §3.1).
//
// The orthogonal-to-behavioral DCE pin. Behavioral pins catch DCE
// eliding a *live* store via exit-code distance; THIS pin catches
// DCE deleting an *exported-but-internally-unused* symbol via
// linkage attribute survival.
//
// Why a unit test (not corpus example):
//   * PE .exe emits zero IMAGE_SYMBOL table (`pe.cpp`).
//     A corpus-runner symbol-table-inspection check would need
//     FF1-PE for .exe (not shipped — FF1-PE is anchored).
//   * c has no `static` keyword (no parser path produces
//     SymbolBinding::Local). The MIR-tier fixture must be hand-built.
//   * MIR-tier inspection is format-blind + works on every host.
//     The `isExternallyVisible(funcBinding, funcVisibility)` predicate
//     IS the contract DCE consults; this test asserts DCE honors it.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/section_kind.hpp"   // StaticInitSchedule / StaticInitPhase
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "opt/optimizer.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace dss;

namespace {

struct LinkageFixture {
    Mir          mir;
    TypeId       voidFn;
    SymbolId     exportedSym;
    SymbolId     deadLocalSym;
};

// Build a 2-function module:
//   * exported_no_callers: SymbolBinding::Global, SymbolVisibility::Default
//     — externally visible BUT has zero internal callers. Without the
//     linkage protect, DCE would happily delete it as "unreachable."
//   * dead_local: SymbolBinding::Local — DCE-eligible. Has zero
//     callers AND non-external — must be elided.
LinkageFixture buildExportedAndDead(TypeInterner& interner) {
    LinkageFixture f;
    f.voidFn = interner.fnSig({}, interner.primitive(TypeKind::Void),
                              CallConv::CcSysV);
    f.exportedSym  = SymbolId{100};
    f.deadLocalSym = SymbolId{101};

    MirBuilder mb;
    // Function 1: exported_no_callers — Global/Default = externally
    // visible. Body: just return.
    mb.addFunction(f.voidFn, f.exportedSym,
                   SymbolBinding::Global, SymbolVisibility::Default);
    MirBlockId const eb1 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(eb1);
    mb.addReturn();

    // Function 2: dead_local — Local binding = DCE-eligible. Body:
    // just return.
    mb.addFunction(f.voidFn, f.deadLocalSym,
                   SymbolBinding::Local, SymbolVisibility::Default);
    MirBlockId const eb2 = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(eb2);
    mb.addReturn();

    f.mir = std::move(mb).finish();
    return f;
}

// Confirm exactly one function with the given SymbolId survives in
// the rebuilt module.
bool moduleContainsFuncSymbol(Mir const& mir, SymbolId sym) {
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        if (mir.funcSymbol(mir.funcAt(i)) == sym) return true;
    }
    return false;
}

} // namespace

// The core linkage-protect contract: an externally-visible function
// with zero internal callers MUST survive DCE. A buggy DCE that only
// considers intra-module reachability would delete it. The linkage
// predicate `isExternallyVisible(binding, visibility) == true` is
// the explicit guard.
TEST(DceLinkage, ExportedFunctionSurvivesEvenWithNoCallers) {
    TypeInterner interner{CompilationUnitId{1}};
    auto fx = buildExportedAndDead(interner);

    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    DiagnosticReporter rep;
    opt::OptPipeline pipeline{"linkage-test", {opt::PassId::Dce}};
    auto const result = opt::optimize(fx.mir, target, interner, pipeline, rep);
    ASSERT_TRUE(result.ok) << "DCE pass must complete";
    EXPECT_EQ(rep.errorCount(), 0u);

    // The Global/Default function MUST be present — its linkage
    // attribute (externally-visible) protects it from DCE.
    EXPECT_TRUE(moduleContainsFuncSymbol(fx.mir, fx.exportedSym))
        << "exported_no_callers (Global/Default) survives DCE per "
           "D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD contract";
}

// Same fixture, the orthogonal half: a Local-binding function with
// zero callers MUST be eliminated. This pins the OTHER direction —
// DCE actually does remove unused symbols when their linkage permits.
TEST(DceLinkage, LocalFunctionWithNoCallersIsEliminated) {
    TypeInterner interner{CompilationUnitId{1}};
    auto fx = buildExportedAndDead(interner);

    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    DiagnosticReporter rep;
    opt::OptPipeline pipeline{"linkage-test", {opt::PassId::Dce}};
    auto const result = opt::optimize(fx.mir, target, interner, pipeline, rep);
    ASSERT_TRUE(result.ok);

    EXPECT_FALSE(moduleContainsFuncSymbol(fx.mir, fx.deadLocalSym))
        << "dead_local (Local binding, no callers) MUST be eliminated";
}

// The predicate that gates the DCE protect. Reading it back from
// the surviving function's MirFuncId confirms DCE didn't corrupt
// the binding/visibility while rebuilding.
TEST(DceLinkage, PreservesBindingVisibilityOnSurvivor) {
    TypeInterner interner{CompilationUnitId{1}};
    auto fx = buildExportedAndDead(interner);

    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    DiagnosticReporter rep;
    opt::OptPipeline pipeline{"linkage-test", {opt::PassId::Dce}};
    (void)opt::optimize(fx.mir, target, interner, pipeline, rep);

    // Find the exported survivor + check its attributes.
    MirFuncId survivor;
    std::size_t const nf = fx.mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        if (fx.mir.funcSymbol(fx.mir.funcAt(i)) == fx.exportedSym) {
            survivor = fx.mir.funcAt(i);
            break;
        }
    }
    ASSERT_TRUE(survivor.valid());
    EXPECT_EQ(fx.mir.funcBinding(survivor),    SymbolBinding::Global);
    EXPECT_EQ(fx.mir.funcVisibility(survivor), SymbolVisibility::Default);
    EXPECT_TRUE(isExternallyVisible(fx.mir.funcBinding(survivor),
                                    fx.mir.funcVisibility(survivor)));
}

// ── D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN ─────────────
//
// A STATIC INITIALIZER IS A THIRD KIND OF ROOT, and without it the whole feature
// is deleted at `--config=release`.
//
// ★★ WHY IT DOES NOT FALL OUT OF EITHER EXISTING ROUTE. `runDce` keeps a function
// iff it is externally visible, is named by a `GlobalAddr` in a live function, or
// is named by a live global's initializer. A `__attribute__((constructor))`
// function written the way C code actually writes it — `static`, so Local binding
// — satisfies NONE of those: being called from nowhere in the program text is
// what it is FOR. It is reached by the RUNTIME, which is not an edge this graph
// has.
//
// ★ AND THE FAILURE IS CONFIG-SPLIT, WHICH IS THE WORST SHAPE. The debug pipeline
// is `Identity`, so a missing root clause is invisible in every debug build and
// every debug test; only a release build loses the initializer. The corpus
// example carries a release arm for the same reason, and these two pins are what
// name the cause when it does.
TEST(DceLinkage, StaticInitializerFunctionSurvivesDespiteLocalBindingAndNoCallers) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidFn = interner.fnSig({}, interner.primitive(TypeKind::Void),
                                         CallConv::CcSysV);
    SymbolId const ctorSym = SymbolId{200};
    SymbolId const dtorSym = SymbolId{201};
    SymbolId const plainSym = SymbolId{202};

    StaticInitSchedule beforeEntry;
    beforeEntry.setPriorityFor(StaticInitPhase::BeforeEntry, 101);
    StaticInitSchedule afterEntry;
    afterEntry.setPriorityFor(StaticInitPhase::AfterEntry,
                              kUnprioritizedStaticInit);

    MirBuilder mb;
    mb.addFunction(voidFn, ctorSym, SymbolBinding::Local,
                   SymbolVisibility::Default, false, false, false, false,
                   beforeEntry);
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    mb.addReturn();
    mb.addFunction(voidFn, dtorSym, SymbolBinding::Local,
                   SymbolVisibility::Default, false, false, false, false,
                   afterEntry);
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    mb.addReturn();
    // THE CONTROL, in the same module: byte-identical in every respect EXCEPT the
    // schedule. Without it a DCE that had simply stopped deleting anything would
    // pass both assertions above.
    mb.addFunction(voidFn, plainSym, SymbolBinding::Local,
                   SymbolVisibility::Default);
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    DiagnosticReporter rep;
    opt::OptPipeline pipeline{"static-init-root-test", {opt::PassId::Dce}};
    auto const result = opt::optimize(mir, **targetR, interner, pipeline, rep);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(rep.errorCount(), 0u);

    EXPECT_TRUE(moduleContainsFuncSymbol(mir, ctorSym))
        << "a BEFORE-entry initializer is a root: it is called by the runtime, "
           "not by the program, so no liveness edge reaches it";
    EXPECT_TRUE(moduleContainsFuncSymbol(mir, dtorSym))
        << "…and so is an AFTER-entry one. The root clause asks `staticInit."
           "any()` rather than testing one channel precisely so this half cannot "
           "be lost while the constructor half stays green";
    EXPECT_FALSE(moduleContainsFuncSymbol(mir, plainSym))
        << "THE CONTROL: an identical Local function with no schedule must still "
           "be eliminated — otherwise the two pins above prove only that DCE "
           "stopped working";
}

// The schedule must SURVIVE the rebuild, not merely protect the function from it.
// `runDce` clones every live function through the shared rebuild helper, and a
// clone that dropped the schedule would produce a module whose initializer is
// present, unreferenced, and no longer scheduled — so the linker emits no call
// and the next pass's DCE deletes it outright.
TEST(DceLinkage, StaticInitScheduleSurvivesTheDceRebuild) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const voidFn = interner.fnSig({}, interner.primitive(TypeKind::Void),
                                         CallConv::CcSysV);
    SymbolId const ctorSym = SymbolId{300};
    StaticInitSchedule sched;
    sched.setPriorityFor(StaticInitPhase::BeforeEntry, 137);

    MirBuilder mb;
    mb.addFunction(voidFn, ctorSym, SymbolBinding::Local,
                   SymbolVisibility::Default, false, false, false, false, sched);
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    mb.addReturn();
    Mir mir = std::move(mb).finish();

    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    DiagnosticReporter rep;
    opt::OptPipeline pipeline{"static-init-carry-test", {opt::PassId::Dce}};
    ASSERT_TRUE(opt::optimize(mir, **targetR, interner, pipeline, rep).ok);

    bool seen = false;
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f) != ctorSym) continue;
        seen = true;
        auto const after = mir.funcStaticInit(f);
        ASSERT_TRUE(after.beforeEntry().has_value())
            << "the rebuilt function kept no schedule — it would be emitted and "
               "never called";
        EXPECT_EQ(*after.beforeEntry(), 137u)
            << "…and the PRIORITY has to survive too: keeping the fact while "
               "losing the position reads as working until the order matters";
    }
    EXPECT_TRUE(seen);
}
