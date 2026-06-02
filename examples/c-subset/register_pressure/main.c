// D-CSUBSET-BINOP-RIGHT-CLOBBER register-pressure pin.
//
// The regalloc-tier fix EXCLUDES operand[1..N]'s physical registers
// from the candidate set when allocating a `requires2Address`
// result. This is a correctness-over-density trade — it deliberately
// gives up a possible register reuse to prevent the clobber bug.
// User concern (2026-06-02): "watch that this exclusion doesn't
// cause spurious spills under tight pressure."
//
// This example sustains 7 simultaneously-live module globals + a
// 7-operand additive chain. The MIR produces ~14 vregs (one
// lea+load pair per global) plus the running accumulator. Each
// `+` is a `requires2Address` add — exercising the exclusion at
// EVERY join site, NOT just at one. The MS x64 callee-saved +
// caller-saved GPR pool has plenty of headroom for this width,
// so the expected behavior is: regalloc succeeds without falling
// through to the spill path, AND the spill-path's exclusion-aware
// guard (silent-failure HIGH-1 fold) is reachable but inert.
//
// Sum: 1+2+3+4+5+6+7 = 28; + 14 = 42. Always-run regression pin
// — if a future regalloc change causes spurious spills (e.g., by
// over-tightening the exclusion to also forbid operand[0]) the
// compile may fail-loud on spill exhaustion (R_Spilled* in the
// reporter), or — worse — silently miscompile via a chain
// reaction. Both surface as a wrong exit code on this pin.
int a = 1;
int b = 2;
int c = 3;
int d = 4;
int e = 5;
int f = 6;
int g = 7;

int main() {
    return a + b + c + d + e + f + g + 14;
}
