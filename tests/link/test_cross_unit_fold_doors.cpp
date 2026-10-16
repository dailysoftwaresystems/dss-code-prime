// P69 fold 3 (lane `xa`) — ONE NAME DEFINED BY TWO UNITS: ONE TABLE OF PAIRS, FOUR DOORS, ONE ANSWER.
// D-LK-WEAK-EXTERNAL-BODY-OUTRANKED-A-SELECT-ANY-DEFINITION-BY-LINK-ORDER
// D-C-TENTATIVE-DEFINITIONS-OF-ONE-NAME-IN-TWO-UNITS-REFUSED
//
// A definition of an external name is one of four things to a link: STRONG, TENTATIVE (a common), an OVERRIDABLE
// weak definition, a SELECT-ANY weak definition. What a pair of them is — which one the name keeps, or that the
// pair is refused — is the link's FORMAT's to say (its document: `commonYieldsTo`, `weakDefinitionBesideStrong`,
// and the rank between the two weak kinds), and it is decided in ONE place (`linker::resolveCrossCuDefs`).
//
// THREE tiers hand that place their definitions, and until this fold each stated what it happened to know:
//   * the UNIT LINKER (`linker::link`, a row of an object or of a compiled unit) stated a rank;
//   * the CROSS-UNIT MIR MERGE (`mergeCuMirs`, the default whole-program build) stated nothing;
//   * the SUMMARY INDEX (`buildSummaryIndex`, `--lto=thin`) stated nothing.
// So ONE program elected one definition through one door and another through the next. ✔MEASURED 2026-10-12 on
// DSS before the fold, pe64: `__declspec(selectany) int v = 11;` beside `__attribute__((weak)) int v = 44;` ran 11
// through the unit linker in both orders and 44 through the merge with the overridable unit named first; a
// select-any definition beside a strong one linked (22) where link.exe, lld-link and GNU ld refuse; and two units
// each holding `int v;` were refused everywhere as two strong definitions.
//
// THE TABLE below is each format's answer for every ordered pair, as its reference linkers gave it:
//   ELF     gcc 13.3 and clang 18.1 objects under GNU ld 2.42 and ld.lld 18.1 (2026-10-07, -08 and -12). ELF has
//           ONE spelling of a weak definition (STB_WEAK), so its two weak columns are one measurement.
//   PE      cl 19.44, clang 19.1.5 (both targets) and MinGW gcc 13.2 objects under link.exe 14.44, lld-link
//           19.1.5 and GNU ld 2.42 (2026-10-08, -10 and -12). Two overridable definitions are the one cell its
//           linkers split on (link.exe and lld-link refuse the pair, GNU ld's PE linker keeps the first): what
//           works is kept.
//   Mach-O  Apple clang 21 objects under ld-1267 and ld64-957.1 (2026-10-07 and -08). One spelling of a weak
//           definition, and a common yields to it.
// EVERY door must give the table's answer for every cell, both orders: the fold itself, the unit linker (a
// relocatable artifact read back), the MIR merge, and the summary index.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "link/common_yield.hpp"
#include "link/cross_cu_resolve.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "mir/merge/mir_merge.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/summary/mir_summary.hpp"
#include "mir/summary/summary_index.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// The four things a definition is, in the table's order.
enum class What : std::uint8_t { Strong, Tentative, Overridable, SelectAny };
constexpr std::array<What, 4> kEvery{What::Strong, What::Tentative, What::Overridable, What::SelectAny};
constexpr char const* kWhatName[] = {"strong", "tentative", "overridable", "select-any"};

[[nodiscard]] constexpr SymbolBinding bindingOf(What w) noexcept {
    return w == What::Overridable || w == What::SelectAny ? SymbolBinding::Weak : SymbolBinding::Global;
}
[[nodiscard]] constexpr std::optional<WeakDefinitionKind> kindOf(What w) noexcept {
    if (w == What::Overridable) return WeakDefinitionKind::Overridable;
    if (w == What::SelectAny) return WeakDefinitionKind::SelectAny;
    return std::nullopt;
}

