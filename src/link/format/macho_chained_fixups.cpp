#include "link/format/macho_chained_fixups.hpp"

#include "link/format/byte_emit.hpp"

namespace dss::macho::detail {

std::vector<std::uint8_t>
buildChainedFixupsPayload(std::vector<ChainedFixupImport> const& imports,
                          std::uint32_t                          segmentCount,
                          ChainedSegInfo const*                  segInfo) {
    using namespace dss::link::format::detail;
    std::vector<std::uint8_t> out;

    // Layout offsets. `dyld_chained_starts_in_image` is a table INDEXED BY
    // SEGMENT: `seg_count` is the image's LC_SEGMENT_64 count, and it is
    // followed by exactly that many `seg_info_offset[i]` entries, 0 meaning
    // "this segment carries no chains". When `segInfo` is provided (the
    // D-LK6-14-INTEGRATION-GOT-SLOTS path) region 1 ALSO contains the
    // `dyld_chained_starts_in_segment` struct after the table, and the entry
    // at `segInfo->segmentIndex` — and only that one — points at it. When
    // `segInfo` is null, every entry is 0 and dyld processes no fixups.
    //
    // ⚠ `seg_count` USED TO BE HARD-CODED 1 with a single entry, whatever the
    // image's real segment count was —
    // [[D-LK6-14-CHAINED-STARTS-SEG-COUNT-MISDECLARED]];
    // the measurement, its controls, and the reader's refusal
    // are in the header's starts_in_image note. The caller supplies both facts
    // from values it already computes for `ncmds` and for the __got bind
    // opcodes, so no new fact enters this emitter.
    //
    // ★ REGION AND STRUCT OFFSETS ARE 8-ALIGNED, which is NOT the arithmetic
    // `4 + 4 × seg_count` a reader might predict: `dyld_chained_starts_in_
    // segment` declares `_Alignof` 8 (a `uint64_t segment_offset` at its own
    // offset 8) and ld64 pads to satisfy it — measured on three images, one of
    // which discriminates. Same reason `starts_offset` is the 28-byte header
    // rounded to 32 rather than 28.
    std::size_t const startsOff =
        alignUp(kDyldChainedFixupsHeaderSz, kDyldChainedRegionAlign);
    // seg_count (u32) + seg_info_offset[segmentCount] (u32 each).
    std::size_t const startsInImageTableSz =
        4u + 4u * static_cast<std::size_t>(segmentCount);
    // Offset of the starts_in_segment struct WITHIN the starts_in_image
    // struct — which is what `seg_info_offset[i]` means. 0 when absent.
    std::size_t const segStructOff = segInfo
        ? alignUp(startsInImageTableSz, kDyldChainedRegionAlign)
        : 0u;
    std::size_t const startsInSegmentSize = segInfo
        ? kDyldChainedStartsInSegmentHdrSz
            + 2u * segInfo->pageStarts.size()
        : 0u;
    std::size_t const startsSize  = segInfo
        ? segStructOff + startsInSegmentSize
        : startsInImageTableSz;
    std::size_t const importsOff  = startsOff + startsSize;
    std::size_t const importsSize = kDyldChainedImportSz * imports.size();
    std::size_t const symbolsOff  = importsOff + importsSize;

    // Header — SEVEN u32s, 28 bytes (D-LK6-14-CHAINED-FIXUPS-HEADER-TRUNCATED;
    // the measurement and the failure it produced are in the header's
    // struct-shape note). The last two fields are u32, not u16: written as u16
    // the header was 4 bytes short, and a conforming reader took
    // `symbols_format` from the NEXT REGION's `seg_count` and saw a
    // zlib-compressed symbol pool that was never compressed.
    appendU32LE(out, kDyldChainedFixupsVersion);
    appendU32LE(out, static_cast<std::uint32_t>(startsOff));
    appendU32LE(out, static_cast<std::uint32_t>(importsOff));
    appendU32LE(out, static_cast<std::uint32_t>(symbolsOff));
    appendU32LE(out, static_cast<std::uint32_t>(imports.size()));
    appendU32LE(out, kDyldChainedImportsFormat);
    appendU32LE(out, kDyldChainedSymbolsFormat);

    // Pad the 28-byte header out to the 8-aligned `starts_offset` written
    // above, so the region actually begins where the header says it does.
    while (out.size() < startsOff) out.push_back(0);

    // Region 1a: dyld_chained_starts_in_image — seg_count, then ONE
    // seg_info_offset per segment. Only the segment named by `segInfo`
    // carries chains; every other entry is 0.
    appendU32LE(out, segmentCount);
    for (std::uint32_t i = 0; i < segmentCount; ++i) {
        bool const chained = segInfo != nullptr && i == segInfo->segmentIndex;
        appendU32LE(out, chained ? static_cast<std::uint32_t>(segStructOff)
                                 : 0u);
    }
    if (segInfo) {
        // Pad the table out to the 8-aligned struct offset the entry above
        // points at (a no-op when the table already ends 8-aligned).
        while (out.size() < startsOff + segStructOff) out.push_back(0);
        // Region 1b: dyld_chained_starts_in_segment (22-byte header
        // + page_starts array).
        std::uint32_t const segStructSize = static_cast<std::uint32_t>(
            startsInSegmentSize);
        appendU32LE(out, segStructSize);
        appendU16LE(out, segInfo->pageSize);
        appendU16LE(out, segInfo->pointerFormat);
        appendU64LE(out, segInfo->segmentOffset);
        appendU32LE(out, 0u);  // max_valid_pointer (0 for 64-bit)
        appendU16LE(out, static_cast<std::uint16_t>(
                             segInfo->pageStarts.size()));
        for (std::uint16_t pageStart : segInfo->pageStarts) {
            appendU16LE(out, pageStart);
        }
    }

    // Build the symbols pool first so we know each import's
    // name_offset. The pool starts with a NUL sentinel (offset 0
    // is the empty-name marker per Apple's convention).
    std::vector<std::uint32_t> nameOffsets;
    nameOffsets.reserve(imports.size());
    std::vector<std::uint8_t> symbolsPool;
    symbolsPool.push_back(0);
    for (auto const& imp : imports) {
        nameOffsets.push_back(static_cast<std::uint32_t>(symbolsPool.size()));
        symbolsPool.insert(symbolsPool.end(),
                           imp.name.begin(), imp.name.end());
        symbolsPool.push_back(0);
    }

    // Region 2: imports array. Each DYLD_CHAINED_IMPORT packs into
    // 4 bytes (format=1, flat 8/1/23 — NOT the msb/low split of
    // the addend variant):
    //   bits [ 0.. 7]  lib_ordinal (signed 8-bit; -2 = MAIN, -1 = SELF,
    //                                1..N = LC_LOAD_DYLIB index)
    //   bit  [ 8]      weak_import
    //   bits [ 9..31]  name_offset (23-bit; > 2^23-1 invalid)
    for (std::size_t i = 0; i < imports.size(); ++i) {
        auto const& imp = imports[i];
        std::uint32_t packed = 0;
        packed |= static_cast<std::uint32_t>(
                    static_cast<std::uint8_t>(imp.libOrdinal)) & 0xFFu;
        if (imp.weakImport) packed |= 0x100u;
        // Silent-truncation guard (D-LK6-14-NAME-OFFSET-OVERFLOW):
        // the 23-bit name_offset field cannot represent symbol-pool
        // offsets > 8MiB-1. Caller-side enforcement at the walker
        // (encodeExecDynamic pre-check) fires loud K_SymbolUndefined.
        // The mask below is defense-in-depth — a contract violation
        // cannot write nonsense bits 24..31.
        packed |= (nameOffsets[i] & 0x7FFFFFu) << 9;
        appendU32LE(out, packed);
    }

    // Region 3: symbols pool.
    out.insert(out.end(), symbolsPool.begin(), symbolsPool.end());

    return out;
}

} // namespace dss::macho::detail
