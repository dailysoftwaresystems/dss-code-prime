/* D-TARGET-ENCODING-WIDTH-GUARD (LD-7): the runtime witness for a
 * `long double _Complex`'s PROMOTED ZERO — the real→complex construct, whose
 * imaginary half IS that zero, and the truthiness spellings that compare
 * against it.
 *
 * ✔MEASURED BEFORE THE CHANGE, through the shipped CLI at de1e83ef with one
 * tiny program per shape compiled ALONE so no refusal could mask another:
 *
 *     long double _Complex z = 2.0L;   rc 1  BOTH long-double axes
 *     !z                               rc 1  BOTH long-double axes
 *     z == w                           rc 0  (already worked)
 *     z + w                            rc 0  (already worked)
 *
 * on x86_64:elf64-x86_64-linux-exec (F80) and arm64:elf64-aarch64-linux-exec
 * (F128), each with `L_UnsupportedLoweringForOpcode: MIR Const %N is a float
 * (FPR-class) literal at MIR→LIR` — a diagnostic naming THIS anchor. All four
 * compiled on x86_64:pe64-x86_64-windows-exec, where `long double` IS `double`.
 *
 * ★★ ONE GATE, THREE DOORS. C 6.5.3.3p5 makes `!E` exactly `(0 == E)`, and
 * C 6.3.1.7 makes a real→complex conversion "(the real value, +0)". Both need a
 * float ZERO of the element type, and a register machine has no float-immediate
 * form — so the zero must be promoted to an anonymous rodata global and LOADED.
 * `elementFloatConst` is the ONE owner of that promotion, and its width list
 * was F64/F32 only: an F80/F128 zero fell through to a bare MIR `Const` and
 * dead-ended. Widening that list fixes the two shapes here AND scalar
 * `!someLongDouble` (arm 33 of `examples/c/c_long_double_convert`) together.
 *
 * ★ WHY THIS IS A SEPARATE FILE FROM BOTH OF ITS RELATIVES, and both reasons
 * are MEASURED rather than stylistic:
 *   • not in `c_long_double_convert`, because ✔cl.exe 19.51 refuses
 *     `__builtin_complex` outright (`error C2065`) — MSVC has no C99
 *     `_Complex` — and that file's reference oracle rests on MSVC's positive
 *     vote for the f64 axis;
 *   • not in `c99_complex_truth`, because ✔a ninth arm there would need
 *     `acc += 256` and a POSIX exit status is EIGHT BITS: 511 & 0xFF is 255,
 *     which is that file's PASSING value, so the new arm would have been
 *     vacuous on every Linux leg and indistinguishable from a pass.
 *
 * ★★ EVERY ARM IS BUILT SO THAT READING ONLY ONE COMPONENT CHANGES THE ANSWER,
 * the same construction `c99_complex_truth` uses: `g_imOnly` = (0, 7) is truthy
 * ONLY if the imaginary part is read, and it compares equal to `g_reOnly` =
 * (5, 0) ONLY if one of the two components is ignored. `g_fromReal` is the
 * construct arm: a construct that DROPPED the imaginary zero leaves whatever
 * was in that half of the object, so it stops comparing equal to the real 2.0L.
 *
 * ⓘ ON THE f64 AXIS (pe64 x86_64, Apple arm64) the element IS `double`, so
 * this file collapses to the shape `c99_complex_truth` already covers and does
 * NOT discriminate there. It is the two WIDE axes — F80 components at offsets
 * {0, 16} on x87-80, F128 on ieee128 — that this file exists for, and saying so
 * is better than implying three-axis coverage the arms cannot deliver.
 *
 * ANTI-FOLD: every complex and every real operand is a MUTABLE GLOBAL written
 * at runtime, so the `release` arm measures real runtime comparisons rather
 * than a constant fold.
 *
 * ✔REFERENCE ORACLE, each probed SEPARATELY and RUN: WSL gcc 13.3.0 (−O0/−O2),
 * WSL clang 18.1.3 (−O0/−O2) and aarch64-linux-gnu-gcc 13.3.0 under
 * qemu-aarch64 (−O0/−O2) all return 42.
 *
 * exit = 42; each arm returns its own number, so a failure names itself.
 */

#include <complex.h>

long double _Complex g_zero;      /* (0, 0) */
long double _Complex g_reOnly;    /* (5, 0) — nonzero real, ZERO imaginary */
long double _Complex g_imOnly;    /* (0, 7) — ZERO real, nonzero imaginary */
long double _Complex g_both;      /* (5, 7) */
long double _Complex g_bothCopy;  /* (5, 7) — a distinct object, equal value */
long double _Complex g_fromReal;  /* built from a REAL: its imaginary half IS
                                   * the promoted zero this file is about */
