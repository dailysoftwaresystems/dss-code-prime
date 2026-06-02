#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_regalloc.hpp"

#include <cstdint>
#include <vector>

// LIR calling-convention lowering pass (ML7). Consumes a post-
// regalloc `Lir` module (every vreg replaced by a physical register,
// spills materialized as `frame_load`/`frame_store` pseudo-ops) plus
// the matching `LirAllocation` side-table, and produces a fresh
// `Lir` module where:
//
//   * Each function carries a PROLOGUE prepended to its entry block:
//     callee-saved registers (the subset actually used by the
//     function) are stored to the saved-reg area; the stack pointer
//     is adjusted by the total frame size.
//   * Each return site carries an EPILOGUE inserted immediately
//     before the return: the stack pointer is restored; callee-saved
//     registers are reloaded.
//   * `frame_load` pseudo-ops become `load result, [SP + slotOffset]`.
//   * `frame_store` pseudo-ops become `store value, [SP + slotOffset]`.
//
// **Frame layout (target-blind, D-ML7-2.2 closure 2026-06-02)**:
//   [SP+0 .. SP+outgoingArgAreaSize)              outgoing-args area
//                                                  — THIS fn's reserved
//                                                  space for ITS calls.
//                                                  Encompasses BOTH the
//                                                  callee's shadow space
//                                                  (Win64=32, SysV=0) at
//                                                  [SP+0..shadowSpaceBytes)
//                                                  AND any explicit
//                                                  stack-arg overflow at
//                                                  [SP+shadowSpaceBytes..).
//                                                  Zero on leaf fns (no
//                                                  calls means no callee
//                                                  to home args for).
//   [SP+outgoingArgAreaSize
//      .. SP+outgoingArgAreaSize+savedRegAreaSize) saved callee-saved regs
//   [SP+outgoingArgAreaSize+savedRegAreaSize ..)   spill slots
//   [SP+totalFrameSize)                            the original pre-prologue SP
//
// `outgoingArgAreaSize = hasCalls ? (cc.shadowSpaceBytes +
//    max_overflow_slots * slotSize) : 0`. Under slot-aligned cc
// (Win64 ms_x64) max_overflow_slots = max(0, max_args_across_calls -
// max(argGprs, argFprs)). Under independent-counters (SysV/AAPCS64)
// it's the per-class overflow sum across calls.
//
// `totalFrameSize = hasCalls
//    ? alignedSizeWithBias(rawPreShadow, cc.stackAlignment,
//                          cc.callPushBytes)
//    : alignUp(rawPreShadow, cc.stackAlignment)`
// where `rawPreShadow = outgoingArgAreaSize + savedRegAreaSize +
// spillAreaSize`. The Win64 shadow-space requirement collapses INTO
// outgoingArgAreaSize (no separate max() with shadowSpaceBytes —
// it's already there).
//
// Spill slot N is at offset `outgoingArgAreaSize + savedRegAreaSize +
// N * regWidth`. `regWidth` is the cc's primary integer register
// width (8 bytes on x86_64/ARM64).
//
// **Target-blind output**: prologue/epilogue use the schema's
// `load`/`store`/`add`/`sub` opcodes (resolved by mnemonic) plus the
// existing 3-operand memory addressing. Each saved register's class
// is read from `schema.registerInfo(ordinal)->regClass` so an FPR
// callee-save (e.g. MS-x64 xmm6..xmm15) materializes with FPR class,
// not silently as GPR. Push/pop optimization (target-specific) is
// deferred to the assembler tier — see plan 13 AS1.
//
// **Reads**:
//   * `TargetSchema.callingConventions[alloc.callingConventionIndex]`
//     for the cc's `calleeSaved` set, `stackPointer.ordinal`,
//     `stackAlignment`.
//   * `TargetSchema.frameLoadMnemonic()` / `frameStoreMnemonic()` to
//     locate the pseudo-ops being materialized.
//   * The schema's `opcodeByMnemonic` for `"mov"`, `"add"`, `"sub"`,
//     `"ret"`.
//
// **Failure modes**: the result's `ok()` returns false iff the output
// module is non-empty AND every function got a layout (matches the
// cycle-3a `LirAllocation::ok()` discipline — derived, not stored).
// Possible diagnostic emission paths:
//   * Required opcode missing from the schema → `L_RequiredLirOpcodeMissing`.
//   * `widthForClass` returns 0 (no GPR/FPR width declared) →
//     `L_RequiredLirOpcodeMissing` from `computeFrameLayout`.
//   * Stack-pointer register not declared on the cc → caught at
//     schema-load time by `validate()` for register-machine ABIs
//     (also defensively re-checked here as
//     `L_RequiredLirOpcodeMissing`).
//   * `alloc.perFunc.size() != src.moduleFuncCount()` →
//     `L_VirtualRegInPostRegalloc`.
//   * `funcAlloc.originalSymbol` doesn't match the rewritten
//     function's symbol → `L_VirtualRegInPostRegalloc` (the
//     parallel-index structural guard).
//   * Per-function allocation failed (`funcAlloc.ok == false`) →
//     `L_VirtualRegInPostRegalloc`.
//   * Calling-convention index out of range →
//     `R_CallingConventionLookupFailed`.
//   * Malformed `frame_store` operands or > 2-successor terminator →
//     `L_UnsupportedLoweringForOpcode`.

