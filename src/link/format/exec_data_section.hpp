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

// Shared exec-image DATA-SECTION substrate for the format walkers — the
// kind-parameterized layout + symbolVa logic ELF / PE / Mach-O all reuse so a
// section's byte layout lives in ONE place, not copy-pasted per writer. Closes
// D-LK4-DATA-PRODUCER (writable `.data` + zero-fill `.bss`) atop the original
// `D-LK1-ELF-EXEC-DATA-SECTIONS` rodata arm. Two concerns live here:
//
//   1. `buildExecDataSection(kind, …)` — select the `AssembledData` items whose
//      `section == kind`, validate them (no data→data relocations; a Bss item
//      carries no file bytes), lay them out with per-item alignment padding,
//      record each matching item's section-relative start offset + its original
//      `dataItems` index, the section's span size, and the H1 section alignment
//      (max of the schema floor and every matching item's alignment, so each
//      item's section-relative offset is ALSO its absolute-VA alignment). For a
//      `Bss` section the file `bytes` stay EMPTY and `spanSize` is the zero-fill
//      memory extent (the section header reserves the size; the file does not
//      store it).
//
//   2. `addDataSymbolVas` — add each NAMED matching item's absolute VA
//      (`sectionVa + itemOffsets[j]`) to the caller's `symbolVa` map so a
//      `.text` relocation that targets a data SymbolId (the code's
//      `lea reg,[rip+g]` / ADRP+ADD) resolves through the shared
//      `applyExecRelocations` kernel — no data loop is added to that kernel; it
//      stays functions-only. Anonymous `SymbolId{}` items (read-only constants /
//      padding) are referenced by section offset, never by symbol, so they are
//      skipped (M1: emplacing two `SymbolId{}` items would otherwise false-fire
//      the duplicate guard).
//
// Both functions are header-only `inline` (like `exec_reloc_apply.hpp`) and live
// in `dss::link::format`. They are FORMAT-NEUTRAL: every diagnostic is fronted by
// the caller's `writerName` so the writers share ONE message each (no `if(format)`
// anywhere; the writers read section flags + names from the schema row, never
// hardcoding them). DiagnosticCode values: data→data-reloc reject →
// `K_NoMatchingObjectFormat`; Bss-with-bytes → `K_BssDataHasBytes`; a duplicate
// NAMED data symbol → `K_DuplicateDataSymbol`.

