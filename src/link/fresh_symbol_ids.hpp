#pragma once

#include "asm/asm.hpp"                         // AssembledModule
#include "core/types/diagnostic_reporter.hpp"  // DiagnosticReporter, report
#include "core/types/parse_diagnostic.hpp"     // DiagnosticCode::K_SymbolIdSpaceExhausted
#include "core/types/strong_ids.hpp"           // SymbolId
#include "core/types/symbol_id_cursor.hpp"     // SymbolIdEnd -- the one rule of a fresh SymbolId

#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <vector>

// ★★ THE LINK TIER'S DOORS OF A FRESH SymbolId (P69, lane `cs`, fold f26d -- review-csf26c2 N9; f26e -- B1). Every id
// a link-tier pass mints comes through a door of the one home of a fresh id (core/types/symbol_id_cursor.hpp): a pass
// on a module through `ModuleSymbolIds` below (the home's OPTIONAL door over the module's end), the link merge's new
// numbering through the home's REPORTED door (`ReportedSymbolIds`, linker.cpp), an object reader's table through
// `RecordSymbolIds` (link/format/record_symbol_ids.hpp). No link-tier source adds to an id
// (tests/core/test_symbol_id_mints_guard.cpp), and every refusal is the home's one sentence under K_.
//
// ★ AN ASSEMBLED MODULE IS A COUNTING SPACE, AND IT CARRIES ITS END (`AssembledModule::symbolIdEnd`), GROW-ONLY.
// A pass that mints on a module -- the entry trampoline, the branch veneers, the import-slot materializations, the
// GOT slots, the PE residue runner, the import-address folds, the null-address symbols, the required entry
// reference, the weak-name restatement, a discarded section group's reference row -- opens a `ModuleSymbolIds` ON the
// module. Opening raises the module's end past every id ANY holder of the module holds (`kModuleSymbolIdHolders`:
// every member the types declare, which tests/link/test_module_symbol_id_holders.cpp derives from the headers and
// compares in both directions), and the door holds a REFERENCE to that end, which every take that succeeds advances:
// a second door opened on the module, before or after the first wrote anything back, continues the first, and an id
// an erasure freed stays below the end and is never handed out again. Exhaustion is the door's own refusal, by name
// (`K_SymbolIdSpaceExhausted`), and a refusal writes NOTHING; the caller propagates the failure and never sees an
// invalid id.
//
// THE SCAN'S HISTORY, which is why the holders are a table the types check: the scan this replaced read the
// holders by hand and was WIDENED seven times, each after an id it did not read was handed out again (a promoted
// string literal's datum, a computed-goto label, an import's address slot and call entry, a null-address symbol,
// an image-defined symbol, an exception scope's filter and personality, a symbol ROW of a reader's unit, a carried
// handler's routine, relocations and area) -- and it still missed five holders (the function and data relocation
// targets, the link-order positions, the constructor schedule and the unit's entry). It also told its callers to
// "mint sequentially from the returned value", since a second call between mints returned the same number; the
// carried end makes that a property of the module, not of the caller.
namespace dss::linker {

// One holder of an assembled module: its path by member names (the type walk's spelling), and the read that
// raises `highest` to the highest id value it holds.
struct ModuleSymbolIdHolder {
    std::string_view path;
    void (*read)(AssembledModule const&, std::uint32_t& highest);
};

// Raise `highest` to `held`'s value. Not in a `detail` namespace: opening `dss::linker::detail` here would make
// every includer's unqualified `detail::` (meaning `dss::detail`) find this one instead.
inline void seeHeldSymbolId(std::uint32_t& highest, SymbolId held) noexcept {
    if (held.v > highest) highest = held.v;
}

// The table's size is its rows' (a row dropped or added is one contiguous edit).
inline constexpr ModuleSymbolIdHolder kModuleSymbolIdHolders[] = {
    {"functions.symbol",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& f : m.functions) seeHeldSymbolId(h, f.symbol);
     }},
    {"functions.relocations.target",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& f : m.functions)
             for (auto const& r : f.relocations) seeHeldSymbolId(h, r.target);
     }},
    {"functions.blockSymbols.symbol",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& f : m.functions)
             for (auto const& b : f.blockSymbols) seeHeldSymbolId(h, b.symbol);
     }},
    {"functions.sehScopes.filterFuncletSymbol",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& f : m.functions)
             for (auto const& s : f.sehScopes) seeHeldSymbolId(h, s.filterFuncletSymbol);
     }},
    {"functions.sehScopes.personalitySymbol",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& f : m.functions)
             for (auto const& s : f.sehScopes) seeHeldSymbolId(h, s.personalitySymbol);
     }},
    {"functions.languageHandler.personality",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& f : m.functions)
             if (f.languageHandler.has_value()) seeHeldSymbolId(h, f.languageHandler->personality);
     }},
    {"functions.languageHandler.relocations.target",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& f : m.functions)
             if (f.languageHandler.has_value())
                 for (auto const& r : f.languageHandler->relocations) seeHeldSymbolId(h, r.target);
     }},
    {"functions.languageHandler.dataArea.item",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& f : m.functions)
             if (f.languageHandler.has_value() && f.languageHandler->dataArea.has_value())
                 seeHeldSymbolId(h, f.languageHandler->dataArea->item);
     }},
    {"dataItems.symbol",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& d : m.dataItems) seeHeldSymbolId(h, d.symbol);
     }},
    {"dataItems.relocations.target",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& d : m.dataItems)
             for (auto const& r : d.relocations) seeHeldSymbolId(h, r.target);
     }},
    {"externImports.symbol",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& x : m.externImports) seeHeldSymbolId(h, x.symbol);
     }},
    {"externImports.addressSlotSymbol",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& x : m.externImports) seeHeldSymbolId(h, x.addressSlotSymbol);
     }},
    {"externImports.callEntrySymbol",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& x : m.externImports) seeHeldSymbolId(h, x.callEntrySymbol);
     }},
    {"nullAddressSymbols",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (SymbolId const s : m.nullAddressSymbols) seeHeldSymbolId(h, s);
     }},
    {"imageDefinedSymbols.symbol",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& b : m.imageDefinedSymbols) seeHeldSymbolId(h, b.symbol);
     }},
    {"symbols.symbol",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& r : m.symbols) seeHeldSymbolId(h, r.symbol);
     }},
    {"linkPositionOfDefinition",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& [definition, at] : m.linkPositionOfDefinition) {
             (void)at;
             seeHeldSymbolId(h, definition);
         }
     }},
    {"staticInitSchedule.symbol",
     [](AssembledModule const& m, std::uint32_t& h) {
         for (auto const& e : m.staticInitSchedule) seeHeldSymbolId(h, e.symbol);
     }},
    {"userEntrySymbol",
     [](AssembledModule const& m, std::uint32_t& h) {
         if (m.userEntrySymbol.has_value()) seeHeldSymbolId(h, *m.userEntrySymbol);
     }},
};

