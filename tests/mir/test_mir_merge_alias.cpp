// P69 (lane `cs`) — A SECOND NAME OF A DEFINITION THROUGH THE CROSS-UNIT MERGE (`mergeCuMirs`).
//
// The merge is the live route of every multi-unit build, and it holds one definition per name — the
// one the link's election picks. An alias is a definition of its name there as it is at the unit
// linker: it enters the election with its OWN binding and kind, so
//   * an alias that wins its name is carried as one more name on its target's merged definition, and
//     every unit's reference to the name STAYS a reference row (an alias has no body to call
//     directly; the link resolves the row to the alias);
//   * a WEAK alias yields to another unit's strong definition of the name: it is gone, and the
//     references go to the winner directly, as any loser's do;
//   * a STRONG alias beside another unit's strong definition of the name is two definitions, refused;
//   * a module-private alias is neither winner nor loser, and is kept;
//   * an alias whose TARGET lost its own name is what the LINK's document says the loser's bytes become
//     (`supersededDefinition`): where they are kept (ELF), the alias stays on its own unit's body, kept
//     module-private under an id of its own while the name goes to the winner; where they are replaced
//     whole (the Mach-O documents' statement), the alias names the winning definition.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "link/object_format_schema.hpp"
#include "mir/merge/mir_merge.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dss;

namespace {

using Kind = WeakDefinitionKind;

constexpr MirLinkage kStrong{SymbolBinding::Global, SymbolVisibility::Default};
constexpr MirLinkage kWeak{SymbolBinding::Weak, SymbolVisibility::Default, Kind::Overridable};
constexpr MirLinkage kLocal{SymbolBinding::Local, SymbolVisibility::Default};

[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> shippedFormat(char const* name) {
    auto f = ObjectFormatSchema::loadShipped(name);
    EXPECT_TRUE(f.has_value()) << name;
    return f.has_value() ? *f : nullptr;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  (no diagnostics)"} : s;
}

[[nodiscard]] bool reported(DiagnosticReporter const& rep, DiagnosticCode code, std::string_view text) {
    for (auto const& d : rep.all()) {
        if (d.code == code && d.actual.find(text) != std::string::npos) return true;
    }
    return false;
}

// One unit: named functions, aliases, and reference rows, each symbol named by `names`.
struct Unit {
    TypeInterner                                   interner;
    Mir                                            mir;
    std::vector<ExternImport>                      imports;
    std::unordered_map<std::uint32_t, std::string> names;
    explicit Unit(std::uint32_t cu) : interner{CompilationUnitId{cu}} {}
};

struct Fn {
    std::uint32_t symbol;
    char const*   name;
    MirLinkage    linkage;
};
struct Alias {
    std::uint32_t symbol;
    char const*   name;
    std::uint32_t target;
    MirLinkage    linkage;
};
struct Ref {
    std::uint32_t symbol;
    char const*   name;
};

void build(Unit& u, std::vector<Fn> const& fns, std::vector<Alias> const& aliases, std::vector<Ref> const& refs) {
    TypeId const sig = u.interner.fnSig({}, u.interner.primitive(TypeKind::Void), CallConv::CcSysV);
    MirBuilder   b;
    for (Fn const& f : fns) {
        (void)b.addFunction(sig, SymbolId{f.symbol}, f.linkage);
        MirBlockId const e = b.createBlock(StructCfMarker::EntryBlock);
        b.beginBlock(e);
        b.addReturn();
        u.names[f.symbol] = f.name;
    }
    for (Alias const& a : aliases) {
        b.addAlias(SymbolId{a.symbol}, SymbolId{a.target}, a.linkage);
        u.names[a.symbol] = a.name;
    }
    for (Ref const& r : refs) {
        ExternImport row;
        row.symbol      = SymbolId{r.symbol};
        row.mangledName = r.name;
        u.imports.push_back(std::move(row));
        u.names[r.symbol] = r.name;
    }
    b.stateSelfContainedSymbolIds();
    u.mir = std::move(b).finish();
}

struct Merged {
    std::optional<MergedMirModule> module;
    DiagnosticReporter             rep;
};

void merge(Merged& out, Unit const& a, Unit const& b, char const* linkFormat = "elf64-x86_64-linux-exec") {
    auto const format = shippedFormat(linkFormat);
    ASSERT_NE(format, nullptr);
    std::vector<MergeCuInput> cus(2);
    Unit const* units[2] = {&a, &b};
    for (int i = 0; i < 2; ++i) {
        Unit const& u = *units[i];
        cus[i].mir           = &u.mir;
        cus[i].interner      = &u.interner;
        cus[i].externImports = u.imports;
        cus[i].nameOf        = [&u](SymbolId s) {
            auto const it = u.names.find(s.v);
            return it == u.names.end() ? std::string{} : it->second;
        };
    }
    out.module = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, {}, *format, out.rep);
}

[[nodiscard]] std::string nameIn(MergedMirModule const& m, SymbolId s) {
    auto const it = m.symbolNames.find(s.v);
    return it == m.symbolNames.end() ? std::string{} : it->second;
}

[[nodiscard]] bool definesFunctionNamed(MergedMirModule const& m, std::string_view name) {
    for (std::uint32_t i = 0; i < m.mir.moduleFuncCount(); ++i) {
        if (nameIn(m, m.mir.funcSymbol(m.mir.funcAt(i))) == name) return true;
    }
    return false;
}

// The merged function whose symbol is `s`, or nullptr.
[[nodiscard]] std::optional<MirFuncId> functionOf(MergedMirModule const& m, SymbolId s) {
    for (std::uint32_t i = 0; i < m.mir.moduleFuncCount(); ++i) {
        if (m.mir.funcSymbol(m.mir.funcAt(i)) == s) return m.mir.funcAt(i);
    }
    return std::nullopt;
}

// The merged function named `name` that is NOT module-private, or nullptr.
[[nodiscard]] std::optional<MirFuncId> externalFunctionNamed(MergedMirModule const& m, std::string_view name) {
    for (std::uint32_t i = 0; i < m.mir.moduleFuncCount(); ++i) {
        MirFuncId const f = m.mir.funcAt(i);
        if (nameIn(m, m.mir.funcSymbol(f)) == name && m.mir.funcBinding(f) != SymbolBinding::Local) return f;
    }
    return std::nullopt;
}

[[nodiscard]] ExternImport const* referenceRowNamed(MergedMirModule const& m, std::string_view name) {
    for (ExternImport const& e : m.externImports) {
        if (e.mangledName == name) return &e;
    }
    return nullptr;
}

}  // namespace

