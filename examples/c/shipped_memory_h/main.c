/* c81 (the sqlite shell.c CLI route): ship <memory.h> — shell.c's base85.c
 * section includes it UNCONDITIONALLY (shell.c:7121, not !_WIN32-guarded);
 * the header was missing -> F001A. <memory.h> is the pre-ANSI legacy alias
 * of <string.h> (every libc ships it as a forwarder), so memory.json mirrors
 * string.json's mem* surface and the FIRST-WINS injection dedup makes the
 * shell.c double-include (<string.h> AND <memory.h> in one TU) mint exactly
 * one extern per symbol. This example includes ONLY <memory.h>: the mem*
 * family must resolve from the alias alone, on every format (the include is
 * unconditional in shell.c, so pe must resolve it too — hence all 4 targets).
 * RED-ON-DISABLE: delete memory.json -> F001A on the include. */
#include <memory.h>

int main(void) {
    char src[8] = "dss-c81";     /* 7 chars + NUL (c62 zero-fill) */
    char dst[8];
    memcpy(dst, src, 8);
    if (memcmp(dst, src, 8) != 0) return 1;
    if (memchr(dst, '8', 8) == 0) return 2;   /* finds the '8' of "c81" */
    memset(dst, 0, 8);
    if (dst[0] != 0 || dst[7] != 0) return 3;
    memmove(dst, src, 8);
    if (dst[4] != 'c' || dst[6] != '1') return 4;
    return 42;
}
