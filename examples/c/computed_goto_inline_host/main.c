/* D-CSUBSET-COMPUTED-GOTO — the inline-INTO-a-computed-goto-host witness.
 *
 * A computed-goto threaded interpreter whose dispatch function ALSO calls a
 * MULTI-BLOCK, inline-eligible helper (`adjust`, which has an if/else so it is
 * not a single-block leaf). Under the RELEASE pipeline (Inlining ON), `main` is
 * an inline HOST that contains `&&label`/`goto *p` (BlockAddress + IndirectBr)
 * AND has a multi-block callee in its plan — the exact shape that, before the
 * routing fix, sent `main` through the `MultiBlockInliner` whose hand-rolled
 * caller-host emit copies a BlockAddress's block-id payload verbatim (a stale id
 * after the host's blocks are renumbered = SILENT MISCOMPILE) and aborts on the
 * IndirectBr terminator (no arm).
 *
 * ⚠ UPDATED 2026-09-02 (P53, D-CG-INLINE-MULTIBLOCK-INTO-COMPUTED-GOTO-HOST).
 * THIS PARAGRAPH USED TO SAY `adjust` "simply stays a call, not inlined into the
 * computed-goto host", and it is no longer true — that sentence described the
 * ROUTING WORKAROUND, not the behaviour. The workaround is gone: the
 * `MultiBlockInliner` now has its own `BlockAddress` arm (payload remapped via
 * `blockMap_`) and `IndirectBr` arm (successors remapped), so a multi-block
 * callee IS inlined into a computed-goto host, and `functionHasComputedGoto` —
 * the guard this comment named — has been deleted along with its half of the
 * `singleBlockOnly` condition. ✔MEASURED: this example now inlines.
 *
 * RED-ON-DISABLE: delete the `BlockAddress` arm from
 * `MultiBlockInliner::emitCallerInst` and this example's release arm exits with
 * an ACCESS_VIOLATION instead of 11; delete the `IndirectBr` arm from
 * `emitTerminator` and it aborts on an unhandled terminator opcode. ✔Both
 * MEASURED. The stale routing guard is NOT the disable knob any more.
 *
 * acc starts at 1; prog = {add5, dbl, add5, halt}; adjust(x) = x>100 ? x-50 : x+3.
 *   pc0 add5: acc = adjust(1)  = 4   (1<=100 -> 1+3)
 *   pc1 dbl : acc = 4*2        = 8
 *   pc2 add5: acc = adjust(8)  = 11  (8<=100 -> 8+3)
 *   pc3 halt: return 11
 * Witnessed under BOTH baseline and the full release pipeline, x86 + arm64 qemu. */

int adjust(int x) {
    int r;
    if (x > 100) { r = x - 50; } else { r = x + 3; }
    return r;
}

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
    acc = adjust(acc);
    pc = pc + 1;
    goto *tbl[prog[pc]];

op_dbl:
    acc = acc * 2;
    pc = pc + 1;
    goto *tbl[prog[pc]];

op_halt:
    return acc;    /* 11 */
}
