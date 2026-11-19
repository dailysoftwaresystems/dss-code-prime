/* P69 (lane `cs`) — D-CSUBSET-GNUC-PREDEFINE-SELECTS-UNIMPLEMENTED-BUILTIN: the checked-arithmetic
 * and stack-allocation builtins `__GNUC__` routes real code into. Exit 42 when each behaves as
 * gcc and clang define it, otherwise a bitmask of the groups that did not:
 *   1 `__builtin_{add,sub,mul}_overflow` at INFINITE precision over mixed operand and result
 *     types — int, long long, unsigned, unsigned char, unsigned long long; the u64 × u64 and
 *     i64 × i64 products that need a 128-bit exact result
 *   2 the typed spellings, one of each family (s/u × add/sub/mul × int/long/long long)
 *   4 a `_Bool` and an enumeration result — clang's meaning (the `_Bool` keeps the exact
 *     result's low bit); gcc refuses both, so this group runs only under `__clang__` (which DSS
 *     defines too)
 *   8 `__builtin_alloca`: 16-aligned, distinct, every block live until the function returns
 *     (allocated in a loop)
 *   16 memory alloca'd INSIDE a VLA scope survives that scope's exit (gcc's documented rule)
 *   32 ... and survives a `goto` out of that scope taken BEFORE the alloca runs again: the
 *      scope is pinned as a whole, from its first statement, not from the alloca on (clang at
 *      -O0 frees the blocks here; gcc keeps them, and the union keeps what one keeps)
 *   64 a pinned scope still frees the VLAs of the unpinned scopes inside it, on a `continue`
 *      and on a `goto`: 200 iterations of a 64 KiB inner array (12.5 MiB, were none freed)
 *      would overflow the stack. Each array is sixteen pages, so on Windows every
 *      iteration's descent walks the guard pages (D-CSUBSET-VLA-WIN64-STACK-PROBE)
 * Every operand is derived from argc, so no answer is a constant the optimizer could fold.
 * Under a compiler defining no `__GNUC__` (MSVC 19.51) the file is a bare `return 42`. */
#if defined(__GNUC__)
#include <limits.h>

/* Writes zeros over the stack below the caller's frame, one volatile store at a time: a block
 * freed under the caller is overwritten before the caller reads it again. */
static __attribute__((noinline)) void scrub(int one) {
    volatile char buf[4096];
    for (int i = 0; i < (int)sizeof buf; ++i) buf[i] = (char)(one - 1);
}

static __attribute__((noinline)) int alloca_past_a_goto(int one) {
    char *p[2] = {0, 0};
    {
        char vla[one * 16];
        __builtin_memset(vla, 1, sizeof vla);
        for (int i = 0;; ++i) {
            if (i == 2) goto done;
            p[i] = __builtin_alloca(16);
            __builtin_memset(p[i], 7, 16);
        }
    }
done:
    scrub(one);
    return p[0][0] != 7 || p[1][15] != 7;
}

static __attribute__((noinline)) int pinned_scope_frees_inner_vlas(int one, int iters) {
    long sum = 0;
    for (int i = 0; i < iters; ++i) {          /* `continue` out of the inner scope */
        char v[one * 16];
        v[0] = 1;
        char *p = __builtin_alloca(1);
        p[0] = 1;
        {
            char w[one * 65536];
            __builtin_memset(w, 1, sizeof w);
            if (one > 0) { sum += w[sizeof w - 1] + v[0] + p[0]; continue; }
        }
        sum -= 1000;
    }
    for (int i = 0; i < iters; ++i) {          /* `goto` out of a pinned scope and the inner one */
        {
            char v[one * 16];
            v[0] = 1;
            char *p = __builtin_alloca(1);
            p[0] = 1;
            {
                char w[one * 65536];
                __builtin_memset(w, 1, sizeof w);
                sum += w[0] + v[0] + p[0];
                if (one > 0) goto next;
            }
            sum -= 1000;
        }
    next:;
    }
    return sum != 6L * iters;
}

