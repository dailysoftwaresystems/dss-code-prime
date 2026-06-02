// D-LK6-14-PAYLOAD-PIN: direct unit tests for the chained-fixups
// payload builder hoisted from macho.cpp at the d312c1c audit fold.
// Pins byte-structure invariants of the LC_DYLD_CHAINED_FIXUPS
// __LINKEDIT payload independently of the encodeExec* integration —
// D-LK6-14-INTEGRATION will call this same primitive, so any
// regression here would propagate to every chained-fixups binary.
//
// Coverage:
//   * header byte layout (24 bytes, field offsets 0/4/8/12/16/20/22)
//   * starts_in_image region (seg_count=1, seg_info_offset[0]=0)
//   * imports array packing (libOrdinal in low 8 bits, weak_import
//     at bit 8, name_offset at bits 9..31)
//   * symbols pool layout (leading NUL sentinel + packed names)
//   * empty-imports edge case
//   * name_offset 23-bit field boundary
//
// (Closes test-analyzer + silent-failure HIGH-2 + code-architect
// FOLD-NOW convergence: the prior commit shipped the helper with
// zero direct unit coverage despite the commit message claiming
// otherwise.)

#include "link/format/macho_chained_fixups.hpp"
#include "link_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using dss::macho::detail::ChainedFixupImport;
using dss::macho::detail::buildChainedFixupsPayload;
using dss::macho::detail::kDyldChainedFixupsHeaderSz;
using dss::macho::detail::kDyldChainedImportSz;
// D-TEST-LE-READ-HELPERS CLOSED (8aabc04 audit fold 2026-06-01):
// promoted to the shared `link_test_support.hpp` (where readU64LE
// already lives); 2nd consumer trigger met.
using dss::link_format::test::readU16LE;
using dss::link_format::test::readU32LE;

} // namespace

TEST(MachoChainedFixupsPayload, HeaderFieldsForSingleImport) {
    std::vector<ChainedFixupImport> imports{
        {"_printf", /*libOrdinal=*/1, /*weakImport=*/false},
    };
    auto const out = buildChainedFixupsPayload(imports);

    // Header (24 bytes).
    ASSERT_GE(out.size(), kDyldChainedFixupsHeaderSz);
    EXPECT_EQ(readU32LE(out,  0), 0u)            << "fixups_version";
    EXPECT_EQ(readU32LE(out,  4), 24u)           << "starts_offset (= 24 = sizeof header)";
    // starts_in_image = 4 (seg_count) + 4 (offset[0]) = 8 bytes
    EXPECT_EQ(readU32LE(out,  8), 32u)           << "imports_offset (= 24 + 8)";
    EXPECT_EQ(readU32LE(out, 12), 36u)           << "symbols_offset (= 32 + 4)";
    EXPECT_EQ(readU32LE(out, 16), 1u)            << "imports_count";
    EXPECT_EQ(readU16LE(out, 20), 1u)            << "imports_format = DYLD_CHAINED_IMPORT";
    EXPECT_EQ(readU16LE(out, 22), 0u)            << "symbols_format = uncompressed";
}

TEST(MachoChainedFixupsPayload, StartsInImageHasOneSegmentZeroOffset) {
    std::vector<ChainedFixupImport> imports{
        {"_printf", 1, false},
    };
    auto const out = buildChainedFixupsPayload(imports);
    EXPECT_EQ(readU32LE(out, 24), 1u)            << "seg_count";
    EXPECT_EQ(readU32LE(out, 28), 0u)
        << "seg_info_offset[0] = 0 (no chains in segment v1); "
           "D-LK6-14-INTEGRATION-GOT-SLOTS populates this";
}

TEST(MachoChainedFixupsPayload, ImportPackingLibOrdinalMain) {
    // libOrdinal = -2 (MAIN_EXECUTABLE) packs as low 8 bits = 0xFE.
    std::vector<ChainedFixupImport> imports{
        {"_main_sym", /*libOrdinal=*/-2, /*weakImport=*/false},
    };
    auto const out = buildChainedFixupsPayload(imports);
    std::uint32_t const packed = readU32LE(out, 32);
    EXPECT_EQ(packed & 0xFFu, 0xFEu)           << "lib_ordinal = -2 (two's complement low byte)";
    EXPECT_EQ((packed >> 8) & 0x1u, 0u)        << "weak_import bit clear";
    EXPECT_EQ(packed >> 9, 1u)                 << "name_offset = 1 (NUL sentinel at 0)";
}

