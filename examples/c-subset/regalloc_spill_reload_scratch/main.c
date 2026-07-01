/* c75 (D-AS-REGALLOC-SPILL-RELOAD-SCRATCH): a function under GENERAL
 * register pressure (peak simultaneous live values > the target's GPR
 * count) must still be able to RELOAD a spilled vreg. The linear-scan
 * allocator reserves nothing for spill reloads and the rewriter builds
 * its scratch pool function-globally, so a function that assigns all of
 * a class's registers left the reload path with an EMPTY scratch pool
 * -> error[L_VirtualRegInPostRegalloc]. This is sqlite func 1033's
 * shape (a spilled `arg`, isCall=0), the frontier the post-c74 re-probe
 * reached after the whole 8 MB amalgamation compiled.
 *
 * FIX: the allocator reserves K caller-saved registers per class as
 * guaranteed spill-reload scratch (K = the function's max single-
 * instruction register-reload demand, derived per-target — never a
 * hardcoded count). The reserved registers flow to the rewriter's
 * scratch pool automatically.
 *
 * WITNESS: `f` has 15 int params, each used in TWO inline sums. While
 * the SECOND sum evaluates, the first sum's result (1 vreg) coexists
 * with all 15 still-live param vregs -> MAXLIVE = 16 > the 15 allocatable
 * x86_64 GPRs -> the allocator spills, and (pre-fix) the empty scratch
 * pool cannot reload the spilled vreg. `main` calls `f` with 15 args (a
 * call that FITS the register file, so NO wide-call operand explosion —
 * that separate limit is the deferred D-AS-REGALLOC-WIDE-CALL-OPERAND-
 * COUNT). arm64 (~30 GPRs) never spills here, so it compiles trivially
 * and simply verifies the value. RED-ON-DISABLE: revert the reservation
 * -> error[L_VirtualRegInPostRegalloc] on x86_64. => 42. */
int f(int a0,int a1,int a2,int a3,int a4,int a5,int a6,int a7,
      int a8,int a9,int a10,int a11,int a12,int a13,int a14){
    return (a0+a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14)
         + (a0+a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14);
}
int main(void){
    /* 2 * (1+2+3+4+5+6) = 2 * 21 = 42 */
    return f(1,2,3,4,5,6,0,0,0,0,0,0,0,0,0);
}
