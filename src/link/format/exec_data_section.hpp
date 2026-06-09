#pragma once

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "link/format/byte_emit.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Shared exec-image read-only-data-section substrate for the format
// walkers (ELF `.rodata`, Mach-O `__TEXT,__const`; PE `.rdata` is a
// future fold). Two concerns live here — both extracted VERBATIM from
// elf.cpp's `D-LK1-ELF-EXEC-DATA-SECTIONS` arm so the ELF byte-pins
// stay green and the Mach-O `__const` writer (the exec read-only data-
// section arm, mirroring that same ELF anchor) reuses the SAME
// validated layout + symbolVa logic rather than copy-pasting it:
//
//   1. `buildExecRodata` — validate each `AssembledData` item (must be
//      `DataSectionKind::Rodata`; must carry NO relocations — data→data
//      references are deferred), concatenate the item bytes with per-
//      item alignment padding, record each item's section-relative
//      start offset, and compute the H1 section alignment (max of the
//      schema floor and every item's alignment, so each item's
//      section-relative offset is ALSO its absolute-VA alignment).
//
//   2. `addDataSymbolVas` — add each NAMED data item's absolute VA
//      (`sectionVa + itemOffsets[i]`) to the caller's `symbolVa` map so
//      a `.text` relocation that targets a data SymbolId (the code's
//      `lea reg,[rip+g]` / ADRP+ADD) resolves through the shared
//      `applyExecRelocations` kernel — no rodata loop is added to that
//      kernel; it stays functions-only. Anonymous `SymbolId{}` items
//      (read-only constants / padding) are referenced by section
//      offset, never by symbol, so they are skipped (M1: emplacing two
//      `SymbolId{}` items would otherwise false-fire the duplicate
//      guard).
//
// Both functions are header-only `inline` (like `exec_reloc_apply.hpp`)
// and live in `dss::link::format`. They are FORMAT-NEUTRAL: every
// diagnostic is fronted by the caller's `writerName` so ELF + Mach-O
// share ONE message each (no `if(format)` anywhere). The DiagnosticCode
// values match what elf.cpp historically emitted: non-Rodata reject and
// data→data-reloc reject use `K_NoMatchingObjectFormat`; a duplicate
// NAMED data symbol uses `K_DuplicateDataSymbol`.

