// D-PARSE-NINE-NESTED-CASTS-ARE-REFUSED-BY-THE-SPECULATION-CAP-WITH-A-FABRICATED-SYNTAX-ERROR
//
// A RUNNABLE witness for the parser's speculation ceilings. c's `operand` alt
// speculates once per nested cast -- `(int)` can begin `castExpr` or
// `compoundLiteralExpr`, so the LL(k) prune cannot choose -- which makes a cast
// chain the shape that walks every ceiling the parser puts on speculation.
// Before the fix there were FOUR of them stacked here, three hardcoded, and
// every one refused a legal program with `error[P_NoAlternativeMatched]:
// expected 'Identifier', ... -- got 'int'`, i.e. the compiler blaming the
// author's own correct `int`:
//
//   * the parser's speculation-depth cap, hardcoded 8   -> refused at 9
//   * the builder's checkpoint cap, hardcoded 64        -> would refuse at 65
//   * the probe token budget, hardcoded 16 x lookahead  -> refused at 342
//   * the expression-nesting cap, whose own diagnostic was ERASED by the
//     probe rollback that fired inside                  -> refused at 1024
//
// The depths below are chosen to stand ONE PAST each of the first two: 9 is the
// depth the row is named after, and 65 is the builder cap plus one. 256 is well
// past both and is a depth gcc 13.3.0, mingw-w64 gcc 13.2.0, clang 18.1.3 and
// MSVC 19.51 were each MEASURED to compile.
//
// ANTI-FOLD: every chain starts from a MUTABLE GLOBAL, so no arm of the front
// end can constant-fold the chain away and report green without parsing it.
// The `release` arm additionally proves the folded result survives
// Mem2Reg/CSE/LICM/DCE with the same exit code.
//
// exit = 3 + 4 + 5 + 30 = 42.

int g3 = 3;
int g4 = 4;
int g5 = 5;

// 9 nested casts -- the depth the row is named after (the old parser cap was 8)
static int nine(void)
{
    return (int)(int)(int)(int)(int)(int)(int)(int)(int)g3;
}

// 65 nested casts -- one past the builder's old hardcoded checkpoint cap of 64
static int sixtyFive(void)
{
    return (int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)g4;
}

// 256 nested casts -- well past both, and MEASURED-accepted by all four references
static int twoFiftySix(void)
{
    return (int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)g5;
}

int main(void)
{
    return nine() + sixtyFive() + twoFiftySix() + 30;
}
