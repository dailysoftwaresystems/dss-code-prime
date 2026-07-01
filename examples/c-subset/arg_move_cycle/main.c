/* c76 (D-ML7-2.3 move-cycle + its scratch-picker soundness completion,
 * a.k.a. the re-characterized D-AS-REGALLOC-FPR-SPILL-RELOAD-SCRATCH):
 * an arg-register PERMUTATION at a call must be resolved as a parallel copy
 * whose cycle is broken via a config-driven scratch that avoids EVERY
 * committed/identity destination — not just the still-pending moves.
 *
 * WITNESS (an 8-FP-arg ROTATION = the reported rot8 shape): g receives
 * a..i in the 8 SysV FP arg registers (xmm0..7) and calls sink with them
 * rotated left by one: sink(b,c,d,e,f,h,i,a). Every source FPR is also a
 * destination FPR -> ONE 8-cycle. No in-order emission is correct; the fix
 * copies one cycle member to a caller-saved scratch FPR (SysV xmm8..15) and
 * redirects its readers, linearizing the cycle. sink positional-weights all
 * 8 args so ANY mis-rotation changes the result:
 *   sink(p0..p7) = sum_{k=0..7} (k+1)*p[k]
 * Inputs 1..8; the rotated call passes (2,3,4,5,6,7,8,1):
 *   2*1+3*2+4*3+5*4+6*5+7*6+8*7+1*8 = 2+6+12+20+30+42+56+8 = 176.
 * main maps 176 -> 42 via -134.
 *
 * The FP args are derived from a VOLATILE global (`one`==1) rather than
 * literals, so they are NON-CONSTANT: this keeps the `release` shipped-
 * pipeline arm from tripping the SEPARATE pre-existing FP-constant bug
 * D-OPT-RELEASE-FP-CONSTANT-UNLOADED-XMM (c77) while still witnessing the
 * optimizer x cycle-break composition. Values are identical to constant rot8.
 *
 * RED-ON-DISABLE (two tiers):
 *  - revert the whole move-cycle fix -> error[L_MoveCycleUnsupported] (the
 *    pre-fix detector bails before codegen);
 *  - revert only the scratch-picker soundness half -> the cycle-break scratch
 *    lands on a committed low arg register (xmm0) and clobbers a delivered
 *    argument -> a SILENT wrong value on SysV (rot8 -> 1, not 42). => 42. */
volatile int one = 1;

double sink(double p0, double p1, double p2, double p3,
            double p4, double p5, double p6, double p7) {
    return p0*1.0 + p1*2.0 + p2*3.0 + p3*4.0
         + p4*5.0 + p5*6.0 + p6*7.0 + p7*8.0;
}

double g(double a, double b, double c, double d,
         double e, double f, double h, double i) {
    return sink(b, c, d, e, f, h, i, a);   /* rotate-left: the 8-cycle */
}

int main(void) {
    double u = (double)one;                /* 1, but non-constant */
    double r = g(u*1.0, u*2.0, u*3.0, u*4.0, u*5.0, u*6.0, u*7.0, u*8.0);
    if (r != 176.0) return 1;
    return (int)(r - 134.0);               /* 42 */
}