static __attribute__((noinline)) int alloca_in_a_loop(int one) {
    char *keep[8];
    int bad = 0;
    for (int i = 0; i < 8; ++i) {
        keep[i] = __builtin_alloca((unsigned long)(one * 24 + i));
        __builtin_memset(keep[i], i + 1, 24);
        if (((unsigned long long)keep[i] & 15u) != 0) bad = 1;
    }
    for (int i = 0; i < 8; ++i)
        if (keep[i][23] != i + 1) bad = 1;
    for (int i = 1; i < 8; ++i)
        if (keep[i] == keep[i - 1]) bad = 1;
    return bad;
}

static __attribute__((noinline)) int alloca_in_a_vla_scope(int one) {
    char *p = 0;
    {
        char vla[one * 16];
        __builtin_memset(vla, 0, sizeof vla);
        p = __builtin_alloca(32);
        __builtin_memset(p, 7, 32);
    }
    {
        char vla2[one * 64];
        __builtin_memset(vla2, 9, sizeof vla2);
    }
    return p[31] != 7;
}

int main(int argc, char **argv) {
    (void)argv;
    int bad = 0;
    int one = argc;   /* 1 under every runner */
    int r = 0;
    unsigned u = 0;
    unsigned char c = 0;
    long long ll = 0;
    unsigned long long ull = 0;

    int big = INT_MAX - 1 + one;
    if (!__builtin_add_overflow(big, one, &r) || r != INT_MIN) bad |= 1;
    if (__builtin_add_overflow(big, one, &ll) || ll != 2147483648LL) bad |= 1;
    if (!__builtin_sub_overflow(one - 1, one, &u) || u != UINT_MAX) bad |= 1;
    if (__builtin_mul_overflow(-one, -one, &u) || u != 1) bad |= 1;
    if (!__builtin_mul_overflow(0x10000 * one, 0x10000, &r) || r != 0) bad |= 1;
    if (__builtin_add_overflow(LLONG_MAX - 1 + one, LLONG_MAX, &ull)
        || ull != 0xfffffffffffffffeull) bad |= 1;
    if (!__builtin_add_overflow(200 * one, 100, &c) || c != 44) bad |= 1;
    if (__builtin_mul_overflow(LLONG_MIN + 1 - one, -one, &ull)
        || ull != 0x8000000000000000ull) bad |= 1;
    unsigned long long all = ~0ull + (unsigned long long)(one - 1);
    if (!__builtin_mul_overflow(all, 2ull, &ull) || ull != 0xfffffffffffffffeull) bad |= 1;
    if (__builtin_mul_overflow(all, (unsigned long long)one, &ull) || ull != ~0ull) bad |= 1;
    if (!__builtin_mul_overflow(LLONG_MAX, 2LL * one, &ll) || ll != -2) bad |= 1;

    long l = 0;
    unsigned long ul = 0;
    if (!__builtin_sadd_overflow(INT_MAX, one, &r)) bad |= 2;
    if (__builtin_saddl_overflow(1L, 2L * one, &l) || l != 3) bad |= 2;
    if (!__builtin_ssubll_overflow(LLONG_MIN, (long long)one, &ll)) bad |= 2;
    if (!__builtin_uadd_overflow(UINT_MAX, (unsigned)one, &u) || u != 0) bad |= 2;
    if (!__builtin_umull_overflow(ULONG_MAX, 2ul * (unsigned long)one, &ul)) bad |= 2;
    if (__builtin_umulll_overflow(3ull, 4ull * (unsigned long long)one, &ull) || ull != 12)
        bad |= 2;
    if (!__builtin_usub_overflow(0u, (unsigned)one, &u)) bad |= 2;
    if (!__builtin_smul_overflow(INT_MAX, 2 * one, &r)) bad |= 2;

#if defined(__clang__)
    _Bool b = 0;
    if (!__builtin_add_overflow(one, one, &b) || b != 0) bad |= 4;
    if (__builtin_add_overflow(one - 1, one, &b) || b != 1) bad |= 4;
    enum E { EA, EB } e = EA;
    if (__builtin_add_overflow(one, 0, &e) || e != EB) bad |= 4;
#endif

    if (alloca_in_a_loop(one)) bad |= 8;
    if (alloca_in_a_vla_scope(one)) bad |= 16;
    if (alloca_past_a_goto(one)) bad |= 32;
    if (pinned_scope_frees_inner_vlas(one, 200 * one)) bad |= 64;
    return bad == 0 ? 42 : bad;
}
#else
int main(void) { return 42; }
#endif
