// TF-C66 (D-FFI-SHIPPED-LIBS-OS-ONLY testfixture recipe): the C99 <inttypes.h>
// format-length macros ship as an OS/libc descriptor (inttypes.json). The
// sqlite full-source testfixture uses exactly these four (PRIi64/PRIu32/PRIi32/
// PRIu64 -- e.g. wal.c's `"...:%" PRIi64` adjacent-string concat). Each expands
// to a STRING-LITERAL FRAGMENT that adjacent-string-concats into a format
// string. The 64-bit prefix is PER-FORMAT (#47-class): glibc LP64 -> "l..."
// (int64_t is `long`); Apple/Windows -> "ll..." (int64_t is `long long`). BOTH
// start with 'l' after the conversion char; the 32-bit pair is `int` (no length
// prefix) everywhere. This checks the WIDTH CLASS the descriptor must provide --
// a 64-bit specifier carries the `l` length modifier, a 32-bit one does not --
// which holds on every object format, so one exit code (42) witnesses it
// portably. The exact per-format `l` vs `ll` divergence is pinned by the
// descriptor decode test + the pe64 runtime probe.
#include <inttypes.h>

static int strEq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

int main(void) {
    const char *p64 = "%" PRIi64;   // "%li" (elf) or "%lli" (macho/pe)
    const char *u64 = "%" PRIu64;   // "%lu"       or "%llu"
    const char *p32 = "%" PRIi32;   // "%i"  everywhere
    const char *u32 = "%" PRIu32;   // "%u"  everywhere

    // 64-bit specifiers: '%', then a 'l' length run, then the conversion.
    if (p64[0] != '%' || p64[1] != 'l') return 1;
    if (u64[0] != '%' || u64[1] != 'l') return 2;
    // 32-bit specifiers: '%' then the conversion char directly (no length).
    if (!strEq(p32, "%i")) return 3;
    if (!strEq(u32, "%u")) return 4;
    // The 64-bit prefix must be STRICTLY longer than the 32-bit one (l-run
    // present) -- the property a wrong-width descriptor would break.
    int n64 = 0; while (p64[n64]) n64++;
    int n32 = 0; while (p32[n32]) n32++;
    if (n64 <= n32) return 5;
    return 42;
}
