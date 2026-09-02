/* P46 (D-CSUBSET-COMPLEX-TO-REAL-IMPLICIT-CONVERSION-REFUSED): the runtime
 * witness for the implicit complex → real conversion C 6.3.1.7p2 defines — the
 * imaginary part is discarded, and NO cast is required to request it.
 *
 * WHY THIS IS A SEPARATE EXAMPLE FROM `c99_tgmath_complex`, and the reason is a
 * measured gate rather than tidiness: the tgmath example needs the eleven
 * `<complex.h>` extern imports, and those are gated off Mach-O until a real Mac
 * measures their Darwin link names (`tests/ffi/test_darwin_link_name_oracle`,
 * `tests/ffi/data/darwin-link-names.tsv`). THIS half needs no library at all —
 * `__builtin_complex` and the conversion are compiler-intrinsic — so it runs on
 * ALL FOUR targets, darwin included, and the conversion is therefore witnessed
 * on every leg even while the dispatch half is not.
 *
 * THE THREE POSITIONS ARE ONE RULE, WHICH IS WHY ALL THREE ARE HERE. C 6.5.16.1p1
 * asks only that both operands of an assignment have ARITHMETIC type, which
 * C 6.2.5p11+p18 make true of a complex; C 6.5.2.2p7 converts a call ARGUMENT
 * "as if by assignment"; and C 6.3.1.7p2 covers an integer target. A fix that
 * admitted only the assignment would look complete and leave the argument
 * position refusing.
 *
 * ✔MEASURED, each reference probed SEPARATELY and every binary RUN: gcc 13.3.0
 * `-std=c2x`, clang 18.1.3 `-std=c23` and mingw-w64 gcc 13.2.0 all compile this
 * file and exit 42. MSVC 19.51 ABSTAINS — it has no `_Complex` type specifier at
 * all (`error C2146`), which removes it from the vote rather than opposing it.
 *
 * ⚠ THE ONE TARGET THAT MUST NOT TAKE THE DISCARDING PATH IS `_Bool`, and check
 * 5 is here to keep it that way. `_Bool b = z;` is NOT "discard the imaginary
 * part and truncate"; C 6.3.1.2 makes it "compares unequal to 0", which for a
 * complex tests BOTH components — so `(0, 4i)` is TRUE even though its real part
 * is zero. Route it through the discard and it becomes FALSE: a silent wrong
 * answer ([[D-CSUBSET-COMPLEX-LOGICAL-NOT-AND-BOOL-CAST-SILENT-MISCOMPILE]],
 * [[D-CSUBSET-COMPLEX-TO-BOOL-ASSIGNMENT-NOT-ADMITTED-BY-THE-SEMANTIC-TIER]]).
 * That is exactly what the `tk != TypeKind::Bool` clause in `coerce` and the
 * admits-or-falls-through shape in `isAssignable` exist to prevent, and this
 * check is the runtime half of that guard.
 *
 * ANTI-FOLD: every operand rides a MUTABLE GLOBAL, so the `release`
 * shippedPipeline arm exercises real conversions rather than constants folded
 * before any of this runs.
 *
 * exit = 42.
 */

double _Complex g_z;    /* (3.75, 4.0) — a real part with a fraction, so the  */
                        /* integer target's truncation is observable          */
double _Complex g_zi;   /* (0.0, 4.0)  — real part ZERO, imaginary non-zero:  */
                        /* the shape that separates truthiness from discard   */
int             g_add;  /* 39 — keeps the exit code off any single check      */

static double realSink(double v) { return v + 0.0; }
static int    intSink(int v)     { return v; }

int main(void) {
    g_z   = __builtin_complex(3.75, 4.0);
    g_zi  = __builtin_complex(0.0, 4.0);
    g_add = 39;

    /* 1: assignment to a `double` — C 6.5.16.1p1, no cast. */
    double const d = g_z;
    if (d != 3.75)                       return 1;

    /* 2: assignment to a NARROWER float — the conversion composes with the
     *    ordinary F64→F32 narrowing rather than bypassing it. */
    float const f = g_z;
    if (f != 3.75f)                      return 2;

    /* 3: assignment to an INTEGER — C 6.3.1.7p2 then 6.3.1.4, truncating
     *    toward zero. 3.75 → 3, which a rounding conversion would make 4. */
    int const n = g_z;
    if (n != 3)                          return 3;

    /* 4: ARGUMENT position — C 6.5.2.2p7 makes this the SAME rule, and a fix
     *    that admitted only assignment would fail exactly here. */
    if (realSink(g_z) != 3.75)           return 4;
    if (intSink(g_z) != 3)               return 5;

    /* 5: `_Bool` is NOT the discarding path. (0, 4i) has a ZERO real part, so a
     *    discard would make this false; C 6.3.1.2 truthiness makes it TRUE. */
    _Bool const b = g_zi;
    if (!b)                              return 6;
    if (!g_zi)                           return 7;   /* the operand form too */

    /* 6: and the explicit cast, which was always legal, still means the same
     *    thing — the implicit conversion did not change what a cast does. */
    if ((double)g_z != 3.75)             return 8;
    if ((int)g_z != 3)                   return 9;

    return n + g_add;   /* 3 + 39 = 42 */
}
