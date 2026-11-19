// THE LINK TIER'S DOORS OF A FRESH SymbolId (P69 lane `cs`, fold f26d -- review-csf26c2 N9): the module door
// (`ModuleSymbolIds`, opened on a module that carries its end) and the door of a new space (`NewSpaceSymbolIds`, the
// link merge's numbering), link/fresh_symbol_ids.hpp; and an object reader's record table (`RecordSymbolIds`,
// link/format/record_symbol_ids.hpp).
//
// THE UNIVERSE, per door: a mint past every holder of its space; a mint after a mint; a second door of the space;
// an id an erasure freed; exhaustion, refused by name. The module door's holder axis is the scan's own table
// (`kModuleSymbolIdHolders`): one cell PER ROW, a module whose only maximum sits in that holder -- and a row with
// no planting below is a failure, so every declared row is a VISITED row (that the table is every holder the types
// declare is test_module_symbol_id_holders.cpp's).
//
// RED-ON-DISABLE: drop a holder's read from the table and its cell hands out the planted id; let the door copy the
// module's end instead of referring to it and the two-door cell hands out one id twice; compute the next unsaturated
// and the exhausted cells hand out an id.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_id_cursor.hpp"
#include "link/fresh_symbol_ids.hpp"
#include "link/format/record_symbol_ids.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace dss;

namespace {

constexpr std::uint32_t kLast    = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kPlanted = 1000u;

[[nodiscard]] std::size_t countOf(DiagnosticReporter const& rep, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) n += d.code == code ? 1u : 0u;
    return n;
}

[[nodiscard]] AssembledFunction functionOf(std::uint32_t id) {
    AssembledFunction f;
    f.symbol = SymbolId{id};
    f.bytes.assign(4, 0u);
    return f;
}

// A module of small ids (function #1, datum #2, import #3): the baseline every planting raises past.
[[nodiscard]] AssembledModule baseline() {
    AssembledModule m;
    m.functions.push_back(functionOf(1));
    m.expectedFuncCount = 1;
    AssembledData d;
    d.symbol = SymbolId{2};
    d.bytes.assign(4, 0u);
    m.dataItems.push_back(d);
    ExternImport x;
    x.symbol      = SymbolId{3};
    x.mangledName = "imported";
    m.externImports.push_back(x);
    return m;
}

[[nodiscard]] Relocation relocationTo(SymbolId target) {
    Relocation r;
    r.target = target;
    return r;
}

// One planting per holder, keyed by the holder's path: `id` goes into that holder ALONE.
[[nodiscard]] std::map<std::string, std::function<void(AssembledModule&, SymbolId)>> plantings() {
    std::map<std::string, std::function<void(AssembledModule&, SymbolId)>> p;
    p["functions.symbol"] = [](AssembledModule& m, SymbolId id) { m.functions.push_back(functionOf(id.v)); };
    p["functions.relocations.target"] = [](AssembledModule& m, SymbolId id) {
        m.functions[0].relocations.push_back(relocationTo(id));
    };
    p["functions.blockSymbols.symbol"] = [](AssembledModule& m, SymbolId id) {
        m.functions[0].blockSymbols.push_back(SyntheticBlockSymbol{id, 0});
    };
    p["functions.sehScopes.filterFuncletSymbol"] = [](AssembledModule& m, SymbolId id) {
        SehScopeEntry s;
        s.filterFuncletSymbol = id;
        m.functions[0].sehScopes.push_back(s);
    };
    p["functions.sehScopes.personalitySymbol"] = [](AssembledModule& m, SymbolId id) {
        SehScopeEntry s;
        s.personalitySymbol = id;
        m.functions[0].sehScopes.push_back(s);
    };
    p["functions.languageHandler.personality"] = [](AssembledModule& m, SymbolId id) {
        LanguageHandler h;
        h.personality = id;
        m.functions[0].languageHandler = h;
    };
    p["functions.languageHandler.relocations.target"] = [](AssembledModule& m, SymbolId id) {
        LanguageHandler h;
        h.relocations.push_back(relocationTo(id));
        m.functions[0].languageHandler = h;
    };
    p["functions.languageHandler.dataArea.item"] = [](AssembledModule& m, SymbolId id) {
        LanguageHandler h;
        h.dataArea = DataAreaRef{id, 0};
        m.functions[0].languageHandler = h;
    };
    p["dataItems.symbol"] = [](AssembledModule& m, SymbolId id) {
        AssembledData d;
        d.symbol = id;
        d.bytes.assign(4, 0u);
        m.dataItems.push_back(d);
    };
    p["dataItems.relocations.target"] = [](AssembledModule& m, SymbolId id) {
        m.dataItems[0].relocations.push_back(relocationTo(id));
    };
    p["externImports.symbol"] = [](AssembledModule& m, SymbolId id) {
        ExternImport x;
        x.symbol      = id;
        x.mangledName = "planted";
        m.externImports.push_back(x);
    };
    p["externImports.addressSlotSymbol"] = [](AssembledModule& m, SymbolId id) {
        m.externImports[0].addressSlotSymbol = id;
    };
    p["externImports.callEntrySymbol"] = [](AssembledModule& m, SymbolId id) {
        m.externImports[0].callEntrySymbol = id;
    };
    p["nullAddressSymbols"] = [](AssembledModule& m, SymbolId id) { m.nullAddressSymbols.push_back(id); };
    p["imageDefinedSymbols.symbol"] = [](AssembledModule& m, SymbolId id) {
        AssembledModule::ImageDefinedBinding b;
        b.symbol = id;
        m.imageDefinedSymbols.push_back(b);
    };
    p["symbols.symbol"] = [](AssembledModule& m, SymbolId id) {
        m.symbols.push_back(ModuleSymbol{id, "planted_row", SymbolBinding::Local, SymbolVisibility::Default});
    };
    p["linkPositionOfDefinition"] = [](AssembledModule& m, SymbolId id) {
        m.linkPositionOfDefinition.insert_or_assign(id, 0u);
    };
    p["staticInitSchedule.symbol"] = [](AssembledModule& m, SymbolId id) {
        m.staticInitSchedule.push_back(LirStaticInitEntry{id, {}});
    };
    p["userEntrySymbol"] = [](AssembledModule& m, SymbolId id) { m.userEntrySymbol = id; };
    return p;
}

} // namespace

