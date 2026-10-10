/* P69 (lane `cs`) — D-C-A-VA-LIST-REACHED-THROUGH-ITS-DECAYED-POINTER-READS-THE-POINTER-VARIABLE:
 * on SysV x86-64 a `va_list` is an ARRAY of one tag (`__va_list_tag[1]`), so the list can be
 * reached through its DECAYED pointer — a pointer to the tag — and every va_* operation made
 * through that pointer walks the SAME list. DSS read the pointer VARIABLE's own slot as the tag
 * (a wrong answer, in silence) and refused a conditional of two lists. Where the list is not an
 * array (Win64's and Apple's `char *`, AAPCS64's structure) none of these shapes exists, so the
 * program checks nothing there and exits 42. On SysV x86-64: exit 42 when every shape agrees,
 * otherwise a bitmask of the shapes that broke:
 *   1 va_arg through `__typeof__(&a[0]) p = a` (p and a are ONE list),
 *   2 va_copy INTO such a pointer (the copy fills the tag it points at),
 *   4 va_copy FROM such a pointer,
 *   8 a PARAMETER of the decayed type, walked by va_arg,
 *   16 a CONDITIONAL of two lists as the operand of va_copy, va_arg and va_end.
 * Every operand comes from argc, so no shape folds. */
#include <stdarg.h>

#if defined(__x86_64__) && !defined(_WIN32)

typedef __typeof__(&((va_list *)0)[0][0]) va_tag_ptr;   /* the list's decayed type */

static int through_ptr(int n, ...) {
    va_list a;
    va_start(a, n);
    va_tag_ptr p = a;
    int x = va_arg(p, int);
    int y = va_arg(a, int);   /* the second argument: p walked a */
    va_end(a);
    return x * 10 + y;
}

static int copy_into_ptr(int n, ...) {
    va_list a, b;
    va_start(a, n);
    va_tag_ptr p = b;
    va_copy(p, a);
    int x = va_arg(b, int);
    int y = va_arg(a, int);
    va_end(b);
    va_end(a);
    return x == y ? 40 + x : 1;
}

static int copy_from_ptr(int n, ...) {
    va_list a, b;
    va_start(a, n);
    va_tag_ptr p = a;
    va_copy(b, p);
    int x = va_arg(b, int);
    int y = va_arg(a, int);
    va_end(b);
    va_end(a);
    return x == y ? 40 + x : 1;
}

static int walk(va_tag_ptr p, int n) {
    int s = 0;
    for (int i = 0; i < n; ++i) s += va_arg(p, int);
    return s;
}

static int through_param(int n, ...) {
    va_list a;
    va_start(a, n);
    int const s = walk(a, n);
    va_end(a);
    return s;
}

static int through_conditional(int k, ...) {
    va_list a, b, c;
    va_start(a, k);
    va_copy(k ? b : c, a);
    int x = va_arg(k ? b : c, int);
    va_end(k ? b : c);
    va_end(a);
    return 40 + x;
}

int main(int argc, char **argv) {
    (void)argv;
    int const k = argc;   /* 1 under every runner */
    int bad = 0;
    if (through_ptr(2, 4 * k, 2 * k) != 42) bad |= 1;
    if (copy_into_ptr(1, 2 * k) != 42) bad |= 2;
    if (copy_from_ptr(1, 2 * k) != 42) bad |= 4;
    if (through_param(3, 12 * k, 13 * k, 17 * k) != 42) bad |= 8;
    if (through_conditional(k, 2 * k) != 42) bad |= 16;
    return bad == 0 ? 42 : bad;
}

#else

int main(void) { return 42; }

#endif
