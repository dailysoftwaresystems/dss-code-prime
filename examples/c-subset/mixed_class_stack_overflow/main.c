/* c76 (D-FF3-SYSV-MIXED-CLASS-STACK-OVERFLOW-OFFSET, folded into c76): on
 * x86-64 SysV, a callee with BOTH integer/GPR-class params overflowing past
 * the 6 GPR arg registers AND float/FP-class params overflowing past the 8 FP
 * arg registers must read the overflowed GPR stack arg and the overflowed FP
 * stack arg from DISTINCT, monotonic incoming-stack offsets — not the same
 * slot. Pre-fix, the two independent per-class overflow counters each restart
 * the incoming-stack cursor at 0, so the FP overflow arg reads the INT overflow
 * arg's slot (garbage double).
 *
 * WITNESS (the canonical repro): `int probe(int i1..i7, double d0..d8)` — 7
 * ints (i7 overflows the 6 GPR arg regs) and 9 doubles (d8 overflows the 8 FP
 * arg regs). isum + (int)dsum = (1+2+3+4+5+6+7) + (1+2+3+4+5+6+7+8+9)
 * = 28 + 45 = 73.
 *
 * RED-ON-DISABLE (WITNESSED on build-rel): revert the D-FF3 offset fix -> on
 * SysV the FP overflow arg (d8) reads the GPR overflow arg's (i7) slot -> the
 * returned value drops to 64 instead of 73. AAPCS64 and Win64 ms_x64 slot-align
 * so they never collide (73 on those legs regardless).
 *
 * This example OMITS optimizedPipelines (documented carve-out): its PLAIN
 * unweighted double sum trips the SEPARATE pre-existing release-only bug
 * D-OPT-RELEASE-FP-CONSTANT-UNLOADED-XMM (c77) — see expected.json. The D-FF3
 * invariant keeps a release-arm witness via mixed_class_stack_overflow_
 * interleaved (weighted sum, sidesteps that bug). => 73. */
int probe(int i1,int i2,int i3,int i4,int i5,int i6,int i7,
          double d0,double d1,double d2,double d3,double d4,
          double d5,double d6,double d7,double d8){
    int isum = i1+i2+i3+i4+i5+i6+i7;
    double dsum = d0+d1+d2+d3+d4+d5+d6+d7+d8;
    return isum + (int)dsum;
}
int main(void){
    return probe(1,2,3,4,5,6,7, 1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0);
}
