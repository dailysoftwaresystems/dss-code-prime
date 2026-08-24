/* D-LK4-DATA-PRODUCER (writable data sections) — the RUNTIME witness that a
 * store to a mutable global no longer faults. Before this cycle EVERY
 * initialized global landed in read-only `.rodata` (asm.cpp unconditionally
 * stamped Rodata), so `gd = gd + 2` segfaulted (SIGSEGV / exit 139). Now a
 * mutable initialized global → writable `.data` and a tentative zero-init
 * global → `.bss` (zero-fill), both writable.
 *
 * Exercises BOTH writable section kinds, each WRITTEN then READ into the exit:
 *   - gd: initialized mutable global (5) → `.data`; a runtime store `gd = gd+2`
 *         then a runtime LOAD of gd into the result (5+2 = 7).
 *   - gz: tentative zero-init global → `.bss`; a runtime store `gz = 35` then a
 *         runtime LOAD of gz (35).
 * The reads are genuine runtime global LOADs (not folded by Mem2Reg/ConstFold —
 * a global's value can change between the store and the read, so the optimizer
 * must reload), so the stores genuinely execute. A regression that reverts the
 * section choice faults at the store; a regression that mis-lays-out the
 * section reads the wrong bytes and flips the exit code. Exit = 7 + 35 = 42. */

int gd = 5;        /* initialized mutable global -> .data */
int gz;            /* tentative zero-init global  -> .bss */

int main(void) {
    gd = gd + 2;   /* runtime store to a .data global: 5 + 2 = 7 */
    gz = 35;       /* runtime store to a .bss global              */
    return gd + gz; /* runtime LOADs of both: 7 + 35 = 42 */
}
