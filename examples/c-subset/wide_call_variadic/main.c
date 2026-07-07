/* c76 (D-AS-REGALLOC-WIDE-CALL-OPERAND-COUNT, option E): the wide-call arg
 * materialization must also handle a VARIADIC callee. On SysV the fixed
 * parameter `n` takes the first GPR arg register (rdi); the varargs travel
 * the variadicArgsAlwaysStack path (each promoted vararg materialized to the
 * outgoing stack). With 10 int varargs the outgoing arg list far exceeds the
 * register file, so the pre-regalloc split into `store_outgoing_arg` carriers
 * is required or the Call exhausts regalloc.
 *
 * WITNESS: vsum reads its 10 varargs with a POSITION weight k (1..10). The
 * values are descending 10..1, so every position has a distinct value AND a
 * distinct weight -> swapping any two varargs, or reading one from a wrong
 * overflow slot, changes the weighted sum (delta = (vi-vj)*(j-i) != 0):
 *
 *   sum_{k=1..10} (11-k) * k
 *   = 10*1 + 9*2 + 8*3 + 7*4 + 6*5 + 5*6 + 4*7 + 3*8 + 2*9 + 1*10
 *   = 10+18+24+28+30+30+28+24+18+10 = 220
 *
 * RED-ON-DISABLE: revert the wide-call pass -> the variadic call's outgoing
 * args re-exhaust the register file -> error[L_VirtualRegInPostRegalloc].
 * Int-only varargs, so the release arm is FP-constant-bug-safe. => 220. */
#include <stdarg.h>
int vsum(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int acc = 0;
    for (int k = 1; k <= n; ++k) acc += va_arg(ap, int) * k;
    va_end(ap);
    return acc;
}
int main(void) {
    return vsum(10, 10,9,8,7,6,5,4,3,2,1);
}
