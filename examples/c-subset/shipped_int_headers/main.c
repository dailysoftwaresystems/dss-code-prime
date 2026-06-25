/* Cluster G (SQLite-readiness) — the c-subset resolves <stdint.h> / <stddef.h> /
 * <stdarg.h> via neutral shipped JSON descriptors (the FIRST real consumer of the
 * FC14 typedef-injection path). int32_t/uint8_t (global + local) and size_t are
 * descriptor typedefs; <stdarg.h> resolves via its descriptor shell (its va_list +
 * va_* builtins are exercised by the variadic corpora, not here — keeping this a
 * clean typedef/include witness that passes the release pipeline too). Mutable
 * globals keep the optimizer from folding the typedef'd values away, so the release
 * arm runs the real path. exit 42 == 40 + 2. RED-on-disable: delete any of the 3
 * shippedLibs/<hdr>.json -> the matching `#include` fires F_ShippedHeaderNotFound
 * (rendered F001A) and the compile fails. */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

int32_t g_a = 40;          /* stdint typedef in global type position */
uint8_t g_b = 2;           /* stdint typedef */

int main(void) {
    size_t  n = (size_t)g_b;          /* stddef typedef */
    int32_t r = g_a + (int32_t)n;     /* stdint typedef arithmetic */
    return (int)r;                    /* 40 + 2 = 42 */
}
