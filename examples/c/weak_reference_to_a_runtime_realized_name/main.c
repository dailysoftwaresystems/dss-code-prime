/* D-LK-ARCHIVE-SEARCH-FETCHES-A-MEMBER-FOR-A-WEAK-REFERENCE (P69 round 5): a WEAK
 * reference to a C library function binds. To a program, DSS's runtime IS the
 * platform's library, so a name whose body DSS ships as source on the format (a
 * descriptor's `realization`) answers a weak reference exactly as a name the
 * platform's own image exports does.
 *   * `strtol` — DSS ships its body on pe and Mach-O (the C23 binary prefixes);
 *     on ELF it is libc.so.6's. glibc 2.39 (gcc 13.3.0 and clang 18.1.3 on
 *     x86_64, gcc on aarch64) and libSystem (Apple clang 21, arm64 and x86_64)
 *     bind this reference and call the function on their default link
 *     (✔MEASURED 2026-10-08, the first two checks alone).
 *   * `memset_explicit` — C23 7.26.6.2, which no measured reference library has;
 *     DSS ships its body on every format, ELF included.
 * Round 4 searched DSS's runtime archive as an operator's, and an ELF or PE
 * archive search fetches nothing for a weak reference, so `strtol` read as NULL
 * on pe and `memset_explicit` on pe and ELF.
 *
 * THE FORK THIS DECIDES, and its cost: MinGW gcc + GNU ld's PE linker answer 1
 * for the first check when the function is CALLED — the weak external asks no
 * library search and the UCRT's import library is an archive — and a static
 * glibc answers NULL for a library function nothing else pulls in (`puts`).
 * DSS answers as the default, dynamic link of each platform does.
 *
 * Each check has its own code. */
extern long strtol(const char *, char **, int) __attribute__((weak));
extern void *memset_explicit(void *, int, __SIZE_TYPE__) __attribute__((weak));

int main(void) {
    char b[4] = {7, 7, 7, 7};
    if (!strtol) return 1;
    if (strtol("40", 0, 10) != 40) return 2;
    if (!memset_explicit) return 3;
    memset_explicit(b, 2, 1);
    return 40 + b[0];
}
