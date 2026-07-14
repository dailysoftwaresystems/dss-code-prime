#pragma once

#include "core/export.hpp"
#include "mir/mir.hpp"

namespace dss {

class DiagnosticReporter;
class TypeInterner;

// `MirVerifier` (ML3) — the structural / dominance / type-consistency
// verifier for a frozen `Mir` module. Mirrors `HirVerifier`'s API +
// discipline:
//
//   - construct over a `Mir const&` (must outlive the verifier);
//   - optional `TypeInterner const*` enables rules that decode `TypeId`
//     (terminator typing, Arg index, no-extension-types-in-MIR); absent
//     ⇒ those rules are skipped (a module built directly in a test
//     fixture, with no semantic phase, has no interner to consult).
//   - one public entry point: `verify(DiagnosticReporter&) → bool`
//     returns true iff THIS run emitted no Error-severity diagnostic
//     (delta on the reporter's error count, so a reporter carrying
//     prior-phase errors doesn't make a clean module look dirty).
//
// COLLECT-ALL discipline: every rule sweeps the whole module; one run
// surfaces every violation. The `Mir`'s build-time + freeze-time
// invariants are still enforced as aborts by `MirBuilder`; this
// verifier is for the deeper layers (CFG predecessors, dominator-tree-
// requiring SSA use-dom-def, FnSig-decoded terminator typing) that
// genuinely need a downstream layer to check — AND for the
// direct-`Mir`-ctor path that bypasses `MirBuilder` (synthetic IR
// in test fixtures, future optimizer-built modules).
//
// Diagnostic codes: `I_*` band (0xA00x). The renderer prints "I0001"
// etc. The verifier emits `Error`-severity diagnostics for every
// violation; node identity (`MirInstId.v` / `MirBlockId.v`) travels
// in `ParseDiagnostic::actual` so the reporter's dedup key (which
// folds in `actual`) never collapses two distinct violations even
// when both lack a source span.
class DSS_EXPORT MirVerifier {
public:
    explicit MirVerifier(Mir const& mir, TypeInterner const* interner = nullptr) noexcept
        : mir_(mir), interner_(interner) {}

    // The verifier stores a reference; rvalue binding is forbidden.
    MirVerifier(Mir&&)                      = delete;
    MirVerifier(Mir&&, TypeInterner const*) = delete;

    // Run every rule, reporting each violation into `reporter`. Returns
    // true iff THIS run emitted no Error-severity diagnostic. The
    // delta-on-errorCount discipline matches HirVerifier.
    [[nodiscard]] bool verify(DiagnosticReporter& reporter) const;

private:
    // Re-run ML1's structural invariants on the frozen module so the
    // direct-`Mir`-ctor path is covered the same way as `MirBuilder`.
    // Checks: opcode-validity, operand-count in `[min,max]`, successor-
    // count in `[min,max]`, result-type rule (`R::Value` ↔ valid typeId),
    // phi-only-uses-phi-pool, Const.payload in literal-pool range.
    // Emits I_VerifierFailure.
    void checkStructuralInvariants(DiagnosticReporter& reporter) const;

    // Each function has exactly one block marked `StructCfMarker::EntryBlock`
    // AND that block is `funcBlockAt(f, 0)`. Emits I_NoEntryBlock /
    // I_MultipleEntryBlocks / I_EntryBlockNotFirst.
    void checkEntryBlocks(DiagnosticReporter& reporter) const;

    // Every block's last instruction is a terminator opcode. (ML1
    // already aborts on build-time violations; this re-checks for the
    // direct-ctor path.) Emits I_BlockNotTerminated.
    void checkBlockTermination(DiagnosticReporter& reporter) const;

    // Every Phi's incoming.pred must be a CFG-predecessor of the phi's
    // enclosing block. Computes predecessors in O(E) by inverting
    // `blockSuccessors`. Emits I_PhiPredNotInCfg.
    void checkPhiIncomings(DiagnosticReporter& reporter) const;

    // c115 SEH (D-WIN64-SEH-FUNCLETS): the region-skeleton pairing rules —
    // filter (SehTryBegin succ[1]) single-pred + SehFilterReturn-terminated
    // with the matching region payload; handler single-pred; SehTryEnd payload
    // names an existing region; SehExceptionCode/Info only in SEH functions.
    // Emits I_SehStructure. Zero-cost when the module has no SehTryBegin.
    void checkSehStructure(DiagnosticReporter& reporter) const;

    // VLA C5 (D-CSUBSET-VLA): the block-scope stack-teardown pairing rule —
    // every `StackRestore`'s operand[0] must be a `StackSave`, and its scopeId
    // payload must equal that StackSave's payload. The generic SSA dominance check
    // (checkDomination) already enforces that the StackSave dominates each restore
    // (it is a value operand); this adds the STRUCTURAL pairing the flat IR cannot
    // otherwise express. NOT a coverage claim ("every exit edge covered") — the
    // flattened CFG can't support that; pairing + dominance is the provable check
    // (audit fix #6). Emits I_VlaStackRestorePairing. Zero-cost when no StackSave.
    void checkVlaStackTeardown(DiagnosticReporter& reporter) const;

    // SSA invariant: every value operand is defined in a block that
    // DOMINATES the use site (or in the same block, with the def
    // preceding the use). Computes dominator tree via Cooper-Harvey-
    // Kennedy iterative algorithm per-function. Emits I_NotDominated.
    //
    // ALSO hosts the StructCfMarker EQUALITY check (it shares the
    // per-function preds/RPO/dom computation): every REACHABLE block's
    // stored marker must equal the canonical CFG derivation
    // (`deriveStructCfMarkers`, recomputed INDEPENDENTLY here — the
    // verifier never trusts a producer-supplied vector). Emits one
    // I_StructCfMismatch per mismatching block, naming stored +
    // derived. Replaced the pre-derivation count-parity model
    // (IfThen/IfJoin pairing counts, the ExitBlock-terminator rule,
    // the LoopHeader-back-edge rule) — all subsumed by equality.
    void checkDomination(DiagnosticReporter& reporter) const;

    // Interner-gated rules — skipped when `interner_ == nullptr`:
    //   - CondBr.condition is Bool
    //   - Return value's type matches enclosing function's FnSig return
    //   - Arg.argIndex < FnSig.paramCount
    //   - No instruction's typeId resolves to TypeKind::Extension
    // Emits I_TerminatorTypeMismatch / I_ArgIndexOutOfRange /
    // I_ExtensionTypeInMir.
    void checkTypeInvariants(DiagnosticReporter& reporter) const;

    Mir const&          mir_;
    TypeInterner const* interner_;
};

} // namespace dss
