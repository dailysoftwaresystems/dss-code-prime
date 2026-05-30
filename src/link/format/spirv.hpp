#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <vector>

// SPIR-V module writer (LK9 skeleton — plan 14 LK9).
//
// Cycle scope (intentionally minimal — plan 17 §0 owns the full
// MIR→SPIR-V pipeline; LK9 is the LINKER substrate skeleton):
//   * Emits the SPIR-V module header (5 × 32-bit words = 20 bytes):
//       word[0] = 0x07230203   (magic; spec-fixed; the bit pattern
//                               of this word also encodes the
//                               consumer's endianness contract)
//       word[1] = 0x00010600   (version 1.6 — major in bits 16..23,
//                               minor in bits 8..15)
//       word[2] = 0            (generator magic — `0` = unspecified;
//                               plan 17 may pick a Khronos-registered
//                               id once the tool ships)
//       word[3] = 0            (bound — `<id>` upper bound; LK9 has
//                               no ids, so 0)
//       word[4] = 0            (reserved; must be 0 per spec §2.3)
//     per SPIR-V Spec §2.3 (Physical Layout of a SPIR-V Module
//     Binary).
//   * No instruction stream. Plan 17 (MIR→SPIR-V walker) is what
//     emits `OpCapability` / `OpExtension` / `OpMemoryModel` /
//     `OpEntryPoint` / `OpTypeFunction` / `Op*` from MIR. The
//     skeleton intentionally does NOT consume `module.functions`
//     — those carry native-ISA bytes from the x86_64 / ARM64
//     assembler that have no meaning in SPIR-V (typed-value
//     bytecode for a typed value VM; plan 17 dispatches MIR
//     directly without going through LIR per plan 17 §2.5).
//
// AssembledModule contract under the skeleton (mirrors LK8):
// non-empty `module.functions` / `externImports` / non-zero
// `expectedFuncCount` are fail-loud signals. Empty inputs are the
// expected skeleton case.
//
// The walker is target-blind: every SPIR-V word it emits is
// spec-fixed (§2.3). Future plan-17 additions will route through
// JSON-declared opcode + decoration + storage-class vocabulary
// (`spirv.format.json` co-evolves with plan 17 phases).

namespace dss::spirv {

[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
encode(AssembledModule const&    module,
       TargetSchema const&       targetSchema,
       ObjectFormatSchema const& objectFormatSchema,
       DiagnosticReporter&       reporter);

} // namespace dss::spirv