// EVERY HOLDER: a module whose only id past the baseline sits in that holder alone; the door's id is past it.
TEST(FreshSymbolIds, ADoorMintsPastAnIdHeldInAnyHolderOfTheModule) {
    auto const plant = plantings();
    std::set<std::string> rows;
    for (linker::ModuleSymbolIdHolder const& holder : linker::kModuleSymbolIdHolders) {
        std::string const path{holder.path};
        SCOPED_TRACE(path);
        rows.insert(path);
        auto const it = plant.find(path);
        if (it == plant.end()) {
            ADD_FAILURE() << "the scan's row '" << path << "' has no planting cell: a row nothing visits";
            continue;
        }
        AssembledModule m = baseline();
        it->second(m, SymbolId{kPlanted});
        EXPECT_EQ(linker::maxExistingSymbolIdV(m), kPlanted) << "the scan reads the holder";
        DiagnosticReporter rep;
        linker::ModuleSymbolIds ids{m};
        std::optional<SymbolId> const id = ids.take(rep, "the holder cell");
        ASSERT_TRUE(id.has_value());
        EXPECT_GT(id->v, kPlanted) << "the door handed out an id the holder holds, or one below it";
        EXPECT_EQ(rep.errorCount(), 0u);
    }
    for (auto const& [path, _] : plant) {
        EXPECT_TRUE(rows.contains(path)) << "the planting '" << path << "' names no row of the scan";
    }
}

// A MINT AFTER A MINT, AND A SECOND DOOR: two doors opened on one module before either mints, taking alternately,
// hand out distinct ids, each past the other's last; and the module remembers them (a door opened later continues).
TEST(FreshSymbolIds, TwoDoorsOnOneModuleContinueEachOther) {
    AssembledModule m = baseline();
    DiagnosticReporter rep;
    linker::ModuleSymbolIds a{m};
    linker::ModuleSymbolIds b{m};
    std::set<std::uint32_t> seen;
    std::uint32_t last = linker::maxExistingSymbolIdV(m);
    for (int k = 0; k < 4; ++k) {
        for (linker::ModuleSymbolIds* door : {&a, &b}) {
            std::optional<SymbolId> const id = door->take(rep, "the two-door cell");
            ASSERT_TRUE(id.has_value());
            EXPECT_GT(id->v, last) << "a take is past every id either door handed out";
            EXPECT_TRUE(seen.insert(id->v).second) << "one id handed out twice";
            last = id->v;
        }
    }
    EXPECT_EQ(m.symbolIdEnd.value(), last + 1u) << "the module carries the end both doors advanced";
    linker::ModuleSymbolIds later{m};
    std::optional<SymbolId> const next = later.take(rep, "the later door");
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->v, last + 1u) << "a door opened later, with nothing written back, continues the first two";
    EXPECT_EQ(rep.errorCount(), 0u);
}

