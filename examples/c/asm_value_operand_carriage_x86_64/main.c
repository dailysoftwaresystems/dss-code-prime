/* D-MIR-ASM-BY-ADDRESS-OPERAND-BOUND-TO-A-REGISTER-RECEIVES-ITS-ADDRESS and
 * D-ASM-MULTI-REGISTER-OPERAND-BINDING-NOT-REALIZED — x86_64.
 *
 * A struct, a union, a `_Complex` or a 128-bit integer lives in MEMORY in this
 * compiler's middle end. Bound to an asm REGISTER constraint it must reach the
 * template as its VALUE: loaded into the register(s) before the template and
 * stored back after it. Before the fix an `__int128` or a `_Complex float` on
 * "r" reached the template as its ADDRESS (rc=0, the wrong bits, debug and
 * release).
 *
 * gcc binds a 16-byte value to ONE "r" operand as a register PAIR; AT&T gives
 * the template a name only for the LOW register (`%0`), so the high half is
 * carried untouched through a `+` operand. A value in an XMM register ("x") is
 * moved by the SSE instruction the template names. gcc 13.3.0 runs this file
 * to 42 at -O0 and -O2 (clang 18.1.3 refuses the 16-byte struct and complex
 * inputs, "indirect register inputs"). Every check is BITWISE over the value's
 * own bytes; each failing check has its own exit code.
 *
 * The x87 `long double` shape is ELF-only: under the Windows x64 ABI a
 * `long double` IS a `double`, a different operand altogether.
 */
#include <string.h>

typedef unsigned long long u64;
static const u64 W0 = 0x0123456789ABCDEFull, W1 = 0x0FEDCBA987654321ull;

struct pair { u64 a, b; };
struct eight { unsigned int a, b; };
struct four { unsigned char b[4]; };
struct two { unsigned char b[2]; };
struct one { unsigned char b[1]; };

static __int128 make128(u64 lo, u64 hi) {
    __int128 v; u64 p[2] = { lo, hi }; memcpy(&v, p, 16); return v;
}
static void halves(void const *v, u64 *lo, u64 *hi) {
    u64 p[2]; memcpy(p, v, 16); *lo = p[0]; *hi = p[1];
}

/* 1. a PAIR read through its low register */
__attribute__((noinline)) static int pair_in(__int128 v) {
    u64 a;
    __asm__("movq %1, %0" : "=r"(a) : "r"(v));
    return a == W0;
}

/* 2. a PAIR read-modify-written: the low half incremented, the high half
 *    carried untouched through the register the template cannot name */
__attribute__((noinline)) static int pair_inout(__int128 v) {
    __asm__("addq $1, %0" : "+r"(v));
    u64 lo, hi; halves(&v, &lo, &hi);
    return lo == W0 + 1 && hi == W1;
}

/* 3. a 16-byte STRUCT on a pair, beside a register clobber. (Through a
 *    pointer: gcc -O2 cannot print a 16-byte operand it allocates to r8..r15 —
 *    "unsupported operand size for extended register" — and a by-value
 *    parameter steers it there.) */
__attribute__((noinline)) static void struct_bump(struct pair *p) {
    struct pair s = *p;
    __asm__("addq $1, %0" : "+r"(s) : : "rbx");
    *p = s;
}
__attribute__((noinline)) static int struct_pair_clobbers(struct pair s) {
    struct_bump(&s);
    return s.a == W0 + 1 && s.b == W1;
}

/* 4. a `_Complex double` on a pair: the real part is the low register */
__attribute__((noinline)) static int complex_pair_in(_Complex double z) {
    u64 re, want[2];
    __asm__("movq %1, %0" : "=r"(re) : "r"(z));
    memcpy(want, &z, 16);
    return re == want[0];
}

/* 5. small structs in ONE general register, every direction */
__attribute__((noinline)) static int small_structs(struct one o, struct two t, struct four f, struct eight e) {
    u64 a;
    __asm__("movzbq %1, %0" : "=r"(a) : "r"(o));
    if (a != o.b[0]) return 0;
    __asm__("movl %k1, %k0" : "=r"(t) : "r"(0x1234u));
    if (t.b[0] != 0x34 || t.b[1] != 0x12) return 0;
    __asm__("addl $1, %k0" : "+r"(f));
    if (f.b[0] != 2 || f.b[1] != 2 || f.b[2] != 3 || f.b[3] != 4) return 0;
    __asm__("addq $1, %0" : "+r"(e));
    return e.a == 42 && e.b == 7;
}

/* 6. a `_Complex float` in ONE general register */
__attribute__((noinline)) static int complex_float_in(_Complex float z) {
    u64 a, want;
    __asm__("movq %1, %0" : "=r"(a) : "r"(z));
    memcpy(&want, &z, 8);
    return a == want;
}

