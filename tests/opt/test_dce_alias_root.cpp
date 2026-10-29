// P69 (lane `cs`) — A SECOND NAME OF A DEFINITION ROOTS IT AGAINST DEAD-CODE REMOVAL.
//
// The object producer writes an alias (`Mir::aliases`) as one more symbol row on its target's
// definition, and every rebuild carries the aliases whole — so a target DCE deleted would leave a row
// naming nothing. Every alias roots its target, whatever its own binding: an internal alias's uses
// were bound to the target by the semantic tier, so nothing else would keep a module-private target
// that only an alias names. The pin: a module-private function and a module-private global that
// nothing references survive DCE when an alias names them, and are deleted when none does (CONTROL).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "opt/optimizer.hpp"
#include "opt/passes/dce.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace dss;

namespace {

constexpr SymbolId kExported{100};
constexpr SymbolId kPrivateFn{101};
constexpr SymbolId kPrivateGlobal{102};
constexpr SymbolId kFnAlias{103};
constexpr SymbolId kGlobalAlias{104};

[[nodiscard]] Mir moduleOf(TypeInterner& in, bool withAliases, MirLinkage aliasLinkage) {
    TypeId const voidFn = in.fnSig({}, in.primitive(TypeKind::Void), CallConv::CcSysV);
    MirBuilder   b;
    b.addFunction(voidFn, kExported, SymbolBinding::Global, SymbolVisibility::Default);
    MirBlockId const e1 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e1);
    b.addReturn();
    b.addFunction(voidFn, kPrivateFn, SymbolBinding::Local, SymbolVisibility::Default);
    MirBlockId const e2 = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e2);
    b.addReturn();
    (void)b.addGlobal(in.primitive(TypeKind::I32), kPrivateGlobal, UINT32_MAX, MirFuncId{},
                      MirLinkage{SymbolBinding::Local, SymbolVisibility::Default}, /*isConst=*/false,
                      MirThreadStorage::Shared);
    if (withAliases) {
        b.addAlias(kFnAlias, kPrivateFn, aliasLinkage);
        b.addAlias(kGlobalAlias, kPrivateGlobal, aliasLinkage);
    }
    b.stateSelfContainedSymbolIds();
    return std::move(b).finish();
}

[[nodiscard]] bool definesFunction(Mir const& m, SymbolId s) {
    for (std::uint32_t i = 0; i < m.moduleFuncCount(); ++i) {
        if (m.funcSymbol(m.funcAt(i)) == s) return true;
    }
    return false;
}
[[nodiscard]] bool definesGlobal(Mir const& m, SymbolId s) {
    for (std::uint32_t i = 0; i < m.moduleGlobalCount(); ++i) {
        if (m.globalSymbol(m.globalAt(i)) == s) return true;
    }
    return false;
}

}  // namespace

TEST(DceAliasRoot, AnAliasKeepsTheDefinitionItNamesWhateverItsOwnBinding) {
    for (MirLinkage const linkage : {MirLinkage{SymbolBinding::Global, SymbolVisibility::Default},
                                     MirLinkage{SymbolBinding::Local, SymbolVisibility::Default}}) {
        SCOPED_TRACE(linkage.binding == SymbolBinding::Local ? "a module-private alias" : "an external alias");
        TypeInterner       in{CompilationUnitId{1}};
        Mir                m = moduleOf(in, /*withAliases=*/true, linkage);
        DiagnosticReporter rep;
        auto const         r = opt::passes::runDce(m, in, rep);
        ASSERT_TRUE(r.ok);
        EXPECT_TRUE(definesFunction(m, kPrivateFn)) << "the function an alias names is not dead";
        EXPECT_TRUE(definesGlobal(m, kPrivateGlobal)) << "the global an alias names is not dead";
        EXPECT_EQ(m.moduleAliasCount(), 2u) << "the rebuild carries the aliases";
    }
    // CONTROL: the same module with no alias loses both — they are dead, and DCE removes the dead.
    TypeInterner       in{CompilationUnitId{1}};
    Mir                m = moduleOf(in, /*withAliases=*/false, MirLinkage{});
    DiagnosticReporter rep;
    auto const         r = opt::passes::runDce(m, in, rep);
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(definesFunction(m, kPrivateFn));
    EXPECT_FALSE(definesGlobal(m, kPrivateGlobal));
    EXPECT_TRUE(definesFunction(m, kExported));
}
