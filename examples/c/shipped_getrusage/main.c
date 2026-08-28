/* c81 (the sqlite shell.c CLI route): ship <sys/resource.h> — shell.c's
 * performance timer calls getrusage(RUSAGE_SELF, &sBegin) into a static
 * struct rusage and reads .ru_utime/.ru_stime (shell.c:26135+); the header
 * was missing -> F001A. LAYOUT-critical: libc FILLS the caller's 144-byte
 * object, so the struct is glibc-exact (2 leading struct timeval + the 14
 * long fields; verified against bits/types/struct_rusage.h, x86_64 == arm64).
 * The nested-member reads below (.ru_utime.tv_sec / .tv_usec) prove the
 * timeval FIELD TYPE interns to the SAME TypeId as sys/time.json's timeval
 * (one field scope, either include order). The tail write/read of ru_nivcsw
 * pins the LAST field's offset (136) — a short or misaligned layout would
 * let libc's 144-byte fill clobber the sentinel below it or misread.
 * RED-ON-DISABLE: delete sys/resource.json -> F001A on the include.
 *
 * ★ D-LANG-TYPE-IDENTITY-VOCABULARY (2026-07-20): the "either include order"
 * claim above was FALSE for one cycle and nothing here noticed, because this
 * corpus included only ONE of the two headers. sys/time.json retagged its
 * timeval fields to `i64 "long"` while sys/resource.json kept bare `i64`, so
 * the tag interned as TWO TypeIds and only the FIRST-INCLUDED one got a field
 * scope: `<sys/time.h>` before `<sys/resource.h>` made the `.ru_utime.tv_sec`
 * read below fail S000D, and the reverse order compiled. So BOTH headers are
 * included here now, in the order that used to break, and a `struct timeval`
 * is cross-assigned from `r.ru_utime` — an assignment that only type-checks
 * when the two descriptors' declarations are ONE type. (The invariant is also
 * machine-checked now: `ffi::ShippedTypeConsistency` fails the compile loud on
 * a divergence, and tests/ffi/test_shipped_type_consistency.cpp sweeps every
 * shipped descriptor on every target.) */
#include <sys/time.h>
#include <sys/resource.h>

int main(void) {
    struct rusage r;
    struct timeval tv;               /* the SAME tag, via the OTHER descriptor */
    long sentinel = 7;               /* stack canary right below/above r */
    r.ru_nivcsw = -1;                /* the LAST field (offset 136) */
    int rc = getrusage(RUSAGE_SELF, &r);
    if (rc != 0) return 1;           /* libc must accept the pointer + fill it */
    if (r.ru_utime.tv_sec < 0) return 2;      /* nested member via timeval scope */
    if (r.ru_utime.tv_usec < 0 || r.ru_utime.tv_usec > 999999) return 3;
    if (r.ru_stime.tv_sec < 0) return 4;
    if (r.ru_nivcsw < 0) return 5;   /* libc overwrote the -1 marker (>=0 count) */
    if (sentinel != 7) return 6;     /* a wrong sizeof would smash the frame */
    /* Cross-descriptor identity: `tv` is <sys/time.h>'s `struct timeval`,
     * `r.ru_utime` is <sys/resource.h>'s. This assignment only type-checks when
     * the two declarations interned to ONE TypeId — the exact thing that broke. */
    tv = r.ru_utime;
    if (tv.tv_sec != r.ru_utime.tv_sec) return 7;
    if (tv.tv_usec != r.ru_utime.tv_usec) return 8;
    return 42;
}
