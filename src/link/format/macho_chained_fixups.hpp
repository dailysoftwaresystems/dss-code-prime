#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <string>
#include <vector>

// D-LK6-14 chained-fixups substrate — private internal header.
// Apple's modern dyld binding format (Xcode 12+, macOS 12+) packs
// binding info into a compact chained-pointer table in __LINKEDIT.
// The `LC_DYLD_CHAINED_FIXUPS = 0x80000034` load command points at
// a `dyld_chained_fixups_header` whose offsets locate four regions:
//   1. dyld_chained_starts_in_image — per-segment page_starts array
//   2. dyld_chained_imports          — N × DYLD_CHAINED_IMPORT (4B)
//   3. symbols pool                   — packed NUL-terminated names
//   4. (chained pointers live in __DATA_CONST — populated by
//      D-LK6-14-INTEGRATION-GOT-SLOTS; not part of THIS payload.)
//
// `buildChainedFixupsPayload` builds regions 1+2+3. Hoisted out of
// macho.cpp's anonymous namespace at the d312c1c audit fold so
// direct unit tests can pin byte structure independently of the
// integration fold (test-analyzer + silent-failure + code-architect
// 3-agent convergence). The function is pure byte-emit — no
// reporter access, no schema dispatch — so unit-testable.
//
// Format references (Apple `<mach-o/fixup-chains.h>`, reproduced
// inline to keep src/link/format/ free of host-OS SDK pulls):
//   * dyld_chained_fixups_header (28 bytes — SEVEN u32s, no u16 in it):
//       [ 0.. 3] fixups_version  (must be 0)
//       [ 4.. 7] starts_offset   (offset to dyld_chained_starts_in_image)
//       [ 8..11] imports_offset  (offset to imports array)
//       [12..15] symbols_offset  (offset to symbol pool)
//       [16..19] imports_count   (N — number of extern bindings)
//       [20..23] imports_format  (1 = DYLD_CHAINED_IMPORT)
//       [24..27] symbols_format  (0 = uncompressed, 1 = zlib)
//
// ⚠⚠ THE LAST TWO FIELDS WERE u16 HERE, AND THE HEADER 24 BYTES, UNTIL
// D-LK6-14-CHAINED-FIXUPS-HEADER-TRUNCATED. That is a WRONG ON-WIRE SHAPE, and
// it did not fail loud anywhere: with `symbols_format` written as a u16 zero at
// [22..23], a conforming reader took `imports_format` from [20..23] and got 1
// by accident (the zero high half), then took `symbols_format` from [24..27] —
// which is the FIRST FIELD OF THE NEXT REGION, `seg_count`, always ≥ 1 — and
// read the payload as declaring a ZLIB-COMPRESSED symbol pool over bytes that
// are not compressed.
// ✔MEASURED 2026-09-05 on the operator's Apple Silicon host, three ways rather
// than recalled: (1) the SDK text, `struct dyld_chained_fixups_header` = seven
// `uint32_t`; (2) Apple's own compiler, `sizeof` = 28 with `imports_format` at
// 20 and `symbols_format` at 24; (3) the emitted bytes of an ld64-linked
// executable, whose header reads `... imports_count 1, imports_format 1,
// symbols_format 0` across 28 bytes before its starts region. And the
// CONSEQUENCE was observed, not deduced: `dyld_info -exports` on a DSS-built
// chained-fixups image refused the listing with `chained fixups, symbols_format
// unknown (1)`, where the legacy-path twin of the same program listed
// `_weak_helper [weak-def]` normally.
// ⓘ dyld itself LOADED AND RAN the malformed image (rc 42 on real hardware),
// which is exactly why this survived: the defect is invisible to "does it
// work?" and visible only to a reader that parses the struct.
//   * dyld_chained_starts_in_image (variable):
//       [ 0.. 3] seg_count       (the image's LC_SEGMENT_64 COUNT)
//       [ 4..]   seg_info_offset[seg_count] (u32 each; 0 = no chains)
//
// ⚠⚠ `seg_count` WAS HARD-CODED 1 HERE, WHATEVER THE IMAGE'S REAL SEGMENT
// COUNT WAS, UNTIL D-LK6-14-CHAINED-STARTS-SEG-COUNT-MISDECLARED. This is a
// table INDEXED BY SEGMENT — Apple's own text on the field reads "each entry
// is offset into this struct for that segment" — so declaring one entry for a
// four-segment image both understates the table and points a reader at the
// wrong index. `dyld_info -exports` refused such an image with `chained
// fixups, seg_count does not match number of segments`.
// ✔MEASURED 2026-09-05 on the operator's Apple Silicon host (macOS 26.6.2,
// Apple clang 21.0.0), three instruments with a CONTROL on each:
//   (1) the SDK text, verbatim as quoted above;
//   (2) Apple's own compiler — `sizeof(dyld_chained_starts_in_image)` 8,
//       `seg_count` @0, `seg_info_offset` @4, element width 4;
//   (3) ld64's emitted bytes for two programs that DIFFER ONLY in segment
//       count — a four-segment exec (__PAGEZERO/__TEXT/__DATA_CONST/
//       __LINKEDIT) declares `seg_count 4` with the table `[0, 0, 24, 0]`,
//       and the same program plus one writable global (five segments, __DATA
//       inserted) declares `seg_count 5` with `[0, 0, 24, 0, 0]`. The second
//       image is the CONTROL that makes the first mean something: ld64 TRACKS
//       the image's segment count rather than always writing 4.
//
// ★ EACH `seg_info_offset[i]` IS 8-ALIGNED WITHIN THIS STRUCT, and so is
// `starts_offset` within the payload — which REFUTES the closing note that
// predicted `4 + 4 × seg_count` for the struct offset. In the four-segment
// image the table ends at 20 and ld64 writes 24; in the five-segment one it
// ends at 24 and ld64 writes 24; in a three-segment dylib it ends at 16 and
// ld64 writes 16 — the four-segment case is the one that discriminates, and
// it discriminates AGAINST the arithmetic prediction. The alignment is not
// mimicry: `_Alignof(struct dyld_chained_starts_in_segment)` is 8 (measured
// from Apple's compiler) because the struct carries a `uint64_t
// segment_offset` at its own offset 8, and dyld reads it through that type.
// ⓘ A 4-mod-8 struct offset is nonetheless TOLERATED by the reader: an ld64
// image byte-patched to place the struct at 20 still listed its exports (the
// run arm of that experiment was inconclusive — its CONTROL, the same
// copy-and-re-sign with the struct left at 24, failed the same way, so the
// patch machinery and not the misalignment explains it). DSS aligns because
// the type declares the requirement, not because a reader enforced it.
//   * DYLD_CHAINED_IMPORT (4 bytes packed; format=1):
//       bits [ 0.. 7]  lib_ordinal (signed 8-bit; -2=MAIN, -1=SELF,
//                                   1..N = LC_LOAD_DYLIB index)
//       bit  [ 8]      weak_import
//       bits [ 9..31]  name_offset (23-bit; > 2^23-1 = 8MiB-1 invalid)
//
// NOTE: prior docblock incorrectly framed lib_ordinal as a 1-bit
// msb + 7-bit low split — that's `DYLD_CHAINED_IMPORT_ADDEND64`
// (format=3), a DIFFERENT struct. Format=1 is flat 8/1/23.
// (4-agent convergence on d312c1c audit: code-reviewer + silent-
// failure + comment-analyzer + type-design.)