// What a pair is: the name keeps the FIRST unit's definition, the SECOND's, or the pair is REFUSED.
enum class Pair : std::uint8_t { First, Second, Refused };
constexpr Pair F = Pair::First, S = Pair::Second, X = Pair::Refused;

struct Family {
    char const* label;
    char const* document;   // the relocatable document of the family: every door reads this one
    // [first][second], in `kEvery`'s order: strong, tentative, overridable, select-any.
    Pair table[4][4];
};

constexpr Family kFamilies[] = {
    {"ELF x86_64", "elf64-x86_64-linux",
     {/* strong      */ {X, F, F, F},
      /* tentative   */ {S, F, F, F},    // a common outranks every weak definition (gABI)
      /* overridable */ {S, S, F, F},
      /* select-any  */ {S, S, F, F}}},
    {"PE x86_64", "pe64-x86_64-windows",
     {/* strong      */ {X, F, F, X},    // a select-any definition beside a strong one is a DUPLICATE
      /* tentative   */ {S, F, F, S},    // strong > select-any > common > overridable
      /* overridable */ {S, S, F, S},
      /* select-any  */ {X, F, F, F}}},
    {"Mach-O x86_64", "macho64-x86_64-darwin",
     {/* strong      */ {X, F, F, F},
      /* tentative   */ {S, F, S, S},    // a common yields to ANY definition
      /* overridable */ {S, F, F, F},
      /* select-any  */ {S, F, F, F}}},
};

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> format;
};
[[nodiscard]] Loaded load(Family const& fam) {
    Loaded l;
    auto   t = TargetSchema::loadShipped("x86_64");
    auto   f = ObjectFormatSchema::loadShipped(fam.document);
    EXPECT_TRUE(t.has_value() && f.has_value()) << fam.document;
    if (t.has_value()) l.target = *t;
    if (f.has_value()) l.format = *f;
    return l;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

// What a door made of a pair. `first`/`second`: the definition the name kept is that unit's. A door that cannot
// tell two tentative definitions apart (they hold no value) answers `first` for that pair, as the table does.
struct Answer {
    Pair        pair = Pair::Refused;
    bool        understood = true;   // false: the door's output could not be read (said in `why`)
    std::string why;
};
[[nodiscard]] Answer notUnderstood(std::string why) { return Answer{Pair::Refused, false, std::move(why)}; }

// Unit 1 defines the name with the value 11 and unit 2 with 22 (where a definition has a value at all).
constexpr std::uint8_t kValueOf[2] = {11, 22};

// The survivor of a link that kept ONE definition, read as "holds a value" or "is a common".
[[nodiscard]] Answer survivorOf(What first, What second, bool isCommon, std::optional<std::uint8_t> value) {
    if (isCommon) {
        if (first == What::Tentative) return Answer{Pair::First};
        if (second == What::Tentative) return Answer{Pair::Second};
        return notUnderstood("a common survived a pair that holds no tentative definition");
    }
    if (!value.has_value()) return notUnderstood("the surviving definition holds no value");
    if (*value == kValueOf[0] && first != What::Tentative) return Answer{Pair::First};
    if (*value == kValueOf[1] && second != What::Tentative) return Answer{Pair::Second};
    return notUnderstood("the surviving definition holds " + std::to_string(*value));
}

// ── DOOR 1: the fold itself, handed what a door hands it ─────────────────────────────────────────────────────────
[[nodiscard]] Answer throughTheFold(Loaded const& L, What first, What second) {
    std::vector<linker::CrossCuDef> defs;
    What const                      whats[2] = {first, second};
    for (std::uint32_t u = 0; u < 2; ++u) {
        defs.push_back(linker::CrossCuDef{"shared", bindingOf(whats[u]),
                                          LinkedSymbolKey{CompilationUnitId{u + 1}, SymbolId{1}}});
        linker::stateTheLinksAnswers(defs.back(), *L.format, kindOf(whats[u]));
        defs.back().tentative = whats[u] == What::Tentative;
    }
    auto const r = linker::resolveCrossCuDefs(defs);
    if (!r.conflicts.empty() || !r.collisions.empty() || !r.undecided.empty()) return Answer{Pair::Refused};
    auto const it = r.winners.find("shared");
    if (it == r.winners.end()) return notUnderstood("the fold elected nothing");
    return Answer{it->second.cuId.v == 1 ? Pair::First : Pair::Second};
}

// ── DOOR 2: the unit linker — two hand-built units linked into a relocatable artifact, read back ───────────────
[[nodiscard]] AssembledModule linkUnit(std::uint32_t cu, What what) {
    AssembledModule m;
    m.cuId = CompilationUnitId{cu};
    if (what == What::Tentative) {
        ExternImport e;
        e.symbol          = SymbolId{1};
        e.mangledName     = "shared";
        e.isData          = true;
        e.commonSize      = 4;
        e.commonAlignment = 4;
        m.externImports.push_back(std::move(e));
        return m;
    }
    AssembledData d;
    d.symbol    = SymbolId{1};
    d.section   = DataSectionKind::Data;
    d.bytes     = {kValueOf[cu - 1], 0, 0, 0};
    d.alignment = Alignment::of<4>();
    ModuleSymbol row{d.symbol, "shared", bindingOf(what), SymbolVisibility::Default};
    row.weakKind = kindOf(what);
    m.symbols.push_back(std::move(row));
    m.dataItems.push_back(std::move(d));
    return m;
}

[[nodiscard]] std::optional<AssembledModule> readBack(Family const& fam, Loaded const& L,
                                                      std::vector<std::uint8_t> const& bytes,
                                                      DiagnosticReporter&              rep) {
    std::string_view const doc{fam.document};
    if (doc.starts_with("macho")) {
        return macho::readRelocatableObject(bytes, *L.target, *L.format, rep, CompilationUnitId{1});
    }
    if (doc.starts_with("pe")) {
        return pe::readRelocatableObject(bytes, *L.target, *L.format, rep, CompilationUnitId{1});
    }
    return elf::readRelocatableObject(bytes, *L.target, *L.format, rep, CompilationUnitId{1});
}

[[nodiscard]] Answer throughTheUnitLinker(Family const& fam, Loaded const& L, What first, What second) {
    std::vector<AssembledModule> const mods{linkUnit(1, first), linkUnit(2, second)};
    DiagnosticReporter                 rep;
    auto const artifact = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
    if (!artifact.ok()) {
        for (auto const& d : rep.all()) {
            if (d.code == DiagnosticCode::K_SymbolRedefinedAcrossUnits) return Answer{Pair::Refused};
        }
        return notUnderstood("the link failed, and not as a redefinition:" + diagnosticsOf(rep));
    }
    DiagnosticReporter readRep;
    auto const         back = readBack(fam, L, artifact.bytes, readRep);
    if (!back.has_value()) return notUnderstood("the artifact was not read back:" + diagnosticsOf(readRep));
    bool                        common = false;
    std::optional<std::uint8_t> value;
    std::size_t                 definitions = 0;
    for (auto const& e : back->externImports) {
        if (e.mangledName == "shared" && e.commonSize != 0u) common = true;
    }
    for (auto const& s : back->symbols) {
        if (s.name != "shared") continue;
        ++definitions;
        for (auto const& d : back->dataItems) {
            if (d.symbol == s.symbol && !d.bytes.empty()) value = d.bytes[0];
        }
    }
    if (common && definitions != 0) return notUnderstood("the artifact holds a common AND a definition of the name");
    if (!common && definitions != 1) {
        return notUnderstood("the artifact defines the name " + std::to_string(definitions) + " time(s)");
    }
    return survivorOf(first, second, common, value);
}

// ── DOORS 3 and 4: the MIR merge and the summary index, over two MIR modules ───────────────────────────────────
struct MirUnit {
    TypeInterner interner;
    Mir          mir;
    explicit MirUnit(std::uint32_t cu) : interner{CompilationUnitId{cu}} {}
};

void buildMirUnit(MirUnit& unit, std::uint32_t cu, What what) {
    TypeId const i32 = unit.interner.primitive(TypeKind::I32);
    MirBuilder   b;
    MirLinkage   linkage{bindingOf(what), SymbolVisibility::Default};
    if (auto const k = kindOf(what); k.has_value()) linkage.weakKind = *k;
    linkage.tentative = what == What::Tentative;
    std::uint32_t literal = UINT32_MAX;
    if (what != What::Tentative) {
        MirLiteralValue v;
        v.value = std::int64_t{kValueOf[cu - 1]};
        v.core  = TypeKind::I32;
        literal = b.literalPoolAdd(v);
    }
    (void)b.addGlobal(i32, SymbolId{7}, literal, MirFuncId{}, linkage, /*isConst=*/false, MirThreadStorage::Shared);
    unit.mir = std::move(b).finish();
}

[[nodiscard]] std::string nameOfSeven(SymbolId s) { return s.v == 7 ? std::string{"shared"} : std::string{}; }

[[nodiscard]] Answer throughTheMirMerge(Loaded const& L, What first, What second) {
    MirUnit a{1}, b{2};
    buildMirUnit(a, 1, first);
    buildMirUnit(b, 2, second);
    std::vector<MergeCuInput> cus(2);
    MirUnit*                  units[2] = {&a, &b};
    for (int u = 0; u < 2; ++u) {
        cus[u].mir             = &units[u]->mir;
        cus[u].interner        = &units[u]->interner;
        cus[u].nameOf          = nameOfSeven;
        cus[u].aggregateLayout = L.target->aggregateLayout();
    }
    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, {}, *L.format, rep);
    if (rep.errorCount() != 0) {
        for (auto const& d : rep.all()) {
            if (d.code == DiagnosticCode::K_SymbolRedefinedAcrossUnits) return Answer{Pair::Refused};
        }
        return notUnderstood("the merge failed, and not as a redefinition:" + diagnosticsOf(rep));
    }
    if (!merged.has_value()) return notUnderstood("the merge returned nothing and reported nothing");
    Mir const&                  mm = merged->mir;
    std::size_t                 definitions = 0;
    bool                        common      = false;
    std::optional<std::uint8_t> value;
    for (std::uint32_t i = 0; i < mm.moduleGlobalCount(); ++i) {
        MirGlobalId const g  = mm.globalAt(i);
        auto const        it = merged->symbolNames.find(mm.globalSymbol(g).v);
        if (it == merged->symbolNames.end() || it->second != "shared") continue;
        ++definitions;
        common = mm.globalIsCommon(g);
        if (std::uint32_t const idx = mm.globalInitLiteralIndex(g); idx != UINT32_MAX) {
            if (auto const* v = std::get_if<std::int64_t>(&mm.literalValue(idx).value)) {
                value = static_cast<std::uint8_t>(*v);
            }
        }
    }
    if (definitions != 1) {
        return notUnderstood("the merged module defines the name " + std::to_string(definitions) + " time(s)");
    }
    return survivorOf(first, second, common, value);
}

