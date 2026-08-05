/*
 * Runtime witness for `snprintf` on all three legs -- and the FIRST witness in the
 * tree that can pin the UCRT `_Options` bit at all.
 *
 * WHAT IS UNDER TEST. stdio.json ships `snprintf` two ways: elf imports the real
 * libc symbol, while pe64 has NO symbol to import (MEASURED: `objdump -p
 * C:/Windows/System32/ucrtbase.dll` exports neither `snprintf` nor
 * `__stdio_common_vsnprintf`; msvcrt.dll does not export bare `snprintf` either),
 * so its body is COMPILER-SYNTHESIZED by src/mir/merge/synth_stdio_shim.cpp over
 * the SAME `__stdio_common_vsprintf` core `sprintf` already uses, differing in
 * exactly two arguments:
 *     _Options     = STANDARD_SNPRINTF_BEHAVIOR (bit 1), not sprintf's legacy bit 0
 *     _BufferCount = the caller's REAL n, not the (size_t)-1 unbounded sentinel
 * followed by the header's `r < 0 ? -1 : r` clamp.
 *
 * WHY THERE IS NO DARWIN ARM IN expected.json. stdio.json gates the `snprintf` import
 * row to `["elf"]`: that libSystem exports it is INFERRED, not MEASURED (no Mac and no
 * macOS SDK were reachable when this landed), and under the eager-import law a wrong
 * guess breaks EVERY macho binary's LOAD -- not just the ones calling snprintf. So it
 * follows the same staging stdio.json applies to `popen`/`pclose`/`fileno`: macho fails
 * loud S0001 until a real run witnesses the export. The macho descriptor row and the
 * darwin arm here land TOGETHER, in one edit, after the operator's Mac run. See the
 * `$comment` in expected.json and stdio.json's TF-C119 clause.
 *
 * WHY THE OLDER sprintf WITNESS COULD NOT DO THIS JOB. shipped_sprintf_ucrt says so
 * itself: with `_BufferCount = (size_t)-1` no truncation is reachable, so _Options
 * 0, 1 and 2 produce byte-identical output AND an identical return -- a wrong bit is
 * INVISIBLE there. It named the fix: "pinning _Options properly needs a
 * bounded-buffer consumer (snprintf), not yet shipped." This is that consumer.
 *
 * THE PINS ARE CHOSEN AS THE EXACT POINTS WHERE THE CANDIDATE BEHAVIOURS DIVERGE,
 * because every one of those behaviours is silent -- none errors, none fails to
 * link. TWO INDEPENDENT WRONG-ARGUMENT FAILURES ARE COVERED, and it is worth being
 * precise about which check catches which, since they are NOT the same set:
 *   (a) `_Options` bit cleared -> the pre-C99 `_snprintf`, which returns -1 on
 *       truncation instead of the would-be length. MEASURED red: checks 1 and 7
 *       fire; the example exits 50. (Legacy `_snprintf` is ALSO documented to leave
 *       the buffer unterminated -- but MEASURED, the ucrtbase build under test still
 *       wrote the NUL, so NUL placement is not this bit's discriminator and is not
 *       claimed as one here.)
 *   (b) `_BufferCount` replaced by the `(size_t)-1` unbounded sentinel -> the core
 *       writes the WHOLE result past the caller's buffer. MEASURED red: checks 2
 *       and 3 fire; the example exits 51. This is the more dangerous of the two,
 *       because it corrupts memory rather than text, and NOTHING in checks 1/4/5/7
 *       can see it.
 *
 * ANTI-FOLD, AND WHY MUTABLE GLOBALS ALONE WERE NOT CONSIDERED ENOUGH HERE. Every
 * operand and every buffer size is read from a MUTABLE GLOBAL, and several are
 * additionally DERIVED AT RUNTIME from a previous snprintf's return value -- a value
 * no pass can know without modelling the C runtime. So the `release`
 * shippedPipeline's constant folding cannot precompute a single one of these
 * strings and quietly reduce this example to a no-op witness.
 *
 * THE THREE-NAMED-ARG PIN, which is specific to THIS recipe. Every previously
 * shipped stdio shim has at most two named parameters before the `...`; snprintf has
 * THREE (buf, n, fmt), so the synthesized body must derive `ap` at home-slot index 3.
 * An off-by-one there reads the format string or the count as the first vararg:
 * every `%d`/`%s` check below renders garbage and fails. Check 8 extends that to an
 * FP vararg, whose xmm value must ALSO be spilled to its own positional home slot.
 *
 * exit = 42.
 */
#include <stdio.h>
#include <string.h>

char buf[32];

/* Operands -- all mutable globals, never literal arguments to snprintf. */
int          g_val   = 12345;              /* renders as 5 chars */
char         g_text[] = "abcdefghij";      /* renders as 10 chars */
double       g_d     = 2.5;                /* exactly representable, no rounding tie */
unsigned int g_cap4  = 4;                  /* a truncating capacity */
unsigned int g_cap6  = 6;                  /* the exact-fit capacity for "12345" */
unsigned int g_cap5  = 5;                  /* one BYTE short of an exact fit */
unsigned int g_cap0  = 0;                  /* the "write nothing" capacity */
unsigned int g_capBig = 32;                /* the whole buffer */
char         g_fill  = 'Z';                /* the untouched-memory sentinel */

static void fill(void) {
    int i;
    for (i = 0; i < 32; i++) buf[i] = g_fill;
}

