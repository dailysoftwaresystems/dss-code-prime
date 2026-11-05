/* A MEMORY-CLASS AGGREGATE ARGUMENT TRAVELS BY ITS CALLING CONVENTION'S CARRIER, AND AN
 * X87-CLASS RESULT COMES BACK AS A SCALAR LONG DOUBLE (P69 round 1, lane cs, fold f26c,
 * D-TARGET-SYSV-AMD64-MEMORY-CLASS-ARGUMENTS-GO-BY-POINTER-AND-X87-RESULTS-BY-SRET).
 *
 * x86_64 SysV (psABI 3.2.3, `aggregateMemoryArgument: stack`): a struct over 16 bytes is
 * passed as its BYTES in the outgoing argument area, at its own alignment, consuming NO
 * register (the `long` after it still takes the next free one); `struct { long double x; }`
 * is passed the same way and RETURNED in %st0. Win64 and AAPCS64 (`callerCopy`) pass a
 * pointer to a caller copy. Before the fold DSS passed the SysV struct as a pointer in the
 * next integer register and returned the x87 struct through a hidden pointer — what gcc and
 * clang never read (MEASURED 2026-10-07, lane lm's two-unit witnesses); the cross-compiler
 * half is pinned by the native two-unit cells in tests/link, since an example links DSS
 * with DSS. Here both sides are DSS: every call returns its value only if the caller's
 * placement and the callee's reception agree — at +0, after an odd number of stacked
 * eightbytes (a 16-aligned struct begins at +16), with a 32-aligned member, through a
 * variadic fixed parameter and through va_arg. Values route through MUTABLE globals so the
 * release arm keeps runtime calls, and it INLINES the inlinable callees: the inliner maps a
 * stacked reception to the caller's carrier, so the spliced bodies must read the same
 * values. Every return before 42 names the first fact that failed. */
#include <stdarg.h>

struct M24 { long a; long b; long c; };          /* 24 bytes: MEMORY class         */
struct LD  { long double x; };                   /* SysV X87 class                 */
struct A16 { long double x; long y; };           /* 32 bytes, 16-aligned on SysV   */
struct A32 { _Alignas(32) long v; long w; };     /* 32 bytes, 32-aligned           */
union  U2  { long double a; long double b; };    /* SysV X87 class (a union)        */

long g1 = 1, g2 = 2, g3 = 3, g4 = 4, g7 = 7;
long double gd = 5;

long take_m24(struct M24 m, long x) { return m.a + 10 * m.b + 100 * m.c + 1000 * x; }

__attribute__((noinline)) long mixed(long a, struct M24 m, double d, long b) {
    return a + 10 * m.a + 100 * m.c + 1000 * (long)d + 10000 * b;
}

struct LD mk_ld(long double v) { struct LD r; r.x = v; return r; }
__attribute__((noinline)) struct LD mk_ld_ni(long double v) { struct LD r; r.x = v + 1; return r; }
long double get_ld(struct LD s) { return s.x; }
union U2 mk_u2(long double v) { union U2 u; u.a = v; return u; }

long after_odd(long a1, long a2, long a3, long a4, long a5, long a6, long s1, struct A16 v) {
    return a1 + a6 + 10 * s1 + 100 * v.y + 1000 * (long)v.x + 0 * (a2 + a3 + a4 + a5);
}

__attribute__((noinline)) long take_a32(long a1, long a2, long a3, long a4, long a5, long a6,
                                         long s1, struct A32 a) {
    return a1 + 10 * s1 + 100 * a.v + 1000 * a.w + 0 * (a2 + a3 + a4 + a5 + a6);
}

long var_m24(int n, ...) {
    va_list ap;
    va_start(ap, n);
    struct M24 m = va_arg(ap, struct M24);
    long r = va_arg(ap, long);
    va_end(ap);
    return m.a + 10 * m.b + 100 * m.c + 1000 * r + n;
}

long fixed_then_var(struct M24 m, ...) {
    va_list ap;
    va_start(ap, m);
    long r = va_arg(ap, long);
    va_end(ap);
    return m.c + 10 * r;
}

int main(void) {
    struct M24 m;
    m.a = g1; m.b = g2; m.c = g3;
    if (take_m24(m, g4) != 4321) return 1;
    if (mixed(g7, m, (double)g2, g4) != 7 + 10 + 300 + 2000 + 40000) return 2;
    if (get_ld(mk_ld(gd)) != 5) return 3;
    if (mk_ld_ni(gd).x != 6) return 4;
    struct LD l = mk_ld_ni(gd);
    if (get_ld(l) != 6) return 5;
    if (mk_u2(gd).b != 5) return 6;
    struct A16 v;
    v.x = gd; v.y = g3;
    if (after_odd(g1, g2, g3, g4, g1, g2, g7, v) != 1 + 2 + 70 + 300 + 5000) return 7;
    struct A32 a;
    a.v = g2; a.w = g3;
    if (take_a32(g1, g2, g3, g4, g1, g2, g4, a) != 1 + 40 + 200 + 3000) return 8;
    if (var_m24(9, m, g4) != 1 + 20 + 300 + 4000 + 9) return 9;
    if (fixed_then_var(m, g7) != 3 + 70) return 10;
    return 42;
}
