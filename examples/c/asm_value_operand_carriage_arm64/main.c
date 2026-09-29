/* D-MIR-ASM-BY-ADDRESS-OPERAND-BOUND-TO-A-REGISTER-RECEIVES-ITS-ADDRESS and
 * D-ASM-MULTI-REGISTER-OPERAND-BINDING-NOT-REALIZED — aarch64.
 *
 * A struct, a union, a `_Complex` or a 128-bit integer lives in MEMORY in this
 * compiler's middle end. Bound to an asm REGISTER constraint it must reach the
 * template as its VALUE: loaded into the register(s) before the template and
 * stored back after it. Before the fix the template received the value's
 * ADDRESS (`__int128` and `_Complex float` on "r": rc=0, the wrong bits, debug
 * and release) or only its first 8 bytes (a 16-byte struct on "w").
 *
 * gcc binds a value TWICE a register wide to ONE "r" operand as a register
 * PAIR, and `%H0` names the second register. aarch64-linux-gnu-gcc 13.3.0 runs
 * this file to 42 at -O0 and -O2 under qemu-aarch64 (clang 18.1.3 refuses
 * `%H`). Every check is BITWISE over the value's own bytes, built from
 * patterns whose two halves differ, so an address, a half, or the two halves
 * swapped cannot pass; each failing check has its own exit code.
 *
 * The binary128 `long double` shapes are Linux-only: on Apple arm64 a
 * `long double` IS a `double`, a different operand altogether.
 */
#include <string.h>

typedef unsigned long u64;
static const u64 W0 = 0x0123456789ABCDEFul, W1 = 0x0FEDCBA987654321ul;

struct pair { u64 a, b; };

static __int128 make128(u64 lo, u64 hi) {
    __int128 v; u64 p[2] = { lo, hi }; memcpy(&v, p, 16); return v;
}
static void halves(void const *v, u64 *lo, u64 *hi) {
    u64 p[2]; memcpy(p, v, 16); *lo = p[0]; *hi = p[1];
}

/* 1. a PAIR read: `%2` is the low register, `%H2` the high one */
__attribute__((noinline)) static int pair_in(__int128 v) {
    u64 a, b;
    __asm__("mov %0, %2\n\tmov %1, %H2" : "=&r"(a), "=&r"(b) : "r"(v));
    return a == W0 && b == W1;
}

/* 2. a PAIR written, both halves */
__attribute__((noinline)) static int pair_out(u64 x, u64 y) {
    __int128 v;
    __asm__("mov %0, %1\n\tmov %H0, %2" : "=&r"(v) : "r"(x), "r"(y));
    u64 lo, hi; halves(&v, &lo, &hi);
    return lo == x && hi == y;
}

/* 3. a PAIR read AND written: the two halves swapped in place */
__attribute__((noinline)) static int pair_inout(__int128 v) {
    __asm__("eor %0, %0, %H0\n\teor %H0, %H0, %0\n\teor %0, %0, %H0" : "+r"(v));
    u64 lo, hi; halves(&v, &lo, &hi);
    return lo == W1 && hi == W0;
}

/* 4. a 16-byte STRUCT on a pair, beside register clobbers */
__attribute__((noinline)) static int struct_pair_clobbers(struct pair s) {
    __asm__("add %0, %0, #1\n\tadd %H0, %H0, #2" : "+r"(s) : : "x9", "x10", "x11");
    return s.a == W0 + 1 && s.b == W1 + 2;
}

/* 5. a `_Complex double` on a pair: the real part low, the imaginary high */
__attribute__((noinline)) static int complex_pair_in(_Complex double z) {
    u64 re, im, want[2];
    __asm__("mov %0, %2\n\tmov %1, %H2" : "=&r"(re), "=&r"(im) : "r"(z));
    memcpy(want, &z, 16);
    return re == want[0] && im == want[1];
}

/* 6. a `_Complex float` in ONE register */
__attribute__((noinline)) static int complex_float_in(_Complex float z) {
    u64 a, want;
    __asm__("mov %0, %1" : "=r"(a) : "r"(z));
    memcpy(&want, &z, 8);
    return a == want;
}

