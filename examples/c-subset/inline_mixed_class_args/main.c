/* D-OPT-RELEASE-SYSV-MIXED-CLASS-REG-ARG-DROP (2026-07-07): under the shipped
 * `release` pipeline (which runs Inlining), a callee with MIXED integer/GPR and
 * float/FP register-class parameters was miscompiled when inlined. On a SysV-
 * style CC the GPR and FPR argument pools count INDEPENDENTLY, so `int a,
 * double x, int b, double y, double z` gives a=gpr#0, b=gpr#1, x=fpr#0, y=fpr#1,
 * z=fpr#2 — the FP params carry per-class ordinals {0,1,2} that COLLIDE with the
 * int params' {0,1}. The inliner mapped a callee `Arg` to the caller's actual by
 * that per-class ordinal, so `x` (fpr#0) spliced the FIRST integer actual (a
 * garbage double), etc. → a silent wrong result. FIX: the MIR `Arg` now also
 * carries a FLAT call-operand POSITION (a=0, x=1, b=2, y=3, z=4), which the
 * inliner uses to map the RIGHT actual.
 *
 * WITNESS: isum(2, 10.0, 3, 20.0, 43.0) = (int)(10+20+43) + 2 + 3 = 73+5 = 78.
 * The interleaved int/FP params make any single-slot mis-mapping observable.
 * RED-ON-DISABLE (release arm, WSL/qemu legs): before the fix the release build
 * returned garbage (the FP args read integer actuals); debug/gcc = 78.
 * ms_x64 (Windows) was immune (slot-aligned CC → ordinal == position). This
 * single root also fixed the sibling D-OPT-RELEASE-FP-CONSTANT-UNLOADED-XMM. */
int isum(int a, double x, int b, double y, double z) {
    return (int)(x + y + z) + a + b;
}
int main(void) {
    return isum(2, 10.0, 3, 20.0, 43.0);
}