[[nodiscard]] Answer throughTheSummaryIndex(Loaded const& L, What first, What second) {
    MirUnit a{1}, b{2};
    buildMirUnit(a, 1, first);
    buildMirUnit(b, 2, second);
    std::vector<mirsum::ModuleSummary> summaries;
    for (MirUnit* unit : {&a, &b}) {
        mirsum::SummaryCuInput in;
        in.mir            = &unit->mir;
        in.nameOf         = nameOfSeven;
        in.moduleDigest   = "digest";
        in.targetIdentity = "one target";
        summaries.push_back(mirsum::buildModuleSummary(in));
    }
    DiagnosticReporter rep;
    auto const index = mirsum::buildSummaryIndex(summaries, mirsum::SummaryIndexPolicy{}, *L.format, rep);
    if (rep.errorCount() != 0) {
        for (auto const& d : rep.all()) {
            if (d.code == DiagnosticCode::K_SymbolRedefinedAcrossUnits) return Answer{Pair::Refused};
        }
        return notUnderstood("the index failed, and not as a redefinition:" + diagnosticsOf(rep));
    }
    if (!index.has_value()) return notUnderstood("the index returned nothing and reported nothing");
    auto const site = index->definitionOf("shared");
    if (!site.has_value()) return notUnderstood("the index elected nothing");
    return Answer{site->moduleIndex == 0 ? Pair::First : Pair::Second};
}

