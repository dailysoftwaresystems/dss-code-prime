/* ═══ DSS PLATFORM RUNTIME — C23 <string.h> `memset_explicit`, on every object format ══════
 *
 * The IMPLEMENTATION half of C23 7.26.6.2 `memset_explicit`. The DECLARATION half is the
 * `memset_explicit` row of `src/dss-config/shippedLibs/string.json`, whose per-symbol
 * `realization` map names THIS file for `elf`, `pe` and `macho`.
 *
 * ★ WHY A BODY, ON EVERY FORMAT. C23 requires the function and no shipped C library has
 * it: ✔MEASURED 2026-10-07 (lane `cs`'s probes iso1 / iso2g / iso2w / mac1) — glibc 2.39 exports
 * no `memset_explicit` (gcc 13.3.0 and clang 18.1.3 at -std=c2x link-fail on it), ucrtbase.dll
 * exports none under any spelling (`GetProcAddress`), libSystem none (`dlsym`, Darwin 25.6.0),
 * and mingw-w64 13.2.0, MSVC 19.51 and Apple clang 21.0.0 refuse it. ISO C is part of what DSS
 * answers to, so the platform runtime fills the gap.
 *
 * ★★ THE STORES ARE ALWAYS PERFORMED. 7.26.6.2p2: the function exists so that sensitive
 * data can be made inaccessible, and the stores must not be elided "regardless of
 * optimizations". Each one is a VOLATILE access, which no optimizer may remove or merge
 * (C 5.1.2.3p6), and the body is a separate translation unit that no caller is inlined
 * across.
 *
 * ★★ THE SIGNATURE IS CHECKED BY THE COMPILER: this unit includes <string.h>, so the
 * definition below must match the descriptor row that publishes it.
 */

#include <string.h>

void *memset_explicit(void *s, int c, size_t n) {
    volatile unsigned char *p = s;
    unsigned char const value = (unsigned char)c;
    while (n != 0) {
        *p = value;
        ++p;
        --n;
    }
    return s;
}
