#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <vector>

// ELF64 relocatable object (.o) writer — plan 14 LK1 cycle 1.
//
// One format walker plugged into the format-blind `link()` dispatch
// at `linker.cpp`. The shape mirrors the assembler-tier walkers
// (`src/asm/format/x86_variable.cpp` / `fixed32.cpp`) — a pure free
// function that consumes structured input + schema configs and emits
// bytes, leaving error reporting to the caller-owned reporter.
//
// Cycle scope (minimal valid ET_REL object):
//   * Elf64_Ehdr with the identity bytes + e_type=ET_REL.
//   * SHT_NULL + .text + .rela.text + .symtab + .strtab + .shstrtab.
//   * One symbol per AssembledFunction; GLOBAL binding, STT_FUNC type.
//   * Elf64_Rela entries translated from `AssembledFunction::relocations`.
//   * No program headers — ET_REL doesn't carry them; the ET_EXEC
//     path (program headers + segment layout) is still owed within
//     plan 14 LK1's scope.
//   * No dynamic-linking machinery (deferred to LK6 per plan 14).
//
// Diagnostics: callers detect failure via `reporter.errorCount()`.
// The parallel-index gate (`expectedFuncCount` vs `resolvedFuncCount`)
// sits in `link()`'s pre-walker `linkagePassed` snapshot upstream,
// not inside the walker. On internal `K_*` emission this function
// returns an empty byte vector.

namespace dss::elf {

[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter);

} // namespace dss::elf
