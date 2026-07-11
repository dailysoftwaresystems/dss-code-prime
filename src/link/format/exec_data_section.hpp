#pragma once

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"   // F5: TargetSchema::relocationInfo (abs64)
#include "link/format/byte_emit.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

    // TLS C1 audit fold M-3 (D-CSUBSET-THREAD-LOCAL): the zero-fill
    // discrimination routes through the ONE shared `isZeroFill` predicate
    // (section_kind.hpp) — Bss AND Tbss reserve extent without file bytes.
    // The former exact `== DataSectionKind::Bss` test would have silently
    // laid a Tbss item out as file-backed (0 bytes where reservedSize was
    // the real per-thread span).
    bool const zeroFill = isZeroFill(kind);

    for (std::size_t i = 0; i < dataItems.size(); ++i) {
        auto const& d = dataItems[i];
        if (d.section != kind) continue;     // belongs to another section
        // A data item carrying its OWN relocations (data->data references —
        // a vtable / cross-CU thunk slot). The writers that don't patch them
        // (Mach-O) reject loud; PE + ELF-dynamic (`allowItemRelocations`)
        // patch them post-layout using the offsets this function records.
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
        // Defense in depth (validateAssembledData ran first): a zero-fill
        // (Bss/Tbss) item must carry NO file bytes — its span comes from
        // reservedSize, never from stored bytes (sizeInSection() reads
        // reservedSize for zero-fill kinds and bytes.size() otherwise).
        if (zeroFill && !d.bytes.empty()) {
            emit(reporter, DiagnosticCode::K_BssDataHasBytes,
                 std::format("{}: {} data item #{} (SymbolId={{ {} }}) carries "
                             "{} file byte(s) — a zero-fill section reserves "
                             "size without storing bytes.",
                             writerName, dataSectionKindName(kind), i,
                             d.symbol.v, d.bytes.size()));
            return std::nullopt;
        }

        std::uint64_t const itemSize = d.sizeInSection();
        // Lay each item at its alignment within the section span; record the
        // section-relative start + the original index (for symbolVa). For a
        // zero-fill kind the span is virtual (no bytes appended) but offsets
        // advance the same.
        std::uint64_t const aligned = d.alignment.alignUp(layout.spanSize);
        if (!zeroFill) {
            while (layout.bytes.size() < aligned) layout.bytes.push_back(0);
        }
        layout.spanSize = aligned;
        layout.itemOffsets.push_back(layout.spanSize);
        layout.itemIndices.push_back(i);
        if (!zeroFill) {
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

// D-CSUBSET-THREAD-LOCAL (TLS C1): register each NAMED thread-local data
// item's THREAD-POINTER OFFSET (tpoff) into the caller's `symbolVa` map, and
// its SymbolId into `tlsSymbols`.
//
// THE SYMBOLVA-REUSE TRICK (arc discovery D3): the shared
// `applyExecRelocations` kernel's `Linear` formula reads `S` from `symbolVa`
// and computes `int64(S) + A (+bias)` with a signed-fit check before a
// width-truncating LE write (exec_reloc_apply.hpp) — so storing the SIGNED
// tpoff BIT-CAST to u64 here makes a `tls-tpoff32` relocation patch the
// correct (possibly negative) 32-bit offset with ZERO kernel changes. The
// map's value is therefore NOT a VA for these symbols — which is exactly why
// the caller MUST (a) keep TLS items OUT of `addDataSymbolVas` (one entry per
// symbol, the tpoff) and (b) run the CRIT-1 cross-check (a non-tls
// relocation against a `tlsSymbols` member would embed the bit-cast tpoff as
// an address; a tls relocation against a non-member would embed a VA as a
// tpoff — both silent-garbage classes).
//
// The tpoff FORMULA is keyed on the TARGET's declared `TlsVariant` (config,
// never a machine-identity branch) — audit fold HIGH-1's exact physics,
// gcc-witnessed:
//   * Variant II (x86_64): tp points ONE PAST the aligned block end →
//         tpoff = int64(templateOffset) − int64(alignedBlockSize)
//     where alignedBlockSize = alignUp(block memsz, p_align)  (NEGATIVE).
//   * Variant I (arm64): the block follows the TCB header, itself rounded
//     up to the block alignment →
//         tpoff = int64(alignUp(tcbHeaderBytes, tlsAlignment))
//               + int64(templateOffset)                        (POSITIVE).
// Both arms land NOW, config-keyed — TLS C2 only supplies arm64's `tls`
// identity block, zero code here.
//
// `templateOffset` = `blockBaseOffset + layout.itemOffsets[j]`:
// `blockBaseOffset` is 0 for the .tdata call and the tbss block base
// (`alignUp(tdataSpan, tbss maxAlign)`) for the .tbss call, so both
// sections' items index ONE contiguous per-thread block. Anonymous
// `SymbolId{}` items are skipped (M1 mirror); a duplicate NAMED symbol
// fails loud (`K_DuplicateDataSymbol`), mirroring `addDataSymbolVas`.
[[nodiscard]] inline bool addTlsSymbolOffsets(
    std::vector<AssembledData> const&            dataItems,
    ExecDataSectionLayout const&                 layout,
    std::uint64_t                                blockBaseOffset,
    std::uint64_t                                alignedBlockSize,
    std::uint64_t                                tlsAlignment,
    TlsIdentity const&                           tlsIdentity,
    std::unordered_map<SymbolId, std::uint64_t>& symbolVa,
    std::unordered_set<SymbolId>&                tlsSymbols,
    std::string_view                             writerName,
    DiagnosticReporter&                          reporter) {
    using ::dss::link::format::detail::emit;
    auto const alignUp64 = [](std::uint64_t v, std::uint64_t a) noexcept {
        return a <= 1 ? v : ((v + a - 1) / a) * a;
    };
    for (std::size_t j = 0; j < layout.itemIndices.size(); ++j) {
        auto const& di = dataItems[layout.itemIndices[j]];
        // M1 mirror — anonymous items are offset-referenced, never reloc
        // targets, and must NOT join symbolVa / tlsSymbols.
        if (di.symbol == SymbolId{}) continue;
        std::uint64_t const templateOffset =
            blockBaseOffset + layout.itemOffsets[j];
        std::int64_t tpoff = 0;
        switch (tlsIdentity.variant) {
            case TlsVariant::Variant2:
                tpoff = static_cast<std::int64_t>(templateOffset)
                      - static_cast<std::int64_t>(alignedBlockSize);
                break;
            case TlsVariant::Variant1:
                tpoff = static_cast<std::int64_t>(
                            alignUp64(tlsIdentity.tcbHeaderBytes,
                                      tlsAlignment))
                      + static_cast<std::int64_t>(templateOffset);
                break;
        }
        if (!symbolVa.emplace(di.symbol,
                              static_cast<std::uint64_t>(tpoff)).second) {
            emit(reporter, DiagnosticCode::K_DuplicateDataSymbol,
                 std::format("{}: thread-local data SymbolId={{ {} }} "
                             "collides with another symbol — caller must "
                             "give each data item a unique SymbolId "
                             "distinct from function ids "
                             "(D-CSUBSET-THREAD-LOCAL).",
                             writerName, di.symbol.v));
            return false;
        }
        tlsSymbols.insert(di.symbol);
    }
    return true;
}

// D-CSUBSET-THREAD-LOCAL (TLS C3, mechanism 6): register each NAMED
// thread-local data item's POSITIVE TEMPLATE OFFSET (the section-relative
// byte offset within the per-thread block) into `symbolVa`, and its
// SymbolId into `tlsSymbols`. This is the PE (`pe-indexed`) analog of
// `addTlsSymbolOffsets` above — but DELIBERATELY DISTINCT: PE's access
// sequence computes `blockBase + secrel(sym)` where `blockBase` is looked
// up at RUNTIME from the module's TLS array, so `sym`'s stored value is its
// FORWARD offset in the template (Start-relative), NOT the ELF NEGATIVE
// Variant-II tpoff (which is relative to a tp register that points PAST the
// block). The shared `applyExecRelocations` `Linear` kernel then patches the
// `lea`'s disp32 = `int64(templateOffset) + addend` — an ASLR-invariant
// link-time constant (no base relocation: it is a RELATIVE displacement, not
// an address). `templateOffset = blockBaseOffset + itemOffsets[j]`:
// `blockBaseOffset` is 0 for the `.tls` (tdata template) call and the tbss
// block base (`alignUp(tdataSpan, tbssAlign)`) for the tbss call, so both
// index ONE contiguous per-thread block (a tbss var's offset lands in the
// SizeOfZeroFill tail). Anonymous `SymbolId{}` items are skipped (M1 mirror);
// a duplicate NAMED symbol fails loud (`K_DuplicateDataSymbol`). The CRIT-1
// cross-check the caller runs is IDENTICAL (a tls-flagged reloc must target a
// `tlsSymbols` member and vice versa) — the value class stored here is a
// section-relative offset, still not a VA, so a non-tls reloc against a
// member would embed it as an address.
[[nodiscard]] inline bool addTlsTemplateOffsets(
    std::vector<AssembledData> const&            dataItems,
    ExecDataSectionLayout const&                 layout,
    std::uint64_t                                blockBaseOffset,
    std::unordered_map<SymbolId, std::uint64_t>& symbolVa,
    std::unordered_set<SymbolId>&                tlsSymbols,
    std::string_view                             writerName,
    DiagnosticReporter&                          reporter) {
    using ::dss::link::format::detail::emit;
    for (std::size_t j = 0; j < layout.itemIndices.size(); ++j) {
        auto const& di = dataItems[layout.itemIndices[j]];
        if (di.symbol == SymbolId{}) continue;   // M1 mirror — anonymous
        std::uint64_t const templateOffset =
            blockBaseOffset + layout.itemOffsets[j];
        if (!symbolVa.emplace(di.symbol, templateOffset).second) {
            emit(reporter, DiagnosticCode::K_DuplicateDataSymbol,
                 std::format("{}: thread-local data SymbolId={{ {} }} "
                             "collides with another symbol — caller must "
                             "give each data item a unique SymbolId "
                             "distinct from function ids "
                             "(D-CSUBSET-THREAD-LOCAL).",
                             writerName, di.symbol.v));
            return false;
        }
        tlsSymbols.insert(di.symbol);
    }
    return true;
}

// F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): patch each reloc-bearing data item's bytes
// IN PLACE with its target symbol's absolute VA. Shared by the exec writers
// (PE/ELF/Mach-O): an exec image has resolved VAs, so an abs64 data fixup is
// written directly into the section bytes (no `.rela` section for an exec image; a
// PIE image carries the loader-applied slide). `bytesOut` is the section's MUTABLE
// byte buffer (the caller emits it); `layout` is the one buildExecDataSection
// produced for `dataItems` + this section's kind (itemOffsets/itemIndices
// parallel); `sectionVa` is the section base (a VA — for PE an RVA). Only ABSOLUTE
// relocs are valid in data (a pc-relative kind is a producer bug). `siteVasOut`
// (optional): each 8-byte-absolute fixup's `sectionVa + patchOffset` is appended —
// PE passes it for the `.reloc` DIR64 base-relocation table; ELF / Mach-O pass
// nullptr. Returns false + emits on any error (unresolved target, pc-relative /
// zero-width kind, write overrun, or — when collecting sites — a non-8-byte
// absolute reloc with no base-reloc representation).
[[nodiscard]] inline bool applyDataItemRelocations(
    std::vector<std::uint8_t>&                          bytesOut,
    std::vector<AssembledData> const&                   dataItems,
    ExecDataSectionLayout const&                        layout,
    std::uint64_t                                       sectionVa,
    std::unordered_map<SymbolId, std::uint64_t> const&  symbolVa,
    TargetSchema const&                                 targetSchema,
    std::string_view                                    writerName,
    DiagnosticReporter&                                 reporter,
    std::vector<std::uint64_t>*                         siteVasOut = nullptr) {
    using ::dss::link::format::detail::emit;
    for (std::size_t j = 0; j < layout.itemIndices.size(); ++j) {
        auto const& di = dataItems[layout.itemIndices[j]];
        if (di.relocations.empty()) continue;
        std::size_t const itemBaseOff =
            static_cast<std::size_t>(layout.itemOffsets[j]);
        for (auto const& rel : di.relocations) {
            auto const sIt = symbolVa.find(rel.target);
            if (sIt == symbolVa.end()) {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     std::format("{}: data item SymbolId={{ {} }} has a relocation "
                                 "targeting symbol #{} defined by no function / "
                                 "extern / data item.",
                                 writerName, di.symbol.v, rel.target.v));
                return false;
            }
            auto const* tri = targetSchema.relocationInfo(rel.kind);
            if (tri == nullptr) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format("{}: data item SymbolId={{ {} }} relocation kind {} "
                                 "has no TargetRelocationInfo on the target schema.",
                                 writerName, di.symbol.v, rel.kind.v));
                return false;
            }
            if (tri->pcRelative || tri->widthBytes == 0) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format("{}: data item SymbolId={{ {} }} relocation '{}' is "
                                 "pc-relative or zero-width — a data pointer needs an "
                                 "absolute fixup with a concrete write width.",
                                 writerName, di.symbol.v, tri->name));
                return false;
            }
            std::size_t const patchOff = itemBaseOff + rel.offset;
            if (patchOff + tri->widthBytes > itemBaseOff + di.bytes.size()
                || patchOff + tri->widthBytes > bytesOut.size()) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format("{}: data item SymbolId={{ {} }} relocation offset "
                                 "{} + width {} overruns the item's {} bytes.",
                                 writerName, di.symbol.v, rel.offset,
                                 static_cast<int>(tri->widthBytes), di.bytes.size()));
                return false;
            }
            std::uint64_t const value = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(sIt->second) + rel.addend);
            for (std::uint8_t b = 0; b < tri->widthBytes; ++b) {
                bytesOut[patchOff + b] =
                    static_cast<std::uint8_t>((value >> (8u * b)) & 0xFFu);
            }
            if (siteVasOut != nullptr) {
                if (tri->widthBytes != 8) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         std::format("{}: data item SymbolId={{ {} }} absolute "
                                     "relocation '{}' has width {} — only an 8-byte "
                                     "absolute fixup has a base-relocation form.",
                                     writerName, di.symbol.v, tri->name,
                                     static_cast<int>(tri->widthBytes)));
                    return false;
                }
                siteVasOut->push_back(
                    sectionVa + static_cast<std::uint64_t>(patchOff));
            }
        }
    }
    return true;
}

} // namespace dss::link::format
