/* libSystem's answers, for DSS's <stdio.h> entry points (stdio.c) on Mach-O. The primitives themselves are libSystem's
 * own exports, imported by the private `__dss_platform_*` rows of stdio.json under their plain names; this unit holds
 * only what the format-neutral engine asks about the platform (stdio.c's argument-class letters: i int, l long,
 * L long long, j intmax_t, z size_t, t ptrdiff_t, d double, D long double, p pointer):
 *   * `%n` — libSystem performs it (✔MEASURED, Apple clang 21 on both arches).
 *   * positional arguments — libSystem has them: ✔MEASURED `printf("[%2$d %1$d]", 1, 2)` prints `[2 1]`.
 *   * a base prefix with no digit after it — the item is the "0", the letter left in the input: ✔MEASURED
 *     `sscanf("0xz", "%i")` stores 0 having consumed one character, and a stream reads 'x' next.
 *   * vendor length modifiers (📄 Apple printf(3)): `q` a quad (long long); vendor conversions `D`, `O`, `U` (long,
 *     deprecated spellings of ld, lo, lu), `C` (lc) and `S` (ls).
 *
 * Mach-O's only. <stdio.h> declares these answers (stdio.json's private rows), so each definition is checked against
 * the one declaration every format's unit shares. */
#include <stdio.h>

int __dss_platform_printf_performs_n(void) {
    return 1;
}

int __dss_platform_printf_positional(void) {
    return 1;
}

int __dss_platform_prefix_without_digits_is_zero(void) {
    return 1;
}

/* `scan`: 0 as printf reads the length, 1 as scanf does — libSystem's `q` is a quad in both (✔MEASURED `%qn`
 * stores 8 bytes in printf and scanf, P69 round 4). */
int __dss_platform_length_extension(const char *text, int scan, char *argClass) {
    (void)scan;
    if (text[0] == 'q') {
        *argClass = 'L';
        return 1;
    }
    return 0;
}

int __dss_platform_conversion_extension(int conversion, char *argClass) {
    if (conversion == 'D' || conversion == 'O' || conversion == 'U') {
        *argClass = 'l';
        return 1;
    }
    if (conversion == 'C') {
        *argClass = 'i';
        return 1;
    }
    if (conversion == 'S') {
        *argClass = 'p';
        return 1;
    }
    return 0;
}