long double          g_two = 2.0L;
long double          g_five = 5.0L;
long double          g_zeroR = 0.0L;

int main(void) {
    g_zero     = __builtin_complex(0.0L, 0.0L);
    g_reOnly   = __builtin_complex(5.0L, 0.0L);
    g_imOnly   = __builtin_complex(0.0L, 7.0L);
    g_both     = __builtin_complex(5.0L, 7.0L);
    g_bothCopy = __builtin_complex(5.0L, 7.0L);
    g_fromReal = g_two;              /* THE CONSTRUCT — arm 3 */

    /* ── ARM 1: the values really are what the later arms assume ──────────
     * Non-vacuity. If the constructor itself were broken these would fire
     * here instead of making every arm below trivially true. */
    if (g_reOnly == g_imOnly) return 1;
    if (g_both == g_reOnly)   return 1;
    if (!(g_both == g_bothCopy)) return 1;
    if (!(g_five != g_zeroR)) return 1;

    /* ── ARM 2: truthiness — `!z` and `(_Bool)z` read BOTH components ─────
     * C 6.3.1.2: a scalar's conversion to bool is "compares equal to 0", and
     * for a complex that is the componentwise comparison against (0, 0).
     * ⚠ THIS IS THE SHAPE THAT DID NOT COMPILE: the zero it compares against
     * is the promoted one. */
    if (!g_zero   != 1) return 2;    /* (0,0) is FALSE  */
    if (!g_imOnly != 0) return 2;    /* imaginary-ONLY is TRUTHY */
    if (!g_reOnly != 0) return 2;
    if (!g_both   != 0) return 2;
    if ((_Bool)g_zero   != 0) return 2;
    if ((_Bool)g_imOnly != 1) return 2;
    if ((_Bool)g_reOnly != 1) return 2;
    if (!!g_imOnly != 1) return 2;
    if (!!g_zero   != 0) return 2;

    /* ── ARM 3: the real→complex CONSTRUCT, whose imaginary half is the zero ─
     * `g_fromReal` was built from the real 2.0L, so it must equal 2.0L and
     * must NOT equal a complex with the same real part and a nonzero
     * imaginary one. A construct that dropped the zero fails the first check
     * (the imaginary half holds whatever was there); one that put the REAL
     * value in both halves fails the second. */
    if (!(g_fromReal == g_two)) return 3;
    if (g_fromReal != g_two)    return 3;
    if (!g_fromReal != 0)       return 3;
    {
        long double _Complex twoAndSeven = __builtin_complex(2.0L, 7.0L);
        if (g_fromReal == twoAndSeven) return 3;
    }

    /* ── ARM 4: mixed real/complex equality takes the SAME promotion ──────
     * C 6.3.1.7 converts the real operand to a complex with a +0 imaginary
     * part, so `(5,0) == 5.0L` holds and `(5,7) == 5.0L` must not. */
    if (!(g_reOnly == g_five)) return 4;
    if (g_both == g_five)      return 4;
    if (!(g_both != g_five))   return 4;
    if (!(g_zero == g_zeroR))  return 4;

    /* ── ARM 5: the CONTROL positions, which already worked before this
     * cycle and must keep working — `if`, `while`, `?:` and `&&` get their
     * zero from `coerceCondition`, a different route to the same promotion. */
    {
        int n = 0;
        if (g_imOnly) n += 1;
        if (g_zero)   n += 100;
        while (g_reOnly) { n += 2; break; }
        n += (g_imOnly ? 4 : 40);
        n += (g_zero   ? 80 : 8);
        if (g_both && 1) n += 16;
        if (g_zero && 1) n += 200;
        if (n != 31) return 5;
    }

    /* ── ARM 6: ARITHMETIC still works and still agrees with the zeros ────
     * Addition already compiled before this cycle; it is here so a change
     * that fixed the zero by breaking the components would be caught. */
    {
        long double _Complex s = g_reOnly + g_imOnly;   /* (5, 7) */
        if (!(s == g_both))       return 6;
        if (s == g_reOnly)        return 6;
        if (!s != 0)              return 6;
        if ((g_zero + g_zero) != g_zero) return 6;
        if (!(g_zero + g_zero) != 1)     return 6;
    }

    return 42;
}
