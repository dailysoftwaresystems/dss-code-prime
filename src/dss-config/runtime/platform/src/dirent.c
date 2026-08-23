/* ═══ DSS PLATFORM RUNTIME — <dirent.h> for the `pe` object format ═══════════
 *
 * POSIX opendir/readdir/closedir over the Win32 find-file primitives. This is
 * the IMPLEMENTATION half of the pair whose DECLARATION half is
 * `src/dss-config/shippedLibs/dirent.json`: that descriptor declares the ABI,
 * this file realizes it, and the descriptor's per-format `realization` map is
 * the single fact that binds the two (`{"pe": {"unit": "dirent"}}`).
 *
 * ★ WHY THIS FILE EXISTS AT ALL. Windows has no POSIX directory API. Every
 * production toolchain answers that the same way — the COMPILER synthesizes
 * only stateless glue (builtins, thunks, TLS sequences, import stubs, va_arg,
 * libcall lowering), and a RUNTIME LIBRARY OF COMPILED SOURCE provides
 * everything with state, allocation or nontrivial control flow. opendir holds
 * a handle across three calls, allocates, and buffers an entry: it is squarely
 * the second category. mingw-w64 reaches the identical conclusion and ships
 * the identical shape — ordinary C in `mingw-w64-crt/misc/dirent.c` over these
 * very primitives. gcc's literal answer here is "link libmingwex"; DSS cannot
 * take it without breaking BUILD ANY TARGET INSIDE ANY HOST
 * (D-HARNESS-CROSS-HOST-ANY-TARGET) and re-adopting the third-party runtime
 * dependency the pe→UCRT migration ran to eliminate. So DSS ships its own.
 *
 * ★★ WIDE, NOT NARROW — AND THAT IS A CORRECTNESS DECISION, NOT A PREFERENCE.
 * The narrow `_findfirst64i32` family interprets and produces file names in the
 * process ANSI code page. Any name containing a character outside that code
 * page comes back MANGLED — no error, no short read, just wrong bytes in
 * `d_name`. That is a silent-wrongness class the bar forbids outright, so this
 * unit uses `_wfindfirst64i32`/`_wfindnext64i32` and converts through UTF-8
 * with `MultiByteToWideChar`/`WideCharToMultiByte`. SQLite's own `windirent`
 * shim made the same call for the same reason.
 *
 * ★ THE OPAQUE `DIR` IS HONOURED, NOT CIRCUMVENTED. `dirent.json` types `DIR`
 * as a typedef to the EMPTY named struct (the `FILE` precedent) so user code
 * can only ever hold a `DIR *` and member access on it fails loud. This unit
 * therefore does NOT reach into `DIR`: it allocates its own private
 * `DssDirStream` and hands back a `void *`, exactly as the descriptor's
 * `fn(ptr<char>) -> ptr<void>` says. The three definitions below spell the
 * descriptor's signature verbatim; a divergence is a compile error in this
 * file, because <dirent.h> is included and the descriptor's declarations are
 * therefore in scope.
 *
 * ★★★ AND THAT INCLUDE IS THE STRUCTURAL POINT, NOT A CONVENIENCE. `struct
 * dirent` is declared ONCE — in the descriptor — and this file consumes that
 * one declaration. So the agreement between the ABI DSS publishes and the
 * bytes DSS writes is checked BY THE COMPILER, on every build: mutate the pe
 * `struct dirent` in `dirent.json` (drop `d_name`, rename it, change its
 * extent) and this translation unit STOPS COMPILING. There is no second copy
 * of the layout to drift, and no way to ship a runtime that disagrees with its
 * own header.
 *
 * PROVENANCE — the pe `struct dirent` layout matches mingw-w64's, which is the
 * de-facto POSIX-on-Windows ABI (reference compilers ARE the spec):
 *   long d_ino; unsigned short d_reclen; unsigned short d_namlen;
 *   char d_name[FILENAME_MAX], where mingw's FILENAME_MAX is 260.
 * `d_ino` and `d_reclen` are ALWAYS ZERO there — Win32 exposes neither an
 * inode number nor an on-disk record length — and this unit reproduces that
 * rather than inventing values a caller might come to trust.
 */

#include <dirent.h>
#include <io.h>
#include <stdlib.h>
#include <windows.h>

