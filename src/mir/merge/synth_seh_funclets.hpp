#pragma once

// c116 (D-WIN64-SEH-FUNCLETS): the MSVC-x64 SEH funclet-synthesis MIR pass.
//
// The c115 frontend lowers `__try { body } __except (filter) { handler }` to a flat
// CFG with 5 neutral MIR ops:
//   pre:      SehTryBegin(id), succs [tryBB, filterBB]
//   tryBB:    body …; SehTryEnd(id); Br(joinBB)
//   filterBB: filter …; SehFilterReturn(i32, id) → handlerBB
//   handlerBB: handler …; Br(joinBB)
//   joinBB:   continuation
// where the filter reads `SehExceptionCode` / `SehExceptionInfo`.
//
// This pass makes the catch RUNNABLE on x64. For each `__try` region it:
//   * EXTRACTS the filter block into a synthesized FUNCLET MIR function — a normal
//     `ms_x64` function `int filter(void* exceptionPointers, u64 establisher)`.
//     ms_x64 already maps arg0→rcx, arg1→rdx = the exact x64 filter ABI, so NO new
//     calling convention is needed. `SehExceptionInfo` → the funclet's arg0;
//     `SehExceptionCode` → `*(u32*)*(void**)arg0` (EXCEPTION_POINTERS.ExceptionRecord
//     @0 → EXCEPTION_RECORD.ExceptionCode @0); `SehFilterReturn(v)` → `return v`.
//     (c116a: the establisher/arg1 is UNUSED — no parent-local recovery yet; that
//     is H1 / c116b.)
//   * REDUCES the parent's filterBB to a STUB `[Const i32 0; SehFilterReturn(const,
//     id) → handlerBB]`. This preserves the H2 CFG-fiction edge SehFilterReturn →
//     handlerBB (so handlerBB stays FORWARD-reachable = prune-safe, and single-pred
//     = verifier-clean) while removing every `SehException*` read from the parent
//     (they now live ONLY in the funclet, where arg0 genuinely is the exception
//     pointers). The stub emits NO runtime branch at mir_to_lir; the OS dispatches
//     into handlerBB via the scope table's JumpTarget.
//   * KEEPS `SehTryBegin` / `SehTryEnd` as markers in the parent (they emit nothing;
//     the guarded body falls through normally) and records the scope's block range.
//
// The scope records the pass returns (`MirSehScope`, keyed by parent MIR block ids)
// are threaded to `lowerToLir`, which translates the MIR block ids to LIR block ids
// and emits a `SehScopeDescriptor`; `compile_pipeline.cpp` then binds byte offsets
// and attaches a `SehScopeEntry` to the parent's `FrameUnwindInfo.sehScopes`, and
// the pe writer emits the `__C_specific_handler` scope table + UNW_FLAG_EHANDLER.
//
// The `__C_specific_handler` personality is imported ON DEMAND here (its
// `ExternImport` is appended to `externImports`) — NEVER eager-imported via
// windows.json `symbols` (the c101 0xC0000139 loader law); the c111
// `synthesizePeStartup` `__wgetmainargs` synthesis is the precedent.
//
// This pass mirrors the `synthesizePeStartup` structure (MirBuilder rebuild of a
// frozen module + appended synth functions + `cloneGlobalsVerbatim`). It is a
// no-op (fast presence scan) for any module without a `SehTryBegin`.

#include "core/export.hpp"
#include "core/types/extern_import.hpp"       // ExternImport
#include "core/types/strong_ids.hpp"          // SymbolId, MirBlockId
#include "mir/mir_node.hpp"                    // MirBlockId

#include <cstdint>
#include <vector>

namespace dss {

class Mir;
class TypeInterner;
class DiagnosticReporter;

// One SEH scope, keyed by PARENT MIR block ids (translated to LIR block ids by
// `lowerToLir`). `beginBlock` = the guarded body's entry (SehTryBegin's succ[0]);
// `endBlock` = the guarded body's LAST block in the CONTIGUOUS layout the funclet
// pass imposes (== `beginBlock` for a single-block body; the last of the region's
// contiguously-laid-out blocks for a multi-block body — c116b). The scope's half-
// open PC END is the offset of whatever block is laid out immediately AFTER
// `endBlock` (computed at link time from the byte-offset map; the region-contiguity
// reorder guarantees that next block is the FIRST non-region block, so [Begin,End)
// covers exactly the guarded body). `handlerBlock` = the `__except` body (the scope-
// table JumpTarget); `filterFuncletSymbol` = the synthesized funclet.
struct DSS_EXPORT MirSehScope {
    SymbolId   parentFuncSymbol{};      // the function that guards this region
    MirBlockId beginBlock{};            // guarded body entry (SehTryBegin succ[0])
    MirBlockId endBlock{};              // guarded body's LAST block (contiguous layout)
    MirBlockId handlerBlock{};          // the __except handler body
    SymbolId   filterFuncletSymbol{};   // the synthesized filter funclet
    SymbolId   personalitySymbol{};     // __C_specific_handler extern
};

// Synthesize the SEH filter funclets + record the scope ranges. When any function
// in `mir` contains a `SehTryBegin`:
//   * `mir` is REBUILT with each parent's filterBB reduced to a stub + one appended
//     funclet function per `__try` region (Mir is frozen — the shared
//     MirFunctionRebuilder substrate does the clone);
//   * the `__C_specific_handler` personality import is appended to `externImports`;
//   * the returned vector carries one `MirSehScope` per region.
// A module with no `__try` is a clean no-op (returns empty; `mir`/`externImports`
// untouched). Returns false (fail-loud, reported) on an unsupported SEH shape a
// c116a scaffold cannot lower (e.g. a multi-basic-block filter or guarded body —
// D-WIN64-SEH-FUNCLETS; those are the c116b frontier). On the no-op / success path
// returns true and fills `outScopes`.
[[nodiscard]] DSS_EXPORT bool
synthesizeSehFunclets(Mir&                        mir,
                      TypeInterner&               interner,
                      std::vector<ExternImport>&  externImports,
                      std::vector<MirSehScope>&   outScopes,
                      DiagnosticReporter&         reporter);

} // namespace dss
