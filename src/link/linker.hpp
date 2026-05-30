#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// Linker engine (plan 14 LK4 substrate).
//
// FORMAT-BLIND. The engine consumes an `AssembledModule` from the
// assembler (plan 13) plus a `TargetSchema` (for relocation-formula
// lookup) plus an `ObjectFormatSchema` (for format-side reloc names
// and section/symbol-table layout). It applies relocations,
// resolves symbols, and produces a `LinkedImage` — a byte buffer
// laid out per the format's section conventions.
//
// LK4 substrate ships: parallel-index discipline, derived `ok()`,
// the K_* diagnostic family (substrate-tier — per-format codes
// land alongside their LK* cycles), the format-blind dispatch
// shell that calls a per-format walker (none registered yet —
// LK1/LK2/LK3 plug in).
//
// **Out of cycle scope** (anchored as plan 14 §3.1 deferred items):
//   * Per-format walkers (ELF / PE / Mach-O). LK1-LK3.
//   * Symbol-table merge across multiple CUs. LK11 + CU6.
//   * TLS lowering. LK5.
//   * Dynamic linking + imports. LK6.
//   * Build-id placement. Part of LK1-LK3.

namespace dss {

// One linked image. Byte buffer + which output format produced it.
// LK4 substrate produces an empty buffer (no format walker plugged
// in yet); the shape is here so plan 14 cycles 1+ extend it.
struct DSS_EXPORT LinkedImage {
    ObjectFormatKind          format = ObjectFormatKind::Unknown;
    std::vector<std::uint8_t> bytes;
    // The number of `AssembledFunction` inputs the engine
    // STARTED processing. Mirrors `AssembledModule::expected
    // FuncCount` / `LirCallconvResult::perFunc.size()` —
    // parallel-index discipline. `ok()` checks this against
    // the per-function-resolved count.
    std::size_t               expectedFuncCount = 0;
    std::size_t               resolvedFuncCount = 0;

    [[nodiscard]] bool ok() const noexcept {
        return expectedFuncCount > 0
            && resolvedFuncCount == expectedFuncCount;
    }
};

} // namespace dss

namespace dss::linker {

// Format-blind linker entrypoint. Same target-blind shape that
// `assemble()` mirrors from ML5 cycle 2a's pivot.
//
// Lives in `dss::linker` (alongside `writeImage`) — D-LK9-2 closed
// at LK10 cycle 2 (was historically `dss::link` until cycle 1's
// writer pre-positioned the namespace).
//
// **Behavior**:
//   * Walks every relocation in every `AssembledFunction`.
//   * Verifies each `Relocation::kind` resolves via
//     `targetSchema.relocationInfo(kind)` AND via
//     `objectFormatSchema.relocationByKind(kind)`. A mismatch on
//     either side emits `K_RelocationKindMismatch`.
//   * Reports `K_SymbolUndefined` when a `Relocation::target`
//     symbol is not declared by any `AssembledFunction` in the
//     module. (v1 is single-CU; cross-CU resolution is LK11; FFI
//     import resolution is LK6.)
//   * Dispatches the per-format byte-emission walker via a
//     closed-enum switch over `ObjectFormatKind`. LK1 plugged in
//     ELF (`src/link/format/elf.cpp`); PE / Mach-O / WASM / SPIR-V
//     arms still fire `K_NoMatchingObjectFormat` until their
//     walkers land (LK2 / LK3 / plan 18 / plan 17).
//
// Returns a `LinkedImage` whose `ok()` reflects parallel-index
// shape. The reporter is the success channel for per-relocation
// failures.
[[nodiscard]] DSS_EXPORT LinkedImage
link(AssembledModule const&       module,
     TargetSchema const&          targetSchema,
     ObjectFormatSchema const&    objectFormatSchema,
     DiagnosticReporter&          reporter);

} // namespace dss::linker
