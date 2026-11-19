/* <stdlib.h>'s C23 surface beyond the platform's own, run on every pair, from <stdlib.h> (and <stdio.h> for the
 * report) alone:
 *   - names no platform's C library exports, whose bodies DSS ships (runtime/platform/src/stdlib.c): memalignment —
 *     the maximum alignment an address satisfies, zero for a null pointer — free_sized and free_aligned_sized;
 *   - strfromd/strfromf/strfroml (C23 7.24.1.3: each the snprintf of its value; glibc exports them, the UCRT and
 *     libSystem do not, so pe and Mach-O run runtime/platform/src/stdlib_strfrom.c);
 *   - aligned_alloc (the UCRT exports none: pe runs runtime/platform/src/stdlib_aligned_alloc.c);
 *   - the multibyte conversions and strtold, imports on every pair, read in the "C" locale;
 *   - C23 7.24p2's once trio — once_flag, ONCE_FLAG_INIT and call_once — with no <threads.h> in sight. */
#include <stdio.h>
#include <stdlib.h>

static _Alignas(16) char block[64];

static once_flag g_once  = ONCE_FLAG_INIT;
static int       g_inits = 0;

static void init_once(void) {
    ++g_inits;
}

int main(void) {
    size_t const none  = memalignment(0);
    size_t const base  = memalignment(block);
    size_t const plus1 = memalignment(block + 1);
    size_t const plus8 = memalignment(block + 8);
    printf("memalignment: null=%u base_at_least_16=%d plus1=%u plus8=%u\n", (unsigned)none, base >= 16,
           (unsigned)plus1, (unsigned)plus8);

    char d[16];
    char f[16];
    char l[16];
    int const nd = strfromd(d, sizeof d, "%.3f", 2.5);
    int const nf = strfromf(f, sizeof f, "%g", 0.5f);
    int const nl = strfroml(l, sizeof l, "%.1f", 1.5L);
    printf("strfrom: %s/%d %s/%d %s/%d\n", d, nd, f, nf, l, nl);

    void *const a    = aligned_alloc(16, 64);
    int const   a_ok = a != 0 && memalignment(a) >= 16;
    free_aligned_sized(a, 16, 64);
    void *const m = malloc(8);
    free_sized(m, 8);
    printf("aligned_alloc: 16-aligned=%d\n", a_ok);

    wchar_t      wc    = 0;
    int const    mb    = mbtowc(&wc, "A", 1);
    char         one[8] = {0};
    int const    wb    = wctomb(one, L'B');
    wchar_t      ws[4];
    size_t const n1    = mbstowcs(ws, "hi", 4);
    char         cs[4] = {0};
    size_t const n2    = wcstombs(cs, L"ok", 4);
    printf("multibyte: mbtowc=%d/%d wctomb=%d/%c mbstowcs=%u/%d wcstombs=%u/%s\n", mb, wc == L'A', wb, one[0],
           (unsigned)n1, ws[0] == L'h' && ws[1] == L'i', (unsigned)n2, cs);

    long double const ld = strtold("2.5", 0);
    printf("strtold: %d\n", ld == 2.5L);

    call_once(&g_once, init_once);
    call_once(&g_once, init_once);
    printf("call_once: initializer ran %d time(s)\n", g_inits);

    int const ok = none == 0 && base >= 16 && plus1 == 1 && plus8 == 8 && nd == 5 && nf == 3 && nl == 3 && a_ok &&
                   mb == 1 && wc == L'A' && wb == 1 && n1 == 2 && n2 == 2 && ld == 2.5L && g_inits == 1;
    return ok ? 42 : 1;
}
