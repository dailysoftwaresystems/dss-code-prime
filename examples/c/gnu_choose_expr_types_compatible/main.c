/* GNU `__builtin_choose_expr` + `__builtin_types_compatible_p` — the pre-C11
 * `_Generic`, and the reason they land in one example rather than two.
 *
 * ★★★ NEITHER HALF IS MUCH USE ALONE. `__builtin_types_compatible_p` answers a
 * question and `__builtin_choose_expr` acts on an answer; together they are how
 * a portable header writes a type-dispatching macro that still compiles as C89,
 * which is the form SHAPE 5 below spells out. Splitting them into two examples
 * would have produced two files that each demonstrate half an idiom.
 * ✔MEASURED through the shipped CLI at HEAD `60198126`: both were
 * `error[S0001] … undeclared identifier` / `error[P0002] expected 'ParenClose'
 * — got 'int'`, i.e. DSS was reading each as an ordinary CALL. Neither can be:
 * `__builtin_types_compatible_p` takes two TYPE-NAMES, and
 * `__builtin_choose_expr` DISCARDS one operand unevaluated.
 * ✔MEASURED on gcc 13.3.0, clang 18.1.3 and clang 19.1.1 (`-std=gnu17`): all
 * three compile this exact file and RUN it to 42.
 *
 * ★★ SHAPE 1 IS THE ONE THAT PROVES THE DISCARD IS REAL. `"not an int"` is a
 * `char *`, and the enclosing initializer is an `int` — so this line COMPILES
 * only if the unchosen arm is dropped before it is ever type-checked as a value.
 * gcc documents exactly that ("the unused expression … may be of any type"). An
 * implementation that lowered both arms and picked at run time would fail here
 * rather than answer wrongly, which is why this shape belongs first.
 *
 * ★★ SHAPE 3 IS THE ONE THAT WOULD SILENTLY BREAK ON A WINDOWS TARGET. On
 * LLP64 — the pe64 leg — `long` and `int` share a REPRESENTATION, so a
 * compatibility test written as a width comparison answers 1 for `(int, long)`
 * on Windows and 0 on Linux, from the same source, with no diagnostic anywhere.
 * The correct answer is 0 on BOTH, because C compatibility is about TYPE
 * IDENTITY and not about representation — which is what
 * [[D-LANG-TYPE-IDENTITY-VOCABULARY]] made the interned identity carry. This
 * file is built for both data models precisely so that line is a cross-target
 * discriminator rather than a local one.
 *
 * ★★ SHAPE 6 IS THE UNEVALUATED PROOF AT ITS SHARPEST: `1/0` in the discarded
 * arm of a CONSTANT EXPRESSION. If the const-eval engine visited both arms it
 * would trip its own divide-by-zero wall and refuse the array dimension; folding
 * to 4 is only possible if the discarded arm is never visited at all. gcc and
 * both clangs agree.
 *
 * ★ SHAPE 4 pins the RESULT TYPE rather than the result value — the node must
 * take the WINNER's type, not the controlling expression's and not the first
 * arm's. `_Generic` is the only construct in the language that can ask, which is
 * why it appears in a file that is not about `_Generic`.
 */

typedef int dss_myint;

/* SHAPE 6 — the discarded arm holds `1/0`, inside a constant expression.
 * `dss_dim` is `int[4]`. */
int dss_dim[__builtin_choose_expr(1, 4, 1 / 0)];
_Static_assert(__builtin_choose_expr(0, 1, 4) == 4,
               "choose_expr must fold in a constant expression");
_Static_assert(__builtin_types_compatible_p(int, dss_myint) == 1,
               "a typedef is compatible with the type it names");
_Static_assert(__builtin_types_compatible_p(int, long) == 0,
               "int and long are distinct types under EVERY data model");

/* The GNU pre-C11 `_Generic`, spelled the way a real header spells it. */
#define DSS_IS(T, x)  __builtin_types_compatible_p(T, __typeof__(x))
#define DSS_PICK(x)   __builtin_choose_expr(DSS_IS(double, x), 40, 2)

volatile int dss_seed = 5;

int main(void) {
    int    r;
    double d;
    int    i;
    int    seed;

    d    = 0.0;
    i    = 0;
    seed = dss_seed;
    (void)d;
    (void)i;

    /* SHAPE 1 — THE DISCARD. See the header note: the unchosen arm is a
     * `char *` and this is an `int` initializer. */
    r = __builtin_choose_expr(1, 42, "not an int");
    if (r != 42) return 1;

    /* SHAPE 2 — the other direction, and the discarded arm is again of a type
     * the surviving context would reject. */
    if (__builtin_choose_expr(0, "not an int", 42) != 42) return 2;

    /* SHAPE 3 — TYPE IDENTITY, NOT REPRESENTATION. The `(int, long)` line is the
     * cross-data-model discriminator; the rest fix the easy directions. */
    if (__builtin_types_compatible_p(int, int) != 1) return 3;
    if (__builtin_types_compatible_p(int, long) != 0) return 4;
    if (__builtin_types_compatible_p(int, dss_myint) != 1) return 5;
    if (__builtin_types_compatible_p(char *, int *) != 0) return 6;
    if (__builtin_types_compatible_p(double, double) != 1) return 7;

    /* SHAPE 4 — THE RESULT TYPES, asked the only way C can ask. The chosen arm's
     * type IS the expression's type (int here, double there), and
     * `__builtin_types_compatible_p` yields `int`. */
    if (_Generic(__builtin_choose_expr(1, 1, 1.0), int: 1, double: 0, default: 0) != 1) return 8;
    if (_Generic(__builtin_choose_expr(0, 1, 1.0), int: 0, double: 1, default: 0) != 1) return 9;
    if (_Generic(__builtin_types_compatible_p(int, int), int: 1, default: 0) != 1) return 10;

    /* SHAPE 5 — THE IDIOM. One macro, two call sites, two different answers,
     * chosen entirely at compile time from the argument's type. */
    if (DSS_PICK(d) != 40) return 11;
    if (DSS_PICK(i) != 2)  return 12;

    /* SHAPE 6 — the array dimension really is 4 elements. */
    if ((int)(sizeof dss_dim / sizeof dss_dim[0]) != 4) return 13;

    /* Every mismatch above already returned its own code; this arithmetic is a
     * second, weaker check kept so the exit code is a FUNCTION of the measured
     * answers rather than a constant a miscompile could still produce.
     * 40 + 2 == 42, and the seed is added then subtracted so a folded-away
     * volatile load changes the answer. */
    return DSS_PICK(d) + DSS_PICK(i) + seed - dss_seed;
}
