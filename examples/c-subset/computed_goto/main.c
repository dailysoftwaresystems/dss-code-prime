/* D-CSUBSET-COMPUTED-GOTO runtime witness: the GNU computed-goto extension
   (`&&label` label-address + `goto *expr` indirect branch) driving a tiny
   threaded-dispatch bytecode interpreter.

   A local jump table `tbl[]` holds the runtime addresses of three operation
   labels (filled via `&&op_*`). A program `prog[]` of opcodes is executed by
   repeatedly `goto *tbl[prog[pc]]` — the classic interpreter "threading" that
   computed goto exists for. The accumulator starts at 1 and the program is
   {add5, dbl, add5, halt}:  ((1 + 5) * 2) + 5 = 17, returned as the exit code.

   What this exercises end to end:
     - `&&op_add5` / `&&op_dbl` / `&&op_halt`  -> three distinct LabelAddressOf
       -> BlockAddress -> a synthetic per-block symbol materialized by `lea`
       (x86 rel32) / `adrp+add` (arm64), bound by the linker to each label
       block's interior VA.
     - `goto *tbl[prog[pc]]`                    -> IndirectBr whose successors are
       ALL THREE address-taken blocks (so reachability/DCE keep them; the
       address-taken target blocks are never folded/merged by SimplifyCfg).
     - run under BOTH the baseline pipeline AND the full release pipeline
       (which runs Inlining — the gate must REFUSE to inline this function).

   RED-ON-DISABLE intuition: if an address-taken target block were folded away
   (SimplifyCfg) or its synthetic symbol mis-bound, the indirect jump would land
   on wrong/deleted code and the exit code would not be 17; if the IndirectBr
   dropped a successor during an opt clone, DCE would delete that label's block. */

int main(void) {
    void *tbl[3];
    tbl[0] = &&op_add5;
    tbl[1] = &&op_dbl;
    tbl[2] = &&op_halt;

    int prog[4];
    prog[0] = 0;   /* add5 */
    prog[1] = 1;   /* dbl  */
    prog[2] = 0;   /* add5 */
    prog[3] = 2;   /* halt */

    int acc = 1;
    int pc = 0;
    goto *tbl[prog[pc]];

op_add5:
    acc = acc + 5;
    pc = pc + 1;
    goto *tbl[prog[pc]];

op_dbl:
    acc = acc * 2;
    pc = pc + 1;
    goto *tbl[prog[pc]];

op_halt:
    return acc;    /* ((1 + 5) * 2) + 5 == 17 */
}
