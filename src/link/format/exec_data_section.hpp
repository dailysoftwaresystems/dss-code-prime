#pragma once

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/symbol_attrs.hpp"    // definitionIsPreemptible (the ONE owner)
#include "core/types/target_schema.hpp"   // F5: TargetSchema::relocationInfo (abs64)
#include "link/format/byte_emit.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Shared exec-image DATA-SECTION substrate for the format walkers — the
// kind-parameterized layout + symbolVa logic ELF / PE / Mach-O all reuse so a
// section's byte layout lives in ONE place, not copy-pasted per writer. Closes
// D-LK4-DATA-PRODUCER (writable `.data` + zero-fill `.bss`) atop the original
// `D-LK1-ELF-EXEC-DATA-SECTIONS` rodata arm.
//
// ⚠ THE TWO NUMBERED CONCERNS BELOW ARE THE ONES THIS HEADER WAS BUILT AROUND,
// NOT AN INVENTORY OF WHAT IT NOW HOLDS — it grew `mergeFileBackedDataSection`,
// `addTlsSymbolOffsets`, `addTlsTemplateOffsets`, `collectPreemptibleDefinitions`
// and `applyDataItemRelocations` afterwards, each documented at its own
// definition. The count is left as history rather than re-counted here, because
// a summary that must be renumbered on every addition is one that goes stale
// silently; what a reader needs is that EVERY function here is format-neutral
// and fronted by the caller's `writerName`, which is stated below and stays true.
//
// The two founding concerns:
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

// Where a section whose members demand `align` must START, expressed as a FILE
// OFFSET, inside an image whose allocated sections satisfy the verbatim-mapping
// identity `va == imageBaseVa + fileOffset` (the arrangement every exec writer
// here builds: one PT_LOAD/segment mapping the file at a constant delta).
// `cursorOffset` is the first free file offset; the answer is >= it.
//
// ★★ THE RULE THIS EXISTS TO STATE: **ALIGN THE ADDRESS, THEN DERIVE THE
// OFFSET — NEVER ALIGN THE OFFSET.** `alignUp(cursorOffset, align)` produces an
// address that is a multiple of `align` only when `imageBaseVa` ALREADY is, so a
// request stronger than the image base's own alignment is silently downgraded to
// a multiple of `gcd(align, imageBaseVa)`. Nothing fails: the section header
// still says `sh_addralign = align`, the segment still maps, the program still
// runs — on an object placed somewhere it did not ask to be.
//
// ✔MEASURED (P64) and this is why the helper is worth its name: an image base of
// 0x400000 divides 8192, so `alignUp` on the OFFSET gave the right answer on
// `elf64-x86_64-linux-exec` (`.text` VA 0x401000, base 0x400000) and the WRONG
// one, by exactly 4096, on `elf64-aarch64-linux-exec` (`.text` VA 0x400000, base
// 0x3FF000) — same engine, same section, same alignment request, and the
// difference lived entirely in a DECLARED address. The x86_64 leg was not
// verifying the rule; it was benefiting from an accident of its own base.
// Aligning the VA is correct for EVERY base, so no leg is left green by luck.
//
// ⓘ The `align <= 1` short-circuit keeps the no-alignment case byte-identical.
// ⓘ A base-relative image (`imageBaseVa == 0`) makes the two domains coincide,
// so this returns exactly what rounding the offset would have — correct, and
// the reason no ET_DYN byte moves. ⚠ THAT IS A STATEMENT ABOUT THE LINK-TIME
// ADDRESS ONLY: what a base-relative image is finally mapped at is the loader's
// slide, whose granularity is the largest `p_align` among its PT_LOADs, and
// this helper has no say in that.
[[nodiscard]] inline constexpr std::uint64_t imageOffsetForAlignedVa(
        std::uint64_t imageBaseVa,
        std::uint64_t cursorOffset,
        std::uint64_t align) noexcept {
    if (align <= 1) return cursorOffset;
    std::uint64_t const va = imageBaseVa + cursorOffset;
    std::uint64_t const alignedVa = ((va + align - 1) / align) * align;
    return alignedVa - imageBaseVa;
}

