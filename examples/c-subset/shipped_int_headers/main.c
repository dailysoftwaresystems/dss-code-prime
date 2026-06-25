/* Cluster G (SQLite-readiness) — the c-subset resolves <stdint.h> / <stddef.h> /
 * <stdarg.h> via neutral shipped JSON descriptors (the FIRST real consumer of the
 * FC14 typedef-injection path). int32_t/uint8_t (global + local) and size_t are
 * descriptor typedefs; NULL is a stddef descriptor CONSTANT used in pointer
 * context (the null-pointer-constant path SQLite leans on heavily); <stdarg.h>
 * resolves via its descriptor shell (its va_list + va_* builtins are exercised by
 * the variadic corpora, not here — keeping this a clean typedef/include witness
 * that passes the release pipeline too). Mutable globals keep the optimizer from
 * folding the typedef'd values away, so the release arm runs the real path. exit
 * 42 == 40 + 2. RED-on-disable: delete any of the 3 shippedLibs/<hdr>.json -> the
 * matching `#include` fires F_ShippedHeaderNotFound (rendered F001A) and the
 * compile fails. */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

int32_t g_a = 40;          /* stdint typedef in global type position */
uint8_t g_b = 2;           /* stdint typedef */

int main(void) {
    size_t  n = (size_t)g_b;           /* stddef typedef */
    int32_t r = g_a + (int32_t)n;      /* stdint typedef arithmetic: 40 + 2 = 42 */
    int*    p = &g_a;                  /* builtin-int pointer declarator (i32* == int32_t*
                                          on every current target). NOTE the BUILTIN `int*`
                                          declarator is deliberate: a DESCRIPTOR-typedef in
                                          pointer-declarator position (`int32_t* p`) is a
                                          separate parser gap — descriptor typedefs aren't
                                          parser-visible type-names, so `int32_t * p` parses
                                          as multiplication (deferred
                                          D-CSUBSET-DESCRIPTOR-TYPEDEF-POINTER-DECL). */
    if (p == NULL) { r = 0; }          /* stddef NULL constant in pointer context; p is
                                          non-null so r stays 42 (witnesses NULL resolves +
                                          is pointer-compatible — not silently broken) */
    return (int)r;                     /* 42 */
}
