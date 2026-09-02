/* D-TARGET-REGISTER-CLASS-OPS-HAVE-NO-LONG-REACH-MEMORY-FORM: the RUN witness
 * for the AArch64 SIMD&FP scaled unsigned-offset LDR/STR (`fldr_u`/`fstr_u`),
 * in BOTH directions, at frame displacements the unscaled LDUR/STUR imm9
 * (-256..255) cannot reach.
 *
 * ⚠⚠ WHAT THIS EXAMPLE IS ABOUT, AND WHY A COMPILE-ONLY PIN WOULD NOT DO. The
 * frame chokepoint (`selectFrameMemOp`) has swapped an out-of-reach frame
 * access onto the scaled form since D-ASM-AARCH64-LARGE-FRAME-IMM12 — but only
 * for the INTEGER file, because the integer file was the only one whose scaled
 * twin was declared. An FP frame access past +-256 kept `fstur`/`fldur` and
 * fail-louded `A_ImmediateOperandOutOfRange` at the encoder with no form to
 * select. That is a REFUSAL, so a compile-only pin proves the refusal is gone;
 * it does NOT prove the displacement is right. The scaled field is
 * `byteOffset / accessSize`, so a wrong scale encodes a DIFFERENT slot at the
 * same instruction width — an access that assembles clean, runs, and reads
 * someone else's frame. Only execution separates those two outcomes, which is
 * why the exit code below is arithmetic over values that must survive a round
 * trip through high frame slots.
 *
 * THE SHAPE. `deep` takes 24 doubles — AAPCS64 passes 8 in v0..v7 and the rest
 * on the INCOMING STACK, so p08..p23 are read at [sp + frame + 0..127]. It then
 * calls `sink` with 40 doubles, whose args 8..39 occupy an OUTGOING stack area
 * of 256 bytes; every incoming read therefore sits at least 256 bytes up, past
 * the short reach, and must take the scaled load. The outgoing stores at
 * [sp + 8*(i-8)] climb to 248 and stay inside it, so BOTH forms appear in one
 * function and a regression that confused them shows up as a wrong number
 * rather than as a compile error.
 *
 * `spillFar` forces the STORE direction: it holds sixteen doubles live across
 * two calls, so the callee-saved SIMD&FP registers d8..d15 are used and their
 * prologue saves land above a 256-byte outgoing-argument area.
 *
 * MISCOMPILE SENSITIVITY. Every value is distinct and weighted, so reading the
 * WRONG slot (a mis-scaled displacement) or the wrong HALF of one (a
 * width-defaulted access) changes the total. The doubles are exact binary
 * fractions and the sums are exact in IEEE-754 double, so the result is a
 * single integer with no rounding slack:
 *
 *   deep(...)     = 1+2+...+24                       = 300
 *   spillFar(...) = 1+2+...+16, each doubled by sink = 272
 *   300 + 272 = 572 -> exit 572 & 0xFF = 60.
 *
 * ⚠ THE EXIT CODE IS TAKEN MOD 256 EXPLICITLY rather than left to the runtime:
 * a POSIX exit status carries eight bits, and letting the truncation happen
 * implicitly would make the pin depend on what a wrapper does with 572.
 *
 * arm64-ELF under qemu-aarch64.
 */

double sink(double p00, double p01, double p02, double p03,
            double p04, double p05, double p06, double p07,
            double p08, double p09, double p10, double p11,
            double p12, double p13, double p14, double p15,
            double p16, double p17, double p18, double p19,
            double p20, double p21, double p22, double p23,
            double p24, double p25, double p26, double p27,
            double p28, double p29, double p30, double p31,
            double p32, double p33, double p34, double p35,
            double p36, double p37, double p38, double p39)
{
    /* Reads a spread of the incoming stack slots so the arguments are not dead
     * and the caller cannot drop the stores. */
    return p08 + p20 + p31 + p39;
}

/* 24 incoming doubles, read AFTER a 40-argument call has pushed the outgoing
 * area under them: every p08..p23 read is a frame load past the short reach. */
double deep(double p00, double p01, double p02, double p03,
            double p04, double p05, double p06, double p07,
            double p08, double p09, double p10, double p11,
            double p12, double p13, double p14, double p15,
            double p16, double p17, double p18, double p19,
            double p20, double p21, double p22, double p23)
{
    double guard = sink(p00, p01, p02, p03, p04, p05, p06, p07,
                        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    /* guard is 0.0 by construction (every slot sink reads was passed 0.0); it
     * exists so the call cannot be eliminated and the outgoing area cannot be
     * shrunk away. */
    return guard
         + p00 + p01 + p02 + p03 + p04 + p05 + p06 + p07
         + p08 + p09 + p10 + p11 + p12 + p13 + p14 + p15
         + p16 + p17 + p18 + p19 + p20 + p21 + p22 + p23;
}

/* Sixteen values live ACROSS two calls: the SIMD&FP callee-saved registers are
 * used, and their prologue saves sit above the outgoing-argument area — the
 * STORE direction, past the short reach. */
double spillFar(double a, double b, double c, double d,
                double e, double f, double g, double h,
                double i, double j, double k, double l,
                double m, double n, double o, double p)
{
    double z0 = sink(a, b, c, d, e, f, g, h,
                     0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                     0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                     0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                     0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    double z1 = sink(i, j, k, l, m, n, o, p,
                     0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                     0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                     0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                     0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    /* Both z0 and z1 are 0.0; every a..p must survive the two calls, which is
     * what forces the callee-saved SIMD&FP registers into use. */
    return z0 + z1
         + 2.0 * (a + b + c + d + e + f + g + h
                  + i + j + k + l + m + n + o + p);
}

int main(void)
{
    double r = deep(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0,
                    9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0,
                    17.0, 18.0, 19.0, 20.0, 21.0, 22.0, 23.0, 24.0);
    r = r + spillFar(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0,
                     9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0);
    return (int)((long)r & 255);          /* 572 & 255 = 60 */
}
