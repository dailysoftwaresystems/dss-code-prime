/* P69 (lane `cs`) — D-CSUBSET-GNUC-PREDEFINE-SELECTS-UNIMPLEMENTED-BUILTIN: the GNU builtins
 * DSS's own `__GNUC__` claim routes real code into (`likely()` is `__builtin_expect`; sqlite's
 * generate_series takes `__builtin_ceil` / `__builtin_floor` under `__GNUC__`). Exit 42 when
 * every one behaves as gcc and clang define it, otherwise a bitmask of the groups that did not:
 *   1 `__builtin_expect` / `likely()` — a run-time value, an enumerator, an array bound, a
 *     static initializer
 *   2 library builtins with NO header: strlen, memset, memcpy, memcmp, strcmp
 *   4 the math library builtins sqlite selects: ceil and floor, and fabs
 *   8 the floating constants: inf, huge_val, a quiet NaN and its payload bits
 *   16 ffs / parity / popcountl / clzl / ctzl at run time, the `l` width per data model
 *   32 prefetch evaluates its operand's side effects; assume_aligned is its operand
 *   64 object_size: a named object, an offset into it, a member subobject, a string literal,
 *      and the documented unknown answer through a pointer parameter
 *   128 `__has_builtin` answers for every name above
 * Integer-constant-expression uses are `_Static_assert`s: a wrong fold refuses the build.
 * Under a compiler defining no `__GNUC__` (MSVC 19.51) the file is a bare `return 42`. */
#if defined(__GNUC__)
/* No header at all: every library function below is reached through its `__builtin_` name. */

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

_Static_assert(__builtin_expect(1, 1), "__builtin_expect is an integer constant expression");
_Static_assert(__builtin_ffs(8) == 4 && __builtin_parity(7) == 1
               && __builtin_popcountl(7) == 3 && __builtin_clz(1u) == 31,
               "the bit builtins fold");
enum { FIVE = __builtin_expect(5, 0) };
static int bounded[__builtin_expect(3, 1)];
static long folded = __builtin_expect(42, 1);
static double posinf = __builtin_inf();
static double quiet = __builtin_nan("");
static float quietf = __builtin_nanf("1");
static char g[16];
struct S { char a[4]; char b[8]; };
static struct S s;

static int evaluated;
static const char *touch(const char *p) { ++evaluated; return p; }

static __attribute__((noinline)) unsigned long size_through(char *p) {
    return __builtin_object_size(p, 0);
}

int main(int argc, char **argv) {
    (void)argv;
    int bad = 0;
    int one = argc;   /* 1 under every runner: no operand below is a constant to the optimizer */

    if (!likely(one == 1) || unlikely(one != 1) || __builtin_expect(one + 41, 1) != 42
        || FIVE != 5 || sizeof bounded != 3 * sizeof(int) || folded != 42
        || __builtin_expect_with_probability(one, 1, 0.9) != 1)
        bad |= 1;

    char buf[8];
    __builtin_memset(buf, 'x', 7);
    buf[7] = 0;
    char copy[8];
    __builtin_memcpy(copy, buf, 8);
    if (__builtin_strlen(buf) != 7 || __builtin_strcmp(copy, "xxxxxxx") != 0
        || __builtin_memcmp(copy, buf, 8) != 0 || __builtin_strlen(&"abc"[one]) != 2)
        bad |= 2;

    double x = one + 40.25;
    if (__builtin_ceil(x) != 42.0 || __builtin_floor(x) != 41.0 || __builtin_fabs(-x) != x)
        bad |= 4;

    unsigned long long bits = 0;
    unsigned int fbits = 0;
    double huge = __builtin_huge_val();
    __builtin_memcpy(&bits, &quiet, sizeof bits);
    __builtin_memcpy(&fbits, &quietf, sizeof fbits);
    if (!(posinf > 1e308) || huge != posinf || quiet == quiet
        || bits != 0x7ff8000000000000ull || fbits != 0x7fc00001u
        || !(__builtin_inff() > 3e38f) || !(__builtin_huge_valf() > 3e38f))
        bad |= 8;

    unsigned long all = ~0ul + (unsigned long)(one - 1);
    if (__builtin_ffs(one - 1) != 0 || __builtin_ffs(one << 20) != 21
        || __builtin_ffsll((long long)one << 40) != 41 || __builtin_ffsl(one) != 1
        || __builtin_parity((unsigned)one + 6u) != 1 || __builtin_parityll(3ull * one) != 0
        || __builtin_popcountl(all) != (int)(8 * sizeof(long))
        || __builtin_clzl((unsigned long)one) != (int)(8 * sizeof(long)) - 1
        || __builtin_ctzl((unsigned long)one << 3) != 3)
        bad |= 16;

    int arr[4] = {0};
    int *q = arr;
    __builtin_prefetch(touch((const char *)q));
    __builtin_prefetch(q++, 1);
    __builtin_prefetch(q, 0, 3);
    if (evaluated != 1 || q != arr + 1
        || __builtin_assume_aligned(arr, sizeof(int)) != (void *)arr)
        bad |= 32;

    if (__builtin_object_size(g, 0) != 16 || __builtin_object_size(g + 4, 0) != 12
        || __builtin_object_size(&s.b[2], 3) != 6 || __builtin_object_size(&s.a[1], 1) != 3
        || __builtin_object_size(&s.a[1], 0) != 11 || __builtin_object_size("abc", 0) != 4
        || size_through(g) != (unsigned long)-1 || __builtin_object_size(buf, 2) != 8)
        bad |= 64;

#if __has_builtin(__builtin_expect) && __has_builtin(__builtin_strlen) \
    && __has_builtin(__builtin_ceil) && __has_builtin(__builtin_trap) \
    && __has_builtin(__builtin_nan) && __has_builtin(__builtin_object_size) \
    && __has_builtin(__builtin_alloca) && __has_builtin(__builtin_ffs)
#else
    bad |= 128;
#endif

    if (one > 5) __builtin_trap();     /* never taken */
    if (one > 6) __builtin_abort();    /* never taken */
    return bad == 0 ? 42 : bad;
}
#else
int main(void) { return 42; }
#endif
