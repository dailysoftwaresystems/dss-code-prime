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

// Read `LC_SYMTAB` (filtered by `LC_DYSYMTAB` external-defined slice
// when present) from a 64-bit Mach-O binary; emit one `ImportSurface`
// row per externally-defined symbol. Closes anchor
// D-FF1-PARTIAL-CORRUPTION-MACHO (the `F_BinaryReaderPartialCorruption`
// counter contract end-of-parse Warning, mirroring ELF+PE).
//
// 64-bit Mach-O only (`mach_header_64` / magic `0xFEEDFACF`). FAT
// universal binaries (`0xCAFEBABE`) and 32-bit Mach-O (`0xFEEDFACE`)
// are dispatched to `UnsupportedFormat` upstream — anchors
// D-FF1-MACHO-FAT and D-FF1-MACHO-32 reserved.
//
// All defined exports are mapped to `SymbolKind::Function` (v1, like
// PE); the section-table walk that distinguishes __TEXT vs __DATA vs
// __TLS is deferred — anchor D-FF1-MACHO-SECT-KIND. Trigger: first
// downstream FFI consumer needs to distinguish data-symbol exports
// from function-symbol exports (likely FF6 libc smoke on macOS
// reading a real `__DATA` global, OR an FFI ingest path that
// surfaces a misclassified TLS variable).
//
// Weak-def detection (`n_desc & N_WEAK_DEF`) is deferred — anchor
// D-FF1-MACHO-WEAK-DEF. Trigger: first symbol with `n_desc &
// N_WEAK_DEF` observed in a real corpus that the FFI pipeline
// surfaces as `SymbolLinkage::External` and downstream
// `applyCMangling` / abi-catalog dispatch needs to disambiguate.
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, BinaryReadError>
readMacho(std::span<std::uint8_t const> bytes,
          std::string_view              libraryPathLabel,
          DiagnosticReporter&           reporter);

} // namespace dss::ffi
