#include "link/format/macho_chained_fixups.hpp"

#include "link/format/byte_emit.hpp"

namespace dss::macho::detail {

std::vector<std::uint8_t>
buildChainedFixupsPayload(std::vector<ChainedFixupImport> const& imports,
                          ChainedSegInfo const*                  segInfo) {
    using namespace dss::link::format::detail;
    std::vector<std::uint8_t> out;

    // Layout offsets. The dyld_chained_starts_in_image region holds
    // a seg_count + one seg_info_offset[0] entry. When `segInfo` is
    // provided (the D-LK6-14-INTEGRATION-GOT-SLOTS path), region 1
    // ALSO contains the `dyld_chained_starts_in_segment` struct
    // directly after the seg_info_offset table — pointed at by
    // seg_info_offset[0] = 8. When `segInfo` is null (substrate
    // path), seg_info_offset[0] = 0 ("no chains in segment") and
    // dyld processes no fixups.
    constexpr std::uint32_t kSegCount = 1u;
    std::size_t const startsOff      = kDyldChainedFixupsHeaderSz;
    // starts_in_image header = seg_count (u32) + seg_info_offset[0] (u32).
    constexpr std::size_t kStartsInImageHdrSz = 4u + 4u * kSegCount;
    std::size_t const startsInSegmentSize = segInfo
        ? kDyldChainedStartsInSegmentHdrSz
            + 2u * segInfo->pageStarts.size()
        : 0u;
    std::size_t const startsSize  = kStartsInImageHdrSz + startsInSegmentSize;
    std::size_t const importsOff  = startsOff + startsSize;
    std::size_t const importsSize = kDyldChainedImportSz * imports.size();
    std::size_t const symbolsOff  = importsOff + importsSize;

    // Header (24 bytes).
    appendU32LE(out, kDyldChainedFixupsVersion);
    appendU32LE(out, static_cast<std::uint32_t>(startsOff));
    appendU32LE(out, static_cast<std::uint32_t>(importsOff));
    appendU32LE(out, static_cast<std::uint32_t>(symbolsOff));
    appendU32LE(out, static_cast<std::uint32_t>(imports.size()));
    appendU16LE(out, kDyldChainedImportsFormat);
    appendU16LE(out, kDyldChainedSymbolsFormat);

    // Region 1a: dyld_chained_starts_in_image (8 bytes for kSegCount=1).
    appendU32LE(out, kSegCount);
    if (segInfo) {
        // seg_info_offset[0] = 8 — points at the
        // dyld_chained_starts_in_segment struct that immediately
        // follows the seg_info_offset table.
        appendU32LE(out, static_cast<std::uint32_t>(kStartsInImageHdrSz));
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
    } else {
        // Substrate path: seg_info_offset[0] = 0 → dyld sees "no
        // chains in segment". No starts_in_segment emitted.
        appendU32LE(out, 0u);
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
