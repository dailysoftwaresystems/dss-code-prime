#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "mir/mir.hpp"

#include <memory>

// MIR → LIR instruction selection (plan 12 ML5 cycle 3). Takes a frozen
// MIR module plus the chosen `TargetSchema` (the cycle-2b-shaped JSON
// descriptor) and produces a frozen `Lir` module: the SAME function/block
// CFG topology, but per-block MIR instructions rewritten as per-target LIR
// instructions. Block-for-block 1:1 (no block splitting at this layer);
// MIR values map to fresh LIR virtual registers per-function. Physical
// register assignment + spilling happen later in ML6 regalloc.
//
// First consumer of the cycle-2b `TargetSchema` opcode vocabulary. All
// opcode dispatch goes through `schema.opcodeByMnemonic("add")` etc.;
// nothing in the lowerer hardcodes processor names. Adding ARM64 = drop
// `arm64.target.json` declaring `arg`/`mov`/`add`/`sub`/`mul`/`ret`
// mnemonics; the lowerer is target-blind. (The register-file + calling-
// convention sections of `TargetSchema` are the next-tier consumers —
// ML6 regalloc + ML7 callconv lowering. Cycle 3a does not yet read them.)
//
// Cycle 3a scope (this revision): straight-line vertical slice — Function +
// Block + Arg + Const + Add + Sub + Mul + Return. Control flow (Br/CondBr/
// Switch), comparison (ICmp*/FCmp*), memory (Alloca/Load/Store/Gep),
// calls (Call/IntrinsicCall/GlobalAddr), phi, casts, and aggregate ops
// are deliberately fail-loud-deferred via `L_UnsupportedLoweringForOpcode`
// — same discipline as ML2 cycle 1's HIR→MIR vertical slice.

namespace dss {

// Output of MIR→LIR lowering. `ok` mirrors ML2's delta-on-errorCount —
// `true` iff this lowering pass added no new error-severity diagnostics.
// `lir` is the frozen module the assembler (AS1 onward) will consume.
struct DSS_EXPORT MirToLirResult {
    Lir  lir;
    bool ok = true;
};

// Lower the frozen `mir` module to LIR, dispatched against `target`.
// Diagnostics are emitted into `reporter`; unsupported opcodes produce
// `L_UnsupportedLoweringForOpcode` and the lowerer seals the affected
// block with a `ret` terminator so `LirBuilder::finish()` does not abort
// in error paths.
//
// Threading: single-pass, single-threaded, no global state. The caller
// owns `mir` + `target` + `reporter`; the returned `Lir` is move-owned.
[[nodiscard]] DSS_EXPORT MirToLirResult
lowerToLir(Mir const&          mir,
           TargetSchema const& target,
           DiagnosticReporter& reporter);

} // namespace dss
