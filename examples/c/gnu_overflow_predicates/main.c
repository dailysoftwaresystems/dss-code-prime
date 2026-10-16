/* gcc's checked-arithmetic PREDICATES: __builtin_{add,sub,mul}_overflow_p(a, b, c) compute a OP b
 * at infinite precision, cast it to the type of `c` -- its own type, unpromoted, or a bit-field's
 * width and signedness -- and answer whether the cast changed it. Nothing is stored; `c`'s value
 * is never read, its side effects are evaluated (once). An integer constant expression when `a`
 * and `b` are, whatever `c` is.
 *
 * Only gcc has them (clang: "unknown builtin"), so the program asks `__has_builtin` and a compiler
 * without them returns 42 untested. The exit code is 42, or a bitmask of the groups that failed:
 *   1  run time, plain integer targets       8  the third operand's side effect, once
 *   2  run time, a bit-field target         16  the 128-bit operands
 *   4  the constant answers (enum, static initializer, array bound)
 */
#if defined(__has_builtin)
#  if __has_builtin(__builtin_add_overflow_p)
#    define HAVE_OVERFLOW_P 1
#  endif
#endif

#ifdef HAVE_OVERFLOW_P
enum {
    A = 2147483647, B = 3,
    C = __builtin_add_overflow_p(A, B, (__typeof__(A + B))0) ? 0 : A + B,   /* gcc's manual */
    D = __builtin_add_overflow_p(1, 127, (signed char)0),
    E = __builtin_sub_overflow_p(0, 1, (unsigned)0),
    F = __builtin_mul_overflow_p(65536, 65536, (long long)0)
};
static int st = __builtin_mul_overflow_p(65536, 65536, (int)0);
static char arr[__builtin_add_overflow_p(1, 1, (int)0) + 1];
struct S { int x : 3; unsigned y : 3; };
#endif

int main(int argc, char **argv) {
    (void)argv;
    int bad = 0;
#ifdef HAVE_OVERFLOW_P
    int const one = argc;                    /* 1, opaque to the compiler */
    int const big = 2147483646 + one;        /* INT_MAX */
    long long const huge = 9223372036854775806LL + one;   /* LLONG_MAX */

    if (!__builtin_add_overflow_p(big, one, (int)0)) bad |= 1;
    if (__builtin_add_overflow_p(big, one, (long long)0)) bad |= 1;
    if (!__builtin_sub_overflow_p(0, one, (unsigned)0)) bad |= 1;
    if (__builtin_sub_overflow_p(0, one, (int)0)) bad |= 1;
    if (!__builtin_mul_overflow_p(big, 2 * one, (int)0)) bad |= 1;
    if (!__builtin_add_overflow_p(100 * one, 28, (signed char)0)) bad |= 1;
    if (__builtin_add_overflow_p(100 * one, 27, (signed char)0)) bad |= 1;
    if (__builtin_add_overflow_p(100 * one, 155, (unsigned char)0)) bad |= 1;
    if (!__builtin_mul_overflow_p(huge, 2 * one, (long long)0)) bad |= 1;
    if (__builtin_mul_overflow_p(huge, 2 * one, (unsigned long long)0)) bad |= 1;

    struct S s = {0, 0};
    if (!__builtin_add_overflow_p(3 * one, one, s.x)) bad |= 2;
    if (__builtin_add_overflow_p(3 * one, 0, s.x)) bad |= 2;
    if (!__builtin_sub_overflow_p(-4 * one, one, s.x)) bad |= 2;
    if (!__builtin_add_overflow_p(7 * one, one, s.y)) bad |= 2;
    if (__builtin_add_overflow_p(7 * one, 0, s.y)) bad |= 2;

    if (C != 0 || D != 1 || E != 1 || F != 0 || st != 1 || sizeof arr != 1) bad |= 4;

    int calls = 0;
    if (__builtin_add_overflow_p(one, 2, (calls++, 0))) bad |= 8;
    if (calls != 1) bad |= 8;

#  if defined(__SIZEOF_INT128__)
    __extension__ typedef __int128 i128;      /* `__extension__`: ISO C has no __int128 */
    i128 const h = (i128)one << 100;
    if (!__builtin_mul_overflow_p(h, (long long)one << 30, (i128)0)) bad |= 16;
    if (__builtin_mul_overflow_p(h, 2 * one, (i128)0)) bad |= 16;
    if (!__builtin_add_overflow_p(h, h, (long long)0)) bad |= 16;
#  endif
#endif
    (void)argc;
    return bad ? bad : 42;
}
