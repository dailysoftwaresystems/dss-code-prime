/* P69 (lane `cs`, D-CSUBSET-GNUC-PREDEFINE-SELECTS-UNIMPLEMENTED-BUILTIN, review M5(a)): the ISO C
 * <string.h> and <ctype.h> functions DSS's shipped descriptors lacked, each called through its
 * header and checked against the value C gives it. Exit 42 when every check holds, otherwise 100
 * plus the FIRST check that does not:
 *   1 strcoll and strxfrm (the "C" locale collates as strcmp, and the transform is a copy),
 *   2 strpbrk, 3 strtok (empty fields skipped, then a null pointer), 4 memccpy (C23: the copy
 *   stops after the byte, and a null pointer when it is absent), 5 strdup (C23), 6 strndup (C23:
 *   at most n bytes, always terminated), 7 memset_explicit (C23), 8 isblank (C99), 9 the library
 *   builtin `__builtin_strndup`, wherever `__has_builtin` answers 1 for it: it binds the very
 *   function 6 calls.
 * How each one is PROVIDED differs, and that is the point of the row: a C library export on
 * every format for 1-3 and 8 (glibc, ucrtbase.dll, libSystem) and on elf and macho for 4-6 and
 * 9; ucrtbase.dll's `_memccpy` / `_strdup` on pe for 4-5 (its only spellings); a body DSS's
 * platform runtime ships for 6 and 9 on pe and for 7 everywhere, because no shipped C library
 * has one there. A reference compiler runs only the checks ITS library provides in that mode
 * (the HAVE_ macros below); DSS (`__DSSCP__`) runs every one. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#if defined(__DSSCP__) || defined(_WIN32) || (defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L)
#define HAVE_MEMCCPY_STRDUP 1   /* C23; glibc declares them in its C23 mode, both Windows libraries in every mode */
#else
#define HAVE_MEMCCPY_STRDUP 0
#endif
#if defined(__DSSCP__) || (!defined(_WIN32) && defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L)
#define HAVE_STRNDUP 1          /* C23; glibc's C23 mode — ucrtbase.dll exports none */
#else
#define HAVE_STRNDUP 0
#endif
#if defined(__DSSCP__)
#define HAVE_MEMSET_EXPLICIT 1  /* C23; no reference's C library exports one */
#else
#define HAVE_MEMSET_EXPLICIT 0
#endif

static int check(int argc) {
    char const *const sep = argc > 0 ? "," : ";";

    /* 1: strcoll / strxfrm */
    char xf[8];
    size_t const xn = strxfrm(xf, "hello", sizeof xf);
    if (!(strcoll("abc", "abd") < 0 && strcoll("b", "a") > 0 && strcoll("same", "same") == 0)
        || xn != 5 || strcmp(xf, "hello") != 0)
        return 1;

    /* 2: strpbrk */
    char const *const kv = "key=value";
    char *const eq = strpbrk(kv, "=:");
    if (eq == NULL || eq - kv != 3 || strpbrk(kv, "#!") != NULL) return 2;

    /* 3: strtok */
    char line[] = "a,bb,,ccc";
    char *const t1 = strtok(line, sep);
    char *const t2 = strtok(NULL, sep);
    char *const t3 = strtok(NULL, sep);
    char *const t4 = strtok(NULL, sep);
    if (t1 == NULL || strcmp(t1, "a") != 0 || t2 == NULL || strcmp(t2, "bb") != 0
        || t3 == NULL || strcmp(t3, "ccc") != 0 || t4 != NULL)
        return 3;

#if HAVE_MEMCCPY_STRDUP
    /* 4: memccpy */
    char dst[16];
    memset(dst, 'z', sizeof dst);
    char *const after = memccpy(dst, "abc;def", ';', sizeof dst);
    if (after != dst + 4 || memcmp(dst, "abc;", 4) != 0 || dst[4] != 'z'
        || memccpy(dst, "abc", '#', 3) != NULL)
        return 4;

    /* 5: strdup */
    char *const d = strdup(argc > 0 ? "forty-two" : "");
    if (d == NULL || strcmp(d, "forty-two") != 0) return 5;
    free(d);
#endif

#if HAVE_STRNDUP
    /* 6: strndup */
    char *const n1 = strndup("abcdef", 3);
    char *const n2 = strndup("ab", 10);
    if (n1 == NULL || strcmp(n1, "abc") != 0 || n2 == NULL || strcmp(n2, "ab") != 0) return 6;
    free(n1);
    free(n2);
#endif

#if HAVE_MEMSET_EXPLICIT
    /* 7: memset_explicit */
    unsigned char secret[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    void *const r = memset_explicit(secret, argc - argc, sizeof secret);
    unsigned acc = 0;
    for (unsigned i = 0; i < sizeof secret; ++i) acc |= secret[i];
    if (r != (void *)secret || acc != 0) return 7;
#endif

    /* 8: isblank */
    if (!isblank(' ') || !isblank('\t') || isblank('\n') || isblank('a')) return 8;

#if HAVE_STRNDUP && defined(__has_builtin)
#if __has_builtin(__builtin_strndup)
    /* 9: __builtin_strndup */
    char *const b1 = __builtin_strndup(argc > 0 ? "forty-two" : "", 5);
    if (b1 == NULL || strcmp(b1, "forty") != 0) return 9;
    free(b1);
#endif
#endif
    return 0;
}

int main(int argc, char **argv) {
    (void)argv;
    int const failed = check(argc);
    return failed == 0 ? 42 : 100 + failed;
}
