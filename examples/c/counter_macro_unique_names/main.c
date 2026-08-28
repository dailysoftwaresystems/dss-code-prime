/* D-CSUBSET-COUNTER-MACRO-NOT-EXPANDED: `__COUNTER__` is a predefined macro whose
 * value is a per-TRANSLATION-UNIT counter advancing ONCE PER EXPANSION — the
 * gcc/clang facility every unique-name idiom is built on.
 *
 * ★★ WHAT THIS EXAMPLE IS RED AGAINST, AND WHY A ONE-USE EXAMPLE WOULD NOT BE.
 * At the pre-change HEAD `DSS_CAT(v_, __COUNTER__)` pasted the LITERAL TEXT and
 * minted a symbol named `v___COUNTER__`. A SINGLE use compiled CLEAN — it links,
 * it runs, and only someone reading a symbol table would ever notice — so an
 * example with one use would have passed against the broken behaviour. TWO uses
 * at file scope collided into a duplicate definition (S0002 + S0001), so the fact
 * that this TU compiles AT ALL is the distinctness assertion, and it is a
 * COMPILE-time one that no runtime arm could weaken.
 *
 * FOUR ARMS:
 *   1+2. `v_0` and `v_1` are named directly, which PINS the numbering decision:
 *        the count starts at 0 and steps by 1. ✔MEASURED that this is what gcc
 *        13.3.0, clang 18.1.3 and clang 19.1.1 all do. Nothing observable SHOULD
 *        depend on the specific integer — the contract is uniqueness — but the
 *        decision to agree was made deliberately and is pinned here so it cannot
 *        drift silently.
 *   3.   the count keeps advancing INSIDE a function body and within ONE
 *        expression: three reads in one declaration must yield 2, 3, 4. An
 *        implementation that advanced per SOURCE LINE, or that cached the value
 *        per translation-unit-position, scores 0 here while passing 1 and 2.
 *   4.   the two minted objects are distinct STORAGE, not merely distinct names.
 *
 * ANTI-FOLD: the score is multiplied by a runtime `argc` predicate, so the release
 * pipeline cannot reduce `main` to a single constant return.
 *
 * Data-model-independent (int arithmetic only), so the one exit code holds on all
 * four targets and on the shipped release pipeline. exit = 42 * 1 * 1 * 1 = 42.
 */

#define DSS_CAT2(a, b) a##b
#define DSS_CAT(a, b) DSS_CAT2(a, b)

/* The idiom itself, twice — the shape that could not be written before. */
static int DSS_CAT(v_, __COUNTER__) = 20;   /* counter 0 -> v_0 */
static int DSS_CAT(v_, __COUNTER__) = 22;   /* counter 1 -> v_1 */

int main(int argc, char **argv) {
    (void)argv;
    int const sum = v_0 + v_1;                       /* arms 1+2 -> 42 */
    int const a = __COUNTER__;                       /* 2 */
    int const b = __COUNTER__;                       /* 3 */
    int const c = __COUNTER__;                       /* 4 */
    int const seq_ok      = (a == 2 && b == 3 && c == 4) ? 1 : 0;
    int const distinct_ok = (&v_0 != &v_1) ? 1 : 0;
    return sum * seq_ok * distinct_ok * ((argc > 0) ? 1 : 0);
}
