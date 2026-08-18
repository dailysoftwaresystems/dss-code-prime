/* ═══ DSS PLATFORM RUNTIME — <unistd.h> for the `pe` object format ══════════
 *
 * The IMPLEMENTATION half of the names mingw-w64's <unistd.h> declares ITSELF
 * rather than re-exporting from <io.h>/<process.h>. The DECLARATION half is
 * `src/dss-config/shippedLibs/unistd.json`; that descriptor's per-format
 * `realization` map is the single fact binding the two, exactly as
 * `dirent.json` binds `dirent.c`.
 *
 * ★ WHY A BODY AND NOT A `linkName`. Each of these names has NO export of its
 * own spelling in any Windows platform image, and for the first pair the
 * nearest-looking export is a TRAP:
 *
 *   - `sleep`  — ucrtbase DOES export `_sleep`, and it is the wrong function.
 *     POSIX `sleep` takes SECONDS; the CRT's `_sleep` takes MILLISECONDS. A
 *     `linkName` onto it would link clean, load clean, and sleep for 1/1000th
 *     of the requested time — the silent-wrong-answer class this project
 *     refuses outright. MEASURED: `objdump -p ucrtbase.dll` (4340 name-table
 *     entries) finds `_sleep`, and finds NO `sleep` and NO `usleep`.
 *   - `ftruncate` — mingw realizes it as a HEADER INLINE over `_chsize`, so
 *     there is no symbol of that name to bind at all. The authoring check that
 *     governs `linkName` ("does a real compiler for THIS target emit THIS name
 *     for THIS C identifier") therefore fails for it by construction, which is
 *     precisely the signal that the answer is a body.
 *
 * ★★ THE LAYOUT/SIGNATURE AGREEMENT IS CHECKED BY THE COMPILER. This unit
 * `#include`s <unistd.h>, so the descriptor's own declarations are in scope and
 * each definition below must match the row that publishes it. Change a pe row's
 * signature in `unistd.json` and this translation unit STOPS COMPILING — there
 * is no second copy of the ABI to drift.
 *
 * PROVENANCE — every signature is mingw-w64's, measured against
 * `x86_64-w64-mingw32-gcc` (reference compilers ARE the spec):
 *   unsigned int sleep(unsigned int);            int usleep(useconds_t);
 *   int ftruncate(int, off32_t);                 int ftruncate64(int, off64_t);
 *   int truncate(const char *, off32_t);         int truncate64(const char *, off64_t);
 *   void swab(char *, char *, int);
 * `off32_t` is `long`, which LLP64 makes 32-bit — hence the i32 offset on the
 * un-suffixed pair and i64 on the `64` pair. That split is the whole reason both
 * spellings exist, and collapsing them would silently truncate every offset
 * above 2 GiB.
 */

#include <unistd.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>

/* POSIX: return the number of seconds NOT slept (0 when the full interval
 * elapsed). Win32 `Sleep` is not interruptible by a signal in this model, so it
 * always completes and the answer is always 0 — reported honestly rather than
 * inventing a remainder no caller could act on. */
unsigned int sleep(unsigned int seconds) {
    /* Milliseconds are a 32-bit unsigned on Win32, so anything at or above
     * (0xFFFFFFFF / 1000) seconds would wrap. Clamp to INFINITE-1 ms instead of
     * wrapping into a SHORT sleep, which is the dangerous direction. */
    if (seconds >= 4294967u) {
        Sleep(4294966000u);
    } else {
        Sleep(seconds * 1000u);
    }
    return 0u;
}

/* POSIX microseconds -> Win32 milliseconds, ROUNDED UP. A sub-millisecond
 * request must still yield the processor rather than becoming Sleep(0), and
 * sleeping slightly longer than asked is the safe direction; truncating toward
 * zero would turn a 999 us delay into no delay at all. */
int usleep(unsigned int useconds) {
    Sleep((useconds + 999u) / 1000u);
    return 0;
}

/* mingw's <unistd.h> defines ftruncate as an inline `return _chsize(fd, len);`
 * — reproduced here as a real symbol because DSS publishes a declaration and
 * must therefore publish a definition to go with it. */
int ftruncate(int fd, long length) {
    return _chsize(fd, length);
}

/* The 64-bit-offset twin. `_chsize_s` returns an errno_t (0 on success), NOT
 * the 0/-1 of the POSIX contract, so the result is NORMALISED here; returning
 * it raw would make every failure look like a distinct errno-shaped success to
 * a caller testing `< 0`. */
int ftruncate64(int fd, long long length) {
    if (_chsize_s(fd, length) != 0) return -1;
    return 0;
}

/* Path-based truncation: open, resize, close. The fd is closed on EVERY exit
 * path including the failure one — a leaked descriptor here would be a slow
 * resource bug invisible to any single test. */
int truncate(const char *path, long length) {
    int fd;
    int rc;

    if (path == 0) return -1;
    fd = _open(path, O_WRONLY | _O_BINARY);
    if (fd < 0) return -1;
    rc = _chsize(fd, length);
    if (_close(fd) != 0) return -1;
    if (rc != 0) return -1;
    return 0;
}

int truncate64(const char *path, long long length) {
    int fd;
    int rc;

    if (path == 0) return -1;
    fd = _open(path, O_WRONLY | _O_BINARY);
    if (fd < 0) return -1;
    rc = _chsize_s(fd, length);
    if (_close(fd) != 0) return -1;
    if (rc != 0) return -1;
    return 0;
}

/* Copy `n` bytes from `from` to `to`, swapping each ADJACENT PAIR. An odd `n`
 * leaves the trailing byte alone (the last pair is incomplete), and a negative
 * or <2 count copies nothing — both are the historical CRT/glibc behaviours.
 * Written out rather than bound to ucrtbase's `_swab` because this descriptor's
 * pe arm is realized as a whole: one format cannot name both a library image
 * and a source (refusal R3), and a six-line byte loop is not worth splitting
 * the arm for. */
void swab(char *from, char *to, int n) {
    int i;

    if (from == 0 || to == 0 || n < 2) return;
    i = 0;
    while (i + 1 < n) {
        char a;
        char b;
        a = from[i];
        b = from[i + 1];
        to[i] = b;
        to[i + 1] = a;
        i = i + 2;
    }
}
