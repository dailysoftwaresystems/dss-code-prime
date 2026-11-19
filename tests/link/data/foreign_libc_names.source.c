/* P69 fold 3e, lane p69/xa (ledgers 547, 549): the FOREIGN units of tests/link/test_foreign_libc_names.cpp -- an
 * object a reference compiler built that calls the C library by the names ITS OWN headers give the calls, linked by
 * DSS beside a DSS `main` that returns what the unit's function returns (42 when every call answered as the
 * platform's function does).
 *
 * One unit per macro:
 *   LIBC_STRTO   strtol strtoul strtoll strtoull strtoimax strtoumax wcstoimax wcstoumax, and the prefix "0b": a C17
 *                function reads "0b1" as 0 with "b1" left, a C23 one as 1 (glibc >= 2.38 under -std=c2x/gnu2x
 *                redirects to its `__isoc23_*`; UCRT and libSystem export only the C17 functions)
 *   LIBC_SCANF   sscanf vsscanf fscanf vfscanf scanf vscanf (stdin pointed at a file first)
 *   LIBC_FORTIFY the calls a fortifying header turns into its checked entry points (Ubuntu's gcc defines
 *                _FORTIFY_SOURCE; the macOS SDK's own default level): snprintf sprintf printf fprintf vsnprintf
 *                vsprintf vfprintf fgets fread strcat strncat getcwd gethostname open read pread readlink realpath
 *                mbstowcs wcstombs, and the writes memcpy memmove memset strcpy strncpy memccpy strlcpy strlcat
 *                (the last two where the platform has them); with _FORTIFY_SOURCE=0 the plain names
 *   LIBC_CTYPE   every <ctype.h> classification and both mappings (glibc's macros read `__ctype_b_loc` and the
 *                two mapping tables' accessors)
 *   LIBC_SIGNAL  signal and raise (glibc's strict modes spell `signal` as `__sysv_signal`)
 *   LIBC_EXIT    _Exit(42), which MinGW-w64's <stdlib.h> spells `_exit`
 *   LIBC_PEXIT   POSIX's _exit(42)
 * and two units that call NO C library function, for the compiler-support routines a foreign object names:
 *   LIBC_STACKPROBE  a 64 KiB frame (Apple clang calls `___chkstk_darwin`)
 *   LIBC_LDOUBLE     binary128 `long double` arithmetic (aarch64: `__extenddftf2`, `__multf3`, `__trunctfdf2`)
 * and LIBC_WITH_MAIN beside any of them, the reference's own program of that unit.
 *
 * HOW EACH OBJECT WAS BUILT (2026-11-05, lane xa; each TWICE, byte-identical, recording no host path; the reference's
 * own program of each, the unit with -DLIBC_WITH_MAIN linked by its own toolchain, RUN: every one exits 42), from
 * this text less this paragraph:
 *   ELF (`build_elf_fixtures.sh`, WSL Ubuntu 24.04; each from a copy named foreign_libc_names.c; aarch64 run under
 *   qemu-aarch64 with the cross sysroot):
 *     gcc 13.3.0 -std=<mode> -O2 -c -D<unit>          -> *_x86_64_elf_gcc_<mode>_O2.o
 *     clang 19.1.1 -std=c17 -O2 -c -D<unit>           -> *_x86_64_elf_clang_c17_O2.o
 *     aarch64-linux-gnu-gcc 13.3.0, clang 19.1.1 --target=aarch64-linux-gnu, the same -> *_arm64_elf_*
 *   PE (`build_pe_fixtures.py`, Windows 11; each from a copy named foreign_libc_names.c):
 *     cl 19.44.35229 /nologo /O2 /MD /GS- /Brepro /experimental:deterministic /pathmap:<dir>=X:\dss-fixture
 *        [/std:clatest] /D<unit> /c                   -> *_x86_64_pe_cl_MD[_clatest]_O2.obj
 *     clang-cl 19.1.5 /nologo /O2 /MD /GS- /Brepro /D<unit> /c   -> *_x86_64_pe_clangcl_MD_O2.obj
 *     mingw-w64 gcc 13.2.0 (x86_64-ucrt-posix-seh) -O2 -std=c17 -D<unit> -c   -> *_x86_64_pe_gcc_c17_O2.o
 *       (its -std=gnu2x strto object and its LIBC_PEXIT object are byte-identical to the c17 strto and LIBC_EXIT
 *       ones, and are not committed)
 *   Mach-O (the harness's probe-reference-cc on macos-arm64-debug, run 20261105-202902-b2874be3, macOS 26):
 *     Apple clang 21.0.0 -arch arm64 -std=<mode> -O2 -ffile-prefix-map=<probe dir>=. [-U_FORTIFY_SOURCE
 *        -D_FORTIFY_SOURCE=0 for fortify0] -D<unit> -c   -> *_arm64_macho_clang_<mode>_O2.o
 *       (its -std=gnu2x strto object is byte-identical to the c17 one, and is not committed)
 * Sizes and md5s (no guard enforces one: they record WHICH BYTES were measured):
 *   strto_x86_64_elf_gcc_c17                2856  md5 09958958c30989e2dc179797b2f91c8f
 *   strto_x86_64_elf_gcc_gnu2x              2920  md5 3aa2f6194c2670cc76459d8a13b0ed77
 *   strto_x86_64_elf_clang_c17              2120  md5 2d7d04bdd18a5adf0b0741af8786523e
 *   scanf_x86_64_elf_gcc_c17                4112  md5 141cb494ea4a606b34c46ba2c58a6255
 *   scanf_x86_64_elf_clang_c17              3552  md5 f3059eb744b808c0151ddb07200ede58
 *   fortify_x86_64_elf_gcc_gnu17            9128  md5 24f81f5da25794f91b1d43e0d1b3a892
 *   ctype_x86_64_elf_gcc_c17                2432  md5 d156c01463e88087a4af278422d36052
 *   ctype_x86_64_elf_clang_c17              2104  md5 63373ecdd597dc89cab41e64edfd9b96
 *   signal_x86_64_elf_gcc_c17               1696  md5 2428ba034e996a457e9407761e205379
 *   pexit_x86_64_elf_gcc_c17                1384  md5 1272b638c00cdac8eb78f5c51f469cbf
 *   strto_arm64_elf_gcc_c17                 3152  md5 f92f1b42836cf290f892e45057ac4556
 *   strto_arm64_elf_gcc_gnu2x               3224  md5 fc1c41135fa3f4be3a3b91724db79105
 *   strto_arm64_elf_clang_c17               2320  md5 ebd14ba7a1276dbd07e7d47f32975714
 *   scanf_arm64_elf_gcc_c17                 4784  md5 b5cbcd03b050f97b6b3cf8b10a1bcb47
 *   scanf_arm64_elf_clang_c17               3672  md5 0897b0d25993a015d5d00e87736e0ee0
 *   fortify_arm64_elf_gcc_gnu17             9936  md5 8618f818507b93da53ff947c4514b940
 *   ctype_arm64_elf_gcc_c17                 2376  md5 69862b46d0bf2e161bc2fab717d8143e
 *   ctype_arm64_elf_clang_c17               2128  md5 db8b0de26d4cd247710fe2236bea2c8a
 *   signal_arm64_elf_gcc_c17                1856  md5 cb8315914034f36df11b337463404e73
 *   pexit_arm64_elf_gcc_c17                 1432  md5 1a6e5cbb884b44989a91210c04611446
 *   ldouble_arm64_elf_gcc_c17               1824  md5 1cc9d501c6b82158f59b759aeaaec255
 *   ldouble_arm64_elf_clang_c17             1600  md5 6b44e66b6bd7a72664023945c890cf11
 *   strto_x86_64_pe_cl_MD                   2753  md5 07e51938aa6d449e7ab6dcf5e7725a2f
 *   strto_x86_64_pe_cl_MD_clatest           2761  md5 914d47bd08c87b697d084137ea131f56
 *   strto_x86_64_pe_clangcl_MD              2242  md5 284618c69ebcdf0eeef020bff86e00f1
 *   strto_x86_64_pe_gcc_c17                 1656  md5 ab032729a1659dae4fab5772d2465662
 *   exit_x86_64_pe_cl_MD                    1029  md5 70b4dae454f635228acffe46b3a18258
 *   exit_x86_64_pe_gcc_c17                   784  md5 25d87c67979b373779bcba895379c8d9
 *   ctype_x86_64_pe_cl_MD                   3003  md5 2b64d0ebf9040f4043eefa0dd03325ea
 *   ctype_x86_64_pe_gcc_c17                 2183  md5 5a2d6a5829ac1abc6fc94fbb7c6e3a95
 *   strto_arm64_macho_clang_c17             1488  md5 5795167c3fd06c1f09115ead8973fb5a
 *   scanf_arm64_macho_clang_c17             2224  md5 d6d16c0d7a7edb2d46f5e9e83d7a136e
 *   fortify_arm64_macho_clang_gnu17         6024  md5 26b8c4838acbf36109f70093451afc7d
 *   fortify0_arm64_macho_clang_gnu17        5896  md5 0ee44e4dcf1b8e19d788e91dcc75d223
 *   ctype_arm64_macho_clang_c17             2808  md5 edeaeb4de9aee0f63edf269dfaad2272
 *   signal_arm64_macho_clang_c17             944  md5 e6d3f9b6b2a1394c19f1d66f2fad2c30
 *   exit_arm64_macho_clang_c17               560  md5 447054ce55e60012aa79b043d6cbf6db
 *   pexit_arm64_macho_clang_c17              560  md5 115e2f79375ac21be01336dd116f3842
 *   stackprobe_arm64_macho_clang_c17         888  md5 acfb8d024307a250b92876f196e3712b
 */
