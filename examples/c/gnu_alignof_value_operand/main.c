/* [[D-CSUBSET-ALIGNOF-VALUE-OPERAND]] — `_Alignof` / `__alignof__` applied to an
 * EXPRESSION rather than a type-name, in all three spellings and both the
 * parenthesised and paren-free forms.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. ISO C makes alignof a type-name-only operator,
 * and DSS implemented exactly that — which meant it rejected a form EVERY
 * reference compiler on this host accepts. ✔MEASURED through the shipped CLI at
 * HEAD `60198126`, immediately before this cycle: `__alignof__(d)`,
 * `__alignof__ d`, `_Alignof(d)` ALL failed with `error[P0009]` naming
 * `AlignofKeyword` among the expected tokens. ✔MEASURED on gcc 13.3.0,
 * clang 18.1.3 and clang 19.1.1 (`-std=gnu17`, matched positive+negative
 * controls): every one of them compiles and RUNS this file to 42.
 *
 * ★★ THE SHAPE THAT IS WORTH MORE THAN THE REST IS SHAPE 4, AND IT LOOKS LIKE
 * NOTHING. `__alignof__(*p)` must read the alignment of the POINTEE. The
 * lowering finds the operand's type by asking the semantic tier for the stamp on
 * the operand node — and the *older* way to write that probe, a DFS that
 * descends past an unstamped node to the first stamped LEAF, would find `p`
 * instead and answer 8. That is not a hypothetical: it is exactly
 * [[D-CSUBSET-SIZEOF-DEREF-ARRAY-SILENT-FALLBACK]], where the same descent made
 * `sizeof(*p)` size the pointer, under-allocated a pthread mutex, and turned
 * into a deterministic SIGABRT in a shipped SQLite. `*p` here is a `char *`, so
 * the two readings answer 1 and 8 — they cannot both be right and they cannot
 * both pass.
 *
 * ★★ SHAPE 6 IS THE OTHER HALF: a `char[3]`, where alignof (1) and sizeof (3)
 * DISAGREE. It exists because the value arm is a near-copy of the sizeof value
 * arm, so the failure mode that a review would miss is a paste that reads SIZE
 * where it should read ALIGNMENT. Every other shape in this file agrees under
 * both readings; this one does not.
 *
 * ★ SHAPE 7 IS A REGRESSION PIN FOR A FORM THAT ALREADY SHIPPED. Adding the
 * value arm moved `hirLowering.alignofRule` from the FORM (`alignofType`) to the
 * new WRAPPER (`alignofExpr`), and three consumers dispatch on that rule id —
 * the const-expr fold, the Pass-1.5 type probe, and the wrapper-peel. A fix that
 * added the value form without teaching the const-expr fold to descend through
 * the wrapper would leave `int a[_Alignof(double)]` failing
 * S_NonConstantArrayLength: a REGRESSION in the type form, caused by the value
 * form, and invisible to any test that only exercises the new feature.
 *
 * ⚠ NOT CLAIMED HERE: gcc reports the DECLARED alignment of an OVER-ALIGNED
 * OBJECT (`int x __attribute__((aligned(64))); __alignof__(x)` is 64 on gcc and
 * 4 under the type reading), while clang reports the TYPE alignment. DSS takes
 * the type reading — the one the C text and clang give. The divergence is
 * anchored at [[D-CSUBSET-ALIGNOF-VALUE-OVERALIGNED-OBJECT]] rather than
 * silently absorbed, and this file deliberately does NOT pin behaviour its own
 * references disagree about.
 */

/* SHAPE 7 — THE TYPE FORM, AT FILE SCOPE, IN THE TWO CONST-EXPR POSITIONS THAT
 * ROUTE THROUGH THE MOVED RULE ID. `dss_dim` is `int[8]`. */
int dss_dim[_Alignof(double)];
_Static_assert(_Alignof(double) == 8, "the type form must still fold");

/* SHAPE 8 — THE VALUE FORM IN A CONST-EXPR. The operand `*(double *)0` is
 * UNEVALUATED (C 6.5.3.4p1), so this is a well-defined constant expression and
 * not a null dereference: only the operand's TYPE reaches the operator. */
int dss_val_dim[__alignof__(*(double *)0)];
_Static_assert(__alignof__(*(double *)0) == 8,
               "the value form must fold in a constant expression");

/* Kept `volatile` so no arm of the optimizer can fold the loads away and turn a
 * behavioural check into a constant. The `release` arm of this example is where
 * that matters. */
volatile int dss_seed = 2;

int main(void) {
    double d;
    char   buf[4];
    char  *p;
    char   c3[3];
    double a4[4];
    int    seed;

    d    = 0.0;
    p    = buf;
    seed = dss_seed;
    (void)d;
    (void)p;
    (void)c3;
    (void)a4;

    /* SHAPE 1 — the GNU dunder spelling, parenthesised. */
    if ((int)__alignof__(d) != 8) return 1;

    /* SHAPE 2 — the PAREN-FREE form. It comes free with the value arm because
     * the operand rule ends in `castOperand`, the same precedence-90 expression
     * atom `sizeof e` ends in — so it is not a second feature, and this line is
     * here to prove that claim rather than to restate it. */
    if ((int)__alignof__ d != 8) return 2;

    /* SHAPE 3 — the ISO C11 spelling on a VALUE operand. The gap was never a
     * GNU-spelling gap: it hit `_Alignof` exactly as hard, and this line is the
     * witness for that half. */
    if ((int)_Alignof(d) != 8) return 3;

    /* SHAPE 4 — THE POINTEE, NOT THE POINTER. See the header note: the two
     * candidate readings answer 1 and `sizeof(char *)`. */
    if ((int)__alignof__(*p) != 1) return 4;
    if ((int)__alignof__(p) != (int)sizeof(char *)) return 5;

    /* SHAPE 5 — an INDEXED operand, whose element type is what is asked about. */
    if ((int)__alignof__(a4[0]) != 8) return 6;

    /* SHAPE 6 — alignof and sizeof DISAGREE. A paste of the sizeof arm answers
     * 3 here; the correct answer is 1. */
    if ((int)__alignof__(c3) != 1) return 7;
    if ((int)sizeof(c3) != 3) return 8;

    /* SHAPE 7/8 — the two const-expr array dimensions really are 8 elements. */
    if ((int)(sizeof dss_dim / sizeof dss_dim[0]) != 8) return 9;
    if ((int)(sizeof dss_val_dim / sizeof dss_val_dim[0]) != 8) return 10;

    /* Every mismatch above already returned its own code, so this arithmetic is
     * a second, weaker check — kept only so the exit code is a FUNCTION of the
     * measured values rather than a constant a miscompile could still produce.
     * 8 + 1 + 8 + 1 + 8 + 8 + 8 == 42, and `seed` (2) is subtracted after being
     * added, so a folded-away volatile load changes the answer. */
    return (int)__alignof__(d)
         + (int)__alignof__(*p)
         + (int)__alignof__(a4[0])
         + (int)__alignof__(c3)
         + (int)_Alignof(double)
         + (int)(sizeof dss_dim / sizeof dss_dim[0])
         + seed
         - (dss_seed - 8);
}
