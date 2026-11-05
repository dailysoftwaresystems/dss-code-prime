#pragma once

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "link/format/byte_emit.hpp"
#include "link/format/dwarf_cfi.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dss::link::format {

// ★ A CARRIED HANDLER A WRITER CANNOT STATE IS REFUSED, NEVER DROPPED.
//
// `AssembledFunction::languageHandler` is a foreign object's handler: the routine
// an exception dispatcher calls for the function's frame, and the data it reads.
// It comes in TWO SHAPES, one per unwind format:
//   * the Win64 shape -- `data` and its `relocations`, the bytes a COFF object
//     wrote after its UNWIND_INFO. The PE writer states it in the function's
//     UNWIND_INFO; an `.eh_frame` has no place for such bytes.
//   * the DWARF shape -- `dataArea`, a pointer to the area in a data item of the
//     link, or none. The ELF and Mach-O writers state it in `.eh_frame`: the CIE
//     names the routine, the FDE points at the area (P69 fold 3d,
//     `buildEhFrame`). The PE writer refuses it.
// A function described WITHOUT its handler leaves an exception dispatched to its
// frame with no routine to call, and the image looking complete. So each writer
// refuses, by name and before any byte, a handler of the shape it cannot state,
// and every flavour of it that writes no `.eh_frame` refuses a handler of any
// shape.

// The Win64 shape, refused by a writer whose unwind table is `.eh_frame`.
// Returns true when it refused (the diagnostic is reported).
[[nodiscard]] inline bool refuseAWin64HandlerTheEhFrameCannotState(std::span<AssembledFunction const> functions,
                                                                    std::string_view                   writer,
                                                                    DiagnosticReporter&                reporter) {
    for (std::size_t fi = 0; fi < functions.size(); ++fi) {
        auto const& handler = functions[fi].languageHandler;
        if (!handler.has_value() || (handler->data.empty() && handler->relocations.empty())) continue;
        detail::emit(reporter, DiagnosticCode::K_UnwindRuleUnrepresentable,
             std::format("{}: function #{} carries a handler whose data its object wrote after the function's "
                         "Win64 unwind information ({} byte(s), {} relocation(s)); an `.eh_frame` names a "
                         "handler's data only by a pointer to a language-specific data area, so the routine "
                         "would be called without the data it reads.",
                         writer, fi, handler->data.size(), handler->relocations.size()));
        return true;
    }
    return false;
}

// Any handler, refused by a flavour of a writer that writes no `.eh_frame`
// handler: `why` says what the flavour lacks. Returns true when it refused.
[[nodiscard]] inline bool refuseAHandlerThisFlavourCannotState(std::span<AssembledFunction const> functions,
                                                               std::string_view                   writer,
                                                               std::string_view                   why,
                                                               DiagnosticReporter&                reporter) {
    for (std::size_t fi = 0; fi < functions.size(); ++fi) {
        if (!functions[fi].languageHandler.has_value()) continue;
        detail::emit(reporter, DiagnosticCode::K_UnwindRuleUnrepresentable,
             std::format("{}: function #{} carries the handler routine its object named for it, and {}; "
                         "describing the function without its handler would leave an exception dispatched "
                         "to its frame with no routine to call.",
                         writer, fi, why));
        return true;
    }
    return false;
}

// Each function's handler as `buildEhFrame` states it -- the routine keyed by its
// symbol, so the FDEs naming one routine share a CIE -- or empty when no function
// carries one. Call it after `refuseAWin64HandlerTheEhFrameCannotState`.
[[nodiscard]] inline std::vector<std::optional<EhFrameHandler>>
ehFrameHandlersOf(std::span<AssembledFunction const> functions) {
    std::vector<std::optional<EhFrameHandler>> out;
    bool any = false;
    for (auto const& fn : functions) any = any || fn.languageHandler.has_value();
    if (!any) return out;
    out.reserve(functions.size());
    for (auto const& fn : functions) {
        if (!fn.languageHandler.has_value()) {
            out.emplace_back();
            continue;
        }
        out.emplace_back(EhFrameHandler{fn.languageHandler->personality.v, fn.languageHandler->dataArea.has_value()});
    }
    return out;
}

