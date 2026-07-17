#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "ffi/binary_reader.hpp"
#include "ffi/import_surface.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace dss::ffi {

// Read the export surface of a 64-bit Mach-O binary into `ImportSurface`
// rows. c160 HARDENING (D-FF1-MACHO-READER; the Mach-O twin of the c159
// PE reader):
//   * PRIMARY -- the EXPORT TRIE (`LC_DYLD_INFO`/`LC_DYLD_INFO_ONLY`
//     export_off, or `LC_DYLD_EXPORTS_TRIE`) is what dlsym resolves, so
//     when present it is the authoritative surface. Each terminal's
//     address is classified by SECTION membership; reexports
//     (`EXPORT_SYMBOL_FLAGS_REEXPORT`) surface as `SymbolKind::Forwarder`
//     + `forwardTarget` (the PE-forwarder analog).
//   * FALLBACK -- images with no trie (a relocatable `.o`, an ancient
//     dylib, a minimal fixture) walk `LC_SYMTAB` (filtered by the
//     `LC_DYSYMTAB` external-defined slice when present), classifying by
//     each entry's `n_sect`.
// Closes anchor D-FF1-PARTIAL-CORRUPTION-MACHO (the
// `F_BinaryReaderPartialCorruption` end-of-parse Warning, mirroring
// ELF+PE): structural trie/region truncation fails loud; per-entry
// corruption (an out-of-section address, an out-of-range `n_sect`, an
// empty resolved name) is skipped + summarized.
//
// 64-bit Mach-O only (`mach_header_64` / magic `0xFEEDFACF`). FAT
// universal binaries (`0xCAFEBABE`) and 32-bit Mach-O (`0xFEEDFACE`)
// are dispatched to `UnsupportedFormat` upstream -- anchors
// D-FF1-MACHO-FAT and D-FF1-MACHO-32 reserved.
//
// Kind classification (c160): a section carrying instruction attributes
// (`S_ATTR_PURE_INSTRUCTIONS` / `S_ATTR_SOME_INSTRUCTIONS`) is code ->
// `SymbolKind::Function`; every other mapped section (`__DATA`,
// `__DATA_CONST`, `__TEXT,__const`) is data -> `SymbolKind::Object`.
// LIMITS reserved to anchor D-FF1-MACHO-SECT-KIND (the RICHER
// beyond-text/data taxonomy -- a closed-enum `MachOSectionKind` for
// coalesced sections / dyld stubs / `S_THREAD_LOCAL_*` -> `SymbolKind::Tls`,
// which today classify as Object).
//
// Weak-def detection (trie `EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION` /
// nlist `n_desc & N_WEAK_DEF`) surfacing as `SymbolLinkage::Weak` is
// deferred -- anchor D-FF1-MACHO-WEAK-DEF. Trigger: first weak
// definition in a real corpus that downstream `applyCMangling` /
// abi-catalog dispatch needs to disambiguate.
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, BinaryReadError>
readMacho(std::span<std::uint8_t const> bytes,
          std::string_view              libraryPathLabel,
          DiagnosticReporter&           reporter);

} // namespace dss::ffi
