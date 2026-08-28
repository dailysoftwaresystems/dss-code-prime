// D-OPT1-LICM-CONDITIONAL-PIN (step 13.6 cycle 1, 2026-06-03): the
// subtlest LICM bug class — PARTIALLY-invariant expressions. `a * b`
// LOOKS invariant (both are scalar locals, neither under a top-level
// store in the loop body) but `a` IS conditionally re-assigned in
// the `if (i == 3)` arm. A naive LICM that hoists `a * b` OUT of
// the loop based on "no UNCONDITIONAL store of a or b inside the
// loop" produces wrong-code: every iteration would see the same
// initial `a` (= 3), missing the increment at i=3.
//
// Iteration trace (correct):
//   i=0: a=3, acc=0+3*4=12, i=1
//   i=1: a=3, acc=24, i=2
//   i=2: a=3, acc=36, i=3
//   i=3: a=3+1=4 (the conditional store), acc=36+4*4=52, i=4
//   i=4: a=4, acc=52+16=68, i=5 → exit, return 68
//
// **The trap**: a buggy LICM hoists `a * b` outside the loop using
// `a=3`, yielding acc = 5 * 12 = 60 (= the value of the
// `loop_invariant/` example where `a` is GENUINELY invariant). The
// 8-point exit-code distance (68 vs 60) is bisectable.
//
// Companion to `loop_invariant/` (positive LICM pin): together they
// pin BOTH "LICM fires correctly when invariant" AND "LICM does NOT
// fire when conditional mutation present."

int main() {
    int a;
    int b;
    int i;
    int acc;
    a = 3;
    b = 4;
    acc = 0;
    i = 0;
    while (i < 5) {
        if (i == 3) {
            a = a + 1;
        }
        acc = acc + a * b;
        i = i + 1;
    }
    return acc;
}
