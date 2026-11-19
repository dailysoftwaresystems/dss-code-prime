/* A DECIMAL integer constant past every signed type its suffix admits —
 * `9223372036854775808`, `9223372036854775808L`, `9223372036854775808LL` — has no
 * type in C's own lists (C 6.4.4.1p6), and DSS refused it (S_IntegerLiteralTooLarge)
 * where every reference compiles it. DSS now reads it as `unsigned long long` and
 * warns (S_IntegerLiteralImplicitlyUnsigned), as clang does; see expected.json for
 * what each reference gives it. The exit code is a bitmask of the checks that broke:
 *   1  the static initializers hold 2^63 (all references agree)
 *   2  the unsuffixed decimal 2^64-1 is that value (all references agree)
 *   4  the constant in an enumerator and an array bound
 *   8  the unsuffixed and `L` spellings are `unsigned long long` by _Generic
 *  16  the `LL` spelling is `unsigned long long` by _Generic
 *  32  `sizeof` of the unsuffixed spelling is 8
 *  64  the constant flows through code with run-time operands
 */
static unsigned long long a = 9223372036854775808LL;
static unsigned long long b = 9223372036854775808;
static unsigned long long c = 18446744073709551615;

enum { Positive = (9223372036854775808 > 0) ? 1 : 0 };
static char bound[(9223372036854775808 / 2 == 4611686018427387904ULL) ? 8 : 1];

static unsigned long long id(unsigned long long x) { return x; }

int main(int argc, char **argv) {
    (void)argv;
    unsigned long long const one = (unsigned long long)argc;   /* 1 at run time */
    int r = 0;
    if (a != 9223372036854775808ULL || b != 9223372036854775808ULL) r |= 1;
    if (c != 18446744073709551615ULL) r |= 2;
    if (Positive != 1 || sizeof bound != 8) r |= 4;
    if (_Generic(9223372036854775808, unsigned long long: 0, default: 1)
        + _Generic(9223372036854775808L, unsigned long long: 0, default: 1) != 0) r |= 8;
    if (_Generic(9223372036854775808LL, unsigned long long: 0, default: 1) != 0) r |= 16;
    if (sizeof(9223372036854775808) != 8) r |= 32;
    if (id(9223372036854775808LL + one - 1) != 9223372036854775808ULL
        || id(9223372036854775808 * one) / 2 != 4611686018427387904ULL
        || id(18446744073709551615 - one) != 18446744073709551614ULL) r |= 64;
    return r == 0 ? 42 : r;
}