// Fill the handler fields of an IMAGE's `.eh_frame` (pc-relative, direct): a
// personality field names its routine's address in the image -- a function of
// the image, or an import's call stub -- and a data-area field the area's
// first byte. `symbolVa` is the writer's one map of addresses; `threadLocal`
// the symbols whose entry in it is no address. Refused by name, before the
// field is written: a routine or area with no address in the image, a routine
// the link binds as a DATA import (its entry is the slot holding the address,
// and the dispatcher would call the slot), either one thread-local, an area
// lying outside its item, and a distance a 32-bit field cannot hold.
// Returns false after reporting.
[[nodiscard]] inline bool fillEhFrameHandlerFields(EhFrameSection& sec, std::uint64_t ehFrameVa,
                                                   AssembledModule const&                             module,
                                                   std::unordered_map<SymbolId, std::uint64_t> const& symbolVa,
                                                   std::unordered_set<SymbolId> const&                threadLocal,
                                                   std::string_view writer, DiagnosticReporter& reporter) {
    if (sec.handlerPatches.empty()) return true;
    auto const nameOf = [&](SymbolId id) -> std::string {
        for (auto const& row : module.symbols) {
            if (row.symbol == id && !row.name.empty()) return "'" + row.name + "'";
        }
        for (auto const& ex : module.externImports) {
            if (ex.symbol == id) return "'" + ex.mangledName + "'";
        }
        return "symbol #" + std::to_string(id.v);
    };
    auto const refuse = [&](std::size_t fi, std::string const& what) {
        detail::emit(reporter, DiagnosticCode::K_UnwindRuleUnrepresentable,
                     std::format("{}: function #{}'s handler {}; its `.eh_frame` field would name no routine "
                                 "or data the dispatcher could use.",
                                 writer, fi, what));
        return false;
    };
    // Each field's address, resolved and checked before any byte is written.
    std::unordered_map<std::uint32_t, std::uint64_t> fieldVa;   // byteOffset -> address
    for (auto const& p : sec.handlerPatches) {
        if (p.funcIndex >= module.functions.size() || !module.functions[p.funcIndex].languageHandler.has_value()) {
            return refuse(p.funcIndex, "field was recorded for a function that carries no handler");
        }
        LanguageHandler const& h = *module.functions[p.funcIndex].languageHandler;
        if (p.field == EhFrameHandlerPatch::Field::Personality) {
            for (auto const& ex : module.externImports) {
                if (ex.symbol == h.personality && ex.isData) {
                    return refuse(p.funcIndex, "names routine " + nameOf(h.personality)
                                                   + ", which this link binds as a data import");
                }
            }
            auto const it = symbolVa.find(h.personality);
            if (it == symbolVa.end() || it->second == 0u) {
                return refuse(p.funcIndex, "names routine " + nameOf(h.personality)
                                               + ", which has no address in this image");
            }
            if (threadLocal.contains(h.personality)) {
                return refuse(p.funcIndex, "names routine " + nameOf(h.personality) + ", a thread-local symbol");
            }
            fieldVa.emplace(p.byteOffset, it->second);
            continue;
        }
        if (!h.dataArea.has_value()) {
            return refuse(p.funcIndex, "has a data-area field and states no data area");
        }
        DataAreaRef const& area = *h.dataArea;
        AssembledData const* item = nullptr;
        for (auto const& di : module.dataItems) {
            if (di.symbol == area.item) {
                item = &di;
                break;
            }
        }
        auto const it = symbolVa.find(area.item);
        if (item == nullptr || it == symbolVa.end() || it->second == 0u) {
            return refuse(p.funcIndex, "names a data area in " + nameOf(area.item)
                                           + ", which is no data item with an address in this image");
        }
        if (threadLocal.contains(area.item)) {
            return refuse(p.funcIndex, "names a data area in " + nameOf(area.item) + ", a thread-local item");
        }
        if (area.offset < 0 || static_cast<std::uint64_t>(area.offset) >= item->bytes.size()) {
            return refuse(p.funcIndex, std::format("names a data area at offset {} of {}, whose {} file byte(s) "
                                                   "do not hold it",
                                                   area.offset, nameOf(area.item), item->bytes.size()));
        }
        fieldVa.emplace(p.byteOffset, it->second + static_cast<std::uint64_t>(area.offset));
    }
    std::string const err = applyEhFrameHandlerFields(
        sec, ehFrameVa, [&](EhFrameHandlerPatch const& p) { return fieldVa.at(p.byteOffset); });
    if (!err.empty()) {
        detail::emit(reporter, DiagnosticCode::K_UnwindRuleUnrepresentable, std::format("{}: {}", writer, err));
        return false;
    }
    return true;
}

} // namespace dss::link::format
