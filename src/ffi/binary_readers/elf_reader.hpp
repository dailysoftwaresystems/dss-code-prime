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

// Read `.dynsym` + `.dynstr` from an ELF64 LE binary; emit one
// `ImportSurface` row per non-empty-name dynamic symbol. Shares
// byte-decode primitives + reporter wiring with the sibling readers
// via `binary_readers/reader_common.hpp`.
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, BinaryReadError>
readElf64(std::span<std::uint8_t const> bytes,
          std::string_view              libraryPathLabel,
          DiagnosticReporter&           reporter);

} // namespace dss::ffi