// AN ID AN ERASURE FREED is never handed out again: a door opened after the holder at the maximum was erased.
TEST(FreshSymbolIds, AnIdAnErasureFreedIsNeverHandedOutAgain) {
    AssembledModule m = baseline();
    m.functions.push_back(functionOf(50));
    DiagnosticReporter rep;
    { linker::ModuleSymbolIds opened{m}; }   // a pass opened a door on the module and minted nothing
    m.functions.pop_back();                  // ... then erased the function #50
    ASSERT_LT(linker::maxExistingSymbolIdV(m), 50u) << "the PREMISE: no holder holds #50 any more";
    linker::ModuleSymbolIds ids{m};
    std::optional<SymbolId> const id = ids.take(rep, "the erasure cell");
    ASSERT_TRUE(id.has_value());
    EXPECT_GT(id->v, 50u) << "the erased #50 may still be named outside the module";
}

// EXHAUSTION is the door's own refusal, by name: never 0, never a wrapped id.
TEST(FreshSymbolIds, AnExhaustedModuleIsRefusedByName) {
    for (std::uint32_t const held : {kLast - 1u, kLast}) {
        SCOPED_TRACE(held);
        AssembledModule m = baseline();
        m.functions.push_back(functionOf(held));
        DiagnosticReporter rep;
        linker::ModuleSymbolIds ids{m};
        EXPECT_FALSE(ids.take(rep, "the exhausted cell").has_value()) << "no id past the last value";
        EXPECT_EQ(countOf(rep, DiagnosticCode::K_SymbolIdSpaceExhausted), 1u);
        ASSERT_FALSE(rep.all().empty());
        EXPECT_NE(rep.all().front().actual.find("the exhausted cell"), std::string::npos)
            << "the refusal names what was being minted";
    }
    // The control: one id short of the edge mints that id once, then refuses.
    AssembledModule m = baseline();
    m.functions.push_back(functionOf(kLast - 2u));
    DiagnosticReporter rep;
    linker::ModuleSymbolIds ids{m};
    std::optional<SymbolId> const id = ids.take(rep, "the edge cell");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->v, kLast - 1u);
    EXPECT_FALSE(ids.take(rep, "the edge cell").has_value());
    EXPECT_EQ(countOf(rep, DiagnosticCode::K_SymbolIdSpaceExhausted), 1u);
}

// TAKE K OR NONE (P69 fold f26d, the take sites): a door asked for more ids than its space has left hands out none
// and writes NOTHING -- the module's end and every holder as they were; asked for what is left, it hands out exactly
// those, in order, and the end moves past them. A pass that needs several ids on a module its caller holds asks for
// them this way, so an exhausted space refuses it before it has touched the module.
TEST(FreshSymbolIds, ARefusedTakeOfSeveralIdsWritesNothing) {
    AssembledModule m = baseline();
    m.symbolIdEnd.keepClearOfHighest(kLast - 3u);   // two ids are left: kLast - 2 and kLast - 1
    std::uint32_t const endBefore       = m.symbolIdEnd.value();
    std::uint32_t const heldBefore      = linker::maxExistingSymbolIdV(m);
    std::size_t const   functionsBefore = m.functions.size();
    DiagnosticReporter rep;
    linker::ModuleSymbolIds ids{m};
    EXPECT_EQ(m.symbolIdEnd.value(), endBefore) << "CONTROL: opening a door on an end past every holder writes nothing";
    EXPECT_FALSE(ids.take(3u, rep, "the three-id cell").has_value()) << "three ids where two are left";
    EXPECT_EQ(countOf(rep, DiagnosticCode::K_SymbolIdSpaceExhausted), 1u) << "refused once, by name";
    EXPECT_EQ(m.symbolIdEnd.value(), endBefore) << "a refusal writes nothing: the end did not move";
    EXPECT_EQ(linker::maxExistingSymbolIdV(m), heldBefore);
    EXPECT_EQ(m.functions.size(), functionsBefore);
    // The control: the two ids that are left, in order, and the end past them.
    std::optional<std::vector<SymbolId>> const two = ids.take(2u, rep, "the two-id cell");
    ASSERT_TRUE(two.has_value());
    ASSERT_EQ(two->size(), 2u);
    EXPECT_EQ((*two)[0].v, kLast - 2u);
    EXPECT_EQ((*two)[1].v, kLast - 1u);
    EXPECT_EQ(m.symbolIdEnd.value(), kLast) << "the module carries the end past both";
    EXPECT_EQ(countOf(rep, DiagnosticCode::K_SymbolIdSpaceExhausted), 1u) << "the control refuses nothing";
}

