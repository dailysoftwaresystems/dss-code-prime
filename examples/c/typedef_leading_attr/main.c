/* P42 lane W — two accept-direction conformance fixes that share one witness.
 *
 * (1) D-CSUBSET-LEADING-ATTRIBUTE-BEFORE-TYPEDEF-IS-A-PARSE-ERROR.
 *     A GNU or C23 attribute in the LEADING position — before the `typedef`
 *     keyword — was a PARSE error, while the MID (`typedef __attribute__((x))
 *     int T;`) and TRAILING (`typedef int T __attribute__((x));`) spellings
 *     parsed. MEASURED at 301e2a63 with the shipped CLI: file scope gave
 *     `error[P0009] expected 'Identifier', … — got 'typedef'` and block scope
 *     gave `error[P0001] expected 'EndStatement' — got 'typedef'` (two codes,
 *     because the two positions failed at different rules). gcc 13.3.0
 *     (`-std=c2x`) and clang 18.1.3 (`-std=c23`), probed SEPARATELY, accept
 *     every form below, and both HONOUR a leading `deprecated` at the alias's
 *     use — which is why the fix routes the leading run through the SAME
 *     `typedefDeclSpecifiers` prefix the post-keyword run already uses instead
 *     of parsing it and throwing it away.
 *
 *     The grammar cost is named where it is paid: FIRST(typedefDecl) gains
 *     {AttributeKeyword, BracketOpen} and now overlaps `topLevelDecl`, so
 *     `topLevel`, `statement` and `extensionTopLevel`'s inner alt are marked
 *     `speculative: true` — the resolution `detectAmbiguousAlternatives`'s own
 *     refusal text prescribes. Every OTHER lead token keeps a unique 1-token
 *     FIRST and still takes the unique-production direct descent, which is what
 *     the `plain` rows below witness.
 *
 * (2) D-CSUBSET-EXPLICIT-BOOL-CAST-OF-A-FUNCTION-DESIGNATOR.
 *     `(_Bool)fn` was `error[S0010]` while `if (fn)` on the SAME designator
 *     compiled and ran — the explicit-cast site disagreeing with the condition
 *     site about one type's truth value. C 6.3.2.1p4 decays the designator to
 *     its address in a cast exactly as it does in a condition, and the address
 *     of a function is never null, so the answer is TRUE.
 *
 * RED-ON-DISABLE, (1): drop the leading `{repeat}` from `typedefDeclSpecifiers`
 * in `c.lang.json` — the five attributed typedefs stop parsing and this example
 * does not compile.
 * RED-ON-DISABLE, (2): narrow `isExplicitCastable`'s FnSig arm back to
 * `tk == TypeKind::Ptr` — `(_Bool)fn` fires S_InvalidCast and this example does
 * not compile.
 *
 * `io()` keeps every value runtime so the optimized (release) arm exercises the
 * real materialization rather than folding the whole body to a constant. The
 * truth rows are chosen so a WRONG answer gives a DIFFERENT exit code: a
 * designator that read as FALSE drops 5, and a null function pointer that read
 * as TRUE adds 100. */

int io(int x) { return x; }

/* ── (1) the leading position, in every spelling a reference accepts ───────── */

__attribute__((unused)) typedef int LeadGnuT;             /* file scope, GNU     */
[[maybe_unused]] typedef int LeadStdT;                    /* file scope, C23     */
__attribute__((aligned(4))) typedef int LeadArgT;         /* leading WITH an arg */
__extension__ __attribute__((unused)) typedef long long LeadExtT;  /* nested prefixes */

/* The controls for the speculative dispatch: a lead token that was already
 * unambiguous must mean exactly what it meant before. */
typedef int PlainT;                                       /* bare `typedef`      */
__extension__ typedef long long PlainExtT;                /* bare `__extension__`*/
static int gStatic = 3;                                   /* bare `static`       */
struct Pair { int lo; int hi; };                          /* bare `struct`       */
;                                                         /* a stray file-scope `;` */

/* ── (2) the function designator ───────────────────────────────────────────── */

int fn(int x) { return x; }

int main(void) {
    __attribute__((unused)) typedef int LeadBlockT;       /* BLOCK scope, leading */
    typedef int PlainBlockT;                              /* the block control    */

    LeadGnuT   a = io(6);
    LeadStdT   b = io(5);
    LeadArgT   c = io(4);
    LeadExtT   d = io(3);
    LeadBlockT e = io(2);
    PlainT     f = io(7);
    PlainExtT  g = io(8);
    PlainBlockT h = io(1);

    struct Pair p;
    p.lo = io(gStatic);
    p.hi = io(0);

    /* A function DESIGNATOR converts to `_Bool` as its address: always true.
     * A NULL function pointer converts to false. Neither is foldable to a
     * constant by inspection of the cast alone, which is the point. */
    _Bool designatorIsTrue = (_Bool)fn;
    _Bool nullFnPtrIsFalse = (_Bool)(int (*)(int))0;

    int sum = (int)a + (int)b + (int)c + (int)d + (int)e
            + (int)f + (int)g + (int)h + p.lo + p.hi;     /* 6+5+4+3+2+7+8+1+3+0 = 39 */

    return sum
         + (designatorIsTrue ? 3 : 0)                     /* + 3 -> 42            */
         + (nullFnPtrIsFalse ? 100 : 0)                   /* + 0                  */
         + fn(0);
}
