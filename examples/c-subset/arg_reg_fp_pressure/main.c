/* D-AS-REGALLOC-ARG-REGISTER-OCCUPIED (c75 correctness fix, variant 2 —
 * FP). The sibling of the GPR case, on the FLOAT arg registers. Under FP
 * register pressure (many double params, each used in two sums so all stay
 * live simultaneously), the ALLOCATOR must not assign an incoming FP
 * argument register to a DIFFERENT vreg's home while the parameter it holds
 * is still live in it.
 *
 * Pre-fix (a PRE-EXISTING allocator bug that c75's added pressure unmasked —
 * before c75 these functions failed to compile with L_VirtualRegInPostRegalloc,
 * so the latent bug never surfaced): on x86_64 SysV the allocator assigned
 * xmm7 (the 8th FP arg register) to another param's home, clobbering the
 * incoming FP param in xmm7 -> that param read as a neighbour and the result
 * was 54 instead of 42 (a SILENT miscompile). The fix models each incoming
 * FP arg register as occupied [entry, argOp) and keeps allocation (and
 * spill-evict) off it while a range that could alias the incoming param is
 * live.
 *
 * RED-ON-DISABLE: revert the fix -> x86_64 returns 54 (WRONG). arm64 (32
 * FPRs) never spills here (trivially correct, value-checked).
 * fsum = 2 * (1+2+3+4+5+6) = 2 * 21 = 42.0 -> (int)42. */
double fsum(double a0,double a1,double a2,double a3,double a4,double a5,
            double a6,double a7,double a8,double a9,double a10,double a11,
            double a12,double a13,double a14,double a15){
    double s1 = a0+a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14+a15;
    double s2 = a0+a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14+a15;
    return s1 + s2;
}
int main(void){
    double r = fsum(1.0,2.0,3.0,4.0,5.0,6.0,0.0,0.0,
                    0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);
    return (int)r;   /* 42 */
}
