// OPT7 cycle-3 NON-LEAF inline correctness pin
// (D-OPT7-INLINE-LEGALITY-GATE — recursion-safe non-leaf arm), runtime
// tier.
//
// `helper` is a NON-LEAF callee: its body CALLS `twice`. OPT7 cycle 3
// LIFTS the leaf restriction in the §2.9 gate — a callee whose body
// contains a regular `Call` is now inlinable (the inner Call clones
// correctly via the splice's generic arm; recursion is blocked
// separately by the call-graph SCC gate, and this chain is acyclic).
//
// The `["Inlining"]` optimized arm flattens the whole chain across the
// pipeline's fixed-point iterations:
//   * main calls helper (non-leaf) → helper is inlined into main; its
//     body's call-to-twice is cloned in alongside;
//   * the next iteration inlines that residual call-to-twice → main
//     computes `20 * 2 + 1` entirely inline.
// twice(20) = 40; helper = 40 + 1 = 41 → main returns 41. The operands
// are picked so the result is RUNTIME-MEANINGFUL (not a vacuous fold —
// the `["Inlining"]`-only arm runs no ConstFold; the multiply + add
// execute at runtime).
//
// The optimized arm MUST equal the baseline (41).
//
// RED-on-disable: re-adding the `Call`-leaf refusal in
// inlineLegalityGate refuses the non-leaf `helper` → main's call to
// helper is NOT inlined. The program STILL returns 41 (inlining is a
// correctness-neutral transform), so this corpus is a CORRECTNESS pin
// (optimized arm == baseline), complemented by the MIR-tier
// `NonLeafCalleeIsInlined` pin that asserts the inline actually fires
// (callsInlined / Call-count), whose RED-on-disable is direct.

int twice(int x) {
    return x * 2;
}

int helper(int x) {
    return twice(x) + 1;
}

int main() {
    return helper(20);
}