/* 7. a 4-byte struct in a SIMD&FP register, overwritten by a float's bits */
struct four { unsigned char b[4]; };
__attribute__((noinline)) static int four_on_w(struct four s, float f) {
    __asm__("fmov %s0, %s1" : "+w"(s) : "w"(f));
    unsigned fb, sb; memcpy(&fb, &f, 4); memcpy(&sb, &s, 4);
    return sb == fb;
}

/* 8. an `__int128` in ONE Q register */
__attribute__((noinline)) static int q_copy(__int128 v, __int128 w) {
    __asm__("mov %0.16b, %1.16b" : "+w"(v) : "w"(w));
    u64 lo, hi; halves(&v, &lo, &hi);
    return lo == W1 && hi == W0;
}

/* 9. `asm goto` with a PAIR output read on BOTH edges */
__attribute__((noinline)) static int goto_pair(u64 x, long take) {
    __int128 v;
    __asm__ goto("mov %0, %1\n\tmov %H0, %1\n\tcmp %2, #0\n\tb.ne %l[yes]"
                 : "=&r"(v) : "r"(x), "r"(take) : "cc" : yes);
    { u64 lo, hi; halves(&v, &lo, &hi); return (lo == x && hi == x) ? 1 : 0; }
yes:
    { u64 lo, hi; halves(&v, &lo, &hi); return (lo == x && hi == x) ? 2 : 0; }
}

/* 10. an RVALUE of a memory-resident kind: a product, never an lvalue */
__attribute__((noinline)) static int rvalue_pair(__int128 x, __int128 y) {
    u64 a, b, want[2];
    __asm__("mov %0, %2\n\tmov %1, %H2" : "=&r"(a), "=&r"(b) : "r"(x * y));
    __int128 p = x * y; memcpy(want, &p, 16);
    return a == want[0] && b == want[1];
}

#if !defined(__APPLE__)
/* 11. a binary128 `long double` on a GENERAL-register pair */
__attribute__((noinline)) static int ldouble_pair(long double x) {
    u64 a, b, want[2];
    __asm__("mov %0, %2\n\tmov %1, %H2" : "=&r"(a), "=&r"(b) : "r"(x));
    memcpy(want, &x, 16);
    return a == want[0] && b == want[1];
}

/* 12. a `_Complex long double` on a Q-register PAIR: the first Q's low half
 *     kept and its upper half zeroed by the template, the second Q untouched */
__attribute__((noinline)) static int complex_ld_q_pair(_Complex long double z) {
    u64 before[4], after[4];
    memcpy(before, &z, 32);
    __asm__("fmov %d0, %d0" : "+w"(z));
    memcpy(after, &z, 32);
    return after[0] == before[0] && after[1] == 0
        && after[2] == before[2] && after[3] == before[3];
}
#endif

int main(void) {
    if (!pair_in(make128(W0, W1))) return 11;
    if (!pair_out(W0, W1)) return 12;
    if (!pair_inout(make128(W0, W1))) return 13;
    { struct pair s = { W0, W1 }; if (!struct_pair_clobbers(s)) return 14; }
    {
        _Complex double z; u64 p[2] = { W0, W1 }; memcpy(&z, p, 16);
        if (!complex_pair_in(z)) return 15;
    }
    {
        _Complex float z; u64 p = W0; memcpy(&z, &p, 8);
        if (!complex_float_in(z)) return 16;
    }
    {
        struct four s = { { 1, 2, 3, 4 } }; float f; unsigned fb = 0x40490FDBu;
        memcpy(&f, &fb, 4);
        if (!four_on_w(s, f)) return 17;
    }
    if (!q_copy(make128(1, 2), make128(W1, W0))) return 18;
    if (goto_pair(W0, 0) != 1 || goto_pair(W1, 1) != 2) return 19;
    if (!rvalue_pair(make128(W0, 3), make128(W1, 5))) return 20;
#if !defined(__APPLE__)
    {
        long double x; u64 p[2] = { W0, W1 }; memcpy(&x, p, 16);
        if (!ldouble_pair(x)) return 21;
    }
    {
        _Complex long double z; u64 p[4] = { W0, W1, W1, W0 }; memcpy(&z, p, 32);
        if (!complex_ld_q_pair(z)) return 22;
    }
#endif
    return 42;
}
