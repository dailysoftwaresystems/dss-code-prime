#pragma once

// ★ THE STATE OF AN ASSEMBLED MODULE, AS ONE STRING (P69, lane `cs`, fold f26d -- the take sites). A pass refused by
// an exhausted id space must leave the module its caller holds exactly as it was; this is the test's reading of
// "exactly": two modules with equal digests hold the same rows, the same bytes, the same ids in every holder and the
// same carried end. Read field by field (C++ has no reflection), so the digest names what it reads:
//   * every member of `AssembledModule` (asm/asm.hpp): functions, expectedFuncCount, cuId, symbols, externImports,
//     dataItems, sectionGroupOfInputSection, sectionGroupRuleOf, linkPosition, linkPositionOfDefinition,
//     nullAddressSymbols, imageDefinedSymbols, staticInitSchedule, linkerRequests (each list's size), imageEntryOverride,
//     userEntrySymbol, symbolIdEnd;
//   * of a function: symbol, bytes, relocations, sourceMap (its size), cfi (present or not), sehScopes,
//     languageHandler, blockSymbols, blockByteOffsets, inputSection;
//   * of a data item: symbol, section, bytes, alignment, relocations, reservedSize, inputSection;
//   * of a symbol row and an import row: every member their structs declare (core/types/extern_import.hpp).
// The unordered maps are read in key order, so the digest is the module's, never its hash order's.
// BLIND TO: a function's CFI CONTENT and its source map's entries (their presence and count are read) -- no take site
// writes either.

#include "asm/asm.hpp"
#include "core/types/extern_import.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace dss::test {

namespace module_digest_detail {

inline void put(std::string& out, std::string_view field, std::string const& value) {
    out += field;
    out += '=';
    out += value;
    out += ';';
}

[[nodiscard]] inline std::string bytesOf(std::vector<std::uint8_t> const& b) {
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a over the bytes, with their count beside it
    for (std::uint8_t const c : b) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return std::format("{}:{:016x}", b.size(), h);
}

[[nodiscard]] inline std::string relocationsOf(std::vector<Relocation> const& rs) {
    std::string s = std::format("{}[", rs.size());
    for (Relocation const& r : rs)
        s += std::format("({},{},{},{},{})", r.offset, r.target.v, r.kind.v, r.addend, r.bytesAfterField);
    return s + "]";
}

[[nodiscard]] inline std::string sliceOf(std::optional<InputSectionSlice> const& s) {
    return s.has_value() ? std::format("{}+{}", s->section, s->offset) : std::string{"-"};
}

}  // namespace module_digest_detail

