/* <stdlib.h>'s everyday C23 7.24 surface, on every pair (P68 round 12, D-C-STDLIB-H-LACKS-THIRTY-FIVE-ISO-NAMES,
 * fold S1): EXIT_SUCCESS, EXIT_FAILURE, RAND_MAX, MB_CUR_MAX, div_t / ldiv_t / lldiv_t and div / ldiv / lldiv,
 * atoll, llabs, strtoull, strtof, mblen, and quick_exit / at_quick_exit / _Exit.
 *
 * Every value is checked by VALUE and every type by _Generic, so a name bound to the wrong entry point, a struct
 * laid out in the wrong order, or a macro of the wrong type fails with its own exit code:
 *   - MB_CUR_MAX is size_t on EVERY pair — a MEANING fork (the UCRT and Apple spell it int, glibc size_t)
 *     decided by C23 7.24p3; only _Generic and sizeof can tell, so both are asserted here.
 *   - div / ldiv / lldiv return structs by value through three different ABIs (Win64 returns lldiv_t through a
 *     hidden pointer, SysV in RAX:RDX, AAPCS64 in X0/X1); the operands make quot and rem differ in sign and
 *     magnitude, so a swapped or mis-returned pair cannot pass.
 *   - the program ENDS through quick_exit(1): the handler at_quick_exit registered turns it into _Exit(42), so exit
 *     42 proves the registration, the run of the handler, and _Exit together (1 = never registered). */
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>

#define IS(T, e) _Generic((e), T: 1, default: 0)

static void on_quick_exit(void) { _Exit(42); }

int main(void) {
    /* EXIT_SUCCESS / EXIT_FAILURE / RAND_MAX: macros the preprocessor sees, of type int. */
#if !defined(EXIT_SUCCESS) || !defined(EXIT_FAILURE) || !defined(RAND_MAX) || !defined(MB_CUR_MAX)
#error "a <stdlib.h> macro is missing"
#endif
#if EXIT_SUCCESS != 0 || EXIT_FAILURE != 1 || RAND_MAX < 32767
#error "a <stdlib.h> macro has the wrong value"
#endif
#if defined(_WIN32)
    if (RAND_MAX != 32767) return 10;          /* the UCRT's own rand() range */
#else
    if (RAND_MAX != 2147483647) return 10;     /* glibc's and Apple's */
#endif
    if (!IS(int, EXIT_SUCCESS) || !IS(int, EXIT_FAILURE) || !IS(int, RAND_MAX)) return 11;
    for (int i = 0; i < 64; ++i) {
        int const r = rand();
        if (r < 0 || r > RAND_MAX) return 12;
    }

    /* MB_CUR_MAX: size_t on every pair, 1 in the C locale. */
    if (!IS(size_t, MB_CUR_MAX) || sizeof(MB_CUR_MAX) != sizeof(size_t)) return 13;
    if (MB_CUR_MAX != 1) return 14;

    /* div_t / ldiv_t / lldiv_t: {quot, rem} in that order, each member the operand type. */
    if (offsetof(div_t, quot) != 0 || offsetof(div_t, rem) != sizeof(int)) return 15;
    if (offsetof(ldiv_t, quot) != 0 || offsetof(ldiv_t, rem) != sizeof(long)) return 16;
    if (offsetof(lldiv_t, quot) != 0 || offsetof(lldiv_t, rem) != sizeof(long long)) return 17;
    if (sizeof(div_t) != 2 * sizeof(int) || sizeof(ldiv_t) != 2 * sizeof(long)
        || sizeof(lldiv_t) != 2 * sizeof(long long)) return 18;
    div_t const d = div(-7, 2);
    if (d.quot != -3 || d.rem != -1) return 19;
    div_t const d2 = div(7, -2);
    if (d2.quot != -3 || d2.rem != 1) return 20;
    ldiv_t const ld = ldiv(-2000000007L, 1000L);
    if (ld.quot != -2000000L || ld.rem != -7L) return 21;
    lldiv_t const lld = lldiv(-7000000001LL, 1000000000LL);
    if (lld.quot != -7LL || lld.rem != -1LL) return 22;
    if (!IS(div_t, div(1, 1)) || !IS(ldiv_t, ldiv(1L, 1L)) || !IS(lldiv_t, lldiv(1LL, 1LL))) return 23;

    /* atoll / llabs / strtoull / strtof: values past 32 bits, and each result's type. */
    if (atoll("-9000000000") != -9000000000LL || !IS(long long, atoll("0"))) return 24;
    if (llabs(-9000000000LL) != 9000000000LL || !IS(long long, llabs(0LL))) return 25;
    char *end = NULL;
    unsigned long long const u = strtoull("18446744073709551615x", &end, 10);
    if (u != ULLONG_MAX || end == NULL || *end != 'x' || !IS(unsigned long long, strtoull("0", NULL, 10))) return 26;
    float const f = strtof("1.5e1", &end);
    if (f != 15.0f || *end != '\0' || !IS(float, strtof("0", NULL))) return 27;

    /* mblen, in the C locale. */
    if (mblen("a", 1) != 1 || mblen("", 1) != 0) return 28;

    /* quick_exit / at_quick_exit / _Exit: the handler turns quick_exit(1) into exit 42. */
    if (at_quick_exit(on_quick_exit) != 0) return 33;
    quick_exit(1);
}