TEST(MirMergeAlias, AnAliasThatWinsItsNameIsCarriedAndEveryReferenceToItStaysOneToTheName) {
    Unit a{1}, b{2};
    build(a, {Fn{1, "g", kStrong}}, {Alias{2, "f", 1, kStrong}}, {Ref{2, "f"}});
    build(b, {Fn{1, "use", kStrong}}, {}, {Ref{5, "f"}});
    Merged m;
    merge(m, a, b);
    ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
    EXPECT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
    MergedMirModule const& mm = *m.module;
    ASSERT_EQ(mm.mir.moduleAliasCount(), 1u);
    MirAlias const& alias = mm.mir.aliases()[0];
    EXPECT_EQ(nameIn(mm, alias.alias), "f");
    EXPECT_EQ(nameIn(mm, alias.target), "g");
    ExternImport const* row = referenceRowNamed(mm, "f");
    ASSERT_NE(row, nullptr) << "a reference to a name an alias won stays a reference row";
    EXPECT_EQ(row->symbol, alias.alias) << "one name, one merged id: the row and the alias are the same symbol";
}

TEST(MirMergeAlias, AWeakAliasYieldsToAStrongDefinitionOfItsName) {
    Unit a{1}, b{2};
    build(a, {Fn{1, "g", kStrong}}, {Alias{2, "f", 1, kWeak}}, {Ref{2, "f"}});
    build(b, {Fn{1, "f", kStrong}}, {}, {});
    Merged m;
    merge(m, a, b);
    ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
    EXPECT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
    EXPECT_EQ(m.module->mir.moduleAliasCount(), 0u) << "the losing alias is gone with its name";
    EXPECT_TRUE(definesFunctionNamed(*m.module, "f"));
    EXPECT_EQ(referenceRowNamed(*m.module, "f"), nullptr) << "a reference to a name a definition won goes to it";
}

