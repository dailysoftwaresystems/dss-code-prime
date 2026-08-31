/* P46 (D-CSUBSET-COMPLEX-TO-REAL-IMPLICIT-CONVERSION-REFUSED,
 * D-CSUBSET-TGMATH-COMPLEX): the runtime witness for the TWO halves that had to
 * land together — the implicit complex -> real conversion C 6.3.1.7p2 defines,
 * and the <tgmath.h> complex dispatch C23 7.25 requires, which is the only
 * thing that keeps the first half from being a SILENT WRONG ANSWER.
 *
 * WHY BOTH IN ONE EXAMPLE, and it is the whole point: until P46 `sqrt(z)` under
 * <tgmath.h> was loud ONLY because `double d = z;` was refused one subsystem
 * over. Admit the conversion alone and `sqrt(z)` on a (0,4i) compiles and
 * answers 0 — the real prototype's `creal` — where gcc 13.3.0, clang 18.1.3 and
 * mingw-w64 gcc 13.2.0 all dispatch to `csqrt` and answer 1. This example fails
 * on EITHER half being reverted, which is exactly the coupling.
 *
 * WHAT DISPATCH LOOKS LIKE HERE. `_Generic` selection is static and `sizeof`
 * does not evaluate its operand, so `sizeof(sqrt(z)) == sizeof(double _Complex)`
 * iff the COMPLEX arm was selected (16) and `== sizeof(double)` (8) iff the
 * macro reached the real prototype. Checks 1-3 are that compile-time witness;
 * checks 4-9 are the runtime VALUES, which are what a wrong-library binding
 * (right arm, wrong symbol) would break while the sizeof witness stayed green.
 *
 * THE DARWIN ARM LANDED 2026-08-31, AND WHAT IT COST IS THE POINT. This example
 * first shipped with NO darwin arm: the eleven complex extern rows were gated
 * `availableObjectFormats: [pe, elf]` because
 * `tests/ffi/test_darwin_link_name_oracle` requires every Mach-O-visible
 * imported function to have a MEASURED row in
 * `tests/ffi/data/darwin-link-names.tsv` — what a real Darwin compiler emits
 * for that identifier, per arch — and none of the eleven had one. The oracle is
 * right: a wrong-but-existing spelling links clean, loads clean and misbinds
 * SILENTLY, so macho REFUSED a complex argument loudly rather than guess.
 * The measurement was then taken on a real Mac (macOS 26.6.2, Apple clang
 * 21.0.0): all eleven emit the PLAIN decorated name on BOTH arches, so all
 * eleven imply an absent `linkName`. ★ THAT ANSWER IS WHY THE GATE WAS RIGHT
 * RATHER THAN PARANOID — "they are probably plain" would have been CORRECT and
 * still worthless, because the same reasoning applied to `stat` is exactly what
 * bound the legacy 32-bit-inode callee and made sqlite call every database
 * malformed. An absent measurement and a measurement that came back plain are
 * different states; only one of them is evidence.
 *
 * THE REAL-ONLY MACROS ARE NOT EXERCISED HERE AND THAT IS DELIBERATE: `floor`,
 * `ceil`, `log10`, `atan2`, `fmod` and `ldexp` have no <complex.h> counterpart,
 * so a complex argument must REFUSE — MEASURED, gcc and clang both reject all
 * six — and a refusal cannot be witnessed by a program that runs. It is pinned
 * at the semantic tier in test_semantic_analyzer_c (TgmathRealOnly*), which is
 * the tier that can assert a diagnostic.
 *
 * ANTI-FOLD: every operand rides a MUTABLE GLOBAL (the c11_atomic /
 * c99_tgmath precedent) so the `release` shippedPipeline arm witnesses real
 * dispatched csqrt/cabs/cpow/cexp/clog calls rather than constants a
 * const-fold could have answered at compile time.
 *
 * TOLERANCES, and why they are not laziness: csqrt/cpow/cexp/clog are libm
 * transcendentals whose last ulp legitimately differs between glibc, ucrtbase
 * and Apple's libm, so a bit-exact pin would be a cross-leg flake rather than a
 * guard. `cabs(3+4i) == 5` IS pinned exactly (hypot of 3,4 is representable),
 * as is every conversion result, which is where the exactness matters.
 *
 * exit = 42.
 */
#include <tgmath.h>

double _Complex g_z34;   /* (3, 4)  -> cabs = 5, and the conversion operand   */
double _Complex g_z4i;   /* (0, 4i) -> csqrt = sqrt(2)*(1+i)                  */
double _Complex g_z2i;   /* (0, 2i) -> cpow(z, 2) = -4                        */
double _Complex g_z0;    /* (0, 0)  -> cexp = 1                               */
double _Complex g_ze;    /* (e, 0)  -> clog = 1                               */
double          g_two;   /* 2.0, the cpow exponent (a REAL second argument)   */
double          g_d;     /* 16.0, the real-arm control                        */
float           g_f;     /* 4.0f, the float-arm width control                 */
int             g_i;     /* 9, the integer-rides-the-f64-arm control          */