TEST(MachoChainedFixupsPayload, ImportPackingWeakBitSet) {
    std::vector<ChainedFixupImport> imports{
        {"_weak_sym", /*libOrdinal=*/1, /*weakImport=*/true},
    };
    auto const out = buildChainedFixupsPayload(imports);
    std::uint32_t const packed = readU32LE(out, 32);
    EXPECT_EQ(packed & 0xFFu, 1u);
    EXPECT_EQ((packed >> 8) & 0x1u, 1u)        << "weak_import bit MUST be set";
    EXPECT_EQ(packed >> 9, 1u);
}

TEST(MachoChainedFixupsPayload, SymbolPoolHasLeadingNulSentinel) {
    std::vector<ChainedFixupImport> imports{
        {"a", 1, false},
        {"bb", 1, false},
    };
    auto const out = buildChainedFixupsPayload(imports);
    std::size_t const symbolsOff = readU32LE(out, 12);
    EXPECT_EQ(out[symbolsOff], 0u)
        << "symbols pool MUST start with NUL sentinel (offset 0 is "
           "the empty-name marker per Apple's convention)";
    // First name at offset 1: 'a' '\0' (2 bytes)
    EXPECT_EQ(out[symbolsOff + 1], 'a');
    EXPECT_EQ(out[symbolsOff + 2], 0u);
    // Second name at offset 3: 'b' 'b' '\0'
    EXPECT_EQ(out[symbolsOff + 3], 'b');
    EXPECT_EQ(out[symbolsOff + 4], 'b');
    EXPECT_EQ(out[symbolsOff + 5], 0u);
}

TEST(MachoChainedFixupsPayload, NameOffsetsAdvancePerImport) {
    std::vector<ChainedFixupImport> imports{
        {"first",  1, false},
        {"second", 1, false},
        {"third",  1, false},
    };
    auto const out = buildChainedFixupsPayload(imports);
    // Each packed row at importsOff + i*4. Extract name_offset (bits 9..31).
    std::uint32_t const importsOff = readU32LE(out, 8);
    std::uint32_t const off0 = readU32LE(out, importsOff + 0 * 4) >> 9;
    std::uint32_t const off1 = readU32LE(out, importsOff + 1 * 4) >> 9;
    std::uint32_t const off2 = readU32LE(out, importsOff + 2 * 4) >> 9;
    // Pool layout: [0]NUL [1..5]"first"[0] [7..12]"second"[0] [14..18]"third"[0]
    EXPECT_EQ(off0, 1u)     << "_first_ at pool offset 1 (after NUL sentinel)";
    EXPECT_EQ(off1, 7u)     << "_second_ at pool offset 7 (after 'first\\0')";
    EXPECT_EQ(off2, 14u)    << "_third_ at pool offset 14 (after 'second\\0')";
}

TEST(MachoChainedFixupsPayload, EmptyImportsListEmitsHeaderPlusEmptyRegions) {
    std::vector<ChainedFixupImport> imports{};
    auto const out = buildChainedFixupsPayload(imports);
    // Total size: header (24) + starts_in_image (8) + imports (0) + pool (1 sentinel) = 33.
    EXPECT_EQ(out.size(), 33u);
    EXPECT_EQ(readU32LE(out, 16), 0u)            << "imports_count = 0";
    EXPECT_EQ(readU32LE(out, 12), 32u)           << "symbols_offset = 32 (just past starts_in_image)";
    EXPECT_EQ(out[32], 0u)                     << "pool is just the NUL sentinel";
}

TEST(MachoChainedFixupsPayload, TotalSizeForTwoImports) {
    std::vector<ChainedFixupImport> imports{
        {"_a", 1, false},
        {"_b", 1, false},
    };
    auto const out = buildChainedFixupsPayload(imports);
    // Header (24) + starts_in_image (8) + imports (2*4=8) + pool (1 sentinel + 3 + 3 = 7) = 47.
    EXPECT_EQ(out.size(), 47u);
}
