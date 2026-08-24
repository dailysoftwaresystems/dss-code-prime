/* c46 (D-FFI-TIME-STRUCT-TIMESPEC) — ship `struct timespec` from <time.h> (the
 * next sqlite frontier after c45 cleared FILENAME_MAX). sqlite os_unix does
 * `struct timespec sp;` (a BY-VALUE local, sqlite3.c:47379, fed to clock_gettime),
 * but DSS's shipped <time.h> (time.json) shipped `struct tm` + time_t/clock_t but
 * NOT `struct timespec` -> the type was incomplete -> a by-value object failed
 * error[S0028] S_IncompleteTypeObject. Added `struct timespec` to time.json's
 * `structs` surface: `{ time_t tv_sec; long tv_nsec }` = {i64, i64}, 16 bytes,
 * 8-aligned -- BYTE-IDENTICAL on glibc (elf) and Darwin (macho) LP64 (tv_sec=time_t
 * is i64; tv_nsec=long is i64 on LP64), so FLAT (no per-target variant), like
 * `struct tm`. The layout matters: libc's clock_gettime writes the real 16 bytes
 * and the program reads tv_sec/tv_nsec at OUR offsets.
 *
 * <time.h> is gated availableObjectFormats:[elf,macho] (the FULL-UNIX-build
 * targets); a windows-pe build uses os_win (a separate axis), so this corpus omits
 * the pe target (a pe #include <time.h> fails loud by design).
 *
 * RED-ON-DISABLE: remove `struct timespec` from time.json -> the type is incomplete
 * -> `struct timespec sp;` fails S0028 (would not compile). */
#include <time.h>

int main(void) {
    struct timespec sp;            /* a by-value local — the sqlite os_unix shape */
    if (sizeof(struct timespec) != 16) return 1;   /* {i64,i64}, 16 bytes */

    sp.tv_sec  = 1234567890;       /* a 64-bit value — exercises the i64 tv_sec */
    sp.tv_nsec = 999999999;        /* near 1e9 — the nanosecond field */
    if (sp.tv_sec  != 1234567890) return 2;        /* field round-trips */
    if (sp.tv_nsec != 999999999) return 3;

    /* the two fields are at distinct 8-byte offsets (not aliased) */
    sp.tv_sec = 7;
    if (sp.tv_nsec != 999999999) return 4;         /* writing tv_sec didn't clobber tv_nsec */

    return 42;
}
