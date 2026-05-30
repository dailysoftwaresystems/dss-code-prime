#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <vector>

// WebAssembly module writer (LK8 skeleton — plan 14 LK8).
//
// Cycle scope (intentionally minimal — plan 18 §0 owns the full
// MIR→WAT→.wasm pipeline; LK8 is the LINKER substrate skeleton):
//   * Emits the WebAssembly v1 module preamble:
//       magic   = 0x00 0x61 0x73 0x6d ('\0asm')
//       version = 0x01 0x00 0x00 0x00 (binary format v1, "MVP")
//     per WebAssembly spec §5.1.
//   * No section bodies. Plan 18 (MIR→WAT walker) is what emits real
//     type / import / function / table / memory / global / export /
//     start / element / code / data sections from MIR. The skeleton
//     intentionally does NOT consume `module.functions` — those carry
//     native-ISA bytes from the x86_64 / ARM64 assembler that have no
//     meaning in WASM (WASM is a stack machine, not a register
//     machine, and plan 18 dispatches MIR directly without going
//     through LIR per plan 18 §2.1).
//
// AssembledModule contract under the skeleton: any non-empty
// `module.functions` is a fail-loud signal — it means the caller
// mistakenly routed native-ISA assembler output to the WASM walker.
// The skeleton fires `K_NoMatchingObjectFormat` in that case rather
// than silently shipping a malformed 8-byte module with native bytes
// nowhere visible. Empty `module.functions` is the expected skeleton
// input (LK8 acceptance is "the dispatch routes + the module bytes
// validate as a parseable empty WASM module").
//
// The walker is target-blind: every WASM-specific number it emits
// (magic + version) is fixed by the WebAssembly spec, not a target
// knob. Future plan-18 additions will route through JSON-declared
// section vocabulary (`wasm.format.json`) parallel to how ELF/PE/
// MachO walkers read section/identity declarations today.

namespace dss::wasm {

[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter);

} // namespace dss::wasm
