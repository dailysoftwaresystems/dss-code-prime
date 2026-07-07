/* D-AS-REGALLOC-ARG-REGISTER-OCCUPIED (c75 correctness fix, implicit-clobber
 * sibling). A division under register pressure whose divisor is spilled must
 * NOT reload the divisor into one of the division's own implicit registers
 * (x86 idiv uses rax = dividend, rdx = high-half/remainder).
 *
 * Pre-fix: c75's reserve-K puts the non-arg caller-saved register rax into
 * the rewriter's spill-reload scratch pool, and the rewriter's operand-reload
 * path did not forbid the current instruction's implicit rax/rdx. So the
 * spilled divisor `c` reloaded INTO rax, clobbering the dividend the idiv
 * still needed -> the division computed 3/3=1 instead of 121/3=40 -> 121
 * instead of 160 (a SILENT miscompile the re-audit caught; pre-c75 this input
 * fail-loud with L_VirtualRegInPostRegalloc). FIX: the rewriter forbids the
 * instruction's own implicitRegisters (inputs union clobbered) as reload
 * scratch — the same union the allocator already excludes from crossing
 * ranges, applied at the op where the transient reload happens.
 *
 * RED-ON-DISABLE: revert the fix -> x86_64 returns 121 (WRONG). arm64 (no
 * rax/rdx idiv constraint, ~30 GPRs) is correct either way. q = 121/3 = 40;
 * rest = 7+8+..+15 = 99; early = 1+2+..+6 = 21 => 160. */
int probe(int a,int b,int c,int d,int e,int f,int g,int h,
          int i,int j,int k,int l,int m,int n,int o){
    int q     = (a+b+c+d+e+f+100) / c;   /* idiv: dividend 121, divisor c=3 */
    int rest  = g+h+i+j+k+l+m+n+o;
    int early = a+b+c+d+e+f;
    return q + rest + early;              /* 40 + 99 + 21 = 160 */
}
int main(void){
    return probe(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);
}
