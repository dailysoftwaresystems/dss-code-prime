/* A DENSE SWITCH LAID OUT AFTER A BLOCK-INSERTING PASS HAS RUN
 * (D-LIR-DESCRIPTOR-BLOCK-IDS-SHIFTED-BY-A-BLOCK-INSERTING-PASS).
 *
 * A dense switch lowers to a jump table, a `.data` array of case addresses. The
 * table names its case blocks from DATA, by the block ids MIR->LIR gave them, and
 * is bound after assembly to the FINAL code's offsets. A LIR block id is a
 * position, so every block a later pass inserts renumbers every block after it.
 * Two passes insert blocks:
 *   - the inline-asm expansion: a label inside a template becomes a block;
 *   - the calling convention on Windows x64: a runtime stack descent (a VLA,
 *     `alloca`) walks the guard page in a loop of its own blocks.
 * Until the table's ids were translated through both passes, every switch laid
 * out after either one dispatched to the WRONG case, with no diagnostic.
 *
 * Each shape changes the exit code when it resolves wrongly:
 *   dispatch_before    — a switch laid out BEFORE both (the control)        (1)
 *   spin               — the template's own loop                            (2)
 *   dispatch_after_asm — a switch in a function after the template's        (4)
 *   asm_then_dispatch  — a template and a switch in ONE function            (8)
 *   vla_frame          — a runtime descent (walked on Windows x64)          (16)
 *   dispatch_after_vla — a switch in a function after the descent's        (64)
 *   vla_then_dispatch  — a descent and a switch in ONE function            (128)
 * gcc, clang and mingw gcc run the asm-and-switch shapes to 42 (P69, 2026-10-07).
 */
#if defined(__x86_64__)
#define ASM_LOOP(acc, n)                                                      \
    __asm__("1:\n\t"                                                          \
            "addl $2, %0\n\t"                                                 \
            "subl $1, %1\n\t"                                                 \
            "jnz 1b"                                                          \
            : "+r"(acc), "+r"(n) : : "cc")
#elif defined(__aarch64__)
#define ASM_LOOP(acc, n)                                                      \
    __asm__("1:\n\t"                                                          \
            "add %w0, %w0, #2\n\t"                                            \
            "sub %w1, %w1, #1\n\t"                                            \
            "cbnz %w1, 1b"                                                    \
            : "+r"(acc), "+r"(n) : : "cc")
#else
#error "this example needs an x86_64 or aarch64 inline-asm template"
#endif

#define NOINLINE __attribute__((noinline))

#define TEN_CASES(base)                                                       \
    case 0: return (base) + 10;                                               \
    case 1: return (base) + 11;                                               \
    case 2: return (base) + 12;                                               \
    case 3: return (base) + 13;                                               \
    case 4: return (base) + 14;                                               \
    case 5: return (base) + 15;                                               \
    case 6: return (base) + 16;                                               \
    case 7: return (base) + 17;                                               \
    case 8: return (base) + 18;                                               \
    case 9: return (base) + 19;                                               \
    default: return -1

NOINLINE int dispatch_before(int k) {
    switch (k) { TEN_CASES(0); }
}

NOINLINE int spin(int n) {
    int acc = 0;
    ASM_LOOP(acc, n);
    return acc;
}

NOINLINE int dispatch_after_asm(int k) {
    switch (k) { TEN_CASES(0); }
}

NOINLINE int asm_then_dispatch(int n, int k) {
    int acc = 0;
    ASM_LOOP(acc, n);
    switch (k) { TEN_CASES(acc); }
}

/* Twice the 4 KiB guard page, so the walk loops on Windows x64. */
NOINLINE int vla_frame(int n) {
    volatile char block[n];
    block[0] = 1;
    block[n - 1] = 2;
    return block[0] + block[n - 1];
}

NOINLINE int dispatch_after_vla(int k) {
    switch (k) { TEN_CASES(0); }
}

NOINLINE int vla_then_dispatch(int n, int k) {
    volatile char block[n];
    block[n - 1] = 7;
    switch (k) { TEN_CASES(block[n - 1]); }
}

int main(void) {
    int bad = 0;
    for (int k = 0; k < 10; ++k) {
        if (dispatch_before(k) != 10 + k) bad |= 1;
        if (dispatch_after_asm(k) != 10 + k) bad |= 4;
        if (asm_then_dispatch(3, k) != 16 + k) bad |= 8;
        if (dispatch_after_vla(k) != 10 + k) bad |= 64;
        if (vla_then_dispatch(8192, k) != 17 + k) bad |= 128;
    }
    if (dispatch_before(42) != -1 || dispatch_after_asm(-1) != -1) bad |= 1;
    if (spin(3) != 6) bad |= 2;
    if (vla_frame(8192) != 3) bad |= 16;
    return bad ? bad : 42;
}
