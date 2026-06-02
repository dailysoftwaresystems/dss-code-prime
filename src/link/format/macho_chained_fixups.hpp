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
//   * dyld_chained_fixups_header (24 bytes):
//       [ 0.. 3] fixups_version  (must be 0)
//       [ 4.. 7] starts_offset   (offset to dyld_chained_starts_in_image)
//       [ 8..11] imports_offset  (offset to imports array)
//       [12..15] symbols_offset  (offset to symbol pool)
//       [16..19] imports_count   (N — number of extern bindings)
//       [20..21] imports_format  (1 = DYLD_CHAINED_IMPORT)
//       [22..23] symbols_format  (0 = uncompressed)
//   * dyld_chained_starts_in_image (variable):
//       [ 0.. 3] seg_count       (number of segments — 1 in v1 substrate)
//       [ 4..]   seg_info_offset[seg_count] (u32 each; 0 = no chains)
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
constexpr std::uint16_t kDyldChainedImportsFormat = 1u;  // DYLD_CHAINED_IMPORT
constexpr std::uint16_t kDyldChainedSymbolsFormat = 0u;  // uncompressed
constexpr std::size_t   kDyldChainedFixupsHeaderSz = 24u;
constexpr std::size_t   kDyldChainedImportSz       = 4u;

// Max name_offset that fits the 23-bit field. 8 MiB - 1.
constexpr std::uint32_t kDyldChainedImportNameOffsetMax = (1u << 23) - 1u;

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
// PRECONDITION: every import's eventual `name_offset` (computed
// from the symbols-pool layout this function produces) must fit
// in 23 bits. Names totaling > `kDyldChainedImportNameOffsetMax`
// bytes silently truncate the packed import row's `name_offset`
// field — anchor D-LK6-14-NAME-OFFSET-OVERFLOW. Callers should
// pre-check the cumulative byte count of `imp.name + 1` (NUL)
// across all imports.
[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
buildChainedFixupsPayload(std::vector<ChainedFixupImport> const& imports);

} // namespace dss::macho::detail