namespace dss::link::format {

// Result of laying out ONE data section (a single `DataSectionKind`) from the
// matching subset of `module.dataItems`.
//
// `bytes` carries the concatenated, per-item-aligned FILE payload for a
// file-backed section (`Rodata`/`Data`). For a `Bss` (zero-fill) section it is
// EMPTY by construction — Bss reserves memory without storing file bytes — and
// `spanSize` (not `bytes.size()`) is the section's in-memory extent. `itemOffsets`
// and `itemIndices` are PARALLEL (one entry per MATCHING item): `itemOffsets[j]`
// is item `itemIndices[j]`'s section-relative start, so symbolVa registration
// maps each offset back to its original `dataItems` index.
struct ExecDataSectionLayout {
    std::vector<std::uint8_t>  bytes;        // file bytes (EMPTY for Bss)
    std::vector<std::uint64_t> itemOffsets;  // per-matching-item section-relative start
    std::vector<std::size_t>   itemIndices;  // original index in dataItems, parallel
    std::uint64_t              spanSize = 0; // section extent (== bytes.size() for
                                             // file-backed; the zero-fill size for Bss)
    std::uint64_t              maxAlign = 1; // H1: max(floor, all matching item aligns)
    [[nodiscard]] bool empty() const noexcept { return itemIndices.empty(); }
};

// Validate + lay out the `dataItems` whose `section == kind` into ONE section.
//
// Items whose section differs from `kind` are SKIPPED (they belong to another
// section the caller lays out with its own call) — the per-format linker gate
// (`acceptsDataSection`) already rejected any kind the format does not advertise,
// and the three `DataSectionKind` values are each handled by exactly one call, so
// no item is ever silently dropped. `sectionAlignFloor` is the schema row's
// `addrAlign`; H1 raises `maxAlign` to the strictest matching-item alignment.
// Returns nullopt (after emitting exactly one diagnostic) if a matching item
// carries its own relocations (data→data references — deferred) or if a Bss item
// carries file bytes (substrate-shape violation — caught earlier by
// `validateAssembledData`, re-checked here in depth). An EMPTY matching set
// yields an empty layout (`empty()==true`, `maxAlign==max(1,floor)`); the caller
// decides whether to emit the section at all. D-LK4-DATA-PRODUCER.
[[nodiscard]] inline std::optional<ExecDataSectionLayout> buildExecDataSection(
    std::vector<AssembledData> const& dataItems,
    DataSectionKind                   kind,
    std::uint64_t                     sectionAlignFloor,
    std::string_view                  writerName,
    DiagnosticReporter&               reporter,
    // PE patches data-item (data→data) relocations into the laid-out bytes
    // AFTER this call (its cross-CU thunk-slot feature), so it passes `true`.
    // ELF / Mach-O do NOT yet patch data-item relocations, so they leave this
    // `false` and a reloc-bearing item fails loud here (no silent unpatched
    // slot). Deferral anchor: D-LK1-ELF-RODATA-DATAITEM-RELOC.
    bool                              allowItemRelocations = false) {
    using ::dss::link::format::detail::emit;

    ExecDataSectionLayout layout;
    // The schema row's addrAlign is the FLOOR; H1 below raises it to the
    // strictest matching member alignment.
    layout.maxAlign = std::max<std::uint64_t>(1, sectionAlignFloor);

    bool const isBss = (kind == DataSectionKind::Bss);

    for (std::size_t i = 0; i < dataItems.size(); ++i) {
        auto const& d = dataItems[i];
        if (d.section != kind) continue;     // belongs to another section
        // A data item carrying its OWN relocations (data->data references —
        // a vtable / cross-CU thunk slot). The writers that don't patch them
        // (ELF / Mach-O) reject loud; PE (`allowItemRelocations`) patches them
        // post-layout using the offsets this function records.
        if (!d.relocations.empty() && !allowItemRelocations) {
            emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("{}: {} data item #{} (SymbolId={{ {} }}) carries "
                             "{} relocation(s); data->data relocations are not "
                             "yet supported by this writer (deferred D-LK1-ELF-"
                             "RODATA-DATAITEM-RELOC — it patches FUNCTION "
                             "relocations only).",
                             writerName, dataSectionKindName(kind), i,
                             d.symbol.v, d.relocations.size()));
            return std::nullopt;
        }
        // Defense in depth (validateAssembledData ran first): a Bss item must
        // carry NO file bytes — it is zero-fill, so its span comes from
        // reservedSize, never from stored bytes (sizeInSection() reads
        // reservedSize for Bss and bytes.size() otherwise).
        if (isBss && !d.bytes.empty()) {
            emit(reporter, DiagnosticCode::K_BssDataHasBytes,
                 std::format("{}: Bss data item #{} (SymbolId={{ {} }}) carries "
                             "{} file byte(s) — Bss is zero-fill and must reserve "
                             "size without storing bytes.",
                             writerName, i, d.symbol.v, d.bytes.size()));
            return std::nullopt;
        }

        std::uint64_t const itemSize = d.sizeInSection();
        // Lay each item at its alignment within the section span; record the
        // section-relative start + the original index (for symbolVa). For Bss
        // the span is virtual (no bytes appended) but offsets advance the same.
        std::uint64_t const aligned = d.alignment.alignUp(layout.spanSize);
        if (!isBss) {
            while (layout.bytes.size() < aligned) layout.bytes.push_back(0);
        }
        layout.spanSize = aligned;
        layout.itemOffsets.push_back(layout.spanSize);
        layout.itemIndices.push_back(i);
        if (!isBss) {
            layout.bytes.insert(layout.bytes.end(),
                                d.bytes.begin(), d.bytes.end());
        }
        layout.spanSize += itemSize;
        // H1 — section alignment covers the strictest matching item (gABI:
        // sh_addralign = max of member alignments) so each item's section-
        // relative offset is also its absolute-VA alignment.
        layout.maxAlign = std::max<std::uint64_t>(layout.maxAlign,
                                                  d.alignment.bytes());
    }
    // Invariant: a file-backed section's byte count equals its span size.
    return layout;
}

// Add each NAMED matching data item's absolute VA (`sectionVa + itemOffsets[j]`)
// to `symbolVa`. `layout` MUST be the one `buildExecDataSection` produced for the
// SAME `dataItems` + kind (its `itemOffsets`/`itemIndices` are parallel). Works
// uniformly for Rodata/Data/Bss — a Bss global is reloc-addressable by VA just
// like a file-backed one. Anonymous `SymbolId{}` items are skipped (M1). A
// duplicate NAMED symbol (collision with a function symbol or another data
// symbol) is a caller bug (REDEFINITION); emits `K_DuplicateDataSymbol` and
// returns false.
[[nodiscard]] inline bool addDataSymbolVas(
    std::vector<AssembledData> const&            dataItems,
    ExecDataSectionLayout const&                 layout,
    std::uint64_t                                sectionVa,
    std::unordered_map<SymbolId, std::uint64_t>& symbolVa,
    std::string_view                             writerName,
    DiagnosticReporter&                          reporter) {
    using ::dss::link::format::detail::emit;
    for (std::size_t j = 0; j < layout.itemIndices.size(); ++j) {
        auto const& di = dataItems[layout.itemIndices[j]];
        // M1 — anonymous items (the `SymbolId{}` sentinel) are referenced by
        // section offset, NOT by symbol, so they are never reloc targets and
        // must NOT join symbolVa.
        if (di.symbol == SymbolId{}) continue;
        std::uint64_t const va = sectionVa + layout.itemOffsets[j];
        if (!symbolVa.emplace(di.symbol, va).second) {
            emit(reporter, DiagnosticCode::K_DuplicateDataSymbol,
                 std::format("{}: data SymbolId={{ {} }} collides with another "
                             "symbol — caller must give each data item a unique "
                             "SymbolId distinct from function ids.",
                             writerName, di.symbol.v));
            return false;
        }
    }
    return true;
}

} // namespace dss::link::format
