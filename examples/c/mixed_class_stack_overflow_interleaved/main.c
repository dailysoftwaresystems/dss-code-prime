/* c76 (D-FF3-SYSV-MIXED-CLASS-STACK-OVERFLOW-OFFSET, interleaved variant):
 * the SAME SysV shared-cursor invariant as mixed_class_stack_overflow, but with
 * the int and double params INTERLEAVED in the signature rather than grouped —
 * so the per-class overflow cursors are advanced in an interleaved order and
 * must STILL resolve to distinct monotonic incoming-stack offsets.
 *
 * WITNESS: `int probe(int i1, double d1, int i2, double d2, ... int i7, double
 * d7, double d8, double d9)` — 7 ints (i7 overflows the 6 GPR arg regs) and 9
 * doubles (d9 overflows the 8 FP arg regs). Both classes are weighted by
 * position and the values descend, so every position is distinct value AND
 * weight -> a slot collision, swap, or wrong offset changes the exit code:
 *   isum = sum_{k=1..7} (8-k)*k = 7*1+6*2+5*3+4*4+3*5+2*6+1*7 = 84
 *   dsum = sum_{k=1..9} (10-k)*k = 9*1+8*2+7*3+6*4+5*5+4*6+3*7+2*8+1*9 = 165
 *   total = 84 + 165 = 249
 *
 * FP args derived from a VOLATILE global (`one`==1) so the `release` arm does
 * not trip D-OPT-RELEASE-FP-CONSTANT-UNLOADED-XMM (c77).
 *
 * RED-ON-DISABLE: revert the D-FF3 offset fix -> on SysV the FP overflow arg
 * reads the GPR overflow arg's slot -> a wrong (not 249) value; AAPCS64/Win64
 * slot-align so they are unaffected. => 249. */
volatile int one = 1;

int probe(int i1, double d1, int i2, double d2, int i3, double d3,
          int i4, double d4, int i5, double d5, int i6, double d6,
          int i7, double d7, double d8, double d9){
    int isum = i1*1+i2*2+i3*3+i4*4+i5*5+i6*6+i7*7;
    double dsum = d1*1+d2*2+d3*3+d4*4+d5*5+d6*6+d7*7+d8*8+d9*9;
    return isum + (int)dsum;
}
int main(void){
    double u = (double)one;   /* 1, non-constant */
    return probe(7, u*9.0, 6, u*8.0, 5, u*7.0, 4, u*6.0, 3, u*5.0, 2, u*4.0,
                 1, u*3.0, u*2.0, u*1.0);
}
