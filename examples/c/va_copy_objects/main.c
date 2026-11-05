/* P69 (lane `cs`) — D-C-STDARG-VA-COPY-MISSING: C99 7.15.1.2 / C23 7.16.1.2 `va_copy(dest,
 * src)` makes `dest` a COPY of `src` — the same position in the same argument list, each
 * then advanced on its own. Seven shapes; exit 42 when all seven agree, otherwise a bitmask
 * of the shapes that broke:
 *   1 a copy taken at va_start, both walked to the end,
 *   2 a copy taken MID-WALK (it starts where the source had got to),
 *   4 doubles (the SysV floating-point cursor is a separate field of the tag),
 *   8 more integers than argument registers (the SysV overflow cursor),
 *   16 a copy of a `va_list` PARAMETER — on SysV that parameter is a pointer to the
 *      caller's tag, and the copy must be the TAG, not the pointer,
 *   32 <stdarg.h> makes `va_copy` a MACRO (C 7.16), so the portable `#ifdef va_copy`
 *      test finds it — shape 2 takes the GNU `__va_copy` spelling where it exists,
 *   64 a copy INTO a `va_list` PARAMETER — the other side of shape 16: on SysV the copy
 *      fills the caller's tag the parameter points at, never the pointer itself.
 * Every operand comes from argc, so no shape folds. */
#include <stdarg.h>
#include <string.h>

static int sum_twice(int n, ...) {
    va_list a, b;
    va_start(a, n);
    va_copy(b, a);
    int s = 0;
    for (int i = 0; i < n; ++i) s += va_arg(a, int);
    for (int i = 0; i < n; ++i) s += va_arg(b, int);
    va_end(b);
    va_end(a);
    return s;
}

static int mid_walk(int n, ...) {
    va_list a, b;
    va_start(a, n);
    int first = va_arg(a, int);
#ifdef __va_copy
    __va_copy(b, a);   /* gcc's and clang's <stdarg.h> spell it too; MSVC's does not */
#else
    va_copy(b, a);
#endif
    int ra = 0, rb = 0;
    for (int i = 1; i < n; ++i) ra += va_arg(a, int);
    for (int i = 1; i < n; ++i) rb += va_arg(b, int);
    va_end(b);
    va_end(a);
    return first * 100 + (ra == rb ? ra : -1);
}

static double dsum_twice(int n, ...) {
    va_list a, b;
    va_start(a, n);
    va_copy(b, a);
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += va_arg(a, double);
    for (int i = 0; i < n; ++i) s += va_arg(b, double);
    va_end(b);
    va_end(a);
    return s;
}

static int helper(va_list ap, int n) {
    va_list c;
    va_copy(c, ap);
    int s = 0;
    for (int i = 0; i < n; ++i) s += va_arg(c, int);
    va_end(c);
    return s;
}

static int through_param(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int const first = helper(ap, n);
    int const second = helper(ap, n);
    va_end(ap);
    return first == second ? first : -1;
}

static int into_param(va_list dst, int n, ...) {
    va_list src;
    va_start(src, n);
    va_copy(dst, src);
    int s = 0;
    for (int i = 0; i < n; ++i) s += va_arg(dst, int);
    va_end(dst);
    int t = 0;
    for (int i = 0; i < n; ++i) t += va_arg(src, int);   /* the source never moved */
    va_end(src);
    return s == t ? s : -1;
}

int main(int argc, char **argv) {
    (void)argv;
    int const k = argc;   /* 1 under every runner */
    int bad = 0;
    if (sum_twice(3, 5 * k, 7 * k, 9 * k) != 42) bad |= 1;
    if (mid_walk(4, 4 * k, 1 * k, 2 * k, 3 * k) != 406) bad |= 2;
    if (dsum_twice(3, 1.5 * k, 2.25 * k, 17.25 * k) != 42.0) bad |= 4;
    if (sum_twice(10, k, 2 * k, k, 2 * k, k, 2 * k, k, 2 * k, k, 8 * k) != 42) bad |= 8;
    if (through_param(3, 10 * k, 11 * k, 0 * k) != 21) bad |= 16;
#ifndef va_copy
    bad |= 32;
#endif
    va_list scratch;
    memset(&scratch, 0, sizeof scratch);   /* a determinate value to pass, on every ABI */
    if (into_param(scratch, 3, 12 * k, 13 * k, 17 * k) != 42) bad |= 64;
    return bad == 0 ? 42 : bad;
}
