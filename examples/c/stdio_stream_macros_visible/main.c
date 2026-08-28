/* D-CONFIG-STDIO-STREAM-MACROS-INVISIBLE-ON-ELF: `stdin`, `stdout` and `stderr`
   must be MACROS, on every object format.

   ISO C 7.23.1 lists all three among the macros a conforming <stdio.h> defines,
   and both ELF references agree: glibc writes the self-referential
   `#define stdin stdin` for exactly this reason, so `#ifdef stdin` answers TRUE
   under gcc and clang. ✔MEASURED 2026-08-27 (gcc 13.3.0 -std=c2x, clang 18.1.3
   -std=c23, with a control #error that fired on both): rc=0 for all three names.

   ⚠ DSS ANSWERED FALSE ON ELF ONLY. `src/dss-config/shippedLibs/stdio.json`
   declared the three macros with per-format `variants` for `pe` and `macho`
   alone; on ELF they existed solely as `symbols`, so the names were usable in
   CODE and invisible to `#ifdef`. ✔MEASURED before the fix, same binary, same
   source: elf rc=1 with three `P001E` #error hits, pe rc=0, macho rc=0. The
   variant selector has no fallback arm by design — zero matching variants means
   the macro is simply not injected, never a wrong replacement — so the fix is a
   third `elf` variant, not a loader change.

   ★ WHY THIS EXAMPLE ASSERTS A RUNTIME VALUE RATHER THAN COMPILING AN #error.
   An `#error` proves the macro is missing by FAILING THE BUILD, which a green
   corpus cannot express — the example would have to be expected-to-fail, and an
   expected-failure witnesses the diagnostic, not the feature. Folding each
   `#ifdef` into the EXIT CODE makes the property a positive, runnable fact on
   every leg: lose any one of the three macros and this example returns 38, 34
   or 26 instead of 42, on the exact format that lost it.

   ★ AND IT ASSERTS THE EXPANSION STILL WORKS, which the `#ifdef` alone cannot.
   On ELF the replacement is self-referential (`stdout` -> `stdout`), so the
   preprocessor's hideset is what stops it recursing; on PE it is a CALL
   (`(__acrt_iob_func(1))`) and on Mach-O a RENAME (`__stdoutp`). The `fputs`
   below runs the expansion on whichever of those three this leg took, and its
   bytes are asserted exactly.

   RED-ON-DISABLE: drop the `elf` variant from any of the three macros in
   stdio.json -> that name's `#ifdef` goes false -> this example returns 38 / 34
   / 26 on both ELF arms while pe and macho stay 42, so the arm that lost the
   fact is the arm that reds. */
#include <stdio.h>

#ifdef stdin
#define DSS_STDIN_IS_A_MACRO 1
#else
#define DSS_STDIN_IS_A_MACRO 0
#endif

#ifdef stdout
#define DSS_STDOUT_IS_A_MACRO 1
#else
#define DSS_STDOUT_IS_A_MACRO 0
#endif

#ifdef stderr
#define DSS_STDERR_IS_A_MACRO 1
#else
#define DSS_STDERR_IS_A_MACRO 0
#endif

/* Distinct weights, so a failure names WHICH stream lost its macro rather than
   just "one of them did". */
static int macroScore(void) {
    return DSS_STDIN_IS_A_MACRO * 4
         + DSS_STDOUT_IS_A_MACRO * 8
         + DSS_STDERR_IS_A_MACRO * 16;
}

int main(void) {
    /* The EXPANSION, on whichever form this format declares. A macro that is
       visible to `#ifdef` but expands to something unusable would pass the
       score check and fail here. */
    if (fputs("streammacro:ok\n", stdout) < 0) return 1;
    if (fflush(stdout) != 0) return 2;
    return macroScore() + 14;   /* 4 + 8 + 16 + 14 = 42 */
}
