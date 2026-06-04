#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "link/symbol_kind.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Linker engine (plan 14, LK1–LK10 closed end-to-end 2026-05-30).
//
// FORMAT-BLIND. The engine consumes an `AssembledModule` from the
// assembler (plan 13) plus a `TargetSchema` (for relocation-formula
// lookup) plus an `ObjectFormatSchema` (for format-side reloc names
// and section/symbol-table layout). It applies relocations,
// resolves symbols, and produces a `LinkedImage` — a byte buffer
// laid out per the format's section conventions.
//
// LK4 substrate (the original 2026-05-29 land) shipped: parallel-
// index discipline, derived `ok()`, the K_* diagnostic family
// (substrate-tier — per-format codes land alongside their LK*
// cycles), the format-blind dispatch shell that calls a per-format
// walker. LK1/LK2/LK3 plugged in ELF / PE / Mach-O walkers (both
// relocatable .o/.obj AND executable images); LK6 closed intra-
// module reloc apply + dynamic linking (PE IAT / ELF GOT+PLT /
// Mach-O LC_DYLD_INFO_ONLY) + HIR→AS extern thread-through; LK7
// added codesign placeholders; LK8/LK9 added WASM/SPIR-V skeletons;
// LK10 added file emission (`dss::linker::writeImage`) + driver
// pipeline wiring.
//
// **Still deferred** (anchored as plan 14 §3.1 items):
//   * Cross-CU symbol-table merge. LK11 + CU6 (v1.x).
//   * TLS lowering. LK5 (first TLS-bearing corpus trigger).
//   * Lazy-binding upgrade paths (D-LK6-11/12/13/14 — eager binding
//     ships today via DF_1_NOW / IAT eager / LC_DYLD_INFO_ONLY).
//   * ARM64 reloc-formula reshape (D-LK6-1 — first non-x86_64 arch).

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
    // Count of indexed symbols — meaning depends on the link arity:
    //   * Single-CU (N==1): distinct (cuId, SymbolId) compound keys in the per-module
    //     index (D-LK4-3 — the compound key keeps two CUs' colliding bare SymbolId
    //     distinct; a regression to a bare key collapses them, which the collision pin
    //     asserts via the count + the absence of a false duplicate).
    //   * Cross-CU (N>1, LK11a): the number of resolved global DEFINITIONS in
    //     `resolvedGlobalDefs` after weak-vs-strong resolution.
    // Read by tests; not load-bearing for emission.
    std::size_t               symbolCount = 0;

    // LK11a: the cross-CU symbol-resolution outcome — for each externally-visible
    // NAME defined across the linked CUs, the WINNING definition's compound key after
    // weak-vs-strong resolution (a strong/Global def shadows weak; among all-weak the
    // lowest (cuId, SymbolId) wins deterministically). Empty for single-CU links (no
    // cross-CU merge). Load-bearing for the OPT7 Weak-inline guard: the optimizer must
    // not inline a weak callee whose winning definition here is a DIFFERENT, strong one.
    std::unordered_map<std::string, LinkedSymbolKey> resolvedGlobalDefs;

    // LK11a: a resolved cross-CU REFERENCE — an extern import (a reference) whose name
    // is DEFINED in a sibling CU. A local/sibling definition shadows the extern
    // declaration, so the reference binds to that definition (NOT a DLL import). LK11a
    // records the symbolic edge; LK11b patches the referencing relocations to the
    // definition's address once the merged image is laid out. An extern with NO cross-CU
    // definition stays a real FFI import (resolved via the import table, unchanged).
    struct CrossCuRef {
        LinkedSymbolKey reference;   // (referencing cuId, the extern import's SymbolId)
        LinkedSymbolKey definition;  // the winning sibling-CU definition's compound key
    };
    std::vector<CrossCuRef> resolvedCrossCuRefs;

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
// D-LK4-3 — multi-module entry. The linker indexes every module's symbols under
// its `AssembledModule::cuId` (compound key `(cuId, SymbolId)`) so N CUs' tables
// coexist without per-arena SymbolId collisions. A 1-element span is the single-CU
// path (full image emission, behavior unchanged). N>1 builds the collision-proof
// index + validates, then fail-louds `K_CrossCuMergeUnsupported` — the multi-CU
// image MERGE (cross-CU name resolution + weak-vs-strong) is LK11.
[[nodiscard]] DSS_EXPORT LinkedImage
link(std::span<AssembledModule const> modules,
     TargetSchema const&          targetSchema,
     ObjectFormatSchema const&    objectFormatSchema,
     DiagnosticReporter&          reporter);

// Single-module convenience overload (the v1 single-CU signature). Delegates to
// the span entry with a 1-element span — every existing single-CU caller is
// source-unchanged.
[[nodiscard]] DSS_EXPORT LinkedImage
link(AssembledModule const&       module,
     TargetSchema const&          targetSchema,
     ObjectFormatSchema const&    objectFormatSchema,
     DiagnosticReporter&          reporter);

} // namespace dss::linker
