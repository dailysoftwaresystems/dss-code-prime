/* A CONSTANT CONDITION HAS ITS C TYPE, AND WHAT FOLLOWS IT IS REACHED WHEN C SAYS SO.
 *
 * The compiler folds the condition of every `if` below (each operand is a constant) and
 * concludes from it which statements are never reached. A fold that gets the TYPE of a
 * constant wrong gets its VALUE wrong — and then the run time walks into code the compiler
 * marked unreachable. Each pair of checks takes an `if` both ways: one condition true in C,
 * one false.
 *
 *   A. A statement expression's value is its last expression's (`({ g.b; })` is 5), and a
 *      bit-field is promoted by its width: `g.b` over an `unsigned b : 3` is an `int`. (The
 *      TYPE of `({ g.b; })` is a fork — gcc promotes it, clang and DSS keep the member's
 *      declared `unsigned int` — so this program asks only its value; the fork is pinned in
 *      tests/analysis/semantic/test_bit_field_value_is_promoted_by_its_width.cpp.)
 *   B. A conditional has the type the usual arithmetic conversions give its arms: `1 ? 5 :
 *      0u` is an `unsigned int`; `1 ? g.b : 0` an `int`; the unselected arm is never
 *      evaluated (`1 / 0`).
 *   C. A floating operation is folded at its type and rounded to it: `2.0f * 3.0f` is a
 *      `float`, and 2^24 + 1 is no float.
 *   D. A folded builtin has its own type: `__builtin_types_compatible_p` is an `int`.
 *   E. A `const` array read whole is its address: `!z` is 0 and `z == 0` false.
 *
 * Each failed check returns its own code; the program exits 42 when all hold.
 */

static const struct S { int pad; unsigned b : 3; } g = { 0, 5 };
static const int z[2] = { 0, 7 };

static int taken;

#define TRUE_IN_C(cond, code)  do { if (cond) ++taken; else return (code); } while (0)
#define FALSE_IN_C(cond, code) do { if (cond) return (code); ++taken; } while (0)

int main(void) {
    /* A */
    TRUE_IN_C(({ g.b; }) == 5, 1);
    FALSE_IN_C(({ g.b; }) != 5, 2);
    TRUE_IN_C(g.b - 6 < 0, 3);
    FALSE_IN_C(g.b - 6 >= 0, 4);
    /* B */
    TRUE_IN_C((1 ? 5 : 0u) - 6 > 0, 5);
    FALSE_IN_C((1 ? 5 : 0u) - 6 < 0, 6);
    TRUE_IN_C((1 ? g.b : 0) - 6 < 0, 7);
    FALSE_IN_C((1 ? g.b : 0u) - 6 < 0, 8);
    TRUE_IN_C((1 ? 5 : 1 / 0) == 5, 9);
    /* C */
    TRUE_IN_C((1 ? 2.0f * 3.0f : 1.0f) == 6.0f, 10);
    FALSE_IN_C((1 ? 2.0f * 3.0f : 1.0f) < 0, 11);
    TRUE_IN_C(16777216.0f + 1.0f - 16777216.0f == 0.0f, 12);
    FALSE_IN_C(16777216.0f + 1.0f - 16777216.0f != 0.0f, 13);
    /* D */
    TRUE_IN_C(__builtin_types_compatible_p(int, long) - 1 < 0, 14);
    FALSE_IN_C(__builtin_types_compatible_p(int, long) - 1 >= 0, 15);
    TRUE_IN_C((1 ? __builtin_types_compatible_p(int, int) : 0) == 1, 16);
    /* E */
    FALSE_IN_C(!z, 17);
    FALSE_IN_C(z == 0, 18);
    TRUE_IN_C(z != 0, 19);
    return taken == 19 ? 42 : 20;
}