namespace dss::link::format {

// Result of laying out the read-only data section from `module.dataItems`.
struct ExecRodataLayout {
    std::vector<std::uint8_t>  bytes;        // concatenated item bytes (padded)
    std::vector<std::uint64_t> itemOffsets;  // per-item start offset into `bytes`
    std::uint64_t              maxAlign = 1; // H1: max(sectionAlignFloor, all item alignments)
};

// Validate + lay out `dataItems` into a single read-only data section.
//
// `sectionAlignFloor` is the section schema row's `addrAlign` (the
// floor); H1 raises `maxAlign` to the strictest item alignment. Returns
// nullopt (after emitting exactly one diagnostic) if any item is not
// `Rodata` or carries its own relocations. An EMPTY `dataItems` yields
// an empty layout (`bytes.empty()`, `maxAlign == max(1, floor)`); the
// caller decides whether to emit a section at all.
[[nodiscard]] inline std::optional<ExecRodataLayout> buildExecRodata(
    std::vector<AssembledData> const& dataItems,
    std::uint64_t                     sectionAlignFloor,
    std::string_view                  writerName,
    DiagnosticReporter&               reporter) {
    using ::dss::link::format::detail::emit;

    ExecRodataLayout layout;
    // The schema row's addrAlign is the FLOOR for the section; H1 below
    // raises it to the strictest member alignment.
    layout.maxAlign = std::max<std::uint64_t>(1, sectionAlignFloor);
    layout.itemOffsets.reserve(dataItems.size());

    for (std::size_t i = 0; i < dataItems.size(); ++i) {
        auto const& d = dataItems[i];
        // Only Rodata closes this cycle. Data/Bss arms remain anchored
        // under D-LK4-RODATA-PRODUCER. The reject runs UNCONDITIONALLY
        // across every item (NOT guarded by a hasRodata bool), so a
        // module carrying ONLY Data/Bss items cannot slip past the gate
        // via a dropped scan.
        if (d.section != DataSectionKind::Rodata) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("{}: AssembledData item #{} has section={} "
                             "not yet supported by the read-only data "
                             "writer — only Rodata closes this cycle; "
                             "Data/Bss arms remain anchored under "
                             "D-LK4-RODATA-PRODUCER.",
                             writerName, i,
                             dataSectionKindName(d.section)));
            return std::nullopt;
        }
        // A data item carrying its OWN relocations (data→data references
        // — a vtable / pointer table) is deferred: this writer patches
        // FUNCTION relocations only. `int g=42;` produces neither.
        if (!d.relocations.empty()) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("{}: rodata data item #{} (SymbolId={{ {} }}) "
                             "carries {} relocation(s); data->data "
                             "relocations are not yet supported "
                             "(deferred D-LK1-ELF-RODATA-DATAITEM-RELOC — "
                             "the writer patches FUNCTION relocations "
                             "only; the Mach-O __const writer shares this "
                             "helper and the same deferral).",
                             writerName, i, d.symbol.v,
                             d.relocations.size()));
            return std::nullopt;
        }
    }

    // Lay out the item bytes with per-item alignment padding; record
    // each item's section-relative start offset (the start of that
    // item's bytes within the concatenated payload).
    for (auto const& d : dataItems) {
        std::uint64_t const aligned = d.alignment.alignUp(
            static_cast<std::uint64_t>(layout.bytes.size()));
        while (layout.bytes.size() < aligned) layout.bytes.push_back(0);
        layout.itemOffsets.push_back(
            static_cast<std::uint64_t>(layout.bytes.size()));
        layout.bytes.insert(layout.bytes.end(),
                            d.bytes.begin(), d.bytes.end());
    }
    // H1 — section alignment must cover the strictest item (gABI:
    // sh_addralign = max of member alignments) so EVERY item's
    // section-relative offset is also its absolute-VA alignment. The
    // schema row's addrAlign is the floor. (Reachable: a 16-aligned
    // i128/u128 rodata global from the producer.) For the single-int
    // corpus max(8,4)=8 → bytes unchanged.
    for (auto const& d : dataItems) {
        layout.maxAlign = std::max<std::uint64_t>(layout.maxAlign,
                                                  d.alignment.bytes());
    }

    return layout;
}

// Add each NAMED data item's absolute VA (`sectionVa + itemOffsets[i]`)
// to `symbolVa`. Anonymous `SymbolId{}` items are skipped (M1). A
// duplicate NAMED symbol (collision with a function symbol or another
// data symbol) is a caller bug (REDEFINITION, not undefined); emits
// `K_DuplicateDataSymbol` and returns false. `itemOffsets` must be the
// vector `buildExecRodata` produced for the SAME `dataItems` (one entry
// per item, in order).
[[nodiscard]] inline bool addDataSymbolVas(
    std::vector<AssembledData> const&            dataItems,
    std::uint64_t                                sectionVa,
    std::vector<std::uint64_t> const&            itemOffsets,
    std::unordered_map<SymbolId, std::uint64_t>& symbolVa,
    std::string_view                             writerName,
    DiagnosticReporter&                          reporter) {
    using ::dss::link::format::detail::emit;
    for (std::size_t i = 0; i < dataItems.size(); ++i) {
        auto const& di = dataItems[i];
        // M1 — anonymous items (the `SymbolId{}` sentinel) are
        // referenced by section offset, NOT by symbol, so they are
        // never reloc targets and must NOT join symbolVa.
        if (di.symbol == SymbolId{}) continue;
        std::uint64_t const va = sectionVa + itemOffsets[i];
        if (!symbolVa.emplace(di.symbol, va).second) {
            emit(reporter, DiagnosticCode::K_DuplicateDataSymbol,
                 std::format("{}: rodata SymbolId={{ {} }} collides with "
                             "another symbol — caller must give each data "
                             "item a unique SymbolId distinct from "
                             "function ids.",
                             writerName, di.symbol.v));
            return false;
        }
    }
    return true;
}

} // namespace dss::link::format