// The highest id value any holder of `module` holds (0: none) -- a READ; it mints nothing.
[[nodiscard]] inline std::uint32_t maxExistingSymbolIdV(AssembledModule const& module) noexcept {
    std::uint32_t highest = 0;
    for (ModuleSymbolIdHolder const& holder : kModuleSymbolIdHolders) holder.read(module, highest);
    return highest;
}

// Where the link tier's doors state a refusal: the reporter, under K_ (the home's one emitter writes the sentence).
[[nodiscard]] inline SymbolIdRefusalSink linkRefusalSink(DiagnosticReporter& reporter) noexcept {
    return SymbolIdRefusalSink{&reporter, DiagnosticCode::K_SymbolIdSpaceExhausted};
}

// THE MODULE DOOR. Opened ON a module: the module's carried end is raised past every id its holders hold (the end is
// never below an id the module holds -- on a module whose end already says so, opening writes nothing), and every
// take that SUCCEEDS advances THAT end, so the module remembers every id any door handed out (a second door opened
// on it, before or after, continues the first).
// ★ A REFUSAL MOVES NOTHING (P69 fold f26d, main's ruling on the take sites): a take that cannot hand out every id it
// was asked for writes nothing -- not the end, not a reserved value it stepped over. A pass that needs several ids on
// a module ITS CALLER holds asks for all of them at once (`take(count, ...)`), so it is refused before it has touched
// the module; one id at a time is for a door opened on the pass's own scratch copy, which the caller discards on a
// refusal (tests/link/test_module_door_take_sites.cpp holds every take site to it).
class ModuleSymbolIds {
public:
    explicit ModuleSymbolIds(AssembledModule& module) noexcept : end_(module.symbolIdEnd) {
        end_.keepClearOfHighest(maxExistingSymbolIdV(module));
    }
    ModuleSymbolIds(ModuleSymbolIds const&)            = delete;
    ModuleSymbolIds& operator=(ModuleSymbolIds const&) = delete;

    // The module's next id; or, the space exhausted, nullopt -- the refusal REPORTED by the home's OPTIONAL door,
    // naming `who`, and nothing written.
    [[nodiscard]] std::optional<SymbolId> take(DiagnosticReporter& reporter, std::string_view who) {
        return OptionalSymbolIds{end_, linkRefusalSink(reporter)}.take(who);
    }

    // The module's next `count` ids, ALL OR NONE: the space too short for every one of them, nullopt -- the refusal
    // REPORTED once, naming `who`, and nothing written.
    [[nodiscard]] std::optional<std::vector<SymbolId>> take(std::size_t count, DiagnosticReporter& reporter,
                                                            std::string_view who) {
        return OptionalSymbolIds{end_, linkRefusalSink(reporter)}.take(count, who);
    }

private:
    SymbolIdEnd& end_;   // the module's: raised when the door opens, advanced by a take that succeeds, by nothing else
};

// THE DOOR OF A NEW SPACE WHOSE IDS PASS THROUGH MANY HANDS BEFORE THEY REACH A MODULE -- the link merge's numbering
// of the merged module -- is the home's REPORTED door (`ReportedSymbolIds`): the refusal reported once, by name, the
// invalid id from then on, and the merged module leaves `mergeModules` ONLY through the door's `seal` (nullopt once
// refused) or `withhold` (P69 f26e, B1). The merged module carries no end of the space: every id the merge handed out
// that anything names is held by the merged module (its one output), so the module's scan is that end, and a door
// opened on it continues the space.

}  // namespace dss::linker