[[nodiscard]] inline std::string digestOf(AssembledModule const& m) {
    using namespace module_digest_detail;
    std::string out;
    put(out, "functions", std::to_string(m.functions.size()));
    for (AssembledFunction const& f : m.functions) {
        std::string s = std::format("{}|{}|{}|map{}|cfi{}", f.symbol.v, bytesOf(f.bytes), relocationsOf(f.relocations),
                                    f.sourceMap.size(), f.cfi.has_value() ? 1 : 0);
        for (SehScopeEntry const& e : f.sehScopes)
            s += std::format("|seh{},{},{},{},{}", e.beginByteOffset, e.endByteOffset, e.jumpTargetByteOffset,
                             e.filterFuncletSymbol.v, e.personalitySymbol.v);
        if (f.languageHandler.has_value()) {
            LanguageHandler const& h = *f.languageHandler;
            s += std::format("|lh{},{},{},{},{}", h.personality.v, h.exceptionPhase ? 1 : 0, h.unwindPhase ? 1 : 0,
                             bytesOf(h.data), relocationsOf(h.relocations));
            if (h.dataArea.has_value()) s += std::format(",area{}+{}", h.dataArea->item.v, h.dataArea->offset);
        }
        for (SyntheticBlockSymbol const& b : f.blockSymbols) s += std::format("|blk{}@{}", b.symbol.v, b.blockByteOffset);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> offsets(f.blockByteOffsets.begin(), f.blockByteOffsets.end());
        std::ranges::sort(offsets);
        for (auto const& [block, at] : offsets) s += std::format("|off{}={}", block, at);
        s += "|in" + sliceOf(f.inputSection);
        put(out, "fn", s);
    }
    put(out, "expectedFuncCount", std::to_string(m.expectedFuncCount));
    put(out, "cuId", std::to_string(m.cuId.v));
    put(out, "symbols", std::to_string(m.symbols.size()));
    for (ModuleSymbol const& r : m.symbols)
        put(out, "sym",
            std::format("{}|{}|{}|{}|{}|{}|{}|{}|{}", r.symbol.v, r.name, static_cast<int>(r.binding),
                        static_cast<int>(r.visibility), static_cast<int>(r.duplicateMatch),
                        r.weakKind.has_value() ? static_cast<int>(*r.weakKind) : -1, r.referencedByName ? 1 : 0,
                        r.oneInstancePerProcess ? 1 : 0, r.weakDefaultName));
    put(out, "externImports", std::to_string(m.externImports.size()));
    for (ExternImport const& e : m.externImports)
        put(out, "ext",
            std::format("{}|{}|{}|{}{}{}{}{}{}{}{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", e.symbol.v, e.mangledName,
                        e.libraryPath, e.isData ? 1 : 0, e.isThreadLocal ? 1 : 0,
                        e.recordStatesStorageDuration ? 1 : 0, e.isEagerImport ? 1 : 0, e.searchesArchives ? 1 : 0,
                        e.isPreemptionReference ? 1 : 0, e.readThroughSlot ? 1 : 0, e.requiredByDirective ? 1 : 0,
                        static_cast<int>(e.binding), e.dataSizeBytes, e.dataAlignBytes, e.version,
                        e.addressSlotSymbol.v, e.callEntrySymbol.v, static_cast<int>(e.kindOrigin), e.fallbackName,
                        e.commonSize, e.commonAlignment, static_cast<int>(e.commonVisibility),
                        e.commonAlignmentStated));
    put(out, "dataItems", std::to_string(m.dataItems.size()));
    for (AssembledData const& d : m.dataItems)
        put(out, "data",
            std::format("{}|{}|{}|{}|{}|{}|{}", d.symbol.v, static_cast<int>(d.section), bytesOf(d.bytes),
                        d.alignment.bytes(), relocationsOf(d.relocations), d.reservedSize, sliceOf(d.inputSection)));
    std::vector<std::pair<std::uint32_t, std::string>> groups(m.sectionGroupOfInputSection.begin(),
                                                              m.sectionGroupOfInputSection.end());
    std::ranges::sort(groups);
    for (auto const& [section, group] : groups) put(out, "groupOf", std::format("{}={}", section, group));
    std::vector<std::pair<std::string, SectionGroupRule>> rules(m.sectionGroupRuleOf.begin(), m.sectionGroupRuleOf.end());
    std::ranges::sort(rules, {}, &std::pair<std::string, SectionGroupRule>::first);
    for (auto const& [group, rule] : rules)
        put(out, "rule", std::format("{}={},{}", group, static_cast<int>(rule.keep), rule.leader));
    put(out, "linkPosition", m.linkPosition.has_value() ? std::to_string(*m.linkPosition) : std::string{"-"});
    std::vector<std::pair<std::uint32_t, std::uint32_t>> positions;
    for (auto const& [definition, at] : m.linkPositionOfDefinition) positions.emplace_back(definition.v, at);
    std::ranges::sort(positions);
    for (auto const& [definition, at] : positions) put(out, "positionOf", std::format("{}={}", definition, at));
    std::string nulls;
    for (SymbolId const s : m.nullAddressSymbols) nulls += std::format("{},", s.v);
    put(out, "nullAddressSymbols", nulls);
    for (auto const& b : m.imageDefinedSymbols)
        put(out, "imageDefined", std::format("{}={}", b.symbol.v, static_cast<int>(b.address)));
    for (auto const& e : m.staticInitSchedule)
        put(out, "init",
            std::format("{}|{}|{}", e.symbol.v, e.schedule.beforeEntry().value_or(0xFFFFFFFFu),
                        e.schedule.afterEntry().value_or(0xFFFFFFFFu)));
    put(out, "linkerRequests",
        std::format("{},{},{},{},{}", m.linkerRequests.image.size(), m.linkerRequests.exports.size(),
                    m.linkerRequests.hides.size(), m.linkerRequests.handOn.size(), m.linkerRequests.warnings.size()));
    put(out, "imageEntryOverride",
        m.imageEntryOverride.has_value() ? std::to_string(*m.imageEntryOverride) : std::string{"-"});
    put(out, "userEntrySymbol", m.userEntrySymbol.has_value() ? std::to_string(m.userEntrySymbol->v) : std::string{"-"});
    put(out, "symbolIdEnd", std::to_string(m.symbolIdEnd.value()));
    return out;
}

}  // namespace dss::test