int main(void) {
    int r, r2, r3;

    /* 1. TRUNCATION RETURN -- the headline C99 rule and the primary _Options pin.
     *    "12345" is 5 chars but only 4 bytes are offered, so the return must be the
     *    length the output WOULD have had (5), NOT the 3 characters actually stored
     *    and NOT the -1 the legacy `_snprintf` returns. This single check is what
     *    the sprintf witness structurally could not perform. */
    fill();
    r = snprintf(buf, g_cap4, "%d", g_val);
    if (r != 5) return 50;

    /* 2. TRUNCATION NUL-TERMINATES WITHIN n. C99 requires the last of the n bytes to
     *    be the NUL, so exactly n-1 = 3 characters survive. Pinned as a byte value
     *    and not by strcmp alone, because an unterminated buffer would make the
     *    strcmp itself run off the end rather than report a difference. MEASURED
     *    red-on-disable: this is the check that fires (exit 51) when the shim
     *    forwards the unbounded sentinel instead of the real n -- the core then
     *    writes "12345\0" and buf[3] is '4'. */
    if (buf[3] != '\0') return 51;
    if (strcmp(buf, "123") != 0) return 52;

    /* 3. TRUNCATION DOES NOT WRITE PAST n. If the shim ever forwarded the
     *    `(size_t)-1` unbounded sentinel instead of the caller's real n, the core
     *    would happily write all 5 characters plus a NUL -- overrunning the caller's
     *    buffer. Nothing above would notice: check 1 would still see 5 and check 2
     *    would still find a NUL at [3] if the overrun happened to land there. Only
     *    the sentinel byte just past the promised region catches it. */
    if (buf[4] != g_fill) return 53;

    /* 4. EXACT FIT. n is exactly strlen+1, so the whole string plus its NUL fit and
     *    nothing is truncated -- the return is the same 5, distinguishing "returns
     *    the would-be length" from "returns the truncated length". */
    fill();
    r = snprintf(buf, g_cap6, "%d", g_val);
    if (r != 5) return 54;
    if (strcmp(buf, "12345") != 0) return 55;
    if (buf[6] != g_fill) return 56;

    /* 5. n == 0 WITH A NULL BUFFER -- the standard buffer-sizing idiom, and the
     *    check that proves the real n reaches the core rather than the sentinel.
     *    The call must write nothing at all (a NULL destination is legal only
     *    BECAUSE n is 0) and still report the would-be length. */
    r = snprintf((char *)0, g_cap0, "%s", g_text);
    if (r != 10) return 57;

    /* 6. n == 0 WITH A NON-NULL BUFFER -- the buffer must be left completely
     *    untouched, not even NUL-terminated (C99: "nothing is written"). A shim that
     *    "helpfully" wrote a NUL would corrupt a caller who is only measuring. */
    fill();
    r = snprintf(buf, g_cap0, "%d", g_val);
    if (r != 5) return 58;
    if (buf[0] != g_fill) return 59;

    /* 7. THE OFF-BY-ONE BOUNDARY: n == strlen exactly, so precisely one character is
     *    lost to the mandatory NUL. Separates a correct implementation from one that
     *    is off by a single byte in either direction, which checks 1 and 4 straddle
     *    without pinning. */
    fill();
    r = snprintf(buf, g_cap5, "%d", g_val);
    if (r != 5) return 60;
    if (strcmp(buf, "1234") != 0) return 61;
    if (buf[5] != g_fill) return 62;

    /* 8. VARIADIC MARSHALLING ACROSS THREE NAMED ARGS, including the FP case.
     *    snprintf's `ap` starts one home slot later than sprintf's, and an FP vararg
     *    must additionally be spilled from its xmm register into that positional
     *    slot -- a lost or mis-indexed spill is invisible to every integer check
     *    above. A mixed conversion also proves several varargs are consumed in
     *    order, not just the first. */
    fill();
    r = snprintf(buf, g_capBig, "%s|%d|%.1f", g_text, g_val, g_d);
    if (strcmp(buf, "abcdefghij|12345|2.5") != 0) return 63;
    if (r != (int)strlen(buf)) return 64;

    /* 9. RUNTIME-DERIVED OPERANDS. `r` is the return of a real snprintf call, so no
     *    pass can know it without modelling the CRT; it is fed back BOTH as a
     *    formatted value and as the capacity of the next call. This is the check
     *    that makes the `release` arm's constant folding structurally unable to
     *    precompute this example's strings. r == 20 here. */
    fill();
    r2 = snprintf(buf, g_capBig, "%d", r);
    if (strcmp(buf, "20") != 0) return 65;
    if (r2 != 2) return 66;

    /*    ...and now as a bounded, TRUNCATING capacity derived from that: n = 2 means
     *    one character plus the NUL, while the would-be length is still 10. */
    fill();
    r3 = snprintf(buf, (unsigned int)r2, "%s", g_text);
    if (r3 != 10) return 67;
    if (strcmp(buf, "a") != 0) return 68;
    if (buf[2] != g_fill) return 69;

    /* The stdout assertion. Printing the MEASURED numbers (rather than a fixed
     * banner) means the harness's byte-exact stdout comparison is itself a pin: a
     * wrong return value shows up in the diff even if a future edit weakens a
     * check above. All three legs must produce identical text. */
    printf("mix=%d|%s\n", r, buf);
    printf("would=%d back=%d\n", r3, r2);
    return 42;
}
