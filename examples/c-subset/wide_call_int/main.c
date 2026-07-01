/* c76 (D-AS-REGALLOC-WIDE-CALL-OPERAND-COUNT, option E): a CALL with more
 * scalar arguments than the calling convention passes in registers must have
 * its overflow arguments split to stack `store_outgoing_arg` carriers BEFORE
 * register allocation — otherwise the single Call instruction holds more
 * live register-operands than the machine has registers and regalloc exhausts
 * the file (error[L_VirtualRegInPostRegalloc]). This is sqlite's func-2088
 * wide-call blocker.
 *
 * WITNESS: `probe` takes 12 int params. On x86_64 SysV the first 6 go in the
 * GPR arg registers (rdi,rsi,rdx,rcx,r8,r9); a7..a12 OVERFLOW to the outgoing
 * stack. The overflow args are distinct-valued (1,2,3,4,5,6) AND distinct-
 * weighted (*2,*3,*4,*5,*6,*7), so a WRONG overflow-slot offset OR a SWAP of
 * any two overflow args changes the result (delta = (vi-vj)*(wj-wi) != 0):
 * the exit code is a strong witness, never a sum that hides swaps.
 *
 *   register args a1..a6 = 1 each, weight 1  -> 6
 *   overflow  a7..a12    = 1,2,3,4,5,6, weight 2..7
 *                        -> 1*2+2*3+3*4+4*5+5*6+6*7 = 2+6+12+20+30+42 = 112
 *   total = 118
 *
 * RED-ON-DISABLE: revert the wide-call materialization pass (or its
 * compile_pipeline invocation) -> this 12-arg call re-exhausts the register
 * file -> error[L_VirtualRegInPostRegalloc] on x86_64. => 118. */
int probe(int a1,int a2,int a3,int a4,int a5,int a6,
          int a7,int a8,int a9,int a10,int a11,int a12){
    return a1 + a2 + a3 + a4 + a5 + a6
         + a7*2 + a8*3 + a9*4 + a10*5 + a11*6 + a12*7;
}
int main(void){
    return probe(1,1,1,1,1,1, 1,2,3,4,5,6);
}