/* The private stream object `opendir` returns as an opaque `void *`.
 *
 * `pending` exists because Win32 has no "open the directory, then read the
 * first entry" split: `_wfindfirst64i32` OPENS and READS in one call. So the
 * first `readdir` must hand back what `opendir` already fetched instead of
 * advancing, or the directory's first entry (always ".") is silently dropped. */
struct DssDirStream {
    long long           find;     /* _wfindfirst64i32 handle; -1 = closed/none */
    int                 pending;  /* 1 = `data` holds an unread result         */
    struct _wfinddata_t data;     /* the Win32 find record (wide name)         */
    struct dirent       entry;    /* the POSIX record handed back to callers   */
};

/* UTF-8 -> UTF-16, into a freshly malloc'd buffer with `extra` spare wide
 * units past the terminator (opendir needs two, for the "\\*" it appends).
 * Returns 0 on any failure — a conversion this unit cannot complete is an
 * error it reports, never a truncated name it passes off as the real one. */
static unsigned short *dssWidenUtf8(const char *s, int extra) {
    int             n;
    unsigned short *w;

    /* -1 counts the terminator, so `n` already includes it. */
    n = MultiByteToWideChar(CP_UTF8, 0, (char *)s, -1, 0, 0);
    if (n <= 0) return 0;
    w = (unsigned short *)malloc((unsigned long long)(n + extra) * 2);
    if (w == 0) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, (char *)s, -1, w, n) != n) {
        free(w);
        return 0;
    }
    return w;
}

void *opendir(const char *name) {
    struct DssDirStream *d;
    unsigned short      *pattern;
    int                  i;

    if (name == 0 || name[0] == 0) return 0;

    d = (struct DssDirStream *)malloc(sizeof(struct DssDirStream));
    if (d == 0) return 0;

    /* Two spare wide units: the separator and the wildcard. The terminator's
     * slot is already counted by the -1 length above, and it is reused for the
     * separator, so the appended run is "\\*" + a new terminator = 3 units of
     * which 1 is the old terminator's. */
    pattern = dssWidenUtf8(name, 2);
    if (pattern == 0) {
        free(d);
        return 0;
    }
    i = 0;
    while (pattern[i] != 0) i = i + 1;
    /* Do not double the separator when the caller already ended with one —
     * "C:\\" is a legal directory whose name IS its trailing separator. */
    if (i > 0 && pattern[i - 1] != 92 && pattern[i - 1] != 47) {
        pattern[i] = 92; /* '\' */
        i = i + 1;
    }
    pattern[i] = 42; /* '*' */
    pattern[i + 1] = 0;

    d->find = _wfindfirst64i32(pattern, &d->data);
    free(pattern);
    if (d->find == -1) {
        free(d);
        return 0;
    }
    d->pending = 1;
    return (void *)d;
}

void *readdir(void *dirp) {
    struct DssDirStream *d;
    int                  n;

    d = (struct DssDirStream *)dirp;
    if (d == 0 || d->find == -1) return 0;

    if (d->pending == 0) {
        if (_wfindnext64i32(d->find, &d->data) != 0) return 0;
    }
    d->pending = 0;

    /* The whole reason this unit is wide: `data.name` is UTF-16, `d_name` is
     * bytes. A failed conversion returns 0 (end-of-directory to the caller)
     * rather than a half-written name — this is the one place a silent
     * mangling could enter, so it is the one place that refuses. */
    n = WideCharToMultiByte(CP_UTF8, 0, d->data.name, -1,
                            d->entry.d_name, (int)sizeof(d->entry.d_name),
                            0, 0);
    if (n <= 0) return 0;

    /* mingw-w64 semantics, reproduced deliberately: Win32 exposes neither an
     * inode number nor an on-disk record length, so both stay 0 rather than
     * carrying a value a caller might come to trust. `n` counts the
     * terminator; `d_namlen` does not. */
    d->entry.d_ino = 0;
    d->entry.d_reclen = 0;
    d->entry.d_namlen = (unsigned short)(n - 1);
    return (void *)&d->entry;
}

int closedir(void *dirp) {
    struct DssDirStream *d;
    int                  rc;

    d = (struct DssDirStream *)dirp;
    if (d == 0) return -1;
    rc = 0;
    if (d->find != -1) {
        if (_findclose(d->find) != 0) rc = -1;
        d->find = -1;
    }
    free(d);
    return rc;
}
