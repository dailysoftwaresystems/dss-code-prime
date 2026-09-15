/* D-C-ATOMIC-COMPOUND-ASSIGNMENT-AND-INCREMENT-ARE-A-LOAD-THEN-A-SEPARATE-STORE
 * — the SEMANTICS witness for the generic compare-exchange entry of DSS's own
 * pe64 atomics runtime, `__atomic_compare_exchange`, called DIRECTLY.
 *
 * ★★★ WHY A DIRECT CALL. That entry exists to serve the compare-exchange retry
 * loop compound assignment and `++`/`--` on an under-aligned `_Atomic` lower to,
 * and that lowering is a different change. Calling the entry by name exercises
 * the body on its own, whether or not the lowering has landed — the way a
 * foreign object linked into the same program would call it.
 *
 * ★★ THE CONTRACT BEING PINNED, as clang's own call site reads it (✔MEASURED,
 * clang 18.1.3 -O1 on `g.a += 5`: size, object, &expected slot, &desired slot,
 * 5, 5; then it tests the returned byte and, on false, RELOADS its expected
 * slot): if the object equals `*expected`, store `*desired` and return 1 with
 * `*expected` untouched; otherwise return 0, leave the object untouched and
 * write the value OBSERVED into `*expected`. A compare-exchange that returned 0
 * without that write-back would spin a real caller's retry loop forever.
 *
 * ★ EVERY ARM OF THE BODY IS REACHED, AND THE PLACEMENT IS WHAT CHOOSES IT. The
 * objects sit at fixed offsets inside 64-byte lines of a byte arena, so each
 * one's arm is decided before the program runs:
 *   crossing4  4 bytes at byte 62 — crosses a line → the process lock
 *   crossing8  8 bytes at byte 58 — crosses a line → the process lock
 *   inside4    4 bytes at byte 54 — inside a line  → `lock cmpxchg`, 32-bit
 *   inside8    8 bytes at byte 50 — inside a line  → `lock cmpxchg`, 64-bit
 *   residue    2 bytes at byte 3 of an aligned 8-byte block → the block CAS
 * Each check also proves the byte just below the object survived, since C11
 * §6.5.1p2 makes it a distinct memory location the runtime does not own.
 *
 * exit 42 when every check holds; otherwise 100 + (arm × 10) + the check that
 * failed, so a red names the arm and the half of the contract that broke.
 */
#include <stddef.h>
#include <stdint.h>

extern _Bool __atomic_compare_exchange(size_t size, void *mem, void *expected,
                                       void *desired, int success, int failure);

struct __attribute__((packed)) P4 { char c; _Atomic unsigned a; };
struct __attribute__((packed)) P8 { char c; _Atomic unsigned long long a; };

static unsigned char g_arena[1024];

static uintptr_t lineStart(unsigned line) {
    uintptr_t const first = ((uintptr_t)(void *)g_arena + 63u) & ~(uintptr_t)63u;
    return first + (uintptr_t)(64u * line);
}

static int check4(struct P4 *p, int base) {
    void *const mem = (void *)((uintptr_t)(void *)p + 1u);
    p->c = 0x5A;
    p->a = 0x11111111u;
    unsigned expected = 0x11111111u;
    unsigned desired  = 0x22222222u;
    if (__atomic_compare_exchange(4, mem, &expected, &desired, 5, 5) != 1) return base + 1;
    if (expected != 0x11111111u) return base + 2;
    if (p->a != 0x22222222u) return base + 3;
    unsigned stale = 0x11111111u;
    unsigned other = 0x33333333u;
    if (__atomic_compare_exchange(4, mem, &stale, &other, 5, 5) != 0) return base + 4;
    if (stale != 0x22222222u) return base + 5;
    if (p->a != 0x22222222u) return base + 6;
    if (p->c != 0x5A) return base + 7;
    return 0;
}

static int check8(struct P8 *p, int base) {
    void *const mem = (void *)((uintptr_t)(void *)p + 1u);
    p->c = 0x5A;
    p->a = 0x1111111111111111ull;
    unsigned long long expected = 0x1111111111111111ull;
    unsigned long long desired  = 0x2222222222222222ull;
    if (__atomic_compare_exchange(8, mem, &expected, &desired, 5, 5) != 1) return base + 1;
    if (expected != 0x1111111111111111ull) return base + 2;
    if (p->a != 0x2222222222222222ull) return base + 3;
    unsigned long long stale = 0x1111111111111111ull;
    unsigned long long other = 0x3333333333333333ull;
    if (__atomic_compare_exchange(8, mem, &stale, &other, 5, 5) != 0) return base + 4;
    if (stale != 0x2222222222222222ull) return base + 5;
    if (p->a != 0x2222222222222222ull) return base + 6;
    if (p->c != 0x5A) return base + 7;
    return 0;
}

/* Two plain bytes inside an aligned 8-byte block. No `_Atomic` short is
 * involved — DSS cannot lower one — so the object is reached through a byte
 * pointer and the two neighbours on either side are checked. */
static int checkResidue(unsigned char *block, int base) {
    for (int i = 0; i < 8; ++i) block[i] = 0xA5;
    block[3] = 0x11;
    block[4] = 0x11;
    unsigned char expected[2] = {0x11, 0x11};
    unsigned char desired[2]  = {0x22, 0x22};
    if (__atomic_compare_exchange(2, block + 3, expected, desired, 5, 5) != 1) return base + 1;
    if (expected[0] != 0x11 || expected[1] != 0x11) return base + 2;
    if (block[3] != 0x22 || block[4] != 0x22) return base + 3;
    unsigned char stale[2] = {0x11, 0x11};
    unsigned char other[2] = {0x33, 0x33};
    if (__atomic_compare_exchange(2, block + 3, stale, other, 5, 5) != 0) return base + 4;
    if (stale[0] != 0x22 || stale[1] != 0x22) return base + 5;
    if (block[3] != 0x22 || block[4] != 0x22) return base + 6;
    if (block[2] != 0xA5 || block[5] != 0xA5) return base + 7;
    return 0;
}

int main(void) {
    struct P4 *const crossing4 = (struct P4 *)(void *)(lineStart(1) + 61u);
    struct P8 *const crossing8 = (struct P8 *)(void *)(lineStart(3) + 57u);
    struct P4 *const inside4   = (struct P4 *)(void *)(lineStart(5) + 53u);
    struct P8 *const inside8   = (struct P8 *)(void *)(lineStart(7) + 49u);
    unsigned char *const block = (unsigned char *)(void *)lineStart(9);
    int rc = check4(crossing4, 110);
    if (rc != 0) return rc;
    rc = check8(crossing8, 120);
    if (rc != 0) return rc;
    rc = check4(inside4, 130);
    if (rc != 0) return rc;
    rc = check8(inside8, 140);
    if (rc != 0) return rc;
    rc = checkResidue(block, 150);
    if (rc != 0) return rc;
    return 42;
}
