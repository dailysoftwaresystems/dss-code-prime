/* D-C-ATOMIC-COMPOUND-ASSIGNMENT-AND-INCREMENT-ARE-A-LOAD-THEN-A-SEPARATE-STORE —
 * the THREADED witness for an UNDER-ALIGNED `_Atomic` object: a packed member
 * placed so its four bytes STRADDLE a cache line.
 *
 * ★★★ WHY A SEPARATE EXAMPLE. An under-aligned `_Atomic` access does not take the
 * native instruction pair — it calls the object format's GENERIC atomics runtime
 * (`__atomic_load`/`__atomic_store`, D-CSUBSET-PACKED-ATOMIC-MEMBER), whose lock
 * table is what arbitrates the object. A compound assignment on such an object was
 * a runtime LOAD then a separate runtime STORE (✔MEASURED by disassembly of the
 * DSS elf64 build at `caf053eb`: two PLT calls, `__atomic_load` then
 * `__atomic_store`), and it lost updates exactly like the aligned forms. It is now
 * the same retry loop, and its COMMIT goes through the runtime's
 * `__atomic_compare_exchange` — under the SAME lock as its load. A native
 * compare-exchange committing a runtime-loaded value would be two arbiters for one
 * object. ✔The references agree the entry is the answer here: clang 18 and Apple
 * clang 21 both call `__atomic_compare_exchange` for this very lvalue.
 *
 * ★★ THE OBJECT STRADDLES A CACHE LINE ON PURPOSE (the placement
 * `packed_atomic_member_concurrency` uses): 61 bytes past a 64-byte-aligned address
 * inside a byte arena, so the `_Atomic unsigned` sits at byte 62 of a line. Reached
 * through a `struct Packed *`, so the lvalue's PROVABLE alignment stays 1 and the
 * address cannot be folded away.
 *
 * ★ EVERY VERDICT IS AN EXACT VALUE: `+= 1` and `++` → 2N each, `*= 3` →
 * 3^(2N) mod 2^32, and a packed `_Atomic double` `+= 1.0` → exactly 2N (an
 * integer far below 2^53). Any lost update moves it; no interleaving can.
 * ⚠ The arm64 qemu leg does not enforce the alignment check (P53: one binary exits
 * 42 under qemu and dies rc 135 on native aarch64), so its green is a
 * lost-update witness there, never an alignment-safety one.
 *
 * exit = 42; each arm returns its own code on failure.
 */
#include <stdint.h>
#include <threads.h>

#define kRounds 50000u

struct __attribute__((packed)) Packed {
    char              c;
    _Atomic unsigned  a;
};

/* ★ THE FLOATING ARM. An under-aligned `_Atomic double` takes BOTH new routes at
 * once: its read-modify-write runs over the 64-bit representation (a retyped
 * address and a cross-class bit move at each end), AND that representation's load
 * and commit go through the atomics runtime because the member is provably
 * 1-aligned. A witness of either route alone would say nothing about the pair. */
struct __attribute__((packed)) PackedD {
    char              c;
    _Atomic double    d;
};

static unsigned char   g_arena_add[256];
static unsigned char   g_arena_inc[256];
static unsigned char   g_arena_mul[256];
static unsigned char   g_arena_dbl[256];
static struct Packed  *g_add;
static struct Packed  *g_inc;
static struct Packed  *g_mul;
static struct PackedD *g_dbl;

static uintptr_t straddling_base(unsigned char *arena) {
    return ((uintptr_t)(void *)arena + 63u) & ~(uintptr_t)63u;
}
static struct Packed *straddling(unsigned char *arena) {
    return (struct Packed *)(void *)(straddling_base(arena) + 61u);
}
/* The double's eight bytes span the line boundary too: it starts at byte 60. */
static struct PackedD *straddling_d(unsigned char *arena) {
    return (struct PackedD *)(void *)(straddling_base(arena) + 59u);
}

static int add_worker(void *arg) { (void)arg; for (unsigned i = 0; i < kRounds; ++i) g_add->a += 1u; return 0; }
static int inc_worker(void *arg) { (void)arg; for (unsigned i = 0; i < kRounds; ++i) ++g_inc->a; return 0; }
static int mul_worker(void *arg) { (void)arg; for (unsigned i = 0; i < kRounds; ++i) g_mul->a *= 3u; return 0; }
static int dbl_worker(void *arg) { (void)arg; for (unsigned i = 0; i < kRounds; ++i) g_dbl->d += 1.0; return 0; }

int main(void) {
    g_add = straddling(g_arena_add);
    g_inc = straddling(g_arena_inc);
    g_mul = straddling(g_arena_mul);
    g_dbl = straddling_d(g_arena_dbl);
    g_add->c = 0x5A; g_add->a = 0u;
    g_inc->c = 0x5A; g_inc->a = 0u;
    g_mul->c = 0x5A; g_mul->a = 1u;
    g_dbl->c = 0x5A; g_dbl->d = 0.0;

    /* ── single-threaded: the yielded values through the runtime-routed loop ── */
    if (g_add->a++ != 0u) return 1;          /* postfix yields the OLD value */
    if (++g_add->a != 2u) return 2;          /* prefix yields the NEW value  */
    if ((g_add->a += 5u) != 7u) return 3;    /* compound yields the NEW value */
    if (g_add->c != 0x5A) return 4;          /* the neighbour byte is not ours */
    g_add->a = 0u;
    if ((g_dbl->d += 2.5) != 2.5 || g_dbl->d-- != 2.5 || g_dbl->d != 1.5) return 9;
    if (g_dbl->c != 0x5A) return 10;
    g_dbl->d = 0.0;

    int (*const workers[4])(void *) = { add_worker, inc_worker, mul_worker, dbl_worker };
    thrd_t threads[8];
    for (int i = 0; i < 8; ++i) {
        if (thrd_create(&threads[i], workers[i / 2], (void *)0) != thrd_success) return 90;
    }
    int rc = 0;
    for (int i = 0; i < 8; ++i) {
        if (thrd_join(threads[i], &rc) != thrd_success) return 91;
    }

    unsigned mul_want = 1u;
    for (unsigned i = 0; i < 2u * kRounds; ++i) mul_want *= 3u;

    if (g_add->a != 2u * kRounds) return 5;
    if (g_inc->a != 2u * kRounds) return 6;
    if (g_mul->a != mul_want)     return 7;
    if (g_add->c != 0x5A || g_inc->c != 0x5A || g_mul->c != 0x5A) return 8;
    if (g_dbl->d != 2.0 * kRounds) return 11;
    if (g_dbl->c != 0x5A)         return 12;
    return 42;
}
