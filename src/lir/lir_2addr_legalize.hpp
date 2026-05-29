#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <cstddef>

// 2-address legalize pass — plan 13 AS3.
//
// Post-regalloc, pre-assembly pass that normalizes 3-address LIR
// instructions into the 2-address shape some targets require. For
// every instruction whose target opcode declares
// `requires2Address == true`, the pass ensures
// `instResult == instOperands[0].reg`; when they differ, it inserts
// a synthetic `mov instResult, instOperands[0].reg` before the
// instruction and rewrites the instruction's `operands[0]` to be
// the result register.
//
// x86's reg-reg arithmetic needs this (REX.W 0x03 /r writes into the
// ModR/M.rm operand, so the dest IS one of the sources); ARM64's
// reg-reg arithmetic is 3-address natively and leaves
// `requires2Address == false` per-opcode.
//
// The pass is **target-blind**: it never branches on `schema.name()`.
// Per-opcode `requires2Address` is the only signal. Same discipline
// as `lir_rewrite.cpp` (post-regalloc vreg→phys substitution) and
// `lir_callconv.cpp` (prologue/epilogue materialization) — a pure
// rewrite-pass over `Lir` consuming `TargetSchema` for opcode
// metadata and producing a fresh `Lir` module.

namespace dss {

struct DSS_EXPORT LirTwoAddrLegalizeResult {
    Lir         lir{};
    // Number of LIR functions the pass STARTED rewriting. Mirrors
    // `AssembledModule::expectedFuncCount` / `LirCallconvResult::
    // perFunc.size()`. Used to verify the output's parallel-index
    // discipline survived the rewrite.
    std::size_t expectedFuncCount = 0;
    // True iff every function the pass began was rewritten without a
    // schema-config failure (e.g. missing `mov` opcode). A false here
    // means downstream MUST NOT consume `lir` for assembly — the
    // legalize invariant (`result == operands[0]` for every
    // `requires2Address` opcode) is NOT guaranteed to hold.
    bool        allFunctionsLegalized = false;

    // Mirrors `LirCallconvResult::ok()` discipline: shape-consistency
    // is the success channel. Empty input legitimately produces
    // `ok() == false` (no functions to legalize → caller distinguishes
    // via expectedFuncCount).
    [[nodiscard]] bool ok() const noexcept {
        return expectedFuncCount > 0
            && lir.moduleFuncCount() == expectedFuncCount
            && allFunctionsLegalized;
    }
};

[[nodiscard]] DSS_EXPORT LirTwoAddrLegalizeResult
legalizeTwoAddress(Lir const&          src,
                   TargetSchema const& schema,
                   DiagnosticReporter& reporter);

} // namespace dss
