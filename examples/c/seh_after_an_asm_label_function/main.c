/* A `__try` LAID OUT AFTER A BLOCK-INSERTING PASS HAS RUN
 * (D-LIR-DESCRIPTOR-BLOCK-IDS-SHIFTED-BY-A-BLOCK-INSERTING-PASS).
 *
 * A Windows x64 `__try` is a SCOPE TABLE row: the byte range the guarded body
 * occupies and where its handler begins, read by the OS when a fault is raised.
 * The pipeline builds the row after assembly from three block ids MIR->LIR gave
 * it — the guarded body's first and LAST block and the handler's — against the
 * FINAL code's offsets. A LIR block id is a position, so every block a later
 * pass inserts renumbers every block after it: an inline-asm template's label,
 * and the guard-page walk of a runtime stack descent. Until the ids were
 * translated through both passes, a `__try` laid out after either one guarded
 * the wrong bytes, and its access violation escaped and killed the process.
 *
 * A guarded body can also BECOME several blocks itself — a template inside it —
 * and then its range must end after the last of them, not the first.
 *
 * Each shape changes the exit code when it resolves wrongly (a fault that
 * escapes ends the process with 0xC0000005 instead):
 *   guard_before              — a `__try` laid out BEFORE both (the control) (1)
 *   spin                      — the template's own loop                      (2)
 *   guard_after_asm           — a `__try` in a function after the template's (4)
 *   guard_spanning_a_template — a template INSIDE the guarded body, and the
 *                               fault after it, in the body's last piece     (8)
 *   vla_frame                 — a runtime descent, walked                   (16)
 *   guard_after_vla           — a `__try` in a function after the descent's (64)
 * Windows x64 only: `__try` is SEH. MSVC has `__try` and no x64 inline asm,
 * mingw gcc the reverse; the program is the union, which is DSS's to compile.
 */
#include <windows.h>

#define NOINLINE __attribute__((noinline))

#define ASM_LOOP(acc, n)                                                      \
    __asm__("1:\n\t"                                                          \
            "addl $2, %0\n\t"                                                 \
            "subl $1, %1\n\t"                                                 \
            "jnz 1b"                                                          \
            : "+r"(acc), "+r"(n) : : "cc")

NOINLINE int guard_before(volatile int *p) {
    int rc = 0;
    __try {
        rc = *p;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        rc = 42;
    }
    return rc;
}

NOINLINE int spin(int n) {
    int acc = 0;
    ASM_LOOP(acc, n);
    return acc;
}

NOINLINE int guard_after_asm(volatile int *p) {
    int rc = 0;
    __try {
        rc = *p;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        rc = 42;
    }
    return rc;
}

NOINLINE int guard_spanning_a_template(volatile int *p, int n) {
    int rc = 0;
    __try {
        int acc = 0;
        ASM_LOOP(acc, n);
        rc = acc + *p;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        rc = 42;
    }
    return rc;
}

/* Twice the 4 KiB guard page, so the walk loops. */
NOINLINE int vla_frame(int n) {
    volatile char block[n];
    block[0] = 1;
    block[n - 1] = 2;
    return block[0] + block[n - 1];
}

NOINLINE int guard_after_vla(volatile int *p) {
    int rc = 0;
    __try {
        rc = *p;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        rc = 42;
    }
    return rc;
}

int main(void) {
    /* One no-access page: reading it raises EXCEPTION_ACCESS_VIOLATION. */
    volatile int *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) return 200;   /* the environment, not a result */
    int bad = 0;
    if (guard_before(p) != 42) bad |= 1;
    if (spin(3) != 6) bad |= 2;
    if (guard_after_asm(p) != 42) bad |= 4;
    if (guard_spanning_a_template(p, 3) != 42) bad |= 8;
    if (vla_frame(8192) != 3) bad |= 16;
    if (guard_after_vla(p) != 42) bad |= 64;
    return bad ? bad : 42;
}
