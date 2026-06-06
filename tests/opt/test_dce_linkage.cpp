// D-OPT2-DCE-LINKAGE-SYMTAB-ASSERTION (plan 22 §3.1).
//
// The orthogonal-to-behavioral DCE pin. Behavioral pins catch DCE
// eliding a *live* store via exit-code distance; THIS pin catches
// DCE deleting an *exported-but-internally-unused* symbol via
// linkage attribute survival.
//
// Why a unit test (not corpus example):
//   * PE .exe emits zero IMAGE_SYMBOL table (`pe.cpp:1099-1100`).
//     A corpus-runner symbol-table-inspection check would need
//     FF1-PE for .exe (not shipped — FF1-PE is anchored).
//   * c-subset has no `static` keyword (no parser path produces
//     SymbolBinding::Local). The MIR-tier fixture must be hand-built.
//   * MIR-tier inspection is format-blind + works on every host.
//     The `isExternallyVisible(funcBinding, funcVisibility)` predicate
//     IS the contract DCE consults; this test asserts DCE honors it.

#include "core/types/diagnostic_reporter.hpp"
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
