/*
 * TU 2 of the MULTI-CU pe64 `sprintf` shim witness
 * (D-FFI-PE-CRT-UCRT-MIGRATION, Phase 3).
 *
 * This TU owns every formatted OPERAND (as a mutable global) and every small
 * `sprintf`-calling WRAPPER; main.c consumes both across the CU boundary. That
 * split is what forces the whole-program merge: main.c's references to
 * `g_pos` / `fmt_int` are unresolved externs per-CU and only bind after
 * mir_merge folds both TUs into one module.
 *
 * WHY THE WRAPPERS ARE TINY: each is a single `return sprintf(...)`, far under
 * the shipped release pipeline's `inlineThreshold` (50), so the merged-module
 * Inlining pass WILL splice each one into main. That is deliberate — it puts
 * the `sprintf` call site INSIDE main's body, where the inliner then gets a
 * second chance to inline the callee it now resolves: the SYNTHESIZED SHIM
 * itself. The shim must NOT be inlined (see main.c's header comment), so this
 * shape drives `inlining.cpp`'s `VaHomeArgAreaAddr` callee refusal at exactly
 * the moment it matters.
 *
 * ANTI-FOLD: every operand is a MUTABLE global (never `const`, never a
 * literal argument), so the `release` arm's ConstFold cannot precompute a
 * formatted string and quietly turn the witness into a no-op. main.c also
 * WRITES `g_pos` at runtime and re-reads it through `fmt_int`, which pins the
 * merge as a genuine single-storage binding rather than two per-CU copies.
 */
#include <stdio.h>

int          g_pos = 12345;                    /* a plain positive int */
int          g_neg = -42;                      /* a negative int */
unsigned int g_hex = 0xDEADBEEFu;              /* unsigned hex */
long long    g_big = 9223372036854775807LL;    /* INT64_MAX -- the 64-bit proof */
int          g_prec = 3;                       /* star-precision vararg */

/* THE FP VARARGS. On Win64 a variadic call must ALSO spill each xmm-passed
 * double into its POSITIONAL home slot, because the callee reads varargs
 * through a va_list carrying no type information -- and the shim forwards
 * precisely that home area as `ap`. A lost or mis-indexed spill is invisible
 * to every integer-only check (integer varargs travel in the registers the
 * home area already mirrors). Values are exactly representable with no
 * rounding tie, so the expected text is identical under UCRT, glibc and
 * Darwin libc (a tie like %.1f of 2.25 would diverge: round-half-to-even vs
 * half-away-from-zero). */
double g_d1 = 3.5;
double g_d2 = 1.5;
double g_d3 = 2.5;

/* A cross-CU sprintf call site with an INT vararg. */
int fmt_int(char *buf) { return sprintf(buf, "%d", g_pos); }

/* A cross-CU sprintf call site with a lone DOUBLE vararg. */
int fmt_dbl(char *buf) { return sprintf(buf, "%.2f", g_d1); }

/* A cross-CU sprintf call site with INT and DOUBLE varargs INTERLEAVED --
 * the positional-ordering case a contiguous-spill bug still passes
 * `fmt_dbl` on. */
int fmt_mixed(char *buf) {
    return sprintf(buf, "%d|%.1f|%d|%.1f", g_pos, g_d2, g_neg, g_d3);
}

/* THE REAL CONSUMER'S SHAPE. sqlite's src/test1.c shellDtostr() -- the
 * function the fpconv1-2.0 test compares SQLite's own float-to-text against
 * -- calls exactly `sprintf(z, "%#+.*e", n, r)`: an int vararg (star
 * precision) consumed BEFORE a double. Two things are pinned at once.
 * (a) MARSHALLING: a shim that mis-orders the home slots renders the wrong
 *     precision -- the fpconv1 failure mode exactly.
 * (b) THE UCRT BINDING: legacy msvcrt renders %e with a THREE-digit exponent
 *     ("+3.500e+000") while UCRT is C-conformant at two ("+3.500e+00"), as
 *     are glibc and Darwin libc. So this strcmp fails loudly if the pe
 *     binding ever regresses to msvcrt, and it doubles as the cross-target
 *     agreement check. */
int fmt_star(char *buf) { return sprintf(buf, "%#+.*e", g_prec, g_d1); }