TEST(MirMergeAlias, AStrongAliasBesideAStrongDefinitionOfItsNameIsTwoDefinitions) {
    Unit a{1}, b{2};
    build(a, {Fn{1, "g", kStrong}}, {Alias{2, "f", 1, kStrong}}, {Ref{2, "f"}});
    build(b, {Fn{1, "f", kStrong}}, {}, {});
    Merged m;
    merge(m, a, b);
    EXPECT_TRUE(reported(m.rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits, "'f'")) << diagnosticsOf(m.rep);
}

TEST(MirMergeAlias, AModulePrivateAliasIsNeitherWinnerNorLoser) {
    Unit a{1}, b{2};
    build(a, {Fn{1, "g", kStrong}}, {Alias{2, "f", 1, kLocal}}, {});
    build(b, {Fn{1, "f", kStrong}}, {}, {});
    Merged m;
    merge(m, a, b);
    ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
    EXPECT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
    ASSERT_EQ(m.module->mir.moduleAliasCount(), 1u);
    EXPECT_EQ(m.module->mir.aliases()[0].linkage, kLocal);
    EXPECT_TRUE(definesFunctionNamed(*m.module, "f")) << "the other unit's external `f` is a different entity";
}

TEST(MirMergeAlias, AnAliasWhoseTargetLostItsNameIsWhatTheLinkMakesOfTheLosersBytes) {
    {
        // ELF keeps a superseded weak definition's bytes: the alias stays on its own unit's body.
        Unit a{1}, b{2};
        build(a, {Fn{1, "g", kWeak}}, {Alias{2, "f", 1, kStrong}}, {Ref{2, "f"}});
        build(b, {Fn{1, "g", kStrong}}, {}, {});
        Merged m;
        merge(m, a, b);
        ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
        EXPECT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
        MergedMirModule const& mm = *m.module;
        ASSERT_EQ(mm.mir.moduleAliasCount(), 1u) << "the alias of a body whose name was lost is kept";
        auto const kept   = functionOf(mm, mm.mir.aliases()[0].target);
        auto const winner = externalFunctionNamed(mm, "g");
        ASSERT_TRUE(kept.has_value()) << "the alias names no function of the merged module";
        ASSERT_TRUE(winner.has_value()) << "the name `g` went to no definition";
        EXPECT_NE(*kept, *winner) << "the alias moved to the body that won the name, where the link keeps it on its own";
        EXPECT_EQ(mm.mir.funcBinding(*kept), SymbolBinding::Local)
            << "the kept body is module-private: the name is the winner's";
    }
    {
        // A document that states the superseded definition REPLACED WHOLE (Mach-O's, the one shipped
        // statement of it for an overridable weak definition): the alias names the winning definition.
        // The merge reads the statement, not the format — a Mach-O C unit never states an alias (its
        // front end refuses one), and this case is the statement's.
        Unit a{1}, b{2};
        build(a, {Fn{1, "g", kWeak}}, {Alias{2, "f", 1, kStrong}}, {Ref{2, "f"}});
        build(b, {Fn{1, "g", kStrong}}, {}, {});
        Merged m;
        merge(m, a, b, "macho64-x86_64-darwin-exec");
        ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
        EXPECT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
        MergedMirModule const& mm = *m.module;
        ASSERT_EQ(mm.mir.moduleAliasCount(), 1u);
        auto const winner = externalFunctionNamed(mm, "g");
        ASSERT_TRUE(winner.has_value());
        EXPECT_EQ(mm.mir.aliases()[0].target, mm.mir.funcSymbol(*winner))
            << "a definition replaced whole is the winner's, and so is the alias";
    }
    // CONTROL: the same alias of a target that keeps its name merges onto it.
    Unit c{1}, d{2};
    build(c, {Fn{1, "g", kStrong}}, {Alias{2, "f", 1, kStrong}}, {Ref{2, "f"}});
    build(d, {Fn{1, "h", kStrong}}, {}, {});
    Merged n;
    merge(n, c, d);
    ASSERT_TRUE(n.module.has_value()) << diagnosticsOf(n.rep);
    EXPECT_EQ(n.rep.errorCount(), 0u) << diagnosticsOf(n.rep);
    ASSERT_EQ(n.module->mir.moduleAliasCount(), 1u);
    auto const g = externalFunctionNamed(*n.module, "g");
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(n.module->mir.aliases()[0].target, n.module->mir.funcSymbol(*g));
}