[[nodiscard]] char const* nameOf(Pair p) {
    return p == Pair::First ? "the FIRST unit's definition" : p == Pair::Second ? "the SECOND unit's definition"
                                                                              : "REFUSED";
}

// Runs every ordered pair of every family through `door` and holds each answer against the table.
template <class Door>
void everyCellAgreesWithTheTable(char const* doorName, Door&& door) {
    std::size_t cells = 0;
    for (Family const& fam : kFamilies) {
        Loaded const L = load(fam);
        ASSERT_TRUE(L.target && L.format) << fam.document;
        for (std::size_t i = 0; i < kEvery.size(); ++i) {
            for (std::size_t j = 0; j < kEvery.size(); ++j) {
                SCOPED_TRACE(std::string{fam.label} + ": " + kWhatName[i] + " then " + kWhatName[j]);
                Answer const got = door(fam, L, kEvery[i], kEvery[j]);
                ASSERT_TRUE(got.understood) << doorName << ": " << got.why;
                EXPECT_STREQ(nameOf(got.pair), nameOf(fam.table[i][j]))
                    << doorName << " must give this pair the answer its format's reference linkers give";
                ++cells;
            }
        }
    }
    EXPECT_EQ(cells, 48u) << "three families, sixteen ordered pairs each";
}

}  // namespace

