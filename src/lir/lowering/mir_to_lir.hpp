#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "mir/mir.hpp"

#include <memory>
#include <vector>

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
//
// `lirToMir` is a substrate-tier reverse-mapping (LirInstId → MirInstId)
// the lowerer populates as it emits LIR instructions. The vector is
// indexed by `LirInstId.v` (slot 0 is the arena's invalid sentinel,
// already-default-`InvalidMirInst`-initialized). Multiple LIR insts
// may map to the same source MIR inst (cycle-3b Switch lowering emits
// 2N+1 LIR blocks per MIR Switch; cycle-3c memory ops emit Load
// followed by additional address-mode insts); some LIR insts have no
// MIR counterpart (Switch's "next-compare" blocks, phi-edge parallel-
// copy moves), in which case the entry is the default `InvalidMirInst`.
//
// `LirVerifier` consumes this mapping to cross-reference LIR vreg
// classes against MIR types WITHOUT the cycle-3e positional-alignment
// hazard that silently skipped switch-bearing functions.
struct DSS_EXPORT MirToLirResult {
    Lir                    lir;
    std::vector<MirInstId> lirToMir;
    // Extern symbol descriptors propagated from `HirToMirResult.
    // externImports` (LK6 cycle 2d — D-LK6-6 closure). LIR does
    // not consume these structurally (call sites carry SymbolRef
    // operands keyed by SymbolId, the same shape as intra-module
    // calls), but the assembler needs them to populate
    // `AssembledModule.externImports`, so we propagate verbatim.
    std::vector<ExternImport> externImports;
    bool                   ok = true;
};

// Lower the frozen `mir` module to LIR, dispatched against `target`.
// Diagnostics are emitted into `reporter`; unsupported opcodes produce
// `L_UnsupportedLoweringForOpcode` and the lowerer seals the affected
// block with a `ret` terminator so `LirBuilder::finish()` does not abort
// in error paths.
//
// `interner` is the CU's type interner — cycle 3d uses it to classify
// MIR values into `LirRegClass` (Float/Double → FPR, everything else
// → GPR), driving correct vreg allocation for float arithmetic, float
// casts, and cross-class Bitcast. Read-only here (the lowerer never
// mints new types); matches the pattern in `lowerToMir` from ML2.
//
// Threading: single-pass, single-threaded, no global state. The caller
// owns `mir` + `target` + `interner` + `reporter`; the returned `Lir`
// is move-owned.
[[nodiscard]] DSS_EXPORT MirToLirResult
lowerToLir(Mir const&          mir,
           TargetSchema const& target,
           TypeInterner const& interner,
           DiagnosticReporter& reporter,
           // Extern symbols extracted by HIR→MIR lowering
           // (`HirToMirResult.externImports`). Propagated verbatim
           // through the returned `MirToLirResult.externImports`;
           // the LIR lowerer itself does not consume them
           // structurally. Defaults to empty for static modules
           // (LK6 cycle 2d — D-LK6-6 closure).
           std::vector<ExternImport> externImports = {});

} // namespace dss
