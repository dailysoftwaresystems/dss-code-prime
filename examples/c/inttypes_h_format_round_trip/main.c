/* <inttypes.h> (C 7.8) end to end on every pair: every PRI width formats its own type through printf-family
 * calls, the SCN macro of the same type reads it back, and imaxabs, imaxdiv, strtoimax, strtoumax, wcstoimax and
 * wcstoumax run against the platform's C library. Each PRI/SCN macro expands to the `type-format` predefine of its
 * typedef, so a wrong length modifier is a wrong VALUE read back here — or a crash — not a compile error. */
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int ok, char const *what) {
    if (!ok) {
        ++failures;
        printf("FAIL %s\n", what);
    }
}

#define ROUND_TRIP(T, PRI, SCN, VALUE)                                                                    \
    do {                                                                                                  \
        T const v = (VALUE);                                                                              \
        T back = 0;                                                                                       \
        char buf[64];                                                                                     \
        snprintf(buf, sizeof buf, "%" PRI, v);                                                            \
        check(sscanf(buf, "%" SCN, &back) == 1 && back == v, #PRI "/" #SCN);                              \
    } while (0)

int main(void) {
    ROUND_TRIP(int8_t, PRId8, SCNd8, -100);
    ROUND_TRIP(int16_t, PRIi16, SCNi16, -30000);
    ROUND_TRIP(int32_t, PRId32, SCNd32, -2000000000);
    ROUND_TRIP(int64_t, PRId64, SCNd64, -9000000000000000000);
    ROUND_TRIP(uint8_t, PRIu8, SCNu8, 250);
    ROUND_TRIP(uint16_t, PRIx16, SCNx16, 0xBEEF);
    ROUND_TRIP(uint32_t, PRIo32, SCNo32, 037777777777u);
    ROUND_TRIP(uint64_t, PRIx64, SCNx64, 0xFEDCBA9876543210u);
    ROUND_TRIP(int_least8_t, PRIdLEAST8, SCNdLEAST8, -7);
    ROUND_TRIP(int_least64_t, PRIdLEAST64, SCNdLEAST64, 123456789012345);
    ROUND_TRIP(uint_least16_t, PRIuLEAST16, SCNuLEAST16, 65535);
    ROUND_TRIP(int_fast16_t, PRIdFAST16, SCNdFAST16, -12345);
    ROUND_TRIP(int_fast32_t, PRIiFAST32, SCNiFAST32, 2000000000);
    ROUND_TRIP(uint_fast64_t, PRIuFAST64, SCNuFAST64, 18000000000000000000u);
    ROUND_TRIP(intmax_t, PRIdMAX, SCNdMAX, -1234567890123456789);
    ROUND_TRIP(uintmax_t, PRIxMAX, SCNxMAX, 0x0123456789ABCDEFu);
    ROUND_TRIP(intptr_t, PRIdPTR, SCNdPTR, -4096);
    ROUND_TRIP(uintptr_t, PRIXPTR, SCNxPTR, 0xFFFF0000u);

    char upper[32];
    snprintf(upper, sizeof upper, "%" PRIX64, (uint64_t)0xabcdefu);
    check(strcmp(upper, "ABCDEF") == 0, "PRIX64 upper case");

    imaxdiv_t const q = imaxdiv(-7, 2);
    check(q.quot == -3 && q.rem == -1, "imaxdiv");
    check(imaxabs(-42) == 42, "imaxabs");

    char *end = NULL;
    intmax_t const s = strtoimax("  -9223372036854775807x", &end, 10);
    check(s == -INTMAX_MAX && *end == 'x', "strtoimax");
    uintmax_t const u = strtoumax("0x10", &end, 0);
    check(u == 16 && *end == '\0', "strtoumax");
    wchar_t *wend = NULL;
    intmax_t const ws = wcstoimax(L"-77 rest", &wend, 10);
    check(ws == -77 && *wend == L' ', "wcstoimax");
    uintmax_t const wu = wcstoumax(L"ff", &wend, 16);
    check(wu == 255 && *wend == L'\0', "wcstoumax");

    printf("inttypes: %s, sizeof(imaxdiv_t)=%u\n", failures == 0 ? "all round trips ok" : "FAILED",
           (unsigned)sizeof(imaxdiv_t));
    return failures == 0 ? 42 : 1;
}
