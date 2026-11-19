/* The UCRT's formatted-I/O primitives and answers, for DSS's <stdio.h> entry points (stdio.c).
 *
 * ucrtbase.dll exports NO printf or scanf family (✔MEASURED, its export table): in the SDK every one of them is a
 * HEADER INLINE over one of four `__stdio_common_v*` cores. The five v-primitives below are those inlines' bodies,
 * with the option bits the SDK passes (📄 SDK 10.0.26100.0 <corecrt_stdio_config.h> and <stdio.h>):
 * `_CRT_INTERNAL_LOCAL_PRINTF_OPTIONS` and `_CRT_INTERNAL_LOCAL_SCANF_OPTIONS` are 0 without the legacy opt-ins DSS
 * never links, sprintf/vsprintf add LEGACY_VSPRINTF_NULL_TERMINATION (bit 0) with the unbounded count,
 * snprintf/vsnprintf add STANDARD_SNPRINTF_BEHAVIOR (bit 1) with the caller's count, and the two buffer printers
 * clamp a negative result to -1 as the SDK does. The locale argument is null: the UCRT then uses the calling
 * thread's current locale, as its own printf does.
 *
 * The answers the format-neutral engine asks (stdio.c's argument-class letters: i int, l long, L long long,
 * j intmax_t, z size_t, t ptrdiff_t, d double, D long double, p pointer):
 *   * `%n` — the UCRT does NOT perform it by default: 📄 MSVC `_set_printf_count_output` (a process-global switch
 *     DSS does not flip), ✔MEASURED `printf("ab%n", &k)` kills the process with 0xC0000409. The engine performs it.
 *   * positional arguments — none in printf: 📄 MSVC, positional parameters exist only in the `_printf_p` family;
 *     ✔MEASURED `printf("[%2$d %1$d]", 1, 2)` prints `[$d $d]`.
 *   * a base prefix with no digit after it — the directive FAILS: ✔MEASURED `sscanf("0xz", "%i")` returns 0 with
 *     "0x" consumed, and "0x" at the end of input is an input failure.
 *   * vendor size prefixes (📄 MSVC "Format specification syntax"): `I64` __int64, `I32` __int32, `I` the pointer
 *     width, `w` a wide character or string (with `c`/`s`, whose own classes stand); vendor types `C` (the opposite
 *     width's character), `S` (the opposite width's string), `Z` (an ANSI_STRING / UNICODE_STRING pointer).
 *     `I` alone is a size prefix ONLY before an integer conversion, and printf and scanf part ways after any other
 *     (📄 SDK 10.0.26100.0 ucrt/inc/corecrt_internal_stdio_output.h and _input.h, the "state_case_size" and
 *     length parsers): printf consumes the `I` as no size at all — ✔MEASURED `%In` stores a 4-byte int (P69 round
 *     4) — while scanf reads it as the CONVERSION `I`, its synonym of `i`. The integer conversions are the UCRT's
 *     d i o u x X and C23's b B, which DSS's engine adds and the prefix sizes like the others.
 *
 * pe's only: the other formats' C libraries export the primitives by their plain names. */
#include <stdarg.h>
#include <stdio.h>

int __dss_platform_vfprintf(FILE *stream, const char *format, va_list ap) {
    return __stdio_common_vfprintf(0, stream, format, NULL, ap);
}

int __dss_platform_vsprintf(char *buffer, const char *format, va_list ap) {
    int const written = __stdio_common_vsprintf(1, buffer, (size_t)-1, format, NULL, ap);
    return written < 0 ? -1 : written;
}

int __dss_platform_vsnprintf(char *buffer, size_t count, const char *format, va_list ap) {
    int const written = __stdio_common_vsprintf(2, buffer, count, format, NULL, ap);
    return written < 0 ? -1 : written;
}

int __dss_platform_vfscanf(FILE *stream, const char *format, va_list ap) {
    return __stdio_common_vfscanf(0, stream, format, NULL, ap);
}

int __dss_platform_vsscanf(const char *input, const char *format, va_list ap) {
    return __stdio_common_vsscanf(0, input, (size_t)-1, format, NULL, ap);
}

int __dss_platform_printf_performs_n(void) {
    return 0;
}

int __dss_platform_printf_positional(void) {
    return 0;
}

int __dss_platform_prefix_without_digits_is_zero(void) {
    return 0;
}

static int is_integer_conversion(char c) {
    return c == 'd' || c == 'i' || c == 'o' || c == 'u' || c == 'x' || c == 'X' || c == 'b' || c == 'B';
}

/* `scan`: 0 as printf reads the length, 1 as scanf does. Returns the characters the length takes (0: none here). */
int __dss_platform_length_extension(const char *text, int scan, char *argClass) {
    if (text[0] == 'I' && text[1] == '6' && text[2] == '4') {
        *argClass = 'L';
        return 3;
    }
    if (text[0] == 'I' && text[1] == '3' && text[2] == '2') {
        *argClass = 'i';
        return 3;
    }
    if (text[0] == 'I') {
        if (is_integer_conversion(text[1])) {
            *argClass = 'z';
            return 1;
        }
        if (scan) return 0;   /* scanf: the conversion `I` */
        *argClass = 0;        /* printf: consumed, sizing nothing */
        return 1;
    }
    if (text[0] == 'w') {
        *argClass = 'i';
        return 1;
    }
    return 0;
}

int __dss_platform_conversion_extension(int conversion, char *argClass) {
    if (conversion == 'C') {
        *argClass = 'i';
        return 1;
    }
    if (conversion == 'S' || conversion == 'Z') {
        *argClass = 'p';
        return 1;
    }
    return 0;
}
