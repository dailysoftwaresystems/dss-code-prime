/* C23's formatted I/O and integer conversions (D-C-C23-CONVERSIONS-MISSING-ON-THE-UCRT-AND-LIBSYSTEM), end to end on
 * every pair: printf's `b`/`B` conversions and `wN`/`wfN` lengths, `%n` of every length, scanf's `%b` and the `0b`
 * form of `%i`, and the `0b` subject of the strto family. glibc renders all of them itself; on the UCRT and libSystem
 * DSS's own engine (runtime/platform/src/stdio.c, stdlib_strto.c, inttypes.c) renders exactly these and hands every
 * other specification to the platform. Conversions whose spelling is the PLATFORM's (%p, %a, a long double, a wide
 * string) are compared in-program against the platform's own rendering of the same specification, never printed. */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int ok, char const *what) {
    if (!ok) {
        ++failures;
        printf("FAIL %s\n", what);
    }
}

static int via_vsnprintf(char *buf, size_t n, char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int const r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

static int via_vsprintf(char *buf, char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int const r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

static int via_vsscanf(char const *in, char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int const r = vsscanf(in, fmt, ap);
    va_end(ap);
    return r;
}

static int via_vfprintf(FILE *f, char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int const r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int main(void) {
    /* 1. b / B with every flag, width and precision form (star widths/precisions, negative ones included). */
    printf("[%b] [%B] [%#b] [%#B] [%08b] [%-8b|] [%.12b] [%*b] [%.*b] [%-*b|] [%*b|] [%#.0b] [%.0b] [%#010b] [%+b] [% b]\n",
           5u, 5u, 5u, 5u, 5u, 5u, 5u, 10, 5u, 3, 5u, 6, 5u, -6, 5u, 0u, 0u, 5u, 5u, 5u);

    /* 2. every length on b, and C23's exact- and fast-width lengths on every integer conversion. */
    printf("[%hhb] [%hb] [%lb] [%llb] [%jb] [%zb] [%tb]\n", (unsigned char)300, (unsigned short)70000,
           (unsigned long)6, (unsigned long long)9, (uintmax_t)10, (size_t)11, (ptrdiff_t)12);
    printf("[%w8d] [%w16i] [%w32u] [%w64x] [%w8b] [%w64X] [%wf8d] [%wf16b] [%wf32i] [%wf64u] [%wf16x]\n",
           (int8_t)-5, (int16_t)-300, (uint32_t)4000000000u, (uint64_t)0xDEADBEEFCAFEull, (uint8_t)6,
           (uint64_t)255, (int_fast8_t)-7, (uint_fast16_t)9, (int_fast32_t)-70000, (uint_fast64_t)123456789012ull,
           (uint_fast16_t)0xABC);

    /* 3. %n of every length — the pe engine performs it (the UCRT's own refuses). */
    signed char n1 = 0;
    short n2 = 0;
    int n3 = 0;
    long n4 = 0;
    long long n5 = 0;
    intmax_t n6 = 0;
    size_t n7 = 0;
    ptrdiff_t n8 = 0;
    int8_t n9 = 0;
    int_fast32_t n10 = 0;
    char nbuf[128];
    int const nr = snprintf(nbuf, sizeof nbuf, "a%hhnbb%hnccc%nd%lnee%llnf%jngg%znh%tn%bi%w8nj%wf32n", &n1, &n2, &n3,
                            &n4, &n5, &n6, &n7, &n8, 3u, &n9, &n10);
    printf("n: %d %d %d %ld %lld %jd %zu %td %d %d | %d %s\n", n1, n2, n3, n4, n5, n6, n7, n8, (int)n9, (int)n10, nr,
           nbuf);

    /* 4. snprintf truncation and the would-be length across a %b; sprintf/vsprintf/vsnprintf through helpers. */
    char small[5];
    int const w = snprintf(small, sizeof small, "%b|%d", 255u, 7);
    char big[64];
    int const s1 = sprintf(big, "<%B>", 6u);
    char big2[64];
    int const s2 = via_vsprintf(big2, "<%#b>", 6u);
    char big3[8];
    int const s3 = via_vsnprintf(big3, sizeof big3, "%b%b", 255u, 255u);
    int const s0 = snprintf(NULL, 0, "%b", 1023u);
    printf("trunc: %d [%s] %d [%s] %d [%s] %d [%s] %d\n", w, small, s1, big, s2, big2, s3, big3, s0);

    /* 5. a mixed format: the platform's own conversions beside the engine's, byte-identical to the platform's. */
    char mixed[256];
    char ref[256];
    long double const ld = 2.5L;
    void *const ptr = &n3;
    wchar_t const wide[] = L"wide";
    snprintf(mixed, sizeof mixed, "%s %5.2f %b %c %% %Lf %ls %jd %zd %p %a", "str", 3.14159, 9u, 'Z', ld, wide,
             (intmax_t)-42, (size_t)77, ptr, 1.0);
    int const refHead = snprintf(ref, sizeof ref, "%s %5.2f ", "str", 3.14159);
    snprintf(ref + refHead, sizeof ref - (size_t)refHead, "1001 %c %% %Lf %ls %jd %zd %p %a", 'Z', ld, wide,
             (intmax_t)-42, (size_t)77, ptr, 1.0);
    check(strcmp(mixed, ref) == 0, "mixed format matches the platform's own rendering");
    printf("mixed: %s\n", strcmp(mixed, ref) == 0 ? "same" : mixed);

    /* 6. a stream sink: one call, several pieces. */
    FILE *tf = tmpfile();
    if (tf != NULL) {
        int const fw = via_vfprintf(tf, "%s=%#b;%d\n", "k", 5u, 42);
        rewind(tf);
        char line[32] = "";
        if (fgets(line, sizeof line, tf) == NULL) line[0] = '\0';
        size_t const len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        printf("stream: %d [%s]\n", fw, line);
        fclose(tf);
    }

    /* 7. scanf's %b, and %i over the 0b / 0x / 0 / decimal forms, with widths and %n. */
    unsigned b1 = 0;
    int i1 = 0, i2 = 0, i3 = 0, i4 = 0, i5 = 0, cnt = 0;
    int const r1 = sscanf("101 0b111 0x1F 017 -0b11 42", "%b %i %i %i %i %i%n", &b1, &i1, &i2, &i3, &i4, &i5, &cnt);
    printf("scan: %d %u %d %d %d %d %d %d\n", r1, b1, i1, i2, i3, i4, i5, cnt);
    unsigned b2 = 0, b3 = 0;
    int32_t w32 = 0;
    int_fast16_t wf16 = 0;
    int const r2 = via_vsscanf("0B1010 11 -123 77", "%b %2b %w32d %wf16d", &b2, &b3, &w32, &wf16);
    printf("scan2: %d %u %u %d %d\n", r2, b2, b3, (int)w32, (int)wf16);
    int z = -1, zc = -1;
    int const r3 = sscanf("0b", "%i%n", &z, &zc);
    printf("scan3: %d %d %d\n", r3, z, zc);

    /* 8. fscanf from a stream. */
    FILE *sf = tmpfile();
    if (sf != NULL) {
        fputs("0b1101 11 0b0", sf);
        rewind(sf);
        int f1 = 0, f3 = 0;
        unsigned f2 = 0;
        int const fr = fscanf(sf, "%i %b %i", &f1, &f2, &f3);
        printf("fscan: %d %d %u %d\n", fr, f1, f2, f3);
        fclose(sf);
    }

    /* 9. the strto family's 0b subject: prefix, base 2, base 0, the prefix alone, overflow, whitespace and sign. */
    char *end = NULL;
    long const t1 = strtol("0b101", &end, 0);
    int const e1 = (int)(end - (char const *)"0b101");
    (void)e1;
    long const t2 = strtol("  -0B11xyz", &end, 2);
    char const *const alone = "0b";
    long const t3 = strtol(alone, &end, 0);
    int const aloneAt = (int)(end - alone);
    errno = 0;
    long long const t4 = strtoll("0b1111111111111111111111111111111111111111111111111111111111111111", &end, 0);
    int const e4 = errno;
    unsigned long const t5 = strtoul("-0b1", &end, 2);
    unsigned long long const t6 = strtoull("0b100", &end, 16);
    intmax_t const t7 = strtoimax("0b1001", &end, 0);
    uintmax_t const t8 = strtoumax("+0b11", &end, 2);
    wchar_t *wend = NULL;
    intmax_t const t9 = wcstoimax(L" -0b101", &wend, 0);
    uintmax_t const t10 = wcstoumax(L"0b1z", &wend, 0);
    printf("strto: %ld %ld %ld@%d %d %d %d %llu %jd %ju %jd %ju\n", t1, t2, t3, aloneAt, t4 == LLONG_MAX, e4 == ERANGE,
           t5 == ULONG_MAX, t6, t7, t8, t9, t10);

    /* 10. <inttypes.h>'s binary macros, round trip. */
    char pb[64];
    uint32_t const v32 = 0xA5u;
    snprintf(pb, sizeof pb, "%" PRIb32 "|%" PRIB8 "|%#" PRIbMAX, v32, (uint8_t)3, (uintmax_t)2);
    uint32_t back = 0;
    int const rb = sscanf(pb, "%" SCNb32, &back);
    printf("pri: [%s] %d %u\n", pb, rb, (unsigned)back);

    /* 11. positional arguments beside C23 conversions — an argument used twice, a positional `*` width (negative:
     *     left-justified). glibc and libSystem have positional printf; the UCRT's printf has none (📄 MSVC: only the
     *     _printf_p family), so on Windows the cell prints the same line from a sequential format. */
#if defined(_WIN32)
    printf("pos: [%b %d %#B] [%*b|]\n", 5u, 7, 6u, -5, 6u);
#else
    printf("pos: [%2$b %1$d %3$#B] [%3$*4$b|]\n", 7, 5u, 6u, -5);
#endif

    /* 12. specifications no fixed buffer bounds, and the vendor 64-bit length on %n (P69 round 4): a hundred repeated
     *     flags (C's flags are a SET: the same as one of each, on every reference), a hundred-flag UNKNOWN
     *     specification (compared with the platform's own reading of it, never printed — glibc, the UCRT and libSystem
     *     print it three ways), a `*` width of INT_MIN (C: `-` and a width of 2^31, a count past INT_MAX — every
     *     reference fails the call with -1, glibc and libSystem with EOVERFLOW), a 208-character scanset, and %n
     *     through the platform's own 64-bit length (`q` on glibc and libSystem, `I64` on the UCRT) storing all eight
     *     bytes, in printf and in scanf. */
#define DASH10 "----------"
#define DASH100 DASH10 DASH10 DASH10 DASH10 DASH10 DASH10 DASH10 DASH10 DASH10 DASH10
#define PLUS10 "++++++++++"
#define PLUS100 PLUS10 PLUS10 PLUS10 PLUS10 PLUS10 PLUS10 PLUS10 PLUS10 PLUS10 PLUS10
#define AZ26 "abcdefghijklmnopqrstuvwxyz"
    char lf[96];
    int const lfn = snprintf(lf, sizeof lf, "%b|%" DASH100 "5d|%" PLUS100 "d|%" DASH100 "#5b|", 5u, 7, 9, 2u);
    char uk[96];
    char ukRef[96];
    int const ukn = snprintf(uk, sizeof uk, "%b[%" DASH100 "y]", 5u);
    int const ukRefN = snprintf(ukRef, sizeof ukRef, "[%" DASH100 "y]");
    int const ukSame = ukn == ukRefN + 3 && strncmp(uk, "101", 3) == 0 && strcmp(uk + 3, ukRef) == 0;
    char imBuf[64];
    errno = 0;
    int const im = snprintf(imBuf, sizeof imBuf, "%b|%*b|", 1u, INT_MIN, 5u);
    int const imOverflow = errno == EOVERFLOW;
    unsigned sb = 0;
    char word[64] = "";
    int sn = -1;
    int const sr = sscanf("0b11 hello world", "%b %63[" AZ26 AZ26 AZ26 AZ26 AZ26 AZ26 AZ26 AZ26 "]%n", &sb, word, &sn);
    long long qn = 0x5555555555555555LL;
    long long qs = 0x5555555555555555LL;
    unsigned qv = 0;
#if defined(_WIN32)
    snprintf(nbuf, sizeof nbuf, "%babc%I64n", 1u, &qn);
    int const qr = sscanf("0b1 x", "%b x%I64n", &qv, &qs);
#else
    snprintf(nbuf, sizeof nbuf, "%babc%qn", 1u, &qn);
    int const qr = sscanf("0b1 x", "%b x%qn", &qv, &qs);
#endif
    printf("long: %d [%s] %s | %d %d | %d %u [%s] %d | %lld %d %lld\n", lfn, lf, ukSame ? "same" : uk, im, imOverflow,
           sr, sb, word, sn, qn, qr, qs);
#if defined(_WIN32)
    /* The UCRT's `I` alone sizes ONLY an integer conversion (📄 its parser): printf reads `%In` as `%n` — an int
     * store, the guard beside it untouched — and scanf reads `%In` as its conversion `I` (= `i`) and then the literal
     * `n` (✔MEASURED, MSVC 19.51 over the UCRT). */
    struct { int k; int guard; } printedI = {-1, -1}, scannedI = {-1, -1};
    unsigned su = 0;
    snprintf(nbuf, sizeof nbuf, "%babc%In", 1u, &printedI.k);
    int const sir = sscanf("0b1 7n", "%b %In", &su, &scannedI.k);
    printf("ucrt I: %d %d | %d %u %d %d\n", printedI.k, printedI.guard, sir, su, scannedI.k, scannedI.guard);
#endif

    /* 13. a `%n` that is the ONLY specification its platform lacks (P69 round 4). Every `%n` above stands beside a
     *     `%b`, and one `%b` already hands the whole call to DSS's engine whatever the platform says of `%n` — so
     *     none of them asks the question this call asks: with nothing else of C23 in the format, the call reaches the
     *     UCRT, which refuses `%n`, unless the engine takes it for that alone. glibc and libSystem perform it
     *     themselves, from a format that is a string literal. */
    int lone = -1;
    char loneBuf[16];
    int const loneR = snprintf(loneBuf, sizeof loneBuf, "abc%nde", &lone);
    printf("lone n: %d %d [%s]\n", loneR, lone, loneBuf);

    printf("c23 formatted io: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 42 : 1;
}