/* 7. an `__int128` in ONE XMM register, overwritten by another */
__attribute__((noinline)) static int xmm_copy(__int128 v, __int128 w) {
    __asm__("movaps %1, %0" : "+x"(v) : "x"(w));
    u64 lo, hi; halves(&v, &lo, &hi);
    return lo == W1 && hi == W0;
}

/* 8. structs of 8, 4 and 2 bytes in XMM registers */
__attribute__((noinline)) static int xmm_structs(double d, float f, struct two t) {
    struct eight e;
    __asm__("movsd %1, %0" : "=x"(e) : "x"(d));
    u64 eb, db; memcpy(&eb, &e, 8); memcpy(&db, &d, 8);
    if (eb != db) return 0;
    struct four s = { { 9, 9, 9, 9 } };
    __asm__("movss %1, %0" : "+x"(s) : "x"(f));
    unsigned sb, fb; memcpy(&sb, &s, 4); memcpy(&fb, &f, 4);
    if (sb != fb) return 0;
    /* a 2-byte struct rides an XMM register through a general one (gcc:
     * `movzwl` + `movd`); an empty template must hand it back unchanged */
    struct two t2 = t;
    __asm__ volatile("nop" : "+x"(t2));
    return t2.b[0] == t.b[0] && t2.b[1] == t.b[1];
}

/* 9. a PINNED letter with a struct: `"+a"` — the struct IS %rax */
__attribute__((noinline)) static int pinned_struct(struct eight e) {
    __asm__("addq $1, %%rax" : "+a"(e));
    return e.a == 42 && e.b == 7;
}

/* 10. `asm goto` with a struct PAIR output read on BOTH edges */
__attribute__((noinline)) static int goto_pair(u64 x, int take) {
    struct pair v;
    __asm__ goto("movq %1, %0\n\tcmpl $0, %2\n\tjne %l[yes]"
                 : "=&r"(v) : "r"(x), "r"(take) : "cc" : yes);
    return v.a == x ? 1 : 0;
yes:
    return v.a == x ? 2 : 0;
}

/* 11. an RVALUE of a memory-resident kind: a call's struct result */
__attribute__((noinline)) static struct pair mk(u64 a, u64 b) {
    struct pair p = { a, b }; return p;
}
__attribute__((noinline)) static int rvalue_struct(void) {
    u64 a;
    __asm__("movq %1, %0" : "=r"(a) : "r"(mk(W0, W1)));
    return a == W0;
}

#if !defined(_WIN64)
/* 12. an x87 `long double` (64-bit significand + 16-bit sign/exponent) on a
 *     general-register PAIR: the significand incremented, the rest untouched */
__attribute__((noinline)) static void x87_bump(long double *p) {
    long double x = *p;
    __asm__("addq $1, %0" : "+r"(x));
    *p = x;
}
__attribute__((noinline)) static int x87_pair(long double x) {
    u64 before[2], after[2];
    memcpy(before, &x, 16);
    x87_bump(&x);
    memcpy(after, &x, 16);
    return after[0] == before[0] + 1
        && (after[1] & 0xFFFFull) == (before[1] & 0xFFFFull);
}
#endif

int main(void) {
    if (!pair_in(make128(W0, W1))) return 11;
    if (!pair_inout(make128(W0, W1))) return 12;
    { struct pair s = { W0, W1 }; if (!struct_pair_clobbers(s)) return 13; }
    {
        _Complex double z; u64 p[2] = { W0, W1 }; memcpy(&z, p, 16);
        if (!complex_pair_in(z)) return 14;
    }
    {
        struct one o = { { 0xA5 } }; struct two t = { { 0, 0 } };
        struct four f = { { 1, 2, 3, 4 } }; struct eight e = { 41, 7 };
        if (!small_structs(o, t, f, e)) return 15;
    }
    {
        _Complex float z; u64 p = W0; memcpy(&z, &p, 8);
        if (!complex_float_in(z)) return 16;
    }
    if (!xmm_copy(make128(1, 2), make128(W1, W0))) return 17;
    {
        double d; u64 db = W1; memcpy(&d, &db, 8);
        float f; unsigned fb = 0x40490FDBu; memcpy(&f, &fb, 4);
        struct two t = { { 0x5A, 0xC3 } };
        if (!xmm_structs(d, f, t)) return 18;
    }
    { struct eight e = { 41, 7 }; if (!pinned_struct(e)) return 19; }
    if (goto_pair(W0, 0) != 1 || goto_pair(W1, 1) != 2) return 20;
    if (!rvalue_struct()) return 21;
#if !defined(_WIN64)
    {
        long double x; u64 p[2] = { W0, 0x3FFFull }; memcpy(&x, p, 16);
        if (!x87_pair(x)) return 22;
    }
#endif
    return 42;
}
