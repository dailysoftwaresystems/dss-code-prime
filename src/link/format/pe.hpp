#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <vector>

// PE/COFF relocatable object (.obj) writer — plan 14 LK2 cycle 1.
//
// Second format walker plugged into the format-blind `link()`
// dispatch. The shape mirrors LK1's ELF walker
// (`src/link/format/elf.cpp`) — pure free function consuming
// structured input + schema configs, emitting bytes, leaving error
// reporting to the caller-owned reporter.
//
// Cycle scope (minimal valid relocatable `.obj` for x86_64-windows):
//   * IMAGE_FILE_HEADER (20 bytes) with the identity bytes.
//   * One IMAGE_SECTION_HEADER (40 bytes) per emitted section.
//   * `.text` raw bytes + per-section IMAGE_RELOCATION table.
//   * IMAGE_SYMBOL[] (18 bytes packed each) — one symbol per
//     AssembledFunction + extern symbols for unresolved relocation
//     targets. PE/COFF does not require local-then-global ordering.
//   * String table (length-prefix u32 size + NUL-terminated
//     strings) for any symbol name > 8 chars.
//   * No optional header, no program headers — .obj only. The
//     image-side (.exe/.dll) path arrives in plan 14 LK1 cycle 2
//     scope (for ELF) and the equivalent later cycle for PE.
//
// Diagnostics: callers detect failure via `reporter.errorCount()`.
// On internal `K_*` emission the walker returns an empty byte
// vector.

namespace dss::pe {

[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter);

} // namespace dss::pe
