/* D-AS-REGALLOC-ARG-REGISTER-OCCUPIED (c75 correctness fix, variant 1 —
 * GPR). A function with more integer parameters than the target's integer
 * argument registers, under enough register pressure to spill, must NOT
 * reuse an incoming ARGUMENT register while the parameter it holds is still
 * live in it. An incoming arg register is occupied over [entry, argOp) —
 * the param's `arg` op materializes it, and callconv inserts the
 * mov-from-argreg POST-regalloc — so it looks free to the allocator/rewriter
 * in that window.
 *
 * Pre-fix, c75's reserve-K reservation reserved x86_64 SysV's r9 (the last
 * caller-saved GPR AND the 6th integer arg register) as spill-reload
 * scratch; the rewriter staged a reload through r9 at function entry,
 * clobbering the 6th incoming parameter (`f`) before its own `arg` op read
 * it — so `f` read as `e` (the 5th param) and the result was 149 instead of
 * 156. A SILENT miscompile the independent audit caught. The fix (1)
 * arg-excludes the reservation and (2) forbids arg registers as scratch for
 * a spilled `arg`-op result.
 *
 * RED-ON-DISABLE: revert the fix -> x86_64 returns 149 (WRONG). arm64
 * (~30 GPRs) never spills here so it is trivially correct and value-checks.
 * fx = 6*7 = 42; rest = 7+8+..+15 = 99; early = 1+2+..+5 = 15 => 156. */
int probe(int a,int b,int c,int d,int e,int f,int g,int h,
          int i,int j,int k,int l,int m,int n,int o){
    int fx    = f * 7;
    int rest  = g+h+i+j+k+l+m+n+o;
    int early = a+b+c+d+e;
    return fx + rest + early;   /* 156 */
}
int main(void){
    return probe(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);
}
