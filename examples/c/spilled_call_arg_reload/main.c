/* c77 (D-AS-REGALLOC-DIRECT-ARG-RELOAD): the func-2088 shape.
 * A DIRECT call with 6 GPR register-args whose values are live across a
 * high-pressure loop, so linear-scan SPILLS them; at the call each spilled
 * arg must be reloaded into its ABI arg register. c76's wide-call
 * materialization only stacks args BEYOND the register pool — it does not
 * touch these IN-POOL args. WITHOUT the c77 fix, the rewriter reloads each
 * spilled arg into a SCRATCH register, but SysV has only 3 non-arg
 * caller-saved GPR scratch (rax/r10/r11) while up to 6 GPR args can spill ->
 * exhaustion at the 4th -> error[L_VirtualRegInPostRegalloc] (the exact
 * sqlite func-2088 blocker). WITH the fix, a spilled call-arg loads DIRECTLY
 * into its ABI arg register (skip the scratch -> demand == supply, 0 scratch
 * for call args).
 *
 * `weighted` is self-recursive (the `a < 0` guard never fires for our
 * inputs) so the release Inliner leaves it a REAL direct call (func 2088 is
 * a non-inlined direct call). Distinct decimal weights (1,10,...,100000)
 * make a WRONG reload change the exit code -> a silent mis-reload is caught,
 * not just a crash. The exhaustion is a Mem2Reg-SSA-spill phenomenon, so the
 * red-on-disable manifests under the RELEASE pipeline (in debug the locals
 * stay in memory and never spill); the release arm is the load-bearing
 * witness. arm64 (~30 GPRs) never spills -> value-checks the same. => 227. */

int weighted(int a, int b, int c, int d, int e, int f) {
    if (a < 0)
        return weighted(a + 1, b, c, d, e, f);   /* non-inlinable guard */
    return a * 1 + b * 10 + c * 100 + d * 1000 + e * 10000 + f * 100000;
}

int driver(int seed, int n) {
    int a0 = seed + 1, a1 = seed + 2, a2 = seed + 3;
    int a3 = seed + 4, a4 = seed + 5, a5 = seed + 6;

    int s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
    for (int i = 0; i < n; i++) {   /* high pressure: a0..a5 live across it */
        s0 += i * 3;
        s1 += i * 5 + s0;
        s2 += i * 7 + s1;
        s3 += i * 11 + s2;
        s4 += i * 13 + s3;
        s5 += i * 17 + s4;
        s6 += i * 19 + s5;
        s7 += i * 23 + s6;
    }

    int r = weighted(a0, a1, a2, a3, a4, a5);   /* 6 spilled GPR args */

    return r + s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7
             + a0 + a1 + a2 + a3 + a4 + a5;
}

int main(void) {
    return driver(3, 4) & 0xFF;   /* 992483 & 0xFF = 227 */
}
