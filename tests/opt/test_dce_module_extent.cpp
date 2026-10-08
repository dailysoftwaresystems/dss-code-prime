// D-OPT-DCE-DELETES-A-RELOCATABLE-MEMBERS-HIDDEN-DEFINITIONS — DCE's roots follow what the module IS to the link that
// completes it (`opt::ModuleExtent`), never the image-level visibility alone.
//
// ✔MEASURED before the extent existed (P69, lane lm): every DCE root read `isExternallyVisible`, an IMAGE question, at
// every stage. A release static-library member lost every hidden definition its unit never called; a release image
// failed K_SymbolUndefined when a SIBLING CU or an object input called a hidden function the defining CU never called
// (release.pipeline.json runs `release-unit`, which holds Dce, on each CU before the merge); a hidden `main` failed its
// own release link. gcc 13.3.0 and clang 18.1.3 keep a hidden function and a hidden datum nothing in their unit
// references at `-O2 -c` (probe-reference-cc run 20261001-152924-ba09ffc4): a hidden definition is still a link-time
// definition that another input resolves by name.
//
// The fixture is hand-built MIR — format-blind, every host — with one definition per root question:
//   hidden_fn / hidden_data   Global + Hidden, unreferenced   the defect's subjects
//   weak_hidden_fn            Weak + Hidden, unreferenced     a weak definition is a link-time definition too
//   local_fn / local_data     Local, unreferenced             the CONTROL: dead at every extent
//   exported_fn               Global + Default, unreferenced  the image lever's control: a root at every extent
//   hidden_entry              Global + Hidden, unreferenced   reached only by the entry trampoline (`entryRoots`)
// The end-to-end faces (a member, an object input, a sibling CU, a hidden main — each linked at release) are pinned by
// tests/program/test_release_hidden_definitions.cpp and run by the examples hidden_definition_across_units_and_entry,
// hidden_definition_in_a_static_library_member and hidden_definition_called_by_an_object_input.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "opt/optimizer.hpp"
#include "opt/passes/dce.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <span>

using namespace dss;

namespace {

constexpr SymbolId kHiddenFn{100};
constexpr SymbolId kWeakHiddenFn{101};
constexpr SymbolId kLocalFn{102};
constexpr SymbolId kExportedFn{103};
constexpr SymbolId kHiddenEntry{104};
constexpr SymbolId kHiddenData{200};
constexpr SymbolId kLocalData{201};

[[nodiscard]] Mir buildModule(TypeInterner& interner) {
    TypeId const i32 = interner.primitive(TypeKind::I32);
    TypeId const sig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirLiteralValue v1; v1.value = std::int64_t{1}; v1.core = TypeKind::I32;
    MirLiteralValue v2; v2.value = std::int64_t{2}; v2.core = TypeKind::I32;
    mb.addGlobal(i32, kHiddenData, mb.literalPoolAdd(v1), MirFuncId{}, SymbolBinding::Global,
                 SymbolVisibility::Hidden, /*isConst=*/false, MirThreadStorage::Shared);
    mb.addGlobal(i32, kLocalData, mb.literalPoolAdd(v2), MirFuncId{}, SymbolBinding::Local,
                 SymbolVisibility::Default, /*isConst=*/false, MirThreadStorage::Shared);
    auto fn = [&](SymbolId sym, SymbolBinding binding, SymbolVisibility visibility) {
        mb.addFunction(sig, sym, binding, visibility);
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirLiteralValue v0; v0.value = std::int64_t{0}; v0.core = TypeKind::I32;
        mb.addReturn(mb.addConst(v0, i32));
    };
    fn(kHiddenFn, SymbolBinding::Global, SymbolVisibility::Hidden);
    fn(kWeakHiddenFn, SymbolBinding::Weak, SymbolVisibility::Hidden);
    fn(kLocalFn, SymbolBinding::Local, SymbolVisibility::Default);
    fn(kExportedFn, SymbolBinding::Global, SymbolVisibility::Default);
    fn(kHiddenEntry, SymbolBinding::Global, SymbolVisibility::Hidden);
    return std::move(mb).finish();
}

[[nodiscard]] bool hasFunc(Mir const& mir, SymbolId sym) {
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        if (mir.funcSymbol(mir.funcAt(i)) == sym) return true;
    }
    return false;
}

[[nodiscard]] bool hasGlobal(Mir const& mir, SymbolId sym) {
    for (std::uint32_t i = 0; i < mir.moduleGlobalCount(); ++i) {
        if (mir.globalSymbol(mir.globalAt(i)) == sym) return true;
    }
    return false;
}

}  // namespace

