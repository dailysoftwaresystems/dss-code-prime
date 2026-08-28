/* D-CSUBSET-TYPEOF-UNQUAL-GNU-SPELLING: the GNU spellings `__typeof_unqual__` and
 * `__typeof_unqual` are keyword-table ALIASES onto the C23 `typeof_unqual` token
 * kind — the qualifier-STRIPPING arm — exactly as `__typeof__`/`__typeof` are
 * aliases onto the qualifier-PRESERVING one (examples/c/gnu_alias_typeof_alignof,
 * the deliberate structural echo). No new token kind, no semantic change.
 *
 * ★ WHY THE STRIP IS PROVEN THROUGH `volatile` AND NOT THROUGH `const`. MEASURED
 * on this host at the pre-change HEAD: a `const`-sourced `typeof(g) x = 1; x = 2;`
 * compiles CLEAN in DSS under EVERY spelling, because only VolatileQual is
 * interned on a TypeId — const is carried by a separate declarator scan. So
 * assignability cannot tell the stripping arm from the preserving one here, and an
 * example built on it would have been vacuous. `_Generic` over a POINTER to the
 * typeof result can: MEASURED, `__typeof__(g) *p` selects the
 * `volatile unsigned char *` arm and `__typeof_unqual__(g) *p` selects the plain
 * `unsigned char *` arm. That is what arms 1 and 2 assert, so this example is RED
 * on a WRONG alias (a row bound to TypeofKeyword scores 0 instead of 4), not
 * merely on a missing one.
 *
 * ★ WHY ARMS 3 AND 4 COMPARE RATHER THAN SUM. They prove the resolved CORE is
 * still `unsigned char` by a RUNTIME wrap at 256. A wrapped value differs from its
 * unwrapped `int` self by EXACTLY 256, and a POSIX exit status is taken `& 255` —
 * so a summed wrap arm is invisible on the linux legs and would pass against an
 * `int` alias. Comparing the wrapped value against its expected u8 residue makes
 * the difference 14 (resp. 20) instead of 256, which no modulus can hide.
 *
 * ANTI-FOLD: `g_wrap` is a mutable `volatile` global and `argc` is a runtime
 * argument, so neither wrap is const-folded, in the release pipeline either.
 * BOTH spellings are load-bearing twice over, so deleting either keyword row makes
 * this example fail to COMPILE.
 *
 * Data-model-independent (`unsigned char` is 8 bits under LP64 and LLP64 alike),
 * so the one exit code holds on all four targets and on the shipped release
 * pipeline. exit = 4 + 4 + 14 + 20 = 42.
 */

static volatile unsigned char g_wrap = 250;

int main(int argc, char **argv) {
    (void)argv;

    /* ARM 1 + 2 — the STRIP: `volatile` must NOT survive the specifier. */
    __typeof_unqual__(g_wrap) *p1 = (unsigned char *)0;
    __typeof_unqual(g_wrap)   *p2 = (unsigned char *)0;
    int a = _Generic(p1, volatile unsigned char *: 0, unsigned char *: 4, default: 1);
    int b = _Generic(p2, volatile unsigned char *: 0, unsigned char *: 4, default: 1);

    /* ARM 3 + 4 — the CORE: still `unsigned char`, so the store wraps at 256. */
    __typeof_unqual__(g_wrap) w1 = g_wrap + argc * 20;   /* 270 -> 14 */
    __typeof_unqual(g_wrap)   w2 = g_wrap + argc * 46;   /* 296 -> 40 */
    int c = (w1 == 14) ? 14 : 0;
    int d = (w2 == 40) ? 20 : 0;

    return a + b + c + d;
}
