#pragma once

#include "link/format/macho_indirect_symbols.hpp"  // kMachoNlist64Size, kMachoNlistTypeOff

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// The Mach-O IMAGE writer's LC_DYSYMTAB band invariant, as a pure predicate
// over the bytes that were actually emitted — so it can be EXERCISED by a test
// rather than only read. Sibling of `machoIndirectSymbolBreach` (same file
// family, same argument) and of the ELF writer's `elfSymtabPartitionBreach`.
//
// LC_DYSYMTAB partitions the nlist_64 table into three CONTIGUOUS bands that
// `nm`, `strip`, `dyld_info` and dyld itself read by index range rather than
// by inspecting each record: local symbols FIRST (`ilocalsym`/`nlocalsym`),
// then externally defined (`iextdefsym`/`nextdefsym`), then undefined
// (`iundefsym`/`nundefsym`). Every record inside a band must carry the n_type
// the band promises: a local is `N_SECT` with NO `N_EXT`, an external
// definition is `N_SECT|N_EXT`, an undefined import is `N_UNDF|N_EXT`. Both
// image arms emit the bands in that order by construction
// (D-LINK-MACHO-IMAGE-STATIC-FN-EMITTED-N-EXT) — but "by construction" is a
// comment, and the alias pass appends records AFTER the binding-ordered loop,
// which is exactly the shape that lands a record on the wrong side of a
// boundary and lets a conforming reader mis-partition the whole table with no
// load failure. So the invariant is CHECKED over the finished bytes.
//
// This is Mach-O's record layout, not shared vocabulary: it belongs to the
// Mach-O writer exactly as `stbForBinding` belongs to the ELF one.

namespace dss::link::format {

// N_SECT (0x0E) with no N_EXT: a defined symbol local to its image.
inline constexpr std::uint8_t kMachoNTypeSectLocal = 0x0E;
// N_SECT | N_EXT (0x0F): an externally visible defined symbol.
inline constexpr std::uint8_t kMachoNTypeSectExt   = 0x0F;

// The six LC_DYSYMTAB band fields, in their wire order.
struct MachoDysymtabBands {
    std::uint32_t ilocalsym  = 0;
    std::uint32_t nlocalsym  = 0;
    std::uint32_t iextdefsym = 0;
    std::uint32_t nextdefsym = 0;
    std::uint32_t iundefsym  = 0;
    std::uint32_t nundefsym  = 0;
};

// Returns an empty string when the three bands tile `nlistBytes` exactly, in
// order, and every record carries the n_type its band promises; otherwise a
// sentence naming the offending field or record, for the caller's diagnostic.
[[nodiscard]] inline std::string
machoDysymtabBandBreach(std::span<std::uint8_t const> nlistBytes,
                        MachoDysymtabBands const&     bands) {
    if (nlistBytes.size() % kMachoNlist64Size != 0) {
        return "the nlist_64 table is " + std::to_string(nlistBytes.size())
               + " bytes, which is not a whole number of "
               + std::to_string(kMachoNlist64Size) + "-byte records";
    }
    std::size_t const nsyms = nlistBytes.size() / kMachoNlist64Size;
    if (bands.ilocalsym != 0) {
        return "ilocalsym is " + std::to_string(bands.ilocalsym)
               + " but the local band must open the table at index 0";
    }
    if (bands.iextdefsym != bands.nlocalsym) {
        return "iextdefsym is " + std::to_string(bands.iextdefsym)
               + " but the externally-defined band must start exactly where "
                 "the local band ends (nlocalsym = "
               + std::to_string(bands.nlocalsym) + ")";
    }
    if (bands.iundefsym != bands.iextdefsym + bands.nextdefsym) {
        return "iundefsym is " + std::to_string(bands.iundefsym)
               + " but the undefined band must start exactly where the "
                 "externally-defined band ends (iextdefsym + nextdefsym = "
               + std::to_string(bands.iextdefsym + bands.nextdefsym) + ")";
    }
    if (bands.iundefsym + bands.nundefsym != nsyms) {
        return "the three bands end at index "
               + std::to_string(bands.iundefsym + bands.nundefsym)
               + " but the table holds " + std::to_string(nsyms)
               + " symbols — the bands must tile LC_SYMTAB.nsyms exactly";
    }
    for (std::size_t i = 0; i < nsyms; ++i) {
        std::uint8_t const nType =
            nlistBytes[i * kMachoNlist64Size + kMachoNlistTypeOff];
        char const*   band     = nullptr;
        std::uint8_t  expected = 0;
        if (i < bands.iextdefsym) {
            band = "local"; expected = kMachoNTypeSectLocal;
        } else if (i < bands.iundefsym) {
            band = "externally-defined"; expected = kMachoNTypeSectExt;
        } else {
            band = "undefined"; expected = kMachoNTypeUndefExt;
        }
        if (nType != expected) {
            return "symbol #" + std::to_string(i) + " sits in the " + band
                   + " band but its n_type is "
                   + std::to_string(static_cast<unsigned>(nType))
                   + " rather than "
                   + std::to_string(static_cast<unsigned>(expected))
                   + " — a reader walking the band by index would misdescribe "
                     "its linkage";
        }
    }
    return {};
}

} // namespace dss::link::format