// A LinkInput shares its link with inputs DCE cannot see, and any of them may name an external-linkage definition:
// every Global/Weak definition is a root, hidden or not. The Local control is still dead — the extent widens the roots
// to EXTERNAL LINKAGE, never to everything.
TEST(DceModuleExtent, ALinkInputKeepsEveryExternalLinkageDefinitionWhateverItsVisibility) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildModule(interner);
    DiagnosticReporter rep;
    auto const r = opt::passes::runDce(mir, interner, rep, opt::ModuleExtent::LinkInput);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_TRUE(hasFunc(mir, kHiddenFn)) << "a hidden definition is a link-time definition another input resolves";
    EXPECT_TRUE(hasFunc(mir, kWeakHiddenFn)) << "a weak hidden definition likewise";
    EXPECT_TRUE(hasGlobal(mir, kHiddenData)) << "a hidden DATUM likewise (DCE's second root table)";
    EXPECT_TRUE(hasFunc(mir, kExportedFn));
    EXPECT_TRUE(hasFunc(mir, kHiddenEntry));
    EXPECT_FALSE(hasFunc(mir, kLocalFn)) << "CONTROL: a Local definition nothing reaches is dead at every extent";
    EXPECT_FALSE(hasGlobal(mir, kLocalData)) << "CONTROL: a Local datum nothing reaches is dead at every extent";
}

// A WholeImage sees every input that can name its definitions: a definition no other IMAGE can see and nothing in the
// module reaches is dead — the lever D-CSUBSET-LINKAGE-VISIBILITY-SYNTAX pinned, kept where it is right. The exported
// definition is the control (a root at every extent).
TEST(DceModuleExtent, AWholeImageDeletesAHiddenDefinitionNothingReaches) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildModule(interner);
    DiagnosticReporter rep;
    auto const r = opt::passes::runDce(mir, interner, rep, opt::ModuleExtent::WholeImage);
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(hasFunc(mir, kHiddenFn));
    EXPECT_FALSE(hasFunc(mir, kWeakHiddenFn));
    EXPECT_FALSE(hasGlobal(mir, kHiddenData));
    EXPECT_FALSE(hasFunc(mir, kHiddenEntry)) << "no entry root handed in: the hidden entry is unreferenced here";
    EXPECT_TRUE(hasFunc(mir, kExportedFn)) << "CONTROL: an externally visible definition is a root at every extent";
    EXPECT_FALSE(hasFunc(mir, kLocalFn));
}

// The image's entry is reached by NAME from the entry trampoline — a link input the driver synthesizes — so a
// WholeImage keeps it whatever its visibility. Only the named entry is rooted: its hidden sibling still goes.
TEST(DceModuleExtent, AWholeImageKeepsTheEntryItIsHandedWhateverItsVisibility) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildModule(interner);
    DiagnosticReporter rep;
    SymbolId const roots[] = {kHiddenEntry};
    auto const r = opt::passes::runDce(mir, interner, rep, opt::ModuleExtent::WholeImage,
                                       std::span<SymbolId const>{roots});
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(hasFunc(mir, kHiddenEntry)) << "a hidden main is still the program's entry";
    EXPECT_FALSE(hasFunc(mir, kHiddenFn)) << "the entry root keeps the entry, not every hidden definition";
}

// The defaults are the hand-built-fixture convention — a module with no context IS the whole program, with no entry
// to keep — so every direct caller of the pass is byte-identical to the pre-extent behaviour. (The production
// chokepoint `optimizeModule` takes both with no default.)
TEST(DceModuleExtent, TheDefaultIsTheWholeImageFixtureConvention) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildModule(interner);
    DiagnosticReporter rep;
    auto const r = opt::passes::runDce(mir, interner, rep);
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(hasFunc(mir, kHiddenFn));
    EXPECT_FALSE(hasGlobal(mir, kHiddenData));
    EXPECT_TRUE(hasFunc(mir, kExportedFn));
}

// The relay: `opt::optimize` hands the extent and the entry roots to the Dce leaf through the schedule interpreter —
// pinned through the ENGINE, so a relay that dropped either one (the leaf would then see the WholeImage default) is a
// red here even though the pass itself is right.
TEST(DceModuleExtent, TheEngineRelaysTheExtentAndTheEntryRootsToTheDceLeaf) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    opt::OptPipeline const pipeline{"dce-module-extent", {opt::PassId::Dce}};
    {
        TypeInterner interner{CompilationUnitId{1}};
        Mir mir = buildModule(interner);
        DiagnosticReporter rep;
        auto const r = opt::optimize(mir, **targetR, interner, pipeline, rep, {}, std::nullopt, {},
                                     opt::ModuleExtent::LinkInput);
        ASSERT_TRUE(r.ok);
        EXPECT_TRUE(hasFunc(mir, kHiddenFn)) << "LinkInput through optimize()";
        EXPECT_TRUE(hasGlobal(mir, kHiddenData)) << "LinkInput through optimize()";
    }
    {
        TypeInterner interner{CompilationUnitId{1}};
        Mir mir = buildModule(interner);
        DiagnosticReporter rep;
        SymbolId const roots[] = {kHiddenEntry};
        auto const r = opt::optimize(mir, **targetR, interner, pipeline, rep, {}, std::nullopt, {},
                                     opt::ModuleExtent::WholeImage, std::span<SymbolId const>{roots});
        ASSERT_TRUE(r.ok);
        EXPECT_TRUE(hasFunc(mir, kHiddenEntry)) << "the entry roots through optimize()";
        EXPECT_FALSE(hasFunc(mir, kHiddenFn)) << "WholeImage through optimize()";
    }
}
