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

    // The successor half of the line above, per block — and it is a SEPARATE
    // method because the successor arity is a property of the block's TERMINATOR
    // while the rest of the sweep walks instructions. ⚠ The claim "successor-
    // count in [min,max]" stood in this docblock while NO such check existed
    // anywhere in the verifier (✔MEASURED, cycle P20); it is true now because it
    // was implemented, and the implementation carries the measurement. Also
    // enforces `InlineAsmGoto`'s stronger rule — successors == labels + the
    // fall-through — which the `[min,max]` range cannot express.
    // Emits I_VerifierFailure.
    void checkTerminatorSuccessorArity(DiagnosticReporter& reporter,
                                       MirBlockId block) const;

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

    // TF-C112 (D-MIR-VERIFIER-NO-CALLSITE-SIGNATURE-CHECK): the CALL-SITE
    // signature belt. `checkTypeInvariants` checks an `Arg` against the
    // ENCLOSING function's FnSig; nothing checked a `Call`'s operands against
    // its CALLEE's FnSig, so every hand-built call in every MIR-tier synthesis
    // pass (synth_stdio_shim / synth_threads_shim / synth_pe_startup / anything
    // added later) could pass the wrong number of arguments, or the wrong type
    // at a position, with no tier objecting. The frontend path was already
    // covered — `HirVerifier::checkCallArguments` runs the same arity +
    // per-position rule on every cst_to_hir-produced call — but a pass that
    // emits MIR DIRECTLY bypasses HIR entirely, which is precisely the closure
    // that rule's own comment defers ("when the first non-cst_to_hir producer
    // arrives"). Emits I_CallSignatureMismatch. Interner-gated.
    //
    // P36 (D-MIR-VERIFIER-CALLSITE-RESULT-TYPE-UNCHECKED): it now also checks
    // the Call's RESULT against the callee's declared return, with explicit
    // arms for no-result/`void`, a by-value-class return's ABI piece 0, and a
    // scalar return. That check runs BEFORE the physical-vs-semantic operand
    // gate below, because the gate bails on a by-value-class return and would
    // otherwise silence exactly the struct-return shape the result rule is for.
    //
    // ⚠ THE RULE'S OWN LIMITS — it covers strictly less than "the call is
    // wired correctly", and must not be read as covering more:
    //   * TYPE-BLIND TRANSPOSITION. Two operands whose declared parameter
    //     types are the SAME TypeId are interchangeable to this rule BY
    //     CONSTRUCTION. The bug that motivated it — swapping `buf` and `fmt`
    //     in synthesizeStdioShim's `sprintf` arm, both `char*` at parameters
    //     1 and 3 of one signature — is INVISIBLE here and stays invisible:
    //     only POSITION distinguishes them, and no type check at any tier can
    //     read position. Only a per-body test pin catches that shape.
    //   * SHARED-SIGNATURE CALLEES. `__stdio_common_vsprintf` and
    //     `__stdio_common_vsscanf` deliberately share ONE FnSig TypeId
    //     (synth_stdio_shim.cpp) because their parameter lists are identical,
    //     so mis-wiring a recipe to the wrong one of that pair is a SYMBOL-
    //     level error this rule cannot see — again, only a test can.
    //   * `void*` POINTEE SLACK. A `ptr<void>` on EITHER side matches any
    //     pointer. MEASURED, not assumed: `Mem2Reg` promoting a `va_list ap`
    //     local forwards the `VaHomeArgAreaAddr` leaf (typed `ptr<void>`)
    //     straight into a parameter declared `ptr<i8>`, erasing the pointee
    //     with no retagging Cast — so pointee identity is NOT a MIR invariant
    //     after optimization. Every other pointee mismatch is still rejected.
    //   * ABI-LOWERED OPERAND LISTS. A MIR Call's operand list is PHYSICAL,
    //     not semantic: a by-value aggregate parameter expands to a variable
    //     number of register pieces / carriers, and a by-value-class return
    //     may PREPEND an sret pointer with no marker on the SysV/Win64 hidden-
    //     arg path. Where the 1:1 operand↔parameter correspondence therefore
    //     cannot be established, the call is SKIPPED WHOLE rather than
    //     guessed at (see the gate in the .cpp). Those calls get no coverage.
    void checkCallSignatures(DiagnosticReporter& reporter) const;

    // FC17.9(d) cycle 1b (D-CSUBSET-ATOMIC): the atomic-lowering belt. A plain
    // `Load` (accessed type = its result type) or `Store` (accessed type = the
    // pointee of its address operand) whose accessed type `isAtomicQualified` is a
    // MISSED funnel site — the scalar-access chokepoint must have lowered it to
    // AtomicLoad/AtomicStore; a plain op would silently perform a NON-atomic
    // access. Emits I_AtomicAccessNotLowered. The ONE exemption:
    // `MirInstFlags::AtomicInitExempt` Stores (C11 7.17.2.1 — object
    // initialization is not itself atomic). Interner-gated (needs
    // isAtomicQualified); skipped when `interner_ == nullptr`.
    void checkAtomicAccessLowered(DiagnosticReporter& reporter) const;

    // P36 (D-MIR-VERIFIER-STORE-CALLARG-TYPE-BLIND): the MEMORY-WRITE seam. A
    // `Store`/`AtomicStore`'s VALUE must have the type of the slot it lands in —
    // the pointee of its address operand — under the ONE value↔slot
    // compatibility notion (`sameSlotType` in the .cpp) that the two call rules
    // also use. Until this rule, `checkAtomicAccessLowered` walked every Store
    // and read ONLY the pointee's atomic qualification, so a wrong `TypeId` on a
    // stored value passed SILENTLY: that is how
    // D-CSUBSET-INT128-NARROWING-CAST-SITE-INCOMPLETE hid — its `return` form
    // was diagnosed (`I_TerminatorTypeMismatch`) while the store-shaped form
    // carried the SAME wrong TypeId with no diagnostic at all. Emits
    // I_StoreValueTypeMismatch. Interner-gated.
    //
    // ⚠ THE ONE NARROWING, stated because a rule tuned to its own output
    // asserts nothing: a `ptr<void>` pointee makes NO type claim (it is MIR's
    // spelling for "an address whose pointee is unknown"), so a store through
    // one is not judged. That is the `void*` arm of `sameSlotType` read one
    // level down, and it lives at the seam rather than in the notion because it
    // is a fact about what a `void` pointee MEANS.
    void checkStoreValueTypes(DiagnosticReporter& reporter) const;

    Mir const&          mir_;
    TypeInterner const* interner_;
};

} // namespace dss
