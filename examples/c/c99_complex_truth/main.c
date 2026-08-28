/* D-CSUBSET-COMPLEX — runtime witness for the COMPARISON half of C99 `_Complex`:
 * equality, truthiness, logical `!`, and the `_Bool` conversion. RUN on BOTH the
 * baseline (debug) and the `release` (shippedPipeline) arm.
 *
 * C 6.5.9p2 admits `==`/`!=` on any ARITHMETIC pair and a complex IS arithmetic
 * (6.2.5p11 makes the complex types floating, p18 makes the floating types
 * arithmetic, p21 makes them scalar); C 6.3.1.2 then defines a scalar's conversion
 * to `bool` as "compares equal to 0". For a complex both are the SAME componentwise
 * comparison against (0, 0), so all four spellings share one emitter — and this
 * example is the end-to-end proof that they agree at RUNTIME.
 *
 * ★★ EVERY ARM IS BUILT SO THAT LOOKING AT ONLY ONE COMPONENT CHANGES THE EXIT
 * CODE. That is the whole design, because the two defects this example was written
 * for were exactly "the imaginary part was ignored":
 *   • `!z` used to compare the complex's ADDRESS against 0 → constant FALSE.
 *     ✔MEASURED: `!(0.0 + 0.0i)` gave 0 where gcc 13.3.0 (`-std=c2x`) and clang
 *     18.1.3 (`-std=c23`), probed SEPARATELY, both give 1.
 *   • `(_Bool)z` used to discard the imaginary part (C 6.3.1.7's REAL-target rule,
 *     which `bool` is not governed by). ✔MEASURED: `(_Bool)(0.0 + 5.0i)` gave false
 *     where both references give true.
 * So `g_imOnly` = (0, 7) is truthy ONLY if the imaginary part is read, and
 * `g_reOnly` = (5, 0) vs `g_both` = (5, 7) are equal ONLY if it is ignored.
 *
 * ANTI-FOLD: every complex is a MUTABLE GLOBAL written at runtime (the c99_complex
 * precedent), so the `release` arm proves real runtime comparisons, not a constant
 * fold. pe64 (Win64), elf-x86_64 (SysV), elf-aarch64 (AAPCS64, qemu) and
 * macho64-arm64 must all agree — F32/F64 components have no per-format axis.
 *
 * exit = 1+2+4+8+16+32+64+128 = 255 iff every arm passes; each failing arm subtracts
 * its own distinct power of two, so the exit code NAMES which arm broke.
 */

#include <complex.h>

double _Complex g_zero;     /* (0, 0) */
double _Complex g_reOnly;   /* (5, 0) — nonzero real, ZERO imaginary */
double _Complex g_imOnly;   /* (0, 7) — ZERO real, nonzero imaginary */
double _Complex g_both;     /* (5, 7) */
double _Complex g_bothCopy; /* (5, 7) — a distinct object with equal value */
float  _Complex gf_zero;    /* (0, 0) — the F32-ELEMENT arm */
float  _Complex gf_a;       /* (2, 3) */
float  _Complex gf_b;       /* (2, 3) */

int main(void) {
    g_zero     = __builtin_complex(0.0, 0.0);
    g_reOnly   = __builtin_complex(5.0, 0.0);
    g_imOnly   = __builtin_complex(0.0, 7.0);
    g_both     = __builtin_complex(5.0, 7.0);
    g_bothCopy = __builtin_complex(5.0, 7.0);
    gf_zero    = __builtin_complex(0.0, 0.0);
    gf_a       = __builtin_complex(2.0, 3.0);
    gf_b       = __builtin_complex(2.0, 3.0);

    int acc = 0;

    /* 1 — `if (z)`: the truthiness chokepoint. `g_imOnly` is TRUE only if the
     *     imaginary component is part of the test. */
    {
        int ok = 1;
        if (g_zero)   ok = 0;
        if (!g_imOnly) ok = 0;
        if (!g_both)   ok = 0;
        if (!g_reOnly) ok = 0;
        if (ok) acc += 1;
    }

    /* 2 — logical `!`: C 6.5.3.3p5 makes it `(z == 0)`. THE REGRESSION ARM. */
    {
        int ok = 1;
        if (!g_zero != 1)   ok = 0;   /* !(0,0) must be 1 — was 0 */
        if (!g_imOnly != 0) ok = 0;   /* !(0,7) must be 0 */
        if (!g_reOnly != 0) ok = 0;   /* !(5,0) must be 0 */
        if (ok) acc += 2;
    }

    /* 3 — `(_Bool)z`: C 6.3.1.2 tests the WHOLE value. THE OTHER REGRESSION ARM. */
    {
        int ok = 1;
        if ((_Bool)g_imOnly != 1) ok = 0;   /* was 0 — imaginary discarded */
        if ((_Bool)g_reOnly != 1) ok = 0;
        if ((_Bool)g_zero   != 0) ok = 0;
        if (ok) acc += 4;
    }

    /* 4 — `==` / `!=` between two complex objects. `g_reOnly` and `g_both` share a
     *     real part and differ in the imaginary one, so a real-part-only equality
     *     would call them equal. */
    {
        int ok = 1;
        if (!(g_both == g_bothCopy)) ok = 0;
        if (!(g_both != g_reOnly))   ok = 0;
        if (g_reOnly == g_both)      ok = 0;
        if (g_zero != g_zero)        ok = 0;
        if (ok) acc += 8;
    }

    /* 5 — mixed real/complex equality: the real operand is promoted to a complex
     *     with a ZERO imaginary part, so `(5,0) == 5.0` holds and `(5,7) == 5.0`
     *     must NOT. */
    {
        int ok = 1;
        if (!(g_reOnly == 5.0)) ok = 0;
        if (g_both == 5.0)      ok = 0;
        if (!(g_both != 5.0))   ok = 0;
        if (ok) acc += 16;
    }

    /* 6 — the loop controlling expressions (`while`, `for`) take the same path. */
    {
        int ok = 0;
        while (g_imOnly) { ok += 1; break; }
        for (; g_zero; ) { ok += 100; break; }
        if (ok == 1) acc += 32;
    }

    /* 7 — the short-circuit operands and the ternary condition. */
    {
        int ok = 1;
        if (!(g_imOnly && 1)) ok = 0;
        if (g_zero && 1)      ok = 0;
        if (!(g_zero || 1))   ok = 0;
        if ((g_imOnly ? 3 : 9) != 3) ok = 0;
        if ((g_zero   ? 3 : 9) != 9) ok = 0;
        if (ok) acc += 64;
    }

    /* 8 — the F32-ELEMENT arm: the component offsets are {0, 4} and the compares
     *     are F32, so a hard-coded F64 element size or opcode breaks here only. */
    {
        int ok = 1;
        if (!(gf_a == gf_b)) ok = 0;
        if (gf_a == gf_zero) ok = 0;
        if (!!gf_zero)       ok = 0;
        if (!gf_a)           ok = 0;
        if (ok) acc += 128;
    }

    return acc;
}
