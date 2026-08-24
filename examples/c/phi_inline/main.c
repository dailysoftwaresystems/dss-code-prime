// OPT7 callee-Phi inline correctness pin
// (D-OPT7-MULTIBLOCK-SPLICE-PHI), runtime tier.
//
// `pick` is a value-producing ternary whose CONDITION is the ARGUMENT:
// `return k ? 7 : 9;`. A value-producing `?:` lowers to a MIR `Phi` at
// the merge BEFORE Mem2Reg (hir_to_mir), so — with an `[Inlining]`-only
// optimized arm (Inlining runs before Mem2Reg) — the callee `pick`
// carries a REAL `Phi` when the inliner sees it. The OLD inliner refused
// any Phi-bearing callee; this cycle's deferred callee-phi flush clones
// it: `pick`'s blocks are cloned into `main`, the join `Phi` is recloned
// (placeholder + incomings remapped through the value/block maps), and
// the post-inline ConstFold over the now-constant condition (k == 0)
// reads the CLONED wiring → picks the `9` arm.
//
// CRITICAL: the condition MUST be argument-keyed, NEVER a compile-time
// constant in the callee. A constant-condition callee gets its Phi
// folded BEFORE the inliner sees it → the clone is vacuous. With the
// arg-keyed callee, the inliner clones a real Phi; main calls `pick(0)`
// so the post-inline fold reads the cloned Phi: exit 9 IFF the incomings
// are correctly value<->pred paired, 7 IFF transposed.
//
// main calls `pick(0)` → 9. The `Inlining` optimized arm MUST equal the
// baseline (9).
//
// RED-on-disable: transpose the cloned Phi's two incomings (7<->9) and
// `pick(0)` folds to 7 → the examples-runner differential ASSERT
// (optimized arm exit == baseline exit == 9) fires. (Demonstrated in the
// cycle gate by transposing the clone, then restoring.)

int pick(int k) {
    return k ? 7 : 9;
}

int main() {
    return pick(0);
}