namespace dss::macho::detail {

// On-wire constants reproduced from Apple's `<mach-o/fixup-chains.h>`.
constexpr std::uint32_t kDyldChainedFixupsVersion = 0u;
// ⚠ BOTH FORMAT FIELDS ARE u32, and the type is load-bearing rather than
// cosmetic — see the header-shape note above for what a u16 pair did to the
// two fields that follow. D-LK6-14-CHAINED-FIXUPS-HEADER-TRUNCATED.
constexpr std::uint32_t kDyldChainedImportsFormat = 1u;  // DYLD_CHAINED_IMPORT
constexpr std::uint32_t kDyldChainedSymbolsFormat = 0u;  // uncompressed
constexpr std::size_t   kDyldChainedFixupsHeaderSz = 28u;
constexpr std::size_t   kDyldChainedImportSz       = 4u;
// Region/struct alignment inside the payload. `dyld_chained_starts_in_segment`
// declares `_Alignof` 8 (its `segment_offset` is a `uint64_t` at struct offset
// 8) and ld64 8-aligns both `starts_offset` and every `seg_info_offset[i]`.
// D-LK6-14-CHAINED-STARTS-SEG-COUNT-MISDECLARED; the three-image measurement
// is in the struct-shape note above.
constexpr std::size_t   kDyldChainedRegionAlign    = 8u;
// dyld_chained_starts_in_segment fixed header size (before page_starts):
//   size u32 + page_size u16 + pointer_format u16 + segment_offset u64
//   + max_valid_pointer u32 + page_count u16 = 22 bytes.
constexpr std::size_t   kDyldChainedStartsInSegmentHdrSz = 22u;
// DYLD_CHAINED_PTR_64 (non-authenticated 64-bit chained pointers).
// Pairs with the bitfield layout assembled by the writer
// (ordinal:24 addend:8 reserved:19 next:12 bind:1); a pointer_format
// swap MUST move together with the bits.
// Anchored: D-LK6-14-CHAINEDPTR-FORMAT-COUPLING.
// Trigger: D-LK6-14-ARM64E lands an arm64e pointer_format = 1
// (DYLD_CHAINED_PTR_ARM64E) with the auth-discriminator variant.
constexpr std::uint16_t kDyldChainedPtrFormat64 = 6u;
// `next` field stride for DYLD_CHAINED_PTR_64: 4-byte units. Adjacent
// 8-byte chained-pointer slots are therefore 2 units apart. Apple's
// DYLD_CHAINED_PTR_ARM64E uses 8-byte units — when arm64e lands,
// introduce a sibling `kDyldChainedPtrArm64eNextStride` constant.
constexpr std::uint64_t kDyldChainedPtr64NextStride = 2u;
// DYLD_CHAINED_PTR_START_NONE — page has no chained pointers.
// Reserved for D-LK6-14-MULTI-PAGE-GOT (pages between chains will
// carry this sentinel in `page_starts[i]`).
constexpr std::uint16_t kDyldChainedPtrStartNone = 0xFFFFu;

// Max name_offset that fits the 23-bit field. 8 MiB - 1.
constexpr std::uint32_t kDyldChainedImportNameOffsetMax = (1u << 23) - 1u;

// Per-segment chain layout. When passed to buildChainedFixupsPayload,
// the helper emits a `dyld_chained_starts_in_segment` struct inside
// Region 1 and points `seg_info_offset[segmentIndex]` at it, leaving
// every OTHER segment's entry 0 ("no chains in this segment"). When
// omitted, every entry in the table is 0 — an image whose segments
// carry no chains at all.
//
// Apple `dyld_chained_starts_in_segment` (22-byte header + page_starts):
//   [ 0.. 3] size              (this struct + page_starts array)
//   [ 4.. 5] page_size         (0x1000 x86_64; 0x4000 arm64)
//   [ 6.. 7] pointer_format    (6 = DYLD_CHAINED_PTR_64)
//   [ 8..15] segment_offset    (VM offset of segment from __TEXT base)
//   [16..19] max_valid_pointer (0 for 64-bit)
//   [20..21] page_count        (length of page_starts)
//   [22..]   page_starts[N]    (u16 each; 0xFFFF = no chain on page)
struct ChainedSegInfo {
    // WHICH LC_SEGMENT_64 carries the chains, as its 0-based ordinal in
    // load-command emission order — the index this struct's offset is written
    // at in `seg_info_offset[]`. The caller derives it from the same segment
    // presence flags that decide the segment count; it is never a constant,
    // because the conditional segments (__PAGEZERO, __DATA_CONST, __DATA) move
    // every later ordinal. D-LK6-14-CHAINED-STARTS-SEG-COUNT-MISDECLARED.
    std::uint32_t              segmentIndex  = 0;
    std::uint64_t              segmentOffset = 0;
    std::uint16_t              pageSize      = 0x1000;
    std::uint16_t              pointerFormat = kDyldChainedPtrFormat64;
    std::vector<std::uint16_t> pageStarts;  // size = page_count
};

// One row supplied by the caller. `libOrdinal` is the signed 8-bit
// library reference: -2 = MAIN_EXECUTABLE, -1 = SELF, 1..N = the
// 1-based ordinal of an LC_LOAD_DYLIB in declaration order.
struct ChainedFixupImport {
    std::string  name;
    std::int8_t  libOrdinal = -2;
    bool         weakImport = false;
};

// Build the LC_DYLD_CHAINED_FIXUPS payload (regions 1+2+3). The
// returned blob is the byte sequence the linker writes into
// __LINKEDIT at the file offset LC_DYLD_CHAINED_FIXUPS::dataoff
// points at. Pure byte-emit — no reporter, no schema, no I/O.
//
// `segmentCount` is the image's LC_SEGMENT_64 count and sizes the
// `seg_info_offset[]` table. It has NO DEFAULT deliberately: the
// value this parameter exists to carry was previously a hard-coded
// 1 inside the emitter, and a defaulted parameter would let the next
// caller inherit exactly that wrong constant without writing it down
// (D-LK6-14-CHAINED-STARTS-SEG-COUNT-MISDECLARED).
//
// PRECONDITION: `segInfo->segmentIndex < segmentCount`. A caller that
// violates it names a segment the table has no entry for, and the
// emitted image would tell dyld no segment carries chains — the
// producer checks this before calling and fails loud.
//
// PRECONDITION: every import's eventual `name_offset` (computed
// from the symbols-pool layout this function produces) must fit
// in 23 bits. Names totaling > `kDyldChainedImportNameOffsetMax`
// bytes silently truncate the packed import row's `name_offset`
// field — anchor D-LK6-14-NAME-OFFSET-OVERFLOW. Callers should
// pre-check the cumulative byte count of `imp.name + 1` (NUL)
// across all imports.
[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
buildChainedFixupsPayload(std::vector<ChainedFixupImport> const& imports,
                          std::uint32_t         segmentCount,
                          ChainedSegInfo const* segInfo = nullptr);

} // namespace dss::macho::detail
