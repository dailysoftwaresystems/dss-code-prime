/* D-C-PREPROCESSED-INPUT-REFUSES-GCC-LINEMARKERS: the GNU LINEMARKER form
 * `# N "file" [flags]` — what `gcc -E` and `clang -E` write where a `#line` would
 * go — is accepted and SETS the presumed position, flag tail and all.
 *
 * ★ WHY THIS FILE LOOKS LIKE PREPROCESSOR OUTPUT: it IS the shape. A TU that has
 * been through `-E` carries nothing else; at the pre-change HEAD every one of them
 * died on `error[P0015] unsupported preprocessor directive: 1`, so a conformance
 * census had to run `-E -P` instead and throw away exactly the provenance it
 * wanted. ✔MEASURED alongside this example: one real `gcc -E` of <stdio.h> +
 * <string.h> is 1147 lines carrying 152 linemarkers; it now compiles and RUNS
 * (exit 42) with the same single diagnostic the `-P` form produces.
 *
 * FOUR ARMS, and each one would be scored 0 by a plausible wrong implementation:
 *   1. the presumed LINE is taken from a marker WITH a flag tail (`1 3 4`) — an
 *      implementation that parsed the number and stopped at the flags would
 *      mis-number here.
 *   2. the presumed line survives a marker carrying the OTHER nesting flag (`2`),
 *      so the enter/return pair is not silently treated as two different features.
 *   3. the presumed FILE drives `__FILE__`, and it is read at TWO different
 *      markers whose names differ in their first byte — an implementation that
 *      set the line but not the name scores 0 here while passing arms 1 and 2.
 *   4. line number ZERO is accepted, which `#line` forbids (C23 6.10.4p2) and
 *      which gcc's own `-E` output opens with. ✔MEASURED: gcc 13.3.0 emits
 *      `# 0 "tu.c"` and recompiles its own output rc=0, so importing `#line`'s
 *      floor here would refuse the very bytes this row exists to read.
 *
 * ANTI-FOLD: the whole score is multiplied by a runtime `argc` predicate, so the
 * release pipeline cannot reduce `main` to a single constant return.
 *
 * Data-model-independent (int arithmetic and byte comparisons only), so the one
 * exit code holds on all four targets and on the shipped release pipeline.
 * exit = 10 + 10 + 12 + 10 = 42.
 */

/* ARM 1 — a marker WITH the full gcc flag tail: enter-file, system-header,
 * extern-c-linkage. The next line is presumed line 100 of "Alpha.h". */
# 100 "Alpha.h" 1 3 4
static int arm1_line(void) { return __LINE__; }        /* 100 */
static int arm1_file_first(void) { return __FILE__[0]; } /* 'A' == 65 */

/* ARM 2 — the RETURN-to-file flag, the other member of the exclusive pair. */
# 200 "Beta.h" 2 3 4
static int arm2_line(void) { return __LINE__; }        /* 200 */
static int arm2_file_first(void) { return __FILE__[0]; } /* 'B' == 66 */

/* ARM 4 — line ZERO, legal in a linemarker and illegal in `#line`. */
# 0 "Zero.h"
static int arm4_line(void) { return __LINE__; }        /* 0 */

/* Back to an ordinary presumed position for the entry point itself. */
# 60 "main.c" 2
int main(int argc, char **argv) {
    (void)argv;
    int const a = (arm1_line() == 100) ? 10 : 0;
    int const b = (arm2_line() == 200) ? 10 : 0;
    int const c = (arm1_file_first() == 'A' && arm2_file_first() == 'B') ? 12 : 0;
    int const d = (arm4_line() == 0) ? 10 : 0;
    return (a + b + c + d) * ((argc > 0) ? 1 : 0);
}
