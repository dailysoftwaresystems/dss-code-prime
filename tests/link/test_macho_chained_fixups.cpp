// D-LK6-14-PAYLOAD-PIN: direct unit tests for the chained-fixups
// payload builder hoisted from macho.cpp at the d312c1c audit fold.
// Pins byte-structure invariants of the LC_DYLD_CHAINED_FIXUPS
// __LINKEDIT payload independently of the encodeExec* integration —
// D-LK6-14-INTEGRATION will call this same primitive, so any
// regression here would propagate to every chained-fixups binary.
//
// Coverage:
//   * header byte layout (28 bytes, field offsets 0/4/8/12/16/20/24)
//   * starts_in_image region — seg_count is the IMAGE'S segment count and
//     the table carries one seg_info_offset per segment, the chained one
//     alone non-zero (D-LK6-14-CHAINED-STARTS-SEG-COUNT-MISDECLARED)
//   * the 8-alignment of `starts_offset` and of the starts_in_segment
//     struct offset, on BOTH table parities
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
using dss::macho::detail::ChainedSegInfo;
using dss::macho::detail::buildChainedFixupsPayload;
using dss::macho::detail::kDyldChainedFixupsHeaderSz;
using dss::macho::detail::kDyldChainedImportSz;
using dss::macho::detail::kDyldChainedRegionAlign;
using dss::macho::detail::kDyldChainedStartsInSegmentHdrSz;

// The image shape these pins describe: __PAGEZERO / __TEXT / __DATA_CONST /
// __LINKEDIT, with __DATA_CONST (index 2) the segment that carries the
// chains. It is the shape `encodeExecDynamic` produces for a module with
// externs and no writable globals, and the shape ld64 was measured on.
constexpr std::uint32_t kFourSegmentImage   = 4u;
constexpr std::uint32_t kDataConstSegIndex  = 2u;
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
    auto const out = buildChainedFixupsPayload(imports, kFourSegmentImage);

    // ⚠ HEADER IS 28 BYTES: SEVEN u32s (D-LK6-14-CHAINED-FIXUPS-HEADER-TRUNCATED).
    // This cell PINNED THE DEFECT before: it asserted a 24-byte header with
    // u16 format fields, so it stayed green over a payload Apple's own
    // `dyld_info` refuses to read. The numbers below come from Apple's header
    // and Apple's compiler (`sizeof` = 28, `imports_format` @20,
    // `symbols_format` @24), and from the bytes of an ld64-linked executable.
    ASSERT_GE(out.size(), kDyldChainedFixupsHeaderSz);
    EXPECT_EQ(kDyldChainedFixupsHeaderSz, 28u)
        << "sizeof(dyld_chained_fixups_header) — seven uint32_t, measured "
           "against the installed MacOSX.sdk";
    EXPECT_EQ(readU32LE(out,  0), 0u)            << "fixups_version";
    EXPECT_EQ(readU32LE(out,  4), 32u)
        << "starts_offset = the 28-byte header rounded up to 8 — ld64 writes "
           "32 here on every image measured, and the region it names begins "
           "with a struct whose _Alignof is 8";
    // starts_in_image = 4 (seg_count) + 4 × 4 (one entry per segment) = 20.
    EXPECT_EQ(readU32LE(out,  8), 52u)           << "imports_offset (= 32 + 20)";
    EXPECT_EQ(readU32LE(out, 12), 56u)           << "symbols_offset (= 52 + 4)";
    EXPECT_EQ(readU32LE(out, 16), 1u)            << "imports_count";
    EXPECT_EQ(readU32LE(out, 20), 1u)            << "imports_format = DYLD_CHAINED_IMPORT (u32)";
    EXPECT_EQ(readU32LE(out, 24), 0u)
        << "symbols_format = uncompressed (u32). Written as a u16 this field "
           "landed at [22..23] and a reader took the NEXT region's seg_count "
           "instead, seeing a zlib-compressed pool that was never compressed";
}

// D-LK6-14-CHAINED-STARTS-SEG-COUNT-MISDECLARED. `seg_count` is the image's
// LC_SEGMENT_64 count, NOT a constant: this cell asserted 1 with a single
// entry before, which is what an Apple reader refused. `dyld_chained_starts_
// in_image` is a table INDEXED BY SEGMENT — Apple's own text on the field is
// "each entry is offset into this struct for that segment".
TEST(MachoChainedFixupsPayload, StartsInImageDeclaresOneEntryPerSegment) {
    std::vector<ChainedFixupImport> imports{
        {"_printf", 1, false},
    };
    auto const out = buildChainedFixupsPayload(imports, kFourSegmentImage);
    std::uint32_t const startsOff = readU32LE(out, 4);
    EXPECT_EQ(readU32LE(out, startsOff), kFourSegmentImage)
        << "seg_count must be the image's segment count, never a constant — "
           "D-LK6-14-CHAINED-STARTS-SEG-COUNT-MISDECLARED";
    // No ChainedSegInfo → no segment carries chains, so EVERY entry is 0.
    // The table must still be seg_count entries long: a reader indexes it.
    for (std::uint32_t i = 0; i < kFourSegmentImage; ++i) {
        EXPECT_EQ(readU32LE(out, startsOff + 4 + 4 * i), 0u)
            << "seg_info_offset[" << i << "] must be 0 with no chains "
               "declared for any segment";
    }
    // The table is sized by seg_count, so the next region starts after it.
    EXPECT_EQ(readU32LE(out, 8), startsOff + 4 + 4 * kFourSegmentImage)
        << "imports_offset must clear the whole seg_info_offset table";
}

