/* C23 6.10.6p4 makes the
 * `#line` digit sequence "interpreted as a decimal integer, ignoring any
 * optional digit separators (6.4.4.2)". DSS refused every such directive with
 * `#line requires a digit sequence — got: 1'000`, because the validator's
 * character test was a literal `"0123456789"` set.
 *
 * ⚠ THE SECTION IS 6.10.6, NOT 6.10.4 — C23 renumbered the preprocessor and
 * 6.10.4 is now `#embed`. The separator BYTE is not spelled in the engine
 * either: it is read from the language's own `numberStyle.digitSeparator`, so a
 * language declaring none still accepts digits only.
 *
 * ★ IGNORED, NOT MERELY TOLERATED. A scan that stopped at the separator would
 * accept the directive and renumber to 1 instead of 1000 — accepted-and-wrong,
 * which is worse than the refusal it replaces. Every arm therefore RETURNS a
 * value derived from `__LINE__` after its directive, so the exit code moves when
 * the value is wrong rather than only when the compile fails.
 *
 * ✔MEASURED BY EXECUTION under `-std=c2x`: gcc 13.3.0 and clang 18.1.3 both
 * build and run this to exit 42; MSVC 19.51.36252 under `/std:clatest` accepts
 * the same shapes. ⚠ Under `-std=c17` gcc and clang REFUSE it, and correctly so
 * — digit separators are a C23 addition, which is exactly why this is a separate
 * example from `line_directive_macro_operand` next door: that one holds under
 * C17 as well, and a shared exit code could not say which rule regressed.
 *
 * THREE ARMS:
 *   1. the LITERAL form with one separator.
 *   2. the MACRO-EXPANDED form (C23 6.10.6p6) with TWO separators, one of them
 *      adjacent to another — an implementation stripping only the first gives
 *      2000 -> 200 and scores 0 here while passing arm 1.
 *   3. the literal form WITH a file operand, so the separator handling is proven
 *      not to have eaten the operand that follows it.
 *
 * ANTI-FOLD: the score is multiplied by a runtime `argc` predicate, so the
 * release pipeline cannot reduce `main` to a single constant return.
 *
 * Data-model-independent (int arithmetic and byte comparisons only).
 * exit = 14 * 3 = 42.
 */

/* ARM 1 — the LITERAL form. A scan stopping at the separator gives 1. */
#line 1'000
static int arm1_line(void) { return __LINE__; }          /* 1000 */

/* ARM 2 — the MACRO-EXPANDED form, with two separators. */
#define TWO_THOUSAND 2'0'00
#line TWO_THOUSAND
static int arm2_line(void) { return __LINE__; }          /* 2000 */

/* ARM 3 — a separated digit sequence FOLLOWED by a file operand. */
#line 3'000 "Sep.h"
static int arm3_line(void) { return __LINE__; }          /* 3000 */
static int arm3_file_first(void) { return __FILE__[0]; } /* 'S' == 83 */

/* Back to an ordinary presumed position for the entry point itself. */
#line 4000 "main.c"
int main(int argc, char **argv) {
    (void)argv;
    int const a = (arm1_line() == 1000) ? 14 : 0;
    int const b = (arm2_line() == 2000) ? 14 : 0;
    int const c = (arm3_line() == 3000 && arm3_file_first() == 'S') ? 14 : 0;
    return (a + b + c) * ((argc > 0) ? 1 : 0);
}
