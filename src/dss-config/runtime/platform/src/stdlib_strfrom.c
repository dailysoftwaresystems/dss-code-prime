/* <stdlib.h>'s C23 floating-to-string conversions — strfromd, strfromf, strfroml — where the platform's C library
 * exports none of them (✔MEASURED P69: the UCRT, through mingw-w64 13.2.0 and MSVC 19.51, and Apple's libSystem, on
 * both arches; glibc exports all three @GLIBC_2.25, and ELF binds those).
 *
 * C23 7.24.1.3: each is EQUIVALENT to `snprintf(s, n, format, fp)`, where the format is `%[.precision]` and one of
 * a A e E f F g G — no flags, no width, no length modifier — and fp is the value converted. So each body IS that call;
 * strfromf's float promotes to double exactly as a variadic argument does, and strfroml writes the `L` length modifier
 * C requires for a long double in front of the conversion, the one change the C text calls for. A format outside that
 * grammar is undefined behaviour in C and is handed through as it is.
 *
 * pe's and Mach-O's: one unit for both, since nothing here is format-specific. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int strfromd(char *restrict s, size_t n, const char *restrict format, double fp) {
    return snprintf(s, n, format, fp);
}

int strfromf(char *restrict s, size_t n, const char *restrict format, float fp) {
    return snprintf(s, n, format, (double)fp);
}

int strfroml(char *restrict s, size_t n, const char *restrict format, long double fp) {
    /* `%[.precision]c` becomes `%[.precision]Lc`. The grammar has at most a '.', digits and one conversion letter, so
     * a short buffer holds every precision a program can spell meaningfully; a longer format is outside C's grammar. */
    char spec[64];
    size_t const len = strlen(format);
    if (len < 2 || len + 2 > sizeof spec) return snprintf(s, n, format, fp);
    memcpy(spec, format, len - 1);
    spec[len - 1] = 'L';
    spec[len] = format[len - 1];
    spec[len + 1] = '\0';
    return snprintf(s, n, spec, fp);
}
