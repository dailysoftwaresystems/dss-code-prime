/*
 * TU 1 of the MULTI-CU pe64 `sprintf` shim witness
 * (D-FFI-PE-CRT-UCRT-MIGRATION, Phase 3).
 *
 * WHY A SECOND, MULTI-CU WITNESS EXISTS ALONGSIDE `shipped_sprintf_ucrt`:
 * the two drivers place the shim synthesis at DIFFERENT points in the
 * pipeline, and only one of them is what the real consumer uses.
 *
 *   * single-CU (`compile_pipeline.cpp`) synthesizes the shim POST-optimize,
 *     inside `lowerCuMirToAssembly` -- the optimizer NEVER SEES the shim
 *     body. `shipped_sprintf_ucrt` (one source file) exercises only this.
 *   * multi-CU (`program.cpp`) reconstructs the recipe map from the MERGED
 *     symbol names and synthesizes PRE-optimize, so `optimizeModule`'s full
 *     release pipeline -- Inlining, ConstFold, Mem2Reg, CopyProp, Cse, Licm,
 *     SimplifyCfg, Dce -- then runs OVER the synthesized shim body.
 *
 * The real consumer is the sqlite testfixture, a multi-CU `--project` build.
 * So the pe64 `sprintf` fix actually SHIPS through the second seam, which had
 * no runtime witness at all. This example is that witness.
 *
 * ★ THE SPECIFIC THING ONLY THIS EXAMPLE CAN CATCH. The shim body is a single
 * block of roughly ten instructions -- comfortably under the shipped
 * `release.pipeline.json` `inlineThreshold` of 50 -- and after the merge the
 * inliner RESOLVES `sprintf` to it (a defined function in the same module, no
 * longer an extern). Nothing but one line in `src/opt/passes/inlining.cpp`
 * stops it from being spliced into its caller: the refusal of any callee
 * containing a `VaHomeArgAreaAddr` (or `VaRegSaveAreaAddr` /
 * `VaOverflowArgAreaAddr`) leaf. Those leaves lower to `lea reg, [sp+off]`
 * against the CALLEE's OWN frame -- only the callee's variadic prologue
 * spills the home area -- so a spliced shim would compute `ap` from the
 * CALLER's frame, which never spilled anything, and read GARBAGE. That
 * failure is silent: it compiles, it links, it runs, and it prints wrong
 * text. Under the single-CU seam the shim is created after the optimizer has
 * already finished, so no single-source example can reach this at all.
 *
 * The wrappers live in fmt.c precisely so they are inlined INTO main first,
 * putting the sprintf call site in main's body where the shim-inline refusal
 * is then actually consulted.
 *
 * elf/macho keep importing the real libc `sprintf` unchanged (stdio.json's
 * split is [elf,macho] plain-FFI vs [pe] synthesize), so running the same two
 * sources on all four hosted targets is simultaneously a cross-target
 * agreement check: every target must produce IDENTICAL strings.
 *
 * exit 42 on success; a distinct code per check.
 */
#include <stdio.h>
#include <string.h>

/* fmt.c's operands, bound across the CU boundary by the whole-program merge. */
extern int          g_pos;
extern int          g_neg;
extern unsigned int g_hex;
extern long long    g_big;
extern double       g_d2;
extern double       g_d3;

/* fmt.c's sprintf-calling wrappers -- inline candidates (see fmt.c). */
extern int fmt_int(char *buf);
extern int fmt_dbl(char *buf);
extern int fmt_mixed(char *buf);
extern int fmt_star(char *buf);

int main(void) {
    char buf[64];
    int  n;

    /* ── Part 1: sprintf reached through the OTHER TU's wrappers ────────── */

    /* 1. an int vararg, formatted in fmt.c, checked here. */
    n = fmt_int(buf);
    if (strcmp(buf, "12345") != 0) return 50;
    /* the return value must be the real character count, not garbage. */
    if (n != 5) return 51;

    /* 2. THE FP PROOF: a lone double vararg. No integer check can catch a
     *    lost xmm->home-slot spill. */
    if (fmt_dbl(buf) < 0) return 52;
    if (strcmp(buf, "3.50") != 0) return 53;

    /* 3. int and double varargs INTERLEAVED -- the positional-ordering
     *    case. */
    if (fmt_mixed(buf) < 0) return 54;
    if (strcmp(buf, "12345|1.5|-42|2.5") != 0) return 55;

    /* 4. the sqlite shellDtostr() shape: a star-precision int vararg
     *    consumed BEFORE a double, and a two-digit exponent (UCRT/C99, not
     *    msvcrt's three). */
    if (fmt_star(buf) < 0) return 56;
    if (strcmp(buf, "+3.500e+00") != 0) return 57;

    /* ── Part 2: sprintf called DIRECTLY from THIS TU on the OTHER TU's
     *    data. Both translation units must bind to the SAME merged shim
     *    definition -- a per-CU shim would give two, and a shim reachable
     *    from only one TU would leave the other's call undefined. ───────── */

    /* 5. a 64-bit long long past INT32_MAX. */
    sprintf(buf, "%lld", g_big);
    if (strcmp(buf, "9223372036854775807") != 0) return 58;

    /* 6. unsigned hex, lowercase. */
    sprintf(buf, "%x", g_hex);
    if (strcmp(buf, "deadbeef") != 0) return 59;

    /* 7. a lone double vararg from THIS TU's own call site. */
    sprintf(buf, "%.1f", g_d2);
    if (strcmp(buf, "1.5") != 0) return 60;

    /* 8. int/double INTERLEAVED from THIS TU's own call site, in the
     *    opposite order to check 3 (double first, then int, then double). */
    sprintf(buf, "%d/%.3f/%d", g_neg, g_d3, g_pos);
    if (strcmp(buf, "-42/2.500/12345") != 0) return 61;

    /* ── Part 3: the merge is ONE storage, and the format really runs at
     *    RUNTIME. Write a global from this TU, then re-read it through the
     *    OTHER TU's wrapper. Two per-CU copies would still print 12345, and
     *    a constant-folded format string would too. ─────────────────────── */
    g_pos = 67900;
    n = fmt_int(buf);
    if (strcmp(buf, "67900") != 0) return 62;
    if (n != 5) return 63;

    return 42;
}