// The table states what each family's DOCUMENT must hold for the pairs to come out as measured — the control that
// the cells above are read off shipped documents and not off a default.
TEST(CrossUnitFoldDoors, EachFamilysDocumentStatesWhatItsTableNeeds) {
    struct Want {
        char const*                 document;
        CommonYieldsTo              commonYieldsTo;
        std::optional<WeakBesideStrong> overridable, selectAny;
    };
    for (Want const& w : {Want{"elf64-x86_64-linux", CommonYieldsTo::StrongDefinition, WeakBesideStrong::Yields,
                               WeakBesideStrong::Yields},
                          Want{"pe64-x86_64-windows", CommonYieldsTo::NonOverridableDefinition,
                               WeakBesideStrong::Yields, WeakBesideStrong::Collides},
                          Want{"macho64-x86_64-darwin", CommonYieldsTo::AnyDefinition, WeakBesideStrong::Yields,
                               WeakBesideStrong::Yields}}) {
        SCOPED_TRACE(w.document);
        auto const f = ObjectFormatSchema::loadShipped(w.document);
        ASSERT_TRUE(f.has_value());
        EXPECT_EQ((*f)->commonYieldsTo(), std::optional<CommonYieldsTo>{w.commonYieldsTo});
        EXPECT_EQ((*f)->weakDefinitionBesideStrong().answerFor(WeakDefinitionKind::Overridable), w.overridable);
        EXPECT_EQ((*f)->weakDefinitionBesideStrong().answerFor(WeakDefinitionKind::SelectAny), w.selectAny);
    }
}

