/* ═══ DSS PLATFORM RUNTIME — C23 <string.h> `strndup` for the `pe` object format ══════════
 *
 * The IMPLEMENTATION half of C23 7.26.2.7 `strndup` on `pe`. The DECLARATION half is the
 * `strndup` row of `src/dss-config/shippedLibs/string.json`, whose per-symbol `realization`
 * map names THIS file for `pe`; on `elf` the row imports the C library's own.
 *
 * ★ WHY A BODY. C23 requires the function and the pe C library has none: ✔MEASURED
 * 2026-10-07 (lane `cs`'s probes iso1 / iso2w) — ucrtbase.dll exports no `strndup`
 * (`GetProcAddress`), mingw-w64 13.2.0 link-fails on it and MSVC 19.51 refuses it, while
 * glibc 2.39 exports one (gcc 13.3.0 and clang 18.1.3 at -std=c2x build and run it to 42).
 *
 * ★★ THE SEMANTICS ARE 7.26.2.7's: at most `n` bytes of `s` are read — the copy stops at the
 * first null character before `n` — and the result is always null-terminated, in space
 * from `malloc`, so the caller's `free` (the same C library) releases it; a null pointer
 * when that space cannot be had.
 *
 * ★★ THE SIGNATURE IS CHECKED BY THE COMPILER: this unit includes <string.h>, so the
 * definition below must match the descriptor row that publishes it.
 */

#include <stdlib.h>
#include <string.h>

char *strndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len] != '\0') ++len;
    char *copy = malloc(len + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}