// The mapping granularity a LOADABLE SEGMENT may promise, given the image base
// it sits in, the format's page alignment, and the strongest alignment among the
// sections the segment maps.
//
// ★★ WHY A SEGMENT HAS TO PROMISE ANYTHING. A BASE-RELATIVE image (an ELF
// ET_DYN — a PIE or a shared object) has no fixed address: the loader places it
// at a slide that is a multiple of the LARGEST segment alignment the image
// declares. So a member asking for more than that keeps the address the linker
// chose and LOSES its alignment at load, and nothing in the image records that
// it happened.
//
// ✔MEASURED (P64), twenty runs per port because one run decides nothing here: a
// DSS PIE carrying `aligned(8192)` statics, with every segment alignment pinned
// at the page size, returned 42 or 50 DEPENDING ON THE RUN — 10 of 20 wrong on
// arm64, 12 of 20 on x86_64 — while gcc's PIE of the same source returned 42
// twenty times out of twenty, having raised that one segment to 0x2000 and left
// its siblings at 0x1000.
//
// ⚠ RAISED ONLY AS FAR AS THE IMAGE BASE CAN KEEP IT. A loader maps
// `file[offset …]` at `base + offset`, and ELF's kernel loader additionally
// requires `p_vaddr % p_align == p_offset % p_align`; the two differ by exactly
// `imageBaseVa`, so the strongest promise a segment can honour is bounded by the
// base's own alignment. A base-relative image has `imageBaseVa == 0` and is
// never bounded; a fixed-address image is mapped at its stated address and loses
// nothing by being. That is why there is no image-flavour test here — the bound
// falls out of the arithmetic, and a flavour test would be a second owner for a
// fact this expression already holds.
//
// ⓘ ONE IMPLEMENTATION ON PURPOSE. Both ELF exec walkers need this and neither
// may own it: they name their base differently and would drift into disagreeing
// about what a segment claims, which is precisely the failure above wearing a
// different hat.
[[nodiscard]] inline constexpr std::uint64_t segmentAlignForImageBase(
        std::uint64_t imageBaseVa,
        std::uint64_t pageAlign,
        std::uint64_t strongestMemberAlign) noexcept {
    std::uint64_t promise = std::max(pageAlign, strongestMemberAlign);
    while (promise > pageAlign && imageBaseVa % promise != 0) {
        promise = std::max(pageAlign, promise / 2);
    }
    return promise;
}

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
    // Every writer passes `true` for the kinds whose item relocations it
    // patches/emits (`data` + `relro` on all three formats; `tdata` on ELF
    // and PE — the Mach-O exec passes FALSE for tdata, an intentional
    // fail-loud deferral; exec `rodata` is `true` on PE, the ELF dynamic
    // arm, AND the Mach-O exec — the latter guarded downstream by the
    // `__TEXT,__const` rebase wall), and leaves `false` where a
    // reloc-bearing item is a producer-contract breach (read-only `rodata` on
    // the static/relocatable arms — the RodataDataItemWithRelocationFailsLoud
    // discipline): the item then fails loud here, never a silent unpatched
    // slot. Deferral anchor: D-LK1-ELF-RODATA-DATAITEM-RELOC.
    bool                              allowItemRelocations = false,
    // OPT-OUT for items the CALLER lays out in a section of their OWN rather
    // than in this shared one -- `dataItems` indices this call must SKIP
    // ENTIRELY. Skipped, not merely left unnamed: an excluded item contributes
    // no bytes, no offset and no span here, so the shared section stays
    // byte-identical to what it would have been had the item never existed.
    // (An item that still occupied space would leave a hole no linker can
    // account for AND a second copy of its bytes in the section that really
    // owns it.)
    //
    // The COFF writer is the caller: COFF expresses a WEAK DEFINED symbol as a
    // COMDAT select-any section holding exactly ONE body (PE/COFF spec,
    // "COMDAT Sections (Object Only)"; D-LK-OBJECT-WEAK-DEF-RELOCATABLE),
    // because the duplicate-resolution policy is a property of the SECTION --
    // two weak bodies sharing one section would be kept or discarded as a
    // unit, which is a different and wrong program.
    //
    // ⚠ MUST BE SORTED ASCENDING -- the membership test below is a binary
    // search. The default excludes nothing, so every other caller (ELF ET_REL
    // + exec, Mach-O, the PE image arm) is unchanged BY CONSTRUCTION.
    std::span<std::size_t const>      excludedItemIndices = {}) {
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
        // Laid out by the caller in a section of its own (see the parameter's
        // note) -- contributes nothing at all here. ASCENDING by contract, so
        // the membership test is a binary search rather than a linear scan
        // inside a per-item loop (this runs over every data item of a module).
        if (std::binary_search(excludedItemIndices.begin(),
                               excludedItemIndices.end(), i)) {
            continue;
        }
        // A data item carrying its OWN relocations (data->data references —
        // a vtable / fn-ptr table / cross-CU thunk slot). Where the caller
        // patches/emits them (`allowItemRelocations=true`) they flow through;
        // where a reloc-bearing item is a producer-contract breach (read-only
        // `rodata` on most arms) it rejects loud here.
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

// Merge the file-backed `src` layout onto the END of the file-backed `into`
// layout so a caller can treat two `DataSectionKind`s as ONE section. The c145
// D-LK-RELRO-CONST-DATA-RELOCATABLE EXEC decision: the exec IMAGE folds a
// RelRoConst item into an existing relocated-writable section ("treat relro
// like `.data`" — the read-only-after hardening is a linker-SEGMENT nicety not
// required for correctness), so every exec writer routes relro through the SAME
// machinery its ordinary data section already uses (symbolVa registration,
// applyDataItemRelocations, base-reloc collection, section emission) instead of
// threading a whole parallel section through the intricate exec layout. The
// RELOCATABLE (.o) path does NOT merge — it emits a distinct `.data.rel.ro` +
// `.rela.data.rel.ro` (gcc's contract).
//
// `src`'s items are placed at `alignUp(into.spanSize, src.maxAlign)` so each
// keeps its within-section alignment (the base is a multiple of `src.maxAlign`,
// itself the max of every `src` item's alignment, so every shifted offset stays
// aligned). `itemOffsets`/`itemIndices` are appended (offsets shifted by that
// base) so `addDataSymbolVas` / `applyDataItemRelocations` over the merged
// layout still map each item to its bytes. Both layouts MUST be file-backed
// (NOT zero-fill) — the only merge the writers need; a zero-fill merge would
// mishandle the empty-`bytes` invariant. `into` is grown; `src` is unchanged.
inline void mergeFileBackedDataSection(ExecDataSectionLayout&       into,
                                       ExecDataSectionLayout const& src) {
    if (src.itemIndices.empty()) return;   // no-op: nothing to merge
    std::uint64_t const base =
        src.maxAlign <= 1
            ? into.spanSize
            : ((into.spanSize + src.maxAlign - 1) / src.maxAlign) * src.maxAlign;
    while (into.bytes.size() < base) into.bytes.push_back(0);
    for (std::size_t j = 0; j < src.itemIndices.size(); ++j) {
        into.itemOffsets.push_back(base + src.itemOffsets[j]);
        into.itemIndices.push_back(src.itemIndices[j]);
    }
    into.bytes.insert(into.bytes.end(), src.bytes.begin(), src.bytes.end());
    into.spanSize = base + src.spanSize;
    into.maxAlign = std::max(into.maxAlign, src.maxAlign);
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

// ★★ D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING — the STATIC-INITIALIZER
// half of the ADDRESS residual: WHICH of this module's own definitions may the
// loader replace, so a data slot initialized with one's address must be filled
// by the LOADER instead of baked by this writer.
//
// A data slot holding `&g` for a definition this artifact owns is normally a
// link-time constant: `applyDataItemRelocations` below bakes S + A, and a slid
// image emits the format's SELF-RELATIVE rebase row so the loader adds the
// slide. That is right for a definition no loader can replace. It is WRONG for a
// PREEMPTIBLE one, whose winner may live in another image entirely: baking this
// artifact's own body address splits ONE identifier into TWO addresses INSIDE
// ONE ARTIFACT — the CODE form (already routed through the loader-resolved slot)
// and the INITIALIZER form disagree, a C 6.2.2p2 identity break the program can
// observe with `==` and nothing diagnoses.
//
// ✔MEASURED (gcc 13.3.0 and clang 18.1.3, one `.so` each, `-O1 -shared -fPIC`,
// `fp tbl[3] = { &w, &st, &sf }` with `w` weak, `st` strong global and `sf`
// `static` as the IN-OBJECT control): both references emit a SYMBOL-based
// `R_X86_64_64 w` / `R_X86_64_64 st` for the two preemptible entries and leave
// the `static` entry a bare `R_X86_64_RELATIVE`. The control is what makes that
// a statement about preemptibility rather than about the target.
//
// ★ WHY THE SELECTION IS HERE AND THE ENCODING IS NOT. WHICH definitions qualify
// is a module + declaration fact — `definitionIsPreemptible` is its ONE owner,
// and every writer must answer it identically for one symbol or the tiers drift
// (`core/types/symbol_attrs.hpp` states that reason for the three tiers that
// already share it). HOW the resulting slot is expressed is per format: ELF
// writes a `.rela.dyn` symbol row over a ZEROED slot, Mach-O would write a BIND
// where it writes a REBASE today. Only the first half lives here.
//
// The RETURNED SET IS EMPTY whenever `preemptibleBindings` is empty — which is
// every exec / PIE flavour (a main executable is always its own winner), every
// relocatable and static-library flavour (no loader is involved), every PE
// (Windows has no symbol interposition) and both Mach-O dylibs until their own
// list is declared. An empty set means every caller's bytes are unchanged, so
// this function is INERT for every format that does not declare the key.
[[nodiscard]] inline std::unordered_set<SymbolId> collectPreemptibleDefinitions(
    AssembledModule const&         module,
    std::span<SymbolBinding const> preemptibleBindings) {
    std::unordered_set<SymbolId> preemptible;
    if (preemptibleBindings.empty()) return preemptible;  // nothing qualifies
    for (auto const& ms : module.symbols) {
        // A nameless row is not in any dynamic export set, so no loader can see
        // it — the same reason the visibility half of the predicate is asked
        // first and universally.
        if (ms.name.empty()) continue;
        if (!::dss::definitionIsPreemptible(ms.binding, ms.visibility,
                                            preemptibleBindings)) {
            continue;
        }
        preemptible.insert(ms.symbol);
    }
    return preemptible;
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
            // Data-item relocations are LINEAR fixups only: a non-Linear kind
            // (call26 / adr_prel_pg_hi21 / add_abs_lo12_nc) would apply
            // instruction-formula semantics to data bytes (c147 silent-failure
            // -review fold; the Mach-O MH_OBJECT data-reloc gate's discriminator).
            // A zero-width kind has nothing to write. Both are producer bugs.
            //
            // A LINEAR pc-relative kind, HOWEVER, is legitimate in data: a
            // position-independent jump table (a gcc `switch`) stores 4-byte
            // SELF-relative offsets (`entry = target - table_base`), reconstructed
            // by the c164/c167 relocatable-object reader as Linear pcRelative data
            // relocations. DSS emits a fixed-base image, so the patch VA is known
            // here -- apply `S + A - P + addendBias` exactly as the schema formula
            // states (the same value the instruction-fixup path computes for code).
            if (tri->formulaKind != RelocFormulaKind::Linear || tri->widthBytes == 0) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format("{}: data item SymbolId={{ {} }} relocation '{}' is "
                                 "zero-width or a non-linear instruction fixup - a "
                                 "data fixup must be a LINEAR absolute or pc-relative "
                                 "kind with a concrete write width.",
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
            // value = S + A + (pcRel ? -P : 0) + addendBias  (the schema formula).
            std::uint64_t const patchVa =
                sectionVa + static_cast<std::uint64_t>(patchOff);
            std::int64_t signedValue = static_cast<std::int64_t>(sIt->second)
                                     + rel.addend
                                     + static_cast<std::int64_t>(tri->addendBias);
            if (tri->pcRelative) signedValue -= static_cast<std::int64_t>(patchVa);
            // Signed-fit tripwire for a narrow (< 8-byte) fixup -- the SAME
            // last-line guard the instruction-fixup kernel applies
            // (`applyExecRelocations`, exec_reloc_apply.hpp): a value outside the
            // width's signed range would be silently truncated to its low bytes
            // (a wrong displacement baked into data). Relevant now that a width-4
            // pc-relative kind (a jump-table `rel32` entry) reaches this write;
            // also closes the pre-existing width-4 absolute (`abs32`) exposure.
            if (tri->widthBytes < 8) {
                std::int64_t const sMax =
                    (std::int64_t{1} << (8 * tri->widthBytes - 1)) - 1;
                if (signedValue < -sMax - 1 || signedValue > sMax) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         std::format("{}: data item SymbolId={{ {} }} relocation '{}' "
                                     "value {} does not fit signed in widthBytes={} - "
                                     "would silently truncate.",
                                     writerName, di.symbol.v, tri->name, signedValue,
                                     static_cast<int>(tri->widthBytes)));
                    return false;
                }
            }
            std::uint64_t const value = static_cast<std::uint64_t>(signedValue);
            for (std::uint8_t b = 0; b < tri->widthBytes; ++b) {
                bytesOut[patchOff + b] =
                    static_cast<std::uint8_t>((value >> (8u * b)) & 0xFFu);
            }
            // A pc-relative (self-relative) data fixup is invariant under a uniform
            // image rebase, so it needs NO base-relocation entry. Only an ABSOLUTE
            // 8-byte pointer slot does (PE `.reloc` DIR64).
            if (siteVasOut != nullptr && !tri->pcRelative) {
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
