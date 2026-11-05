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
//     whole (the Mach-O documents' statement), the alias names the winning definition; where their
//     section is DISCARDED whole (PE's select-any kind), a STRONG alias names the winner for its own unit's
//     references, and another unit's use of the alias's name — which no definition is left to hold —
//     is refused by name; a WEAK alias stays the weak definition of its own name it is, and where it wins
//     the name it names the winning copy.

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
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace dss;

namespace {

using Kind = WeakDefinitionKind;

constexpr MirLinkage kStrong{SymbolBinding::Global, SymbolVisibility::Default};
constexpr MirLinkage kWeak{SymbolBinding::Weak, SymbolVisibility::Default, Kind::Overridable};
constexpr MirLinkage kLocal{SymbolBinding::Local, SymbolVisibility::Default};
constexpr MirLinkage kSelectAny{SymbolBinding::Weak, SymbolVisibility::Default, Kind::SelectAny};

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

// A DATUM of a unit: `int <name> = <value>;` under `linkage`.
struct Datum {
    std::uint32_t symbol;
    char const*   name;
    MirLinkage    linkage;
    std::int64_t  value;
};
// A function of a unit that takes the address of `reads` — a USE of that symbol, in code.
struct User {
    std::uint32_t symbol;
    char const*   name;
    std::uint32_t reads;
};

// One unit of data: its datums, its aliases, its reference rows, and the functions that use symbols.
void buildData(Unit& u, std::vector<Datum> const& data, std::vector<Alias> const& aliases,
               std::vector<Ref> const& refs, std::vector<User> const& users) {
    TypeId const i32 = u.interner.primitive(TypeKind::I32);
    TypeId const sig = u.interner.fnSig({}, u.interner.primitive(TypeKind::Void), CallConv::CcSysV);
    MirBuilder   b;
    for (Datum const& d : data) {
        MirLiteralValue lit;
        lit.value = d.value;
        lit.core  = TypeKind::I32;
        (void)b.addGlobal(i32, SymbolId{d.symbol}, b.literalPoolAdd(lit), MirFuncId{}, d.linkage,
                          /*isConst=*/false, MirThreadStorage::Shared);
        u.names[d.symbol] = d.name;
    }
    for (User const& f : users) {
        (void)b.addFunction(sig, SymbolId{f.symbol}, kStrong);
        MirBlockId const e = b.createBlock(StructCfMarker::EntryBlock);
        b.beginBlock(e);
        (void)b.addGlobalAddr(SymbolId{f.reads}, u.interner.pointer(i32));
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
        row.isData      = true;
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

// The merge of `units`, in that order (the election keeps the lower key's copy of two select-any ones), under
// `format`, in this process.
void runMerge(Merged& out, std::vector<Unit const*> const& units, ObjectFormatSchema const& format) {
    std::vector<MergeCuInput> cus(units.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        Unit const& u = *units[i];
        cus[i].mir           = &u.mir;
        cus[i].interner      = &u.interner;
        cus[i].externImports = u.imports;
        cus[i].nameOf        = [&u](SymbolId s) {
            auto const it = u.names.find(s.v);
            return it == u.names.end() ? std::string{} : it->second;
        };
    }
    out.module = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, {}, format, out.rep);
}

// Merges `units`, in that order, under `format` — FIRST in a child process, which must end normally, then here.
// WHY THE CHILD EXISTS: so that a MUTANT's crash stays inside one case. No case here depends on the merge stopping
// — no input of this suite reaches a stop of the unchanged merge, and none is meant to. But the merge stops itself
// on a broken internal invariant (an `std::abort` naming it: two of its steps must agree, as step 3b's binding of
// a unit's reference rows and step 6's emission of them do), and a mutation of one of the two (a sweep's arm) is
// caught by the other's stop. Run here first, that stop would end this binary and every case's report with it; in
// the child it is a red of the case that asked for the merge, said with the merge's own words, and the cases after
// it still run. Each merge therefore costs one more process (on Windows a death test runs the binary again).
void mergeUnder(Merged& out, std::vector<Unit const*> const& units, ObjectFormatSchema const& format) {
    ASSERT_EXIT(
        {
            Merged probe;
            runMerge(probe, units, format);
            std::exit(0);
        },
        ::testing::ExitedWithCode(0), "")
        << "the merge stopped itself on a broken internal invariant";
    runMerge(out, units, format);
}

void mergeAll(Merged& out, std::vector<Unit const*> const& units, char const* linkFormat = "elf64-x86_64-linux-exec") {
    auto const format = shippedFormat(linkFormat);
    ASSERT_NE(format, nullptr);
    mergeUnder(out, units, *format);
}

void merge(Merged& out, Unit const& a, Unit const& b, char const* linkFormat = "elf64-x86_64-linux-exec") {
    mergeAll(out, {&a, &b}, linkFormat);
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

// The merged global named `name` that is NOT module-private, or nullopt.
[[nodiscard]] std::optional<MirGlobalId> externalGlobalNamed(MergedMirModule const& m, std::string_view name) {
    for (std::uint32_t i = 0; i < m.mir.moduleGlobalCount(); ++i) {
        MirGlobalId const g = m.mir.globalAt(i);
        if (nameIn(m, m.mir.globalSymbol(g)) == name && m.mir.globalLinkage(g).binding != SymbolBinding::Local)
            return g;
    }
    return std::nullopt;
}

// The value the merged global whose symbol is `s` is initialized with, or nullopt.
[[nodiscard]] std::optional<std::int64_t> initialValueOf(MergedMirModule const& m, SymbolId s) {
    for (std::uint32_t i = 0; i < m.mir.moduleGlobalCount(); ++i) {
        MirGlobalId const g = m.mir.globalAt(i);
        if (m.mir.globalSymbol(g) != s) continue;
        std::uint32_t const lit = m.mir.globalInitLiteralIndex(g);
        if (lit == UINT32_MAX) return std::nullopt;
        if (auto const* v = std::get_if<std::int64_t>(&m.mir.literalValue(lit).value)) return *v;
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] ExternImport const* referenceRowNamed(MergedMirModule const& m, std::string_view name) {
    for (ExternImport const& e : m.externImports) {
        if (e.mangledName == name) return &e;
    }
    return nullptr;
}

// The symbol whose address the merged function named `user` takes (a `User` of `buildData`), or nullopt.
[[nodiscard]] std::optional<SymbolId> readBy(MergedMirModule const& m, std::string_view user) {
    auto const f = externalFunctionNamed(m, user);
    if (!f.has_value()) return std::nullopt;
    for (std::uint32_t bi = 0; bi < m.mir.funcBlockCount(*f); ++bi) {
        MirBlockId const b = m.mir.funcBlockAt(*f, bi);
        for (std::uint32_t ii = 0; ii < m.mir.blockInstCount(b); ++ii) {
            MirInstId const inst = m.mir.blockInstAt(b, ii);
            if (m.mir.instOpcode(inst) == MirOpcode::GlobalAddr) return m.mir.globalAddrSymbol(inst);
        }
    }
    return std::nullopt;
}

// The merged aliases named `name`.
[[nodiscard]] std::vector<MirAlias> aliasesNamed(MergedMirModule const& m, std::string_view name) {
    std::vector<MirAlias> out;
    for (MirAlias const& a : m.mir.aliases()) {
        if (nameIn(m, a.alias) == name) out.push_back(a);
    }
    return out;
}

// The VALUE the merged function `user` (a `User` of `buildData`) reads, as the link resolves it: its read followed
// through an alias to the definition it names, and through a reference row to the external alias or global that
// holds the row's name. nullopt: the read ends in no initialized definition.
[[nodiscard]] std::optional<std::int64_t> valueReadBy(MergedMirModule const& m, std::string_view user) {
    std::optional<SymbolId> sym = readBy(m, user);
    for (int hop = 0; sym.has_value() && hop < 8; ++hop) {
        if (auto const v = initialValueOf(m, *sym); v.has_value()) return v;
        std::optional<SymbolId> next;
        for (MirAlias const& a : m.mir.aliases()) {
            if (a.alias == *sym) next = a.target;
        }
        for (ExternImport const& e : m.externImports) {
            if (next.has_value() || e.symbol != *sym) continue;
            for (MirAlias const& a : m.mir.aliases()) {
                if (nameIn(m, a.alias) == e.mangledName && a.linkage.binding != SymbolBinding::Local) next = a.alias;
            }
            if (auto const g = externalGlobalNamed(m, e.mangledName); !next.has_value() && g.has_value())
                next = m.mir.globalSymbol(*g);
        }
        sym = next;
    }
    return std::nullopt;
}

[[nodiscard]] std::size_t countOf(DiagnosticReporter const& rep, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) n += d.code == code ? 1u : 0u;
    return n;
}

// The shipped PE image document with its `supersededDefinition` statement taken out — that key, and nothing else of
// the document. A statement per kind must answer EVERY kind (the loader refuses one that leaves a kind out), so a
// document that states nothing of a kind is one that states nothing of the key at all.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> peImageDocumentStatingNoSupersededDefinition() {
    std::ifstream in{dss::test::configRoot() / "object-formats" / "pe64-x86_64-windows-exec.format.json",
                     std::ios::binary};
    std::stringstream text;
    text << in.rdbuf();
    std::string doc = text.str();
    std::string_view constexpr key = "\"supersededDefinition\":";
    auto const at = doc.find(key);
    EXPECT_NE(at, std::string::npos) << "the shipped PE image document states no supersededDefinition";
    if (at == std::string::npos) return nullptr;
    auto const open  = doc.find('{', at + key.size());
    auto const close = open == std::string::npos ? std::string::npos : doc.find('}', open);
    EXPECT_TRUE(open != std::string::npos && close != std::string::npos) << "the statement is no object";
    if (open == std::string::npos || close == std::string::npos) return nullptr;
    auto const comma = doc.find_first_not_of(" \t\r\n", close + 1);
    EXPECT_TRUE(comma != std::string::npos && doc[comma] == ',') << "the statement is the document's last key";
    if (comma == std::string::npos || doc[comma] != ',') return nullptr;
    doc.erase(at, comma - at + 1);
    auto const loaded =
        ObjectFormatSchema::loadFromText(doc, "pe64-x86_64-windows-exec, with no supersededDefinition");
    EXPECT_TRUE(loaded.has_value()) << "the PE image document without supersededDefinition did not load";
    return loaded.has_value() ? *loaded : nullptr;
}

}  // namespace

TEST(MirMergeAlias, AnAliasThatWinsItsNameIsCarriedAndEveryReferenceToItStaysOneToTheName) {
    {
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
    {
        // The name the alias wins is ALSO a definition of another unit's — a weak one, which the strong alias beats
        // (as every reference linker ranks them): a name the program defines, and still the reference to it stays a
        // reference to the name, since what won it is the alias, which has no body to call.
        Unit a{1}, b{2}, c{3};
        build(a, {Fn{1, "g", kStrong}}, {Alias{2, "f", 1, kStrong}}, {Ref{2, "f"}});
        build(b, {Fn{1, "f", kWeak}}, {}, {});
        build(c, {Fn{1, "use", kStrong}}, {}, {Ref{5, "f"}});
        Merged m;
        mergeAll(m, {&a, &b, &c});
        ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
        EXPECT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
        MergedMirModule const& mm = *m.module;
        ASSERT_EQ(mm.mir.moduleAliasCount(), 1u);
        MirAlias const& alias = mm.mir.aliases()[0];
        EXPECT_EQ(nameIn(mm, alias.alias), "f");
        ExternImport const* row = referenceRowNamed(mm, "f");
        ASSERT_NE(row, nullptr) << "a reference to a name an alias won stays a reference row, beside a weak "
                                   "definition of the name the alias beat";
        EXPECT_EQ(row->symbol, alias.alias) << "one name, one merged id: the row and the alias are the same symbol";
    }
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

// The review of fold f26a, F3 — WHERE THE ALIAS THAT WON A TARGET'S NAME LEADS. Under a document that states a
// superseded definition REPLACED WHOLE (the Mach-O documents' statement; the merge reads the statement, as the case
// above says), an alias whose target lost its name names the winning DEFINITION — and where what won the target's name
// is itself an alias, the definition THAT alias names, followed through (an alias never names another alias). Unit a:
// a weak `g` and a strong alias `f` of it; unit b: a strong alias `g` of its own `h`. `g` goes to b's alias, so `f`
// names `h`.
TEST(MirMergeAlias, AnAliasWhoseTargetLostItsNameToAnAliasNamesThatAliassDefinition) {
    Unit a{1}, b{2};
    build(a, {Fn{1, "g", kWeak}}, {Alias{2, "f", 1, kStrong}}, {Ref{2, "f"}});
    build(b, {Fn{1, "h", kStrong}}, {Alias{2, "g", 1, kStrong}}, {Ref{2, "g"}});
    Merged m;
    merge(m, a, b, "macho64-x86_64-darwin-exec");
    ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
    EXPECT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
    MergedMirModule const& mm = *m.module;
    auto const h = externalFunctionNamed(mm, "h");
    ASSERT_TRUE(h.has_value()) << "the name `h` went to no definition";
    EXPECT_FALSE(externalFunctionNamed(mm, "g").has_value()) << "unit a's weak `g` beat the strong alias of it";
    auto const f = aliasesNamed(mm, "f");
    ASSERT_EQ(f.size(), 1u);
    EXPECT_EQ(f[0].target, mm.mir.funcSymbol(*h))
        << "the alias whose target lost its name to another alias does not name that alias's definition";
    auto const g = aliasesNamed(mm, "g");
    ASSERT_EQ(g.size(), 1u);
    EXPECT_EQ(g[0].target, mm.mir.funcSymbol(*h));
}

// F3's other half: the names come back — unit a: a weak `x` and a strong alias `y` of it; unit b: a weak `y` and a
// strong alias `x` of it. Each alias wins the name the other's target lost, so following the winners from either
// target returns to it and ends in no definition: each alias is refused by name, and nothing is built on the names.
TEST(MirMergeAlias, AliasesWhoseTargetsLostTheirNamesToEachOtherAreRefusedByName) {
    Unit a{1}, b{2};
    build(a, {Fn{1, "x", kWeak}}, {Alias{2, "y", 1, kStrong}}, {Ref{2, "y"}});
    build(b, {Fn{1, "y", kWeak}}, {Alias{2, "x", 1, kStrong}}, {Ref{2, "x"}});
    Merged m;
    merge(m, a, b, "macho64-x86_64-darwin-exec");
    EXPECT_TRUE(reported(m.rep, DiagnosticCode::K_CrossCuMergeUnsupported,
                         "which lost its name to an alias whose own target lost its name in turn"))
        << "an alias of a cycle of aliases was not refused by name" << diagnosticsOf(m.rep);
    EXPECT_EQ(countOf(m.rep, DiagnosticCode::K_CrossCuMergeUnsupported), 2u) << diagnosticsOf(m.rep);
    EXPECT_EQ(countOf(m.rep, DiagnosticCode::I_VerifierFailure), 0u)
        << "the merge built an alias of an alias on the cycle" << diagnosticsOf(m.rep);
}

// PE's select-any kind is `discardedWhole`: the losing copy's section goes with every label in it, the
// alias's among them, so an external alias of a losing copy is WITHDRAWN from the election of its name
// (`linker::resolveSettlingCarriedNames`) and kept only as a module-private name of the winner. MEASURED
// (probe mj1, the function's own statement): the alias's OWN unit's reads reach the winner (lld-link on
// clang-cl /O2 objects), and another unit can only name the alias, whose name then has no definition —
// lld-link and GNU ld refuse that link ("undefined"), link.exe links garbage. An alias nobody else uses
// is taken, as every reference takes it.
TEST(MirMergeAlias, AnAliasOfACopyDiscardedWholeNamesTheWinnerAndAnotherUnitsUseOfItsNameIsRefused) {
    // The unit whose copy of `w` loses (the election keeps the lower key's copy of two select-any
    // ones): `w` = 9, an external alias `m` its own `read_m` uses by name, and a `static` alias `s`
    // whose use the semantic tier bound to the target (`read_s` names `w`'s own id).
    auto const losing = [](Unit& u) {
        buildData(u, {Datum{1, "w", kSelectAny, 9}},
                  {Alias{2, "m", 1, kStrong}, Alias{4, "s", 1, kLocal}},
                  {Ref{2, "m"}},
                  {User{3, "read_m", 2}, User{5, "read_s", 1}});
    };
    char const* const pe = "pe64-x86_64-windows-exec";
    {
        // Only its own unit uses the alias: each alias names the winning copy, module-private, and the
        // unit's own read of the external one reaches the winner directly — no name `m` is left outside.
        Unit a{1}, b{2};
        buildData(a, {Datum{1, "w", kSelectAny, 7}}, {}, {}, {});
        losing(b);
        Merged m;
        merge(m, a, b, pe);
        ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
        EXPECT_EQ(m.rep.errorCount(), 0u)
            << "an alias of a copy discarded whole was refused although no other unit names it"
            << diagnosticsOf(m.rep);
        MergedMirModule const& mm = *m.module;
        auto const winner = externalGlobalNamed(mm, "w");
        ASSERT_TRUE(winner.has_value()) << "the name `w` went to no definition";
        EXPECT_EQ(initialValueOf(mm, mm.mir.globalSymbol(*winner)), std::optional<std::int64_t>{7})
            << "the winner is the lower key's copy";
        ASSERT_EQ(mm.mir.moduleAliasCount(), 2u) << "an alias of a discarded copy is kept, on the winner";
        for (MirAlias const& alias : mm.mir.aliases()) {
            EXPECT_EQ(alias.target, mm.mir.globalSymbol(*winner))
                << "alias '" << nameIn(mm, alias.alias) << "' does not name the winning copy";
            EXPECT_EQ(alias.linkage, kLocal)
                << "alias '" << nameIn(mm, alias.alias) << "' of a discarded copy kept a name outside the "
                   "module, which no reference image holds";
        }
        EXPECT_EQ(readBy(mm, "read_m"), std::optional<SymbolId>{mm.mir.globalSymbol(*winner)})
            << "the unit's own read of its withdrawn alias does not reach the winning copy";
        EXPECT_EQ(referenceRowNamed(mm, "m"), nullptr)
            << "a reference row of the withdrawn alias's name was left for the link to resolve";
        for (std::uint32_t i = 0; i < mm.mir.moduleGlobalCount(); ++i) {
            EXPECT_NE(initialValueOf(mm, mm.mir.globalSymbol(mm.mir.globalAt(i))), std::optional<std::int64_t>{9})
                << "the discarded copy's bytes were kept";
        }
    }
    {
        // ANOTHER unit uses the alias's name: no definition of `m` is left, refused by name.
        Unit a{1}, b{2};
        buildData(a, {Datum{1, "w", kSelectAny, 7}}, {}, {Ref{5, "m"}}, {User{6, "use_m", 5}});
        losing(b);
        Merged m;
        merge(m, a, b, pe);
        EXPECT_TRUE(reported(m.rep, DiagnosticCode::K_SymbolUndefined,
                             "CU #1 references 'm', which only the definition of 'w' in CU #2 carried"))
            << "another unit's use of the name of an alias whose copy was discarded whole was not refused by "
               "name" << diagnosticsOf(m.rep);
        EXPECT_EQ(countOf(m.rep, DiagnosticCode::K_SymbolUndefined), 1u) << diagnosticsOf(m.rep);
    }
    {
        // CONTROL: the other unit DECLARES `m` and uses it nowhere — every reference takes the link.
        Unit a{1}, b{2};
        buildData(a, {Datum{1, "w", kSelectAny, 7}}, {}, {Ref{5, "m"}}, {});
        losing(b);
        Merged m;
        merge(m, a, b, pe);
        ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
        EXPECT_EQ(m.rep.errorCount(), 0u)
            << "an unused declaration of the alias's name in another unit was taken as a use"
            << diagnosticsOf(m.rep);
    }
    {
        // CONTROL: the same use where the link KEEPS a superseded definition's bytes (ELF, an overridable
        // weak copy beside a strong one): the alias stays on the losing unit's own bytes, and the other
        // unit's use of it binds there.
        Unit a{1}, b{2};
        buildData(a, {Datum{1, "w", kStrong, 7}}, {}, {Ref{5, "m"}}, {User{6, "use_m", 5}});
        buildData(b, {Datum{1, "w", kWeak, 9}}, {Alias{2, "m", 1, kStrong}}, {Ref{2, "m"}},
                  {User{3, "read_m", 2}});
        Merged m;
        merge(m, a, b);
        ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
        EXPECT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
        ASSERT_EQ(m.module->mir.moduleAliasCount(), 1u);
        EXPECT_EQ(initialValueOf(*m.module, m.module->mir.aliases()[0].target), std::optional<std::int64_t>{9})
            << "where the link keeps a superseded definition's bytes, the alias stays on them";
    }
}

// cs-28's MAJOR, cell (i): unit a `__declspec(selectany) int w = 7; __attribute__((weak)) int m = 5;`, unit b
// `__declspec(selectany) int w = 9; extern int m __attribute__((alias("w")));` — b's copy loses `w`, so b's alias
// is no definition of `m`, and `m` is unit a's weak one. ✔MEASURED (probe mj1, order a b): lld-link 19.1.5 reads
// 5 through `m` from unit a and from a third unit (link.exe 14.51 links garbage, GNU ld 2.42 refuses); the
// alias's own unit reads the winning copy (7, lld-link on /O2 objects). Before the fix the alias, strong, beat
// the weak `m` and every read gave 7 — silent — or, with a third unit reading `m`, the link was refused.
// The ELF CONTROL: the same program where the link KEEPS a superseded definition's bytes (`w` overridable weak,
// ELF) — the alias stays in the election on its own unit's kept bytes and beats the weak `m`: gcc 13.3 and clang
// 18.1.3 under GNU ld 2.42 and ld.lld 18.1.3 read 9 through `m` everywhere (the same probe).
TEST(MirMergeAlias, AnAliasOfALosingCopyLeavesTheElectionOfItsName) {
    auto const unitA = [](Unit& u, MirLinkage copy) {
        buildData(u, {Datum{1, "w", copy, 7}, Datum{2, "m", kWeak, 5}}, {}, {}, {User{3, "use_m", 2}});
    };
    auto const unitB = [](Unit& u, MirLinkage copy) {
        buildData(u, {Datum{1, "w", copy, 9}}, {Alias{2, "m", 1, kStrong}}, {Ref{2, "m"}}, {User{3, "read_m", 2}});
    };
    auto const thirdReadingM = [](Unit& u) { buildData(u, {}, {}, {Ref{1, "m"}}, {User{2, "main_m", 1}}); };
    for (bool const third : {false, true}) {
        Unit a{1}, b{2}, c{3};
        unitA(a, kSelectAny);
        unitB(b, kSelectAny);
        thirdReadingM(c);
        Merged m;
        if (third) mergeAll(m, {&a, &b, &c}, "pe64-x86_64-windows-exec");
        else       mergeAll(m, {&a, &b}, "pe64-x86_64-windows-exec");
        SCOPED_TRACE(third ? "a third unit reads m" : "two units");
        ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
        EXPECT_EQ(m.rep.errorCount(), 0u) << "the alias of the losing copy still ran for its name"
                                          << diagnosticsOf(m.rep);
        MergedMirModule const& mm = *m.module;
        auto const weakM  = externalGlobalNamed(mm, "m");
        auto const winner = externalGlobalNamed(mm, "w");
        ASSERT_TRUE(weakM.has_value()) << "`m` went to no definition: the withdrawn alias took unit a's weak one with it";
        ASSERT_TRUE(winner.has_value());
        EXPECT_EQ(initialValueOf(mm, mm.mir.globalSymbol(*weakM)), std::optional<std::int64_t>{5});
        EXPECT_EQ(initialValueOf(mm, mm.mir.globalSymbol(*winner)), std::optional<std::int64_t>{7});
        EXPECT_EQ(readBy(mm, "use_m"), std::optional<SymbolId>{mm.mir.globalSymbol(*weakM)})
            << "unit a's read of `m` does not reach its own weak definition";
        EXPECT_EQ(readBy(mm, "read_m"), std::optional<SymbolId>{mm.mir.globalSymbol(*winner)})
            << "the alias's own unit does not read the winning copy";
        if (third) {
            EXPECT_EQ(readBy(mm, "main_m"), std::optional<SymbolId>{mm.mir.globalSymbol(*weakM)})
                << "a third unit's read of `m` does not reach unit a's weak definition";
        }
        for (MirAlias const& alias : aliasesNamed(mm, "m")) {
            EXPECT_EQ(alias.linkage, kLocal) << "the withdrawn alias kept its name outside the module";
        }
    }
    // CONTROL (ELF, keepsItsBytes): the alias stays in the election on its own unit's kept bytes, and wins.
    Unit a{1}, b{2};
    unitA(a, kWeak);
    unitB(b, kWeak);
    Merged m;
    merge(m, a, b);
    ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
    EXPECT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
    auto const kept = aliasesNamed(*m.module, "m");
    ASSERT_EQ(kept.size(), 1u) << "where the link keeps the loser's bytes, the alias stays a definition of `m`";
    EXPECT_EQ(kept[0].linkage, kStrong);
    EXPECT_EQ(initialValueOf(*m.module, kept[0].target), std::optional<std::int64_t>{9})
        << "the alias stays on its own unit's kept bytes";
    EXPECT_FALSE(externalGlobalNamed(*m.module, "m").has_value()) << "the weak `m` beat the strong alias";
}

// cs-28's MAJOR, cells (ii) and (ii'): two units each `__declspec(selectany) int w = N; extern int m
// __attribute__((alias("w")));`. ✔MEASURED (probe mj1, both orders): GNU ld 2.42 and lld-link 19.1.5 LINK it — the
// losing unit's `m` goes with its section, the winning copy's `m` holds the name, every read gives the winner
// (link.exe 14.51 refuses, LNK2005); a third unit's strong `int m = 1;` beside them is refused by all three (the
// WINNING copy's alias and the strong `m` are two definitions). Before the fix both were refused.
TEST(MirMergeAlias, TwoAliasesOfTheirOwnCopiesAreOneDefinitionOfTheirName) {
    auto const withAlias = [](Unit& u, std::int64_t value, char const* reader) {
        buildData(u, {Datum{1, "w", kSelectAny, value}}, {Alias{2, "m", 1, kStrong}}, {Ref{2, "m"}},
                  {User{3, reader, 2}});
    };
    char const* const pe = "pe64-x86_64-windows-exec";
    {
        Unit a{1}, b{2}, c{3};
        withAlias(a, 7, "read_a");
        withAlias(b, 9, "read_b");
        buildData(c, {}, {}, {Ref{1, "m"}}, {User{2, "main_m", 1}});
        Merged m;
        mergeAll(m, {&a, &b, &c}, pe);
        ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
        EXPECT_EQ(m.rep.errorCount(), 0u) << "the losing copy's alias was a second definition of `m`"
                                          << diagnosticsOf(m.rep);
        MergedMirModule const& mm = *m.module;
        auto const winner = externalGlobalNamed(mm, "w");
        ASSERT_TRUE(winner.has_value());
        EXPECT_EQ(initialValueOf(mm, mm.mir.globalSymbol(*winner)), std::optional<std::int64_t>{7});
        std::size_t external = 0, modulePrivate = 0;
        for (MirAlias const& alias : aliasesNamed(mm, "m")) {
            EXPECT_EQ(alias.target, mm.mir.globalSymbol(*winner)) << "an alias `m` does not name the winning copy";
            (alias.linkage == kStrong ? external : modulePrivate) += 1;
        }
        EXPECT_EQ(external, 1u) << "the winning copy's alias does not hold the name";
        EXPECT_EQ(modulePrivate, 1u) << "the losing copy's alias is not module-private";
        EXPECT_EQ(readBy(mm, "read_b"), std::optional<SymbolId>{mm.mir.globalSymbol(*winner)})
            << "the losing unit's read does not reach the winning copy";
        ExternImport const* row = referenceRowNamed(mm, "m");
        ASSERT_NE(row, nullptr) << "the reads of the name the winning alias holds are no longer references to it";
        EXPECT_EQ(readBy(mm, "main_m"), std::optional<SymbolId>{row->symbol});
        EXPECT_EQ(readBy(mm, "read_a"), std::optional<SymbolId>{row->symbol});
    }
    {
        // (ii'): a third unit's strong `m` beside them — two definitions, refused.
        Unit a{1}, b{2}, c{3};
        withAlias(a, 7, "read_a");
        withAlias(b, 9, "read_b");
        buildData(c, {Datum{1, "m", kStrong, 1}}, {}, {}, {});
        Merged m;
        mergeAll(m, {&a, &b, &c}, pe);
        EXPECT_TRUE(reported(m.rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits, "'m'")) << diagnosticsOf(m.rep);
        EXPECT_EQ(countOf(m.rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 1u)
            << "the losing copy's alias was counted a definition of `m` too" << diagnosticsOf(m.rep);
    }
}

// A CYCLE (probe mj1's seventh cell): unit a `selectany y = 1; x alias of y`, unit b `selectany x = 2; y alias of
// x` — each copy is elected under the other's name. ✔MEASURED, both orders: every PE reference keeps both sections
// (each COMDAT key is seen once) and refuses x and y as twice defined (LNK2005, lld-link "duplicate symbol", GNU ld
// "multiple definition"). The aliases wait on each other and stay in the election, which refuses both names; and
// nothing is built on them afterwards (before the fix the merge went on to name an alias by another alias and
// failed its own verification).
TEST(MirMergeAlias, AliasesWhoseCopiesWaitOnEachOtherAreRefusedAsTwiceDefinedNames) {
    Unit a{1}, b{2};
    buildData(a, {Datum{1, "y", kSelectAny, 1}}, {Alias{2, "x", 1, kStrong}}, {Ref{2, "x"}}, {User{3, "ua", 2}});
    buildData(b, {Datum{1, "x", kSelectAny, 2}}, {Alias{2, "y", 1, kStrong}}, {Ref{2, "y"}}, {User{3, "ub", 2}});
    Merged m;
    merge(m, a, b, "pe64-x86_64-windows-exec");
    EXPECT_TRUE(reported(m.rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits, "\"x\"")) << diagnosticsOf(m.rep);
    EXPECT_TRUE(reported(m.rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits, "\"y\"")) << diagnosticsOf(m.rep);
    EXPECT_EQ(countOf(m.rep, DiagnosticCode::I_VerifierFailure), 0u)
        << "the merge built an alias of an alias on the refused names" << diagnosticsOf(m.rep);
    EXPECT_EQ(countOf(m.rep, DiagnosticCode::K_CrossCuMergeUnsupported), 0u) << diagnosticsOf(m.rep);
}

// Step 5b's last arm: a document that states NOTHING of what becomes of a superseded definition of the target's
// kind (here PE's image document with its `supersededDefinition` statement taken out) — the alias of the losing
// copy is refused by name, as the link refuses it; never withdrawn and never kept on a guess.
TEST(MirMergeAlias, AnAliasOfACopyWhoseFateTheDocumentDoesNotStateIsRefusedByName) {
    auto const unstated = peImageDocumentStatingNoSupersededDefinition();
    ASSERT_NE(unstated, nullptr);
    Unit a{1}, b{2};
    buildData(a, {Datum{1, "w", kSelectAny, 7}}, {}, {}, {});
    buildData(b, {Datum{1, "w", kSelectAny, 9}}, {Alias{2, "m", 1, kStrong}}, {Ref{2, "m"}}, {User{3, "read_m", 2}});
    Merged m;
    mergeUnder(m, {&a, &b}, *unstated);
    EXPECT_TRUE(reported(m.rep, DiagnosticCode::K_CrossCuMergeUnsupported,
                         "the link's document states nothing of what becomes of a replaced definition of its kind"))
        << diagnosticsOf(m.rep);
    // CONTROL: the shipped statement (select-any discarded whole) takes the same program.
    Merged shipped;
    merge(shipped, a, b, "pe64-x86_64-windows-exec");
    ASSERT_TRUE(shipped.module.has_value()) << diagnosticsOf(shipped.rep);
    EXPECT_EQ(shipped.rep.errorCount(), 0u) << diagnosticsOf(shipped.rep);
}

// The review of fold f26a, F1: a WEAK alias of a select-any copy is a weak definition of its OWN name, never withdrawn
// with its copy (`linker::carriedNameGoesWithItsCarrier`): when another definition wins the name, every read follows
// the name, its own unit's included; when it wins the name, it names the WINNING copy of its target. ✔MEASURED
// 2026-10-20 (lane cs's probes wk1 and wk2: clang-cl 19.1.5 under link.exe 14.51 and lld-link 19.1.5, mingw-w64 gcc
// 13.2 under GNU ld 2.42; both orders, both optimisation levels). Unit b of every cell is `__declspec(selectany) int
// w = 9; extern int m __attribute__((weak, alias("w"))); int ub(void) { return m; }`; what each reader reads, order
// a b [b a]:
//   * wk1 cell 1 — a `selectany w = 7`, c `int m = 1;`, main reads m: all three linkers ua 7, ub 1, main 1 [9, 1, 1];
//   * wk1 cell 3 — no other m, main reads m: clang's two linkers 7, 7, 7 [9, 9, 9] (GNU ld refuses: undefined);
//   * wk1 cell 4 — the same, main does not read m: clang's two linkers 7, 7 [9, 9];
//   * wk1 cell 5 — a weak alias `m` of its own copy in each unit, main reads m: clang's two linkers 7, 7, 7
//     [9, 9, 9] — two weak definitions that are ONE definition (each names the copy that won `w`) are no pair;
//   * wk2 cell 5 — a `selectany w = 7; __attribute__((weak)) int m = 5;`, nothing else names m: GNU ld 5, 5
//     [9, 9] (clang's linkers refuse: two weak externals with different defaults) — the first weak definition wins.
// Checkpoint 30 withdrew the weak alias like a strong one: cell 1 read 7 in b, cell 3 was refused K_SymbolUndefined,
// wk2's cell 5 read 7 in b.
TEST(MirMergeAlias, AWeakAliasOfACopyIsAWeakDefinitionOfItsOwnName) {
    using Build = std::function<void(Unit&)>;
    Build const unitB = [](Unit& u) {
        buildData(u, {Datum{1, "w", kSelectAny, 9}}, {Alias{2, "m", 1, kWeak}}, {Ref{2, "m"}}, {User{3, "ub", 2}});
    };
    Build const copyA = [](Unit& u) { buildData(u, {Datum{1, "w", kSelectAny, 7}}, {}, {}, {User{3, "ua", 1}}); };
    Build const aliasA = [](Unit& u) {
        buildData(u, {Datum{1, "w", kSelectAny, 7}}, {Alias{2, "m", 1, kWeak}}, {Ref{2, "m"}}, {User{3, "ua", 2}});
    };
    Build const weakMA = [](Unit& u) {
        buildData(u, {Datum{1, "w", kSelectAny, 7}, Datum{2, "m", kWeak, 5}}, {}, {}, {User{3, "ua", 2}});
    };
    Build const strongM    = [](Unit& u) { buildData(u, {Datum{1, "m", kStrong, 1}}, {}, {}, {}); };
    Build const mainReadsM = [](Unit& u) { buildData(u, {}, {}, {Ref{1, "m"}}, {User{2, "main_m", 1}}); };
    struct Read {
        char const*  user;
        std::int64_t ab, ba;   // the value read in order a b, and in order b a
    };
    struct Cell {
        char const*        name;
        Build              a;
        std::vector<Build> rest;   // the units after a and b
        std::vector<Read>  reads;
    };
    std::vector<Cell> const cells{
        {"wk1 cell 1: a strong m elsewhere", copyA, {strongM, mainReadsM},
         {{"ua", 7, 9}, {"ub", 1, 1}, {"main_m", 1, 1}}},
        {"wk1 cell 3: no other m, main reads it", copyA, {mainReadsM}, {{"ua", 7, 9}, {"ub", 7, 9}, {"main_m", 7, 9}}},
        {"wk1 cell 4: no other m", copyA, {}, {{"ua", 7, 9}, {"ub", 7, 9}}},
        {"wk1 cell 5: a weak alias of its own copy in each unit", aliasA, {mainReadsM},
         {{"ua", 7, 9}, {"ub", 7, 9}, {"main_m", 7, 9}}},
        {"wk2 cell 5: a weak m beside it, nothing else names m", weakMA, {}, {{"ua", 5, 9}, {"ub", 5, 9}}},
    };
    for (Cell const& cell : cells) {
        for (bool const ba : {false, true}) {
            SCOPED_TRACE(std::string{cell.name} + (ba ? ", order b a" : ", order a b"));
            std::vector<Build> order{ba ? unitB : cell.a, ba ? cell.a : unitB};
            order.insert(order.end(), cell.rest.begin(), cell.rest.end());
            std::vector<std::unique_ptr<Unit>> units;
            std::vector<Unit const*>           in;
            for (std::size_t i = 0; i < order.size(); ++i) {
                units.push_back(std::make_unique<Unit>(static_cast<std::uint32_t>(i + 1)));
                order[i](*units.back());
                in.push_back(units.back().get());
            }
            Merged m;
            mergeAll(m, in, "pe64-x86_64-windows-exec");
            ASSERT_TRUE(m.module.has_value()) << diagnosticsOf(m.rep);
            EXPECT_EQ(m.rep.errorCount(), 0u) << "the weak alias's program was refused" << diagnosticsOf(m.rep);
            for (Read const& r : cell.reads) {
                EXPECT_EQ(valueReadBy(*m.module, r.user), std::optional<std::int64_t>{ba ? r.ba : r.ab})
                    << r.user << " reads another `m` than the link gives it";
            }
            for (MirAlias const& alias : aliasesNamed(*m.module, "m")) {
                EXPECT_NE(alias.linkage, kLocal)
                    << "a weak alias was kept as a module-private name: withdrawn with its copy like a strong one";
            }
        }
    }
}