// THE DOOR OF A NEW SPACE (the link merge's numbering): from 1, a reserved value stepped over, and exhaustion
// reported ONCE, the invalid id from then on, the owner told (`refused`).
TEST(FreshSymbolIds, ANewSpaceIsNumberedFromOneAndRefusesOnceWhenExhausted) {
    // A reporter with no dedup window: the window drops a word-for-word repeat of a recent report, which hid a door
    // reporting per take (MEASURED: under the default window the arm survived on macos-arm64-debug, run 20261117-084234-b06ffb3c).
    DiagnosticReporter::Config noWindow;
    noWindow.dedupWindow = 0;
    DiagnosticReporter rep{noWindow};
    // The merge's numbering is the home's REPORTED door (P69 f26e, B1): its owner's output leaves through the seal.
    SymbolIdRefusalSink const sink = linker::linkRefusalSink(rep);
    ReportedSymbolIds fresh{SymbolIdEnd{}, sink, "the new-space cell"};
    EXPECT_EQ(fresh.take().v, 1u);
    EXPECT_EQ(fresh.take().v, 2u);
    EXPECT_EQ(fresh.seal(std::string{"merged"}), std::optional<std::string>{"merged"})
        << "CONTROL: an unrefused space's output is sealed whole";

    SymbolIdEnd nearReserved;
    nearReserved.keepClearOfHighest(kTlsIndexReservedSymbolIdValue - 1u);
    ReportedSymbolIds past{nearReserved, sink, "the reserved cell"};
    EXPECT_EQ(past.take().v, kTlsIndexReservedSymbolIdValue + 1u) << "the value a writer reserves is no merged id";
    EXPECT_TRUE(past.seal(1).has_value());

    SymbolIdEnd edge;
    edge.keepClearOfHighest(kLast - 2u);
    ReportedSymbolIds last{edge, sink, "the edge cell"};
    EXPECT_EQ(last.take().v, kLast - 1u);
    EXPECT_FALSE(last.take().valid());
    EXPECT_FALSE(last.take().valid());
    EXPECT_FALSE(last.seal(std::string{"merged"}).has_value()) << "a refused space's output was sealed";
    EXPECT_EQ(countOf(rep, DiagnosticCode::K_SymbolIdSpaceExhausted), 1u) << "reported once, not per take";
}

// AN OBJECT'S RECORD TABLE: an index space that ends below the first reserved value (a table that reaches it is
// refused when it is OPENED), and a counting space past it -- the home's OPTIONAL door -- that steps over the
// reserved values and refuses the last, at every take, writing nothing (P69 f26e, B1).
TEST(FreshSymbolIds, ARecordTableEndsBelowTheReservedValuesAndRefusesItsExhaustion) {
    {
        DiagnosticReporter rep;
        EXPECT_FALSE(link::format::RecordSymbolIds::open(kFirstWriterReservedSymbolIdValue, rep, "the reader cell"))
            << "record 0 would take the first reserved value as its id";
        EXPECT_EQ(countOf(rep, DiagnosticCode::K_SymbolIdSpaceExhausted), 1u);
    }
    DiagnosticReporter rep;
    std::optional<link::format::RecordSymbolIds> opened =
        link::format::RecordSymbolIds::open(kFirstWriterReservedSymbolIdValue - 1u, rep, "the reader cell");
    ASSERT_TRUE(opened.has_value()) << "a table one short of the first reserved value fits";
    link::format::RecordSymbolIds& ids = *opened;
    EXPECT_EQ(ids.of(0).v, kFirstWriterReservedSymbolIdValue - 1u) << "record 0 takes the record count";
    std::uint32_t taken = 0;
    std::uint32_t lastId = 0;
    for (std::optional<SymbolId> id = ids.fresh(); id.has_value(); id = ids.fresh()) {
        EXPECT_FALSE(isWriterReservedSymbolIdValue(id->v)) << "a fresh id is never a reserved value";
        EXPECT_GT(id->v, lastId);
        lastId = id->v;
        ++taken;
        ASSERT_LT(taken, 1000u) << "the space past the table never ends";
    }
    EXPECT_EQ(lastId, kLast - 1u) << "the last value is never handed out";
    EXPECT_EQ(countOf(rep, DiagnosticCode::K_SymbolIdSpaceExhausted), 1u) << "the refused take, by name";
    EXPECT_FALSE(ids.fresh().has_value()) << "and stays refused";
    EXPECT_EQ(countOf(rep, DiagnosticCode::K_SymbolIdSpaceExhausted), 2u) << "every refused take is reported";
    EXPECT_EQ(ids.end().value(), kLast) << "the end the reader's module carries: a refusal wrote nothing";
}
