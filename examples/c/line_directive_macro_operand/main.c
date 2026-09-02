/* D-CPP-LINE-DIRECTIVE-MACRO-OPERAND: C23 6.10.6p6 — a `# line pp-tokens`
 * directive that matches NEITHER literal form is PERMITTED. Its operand tokens
 * are "processed just as in normal text", and the directive resulting after all
 * replacements shall match one of the two literal forms.
 *
 * ⚠ THE SECTION IS 6.10.6, NOT 6.10.4. C23 renumbered the preprocessor: 6.10.4
 * is now `#embed` (Binary resource inclusion) and Line control moved to 6.10.6.
 * The C11 number was 6.10.4, and older comments in this repository carry it
 * under a "C23" label.
 *
 * ★ WHY AN EXIT CODE AND NOT A COMPILE. Every arm below RETURNS a value derived
 * from `__LINE__`/`__FILE__` AFTER its directive, so a wrong renumbering changes
 * the exit code instead of merely compiling. A `#line` implementation that
 * accepts the directive and mis-numbers is the silent-wrongness the bar forbids,
 * and "it compiled" cannot see it.
 *
 * ✔MEASURED BY EXECUTION, gcc 13.3.0 and clang 18.1.3 probed SEPARATELY: this
 * file builds and runs to exit 42 under both, and MSVC 19.51.36252 accepts every
 * shape in it. Nothing here needs C23 number lexing, so it holds under `-std=c17`
 * too — the C23 digit-separator arms live in their own example next door
 * (`line_directive_digit_separators`), because they close a DIFFERENT row and a
 * shared exit code cannot say which one regressed.
 *
 * FIVE ARMS, and each is scored 0 by a different plausible wrong implementation:
 *   1. an OBJECT-LIKE macro expanding to the digit sequence — the row's own
 *      example, and the arm an implementation that never expands scores 0 on.
 *   2. ONE macro carrying BOTH the number and the quoted name — an
 *      implementation that expands the first operand token only takes the
 *      digits, passes arm 1, and scores 0 here.
 *   3. a FUNCTION-LIKE macro whose `##` paste ASSEMBLES the digits — a private
 *      one-token substituter passes arms 1 and 2 and scores 0 here, because
 *      arity, argument pre-expansion and pasting are the ordinary expander's.
 *   4. RESCAN (a macro expanding to another macro) with the name supplied by a
 *      SECOND macro in the same directive.
 *   5. ★ THE SELF-REFERENTIAL ARM, the only one that fails toward a WRONG
 *      NUMBER rather than an error: `#line __LINE__` after an earlier `#line`.
 *      The operand's `__LINE__` must read the mapping in effect BEFORE this
 *      directive — not the physical line, and not the record being created.
 *
 * ANTI-FOLD: the whole score is multiplied by a runtime `argc` predicate, so the
 * release pipeline cannot reduce `main` to a single constant return.
 *
 * Data-model-independent (int arithmetic and byte comparisons only), so the one
 * exit code holds on all four targets and on the shipped release pipeline.
 * exit = 8 + 8 + 8 + 9 + 9 = 42.
 */

/* ARM 1 — an OBJECT-LIKE macro expanding to the digit sequence. */
#define ALPHA 300
#line ALPHA
static int arm1_line(void) { return __LINE__; }          /* 300 */

/* ARM 2 — ONE macro carrying BOTH halves of the second literal form. */
#define BETA 400 "Beta.h"
#line BETA
static int arm2_line(void) { return __LINE__; }          /* 400 */
static int arm2_file_first(void) { return __FILE__[0]; } /* 'B' == 66 */

/* ARM 3 — a FUNCTION-LIKE macro whose `##` paste assembles the digits. */
#define JOIN(a, b) a##b
#line JOIN(5, 00)
static int arm3_line(void) { return __LINE__; }          /* 500 */

/* ARM 4 — RESCAN: the number arrives through a second macro, and the name
 * through a third, in one directive. */
#define GAMMA_N SIX_HUNDRED
#define SIX_HUNDRED 600
#define GAMMA_F "Gamma.h"
#line GAMMA_N GAMMA_F
static int arm4_line(void) { return __LINE__; }          /* 600 */
static int arm4_file_first(void) { return __FILE__[0]; } /* 'G' == 71 */

/* ARM 5 — the SELF-REFERENTIAL case. `#line 700 "Delta.h"` makes the next line
 * presumed 700, so the `#line __LINE__` two lines below it sees 701 and the line
 * after THAT is 701. An implementation resolving the operand against the record
 * it is about to create, or against the physical line, gives another number and
 * this arm scores 0 while every other arm still passes. */
#line 700 "Delta.h"
static int arm5_before(void) { return __LINE__; }        /* 700 */
#line __LINE__
static int arm5_after(void) { return __LINE__; }         /* 701 */

/* Back to an ordinary presumed position for the entry point itself. */
#line 1000 "main.c"
int main(int argc, char **argv) {
    (void)argv;
    int const a = (arm1_line() == 300) ? 8 : 0;
    int const b = (arm2_line() == 400 && arm2_file_first() == 'B') ? 8 : 0;
    int const c = (arm3_line() == 500) ? 8 : 0;
    int const d = (arm4_line() == 600 && arm4_file_first() == 'G') ? 9 : 0;
    int const e = (arm5_before() == 700 && arm5_after() == 701) ? 9 : 0;
    return (a + b + c + d + e) * ((argc > 0) ? 1 : 0);
}
