/* c76 (D-AS-REGALLOC-WIDE-CALL-OPERAND-COUNT, option E): a wide call THROUGH
 * a function pointer. Two invariants compose here: (1) the >6-arg overflow
 * must be split to `store_outgoing_arg` carriers before regalloc; (2) the
 * indirect callee register (the vreg holding `fp`) must SURVIVE arg setup
 * (FC4-c2 / L_IndirectCalleeClobberedByArgSetup) — the wide-call pass must
 * not let the callee-pointer register be reused as an arg-move scratch, or
 * the final `call *reg` jumps to garbage.
 *
 * WITNESS: `probe` takes 9 int params (a7..a9 overflow the 6 SysV GPR arg
 * registers). Values descending 9..1, weight 1..9, so every position is
 * distinct value AND weight -> a swap, a wrong overflow-slot read, OR a
 * clobbered callee pointer all change the result:
 *
 *   sum_{k=1..9} (10-k) * k
 *   = 9*1 + 8*2 + 7*3 + 6*4 + 5*5 + 4*6 + 3*7 + 2*8 + 1*9
 *   = 9+16+21+24+25+24+21+16+9 = 165
 *
 * RED-ON-DISABLE: revert the wide-call pass -> the 9-arg indirect call
 * re-exhausts the register file -> error[L_VirtualRegInPostRegalloc].
 * Int-only, so the release arm is FP-constant-bug-safe. => 165. */
int probe(int a1,int a2,int a3,int a4,int a5,int a6,int a7,int a8,int a9){
    return a1*1 + a2*2 + a3*3 + a4*4 + a5*5 + a6*6 + a7*7 + a8*8 + a9*9;
}
int main(void){
    int (*fp)(int,int,int,int,int,int,int,int,int) = probe;
    return fp(9,8,7,6,5,4,3,2,1);
}
