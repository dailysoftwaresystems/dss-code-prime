/* c76: crosses BOTH c76 thresholds in ONE callee, so it exercises the wide-call
 * arg materialization AND the D-FF3 SysV mixed-class overflow together.
 *   - 8 int params: i7,i8 overflow past the 6 SysV GPR arg registers;
 *   - 11 double params: d9,d10,d11 overflow past the 8 SysV FP arg registers.
 * The pre-regalloc lowerWideCallArgs pass must split BOTH the int and the FP
 * overflow args to `store_outgoing_arg` carriers (D-AS-REGALLOC-WIDE-CALL-
 * OPERAND-COUNT), and the callee must read the GPR-overflow and FP-overflow
 * stack args from DISTINCT monotonic offsets (D-FF3).
 *
 * WITNESS: register args are uniform (=1) but the OVERFLOW args are distinct-
 * valued and distinct-weighted so any wrong overflow-slot offset OR a GPR/FP
 * slot collision changes the exit code:
 *   isum: i1..i6 = 1 (weight 1..6) -> 21 ; i7,i8 = 2,3 (weight 7,8) -> 14+24=38
 *         isum = 59
 *   dsum: d1..d8 = 1 (weight 1..8) -> 36 ; d9,d10,d11 = 2,3,4 (weight 9,10,11)
 *         -> 18+30+44 = 92 ; dsum = 128
 *   total = 59 + 128 = 187
 *
 * FP args derived from a VOLATILE global (`one`==1) so the `release` arm does
 * not trip D-OPT-RELEASE-FP-CONSTANT-UNLOADED-XMM (c77).
 *
 * RED-ON-DISABLE (two tiers): revert the wide-call pass -> the 19-arg call
 * re-exhausts the register file -> error[L_VirtualRegInPostRegalloc]; revert
 * the D-FF3 offset fix -> on SysV the FP overflow reads a GPR-overflow slot ->
 * a wrong (not 187) value. => 187. */
volatile int one = 1;

int probe(int i1,int i2,int i3,int i4,int i5,int i6,int i7,int i8,
          double d1,double d2,double d3,double d4,double d5,double d6,
          double d7,double d8,double d9,double d10,double d11){
    int isum = i1*1+i2*2+i3*3+i4*4+i5*5+i6*6 + i7*7+i8*8;
    double dsum = d1*1+d2*2+d3*3+d4*4+d5*5+d6*6+d7*7+d8*8
                + d9*9+d10*10+d11*11;
    return isum + (int)dsum;
}
int main(void){
    double u = (double)one;   /* 1, non-constant */
    return probe(1,1,1,1,1,1, 2,3,
                 u,u,u,u,u,u,u,u, u*2.0,u*3.0,u*4.0);
}
