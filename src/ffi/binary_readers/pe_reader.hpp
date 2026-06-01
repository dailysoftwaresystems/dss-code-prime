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

// Read `.edata` (export directory) from a PE32 / PE32+ binary; emit
// one `ImportSurface` row per exported NAME. Ordinal-only exports are
// surfaced as empty (v1).
//
// D-FF1-NEST split (FF1-MachO cycle): hoisted out of binary_reader.cpp
// at the 3rd-reader landing — see binary_readers/reader_common.hpp.
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, BinaryReadError>
readPe(std::span<std::uint8_t const> bytes,
       std::string_view              libraryPathLabel,
       DiagnosticReporter&           reporter);

} // namespace dss::ffi
