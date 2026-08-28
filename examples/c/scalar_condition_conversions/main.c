/* D-CSUBSET-ENUM-FNSIG-NULLPTR-CONDITIONS-SKIP-THE-TRUTHINESS-CHOKEPOINT
 *
 * END-TO-END RUNTIME WITNESS for the C 6.8.4.1 / 6.8.5 / 6.5.15 / 6.5.13 /
 * 6.5.14 rule that a CONTROLLING EXPRESSION "shall have scalar type" and is
 * compared unequal to 0. Scalar is arithmetic (which INCLUDES the enumerated
 * types, C 6.2.5p17) union pointer (C 6.2.5p21), and a function designator
 * becomes one by C 6.3.2.1p4.
 *
 * WHAT WAS BROKEN. Two whole scalar kinds -- Enum and a function DESIGNATOR
 * (FnSig) -- fell straight through the front end's ONE truthiness chokepoint
 * (`coerceCondition`) and reached the MIR CondBr terminator still carrying
 * their source type, where the verifier's condition-must-be-Bool invariant
 * fired on LEGAL input:
 *     enum E { K = 3 }; enum E k = K; if (k) ...  -> I_TerminatorTypeMismatch
 *     if (fn) ...                                -> I_TerminatorTypeMismatch
 * while `int y = k; if (y)`, `!k` and `k != 0` all worked. clang accepts every
 * form; gcc accepts every form. The INVARIANT was right -- a front-end
 * conversion was missing -- so the fix is the conversion, not a relaxed check.
 * The third member, C23 `nullptr_t`, is fixed in the same chokepoint but is
 * NOT claimed here: `if (nullptr)` -- the LITERAL, which is what this file can
 * spell -- already worked, because the literal is an integer zero before the
 * chokepoint sees it. Only a nullptr_t-typed VALUE was broken, and such an
 * object is still refused one tier down by a separate open gap, so that arm is
 * pinned at the HIR tier instead. The two nullptr arms below stay as REGRESSION
 * coverage: `nullptr` must remain statically false in a condition.
 *
 * WHY THIS EXAMPLE CAN FAIL, i.e. what each input DISCRIMINATES:
 *
 *  (1) EVERY enumerator used as a truth value here has an EVEN nonzero value
 *      (EVEN=4, WIDE=256). A `Cast(Enum -> Bool)` lowering -- which is what a
 *      naive "just coerce it to bool" fix produces, and which MIR lowers as
 *      Trunc -- keeps only the LOW BIT, so `if (EVEN)` would be FALSE and every
 *      arm below would score 0. An ODD probe value cannot see that bug at all.
 *  (2) WIDE = 256 is nonzero only ABOVE the low byte: an enum projected to a
 *      narrower container than its underlying int reads 0 and the arm scores 0.
 *  (3) The enum values arrive through `pick()`, taking an int the caller
 *      computes -- so `release` (with Inlining + ConstFold + Mem2Reg) cannot
 *      fold the conditions away and the optimized arm still executes them.
 *  (4) `nullptr` is statically FALSE (C23 6.3.2.4p2 -- "the result is false"),
 *      so its arms score by taking the ELSE side. An arm that mis-lowered
 *      nullptr as true scores differently instead of merely not crashing.
 *
 * Each arm returns a DISTINCT weight, so any single arm going wrong moves the
 * exit code to a value no other single failure produces.
 *   1+2+3+4+5+6+7+8+9+10+11+12+13+14 = 105
 */

enum Color { NONE = 0, EVEN = 4 };     /* EVEN is nonzero with a ZERO low bit  */
enum Wide  { W_ZERO = 0, W_HIGH = 256 };  /* nonzero only above the low byte   */

struct Tag { enum Color c : 4; int pad; };   /* an enum-typed BIT-FIELD        */

/* Opaque producers: the optimizer cannot see the value at the call site, so
 * the conditions below survive ConstFold/Inlining into the `release` arm. */
static enum Color pick(int i)      { return i == 0 ? NONE : EVEN; }
static enum Wide  pick_wide(int i) { return i == 0 ? W_ZERO : W_HIGH; }
static int        helper(int v)    { return v + 1; }

/* ── the six controlling-expression SITES, on an enum ─────────────────────── */
static int arm_if(enum Color c)      { if (c) return 1; return 0; }
static int arm_while(enum Color c)   { int n = 0; while (c) { n = 2; c = NONE; } return n; }
static int arm_for(enum Color c)     { int n = 0; for (; c; c = NONE) n = 3; return n; }
static int arm_dowhile(enum Color c) { int n = 0; int g = 1;
                                       do { n = n + 4; g = 0; } while (c && g);
                                       return n; }