namespace dss {

// D-LK10-ENTRY-TRAMP-PROLOGUE: smallest non-negative integer `N`
// satisfying BOTH `N >= rawBytes` AND `N ≡ entryBias (mod
// stackAlignment)`. This is the single formula that decides how
// many bytes a CALL-MAKING function must subtract from its stack
// pointer at entry to (a) reserve at least `rawBytes` of frame
// space and (b) land the call site at the cc's stack-alignment.
//
// Consumers:
//   * Trampoline emitter (`src/link/entry_trampoline.cpp`): passes
//     `rawBytes = cc.shadowSpaceBytes`, `entryBias =
//     cc.entryStackPointerBias`. Result: 40 on Win64 (32 shadow +
//     8 realign), 0 on SysV ELF / Mach-O / ARM64.
//   * ML7 callconv lowering (`lir_callconv.cpp::computeFrameLayout`):
//     anchored D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY for when normal-
//     function call-site shadow-space tightening lands (today ML7
//     calls `alignUp(savedRegs + spill, alignment)` directly, which
//     is `alignedSizeWithBias(... , bias=0)` for non-call-makers
//     after callconv-pass-injected call-site shadow allocates
//     separately).
//
// Algorithm: clamp `rawBytes` up to `entryBias`, then add whole
// `stackAlignment` quanta until the congruence holds. Closed-form
// version computes the modular delta in one step.
//
// Pre-conditions (caller's responsibility — validator at schema-
// load time enforces these for cc fields):
//   * `stackAlignment` is a non-zero power of two.
//   * `entryBias < stackAlignment` (bias is an offset INTO the
//     quantum, not a multiple of it).
//
// `stackAlignment == 0` returns `rawBytes` verbatim (degenerate
// case for non-register-machine targets).
[[nodiscard]] constexpr std::uint32_t
alignedSizeWithBias(std::uint32_t rawBytes,
                    std::uint32_t stackAlignment,
                    std::uint32_t entryBias) noexcept {
    if (stackAlignment == 0) return rawBytes;
    std::uint32_t const remainder = rawBytes % stackAlignment;
    if (remainder == entryBias) return rawBytes;
    // delta = (entryBias - remainder) mod stackAlignment, computed
    // in unsigned arithmetic so wrap is well-defined.
    std::uint32_t const delta =
        (entryBias + stackAlignment - remainder) % stackAlignment;
    return rawBytes + delta;
}

// Per-function frame layout computed before emission. Stored so
// downstream passes (AS1 unwind info, debug-info DWARF .debug_frame
// generation) can read it without re-computing.
//
// `savedRegs` are typed `LirReg` (isPhysical=1, class carried per
// entry) so prologue/epilogue emission picks the correct store/load
// opcode per class. A bare `uint16_t` ordinal would silently
// misclassify FPR/VR callee-saves (MS-x64's xmm6..xmm15) as GPR.
struct DSS_EXPORT FrameLayout {
    std::uint32_t       totalFrameSize    = 0;  // bytes the prologue subtracts from SP
    std::uint32_t       outgoingArgAreaSize = 0; // bytes reserved at [SP+0..) for THIS function's outgoing stack args
    std::uint32_t       savedRegAreaSize  = 0;  // bytes occupied by callee-saved regs
    std::uint32_t       spillAreaSize     = 0;  // bytes occupied by spill slots
    std::uint32_t       slotSize          = 0;  // uniform per-class spill-slot width (bytes; = max(GPR width, FPR width))
    std::uint32_t       outgoingSlotSize  = 0;  // outgoing-arg slot width (bytes; = pointer width = GPR width)
    std::vector<LirReg> savedRegs;              // callee-saved phys regs actually used
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY: did this function contain at
    // least one call-shaped opcode (per `TargetOpcodeInfo::isCall`)
    // at frame-layout time? True ⇒ totalFrameSize incorporates
    // `cc.shadowSpaceBytes` AND satisfies `N ≡ cc.callPushBytes mod
    // cc.stackAlignment`. False ⇒ totalFrameSize is the existing
    // `alignUp(raw, cc.stackAlignment)` (leaf-fn rule). Exposed so
    // downstream consumers (debug-info unwind, audit tests, future
    // CFI emitters) can verify the invariant without re-scanning
    // the source LIR.
    bool                hasCalls          = false;