// The producer arm: exactly ONE segment carries chains, and its entry — and
// only its entry — points at the dyld_chained_starts_in_segment struct.
TEST(MachoChainedFixupsPayload, StartsInImagePointsOnlyAtTheChainedSegment) {
    std::vector<ChainedFixupImport> imports{
        {"_printf", 1, false},
    };
    ChainedSegInfo seg;
    seg.segmentIndex  = kDataConstSegIndex;
    seg.segmentOffset = 0x8000;
    seg.pageSize      = 0x4000;
    seg.pageStarts.push_back(0u);
    auto const out = buildChainedFixupsPayload(imports, kFourSegmentImage, &seg);

    std::uint32_t const startsOff = readU32LE(out, 4);
    EXPECT_EQ(readU32LE(out, startsOff), kFourSegmentImage)
        << "seg_count — D-LK6-14-CHAINED-STARTS-SEG-COUNT-MISDECLARED";
    // Table end is 4 + 4×4 = 20; the struct offset is that rounded up to 8.
    // ✔MEASURED: ld64 writes exactly 24 here for a four-segment exec.
    constexpr std::uint32_t kExpectedStructOff = 24u;
    for (std::uint32_t i = 0; i < kFourSegmentImage; ++i) {
        std::uint32_t const entry = readU32LE(out, startsOff + 4 + 4 * i);
        if (i == kDataConstSegIndex) {
            EXPECT_EQ(entry, kExpectedStructOff)
                << "seg_info_offset[" << i << "] must point at the "
                   "starts_in_segment struct";
        } else {
            EXPECT_EQ(entry, 0u)
                << "seg_info_offset[" << i << "] must be 0 — that segment "
                   "carries no chains, and a non-zero entry would send dyld "
                   "walking a chain in a segment that has none";
        }
    }
    // And the struct really is there, at the offset the entry declares.
    std::size_t const structOff = startsOff + kExpectedStructOff;
    EXPECT_EQ(readU32LE(out, structOff + 0),
              static_cast<std::uint32_t>(kDyldChainedStartsInSegmentHdrSz + 2u))
        << "dyld_chained_starts_in_segment.size = header + 2 × page_count";
    EXPECT_EQ(readU16LE(out, structOff + 4), 0x4000u)  << "page_size";
    EXPECT_EQ(readU16LE(out, structOff + 6), 6u)       << "pointer_format";
    EXPECT_EQ(readU16LE(out, structOff + 20), 1u)      << "page_count";
    EXPECT_EQ(readU16LE(out, structOff + 22), 0u)      << "page_starts[0]";
}

// The struct offset is 8-ALIGNED, which the table's own length does NOT give
// you for free: `4 + 4 × seg_count` is 8-aligned only when seg_count is ODD.
// Both parities are asserted here because a fix that just wrote the table
// length would be right for one and wrong for the other.
// ✔MEASURED on Apple Silicon, three ld64 images: a 4-segment exec (table ends
// 20) → 24; a 5-segment exec (table ends 24) → 24; a 3-segment dylib (table
// ends 16) → 16. `_Alignof(struct dyld_chained_starts_in_segment)` is 8 —
// it carries a uint64_t segment_offset at its own offset 8.
TEST(MachoChainedFixupsPayload, StartsInSegmentStructOffsetIsEightAligned) {
    struct Arm { std::uint32_t segCount; std::uint32_t expectedOffset; };
    // 3 and 5 need no padding; 4 and 6 do. Every arm's expected value is the
    // one ld64 was measured to write for that table length.
    Arm const arms[] = {{3u, 16u}, {4u, 24u}, {5u, 24u}, {6u, 32u}};
    for (Arm const& arm : arms) {
        std::vector<ChainedFixupImport> imports{{"_printf", 1, false}};
        ChainedSegInfo seg;
        seg.segmentIndex = arm.segCount - 2u;  // the segment before __LINKEDIT
        seg.pageStarts.push_back(0u);
        auto const out =
            buildChainedFixupsPayload(imports, arm.segCount, &seg);
        std::uint32_t const startsOff = readU32LE(out, 4);
        EXPECT_EQ(startsOff % kDyldChainedRegionAlign, 0u)
            << "starts_offset must be 8-aligned (seg_count=" << arm.segCount
            << ")";
        std::uint32_t const entry =
            readU32LE(out, startsOff + 4 + 4 * seg.segmentIndex);
        EXPECT_EQ(entry, arm.expectedOffset)
            << "seg_count=" << arm.segCount << ": the struct offset must be "
               "the table length (" << (4 + 4 * arm.segCount)
            << ") rounded up to 8";
        EXPECT_EQ((startsOff + entry) % kDyldChainedRegionAlign, 0u)
            << "the struct's ABSOLUTE offset in the payload must be 8-aligned "
               "too — the payload itself lands 8-aligned in __LINKEDIT";
    }
}