/* A real-parameter sink: C 6.5.2.2p7 converts a call ARGUMENT "as if by
 * assignment", so this is the SAME rule as `double d = z;` and not a second
 * one. It is a real function, never a macro, so the tgmath surface is not
 * involved in check 12 at all. */
static double realSink(double v) { return v; }

static int closeTo(double a, double b) {
    double const d = a - b;
    return d < 1e-9 && d > -1e-9;
}

int main(void) {
    g_z34 = __builtin_complex(3.0, 4.0);
    g_z4i = __builtin_complex(0.0, 4.0);
    g_z2i = __builtin_complex(0.0, 2.0);
    g_z0  = __builtin_complex(0.0, 0.0);
    g_ze  = __builtin_complex(2.718281828459045, 0.0);
    g_two = 2.0;
    g_d   = 16.0;
    g_f   = 4.0f;
    g_i   = 9;

    /* 1-3: COMPILE-TIME dispatch witnesses.  `_Generic` selection is static and
     * `sizeof` does not evaluate its operand, so a 16-byte answer means the
     * COMPLEX arm was selected and an 8-byte one means the macro reached the
     * real prototype.  THIS IS THE ONLY TIER THAT CAN SEE A MISSING ARM: a
     * complex arm that is simply GONE produces no diagnostic at all (the
     * argument falls to `default:` and silently converts), so the semantic-tier
     * pins stay green over it -- MEASURED, red-on-disable arm M3.  ALL ELEVEN
     * macros are witnessed here for that reason, not just a spot check of two. */
    if (sizeof(sqrt(g_z4i)) != sizeof(double _Complex)) return 1;
    if (sizeof(sin(g_z34))  != sizeof(double _Complex)) return 14;
    if (sizeof(cos(g_z34))  != sizeof(double _Complex)) return 15;
    if (sizeof(tan(g_z34))  != sizeof(double _Complex)) return 16;
    if (sizeof(asin(g_z34)) != sizeof(double _Complex)) return 17;
    if (sizeof(acos(g_z34)) != sizeof(double _Complex)) return 18;
    if (sizeof(atan(g_z34)) != sizeof(double _Complex)) return 19;
    if (sizeof(exp(g_z34))  != sizeof(double _Complex)) return 20;
    if (sizeof(log(g_z34))  != sizeof(double _Complex)) return 21;
    /* pow dispatches on EITHER argument (C23 7.25p3), so all three mixes. */
    if (sizeof(pow(g_z34, g_z34)) != sizeof(double _Complex)) return 22;
    if (sizeof(pow(g_z34, g_two)) != sizeof(double _Complex)) return 23;
    if (sizeof(pow(g_two, g_z34)) != sizeof(double _Complex)) return 24;
    /* The REAL arms must keep their real widths -- the control without which
     * every check above would pass over a macro answering complex for
     * everything, which is the mirror defect. */
    if (sizeof(sqrt(g_d))   != sizeof(double))          return 2;
    if (sizeof(sqrt(g_f))   != sizeof(float))           return 25;
    if (sizeof(sqrt(g_i))   != sizeof(double))          return 26;
    if (sizeof(pow(g_d, g_d)) != sizeof(double))        return 27;
    /* fabs is the ONE that sizeof cannot discriminate, stated rather than
     * quietly omitted: `cabs` returns a REAL, so 8 bytes holds whichever arm
     * was selected.  Its arm is separated only by the VALUE, at check 6. */
    if (sizeof(fabs(g_z34)) != sizeof(double))          return 3;

    /* 4-9: RUNTIME values — the half a sizeof witness cannot see. */
    if (!closeTo(__builtin_creal(sqrt(g_z4i)), 1.4142135623730951)) return 4;
    if (!closeTo(__builtin_cimag(sqrt(g_z4i)), 1.4142135623730951)) return 5;
    if (fabs(g_z34) != 5.0)                                         return 6;
    if (!closeTo(__builtin_creal(pow(g_z2i, g_two)), -4.0))         return 7;
    if (!closeTo(__builtin_creal(exp(g_z0)), 1.0))                  return 8;
    if (!closeTo(__builtin_creal(log(g_ze)), 1.0))                  return 9;

    /* 10-12: the implicit complex -> real conversion itself, in all three
     * positions C 6.3.1.7p2 / 6.5.16.1p1 / 6.5.2.2p7 govern. The imaginary
     * part is DISCARDED, which is conformant — all three references do it. */
    double const dropped = g_z34;                    /* assignment, no cast   */
    if (dropped != 3.0)                              return 10;
    int const truncated = g_z34;                     /* complex -> integer    */
    if (truncated != 3)                              return 11;
    if (realSink(g_z34) != 3.0)                      return 12; /* argument   */

    /* The real arm is untouched by any of it: sqrt(16.0) is still 4. */
    if (!closeTo(sqrt(g_d), 4.0))                    return 13;

    return 42;
}