#if defined(LIBC_STRTO)
#include <inttypes.h>
#include <stdlib.h>
#include <wchar.h>
int dss_libc_names(void) {
    char *end;
    wchar_t *wend;
    if (strtol("-42", &end, 10) != -42L || *end) return 1;
    if (strtoul("42", &end, 10) != 42UL || *end) return 2;
    if (strtoll("-42", &end, 10) != -42LL || *end) return 3;
    if (strtoull("42", &end, 10) != 42ULL || *end) return 4;
    if (strtoimax("-42", &end, 10) != -42 || *end) return 5;
    if (strtoumax("42", &end, 10) != 42U || *end) return 6;
    if (wcstoimax(L"-42", &wend, 10) != -42 || *wend) return 7;
    if (wcstoumax(L"42", &wend, 10) != 42U || *wend) return 8;
#if defined(__GLIBC__) && defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L
    if (strtol("0b1", &end, 0) != 1L || *end) return 9;
#else
    if (strtol("0b1", &end, 0) != 0L || *end != 'b') return 9;
#endif
    return 42;
}
#elif defined(LIBC_SCANF)
#define _POSIX_C_SOURCE 200809L   /* fileno and dup2; the scanf family's redirect is the strict mode's still */
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
static int v_sscanf(const char *s, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int n = vsscanf(s, f, ap);
    va_end(ap);
    return n;
}
static int v_fscanf(FILE *s, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int n = vfscanf(s, f, ap);
    va_end(ap);
    return n;
}
static int v_scanf(const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int n = vscanf(f, ap);
    va_end(ap);
    return n;
}
int dss_libc_names(void) {
    int x = 0;
    if (sscanf("7", "%d", &x) != 1 || x != 7) return 11;
    if (v_sscanf("8", "%d", &x) != 1 || x != 8) return 12;
    FILE *t = tmpfile();
    if (!t) return 13;
    fputs("9 10", t);
    rewind(t);
    if (fscanf(t, "%d", &x) != 1 || x != 9) return 14;
    if (v_fscanf(t, "%d", &x) != 1 || x != 10) return 15;
    /* stdin reads a file of its own, written through its descriptor so no stream holds its bytes */
    FILE *in = tmpfile();
    if (!in || write(fileno(in), "11 12", 5) != 5 || lseek(fileno(in), 0, SEEK_SET) != 0) return 16;
    if (dup2(fileno(in), 0) != 0) return 16;
    if (scanf("%d", &x) != 1 || x != 11) return 17;
    if (v_scanf("%d", &x) != 1 || x != 12) return 18;
    return 42;
}
#elif defined(LIBC_FORTIFY)
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE   /* strlcpy and strlcat beside the POSIX names under a strict -std */
#endif
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
static int v_snprintf(char *b, size_t n, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vsnprintf(b, n, f, ap);
    va_end(ap);
    return r;
}
static int v_sprintf(char *b, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vsprintf(b, f, ap);
    va_end(ap);
    return r;
}
static int v_fprintf(FILE *s, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vfprintf(s, f, ap);
    va_end(ap);
    return r;
}
/* A length, a flag or a source the compiler cannot see: the fortifying headers call a checked entry point only when
 * the destination's size is known and the length is not provably within it (a constant safe length calls the plain
 * function, and gcc folds a strcat of known strings away). */
