/* <inttypes.h>'s greatest-width conversions — the C23 entry points DSS supplies where the platform's C library predates
 * C23's binary subject (D-C-C23-CONVERSIONS-MISSING-ON-THE-UCRT-AND-LIBSYSTEM): strtoimax, strtoumax and their wide
 * twins wcstoimax, wcstoumax. The same front as stdlib_strto.c (C23 7.24.1.7p3's prefixed binary subject, converted
 * here with C 7.24.1.7's sign and range rules; every other input to the platform's own function whole, through the
 * private `__dss_platform_*` rows), written once per character width. */
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

static int narrow_is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int wide_is_space(wchar_t c) { return c == L' ' || (c >= L'\t' && c <= L'\r'); }

static const char *narrow_prefixed_binary(const char *s, int base, int *negative) {
    if (base != 0 && base != 2) return NULL;
    while (narrow_is_space(*s)) ++s;
    *negative = 0;
    if (*s == '+' || *s == '-') {
        *negative = (*s == '-');
        ++s;
    }
    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B') && (s[2] == '0' || s[2] == '1')) return s + 2;
    return NULL;
}

static const wchar_t *wide_prefixed_binary(const wchar_t *s, int base, int *negative) {
    if (base != 0 && base != 2) return NULL;
    while (wide_is_space(*s)) ++s;
    *negative = 0;
    if (*s == L'+' || *s == L'-') {
        *negative = (*s == L'-');
        ++s;
    }
    if (s[0] == L'0' && (s[1] == L'b' || s[1] == L'B') && (s[2] == L'0' || s[2] == L'1')) return s + 2;
    return NULL;
}

static uintmax_t narrow_magnitude(const char *d, const char **end, int *overflow) {
    uintmax_t m = 0;
    *overflow = 0;
    for (; *d == '0' || *d == '1'; ++d) {
        if (m > (UINTMAX_MAX >> 1)) *overflow = 1;
        else m = (m << 1) | (uintmax_t)(*d - '0');
    }
    *end = d;
    return m;
}

static uintmax_t wide_magnitude(const wchar_t *d, const wchar_t **end, int *overflow) {
    uintmax_t m = 0;
    *overflow = 0;
    for (; *d == L'0' || *d == L'1'; ++d) {
        if (m > (UINTMAX_MAX >> 1)) *overflow = 1;
        else m = (m << 1) | (uintmax_t)(*d - L'0');
    }
    *end = d;
    return m;
}

static intmax_t signed_result(uintmax_t m, int overflow, int negative) {
    uintmax_t const limit = negative ? (uintmax_t)INTMAX_MAX + 1u : (uintmax_t)INTMAX_MAX;
    if (overflow || m > limit) {
        errno = ERANGE;
        return negative ? INTMAX_MIN : INTMAX_MAX;
    }
    if (negative) return m == (uintmax_t)INTMAX_MAX + 1u ? INTMAX_MIN : -(intmax_t)m;
    return (intmax_t)m;
}

static uintmax_t unsigned_result(uintmax_t m, int overflow, int negative) {
    if (overflow) {
        errno = ERANGE;
        return UINTMAX_MAX;
    }
    return negative ? (uintmax_t)0 - m : m;
}

intmax_t __dss_isoc23_strtoimax(const char *restrict nptr, char **restrict endptr, int base) {
    int negative = 0;
    const char *d = narrow_prefixed_binary(nptr, base, &negative);
    if (d == NULL) return __dss_platform_strtoimax(nptr, endptr, base);
    const char *end = d;
    int overflow = 0;
    uintmax_t const m = narrow_magnitude(d, &end, &overflow);
    if (endptr != NULL) *endptr = (char *)end;
    return signed_result(m, overflow, negative);
}

uintmax_t __dss_isoc23_strtoumax(const char *restrict nptr, char **restrict endptr, int base) {
    int negative = 0;
    const char *d = narrow_prefixed_binary(nptr, base, &negative);
    if (d == NULL) return __dss_platform_strtoumax(nptr, endptr, base);
    const char *end = d;
    int overflow = 0;
    uintmax_t const m = narrow_magnitude(d, &end, &overflow);
    if (endptr != NULL) *endptr = (char *)end;
    return unsigned_result(m, overflow, negative);
}

intmax_t __dss_isoc23_wcstoimax(const wchar_t *restrict nptr, wchar_t **restrict endptr, int base) {
    int negative = 0;
    const wchar_t *d = wide_prefixed_binary(nptr, base, &negative);
    if (d == NULL) return __dss_platform_wcstoimax(nptr, endptr, base);
    const wchar_t *end = d;
    int overflow = 0;
    uintmax_t const m = wide_magnitude(d, &end, &overflow);
    if (endptr != NULL) *endptr = (wchar_t *)end;
    return signed_result(m, overflow, negative);
}

uintmax_t __dss_isoc23_wcstoumax(const wchar_t *restrict nptr, wchar_t **restrict endptr, int base) {
    int negative = 0;
    const wchar_t *d = wide_prefixed_binary(nptr, base, &negative);
    if (d == NULL) return __dss_platform_wcstoumax(nptr, endptr, base);
    const wchar_t *end = d;
    int overflow = 0;
    uintmax_t const m = wide_magnitude(d, &end, &overflow);
    if (endptr != NULL) *endptr = (wchar_t *)end;
    return unsigned_result(m, overflow, negative);
}
