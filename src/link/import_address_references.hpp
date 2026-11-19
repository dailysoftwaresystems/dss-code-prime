#pragma once

#include "asm/asm.hpp"                     // AssembledModule, AssembledData, Relocation
#include "core/types/alignment.hpp"        // Alignment
#include "core/types/diagnostic_reporter.hpp"   // DiagnosticReporter — the no-slot refusal
#include "core/types/extern_import.hpp"    // ExternImport, ExternKindOrigin
#include "core/types/parse_diagnostic.hpp"      // ParseDiagnostic, DiagnosticCode
#include "core/types/section_kind.hpp"     // DataSectionKind
#include "core/types/target_schema.hpp"    // TargetSchema
#include "core/types/type_lattice/type_layout.hpp"   // scalarByteSize — the format's pointer width
#include "link/fresh_symbol_ids.hpp"       // ModuleSymbolIds
#include "link/object_format_schema.hpp"   // importAddressSymbolPrefix
#include "link/pointer_reloc.hpp"          // absolutePointerRelocKind

#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

// ── A REFERENCE TO AN IMPORT'S ADDRESS SLOT, SPELLED BY NAME ──────────────
// D-LK-PE-DLLIMPORT-OBJECT-REFERENCE-UNRESOLVED (P69).
//
// COFF gives every import TWO names. `X` is the import itself — the function a
// CALL reaches (through the linker's thunk) — and `__imp_X` is the SLOT the
// loader fills with X's address: the IAT entry. Code compiled against a
// `__declspec(dllimport)` declaration names only the second, as
// `call [__imp_X]` and `mov rax, [__imp_X]`; that is every MSVC `/MD` object,
// because the UCRT headers declare the C library dllimport. ✔MEASURED
// 2026-09-30 (cl 19.51 `/c /O2 /MD`, read with dumpbin): the object's symbol
// table holds `__imp_puts` as an UNDEF, type-0 symbol and NO `puts` at all,
// both the call and the address are REL32 against it, and a DSS link of it
// refused `undefined symbol '__imp_puts'` — no dllimport-compiled object could
// be linked by DSS.
//
// The meaning is the format's, so the SPELLING is the format's too:
// `importAddressSymbolPrefix` in its document, never a string in code. Each
// UNBOUND row named `<prefix>X` becomes one of two things:
//   * X is IMPORTED (no linked unit defines it): a row importing `X` whose
//     ADDRESS SLOT (`ExternImport::addressSlotSymbol`) is the old symbol — so
//     every relocation the object wrote against `__imp_X` names X's slot, which
//     the image walker binds to the IAT entry — and whose own symbol is fresh
//     and unreferenced. The kind is left `Pending`: `__imp_X` states nothing
//     about whether X is a function or a datum, so the DEFINITION the binder
//     finds decides it (or the row it folds into at the merge).
//   * X is DEFINED by a linked unit (P69 review M3): what link.exe does with
//     LNK4217 and lld-link does too — a POINTER of the image's own, holding X's
//     address. The old symbol becomes that pointer: a read-only data item of
//     one pointer with one absolute relocation to X (which the image writer
//     base-relocates like any other), and a fresh unbound row for `X` that the
//     cross-unit resolver binds to its definition. `call [__imp_X]` then calls
//     X and `mov rax, [__imp_X]` reads X's address.
// ★ NOT REWRITTEN: a row already bound to a library, or already naming a slot.
namespace dss::linker {

struct ImportAddressFoldCounts {
    std::size_t importedSlots = 0;   // rows made an import's address slot
    std::size_t localPointers = 0;   // rows made a pointer to a linked definition
};

// The local pointer is one pointer of the format's data model, filled through
// the target's address-slot relocation of that width (`absolutePointerRelocKind`).
// Returns what it rewrote, or nullopt with the refusal reported. A local
// definition whose target declares no such relocation is REFUSED by name, the
// target and the width named (P69 fold 3c): it used to be left untouched, and the
// link then refused "`__imp_X` undefined", which named neither — a pointer with
// no fixup would hold zero. A local definition whose format states no pointer
// width is left untouched — never a guessed width: the data model is required
// and closed at load, and the link that follows refuses a format without a width
// by name before it sizes any slot (`linker::link`).
[[nodiscard]] inline std::optional<ImportAddressFoldCounts> foldImportAddressReferences(
        std::span<AssembledModule>             modules,
        ObjectFormatSchema const&              format,
        TargetSchema const&                    target,
        std::unordered_set<std::string> const& definedNames,
        DiagnosticReporter&                    reporter) {
    ImportAddressFoldCounts counts;
    std::string_view const prefix = format.importAddressSymbolPrefix();
    if (prefix.empty()) return counts;
    std::optional<std::uint64_t> const width = scalarByteSize(TypeKind::Ptr, format.dataModel());
    std::optional<RelocationKind> const absPtr =
        width.has_value() ? absolutePointerRelocKind(target, static_cast<std::uint8_t>(*width))
                          : std::nullopt;
    std::string const targetName{target.name()};
    for (auto& mod : modules) {
        // THE PLAN, before the module is touched (P69 fold f26d, the take sites): which `__imp_` rows this module
        // rewrites, and how. Until this fold each row was rewritten as it was met and its id taken there, so a
        // refusal at the second row -- the id space exhausted, or no slot relocation -- left the first rewritten.
        enum class Fold : std::uint8_t { Imported, LocalPointer };
        std::vector<std::pair<std::size_t, Fold>> plan;
        for (std::size_t r = 0; r < mod.externImports.size(); ++r) {
            ExternImport const& ext = mod.externImports[r];
            std::string_view const name{ext.mangledName};
            bool const prefixed = ext.libraryPath.empty() && !ext.addressSlotSymbol.valid()
                               && name.size() > prefix.size()
                               && name.substr(0, prefix.size()) == prefix;
            if (!prefixed) continue;
            std::string const target{name.substr(prefix.size())};
            if (definedNames.count(target) == 0) {
                plan.emplace_back(r, Fold::Imported);
                continue;
            }
            if (!width.has_value()) continue;   // refused by name by the link (above)
            if (!absPtr.has_value()) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::K_AbsolutePointerRelocMissing;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "linker: '{}' names the address slot of '{}', which a linked unit defines, so "
                    "the slot is a pointer of the image's own holding that definition's address "
                    "(link.exe's LNK4217) -- and target '{}' declares no relocation that fills a "
                    "{}-byte address slot (Linear, absolute, neither thread-local nor "
                    "image-relative, of a range that holds every address). A pointer with no "
                    "fixup would hold zero.",
                    name, target, targetName, *width);
                reporter.report(std::move(d));
                return std::nullopt;
            }
            plan.emplace_back(r, Fold::LocalPointer);
        }
        if (plan.empty()) continue;
        // Every rewritten row's id from the module's door, ALL AT ONCE: a space too short for every one of them is
        // refused there by name, and the module is left as it was.
        std::optional<std::vector<SymbolId>> const taken = ModuleSymbolIds{mod}.take(
            plan.size(), reporter, std::format("linker: the {} import-address row(s) of a unit", plan.size()));
        if (!taken.has_value()) return std::nullopt;
        // THE REWRITE, in the module's own row order.
        std::vector<AssembledData> localPointers;
        std::vector<ExternImport>  definitionRows;
        std::vector<bool>          becomesALocalPointer(mod.externImports.size(), false);
        for (std::size_t k = 0; k < plan.size(); ++k) {
            ExternImport& ext = mod.externImports[plan[k].first];
            std::string target = ext.mangledName.substr(prefix.size());
            if (plan[k].second == Fold::Imported) {
                ext.addressSlotSymbol = ext.symbol;
                ext.symbol            = (*taken)[k];
                ext.mangledName       = std::move(target);
                ext.kindOrigin        = ExternKindOrigin::Pending;
                ++counts.importedSlots;
                continue;
            }
            auto const pointerBytes = static_cast<std::uint8_t>(*width);   // engaged: the plan read `absPtr`
            // The local pointer: the old `__imp_X` symbol names it now.
            ExternImport def;
            def.symbol      = (*taken)[k];
            def.mangledName = std::move(target);
            def.binding     = ext.binding;
            def.isData      = ext.isData;
            def.kindOrigin  = ExternKindOrigin::Pending;
            AssembledData slot;
            slot.symbol    = ext.symbol;
            slot.section   = DataSectionKind::RelRoConst;
            slot.bytes.assign(pointerBytes, 0u);
            slot.alignment = Alignment::ofRuntimePow2(pointerBytes);
            Relocation fill;
            fill.offset = 0;
            fill.target = def.symbol;
            fill.kind   = *absPtr;
            fill.addend = 0;
            slot.relocations.push_back(fill);
            localPointers.push_back(std::move(slot));
            definitionRows.push_back(std::move(def));
            becomesALocalPointer[plan[k].first] = true;
            ++counts.localPointers;
        }
        std::vector<ExternImport> kept;
        kept.reserve(mod.externImports.size());
        for (std::size_t r = 0; r < mod.externImports.size(); ++r) {
            if (!becomesALocalPointer[r]) kept.push_back(std::move(mod.externImports[r]));
        }
        for (auto& r : definitionRows) kept.push_back(std::move(r));
        mod.externImports = std::move(kept);
        for (auto& d : localPointers) mod.dataItems.push_back(std::move(d));
    }
    return counts;
}

}  // namespace dss::linker
