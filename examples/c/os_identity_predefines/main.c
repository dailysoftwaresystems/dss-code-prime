/* The operating-system and object-format IDENTITY predefines — and the platform surface each one claims, used
 * the way portable C uses it (P68, lane lm, #2: the ELF pairs predefined none of the Linux names).
 *
 *   Linux (ELF):   __linux__  __linux  __gnu_linux__  __unix__  __unix  __ELF__
 *   Darwin (Mach-O): __APPLE__  __MACH__           Windows (PE): _WIN32  _WIN64
 *
 * Each group sets one bit; the exit is 42 only when all six hold, otherwise the bitmask of the groups that did.
 *
 *   1  EXACTLY ONE operating-system family is claimed, and every spelling of it agrees (`__linux__` with
 *      `__linux` and `__gnu_linux__`; `__unix__` with `__unix`; `__APPLE__` with `__MACH__`; `_WIN32` with `_WIN64`)
 *   2  the strict-ISO namespace: the GNU-mode spellings `linux` and `unix` are NOT predefined (a program may use
 *      them as identifiers — gcc and clang define them only outside the ISO modes)
 *   3  `__ELF__` exactly where the object format is ELF (here: the Linux pairs)
 *   4  `__unix__` backs the POSIX layer: getpid, and a file round trip through open / write / lseek / read / close
 *   5  `__linux__` backs what programs take its arm for — sqlite enables pread/pwrite (HAVE_PREAD) and memory-mapped
 *      I/O (SQLITE_MAX_MMAP_SIZE, and HAVE_MREMAP under _GNU_SOURCE) on it: pwrite/pread at an offset, and a
 *      shared mapping of the file grown by mremap and read back
 *   6  the surface is ABSENT where the identity is: the Windows and Darwin arms take their own paths
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1   /* mremap, as sqlite defines it under __GNUC__ */
#endif
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/mman.h>
#endif

#if defined(__linux__)
#define FAMILIES_LINUX 1
#else
#define FAMILIES_LINUX 0
#endif
#if defined(__APPLE__)
#define FAMILIES_APPLE 1
#else
#define FAMILIES_APPLE 0
#endif
#if defined(_WIN32)
#define FAMILIES_WIN 1
#else
#define FAMILIES_WIN 0
#endif

static volatile int vZero = 0;

int main(void) {
    int bits = 0;

    /* ── 1: exactly one family, its spellings agree ───────────────────────── */
    {
        int ok = FAMILIES_LINUX + FAMILIES_APPLE + FAMILIES_WIN == 1;
#if defined(__linux__) != defined(__linux) || defined(__linux__) != defined(__gnu_linux__)
        ok = 0;
#endif
#if defined(__unix__) != defined(__unix)
        ok = 0;
#endif
#if defined(__linux__) && !defined(__unix__)
        ok = 0;   /* every Linux reference claims Unix too */
#endif
#if defined(__APPLE__) != defined(__MACH__)
        ok = 0;
#endif
#if defined(_WIN32) && !defined(_WIN64)
        ok = 0;   /* the only Windows pair is 64-bit */
#endif
#if (defined(__linux__) || defined(__unix__)) && (defined(__APPLE__) || defined(_WIN32))
        ok = 0;
#endif
#if (__linux__ + 0) != (defined(__linux__) ? 1 : 0) || (__unix__ + 0) != (defined(__unix__) ? 1 : 0)
        ok = 0;   /* each is defined as 1 where it is defined */
#endif
        if (ok) bits |= 1;
    }

    /* ── 2: `linux` and `unix` are the program's identifiers ──────────────── */
    {
        int linux = 5, unix = 7;   /* compiles only if neither name is a predefined macro */
        if (linux + unix == 12) bits |= 2;
    }

    /* ── 3: `__ELF__` where the format is ELF ─────────────────────────────── */
#if defined(__ELF__) == defined(__linux__)
    bits |= 4;
#endif

    /* ── 4: `__unix__` backs the POSIX layer (Darwin's arm runs it too) ───── */
#if defined(__unix__) || defined(__APPLE__)
    {
        /* A per-process name under /tmp: two arms of this example (debug, release) may run at once. */
        char path[64];
        int ok = getpid() > 0;
        snprintf(path, sizeof path, "/tmp/dss_os_identity_%d.tmp", (int)getpid());
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) ok = 0;
        if (ok && write(fd, "abcdefgh", 8) != 8) ok = 0;
        if (ok && lseek(fd, 2, SEEK_SET) != 2) ok = 0;
        char buf[4] = {0};
        if (ok && (read(fd, buf, 3) != 3 || memcmp(buf, "cde", 3) != 0)) ok = 0;
    #if defined(__linux__)
        /* ── 5: `__linux__` backs pread/pwrite and mmap/mremap/munmap ─────── */
        {
            int ok5 = ok;
            char p[4] = {0};
            if (ok5 && pwrite(fd, "XY", 2, 6) != 2) ok5 = 0;
            if (ok5 && (pread(fd, p, 3, 5) != 3 || memcmp(p, "fXY", 3) != 0)) ok5 = 0;
            if (ok5 && ftruncate(fd, 4096) != 0) ok5 = 0;
            char *m = ok5 ? (char *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : NULL;
            if (!ok5 || m == MAP_FAILED) ok5 = 0;
            if (ok5 && memcmp(m, "abcdefXY", 8) != 0) ok5 = 0;
            if (ok5 && ftruncate(fd, 8192) != 0) ok5 = 0;
            char *g = ok5 ? (char *)mremap(m, 4096, 8192, MREMAP_MAYMOVE) : NULL;
            if (!ok5 || g == MAP_FAILED) ok5 = 0;
            if (ok5) {
                g[8191] = 'Z';
                if (g[1] != 'b' || g[8191] != 'Z') ok5 = 0;
                if (munmap(g, 8192) != 0) ok5 = 0;
            }
            if (ok5) bits |= 16;
        }
    #else
        bits |= 16;   /* not a Linux pair: nothing to claim */
    #endif
        if (fd >= 0) close(fd);
        unlink(path);
        if (ok) bits |= 8;
    }
#else
    bits |= 8 | 16;   /* Windows: neither Unix nor Linux is claimed */
#endif

    /* ── 6: no Linux surface where Linux is not claimed ───────────────────── */
#if !defined(__linux__) && (defined(MREMAP_MAYMOVE) || defined(MAP_FAILED)) && !defined(__APPLE__)
    /* a Windows pair must not see <sys/mman.h>'s Linux names without including it */
#else
    bits |= 32;
#endif

    return (bits == 63 ? 42 : bits) + vZero;
}