static size_t opaque_size(size_t n) {
    volatile size_t v = n;
    return v;
}
static int opaque_int(int n) {
    volatile int v = n;
    return v;
}
static char const *opaque_str(char const *s) {
    char const *volatile v = s;
    return v;
}
int dss_libc_names(void) {
    /* realpath's buffer is PATH_MAX (4096 on glibc, whose checked realpath demands it; 1024 on macOS, where a frame
     * past a page would also call the compiler's stack probe, LIBC_STACKPROBE's to measure) */
    char b[64], big[PATH_MAX];
    wchar_t w[8];
    if (snprintf(b, sizeof b, "%d", 42) != 2 || strcmp(b, "42")) return 21;
    if (sprintf(b, "%d", 43) != 2 || strcmp(b, "43")) return 22;
    if (printf("%d\n", 44) != 3) return 23;
    if (fprintf(stdout, "%d\n", 45) != 3) return 24;
    if (v_snprintf(b, sizeof b, "%d", 46) != 2 || strcmp(b, "46")) return 25;
    if (v_sprintf(b, "%d", 47) != 2 || strcmp(b, "47")) return 26;
    if (v_fprintf(stdout, "%d\n", 48) != 3) return 27;
    strcpy(b, opaque_str("a"));   /* a destination of unknown length, or gcc folds the strcat into a strcpy */
    strcat(b, opaque_str("b"));
    strncat(b, opaque_str("cd"), opaque_size(1));
    if (strcmp(b, "abc")) return 28;
    FILE *t = tmpfile();
    if (!t) return 29;
    fputs("line\nrest", t);
    rewind(t);
    if (!fgets(b, opaque_int(sizeof b), t) || strcmp(b, "line\n")) return 30;
    if (fread(b, 1, opaque_size(4), t) != 4 || memcmp(b, "rest", 4)) return 31;
    fclose(t);
    if (!getcwd(big, opaque_size(sizeof big))) return 32;
    if (gethostname(b, opaque_size(sizeof b)) != 0) return 33;
    char path[] = "/tmp/dss_libc_names_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0 || write(fd, "abcd", 4) != 4) return 34;
    close(fd);
    fd = open(path, opaque_int(O_RDONLY));
    if (fd < 0) return 35;
    if (read(fd, b, opaque_size(4)) != 4 || memcmp(b, "abcd", 4)) return 36;
    if (pread(fd, b, opaque_size(2), 1) != 2 || memcmp(b, "bc", 2)) return 37;
    close(fd);
    if (!realpath(path, big)) return 38;
    char link[] = "/tmp/dss_libc_names_link_XXXXXX";
    int lfd = mkstemp(link);   /* a fresh name, then the symbolic link under it */
    if (lfd < 0) return 39;
    close(lfd);
    unlink(link);
    if (symlink(path, link) != 0) return 39;
    ssize_t const got = readlink(link, big, opaque_size(sizeof big));
    unlink(link);
    unlink(path);
    if (got != (ssize_t)strlen(path) || memcmp(big, path, (size_t)got)) return 39;
    if (mbstowcs(w, "ab", opaque_size(sizeof w / sizeof w[0])) != 2 || w[0] != L'a') return 40;
    if (wcstombs(b, L"ab", opaque_size(sizeof b)) != 2 || strcmp(b, "ab")) return 41;
    /* the writes: a destination of known size, a length or a source the compiler cannot see */
    char d[16];
    memcpy(d, opaque_str("wxyz"), opaque_size(4));
    if (memcmp(d, "wxyz", 4)) return 51;
    memmove(d + 1, d, opaque_size(3));
    if (memcmp(d, "wwxy", 4)) return 52;
    memset(d, opaque_int('q'), opaque_size(2));
    if (memcmp(d, "qqxy", 4)) return 53;
    strcpy(d, opaque_str("st"));
    if (strcmp(d, "st")) return 54;
    strncpy(d, opaque_str("uv"), opaque_size(3));
    if (strcmp(d, "uv")) return 55;
    if (!memccpy(d, opaque_str("ab!cd"), '!', opaque_size(5)) || memcmp(d, "ab!", 3)) return 56;
#if defined(__APPLE__) || (defined(__GLIBC__) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 38))
    if (strlcpy(d, opaque_str("lmn"), opaque_size(sizeof d)) != 3 || strcmp(d, "lmn")) return 57;
    if (strlcat(d, opaque_str("op"), opaque_size(sizeof d)) != 5 || strcmp(d, "lmnop")) return 58;
#endif
    return 42;
}
#elif defined(LIBC_CTYPE)
#include <ctype.h>
static int opaque_char(int c) {
    volatile int v = c;
    return v;
}
int dss_libc_names(void) {
    /* every classification of <ctype.h> and both mappings, on a character the compiler cannot see, in the "C" locale */
    if (!isalnum(opaque_char('a')) || isalnum(opaque_char('-'))) return 61;
    if (!isalpha(opaque_char('Q')) || isalpha(opaque_char('7'))) return 62;
    if (!isblank(opaque_char('\t')) || isblank(opaque_char('x'))) return 63;
    if (!iscntrl(opaque_char('\n')) || iscntrl(opaque_char('x'))) return 64;
    if (!isdigit(opaque_char('7')) || isdigit(opaque_char('x'))) return 65;
    if (!isgraph(opaque_char('#')) || isgraph(opaque_char(' '))) return 66;
    if (!islower(opaque_char('m')) || islower(opaque_char('M'))) return 67;
    if (!isprint(opaque_char(' ')) || isprint(opaque_char('\a'))) return 68;
    if (!ispunct(opaque_char('!')) || ispunct(opaque_char('a'))) return 69;
    if (!isspace(opaque_char('\r')) || isspace(opaque_char('a'))) return 70;
    if (!isupper(opaque_char('M')) || isupper(opaque_char('m'))) return 71;
    if (!isxdigit(opaque_char('F')) || isxdigit(opaque_char('g'))) return 72;
    if (tolower(opaque_char('M')) != 'm' || tolower(opaque_char('1')) != '1') return 73;
    if (toupper(opaque_char('m')) != 'M' || toupper(opaque_char('1')) != '1') return 74;
    return 42;
}
#elif defined(LIBC_SIGNAL)
#include <signal.h>
static volatile sig_atomic_t seen;
static void on_signal(int s) { seen = s; }
int dss_libc_names(void) {
    /* glibc's strict modes spell `signal` as `__sysv_signal` */
    if (signal(SIGINT, on_signal) == SIG_ERR) return 81;
    if (raise(SIGINT) != 0) return 82;
    return seen == SIGINT ? 42 : 83;
}
#elif defined(LIBC_STACKPROBE)
/* NOT the C library: the compiler's own stack probe a frame past a page calls before it calls out (Apple clang:
 * `___chkstk_darwin`) */
