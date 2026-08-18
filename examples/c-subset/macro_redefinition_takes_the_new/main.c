/* [[D-PP-INCOMPATIBLE-REDEFINITION-IS-FATAL]] + the whitespace half.
 *
 * An incompatible macro redefinition is a C 6.10.3p2 CONSTRAINT VIOLATION, so a
 * diagnostic is required -- but every compiler this language declares itself to
 * be WARNS, keeps translating, and puts the NEW definition into effect. DSS used
 * to make it FATAL, which made real code unbuildable: sqlite's shell.c defines
 * S_ISLNK(mode) before it includes <sys/stat.h>, so the shipped descriptor's own
 * macro lands on top of it.
 *
 * ★ WHY THIS FILE IS A RUN WITNESS AND NOT A COMPILE-ONLY ONE. Matching the
 * reference's SEVERITY is the half a compile-only fixture can see. Matching its
 * VALUE is the half that silently miscompiles: the old code errored AND retained
 * the OLD definition, so a severity-only change would have left DSS agreeing
 * about the diagnostic and disagreeing about the program. Every arm below is
 * therefore an ARITHMETIC contribution to the exit code -- if any redefinition
 * resolved to the first definition instead of the second, the exit code moves.
 *
 * The five shapes are the complete set `sameDefinition` can report, and they are
 * exercised INDIVIDUALLY because generalizing from the one that bit a consumer
 * (parameter spelling) would be a guess wearing a measurement's clothes.
 *
 * ⚠ `volatile` on the seed: without it the optimizer folds every macro use into
 * a literal, and the release arm stops testing the preprocessor at all -- it
 * would pass on a compiler that resolved the redefinitions the wrong way.
 */

/* SHAPE 1 -- parameter spelling. The shape sqlite's shell.c actually hits. */
#define PICK(mode) 0
#define PICK(m) ((m) + 1)

/* SHAPE 2 -- replacement text. */
#define BASE 7
#define BASE 20

/* SHAPE 3 -- arity. Note the second definition takes TWO arguments: if the
 * first were retained, the call below would not even be well-formed, so this
 * arm fails LOUD rather than quietly if the fix regresses. */
#define ADD(a) (a)
#define ADD(a, b) ((a) + (b))

/* SHAPE 4 -- object-like redefined as function-like. */
#define SCALE 1
#define SCALE(a) ((a) * 2)

/* SHAPE 5 -- variadic redefined as non-variadic. */
#define FIRST(a, ...) 0
#define FIRST(a) (a)

/* SHAPE 6 -- white-space PRESENCE, the opposite direction of the same rule.
 * C 6.10.3p2 counts these two replacement lists as DIFFERENT (`3+1` has no
 * separation, `3 + 1` does), so this is a redefinition and the second wins.
 * A sibling pair differing only in the AMOUNT of white space is IDENTICAL and
 * must stay silent -- that pair is pinned in the unit tests, since "no
 * diagnostic" is not observable from a program's exit code. */
#define WS 3+1
#define WS 3 + 1

extern int printf(const char *, ...);

int main(void) {
    volatile int seed = 1;
    int total = 0;

    total += PICK(seed);          /* 2  : (1)+1   -- first def would give 0  */
    total += BASE;                /* 20 : second  -- first def would give 7  */
    total += ADD(seed, 3);        /* 4  : 1+3     -- first def is arity-1    */
    total += SCALE(seed + 4);     /* 10 : 5*2     -- first def is object-like*/
    total += FIRST(seed + 1);     /* 2  : second  -- first def would give 0  */
    total += WS;                  /* 4  : 3+1                                 */

    /* 2 + 20 + 4 + 10 + 2 + 4 == 42 */
    if (total != 42) {
        printf("macro-redefinition: expected 42, got %d\n", total);
        return 1;
    }
    printf("redefinition-takes-the-new\n");
    return total;
}
