/* c53 (POSIX shipped-symbol batch): ship lseek/sysconf/getpid (unistd.json) +
 * nanosleep (time.json) + gettimeofday (sys/time.json) + the _SC_PAGESIZE
 * constant (unistd.json, per-format elf=30 / macho=29). All five were
 * S0001-undeclared in SQLite's os_unix: lseek (sqlite3.c:43723/:43852),
 * sysconf(_SC_PAGESIZE) (:44743), getpid (:40473), nanosleep (:47388),
 * gettimeofday (:47437). Config-only (the c51 mechanism); gated [elf,macho]
 * (a pe build uses os_win), so NO pe target.
 * Anchors: D-FFI-UNISTD-LSEEK-SYSCONF-GETPID (lseek/sysconf/getpid + _SC_PAGESIZE),
 * D-FFI-TIME-NANOSLEEP (nanosleep), D-FFI-SYSTIME-GETTIMEOFDAY (gettimeofday).
 *
 * RED-ON-DISABLE: remove any one of the 5 symbols, or the _SC_PAGESIZE constant,
 * from its descriptor -> S0001 undeclared -> the corpus fails to compile.
 *
 * VALUE-CORRECT, deterministic (sum = 42):
 *   getpid() > 0                                                       (+8)
 *   sysconf(_SC_PAGESIZE) == getpagesize() > 0                         (+8)
 *     ^ a STRONG per-target check: _SC_PAGESIZE must map to the same
 *       page-size quantity getpagesize() reports; a wrong per-format value
 *       (e.g. 30 on macho) would make sysconf return a DIFFERENT quantity.
 *   gettimeofday(&tv,0)==0 && tv.tv_sec > 1e9 (post-2001 wall clock)   (+8)
 *   nanosleep(1 microsecond)==0                                       (+8)
 *   lseek(fd, 5000000000, SEEK_SET) == 5000000000                     (+10)
 *     ^ proves the i64 RETURN WIDTH: 5e9 > 2^32, so a 32-bit lseek return
 *       would truncate to 705032704. off_t is i64; a regular file admits a
 *       seek past EOF without writing (a sparse hole), returning the offset. */
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>

int main(void) {
    int score = 0;

    if (getpid() > 0) score += 8;

    if (sysconf(_SC_PAGESIZE) == getpagesize() && getpagesize() > 0) score += 8;

    struct timeval tv;
    if (gettimeofday(&tv, 0) == 0 && tv.tv_sec > 1000000000) score += 8;

    struct timespec req;
    req.tv_sec = 0;
    req.tv_nsec = 1000;            /* 1 microsecond */
    if (nanosleep(&req, 0) == 0) score += 8;

    int fd = open("/tmp/dss_c53_lseek.tmp", O_RDWR | O_CREAT | O_TRUNC, 384);  /* 0600 */
    if (fd >= 0) {
        long long off = lseek(fd, 5000000000LL, SEEK_SET);   /* > 2^32 */
        if (off == 5000000000LL) score += 10;
        close(fd);
        unlink("/tmp/dss_c53_lseek.tmp");
    }

    return score;   /* 8 + 8 + 8 + 8 + 10 = 42 */
}