#include <string.h>
#include <unistd.h>
int dss_libc_names(void) {
    char huge[65536];
    if (!getcwd(huge, sizeof huge)) return 91;
    return strlen(huge) > 0 ? 42 : 92;
}
#elif defined(LIBC_LDOUBLE)
/* NOT the C library: the compiler's own soft-float routines binary128 `long double` arithmetic calls (aarch64:
 * `__extenddftf2`, `__multf3`, `__trunctfdf2`) */
static double opaque_double(double d) {
    volatile double v = d;
    return v;
}
int dss_libc_names(void) {
    volatile long double k = 3.0L;
    long double const x = (long double)opaque_double(1.5) * k;
    return (double)x == 4.5 ? 42 : 92;
}
#elif defined(LIBC_EXIT)
#include <stdlib.h>
int dss_libc_names(void) { _Exit(42); }
#elif defined(LIBC_PEXIT)
#include <unistd.h>
int dss_libc_names(void) { _exit(42); }
#else
#error "name the unit: -DLIBC_STRTO, -DLIBC_SCANF, -DLIBC_FORTIFY, -DLIBC_CTYPE, -DLIBC_SIGNAL, -DLIBC_STACKPROBE, -DLIBC_LDOUBLE, -DLIBC_EXIT or -DLIBC_PEXIT"
#endif
#if defined(LIBC_WITH_MAIN)   /* the REFERENCE's own program of the unit: its `main` returns what the unit returns */
int main(void) { return dss_libc_names(); }
#endif
