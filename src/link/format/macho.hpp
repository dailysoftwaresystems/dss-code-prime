#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <vector>

// Mach-O 64-bit relocatable object (.o, MH_OBJECT) writer —
// plan 14 LK3 cycle 1.
//
// Third format walker plugged into the format-blind `link()`
// dispatch. Mirrors LK1's ELF walker + LK2's PE walker — pure free
// function consuming structured input + schema configs, emitting
// bytes, leaving error reporting to the caller-owned reporter.
//
// Cycle scope (minimal valid MH_OBJECT for x86_64-darwin):
//   * `mach_header_64` (32 B) with the identity bytes + filetype =
//     MH_OBJECT.
//   * One `LC_SEGMENT_64` (72 B) wrapping all sections; per Apple
//     convention the segment name is empty for MH_OBJECT (each
//     `section_64.segname` carries the per-section segment name
//     like `__TEXT`).
//   * One `section_64` (80 B) for `__text` with two-level naming
//     (`__TEXT`, `__text`).
//   * `LC_SYMTAB` (24 B) pointing at the symbol table + string
//     table file offsets.
//   * `__text` raw bytes + per-section `relocation_info` table
//     (8 B packed each).
//   * `nlist_64[]` (16 B packed each) — defined extern functions
//     + undefined externs. Mach-O does NOT mandate local-then-
//     global ordering when LC_DYSYMTAB is absent; the substrate
//     emits defined-then-undefined for cross-format consistency.
//   * String table (NUL-seeded; same shape as ELF, distinct from
//     PE's u32-size-prefix form).
//
// No LC_DYSYMTAB this cycle (Apple's ld64 re-derives it). No
// LC_DYLD_INFO / LC_MAIN / LC_LOAD_DYLIB — those are executable-
// side concerns anchored at LK6 (dynamic linking) and a future
// macho-executable cycle.

namespace dss::macho {

[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter);

} // namespace dss::macho
