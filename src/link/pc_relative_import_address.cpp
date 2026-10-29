#include "link/pc_relative_import_address.hpp"

#include "core/types/extern_import.hpp"      // ExternKindOrigin
#include "core/types/parse_diagnostic.hpp"   // DiagnosticCode
#include "link/branch_sites.hpp"             // BranchSites, isBranchReference
#include "link/fresh_symbol_ids.hpp"         // maxExistingSymbolIdV

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss::linker {

namespace {

[[nodiscard]] std::string nameOf(AssembledModule const& unit, SymbolId symbol) {
    for (auto const& ms : unit.symbols) {
        if (ms.symbol == symbol && !ms.name.empty()) return ms.name;
    }
    return "#" + std::to_string(symbol.v);
}

// The first PC-relative, non-branch site in a unit's code reaching an import.
struct Site {
    SymbolId      function{};
    std::uint32_t offset = 0;
};

}  // namespace

bool bindPcRelativeImportAddressUnits(std::span<AssembledModule const> units,
                                      std::vector<AssembledModule>&    out,
                                      TargetSchema const&              target,
                                      ObjectFormatSchema const&        format,
                                      DiagnosticReporter&              reporter) {
    auto const meaning = format.pcRelativeImportAddress();
    if (!meaning.has_value()) return true;

    // Names a linked unit DEFINES with external linkage: a reference to one of
    // them reaches that definition, never an import.
    std::unordered_set<std::string> defined;
    for (auto const& u : units) {
        std::unordered_set<std::uint32_t> definedHere;
        for (auto const& fn : u.functions) definedHere.insert(fn.symbol.v);
        for (auto const& d : u.dataItems) definedHere.insert(d.symbol.v);
        for (auto const& ms : u.symbols) {
            if (ms.binding == SymbolBinding::Local || ms.name.empty()) continue;
            if (definedHere.contains(ms.symbol.v)) defined.insert(ms.name);
        }
    }

    BranchSites const sites{target};
    // Per unit: import row index -> the first site that takes its address.
    std::vector<std::map<std::size_t, Site>> found(units.size());
    bool any = false;
    for (std::size_t ui = 0; ui < units.size(); ++ui) {
        auto const& u = units[ui];
        std::unordered_map<std::uint32_t, std::size_t> importRow;
        for (std::size_t i = 0; i < u.externImports.size(); ++i) {
            auto const& ext = u.externImports[i];
            if (ext.libraryPath.empty() || defined.contains(ext.mangledName)) continue;
            // A FUNCTION's question only. A datum has no call entry — a direct
            // code reference to one is the copy-relocation case — and a row
            // whose kind is still pending states none: both belong to the image
            // link's import-reference judgment, which runs after this. A row
            // MIR→LIR reads through a slot (`readThroughSlot`: its `lea` names
            // the slot the writer binds at the import's own symbol, and the
            // address is LOADED from there) reaches that slot, not a stub.
            if (ext.isData || ext.kindOrigin == ExternKindOrigin::Pending
                || ext.readThroughSlot) {
                continue;
            }
            importRow.emplace(ext.symbol.v, i);
        }
        if (importRow.empty()) continue;
        for (auto const& fn : u.functions) {
            for (auto const& rel : fn.relocations) {
                auto const row = importRow.find(rel.target.v);
                if (row == importRow.end()) continue;
                auto const* tri = target.relocationInfo(rel.kind);
                // From the PLACE: a `Linear` row by its own flag, an
                // instruction-word formula (arm64 `adrp`, `adr`) by the
                // formula's — `adrp`/`add` to a dylib function is the shape
                // Apple's ld refuses (`kind=arm64_adrp_lo12`).
                if (tri == nullptr || tri->tls
                    || !relocReadsThePlace(tri->formulaKind, tri->pcRelative)) {
                    continue;
                }
                if (relocFormulaFacts(tri->formulaKind).isGotSlotRelative) continue;
                if (isBranchReference(rel, fn.bytes, target, format, sites)) continue;
                found[ui].try_emplace(row->second, Site{fn.symbol, rel.offset});
                any = true;
            }
        }
    }
    if (!any) return true;

    if (*meaning == PcRelativeImportAddress::Refused) {
        for (std::size_t ui = 0; ui < units.size(); ++ui) {
            for (auto const& [row, site] : found[ui]) {
                ExternImport const& ext = units[ui].externImports[row];
                report(reporter, DiagnosticCode::K_ImportReferenceUnbindable,
                       DiagnosticSeverity::Error,
                       std::format(
                           "linker: the code of '{}' (CU #{}, offset {}) reaches function '{}' "
                           "of library '{}' by a PC-relative displacement that is not a "
                           "branch. A displacement reaches nothing outside this image, so "
                           "in an image of format '{}' it can only compute this image's "
                           "call entry for '{}', which is not its address, and this "
                           "format's linkers refuse the reference. Build the unit "
                           "position-independently (-fPIC), or take the address through "
                           "the GOT.",
                           nameOf(units[ui], site.function), units[ui].cuId.v, site.offset,
                           ext.mangledName, ext.libraryPath, format.name(), ext.mangledName));
            }
        }
        return false;
    }

    // `callEntry`: the unit's address of each such FUNCTION import is its call
    // entry, everywhere in the unit.
    out.assign(units.begin(), units.end());
    for (std::size_t ui = 0; ui < units.size(); ++ui) {
        if (found[ui].empty()) continue;
        AssembledModule& u = out[ui];
        std::uint32_t maxV = maxExistingSymbolIdV(u);
        auto const mint = [&]() -> std::optional<SymbolId> {
            if (maxV == std::numeric_limits<std::uint32_t>::max()) {
                report(reporter, DiagnosticCode::K_SymbolUndefined, DiagnosticSeverity::Error,
                       "linker: SymbolId space exhausted binding a unit to an import's "
                       "call entry.");
                return std::nullopt;
            }
            return SymbolId{++maxV};
        };
        std::unordered_map<std::uint32_t, SymbolId> entryOf;   // import symbol -> its call entry
        for (auto const& [row, site] : found[ui]) {
            ExternImport& ext = u.externImports[row];   // a function: rows are chosen above
            if (!ext.callEntrySymbol.valid()) {
                auto const s = mint();
                if (!s.has_value()) return false;
                ext.callEntrySymbol = *s;
            }
            entryOf.emplace(ext.symbol.v, ext.callEntrySymbol);
        }
        if (entryOf.empty()) continue;
        auto const holdsAnEntry = [&](AssembledData const& d) {
            for (auto const& rel : d.relocations) {
                if (entryOf.contains(rel.target.v)) return true;
            }
            return false;
        };
        auto const retarget = [&](std::vector<Relocation>& rels) {
            for (auto& rel : rels) {
                if (auto const e = entryOf.find(rel.target.v); e != entryOf.end()) {
                    rel.target = e->second;
                }
            }
        };
        std::unordered_set<std::uint32_t> weak;
        for (auto const& ms : u.symbols) {
            if (ms.binding == SymbolBinding::Weak) weak.insert(ms.symbol.v);
        }
        std::unordered_map<std::uint32_t, SymbolId> cloneOf;
        std::vector<AssembledData> clones;
        for (auto& d : u.dataItems) {
            if (!holdsAnEntry(d)) continue;
            if (!weak.contains(d.symbol.v)) {
                retarget(d.relocations);
                continue;
            }
            // A coalescable item (a `.refptr.<name>` COMDAT): other units may be
            // bound to whichever copy wins, so this unit reads a private clone.
            auto const s = mint();
            if (!s.has_value()) return false;
            AssembledData clone = d;
            clone.symbol = *s;
            clone.inputSection.reset();   // its own placement: no longer a slice of the section
            retarget(clone.relocations);
            cloneOf.emplace(d.symbol.v, clone.symbol);
            clones.push_back(std::move(clone));
        }
        for (auto& c : clones) u.dataItems.push_back(std::move(c));
        // The unit's references to a cloned item name its clone; its GOT loads
        // of the import name the call entry (the link mints a slot holding it).
        for (auto& fn : u.functions) {
            for (auto& rel : fn.relocations) {
                if (auto const c = cloneOf.find(rel.target.v); c != cloneOf.end()) {
                    rel.target = c->second;
                    continue;
                }
                auto const e = entryOf.find(rel.target.v);
                if (e == entryOf.end()) continue;
                auto const* tri = target.relocationInfo(rel.kind);
                if (tri != nullptr && relocFormulaFacts(tri->formulaKind).isGotSlotRelative) {
                    rel.target = e->second;
                }
            }
        }
        if (!cloneOf.empty()) {
            for (auto& d : u.dataItems) {
                for (auto& rel : d.relocations) {
                    if (auto const c = cloneOf.find(rel.target.v); c != cloneOf.end()) {
                        rel.target = c->second;
                    }
                }
            }
        }
    }
    return false;
}

}  // namespace dss::linker
