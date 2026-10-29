/* A STATIC `&&label` TABLE LAID OUT AFTER A BLOCK-INSERTING PASS HAS RUN
 * (D-LIR-DESCRIPTOR-BLOCK-IDS-SHIFTED-BY-A-BLOCK-INSERTING-PASS).
 *
 * `static void *tbl[] = {&&L0, &&L1, &&L2}; goto *tbl[i];` reads the label
 * addresses out of DATA, so no instruction names the blocks and the pipeline
 * binds each label's symbol after assembly, by the block id MIR->LIR gave it,
 * to the FINAL code's offsets. A LIR block id is a position, so every block a
 * later pass inserts renumbers every block after it — an inline-asm template's
 * label, and on Windows x64 the guard-page walk of a runtime stack descent.
 * Until the ids were translated through both passes, a label table laid out
 * after either one was REFUSED ("the assembler published no byte offset"), or
 * bound to another block's bytes.
 *
 * Each shape changes the exit code when it resolves wrongly:
 *   pick_before    — a table laid out BEFORE both (the control)            (1)
 *   spin           — the template's own loop                               (2)
 *   pick_after_asm — a table in a function after the template's            (4)
 *   asm_then_pick  — a template and a table in ONE function                (8)
 *   vla_frame      — a runtime descent (walked on Windows x64)             (16)
 *   pick_after_vla — a table in a function after the descent's            (64)
 *   vla_then_pick  — a descent and a table in ONE function                (128)
 * gcc, clang and mingw gcc run the asm-and-table shapes to 42 (P69, 2026-10-07).
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

/* `base + 100 + i` for i in 0..2, through a static table of label addresses —
 * the LAST statements of the function that uses it, once per function. */
#define PICK(base, i)                                                         \
    static void *const tbl[] = {&&L0, &&L1, &&L2};                            \
    goto *tbl[(i)];                                                           \
L0:                                                                           \
    return (base) + 100;                                                      \
L1:                                                                           \
    return (base) + 101;                                                      \
L2:                                                                           \
    return (base) + 102

NOINLINE int pick_before(int i) { PICK(0, i); }

NOINLINE int spin(int n) {
    int acc = 0;
    ASM_LOOP(acc, n);
    return acc;
}

NOINLINE int pick_after_asm(int i) { PICK(0, i); }

NOINLINE int asm_then_pick(int n, int i) {
    int acc = 0;
    ASM_LOOP(acc, n);
    PICK(acc, i);
}

/* Twice the 4 KiB guard page, so the walk loops on Windows x64. */
NOINLINE int vla_frame(int n) {
    volatile char block[n];
    block[0] = 1;
    block[n - 1] = 2;
    return block[0] + block[n - 1];
}

NOINLINE int pick_after_vla(int i) { PICK(0, i); }

/* The array's scope closes before the computed goto: DSS refuses a `goto *`
 * inside a VLA's scope (H_VlaComputedGotoInScope), and the descent and its
 * walk's blocks still come first in the function. */
NOINLINE int vla_then_pick(int n, int i) {
    int base;
    {
        volatile char block[n];
        block[n - 1] = 7;
        base = block[n - 1];
    }
    PICK(base, i);
}

int main(void) {
    int bad = 0;
    for (int i = 0; i < 3; ++i) {
        if (pick_before(i) != 100 + i) bad |= 1;
        if (pick_after_asm(i) != 100 + i) bad |= 4;
        if (asm_then_pick(3, i) != 106 + i) bad |= 8;
        if (pick_after_vla(i) != 100 + i) bad |= 64;
        if (vla_then_pick(8192, i) != 107 + i) bad |= 128;
    }
    if (spin(3) != 6) bad |= 2;
    if (vla_frame(8192) != 3) bad |= 16;
    return bad ? bad : 42;
}
