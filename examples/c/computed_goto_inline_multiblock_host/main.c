/* D-CG-INLINE-MULTIBLOCK-INTO-COMPUTED-GOTO-HOST — the EXECUTION witness for
 * splicing a MULTI-BLOCK callee INTO a function that itself contains a computed
 * goto.
 *
 * A computed-goto threaded interpreter (`&&label` + `goto *p` = BlockAddress +
 * IndirectBr) whose EVERY address-taken label block also calls a MULTI-BLOCK,
 * inline-eligible helper (`adjust`, a three-armed if/else chain). Under the
 * RELEASE pipeline (Inlining ON) `main` therefore routes through
 * `MultiBlockInliner`, and EVERY ONE of its address-taken blocks is SPLIT at the
 * call: the pre-call instructions stay in the label's own block, the callee's
 * CFG is cloned after it, and a fresh continuation block carries the rest.
 *
 * ★ WHY THE EXIT CODE DISCRIMINATES, AND WHY IR INSPECTION WOULD NOT.
 * A computed goto that lands on the WRONG block still produces well-formed IR.
 * Two independent things have to be right for this program to print 116:
 *
 *   1. `&&op_add` must remap through the block-ENTRY map, not the block-EXIT
 *      map. `op_add` is entered TWICE (prog[0] and prog[2]) — the second entry
 *      arrives through `goto *`, i.e. through the BlockAddress. If the address
 *      were remapped to the split chain's CONTINUATION (the natural wrong
 *      answer: that is the block that now carries the label block's original
 *      terminator), the second visit would skip `acc = adjust(acc)` entirely and
 *      the program would exit 113 instead of 116.
 *
 *   2. The `IndirectBr`'s successor set must be remapped 1:1. `trace` is a
 *      base-4 register of WHICH labels ran in WHICH order, so landing on any
 *      other label — or losing an edge and having the prune delete a live label
 *      block — moves the exit code.
 *
 * RED-ON-DISABLE (REMOVE-direction, both arms are in
 * `src/opt/passes/inlining.cpp`):
 *   * delete the `BlockAddress` arm of `MultiBlockInliner::emitCallerInst` and
 *     the verbatim `addInst` fallback copies the OLD block id as the payload —
 *     a stale address into a renumbered arena;
 *   * delete the `IndirectBr` arm of `MultiBlockInliner::emitTerminator` and the
 *     switch's `default:` aborts the compile.
 *
 * adjust(x) = x>100 ? x-50 : (x<0 ? -x : x+3).
 *   prog = {add, mul, add, dec, halt}; acc = 1, trace = 0
 *   op_add : acc = adjust(1)      = 4    trace = 0*4+1 =   1
 *   op_mul : acc = adjust(4)*2    = 14   trace = 1*4+2 =   6
 *   op_add : acc = adjust(14)     = 17   trace = 6*4+1 =  25
 *   op_dec : acc = adjust(17)-7   = 13   trace = 25*4+3 = 103
 *   op_halt: return acc + trace   = 116
 * Witnessed under BOTH baseline and the full release pipeline. */

int adjust(int x) {
    int r;
    if (x > 100) {
        r = x - 50;
    } else if (x < 0) {
        r = 0 - x;
    } else {
        r = x + 3;
    }
    return r;
}

int main(void) {
    void *tbl[4];
    tbl[0] = &&op_add;
    tbl[1] = &&op_mul;
    tbl[2] = &&op_dec;
    tbl[3] = &&op_halt;

    int prog[5];
    prog[0] = 0;   /* add  */
    prog[1] = 1;   /* mul  */
    prog[2] = 0;   /* add  — the SECOND entry into an already-split label block */
    prog[3] = 2;   /* dec  */
    prog[4] = 3;   /* halt */

    int acc = 1;
    int trace = 0;
    int pc = 0;
    goto *tbl[prog[pc]];

op_add:
    acc = adjust(acc);
    trace = trace * 4 + 1;
    pc = pc + 1;
    goto *tbl[prog[pc]];

op_mul:
    acc = adjust(acc) * 2;
    trace = trace * 4 + 2;
    pc = pc + 1;
    goto *tbl[prog[pc]];

op_dec:
    acc = adjust(acc) - 7;
    trace = trace * 4 + 3;
    pc = pc + 1;
    goto *tbl[prog[pc]];

op_halt:
    return acc + trace;   /* 116 */
}
