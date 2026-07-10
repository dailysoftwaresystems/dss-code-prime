/* D-CSUBSET-THREAD-LOCAL (TLS C1): the single-thread thread_local witness.
 *
 * What this CAN prove at runtime:
 *   - the loader COPIED the .tdata template into the main thread's TLS
 *     block (g is read as 7 BEFORE any write — a zeroed or garbage block
 *     would fail here);
 *   - mutation sticks across function calls (the per-thread copy is
 *     ordinary mutable storage);
 *   - a zero-init `static thread_local` (the .tbss arm) starts at 0 and
 *     accumulates;
 *   - `thread_local const` reads through the SAME tp-relative path
 *     (it lives in .tdata, NOT .rodata — per-thread identity).
 *
 * What it CANNOT prove: that the storage is genuinely PER-THREAD — a
 * process-shared alias passes every assertion below. The structural
 * pins (PT_TLS present, SHF_TLS flags, the patched tpoff bytes — see
 * tests/link/test_elf_exec_writer.cpp) and the 2-thread
 * thread_local_pthread example are the discriminators.
 *
 * x86_64-linux only until TLS C2 (arm64) / C3 (pe64) / C4 (Mach-O) land.
 * Exit 42. No stdout (the only extern is the trampoline's libc exit).
 */
thread_local int g = 7;
thread_local const int k = 3;

static int bump(void) {
    static thread_local int counter;   /* .tbss: zero-init per-thread */
    counter = counter + g;
    return counter;
}

int main(void) {
    if (g != 7) return 1;   /* template copy visible BEFORE any write */
    g = 15;                 /* mutate the per-thread copy */
    bump();                 /* counter = 15 */
    int c = bump();         /* counter = 30 */
    if (k != 3) return 2;   /* const thread_local reads through tp + tpoff */
    return g + c - k;       /* 15 + 30 - 3 = 42 */
}