TEST(MachoChainedFixupsPayload, ImportPackingLibOrdinalMain) {
    // libOrdinal = -2 (MAIN_EXECUTABLE) packs as low 8 bits = 0xFE.
    std::vector<ChainedFixupImport> imports{
        {"_main_sym", /*libOrdinal=*/-2, /*weakImport=*/false},
    };
    auto const out = buildChainedFixupsPayload(imports, kFourSegmentImage);
    std::uint32_t const packed = readU32LE(out, readU32LE(out, 8));
    EXPECT_EQ(packed & 0xFFu, 0xFEu)           << "lib_ordinal = -2 (two's complement low byte)";
    EXPECT_EQ((packed >> 8) & 0x1u, 0u)        << "weak_import bit clear";
    EXPECT_EQ(packed >> 9, 1u)                 << "name_offset = 1 (NUL sentinel at 0)";
}

TEST(MachoChainedFixupsPayload, ImportPackingWeakBitSet) {
    std::vector<ChainedFixupImport> imports{
        {"_weak_sym", /*libOrdinal=*/1, /*weakImport=*/true},
    };
    auto const out = buildChainedFixupsPayload(imports, kFourSegmentImage);
    std::uint32_t const packed = readU32LE(out, readU32LE(out, 8));
    EXPECT_EQ(packed & 0xFFu, 1u);
    EXPECT_EQ((packed >> 8) & 0x1u, 1u)        << "weak_import bit MUST be set";
    EXPECT_EQ(packed >> 9, 1u);
}

TEST(MachoChainedFixupsPayload, SymbolPoolHasLeadingNulSentinel) {
    std::vector<ChainedFixupImport> imports{
        {"a", 1, false},
        {"bb", 1, false},
    };
    auto const out = buildChainedFixupsPayload(imports, kFourSegmentImage);
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
    auto const out = buildChainedFixupsPayload(imports, kFourSegmentImage);
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
    auto const out = buildChainedFixupsPayload(imports, kFourSegmentImage);
    // Total: header (28) + pad to 32 + starts_in_image table (4 + 4×4 = 20)
    //        + imports (0) + pool (1 sentinel) = 53.
    EXPECT_EQ(out.size(), 53u);
    EXPECT_EQ(readU32LE(out, 16), 0u)            << "imports_count = 0";
    EXPECT_EQ(readU32LE(out, 12), 52u)           << "symbols_offset = 52 (just past starts_in_image)";
    EXPECT_EQ(out[52], 0u)                     << "pool is just the NUL sentinel";
}

TEST(MachoChainedFixupsPayload, TotalSizeForTwoImports) {
    std::vector<ChainedFixupImport> imports{
        {"_a", 1, false},
        {"_b", 1, false},
    };
    auto const out = buildChainedFixupsPayload(imports, kFourSegmentImage);
    // Header (28) + pad to 32 + starts_in_image table (20) + imports (2×4=8)
    //   + pool (1 sentinel + 3 + 3 = 7) = 67.
    EXPECT_EQ(out.size(), 67u);
}

// The size TRACKS seg_count: one more segment is four more table bytes. A
// builder that still emitted a fixed-width table would give the same size
// for both, which is the defect this arm exists to catch.
TEST(MachoChainedFixupsPayload, TableWidthTracksTheSegmentCount) {
    std::vector<ChainedFixupImport> imports{{"_a", 1, false}};
    auto const four = buildChainedFixupsPayload(imports, 4u);
    auto const five = buildChainedFixupsPayload(imports, 5u);
    EXPECT_EQ(five.size(), four.size() + 4u)
        << "one extra segment is exactly one extra seg_info_offset entry";
    EXPECT_EQ(readU32LE(four, 4), readU32LE(five, 4))
        << "starts_offset does not move — only the table under it grows";
    EXPECT_EQ(readU32LE(five, 8), readU32LE(four, 8) + 4u)
        << "imports_offset shifts by the extra entry";
}
