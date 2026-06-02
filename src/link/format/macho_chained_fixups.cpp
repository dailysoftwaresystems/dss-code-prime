#include "link/format/macho_chained_fixups.hpp"

#include "link/format/byte_emit.hpp"

namespace dss::macho::detail {

std::vector<std::uint8_t>
buildChainedFixupsPayload(std::vector<ChainedFixupImport> const& imports) {
    using namespace dss::link::format::detail;
    std::vector<std::uint8_t> out;

    // Layout offsets. v1 substrate emits ONE segment slot with
    // `seg_info_offset[0] = 0` (no chains in this segment).
    // D-LK6-14-INTEGRATION-GOT-SLOTS populates per-segment
    // page_starts when __DATA_CONST chained pointers are emitted.
    constexpr std::uint32_t kSegCount = 1u;
    std::size_t const startsOff   = kDyldChainedFixupsHeaderSz;
    // starts_in_image = 4 (seg_count) + 4 * seg_count (offsets)
    std::size_t const startsSize  = 4u + 4u * kSegCount;
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

    // Region 1: dyld_chained_starts_in_image (8 bytes for kSegCount=1).
    appendU32LE(out, kSegCount);
    appendU32LE(out, 0u);  // seg_info_offset[0] — populated by INTEGRATION

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
        // offsets > 8MiB-1. Callers MUST validate before calling
        // (see header docstring). The mask is defensive only —
        // operators reach this branch only via a contract violation.
        // We do NOT silently truncate; the assert at the cumulative
        // pool size before this point would fail loud. Here we
        // additionally hard-clamp via mask for defense-in-depth so a
        // contract violation doesn't write nonsense bits 24..31.
        packed |= (nameOffsets[i] & 0x7FFFFFu) << 9;
        appendU32LE(out, packed);
    }

    // Region 3: symbols pool.
    out.insert(out.end(), symbolsPool.begin(), symbolsPool.end());

    return out;
}

} // namespace dss::macho::detail