TEST(CrossUnitFoldDoors, TheFoldGivesEveryPairItsFormatsAnswer) {
    everyCellAgreesWithTheTable("the fold", [](Family const&, Loaded const& L, What first, What second) {
        return throughTheFold(L, first, second);
    });
}

TEST(CrossUnitFoldDoors, TheUnitLinkerGivesEveryPairItsFormatsAnswer) {
    everyCellAgreesWithTheTable("the unit linker", [](Family const& fam, Loaded const& L, What first, What second) {
        return throughTheUnitLinker(fam, L, first, second);
    });
}

TEST(CrossUnitFoldDoors, TheMirMergeGivesEveryPairItsFormatsAnswer) {
    everyCellAgreesWithTheTable("the MIR merge", [](Family const&, Loaded const& L, What first, What second) {
        return throughTheMirMerge(L, first, second);
    });
}

TEST(CrossUnitFoldDoors, TheSummaryIndexGivesEveryPairItsFormatsAnswer) {
    everyCellAgreesWithTheTable("the summary index", [](Family const&, Loaded const& L, What first, What second) {
        return throughTheSummaryIndex(L, first, second);
    });
}

// What the refusal SAYS, through each door that reports: the code of a redefinition and the key that decided it.
TEST(CrossUnitFoldDoors, ASelectAnyDefinitionBesideAStrongOneIsRefusedByTheKeysNameThroughEveryDoor) {
    Family const& pe = kFamilies[1];
    Loaded const  L  = load(pe);
    ASSERT_TRUE(L.target && L.format);
    auto const saysTheKey = [](DiagnosticReporter const& rep) {
        for (auto const& d : rep.all()) {
            if (d.code == DiagnosticCode::K_SymbolRedefinedAcrossUnits
                && d.actual.find("`weakDefinitionBesideStrong` as 'collides'") != std::string::npos
                && d.actual.find("symbol \"shared\" is defined STRONG") != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    for (bool const strongFirst : {true, false}) {
        SCOPED_TRACE(strongFirst ? "the strong definition's unit first" : "the select-any definition's unit first");
        What const first  = strongFirst ? What::Strong : What::SelectAny;
        What const second = strongFirst ? What::SelectAny : What::Strong;
        {
            std::vector<AssembledModule> const mods{linkUnit(1, first), linkUnit(2, second)};
            DiagnosticReporter                 rep;
            auto const out = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
            EXPECT_FALSE(out.ok());
            EXPECT_TRUE(saysTheKey(rep)) << "the unit linker" << diagnosticsOf(rep);
        }
        {
            MirUnit a{1}, b{2};
            buildMirUnit(a, 1, first);
            buildMirUnit(b, 2, second);
            std::vector<MergeCuInput> cus(2);
            cus[0].mir = &a.mir; cus[0].interner = &a.interner; cus[0].nameOf = nameOfSeven;
            cus[1].mir = &b.mir; cus[1].interner = &b.interner; cus[1].nameOf = nameOfSeven;
            DiagnosticReporter rep;
            (void)mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, {}, *L.format, rep);
            EXPECT_TRUE(saysTheKey(rep)) << "the MIR merge" << diagnosticsOf(rep);
            std::vector<mirsum::ModuleSummary> summaries;
            for (MirUnit* unit : {&a, &b}) {
                mirsum::SummaryCuInput in;
                in.mir            = &unit->mir;
                in.nameOf         = nameOfSeven;
                in.moduleDigest   = "digest";
                in.targetIdentity = "one target";
                summaries.push_back(mirsum::buildModuleSummary(in));
            }
            DiagnosticReporter indexRep;
            auto const index = mirsum::buildSummaryIndex(summaries, mirsum::SummaryIndexPolicy{}, *L.format, indexRep);
            EXPECT_TRUE(saysTheKey(indexRep)) << "the summary index" << diagnosticsOf(indexRep);
            ASSERT_TRUE(index.has_value());
            EXPECT_EQ(index->conflictingNames, std::vector<std::string>{"shared"});
        }
    }
}

// ══ THE KEY THAT DECIDES A WEAK DEFINITION BESIDE A STRONG ONE (`weakDefinitionBesideStrong`) ═══════════════════════
//
// What a strong definition is to a weak one of its name is the FORMAT's: ELF and Mach-O replace the weak
// definition, and PE replaces an overridable one and refuses a select-any one as a duplicate. It is read from the
// document of the link that resolves the pair — which an archive's member document never is — and a document that
// does not say is REFUSED BY THE KEY'S NAME the moment the question is asked, rather than answered by a default
// the formats disagree on.

namespace {

// The shipped document `format` with the root key `key` set to `value` — or removed, for a null `value`.
[[nodiscard]] auto documentWith(std::string const& format, char const* key, nlohmann::json value) {
    auto const    path = dss::test::configRoot() / "object-formats" / (format + ".format.json");
    std::ifstream in{path};
    auto          doc = nlohmann::json::parse(in);
    if (value.is_null()) {
        doc.erase(key);
    } else {
        doc[key] = std::move(value);
    }
    return ObjectFormatSchema::loadFromText(doc.dump(), format + ", '" + key + "' rewritten");
}

}  // namespace

TEST(WeakDefinitionBesideStrongKey, ItIsRefusedOnAnArchiveDocumentAndOutsideItsValues) {
    auto const pathNamed = [](auto const& loaded, std::string_view path) {
        bool named = false;
        for (auto const& d : loaded.error()) named = named || d.path == path;
        return named;
    };
    for (char const* archive : {"elf64-x86_64-linux-staticlib", "pe64-x86_64-windows-staticlib",
                                "macho64-x86_64-darwin-staticlib"}) {
        SCOPED_TRACE(archive);
        auto const onArchive = documentWith(archive, "weakDefinitionBesideStrong", "yields");
        ASSERT_FALSE(onArchive.has_value()) << "an archive document resolves no pair: declaring the key is refused";
        EXPECT_TRUE(pathNamed(onArchive, "/weakDefinitionBesideStrong"));
    }
    auto const badValue = documentWith("elf64-x86_64-linux-exec", "weakDefinitionBesideStrong", "sometimes");
    ASSERT_FALSE(badValue.has_value()) << "a value outside the closed set must be refused";
    auto const badKind = documentWith("pe64-x86_64-windows-exec", "weakDefinitionBesideStrong",
                                      nlohmann::json::object({{"overridable", "yields"}, {"tentative", "collides"}}));
    ASSERT_FALSE(badKind.has_value()) << "a kind that is no weak-definition kind must be refused";
    auto const oneKind = documentWith("pe64-x86_64-windows-exec", "weakDefinitionBesideStrong",
                                      nlohmann::json::object({{"overridable", "yields"}}));
    ASSERT_FALSE(oneKind.has_value()) << "an answer per kind states one for EVERY kind";
    EXPECT_TRUE(pathNamed(oneKind, "/weakDefinitionBesideStrong"));
    // CONTROLS: each value of the set loads, in either form.
    EXPECT_TRUE(documentWith("elf64-x86_64-linux-exec", "weakDefinitionBesideStrong", "collides").has_value());
    EXPECT_TRUE(documentWith("elf64-x86_64-linux-exec", "weakDefinitionBesideStrong", "yields").has_value());
    EXPECT_TRUE(documentWith("pe64-x86_64-windows-exec", "weakDefinitionBesideStrong",
                             nlohmann::json::object({{"overridable", "yields"}, {"select-any", "collides"}}))
                    .has_value());
}

TEST(WeakDefinitionBesideStrongKey, ADocumentThatDoesNotSayIsRefusedByTheKeysNameWhereTheQuestionIsAsked) {
    auto const loadedSilent = documentWith("elf64-x86_64-linux", "weakDefinitionBesideStrong", nullptr);
    ASSERT_TRUE(loadedSilent.has_value()) << "a document may be silent: nothing is asked until a pair needs it";
    Loaded L = load(kFamilies[0]);
    ASSERT_TRUE(L.target && L.format);
    L.format = *loadedSilent;
    EXPECT_FALSE(L.format->weakDefinitionBesideStrong().answerFor(WeakDefinitionKind::Overridable).has_value());

    auto const refusedByTheKey = [](DiagnosticReporter const& rep) {
        for (auto const& d : rep.all()) {
            if (d.code == DiagnosticCode::K_NoMatchingObjectFormat
                && d.actual.find("`weakDefinitionBesideStrong`") != std::string::npos
                && d.actual.find("symbol \"shared\"") != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    for (What const weak : {What::Overridable, What::SelectAny}) {
        for (bool const strongFirst : {true, false}) {
            SCOPED_TRACE(std::string{kWhatName[static_cast<std::size_t>(weak)]}
                         + (strongFirst ? " after the strong definition" : " before the strong definition"));
            What const first  = strongFirst ? What::Strong : weak;
            What const second = strongFirst ? weak : What::Strong;
            // The fold records the pair as one its document does not decide, and elects nothing for the name.
            std::vector<linker::CrossCuDef> defs;
            What const                      whats[2] = {first, second};
            for (std::uint32_t u = 0; u < 2; ++u) {
                defs.push_back(linker::CrossCuDef{"shared", bindingOf(whats[u]),
                                                  LinkedSymbolKey{CompilationUnitId{u + 1}, SymbolId{1}}});
                linker::stateTheLinksAnswers(defs.back(), *L.format, kindOf(whats[u]));
            }
            auto const r = linker::resolveCrossCuDefs(defs);
            ASSERT_EQ(r.undecided.size(), 1u);
            EXPECT_EQ(r.undecided[0].question, linker::CrossCuQuestion::WeakBesideStrong);
            EXPECT_TRUE(r.conflicts.empty() && r.collisions.empty());
            auto const refusal = linker::refusalOf(r.undecided[0], *L.format);
            EXPECT_EQ(refusal.code, DiagnosticCode::K_NoMatchingObjectFormat);
            EXPECT_NE(refusal.text.find("`weakDefinitionBesideStrong`"), std::string::npos) << refusal.text;
            // And every door that reports says so.
            {
                std::vector<AssembledModule> const mods{linkUnit(1, first), linkUnit(2, second)};
                DiagnosticReporter                 rep;
                auto const out = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
                EXPECT_FALSE(out.ok());
                EXPECT_TRUE(refusedByTheKey(rep)) << "the unit linker" << diagnosticsOf(rep);
            }
            {
                MirUnit a{1}, b{2};
                buildMirUnit(a, 1, first);
                buildMirUnit(b, 2, second);
                std::vector<MergeCuInput> cus(2);
                cus[0].mir = &a.mir; cus[0].interner = &a.interner; cus[0].nameOf = nameOfSeven;
                cus[1].mir = &b.mir; cus[1].interner = &b.interner; cus[1].nameOf = nameOfSeven;
                DiagnosticReporter rep;
                (void)mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, {}, *L.format, rep);
                EXPECT_TRUE(refusedByTheKey(rep)) << "the MIR merge" << diagnosticsOf(rep);
            }
        }
    }
    // THE CONTROL: where no strong definition meets a weak one the silent document is asked nothing — two weak
    // definitions fold to the first, and two strong ones are the ordinary redefinition.
    EXPECT_EQ(throughTheFold(L, What::Overridable, What::Overridable).pair, Pair::First);
    EXPECT_EQ(throughTheFold(L, What::Strong, What::Strong).pair, Pair::Refused);
    Answer const twoWeak = throughTheUnitLinker(kFamilies[0], L, What::Overridable, What::Overridable);
    EXPECT_TRUE(twoWeak.understood) << twoWeak.why;
    EXPECT_EQ(twoWeak.pair, Pair::First);
}
