/* c83 (D-FFI-MACHO-TIMEVAL-TV-USEC-WIDTH) — shipped <sys/time.h> struct timeval
 * on macOS: the tv_usec WIDTH witness + the RED-ON-DISABLE pin for the macho
 * variant. THE same-size trap again (c15c stat taught it): Darwin's timeval is
 * 16 bytes — the SAME sizeof as glibc's {i64,i64} — but tv_usec is
 * __darwin_suseconds_t = __int32_t (xnu bsd/sys/_types.h), not glibc's long,
 * while tv_sec stays long (bsd/arm/_types.h __darwin_time_t). So sizeof CANNOT
 * discriminate; the pins are:
 *   (a) sizeof(tv.tv_usec) == 4 — the folded field WIDTH;
 *   (b) the STORE-width witness: poison the 4 trailing padding bytes (12..15),
 *       store tv_usec — an i32 store leaves them intact, a stale i64 store
 *       zeroes them (codegen truth, not just frontend type truth);
 *   (c) the real-libSystem LOAD witness: gettimeofday writes a genuine i32
 *       microseconds value; through the i32 field it reads back in [0, 1e6) —
 *       through a stale i64 field the read folds 4 UNDEFINED padding bytes
 *       into the high half (little-endian misread of stack garbage).
 * RED-ON-DISABLE: regress the macho variant's tv_usec to i64 (or neuter the
 * variant selector) → (a) fails → exit 4 (and (b)/(c) would too); DELETE the
 * macho variant → struct timeval is not injected → the compile fails. */
#include <sys/time.h>

int main(void) {
    struct timeval tv;
    char *p = (char *)&tv;

    /* (a) the compile-time width pins */
    if (sizeof(struct timeval) != 16) return 2;   /* same TOTAL size as elf   */
    if (sizeof(tv.tv_sec)  != 8) return 3;        /* __darwin_time_t = long   */
    if (sizeof(tv.tv_usec) != 4) return 4;        /* THE divergence (elf: 8)  */

    /* (b) the runtime STORE-width witness */
    p[12] = 1; p[13] = 2; p[14] = 3; p[15] = 4;   /* poison the trailing pad  */
    tv.tv_usec = 7;                               /* i32 store: bytes 8..11   */
    if (p[12] != 1 || p[13] != 2 || p[14] != 3 || p[15] != 4) return 5;
    if (tv.tv_usec != 7) return 6;

    /* (c) the real-libSystem LOAD witness (runs on the macos-latest CI leg) */
    if (gettimeofday(&tv, 0) != 0) return 7;
    if (tv.tv_sec < 1000000000) return 8;                   /* post-2001 clock */
    if (tv.tv_usec < 0 || tv.tv_usec >= 1000000) return 9;  /* [0, 1e6)        */

    return 42;
}
