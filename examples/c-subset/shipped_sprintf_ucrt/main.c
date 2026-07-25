/*
 * Runtime witness for the pe64 `sprintf` COMPILER-SYNTHESIZED shim
 * (D-FFI-PE-CRT-UCRT-MIGRATION, P3 first increment). stdio.json's pe
 * `sprintf` row now carries `synthesize: "sprintf"` instead of importing a
 * concrete CRT symbol, because the modern UCRT (`ucrtbase.dll`) exports NO
 * concrete `sprintf` at all -- only the common core `__stdio_common_vsprintf`,
 * which every printf-family entry point routes through. The synthesized body
 * is: the `VaHomeArgAreaAddr` va_start-equivalent -> one call to
 * `__stdio_common_vsprintf(_Options=1, buf, (size_t)-1, fmt, NULL, ap)` ->
 * return.
 *
 * WHY THIS EXISTS: the descriptor change alone proves nothing about whether
 * the synthesized body's variadic marshalling actually produces byte-correct
 * output at runtime -- the `_Options`/`_BufferCount` constants and the ms_x64
 * shadow-register dup for spilled FP/int varargs are exactly the kind of
 * detail that fails SILENTLY (wrong digit, wrong padding, truncated string)
 * rather than at link time. So every basic C value sprintf must marshal is
 * exercised here: a positive int, a negative int, a string, a char, unsigned
 * hex, a 64-bit `long long` past INT32_MAX, width/flag combinations
 * (zero-pad, '+', left-justify), and -- the case only an FP vararg can reach
 * -- a `double`, both alone and INTERLEAVED with integer varargs, each
 * compared byte-for-byte with strcmp
 * against the literal a correct libc sprintf produces. elf/macho keep
 * importing the real libc `sprintf` unchanged (the descriptor split is
 * [elf,macho] plain-FFI vs [pe] synthesize), so this example is also a
 * cross-target agreement check: all four hosted targets must produce
 * IDENTICAL strings for identical inputs.
 *
 * ANTI-FOLD: every formatted value lives in a MUTABLE GLOBAL, never a literal
 * argument to sprintf, so the `release` shippedPipeline's constant-folding
 * cannot precompute the formatted strings at compile time and silently turn
 * this into a no-op witness -- each call must still reach the real
 * synthesized shim (or real libc) at runtime.
 */
#include <stdio.h>
#include <string.h>

int          g_pos    = 12345;                  /* a plain positive int */
int          g_neg    = -42;                    /* a negative int */
char         g_ch     = 'Q';                    /* a char */
char         g_str[]  = "hello, sprintf";        /* a string */
unsigned int g_hex    = 0xDEADBEEFu;             /* unsigned hex */
long long    g_big    = 9223372036854775807LL;   /* INT64_MAX -- the 64-bit proof */
int          g_zero8  = 42;                      /* zero-padded width */
int          g_plus   = 7;                       /* '+' flag */
char         g_ljust[] = "ab";                   /* left-justified width */
/* THE FP VARARGS. On Win64 a variadic call must ALSO spill each xmm-passed
 * double into the corresponding home slot, because the callee reads them
 * through a va_list that carries no type information -- and this shim forwards
 * exactly that home area as `ap`. So a dropped dup is invisible to every
 * integer-only check above and shows up ONLY here. Values are chosen to be
 * exactly representable with no rounding tie, so the expected text is
 * identical under UCRT, glibc and Darwin libc (a tie like %.1f of 2.25 would
 * diverge: round-half-to-even vs half-away-from-zero). */
double       g_d1     = 3.5;
double       g_d2     = 1.5;
double       g_d3     = 2.5;
int          g_prec   = 3;                       /* star-precision vararg */

