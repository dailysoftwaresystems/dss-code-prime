/* <stdlib.h>'s integer conversions — the C23 entry points DSS supplies where the platform's C library predates C23's
 * binary subject (D-C-C23-CONVERSIONS-MISSING-ON-THE-UCRT-AND-LIBSYSTEM). ✔MEASURED: the UCRT and libSystem answer
 * strtol("0b101", NULL, 0) with 0 (the subject "0", ending at 'b'); C23 7.24.1.7p3 makes "0b"/"0B" followed by a binary
 * digit the prefix of a base-2 subject when the base is 0 or 2, and glibc's C23 entry point answers 5.
 *
 * THE FRONT. Only a subject in that prefixed binary form is converted here — its digits, then C 7.24.1.7's sign and
 * range rules (ERANGE and the type's MAX/MIN; an unsigned negation in the return type). Every other input goes to the
 * platform's own function WHOLE, through the private `__dss_platform_*` rows (plain imports), so it keeps the
 * platform's exact answer — errno, endptr, locale. "0b" with no binary digit after it is not the prefixed form: the
 * platform reads the subject "0" and stops at the 'b', exactly as C23 says (the expected form needs a digit). */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

/* A C23 prefixed binary subject at s: its first binary digit (after the prefix), or NULL when s is not one. */
static const char *prefixed_binary(const char *s, int base, int *negative) {
    if (base != 0 && base != 2) return NULL;
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) ++s;
    *negative = 0;
    if (*s == '+' || *s == '-') {
        *negative = (*s == '-');
        ++s;
    }
    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B') && (s[2] == '0' || s[2] == '1')) return s + 2;
    return NULL;
}

/* The digits' magnitude; *overflow when it exceeds uintmax_t; *end one past the last digit. */
static uintmax_t binary_magnitude(const char *d, const char **end, int *overflow) {
    uintmax_t m = 0;
    *overflow = 0;
    for (; *d == '0' || *d == '1'; ++d) {
        if (m > (UINTMAX_MAX >> 1)) *overflow = 1;
        else m = (m << 1) | (uintmax_t)(*d - '0');
    }
    *end = d;
    return m;
}

/* C 7.24.1.7p8 for a signed type whose maximum is `max`: the value, or the saturated bound and ERANGE. */
static intmax_t signed_result(uintmax_t m, int overflow, int negative, uintmax_t max, intmax_t min, int *range) {
    uintmax_t const limit = negative ? max + 1u : max;
    *range = overflow || m > limit;
    if (*range) return negative ? min : (intmax_t)max;
    if (negative) return m == max + 1u ? min : -(intmax_t)m;
    return (intmax_t)m;
}

long __dss_isoc23_strtol(const char *restrict nptr, char **restrict endptr, int base) {
    int negative = 0;
    const char *d = prefixed_binary(nptr, base, &negative);
    if (d == NULL) return __dss_platform_strtol(nptr, endptr, base);
    const char *end = d;
    int overflow = 0;
    uintmax_t const m = binary_magnitude(d, &end, &overflow);
    if (endptr != NULL) *endptr = (char *)end;
    int range = 0;
    long const v = (long)signed_result(m, overflow, negative, (uintmax_t)LONG_MAX, LONG_MIN, &range);
    if (range) errno = ERANGE;
    return v;
}

long long __dss_isoc23_strtoll(const char *restrict nptr, char **restrict endptr, int base) {
    int negative = 0;
    const char *d = prefixed_binary(nptr, base, &negative);
    if (d == NULL) return __dss_platform_strtoll(nptr, endptr, base);
    const char *end = d;
    int overflow = 0;
    uintmax_t const m = binary_magnitude(d, &end, &overflow);
    if (endptr != NULL) *endptr = (char *)end;
    int range = 0;
    long long const v = (long long)signed_result(m, overflow, negative, (uintmax_t)LLONG_MAX, LLONG_MIN, &range);
    if (range) errno = ERANGE;
    return v;
}

unsigned long __dss_isoc23_strtoul(const char *restrict nptr, char **restrict endptr, int base) {
    int negative = 0;
    const char *d = prefixed_binary(nptr, base, &negative);
    if (d == NULL) return __dss_platform_strtoul(nptr, endptr, base);
    const char *end = d;
    int overflow = 0;
    uintmax_t const m = binary_magnitude(d, &end, &overflow);
    if (endptr != NULL) *endptr = (char *)end;
    if (overflow || m > (uintmax_t)ULONG_MAX) {
        errno = ERANGE;
        return ULONG_MAX;
    }
    unsigned long const v = (unsigned long)m;
    return negative ? (unsigned long)0 - v : v;
}

unsigned long long __dss_isoc23_strtoull(const char *restrict nptr, char **restrict endptr, int base) {
    int negative = 0;
    const char *d = prefixed_binary(nptr, base, &negative);
    if (d == NULL) return __dss_platform_strtoull(nptr, endptr, base);
    const char *end = d;
    int overflow = 0;
    uintmax_t const m = binary_magnitude(d, &end, &overflow);
    if (endptr != NULL) *endptr = (char *)end;
    if (overflow || m > (uintmax_t)ULLONG_MAX) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    unsigned long long const v = (unsigned long long)m;
    return negative ? (unsigned long long)0 - v : v;
}
