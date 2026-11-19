// The `r_info` word of a Mach-O `relocation_info` entry (<mach-o/reloc.h>), and the part of it a format
// document states.
//
// LAYOUT, least significant bit first: r_symbolnum bits 0..23, r_pcrel bit 24, r_length bits 25..26,
// r_extern bit 27, r_type bits 28..31. A format document's wire id -- a `relocations` row's `nativeId`, and
// each id of a `macho.differenceRelocations` row -- is the PACKED (r_type<<28)|(r_length<<25)|(r_pcrel<<24):
// everything except the two fields that belong to ONE entry (which symbol or section it names, `r_symbolnum`,
// and which of the two that is, `r_extern`).
//
// ONE HOME for the layout's READ side and its LOAD-time rules: the object reader (`macho_object_reader.cpp`)
// takes an entry apart with these, and the Mach-O backend's validation (`macho_backend.cpp`) holds the ids a
// document declares to them. (The writer in `macho.cpp` composes the word from a row's `nativeId` and states
// the same layout where it does.)
#pragma once

#include <cstdint>

namespace dss::macho {

inline constexpr std::uint32_t kRInfoSymbolnumMask = 0x00FFFFFFu;   // r_symbolnum, bits 0..23
inline constexpr std::uint32_t kRInfoPcrelBit      = 1u << 24;      // r_pcrel
inline constexpr std::uint32_t kRInfoLengthShift   = 25;            // r_length, bits 25..26
inline constexpr std::uint32_t kRInfoLengthMask    = 0x3u;
inline constexpr std::uint32_t kRInfoExternBit     = 1u << 27;      // r_extern
inline constexpr std::uint32_t kRInfoNativeIdMask  = 0xF7000000u;   // r_type | r_length | r_pcrel

// The width of the field an entry patches. `r_length` is the log2 of its byte count: 0..3 -> 1, 2, 4, 8.
[[nodiscard]] constexpr std::uint32_t relocationFieldBytes(std::uint32_t nativeId) noexcept {
    return 1u << ((nativeId >> kRInfoLengthShift) & kRInfoLengthMask);
}

static_assert((kRInfoNativeIdMask & (kRInfoExternBit | kRInfoSymbolnumMask)) == 0u
                  && (kRInfoNativeIdMask | kRInfoExternBit | kRInfoSymbolnumMask) == 0xFFFFFFFFu,
              "a wire id and the two per-entry fields partition the r_info word");
static_assert((kRInfoNativeIdMask & kRInfoPcrelBit) != 0u
                  && (kRInfoNativeIdMask & (kRInfoLengthMask << kRInfoLengthShift))
                         == (kRInfoLengthMask << kRInfoLengthShift),
              "r_pcrel and r_length are part of the wire id");
static_assert(relocationFieldBytes(0x04000000u) == 4u && relocationFieldBytes(0x06000000u) == 8u
                  && relocationFieldBytes(0x54000000u) == 4u && relocationFieldBytes(0x16000000u) == 8u,
              "r_length 2 is a 4-byte field and r_length 3 an 8-byte one, whatever the type");

}  // namespace dss::macho
