/* A DSS-built static library and the program that links it BOTH call the printf family, and the program also uses
 * <threads.h> (P68 round 11, D-LK-SYNTHESIZED-LIBRARY-BODY-DEFINED-STRONG-IN-EVERY-UNIT).
 *
 * On pe the UCRT exports none of printf / fprintf / sprintf / snprintf / sscanf / vfprintf, so DSS synthesizes each
 * body into the unit that references it: the library's member (lib.c) and this program's CU each carry a copy.
 * Those copies were STRONG, and the link failed with K_SymbolRedefinedAcrossUnits for all six (MEASURED at
 * R11-F5). Every synthesized body is now Weak + Hidden: the linker keeps one copy per image, and it is exported
 * by nothing. The <threads.h> half exercises the other synthesized family on pe and Mach-O — mtx_* and call_once,
 * whose adapter is a synthesized body too — through the same linkage (only exec formats declare its vehicle, so
 * it lives in the program, not the library). Exit 42; stdout pinned. */
#include <stdio.h>
#include <threads.h>

int lib_value(FILE *out);

static once_flag g_once = ONCE_FLAG_INIT;
static int g_base;
static void init_once(void) { g_base += 14; }

int main(void) {
    mtx_t m;
    if (mtx_init(&m, mtx_plain) != thrd_success) return 1;
    if (mtx_lock(&m) != thrd_success) return 2;
    call_once(&g_once, init_once);
    call_once(&g_once, init_once);   /* once: g_base stays 14 */
    int const v = lib_value(stdout);  /* 14 */
    char buf[16];
    snprintf(buf, sizeof buf, "%d", v + g_base);
    int w = 0;
    if (sscanf(buf, "%d", &w) != 1) return 3;
    printf("main: %d\n", w);          /* 28 */
    if (mtx_unlock(&m) != thrd_success) return 4;
    mtx_destroy(&m);
    return w + 14;
}