static int arm_ternary(enum Color c) { return c ? 5 : 0; }
static int arm_and(enum Color c)     { return (c && 1) ? 6 : 0; }
static int arm_or(enum Color c)      { return (c || 0) ? 7 : 0; }

/* ── the ASSIGNMENT form of the same conversion (C 6.3.1.2) ───────────────── */
static int arm_boolinit(enum Color c) { _Bool b = c; return b ? 8 : 0; }

/* ── the same rule on a WIDER underlying value, and on a BIT-FIELD ────────── */
static int arm_wide(enum Wide w)     { if (w) return 9; return 0; }
static int arm_bitfield(int seed)    { struct Tag t; t.pad = 0; t.c = pick(seed);
                                       if (t.c) { return 10; }
                                       return 0; }

/* ── a function DESIGNATOR as a condition (C 6.3.2.1p4): never null ───────── */
static int arm_fndesig(void)         { if (helper) return 11; return 0; }
static int arm_fndesig_ternary(void) { return helper ? 12 : 0; }

/* ── C23 nullptr_t: statically FALSE, so the ELSE side scores ─────────────── */
static int arm_nullptr(void)         { if (nullptr) return 0; return 13; }
static int arm_nullptr_and(void)     { return (nullptr && 1) ? 0 : 14; }

/* ── D-CSUBSET-LOGICAL-NOT-ON-A-FLOAT-MINTS-A-BARE-FLOAT-CONST ────────────
 * `!E` is `(0 == E)` (C 6.5.3.3p5), so a FLOAT operand is as legal there as
 * it is in `if (f)`. It was not: the `!` lowering minted the comparison zero
 * as a bare MIR float Const, which has no encoder path, and `!f` / `!d` were
 * refused for EVERY float width while `if (f)` compiled. Both arms take a
 * runtime-produced value so no constant fold can hide the lowering, and both
 * score on the NONZERO input -- i.e. on `!x` being FALSE -- so an arm that
 * lost the comparison entirely (returning a constant true) scores 0. */
static double     pick_d(int i)      { return i ? 2.5 : 0.0; }
static float      pick_f(int i)      { return i ? 2.5f : 0.0f; }
static int arm_not_double(int i)     { return !pick_d(i) ? 0 : 15; }
static int arm_not_float(int i)      { return !pick_f(i) ? 0 : 16; }

int main(void) {
    int r = 0;
    enum Color on  = pick(1);          /* EVEN  = 4   */
    enum Color off = pick(0);          /* NONE  = 0   */

    /* the TRUE side of every site: 1+2+3+4+5+6+7 = 28 */
    r = r + arm_if(on);
    r = r + arm_while(on);
    r = r + arm_for(on);
    r = r + arm_dowhile(on);
    r = r + arm_ternary(on);
    r = r + arm_and(on);
    r = r + arm_or(on);

    /* the FALSE side of the same sites must contribute NOTHING: an enum whose
     * truth value is stuck at `true` (e.g. a lowering that branched on the
     * enum's ADDRESS or on a nonzero tag word) would add 1+2+3+5+6+7 here. The
     * do-while arm runs its body once by definition, so it is excluded. */
    r = r + arm_if(off) + arm_while(off) + arm_for(off)
          + arm_ternary(off) + arm_and(off) + arm_or(off);

    r = r + arm_boolinit(on);                      /* 8  */
    r = r + arm_boolinit(off);                     /* 0  */
    r = r + arm_wide(pick_wide(1));                /* 9  */
    r = r + arm_wide(pick_wide(0));                /* 0  */
    r = r + arm_bitfield(1);                       /* 10 */
    r = r + arm_bitfield(0);                       /* 0  */
    r = r + arm_fndesig();                         /* 11 */
    r = r + arm_fndesig_ternary();                 /* 12 */
    r = r + arm_nullptr();                         /* 13 */
    r = r + arm_nullptr_and();                     /* 14 */
    r = r + arm_not_double(1);                     /* 15 -- !2.5 is false     */
    r = r + arm_not_double(0);                     /* 0  -- !0.0 is true      */
    r = r + arm_not_float(1);                      /* 16 -- !2.5f is false    */
    r = r + arm_not_float(0);                      /* 0  -- !0.0f is true     */

    return r;   /* 28 + 8 + 9 + 10 + 11 + 12 + 13 + 14 + 15 + 16 = 136 */
}
