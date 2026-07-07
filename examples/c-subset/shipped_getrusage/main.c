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
 * RED-ON-DISABLE: delete sys/resource.json -> F001A on the include. */
#include <sys/resource.h>

int main(void) {
    struct rusage r;
    long sentinel = 7;               /* stack canary right below/above r */
    r.ru_nivcsw = -1;                /* the LAST field (offset 136) */
    int rc = getrusage(RUSAGE_SELF, &r);
    if (rc != 0) return 1;           /* libc must accept the pointer + fill it */
    if (r.ru_utime.tv_sec < 0) return 2;      /* nested member via timeval scope */
    if (r.ru_utime.tv_usec < 0 || r.ru_utime.tv_usec > 999999) return 3;
    if (r.ru_stime.tv_sec < 0) return 4;
    if (r.ru_nivcsw < 0) return 5;   /* libc overwrote the -1 marker (>=0 count) */
    if (sentinel != 7) return 6;     /* a wrong sizeof would smash the frame */
    return 42;
}