int main(void) {
    char buf[64];
    int  n;

    /* 1. a plain positive int */
    sprintf(buf, "%d", g_pos);
    if (strcmp(buf, "12345") != 0) return 50;

    /* 2. a negative int */
    sprintf(buf, "%d", g_neg);
    if (strcmp(buf, "-42") != 0) return 51;

    /* 3. a string */
    sprintf(buf, "%s", g_str);
    if (strcmp(buf, "hello, sprintf") != 0) return 52;

    /* 4. a char */
    sprintf(buf, "%c", g_ch);
    if (strcmp(buf, "Q") != 0) return 53;

    /* 5. unsigned hex, lowercase */
    sprintf(buf, "%x", g_hex);
    if (strcmp(buf, "deadbeef") != 0) return 54;

    /* 6. THE 64-BIT PROOF: a long long past INT32_MAX */
    sprintf(buf, "%lld", g_big);
    if (strcmp(buf, "9223372036854775807") != 0) return 55;

    /* 7. zero-padded width */
    sprintf(buf, "%08d", g_zero8);
    if (strcmp(buf, "00000042") != 0) return 56;

    /* 8. '+' flag on a positive value */
    sprintf(buf, "%+d", g_plus);
    if (strcmp(buf, "+7") != 0) return 57;

    /* 9. left-justified width -- padding must land on the correct side */
    sprintf(buf, "[%-5s]", g_ljust);
    if (strcmp(buf, "[ab   ]") != 0) return 58;

    /* 10. multiple mixed conversions in one call -- exercises the shim's
     *     va_list forwarding across more than one argument in sequence. */
    n = sprintf(buf, "%s=%d/%x", g_str, g_pos, g_hex);
    if (strcmp(buf, "hello, sprintf=12345/deadbeef") != 0) return 59;
    /* sprintf returns the number of characters written (excluding the NUL).
     * SCOPE, precisely: this proves the return value is the real count rather
     * than garbage or a dropped result. It does NOT prove the _Options bit is
     * right -- measured against the real ucrtbase core, _Options 0, 1
     * (LEGACY_VSPRINTF_NULL_TERMINATION) and 2 (STANDARD_SNPRINTF_BEHAVIOR)
     * all yield byte-identical output AND an identical return value once
     * _BufferCount is (size_t)-1, because no truncation is reachable. So a
     * wrong bit is INVISIBLE to every check here except 13, which catches a
     * LEGACY_THREE_DIGIT_EXPONENTS contamination via the exponent width.
     * Pinning _Options properly needs a bounded-buffer consumer (snprintf),
     * which is not shipped yet -- tracked in D-FFI-PE-CRT-UCRT-MIGRATION. */
    if (n != (int)strlen(buf)) return 60;

    /* 11. THE FP PROOF: a double vararg. Nothing above can catch a lost
     *     xmm->home-slot spill, because integer varargs travel in the integer
     *     registers the home area already mirrors. */
    sprintf(buf, "%.2f", g_d1);
    if (strcmp(buf, "3.50") != 0) return 61;

    /* 12. FP and integer varargs INTERLEAVED -- the ordering case. Each
     *     argument's home slot is positional, so a shim that spilled the
     *     doubles contiguously (rather than at their own argument indices)
     *     still passes check 11 and fails here. */
    sprintf(buf, "%d|%.1f|%d|%.1f", g_pos, g_d2, g_neg, g_d3);
    if (strcmp(buf, "12345|1.5|-42|2.5") != 0) return 62;

    /* 13. THE REAL CONSUMER'S SHAPE. sqlite's src/test1.c:1113 shellDtostr()
     *     -- the function fpconv1-2.0 compares SQLite's own float-to-text
     *     against -- calls exactly `sprintf(z, "%#+.*e", n, r)`: an int
     *     vararg (star precision) followed by a double. Two independent
     *     things are pinned here.
     *     (a) MARSHALLING: star-precision consumes an int vararg BEFORE the
     *         double, so a shim that mis-orders the home slots renders the
     *         wrong precision -- the fpconv1 failure mode exactly.
     *     (b) THE UCRT BINDING ITSELF: legacy msvcrt renders %e with a
     *         THREE-digit exponent ("+3.500e+000"), while UCRT is
     *         C-conformant at two ("+3.500e+00") -- as are glibc and Darwin.
     *         So this one strcmp fails loudly if the pe binding ever regresses
     *         to msvcrt, and it is simultaneously the cross-target agreement
     *         check: all four targets must produce the identical string. */
    sprintf(buf, "%#+.*e", g_prec, g_d1);
    if (strcmp(buf, "+3.500e+00") != 0) return 63;

    return 42;
}