    // Derived: saved-reg area starts immediately after the outgoing-
    // args area. Updated by D-ML7-2.2 closure (2026-06-02) — the
    // outgoing area is the new SP+0 zone for stack-arg overflow on
    // ANY cc that overflows its argGprs/argFprs pool. Zero when this
    // function makes no calls or every call fits in the register
    // pool — backward-compatible with leaf-fn / register-only-call
    // shapes.
    [[nodiscard]] constexpr std::uint32_t
    savedRegAreaOffset() const noexcept { return outgoingArgAreaSize; }

    // Derived: spill area starts immediately after the saved-reg area
    // (which itself starts after the outgoing-args area).
    [[nodiscard]] constexpr std::uint32_t
    spillAreaOffset() const noexcept {
        return outgoingArgAreaSize + savedRegAreaSize;
    }
};

struct DSS_EXPORT LirCallconvResult {
    Lir                       lir{};
    std::vector<FrameLayout>  perFunc;  // 1:1 with src.funcAt(i)

    // Derived: true iff the output module is non-empty AND every
    // function got a layout. Matches cycle-3a's `LirAllocation::ok()`
    // discipline (computed on access; cannot drift from per-function
    // results).
    [[nodiscard]] bool ok() const noexcept {
        return lir.moduleFuncCount() > 0
            && perFunc.size() == lir.moduleFuncCount();
    }

    // Find the FrameLayout for the given function by position in
    // the OUTPUT module. Returns nullptr if the function index is
    // out of range. (LirFuncId arena tags differ across passes, so
    // positional lookup is the substrate-tier contract.)
    [[nodiscard]] FrameLayout const* forFuncByIndex(std::uint32_t i) const noexcept {
        return (i < perFunc.size()) ? &perFunc[i] : nullptr;
    }
};

[[nodiscard]] DSS_EXPORT LirCallconvResult
materializeCallingConvention(Lir const&           src,
                             TargetSchema const&  schema,
                             LirAllocation const& alloc,
                             DiagnosticReporter&  reporter);

} // namespace dss
