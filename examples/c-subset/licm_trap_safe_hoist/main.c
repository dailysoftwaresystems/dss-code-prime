// D-OPT6-LICM-TRAP-SAFE-HOIST runtime negative pin (cycle 11, 2026-06-04):
// the end-to-end miscompile-pin that HARD-STOPPED cycle 10j — now
// constructible because Arc A (cycles 10p-10r) landed observable integer
// division codegen (idiv raises #DE = STATUS_INTEGER_DIVIDE_BY_ZERO at
// runtime).
//
// `num / d` inside loop_div's body is LOOP-INVARIANT and unconditionally
// dominates the loop body. The ONLY reason a correct LICM must NOT hoist it
// to the preheader is that SDiv is TRAP-ELIGIBLE. isTrapEligible() in
// src/opt/passes/licm.cpp gates exactly this case.
//
// `num` is a NAMED LOCAL assigned in the entry block (num = 100), NOT an
// inline literal: that is load-bearing. An inline `100 / d` would leave the
// Const(100) MIR node INSIDE the loop body, so the SDiv would have a
// loop-internal operand and never be invariant — LICM would skip it gate or
// no gate, making this pin VACUOUS (verified: with the literal form the pin
// passed even after isTrapEligible was removed). Assigning `num` in the
// entry block makes both SDiv operands (num, d) loop-external — a genuine
// hoist candidate that ONLY the trap-eligible gate blocks. This mirrors the
// loop_invariant/ positive pin (named locals a, b feeding `a * b`).
//
// The trap: main calls loop_div(0, 0) — n=0 so the loop runs ZERO times
// (the SDiv never executes -> return 0), and d=0 so the division WOULD
// divide-by-zero IF it ran. The preheader executes UNCONDITIONALLY even at
// a 0 trip count, so a LICM that hoisted `num / d` into the preheader would
// run 100/0 once -> #DE -> abnormal exit. Correct LICM keeps the SDiv in the
// loop body -> it never runs -> clean exit 0. crash-vs-0 is maximally
// bisectable.
//
// `n` and `d` are PARAMETERS (not literals): no pipeline pass can prove the
// loop is 0-trip and delete it, nor const-fold `num / d` (d is opaque inside
// loop_div). Without OPT7 inlining, the loop_div(0, 0) call-site values do
// not propagate into the body. So the SDiv survives intact to LICM in every
// arm (including full-release-like).

int loop_div(int n, int d) {
    int acc;
    int i;
    int num;
    acc = 0;
    i = 0;
    num = 100;
    while (i < n) {
        acc = acc + num / d;
        i = i + 1;
    }
    return acc;
}

int main() {
    return loop_div(0, 0);
}
